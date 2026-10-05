// The native playback transaction owner. It never parses mpv, never resolves an
// `aji` symbol and never issues a core call: it hands the surface one typed
// command at a time.
//
// Requested is not effective: a submitted admission changes only what is
// pending, and `effective` is copied from an observation whose whole plan
// describes the admitted one. Acknowledgement is never the transport's return.
// The device and artifact claims are held from before submission until an
// acknowledged stop or the death of the borrowed core, so endSource() MUST run
// AFTER that core has been synchronously destroyed - never while the filter and
// its jobs are still alive - and before this controller is destroyed.

#pragma once

#include <functional>
#include <optional>

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "native/prepare/NativeArtifactStore.hpp"
#include "native/status/NativeFilterCommand.hpp"
#include "native/prepare/NativePreparedPlan.hpp"
#include "native/prepare/NativePreparationPlan.hpp"
#include "native/status/NativeStatus.hpp"
#include "native/status/NativeTypes.hpp"
#include "native/prepare/NativeWorkLease.hpp"

namespace mplayer {
namespace native {

/// What the surface must do with the live core. `Attach` carries the exact `vf`
/// value; `Admit`/`Drain`/`Suspend`/`Resume` carry the compact JSON argument of
/// `vf-command mikai_native <verb>`; `Off` carries the `vf remove` target.
enum class NativePlaybackCommandKind { Attach, Admit, Drain, Suspend, Resume, Off };

struct NativePlaybackCommand {
  NativePlaybackCommandKind kind = NativePlaybackCommandKind::Attach;
  /// Attach: the exact `vf` value. Off: the `@label` removal target.
  /// Admit/Drain/Suspend/Resume: compact JSON bytes.
  QByteArray spec;
  /// The wire request id of this transition: the admission's own id, or a
  /// controller-minted one for a control action.
  QString requestId;
  quint64 epoch = 0;
  quint64 revision = 0;
  /// The bound the control wire carries; zero for Attach/Off.
  quint32 timeoutMs = 0;

  bool operator==(const NativePlaybackCommand& other) const {
    return kind == other.kind && spec == other.spec && requestId == other.requestId &&
           epoch == other.epoch && revision == other.revision && timeoutMs == other.timeoutMs;
  }
  bool operator!=(const NativePlaybackCommand& other) const { return !(*this == other); }
};

class NativePlaybackController {
 public:
  enum class State {
    Idle,       ///< No source.
    Attached,   ///< Probe filter attached, nothing admitted.
    Admitting,  ///< An admission is submitted and unacknowledged; nothing effective yet.
    Active,     ///< An observed snapshot acknowledged the whole admitted plan.
    Draining,   ///< A drain/suspend/off transition is in flight.
    Suspended,  ///< Suspension acknowledged; claims released.
    Off,        ///< Native processing off; the filter may still be attached.
    Failed      ///< A named non-Active end of a bounded transition.
  };

  /// The observed verdict of one call, for the consumer that must decide what to
  /// show or retry. It never reports Active from a request alone.
  enum class Outcome {
    Ignored,
    Pending,
    Activated,
    Suspended,
    Drained,
    Removed,
    Refused,
    Cancelled,
    TimedOut
  };

  struct Decision {
    Outcome outcome = Outcome::Ignored;
    QString reasonKey;
    QString reason;

    bool operator==(const Decision& other) const {
      return outcome == other.outcome && reasonKey == other.reasonKey && reason == other.reason;
    }
    bool operator!=(const Decision& other) const { return !(*this == other); }
  };

  /// What the surface may show. `effective`/`counters` are present only after an
  /// observation acknowledged them; `requested` is what was asked for and is
  /// never promoted into `effective`.
  struct Status {
    State state = State::Idle;
    quint64 sourceEpoch = 0;
    bool confirmed = false;
    std::optional<NativeEffectivePlan> effective;
    std::optional<NativeCadenceCounters> counters;
    AvailabilityState availability;
    std::optional<SelectionState> requested;
    /// The id of the acknowledged admission; empty until one is confirmed.
    QString requestId;
    /// The id of the transition in flight; empty when none is.
    QString pendingRequestId;
    /// The artifact keys currently claimed for the effective plan.
    QStringList buildKeys;
    QString reasonKey;
    QString reason;
  };

  /// The bound used when the caller names none; it is also what the control wire
  /// carries, so the filter and the watchdog cannot disagree.
  static constexpr quint32 kDefaultControlTimeoutMs = 10000;

  NativePlaybackController();
  ~NativePlaybackController();

  NativePlaybackController(const NativePlaybackController&) = delete;
  NativePlaybackController& operator=(const NativePlaybackController&) = delete;

  void setCommandSink(std::function<void(const NativePlaybackCommand&)> sink);
  void setControlTimeoutMs(quint32 timeoutMs);

  /// The options every `Attach` spec this controller builds carries, in order.
  /// Empty is the probe-only binding, which can pass frames through but can
  /// never admit: the filter opens its inference shim only for an explicitly
  /// named producer library, so a caller that intends an admission supplies the
  /// owning bundle's own `lib=<aji.dll>` here, proved against that bundle. The
  /// controller never guesses a path; an option the core cannot take is refused
  /// by name when the attach is built.
  void setAttachOptions(const QVector<NativeFilterOption>& options);

  /// Brackets one source epoch. Observations read under any other epoch are
  /// dropped; the effective plan and its claims survive a same-session epoch
  /// change, because a source reset must never reload an engine.
  void beginSource(quint64 sourceEpoch);
  /// Releases every claim and forgets the session. MUST run AFTER the borrowed
  /// core has been synchronously destroyed, and before this controller is.
  void endSource();

  /// Attach the filter. Safe with no artifact and no engine: with no attach
  /// options it binds a probe that passes frames through until a plan is
  /// admitted. A caller that intends an admission first names the owning
  /// bundle's own producer library through `setAttachOptions`, because the
  /// filter opens its inference shim only for an explicitly named one and a
  /// chain bound without it can never admit.
  Decision attach();
  /// Resolve and submit an admission. It acquires the inference claim before any
  /// work is submitted and an artifact claim per resolved artifact; on refusal
  /// nothing is emitted and the previous admission is untouched.
  Decision requestAdmission(const PreparedAdmissionRequest& request, qint64 nowMs);
  /// Stop processing in order. On acknowledgement the device and artifact claims
  /// are released and the acknowledged plan is dropped.
  Decision drain(qint64 nowMs);
  /// Suspend acknowledged resources; `resume` revalidates artifacts and device.
  Decision suspend(qint64 nowMs);
  Decision resume(qint64 nowMs);
  /// Requested off: drain, then remove the filter. Never a media retry.
  Decision requestOff(qint64 nowMs);
  /// Abandons the transition in flight and releases its artifact claims. The
  /// device claim stays held: only an acknowledged drain/suspend/removal or core
  /// destruction releases it.
  Decision cancelPending();

  /// Holds plan changes for a frozen capture: requests are recorded, not
  /// delivered, so the effective plan cannot move under a capture. Refused while
  /// a transition is already in flight, whose settling epoch is already moving.
  bool lockPlan();
  /// Releases one lock; at depth zero the queued request, if any, is submitted.
  Decision unlockPlan(qint64 nowMs);
  bool planLocked() const { return planLockDepth_ > 0; }
  /// True while a request is queued behind the lock.
  bool hasQueuedTransition() const { return queuedPlan_.has_value(); }
  /// True while a transition is submitted and unacknowledged.
  bool hasPendingTransition() const { return pending_.has_value(); }

  /// Drops every plan lock and the queued request without submitting it: the
  /// frozen capture is over because the session is being torn down.
  void abandonPlanLock();
  /// Marks the session as tearing down. A transition that ends without an
  /// acknowledgement then retains its claims instead of releasing an artifact
  /// the filter may still be loading, so nothing in flight is dropped; only core
  /// destruction (endSource) releases what was retained. A cancelled local
  /// pending is never a native acknowledgement, and no stop is faked here.
  void beginTeardown();
  bool teardownActive() const { return teardown_; }

  /// The decoder reported new source facts. A changed display geometry or
  /// cadence drops a pending admission and opens a new temporal generation;
  /// nothing is built or loaded here. True when it invalidated the pending work.
  bool onSourceGeometry(const SourceGeometry& source);

  /// Applies one observed reader view read under `sourceEpoch`.
  Decision observe(const NativeStatusView& view, quint64 sourceEpoch, qint64 nowMs);
  /// The transport's result for a command. A late or superseded id is dropped;
  /// an accepted transport is progress, never acknowledgement.
  Decision onCommandCompleted(const QString& requestId, bool transportAccepted,
                              const QString& reasonKey = QString(),
                              const QString& reason = QString());
  /// Bounded deadline watchdog.
  Decision tick(qint64 nowMs);

  const Status& status() const { return status_; }
  State state() const { return status_.state; }
  bool holdsInferenceLease() const { return inferenceLease_.has_value(); }
  quint64 sourceEpoch() const { return sourceEpoch_; }
  quint64 generation() const { return generation_; }
  quint64 lastObservedRevision() const { return lastObservedRevision_; }

 private:
  /// The acknowledged or pending admission's identity and claims.
  struct Admission {
    PreparedAdmissionRequest request;
    QVector<ArtifactLease> leases;
    QStringList buildKeys;
    bool confirmed = false;
  };

  struct Pending {
    NativePlaybackCommandKind kind = NativePlaybackCommandKind::Admit;
    QString requestId;
    quint64 epoch = 0;
    quint64 revision = 0;
    quint64 generation = 0;
    qint64 deadlineMs = 0;
    QVector<ArtifactLease> leases;
    QStringList buildKeys;
    std::optional<PreparedAdmissionRequest> request;
    std::optional<PreparationPlan> plan;
    SelectionState selection;
    QByteArray payload;
    bool removeAfterDrain = false;
  };

  /// The published sentence for the transition a lock is holding, so a caption
  /// reading the status carries the real queued meaning.
  QString queuedRequestReason() const;
  /// The transition a locked request recorded; the last request wins.
  struct QueuedPlan {
    NativePlaybackCommandKind kind = NativePlaybackCommandKind::Attach;
    PreparedAdmissionRequest admission;
  };

  Decision queueWhileLocked(NativePlaybackCommandKind kind,
                            const PreparedAdmissionRequest* admission);
  /// Moves the pending transition's artifact claims somewhere that outlives it.
  void retainPendingClaims();
  /// Takes every artifact claim this controller currently holds, so a removal
  /// can keep them pinned across it.
  QVector<ArtifactLease> takeHeldClaims();
  /// Releases every claim this controller holds. Only an acknowledged stop may
  /// call this; nothing that ends without one does.
  void releaseNativeClaims();
  /// Submits the tracked removal: only its accepted transport plus the observed
  /// absence of the filter's document may turn it into Removed.
  Decision startRemoval(qint64 nowMs);
  Decision startControl(NativePlaybackCommandKind kind, const char* action, bool removeAfterDrain,
                        qint64 nowMs);
  Decision deliver(const NativePlaybackCommand& command);
  Decision refuse(const QString& reasonKey, const QString& reason);
  Decision ignored(const QString& reasonKey = QString(), const QString& reason = QString());
  Decision pendingOutcome(const QString& reasonKey = QString(), const QString& reason = QString());
  void clearPending();
  void clearAdmission();
  void dropEffective();
  void noteReason(const QString& reasonKey, const QString& reason);
  Decision onDeadline(qint64 nowMs);
  Decision applySnapshot(const StatusSnapshot& snapshot, qint64 nowMs);
  Decision applyUnavailable(const NativeStatusView& view);
  QString nextRequestId();

  std::function<void(const NativePlaybackCommand&)> sink_;
  quint32 controlTimeoutMs_ = kDefaultControlTimeoutMs;
  /// What the filter is bound with. Empty is probe-only; the surface adds the
  /// proved producer library before it asks for an admission.
  QVector<NativeFilterOption> attachOptions_;
  quint64 sourceEpoch_ = 0;
  quint64 generation_ = 0;
  quint64 nextRequestIdCounter_ = 0;
  bool sourceOpen_ = false;
  quint64 lastObservedRevision_ = 0;
  std::optional<StatusSnapshot> lastObserved_;
  std::optional<NativeWorkLease> inferenceLease_;
  std::optional<Admission> admission_;
  std::optional<Pending> pending_;
  int planLockDepth_ = 0;
  /// The source epoch the lock was taken under. A queued request is dropped
  /// rather than applied to a different source: a plan resolved for one source
  /// says nothing about another.
  quint64 lockEpoch_ = 0;
  std::optional<QueuedPlan> queuedPlan_;
  bool teardown_ = false;
  /// Claims a teardown retained because no acknowledgement ever arrived. Held
  /// until endSource: releasing them would unpin an artifact a live filter may
  /// still be reading.
  QVector<ArtifactLease> retainedTeardownLeases_;
  Status status_;
};

}  // namespace native
}  // namespace mplayer
