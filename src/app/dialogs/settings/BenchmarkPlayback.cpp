#include "BenchmarkPlayback.hpp"

#include "MpvHandle.hpp"
#include "MpvRenderContext.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>

#include <native/components/AnimeJanaiBuildLog.hpp>

namespace mikai::app {
namespace {

constexpr int kStartupTimeoutMs = 20000;
constexpr int kBuildTimeoutMs = 10 * 60 * 1000;
constexpr double kSampleSeconds = 3.0;
constexpr int kBackendLogTail = 4;

class PlaybackText {
    Q_DECLARE_TR_FUNCTIONS(mikai::app::BenchmarkPlayback)
};

struct CurrentContextGuard {
    QOpenGLContext& context;
    ~CurrentContextGuard() { context.doneCurrent(); }
};

}  // namespace

BenchmarkSampleResult runBenchmarkSample(
    const QString& clipPath,
    const QSize& targetSize,
    const std::vector<std::pair<std::string, std::string>>& mpvOptions,
    mplayer::native::Backend backend,
    int slot,
    QOffscreenSurface& surface,
    const std::function<void(const QString&)>& onPhase,
    const std::function<void(BenchmarkEngineBuildUpdate, const QString&)>& onEngine) {
    BenchmarkSampleResult result;
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
    QOpenGLFramebufferObject framebuffer(targetSize);
    if (!framebuffer.isValid()) {
        result.error = PlaybackText::tr("Could not allocate the benchmark framebuffer.");
        return result;
    }
    QOpenGLFunctions* const gl = context.functions();
    QString playbackError;
    QStringList backendLog;
    std::mutex wakeMutex;
    std::condition_variable frameAvailable;
    std::atomic<bool> updatePending{false};
    BenchmarkFrameSample sample;
    mplayer::native::AnimeJanaiBuildGate buildGate;
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
        mplayer::MpvRenderContext renderer(handle, glApi);
        renderer.setFlipY(false);
        renderer.setUpdateCallback([&] {
            updatePending.store(true, std::memory_order_release);
            frameAvailable.notify_one();
        });
        handle.setEventSink([&](mpv_event* event) {
            if (event == nullptr || event->data == nullptr) return;
            if (event->event_id == MPV_EVENT_END_FILE) {
                const auto* ended = static_cast<const mpv_event_end_file*>(event->data);
                if (ended->reason == MPV_END_FILE_REASON_ERROR) {
                    playbackError = withDetail(PlaybackText::tr("Playback failed: %1")
                                                   .arg(QString::fromStdString(
                                                       handle.describeError(ended->error))));
                }
                return;
            }
            if (event->event_id != MPV_EVENT_LOG_MESSAGE) return;
            const auto* message = static_cast<const mpv_event_log_message*>(event->data);
            const QString line = QString::fromUtf8(message->text ? message->text : "").trimmed();
            const QString prefix = QString::fromUtf8(message->prefix ? message->prefix : "");
            if (message->log_level > 0 && message->log_level <= MPV_LOG_LEVEL_ERROR) {
                noteBackendLog(line);
                if (buildGate.buildActive()) {
                    buildGate.fail();
                    publishEngine(BenchmarkEngineBuildUpdate::Failed);
                }
                return;
            }
            if (watchBuild && buildGate.buildActive()) {
                const QString lowered = line.toLower();
                if (lowered.contains(QLatin1String("error")) ||
                    lowered.contains(QLatin1String("failed")) ||
                    lowered.contains(QLatin1String("not supported")) ||
                    lowered.contains(QLatin1String("no builder")) ||
                    lowered.contains(QLatin1String("out of memory"))) {
                    noteBackendLog(prefix + QStringLiteral(": ") + line);
                }
            }
            if (!watchBuild) return;
            const bool wasBuilding = buildGate.buildActive();
            const auto buildEvent = buildGate.observeLog(prefix, line, backend, slot);
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
                                                        targetSize.width(), targetSize.height(), 0);
                if (renderError < 0) {
                    result.error = withDetail(PlaybackText::tr("Rendering failed: %1")
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
                const bool eligible = !watchBuild || buildGate.readyToMeasure();
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
                    result.error = PlaybackText::tr("No new video frames were rendered during measurement.");
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
        result.error = withDetail(PlaybackText::tr("Benchmark failed: %1").arg(QString::fromUtf8(error.what())));
    }
    return result;
}

}  // namespace mikai::app
