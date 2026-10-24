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
#include <QSet>
#include <QSettings>
#include <QStringList>
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
QString pointerId(const void* pointer) {
    return pointer == nullptr
               ? QStringLiteral("none")
               : QStringLiteral("0x%1").arg(
                     static_cast<qulonglong>(reinterpret_cast<quintptr>(pointer)), 0, 16);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    std::fprintf(stderr, "Platform: %s, screen: %s\n",
                 qPrintable(QGuiApplication::platformName()),
                 app.primaryScreen() != nullptr ? qPrintable(app.primaryScreen()->name()) : "none");
    QStringList positionalArguments;
    bool knownBright = false;
    bool steadyFullscreen = false;
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QLatin1String("--known-bright")) {
            knownBright = true;
        } else if (argument == QLatin1String("--steady-fullscreen")) {
            steadyFullscreen = true;
        } else {
            positionalArguments.append(argument);
        }
    }
    if (positionalArguments.size() < 3 || positionalArguments.size() > 4) {
        std::fprintf(stderr,
                     "Usage: fullscreen_stability_probe <video> <seconds> <output-directory> "
                     "[anime4k-preset] [--known-bright] [--steady-fullscreen]\n");
        return 2;
    }
    bool validDuration = false;
    const int seconds = positionalArguments.at(1).toInt(&validDuration);
    if (!validDuration || seconds < 5 || seconds > 1800) return 2;
    const int isolationTimeoutMs = seconds * 1000 + 60000;
    const auto isolation = mikai::probes::isolation::enter(
        argc, argv, {"fullscreen_stability_probe", false, isolationTimeoutMs});
    if (isolation != mikai::probes::isolation::Gate::RunHere)
        return mikai::probes::isolation::exitCode();
    const QString outputPath = positionalArguments.at(2);
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
    const QString preset = positionalArguments.size() == 4 ? positionalArguments.at(3)
                                                            : QStringLiteral("off");
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
    const QString media = positionalArguments.at(0);
    auto* dialog = new mplayer::shell::PlayerDialog(
        QStringLiteral("Fullscreen stability verification"), media,
        {{1080, media}}, 1080, &host);
    auto* surface = dialog->findChild<mplayer::VideoSurface*>();
    auto* shell = dialog->findChild<mplayer::shell::PlayerShell*>();
    if (surface == nullptr || shell == nullptr) return 2;
    const auto activeSurface = [&]() -> mplayer::VideoSurface* {
        if (!dialog->isPipActive()) return surface;
        for (QWidget* window : QApplication::topLevelWidgets()) {
            auto* pip = qobject_cast<mplayer::shell::PipFloat*>(window);
            if (pip != nullptr && pip->isVisible() && pip->videoSurface() != nullptr)
                return pip->videoSurface();
        }
        return nullptr;
    };
    QString mediaError;
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
    qint64 nextPresentationTransition = 30000;
    int presentationPhase = 0;
    quint64 swaps = 0;
    quint64 samples = 0;
    quint64 blackSamples = 0;
    quint64 composedSamples = 0;
    quint64 blackCompositions = 0;
    quint64 missingSamples = 0;
    quint64 missingOwnerSamples = 0;
    quint64 stalledSeconds = 0;
    quint64 slowTicks = 0;
    quint64 previousFrames = 0;
    double previousPosition = 0;
    mplayer::VideoSurface* previousOwner = nullptr;
    mplayer::VideoSurface* lastObservedOwner = nullptr;
    qint64 ownerTransitionAtMs = -1;
    qint64 activeOwnerUnavailableSinceMs = -1;
    QString lastSurfaceId;
    QString lastContextId;
    quint64 lastSourceEpoch = 0;
    quint64 ownerTransitions = 0;
    quint64 contextIdentityChanges = 0;
    quint64 sourceEpochChanges = 0;
    quint64 fullscreenViolations = 0;
    quint64 validMainSamples = 0;
    quint64 validPipSamples = 0;
    quint64 currentPipSamples = 0;
    quint64 completedPipPhases = 0;
    quint64 inactiveSamples = 0;
    qint64 contextAgeStartMs = -1;
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
    record({{"event", "runMode"},
            {"mode", steadyFullscreen ? "steady-fullscreen" : "transition"},
            {"durationSeconds", seconds}, {"knownBrightFixture", knownBright}});
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &app,
                     [&](Qt::ApplicationState state) {
        record({{"event", "applicationStateChanged"}, {"elapsedMs", clock.elapsed()},
                {"state", static_cast<int>(state)}});
    });
    QSet<mplayer::VideoSurface*> watchedSurfaces;
    bool compositionCaptureActive = false;
    const auto captureComposition = [&](mplayer::VideoSurface* composedSurface) {
        if (compositionCaptureActive || clock.elapsed() < 5000 ||
            composedSurface != activeSurface() || !composedSurface->hasVideo()) return;
        compositionCaptureActive = true;
        QElapsedTimer work;
        work.start();
        QOpenGLContext* previous = QOpenGLContext::currentContext();
        QSurface* previousSurface = previous != nullptr ? previous->surface() : nullptr;
        composedSurface->makeCurrent();
        QOpenGLContext* context = QOpenGLContext::currentContext();
        if (context == composedSurface->context()) {
            auto* gl = context->functions();
            GLint framebuffer = 0;
            GLint alignment = 0;
            gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
            gl->glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
            gl->glBindFramebuffer(GL_FRAMEBUFFER, composedSurface->defaultFramebufferObject());
            gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
            const QSize deviceSize =
                composedSurface->size() * composedSurface->devicePixelRatioF();
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
            composedSurface->doneCurrent();
        maxReadbackMs = std::max(maxReadbackMs, work.elapsed());
        compositionCaptureActive = false;
    };
    const auto watchSurface = [&](mplayer::VideoSurface* candidate) {
        if (candidate == nullptr || watchedSurfaces.contains(candidate)) return;
        watchedSurfaces.insert(candidate);
        QObject::connect(candidate, &mplayer::VideoSurface::errorOccurred, &app,
                         [&](const QString& error) { mediaError = error; });
        QObject::connect(candidate, &QOpenGLWidget::frameSwapped, &app, [&] { ++swaps; });
        QObject::connect(candidate, &QOpenGLWidget::aboutToCompose, &app,
                         [&, candidate] { captureComposition(candidate); }, Qt::DirectConnection);
        QObject::connect(candidate, &QObject::destroyed, &app,
                         [&, candidate] { watchedSurfaces.remove(candidate); });
    };
    watchSurface(surface);
    QTimer sample;
    sample.setTimerType(Qt::PreciseTimer);
    sample.setInterval(32);
    QObject::connect(&sample, &QTimer::timeout, &app, [&] {
        QElapsedTimer work;
        work.start();
        const qint64 now = clock.elapsed();
        if (!steadyFullscreen && now >= nextPresentationTransition) {
            if (presentationPhase == 0) {
                dialog->showNormal();
                presentationPhase = 1;
                nextPresentationTransition = now + 4000;
            } else if (presentationPhase == 1) {
                dialog->showFullScreen();
                presentationPhase = 2;
                nextPresentationTransition = now + 26000;
            } else if (presentationPhase == 2) {
                currentPipSamples = 0;
                dialog->togglePip();
                presentationPhase = 3;
                nextPresentationTransition = now + 5000;
            } else {
                if (currentPipSamples < 2)
                    mediaError = QStringLiteral("PiP did not produce two advancing playback samples");
                ++completedPipPhases;
                dialog->togglePip();
                dialog->showFullScreen();
                presentationPhase = 0;
                nextPresentationTransition = now + 30000;
            }
            auto* transitionedOwner = activeSurface();
            watchSurface(transitionedOwner);
            record({{"event", "presentationTransition"}, {"elapsedMs", now},
                    {"phase", presentationPhase}, {"dialogVisible", dialog->isVisible()},
                    {"fullscreen", dialog->isFullScreen()}, {"pipActive", dialog->isPipActive()},
                    {"hasVideo", transitionedOwner != nullptr && transitionedOwner->hasVideo()},
                    {"activePosition", transitionedOwner != nullptr
                                           ? transitionedOwner->position() : -1.0},
                    {"activePlaying", transitionedOwner != nullptr &&
                                          transitionedOwner->isPlaying()},
                    {"activeFrames", static_cast<qint64>(
                                         transitionedOwner != nullptr
                                             ? transitionedOwner->playbackDiagnostics()
                                                   .renderedOutputFrames : 0)}});
        }
        auto* owner = activeSurface();
        watchSurface(owner);
        if (owner != lastObservedOwner) {
            lastObservedOwner = owner;
            ownerTransitionAtMs = now;
            activeOwnerUnavailableSinceMs =
                owner != nullptr && owner->hasVideo() ? -1 : now;
        } else if (owner != nullptr && owner->hasVideo()) {
            if (activeOwnerUnavailableSinceMs >= 0) ownerTransitionAtMs = now;
            activeOwnerUnavailableSinceMs = -1;
        } else if (activeOwnerUnavailableSinceMs < 0) {
            activeOwnerUnavailableSinceMs = now;
        }
        bool foreground = dialog->isActiveWindow() &&
                          QGuiApplication::applicationState() == Qt::ApplicationActive;
#ifdef Q_OS_WIN
        foreground = foreground &&
                     GetForegroundWindow() == reinterpret_cast<HWND>(dialog->winId());
#endif
        if (steadyFullscreen && now > 5000 && !foreground) {
            ++inactiveSamples;
            QJsonObject foregroundEvent{{"event", "foregroundLost"}, {"elapsedMs", now}};
#ifdef Q_OS_WIN
            const HWND foregroundWindow = GetForegroundWindow();
            DWORD foregroundProcessId = 0;
            GetWindowThreadProcessId(foregroundWindow, &foregroundProcessId);
            QString foregroundImage = QStringLiteral("unavailable");
            if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                             foregroundProcessId)) {
                wchar_t imagePath[1024]{};
                DWORD imagePathSize = 1024;
                if (QueryFullProcessImageNameW(process, 0, imagePath, &imagePathSize))
                    foregroundImage = QString::fromWCharArray(imagePath, imagePathSize)
                                          .section(QLatin1Char('\\'), -1);
                CloseHandle(process);
            }
            foregroundEvent.insert(QStringLiteral("foregroundProcessId"),
                                   static_cast<qint64>(foregroundProcessId));
            foregroundEvent.insert(QStringLiteral("foregroundImage"), foregroundImage);
            foregroundEvent.insert(QStringLiteral("foregroundHandle"),
                                   pointerId(foregroundWindow));
#endif
            record(foregroundEvent);
            if (mediaError.isEmpty())
                mediaError = QStringLiteral("Steady fullscreen is no longer foreground");
        }
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
        if (now > 5000) {
            if (owner == nullptr || !owner->hasVideo()) {
                ++missingOwnerSamples;
            } else {
                const QPoint center = owner->mapToGlobal(owner->rect().center());
                QScreen* screen = owner->screen();
                const QImage image =
                    desktopImage(screen, QRect(center - QPoint(48, 32), QSize(96, 64)));
                ++samples;
                missingSamples += image.isNull();
                if (isBlack(image)) {
                    ++blackSamples;
                    if (blackSamples <= 8) {
                        saveImage(desktopImage(screen), QStringLiteral("black-%1.png").arg(now));
                        record({{"blackAtMs", now}, {"position", owner->position()},
                                {"pipActive", dialog->isPipActive()},
                                {"knownBrightFixture", knownBright},
                                {"applicationActive", QGuiApplication::applicationState() ==
                                                          Qt::ApplicationActive},
                                {"windowActive", dialog->isActiveWindow()},
                                {"focusException", !dialog->isActiveWindow() ||
                                                       QGuiApplication::applicationState() !=
                                                           Qt::ApplicationActive}});
                    }
                }
            }
        }
        if (now - lastSummary >= 1000) {
            const auto mainDiagnostics = surface->playbackDiagnostics();
            const auto ownerDiagnostics =
                owner != nullptr ? owner->playbackDiagnostics() : mplayer::PlaybackDiagnostics{};
            const quint64 ownerFrames = ownerDiagnostics.renderedOutputFrames;
            const double ownerPosition = owner != nullptr ? owner->position() : -1.0;
            const QString ownerId = pointerId(owner);
            const QString contextId = pointerId(owner != nullptr ? owner->context() : nullptr);
            const quint64 ownerSourceEpoch = owner != nullptr ? owner->sourceEpoch() : 0;
            if (owner != nullptr && owner->hasVideo()) {
                if (!lastSurfaceId.isEmpty() && ownerId != lastSurfaceId) ++ownerTransitions;
                lastSurfaceId = ownerId;
                if (owner->context() != nullptr) {
                    if (!lastContextId.isEmpty() && contextId != lastContextId) {
                        ++contextIdentityChanges;
                        contextAgeStartMs = now;
                    } else if (lastContextId.isEmpty()) {
                        contextAgeStartMs = now;
                    }
                    lastContextId = contextId;
                }
                if (lastSourceEpoch != 0 && ownerSourceEpoch != lastSourceEpoch)
                    ++sourceEpochChanges;
                lastSourceEpoch = ownerSourceEpoch;
            }
            if (steadyFullscreen && !dialog->isFullScreen()) ++fullscreenViolations;
            const qint64 contextAgeMs =
                contextAgeStartMs >= 0 ? now - contextAgeStartMs : -1;
            if (owner != previousOwner) {
                previousOwner = owner;
            } else if (owner != nullptr && owner->hasVideo() && owner->isPlaying() &&
                       ownerFrames > previousFrames && ownerPosition > previousPosition) {
                if (owner == surface) {
                    ++validMainSamples;
                } else {
                    ++validPipSamples;
                    ++currentPipSamples;
                }
            } else if (now > 5000 && ownerTransitionAtMs >= 0 &&
                       now - ownerTransitionAtMs > 1500 &&
                       (owner == nullptr ? dialog->hasActiveSource()
                                         : !owner->hasVideo() || !owner->isPlaying() ||
                                               ownerFrames == previousFrames ||
                                               ownerPosition <= previousPosition)) {
                ++stalledSeconds;
            }
            previousFrames = ownerFrames;
            previousPosition = ownerPosition;
            record({{"elapsedMs", now}, {"position", ownerPosition},
                    {"frames", static_cast<qint64>(ownerFrames)},
                    {"mainPosition", surface->position()},
                    {"mainFrames", static_cast<qint64>(
                                       mainDiagnostics.renderedOutputFrames)},
                    {"mainPlaying", surface->isPlaying()},
                    {"activeSurfaceFound", owner != nullptr},
                    {"activeSurfaceIsPip", owner != nullptr && owner != surface},
                    {"activePlaying", owner != nullptr && owner->isPlaying()},
                    {"activeHasVideo", owner != nullptr && owner->hasVideo()},
                    {"activePosition", ownerPosition},
                    {"activeFrames", static_cast<qint64>(ownerFrames)},
                    {"mode", steadyFullscreen ? "steady-fullscreen" : "transition"},
                    {"steadyFullscreen", steadyFullscreen},
                    {"activeSurfaceId", ownerId}, {"activeGlContextId", contextId},
                    {"activeSourceEpoch", static_cast<qint64>(ownerSourceEpoch)},
                    {"contextAgeMs", contextAgeMs},
                    {"ownerTransitions", static_cast<qint64>(ownerTransitions)},
                    {"contextIdentityChanges",
                     static_cast<qint64>(contextIdentityChanges)},
                    {"sourceEpochChanges", static_cast<qint64>(sourceEpochChanges)},
                    {"fullscreenViolations", static_cast<qint64>(fullscreenViolations)},
                    {"swaps", static_cast<qint64>(swaps)}, {"maxGuiGapMs", maxGap},
                    {"applicationActive", QGuiApplication::applicationState() ==
                                              Qt::ApplicationActive},
                    {"windowActive", dialog->isActiveWindow()},
                    {"windowFocused", dialog->isActiveWindow() && dialog->hasFocus()},
                    {"pipActive", dialog->isPipActive()},
                    {"knownBrightFixture", knownBright},
                    {"blackSamples", static_cast<qint64>(blackSamples)},
                    {"compositions", static_cast<qint64>(composedSamples)},
                    {"blackCompositions", static_cast<qint64>(blackCompositions)},
                    {"missingOwnerSamples", static_cast<qint64>(missingOwnerSamples)},
                    {"validMainSamples", static_cast<qint64>(validMainSamples)},
                    {"validPipSamples", static_cast<qint64>(validPipSamples)},
                    {"inactiveSamples", static_cast<qint64>(inactiveSamples)},
                    {"foreground", foreground},
                    {"residentBytes", residentBytes()}, {"playing", owner != nullptr &&
                                                                        owner->isPlaying()},
                    {"fullscreen", dialog->isFullScreen()}, {"error", mediaError}});
            lastSummary = now;
        }
        if (now > 10000 && now - lastInteraction >= 17000) {
            QTest::mouseMove(shell, QPoint(shell->width() / 2, shell->height() / 2));
            QTest::mouseMove(shell, QPoint(2, 2));
            lastInteraction = now;
        }
        if (now > 5000 && now - lastScreenshot >= 60000) {
            QScreen* screen = owner != nullptr ? owner->screen() : dialog->screen();
            saveImage(desktopImage(screen), QStringLiteral("fullscreen-%1.png").arg(now));
            lastScreenshot = now;
        }
        if (activeOwnerUnavailableSinceMs >= 0 &&
            now - activeOwnerUnavailableSinceMs > 15000 && mediaError.isEmpty()) {
            mediaError = QStringLiteral("No active playback surface/video after 15s wait");
        }
        previousSampleWorkMs = work.elapsed();
        maxSampleWorkMs = std::max(maxSampleWorkMs, previousSampleWorkMs);
        if (now < seconds * 1000LL && mediaError.isEmpty() && !host.failed) return;
        sample.stop();
        auto* finalOwner = activeSurface();
        const QString finalSurfaceId = pointerId(finalOwner);
        const QString finalContextId =
            pointerId(finalOwner != nullptr ? finalOwner->context() : nullptr);
        const quint64 finalSourceEpoch =
            finalOwner != nullptr ? finalOwner->sourceEpoch() : 0;
        const bool stableFullscreenOwnership =
            !steadyFullscreen ||
            (ownerTransitions == 0 && contextIdentityChanges == 0 &&
             sourceEpochChanges == 0 && fullscreenViolations == 0 &&
             (lastSurfaceId.isEmpty() || finalSurfaceId == lastSurfaceId) &&
             (lastContextId.isEmpty() || finalContextId == lastContextId) &&
             (lastSourceEpoch == 0 || finalSourceEpoch == lastSourceEpoch));
        const bool passed = mediaError.isEmpty() && !host.failed && finalOwner != nullptr &&
            finalOwner->hasVideo() && finalOwner->position() >= seconds - 5 &&
            composedSamples > 0 && (!knownBright || blackCompositions == 0) &&
            stalledSeconds == 0 && maxGap < 1000 && dialog->isFullScreen() &&
            finalOwner->anime4KChainId() == preset && stableFullscreenOwnership &&
            validMainSamples >= 2 &&
            (steadyFullscreen
                 ? foreground && inactiveSamples == 0 && samples > 0 &&
                       missingSamples == 0 && (!knownBright || blackSamples == 0)
                 : (seconds < 66 || (completedPipPhases > 0 && validPipSamples >= 2)));
        record({{"result", passed ? "PASS" : "FAIL"}, {"elapsedMs", now},
                {"requestedPreset", preset},
                {"appliedPreset", finalOwner != nullptr ? finalOwner->anime4KChainId()
                                                        : QStringLiteral("unavailable")},
                {"activeOwnerFoundAtFinish", finalOwner != nullptr},
                {"activeOwnerIsPipAtFinish", finalOwner != nullptr && finalOwner != surface},
                {"activePositionAtFinish",
                 finalOwner != nullptr ? finalOwner->position() : -1.0},
                {"mode", steadyFullscreen ? "steady-fullscreen" : "transition"},
                {"steadyFullscreen", steadyFullscreen},
                {"finalSurfaceId", finalSurfaceId},
                {"finalGlContextId", finalContextId},
                {"finalSourceEpoch", static_cast<qint64>(finalSourceEpoch)},
                {"contextAgeMs", contextAgeStartMs >= 0 ? now - contextAgeStartMs : -1},
                {"ownerTransitions", static_cast<qint64>(ownerTransitions)},
                {"contextIdentityChanges",
                 static_cast<qint64>(contextIdentityChanges)},
                {"sourceEpochChanges", static_cast<qint64>(sourceEpochChanges)},
                {"fullscreenViolations", static_cast<qint64>(fullscreenViolations)},
                {"stableFullscreenOwnership", stableFullscreenOwnership},
                {"knownBrightFixture", knownBright},
                {"blackFramesEvaluated", knownBright},
                {"applicationActiveAtFinish", QGuiApplication::applicationState() ==
                                                  Qt::ApplicationActive},
                {"windowActiveAtFinish", dialog->isActiveWindow()},
                {"windowFocusedAtFinish", dialog->isActiveWindow() && dialog->hasFocus()},
                {"samples", static_cast<qint64>(samples)},
                {"blackSamples", static_cast<qint64>(blackSamples)},
                {"missingSamples", static_cast<qint64>(missingSamples)},
                {"missingOwnerSamples", static_cast<qint64>(missingOwnerSamples)},
                {"validMainSamples", static_cast<qint64>(validMainSamples)},
                {"validPipSamples", static_cast<qint64>(validPipSamples)},
                {"completedPipPhases", static_cast<qint64>(completedPipPhases)},
                {"inactiveSamples", static_cast<qint64>(inactiveSamples)},
                {"foregroundAtFinish", foreground},
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
