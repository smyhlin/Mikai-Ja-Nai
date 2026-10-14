#include "CompareRenderer.hpp"

#include "Anime4K.hpp"
#include "MpvHandle.hpp"
#include "MpvRenderContext.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <clocale>
#include <cmath>

namespace mplayer::shell {

namespace {
constexpr int kPollMs = 40;
constexpr int kSourceTimeoutMs = 20000;
constexpr int kBuildTimeoutMs = 20 * 60 * 1000;

struct CommandResult {
    bool finished = false;
    int error = 0;
};

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
        if (!bound_ || previous_ == &context_) return;
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
    QString buildDetail;
    bool restarted = false;
    bool processingObserved = false;
    bool useUpscaleTarget = false;
    int requestedSlot = 0;

    ~State() {
        // State dies on CompareWindow's GUI thread, so the guard makes the
        // context current for the GL releases and drops it immediately after.
        const ScopedCurrent current(context, &target);
        rendering.reset();
        upscaleFramebuffer.reset();
        framebuffer.reset();
        core.reset();
    }

    bool initialize(const QSize& size, double timestamp) {
        std::setlocale(LC_NUMERIC, "C");
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
            framebuffer = std::make_unique<QOpenGLFramebufferObject>(size);
        }
        if (!framebuffer->isValid()) {
            error = QCoreApplication::translate("CompareRenderer", "The comparison framebuffer could not be created.");
            return false;
        }
        if (!core) {
            const std::string startTimestamp = QString::number(timestamp, 'f', 6).toStdString();
            core = std::make_shared<MpvHandle>(
                std::vector<std::pair<std::string, std::string>>{
                    {"vo", "libmpv"}, {"hwdec", "nvdec"}, {"pause", "yes"},
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
                    if (ended->reason == MPV_END_FILE_REASON_ERROR)
                        error = QCoreApplication::translate("CompareRenderer", "The comparison source could not be loaded: %1")
                                .arg(QString::fromStdString(core->describeError(ended->error)));
                }
                if (event->event_id != MPV_EVENT_LOG_MESSAGE || event->data == nullptr) return;
                const auto* message = static_cast<mpv_event_log_message*>(event->data);
                const QString text = QString::fromUtf8(message->text).trimmed();
                const QString prefix = QString::fromUtf8(message->prefix);
                if (message->log_level <= MPV_LOG_LEVEL_FATAL) {
                    error = QCoreApplication::translate("CompareRenderer", "The comparison renderer failed: %1").arg(text);
                }
                const QString combined = prefix + QLatin1Char(' ') + text;
                const bool isAjiLog =
                    combined.contains(QLatin1String("animejanai"), Qt::CaseInsensitive) ||
                    combined.contains(QLatin1String("mikai_aji"), Qt::CaseInsensitive) ||
                    combined.contains(QLatin1String("Configured slot"), Qt::CaseInsensitive) ||
                    combined.contains(QLatin1String("TensorRT engine"), Qt::CaseInsensitive) ||
                    combined.contains(QLatin1String("building engine:"), Qt::CaseInsensitive);
                if (!isAjiLog) return;
                if ((text.contains(QStringLiteral("Configured slot %1:").arg(requestedSlot),
                                   Qt::CaseInsensitive) ||
                     (requestedSlot > 0 &&
                      text.contains(QStringLiteral("Configured slot"), Qt::CaseInsensitive))) &&
                    text.contains(QLatin1String(" -> "))) {
                    processingObserved = true;
                }
                if (text.contains(QStringLiteral("Building TensorRT engine"), Qt::CaseInsensitive) ||
                    text.contains(QStringLiteral("building engine:"), Qt::CaseInsensitive) ||
                    text.contains(QStringLiteral("background engine build:"), Qt::CaseInsensitive))
                    buildDetail = QCoreApplication::translate("CompareRenderer", "Building the comparison TensorRT engine");
                if (message->log_level <= MPV_LOG_LEVEL_ERROR ||
                    text.contains(QLatin1String("build failed"), Qt::CaseInsensitive))
                    error = QCoreApplication::translate("CompareRenderer", "The comparison model failed: %1").arg(text);
            });
            rendering = std::make_unique<MpvRenderContext>(*core, loadGlApi());
            rendering->setFlipY(true);
        }
        return true;
    }

    bool ensureUpscaleFramebuffer(const QSize& desired) {
        const ScopedCurrent current(context, &target);
        if (!current || !target.isValid()) return false;
        if (!upscaleFramebuffer || upscaleFramebuffer->size() != desired) {
            upscaleFramebuffer = std::make_unique<QOpenGLFramebufferObject>(desired);
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

    QImage pump() {
        core->drainEvents();
        QImage image;
        {
            // Scoped so the nested event loop below runs with no comparison
            // context current: whatever it reaches has to bind its own surface.
            const ScopedCurrent current(context, &target);
            if (!current) {
                error = QCoreApplication::translate("CompareRenderer", "The comparison OpenGL context was lost.");
                return {};
            }
            QOpenGLFramebufferObject* activeFbo =
                (useUpscaleTarget && upscaleFramebuffer && upscaleFramebuffer->isValid())
                    ? upscaleFramebuffer.get()
                    : framebuffer.get();
            rendering->update();
            activeFbo->bind();
            // The same defined black the live surface paints before mpv draws
            // into it, and the same viewport: an FBO that was never cleared
            // keeps the previous frame's pixels around whatever the new picture
            // covers, which reads as a stale or half-lit frame after a model
            // switch whose output size did not change with the filter.
            if (QOpenGLFunctions* gl = context.functions()) {
                gl->glViewport(0, 0, activeFbo->width(), activeFbo->height());
                gl->glDisable(GL_SCISSOR_TEST);
                gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                gl->glClear(GL_COLOR_BUFFER_BIT);
            }
            const int result = rendering->render(activeFbo->handle(), activeFbo->width(),
                                                 activeFbo->height(), 0);
            rendering->reportSwap();
            if (result < 0) {
                error = QCoreApplication::translate("CompareRenderer", "The comparison renderer failed: %1")
                            .arg(QString::fromStdString(core->describeError(result)));
            } else if (activeFbo != framebuffer.get() && framebuffer && framebuffer->isValid()) {
                QOpenGLFramebufferObject::blitFramebuffer(
                    framebuffer.get(),
                    QRect(0, 0, framebuffer->width(), framebuffer->height()),
                    activeFbo,
                    QRect(0, 0, activeFbo->width(), activeFbo->height()),
                    GL_COLOR_BUFFER_BIT, GL_LINEAR);
                image = framebuffer->toImage();
            } else {
                image = activeFbo->toImage();
            }
        }
        QCoreApplication::processEvents();
        QThread::msleep(kPollMs);
        return image;
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

    QImage settle(bool requireRestart, bool requireProcessing, const Cancelled& cancelled,
                  const Progress& progress) {
        QElapsedTimer timer;
        timer.start();
        QImage previous;
        QString reported;
        bool processingFrameRequested = false;
        std::shared_ptr<CommandResult> buildPoll;
        while (timer.elapsed() < (requireProcessing && !buildDetail.isEmpty()
                                      ? kBuildTimeoutMs : kSourceTimeoutMs) &&
               error.isEmpty() && !cancelled()) {
            const QImage image = pump();
            if (cancelled() || !error.isEmpty()) return {};
            if (requireProcessing && !processingObserved &&
                (!buildPoll || buildPoll->finished)) {
                buildPoll = command({"vf-command", native::kAnimeJanaiFilterLabel, "poll", ""});
            }
            if (requireProcessing && processingObserved && !processingFrameRequested) {
                processingFrameRequested = true;
                restarted = false;
                previous = {};
                if (!waitForCommand(command({"seek", "0", "absolute+exact"}), cancelled)) return {};
                continue;
            }
            if (!buildDetail.isEmpty() && buildDetail != reported) {
                reported = buildDetail;
                progress(buildDetail);
            }
            if (timer.elapsed() >= 200 && (!requireRestart || restarted) &&
                (!requireProcessing || processingObserved) && !image.isNull() &&
                image == previous) return image;
            previous = image;
        }
        if (!cancelled() && error.isEmpty())
            error = QCoreApplication::translate("CompareRenderer", "The comparison output did not settle.");
        return {};
    }
};

CompareRenderer::CompareRenderer() : state_(std::make_unique<State>()) {}
CompareRenderer::~CompareRenderer() = default;

bool CompareRenderer::capture(const QString& source, double timestamp, const QSize& outputSize,
                              const Cancelled& cancelled, const Progress& progress) {
    std::setlocale(LC_NUMERIC, "C");
    State& state = *state_;
    state.error.clear();
    state.sourceFrame = {};
    state.sourceUrl = source;
    state.sourceTimestamp = timestamp;
    state.buildDetail.clear();
    state.useUpscaleTarget = false;
    try {
        if (!state.initialize(outputSize, timestamp) || cancelled()) return false;
        const std::string startTimestamp = QString::number(timestamp, 'f', 6).toStdString();
        if (!state.waitForCommand(state.command({"set", "vf", ""}), cancelled) ||
            !state.waitForCommand(state.command({"set", "glsl-shaders", ""}), cancelled) ||
            !state.waitForCommand(state.command({"set", "start", startTimestamp}), cancelled)) return false;
        state.restarted = false;
        state.core->loadFile(source.toStdString());
        const QImage settledSource = state.settle(true, false, cancelled, progress);
        if (settledSource.isNull()) return false;
        state.framePath = state.files.filePath(QStringLiteral("source.png"));
        if (!state.waitForCommand(state.command({"screenshot-to-file", state.framePath.toStdString(), "video"}), cancelled) ||
            !QFileInfo::exists(state.framePath)) {
            state.error.clear();
            settledSource.save(state.framePath, "PNG");
        }
        state.sourceFrame = QImage(state.framePath);
        if (state.sourceFrame.isNull() && !settledSource.isNull()) {
            settledSource.save(state.framePath, "PNG");
            state.sourceFrame = QImage(state.framePath);
        }
        if (state.sourceFrame.isNull()) {
            state.error = QCoreApplication::translate("CompareRenderer", "The decoded comparison source frame could not be read.");
            return false;
        }
        if (!state.waitForCommand(state.command({"set", "start", "0"}), cancelled)) return false;
        state.restarted = false;
        state.core->loadFile(state.framePath.toStdString());
        return !state.settle(true, false, cancelled, progress).isNull();
    } catch (const MpvError& error) {
        state.error = QCoreApplication::translate("CompareRenderer", "The comparison renderer failed: %1").arg(QString::fromUtf8(error.what()));
        return false;
    }
}

QImage CompareRenderer::render(const QString& shaderId, const native::AnimeJanaiPlan& plan,
                               const Cancelled& cancelled, const Progress& progress) {
    std::setlocale(LC_NUMERIC, "C");
    State& state = *state_;
    state.error.clear();
    state.buildDetail.clear();
    state.useUpscaleTarget = false;
    if (state.sourceFrame.isNull() || cancelled()) return {};
    try {
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
        // Anime4K Upscale shaders carry `//!WHEN OUTPUT.w MAIN.w / 1.200 >`.
        // When the window FBO is smaller than 1.2x the video frame (e.g. a 1080p
        // video on a 1080p display or in a windowed viewport), rendering into an
        // internal 2x FBO ensures mpv executes the Upscale stages instead of
        // skipping them.
        if (!plan.enabled && chain.contains(QLatin1String("Upscale"), Qt::CaseInsensitive) &&
            state.sourceFrame.width() <= 1920 && state.sourceFrame.height() <= 1080 &&
            (state.framebuffer->width() <= state.sourceFrame.width() * 1.2 ||
             state.framebuffer->height() <= state.sourceFrame.height() * 1.2)) {
            const QSize upscaleSize(
                std::min(3840, std::max(state.framebuffer->width(), state.sourceFrame.width() * 2)),
                std::min(2160, std::max(state.framebuffer->height(), state.sourceFrame.height() * 2)));
            if (upscaleSize.width() > state.framebuffer->width() &&
                upscaleSize.height() > state.framebuffer->height() &&
                state.ensureUpscaleFramebuffer(upscaleSize)) {
                state.useUpscaleTarget = true;
            }
        }
        // Re-decode the immutable PNG under the new pipeline, never accept the
        // old paused frame or an empty reconfiguration framebuffer.
        //
        // The whole vf chain is cleared before the new one goes in: `set vf ""`
        // drops the filter the previous candidate left in the core, so a switch
        // between two AnimeJaNai models cannot leave the first one running and
        // answering for the second. The attach is then `vf add`, exactly as the
        // live surface submits its own plan - whose spec carries the @mikai_aji
        // label and therefore replaces by label.
        const std::string targetHwdec =
            (plan.enabled && plan.backend == native::Backend::DirectML) ? "d3d11va" : "nvdec";
        state.requestedSlot = plan.enabled ? plan.slot : 0;
        state.processingObserved = false;
        if (!state.waitForCommand(state.command({"stop"}), cancelled) ||
            !state.waitForCommand(state.command({"set", "vf", ""}), cancelled) ||
            !state.waitForCommand(state.command({"set", "hwdec", targetHwdec}), cancelled) ||
            !state.waitForCommand(state.command({"set", "start", "0"}), cancelled) ||
            !state.waitForCommand(state.command({"set", "glsl-shaders", chain.toStdString()}), cancelled)) return {};
        if (!filter.isEmpty() &&
            !state.waitForCommand(state.command({"vf", "add", filter.toStdString()}), cancelled)) return {};
        state.restarted = false;
        state.core->loadFile(state.framePath.toStdString());
        return state.settle(true, plan.enabled, cancelled, progress);
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
    if (!state.core) return;
    const ScopedCurrent current(state.context, &state.target);
    const std::string label = "@" + std::string(native::kAnimeJanaiFilterLabel);
    try {
        state.core->commandAsync({"vf", "remove", label}, [](const AsyncReply&) {});
        // One drain so the request reaches the core before it is destroyed: the
        // reply itself is discarded, because a detached filter has nothing left
        // to say that this caller needs.
        state.core->drainEvents();
    } catch (const MpvError&) {
        // A core that can no longer answer a removal has nothing to remove from.
    }
    state.rendering.reset();
    state.core.reset();
}

QString CompareRenderer::error() const { return state_->error; }

}
