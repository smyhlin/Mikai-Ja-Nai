#include "pages/MainWindow.hpp"
#include "theme/Theme.hpp"

#include <backend/MikaiClient.hpp>
#include <backend/Runtime.hpp>

#include <QApplication>
#include <QDir>

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
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Mikai Wrapper"));
    app.setOrganizationName(QStringLiteral("Mikai"));
    app.setStyle(QStringLiteral("Fusion"));
    app.setStyleSheet(mikai::app::theme::buildStylesheet());

    provisionMpvRuntime();

    mikai::backend::MikaiClient client;
    mikai::app::MainWindow window(client);
    window.show();
    return app.exec();
}
