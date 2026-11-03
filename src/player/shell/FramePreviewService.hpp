// The frame behind the timeline's hover card: a second, headless libmpv core
// (vo=null, screenshot-sw=yes) that decodes one frame per hovered instant on
// its own thread, so a hover never moves the player's playhead, never paints
// through its render context and never blocks the GUI thread.

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QtGlobal>

#include <optional>

QT_BEGIN_NAMESPACE
class QThread;
QT_END_NAMESPACE

namespace mplayer {
namespace shell {

class FramePreviewWorker;

/// Copies one `screenshot-raw video rgba` capture into an owned frame.
///
/// Every declared size is validated before it indexes anything: the channel
/// order must be RGBA, the pitch must cover the width, and a payload shorter
/// than the declared rows is refused rather than read past. Padding beyond
/// `width * 4` per row is dropped. A refused capture is a null image and, when
/// `reason` is given, the sentence that says why.
QImage previewFrameFromRaw(const QByteArray& format, qint64 width, qint64 height, qint64 stride,
                           const char* payload, qsizetype size, QString* reason = nullptr);

/// One decoded preview frame.
///
/// Two times, deliberately: `requestedSeconds` is the instant the pointer named
/// when the decode was issued, which is what a later request is matched
/// against, and `decodedSeconds` is the timestamp the decoder itself reported
/// for the picture, which is what the card may print and what the cache
/// describes. Labelling a picture with the pointer's instant when it is a
/// different frame is the one thing a preview must never do.
struct PreviewFrame {
    double requestedSeconds = -1.0;
    double decodedSeconds = -1.0;
    QImage image;
};

/// Bounded, recency-ordered store of decoded preview frames.
///
/// Keyed by `(source epoch, 0.5 s bucket of the requested instant)`: two hovers
/// of the same instant serve the same picture, and a source reloaded under the
/// same URL starts a new epoch rather than reusing the old picture. Sixty-four
/// entries is a full sweep of a normal episode's timeline in both directions
/// plus room to return to a recent instant.
class PreviewFrameCache {
public:
    /// Entries kept. A sweep that leaves the oldest ones behind is exactly what
    /// makes a second pass fast; keeping every frame of a two-hour film is how a
    /// thumbnail cache becomes a memory leak.
    static constexpr int kCapacity = 64;

    /// Bucket index for an instant: 2 buckets per second, so a request landing
    /// anywhere in a half second shares the frame already decoded for it.
    static qint64 bucketOf(double seconds);

    void clear();

    /// Stores or refreshes `frame` for `(epoch, requestedSeconds)` and drops the
    /// least recently used entry when the store is full.
    void insert(quint64 epoch, const PreviewFrame& frame);

    /// The frame for `(epoch, requestedSeconds)`, marked as just used. Nullopt is
    /// a miss - a different epoch or a bucket never decoded is not "close enough".
    std::optional<PreviewFrame> lookup(quint64 epoch, double requestedSeconds);

    int size() const { return entries_.size(); }
    /// How many entries have been dropped to stay inside kCapacity. Monotonic,
    /// so a consumer can tell eviction from never-having-stored.
    quint64 evictions() const { return evictions_; }

private:
    using Key = QPair<quint64, qint64>;
    QHash<Key, PreviewFrame> entries_;
    /// Least recently used first. Linear, because the store never holds more
    /// than kCapacity keys and a hash with a timestamp column costs more than it
    /// saves at this size.
    QList<Key> recency_;

    quint64 evictions_ = 0;
};

/// What the preview path actually did, so a card that feels slow can be told
/// apart from a cache that is not working.
struct FramePreviewStats {
    /// Requests the card issued. Each one is either a cache hit or a worker
    /// request; the two counters below must add up to this.
    quint64 requests = 0;
    /// Answered from the store, with no worker round trip, no settle delay and
    /// no seek.
    quint64 cacheHits = 0;
    /// Forwarded to the decoder: a load, a seek and a capture at most once.
    quint64 workerRequests = 0;
    /// Seek commands mpv accepted on the preview core, counted on the worker
    /// where they are issued: an exact one, or a keyframe pass plus the exact
    /// refinement that follows it, per request served from a miss. A request is
    /// not a seek, so this is the number a "the store answered" claim uses.
    quint64 seeks = 0;
    /// Wall time of the last cache hit, including the reply. A hit is a lookup
    /// and a signal, so this is microseconds in practice and any number above a
    /// millisecond means something else answered.
    qint64 lastHitLatencyUs = -1;
    /// Wall time from the last request to its frame (or its refusal).
    qint64 lastSettleLatencyMs = -1;
};

/// Decodes single frames of the current source on its own thread.
///
/// The service lives on the GUI thread and owns the thread; the mpv core is
/// created, used and destroyed on the worker, and only plain values cross.
class FramePreviewService : public QObject {
    Q_OBJECT

public:
    explicit FramePreviewService(QObject* parent = nullptr);
    ~FramePreviewService() override;

    /// The media the next request decodes from, and the identity of the loaded
    /// source it belongs to.
    ///
    /// The epoch is what makes a same-URL reload a new source: a player that
    /// retries a stream publishes the same URL again with the core's own epoch,
    /// and a frame decoded before that must never describe the picture after it.
    /// The core is not created here - a window nobody hovers costs no decoder.
    void setSource(const QString& url, quint64 sourceEpoch);
    QString source() const { return source_; }
    quint64 sourceEpoch() const { return sourceEpoch_; }

    /// Asks for the frame at `seconds`; the answer carries `token` back, so a
    /// caller that hovers faster than the decoder shows only its last request.
    /// `token` 0 is not a request. An instant already in the store answers from
    /// it, without touching the worker.
    void requestFrame(quint64 token, double seconds);

    /// Warms the decoder for `seconds` without a card waiting on it: the frame
    /// lands in the store and is never published as a reply. Low priority by
    /// construction - the worker serves the newest request, so a hover that
    /// arrives during a warm-up replaces it, and a caller that only prewarms
    /// while playback is genuinely running never competes with the first frame.
    void prewarm(double seconds);

    /// Player activity that is not a hover: playback advancing, a pointer move
    /// over the window, a seek, a pause. A warm preview core is worth keeping
    /// exactly as long as somebody is still watching, so the idle release counts
    /// a session nobody touched rather than a session that has been open for
    /// five minutes. Never creates a worker, and a burst of activity is
    /// coalesced to one message per second, so a per-frame caller queues nothing.
    void noteActivity();

    /// Drops the outstanding request. A capture in flight is cancelled; a seek
    /// or a load that cannot be recalled settles without answering.
    void cancel();

    /// Stops the worker without waiting for it: the core is destroyed on the
    /// worker thread and the thread quits behind the GUI. Idempotent, and a
    /// shut service starts nothing afterwards.
    void shutdown();

    FramePreviewStats stats() const { return stats_; }

signals:
    /// `requestedSeconds` is the instant the request named (the identity a
    /// caller matches its own token and time against); `decodedSeconds` is the
    /// timestamp the decoder reported for `frame`. A superseded decode that
    /// finished is still delivered, so a card can hold it as an explicitly
    /// stale picture instead of dropping it.
    void frameReady(quint64 token, double requestedSeconds, double decodedSeconds,
                    const QImage& frame);
    /// No frame exists for that instant - an unseekable source, a refused
    /// capture - and `reason` is the sentence that says so.
    void frameUnavailable(quint64 token, double seconds, const QString& reason);

    // ── posted to the worker; not part of the consumer's contract ──────────
    void sourceChanged(const QString& url, quint64 generation);
    void frameRequested(quint64 token, double seconds);
    void cancelRequested();
    void activityNoted();

private:
    void ensureWorker();
    /// Internal requests carry this bit, so a warm-up reply can never be
    /// mistaken for a card's request.
    static constexpr quint64 kInternalTokenBit = quint64(1) << 63;
    static bool isInternal(quint64 token) { return (token & kInternalTokenBit) != 0; }

    QThread* thread_ = nullptr;
    FramePreviewWorker* worker_ = nullptr;
    QString source_;
    quint64 sourceEpoch_ = 0;
    PreviewFrameCache cache_;
    FramePreviewStats stats_;
    /// Bumped per source change, so the worker can tell a request issued
    /// against the source it holds from one issued against a replaced URL.
    quint64 generation_ = 0;
    quint64 pendingToken_ = 0;
    /// The source epoch each outstanding request was issued under. A reply is
    /// stored and published only under the epoch it asked about, so a signal
    /// still crossing threads when the source changed cannot describe the new one.
    QHash<quint64, quint64> requestEpochs_;
    /// Started with each worker request; read when its reply settles.
    QElapsedTimer pendingSince_;
    /// The last activity this service forwarded, so a caller that reports every
    /// mouse move and every playback tick costs one comparison.
    QElapsedTimer activitySince_;
    quint64 prewarmToken_ = 0;
    quint64 nextInternalToken_ = kInternalTokenBit;
    bool stopped_ = false;
};

}  // namespace shell
}  // namespace mplayer
