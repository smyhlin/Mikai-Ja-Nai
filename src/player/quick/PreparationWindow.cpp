#include "PreparationWindow.hpp"
#include "PlayerController.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QDebug>
#include <QEvent>
#include <QGuiApplication>
#include <QHideEvent>
#include <QQuickItem>
#include <QScreen>
#include <QShowEvent>
#include <QQmlEngine>
#include <QTimer>
#include <QToolTip>
#include <QVariantMap>

#include <QtMath>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

namespace mplayer {
namespace quick {

namespace {

/// Same anchor the Widgets overlay used: centred on the player's screen, 80px
/// below its top edge, so the viewer's eye lands there with the player behind.
constexpr int kAnchorTopOffset = 80;
constexpr int kSuccessReadTimeMs = 1500;
constexpr int kStackingPollMs = 250;

}  // namespace

PreparationWindow::PreparationWindow(PlayerController* controller, QWindow* transientParent)
    : controller_(controller) {
    setTitle(tr("Engine preparation"));
    // Tool + frameless + topmost is the Widgets overlay's working recipe. The
    // missing piece for a QWindow is focus: a status window must never take
    // activation away from the player, so the platform gets a no-activate
    // window and its clicks still arrive.
    setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
             | Qt::WindowDoesNotAcceptFocus);
    if (transientParent != nullptr) setTransientParent(transientParent);
    // The card's rounded corners need the window's own background to be clear;
    // the QML root paints the surface and border.
    setColor(Qt::transparent);
    setResizeMode(QQuickView::SizeViewToRootObject);

    finishTimer_ = new QTimer(this);
    finishTimer_->setSingleShot(true);
    finishTimer_->setInterval(kSuccessReadTimeMs);
    connect(finishTimer_, &QTimer::timeout, this, [this] { hide(); });

    elapsedTimer_ = new QTimer(this);
    elapsedTimer_->setInterval(1000);
    connect(elapsedTimer_, &QTimer::timeout, this, [this] {
        const int seconds = static_cast<int>(elapsed_.elapsed() / 1000);
        if (seconds == elapsedSeconds_) return;
        elapsedSeconds_ = seconds;
        emit elapsedChanged();
    });

    // Windows lets a competing topmost window cover this tool; X11 window
    // managers can do the same. Reassert only where raising is observable and
    // non-activating. Wayland owns stacking and must not be polled with futile
    // raise requests.
    const QString platform = QGuiApplication::platformName();
#ifdef Q_OS_WIN
    const bool canReassertStacking = platform == QStringLiteral("windows");
#else
    const bool canReassertStacking = platform == QStringLiteral("xcb");
#endif
    if (canReassertStacking) {
        stackingTimer_ = new QTimer(this);
        stackingTimer_->setInterval(kStackingPollMs);
        connect(stackingTimer_, &QTimer::timeout, this, &PreparationWindow::maintainTopmost);
    }
}

PreparationWindow::~PreparationWindow() {
    tearingDown_ = true;
    stopStackingTimer();
    if (finishTimer_ != nullptr) finishTimer_->stop();
    if (elapsedTimer_ != nullptr) elapsedTimer_->stop();
    if (isVisible()) hide();
}

void PreparationWindow::loadView() {
    if (rootObject() != nullptr) return;
    setInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(controller_)},
        {QStringLiteral("view"), QVariant::fromValue<QObject*>(this)},
    });
    loadFromModule(QStringLiteral("Mikai.Player"), QStringLiteral("PreparationView"));
    if (rootObject() == nullptr) {
        qWarning() << "PreparationWindow could not load Mikai.Player/PreparationView; "
                      "the preparation status stays hidden";
    }
}

void PreparationWindow::setRun(Run run) {
    if (run_ == run) return;
    run_ = run;
    emit stateChanged();
}

void PreparationWindow::present() {
    if (tearingDown_ || controller_ == nullptr) return;
    loadView();
    if (rootObject() == nullptr) return;
    finishTimer_->stop();
    elapsedSeconds_ = 0;
    elapsed_.restart();
    elapsedTimer_->start();
    emit elapsedChanged();
    setRun(Run::Running);
    if (!isVisible()) {
        // Size-to-root already computed the card; place it before it is mapped
        // so the first frame does not jump across the screen.
        if (rootObject()->width() > 0.0 && rootObject()->height() > 0.0) {
            resize(qCeil(rootObject()->width()), qCeil(rootObject()->height()));
        }
        positionNearParent();
        show();
    }
    maintainTopmost();
    if (stackingTimer_ != nullptr && !stackingTimer_->isActive()) stackingTimer_->start();
}

void PreparationWindow::finish(bool ok) {
    if (run_ == Run::Idle) {
        // The scene never loaded, so no presentation exists to settle. The
        // owner still learns the run ended.
        if (!tearingDown_) emit dismissed();
        return;
    }
    if (run_ != Run::Running) return;
    setRun(ok ? Run::Succeeded : Run::Failed);
    if (elapsedTimer_ != nullptr) elapsedTimer_->stop();
    if (ok) {
        finishTimer_->start();
        return;
    }
    // A failure stays until the viewer dismisses it, always above the player.
    if (isVisible()) maintainTopmost();
}

void PreparationWindow::dismiss() {
    if (finishTimer_ != nullptr) finishTimer_->stop();
    if (elapsedTimer_ != nullptr) elapsedTimer_->stop();
    stopStackingTimer();
    if (isVisible()) {
        hide();
        return;
    }
    if (run_ == Run::Idle) return;
    setRun(Run::Idle);
    if (!tearingDown_) emit dismissed();
}

bool PreparationWindow::stackingPollActive() const {
    return stackingTimer_ != nullptr && stackingTimer_->isActive();
}

void PreparationWindow::showEvent(QShowEvent* event) {
    QQuickView::showEvent(event);
    if (run_ != Run::Idle) syncStackingTimer();
}

void PreparationWindow::hideEvent(QHideEvent* event) {
    if (finishTimer_ != nullptr) finishTimer_->stop();
    if (elapsedTimer_ != nullptr) elapsedTimer_->stop();
    stopStackingTimer();
    const bool hadRun = run_ != Run::Idle;
    QQuickView::hideEvent(event);
    setRun(Run::Idle);
    if (hadRun && !tearingDown_) emit dismissed();
}

void PreparationWindow::closeEvent(QCloseEvent* event) {
    // There is no title bar, but a window manager shortcut can still ask. The
    // answer is the same as the window's own Dismiss action, never a closed
    // window that cannot be shown again.
    event->ignore();
    dismiss();
}

bool PreparationWindow::event(QEvent* event) {
    const bool languageChange = event->type() == QEvent::LanguageChange;
    const bool handled = QQuickView::event(event);
    if (languageChange) {
        setTitle(tr("Engine preparation"));
        if (QQmlEngine* qmlEngine = engine()) qmlEngine->retranslate();
    }
    return handled;
}

void PreparationWindow::stopStackingTimer() {
    if (stackingTimer_ == nullptr || !stackingTimer_->isActive()) return;
    stackingTimer_->stop();
}

void PreparationWindow::syncStackingTimer() {
    if (stackingTimer_ == nullptr) return;
    if (isVisible() && run_ != Run::Idle) {
        maintainTopmost();
        if (!stackingTimer_->isActive()) stackingTimer_->start();
        return;
    }
    stopStackingTimer();
}

void PreparationWindow::maintainTopmost() {
    if (!isVisible()) {
        stopStackingTimer();
        return;
    }
    // A popup or tooltip the viewer is reading outranks this window's band.
    // Re-asserting topmost here while one is on screen would move this window
    // above it, because the last topmost request wins. The timer keeps running,
    // so the band is taken back the moment the popup closes.
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        if (app->activePopupWidget() != nullptr || QToolTip::isVisible()) return;
        // A Widget modal box cannot outrank a topmost window, so the band is
        // given up while one is up and taken back once it is answered.
        if (app->activeModalWidget() != nullptr) {
            yieldForModal();
            return;
        }
    }
#ifdef Q_OS_WIN
    const HWND window = reinterpret_cast<HWND>(winId());
    if (window == nullptr) return;
    if (yieldedForModal_) {
        yieldedForModal_ = false;
    } else if ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
        // The topmost flag only identifies the band; another topmost HWND may
        // still precede this one in z-order. Reassert only when that is true.
        bool coveredByTopmost = false;
        for (HWND above = GetWindow(window, GW_HWNDPREV);
             above != nullptr; above = GetWindow(above, GW_HWNDPREV)) {
            if (IsWindowVisible(above)
                && (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
                coveredByTopmost = true;
                break;
            }
        }
        if (!coveredByTopmost) {
            stackingFailureReported_ = false;
            return;
        }
    }
    if (SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
        stackingFailureReported_ = false;
    } else if (!stackingFailureReported_) {
        const DWORD error = GetLastError();
        qWarning() << "PreparationWindow could not reassert HWND_TOPMOST for HWND"
                   << static_cast<void*>(window) << "; Win32 error" << error;
        stackingFailureReported_ = true;
    }
#else
    // QWindow::raise() is non-activating on X11; the timer is not created on
    // Wayland, where the compositor alone controls global stacking.
    raise();
#endif
}

void PreparationWindow::yieldForModal() {
    if (yieldedForModal_) return;
#ifdef Q_OS_WIN
    const HWND window = reinterpret_cast<HWND>(winId());
    if (window != nullptr) {
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
#endif
    // X11 stacking is raised, not banded: leaving raise() out of the next tick
    // is the whole yield.
    yieldedForModal_ = true;
}

void PreparationWindow::positionNearParent() {
    QScreen* screen = nullptr;
    if (transientParent() != nullptr) screen = transientParent()->screen();
    if (screen == nullptr) screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) return;
    const QRect area = screen->availableGeometry();
    const int w = qMax(width(), 1);
    const int h = qMax(height(), 1);
    const int x = area.x() + (area.width() - w) / 2;
    const int y = area.y() + kAnchorTopOffset;
    setPosition(QPoint(qBound(area.left(), x, area.left() + qMax(0, area.width() - w)),
                       qBound(area.top(), y, area.top() + qMax(0, area.height() - h))));
}

}  // namespace quick
}  // namespace mplayer
