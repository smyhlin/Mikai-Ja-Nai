// The lightbox shell: chrome, display state machine, zoom/pan and input.
//
// This is a port of mikai_ui/media_shell.py, and the port is deliberately not
// faithful in one respect. The Python shell promotes every chrome widget to a
// native HWND and re-raises it in _promote_chrome / _raise_chrome /
// _refresh_hud / _layout, because mpv used to take a native winId and a native
// child window always composites above non-native siblings. VideoSurface is now
// a QOpenGLWidget, so none of that is needed: the chrome is ordinary sibling
// widgets and z-order is Qt's problem. All of it is deleted here rather than
// translated, along with the cursor poll that existed only because mouse moves
// over a foreign window never reached Qt.
//
// The other deliberate difference: zoom actually scales the surface. In the
// Python, _layout() positions the surface at the *fitted* size plus the pan
// offset, so ZOOM_IN_PAN moves a rectangle that was never scaled - the state,
// the cursor shape, the "drag to pan" hint and _clamp()'s scaled bounds all
// assume a scaled rect, and the surface is the only thing not scaled.

#pragma once

#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPointF>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>
#include <QRect>
#include <QString>
#include <QTimer>
#include <QWidget>

#include "Anime4K.hpp"
#include "ShellTheme.hpp"
#include "ShellWidgets.hpp"

namespace mplayer {
class VideoSurface;
}

namespace mplayer {
namespace shell {

class CenterGlyph;
class KeyHints;
class MenuPopup;
class Panel;
class PillButton;
class SettingsFlyout;
class StateHUD;
class Toast;
class VolumeGroup;
class ScrubBar;
class IconButton;

class PlayerShell : public QWidget {
    Q_OBJECT

public:
    enum class State { DefaultFit, ScrollOut, ZoomInPan, FullscreenActive, FullscreenAutoHidden };

    explicit PlayerShell(QWidget* parent = nullptr, bool frameless = true);
    ~PlayerShell() override;

    /// The widget mpv renders into. It is a plain QOpenGLWidget child: the host
    /// hands the URL to VideoSurface::open(), there is no winId() handshake.
    VideoSurface* videoSurface() const { return surface_; }

    // ── host → shell ────────────────────────────────────────────────
    void setMedia(const QString& path, const QString& origin = {}, const QString& quality = {});
    void setDuration(double seconds);
    void setPosition(double seconds);
    void setPlaying(bool playing);
    void setVolume(double volume, bool fromHost = false);
    void setMuted(bool muted);
    void setRate(double rate);
    void setQuality(const QString& quality);
    /// The heights the settings popover offers, in the host's own naming.
    void setQualityOptions(const QVector<SettingsFlyout::QualityOption>& options);
    void setAnime4KMode(anime4k::Mode mode);
    void setStreamStats(double bytesPerSecond, double framesPerSecond);
    void setSourceSize(int width, int height);
    void setEpisodeStepping(bool hasPrevious, bool hasNext);
    /// Re-reads the engine's pause state and repaints the play control, for
    /// the paths that change the core without a playingChanged arriving.
    void updatePlayIcon();
    /// Opens the picture context menu at a point in shell coordinates.
    void openPictureMenu(const QPoint& position);
    /// The dub and subtitle pickers only appear when there is a choice.
    void setAudioAvailable(bool available);
    void setSubtitlesAvailable(bool available);
    void setAspectRatio(double ratio);

    State state() const;
    QString stateName() const;

    // ── canvas transform ────────────────────────────────────────────
    void resetTransform();
    void zoomAt(double target, const QPointF& anchor = QPointF());
    void zoomBy(double factor, const QPointF& anchor = QPointF());
    void toggleHud();

    /// The dialog is the shell's host and drives both of these directly:
    /// fullscreen is a composition decision, and wake() is what re-arms the
    /// auto-hide when the pointer is over the feature strip, the status
    /// panel or the series drawer - without it the drawer vanishes under the
    /// pointer, because no move ever crosses the shell itself.
    /// enterFullscreen()/leaveFullscreen() only sync the shell's composition:
    /// the host has already moved its own window, so they emit nothing.
    void enterFullscreen();
    /// Windowed mode. Only a standalone frameless shell paints a header for
    /// it; a hosted shell has an OS frame.
    void leaveFullscreen();
    /// The only intent path: every window-mode gesture lands here and emits
    /// exactly one fullscreenChanged(desired).
    void windowModeToggled();
    void buildTitleBar();

    /// Opens the speed/quality popover. The host owns the quality list inside it.
    void openSettings();
    /// Closes every floating overlay in the player window and reports whether
    /// one was visible.
    bool dismissOverlay();
    /// Adds a host-owned floating overlay to the set dismissOverlay() closes
    /// and hasVisibleOverlay() reports on. The shell registers its own
    /// popovers; the dialog's dub and subtitle pickers are children of the
    /// dialog, built after the shell exists, so it cannot know them.
    void registerOverlay(QWidget* overlay);
    /// Whether any registered overlay is on screen. The host asks this instead
    /// of walking its own subtree for popovers.
    bool hasVisibleOverlay() const;
    void wake();
signals:
    // ── shell → host ────────────────────────────────────────────────
    void playPauseRequested();
    void seekRequested(double seconds);
    void rateChanged(double rate);
    void anime4KModeChanged(anime4k::Mode mode);
    void volumeChanged(double volume);
    void muteChanged(bool muted);
    void fullscreenChanged(bool fullscreen);
    void pipRequested();
    void audioRequested();
    void subtitlesRequested();
    void qualityRequested(int height);
    void nextRequested();
    void prevRequested();
    void downloadRequested();
    void openOriginalRequested();
    void mediaDropped(const QString& path);
    void closeRequested();
    /// A right-click on the picture asked for the current frame. The shell owns
    /// no clipboard and no image, so the host reads the surface and copies it.
    void frameRequested();
    /// The viewer asked for the shader comparison window. The host owns the
    /// window: it is the only side that knows the live surface and where a
    /// preference would be stored.
    void compareRequested();
    void copyLinkRequested();
    void stateChanged(const QString& state);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildSurface();
    void buildWindowControls();
    void buildChevrons();
    void buildDock();
    void buildFooter();
    void buildOverlays();

    void applyState();
    void setStateCursor();
    void layoutChrome();

    void enterAutoHidden();
    void onPlayClicked();
    void onScrub(double fraction);
    void onVolume(double volume);
    void toggleMute();
    void onRate(double rate);
    void onAnime4KMode(anime4k::Mode mode);
    void openEnhancements();
    void openAnime4K();
    void refreshEnhancementMenus();
    void updateInfoChip();
    void openKeys();
    void openMenu();
    void buildPictureMenu();
    void exitViewer();
    void updateTimeLabels();

    QSize fitted() const;
    QPointF clampOffset(double scale, const QPointF& offset) const;
    bool hitEdges(const QPointF& pos);
    void grabPointer();
    void releasePointer();

    VideoSurface* surface_ = nullptr;

    MenuPopup* pictureMenu_ = nullptr;
    Panel* windowControls_ = nullptr;
    Panel* titleBar_ = nullptr;
    IconButton* titleMin_ = nullptr;
    IconButton* titleMax_ = nullptr;
    IconButton* titleClose_ = nullptr;
    PillButton* btnOriginal_ = nullptr;
    IconButton* btnMin_ = nullptr;
    IconButton* btnClose_ = nullptr;

    IconButton* btnPrev_ = nullptr;
    IconButton* btnNext_ = nullptr;

    Panel* dock_ = nullptr;
    VolumeGroup* vol_ = nullptr;
    IconButton* btnPlay_ = nullptr;
    QLabel* lblElapsed_ = nullptr;
    ScrubBar* scrub_ = nullptr;
    QLabel* lblRemaining_ = nullptr;
    IconButton* btnPip_ = nullptr;
    IconButton* btnWindowMode_ = nullptr;
    IconButton* btnAudio_ = nullptr;
    IconButton* btnSubs_ = nullptr;
    IconButton* btnEnhance_ = nullptr;
    IconButton* btnSettings_ = nullptr;
    SettingsFlyout* flyout_ = nullptr;
    MenuPopup* enhancementsPopup_ = nullptr;
    MenuPopup* anime4kPopup_ = nullptr;
    /// The Anime4K row inside enhancementsPopup_, rebuilt on every refresh.
    /// Hovering it opens the submenu; the popover itself stays click-only.
    QWidget* anime4kRow_ = nullptr;
    QVector<SettingsFlyout::QualityOption> qualityOptions_;
    anime4k::Mode anime4kMode_ = anime4k::Mode::Off;

    Panel* footerLeft_ = nullptr;
    QLabel* lblNetworkStats_ = nullptr;
    QLabel* lblEnhancement_ = nullptr;
    Panel* footerRight_ = nullptr;
    IconButton* btnKeys_ = nullptr;
    IconButton* btnMore_ = nullptr;
    KeyHints* keys_ = nullptr;
    MenuPopup* morePopup_ = nullptr;

    StateHUD* hud_ = nullptr;
    Toast* toast_ = nullptr;
    CenterGlyph* center_ = nullptr;

    QTimer hideTimer_;
    QElapsedTimer sincePosition_;

    /// Every floating overlay in the player window: the four popovers, the
    /// settings flyout, the shortcut card, plus whatever the host registers.
    /// dismissOverlay()/hasVisibleOverlay() used to find these by walking the
    /// whole widget tree and asking each node for a dynamic property, which
    /// on a long playlist means a 150-widget walk several times a second.
    ///
    /// Held raw because every entry is a child widget that outlives its own
    /// registration: the shell's belong to the shell, the dialog's to the
    /// dialog, and neither outlives the other in the hosted player.
    QVector<QWidget*> overlays_;
    /// Whether a corner mask is currently installed on the surface. Tracked
    /// rather than read back from QWidget::mask(), which returns by value.
    bool hasMask_ = false;

    // canvas / fsm
    double scale_ = 1.0;
    QPointF offset_;
    bool panning_ = false;
    bool fullscreen_ = false;
    /// Height the title bar takes off the top of the canvas; 0 when no title
    /// bar is painted.
    int titleBarOffset_ = 0;
    bool hudHidden_ = false;
    bool rounded_ = true;
    double aspect_ = kAspectFallback;
    bool showHud_ = false;
    /// True only for a standalone frameless top-level shell, which must supply
    /// its own header. A hosted shell uses the OS frame.
    bool frameless_ = true;
    QString lastState_;

    // media
    QString path_;
    double duration_ = 0.0;
    double position_ = 0.0;
    bool playing_ = false;
    double volume_ = 0.7;
    bool muted_ = false;
    double rate_ = 1.0;
    QString quality_;
    QString origin_;
    bool hasMedia_ = false;
    bool hasPreviousEpisode_ = false;
    bool hasNextEpisode_ = false;
    double networkBytesPerSecond_ = 0.0;
    double streamFramesPerSecond_ = 0.0;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    int outputWidth_ = 0;
    int outputHeight_ = 0;

    // pointer gesture
    QPointF press_;
    QPointF pressOffset_;
    bool moved_ = false;
    QRect dragGeometry_;
    QPoint dragOrigin_;
    bool resizingLeft_ = false;
    bool resizingTop_ = false;
    bool resizingRight_ = false;
    bool resizingBottom_ = false;
    bool grabbed_ = false;
    QPoint lastMousePos_;
};

}  // namespace shell
}  // namespace mplayer
