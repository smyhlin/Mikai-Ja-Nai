// The native HUD probe exercises the production typed controller, native Quick
// scene and its owning VideoSession. It never supplies a status snapshot.
// --framebuffer-capture reads the Quick scene's rendered framebuffer.

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QQuickWindow>
#include <QScreen>
#include <QSGRendererInterface>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <functional>

#include "PlayerController.hpp"
#include "PlayerWindow.hpp"
#include "ProbeRuntime.hpp"
#include "ProbeUi.hpp"
#include "VideoSession.hpp"
#include "native/status/NativeMenu.hpp"
#include "native/status/NativeTypes.hpp"

using namespace mplayer;
using namespace mplayer::quick;

namespace {

int failures = 0;

void check(bool ok, const QString& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) ++failures;
}

void note(const QString& what) { std::printf("NOTE %s\n", qPrintable(what)); }

struct StreamStatsReading {
    double bytesPerSecond = 0.0;
    double framesPerSecond = 0.0;
    StreamFpsSource source = StreamFpsSource::Unknown;
    bool seen = false;
};

struct CropReading {
    long total = 0;
    long bright = 0;
    long brightColumns = 0;
    long dark = 0;

    double darkFraction() const { return total > 0 ? double(dark) / double(total) : 0.0; }
};

CropReading readCrop(const QImage& crop) {
    CropReading reading;
    if (crop.isNull()) return reading;
    for (int x = 0; x < crop.width(); ++x) {
        bool columnCarriesText = false;
        for (int y = 0; y < crop.height(); ++y) {
            const QColor pixel = crop.pixelColor(x, y);
            const int sum = pixel.red() + pixel.green() + pixel.blue();
            ++reading.total;
            if (sum > 480) {
                ++reading.bright;
                columnCarriesText = true;
            } else if (sum < 150) {
                ++reading.dark;
            }
        }
        if (columnCarriesText) ++reading.brightColumns;
    }
    return reading;
}

QImage presentedWindow(PlayerWindow* window) {
    if (window == nullptr || window->screen() == nullptr) return {};
    return window->screen()->grabWindow(window->winId()).toImage();
}

/// Saves the actual native scene. Bright-pixel checks apply only to the
/// explicitly bright lavfi testsrc2 workload; otherwise the image is evidence,
/// not a black-frame verdict.
void shoot(PlayerWindow* window, const QString& name, bool framebufferCapture,
           bool knownBright, bool hudExpected) {
    if (window == nullptr) {
        check(false, QStringLiteral("the production Quick window exists for screenshot: ") + name);
        return;
    }
    QImage image;
    if (framebufferCapture) {
        image = window->grabWindow();
    } else {
        window->raiseWindow();
        QCoreApplication::processEvents();
        QThread::msleep(250);
        QCoreApplication::processEvents();
        image = presentedWindow(window);
    }
    if (image.isNull()) {
        check(false, QStringLiteral("the actual Quick scene produced a screenshot: ") + name);
        return;
    }
    if (framebufferCapture) {
        const QString fullPath = QDir::current().filePath(
            QStringLiteral("build-cmake/native_hud_probe_fbo_full.png"));
        check(QDir().mkpath(QFileInfo(fullPath).absolutePath()) && image.save(fullPath),
              QStringLiteral("the fullscreen Quick framebuffer evidence was saved"));
    }

    const qreal ratio = window->devicePixelRatio();
    const int hudWidth = qMin(image.width(), qRound(320 * ratio));
    const int hudHeight = qMin(image.height(), qRound(150 * ratio));
    const QImage hudCrop = image.copy(image.width() - hudWidth, 0, hudWidth, hudHeight);
    const QString path = QDir::current().filePath(QStringLiteral("build-cmake/") + name);
    const bool written = QDir().mkpath(QFileInfo(path).absolutePath()) && hudCrop.save(path);
    check(written, QStringLiteral("screenshot written: ") + name);
    if (!written) return;
    if (hudExpected) {
        const CropReading reading = readCrop(hudCrop);
        check(reading.bright >= 120 && reading.brightColumns >= 40,
              QStringLiteral("the captured Quick HUD shows rendered text: %1 bright pixels over %2 columns")
                  .arg(reading.bright)
                  .arg(reading.brightColumns));
        check(reading.darkFraction() >= 0.05,
              QStringLiteral("the captured Quick HUD overlays its native scene (%1% dark pixels)")
                  .arg(100.0 * reading.darkFraction(), 0, 'f', 1));
    } else {
        note(QStringLiteral("native HUD is not rendered for the actual selected runtime/status"));
    }
    if (knownBright) {
        const CropReading reading = readCrop(image);
        check(reading.total > 0 && reading.bright > 0,
              QStringLiteral("the known-bright testsrc2 Quick scene contains rendered pixels"));
    }
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    return predicate();
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QSG_RENDER_LOOP", QByteArrayLiteral("basic"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QApplication app(argc, argv);
    if (!mikai::probes::initializeUi(app)) return 2;
    app.setQuitOnLastWindowClosed(false);

    const bool framebufferCapture =
        app.arguments().contains(QStringLiteral("--framebuffer-capture"));
    QStringList positionalArguments = app.arguments();
    positionalArguments.removeFirst();
    positionalArguments.removeAll(QStringLiteral("--framebuffer-capture"));
    std::fprintf(stderr, "capture mode: %s\n",
                 framebufferCapture ? "fullscreen Quick framebuffer" : "presented Quick window");

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiNativeHudProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("NativeHudProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());

    if (!mplayer::probes::selectRuntime()) return 2;

    const bool knownBright = positionalArguments.isEmpty();
    const QString url = knownBright
                            ? QStringLiteral("av://lavfi:testsrc2=size=1280x720:rate=25")
                            : positionalArguments.constFirst();
    auto* controller = new PlayerController(QStringLiteral("native HUD probe"), url);
    PlayerWindow* const window = controller->window();
    VideoSession* const session = controller->session();
    check(window != nullptr && session != nullptr,
          QStringLiteral("the HUD belongs to its production PlayerWindow and VideoSession"));
    if (window == nullptr || session == nullptr) {
        delete controller;
        return 2;
    }
    StreamStatsReading streamStats;
    QObject::connect(session, &VideoSession::streamStatsChanged, controller,
                     [&streamStats](double bytesPerSecond, double framesPerSecond,
                                    StreamFpsSource source) {
                         streamStats.bytesPerSecond = bytesPerSecond;
                         streamStats.framesPerSecond = framesPerSecond;
                         streamStats.source = source;
                         streamStats.seen = true;
                     });
    window->resize(1280, 760);
    controller->showWindow();
    if (framebufferCapture) controller->toggleFullscreen();
    controller->startPlayer();

    const bool sceneReady = waitFor([&] {
        return window->rootObject() != nullptr && window->isVisible();
    }, 10000);
    check(sceneReady, QStringLiteral("Mikai.App PlayerEntry loaded in the real Quick window"));
    if (!sceneReady) {
        delete controller;
        return 2;
    }

    const bool videoReady = waitFor([&] {
        return session->hasVideo() && window->hasVisibleOutputFrame();
    }, 12000);
    check(videoReady,
          QStringLiteral("the session's own source produced a visible Quick video frame"));
    check(window->controller() == controller && controller->session() == session,
          QStringLiteral("controller, scene and source retain one playback owner"));
    check(streamStats.seen &&
              qAbs(controller->networkBytesPerSecond() - streamStats.bytesPerSecond) < 0.01 &&
              qAbs(controller->streamFramesPerSecond() - streamStats.framesPerSecond) < 0.01,
          QStringLiteral("Quick stream-stat properties consume this session's observed values"));
    check(streamStats.source == StreamFpsSource::Unknown ||
              streamStats.source == StreamFpsSource::MpvOutput ||
              streamStats.source == StreamFpsSource::Container,
          QStringLiteral("measured FPS retains its source classification"));

    // The controller's published fields and the native consumer's status lines
    // are derived from the session's real status and the process's real runtime
    // selection/preferences; this probe never injects a made-up snapshot.
    native::menu::Inputs actualInputs;
    actualInputs.preferences = native::loadNativePreferences();
    actualInputs.runtime = native::currentSelection();
    actualInputs.status = session->nativeStatus();
    const native::menu::StatusLines actualLines = native::menu::statusLines(actualInputs);
    check(controller->nativeHudVisible() == actualLines.visible,
          QStringLiteral("typed controller HUD visibility matches actual native status inputs"));
    check(controller->nativeHudActive() == actualLines.active,
          QStringLiteral("typed controller active state is owned by the filter report"));
    check(controller->nativeHudState() == actualLines.state,
          QStringLiteral("typed Quick HUD state matches the actual producer report"));
    const native::NativeStatusView status = session->nativeStatus();
    if (status.snapshot.has_value()) {
        check(controller->nativeHudHeadline() == actualLines.headline &&
                  controller->nativeHudProvider() == actualLines.provider &&
                  controller->nativeHudModel() == actualLines.model &&
                  controller->nativeHudGeometry() == actualLines.geometry &&
                  controller->nativeHudFps() == actualLines.fps &&
                  controller->nativeHudInterpolation() == actualLines.interpolation &&
                  controller->nativeHudDropped() == actualLines.dropped &&
                  controller->nativeHudReason() == actualLines.reason,
              QStringLiteral("typed Quick HUD details match the session's actual producer snapshot"));
        QString key;
        QString reason;
        check(native::statusSnapshotIsConsistent(*status.snapshot, &key, &reason),
              QStringLiteral("the session's published native snapshot is internally consistent"));
    } else {
        check(!controller->nativeHudActive(),
              QStringLiteral("absence of a producer snapshot never claims active processing"));
        if (controller->nativeHudVisible()) {
            check(!controller->nativeHudHeadline().isEmpty(),
                  QStringLiteral("a real persisted request has a nonempty native HUD headline"));
        }
    }
    note(QStringLiteral("actual native HUD: visible=%1 active=%2 state=%3")
             .arg(controller->nativeHudVisible())
             .arg(controller->nativeHudActive())
             .arg(controller->nativeHudState()));

    const bool hudExpected = controller->nativeHudVisible();
    shoot(window, framebufferCapture ? QStringLiteral("native_hud_probe_fbo.png")
                                     : QStringLiteral("native_hud_probe.png"),
          framebufferCapture, knownBright, hudExpected);

    // Snapshot observable playback and HUD state around presentation-only mode
    // changes. The source epoch and URL must not be replaced, and the HUD's
    // consumer-facing state must not write back into native processing.
    const uint64_t pipSourceEpoch = session->sourceEpoch();
    const QString pipSourceUrl = session->sourceUrl();
    const double pipStartPosition = session->position();
    const bool pipWasPlaying = controller->playing();
    const double pipVolume = controller->volume();
    const bool pipMuted = controller->muted();
    const double pipRate = controller->rate();
    const bool pipHudVisible = controller->nativeHudVisible();
    const bool pipHudActive = controller->nativeHudActive();
    const QString pipHudState = controller->nativeHudState();
    const QString pipHudHeadline = controller->nativeHudHeadline();
    const QString pipHudProvider = controller->nativeHudProvider();
    const QString pipHudModel = controller->nativeHudModel();
    const QString pipHudGeometry = controller->nativeHudGeometry();
    const QString pipHudReason = controller->nativeHudReason();
    QElapsedTimer pipElapsed;
    pipElapsed.start();

    // PiP is the same Quick window and session. The production scene suppresses
    // its main-window-only HUD presentation while compact, without substituting
    // a second controller, decoder, status or model.
    controller->togglePip();
    const bool enteredPip = waitFor([&] {
        return controller->isPipActive() && window->compactPip();
    }, 2500);
    check(enteredPip, QStringLiteral("the actual Quick window enters compact PiP presentation"));
    check(controller->window() == window && controller->session() == session &&
              window->controller() == controller,
          QStringLiteral("PiP retains the exact PlayerWindow, controller and VideoSession"));
    check(session->sourceEpoch() == pipSourceEpoch && session->sourceUrl() == pipSourceUrl &&
              controller->mediaPath() == pipSourceUrl,
          QStringLiteral("PiP changes presentation without replacing the source or epoch"));
    const double pipExpectedPosition =
        pipStartPosition + (pipWasPlaying ? pipElapsed.elapsed() / 1000.0 * pipRate : 0.0);
    check(qAbs(session->position() - pipExpectedPosition) <= 1.0,
          QStringLiteral("PiP preserves the same session playhead (expected %1, observed %2)")
              .arg(pipExpectedPosition, 0, 'f', 2)
              .arg(session->position(), 0, 'f', 2));
    check(controller->playing() == pipWasPlaying &&
              qAbs(controller->volume() - pipVolume) < 0.01 &&
              controller->muted() == pipMuted && qAbs(controller->rate() - pipRate) < 0.001,
          QStringLiteral("PiP preserves pause intent, volume, mute and rate"));
    check(controller->nativeHudVisible() == pipHudVisible &&
              controller->nativeHudActive() == pipHudActive &&
              controller->nativeHudState() == pipHudState &&
              controller->nativeHudHeadline() == pipHudHeadline &&
              controller->nativeHudProvider() == pipHudProvider &&
              controller->nativeHudModel() == pipHudModel &&
              controller->nativeHudGeometry() == pipHudGeometry &&
              controller->nativeHudReason() == pipHudReason,
          QStringLiteral("PiP hides presentation only; native HUD remains a consumer of unchanged status"));
    check(window->rootObject()->property("pipMode").toBool(),
          QStringLiteral("the production scene hides the main HUD in compact PiP mode"));
    controller->togglePip();
    const bool restored = waitFor([&] { return !controller->isPipActive() && !window->compactPip(); },
                                  2500);
    check(restored, QStringLiteral("the same Quick window restores from PiP"));

    check(controller->window() == window && controller->session() == session &&
              window->controller() == controller &&
              session->sourceEpoch() == pipSourceEpoch &&
              session->sourceUrl() == pipSourceUrl &&
              qAbs(session->position() -
                   (pipStartPosition +
                    (pipWasPlaying ? pipElapsed.elapsed() / 1000.0 * pipRate : 0.0))) <=
                  1.0 &&
              controller->playing() == pipWasPlaying,
          QStringLiteral("restoring full presentation keeps the same session, playhead and pause intent"));
    check(controller->nativeHudVisible() == pipHudVisible &&
              controller->nativeHudActive() == pipHudActive &&
              controller->nativeHudState() == pipHudState,
          QStringLiteral("returning from PiP does not mutate native processing status"));

    const bool closed = controller->requestClose();
    check(closed, QStringLiteral("the Quick controller acknowledges native window close"));
    delete controller;

    if (failures == 0) {
        std::printf("OK native_hud_probe\n");
    } else {
        std::fprintf(stderr, "FAILURES %d\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
