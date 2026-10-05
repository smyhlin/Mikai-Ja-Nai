#include "CrashLog.hpp"
#include "I18n.hpp"
#include "pages/MainWindow.hpp"
#include "theme/Theme.hpp"

#include <backend/MikaiClient.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>

#include <QApplication>
#include <QDebug>
#include <QTimer>

namespace {

/// Chooses and pins the mpv module for this process, once.
///
/// The player is in-process, so the module has to be resolved before the first
/// core rather than when an episode opens. A missing or refused runtime is one
/// clear log line here: home, search and the settings dialog all work without
/// it, and the player reports the same named reason when it is asked to play.
///
/// Every outcome is named, and none of them is silent. A Native request that
/// fell back to Standard says so on its own line even though playback works,
/// because "the app started" is not evidence that the chosen runtime ran.
void selectPlayerRuntime() {
    const mplayer::native::RuntimeSelection selection = mplayer::native::selectStartupRuntime();
    if (selection.loaded) {
        qInfo().noquote() << "mikai: player runtime" << mplayer::native::runtimeStatusText();
    }
    if (selection.nativeNotApplied) {
        // The native half of the answer rides alongside Standard's, and it is
        // the one that names what the user asked for and did not get.
        qWarning().noquote() << "mikai: native runtime not applied:" << selection.reason;
    }
    if (selection.restartRequired) {
        qInfo().noquote() << "mikai: runtime choice pending" << selection.reason;
    }
    if (!selection.loaded) {
        // Startup selected nothing and provisioned nothing; the first play runs
        // the provisioning attempt on the pool. Naming it here is what stops a
        // missing runtime from reading as a black player with no cause.
        qWarning().noquote()
            << "mikai: mpv runtime unavailable:" << selection.reason
            << "- the first playback will retry provisioning";
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Before anything else, so a failure during startup still leaves a trace.
    // The log and any crash report land in the folder this exe runs from.
    mikai::app::installCrashLog();

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

    mikai::backend::MikaiClient client;
    mikai::app::MainWindow window(client);
    window.show();
    // After the first paint, never before it: selecting the runtime validates
    // the bundle on disk (hashing ~100MB on a cold cache), and the app is fully
    // usable without a runtime — home, search and settings never touch mpv.
    // Blocking show() on it is what made launch take half a minute.
    QTimer::singleShot(0, &selectPlayerRuntime);
    return app.exec();
}
