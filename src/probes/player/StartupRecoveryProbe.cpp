#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QOpenGLWidget>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <optional>

#include "ProbeIsolation.hpp"
#include "VideoSurface.hpp"
#include "backend/MikaiClient.hpp"
#include "pages/MainWindow.hpp"
#include "player/shell/PlayerDialog.hpp"

namespace {

template <typename Predicate>
bool waitUntil(Predicate ready, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return ready();
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Exercise a real cold MainWindow media open through asynchronous runtime resolution."));
    parser.addHelpOption();
    QCommandLineOption mediaOption(QStringList{QStringLiteral("media")},
                                   QStringLiteral("Existing local video fixture."),
                                   QStringLiteral("path"));
    QCommandLineOption timeoutOption(QStringList{QStringLiteral("timeout-seconds")},
                                     QStringLiteral("Startup deadline (10..900 seconds)."),
                                     QStringLiteral("seconds"), QStringLiteral("180"));
    parser.addOption(mediaOption);
    parser.addOption(timeoutOption);
    parser.process(app);
    const QString media = QFileInfo(parser.value(mediaOption)).absoluteFilePath();
    bool timeoutOk = false;
    const int timeoutSeconds = parser.value(timeoutOption).toInt(&timeoutOk);
    if (!QFileInfo(media).isFile() || !timeoutOk || timeoutSeconds < 10 || timeoutSeconds > 900) {
        std::fprintf(stderr, "Usage: StartupRecoveryProbe --media <known-bright-video> "
                             "[--timeout-seconds 180]\n");
        return 2;
    }

    const auto isolation = mikai::probes::isolation::enter(
        argc, argv, {"startup_recovery_probe", true, timeoutSeconds * 1000 + 60000});
    if (isolation != mikai::probes::isolation::Gate::RunHere)
        return mikai::probes::isolation::exitCode();

    QTemporaryDir settings;
    if (!settings.isValid()) return 2;
    app.setOrganizationName(QStringLiteral("MikaiStartupRecoveryProbe"));
    app.setApplicationName(QStringLiteral("StartupRecoveryProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());

    mikai::backend::MikaiClient client(QString(), QStringLiteral("http://127.0.0.1:1"));
    mikai::app::MainWindow window(client, mikai::app::MainWindow::StartupMode::FilePlayer);
    window.resize(960, 600);
    window.show();
    QElapsedTimer elapsed;
    elapsed.start();
    const bool accepted = window.openMedia(media);
    mplayer::shell::PlayerDialog* dialog = window.currentMediaPlayer();
    auto* surface = dialog != nullptr ? dialog->findChild<mplayer::VideoSurface*>() : nullptr;
    if (!accepted || dialog == nullptr || surface == nullptr) {
        std::fprintf(stderr, "FAIL: MainWindow did not retain the requested media player\n");
        return 1;
    }

    QString playbackError;
    QObject::connect(surface, &mplayer::VideoSurface::errorOccurred, &app,
                     [&playbackError](const QString& error) { playbackError = error; });
    const bool ready = waitUntil(
        [&] {
            return surface->isContextReady() && surface->hasVideo() &&
                   surface->videoWidth() > 0 && surface->videoHeight() > 0 &&
                   surface->playbackDiagnostics().renderedOutputFrames >= 3;
        }, timeoutSeconds * 1000);
    const auto path = ready ? surface->probeMpvProperty(
                                  QStringLiteral("path"),
                                  mplayer::ProbePropertyKind::Text)
                             : std::nullopt;
    const auto mpvVersion = ready ? surface->probeMpvProperty(
                                        QStringLiteral("mpv-version"),
                                        mplayer::ProbePropertyKind::Text)
                                   : std::nullopt;
    const auto ffmpegVersion = ready ? surface->probeMpvProperty(
                                           QStringLiteral("ffmpeg-version"),
                                           mplayer::ProbePropertyKind::Text)
                                      : std::nullopt;
    std::fprintf(stderr, "runtime versions mpv=%s ffmpeg=%s\n",
                 mpvVersion ? qPrintable(mpvVersion->text) : "unavailable",
                 ffmpegVersion ? qPrintable(ffmpegVersion->text) : "unavailable");
    const QString expected = QDir::cleanPath(media);
    const QString actual = path && path->kind == mplayer::ProbePropertyValue::Kind::Text
                               ? QDir::cleanPath(path->text)
                               : QString();
    const bool passed = ready && playbackError.isEmpty() && path.has_value() &&
                        actual.compare(expected, Qt::CaseInsensitive) == 0 &&
                        surface->isPlaying();
    std::fprintf(stderr,
                 "startup recovery %s elapsed=%lldms context=%d video=%d frames=%llu "
                 "path=%s expected=%s playing=%d error=%s\n",
                 passed ? "PASS" : "FAIL", static_cast<long long>(elapsed.elapsed()),
                 int(surface->isContextReady()), int(surface->hasVideo()),
                 static_cast<unsigned long long>(surface->playbackDiagnostics().renderedOutputFrames),
                 qPrintable(actual), qPrintable(expected), int(surface->isPlaying()),
                 qPrintable(playbackError));
    window.close();
    return passed ? 0 : 1;
}
