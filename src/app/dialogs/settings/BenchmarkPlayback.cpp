#include "BenchmarkPlayback.hpp"

#include "MpvHandle.hpp"
#include "MpvRenderContext.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QThread>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>

#include <backend/Cancellation.hpp>
#include <native/components/AnimeJanaiBuildLog.hpp>

namespace mikai::app {
namespace {

constexpr int kStartupTimeoutMs = 20000;
constexpr int kBuildTimeoutMs = 10 * 60 * 1000;
constexpr double kSampleSeconds = 3.0;
constexpr int kBackendLogTail = 3;

class PlaybackText {
    Q_DECLARE_TR_FUNCTIONS(mikai::app::BenchmarkPlayback)
};

struct CurrentContextGuard {
    QOpenGLContext& context;
    ~CurrentContextGuard() { context.doneCurrent(); }
};

}  // namespace

mplayer::native::AnimeJanaiSourceContext readBenchmarkSourceContext(
    const QString& clipPath, QString* error) {
    if (error) error->clear();
    if (backend::WorkCancellationScope::cancellationRequested()) {
        if (error) *error = PlaybackText::tr("Benchmark cancelled.");
        return {};
    }
    try {
        mplayer::MpvHandle handle({{"vo", "null"}, {"audio", "no"}, {"hwdec", "no"},
                                  {"pause", "yes"}, {"config", "no"}});
        bool loaded = false;
        QString failure;
        handle.setEventSink([&](mpv_event* event) {
            if (event == nullptr) return;
            if (event->event_id == MPV_EVENT_FILE_LOADED) loaded = true;
            if (event->event_id == MPV_EVENT_END_FILE && event->data != nullptr) {
                const auto* ended = static_cast<const mpv_event_end_file*>(event->data);
                if (ended->reason == MPV_END_FILE_REASON_ERROR)
                    failure = QString::fromStdString(handle.describeError(ended->error));
            }
        });
        handle.loadFile(clipPath.toStdString());
        QElapsedTimer deadline;
        deadline.start();
        while (!loaded && failure.isEmpty() && deadline.elapsed() < kStartupTimeoutMs &&
               !backend::WorkCancellationScope::cancellationRequested()) {
            handle.drainEvents();
            QThread::msleep(5);
        }
        if (backend::WorkCancellationScope::cancellationRequested()) {
            if (error) *error = PlaybackText::tr("Benchmark cancelled.");
            return {};
        }
        if (!loaded) {
            if (error) *error = failure.isEmpty()
                ? PlaybackText::tr("Timed out reading benchmark source metadata.") : failure;
            return {};
        }
        return {static_cast<int>(handle.getNumber("video-params/w")),
                static_cast<int>(handle.getNumber("video-params/h")),
                handle.getNumber("container-fps")};
    } catch (const mplayer::MpvError& failure) {
        if (error) *error = QString::fromUtf8(failure.what());
        return {};
    }
}

BenchmarkSampleResult runBenchmarkSample(
    const QString& clipPath,
    const QSize& targetSize,
    const std::vector<std::pair<std::string, std::string>>& mpvOptions,
    mplayer::native::Backend backend,
    int slot,
    QOffscreenSurface& surface,
    const std::function<void(const QString&)>& onPhase,
    const std::function<void(BenchmarkEngineBuildUpdate, const QString&)>& onEngine,
    int ajiStageCount,
    const std::atomic<bool>* cancelled) {
    BenchmarkSampleResult result;
    if (backend::WorkCancellationScope::cancellationRequested() ||
        (cancelled != nullptr && cancelled->load(std::memory_order_relaxed))) {
        result.error = PlaybackText::tr("Benchmark cancelled.");
        return result;
    }
    if (clipPath.isEmpty() || targetSize.width() <= 0 || targetSize.height() <= 0) {
        result.error = PlaybackText::tr("Invalid benchmark clip or output size.");
        return result;
    }
    if (!surface.isValid()) {
        result.error = PlaybackText::tr("The offscreen OpenGL surface is unavailable.");
        return result;
    }

    QOpenGLContext context;
    context.setFormat(surface.format());
    if (!context.create() || !context.makeCurrent(&surface)) {
        result.error = PlaybackText::tr("Could not initialize the benchmark OpenGL context.");
        return result;
    }
    // Render resources must be destroyed while their OpenGL context is current.
    CurrentContextGuard currentContext{context};
    QOpenGLFramebufferObjectFormat framebufferFormat;
    framebufferFormat.setInternalTextureFormat(mplayer::kGlInternalFormatRgba8);
    QOpenGLFramebufferObject framebuffer(targetSize, framebufferFormat);
    if (!framebuffer.isValid() || !framebuffer.bind()) {
        result.error = PlaybackText::tr("Could not allocate the benchmark framebuffer.");
        return result;
    }
    QOpenGLFunctions* const gl = context.functions();
    GLint targetDepthBits = 0;
    gl->glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                               0x8212, &targetDepthBits);
    const int targetFormat =
        static_cast<int>(framebuffer.format().internalTextureFormat());
    const int targetDepth = targetDepthBits > 0 ? targetDepthBits : 0;
    QString playbackError;
    QStringList backendLog;
    std::mutex wakeMutex;
    std::condition_variable frameAvailable;
    std::atomic<bool> updatePending{false};
    BenchmarkFrameSample sample;
    mplayer::native::AnimeJanaiBuildGate buildGate;
    int configuredAjiStages = 0;
    const bool watchBuild = slot >= 0 && backend == mplayer::native::Backend::TensorRT;
    QElapsedTimer startupTimer;
    QElapsedTimer buildTimer;
    QElapsedTimer clock;
    clock.start();

    const auto withDetail = [&backendLog](const QString& message) {
        QStringList lines{message};
        lines.append(backendLog);
        return lines.join(QLatin1Char('\n'));
    };
    const auto noteBackendLog = [&backendLog](const QString& text) {
        if (text.isEmpty() || backendLog.contains(text)) return;
        backendLog.append(text);
        while (backendLog.size() > kBackendLogTail) backendLog.removeFirst();
    };
    const auto publishEngine = [&onEngine, &clipPath](BenchmarkEngineBuildUpdate update) {
        if (onEngine) onEngine(update, clipPath);
    };

    try {
        std::vector<std::pair<std::string, std::string>> options;
        options.reserve(mpvOptions.size() + 7);
        options.insert(options.end(), mpvOptions.begin(), mpvOptions.end());
        options.emplace_back("vo", "libmpv");
        options.emplace_back("audio", "no");
        options.emplace_back("cache", "no");
        options.emplace_back("loop-file", "inf");
        options.emplace_back("untimed", "yes");
        options.emplace_back("framedrop", "no");
        options.emplace_back("pause", "no");
        mplayer::MpvHandle handle(options);
        const mplayer::GlApi glApi = mplayer::loadGlApi();
        // The update target is handed over at construction, where mpv raises it
        // once itself: assigning it afterwards would leave a window in which a
        // callback from another thread finds an empty target.
        mplayer::MpvRenderContext renderer(handle, glApi, [&] {
            updatePending.store(true, std::memory_order_release);
            frameAvailable.notify_one();
        });
        renderer.setFlipY(false);
        handle.setEventSink([&](mpv_event* event) {
            if (event == nullptr || event->data == nullptr) return;
            if (event->event_id == MPV_EVENT_END_FILE) {
                const auto* ended = static_cast<const mpv_event_end_file*>(event->data);
                if (ended->reason == MPV_END_FILE_REASON_ERROR) {
                    if (watchBuild && buildGate.buildActive()) {
                        buildGate.fail();
                        publishEngine(BenchmarkEngineBuildUpdate::Failed);
                    } else {
                        playbackError = withDetail(PlaybackText::tr("Playback failed: %1")
                                                       .arg(QString::fromStdString(
                                                           handle.describeError(ended->error))));
                    }
                }
                return;
            }
            if (event->event_id != MPV_EVENT_LOG_MESSAGE) return;
            const auto* message = static_cast<const mpv_event_log_message*>(event->data);
            const QString line = QString::fromUtf8(message->text ? message->text : "").trimmed();
            if (line.isEmpty()) return;
            const QString prefix = QString::fromUtf8(message->prefix ? message->prefix : "");
            if (!watchBuild) {
                if (message->log_level > 0 && message->log_level <= MPV_LOG_LEVEL_ERROR) {
                    noteBackendLog(prefix + QStringLiteral(": ") + line);
                }
                return;
            }
            const bool wasBuilding = buildGate.buildActive();
            const auto buildEvent = buildGate.observeLog(prefix, line, backend, slot);
            if (buildEvent == mplayer::native::AnimeJanaiBuildLogEvent::SlotConfigured)
                ++configuredAjiStages;
            const bool authoritativeBuildFailure =
                buildEvent == mplayer::native::AnimeJanaiBuildLogEvent::TensorRtBuildFailed;
            if (authoritativeBuildFailure) {
                const QStringList buildTail = mplayer::native::readAnimeJanaiBuildLogTail(
                    buildGate.failureLogPath(), mplayer::native::nativeRoot());
                if (buildTail.isEmpty()) {
                    noteBackendLog(prefix + QStringLiteral(": ") + line);
                } else {
                    for (const QString& detail : buildTail) noteBackendLog(detail);
                }
                publishEngine(BenchmarkEngineBuildUpdate::Failed);
            } else if (buildGate.failureLogPath().isEmpty() && message->log_level > 0 &&
                       message->log_level <= MPV_LOG_LEVEL_ERROR) {
                // END_FILE can precede its source-error log; freeze only an authoritative builder tail.
                noteBackendLog(prefix + QStringLiteral(": ") + line);
            } else if (buildGate.buildActive()) {
                const QString lowered = line.toLower();
                if (lowered.contains(QLatin1String("error")) ||
                    lowered.contains(QLatin1String("failed")) ||
                    lowered.contains(QLatin1String("not supported")) ||
                    lowered.contains(QLatin1String("no builder")) ||
                    lowered.contains(QLatin1String("out of memory"))) {
                    noteBackendLog(prefix + QStringLiteral(": ") + line);
                }
            }
            if (buildEvent == mplayer::native::AnimeJanaiBuildLogEvent::TensorRtBuildStarted &&
                !wasBuilding) {
                sample.reset();
                buildTimer.start();
                publishEngine(BenchmarkEngineBuildUpdate::Started);
            } else if (buildEvent == mplayer::native::AnimeJanaiBuildLogEvent::SlotConfigured) {
                startupTimer.restart();
                if (wasBuilding) {
                    if (result.enginePrepMs < 0) result.enginePrepMs = 0;
                    result.enginePrepMs += buildTimer.elapsed();
                    publishEngine(BenchmarkEngineBuildUpdate::Ready);
                }
            }
        });
        if (slot >= 0) handle.setBackendLogObservationEnabled(true);
        if (onPhase) onPhase(PlaybackText::tr("Waiting for video output"));
        handle.loadFile(clipPath.toStdString());
        startupTimer.start();

        for (;;) {
            handle.drainEvents();
            if (backend::WorkCancellationScope::cancellationRequested() ||
                (cancelled != nullptr && cancelled->load(std::memory_order_relaxed))) {
                result.error = PlaybackText::tr("Benchmark cancelled.");
                break;
            }
            if (!playbackError.isEmpty()) {
                result.error = playbackError;
                break;
            }
            if (buildGate.failed()) {
                result.error = withDetail(PlaybackText::tr("TensorRT engine build failed."));
                break;
            }
            if (watchBuild && buildGate.buildActive()) {
                if (buildTimer.elapsed() >= kBuildTimeoutMs) {
                    buildGate.fail();
                    publishEngine(BenchmarkEngineBuildUpdate::Failed);
                    result.error = withDetail(PlaybackText::tr("TensorRT engine build timed out."));
                    break;
                }
            } else if (!sample.started() && startupTimer.elapsed() >= kStartupTimeoutMs) {
                result.error = withDetail(
                    watchBuild && !buildGate.readyToMeasure()
                        ? PlaybackText::tr("TensorRT did not confirm the selected slot is ready.")
                        : PlaybackText::tr("Timed out waiting for rendered video frames."));
                break;
            }

            updatePending.store(false, std::memory_order_release);
            const uint64_t updateFlags = renderer.update();
            if (updateFlags & MPV_RENDER_UPDATE_FRAME) {
                mpv_render_frame_info frameInfo{};
                if (!renderer.nextFrameInfo(frameInfo)) {
                    result.error = PlaybackText::tr("Could not read rendered-frame information.");
                    break;
                }
                gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.handle());
                const int renderError = renderer.render(static_cast<int>(framebuffer.handle()),
                                                        targetSize.width(), targetSize.height(),
                                                        targetFormat, targetDepth);
                if (renderError < 0) {
                    result.error = withDetail(
                        PlaybackText::tr("Rendering failed: %1")
                            .arg(QString::fromStdString(handle.describeError(renderError))));
                    break;
                }
                gl->glFinish();
                renderer.reportSwap();
                const GLenum glError = gl->glGetError();
                if (glError != GL_NO_ERROR) {
                    result.error = PlaybackText::tr("OpenGL rendering failed (0x%1).")
                                       .arg(static_cast<quint32>(glError), 0, 16);
                    break;
                }
                const bool eligible = !watchBuild ||
                    (buildGate.readyToMeasure() &&
                     (ajiStageCount <= 1 || configuredAjiStages >= ajiStageCount));
                if (eligible && mplayer::isRealNewFrame(frameInfo)) {
                    const bool startingSample = !sample.started();
                    sample.recordFrame(clock.nsecsElapsed());
                    if (startingSample && onPhase) {
                        onPhase(PlaybackText::tr("Measuring throughput"));
                    }
                }
            }
            if (sample.started() && sample.seconds(clock.nsecsElapsed()) >= kSampleSeconds) {
                result.measuredFrames = sample.frames;
                result.sampleSeconds = sample.seconds(clock.nsecsElapsed());
                if (sample.frames == 0) {
                    result.error =
                        PlaybackText::tr("No new video frames were rendered during measurement.");
                } else {
                    result.fps = sample.frames / result.sampleSeconds;
                }
                break;
            }
            if (!(updateFlags & MPV_RENDER_UPDATE_FRAME)) {
                std::unique_lock lock(wakeMutex);
                frameAvailable.wait_for(lock, std::chrono::milliseconds(5), [&] {
                    return updatePending.load(std::memory_order_acquire);
                });
            }
        }
    } catch (const mplayer::MpvError& error) {
        result.error = withDetail(PlaybackText::tr("mpv: %1").arg(QString::fromUtf8(error.what())));
    } catch (const std::exception& error) {
        result.error = withDetail(
            PlaybackText::tr("Benchmark failed: %1").arg(QString::fromUtf8(error.what())));
    }
    return result;
}

}  // namespace mikai::app
