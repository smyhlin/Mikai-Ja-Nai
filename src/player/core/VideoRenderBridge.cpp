#include "VideoRenderBridge.hpp"

#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>
#include <vector>

#include "gui/UiProfile.hpp"

#include "MpvApi.hpp"
#include "MpvHandle.hpp"

namespace mplayer {

namespace {

// A lost reply must never strand a frame: two unpaintable rounds are a transient
// occlusion, more is a surface nothing composites.
constexpr int kUnpaintableGraceTicks = 2;
// About one 60 Hz frame, so a grace that became paintable again is retried
// before mpv's own video-output timeout runs out.
constexpr int kUnpaintableRetryMs = 16;
constexpr double kMinRenderLeadMs = 0.25;
constexpr double kMaxRenderLeadMs = 8.0;

// Integer GL tokens the GL 1.1 headers do not define. They are values, not an
// extension check: every one below is only used on an attachment this renderer
// created itself, at the format the driver already accepted for it.
constexpr GLenum kGlRgba = 0x1908;
constexpr GLenum kGlUnsignedByte = 0x1401;
constexpr GLenum kGlUnsignedShort = 0x1403;
constexpr GLenum kGlHalfFloat = 0x140B;  // GL 3.0
constexpr GLenum kGlPackAlignment = 0x0D05;
// glBindFramebuffer targets.
constexpr GLenum kGlReadFramebuffer = 0x8CA8;
constexpr GLenum kGlDrawFramebuffer = 0x8CA9;
// glGetIntegerv binding queries: GL_DRAW_FRAMEBUFFER_BINDING and
// GL_FRAMEBUFFER_BINDING are both 0x8CA6; the read binding is 0x8CAA.
constexpr GLenum kGlReadFramebufferBinding = 0x8CAA;
constexpr GLenum kGlDrawFramebufferBinding = 0x8CA6;

/// The caller's framebuffer bindings, restored at the end of every bridge pass.
struct FboBindings {
    GLint read = 0;
    GLint draw = 0;
};

FboBindings captureFboBindings(QOpenGLFunctions* gl) {
    FboBindings bindings;
    gl->glGetIntegerv(kGlReadFramebufferBinding, &bindings.read);
    gl->glGetIntegerv(kGlDrawFramebufferBinding, &bindings.draw);
    return bindings;
}

void bindFboRead(QOpenGLFunctions* gl, GLuint fbo) {
    gl->glBindFramebuffer(kGlReadFramebuffer, fbo);
}

void bindFboDraw(QOpenGLFunctions* gl, GLuint fbo) {
    gl->glBindFramebuffer(kGlDrawFramebuffer, fbo);
}

void restoreFboBindings(QOpenGLFunctions* gl, const FboBindings& bindings) {
    gl->glBindFramebuffer(kGlReadFramebuffer, static_cast<GLuint>(bindings.read));
    gl->glBindFramebuffer(kGlDrawFramebuffer, static_cast<GLuint>(bindings.draw));
}

// Framebuffer queries used by the target probe. Component sizes are part of the
// attachment query, not of the GL 1.1 header.
constexpr GLenum kGlColorAttachmentObjectType = 0x8CD0;
constexpr GLenum kGlColorAttachmentObjectName = 0x8CD1;
constexpr GLenum kGlTextureObject = 0x1702;
constexpr GLenum kGlRenderbufferObject = 0x8D41;
constexpr GLenum kGlTextureInternalFormat = 0x1003;
constexpr GLenum kGlRenderbufferInternalFormat = 0x8D44;
constexpr GLenum kGlRedSize = 0x8212;
constexpr GLenum kGlGreenSize = 0x8213;
constexpr GLenum kGlBlueSize = 0x8214;

qint64 steadyClockNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool supportedSizedFormat(int format) {
    return format == kGlInternalFormatRgba8 || format == kGlInternalFormatRgb10A2 ||
           format == kGlInternalFormatRgba16 || format == kGlInternalFormatRgba16F;
}

int bytesPerPixel(int internalFormat) {
    return internalFormat == kGlInternalFormatRgba16F || internalFormat == kGlInternalFormatRgba16
               ? 8
               : 4;
}

bool clientFramePacingEnabled() {
    // Read once per process: the kill-switch exists for a rollback and for field
    // diagnostics, both of which relaunch. Only an explicit "0" turns it off.
    static const bool disabled =
        qEnvironmentVariable("MIKAI_CLIENT_FRAME_PACING") == QStringLiteral("0");
    return !disabled;
}

/// Reads a framebuffer this renderer owns at exactly the precision its
/// attachment was allocated with. Rows are flipped in place (GL is bottom-first,
/// QImage top-first) so a half-float 4K buffer is never copied twice.
QImage readFramebufferPixels(QOpenGLExtraFunctions* gl, GLuint source, const QSize& size,
                             int internalFormat) {
    if (gl == nullptr || source == 0 || size.isEmpty()) return QImage();
    GLenum readType = kGlUnsignedByte;
    QImage::Format imageFormat = QImage::Format_RGBA8888;
    switch (internalFormat) {
        case kGlInternalFormatRgba16F:
            readType = kGlHalfFloat;
            imageFormat = QImage::Format_RGBA16FPx4;
            break;
        case kGlInternalFormatRgba16:
            readType = kGlUnsignedShort;
            imageFormat = QImage::Format_RGBA64;
            break;
        case kGlInternalFormatRgb10A2:
            // Widened to 16 bits rather than copied packed: the channel order of
            // a 10:10:10:2 word is an image-layout question, and widening is exact.
            readType = kGlUnsignedShort;
            imageFormat = QImage::Format_RGBA64;
            break;
        default:
            break;
    }
    QImage picture(size, imageFormat);
    if (picture.isNull()) return QImage();
    const FboBindings bindings = captureFboBindings(gl);
    bindFboRead(gl, source);
    GLint previousPack = 4;
    gl->glGetIntegerv(kGlPackAlignment, &previousPack);
    gl->glPixelStorei(kGlPackAlignment, 1);
    gl->glReadPixels(0, 0, size.width(), size.height(), kGlRgba, readType, picture.bits());
    gl->glPixelStorei(kGlPackAlignment, static_cast<GLint>(previousPack));
    restoreFboBindings(gl, bindings);
    picture.flip(Qt::Vertical);
    return picture;
}

// ── process-wide frame-timing log ─────────────────────────────────────────
// One buffered writer, protected by one mutex: the render thread and the GUI
// writer share the same file without interleaving partial lines.

struct TimingLogStorage {
    QMutex mutex;
    QFile file;
    QByteArray buffer;
    QElapsedTimer flushClock;
    QString path;
    bool broken = false;
};

TimingLogStorage& timingLogStorage() {
    static TimingLogStorage storage;
    return storage;
}

void flushTimingLogLocked(TimingLogStorage& storage) {
    if (storage.buffer.isEmpty()) return;
    if (storage.file.isOpen() && storage.file.write(storage.buffer) != storage.buffer.size()) {
        storage.broken = true;
        qWarning("VideoRenderBridge timing log: write failed for '%s'",
                 qUtf8Printable(storage.file.fileName()));
        storage.file.close();
    }
    storage.buffer.resize(0);
    storage.flushClock.restart();
}

/// A core that has to outlive the bridge that could not free it with its GL
/// context current. It is never destroyed, because destroying an mpv core whose
/// render context is still alive faults inside the renderer; the leak is loud and
/// bounded by one failed teardown, never per frame.
struct OrphanedCore {
    OrphanedCore(std::shared_ptr<MpvHandle>&& lease, std::unique_ptr<MpvRenderContext>&& wrapper)
        : lease(std::move(lease)), wrapper(std::move(wrapper)) {}
    std::shared_ptr<MpvHandle> lease;
    std::unique_ptr<MpvRenderContext> wrapper;
};

QMutex& orphanMutex() {
    static QMutex mutex;
    return mutex;
}

std::vector<OrphanedCore*>& orphanedCores() {
    // Never destroyed: process-static storage whose destructor would run after
    // the GL contexts and the mpv runtime are gone.
    static auto* orphans = new std::vector<OrphanedCore*>();
    return *orphans;
}

}  // namespace

// ── frame-timing log API ─────────────────────────────────────────────────

void setRenderFrameTimingLogPath(const QString& path) {
    TimingLogStorage& storage = timingLogStorage();
    QMutexLocker lock(&storage.mutex);
    if (storage.path == path) return;
    flushTimingLogLocked(storage);
    if (storage.file.isOpen()) storage.file.close();
    storage.file.setFileName(QString());
    storage.path = path;
    storage.broken = false;
    storage.buffer.resize(0);
    // The frame-timing log prints these counters, so they are collected for that
    // launch even when MIKAI_UI_PROFILE was not set.
    mikai::gui::uiprofile::forceCounters(!path.isEmpty());
}

QString renderFrameTimingLogPath() {
    TimingLogStorage& storage = timingLogStorage();
    QMutexLocker lock(&storage.mutex);
    return storage.path;
}

void appendRenderFrameTimingLog(const QString& line) {
    // Overlong input is a caller bug, not a line: dropped rather than truncated.
    if (line.size() > 4096) return;
    TimingLogStorage& storage = timingLogStorage();
    QMutexLocker lock(&storage.mutex);
    if (storage.path.isEmpty() || storage.broken) return;
    if (storage.file.isOpen() && storage.file.fileName() != storage.path) {
        flushTimingLogLocked(storage);
        storage.file.close();
        storage.file.setFileName(QString());
    }
    if (!storage.file.isOpen()) {
        storage.file.setFileName(storage.path);
        if (!storage.file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            storage.broken = true;
            qWarning("VideoRenderBridge timing log: cannot open '%s': %s",
                     qUtf8Printable(storage.path), qUtf8Printable(storage.file.errorString()));
            storage.file.setFileName(QString());
            return;
        }
        storage.flushClock.start();
    }
    storage.buffer.append(line.toUtf8());
    storage.buffer.append('\n');
    if (storage.buffer.size() < 32768 && storage.flushClock.elapsed() < 1000) return;
    flushTimingLogLocked(storage);
}

// ── construction / context lifecycle ─────────────────────────────────────

VideoRenderBridge::VideoRenderBridge(GuiInvoker toGuiThread)
    : toGuiThread_(std::move(toGuiThread)) {}

VideoRenderBridge::~VideoRenderBridge() {
    if (releaseRenderContext()) return;
    // The original GL context is gone or not current, so neither the wrapper nor
    // the core may be freed here. Both are retained (releasePending()) and the
    // reason is reported: this is an adapter bug in its SG/context teardown, not
    // a normal path.
    if (render_ || core_) {
        qCritical("VideoRenderBridge: render context cannot be released without its creating "
                  "OpenGL context current; retaining the wrapper and the core lease");
        OrphanedCore* orphan = new OrphanedCore(std::move(core_), std::move(render_));
        QMutexLocker lock(&orphanMutex());
        orphanedCores().push_back(orphan);
    }
}

void VideoRenderBridge::setGuiInvoker(GuiInvoker toGuiThread) {
    toGuiThread_ = std::move(toGuiThread);
}

bool VideoRenderBridge::createRenderContext(std::shared_ptr<MpvHandle> core,
                                            std::weak_ptr<const void> owner,
                                            std::function<void()> onUpdate, QString* reason) {
    if (!core) {
        if (reason != nullptr) *reason = QStringLiteral("no mpv core lease was provided");
        return false;
    }
    if (QOpenGLContext::currentContext() == nullptr) {
        if (reason != nullptr) *reason = QStringLiteral("no OpenGL context is current");
        return false;
    }
    if (render_ != nullptr) return true;

    // A new context owns a new target attachment whose format says nothing about
    // the one the previous context probed, so the probe starts fresh here as well
    // as at teardown.
    resetTargetFormat();
    core_ = std::move(core);
    updateOwner_ = std::move(owner);
    const quint64 generation = generation_.load() + 1;
    queuedUpdate_.store(0);
    const GlApi gl = loadGlApi();

    // The hop is coalesced: every callback mpv raises before the rendering thread
    // answers it collapses into one, and a callback that arrives after the owner
    // died is dropped instead of reaching destroyed state.
    std::function<void()> relay = [this, generation, onUpdate = std::move(onUpdate)] {
        // A callback raised by a render context that has since been released
        // would touch a view that no longer owns a renderer.
        if (generation != generation_.load()) return;
        if (updateOwner_.expired()) return;
        if (queuedUpdate_.exchange(generation) == generation) return;
        if (onUpdate) onUpdate();
    };

    try {
        render_ = std::make_unique<MpvRenderContext>(*core_, gl, std::move(relay),
                                                     /*advancedControl=*/true);
    } catch (...) {
        render_.reset();
        core_.reset();
        updateOwner_.reset();
        throw;
    }
    // mpv draws with the origin at the bottom-left. The texture the Quick node
    // wraps and the FBO the Widget adapter blits both take that orientation, so
    // the flip stays on for every consumer and no consumer re-flips a frame.
    flipY_ = 1;
    render_->setFlipY(true);
    generation_.store(generation);
    releasePending_.store(false);
    contextReady_.store(true);
    return true;
}

bool VideoRenderBridge::contextAboutToBeDestroyed() {
    clearPacingProfile();
    pacingTimerArmed_ = false;
    pacingTargetTime_ = 0;
    pacingRetryOnly_ = false;
    pacingMeasurePending_ = false;
    pacedPaintReleased_ = false;
    skippingFrames_ = false;
    unpaintableStreak_ = 0;

    if (render_ == nullptr && outputFbo_ == nullptr && engineFrameHoldFbo_ == nullptr) {
        contextReady_.store(false);
        return true;
    }
    QOpenGLContext* current = QOpenGLContext::currentContext();
    if (current == nullptr ||
        (render_ != nullptr && render_->creatingContext() != nullptr &&
         current != render_->creatingContext())) {
        releasePending_.store(true);
        qCritical("VideoRenderBridge: context teardown without the creating OpenGL context "
                  "current; retaining the render wrapper and the core lease");
        return false;
    }
    // One last picture out of this context before its objects go. Qt destroys a
    // scene-graph or widget framebuffer before the context, so the private
    // snapshot is the only thing left to read.
    if (!destroying_.load()) {
        if (!engineFrameHoldFbo_ && hasVisibleOutputFrame_) captureEngineFrameHold();
        captureTeardownPicture();
    }
    deliverReadback(QImage(), QStringLiteral("the render context was destroyed"));

    // Free FIRST, then let go of the GL-owned storage; the core lease is kept so
    // a recreated context can attach to the same core.
    render_.reset();
    outputFbo_.reset();
    outputSize_ = QSize();
    outputInternalFormat_ = 0;
    outputDepthBitsPerChannel_ = 0;
    outputHasFrame_ = false;
    engineFrameHoldFbo_.reset();
    engineFrameHoldActive_ = false;
    engineFrameHoldAwaitingOutput_ = false;
    engineFrameHoldNeedsLoadLanding_ = false;
    engineFrameHoldLandingSeen_ = false;
    engineFrameHoldEpoch_ = 0;
    resetTargetFormat();
    contextReady_.store(false);
    releasePending_.store(false);
    ++generation_;
    queuedUpdate_.store(0);
    hasVisibleOutputFrame_ = false;
    videoFramePainted_ = false;
    framePending_ = false;
    forceRender_ = true;
    return true;
}

bool VideoRenderBridge::releaseRenderContext() {
    clearPacingProfile();
    pacingTimerArmed_ = false;
    pacingTargetTime_ = 0;
    pacingRetryOnly_ = false;
    pacingMeasurePending_ = false;
    pacedPaintReleased_ = false;
    skippingFrames_ = false;
    unpaintableStreak_ = 0;

    if (render_ == nullptr && !outputFbo_ && !engineFrameHoldFbo_) {
        core_.reset();
        updateOwner_.reset();
        contextReady_.store(false);
        releasePending_.store(false);
        queuedUpdate_.store(0);
        return true;
    }
    QOpenGLContext* current = QOpenGLContext::currentContext();
    if (current == nullptr ||
        (render_ != nullptr && render_->creatingContext() != nullptr &&
         current != render_->creatingContext())) {
        // Neither the wrapper nor the core may be dropped under a live context.
        releasePending_.store(true);
        qCritical("VideoRenderBridge: releaseRenderContext without its creating OpenGL context "
                  "current; the render wrapper and core lease are retained, retry from the "
                  "context teardown");
        return false;
    }
    // Teardown readback: one picture, at the target's own precision, before the
    // objects that hold it are freed.
    if (!destroying_.load()) {
        if (!engineFrameHoldFbo_ && hasVisibleOutputFrame_) captureEngineFrameHold();
        captureTeardownPicture();
    }
    deliverReadback(QImage(), QStringLiteral("the render context was released"));

    // Fixed order: the render context is freed while its creating context is
    // current, and only then is the core lease dropped. Freeing the wrapper also
    // detaches mpv's update callback, so a callback in flight cannot reach a
    // destroyed receiver.
    render_.reset();
    outputFbo_.reset();
    outputSize_ = QSize();
    outputInternalFormat_ = 0;
    outputDepthBitsPerChannel_ = 0;
    outputHasFrame_ = false;
    engineFrameHoldFbo_.reset();
    engineFrameHoldActive_ = false;
    engineFrameHoldAwaitingOutput_ = false;
    engineFrameHoldNeedsLoadLanding_ = false;
    engineFrameHoldLandingSeen_ = false;
    engineFrameHoldEpoch_ = 0;
    resetTargetFormat();
    contextReady_.store(false);
    releasePending_.store(false);
    ++generation_;
    queuedUpdate_.store(0);
    updateOwner_.reset();
    core_.reset();
    hasVisibleOutputFrame_ = false;
    videoFramePainted_ = false;
    framePending_ = false;
    forceRender_ = true;
    return true;
}

// ── stable output ────────────────────────────────────────────────────────

bool VideoRenderBridge::ensureOutput(const RenderTargetFacts& facts) {
    const QSize size = facts.devicePixelSize;
    if (size.width() <= 0 || size.height() <= 0) return false;
    int format = facts.colorInternalFormat;
    if (!supportedSizedFormat(format)) format = kGlInternalFormatRgba8;
    if (outputFbo_ != nullptr && outputFbo_->isValid() && outputSize_ == size &&
        outputInternalFormat_ == format) {
        // Same target: nothing is reallocated. Only the depth hint can change.
        outputDepthBitsPerChannel_ = facts.depthBitsPerChannel;
        return true;
    }
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (ctx == nullptr) return false;
    QOpenGLFramebufferObjectFormat fboFormat;
    fboFormat.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    fboFormat.setInternalTextureFormat(format);
    auto fbo = std::make_unique<QOpenGLFramebufferObject>(size, fboFormat);
    if (!fbo->isValid()) {
        outputFbo_.reset();
        outputSize_ = QSize();
        outputInternalFormat_ = 0;
        return false;
    }
    // A new attachment holds undefined pixels outside the video rectangle, and
    // this buffer is what every consumer blits or wraps.
    QOpenGLFunctions* gl = ctx->functions();
    const FboBindings bindings = captureFboBindings(gl);
    if (fbo->bind()) {
        gl->glViewport(0, 0, size.width(), size.height());
        gl->glDisable(GL_BLEND);
        gl->glDisable(GL_SCISSOR_TEST);
        gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        gl->glClear(GL_COLOR_BUFFER_BIT);
    }
    restoreFboBindings(gl, bindings);
    outputFbo_ = std::move(fbo);
    outputSize_ = size;
    outputInternalFormat_ = format;
    outputDepthBitsPerChannel_ = facts.depthBitsPerChannel;
    outputHasFrame_ = false;
    return true;
}

unsigned int VideoRenderBridge::outputTextureId() const {
    return outputFbo_ != nullptr ? outputFbo_->texture() : 0;
}

int VideoRenderBridge::outputFbo() const {
    return outputFbo_ != nullptr ? static_cast<int>(outputFbo_->handle()) : 0;
}

QSize VideoRenderBridge::outputSize() const {
    return outputSize_;
}

int VideoRenderBridge::outputInternalFormat() const {
    return outputInternalFormat_;
}

// ── render pass ──────────────────────────────────────────────────────────

PaintScheduleDecision VideoRenderBridge::decidePaintSchedule(bool pacedRelease, bool forced,
                                                             bool armedSlot, bool frameOwed,
                                                             bool holdOwesOutput) {
    PaintScheduleDecision decision;
    // A hold taken across a source change is released by the frame it waits for,
    // and that release is a presentation of its own.
    decision.consumesFrame = pacedRelease || forced || (frameOwed && (!armedSlot || holdOwesOutput));
    decision.keepsArmedSlot = armedSlot && !decision.consumesFrame;
    return decision;
}

RenderResult VideoRenderBridge::renderToOutput(const RenderSnapshot& snapshot) {
    RenderResult result;
    // This pass answers whatever the coalescing relay queued before it ran.
    queuedUpdate_.store(0);

    const bool profilePaint = mikai::gui::uiprofile::countersEnabled();
    const qint64 paintStartNs = profilePaint ? mikai::gui::uiprofile::nowNs() : 0;
    if (profilePaint) {
        ++profilePaintCalls_;
        profileLastPaintEntryNs_ = paintStartNs;
        if (profilePacingArmedAtNs_ != 0 && profilePacingAwaitingPaint_) {
            profileLastPaintDelayMs_ =
                mikai::gui::uiprofile::elapsedMs(profilePacingArmedAtNs_);
        } else {
            profileLastPaintDelayMs_ = -1;
        }
    }

    if (render_ == nullptr || !contextReady_.load()) {
        pacedPaintReleased_ = false;
        result.pass = RenderResult::Pass::None;
        // A context gap: the picture read back from the previous context is the
        // frame the viewer was looking at. It is put into the stable output so
        // both adapters show it through the same path (the Widget blit or the
        // Quick node), at this target's own precision.
        QOpenGLContext* gapContext = QOpenGLContext::currentContext();
        if (gapContext == nullptr || teardownPicture_.isNull() ||
            !ensureOutput(snapshot.target)) {
            return result;
        }
        QOpenGLFunctions* gapGl = gapContext->functions();
        // Whatever Qt (or a caller) had bound must be back at return: the adapter
        // returns to Qt's renderer, which continues drawing into ITS target.
        const FboBindings gapBindings = captureFboBindings(gapGl);
        const int gapWidth = outputSize_.width();
        const int gapHeight = outputSize_.height();
        const GLuint gapTarget = static_cast<GLuint>(outputFbo_->handle());
        gapGl->glBindFramebuffer(GL_FRAMEBUFFER, gapTarget);
        gapGl->glViewport(0, 0, gapWidth, gapHeight);
        gapGl->glDisable(GL_BLEND);
        gapGl->glDisable(GL_SCISSOR_TEST);
        gapGl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        gapGl->glClear(GL_COLOR_BUFFER_BIT);
        if (!drawRetainedPicture(gapTarget, gapWidth, gapHeight)) return result;
        restoreFboBindings(gapGl, gapBindings);
        result.pass = RenderResult::Pass::TeardownPicture;
        result.targetWidth = gapWidth;
        result.targetHeight = gapHeight;
        ++presentationSerial_;
        result.presentationSerial = presentationSerial_;
        if (pendingReadback_) {
            QString reason;
            const QImage picture = readVideoFrame(&reason);
            deliverReadback(picture, reason);
        }
        return result;
    }
    if (!ensureOutput(snapshot.target)) {
        // No target size, nothing to render into; not a renderer failure.
        pacedPaintReleased_ = false;
        result.pass = RenderResult::Pass::None;
        return result;
    }
    QOpenGLContext* glContext = QOpenGLContext::currentContext();
    if (glContext == nullptr) {
        result.pass = RenderResult::Pass::Failed;
        if (!renderFailureStreak_) {
            renderFailureStreak_ = true;
            result.renderError = -1;
        }
        return result;
    }
    QOpenGLFunctions* gl = glContext->functions();
    QOpenGLExtraFunctions* extra = glContext->extraFunctions();
    // The pass owns the framebuffer binding only between entry and return. The
    // adapter hands the context straight back to Qt's renderer, whose binding
    // cache would otherwise keep drawing into this bridge's output.
    const FboBindings entryBindings = captureFboBindings(gl);

    const int width = outputSize_.width();
    const int height = outputSize_.height();
    result.targetWidth = width;
    result.targetHeight = height;
    const GLuint target = static_cast<GLuint>(outputFbo_->handle());

    // ── what this pass may take ───────────────────────────────────────────
    const bool pacedRelease = pacedPaintReleased_;
    pacedPaintReleased_ = false;
    const bool armedSlot = pacingTimerArmed_ && !pacingRetryOnly_;
    PaintScheduleDecision schedule = decidePaintSchedule(
        pacedRelease, forceRender_, armedSlot, framePending_, engineFrameHoldAwaitingOutput_);

    bool frameArrived = false;
    if (schedule.consumesFrame || !schedule.keepsArmedSlot) {
        // A paused chrome repaint already has the picture it will show: asking
        // mpv whether a frame arrived is a context sync with nothing coming.
        //
        // It is only quiet while the output really is the current source's
        // picture. A load that has been submitted but not yet delivered a frame
        // (landing gate armed) makes `outputHasFrame_` describe the OUTGOING
        // source, and skipping update() here is what left a paused reload with no
        // frames: nothing ever asked mpv again after the forced paint that ran
        // before the new source had one.
        const bool pausedQuiet = snapshot.paused && !framePending_ && !pacingTimerArmed_ &&
                                 !engineFrameHoldAwaitingOutput_ &&
                                 !engineFrameHoldNeedsLoadLanding_ && !schedule.consumesFrame &&
                                 outputHasFrame_;
        if (!pausedQuiet) {
            const uint64_t flags = render_->update();
            frameArrived = (flags & MPV_RENDER_UPDATE_FRAME) != 0;
            if (!schedule.consumesFrame)
                schedule = decidePaintSchedule(false, false, false, frameArrived, false);
        }
    }
    const bool frameOwed = framePending_ || frameArrived;
    const bool consumesFrame = schedule.consumesFrame;
    const bool newOutputFrame = consumesFrame && frameOwed;
    if (schedule.keepsArmedSlot) {
        // The slot is the frame's: it survives this pass, with its measurement.
        if (profilePaint) profileLastPaintDelayMs_ = -1;
    } else {
        if (profilePaint && armedSlot && !frameOwed) ++profilePacingDisarms_;
        clearPacingProfile();
        pacingTimerArmed_ = false;
        pacingTargetTime_ = 0;
        pacingRetryOnly_ = false;
    }

    // A hold asked for while no context was current could not capture; take it
    // now, from the frame mpv is still holding.
    if (engineFrameHoldActive_ && !engineFrameHoldFbo_ && outputHasFrame_) {
        captureEngineFrameHold();
    }

    bool freshOutputFrame = false;
    int frameInfoValid = -1;
    if (engineFrameHoldAwaitingOutput_ || newOutputFrame) {
        mpv_render_frame_info frameInfo{};
        frameInfoValid = render_->nextFrameInfo(frameInfo) ? 1 : 0;
        const bool realFrame = frameInfoValid == 1 && isRealNewFrame(frameInfo);
        // mpv bumps the command epoch at SUBMISSION, so an epoch comparison is
        // already true for the outgoing source's last frame; only the landing
        // event proves the replacement took.
        const bool replacementLanded =
            !engineFrameHoldNeedsLoadLanding_ || engineFrameHoldLandingSeen_;
        freshOutputFrame = realFrame && replacementLanded;
    }
    // forceRender_ covers a framebuffer Qt hands back empty: a resize or a
    // re-show wipes the picture, and with a paused core no frame is coming.
    const bool haveFrame = newOutputFrame || forceRender_;
    const bool retainedPictureVisible =
        engineFrameHoldFbo_ != nullptr &&
        (engineFrameHoldActive_ || (engineFrameHoldAwaitingOutput_ && !freshOutputFrame));

    bool presentedFrame = false;
    bool heldPictureDrawn = false;
    bool presentedPictureDrawn = false;
    int renderResult = 0;
    qint64 retentionMicros = 0;
    qint64 engineMicros = 0;
    qint64 phaseMark = profilePaint ? mikai::gui::uiprofile::nowNs() : 0;

    const auto clearOutput = [&] {
        gl->glBindFramebuffer(GL_FRAMEBUFFER, target);
        gl->glViewport(0, 0, width, height);
        gl->glDisable(GL_BLEND);
        gl->glDisable(GL_SCISSOR_TEST);
        gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        gl->glClear(GL_COLOR_BUFFER_BIT);
    };

    if (retainedPictureVisible) {
        heldPictureDrawn = drawEngineFrameHold(target, width, height);
        if (heldPictureDrawn && newOutputFrame) {
            // The hold stands in for the frame this pass consumed, so mpv is told
            // the frame was consumed and the next one may come.
            renderResult = render_->skipRendering();
            if (renderResult == 0) render_->reportSwap();
            framePending_ = false;
        }
        if (heldPictureDrawn) {
            outputHasFrame_ = false;
            result.pass = RenderResult::Pass::HeldPicture;
            result.presentedHeldPicture = true;
        }
        if (profilePaint) {
            retentionMicros += (mikai::gui::uiprofile::nowNs() - phaseMark) / 1000;
            phaseMark = mikai::gui::uiprofile::nowNs();
        }
    } else if (!consumesFrame && (outputHasFrame_ || armedSlot)) {
        // A UI-only pass presents the picture that is already in the stable
        // output. Re-running mpv's render here would consume the frame the armed
        // slot is still waiting to present, so the slot's frame is left pending.
        presentedPictureDrawn = true;
        result.pass = RenderResult::Pass::OutputReuse;
        result.reusedOutput = true;
    } else {
        clearOutput();
        renderResult = render_->render(static_cast<int>(target), width, height,
                                       outputInternalFormat_, outputDepthBitsPerChannel_);
        presentedFrame = haveFrame && renderResult == 0;
        if (renderResult >= 0) {
            outputHasFrame_ = true;
            result.pass = RenderResult::Pass::VideoFrame;
        }
    }
    if (profilePaint) {
        engineMicros += (mikai::gui::uiprofile::nowNs() - phaseMark) / 1000;
    }

    // Put back the state mpv leaves behind: Qt owns the paint engine's binding
    // and array state outside this window. GL_UNPACK_ROW_LENGTH does not exist in
    // an ES context, where setting it is INVALID_ENUM.
    if (extra != nullptr) {
        extra->glBindVertexArray(0);
        if (!glContext->isOpenGLES()) extra->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        extra->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
    restoreFboBindings(gl, entryBindings);

    // ── failure handling ─────────────────────────────────────────────────
    if (renderResult < 0) {
        outputHasFrame_ = false;
        videoFramePainted_ = false;
        result.pass = RenderResult::Pass::Failed;
        // One report per streak: a renderer that fails every pass would repeat
        // the same message for every frame it was asked for.
        result.renderError = renderFailureStreak_ ? 0 : renderResult;
        renderFailureStreak_ = true;
        // The retained picture stands in for the frame the renderer could not
        // produce, so a reported failure is never also a black clear.
        const qint64 fallbackStart = profilePaint ? mikai::gui::uiprofile::nowNs() : 0;
        const bool retainedDrawn = drawRetainedPicture(target, width, height);
        if (!retainedDrawn) clearOutput();
        if (profilePaint) {
            retentionMicros += (mikai::gui::uiprofile::nowNs() - fallbackStart) / 1000;
        }
        restoreFboBindings(gl, entryBindings);
    } else {
        renderFailureStreak_ = false;
        hasRenderedFrame_ = true;
        if (presentedFrame) {
            // Only ever set: a UI-only repaint must not throw away the report the
            // frame before it is still owed.
            videoFramePainted_ = true;
        }
    }

    // ── hold release / landing gate ──────────────────────────────────────
    if (renderResult == 0 && freshOutputFrame && !retainedPictureVisible) {
        hasVisibleOutputFrame_ = true;
        outputHasFrame_ = true;
        engineFrameHoldAwaitingOutput_ = false;
        engineFrameHoldNeedsLoadLanding_ = false;
        engineFrameHoldLandingSeen_ = false;
        engineFrameHoldEpoch_ = 0;
        // The fresh output replaced the transition snapshot: its GPU storage goes
        // now, so ordinary playback retains no extra video framebuffer.
        if (!engineFrameHoldActive_) engineFrameHoldFbo_.reset();
        // A frame of the frame the reload asked for is on screen - the landing
        // gate above proved it - and the teardown readback has done its job.
        teardownPicture_ = QImage();
        teardownPictureFormat_ = 0;
    }
    if (renderResult == 0 && newOutputFrame && presentedFrame) {
        hasVisibleOutputFrame_ = true;
        teardownPicture_ = QImage();
        teardownPictureFormat_ = 0;
        result.outputUpdated = true;
    }
    result.freshOutputFrame = renderResult == 0 && freshOutputFrame && presentedFrame;
    result.frameConsumed = consumesFrame;
    result.holdReleased = result.freshOutputFrame && !retainedPictureVisible;

    if (consumesFrame) framePending_ = false;
    forceRender_ = false;

    if (result.pass != RenderResult::Pass::Failed &&
        result.pass != RenderResult::Pass::None && !schedule.keepsArmedSlot) {
        ++presentationSerial_;
        result.presentationSerial = presentationSerial_;
    }

    // ── on-demand readback, exactly once ─────────────────────────────────
    if (pendingReadback_ && renderResult >= 0 &&
        result.pass != RenderResult::Pass::None) {
        QString reason;
        const QImage picture = readVideoFrame(&reason);
        deliverReadback(picture, reason);
    }

    if (!renderFrameTimingLogPath().isEmpty()) {
        logFrameTiming(
            QStringLiteral("paint result=%1 newframe=%2 pending=%3 forced=%4 held=%5 reused=%6 "
                           "lead_ms=%7 paint_delay_ms=%8 disarm=%9 internal_format=%10")
                .arg(renderResult)
                .arg(static_cast<int>(newOutputFrame))
                .arg(static_cast<int>(framePending_))
                .arg(static_cast<int>(forceRender_))
                .arg(static_cast<int>(heldPictureDrawn))
                .arg(static_cast<int>(presentedPictureDrawn))
                .arg(QString::number(renderLeadMs_, 'f', 2))
                .arg(profileLastPaintDelayMs_.load())
                .arg(static_cast<qulonglong>(profilePacingDisarms_.load()))
                .arg(outputInternalFormat_, 0, 16));
    }

    if (profilePaint) {
        publishProfilePaint(result, paintStartNs, engineMicros, retentionMicros);
        profilePacingReason_ = "none";
    }
    return result;
}

RenderResult VideoRenderBridge::render(const RenderSnapshot& snapshot, int targetFbo,
                                       int targetWidth, int targetHeight) {
    RenderResult result = renderToOutput(snapshot);
    blitOutput(targetFbo, targetWidth, targetHeight);
    if (targetWidth > 0 && targetHeight > 0) {
        result.targetWidth = targetWidth;
        result.targetHeight = targetHeight;
    }
    return result;
}

void VideoRenderBridge::blitOutput(int targetFbo, int targetWidth, int targetHeight) {
    if (outputFbo_ == nullptr || !outputFbo_->isValid() || targetWidth <= 0 ||
        targetHeight <= 0) {
        return;
    }
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (ctx == nullptr) return;
    QOpenGLExtraFunctions* gl = ctx->extraFunctions();
    if (gl == nullptr) return;
    const FboBindings bindings = captureFboBindings(gl);
    // A filtered blit between fixed-point and floating-point buffers is not
    // defined by GL, so the formats are compared instead of assumed equal.
    const GLenum filter =
        outputInternalFormat_ == renderInternalFormat_ ? GL_LINEAR : GL_NEAREST;
    bindFboRead(gl, outputFbo_->handle());
    bindFboDraw(gl, static_cast<GLuint>(targetFbo));
    gl->glBlitFramebuffer(0, 0, outputFbo_->width(), outputFbo_->height(), 0, 0, targetWidth,
                          targetHeight, GL_COLOR_BUFFER_BIT, filter);
    restoreFboBindings(gl, bindings);
}

void VideoRenderBridge::advanceRenderState(const RenderSnapshot& snapshot, bool canPresent) {
    Q_UNUSED(snapshot);
    queuedUpdate_.store(0);
    if (render_ == nullptr || !contextReady_.load()) return;
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (ctx == nullptr) return;
    // update() runs on every callback, painted or not: in advanced mode it is
    // what releases the core's queued work and allocates its decoder textures.
    const uint64_t flags = render_->update();
    const bool frame = (flags & MPV_RENDER_UPDATE_FRAME) != 0;
    if (canPresent) {
        // Leaving skip mode forces one redraw: the buffer mpv last handed over
        // may have been consumed by a skip.
        if (skippingFrames_) {
            skippingFrames_ = false;
            forceRender_ = true;
            if (!renderFrameTimingLogPath().isEmpty())
                logFrameTiming(QStringLiteral("skip-mode left; forced redraw"));
        }
        unpaintableStreak_ = 0;
        framePending_ = framePending_ || frame;
        return;
    }
    // A single transient non-paintable round (a popup, an occlusion, a DWM
    // operation) must not discard a frame: the streak has to persist first.
    ++unpaintableStreak_;
    if (unpaintableStreak_ < kUnpaintableGraceTicks) {
        framePending_ = framePending_ || frame;
        if (!pacingTimerArmed_ || pacingRetryOnly_) {
            clearPacingProfile();
            pacingTimerArmed_ = true;
            pacingTargetTime_ = 0;
            // Retry timers must not bias presentation-lead estimates.
            pacingRetryOnly_ = true;
        }
        if (!renderFrameTimingLogPath().isEmpty())
            logFrameTiming(QStringLiteral("unpaintable streak=%1 (grace; frame held)")
                               .arg(unpaintableStreak_));
        return;
    }
    if (!skippingFrames_) {
        skippingFrames_ = true;
        if (!renderFrameTimingLogPath().isEmpty())
            logFrameTiming(QStringLiteral("skip-mode entered (streak=%1)").arg(unpaintableStreak_));
    }
    // Nothing can be composited, so the work mpv waits on is answered with ONE
    // report: its new frame is consumed by a skip (which the render API counts as
    // rendered), or the painted frame the compositor will never show is acked as
    // the intentional drop it is.
    const bool owed = videoFramePainted_ || frame;
    const bool consumed = !frame || render_->skipRendering() == 0;
    framePending_ = false;
    if (consumed) {
        videoFramePainted_ = false;
        if (owed) render_->reportSwap();
    }
}

void VideoRenderBridge::refresh(const RenderSnapshot& snapshot, bool canPresent) {
    // The chain changed under a possibly paused core, so the next paint redraws
    // even when mpv announces no new frame. This is the paused-refresh path, and
    // it covers position zero: no frame is required to run it.
    forceRender_ = true;
    advanceRenderState(snapshot, canPresent);
}

void VideoRenderBridge::acknowledgeDroppedFrame() {
    if (!videoFramePainted_) return;
    videoFramePainted_ = false;
    if (render_ == nullptr) return;
    render_->reportSwap();
}

void VideoRenderBridge::reportPresentedFrame() {
    // The lead is measured at the swap: composition and the vsync wait happen
    // between the render pass returning and the compositor getting the frame.
    if (pacingMeasurePending_) {
        pacingMeasurePending_ = false;
        const double tookMs = static_cast<double>(steadyClockNowNs() - pacingFiredAtNs_) / 1.0e6;
        if (tookMs > 0.0 && tookMs < 1000.0) {
            renderLeadMs_ = std::clamp(0.8 * renderLeadMs_ + 0.2 * tookMs, kMinRenderLeadMs,
                                       kMaxRenderLeadMs);
        }
    }
    if (!videoFramePainted_) return;
    videoFramePainted_ = false;
    if (render_ == nullptr) return;
    if (!renderFrameTimingLogPath().isEmpty()) {
        logFrameTiming(
            QStringLiteral("swap now=%1").arg(QString::number(steadyClockNowNs())));
    }
    render_->reportSwap();
}

// ── pacing ───────────────────────────────────────────────────────────────

int VideoRenderBridge::schedulePacedPaint(bool canPresent, int64_t targetTimeUs,
                                          int64_t nowUs, uint64_t apiVersion) {
    const bool profilePacing = mikai::gui::uiprofile::countersEnabled();
    const bool haveWork = framePending_ || videoFramePainted_;

    if (!canPresent) {
        if (forceRender_ || haveWork) {
            clearPacingProfile();
            pacingTimerArmed_ = true;
            pacingTargetTime_ = 0;
            pacingRetryOnly_ = true;
            if (profilePacing) profilePacingReason_ = "unpaintable";
            return kUnpaintableRetryMs;
        }
        return -1;
    }
    if (forceRender_ || !haveWork) {
        clearPacingProfile();
        pacingTimerArmed_ = false;
        pacingTargetTime_ = 0;
        pacingRetryOnly_ = false;
        if (profilePacing) profilePacingReason_ = forceRender_ ? "forced" : "idle";
        return forceRender_ ? 0 : -1;
    }
    if (!clientFramePacingEnabled()) {
        clearPacingProfile();
        pacingTimerArmed_ = false;
        pacingTargetTime_ = 0;
        pacingRetryOnly_ = false;
        if (profilePacing) profilePacingReason_ = "disabled";
        return 0;
    }
    if (!framePending_ && videoFramePainted_) {
        // A painted frame needs composition and its swap report, not a slot.
        clearPacingProfile();
        pacingTimerArmed_ = false;
        pacingTargetTime_ = 0;
        pacingRetryOnly_ = false;
        if (profilePacing) profilePacingReason_ = "awaiting-swap";
        return 0;
    }

    // The caller may pass zeros: the bridge can read mpv's notification itself,
    // so an adapter never has to know target_time units or the client API version.
    int64_t rawTargetTime = targetTimeUs;
    bool haveTarget = rawTargetTime > 0;
    bool repeatedOrRedraw = false;
    if (render_ != nullptr) {
        if (rawTargetTime <= 0 || nowUs <= 0 || apiVersion == 0) {
            mpv_render_frame_info info{};
            if (render_->nextFrameInfo(info)) {
                repeatedOrRedraw =
                    (info.flags & (MPV_RENDER_FRAME_INFO_REDRAW | MPV_RENDER_FRAME_INFO_REPEAT)) !=
                    0;
                if (rawTargetTime <= 0 && isRealNewFrame(info)) {
                    rawTargetTime = info.target_time;
                    haveTarget = true;
                }
            }
            if (nowUs <= 0) nowUs = render_->timeUs();
            if (apiVersion == 0) apiVersion = MpvHandle::apiVersion();
        }
    } else {
        if (nowUs <= 0) nowUs = steadyClockNowNs() / 1000;
        if (apiVersion == 0) apiVersion = MpvHandle::apiVersion();
    }

    const std::optional<int> delay =
        haveTarget ? clientPacingDelayMilliseconds(rawTargetTime, nowUs, renderLeadMs_, apiVersion)
                   : std::nullopt;
    if (!delay.has_value()) {
        // No frame info, a slot that already passed, or a target so far out it
        // belongs to a paused core: paint at once.
        clearPacingProfile();
        pacingTimerArmed_ = false;
        pacingTargetTime_ = 0;
        pacingRetryOnly_ = false;
        if (profilePacing) {
            profilePacingReason_ = repeatedOrRedraw
                                       ? "repeat"
                                       : (haveTarget ? "elapsed" : "no-info");
        }
        return 0;
    }
    if (pacingTimerArmed_ && !pacingRetryOnly_ && pacingTargetTime_ <= rawTargetTime) {
        // Preserve the earlier presentation slot the adapter already armed.
        if (profilePacing) profilePacingReason_ = "paced";
        return -1;
    }
    pacingTimerArmed_ = true;
    pacingTargetTime_ = rawTargetTime;
    pacingRetryOnly_ = false;
    if (profilePacing) {
        // The measurement window is wall-clock nanoseconds; mpv's clock unit is
        // per API version and is never mixed into it.
        profilePacingArmedAtNs_ = steadyClockNowNs();
        profilePacingAwaitingPaint_ = true;
        profilePacingFired_ = false;
        profilePacingReason_ = "paced";
    }
    if (!renderFrameTimingLogPath().isEmpty()) {
        logFrameTiming(QStringLiteral("pace deferred target_time=%1 now_us=%2 delay_ms=%3 lead_ms=%4")
                           .arg(QString::number(rawTargetTime), QString::number(nowUs))
                           .arg(*delay)
                           .arg(QString::number(renderLeadMs_, 'f', 2)));
    }
    return *delay;
}

void VideoRenderBridge::notePacedPaintStarted() {
    if (!pacingTimerArmed_) return;
    const bool retry = pacingRetryOnly_;
    pacingTimerArmed_ = false;
    pacingTargetTime_ = 0;
    pacingRetryOnly_ = false;
    if (retry) {
        pacedPaintReleased_ = false;
        pacingMeasurePending_ = false;
        pacingFiredAtNs_ = 0;
        return;
    }
    // The paint this update() asks for is the frame's presentation, not a
    // repaint: it may consume the frame and report its swap.
    pacedPaintReleased_ = true;
    pacingFiredAtNs_ = steadyClockNowNs();
    pacingMeasurePending_ = true;
    if (mikai::gui::uiprofile::countersEnabled() && profilePacingAwaitingPaint_)
        profilePacingFired_ = true;
}

void VideoRenderBridge::stopPacing() {
    pacingTimerArmed_ = false;
    pacingTargetTime_ = 0;
    pacingRetryOnly_ = false;
    pacingMeasurePending_ = false;
    pacedPaintReleased_ = false;
    clearPacingProfile();
}

void VideoRenderBridge::clearPacingProfile() {
    profilePacingArmedAtNs_ = 0;
    profilePacingAwaitingPaint_ = false;
    profilePacingFired_ = false;
    profilePacingReason_ = "none";
}

// ── retained pictures ────────────────────────────────────────────────────

void VideoRenderBridge::setEngineFrameHold(bool active) {
    if (active) {
        if (engineFrameHoldActive_) return;
        engineFrameHoldActive_ = true;
        engineFrameHoldAwaitingOutput_ = false;
        // Capture before the filter changes; ordinary playback copies nothing. A
        // filter change needs no load to land, so it must not inherit a gate.
        if (!engineFrameHoldFbo_) {
            engineFrameHoldNeedsLoadLanding_ = false;
            engineFrameHoldLandingSeen_ = false;
            engineFrameHoldEpoch_ = 0;
            // Without a current context this is a no-op; renderToOutput() takes
            // the capture as soon as the rendering thread has one.
            captureEngineFrameHold();
        }
        return;
    }
    if (!engineFrameHoldActive_ && !engineFrameHoldAwaitingOutput_) return;
    engineFrameHoldActive_ = false;
    // The first successful new output frame is the only event that replaces a
    // held picture; without one the next paint may render directly.
    engineFrameHoldAwaitingOutput_ = engineFrameHoldFbo_ != nullptr;
}

void VideoRenderBridge::retainAcrossLoad() {
    if (engineFrameHoldActive_) return;  // a freeze transition owns it
    if (engineFrameHoldFbo_ && engineFrameHoldAwaitingOutput_) {
        // The snapshot is already the right one, but the gate belongs to the load
        // about to be issued, not to the last one that landed.
        engineFrameHoldNeedsLoadLanding_ = true;
        engineFrameHoldLandingSeen_ = false;
        return;
    }
    if (hasVisibleOutputFrame_ && captureEngineFrameHold()) {
        // Awaiting, not active: the picture stands in only until the replacement
        // delivers and then releases its GPU storage.
        engineFrameHoldAwaitingOutput_ = true;
        engineFrameHoldNeedsLoadLanding_ = true;
        engineFrameHoldLandingSeen_ = false;
        return;
    }
    // No picture to keep, but the frame read out of a dying context is still what
    // the viewer was looking at, so it stays until the reload shows something.
    if (teardownPicture_.isNull()) {
        invalidateRetainedPictures();
    } else {
        clearRetainedPictureKeepingTeardown();
    }
}

void VideoRenderBridge::armLoadLanding(uint64_t sourceEpoch) {
    // The stable output still holds the OUTGOING source's picture at this point.
    // Until the incoming one delivers, "the output has a frame" must not be read
    // as "the current source's picture is on screen": that is what keeps the
    // next pass pulling mpv instead of reusing the stale picture forever.
    outputHasFrame_ = false;
    engineFrameHoldNeedsLoadLanding_ = true;
    engineFrameHoldLandingSeen_ = false;
    engineFrameHoldEpoch_ = sourceEpoch;
}

void VideoRenderBridge::noteLoadLanded(uint64_t sourceEpoch) {
    if (!engineFrameHoldNeedsLoadLanding_) return;
    if (engineFrameHoldEpoch_ != 0 && sourceEpoch != engineFrameHoldEpoch_) return;
    engineFrameHoldLandingSeen_ = true;
}

void VideoRenderBridge::noteLoadRefused(uint64_t sourceEpoch) {
    if (!engineFrameHoldNeedsLoadLanding_) return;
    if (engineFrameHoldEpoch_ != 0 && sourceEpoch != engineFrameHoldEpoch_) return;
    // A load that will never land must not gate the next frame forever.
    engineFrameHoldNeedsLoadLanding_ = false;
    engineFrameHoldLandingSeen_ = false;
    engineFrameHoldEpoch_ = 0;
}

void VideoRenderBridge::invalidateRetainedPictures() {
    engineFrameHoldActive_ = false;
    engineFrameHoldAwaitingOutput_ = false;
    engineFrameHoldNeedsLoadLanding_ = false;
    engineFrameHoldLandingSeen_ = false;
    engineFrameHoldEpoch_ = 0;
    hasVisibleOutputFrame_ = false;
    if (engineFrameHoldFbo_) {
        // ~QOpenGLFramebufferObject needs its GL context; the adapters queue this
        // with the owning context current. Qt reports the mismatch itself if not.
        engineFrameHoldFbo_.reset();
    }
    invalidatePresentedOutput();
    teardownPicture_ = QImage();
    teardownPictureFormat_ = 0;
}

void VideoRenderBridge::clearRetainedPictureKeepingTeardown() {
    engineFrameHoldActive_ = false;
    engineFrameHoldAwaitingOutput_ = false;
    engineFrameHoldNeedsLoadLanding_ = false;
    engineFrameHoldLandingSeen_ = false;
    engineFrameHoldEpoch_ = 0;
    hasVisibleOutputFrame_ = false;
    if (engineFrameHoldFbo_) engineFrameHoldFbo_.reset();
}

void VideoRenderBridge::invalidatePresentedOutput() {
    outputHasFrame_ = false;
    hasVisibleOutputFrame_ = false;
    videoFramePainted_ = false;
}

bool VideoRenderBridge::hasRetainedPicture() const {
    return (engineFrameHoldFbo_ != nullptr && engineFrameHoldFbo_->isValid()) ||
           !teardownPicture_.isNull();
}

qint64 VideoRenderBridge::retainedPictureBytes() const {
    qint64 bytes = teardownPicture_.sizeInBytes();
    if (engineFrameHoldFbo_ && engineFrameHoldFbo_->isValid()) {
        // The snapshot is stored at the target's own precision, so its per-pixel
        // cost follows the probed format, not a fixed RGBA8 assumption.
        bytes += qint64(engineFrameHoldFbo_->width()) * engineFrameHoldFbo_->height() *
                 bytesPerPixel(engineFrameHoldFbo_->format().internalTextureFormat());
    }
    return bytes;
}

bool VideoRenderBridge::captureEngineFrameHold() {
    if (!contextReady_.load() || render_ == nullptr) return false;
    if (!hasVisibleOutputFrame_ && !outputHasFrame_) return false;
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (ctx == nullptr) return false;
    if (render_->creatingContext() != nullptr && ctx != render_->creatingContext()) return false;
    const QSize size = outputSize_;
    if (size.isEmpty()) return false;
    // The snapshot takes the output's own precision: an 8-bit copy of a 10-bit or
    // float target would quantize the picture the hold exists to preserve.
    const int format = outputInternalFormat_ != 0 ? outputInternalFormat_ : kGlInternalFormatRgba8;
    if (engineFrameHoldFbo_ == nullptr || engineFrameHoldFbo_->size() != size ||
        static_cast<int>(engineFrameHoldFbo_->format().internalTextureFormat()) != format) {
        QOpenGLFramebufferObjectFormat fboFormat;
        fboFormat.setAttachment(QOpenGLFramebufferObject::NoAttachment);
        fboFormat.setInternalTextureFormat(format);
        auto snapshot = std::make_unique<QOpenGLFramebufferObject>(size, fboFormat);
        if (!snapshot->isValid()) return false;
        engineFrameHoldFbo_.reset();
        engineFrameHoldFbo_ = std::move(snapshot);
    }
    QOpenGLFunctions* gl = ctx->functions();
    const FboBindings bindings = captureFboBindings(gl);
    bool frameConsumed = false;
    bool captured = false;
    if (engineFrameHoldFbo_->bind()) {
        gl->glViewport(0, 0, size.width(), size.height());
        gl->glDisable(GL_BLEND);
        gl->glDisable(GL_SCISSOR_TEST);
        // New attachments have undefined pixels outside the video rectangle.
        gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        gl->glClear(GL_COLOR_BUFFER_BIT);
        const uint64_t updateFlags = render_->update();
        frameConsumed = framePending_ || (updateFlags & MPV_RENDER_UPDATE_FRAME) != 0;
        const int result = render_->render(static_cast<int>(engineFrameHoldFbo_->handle()),
                                           size.width(), size.height(), format,
                                           outputDepthBitsPerChannel_);
        captured = result >= 0;
    }
    restoreFboBindings(gl, bindings);
    if (!captured) {
        engineFrameHoldFbo_.reset();
        return false;
    }
    framePending_ = false;
    // Capturing can consume an update, but it is not an on-screen presentation.
    if (frameConsumed || videoFramePainted_) {
        videoFramePainted_ = false;
        render_->reportSwap();
    }
    return true;
}

bool VideoRenderBridge::captureTeardownPicture() {
    if (destroying_.load()) return false;
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (ctx == nullptr) return false;
    QOpenGLExtraFunctions* gl = ctx->extraFunctions();
    if (gl == nullptr) return false;
    if (engineFrameHoldFbo_ == nullptr || !engineFrameHoldFbo_->isValid()) return false;
    const QSize size = engineFrameHoldFbo_->size();
    if (size.isEmpty()) return false;
    const int format =
        static_cast<int>(engineFrameHoldFbo_->format().internalTextureFormat());
    QImage picture = readFramebufferPixels(gl, engineFrameHoldFbo_->handle(), size, format);
    if (picture.isNull()) return false;
    teardownPicture_ = std::move(picture);
    teardownPictureFormat_ = format;
    mikai::gui::uiprofile::noteTeardownPictureCaptured();
    if (mikai::gui::uiprofile::enabled()) {
        mikai::gui::uiprofile::log(
            "surface.teardownPicture",
            QStringLiteral("size=%1x%2 internal_format=%3 bytes=%4")
                .arg(size.width())
                .arg(size.height())
                .arg(teardownPictureFormat_, 0, 16)
                .arg(teardownPicture_.sizeInBytes()));
    }
    return true;
}

bool VideoRenderBridge::drawEngineFrameHold(unsigned int target, int width, int height) {
    if (engineFrameHoldFbo_ == nullptr || !engineFrameHoldFbo_->isValid() || width <= 0 ||
        height <= 0) {
        return false;
    }
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (ctx == nullptr) return false;
    QOpenGLExtraFunctions* gl = ctx->extraFunctions();
    if (gl == nullptr) return false;
    const FboBindings bindings = captureFboBindings(gl);
    // A filtered blit between fixed-point and floating-point buffers is not
    // defined by GL, so the two formats are compared instead of assumed equal.
    const GLenum filter =
        static_cast<int>(engineFrameHoldFbo_->format().internalTextureFormat()) ==
                outputInternalFormat_
            ? GL_LINEAR
            : GL_NEAREST;
    bindFboRead(gl, engineFrameHoldFbo_->handle());
    bindFboDraw(gl, target);
    gl->glBlitFramebuffer(0, 0, engineFrameHoldFbo_->width(), engineFrameHoldFbo_->height(), 0, 0,
                          width, height, GL_COLOR_BUFFER_BIT, filter);
    restoreFboBindings(gl, bindings);
    return true;
}

bool VideoRenderBridge::drawRetainedPicture(unsigned int target, int width, int height) {
    if (drawEngineFrameHold(target, width, height)) return true;
    if (teardownPicture_.isNull()) return false;
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (ctx == nullptr) return false;
    QOpenGLFunctions* gl = ctx->functions();
    GLint previousViewport[4] = {0, 0, 0, 0};
    gl->glGetIntegerv(GL_VIEWPORT, previousViewport);
    const FboBindings bindings = captureFboBindings(gl);
    bindFboDraw(gl, target);
    gl->glViewport(0, 0, width, height);
    const bool painted = paintTeardownPicture();
    gl->glViewport(previousViewport[0], previousViewport[1], previousViewport[2],
                   previousViewport[3]);
    restoreFboBindings(gl, bindings);
    return painted;
}

bool VideoRenderBridge::paintTeardownPicture() {
    if (teardownPicture_.isNull()) return false;
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (ctx == nullptr) return false;
    QOpenGLExtraFunctions* gl = ctx->extraFunctions();
    if (gl == nullptr) return false;
    const QImage& picture = teardownPicture_;
    // The upload target is allocated for the layout the QImage actually stores,
    // not for the format the picture was read from: a 10:10:10:2 attachment was
    // read back widened to 16 bits per channel, and uploading 16-bit words into
    // a packed 10-bit texture would be a type/size mismatch.
    GLenum uploadType = kGlUnsignedByte;
    int uploadInternalFormat = kGlInternalFormatRgba8;
    switch (picture.format()) {
        case QImage::Format_RGBA64:
            uploadType = kGlUnsignedShort;
            uploadInternalFormat = kGlInternalFormatRgba16;
            break;
        case QImage::Format_RGBA16FPx4:
            uploadType = kGlHalfFloat;
            uploadInternalFormat = kGlInternalFormatRgba16F;
            break;
        default:
            break;
    }
    // One upload target per fallback paint: this is the reload window in which a
    // lost context's last picture stands in, never normal playback.
    QOpenGLFramebufferObjectFormat fboFormat;
    fboFormat.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    fboFormat.setInternalTextureFormat(uploadInternalFormat);
    QOpenGLFramebufferObject upload(picture.size(), fboFormat);
    if (!upload.isValid() || upload.texture() == 0) return false;
    GLint previousTexture = 0;
    GLint previousUnpack = 4;
    gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    gl->glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpack);
    gl->glBindTexture(GL_TEXTURE_2D, upload.texture());
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, picture.width(), picture.height(), kGlRgba,
                        uploadType, picture.constBits());
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, static_cast<GLint>(previousUnpack));
    gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));

    GLint viewport[4] = {0, 0, 0, 0};
    gl->glGetIntegerv(GL_VIEWPORT, viewport);
    const int targetWidth = viewport[2] > 0 ? viewport[2] : picture.width();
    const int targetHeight = viewport[3] > 0 ? viewport[3] : picture.height();
    // The readback is the finished frame, letterbox bars included, so it is
    // fitted by its own aspect rather than stretched over the whole target.
    const double scale = std::min(static_cast<double>(targetWidth) / picture.width(),
                                  static_cast<double>(targetHeight) / picture.height());
    if (!(scale > 0.0)) return false;
    const int fittedWidth = std::max(1, static_cast<int>(std::lround(picture.width() * scale)));
    const int fittedHeight = std::max(1, static_cast<int>(std::lround(picture.height() * scale)));
    const int offsetX = viewport[0] + (targetWidth - fittedWidth) / 2;
    const int offsetY = viewport[1] + (targetHeight - fittedHeight) / 2;

    const GLenum filter =
        uploadInternalFormat == outputInternalFormat_ ? GL_LINEAR : GL_NEAREST;
    const FboBindings bindings = captureFboBindings(gl);
    bindFboRead(gl, upload.handle());
    // The QImage was flipped to top-first, so the source rows are sampled
    // bottom-up here: the destination rect is flipped rather than the pixels.
    gl->glBlitFramebuffer(0, 0, picture.width(), picture.height(), offsetX, offsetY + fittedHeight,
                          offsetX + fittedWidth, offsetY, GL_COLOR_BUFFER_BIT, filter);
    restoreFboBindings(gl, bindings);
    return true;
}

// ── target format ────────────────────────────────────────────────────────

void VideoRenderBridge::probeTargetFormat(int targetFbo) {
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (ctx == nullptr) return;
    QOpenGLExtraFunctions* gl = ctx->extraFunctions();
    if (gl == nullptr) return;
    // Probe the framebuffer mpv will actually draw into, not whichever one a
    // caller happened to leave bound.
    gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(targetFbo));
    renderFormatProbed_ = true;
    mikai::gui::uiprofile::noteRenderTargetProbe();
    if (ctx->isOpenGLES()) {
        // ES does not answer component-size or sized-format attachment queries
        // for color attachments: asking is INVALID_ENUM. mpv gets its documented
        // 8-bit default rather than a guessed precision.
        renderInternalFormat_ = 0;
        renderDepthBitsPerChannel_ = 0;
        renderTargetColorBits_ = RenderTargetColorBits();
        if (!renderFrameTimingLogPath().isEmpty())
            logFrameTiming(QStringLiteral("fbo probe skipped (OpenGL ES context)"));
        return;
    }
    if (targetFbo == 0) {
        // The default framebuffer has no color attachment points at all, so the
        // attachment queries below are invalid there (drivers answer
        // INVALID_ENUM / INVALID_OPERATION, and the error then surfaces from the
        // next GL user). The surface format is the only truth available; the
        // sized internal format stays unknown rather than guessed, so mpv keeps
        // its documented 8-bit default.
        const QSurfaceFormat surface = ctx->format();
        renderTargetColorBits_ = RenderTargetColorBits{surface.redBufferSize(),
                                                       surface.greenBufferSize(),
                                                       surface.blueBufferSize()};
        renderInternalFormat_ = 0;
        renderDepthBitsPerChannel_ =
            (renderTargetColorBits_.red > 0 &&
             renderTargetColorBits_.red == renderTargetColorBits_.green &&
             renderTargetColorBits_.red == renderTargetColorBits_.blue)
                ? renderTargetColorBits_.red
                : 0;
        if (!renderFrameTimingLogPath().isEmpty())
            logFrameTiming(QStringLiteral("fbo probe: default framebuffer, surface format only"));
        return;
    }
    GLint objectType = 0;
    GLint objectName = 0;
    GLint internalFormat = 0;
    gl->glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                              kGlColorAttachmentObjectType, &objectType);
    gl->glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                              kGlColorAttachmentObjectName, &objectName);
    gl->glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, kGlRedSize,
                                              &renderTargetColorBits_.red);
    gl->glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, kGlGreenSize,
                                              &renderTargetColorBits_.green);
    gl->glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, kGlBlueSize,
                                              &renderTargetColorBits_.blue);
    if (objectType == static_cast<GLint>(kGlTextureObject)) {
        GLint previousTexture = 0;
        gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(objectName));
        gl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, kGlTextureInternalFormat, &internalFormat);
        gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    } else if (objectType == static_cast<GLint>(kGlRenderbufferObject)) {
        GLint previousRenderbuffer = 0;
        gl->glGetIntegerv(0x8CA7, &previousRenderbuffer);
        gl->glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(objectName));
        gl->glGetRenderbufferParameteriv(GL_RENDERBUFFER, kGlRenderbufferInternalFormat,
                                         &internalFormat);
        gl->glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previousRenderbuffer));
    }
    if (renderTargetColorBits_.red > 0 &&
        renderTargetColorBits_.red == renderTargetColorBits_.green &&
        renderTargetColorBits_.red == renderTargetColorBits_.blue) {
        renderDepthBitsPerChannel_ = renderTargetColorBits_.red;
    } else {
        renderDepthBitsPerChannel_ = 0;
    }
    renderInternalFormat_ = supportedSizedFormat(internalFormat) ? internalFormat : 0;
    if (!renderFrameTimingLogPath().isEmpty()) {
        logFrameTiming(QStringLiteral("fbo red_bits=%1 green_bits=%2 blue_bits=%3 depth_bits=%4 "
                                      "internal_format=%5")
                           .arg(renderTargetColorBits_.red)
                           .arg(renderTargetColorBits_.green)
                           .arg(renderTargetColorBits_.blue)
                           .arg(renderDepthBitsPerChannel_)
                           .arg(internalFormat, 0, 16));
    }
}

void VideoRenderBridge::resetTargetFormat() {
    // Never called per paint or per resize: these queries cost a round trip and
    // their answer describes one attachment, so a new context or a released one
    // makes it unknown again rather than merely stale.
    mikai::gui::uiprofile::noteRenderTargetFormatReset();
    renderFormatProbed_ = false;
    renderInternalFormat_ = 0;
    renderDepthBitsPerChannel_ = 0;
    renderTargetColorBits_ = RenderTargetColorBits();
}

// ── readback ─────────────────────────────────────────────────────────────

void VideoRenderBridge::captureDiagnosticsAsync(
    const RenderDiagnosticsRequest& request, const RenderSnapshot& snapshot,
    std::function<void(RenderDiagnosticsResult)> completion) {
    if (!completion) return;
    postDiagnosticsResult(captureDiagnostics(request, snapshot), std::move(completion));
}

void VideoRenderBridge::postDiagnosticsResult(
    RenderDiagnosticsResult result,
    std::function<void(RenderDiagnosticsResult)> completion) {
    if (!completion) return;
    if (toGuiThread_) {
        toGuiThread_([completion = std::move(completion), result = std::move(result)]() mutable {
            completion(std::move(result));
        });
        return;
    }
    completion(std::move(result));
}

RenderDiagnosticsResult VideoRenderBridge::captureDiagnostics(
    const RenderDiagnosticsRequest& request, const RenderSnapshot& snapshot) {
    RenderDiagnosticsResult result;
    result.contextGeneration = generation_.load();
    result.sourceEpoch = snapshot.sourceEpoch;
    result.position = snapshot.position;
    result.outputSize = outputSize_;

    if (request.contextGeneration != result.contextGeneration ||
        request.coreIdentity != reinterpret_cast<quintptr>(core_.get()) ||
        request.sourceEpoch != snapshot.sourceEpoch ||
        !snapshot.positionValid || !std::isfinite(snapshot.position) ||
        std::abs(snapshot.position - request.position) > 0.01) {
        result.error = QStringLiteral("diagnostic request no longer matches the active context, source, or position");
        return result;
    }
    if (!contextReady_.load() || !render_ || !outputFbo_ || !outputFbo_->isValid()) {
        result.error = QStringLiteral("diagnostic capture has no live render output");
        return result;
    }
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (!context) {
        result.error = QStringLiteral("diagnostic capture has no owning OpenGL context current");
        return result;
    }
    QOpenGLExtraFunctions* gl = context->extraFunctions();
    if (!gl) {
        result.error = QStringLiteral("diagnostic capture has no OpenGL functions");
        return result;
    }
    GLint framebufferPackBuffer = 0;
    GLint framebufferPackAlignment = 4;
    GLint framebufferPackRowLength = 0;
    GLint framebufferPackSkipRows = 0;
    GLint framebufferPackSkipPixels = 0;
    gl->glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &framebufferPackBuffer);
    gl->glGetIntegerv(GL_PACK_ALIGNMENT, &framebufferPackAlignment);
    gl->glGetIntegerv(GL_PACK_ROW_LENGTH, &framebufferPackRowLength);
    gl->glGetIntegerv(GL_PACK_SKIP_ROWS, &framebufferPackSkipRows);
    gl->glGetIntegerv(GL_PACK_SKIP_PIXELS, &framebufferPackSkipPixels);
    if (framebufferPackBuffer != 0) {
        result.error = QStringLiteral("pixel-pack buffer %1 is bound").arg(framebufferPackBuffer);
        return result;
    }
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl->glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    gl->glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    result.framebuffer = readVideoFrame(&result.error);
    gl->glPixelStorei(GL_PACK_ALIGNMENT, framebufferPackAlignment);
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, framebufferPackRowLength);
    gl->glPixelStorei(GL_PACK_SKIP_ROWS, framebufferPackSkipRows);
    gl->glPixelStorei(GL_PACK_SKIP_PIXELS, framebufferPackSkipPixels);
    const QString framebufferRestoreError = gl->glGetError() == GL_NO_ERROR
                                                ? QString()
                                                : QStringLiteral("OpenGL error restoring framebuffer pack state");
    if (result.error.isEmpty()) result.error = framebufferRestoreError;
    if (result.framebuffer.isNull() || !result.error.isEmpty()) {
        if (result.error.isEmpty()) result.error = QStringLiteral("video framebuffer capture failed");
        return result;
    }
    auto& state = result.glState;
    state.stencilTest = gl->glIsEnabled(GL_STENCIL_TEST);
    state.depthTest = gl->glIsEnabled(GL_DEPTH_TEST);
    state.blend = gl->glIsEnabled(GL_BLEND);
    state.scissorTest = gl->glIsEnabled(GL_SCISSOR_TEST);
    state.dither = gl->glIsEnabled(GL_DITHER);
    GLboolean colorMask[4]{};
    gl->glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    for (int channel = 0; channel < 4; ++channel)
        state.colorWriteMask[static_cast<size_t>(channel)] = colorMask[channel] == GL_TRUE;
    gl->glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &state.pixelUnpackBufferBinding);
    gl->glGetIntegerv(GL_UNPACK_ALIGNMENT, &state.unpackAlignment);
    gl->glGetIntegerv(GL_UNPACK_ROW_LENGTH, &state.unpackRowLength);
    const GLubyte* vendor = gl->glGetString(GL_VENDOR);
    const GLubyte* renderer = gl->glGetString(GL_RENDERER);
    if (vendor)
        result.glVendor = QString::fromLatin1(reinterpret_cast<const char*>(vendor));
    if (renderer)
        result.glRenderer = QString::fromLatin1(reinterpret_cast<const char*>(renderer));
    const auto glError = [&]() {
        const GLenum error = gl->glGetError();
        return error == GL_NO_ERROR ? QString() : QStringLiteral("GL error 0x%1").arg(error, 0, 16);
    };
    const QString stateError = glError();
    if (!stateError.isEmpty()) {
        result.error = stateError;
        return result;
    }
    if (!vendor || !renderer) {
        result.error = QStringLiteral("OpenGL vendor or renderer identity is unavailable");
        return result;
    }
    if (!request.captureGlTextures) {
        result.success = true;
        return result;
    }

#ifdef Q_OS_WIN
    using GetLevel = void(__stdcall*)(GLuint, GLint, GLenum, GLint*);
    using GetImage = void(__stdcall*)(GLuint, GLint, GLenum, GLenum, GLsizei, void*);
#else
    using GetLevel = void(*)(GLuint, GLint, GLenum, GLint*);
    using GetImage = void(*)(GLuint, GLint, GLenum, GLenum, GLsizei, void*);
#endif
    const auto getLevel = reinterpret_cast<GetLevel>(
        context->getProcAddress("glGetTextureLevelParameteriv"));
    const auto getImage = reinterpret_cast<GetImage>(context->getProcAddress("glGetTextureImage"));
    if (!getLevel || !getImage) {
        result.error = QStringLiteral("OpenGL 4.5 texture readback unavailable");
        return result;
    }

    GLint previousPackBuffer = 0;
    GLint previousPackAlignment = 4;
    GLint previousPackRowLength = 0;
    GLint previousPackSkipRows = 0;
    GLint previousPackSkipPixels = 0;
    gl->glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPackBuffer);
    gl->glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
    gl->glGetIntegerv(GL_PACK_ROW_LENGTH, &previousPackRowLength);
    gl->glGetIntegerv(GL_PACK_SKIP_ROWS, &previousPackSkipRows);
    gl->glGetIntegerv(GL_PACK_SKIP_PIXELS, &previousPackSkipPixels);
    const QString packStateError = glError();
    if (!packStateError.isEmpty()) {
        result.error = packStateError;
        return result;
    }
    if (previousPackBuffer != 0) {
        result.error = QStringLiteral("pixel-pack buffer %1 is bound").arg(previousPackBuffer);
        return result;
    }

    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl->glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    gl->glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    int liveTextures = 0;
    int lumaCount = 0;
    int chromaCount = 0;
    bool finiteActivePlanes = true;
    QString captureError;
    for (GLuint name = 1; name <= 512; ++name) {
        if (!gl->glIsTexture(name)) continue;
        ++liveTextures;
        GLint width = 0, height = 0, depth = 0, internalFormat = 0;
        getLevel(name, 0, GL_TEXTURE_WIDTH, &width);
        getLevel(name, 0, GL_TEXTURE_HEIGHT, &height);
        getLevel(name, 0, GL_TEXTURE_DEPTH, &depth);
        getLevel(name, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
        captureError = glError();
        if (!captureError.isEmpty()) {
            captureError = QStringLiteral("texture %1 query: %2").arg(name).arg(captureError);
            break;
        }
        const bool luma = internalFormat == GL_R8 && width == outputSize_.width() &&
                          height == outputSize_.height();
        const bool chroma = internalFormat == GL_RG8 &&
                            width == (outputSize_.width() + 1) / 2 &&
                            height == (outputSize_.height() + 1) / 2;
        const bool color = (internalFormat == GL_RGBA8 || internalFormat == GL_RGBA16F ||
                            internalFormat == GL_RGBA32F || internalFormat == GL_RGB10_A2) &&
                           width > 0 && height > 0;
        if ((!luma && !chroma && !color) || depth > 1) continue;
        const GLenum externalFormat = color ? GL_RGBA : luma ? GL_RED : GL_RG;
        const GLenum externalType = color ? GL_FLOAT : GL_UNSIGNED_BYTE;
        const qint64 bytesPerPixel = color ? 4 * qint64(sizeof(GLfloat)) : luma ? 1 : 2;
        const qint64 byteCount = qint64(width) * height * bytesPerPixel;
        if (width <= 0 || height <= 0 || byteCount <= 0 ||
            byteCount > std::numeric_limits<GLsizei>::max()) {
            captureError = QStringLiteral("texture %1 capture size out of range").arg(name);
            break;
        }
        RenderDiagnosticsTexture texture;
        texture.plane = color ? RenderDiagnosticsTexture::Plane::Color
                              : luma ? RenderDiagnosticsTexture::Plane::Luma
                                     : RenderDiagnosticsTexture::Plane::Chroma;
        texture.textureName = name;
        texture.width = width;
        texture.height = height;
        texture.internalFormat = internalFormat;
        texture.externalFormat = static_cast<int>(externalFormat);
        texture.externalType = static_cast<int>(externalType);
        texture.bytes.resize(static_cast<qsizetype>(byteCount));
        getImage(name, 0, externalFormat, externalType, static_cast<GLsizei>(byteCount),
                 texture.bytes.data());
        captureError = glError();
        if (!captureError.isEmpty()) {
            captureError = QStringLiteral("texture %1 readback: %2").arg(name).arg(captureError);
            break;
        }
        if (color) {
            texture.preview = QImage(width, height, QImage::Format_RGB888);
            if (texture.preview.isNull()) {
                captureError = QStringLiteral("cannot allocate color texture preview");
                break;
            }
            const auto* values = reinterpret_cast<const GLfloat*>(texture.bytes.constData());
            for (int y = 0; y < height; ++y) {
                uchar* row = texture.preview.scanLine(y);
                for (int x = 0; x < width; ++x) {
                    const qsizetype base = (qsizetype(y) * width + x) * 4;
                    for (int channel = 0; channel < 4; ++channel) {
                        const GLfloat value = values[base + channel];
                        if (!std::isfinite(value)) ++texture.nonFiniteCounts[static_cast<size_t>(channel)];
                        if (channel < 3) {
                            row[x * 3 + channel] =
                                std::isfinite(value)
                                    ? static_cast<uchar>(std::clamp(value, 0.0f, 1.0f) * 255.0f)
                                    : 0;
                        }
                    }
                }
            }
            if (height == outputSize_.height() &&
                (width == outputSize_.width() || width == (outputSize_.width() + 1) / 2)) {
                finiteActivePlanes =
                    finiteActivePlanes &&
                    std::all_of(texture.nonFiniteCounts.begin(), texture.nonFiniteCounts.end(),
                                [](quint64 count) { return count == 0; });
            }
        } else if (luma) {
            texture.preview = QImage(reinterpret_cast<const uchar*>(texture.bytes.constData()),
                                      width, height, width, QImage::Format_Grayscale8).copy();
            ++lumaCount;
        } else {
            ++chromaCount;
        }
        result.textures.push_back(std::move(texture));
    }
    result.liveTextureCount = liveTextures;
    gl->glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, previousPackRowLength);
    gl->glPixelStorei(GL_PACK_SKIP_ROWS, previousPackSkipRows);
    gl->glPixelStorei(GL_PACK_SKIP_PIXELS, previousPackSkipPixels);
    const QString restoreError = glError();
    if (captureError.isEmpty() && !restoreError.isEmpty()) captureError = restoreError;
    if (captureError.isEmpty() && (lumaCount != 1 || chromaCount != 1))
        captureError = QStringLiteral("no unambiguous NV12 texture pair (live=%1, luma=%2, chroma=%3)")
                           .arg(liveTextures).arg(lumaCount).arg(chromaCount);
    if (captureError.isEmpty() && !finiteActivePlanes)
        captureError = QStringLiteral("non-finite values in active output-sized color planes");
    if (!captureError.isEmpty()) {
        result.error = captureError;
        return result;
    }
    result.success = true;
    return result;
}

QImage VideoRenderBridge::readVideoFrame(QString* reason) {
    if (reason != nullptr) reason->clear();
    if (outputFbo_ == nullptr || !outputFbo_->isValid() || outputSize_.isEmpty()) {
        if (reason != nullptr) *reason = QStringLiteral("no rendered output is available");
        return QImage();
    }
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (ctx == nullptr) {
        if (reason != nullptr) *reason = QStringLiteral("no OpenGL context is current");
        return QImage();
    }
    QOpenGLExtraFunctions* gl = ctx->extraFunctions();
    if (gl == nullptr) {
        if (reason != nullptr) *reason = QStringLiteral("the OpenGL context has no functions");
        return QImage();
    }
    const QImage picture = readFramebufferPixels(gl, outputFbo_->handle(), outputFbo_->size(),
                                                 outputInternalFormat_);
    if (picture.isNull() && reason != nullptr)
        *reason = QStringLiteral("the output framebuffer could not be read");
    return picture;
}

void VideoRenderBridge::readVideoFrameAsync(std::function<void(QImage, QString)> completion) {
    if (!completion) return;
    if (pendingReadback_ != nullptr) {
        // One readback is an API level, not a queue: the second caller is told so
        // instead of silently replacing the first completion.
        std::function<void(QImage, QString)> rejected = std::move(completion);
        const QString reason = QStringLiteral("a video readback is already in flight");
        if (toGuiThread_) {
            toGuiThread_([rejected, reason] { rejected(QImage(), reason); });
        } else {
            rejected(QImage(), reason);
        }
        return;
    }
    pendingReadback_ = std::move(completion);
}

void VideoRenderBridge::deliverReadback(const QImage& image, const QString& reason) {
    if (pendingReadback_ == nullptr) return;
    std::function<void(QImage, QString)> completion = std::move(pendingReadback_);
    pendingReadback_ = nullptr;
    if (toGuiThread_) {
        // The GUI thread owns the session and the completion callback.
        toGuiThread_([completion, image, reason] { completion(image, reason); });
        return;
    }
    completion(image, reason);
}

// ── profiling ────────────────────────────────────────────────────────────

void VideoRenderBridge::publishProfilePaint(const RenderResult& result, qint64 paintStartNs,
                                            qint64 engineMicros, qint64 retentionMicros) {
    profileLastPaintEntryNs_ = paintStartNs;
    if (!mikai::gui::uiprofile::countersEnabled()) return;
    int pass = mikai::gui::uiprofile::kPaintChromeOnly;
    switch (result.pass) {
        case RenderResult::Pass::HeldPicture:
            pass = mikai::gui::uiprofile::kPaintRetained;
            break;
        case RenderResult::Pass::VideoFrame:
            pass = result.freshOutputFrame ? mikai::gui::uiprofile::kPaintVideoFrame
                                           : mikai::gui::uiprofile::kPaintChromeOnly;
            break;
        default:
            break;
    }
    const qint64 totalMicros = (mikai::gui::uiprofile::nowNs() - paintStartNs) / 1000;
    mikai::gui::uiprofile::recordPaintPass(pass, totalMicros);
    // The overlay belongs to the adapter chrome, so its cost lands in the margin
    // of this split rather than as a phase the bridge did not measure.
    mikai::gui::uiprofile::notePaintPhase(pass, 0, engineMicros,
                                          totalMicros - engineMicros - retentionMicros);
    mikai::gui::uiprofile::notePaintPhase(pass, 1, retentionMicros, 0);
}

void VideoRenderBridge::logFrameTiming(const QString& line) {
    appendRenderFrameTimingLog(line);
}

UiProfileCounters VideoRenderBridge::uiProfileCounters() const {
    UiProfileCounters counters;
    counters.initializeGlCalls = profileInitializeGlCalls_.load();
    counters.contextGeneration = generation_.load();
    counters.paintCalls = profilePaintCalls_.load();
    counters.pacingDisarms = profilePacingDisarms_.load();
    counters.lastPaintDelayMs = profileLastPaintDelayMs_.load();
    counters.lastPaintEntryNs = profileLastPaintEntryNs_.load();
    return counters;
}

}  // namespace mplayer
