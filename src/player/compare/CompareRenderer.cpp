#include "CompareRenderer.hpp"

#include "Anime4K.hpp"
#include "MpvHandle.hpp"
#include "MpvRenderContext.hpp"
#include "native/components/AnimeJanaiBuildLog.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"
#include "native/components/NativePipelineSpec.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QTemporaryDir>
#include <QStringList>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace mplayer::shell {

namespace {
constexpr int kPollMs = 40;
/// A frame-step or a restart is answered in milliseconds; polling at the
/// still-render cadence would multiply that by the step count.
constexpr int kFrameWaitMs = 5;
constexpr int kSourceTimeoutMs = 20000;
constexpr int kStepTimeoutMs = 10000;
constexpr int kBuildTimeoutMs = 20 * 60 * 1000;
/// Steps allowed past the computed point before a replay is declared stalled.
constexpr int kMaxExtraSteps = 8;

struct CommandResult {
    bool finished = false;
    int error = 0;
};

/// Failures that mean the stage is gone or its output cannot be displayed.
/// Everything else is diagnostic text: the fork logs ordinary progress at error
/// level during a build, and aborting on any line that says "error" killed
/// renders that were working.
bool isFatalComparisonLog(const QString& prefix, const QString& text) {
    if (prefix == QLatin1String("vf") || prefix.contains(QLatin1String("aji"))) {
        return text.contains(QLatin1String("because it has failed")) ||
               text.contains(QLatin1String("Cannot convert decoder/filter output")) ||
               text.contains(QLatin1String("Failed to create inference context")) ||
               text.contains(QLatin1String("engine set up failed"), Qt::CaseInsensitive) ||
               text.contains(QLatin1String("failed to load engine"), Qt::CaseInsensitive);
    }
    return text.contains(QLatin1String("Cannot convert decoder/filter output"));
}

// GL objects are allocated and released through the driver while mpv's render
// context and the framebuffer object go, so the context is bound only for that.
class ScopedCurrent {
public:
    ScopedCurrent(QOpenGLContext& context, QSurface* surface)
        : context_(context), previous_(QOpenGLContext::currentContext()),
          previousSurface_(previous_ != nullptr ? previous_->surface() : nullptr),
          bound_(context.makeCurrent(surface)) {
        if (!bound_)
            qWarning() << "CompareRenderer: could not bind the offscreen OpenGL context";
    }

    ~ScopedCurrent() {
        if (!bound_) return;
        if (previous_ == &context_ && previousSurface_ == context_.surface()) return;
        if (previous_ != nullptr && previousSurface_ != nullptr) {
            if (previous_->makeCurrent(previousSurface_)) return;
            qWarning() << "CompareRenderer: could not restore the prior OpenGL context";
        }
        context_.doneCurrent();
    }

    explicit operator bool() const noexcept { return bound_; }

private:
    QOpenGLContext& context_;
    // The surface is captured before the bind: surface() answers for a context
    // that is current, and ours displaces it.
    QOpenGLContext* previous_;
    QSurface* previousSurface_;
    bool bound_;
};
}

struct CompareRenderer::State {
    QOpenGLContext context;
    QOffscreenSurface target;
    QTemporaryDir files;
    std::unique_ptr<QOpenGLFramebufferObject> framebuffer;
    std::unique_ptr<QOpenGLFramebufferObject> upscaleFramebuffer;
    std::shared_ptr<MpvHandle> core;
    std::unique_ptr<MpvRenderContext> rendering;
    QImage sourceFrame;
    QSize targetOutputSize;
    QString sourceUrl;
    double sourceTimestamp = 0.0;
    QString error;
    QString framePath;
    QList<QPair<QString, QString>> stillOptions;
    QString buildDetail;
    bool restarted = false;
    bool processingObserved = false;
    bool acceptBuildEvidence = false;
    bool backendSubstituted = false;
    bool useUpscaleTarget = false;
    int requestedSlot = 0;
    native::Backend requestedBackend = native::Backend::TensorRT;
    native::AnimeJanaiBuildGate buildGate;
    int configuredStages = 0;
    QStringList diagnosticTail;
    int ajiPollIndex = 0;
    QStringList ajiLabels;
    std::shared_ptr<native::AnimeJanaiStageBuildLease> stageBuildLease;

    // ── native replay ────────────────────────────────────────────────────
    /// Real source facts the replay needs: its coded geometry, its cadence and
    /// whether the decoder it opened actually feeds the backend.
    QSize sourceCodedSize;
    double sourceFrameDuration = 0.0;
    /// The last picture mpv handed us. Only a pump that consumed a frame
    /// replaces it, so a paused core is never re-read as fresh delivery.
    QImage lastFrame;
    /// The same-canvas picture of the last native render with its stage
    /// removed: the only honest baseline for a model's effect.
    QImage baselineFrame;
    /// Set by mpv's update callback on its own thread; only read as a hint by
    /// the poll, which never depends on it for correctness.
    std::atomic<bool> framePending{false};
    quint64 presentedFrames = 0;
    quint64 updateCalls = 0;
    quint64 updateFrameFlags = 0;
    QString lastHwdec;
    QString lastStageInput;
    QSize lastVoOutputSize;
    native::AnimeJanaiConfiguredSlot configuredSlot;
    /// The FBO geometry the last replay drew into.
    QSize canvasSize;
    /// Evidence of the last staged replay, kept after the baseline pass has
    /// overwritten the live counters.
    QString modelSummary;
    /// The frame mpv presented for the last staged pass, read through
    /// `screenshot-raw` (bgr0) before the baseline pass replaced it.
    QImage modelVoFrame;
    /// Bumped by stop(): a nested wait loop that outlives its renderer stops
    /// instead of touching a torn-down core. The loops pump processEvents, so a
    /// close or a second render can otherwise re-enter this state.
    quint64 generation = 1;
    int replaySteps = 0;

    ~State() {
        // State dies on CompareWindow's GUI thread, so the guard makes the
        // context current for the GL releases and drops it immediately after.
        const ScopedCurrent current(context, &target);
        rendering.reset();
        upscaleFramebuffer.reset();
        framebuffer.reset();
        core.reset();
    }

    /// Writes the captured frame as raw RGB and points the core at it. The pinned
    /// runtime has no PNG decoder, so an image file is not something this core
    /// can open; rawvideo is.
    ///
    /// The bytes go out on a pool thread: one 4K frame is 24.9 MB, and writing
    /// it on the GUI thread freezes the window and every nested event loop the
    /// renderer is pumping. The caller joins the write, so nothing reads the
    /// file before it is complete.
    bool prepareStillSource(const std::function<bool()>& cancelled) {
        const QImage rgb = sourceFrame.convertToFormat(QImage::Format_RGB888);
        if (rgb.isNull() || rgb.width() <= 0 || rgb.height() <= 0) {
            error = QCoreApplication::translate("CompareRenderer",
                                                "The comparison source frame is unreadable.");
            return false;
        }
        framePath = files.filePath(QStringLiteral("source.rgb"));
        // One frame is the whole picture: the core repeats it with a loop
        // filter, so extra copies are bytes nobody reads.
        struct StillWrite {
            QImage image;
            QString path;
            std::atomic<bool> cancelled{false};
            std::atomic<bool> done{false};
            std::atomic<bool> failed{false};
        };
        auto write = std::make_shared<StillWrite>();
        write->image = rgb;
        write->path = framePath;
        // P1-10: the worker captures only the write state. The GUI-owned
        // cancellation callback must never run on a pool thread: the GUI side
        // evaluates it while pumping below and stores the result in
        // write->cancelled (release), which the worker reads (acquire) at row
        // boundaries.
        QThreadPool::globalInstance()->start(QRunnable::create([write] {
            QFile file(write->path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                write->failed.store(true, std::memory_order_release);
                write->done.store(true, std::memory_order_release);
                return;
            }
            const int rowBytes = write->image.width() * 3;
            for (int y = 0; y < write->image.height(); ++y) {
                if (write->cancelled.load(std::memory_order_acquire)) {
                    break;
                }
                if (file.write(reinterpret_cast<const char*>(
                                   write->image.constScanLine(y)),
                               rowBytes) != rowBytes) {
                    write->failed.store(true, std::memory_order_release);
                    break;
                }
            }
            file.close();
            write->done.store(true, std::memory_order_release);
        }));
        // Events are still pumped: a close or a second render arriving while the
        // write runs must unwind this one.
        const quint64 startedIn = generation;
        while (!write->done.load(std::memory_order_acquire)) {
            QCoreApplication::processEvents();
            if (generation != startedIn || (cancelled && cancelled())) {
                write->cancelled.store(true, std::memory_order_release);
                QThreadPool::globalInstance()->waitForDone();
                return false;
            }
            QThread::msleep(1);
        }
        if (write->failed.load(std::memory_order_acquire) ||
            write->cancelled.load(std::memory_order_acquire)) {
            if (generation == startedIn) {
                error = QCoreApplication::translate(
                    "CompareRenderer", "The comparison source frame could not be written.");
            }
            return false;
        }
        // mpv splits this option on commas, not colons; colon-joined pairs are
        // read as one unknown value and the demuxer falls back to yuv420p, which
        // turns the still into coloured noise.
        const QString pixelFormat =
            QStringLiteral("video_size=%1x%2,pixel_format=rgb24,framerate=%3")
                .arg(rgb.width())
                .arg(rgb.height())
                .arg(sourceFrameDuration > 0.0 ? 1.0 / sourceFrameDuration : 25.0, 0, 'f', 6);
        // Per-file options, not global ones: a failed native render re-creates
        // the core, and global demuxer settings die with it - the next still
        // then opens the raw file as a real container.
        stillOptions = {{QStringLiteral("demuxer"), QStringLiteral("lavf")},
                        {QStringLiteral("demuxer-lavf-format"), QStringLiteral("rawvideo")},
                        {QStringLiteral("demuxer-lavf-o"), pixelFormat}};
        return true;
    }

    /// Loads the still with its demuxer selection as per-file options, so a
    /// re-created core cannot leave the next still opening as a container.
    bool loadStill(const QString& startText, const Cancelled& cancelled) {
        QList<QPair<QString, QString>> options = stillOptions;
        if (!startText.isEmpty()) options.append({QStringLiteral("start"), startText});
        return loadWithOptions(framePath, options, cancelled);
    }

    bool initialize(const QSize& size, double timestamp) {
        if (!files.isValid() || size.isEmpty() || !std::isfinite(timestamp)) {
            error = QCoreApplication::translate("CompareRenderer", "The comparison frame storage or geometry is unavailable.");
            return false;
        }
        targetOutputSize = size;
        if (!context.isValid()) {
            context.setFormat(QSurfaceFormat::defaultFormat());
            if (!context.create()) {
                error = QCoreApplication::translate("CompareRenderer", "The comparison OpenGL context could not be created.");
                return false;
            }
            target.setFormat(context.format());
            target.create();
        }
        const ScopedCurrent current(context, &target);
        if (!current || !target.isValid()) {
            error = QCoreApplication::translate("CompareRenderer", "The comparison OpenGL context is unavailable.");
            return false;
        }
        if (!framebuffer || framebuffer->size() != size) {
            QOpenGLFramebufferObjectFormat format;
            format.setInternalTextureFormat(kGlInternalFormatRgba8);
            framebuffer = std::make_unique<QOpenGLFramebufferObject>(size, format);
        }
        if (!framebuffer->isValid()) {
            error = QCoreApplication::translate("CompareRenderer", "The comparison framebuffer could not be created.");
            return false;
        }
        if (!core) {
            const std::string startTimestamp = mpvNumber(timestamp);
            core = std::make_shared<MpvHandle>(
                std::vector<std::pair<std::string, std::string>>{
                    {"vo", "libmpv"}, {"hwdec", "auto-safe"}, {"pause", "yes"},
                    // Qt owns presentation: an offscreen FBO never supplies the
                    // vsync timing the video output would otherwise wait for, so
                    // a frame-paced core delivers the first picture and then
                    // stalls - which a buffered native stage reads as "no input".
                    {"video-timing-offset", "0"},
                    {"audio", "no"}, {"sub", "no"}, {"keep-open", "yes"},
                    {"image-display-duration", "inf"}, {"screenshot-sw", "yes"},
                    {"screenshot-format", "png"}, {"input-default-bindings", "no"},
                    {"start", startTimestamp},
                    {"http-header-fields", "Accept-Language: uk-UA"},
                    {"user-agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/142.0.0.0 Safari/537.36"}});
            core->setBackendLogObservationEnabled(true);
            core->setEventSink([this](mpv_event* event) {
                if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) restarted = true;
                if (event->event_id == MPV_EVENT_END_FILE && event->data != nullptr) {
                    const auto* ended = static_cast<mpv_event_end_file*>(event->data);
                    if (ended->reason == MPV_END_FILE_REASON_ERROR) {
                        const QString tail = diagnosticTail.join(QStringLiteral(" · "));
                        error = QCoreApplication::translate("CompareRenderer", "The comparison source could not be loaded: %1")
                                    .arg(QString::fromStdString(core->describeError(ended->error))) +
                                (tail.isEmpty() ? QString()
                                                : QStringLiteral(" — ") + tail);
                    }
                }
                if (event->event_id != MPV_EVENT_LOG_MESSAGE || event->data == nullptr) return;
                const auto* message = static_cast<mpv_event_log_message*>(event->data);
                const QString text = QString::fromUtf8(message->text).trimmed();
                const QString prefix = QString::fromUtf8(message->prefix);
                if (message->log_level > 0 && message->log_level <= MPV_LOG_LEVEL_ERROR) {
                    if (!diagnosticTail.contains(text)) {
                        diagnosticTail.append(text);
                        while (diagnosticTail.size() > 3) diagnosticTail.removeFirst();
                    }
                    // Only a failure that means the stage is gone or the frame
                    // cannot be displayed aborts the render. Matching the word
                    // "error" anywhere killed good renders on messages that
                    // merely mentioned one, and let real failures logged at
                    // another level through.
                    if (acceptBuildEvidence && error.isEmpty() &&
                        isFatalComparisonLog(prefix, text)) {
                        error = QCoreApplication::translate("CompareRenderer",
                                                           "The comparison model failed: %1")
                                    .arg(text);
                    }
                }
                const QString combined = prefix + QLatin1Char(' ') + text;
                const bool isAjiLog =
                    combined.contains(QLatin1String("animejanai"), Qt::CaseInsensitive) ||
                    combined.contains(QLatin1String("mikai_aji"), Qt::CaseInsensitive) ||
                    combined.contains(QLatin1String("Configured slot"), Qt::CaseInsensitive) ||
                    combined.contains(QLatin1String("TensorRT engine"), Qt::CaseInsensitive) ||
                    combined.contains(QLatin1String("tensorrt-rtx"), Qt::CaseInsensitive) ||
                    combined.contains(QLatin1String("building engine:"), Qt::CaseInsensitive);
                if (!isAjiLog) return;
                if (!acceptBuildEvidence) return;
                const auto buildEvent = native::isTensorRtFamily(requestedBackend)
                    ? buildGate.observeLog(prefix, text, requestedBackend, requestedSlot)
                    : native::classifyAnimeJanaiBuildLog(prefix, text, requestedBackend,
                                                         requestedSlot);
                if (buildEvent == native::AnimeJanaiBuildLogEvent::SlotConfigured) {
                    ++configuredStages;
                    if (const auto configured = native::parseAnimeJanaiConfiguredSlot(combined))
                        configuredSlot = *configured;
                    processingObserved =
                        !ajiLabels.isEmpty() && configuredStages >= ajiLabels.size();
                }
                if (buildEvent == native::AnimeJanaiBuildLogEvent::BackendSubstituted) {
                    // The dispatcher kept running on the classic backend after
                    // the requested RTX one failed; the frame would not be the
                    // chosen pipeline's output.
                    backendSubstituted = true;
                    error = QCoreApplication::translate(
                                "CompareRenderer",
                                "TensorRT for RTX did not start and the core fell back to "
                                "TensorRT; the comparison did not run the requested backend: %1")
                                .arg(text);
                }
                if (buildEvent == native::AnimeJanaiBuildLogEvent::TensorRtEngineWarmupFailed) {
                    // The core retries the cached engine next; keep the warm
                    // log's errors for whatever verdict that load produces.
                    const QStringList warmTail = native::readAnimeJanaiBuildLogTail(
                        buildGate.warmupLogPath(), native::nativeRoot());
                    for (const QString& detail : warmTail) {
                        if (!diagnosticTail.contains(detail)) diagnosticTail.append(detail);
                    }
                    while (diagnosticTail.size() > 3) diagnosticTail.removeFirst();
                }
                if (native::animeJanaiEnginePrepEvent(buildEvent)) {
                    const bool preparing =
                        buildEvent == native::AnimeJanaiBuildLogEvent::TensorRtEnginePrepStarted;
                    if (requestedBackend == native::Backend::TensorRTRTX) {
                        buildDetail = preparing
                            ? QCoreApplication::translate("CompareRenderer", "Preparing the comparison TensorRT for RTX engine")
                            : QCoreApplication::translate("CompareRenderer", "Building the comparison TensorRT for RTX engine");
                    } else {
                        buildDetail = preparing
                            ? QCoreApplication::translate("CompareRenderer", "Preparing the comparison TensorRT engine")
                            : QCoreApplication::translate("CompareRenderer", "Building the comparison TensorRT engine");
                    }
                }
                if (buildGate.failed() && !backendSubstituted) {
                    const QStringList tail = native::readAnimeJanaiBuildLogTail(
                        buildGate.failureLogPath(), native::nativeRoot());
                    error = QCoreApplication::translate("CompareRenderer", "The comparison model failed: %1")
                                .arg((tail.isEmpty() ? diagnosticTail : tail).join(QLatin1Char('\n')));
                }
            });
            // Advanced control stays off, which is the mode this offscreen
            // renderer is meant to run in: mpv owns the swap, and nothing here
            // has to answer every callback with report_swap(). The callback only
            // marks that mpv has output; pumpFrame() draws it when update()
            // reports a new frame.
            rendering = std::make_unique<MpvRenderContext>(
                *core, loadGlApi(),
                [this] { framePending.store(true, std::memory_order_release); }, false);
            rendering->setFlipY(true);
        }
        return true;
    }

    bool ensureUpscaleFramebuffer(const QSize& desired) {
        const ScopedCurrent current(context, &target);
        if (!current || !target.isValid()) return false;
        if (!upscaleFramebuffer || upscaleFramebuffer->size() != desired) {
            QOpenGLFramebufferObjectFormat format;
            format.setInternalTextureFormat(kGlInternalFormatRgba8);
            upscaleFramebuffer = std::make_unique<QOpenGLFramebufferObject>(desired, format);
        }
        return upscaleFramebuffer->isValid();
    }

    std::shared_ptr<CommandResult> command(std::vector<std::string> arguments) {
        auto result = std::make_shared<CommandResult>();
        if (!core->commandAsync(std::move(arguments), [result](const AsyncReply& reply) {
                result->error = reply.error;
                result->finished = true;
            }).valid()) {
            result->error = MPV_ERROR_COMMAND;
            result->finished = true;
        }
        return result;
    }

    QOpenGLFramebufferObject* activeTarget() {
        return (useUpscaleTarget && upscaleFramebuffer && upscaleFramebuffer->isValid())
                   ? upscaleFramebuffer.get()
                   : framebuffer.get();
    }

    /// Records what the output and the decoder report for the frame just drawn.
    /// These are the delivery signals a verdict may rest on: `video-out-params`
    /// is set from the frame `write_video()` handed the output, so it names the
    /// geometry of a picture that really left the filter chain.
    void observeDelivery() {
        try {
            lastVoOutputSize = QSize(qRound(core->getNumber("video-out-params/w")),
                                     qRound(core->getNumber("video-out-params/h")));
        } catch (const MpvError&) {
            lastVoOutputSize = {};
        }
        try {
            lastHwdec = QString::fromStdString(core->getString("hwdec-current"));
        } catch (const MpvError&) {
            lastHwdec.clear();
        }
    }

    /// Draws exactly the frame mpv handed over, and nothing otherwise. The
    /// update flag is the delivery signal: a paused core that has no new frame
    /// leaves the previous picture in place, and re-drawing on a timer is what
    /// made every model look identical while the first frame of a freshly
    /// rebuilt stage was still on screen.
    bool pumpFrame() {
        core->drainEvents();
        if (!rendering) return false;
        // Scoped so anything the drain below reaches binds its own surface.
        const ScopedCurrent current(context, &target);
        if (!current) {
            error = QCoreApplication::translate("CompareRenderer",
                                                "The comparison OpenGL context was lost.");
            return false;
        }
        QOpenGLFramebufferObject* activeFbo = activeTarget();
        if (activeFbo == nullptr || !activeFbo->isValid()) {
            error = QCoreApplication::translate(
                "CompareRenderer", "The comparison framebuffer is unavailable.");
            return false;
        }
        ++updateCalls;
        if ((rendering->update() & MPV_RENDER_UPDATE_FRAME) == 0) return false;
        ++updateFrameFlags;
        activeFbo->bind();
        // The same defined black the live surface paints before mpv draws into
        // it, and the same viewport: an FBO that was never cleared keeps the
        // previous frame's pixels around whatever the new picture covers.
        if (QOpenGLFunctions* gl = context.functions()) {
            gl->glViewport(0, 0, activeFbo->width(), activeFbo->height());
            gl->glDisable(GL_SCISSOR_TEST);
            gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            gl->glClear(GL_COLOR_BUFFER_BIT);
        }
        const int targetFormat = static_cast<int>(activeFbo->format().internalTextureFormat());
        const int result = rendering->render(activeFbo->handle(), activeFbo->width(),
                                             activeFbo->height(), targetFormat, 8);
        if (result < 0) {
            error = QCoreApplication::translate("CompareRenderer",
                                                "The comparison renderer failed: %1")
                        .arg(QString::fromStdString(core->describeError(result)));
            return false;
        }
        lastFrame = activeFbo->toImage();
        ++presentedFrames;
        observeDelivery();
        return true;
    }

    /// Polls the delivery path once. The returned image is the last picture mpv
    /// handed over, so a caller that compares consecutive answers still sees a
    /// picture stop changing when no new frame arrives.
    QImage pump() {
        pumpFrame();
        QCoreApplication::processEvents();
        QThread::msleep(kPollMs);
        return lastFrame;
    }

    /// Waits for `after` to be passed by a frame mpv really delivered.
    bool waitPresented(quint64 after, int timeoutMs, const Cancelled& cancelled) {
        const quint64 startedIn = generation;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs && !cancelled() && error.isEmpty()) {
            if (pumpFrame() && presentedFrames > after) return true;
            QCoreApplication::processEvents();
            if (generation != startedIn) return false;
            QThread::msleep(kFrameWaitMs);
        }
        return false;
    }

    double timePos() const {
        try {
            return core->getNumber("time-pos");
        } catch (const MpvError&) {
            return -1.0;
        }
    }

    bool waitForCommand(const std::shared_ptr<CommandResult>& result,
                        const Cancelled& cancelled) {
        QElapsedTimer timer;
        timer.start();
        while (!result->finished && error.isEmpty() && !cancelled() &&
               timer.elapsed() < kSourceTimeoutMs) {
            core->drainEvents();
            if (result->finished || !error.isEmpty() || cancelled()) break;
            {
                const ScopedCurrent current(context, &target);
                if (current && rendering) {
                    rendering->update();
                }
            }
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        if (cancelled()) return false;
        if (!result->finished) {
            if (error.isEmpty()) error = QCoreApplication::translate("CompareRenderer", "The comparison command did not complete.");
            return false;
        }
        if (result->error < 0) {
            error = QCoreApplication::translate("CompareRenderer", "The comparison command failed: %1")
                        .arg(QString::fromStdString(core->describeError(result->error)));
            return false;
        }
        return error.isEmpty();
    }

    /// Reads the last frame handed to the video output through mpv's own
    /// software screenshot path, independent of the framebuffer.
    QImage queryVoFrame();

    bool setOption(const char* name, const QString& value, const Cancelled& cancelled) {
        return waitForCommand(command({"set", name, value.toStdString()}), cancelled);
    }

    /// Loads with options that apply to this file only. Global set/reset pairs
    /// lose the still's demuxer selection the moment the core is re-created,
    /// and the next still then opens as a real container.
    bool loadWithOptions(const QString& path, const QList<QPair<QString, QString>>& options,
                         const Cancelled& cancelled) {
        NodeValue args;
        args.format = MPV_FORMAT_NODE_ARRAY;
        NodeValue name;
        name.format = MPV_FORMAT_STRING;
        name.text = "loadfile";
        NodeValue url;
        url.format = MPV_FORMAT_STRING;
        url.text = path.toStdString();
        NodeValue flags;
        flags.format = MPV_FORMAT_STRING;
        flags.text = "replace";
        NodeValue index;
        index.format = MPV_FORMAT_INT64;
        index.integer = -1;
        NodeValue optionMap;
        optionMap.format = MPV_FORMAT_NODE_MAP;
        for (const auto& option : options) {
            NodeValue value;
            value.format = MPV_FORMAT_STRING;
            value.text = option.second.toStdString();
            optionMap.map.emplace_back(option.first.toStdString(), value);
        }
        args.list = {name, url, flags, index, optionMap};
        auto result = std::make_shared<CommandResult>();
        if (!core->commandNodeAsync(args, [result](const AsyncReply& reply) {
                 result->error = reply.error;
                 result->finished = true;
            }).valid()) {
            result->error = MPV_ERROR_COMMAND;
            result->finished = true;
        }
        return waitForCommand(result, cancelled);
    }

    bool ensureCanvas(const QSize& canvas) {
        if (canvas.isEmpty()) {
            error = QCoreApplication::translate("CompareRenderer",
                                                "The comparison geometry is unavailable.");
            return false;
        }
        useUpscaleTarget = false;
        if (framebuffer && framebuffer->size() == canvas) {
            canvasSize = canvas;
            return framebuffer->isValid();
        }
        const ScopedCurrent current(context, &target);
        if (!current) return false;
        QOpenGLFramebufferObjectFormat format;
        format.setInternalTextureFormat(kGlInternalFormatRgba8);
        framebuffer = std::make_unique<QOpenGLFramebufferObject>(canvas, format);
        if (!framebuffer->isValid()) {
            error = QCoreApplication::translate(
                "CompareRenderer", "The comparison framebuffer could not be created.");
            return false;
        }
        // Nothing drawn before the resize belongs to this canvas.
        lastFrame = {};
        lastVoOutputSize = {};
        canvasSize = canvas;
        return true;
    }

    /// Waits until the fork has configured every stage the plan declares, and
    /// returns the ring depth it reported (or the default when the build log has
    /// no depth suffix).
    bool waitForConfigured(const Cancelled& cancelled, const Progress& progress,
                           int* depth) {
        const quint64 startedIn = generation;
        *depth = native::kAnimeJanaiDefaultRingDepth;
        QElapsedTimer timer;
        timer.start();
        std::shared_ptr<CommandResult> poll;
        QString reported;
        while (timer.elapsed() < kBuildTimeoutMs && !cancelled() && error.isEmpty()) {
            pumpFrame();
            if (processingObserved) {
                *depth = configuredSlot.depth.value_or(native::kAnimeJanaiDefaultRingDepth);
                return true;
            }
            if (error.isEmpty() && !ajiLabels.isEmpty() && (!poll || poll->finished)) {
                const QString label = ajiLabels.at(ajiPollIndex++ % ajiLabels.size());
                poll = command({"vf-command", label.toStdString(), "poll", ""});
            }
            if (!buildDetail.isEmpty() && buildDetail != reported) {
                reported = buildDetail;
                progress(buildDetail);
            }
            QCoreApplication::processEvents();
            if (generation != startedIn) return false;
            QThread::msleep(kFrameWaitMs);
        }
        if (error.isEmpty() && !cancelled() && generation == startedIn) {
            error = QCoreApplication::translate(
                "CompareRenderer",
                "The comparison model was configured but produced no frame; the pipeline "
                "stalled before its output reached the surface.");
        }
        return false;
    }

    /// Replays the held frame through the plan's real pipeline.
    ///
    /// `attachStage` false renders the identical decode, seek and step count
    /// with the stage removed: the only baseline a model's effect may be
    /// measured against. The picture is produced by presenting frames one at a
    /// time, never by reloading the file: a reload destroys the stage instance
    /// that has just configured, and its replacement shows the first frame of an
    /// empty ring - a pass-through that looks exactly like "the model only
    /// changed the colours".
    QImage replay(const native::AnimeJanaiPlan& plan, bool attachStage, double heldPts,
                  const QSize& canvas, const Cancelled& cancelled,
                  const Progress& progress) {
        if (sourceUrl.isEmpty() || !(sourceFrameDuration > 0.0)) {
            error = QCoreApplication::translate(
                "CompareRenderer",
                "The comparison source does not report a frame cadence, so the model "
                "cannot replay it frame by frame.");
            return {};
        }
        const native::NativePipelineSpec spec = native::nativePipelineFor(
            plan, sourceCodedSize.isValid() ? sourceCodedSize : sourceFrame.size());
        // A baseline carries no stage: it keeps the plan's decoder so the two
        // pictures differ only by the model, never by how they were decoded.
        // An unset canvas means "the stage decides": its Configured line names
        // the geometry it converts to, and the plan's own scale factor is only
        // a hint from model metadata.
        QSize target = canvas;
        if (!target.isValid()) {
            // The plan's declared geometry, not the pane's: the canvas must not
            // change once frames are being drawn into it. A hint that disagrees
            // with the stage's Configured line is corrected below, before the
            // first frame of the replay is presented.
            target = spec.expectedOutput.isValid() ? spec.expectedOutput : targetOutputSize;
        }
        if (!ensureCanvas(target)) return {};

        ajiLabels = attachStage ? spec.ajiLabels : QStringList();
        restarted = false;
        processingObserved = false;
        configuredStages = 0;
        configuredSlot = {};
        replaySteps = 0;
        if (attachStage) modelSummary.clear();
        buildGate = {};
        buildDetail.clear();
        diagnosticTail.clear();
        lastHwdec.clear();
        lastVoOutputSize = {};
        acceptBuildEvidence = attachStage && !spec.vf.isEmpty();
        requestedBackend = plan.backend;
        requestedSlot = plan.slot;
        ajiPollIndex = 0;
        if (attachStage) native::preloadAnimeJanaiBackendLibs(plan.backendLibDirs);
        const QString hwdec = spec.hwdec.isEmpty() ? QStringLiteral("auto-safe") : spec.hwdec;
        const QString vf = attachStage ? spec.vf : QString();

        // Loading a few frames early lets the engine warm up on real input while
        // the core is still paused, so no pass-through frame is ever presented.
        const double warmup =
            std::max(0.0, heldPts - double(native::kAnimeJanaiDefaultRingDepth + 1) *
                                   sourceFrameDuration);
        if (!setOption("vf", vf, cancelled) ||
            !setOption("glsl-shaders", QString(), cancelled) ||
            !setOption("hwdec", hwdec, cancelled) ||
            !setOption("pause", QStringLiteral("yes"), cancelled) ||
            !setOption("aid", QStringLiteral("no"), cancelled) ||
            !setOption("sid", QStringLiteral("no"), cancelled) ||
            !setOption("osd-level", QStringLiteral("0"), cancelled) ||
            !setOption("hr-seek", QStringLiteral("yes"), cancelled) ||
            !setOption("keep-open", QStringLiteral("yes"), cancelled) ||
            !setOption("demuxer-lavf-format", QString(), cancelled)) {
            return {};
        }
        const quint64 loadedAt = presentedFrames;
        if (!loadWithOptions(sourceUrl,
                             {{QStringLiteral("start"),
                               QString::fromStdString(mpvNumber(warmup))}},
                             cancelled)) {
            return {};
        }
        if (!waitPresented(loadedAt, kSourceTimeoutMs, cancelled)) {
            if (error.isEmpty()) {
                error = QCoreApplication::translate(
                    "CompareRenderer", "The comparison source produced no frame to replay.");
            }
            return {};
        }
        // The decoder verdict decides whether the stage can receive what it was
        // built for at all; a software frame is not a model result.
        if (!waitForNativeDecoder(plan.backend, cancelled)) return {};

        int depth = 0;
        if (attachStage && !waitForConfigured(cancelled, progress, &depth)) return {};
        if (attachStage && configuredSlot.output.isValid() &&
            configuredSlot.output != canvasSize) {
            // The stage converts to its own geometry: draw into exactly that, so
            // the read-back is the model's picture and not a rescale of it.
            const QSize declared(
                qMin(7680, configuredSlot.output.width()),
                qMin(4320, configuredSlot.output.height()));
            if (!ensureCanvas(declared)) return {};
        }
        const double preroll =
            std::max(0.0, heldPts - double(depth + 1) * sourceFrameDuration);
        if (attachStage && preroll <= 0.0 && heldPts <= (depth + 1) * sourceFrameDuration) {
            // The ring cannot be filled before the first frame of the file; say
            // so instead of presenting a warm-up frame.
            diagnosticTail.append(QCoreApplication::translate(
                "CompareRenderer",
                "The held frame is too close to the start of the file for the model's "
                "input buffer, so the replay starts at the first frame."));
        }
        const quint64 seekedAt = presentedFrames;
        core->seekExact(preroll);
        // A seek to the position already held may be answered without a restart;
        // the following steps are what prove the replay is advancing.
        (void)waitPresented(seekedAt, kSourceTimeoutMs, cancelled);
        if (!error.isEmpty()) return {};

        const int required =
            std::max(1, int(std::lround((heldPts - preroll) / sourceFrameDuration)));
        bool reached = false;
        for (int step = 1; step <= required + kMaxExtraSteps && !cancelled() && error.isEmpty();
             ++step) {
            const quint64 before = presentedFrames;
            if (!waitForCommand(command({"frame-step"}), cancelled)) return {};
            if (!waitPresented(before, kStepTimeoutMs, cancelled)) {
                if (error.isEmpty()) {
                    error = QCoreApplication::translate(
                        "CompareRenderer",
                        "A processed frame reached the output but was never presented.");
                }
                return {};
            }
            ++replaySteps;
            if (step >= required &&
                timePos() >= heldPts - 0.5 * sourceFrameDuration) {
                reached = true;
                break;
            }
        }
        if (cancelled()) return {};
        if (!reached) {
            if (error.isEmpty()) {
                error = QCoreApplication::translate(
                    "CompareRenderer",
                    "A processed frame reached the output but was never presented.");
            }
            return {};
        }
        if (attachStage && configuredSlot.output.isValid() && lastVoOutputSize.isValid() &&
            lastVoOutputSize != configuredSlot.output) {
            error = QCoreApplication::translate(
                        "CompareRenderer",
                        "The comparison model was configured for %1x%2, but the delivered "
                        "picture is %3x%4.")
                        .arg(configuredSlot.output.width())
                        .arg(configuredSlot.output.height())
                        .arg(lastVoOutputSize.width())
                        .arg(lastVoOutputSize.height());
            return {};
        }
        if (attachStage) {
            modelSummary = QStringLiteral("hwdec=%1 vo=%2x%3 canvas=%4x%5 steps=%6 depth=%7")
                               .arg(lastHwdec.isEmpty() ? QStringLiteral("none") : lastHwdec)
                               .arg(lastVoOutputSize.width())
                               .arg(lastVoOutputSize.height())
                               .arg(canvasSize.width())
                               .arg(canvasSize.height())
                               .arg(replaySteps)
                               .arg(depth);
        }
        if (qEnvironmentVariableIsSet("MIKAI_COMPARE_TRACE")) {
            std::fprintf(stderr,
                         "[compare] replay stage=%d hwdec=%s vo=%dx%d canvas=%dx%d "
                         "presented=%llu steps=%d depth=%d pos=%.3f vf=%s\n",
                         attachStage ? 1 : 0, qPrintable(lastHwdec),
                         lastVoOutputSize.width(), lastVoOutputSize.height(),
                         canvasSize.width(), canvasSize.height(),
                         static_cast<unsigned long long>(presentedFrames), replaySteps, depth,
                         timePos(), qPrintable(vf));
        }
        return lastFrame;
    }

    /// Reads the decoder the core actually opened. `hwdec` only echoes the
    /// request; `hwdec-current` is the method that ran, which is what decides
    /// whether the stage receives device frames.
    bool waitForNativeDecoder(native::Backend backend, const Cancelled& cancelled) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 4000 && !cancelled() && error.isEmpty()) {
            if (core) {
                try {
                    lastHwdec = QString::fromStdString(core->getString("hwdec-current"));
                } catch (const MpvError&) {
                    lastHwdec.clear();
                }
            }
            if (!lastHwdec.isEmpty()) break;
            pumpFrame();
            QCoreApplication::processEvents();
            QThread::msleep(kFrameWaitMs);
        }
        if (cancelled()) return false;
        if (native::decoderFeedsNativeBackend(lastHwdec, backend)) return true;
        error = QCoreApplication::translate(
                    "CompareRenderer",
                    "Hardware decoding is unavailable, so the comparison cannot run the "
                    "same pipeline as the player: the core reports the decoder '%1'.")
                    .arg(lastHwdec.isEmpty() ? QStringLiteral("none") : lastHwdec);
        return false;
    }

    QImage renderNative(const native::AnimeJanaiPlan& plan, const Cancelled& cancelled,
                        const Progress& progress) {
        const double heldPts = sourceTimestamp;
        baselineFrame = {};
        // One canvas for both passes: whatever geometry the stage declared, so
        // the baseline is drawn by the same scaler into the same size and the
        // gate compares like with like.
        modelVoFrame = {};
        QImage model = replay(plan, true, heldPts, QSize(), cancelled, progress);
        if (model.isNull() || cancelled()) return model;
        // The frame mpv handed to the output for this pass, read before the
        // baseline replaces it: proof that the read-back is the delivered frame.
        modelVoFrame = queryVoFrame();
        const QSize canvas = canvasSize;
        // The baseline is rendered after the model, with the stage removed and
        // everything else identical.
        baselineFrame = replay(plan, false, heldPts, canvas, cancelled, progress);
        if (baselineFrame.isNull()) {
            // The model picture is still a result; there is simply nothing
            // honest to measure it against, and the gate says so.
            const QString baselineError = error;
            error.clear();
            diagnosticTail.append(baselineError);
        }
        return model;
    }

    /// Waits until the core has restart frame and stops changing. Only the
    /// still path uses this: a native plan is replayed frame by frame, where a
    /// picture that stops changing is a stalled stage, not a settled one.
    QImage settle(bool requireRestart, const Cancelled& cancelled, const Progress& progress) {
        QElapsedTimer timer;
        timer.start();
        QImage previous;
        QString reported;
        while (timer.elapsed() < kSourceTimeoutMs && error.isEmpty() && !cancelled()) {
            const QImage image = pump();
            if (cancelled() || !error.isEmpty()) return {};
            if (!buildDetail.isEmpty() && buildDetail != reported) {
                reported = buildDetail;
                progress(buildDetail);
            }
            if (timer.elapsed() >= 200 && (!requireRestart || restarted) && !image.isNull() &&
                image == previous) {
                return image;
            }
            previous = image;
        }
        if (!cancelled() && error.isEmpty()) {
            error = QCoreApplication::translate("CompareRenderer",
                                                "The comparison output did not settle.");
            if (!diagnosticTail.isEmpty())
                error += QLatin1Char('\n') + diagnosticTail.join(QLatin1Char('\n'));
        }
        return {};
    }
};

CompareRenderer::CompareRenderer() : state_(std::make_unique<State>()) {}
CompareRenderer::~CompareRenderer() = default;

bool CompareRenderer::capture(const QString& source, double timestamp, const QSize& outputSize,
                              const Cancelled& cancelled, const Progress& progress,
                              const std::function<QImage()>& fallbackProvider,
                              QImage* capturedSource) {
    State& state = *state_;
    state.error.clear();
    state.sourceFrame = {};
    state.sourceUrl = source;
    state.sourceTimestamp = timestamp;
    state.buildDetail.clear();
    state.useUpscaleTarget = false;
    try {
        if (!state.initialize(outputSize, timestamp) || cancelled()) return false;
        // The still's demuxer selection is a per-file option now, so it cannot
        // follow the core into a real source. Only the start position is global.
        const std::string startTimestamp = mpvNumber(timestamp);
        if (!state.waitForCommand(state.command({"set", "vf", ""}), cancelled) ||
            !state.waitForCommand(state.command({"set", "glsl-shaders", ""}), cancelled) ||
            !state.waitForCommand(state.command({"set", "start", startTimestamp}), cancelled)) return false;
        state.restarted = false;
        state.core->loadFile(source.toStdString());
        QImage settledSource = state.settle(true, cancelled, progress);
        if (!settledSource.isNull()) {
            const int nativeWidth = qRound(state.core->getNumber("video-params/w"));
            const int nativeHeight = qRound(state.core->getNumber("video-params/h"));
            state.sourceCodedSize = QSize(nativeWidth, nativeHeight);
            // The container's cadence is what the replay steps by. `container-fps`
            // is a container fact and stays available on a paused core;
            // `estimated-vf-fps` only exists once the chain produced output.
            double fps = 0.0;
            try {
                fps = state.core->getNumber("container-fps");
            } catch (const MpvError&) {
                fps = 0.0;
            }
            if (!(fps > 0.0)) {
                try {
                    fps = state.core->getNumber("estimated-vf-fps");
                } catch (const MpvError&) {
                    fps = 0.0;
                }
            }
            state.sourceFrameDuration = fps > 0.0 ? 1.0 / fps : 0.0;
            if (nativeWidth <= 0 || nativeHeight <= 0) {
                state.error = QCoreApplication::translate(
                    "CompareRenderer", "The native comparison source geometry is unavailable.");
                return false;
            }
            if (state.framebuffer->size() != QSize(nativeWidth, nativeHeight)) {
                const ScopedCurrent current(state.context, &state.target);
                if (!current) return false;
                QOpenGLFramebufferObjectFormat format;
                format.setInternalTextureFormat(kGlInternalFormatRgba8);
                state.framebuffer = std::make_unique<QOpenGLFramebufferObject>(
                    QSize(nativeWidth, nativeHeight), format);
                if (!state.framebuffer->isValid()) {
                    state.error = QCoreApplication::translate(
                        "CompareRenderer", "The native comparison framebuffer could not be created.");
                    return false;
                }
                // Rerender the same decoded frame into its native geometry. The first
                // display-sized render is used only to settle source loading.
                state.restarted = false;
                state.core->seek(timestamp);
                settledSource = state.settle(false, cancelled, progress);
            }
            if (settledSource.isNull()) return false;
            state.sourceFrame = settledSource;
        } else {
            // The source refused a second decode; only now is the playing
            // surface asked for its untouched frame.
            const QImage fallback = fallbackProvider && !cancelled() ? fallbackProvider()
                                                                    : QImage();
            if (fallback.isNull() || cancelled()) return false;
            state.error.clear();
            state.diagnosticTail.clear();
            state.sourceFrame = fallback;
        }
        // The still goes back into the core for the candidate render, and the
        // pinned runtime ships no image codecs: a PNG round-trip through mpv
        // fails there with "Could not open libavcodec encoder for saving
        // images". Raw RGB is a format this runtime does decode.
        if (!state.prepareStillSource(cancelled)) return false;
        state.restarted = false;
        if (!state.loadStill(QString(), cancelled)) return false;
        const bool settled = !state.settle(true, cancelled, progress).isNull();
        if (settled) {
            state.error.clear();
            if (capturedSource != nullptr) *capturedSource = state.sourceFrame;
        }
        return settled;
    } catch (const MpvError& error) {
        state.error = QCoreApplication::translate("CompareRenderer", "The comparison renderer failed: %1").arg(QString::fromUtf8(error.what()));
        return false;
    }
}

QImage CompareRenderer::render(const QString& shaderId, const native::AnimeJanaiPlan& plan,
                               const Cancelled& cancelled, const Progress& progress) {
    State& state = *state_;
    state.error.clear();
    state.buildDetail.clear();
    state.useUpscaleTarget = false;
    if (state.sourceFrame.isNull() || cancelled()) return {};
    try {
        if (!state.framebuffer) {
            state.error = QCoreApplication::translate(
                "CompareRenderer", "The comparison framebuffer is unavailable.");
            return {};
        }
        if (!state.core && !state.initialize(state.framebuffer->size(), 0.0)) return {};
        const QString filter = plan.enabled ? QString::fromUtf8(plan.filterSpec) : QString();
        const QString shadersDir = anime4k::findShadersDir();
        const QString chain =
            plan.enabled ? QString() : anime4k::shaderStringForCandidate(shaderId, shadersDir);
        if (!plan.enabled && shaderId != QLatin1String("off") && chain.isEmpty()) {
            state.error = QCoreApplication::translate(
                              "CompareRenderer",
                              "The shader chain for %1 could not be resolved.")
                              .arg(shaderId);
            return {};
        }
        // Restore the requested output before deciding whether Anime4K needs an internal 2x target.
        if (state.framebuffer->size() != state.targetOutputSize) {
            const ScopedCurrent current(state.context, &state.target);
            if (!current) return {};
            QOpenGLFramebufferObjectFormat format;
            format.setInternalTextureFormat(kGlInternalFormatRgba8);
            state.framebuffer = std::make_unique<QOpenGLFramebufferObject>(
                state.targetOutputSize, format);
            if (!state.framebuffer->isValid()) {
                state.error = QCoreApplication::translate(
                    "CompareRenderer", "The comparison output framebuffer could not be created.");
                return {};
            }
        }

        int planScale = 1;
        if (plan.enabled) {
            for (const auto& stage : plan.stages) {
                if (stage.scaleFactor > planScale) planScale = stage.scaleFactor;
            }
        }
        const bool a4kUpscale = !plan.enabled && chain.contains(QLatin1String("Upscale"), Qt::CaseInsensitive);
        bool upscaleCapped = false;
        if ((plan.enabled && planScale > 1) || a4kUpscale) {
            const int scale = plan.enabled ? planScale : 2;
            const QSize upscaleSize(
                std::min(7680, std::max(state.framebuffer->width(), state.sourceFrame.width() * scale)),
                std::min(4320, std::max(state.framebuffer->height(), state.sourceFrame.height() * scale)));
            upscaleCapped = upscaleSize.width() <= state.sourceFrame.width() * 1.2;
            if (upscaleSize.width() > state.framebuffer->width() ||
                upscaleSize.height() > state.framebuffer->height()) {
                if (state.ensureUpscaleFramebuffer(upscaleSize)) {
                    state.useUpscaleTarget = true;
                }
            }
        }
        // The immutable native RGB source is unchanged; only output allocation varies.
        // Re-decode the immutable raw RGB frame under the new pipeline, never accept the
        // old paused frame or an empty reconfiguration framebuffer.
        //
        // The whole vf chain is cleared before the new one goes in: `set vf ""`
        // drops the filter the previous candidate left in the core, so a switch
        // between two AnimeJaNai models cannot leave the first one running and
        // answering for the second. The full ordered plan is then attached as
        // one vf pipeline, with unique app-owned labels for its native stages.
        // Only the still path reaches this point, and a rawvideo source has no
        // decoder to align: `auto-safe` leaves the user's choice untouched.
        const std::string targetHwdec = "auto-safe";
        state.ajiLabels.clear();
        if (state.stageBuildLease) {
            state.error = QCoreApplication::translate(
                "CompareRenderer",
                "The previous AnimeJaNai comparison pipeline has not been detached.");
            return {};
        }
        if (plan.requiresAji) {
            if (plan.cacheKeys.isEmpty()) {
                state.error = QCoreApplication::translate(
                    "CompareRenderer",
                    "The comparison plan has no owned AnimeJaNai cache keys.");
                return {};
            }
            QString busyKey;
            QString busyOwner;
            state.stageBuildLease = plan.cacheLease;
            if (!state.stageBuildLease || !state.stageBuildLease->held())
                state.stageBuildLease = native::AnimeJanaiStageBuildLease::tryAcquire(
                plan.cacheKeys, QStringLiteral("Compare isolated renderer"),
                &busyKey, &busyOwner);
            if (!state.stageBuildLease) {
                state.error = QCoreApplication::translate(
                    "CompareRenderer",
                    "The AnimeJaNai stage cache %1 is already in use by %2.")
                                  .arg(busyKey, busyOwner);
                return {};
            }
        }
        // T-314: the lease must not outlive a failed render. It was released
        // only on the success path, so every null-output return below left it
        // held until the owner called stop() - which a torn-down window might
        // never do. The scope guard gives it back on ANY exit from this
        // function; the success path disowns it first so the release stays
        // where the reader can see it.
        struct BuildLeaseGuard {
            std::shared_ptr<native::AnimeJanaiStageBuildLease>* slot = nullptr;
            ~BuildLeaseGuard() {
                if (slot != nullptr && *slot) {
                    (*slot)->release();
                    slot->reset();
                }
            }
        } leaseGuard;
        leaseGuard.slot = &state.stageBuildLease;
        if (plan.enabled && plan.requiresAji) {
            // A native stage is replayed from the real source: the still path
            // feeds a software RGB frame through a format normaliser, which is a
            // different program from the one the player runs, and it destroys the
            // configured stage by reloading the file to get another frame.
            QImage native = state.renderNative(plan, cancelled, progress);
            if (!native.isNull()) {
                leaseGuard.slot = nullptr;
                if (state.stageBuildLease) state.stageBuildLease->release();
                state.stageBuildLease.reset();
            }
            return native;
        }
        state.requestedSlot = 0;
        state.ajiPollIndex = 0;
        state.processingObserved = false;
        state.requestedBackend = plan.backend;
        state.buildGate = {};
        state.configuredStages = 0;
        state.backendSubstituted = false;
        state.diagnosticTail.clear();
        if (upscaleCapped) {
            // T-313: appended after the clear, so the note survives into the
            // render's diagnostic tail: a source beyond the 4K internal-target
            // clamp may run its x2 Upscale candidates as passthrough, and the
            // comparison must say so rather than read as an executed shader.
            state.diagnosticTail.append(
                QCoreApplication::translate(
                    "CompareRenderer",
                    "This source exceeds the internal comparison target, so x2 upscaling "
                    "candidates may render unchanged."));
        }
        // Invalidate prior configured-slot evidence before stop/vf-clear can
        // dispatch queued log events from the previous filter generation.
        state.processingObserved = false;
        state.acceptBuildEvidence = false;
        state.configuredStages = 0;
        state.buildGate = {};
        if (!state.waitForCommand(state.command({"stop"}), cancelled) ||
            !state.waitForCommand(state.command({"set", "vf", ""}), cancelled) ||
            !state.waitForCommand(state.command({"set", "hwdec", targetHwdec}), cancelled) ||
            !state.waitForCommand(state.command({"set", "start", "0"}), cancelled) ||
            !state.waitForCommand(state.command({"set", "glsl-shaders", chain.toStdString()}), cancelled)) return {};
        if (plan.requiresAji) native::preloadAnimeJanaiBackendLibs(plan.backendLibDirs);
        if (!filter.isEmpty()) {
            state.acceptBuildEvidence = true;
            // Buffered filters need repeated input even though Compare freezes one frame.
            QString frozenPipeline = QStringLiteral(
                "@mikai_compare_repeat:lavfi=[loop=loop=-1:size=1:start=0],");
            frozenPipeline += filter;
            const bool added = state.waitForCommand(
                state.command({"vf", "add", frozenPipeline.toStdString()}), cancelled);
            if (!added) return {};
        }
        state.restarted = false;
        if (!state.loadStill(QString(), cancelled)) return {};
        QImage output = state.settle(true, cancelled, progress);
        if (!output.isNull()) {
            // T-314: disown first - the guard must not double-release a lease
            // the success path has already given back.
            leaseGuard.slot = nullptr;
            if (state.stageBuildLease) state.stageBuildLease->release();
            state.stageBuildLease.reset();
        }
        return output;
    } catch (const MpvError& error) {
        state.error = QCoreApplication::translate("CompareRenderer", "The comparison renderer failed: %1").arg(QString::fromUtf8(error.what()));
        return {};
    }
}

void CompareRenderer::stop() {
    // Called only after render/capture has unwound, while its owner still keeps
    // the renderer alive. The filter is detached first: its destruction
    // terminates and joins a builder thread, and an anakin/TRT engine is torn
    // down while its own core still answers. The scoped context keeps that call
    // off the driver's deallocation path.
    State& state = *state_;
    ++state.generation;
    if (!state.core) {
        if (state.stageBuildLease) state.stageBuildLease->release();
        state.stageBuildLease.reset();
        return;
    }
    const ScopedCurrent current(state.context, &state.target);
    try {
        // The removal is what joins the fork's builder thread, so its reply has
        // to be observed before the core is destroyed. A fire-and-forget removal
        // plus one drain left the TensorRT build running inside a filter the
        // process was already tearing down - an abort inside the runtime, which
        // is why the window could vanish without a message.
        // X-M13: was a `never` cancel predicate, so each label's removal
        // could wait the full 20 s kSourceTimeoutMs on the close path
        // (20 s x N labels). Each label now gets a 3 s deadline; the
        // wait still pumps events, so this is latency, not a freeze.
        QElapsedTimer labelDeadline;
        for (const QString& filterLabel : state.ajiLabels) {
            const std::string label = "@" + filterLabel.toStdString();
            labelDeadline.start();
            const std::function<bool()> expired = [&labelDeadline] {
                return labelDeadline.elapsed() >= 3000;
            };
            if (!state.waitForCommand(state.command({"vf", "remove", label}), expired))
                state.error.clear();
        }
    } catch (const MpvError&) {
        // A core that can no longer answer a removal has nothing to remove from.
    }
    state.rendering.reset();
    state.core.reset();
    if (state.stageBuildLease) state.stageBuildLease->release();
    state.stageBuildLease.reset();
}

QImage CompareRenderer::baseline() const { return state_->baselineFrame; }

QString CompareRenderer::replaySummary() const { return state_->modelSummary; }

QImage CompareRenderer::lastVideoFrame() const { return state_->modelVoFrame; }

QImage CompareRenderer::State::queryVoFrame() {
    State& state = *this;
    if (!state.core || !state.rendering) return {};
    struct Reply {
        bool finished = false;
        QImage image;
    };
    auto reply = std::make_shared<Reply>();
    NodeValue args;
    args.format = MPV_FORMAT_NODE_ARRAY;
    NodeValue name;
    name.format = MPV_FORMAT_STRING;
    name.text = "screenshot-raw";
    NodeValue what;
    what.format = MPV_FORMAT_STRING;
    what.text = "video";
    args.list = {name, what};
    if (!state.core->commandNodeAsync(args, [reply](const AsyncReply& answer) {
            reply->finished = true;
            if (!answer.ok() || answer.node.format != MPV_FORMAT_NODE_MAP) return;
            int width = 0;
            int height = 0;
            int stride = 0;
            QString format;
            std::string bytes;
            for (const auto& entry : answer.node.map) {
                const NodeValue& value = entry.second;
                if (entry.first == "w") width = int(value.integer);
                else if (entry.first == "h") height = int(value.integer);
                else if (entry.first == "stride") stride = int(value.integer);
                else if (entry.first == "format") format = QString::fromStdString(value.text);
                else if (entry.first == "data") bytes = value.bytes;
            }
            if (width <= 0 || height <= 0 || stride < width * 4 || bytes.empty()) return;
            // X-C5: QImage(...).copy() reads stride * height bytes; a
            // truncated node would read past the end of `bytes`.
            if (qint64(bytes.size()) < qint64(stride) * qint64(height)) return;
            if (format != QLatin1String("bgr0")) return;
            // copy(): the reply's storage dies with the next drain.
            reply->image = QImage(reinterpret_cast<const uchar*>(bytes.data()), width, height,
                                  stride, QImage::Format_RGB32)
                               .copy();
        }).valid()) {
        return {};
    }
    QElapsedTimer timer;
    timer.start();
    while (!reply->finished && timer.elapsed() < 5000 && state.error.isEmpty()) {
        state.pumpFrame();
        QCoreApplication::processEvents();
        QThread::msleep(kFrameWaitMs);
    }
    return reply->image;
}

QString CompareRenderer::error() const { return state_->error; }

}
