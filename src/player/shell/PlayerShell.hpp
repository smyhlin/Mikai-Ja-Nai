// The lightbox shell: chrome, display state machine and input.
//
// The picture is always shown fitted: there is no zoom and no pan. Every way
// in to a scaled frame (wheel, +/-/R keys, drag, the reset menu rows) is gone,
// so a viewer cannot end up in a frame they cannot find their way out of.
//
// This is a port of mikai_ui/media_shell.py, and the port is deliberately not
// faithful in one respect. The Python shell promotes every chrome widget to a
// native HWND and re-raises it in _promote_chrome / _raise_chrome /
// _refresh_hud / _layout, because mpv used to take a native winId and a native
// child window always composites above non-native siblings. All of that is
// deleted here rather than translated, along with the cursor poll that existed
// only because mouse moves over a foreign window never reached Qt.
//
// The chrome that stands OVER the picture is not a sibling of the surface in
// the visual sense either. The surface is a QOpenGLWidget, so a control that
// painted its own backing store above it would be composited over the picture
// by the window system - a second raster pass per frame, and a stacking the
// shell cannot control. Those controls are drawn by the surface's own painter
// instead, in the same paintGL that carries mpv's frame (ChromePaint /
// ChromeHost in ShellWidgets.hpp): one raster path. What is left of them as
// widgets is what a widget is for here - a hit target, a tooltip, an accessible
// name, a slot in a layout. The popovers are the exception: they are opaque
// cards, so compositing them is not a second pass and they paint themselves.
//
// The second deliberate difference, since the port: zoom is gone entirely,
// where the Python kept a ZOOM_IN_PAN state whose _layout() moved a rectangle
// that was never actually scaled.

#pragma once

#include <QCloseEvent>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPointF>
#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QPainter;
QT_END_NAMESPACE

#include "Anime4K.hpp"
#include "ShellTheme.hpp"
#include "ShellWidgets.hpp"
#include "native/status/NativeMenu.hpp"

namespace mikai::gui {
class VideoDropTarget;
}

namespace mplayer {
class VideoSurface;
enum class StreamFpsSource : int;
namespace native {
class AnimeJanaiCatalogWatcher;
}
}

namespace mplayer {
namespace shell {

class CenterGlyph;
class KeyHints;
class MenuPopup;
class Panel;
class PillButton;
class SettingsFlyout;
class Toast;
class VolumeGroup;
class ScrubBar;
class IconButton;

class PlayerShell : public QWidget, public ChromeHost {
    Q_OBJECT

public:
    enum class State { DefaultFit, FullscreenActive, FullscreenAutoHidden };

    explicit PlayerShell(QWidget* parent = nullptr, bool frameless = true);
    ~PlayerShell() override;

    // ── painted chrome host ─────────────────────────────────────────
    // The shell owns the painter the chrome over the picture goes into, but it
    // does not own that painter: the video surface's paintGL does. The two
    // entry points below are what an adopted widget's update() and its own
    // notifications are routed to (see ShellWidgets.hpp).
    void chromeDirty(QWidget* widget, const QRect& rect) override;
    void chromeDirtyAll() override;

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
    void setAnimeJanaiSelection(const QString& id, const QString& status);
    void setAnimeJanaiBackend(const QString& backend);

    // ── native processing (the enhancement tree's native branch) ────
    /// The four inputs the native branch and its status line read, replaced as
    /// one. They are read-only here: the shell never writes a preference and
    /// never claims a capability the producer has not reported. This refreshes
    /// the open branch and the status HUD, so a change made in the settings
    /// dialog or a status reply from the filter is visible without reopening.
    void setNativeMenuInputs(const native::menu::Inputs& inputs);
    /// The inputs as currently held, for a caller that wants to read or amend
    /// one of them without reading the store again.
    const native::menu::Inputs& nativeInputs() const { return nativeInputs_; }
    /// Re-renders the native status HUD from the inputs as currently held.
    /// Public because a caller that already holds the inputs can refresh the
    /// chip without publishing them a second time.
    void updateNativeHud();
    /// Applies the catalogue preset `presetId` through the same path an
    /// enhancement-menu row takes, so a hotkey, the menu and the tree cannot
    /// drift: one toast, one mode change, one saved chain. The id is
    /// resolved against the catalogue, so an unknown id is a no-op rather than
    /// a chain nobody has. "off" is a real preset here and turns the chain off.
    void applyAnime4KPreset(const QString& presetId);
    void setStreamStats(double bytesPerSecond, double framesPerSecond,
                        StreamFpsSource fpsSource);
    void setSourceSize(int width, int height);
    void setEpisodeStepping(bool hasPrevious, bool hasNext);
    void setEpisodeLabel(const QString& label);
    QString episodeLabel() const { return episodeLabel_; }
    /// Re-reads the engine's pause state and repaints the play control, for
    /// the paths that change the core without a playingChanged arriving.
    void updatePlayIcon();
    /// Opens the picture context menu at a point in shell coordinates.
    void openPictureMenu(const QPoint& position);
    void setTrackAvailability(int audioTracks, int subtitleTracks, bool localMedia);
    void setAspectRatio(double ratio);

    State state() const;
    QString stateName() const;

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
    void animeJanaiPresetRequested(const QString& id);
    void animeJanaiBackendRequested(const QString& backend);
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
    void openFolderRequested();
    void stateChanged(const QString& state);
    /// A native row was chosen. The action is typed rather than spelled, and
    /// the value is the backend stable name or the recipe id, empty for
    /// Off/On. One handler serves the menu, the settings dialog and the toggle.
    void nativeActionRequested(native::menu::Action action, const QString& value);

protected:
    bool event(QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
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

    /// The surface's half of the chrome, connected to VideoSurface::paintOverlay
    /// with a painter in shell coordinates clipped to the picture.
    void paintSurfaceOverlay(QPainter* painter);
    /// Chrome that is nobody's widget - the chevron discs and the empty-state
    /// hint - plus the adopted controls. `clip` separates the two callers: the
    /// surface's painter passes the whole shell, the shell's paintEvent passes
    /// the window with the picture taken out.
    void paintChrome(QPainter& painter, const QRect& clip);

    void enterAutoHidden();
    void onPlayClicked();
    void onSkipForward();
    void onScrub(double fraction);
    void onVolume(double volume);
    void toggleMute();
    void onRate(double rate);
    void onAnime4KMode(anime4k::Mode mode);
    void onAnime4KPreset(const QString& presetId);
    /// Opens the enhancement tree at its root, replacing whatever was open.
    void openEnhancements();
    /// One row of the enhancement tree, either rested on (activated == false) or
    /// chosen (activated == true). A submenu row opens the next level beside
    /// this panel; a preset row is applied when it was chosen. Either way the
    /// levels below this panel go away with it.
    void onEnhancementRow(MenuPopup* panel, int spot, bool activated);
    /// Shows one catalogue level in the panel at `depth`, beside `anchor`, and
    /// closes every level below it.
    void showEnhancementLevel(int depth, const QString& levelId, QWidget* anchor);
    /// Closes every panel of the tree deeper than `depth`.
    void collapseEnhancementTree(int depth);
    /// Rebuilds the rows of every level on screen, which is how the marks
    /// follow a chain that changed underneath them.
    void refreshEnhancementTree();
    /// A relevant catalogue file changed on disk: re-enumerate the presets and
    /// redraw the levels that are open plus the footer chip. It reads no
    /// selection and writes none - a paste or a removal changes what the menu
    /// can offer, not what the viewer chose.
    void onAnimeJanaiCatalogueChanged();
    /// Closes the open levels that no longer name anything in the catalogue,
    /// deepest first, so a removed RIFE pin takes its now-empty version level
    /// with it instead of leaving a dead panel standing.
    void pruneStaleEnhancementLevels();
    /// Puts the hover filter on the rows of one panel, so resting on a row
    /// opens what sits under it.
    void watchEnhancementRows(MenuPopup* panel);
    void cancelEnhancementHover();
    bool hasVisibleEnhancementDescendant(int depth) const;
    /// The AnimeJaNai chip's own text, tooltip and accessible name, derived from
    /// the retained selection and the last status the host reported. Separated
    /// from setAnimeJanaiSelection() so a language change re-derives the chip
    /// without re-announcing a selection to the filter.
    void updateAnimeJanaiChip();
    void updateInfoChip();
    void retranslateUi();
    void openKeys();
    void buildPictureMenu();
    void updatePictureMenu();
    QString localMediaFolder() const;
    void exitViewer();
    void updateTimeLabels();
    /// Repaints the picture when the empty-state hint appears or goes away - it
    /// is painted, not a widget, so no update carries it.
    void updateEmptyState();

    QSize fitted() const;
    bool hitEdges(const QPointF& pos);
    void grabPointer();
    void releasePointer();

    VideoSurface* surface_ = nullptr;
    mikai::gui::VideoDropTarget* videoDrop_ = nullptr;
    /// Filled once, where the chrome is built; a frame only walks it.
    ChromeList chrome_;

    MenuPopup* pictureMenu_ = nullptr;
    /// Where the picture menu was opened, in shell coordinates: an open menu is
    /// re-placed against it after a language change, because a longer
    /// translation grows the card and an unrepositioned one is clamped by
    /// nothing and clipped by the window that parents it.
    QPoint pictureMenuAnchor_;
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
    IconButton* btnSkipForward_ = nullptr;
    ChromeLabel* lblElapsed_ = nullptr;
    ScrubBar* scrub_ = nullptr;
    ChromeLabel* lblRemaining_ = nullptr;
    IconButton* btnPip_ = nullptr;
    IconButton* btnWindowMode_ = nullptr;
    IconButton* btnAudio_ = nullptr;
    IconButton* btnSubs_ = nullptr;
    IconButton* btnEnhance_ = nullptr;
    IconButton* btnSettings_ = nullptr;
    SettingsFlyout* flyout_ = nullptr;
    /// The enhancement tree: one panel per level, side by side. The first is
    /// ENHANCEMENT, and every later one is the submenu of a row in the panel
    /// before it, so the tree is the prefix of this vector that is on screen.
    /// The panels are pooled and hidden rather than destroyed, because a level
    /// is reopened every time the pointer crosses the row that leads to it.
    QVector<MenuPopup*> enhancementPanels_;
    /// The catalogue level each panel shows, parallel to `enhancementPanels_`.
    /// Empty is the root, which lists the tree's own branches rather than a
    /// catalogue level.
    QStringList enhancementLevels_;
    /// Whether the tree grows to the left. Decided when the first submenu is
    /// placed, from which side of its panel has room, and held for the whole
    /// open tree: a cascade that flipped sides per level would cover itself.
    bool enhancementGrowLeft_ = false;
    QVector<SettingsFlyout::QualityOption> qualityOptions_;
    anime4k::Mode anime4kMode_ = anime4k::Mode::Off;
    QString animeJanaiPresetId_;
    /// The last status text the host reported for `animeJanaiPresetId_`, kept
    /// so the chip can be rebuilt in another language: its wording is partly
    /// ours ("Preparing…") and only the host can say what the filter is doing.
    QString animeJanaiStatus_;
    QString animeJanaiBackend_ = QStringLiteral("tensorrt");
    ChromeLabel* lblAnimeJanai_ = nullptr;
    /// Lifetime-owned observation of the catalogue the enhancement tree draws
    /// from. It is a child of the shell because the tree must follow a paste,
    /// a registration or a removal for as long as the viewer has the player
    /// open, without anyone reopening the menu to force a re-read.
    native::AnimeJanaiCatalogWatcher* animeJanaiCatalogWatcher_ = nullptr;

    Panel* footerLeft_ = nullptr;
    ChromeLabel* lblEpisode_ = nullptr;
    ChromeLabel* lblNetworkStats_ = nullptr;
    ChromeLabel* lblEnhancement_ = nullptr;
    /// The native status chip. Shown only for a loaded Native runtime, when
    /// processing is requested or the filter reports a state of its own.
    ChromeLabel* lblNative_ = nullptr;
    /// The native branch's inputs. Default-constructed to the truth of this
    /// build: nothing requested, no engine available.
    native::menu::Inputs nativeInputs_;
    KeyHints* keys_ = nullptr;

    Toast* toast_ = nullptr;
    CenterGlyph* center_ = nullptr;

    QTimer hideTimer_;
    QTimer enhancementHoverTimer_;
    QPointer<MenuPopup> pendingEnhancementHoverPanel_;
    QPointer<QWidget> pendingEnhancementHoverRow_;
    int pendingEnhancementHoverSpot_ = -1;
    QElapsedTimer sincePosition_;

    QVector<QPointer<QWidget>> overlays_;
    /// Whether a corner mask is currently installed on the surface. Tracked
    /// rather than read back from QWidget::mask(), which returns by value.
    bool hasMask_ = false;

    // canvas / fsm
    bool fullscreen_ = false;
    /// Height the title bar takes off the top of the canvas; 0 when no title
    /// bar is painted.
    int titleBarOffset_ = 0;
    bool hudHidden_ = false;
    /// Tracked so only the hint's own transitions repaint the picture.
    bool emptyStateShown_ = false;
    bool rounded_ = true;
    double aspect_ = kAspectFallback;
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
    StreamFpsSource streamFpsSource_ = static_cast<StreamFpsSource>(0);
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    int outputWidth_ = 0;
    int outputHeight_ = 0;
    QString episodeLabel_;

    // pointer gesture
    QPointF press_;
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
