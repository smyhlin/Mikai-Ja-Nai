#include "MpvVideoItem.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMutex>
#include <QMutexLocker>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>
#include <QScreen>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QThread>
#include <QtMath>
#include <QtQuick/qsgtexture_platform.h>

#include <atomic>
#include <optional>
#include <utility>

#include "../core/MpvHandle.hpp"
#include "../core/VideoRenderBridge.hpp"

namespace mplayer::quick {
namespace {

/// Budget for one synchronous `readVideoFrame` capture. The completion is
/// delivered on the GUI thread by the bridge's invoker, so the wait is a bounded
/// event-loop spin, never a wait on the render thread.
constexpr int kSyncReadbackBudgetMs = 2000;
/// How long `waitForContext` processes events between two context checks.
constexpr int kContextPollSliceMs = 10;

/// Dithering/quantization precision of an internal format, as the renderer
/// reports it for the stable output. 8 bits is mpv's documented default for an
/// unnamed target, so an unknown format stays at 8.
int channelBitsForInternalFormat(int internalFormat) {
    switch (internalFormat) {
    case kGlInternalFormatRgb10A2:
        return 10;
    case kGlInternalFormatRgba16:
    case kGlInternalFormatRgba16F:
        return 16;
    default:
        return 8;
    }
}

}  // namespace

/// State shared between the GUI-side item and the scene-graph node.
///
/// The node may outlive the item (the scene graph deletes it, not the item), so
/// everything it needs lives here: the last immutable snapshot, the typed
/// command slot the GUI thread fills, and the atomically published facts the GUI
/// reads back. One mutex guards the non-atomic fields; it is only ever held for
/// a struct copy, never across a GL call or a session call, so neither thread
/// blocks on the other's work.
struct MpvVideoItemSharedState {
    QMutex mutex;
    RenderSnapshot snapshot;
    quint64 sessionStamp = 0;
    quint64 contextRequestSerial = 0;
    std::shared_ptr<MpvHandle> coreLease;

    bool refresh = false;
    bool acquireRetained = false;
    bool releaseRetained = false;
    bool retainAcrossLoad = false;
    bool invalidateRetained = false;
    bool releaseContext = false;
    std::optional<uint64_t> armLanding;
    std::optional<uint64_t> landedEpoch;
    std::optional<uint64_t> refusedEpoch;
    std::function<void(QImage, QString)> readback;
    std::optional<RenderDiagnosticsRequest> diagnosticsRequest;
    bool diagnosticsInFlight = false;
    std::function<void(RenderDiagnosticsResult)> diagnosticsCompletion;
    /// Source identity a forced render was last requested for: a new source or its
    /// first video track must render once instead of serving a stale output.
    uint64_t lastForcedEpoch = 0;
    bool lastForcedVideo = false;
    /// The playhead the GUI thread last confirmed for the current source. The
    /// render thread reports this when it frees a context, so the reload that
    /// follows a context loss resumes exactly where the picture was WITHOUT any
    /// render-thread read of the session.
    double presentationPosition = 0.0;

    std::atomic<bool> visibleOutputFrame{false};
    std::atomic<bool> retainedPicture{false};
    std::atomic<qint64> retainedBytes{0};
    std::atomic<int> outputInternalFormat{0};
    std::atomic<int> outputDepthBits{0};
    std::atomic<int> colorBitsRed{0};
    std::atomic<int> colorBitsGreen{0};
    std::atomic<int> colorBitsBlue{0};
    /// The scene graph runs its GL work on the GUI thread (the basic render
    /// loop), so the item may legitimately drive the bridge itself while Qt
    /// cannot composite. False on a threaded scene graph, where the render
    /// thread owns every one of those calls.
    std::atomic<bool> renderOnGuiThread{false};
    /// A render pass has been requested (mpv has a frame, a sync happened, the
    /// session asked for a repaint). Owned by the item, not by a node: the render
    /// wrapper outlives node replacements and mpv's callback must never reference a
    /// node that may already be deleted.
    std::atomic<bool> renderRequested{false};
    /// The GL context that created the render wrapper has been destroyed. Set from
    /// the context's own aboutToBeDestroyed signal so the fact survives the node
    /// being deleted first.
    std::atomic<bool> glContextLost{false};
    /// A render-context teardown has started and no new context is ready yet.
    /// Published BEFORE mpv's render context is freed, because freeing it kills
    /// the video output and mpv then reports END_FILE(VO_INIT_FAILED); the
    /// session pumps that event on the GUI thread before the queued context-lost
    /// notification can arrive. Reading this makes the retired output
    /// attributable to the renderer instead of the source.
    std::atomic<bool> teardownInFlight{false};
    /// One GUI refresh is already queued for the renderer's wake-ups: mpv can
    /// raise frames faster than the GUI rebuilds a snapshot, and every extra post
    /// would only refresh state that the next one overwrites.
    std::atomic<bool> guiRefreshPosted{false};
};

namespace {

RenderDiagnosticsResult diagnosticFailure(const RenderDiagnosticsRequest& request,
                                          const QString& reason);

void finishDiagnostics(MpvVideoItemSharedState& state, RenderDiagnosticsResult result) {
    std::function<void(RenderDiagnosticsResult)> completion;
    {
        QMutexLocker lock(&state.mutex);
        completion = std::move(state.diagnosticsCompletion);
        state.diagnosticsRequest.reset();
        state.diagnosticsInFlight = false;
    }
    if (completion) completion(std::move(result));
}
void cancelDiagnostics(MpvVideoItemSharedState& state, const QString& reason) {
    std::function<void(RenderDiagnosticsResult)> completion;
    RenderDiagnosticsRequest request;
    {
        QMutexLocker lock(&state.mutex);
        if (!state.diagnosticsCompletion) return;
        completion = std::move(state.diagnosticsCompletion);
        if (state.diagnosticsRequest) request = *state.diagnosticsRequest;
        state.diagnosticsRequest.reset();
        state.diagnosticsInFlight = false;
    }
    completion(diagnosticFailure(request, reason));
}

RenderDiagnosticsResult diagnosticFailure(const RenderDiagnosticsRequest& request,
                                          const QString& reason) {
    RenderDiagnosticsResult result;
    result.contextGeneration = request.contextGeneration;
    result.sourceEpoch = request.sourceEpoch;
    result.position = request.position;
    result.error = reason;
    return result;
}

}  // namespace

/// Render state for the next pass, derived from the item's live geometry. The
/// cached snapshot can predate the item's size or the window's exposure (a
/// session bound while the scene is still being laid out), so the output must be
/// sized from what the item is NOW, not from that stale copy: an output that is
/// never ensured renders nothing and no frame can ever land.
RenderTargetFacts liveTargetFacts(const RenderTargetFacts& cached, const QSizeF& itemSize,
                                  qreal devicePixelRatio, bool presentable,
                                  bool renderContextReady) {
    RenderTargetFacts facts = cached;
    const qreal dpr = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
    facts.devicePixelRatio = dpr;
    const qreal scaledWidth = itemSize.width() * dpr;
    const qreal scaledHeight = itemSize.height() * dpr;
    facts.devicePixelSize = (scaledWidth > 0.0 && scaledHeight > 0.0)
                                ? QSize(qCeil(scaledWidth - 1e-6), qCeil(scaledHeight - 1e-6))
                                : QSize();
    facts.presentable = presentable;
    facts.renderContextReady = renderContextReady;
    return facts;
}

/// One drained set of presenter commands. Both the scene-graph node and the
/// item's hidden-window drain consume the same slots, so a command queued while
/// nothing renders is never lost and never applied twice.
struct MpvBridgeCommands {
    RenderSnapshot snapshot;
    quint64 sessionStamp = 0;
    double presentationPosition = 0.0;
    bool refresh = false;
    bool acquireRetained = false;
    bool releaseRetained = false;
    bool retainAcrossLoad = false;
    bool invalidateRetained = false;
    bool releaseContext = false;
    std::optional<uint64_t> armLanding;
    std::optional<uint64_t> landedEpoch;
    std::optional<uint64_t> refusedEpoch;
    std::optional<RenderDiagnosticsRequest> diagnosticsRequest;
};

/// Copies and clears the pending commands. `readback` is only taken when the
/// caller can actually deliver it: a readback needs a real render pass, so the
/// hidden drain leaves it queued for the next one.
MpvBridgeCommands takeBridgeCommands(MpvVideoItemSharedState& state,
                                     std::function<void(QImage, QString)>* readback) {
    MpvBridgeCommands commands;
    QMutexLocker lock(&state.mutex);
    commands.snapshot = state.snapshot;
    commands.sessionStamp = state.sessionStamp;
    commands.presentationPosition = state.presentationPosition;
    commands.refresh = state.refresh;
    commands.acquireRetained = state.acquireRetained;
    commands.releaseRetained = state.releaseRetained;
    commands.retainAcrossLoad = state.retainAcrossLoad;
    commands.invalidateRetained = state.invalidateRetained;
    commands.releaseContext = state.releaseContext;
    commands.armLanding = state.armLanding;
    commands.landedEpoch = state.landedEpoch;
    commands.refusedEpoch = state.refusedEpoch;
    state.refresh = false;
    state.acquireRetained = false;
    state.releaseRetained = false;
    state.retainAcrossLoad = false;
    state.invalidateRetained = false;
    state.releaseContext = false;
    state.armLanding.reset();
    state.landedEpoch.reset();
    state.refusedEpoch.reset();
    if (readback) {
        *readback = std::move(state.readback);
        state.readback = nullptr;
    }
    if (!state.diagnosticsInFlight && state.diagnosticsRequest) {
        commands.diagnosticsRequest = state.diagnosticsRequest;
        state.diagnosticsInFlight = true;
    }
    return commands;
}

/// Transition holds and load landings. They describe what the stable output must
/// show during the very pass that follows, so they run before it.
void applyBridgeTransitions(VideoRenderBridge& bridge, const MpvBridgeCommands& commands) {
    if (commands.acquireRetained) bridge.setEngineFrameHold(true);
    if (commands.releaseRetained) bridge.clearRetainedPictureKeepingTeardown();
    if (commands.retainAcrossLoad) bridge.retainAcrossLoad();
    if (commands.armLanding) bridge.armLoadLanding(*commands.armLanding);
    if (commands.landedEpoch) bridge.noteLoadLanded(*commands.landedEpoch);
    if (commands.refusedEpoch) bridge.noteLoadRefused(*commands.refusedEpoch);
    if (commands.invalidateRetained) bridge.invalidateRetainedPictures();
}

// ── scene-graph node ─────────────────────────────────────────────────────────

/// The texturing node plus the render pass.
///
/// Qt Quick calls updatePaintNode() on the rendering thread with the owning GL
/// context current (its own OpenGL items create their FBOs there), so the stable
/// output is sized and wrapped there. The mpv pass itself runs from the window's
/// beforeRendering stage instead: mpv_render_context_render() may block until
/// the frame's display time, and updatePaintNode() runs while the GUI thread is
/// blocked in the scene-graph sync.
class MpvVideoNode : public QObject, public QSGSimpleTextureNode {
    Q_OBJECT

public:
    MpvVideoNode(QQuickWindow* window, std::shared_ptr<VideoRenderBridge> bridge,
                 std::shared_ptr<MpvVideoItemSharedState> state,
                 std::weak_ptr<const void> owner, std::function<void()> wakeGui);
    ~MpvVideoNode() override;

    /// Points the node at the bridge output and rebuilds the texture wrapper only
    /// when the output was reallocated. Runs with the context current.
    void syncOutput(const QSizeF& itemSize);
    /// Creates the mpv render context (context current) and sizes the output from
    /// the item's live target facts.
    void prepareContext(const std::shared_ptr<MpvHandle>& core, const RenderTargetFacts& facts);
    /// Requests one bridge pass on the next frame. Safe from mpv's own threads.
    void scheduleRender();

public Q_SLOTS:
    void renderFrame();
    void acknowledgeFrame();

Q_SIGNALS:
    void contextReady(quint64 generation);
    /// The render context is gone (or is going away). Carries the playhead that
    /// was captured on the GUI thread before the free, so the session's reload
    /// lands where the picture was.
    void contextLost(double capturedPosition);
    void renderFailed(const QString& message);
    void presented(quint64 serial, quint64 sourceEpoch, quint64 sessionStamp,
                   bool freshOutputFrame, bool heldPicture, bool reusedPicture);
    void factsChanged();

private:
    bool releaseRenderContext();
    /// Restores the framebuffer binding that was current when the external block
    /// started. Never a guessed 0: Qt Quick may render into a redirected target.
    void restoreQtRenderTarget(int binding);
    /// Remembers the GL context that created the render wrapper and ties the
    /// bridge's teardown to that context's destruction, which is the only moment
    /// the wrapper can be freed with its creator current.
    void attachCreatingContext();
    void onCreatingContextDestroyed();
    void dropTextureWrapper();
    /// Installs the valid 1x1 "nothing loaded" texture; never a null texture.
    void installPlaceholder();
    void publishRenderFacts();
    void publishOutputFormat();

    QQuickWindow* window_ = nullptr;
    std::shared_ptr<VideoRenderBridge> bridge_;
    std::shared_ptr<MpvVideoItemSharedState> state_;
    std::weak_ptr<const void> owner_;
    /// Posts one GUI-side snapshot refresh. Lives in the node so mpv's callback
    /// never touches the item directly.
    std::function<void()> wakeGui_;

    /// The wrapper this node installed, tracked here because `QSGSimpleTextureNode`
    /// setters must not be called from a node destructor (see ~MpvVideoNode).
    QSGTexture* wrapperTexture_ = nullptr;
    unsigned int wrappedTextureId_ = 0;
    QSize wrappedTextureSize_;
    bool hasWrappedTexture_ = false;

    RenderSnapshot passSnapshot_;
    bool painted_ = false;
    RenderResult lastResult_;
    quint64 lastSessionStamp_ = 0;

    bool contextAttempted_ = false;
    quint64 attemptedRequestSerial_ = 0;
    const MpvHandle* attemptedCore_ = nullptr;
    /// The GL context the render wrapper was created against. Null until one is
    /// created, and cleared when that context is destroyed.
    QOpenGLContext* creatingContext_ = nullptr;
    QMetaObject::Connection creatingContextGone_;

    bool publishedVisible_ = false;
    bool publishedRetained_ = false;
    qint64 publishedRetainedBytes_ = 0;
    int publishedFormat_ = -1;
};

MpvVideoNode::MpvVideoNode(QQuickWindow* window, std::shared_ptr<VideoRenderBridge> bridge,
                           std::shared_ptr<MpvVideoItemSharedState> state,
                           std::weak_ptr<const void> owner, std::function<void()> wakeGui)
    : window_(window),
      bridge_(std::move(bridge)),
      state_(std::move(state)),
      owner_(std::move(owner)),
      wakeGui_(std::move(wakeGui)) {
    qsgnode_set_description(this, QStringLiteral("mpv-video"));
    setFiltering(QSGTexture::Linear);
    // mpv renders with flip_y set, so the stable output carries the image in the
    // same orientation the scene graph samples a framebuffer texture with; the
    // Widgets adapter composites the identical texture, so both views agree.
    setTextureCoordinatesTransform(QSGSimpleTextureNode::NoTransform);

    // Both stages are emitted on this node's thread (the scene-graph thread, or
    // the GUI thread under the basic loop), so these are direct calls into it.
    connect(window_, &QQuickWindow::beforeRendering, this, &MpvVideoNode::renderFrame);
    connect(window_, &QQuickWindow::afterRendering, this, &MpvVideoNode::acknowledgeFrame);
}

MpvVideoNode::~MpvVideoNode() {
    if (creatingContextGone_) disconnect(creatingContextGone_);
    // The wrapper is not owned by this node, so it is deleted here, with the scene
    // graph's context current, exactly as Qt's own scene-graph texture nodes do.
    // No setter is called: the node is going away, so there is no valid replacement
    // texture to install. (`setTexture(nullptr)` is not an alternative at any point
    // in a node's life: Qt asserts on null and then dereferences the argument, which
    // is the SIGSEGV the isolated probe hit.)
    if (wrapperTexture_ != nullptr) {
        if (QOpenGLContext::currentContext() != nullptr) delete wrapperTexture_;
        wrapperTexture_ = nullptr;
    }
    if (!bridge_ || !bridge_->contextReady()) return;
    // A node is also deleted while its owner still lives: a detach, a re-parent, a
    // node replacement. Freeing mpv's render context there retires the core's
    // video output for a reason the core cannot undo - mpv reports
    // END_FILE(VO_INIT_FAILED) and does not create a VO again for the still-loaded
    // file, so a reload restores the track but the render API keeps answering
    // "no frame" and the picture stays black after the re-attach. While the owner
    // lives the wrapper is therefore KEPT and the next node adopts it on the same
    // context.
    if (!owner_.expired()) return;
    // The owner item is gone, so nothing can adopt the wrapper: this is the real
    // teardown and the cleanup contract applies. The scene graph deletes nodes with
    // their context current; a release without it is refused by the bridge, which
    // then retains for the context's own aboutToBeDestroyed.
    const bool wasReady = true;
    double captured = 0.0;
    if (state_) {
        QMutexLocker lock(&state_->mutex);
        captured = state_->presentationPosition;
    }
    const bool freed = releaseRenderContext();
    if (!freed) {
        qCritical("MpvVideoItem: the mpv render context and its core lease were retained "
                  "because no creating OpenGL context was current during scene-graph teardown");
    } else if (wasReady) {
        emit contextLost(captured);
    }
}

void MpvVideoNode::attachCreatingContext() {
    QOpenGLContext* current = QOpenGLContext::currentContext();
    if (current == nullptr || current == creatingContext_) return;
    if (creatingContextGone_) disconnect(creatingContextGone_);
    creatingContext_ = current;
    // Direct: the bridge captures its teardown picture and frees mpv's render
    // context while this context is still current, and keeps the core lease so a
    // recreated context can attach again. Without this the wrapper outlives the
    // context that owns it and every later frame is rendered against dead objects
    // while the session still believes the renderer is ready.
    creatingContextGone_ = connect(
        current, &QOpenGLContext::aboutToBeDestroyed, this,
        [this] { onCreatingContextDestroyed(); }, Qt::DirectConnection);
    // Anchor the wrapper's teardown to the context itself as well, so the fact is
    // recorded even when this node is deleted before the context goes. The lambda
    // holds a WEAK bridge on purpose: a strong one would keep the bridge - and
    // with it the core lease - alive until a shared/global GL context dies, long
    // after the window, and the core could never expire. Both handlers may run;
    // contextAboutToBeDestroyed() is idempotent.
    if (bridge_ && state_) {
        const std::weak_ptr<VideoRenderBridge> weakBridge = bridge_;
        const std::shared_ptr<MpvVideoItemSharedState> state = state_;
        connect(
            current, &QOpenGLContext::aboutToBeDestroyed, current,
            [weakBridge, state] {
                state->glContextLost.store(true);
                if (const std::shared_ptr<VideoRenderBridge> bridge = weakBridge.lock())
                    bridge->contextAboutToBeDestroyed();
            },
            Qt::DirectConnection);
    }
}

void MpvVideoNode::onCreatingContextDestroyed() {
    if (creatingContextGone_) {
        disconnect(creatingContextGone_);
        creatingContextGone_ = QMetaObject::Connection();
    }
    creatingContext_ = nullptr;
    if (state_) state_->glContextLost.store(true);
    if (!bridge_) return;
    const bool wasReady = bridge_->contextReady();
    double captured = 0.0;
    if (state_) {
        QMutexLocker lock(&state_->mutex);
        captured = state_->presentationPosition;
    }
    // The context is still current here: this is the bridge's documented
    // context-loss path. It frees the wrapper, keeps the core lease, and marks the
    // bridge not ready so the next attach creates a context against the new one.
    if (state_) state_->teardownInFlight.store(true);
    bridge_->contextAboutToBeDestroyed();
    // No placeholder can be created while the context is dying, and installing a
    // null texture is illegal. The wrapper stays installed and is freed by the node
    // destructor, which the same scene-graph invalidation runs with a context
    // current; only the bookkeeping that says "this is not a live output" changes.
    wrappedTextureId_ = 0;
    wrappedTextureSize_ = QSize();
    hasWrappedTexture_ = false;
    if (state_) {
        // The attachment format died with that context.
        state_->outputInternalFormat.store(0);
        state_->colorBitsRed.store(0);
        state_->colorBitsGreen.store(0);
        state_->colorBitsBlue.store(0);
        state_->outputDepthBits.store(0);
    }
    publishedFormat_ = -1;
    painted_ = false;
    if (state_) state_->renderRequested.store(false);
    // A recreated context must be requested again: the failure latches above only
    // ever suppress repeats for one context.
    contextAttempted_ = false;
    attemptedRequestSerial_ = 0;
    attemptedCore_ = nullptr;
    if (wasReady) emit contextLost(captured);
}

void MpvVideoNode::dropTextureWrapper() {
    // QSGSimpleTextureNode requires a valid texture at every point of its life:
    // its setTexture() asserts on null and then dereferences the argument
    // unconditionally, so `setTexture(nullptr)` is illegal in a live node as well
    // as in a destructor. The wrapper is therefore replaced by the valid 1x1
    // placeholder (the same "nothing loaded" visual the node starts with) and only
    // then freed. Without a context there is neither a replacement to create nor a
    // GL object to free: the wrapper stays installed and the node destructor frees
    // it during scene-graph teardown, where the context is current.
    if (wrapperTexture_ == nullptr) return;
    if (QOpenGLContext::currentContext() == nullptr) return;
    installPlaceholder();
}

void MpvVideoNode::installPlaceholder() {
    if (window_ == nullptr || QOpenGLContext::currentContext() == nullptr) return;
    QImage black(1, 1, QImage::Format_RGBA8888);
    black.fill(Qt::black);
    QSGTexture* placeholder = window_->createTextureFromImage(black);
    if (placeholder == nullptr) return;
    placeholder->setFiltering(QSGTexture::Nearest);
    QSGTexture* previous = wrapperTexture_;
    setTexture(placeholder);
    wrapperTexture_ = placeholder;
    wrappedTextureId_ = 0;
    wrappedTextureSize_ = QSize();
    hasWrappedTexture_ = false;
    if (previous != nullptr && previous != placeholder) delete previous;
    markDirty(QSGNode::DirtyMaterial);
}

void MpvVideoNode::restoreQtRenderTarget(int binding) {
    // The bridge leaves no FBO of its own bound and restores the read/draw bindings
    // it found internally. This is the boundary restore Qt documents (its own OpenGL
    // item does the same before endExternalCommands): put back the binding that was
    // current when the block started, never a guessed 0, because Qt Quick may render
    // into a redirected target.
    //
    // Only the combined GL_FRAMEBUFFER form is used on purpose: GL_FRAMEBUFFER and
    // GL_FRAMEBUFFER_BINDING are valid on every GL/ES version, while the separate
    // read/draw enums are 3.0+/ES3-only and are INVALID_ENUM on the pre-3.0 context
    // a bare probe can obtain. The bridge owns the read binding.
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (context == nullptr) return;
    context->functions()->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(binding));
}

void MpvVideoNode::scheduleRender() {
    // Coalesced. Qt only permits scheduling an update from the GUI thread or from
    // updatePaintNode(), so nothing here calls QQuickWindow::update(): a pass
    // requested from mpv's own threads arrives through the GUI wake-up, and a pass
    // requested during a sync runs in the frame that sync belongs to. The flag
    // lives in the state shared with the item, not in this node, because the wrapper
    // (and therefore mpv's callback) can outlive the node: a node that has been
    // replaced must never be referenced from renderer callbacks.
    if (state_) state_->renderRequested.store(true);
}

bool MpvVideoNode::releaseRenderContext() {
    if (!bridge_) return true;
    if (!bridge_->contextReady() && !bridge_->releasePending()) return true;
    // Marked before the bridge frees mpv's render context: that free is what
    // makes mpv retire the video output, and the session must be able to
    // attribute the resulting END_FILE to the renderer.
    if (state_) state_->teardownInFlight.store(true);
    return bridge_->releaseRenderContext();
}

void MpvVideoNode::prepareContext(const std::shared_ptr<MpvHandle>& core,
                                  const RenderTargetFacts& facts) {
    if (!bridge_) return;
    quint64 requestSerial = 0;
    {
        QMutexLocker lock(&state_->mutex);
        requestSerial = state_->contextRequestSerial;
    }
    if (state_ && state_->glContextLost.load() && bridge_->contextReady()) {
        // The wrapper's context was destroyed but the bridge was not told in time
        // to free it. Retry now; only a matching creating context can succeed.
        bridge_->contextAboutToBeDestroyed();
        if (bridge_->contextReady()) {
            emit renderFailed(QStringLiteral(
                "the mpv render context still belongs to a destroyed OpenGL context and cannot "
                "be freed from this one"));
            return;
        }
    }
    if (bridge_->releasePending()) {
        // A previous teardown found no creating context current and retained the
        // wrapper with its core lease. Retry now, where a context is current; if
        // its original context is the one now current this frees it and the fresh
        // attach below can proceed.
        bridge_->contextAboutToBeDestroyed();
        if (bridge_->releasePending()) {
            emit renderFailed(QStringLiteral(
                "the mpv render context of a previous OpenGL context could not be freed; the "
                "video surface waits for that context's own destruction"));
            return;
        }
        contextAttempted_ = false;
    }
    if (!bridge_->contextReady()) {
        if (!core) return;
        // One attempt per explicit request (or a replaced core), so a refused
        // context is not rebuilt on every frame; a new scene graph starts with a
        // fresh node.
        if (contextAttempted_ && requestSerial == attemptedRequestSerial_ &&
            core.get() == attemptedCore_)
            return;
        contextAttempted_ = true;
        attemptedRequestSerial_ = requestSerial;
        attemptedCore_ = core.get();
        QString reason;
        try {
            // The callback captures the shared state and the GUI wake-up only -
            // never this node. The render wrapper can outlive the node that created
            // it (a detach keeps it for the next node to adopt), and mpv raises this
            // callback on its own threads.
            const std::shared_ptr<MpvVideoItemSharedState> state = state_;
            std::function<void()> wake = wakeGui_;
            if (bridge_->createRenderContext(
                    core, owner_,
                    [state, wake] {
                        if (state) state->renderRequested.store(true);
                        if (wake) wake();
                    },
                    &reason)) {
                // The attachment format belonged to the previous context: a sized
                // format the new one does not accept is an INVALID_ENUM waiting to
                // happen, so the cache is cleared and the output falls back to the
                // documented 8-bit default until the new format is published.
                state_->outputInternalFormat.store(0);
                state_->colorBitsRed.store(0);
                state_->colorBitsGreen.store(0);
                state_->colorBitsBlue.store(0);
                state_->outputDepthBits.store(0);
                publishedFormat_ = -1;
                emit contextReady(bridge_->contextGeneration());
            } else {
                emit renderFailed(reason.isEmpty()
                                      ? QStringLiteral("the mpv render context could not be created")
                                      : reason);
            }
        } catch (const MpvError& error) {
            emit renderFailed(QString::fromUtf8(error.what()));
        }
        if (!bridge_->contextReady()) return;
    }
    if (state_) state_->glContextLost.store(false);
    if (bridge_->contextReady()) {
        // Ready either because it was just created here or because a previous node
        // left it attached to this context: either way this node now owns the
        // teardown for that context.
        attachCreatingContext();
        state_->teardownInFlight.store(false);
    }
    if (facts.devicePixelSize.isValid() && !facts.devicePixelSize.isEmpty())
        bridge_->ensureOutput(facts);
    publishOutputFormat();
}

void MpvVideoNode::syncOutput(const QSizeF& itemSize) {
    setRect(QRectF(0, 0, itemSize.width(), itemSize.height()));
    const unsigned int id = bridge_ && bridge_->contextReady() ? bridge_->outputTextureId() : 0;
    const QSize size = bridge_ ? bridge_->outputSize() : QSize();
    if (id != 0 && !size.isEmpty() &&
        !(hasWrappedTexture_ && id == wrappedTextureId_ && size == wrappedTextureSize_)) {
        QSGTexture* previous = wrapperTexture_;
        QSGTexture* wrapper = QNativeInterface::QSGOpenGLTexture::fromNative(
            static_cast<GLuint>(id), window_, size, QQuickWindow::TextureIsOpaque);
        if (wrapper) {
            wrapper->setFiltering(QSGTexture::Linear);
            wrapper->setMipmapFiltering(QSGTexture::None);
            setTexture(wrapper);
            wrapperTexture_ = wrapper;
            delete previous;
            wrappedTextureId_ = id;
            wrappedTextureSize_ = size;
            hasWrappedTexture_ = true;
            markDirty(QSGNode::DirtyMaterial);
        }
    }
    if (wrapperTexture_ == nullptr) {
        // No output exists yet (before the first pass, or after a release). A node
        // with a null texture still reaches the renderer's material and warns "No
        // QSGTexture provided from updateSampledImage()", and setTexture(nullptr)
        // is not a legal way to get there: the 1x1 stand-in is the "nothing
        // loaded" visual and is replaced by the output texture when one exists.
        installPlaceholder();
    }
}

void MpvVideoNode::renderFrame() {
    const bool requested = state_ && state_->renderRequested.exchange(false);
    if (!requested) return;
    if (!bridge_ || !window_ || !bridge_->contextReady()) return;

    std::function<void(QImage, QString)> readback;
    MpvBridgeCommands commands = takeBridgeCommands(*state_, &readback);
    passSnapshot_ = commands.snapshot;
    lastSessionStamp_ = commands.sessionStamp;

    // Everything below is external GL: the bridge owns its own FBO bindings and
    // GL state, so Qt's cached state is reset first and its command stream is
    // bracketed. The framebuffer bindings found at entry are recorded so they can
    // be restored exactly, whatever Qt Quick renders into.
    window_->beginExternalCommands();
    int entryBinding = 0;
    if (QOpenGLContext* entryContext = QOpenGLContext::currentContext()) {
        // GL_FRAMEBUFFER_BINDING is 0x8CA6 on every GL/ES version; the separate
        // read/draw enums are 3.0+/ES3-only and would themselves be INVALID_ENUM on
        // a pre-3.0 context.
        entryContext->functions()->glGetIntegerv(0x8CA6, &entryBinding);
    }
    QQuickOpenGLUtils::resetOpenGLState();

    if (commands.releaseContext) {
        const bool wasReady = bridge_->contextReady();
        if (commands.diagnosticsRequest) {
            const auto state = state_;
            bridge_->postDiagnosticsResult(
                diagnosticFailure(*commands.diagnosticsRequest,
                                  QStringLiteral("diagnostic capture canceled by render-context teardown")),
                [state](RenderDiagnosticsResult result) {
                    if (state) finishDiagnostics(*state, std::move(result));
                });
        }
        if (!releaseRenderContext()) {
            emit renderFailed(QStringLiteral(
                "the mpv render context could not be released with its OpenGL context current"));
        } else if (wasReady) {
            emit contextLost(commands.presentationPosition);
        }
        // The wrapper described an FBO that no longer exists.
        dropTextureWrapper();
        painted_ = false;
        // A later request (a new scene graph or an explicit re-ensure) may attach
        // the recreated context.
        contextAttempted_ = false;
        attemptedRequestSerial_ = 0;
        attemptedCore_ = nullptr;
        restoreQtRenderTarget(entryBinding);
        window_->endExternalCommands();
        return;
    }

    applyBridgeTransitions(*bridge_, commands);

    if (readback) bridge_->readVideoFrameAsync(std::move(readback));
    // A landing load and a chain change both mean the next pass must RENDER, not
    // reuse the output: mpv initializes a video output only inside
    // mpv_render_context_render(), so a reused pass after a reload leaves the new
    // source without a VO and the stable output showing the previous frame.
    if (commands.refresh || commands.landedEpoch)
        bridge_->refresh(commands.snapshot, commands.snapshot.target.presentable);

    // The bridge refuses a pass whose target size is empty (`ensureOutput` returns
    // false and nothing renders, so mpv never initializes its video output). The
    // node already knows the item's size - it set `rect()` during the sync - so an
    // empty size in the snapshot is filled in from it rather than dropping the
    // pass. Without this, a pass that arrives between an attach and the next GUI
    // snapshot leaves the core with no VO and only a later pass could recover.
    RenderSnapshot passSnapshot = commands.snapshot;
    if (passSnapshot.target.devicePixelSize.isEmpty() && !rect().isEmpty()) {
        const qreal dpr = passSnapshot.target.devicePixelRatio > 0.0
                              ? passSnapshot.target.devicePixelRatio
                              : 1.0;
        passSnapshot.target.devicePixelRatio = dpr;
        passSnapshot.target.devicePixelSize =
            QSize(qCeil(rect().width() * dpr - 1e-6), qCeil(rect().height() * dpr - 1e-6));
    }
    if (passSnapshot.target.devicePixelSize.isValid() &&
        !passSnapshot.target.devicePixelSize.isEmpty()) {
        bridge_->ensureOutput(passSnapshot.target);
    }
    // The pass always paints into the stable output. The output is offscreen, so
    // painting costs nothing a hidden or non-composited window can see, and it is
    // what produces the frame report the session's load gate waits for: skipping
    // here would leave a loaded source unable to ever report its first frame.
    lastResult_ = bridge_->renderToOutput(passSnapshot);
    painted_ = true;
    if (commands.diagnosticsRequest) {
        const auto state = state_;
        bridge_->captureDiagnosticsAsync(
            *commands.diagnosticsRequest, passSnapshot,
            [state](RenderDiagnosticsResult result) {
                if (state) finishDiagnostics(*state, std::move(result));
            });
    }
    publishRenderFacts();
    publishOutputFormat();

    restoreQtRenderTarget(entryBinding);
    window_->endExternalCommands();
}

void MpvVideoNode::acknowledgeFrame() {
    if (!painted_) return;
    painted_ = false;
    if (!bridge_ || !bridge_->contextReady()) return;
    // A composited pass owns a swap report; a pass nothing could composite is
    // acknowledged as the drop it is, so mpv keeps supplying frames either way.
    if (passSnapshot_.target.presentable)
        bridge_->reportPresentedFrame();
    else
        bridge_->acknowledgeDroppedFrame();
    if (lastResult_.presentationSerial == 0) return;
    emit presented(lastResult_.presentationSerial, passSnapshot_.sourceEpoch, lastSessionStamp_,
                   lastResult_.freshOutputFrame, lastResult_.presentedHeldPicture,
                   lastResult_.reusedOutput);
}

void MpvVideoNode::publishRenderFacts() {
    if (!state_ || !bridge_) return;
    const bool visible = bridge_->hasVisibleOutputFrame();
    const bool retained = bridge_->hasRetainedPicture();
    const qint64 bytes = bridge_->retainedPictureBytes();
    bool changed = false;
    if (visible != publishedVisible_) {
        publishedVisible_ = visible;
        state_->visibleOutputFrame.store(visible);
        changed = true;
    }
    if (retained != publishedRetained_) {
        publishedRetained_ = retained;
        state_->retainedPicture.store(retained);
        changed = true;
    }
    if (bytes != publishedRetainedBytes_) {
        publishedRetainedBytes_ = bytes;
        state_->retainedBytes.store(bytes);
        changed = true;
    }
    if (changed) emit factsChanged();
}

void MpvVideoNode::publishOutputFormat() {
    if (!bridge_ || !state_) return;
    const int format = bridge_->outputInternalFormat();
    if (format == publishedFormat_) return;
    publishedFormat_ = format;
    const int bits = channelBitsForInternalFormat(format);
    state_->outputInternalFormat.store(format);
    state_->colorBitsRed.store(bits);
    state_->colorBitsGreen.store(bits);
    state_->colorBitsBlue.store(bits);
    state_->outputDepthBits.store(bridge_->targetDepthBitsPerChannel());
    emit factsChanged();
}

// ── item ─────────────────────────────────────────────────────────────────────

MpvVideoItem::MpvVideoItem(QQuickItem* parent)
    : QQuickItem(parent),
      state_(std::make_shared<MpvVideoItemSharedState>()),
      lifetimeToken_(std::make_shared<int>(0)) {
    setFlag(QQuickItem::ItemHasContents, true);
    // While the window cannot composite, the item itself answers mpv's render
    // callbacks whenever the scene graph's GL work is on this thread.
    hiddenDrainTimer_.setInterval(16);
    hiddenDrainTimer_.setSingleShot(false);
    connect(&hiddenDrainTimer_, &QTimer::timeout, this, &MpvVideoItem::drainHiddenFrames);
    bridge_ = std::make_shared<VideoRenderBridge>();
    // Called from the rendering thread; posting to the item as the receiver makes
    // the completion auto-removed when the item is gone.
    const QPointer<MpvVideoItem> guard(this);
    bridge_->setGuiInvoker([guard](std::function<void()> task) {
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), std::move(task), Qt::QueuedConnection);
    });
}

MpvVideoItem::~MpvVideoItem() {
    hiddenDrainTimer_.stop();
    detachSession();
    // The scene-graph node owns the bridge for as long as it exists and frees the
    // render context from the scene graph's own teardown, with the creating
    // context current. Releasing GL from this destructor would run without it.
    state_.reset();
    bridge_.reset();
}

VideoSession* MpvVideoItem::session() const {
    return session_.data();
}

void MpvVideoItem::setSession(VideoSession* session) {
    if (session_.data() == session) return;
    if (session && session->presenter() && session->presenter() != this) {
        qWarning("MpvVideoItem: refusing a session that another presenter already hosts");
        return;
    }
    const bool wasAttached = session_ != nullptr;
    detachSession();
    session_ = session;
    ++sessionStamp_;
    {
        QMutexLocker lock(&state_->mutex);
        state_->sessionStamp = sessionStamp_;
    }
    if (session_) {
        attachSessionSignals();
        session_->setPresenter(this);
    }
    publishedFactsValid_ = false;
    refreshSnapshot();
    publishFactsIfChanged();
    emit sessionChanged();
    // An explicit detach (setSession(nullptr)) retires the renderer with a real
    // render job: the command is applied by the next pass with the creating context
    // current, so the wrapper and its core lease do not survive the session even
    // while the GL context persists. A session swap re-attaches a context instead.
    if (wasAttached && session_ == nullptr && bridge_ && bridge_->contextReady())
        releaseRenderContext();
    update();
}

void MpvVideoItem::attachSessionSignals() {
    if (!session_) return;
    // State that changes what a pass must do without an mpv frame arriving.
    sessionConnections_.append(connect(session_, &VideoSession::sourceEpochChanged, this,
                                       [this] { requestRepaint(); }));
    sessionConnections_.append(connect(session_, &VideoSession::videoSizeChanged, this,
                                       [this] { requestRepaint(); }));
    sessionConnections_.append(connect(session_, &VideoSession::openingChanged, this,
                                       [this] { requestRepaint(); }));
    sessionConnections_.append(connect(session_, &VideoSession::playingChanged, this,
                                       [this] { requestRepaint(); }));
    // A seek under a paused core produces its picture only if the renderer asks
    // mpv for it: the paused-quiet path in the bridge would otherwise skip
    // update()/render() and the seek's frame would never be drawn. The session
    // announces every seek, so the presenter forces exactly one real pass here.
    sessionConnections_.append(connect(session_, &VideoSession::seekRequested, this, [this] {
        queueCommand([](MpvVideoItemSharedState& state) { state.refresh = true; });
    }));
    // A session that dies while the item is still mounted can no longer supply a
    // core lease. Dropping the cached lease here keeps the core's lifetime tied to
    // real renderer owners: this item, and the bridge while its render context
    // still exists.
    sessionConnections_.append(connect(session_, &QObject::destroyed, this, [this] {
        // The sender is the session that is being destroyed: detach unconditionally
        // so nothing here keeps observing or leasing from it.
        session_ = nullptr;
        detachSessionSignals();
        if (state_) {
            cancelDiagnostics(*state_, QStringLiteral("diagnostic capture canceled because the session was destroyed"));
            QMutexLocker lock(&state_->mutex);
            state_->coreLease.reset();
        }
        publishFactsIfChanged();
        update();
    }));
}

void MpvVideoItem::detachSessionSignals() {
    for (const QMetaObject::Connection& connection : sessionConnections_)
        disconnect(connection);
    sessionConnections_.clear();
}

void MpvVideoItem::detachSession() {
    detachSessionSignals();
    if (session_ && session_->presenter() == this) session_->setPresenter(nullptr);
    session_ = nullptr;
    if (state_) {
        cancelDiagnostics(*state_, QStringLiteral("diagnostic capture canceled by session detach"));
        QMutexLocker lock(&state_->mutex);
        state_->coreLease.reset();
    }
}

void MpvVideoItem::refreshSnapshot() {
    if (!state_) return;
    RenderSnapshot snapshot;
    std::shared_ptr<MpvHandle> core;
    double position = 0.0;
    if (session_) {
        snapshot = session_->renderSnapshot();
        core = session_->coreLease();
        position = session_->position();
    }
    QMutexLocker lock(&state_->mutex);
    if (session_) {
        state_->snapshot = snapshot;
        // Confirmed on the GUI thread where the session lives. The rendering
        // thread reports this copy when it frees a context instead of reading the
        // session itself.
        state_->presentationPosition = position;
        if (snapshot.sourceEpoch != state_->lastForcedEpoch ||
            snapshot.videoAvailable != state_->lastForcedVideo) {
            state_->lastForcedEpoch = snapshot.sourceEpoch;
            state_->lastForcedVideo = snapshot.videoAvailable;
            // A new source - or its first video track - needs one REAL render pass:
            // mpv creates its video output only inside mpv_render_context_render(),
            // and a reuse pass would never do that. This is what makes a reload
            // resume, paused or not.
            state_->refresh = true;
        }
    }
    if (core) state_->coreLease = std::move(core);
    state_->sessionStamp = sessionStamp_;
}

void MpvVideoItem::queueCommand(std::function<void(MpvVideoItemSharedState&)> apply) {
    if (!state_) return;
    {
        QMutexLocker lock(&state_->mutex);
        if (apply) apply(*state_);
    }
    requestRepaint();
}

RenderTargetFacts MpvVideoItem::targetFacts() const {
    RenderTargetFacts facts;
    QQuickWindow* window = this->window();
    const qreal dpr = window && window->effectiveDevicePixelRatio() > 0.0
                          ? window->effectiveDevicePixelRatio()
                          : 1.0;
    facts.devicePixelRatio = dpr;
    const qreal scaledWidth = width() * dpr;
    const qreal scaledHeight = height() * dpr;
    if (scaledWidth > 0.0 && scaledHeight > 0.0) {
        facts.devicePixelSize = QSize(qCeil(scaledWidth - 1e-6), qCeil(scaledHeight - 1e-6));
    }
    if (state_) {
        facts.colorInternalFormat = state_->outputInternalFormat.load();
        facts.colorBits.red = state_->colorBitsRed.load();
        facts.colorBits.green = state_->colorBitsGreen.load();
        facts.colorBits.blue = state_->colorBitsBlue.load();
        facts.depthBitsPerChannel = state_->outputDepthBits.load();
    }
    // A context being torn down is not usable: the flag is published before the
    // free so the session can classify the END_FILE that free provokes.
    facts.renderContextReady =
        bridge_ && bridge_->contextReady() && !(state_ && state_->teardownInFlight.load());
    facts.presentable = canPresentNow();
    return facts;
}

bool MpvVideoItem::canPresentNow() const {
    QQuickWindow* window = this->window();
    if (!window || !window->isVisible() || !window->isExposed()) return false;
    if (!isVisible() || width() <= 0.0 || height() <= 0.0) return false;
    if (qFuzzyIsNull(opacity())) return false;
    return true;
}

void MpvVideoItem::requestRepaint() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this] { requestRepaint(); }, Qt::QueuedConnection);
        return;
    }
    refreshSnapshot();
    update();
}

void MpvVideoItem::acquireRetainedPicture() {
    queueCommand([](MpvVideoItemSharedState& state) { state.acquireRetained = true; });
}

void MpvVideoItem::releaseRetainedPicture() {
    queueCommand([](MpvVideoItemSharedState& state) { state.releaseRetained = true; });
}

void MpvVideoItem::retainAcrossLoad() {
    queueCommand([](MpvVideoItemSharedState& state) { state.retainAcrossLoad = true; });
}

void MpvVideoItem::armLoadLanding(uint64_t sourceEpoch) {
    queueCommand([sourceEpoch](MpvVideoItemSharedState& state) { state.armLanding = sourceEpoch; });
}

void MpvVideoItem::noteLoadLanded(uint64_t sourceEpoch) {
    queueCommand([sourceEpoch](MpvVideoItemSharedState& state) { state.landedEpoch = sourceEpoch; });
}

void MpvVideoItem::noteLoadRefused(uint64_t sourceEpoch) {
    queueCommand([sourceEpoch](MpvVideoItemSharedState& state) { state.refusedEpoch = sourceEpoch; });
}

void MpvVideoItem::invalidateRetainedPictures() {
    queueCommand([](MpvVideoItemSharedState& state) { state.invalidateRetained = true; });
}

bool MpvVideoItem::hasVisibleOutputFrame() const {
    return state_ && state_->visibleOutputFrame.load();
}

bool MpvVideoItem::hasRetainedPicture() const {
    return state_ && state_->retainedPicture.load();
}

qint64 MpvVideoItem::retainedPictureBytes() const {
    return state_ ? state_->retainedBytes.load() : 0;
}

void MpvVideoItem::refreshRenderState() {
    queueCommand([](MpvVideoItemSharedState& state) { state.refresh = true; });
}

QImage MpvVideoItem::grabVideoFrame(QString* reason) const {
    return const_cast<MpvVideoItem*>(this)->readVideoFrame(reason);
}
void MpvVideoItem::requestRenderDiagnostics(
    RenderDiagnosticsRequest request,
    std::function<void(RenderDiagnosticsResult)> completion) {
    if (!completion) return;
    if (QThread::currentThread() != thread()) {
        const QPointer<MpvVideoItem> guard(this);
        QMetaObject::invokeMethod(
            this,
            [guard, request, completion = std::move(completion)]() mutable {
                if (guard)
                    guard->requestRenderDiagnostics(request, std::move(completion));
            },
            Qt::QueuedConnection);
        return;
    }
    const auto fail = [&completion, &request](const QString& reason) {
        completion(diagnosticFailure(request, reason));
    };
    if (!state_ || !bridge_ || !session_) {
        fail(QStringLiteral("video item is detached or destroyed"));
        return;
    }
    QQuickWindow* window = this->window();
    QSGRendererInterface* renderer = window ? window->rendererInterface() : nullptr;
    if (!window || !renderer || renderer->graphicsApi() != QSGRendererInterface::OpenGL) {
        fail(QStringLiteral("diagnostic capture requires an attached OpenGL Quick window"));
        return;
    }
    if (!canPresentNow() && !state_->renderOnGuiThread.load()) {
        fail(QStringLiteral("diagnostic capture requires a visible render target"));
        return;
    }
    const std::shared_ptr<MpvHandle> core = session_->coreLease();
    if (!core || !bridge_->contextReady()) {
        fail(QStringLiteral("diagnostic capture has no live mpv render context"));
        return;
    }
    request.coreIdentity = reinterpret_cast<quintptr>(core.get());
    if (request.contextGeneration != bridge_->contextGeneration()) {
        fail(QStringLiteral("diagnostic request context generation is stale"));
        return;
    }
    bool busy = false;
    {
        QMutexLocker lock(&state_->mutex);
        if (state_->diagnosticsCompletion) {
            busy = true;
        } else {
            state_->diagnosticsRequest = request;
            const QPointer<MpvVideoItem> guard(this);
            state_->diagnosticsCompletion =
                [guard, request, completion = std::move(completion)](
                    RenderDiagnosticsResult result) mutable {
                    if (!guard) return;
                    bool stillCurrent = false;
                    if (guard->state_) {
                        QMutexLocker stateLock(&guard->state_->mutex);
                        const auto& snapshot = guard->state_->snapshot;
                        stillCurrent = snapshot.sourceEpoch == request.sourceEpoch &&
                                       snapshot.positionValid &&
                                       std::abs(snapshot.position - request.position) <= 0.01 &&
                                       reinterpret_cast<quintptr>(
                                           guard->state_->coreLease.get()) == request.coreIdentity;
                    }
                    stillCurrent = stillCurrent && guard->bridge_ &&
                                   guard->bridge_->contextGeneration() == request.contextGeneration;
                    if (!stillCurrent) {
                        result.success = false;
                        result.error =
                            QStringLiteral("diagnostic result canceled because the source or context changed");
                    }
                    completion(std::move(result));
                };
        }
    }
    if (busy) {
        fail(QStringLiteral("a render diagnostic request is already in flight"));
        return;
    }
    requestRepaint();
}

QImage MpvVideoItem::readVideoFrame(QString* reason) {
    QString localReason;
    if (!reason) reason = &localReason;
    reason->clear();
    if (QThread::currentThread() != thread()) {
        *reason = QStringLiteral("readVideoFrame must be called on the GUI thread");
        return {};
    }
    if (!bridge_) {
        *reason = QStringLiteral("the video surface is not initialized");
        return {};
    }
    if (!bridge_->contextReady()) {
        *reason = QStringLiteral("the video surface has no OpenGL render context");
        return {};
    }

    // The completion writes into a shared record, never into stack references: a
    // readback still queued inside the bridge after this returns then writes into
    // a record that stays alive, so a late delivery cannot touch a dead frame.
    struct Result {
        bool done = false;
        QImage image;
        QString failure;
    };
    const auto result = std::make_shared<Result>();
    readVideoFrameAsync([result](QImage frame, QString why) {
        result->image = std::move(frame);
        result->failure = std::move(why);
        result->done = true;
    });
    if (!result->done) {
        // Bounded spin: the render thread never waits for the GUI thread, so the
        // completion can always arrive; a window that renders nothing times out
        // with a reason instead of hanging.
        QElapsedTimer timer;
        timer.start();
        while (!result->done && timer.elapsed() < kSyncReadbackBudgetMs)
            QCoreApplication::processEvents(QEventLoop::AllEvents, kContextPollSliceMs);
        if (!result->done) {
            *reason = QStringLiteral("timed out waiting for the video frame readback");
            return {};
        }
    }
    if (!result->failure.isEmpty()) {
        *reason = result->failure;
        return {};
    }
    if (result->image.isNull()) {
        *reason = QStringLiteral("the video surface has no frame to read back");
        return {};
    }
    return result->image;
}

void MpvVideoItem::readVideoFrameAsync(std::function<void(QImage, QString)> completion) {
    if (!completion) return;
    if (!state_) {
        completion({}, QStringLiteral("the video surface is not initialized"));
        return;
    }
    if (!bridge_ || !bridge_->contextReady()) {
        completion({}, QStringLiteral("the video surface has no OpenGL render context"));
        return;
    }
    bool busy = false;
    {
        QMutexLocker lock(&state_->mutex);
        if (state_->readback)
            busy = true;
        else
            state_->readback = std::move(completion);
    }
    if (busy) {
        completion({}, QStringLiteral("a video frame readback is already in flight"));
        return;
    }
    requestRepaint();
}

bool MpvVideoItem::ensureRenderContext(std::shared_ptr<MpvHandle> core, QString* reason) {
    QString localReason;
    if (!reason) reason = &localReason;
    reason->clear();
    if (!core) {
        *reason = QStringLiteral("the session has no mpv core");
        return false;
    }
    if (!bridge_ || !state_) {
        *reason = QStringLiteral("the video surface is not initialized");
        return false;
    }
    if (bridge_->contextReady()) return true;
    QQuickWindow* window = this->window();
    if (!window) {
        *reason = QStringLiteral("the video surface is not in a window");
        return false;
    }
    QSGRendererInterface* renderer = window->rendererInterface();
    if (!renderer || renderer->graphicsApi() != QSGRendererInterface::OpenGL) {
        *reason = QStringLiteral("the scene graph is not rendering through OpenGL");
        return false;
    }
    {
        QMutexLocker lock(&state_->mutex);
        state_->coreLease = std::move(core);
        ++state_->contextRequestSerial;
    }
    renderErrorReported_ = false;
    // Accepted, not created: the context can only be built with the owning GL
    // context current, on the rendering thread. The session learns the outcome
    // through handleRenderContextReady()/handleRenderError().
    update();
    return true;
}

void MpvVideoItem::releaseRenderContext() {
    if (!bridge_) return;
    // Queue-time marker: the actual free can only happen on the rendering thread,
    // and mpv's retired output must be attributable from this moment on.
    if (state_) state_->teardownInFlight.store(true);
    // Cache the playhead here, on the GUI thread that owns the session, BEFORE
    // the free is queued. The node reports this value when it actually frees the
    // context, so the session's reload resumes where the picture was without any
    // render-thread read of the session.
    refreshSnapshot();
    if (!bridge_->contextReady() && !bridge_->releasePending()) {
        if (state_) {
            state_->teardownInFlight.store(false);
            QMutexLocker lock(&state_->mutex);
            state_->coreLease.reset();
        }
        return;
    }
    queueCommand([](MpvVideoItemSharedState& state) { state.releaseContext = true; });
}

bool MpvVideoItem::waitForContext(int timeoutMs) {
    if (!bridge_) return false;
    if (bridge_->contextReady()) return true;
    if (QThread::currentThread() != thread()) return false;
    QElapsedTimer timer;
    timer.start();
    const int budget = qMax(timeoutMs, 0);
    while (!bridge_->contextReady() && timer.elapsed() < budget)
        QCoreApplication::processEvents(QEventLoop::AllEvents, kContextPollSliceMs);
    return bridge_->contextReady();
}

quint64 MpvVideoItem::renderContextGeneration() const {
    return bridge_ ? bridge_->contextGeneration() : 0;
}

UiProfileCounters MpvVideoItem::uiProfileCounters() const {
    return bridge_ ? bridge_->uiProfileCounters() : UiProfileCounters();
}

// ── scene graph ─────────────────────────────────────────────────────────────

QSGNode* MpvVideoItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    QQuickWindow* window = this->window();
    auto* node = static_cast<MpvVideoNode*>(oldNode);
    if (!bridge_ || !state_ || !session_ || !window) {
        // Returning null deletes the node, and its destructor frees the render
        // context with the old context current.
        return nullptr;
    }
    QSGRendererInterface* renderer = window->rendererInterface();
    if (!renderer || renderer->graphicsApi() != QSGRendererInterface::OpenGL) {
        if (!renderErrorReported_) {
            renderErrorReported_ = true;
            // This runs on the rendering thread: the session is only reachable
            // through its own thread.
            QMetaObject::invokeMethod(
                this,
                [this] {
                    if (session_ && renderError)
                        renderError(QStringLiteral("the scene graph is not rendering through "
                                                   "OpenGL; the video surface cannot present"));
                },
                Qt::QueuedConnection);
        }
        return nullptr;
    }

    if (!node) {
        // mpv's frame callback runs on its own threads: it may only schedule, and
        // the snapshot the pass reads must be rebuilt on this item's thread.
        const QPointer<MpvVideoItem> owner(this);
        const std::shared_ptr<MpvVideoItemSharedState> state = state_;
        node = new MpvVideoNode(window, bridge_, state_, lifetimeToken_,
                                [owner, state] {
                                    if (!owner || state->guiRefreshPosted.exchange(true)) return;
                                    QMetaObject::invokeMethod(
                                        owner.data(),
                                        [owner, state] {
                                            state->guiRefreshPosted.store(false);
                                            if (owner) owner->requestRepaint();
                                        },
                                        Qt::QueuedConnection);
                                });
        // Queued: the node lives on the rendering thread, the item on the GUI
        // thread. Qt removes undelivered reports when the item is destroyed.
        connect(node, &MpvVideoNode::contextReady, this,
                [this](quint64 generation) {
                    if (session_ && renderContextReady) renderContextReady(generation);
                },
                Qt::QueuedConnection);
        connect(node, &MpvVideoNode::contextLost, this,
                [this](double capturedPosition) { onNodeContextLost(capturedPosition); },
                Qt::QueuedConnection);
        connect(node, &MpvVideoNode::renderFailed, this,
                [this](const QString& message) { onNodeRenderError(message); },
                Qt::QueuedConnection);
        connect(node, &MpvVideoNode::presented, this,
                [this](quint64 serial, quint64 epoch, quint64 stamp, bool fresh, bool held,
                       bool reused) { onNodePresented(serial, epoch, stamp, fresh, held, reused); },
                Qt::QueuedConnection);
        connect(node, &MpvVideoNode::factsChanged, this, [this] { onNodeFactsChanged(); },
                Qt::QueuedConnection);
    }

    RenderSnapshot snapshot;
    std::shared_ptr<MpvHandle> core;
    {
        QMutexLocker lock(&state_->mutex);
        snapshot = state_->snapshot;
        core = state_->coreLease;
    }
    if (width() > 0.0 && height() > 0.0) {
        // The output is sized from the item's live geometry: the cached snapshot
        // may predate the layout and the window's exposure, and an output that is
        // never ensured renders nothing.
        // Presentability comes from the GUI-built snapshot: this phase runs on the
        // scene-graph thread under a threaded loop and must not read item state
        // beyond what Qt itself reads here (its own OpenGL item reads geometry and
        // the window's effective DPR in this function).
        const RenderTargetFacts facts = liveTargetFacts(
            snapshot.target, QSizeF(width(), height()), window->effectiveDevicePixelRatio(),
            snapshot.target.presentable, bridge_->contextReady());
        node->prepareContext(core, facts);
        node->syncOutput(QSizeF(width(), height()));
        // The pass that follows this sync reads the snapshot: publish the target
        // sized above so the first pass after a bind, a resize or an exposure is
        // never handed an empty target and never silently renders nothing.
        {
            QMutexLocker lock(&state_->mutex);
            state_->snapshot.target = facts;
        }
    }
    node->scheduleRender();
    // A basic render loop runs this on the GUI thread; only then may the item
    // drive the bridge itself when Qt stops composing.
    const bool onGuiThread = QThread::currentThread() == thread();
    state_->renderOnGuiThread.store(onGuiThread);
    if (onGuiThread) updateHiddenDrain();
    return node;
}

void MpvVideoItem::updateHiddenDrain() {
    if (!bridge_ || !state_) {
        hiddenDrainTimer_.stop();
        return;
    }
    const bool drainable = state_->renderOnGuiThread.load() && window() &&
                           bridge_->contextReady() && !canPresentNow();
    if (!drainable) {
        if (hiddenDrainTimer_.isActive()) hiddenDrainTimer_.stop();
        return;
    }
    if (!hiddenDrainTimer_.isActive()) hiddenDrainTimer_.start();
}

void MpvVideoItem::deliverPresentation(const RenderResult& result, uint64_t sourceEpoch,
                                       quint64 sessionStamp) {
    if (!session_ || sessionStamp != sessionStamp_ || result.presentationSerial == 0) return;
    PresentationReport report;
    report.serial = result.presentationSerial;
    report.sourceEpoch = sourceEpoch;
    report.freshOutputFrame = result.freshOutputFrame;
    report.heldPicture = result.presentedHeldPicture;
    report.reusedSnapshot = result.reusedOutput;
    if (framePresented) framePresented(report);
    if (result.freshOutputFrame) emit videoFramePresented();
}

void MpvVideoItem::drainHiddenFrames() {
    if (!bridge_ || !state_) {
        hiddenDrainTimer_.stop();
        return;
    }
    // On a threaded scene graph the render thread owns the context; the item
    // must not make it current behind Qt's back.
    if (!state_->renderOnGuiThread.load()) {
        hiddenDrainTimer_.stop();
        return;
    }
    QQuickWindow* window = this->window();
    if (!window || !bridge_->contextReady() || canPresentNow()) {
        hiddenDrainTimer_.stop();
        return;
    }
    QSGRendererInterface* renderer = window->rendererInterface();
    QOpenGLContext* context = nullptr;
    if (renderer && renderer->graphicsApi() == QSGRendererInterface::OpenGL) {
        // The only documented accessor in Qt 6, and this is the thread that owns
        // the context under the basic render loop: no Qt 5 fallback exists here.
        context = static_cast<QOpenGLContext*>(
            renderer->getResource(window, QSGRendererInterface::OpenGLContextResource));
    }
    if (!context) {
        hiddenDrainTimer_.stop();
        return;
    }
    if (!hiddenDrainSurface_) hiddenDrainSurface_ = std::make_unique<QOffscreenSurface>();
    if (!hiddenDrainSurface_->isValid() || hiddenDrainSurface_->format() != context->format()) {
        hiddenDrainSurface_->destroy();
        hiddenDrainSurface_->setScreen(window->screen());
        hiddenDrainSurface_->setFormat(context->format());
        hiddenDrainSurface_->create();
    }
    if (!hiddenDrainSurface_->isValid()) {
        qWarning("MpvVideoItem: no compatible offscreen surface for the hidden-window render "
                 "state drain; mpv frame supply stays paused until the window is shown");
        hiddenDrainTimer_.stop();
        return;
    }
    const bool alreadyCurrent = QOpenGLContext::currentContext() == context;
    if (!alreadyCurrent && !context->makeCurrent(hiddenDrainSurface_.get())) {
        hiddenDrainTimer_.stop();
        return;
    }

    // A readback needs a real pass to be delivered, so it is left queued.
    MpvBridgeCommands commands = takeBridgeCommands(*state_, nullptr);
    if (commands.releaseContext) {
        const bool wasReady = bridge_->contextReady();
        if (commands.diagnosticsRequest) {
            const auto state = state_;
            bridge_->postDiagnosticsResult(
                diagnosticFailure(*commands.diagnosticsRequest,
                                  QStringLiteral("diagnostic capture canceled by render-context teardown")),
                [state](RenderDiagnosticsResult result) {
                    if (state) finishDiagnostics(*state, std::move(result));
                });
        }
        if (!bridge_->releaseRenderContext()) {
            qCritical("MpvVideoItem: the mpv render context could not be released while hidden");
        } else if (wasReady) {
            onNodeContextLost(commands.presentationPosition);
        }
    } else {
        applyBridgeTransitions(*bridge_, commands);
        if (commands.refresh) bridge_->refresh(commands.snapshot, false);
        // The output is offscreen, so the pass still renders and still reports:
        // a hidden view must be able to finish its load, and mpv's callback gets
        // its answer. Nothing composites it, so it is acknowledged as a drop.
        // Same target fallback as the composited path: an empty size would make the
        // bridge drop the pass and mpv would never initialize its video output.
        RenderSnapshot hiddenSnapshot = commands.snapshot;
        if (hiddenSnapshot.target.devicePixelSize.isEmpty() && width() > 0.0 && height() > 0.0) {
            const qreal dpr = hiddenSnapshot.target.devicePixelRatio > 0.0
                                  ? hiddenSnapshot.target.devicePixelRatio
                                  : 1.0;
            hiddenSnapshot.target.devicePixelRatio = dpr;
            hiddenSnapshot.target.devicePixelSize =
                QSize(qCeil(width() * dpr - 1e-6), qCeil(height() * dpr - 1e-6));
        }
        if (hiddenSnapshot.target.devicePixelSize.isValid() &&
            !hiddenSnapshot.target.devicePixelSize.isEmpty()) {
            bridge_->ensureOutput(hiddenSnapshot.target);
        }
        const RenderResult result = bridge_->renderToOutput(hiddenSnapshot);
        bridge_->acknowledgeDroppedFrame();
        if (commands.diagnosticsRequest) {
            const auto state = state_;
            bridge_->captureDiagnosticsAsync(
                *commands.diagnosticsRequest, hiddenSnapshot,
                [state](RenderDiagnosticsResult diagnostic) {
                    if (state) finishDiagnostics(*state, std::move(diagnostic));
                });
        }
        deliverPresentation(result, commands.snapshot.sourceEpoch, commands.sessionStamp);
    }

    if (!alreadyCurrent) context->doneCurrent();
    updateHiddenDrain();
}

void MpvVideoItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        // The snapshot carries the target size the pass renders against, so a
        // resize must rebuild it, not only dirty the node.
        publishFactsIfChanged();
        requestRepaint();
    }
}

void MpvVideoItem::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    switch (change) {
    case ItemSceneChange: {
        // Exposure and scene-graph lifetime belong to the window: presentability
        // and a pending context request change with them, and a recreated scene
        // graph must rebuild the node.
        for (const QMetaObject::Connection& connection : std::as_const(windowConnections_))
            disconnect(connection);
        windowConnections_.clear();
        if (value.window) {
            windowConnections_.append(connect(value.window, &QWindow::visibleChanged, this, [this] {
                if (!canPresentNow() && state_)
                    cancelDiagnostics(*state_, QStringLiteral("diagnostic capture canceled because the window became hidden"));
                publishFactsIfChanged();
                requestRepaint();
            }));
            windowConnections_.append(
                connect(value.window, &QQuickWindow::sceneGraphInitialized, this, [this] {
                    publishFactsIfChanged();
                    requestRepaint();
                }));
        }
        publishFactsIfChanged();
        requestRepaint();
        break;
    }
    case ItemVisibleHasChanged:
    case ItemOpacityHasChanged:
        if (!canPresentNow() && state_)
            cancelDiagnostics(*state_, QStringLiteral("diagnostic capture canceled because the item became hidden"));
        publishFactsIfChanged();
        requestRepaint();
        break;
    default:
        break;
    }
}

void MpvVideoItem::releaseResources() {
    // The scene graph is being invalidated. The node - and with it the render
    // context - is destroyed by that teardown with the old context current; a
    // new node re-attaches the bridge to the recreated context. Nothing can be
    // freed here: this may run on either thread and the item owns no GL.
    if (state_) {
        cancelDiagnostics(*state_, QStringLiteral("diagnostic capture canceled by scene-graph teardown"));
        state_->visibleOutputFrame.store(false);
        state_->retainedPicture.store(false);
        state_->retainedBytes.store(0);
    }
    publishFactsIfChanged();
}

void MpvVideoItem::publishFactsIfChanged() {
    if (!state_) return;
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this] { publishFactsIfChanged(); }, Qt::QueuedConnection);
        return;
    }
    const RenderTargetFacts facts = targetFacts();
    const bool changed = !publishedFactsValid_ || factsChanged(facts);
    publishedFacts_ = facts;
    publishedFactsValid_ = true;
    if (changed && targetStateChanged) targetStateChanged(facts);
    // A changed target is a changed pass input: the next paint must see it.
    if (changed) requestRepaint();
    updateHiddenDrain();
}

bool MpvVideoItem::factsChanged(const RenderTargetFacts& facts) const {
    return facts.devicePixelSize != publishedFacts_.devicePixelSize ||
           !qFuzzyCompare(facts.devicePixelRatio + 1.0, publishedFacts_.devicePixelRatio + 1.0) ||
           facts.colorInternalFormat != publishedFacts_.colorInternalFormat ||
           facts.colorBits.red != publishedFacts_.colorBits.red ||
           facts.colorBits.green != publishedFacts_.colorBits.green ||
           facts.colorBits.blue != publishedFacts_.colorBits.blue ||
           facts.depthBitsPerChannel != publishedFacts_.depthBitsPerChannel ||
           facts.renderContextReady != publishedFacts_.renderContextReady ||
           facts.presentable != publishedFacts_.presentable;
}

void MpvVideoItem::onNodePresented(quint64 serial, uint64_t sourceEpoch, quint64 sessionStamp,
                                   bool freshOutputFrame, bool heldPicture, bool reusedPicture) {
    RenderResult result;
    result.presentationSerial = serial;
    result.freshOutputFrame = freshOutputFrame;
    result.presentedHeldPicture = heldPicture;
    result.reusedOutput = reusedPicture;
    deliverPresentation(result, sourceEpoch, sessionStamp);
}

void MpvVideoItem::onNodeContextReady(quint64 generation) {
    // A recreated context has no video output yet, and mpv only creates one inside
    // a render call: force the next pass to render instead of reusing.
    queueCommand([](MpvVideoItemSharedState& state) { state.refresh = true; });
    // A ready context is also when the session submits its deferred load, so the
    // snapshot the first pass reads is rebuilt here rather than at bind time.
    requestRepaint();
    updateHiddenDrain();
    if (!session_ || !renderContextReady) return;
    renderContextReady(generation);
}

void MpvVideoItem::onNodeContextLost(double capturedPosition) {
    updateHiddenDrain();
    if (state_)
        cancelDiagnostics(*state_, QStringLiteral("diagnostic capture canceled by render-context loss"));
    if (!session_) return;
    // The position this session's reload must resume at is reported first, so the
    // loss handler already sees it.
    if (reloadPositionCaptured) reloadPositionCaptured(capturedPosition);
    if (renderContextLost) renderContextLost();
}

void MpvVideoItem::onNodeRenderError(const QString& message) {
    if (!session_ || !renderError) {
        qWarning().noquote() << "MpvVideoItem:" << message;
        return;
    }
    renderError(message);
}

void MpvVideoItem::onNodeFactsChanged() {
    publishFactsIfChanged();
}

}  // namespace mplayer::quick

#include "MpvVideoItem.moc"
