#include "CrashLog.hpp"
#include "I18n.hpp"
#include "pages/MainWindow.hpp"
#include "theme/Theme.hpp"

#include <backend/MikaiClient.hpp>
#include <backend/Runtime.hpp>

#include <QApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

namespace {

/// Put the directory holding libmpv on PATH before the first QApplication.
///
/// The player is in-process here, so the loader resolves libmpv when the
/// process starts rather than when an episode opens. Doing it up front means a
/// missing runtime is one clear log line instead of a dialog on first play.
void provisionMpvRuntime() {
    const QDir runtime = mikai::backend::ensureRuntime();
    if (runtime.path().isEmpty()) {
        qWarning("mikai: no mpv runtime found; playback will not start");
        return;
    }
    QDir::addSearchPath(QStringLiteral("mpv"), runtime.path());
    const QByteArray current = qgetenv("PATH");
    QByteArray path = runtime.path().toLocal8Bit();
    if (!current.isEmpty()) {
        path += QByteArrayLiteral(";") + current;
    }
    qputenv("PATH", path);
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

    provisionMpvRuntime();

    mikai::backend::MikaiClient client;
    mikai::app::MainWindow window(client);
    window.show();
    return app.exec();
}
