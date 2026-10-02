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

#include <QElapsedTimer>
#include <QMetaType>
#include <QOpenGLWidget>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>
#include <vector>

#include "Anime4K.hpp"
#include "native/NativeStatus.hpp"

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
    /// Sets the paused state outright. togglePause cannot restore a known
    /// state, which is what handing playback back to a parked core needs.
    void setPaused(bool paused);
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
    /// Swaps in an arbitrary chain by candidate id, which is what the compare
    /// window needs: it can name a single .glsl that has no Mode at all. The
    /// id is kept so the choice survives a re-resolve, never the resolved path
    /// list, which goes stale the moment the shaders folder moves.
    void setAnime4KChain(const QString& candidateId);
    /// The candidate id currently applied, empty for Off.
    QString anime4KChainId() const { return anime4kChainId_; }
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
    /// Records the pause state and announces a change. Every local pause goes
    /// through here, because the state pump can only report what it did not
    /// already know.
    void setPlaying(bool playing);
    double position() const;
    double duration() const;
    int videoWidth() const;
    int videoHeight() const;
    bool hasVideo() const;

    // ── native status ─────────────────────────────────────────────────────
    //
    // The surface owns one reader for its core and drives it from the existing
    // 50 ms pump, never from paintGL and never through a synchronous property
    // read. Polling is OFF by default: the parent that owns the native UI turns
    // it on only while native work is requested (Preparing, Switching or
    // Active), and turns it off once the filter reports anything else.
    void setNativeStatusPolling(bool enabled);
    bool nativeStatusPolling() const { return nativeStatusPolling_; }
    /// The last published native status. Unavailable with a named reason until
    /// the filter answers, and again whenever the filter is absent or its
    /// document is refused. Never synthesised: this is the reader's own view.
    const native::NativeStatusView& nativeStatus() const { return nativeStatus_.status(); }

signals:
    void playingChanged(bool playing);
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void videoSizeChanged(int width, int height);
    void streamStatsChanged(double bytesPerSecond, double framesPerSecond);
    void errorOccurred(const QString& message);
    void nativeStatusChanged(const native::NativeStatusView& status);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;
    void showEvent(QShowEvent* event) override;

private:
    /// Creates the engine with vo=libmpv. Both open() and initializeGL() need it
    /// and whichever runs first wins, so the options cannot be set in only one.
    void ensureEngine();
    /// Issues the deferred load once the render context exists.
    bool startPendingUrl();
    void pumpEvents();
    void handleState();
    void updateStreamStats();
    /// Subscribes to every property the HUD reads. The values arrive as events,
    /// so the 50 ms pump never takes the core lock to learn what it shows.
    void observePlaybackState();
    void publishObservedState();
    /// Records the chain mpv was just handed, so a caller reading right after
    /// the write sees it without waiting for the observer.
    void applyShaderChain(const QString& shaders);
    /// One native-status pump step, driven from the 50 ms timer. The reader is
    /// ticked even while polling is off, so a timed-out request is still
    /// abandoned and the current source epoch keeps being observed.
    void pollNativeStatus();
    /// True while the reader should issue a request: polling was requested, the
    /// filter has not been found absent, and the last state read is one that a
    /// poll can advance (or has not been read at all).
    bool nativeStatusShouldPoll() const;
    /// Drops the reader's binding, cancelling any in-flight request, and
    /// optionally rebinds it to the core that is alive now. MUST run before the
    /// borrowed core is destroyed, and before a new source is bound.
    void resetNativeStatusReader(bool reattach);

    // Held so the render context can be freed with it current at teardown.
    QOpenGLContext* context_ = nullptr;
    std::unique_ptr<MpvHandle> mpv_;
    std::unique_ptr<MpvRenderContext> render_;
    QString url_;
    anime4k::Mode anime4kMode_ = anime4k::Mode::Off;
    QString anime4kChainId_;

    // mpv keeps running whether or not we repaint, so the state pump cannot
    // live in paintGL: a paused video produces no frames, and the clock,
    // the duration and the pause state would all freeze with the last one.
    QTimer statePump_;
    QElapsedTimer statsClock_;
    // Monotonic, elapsed-from-construction: the reader's cadence must not be
    // read off a wall clock that can step.
    QElapsedTimer nativeStatusClock_;

    // The one native-status reader this surface owns, bound to its own core.
    native::NativeStatusConsumer nativeStatus_;
    bool nativeStatusPolling_ = false;
    /// Set when the filter is absent (the async property read returned an mpv
    /// error): polling stops until the request state changes or a new source is
    /// opened, so an absent filter is probed once, not hammered.
    bool nativeStatusHalted_ = false;

    bool contextReady_ = false;
    bool pendingOpen_ = false;
    uint64_t lastGeneration_ = 0;
    bool framePending_ = false;   // set by mpv's update callback
    /// Forces the next paint to render even with no freshly decoded frame:
    /// set when Qt hands the widget a new framebuffer (resize, re-show) and
    /// the previous picture is no longer there to composite.
    bool forceRender_ = true;
    bool playing_ = false;
    double position_ = 0.0;
    bool hasVideo_ = false;
    double duration_ = 0.0;
    int videoWidth_ = 0;
    int videoHeight_ = 0;
    double networkBytesPerSecond_ = 0.0;
    double streamFramesPerSecond_ = 0.0;

    // Observed values, written only by the property observers during
    // drainEvents. Everything the HUD reads comes from here.
    double observedDuration_ = 0.0;
    double observedPosition_ = 0.0;
    bool observedPaused_ = false;
    double observedVideoWidth_ = 0.0;
    double observedVideoHeight_ = 0.0;
    double observedCacheSpeed_ = 0.0;
    double observedEstimatedFps_ = 0.0;
    double observedContainerFps_ = 0.0;
    QString shaderChain_;
    /// The subscriptions this widget owns, released before the core.
    std::vector<uint64_t> observers_;
};

}  // namespace mplayer

// Declared exactly once, here, where the view is complete: the change signal
// carries it by const reference and a queued connection needs the type known
// to the meta-object system.
Q_DECLARE_METATYPE(mplayer::native::NativeStatusView)
