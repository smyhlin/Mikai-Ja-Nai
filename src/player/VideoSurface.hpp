// The video surface: a QOpenGLWidget that mpv draws into directly.
//
// No winId(), no native child HWND. That is the entire point: the Python player
// gave mpv a native window, and a native child window on Windows always paints
// above non-native siblings no matter what raise() does, which is why the dock
// kept vanishing behind the video. Here mpv renders into the framebuffer Qt is
// already painting, so the chrome is ordinary sibling widgets and z-order is
// Qt's problem again - where it belongs.
//
// The surface is also the single place that knows the OpenGL context exists:
// loadGlApi() can only resolve entry points from inside a current context, so
// the render context is created in initializeGL and destroyed in the
// destructor before Qt tears the context down.

#pragma once

#include <QOpenGLWidget>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

#include "Anime4K.hpp"

namespace mplayer {
class MpvHandle;
class MpvRenderContext;
}  // namespace mplayer

namespace mplayer {

class VideoSurface : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit VideoSurface(QWidget* parent = nullptr);
    ~VideoSurface() override;

    // Loads url. Returns false only if the engine itself could not be created;
    // a stream that fails to open reports through errorOccurred instead.
    bool open(const QString& url);
    void close();

    void togglePause();
    void seek(double seconds);
    void setVolume(double volume);   // 0..100
    void setMuted(bool muted);
    void setSpeed(double speed);

    // Swaps the Anime4K shader chain on the live handle, so the change lands
    // on the next rendered frame without reopening the file. Remembered, and
    // re-applied, because a mode picked before the engine exists still has to
    // take effect once it does.
    void setAnime4KMode(anime4k::Mode mode);
    anime4k::Mode anime4KMode() const { return anime4kMode_; }
    /// What mpv reports for glsl-shaders right now. It only echoes back the
    /// string it was given - mpv does not validate a path list, so this cannot
    /// tell a working chain from a preset naming a deleted file. Useful for
    /// Off vs On; the probe proves the rest with pixels.
    QString effectiveShaderChain() const;

    bool isContextReady() const { return contextReady_; }
    /// Spins the event loop until the render context exists. A surface that has
    /// been constructed but not yet shown never gets one, so a caller that
    /// needs a framebuffer has to be able to wait for it.
    bool waitForContext(int timeoutMs);

    bool isPlaying() const;
    double position() const;
    double duration() const;
    int videoWidth() const;
    int videoHeight() const;
    bool hasVideo() const;

signals:
    void playingChanged(bool playing);
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void videoSizeChanged(int width, int height);
    void errorOccurred(const QString& message);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    /// Creates the engine with vo=libmpv. Both open() and initializeGL() need it
    /// and whichever runs first wins, so the options cannot be set in only one.
    void ensureEngine();
    /// Issues the deferred load once the render context exists.
    bool startPendingUrl();
    void pumpEvents();
    void handleState();

    // Held so the render context can be freed with it current at teardown.
    QOpenGLContext* context_ = nullptr;
    std::unique_ptr<MpvHandle> mpv_;
    std::unique_ptr<MpvRenderContext> render_;
    QString url_;
    anime4k::Mode anime4kMode_ = anime4k::Mode::Off;

    // mpv keeps running whether or not we repaint, so the state pump cannot
    // live in paintGL: a paused video produces no frames, and the clock,
    // the duration and the pause state would all freeze with the last one.
    QTimer statePump_;

    bool contextReady_ = false;
    bool pendingOpen_ = false;
    uint64_t lastGeneration_ = 0;
    bool framePending_ = false;   // set by mpv's update callback
    bool playing_ = false;
    double position_ = 0.0;
    bool hasVideo_ = false;
    double duration_ = 0.0;
    int videoWidth_ = 0;
    int videoHeight_ = 0;
};

}  // namespace mplayer
