#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSettings>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <optional>

#include "ProbeIsolation.hpp"
#include "backend/MikaiClient.hpp"
#include "pages/MainWindow.hpp"
#include "player/core/VideoSession.hpp"
#include "player/quick/PlayerController.hpp"

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
    QPointer<mplayer::quick::PlayerController> controller = window.currentMediaPlayer();
    mplayer::VideoSession* session = controller != nullptr ? controller->session() : nullptr;
    QPointer<mplayer::VideoSession> guardedSession(session);
    if (!accepted || controller == nullptr || session == nullptr) {
        std::fprintf(stderr, "FAIL: MainWindow did not retain the requested media controller\n");
        return 1;
    }

    QString playbackError;
    const auto errorConnection =
        QObject::connect(session, &mplayer::VideoSession::errorOccurred, &app,
                         [&playbackError](const QString& error) { playbackError = error; });
    const QString expected = QDir::cleanPath(media);
    const uint64_t initialEpoch = session->sourceEpoch();
    const auto stillOwnsRequestedSource = [&] {
        return controller != nullptr && guardedSession != nullptr &&
               window.currentMediaPlayer() == controller &&
               controller->session() == guardedSession &&
               QDir::cleanPath(guardedSession->sourceUrl()) == expected;
    };
    const bool ready = waitUntil(
        [&] {
            if (!stillOwnsRequestedSource() || controller->window() == nullptr ||
                !controller->isWindowVisible()) {
                return false;
            }
            return guardedSession->isContextReady() && guardedSession->hasVideo() &&
                   guardedSession->videoWidth() > 0 && guardedSession->videoHeight() > 0 &&
                   guardedSession->playbackDiagnostics().renderedOutputFrames >= 3 &&
                   controller->window()->hasVisibleOutputFrame();
        },
        timeoutSeconds * 1000);
    const uint64_t readyEpoch = guardedSession != nullptr ? guardedSession->sourceEpoch() : 0;
    const auto path = ready && stillOwnsRequestedSource()
                          ? guardedSession->probeMpvProperty(
                                QStringLiteral("path"), mplayer::ProbePropertyKind::Text)
                          : std::nullopt;
    const auto mpvVersion = ready && stillOwnsRequestedSource()
                                ? guardedSession->probeMpvProperty(
                                      QStringLiteral("mpv-version"),
                                      mplayer::ProbePropertyKind::Text)
                                : std::nullopt;
    const auto ffmpegVersion = ready && stillOwnsRequestedSource()
                                   ? guardedSession->probeMpvProperty(
                                         QStringLiteral("ffmpeg-version"),
                                         mplayer::ProbePropertyKind::Text)
                                   : std::nullopt;
    std::fprintf(stderr, "runtime versions mpv=%s ffmpeg=%s\n",
                 mpvVersion ? qPrintable(mpvVersion->text) : "unavailable",
                 ffmpegVersion ? qPrintable(ffmpegVersion->text) : "unavailable");
    const QString actual =
        path && path->kind == mplayer::ProbePropertyValue::Kind::Text
            ? QDir::cleanPath(path->text)
            : QString();
    const auto renderedFrames =
        guardedSession != nullptr
            ? guardedSession->playbackDiagnostics().renderedOutputFrames
            : uint64_t{0};
    const bool sourceStable = stillOwnsRequestedSource() && readyEpoch != 0 &&
                              guardedSession->sourceEpoch() == readyEpoch;
    const bool windowPresented = controller != nullptr && controller->window() != nullptr &&
                                 controller->isWindowVisible() &&
                                 controller->window()->hasVisibleOutputFrame();
    const bool passed = ready && windowPresented && sourceStable && playbackError.isEmpty() &&
                        path.has_value() &&
                        actual.compare(expected, Qt::CaseInsensitive) == 0 &&
                        guardedSession->isPlaying();
    QObject::disconnect(errorConnection);
    std::fprintf(stderr,
                 "startup recovery %s elapsed=%lldms context=%d video=%d frames=%llu "
                 "epoch=%llu->%llu path=%s expected=%s playing=%d window-presented=%d "
                 "source-stable=%d error=%s\n",
                 passed ? "PASS" : "FAIL", static_cast<long long>(elapsed.elapsed()),
                 int(guardedSession != nullptr && guardedSession->isContextReady()),
                 int(guardedSession != nullptr && guardedSession->hasVideo()),
                 static_cast<unsigned long long>(renderedFrames),
                 static_cast<unsigned long long>(initialEpoch),
                 static_cast<unsigned long long>(readyEpoch), qPrintable(actual),
                 qPrintable(expected),
                 int(guardedSession != nullptr && guardedSession->isPlaying()),
                 windowPresented, sourceStable, qPrintable(playbackError));
    window.close();
    return passed ? 0 : 1;
}
