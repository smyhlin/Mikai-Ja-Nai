#include "VideoSurface.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QThread>
#include <QTimer>
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
    // Applied as a construction option so a mode restored from settings is in
    // place before the first frame. mpv accepts the same write at runtime - it
    // is the path setAnime4KMode uses when the user changes preset mid-play.
    const QString shaders = anime4k::shaderStringForMode(anime4kMode_);
    if (!shaders.isEmpty()) {
        try {
            mpv_->setString("glsl-shaders", shaders.toStdString());
        } catch (const MpvError& err) {
            emit errorOccurred(QStringLiteral("Anime4K: %1").arg(QString::fromStdString(err.what())));
        }
    }
}

void VideoSurface::setAnime4KMode(anime4k::Mode mode) {
    if (mode == anime4kMode_) return;
    anime4kMode_ = mode;
    if (!mpv_) return;
    try {
        mpv_->setString("glsl-shaders", anime4k::shaderStringForMode(mode).toStdString());
    } catch (const MpvError& err) {
        emit errorOccurred(QStringLiteral("Anime4K: %1").arg(QString::fromStdString(err.what())));
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
    try {
        return QString::fromStdString(mpv_->getString("glsl-shaders"));
    } catch (const MpvError&) {
        return QString();
    }
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
}

VideoSurface::~VideoSurface() {
    // The render context holds a pointer to the mpv core, so it has to go first,
    // unconditionally. Members are destroyed in reverse declaration order, which
    // would do that anyway - but resetting mpv_ in the body destroys the core
    // here, and the render context's own destructor then runs against a freed
    // handle. Gating the render reset on the GL context being current used to
    // skip it precisely at process exit, where the context is already gone.
    if (render_) {
        // Freeing the render context releases GL objects, so a current context is
        // not optional. At teardown there usually is none: the widget is being
        // deleted, not painted, and the crash is an access violation inside the
        // driver rather than anything mpv can report.
        const bool borrowed = QOpenGLContext::currentContext() == nullptr
                              && context_ != nullptr;
        if (borrowed) QOpenGLWidget::makeCurrent();
        render_.reset();
        if (borrowed) QOpenGLWidget::doneCurrent();
    }
    mpv_.reset();
}

bool VideoSurface::open(const QString& url) {
    url_ = url;
    networkBytesPerSecond_ = 0.0;
    streamFramesPerSecond_ = 0.0;
    statsClock_.restart();
    emit streamStatsChanged(0.0, 0.0);
    ensureEngine();
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
    emit streamStatsChanged(0.0, 0.0);
    handleState();
}

void VideoSurface::togglePause() {
    if (!mpv_) return;
    try {
        const bool paused = mpv_->getFlag("pause");
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
        auto gl = std::make_shared<GlApi>(loadGlApi());
        ensureEngine();
        render_ = std::make_unique<MpvRenderContext>(*mpv_, *gl);
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
    if (contextReady_) render_->update();
}

void VideoSurface::paintGL() {
    QOpenGLWidget::paintGL();
    if (!contextReady_ || !render_) {
        return;
    }
    pumpEvents();
    // update() bumps mpv's generation counter only when the decoder
    // actually produced a frame, so it doubles as the frame-available
    // check and the keep-going signal. Repainting unconditionally here
    // would spin a core on a paused video.
    const uint64_t generation = render_->update();
    const bool haveFrame = framePending_ || generation != lastGeneration_;
    lastGeneration_ = generation;
    // Qt renders QOpenGLWidget into its own framebuffer object, so fbo=0 would
    // make mpv bind the window's default framebuffer and paint nothing. The FBO
    // is also sized in device pixels, so passing logical width()/height() fills
    // only the bottom-left quarter on a HiDPI display.
    const qreal ratio = devicePixelRatioF();
    const int targetWidth = std::max(1, static_cast<int>(width() * ratio));
    const int targetHeight = std::max(1, static_cast<int>(height() * ratio));
    render_->render(static_cast<int>(defaultFramebufferObject()), targetWidth, targetHeight, 0);
    render_->reportSwap();
    framePending_ = false;
    if (haveFrame) QOpenGLWidget::update();
}

// ── state pump ────────────────────────────────────────────────────────────

void VideoSurface::pumpEvents() {
    if (!mpv_) return;
    try {
        mpv_->drainEvents();
    } catch (const MpvError&) {
        return;
    }
    try {
        const double dur = mpv_->getNumber("duration");
        if (dur > 0 && dur != duration_) {
            duration_ = dur;
            emit durationChanged(dur);
        }
        const double pos = mpv_->getNumber("time-pos");
        // 10 ms is finer than any display, so this cannot look stepped.
        if (std::abs(pos - position_) > 0.01) {
            position_ = pos;
            emit positionChanged(pos);
        }
        // Compare like with like: the property is "pause", the cached state is
        // "playing". Guarding on `paused != playing_` means a playing video
        // (false vs false) never updates, so isPlaying() stays false forever.
        const bool playing = !mpv_->getFlag("pause");
        if (playing != playing_) {
            playing_ = playing;
            emit playingChanged(playing_);
        }
    } catch (const MpvError&) {
        // Properties are unavailable before a file loads; that is not an error.
    }
    handleState();
    updateStreamStats();
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

    double bytesPerSecond = 0.0;
    double framesPerSecond = 0.0;
    try {
        bytesPerSecond = mpv_->getNumber("cache-speed");
    } catch (const MpvError&) {
    }
    try {
        framesPerSecond = mpv_->getNumber("estimated-vf-fps");
    } catch (const MpvError&) {
    }
    if (framesPerSecond <= 0.0) {
        try {
            framesPerSecond = mpv_->getNumber("container-fps");
        } catch (const MpvError&) {
        }
    }

    bytesPerSecond = std::max(0.0, bytesPerSecond);
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
    try {
        const bool vid = mpv_->getNumber("video-params/w") > 0;
        if (vid != hasVideo_) hasVideo_ = vid;
        if (hasVideo_) {
            const int w = static_cast<int>(mpv_->getNumber("video-params/w"));
            const int h = static_cast<int>(mpv_->getNumber("video-params/h"));
            if (w != videoWidth_ || h != videoHeight_) {
                videoWidth_ = w;
                videoHeight_ = h;
                emit videoSizeChanged(w, h);
            }
        }
    } catch (const MpvError&) {}
}

}  // namespace mplayer
