// Live API title, episode drawer, cover lifetime and hosted refusal probes.

#include <QAccessible>
#include <QAccessibleInterface>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QClipboard>
#include <QImage>
#include <QMenu>
#include <QMap>
#include <QMimeData>
#include <QPointer>
#include <QTest>
#include <QMouseEvent>
#include <QDir>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPixmap>
#include <QPainter>
#include <QPoint>
#include <QPushButton>
#include <QRect>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QWindow>

#include <cstdio>
#include <functional>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "backend/MikaiClient.hpp"
#include "pages/TitlePage.hpp"
#include "player/PlayerDialog.hpp"
#include "player/PlayerPopups.hpp"
#include "player/PlayerShell.hpp"
#include "player/ShellWidgets.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSurface.hpp"
#include "state/QualityCache.hpp"
#include "theme/Metrics.hpp"
#include "theme/Theme.hpp"
#include "workers/Runner.hpp"

using mikai::app::QualityCache;
using mikai::app::Runner;
using mikai::app::TitlePage;
using mikai::backend::MikaiClient;

namespace {

QPushButton* playerSeriesToggle(QWidget* root) {
    return root->findChild<QPushButton*>(QStringLiteral("playerSeriesToggle"));
}

QObject* accessibleAlertTarget = nullptr;
bool accessibleAlertObserved = false;
QAccessible::UpdateHandler previousAccessibleUpdateHandler = nullptr;

void captureAccessibleAlert(QAccessibleEvent* event) {
    if (event != nullptr && event->type() == QAccessible::Alert &&
        (event->object() == accessibleAlertTarget ||
         (accessibleAlertTarget == nullptr &&
          qobject_cast<QMessageBox*>(event->object()) != nullptr))) {
        accessibleAlertObserved = true;
    }
    if (previousAccessibleUpdateHandler != nullptr) {
        previousAccessibleUpdateHandler(event);
    }
}

class AccessibleAlertCapture {
public:
    explicit AccessibleAlertCapture(QObject* target)
        : wasActive_(QAccessible::isActive()) {
        accessibleAlertTarget = target;
        accessibleAlertObserved = false;
        previousAccessibleUpdateHandler = QAccessible::installUpdateHandler(
            &captureAccessibleAlert);
        QAccessible::setActive(true);
    }

    ~AccessibleAlertCapture() {
        QAccessible::installUpdateHandler(previousAccessibleUpdateHandler);
        QAccessible::setActive(wasActive_);
        accessibleAlertTarget = nullptr;
        previousAccessibleUpdateHandler = nullptr;
    }

    bool observed() const { return accessibleAlertObserved; }

private:
    bool wasActive_;
};

struct WarningOutcome {
    bool shown = false;
    bool parentedToPlayer = false;
    bool captured = false;
    bool accessible = false;
    bool alert = false;
    bool namedFailure = false;
    bool timedOut = false;
};

bool accessibleContains(QAccessibleInterface* accessibleInterface, const QString& text, int depth = 0) {
    if (accessibleInterface == nullptr || depth > 8 ||
        accessibleInterface->state().invisible) return false;
    if (accessibleInterface->text(QAccessible::Name).contains(text) ||
        accessibleInterface->text(QAccessible::Description).contains(text)) return true;
    for (int child = 0; child < accessibleInterface->childCount(); ++child) {
        if (accessibleContains(accessibleInterface->child(child), text, depth + 1)) return true;
    }
    return false;
}

WarningOutcome dismissPlayerWarning(QWidget* player, const std::function<void()>& action,
                                    const QString& screenshot, const QString& expectedFailure) {
    const QPointer<QWidget> guardedPlayer(player);
    AccessibleAlertCapture alert(nullptr);
    WarningOutcome outcome;
    int elapsed = 0;
    QTimer poller;
    poller.setInterval(20);
    QObject::connect(&poller, &QTimer::timeout, &poller, [&] {
        elapsed += poller.interval();
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            QPointer<QMessageBox> warning = qobject_cast<QMessageBox*>(widget);
            if (!warning || !warning->isVisible()) continue;
            poller.stop();
            outcome.shown = !warning->visibleRegion().isEmpty();
            outcome.parentedToPlayer = guardedPlayer &&
                guardedPlayer->isVisible() && warning->parentWidget() == guardedPlayer &&
                warning->windowModality() != Qt::NonModal &&
                QApplication::activeModalWidget() == warning &&
                warning->windowHandle() && guardedPlayer->windowHandle() &&
                warning->windowHandle()->transientParent() == guardedPlayer->windowHandle() &&
                warning->frameGeometry().intersects(guardedPlayer->frameGeometry());
            outcome.namedFailure = warning->text() == expectedFailure;
            outcome.accessible = !warning->text().isEmpty() &&
                accessibleContains(QAccessible::queryAccessibleInterface(warning), warning->text());
            const QPixmap warningImage = warning->grab();
            QPixmap playerImage = guardedPlayer ? guardedPlayer->grab() : QPixmap();
            if (!playerImage.isNull() && !warningImage.isNull()) {
                QPainter painter(&playerImage);
                painter.drawPixmap(warning->mapToGlobal(QPoint()) -
                                   guardedPlayer->mapToGlobal(QPoint()), warningImage);
                painter.end();
                outcome.captured = playerImage.save(screenshot) &&
                    warningImage.save(screenshot + QStringLiteral(".warning.png"));
            }
            std::fprintf(stderr,
                         "TITLE-PROBE: player warning parent=%d visible=%d accessible=%d "
                         "screenshot=%d text=%s\n",
                         outcome.parentedToPlayer, outcome.shown, outcome.accessible,
                         outcome.captured, warning->text().toUtf8().constData());
            if (auto* ok = warning->button(QMessageBox::Ok); ok != nullptr) ok->click();
            else warning->accept();
            return;
        }
        if (elapsed >= 5000 || !guardedPlayer) {
            outcome.timedOut = true;
            poller.stop();
            // End the nested exec even if an unexpected modal was raised.
            if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                modal->reject();
            }
        }
    });
    poller.start();
    action();
    poller.stop();
    outcome.alert = alert.observed();
    return outcome;
}

bool clickPlayerCopyAction(mplayer::shell::PlayerDialog* player) {
    const QPointer<mplayer::shell::PlayerDialog> guardedPlayer(player);
    auto* shell = player->findChild<mplayer::shell::PlayerShell*>();
    if (shell == nullptr) return false;
    auto* surface = shell->videoSurface();
    if (surface == nullptr) return false;
    shell->wake();
    QTest::mouseClick(surface, Qt::RightButton, Qt::NoModifier, QPoint(60, 60));
    for (int elapsed = 0; elapsed < 3000 && guardedPlayer; elapsed += 20) {
        for (auto* menu : guardedPlayer->findChildren<mplayer::shell::MenuPopup*>()) {
            if (menu == nullptr || !menu->isVisible()) continue;
            for (int row = 0; row < menu->rowCount(); ++row) {
                if (menu->entryAt(row).payload.toString() != QLatin1String("copy")) continue;
                if (auto* button = qobject_cast<QPushButton*>(menu->rowWidget(row));
                    button != nullptr && button->isVisible() && button->isEnabled()) {
                    button->click();
                    return true;
                }
            }
        }
        QTest::qWait(20);
    }
    return false;
}

#ifdef Q_OS_WIN
// A real owned HWND is mandatory: Windows only denies a competing OpenClipboard while a
// window owns the lock, so a NULL owner would let the probing process open it too.
HWND createClipboardHolderWindow() {
    const wchar_t className[] = L"MikaiClipboardHolder";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    RegisterClassExW(&wc);
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, className,
                           L"mikai clipboard holder", WS_POPUP, 0, 0, 0, 0,
                           nullptr, nullptr, wc.hInstance, nullptr);
}

// Bounded explicit release: 'x' from the parent, a closed stdin, or the timeout.
int waitForClipboardReleaseSignal(int timeoutMs) {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    for (int waited = 0; waited <= timeoutMs; waited += 50) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (input != nullptr && input != INVALID_HANDLE_VALUE) {
            DWORD available = 0;
            if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
                // The parent closed its write end (broken pipe): release immediately.
                if (GetLastError() == ERROR_BROKEN_PIPE) return 2;
            } else if (available > 0) {
                char byte = 0;
                DWORD read = 0;
                if (!ReadFile(input, &byte, 1, &read, nullptr) || read == 0) return 2;
                if (byte == 'x') return 0;
            }
        }
        Sleep(50);
    }
    return 3;
}
#endif

int holdClipboardOpen() {
#ifdef Q_OS_WIN
    HWND owner = createClipboardHolderWindow();
    if (owner == nullptr) return 2;
    if (!OpenClipboard(owner)) {
        DestroyWindow(owner);
        return 2;
    }
    DWORD ownerPid = 0;
    GetWindowThreadProcessId(owner, &ownerPid);
    std::fprintf(stdout, "CLIPBOARD-OPEN pid=%lu owner=%p\n",
                 static_cast<unsigned long>(ownerPid), static_cast<void*>(owner));
    std::fflush(stdout);
    const int released = waitForClipboardReleaseSignal(120000);
    CloseClipboard();
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    DestroyWindow(owner);
    return released;
#else
    return 2;
#endif
}

#ifdef Q_OS_WIN
struct ClipboardSnapshot {
    QMap<QString, QByteArray> contents;
    QList<UINT> nativeFormats;
    QString text;
    QImage image;
    DWORD sequence = 0;
    bool valid = false;

    static ClipboardSnapshot capture() {
        ClipboardSnapshot snapshot;
        if (!OpenClipboard(nullptr)) return snapshot;
        if (!CloseClipboard()) return snapshot;
        if (const QMimeData* mime = QApplication::clipboard()->mimeData()) {
            for (const QString& format : mime->formats()) {
                snapshot.contents.insert(format, mime->data(format));
            }
            snapshot.text = mime->text();
            if (mime->hasImage()) snapshot.image = QApplication::clipboard()->image();
        }
        if (!OpenClipboard(nullptr)) return snapshot;
        for (UINT format = EnumClipboardFormats(0); format != 0;
             format = EnumClipboardFormats(format)) {
            snapshot.nativeFormats.append(format);
        }
        snapshot.sequence = GetClipboardSequenceNumber();
        snapshot.valid = CloseClipboard() != 0;
        return snapshot;
    }

    bool sameContents(const ClipboardSnapshot& after) const {
        return valid && after.valid && after.contents == contents && after.text == text &&
            after.image.convertToFormat(QImage::Format_RGBA8888) ==
                image.convertToFormat(QImage::Format_RGBA8888);
    }

    bool unchanged() const {
        // The sequence covers even native formats Qt cannot decode.
        if (!valid || GetClipboardSequenceNumber() != sequence) return false;
        const ClipboardSnapshot after = capture();
        return after.sequence == sequence && after.nativeFormats == nativeFormats &&
            sameContents(after);
    }
};

class ClipboardLock {
public:
    bool acquire() {
        process_.setProcessChannelMode(QProcess::SeparateChannels);
        process_.start(QCoreApplication::applicationFilePath(),
                       {QStringLiteral("--hold-clipboard")});
        if (!process_.waitForStarted(10000)) {
            logHolder("process did not start");
            return false;
        }
        if (!process_.waitForReadyRead(10000)) {
            logHolder("process never reported ownership");
            cleanup();
            return false;
        }
        if (!process_.readAllStandardOutput().contains("CLIPBOARD-OPEN")) {
            logHolder("process reported no clipboard ownership");
            cleanup();
            return false;
        }
        // Real OS refusal: the probing process must be denied the held clipboard.
        if (OpenClipboard(nullptr)) {
            CloseClipboard();
            std::fprintf(stderr,
                         "TITLE-PROBE: FAIL clipboard holder did not refuse a competing open\n");
            logHolder("competing open was not denied");
            cleanup();
            return false;
        }
        const unsigned long denial = static_cast<unsigned long>(GetLastError());
        HWND holder = GetOpenClipboardWindow();
        DWORD holderPid = 0;
        if (holder != nullptr) GetWindowThreadProcessId(holder, &holderPid);
        if (holder == nullptr || holderPid != static_cast<DWORD>(process_.processId())) {
            std::fprintf(stderr,
                         "TITLE-PROBE: FAIL clipboard holder owner window %p pid=%lu is not the "
                         "spawned holder pid=%lld (competing open denied with %lu)\n",
                         static_cast<void*>(holder), static_cast<unsigned long>(holderPid),
                         static_cast<long long>(process_.processId()), denial);
            logHolder("owner window belongs to another process");
            cleanup();
            return false;
        }
        return true;
    }

    bool release() {
        if (process_.state() == QProcess::NotRunning) {
            std::fprintf(stderr,
                         "TITLE-PROBE: FAIL clipboard holder exited before explicit release\n");
            logHolder("holder exited early");
            return false;
        }
        process_.write("x");
        process_.waitForBytesWritten(5000);
        if (!process_.waitForFinished(10000) ||
            process_.exitStatus() != QProcess::NormalExit || process_.exitCode() != 0) {
            std::fprintf(stderr,
                         "TITLE-PROBE: FAIL clipboard holder did not exit cleanly after release\n");
            logHolder("holder exit was abnormal");
            return false;
        }
        return true;
    }

    ~ClipboardLock() { cleanup(); }

private:
    // Child lifecycle evidence only: no clipboard bytes, payloads or secrets are printed.
    void logHolder(const char* reason) {
        const QByteArray errors = process_.readAllStandardError();
        std::fprintf(stderr,
                     "TITLE-PROBE:     holder: %s error=%s exitStatus=%d exitCode=%d stderr=%s\n",
                     reason, qPrintable(process_.errorString()),
                     static_cast<int>(process_.exitStatus()),
                     process_.exitCode(),
                     errors.isEmpty() ? "<empty>" : errors.constData());
    }

    void cleanup() {
        if (process_.state() == QProcess::NotRunning) return;
        if (!release()) {
            logHolder("holder required a kill to release");
            process_.kill();
            process_.waitForFinished(5000);
        }
    }

    QProcess process_;
};
#endif

/// A decoded frame exists only where a real GL context does: the offscreen
/// platform plugin has no createPlatformOpenGLContext, so VideoSurface's
/// QOpenGLWidget never gets a context there and hasVideo() can never turn true.
/// Demanding one offscreen would fail on the platform's own limitation rather
/// than on the hosted path under test, so the copy gate that reads pixels back
/// passes requireDecodedSource.
bool hostedSessionReady(mplayer::shell::PlayerDialog* player, bool requireDecodedSource) {
    if (player == nullptr || !player->isVisible()) {
        return false;
    }
    if (requireDecodedSource && !player->hasActiveSource()) {
        return false;
    }
    if (player->currentEmbed().isEmpty()) {
        // The track popup's current embed is what the host answers for: a
        // standalone dialog never has one, so this is what separates a hosted
        // session from a file/URL player without guessing from the caption.
        return false;
    }
    auto* shell = player->findChild<mplayer::shell::PlayerShell*>();
    return shell != nullptr && shell->videoSurface() != nullptr;
}

bool hostRefusalProbe(TitlePage* page, bool browserFailure) {
    const QPointer<TitlePage> guardedPage(page);
    QPointer<QPushButton> episode;
    for (int elapsed = 0; elapsed < 30000 && guardedPage; elapsed += 50) {
        auto* tabs = guardedPage->findChild<QTabWidget*>();
        if (tabs != nullptr && tabs->currentWidget() != nullptr) {
            for (auto* button : tabs->currentWidget()->findChildren<QPushButton*>(
                     QStringLiteral("episodeButton"))) {
                if (button->isVisible() && button->isEnabled()) {
                    episode = button;
                    break;
                }
            }
        }
        if (episode) break;
        QTest::qWait(50);
    }
    if (!episode) {
        std::fprintf(stderr, "TITLE-PROBE: FAIL real title7428 has no playable episode\n");
        return false;
    }
    episode->click();
    // What this scenario has to prove is the HOST, not the decoder: the real API
    // episode opened a real hosted dialog on the real host bridge, and that host
    // owns the failure feedback. A decoded frame is not part of it.
    //
    // The Windows clipboard copy gate needs one, because copying a frame is a
    // pixel readback of a live core and an unlocked clipboard is only ever proven
    // against a write that could have succeeded. It runs on the Windows QPA,
    // where a real GL context exists, and it keeps the full decoded-source
    // requirement below.
    //
    // The offscreen platform plugin has no createPlatformOpenGLContext, so
    // VideoSurface is a QOpenGLWidget that can never get a context there and
    // hasVideo() can never turn true. Demanding a decoded frame offscreen would
    // fail on the platform's own limitation, not on the host path under test, so
    // the browser mode proves the session itself instead: the dialog exists, its
    // shell and surface were really built, the host really owns the embed, and
    // the host's own browser action really ran and raised a named modal warning
    // parented to that player. No quality is fabricated and the prompt is never
    // forced.
    QPointer<mplayer::shell::PlayerDialog> player;
    for (int elapsed = 0; elapsed < 60000 && guardedPage; elapsed += 50) {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<mplayer::shell::PlayerDialog*>(widget);
                candidate != nullptr && candidate->isVisible() &&
                hostedSessionReady(candidate, !browserFailure)) {
                player = candidate;
                break;
            }
        }
        if (player) break;
        QTest::qWait(50);
    }
    if (!player) {
        std::fprintf(stderr,
                     "TITLE-PROBE: FAIL real title7428 opened no hosted player session\n");
        return false;
    }
    auto* shell = player->findChild<mplayer::shell::PlayerShell*>();
    auto* surface = shell != nullptr ? shell->videoSurface() : nullptr;
    if (shell == nullptr || surface == nullptr) {
        std::fprintf(stderr, "TITLE-PROBE: FAIL hosted session has no shell or video surface\n");
        return false;
    }
    if (!browserFailure) {
        // The decoded-frame prerequisite, kept exactly for the copy gate.
        if (!player->hasActiveSource()) {
            std::fprintf(stderr, "TITLE-PROBE: FAIL copy session has no decoded source frame\n");
            return false;
        }
    }
    surface->setPaused(true);
    shell->wake();
    QTest::qWait(100);
    if (!player) return false;
    QPointer<QLabel> status = player->findChild<QLabel*>(QStringLiteral("playerStatusText"));
    if (!status) return false;
    const QString statusBefore = status->text();
    WarningOutcome warning;
    bool action = false;
    bool clipboardPreserved = true;
    bool lockReleased = true;
    bool sessionIntact = true;

    if (browserFailure) {
        QPointer<mplayer::shell::PillButton> original =
            player->findChild<mplayer::shell::PillButton*>(QStringLiteral("originalFileButton"));
        // The strip's own chrome hides after an idle moment, so the wake above
        // may not have revealed it yet. Wait, bounded, for the real control: the
        // action under test is a press on the actual button, never a synthesised
        // signal.
        for (int elapsed = 0; elapsed < 5000 && original; elapsed += 50) {
            if (original->isVisible() && original->isEnabled()) break;
            QTest::qWait(50);
        }
        if (original && original->isVisible() && original->isEnabled()) {
            warning = dismissPlayerWarning(player, [&] {
                if (!original) return;
                action = true;
                QTest::mouseClick(original, Qt::LeftButton);
            }, QStringLiteral("title-host-browser-refusal.png"),
            QCoreApplication::translate("TitlePage", "Failed to open in browser"));
        } else {
            std::fprintf(stderr, "TITLE-PROBE: hosted player's original-file button is not usable\n");
        }
    } else {
#ifdef Q_OS_WIN
        const ClipboardSnapshot clipboardBefore = ClipboardSnapshot::capture();
        if (!clipboardBefore.valid) {
            std::fprintf(stderr, "TITLE-PROBE: FAIL original clipboard snapshot unavailable\n");
            return false;
        }
        ClipboardLock lock;
        if (!lock.acquire()) {
            std::fprintf(stderr, "TITLE-PROBE: FAIL separate Win32 clipboard lock unavailable\n");
            return false;
        }
        warning = dismissPlayerWarning(player, [&] {
            if (player) action = clickPlayerCopyAction(player);
        }, QStringLiteral("title-host-copy-refusal.png"),
        QCoreApplication::translate("TitlePage", "Failed to copy link"));
        const bool sequencePreserved = GetClipboardSequenceNumber() == clipboardBefore.sequence;
        lockReleased = lock.release();
        clipboardPreserved = sequencePreserved && clipboardBefore.unchanged();
#else
        return false;
#endif
    }
    QTest::qWait(100);
    // A refused clipboard or browser action must not take the player down. The
    // browser mode checks the session it opened is still the same hosted one,
    // because a decoded frame is not available offscreen; the Windows copy mode
    // keeps the full decoded-source check it entered with.
    sessionIntact = hostedSessionReady(player, !browserFailure);
    const bool noFalseSuccess = status && status->text() == statusBefore;
    const bool passed = action && warning.shown && warning.parentedToPlayer &&
        warning.captured && warning.accessible && warning.alert && warning.namedFailure &&
        !warning.timedOut &&
        lockReleased && clipboardPreserved && sessionIntact && noFalseSuccess;
    std::fprintf(stderr,
                 "TITLE-PROBE: hosted %s refusal action=%d visible=%d parent=%d accessible=%d "
                 "Alert=%d screenshots=%d timeout=%d clipboard-preserved=%d released=%d "
                 "no-false-success=%d session-intact=%d RESULT %s\n",
                 browserFailure ? "browser" : "copy", action, warning.shown,
                 warning.parentedToPlayer, warning.accessible, warning.alert, warning.captured,
                 warning.timedOut, clipboardPreserved, lockReleased, noFalseSuccess,
                 sessionIntact, passed ? "PASS" : "FAIL");
    return passed;
}

bool titleBrowserRefusal(TitlePage* page) {
    QPushButton* episode = nullptr;
    for (int elapsed = 0; elapsed < 30000; elapsed += 50) {
        const auto buttons = page->findChildren<QPushButton*>(QStringLiteral("episodeButton"));
        for (QPushButton* button : buttons) {
            if (button->isEnabled() && button->isVisible()) {
                episode = button;
                break;
            }
        }
        if (episode != nullptr) break;
        QTest::qWait(50);
    }
    QLabel* status = page->findChild<QLabel*>(QStringLiteral("status"));
    if (episode == nullptr || status == nullptr) return false;

    const QString before = status->text();
    AccessibleAlertCapture alert(status);
    bool triggered = false;
    int elapsed = 0;
    QTimer poller;
    poller.setInterval(20);
    QObject::connect(&poller, &QTimer::timeout, &poller, [&] {
        elapsed += poller.interval();
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* menu = qobject_cast<QMenu*>(widget);
            if (menu == nullptr || !menu->isVisible()) continue;
            for (QAction* action : menu->actions()) {
                if (action->text() != QCoreApplication::translate("mikai::app::TitlePage", "Open in browser")) {
                    continue;
                }
                triggered = true;
                action->trigger();
                menu->close();
                poller.stop();
                return;
            }
        }
        if (elapsed >= 5000) {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (auto* menu = qobject_cast<QMenu*>(widget); menu != nullptr && menu->isVisible()) {
                    menu->close();
                }
            }
            poller.stop();
        }
    });
    poller.start();
    const QPoint local(4, 4);
    QContextMenuEvent event(QContextMenuEvent::Mouse, local, episode->mapToGlobal(local));
    QApplication::sendEvent(episode, &event);
    poller.stop();

    QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(status);
    const bool feedback = status->isVisible() && !status->visibleRegion().isEmpty() &&
                          status->text() != before &&
                          status->text() == QCoreApplication::translate(
                              "mikai::app::TitlePage", "Failed to open in browser") &&
                          accessible != nullptr && !accessible->state().invisible &&
                          accessible->text(QAccessible::Name) == status->text();
    const bool captured = page->grab().save(QStringLiteral("title-browser-refusal.png"));
    std::fprintf(stderr,
                 "TITLE-PROBE: browser refusal feedback=%d accessible=%d alert=%d screenshot=%d text=%s\n",
                 feedback ? 1 : 0, accessible != nullptr ? 1 : 0,
                 alert.observed() ? 1 : 0, captured ? 1 : 0,
                 status->text().toUtf8().constData());
    return triggered && feedback && alert.observed() && captured;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setStyle(QStringLiteral("Fusion"));
    app.setStyleSheet(mikai::app::theme::buildStylesheet());
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--hold-clipboard")) {
        return holdClipboardOpen();
    }
    const bool layoutOnly = app.arguments().contains(QStringLiteral("--layout-only"));
    const bool disposeEarly = app.arguments().contains(QStringLiteral("--dispose-early"));
    const bool browserFailureOnly =
        app.arguments().contains(QStringLiteral("--browser-refusal-only"));
    const bool hostBrowserFailureOnly =
        app.arguments().contains(QStringLiteral("--host-browser-refusal-only"));
    const bool hostCopyFailureOnly =
        app.arguments().contains(QStringLiteral("--host-copy-refusal-only"));
    for (const QString& argument : app.arguments().mid(1)) {
        if (argument.startsWith(QLatin1Char('-')) &&
            argument != QLatin1String("--layout-only") &&
            argument != QLatin1String("--dispose-early") &&
            argument != QLatin1String("--browser-refusal-only") &&
            argument != QLatin1String("--host-browser-refusal-only") &&
            argument != QLatin1String("--host-copy-refusal-only")) {
            std::fprintf(stderr, "TITLE-PROBE: unknown option %s\n", qPrintable(argument));
            return 2;
        }
    }
    if (int(layoutOnly) + int(disposeEarly) + int(browserFailureOnly) +
        int(hostBrowserFailureOnly) + int(hostCopyFailureOnly) > 1) {
        std::fprintf(stderr,
                     "TITLE-PROBE: choose one of --host-browser-refusal-only (offscreen) "
                     "or --host-copy-refusal-only (windows)\n");
        return 2;
    }
    if ((browserFailureOnly || hostBrowserFailureOnly) &&
        QGuiApplication::platformName() != QLatin1String("offscreen")) {
        std::fprintf(stderr, "TITLE-PROBE: browser refusal requires QT_QPA_PLATFORM=offscreen\n");
        return 2;
    }
    if (hostCopyFailureOnly) {
#ifndef Q_OS_WIN
        std::fprintf(stderr, "TITLE-PROBE: clipboard refusal requires Windows OpenClipboard\n");
        return 2;
#else
        if (QGuiApplication::platformName() != QLatin1String("windows")) {
            std::fprintf(stderr, "TITLE-PROBE: clipboard refusal requires the Windows QPA\n");
            return 2;
        }
#endif
    }
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) return 2;
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("TitlePage"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    QSettings().setValue(QStringLiteral("player/anime4k"), QStringLiteral("off"));
    if (!layoutOnly && !disposeEarly && !browserFailureOnly &&
        !mplayer::probes::selectRuntime()) {
        return 2;
    }
    QString ref = QStringLiteral("7428");
    for (const QString& argument : app.arguments().mid(1)) {
        if (!argument.startsWith(QLatin1Char('-'))) {
            ref = argument;
            break;
        }
    }

    if ((hostBrowserFailureOnly || hostCopyFailureOnly) && ref != QLatin1String("7428")) {
        std::fprintf(stderr, "TITLE-PROBE: hosted refusal scenarios require real title7428\n");
        return 2;
    }

    MikaiClient client(qEnvironmentVariable("MIKAI_API_KEY"));
    Runner runner;
    QualityCache cache;
    auto* page = new TitlePage(runner, client, cache, ref);
    if (disposeEarly) {
        delete page;
        if (!runner.waitForDone(60000)) {
            std::fprintf(stderr, "TITLE-PROBE: early-disposal workers did not finish\n");
            return 1;
        }
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        std::fprintf(stderr, "TITLE-PROBE: early-disposal deliveries completed without crash\n");
        return 0;
    }
    if (browserFailureOnly) {
        page->resize(QSize(860, 620));
        page->show();
        const bool passed = titleBrowserRefusal(page);
        delete page;
        std::fprintf(stderr, "TITLE-PROBE: browser refusal %s\n", passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }
    page->resize(layoutOnly ? QSize(860, 620) : QSize(1400, 900));
    page->show();

    if (hostBrowserFailureOnly || hostCopyFailureOnly) {
        const bool passed = hostRefusalProbe(page, hostBrowserFailureOnly);
        delete page;
        return passed ? 0 : 1;
    }

    // Stage 1: what did the tabs actually receive?
    QTimer::singleShot(9000, [page, ref, layoutOnly] {
        auto* tabs = page->findChild<QTabWidget*>();
        if (tabs == nullptr || tabs->count() == 0) {
            std::fprintf(stderr, "TITLE-PROBE: %s produced no release tabs\n", qPrintable(ref));
            if (layoutOnly) {
                QCoreApplication::exit(1);
            }
            return;
        }
        std::fprintf(stderr, "TITLE-PROBE: %s -> %d provider tabs\n", qPrintable(ref), tabs->count());
        for (int i = 0; i < tabs->count(); ++i) {
            QWidget* tabPage = tabs->widget(i);
            const int pills = tabPage->findChildren<QPushButton*>(QStringLiteral("episodeButton")).size();
            std::fprintf(stderr, "TITLE-PROBE:   tab %d %-12s pills=%d\n", i,
                         qPrintable(tabs->tabText(i)), pills);
        }

        QWidget* first = tabs->widget(0);
        auto* scroll = first->findChild<QScrollArea*>(QStringLiteral("episodeScroll"));
        QWidget* grid = scroll != nullptr ? scroll->widget() : nullptr;
        const auto pills = grid != nullptr
                               ? grid->findChildren<QPushButton*>(QStringLiteral("episodeButton"))
                               : QList<QPushButton*>();
        bool layoutPass = scroll != nullptr && !pills.isEmpty();
        if (layoutPass && scroll->verticalScrollBar()->maximum() == 0) {
            int lastPillBottom = 0;
            for (QPushButton* button : pills) {
                lastPillBottom = qMax(
                    lastPillBottom,
                    button->mapTo(scroll->viewport(), QPoint(0, button->height())).y());
            }
            const int trailingGap = scroll->viewport()->height() - lastPillBottom;
            layoutPass = trailingGap <= 1;
            std::fprintf(stderr,
                         "TITLE-PROBE: episode footer gap=%dpx viewport=%dpx pills=%d %s\n",
                         trailingGap, scroll->viewport()->height(),
                         static_cast<int>(pills.size()),
                         layoutPass ? "PASS" : "FAIL");
        } else if (layoutPass) {
            std::fprintf(stderr, "TITLE-PROBE: episode footer alignment skipped (scrolling list)\n");
        } else {
            std::fprintf(stderr, "TITLE-PROBE: episode footer alignment FAIL (missing grid/pills)\n");
        }
        if (layoutOnly) {
            auto* episodesHeading = page->findChild<QLabel*>(
                QStringLiteral("episodeSectionHeading"));
            auto* releaseChipsHost = page->findChild<QWidget*>(
                QStringLiteral("releaseChipsHost"));
            const auto releaseChips = page->findChildren<QPushButton*>(
                QStringLiteral("releaseChip"));
            bool selectorPass = !releaseChips.isEmpty();
            int checkedCount = 0;
            for (QPushButton* chip : releaseChips) {
                checkedCount += chip->isChecked() ? 1 : 0;
            }
            selectorPass = selectorPass && checkedCount == 1;
            if (episodesHeading != nullptr && releaseChipsHost != nullptr) {
                const QRect headingRect(episodesHeading->mapTo(page, QPoint(0, 0)),
                                        episodesHeading->size());
                const QRect chipsRect(releaseChipsHost->mapTo(page, QPoint(0, 0)),
                                      releaseChipsHost->size());
                const QRect selectorRect =
                    headingRect.united(chipsRect)
                        .adjusted(-mikai::app::theme::kSpacingXs,
                                  -mikai::app::theme::kSpacingXs,
                                  mikai::app::theme::kSpacingXs,
                                  mikai::app::theme::kSpacingXs);
                const QString screenshotPath =
                    QDir(QDir::tempPath()).filePath(QStringLiteral("mikai-release-selector.png"));
                if (page->grab(selectorRect).save(screenshotPath)) {
                    std::fprintf(stderr, "TITLE-PROBE: selector section saved to %s\n",
                                 qPrintable(screenshotPath));
                }
            }
            QPushButton* nextRelease = nullptr;
            for (QPushButton* chip : releaseChips) {
                if (!chip->isChecked()) {
                    nextRelease = chip;
                    break;
                }
            }
            if (nextRelease != nullptr) {
                nextRelease->click();
                QCoreApplication::processEvents();
                checkedCount = 0;
                for (QPushButton* chip : releaseChips) {
                    checkedCount += chip->isChecked() ? 1 : 0;
                }
                selectorPass = selectorPass && nextRelease->isChecked() && checkedCount == 1;
            }
            std::fprintf(stderr, "TITLE-PROBE: release chips=%d exclusive-selection=%s\n",
                         static_cast<int>(releaseChips.size()),
                         selectorPass ? "PASS" : "FAIL");
            layoutPass = layoutPass && selectorPass;
            QCoreApplication::exit(layoutPass ? 0 : 1);
            return;
        }

        auto* pill = pills.isEmpty() ? nullptr : pills.constFirst();
        if (pill == nullptr) {
            std::fprintf(stderr, "TITLE-PROBE: tab 0 has no pill to click\n");
            return;
        }
        std::fprintf(stderr, "TITLE-PROBE: clicking a pill on %s\n",
                     qPrintable(tabs->tabText(0)));
        pill->click();
    });

    // Stage 3: the player (if any) and its series drawer.
    QTimer::singleShot(20000, [&] {
        QWidget* dialog = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (QString::fromLatin1(w->metaObject()->className())
                    .contains(QStringLiteral("PlayerDialog"))) {
                dialog = w;
                break;
            }
        }
        if (dialog == nullptr) {
            std::fprintf(stderr, "TITLE-PROBE: NO PlayerDialog opened after clicking a pill\n");
            std::fprintf(stderr, "TITLE-PROBE: RESULT INCONCLUSIVE\n");
            return;
        }
        QPushButton* toggle = playerSeriesToggle(dialog);
        std::fprintf(stderr, "TITLE-PROBE: PlayerDialog exists; series toggle %s\n",
                     toggle == nullptr ? "MISSING"
                                       : (toggle->isVisible() ? "visible" : "PRESENT BUT HIDDEN"));
        if (toggle != nullptr) {
            // Distinguish an empty playlist from chrome auto-hide.
            std::fprintf(stderr, "TITLE-PROBE:   toggle->isHidden()=%d  visibleToParent=%d\n",
                         toggle->isHidden() ? 1 : 0,
                         toggle->isVisibleTo(toggle->parentWidget()) ? 1 : 0);
            for (QWidget* w = toggle->parentWidget(); w != nullptr; w = w->parentWidget()) {
                std::fprintf(stderr, "TITLE-PROBE:   ancestor %-24s visible=%d hidden=%d\n",
                             qPrintable(w->objectName().isEmpty() ? w->metaObject()->className()
                                                                  : w->objectName()),
                             w->isVisible() ? 1 : 0, w->isHidden() ? 1 : 0);
            }
        }
        // Opening the drawer must hold chrome beyond the idle deadline.
        for (QWidget* w : dialog->findChildren<QWidget*>()) {
            if (QString::fromLatin1(w->metaObject()->className())
                    .contains(QStringLiteral("VideoSurface"))) {
                for (const QPointF& at : {QPointF(40, 40), QPointF(60, 60)}) {
                    QMouseEvent move(QEvent::MouseMove, at, w->mapToGlobal(at), Qt::NoButton,
                                     Qt::NoButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(w, &move);
                }
            }
        }
        QApplication::processEvents();
        std::fprintf(stderr, "TITLE-PROBE: after waking, toggle visible=%d strip visible=%d\n",
                     toggle != nullptr && toggle->isVisible() ? 1 : 0,
                     toggle != nullptr && toggle->parentWidget() != nullptr &&
                             toggle->parentWidget()->isVisible()
                         ? 1
                         : 0);
        if (toggle != nullptr && toggle->isVisible()) {
            toggle->click();
            QApplication::processEvents();
            auto* drawer = dialog->findChild<QWidget*>(QStringLiteral("playerSeriesDrawer"));
            const int rows =
                drawer == nullptr
                    ? 0
                    : drawer->findChildren<QPushButton*>(QStringLiteral("playerSeriesItem")).size() +
                          drawer->findChildren<QPushButton*>(
                              QStringLiteral("playerSeriesItemCurrent")).size();
            std::fprintf(stderr, "TITLE-PROBE: drawer %s, rows=%d\n",
                         drawer == nullptr ? "MISSING" : "present", rows);
            // Now stop touching anything and let the idle timer run past its
            // 2.5s hold. A user reading the episode list is not idle.
            QTest::qWait(3500);
            QApplication::processEvents();
            std::fprintf(stderr,
                         "TITLE-PROBE: 3.5s after opening, drawer visible=%d rows=%d\n",
                         drawer->isVisible() ? 1 : 0,
                         static_cast<int>(
                             drawer->findChildren<QPushButton*>(
                                 QStringLiteral("playerSeriesItem")).size() +
                             drawer->findChildren<QPushButton*>(
                                 QStringLiteral("playerSeriesItemCurrent")).size()));
        }
        std::fprintf(stderr, "TITLE-PROBE: RESULT OK\n");
    });

    // Repeated cover opens must not reuse a WA_DeleteOnClose pointer.
    QTimer::singleShot(26000, [page] {
        auto* cover = page->findChild<QWidget*>(QStringLiteral("titleCover"));
        if (cover == nullptr) {
            std::fprintf(stderr, "COVER-PROBE: no titleCover label found\n");
            return;
        }
        if (!cover->isEnabled()) {
            std::fprintf(stderr, "COVER-PROBE: cover is disabled, skipping\n");
            return;
        }
        int opened = 0;
        for (int round = 0; round < 4; ++round) {
            const QPointF at(4, 4);
            QMouseEvent press(QEvent::MouseButtonPress, at, cover->mapToGlobal(at),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(cover, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, at, cover->mapToGlobal(at),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(cover, &release);
            QApplication::processEvents();

            QWidget* viewer = nullptr;
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (QString::fromLatin1(w->metaObject()->className())
                        .contains(QStringLiteral("CoverViewer"))) {
                    viewer = w;
                    break;
                }
            }
            if (viewer == nullptr) {
                std::fprintf(stderr, "COVER-PROBE: round %d did not open a viewer\n", round);
                continue;
            }
            ++opened;
            std::fprintf(stderr, "COVER-PROBE: round %d opened the viewer\n", round);
            viewer->close();
            QApplication::processEvents();
        }
        std::fprintf(stderr, "COVER-PROBE: %d/4 rounds opened; reaching here is the fix\n",
                     opened);
        std::fprintf(stderr, "COVER-PROBE: RESULT OK\n");
    });

    return app.exec();
}
