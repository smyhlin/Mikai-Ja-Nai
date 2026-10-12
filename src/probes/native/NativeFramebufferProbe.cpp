// The M43/N1 framebuffer proof: runs the actual VideoSurface QOpenGLWidget
// through the real assembly and controller and reads the pixels back out.
//
// The caller's fixture is static, so one paused position carries one content
// before and after the native plan: no seek is issued and a native-off grab
// pair bounds the readback noise the native output must exceed. That proves
// the wiring changed the picture, not that the filter is numerically correct,
// fast, or fidelity-preserving.

#include <QApplication>
#include <QColor>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>

#include <VideoSurface.hpp>
#include <native/prepare/NativePreparationContext.hpp>
#include <native/prepare/NativePreparedPlan.hpp>
#include <native/status/NativeStatus.hpp>
#include <native/status/NativeTypes.hpp>
#include <native/models/NativeUserRecipes.hpp>

#include "ProbeRuntime.hpp"
#include "state/NativePlaybackAssembly.hpp"
#include "workers/Runner.hpp"

using mplayer::VideoSurface;
using mplayer::native::Backend;
using mplayer::native::NativePlaybackController;
using mplayer::native::NativePreparationContext;
using mplayer::native::NativeState;
using mplayer::native::PreparedAdmissionRequest;
using mplayer::native::Recipe;
using mplayer::native::SelectionState;
using mplayer::native::UserRecipeStore;
using mikai::app::NativeAssemblyRequest;
using mikai::app::Runner;

namespace {

constexpr int kPass = 0;
constexpr int kFail = 1;
constexpr int kSkip = 77;
constexpr int kUsage = 2;

constexpr int kDiffGrid = 32;
constexpr double kNearBlackFloor = 12.0;  // 0x080808 sums to 24; keep lit content lit.

int failures = 0;

bool check(bool ok, const QString& what) {
    std::fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) ++failures;
    return ok;
}

void note(const QString& text) { std::fprintf(stderr, "NOTE %s\n", qPrintable(text)); }
void say(const QString& text) { std::fprintf(stderr, "%s\n", qPrintable(text)); }

/// Spins the GUI event loop until `done` or the bound; the surface's own 50 ms
/// pump is what advances a native transition, so this is the wait, not a sleep.
bool pumpUntil(const std::function<bool()>& done, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (true) {
        if (done()) return true;
        if (clock.elapsed() >= timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
}

void pumpFor(int ms) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
}

/// One framebuffer read plus the geometry and health it was taken with. The
/// source size is the decoder's own, so the band is the true source aspect even
/// when the filter has changed the output size.
struct Grab {
    QImage image;
    int sourceWidth = 0;
    int sourceHeight = 0;
    QRect band;
    double nonblack = 0.0;
    double mean = 0.0;
    double variance = 0.0;
    int glErrors = -1;  // -1: the context was not current, so nothing was read.
    bool valid = false;
};

int readGlErrors(VideoSurface& surface) {
    QOpenGLContext* context = surface.context();
    if (context == nullptr || QOpenGLContext::currentContext() != context) return -1;
    QOpenGLFunctions* functions = context->functions();
    if (functions == nullptr) return -1;
    int errors = 0;
    while (functions->glGetError() != GL_NO_ERROR && errors < 32) ++errors;
    return errors;
}

Grab grab(VideoSurface& surface) {
    Grab grab;
    grab.image = surface.grabFramebuffer();
    grab.sourceWidth = surface.videoWidth();
    grab.sourceHeight = surface.videoHeight();
    grab.glErrors = readGlErrors(surface);
    if (grab.image.isNull()) return grab;
    const QSize source(grab.sourceWidth, grab.sourceHeight);
    if (!source.isValid() || source.isEmpty() || grab.image.size().isEmpty()) return grab;
    const QSize band = source.scaled(grab.image.size(), Qt::KeepAspectRatio);
    if (!band.isValid() || band.isEmpty()) return grab;
    grab.band = QRect(QPoint((grab.image.width() - band.width()) / 2,
                             (grab.image.height() - band.height()) / 2),
                      band);
    double sum = 0.0;
    double sumSquares = 0.0;
    qint64 count = 0;
    qint64 lit = 0;
    for (int y = grab.band.top(); y <= grab.band.bottom(); ++y) {
        for (int x = grab.band.left(); x <= grab.band.right(); ++x) {
            const QColor pixel = grab.image.pixelColor(x, y);
            const double luma =
                0.2126 * pixel.red() + 0.7152 * pixel.green() + 0.0722 * pixel.blue();
            sum += luma;
            sumSquares += luma * luma;
            ++count;
            if (pixel.red() + pixel.green() + pixel.blue() > kNearBlackFloor) ++lit;
        }
    }
    if (count > 0) {
        grab.mean = sum / count;
        grab.variance = std::max(0.0, sumSquares / count - grab.mean * grab.mean);
        grab.nonblack = double(lit) / double(count);
    }
    grab.valid = count > 0;
    return grab;
}

QColor sampleNormalized(const Grab& grab, double u, double v) {
    const int x = grab.band.x() +
                  std::clamp(int(u * (grab.band.width() - 1) + 0.5), 0, grab.band.width() - 1);
    const int y = grab.band.y() +
                  std::clamp(int(v * (grab.band.height() - 1) + 0.5), 0, grab.band.height() - 1);
    return grab.image.pixelColor(x, y);
}

/// Mean per-channel difference over a normalized grid, so two bands that no
/// longer share a pixel geometry still compare the same picture content.
double normalizedDiff(const Grab& a, const Grab& b) {
    if (!a.valid || !b.valid) return -1.0;
    double total = 0.0;
    qint64 count = 0;
    for (int gy = 0; gy < kDiffGrid; ++gy) {
        for (int gx = 0; gx < kDiffGrid; ++gx) {
            const double u = (gx + 0.5) / kDiffGrid;
            const double v = (gy + 0.5) / kDiffGrid;
            const QColor pa = sampleNormalized(a, u, v);
            const QColor pb = sampleNormalized(b, u, v);
            total += (std::abs(pa.red() - pb.red()) + std::abs(pa.green() - pb.green()) +
                      std::abs(pa.blue() - pb.blue())) /
                     3.0;
            ++count;
        }
    }
    return count > 0 ? total / count : -1.0;
}

bool savePng(const Grab& grab, const QString& path) {
    if (grab.image.isNull()) return false;
    return grab.image.save(path);
}

/// The refusal keys that name a missing prerequisite of this machine/session
/// rather than a wrong native plan: with those the probe can only say the run
/// could not be demonstrated here.
bool refusalIsEnvironment(const QString& key) {
    return key == QStringLiteral("native.assembly.sourceUnobserved") ||
           key == QStringLiteral("native.assembly.deviceUnobserved") ||
           key == QStringLiteral("native.assembly.moduleUnobserved") ||
           key == QStringLiteral("native.assembly.moduleMismatch") ||
           key.startsWith(QStringLiteral("native.prepare.")) ||
           key.startsWith(QStringLiteral("native.bundle."));
}

struct Options {
    QString media;
    QString recipeId;
    Backend backend = Backend::TensorRT;
    QString outputDir;
};

struct AssemblyJob {
    std::atomic<bool> done{false};
    bool workerFailed = false;
    QString workerError;
    mikai::app::NativeAssemblyOutcome outcome;
};

QString bandText(const Grab& grab) {
    return QStringLiteral("%1x%2@%3,%4")
        .arg(grab.band.width())
        .arg(grab.band.height())
        .arg(grab.band.x())
        .arg(grab.band.y());
}

int runProof(const Options& options) {
    VideoSurface surface;
    QPointer<VideoSurface> guard(&surface);
    surface.resize(640, 360);
    surface.setWindowTitle(QStringLiteral("NativeFramebufferProbe"));
    QString mediaError;
    QString firstFailureKey;
    QString firstFailureReason;
    QObject::connect(&surface, &VideoSurface::errorOccurred, &surface,
                     [&](const QString& message) {
                         mediaError = message;
                         std::fprintf(stderr, "media error: %s\n", qPrintable(message));
                     });
    QObject::connect(&surface, &VideoSurface::nativeProcessingFailure, &surface,
                     [&](const QString& key, const QString& reason) {
                         if (firstFailureKey.isEmpty()) {
                             firstFailureKey = key;
                             firstFailureReason = reason;
                         }
                         std::fprintf(stderr, "native failure: %s %s\n", qPrintable(key),
                                      qPrintable(reason));
                     });
    surface.show();

    const auto conclude = [&](int code) {
        surface.settleNativeForTeardown(4000);
        surface.hide();
        surface.close();
        return code;
    };

    if (!surface.waitForContext(20000)) {
        say(QStringLiteral("no GL context was created for the surface"));
        return conclude(kFail);
    }
    if (!surface.open(options.media)) {
        say(QStringLiteral("the engine refused to open %1").arg(options.media));
        return conclude(kFail);
    }
    if (!pumpUntil(
            [&] {
                return !guard.isNull() && surface.hasVideo() && surface.videoWidth() > 0 &&
                       surface.isContextReady();
            },
            20000)) {
        say(QStringLiteral("the source never produced a decoded frame (media error: %1)")
                .arg(mediaError.isEmpty() ? QStringLiteral("<none>") : mediaError));
        return conclude(kSkip);
    }

    // Start the original pipeline, then park it: grabs are only taken while
    // playback is parked and no seek is ever issued, so the control pair cannot
    // pick up seek noise.
    surface.setPaused(false);
    pumpUntil([&] { return !guard.isNull() && surface.position() > 0.25; }, 10000);
    surface.setPaused(true);
    pumpUntil([&] { return !guard.isNull() && !surface.isPlaying(); }, 5000);
    pumpFor(300);

    const QDir outputDir(options.outputDir);
    if (!outputDir.exists() && !QDir().mkpath(options.outputDir)) {
        say(QStringLiteral("output directory %1 could not be created").arg(options.outputDir));
        return conclude(kFail);
    }
    const Grab control1 = grab(surface);
    pumpFor(200);
    const Grab control2 = grab(surface);
    check(control1.valid && control2.valid,
          QStringLiteral("a native-off picture was read at the paused position"));
    if (!control1.valid || !control2.valid) {
        say(QStringLiteral("source=%1x%2 hasVideo=%3 contextReady=%4")
                .arg(surface.videoWidth())
                .arg(surface.videoHeight())
                .arg(int(surface.hasVideo()))
                .arg(int(surface.isContextReady())));
        return conclude(kFail);
    }
    check(control1.nonblack > 0.5,
          QStringLiteral("the native-off band is lit (%1% nonblack, mean %2)")
              .arg(control1.nonblack * 100.0, 0, 'f', 1)
              .arg(control1.mean, 0, 'f', 2));
    const double controlNoise = normalizedDiff(control2, control1);
    const bool controlsSaved =
        savePng(control1, outputDir.filePath(QStringLiteral("native-off-1.png"))) &&
        savePng(control2, outputDir.filePath(QStringLiteral("native-off-2.png")));
    check(controlsSaved, QStringLiteral("the native-off grabs were written to %1")
                             .arg(options.outputDir));
    say(QStringLiteral("control pair: band %1 mean %2 variance %3 nonblack %4% "
                       "noise(random)=%5 GLerrors=%6")
            .arg(bandText(control1))
            .arg(control1.mean, 0, 'f', 3)
            .arg(control1.variance, 0, 'f', 3)
            .arg(control1.nonblack * 100.0, 0, 'f', 1)
            .arg(controlNoise, 0, 'f', 4)
            .arg(control1.glErrors));

    surface.setNativeStatusPolling(true);
    // The probe is armed for the backend this run actually admits, so the filter
    // never observes frames a decoder that backend cannot consume produced. The
    // alignment is asynchronous, so the probe may not be attached yet here.
    surface.armNativeProbe(options.backend);
    const auto probeObserved = [&] {
        const auto& view = surface.nativeStatus();
        return view.snapshot.has_value() && view.snapshot->probe.has_value() &&
               view.snapshot->probe->source.has_value() &&
               view.snapshot->probe->source->isKnown() && view.snapshot->probe->device.has_value() &&
               view.snapshot->probe->device->isKnown();
    };
    bool observed = pumpUntil(probeObserved, 12000);
    if (!observed) {
        // The producer observes from decoded frames; park-then-attach can leave
        // it without one, so one bounded unpause is allowed to feed it before
        // the position is parked again.
        note(QStringLiteral("the producer published no observation while paused; feeding it one "
                            "bounded burst of decoded frames"));
        surface.setPaused(false);
        observed = pumpUntil(probeObserved, 12000);
        surface.setPaused(true);
        pumpUntil([&] { return !guard.isNull() && !surface.isPlaying(); }, 5000);
        pumpFor(200);
    }
    if (!observed) {
        const auto& view = surface.nativeStatus();
        say(QStringLiteral("the filter published no complete source/device observation "
                           "(availability=%1 %2)")
                .arg(view.availability.reasonKey, view.availability.reason));
        return conclude(view.availability.reasonKey == QStringLiteral("native.status.missing")
                            ? kFail
                            : kSkip);
    }
    {
        const auto snapshot = surface.nativeStatus().snapshot;
        check(snapshot->state == NativeState::Probe,
              QStringLiteral("the attached filter reports probe-only passthrough (state=%1)")
                  .arg(mplayer::native::stableName(snapshot->state)));
        check(!snapshot->effective.has_value(),
              QStringLiteral("the attached filter reports no effective plan"));
        check(!snapshot->counters.has_value(),
              QStringLiteral("the attached filter reports no cadence counters"));
        if (snapshot->state != NativeState::Probe) {
            say(QStringLiteral("the filter's own reason: %1").arg(snapshot->reason));
        }
    }

    const std::optional<NativePreparationContext> context = surface.nativePreparationContext();
    if (!context.has_value()) {
        say(QStringLiteral("the surface produced no preparation context"));
        return conclude(kSkip);
    }
    // The decoder the core reported, and the backend it was aligned FOR. A run
    // that admits TensorRT but decoded on something else must not be reported as
    // a proof of anything: this is the alignment contract, stated as an
    // observation.
    say(QStringLiteral("aligned decoder: %1 for backend %2")
            .arg(context->decoder.isEmpty() ? QStringLiteral("<unreported>") : context->decoder,
                 context->desiredBackend.has_value()
                     ? mplayer::native::stableName(*context->desiredBackend)
                     : QStringLiteral("<none>")));
    check(!context->decoder.isEmpty() && context->desiredBackend.has_value() &&
              *context->desiredBackend == options.backend,
          QStringLiteral("the core reports a decoder it was aligned to for the requested "
                         "backend (decoder=%1 backend=%2)")
              .arg(context->decoder,
                   context->desiredBackend.has_value()
                       ? mplayer::native::stableName(*context->desiredBackend)
                       : QStringLiteral("<none>")));

    // The explicit paused Prepare contract: two REAL adjacent frames, produced
    // while paused, with the original position restored. This runs BEFORE any
    // admission so the capture path is exercised on a surface no native plan can
    // be altering.
    {
        struct PairCapture {
            std::atomic<bool> done{false};
            int firstIndex = -1;
            int secondIndex = -1;
            double firstPosition = 0.0;
            double secondPosition = 0.0;
            quint64 firstEpoch = 0;
            quint64 secondEpoch = 0;
            QString firstKey;
            QString secondKey;
            bool adjacent = false;
            // The cadence the surface itself measured and delivered with the first
            // frame. Kept, not recomputed: the restore bound and this check must
            // be judged against the SAME observed interval, or the probe would be
            // holding the surface to a number it never claimed.
            std::optional<mplayer::native::Rational> firstFps;
        };
        auto pair = std::make_shared<PairCapture>();
        const double parkedAt = surface.position();
        QObject::connect(
            &surface, &VideoSurface::nativeReferenceFrameReady, &surface,
            [pair, surfacePtr = &surface, parkedAt](const mplayer::native::FrameFixture& frame,
                                          quint64 sourceEpoch, double framePosition, int frameIndex,
                                          std::optional<mplayer::native::Rational> frameFps) {
                if (frameIndex == 0) {
                    pair->firstIndex = 0;
                    pair->firstPosition = framePosition;
                    pair->firstEpoch = sourceEpoch;
                    // A pair needs a real interval; an unobserved cadence would make
                    // "adjacent" a claim this run cannot make.
                    pair->adjacent = frameFps.has_value() && frameFps->isValid();
                    pair->firstFps = frameFps;
                } else if (frameIndex == 1) {
                    pair->secondIndex = 1;
                    pair->secondPosition = framePosition;
                    pair->secondEpoch = sourceEpoch;
                }
                if (pair->firstIndex >= 0 && pair->secondIndex >= 0) {
                    // Two distinct observations: same source epoch, different
                    // position, and a bounded forward step consistent with the
                    // observed cadence.
                    const bool sameSource = pair->firstEpoch == pair->secondEpoch;
                    const double step = pair->secondPosition - pair->firstPosition;
                    const bool moved = step > 0.0;
                    check(sameSource && moved,
                          QStringLiteral("the temporal pair is two real adjacent frames of ONE "
                                         "source (epoch0=%1 epoch1=%2 step=%3s)")
                              .arg(pair->firstEpoch)
                              .arg(pair->secondEpoch)
                              .arg(step, 0, 'f', 6));
                    // Half an OBSERVED source frame, from the cadence the signal
                    // just delivered - never a constant. mpv decodes from container
                    // timestamps, so an exact seek lands on a frame boundary and
                    // the core's own clock reads back the boundary, not the value
                    // that was written; demanding bit equality would refuse every
                    // restore of a source whose timestamps are not frame aligned
                    // while saying nothing the half-frame bound does not already
                    // say. An unobserved or degenerate cadence has no honest bound,
                    // so the pair cannot make this claim at all and the read is
                    // skipped rather than widened into one that accepts anything.
                    const double restoredAt = surfacePtr->position();
                    const bool cadenceKnown = pair->adjacent;
                    const double halfFrame =
                        cadenceKnown
                            ? 0.5 * static_cast<double>(pair->firstFps->den) /
                                  static_cast<double>(pair->firstFps->num)
                            : 0.0;
                    const bool restored =
                        cadenceKnown && halfFrame > 0.0 &&
                        std::abs(restoredAt - parkedAt) <= halfFrame;
                    check(restored,
                          QStringLiteral("the parked position was restored after the pair, within "
                                         "half an observed source frame (before=%1 after=%2 "
                                         "tolerance=%3)")
                              .arg(parkedAt, 0, 'f', 6)
                              .arg(restoredAt, 0, 'f', 6)
                              .arg(halfFrame, 0, 'f', 6));
                    pair->done.store(true);
                }
            });
        QObject::connect(
            &surface, &VideoSurface::nativeReferenceFrameFailed, &surface,
            [pair](const QString& reasonKey, const QString& reason, int frameIndex) {
                if (frameIndex == 0) {
                    pair->firstKey = reasonKey;
                    pair->done.store(true);
                } else if (frameIndex == 1) {
                    pair->secondKey = reasonKey;
                    pair->done.store(true);
                }
                std::fprintf(stderr, "reference frame %d refused: %s (%s)\n", frameIndex,
                             qPrintable(reasonKey), qPrintable(reason));
            });
        surface.requestNativeReferenceFrame(0);
        const bool firstOk =
            pumpUntil([&] { return pair->firstIndex == 0 || pair->done.load(); }, 15000) &&
            pair->firstIndex == 0;
        check(firstOk,
              QStringLiteral("a real decoded frame was captured at the paused position without "
                             "resuming (%1)")
                  .arg(firstOk ? QStringLiteral("index 0 delivered")
                               : pair->firstKey.isEmpty()
                                       ? QStringLiteral("no answer")
                                       : pair->firstKey));
        if (firstOk) {
            surface.requestNativeReferenceFrame(1);
            const bool secondOk =
                pumpUntil([&] { return pair->done.load(); }, 20000) && pair->secondIndex == 1;
            check(secondOk,
                  QStringLiteral("the second temporal frame is a real adjacent observation (%1)")
                      .arg(secondOk ? QStringLiteral("index 1 delivered")
                                    : pair->secondKey.isEmpty()
                                            ? QStringLiteral("no answer")
                                            : pair->secondKey));
            check(pair->adjacent,
                  QStringLiteral("the source cadence was measured, not assumed"));
        }
    }
    const QSize sourceSize = context->source.displaySize().value_or(QSize());
    say(QStringLiteral("observed source=%1x%2 fps=%3 csp=%4 bitDepth=%5")
            .arg(sourceSize.width())
            .arg(sourceSize.height())
            .arg(context->source.fps.has_value() ? context->source.fps->toString()
                                                 : QStringLiteral("<absent>"))
            .arg(context->source.csp.isEmpty() ? QStringLiteral("<absent>") : context->source.csp)
            .arg(context->source.bitDepth));
    say(QStringLiteral("observed device decoder=%1 compute=%2 sm=%3.%4 module=%5")
            .arg(context->device.decoderDeviceUuid, context->device.computeDeviceUuid)
            .arg(context->device.smMajor)
            .arg(context->device.smMinor)
            .arg(context->modulePath));

    NativeAssemblyRequest request;
    request.selection = SelectionState{options.recipeId, options.backend};
    request.context = *context;
    request.requestId = QStringLiteral("native-framebuffer-probe#1");
    request.revision = 1;

    Runner runner;
    auto job = std::make_shared<AssemblyJob>();
    runner.run(
        [job, request]() -> QVariant {
            job->outcome = mikai::app::assembleNativeAdmission(request);
            job->done.store(true);
            return QVariant();
        },
        [](const QVariant&) {},
        [job](const backend::Failure& failure) {
            job->workerError = backend::failureLine(failure);
            job->workerFailed = true;
            job->done.store(true);
        });
    if (!pumpUntil([&] { return job->done.load(); }, 180000)) {
        runner.waitForDone(2000);
        say(QStringLiteral("the assembly worker did not finish inside its bound"));
        return conclude(kFail);
    }
    if (job->workerFailed || !job->outcome.ok) {
        const QString key = job->outcome.ok ? QStringLiteral("native.assembly.workerFailed")
                                            : job->outcome.reasonKey;
        const QString reason = job->outcome.ok ? job->workerError : job->outcome.reason;
        say(QStringLiteral("the whole-plan assembly refused %1: %2").arg(key, reason));
        return conclude(!job->workerFailed && refusalIsEnvironment(key) ? kSkip : kFail);
    }
    say(QStringLiteral("assembled plan: recipe=%1 backend=%2 stages=%3 output=%4")
            .arg(job->outcome.request.recipeId)
            .arg(mplayer::native::stableName(job->outcome.request.backend))
            .arg(job->outcome.request.plan.stages.size())
            .arg(job->outcome.request.plan.outputGeometry().has_value()
                     ? QStringLiteral("%1x%2")
                           .arg(job->outcome.request.plan.outputGeometry()->width())
                           .arg(job->outcome.request.plan.outputGeometry()->height())
                     : QStringLiteral("<unresolved>")));
    say(QStringLiteral("measured build: coreSha=%1 sourceAbi=%2 device=%3 sm=%4.%5 runtime=%6 "
                       "cuda=%7")
            .arg(job->outcome.request.build.coreSha)
            .arg(job->outcome.request.build.sourceAbi)
            .arg(job->outcome.request.build.deviceUuid)
            .arg(job->outcome.request.build.smMajor)
            .arg(job->outcome.request.build.smMinor)
            .arg(job->outcome.request.build.runtimeVersion, job->outcome.request.build.cudaVersion));

    PreparedAdmissionRequest submitted = job->outcome.request;
    submitted.epoch = surface.nativePlaybackStatus().sourceEpoch;
    surface.setNativeStatusPolling(true);
    surface.requestNativeAdmission(submitted);
    surface.setPaused(false);
    const auto activeWithCounters = [&] {
        const auto status = surface.nativePlaybackStatus();
        const auto& view = surface.nativeStatus();
        return status.state == NativePlaybackController::State::Active && status.confirmed &&
               status.pendingRequestId.isEmpty() && status.effective.has_value() &&
               status.counters.has_value() && status.counters->framesIn > 0 &&
               view.snapshot.has_value() && view.snapshot->state == NativeState::Active;
    };
    const bool activated = pumpUntil(activeWithCounters, 120000);
    surface.setPaused(true);
    pumpUntil([&] { return !guard.isNull() && !surface.isPlaying(); }, 5000);
    pumpFor(300);
    if (!activated) {
        const auto status = surface.nativePlaybackStatus();
        say(QStringLiteral("native never became Active with completed inference: state=%1 "
                           "confirmed=%2 effective=%3 counters=%4 failure=%5 %6")
                .arg(int(status.state))
                .arg(int(status.confirmed))
                .arg(int(status.effective.has_value()))
                .arg(int(status.counters.has_value()))
                .arg(firstFailureKey, firstFailureReason));
        return conclude(kFail);
    }
    {
        const auto status = surface.nativePlaybackStatus();
        const auto& plan = *status.effective;
        say(QStringLiteral("effective plan: recipe=%1 provider=%2 model=%3 stages=%4 order=%5 "
                           "source=%6x%7 output=%8x%9 sourceFps=%10 outputFps=%11 decoder=%12 csp=%13")
                .arg(plan.recipe, plan.provider, plan.model)
                .arg(plan.spatialStages)
                .arg(plan.order)
                .arg(plan.sourceSize.w)
                .arg(plan.sourceSize.h)
                .arg(plan.outputSize.w)
                .arg(plan.outputSize.h)
                .arg(plan.sourceFps.has_value() ? plan.sourceFps->toString()
                                                : QStringLiteral("<absent>"))
                .arg(plan.outputFps.has_value() ? plan.outputFps->toString()
                                                : QStringLiteral("<absent>"))
                .arg(plan.decoder, plan.csp));
        // The effective block exists only after a frame completed under the
        // admitted plan, and its provider is the runtime the filter actually
        // loaded - a fallback would name itself here instead.
        check(plan.provider.compare(QStringLiteral("tensorrt"), Qt::CaseInsensitive) == 0,
              QStringLiteral("the effective provider names the CUDA runtime (%1)").arg(plan.provider));
        if (plan.interpolation.has_value()) {
            say(QStringLiteral("effective interpolation: factor=%1 model=%2")
                    .arg(plan.interpolation->factor.toString(), plan.interpolation->model));
        }
        say(QStringLiteral("cadence counters: in=%1 out=%2 dropped=%3 interpolated=%4 sceneCuts=%5 "
                           "requestId=%6 buildKeys=%7")
                .arg(status.counters->framesIn)
                .arg(status.counters->framesOut)
                .arg(status.counters->dropped)
                .arg(status.counters->interpolated)
                .arg(status.counters->sceneCuts)
                .arg(status.requestId)
                .arg(status.buildKeys.join(QLatin1Char(','))));
    }

    const Grab native1 = grab(surface);
    pumpFor(200);
    const Grab native2 = grab(surface);
    check(native1.valid && native2.valid,
          QStringLiteral("a native picture was read at the same paused position"));
    if (!native1.valid || !native2.valid) {
        say(QStringLiteral("native source=%1x%2 hasVideo=%3")
                .arg(surface.videoWidth())
                .arg(surface.videoHeight())
                .arg(int(surface.hasVideo())));
        return conclude(kFail);
    }
    const bool nativeSaved =
        savePng(native1, outputDir.filePath(QStringLiteral("native-1.png"))) &&
        savePng(native2, outputDir.filePath(QStringLiteral("native-2.png")));
    check(nativeSaved, QStringLiteral("the native grabs were written to %1").arg(options.outputDir));
    // Same widget and same source means the only variable is the filter: a
    // resized framebuffer would make the delta a resize artifact instead.
    check(control1.image.size() == native1.image.size() && control1.band.size() == native1.band.size(),
          QStringLiteral("the framebuffer and band geometry are unchanged between the control and "
                         "native grabs"));
    const double nativeStability = normalizedDiff(native2, native1);
    const double delta = normalizedDiff(native1, control1);
    say(QStringLiteral("native pair: band %1 mean %2 variance %3 nonblack %4% "
                       "stability=%5 GLerrors=%6")
            .arg(bandText(native1))
            .arg(native1.mean, 0, 'f', 3)
            .arg(native1.variance, 0, 'f', 3)
            .arg(native1.nonblack * 100.0, 0, 'f', 1)
            .arg(nativeStability, 0, 'f', 4)
            .arg(native1.glErrors));
    const double noise = std::max(controlNoise, nativeStability);
    const double required = std::max(4.0 * noise, 0.5);
    say(QStringLiteral("pixel delta native-vs-native-off=%1 required>%2 (measured control noise=%3 "
                       "native stability=%4)")
            .arg(delta, 0, 'f', 4)
            .arg(required, 0, 'f', 4)
            .arg(controlNoise, 0, 'f', 4)
            .arg(nativeStability, 0, 'f', 4));
    check(native1.nonblack > 0.5,
          QStringLiteral("the native band is lit (%1% nonblack)").arg(native1.nonblack * 100.0, 0,
                                                                      'f', 1));
    check(delta > required,
          QStringLiteral("the native output differs from the native-off control beyond the "
                         "measured noise"));
    say(QStringLiteral("MEASUREMENT LIMIT: pixel comparison on a static fixture is a wiring proof, "
                       "not a performance or fidelity certification; no seek was issued between "
                       "the control grabs"));

    // A frozen plan must take a request without activating it, and the latest
    // request must be the one that activates once the lock is released.
    {
        QString lockKey;
        QString lockReason;
        auto lock = surface.acquireNativePlanLock(&lockKey, &lockReason);
        if (!lock.has_value()) {
            std::fprintf(stdout, "RESULT-M31: SKIP (the plan lock was not exercised: %s: %s)\n",
                         qPrintable(lockKey), qPrintable(lockReason));
            note(QStringLiteral("this run does not claim the plan-lock contract"));
        } else {
            const QString lockedRequestId = surface.nativePlaybackStatus().requestId;
            PreparedAdmissionRequest second = submitted;
            second.requestId = QStringLiteral("native-framebuffer-probe#2");
            second.revision = 2;
            surface.setNativeStatusPolling(true);
            surface.requestNativeAdmission(second);
            PreparedAdmissionRequest third = submitted;
            third.requestId = QStringLiteral("native-framebuffer-probe#3");
            third.revision = 3;
            surface.requestNativeAdmission(third);
            pumpFor(1500);
            const auto locked = surface.nativePlaybackStatus();
            const bool gated = check(
                surface.queuedNativeTransitions() == 1 &&
                    locked.state == NativePlaybackController::State::Active && locked.confirmed &&
                    locked.requestId == lockedRequestId && locked.pendingRequestId.isEmpty(),
                QStringLiteral("a locked admission is queued, not activated (queued=%1 state=%2 "
                               "requestId=%3)")
                    .arg(surface.queuedNativeTransitions())
                    .arg(int(locked.state))
                    .arg(locked.requestId));
            lock.reset();
            // Activation needs a real completed output, as the compare path
            // does: a paused frame is not reprocessed, so playback resumes
            // until #3 carries its own acknowledged counters.
            surface.setPaused(false);
            const bool reactivated = pumpUntil(
                [&] {
                    const auto status = surface.nativePlaybackStatus();
                    return status.state == NativePlaybackController::State::Active &&
                           status.confirmed && status.pendingRequestId.isEmpty() &&
                           status.counters.has_value() && status.counters->framesIn > 0 &&
                           status.requestId == QStringLiteral("native-framebuffer-probe#3");
                },
                60000);
            surface.setPaused(true);
            pumpUntil([&] { return !guard.isNull() && !surface.isPlaying(); }, 5000);
            pumpFor(300);
            const bool latestWins = surface.nativePlaybackStatus().requestId ==
                                    QStringLiteral("native-framebuffer-probe#3");
            check(reactivated,
                  QStringLiteral("the latest queued request reaches a real completed output after "
                                 "the unlock (requestId=%1)")
                      .arg(surface.nativePlaybackStatus().requestId));
            check(latestWins,
                  QStringLiteral("the latest of two queued requests is the one that activated"));
            std::fprintf(stdout, "RESULT-M31: %s\n",
                         (gated && reactivated && latestWins)
                             ? "PASS (the latest queued request activated only after the unlock)"
                             : "FAIL");
        }
    }

    surface.turnNativeOff();
    const bool removed = pumpUntil(
        [&] {
            const auto status = surface.nativePlaybackStatus();
            return status.state == NativePlaybackController::State::Off && !status.confirmed &&
                   !status.effective.has_value() && status.pendingRequestId.isEmpty();
        },
        60000);
    check(removed, QStringLiteral("native removal was acknowledged as Off with no effective plan"));
    // The reader halted on the drain-acknowledged Off, so a fresh read is armed
    // to observe the removal itself. "Not Active" alone would also accept a
    // Failed filter, which is not absence.
    const bool vfAbsent = pumpUntil(
        [&] {
            const mplayer::PlaybackDiagnostics diagnostics = surface.playbackDiagnostics();
            return diagnostics.nativeFilterPresent.has_value() && !*diagnostics.nativeFilterPresent;
        },
        10000);
    surface.setNativeStatusPolling(true);
    const bool readerNamedAbsence = pumpUntil(
        [&] {
            return surface.nativeStatus().availability.reasonKey ==
                   QStringLiteral("native.status.missing");
        },
        8000);
    check(vfAbsent || readerNamedAbsence,
          QStringLiteral("the native filter is actually gone after the acknowledged removal "
                         "(vfChainAbsent=%1 readerUnavailable=%2)")
              .arg(int(vfAbsent))
              .arg(int(readerNamedAbsence)));

    const double positionBefore = surface.position();
    surface.setPaused(false);
    const bool playingAgain =
        pumpUntil([&] { return !guard.isNull() && surface.position() > positionBefore + 0.02; },
                  6000);
    surface.setPaused(true);
    pumpUntil([&] { return !guard.isNull() && !surface.isPlaying(); }, 5000);
    pumpFor(300);
    check(playingAgain, QStringLiteral("the core still decodes and plays after native removal"));

    // Taken after frames have flowed through the now filter-free chain, so this
    // is a real native-off render and not the filter's last output left behind.
    const Grab afterOff = grab(surface);
    check(afterOff.valid, QStringLiteral("a picture was read after native removal"));
    if (afterOff.valid) {
        savePng(afterOff, outputDir.filePath(QStringLiteral("native-off-after.png")));
        const double offDelta = normalizedDiff(afterOff, control1);
        say(QStringLiteral("post-off picture delta vs the native-off control=%1").arg(offDelta, 0, 'f',
                                                                                     4));
        check(offDelta <= required,
              QStringLiteral("the post-off picture matches the native-off control within noise"));
    }

    check(surface.isContextReady(), QStringLiteral("the GL context is alive at the end"));
    check(mediaError.isEmpty(), QStringLiteral("no media error was reported"));

    return conclude(failures == 0 ? kPass : kFail);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "M43/N1 proof: runs the real VideoSurface with the native filter and proves the pixels."));
    parser.addHelpOption();
    const QCommandLineOption mediaOption(QStringLiteral("media"),
                                         QStringLiteral("Media the surface opens."),
                                         QStringLiteral("url"));
    const QCommandLineOption recipeOption(QStringLiteral("recipe"),
                                          QStringLiteral("Stored recipe id to admit."),
                                          QStringLiteral("id"));
    const QCommandLineOption recipeFileOption(
        QStringLiteral("recipe-file"),
        QStringLiteral("Recipe document to store immutably through UserRecipeStore, then admit."),
        QStringLiteral("path"));
    const QCommandLineOption bundleOption(
        QStringLiteral("bundle"),
        QStringLiteral("Absolute root of the validated native bundle (MIKAI_PROBE_BUNDLE)."),
        QStringLiteral("path"));
    const QCommandLineOption outputOption(
        QStringLiteral("output-dir"),
        QStringLiteral("Directory the PNGs and evidence lines are written to."),
        QStringLiteral("path"), QStringLiteral("native-framebuffer-proof"));
    const QCommandLineOption orgOption(QStringLiteral("app-org"),
                                       QStringLiteral("Organization that owns the app-local tree."),
                                       QStringLiteral("org"), QStringLiteral("Mikai"));
    const QCommandLineOption nameOption(
        QStringLiteral("app-name"), QStringLiteral("Application name that owns the app-local tree."),
        QStringLiteral("name"), QStringLiteral("Mikai Wrapper"));
    parser.addOptions(QList<QCommandLineOption>{mediaOption, recipeOption, recipeFileOption,
                                                bundleOption, outputOption, orgOption, nameOption});
    parser.process(app);

    const bool hasRecipe = parser.isSet(recipeOption);
    const bool hasRecipeFile = parser.isSet(recipeFileOption);
    if (!parser.isSet(mediaOption) || hasRecipe == hasRecipeFile) {
        std::fprintf(stderr,
                     "native_framebuffer_probe: --media and exactly one of --recipe/--recipe-file "
                     "are required\n");
        return kUsage;
    }

    QCoreApplication::setOrganizationName(parser.value(orgOption));
    QCoreApplication::setApplicationName(parser.value(nameOption));

    const QString bundle = parser.isSet(bundleOption) ? parser.value(bundleOption) : QString();
    if (!bundle.isEmpty()) qputenv("MIKAI_PROBE_BUNDLE", bundle.toUtf8());
    if (!mplayer::probes::selectRuntime()) {
        std::fprintf(stderr, "native_framebuffer_probe: no runtime could be selected\n");
        return kUsage;
    }

    Options options;
    options.media = parser.value(mediaOption);
    options.outputDir = parser.value(outputOption);
    if (hasRecipeFile) {
        const QString path = parser.value(recipeFileOption);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "native_framebuffer_probe: the recipe file %s cannot be read\n",
                         qPrintable(path));
            return kUsage;
        }
        Recipe recipe;
        QString key;
        QString reason;
        if (!mplayer::native::parseRecipe(file.readAll(), &recipe, &key, &reason)) {
            std::fprintf(stderr, "native_framebuffer_probe: the recipe file was refused %s: %s\n",
                         qPrintable(key), qPrintable(reason));
            return kUsage;
        }
        UserRecipeStore store;
        const std::optional<Recipe> stored = store.saveRecipe(recipe, &key, &reason);
        if (!stored.has_value()) {
            std::fprintf(stderr,
                         "native_framebuffer_probe: the recipe could not be stored %s: %s\n",
                         qPrintable(key), qPrintable(reason));
            return kUsage;
        }
        options.recipeId = stored->id;
        options.backend = stored->backend;
        std::fprintf(stderr, "native_framebuffer_probe: the recipe file is stored immutably as %s\n",
                     qPrintable(stored->id));
    } else {
        options.recipeId = parser.value(recipeOption);
    }

    const int code = runProof(options);
    std::fprintf(stdout, "RESULT: %s\n",
                 code == kPass ? "PASS" : (code == kSkip ? "SKIP" : "FAIL"));
    std::fflush(stdout);
    return code;
}
