#include "PlayerDialog.hpp"
#include "PlayerShell.hpp"
#include "ProbeIsolation.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSurface.hpp"

#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QTest>

#include <algorithm>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif

namespace {

class PlaybackHost final : public mplayer::shell::PlayerHost {
public:
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    void playbackFailed() override { failed = true; }
    bool failed = false;
};

qint64 residentBytes() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<qint64>(counters.WorkingSetSize);
#endif
    return -1;
}

bool isBlack(const QImage& image) {
    if (image.isNull()) return false;
    int lit = 0;
    int count = 0;
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4) {
            const QColor color = image.pixelColor(x, y);
            lit += color.red() + color.green() + color.blue() > 24;
            ++count;
        }
    }
    return count > 0 && lit * 100 < count;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    std::fprintf(stderr, "Platform: %s, screen: %s\n",
                 qPrintable(QGuiApplication::platformName()),
                 app.primaryScreen() != nullptr ? qPrintable(app.primaryScreen()->name()) : "none");
    const auto isolation = mikai::probes::isolation::enter(
        argc, argv, {"fullscreen_stability_probe", false, 1200000});
    if (isolation != mikai::probes::isolation::Gate::RunHere)
        return mikai::probes::isolation::exitCode();
    if (argc < 4) {
        std::fprintf(stderr, "Usage: fullscreen_stability_probe <moving-nonblack-video> <seconds> <output-directory> [anime4k-preset]\n");
        return 2;
    }
    bool validDuration = false;
    const int seconds = QString::fromLocal8Bit(argv[2]).toInt(&validDuration);
    if (!validDuration || seconds < 5 || seconds > 1100) return 2;
    const QString outputPath = QString::fromLocal8Bit(argv[3]);
    if (!QDir().mkpath(outputPath)) return 2;
    QFile evidence(QDir(outputPath).filePath(QStringLiteral("samples.jsonl")));
    if (!evidence.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
    QTemporaryDir settings;
    if (!settings.isValid()) return 2;
    app.setOrganizationName(QStringLiteral("MikaiStabilityProbe"));
    app.setApplicationName(QStringLiteral("FullscreenStabilityProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    const QString preset = argc > 4 ? QString::fromLocal8Bit(argv[4]) : QStringLiteral("off");
    QSettings().setValue(QStringLiteral("player/anime4k"), preset);
    const QString module = qEnvironmentVariable("MIKAI_MPV_MODULE");
    if (module.isEmpty()) {
        if (!mplayer::probes::selectRuntime()) return 2;
    } else {
        const auto runtime = mplayer::native::selectStandardRuntime(module);
        mplayer::probes::detail::reportSelected(runtime, "explicit module");
        if (!runtime.loaded) {
            std::fprintf(stderr, "Runtime refused: %s\n", qPrintable(runtime.reason));
            return 2;
        }
    }

    PlaybackHost host;
    const QString media = QString::fromLocal8Bit(argv[1]);
    auto* dialog = new mplayer::shell::PlayerDialog(
        QStringLiteral("Fullscreen stability verification"), media,
        {{1080, media}}, 1080, &host);
    auto* surface = dialog->findChild<mplayer::VideoSurface*>();
    auto* shell = dialog->findChild<mplayer::shell::PlayerShell*>();
    if (surface == nullptr || shell == nullptr) return 2;
    QString mediaError;
    QObject::connect(surface, &mplayer::VideoSurface::errorOccurred, &app,
                     [&](const QString& error) { mediaError = error; });
    dialog->startPlayer();
    dialog->showFullScreen();
    dialog->raise();
    dialog->activateWindow();
    QCursor::setPos(dialog->mapToGlobal(QPoint(2, 2)));

    QElapsedTimer clock;
    clock.start();
    qint64 previousTick = 0;
    qint64 maxGap = 0;
    qint64 previousSampleWorkMs = 0;
    qint64 maxSampleWorkMs = 0;
    qint64 maxReadbackMs = 0;
    qint64 maxDesktopGrabMs = 0;
    qint64 maxImageSaveMs = 0;
    qint64 lastSummary = 0;
    qint64 lastInteraction = 0;
    qint64 lastScreenshot = -60000;
    quint64 swaps = 0;
    quint64 samples = 0;
    quint64 blackSamples = 0;
    quint64 composedSamples = 0;
    quint64 blackCompositions = 0;
    quint64 missingSamples = 0;
    quint64 stalledSeconds = 0;
    quint64 slowTicks = 0;
    quint64 previousFrames = 0;
    double previousPosition = 0;
    const auto record = [&](QJsonObject row) {
        evidence.write(QJsonDocument(row).toJson(QJsonDocument::Compact));
        evidence.write("\n");
        evidence.flush();
    };
    const auto saveImage = [&](const QImage& image, const QString& name) {
        QElapsedTimer work;
        work.start();
        const QString path = QDir(outputPath).filePath(name);
        if (!image.save(path)) {
            mediaError = QStringLiteral("Could not save evidence: %1").arg(path);
        }
        maxImageSaveMs = std::max(maxImageSaveMs, work.elapsed());
    };
    const auto desktopImage = [&](QScreen* screen, const QRect& region = QRect()) {
        QElapsedTimer work;
        work.start();
        QImage image = region.isEmpty()
                           ? screen->grabWindow(0).toImage()
                           : screen->grabWindow(0, region.x(), region.y(),
                                                region.width(), region.height()).toImage();
        maxDesktopGrabMs = std::max(maxDesktopGrabMs, work.elapsed());
        return image;
    };
    if (QScreen* screen = dialog->screen()) {
        record({{"event", "initialScreen"}, {"elapsedMs", clock.elapsed()},
                {"screen", screen->name()}, {"width", screen->geometry().width()},
                {"height", screen->geometry().height()}, {"dpr", screen->devicePixelRatio()}});
        QObject::connect(screen, &QScreen::geometryChanged, &app, [&](const QRect& geometry) {
            record({{"event", "screenGeometryChanged"}, {"elapsedMs", clock.elapsed()},
                    {"width", geometry.width()}, {"height", geometry.height()}});
        });
    }
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &app,
                     [&](Qt::ApplicationState state) {
        record({{"event", "applicationStateChanged"}, {"elapsedMs", clock.elapsed()},
                {"state", static_cast<int>(state)}});
    });
    QObject::connect(surface, &QOpenGLWidget::frameSwapped, &app, [&] { ++swaps; });
    bool compositionCaptureActive = false;
    QObject::connect(surface, &QOpenGLWidget::aboutToCompose, &app, [&] {
        if (compositionCaptureActive || clock.elapsed() < 5000 || !surface->hasVideo()) return;
        compositionCaptureActive = true;
        QElapsedTimer work;
        work.start();
        QOpenGLContext* previous = QOpenGLContext::currentContext();
        QSurface* previousSurface = previous != nullptr ? previous->surface() : nullptr;
        surface->makeCurrent();
        QOpenGLContext* context = QOpenGLContext::currentContext();
        if (context == surface->context()) {
            auto* gl = context->functions();
            GLint framebuffer = 0;
            GLint alignment = 0;
            gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
            gl->glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
            gl->glBindFramebuffer(GL_FRAMEBUFFER, surface->defaultFramebufferObject());
            gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
            const QSize deviceSize = surface->size() * surface->devicePixelRatioF();
            QImage image(96, 64, QImage::Format_RGBA8888);
            gl->glReadPixels(deviceSize.width() / 2 - 48, deviceSize.height() / 2 - 32,
                             96, 64, GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
            ++composedSamples;
            if (isBlack(image)) {
                ++blackCompositions;
                if (blackCompositions <= 8)
                    saveImage(image, QStringLiteral("black-composition-%1.png").arg(clock.elapsed()));
            } else if (composedSamples == 1) {
                saveImage(image, QStringLiteral("composed-video.png"));
            }
            gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
            gl->glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        }
        if (previous != nullptr && previousSurface != nullptr)
            previous->makeCurrent(previousSurface);
        else
            surface->doneCurrent();
        maxReadbackMs = std::max(maxReadbackMs, work.elapsed());
        compositionCaptureActive = false;
    }, Qt::DirectConnection);
    QTimer sample;
    sample.setTimerType(Qt::PreciseTimer);
    sample.setInterval(32);
    QObject::connect(&sample, &QTimer::timeout, &app, [&] {
        QElapsedTimer work;
        work.start();
        const qint64 now = clock.elapsed();
        const qint64 gap = previousTick == 0 ? 0 : now - previousTick;
        previousTick = now;
        if (now > 5000) {
            maxGap = std::max(maxGap, gap);
            slowTicks += gap > 250;
            if (gap > 250) {
                record({{"event", "guiGap"}, {"elapsedMs", now}, {"gapMs", gap},
                        {"previousSampleWorkMs", previousSampleWorkMs},
                        {"maxReadbackMs", maxReadbackMs}, {"maxDesktopGrabMs", maxDesktopGrabMs},
                        {"maxImageSaveMs", maxImageSaveMs}});
            }
        }
        if (now > 5000 && surface->hasVideo()) {
            const QPoint center = surface->mapToGlobal(surface->rect().center());
            QScreen* screen = dialog->screen();
            const QImage image = desktopImage(screen, QRect(center - QPoint(48, 32), QSize(96, 64)));
            ++samples;
            missingSamples += image.isNull();
            if (isBlack(image)) {
                ++blackSamples;
                if (blackSamples <= 8) {
                    saveImage(desktopImage(screen), QStringLiteral("black-%1.png").arg(now));
                    record({{"blackAtMs", now}, {"position", surface->position()}});
                }
            }
        }
        if (now - lastSummary >= 1000) {
            const auto diagnostics = surface->playbackDiagnostics();
            if (now > 10000 && surface->hasVideo() &&
                (diagnostics.renderedOutputFrames == previousFrames ||
                 surface->position() <= previousPosition)) ++stalledSeconds;
            previousFrames = diagnostics.renderedOutputFrames;
            previousPosition = surface->position();
            record({{"elapsedMs", now}, {"position", previousPosition},
                    {"frames", static_cast<qint64>(previousFrames)},
                    {"swaps", static_cast<qint64>(swaps)}, {"maxGuiGapMs", maxGap},
                    {"blackSamples", static_cast<qint64>(blackSamples)},
                    {"compositions", static_cast<qint64>(composedSamples)},
                    {"blackCompositions", static_cast<qint64>(blackCompositions)},
                    {"residentBytes", residentBytes()}, {"playing", surface->isPlaying()},
                    {"fullscreen", dialog->isFullScreen()}, {"error", mediaError}});
            lastSummary = now;
        }
        if (now > 10000 && now - lastInteraction >= 17000) {
            QTest::mouseMove(shell, QPoint(shell->width() / 2, shell->height() / 2));
            QTest::mouseMove(shell, QPoint(2, 2));
            lastInteraction = now;
        }
        if (now > 5000 && now - lastScreenshot >= 60000) {
            saveImage(desktopImage(dialog->screen()), QStringLiteral("fullscreen-%1.png").arg(now));
            lastScreenshot = now;
        }
        if (now > 15000 && !surface->hasVideo() && mediaError.isEmpty()) {
            mediaError = QStringLiteral("No video/context after startup deadline");
        }
        previousSampleWorkMs = work.elapsed();
        maxSampleWorkMs = std::max(maxSampleWorkMs, previousSampleWorkMs);
        if (now < seconds * 1000LL && mediaError.isEmpty() && !host.failed) return;
        sample.stop();
        const bool passed = mediaError.isEmpty() && !host.failed && surface->hasVideo() &&
            surface->position() >= seconds - 5 && composedSamples > 0 &&
            blackCompositions == 0 && stalledSeconds == 0 && maxGap < 1000 &&
            dialog->isFullScreen() && surface->anime4KChainId() == preset;
        record({{"result", passed ? "PASS" : "FAIL"}, {"elapsedMs", now},
                {"requestedPreset", preset}, {"appliedPreset", surface->anime4KChainId()},
                {"samples", static_cast<qint64>(samples)},
                {"blackSamples", static_cast<qint64>(blackSamples)},
                {"missingSamples", static_cast<qint64>(missingSamples)},
                {"compositions", static_cast<qint64>(composedSamples)},
                {"blackCompositions", static_cast<qint64>(blackCompositions)},
                {"desktopCaptureAvailable", samples > 0 && blackSamples < samples},
                {"stalledSeconds", static_cast<qint64>(stalledSeconds)},
                {"slowTicks", static_cast<qint64>(slowTicks)}, {"maxGuiGapMs", maxGap},
                {"maxSampleWorkMs", maxSampleWorkMs}, {"maxReadbackMs", maxReadbackMs},
                {"maxDesktopGrabMs", maxDesktopGrabMs}, {"maxImageSaveMs", maxImageSaveMs}});
        std::fprintf(stderr, "fullscreen stability: %s, elapsed=%lldms compositions=%llu black=%llu stalled=%llu maxGuiGap=%lldms\n",
                     passed ? "PASS" : "FAIL", static_cast<long long>(now),
                     static_cast<unsigned long long>(composedSamples), static_cast<unsigned long long>(blackCompositions),
                     static_cast<unsigned long long>(stalledSeconds), static_cast<long long>(maxGap));
        dialog->close();
        QTimer::singleShot(500, &app, [&app, passed] { app.exit(passed ? 0 : 1); });
    });
    sample.start();
    return app.exec();
}
