// The player's one native Qt Quick window.
//
// A real QQuickWindow (QQuickView) presenting the OpenGL scene: the video item
// wraps mpv's GPU output and every control is a QML item in the same scene.
// No QQuickWidget, no winId embedding, no QWidget container.
//
// The root is the executable module's own `Mikai.App/PlayerEntry`, which
// inherits `Mikai.Player/PlayerView` and takes this session's PlayerController
// as its required `controller` property. `loadFromModule` is the one load path;
// a failure is reported, never substituted with another root.
//
// Owns fullscreen/windowed intent and the remembered geometries, the
// same-window compact PiP presentation (resize + topmost intent only; the
// session is unchanged), native move/resize, frame-presented and overlay-free
// capture hooks, one hover-thumbnail image provider, and close delegation to
// PlayerController::requestClose().

#pragma once

#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include <QUrl>

#include <Qt>

#include "../core/RenderDiagnostics.hpp"

#include <QQuickView>

#include <functional>

QT_BEGIN_NAMESPACE
class QCloseEvent;
class QHideEvent;
class QKeyEvent;
class QMoveEvent;
class QResizeEvent;
class QScreen;
class QShowEvent;
class QQuickImageProvider;
QT_END_NAMESPACE

namespace mplayer {

class VideoSession;

namespace quick {

class MpvVideoItem;
class PlayerController;

class PlayerWindow : public QQuickView {
    Q_OBJECT

public:
    /// Initial compact PiP size in device-independent pixels.
    static constexpr QSize kCompactSize{480, 270};
    /// Image provider id: `image://mikai-hover/<token>`.
    static constexpr const char* kHoverPreviewProviderId = "mikai-hover";

    explicit PlayerWindow(PlayerController* controller, QWindow* transientParent = nullptr);
    ~PlayerWindow() override;

    PlayerController* controller() const { return controller_; }
    VideoSession* session() const;

    /// Loads `Mikai.App/PlayerEntry` through `loadFromModule` with `controller`
    /// as the required initial property. A failure emits sceneLoadFailed and
    /// leaves the window unshown.
    void loadPlayerScene();

    /// The render item: resolved from the scene tree once Ready, or supplied by
    /// a view owner (tests/probes).
    MpvVideoItem* videoItem() const { return videoItem_; }
    void setVideoItem(MpvVideoItem* item);

    void showWindowed();
    void showFullscreen();
    void hideWindow();

    bool fullscreenIntent() const { return fullscreenIntent_; }
    void setFullscreenIntent(bool fullscreen);
    void applyWindowMode();

    bool compactPip() const { return compactPip_; }
    /// Same window and session; never reopens, seeks or touches native state.
    void setCompactPip(bool compact);

    /// Wayland may leave stacking to the compositor; the intent is still kept.
    bool topmostIntent() const { return topmostIntent_; }
    void setTopmostIntent(bool topmost);
    void raiseWindow();

    /// Separate boxes so neither presentation overwrites the other.
    QRect rememberedGeometry() const { return rememberedGeometry_; }
    void setRememberedGeometry(const QRect& geometry);
    QRect compactGeometry() const { return compactGeometry_; }
    void setCompactGeometry(const QRect& geometry);
    QRect clampedGeometry(const QRect& geometry, QScreen* screen = nullptr) const;

    void beginSystemMove();
    void beginSystemResize(Qt::Edges edges);

    bool hasVisibleOutputFrame() const;
    /// The video item's own render target, never QQuickWindow::grabWindow().
    QImage grabVideoFrame(QString* reason = nullptr) const;
    void requestRepaint();
    /// Requests an immutable diagnostic snapshot from the active render item.
    /// Completion is queued by the render item; this window never makes a
    /// graphics context current or performs GL readback itself.
    void requestRenderDiagnostics(
        RenderDiagnosticsRequest request,
        std::function<void(RenderDiagnosticsResult)> completion);

    /// Called by the video item after compositing; emits framePresented once.
    void noteFramePresented();

    /// Stores the one hover thumbnail for `token` and republishes the source
    /// URL. Stale tokens are ignored, so a late decode cannot replace a newer
    /// frame. No per-video-frame work happens here.
    void setHoverPreviewFrame(quint64 token, const QImage& image);
    void clearHoverPreviewFrame();
    /// `image://mikai-hover/<token>`, empty when no frame is stored. A new token
    /// changes the URL, which is what invalidates the image loader's cache.
    QUrl hoverPreviewSource() const;

    /// Accepts only the close the controller's acknowledged teardown approved.
    void acceptClose();

signals:
    void framePresented();
    /// A user close request; the controller answers with acceptClose() or
    /// leaves the window up when the close was refused.
    void closeRequested();
    void compactPipChanged(bool compact);
    void fullscreenChanged(bool fullscreen);
    void windowedGeometryChanged(const QRect& geometry);
    void sceneReady();
    void sceneLoadFailed(const QString& error);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    /// Only keys the QML scene did not accept reach the controller's chain.
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;

private:
    friend class HoverPreviewImageProvider;

    void configureSurface();
    void applyControllerProperty();
    void installHoverPreviewProvider();
    void applyCompactPresentation();
    void captureWindowedGeometry();
    void restoreWindowedGeometry();
    void syncMinimumSize();

    PlayerController* controller_ = nullptr;
    MpvVideoItem* videoItem_ = nullptr;
    QQuickImageProvider* hoverPreviewProvider_ = nullptr;
    QImage hoverPreviewImage_;
    quint64 hoverPreviewToken_ = 0;
    QRect rememberedGeometry_;
    QRect compactGeometry_;
    bool fullscreenIntent_ = true;
    bool compactPip_ = false;
    bool topmostIntent_ = false;
    bool closeAccepted_ = false;
    bool sceneLoaded_ = false;
    bool showRequested_ = false;
    bool applyingWindowMode_ = false;
    bool windowModeRequestPending_ = false;
};

}  // namespace quick
}  // namespace mplayer
