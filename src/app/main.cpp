#include "CrashLog.hpp"
#include "I18n.hpp"
#include "pages/MainWindow.hpp"
#include "theme/Theme.hpp"

#include <backend/MikaiClient.hpp>
#include <native/NativeRuntimeSelection.hpp>

#include <QApplication>
#include <QDebug>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

namespace {

/// Chooses and pins the mpv module for this process, once.
///
/// The player is in-process, so the module has to be resolved before the first
/// core rather than when an episode opens. A missing or refused runtime is one
/// clear log line here: home, search and the settings dialog all work without
/// it, and the player reports the same named reason when it is asked to play.
void selectPlayerRuntime() {
    const mplayer::native::RuntimeSelection selection = mplayer::native::selectStartupRuntime();
    if (selection.loaded) {
        qInfo().noquote() << "mikai: player runtime" << mplayer::native::runtimeStatusText();
        if (selection.nativeNotApplied) {
            qWarning().noquote() << "mikai: native runtime not applied:" << selection.reason;
        }
        return;
    }
    qWarning().noquote() << "mikai: mpv runtime unavailable:" << selection.reason;
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

    const QLocale locale(mikai::app::resolvedLanguageCode());
    QLocale::setDefault(locale);
    QTranslator qtTranslator;
    QTranslator appTranslator;
    if (locale.language() == QLocale::Ukrainian) {
        const QString translationsPath = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
        if (qtTranslator.load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                              translationsPath)) {
            app.installTranslator(&qtTranslator);
        } else {
            qWarning("mikai: Ukrainian Qt platform translations were not found");
        }

        if (appTranslator.load(QStringLiteral(":/i18n/mikai_uk.qm"))) {
            app.installTranslator(&appTranslator);
        } else {
            qWarning("mikai: embedded Ukrainian application translations were not found");
        }
    }

    app.setStyle(QStringLiteral("Fusion"));
    app.setStyleSheet(mikai::app::theme::buildStylesheet());

    // After the application identity and the translators, and before any
    // widget can reach an mpv core: the selection reads QSettings, which needs
    // the organization name, and its strings reach the log through the same
    // translators.
    selectPlayerRuntime();

    mikai::backend::MikaiClient client;
    mikai::app::MainWindow window(client);
    window.show();
    return app.exec();
}