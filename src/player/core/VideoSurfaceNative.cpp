// Native status polling, probe alignment, prepared plan admission, and
// software reference-frame capture state machine for VideoSurface.

#include "VideoSurface.hpp"

#include "MpvApi.hpp"
#include "MpvHandle.hpp"
#include "native/bundle/NativeBundle.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "native/status/NativeFilterCommand.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace mplayer {
namespace {

constexpr qint64 kRestoreSettleTimeoutMs = 5000;
constexpr int kRestoreSettleMaxAttempts = 5;

std::string mpvNumber(double value) {
    return QString::number(value, 'f', 6).toStdString();
}


/// The runtime module the live core was loaded from, or empty when none was
/// selected. bundle() hands back a reference into the loader's state, so the
/// value is copied here rather than read after the lock is gone.
QString currentMpvModulePath() {
    MpvApi& api = MpvApi::instance();
    if (!api.loaded()) return QString();
    return api.bundle().modulePath;
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

// ── native status ─────────────────────────────────────────────────────────
const char* VideoSurface::nativePlaybackStateName(
    native::NativePlaybackController::State state) {
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
        {"seek", mpvNumber(adjacent), "absolute+exact"},
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
            {"seek", mpvNumber(restore), "absolute+exact"},
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
                    {"seek", mpvNumber(restore), "absolute+exact"},
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
        mpv_->commandAsync({"seek", mpvNumber(*resume), "absolute+exact"},
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
        mpv_->commandAsync({"seek", mpvNumber(*resume), "absolute+exact"},
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


}  // namespace mplayer
