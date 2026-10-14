#include "CompareRenderer.hpp"

#include "Anime4K.hpp"
#include "MpvHandle.hpp"
#include "MpvRenderContext.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QTemporaryDir>
#include <QThread>

#include <clocale>
#include <cmath>

namespace mplayer::shell {

namespace {
constexpr int kPollMs = 40;
constexpr int kSourceTimeoutMs = 20000;
constexpr int kBuildTimeoutMs = 20 * 60 * 1000;

std::string formatTimestampInvariant(double timestamp) {
    return QString::number(timestamp, 'f', 6).toStdString();
}

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
    QOpenGLContext* previous_;
    QSurface* previousSurface_;
    bool bound_;
};
}  // namespace

struct CompareRenderer::State {
    QOpenGLContext context;
    QOffscreenSurface target;
    QTemporaryDir files;
    std::unique_ptr<QOpenGLFramebufferObject> framebuffer;
    std::shared_ptr<MpvHandle> core;
    std::unique_ptr<MpvRenderContext> rendering;
    QImage sourceFrame;
    QString error;
    QString framePath;
    QString buildDetail;
    bool restarted = false;
    bool processingObserved = false;
    int requestedSlot = 0;

    ~State() {
        const ScopedCurrent current(context, &target);
        rendering.reset();
        framebuffer.reset();
        core.reset();
    }

    bool initialize(const QSize& size, double timestamp) {
        if (!files.isValid() || size.isEmpty() || !std::isfinite(timestamp)) {
            error = QCoreApplication::translate("CompareRenderer", "The comparison frame storage or geometry is unavailable.");
            return false;
        }
        std::setlocale(LC_NUMERIC, "C");
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
            core = std::make_shared<MpvHandle>(
                std::vector<std::pair<std::string, std::string>>{
                    {"vo", "libmpv"}, {"hwdec", "nvdec"}, {"pause", "yes"},
                    {"audio", "no"}, {"sub", "no"}, {"keep-open", "yes"},
                    {"image-display-duration", "inf"}, {"screenshot-sw", "yes"},
                    {"screenshot-format", "png"}, {"input-default-bindings", "no"},
                    {"start", formatTimestampInvariant(timestamp)},
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
                if (!prefix.contains(QLatin1String("animejanai"))) return;
                if (text.contains(QStringLiteral("Configured slot %1:").arg(requestedSlot)) &&
                    text.contains(QLatin1String(" -> "))) processingObserved = true;
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
            const ScopedCurrent current(context, &target);
            if (!current) {
                error = QCoreApplication::translate("CompareRenderer", "The comparison OpenGL context was lost.");
                return {};
            }
            rendering->update();
            framebuffer->bind();
            if (QOpenGLFunctions* gl = context.functions()) {
                gl->glViewport(0, 0, framebuffer->width(), framebuffer->height());
                gl->glDisable(GL_SCISSOR_TEST);
                gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                gl->glClear(GL_COLOR_BUFFER_BIT);
            }
            const int result = rendering->render(framebuffer->handle(), framebuffer->width(),
                                                 framebuffer->height(), 0);
            rendering->reportSwap();
            if (result < 0)
                error = QCoreApplication::translate("CompareRenderer", "The comparison renderer failed: %1")
                            .arg(QString::fromStdString(core->describeError(result)));
            image = result < 0 ? QImage() : framebuffer->toImage();
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
               timer.elapsed() < kSourceTimeoutMs) pump();
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
    State& state = *state_;
    state.error.clear();
    state.sourceFrame = {};
    state.buildDetail.clear();
    try {
        if (!state.initialize(outputSize, timestamp) || cancelled()) return false;
        if (!state.waitForCommand(state.command({"set", "vf", ""}), cancelled) ||
            !state.waitForCommand(state.command({"set", "glsl-shaders", ""}), cancelled) ||
            !state.waitForCommand(state.command({"set", "start", formatTimestampInvariant(timestamp)}), cancelled)) return false;
        state.restarted = false;
        state.core->loadFile(source.toStdString());
        if (state.settle(true, false, cancelled, progress).isNull()) return false;
        state.framePath = state.files.filePath(QStringLiteral("source.png"));
        if (!state.waitForCommand(state.command({"screenshot-to-file", state.framePath.toStdString(), "video"}), cancelled)) return false;
        state.sourceFrame = QImage(state.framePath);
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
    State& state = *state_;
    state.error.clear();
    state.buildDetail.clear();
    if (state.sourceFrame.isNull() || cancelled()) return {};
    try {
        if (!state.core && !state.initialize(state.framebuffer->size(), 0.0)) return {};
        const QString filter = plan.enabled ? QString::fromUtf8(plan.filterSpec) : QString();
        const QString chain = plan.enabled ? QString()
                                           : anime4k::shaderStringForCandidate(shaderId, anime4k::findShadersDir());
        if (!state.waitForCommand(state.command({"stop"}), cancelled) ||
            !state.waitForCommand(state.command({"set", "vf", ""}), cancelled) ||
            !state.waitForCommand(state.command({"set", "glsl-shaders", chain.toStdString()}), cancelled)) return {};
        if (!filter.isEmpty() &&
            !state.waitForCommand(state.command({"vf", "add", filter.toStdString()}), cancelled)) return {};
        state.requestedSlot = plan.enabled ? plan.slot : 0;
        state.processingObserved = false;
        state.restarted = false;
        state.core->loadFile(state.framePath.toStdString());
        return state.settle(true, plan.enabled, cancelled, progress);
    } catch (const MpvError& error) {
        state.error = QCoreApplication::translate("CompareRenderer", "The comparison renderer failed: %1").arg(QString::fromUtf8(error.what()));
        return {};
    }
}

void CompareRenderer::stop() {
    State& state = *state_;
    if (!state.core) return;
    const ScopedCurrent current(state.context, &state.target);
    const std::string label = "@" + std::string(native::kAnimeJanaiFilterLabel);
    try {
        state.core->commandAsync({"vf", "remove", label}, [](const AsyncReply&) {});
        state.core->drainEvents();
    } catch (const MpvError&) {
    }
    state.rendering.reset();
    state.core.reset();
}

QString CompareRenderer::error() const { return state_->error; }

}  // namespace mplayer::shell
