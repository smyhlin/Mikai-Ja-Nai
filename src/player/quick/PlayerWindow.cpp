#include "PlayerWindow.hpp"

#include "MpvVideoItem.hpp"
#include "PlayerController.hpp"
#include "PlayerIconProvider.hpp"

#include <QCloseEvent>
#include <QEvent>
#include <QGuiApplication>
#include <QHideEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMoveEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QVariant>
#include <QStringList>
#include <QVariantMap>
#include <QWindow>

#include <algorithm>
#include <utility>

namespace mplayer {
namespace quick {

namespace {

constexpr QSize kWindowedDefaultSize{1120, 700};
constexpr QSize kWindowedMinimumSize{860, 520};
constexpr int kCompactMargin = 24;

/// Wayland hands placement, stacking and interactive moves to the compositor.
bool isWaylandPlatform() {
    return QGuiApplication::platformName().startsWith(QLatin1String("wayland"),
                                                       Qt::CaseInsensitive);
}

}  // namespace

/// Serves the one hover thumbnail to QML at `image://mikai-hover/<token>`.
///
/// The image loading thread reads the window's published token and frame under
/// this provider's mutex; the GUI thread publishes under the same mutex. A
/// request for an older token returns a null image, so a decode that finishes
/// after the pointer moved on cannot replace the current thumbnail. The window
/// detaches the provider on destruction so an in-flight request cannot touch a
/// dead window.
class HoverPreviewImageProvider : public QQuickImageProvider {
public:
    explicit HoverPreviewImageProvider(PlayerWindow* window)
        : QQuickImageProvider(QQuickImageProvider::Image), window_(window) {}

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override {
        Q_UNUSED(requestedSize);
        QMutexLocker locker(&mutex);
        if (window_ == nullptr) return QImage();
        const QString tokenText = id.section(QLatin1Char('?'), 0, 0);
        bool ok = false;
        const quint64 token = tokenText.toULongLong(&ok);
        if (!ok || token == 0 || token != window_->hoverPreviewToken_) return QImage();
        if (window_->hoverPreviewImage_.isNull()) return QImage();
        const QImage image = window_->hoverPreviewImage_;
        if (size != nullptr) *size = image.size();
        return image;
    }

    /// Called from the window destructor while the engine still owns this.
    void detachWindow() {
        QMutexLocker locker(&mutex);
        window_ = nullptr;
    }

    QMutex mutex;

private:
    PlayerWindow* window_ = nullptr;
};

PlayerWindow::PlayerWindow(PlayerController* controller, QWindow* transientParent)
    : controller_(controller) {
    setObjectName(QStringLiteral("playerWindow"));
    // A transient relationship, not a native QWindow parent and never a QWidget.
    if (transientParent != nullptr) {
        setTransientParent(transientParent);
    }
    setFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
             Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);

    configureSurface();
    applyControllerProperty();
    installHoverPreviewProvider();
    syncMinimumSize();
    restoreWindowedGeometry();

    // A renderer/context failure leaves no picture to present; the window owns
    // no other failure channel, so the controller learns through this one.
    connect(this, &QQuickWindow::sceneGraphError, this,
            [this](QQuickWindow::SceneGraphError, const QString& message) {
                emit sceneLoadFailed(message.isEmpty()
                                         ? tr("The player's render context could not be created.")
                                         : message);
            });

    if (controller_ != nullptr) {
        connect(controller_, &PlayerController::mediaChanged, this, [this]() {
            if (controller_ != nullptr) setTitle(controller_->windowTitle());
        });
        connect(controller_, &QObject::destroyed, this, [this]() { controller_ = nullptr; });
    }
}

PlayerWindow::~PlayerWindow() {
    if (auto* provider = static_cast<HoverPreviewImageProvider*>(hoverPreviewProvider_)) {
        provider->detachWindow();
    }
}

VideoSession* PlayerWindow::session() const {
    return controller_ != nullptr ? controller_->session() : nullptr;
}

void PlayerWindow::configureSurface() {
    // The scene paints the picture and the chrome; the window itself only fills
    // a letterbox with this colour, so a frame gap reads as black, not white.
    setColor(Qt::black);
    setResizeMode(QQuickView::SizeRootObjectToView);
}

void PlayerWindow::applyControllerProperty() {
    if (controller_ == nullptr) return;
    // Required root property: set before the one load path touches the module.
    setInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(controller_)}});
    setTitle(controller_->windowTitle());
}

void PlayerWindow::installHoverPreviewProvider() {
    QQmlEngine* qmlEngine = engine();
    if (qmlEngine == nullptr || hoverPreviewProvider_ != nullptr) return;
    auto* provider = new HoverPreviewImageProvider(this);
    hoverPreviewProvider_ = provider;
    qmlEngine->addImageProvider(QString::fromLatin1(kHoverPreviewProviderId), provider);
    // The scene's icon authority and request grammar are owned by the provider.
    installPlayerIcons(qmlEngine);
}

void PlayerWindow::loadPlayerScene() {
    if (sceneLoaded_ || status() != QQuickView::Null) return;

    applyControllerProperty();
    installHoverPreviewProvider();
    loadFromModule(QStringLiteral("Mikai.App"), QStringLiteral("PlayerEntry"));

    const auto errorText = [this]() {
        QStringList messages;
        const QList<QQmlError> loadErrors = errors();
        for (const QQmlError& error : loadErrors) messages.append(error.toString());
        if (messages.isEmpty()) {
            return tr("The player scene %1 could not be loaded.")
                .arg(QStringLiteral("Mikai.App/PlayerEntry"));
        }
        return messages.join(QLatin1Char('\n'));
    };

    // The render item is a child of the real root, found by objectName. There
    // is no alternate root and no second scene to fall back to.
    const auto finishScene = [this]() {
        if (sceneLoaded_) return;
        QQuickItem* root = rootObject();
        if (root == nullptr) return;
        auto* item = qobject_cast<MpvVideoItem*>(
            root->findChild<QQuickItem*>(QStringLiteral("playerVideo")));
        if (item == nullptr) {
            emit sceneLoadFailed(
                tr("The player scene is missing its required video surface."));
            return;
        }
        VideoSession* current = session();
        if (current == nullptr) {
            emit sceneLoadFailed(tr("The player session is not available."));
            return;
        }
        if (item->session() == nullptr) item->setSession(current);
        if (item->session() != current) {
            emit sceneLoadFailed(
                tr("The player video surface is attached to a different session."));
            return;
        }
        setVideoItem(item);
        sceneLoaded_ = true;
        if (showRequested_) {
            applyWindowMode();
            show();
            applyWindowMode();
            requestActivate();
        }
        emit sceneReady();
    };

    switch (status()) {
    case QQuickView::Error:
        emit sceneLoadFailed(errorText());
        break;
    case QQuickView::Null:
        emit sceneLoadFailed(tr("The player scene could not be created."));
        break;
    case QQuickView::Loading:
        connect(this, &QQuickView::statusChanged, this,
                [this, finishScene, errorText](QQuickView::Status next) {
                    if (next == QQuickView::Ready) {
                        finishScene();
                    } else if (next == QQuickView::Error && !sceneLoaded_) {
                        emit sceneLoadFailed(errorText());
                    }
                });
        break;
    case QQuickView::Ready:
        finishScene();
        break;
    }
}

void PlayerWindow::setVideoItem(MpvVideoItem* item) {
    if (videoItem_ == item) return;
    if (videoItem_ != nullptr) {
        disconnect(videoItem_, &MpvVideoItem::videoFramePresented, this,
                   &PlayerWindow::noteFramePresented);
    }
    videoItem_ = item;
    if (item == nullptr) return;

    connect(item, &MpvVideoItem::videoFramePresented, this, &PlayerWindow::noteFramePresented,
            Qt::UniqueConnection);
    connect(item, &QObject::destroyed, this, [this, item]() {
        if (videoItem_ == item) videoItem_ = nullptr;
    });

    // One decoder: the item presents this window's one session. It is never
    // replaced here, only attached when the scene did not bind it yet.
    if (VideoSession* current = session()) {
        if (item->session() == nullptr) item->setSession(current);
    }
}

void PlayerWindow::showWindowed() {
    showRequested_ = true;
    setFullscreenIntent(false);
    if (!compactPip_) windowModeRequestPending_ = true;
    if (!sceneLoaded_) loadPlayerScene();
    if (!sceneLoaded_) return;
    applyWindowMode();
    if (!isVisible()) show();
    applyWindowMode();
    requestActivate();
}

void PlayerWindow::showFullscreen() {
    showRequested_ = true;
    setFullscreenIntent(true);
    if (!compactPip_) windowModeRequestPending_ = true;
    if (!sceneLoaded_) loadPlayerScene();
    if (!sceneLoaded_) return;
    applyWindowMode();
    if (!isVisible()) show();
    applyWindowMode();
    requestActivate();
}

void PlayerWindow::hideWindow() {
    showRequested_ = false;
    if (isVisible()) hide();
}

void PlayerWindow::setFullscreenIntent(bool fullscreen) {
    if (fullscreenIntent_ == fullscreen) return;
    fullscreenIntent_ = fullscreen;
    if (!compactPip_) windowModeRequestPending_ = true;
    applyWindowMode();
    emit fullscreenChanged(fullscreen);
}

void PlayerWindow::applyWindowMode() {
    if (applyingWindowMode_) return;
    applyingWindowMode_ = true;
    if (compactPip_) {
        applyCompactPresentation();
        applyingWindowMode_ = false;
        return;
    }
    const Qt::WindowStates states = windowStates();
    const bool fullscreenNow = states.testFlag(Qt::WindowFullScreen);
    if (fullscreenIntent_) {
        if (!fullscreenNow && isVisible()) captureWindowedGeometry();
        if (!fullscreenNow) setWindowStates(states | Qt::WindowFullScreen);
    } else {
        if (fullscreenNow) setWindowStates(states & ~Qt::WindowFullScreen);
        restoreWindowedGeometry();
    }
    applyingWindowMode_ = false;
}

void PlayerWindow::setCompactPip(bool compact) {
    if (compactPip_ == compact) return;

    if (compact) {
        // Remember where the windowed player was before it shrinks; coming
        // back out restores that rectangle, not the compact one.
        captureWindowedGeometry();
        const bool fullscreenNow = windowStates().testFlag(Qt::WindowFullScreen);
        compactPip_ = true;
        syncMinimumSize();
        applyingWindowMode_ = true;
        setFlag(Qt::FramelessWindowHint, true);
        setTopmostIntent(true);
        if (fullscreenNow) {
            setWindowStates(windowStates() & ~Qt::WindowFullScreen);
        }
        applyingWindowMode_ = false;
        applyCompactPresentation();
    } else {
        compactPip_ = false;
        // Written directly: the compact rectangle must never become the
        // remembered windowed rectangle, and the restored rectangle must not be
        // captured back by the minimum-size change.
        applyingWindowMode_ = true;
        setFlag(Qt::FramelessWindowHint, false);
        setTopmostIntent(false);
        if (fullscreenIntent_) {
            syncMinimumSize();
            setWindowStates(windowStates() | Qt::WindowFullScreen);
        } else {
            restoreWindowedGeometry();
            syncMinimumSize();
        }
        applyingWindowMode_ = false;
    }
    emit compactPipChanged(compact);
}

void PlayerWindow::applyCompactPresentation() {
    syncMinimumSize();
    QScreen* targetScreen = screen();
    if (targetScreen == nullptr) targetScreen = QGuiApplication::primaryScreen();
    if (targetScreen == nullptr) {
        resize(kCompactSize);
        return;
    }
    QRect target = compactGeometry_;
    if (!target.isValid() || target.width() <= 0 || target.height() <= 0) {
        const QRect area = targetScreen->availableGeometry();
        const int width = std::min(kCompactSize.width(), std::max(1, area.width()));
        const int height = std::min(kCompactSize.height(), std::max(1, area.height()));
        target = QRect(area.right() - width - kCompactMargin + 1,
                       area.bottom() - height - kCompactMargin + 1, width, height);
    }
    // On Wayland the compositor may ignore this placement; the size still
    // applies, and the same window and session keep playing either way.
    setGeometry(clampedGeometry(target, targetScreen));
}

void PlayerWindow::setTopmostIntent(bool topmost) {
    if (topmostIntent_ == topmost) return;
    topmostIntent_ = topmost;
    if (isWaylandPlatform()) {
        // No client-side stacking on Wayland: the intent is kept so the state
        // stays truthful, but the compositor decides what is on top.
        return;
    }
    if (flags().testFlag(Qt::WindowStaysOnTopHint) != topmost) {
        setFlag(Qt::WindowStaysOnTopHint, topmost);
        if (topmost && isVisible()) raise();
    }
}

void PlayerWindow::raiseWindow() {
    if (isVisible()) raise();
}
void PlayerWindow::beginSystemMove() {
    if (!isVisible() || windowStates().testFlag(Qt::WindowFullScreen) ||
        windowStates().testFlag(Qt::WindowMaximized)) {
        return;
    }
    startSystemMove();
}

void PlayerWindow::beginSystemResize(Qt::Edges edges) {
    if (!isVisible() || windowStates().testFlag(Qt::WindowFullScreen) ||
        windowStates().testFlag(Qt::WindowMaximized)) {
        return;
    }

    constexpr int validEdges = static_cast<int>(Qt::LeftEdge) |
                               static_cast<int>(Qt::RightEdge) |
                               static_cast<int>(Qt::TopEdge) |
                               static_cast<int>(Qt::BottomEdge);
    if ((static_cast<int>(edges) & ~validEdges) != 0) return;
    const bool left = edges.testFlag(Qt::LeftEdge);
    const bool right = edges.testFlag(Qt::RightEdge);
    const bool top = edges.testFlag(Qt::TopEdge);
    const bool bottom = edges.testFlag(Qt::BottomEdge);
    const int edgeCount = static_cast<int>(left) + static_cast<int>(right) +
                          static_cast<int>(top) + static_cast<int>(bottom);
    if (edgeCount < 1 || edgeCount > 2 || (left && right) || (top && bottom)) return;
    startSystemResize(edges);
}

QRect PlayerWindow::clampedGeometry(const QRect& geometry, QScreen* targetScreen) const {
    if (!geometry.isValid()) return geometry;
    QScreen* target = targetScreen;
    if (target == nullptr) target = QGuiApplication::screenAt(geometry.center());
    if (target == nullptr) target = screen();
    if (target == nullptr) target = QGuiApplication::primaryScreen();
    if (target == nullptr) return geometry;

    const QRect area = target->availableGeometry();
    if (area.isEmpty()) return geometry;

    QRect result = geometry;
    if (result.width() > area.width()) result.setWidth(area.width());
    if (result.height() > area.height()) result.setHeight(area.height());
    const int x = std::clamp(result.x(), area.left(), area.right() - result.width() + 1);
    const int y = std::clamp(result.y(), area.top(), area.bottom() - result.height() + 1);
    result.moveTopLeft(QPoint(x, y));
    return result;
}

void PlayerWindow::setRememberedGeometry(const QRect& geometry) {
    if (geometry.isValid()) rememberedGeometry_ = geometry;
}

void PlayerWindow::setCompactGeometry(const QRect& geometry) {
    if (geometry.isValid()) compactGeometry_ = geometry;
}

void PlayerWindow::captureWindowedGeometry() {
    const Qt::WindowStates states = windowStates();
    if (!isVisible() || states.testFlag(Qt::WindowFullScreen) ||
        states.testFlag(Qt::WindowMaximized)) return;
    if (compactPip_) {
        const QRect compact = geometry();
        if (compact.isValid() && compact != compactGeometry_) compactGeometry_ = compact;
        return;
    }
    const QRect current = geometry();
    if (!current.isValid() || current == rememberedGeometry_) return;
    rememberedGeometry_ = current;
    emit windowedGeometryChanged(current);
}

void PlayerWindow::restoreWindowedGeometry() {
    QScreen* targetScreen = screen();
    if (targetScreen == nullptr) targetScreen = QGuiApplication::primaryScreen();
    QRect target = rememberedGeometry_;
    if (!target.isValid() || target.width() <= 0 || target.height() <= 0) {
        QSize size = kWindowedDefaultSize;
        if (targetScreen != nullptr) {
            const QRect area = targetScreen->availableGeometry();
            size = size.boundedTo(area.size());
            target = QRect(area.x() + (area.width() - size.width()) / 2,
                           area.y() + (area.height() - size.height()) / 2, size.width(),
                           size.height());
        } else {
            target = QRect(QPoint(0, 0), size);
        }
    }
    setGeometry(clampedGeometry(target, targetScreen));
}

void PlayerWindow::syncMinimumSize() {
    setMinimumSize(compactPip_ ? kCompactSize : kWindowedMinimumSize);
}

bool PlayerWindow::hasVisibleOutputFrame() const {
    return videoItem_ != nullptr && videoItem_->hasVisibleOutputFrame();
}

QImage PlayerWindow::grabVideoFrame(QString* reason) const {
    if (videoItem_ == nullptr) {
        if (reason != nullptr) *reason = tr("The video surface is not available.");
        return QImage();
    }
    return videoItem_->grabVideoFrame(reason);
}

void PlayerWindow::requestRepaint() {
    if (videoItem_ != nullptr) videoItem_->requestRepaint();
    update();
}

void PlayerWindow::requestRenderDiagnostics(
    RenderDiagnosticsRequest request,
    std::function<void(RenderDiagnosticsResult)> completion) {
    if (!completion) return;
    if (videoItem_ != nullptr) {
        videoItem_->requestRenderDiagnostics(request, std::move(completion));
        return;
    }

    RenderDiagnosticsResult result;
    result.error = tr("The video surface is not available.");
    QMetaObject::invokeMethod(
        this,
        [completion = std::move(completion), result = std::move(result)]() mutable {
            completion(std::move(result));
        },
        Qt::QueuedConnection);
}

void PlayerWindow::noteFramePresented() {
    emit framePresented();
}

void PlayerWindow::setHoverPreviewFrame(quint64 token, const QImage& image) {
    if (token == 0) return;
    auto* provider = static_cast<HoverPreviewImageProvider*>(hoverPreviewProvider_);
    if (provider == nullptr) {
        if (token < hoverPreviewToken_) return;
        hoverPreviewToken_ = token;
        hoverPreviewImage_ = image;
        return;
    }
    QMutexLocker locker(&provider->mutex);
    if (token < hoverPreviewToken_) return;
    hoverPreviewToken_ = token;
    hoverPreviewImage_ = image;
}

void PlayerWindow::clearHoverPreviewFrame() {
    auto* provider = static_cast<HoverPreviewImageProvider*>(hoverPreviewProvider_);
    if (provider == nullptr) {
        hoverPreviewImage_ = QImage();
        return;
    }
    QMutexLocker locker(&provider->mutex);
    hoverPreviewImage_ = QImage();
}

QUrl PlayerWindow::hoverPreviewSource() const {
    if (hoverPreviewToken_ == 0 || hoverPreviewImage_.isNull()) return QUrl();
    return QUrl(QStringLiteral("image://%1/%2")
                    .arg(QLatin1String(kHoverPreviewProviderId))
                    .arg(hoverPreviewToken_));
}

void PlayerWindow::acceptClose() {
    if (closeAccepted_) return;
    closeAccepted_ = true;
    QMetaObject::invokeMethod(this, [this]() { close(); }, Qt::QueuedConnection);
}

void PlayerWindow::showEvent(QShowEvent* event) {
    if (!sceneLoaded_ && rootObject() == nullptr) loadPlayerScene();
    QQuickView::showEvent(event);
    if (!sceneLoaded_) {
        // A failed scene must never be presented as an empty player.
        hide();
        return;
    }
    if (!videoItem_) {
        if (QQuickItem* root = rootObject()) {
            if (auto* item = qobject_cast<MpvVideoItem*>(
                    root->findChild<QQuickItem*>(QStringLiteral("playerVideo")))) {
                setVideoItem(item);
            }
        }
    }
    syncMinimumSize();
    if (!compactPip_) windowModeRequestPending_ = true;
    applyWindowMode();
}

void PlayerWindow::hideEvent(QHideEvent* event) {
    QQuickView::hideEvent(event);
}

void PlayerWindow::closeEvent(QCloseEvent* event) {
    if (!closeAccepted_) {
        // The controller answers through acceptClose(); until then the window
        // stays up and any pending work is not interrupted.
        event->ignore();
        emit closeRequested();
        return;
    }
    QQuickView::closeEvent(event);
}

void PlayerWindow::resizeEvent(QResizeEvent* event) {
    QQuickView::resizeEvent(event);
    if (!applyingWindowMode_ && isVisible()) captureWindowedGeometry();
}

void PlayerWindow::moveEvent(QMoveEvent* event) {
    QQuickView::moveEvent(event);
    if (!applyingWindowMode_ && isVisible()) captureWindowedGeometry();
}

void PlayerWindow::keyPressEvent(QKeyEvent* event) {
    QQuickView::keyPressEvent(event);
    if (event->isAccepted()) return;
    if (controller_ == nullptr) return;
    // Only keys the scene did not consume reach the one dispatch chain.
    if (controller_->handleKey(event->key(), static_cast<int>(event->modifiers()))) {
        event->accept();
    }
}

bool PlayerWindow::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::LanguageChange:
        if (controller_ != nullptr) setTitle(controller_->windowTitle());
        if (QQmlEngine* qmlEngine = engine()) qmlEngine->retranslate();
        break;
    case QEvent::WindowStateChange:
        if (!compactPip_) {
            const bool fullscreenNow = windowStates().testFlag(Qt::WindowFullScreen);
            if (windowModeRequestPending_) {
                if (isVisible() && fullscreenNow == fullscreenIntent_) {
                    windowModeRequestPending_ = false;
                } else if (!applyingWindowMode_ && isVisible()) {
                    // The native show can report its initial windowed state
                    // after a pre-show fullscreen request. Keep that explicit
                    // request authoritative until the window acknowledges it.
                    applyWindowMode();
                }
            } else if (!applyingWindowMode_ && fullscreenNow != fullscreenIntent_) {
                // After the requested mode has been acknowledged, a native
                // window action (title-bar button, platform shortcut) owns state.
                fullscreenIntent_ = fullscreenNow;
                emit fullscreenChanged(fullscreenNow);
            }
        }
        break;
    case QEvent::ScreenChangeInternal:
        if (isVisible() && !windowStates().testFlag(Qt::WindowFullScreen)) {
            const QRect clamped = clampedGeometry(geometry());
            if (clamped != geometry()) setGeometry(clamped);
        }
        break;
    case QEvent::DevicePixelRatioChange:
        // Qt resizes the backing store; ask for the next frame so the picture
        // is re-presented at the new ratio instead of being stretched.
        update();
        break;
    default:
        break;
    }
    return QQuickView::event(event);
}

}  // namespace quick
}  // namespace mplayer
