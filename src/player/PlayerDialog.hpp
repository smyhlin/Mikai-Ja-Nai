#pragma once

#include <QDialog>
#include <QEvent>
#include <QList>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

QT_BEGIN_NAMESPACE
class QCloseEvent;
class QFrame;
class QGraphicsOpacityEffect;
class QKeyEvent;
class QLabel;
class QMouseEvent;
class QMoveEvent;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QScreen;
class QSlider;
class QTimer;
class QVariantAnimation;
class QWidget;
QT_END_NAMESPACE

#include "Anime4K.hpp"

namespace mplayer {
class VideoSurface;
}

namespace mplayer {
namespace shell {

class CompareWindow;

class PlayerShell;
class MenuPopup;
class SeriesDrawer;
struct MenuEntry;

/// One playable height. A quality map entry survives only when its url is a
/// non-blank string, which is what `usableQualities` enforces.
struct QualityEntry {
    int height = 0;
    QString url;
};

/// Everything the dialog cannot do for itself. Implemented by the C API layer
/// and non-owning: it must outlive the dialog.
class PlayerHost {
public:
    virtual ~PlayerHost() = default;
    virtual void requestVariant(const QString& embed, const QString& label) = 0;
    virtual void requestEpisode(int index) = 0;
    virtual void requestTrackProbe(const QString& embed) = 0;
    virtual void requestOpenOriginal() = 0;
    virtual void requestCopyStreamUrl() = 0;
    virtual void playerClosed() = 0;
    /// The stream could not be played: the host re-extracts and reloads.
    virtual void playbackFailed() = 0;
};

/// A frameless always-on-top float showing the same stream in its own
/// VideoSurface: mpv gives one core per surface, so the float decodes a second
/// time rather than sharing a handle it cannot have.
class PipFloat : public QWidget {
    Q_OBJECT

public:
    explicit PipFloat(QWidget* parent = nullptr);

    VideoSurface* videoSurface() const { return surface_; }
    /// Opens `url` and seeks to `position` once the stream is up.
    void openStream(const QString& url, double position);
    /// Parks the float against a screen work area, bottom-right by default.
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
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildChrome();
    QPushButton* buildButton(const QString& name, const QString& hint, qreal size,
                                   QWidget* parent = nullptr);
    void updateVolumeChip();
    /// A press on the timeline seeks there and starts scrubbing. The
    /// overlay claims presses before the slider sees them, so this is the
    /// only path to click-to-seek.
    void timelinePress(QMouseEvent* event);
    /// Seeks the float's own core to the timeline position under `x`, in
    /// window coordinates, and moves the handle to match.
    void timelineSeekTo(double x);

    /// Snaps to the nearest corner of the screen the float was dropped on.
    void snapToCorner();
    /// Pulls the float back inside a work area. After a monitor change the
    /// pooled position can name a screen that is no longer there, and a window
    /// there cannot be clicked at all.
    void clampIntoWorkArea(QScreen* target);
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
    QWidget* overlay_ = nullptr;
    QPushButton* muteButton_ = nullptr;
    QWidget* volumeChip_ = nullptr;
    QSlider* volumeSlider_ = nullptr;
    QPushButton* restoreButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
    QPushButton* playButton_ = nullptr;
    QSlider* scrub_ = nullptr;
    QLabel* elapsed_ = nullptr;
    QLabel* remaining_ = nullptr;

    QGraphicsOpacityEffect* overlayEffect_ = nullptr;
    QVariantAnimation* overlayFade_ = nullptr;
    QTimer* hideTimer_ = nullptr;

    double overlayOpacity_ = 1.0;
    double pendingSeek_ = -1.0;
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
    bool pressed_ = false;
    bool dragging_ = false;
};

/// The composition root: hosts the shell, translates its intent to the engine
/// and carries the three panels the shell has no room for.
class PlayerDialog : public QDialog {
    Q_OBJECT

public:
    PlayerDialog(const QString& caption, const QString& url, QWidget* parent = nullptr);
    PlayerDialog(const QString& caption,
                 const QString& url,
                 const QList<QualityEntry>& qualities,
                 int initialHeight,
                 PlayerHost* host,
                 QWidget* parent = nullptr);
    ~PlayerDialog() override;

    // ── host → dialog ────────────────────────────────────────────────
    /// Steps to another episode in place: same window, same position.
    void applyEpisode(const QString& label,
                      const QString& embed,
                      int index,
                      const QList<QualityEntry>& qualities,
                      const QVector<QVariantMap>& variants);
    /// Swaps the dub/sub track; playback resumes at the same position.
    void applyVariant(const QList<QualityEntry>& qualities,
                      const QString& label,
                      const QString& embed = QString());
    /// Reloads from a re-extracted quality map after a failed stream load.
    void applyRefreshedStreams(const QList<QualityEntry>& qualities, int height = 0);
    void setPlaylist(const QVector<QVariantMap>& playlist, int index = 0);
    /// Publishes the dub/sub variants the popover lists and which embed is
    /// current. The track chip only appears once there is something to pick.
    void setVariants(const QVector<QVariantMap>& variants, const QString& currentEmbed);
    void stepEpisode(int delta);
    /// Badges one track with the quality a probe found for it.
    void setTrackQuality(const QString& embed, int quality);
    void showError(const QString& message);
    /// Opens the first stream; the engine exists from the shell's surface.
    void startPlayer();

    bool isPipActive() const;
    /// The height the shell is currently labelled with, 0 when unknown.
    int currentHeight() const { return currentHeight_; }
    /// The embed the track popup marks as current, for the host to read back.
    QString currentEmbed() const { return currentEmbed_; }

    void toggleSeries();
    void setSeriesExpanded(bool expanded);
    /// The dub and subtitle pickers are separate, so a viewer picks what they
    /// want by kind instead of reading one mixed list.
    void openAudioMenu();
    void openSubtitlesMenu();
    void togglePip();

signals:
    /// The engine could not be created at all: the host should provision the
    /// mpv runtime and call startPlayer() again.
    void runtimeRequested();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct VariantInfo {
        QString embed;
        QString label;
        QString display;
        QString track;
        QString kind;       // "voice" or "sub", straight from the release
        QString kindLabel;
        int quality = 0;
    };

    void copyStreamUrl();
    void buildStrip();
    void buildStatus();
    void connectShell();
    void connectEngine();

    void layoutPanels();
    void updateSeriesToggle();
    void publishQualityOptions();
    void syncStatusVisibility();
    void syncPipBar();
    void flashStatus(const QString& message);
    void showErrorStatus(const QString& message);
    void retryPlayback();
    void watchPanels();
    void placeWindowed();
    void rememberWindowedGeometry();
    void fillQualityMenu();
    void probeUnknownTracks();
    void setVariantsVisibility();
    /// Rows for one kind of track, or every kind when `kind` is empty.
    QVector<MenuEntry> trackEntries(const QString& kind) const;
    static QString trackRowText(const VariantInfo& variant);
    static QList<QualityEntry> usableQualities(const QList<QualityEntry>& qualities);
    static VariantInfo variantFromMap(const QVariantMap& map);

    void onQualityPicked(const QVariant& payload);
    void onTrackPicked(const QVariant& payload);
    void onMediaDropped(const QString& path);
    /// Puts the frame currently on screen on the clipboard as an image.
    void copyCurrentFrame();
    /// Opens (or raises) the shader comparison window.
    void openCompareWindow();
    void onShellStateChanged(const QString& state);
    void onShellFullscreenChanged(bool fullscreen);
    void onEngineError(const QString& message);
    void onEngineDuration(double seconds);
    void onEnginePosition(double seconds);
    void onEnginePlaying(bool playing);
    void onEngineVideoSize(int width, int height);
    /// Loads `url` and restores the position, pause, volume and speed the
    /// surface was carrying, so a stream swap continues instead of restarting.
    /// Returns false when the engine itself could not be created, which is the
    /// host's cue to provision the mpv runtime.
    bool openStream(const QString& url, const QString& quality, bool resume = true);
    /// Applies the saved resume point once the new stream reports a timeline.
    void restoreResumePoint();
    void goToEpisode(int position, bool closeList = false);
    void setQualities(const QList<QualityEntry>& qualities);
    /// Picks a height: the requested one when it is on offer, else the best.
    int pickTargetHeight(int requested) const;
    bool onShellKey(QKeyEvent* event);
    QPoint settingsAnchor() const;
    void enterPip();
    void leavePip(bool closing = false);

    PlayerShell* shell_ = nullptr;
    VideoSurface* surface_ = nullptr;
    PlayerHost* host_ = nullptr;
    bool directMedia_ = false;

    MenuPopup* audioPopup_ = nullptr;
    MenuPopup* subtitlesPopup_ = nullptr;
    SeriesDrawer* seriesDrawer_ = nullptr;
    QWidget* strip_ = nullptr;
    QFrame* statusPanel_ = nullptr;
    QPushButton* seriesButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* retryButton_ = nullptr;
    QTimer* statusTimer_ = nullptr;
    PipFloat* pip_ = nullptr;

    QString caption_;
    QString url_;
    QList<QualityEntry> qualities_;
    QVector<VariantInfo> variants_;
    QVector<QVariantMap> playlist_;
    QString currentEmbed_;
    int currentHeight_ = 0;
    int episodeIndex_ = 0;
    double aspect_ = 16.0 / 9.0;
    double volume_ = 0.7;      // 0..1, what the shell shows
    double speed_ = 1.0;
    anime4k::Mode anime4kMode_ = anime4k::Mode::Off;
    /// The applied chain as a candidate id. A chain with no Mode still has to
    /// survive a restart, and an enum ordinal cannot name one.
    QString anime4kChainId_ = QStringLiteral("off");
    /// The comparison window, created on first use and kept: it borrows the
    /// surface, and two of them would fight over the same core.
    CompareWindow* compare_ = nullptr;
    bool muted_ = false;
    QRect windowedSize_;
    bool fullscreen_ = true;
    bool transitioning_ = false;
    bool seriesExpanded_ = false;
    bool errorActive_ = false;
    bool refreshAttempted_ = false;
    bool closed_ = false;
    bool forwardingKey_ = false;
    bool pipWasFullscreen_ = false;
    // The float is kept, not destroyed. Each one owns a QOpenGLWidget with a
    // live mpv render context, and building or tearing one down while the
    // other is on screen faults the display driver. Re-entering PiP reuses
    // this one instead of paying that cost again.
    PipFloat* pipPool_ = nullptr;
    // What the main core was doing when PiP took over, so leaving PiP restores
    // the state rather than guessing it from a toggle.
    double pipResumePosition_ = 0.0;
    bool pipResumePlaying_ = true;
    // The resume point a stream swap holds until the new file reports a
    // duration: mpv cannot seek a stream it has not loaded yet.
    bool resumePending_ = false;
    double resumePosition_ = 0.0;
    bool resumePaused_ = false;
    // False while the surface has no mpv core at all: a retry then asks the
    // host to provision the runtime instead of reloading a dead url.
    bool engineReady_ = false;
    QVector<QObject*> panelWatchers_;
};

}  // namespace shell
}  // namespace mplayer
