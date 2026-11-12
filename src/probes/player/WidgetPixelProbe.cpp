// Pixel evidence is captured from the production Quick player window and its video item.

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QQuickItem>
#include <QScreen>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <utility>

#include "PlayerController.hpp"
#include "PlayerWindow.hpp"
#include "ProbePicture.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSession.hpp"

namespace {

void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

struct Capture {
    QImage framebuffer;
    double position = -1.0;
    int width = 0;
    int height = 0;
};

Capture capture(mplayer::quick::PlayerController& controller) {
    Capture result;
    result.framebuffer = controller.window()->grabVideoFrame();
    result.position = controller.session()->position();
    result.width = controller.session()->videoWidth();
    result.height = controller.session()->videoHeight();
    return result;
}

QImage grabDisplayed(mplayer::quick::PlayerWindow* window, QQuickItem* videoItem) {
    if (window == nullptr || videoItem == nullptr || !window->isExposed()) return {};
    QScreen* screen = window->screen();
    if (screen == nullptr) screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) return {};
    QImage whole = screen->grabWindow(window->winId()).toImage();
    if (whole.isNull()) return {};
    const QRectF sceneRect = videoItem->mapRectToScene(videoItem->boundingRect());
    const qreal dpr = whole.devicePixelRatio() > 0.0 ? whole.devicePixelRatio() : 1.0;
    const QRect crop(qRound(sceneRect.x() * dpr), qRound(sceneRect.y() * dpr),
                     qRound(sceneRect.width() * dpr), qRound(sceneRect.height() * dpr));
    return whole.copy(crop.intersected(whole.rect()));
}

bool allUnknown(const mplayer::PlaybackDiagnostics& diagnostics) {
    return !diagnostics.estimatedVfFps.has_value() &&
           !diagnostics.decoderDropCount.has_value() &&
           !diagnostics.voDropCount.has_value() &&
           !diagnostics.nativeFilterPresent.has_value() && !diagnostics.vfCount.has_value();
}

}  // namespace

int main(int argc, char** argv) {
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setOption(QSurfaceFormat::DebugContext, true);
    QSurfaceFormat::setDefaultFormat(format);

    const QStringList arguments = [argc, argv] {
        QStringList list;
        for (int index = 1; index < argc; ++index) list.append(QString::fromLocal8Bit(argv[index]));
        return list;
    }();
    double preDpr = 0.0;
    for (int index = 0; index < arguments.size(); ++index) {
        if (arguments.at(index) == QLatin1String("--dpr") && index + 1 < arguments.size()) {
            bool ok = false;
            const double value = arguments.at(index + 1).toDouble(&ok);
            if (ok && value > 0.0) preDpr = value;
        }
    }
    if (preDpr > 0.0) {
        qputenv("QT_SCALE_FACTOR", QByteArray::number(preDpr));
        qputenv("QT_AUTO_SCREENSCALE_FACTOR", QByteArray("0"));
    }

    qputenv("MIKAI_UI_PROFILE", QByteArrayLiteral("1"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (!mplayer::probes::selectRuntime()) return 2;

    QString outPath = QStringLiteral("widget_pixel_probe.png");
    QString displayOutPath = QStringLiteral("widget_pixel_probe_display.png");
    QString fboOutPath = QStringLiteral("widget_pixel_probe_fbo.png");
    QString sourceUrl;
    QString referencePath;
    double referenceAt = 6.333;
    QSize explicitSize;
    bool fullscreen = false;
    for (int index = 0; index < arguments.size(); ++index) {
        const QString& argument = arguments.at(index);
        const auto value = [&](const char* name) -> QString {
            if (index + 1 >= arguments.size()) {
                std::fprintf(stderr, "widget_pixel_probe: %s needs a value\n", name);
                return {};
            }
            return arguments.at(++index);
        };
        if (argument == QLatin1String("--source")) {
            sourceUrl = value("--source");
        } else if (argument == QLatin1String("--reference")) {
            referencePath = value("--reference");
        } else if (argument == QLatin1String("--at")) {
            bool ok = false;
            const double parsed = value("--at").toDouble(&ok);
            if (!ok || !std::isfinite(parsed) || parsed < 0.0) {
                std::fprintf(stderr, "widget_pixel_probe: --at needs a non-negative number\n");
                return 2;
            }
            referenceAt = parsed;
        } else if (argument == QLatin1String("--output")) {
            outPath = value("--output");
        } else if (argument == QLatin1String("--display-output")) {
            displayOutPath = value("--display-output");
        } else if (argument == QLatin1String("--fbo-output")) {
            fboOutPath = value("--fbo-output");
        } else if (argument == QLatin1String("--size")) {
            const QStringList parts = value("--size").split(QLatin1Char('x'));
            bool widthOk = false;
            bool heightOk = false;
            if (parts.size() != 2) {
                std::fprintf(stderr, "widget_pixel_probe: --size needs <WxH>\n");
                return 2;
            }
            const int width = parts.at(0).toInt(&widthOk);
            const int height = parts.at(1).toInt(&heightOk);
            if (!widthOk || !heightOk || width <= 0 || height <= 0) {
                std::fprintf(stderr, "widget_pixel_probe: --size needs a positive <WxH>\n");
                return 2;
            }
            explicitSize = QSize(width, height);
        } else if (argument == QLatin1String("--dpr")) {
            if (index + 1 < arguments.size()) ++index;
        } else if (argument == QLatin1String("--fullscreen")) {
            fullscreen = true;
        } else if (!argument.startsWith(QLatin1String("--"))) {
            outPath = argument;
        }
    }
    const bool referenceMode = !sourceUrl.isEmpty() || !referencePath.isEmpty();
    if (referenceMode && (sourceUrl.isEmpty() || referencePath.isEmpty())) {
        std::fprintf(stderr,
                     "usage: widget_pixel_probe [output.png]\n"
                     "       widget_pixel_probe --source <media> --reference <png> [--at <seconds>]\n"
                     "       [--output <png>] [--display-output <png>] [--fbo-output <png>]\n"
                     "       [--size <WxH>] [--dpr <ratio>] [--fullscreen]\n");
        return 2;
    }
    QImage reference;
    if (referenceMode) {
        reference = QImage(referencePath);
        if (reference.isNull()) {
            std::fprintf(stderr, "widget_pixel_probe: the reference image could not be read: %s\n",
                         qPrintable(referencePath));
            return 2;
        }
        std::fprintf(stderr, "widget_pixel_probe: reference %s %dx%d for %s at %.6f\n",
                      qPrintable(referencePath), reference.width(), reference.height(),
                      qPrintable(sourceUrl), referenceAt);
    }

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiPixelProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("WidgetPixelProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());

    const QString source = referenceMode ? sourceUrl
                                         : QStringLiteral("av://lavfi:testsrc2=size=320x180:rate=25");
    mplayer::quick::PlayerController controller(QStringLiteral("WidgetPixelProbe"), source);
    auto* window = controller.window();
    if (explicitSize.isValid()) window->resize(explicitSize);
    else if (!fullscreen) window->resize(640, 360);
    window->setTopmostIntent(true);
    QObject::connect(window, &mplayer::quick::PlayerWindow::sceneLoadFailed, &app,
                     [&app](const QString& error) {
                         std::fprintf(stderr, "FAIL Quick player scene: %s\n", qPrintable(error));
                         app.exit(2);
                     });
    if (!fullscreen) controller.toggleFullscreen();
    else controller.showWindow();
    controller.startPlayer();
    auto* session = controller.session();
    session->setDiagnosticsPolling(true);
    session->setSpeed(1.0);
    if (referenceMode) session->setPaused(true);

    QString mediaError;
    QObject::connect(session, &mplayer::VideoSession::errorOccurred, &app,
                     [&mediaError](const QString& message) {
                         mediaError = message;
                         say("session error: %s\n", qPrintable(message));
                     });
    double reportedFps = 0.0;
    double reportedBytes = 0.0;
    auto fpsSource = mplayer::StreamFpsSource::Unknown;
    QObject::connect(session, &mplayer::VideoSession::streamStatsChanged, &app,
                     [&reportedFps, &reportedBytes, &fpsSource](double bytes, double fps,
                                                                  mplayer::StreamFpsSource source) {
                         reportedBytes = bytes;
                         reportedFps = fps;
                         fpsSource = source;
                     });

    mplayer::probes::Verdict verdict;
    QElapsedTimer clock;
    clock.start();
    QElapsedTimer stageClock;
    stageClock.start();
    QTimer tick;
    tick.setInterval(100);
    quint64 presentedFrames = 0;
    QObject::connect(window, &mplayer::quick::PlayerWindow::framePresented, &app,
                     [&presentedFrames] { ++presentedFrames; });
    auto* videoItem = window->rootObject() != nullptr
                          ? window->rootObject()->findChild<QQuickItem*>(QStringLiteral("playerVideo"))
                          : nullptr;
    enum class Stage { Sampling, CaptureReference, Display, Clear, Reopen, Done };
    Stage stage = referenceMode ? Stage::CaptureReference : Stage::Sampling;
    Capture first;
    Capture later;
    bool haveFirst = false;
    bool haveLater = false;
    bool seekRequested = false;
    bool targetAcknowledged = false;
    quint64 presentationBaseline = 0;
    double referenceFps = 24.0;
    QImage capturedScreen;
    QImage capturedFbo;
    QImage displayPrevious;
    bool proofed = false;
    mplayer::PlaybackDiagnostics liveFirst;
    mplayer::PlaybackDiagnostics afterClose;
    mplayer::PlaybackDiagnostics idleClosed;
    mplayer::PlaybackDiagnostics afterOpen;
    mplayer::PlaybackDiagnostics renewed;

    const auto finish = [&] {
        if (proofed) return;
        proofed = true;
        const QSurfaceFormat used = window->format();
        say("Quick GL surface format=%d.%d profile=%d samples=%d\n", used.majorVersion(),
            used.minorVersion(), int(used.profile()), used.samples());
        const Capture sample = haveLater ? later : capture(controller);
        say("hasVideo=%d playing=%d size=%dx%d contextReady=%d\n", int(session->hasVideo()),
            int(session->isPlaying()), sample.width, sample.height,
            int(session->isContextReady()));
        say("position=%.2f duration=%.2f\n", sample.position, session->duration());
        const auto profile = session->uiProfileCounters();
        say("ui-profile initializeGL=%llu context=%llu paints=%llu pacing-disarms=%llu last-paint-delay-ms=%lld\n",
            static_cast<unsigned long long>(profile.initializeGlCalls),
            static_cast<unsigned long long>(profile.contextGeneration),
            static_cast<unsigned long long>(profile.paintCalls),
            static_cast<unsigned long long>(profile.pacingDisarms),
            static_cast<long long>(profile.lastPaintDelayMs));
        verdict.require(profile.initializeGlCalls > 0 && profile.paintCalls > 0 &&
                            profile.lastPaintEntryNs > 0,
                        QStringLiteral("MIKAI_UI_PROFILE presentation hooks report real Quick initialization and paint counters"));
        verdict.require(profile.contextGeneration == session->renderContextGeneration(),
                        QStringLiteral("UI profiling context generation matches the active Quick renderer"));
        const auto diagnostics = session->playbackDiagnostics();
        if (diagnostics.estimatedVfFps) {
            say("estimated-vf-fps (raw): %.3f\n", *diagnostics.estimatedVfFps);
            verdict.metric(QStringLiteral("raw estimated-vf-fps %1")
                               .arg(*diagnostics.estimatedVfFps, 0, 'f', 3));
        } else {
            verdict.instrumentation(false, QStringLiteral("raw estimated-vf-fps reported by the core"));
        }
        if (diagnostics.decoderDropCount) {
            verdict.require(true, QStringLiteral("decoder-frame-drop-count read from core (%1)")
                                      .arg(qlonglong(*diagnostics.decoderDropCount)));
        } else {
            verdict.instrumentation(false, QStringLiteral("decoder-frame-drop-count reported by core"));
        }
        if (diagnostics.voDropCount) {
            verdict.require(true, QStringLiteral("frame-drop-count (VO drops) read from core (%1)")
                                      .arg(qlonglong(*diagnostics.voDropCount)));
        } else {
            verdict.require(false, QStringLiteral("frame-drop-count (VO drops) read from core (not substituted with 0)"));
        }
        if (diagnostics.nativeFilterPresent) {
            verdict.require(!*diagnostics.nativeFilterPresent,
                            QStringLiteral("native filter absent from standard core vf chain"));
        } else {
            verdict.instrumentation(false, QStringLiteral("live vf chain read from core"));
        }
        verdict.metric(QStringLiteral("reported stream stats: %1 B/s, %2 fps, source %3")
                           .arg(reportedBytes, 0, 'f', 0).arg(reportedFps, 0, 'f', 2)
                           .arg(int(fpsSource)));

        // The renderer performs this read on its owning render thread. The request is
        // bound to this exact session/context/source/playhead; the GUI never makes GL current.
        RenderDiagnosticsRequest diagnosticRequest;
        diagnosticRequest.contextGeneration = session->renderContextGeneration();
        diagnosticRequest.sourceEpoch = session->sourceEpoch();
        diagnosticRequest.position = session->position();
        RenderDiagnosticsResult renderDiagnostics;
        bool diagnosticsCompleted = false;
        QEventLoop diagnosticsLoop;
        QTimer diagnosticsTimeout;
        diagnosticsTimeout.setSingleShot(true);
        QObject::connect(&diagnosticsTimeout, &QTimer::timeout, &diagnosticsLoop,
                         &QEventLoop::quit);
        window->requestRenderDiagnostics(
            diagnosticRequest, [&](RenderDiagnosticsResult result) {
                renderDiagnostics = std::move(result);
                diagnosticsCompleted = true;
                diagnosticsLoop.quit();
            });
        diagnosticsTimeout.start(3000);
        if (!diagnosticsCompleted) diagnosticsLoop.exec(QEventLoop::ExcludeUserInputEvents);
        diagnosticsTimeout.stop();
        verdict.require(diagnosticsCompleted && renderDiagnostics.success,
                        QStringLiteral("typed Quick render diagnostics completed (%1)")
                            .arg(diagnosticsCompleted ? renderDiagnostics.error
                                                      : QStringLiteral("render-thread request timed out")));
        if (diagnosticsCompleted && renderDiagnostics.success) {
            const auto& state = renderDiagnostics.glState;
            verdict.metric(QStringLiteral("Quick GL state: stencil=%1 depth=%2 blend=%3 scissor=%4 "
                                          "dither=%5 colorMask=%6%7%8%9 unpackBuffer=%10 "
                                          "unpackAlignment=%11 unpackRowLength=%12")
                               .arg(int(state.stencilTest)).arg(int(state.depthTest))
                               .arg(int(state.blend)).arg(int(state.scissorTest))
                               .arg(int(state.dither)).arg(int(state.colorWriteMask[0]))
                               .arg(int(state.colorWriteMask[1])).arg(int(state.colorWriteMask[2]))
                               .arg(int(state.colorWriteMask[3])).arg(state.pixelUnpackBufferBinding)
                               .arg(state.unpackAlignment).arg(state.unpackRowLength));
            verdict.metric(QStringLiteral("OpenGL vendor=%1 renderer=%2")
                               .arg(renderDiagnostics.glVendor, renderDiagnostics.glRenderer));
            verdict.instrumentation(!renderDiagnostics.glVendor.isEmpty() &&
                                        !renderDiagnostics.glRenderer.isEmpty(),
                                    QStringLiteral("production Quick render context reports GL vendor and renderer"));
            verdict.require(renderDiagnostics.error.isEmpty(),
                            QStringLiteral("renderer-owned diagnostic readback/state operation reports no error"));
            verdict.require(renderDiagnostics.sourceEpoch == session->sourceEpoch() &&
                                renderDiagnostics.contextGeneration == session->renderContextGeneration(),
                            QStringLiteral("render diagnostics belong to the active Quick source/context"));
        }
        verdict.metric(QStringLiteral("renderer-owned snapshot reports only errors from its bounded readback/state operation; "
                                      "Quick exposes no continuous GL debug-message stream"));
        const QImage proofFramebuffer = diagnosticsCompleted && renderDiagnostics.success
                                            ? renderDiagnostics.framebuffer : sample.framebuffer;
        const bool saved = !proofFramebuffer.isNull() && proofFramebuffer.save(outPath);
        say("video-item framebuffer: %dx%d -> %s\n", proofFramebuffer.width(),
            proofFramebuffer.height(), saved ? qPrintable(outPath) : "FAILED");
        verdict.require(saved, QStringLiteral("proof frame written to %1").arg(outPath));
        verdict.require(mediaError.isEmpty(),
                        QStringLiteral("no playback error reported (%1)")
                            .arg(mediaError.isEmpty() ? QStringLiteral("none") : mediaError));
        if (!referenceMode) {
            verdict.require(session->isContextReady() && session->hasVisibleOutputFrame(),
                            QStringLiteral("production Quick video presenter has a visible output frame"));
            verdict.require(sample.width > 0 && sample.height > 0,
                            QStringLiteral("live source reports video dimensions (%1x%2)")
                                .arg(sample.width).arg(sample.height));
            if (videoItem != nullptr) {
                const qreal dpr = window->devicePixelRatio();
                const QSize expected(qRound(videoItem->width() * dpr),
                                     qRound(videoItem->height() * dpr));
                verdict.require(!sample.framebuffer.isNull() &&
                                    qAbs(sample.framebuffer.width() - expected.width()) <= 1 &&
                                    qAbs(sample.framebuffer.height() - expected.height()) <= 1,
                                QStringLiteral("video-item framebuffer matches its physical Quick geometry (%1x%2, expected %3x%4)")
                                    .arg(sample.framebuffer.width()).arg(sample.framebuffer.height())
                                    .arg(expected.width()).arg(expected.height()));
            } else {
                verdict.require(false, QStringLiteral("production PlayerEntry exposes the video item for geometry proof"));
            }
            verdict.require(haveFirst && haveLater && later.position > first.position,
                            QStringLiteral("live Quick playback advanced between measured frames (%1 -> %2)")
                                .arg(first.position, 0, 'f', 2).arg(later.position, 0, 'f', 2));
            const auto picture = mplayer::probes::measurePicture(sample.framebuffer,
                                                                  sample.width, sample.height);
            verdict.require(!picture.isMostlyBlack(),
                            QStringLiteral("known-bright generated source carries visible pixels (%1/%2 lit)")
                                .arg(picture.lit).arg(picture.sampled));
        } else {
            verdict.instrumentation(false, QStringLiteral("generated-source motion checks are unavailable for a parked reference source"));
        }
        liveFirst = diagnostics;
    };

    QObject::connect(&tick, &QTimer::timeout, &app, [&] {
        if (videoItem == nullptr && window->rootObject() != nullptr) {
            videoItem = window->rootObject()->findChild<QQuickItem*>(QStringLiteral("playerVideo"));
        }
        if (!mediaError.isEmpty()) {
            finish();
            if (!referenceMode) {
                tick.stop();
                app.exit(verdict.report("widget_pixel_probe"));
            }
            return;
        }
        switch (stage) {
        case Stage::Sampling: {
            if (!session->isContextReady() || !session->hasVideo()) break;
            const double position = session->position();
            if (!haveFirst && position >= 0.5) {
                first = capture(controller);
                haveFirst = !first.framebuffer.isNull();
            } else if (haveFirst && position >= first.position + 1.0) {
                later = capture(controller);
                haveLater = !later.framebuffer.isNull();
                if (!haveLater) break;
                finish();
                verdict.require(reportedFps > 0.0 && fpsSource != mplayer::StreamFpsSource::Unknown,
                                QStringLiteral("live source publishes qualified FPS before close"));
                if (liveFirst.estimatedVfFps && *liveFirst.estimatedVfFps > 0.0) {
                    verdict.require(fpsSource == mplayer::StreamFpsSource::MpvOutput,
                                    QStringLiteral("available mpv FPS observation uses output provenance"));
                }
                session->close();
                afterClose = session->playbackDiagnostics();
                verdict.require(reportedFps == 0.0 && fpsSource == mplayer::StreamFpsSource::Unknown,
                                QStringLiteral("closing live source clears displayed FPS and provenance"));
                stageClock.restart();
                stage = Stage::Display;
            } else if (!haveLater && (later.framebuffer.isNull() || position - later.position >= 0.2)) {
                later = capture(controller);
            }
            if (clock.elapsed() >= 9000 && !haveLater) {
                verdict.require(false, QStringLiteral("live Quick source produced two moving playback samples"));
                finish();
                stage = Stage::Done;
                app.exit(verdict.report("widget_pixel_probe"));
            }
            break;
        }
        case Stage::CaptureReference: {
            if (!videoItem) {
                if (clock.elapsed() < 10000) break;
                verdict.require(false, QStringLiteral("production PlayerEntry exposes playerVideo"));
                tick.stop(); app.exit(verdict.report("widget_pixel_probe")); break;
            }
            const auto fps = session->probeMpvProperty(QStringLiteral("container-fps"),
                                                       mplayer::ProbePropertyKind::Number);
            if (fps && fps->isNumeric() && fps->asNumber() > 0.0) referenceFps = fps->asNumber();
            const double frameSeconds = 1.0 / referenceFps;
            if (clock.elapsed() >= 60000) {
                verdict.require(false, QStringLiteral("real source settled at the requested reference frame"));
                tick.stop(); app.exit(verdict.report("widget_pixel_probe")); break;
            }
            if (!seekRequested) {
                if (!session->isContextReady() || !session->hasVideo() || session->duration() <= 0.0) break;
                const auto subtitle = session->probeMpvProperty(QStringLiteral("sid"),
                                                               mplayer::ProbePropertyKind::Text);
                if (!subtitle) {
                    verdict.instrumentation(false, QStringLiteral("diagnostic source reports subtitle selection"));
                    tick.stop(); app.exit(verdict.report("widget_pixel_probe")); break;
                }
                if (subtitle->text != QStringLiteral("no")) {
                    if (!session->probeCycleMpvTrack(QStringLiteral("sid"))) {
                        verdict.instrumentation(false, QStringLiteral("diagnostic source accepts subtitle cycling"));
                        tick.stop(); app.exit(verdict.report("widget_pixel_probe")); break;
                    }
                    break;
                }
                session->seek(referenceAt);
                seekRequested = true;
                stageClock.restart();
                break;
            }
            const bool onFrame = session->isContextReady() && session->hasVideo() &&
                                 session->pauseObserved() &&
                                 std::abs(session->position() - referenceAt) <= frameSeconds;
            if (!onFrame || !window->isActive()) break;
            if (!targetAcknowledged) {
                targetAcknowledged = true;
                presentationBaseline = presentedFrames;
                stageClock.restart();
                window->requestRepaint();
                break;
            }
            if (presentedFrames <= presentationBaseline || stageClock.elapsed() < 250) break;
            // Read the on-screen scene first, before asking the render item for its own FBO.
            capturedScreen = grabDisplayed(window, videoItem);
            capturedFbo = window->grabVideoFrame();
            const bool displaySaved = capturedScreen.save(displayOutPath);
            const bool fboSaved = capturedFbo.save(fboOutPath);
            verdict.require(displaySaved, QStringLiteral("on-screen proof frame written to %1").arg(displayOutPath));
            verdict.require(fboSaved, QStringLiteral("video-item framebuffer proof frame written to %1").arg(fboOutPath));
            if (capturedScreen.isNull()) {
                verdict.instrumentation(false, QStringLiteral("production Quick window pixels are available from the display"));
            } else {
                const auto screenPicture = mplayer::probes::measurePicture(capturedScreen,
                                                                            session->videoWidth(),
                                                                            session->videoHeight());
                if (!reference.isNull() && !screenPicture.rect.isEmpty()) {
                    const QImage shown = capturedScreen.copy(screenPicture.rect).convertToFormat(QImage::Format_Grayscale8);
                    const QImage expected = reference.convertToFormat(QImage::Format_Grayscale8)
                                                .scaled(shown.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    qint64 expectedLit = 0;
                    qint64 shownLit = 0;
                    for (int y = 0; y < shown.height(); ++y) {
                        for (int x = 0; x < shown.width(); ++x) {
                            if (qGray(expected.pixel(x, y)) <= mplayer::probes::kLitPixelSum) continue;
                            ++expectedLit;
                            if (qGray(shown.pixel(x, y)) > mplayer::probes::kLitPixelSum) ++shownLit;
                        }
                    }
                    if (expectedLit > 0) {
                        verdict.require(double(shownLit) / double(expectedLit) >= 0.999,
                                        QStringLiteral("displayed video shows the independently decoded reference's lit pixels (%1/%2)")
                                            .arg(shownLit).arg(expectedLit));
                    } else {
                        verdict.instrumentation(false,
                                                QStringLiteral("reference image carries lit pixels for a screen comparison"));
                    }
                }
            }
            verdict.require(!capturedFbo.isNull(), QStringLiteral("production video item's framebuffer is readable"));
            finish();
            tick.stop();
            stage = Stage::Done;
            app.exit(verdict.report("widget_pixel_probe"));
            break;
        }
        case Stage::Display: {
            if (stageClock.elapsed() < 300) break;
            const QImage now = grabDisplayed(window, videoItem);
            if (now.isNull()) {
                verdict.instrumentation(false, QStringLiteral("production Quick screen pixels are available"));
            } else if (!displayPrevious.isNull() && displayPrevious == now) {
                const bool saved = now.save(displayOutPath);
                verdict.require(saved, QStringLiteral("on-screen proof frame written to %1").arg(displayOutPath));
                const bool fboSaved = !later.framebuffer.isNull() && later.framebuffer.save(fboOutPath);
                verdict.require(fboSaved, QStringLiteral("video-item framebuffer proof frame written to %1").arg(fboOutPath));
                const auto shown = mplayer::probes::measurePicture(now, later.width, later.height);
                const auto fbo = mplayer::probes::measurePicture(later.framebuffer, later.width, later.height);
                verdict.require(!shown.isMostlyBlack(), QStringLiteral("displayed generated bright source carries real content"));
                verdict.require(shown.rect.size() == fbo.rect.size(),
                                QStringLiteral("screen and video-item framebuffer show the same picture geometry"));
                stage = Stage::Clear;
                stageClock.restart();
            } else {
                displayPrevious = now;
            }
            break;
        }
        case Stage::Clear:
            if (stageClock.elapsed() >= 800) {
                idleClosed = session->playbackDiagnostics();
                verdict.require(reportedFps == 0.0 && fpsSource == mplayer::StreamFpsSource::Unknown,
                                QStringLiteral("idle-core events cannot revive closed-source FPS"));
                session->open(QStringLiteral("av://lavfi:testsrc2=size=256x144:rate=15"));
                afterOpen = session->playbackDiagnostics();
                stageClock.restart();
                stage = Stage::Reopen;
            }
            break;
        case Stage::Reopen: {
            const auto diagnostics = session->playbackDiagnostics();
            const bool observed = diagnostics.estimatedVfFps &&
                                  *diagnostics.estimatedVfFps >= 14.5 &&
                                  *diagnostics.estimatedVfFps <= 15.5 &&
                                  session->videoWidth() == 256 && session->videoHeight() == 144 &&
                                  diagnostics.decoderDropCount && diagnostics.voDropCount &&
                                  diagnostics.nativeFilterPresent && diagnostics.vfCount;
            if (!observed && stageClock.elapsed() < 4000) break;
            renewed = diagnostics;
            verdict.require(allUnknown(afterClose), QStringLiteral("close drops all raw observations before events drain"));
            verdict.require(allUnknown(idleClosed), QStringLiteral("idle closed core late events leave raw observations unknown"));
            verdict.require(allUnknown(afterOpen), QStringLiteral("open clears prior source metrics synchronously"));
            verdict.require(renewed.vfCount && renewed.nativeFilterPresent && !*renewed.nativeFilterPresent,
                            QStringLiteral("reopened source vf chain is re-read with native filter absent"));
            if (liveFirst.estimatedVfFps) {
                verdict.require(renewed.estimatedVfFps && *renewed.estimatedVfFps >= 14.5 &&
                                    *renewed.estimatedVfFps <= 15.5 && session->videoWidth() == 256 &&
                                    session->videoHeight() == 144,
                                QStringLiteral("reopened 256x144/15fps source replaces cadence and geometry"));
            } else {
                verdict.instrumentation(false, QStringLiteral("raw estimated-vf-fps reported on first source"));
            }
            if (liveFirst.voDropCount) verdict.require(renewed.voDropCount.has_value(), QStringLiteral("reopened source re-observes VO drops"));
            else verdict.instrumentation(false, QStringLiteral("VO drops reported by core"));
            if (liveFirst.decoderDropCount) verdict.require(renewed.decoderDropCount.has_value(), QStringLiteral("reopened source re-observes decoder drops"));
            else verdict.instrumentation(false, QStringLiteral("decoder drops reported by core"));
            tick.stop();
            stage = Stage::Done;
            app.exit(verdict.report("widget_pixel_probe"));
            break;
        }
        case Stage::Done:
            break;
        }
    });
    tick.start();
    return app.exec();
}
