// The verdict reads the surface's own framebuffer, never the chrome grab.

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

#include <cstdio>

#include "PlayerShell.hpp"
#include "ProbePicture.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSurface.hpp"

using namespace mplayer;

namespace {

int visibleChrome(const mplayer::shell::PlayerShell* shell) {
    int count = 0;
    for (const QWidget* child : shell->findChildren<QWidget*>()) {
        if (child->isVisible()) ++count;
    }
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // A close must not end the loop before the guard is checked.
    app.setQuitOnLastWindowClosed(false);

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiShellProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("ShellProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }

    const QString media = argc > 1
                              ? QString::fromLocal8Bit(argv[1])
                              : QStringLiteral("av://lavfi:testsrc2=size=1280x720:rate=25");

    auto* shell = new shell::PlayerShell(nullptr, /*frameless=*/false);
    shell->resize(1280, 760);
    shell->show();

    shell->setMedia(media, QStringLiteral("local"), QStringLiteral("1080p"));
    shell->setDuration(600);
    shell->setRate(1.0);
    shell->setVolume(0.6);
    shell->setPlaying(true);
    shell->setAspectRatio(16.0 / 9.0);

    auto* surface = shell->videoSurface();
    surface->open(media);
    QObject::connect(surface, &VideoSurface::positionChanged, shell,
                     [shell](double seconds) { shell->setPosition(seconds); });
    QObject::connect(surface, &VideoSurface::durationChanged, shell,
                     [shell](double seconds) { shell->setDuration(seconds); });

    QString mediaError;
    QObject::connect(surface, &VideoSurface::errorOccurred, &app,
                     [&mediaError](const QString& message) { mediaError = message; });

    mplayer::probes::Verdict verdict;
    mplayer::probes::PlaybackSampler sampler(*surface);
    QElapsedTimer clock;
    clock.start();
    bool done = false;
    bool chromeBeforeShot = false;
    bool chromeAfterShot = false;

    QTimer tick;
    tick.setInterval(100);
    const auto finish = [&] {
        if (done) return;
        done = true;
        tick.stop();

        verdict.metric(QStringLiteral("chrome widgets visible after auto-hide: %1")
                           .arg(visibleChrome(shell)));
        verdict.metric(QStringLiteral("state after idle: %1").arg(shell->stateName()));
        const auto popup =
            static_cast<qulonglong>(reinterpret_cast<quintptr>(QApplication::activePopupWidget()));
        verdict.metric(QStringLiteral("activePopup: 0x%1").arg(popup, 0, 16));

        const mplayer::probes::PlaybackSample& sample = sampler.later();
        verdict.metric(QStringLiteral("video: %1x%2 position=%3 s duration=%4 s playing=%5")
                           .arg(sample.videoWidth)
                           .arg(sample.videoHeight)
                           .arg(sample.position, 0, 'f', 2)
                           .arg(surface->duration(), 0, 'f', 2)
                           .arg(int(surface->isPlaying())));

        verdict.metric(QStringLiteral("native status: polling=%1 (native work not requested); "
                                      "filter presence/absence not proven by this probe")
                           .arg(int(surface->nativeStatusPolling())));

        const bool saved = sample.hasPicture() &&
                           sample.framebuffer.save(QStringLiteral("shell_probe_gl.png"));
        verdict.metric(QStringLiteral("framebuffer png: %1")
                           .arg(saved ? QStringLiteral("written") : QStringLiteral("FAILED")));
        verdict.require(saved, QStringLiteral("framebuffer PNG written to shell_probe_gl.png"));

        verdict.require(mediaError.isEmpty(),
                        QStringLiteral("no playback error reported (%1)")
                            .arg(mediaError.isEmpty() ? QStringLiteral("none") : mediaError));

        mplayer::probes::requirePlayback(verdict, sampler.earlier(), sampler.later());

        // PlayerShell has no WA_DeleteOnClose, so close() leaves it alive.
        QPointer<shell::PlayerShell> guard(shell);
        shell->close();
        if (!guard.isNull()) shell->deleteLater();
        QTimer::singleShot(200, &app, [&, guard] {
            verdict.require(guard.isNull(), QStringLiteral("shell closed and destroyed"));
            app.exit(verdict.report("shell_probe"));
        });
    };

    QObject::connect(&tick, &QTimer::timeout, &app, [&] {
        if (done) return;
        if (!chromeBeforeShot && clock.elapsed() >= 2000) {
            chromeBeforeShot = true;
            const QImage frame = shell->grab().toImage();
            frame.save(QStringLiteral("shell_probe.png"));
            verdict.metric(QStringLiteral("chrome widgets visible before auto-hide: %1")
                               .arg(visibleChrome(shell)));
        }
        if (!chromeAfterShot && clock.elapsed() >= 4000) {
            chromeAfterShot = true;
            const QImage frame = shell->grab().toImage();
            frame.save(QStringLiteral("shell_probe_autohidden.png"));
        }
        const bool paired = sampler.tick();
        if (!mediaError.isEmpty() || (paired && chromeAfterShot) || clock.elapsed() >= 9000) {
            finish();
        }
    });
    tick.start();

    return app.exec();
}
