#include "FramePreviewService.hpp"

#include "MpvApi.hpp"
#include "MpvHandle.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <mpv/client.h>

#include <cmath>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mplayer {
namespace shell {

namespace {

// ── worker cadence ─────────────────────────────────────────────────────────
// Busy: a local seek settles in tens of milliseconds. Idle: keep the event
// queue drained without a wakeup per painted frame.
constexpr int kBusyTickMs = 8;
constexpr int kIdleTickMs = 200;

// ── bounds ────────────────────────────────────────────────────────────────
constexpr qint64 kLoadTimeoutMs = 20000;
constexpr qint64 kSeekTimeoutMs = 4000;
constexpr qint64 kCaptureTimeoutMs = 5000;
// A failing source is answered from memory instead of being retried per pixel.
constexpr qint64 kRetryAfterFailureMs = 2500;
// A loaded source nobody hovers holds a demuxer cache and, for a stream, a
// connection; the next hover pays a reload instead. Five minutes: a viewer who
// steps away and comes back with the pointer should not pay a reopen, and any
// hover or capture in between resets the clock, so this is idleness and not the
// length of the session.
constexpr qint64 kIdleDropMs = 300000;

// Within one frame time is the same frame: a pointer nudging must not throw.
constexpr double kSameInstantSec = 0.04;
// A sweep would be a seek per pixel; the decode waits for the pointer to rest.
// Twenty milliseconds is one frame at 50 Hz: enough to coalesce a move, short
// enough that the card answers the instant the pointer stopped on.
constexpr qint64 kHoverSettleMs = 20;
// A jump further than this is crossed by a keyframe seek first, then refined to
// the exact instant. Two and a half seconds is roughly one GOP on the sources
// this player sees: below it an exact seek is already the cheaper path.
constexpr double kKeyframeJumpSec = 2.5;
// `absolute+exact` lands on the requested frame, so a larger miss is a refused
// seek dressed as a success rather than ordinary rounding.
constexpr double kPositionToleranceSec = 0.75;

// A hostile or broken reply must not choose how much this process allocates.
constexpr int kMaxPreviewSide = 1024;
constexpr int kMaxPreviewStride = 1 << 20;

// One monotonic clock for every deadline in this file.
qint64 nowMs() {
    static const QElapsedTimer clock = [] {
        QElapsedTimer timer;
        timer.start();
        return timer;
    }();
    return clock.elapsed();
}

const NodeValue* nodeMember(const NodeValue& map, const char* key) {
    for (const std::pair<std::string, NodeValue>& member : map.map) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

/// `vo=null` plus `screenshot-sw=yes` is the whole trick: the capture reads the
/// decoded frame from the software chain, so no video output, GL context or
/// window is involved. The rest keeps a preview's cost bounded and silent.
std::vector<std::pair<std::string, std::string>> previewOptions() {
    return {
        {"vo", "null"},
        {"pause", "yes"},
        {"audio", "no"},
        {"sub", "no"},
        {"aid", "no"},
        {"sid", "no"},
        {"keep-open", "yes"},
        {"screenshot-sw", "yes"},
        // The capture reads the software chain, so no hardware decoder can be
        // substituted: these are the speed options the paused preview core may
        // take without changing what the picture is.
        {"hwdec", "no"},
        {"vd-lavc-fast", "yes"},
        {"vd-lavc-skiploopfilter", "all"},
        {"hr-seek-framedrop", "yes"},
        {"terminal", "no"},
        {"config", "no"},
        {"idle", "yes"},
        {"force-window", "no"},
        {"osc", "no"},
        {"osd-level", "0"},
        {"input-default-bindings", "no"},
        {"input-vo-keyboard", "no"},
        {"load-scripts", "no"},
        {"ytdl", "no"},
        {"sub-auto", "no"},
        {"audio-file-auto", "no"},
        // Same identity the live surface presents: MoonAnime answers 400 to a
        // playlist that arrives without Accept-Language.
        {"user-agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
         "Chrome/142.0.0.0 Safari/537.36"},
        {"http-header-fields", "Accept-Language: uk-UA"},
        {"demuxer-readahead-secs", "3"},
        {"demuxer-max-bytes", "33554432"},
        {"demuxer-max-back-bytes", "8388608"},
        // The card is a few hundred pixels wide at most, so the frame leaves the
        // decoder already inside a 640x640 box; the cross-thread copy and the
        // QImage stay small for a 4K source, portrait video included.
        {"vf", "scale=w=640:h=640:force_original_aspect_ratio=decrease:flags=bilinear"},
    };
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  capture decode
// ═══════════════════════════════════════════════════════════════════════════

QImage previewFrameFromRaw(const QByteArray& format, qint64 width, qint64 height, qint64 stride,
                           const char* payload, qsizetype size, QString* reason) {
    const auto refuse = [reason](const QString& text) {
        if (reason != nullptr) *reason = text;
        return QImage();
    };
    if (format != QByteArrayLiteral("rgba")) {
        return refuse(QCoreApplication::translate(
            "mplayer::shell::FramePreviewService",
            "The captured frame is not in the requested RGBA order."));
    }
    if (width <= 0 || height <= 0 || width > kMaxPreviewSide || height > kMaxPreviewSide) {
        return refuse(QCoreApplication::translate(
            "mplayer::shell::FramePreviewService",
            "The capture declared a size this build cannot hold."));
    }
    if (stride < width * 4 || stride > kMaxPreviewStride) {
        return refuse(QCoreApplication::translate(
            "mplayer::shell::FramePreviewService",
            "The capture pitch does not cover the declared width."));
    }
    // qint64, not qsizetype: `stride * height` overflows a 32-bit qsizetype
    // before the comparison could reject it.
    if (payload == nullptr || static_cast<qint64>(size) < stride * height) {
        return refuse(QCoreApplication::translate(
            "mplayer::shell::FramePreviewService",
            "The capture payload is shorter than the declared rows."));
    }
    QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGBA8888);
    if (image.isNull()) {
        return refuse(QCoreApplication::translate(
            "mplayer::shell::FramePreviewService",
            "The captured frame could not be allocated."));
    }
    // Padding beyond width * 4 per row is dropped here: the image is packed.
    const qsizetype rowBytes = static_cast<qsizetype>(width) * 4;
    const int rows = static_cast<int>(height);
    for (int y = 0; y < rows; ++y) {
        std::memcpy(image.scanLine(y), payload + static_cast<qsizetype>(y) * stride,
                    static_cast<size_t>(rowBytes));
    }
    if (reason != nullptr) reason->clear();
    return image;
}

// ═══════════════════════════════════════════════════════════════════════════
//  cache
// ═══════════════════════════════════════════════════════════════════════════

qint64 PreviewFrameCache::bucketOf(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) return -1;
    return static_cast<qint64>(std::llround(seconds * 2.0));
}

void PreviewFrameCache::clear() {
    entries_.clear();
    recency_.clear();
}

void PreviewFrameCache::insert(quint64 epoch, const PreviewFrame& frame) {
    const qint64 bucket = bucketOf(frame.requestedSeconds);
    if (bucket < 0 || frame.image.isNull()) return;
    const Key key(epoch, bucket);
    if (!entries_.contains(key)) {
        while (recency_.size() >= kCapacity) {
            // Oldest first: the instant a pointer is least likely to return to.
            const Key victim = recency_.takeFirst();
            entries_.remove(victim);
            ++evictions_;
        }
        recency_.append(key);
    } else {
        recency_.removeAll(key);
        recency_.append(key);
    }
    entries_.insert(key, frame);
}

std::optional<PreviewFrame> PreviewFrameCache::lookup(quint64 epoch,
                                                      double requestedSeconds) {
    const qint64 bucket = bucketOf(requestedSeconds);
    if (bucket < 0) return std::nullopt;
    const Key key(epoch, bucket);
    const auto found = entries_.constFind(key);
    if (found == entries_.constEnd()) return std::nullopt;
    // A hit is a use: the instant the pointer came back to is now the newest.
    recency_.removeAll(key);
    recency_.append(key);
    return found.value();
}

// ═══════════════════════════════════════════════════════════════════════════
//  worker
// ═══════════════════════════════════════════════════════════════════════════

class FramePreviewWorker : public QObject {
    Q_OBJECT

public:
    explicit FramePreviewWorker(QObject* parent = nullptr);
    ~FramePreviewWorker() override;

public slots:
    void initialize();
    void setSource(const QString& url, quint64 generation);
    void requestFrame(quint64 token, double seconds);
    void noteActivity();
    void cancelPending();
    void shutdown();

signals:
    void frameReady(quint64 token, double requestedSeconds, double decodedSeconds,
                    const QImage& frame);
    void frameUnavailable(quint64 token, double seconds, const QString& reason);
    /// Seeks mpv accepted so far, in issue order. Published from the worker
    /// because that is where a seek is actually sent.
    void seeksChanged(quint64 total);

private:
    /// One step at a time; a superseding request is served once the current
    /// step settles, never in the middle of it.
    enum class Phase { Idle, Load, Seek, Capture };

    void tick();
    void tickLoad();
    void tickSeek();
    void tickCapture();
    void tickIdle();

    void pump();
    bool ensureCore();
    void releaseCore();

    void startLoad();
    void confirmLoad();
    void startSeek();
    void confirmSeek();
    void captureFrame();

    void finishWithFrame(quint64 token, double requestedSeconds, double decodedSeconds,
                         const QImage& frame);
    void fail(const QString& reason);
    void abandon();

    void onEvent(mpv_event* event);
    void updateTimer();
    QString coreError(int error) const;

    QTimer* timer_ = nullptr;
    std::unique_ptr<MpvHandle> core_;
    QString url_;
    /// Empty until this URL is loaded and seekable; pump() loads when it
    /// disagrees with `url_` and seeks otherwise.
    QString loadedUrl_;
    quint64 generation_ = 0;

    Phase phase_ = Phase::Idle;
    /// The request being served and the newest asked for; a reply is published
    /// only when both still name it.
    quint64 activeToken_ = 0;
    double activeSeconds_ = 0.0;
    quint64 requestedToken_ = 0;
    double requestedSeconds_ = 0.0;

    /// Where the core said the picture it just captured actually is. Reported by
    /// the seek's own position read, never assumed from the request: the card
    /// prints this time and the cache describes its picture by it.
    double decodedSeconds_ = -1.0;
    /// Where the last completed seek landed. A jump further than
    /// kKeyframeJumpSec from it is crossed by keyframes first.
    double settledSeconds_ = -1.0;

    /// Baselines are taken when a step is issued, so an event from the previous
    /// step is never read as this one's completion.
    int fileLoadedCount_ = 0;
    int restartCount_ = 0;
    int endCount_ = 0;
    int lastEndError_ = 0;
    int fileLoadedBaseline_ = 0;
    int restartBaseline_ = 0;
    int endBaseline_ = 0;

    qint64 deadlineMs_ = 0;
    qint64 retryAfterMs_ = 0;
    qint64 idleSinceMs_ = 0;
    /// Seeks mpv accepted. Monotonic for the lifetime of this worker.
    quint64 seekCount_ = 0;
    /// Pushed forward by every hover: only a pointer at rest is served.
    qint64 startAfterMs_ = 0;
    /// A completion read is in flight; the tick must not issue a second one.
    bool confirming_ = false;
    bool seekRetried_ = false;
    bool idleDropDone_ = false;
    bool shuttingDown_ = false;

    AsyncRequestId positionRequest_;
    AsyncRequestId captureRequest_;
    /// The last failure sentence, replayed by the retry window.
    QString failure_;
};

FramePreviewWorker::FramePreviewWorker(QObject* parent) : QObject(parent) {}

FramePreviewWorker::~FramePreviewWorker() {
    // Deleted through deleteLater on the worker thread, so the core dies where
    // it was created; shutdown() has already dropped it on the ordinary path.
    core_.reset();
}

void FramePreviewWorker::initialize() {
    if (timer_ != nullptr) return;
    timer_ = new QTimer(this);
    timer_->setTimerType(Qt::CoarseTimer);
    timer_->setInterval(kIdleTickMs);
    connect(timer_, &QTimer::timeout, this, &FramePreviewWorker::tick);
}

void FramePreviewWorker::setSource(const QString& url, quint64 generation) {
    // Sources arrive in publication order; an older generation is already gone.
    if (generation < generation_) return;
    generation_ = generation;
    if (url == url_) return;
    url_ = url;
    requestedToken_ = 0;
    activeToken_ = 0;
    phase_ = Phase::Idle;
    loadedUrl_.clear();
    failure_.clear();
    retryAfterMs_ = 0;
    confirming_ = false;
    seekRetried_ = false;
    idleDropDone_ = false;
    settledSeconds_ = -1.0;
    decodedSeconds_ = -1.0;
    positionRequest_ = AsyncRequestId{};
    captureRequest_ = AsyncRequestId{};
    if (core_ != nullptr) {
        // A capture or read for the replaced source must not land in the new
        // one's state machine.
        core_->cancelAllAsync();
        try {
            core_->stop();
        } catch (const MpvError&) {
            // Nothing was loaded to stop.
        }
    }
    if (url_.isEmpty()) releaseCore();
    updateTimer();
}

void FramePreviewWorker::requestFrame(quint64 token, double seconds) {
    if (shuttingDown_ || token == 0 || !std::isfinite(seconds) || seconds < 0.0) return;
    if (url_.isEmpty()) {
        emit frameUnavailable(token, seconds,
                              tr("No media is loaded, so there is no frame to preview."));
        return;
    }
    if (nowMs() < retryAfterMs_) {
        // Answered from memory: a sweep must not re-attempt a refusing source.
        emit frameUnavailable(token, seconds,
                              failure_.isEmpty()
                                  ? tr("The frame preview is not available for this source.")
                                  : failure_);
        return;
    }
    requestedToken_ = token;
    requestedSeconds_ = seconds;
    startAfterMs_ = nowMs() + kHoverSettleMs;
    // A hover is activity: the five-minute idle release counts a decoder nobody
    // asked anything of, not a session that has been open for five minutes.
    idleSinceMs_ = startAfterMs_;
    pump();
    updateTimer();
}

void FramePreviewWorker::noteActivity() {
    // Playback, a pointer or a transport action: the decoder is still wanted.
    idleSinceMs_ = nowMs();
}

void FramePreviewWorker::cancelPending() {
    requestedToken_ = 0;
    activeToken_ = 0;
    if (phase_ == Phase::Capture) {
        if (core_ != nullptr && captureRequest_.valid()) core_->cancelAsync(captureRequest_);
        captureRequest_ = AsyncRequestId{};
        abandon();
    }
    // A load or a seek cannot be recalled: it settles into Idle and answers
    // nothing, because `activeToken_` no longer names a request.
    updateTimer();
}

void FramePreviewWorker::shutdown() {
    shuttingDown_ = true;
    requestedToken_ = 0;
    activeToken_ = 0;
    phase_ = Phase::Idle;
    if (timer_ != nullptr) timer_->stop();
    releaseCore();
}

// ── state machine ─────────────────────────────────────────────────────────

void FramePreviewWorker::tick() {
    if (shuttingDown_) return;
    // Bounded batch; every reply callback runs here, on the worker thread.
    if (core_ != nullptr) core_->drainEvents();
    switch (phase_) {
        case Phase::Load:
            tickLoad();
            break;
        case Phase::Seek:
            tickSeek();
            break;
        case Phase::Capture:
            tickCapture();
            break;
        case Phase::Idle:
            tickIdle();
            break;
    }
    // Outside mpv's event dispatch: a step that finished above starts the
    // newest request here.
    pump();
    updateTimer();
}

void FramePreviewWorker::tickLoad() {
    if (activeToken_ == 0) {
        abandon();
        return;
    }
    if (endCount_ > endBaseline_ && lastEndError_ != 0) {
        fail(tr("The source could not be opened: %1").arg(coreError(lastEndError_)));
        return;
    }
    if (nowMs() > deadlineMs_) {
        fail(tr("The source did not open in time, so there is no frame to preview."));
        return;
    }
    // Both events, because a capture taken before the first frame was displayed
    // would read the pre-seek position: the restart is what proves it was shown.
    if (!confirming_ && restartCount_ > restartBaseline_ &&
        fileLoadedCount_ > fileLoadedBaseline_) {
        confirmLoad();
    }
}

void FramePreviewWorker::confirmLoad() {
    confirming_ = true;
    // A decodable source can still be unseekable - a live stream, a pipe - and
    // a preview of an instant it cannot reach would have to show another frame.
    const AsyncRequestId request = core_->getPropertyAsync(
        "seekable", MPV_FORMAT_FLAG, [this](const AsyncReply& reply) {
            if (phase_ != Phase::Load) return;
            if (activeToken_ == 0) {
                abandon();
                return;
            }
            if (!reply.ok() || !reply.flag) {
                fail(tr("This source cannot be seeked, so no frame preview is available."));
                // It will not learn to: the longer window spares a sweep a
                // reload every few seconds.
                retryAfterMs_ = nowMs() + 60000;
                return;
            }
            loadedUrl_ = url_;
            startSeek();
        });
    if (!request.valid()) {
        fail(tr("The player would not report whether this source can be seeked."));
    }
}

void FramePreviewWorker::tickSeek() {
    if (activeToken_ == 0) {
        abandon();
        return;
    }
    if (endCount_ > endBaseline_ && lastEndError_ != 0) {
        fail(tr("The source stopped before the hovered position could be decoded: %1")
                 .arg(coreError(lastEndError_)));
        return;
    }
    if (nowMs() > deadlineMs_) {
        fail(tr("The seek did not finish in time, so there is no frame to preview."));
        return;
    }
    if (!confirming_ && restartCount_ > restartBaseline_) {
        confirmSeek();
    }
}

void FramePreviewWorker::confirmSeek() {
    confirming_ = true;
    // A restart says the seek finished; the clock proves the frame the capture
    // will see is the one under the pointer.
    positionRequest_ = core_->getPropertyAsync(
        "time-pos", MPV_FORMAT_DOUBLE, [this](const AsyncReply& reply) {
            positionRequest_ = AsyncRequestId{};
            if (phase_ != Phase::Seek) return;
            if (activeToken_ == 0) {
                abandon();
                return;
            }
            if (!reply.ok() || !std::isfinite(reply.number)) {
                fail(tr("The player could not report the position of the requested frame."));
                return;
            }
            // The decoder's own reading of where it landed. It is what the card
            // prints and what the cache describes the picture by, so it is taken
            // before anything else can move.
            decodedSeconds_ = reply.number;
            if (activeToken_ != requestedToken_ ||
                std::abs(requestedSeconds_ - activeSeconds_) > kSameInstantSec) {
                // Still moving: the newest instant is left to pump() once the
                // pointer rests, which is what keeps a sweep from becoming a
                // seek per settled seek.
                if (requestedToken_ == 0 || nowMs() < startAfterMs_) {
                    abandon();
                    return;
                }
                // At rest on a newer instant: seek that one instead.
                activeToken_ = requestedToken_;
                activeSeconds_ = requestedSeconds_;
                seekRetried_ = false;
                startSeek();
                return;
            }
            if (std::abs(reply.number - activeSeconds_) > kPositionToleranceSec) {
                if (!seekRetried_) {
                    // The keyframe pass landed on a neighbouring frame; one
                    // exact seek is what refines it.
                    seekRetried_ = true;
                    startSeek();
                    return;
                }
                fail(tr("The source would not settle on the hovered position."));
                return;
            }
            settledSeconds_ = reply.number;
            captureFrame();
        });
    if (!positionRequest_.valid()) {
        fail(tr("The player would not take the position read."));
    }
}

void FramePreviewWorker::tickCapture() {
    if (activeToken_ == 0) {
        abandon();
        return;
    }
    if (nowMs() > deadlineMs_) {
        if (core_ != nullptr && captureRequest_.valid()) core_->cancelAsync(captureRequest_);
        captureRequest_ = AsyncRequestId{};
        fail(tr("The frame could not be captured in time."));
    }
}

void FramePreviewWorker::tickIdle() {
    if (core_ == nullptr || loadedUrl_.isEmpty() || idleDropDone_) return;
    if (nowMs() - idleSinceMs_ < kIdleDropMs) return;
    // The core stays, so the next hover is a load and not a decoder creation.
    idleDropDone_ = true;
    core_->cancelAllAsync();
    positionRequest_ = AsyncRequestId{};
    captureRequest_ = AsyncRequestId{};
    try {
        core_->stop();
    } catch (const MpvError&) {
        // Nothing was loaded any more.
    }
    loadedUrl_.clear();
}

// ── steps ─────────────────────────────────────────────────────────────────

void FramePreviewWorker::pump() {
    if (shuttingDown_) return;
    if (phase_ != Phase::Idle || requestedToken_ == 0 || url_.isEmpty()) return;
    if (nowMs() < retryAfterMs_ || nowMs() < startAfterMs_) return;
    if (!ensureCore()) return;
    activeToken_ = requestedToken_;
    activeSeconds_ = requestedSeconds_;
    idleDropDone_ = false;
    seekRetried_ = false;
    confirming_ = false;
    if (loadedUrl_ != url_) {
        startLoad();
    } else {
        startSeek();
    }
}

bool FramePreviewWorker::ensureCore() {
    if (core_ != nullptr) return true;
    if (!MpvApi::instance().loaded()) {
        const QString reason =
            tr("The video engine is not available, so the frame cannot be decoded.");
        failure_ = reason;
        retryAfterMs_ = nowMs() + kRetryAfterFailureMs;
        const quint64 token = requestedToken_;
        const double seconds = requestedSeconds_;
        requestedToken_ = 0;
        activeToken_ = 0;
        if (token != 0) emit frameUnavailable(token, seconds, reason);
        return false;
    }
    try {
        core_ = std::make_unique<MpvHandle>(previewOptions());
    } catch (const std::exception& error) {
        const QString reason =
            tr("The preview decoder could not be created: %1").arg(QString::fromUtf8(error.what()));
        failure_ = reason;
        retryAfterMs_ = nowMs() + kRetryAfterFailureMs;
        const quint64 token = requestedToken_;
        const double seconds = requestedSeconds_;
        requestedToken_ = 0;
        activeToken_ = 0;
        if (token != 0) emit frameUnavailable(token, seconds, reason);
        return false;
    }
    core_->setEventSink([this](mpv_event* event) { onEvent(event); });
    updateTimer();
    return true;
}

void FramePreviewWorker::releaseCore() {
    if (core_ == nullptr) {
        if (timer_ != nullptr) timer_->stop();
        return;
    }
    core_->cancelAllAsync();
    positionRequest_ = AsyncRequestId{};
    captureRequest_ = AsyncRequestId{};
    core_.reset();
    if (timer_ != nullptr) timer_->stop();
}

void FramePreviewWorker::startLoad() {
    phase_ = Phase::Load;
    confirming_ = false;
    loadedUrl_.clear();
    fileLoadedBaseline_ = fileLoadedCount_;
    restartBaseline_ = restartCount_;
    endBaseline_ = endCount_;
    deadlineMs_ = nowMs() + kLoadTimeoutMs;
    try {
        core_->loadFile(url_.toStdString());
    } catch (const std::exception& error) {
        fail(tr("The source could not be opened: %1").arg(QString::fromUtf8(error.what())));
    }
}

void FramePreviewWorker::startSeek() {
    phase_ = Phase::Seek;
    confirming_ = false;
    restartBaseline_ = restartCount_;
    endBaseline_ = endCount_;
    deadlineMs_ = nowMs() + kSeekTimeoutMs;
    // A long jump is crossed by keyframes first: `absolute+exact` on the far side
    // of a two-hour file decodes everything it crosses, which is what made a
    // first hover on a late instant take seconds. The position read that follows
    // proves where it landed, and a miss is refined by one exact seek - so the
    // frame the card gets is still the frame at the instant, never the keyframe's
    // neighbour presented as it.
    const bool farJump =
        !seekRetried_ && (settledSeconds_ < 0.0 ||
                          std::abs(activeSeconds_ - settledSeconds_) > kKeyframeJumpSec);
    const char* mode = farJump ? "absolute+keyframes" : "absolute+exact";
    const quint64 token = activeToken_;
    const AsyncRequestId request = core_->commandAsync(
        {"seek", mpvNumber(activeSeconds_), mode},
        [this, token](const AsyncReply& reply) {
            if (!reply.ok()) {
                // A refusal for a superseded seek says nothing about its replacement.
                if (phase_ != Phase::Seek) return;
                if (token != activeToken_ || token != requestedToken_) return;
                fail(tr("The source refused the seek: %1").arg(coreError(reply.error)));
                return;
            }
            // Counted where mpv accepted it, not where the request was queued: an
            // accepted seek was paid for even when the state machine moved on.
            emit seeksChanged(++seekCount_);
        });
    if (!request.valid()) fail(tr("The player would not take the seek request."));
}

void FramePreviewWorker::captureFrame() {
    phase_ = Phase::Capture;
    deadlineMs_ = nowMs() + kCaptureTimeoutMs;
    const quint64 token = activeToken_;
    const double requested = activeSeconds_;
    const double decoded = decodedSeconds_;
    const AsyncRequestId request = core_->commandAsync(
        {"screenshot-raw", "video", "rgba"},
        [this, token, requested, decoded](const AsyncReply& reply) {
            captureRequest_ = AsyncRequestId{};
            if (phase_ != Phase::Capture) return;
            if (token != activeToken_) {
                // The state machine has moved to another instant: this capture
                // describes neither the request in flight nor one still wanted.
                abandon();
                return;
            }
            if (!reply.ok()) {
                fail(tr("The frame could not be captured: %1").arg(coreError(reply.error)));
                return;
            }
            const NodeValue& node = reply.node;
            if (node.format != MPV_FORMAT_NODE_MAP) {
                fail(tr("The frame capture did not report its pixels."));
                return;
            }
            const NodeValue* width = nodeMember(node, "w");
            const NodeValue* height = nodeMember(node, "h");
            const NodeValue* stride = nodeMember(node, "stride");
            const NodeValue* format = nodeMember(node, "format");
            const NodeValue* data = nodeMember(node, "data");
            if (width == nullptr || height == nullptr || stride == nullptr || format == nullptr ||
                data == nullptr) {
                fail(tr("The frame capture is missing a declared member."));
                return;
            }
            QString reason;
            const QImage frame = previewFrameFromRaw(
                QByteArray::fromStdString(format->text), width->integer, height->integer,
                stride->integer, data->bytes.data(),
                static_cast<qsizetype>(data->bytes.size()), &reason);
            if (frame.isNull()) {
                fail(reason.isEmpty()
                         ? tr("The captured frame could not be read.")
                         : reason);
                return;
            }
            finishWithFrame(token, requested, decoded, frame);
        });
    if (!request.valid()) {
        fail(tr("The player would not take the capture request."));
    } else {
        captureRequest_ = request;
    }
}

// ── terminal states ───────────────────────────────────────────────────────

void FramePreviewWorker::finishWithFrame(quint64 token, double requestedSeconds,
                                         double decodedSeconds, const QImage& frame) {
    phase_ = Phase::Idle;
    activeToken_ = 0;
    idleSinceMs_ = nowMs();
    failure_.clear();
    retryAfterMs_ = 0;
    // A finished decode is never thrown away. The pointer may already be on a
    // newer instant - the card then holds this picture as an explicitly stale
    // one and the store keeps it for a return - and the newest request is served
    // by the next pump().
    if (requestedToken_ == token) requestedToken_ = 0;
    if (token != 0) emit frameReady(token, requestedSeconds, decodedSeconds, frame);
}

void FramePreviewWorker::fail(const QString& reason) {
    failure_ = reason;
    retryAfterMs_ = nowMs() + kRetryAfterFailureMs;
    // The newest request is the one the card waits for; it asks about the same
    // source, and the retry window would answer it with this same sentence.
    const quint64 token = requestedToken_;
    const double seconds = requestedSeconds_;
    phase_ = Phase::Idle;
    activeToken_ = 0;
    idleSinceMs_ = nowMs();
    positionRequest_ = AsyncRequestId{};
    captureRequest_ = AsyncRequestId{};
    // The core is left with nothing loaded, so the retry window is what decides
    // when the source is tried again.
    if (core_ != nullptr) {
        core_->cancelAllAsync();
        try {
            core_->stop();
        } catch (const MpvError&) {
            // Nothing was loaded.
        }
    }
    loadedUrl_.clear();
    if (token != 0) {
        requestedToken_ = 0;
        emit frameUnavailable(token, seconds, reason);
    }
}

void FramePreviewWorker::abandon() {
    phase_ = Phase::Idle;
    activeToken_ = 0;
    idleSinceMs_ = nowMs();
    positionRequest_ = AsyncRequestId{};
    captureRequest_ = AsyncRequestId{};
}

// ── core plumbing ─────────────────────────────────────────────────────────

void FramePreviewWorker::onEvent(mpv_event* event) {
    if (event == nullptr) return;
    switch (event->event_id) {
        case MPV_EVENT_FILE_LOADED:
            ++fileLoadedCount_;
            break;
        case MPV_EVENT_PLAYBACK_RESTART:
            ++restartCount_;
            break;
        case MPV_EVENT_END_FILE:
            if (event->data != nullptr) {
                const auto* ended = static_cast<const mpv_event_end_file*>(event->data);
                ++endCount_;
                // EOF and a deliberate stop are not failures; only the error
                // reason names a source that could not be decoded.
                lastEndError_ = ended->reason == MPV_END_FILE_REASON_ERROR ? ended->error : 0;
            }
            break;
        default:
            break;
    }
}

void FramePreviewWorker::updateTimer() {
    if (timer_ == nullptr) return;
    // A core with nothing loaded and nothing asked for has no step to advance.
    if (shuttingDown_ || (requestedToken_ == 0 && phase_ == Phase::Idle && loadedUrl_.isEmpty())) {
        timer_->stop();
        return;
    }
    // A loaded source nobody hovers still ticks slowly: that tick is what
    // releases it after kIdleDropMs.
    const bool busy = phase_ != Phase::Idle || requestedToken_ != 0;
    timer_->setInterval(busy ? kBusyTickMs : kIdleTickMs);
    if (!timer_->isActive()) timer_->start();
}

QString FramePreviewWorker::coreError(int error) const {
    if (core_ == nullptr) return QStringLiteral("mpv");
    return QString::fromStdString(core_->describeError(error));
}

// ═══════════════════════════════════════════════════════════════════════════
//  service
// ═══════════════════════════════════════════════════════════════════════════

FramePreviewService::FramePreviewService(QObject* parent) : QObject(parent) {}

FramePreviewService::~FramePreviewService() { shutdown(); }

void FramePreviewService::setSource(const QString& url, quint64 sourceEpoch) {
    if (stopped_) return;
    // Both halves are identity: the same URL republished for a reloaded source
    // is a different picture, and treating it as "unchanged" is how a stale
    // frame ends up describing the new media.
    if (url == source_ && sourceEpoch == sourceEpoch_) return;
    source_ = url;
    sourceEpoch_ = sourceEpoch;
    pendingToken_ = 0;
    prewarmToken_ = 0;
    // Everything stored describes a source that is no longer loaded.
    cache_.clear();
    ++generation_;
    if (worker_ != nullptr) emit sourceChanged(source_, generation_);
}

void FramePreviewService::requestFrame(quint64 token, double seconds) {
    if (stopped_ || token == 0 || isInternal(token) || !std::isfinite(seconds) || seconds < 0.0) {
        return;
    }
    ++stats_.requests;
    // A card's request outranks a warm-up; the warm-up is asked again by its
    // owner when the pointer is gone.
    prewarmToken_ = 0;
    pendingToken_ = token;
    pendingSince_.start();
    if (source_.isEmpty()) {
        pendingToken_ = 0;
        stats_.lastSettleLatencyMs = pendingSince_.elapsed();
        emit frameUnavailable(token, seconds,
                              tr("No media is loaded, so there is no frame to preview."));
        return;
    }
    if (const std::optional<PreviewFrame> cached = cache_.lookup(sourceEpoch_, seconds)) {
        // The store answers instead of the decoder: no settle delay, no seek and
        // nothing on the worker thread. The reply keeps the two times apart, so
        // the card never prints this request's instant over that picture.
        QElapsedTimer timer;
        timer.start();
        ++stats_.cacheHits;
        pendingToken_ = 0;
        stats_.lastHitLatencyUs = timer.nsecsElapsed() / 1000;
        stats_.lastSettleLatencyMs = timer.elapsed();
        emit frameReady(token, seconds, cached->decodedSeconds, cached->image);
        return;
    }
    ++stats_.workerRequests;
    // The epoch is part of the request's identity: a reply is only ever a
    // picture of the source this request was made against.
    if (requestEpochs_.size() > 128) requestEpochs_.clear();
    requestEpochs_.insert(token, sourceEpoch_);
    ensureWorker();
    emit frameRequested(token, seconds);
}

void FramePreviewService::prewarm(double seconds) {
    if (stopped_ || source_.isEmpty() || !std::isfinite(seconds) || seconds < 0.0) return;
    if (prewarmToken_ != 0 || pendingToken_ != 0) return;
    if (cache_.lookup(sourceEpoch_, seconds).has_value()) return;
    prewarmToken_ = nextInternalToken_++;
    ensureWorker();
    emit frameRequested(prewarmToken_, seconds);
}

void FramePreviewService::noteActivity() {
    // No worker means no decoder to keep warm, and creating one for a mouse move
    // would put a second libmpv core beside a stream that is still opening.
    if (stopped_ || worker_ == nullptr) return;
    if (activitySince_.isValid() && activitySince_.elapsed() < 1000) return;
    activitySince_.start();
    emit activityNoted();
}

void FramePreviewService::cancel() {
    pendingToken_ = 0;
    prewarmToken_ = 0;
    if (worker_ != nullptr) emit cancelRequested();
}

void FramePreviewService::shutdown() {
    if (stopped_) return;
    stopped_ = true;
    pendingToken_ = 0;
    QThread* thread = thread_;
    FramePreviewWorker* worker = worker_;
    thread_ = nullptr;
    worker_ = nullptr;
    if (thread == nullptr) return;
    if (worker != nullptr) {
        // Never waited on: the core dies on the worker, and the GUI returns here.
        disconnect(worker, nullptr, this, nullptr);
        const bool queued = QMetaObject::invokeMethod(worker, [worker, thread] {
            worker->shutdown();
            thread->quit();
        }, Qt::QueuedConnection);
        if (!queued) thread->quit();
    } else {
        thread->quit();
    }
}

void FramePreviewService::ensureWorker() {
    if (stopped_ || thread_ != nullptr) return;
    thread_ = new QThread;
    thread_->setObjectName(QStringLiteral("framePreview"));
    auto* worker = new FramePreviewWorker;
    worker_ = worker;
    worker->moveToThread(thread_);
    connect(thread_, &QThread::started, worker, &FramePreviewWorker::initialize);
    // deleteLater, so the core's destructor still runs on the worker thread.
    connect(thread_, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread_, &QThread::finished, thread_, &QObject::deleteLater);
    connect(this, &FramePreviewService::sourceChanged, worker, &FramePreviewWorker::setSource,
            Qt::QueuedConnection);
    connect(this, &FramePreviewService::frameRequested, worker, &FramePreviewWorker::requestFrame,
            Qt::QueuedConnection);
    connect(this, &FramePreviewService::cancelRequested, worker, &FramePreviewWorker::cancelPending,
            Qt::QueuedConnection);
    connect(this, &FramePreviewService::activityNoted, worker, &FramePreviewWorker::noteActivity,
            Qt::QueuedConnection);
    connect(worker, &FramePreviewWorker::seeksChanged, this,
            [this](quint64 total) { stats_.seeks = total; }, Qt::QueuedConnection);
    connect(worker, &FramePreviewWorker::frameReady, this,
            [this](quint64 token, double requestedSeconds, double decodedSeconds,
                   const QImage& frame) {
                if (stopped_) return;
                if (isInternal(token)) {
                    // A warm-up: it fills the store for the next hover and is
                    // never published as if the card had asked for it.
                    if (token != prewarmToken_) return;
                    prewarmToken_ = 0;
                    cache_.insert(sourceEpoch_,
                                  PreviewFrame{requestedSeconds, decodedSeconds, frame});
                    return;
                }
                const quint64 epoch = requestEpochs_.take(token);
                if (epoch != sourceEpoch_) {
                    // The source changed while this reply was crossing threads:
                    // it describes a picture that is no longer loaded.
                    return;
                }
                cache_.insert(epoch, PreviewFrame{requestedSeconds, decodedSeconds, frame});
                if (token == pendingToken_) {
                    pendingToken_ = 0;
                    stats_.lastSettleLatencyMs = pendingSince_.elapsed();
                }
                // Delivered even when a newer request is already outstanding: the
                // card holds it as an explicitly stale picture, and its own token
                // check decides whether this is the instant under the pointer.
                emit frameReady(token, requestedSeconds, decodedSeconds, frame);
            });
    connect(worker, &FramePreviewWorker::frameUnavailable, this,
            [this](quint64 token, double seconds, const QString& reason) {
                if (stopped_ || isInternal(token)) return;
                if (token != pendingToken_) return;
                pendingToken_ = 0;
                stats_.lastSettleLatencyMs = pendingSince_.elapsed();
                emit frameUnavailable(token, seconds, reason);
            });
    thread_->start();
    // The worker starts with no source; everything it needs is published after
    // it exists, in the same order the caller sees.
    if (!source_.isEmpty()) emit sourceChanged(source_, generation_);
}

}  // namespace shell
}  // namespace mplayer

#include "FramePreviewService.moc"
