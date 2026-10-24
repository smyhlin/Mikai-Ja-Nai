#include "CrashLog.hpp"
#include "I18n.hpp"
#include "WindowRaisePolicy.hpp"
#include "pages/MainWindow.hpp"
#include "state/AppearanceStore.hpp"
#include "theme/Theme.hpp"
#include "workers/Runner.hpp"

#include <ShellTheme.hpp>

#include <backend/MikaiClient.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <shellapi.h>
#endif

namespace {

QStringList launchArguments(int argc, char** argv) {
#ifdef Q_OS_WIN
    // The CRT's narrow argv loses non-ASCII Explorer filenames. Read Windows'
    // actual Unicode command line before QApplication consumes any Qt options.
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments != nullptr) {
        QStringList result;
        result.reserve(count);
        for (int i = 0; i < count; ++i) result.append(QString::fromWCharArray(arguments[i]));
        LocalFree(arguments);
        return result;
    }
#endif
    QStringList result;
    result.reserve(argc);
    for (int i = 0; i < argc; ++i) result.append(QFile::decodeName(argv[i]));
    return result;
}

struct LaunchRequest {
    QString video;
    QString error;
};

LaunchRequest parseLaunchRequest(const QStringList& arguments) {
    LaunchRequest request;
    bool positionalOnly = false;
    bool supplied = false;
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const QString& argument = arguments[index];
        if (!positionalOnly && argument == QStringLiteral("--")) {
            positionalOnly = true;
            continue;
        }
        QString path;
        if (!positionalOnly && argument == QStringLiteral("--open-video")) {
            if (++index == arguments.size()) {
                request.error = QCoreApplication::translate(
                    "CommandLine", "The --open-video option requires a local file path.");
                return request;
            }
            // Consume exactly one literal argument. A filename that starts with
            // '-' is a filename here, never another application or Qt option.
            path = arguments[index];
        } else if (!positionalOnly && argument.startsWith(QLatin1Char('-'))) {
            request.error = QCoreApplication::translate(
                "CommandLine", "Unknown command-line option: %1").arg(argument);
            return request;
        } else {
            path = argument;
        }
        if (supplied) {
            request.error = QCoreApplication::translate(
                "CommandLine", "Specify only one video file per launch.");
            return request;
        }
        supplied = true;
        if (path.isEmpty()) {
            request.error = QCoreApplication::translate(
                "CommandLine", "The video file path cannot be empty.");
            return request;
        }
        // mpv checks readability asynchronously; a UNC stat could block startup.
        request.video = QFileInfo(path).absoluteFilePath();
    }
    return request;
}

void showLaunchError(const QString& error, QWidget* parent = nullptr) {
    qCritical().noquote() << "mikai: cannot open command-line video:" << error;
    auto* message = new QMessageBox(QMessageBox::Critical,
        QCoreApplication::translate("CommandLine", "Cannot open video"),
        error, QMessageBox::Ok, parent);
    message->setTextFormat(Qt::PlainText);
    message->exec();
    delete message;
}

// Cold runtime selection is the launch's one expensive step: describing the
// packaged module hashes ~100MB, and doing that on the GUI thread is what froze
// the first frames. It runs on a Runner worker, and only the commit - loading
// the one module this process pins - runs on the GUI thread.
//
// A player that opens while this is still hashing must not report a missing
// runtime: the resolution is in flight, `runtimeResolutionPending()` says so,
// and the player's own surface shows the pending line instead of a refusal.
// Nothing here reserves a second core: a commit that arrives from another owner
// resolves to the same module or to a recorded refusal, so a second module can
// never be loaded and a hot swap never happens.
void startRuntimeResolution(mikai::app::Runner& runner, mikai::app::MainWindow* window) {
    if (window == nullptr) return;
    const mplayer::native::ResolutionLease lease = mplayer::native::beginResolution();
    if (!lease) {
        // Another owner already has the one resolution, or a module is pinned.
        // Startup stays quiet rather than competing for it: the recorded
        // selection and the pending line already are the truth.
        return;
    }
    // The window is alive in this frame, but the worker finishes after it may
    // have gone, so every GUI body re-checks its guard before touching it, and
    // the dispatch closure is copied before any of it can disappear.
    const QPointer<mikai::app::MainWindow> target(window);
    runner.run(
        [lease] {
            // Resolution validates installed bytes; it neither loads nor
            // downloads, and it is the only half that hashes.
            return QVariant::fromValue(
                mplayer::native::resolveRuntime(mplayer::native::storedRuntimeChoice()));
        },
        [lease, target](const QVariant& result) {
            // The terminal state is published first, and the resolution is
            // released after it: a waiter asks for the settled outcome, and a
            // stale lease must not be able to clear a newer owner's work.
            const mplayer::native::RuntimeSelection selection = mplayer::native::commitRuntime(
                result.value<mplayer::native::RuntimeSelection>());
            mplayer::native::endResolution(lease);
            if (target.isNull()) return;
            if (selection.loaded) {
                qInfo().noquote() << "mikai: player runtime"
                                  << mplayer::native::runtimeStatusText();
            }
            if (selection.nativeNotApplied) {
                qWarning().noquote() << "mikai: native runtime not applied:" << selection.reason;
            }
            if (selection.restartRequired) {
                qInfo().noquote() << "mikai: runtime choice pending" << selection.reason;
            }
            if (!selection.loaded) {
                qWarning().noquote() << "mikai: mpv runtime unavailable:" << selection.reason;
            }
        },
        [lease, target](const mikai::backend::Failure& failure) {
            // A worker that threw never resolved a module: the runtime is still
            // absent, so the reason is named as one and the resolution ends.
            mplayer::native::endResolution(lease);
            if (target.isNull()) return;
            qWarning().noquote() << "mikai: mpv runtime unavailable:"
                                 << mikai::backend::failureLine(failure);
        });
}

}  // namespace

int main(int argc, char** argv) {
    // Before anything else, so a failure during startup still leaves a trace.
    // The log and any crash report land in the folder this exe runs from.
    mikai::app::installCrashLog();

    const QStringList arguments = launchArguments(argc, argv);
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    // Application file arguments belong to our parser, not QApplication. In
    // particular, a video named "-platform" must never select a Qt plugin.
    // Normal launch and the autorun command supply no application arguments.
    int qtArgc = 1;
    char* qtArgv[] = {argv[0], nullptr};
    QApplication app(qtArgc, qtArgv);
    app.setApplicationName(QStringLiteral("Mikai Wrapper"));
    app.setApplicationDisplayName(QStringLiteral("Mikai"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    app.setOrganizationName(QStringLiteral("Mikai"));
    app.setWindowIcon(QIcon(QStringLiteral(":/app/mikai.png")));
    app.setDesktopFileName(QStringLiteral("mikai"));

    // After the identity, which the preference and catalog paths resolve through,
    // and before the runtime selection whose log lines go through these
    // translators. The manager owns them for the life of the application.
    if (!mikai::app::applyInterfaceLanguage(mikai::app::languagePreference())) {
        qWarning("mikai: continuing in English; the requested interface language is unavailable");
    }

    app.setStyle(QStringLiteral("Fusion"));
    mikai::app::theme::applyPalette(mikai::app::currentAppearance());
    mplayer::shell::setAccent(mikai::app::theme::accentColor(mikai::app::currentAppearance()));
    app.setStyleSheet(
        mikai::app::theme::buildStylesheet(mikai::app::currentAppearance()));

    const LaunchRequest launch = parseLaunchRequest(arguments);
    if (!launch.error.isEmpty()) {
        showLaunchError(launch.error);
        return 2;
    }

    // After the application object exists (the policy installs an event filter
    // on it) and before any window is shown, so the first window - the shell -
    // already behaves like every window that follows it: a newly opened player,
    // settings or cover arrives above the app's other windows and is usable
    // immediately. The deliberately non-activating windows (the engine build
    // overlay, a parked PiP float) keep their behaviour; see the policy's own
    // exemption list.
    mikai::app::WindowRaisePolicy::install();

    // The drain runs while the event loop is still alive, never after it: the
    // completion shutdownAll() reports is delivered to the GUI thread, so a call
    // made once exec() has returned would queue a callback nothing ever runs.
    // It is therefore requested when the window closes, while the loop is still
    // accepting posted events, and the quit the completion triggers is the quit
    // that ends the loop, so the drain completes *inside* it.
    //
    // The window's own close is what starts this, so the shell's implicit
    // last-window quit is suppressed: Qt::quitOnLastWindowClosed would otherwise
    // tear the loop down with a worker's last task still running, which is the
    // race the drain exists to close.

    // The startup Runner owns no widget and outlives the window it works for, so
    // the cold runtime resolution can still finish after the window is gone
    // without touching anything the window owned. Its pools are the app's last
    // users of the backend client, which is what makes the drain the right place
    // to end the client's life.
    mikai::app::Runner startupRunner;

    app.setQuitOnLastWindowClosed(false);
    QObject::connect(&app, &QGuiApplication::lastWindowClosed, &app, [&app] {
        // Cancel and drain every pool the app owns. The completion arrives on
        // the GUI thread once each has actually retired, and only then does the
        // loop end - the backend objects a worker may still reference outlive
        // the last task, and the GUI answers throughout.
        mikai::app::Runner::shutdownAll([&app] { app.quit(); });
    });

    {
        mikai::backend::MikaiClient client;
        const auto startupMode = launch.video.isEmpty()
            ? mikai::app::MainWindow::StartupMode::Application
            : mikai::app::MainWindow::StartupMode::FilePlayer;
        mikai::app::MainWindow window(client, startupMode);
        window.show();
        // After the first paint: the runtime is resolved on a worker rather than
        // blocking show(), and the app is fully usable without a runtime - home,
        // search and settings never touch mpv.
        startRuntimeResolution(startupRunner, &window);
        if (!launch.video.isEmpty()) {
            // Establish the app's resolution lease first, then use the exact
            // picker/drop route. The player's existing wait joins that lease;
            // no synchronous hashing or processing-preset override is added.
            QTimer::singleShot(0, &window, [&window, video = launch.video] {
                if (!window.openMedia(video)) {
                    showLaunchError(QCoreApplication::translate(
                        "CommandLine", "The video player could not be opened."), &window);
                }
            });
        }
        app.exec();
        // The window is still open here only when the loop ended another way, so
        // its own teardown ran before anything a worker may still referenced.
        // The client stays alive until it leaves this scope.
    }

    // The loop is done and the backend objects are gone, so no pool a worker
    // used can still reference them. This last call retires anything left from a
    // loop that ended without the window ever closing.
    mikai::app::Runner::shutdownAll([] {});
    return 0;
}
