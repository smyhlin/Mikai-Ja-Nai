#include "CrashLog.hpp"
#include "I18n.hpp"
#include "pages/MainWindow.hpp"
#include "theme/Theme.hpp"
#include "workers/Runner.hpp"

#include <backend/MikaiClient.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>

#include <QApplication>
#include <QDebug>
#include <QPointer>

namespace {

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

    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Mikai Wrapper"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    app.setOrganizationName(QStringLiteral("Mikai"));

    // After the identity, which the preference and catalog paths resolve through,
    // and before the runtime selection whose log lines go through these
    // translators. The manager owns them for the life of the application.
    if (!mikai::app::applyInterfaceLanguage(mikai::app::languagePreference())) {
        qWarning("mikai: continuing in English; the requested interface language is unavailable");
    }

    app.setStyle(QStringLiteral("Fusion"));
    app.setStyleSheet(mikai::app::theme::buildStylesheet());

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
        mikai::app::MainWindow window(client);
        window.show();
        // After the first paint: the runtime is resolved on a worker rather than
        // blocking show(), and the app is fully usable without a runtime - home,
        // search and settings never touch mpv.
        startRuntimeResolution(startupRunner, &window);
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
