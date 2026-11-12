// The verdict observes the production Qt Quick player window and its video session.

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

#include "PlayerController.hpp"
#include "PlayerWindow.hpp"
#include "ProbePicture.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSession.hpp"

using namespace mplayer;

namespace {

struct Sample {
    QImage frame;
    double position = -1.0;
    int width = 0;
    int height = 0;
};

Sample capture(quick::PlayerController& controller) {
    Sample sample;
    sample.frame = controller.window()->grabVideoFrame();
    sample.position = controller.session()->position();
    sample.width = controller.session()->videoWidth();
    sample.height = controller.session()->videoHeight();
    return sample;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
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
    if (!mplayer::probes::selectRuntime()) return 2;

    const bool generatedBrightWorkload = argc <= 1;
    const QString media = generatedBrightWorkload
                              ? QStringLiteral("av://lavfi:testsrc2=size=1280x720:rate=25")
                              : QString::fromLocal8Bit(argv[1]);
    quick::PlayerController controller(QStringLiteral("ShellProbe"), media);
    quick::PlayerWindow* window = controller.window();
    window->resize(1280, 760);
    QObject::connect(window, &quick::PlayerWindow::sceneLoadFailed, &app,
                     [&app](const QString& error) {
                         std::fprintf(stderr, "FAIL Quick player scene: %s\n", qPrintable(error));
                         app.exit(2);
                     });
    controller.showWindow();
    controller.startPlayer();
    controller.session()->setVolume(60.0);
    controller.toggleFullscreen();

    QString mediaError;
    QObject::connect(controller.session(), &VideoSession::errorOccurred, &app,
                     [&mediaError](const QString& message) { mediaError = message; });

    mplayer::probes::Verdict verdict;
    QElapsedTimer clock;
    clock.start();
    QTimer tick;
    tick.setInterval(100);
    Sample earlier;
    Sample later;
    bool sampledEarlier = false;
    bool sampledLater = false;
    bool chromeBeforeShot = false;
    bool chromeAfterShot = false;
    bool windowedIdleProved = false;
    bool fullscreenActivated = false;
    bool fullscreenAutoHideProved = false;
    bool heldIdleProved = false;
    bool pointerWakeProved = false;
    bool interactionSent = false;
    bool interactionProved = false;
    bool done = false;
    QElapsedTimer chromePhase;

    const auto finish = [&] {
        if (done) return;
        done = true;
        tick.stop();
        verdict.metric(QStringLiteral("Quick chrome state after idle: %1")
                           .arg(controller.stateName()));
        verdict.metric(QStringLiteral("Quick window exposed: %1")
                           .arg(int(window->isExposed())));
        const Sample final = sampledLater ? later : capture(controller);
        verdict.metric(QStringLiteral("video: %1x%2 position=%3 s duration=%4 s playing=%5")
                           .arg(final.width)
                           .arg(final.height)
                           .arg(final.position, 0, 'f', 2)
                           .arg(controller.session()->duration(), 0, 'f', 2)
                           .arg(int(controller.session()->isPlaying())));
        verdict.require(window->isExposed() && window->rootObject() != nullptr,
                        QStringLiteral("production Quick player scene is exposed"));
        verdict.require(window->hasVisibleOutputFrame() && !final.frame.isNull(),
                        QStringLiteral("Quick video item presents a measured frame"));
        verdict.require(mediaError.isEmpty(),
                        QStringLiteral("no playback error reported (%1)")
                            .arg(mediaError.isEmpty() ? QStringLiteral("none") : mediaError));
        if (sampledEarlier && sampledLater) {
            verdict.require(later.position > earlier.position,
                            QStringLiteral("live playback position advanced (%1 -> %2 s)")
                                .arg(earlier.position, 0, 'f', 2)
                                .arg(later.position, 0, 'f', 2));
            const auto picture = mplayer::probes::measurePicture(
                later.frame, later.width, later.height);
            if (generatedBrightWorkload) {
                verdict.require(!picture.isMostlyBlack(),
                                QStringLiteral("known-bright Quick test pattern contains visible pixels (%1/%2 lit)")
                                    .arg(picture.lit).arg(picture.sampled));
            } else {
                verdict.instrumentation(false,
                                        QStringLiteral("black-frame verdict is not applied to arbitrary user media"));
            }
        } else {
            verdict.require(false, QStringLiteral("two live Quick playback samples were captured"));
        }
        verdict.require(windowedIdleProved,
                        QStringLiteral("windowed DEFAULT_FIT chrome stays visible after the idle dwell"));
        verdict.require(fullscreenActivated && fullscreenAutoHideProved,
                        QStringLiteral("fullscreen chrome reaches FULLSCREEN_AUTO_HIDDEN after its idle dwell"));
        verdict.require(heldIdleProved,
                        QStringLiteral("active viewer interaction prevents fullscreen chrome auto-hide"));
        verdict.require(pointerWakeProved,
                        QStringLiteral("Quick-window pointer movement wakes hidden chrome"));
        verdict.require(interactionProved,
                        QStringLiteral("Quick-window Space interaction wakes chrome and toggles playback"));

        const bool savedGl = !final.frame.isNull() &&
                             final.frame.save(QStringLiteral("shell_probe_gl.png"));
        verdict.metric(QStringLiteral("video-item framebuffer png: %1")
                           .arg(savedGl ? QStringLiteral("written") : QStringLiteral("FAILED")));
        verdict.require(savedGl, QStringLiteral("video-item framebuffer PNG written to shell_probe_gl.png"));
        if (window->rootObject() != nullptr) {
            const QImage visible = window->grabWindow();
            visible.save(QStringLiteral("shell_probe.png"));
            visible.save(QStringLiteral("shell_probe_autohidden.png"));
            verdict.metric(QStringLiteral("Quick scene screenshots saved: shell_probe.png, shell_probe_autohidden.png"));
        }
        controller.requestClose();
        QTimer::singleShot(100, &app, [&] {
            verdict.require(!window->isVisible(),
                            QStringLiteral("Quick player window closed"));
            app.exit(verdict.report("shell_probe"));
        });
    };

    QObject::connect(&tick, &QTimer::timeout, &app, [&] {
        if (!mediaError.isEmpty()) {
            finish();
            return;
        }
        if (window->hasVisibleOutputFrame() && controller.session()->isPlaying()) {
            const double position = controller.session()->position();
            if (!sampledEarlier && position >= 0.5) {
                earlier = capture(controller);
                sampledEarlier = !earlier.frame.isNull();
            } else if (sampledEarlier && !sampledLater && position >= earlier.position + 1.0) {
                later = capture(controller);
                sampledLater = !later.frame.isNull();
            }
        }
        if (!chromeBeforeShot && clock.elapsed() >= 1000) {
            chromeBeforeShot = true;
            window->grabWindow().save(QStringLiteral("shell_probe.png"));
            verdict.metric(QStringLiteral("Quick scene screenshot captured in DEFAULT_FIT"));
        }
        if (!windowedIdleProved && clock.elapsed() >= 3200) {
            windowedIdleProved = !controller.autoHidden() &&
                                 controller.stateName() == QLatin1String("DEFAULT_FIT");
            controller.toggleFullscreen();
            fullscreenActivated = controller.fullscreen() &&
                                  controller.stateName() == QLatin1String("FULLSCREEN_ACTIVE");
            chromePhase.restart();
        }
        if (fullscreenActivated && !chromeAfterShot && chromePhase.elapsed() >= 2600 &&
            controller.autoHidden()) {
            fullscreenAutoHideProved = true;
            chromeAfterShot = true;
            window->grabWindow().save(QStringLiteral("shell_probe_autohidden.png"));
            verdict.metric(QStringLiteral("Quick scene screenshot captured after fullscreen idle auto-hide"));
            controller.setInteractionActive(true);
            chromePhase.restart();
        }
        if (chromeAfterShot && !heldIdleProved && chromePhase.elapsed() >= 2700) {
            heldIdleProved = !controller.autoHidden() &&
                             controller.stateName() == QLatin1String("FULLSCREEN_ACTIVE");
            controller.setInteractionActive(false);
            chromePhase.restart();
        }
        if (heldIdleProved && !pointerWakeProved && chromePhase.elapsed() >= 2600 &&
            controller.autoHidden()) {
            QMouseEvent move(QEvent::MouseMove, QPointF(20.0, 20.0), Qt::NoButton,
                             Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &move);
            pointerWakeProved = !controller.autoHidden() &&
                                controller.stateName() == QLatin1String("FULLSCREEN_ACTIVE");
            chromePhase.restart();
        }
        if (pointerWakeProved && !interactionSent && chromePhase.elapsed() >= 250) {
            interactionSent = true;
            const bool wasPlaying = controller.session()->isPlaying();
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &press);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &release);
            interactionProved = controller.session()->isPlaying() != wasPlaying &&
                                !controller.autoHidden();
        }
        if ((sampledLater && interactionSent) || clock.elapsed() >= 16000) finish();
    });
    tick.start();
    return app.exec();
}
