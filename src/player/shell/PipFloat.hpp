#pragma once

#include <QList>
#include <QPoint>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QCloseEvent;
class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPainter;
class QResizeEvent;
class QScreen;
class QShowEvent;
class QSlider;
class QTimer;
class QVariantAnimation;
QT_END_NAMESPACE

#include <optional>

#include "ShellWidgets.hpp"

namespace mplayer {
class VideoSurface;
}

namespace mplayer {
namespace shell {

/// A frameless float requesting always-on-top, showing the same stream in its own
/// VideoSurface: mpv gives one core per surface, so the float decodes a second
/// time rather than sharing a handle it cannot have.
///
/// Where the window system permits client placement, it is placed FREELY. A drag
/// drops it exactly where the pointer leaves it -
/// no snap-to-corner on release, no re-pin to a corner on resize, show, or
/// re-entry. The single placement rule is reachability: some part of the float
/// must share pixels with a work area, or it cannot be clicked or dragged back.
/// A pooled float re-enters at the exact position and size the viewer last left
/// it; only an unreachable position (a monitor that went away) is corrected.
/// On Wayland, placement, stacking and interactive moves are compositor-controlled.
///
/// Its chrome is painted, not composited: the float is a ChromeHost, so every
/// control it carries is adopted and drawn from the same paintGL pass that
/// carried mpv's frame. A widget over a QOpenGLWidget that keeps its own
/// backing store is a second raster pass over the picture and a stacking the
/// float cannot control.
// T-404: the float's default geometry, in one place. PipFloat.cpp,
// PlayerDialog.cpp and PlayerDialogProcessing.cpp all used to carry their own
// file-local pair - three copies that could (and had to be kept from drifting).
inline constexpr int kPipDefaultWidth = 480;
inline constexpr int kPipDefaultHeight = 270;

class VolumeTrack;

class PipFloat : public QWidget, public ChromeHost {
    Q_OBJECT

public:
    explicit PipFloat(QWidget* parent = nullptr);

    VideoSurface* videoSurface() const { return surface_; }
    /// Opens `url` and seeks to `position` once the stream is up. `mediaToken`
    /// is the player's opaque identity for the source, passed through so the
    /// float's own core reports the load and end of THIS source.
    void openStream(const QString& url, double position, bool paused, quint64 mediaToken = 0);
    /// Places a brand-new float against a screen work area, bottom-right by
    /// default. Used only for the float's first-ever placement: a pooled float
    /// keeps the exact position and size the viewer last left it, so this is
    /// never called on re-entry.
    void moveToCorner(QScreen* target);
    void closeStream();

    void setPlaying(bool playing);
    /// Display only: nothing is emitted, so answering with a seek could loop.
    void setTime(double position, double duration);
    void setMuted(bool muted);
    /// 0…1, matching the shell. The float runs a second mpv core, so the
    /// level has to be pushed to it rather than read off the main player.
    void setVolume(double volume);

    /// Where the float's own core is. The host reads it to restore the main
    /// player on exit, so both windows resume from what the viewer watched.
    double position() const;
    double duration() const;
    bool isPlaying() const;

signals:
    void muteChanged(bool muted);
    void volumeChanged(double volume);
    void restoreRequested();
    void closeRequested();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

    /// An adopted control changed. The surface fills the whole float, so the
    /// region that has to be redrawn is the whole picture.
    void chromeDirty(QWidget* widget, const QRect& rect) override;
    /// A move or a resize leaves stale pixels the new geometry does not
    /// describe, so everything the surface carries is redrawn.
    void chromeDirtyAll() override;

private:
    void buildChrome();
    void updateVolumeChip();
    /// The mute button's glyph, tooltip and accessible name, from the retained
    /// mute state. Split out of setMuted() so a language change re-labels the
    /// button without writing the float's core.
    void updateMuteChrome();
    /// Re-labels every control from state only - the title, the transport and
    /// mute hints, the two clocks - and repaints the picture the chrome is
    /// drawn into. It asks the core nothing and changes no playback state.
    void retranslateUi();
    /// Draws the float's own chrome - the timeline, which is not a widget -
    /// into the picture's painter. Connected to VideoSurface::paintOverlay.
    void paintSurfaceOverlay(QPainter* painter);
    /// The groove, the played part and the handle, placed from the scrub
    /// slider's value and width.
    void paintTimeline(QPainter& painter);
    /// A press on the timeline seeks there and starts scrubbing. The
    /// overlay claims presses before the slider sees them, so this is the
    /// only path to click-to-seek.
    void timelinePress(QMouseEvent* event);
    /// Seeks the float's own core to the timeline position under `x`, in
    /// window coordinates, and moves the handle to match.
    void timelineSeekTo(double x);

    /// True when any part of the float overlaps any screen's work area. The
    /// float is placed freely, so this is the only placement rule: a float that
    /// shares no pixels with a work area cannot be clicked or dragged back.
    bool overlapsAnyWorkArea() const;
    /// Pulls the float back onto the nearest work area, keeping as much of its
    /// position as reachability allows. After a monitor change the pooled
    /// position can name a screen that is no longer there, and a window there
    /// cannot be clicked at all. Only the unreachable case is corrected; a
    /// viewer's chosen position is never overridden.
    void pullIntoWorkArea();
    void pointerPress(QMouseEvent* event);
    void pointerMove(QMouseEvent* event);
    void pointerRelease(QMouseEvent* event);
    bool handleKey(QKeyEvent* event);
    void fitSurfaces();
    void applyCornerMask();
    void revealControls();
    void hideControls();
    void setOverlayOpacity(double value);

    VideoSurface* surface_ = nullptr;
    /// The adopted chrome in paint order: filled once, in buildChrome's own
    /// construction order, and only walked while painting.
    ChromeList chrome_;
    QWidget* overlay_ = nullptr;
    IconButton* muteButton_ = nullptr;
    Panel* volumeChip_ = nullptr;
    VolumeTrack* volumeSlider_ = nullptr;
    IconButton* restoreButton_ = nullptr;
    IconButton* closeButton_ = nullptr;
    IconButton* playButton_ = nullptr;
    QSlider* scrub_ = nullptr;
    ChromeLabel* elapsed_ = nullptr;
    ChromeLabel* remaining_ = nullptr;

    QVariantAnimation* overlayFade_ = nullptr;
    QTimer* hideTimer_ = nullptr;

    /// Retained display state: what the last setPlaying() reported and the last
    /// clock setTime() rendered. A language change re-derives the labels and
    /// the clocks from these rather than reading the float's core.
    bool playing_ = false;
    double lastPosition_ = 0.0;
    double lastDuration_ = 0.0;

    double overlayOpacity_ = 1.0;
    double pendingSeek_ = -1.0;
    std::optional<bool> pendingLoadPause_;
    quint64 pendingLoadPauseGeneration_ = 0;
    bool muted_ = false;
    double volume_ = 0.7;
    bool draggingVolume_ = false;
    bool draggingTimeline_ = false;
    bool scrubbing_ = false;
    /// True while the pointer is over the float. The auto-hide has to stand
    /// down: fading the controls out from under a pointer that is reaching for
    /// one is the difference between a control and a guessing game.
    bool hovered_ = false;

    QPointF pressGlobal_;
    QPoint pressWindow_;
    QSize pressSize_;
    bool resizingRight_ = false;
    bool resizingBottom_ = false;
    bool pressed_ = false;
    bool dragging_ = false;
    bool closeRequestInProgress_ = false;
};


}  // namespace shell
}  // namespace mplayer
