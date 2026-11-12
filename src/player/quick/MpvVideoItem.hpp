// The native Qt Quick video surface.
//
// One QQuickItem that presents a VideoSession's mpv output into the Qt Quick
// scene graph. The item is the session's VideoPresenter: it owns the shared
// VideoRenderBridge (the same implementation the Widgets adapter uses) and the
// scene-graph texture node that wraps the bridge's ONE stable output texture.
//
// Threading model
//  · The session lives on the GUI thread. It reaches this item only through the
//    VideoPresenter contract, which is documented as GUI-thread calls; every one
//    of them only records a typed command and schedules a frame. Nothing here
//    blocks waiting for the render thread, and the scene-graph thread never
//    touches a VideoSession, a QQuickItem or a QML object.
//  · The render thread owns the bridge's GL state, the output FBO/texture and
//    the command drain. It reads an immutable RenderSnapshot that the GUI thread
//    copied from the session, and it publishes facts (context generation,
//    format, visible frame, retained bytes, counters) atomically for the GUI.
//  · Both the basic (single-threaded) and the threaded render loop are correct:
//    in the basic loop the "render thread" is the GUI thread, so the command
//    queue and the snapshot copy are simply consumed in the same thread.
//
// Lifecycle
//  · The scene-graph node owns the bridge (shared_ptr) for as long as it exists.
//    The item's destructor never frees GL: it detaches the session first, and
//    the node's destructor - the scene graph's own deferred cleanup, which runs
//    with the owning GL context current - frees mpv's render context before
//    dropping the core lease. A failed release is reported, never silently
//    leaked into the item's destructor.
//  · mpv's render-update callback only schedules a repaint (coalesced by the
//    bridge, relayed through the node's lifetime token); presentation reports
//    and context notices travel back as queued signals from the node, which Qt
//    disconnects automatically when the item is destroyed, so no callback ever
//    dereferences a dead item or session.

#pragma once

#include <QImage>
#include <QList>
#include <QPointer>
#include <QQuickItem>
#include <QSize>
#include <QString>
#include <QTimer>

#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <functional>
#include <memory>

#include "../core/VideoPresenter.hpp"
#include "../core/VideoSession.hpp"
#include "../core/RenderDiagnostics.hpp"

QT_BEGIN_NAMESPACE
class QSGNode;
struct UpdatePaintNodeData;
QT_END_NAMESPACE

QT_BEGIN_NAMESPACE
class QOffscreenSurface;
class QOpenGLContext;
QT_END_NAMESPACE

namespace mplayer {

class MpvHandle;
class VideoRenderBridge;

namespace quick {

struct MpvVideoItemSharedState;

/// Gives the session a QML type identity without adding a frontend dependency
/// to the core header: the controller creates the session and QML only ever
/// receives the pointer as a property value.
struct VideoSessionForeign {
    Q_GADGET
    QML_FOREIGN(mplayer::VideoSession)
    QML_NAMED_ELEMENT(VideoSession)
    QML_UNCREATABLE("VideoSession is owned by the player controller")
};

/// Renders one VideoSession into the Qt Quick scene through the shared OpenGL
/// render bridge. The session is injected (QML binds `session: controller
/// session`); the item never creates one.
class MpvVideoItem : public QQuickItem, public VideoPresenter {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(mplayer::VideoSession *session READ session WRITE setSession NOTIFY sessionChanged)

public:
    explicit MpvVideoItem(QQuickItem* parent = nullptr);
    ~MpvVideoItem() override;

    VideoSession* session() const;
    /// Attaches this item as the session's presenter, or detaches it with
    /// nullptr. A session that already has a different presenter is refused:
    /// one session drives exactly one video surface.
    void setSession(VideoSession* session);

    /// Overlay-free capture of the presented video: the render bridge's own
    /// output, never the QML chrome above it. Empty image with `reason` set when
    /// no frame can be read.
    QImage grabVideoFrame(QString* reason = nullptr) const;
    /// Captures the stable output and optional live GL textures asynchronously.
    /// Completion runs on this item's GUI thread; no GL object escapes the renderer.
    void requestRenderDiagnostics(
        RenderDiagnosticsRequest request,
        std::function<void(RenderDiagnosticsResult)> completion);

    QSGNode* updatePaintNode(QSGNode* node, UpdatePaintNodeData* data) override;

    // ── VideoPresenter ────────────────────────────────────────────────────
    RenderTargetFacts targetFacts() const override;
    bool canPresentNow() const override;
    void requestRepaint() override;

    void acquireRetainedPicture() override;
    void releaseRetainedPicture() override;
    void retainAcrossLoad() override;
    void armLoadLanding(uint64_t sourceEpoch) override;
    void noteLoadLanded(uint64_t sourceEpoch) override;
    void noteLoadRefused(uint64_t sourceEpoch) override;
    void invalidateRetainedPictures() override;
    bool hasVisibleOutputFrame() const override;
    bool hasRetainedPicture() const override;
    qint64 retainedPictureBytes() const override;
    void refreshRenderState() override;

    QImage readVideoFrame(QString* reason) override;
    void readVideoFrameAsync(std::function<void(QImage, QString)> completion) override;

    bool ensureRenderContext(std::shared_ptr<MpvHandle> core, QString* reason) override;
    void releaseRenderContext() override;
    bool waitForContext(int timeoutMs) override;
    quint64 renderContextGeneration() const override;
    UiProfileCounters uiProfileCounters() const override;

signals:
    void sessionChanged();
    /// A frame of the loaded source was composited by the scene graph. The
    /// window's loading gate, PiP and Compare wait on this rather than a timer.
    void videoFramePresented();

protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void releaseResources() override;

private:
    /// GUI-side bookkeeping that must survive a scene-graph node being replaced:
    /// session identity, published target facts and the snapshot cache.
    void attachSessionSignals();
    void detachSessionSignals();
    void detachSession();

    /// Copies the session's immutable render state into the shared slot the
    /// render thread reads. GUI thread only; a session that has no renderer yet
    /// leaves the previous snapshot intact.
    void refreshSnapshot();
    void publishFactsIfChanged();
    bool factsChanged(const RenderTargetFacts& facts) const;

    /// Records a GL-thread command and schedules a frame. `apply` runs on the
    /// rendering thread during the next bridge pass.
    void queueCommand(std::function<void(MpvVideoItemSharedState&)> apply);

    /// Pushes one pass's presentation to the session's hooks. Runs on the GUI
    /// thread only, and drops anything belonging to a replaced session or source.
    void deliverPresentation(const RenderResult& result, uint64_t sourceEpoch,
                             quint64 sessionStamp);
    void onNodePresented(quint64 serial, uint64_t sourceEpoch, quint64 sessionStamp,
                         bool freshOutputFrame, bool heldPicture, bool reusedPicture);
    void onNodeContextReady(quint64 generation);
    void onNodeContextLost(double capturedPosition);
    void onNodeRenderError(const QString& message);
    void onNodeFactsChanged();

    /// When Qt cannot composite (window hidden, minimized or obscured) but this
    /// item's OpenGL work runs on the GUI thread - the basic render loop - the
    /// bridge still owes mpv one answer per update, so the item keeps advancing
    /// it with its own context current instead of leaving frame supply stalled
    /// until the window is shown again.
    void updateHiddenDrain();
    void drainHiddenFrames();

    std::shared_ptr<MpvVideoItemSharedState> state_;
    QPointer<VideoSession> session_;
    std::shared_ptr<VideoRenderBridge> bridge_;
    /// Guards the bridge's update callback: the token lives in the item, so it
    /// expires exactly when the item does.
    std::shared_ptr<const void> lifetimeToken_;

    QList<QMetaObject::Connection> sessionConnections_;
    QList<QMetaObject::Connection> windowConnections_;
    quint64 sessionStamp_ = 0;
    RenderTargetFacts publishedFacts_;
    bool publishedFactsValid_ = false;
    bool renderErrorReported_ = false;

    QTimer hiddenDrainTimer_;
    std::unique_ptr<QOffscreenSurface> hiddenDrainSurface_;
};

}  // namespace quick
}  // namespace mplayer
