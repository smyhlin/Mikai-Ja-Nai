
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <cstdio>

#include "WindowRaisePolicy.hpp"
#include "backend/MikaiClient.hpp"
#include "dialogs/ComponentStoreTab.hpp"
#include "pages/MainWindow.hpp"
#include "workers/Runner.hpp"

namespace {

bool g_failed = false;

void check(bool ok, const char* what) {
    std::fprintf(stderr, "[modal-prompt-probe] %s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed = true;
}

void note(const QString& text) {
    std::fprintf(stderr, "[modal-prompt-probe] %s\n", qPrintable(text));
}

/// The ask has no QObject parent, so it is found as a top-level window rather
/// than through a widget subtree.
QDialog* findSetupPrompt() {
    const QWidgetList windows = QApplication::topLevelWidgets();
    for (QWidget* window : windows) {
        if (window->objectName() == QLatin1String("animeJanaiSetupPrompt") &&
            window->isVisible()) {
            return qobject_cast<QDialog*>(window);
        }
    }
    return nullptr;
}

void rejectSetupPrompt() {
    if (QDialog* prompt = findSetupPrompt()) prompt->reject();
}

/// Drives the app's own raise/activation policy until `window` is active.
bool pumpUntilActive(QWidget* window, int timeoutMs = 5000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (QApplication::activeWindow() != window) {
        if (elapsed.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

void drain(mikai::app::MainWindow* window) {
    if (window == nullptr) return;
    if (auto* runner = window->findChild<mikai::app::Runner*>()) {
        runner->cancel();
        if (!runner->waitForDone(15000)) note(QStringLiteral("workers did not retire"));
    }
}

/// A player-like top-level window, shown fullscreen: the real case the ask's
/// native ownership has to survive. It is activated before the shell exists, so
/// no event-loop turn can deliver the inventory result first.
QWidget* makeActiveDecoy(const char* name) {
    auto* decoy = new QWidget(nullptr, Qt::Window);
    decoy->setObjectName(QString::fromLatin1(name));
    decoy->setWindowTitle(QStringLiteral("Probe fullscreen player window"));
    decoy->setMinimumSize(640, 360);
    decoy->showFullScreen();
    const bool fullscreen = decoy->isVisible() && decoy->isFullScreen();
    check(fullscreen, "the player-like decoy is a visible fullscreen top-level window");
    if (!fullscreen) {
        // Ownership against a fullscreen owner cannot be exercised without one.
        delete decoy;
        return nullptr;
    }
    if (!pumpUntilActive(decoy)) {
        check(false, "a fullscreen decoy could not be made active; run the probe on an active "
                     "desktop so the active-window owner can be established");
        delete decoy;
        return nullptr;
    }
    check(true, "the probe owns an active fullscreen player-like window before the "
                "inventory turn");
    return decoy;
}

/// Shows the shell and takes activation back to the decoy without running an
/// event loop, so which window is active when the inventory result is delivered
/// is decided by this probe rather than the desktop's foreground race.
bool bringShellUpUnderDecoy(mikai::app::MainWindow* window, QWidget* decoy) {
    // The policy's documented escape hatch, and the platform's own attribute;
    // production shows the shell with activation.
    window->setAttribute(Qt::WA_ShowWithoutActivating, true);
    window->setProperty("mikaiNoActivateOnShow", true);
    window->resize(1120, 800);
    window->show();
    if (QApplication::activeWindow() != decoy) {
        decoy->raise();
        decoy->activateWindow();
    }
    if (QApplication::activeWindow() != decoy) {
        check(false, "the decoy could not be kept active while the shell went up");
        return false;
    }
    check(true, "the decoy is active when the component inventory lands");
    return true;
}

/// True while an ask may still appear. A ready inventory that asks for nothing
/// means every component is installed: a failed precondition, never a pass.
bool askIsPossible(mikai::app::MainWindow* window) {
    auto* components = window->findChild<mikai::app::ComponentStoreTab*>();
    if (components == nullptr) {
        check(false, "the shell did not build its component inventory");
        return false;
    }
    if (components->inventoryReady() && components->setupPrompt().isEmpty()) {
        check(false, "no setup prompt is due on this machine; run the probe from a scratch "
                     "directory with no installed components");
        return false;
    }
    return true;
}

/// Phase A: the ask's fullscreen native owner is destroyed while the ask owns
/// its nested loop. The ask must survive, and a close arriving at the shell
/// meanwhile must not tear the players down under it.
bool runActiveOwnerDestructionPhase(mikai::backend::MikaiClient& client) {
    QWidget* decoy = makeActiveDecoy("probeActivePlayerA");
    if (decoy == nullptr) return false;

    auto* window = new mikai::app::MainWindow(client);
    if (!bringShellUpUnderDecoy(window, decoy) || !askIsPossible(window)) {
        drain(window);
        delete window;
        delete decoy;
        return false;
    }

    QEventLoop loop;
    QTimer driver;
    driver.setInterval(10);
    QTimer deadline;
    deadline.setSingleShot(true);
    QPointer<mikai::app::MainWindow> windowWatch(window);
    QPointer<QDialog> promptWatch;
    bool acted = false;

    QObject::connect(&driver, &QTimer::timeout, &loop, [&] {
        QDialog* prompt = findSetupPrompt();
        if (prompt == nullptr || acted) {
            // An inventory that is already ready and asks for nothing means this
            // machine has every component: the `acted` check below then fails.
            if (!acted && window != nullptr && !askIsPossible(window)) {
                driver.stop();
                loop.quit();
            }
            return;
        }
        acted = true;
        promptWatch = prompt;

        check(prompt->isVisible() && QApplication::activeModalWidget() == prompt,
              "the setup prompt is the visible active modal window");
        check(decoy != nullptr && decoy->isVisible() && decoy->isFullScreen(),
              "the ask's native owner is the visible fullscreen player-like window");
        check(prompt->windowHandle() != nullptr &&
                  prompt->windowHandle()->transientParent() == decoy->windowHandle(),
              "the setup prompt's transient parent is the fullscreen player-like window");

        // The owner goes away under the ask, the way a player closed by the
        // shell's own paths used to take the QObject-parented dialog with it.
        delete decoy;
        decoy = nullptr;
        check(promptWatch != nullptr && promptWatch->isVisible(),
              "the setup prompt survived its native owner being destroyed");
        check(promptWatch->windowHandle() == nullptr ||
                  promptWatch->windowHandle()->transientParent() == nullptr,
              "the destroyed owner left no transient parent behind");

        QCloseEvent close;
        QCoreApplication::sendEvent(window, &close);
        check(!close.isAccepted(), "the shell refused to close underneath the ask");
        check(windowWatch != nullptr && windowWatch->isVisible(),
              "the shell stayed visible while the ask owned the loop");
        check(promptWatch != nullptr && promptWatch->isVisible(),
              "the ask stayed visible after the close was refused");

        driver.stop();
        promptWatch->reject();
        loop.quit();
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        rejectSetupPrompt();
        loop.quit();
    });

    driver.start();
    deadline.start(20000);
    loop.exec();
    driver.stop();
    deadline.stop();
    rejectSetupPrompt();

    check(acted, "the setup prompt appeared while a fullscreen player-like window was active");
    check(promptWatch.isNull(),
          "the setup prompt was deleted after it was answered instead of leaking");

    if (windowWatch != nullptr) {
        QCloseEvent after;
        QCoreApplication::sendEvent(windowWatch.data(), &after);
        check(after.isAccepted(), "the shell closes once the ask is answered");
    }

    drain(windowWatch.data());
    delete windowWatch.data();
    delete decoy;
    return !g_failed;
}

/// Phase B: the shell is deleted programmatically while the ask owns its loop.
/// The ask must end with it, not outlive it, and the app must stay usable.
bool runShellDestructionPhase(mikai::backend::MikaiClient& client) {
    QWidget* decoy = makeActiveDecoy("probeActivePlayerB");
    if (decoy == nullptr) return false;

    auto* window = new mikai::app::MainWindow(client);
    if (!bringShellUpUnderDecoy(window, decoy) || !askIsPossible(window)) {
        drain(window);
        delete window;
        delete decoy;
        return false;
    }

    QEventLoop loop;
    QTimer driver;
    driver.setInterval(10);
    QTimer deadline;
    deadline.setSingleShot(true);
    QPointer<mikai::app::MainWindow> windowWatch(window);
    QPointer<QDialog> promptWatch;
    bool acted = false;

    QObject::connect(&driver, &QTimer::timeout, &loop, [&] {
        QDialog* prompt = findSetupPrompt();
        if (prompt == nullptr || acted) {
            // An inventory that is already ready and asks for nothing means this
            // machine has every component: the `acted` check below then fails.
            if (!acted && window != nullptr && !askIsPossible(window)) {
                driver.stop();
                loop.quit();
            }
            return;
        }
        acted = true;
        promptWatch = prompt;

        check(prompt->isVisible() && QApplication::activeModalWidget() == prompt,
              "the setup prompt is the visible active modal window");
        check(decoy != nullptr && decoy->isVisible() && decoy->isFullScreen(),
              "the ask's native owner is the visible fullscreen player-like window");
        check(prompt->windowHandle() != nullptr &&
                  prompt->windowHandle()->transientParent() == decoy->windowHandle(),
              "the setup prompt's transient parent is the fullscreen player-like window");

        // Deletion, not close(): no close event is delivered, so only the ask's
        // own owner guard can end the nested loop.
        delete window;
        window = nullptr;
        check(windowWatch.isNull(), "the shell was deleted programmatically");
        driver.stop();
        loop.quit();
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        rejectSetupPrompt();
        loop.quit();
    });

    driver.start();
    deadline.start(20000);
    loop.exec();
    driver.stop();
    deadline.stop();
    rejectSetupPrompt();

    check(acted, "the setup prompt appeared over a fullscreen player-like window before the "
                 "shell was deleted");
    check(windowWatch.isNull(), "the shell is gone");
    check(promptWatch.isNull(),
          "deleting the shell ended the ask instead of leaving its loop running");
    check(QApplication::activeModalWidget() == nullptr,
          "no modal window outlived the shell that asked");

    // Only reached when the phase got this far; a failed run that never saw the
    // ask still has a live shell to take down.
    drain(window);
    delete window;
    window = nullptr;

    // The process must still be able to show and activate a window: the guard
    // ended one ask, it did not leave the app in a half-shut state.
    auto* alive = new QWidget(nullptr, Qt::Window);
    alive->setObjectName(QStringLiteral("probeLivenessWindow"));
    alive->resize(320, 200);
    alive->show();
    check(pumpUntilActive(alive), "the application still shows and activates windows");
    delete alive;
    delete decoy;
    return !g_failed;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("MainWindowModalProbe"));
    // The probe delivers close events directly and deletes its own windows, so
    // an implicit last-window quit must never end it early.
    app.setQuitOnLastWindowClosed(false);

    // Isolated preferences: the probe must not read or write the viewer's.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "[modal-prompt-probe] FAIL: no temporary settings directory\n");
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());

    // The probe never reaches the API. A probe-only key keeps a request that is
    // going to fail on a closed localhost port away from the viewer's real .env.
    const QString envPath = QDir(settingsDir.path()).filePath(QStringLiteral(".env"));
    {
        QFile env(envPath);
        if (env.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            env.write("X-API-Key: probe-only\n");
            env.close();
        }
    }
    mikai::backend::setEnvFilePathOverride(envPath);

    // The app installs this policy before its first window; the probe needs the
    // same rule to activate a decoy from a console.
    mikai::app::WindowRaisePolicy::install();

    // The inventory is a filesystem scan; nothing here touches the network.
    mikai::backend::MikaiClient client(QString(), QStringLiteral("http://127.0.0.1:1"));

    const bool ownerPhase = runActiveOwnerDestructionPhase(client);
    const bool shellPhase = runShellDestructionPhase(client);

    if (!ownerPhase || !shellPhase || g_failed) {
        std::fprintf(stderr, "[modal-prompt-probe] FAIL\n");
        std::fflush(stderr);
        return 1;
    }
    std::fprintf(stderr, "[modal-prompt-probe] PASS\n");
    std::fflush(stderr);
    return 0;
}
