#include "VideoSurface.hpp"

#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QTimer>

#include "MpvHandle.hpp"
#include "MpvRenderContext.hpp"

namespace mplayer {

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
    if (!mpv_) mpv_ = std::make_unique<MpvHandle>();
    try {
        mpv_->loadFile(url.toStdString());
    } catch (const MpvError& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
        return false;
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
double VideoSurface::position() const {
    if (!mpv_ || duration_ <= 0) return 0.0;
    try {
        return mpv_->getNumber("time-pos");
    } catch (const MpvError&) {
        return 0.0;
    }
}
double VideoSurface::duration() const { return duration_; }
int VideoSurface::videoWidth() const { return videoWidth_; }
int VideoSurface::videoHeight() const { return videoHeight_; }
bool VideoSurface::hasVideo() const { return hasVideo_; }

// ── GL lifecycle ──────────────────────────────────────────────────────────

void VideoSurface::initializeGL() {
    QOpenGLWidget::initializeGL();
    try {
        auto gl = std::make_shared<GlApi>(loadGlApi());
        if (!mpv_) mpv_ = std::make_unique<MpvHandle>(
            std::vector<std::pair<std::string, std::string>>{{"vo", "libmpv"}});
        render_ = std::make_unique<MpvRenderContext>(*mpv_, *gl);
        render_->setFlipY(false);
        render_->setUpdateCallback([this] {
            // Fires on an mpv thread. Hop to the GUI thread rather than
            // touching the widget from here.
            QMetaObject::invokeMethod(this, [this] { framePending_ = true; update(); },
                                      Qt::QueuedConnection);
        });
        contextReady_ = true;
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
    // fbo 0 is the framebuffer Qt is already painting, which is the whole
    // point: mpv draws into the same surface as the chrome.
    // Qt renders QOpenGLWidget into its own framebuffer object, so fbo=0 would
    // make mpv bind the window's default framebuffer instead and paint nothing.
    // defaultFramebufferObject() is the handle mpv must draw into.
    // The FBO is sized in device pixels, so the target rect must be too.
    // Passing logical width()/height() makes mpv fill only the bottom-left
    // quarter on a HiDPI display.
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
    bool changed = false;
    try {
        const double dur = mpv_->getNumber("duration");
        if (dur > 0 && dur != duration_) {
            duration_ = dur;
            emit durationChanged(dur);
        }
        const bool paused = mpv_->getFlag("pause");
        if (paused != playing_) {
            playing_ = !paused;
            emit playingChanged(playing_);
            changed = true;
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
