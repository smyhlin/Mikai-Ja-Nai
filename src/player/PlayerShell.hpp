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
    /// Reflects the persisted choice back into the popover, so the ✓ sits on
    /// the row that is actually running.
    void setAnime4KMode(anime4k::Mode mode);
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
    void enterFullscreen();

    /// Opens the speed/quality popover. The host owns the quality list inside it.
    void openSettings();
    /// Closes whichever of the shell's own overlays is up (settings popover,
    /// key hints) and reports whether one was. Esc has to be able to reach
    /// these: both are Qt::Popup windows, so while one is open it holds the
    /// keyboard and the host never sees the key at all.
    bool dismissOverlay();
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

    void exitFullscreen();
    void enterAutoHidden();
    void onPlayClicked();
    void onScrub(double fraction);
    void onVolume(double volume);
    void toggleMute();
    void onRate(double rate);
    void onAnime4KMode(anime4k::Mode mode);
    void openKeys();
    void openMenu();
    void exitViewer();
    void updateTimeLabels();

    QSize fitted() const;
    QPointF clampOffset(double scale, const QPointF& offset) const;
    bool hitEdges(const QPointF& pos);
    void grabPointer();
    void releasePointer();

    VideoSurface* surface_ = nullptr;

    Panel* windowControls_ = nullptr;
    PillButton* btnOriginal_ = nullptr;
    IconButton* btnMin_ = nullptr;
    IconButton* btnWin_ = nullptr;
    IconButton* btnClose_ = nullptr;

    IconButton* btnPrev_ = nullptr;
    IconButton* btnNext_ = nullptr;

    Panel* dock_ = nullptr;
    VolumeGroup* vol_ = nullptr;
    IconButton* btnPlay_ = nullptr;
    QLabel* lblElapsed_ = nullptr;
    ScrubBar* scrub_ = nullptr;
    QLabel* lblRemaining_ = nullptr;
    IconButton* btnFullscreen_ = nullptr;
    IconButton* btnPip_ = nullptr;
    IconButton* btnAudio_ = nullptr;
    IconButton* btnSubs_ = nullptr;
    IconButton* btnSettings_ = nullptr;
    SettingsFlyout* flyout_ = nullptr;
    QVector<SettingsFlyout::QualityOption> qualityOptions_;
    anime4k::Mode anime4kMode_ = anime4k::Mode::Off;

    QWidget* footerLeft_ = nullptr;
    QLabel* lblFile_ = nullptr;
    QLabel* lblOrigin_ = nullptr;
    Panel* footerRight_ = nullptr;
    IconButton* btnReset_ = nullptr;
    IconButton* btnKeys_ = nullptr;
    IconButton* btnMore_ = nullptr;
    KeyHints* keys_ = nullptr;

    StateHUD* hud_ = nullptr;
    Toast* toast_ = nullptr;
    CenterGlyph* center_ = nullptr;

    QTimer hideTimer_;
    QElapsedTimer sincePosition_;

    // canvas / fsm
    double scale_ = 1.0;
    QPointF offset_;
    bool panning_ = false;
    bool fullscreen_ = false;
    bool hudHidden_ = false;
    bool rounded_ = true;
    double aspect_ = kAspectFallback;
    bool showHud_ = false;
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
