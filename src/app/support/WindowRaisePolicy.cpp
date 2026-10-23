#include "WindowRaisePolicy.hpp"

#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

namespace mikai::app {

namespace {

/// Windows only. Qt's own `activateWindow()` ends at SetForegroundWindow, and
/// the shell refuses that call from a process that does not already own the
/// foreground - which is every process at launch. The result is the window the
/// user watched appear, still behind the window he was looking at. When that
/// happens the one supported way to be raised is to attach to the thread that
/// owns the foreground window and ask again from its input queue.
///
/// GUI thread only, and only for a window already mapped on screen: an unmapped
/// window has no input to answer with yet, so attaching buys nothing.
bool forceForeground(QWidget* window) {
#ifdef Q_OS_WIN
    if (window == nullptr) return false;
    HWND target = reinterpret_cast<HWND>(window->winId());
    if (target == nullptr || !IsWindow(target)) return false;
    HWND current = GetForegroundWindow();
    if (current == target) return true;

    const DWORD targetThread = GetWindowThreadProcessId(target, nullptr);
    if (targetThread == 0) return false;
    const DWORD currentThread = current != nullptr
                                    ? GetWindowThreadProcessId(current, nullptr)
                                    : 0;
    if (currentThread == 0 || currentThread == targetThread) return false;

    const DWORD thisThread = GetCurrentThreadId();
    if (!AttachThreadInput(currentThread, thisThread, TRUE)) {
        qDebug() << "mikai: raise: input attach refused for" << window->objectName();
        return false;
    }
    const bool attachedTarget =
        (targetThread != thisThread) &&
        (AttachThreadInput(currentThread, targetThread, TRUE) != FALSE);
    window->raise();
    window->activateWindow();
    SetForegroundWindow(target);
    SetActiveWindow(target);
    if (attachedTarget) {
        AttachThreadInput(currentThread, targetThread, FALSE);
    }
    AttachThreadInput(currentThread, thisThread, FALSE);
    return GetForegroundWindow() == target;
#else
    Q_UNUSED(window);
    return false;
#endif
}

/// A window the app is deliberately allowed to bring to the foreground for the
/// next few minutes: the shell only just launched us, and it wants to see our
/// window. Granting this once is what makes the later `activateWindow()` a
/// request that may be granted, instead of one that is silently dropped, and it
/// scopes to our own process only.
void grantForegroundPermission() {
#ifdef Q_OS_WIN
    AllowSetForegroundWindow(GetCurrentProcessId());
#endif
}

// One pending attempt on a window that appeared but lost the foreground race.
// A short bounded retry is what the launch case needs: the window is mapped and
// the shell has settled, so a later ask is the one that lands. The retry stops
// the moment the window owns the foreground, so a window the user has moved on
// from is never re-raised behind his back.
constexpr int kRaiseAttempts = 5;
constexpr int kRaiseDelayMs = 150;

void requestForeground(QPointer<QWidget> window, int attempt) {
    if (window.isNull()) return;
    const bool live = window->isWindow() && window->isVisible() &&
                      !QCoreApplication::closingDown() && attempt < kRaiseAttempts;
    if (!live) return;
    if (attempt > 0) {
        if (QWidget* active = QApplication::activeWindow();
            active != nullptr && active->window() != window) {
            return;
        }
    }

    window->raise();
    window->activateWindow();
    if (!window->isActiveWindow()) forceForeground(window);
    if (window->isActiveWindow()) return;

    ++attempt;
    if (attempt >= kRaiseAttempts) return;
    QPointer<QWidget> held(window);
    QTimer::singleShot(kRaiseDelayMs, window, [held, attempt] {
        if (held.isNull()) return;
        requestForeground(held, attempt);
    });
}

/// Asks for the foreground, granted to this process first so the request can be
/// answered rather than dropped, and retried once the window is mapped. The
/// immediate call is still made: on a later window inside a process that
/// already owns the foreground it succeeds on the first try and no retry is
/// ever scheduled.
void raiseWindow(QWidget* window) {
    if (window == nullptr || !window->isWindow()) return;
    window->raise();
    window->activateWindow();
    if (window->isActiveWindow()) return;

    QPointer<QWidget> held(window);
    QTimer::singleShot(0, window, [held] {
        if (held.isNull()) return;
        requestForeground(held, 0);
    });
}

}  // namespace

WindowRaisePolicy::WindowRaisePolicy(QObject* parent) : QObject(parent) {}

void WindowRaisePolicy::install() {
    QCoreApplication* application = QCoreApplication::instance();
    if (application == nullptr) return;
    // Idempotent: one live policy per process. Installing a second instance
    // would run the same Show twice per window, which is a wasted request per
    // window rather than a different result, but the promise is one instance and
    // a test that installs twice proves it.
    static WindowRaisePolicy* policy = nullptr;
    if (policy == nullptr) {
        policy = new WindowRaisePolicy(application);
    }
    application->removeEventFilter(policy);
    application->installEventFilter(policy);

    // Grant once, at install, before any window exists: the permission belongs
    // to the process, and a grant made after the window is shown is the same
    // grant arriving too late.
    grantForegroundPermission();
}

bool WindowRaisePolicy::isExempt(const QWidget* window) {
    if (window == nullptr) return true;
    if (window->windowType() == Qt::Popup || window->windowType() == Qt::ToolTip) return true;
    // The engine build overlay and any window that carries the attribute are
    // asking not to be activated; the attribute is honoured verbatim, so a
    // later WA_ShowWithoutActivating anywhere in the app keeps behaving.
    if (window->testAttribute(Qt::WA_ShowWithoutActivating)) return true;
    // A deliberately always-on-top window has already chosen its own stacking
    // (the parked PiP float); raising it over the app's ordinary windows is a
    // no-op at best and a flicker at worst.
    if (window->windowFlags().testFlag(Qt::WindowStaysOnTopHint)) return true;
    // The escape hatch for a window that must appear without activation but
    // cannot carry the attribute (it is created by a helper or by Qt itself).
    if (window->property("mikaiNoActivateOnShow").toBool()) return true;
    return false;
}

bool WindowRaisePolicy::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::Show) return false;
    auto* window = qobject_cast<QWidget*>(watched);
    if (window == nullptr) return false;
    // Only top-level windows stack: a child widget becoming visible is a layout
    // fact, not a window one, and raising it would be meaningless work on every
    // frame of a list.
    if (!window->isWindow()) return false;
    if (isExempt(window)) return false;

    // Both calls, in this order and before the exposure is delivered: `raise()`
    // alone does not move focus, and `activateWindow()` alone can be refused by
    // the platform when the window is not yet mapped - and Windows always
    // refuses it for a process that does not own the foreground, which is every
    // launch. The retry that follows is what moves a freshly launched window
    // above the app the viewer was already looking at; it stops as soon as the
    // window owns the foreground, so an already-focused window is never
    // re-raised. Qt itself returns false from both calls when the platform
    // declines, so no failure is manufactured here.
    raiseWindow(window);
    return false;
}

}  // namespace mikai::app
