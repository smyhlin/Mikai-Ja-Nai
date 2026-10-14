#include "VideoSurface.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include "MpvApi.hpp"
#include "MpvHandle.hpp"
#include "MpvRenderContext.hpp"
#include "backend/Diagnostics.hpp"
#include "native/bundle/NativeBundle.hpp"
#include "native/status/NativeFilterCommand.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"

namespace mplayer {

namespace {

constexpr qint64 kDiagnosticsPollIntervalMs = 500;
// A lost reply must not leave a property unread for the rest of the source.
constexpr qint64 kDiagnosticsRequestTimeoutMs = 5000;
constexpr qint64 kRestoreSettleTimeoutMs = 5000;
constexpr int kRestoreSettleMaxAttempts = 5;

const char* nativePlaybackStateName(native::NativePlaybackController::State state) {
    using State = native::NativePlaybackController::State;
    switch (state) {
        case State::Idle: return "Idle";
        case State::Attached: return "Attached";
        case State::Admitting: return "Admitting";
        case State::Active: return "Active";
        case State::Draining: return "Draining";
        case State::Suspended: return "Suspended";
        case State::Off: return "Off";
        case State::Failed: return "Failed";
    }
    return "Unknown";
}

constexpr const char* kDecoderDropProperty = "decoder-frame-drop-count";
// Current mpv: "frame-drop-count" is the VO drop counter (the old
// vo-drop-frame-count is gone), so it is never read as the decoder's.
constexpr const char* kVoDropProperty = "frame-drop-count";
constexpr const char* kVfProperty = "vf";

/// The runtime module the live core was loaded from, or empty when none was
/// selected. bundle() hands back a reference into the loader's state, so the
/// value is copied here rather than read after the lock is gone.
QString currentMpvModulePath() {
    MpvApi& api = MpvApi::instance();
    if (!api.loaded()) return QString();
    return api.bundle().modulePath;
}

/// The `lib=` option the native filter needs to open its inference shim: the
/// producer library of the validated native bundle this process actually
/// loaded, proved against that bundle's own inventory digest. Empty when the
/// loaded runtime is not a native bundle or the declared dispatcher is gone or
/// no longer matches its recorded bytes - the binding is then probe-only, and
/// an admission against it is refused by the core rather than loaded from a
/// guessed path.
QVector<native::NativeFilterOption> provedNativeAttachOptions() {
    const native::RuntimeSelection& selection = native::currentSelection();
    if (!selection.loaded || selection.kind != native::BundleKind::Native ||
        !selection.resolvedBundle.has_value()) {
        return {};
    }
    const native::Bundle& bundle = *selection.resolvedBundle;
    const QString producer = native::bundlePreparationProducerLibrary();
    for (const native::BundleFile& file : bundle.files) {
        if (QFileInfo(file.path).fileName().compare(producer, Qt::CaseInsensitive) != 0) continue;
        const QString absolute =
            QFileInfo(QDir::cleanPath(QDir(bundle.root).filePath(file.path))).canonicalFilePath();
        if (absolute.isEmpty()) continue;
        QFile handle(absolute);
        if (!handle.open(QIODevice::ReadOnly)) continue;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&handle)) continue;
        if (hash.result().toHex().compare(file.sha256.toLatin1(), Qt::CaseInsensitive) != 0) {
            continue;
        }
        return {native::NativeFilterOption{QStringLiteral("lib"), absolute}};
    }
    return {};
}

QString privateNativeRuntimeUnavailableReason() {
    const native::RuntimeSelection& selection = native::currentSelection();
    if (selection.loaded && selection.kind == native::BundleKind::Native) return {};
    return selection.reason.isEmpty()
               ? QStringLiteral("Private Native playback requires a loaded Native runtime.")
               : selection.reason;
}

// A captured frame larger than this is refused by name rather than allocated:
// no real source is this big, and a hostile reply must not pick our allocation.
constexpr qint64 kMaxReferenceSide = 32768;
constexpr qint64 kMaxReferenceStride = 1 << 20;

/// One member of a node map. mpv writes each key once, so the first match is it.
const NodeValue* mapMember(const NodeValue& map, const char* key) {
    for (const std::pair<std::string, NodeValue>& member : map.map) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

/// Turns one `screenshot-raw video rgba` reply into the RGB8 fixture the oracle
/// consumes. Every declared size is validated in 64-bit before it is used to
/// index anything, and a payload that does not cover the declared rows is
/// refused rather than read past.
bool decodeRawFrame(const NodeValue& node, native::FrameFixture* frame, QString* reasonKey,
                    QString* reason) {
    const auto refuse = [reasonKey, reason](const QString& key, const QString& text) {
        if (reasonKey != nullptr) *reasonKey = key;
        if (reason != nullptr) *reason = text;
        return false;
    };
    if (node.format != MPV_FORMAT_NODE_MAP) {
        return refuse(QStringLiteral("native.reference.replyShape"),
                      QStringLiteral("the capture reply is not a node map"));
    }
    const NodeValue* width = mapMember(node, "w");
    const NodeValue* height = mapMember(node, "h");
    const NodeValue* stride = mapMember(node, "stride");
    const NodeValue* format = mapMember(node, "format");
    const NodeValue* data = mapMember(node, "data");
    if (width == nullptr || height == nullptr || stride == nullptr || format == nullptr ||
        data == nullptr) {
        return refuse(QStringLiteral("native.reference.replyShape"),
                      QStringLiteral("the capture reply is missing a declared member"));
    }
    if (format->text != "rgba") {
        // The requested order is what the channel strip below assumes; a
        // different one would silently swap colour channels.
        return refuse(QStringLiteral("native.reference.format"),
                      QStringLiteral("the core returned '%1', not the requested rgba")
                          .arg(QString::fromStdString(format->text)));
    }
    const qint64 w = width->integer;
    const qint64 h = height->integer;
    const qint64 rowPitch = stride->integer;
    if (w <= 0 || h <= 0 || w > kMaxReferenceSide || h > kMaxReferenceSide) {
        return refuse(QStringLiteral("native.reference.geometry"),
                      QStringLiteral("the capture declared a size this build cannot hold"));
    }
    if (rowPitch < w * 4 || rowPitch > kMaxReferenceStride) {
        return refuse(QStringLiteral("native.reference.stride"),
                      QStringLiteral("the capture pitch does not cover the declared width"));
    }
    if (static_cast<qint64>(data->bytes.size()) < rowPitch * h) {
        return refuse(QStringLiteral("native.reference.payload"),
                      QStringLiteral("the capture payload is shorter than the declared rows"));
    }

    // RGBA is four bytes wide; the fixture is interleaved RGB8, so the pad byte
    // is dropped here and nowhere else.
    const auto* source = reinterpret_cast<const unsigned char*>(data->bytes.data());
    QByteArray rgb;
    rgb.resize(static_cast<qsizetype>(w) * 3 * static_cast<qsizetype>(h));
    unsigned char* destination = reinterpret_cast<unsigned char*>(rgb.data());
    const qsizetype outPitch = static_cast<qsizetype>(w) * 3;
    for (qint64 y = 0; y < h; ++y) {
        const unsigned char* row = source + static_cast<qsizetype>(y) * rowPitch;
        unsigned char* out = destination + static_cast<qsizetype>(y) * outPitch;
        for (qint64 x = 0; x < w; ++x) {
            out[0] = row[0];
            out[1] = row[1];
            out[2] = row[2];
            row += 4;
            out += 3;
        }
    }
    frame->size = QSize(static_cast<int>(w), static_cast<int>(h));
    frame->rgb = std::move(rgb);
    // The frame came from the live core, not from a file read: those provenance
    // fields stay empty rather than naming a file this frame never came from.
    frame->path.clear();
    frame->fileBytes = 0;
    frame->fileSha256.clear();
    return true;
}

/// A short digest of one captured frame, so "the same decoded frame again" is a
/// comparison of observations rather than of megabytes. It is not an identity
/// claim about the picture - it only has to separate a second capture from a
/// verbatim repeat of the first, which any content change does.
QByteArray frameDigest(const QByteArray& rgb) {
    return QCryptographicHash::hash(rgb, QCryptographicHash::Sha256).toHex().left(16);
}

/// The real hwdec method name a backend consumes, as mpv names it. These are
/// mpv's own method names (vd_lavc.c's hwdec_autoprobe_info), not a guess about
/// what the machine happens to have.
QString backendHwdec(const native::Backend backend) {
    return backend == native::Backend::TensorRT ? QStringLiteral("nvdec")
                                                : QStringLiteral("d3d11");
}

/// True when the decoder the core reported actually produces frames `backend`
/// can consume. TensorRT needs the CUDA interop nvdec produces; DirectML needs
/// D3D11 textures. A software decoder produces neither, and every `-copy` variant
/// downloads to system memory, so the frames the backend would receive would
/// carry no device at all - that is a refusal, not a silent CPU path.
bool decoderFeedsBackend(const QString& decoder, const native::Backend backend) {
    const QString name = decoder.trimmed().toLower();
    if (name.isEmpty() || name == QLatin1String("no")) return false;
    // A copying decoder hands mpv software frames: no CUDA context, no D3D11
    // texture, nothing this build's backends bind.
    if (name.endsWith(QLatin1String("-copy"))) return false;
    return backend == native::Backend::TensorRT ? name.startsWith(QLatin1String("nvdec"))
                                                : name.startsWith(QLatin1String("d3d11"));
}

/// Field-wise identity of two controller statuses, so the status signal fires on
/// a real change and not on every pump step.
bool nativeStatusEquals(const native::NativePlaybackController::Status& a,
                        const native::NativePlaybackController::Status& b) {
    return a.state == b.state && a.sourceEpoch == b.sourceEpoch &&
           a.confirmed == b.confirmed && a.requestId == b.requestId &&
           a.pendingRequestId == b.pendingRequestId && a.buildKeys == b.buildKeys &&
           a.reasonKey == b.reasonKey && a.reason == b.reason &&
           a.availability.value == b.availability.value &&
           a.availability.reasonKey == b.availability.reasonKey &&
           a.availability.reason == b.availability.reason && a.requested == b.requested &&
           a.effective == b.effective && a.counters == b.counters;
}

}  // namespace

void VideoSurface::ensureEngine() {
    if (mpv_) return;
    // These are the options the Python player passed through python-mpv, which
    // spelled them with underscores; mpv itself takes dashes.
    const std::vector<std::pair<std::string, std::string>> options{
        // Routes decoded frames into OUR render context. Without it mpv opens a
        // video output of its own, the render context never receives a frame
        // (update() stays at generation 0) and render() still returns success
        // while painting nothing.
        {"vo", "libmpv"},
        // Not cosmetic: MoonAnime's CDN answers 400 to a playlist that arrives
        // without an Accept-Language header, so mpv would sit on a stream that
        // extraction read perfectly well. The value is never inspected, only its
        // presence, and mpv splits http-header-fields on commas so it must stay
        // comma-free.
        {"user-agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
         "Chrome/142.0.0.0 Safari/537.36"},
        {"http-header-fields", "Accept-Language: uk-UA"},
        {"hwdec", "auto-safe"},
        {"keepaspect", "yes"},
        {"volume", "100"},
        // The keyboard belongs to the shell: mpv's own bindings would consume
        // Space and the arrow keys before Qt ever sees them.
        {"input-default-bindings", "no"},
        {"input-vo-keyboard", "no"},
        {"input-cursor", "no"},
        {"osc", "no"},
        // Reference captures must be the decoded source frame, not what the VO
        // rendered: with this set, screenshot-raw reads the current video frame
        // from the software path (no GLSL shader chain, no OSD), which is the
        // only capture this app's oracle may use. Nothing else in the player
        // reads mpv screenshots.
        {"screenshot-sw", "yes"},
    };
    mpv_ = std::make_unique<MpvHandle>(options);
    // A plan may have been selected before the core existed. Request its backend
    // reports before any source or official filter command reaches this core.
    mpv_->setBackendLogObservationEnabled(animeJanaiBackendLogsNeeded());
    // The filter opens its inference shim only for an explicitly named producer
    // library, so the binding an admission is later loaded into has to name the
    // one this core's own bundle ships; without it every admission is refused
    // by the core before its payload is parsed.
    nativePlayback_.setAttachOptions(provedNativeAttachOptions());
    // The reader borrows this core, and is (re)bound here because this is the
    // one place a core is created. It publishes an Unavailable (unread) view
    // until the first poll answers; polling itself stays off until the parent
    // asks for it.
    nativeStatus_.attach(mpv_.get());
    // A fresh core, so any raw number a previous one left reads as unknown.
    resetDiagnostics();
    mpv_->setEventSink([this](mpv_event* event) {
        if (event->event_id == MPV_EVENT_LOG_MESSAGE && event->data != nullptr) {
            const auto* message = static_cast<const mpv_event_log_message*>(event->data);
            const QString line = QString::fromUtf8(message->text == nullptr ? "" : message->text).trimmed();
            const QString prefix = QString::fromUtf8(message->prefix == nullptr ? "" : message->prefix);
            // libmpv echoes the URLs it was handed and the filter commands it
            // was asked to run. Both can carry a signed token, and everything
            // Qt logging emits is persisted by the session log and carried into
            // crash reports, so the redaction happens at this boundary rather
            // than at every reader of a log line. The observation and failure
            // paths below keep the exact mpv text: matching a status marker or
            // naming a failure is not a persistence boundary, and redacting
            // there would change what the player reports to the user.
            const QString safePrefix = mikai::backend::redactDiagnostic(prefix);
            const QString safeLine = mikai::backend::redactDiagnostic(line);
            if (message->log_level <= MPV_LOG_LEVEL_WARN) {
                qWarning().noquote() << "[mpv]" << safePrefix << safeLine;
            } else {
                qInfo().noquote() << "[mpv]" << safePrefix << safeLine;
            }
            const QString lower = (prefix + QLatin1Char(' ') + line).toLower();
            observeAnimeJanaiBackendLog(prefix, line);
            const bool ajiMessage = lower.contains(QStringLiteral("aji")) ||
                                    lower.contains(QStringLiteral("animejanai")) ||
                                    lower.contains(QStringLiteral("inference shim")) ||
                                    (animeJanaiFilterBound_ &&
                                     (lower.contains(QStringLiteral("inference")) ||
                                      lower.contains(QStringLiteral("model")) ||
                                      lower.contains(QStringLiteral("backend")) ||
                                      lower.contains(QStringLiteral("engine"))));
            if (ajiMessage &&
                (lower.contains(QStringLiteral("error")) ||
                 lower.contains(QStringLiteral("failed")) ||
                 lower.contains(QStringLiteral("failure")))) {
                animeJanaiFail(QStringLiteral("animejanai.backend.failed"), line);
            }
            return;
        }
        if (event->event_id != MPV_EVENT_END_FILE || event->data == nullptr) return;
        const auto* ended = static_cast<const mpv_event_end_file*>(event->data);
        if (ended->reason != MPV_END_FILE_REASON_ERROR) return;
        emit errorOccurred(tr("Could not play the video (mpv: %1)").arg(ended->error));
    });
    observePlaybackState();
    // The applied id, not the mode: a chain with no Mode is only recorded until
    // the engine exists, so reading the mode here would drop it before the
    // first frame.
    const QString chainId =
        anime4kChainId_.isEmpty() ? anime4k::modeId(anime4kMode_) : anime4kChainId_;
    const QString shaders = anime4k::shaderStringForCandidate(chainId);
    if (!shaders.isEmpty()) {
        try {
            mpv_->setString("glsl-shaders", shaders.toStdString());
            applyShaderChain(shaders);
        } catch (const MpvError& err) {
            emit errorOccurred(tr("Anime4K: %1").arg(QString::fromStdString(err.what())));
        }
    }
}

void VideoSurface::setAnime4KChain(const QString& candidateId) {
    ++anime4KRequestGeneration_;
    if (candidateId == anime4kChainId_) {
        return;
    }
    anime4kChainId_ = candidateId;
    // A named chain supersedes the enum, but the enum is not cleared: a later
    // setAnime4KMode rewrites both, and leaving it stale would let the two
    // disagree about what is on screen.
    if (!mpv_) {
        return;
    }
    const QString shaders = anime4k::shaderStringForCandidate(candidateId);
    try {
        mpv_->setString("glsl-shaders", shaders.toStdString());
        applyShaderChain(shaders);
    } catch (const MpvError& err) {
        emit errorOccurred(tr("Anime4K: %1").arg(QString::fromStdString(err.what())));
    }
    // vo=libmpv rebuilds the filter chain on the next rendered frame, and a
    // paused video produces none, so without this the change would appear to
    // do nothing until playback resumed.
    if (contextReady_ && render_) {
        render_->update();
    }
    update();
}
void VideoSurface::setAnime4KMode(anime4k::Mode mode) {
    ++anime4KRequestGeneration_;
    // The id is compared too: an override the compare window applied must be
    // taken back when the same mode is picked again.
    const QString id = anime4k::modeId(mode);
    if (mode == anime4kMode_ && id == anime4kChainId_) return;
    anime4kMode_ = mode;
    // The id is the persistence key, so it is recorded here too: a mode chosen
    // from the menu and a chain chosen in the compare window are the same
    // setting, and whichever came last must be what a restart restores.
    anime4kChainId_ = id;
    if (!mpv_) return;
    // The id is resolved, so a mode and a chain go through one path - and only
    // one of the two can name a chain that is not on disk.
    const QString shaders = anime4k::shaderStringForCandidate(anime4kChainId_);
    try {
        mpv_->setString("glsl-shaders", shaders.toStdString());
        applyShaderChain(shaders);
    } catch (const MpvError& err) {
        emit errorOccurred(tr("Anime4K: %1").arg(QString::fromStdString(err.what())));
    }
    // vo=libmpv only rebuilds the filter chain when a new frame is rendered,
    // and a paused video produces none, so the mode would appear to have no
    // effect until playback resumed.
    if (contextReady_ && render_) {
        render_->update();
    }
    update();
}

QString VideoSurface::effectiveShaderChain() const {
    if (!mpv_) return QString();
    // mpv only echoes this back - it does not validate a shader path list - so
    // no answer separates a working chain from a preset naming a deleted file.
    return shaderChain_;
}

void VideoSurface::setAnimeJanaiPlan(const native::AnimeJanaiPlan& plan) {
    const uint64_t currentSourceEpoch = mpv_ != nullptr ? mpv_->sourceEpoch() : 0;
    const bool samePlan =
        animeJanaiPlan_.enabled == plan.enabled &&
        animeJanaiPlan_.available == plan.available &&
        animeJanaiPlan_.presetId == plan.presetId &&
        animeJanaiPlan_.reasonKey == plan.reasonKey &&
        animeJanaiPlan_.reason == plan.reason &&
        animeJanaiPlan_.backend == plan.backend &&
        animeJanaiPlan_.slot == plan.slot &&
        animeJanaiPlan_.libraryPath == plan.libraryPath &&
        animeJanaiPlan_.configPath == plan.configPath &&
        animeJanaiPlan_.modelDir == plan.modelDir &&
        animeJanaiPlan_.rifeModelDir == plan.rifeModelDir &&
        animeJanaiPlan_.trtexecPath == plan.trtexecPath &&
        animeJanaiPlan_.trtexecLibDir == plan.trtexecLibDir &&
        animeJanaiPlan_.statsPath == plan.statsPath &&
        animeJanaiPlan_.filterSpec == plan.filterSpec;
    const bool preserveProcessingObservation =
        mpv_ != nullptr && samePlan && animeJanaiPlan_.enabled &&
        animeJanaiPlan_.available && animeJanaiFilterBound_ && !animeJanaiBusy_ &&
        !animeJanaiReportPending_ && animeJanaiObserved_ &&
        animeJanaiObservedEpoch_ == currentSourceEpoch &&
        animeJanaiEpoch_ == currentSourceEpoch &&
        animeJanaiObservedGeneration_ == animeJanaiGeneration_;
    const QString preservedObservedStatus = animeJanaiObservedStatus_;
    const bool sameConfiguration =
        animeJanaiPlan_.backend == plan.backend &&
        animeJanaiPlan_.libraryPath == plan.libraryPath &&
        animeJanaiPlan_.configPath == plan.configPath &&
        animeJanaiPlan_.modelDir == plan.modelDir &&
        animeJanaiPlan_.rifeModelDir == plan.rifeModelDir &&
        animeJanaiPlan_.trtexecPath == plan.trtexecPath &&
        animeJanaiPlan_.trtexecLibDir == plan.trtexecLibDir;
    const bool slotOnlyChange = animeJanaiFilterBound_ && plan.enabled &&
                                sameConfiguration && animeJanaiPlan_.slot != plan.slot;
    const bool configurationChange = animeJanaiFilterBound_ && plan.enabled &&
                                     !sameConfiguration;
    cancelAnimeJanaiOperation();
    animeJanaiPlan_ = plan;
    if (preserveProcessingObservation) {
        animeJanaiObserved_ = true;
        animeJanaiObservedEpoch_ = currentSourceEpoch;
        animeJanaiObservedGeneration_ = animeJanaiGeneration_;
        animeJanaiObservedStatus_ = preservedObservedStatus;
    }
    if (!mpv_) {
        if (!plan.enabled) {
            setAnimeJanaiStatus(QStringLiteral("Off"));
            return;
        }
        if (!plan.available) {
            animeJanaiFail(plan.reasonKey.isEmpty() ? QStringLiteral("animejanai.plan.unavailable")
                                                    : plan.reasonKey,
                           plan.reason.isEmpty() ? QStringLiteral("AnimeJaNai plan is unavailable")
                                                 : plan.reason);
            return;
        }
        setAnimeJanaiStatus(QStringLiteral("Configured; waiting for a source"));
        return;
    }
    animeJanaiEpoch_ = mpv_->sourceEpoch();
    try {
        mpv_->setBackendLogObservationEnabled(animeJanaiBackendLogsNeeded());
    } catch (const MpvError& error) {
        animeJanaiFail(QStringLiteral("animejanai.backend.failed"),
                       QString::fromUtf8(error.what()));
        return;
    }
    if (!plan.enabled) {
        const quint64 generation = animeJanaiGeneration_;
        const uint64_t epoch = animeJanaiEpoch_;
        if (!animeJanaiFilterBound_ && !animeJanaiEngineBuildActive_) {
            restoreAnimeJanaiHwdec(epoch, generation);
            return;
        }
        animeJanaiBusy_ = true;
        animeJanaiRequest_ = mpv_->commandAsync(
            {"vf", "remove", std::string("@") + native::kAnimeJanaiFilterLabel},
            [this, epoch, generation](const AsyncReply& reply) {
                if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                    generation != animeJanaiGeneration_) return;
                animeJanaiRequest_ = {};
                animeJanaiBusy_ = false;
                if (!reply.ok()) {
                    animeJanaiFail(QStringLiteral("animejanai.filter.removeRefused"),
                                   QString::fromStdString(mpv_->describeError(reply.error)));
                    return;
                }
                animeJanaiFilterBound_ = false;
                finishAnimeJanaiEngineBuild(false, tr("TensorRT engine build cancelled."));
                restoreAnimeJanaiHwdec(epoch, generation);
            });
        if (!animeJanaiRequest_.valid()) {
            animeJanaiBusy_ = false;
            animeJanaiFail(QStringLiteral("animejanai.filter.removeNotSubmitted"),
                           QStringLiteral("mpv did not accept the AnimeJaNai filter removal request"));
        }
        return;
    }
    if (!plan.available) {
        animeJanaiFail(plan.reasonKey.isEmpty() ? QStringLiteral("animejanai.plan.unavailable")
                                                : plan.reasonKey,
                       plan.reason.isEmpty() ? QStringLiteral("AnimeJaNai plan is unavailable")
                                             : plan.reason);
        return;
    }
    const QByteArray expectedFilterPrefix =
        QByteArray("@") + native::kAnimeJanaiFilterLabel + QByteArray(":");
    if (plan.filterSpec.isEmpty() || !plan.filterSpec.startsWith(expectedFilterPrefix)) {
        animeJanaiFail(QStringLiteral("animejanai.plan.filterSpecInvalid"),
                       QStringLiteral("available AnimeJaNai plan has no mikai_aji filter specification"));
        return;
    }
    if (plan.slot == 0) {
        animeJanaiFail(QStringLiteral("animejanai.slot.bypass"),
                       QStringLiteral("AnimeJaNai slot 0 is passthrough and cannot be reported as processing"));
        return;
    }
    if (configurationChange) {
        const uint64_t epoch = mpv_->sourceEpoch();
        const quint64 generation = animeJanaiGeneration_;
        animeJanaiBusy_ = true;
        setAnimeJanaiStatus(QStringLiteral("Switching AnimeJaNai configuration"));
        animeJanaiRequest_ = mpv_->commandAsync(
            {"vf", "remove", std::string("@") + native::kAnimeJanaiFilterLabel},
            [this, epoch, generation](const AsyncReply& reply) {
                if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                    generation != animeJanaiGeneration_) return;
                animeJanaiRequest_ = {};
                animeJanaiBusy_ = false;
                if (!reply.ok()) {
                    animeJanaiFail(QStringLiteral("animejanai.backend.removeRefused"),
                                   QString::fromStdString(mpv_->describeError(reply.error)));
                    return;
                }
                animeJanaiFilterBound_ = false;
                finishAnimeJanaiEngineBuild(false, tr("Previous TensorRT engine build stopped."));
                animeJanaiReconfigureAfterRestore_ = true;
                restoreAnimeJanaiHwdec(epoch, generation);
            });
        if (!animeJanaiRequest_.valid()) {
            animeJanaiBusy_ = false;
            animeJanaiFail(QStringLiteral("animejanai.backend.removeNotSubmitted"),
                           QStringLiteral("mpv did not accept removing the old backend filter"));
        }
        return;
    }
    const native::NativePlaybackController::Status nativeStatus = nativePlayback_.status();
    const native::NativePlaybackController::State nativeState = nativeStatus.state;
    // Controller states are not, by themselves, proof that the filter is in the
    // core: Idle is the normal unrequested state, and Attached can remain after
    // a refused add. The surface's bound bit includes an add/remove in flight
    // and is cleared only by a refused add or an observed removal.
    const bool stateCanBeUnbound =
        nativeState == native::NativePlaybackController::State::Idle ||
        nativeState == native::NativePlaybackController::State::Attached ||
        nativeState == native::NativePlaybackController::State::Off ||
        nativeState == native::NativePlaybackController::State::Failed;
    const bool noPrivateFilterOrTransition = !nativeFilterBound_ && stateCanBeUnbound;
    if (nativeFilterBound_ || !noPrivateFilterOrTransition) {
        const QString details =
            QStringLiteral("private mikai_native state=%1 bound=%2 reason=%3: %4; "
                           "AnimeJaNai cannot run concurrently")
                .arg(QString::fromLatin1(nativePlaybackStateName(nativeState)))
                .arg(nativeFilterBound_ ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(nativeStatus.reasonKey.isEmpty() ? QStringLiteral("none") : nativeStatus.reasonKey,
                     nativeStatus.reason.isEmpty() ? QStringLiteral("no additional detail")
                                                   : nativeStatus.reason);
        animeJanaiFail(QStringLiteral("animejanai.privateNativeConflict"), details);
        return;
    }
    const native::RuntimeSelection& selection = native::currentSelection();
    if (!native::isAnimeJanaiForkRuntime(selection)) {
        animeJanaiFail(QStringLiteral("animejanai.core.unsupported"),
                       QStringLiteral("AnimeJaNai requires the official Standard fork d6d93599d5; "
                                      "the loaded libmpv core is not that supported fork"));
        return;
    }
    if (slotOnlyChange) {
        const uint64_t epoch = mpv_->sourceEpoch();
        setAnimeJanaiStatus(QStringLiteral("Switching AnimeJaNai slot"));
        const quint64 generation = animeJanaiGeneration_;
        animeJanaiBusy_ = true;
        animeJanaiReportPending_ = true;
        animeJanaiRequest_ = mpv_->commandAsync(
            {"vf-command", native::kAnimeJanaiFilterLabel, "slot", std::to_string(plan.slot)},
            [this, epoch, generation](const AsyncReply& reply) {
                if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                    generation != animeJanaiGeneration_) return;
                animeJanaiRequest_ = {};
                animeJanaiBusy_ = false;
                if (!reply.ok()) {
                    animeJanaiFail(QStringLiteral("animejanai.slot.refused"),
                                   QString::fromStdString(mpv_->describeError(reply.error)));
                    return;
                }
                animeJanaiReportPending_ = false;
                setAnimeJanaiStatus(QStringLiteral(
                    "Configured; filter accepted (inference not observed)"));
                publishPendingAnimeJanaiObservation(epoch, generation);
            });
        if (!animeJanaiRequest_.valid()) {
            animeJanaiReportPending_ = false;
            animeJanaiBusy_ = false;
            animeJanaiFail(QStringLiteral("animejanai.slot.notSubmitted"),
                           QStringLiteral("mpv did not accept the AnimeJaNai slot request"));
        }
        return;
    }
    processAnimeJanaiPlan();
}

bool VideoSurface::animeJanaiProcessingObserved() const {
    return animeJanaiObserved_ && animeJanaiFilterBound_ && animeJanaiEpoch_ != 0 &&
           mpv_ != nullptr && animeJanaiObservedEpoch_ == mpv_->sourceEpoch() &&
           animeJanaiObservedGeneration_ == animeJanaiGeneration_;
}

void VideoSurface::resetAnimeJanaiObservation() {
    animeJanaiObserved_ = false;
    animeJanaiObservedEpoch_ = 0;
    animeJanaiObservedGeneration_ = 0;
    animeJanaiObservedStatus_.clear();
    animeJanaiPendingObservedStatus_.clear();
    animeJanaiPendingObservedEpoch_ = 0;
    animeJanaiPendingObservedGeneration_ = 0;
}

// libmpv has one client-level log threshold, so verbose delivery is scoped to
// the active official plan that consumes these backend reports.
bool VideoSurface::animeJanaiBackendLogsNeeded() const {
    return mpv_ != nullptr && animeJanaiSourceRequested_ && !pendingOpen_ &&
           animeJanaiPlan_.enabled && animeJanaiPlan_.available &&
           native::isAnimeJanaiForkRuntime(native::currentSelection());
}

void VideoSurface::observeAnimeJanaiBackendLog(const QString& prefix, const QString& line) {
    if (!animeJanaiBackendLogsNeeded() ||
        (!animeJanaiFilterBound_ && !animeJanaiReportPending_) ||
        (animeJanaiBusy_ && !animeJanaiReportPending_) || animeJanaiEpoch_ == 0 ||
        animeJanaiEpoch_ != mpv_->sourceEpoch()) return;

    const QString lower = (prefix + QLatin1Char(' ') + line).toLower();
    if (animeJanaiPlan_.backend == native::Backend::TensorRT &&
        (lower.contains(QStringLiteral("building tensorrt engine for ")) ||
         lower.contains(QStringLiteral("building engine:")) ||
         lower.contains(QStringLiteral("background engine build:")))) {
        if (!animeJanaiEngineBuildActive_) {
            animeJanaiBuildPollingRefused_ = false;
            animeJanaiBuildNextPollMs_ = 0;
            beginEnginePause();
        }
        animeJanaiEngineBuildActive_ = true;
        const QString detail = tr("Compiling the selected model for this video resolution.");
        setAnimeJanaiStatus(tr("Building TensorRT engine…"));
        emit animeJanaiEngineBuildChanged(true, false, detail);
    }
    const QString backendShort =
        animeJanaiPlan_.backend == native::Backend::TensorRT
            ? QStringLiteral("TRT")
            : QStringLiteral("DML");
    QString presetLabel = animeJanaiPlan_.presetId;
    for (const native::AnimeJanaiPreset& preset : native::animeJanaiPresets()) {
        if (preset.id.compare(animeJanaiPlan_.presetId, Qt::CaseInsensitive) == 0) {
            presetLabel = preset.label;
            break;
        }
    }
    QString observed;
    if (animeJanaiPlan_.presetId.startsWith(QStringLiteral("rife:"), Qt::CaseInsensitive) &&
        lower.contains(QStringLiteral("rife active:")) &&
        lower.contains(QStringLiteral("interpolation"))) {
        double factor = 2.0;
        static const QRegularExpression kRifeFactorRe(
            QStringLiteral("rife\\s+active:\\s*(\\d+)\\s*/\\s*(\\d+)"),
            QRegularExpression::CaseInsensitiveOption);
        if (const QRegularExpressionMatch m = kRifeFactorRe.match(line); m.hasMatch()) {
            const double num = m.captured(1).toDouble();
            const double den = m.captured(2).toDouble();
            if (num > 0.0 && den > 0.0) factor = num / den;
        }
        const double baseFps =
            observedContainerFps_ > 0.0
                ? observedContainerFps_
                : (streamFramesPerSecond_ > 0.0 ? streamFramesPerSecond_ / factor : 23.98);
        const double targetFps = baseFps * factor;
        const int srcH = videoHeight_ > 0
                             ? videoHeight_
                             : static_cast<int>(std::lround(observedVideoHeight_));
        if (srcH > 0) {
            observed = QStringLiteral("%1 · %2 · %3p · %4 → %5 FPS")
                           .arg(backendShort, presetLabel)
                           .arg(srcH)
                           .arg(baseFps, 0, 'f', 2)
                           .arg(targetFps, 0, 'f', 2);
        } else {
            observed = QStringLiteral("%1 · %2 · %3 → %4 FPS")
                           .arg(backendShort, presetLabel)
                           .arg(baseFps, 0, 'f', 2)
                           .arg(targetFps, 0, 'f', 2);
        }
    } else {
        const QString marker = QStringLiteral("configured slot %1:").arg(animeJanaiPlan_.slot);
        if (lower.contains(marker) && lower.contains(QStringLiteral(" -> "))) {
            int inW = videoWidth_ > 0 ? videoWidth_ : static_cast<int>(std::lround(observedVideoWidth_));
            int inH = videoHeight_ > 0 ? videoHeight_ : static_cast<int>(std::lround(observedVideoHeight_));
            int outW = inW * 2;
            int outH = inH * 2;
            static const QRegularExpression kSlotDimRe(
                QStringLiteral("(\\d+)x(\\d+)\\s*->\\s*(\\d+)x(\\d+)"),
                QRegularExpression::CaseInsensitiveOption);
            if (const QRegularExpressionMatch m = kSlotDimRe.match(line); m.hasMatch()) {
                inW = m.captured(1).toInt();
                inH = m.captured(2).toInt();
                outW = m.captured(3).toInt();
                outH = m.captured(4).toInt();
            }
            if (inH > 0 && outH > 0) {
                observed = QStringLiteral("%1 · %2 · %3p → %4p (%5×%6)")
                               .arg(backendShort, presetLabel)
                               .arg(inH)
                               .arg(outH)
                               .arg(outW)
                               .arg(outH);
            } else {
                observed = QStringLiteral("%1 · %2").arg(backendShort, presetLabel);
            }
        }
    }
    if (observed.isEmpty()) return;

    if (animeJanaiFilterBound_ && !animeJanaiReportPending_) {
        publishAnimeJanaiObservation(observed, animeJanaiEpoch_, animeJanaiGeneration_);
    } else {
        animeJanaiPendingObservedStatus_ = std::move(observed);
        animeJanaiPendingObservedEpoch_ = animeJanaiEpoch_;
        animeJanaiPendingObservedGeneration_ = animeJanaiGeneration_;
    }
}

void VideoSurface::publishAnimeJanaiObservation(const QString& status, uint64_t epoch,
                                                quint64 generation) {
    if (!animeJanaiBackendLogsNeeded() || !animeJanaiFilterBound_ ||
        epoch == 0 || epoch != mpv_->sourceEpoch() || epoch != animeJanaiEpoch_ ||
        generation != animeJanaiGeneration_) return;
    animeJanaiObserved_ = true;
    animeJanaiObservedEpoch_ = epoch;
    animeJanaiObservedGeneration_ = generation;
    animeJanaiObservedStatus_ = status;
    setAnimeJanaiStatus(status);
    finishAnimeJanaiEngineBuild(true, tr("TensorRT engine ready; processing started."));
}

void VideoSurface::publishPendingAnimeJanaiObservation(uint64_t epoch, quint64 generation) {
    const QString reported = std::move(animeJanaiPendingObservedStatus_);
    const uint64_t reportEpoch = animeJanaiPendingObservedEpoch_;
    const quint64 reportGeneration = animeJanaiPendingObservedGeneration_;
    animeJanaiPendingObservedEpoch_ = 0;
    animeJanaiPendingObservedGeneration_ = 0;
    if (!reported.isEmpty() && reportEpoch == epoch && reportGeneration == generation) {
        publishAnimeJanaiObservation(reported, reportEpoch, reportGeneration);
    }
}

void VideoSurface::setAnimeJanaiStatus(const QString& status) {
    if (animeJanaiObserved_ && animeJanaiObservedEpoch_ == (mpv_ ? mpv_->sourceEpoch() : 0) &&
        animeJanaiObservedGeneration_ == animeJanaiGeneration_ &&
        status != animeJanaiObservedStatus_) return;
    if (animeJanaiStatus_ == status) return;
    animeJanaiStatus_ = status;
    emit animeJanaiStatusChanged(status);
}

bool VideoSurface::enginePauseHeld() const {
    return enginePauseEpoch_ != 0 && mpv_ != nullptr &&
           enginePauseEpoch_ == mpv_->sourceEpoch() &&
           (enginePreparationPaused_ || animeJanaiEngineBuildActive_);
}

bool VideoSurface::playbackPauseIntent() const {
    return enginePauseHeld() ? enginePauseIntent_ : !isPlaying();
}

void VideoSurface::beginEnginePause() {
    if (enginePauseHeld()) return;
    enginePauseIntent_ = !isPlaying();
    enginePauseRequestGeneration_ = pauseRequestGeneration_;
    if (mpv_ == nullptr || mpv_->sourceEpoch() == 0) return;
    enginePauseEpoch_ = mpv_->sourceEpoch();
    reassertPaused();
    setPlaying(false);
}

void VideoSurface::endEnginePause(bool restoreIntent) {
    if (enginePreparationPaused_ || animeJanaiEngineBuildActive_) return;
    const bool sameSource = mpv_ != nullptr && enginePauseEpoch_ != 0 &&
                            enginePauseEpoch_ == mpv_->sourceEpoch();
    const bool paused = enginePauseIntent_;
    enginePauseEpoch_ = 0;
    if (!restoreIntent || !sameSource) return;
    try {
        mpv_->setPaused(paused);
        setPlaying(!paused);
    } catch (const MpvError& error) {
        emit errorOccurred(QString::fromUtf8(error.what()));
    }
}

void VideoSurface::discardEnginePause() {
    enginePauseEpoch_ = 0;
    enginePreparationPaused_ = false;
}

void VideoSurface::setEnginePreparationPaused(bool active, bool restoreIntent) {
    if (active == enginePreparationPaused_) return;
    if (active) beginEnginePause();
    enginePreparationPaused_ = active;
    if (!active) endEnginePause(restoreIntent);
}

void VideoSurface::pollAnimeJanaiEngineBuild() {
    if (!animeJanaiEngineBuildActive_ || !animeJanaiFilterBound_ || animeJanaiBusy_ ||
        animeJanaiBuildPollingRefused_ || mpv_ == nullptr ||
        animeJanaiBuildPollRequest_.valid() ||
        nativeStatusClock_.elapsed() < animeJanaiBuildNextPollMs_) return;
    animeJanaiBuildNextPollMs_ = nativeStatusClock_.elapsed() + 500;
    const uint64_t epoch = mpv_->sourceEpoch();
    const quint64 generation = animeJanaiGeneration_;
    // The official filter's poll command wakes completion without advancing playback.
    // The payload is required by the transport and ignored by the filter.
    animeJanaiBuildPollRequest_ = mpv_->commandAsync(
        {"vf-command", native::kAnimeJanaiFilterLabel, "poll", ""},
        [this, epoch, generation](const AsyncReply& reply) {
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                generation != animeJanaiGeneration_) return;
            animeJanaiBuildPollRequest_ = {};
            if (!reply.ok() && animeJanaiEngineBuildActive_) {
                animeJanaiBuildPollingRefused_ = true;
                emit animeJanaiFailure(QStringLiteral("animejanai.build.pollRefused: ") +
                                      QString::fromStdString(mpv_->describeError(reply.error)));
            }
        });
    if (!animeJanaiBuildPollRequest_.valid()) {
        animeJanaiBuildPollingRefused_ = true;
        emit animeJanaiFailure(QStringLiteral("animejanai.build.pollNotSubmitted"));
    }
}

void VideoSurface::finishAnimeJanaiEngineBuild(bool succeeded, const QString& detail) {
    if (!animeJanaiEngineBuildActive_) return;
    animeJanaiEngineBuildActive_ = false;
    endEnginePause(true);
    emit animeJanaiEngineBuildChanged(false, succeeded, detail);
}

void VideoSurface::animeJanaiFail(const QString& reasonKey, const QString& reason) {
    animeJanaiReportPending_ = false;
    resetAnimeJanaiObservation();
    if (mpv_ != nullptr) {
        try {
            mpv_->setBackendLogObservationEnabled(false);
        } catch (const MpvError& error) {
            emit errorOccurred(QString::fromUtf8(error.what()));
        }
    }
    const QString detail = reasonKey + QStringLiteral(": ") + reason;
    finishAnimeJanaiEngineBuild(false, detail);
    setAnimeJanaiStatus(QStringLiteral("Processing failed — ") + detail);
    emit animeJanaiFailure(detail);
}

void VideoSurface::cancelAnimeJanaiOperation() {
    if (mpv_ != nullptr && animeJanaiBuildPollRequest_.valid()) {
        mpv_->cancelAsync(animeJanaiBuildPollRequest_);
    }
    animeJanaiBuildPollRequest_ = {};
    ++animeJanaiGeneration_;
    animeJanaiReportPending_ = false;
    resetAnimeJanaiObservation();
    if (mpv_ != nullptr && animeJanaiRequest_.valid()) {
        mpv_->cancelAsync(animeJanaiRequest_);
    }
    animeJanaiRequest_ = {};
    animeJanaiBusy_ = false;
    animeJanaiReconfigureAfterRestore_ = false;
}

void VideoSurface::processAnimeJanaiPlan() {
    if (!mpv_ || !animeJanaiPlan_.enabled || !animeJanaiPlan_.available) return;
    try {
        mpv_->setBackendLogObservationEnabled(animeJanaiBackendLogsNeeded());
    } catch (const MpvError& error) {
        animeJanaiFail(QStringLiteral("animejanai.backend.failed"),
                       QString::fromUtf8(error.what()));
        return;
    }
    if (animeJanaiBusy_) return;
    if (!animeJanaiSourceRequested_ || url_.isEmpty() || pendingOpen_) {
        setAnimeJanaiStatus(QStringLiteral("Configured; waiting for a loaded source"));
        return;
    }
    if (animeJanaiFilterBound_) {
        setAnimeJanaiStatus(QStringLiteral("Configured; filter retained (inference not observed)"));
        return;
    }
    const uint64_t epoch = mpv_->sourceEpoch();
    const quint64 generation = animeJanaiGeneration_;
    animeJanaiBusy_ = true;
    setAnimeJanaiStatus(QStringLiteral("Configuring"));
    // Read and retain the user's decoder choice before temporarily aligning it.
    animeJanaiRequest_ = mpv_->getPropertyAsync(
        "hwdec", MPV_FORMAT_STRING,
        [this, epoch, generation](const AsyncReply& prior) {
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                generation != animeJanaiGeneration_) return;
            animeJanaiRequest_ = {};
            if (!prior.ok() || prior.format != MPV_FORMAT_STRING) {
                animeJanaiBusy_ = false;
                animeJanaiFail(QStringLiteral("animejanai.hwdec.previousUnavailable"),
                               QStringLiteral("could not read the current hwdec setting"));
                return;
            }
            if (animeJanaiPreviousHwdec_.isEmpty()) {
                animeJanaiPreviousHwdec_ = QString::fromStdString(prior.text);
            }
            const QString required = animeJanaiPlan_.backend == native::Backend::TensorRT
                                         ? QStringLiteral("nvdec")
                                         : QStringLiteral("d3d11va");
            const auto write = mpv_->commandAsync(
                {"set", "hwdec", required.toStdString()},
                [this, epoch, generation, required](const AsyncReply& written) {
                    if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                        generation != animeJanaiGeneration_) return;
                    animeJanaiRequest_ = {};
                    if (!written.ok()) {
                        animeJanaiBusy_ = false;
                        animeJanaiFail(QStringLiteral("animejanai.hwdec.writeRefused"),
                                       QString::fromStdString(mpv_->describeError(written.error)));
                        return;
                    }
                    animeJanaiRequest_ = mpv_->getPropertyAsync(
                        "hwdec", MPV_FORMAT_STRING,
                        [this, epoch, generation, required](const AsyncReply& observed) {
                            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                                generation != animeJanaiGeneration_) return;
                            animeJanaiRequest_ = {};
                            if (!observed.ok() || observed.format != MPV_FORMAT_STRING) {
                                animeJanaiBusy_ = false;
                                animeJanaiFail(QStringLiteral("animejanai.hwdec.readbackFailed"),
                                               QStringLiteral("could not read the decoder setting back"));
                                restoreAnimeJanaiHwdec(epoch, generation);
                                return;
                            }
                            const QString decoder = QString::fromStdString(observed.text);
                            if (decoder.compare(required, Qt::CaseInsensitive) != 0) {
                                animeJanaiBusy_ = false;
                                animeJanaiFail(QStringLiteral("animejanai.hwdec.incompatible"),
                                               QStringLiteral("the core reports hwdec='%1'; required '%2'")
                                                   .arg(decoder, required));
                                restoreAnimeJanaiHwdec(epoch, generation);
                                return;
                            }
                            const std::string spec(animeJanaiPlan_.filterSpec.constData(),
                                                   static_cast<size_t>(animeJanaiPlan_.filterSpec.size()));
                            animeJanaiReportPending_ = true;
                            animeJanaiRequest_ = mpv_->commandAsync(
                                {"vf", "add", spec},
                                [this, epoch, generation](const AsyncReply& attached) {
                                    if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                                        generation != animeJanaiGeneration_) return;
                                    animeJanaiRequest_ = {};
                                    animeJanaiBusy_ = false;
                                    if (!attached.ok()) {
                                        animeJanaiFail(QStringLiteral("animejanai.filter.attachRefused"),
                                                       QString::fromStdString(
                                                           mpv_->describeError(attached.error)));
                                        restoreAnimeJanaiHwdec(epoch, generation);
                                        return;
                                    }
                                    animeJanaiFilterBound_ = true;
                                    animeJanaiReportPending_ = false;
                                    setAnimeJanaiStatus(QStringLiteral(
                                        "Configured; filter accepted (inference not observed)"));
                                    publishPendingAnimeJanaiObservation(epoch, generation);
                                    if (contextReady_ && render_) render_->update();
                                    update();
                                });
                            if (!animeJanaiRequest_.valid()) {
                                animeJanaiReportPending_ = false;
                                animeJanaiBusy_ = false;
                                animeJanaiFail(QStringLiteral("animejanai.filter.attachNotSubmitted"),
                                               QStringLiteral("mpv did not accept the AnimeJaNai "
                                                              "filter request"));
                                restoreAnimeJanaiHwdec(epoch, generation);
                            }
                        });
                    if (!animeJanaiRequest_.valid()) {
                        animeJanaiBusy_ = false;
                        animeJanaiFail(QStringLiteral("animejanai.hwdec.readbackNotSubmitted"),
                                       QStringLiteral("mpv did not accept the hwdec readback request"));
                        restoreAnimeJanaiHwdec(epoch, generation);
                    }
                });
            if (!write.valid()) {
                animeJanaiBusy_ = false;
                animeJanaiFail(QStringLiteral("animejanai.hwdec.writeNotSubmitted"),
                               QStringLiteral("mpv did not accept the hwdec write request"));
            }
        });
    if (!animeJanaiRequest_.valid()) {
        animeJanaiBusy_ = false;
        animeJanaiFail(QStringLiteral("animejanai.hwdec.readNotSubmitted"),
                       QStringLiteral("mpv did not accept the previous hwdec read request"));
    }
}

void VideoSurface::restoreAnimeJanaiHwdec(uint64_t epoch, quint64 generation) {
    if (!mpv_ || animeJanaiPreviousHwdec_.isNull() || animeJanaiPreviousHwdec_.isEmpty()) {
        if (animeJanaiReconfigureAfterRestore_) {
            animeJanaiReconfigureAfterRestore_ = false;
            processAnimeJanaiPlan();
        } else if (!animeJanaiPlan_.enabled) {
            setAnimeJanaiStatus(QStringLiteral("Off"));
        }
        return;
    }
    const QString previous = animeJanaiPreviousHwdec_;
    animeJanaiPreviousHwdec_.clear();
    animeJanaiRequest_ = mpv_->commandAsync(
        {"set", "hwdec", previous.toStdString()},
        [this, epoch, generation](const AsyncReply& reply) {
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                generation != animeJanaiGeneration_) return;
            animeJanaiRequest_ = {};
            if (!reply.ok()) {
                animeJanaiReconfigureAfterRestore_ = false;
                animeJanaiFail(QStringLiteral("animejanai.hwdec.restoreRefused"),
                               QString::fromStdString(mpv_->describeError(reply.error)));
                return;
            }
            if (animeJanaiReconfigureAfterRestore_) {
                animeJanaiReconfigureAfterRestore_ = false;
                processAnimeJanaiPlan();
            } else if (!animeJanaiPlan_.enabled) {
                setAnimeJanaiStatus(QStringLiteral("Off"));
            }
        });
    if (!animeJanaiRequest_.valid()) {
        animeJanaiReconfigureAfterRestore_ = false;
        animeJanaiFail(QStringLiteral("animejanai.hwdec.restoreNotSubmitted"),
                       QStringLiteral("mpv did not accept restoring the previous hwdec setting"));
    }
}

void VideoSurface::applyShaderChain(const QString& shaders) {
    // Recorded on success so a caller reading straight after the write sees the
    // chain mpv was just given; the observer keeps it equal to what mpv holds
    // when anything else changes it.
    shaderChain_ = shaders;
}

bool VideoSurface::waitForContext(int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!contextReady_ && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(20);
    }
    return contextReady_;
}

VideoSurface::VideoSurface(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumSize(160, 90);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    // Without this the widget is composited as a separate texture and mpv's
    // frame would be blended against a stale backing store.
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    nativeStatusClock_.start();
    nativePlaybackClock_.start();
    diagnosticsClock_.start();
    // The controller never reaches the core itself: it hands this surface one
    // typed command at a time, and the async reply comes back through the same
    // drain as every other event.
    nativePlayback_.setCommandSink(
        [this](const native::NativePlaybackCommand& command) { deliverNativeCommand(command); });
    // Installed once, before any core exists. The reader calls this from
    // drainEvents on this thread, and the callback only forwards the published
    // view: nothing here can take the core lock or reach into mpv, so no
    // repaint or teardown can run it against a borrowed handle.
    nativeStatus_.setChangeCallback([this](const native::NativeStatusView& view) {
        // A refusal the reader will keep giving back - no filter attached, a
        // read the core would not take, or a filter document that has drifted
        // from this build's schema/protocol/tags - repeats identically for the
        // life of the source. Repeating the request every 500 ms would only
        // repeat the refusal, so the poll stops until the request state changes
        // or another source is opened. The Unavailable view is still forwarded:
        // the surface never invents an Active status.
        if (view.snapshot.has_value()) nativeSnapshotSeen_ = true;
        if (view.availability.value == native::Availability::Unavailable &&
            native::statusRefusalHaltsPolling(view.availability.reasonKey)) {
            // A filter this surface just attached may not have published its tag
            // map yet, so a missing or unreadable property is retried until the
            // first snapshot arrives. Every other refusal - a drifted schema, a
            // tag this build does not know - repeats identically, so it halts.
            const bool transient =
                nativeFilterBound_ && !nativeSnapshotSeen_ &&
                (view.availability.reasonKey == QStringLiteral("native.status.missing") ||
                 view.availability.reasonKey == QStringLiteral("native.status.request"));
            nativeStatusHalted_ = !transient;
        } else if (view.snapshot.has_value()) {
            // The first response named a state no poll can advance (Off,
            // Suspended, Failed, ...): stop after it and wait for a new
            // request, rather than reading the same terminal state forever. A
            // Probe snapshot is NOT one of those - the producer only fills in
            // its source and device facts when it adopts a decoded frame - so
            // the policy lives in one predicate beside the FPS resolver rather
            // than as a special case here.
            nativeStatusHalted_ = !nativeSnapshotKeepsPolling(view.snapshot->state);
        }
        // The reader published a genuine change, so it is the one observation
        // the controller may act on. It is read under the epoch the request was
        // issued at, so a reply for a superseded source cannot be acknowledged.
        if (mpv_) {
            applyNativeDecision(nativePlayback_.observe(view, mpv_->sourceEpoch(),
                                                       nativePlaybackNowMs()));
            // The producer's own source report: a moved display geometry or
            // cadence drops an admission still in flight. Nothing is loaded or
            // rebuilt here - the reset is the whole effect.
            if (view.snapshot.has_value() && view.snapshot->probe.has_value() &&
                view.snapshot->probe->source.has_value()) {
                nativePlayback_.onSourceGeometry(*view.snapshot->probe->source);
                publishNativePlaybackStatus();
            }
        }
        emit nativeStatusChanged(view);
    });
}

VideoSurface::~VideoSurface() {
    cancelAnimeJanaiOperation();
    // The render context holds a pointer to the mpv core, so it has to go first,
    // unconditionally. Members are destroyed in reverse declaration order, which
    // would do that anyway - but resetting mpv_ in the body destroys the core
    // here, and the render context's own destructor then runs against a freed
    // handle. Gating the render reset on the GL context being current used to
    // skip it precisely at process exit, where the context is already gone.
    if (render_) {
        // Freeing the render context releases GL objects, so OUR context has to
        // be current - not merely some context, and not none. At teardown there
        // usually is none: the widget is being deleted, not painted, and a
        // missing context is an access violation inside the driver rather than
        // anything mpv can report.
        const bool borrowed = context_ != nullptr && QOpenGLContext::currentContext() != context_;
        if (borrowed) QOpenGLWidget::makeCurrent();
        render_.reset();
        if (borrowed) QOpenGLWidget::doneCurrent();
    }
    if (mpv_) {
        // Unobserve for real before the core goes, so mpv is not still
        // formatting changes for a widget that is being destroyed.
        for (const uint64_t id : observers_) mpv_->unobserve(id);
        observers_.clear();
    }
    // The native-status reader borrows the same core: release the binding and
    // cancel any in-flight request before the core is reset, so no callback can
    // run against a widget that is going away.
    resetNativeStatusReader(false);
    resetDiagnostics();
    // The core's terminate-destroy synchronously frees the filter graph and its
    // native jobs. The controller's claims are held against exactly that work, so
    // they may only be released after it - never while the core, and therefore a
    // job that may still be reading an artifact, is alive.
    mpv_.reset();
    nativePlayback_.endSource();
}

bool VideoSurface::open(const QString& url) {
    discardEnginePause();
    finishAnimeJanaiEngineBuild(false, tr("The source changed during engine preparation."));
    animeJanaiSourceRequested_ = !url.isEmpty();
    pendingOpen_ = false;
    cancelAnimeJanaiOperation();
    animeJanaiEpoch_ = 0;
    if (animeJanaiPlan_.enabled) {
        setAnimeJanaiStatus(QStringLiteral("Configured; waiting for a loaded source"));
    }
    url_ = url;
    networkBytesPerSecond_ = 0.0;
    streamFramesPerSecond_ = 0.0;
    streamFpsSource_ = StreamFpsSource::Unknown;
    statsClock_.restart();
    observedContainerFps_ = 0.0;
    emit streamStatsChanged(0.0, 0.0, StreamFpsSource::Unknown);
    try {
        if (mpv_ != nullptr) {
            mpv_->setBackendLogObservationEnabled(animeJanaiBackendLogsNeeded());
        }
        ensureEngine();
    } catch (const MpvError& e) {
        // No core could be created - the selected runtime refused, or none was
        // selected. That is an unavailability to report, not an exception for a
        // caller that only asked whether the open worked: this function is the
        // first thing that touches the loader on the GUI thread.
        url_.clear();
        animeJanaiSourceRequested_ = false;
        pendingOpen_ = false;
        diagnosticsActive_ = false;
        if (mpv_ != nullptr) {
            try {
                mpv_->setBackendLogObservationEnabled(false);
            } catch (const MpvError&) {
            }
        }
        emit errorOccurred(QString::fromUtf8(e.what()));
        return false;
    }
    // A new source: the previous filter's snapshot says nothing about this one,
    // so the reader is rebound (which clears the published view) and a halt
    // recorded for the old source does not outlive it.
    resetNativeStatusReader(true);
    nativeStatusHalted_ = false;
    // A source is open, so the controller binds the core's epoch. The load below
    // advances it and the next pump rebinds; the previous session's claims are
    // deliberately kept, because a source change is not a stop.
    nativeSourceActive_ = true;
    syncNativeSource();
    // A new source: the old raw numbers say nothing about this one.
    resetDiagnostics();
    diagnosticsActive_ = true;
    // The load waits for the GL context. With vo=libmpv the video output
    // attaches to the render context, and mpv will not start a VO that has
    // nowhere to draw: loading first leaves the core with no video and a black
    // surface. Callers legitimately open before the widget is ever shown.
    if (!contextReady_) {
        pendingOpen_ = true;
        try {
            mpv_->setBackendLogObservationEnabled(false);
        } catch (const MpvError& e) {
            emit errorOccurred(QString::fromUtf8(e.what()));
        }
        return true;
    }
    return startPendingUrl();
}

bool VideoSurface::startPendingUrl() {
    pendingOpen_ = false;
    if (url_.isEmpty() || mpv_ == nullptr) {
        animeJanaiSourceRequested_ = false;
        return false;
    }
    try {
        mpv_->setBackendLogObservationEnabled(animeJanaiBackendLogsNeeded());
        mpv_->loadFile(url_.toStdString());
    } catch (const MpvError& e) {
        animeJanaiSourceRequested_ = false;
        try {
            mpv_->setBackendLogObservationEnabled(false);
        } catch (const MpvError&) {
        }
        emit errorOccurred(QString::fromUtf8(e.what()));
        return false;
    }
    // mpv also comes up paused when a render context is its only consumer: it
    // has no window of its own to drive playback. Without this the core decodes
    // one frame, hands it over and stops, which reads as a black surface and a
    // frozen clock rather than as an obvious failure.
    try {
        if (enginePreparationPaused_) {
            enginePauseEpoch_ = mpv_->sourceEpoch();
            mpv_->setPaused(true);
            setPlaying(false);
        } else {
            mpv_->setPaused(false);
        }
    } catch (const MpvError&) {
        // The host may intend to open paused and unpause itself.
    }

    // The load advanced the core's source epoch. Binding it here means the epoch
    // a caller reads is the one the admission is requested under, instead of the
    // two racing a rebind on the next pump.
    syncNativeSource();
    animeJanaiEpoch_ = mpv_->sourceEpoch();
    if (animeJanaiPlan_.enabled) processAnimeJanaiPlan();
    return true;
}

void VideoSurface::close() {
    discardEnginePause();
    finishAnimeJanaiEngineBuild(false, tr("Playback closed during engine preparation."));
    animeJanaiSourceRequested_ = false;
    pendingOpen_ = false;
    cancelAnimeJanaiOperation();
    animeJanaiEpoch_ = 0;
    setAnimeJanaiStatus(animeJanaiPlan_.enabled
                            ? QStringLiteral("Configured; waiting for a loaded source")
                            : QStringLiteral("Off"));
    if (!mpv_) return;
    try {
        mpv_->setBackendLogObservationEnabled(false);
    } catch (const MpvError& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
    }
    // Nothing is loaded after this: drop the reader before the source ends so
    // no request in flight can describe a file that is gone, and clear the halt
    // that belonged to the old source.
    resetNativeStatusReader(false);
    nativeStatusHalted_ = false;
    resetDiagnostics();
    diagnosticsActive_ = false;
    try {
        mpv_->stop();
        animeJanaiEpoch_ = mpv_->sourceEpoch();
        emit sourceEpochChanged(mpv_->sourceEpoch());
    } catch (const MpvError&) {
        // A stop on an already-dead core is not worth reporting.
    }
    animeJanaiEpoch_ = 0;
    // Emitted before the reset so the chrome falls back to its play glyph
    // while the surface still reports a loaded file.
    if (playing_) {
        playing_ = false;
        emit playingChanged(false);
    }
    hasVideo_ = false;
    position_ = 0.0;
    duration_ = 0.0;
    videoWidth_ = videoHeight_ = 0;
    networkBytesPerSecond_ = 0.0;
    streamFramesPerSecond_ = 0.0;
    streamFpsSource_ = StreamFpsSource::Unknown;
    // The core keeps reporting while nothing is loaded, and the next open must
    // not start from the previous file's numbers.
    observedDuration_ = 0.0;
    observedPosition_ = 0.0;
    observedVideoWidth_ = 0.0;
    observedVideoHeight_ = 0.0;
    observedCacheSpeed_ = 0.0;
    observedContainerFps_ = 0.0;
    emit streamStatsChanged(0.0, 0.0, StreamFpsSource::Unknown);
    handleState();
}

void VideoSurface::togglePause() {
    setPaused(enginePauseHeld() ? !enginePauseIntent_ : !observedPaused_);
}

void VideoSurface::setPaused(bool paused) {
    ++pauseRequestGeneration_;
    const bool held = enginePauseHeld();
    if ((held || enginePreparationPaused_) &&
        pauseRequestGeneration_ > enginePauseRequestGeneration_) {
        enginePauseIntent_ = paused;
    }
    if (!mpv_) return;
    try {
        mpv_->setPaused(held || paused);
        observedPaused_ = held || paused;
        setPlaying(!held && !paused);
    } catch (const MpvError& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
    }
}

void VideoSurface::reassertPaused() {
    if (!mpv_) return;
    try {
        mpv_->setPaused(true);
        observedPaused_ = true;
        setPlaying(false);
    } catch (const MpvError& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
    }
}

void VideoSurface::seek(double seconds) {
    if (!mpv_) return;
    try {
        mpv_->seek(seconds);
    } catch (const MpvError&) {
        // Seeking before a file is loaded is normal during startup.
    }
}

void VideoSurface::setVolume(double volume) {
    if (!mpv_) return;
    try {
        mpv_->setVolume(volume);
    } catch (const MpvError&) {}
}

void VideoSurface::setMuted(bool muted) {
    if (!mpv_) return;
    try {
        mpv_->setMuted(muted);
    } catch (const MpvError&) {}
}

void VideoSurface::setSpeed(double speed) {
    if (!mpv_) return;
    try {
        mpv_->setSpeed(speed);
    } catch (const MpvError&) {}
}

bool VideoSurface::isPlaying() const { return playing_; }

std::optional<ProbePropertyValue> VideoSurface::probeMpvProperty(const QString& name,
                                                               ProbePropertyKind want) {
    if (mpv_ == nullptr) return std::nullopt;
    const std::string property = name.toStdString();
    try {
        switch (want) {
            case ProbePropertyKind::Flag: {
                ProbePropertyValue value;
                value.kind = ProbePropertyValue::Kind::Flag;
                value.flag = mpv_->getFlag(property);
                return value;
            }
            case ProbePropertyKind::Text: {
                ProbePropertyValue value;
                value.kind = ProbePropertyValue::Kind::Text;
                value.text = QString::fromStdString(mpv_->getString(property));
                return value;
            }
            case ProbePropertyKind::Number:
                break;
        }
    } catch (const MpvError&) {
        return std::nullopt;
    }
    try {
        ProbePropertyValue value;
        value.kind = ProbePropertyValue::Kind::Double;
        value.number = mpv_->getNumber(property);
        return value;
    } catch (const MpvError&) {
        return std::nullopt;
    }
}

AsyncRequestId VideoSurface::requestPlaybackTimestamp(
    std::function<void(uint64_t, std::optional<double>)> callback) {
    if (mpv_ == nullptr || !callback) {
        return {};
    }
    const uint64_t epoch = mpv_->sourceEpoch();
    return mpv_->getPropertyAsync(
        "time-pos", MPV_FORMAT_DOUBLE,
        [this, epoch, callback = std::move(callback)](const AsyncReply& reply) mutable {
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() || !reply.ok() ||
                reply.format != MPV_FORMAT_DOUBLE) {
                callback(epoch, std::nullopt);
                return;
            }
            callback(epoch, reply.number);
        });
}

bool VideoSurface::probeCycleMpvTrack(const QString& property) {
    if (mpv_ == nullptr ||
        (property != QStringLiteral("aid") && property != QStringLiteral("sid"))) {
        return false;
    }
    return mpv_->commandAsync({"cycle", property.toStdString()},
                              [](const AsyncReply&) {}).valid();
}

void VideoSurface::setPlaying(bool playing) {
    // Announced, not just recorded. The state pump cannot notice a change this
    // class makes itself: it compares the core against this very flag, so
    // writing it here and staying silent left the pump nothing to report and
    // the play control frozen on its old glyph - the video paused behind a
    // "play" button, or running behind a "pause" one.
    if (playing == playing_) {
        return;
    }
    playing_ = playing;
    emit playingChanged(playing_);
}
double VideoSurface::position() const { return position_; }

double VideoSurface::duration() const { return duration_; }
int VideoSurface::videoWidth() const { return videoWidth_; }
int VideoSurface::videoHeight() const { return videoHeight_; }
bool VideoSurface::hasVideo() const { return hasVideo_; }
uint64_t VideoSurface::sourceEpoch() const {
    return mpv_ != nullptr ? mpv_->sourceEpoch() : 0;
}

// ── GL lifecycle ──────────────────────────────────────────────────────────

void VideoSurface::initializeGL() {
    QOpenGLWidget::initializeGL();
    context_ = QOpenGLContext::currentContext();
    try {
        const GlApi gl = loadGlApi();
        ensureEngine();
        render_ = std::make_unique<MpvRenderContext>(*mpv_, gl);
        // mpv draws with the origin at the bottom-left; Qt composites the
        // QOpenGLWidget's framebuffer from the top-left. Without this the picture
        // is upside down - which a non-black pixel check happily passes.
        render_->setFlipY(true);
        render_->setUpdateCallback([this] {
            // Fires on an mpv thread. Hop to the GUI thread rather than
            // touching the widget from here.
            QMetaObject::invokeMethod(this, [this] { framePending_ = true; update(); },
                                      Qt::QueuedConnection);
        });
        contextReady_ = true;
        // The first paint after the context exists has to render whatever mpv
        // is holding, even if no frame has been decoded yet: the framebuffer
        // has never been drawn into.
        forceRender_ = true;
        statePump_.setInterval(50);
        connect(&statePump_, &QTimer::timeout, this, &VideoSurface::pumpEvents);
        statePump_.start();
        if (pendingOpen_) startPendingUrl();
    } catch (const MpvError& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
        contextReady_ = false;
    }
}

void VideoSurface::resizeGL(int width, int height) {
    QOpenGLWidget::resizeGL(width, height);
    // Qt hands QOpenGLWidget a freshly allocated framebuffer on a resize, so
    // whatever the last paint drew into the old one is gone. The next paint
    // has to render even though no frame has been decoded since.
    forceRender_ = true;
    if (contextReady_) render_->update();
    // Ask for that paint rather than hoping Qt repaints on its own: the fresh
    // buffer has no defined content, and with a paused core nothing else will
    // schedule one, so the widget would composite undefined pixels.
    update();
}

void VideoSurface::showEvent(QShowEvent* event) {
    QOpenGLWidget::showEvent(event);
    // Same reasoning as resizeGL: a surface that was hidden (the pooled PiP
    // float) cannot assume its framebuffer still holds the last frame, and
    // with a paused core there may never be another one.
    forceRender_ = true;
    if (contextReady_ && render_) {
        render_->update();
        update();
    }
}

void VideoSurface::paintGL() {
    QOpenGLWidget::paintGL();
    if (!contextReady_ || !render_) {
        return;
    }
    // The state pump belongs on the 50 ms timer, not here: paintGL runs only
    // when there is a frame, so a paused core - which produces none - would
    // otherwise freeze the clock and every HUD value with the last picture.
    //
    // MPV_RENDER_UPDATE_FRAME marks a new output frame, independent of UI paints.
    const uint64_t updateFlags = render_->update();
    const bool newOutputFrame = (updateFlags & MPV_RENDER_UPDATE_FRAME) != 0;
    // forceRender_ covers the framebuffer Qt hands back empty: a resize or a
    // re-show wipes the picture, and with a paused core no new frame is coming,
    // so grabFramebuffer() - which reads that buffer instead of repainting -
    // would otherwise read a buffer nothing has drawn into.
    const bool haveFrame = framePending_ || newOutputFrame || forceRender_;

    // Qt renders QOpenGLWidget into its own framebuffer object, so fbo=0 would
    // make mpv bind the window's default framebuffer and paint nothing. The FBO
    // is also sized in device pixels, so passing logical width()/height() fills
    // only the bottom-left quarter on a HiDPI display.
    const GLuint target = static_cast<GLuint>(defaultFramebufferObject());
    const qreal ratio = devicePixelRatioF();
    const int targetWidth = std::max(1, static_cast<int>(width() * ratio));
    const int targetHeight = std::max(1, static_cast<int>(height() * ratio));
    // QOpenGLWidget guarantees its own context is current here, so the render
    // context and this FBO are the ones these entry points act on.
    QOpenGLFunctions* gl = QOpenGLContext::currentContext()->functions();
    // Clear reused Qt buffers before rendering, including pixels outside the video rectangle.
    gl->glBindFramebuffer(GL_FRAMEBUFFER, target);
    gl->glViewport(0, 0, targetWidth, targetHeight);
    // Qt enables blending before paintGL; libmpv requires its default disabled state.
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    const int renderResult = render_->render(static_cast<int>(target), targetWidth, targetHeight, 0);
    render_->reportSwap();
    hasRenderedFrame_ = (renderResult == 0);
    lastRenderedFboSize_ = QSize(targetWidth, targetHeight);
    if (newOutputFrame && renderResult == 0) ++diagnostics_.renderedOutputFrames;
    if (renderResult < 0 && mpv_ != nullptr) {
        // A refused render leaves only the defined black above behind, so it is
        // named instead of being presented as a frame.
        const QString message =
            tr("Video render failed (mpv: %1)")
                .arg(QString::fromStdString(mpv_->describeError(renderResult)));
        QMetaObject::invokeMethod(this, [this, message] { emit errorOccurred(message); },
                                  Qt::QueuedConnection);
    }
    // mpv unbinds; Qt invalidates next.
    gl->glBindFramebuffer(GL_FRAMEBUFFER, target);
    framePending_ = false;
    forceRender_ = false;

    // Only schedule the next frame if a fresh decoded frame arrived. When
    // paused, haveFrame is false so it stops scheduling updates and sits still
    // on the current frame without spinning or going black.
    if (haveFrame) {
        QOpenGLWidget::update();
    }
}

// ── state pump ────────────────────────────────────────────────────────────

void VideoSurface::observePlaybackState() {
    if (!mpv_) return;
    if (!observers_.empty()) return;
    // The registered format is what mpv delivers, so each observer reads
    // exactly what it wrote.
    const auto watch = [this](const char* name, mpv_format format, auto&& apply) {
        // A property this core does not have costs the HUD that one number, not
        // playback, so a refusal is reported and stepped over.
        try {
            const uint64_t id = mpv_->observe(
                name, format, [apply = std::forward<decltype(apply)>(apply)](const Property& p) {
                    apply(p);
                });
            if (id != 0) observers_.push_back(id);
        } catch (const MpvError& error) {
            qWarning("mpv: cannot observe %s (%s)", name, error.what());
        }
    };
    watch("duration", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedDuration_ = p.isNumber() ? p.number : 0.0; });
    watch("time-pos", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedPosition_ = p.isNumber() ? p.number : 0.0; });
    watch("pause", MPV_FORMAT_FLAG, [this](const Property& p) {
        bool paused = p.isFlag() && p.flag;
        if (!paused && mpv_) {
            try {
                paused = mpv_->getFlag("pause");
            } catch (const MpvError&) {
            }
        }
        observedPaused_ = paused;
    });
    watch("video-params/w", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedVideoWidth_ = p.isNumber() ? p.number : 0.0; });
    watch("video-params/h", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedVideoHeight_ = p.isNumber() ? p.number : 0.0; });
    watch("cache-speed", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedCacheSpeed_ = p.isNumber() ? p.number : 0.0; });
    watch("estimated-vf-fps", MPV_FORMAT_DOUBLE, [this](const Property& p) {
        if (diagnosticsActive_ && p.isNumber()) {
            diagnostics_.estimatedVfFps = p.number;
        } else {
            diagnostics_.estimatedVfFps.reset();
        }
    });
    watch("container-fps", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedContainerFps_ = p.isNumber() ? p.number : 0.0; });
    watch("glsl-shaders", MPV_FORMAT_STRING,
          [this](const Property& p) { shaderChain_ = QString::fromStdString(p.text); });
}

void VideoSurface::publishObservedState() {
    if (observedDuration_ > 0 && observedDuration_ != duration_) {
        duration_ = observedDuration_;
        emit durationChanged(duration_);
    }
    // 10 ms is finer than any display, so this cannot look stepped.
    if (std::abs(observedPosition_ - position_) > 0.01) {
        position_ = observedPosition_;
        emit positionChanged(position_);
    }
    // Compare like with like: the property is "pause", the cached state is
    // "playing". Guarding on `paused != playing_` means a playing video
    // (false vs false) never updates, so isPlaying() stays false forever.
    const bool playing = !observedPaused_ && !enginePauseHeld();
    if (playing != playing_) {
        playing_ = playing;
        emit playingChanged(playing_);
    }
}

void VideoSurface::pumpEvents() {
    if (!mpv_) return;
    // Before the drain, so an observation dispatched from it is read under the
    // epoch the controller is currently bound to.
    syncNativeSource();
    if (animeJanaiEpoch_ != 0 && animeJanaiEpoch_ != mpv_->sourceEpoch()) {
        discardEnginePause();
        finishAnimeJanaiEngineBuild(false, tr("The source changed during engine preparation."));
        cancelAnimeJanaiOperation();
        animeJanaiEpoch_ = mpv_->sourceEpoch();
        if (animeJanaiPlan_.enabled) processAnimeJanaiPlan();
    }
    try {
        mpv_->drainEvents();
    } catch (const MpvError&) {
        return;
    }
    publishObservedState();
    handleState();
    updateStreamStats();
    // The last thing the pump does: a request issued here is answered by the
    // next pump's drainEvents, so a reply is never delivered re-entrantly from
    // inside the tick that issued it.
    pollNativeStatus();
    pollDiagnostics();
    pollAnimeJanaiEngineBuild();
    // Bounded deadline for the native transition in flight: a command the core
    // accepted but the filter never acknowledged ends in a named non-Active
    // verdict rather than hanging the transition forever.
    applyNativeDecision(nativePlayback_.tick(nativePlaybackNowMs()));
}

void VideoSurface::updateStreamStats() {
    if (!statsClock_.isValid()) {
        statsClock_.start();
        return;
    }
    if (statsClock_.elapsed() < 500) {
        return;
    }
    statsClock_.restart();

    // Observed, not read: these two belong to a HUD label and must not wait on
    // the core lock.
    const double bytesPerSecond = std::max(0.0, observedCacheSpeed_);
    const ResolvedStreamFps resolvedFps =
        resolveStreamFps(diagnostics_.estimatedVfFps, observedContainerFps_,
                         diagnosticsActive_);
    const double framesPerSecond = resolvedFps.framesPerSecond;
    const StreamFpsSource fpsSource = resolvedFps.source;
    if (std::abs(bytesPerSecond - networkBytesPerSecond_) < 1024.0 &&
        std::abs(framesPerSecond - streamFramesPerSecond_) < 0.01 &&
        fpsSource == streamFpsSource_) {
        return;
    }
    networkBytesPerSecond_ = bytesPerSecond;
    streamFramesPerSecond_ = framesPerSecond;
    streamFpsSource_ = fpsSource;
    emit streamStatsChanged(networkBytesPerSecond_, streamFramesPerSecond_,
                            streamFpsSource_);
}

void VideoSurface::handleState() {
    if (!mpv_) return;
    // One value, two answers. The width decides both whether there is a video at
    // all and how wide it is, and it was observed rather than read, so this costs
    // no trip through the core lock against mpv's own render thread.
    const int w = static_cast<int>(observedVideoWidth_);
    const bool vid = w > 0;
    if (vid != hasVideo_) hasVideo_ = vid;
    if (hasVideo_) {
        const int h = static_cast<int>(observedVideoHeight_);
        if (w != videoWidth_ || h != videoHeight_) {
            videoWidth_ = w;
            videoHeight_ = h;
            emit videoSizeChanged(w, h);
        }
    }
}

// ── native status ─────────────────────────────────────────────────────────

void VideoSurface::setNativeStatusPolling(bool enabled) {
    // A call here is a request-state change: it re-arms a poll that a terminal
    // state or an absent filter had stopped. Nothing is read synchronously -
    // the next pump step is what issues the request, so a caller on the GUI
    // thread never waits on the core lock.
    if (enabled) nativeStatusHalted_ = false;
    nativeStatusPolling_ = enabled;
}

bool VideoSurface::nativeStatusShouldPoll() const {
    // A transition in flight must be observed or its verdict can never arrive -
    // an explicit off/drain would otherwise sit unacknowledged once the caller
    // stopped asking for the status. The caller's own request still decides
    // everything else, so a surface with no native work issues no read.
    const bool transitionPending = !nativePlayback_.status().pendingRequestId.isEmpty();
    return (nativeStatusPolling_ || transitionPending) && !nativeStatusHalted_;
}

void VideoSurface::pollNativeStatus() {
    if (!mpv_) return;
    // The reader is ticked even when it will not issue a request: the tick
    // still abandons a timed-out one and keeps the current source epoch in
    // view. `sourceEpoch()` is bumped by load and stop, so a reply for a
    // superseded source is dropped by the reader rather than published.
    nativeStatus_.tick(nativeStatusClock_.elapsed(), mpv_->sourceEpoch(),
                       nativeStatusShouldPoll());
}

void VideoSurface::resetNativeStatusReader(bool reattach) {
    // Cancels any in-flight request and clears the published view, so a plan
    // from the old source cannot keep reading as current. detach() is safe on a
    // reader that was never attached, which is what a close() before open()
    // looks like.
    nativeStatus_.detach();
    // The published snapshot went with the binding, so the next one is a first
    // for whatever core is bound now.
    nativeSnapshotSeen_ = false;
    if (reattach && mpv_) nativeStatus_.attach(mpv_.get());
}

// ── native playback ───────────────────────────────────────────────────────

void VideoSurface::syncNativeSource() {
    if (mpv_ == nullptr || !nativeSourceActive_) return;
    const uint64_t epoch = mpv_->sourceEpoch();
    if (epoch == nativeBoundEpoch_) return;
    nativeBoundEpoch_ = epoch;
    emit sourceEpochChanged(epoch);
    // A capture in flight belonged to the source being replaced: the core drops
    // its reply, so the caller is told now instead of waiting forever.
    abandonNativeReferenceFrame(
        QStringLiteral("native.reference.staleSource"),
        QStringLiteral("the source changed while the capture was in flight"));
    // The decoder and its alignment belonged to the source that just ended, so
    // they go with it: the next arm must re-align, and a context must never report
    // the previous source's decoder as this one's. The duplicate-detection digest
    // is dropped for the same reason - it described that source's frame.
    nativeProbeAligned_ = false;
    nativeProbeDecoder_.clear();
    nativeProbeBackend_.reset();
    nativeProbeAlign_.reset();
    nativeReferenceLastHash_.clear();
    nativeReferenceLastEpoch_ = 0;
    // A new epoch: the controller discards every reply and observation read
    // under the old one. The effective plan is dropped with it, but the held
    // claims are not - only an acknowledged stop or core death releases those.
    nativePlayback_.beginSource(epoch);
    // The reader's published document was accepted under the epoch that just
    // ended, so it is dropped with that epoch: an old source's probe or factory
    // observation must not keep reading as current for this one, and the next
    // accepted document is a first for this source. The binding state is
    // preserved - a core that was being read stays being read, and a surface
    // whose reader a close() unbound is not silently reattached.
    resetNativeStatusReader(nativeStatus_.attached());
}

void VideoSurface::armNativeObservation() {
    // An explicit native request is a request-state change, which is exactly
    // what re-arms a poll an absent filter or a terminal state had stopped.
    // Without this the admission would time out against a stale halt. The next
    // snapshot is a fresh one for the request, so a transient refusal retries.
    nativeStatusHalted_ = false;
    nativeSnapshotSeen_ = false;
}

void VideoSurface::armNativeProbe(native::Backend backend) {
    const QString unavailable = privateNativeRuntimeUnavailableReason();
    if (!unavailable.isEmpty()) {
        emit nativeProcessingFailure(QStringLiteral("native.runtimeUnavailable"), unavailable);
        return;
    }
    if (mpv_ == nullptr) return;
    syncNativeSource();
    const native::NativePlaybackController::State state = nativePlayback_.state();
    if (state != native::NativePlaybackController::State::Idle &&
        state != native::NativePlaybackController::State::Off &&
        state != native::NativePlaybackController::State::Failed) {
        return;  // already bound to this source's filter
    }
    armNativeObservation();
    // The filter observes whatever frames arrive next, so the decoder has to be
    // settled BEFORE it is attached: a filter that has already seen frames cannot
    // be told which decoder they came from. The alignment is asynchronous and
    // attaches only once the core itself reports a decoder this backend can use.
    beginNativeProbeAlignment({backend, mpv_->sourceEpoch(), backendHwdec(backend)});
}

bool VideoSurface::decoderServesBackend(const QString& decoder, native::Backend backend) {
    return decoderFeedsBackend(decoder, backend);
}

QString VideoSurface::hwdecForBackend(native::Backend backend) { return backendHwdec(backend); }

void VideoSurface::beginNativeProbeAlignment(const NativeProbeAlignment& alignment) {
    if (mpv_ == nullptr) return;
    // One alignment at a time: a second request while one is proving something
    // would leave two answers to compare against a single decoder.
    if (nativeProbeAlignRequest_.valid() || nativeProbeAlign_.has_value()) return;
    if (alignment.required.isEmpty()) {
        emit nativeProcessingFailure(
            QStringLiteral("native.probe.noDecoderForBackend"),
            tr("This build names no decoder for the requested provider, so nothing can be "
               "aligned to it."));
        return;
    }
    nativeProbeAlign_ = alignment;
    // Writing `hwdec` is what starts the alignment: mpv reinits the decoder for the
    // current track and re-seeks to the position it was already at, so the pause,
    // the speed, the mute and the track selection are preserved by mpv itself
    // rather than re-established here. The write is asynchronous because
    // everything on this surface is.
    mpv_->commandAsync({"set", "hwdec", alignment.required.toStdString()},
                       [this, alignment](const AsyncReply& reply) {
                           if (!reply.ok()) {
                               // A refused write means the core would not take the
                               // decoder this backend needs. That is a refusal, not
                               // a reason to attach and observe something else.
                               QString reason;
                               if (mpv_ != nullptr) {
                                   reason = QString::fromStdString(mpv_->describeError(reply.error));
                               }
                               cancelNativeProbeAlignment(
                                   QStringLiteral("native.probe.alignmentRefused"),
                                   tr("the core refused the %1 decoder this provider needs (%2)")
                                       .arg(alignment.required, reason));
                               return;
                           }
                           // The decoder is only real once the core reports it, so
                           // read it back rather than believing the write.
                           NativeProbeRead read;
                           read.kind = NativeProbeRead::Kind::Hwdec;
                           read.epoch = alignment.epoch;
                           read.backend = alignment.backend;
                           nativeProbeAlignRequest_ = mpv_->getPropertyAsync(
                               "hwdec-current", MPV_FORMAT_STRING,
                               [this, read](const AsyncReply& answer) {
                                   nativeProbeAlignRequest_ = AsyncRequestId{};
                                   NativeProbeRead resolved = read;
                                   resolved.hwdec =
                                       answer.ok() ? QString::fromStdString(answer.text) : QString();
                                   applyNativeProbeAlignmentReply(resolved);
                               });
                           if (!nativeProbeAlignRequest_.valid()) {
                               cancelNativeProbeAlignment(
                                   QStringLiteral("native.probe.request"),
                                   tr("the core would not take the decoder read-back"));
                           }
                       });
}

void VideoSurface::applyNativeProbeAlignmentReply(const NativeProbeRead& read) {
    if (mpv_ == nullptr || !nativeProbeAlign_.has_value()) return;
    const NativeProbeAlignment alignment = *nativeProbeAlign_;
    // A reply for another source, or for another backend's alignment, answers
    // nothing: the decoder it describes belongs to a request that is gone.
    if (read.epoch != alignment.epoch || !read.backend.has_value() ||
        *read.backend != alignment.backend) {
        return;
    }
    QString reasonKey;
    QString reason;
    bool accepted = false;
    if (read.hwdec.isEmpty()) {
        // `hwdec-current` is unavailable until a decoder exists, so an absent
        // answer is not evidence of a software decode - it is no answer at all.
        reasonKey = QStringLiteral("native.probe.decoderUnreported");
        reason = tr("the core did not report which decoder it is using");
    } else if (!decoderServesBackend(read.hwdec, alignment.backend)) {
        reasonKey = QStringLiteral("native.probe.decoderIncompatible");
        reason = tr("the core is decoding with '%1', which cannot feed the requested provider "
                    "(%2)")
                     .arg(read.hwdec, native::stableName(alignment.backend));
    } else {
        accepted = true;
    }
    finishNativeProbeAlignment(accepted, read.hwdec, &reasonKey, &reason);
}

void VideoSurface::finishNativeProbeAlignment(bool accepted,
                                              const QString& observedDecoder,
                                              QString* reasonKey, QString* reason) {
    const std::optional<NativeProbeAlignment> alignment = nativeProbeAlign_;
    nativeProbeAlign_.reset();
    if (mpv_ == nullptr || !alignment.has_value()) return;
    if (accepted) {
        const QString unavailable = privateNativeRuntimeUnavailableReason();
        if (!unavailable.isEmpty()) {
            emit nativeProcessingFailure(QStringLiteral("native.runtimeUnavailable"), unavailable);
            return;
        }
    }
    if (!accepted) {
        emit nativeProcessingFailure(reasonKey != nullptr && !reasonKey->isEmpty()
                                         ? *reasonKey
                                         : QStringLiteral("native.probe.alignmentRefused"),
                                     reason != nullptr && !reason->isEmpty()
                                         ? *reason
                                         : tr("the core would not align to a decoder the "
                                              "requested provider can use"));
        return;
    }
    // The OBSERVED decoder, not the requested one: it is what the core reports it
    // is using, and it is the decoder the filter is about to watch frames from.
    nativeProbeDecoder_ = observedDecoder;
    nativeProbeBackend_ = alignment->backend;
    nativeProbeAligned_ = true;
    applyNativeDecision(nativePlayback_.attach());
}

void VideoSurface::cancelNativeProbeAlignment(const QString& reasonKey, const QString& reason) {
    if (nativeProbeAlignRequest_.valid() && mpv_ != nullptr) {
        mpv_->cancelAsync(nativeProbeAlignRequest_);
    }
    nativeProbeAlignRequest_ = AsyncRequestId{};
    nativeProbeAlign_.reset();
    if (!reasonKey.isEmpty()) emit nativeProcessingFailure(reasonKey, reason);
}

void VideoSurface::requestNativeAdmission(const native::PreparedAdmissionRequest& request) {
    const QString unavailable = privateNativeRuntimeUnavailableReason();
    if (!unavailable.isEmpty()) {
        emit nativeProcessingFailure(QStringLiteral("native.runtimeUnavailable"), unavailable);
        return;
    }
    syncNativeSource();
    // The plan names a backend, and the frames it will run on come from whichever
    // decoder this core is using. Without a confirmed alignment there is no
    // evidence the two match, so the admission is refused by name instead of
    // being admitted onto a decoder the plan was never measured for.
    if (nativePlayback_.state() == native::NativePlaybackController::State::Attached &&
        !nativeProbeAligned_) {
        applyNativeDecision(nativePlayback_.requestOff(nativePlaybackNowMs()));
        emit nativeProcessingFailure(
            QStringLiteral("native.admission.decoderUnaligned"),
            tr("The core's decoder was never confirmed for this provider, so the plan cannot be "
               "admitted onto it."));
        return;
    }
    const native::NativePlaybackController::State state = nativePlayback_.state();
    if (state == native::NativePlaybackController::State::Idle ||
        state == native::NativePlaybackController::State::Off ||
        state == native::NativePlaybackController::State::Failed) {
        // The admission's own prerequisite. Attaching is the only native binding
        // this surface issues on its own, and it is reached only from here.
        armNativeObservation();
        const native::NativePlaybackController::Decision attached = nativePlayback_.attach();
        applyNativeDecision(attached);
        if (attached.outcome == native::NativePlaybackController::Outcome::Refused) return;
    }
    applyNativeDecision(nativePlayback_.requestAdmission(request, nativePlaybackNowMs()));
}

void VideoSurface::turnNativeOff() {
    syncNativeSource();
    armNativeObservation();
    applyNativeDecision(nativePlayback_.requestOff(nativePlaybackNowMs()));
}

void VideoSurface::suspendNativeProcessing() {
    syncNativeSource();
    armNativeObservation();
    applyNativeDecision(nativePlayback_.suspend(nativePlaybackNowMs()));
}

void VideoSurface::resumeNativeProcessing() {
    syncNativeSource();
    armNativeObservation();
    applyNativeDecision(nativePlayback_.resume(nativePlaybackNowMs()));
}

void VideoSurface::drainNativeProcessing() {
    syncNativeSource();
    armNativeObservation();
    applyNativeDecision(nativePlayback_.drain(nativePlaybackNowMs()));
}

bool VideoSurface::settleNativeForTeardown(int timeoutMs) {
    if (mpv_ == nullptr) return true;
    if (nativeTeardownSettling_) {
        // A reentrant settle (the wait below spins the event loop) must not start
        // a second stop or touch a surface that a close is already tearing down.
        return false;
    }
    nativeTeardownSettling_ = true;
    // The frozen capture is over: a plan lock would only queue the stop below and
    // time it out.
    nativePlayback_.abandonPlanLock();
    nativePlayback_.beginTeardown();
    nativeStatusPolling_ = true;
    armNativeObservation();
    // The wait spins the event loop, so the surface can be destroyed under it.
    QPointer<VideoSurface> guard(this);
    QElapsedTimer clock;
    clock.start();
    const auto settlePending = [&]() {
        while (!guard.isNull() && nativePlayback_.hasPendingTransition() &&
               clock.elapsed() < timeoutMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
    };
    const auto nativeWorkHeld = [this]() {
        return nativePlayback_.holdsInferenceLease() || nativePlayback_.hasPendingTransition() ||
               nativePlayback_.status().confirmed;
    };
    const auto settled = [&]() {
        // Only a released claim set is proof: a watchdog-cleared pending or a
        // retained claim set is not an acknowledgement.
        return !nativePlayback_.holdsInferenceLease() && !nativePlayback_.hasPendingTransition();
    };

    // An in-flight transition has to land first: the filter refuses control while
    // its own admit job runs. Nothing is cancelled here.
    settlePending();
    if (guard.isNull()) return false;
    bool stopped = !nativeWorkHeld();
    if (!stopped && nativePlayback_.status().confirmed) {
        // An acknowledged plan is really suspended; the Suspended observation is
        // what releases the device and artifact claims.
        applyNativeDecision(nativePlayback_.suspend(nativePlaybackNowMs()));
        settlePending();
        if (guard.isNull()) return false;
        stopped = nativePlayback_.state() == native::NativePlaybackController::State::Suspended &&
                  settled();
    } else if (!stopped) {
        // Anything else may still hold a filter or a plan nobody acknowledged
        // stopping, so a real removal is asked for and awaited. Its absence is
        // never inferred from local state.
        applyNativeDecision(nativePlayback_.requestOff(nativePlaybackNowMs()));
        settlePending();
        if (guard.isNull()) return false;
        stopped = nativePlayback_.state() == native::NativePlaybackController::State::Off &&
                  settled();
    }
    nativeTeardownSettling_ = false;
    return stopped;
}

// ── native plan lock ──────────────────────────────────────────────────────

VideoSurface::NativePlanLock::NativePlanLock(NativePlanLock&& other) noexcept
    : surface_(other.surface_) {
    other.surface_.clear();
}

VideoSurface::NativePlanLock& VideoSurface::NativePlanLock::operator=(
    NativePlanLock&& other) noexcept {
    if (this == &other) return *this;
    release();
    surface_ = other.surface_;
    other.surface_.clear();
    return *this;
}

VideoSurface::NativePlanLock::~NativePlanLock() { release(); }

void VideoSurface::NativePlanLock::release() {
    if (surface_.isNull()) return;
    VideoSurface* surface = surface_.data();
    surface_.clear();
    surface->releaseNativePlanLock();
}

std::optional<VideoSurface::NativePlanLock> VideoSurface::acquireNativePlanLock(
    QString* reasonKey, QString* reason) {
    const auto refuse = [reasonKey, reason](const QString& key, const QString& text) {
        if (reasonKey != nullptr) *reasonKey = key;
        if (reason != nullptr) *reason = text;
        return std::optional<NativePlanLock>();
    };
    if (mpv_ == nullptr || !nativeSourceActive_) {
        return refuse(QStringLiteral("native.playback.noSource"),
                      QStringLiteral("no native source epoch is open to freeze"));
    }
    // The lock is taken against the epoch the capture will actually show.
    syncNativeSource();
    if (!nativePlayback_.lockPlan()) {
        if (!nativePlayback_.status().pendingRequestId.isEmpty()) {
            return refuse(QStringLiteral("native.playback.lockBusy"),
                          QStringLiteral("a native transition is still in flight; the plan "
                                         "cannot be frozen mid-swap"));
        }
        return refuse(QStringLiteral("native.playback.noSource"),
                      QStringLiteral("no native source epoch is open to freeze"));
    }
    return std::optional<NativePlanLock>(NativePlanLock(this));
}

void VideoSurface::releaseNativePlanLock() {
    const bool wasLocked = nativePlayback_.planLocked();
    const native::NativePlaybackController::Decision decision =
        nativePlayback_.unlockPlan(nativePlaybackNowMs());
    if (!wasLocked) return;
    // The queued transition, if any, was just submitted, so the reader has to
    // keep observing for its acknowledgement to arrive; the status change it
    // publishes is the consumer's unlock notification.
    armNativeObservation();
    applyNativeDecision(decision);
}

void VideoSurface::deliverNativeCommand(const native::NativePlaybackCommand& command) {
    if (command.kind == native::NativePlaybackCommandKind::Attach ||
        command.kind == native::NativePlaybackCommandKind::Admit) {
        const QString unavailable = privateNativeRuntimeUnavailableReason();
        if (!unavailable.isEmpty()) {
            emit nativeProcessingFailure(QStringLiteral("native.runtimeUnavailable"), unavailable);
            return;
        }
    }
    if (mpv_ == nullptr) return;
    if (command.kind == native::NativePlaybackCommandKind::Attach &&
        (animeJanaiFilterBound_ || animeJanaiBusy_)) {
        emit nativeProcessingFailure(QStringLiteral("native.playback.animeJanaiConflict"),
                                     QStringLiteral("AnimeJaNai processing is present or transitioning; "
                                                    "private mikai_native cannot be attached concurrently"));
        return;
    }
    // The controller models attach as a state transition; the core's filter chain
    // survives a source change, and mpv's `vf add` replaces an entry with the same
    // label - which would reset the filter's observed state. So a label already in
    // the chain is not added again; the controller has moved to Attached anyway.
    if (command.kind == native::NativePlaybackCommandKind::Attach && nativeFilterBound_) {
        // Nothing goes on the wire, but the READER still has to start watching:
        // the poll this transition is really about was armed by the caller long
        // before the attachment, so a `vf-metadata` refusal read in between
        // latched a halt that nothing after the attachment would clear.
        armNativeObservation();
        return;
    }
    // The exact wire: attach and remove name the filter directly; admit and the
    // packed control document travel as the one free argument of vf-command.
    // mpv applies the escaping, so the spec bytes are handed over untouched.
    const auto bytes = [](const QByteArray& spec) {
        return std::string(spec.constData(), static_cast<size_t>(spec.size()));
    };
    const native::NativePlaybackCommandKind kind = command.kind;
    std::vector<std::string> args;
    switch (command.kind) {
        case native::NativePlaybackCommandKind::Attach:
            args = {"vf", "add", bytes(command.spec)};
            break;
        case native::NativePlaybackCommandKind::Off:
            args = {"vf", "remove", bytes(command.spec)};
            break;
        case native::NativePlaybackCommandKind::Admit:
            args = {"vf-command", native::kNativeStatusLabel, "admit", bytes(command.spec)};
            break;
        case native::NativePlaybackCommandKind::Drain:
        case native::NativePlaybackCommandKind::Suspend:
        case native::NativePlaybackCommandKind::Resume:
            args = {"vf-command", native::kNativeStatusLabel, "control", bytes(command.spec)};
            break;
    }
    // What the chain is about to hold, so a reply the core drops cannot leave the
    // flag stuck; the transport answers below and a refused add clears it.
    if (kind == native::NativePlaybackCommandKind::Attach) nativeFilterBound_ = true;
    const QString requestId = command.requestId;
    // The reply is only the transport's word: it never activates anything. The
    // controller turns an accepted transport into progress and a refusal into a
    // named decision.
    mpv_->commandAsync(std::move(args), [this, requestId, kind](const AsyncReply& reply) {
        const bool accepted = reply.ok();
        // A refused add means the filter is not in the chain. A refused remove
        // is not evidence of absence, so the bound bit stays set and the
        // AnimeJaNai mutual-exclusion guard remains closed.
        if (!accepted && kind == native::NativePlaybackCommandKind::Attach) nativeFilterBound_ = false;
        // A successful `vf remove` transport reply is not the observed absence
        // required by the controller's teardown contract; keep the bound bit
        // set until the status observation returns Outcome::Removed.
        // The filter is in the chain NOW, so this is the one point where the
        // absence a poll may be reading is about to stop being true. Re-arming
        // here is what separates status-before-attachment noise - the refusal a
        // halt already recorded, which repeats identically - from an attachment
        // that really failed: a refused add never reaches this branch, so its
        // absence keeps halting.
        if (accepted && kind == native::NativePlaybackCommandKind::Attach) armNativeObservation();
        const QString reason =
            (accepted || mpv_ == nullptr) ? QString()
                                          : QString::fromStdString(mpv_->describeError(reply.error));
        const native::NativePlaybackController::Decision decision =
            nativePlayback_.onCommandCompleted(
                requestId, accepted,
                accepted ? QString() : QStringLiteral("native.playback.transportRefused"), reason);
        if (!accepted && kind == native::NativePlaybackCommandKind::Attach &&
            decision.outcome == native::NativePlaybackController::Outcome::Ignored) {
            // The controller keeps no transition for an attach, so a refused one
            // is named here rather than swallowed.
            emit nativeProcessingFailure(QStringLiteral("native.playback.attachRefused"), reason);
            return;
        }
        applyNativeDecision(decision);
    });
}

void VideoSurface::applyNativeDecision(const native::NativePlaybackController::Decision& decision) {
    if (decision.outcome == native::NativePlaybackController::Outcome::Removed) {
        nativeFilterBound_ = false;
    }
    if (decision.outcome == native::NativePlaybackController::Outcome::Refused ||
        decision.outcome == native::NativePlaybackController::Outcome::TimedOut) {
        emit nativeProcessingFailure(decision.reasonKey, decision.reason);
    }
    publishNativePlaybackStatus();
}

void VideoSurface::publishNativePlaybackStatus() {
    const native::NativePlaybackController::Status& status = nativePlayback_.status();
    if (nativeStatusPublished_ && nativeStatusEquals(status, lastNativeStatus_)) return;
    lastNativeStatus_ = status;
    nativeStatusPublished_ = true;
    emit nativePlaybackStatusChanged(status);
}

std::optional<native::NativePreparationContext> VideoSurface::nativePreparationContext() const {
    if (mpv_ == nullptr) return std::nullopt;
    const std::optional<native::StatusSnapshot>& snapshot = nativeStatus_.snapshot();
    if (!snapshot.has_value() || !snapshot->probe.has_value()) return std::nullopt;
    const native::NativeProbePayload& probe = *snapshot->probe;
    // Both facts must be the producer's own complete observation: a partial
    // report is not a context, and a caller that guessed the other half would be
    // planning from a device it never saw.
    if (!probe.source.has_value() || !probe.source->isKnown()) return std::nullopt;
    if (!probe.device.has_value() || !probe.device->isKnown()) return std::nullopt;
    const QString modulePath = currentMpvModulePath();
    if (modulePath.isEmpty()) return std::nullopt;
    native::NativePreparationContext context;
    context.source = *probe.source;
    context.device = *probe.device;
    context.modulePath = modulePath;
    // The same accepted document carries the factory observation, so it belongs
    // to this very source: it rode in with the probe just read. Absent stays
    // absent rather than becoming a second query - the caller decides what an
    // unobserved factory means.
    context.factory = snapshot->factory;
    // The decoder the core was aligned to for THIS source, when an explicit
    // alignment completed for it. Empty rather than derived: a caller must be
    // able to tell "no decoder was ever confirmed" from a device fact that only
    // exists because a factory was queried.
    context.decoder = nativeProbeDecoder_;
    context.desiredBackend = nativeProbeBackend_;
    return context;
}

// ── reference capture ─────────────────────────────────────────────────────

namespace {

/// The observed source cadence, taken from the filter's own probe report. This
/// is a measured rational the producer published, never a default frame rate: a
/// source whose cadence was never observed has no honest interval, and the
/// caller is told that instead of being handed one.
std::optional<native::Rational> observedSourceCadence(const VideoSurface& surface) {
    const auto& view = surface.nativeStatus();
    if (!view.snapshot.has_value() || !view.snapshot->probe.has_value()) return std::nullopt;
    const std::optional<native::SourceGeometry>& source = view.snapshot->probe->source;
    if (!source.has_value() || !source->fps.has_value()) return std::nullopt;
    if (!source->fps->isValid()) return std::nullopt;
    return source->fps;
}

/// Half a source frame at `cadence`: the closest this build can say that two
/// positions are the same frame without claiming an exact seek landed. mpv
/// decodes from container timestamps, so `time-pos` lands on a frame boundary
/// rather than on the exact value that was written, and demanding equality
/// would refuse every restore of a source whose timestamps are not frame
/// aligned. Derived from the OBSERVED rate, never a constant; a degenerate rate
/// refuses rather than widening the window into one that accepts anything.
double restoreToleranceFor(const std::optional<native::Rational>& cadence) {
    if (!cadence.has_value() || !cadence->isValid()) return 0.0;
    const double frame = static_cast<double>(cadence->den) / static_cast<double>(cadence->num);
    if (!(frame > 0.0) || frame > 1.0) return 0.0;
    return frame * 0.5;
}

/// True when `observed` is at the position the restore asked for, within the
/// half-frame bound. Anything else is "not there yet": the seek is still being
/// processed, the source moved, or it was refused - and each of those is
/// answered by waiting or by naming the failure, never by reporting success.
bool restoreHasSettled(double observed, double target,
                       const std::optional<native::Rational>& cadence) {
    if (!std::isfinite(observed) || !std::isfinite(target)) return false;
    const double tolerance = restoreToleranceFor(cadence);
    if (tolerance <= 0.0) return false;
    return std::abs(observed - target) <= tolerance;
}

}  // namespace

void VideoSurface::requestNativeReferenceFrame(int frameIndex) {
    if (frameIndex < 0) {
        emit nativeReferenceFrameFailed(QStringLiteral("native.reference.frameIndex"),
                                        tr("A reference capture cannot be asked for frame %1.")
                                            .arg(frameIndex),
                                        frameIndex);
        return;
    }
    // The settle is part of the capture, not an idle gap between two of them:
    // the capture request is already answered and the step flag is already down
    // while the held frame waits for the core's own clock, so neither flag alone
    // covers it. A position still being OWED is what spans that whole window, and
    // it is cleared by every exit - published, refused or abandoned - so this
    // refuses a second capture for exactly as long as one is still owed.
    if (nativeReferenceRequest_.valid() || nativeReferenceStepping_ ||
        nativeProbeResumePosition_.has_value()) {
        emit nativeReferenceFrameFailed(QStringLiteral("native.reference.busy"),
                                        tr("A reference capture is already in flight."),
                                        frameIndex);
        return;
    }
    if (mpv_ == nullptr) {
        emit nativeReferenceFrameFailed(QStringLiteral("native.reference.noCore"),
                                        tr("No mpv core exists to capture from."), frameIndex);
        return;
    }
    if (nativePlayback_.holdsInferenceLease()) {
        // A native plan is admitted, so what the chain holds is no longer simply
        // the decoded source; the capture would not be the reference a
        // preparation needs, and pretending otherwise would compare a native
        // output against the original. This is the truthful refusal - it names
        // why, and it leaves the current plan exactly where it is.
        emit nativeReferenceFrameFailed(
            QStringLiteral("native.reference.nativeActive"),
            tr("Native processing is active, so the captured frame would not be the "
               "Original source."),
            frameIndex);
        return;
    }
    // The same work lease a preparation build takes. Taking it refuses a
    // simultaneous build outright, and fails right here when one already holds
    // the lease - which is the refusal, not a retry.
    std::optional<native::NativeWorkLease> lease =
        native::NativeWorkLease::acquire(native::NativeWorkKind::Inference);
    if (!lease.has_value()) {
        emit nativeReferenceFrameFailed(
            QStringLiteral("native.reference.busy"),
            tr("A native preparation build owns the work lease."), frameIndex);
        return;
    }
    const uint64_t epoch = mpv_->sourceEpoch();
    nativeReferenceLease_ = std::move(lease);
    nativeReferenceFrameIndex_ = frameIndex;

    // The first observation is the frame playback is parked at. Nothing moves:
    // no seek, no unpause, no decode of anything else.
    if (frameIndex == 0) {
        readNativeReferencePosition(epoch, frameIndex);
        return;
    }
    // The adjacent frame needs a measured interval. Without one there is no
    // honest way to name the next frame, and repeating frame 0 would hand the
    // caller a pair that interpolates a frame against itself.
    const std::optional<native::Rational> cadence = observedSourceCadence(*this);
    if (!cadence.has_value()) {
        discardNativeReferenceStep(QStringLiteral("native.reference.cadenceUnknown"),
                                   tr("The source's frame rate was never observed, so the "
                                      "next frame cannot be named without guessing."));
        return;
    }
    // Only a seekable source can go back to where it was, so a neighbour that
    // cannot be undone is refused rather than taken.
    nativeProbeAlignRequest_ = mpv_->getPropertyAsync(
        "seekable", MPV_FORMAT_FLAG, [this, epoch, frameIndex, cadence](const AsyncReply& reply) {
            nativeProbeAlignRequest_ = AsyncRequestId{};
            if (!reply.ok() || !reply.flag) {
                discardNativeReferenceStep(
                    QStringLiteral("native.reference.notSeekable"),
                    tr("This source is not seekable, so the position it is parked at cannot be "
                       "restored after a neighbouring frame is taken."));
                return;
            }
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch()) {
                discardNativeReferenceStep(
                    QStringLiteral("native.reference.staleSource"),
                    tr("The source changed while the capture was being prepared."));
                return;
            }
            // Read where the core actually is, so the restore target is the core's
            // own clock rather than the last position this widget observed.
            readNativeReferencePosition(epoch, frameIndex);
        });
    if (!nativeProbeAlignRequest_.valid()) {
        discardNativeReferenceStep(QStringLiteral("native.reference.request"),
                                   tr("The core would not take the seekability read."));
    }
}

void VideoSurface::readNativeReferencePosition(uint64_t epoch, int frameIndex) {
    if (mpv_ == nullptr) return;
    nativeProbeAlignRequest_ = mpv_->getPropertyAsync(
        "time-pos", MPV_FORMAT_DOUBLE, [this, epoch, frameIndex](const AsyncReply& reply) {
            nativeProbeAlignRequest_ = AsyncRequestId{};
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch()) {
                discardNativeReferenceStep(QStringLiteral("native.reference.staleSource"),
                                           tr("The source changed while the capture was being "
                                              "prepared."));
                return;
            }
            // A source with no position yet has no frame to capture, and its
            // position is not zero.
            if (!reply.ok() || !std::isfinite(reply.number) || reply.number < 0.0) {
                discardNativeReferenceStep(QStringLiteral("native.reference.positionUnknown"),
                                           tr("The core has no known playback position for this "
                                              "source yet."));
                return;
            }
            const double position = reply.number;
            if (frameIndex == 0) {
                issueNativeReferenceCapture(epoch, frameIndex, position);
                return;
            }
            const std::optional<native::Rational> cadence = observedSourceCadence(*this);
            if (!cadence.has_value()) {
                discardNativeReferenceStep(QStringLiteral("native.reference.cadenceUnknown"),
                                           tr("The source's frame rate was never observed, so the "
                                              "next frame cannot be named without guessing."));
                return;
            }
            // One measured interval on from where the core is. The cadence is the
            // producer's observed rational, so this is the real frame boundary
            // rather than a nominal frame time.
            const double interval =
                static_cast<double>(cadence->den) / static_cast<double>(cadence->num);
            stepToAdjacentNativeFrame(position, position + interval, frameIndex);
        });
    if (!nativeProbeAlignRequest_.valid()) {
        discardNativeReferenceStep(QStringLiteral("native.reference.request"),
                                   tr("The core would not take the position read."));
    }
}

void VideoSurface::stepToAdjacentNativeFrame(double restore, double adjacent,
                                            int frameIndex) {
    if (mpv_ == nullptr) return;
    const uint64_t epoch = mpv_->sourceEpoch();
    nativeReferenceStepping_ = true;
    // What the restore must put back. Recorded before the step so a source change
    // part-way through still knows the position that is owed.
    nativeProbeResumePosition_ = restore;
    mpv_->commandAsync(
        {"seek", std::to_string(adjacent), "absolute+exact"},
        [this, epoch, frameIndex, adjacent](const AsyncReply& reply) {
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch()) {
                discardNativeReferenceStep(QStringLiteral("native.reference.staleSource"),
                                           tr("The source changed while the neighbouring frame "
                                              "was being taken."));
                return;
            }
            if (!reply.ok()) {
                // Nothing was moved, so nothing is owed back. The lease goes with
                // it, exactly as it does on every other exit: a named failure must
                // not leave the work lease held by a capture that is over.
                nativeReferenceStepping_ = false;
                nativeProbeResumePosition_.reset();
                nativeReferenceLease_.reset();
                QString reason;
                if (mpv_ != nullptr) {
                    reason = QString::fromStdString(mpv_->describeError(reply.error));
                }
                emit nativeReferenceFrameFailed(
                    QStringLiteral("native.reference.seekRefused"),
                    tr("The core would not step to the neighbouring source frame (%1).")
                        .arg(reason),
                    frameIndex);
                return;
            }
            issueNativeReferenceCapture(epoch, frameIndex, adjacent);
        });
}

void VideoSurface::issueNativeReferenceCapture(uint64_t epoch, int frameIndex,
                                              double position) {
    if (mpv_ == nullptr) return;
    nativeReferenceRequest_ = mpv_->commandAsync(
        {"screenshot-raw", "video", "rgba"},
        [this, epoch, frameIndex, position](const AsyncReply& reply) {
            nativeReferenceRequest_ = AsyncRequestId{};
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch()) {
                discardNativeReferenceStep(QStringLiteral("native.reference.staleSource"),
                                           tr("The source changed while the capture was in "
                                              "flight."));
                return;
            }
            if (!reply.ok()) {
                QString reason;
                if (mpv_ != nullptr) {
                    reason = QString::fromStdString(mpv_->describeError(reply.error));
                }
                finishNativeReferenceCapture(
                    QStringLiteral("native.reference.captureFailed"),
                    tr("The core refused the capture (%1).").arg(reason), frameIndex);
                return;
            }
            native::FrameFixture frame;
            QString reasonKey;
            QString reason;
            if (!decodeRawFrame(reply.node, &frame, &reasonKey, &reason)) {
                finishNativeReferenceCapture(reasonKey, reason, frameIndex);
                return;
            }
            // A neighbour that decoded to the first frame's bytes is not a second
            // observation. Refusing it is the whole point: an interpolation between
            // one frame and itself is not a pair, and delivering it would let a
            // verification "pass" on a duplicated reference.
            const QByteArray digest = frameDigest(frame.rgb);
            if (frameIndex > 0 && digest == nativeReferenceLastHash_ &&
                nativeReferenceLastEpoch_ == epoch) {
                finishNativeReferenceCapture(
                    QStringLiteral("native.reference.duplicate"),
                    tr("The neighbouring source frame decoded to the same pixels as the previous "
                       "one, so it cannot serve as a second reference."),
                    frameIndex);
                return;
            }
            if (frameIndex > 0) {
                // The restore is issued BEFORE this frame is reported, so a caller
                // never receives a frame while the core still sits one frame ahead of
                // where the viewer left it.
                const std::optional<double> resume = nativeProbeResumePosition_;
                nativeReferenceStepping_ = false;
                if (resume.has_value() && mpv_ != nullptr) {
                    // The frame is already captured, so it is HELD here until the
                    // core's own clock confirms the restore landed. Waiting on the
                    // seek's transport reply instead is what published this frame from
                    // a position the core had not returned to yet: mpv answers that
                    // reply when the command is ACCEPTED, one drain before the
                    // playhead has actually moved.
                    awaitNativeReferenceRestore(
                        epoch, frameIndex, *resume, observedSourceCadence(*this),
                        std::make_shared<const native::FrameFixture>(std::move(frame)), position,
                        digest, nativePlaybackNowMs() + kRestoreSettleTimeoutMs);
                    return;
                }
                emitNativeReferenceCaptureReady(frame, epoch, position, frameIndex, digest);
                return;
            }
            emitNativeReferenceCaptureReady(frame, epoch, position, frameIndex, digest);
        });
    if (!nativeReferenceRequest_.valid()) {
        finishNativeReferenceCapture(QStringLiteral("native.reference.transport"),
                                     tr("The core would not take the capture request."),
                                     frameIndex);
    }
}

/// Seeks back and CONFIRMS the seek on the core's own clock, re-issuing it only
/// while the core is still somewhere else. A capture is published only from
/// here, so `nativeReferenceFrameReady` can no longer name a position the viewer
/// has already left.
void VideoSurface::awaitNativeReferenceRestore(uint64_t epoch, int frameIndex, double restore,
                                              std::optional<native::Rational> cadence,
                                              std::shared_ptr<const native::FrameFixture> frame,
                                              std::optional<double> decodedPosition,
                                              std::optional<QByteArray> digest,
                                              qint64 deadlineMs) {
    if (mpv_ == nullptr || epoch != mpv_->sourceEpoch()) {
        refuseNativeReferenceRestore(QStringLiteral("native.reference.staleSource"),
                                     tr("The source changed while the position was being "
                                        "restored."),
                                     frameIndex, epoch);
        return;
    }
    // The id is not decoration: a seek that could not be ISSUED runs no callback,
    // and without this the held frame would sit with a position owed and a lease
    // taken, refusing every later capture with no named reason ever published.
    const AsyncRequestId seek =
        mpv_->commandAsync(
            {"seek", std::to_string(restore), "absolute+exact"},
            [this, epoch, frameIndex, restore, cadence, frame, decodedPosition, digest,
             deadlineMs](const AsyncReply& seekReply) {
                if (mpv_ == nullptr || epoch != mpv_->sourceEpoch()) {
                    refuseNativeReferenceRestore(QStringLiteral("native.reference.staleSource"),
                                                 tr("The source changed while the position was "
                                                    "being restored."),
                                                 frameIndex, epoch);
                    return;
                }
                if (!seekReply.ok()) {
                    QString reason;
                    if (mpv_ != nullptr) {
                        reason = QString::fromStdString(mpv_->describeError(seekReply.error));
                    }
                    refuseNativeReferenceRestore(
                        QStringLiteral("native.reference.restoreRefused"),
                        tr("The core would not restore the parked position (%1).").arg(reason),
                        frameIndex, epoch);
                    return;
                }
                // The transport accepted the seek; that is NOT the completion. The
                // core's own clock is what says the playhead is back.
                pollNativeReferenceRestore(epoch, frameIndex, restore, cadence, frame,
                                           decodedPosition, digest, deadlineMs, 0);
            });
    if (!seek.valid()) {
        refuseNativeReferenceRestore(QStringLiteral("native.reference.restoreRequest"),
                                     tr("The core would not take the seek that restores the "
                                        "parked position."),
                                     frameIndex, epoch);
    }
}

void VideoSurface::pollNativeReferenceRestore(uint64_t epoch, int frameIndex, double restore,
                                             std::optional<native::Rational> cadence,
                                             std::shared_ptr<const native::FrameFixture> frame,
                                             std::optional<double> decodedPosition,
                                             std::optional<QByteArray> digest, qint64 deadlineMs,
                                             int attempts) {
    if (mpv_ == nullptr || epoch != mpv_->sourceEpoch()) {
        refuseNativeReferenceRestore(QStringLiteral("native.reference.staleSource"),
                                     tr("The source changed while the position was being "
                                        "restored."),
                                     frameIndex, epoch);
        return;
    }
    // Without an observed cadence there is no half-frame bound, so NO read can
    // ever settle - and a loop that polls to the attempt bound would end by
    // claiming the core sits somewhere it does not. That is a fact about what
    // this surface can measure, not about the core, so it is named as such and
    // answered here instead of by burning the bound on reads that prove nothing.
    if (restoreToleranceFor(cadence) <= 0.0) {
        refuseNativeReferenceRestore(
            QStringLiteral("native.reference.restoreToleranceUnknown"),
            tr("The source's observed frame rate gives no bound for confirming the restore "
               "landed to within half a frame, so the neighbouring frame is not reported from an "
               "unconfirmed position."),
            frameIndex, epoch);
        return;
    }
    if (nativePlaybackNowMs() > deadlineMs) {
        // The restore is owed and unconfirmed, so nothing is published: a frame
        // whose position this surface cannot vouch for is worse than a named
        // refusal. The lease goes with it, so a build is not blocked forever.
        refuseNativeReferenceRestore(
            QStringLiteral("native.reference.restoreUnsettled"),
            tr("The core did not return to the parked position (%1s) within the bound, so the "
               "neighbouring frame is not reported from a position that was not restored.")
                .arg(restore, 0, 'f', 6),
            frameIndex, epoch);
        return;
    }
    nativeReferenceRestoreRequest_ = mpv_->getPropertyAsync(
        "time-pos", MPV_FORMAT_DOUBLE,
        [this, epoch, frameIndex, restore, cadence, frame, decodedPosition, digest, deadlineMs,
         attempts](const AsyncReply& reply) {
            nativeReferenceRestoreRequest_ = AsyncRequestId{};
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch()) {
                refuseNativeReferenceRestore(
                    QStringLiteral("native.reference.staleSource"),
                    tr("The source changed while the position was being restored."), frameIndex,
                    epoch);
                return;
            }
            // A position the core will not report cannot be judged as restored,
            // and neither can a non-finite one: both are refusals, not "close
            // enough".
            if (!reply.ok() || !std::isfinite(reply.number)) {
                refuseNativeReferenceRestore(
                    QStringLiteral("native.reference.restorePositionUnknown"),
                    tr("The core reported no usable position for the restore, so the parked "
                       "position cannot be confirmed."),
                    frameIndex, epoch);
                return;
            }
            if (restoreHasSettled(reply.number, restore, cadence)) {
                // The owed position is discharged by the observation itself, so it
                // is not left behind as a phantom debt.
                nativeProbeResumePosition_.reset();
                // Before the report, not after: the frame is announced from inside
                // emit, so a handler that reads position() the moment it receives
                // the frame must already see where the core really is.
                announceRestoredPosition(reply.number);
                emitNativeReferenceCaptureReady(*frame, epoch,
                                                decodedPosition.value_or(restore), frameIndex,
                                                digest.value_or(QByteArray()));
                return;
            }
            // Still elsewhere. A bounded retry, so a slow seek lands without the
            // caller waiting on an unbounded loop; the deadline above is what ends
            // it, by name.
            if (attempts >= kRestoreSettleMaxAttempts) {
                refuseNativeReferenceRestore(
                    QStringLiteral("native.reference.restoreUnsettled"),
                    tr("The core is at %1s rather than the parked %2s after %3 restore attempts, "
                       "so the neighbouring frame is not reported from a position that was not "
                       "restored.")
                        .arg(reply.number, 0, 'f', 6)
                        .arg(restore, 0, 'f', 6)
                        .arg(attempts + 1),
                    frameIndex, epoch);
                return;
            }
            const AsyncRequestId reseek =
                mpv_->commandAsync(
                    {"seek", std::to_string(restore), "absolute+exact"},
                    [this, epoch, frameIndex, restore, cadence, frame, decodedPosition, digest,
                     deadlineMs, attempts](const AsyncReply& retry) {
                        if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() || !retry.ok()) {
                            refuseNativeReferenceRestore(
                                retry.ok() ? QStringLiteral("native.reference.staleSource")
                                           : QStringLiteral("native.reference.restoreRefused"),
                                tr("The core would not restore the parked position."), frameIndex,
                                epoch);
                            return;
                        }
                        pollNativeReferenceRestore(epoch, frameIndex, restore, cadence, frame,
                                                   decodedPosition, digest, deadlineMs,
                                                   attempts + 1);
                    });
            // Same rule as the first seek: a retry that could not be issued runs
            // no callback, so it would end the capture with no answer at all.
            if (!reseek.valid()) {
                refuseNativeReferenceRestore(QStringLiteral("native.reference.restoreRequest"),
                                             tr("The core would not take the repeated seek that "
                                                "restores the parked position."),
                                             frameIndex, epoch);
            }
        });
    if (!nativeReferenceRestoreRequest_.valid()) {
        refuseNativeReferenceRestore(QStringLiteral("native.reference.restoreRequest"),
                                     tr("The core would not take the position read that confirms "
                                        "the restore."),
                                     frameIndex, epoch);
    }
}

void VideoSurface::announceRestoredPosition(double confirmed) {
    // The reading settled the restore, so it is finite by construction - but the
    // cache is not written from anything it has not checked, and a NaN reaching
    // position() would make every later comparison meaningless.
    if (!std::isfinite(confirmed)) return;
    // Both caches move together, and that is the whole point: `position_` is what
    // a caller reads, `observedPosition_` is what the next pump would publish,
    // and leaving the observer behind would make the very next tick emit a
    // positionChanged carrying the core's actual (adjacent) frame and undo the
    // truth this call just established. The announcement is emitted whenever the
    // published value moved by more than the pump's own 0.01 s noise floor, so
    // the signal means exactly what it means everywhere else in this class.
    if (std::abs(confirmed - position_) <= 0.01) return;
    position_ = confirmed;
    observedPosition_ = confirmed;
    emit positionChanged(position_);
}

void VideoSurface::refuseNativeReferenceRestore(const QString& reasonKey, const QString& reason,
                                                int frameIndex, uint64_t epoch) {
    Q_UNUSED(epoch);
    if (nativeReferenceRestoreRequest_.valid() && mpv_ != nullptr) {
        mpv_->cancelAsync(nativeReferenceRestoreRequest_);
        nativeReferenceRestoreRequest_ = AsyncRequestId{};
    }
    nativeReferenceStepping_ = false;
    nativeProbeResumePosition_.reset();
    // The lease is released with the refusal, so a named failure never leaves a
    // preparation build blocked behind a capture that will not be answered.
    nativeReferenceLease_.reset();
    emit nativeReferenceFrameFailed(reasonKey, reason, frameIndex);
}

void VideoSurface::emitNativeReferenceCaptureReady(const native::FrameFixture& frame,
                                                   uint64_t epoch, double position,
                                                   int frameIndex, const QByteArray& digest) {
    nativeReferenceLastHash_ = digest;
    nativeReferenceLastEpoch_ = epoch;
    // The capture is over, so the shared lease goes first: a handler that asks for
    // another frame - or starts a build - then owns its own claim, and this
    // callback cannot release it underneath.
    nativeReferenceLease_.reset();
    nativeReferenceStepping_ = false;
    nativeProbeResumePosition_.reset();
    emit nativeReferenceFrameReady(frame, epoch, position, frameIndex,
                                   observedSourceCadence(*this));
}

/// Ends a capture whose result cannot be used, restoring the position this
/// surface moved the core to before anything is reported.
void VideoSurface::finishNativeReferenceCapture(const QString& reasonKey,
                                                const QString& reason, int frameIndex) {
    const std::optional<double> resume = nativeProbeResumePosition_;
    nativeReferenceStepping_ = false;
    nativeProbeResumePosition_.reset();
    nativeReferenceLease_.reset();
    if (resume.has_value() && mpv_ != nullptr) {
        const uint64_t epoch = mpv_->sourceEpoch();
        // The restore is issued before the refusal is published, so the core is
        // never left one frame ahead of where the viewer left it.
        mpv_->commandAsync({"seek", std::to_string(*resume), "absolute+exact"},
                           [this, epoch, reasonKey, reason, frameIndex](const AsyncReply&) {
                               if (mpv_ != nullptr && epoch != mpv_->sourceEpoch()) return;
                               emit nativeReferenceFrameFailed(reasonKey, reason, frameIndex);
                           });
        return;
    }
    emit nativeReferenceFrameFailed(reasonKey, reason, frameIndex);
}

void VideoSurface::abandonNativeReferenceFrame(const QString& reasonKey, const QString& reason) {
    // Only a capture actually in flight can be abandoned; anything else is not this
    // call's to answer. A read that is part of preparing one is retired with it, so
    // a position or seekability answer for a source that just went away cannot go on
    // to seek or capture anything.
    //
    // The settle window counts as in flight. The capture command is already
    // answered and the step flag is already down while a held frame waits for the
    // core's clock, so the outstanding position IS the only thing that spans it -
    // and the retire has to reach the settle read too, or a `time-pos` answer for
    // the source being replaced would still arrive to publish a frame and a
    // position that describe that source, at its frame boundary, from here.
    const bool settling = nativeReferenceRestoreRequest_.valid() ||
                          nativeProbeResumePosition_.has_value();
    if (!nativeReferenceRequest_.valid() && !nativeReferenceStepping_ && !settling) return;
    if (mpv_ != nullptr) {
        if (nativeReferenceRequest_.valid()) mpv_->cancelAsync(nativeReferenceRequest_);
        if (nativeProbeAlignRequest_.valid()) mpv_->cancelAsync(nativeProbeAlignRequest_);
        if (nativeReferenceRestoreRequest_.valid())
            mpv_->cancelAsync(nativeReferenceRestoreRequest_);
    }
    nativeReferenceRequest_ = AsyncRequestId{};
    nativeProbeAlignRequest_ = AsyncRequestId{};
    nativeReferenceRestoreRequest_ = AsyncRequestId{};
    // The restore is deliberately NOT issued: the source this surface moved is gone,
    // and seeking whatever replaced it to the old source's position would be worse
    // than leaving it where it is.
    nativeReferenceStepping_ = false;
    nativeProbeResumePosition_.reset();
    nativeReferenceLease_.reset();
    emit nativeReferenceFrameFailed(reasonKey, reason, nativeReferenceFrameIndex_);
}

void VideoSurface::discardNativeReferenceStep(const QString& reasonKey, const QString& reason) {
    const std::optional<double> resume = nativeProbeResumePosition_;
    nativeReferenceStepping_ = false;
    nativeProbeResumePosition_.reset();
    nativeReferenceLease_.reset();
    if (resume.has_value() && mpv_ != nullptr) {
        const uint64_t epoch = mpv_->sourceEpoch();
        mpv_->commandAsync({"seek", std::to_string(*resume), "absolute+exact"},
                           [this, epoch, reasonKey, reason](const AsyncReply&) {
                               if (mpv_ != nullptr && epoch != mpv_->sourceEpoch()) return;
                               emit nativeReferenceFrameFailed(reasonKey, reason,
                                                               nativeReferenceFrameIndex_);
                           });
        return;
    }
    emit nativeReferenceFrameFailed(reasonKey, reason, nativeReferenceFrameIndex_);
}

// ── raw playback diagnostics ─────────────────────────────────────────────

PlaybackDiagnostics VideoSurface::playbackDiagnostics() const {
    return diagnostics_;
}

void VideoSurface::resetDiagnostics() {
    for (DiagnosticRequest& request : diagnosticRequests_) {
        if (request.pending.valid() && mpv_) mpv_->cancelAsync(request.pending);
        request = DiagnosticRequest{};
    }
    // Unknown, not zero: the previous source's numbers say nothing about this one.
    diagnostics_ = PlaybackDiagnostics{};
    diagnosticsLastPollMs_ = diagnosticsClock_.elapsed();
}

void VideoSurface::pollDiagnostics() {
    if (!mpv_ || !diagnosticsActive_) return;
    const qint64 nowMs = diagnosticsClock_.elapsed();
    if (nowMs - diagnosticsLastPollMs_ < kDiagnosticsPollIntervalMs) return;
    diagnosticsLastPollMs_ = nowMs;
    // Bumped by load/stop: a reply from an old epoch is dropped, not published.
    const uint64_t epoch = mpv_->sourceEpoch();
    issueDiagnostic(kDecoderDropSlot, kDecoderDropProperty, epoch);
    issueDiagnostic(kVoDropSlot, kVoDropProperty, epoch);
    issueDiagnostic(kVfSlot, kVfProperty, epoch);
}

void VideoSurface::issueDiagnostic(DiagnosticSlot slot, const char* property, uint64_t epoch) {
    DiagnosticRequest& request = diagnosticRequests_[slot];
    const qint64 nowMs = diagnosticsClock_.elapsed();
    if (request.pending.valid()) {
        // An answer that never came: abandon it so the property is not stuck
        // unread, and stop presenting the value it was meant to refresh.
        if (nowMs - request.lastRequestMs < kDiagnosticsRequestTimeoutMs) return;
        mpv_->cancelAsync(request.pending);
        request.pending = AsyncRequestId{};
        setDiagnosticUnknown(slot);
    }
    request.lastRequestMs = nowMs;
    request.pending = mpv_->getPropertyAsync(
        property, MPV_FORMAT_NODE, [this, slot, epoch](const AsyncReply& reply) {
            DiagnosticRequest& slotRequest = diagnosticRequests_[slot];
            if (!slotRequest.pending.valid()) return;
            slotRequest.pending = AsyncRequestId{};
            // A reply for a superseded source says nothing about the current one.
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch()) {
                setDiagnosticUnknown(slot);
                return;
            }
            applyDiagnosticReply(slot, reply);
        });
    if (!request.pending.valid()) setDiagnosticUnknown(slot);
}

void VideoSurface::applyDiagnosticReply(DiagnosticSlot slot, const AsyncReply& reply) {
    switch (slot) {
        case kDecoderDropSlot:
            diagnostics_.decoderDropCount = exactInteger(reply);
            break;
        case kVoDropSlot:
            diagnostics_.voDropCount = exactInteger(reply);
            break;
        case kVfSlot: {
            bool present = false;
            int count = 0;
            if (summarizeVfChain(reply, &present, &count)) {
                diagnostics_.nativeFilterPresent = present;
                diagnostics_.vfCount = count;
            } else {
                diagnostics_.nativeFilterPresent.reset();
                diagnostics_.vfCount.reset();
            }
            break;
        }
    }
}

void VideoSurface::setDiagnosticUnknown(DiagnosticSlot slot) {
    switch (slot) {
        case kDecoderDropSlot:
            diagnostics_.decoderDropCount.reset();
            break;
        case kVoDropSlot:
            diagnostics_.voDropCount.reset();
            break;
        case kVfSlot:
            diagnostics_.nativeFilterPresent.reset();
            diagnostics_.vfCount.reset();
            break;
    }
}

}  // namespace mplayer
