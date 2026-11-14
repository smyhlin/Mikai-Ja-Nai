#include "VideoSurface.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHideEvent>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QSurface>
#include <QThread>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <utility>

#include "MpvApi.hpp"
#include "MpvHandle.hpp"

namespace mplayer {
namespace {

// A defined backdrop for a surface with nothing to show. Deliberately the same
// tone the opening placeholder uses, because both say "this is a stage, not a
// frame" rather than the bare black a failed clear leaves behind.
const QColor& videoBackdrop() {
    static const QColor colour(9, 10, 14);
    return colour;
}

/// Antialiased alpha coverage of one rounded corner, rasterised at `size` device
/// pixels: opaque inside the quarter disc, transparent outside it.
QImage cornerCoveragePatch(int size, qreal radiusDevice) {
    QImage patch(size, size, QImage::Format_ARGB32_Premultiplied);
    if (patch.isNull()) return patch;
    patch.fill(Qt::transparent);
    QPainter painter(&patch);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    QPainterPath shape;
    shape.addRoundedRect(QRectF(0.0, 0.0, radiusDevice * 2.0, radiusDevice * 2.0), radiusDevice,
                         radiusDevice);
    painter.drawPath(shape);
    return patch;
}

}  // namespace

VideoSurface::VideoSurface(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumSize(160, 90);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    // Without this the widget is composited as a separate texture and the
    // picture would be blended against a stale backing store - and every pass
    // below is written to define every pixel of its target.
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);

    // The bridge must deliver a queued readback on this object's thread. The
    // rendering thread here IS the GUI thread, but a completion raised inside a
    // render pass may run nested GUI work, so it is posted like any other event.
    presenterToken_ = std::make_shared<const int>(0);
    bridge_ = std::make_unique<VideoRenderBridge>([this](std::function<void()> task) {
        QMetaObject::invokeMethod(this, std::move(task), Qt::QueuedConnection);
    });

    // One non-visual session owns the core, source, transport intent and every
    // observed value; this view owns the chrome and the render context.
    session_ = new VideoSession(this);
    session_->setPresenter(this);

    connect(session_, &VideoSession::openingChanged, this, [this](bool) {
        updatePlaceholderAnimation();
    });

    // The swap report mpv asks for names the moment its frame was presented, and
    // that is what Qt reports here, after the composition it belongs to.
    connect(this, &QOpenGLWidget::frameSwapped, this, [this] { onFrameSwapped(); });

    // One nonblocking timer defers a pending frame to mpv's presentation slot.
    pacingTimer_.setSingleShot(true);
    pacingTimer_.setTimerType(Qt::PreciseTimer);
    connect(&pacingTimer_, &QTimer::timeout, this, [this] { onPacingTick(); });

    // A placeholder that never moves is a frozen frame to a viewer. It only runs
    // while a source is opening and no picture has been presented.
    placeholderTimer_.setInterval(90);
    connect(&placeholderTimer_, &QTimer::timeout, this, [this] {
        ++placeholderPhase_;
        update();
    });
}

VideoSurface::~VideoSurface() {
    destroying_ = true;
    pacingTimer_.stop();
    placeholderTimer_.stop();
    // The hooks belong to the session; clearing them first means no notification
    // below can reach a half-destroyed view.
    if (session_ != nullptr && session_->presenter() == this) {
        session_->setPresenter(nullptr);
    }
    if (contextDestroyedConnection_) {
        disconnect(contextDestroyedConnection_);
        contextDestroyedConnection_ = QMetaObject::Connection();
    }
    if (bridge_ != nullptr) {
        bridge_->setDestroying(true);
        QOpenGLContext* previous = nullptr;
        const bool current = context_ != nullptr && ensureContextCurrent(&previous);
        bridge_->releaseRenderContext();
        if (current) releaseContextCurrent(previous);
        // The bridge's destructor is the last chance: it frees the render
        // context before dropping the core lease, and retains both when no
        // creating context can be current (a real error, logged critically).
        bridge_.reset();
    }
    context_ = nullptr;
    // session_ is a child QObject and is destroyed after this body, which is the
    // required order: the render context was freed above, then the core goes.
}

// ── GL lifecycle ──────────────────────────────────────────────────────────

void VideoSurface::initializeGL() {
    QOpenGLWidget::initializeGL();
    context_ = QOpenGLContext::currentContext();
    if (contextDestroyedConnection_) {
        disconnect(contextDestroyedConnection_);
        contextDestroyedConnection_ = QMetaObject::Connection();
    }
    if (context_ != nullptr) {
        // Direct connection: the bridge needs THIS context current for its
        // teardown readback and for freeing mpv's render context.
        contextDestroyedConnection_ = connect(
            context_, &QOpenGLContext::aboutToBeDestroyed, this,
            [this] { handleContextAboutToBeDestroyed(); }, Qt::DirectConnection);
    }
    paintedOnce_ = false;
    refreshRequested_ = true;
    if (session_ != nullptr) {
        const std::shared_ptr<MpvHandle> core = session_->coreLease();
        if (core) {
            // A recreated context attaches as a new video output while the core
            // keeps the file loaded; the session decides whether the live core
            // still delivers output or the source has to be handed to it again.
            QString reason;
            if (!ensureRenderContext(core, &reason) && !reason.isEmpty()) {
                qWarning("VideoSurface: render context not created: %s",
                         qUtf8Printable(reason));
            }
        }
    }
    notifyTargetState();
    update();
}

void VideoSurface::resizeGL(int width, int height) {
    QOpenGLWidget::resizeGL(width, height);
    // A resize hands the widget a freshly allocated framebuffer whose contents
    // are undefined: the next pass must render (never blit a picture that no
    // longer applies), even with a paused core. The attachment FORMAT is not
    // re-probed - it belongs to the context and the surface format, not the
    // size - so the probe is reset only when a context is created or released.
    paintedOnce_ = false;
    refreshRequested_ = true;
    notifyTargetState();
    update();
}

void VideoSurface::showEvent(QShowEvent* event) {
    QOpenGLWidget::showEvent(event);
    // Same reasoning as resizeGL: a surface that was hidden cannot assume its
    // framebuffer still holds the last frame, and with a paused core there may
    // never be another one.
    paintedOnce_ = false;
    refreshRequested_ = true;
    notifyTargetState();
    update();
}

void VideoSurface::hideEvent(QHideEvent* event) {
    QOpenGLWidget::hideEvent(event);
    // Composition stops with the widget, so the frame it painted will never be
    // presented: the owed report goes out as the drop it is instead of leaving
    // mpv's video output to time out on it.
    pacingTimer_.stop();
    if (bridge_ != nullptr) {
        QOpenGLContext* previous = nullptr;
        if (ensureContextCurrent(&previous)) {
            bridge_->acknowledgeDroppedFrame();
            releaseContextCurrent(previous);
        }
    }
    notifyTargetState();
}

void VideoSurface::paintGL() {
    QOpenGLWidget::paintGL();
    if (session_ == nullptr) return;

    // The pass state is captured here, on the session's own thread (which is
    // also this widget's rendering thread for the basic loop), and is the only
    // thing the renderer reads.
    snapshot_ = session_->renderSnapshot();
    const QSize deviceSize = QSize(width(), height()) * devicePixelRatioF();
    const int targetWidth = std::max(1, deviceSize.width());
    const int targetHeight = std::max(1, deviceSize.height());

    QOpenGLContext* glContext = QOpenGLContext::currentContext();
    if (glContext == nullptr || bridge_ == nullptr || !bridge_->contextReady()) {
        // No renderer: no frame is owed, but the target must still be fully
        // defined (NoPartialUpdate) and the chrome still has to be drawn.
        paintWithoutRenderer();
        return;
    }

    facts_.devicePixelSize = QSize(targetWidth, targetHeight);
    facts_.devicePixelRatio = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    facts_.presentable = canPresentNow();
    facts_.renderContextReady = true;

    QOpenGLFunctions* gl = glContext->functions();
    const GLuint target = static_cast<GLuint>(defaultFramebufferObject());

    QPainter painter(this);
    painter.beginNativePainting();

    // Everything raw stays inside this boundary: Qt owns the paint engine's
    // target and state outside it.
    gl->glBindFramebuffer(GL_FRAMEBUFFER, target);
    gl->glViewport(0, 0, targetWidth, targetHeight);
    // Qt enables blending before paintGL; libmpv requires its default disabled.
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_SCISSOR_TEST);

    // Query the widget color attachment once per context/target, with that
    // target actually bound, so the format handed to mpv is the one it draws
    // into rather than a previous framebuffer's.
    if (!bridge_->targetFormatProbed()) {
        bridge_->probeTargetFormat(static_cast<int>(target));
        notifyTargetState();
    }
    facts_.colorInternalFormat = bridge_->targetInternalFormat();
    facts_.colorBits = bridge_->targetColorBits();
    facts_.depthBitsPerChannel = bridge_->targetDepthBitsPerChannel();

    // ONE stable overlay-free output: mpv renders into it, and this pass blits
    // it into the widget's target. Nothing is copied per frame.
    bridge_->ensureOutput(facts_);
    if (refreshRequested_) {
        // The chain changed under a possibly paused core: one forced advance,
        // then the pass below presents what it produced.
        refreshRequested_ = false;
        bridge_->refresh(snapshot_, facts_.presentable);
    }
    const RenderResult result =
        bridge_->render(snapshot_, static_cast<int>(target), targetWidth, targetHeight);

    if (QOpenGLExtraFunctions* extra = glContext->extraFunctions()) {
        extra->glBindVertexArray(0);
        extra->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        extra->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, target);
    painter.endNativePainting();

    paintedOnce_ = true;
    publishPresentation(result);
    reportRenderFailure(result.renderError);
    // This pass took the frame the pacing slot was holding, so the timer it
    // armed has nothing left to release.
    if (result.frameConsumed) pacingTimer_.stop();

    // No picture at all and a source is opening: the placeholder stands where
    // the first frame will be, over the renderer's defined black. A held or
    // read-back picture is a picture, so it is never covered by the wait state.
    const bool noPicture = result.pass == RenderResult::Pass::None ||
                           result.pass == RenderResult::Pass::Failed;
    if (noPicture && session_->isOpening() && !bridge_->hasRetainedPicture()) {
        paintOpeningPlaceholder(painter);
    }
    updatePlaceholderAnimation();

    emit paintOverlay(&painter);
    // Last, and over the overlay: the coverage is what the window composites.
    paintDisplayCornerCoverage(painter, targetWidth, targetHeight);
}

void VideoSurface::paintWithoutRenderer() {
    pacingTimer_.stop();
    QPainter painter(this);
    painter.fillRect(rect(), videoBackdrop());
    if (session_ != nullptr && session_->isOpening()) paintOpeningPlaceholder(painter);
    emit paintOverlay(&painter);
    const QSize deviceSize = QSize(width(), height()) * devicePixelRatioF();
    paintDisplayCornerCoverage(painter, std::max(1, deviceSize.width()),
                               std::max(1, deviceSize.height()));
}

void VideoSurface::publishPresentation(const RenderResult& result) {
    if (result.presentationSerial == 0) return;
    if (!framePresented) return;
    PresentationReport report;
    report.serial = result.presentationSerial;
    report.sourceEpoch = snapshot_.sourceEpoch;
    report.freshOutputFrame = result.freshOutputFrame;
    report.heldPicture = result.presentedHeldPicture;
    report.reusedSnapshot = result.reusedOutput;
    framePresented(report);
}

void VideoSurface::reportRenderFailure(int renderError) {
    if (renderError >= 0) {
        renderFailureStreak_ = false;
        return;
    }
    // One report per streak: a renderer that fails on every paint would
    // otherwise repeat the same message for every frame it was asked for.
    if (renderFailureStreak_) return;
    renderFailureStreak_ = true;
    if (!this->renderError) return;
    QString detail = QString::number(renderError);
    if (session_ != nullptr && session_->core() != nullptr) {
        detail = QString::fromStdString(session_->core()->describeError(renderError));
    }
    const QString message = tr("Video render failed (mpv: %1)").arg(detail);
    // Never from inside a paint: the session's handler emits the viewer-facing
    // signal, and its consumers may run arbitrary GUI work.
    QMetaObject::invokeMethod(
        this,
        [this, message] {
            if (this->renderError) this->renderError(message);
        },
        Qt::QueuedConnection);
}

// ── render wake, pacing and presentation ──────────────────────────────────

void VideoSurface::onRenderUpdate() {
    if (bridge_ == nullptr || !bridge_->contextReady()) return;
    if (session_ != nullptr) snapshot_ = session_->renderSnapshot();
    QOpenGLContext* previous = nullptr;
    if (!ensureContextCurrent(&previous)) {
        // The context is not available yet; a repaint is what recreates state,
        // and mpv will raise another callback for a frame that is still owed.
        update();
        return;
    }
    const bool presentable = canPresentNow();
    // Exactly one answer per mpv callback: a pending frame kept for the next
    // paint, or the skip acknowledged at once when nothing can composite.
    bridge_->advanceRenderState(snapshot_, presentable);
    int delayMs = bridge_->schedulePacedPaint(presentable, 0, 0, 0);
    releaseContextCurrent(previous);
    if (!clientFramePacingEnabled() && delayMs >= 0) delayMs = 0;
    if (delayMs > 0) {
        // A single-shot timer for the slot mpv named; the paint it releases is
        // the presentation of that frame.
        pacingTimer_.start(delayMs);
        return;
    }
    if (delayMs == 0) update();
    // -1: nothing is owed (a skipped frame, or no frame at all). Any earlier,
    // earlier-target slot stays armed exactly as the bridge left it.
}

void VideoSurface::onPacingTick() {
    if (bridge_ == nullptr || !bridge_->contextReady()) return;
    if (!canPresentNow()) {
        // Qt would discard the update; advance to the skip/ack path instead of
        // stranding the frame endlessly.
        onRenderUpdate();
        return;
    }
    // This paint is the frame's presentation, not a repaint: it may consume the
    // frame and report its swap. Must be consumed before the paint runs.
    bridge_->notePacedPaintStarted();
    update();
}

void VideoSurface::onFrameSwapped() {
    if (bridge_ == nullptr || !bridge_->contextReady()) return;
    QOpenGLContext* previous = nullptr;
    if (!ensureContextCurrent(&previous)) return;
    // mpv reads the moment this arrives as the moment the frame reached the
    // screen, which is exactly what the signal it is sent from reports.
    bridge_->reportPresentedFrame();
    releaseContextCurrent(previous);
}

bool VideoSurface::clientFramePacingEnabled() {
    // The kill-switch is read once per process: it exists for a rollback and for
    // field diagnostics, both of which relaunch. Only an explicit "0" disables it.
    static const bool disabled =
        qEnvironmentVariable("MIKAI_CLIENT_FRAME_PACING") == QStringLiteral("0");
    return !disabled;
}

// ── VideoPresenter: target facts ──────────────────────────────────────────

RenderTargetFacts VideoSurface::targetFacts() const {
    RenderTargetFacts facts = facts_;
    const QSize deviceSize = QSize(width(), height()) * devicePixelRatioF();
    facts.devicePixelSize =
        QSize(std::max(1, deviceSize.width()), std::max(1, deviceSize.height()));
    facts.devicePixelRatio = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    facts.presentable = canPresentNow();
    if (bridge_ != nullptr) {
        facts.renderContextReady = bridge_->contextReady();
        facts.colorInternalFormat = bridge_->targetInternalFormat();
        facts.colorBits = bridge_->targetColorBits();
        facts.depthBitsPerChannel = bridge_->targetDepthBitsPerChannel();
    }
    return facts;
}

bool VideoSurface::canPresentNow() const {
    if (!isVisible()) return false;
    // An opaque sibling can suppress paints without hiding the widget or window.
    if (visibleRegion().isEmpty()) return false;
    const QWindow* handle = window() != nullptr ? window()->windowHandle() : nullptr;
    if (handle == nullptr) return false;
    if ((handle->windowState() & Qt::WindowMinimized) != 0) return false;
    // Once this surface has painted, an unexposed window is one Qt is not
    // compositing, so a frame kept for it would never be presented nor acked.
    // Before the first pass there is nothing to lose: the render context exists
    // because Qt was already painting this widget.
    return !paintedOnce_ || handle->isExposed();
}

void VideoSurface::requestRepaint() { update(); }

void VideoSurface::notifyTargetState() {
    if (!targetStateChanged) return;
    targetStateChanged(targetFacts());
}

// ── VideoPresenter: retained pictures ─────────────────────────────────────

void VideoSurface::acquireRetainedPicture() {
    if (bridge_ == nullptr) return;
    QOpenGLContext* previous = nullptr;
    if (!ensureContextCurrent(&previous)) return;
    // Idempotent: a picture already held is not recaptured.
    bridge_->setEngineFrameHold(true);
    releaseContextCurrent(previous);
    update();
}

void VideoSurface::releaseRetainedPicture() {
    if (bridge_ == nullptr) return;
    bridge_->setEngineFrameHold(false);
    update();
}

void VideoSurface::retainAcrossLoad() {
    if (bridge_ == nullptr) return;
    QOpenGLContext* previous = nullptr;
    const bool current = ensureContextCurrent(&previous);
    bridge_->retainAcrossLoad();
    if (current) releaseContextCurrent(previous);
    update();
}

void VideoSurface::armLoadLanding(uint64_t sourceEpoch) {
    if (bridge_ != nullptr) bridge_->armLoadLanding(sourceEpoch);
}

void VideoSurface::noteLoadLanded(uint64_t sourceEpoch) {
    if (bridge_ != nullptr) bridge_->noteLoadLanded(sourceEpoch);
}

void VideoSurface::noteLoadRefused(uint64_t sourceEpoch) {
    if (bridge_ != nullptr) bridge_->noteLoadRefused(sourceEpoch);
}

void VideoSurface::invalidateRetainedPictures() {
    if (bridge_ == nullptr) return;
    QOpenGLContext* previous = nullptr;
    const bool current = ensureContextCurrent(&previous);
    // Releases the GPU hold and the teardown readback. A source boundary, an
    // intentional clear and a close all land here.
    bridge_->invalidateRetainedPictures();
    if (current) releaseContextCurrent(previous);
    update();
}

void VideoSurface::refreshRenderState() {
    // The chain changed under a possibly paused core: the next pass forces one
    // render-state advance and repaints whatever it produced.
    refreshRequested_ = true;
    update();
}

// ── VideoPresenter: readback ──────────────────────────────────────────────

QImage VideoSurface::readVideoFrame(QString* reason) {
    if (bridge_ == nullptr || !bridge_->contextReady()) {
        if (reason != nullptr) *reason = tr("no render context");
        return QImage();
    }
    QOpenGLContext* previous = nullptr;
    if (!ensureContextCurrent(&previous)) {
        if (reason != nullptr) *reason = tr("the render context could not be made current");
        return QImage();
    }
    // The stable output holds video only; this never captures chrome.
    QImage image = bridge_->readVideoFrame(reason);
    releaseContextCurrent(previous);
    // A normal repaint is scheduled again before returning, but this call does
    // not itself composite the widget.
    update();
    return image;
}

void VideoSurface::readVideoFrameAsync(std::function<void(QImage, QString)> completion) {
    if (bridge_ == nullptr) {
        if (completion) completion(QImage(), tr("no renderer"));
        return;
    }
    bridge_->readVideoFrameAsync(std::move(completion));
    update();
}

// ── VideoPresenter: context lifecycle ─────────────────────────────────────

bool VideoSurface::ensureRenderContext(std::shared_ptr<MpvHandle> core, QString* reason) {
    if (bridge_ == nullptr) {
        // Not a viewer-facing failure: the session keeps its load deferred and
        // asks again once this view can present.
        return false;
    }
    if (bridge_->contextReady()) {
        // Idempotent: a ready context already knows about `core`.
        if (renderContextReady) renderContextReady(bridge_->contextGeneration());
        return true;
    }
    if (!core) return false;
    if (context_ == nullptr) {
        // A widget that has never initialized has no context yet; initializeGL
        // creates it, which is what drains the session's deferred load.
        return false;
    }
    QOpenGLContext* previous = nullptr;
    if (!ensureContextCurrent(&previous)) return false;
    bridge_->setDestroying(destroying_);
    QString why;
    bool created = false;
    try {
        // `core` is the session's shared lease: the bridge keeps it for as long
        // as a render context exists, so the core cannot die before the render
        // context is freed. mpv may invoke its update callback on a render
        // thread; all widget/context work must run on this object's GUI thread.
        created = bridge_->createRenderContext(
            core, presenterToken_,
            [this] {
                QMetaObject::invokeMethod(this, [this] { onRenderUpdate(); },
                                          Qt::QueuedConnection);
            },
            &why);
    } catch (const MpvError& error) {
        why = QString::fromUtf8(error.what());
        created = false;
    }
    releaseContextCurrent(previous);
    if (!created) {
        if (reason != nullptr) *reason = why;
        return false;
    }
    // A new context owns a new widget framebuffer whose attachment format says
    // nothing about the one the previous context probed; the bridge resets it.
    paintedOnce_ = false;
    refreshRequested_ = true;
    if (renderContextReady) renderContextReady(bridge_->contextGeneration());
    update();
    return true;
}

void VideoSurface::releaseRenderContext() {
    if (bridge_ == nullptr) return;
    // Captured before the render context goes: freeing it kills the old video
    // output, which then reports 0 for a file it no longer decodes. The session
    // uses this offset to resume an internal reload.
    if (session_ != nullptr && reloadPositionCaptured) {
        reloadPositionCaptured(session_->position());
    }
    const bool wasReady = bridge_->contextReady();
    QOpenGLContext* previous = nullptr;
    const bool current = context_ != nullptr && ensureContextCurrent(&previous);
    // Frees mpv's render context BEFORE dropping the core lease. A false return
    // means neither was released; the adapter must retry from its context
    // teardown, and the bridge already logged that critically.
    bridge_->releaseRenderContext();
    if (current) releaseContextCurrent(previous);
    if (!current && bridge_->releasePending()) {
        qCritical("VideoSurface: render context release could not run with its creating "
                  "OpenGL context current; the render wrapper and the core lease are "
                  "retained until the context teardown can free them");
    }
    facts_.colorInternalFormat = 0;
    facts_.colorBits = {};
    facts_.depthBitsPerChannel = 0;
    facts_.renderContextReady = false;
    paintedOnce_ = false;
    refreshRequested_ = false;
    pacingTimer_.stop();
    if (wasReady && renderContextLost) renderContextLost();
    notifyTargetState();
}

void VideoSurface::handleContextAboutToBeDestroyed() {
    // Direct connection from QOpenGLContext::aboutToBeDestroyed: the dying
    // context IS current here, which is exactly what the bridge needs to read
    // back its last picture and to free mpv's render context.
    if (bridge_ != nullptr) {
        if (session_ != nullptr && reloadPositionCaptured) {
            reloadPositionCaptured(session_->position());
        }
        bridge_->setDestroying(destroying_);
        const bool released = bridge_->contextAboutToBeDestroyed();
        if (!released) {
            qCritical("VideoSurface: the render context could not be freed while its OpenGL "
                      "context was being destroyed; keeping the render wrapper and core lease");
        }
        if (renderContextLost) renderContextLost();
    }
    // The connection belongs to the context being destroyed; do not disconnect
    // it from inside its own emission.
    contextDestroyedConnection_ = QMetaObject::Connection();
    context_ = nullptr;
    paintedOnce_ = false;
    refreshRequested_ = false;
    pacingTimer_.stop();
    facts_.colorInternalFormat = 0;
    facts_.colorBits = {};
    facts_.depthBitsPerChannel = 0;
    facts_.renderContextReady = false;
    notifyTargetState();
}

bool VideoSurface::waitForContext(int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (bridge_ != nullptr && !bridge_->contextReady() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(20);
    }
    return bridge_ != nullptr && bridge_->contextReady();
}

bool VideoSurface::ensureContextCurrent(QOpenGLContext** previous) {
    if (previous != nullptr) *previous = nullptr;
    if (context_ == nullptr) return false;
    QOpenGLContext* before = QOpenGLContext::currentContext();
    if (previous != nullptr) *previous = before;
    if (before == context_) return true;
    QOpenGLWidget::makeCurrent();
    if (QOpenGLContext::currentContext() != context_) {
        qWarning("VideoSurface: could not make the render context current");
        return false;
    }
    return true;
}

void VideoSurface::releaseContextCurrent(QOpenGLContext* previous) {
    // One that WAS current before is put back with its own surface:
    // frameSwapped can run while a different compositor context is current, and
    // Qt's composition goes on using that one after this returns.
    if (previous == context_) return;
    if (previous != nullptr) {
        if (QSurface* surface = previous->surface()) {
            previous->makeCurrent(surface);
            return;
        }
    }
    // Nothing to hand back, so the context this call made current is released
    // instead of being left behind.
    QOpenGLWidget::doneCurrent();
}

// ── widget chrome ─────────────────────────────────────────────────────────

void VideoSurface::setDisplayCornerRadius(int radius) {
    const int requested = std::max(0, radius);
    if (requested == displayCornerRadius_) return;
    displayCornerRadius_ = requested;
    // The patches describe the previous radius; the next pass rebuilds them. The
    // repaint is what makes the new corners appear without waiting for a frame.
    displayCornerTopLeft_ = QImage();
    displayCornerTopRight_ = QImage();
    displayCornerBottomLeft_ = QImage();
    displayCornerBottomRight_ = QImage();
    displayCornerRatio_ = 0.0;
    // A rounded picture carries alpha the window has to composite; the opaque
    // flag claims it never has any, so it stands only while the corners are
    // square.
    setAttribute(Qt::WA_OpaquePaintEvent, displayCornerRadius_ <= 0);
    update();
}

void VideoSurface::updatePlaceholderAnimation() {
    // The placeholder is painted exactly while a source is opening and no frame
    // of it has been presented yet.
    const bool needed = session_ != nullptr && session_->isOpening() &&
                        (bridge_ == nullptr ||
                         (!bridge_->hasVisibleOutputFrame() && !bridge_->hasRetainedPicture()));
    if (needed == placeholderTimer_.isActive()) return;
    if (needed) {
        placeholderPhase_ = 0;
        placeholderTimer_.start();
    } else {
        placeholderTimer_.stop();
    }
    update();
}

void VideoSurface::paintOpeningPlaceholder(QPainter& painter) {
    // A backdrop, not a frame: defined enough that the window cannot be mistaken
    // for a failed black rectangle.
    painter.fillRect(rect(), videoBackdrop());
    const QPointF centre(width() / 2.0, height() / 2.0);
    const double radius = std::max(10.0, std::min(width(), height()) * 0.035);
    paintOpeningRing(painter, centre, radius, std::max(2.0, radius * 0.18));
}

void VideoSurface::paintOpeningRing(QPainter& painter, const QPointF& centre, double radius,
                                    double penWidth) {
    // This target is single-sampled: a path drawn straight into it has no
    // coverage antialiasing, so the ring is rasterised into one reused buffer
    // and blitted 1:1.
    const qreal ratio = devicePixelRatioF();
    if (!(ratio > 0.0)) return;
    const int extent =
        std::max(2, static_cast<int>(std::ceil((radius + penWidth) * 2.0 * ratio)));
    if (openingRing_.size() != QSize(extent, extent)) {
        openingRing_ = QImage(extent, extent, QImage::Format_ARGB32_Premultiplied);
        openingRingPhase_ = -1;
    }
    if (openingRing_.isNull()) return;
    const int originX = static_cast<int>(std::lround(centre.x() * ratio)) - extent / 2;
    const int originY = static_cast<int>(std::lround(centre.y() * ratio)) - extent / 2;
    if (openingRingPhase_ != placeholderPhase_ || openingRingRadius_ != radius ||
        openingRingRatio_ != ratio) {
        openingRing_.fill(Qt::transparent);
        QPainter ring(&openingRing_);
        ring.setRenderHint(QPainter::Antialiasing, true);
        const QRectF deviceRing(extent / 2.0 - radius * ratio, extent / 2.0 - radius * ratio,
                                radius * 2.0 * ratio, radius * 2.0 * ratio);
        QPen pen(QColor(255, 255, 255, 45));
        pen.setWidthF(penWidth * ratio);
        ring.setPen(pen);
        ring.setBrush(Qt::NoBrush);
        ring.drawEllipse(deviceRing);
        // One sweeping arc, advanced by the timer: an indeterminate wait says
        // "working", never a percentage this surface does not know.
        QPen sweep(QColor(255, 255, 255, 190));
        sweep.setWidthF(penWidth * ratio);
        sweep.setCapStyle(Qt::RoundCap);
        ring.setPen(sweep);
        const int startAngle = (placeholderPhase_ * 24) % 360;
        ring.drawArc(deviceRing, startAngle * 16, 100 * 16);
        openingRingPhase_ = placeholderPhase_;
        openingRingRadius_ = radius;
        openingRingRatio_ = ratio;
    }
    painter.save();
    painter.setWorldTransform(QTransform::fromScale(1.0 / ratio, 1.0 / ratio));
    painter.drawImage(QPoint(originX, originY), openingRing_);
    painter.restore();
}

void VideoSurface::paintDisplayCornerCoverage(QPainter& painter, int targetWidth,
                                              int targetHeight) {
    if (displayCornerRadius_ <= 0) return;
    const qreal ratio = devicePixelRatioF();
    if (!(ratio > 0.0) || targetWidth <= 0 || targetHeight <= 0) return;
    if (displayCornerTopLeft_.isNull() || displayCornerRatio_ != ratio) {
        const qreal radiusDevice = displayCornerRadius_ * ratio;
        const int patch = std::max(2, static_cast<int>(std::ceil(radiusDevice)) + 1);
        // Two patches must never meet: past that the radius does not describe a
        // corner and no coverage is invented for it.
        if (patch * 2 > targetWidth || patch * 2 > targetHeight) return;
        displayCornerTopLeft_ = cornerCoveragePatch(patch, radiusDevice);
        if (displayCornerTopLeft_.isNull()) return;
        displayCornerTopRight_ = displayCornerTopLeft_.flipped(Qt::Horizontal);
        displayCornerBottomLeft_ = displayCornerTopLeft_.flipped(Qt::Vertical);
        displayCornerBottomRight_ = displayCornerTopLeft_.flipped(Qt::Horizontal | Qt::Vertical);
        displayCornerRatio_ = ratio;
    }
    const int patch = displayCornerTopLeft_.width();
    painter.save();
    // Alpha coverage, not paint: the picture and the chrome over it keep their
    // colours, and only what falls outside the rounded shape loses its alpha.
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.setWorldTransform(QTransform::fromScale(1.0 / ratio, 1.0 / ratio));
    painter.drawImage(QPoint(0, 0), displayCornerTopLeft_);
    painter.drawImage(QPoint(targetWidth - patch, 0), displayCornerTopRight_);
    painter.drawImage(QPoint(0, targetHeight - patch), displayCornerBottomLeft_);
    painter.drawImage(QPoint(targetWidth - patch, targetHeight - patch),
                      displayCornerBottomRight_);
    painter.restore();
}

}  // namespace mplayer
