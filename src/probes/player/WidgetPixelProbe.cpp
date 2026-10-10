// The saved PNG is the measured surface framebuffer, never a widget grab.

#include <QApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLDebugLogger>
#include <QOpenGLFunctions>
#include <QScreen>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWindow>

#include <cstdarg>
#include <cstdio>

#include "ProbePicture.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSurface.hpp"

namespace {

void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

const char* glErrorName(GLenum code) {
    switch (code) {
        case 0x0500: return "GL_INVALID_ENUM";
        case 0x0501: return "GL_INVALID_VALUE";
        case 0x0502: return "GL_INVALID_OPERATION";
        case 0x0503: return "GL_STACK_OVERFLOW";
        case 0x0504: return "GL_STACK_UNDERFLOW";
        case 0x0505: return "GL_OUT_OF_MEMORY";
        case 0x0506: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        default: return "GL_UNKNOWN_ERROR";
    }
}

/// The pixels the display actually shows for a widget, read from the screen
/// rather than from the surface. grabFramebuffer() re-renders the whole widget
/// through paintGL before reading it, so it cannot see a composite that only
/// repainted part of the surface - which is exactly how a stale video rectangle
/// survives while every framebuffer readback reports a clean frame.
QImage grabDisplayed(QWidget* widget) {
    if (widget == nullptr) return QImage();
    QWindow* window = widget->window()->windowHandle();
    if (window == nullptr) return QImage();
    QScreen* screen = window->screen();
    if (screen == nullptr) screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) return QImage();
    QPoint origin = widget->mapToGlobal(QPoint(0, 0));
#ifdef Q_OS_WIN
    origin -= screen->geometry().topLeft();
#endif
    return screen->grabWindow(0, origin.x(), origin.y(), widget->width(), widget->height())
        .toImage();
}

class DebugSurface : public mplayer::VideoSurface {
public:
    bool loggerAttached() const { return logger_ != nullptr && logger_->isLogging(); }
    int messagesSeen() const { return messages_; }
    int errorMessagesSeen() const { return errorMessages_; }

protected:
    void initializeGL() override {
        auto* logger = new QOpenGLDebugLogger(this);
        if (logger->initialize()) {
            logger->enableMessages();
            logger->startLogging();
            QObject::connect(logger, &QOpenGLDebugLogger::messageLogged, this,
                             [this](const QOpenGLDebugMessage& message) {
                                 ++messages_;
                                 if (message.type() == QOpenGLDebugMessage::ErrorType) {
                                     ++errorMessages_;
                                 }
                                 say("GL debug: severity=%d source=%d type=0x%04x id=%d %s\n",
                                     int(message.severity()), int(message.source()),
                                     unsigned(message.type()), unsigned(message.id()),
                                     message.message().toUtf8().constData());
                             });
            logger_ = logger;
        } else {
            say("GL debug logger initialize() failed: this context cannot report\n");
            delete logger;
        }
        mplayer::VideoSurface::initializeGL();
    }

private:
    QOpenGLDebugLogger* logger_ = nullptr;
    int messages_ = 0;
    int errorMessages_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setOption(QSurfaceFormat::DebugContext, true);
    QSurfaceFormat::setDefaultFormat(format);

    // The scale factor must be set before QApplication takes the screen's DPR:
    // on a 4K display with 200% scaling the framebuffer is 2x the widget's
    // logical size, and the corruption was reported at that physical geometry.
    const QStringList arguments = [argc, argv] {
        QStringList list;
        for (int index = 1; index < argc; ++index) list.append(QString::fromLocal8Bit(argv[index]));
        return list;
    }();
    double preDpr = 0.0;
    for (int index = 0; index < arguments.size(); ++index) {
        if (arguments.at(index) == QLatin1String("--dpr")) {
            if (index + 1 < arguments.size()) {
                bool ok = false;
                const double d = arguments.at(index + 1).toDouble(&ok);
                if (ok && d > 0.0) preDpr = d;
            }
        }
    }
    if (preDpr > 0.0) {
        qputenv("QT_SCALE_FACTOR", QByteArray::number(preDpr));
        qputenv("QT_AUTO_SCREENSCALE_FACTOR", QByteArray("0"));
    }

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }
    // Two contracts: the generated lavfi source's own checks, or a real file
    // judged against an independently decoded reference PNG of one frame.
    //
    //   widget_pixel_probe [output.png]
    //   widget_pixel_probe --source <media> --reference <png> [--at <seconds>]
    //       [--output <png>] [--display-output <png>] [--fbo-output <png>]
    //       [--size <WxH>] [--dpr <ratio>] [--fullscreen]
    //
    // --at defaults to 6.333. In the reference contract the surface is parked on
    // that frame and the screen pixels are read BEFORE any framebuffer readback,
    // because grabFramebuffer() repaints through paintGL first and would repair
    // the very composite being judged.
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
                return QString();
            }
            return arguments.at(++index);
        };
        if (argument == QLatin1String("--source")) {
            sourceUrl = value("--source");
        } else if (argument == QLatin1String("--reference")) {
            referencePath = value("--reference");
        } else if (argument == QLatin1String("--at")) {
            const QString text = value("--at");
            bool numeric = false;
            const double parsed = text.toDouble(&numeric);
            if (!numeric || !std::isfinite(parsed) || parsed < 0.0) {
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
            const int w = parts.at(0).toInt(&widthOk);
            const int h = parts.at(1).toInt(&heightOk);
            if (!widthOk || !heightOk || w <= 0 || h <= 0) {
                std::fprintf(stderr, "widget_pixel_probe: --size needs a positive <WxH>\n");
                return 2;
            }
            explicitSize = QSize(w, h);
        } else if (argument == QLatin1String("--dpr")) {
            // Validated and applied before QApplication via QT_SCALE_FACTOR;
            // nothing to do here beyond parsing past it.
        } else if (argument == QLatin1String("--fullscreen")) {
            fullscreen = true;
        } else if (!argument.startsWith(QLatin1String("--"))) {
            // The historical positional form: the output PNG alone.
            outPath = argument;
        }
    }
    const bool referenceMode = !sourceUrl.isEmpty() || !referencePath.isEmpty();
    if (referenceMode && (sourceUrl.isEmpty() || referencePath.isEmpty())) {
        std::fprintf(stderr,
                     "usage: widget_pixel_probe [output.png]\n"
                     "       widget_pixel_probe --source <media> --reference <png> "
                     "[--at <seconds>] [--output <png>] [--display-output <png>] "
                     "[--fbo-output <png>] [--size <WxH>] [--dpr <ratio>] [--fullscreen]\n");
        return 2;
    }

    QImage referenceImage;
    if (referenceMode) {
        referenceImage = QImage(referencePath);
        if (referenceImage.isNull()) {
            std::fprintf(stderr, "widget_pixel_probe: the reference image could not be read: %s\n",
                         qPrintable(referencePath));
            return 2;
        }
        std::fprintf(stderr,
                     "widget_pixel_probe: reference %s %dx%d for %s at %.6f\n",
                     qPrintable(referencePath), referenceImage.width(), referenceImage.height(),
                     qPrintable(sourceUrl), referenceAt);
    }

    DebugSurface surface;
    quint64 presentedFrames = 0;
    QObject::connect(&surface, &QOpenGLWidget::frameSwapped, &app,
                     [&] { ++presentedFrames; });
    // The real file is decoded at its own size into the surface's own
    // framebuffer, exactly as the player does it; nothing here forces a
    // geometry, and a mismatch is reported rather than rescaled away.
    if (referenceMode) {
        // The geometry the artifact was reported at, reproduced rather than
        // assumed: a device-pixel ratio and a physical size change the
        // framebuffer Qt allocates, and the earlier failing run measured a
        // different one from the passing run. --dpr is applied via the
        // QT_SCALE_FACTOR environment variable before QApplication creation,
        // because QOpenGLWidget in Qt 6 has no per-widget DPR setter.
        surface.setWindowFlag(Qt::WindowStaysOnTopHint, true);
        if (explicitSize.isValid()) surface.resize(explicitSize);
        if (fullscreen) {
            surface.showFullScreen();
        } else {
            surface.show();
        }
        surface.open(sourceUrl);
    } else {
        surface.resize(320, 180);
        surface.show();
        surface.open(QStringLiteral("av://lavfi:testsrc2=size=320x180:rate=25"));
    }
    surface.setSpeed(1.0);
    if (referenceMode) {
        surface.setPaused(true);
        surface.raise();
        surface.activateWindow();
    }

    QString mediaError;
    QObject::connect(&surface, &mplayer::VideoSurface::errorOccurred, &app,
                     [&mediaError](const QString& message) {
                         mediaError = message;
                         say("surface error: %s\n", qPrintable(message));
                     });

    double reportedFps = 0.0;
    double reportedBytes = 0.0;
    auto reportedFpsSource = mplayer::StreamFpsSource::Unknown;
    QObject::connect(&surface, &mplayer::VideoSurface::streamStatsChanged, &app,
                     [&reportedFps, &reportedBytes, &reportedFpsSource](
                         double bytesPerSecond, double framesPerSecond,
                         mplayer::StreamFpsSource source) {
                         reportedBytes = bytesPerSecond;
                         reportedFps = framesPerSecond;
                         reportedFpsSource = source;
                     });

    QTimer* pump = new QTimer(nullptr);
    QObject::connect(pump, &QTimer::timeout, &surface, [&surface] { surface.update(); });
    pump->start(16);

    mplayer::probes::Verdict verdict;
    mplayer::probes::PlaybackSampler sampler(surface);
    QElapsedTimer clock;
    clock.start();

    enum class Stage { Sampling, Capture, Display, Clearing, Observing, Done };
    Stage stage = Stage::Sampling;
    bool proofed = false;
    QElapsedTimer stageClock;
    stageClock.start();
    bool displayChecked = false;
    QImage displayPrevious;
    // Cadence of the reference frame's source, used only to bound how close the
    // parked core must be: one source frame, never looser.
    double referenceFps = 24.0;
    // The two deliberately separate observations, in capture order: the screen
    // composite as the surface left it, then the framebuffer readback.
    QImage capturedScreen;
    QImage capturedFbo;
    bool capturesTaken = false;
    bool referenceSeekRequested = false;
    QString lastSubtitleSelection;
    bool targetFrameAcknowledged = false;
    quint64 targetPresentationBaseline = 0;
    mplayer::PlaybackDiagnostics liveFirst;
    mplayer::PlaybackDiagnostics afterClose;
    mplayer::PlaybackDiagnostics idleClosed;
    mplayer::PlaybackDiagnostics afterOpen;
    mplayer::PlaybackDiagnostics renewed;
    const auto allUnknown = [](const mplayer::PlaybackDiagnostics& diagnostics) {
        return !diagnostics.estimatedVfFps.has_value() &&
               !diagnostics.decoderDropCount.has_value() &&
               !diagnostics.voDropCount.has_value() &&
               !diagnostics.nativeFilterPresent.has_value() &&
               !diagnostics.vfCount.has_value();
    };

    QTimer tick;
    tick.setInterval(100);
    const auto finish = [&] {
        if (proofed) return;
        proofed = true;
        // Deliberately does not stop the timers or exit: the lifecycle below
        // needs the same event loop.

        const QSurfaceFormat used = surface.format();
        say("GL surface format=%d.%d profile=%d samples=%d\n", used.majorVersion(),
            used.minorVersion(), int(used.profile()), used.samples());
        const mplayer::probes::PlaybackSample& sample = sampler.later();
        say("hasVideo=%d playing=%d size=%dx%d contextReady=%d\n", int(sample.hasVideo),
            int(surface.isPlaying()), sample.videoWidth, sample.videoHeight,
            int(sample.contextReady));
        say("position=%.2f duration=%.2f\n", sample.position, surface.duration());
        const mplayer::PlaybackDiagnostics diagnostics = surface.playbackDiagnostics();
        if (diagnostics.estimatedVfFps.has_value()) {
            say("estimated-vf-fps (raw): %.3f\n", *diagnostics.estimatedVfFps);
            verdict.metric(QStringLiteral("raw estimated-vf-fps %1")
                               .arg(*diagnostics.estimatedVfFps, 0, 'f', 3));
        } else {
            say("estimated-vf-fps (raw): unavailable (the core did not report it on this "
                "source)\n");
            verdict.instrumentation(false,
                                    QStringLiteral("raw estimated-vf-fps reported by the core"));
        }
        if (diagnostics.decoderDropCount.has_value()) {
            say("decoder-frame-drop-count: %lld\n",
                static_cast<long long>(*diagnostics.decoderDropCount));
            verdict.require(true,
                            QStringLiteral("decoder-frame-drop-count read from the core (%1)")
                                .arg(static_cast<qlonglong>(*diagnostics.decoderDropCount)));
        } else {
            say("decoder-frame-drop-count: unavailable (the core did not report it)\n");
            verdict.instrumentation(false,
                                    QStringLiteral("decoder-frame-drop-count reported by the "
                                                   "core (a core limit, never a measured zero)"));
        }
        if (diagnostics.voDropCount.has_value()) {
            say("frame-drop-count (VO drops): %lld\n",
                static_cast<long long>(*diagnostics.voDropCount));
            verdict.require(true,
                            QStringLiteral("frame-drop-count (VO drops) read from the core (%1)")
                                .arg(static_cast<qlonglong>(*diagnostics.voDropCount)));
        } else {
            say("frame-drop-count (VO drops): unavailable (the core did not report it)\n");
            verdict.require(false,
                            QStringLiteral("frame-drop-count (VO drops) read from the core (not "
                                           "substituted with 0)"));
        }
        if (diagnostics.nativeFilterPresent.has_value()) {
            const QString chain = diagnostics.vfCount.has_value()
                                      ? QStringLiteral("%1 entries").arg(*diagnostics.vfCount)
                                      : QStringLiteral("entry count unavailable");
            say("native filter in the live vf chain (%s): %s\n", qPrintable(chain),
                *diagnostics.nativeFilterPresent ? "present" : "absent");
            if (*diagnostics.nativeFilterPresent) {
                verdict.require(false,
                                QStringLiteral("native filter present in the standard core's vf "
                                               "chain"));
            } else {
                verdict.require(true,
                                QStringLiteral("native filter absent from the standard core's vf "
                                               "chain (read from the core)"));
            }
        } else {
            say("native filter in the live vf chain: unknown (the chain was not read)\n");
            verdict.instrumentation(false,
                                    QStringLiteral("the live vf chain read from the core (native "
                                                   "absence provable)"));
        }
        verdict.metric(QStringLiteral("raw diagnostics are sampled asynchronously at a 500 ms "
                                      "cadence; a named-unavailable counter is a core limit, "
                                      "never a measured zero"));

        const char* fpsProvenance =
            reportedFpsSource == mplayer::StreamFpsSource::MpvOutput
                ? "mpv output"
                : reportedFpsSource == mplayer::StreamFpsSource::Container
                      ? "container source"
                      : "unknown";
        say("reported stream stats: %.0f B/s, %.2f fps (%s)\n",
            reportedBytes, reportedFps, fpsProvenance);

        say("native status reader: polling=%s; filter presence is proven through the core's own "
            "vf chain, not through that reader\n",
            surface.nativeStatusPolling() ? "on" : "off");

        verdict.instrumentation(
            surface.loggerAttached(),
            QStringLiteral("GL debug logger attached and logging on the live context"));
        if (surface.loggerAttached()) {
            say("GL debug messages seen: %d (ErrorType %d)\n", surface.messagesSeen(),
                surface.errorMessagesSeen());
            verdict.require(surface.errorMessagesSeen() == 0,
                            QStringLiteral("no GL debug ErrorType message (%1 seen)")
                                .arg(surface.errorMessagesSeen()));
        } else {
            say("GL debug logging UNAVAILABLE: no GL message from this run proves anything\n");
        }

        int errorCount = 0;
        bool swept = false;
        surface.makeCurrent();
        if (surface.context() != nullptr && QOpenGLContext::currentContext() == surface.context()) {
            QOpenGLFunctions functions;
            functions.initializeOpenGLFunctions();
            for (int guard = 0; guard < 32; ++guard) {
                const GLenum code = functions.glGetError();
                if (code == GL_NO_ERROR) break;
                ++errorCount;
                say("glGetError: 0x%04x %s\n", code, glErrorName(code));
            }
            swept = true;
            surface.doneCurrent();
        }
        if (swept) {
            verdict.require(errorCount == 0,
                            QStringLiteral("glGetError queue is empty after playback"));
        } else {
            verdict.instrumentation(false,
                                    QStringLiteral("glGetError sweep ran on the live context"));
        }

        const bool saved = sample.hasPicture() && sample.framebuffer.save(outPath);
        say("framebuffer: %dx%d\n", sample.framebuffer.width(), sample.framebuffer.height());
        say("screenshot: %s -> %s\n", outPath.toUtf8().constData(),
            saved ? "written" : "FAILED");
        verdict.require(saved, QStringLiteral("proof frame written to %1").arg(outPath));

        verdict.require(mediaError.isEmpty(),
                        QStringLiteral("no playback error reported (%1)")
                            .arg(mediaError.isEmpty() ? QStringLiteral("none") : mediaError));

        if (referenceMode) {
            // A paused real file is not moving playback: the cadence, motion and
            // position-advance requirements describe the generated source and are
            // reported as unavailable for this run rather than satisfied by a
            // parked video.
            verdict.instrumentation(
                false,
                QStringLiteral("generated-source playback motion checks are unavailable for a "
                               "parked real source (they describe the lavfi source)"));
        } else {
            mplayer::probes::requirePlayback(verdict, sampler.earlier(), sampler.later());
        }

        if (swept) {
            say("GL errors read back by this probe: %d\n", errorCount);
        } else {
            say("GL errors read back by this probe: unavailable (the context was not current)\n");
        }
        liveFirst = diagnostics;
    };

    QObject::connect(&tick, &QTimer::timeout, &surface, [&] {
        switch (stage) {
            case Stage::Sampling: {
                // Reference mode takes its screen observation FIRST, before any
                // grabFramebuffer has ever run on this surface: grabFramebuffer()
                // repaints through paintGL before it reads, so a screen grab taken
                // after it observes a composite the probe itself repaired. The
                // 16 ms pump timer and the sampler are stopped here so nothing
                // repaints the surface between the grab and the readback.
                if (referenceMode) {
                    // Read the cadence the core itself reports for this source, so
                    // the one-frame settle bound is the source's own rather than a
                    // guess.
                    const auto fps =
                        surface.probeMpvProperty(QStringLiteral("container-fps"),
                                                 mplayer::ProbePropertyKind::Number);
                    if (fps.has_value() && fps->isNumeric() && fps->asNumber() > 0.0) {
                        referenceFps = fps->asNumber();
                    }
                    pump->stop();
                    stageClock.restart();
                    stage = Stage::Capture;
                    break;
                }
                const bool paired = sampler.tick();
                if (!mediaError.isEmpty() || (paired && clock.elapsed() >= 2000) ||
                    clock.elapsed() >= 9000) {
                    finish();
                    if (!mediaError.isEmpty()) {
                        // The first source never played; the lifecycle would only
                        // measure the failure again.
                        tick.stop();
                        pump->stop();
                        stage = Stage::Done;
                        app.exit(verdict.report("widget_pixel_probe"));
                        break;
                    }
                    verdict.require(
                        reportedFps > 0.0 &&
                            reportedFpsSource != mplayer::StreamFpsSource::Unknown,
                        QStringLiteral("a live source publishes qualified FPS before it is closed"));
                    if (liveFirst.estimatedVfFps.has_value() && *liveFirst.estimatedVfFps > 0.0) {
                        verdict.require(
                            reportedFpsSource == mplayer::StreamFpsSource::MpvOutput,
                            QStringLiteral("an available mpv FPS observation uses output provenance"));
                    }
                    surface.close();
                    afterClose = surface.playbackDiagnostics();
                    verdict.require(
                        reportedFps == 0.0 &&
                            reportedFpsSource == mplayer::StreamFpsSource::Unknown,
                        QStringLiteral("closing a live source clears its displayed FPS and provenance"));
                    stageClock.restart();
                    stage = Stage::Display;
                }
                break;
            }
            case Stage::Capture: {
                // Wait until the parked core is on the frame the reference was
                // decoded at, within one source frame, and the surface has drawn
                // it. The tolerance is one frame of the source cadence, never
                // looser: a frame further away is a different picture, and
                // comparing it would hide the artifact instead of catching it.
                const double frameSeconds = surface.videoHeight() > 0 && referenceFps > 0.0
                                                ? 1.0 / referenceFps
                                                : 1.0 / 24.0;
                if (clock.elapsed() >= 60000) {
                    std::fprintf(
                        stderr,
                        "FAIL real source did not settle at %.6f within %.6f s: position=%s "
                        "paused=%d hasVideo=%d %dx%d\n",
                        referenceAt, frameSeconds,
                        surface.hasVideo() ? qPrintable(QString::number(surface.position(), 'f', 6))
                                           : "(no video)",
                        int(surface.pauseObserved()), int(surface.hasVideo()),
                        surface.videoWidth(), surface.videoHeight());
                    verdict.require(
                        false,
                        QStringLiteral("the real source settled on the reference frame "
                                       "(expected %1 s, observed %2 s, paused=%3, %4x%5)")
                            .arg(referenceAt, 0, 'f', 6)
                            .arg(surface.position(), 0, 'f', 6)
                            .arg(int(surface.pauseObserved()))
                            .arg(surface.videoWidth())
                            .arg(surface.videoHeight()));
                    tick.stop();
                    pump->stop();
                    stage = Stage::Done;
                    app.exit(verdict.report("widget_pixel_probe"));
                    break;
                }
                if (!referenceSeekRequested) {
                    if (!surface.isContextReady() || !surface.hasVideo() ||
                        surface.duration() <= 0.0) {
                        surface.update();
                        break;
                    }
                    const auto subtitle = surface.probeMpvProperty(
                        QStringLiteral("sid"), mplayer::ProbePropertyKind::Text);
                    if (!subtitle.has_value()) {
                        verdict.instrumentation(false,
                            QStringLiteral("the diagnostic source reports its subtitle selection"));
                        tick.stop();
                        pump->stop();
                        stage = Stage::Done;
                        app.exit(verdict.report("widget_pixel_probe"));
                        break;
                    }
                    if (subtitle->text != QStringLiteral("no")) {
                        if (subtitle->text != lastSubtitleSelection) {
                            lastSubtitleSelection = subtitle->text;
                            if (!surface.probeCycleMpvTrack(QStringLiteral("sid"))) {
                                verdict.instrumentation(false,
                                    QStringLiteral("the diagnostic source accepts subtitle cycling"));
                                tick.stop();
                                pump->stop();
                                stage = Stage::Done;
                                app.exit(verdict.report("widget_pixel_probe"));
                                break;
                            }
                        }
                        surface.update();
                        break;
                    }
                    // Raw decode references exclude subtitle compositing.
                    surface.seek(referenceAt);
                    referenceSeekRequested = true;
                    stageClock.restart();
                    surface.update();
                    break;
                }
                const bool onFrame =
                    surface.isContextReady() && surface.hasVideo() && surface.pauseObserved() &&
                    std::abs(surface.position() - referenceAt) <= frameSeconds;
                if (!onFrame || !surface.isActiveWindow()) {
                    surface.update();
                    break;
                }
                if (!targetFrameAcknowledged) {
                    targetFrameAcknowledged = true;
                    targetPresentationBaseline = presentedFrames;
                    stageClock.restart();
                    surface.update();
                    break;
                }
                if (presentedFrames <= targetPresentationBaseline || stageClock.elapsed() < 250) {
                    surface.update();
                    break;
                }
                // SCREEN FIRST. This is the only observation of the composite the
                // surface actually left behind: nothing has repainted the widget
                // since it settled, because the pump and the sampler are stopped.
                capturedScreen = grabDisplayed(&surface);
                capturesTaken = true;
                const qreal dpr = surface.devicePixelRatioF();
                const QSize expectedPixels(qRound(surface.width() * dpr),
                                           qRound(surface.height() * dpr));
                if (capturedScreen.isNull() || capturedScreen.size() != expectedPixels) {
                    say("screen: unavailable (logical %dx%d, dpr %.2f, expected %dx%d, captured %dx%d)\n",
                        surface.width(), surface.height(), double(dpr),
                        expectedPixels.width(), expectedPixels.height(),
                        capturedScreen.width(), capturedScreen.height());
                    verdict.instrumentation(
                        false, QStringLiteral("the surface's on-screen pixels could be read "
                                              "(no display available)"));
                    tick.stop();
                    stage = Stage::Done;
                    app.exit(verdict.report("widget_pixel_probe"));
                    break;
                }
                say("screen: %dx%d captured at position %.6f (reference %.6f), logical %dx%d dpr %.2f\n",
                    capturedScreen.width(), capturedScreen.height(), surface.position(),
                    referenceAt, surface.width(), surface.height(), double(dpr));
                // FRAMEBUFFER SECOND, and only now: it repaints the whole widget
                // through paintGL before it reads, so it is a deliberately
                // separate observation of a deliberately different state.
                const mplayer::probes::Picture shown =
                    mplayer::probes::measurePicture(capturedScreen, surface.videoWidth(),
                                                    surface.videoHeight());
                capturedFbo = surface.grabFramebuffer();
                say("framebuffer: %dx%d read after the screen grab\n", capturedFbo.width(),
                    capturedFbo.height());
                const bool screenSaved = capturedScreen.save(displayOutPath);
                say("screen: %s -> %s\n", displayOutPath.toUtf8().constData(),
                    screenSaved ? "written" : "FAILED");
                verdict.require(screenSaved, QStringLiteral("on-screen proof frame written to %1")
                                                  .arg(displayOutPath));
                const bool fboSaved = !capturedFbo.isNull() && capturedFbo.save(fboOutPath);
                say("framebuffer: %s -> %s\n", fboOutPath.toUtf8().constData(),
                    fboSaved ? "written" : "FAILED");
                verdict.require(fboSaved, QStringLiteral("framebuffer proof frame written to %1")
                                               .arg(fboOutPath));

                // Geometry agreement: the picture band the screen shows must be
                // the band the framebuffer holds, at the same place and size. A
                // surface whose composite kept a stale rectangle reports a
                // different band, and that is what a viewer sees as a black
                // block beside a live picture.
                if (!capturedFbo.isNull() && capturedFbo.size() == capturedScreen.size()) {
                    const mplayer::probes::Picture inFramebuffer =
                        mplayer::probes::measurePicture(capturedFbo, surface.videoWidth(),
                                                        surface.videoHeight());
                    verdict.metric(
                        QStringLiteral("screen band %1x%2+%3,%4: lit=%5/%6 (%7%)")
                            .arg(shown.rect.width())
                            .arg(shown.rect.height())
                            .arg(shown.rect.x())
                            .arg(shown.rect.y())
                            .arg(shown.lit)
                            .arg(shown.sampled)
                            .arg(100.0 * shown.litFraction(), 0, 'f', 1));
                    verdict.metric(QStringLiteral("framebuffer band %1x%2+%3,%4: lit=%5/%6 (%7%)")
                                       .arg(inFramebuffer.rect.width())
                                       .arg(inFramebuffer.rect.height())
                                       .arg(inFramebuffer.rect.x())
                                       .arg(inFramebuffer.rect.y())
                                       .arg(inFramebuffer.lit)
                                       .arg(inFramebuffer.sampled)
                                       .arg(100.0 * inFramebuffer.litFraction(), 0, 'f', 1));
                    verdict.require(
                        shown.rect == inFramebuffer.rect,
                        QStringLiteral("the screen shows the same picture band as the framebuffer "
                                       "(screen %1x%2+%3,%4, framebuffer %5x%6+%7,%8)")
                            .arg(shown.rect.width())
                            .arg(shown.rect.height())
                            .arg(shown.rect.x())
                            .arg(shown.rect.y())
                            .arg(inFramebuffer.rect.width())
                            .arg(inFramebuffer.rect.height())
                            .arg(inFramebuffer.rect.x())
                            .arg(inFramebuffer.rect.y()));
                } else {
                    verdict.instrumentation(
                        false,
                        QStringLiteral("screen and framebuffer share one geometry (%1x%2 vs %3x%4)")
                            .arg(capturedScreen.width())
                            .arg(capturedScreen.height())
                            .arg(capturedFbo.width())
                            .arg(capturedFbo.height()));
                }
                verdict.require(!shown.isMostlyBlack(),
                                QStringLiteral("the displayed picture carries real content "
                                               "(lit %1 of %2 sampled)")
                                    .arg(shown.lit)
                                    .arg(shown.sampled));

                // The oracle: the display must carry, in every spatial region,
                // the lit pixels the independently decoded reference carries
                // there. The reference is a separate decode of the same frame, so
                // a black block on a lit reference names the render path. The
                // measure is per-region lit fraction of pixels that are lit in
                // the reference - a lit pixel the surface fails to show - which a
                // row-mean difference cannot catch, because one bright row beside
                // a black block averages out.
                const QImage shownBand = capturedScreen.copy(shown.rect).convertToFormat(
                    QImage::Format_Grayscale8);
                const QImage expected =
                    referenceImage.convertToFormat(QImage::Format_Grayscale8)
                        .scaled(shownBand.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                constexpr int kRegionRows = 12;
                const int regionHeight =
                    shownBand.height() > 0 ? (shownBand.height() + kRegionRows - 1) / kRegionRows : 0;
                if (regionHeight > 0) {
                    qint64 referenceLitTotal = 0;
                    qint64 displayedLitTotal = 0;
                    qint64 worstMissing = 0;
                    int worstRegion = -1;
                    for (int region = 0; region < kRegionRows; ++region) {
                        const int top = region * regionHeight;
                        if (top >= shownBand.height()) break;
                        const int bottom = std::min(shownBand.height(), top + regionHeight);
                        qint64 referenceLit = 0;
                        qint64 displayedLit = 0;
                        for (int y = top; y < bottom; ++y) {
                            for (int x = 0; x < shownBand.width(); ++x) {
                                // A pixel the reference shows as lit is one this
                                // surface must also show; black pixels in the
                                // reference are never counted, so a dark region
                                // of the source cannot fail a bright one.
                                const int expectedLuma = qGray(expected.pixel(x, y));
                                if (expectedLuma <= mplayer::probes::kLitPixelSum) continue;
                                ++referenceLit;
                                if (qGray(shownBand.pixel(x, y)) >
                                    mplayer::probes::kLitPixelSum) {
                                    ++displayedLit;
                                }
                            }
                        }
                        referenceLitTotal += referenceLit;
                        displayedLitTotal += displayedLit;
                        const qint64 missing = referenceLit - displayedLit;
                        if (missing > worstMissing) {
                            worstMissing = missing;
                            worstRegion = region;
                        }
                    }
                    if (referenceLitTotal > 0) {
                        const double covered =
                            double(displayedLitTotal) / double(referenceLitTotal);
                        say("reference: %lld reference-lit pixels, %lld shown (%.1f%%), worst "
                            "region %d missing %lld\n",
                            static_cast<long long>(referenceLitTotal),
                            static_cast<long long>(displayedLitTotal), 100.0 * covered,
                            worstRegion, static_cast<long long>(worstMissing));
                        verdict.metric(
                            QStringLiteral("displayed band covers %1% of the reference's lit pixels "
                                           "(%2 of %3; worst region %4 missing %5)")
                                .arg(100.0 * covered, 0, 'f', 2)
                                .arg(displayedLitTotal)
                                .arg(referenceLitTotal)
                                .arg(worstRegion)
                                .arg(worstMissing));
                        verdict.require(
                            covered >= 0.999,
                            QStringLiteral("the displayed picture shows the lit pixels the "
                                           "independently decoded reference shows (covered %1% of "
                                           "%2 reference-lit pixels)")
                                .arg(100.0 * covered, 0, 'f', 2)
                                .arg(referenceLitTotal));
                    } else {
                        verdict.instrumentation(
                            false,
                            QStringLiteral("the reference picture band carries lit pixels to compare "
                                           "(none found)"));
                    }
                }
                tick.stop();
                stage = Stage::Done;
                app.exit(verdict.report("widget_pixel_probe"));
                break;
            }
            case Stage::Display: {
                if (displayChecked) break;
                // Wait for a settled picture so the comparison is between two
                // frames of one still, not between two different decoded frames.
                if (stageClock.elapsed() < 300) break;
                const QImage now = grabDisplayed(&surface);
                if (now.isNull() || now.size() != surface.size()) {
                    // A screen grab is only possible when this session really has
                    // a display; without one nothing here proves anything.
                    verdict.instrumentation(
                        false, QStringLiteral("the surface's on-screen pixels could be read "
                                              "(no display available)"));
                    say("display: unavailable (no screen grab for %dx%d)\n", surface.width(),
                        surface.height());
                    displayChecked = true;
                    stage = Stage::Clearing;
                    stageClock.restart();
                    break;
                }
                if (!displayPrevious.isNull() && displayPrevious != now) {
                    displayPrevious = now;
                    break;
                }
                if (displayPrevious.isNull()) {
                    displayPrevious = now;
                    break;
                }
                displayChecked = true;
                const QImage framebuffer = sampler.later().framebuffer;
                say("display: %dx%d screen vs %dx%d framebuffer\n", now.width(), now.height(),
                    framebuffer.width(), framebuffer.height());
                const bool screenSaved = now.save(displayOutPath);
                say("screen: %s -> %s\n", displayOutPath.toUtf8().constData(),
                    screenSaved ? "written" : "FAILED");
                verdict.require(screenSaved,
                                QStringLiteral("on-screen proof frame written to %1")
                                    .arg(displayOutPath));
                const bool fboSaved = !framebuffer.isNull() && framebuffer.save(fboOutPath);
                if (!framebuffer.isNull()) {
                    say("framebuffer: %s -> %s\n", fboOutPath.toUtf8().constData(),
                        fboSaved ? "written" : "FAILED");
                }
                verdict.require(fboSaved,
                                QStringLiteral("framebuffer proof frame written to %1")
                                    .arg(fboOutPath));
                if (framebuffer.size() == now.size()) {
                    const mplayer::probes::Picture shown =
                        mplayer::probes::measurePicture(now, sampler.later().videoWidth,
                                                        sampler.later().videoHeight);
                    const mplayer::probes::Picture inFramebuffer =
                        mplayer::probes::measurePicture(framebuffer, sampler.later().videoWidth,
                                                        sampler.later().videoHeight);
                    verdict.metric(QStringLiteral("screen band %1x%2+%3,%4: lit=%5/%6 (%7%)")
                                       .arg(shown.rect.width())
                                       .arg(shown.rect.height())
                                       .arg(shown.rect.x())
                                       .arg(shown.rect.y())
                                       .arg(shown.lit)
                                       .arg(shown.sampled)
                                       .arg(100.0 * shown.litFraction(), 0, 'f', 1));
                    verdict.metric(QStringLiteral("framebuffer band lit=%1/%2 (%3%)")
                                       .arg(inFramebuffer.lit)
                                       .arg(inFramebuffer.sampled)
                                       .arg(100.0 * inFramebuffer.litFraction(), 0, 'f', 1));
                    verdict.require(shown.rect == inFramebuffer.rect,
                                    QStringLiteral("the screen shows the same picture band as the "
                                                   "framebuffer (band %1x%2+%3,%4 on screen, "
                                                   "%5x%6+%7,%8 in the framebuffer)")
                                        .arg(shown.rect.width())
                                        .arg(shown.rect.height())
                                        .arg(shown.rect.x())
                                        .arg(shown.rect.y())
                                        .arg(inFramebuffer.rect.width())
                                        .arg(inFramebuffer.rect.height())
                                        .arg(inFramebuffer.rect.x())
                                        .arg(inFramebuffer.rect.y()));
                    verdict.require(!shown.isMostlyBlack(),
                                    QStringLiteral("the displayed picture carries real content "
                                                   "(lit %1 of %2 sampled)")
                                        .arg(shown.lit)
                                        .arg(shown.sampled));
                } else {
                    verdict.instrumentation(
                        false,
                        QStringLiteral("display and framebuffer share one geometry (%1x%2 vs %3x%4)")
                            .arg(now.width())
                            .arg(now.height())
                            .arg(framebuffer.width())
                            .arg(framebuffer.height()));
                }
                displayPrevious = QImage();
                stage = Stage::Clearing;
                stageClock.restart();
                break;
            }
            case Stage::Clearing:
                if (referenceMode) {
                    // The generated-source lifecycle below closes this surface and
                    // reopens a second lavfi clip; neither describes the real file
                    // this run is judging, and the reference judgement is already
                    // complete above.
                    tick.stop();
                    pump->stop();
                    stage = Stage::Done;
                    app.exit(verdict.report("widget_pixel_probe"));
                    break;
                }
                // The surface's own 50 ms pump keeps draining the idle core's
                // events here: a late estimated-vf-fps from the dead source must
                // not resurface as a current observation.
                if (stageClock.elapsed() >= 800) {
                    idleClosed = surface.playbackDiagnostics();
                    verdict.require(
                        reportedFps == 0.0 &&
                            reportedFpsSource == mplayer::StreamFpsSource::Unknown,
                        QStringLiteral("idle-core events cannot revive the closed source's FPS"));
                    surface.open(QStringLiteral("av://lavfi:testsrc2=size=256x144:rate=15"));
                    afterOpen = surface.playbackDiagnostics();
                    stageClock.restart();
                    stage = Stage::Observing;
                }
                break;
            case Stage::Observing: {
                const mplayer::PlaybackDiagnostics diagnostics = surface.playbackDiagnostics();
                const bool observed = diagnostics.estimatedVfFps.has_value() &&
                                      *diagnostics.estimatedVfFps >= 14.5 &&
                                      *diagnostics.estimatedVfFps <= 15.5 &&
                                      surface.videoWidth() == 256 && surface.videoHeight() == 144 &&
                                      diagnostics.decoderDropCount.has_value() &&
                                      diagnostics.voDropCount.has_value() &&
                                      diagnostics.nativeFilterPresent.has_value() &&
                                      diagnostics.vfCount.has_value();
                if (!observed && stageClock.elapsed() < 4000) break;

                renewed = diagnostics;
                say("lifecycle: first-source fps=%s vo=%s vf=%s native=%s\n",
                    liveFirst.estimatedVfFps.has_value() ? "yes" : "no",
                    liveFirst.voDropCount.has_value() ? "yes" : "no",
                    liveFirst.vfCount.has_value() ? "yes" : "no",
                    liveFirst.nativeFilterPresent.has_value()
                        ? (*liveFirst.nativeFilterPresent ? "present" : "absent")
                        : "unknown");
                say("lifecycle: closed fps=%s vo=%s vf=%s native=%s\n",
                    afterClose.estimatedVfFps.has_value() ? "yes" : "no",
                    afterClose.voDropCount.has_value() ? "yes" : "no",
                    afterClose.vfCount.has_value() ? "yes" : "no",
                    afterClose.nativeFilterPresent.has_value()
                        ? (*afterClose.nativeFilterPresent ? "present" : "absent")
                        : "unknown");
                say("lifecycle: reopened fps=%s vo=%s vf=%s native=%s\n",
                    renewed.estimatedVfFps.has_value() ? "yes" : "no",
                    renewed.voDropCount.has_value() ? "yes" : "no",
                    renewed.vfCount.has_value() ? "yes" : "no",
                    renewed.nativeFilterPresent.has_value()
                        ? (*renewed.nativeFilterPresent ? "present" : "absent")
                        : "unknown");

                verdict.require(allUnknown(afterClose),
                                QStringLiteral("close() drops every raw observation before any "
                                               "event is drained"));
                verdict.require(allUnknown(idleClosed),
                                QStringLiteral("an idle closed core's late events leave every "
                                               "raw observation unknown"));
                verdict.require(allUnknown(afterOpen),
                                QStringLiteral("open() clears the old source's metrics "
                                               "synchronously, before event drain"));

                verdict.require(renewed.vfCount.has_value() &&
                                    renewed.nativeFilterPresent.has_value() &&
                                    !*renewed.nativeFilterPresent,
                                QStringLiteral("the reopened source's vf chain is read again "
                                               "with the native filter absent (never the old "
                                               "source's claim)"));

                if (liveFirst.estimatedVfFps.has_value()) {
                    verdict.require(
                        renewed.estimatedVfFps.has_value() &&
                            *renewed.estimatedVfFps >= 14.5 && *renewed.estimatedVfFps <= 15.5 &&
                            surface.videoWidth() == 256 && surface.videoHeight() == 144,
                        QStringLiteral("the reopened 256x144/15fps source replaces the "
                                       "old cadence and geometry (raw estimated-vf-fps %1)")
                            .arg(renewed.estimatedVfFps.has_value()
                                     ? QString::number(*renewed.estimatedVfFps, 'f', 3)
                                     : QStringLiteral("unavailable")));
                } else {
                    verdict.instrumentation(false,
                                            QStringLiteral("raw estimated-vf-fps reported by the "
                                                           "core on the first source"));
                }
                if (liveFirst.voDropCount.has_value()) {
                    verdict.require(renewed.voDropCount.has_value(),
                                    QStringLiteral("the reopened source re-observes "
                                                   "frame-drop-count (VO drops)"));
                } else {
                    verdict.instrumentation(false,
                                            QStringLiteral("frame-drop-count (VO drops) reported "
                                                           "by the core"));
                }
                if (liveFirst.decoderDropCount.has_value()) {
                    verdict.require(renewed.decoderDropCount.has_value(),
                                    QStringLiteral("the reopened source re-observes "
                                                   "decoder-frame-drop-count"));
                } else {
                    verdict.instrumentation(false,
                                            QStringLiteral("decoder-frame-drop-count reported by "
                                                           "the core"));
                }

                tick.stop();
                pump->stop();
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
