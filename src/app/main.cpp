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

void selectPlayerRuntime() {
    const mplayer::native::RuntimeSelection selection = mplayer::native::selectStartupRuntime();
    if (selection.loaded) {
        qInfo().noquote() << "mikai: player runtime" << mplayer::native::runtimeStatusText();
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

    mikai::backend::MikaiClient client;
    mikai::app::MainWindow window(client);
    window.show();
    // After the first paint, never before it: selecting the runtime validates
    // the bundle on disk (hashing ~100MB on a cold cache), and the app is fully
    // usable without a runtime — home, search and settings never touch mpv.
    // Blocking show() on it is what made launch take half a minute.
    QTimer::singleShot(0, &window, [&window] {
        selectPlayerRuntime();
        window.ensureNativeComponents();
    });
    return app.exec();
}
