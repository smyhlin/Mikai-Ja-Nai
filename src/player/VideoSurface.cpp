#include "VideoSurface.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include "MpvHandle.hpp"
#include "MpvRenderContext.hpp"

namespace mplayer {

void VideoSurface::ensureEngine() {
    if (mpv_) return;
    // These are the options the Python player passed through python-mpv, which
    // spelled them with underscores; mpv itself takes dashes.
    const std::vector<std::pair<std::string, std::string>> options{
        // Routes decoded frames into OUR render context. Without it mpv opens a
        // video output of its own, the render context never receives a frame
        // (update() stays at generation 0) and render() still returns success
        // while painting nothing.
        {"vo", "libmpv"},
        // Not cosmetic: MoonAnime's CDN answers 400 to a playlist that arrives
        // without an Accept-Language header, so mpv would sit on a stream that
        // extraction read perfectly well. The value is never inspected, only its
        // presence, and mpv splits http-header-fields on commas so it must stay
        // comma-free.
        {"user-agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
         "Chrome/142.0.0.0 Safari/537.36"},
        {"http-header-fields", "Accept-Language: uk-UA"},
        {"hwdec", "auto-safe"},
        {"keepaspect", "yes"},
        {"volume", "100"},
        // The keyboard belongs to the shell: mpv's own bindings would consume
        // Space and the arrow keys before Qt ever sees them.
        {"input-default-bindings", "no"},
        {"input-vo-keyboard", "no"},
        {"input-cursor", "no"},
        {"osc", "no"},
    };
    mpv_ = std::make_unique<MpvHandle>(options);
    // The reader borrows this core, and is (re)bound here because this is the
    // one place a core is created. It publishes an Unavailable (unread) view
    // until the first poll answers; polling itself stays off until the parent
    // asks for it.
    nativeStatus_.attach(mpv_.get());
    mpv_->setEventSink([this](mpv_event* event) {
        if (event->event_id != MPV_EVENT_END_FILE || event->data == nullptr) return;
        const auto* ended = static_cast<const mpv_event_end_file*>(event->data);
        if (ended->reason != MPV_END_FILE_REASON_ERROR) return;
        emit errorOccurred(tr("Could not play the video (mpv: %1)").arg(ended->error));
    });
    observePlaybackState();
    // The applied id, not the mode: a chain with no Mode is only recorded until
    // the engine exists, so reading the mode here would drop it before the
    // first frame.
    const QString chainId =
        anime4kChainId_.isEmpty() ? anime4k::modeId(anime4kMode_) : anime4kChainId_;
    const QString shaders = anime4k::shaderStringForCandidate(chainId);
    if (!shaders.isEmpty()) {
        try {
            mpv_->setString("glsl-shaders", shaders.toStdString());
            applyShaderChain(shaders);
        } catch (const MpvError& err) {
            emit errorOccurred(tr("Anime4K: %1").arg(QString::fromStdString(err.what())));
        }
    }
}

void VideoSurface::setAnime4KChain(const QString& candidateId) {
    if (candidateId == anime4kChainId_) {
        return;
    }
    anime4kChainId_ = candidateId;
    // A named chain supersedes the enum, but the enum is not cleared: a later
    // setAnime4KMode rewrites both, and leaving it stale would let the two
    // disagree about what is on screen.
    if (!mpv_) {
        return;
    }
    const QString shaders = anime4k::shaderStringForCandidate(candidateId);
    try {
        mpv_->setString("glsl-shaders", shaders.toStdString());
        applyShaderChain(shaders);
    } catch (const MpvError& err) {
        emit errorOccurred(tr("Anime4K: %1").arg(QString::fromStdString(err.what())));
    }
    // vo=libmpv rebuilds the filter chain on the next rendered frame, and a
    // paused video produces none, so without this the change would appear to
    // do nothing until playback resumed.
    if (contextReady_ && render_) {
        render_->update();
    }
    update();
}
void VideoSurface::setAnime4KMode(anime4k::Mode mode) {
    // The id is compared too: an override the compare window applied must be
    // taken back when the same mode is picked again.
    const QString id = anime4k::modeId(mode);
    if (mode == anime4kMode_ && id == anime4kChainId_) return;
    anime4kMode_ = mode;
    // The id is the persistence key, so it is recorded here too: a mode chosen
    // from the menu and a chain chosen in the compare window are the same
    // setting, and whichever came last must be what a restart restores.
    anime4kChainId_ = id;
    if (!mpv_) return;
    // The id is resolved, so a mode and a chain go through one path - and only
    // one of the two can name a chain that is not on disk.
    const QString shaders = anime4k::shaderStringForCandidate(anime4kChainId_);
    try {
        mpv_->setString("glsl-shaders", shaders.toStdString());
        applyShaderChain(shaders);
    } catch (const MpvError& err) {
        emit errorOccurred(tr("Anime4K: %1").arg(QString::fromStdString(err.what())));
    }
    // vo=libmpv only rebuilds the filter chain when a new frame is rendered,
    // and a paused video produces none, so the mode would appear to have no
    // effect until playback resumed.
    if (contextReady_ && render_) {
        render_->update();
    }
    update();
}

QString VideoSurface::effectiveShaderChain() const {
    if (!mpv_) return QString();
    // mpv only echoes this back - it does not validate a shader path list - so
    // no answer separates a working chain from a preset naming a deleted file.
    return shaderChain_;
}

void VideoSurface::applyShaderChain(const QString& shaders) {
    // Recorded on success so a caller reading straight after the write sees the
    // chain mpv was just given; the observer keeps it equal to what mpv holds
    // when anything else changes it.
    shaderChain_ = shaders;
}

bool VideoSurface::waitForContext(int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!contextReady_ && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(20);
    }
    return contextReady_;
}

VideoSurface::VideoSurface(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumSize(160, 90);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    // Without this the widget is composited as a separate texture and mpv's
    // frame would be blended against a stale backing store.
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    nativeStatusClock_.start();
    // Installed once, before any core exists. The reader calls this from
    // drainEvents on this thread, and the callback only forwards the published
    // view: nothing here can take the core lock or reach into mpv, so no
    // repaint or teardown can run it against a borrowed handle.
    nativeStatus_.setChangeCallback([this](const native::NativeStatusView& view) {
        // The property read answered with an mpv error, which on this core
        // means no `mikai_native` filter is attached. Repeating the request
        // every 500 ms would only repeat the refusal, so the poll stops until
        // the request state changes or another source is opened. The
        // Unavailable view is still forwarded: the surface never invents an
        // Active status.
        const bool filterAbsent =
            view.availability.value == native::Availability::Unavailable &&
            view.availability.reasonKey == QStringLiteral("native.status.missing");
        if (filterAbsent) {
            nativeStatusHalted_ = true;
        } else if (view.snapshot.has_value() &&
                   !native::nativeStateNeedsPolling(view.snapshot->state)) {
            // The first response named a state no poll can advance (Off,
            // Suspended, Failed, ...): stop after it and wait for a new
            // request, rather than reading the same terminal state forever.
            nativeStatusHalted_ = true;
        }
        emit nativeStatusChanged(view);
    });
}

VideoSurface::~VideoSurface() {
    // The render context holds a pointer to the mpv core, so it has to go first,
    // unconditionally. Members are destroyed in reverse declaration order, which
    // would do that anyway - but resetting mpv_ in the body destroys the core
    // here, and the render context's own destructor then runs against a freed
    // handle. Gating the render reset on the GL context being current used to
    // skip it precisely at process exit, where the context is already gone.
    if (render_) {
        // Freeing the render context releases GL objects, so OUR context has to
        // be current - not merely some context, and not none. At teardown there
        // usually is none: the widget is being deleted, not painted, and a
        // missing context is an access violation inside the driver rather than
        // anything mpv can report.
        const bool borrowed = context_ != nullptr && QOpenGLContext::currentContext() != context_;
        if (borrowed) QOpenGLWidget::makeCurrent();
        render_.reset();
        if (borrowed) QOpenGLWidget::doneCurrent();
    }
    if (mpv_) {
        // Unobserve for real before the core goes, so mpv is not still
        // formatting changes for a widget that is being destroyed.
        for (const uint64_t id : observers_) mpv_->unobserve(id);
        observers_.clear();
    }
    // The native-status reader borrows the same core: release the binding and
    // cancel any in-flight request before the core is reset, so no callback can
    // run against a widget that is going away.
    resetNativeStatusReader(false);
    mpv_.reset();
}

bool VideoSurface::open(const QString& url) {
    url_ = url;
    networkBytesPerSecond_ = 0.0;
    streamFramesPerSecond_ = 0.0;
    statsClock_.restart();
    emit streamStatsChanged(0.0, 0.0);
    ensureEngine();
    // A new source: the previous filter's snapshot says nothing about this one,
    // so the reader is rebound (which clears the published view) and a halt
    // recorded for the old source does not outlive it.
    resetNativeStatusReader(true);
    nativeStatusHalted_ = false;
    // The load waits for the GL context. With vo=libmpv the video output
    // attaches to the render context, and mpv will not start a VO that has
    // nowhere to draw: loading first leaves the core with no video and a black
    // surface. Callers legitimately open before the widget is ever shown.
    if (!contextReady_) {
        pendingOpen_ = true;
        return true;
    }
    return startPendingUrl();
}

bool VideoSurface::startPendingUrl() {
    pendingOpen_ = false;
    if (url_.isEmpty()) {
        return false;
    }
    try {
        mpv_->loadFile(url_.toStdString());
    } catch (const MpvError& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
        return false;
    }
    // mpv also comes up paused when a render context is its only consumer: it
    // has no window of its own to drive playback. Without this the core decodes
    // one frame, hands it over and stops, which reads as a black surface and a
    // frozen clock rather than as an obvious failure.
    try {
        mpv_->setPaused(false);
    } catch (const MpvError&) {
        // The host may intend to open paused and unpause itself.
    }
    return true;
}

void VideoSurface::close() {
    if (!mpv_) return;
    // Nothing is loaded after this: drop the reader before the source ends so
    // no request in flight can describe a file that is gone, and clear the halt
    // that belonged to the old source.
    resetNativeStatusReader(false);
    nativeStatusHalted_ = false;
    try {
        mpv_->stop();
    } catch (const MpvError&) {
        // A stop on an already-dead core is not worth reporting.
    }
    // Emitted before the reset so the chrome falls back to its play glyph
    // while the surface still reports a loaded file.
    if (playing_) {
        playing_ = false;
        emit playingChanged(false);
    }
    hasVideo_ = false;
    position_ = 0.0;
    duration_ = 0.0;
    videoWidth_ = videoHeight_ = 0;
    networkBytesPerSecond_ = 0.0;
    streamFramesPerSecond_ = 0.0;
    // The core keeps reporting while nothing is loaded, and the next open must
    // not start from the previous file's numbers.
    observedDuration_ = 0.0;
    observedPosition_ = 0.0;
    observedVideoWidth_ = 0.0;
    observedVideoHeight_ = 0.0;
    observedCacheSpeed_ = 0.0;
    observedEstimatedFps_ = 0.0;
    observedContainerFps_ = 0.0;
    emit streamStatsChanged(0.0, 0.0);
    handleState();
}

void VideoSurface::togglePause() {
    if (!mpv_) return;
    try {
        // The observed pause state, not a synchronous read: this is a control
        // the user is waiting on, and a slow native operation must not hold the
        // HUD behind the core lock.
        const bool paused = observedPaused_;
        mpv_->setPaused(!paused);
        setPlaying(!paused);
    } catch (const MpvError& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
    }
}

void VideoSurface::setPaused(bool paused) {
    if (!mpv_) return;
    try {
        // Unconditional, deliberately. An earlier version short-circuited on
        // the cached playing_ flag, and when that flag was out of step with
        // the core the call was silently dropped - the caller believed it had
        // resumed a surface that stayed frozen. Asking mpv costs one property
        // write; guessing wrong costs a video that will not play.
        mpv_->setPaused(paused);
        setPlaying(!paused);
    } catch (const MpvError& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
    }
}

void VideoSurface::seek(double seconds) {
    if (!mpv_) return;
    try {
        mpv_->seek(seconds);
    } catch (const MpvError&) {
        // Seeking before a file is loaded is normal during startup.
    }
}

void VideoSurface::setVolume(double volume) {
    if (!mpv_) return;
    try {
        mpv_->setVolume(volume);
    } catch (const MpvError&) {}
}

void VideoSurface::setMuted(bool muted) {
    if (!mpv_) return;
    try {
        mpv_->setMuted(muted);
    } catch (const MpvError&) {}
}

void VideoSurface::setSpeed(double speed) {
    if (!mpv_) return;
    try {
        mpv_->setSpeed(speed);
    } catch (const MpvError&) {}
}

bool VideoSurface::isPlaying() const { return playing_; }

void VideoSurface::setPlaying(bool playing) {
    // Announced, not just recorded. The state pump cannot notice a change this
    // class makes itself: it compares the core against this very flag, so
    // writing it here and staying silent left the pump nothing to report and
    // the play control frozen on its old glyph - the video paused behind a
    // "play" button, or running behind a "pause" one.
    if (playing == playing_) {
        return;
    }
    playing_ = playing;
    emit playingChanged(playing_);
}
double VideoSurface::position() const { return position_; }

double VideoSurface::duration() const { return duration_; }
int VideoSurface::videoWidth() const { return videoWidth_; }
int VideoSurface::videoHeight() const { return videoHeight_; }
bool VideoSurface::hasVideo() const { return hasVideo_; }

// ── GL lifecycle ──────────────────────────────────────────────────────────

void VideoSurface::initializeGL() {
    QOpenGLWidget::initializeGL();
    context_ = QOpenGLContext::currentContext();
    try {
        const GlApi gl = loadGlApi();
        ensureEngine();
        render_ = std::make_unique<MpvRenderContext>(*mpv_, gl);
        // mpv draws with the origin at the bottom-left; Qt composites the
        // QOpenGLWidget's framebuffer from the top-left. Without this the picture
        // is upside down - which a non-black pixel check happily passes.
        render_->setFlipY(true);
        render_->setUpdateCallback([this] {
            // Fires on an mpv thread. Hop to the GUI thread rather than
            // touching the widget from here.
            QMetaObject::invokeMethod(this, [this] { framePending_ = true; update(); },
                                      Qt::QueuedConnection);
        });
        contextReady_ = true;
        // The first paint after the context exists has to render whatever mpv
        // is holding, even if no frame has been decoded yet: the framebuffer
        // has never been drawn into.
        forceRender_ = true;
        statePump_.setInterval(50);
        connect(&statePump_, &QTimer::timeout, this, &VideoSurface::pumpEvents);
        statePump_.start();
        if (pendingOpen_) startPendingUrl();
    } catch (const MpvError& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
        contextReady_ = false;
    }
}

void VideoSurface::resizeGL(int width, int height) {
    QOpenGLWidget::resizeGL(width, height);
    // Qt hands QOpenGLWidget a freshly allocated framebuffer on a resize, so
    // whatever the last paint drew into the old one is gone. The next paint
    // has to render even though no frame has been decoded since.
    forceRender_ = true;
    if (contextReady_) render_->update();
}

void VideoSurface::showEvent(QShowEvent* event) {
    QOpenGLWidget::showEvent(event);
    // Same reasoning as resizeGL: a surface that was hidden (the pooled PiP
    // float) cannot assume its framebuffer still holds the last frame, and
    // with a paused core there may never be another one.
    forceRender_ = true;
}

void VideoSurface::paintGL() {
    QOpenGLWidget::paintGL();
    if (!contextReady_ || !render_) {
        return;
    }
    // The state pump belongs on the 50 ms timer, not here: paintGL runs only
    // when there is a frame, so a paused core - which produces none - would
    // otherwise freeze the clock and every HUD value with the last picture.
    //
    // update() flags a redraw only when the decoder actually produced a frame,
    // so it doubles as the frame-available check and the keep-going signal.
    const uint64_t generation = render_->update();
    // forceRender_ covers the framebuffer Qt hands back empty: a resize or a
    // re-show wipes the picture, and with a paused core no new frame is coming,
    // so grabFramebuffer() - which reads that buffer instead of repainting -
    // would otherwise read a buffer nothing has drawn into.
    const bool haveFrame = framePending_ || generation != lastGeneration_ || forceRender_;
    lastGeneration_ = generation;

    // Qt renders QOpenGLWidget into its own framebuffer object, so fbo=0 would
    // make mpv bind the window's default framebuffer and paint nothing. The FBO
    // is also sized in device pixels, so passing logical width()/height() fills
    // only the bottom-left quarter on a HiDPI display.
    const GLuint target = static_cast<GLuint>(defaultFramebufferObject());
    const qreal ratio = devicePixelRatioF();
    const int targetWidth = std::max(1, static_cast<int>(width() * ratio));
    const int targetHeight = std::max(1, static_cast<int>(height() * ratio));
    render_->render(static_cast<int>(target), targetWidth, targetHeight, 0);
    render_->reportSwap();
    framePending_ = false;
    forceRender_ = false;

    // Only schedule the next frame if a fresh decoded frame arrived. When
    // paused, haveFrame is false so it stops scheduling updates and sits still
    // on the current frame without spinning or going black.
    if (haveFrame) {
        QOpenGLWidget::update();
    }
}

// ── state pump ────────────────────────────────────────────────────────────

void VideoSurface::observePlaybackState() {
    if (!mpv_) return;
    if (!observers_.empty()) return;
    // The registered format is what mpv delivers, so each observer reads
    // exactly what it wrote.
    const auto watch = [this](const char* name, mpv_format format, auto&& apply) {
        // A property this core does not have costs the HUD that one number, not
        // playback, so a refusal is reported and stepped over.
        try {
            const uint64_t id = mpv_->observe(
                name, format, [apply = std::forward<decltype(apply)>(apply)](const Property& p) {
                    apply(p);
                });
            if (id != 0) observers_.push_back(id);
        } catch (const MpvError& error) {
            qWarning("mpv: cannot observe %s (%s)", name, error.what());
        }
    };
    watch("duration", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedDuration_ = p.isNumber() ? p.number : 0.0; });
    watch("time-pos", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedPosition_ = p.isNumber() ? p.number : 0.0; });
    watch("pause", MPV_FORMAT_FLAG,
          [this](const Property& p) { observedPaused_ = p.isFlag() && p.flag; });
    watch("video-params/w", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedVideoWidth_ = p.isNumber() ? p.number : 0.0; });
    watch("video-params/h", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedVideoHeight_ = p.isNumber() ? p.number : 0.0; });
    watch("cache-speed", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedCacheSpeed_ = p.isNumber() ? p.number : 0.0; });
    watch("estimated-vf-fps", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedEstimatedFps_ = p.isNumber() ? p.number : 0.0; });
    watch("container-fps", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedContainerFps_ = p.isNumber() ? p.number : 0.0; });
    watch("glsl-shaders", MPV_FORMAT_STRING,
          [this](const Property& p) { shaderChain_ = QString::fromStdString(p.text); });
}

void VideoSurface::publishObservedState() {
    if (observedDuration_ > 0 && observedDuration_ != duration_) {
        duration_ = observedDuration_;
        emit durationChanged(duration_);
    }
    // 10 ms is finer than any display, so this cannot look stepped.
    if (std::abs(observedPosition_ - position_) > 0.01) {
        position_ = observedPosition_;
        emit positionChanged(position_);
    }
    // Compare like with like: the property is "pause", the cached state is
    // "playing". Guarding on `paused != playing_` means a playing video
    // (false vs false) never updates, so isPlaying() stays false forever.
    const bool playing = !observedPaused_;
    if (playing != playing_) {
        playing_ = playing;
        emit playingChanged(playing_);
    }
}

void VideoSurface::pumpEvents() {
    if (!mpv_) return;
    try {
        mpv_->drainEvents();
    } catch (const MpvError&) {
        return;
    }
    publishObservedState();
    handleState();
    updateStreamStats();
    // The last thing the pump does: a request issued here is answered by the
    // next pump's drainEvents, so a reply is never delivered re-entrantly from
    // inside the tick that issued it.
    pollNativeStatus();
}

void VideoSurface::updateStreamStats() {
    if (!statsClock_.isValid()) {
        statsClock_.start();
        return;
    }
    if (statsClock_.elapsed() < 500) {
        return;
    }
    statsClock_.restart();

    // Observed, not read: these two belong to a HUD label and must not wait on
    // the core lock.
    double bytesPerSecond = std::max(0.0, observedCacheSpeed_);
    double framesPerSecond = observedEstimatedFps_;
    if (framesPerSecond <= 0.0) framesPerSecond = observedContainerFps_;
    framesPerSecond = std::max(0.0, framesPerSecond);
    if (std::abs(bytesPerSecond - networkBytesPerSecond_) < 1024.0 &&
        std::abs(framesPerSecond - streamFramesPerSecond_) < 0.01) {
        return;
    }
    networkBytesPerSecond_ = bytesPerSecond;
    streamFramesPerSecond_ = framesPerSecond;
    emit streamStatsChanged(networkBytesPerSecond_, streamFramesPerSecond_);
}

void VideoSurface::handleState() {
    if (!mpv_) return;
    // One value, two answers. The width decides both whether there is a video at
    // all and how wide it is, and it was observed rather than read, so this costs
    // no trip through the core lock against mpv's own render thread.
    const int w = static_cast<int>(observedVideoWidth_);
    const bool vid = w > 0;
    if (vid != hasVideo_) hasVideo_ = vid;
    if (hasVideo_) {
        const int h = static_cast<int>(observedVideoHeight_);
        if (w != videoWidth_ || h != videoHeight_) {
            videoWidth_ = w;
            videoHeight_ = h;
            emit videoSizeChanged(w, h);
        }
    }
}

// ── native status ─────────────────────────────────────────────────────────

void VideoSurface::setNativeStatusPolling(bool enabled) {
    // A call here is a request-state change: it re-arms a poll that a terminal
    // state or an absent filter had stopped. Nothing is read synchronously -
    // the next pump step is what issues the request, so a caller on the GUI
    // thread never waits on the core lock.
    if (enabled) nativeStatusHalted_ = false;
    nativeStatusPolling_ = enabled;
}

bool VideoSurface::nativeStatusShouldPoll() const {
    return nativeStatusPolling_ && !nativeStatusHalted_;
}

void VideoSurface::pollNativeStatus() {
    if (!mpv_) return;
    // The reader is ticked even when it will not issue a request: the tick
    // still abandons a timed-out one and keeps the current source epoch in
    // view. `sourceEpoch()` is bumped by load and stop, so a reply for a
    // superseded source is dropped by the reader rather than published.
    nativeStatus_.tick(nativeStatusClock_.elapsed(), mpv_->sourceEpoch(),
                       nativeStatusShouldPoll());
}

void VideoSurface::resetNativeStatusReader(bool reattach) {
    // Cancels any in-flight request and clears the published view, so a plan
    // from the old source cannot keep reading as current. detach() is safe on a
    // reader that was never attached, which is what a close() before open()
    // looks like.
    nativeStatus_.detach();
    if (reattach && mpv_) nativeStatus_.attach(mpv_.get());
}

}  // namespace mplayer
