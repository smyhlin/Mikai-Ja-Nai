#include "VideoSurface.hpp"

#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QTimer>
#include <cmath>

#include "MpvHandle.hpp"
#include "MpvRenderContext.hpp"

namespace mplayer {

void VideoSurface::ensureEngine() {
    if (mpv_) return;
    // vo=libmpv routes decoded frames into OUR render context. Without it mpv
    // opens a video output of its own, the render context never receives a
    // frame (update() stays at generation 0) and render() still returns success
    // while painting nothing.
    mpv_ = std::make_unique<MpvHandle>(
        std::vector<std::pair<std::string, std::string>>{{"vo", "libmpv"}});
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
    // The render context must die while the GL context is still current,
    // otherwise mpv's cached entry points outlive the context they belong to.
    if (render_ && contextReady_ && QOpenGLContext::currentContext() != nullptr) {
        render_.reset();
    }
    mpv_.reset();
}

bool VideoSurface::open(const QString& url) {
    url_ = url;
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
    playing_ = false;
    hasVideo_ = false;
    position_ = 0.0;
    duration_ = 0.0;
    videoWidth_ = videoHeight_ = 0;
    handleState();
}

void VideoSurface::togglePause() {
    if (!mpv_) return;
    try {
        const bool paused = mpv_->getFlag("pause");
        mpv_->setPaused(!paused);
        playing_ = paused;  // un-pausing
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
double VideoSurface::position() const { return position_; }

double VideoSurface::duration() const { return duration_; }
int VideoSurface::videoWidth() const { return videoWidth_; }
int VideoSurface::videoHeight() const { return videoHeight_; }
bool VideoSurface::hasVideo() const { return hasVideo_; }

// ── GL lifecycle ──────────────────────────────────────────────────────────

void VideoSurface::initializeGL() {
    QOpenGLWidget::initializeGL();
    try {
        auto gl = std::make_shared<GlApi>(loadGlApi());
        ensureEngine();
        render_ = std::make_unique<MpvRenderContext>(*mpv_, *gl);
        render_->setFlipY(false);
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
