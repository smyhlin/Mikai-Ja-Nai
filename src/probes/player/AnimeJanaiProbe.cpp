#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QOpenGLWidget>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QCursor>
#include <QPointer>
#include <QPushButton>
#include <QProgressBar>
#include <QTimer>
#include <QTest>
#include <QRegularExpression>
#include <QScreen>
#include <QDialog>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

#include "VideoSurface.hpp"
#include "PlayerDialog.hpp"
#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
#include "ShellTheme.hpp"
#include "ShellWidgets.hpp"
#include "native/bundle/NativePrefs.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"
#include "native/components/NativeComponentStore.hpp"
#include "native/bundle/NativePaths.hpp"
#include "native/models/NativeSpatialAssets.hpp"
#include "native/models/NativeRife.hpp"
#include "ProbeRuntime.hpp"

using namespace mplayer;
using mplayer::shell::PlayerDialog;

namespace {

QMutex logMutex;
QStringList observedLogs;

QStringList capturedLogs() {
    QMutexLocker lock(&logMutex);
    return observedLogs;
}

QStringList capturedLogsSince(int first) {
    const QStringList logs = capturedLogs();
    return logs.mid(qBound(0, first, logs.size()));
}

void captureLog(QtMsgType type, const QMessageLogContext&, const QString& message) {
    Q_UNUSED(type);
    {
        QMutexLocker lock(&logMutex);
        observedLogs.append(message);
    }
    std::fprintf(stderr, "%s\n", qPrintable(message));
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        if (predicate()) return true;
        QThread::msleep(20);
    }
    return predicate();
}

QWidget* deepestChildAt(QWidget* root, const QPoint& global) {
    QWidget* child = root;
    for (;;) {
        QWidget* next = child->childAt(child->mapFromGlobal(global));
        if (next == nullptr) return child == root ? nullptr : child;
        child = next;
    }
}

#ifdef Q_OS_WIN
bool nativeCursorPosition(QPoint* position) {
    POINT point{};
    SetLastError(ERROR_SUCCESS);
    if (!GetCursorPos(&point)) {
        std::fprintf(stderr, "FAIL native GetCursorPos denied: lastError=%lu\n", GetLastError());
        return false;
    }
    *position = QPoint(point.x, point.y);
    return true;
}

// What may be published about a foreground holder: its process, thread, class
// name and visibility. A window title can name the user's own content, so it is
// never read here.
struct NativeWindowFacts {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    DWORD threadId = 0;
    QString className;
    bool reachable() const { return hwnd != nullptr; }
};

NativeWindowFacts nativeWindowFacts(HWND hwnd) {
    NativeWindowFacts facts;
    facts.hwnd = hwnd;
    if (hwnd == nullptr) return facts;
    facts.threadId = GetWindowThreadProcessId(hwnd, &facts.pid);
    wchar_t className[256]{};
    const int length = GetClassNameW(hwnd, className, 256);
    if (length > 0) facts.className = QString::fromWCharArray(className);
    return facts;
}

// A transient native window this process owns and shows over the dialog - the
// engine popover is one - keeps the input stream with us while the pointer
// traverses, so it counts as owning the foreground. Any window from another
// process does not.
bool nativeOwnedVisibleMenuForeground(HWND host, HWND foreground) {
    if (host == nullptr || foreground == nullptr) return false;
    const NativeWindowFacts facts = nativeWindowFacts(foreground);
    if (facts.pid == 0 || facts.pid != GetCurrentProcessId() || !IsWindowVisible(foreground))
        return false;
    return GetWindow(foreground, GW_OWNER) == host ||
           GetAncestor(foreground, GA_ROOTOWNER) == host;
}

// Ownership of the input stream, as the traversal and every click must see it.
bool nativeForegroundAllowsInput(QWidget* host) {
    if (host == nullptr) return false;
    const HWND hwnd = reinterpret_cast<HWND>(host->winId());
    const HWND foreground = GetForegroundWindow();
    if (foreground == hwnd && !host->isActiveWindow()) return false;
    return foreground == hwnd || nativeOwnedVisibleMenuForeground(hwnd, foreground);
}

bool nativeForegroundOwned(QWidget* host) {
    if (host != nullptr && nativeForegroundAllowsInput(host)) return true;
    const HWND hwnd = host != nullptr ? reinterpret_cast<HWND>(host->winId()) : nullptr;
    const HWND foreground = GetForegroundWindow();
    const NativeWindowFacts facts = nativeWindowFacts(foreground);
    std::fprintf(stderr,
                 "FAIL native foreground ownership lost before input: "
                 "active=%d foreground=%p dialog=%p (pointer left untouched)\n"
                 "     foreground pid=%lu thread=%lu class=%s sameProcess=%d visible=%d "
                 "ownedByDialog=%d\n",
                 host != nullptr && host->isActiveWindow() ? 1 : 0,
                 static_cast<void*>(foreground), static_cast<void*>(hwnd),
                 static_cast<unsigned long>(facts.pid),
                 static_cast<unsigned long>(facts.threadId),
                 facts.className.isEmpty() ? "<unavailable>" : qPrintable(facts.className),
                 facts.pid == GetCurrentProcessId() ? 1 : 0,
                 facts.reachable() && IsWindowVisible(foreground) ? 1 : 0,
                 facts.reachable() && GetWindow(foreground, GW_OWNER) == hwnd ? 1 : 0);
    return false;
}

// Borrows the current foreground thread's input queue for this scope and always
// detaches again, so a refused activation cannot leave the probe's queue joined
// to another thread. Borrowing is skipped when there is nothing foreign to
// borrow from; that is the unnecessary-attach case, and no window of another
// process is touched beyond the documented attach call itself.
class ForegroundInputBorrow {
public:
    explicit ForegroundInputBorrow(DWORD foregroundThread) : threadId_(foregroundThread) {
        if (threadId_ == 0 || threadId_ == GetCurrentThreadId()) return;
        SetLastError(ERROR_SUCCESS);
        attached_ = AttachThreadInput(threadId_, GetCurrentThreadId(), TRUE) != FALSE;
        attachError_ = GetLastError();
    }

    ~ForegroundInputBorrow() {
        if (!attached_) return;
        SetLastError(ERROR_SUCCESS);
        if (AttachThreadInput(threadId_, GetCurrentThreadId(), FALSE) == FALSE)
            detachError_ = GetLastError();
    }

    bool attached() const { return attached_; }
    DWORD attachError() const { return attachError_; }
    DWORD detachError() const { return detachError_; }

private:
    DWORD threadId_;
    bool attached_ = false;
    DWORD attachError_ = ERROR_SUCCESS;
    DWORD detachError_ = ERROR_SUCCESS;
};

// The foreground goes to the process that last received real input, so a
// console-launched probe is refused by a bare SetForegroundWindow without any
// last error being set. The legitimate order is: restore a minimised window,
// then ask once, and when the OS still refuses, ask again while borrowing the
// holder's input queue. Every call's result and last error is checked. Nothing
// is synthesized and no permission is invented: a refusal is reported with the
// holder's process, thread and class name, and the caller fails.
bool nativeActivateForeground(QWidget* host, int waitMs) {
    if (host == nullptr) return false;
    const HWND hwnd = reinterpret_cast<HWND>(host->winId());
    if (IsIconic(hwnd)) {
        SetLastError(ERROR_SUCCESS);
        const BOOL shown = ShowWindow(hwnd, SW_RESTORE);
        const DWORD restoreError = GetLastError();
        if (IsIconic(hwnd)) {
            std::fprintf(stderr,
                         "FAIL native foreground restore refused: dialog=%p shown=%d "
                         "lastError=%lu (pointer left untouched)\n",
                         static_cast<void*>(hwnd), shown ? 1 : 0, restoreError);
            return false;
        }
    }
    const HWND holder = GetForegroundWindow();
    const NativeWindowFacts holderFacts = nativeWindowFacts(holder);
    BOOL accepted = FALSE;
    DWORD activationError = ERROR_SUCCESS;
    DWORD attachError = ERROR_SUCCESS;
    DWORD detachError = ERROR_SUCCESS;
    bool borrowed = false;
    if (holderFacts.reachable() && holder != hwnd && holderFacts.threadId != 0 &&
        holderFacts.threadId != GetCurrentThreadId()) {
        ForegroundInputBorrow borrow(holderFacts.threadId);
        borrowed = borrow.attached();
        attachError = borrow.attachError();
        SetLastError(ERROR_SUCCESS);
        accepted = SetForegroundWindow(hwnd);
        activationError = GetLastError();
        detachError = borrow.detachError();
    } else {
        SetLastError(ERROR_SUCCESS);
        accepted = SetForegroundWindow(hwnd);
        activationError = GetLastError();
    }
    const bool owned = waitUntil([&] { return nativeForegroundAllowsInput(host); }, waitMs);
    if (owned) {
        std::fprintf(stderr, "[AnimeJanaiProbe] foreground-owned dialog=%p route=%s\n",
                     static_cast<void*>(hwnd),
                     borrowed ? "thread-input-borrow" : (accepted ? "direct" : "deferred"));
        return true;
    }
    std::fprintf(stderr,
                 "FAIL native foreground activation denied: accepted=%d borrowedInput=%d "
                 "lastError=%lu attachLastError=%lu detachLastError=%lu active=%d "
                 "foreground=%p dialog=%p (pointer left untouched)\n"
                 "     foreground pid=%lu thread=%lu class=%s sameProcess=%d visible=%d "
                 "ownedByDialog=%d\n",
                 accepted ? 1 : 0, borrowed ? 1 : 0, activationError, attachError, detachError,
                 host->isActiveWindow() ? 1 : 0,
                 static_cast<void*>(GetForegroundWindow()), static_cast<void*>(hwnd),
                 static_cast<unsigned long>(holderFacts.pid),
                 static_cast<unsigned long>(holderFacts.threadId),
                 holderFacts.className.isEmpty() ? "<unavailable>"
                                                 : qPrintable(holderFacts.className),
                 holderFacts.pid == GetCurrentProcessId() ? 1 : 0,
                 holderFacts.reachable() && IsWindowVisible(holder) ? 1 : 0,
                 holderFacts.reachable() && GetWindow(holder, GW_OWNER) == hwnd ? 1 : 0);
    return false;
}

bool nativeClientGeometry(QWidget* host, QPoint* screenOrigin, QSize* clientSize) {
    if (host == nullptr || screenOrigin == nullptr || clientSize == nullptr) return false;
    const HWND hwnd = reinterpret_cast<HWND>(host->winId());
    RECT client{};
    POINT origin{0, 0};
    SetLastError(ERROR_SUCCESS);
    if (!GetClientRect(hwnd, &client)) {
        std::fprintf(stderr, "FAIL native GetClientRect denied: dialog=%p lastError=%lu\n",
                     static_cast<void*>(hwnd), GetLastError());
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    if (!ClientToScreen(hwnd, &origin)) {
        std::fprintf(stderr, "FAIL native ClientToScreen denied: dialog=%p lastError=%lu\n",
                     static_cast<void*>(hwnd), GetLastError());
        return false;
    }
    *screenOrigin = QPoint(origin.x, origin.y);
    *clientSize = QSize(client.right - client.left, client.bottom - client.top);
    if (clientSize->width() > 0 && clientSize->height() > 0 &&
        host->width() > 0 && host->height() > 0)
        return true;
    std::fprintf(stderr,
                 "FAIL native client geometry unavailable: clientPhysical=%dx%d qtClient=%dx%d\n",
                 clientSize->width(), clientSize->height(), host->width(), host->height());
    return false;
}

bool nativeScreenPoint(QWidget* host, const QPoint& qtGlobal, QPoint* nativeScreen) {
    QPoint clientOrigin;
    QSize clientSize;
    if (!nativeClientGeometry(host, &clientOrigin, &clientSize)) return false;
    const QPoint qtLocal = host->mapFromGlobal(qtGlobal);
    *nativeScreen = clientOrigin +
        QPoint(qRound(qtLocal.x() * clientSize.width() / qreal(host->width())),
               qRound(qtLocal.y() * clientSize.height() / qreal(host->height())));
    return true;
}

bool qtGlobalPoint(QWidget* host, const QPoint& nativeScreen, QPoint* qtGlobal) {
    QPoint clientOrigin;
    QSize clientSize;
    if (!nativeClientGeometry(host, &clientOrigin, &clientSize)) return false;
    const QPoint nativeLocal = nativeScreen - clientOrigin;
    const QPoint qtLocal(qRound(nativeLocal.x() * host->width() / qreal(clientSize.width())),
                         qRound(nativeLocal.y() * host->height() / qreal(clientSize.height())));
    *qtGlobal = host->mapToGlobal(qtLocal);
    return true;
}

HWND nativeWindowAt(const QPoint& nativeScreen) {
    return WindowFromPoint(POINT{nativeScreen.x(), nativeScreen.y()});
}

bool sendNativeClick(QWidget* host, QWidget* target) {
    if (!nativeForegroundOwned(host)) return false;
    if (target == nullptr || !target->isVisible() || !target->isEnabled()) {
        std::fprintf(stderr, "FAIL native click target disappeared before input\n");
        return false;
    }
    QPoint cursor;
    QPoint qtGlobal;
    QPoint expected;
    if (!nativeCursorPosition(&cursor) || !qtGlobalPoint(host, cursor, &qtGlobal) ||
        !nativeScreenPoint(host, target->mapToGlobal(target->rect().center()), &expected))
        return false;
    const HWND hwnd = reinterpret_cast<HWND>(host->winId());
    const HWND nativeHit = nativeWindowAt(cursor);
    QWidget* childHit = deepestChildAt(host, qtGlobal);
    if (!target->rect().contains(target->mapFromGlobal(qtGlobal)) ||
        (childHit != target && (childHit == nullptr || !target->isAncestorOf(childHit))) ||
        (nativeHit != hwnd && !IsChild(hwnd, nativeHit))) {
        std::fprintf(stderr,
                     "FAIL native click target proof changed before input: "
                     "target=%s cursorPhysical=(%d,%d) expectedPhysical=(%d,%d) "
                     "childAt=%s nativeHit=%p dialog=%p (no pointer restoration)\n",
                     qPrintable(target->objectName()), cursor.x(), cursor.y(),
                     expected.x(), expected.y(),
                     childHit != nullptr ? qPrintable(childHit->objectName()) : "<none>",
                     static_cast<void*>(nativeHit), static_cast<void*>(hwnd));
        return false;
    }
    INPUT input[2]{};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SetLastError(ERROR_SUCCESS);
    const UINT inserted = SendInput(2, input, sizeof(INPUT));
    if (inserted == 2) return true;
    std::fprintf(stderr,
                 "FAIL native SendInput denied/partial: inserted=%u/2 lastError=%lu "
                 "(Windows does not identify UIPI denial)\n",
                 inserted, GetLastError());
    return false;
}
#endif

bool movePointerTo(QWidget* target, const QPoint& localPosition, QWidget* coordinateHost,
                   bool* targetChanged = nullptr) {
    if (targetChanged != nullptr) *targetChanged = false;
    if (target == nullptr || coordinateHost == nullptr) return false;
    const QPointer<QWidget> guard(target);
    // Every step pumps the player's own event queue on a 4K surface, so the step
    // count is the only bound on how long a traversal takes. The legs below
    // therefore spend a fixed number of steps, never a distance-derived one,
    // which is what makes a cross-desktop move affordable; each leg's final
    // point is the destination exactly, and a leg rebases from wherever the
    // pointer actually stands, so integer residue can never strand it.
    constexpr int kLegSteps = 8;
    constexpr int kMaxLegs = 3;
#ifdef Q_OS_WIN
    const QPoint centerOffset = localPosition - target->rect().center();
    if (!nativeForegroundOwned(coordinateHost)) return false;
    QPoint current;
    if (!nativeCursorPosition(&current)) return false;
    QPoint destination;
    if (!nativeScreenPoint(coordinateHost, target->mapToGlobal(localPosition), &destination))
        return false;
    const QPoint initialDestination = destination;
    const QPoint origin = current;
    QElapsedTimer deadline;
    QElapsedTimer settled;
    deadline.start();
    settled.start();
    int geometryChanges = 0;
    int legs = 0;
    int legStep = -1;
    QPoint legOrigin;
    bool moving = false;
    QRect targetGlobal(target->mapToGlobal(QPoint()), target->size());
    QRect hostGlobal(coordinateHost->mapToGlobal(QPoint()), coordinateHost->size());
    while (deadline.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (!nativeForegroundOwned(coordinateHost)) return false;
        QPoint observed;
        if (!nativeCursorPosition(&observed)) return false;
        if (observed != current) {
            std::fprintf(stderr,
                         "FAIL native pointer changed outside probe traversal: "
                         "lastRequested=(%d,%d) observed=(%d,%d) "
                         "initialTarget=(%d,%d) currentTarget=(%d,%d) "
                         "geometryChanges=%d (no pointer restoration)\n",
                         current.x(), current.y(), observed.x(), observed.y(),
                         initialDestination.x(), initialDestination.y(),
                         destination.x(), destination.y(), geometryChanges);
            return false;
        }
        if (guard.isNull() || !guard->isVisible() || !guard->isEnabled()) {
            if (targetChanged != nullptr) *targetChanged = true;
            else std::fprintf(stderr, "FAIL native pointer target disappeared during traversal\n");
            return false;
        }
        QPoint freshDestination;
        if (!nativeScreenPoint(coordinateHost,
                               guard->mapToGlobal(guard->rect().center() + centerOffset),
                               &freshDestination))
            return false;
        const QRect freshTarget(guard->mapToGlobal(QPoint()), guard->size());
        const QRect freshHost(coordinateHost->mapToGlobal(QPoint()), coordinateHost->size());
        // Input events call layoutChrome; never continue towards a pre-event target.
        if (freshDestination != destination || freshTarget != targetGlobal || freshHost != hostGlobal) {
            std::fprintf(stderr,
                         "[AnimeJanaiProbe] native-pointer-layout-change "
                         "oldPhysical=(%d,%d) newPhysical=(%d,%d) "
                         "targetQt=(%d,%d %dx%d) hostQt=(%d,%d %dx%d)\n",
                         destination.x(), destination.y(), freshDestination.x(), freshDestination.y(),
                         freshTarget.x(), freshTarget.y(), freshTarget.width(), freshTarget.height(),
                         freshHost.x(), freshHost.y(), freshHost.width(), freshHost.height());
            destination = freshDestination;
            targetGlobal = freshTarget;
            hostGlobal = freshHost;
            ++geometryChanges;
            moving = false;
            legStep = -1;
            settled.restart();
        }
        if (legStep < 0) {
            // A leg that follows a layout change, and an arrival, each have to
            // persist for a settle window before the next thing happens.
            if (!moving && settled.elapsed() < 80) {
                QThread::msleep(10);
                continue;
            }
            if (current == destination) {
                if (settled.elapsed() < 80) {
                    QThread::msleep(10);
                    continue;
                }
                std::fprintf(stderr,
                             "[AnimeJanaiProbe] native-pointer-reached origin=(%d,%d) "
                             "initialTarget=(%d,%d) finalTarget=(%d,%d) "
                             "cursorPhysical=(%d,%d) geometryChanges=%d legs=%d\n",
                             origin.x(), origin.y(), initialDestination.x(), initialDestination.y(),
                             destination.x(), destination.y(), current.x(), current.y(),
                             geometryChanges, legs);
                return true;
            }
            if (legs >= kMaxLegs) {
                std::fprintf(stderr,
                             "FAIL native pointer traversal exhausted its leg budget: "
                             "legs=%d cursorPhysical=(%d,%d) initialTarget=(%d,%d) "
                             "finalTarget=(%d,%d) geometryChanges=%d\n",
                             legs, current.x(), current.y(), initialDestination.x(),
                             initialDestination.y(), destination.x(), destination.y(),
                             geometryChanges);
                return false;
            }
            legOrigin = current;
            legStep = 0;
            ++legs;
            moving = true;
        }
        const int stepIndex = legStep + 1;
        const QPoint next = stepIndex >= kLegSteps
            ? destination
            : QPoint(legOrigin.x() + qRound(qreal(destination.x() - legOrigin.x()) * stepIndex / kLegSteps),
                     legOrigin.y() + qRound(qreal(destination.y() - legOrigin.y()) * stepIndex / kLegSteps));
        legStep = stepIndex;
        if (!nativeForegroundOwned(coordinateHost)) return false;
        SetLastError(ERROR_SUCCESS);
        if (!SetCursorPos(next.x(), next.y())) {
            std::fprintf(stderr,
                         "FAIL native SetCursorPos denied: requested=(%d,%d) "
                         "lastError=%lu (no pointer restoration)\n",
                         next.x(), next.y(), GetLastError());
            return false;
        }
        if (!nativeCursorPosition(&observed)) return false;
        if (observed != next) {
            RECT clip{};
            const BOOL clipKnown = GetClipCursor(&clip);
            std::fprintf(stderr,
                         "FAIL native SetCursorPos readback mismatch: requested=(%d,%d) "
                         "observed=(%d,%d) clipKnown=%d clip=(%ld,%ld %ldx%ld) "
                         "(no pointer restoration)\n",
                         next.x(), next.y(), observed.x(), observed.y(), clipKnown ? 1 : 0,
                         clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top);
            return false;
        }
        current = observed;
        if (legStep >= kLegSteps) {
            // The leg is spent; whether the arrival held is decided next round.
            legStep = -1;
            settled.restart();
        }
    }
    std::fprintf(stderr,
                 "FAIL native pointer target did not settle: initialTarget=(%d,%d) "
                 "finalTarget=(%d,%d) cursorPhysical=(%d,%d) geometryChanges=%d legs=%d\n",
                 initialDestination.x(), initialDestination.y(), destination.x(), destination.y(),
                 current.x(), current.y(), geometryChanges, legs);
    return false;
#else
    Q_UNUSED(coordinateHost);
    const QPoint destination = target->mapToGlobal(localPosition);
    const QPoint origin = QCursor::pos();
    for (int step = 1; step <= kLegSteps; ++step) {
        const QPoint point = step >= kLegSteps
            ? destination
            : QPoint(origin.x() + qRound(qreal(destination.x() - origin.x()) * step / kLegSteps),
                     origin.y() + qRound(qreal(destination.y() - origin.y()) * step / kLegSteps));
        QCursor::setPos(point);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (guard.isNull()) return false;
    }
    return true;
#endif
}

bool chooseEnhancementMenu(PlayerDialog* dialog, const QStringList& path,
                          const QString& selectorImage = {}) {
    std::fprintf(stderr, "[AnimeJanaiProbe] menu-begin path=%s\n",
                 qPrintable(path.join(QStringLiteral(" -> "))));
    auto* playerShell = dialog->findChild<shell::PlayerShell*>();
    if (playerShell == nullptr) return false;
    playerShell->wake();
    dialog->raise();
    dialog->activateWindow();
    QCoreApplication::processEvents();
    shell::IconButton* trigger = nullptr;
    for (auto* button : dialog->findChildren<shell::IconButton*>()) {
        if (button->isVisible() && button->icon() == &shell::IconPainter::spark) {
            trigger = button;
            break;
        }
    }
    if (trigger == nullptr) return false;
#ifdef Q_OS_WIN
    const HWND dialogHwnd = reinterpret_cast<HWND>(dialog->winId());
    if (!nativeActivateForeground(dialog, 1000)) return false;
    if (GetForegroundWindow() != dialogHwnd && dialog->isActiveWindow()) {
        // A transient we own still holds the physical foreground; the pointer
        // proof below and WindowFromPoint at the cursor decide the click.
        std::fprintf(stderr,
                     "[AnimeJanaiProbe] foreground-on-owned-transient active=%d dialog=%p\n",
                     dialog->isActiveWindow() ? 1 : 0, static_cast<void*>(dialogHwnd));
    }
    if (!movePointerTo(trigger, trigger->rect().center(), dialog)) return false;
    const QPoint triggerCenter = trigger->mapToGlobal(trigger->rect().center());
    QPoint expectedNative;
    QPoint actualNative;
    QPoint actualQtGlobal;
    if (!nativeScreenPoint(dialog, triggerCenter, &expectedNative) ||
        !nativeCursorPosition(&actualNative) ||
        !qtGlobalPoint(dialog, actualNative, &actualQtGlobal))
        return false;
    const QPoint actualDialogLocal = dialog->mapFromGlobal(actualQtGlobal);
    const QRect triggerLocal(trigger->mapTo(dialog, QPoint(0, 0)), trigger->size());
    const HWND triggerHit = nativeWindowAt(actualNative);
    const bool cursorOnTrigger = triggerLocal.contains(actualDialogLocal);
    if (!cursorOnTrigger ||
        (triggerHit != dialogHwnd && !IsChild(dialogHwnd, triggerHit)) ||
        !sendNativeClick(dialog, trigger)) {
        RECT client{};
        GetClientRect(dialogHwnd, &client);
        std::fprintf(stderr,
                     "FAIL native foreground click missed enhancement trigger: "
                     "active=%d foreground=%p dialog=%p triggerQt=(%d,%d %dx%d) "
                     "cursorPhysical=(%d,%d) expectedPhysical=(%d,%d) "
                     "cursorDialogQt=(%d,%d) clientPhysical=%ldx%ld qtClient=%dx%d "
                     "cursorOnTrigger=%d hit=%p\n",
                     dialog->isActiveWindow() ? 1 : 0,
                     static_cast<void*>(GetForegroundWindow()), static_cast<void*>(dialogHwnd),
                     triggerLocal.x(), triggerLocal.y(), triggerLocal.width(), triggerLocal.height(),
                     actualNative.x(), actualNative.y(), expectedNative.x(), expectedNative.y(),
                     actualDialogLocal.x(), actualDialogLocal.y(),
                     client.right - client.left, client.bottom - client.top,
                     dialog->width(), dialog->height(), cursorOnTrigger ? 1 : 0,
                     static_cast<void*>(triggerHit));
        dialog->grab().save(QDir::current().filePath(QStringLiteral("trigger-hit-failure.png")));
        return false;
    }
#else
    if (!movePointerTo(trigger, trigger->rect().center(), dialog)) return false;
    QTest::mouseClick(trigger, Qt::LeftButton);
#endif
    std::fprintf(stderr, "[AnimeJanaiProbe] menu-root-open\n");
    for (const QString& payload : path) {
        std::fprintf(stderr, "[AnimeJanaiProbe] menu-row-begin payload=%s\n",
                     qPrintable(payload));
        QPointer<QWidget> row;
        QPointer<shell::MenuPopup> ownerPanel;
        QString ownerLevel;
        QString submenuLevel;
        const bool found = waitUntil([&] {
            for (auto* panel : dialog->findChildren<shell::MenuPopup*>()) {
                if (!panel->isVisible()) continue;
                for (int index = 0; index < panel->rowCount(); ++index) {
                    const shell::MenuEntry entry = panel->entryAt(index);
                    QWidget* candidate = panel->rowWidget(index);
                    if (entry.payload.toString() == payload && !entry.disabled &&
                        candidate != nullptr && candidate->isVisible() && candidate->isEnabled()) {
                        row = candidate;
                        ownerPanel = panel;
                        ownerLevel = panel->property("enhancementLevel").toString();
                        submenuLevel = entry.submenu
                            ? entry.payload.toString().mid(QStringLiteral("level|").size())
                            : QString();
                        return true;
                    }
                }
            }
            return false;
        }, 2000);
        if (!found || row.isNull()) {
            std::fprintf(stderr, "FAIL visible enhancement menu choice unavailable: %s\n",
                         qPrintable(payload));
#ifdef Q_OS_WIN
            QPoint physical;
            QPoint qtGlobal;
            if (!nativeCursorPosition(&physical) || !qtGlobalPoint(dialog, physical, &qtGlobal))
                return false;
            const QPoint dialogLocal = dialog->mapFromGlobal(qtGlobal);
            const HWND hwnd = reinterpret_cast<HWND>(dialog->winId());
            RECT client{};
            GetClientRect(hwnd, &client);
            int visiblePanels = 0;
            for (shell::MenuPopup* panel : dialog->findChildren<shell::MenuPopup*>()) {
                if (panel->isVisible()) ++visiblePanels;
            }
            std::fprintf(stderr,
                         "     root-click diagnostics physical=(%d,%d) qtGlobal=(%d,%d) "
                         "dialogLocal=(%d,%d) dialogQt=%dx%d clientPhysical=%ldx%ld "
                         "visiblePanels=%d active=%d foreground=%p dialog=%p nativeHit=%p\n",
                         physical.x(), physical.y(), qtGlobal.x(), qtGlobal.y(),
                         dialogLocal.x(), dialogLocal.y(), dialog->width(), dialog->height(),
                         client.right - client.left, client.bottom - client.top,
                         visiblePanels, dialog->isActiveWindow() ? 1 : 0,
                         static_cast<void*>(GetForegroundWindow()), static_cast<void*>(hwnd),
                         static_cast<void*>(nativeWindowAt(physical)));
#endif
            dialog->grab().save(QDir::current().filePath(QStringLiteral("menu-row-unavailable.png")));
            return false;
        }
        QRect rowGlobal(row->mapToGlobal(QPoint(0, 0)), row->size());
        const QRect hostGlobal(dialog->mapToGlobal(QPoint(0, 0)), dialog->size());
        const QScreen* screen = dialog->screen();
        const QRect available = screen != nullptr ? screen->availableGeometry() : QRect();
        QPoint center = rowGlobal.center();
        QWidget* underPointer = QApplication::widgetAt(center);
        QWidget* childHit = deepestChildAt(dialog, center);
        const bool qtChildExposed =
            childHit == row || (childHit != nullptr && row->isAncestorOf(childHit));
#ifdef Q_OS_WIN
        QPoint nativeCenter;
        if (!nativeForegroundOwned(dialog) || !nativeScreenPoint(dialog, center, &nativeCenter))
            return false;
        const HWND nativeHit = nativeWindowAt(nativeCenter);
        const bool nativeWindowExposed = nativeHit == dialogHwnd || IsChild(dialogHwnd, nativeHit);
#else
        const bool nativeWindowExposed = true;
#endif
        // Qt child geometry plus the native HWND hit distinguishes DPI lookup errors from occlusion.
        const bool exposed = qtChildExposed && nativeWindowExposed;
        const bool reachable = rowGlobal.width() > 0 && rowGlobal.height() > 0 &&
            hostGlobal.contains(rowGlobal) && available.contains(rowGlobal) && exposed;
        if (!reachable) {
            std::fprintf(stderr,
                         "FAIL enhancement row is not fully reachable on screen: %s "
                         "row=(%d,%d %dx%d) host=(%d,%d %dx%d) screen=(%d,%d %dx%d) exposed=%d\n",
                         qPrintable(payload), rowGlobal.x(), rowGlobal.y(), rowGlobal.width(),
                         rowGlobal.height(), hostGlobal.x(), hostGlobal.y(), hostGlobal.width(),
                         hostGlobal.height(), available.x(), available.y(), available.width(),
                         available.height(), exposed ? 1 : 0);
            std::fprintf(stderr,
                         "     Qt hit=%s/%s childAt=%s/%s pointer=(%d,%d) "
                         "active-window=%s dialog-active=%d native-window-exposed=%d\n",
                         underPointer != nullptr ? underPointer->metaObject()->className() : "<none>",
                         underPointer != nullptr ? qPrintable(underPointer->objectName()) : "<none>",
                         childHit != nullptr ? childHit->metaObject()->className() : "<none>",
                         childHit != nullptr ? qPrintable(childHit->objectName()) : "<none>",
                         QCursor::pos().x(), QCursor::pos().y(),
                         QApplication::activeWindow() != nullptr
                             ? QApplication::activeWindow()->objectName().toUtf8().constData()
                             : "<none>",
                         dialog->isActiveWindow() ? 1 : 0, nativeWindowExposed ? 1 : 0);
#ifdef Q_OS_WIN
            POINT cursor{};
            GetCursorPos(&cursor);
            const HWND foreground = GetForegroundWindow();
            const HWND own = reinterpret_cast<HWND>(dialog->winId());
            const UINT dpi = GetDpiForWindow(own);
            QPoint nativeCenter;
            if (!nativeScreenPoint(dialog, center, &nativeCenter)) return false;
            const HWND nativeCenterHit = nativeWindowAt(nativeCenter);
            const HWND rawCenterHit = WindowFromPoint(POINT{center.x(), center.y()});
            RECT windowRect{};
            GetWindowRect(own, &windowRect);
            std::fprintf(stderr,
                         "     Win32 foreground=%p own=%p cursor=(%ld,%ld) "
                         "Qt-center=(%d,%d) raw-hit=%p native-center=(%d,%d) "
                         "scaled-hit=%p dialog-rect=(%ld,%ld %ldx%ld) dpi=%u qt-dpr=%.2f\n",
                         static_cast<void*>(foreground), static_cast<void*>(own),
                         cursor.x, cursor.y, center.x(), center.y(),
                         static_cast<void*>(rawCenterHit), nativeCenter.x(), nativeCenter.y(),
                         static_cast<void*>(nativeCenterHit), windowRect.left, windowRect.top,
                         windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
                         dpi, screen != nullptr ? screen->devicePixelRatio() : 1.0);
#endif
            dialog->grab().save(QDir::current().filePath(QStringLiteral("menu-reachability-failure.png")));
            return false;
        }
        std::fprintf(stderr, "[AnimeJanaiProbe] menu-row-hover payload=%s\n",
                     qPrintable(payload));
        bool targetChanged = false;
        if (!movePointerTo(row.data(), row->rect().center(), dialog, &targetChanged) &&
            !targetChanged)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        bool rowReplaced = false;
        if (row.isNull() || !row->isVisible()) {
            if (!ownerPanel.isNull() && ownerPanel->isVisible() &&
                ownerPanel->property("enhancementLevel").toString() == ownerLevel) {
                for (int index = 0; index < ownerPanel->rowCount(); ++index) {
                    const shell::MenuEntry entry = ownerPanel->entryAt(index);
                    QWidget* candidate = ownerPanel->rowWidget(index);
                    if (entry.payload.toString() == payload && !entry.disabled &&
                        candidate != nullptr && candidate->isVisible() && candidate->isEnabled()) {
                        row = candidate;
                        rowReplaced = true;
                        break;
                    }
                }
            }
            if (row.isNull() || !row->isVisible()) {
                std::fprintf(stderr,
                             "FAIL enhancement menu row disappeared during pointer hover: %s\n",
                             qPrintable(payload));
                return false;
            }
            std::fprintf(stderr,
                         "[AnimeJanaiProbe] re-resolved visible menu row after hover refresh "
                         "payload=%s level=%s\n",
                         qPrintable(payload), qPrintable(ownerLevel));
        }
        if (rowReplaced) {
            if (!movePointerTo(row.data(), row->rect().center() + QPoint(1, 0), dialog) ||
                row.isNull() ||
                !movePointerTo(row.data(), row->rect().center(), dialog))
                return false;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
            if (row.isNull() || !row->isVisible()) {
                std::fprintf(stderr,
                             "FAIL re-resolved enhancement row disappeared on continuous hover: %s\n",
                             qPrintable(payload));
                return false;
            }
        }
        rowGlobal = QRect(row->mapToGlobal(QPoint(0, 0)), row->size());
        center = rowGlobal.center();
        const QRect currentAvailable = screen != nullptr ? screen->availableGeometry() : QRect();
        QWidget* currentChildHit = deepestChildAt(dialog, center);
        const bool currentChildExposed =
            currentChildHit == row || (currentChildHit != nullptr && row->isAncestorOf(currentChildHit));
#ifdef Q_OS_WIN
        QPoint currentNativeCenter;
        if (!nativeScreenPoint(dialog, center, &currentNativeCenter)) return false;
        const HWND currentNativeHit = nativeWindowAt(currentNativeCenter);
        const bool currentNativeExposed =
            currentNativeHit == dialogHwnd || IsChild(dialogHwnd, currentNativeHit);
#else
        const bool currentNativeExposed = true;
#endif
        if (rowGlobal.width() <= 0 || rowGlobal.height() <= 0 ||
            !hostGlobal.contains(rowGlobal) || !currentAvailable.contains(rowGlobal) ||
            !currentChildExposed || !currentNativeExposed) {
            std::fprintf(stderr,
                         "FAIL re-resolved enhancement row is not fully reachable: %s "
                         "row=(%d,%d %dx%d) host=(%d,%d %dx%d) child=%s native=%d\n",
                         qPrintable(payload), rowGlobal.x(), rowGlobal.y(),
                         rowGlobal.width(), rowGlobal.height(), hostGlobal.x(), hostGlobal.y(),
                         hostGlobal.width(), hostGlobal.height(),
                         currentChildHit != nullptr ? qPrintable(currentChildHit->objectName()) : "<none>",
                         currentNativeExposed ? 1 : 0);
            return false;
        }
#ifdef Q_OS_WIN
        QPoint pointerNative;
        QPoint pointer;
        if (!nativeForegroundOwned(dialog) || !nativeCursorPosition(&pointerNative) ||
            !qtGlobalPoint(dialog, pointerNative, &pointer))
            return false;
        QWidget* pointerWidget = QApplication::widgetAt(pointer);
        QWidget* pointerChild = deepestChildAt(dialog, pointer);
        const HWND pointerHit = nativeWindowAt(pointerNative);
        if ((pointerChild != row && (pointerChild == nullptr || !row->isAncestorOf(pointerChild))) ||
            (pointerHit != dialogHwnd && !IsChild(dialogHwnd, pointerHit))) {
            std::fprintf(stderr,
                         "FAIL native pointer traversal missed menu row %s: "
                         "physical=(%d,%d) qtGlobal=(%d,%d) widgetAt=%s/%s childAt=%s "
                         "native-hit=%p dialog=%p\n",
                         qPrintable(payload), pointerNative.x(), pointerNative.y(),
                         pointer.x(), pointer.y(),
                         pointerWidget != nullptr ? pointerWidget->metaObject()->className() : "<none>",
                         pointerWidget != nullptr ? qPrintable(pointerWidget->objectName()) : "<none>",
                         pointerChild != nullptr ? qPrintable(pointerChild->objectName()) : "<none>",
                         static_cast<void*>(pointerHit), static_cast<void*>(dialogHwnd));
            return false;
        }
#endif
        if (!submenuLevel.isEmpty()) {
            const bool openedByHover = waitUntil([&] {
                for (auto* panel : dialog->findChildren<shell::MenuPopup*>()) {
                    if (panel->isVisible() &&
                        panel->property("enhancementLevel").toString() == submenuLevel)
                        return true;
                }
                return false;
            }, 2000);
            std::fprintf(stderr,
                         "[AnimeJanaiProbe] menu-hover-result payload=%s opened=%d\n",
                         qPrintable(payload), openedByHover ? 1 : 0);
            if (!openedByHover) {
                std::fprintf(stderr,
                             "FAIL enhancement submenu did not open from real row hover: %s\n",
                             qPrintable(payload));
                return false;
            }
        }
        if (!selectorImage.isEmpty() && payload.startsWith(QStringLiteral("ajipreset|rife:"))) {
            for (shell::MenuPopup* panel : dialog->findChildren<shell::MenuPopup*>()) {
                if (!panel->isVisible()) continue;
                const QRect panelGlobal(panel->mapToGlobal(QPoint(0, 0)), panel->size());
                if (!hostGlobal.contains(panelGlobal) || !available.contains(panelGlobal)) {
                    std::fprintf(stderr,
                                 "FAIL visible RIFE selector panel is clipped: level=%s "
                                 "panel=(%d,%d %dx%d) host=(%d,%d %dx%d) screen=(%d,%d %dx%d)\\n",
                                 qPrintable(panel->property("enhancementLevel").toString()),
                                 panelGlobal.x(), panelGlobal.y(), panelGlobal.width(),
                                 panelGlobal.height(), hostGlobal.x(), hostGlobal.y(),
                                 hostGlobal.width(), hostGlobal.height(), available.x(),
                                 available.y(), available.width(), available.height());
                    return false;
                }
            }
            const bool saved = dialog->grab().save(selectorImage, "PNG");
            std::fprintf(stderr,
                         "[AnimeJanaiProbe] visible RIFE selector image=%s saved=%d "
                         "row=(%d,%d %dx%d) host=(%d,%d %dx%d) screen=(%d,%d %dx%d) dpr=%.2f\n",
                         qPrintable(selectorImage), saved ? 1 : 0, rowGlobal.x(), rowGlobal.y(),
                         rowGlobal.width(), rowGlobal.height(), hostGlobal.x(), hostGlobal.y(),
                         hostGlobal.width(), hostGlobal.height(), available.x(), available.y(),
                         available.width(), available.height(), row->devicePixelRatioF());
            if (!saved) return false;
        }
#ifdef Q_OS_WIN
        if (!sendNativeClick(dialog, row.data())) {
            std::fprintf(stderr, "FAIL native mouse click could not reach menu row: %s\n",
                         qPrintable(payload));
            return false;
        }
#else
        QTest::mouseClick(row.data(), Qt::LeftButton);
#endif
        std::fprintf(stderr, "[AnimeJanaiProbe] menu-row-clicked payload=%s\n",
                     qPrintable(payload));
        QCoreApplication::processEvents();
    }
    std::fprintf(stderr, "[AnimeJanaiProbe] menu-complete\n");
    return true;
}

bool chooseAnimeJanaiPreset(PlayerDialog* dialog, const QString& presetId,
                            const QString& selectorImage = {}) {
    QStringList path{QStringLiteral("level|animejanai")};
    if (presetId.startsWith(QStringLiteral("spatial:"))) {
        path.append(QStringLiteral("level|animejanai:spatial"));
    } else if (presetId.startsWith(QStringLiteral("rife:"))) {
        const QString version = QFileInfo(presetId.mid(5)).completeBaseName()
                                    .section(QLatin1Char('_'), 0, 1)
                                    .remove(QStringLiteral("rife_v"));
        bool valid = false;
        const int minor = version.section(QLatin1Char('.'), 1, 1).toInt(&valid);
        if (!valid || minor < 7) return false;
        path.append(QStringLiteral("level|animejanai:rife"));
        path.append(QStringLiteral("level|animejanai:rife:range:%1").arg((minor - 7) / 5));
        path.append(QStringLiteral("level|animejanai:rife:version:") + version);
    }
    path.append(QStringLiteral("ajipreset|") + presetId);
    return chooseEnhancementMenu(dialog, path, selectorImage);
}

native::AnimeJanaiPlan prepareOnWorker(const QString& presetId, native::Backend backend) {
    native::AnimeJanaiPlan plan;
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        plan = native::prepareAnimeJanaiPlan(presetId, backend);
        finished.store(true, std::memory_order_release);
    });
    while (!finished.load(std::memory_order_acquire)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(20);
    }
    worker.join();
    return plan;
}

bool changed(const QImage& left, const QImage& right) {
    if (left.isNull() || right.isNull() || left.size() != right.size()) return false;
    const QImage a = left.convertToFormat(QImage::Format_RGB32);
    const QImage b = right.convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < a.height(); y += 4) {
        for (int x = 0; x < a.width(); x += 4) {
            if (a.pixel(x, y) != b.pixel(x, y)) return true;
        }
    }
    return false;
}

bool nonBlack(const QImage& image) {
    if (image.isNull()) return false;
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < rgb.height(); y += 8) {
        for (int x = 0; x < rgb.width(); x += 8) {
            const QRgb pixel = rgb.pixel(x, y);
            if (qRed(pixel) > 8 || qGreen(pixel) > 8 || qBlue(pixel) > 8) return true;
        }
    }
    return false;
}
QMap<QString, QString> engineSnapshot(const QString& modelDir) {
    QMap<QString, QString> result;
    QDirIterator it(modelDir,
                    {QStringLiteral("*.engine")},
                    QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (hash.addData(&file)) result.insert(QFileInfo(path).absoluteFilePath(),
                                               QString::fromLatin1(hash.result().toHex()));
    }
    return result;
}

QString fileSha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}

QStringList freshEngineFiles(const QString& modelDir, const QMap<QString, QString>& before) {
    QStringList files;
    QDirIterator it(modelDir,
                    {QStringLiteral("*.engine")},
                    QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = QFileInfo(it.next()).absoluteFilePath();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        const qint64 bytes = file.size();
        if (bytes <= 0) continue;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file)) continue;
        const QString digest = QString::fromLatin1(hash.result().toHex());
        if (!before.contains(path) || before.value(path) != digest)
            files.append(QStringLiteral("%1|%2|%3").arg(path).arg(bytes).arg(digest));
    }
    return files;
}
QString selectedModelIdentity(const native::AnimeJanaiPlan& plan) {
    if (plan.presetId.startsWith(QStringLiteral("rife:"))) {
        for (const native::AnimeJanaiPreset& preset : native::animeJanaiPresets()) {
            if (preset.id == plan.presetId)
                return QFileInfo(preset.modelFile).completeBaseName();
        }
        return {};
    }
    QFile config(plan.configPath);
    if (!config.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QString contents = QString::fromUtf8(config.readAll());
    const QStringList keys = plan.presetId.startsWith(QStringLiteral("rife:"))
        ? QStringList{QStringLiteral("chain_1_rife_model=")}
        : QStringList{QStringLiteral("chain_1_model_1_name=")};
    for (const QString& key : keys) {
        const int start = contents.indexOf(key);
        if (start < 0) continue;
        const int valueStart = start + key.size();
        int end = contents.indexOf(QLatin1Char('\n'), valueStart);
        if (end < 0) end = contents.size();
        return contents.mid(valueStart, end - valueStart).trimmed();
    }
    return {};
}

QString normalizedArtifactPath(QString path, const QString& baseDirectory) {
    path = path.trimmed();
    if (path.size() >= 2 &&
        ((path.startsWith(QLatin1Char('"')) && path.endsWith(QLatin1Char('"'))) ||
         (path.startsWith(QLatin1Char('\'')) && path.endsWith(QLatin1Char('\'')))))
        path = path.mid(1, path.size() - 2);
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!QDir::isAbsolutePath(path)) path = QDir(baseDirectory).filePath(path);
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath())
        .replace(QLatin1Char('\\'), QLatin1Char('/')).toCaseFolded();
}

QString trtexecOptionValue(const QString& logContents, const QString& option) {
    const QString expression =
        QStringLiteral("(?:^|\\s)--%1(?:=|\\s+)(?:\"([^\"]+)\"|'([^']+)'|([^\\s]+))")
            .arg(QRegularExpression::escape(option));
    const QRegularExpression regex(expression, QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = regex.match(logContents);
    if (!match.hasMatch()) return {};
    for (int group = 1; group <= 3; ++group)
        if (!match.captured(group).isEmpty()) return match.captured(group);
    return {};
}

QStringList selectedFreshEngineFiles(const QStringList& fresh,
                                     const QStringList& buildLogs) {
    QStringList selected;
    for (const QString& entry : fresh) {
        const int separator = entry.indexOf(QLatin1Char('|'));
        if (separator <= 0) continue;
        const QString path = normalizedArtifactPath(entry.left(separator), {});
        for (const QString& evidence : buildLogs) {
            const QStringList fields = evidence.split(QLatin1Char('|'));
            if (fields.size() >= 3 &&
                normalizedArtifactPath(fields.at(2), {}).compare(path) == 0) {
                selected.append(entry);
                break;
            }
        }
    }
    return selected;
}
QMap<QString, QString> buildLogSnapshot(const QString& modelDir) {
    QMap<QString, QString> result;
    QDirIterator it(modelDir, {QStringLiteral("*.log")},
                    QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = QFileInfo(it.next()).absoluteFilePath();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (hash.addData(&file))
            result.insert(path, QString::fromLatin1(hash.result().toHex()));
    }
    return result;
}

QStringList selectedBuildLogEvidence(const QString& modelDir,
                                     const QMap<QString, QString>& before,
                                     const QString& selectedGraphPath) {
    QStringList evidence;
    if (selectedGraphPath.isEmpty()) return evidence;
    QDirIterator it(modelDir, {QStringLiteral("*.log")}, QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = QFileInfo(it.next()).absoluteFilePath();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file)) continue;
        const QString digest = QString::fromLatin1(hash.result().toHex());
        if (before.contains(path) && before.value(path) == digest) continue;
        if (!file.seek(0)) continue;
        const QString contents = QString::fromUtf8(file.readAll());
        QString passedLine;
        for (const QString& line : contents.split(QLatin1Char('\n'))) {
            if (line.contains(QStringLiteral("&&&&PASSED"), Qt::CaseInsensitive) ||
                line.contains(QStringLiteral("&&&& PASSED"), Qt::CaseInsensitive)) {
                passedLine = line.trimmed();
                break;
            }
        }
        if (passedLine.isEmpty()) continue;
        const QString onnxArgument = trtexecOptionValue(contents, QStringLiteral("onnx"));
        const QString engineArgument = trtexecOptionValue(contents, QStringLiteral("saveEngine"));
        const QString normalizedGraph =
            normalizedArtifactPath(onnxArgument, modelDir);
        const QString normalizedEngine =
            normalizedArtifactPath(engineArgument, modelDir);
        const QString normalizedModelDirectory =
            normalizedArtifactPath(modelDir, {});
        if (normalizedGraph.compare(
                normalizedArtifactPath(selectedGraphPath, modelDir),
                Qt::CaseInsensitive) != 0 ||
            QFileInfo(normalizedEngine).suffix().compare(
                QStringLiteral("engine"), Qt::CaseInsensitive) != 0 ||
            normalizedArtifactPath(QFileInfo(normalizedEngine).absolutePath(), {})
                .compare(normalizedModelDirectory, Qt::CaseInsensitive) != 0)
            continue;
        evidence.append(QStringLiteral("%1|%2|%3|%4")
                            .arg(path, digest, normalizedEngine, passedLine));
    }
    return evidence;
}

std::optional<double> numericProperty(VideoSurface* surface, const QString& name) {
    const auto value = surface->probeMpvProperty(name, ProbePropertyKind::Number);
    if (!value || !value->isNumeric()) return std::nullopt;
    return value->asNumber();
}

struct EngineBuildPlaybackEvidence {
    bool armed = false;
    bool originallyPlaying = false;
    bool started = false;
    bool held = true;
    bool completed = false;
    bool succeeded = false;
    bool completionObservedProcessing = false;
    QString source;
    QString corePath;
    uint64_t sourceEpoch = 0;
    QElapsedTimer clock;
    qint64 acknowledgedAt = -1;
    qint64 framebufferAt = -1;
    qint64 lastHeldAt = -1;
    int heldSamples = 0;
    int framebufferSamples = 0;
    int lastNotice = -5000;
    std::optional<double> heldTime;
    QImage heldFramebuffer;
    bool heldImageSaved = false;

    void arm(VideoSurface* surface, bool baselineAdvanced) {
        armed = true;
        const auto pause = surface->probeMpvProperty(QStringLiteral("pause"), ProbePropertyKind::Flag);
        const auto path = surface->probeMpvProperty(QStringLiteral("path"), ProbePropertyKind::Text);
        const auto time = numericProperty(surface, QStringLiteral("time-pos"));
        originallyPlaying = baselineAdvanced && pause &&
            pause->kind == ProbePropertyValue::Kind::Flag && !pause->flag &&
            time && std::isfinite(*time);
        source = surface->sourceUrl();
        sourceEpoch = surface->sourceEpoch();
        if (path && path->kind == ProbePropertyValue::Kind::Text) corePath = path->text;
        held = !source.isEmpty() && sourceEpoch != 0 && !corePath.isEmpty();
    }

    void begin() {
        if (!armed || started) return;
        started = true;
        clock.start();
    }

    void sample(VideoSurface* surface, const QString& nativeRoot) {
        if (!armed || !started || completed) return;
        const qint64 elapsed = clock.elapsed();
        const auto pause = surface->probeMpvProperty(QStringLiteral("pause"), ProbePropertyKind::Flag);
        const auto path = surface->probeMpvProperty(QStringLiteral("path"), ProbePropertyKind::Text);
        const auto time = numericProperty(surface, QStringLiteral("time-pos"));
        const bool paused = pause && pause->kind == ProbePropertyValue::Kind::Flag && pause->flag;
        const bool sameSource = surface->sourceEpoch() == sourceEpoch &&
            surface->sourceUrl() == source && path &&
            path->kind == ProbePropertyValue::Kind::Text && path->text == corePath;
        held &= sameSource;
        if (!paused) {
            // libmpv acknowledges the async pause on its next event pump.
            if (acknowledgedAt >= 0 || elapsed > 2000) held = false;
        } else if (!time || !std::isfinite(*time)) {
            held = false;
        } else {
            if (acknowledgedAt < 0) {
                acknowledgedAt = elapsed;
                heldTime = *time;
            }
            const bool sameTime = heldTime && std::abs(*time - *heldTime) <= 0.0001;
            if (held && !sameTime)
                std::fprintf(stderr, "FAIL [engine-pause] time progressed during build: held=%.6f current=%.6f\n",
                             heldTime ? *heldTime : -1.0, *time);
            held &= sameTime;
            lastHeldAt = elapsed;
            ++heldSamples;
            // Permit one queued presentation after pause acknowledgement.
            if (elapsed - acknowledgedAt >= 250) {
                const QImage frame = surface->grabFramebuffer();
                if (framebufferAt < 0) {
                    framebufferAt = elapsed;
                    heldFramebuffer = frame;
                    held &= nonBlack(frame);
                    heldImageSaved = frame.save(
                        QDir(nativeRoot).filePath(QStringLiteral("animejanai-probe-build-held.png")), "PNG");
                } else {
                    const bool sameFramebuffer = !frame.isNull() && frame == heldFramebuffer;
                    if (held && !sameFramebuffer)
                        std::fprintf(stderr, "FAIL [engine-pause] native framebuffer changed during build at %lld ms\n",
                                     static_cast<long long>(elapsed));
                    held &= sameFramebuffer;
                }
                ++framebufferSamples;
            }
        }
        if (elapsed - lastNotice >= 5000) {
            lastNotice = static_cast<int>(elapsed);
            std::fprintf(stderr,
                         "[engine-pause] during-build-ms=%lld mpv-pause=%d mpv-time-pos=%.6f held-time-pos=%.6f same-source=%d held=%d pause-samples=%d framebuffer-samples=%d\n",
                         static_cast<long long>(elapsed), paused ? 1 : 0,
                         time ? *time : -1.0, heldTime ? *heldTime : -1.0,
                         sameSource ? 1 : 0, held ? 1 : 0, heldSamples, framebufferSamples);
        }
    }

    bool proved() const {
        return originallyPlaying && started && held && completed && succeeded &&
            completionObservedProcessing && acknowledgedAt >= 0 && acknowledgedAt <= 2000 &&
            heldSamples >= 4 && framebufferSamples >= 3 && framebufferAt >= 0 &&
            lastHeldAt - framebufferAt >= 1000 && heldImageSaved;
    }
};

void reportPlaybackMetrics(VideoSurface* surface, const char* phase) {
    const PlaybackDiagnostics metrics = surface->playbackDiagnostics();
    std::fprintf(stderr, "[AnimeJanaiProbe] %s estimated-vf-fps=", phase);
    if (metrics.estimatedVfFps) std::fprintf(stderr, "%.3f\n", *metrics.estimatedVfFps);
    else std::fprintf(stderr, "unavailable\n");
    std::fprintf(stderr, "[AnimeJanaiProbe] %s decoder-frame-drop-count=", phase);
    if (metrics.decoderDropCount) std::fprintf(stderr, "%lld\n", static_cast<long long>(*metrics.decoderDropCount));
    else std::fprintf(stderr, "unavailable\n");
    std::fprintf(stderr, "[AnimeJanaiProbe] %s frame-drop-count=", phase);
    if (metrics.voDropCount) std::fprintf(stderr, "%lld\n", static_cast<long long>(*metrics.voDropCount));
    else std::fprintf(stderr, "unavailable\n");
}

uint64_t observedRenderedOutputFrames(VideoSurface* surface, int durationMs) {
    const uint64_t before = surface->playbackDiagnostics().renderedOutputFrames;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < durationMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(8);
    }
    const uint64_t after = surface->playbackDiagnostics().renderedOutputFrames;
    return after >= before ? after - before : 0;
}

int observedPtsChanges(VideoSurface* surface, int durationMs) {
    QElapsedTimer clock;
    clock.start();
    std::optional<double> previous;
    int changes = 0;
    while (clock.elapsed() < durationMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const auto pts = numericProperty(surface, QStringLiteral("time-pos"));
        if (pts && (!previous || *pts > *previous + 0.0001)) {
            if (previous) ++changes;
            previous = pts;
        }
        QThread::msleep(20);
    }
    return changes;
}

void exerciseTrack(VideoSurface* surface, const QString& property,
                   const std::optional<ProbePropertyValue>& selected, const char* label,
                   int* failures) {
    if (!selected || !selected->isNumeric()) {
        std::fprintf(stderr, "FAIL %s fixture track is unavailable; selected id is not observable\n",
                     label);
        if (failures != nullptr) ++*failures;
        return;
    }
    const double original = selected->asNumber();
    if (!surface->probeCycleMpvTrack(property)) {
        std::fprintf(stderr, "FAIL %s fixture track cycle was not submitted to embedded mpv\n", label);
        if (failures != nullptr) ++*failures;
        return;
    }
    const bool changedTrack = waitUntil([&] {
        const auto current = surface->probeMpvProperty(property, ProbePropertyKind::Number);
        return current && current->isNumeric() && current->asNumber() != original;
    }, 3000);
    if (changedTrack) {
        std::fprintf(stderr, "PASS embedded mpv cycled the %s fixture track %.0f -> observed new track\n",
                     label, original);
    } else {
        std::fprintf(stderr, "FAIL %s fixture did not expose an alternate selected track id\n", label);
        if (failures != nullptr) ++*failures;
    }
}

bool samePreparedPlan(const native::AnimeJanaiPlan& actual,
                      const native::AnimeJanaiPlan& requested) {
    return actual.enabled && actual.available && actual.presetId == requested.presetId &&
           actual.backend == requested.backend && actual.slot == requested.slot &&
           actual.libraryPath == requested.libraryPath &&
           actual.configPath == requested.configPath &&
           actual.modelDir == requested.modelDir &&
           actual.rifeModelDir == requested.rifeModelDir &&
           actual.trtexecPath == requested.trtexecPath &&
           actual.trtexecLibDir == requested.trtexecLibDir &&
           actual.filterSpec == requested.filterSpec;
}

bool hasSelectedFilterLabel(const std::optional<ProbePropertyValue>& vf) {
    return vf && vf->kind == ProbePropertyValue::Kind::Text &&
           vf->text.contains(QStringLiteral("@mikai_aji"), Qt::CaseSensitive);
}

bool compatibleHwdec(const std::optional<ProbePropertyValue>& hwdec, native::Backend backend) {
    if (!hwdec || hwdec->kind != ProbePropertyValue::Kind::Text) return false;
    const QString decoder = hwdec->text.trimmed().toLower();
    if (decoder.isEmpty() || decoder == QLatin1String("no") ||
        decoder.endsWith(QLatin1String("-copy")) || decoder.contains(QLatin1String("software")))
        return false;
    return backend == native::Backend::TensorRT
        ? decoder == QLatin1String("nvdec")
        : decoder == QLatin1String("d3d11va");
}

bool selectedEngineUseLogLine(const QString& line, const QString& spatialPrefix, bool rife) {
    return rife
        ? line.contains(QStringLiteral("RIFE active:"), Qt::CaseInsensitive) &&
              line.contains(QStringLiteral("2/1"), Qt::CaseInsensitive)
        : line.contains(spatialPrefix, Qt::CaseInsensitive);
}

QStringList selectedRifeStatsEvidence(const native::AnimeJanaiPlan& plan) {
    QFile config(plan.configPath);
    QFile stats(plan.statsPath);
    if (!config.open(QIODevice::ReadOnly | QIODevice::Text) ||
        !stats.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    const QString contents = QString::fromUtf8(config.readAll());
    const QString key = QStringLiteral("chain_1_rife_model=");
    const int start = contents.indexOf(key);
    if (start < 0) return {};
    const int valueStart = start + key.size();
    int valueEnd = contents.indexOf(QLatin1Char('\n'), valueStart);
    if (valueEnd < 0) valueEnd = contents.size();
    const QString modelCode = contents.mid(valueStart, valueEnd - valueStart).trimmed();
    if (modelCode.isEmpty()) return {};
    const QStringList lines = QString::fromUtf8(stats.readAll()).split(QLatin1Char('\n'));
    QStringList evidence;
    const QString modelMarker = QStringLiteral("Applied RIFE v%1 Interpolation 2.000x")
                                   .arg(modelCode);
    for (const QString& line : lines) {
        if (line.contains(modelMarker, Qt::CaseInsensitive) &&
            line.contains(QStringLiteral("New Video FPS"), Qt::CaseInsensitive))
            evidence.append(line.trimmed());
    }
    return evidence;
}

QStringList selectedRifeGraphEvidence(const native::AnimeJanaiPlan& plan) {
    const native::AnimeJanaiPreset* selectedPreset = nullptr;
    for (const native::AnimeJanaiPreset& preset : native::animeJanaiPresets()) {
        if (preset.id == plan.presetId && preset.rife) {
            selectedPreset = &preset;
            break;
        }
    }
    if (selectedPreset == nullptr) return {};
    const native::RifeModelPin* selectedPin = nullptr;
    for (const native::RifeModelPin& pin : native::rifeModelPins()) {
        if (pin.fileName == selectedPreset->modelFile) {
            selectedPin = &pin;
            break;
        }
    }
    if (selectedPin == nullptr) return {};

    QSettings config(plan.configPath, QSettings::IniFormat);
    config.beginGroup(QStringLiteral("slot_%1").arg(plan.slot));
    bool modelCodeOk = false;
    const int configuredModelCode =
        config.value(QStringLiteral("chain_1_rife_model")).toString().toInt(&modelCodeOk);
    config.endGroup();
    const int expectedModelCode =
        native::rifeModelCode(selectedPin->version, selectedPin->variant);
    const QString graphPath = QDir(plan.rifeModelDir).filePath(selectedPin->fileName);
    const QFileInfo graphInfo(graphPath);
    if (!modelCodeOk || configuredModelCode != expectedModelCode ||
        !graphInfo.isFile() || graphInfo.size() != selectedPin->bytes)
        return {};
    const QString digest = fileSha256(graphPath);
    if (digest.compare(selectedPin->graphSha256, Qt::CaseInsensitive) != 0) return {};
    return {QStringLiteral("%1|%2|%3|model-code=%4")
                .arg(graphPath)
                .arg(graphInfo.size())
                .arg(digest)
                .arg(configuredModelCode)};
}

int observedFrameChanges(VideoSurface* surface, int durationMs) {
    QImage previous;
    int changes = 0;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < durationMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const QImage current = surface->grabFramebuffer();
        if (!previous.isNull() && changed(previous, current)) ++changes;
        previous = current;
        QThread::msleep(16);
    }
    return changes;
}

void result(bool ok, const char* description) {
    std::fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", description);
}

bool validateRifePlans(native::Backend backend) {
    const QVector<native::AnimeJanaiPreset> presets = native::animeJanaiPresets();
    int expectedCount = 0;
    int failures = 0;
    std::fprintf(stderr,
                 "[AnimeJanaiProbe] RIFE model-plan validation only: no filter is attached and no engine is built\n");
    for (const native::AnimeJanaiPreset& preset : presets) {
        if (!preset.rife) continue;
        ++expectedCount;
        const native::RifeModelPin* pin = nullptr;
        for (const native::RifeModelPin& candidate : native::rifeModelPins()) {
            if (candidate.fileName == preset.modelFile) {
                pin = &candidate;
                break;
            }
        }
        const native::AnimeJanaiPlan plan =
            native::prepareAnimeJanaiPlan(preset.id, backend);
        const QString graphPath =
            pin != nullptr ? QDir(plan.rifeModelDir).filePath(pin->fileName) : QString();
        const int expectedModelCode =
            pin != nullptr ? native::rifeModelCode(pin->version, pin->variant) : 0;
        const QString expectedEnsemble =
            pin != nullptr && pin->ensemble ? QStringLiteral("yes") : QStringLiteral("no");
        QString actualModelCode;
        QString actualEnsemble;
        bool configReadable = false;
        if (!plan.configPath.isEmpty()) {
            QSettings config(plan.configPath, QSettings::IniFormat);
            config.beginGroup(QStringLiteral("slot_%1").arg(preset.slot));
            actualModelCode = config.value(QStringLiteral("chain_1_rife_model")).toString();
            actualEnsemble = config.value(QStringLiteral("chain_1_rife_ensemble")).toString();
            config.endGroup();
            configReadable = config.status() == QSettings::NoError;
        }
        const QFileInfo graphInfo(graphPath);
        const bool graphVerified = pin != nullptr && graphInfo.isFile() &&
            graphInfo.size() == pin->bytes &&
            fileSha256(graphPath).compare(pin->graphSha256, Qt::CaseInsensitive) == 0;
        const bool exactConfig = configReadable &&
            actualModelCode == QString::number(expectedModelCode) &&
            actualEnsemble.compare(expectedEnsemble, Qt::CaseInsensitive) == 0;
        const bool filesExist = QFileInfo(plan.configPath).isFile() &&
            QFileInfo(plan.libraryPath).isFile() && QFileInfo(plan.trtexecPath).isFile() &&
            graphVerified;
        const bool valid = pin != nullptr && plan.enabled && plan.available &&
            plan.presetId == preset.id && plan.backend == backend &&
            !plan.configPath.isEmpty() && !plan.filterSpec.isEmpty() &&
            exactConfig && filesExist;
        std::fprintf(stderr,
                     "%s RIFE plan id=%s backend=%s available=%d slot=%d model-code=%s "
                     "ensemble=%s config=%s graph=%s reason=[%s] %s\n",
                     valid ? "PASS" : "FAIL", qPrintable(preset.id),
                     backend == native::Backend::TensorRT ? "tensorrt" : "directml",
                     plan.available ? 1 : 0, plan.slot, qPrintable(actualModelCode),
                     qPrintable(actualEnsemble), qPrintable(plan.configPath),
                     qPrintable(graphPath), qPrintable(plan.reasonKey), qPrintable(plan.reason));
        if (!valid) {
            ++failures;
            std::fprintf(stderr,
                         "[AnimeJanaiProbe] plan-checks enabled=%d id=%d backend=%d "
                         "filter=%d config=%d config-values=%d library=%d trtexec=%d "
                         "pinned-graph=%d expected-model-code=%d expected-ensemble=%s\n",
                         plan.enabled ? 1 : 0, plan.presetId == preset.id ? 1 : 0,
                         plan.backend == backend ? 1 : 0, plan.filterSpec.isEmpty() ? 0 : 1,
                         QFileInfo(plan.configPath).isFile() ? 1 : 0, exactConfig ? 1 : 0,
                         QFileInfo(plan.libraryPath).isFile() ? 1 : 0,
                         QFileInfo(plan.trtexecPath).isFile() ? 1 : 0,
                         graphVerified ? 1 : 0, expectedModelCode, qPrintable(expectedEnsemble));
        }
    }
    if (expectedCount == 0) {
        std::fprintf(stderr, "FAIL no RIFE presets are present in the AnimeJaNai catalogue\n");
        return false;
    }
    std::fprintf(stderr, "%s validated %d RIFE model plans; no engine build was requested\n",
                 failures == 0 ? "PASS" : "FAIL", expectedCount);
    return failures == 0;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    qInstallMessageHandler(captureLog);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Real embedded-libmpv AnimeJaNai integration probe"));
    parser.addHelpOption();
    QCommandLineOption presetOption(QStringList{QStringLiteral("p"), QStringLiteral("preset")},
                                    QStringLiteral("Prepared AnimeJaNai preset id"),
                                    QStringLiteral("id"));
    QCommandLineOption backendOption(QStringList{QStringLiteral("b"), QStringLiteral("backend")},
                                     QStringLiteral("Explicit backend: tensorrt or directml"),
                                     QStringLiteral("backend"), QStringLiteral("tensorrt"));
    QCommandLineOption importOption(QStringLiteral("install-local-runtime"),
                                    QStringLiteral("Import an existing complete AnimeJaNai runtime directory"),
                                    QStringLiteral("directory"));
    QCommandLineOption provisionOption(QStringLiteral("provision-runtime"),
                                       QStringLiteral("Explicitly provision the AnimeJaNai runtime closure"));
    QCommandLineOption rifeSourceOption(QStringLiteral("rife-source"),
                                        QStringLiteral("Import exact pinned RIFE graphs from an existing extraction root"),
                                        QStringLiteral("extraction-root"));
    QCommandLineOption spatialSourceOption(QStringLiteral("spatial-source"),
                                           QStringLiteral("Publish all exact catalogue ONNX files from an existing directory"),
                                           QStringLiteral("directory"));
    QCommandLineOption appNameOption(QStringLiteral("probe-app-name"),
                                     QStringLiteral("Isolated Qt application name used for probe storage"),
                                     QStringLiteral("name"), QStringLiteral("AnimeJanaiProbe"));
    QCommandLineOption naturalOption(QStringLiteral("natural-player"),
                                     QStringLiteral("Exercise the actual hostless PlayerDialog"));
    QCommandLineOption requireBuildOption(QStringLiteral("require-engine-build"),
                                          QStringLiteral("Require a fresh TensorRT engine build in this run"));
    QCommandLineOption validatePlansOption(QStringLiteral("validate-model-plans"),
                                           QStringLiteral("Validate every real prepared RIFE plan; does not build engines"));
    parser.addOption(presetOption);
    parser.addOption(backendOption);
    parser.addOption(importOption);
    parser.addOption(provisionOption);
    parser.addOption(rifeSourceOption);
    parser.addOption(spatialSourceOption);
    parser.addOption(appNameOption);
    parser.addOption(naturalOption);
    parser.addOption(requireBuildOption);
    parser.addOption(validatePlansOption);
    parser.addPositionalArgument(QStringLiteral("video"), QStringLiteral("Local video file"));
    parser.addPositionalArgument(QStringLiteral("preset"), QStringLiteral("Prepared preset id"),
                                 QStringLiteral("[preset]"));
    parser.addPositionalArgument(QStringLiteral("backend"), QStringLiteral("Backend: tensorrt or directml"),
                                 QStringLiteral("[backend]"));
    parser.process(app);
    const QString appName = parser.value(appNameOption).trimmed();
    if (appName.isEmpty()) {
        std::fprintf(stderr, "FAIL --probe-app-name must not be empty\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiAnimeJanaiProbe"));
    QCoreApplication::setApplicationName(appName);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    const QString actualAppLocalData =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString actualNativeRoot = native::nativeRoot();
    if (actualAppLocalData.isEmpty() || actualNativeRoot.isEmpty() ||
        !QDir().mkpath(actualAppLocalData) || !QDir().mkpath(actualNativeRoot)) {
        std::fprintf(stderr, "FAIL unable to create isolated probe app-local/native root: %s / %s\n",
                     qPrintable(actualAppLocalData), qPrintable(actualNativeRoot));
        return 1;
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, actualAppLocalData);
    std::fprintf(stderr, "[AnimeJanaiProbe] probe-app-name=%s app-local-data=%s native-root=%s\n",
                 qPrintable(appName), qPrintable(actualAppLocalData), qPrintable(actualNativeRoot));
    const bool validatePlansOnly = parser.isSet(validatePlansOption);
    const QStringList positional = parser.positionalArguments();
    if (positional.isEmpty() && !validatePlansOnly) {
        std::fprintf(stderr, "SKIP missing prerequisite: provide a local video file\n");
        return 77;
    }
    const QString file = positional.isEmpty() ? QString() : positional.first();
    if (!validatePlansOnly && !QFileInfo(file).isFile()) {
        std::fprintf(stderr, "SKIP missing prerequisite: video file does not exist: %s\n",
                     qPrintable(file));
        return 77;
    }
    if (validatePlansOnly &&
        (parser.isSet(naturalOption) || parser.isSet(requireBuildOption) ||
         parser.isSet(presetOption) || positional.size() > 1)) {
        std::fprintf(stderr,
                     "FAIL --validate-model-plans is plan-only and cannot be combined with "
                     "natural playback, an engine-build requirement, or a selected preset\n");
        return 1;
    }
    const QString backendArg =
        (parser.isSet(backendOption) ? parser.value(backendOption)
                                     : (positional.size() > 2 ? positional.at(2)
                                                              : QStringLiteral("tensorrt"))).toLower();
    native::Backend backend = native::Backend::TensorRT;
    if (backendArg == QStringLiteral("tensorrt")) backend = native::Backend::TensorRT;
    else if (backendArg == QStringLiteral("directml")) backend = native::Backend::DirectML;
    else {
        std::fprintf(stderr, "FAIL --backend must be tensorrt or directml (received %s)\n",
                     qPrintable(backendArg));
        return 1;
    }
    if ((parser.isSet(importOption) && parser.isSet(provisionOption)) ||
        (parser.isSet(rifeSourceOption) && parser.value(rifeSourceOption).trimmed().isEmpty()) ||
        (parser.isSet(spatialSourceOption) && parser.value(spatialSourceOption).trimmed().isEmpty())) {
        std::fprintf(stderr, "FAIL mutually exclusive runtime operations or empty fixture source\n");
        return 1;
    }
    if (!mplayer::probes::selectRuntime()) {
        std::fprintf(stderr, "SKIP missing prerequisite: no selected embedded libmpv runtime\n");
        return 77;
    }
    const native::RuntimeSelection& selection = native::currentSelection();
    std::fprintf(stderr, "[AnimeJanaiProbe] loaded=%d kind=%s version=%s module=%s\n",
                 selection.loaded ? 1 : 0, qPrintable(native::bundleKindName(selection.kind)),
                 qPrintable(selection.version), qPrintable(selection.modulePath));

    if (parser.isSet(naturalOption)) {
        native::NativePreferencePatch stale;
        stale.enabled = true;
        stale.recipeId = QStringLiteral("janai.legacy.unavailable");
        QString reason;
        if (!native::saveNativePreferences(stale, &reason)) {
            std::fprintf(stderr, "FAIL isolated legacy preference setup: %s\n", qPrintable(reason));
            return 1;
        }
        QSettings settings;
        settings.setValue(QStringLiteral("player/native/shortcuts/toggleProcessing"),
                          QStringLiteral("Ctrl+Shift+J"));
        settings.sync();
    }

    const bool hasPreset = parser.isSet(presetOption) || positional.size() > 1;
    const QString presetId = parser.isSet(presetOption) ? parser.value(presetOption)
                                                       : (positional.size() > 1 ? positional.at(1)
                                                                                : QString());
    const auto progress = [](const native::ComponentProvisionProgress& item) {
        std::fprintf(stderr, "[AnimeJanaiProbe] component=%s received=%lld total=%lld\n",
                     qPrintable(item.pack), static_cast<long long>(item.received),
                     static_cast<long long>(item.total));
    };
    if (parser.isSet(importOption) || parser.isSet(provisionOption)) {
        native::ComponentProvisionResult operation;
        if (parser.isSet(importOption)) {
            operation = native::importAnimeJanaiRuntime(parser.value(importOption), backend,
                                                        progress, [] { return false; });
        } else {
            operation = native::provisionAnimeJanaiRuntime(backend, progress, [] { return false; });
        }
        if (!operation.ok) {
            std::fprintf(stderr, "FAIL AnimeJaNai runtime operation [%s]: %s\n",
                         qPrintable(operation.reasonKey), qPrintable(operation.reason));
            return 1;
        }
        std::fprintf(stderr, "PASS AnimeJaNai runtime operation installed %d component pack(s)\n",
                     static_cast<int>(operation.installedPacks.size()));
    }
    if (parser.isSet(rifeSourceOption)) {
        QString reason;
        const QString root = QFileInfo(parser.value(rifeSourceOption)).absoluteFilePath();
        if (!native::installRifeGraphs(native::rifeArchiveStore(), native::rifeModelPins(),
                                       root, [] { return false; }, &reason)) {
            std::fprintf(stderr, "FAIL exact pinned RIFE fixture import from %s: %s\n",
                         qPrintable(root), qPrintable(reason));
            return 1;
        }
        std::fprintf(stderr, "PASS exact pinned RIFE graph inventory imported from %s\n",
                     qPrintable(root));
    }
    if (parser.isSet(spatialSourceOption)) {
        const QString source = QFileInfo(parser.value(spatialSourceOption)).absoluteFilePath();
        native::SpatialAssetStore store;
        for (const native::SpatialAsset& asset : native::spatialAssetCatalog()) {
            QFile graph(QDir(source).filePath(asset.name));
            if (!graph.open(QIODevice::ReadOnly)) {
                std::fprintf(stderr, "FAIL spatial fixture missing/unreadable catalogue file %s\n",
                             qPrintable(graph.fileName()));
                return 1;
            }
            const QByteArray bytes = graph.readAll();
            QString reasonKey;
            QString reason;
            const native::SpatialAssetPublication published =
                store.publish(asset, bytes, &reasonKey, &reason);
            if (!published.ok) {
                std::fprintf(stderr, "FAIL spatial fixture %s [%s]: %s\n",
                             qPrintable(asset.name), qPrintable(reasonKey), qPrintable(reason));
                return 1;
            }
            std::fprintf(stderr, "PASS exact spatial fixture %s bytes=%lld sha256=%s licence=%s path=%s\n",
                         qPrintable(asset.name), static_cast<long long>(asset.bytes),
                         qPrintable(asset.sha256), qPrintable(native::spatialAssetLicense().spdx),
                         qPrintable(published.graphPath));
        }
    }
    if (validatePlansOnly) return validateRifePlans(backend) ? 0 : 1;

    QPointer<PlayerDialog> naturalDialog;
    QPointer<VideoSurface> naturalSurface;
    QWidget window;
    VideoSurface* surface = nullptr;
    if (parser.isSet(naturalOption)) {
        naturalDialog = new PlayerDialog(QStringLiteral("AnimeJaNai natural-player probe"),
                                         QFileInfo(file).absoluteFilePath());
        naturalDialog->resize(800, 450);
        naturalDialog->show();
        surface = naturalDialog->findChild<VideoSurface*>(QStringLiteral("videoSurface"));
        naturalSurface = surface;
        if (surface == nullptr) {
            std::fprintf(stderr, "SKIP missing prerequisite: PlayerDialog has no main VideoSurface\n");
            naturalDialog->close();
            return 77;
        }
    } else {
        window.setWindowTitle(QStringLiteral("AnimeJaNai embedded-core probe"));
        surface = new VideoSurface(&window);
        surface->setObjectName(QStringLiteral("animeJanaiProbeSurface"));
        auto* layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(surface);
        window.resize(960, 540);
        window.show();
    }
    int failures = 0;
    bool ajiInconclusive = false;
    bool buildActive = false;
    bool buildWindowVisible = false;
    bool buildIndeterminate = false;
    bool buildWindowAbovePlayer = false;
    bool buildElapsedAdvanced = false;
    bool buildScreenshot = false;
    bool buildCompletionVisible = false;
    QString firstElapsedText;
    EngineBuildPlaybackEvidence buildPlayback;
    bool buildFailureObserved = false;
    bool buildFailureTimerSampled = false;
    bool buildFailureTimerStopped = false;
    QTimer buildWindowSampler;
    buildWindowSampler.setInterval(250);
    QObject::connect(&buildWindowSampler, &QTimer::timeout, &window, [&] {
        if (!buildActive || naturalDialog == nullptr) return;
        buildPlayback.sample(surface, actualNativeRoot);
        auto* overlay = naturalDialog->findChild<shell::ProcessingOverlay*>();
        if (overlay == nullptr || !overlay->isVisible()) return;
        for (QLabel* label : overlay->findChildren<QLabel*>()) {
            if (!label->text().contains(QRegularExpression(QStringLiteral("\\d\\d:\\d\\d:\\d\\d"))))
                continue;
            if (firstElapsedText.isEmpty()) firstElapsedText = label->text();
            else buildElapsedAdvanced |= label->text() != firstElapsedText;
        }
    });
    QObject::connect(surface, &VideoSurface::animeJanaiEngineBuildChanged, &window,
                     [&](bool active, bool succeeded, const QString& detail) {
        buildActive = active;
        if (naturalDialog == nullptr) return;
        auto* overlay = naturalDialog->findChild<shell::ProcessingOverlay*>();
        if (active) {
            buildPlayback.begin();
            buildPlayback.sample(surface, actualNativeRoot);
            buildWindowVisible |= naturalDialog->enginePreparationVisible();
            QProgressBar* bar = overlay != nullptr ? overlay->findChild<QProgressBar*>() : nullptr;
            buildIndeterminate |= bar != nullptr && bar->isVisible() && bar->minimum() == 0 && bar->maximum() == 0;
            buildWindowAbovePlayer |= overlay != nullptr && overlay->isWindow() &&
                overlay->windowFlags().testFlag(Qt::WindowStaysOnTopHint);
            if (overlay != nullptr && !buildScreenshot)
                buildScreenshot = overlay->grab().save(
                    QDir(actualNativeRoot).filePath(QStringLiteral("animejanai-probe-build-progress.png")));
            buildWindowSampler.start();
        } else {
            buildWindowSampler.stop();
            if (buildPlayback.armed && buildPlayback.started && !buildPlayback.completed) {
                buildPlayback.completed = true;
                buildPlayback.succeeded = succeeded && !detail.isEmpty();
                buildPlayback.completionObservedProcessing =
                    succeeded && surface->animeJanaiProcessingObserved();
            }
            if (succeeded && overlay != nullptr && overlay->isVisible() && !detail.isEmpty()) {
                for (QLabel* label : overlay->findChildren<QLabel*>())
                    buildCompletionVisible |= label->isVisible() && label->text().contains(detail);
            } else if (!succeeded && buildPlayback.armed && buildPlayback.started &&
                       !buildPlayback.succeeded && !buildFailureObserved) {
                buildFailureObserved = true;
                QPointer<shell::ProcessingOverlay> failedOverlay = overlay;
                QString elapsedAtFailure;
                if (overlay != nullptr) {
                    for (QLabel* label : overlay->findChildren<QLabel*>())
                        if (label->text().contains(QRegularExpression(QStringLiteral("\\d\\d:\\d\\d:\\d\\d"))))
                            elapsedAtFailure = label->text();
                }
                QTimer::singleShot(1200, &window, [&, failedOverlay, elapsedAtFailure, detail] {
                    buildFailureTimerSampled = true;
                    if (failedOverlay == nullptr || !failedOverlay->isVisible() ||
                        elapsedAtFailure.isEmpty()) return;
                    QProgressBar* bar = failedOverlay->findChild<QProgressBar*>();
                    bool elapsedUnchanged = false;
                    bool failureReasonVisible = false;
                    for (QLabel* label : failedOverlay->findChildren<QLabel*>()) {
                        elapsedUnchanged |= label->isVisible() && label->text() == elapsedAtFailure;
                        failureReasonVisible |= !detail.isEmpty() && label->isVisible() &&
                            label->text().contains(detail);
                    }
                    buildFailureTimerStopped = elapsedUnchanged && failureReasonVisible &&
                        bar != nullptr && bar->isVisible() && bar->minimum() == 0 &&
                        bar->maximum() == 1 && bar->value() == 0;
                });
            }
        }
        std::fprintf(stderr, "[engine-operation] active=%d succeeded=%d visible=%d indeterminate=%d detail=%s\n",
                     active, succeeded, naturalDialog->enginePreparationVisible(), buildIndeterminate,
                     qPrintable(detail));
    });
    QStringList animeJanaiFailures;
    QObject::connect(surface, &VideoSurface::animeJanaiFailure, &window,
                     [&animeJanaiFailures](const QString& reason) {
                         animeJanaiFailures.append(reason);
                         std::fprintf(stderr, "[AnimeJaNai processing failure] %s\n",
                                      qPrintable(reason));
                     });
    QStringList mediaErrors;
    QObject::connect(surface, &VideoSurface::errorOccurred, &window,
                     [&mediaErrors](const QString& message) { mediaErrors.append(message); });
    QStringList privateNativeFailures;
    QObject::connect(surface, &VideoSurface::nativeProcessingFailure, &window,
                     [&privateNativeFailures](const QString& key, const QString& reason) {
                         privateNativeFailures.append(key + QStringLiteral(": ") + reason);
                     });
    if (parser.isSet(naturalOption)) {
        // Match the application's normal host flow: show the dialog, then ask
        // PlayerDialog to open its source. The hostless constructor only stores
        // the URL; it does not start playback on its own.
        naturalDialog->startPlayer();
        if (!waitUntil([&] { return naturalDialog->isVisible() &&
                                    naturalDialog->isFullScreen(); }, 5000)) {
            std::fprintf(stderr,
                         "FAIL natural PlayerDialog did not complete deferred fullscreen startup\n");
            naturalDialog->close();
            return 1;
        }
        auto* playerShell = naturalDialog->findChild<shell::PlayerShell*>();
        if (playerShell == nullptr) {
            std::fprintf(stderr, "FAIL natural PlayerDialog has no PlayerShell\n");
            naturalDialog->close();
            return 1;
        }
        // Exercise the same user-intent path as the window-mode button only
        // after showEvent has applied the constructor's deferred fullscreen.
        playerShell->windowModeToggled();
        const QSize narrowSize = QSize(800, 450).expandedTo(naturalDialog->minimumSize());
        naturalDialog->resize(narrowSize);
        const bool narrowWindowReady = waitUntil([&] {
            return naturalDialog->isVisible() && !naturalDialog->isFullScreen() &&
                   naturalDialog->size() == narrowSize;
        }, 5000);
        const QRect actualHost(naturalDialog->mapToGlobal(QPoint(0, 0)),
                               naturalDialog->size());
        std::fprintf(stderr,
                     "[AnimeJanaiProbe] narrow-window mode=%s actual-host=%dx%d "
                     "global=(%d,%d) requested=800x450 dpr=%.2f\n",
                     naturalDialog->isFullScreen() ? "fullscreen" : "windowed",
                     naturalDialog->width(), naturalDialog->height(), actualHost.x(),
                     actualHost.y(), naturalDialog->devicePixelRatioF());
        result(narrowWindowReady,
               "natural PlayerDialog entered its actual minimum-supported windowed size after startup");
        if (!narrowWindowReady) {
            naturalDialog->close();
            return 1;
        }
    }
    if (!parser.isSet(naturalOption) &&
        !surface->open(QFileInfo(file).absoluteFilePath())) {
        std::fprintf(stderr, "FAIL embedded libmpv rejected the source before dispatch: %s\n",
                     qPrintable(mediaErrors.isEmpty() ? QStringLiteral("no surface error reported")
                                                      : mediaErrors.constLast()));
        surface->close();
        return 1;
    }
    if (!surface->waitForContext(10000)) {
        std::fprintf(stderr, "FAIL Qt OpenGL render context unavailable after source dispatch; "
                             "contextReady=%d source=%s\n",
                     surface->isContextReady() ? 1 : 0,
                     qPrintable(QFileInfo(file).absoluteFilePath()));
        surface->close();
        return 1;
    }
    if (!waitUntil([&] { return surface->hasVideo(); }, 30000)) {
        std::fprintf(stderr,
                     "FAIL embedded libmpv did not decode the supplied file within 30s; "
                     "contextReady=%d hasVideo=%d source=%s",
                     surface->isContextReady() ? 1 : 0, surface->hasVideo() ? 1 : 0,
                     qPrintable(QFileInfo(file).absoluteFilePath()));
        if (!mediaErrors.isEmpty())
            std::fprintf(stderr, " last-error=%s", qPrintable(mediaErrors.constLast()));
        std::fprintf(stderr, "\n");
        const QStringList logs = capturedLogs();
        const int first = qMax(0, logs.size() - 30);
        for (const QString& line : logs.mid(first))
            std::fprintf(stderr, "[AnimeJanaiProbe] decode-context: %s\n", qPrintable(line));
        surface->close();
        return 1;
    }
    result(surface->isContextReady() && surface->videoWidth() > 0 && surface->videoHeight() > 0,
           "VideoSurface decoded a real video frame through vo=libmpv");
    if (!surface->isContextReady() || surface->videoWidth() <= 0 || surface->videoHeight() <= 0)
        ++failures;
    const auto actualCoreVersion =
        surface->probeMpvProperty(QStringLiteral("mpv-version"), ProbePropertyKind::Text);
    if (actualCoreVersion) {
        std::fprintf(stderr, "[AnimeJanaiProbe] actual libmpv API version: %s\n",
                     qPrintable(actualCoreVersion->text));
    } else {
        std::fprintf(stderr, "SKIP actual core-version observation: mpv-version property unavailable\n");
    }

    bool seekObserved = false;
    if (parser.isSet(naturalOption)) {
        const auto sendDialogKey = [&](int key) {
            QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
            QCoreApplication::sendEvent(naturalDialog, &press);
            QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
            QCoreApplication::sendEvent(naturalDialog, &release);
        };
        surface->setPaused(false);
        sendDialogKey(Qt::Key_Space);
        const bool keyboardPauseObserved = waitUntil([&] {
            const auto paused = surface->probeMpvProperty(QStringLiteral("pause"),
                                                          ProbePropertyKind::Flag);
            return paused && paused->kind == ProbePropertyValue::Kind::Flag && paused->flag;
        }, 5000);
        result(keyboardPauseObserved, "PlayerDialog Space key paused the embedded source");
        if (!keyboardPauseObserved) ++failures;
        const double beforeSeek = surface->position();
        sendDialogKey(Qt::Key_Right);
        seekObserved = waitUntil([&] { return surface->position() > beforeSeek + 0.25; }, 5000);
    } else {
        surface->setPaused(true);
        const double beforeSeek = surface->position();
        surface->seek(2.0);
        seekObserved = waitUntil([&] { return surface->position() > beforeSeek + 0.25; }, 5000);
    }
    result(seekObserved, parser.isSet(naturalOption)
        ? "PlayerDialog Right key sought the observed source position"
        : "pause and seek changed the observed source position");
    if (!seekObserved) ++failures;
    surface->setVolume(37.0);
    surface->setMuted(true);
    const bool volumeObserved = waitUntil([&] {
        const auto volume = surface->probeMpvProperty(QStringLiteral("volume"), ProbePropertyKind::Number);
        return volume && volume->isNumeric() && std::abs(volume->asNumber() - 37.0) < 0.1;
    }, 5000);
    result(volumeObserved, "volume control reached the embedded mpv core (audio is muted during probe)");
    if (!volumeObserved) ++failures;
    surface->setMuted(false);
    surface->setPaused(false);
    const QImage frameA = surface->grabFramebuffer();
    QThread::msleep(300);
    QCoreApplication::processEvents();
    const QImage frameB = surface->grabFramebuffer();
    const bool moving = changed(frameA, frameB);
    result(moving, "moving video frames reached the actual OpenGL framebuffer");
    if (!moving) ++failures;

    const auto audioId = surface->probeMpvProperty(QStringLiteral("aid"), ProbePropertyKind::Number);
    const auto subtitleId = surface->probeMpvProperty(QStringLiteral("sid"), ProbePropertyKind::Number);
    exerciseTrack(surface, QStringLiteral("aid"), audioId, "audio", &failures);
    exerciseTrack(surface, QStringLiteral("sid"), subtitleId, "subtitle", &failures);

    reportPlaybackMetrics(surface, "baseline");
    const int baselinePtsChanges = observedPtsChanges(surface, 3000);
    const uint64_t baselineRenderedOutputFrames = observedRenderedOutputFrames(surface, 3000);
    const PlaybackDiagnostics baselineDiagnostics = surface->playbackDiagnostics();
    std::fprintf(stderr, "[AnimeJanaiProbe] baseline observed time-pos transitions/3s=%d (sampling evidence, not an FPS claim)\n",
                 baselinePtsChanges);
    std::fprintf(stderr, "[AnimeJanaiProbe] baseline successful mpv-rendered output-frame updates/3s=%llu\n",
                 static_cast<unsigned long long>(baselineRenderedOutputFrames));
    std::fprintf(stderr, "[AnimeJanaiProbe] baseline estimated-vf-fps=%s\n",
                 baselineDiagnostics.estimatedVfFps
                     ? QByteArray::number(*baselineDiagnostics.estimatedVfFps, 'f', 3).constData()
                     : "unavailable");
    if (naturalDialog) {
        naturalDialog->activateWindow();
        QCoreApplication::processEvents();
        QTest::keyClick(naturalDialog.data(), Qt::Key_J, Qt::ControlModifier | Qt::ShiftModifier);
        QCoreApplication::processEvents();
        const bool ignoredLegacyKey = native::loadNativePreferences().enabled &&
                                      !surface->animeJanaiPlan().enabled;
        result(ignoredLegacyKey, "the retired native key neither toggled the stored request nor activated AnimeJaNai");
        result(!surface->animeJanaiProcessingObserved(),
               "no processing observation is claimed before an AnimeJaNai preset runs");
        if (surface->animeJanaiProcessingObserved()) ++failures;
        if (!ignoredLegacyKey) ++failures;
        if (selection.kind == native::BundleKind::Standard) {
            const auto filters =
                surface->probeMpvProperty(QStringLiteral("vf"), ProbePropertyKind::Text);
            const bool noPrivateFilter = filters &&
                !filters->text.contains(QStringLiteral("mikai_native"));
            result(noPrivateFilter,
                   "a stale private request left Standard playback free of the private filter");
            if (!noPrivateFilter) ++failures;
        }
    }
    native::AnimeJanaiPlan plan;
    bool rifePreset = false;
    if (parser.isSet(requireBuildOption) &&
        (!parser.isSet(naturalOption) || backend != native::Backend::TensorRT || !hasPreset)) {
        std::fprintf(stderr, "FAIL --require-engine-build requires --natural-player, --backend tensorrt, and a preset\n");
        ++failures;
    }
    const auto hwdecBeforeAnimeJanai =
        surface->probeMpvProperty(QStringLiteral("hwdec"), ProbePropertyKind::Text);
    if (hasPreset) {
        const QString artifactModelDir = QDir(actualNativeRoot).filePath(
            QStringLiteral("animejanai/models/") +
            (presetId.startsWith(QStringLiteral("rife:"))
                 ? QStringLiteral("rife") : QStringLiteral("spatial")));
        QMap<QString, QString> engineFilesBefore;
        QMap<QString, QString> buildLogsBefore;
        QString artifactSnapshotTimeUtc;
        if (parser.isSet(naturalOption) && parser.isSet(requireBuildOption)) {
            engineFilesBefore = engineSnapshot(artifactModelDir);
            buildLogsBefore = buildLogSnapshot(artifactModelDir);
            artifactSnapshotTimeUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            std::fprintf(stderr,
                         "[AnimeJanaiProbe] request-artifact-before root=%s model-directory=%s utc=%s engines=%lld build-logs=%lld\n",
                         qPrintable(actualNativeRoot), qPrintable(artifactModelDir),
                         qPrintable(artifactSnapshotTimeUtc), static_cast<long long>(engineFilesBefore.size()),
                         static_cast<long long>(buildLogsBefore.size()));
            for (auto it = engineFilesBefore.cbegin(); it != engineFilesBefore.cend(); ++it)
                std::fprintf(stderr, "[AnimeJanaiProbe] request-artifact-before-engine path=%s sha256=%s\n",
                             qPrintable(it.key()), qPrintable(it.value()));
            for (auto it = buildLogsBefore.cbegin(); it != buildLogsBefore.cend(); ++it)
                std::fprintf(stderr, "[AnimeJanaiProbe] request-artifact-before-build-log path=%s sha256=%s\n",
                             qPrintable(it.key()), qPrintable(it.value()));
        }
        if (parser.isSet(naturalOption)) {
            for (const native::AnimeJanaiPreset& candidate : native::animeJanaiPresets()) {
                if (candidate.id == presetId) {
                    plan.presetId = candidate.id;
                    plan.slot = candidate.slot;
                    rifePreset = candidate.rife;
                    plan.backend = backend;
                    plan.enabled = candidate.supported;
                    plan.available = candidate.supported;
                    break;
                }
            }
        } else {
            plan = prepareOnWorker(presetId, backend);
            rifePreset = presetId.startsWith(QStringLiteral("rife:"));
        }
        if (!plan.enabled || !plan.available) {
            ajiInconclusive = true;
            std::fprintf(stderr, "SKIP missing AnimeJaNai prerequisite [%s]: %s\n",
                         qPrintable(plan.reasonKey), qPrintable(plan.reason));
        } else if (!native::isAnimeJanaiForkRuntime(selection)) {
            ajiInconclusive = true;
            std::fprintf(stderr, "SKIP incompatible core: official fork d6d93599d5 ABI8 is required; loaded %s %s\n",
                         qPrintable(native::bundleKindName(selection.kind)), qPrintable(selection.version));
        } else {
            std::fprintf(stderr, "[AnimeJanaiProbe] backend=%s preset=%s slot=%d\n",
                         backend == native::Backend::TensorRT ? "TensorRT/CUDA" : "DirectML/D3D11VA",
                         qPrintable(plan.presetId), plan.slot);
            const int requestLogStart = capturedLogs().size();
            const int priorFailureCount = animeJanaiFailures.size();
            const QString buildRequestTimeUtc =
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            if (parser.isSet(requireBuildOption))
                std::fprintf(stderr,
                             "[AnimeJanaiProbe] fresh-build-request utc=%s before-snapshot-utc=%s root=%s model-directory=%s selected-preset=%s\n",
                             qPrintable(buildRequestTimeUtc), qPrintable(artifactSnapshotTimeUtc),
                             qPrintable(actualNativeRoot), qPrintable(artifactModelDir),
                             qPrintable(presetId));
            const quint64 priorPlanGeneration = naturalDialog
                ? naturalDialog->animeJanaiPlanGeneration() : 0;
            if (parser.isSet(naturalOption)) {
                const bool backendChosen = chooseEnhancementMenu(
                    naturalDialog.data(),
                    {QStringLiteral("level|animejanai"), QStringLiteral("level|animejanai:backend"),
                     QStringLiteral("ajbackend|") + backendArg});
                const QString selectorImage = QDir(actualNativeRoot).filePath(
                    QStringLiteral("animejanai-probe-rife-selector.png"));
                if (parser.isSet(requireBuildOption))
                    buildPlayback.arm(surface, baselinePtsChanges >= 3 && moving);
                const bool presetChosen = backendChosen &&
                    chooseAnimeJanaiPreset(naturalDialog.data(), presetId, selectorImage);
                result(presetChosen, "visible AnimeJaNai menu selection submitted the preset without an activation hotkey");
                if (!presetChosen) {
                    naturalDialog->close();
                    return 1;
                }
            } else {
                surface->setAnimeJanaiPlan(plan);
            }
            const auto requestedPlanMatches = [&] {
                if (naturalDialog &&
                    naturalDialog->animeJanaiPlanGeneration() <= priorPlanGeneration)
                    return false;
                const native::AnimeJanaiPlan live = surface->animeJanaiPlan();
                return live.enabled && live.presetId == plan.presetId &&
                       live.backend == backend && live.slot == plan.slot;
            };
            const auto requestedPlanIsLive = [&] {
                const native::AnimeJanaiPlan live = surface->animeJanaiPlan();
                if (!requestedPlanMatches() || !live.available ||
                    live.configPath.isEmpty() || !QFileInfo(live.configPath).isFile() ||
                    live.libraryPath.isEmpty() || !QFileInfo(live.libraryPath).isFile() ||
                    live.filterSpec.isEmpty())
                    return false;
                return parser.isSet(naturalOption) || samePreparedPlan(live, plan);
            };
            const auto requestedPlanFailed = [&] {
                return requestedPlanMatches() &&
                    (!surface->animeJanaiPlan().available ||
                     animeJanaiFailures.size() > priorFailureCount);
            };
            const auto requestedFilterIsLive = [&] {
                if (!requestedPlanIsLive()) return false;
                return hasSelectedFilterLabel(
                    surface->probeMpvProperty(QStringLiteral("vf"), ProbePropertyKind::Text));
            };
            QElapsedTimer buildClock;
            buildClock.start();
            int lastBuildNotice = -10000;
            const bool configured = waitUntil([&] {
                if (buildClock.elapsed() - lastBuildNotice >= 10000) {
                    lastBuildNotice = static_cast<int>(buildClock.elapsed());
                    std::fprintf(stderr, "[AnimeJanaiProbe] waiting for first engine/filter state (%lld s): %s\n",
                                 static_cast<long long>(buildClock.elapsed() / 1000),
                                 qPrintable(surface->animeJanaiStatus()));
                }
                return requestedPlanFailed() || requestedFilterIsLive();
            }, 600000);
            const bool accepted = configured && requestedFilterIsLive();
            if (!accepted) {
                const bool terminalFailure = requestedPlanFailed();
                std::fprintf(stderr, "%s AnimeJaNai setup: %s\n",
                             terminalFailure ? "FAIL" : "SKIP inconclusive",
                             qPrintable(surface->animeJanaiStatus()));
                if (terminalFailure || parser.isSet(requireBuildOption)) {
                    for (const QString& line : capturedLogsSince(requestLogStart))
                        std::fprintf(stderr, "[AnimeJanaiProbe] request-build-context: %s\n", qPrintable(line));
                }
                if (terminalFailure || parser.isSet(requireBuildOption)) ++failures;
                else ajiInconclusive = true;
            } else {
                std::fprintf(stderr, "[AnimeJanaiProbe] %s\n", qPrintable(surface->animeJanaiStatus()));
                QElapsedTimer coreStateClock;
                coreStateClock.start();
                int lastCoreNotice = -10000;
                const bool coreSettled = waitUntil([&] {
                    if (coreStateClock.elapsed() - lastCoreNotice >= 10000) {
                        lastCoreNotice = static_cast<int>(coreStateClock.elapsed());
                        std::fprintf(stderr, "[AnimeJanaiProbe] waiting for observed live vf membership (%lld s); status=%s\n",
                                     static_cast<long long>(coreStateClock.elapsed() / 1000),
                                     qPrintable(surface->animeJanaiStatus()));
                    }
                    return requestedFilterIsLive() || requestedPlanFailed();
                }, 300000);
                const bool coreAccepted = coreSettled && requestedFilterIsLive();
                const native::AnimeJanaiPlan livePlan = surface->animeJanaiPlan();
                const QString modelIdentity = selectedModelIdentity(livePlan);
                const QString successPrefix = QStringLiteral("Configured slot %1:").arg(plan.slot);
                QElapsedTimer engineUseClock;
                engineUseClock.start();
                int lastEngineUseNotice = -10000;
                const bool effectLogged = coreAccepted && waitUntil([&] {
                    if (engineUseClock.elapsed() - lastEngineUseNotice >= 10000) {
                        lastEngineUseNotice = static_cast<int>(engineUseClock.elapsed());
                        std::fprintf(stderr,
                                     "[AnimeJanaiProbe] waiting for observed configured engine use (%lld s); status=%s logs=%lld\n",
                                     static_cast<long long>(engineUseClock.elapsed() / 1000),
                                     qPrintable(surface->animeJanaiStatus()),
                                     static_cast<long long>(capturedLogsSince(requestLogStart).size()));
                    }
                    for (const QString& line : capturedLogsSince(requestLogStart))
                        if (selectedEngineUseLogLine(line, successPrefix, rifePreset)) return true;
                    return rifePreset && !selectedRifeStatsEvidence(livePlan).isEmpty();
                }, 300000);
                QStringList selectedEngineUseLogs;
                for (const QString& line : capturedLogsSince(requestLogStart))
                    if (selectedEngineUseLogLine(line, successPrefix, rifePreset))
                        selectedEngineUseLogs.append(line);
                for (const QString& line : selectedEngineUseLogs)
                    std::fprintf(stderr, "[AnimeJanaiProbe] selected-engine-use-log=%s\n",
                                 qPrintable(line));
                if (parser.isSet(requireBuildOption) && !effectLogged) {
                    std::fprintf(stderr, "[AnimeJanaiProbe] FAIL no request-bound selected-engine-use log matched the required spatial slot or RIFE active factor; request log follows\n");
                    for (const QString& line : capturedLogsSince(requestLogStart))
                        std::fprintf(stderr, "[AnimeJanaiProbe] request-build-context: %s\n", qPrintable(line));
                }
                if (parser.isSet(requireBuildOption)) {
                    const bool completionSettled = waitUntil([&] {
                        return buildPlayback.completed || requestedPlanFailed();
                    }, 10000);
                    const bool pauseProved = completionSettled && buildPlayback.proved() &&
                        !buildActive && effectLogged && buildCompletionVisible;
                    std::fprintf(stderr,
                                 "[engine-pause] summary started=%d original-playing=%d acknowledgement-ms=%lld held-ms=%lld pause-samples=%d framebuffer-samples=%d held=%d completion=%d successful=%d backend-processing-at-completion=%d held-image=%d source=%s\n",
                                 buildPlayback.started ? 1 : 0, buildPlayback.originallyPlaying ? 1 : 0,
                                 static_cast<long long>(buildPlayback.acknowledgedAt),
                                 static_cast<long long>(buildPlayback.lastHeldAt - buildPlayback.framebufferAt),
                                 buildPlayback.heldSamples, buildPlayback.framebufferSamples,
                                 buildPlayback.held ? 1 : 0, buildPlayback.completed ? 1 : 0,
                                 buildPlayback.succeeded ? 1 : 0,
                                 buildPlayback.completionObservedProcessing ? 1 : 0,
                                 buildPlayback.heldImageSaved ? 1 : 0, qPrintable(buildPlayback.corePath));
                    const QString heldImagePath = QDir(actualNativeRoot).filePath(
                        QStringLiteral("animejanai-probe-build-held.png"));
                    std::fprintf(stderr, "[engine-pause] held-frame-image=%s sha256=%s\n",
                                 qPrintable(heldImagePath), qPrintable(fileSha256(heldImagePath)));
                    result(pauseProved,
                           "real backend build held an originally playing mpv source paused with stable media time, source, and native framebuffer until successful visible completion");
                    if (!pauseProved) ++failures;
                    const bool resumedAfterCompletion = buildPlayback.completed &&
                        buildPlayback.succeeded && waitUntil([&] {
                            const auto pause = surface->probeMpvProperty(
                                QStringLiteral("pause"), ProbePropertyKind::Flag);
                            const auto time = numericProperty(surface, QStringLiteral("time-pos"));
                            const auto path = surface->probeMpvProperty(
                                QStringLiteral("path"), ProbePropertyKind::Text);
                            return pause && pause->kind == ProbePropertyValue::Kind::Flag &&
                                !pause->flag && time && std::isfinite(*time) && buildPlayback.heldTime &&
                                *time > *buildPlayback.heldTime + 0.05 &&
                                path && path->kind == ProbePropertyValue::Kind::Text &&
                                path->text == buildPlayback.corePath &&
                                surface->sourceEpoch() == buildPlayback.sourceEpoch &&
                                surface->sourceUrl() == buildPlayback.source;
                        }, 10000);
                    result(resumedAfterCompletion,
                           "the originally playing real source resumed automatically only after successful engine completion");
                    if (!resumedAfterCompletion) ++failures;
                }
                const uint64_t framesBeforeGeometrySettle =
                    surface->playbackDiagnostics().renderedOutputFrames;
                std::optional<uint64_t> framesAtExpectedGeometry;
                const bool outputGeometrySettled = waitUntil([&] {
                    const auto inputWidth = numericProperty(surface, QStringLiteral("video-params/w"));
                    const auto inputHeight = numericProperty(surface, QStringLiteral("video-params/h"));
                    const auto outputWidth = numericProperty(surface, QStringLiteral("video-out-params/w"));
                    const auto outputHeight = numericProperty(surface, QStringLiteral("video-out-params/h"));
                    if (!inputWidth || !inputHeight || !outputWidth || !outputHeight) {
                        framesAtExpectedGeometry.reset();
                        return false;
                    }
                    const bool expectedGeometry = rifePreset
                        ? *inputWidth == *outputWidth && *inputHeight == *outputHeight
                        : *outputWidth >= *inputWidth * 2.0 &&
                              *outputHeight >= *inputHeight * 2.0;
                    if (!expectedGeometry) {
                        framesAtExpectedGeometry.reset();
                        return false;
                    }
                    const uint64_t renderedFrames =
                        surface->playbackDiagnostics().renderedOutputFrames;
                    if (!framesAtExpectedGeometry) {
                        framesAtExpectedGeometry = renderedFrames;
                        return false;
                    }
                    return renderedFrames >= *framesAtExpectedGeometry + 3;
                }, 60000);
                const uint64_t framesAfterGeometrySettle =
                    surface->playbackDiagnostics().renderedOutputFrames;
                const uint64_t postGeometryFrameUpdates =
                    framesAtExpectedGeometry && framesAfterGeometrySettle >= *framesAtExpectedGeometry
                        ? framesAfterGeometrySettle - *framesAtExpectedGeometry
                        : framesAfterGeometrySettle >= framesBeforeGeometrySettle
                              ? framesAfterGeometrySettle - framesBeforeGeometrySettle : 0;
                std::fprintf(stderr,
                             "[AnimeJanaiProbe] postfilter geometry-settled=%d new-rendered-output-frames=%llu within-60s=%d\n",
                             outputGeometrySettled ? 1 : 0,
                             static_cast<unsigned long long>(postGeometryFrameUpdates),
                             outputGeometrySettled ? 1 : 0);
                const auto inputW = numericProperty(surface, QStringLiteral("video-params/w"));
                const auto inputH = numericProperty(surface, QStringLiteral("video-params/h"));
                const auto outputW = numericProperty(surface, QStringLiteral("video-out-params/w"));
                const auto outputH = numericProperty(surface, QStringLiteral("video-out-params/h"));
                const auto hwdec = surface->probeMpvProperty(QStringLiteral("hwdec-current"),
                                                              ProbePropertyKind::Text);
                const auto vf = surface->probeMpvProperty(QStringLiteral("vf"),
                                                           ProbePropertyKind::Text);
                const bool vfContainsAnimeJanai = hasSelectedFilterLabel(vf);
                const bool membershipKnown = vf && vf->kind == ProbePropertyValue::Kind::Text;
                const int processedPtsChanges = observedPtsChanges(surface, 3000);
                const uint64_t processedRenderedOutputFrames =
                    observedRenderedOutputFrames(surface, 3000);
                const PlaybackDiagnostics processedDiagnostics = surface->playbackDiagnostics();
                const QImage processedFrame = surface->grabFramebuffer();
                const QString imagePath = QDir(actualNativeRoot).filePath(
                    QStringLiteral("animejanai-probe-processed.png"));
                const bool imageSaved = processedFrame.save(imagePath, "PNG");
                const QString imageSha256 = imageSaved ? fileSha256(imagePath) : QString();
                QString guiImagePath;
                bool guiImageSaved = false;
                if (parser.isSet(naturalOption) && naturalDialog) {
                    const bool observedProcessing =
                        waitUntil([&] { return surface->animeJanaiProcessingObserved(); }, 5000);
                    result(observedProcessing,
                           "the HUD reports backend-observed processing for the selected plan");
                    if (!observedProcessing) ++failures;
                    auto* playerShell = naturalDialog->findChild<shell::PlayerShell*>();
                    if (playerShell != nullptr) playerShell->wake();
                    QCoreApplication::processEvents();
                    bool privateHudVisible = false;
                    for (auto* label : naturalDialog->findChildren<QLabel*>()) {
                        if (label->isVisible() &&
                            label->accessibleName().startsWith(QStringLiteral("Native")))
                            privateHudVisible = true;
                    }
                    result(!privateHudVisible,
                           "the Standard Janai HUD contains no unrelated private Native status");
                    if (privateHudVisible) ++failures;
                    std::fprintf(stderr, "[AnimeJanaiProbe] observed-player-status=%s\n",
                                 qPrintable(surface->animeJanaiStatus()));
                    guiImagePath = QDir(actualNativeRoot).filePath(
                        QStringLiteral("animejanai-probe-player-dialog.png"));
                    guiImageSaved = naturalDialog->grab().save(guiImagePath, "PNG");
                }
                const QString guiImageSha256 = guiImageSaved ? fileSha256(guiImagePath) : QString();
                const QString statsSha256 = fileSha256(livePlan.statsPath);
                const QStringList rifeStatsEvidence = rifePreset
                    ? selectedRifeStatsEvidence(livePlan) : QStringList{};
                const QStringList rifeGraphEvidence = rifePreset
                    ? selectedRifeGraphEvidence(livePlan) : QStringList{};
                const QString selectedPlanModelDir =
                    rifePreset ? livePlan.rifeModelDir : livePlan.modelDir;
                const QString selectedGraphPath = rifePreset
                    ? (rifeGraphEvidence.isEmpty()
                           ? QString()
                           : rifeGraphEvidence.constFirst().section(QLatin1Char('|'), 0, 0))
                    : QDir(selectedPlanModelDir).filePath(modelIdentity + QStringLiteral(".onnx"));
                const QStringList freshEngines = parser.isSet(requireBuildOption)
                    ? freshEngineFiles(artifactModelDir, engineFilesBefore) : QStringList{};
                const QStringList selectedBuildLogs =
                    parser.isSet(requireBuildOption)
                        ? selectedBuildLogEvidence(artifactModelDir, buildLogsBefore, selectedGraphPath)
                        : QStringList{};
                const QStringList selectedFreshEngines =
                    selectedFreshEngineFiles(freshEngines, selectedBuildLogs);
                std::fprintf(stderr,
                             "[AnimeJanaiProbe] live-config=%s model-family=%s model-directory=%s artifact-scan-directory=%s stats=%s\n",
                             qPrintable(livePlan.configPath), qPrintable(modelIdentity),
                             qPrintable(selectedPlanModelDir), qPrintable(artifactModelDir),
                             qPrintable(livePlan.statsPath));
                std::fprintf(stderr, "[AnimeJanaiProbe] stats-file=%s sha256=%s\n",
                             qPrintable(livePlan.statsPath), qPrintable(statsSha256));
                if (rifePreset)
                    std::fprintf(stderr,
                                 "[AnimeJanaiProbe] RIFE active factor plus request-bound model-family evidence; engine-to-active-model link is inferred from exact live config/fresh artifact because the core does not expose an engine identity event\n");
                for (const QString& proof : rifeStatsEvidence)
                    std::fprintf(stderr, "[AnimeJanaiProbe] selected-rife-stats-log=%s\n",
                                 qPrintable(proof));
                for (const QString& proof : rifeGraphEvidence)
                    std::fprintf(stderr, "[AnimeJanaiProbe] selected-rife-pinned-graph=%s\n",
                                 qPrintable(proof));
                for (const QString& proof : selectedFreshEngines)
                    std::fprintf(stderr, "[AnimeJanaiProbe] selected-fresh-engine-file-bytes-sha256=%s\n",
                                 qPrintable(proof));
                for (const QString& proof : selectedBuildLogs)
                    std::fprintf(stderr, "[AnimeJanaiProbe] selected-build-log-evidence=%s\n",
                                 qPrintable(proof));
                const bool modelIdentityKnown = !modelIdentity.isEmpty();
                const bool selectedGraphMatched = !selectedGraphPath.isEmpty() &&
                    (rifePreset ? !rifeGraphEvidence.isEmpty()
                                : QFileInfo(selectedGraphPath).isFile() &&
                                      fileSha256(selectedGraphPath).compare(
                                          modelIdentity, Qt::CaseInsensitive) == 0);
                const bool modelDirectoryMatches = !selectedPlanModelDir.isEmpty() &&
                    QFileInfo(selectedPlanModelDir).absoluteFilePath() ==
                        QFileInfo(artifactModelDir).absoluteFilePath();
                const bool modelDirectoryExists = QFileInfo(artifactModelDir).isDir();
                const bool freshEngineMatched = !selectedFreshEngines.isEmpty();
                const bool passedBuildLogMatched = !selectedBuildLogs.isEmpty();
                const bool rifeStatsMatched = !rifePreset || !rifeStatsEvidence.isEmpty();
                const bool rifePinnedGraphMatched = !rifePreset || selectedGraphMatched;
                const bool dialogImageMatched = guiImageSaved && !guiImagePath.isEmpty();
                std::fprintf(stderr,
                             "[AnimeJanaiProbe] request-bound-build-components model-identity=%d selected-graph-path-and-identity=%d plan-model-dir-match=%d scan-dir-exists=%d fresh-engine-with-bytes-sha256=%d changed-passed-build-log-with-exact-onnx-saveEngine=%d rife-live-model-stats=%d rife-pinned-graph=%d player-dialog-image=%d before-engine-count=%d before-log-count=%d\n",
                             modelIdentityKnown ? 1 : 0, selectedGraphMatched ? 1 : 0,
                             modelDirectoryMatches ? 1 : 0,
                             modelDirectoryExists ? 1 : 0, freshEngineMatched ? 1 : 0,
                             passedBuildLogMatched ? 1 : 0, rifeStatsMatched ? 1 : 0,
                             rifePinnedGraphMatched ? 1 : 0, dialogImageMatched ? 1 : 0,
                             static_cast<int>(engineFilesBefore.size()),
                             static_cast<int>(buildLogsBefore.size()));
                if (parser.isSet(requireBuildOption) &&
                    (!modelIdentityKnown || !selectedGraphMatched || !modelDirectoryMatches ||
                     !modelDirectoryExists || !freshEngineMatched || !passedBuildLogMatched ||
                     !rifeStatsMatched ||
                     !rifePinnedGraphMatched || !dialogImageMatched))
                    std::fprintf(stderr,
                                 "FAIL request-bound build evidence incomplete config=%s model-family=%s plan-model-directory=%s scan-directory=%s\n",
                                 qPrintable(livePlan.configPath), qPrintable(modelIdentity),
                                 qPrintable(selectedPlanModelDir), qPrintable(artifactModelDir));
                std::fprintf(stderr,
                             "[AnimeJanaiProbe] live output=%s x %s decoder-input=%s x %s hwdec-current=%s vf-text=%s selected-filter-label=%s\n",
                             outputW ? QByteArray::number(*outputW).constData() : "unavailable",
                             outputH ? QByteArray::number(*outputH).constData() : "unavailable",
                             inputW ? QByteArray::number(*inputW).constData() : "unavailable",
                             inputH ? QByteArray::number(*inputH).constData() : "unavailable",
                             hwdec ? qPrintable(hwdec->text) : "unavailable",
                             vf ? qPrintable(vf->text) : "unavailable",
                             membershipKnown
                                 ? (vfContainsAnimeJanai ? "present" : "absent")
                                 : "unavailable");
                std::fprintf(stderr, "[AnimeJanaiProbe] processed observed time-pos transitions/3s=%d successful mpv-rendered output-frame updates/3s=%llu image=%s sha256=%s saved=%d nonblack=%d player-dialog-image=%s sha256=%s saved=%d\n",
                             processedPtsChanges,
                             static_cast<unsigned long long>(processedRenderedOutputFrames),
                             qPrintable(imagePath), qPrintable(imageSha256),
                             imageSaved ? 1 : 0, nonBlack(processedFrame) ? 1 : 0,
                             qPrintable(guiImagePath), qPrintable(guiImageSha256),
                             guiImageSaved ? 1 : 0);
                reportPlaybackMetrics(surface, "postfilter");
                const bool actualHwdec = compatibleHwdec(hwdec, backend);
                const bool liveMember = vfContainsAnimeJanai;
                const bool outputGeometryKnown = inputW && inputH && outputW && outputH;
                const bool renderedPostfilter = liveMember && actualHwdec &&
                    outputGeometrySettled && postGeometryFrameUpdates >= 3 &&
                    imageSaved && nonBlack(processedFrame) &&
                    observedFrameChanges(surface, 3000) >= 3;
                if (parser.isSet(requireBuildOption)) {
                    const bool progressProved = buildWindowVisible && buildIndeterminate &&
                        buildWindowAbovePlayer && buildElapsedAdvanced && buildScreenshot &&
                        buildCompletionVisible;
                    result(progressProved,
                           "a real fresh engine build shows an on-top indeterminate operation, advancing elapsed time, and visible completion");
                    if (!progressProved) ++failures;
                }
                const bool freshBuildProved = !parser.isSet(requireBuildOption) ||
                    (modelIdentityKnown && selectedGraphMatched && modelDirectoryMatches &&
                     modelDirectoryExists && freshEngineMatched && passedBuildLogMatched && rifeStatsMatched &&
                     rifePinnedGraphMatched && dialogImageMatched);
                const bool spatialUpscaleObserved = rifePreset || (inputW && inputH && outputW && outputH &&
                    *outputW >= *inputW * 2.0 && *outputH >= *inputH * 2.0);
                bool effectObserved = coreAccepted && effectLogged && liveMember && renderedPostfilter &&
                                     spatialUpscaleObserved && freshBuildProved;
                if (rifePreset) {
                    const bool cadenceProved = baselineRenderedOutputFrames >= 5 &&
                        processedRenderedOutputFrames >= 5 &&
                        processedRenderedOutputFrames * 2 >= baselineRenderedOutputFrames * 3 &&
                        inputW && inputH && outputW && outputH &&
                        *inputW == *outputW && *inputH == *outputH;
                    std::fprintf(stderr,
                                 "[AnimeJanaiProbe] RIFE equal-resolution rendered-output cadence baseline-frames=%llu processed-frames=%llu over 3s (time-pos transitions baseline=%d processed=%d; raw estimated-vf-fps=%s) -> %s (minimum factor2 threshold=1.5x)\n",
                                 static_cast<unsigned long long>(baselineRenderedOutputFrames),
                                 static_cast<unsigned long long>(processedRenderedOutputFrames),
                                 baselinePtsChanges, processedPtsChanges,
                                 processedDiagnostics.estimatedVfFps
                                     ? QByteArray::number(*processedDiagnostics.estimatedVfFps, 'f', 3).constData()
                                     : "unavailable",
                                 cadenceProved ? "observed" : "not observed");
                    effectObserved = effectObserved && cadenceProved;
                }
                if (!membershipKnown || !outputGeometryKnown || !actualHwdec ||
                    (processedFrame.isNull() || !imageSaved)) {
                    ajiInconclusive = true;
                    std::fprintf(stderr, "SKIP requested AnimeJaNai proof is inconclusive: required core membership/geometry/hwdec/frame/cadence observation unavailable\n");
                } else if (!coreAccepted || !effectLogged || !liveMember || !renderedPostfilter ||
                           !effectObserved || !freshBuildProved) {
                    std::fprintf(stderr, "FAIL AnimeJaNai inference/render proof incomplete: core acceptance, live vf membership, observed output geometry/cadence, and changed nonblack framebuffer are required\n");
                    ++failures;
                } else {
                    result(true, "AnimeJaNai live filter membership and postfilter framebuffer/output effect observed; no FPS-improvement claim");
                }

                bool slotSwitchTested = false;
                std::fprintf(stderr,
                             "[AnimeJanaiProbe] post-effect checkpoint before builtin slot candidates "
                             "selected=%s slot=%d require-build=%d\n",
                             qPrintable(plan.presetId), plan.slot,
                             parser.isSet(requireBuildOption) ? 1 : 0);
                for (const native::AnimeJanaiPreset& candidate : native::animeJanaiPresets()) {
                    std::fprintf(stderr,
                                 "[AnimeJanaiProbe] builtin candidate id=%s slot=%d supported=%d rife=%d\n",
                                 qPrintable(candidate.id), candidate.slot,
                                 candidate.supported ? 1 : 0, candidate.rife ? 1 : 0);
                    if (parser.isSet(requireBuildOption)) break;
                    if (!candidate.supported || candidate.slot == plan.slot || candidate.rife) continue;
                    native::AnimeJanaiPlan alternate;
                    if (parser.isSet(naturalOption)) {
                        alternate.available = candidate.supported;
                        alternate.presetId = candidate.id;
                        alternate.slot = candidate.slot;
                        alternate.backend = backend;
                    } else {
                        alternate = prepareOnWorker(candidate.id, backend);
                    }
                    if (!alternate.available) continue;
                    slotSwitchTested = true;
                    const int priorAlternateFailureCount = animeJanaiFailures.size();
                    const quint64 priorAlternateGeneration = naturalDialog
                        ? naturalDialog->animeJanaiPlanGeneration() : 0;
                    if (parser.isSet(naturalOption)) {
                        std::fprintf(stderr,
                                     "[AnimeJanaiProbe] builtin slot menu starting id=%s\n",
                                     qPrintable(candidate.id));
                        const bool selectedBuiltin =
                            chooseAnimeJanaiPreset(naturalDialog.data(), candidate.id);
                        std::fprintf(stderr,
                                     "[AnimeJanaiProbe] builtin slot menu returned id=%s selected=%d\n",
                                     qPrintable(candidate.id), selectedBuiltin ? 1 : 0);
                        if (!selectedBuiltin) {
                            naturalDialog->close();
                            return 1;
                        }
                    } else {
                        surface->setAnimeJanaiPlan(alternate);
                    }
                    const auto alternateRequestMatches = [&] {
                        if (naturalDialog &&
                            naturalDialog->animeJanaiPlanGeneration() <= priorAlternateGeneration)
                            return false;
                        const native::AnimeJanaiPlan live = surface->animeJanaiPlan();
                        return live.enabled && live.presetId == candidate.id &&
                               live.backend == backend && live.slot == candidate.slot;
                    };
                    const auto alternatePlanIsLive = [&] {
                        const native::AnimeJanaiPlan live = surface->animeJanaiPlan();
                        if (!alternateRequestMatches() || !live.available ||
                            live.configPath.isEmpty() || live.filterSpec.isEmpty())
                            return false;
                        return parser.isSet(naturalOption) || samePreparedPlan(live, alternate);
                    };
                    const auto alternatePlanFailed = [&] {
                        return alternateRequestMatches() &&
                            (!surface->animeJanaiPlan().available ||
                             animeJanaiFailures.size() > priorAlternateFailureCount);
                    };
                    const auto switchedFilterIsLive = [&] {
                        return alternatePlanIsLive() &&
                            hasSelectedFilterLabel(surface->probeMpvProperty(
                                QStringLiteral("vf"), ProbePropertyKind::Text));
                    };
                    const bool switchSettled = waitUntil([&] {
                        return switchedFilterIsLive() || alternatePlanFailed();
                    }, 300000);
                    const bool switched = switchSettled && switchedFilterIsLive();
                    std::fprintf(stderr,
                                 "[AnimeJanaiProbe] builtin slot settled=%d id=%s\n",
                                 switched ? 1 : 0, qPrintable(candidate.id));
                    result(switched, "compatible builtin AnimeJaNai slot switch completed on the retained filter");
                    if (!switched) ++failures;
                    break;
                }
                if (!slotSwitchTested) {
                    std::fprintf(stderr, parser.isSet(requireBuildOption)
                        ? "SKIP builtin slot switch: omitted from required-build proof to avoid a second expensive engine build\n"
                        : "SKIP builtin slot switch: no other supported installed spatial preset is available\n");
                }
                if (parser.isSet(naturalOption)) {
                    std::fprintf(stderr, "[AnimeJanaiProbe] post-effect checkpoint before Off selection\n");
                    const bool selectedOff = chooseAnimeJanaiPreset(naturalDialog.data(), QString());
                    std::fprintf(stderr,
                                 "[AnimeJanaiProbe] post-effect checkpoint after Off selection selected=%d\n",
                                 selectedOff ? 1 : 0);
                    if (!selectedOff) {
                        naturalDialog->close();
                        return 1;
                    }
                } else {
                    native::AnimeJanaiPlan off;
                    surface->setAnimeJanaiPlan(off);
                }
                const bool removed = waitUntil([&] {
                    const auto chain = surface->probeMpvProperty(QStringLiteral("vf"),
                                                                 ProbePropertyKind::Text);
                    const auto restoredHwdec =
                        surface->probeMpvProperty(QStringLiteral("hwdec"), ProbePropertyKind::Text);
                    const bool absent = chain && chain->kind == ProbePropertyValue::Kind::Text &&
                        !hasSelectedFilterLabel(chain);
                    const bool decoderRestored = hwdecBeforeAnimeJanai && restoredHwdec &&
                        restoredHwdec->kind == ProbePropertyValue::Kind::Text &&
                        restoredHwdec->text.compare(hwdecBeforeAnimeJanai->text,
                                                   Qt::CaseInsensitive) == 0;
                    return absent && decoderRestored;
                }, 300000);
                result(removed, "AnimeJaNai Off removed only its labelled filter and restored hwdec");
                if (!removed) ++failures;
                result(!surface->animeJanaiProcessingObserved(),
                       "Off clears the prior backend processing observation");
                if (surface->animeJanaiProcessingObserved()) ++failures;
            }
        }
    } else {
        std::fprintf(stderr, "PLAYBACK-ONLY: no AnimeJaNai preset supplied; no inference PASS is claimed\n");
    }

    if (parser.isSet(requireBuildOption) && buildFailureObserved) {
        const bool sampledFailure = waitUntil([&] { return buildFailureTimerSampled; }, 2000);
        const bool stoppedOnFailure = sampledFailure && buildFailureTimerStopped;
        result(stoppedOnFailure,
               "the real failed engine operation stayed visible with stopped elapsed timer and non-animated failure bar");
        if (!stoppedOnFailure) ++failures;
    }

    if (naturalDialog && selection.kind == native::BundleKind::Standard) {
        for (const QString& failure : privateNativeFailures)
            std::fprintf(stderr, "[private-native processing failure] %s\n", qPrintable(failure));
        result(privateNativeFailures.isEmpty(),
               "stale private-native settings caused no processing error during Standard-runtime menu playback");
        if (!privateNativeFailures.isEmpty()) ++failures;
    }

    surface->close();
    surface->open(QFileInfo(file).absoluteFilePath());
    const bool changedSource = waitUntil([&] { return surface->hasVideo(); }, 15000);
    result(changedSource, "source change reopened the same real file without an external player or IPC");
    if (!changedSource) ++failures;
    result(!surface->animeJanaiProcessingObserved(),
           "a reopened Original source does not inherit the prior processing observation");
    if (surface->animeJanaiProcessingObserved()) ++failures;
    if (naturalDialog) {
        const bool acceptedClose = naturalDialog->close();
        const bool hiddenAfterClose = naturalDialog.isNull() || !naturalDialog->isVisible();
        const bool sourceStopped = naturalSurface.isNull() || !naturalSurface->hasVideo();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        const bool shutdownCompleted = naturalDialog.isNull() && naturalSurface.isNull();
        result(acceptedClose && hiddenAfterClose && sourceStopped && shutdownCompleted,
               "PlayerDialog accepted shutdown, stopped playback, and destroyed its embedded surface");
        if (!acceptedClose || !hiddenAfterClose || !sourceStopped || !shutdownCompleted) ++failures;
    } else {
        surface->close();
        result(!surface->hasVideo(), "explicit close stopped the embedded source");
        window.close();
    }
    QCoreApplication::processEvents();

    if (failures != 0) {
        std::fprintf(stderr, "FAIL AnimeJaNai GUI probe: %d assertion(s) failed\n", failures);
        return 1;
    }
    if (ajiInconclusive) {
        std::fprintf(stderr, "SKIP requested AnimeJaNai proof is inconclusive; exit 77\n");
        return 77;
    }
    if (!hasPreset) {
        std::fprintf(stderr, "PASS playback-only probe; requested AnimeJaNai processing was not tested\n");
        return 0;
    }
    std::fprintf(stderr, "PASS AnimeJaNai proof completed\n");
    return 0;
}
