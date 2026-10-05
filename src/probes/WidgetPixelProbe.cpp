// The saved PNG is the measured surface framebuffer, never a widget grab.

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLDebugLogger>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <QTimer>

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

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }
    const QString outPath = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                     : QStringLiteral("widget_pixel_probe.png");
    DebugSurface surface;
    surface.resize(320, 180);
    surface.show();
    surface.open(QStringLiteral("av://lavfi:testsrc2=size=320x180:rate=25"));
    surface.setSpeed(1.0);

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

    enum class Stage { Sampling, Clearing, Observing, Done };
    Stage stage = Stage::Sampling;
    bool proofed = false;
    QElapsedTimer stageClock;
    stageClock.start();
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

        mplayer::probes::requirePlayback(verdict, sampler.earlier(), sampler.later());

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
                    stage = Stage::Clearing;
                }
                break;
            }
            case Stage::Clearing:
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
