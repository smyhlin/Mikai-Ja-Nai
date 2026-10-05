#include "NativePlaybackController.hpp"

#include <utility>

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

#include "NativeFilterCommand.hpp"

namespace mplayer {
namespace native {

namespace {

/// The control document this build writes; the filter reads it as one argument.
inline constexpr const char* kControlSchema = "mikai.native.control/1";

QByteArray compactJson(const QJsonObject& object) {
  return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray controlPayload(const char* action, quint64 epoch, quint64 revision, quint32 timeoutMs) {
  QJsonObject object;
  object.insert(QStringLiteral("schema"), QString::fromLatin1(kControlSchema));
  object.insert(QStringLiteral("protocol"), 1);
  object.insert(QStringLiteral("action"), QString::fromLatin1(action));
  object.insert(QStringLiteral("epoch"), QJsonValue(static_cast<qint64>(epoch)));
  object.insert(QStringLiteral("revision"), QJsonValue(static_cast<qint64>(revision)));
  object.insert(QStringLiteral("timeoutMs"), QJsonValue(static_cast<qint64>(timeoutMs)));
  return compactJson(object);
}

QStringList artifactKeys(const PreparedAdmission& prepared) {
  QStringList keys;
  keys.reserve(prepared.artifacts.size());
  for (const ArtifactRecord& record : prepared.artifacts) keys.append(record.buildKey());
  return keys;
}

int spatialStageCount(const PreparationPlan& plan) {
  int count = 0;
  for (const ResolvedStage& stage : plan.stages) {
    if (stage.kind == StageKind::Spatial) ++count;
  }
  return count;
}

const ResolvedStage* temporalStage(const PreparationPlan& plan) {
  for (const ResolvedStage& stage : plan.stages) {
    if (stage.kind == StageKind::Temporal) return &stage;
  }
  return nullptr;
}

/// The observed effective plan must describe the whole chain this controller
/// admitted: its stage count, its temporal presence and factor, and the whole
/// plan's output geometry and cadence. A revision bump alone is never enough.
bool effectiveDescribesPlan(const NativeEffectivePlan& observed, const PreparationPlan& plan,
                            QString* reasonKey, QString* reason) {
  const auto refuse = [reasonKey, reason](const QString& key, const QString& text) {
    if (reasonKey != nullptr) *reasonKey = key;
    if (reason != nullptr) *reason = text;
    return false;
  };

  if (!plan.isResolved()) {
    return refuse(QStringLiteral("native.playback.planUnresolved"),
                  QStringLiteral("the admitted plan is not resolved"));
  }
  if (observed.recipe != plan.recipeId) {
    return refuse(QStringLiteral("native.playback.planMismatch"),
                  QStringLiteral("the observed recipe is not the admitted recipe"));
  }
  if (observed.provider != stableName(plan.backend)) {
    return refuse(QStringLiteral("native.playback.planMismatch"),
                  QStringLiteral("the observed provider is not the admitted backend"));
  }
  if (observed.spatialStages != spatialStageCount(plan)) {
    return refuse(QStringLiteral("native.playback.planMismatch"),
                  QStringLiteral("the observed spatial stage count is not the admitted one"));
  }

  const ResolvedStage* temporal = temporalStage(plan);
  if (observed.interpolation.has_value() != (temporal != nullptr)) {
    return refuse(QStringLiteral("native.playback.planMismatch"),
                  QStringLiteral("the observed chain runs the wrong number of temporal stages"));
  }
  if (temporal != nullptr && observed.interpolation->factor != temporal->factor) {
    return refuse(QStringLiteral("native.playback.planMismatch"),
                  QStringLiteral("the observed temporal factor is not the admitted one"));
  }

  const std::optional<QSize> output = plan.outputGeometry();
  if (!output.has_value() || observed.outputSize.w != output->width() ||
      observed.outputSize.h != output->height()) {
    return refuse(QStringLiteral("native.playback.planMismatch"),
                  QStringLiteral("the observed output geometry is not the admitted one"));
  }

  const std::optional<Rational> cadence = plan.outputCadence();
  if (!cadence.has_value() || !observed.outputFps.has_value() ||
      *observed.outputFps != *cadence) {
    return refuse(QStringLiteral("native.playback.planMismatch"),
                  QStringLiteral("the observed output cadence is not the admitted one"));
  }
  return true;
}

}  // namespace

NativePlaybackController::NativePlaybackController() {
  status_.availability = unavailableAvailability(QStringLiteral("native.playback.idle"),
                                                 QStringLiteral("no native source is open"));
  noteReason(QStringLiteral("native.playback.idle"), QStringLiteral("no native source is open"));
}

NativePlaybackController::~NativePlaybackController() = default;

void NativePlaybackController::setCommandSink(std::function<void(const NativePlaybackCommand&)> sink) {
  sink_ = std::move(sink);
}

void NativePlaybackController::setControlTimeoutMs(quint32 timeoutMs) {
  controlTimeoutMs_ = timeoutMs == 0 ? kDefaultControlTimeoutMs : timeoutMs;
}

void NativePlaybackController::setAttachOptions(const QVector<NativeFilterOption>& options) {
  attachOptions_ = options;
}

QString NativePlaybackController::nextRequestId() {
  return QStringLiteral("playback:%1").arg(++nextRequestIdCounter_);
}

NativePlaybackController::Decision NativePlaybackController::deliver(
    const NativePlaybackCommand& command) {
  if (!sink_) {
    return refuse(QStringLiteral("native.playback.noSink"),
                  QStringLiteral("no command sink is bound to the controller"));
  }
  sink_(command);
  return pendingOutcome();
}

NativePlaybackController::Decision NativePlaybackController::refuse(const QString& reasonKey,
                                                                   const QString& reason) {
  noteReason(reasonKey, reason);
  Decision decision;
  decision.outcome = Outcome::Refused;
  decision.reasonKey = reasonKey;
  decision.reason = reason;
  return decision;
}

NativePlaybackController::Decision NativePlaybackController::ignored(const QString& reasonKey,
                                                                    const QString& reason) {
  Decision decision;
  decision.outcome = Outcome::Ignored;
  decision.reasonKey = reasonKey;
  decision.reason = reason;
  return decision;
}

NativePlaybackController::Decision NativePlaybackController::pendingOutcome(
    const QString& reasonKey, const QString& reason) {
  Decision decision;
  decision.outcome = Outcome::Pending;
  decision.reasonKey = reasonKey;
  decision.reason = reason;
  return decision;
}

void NativePlaybackController::noteReason(const QString& reasonKey, const QString& reason) {
  status_.reasonKey = reasonKey;
  status_.reason = reason;
}

void NativePlaybackController::clearPending() {
  pending_.reset();
  status_.pendingRequestId.clear();
}

void NativePlaybackController::beginTeardown() { teardown_ = true; }

void NativePlaybackController::abandonPlanLock() {
  planLockDepth_ = 0;
  queuedPlan_.reset();
}

void NativePlaybackController::retainPendingClaims() {
  if (!pending_) return;
  for (ArtifactLease& lease : pending_->leases) {
    retainedTeardownLeases_.append(std::move(lease));
  }
  pending_->leases.clear();
  pending_->buildKeys.clear();
}

QVector<ArtifactLease> NativePlaybackController::takeHeldClaims() {
  QVector<ArtifactLease> claims;
  if (admission_) {
    for (ArtifactLease& lease : admission_->leases) claims.append(std::move(lease));
    admission_->leases.clear();
  }
  if (pending_) {
    for (ArtifactLease& lease : pending_->leases) claims.append(std::move(lease));
    pending_->leases.clear();
  }
  return claims;
}

void NativePlaybackController::releaseNativeClaims() {
  // Every claim this controller holds, including a teardown's retained set: only
  // an acknowledged stop may call this.
  takeHeldClaims();
  retainedTeardownLeases_.clear();
  admission_.reset();
  inferenceLease_.reset();
  status_.buildKeys.clear();
  status_.confirmed = false;
  status_.effective.reset();
  status_.counters.reset();
}

void NativePlaybackController::clearAdmission() {
  admission_.reset();
  status_.requestId.clear();
  status_.buildKeys.clear();
  status_.confirmed = false;
  status_.effective.reset();
  status_.counters.reset();
}

void NativePlaybackController::dropEffective() {
  if (admission_) admission_->confirmed = false;
  status_.confirmed = false;
  status_.effective.reset();
  status_.counters.reset();
}

void NativePlaybackController::beginSource(quint64 sourceEpoch) {
  if (sourceOpen_ && sourceEpoch == sourceEpoch_) return;
  sourceEpoch_ = sourceEpoch;
  sourceOpen_ = true;
  ++generation_;
  // The queued request is deliberately kept: the epoch guard is what refuses it
  // on release, by name, so a request resolved for the old source is never
  // applied here and the caller still learns why. Its published reason is
  // replaced by the observation note below, so no stale caption can survive.
  if (teardown_) retainPendingClaims();
  clearPending();
  lastObserved_.reset();
  lastObservedRevision_ = 0;
  status_.sourceEpoch = sourceEpoch;
  dropEffective();
  status_.state = admission_ ? State::Attached : State::Idle;
  status_.availability = unavailableAvailability(QStringLiteral("native.playback.awaitingObservation"),
                                                 QStringLiteral("the source changed; the filter has "
                                                                "not reported yet"));
  noteReason(QStringLiteral("native.playback.awaitingObservation"),
             QStringLiteral("the source changed; the filter has not reported yet"));
}

void NativePlaybackController::endSource() {
  sourceOpen_ = false;
  sourceEpoch_ = 0;
  ++generation_;
  planLockDepth_ = 0;
  queuedPlan_.reset();
  teardown_ = false;
  retainedTeardownLeases_.clear();
  clearPending();
  clearAdmission();
  inferenceLease_.reset();
  lastObserved_.reset();
  lastObservedRevision_ = 0;
  status_ = Status{};
  status_.availability = unavailableAvailability(QStringLiteral("native.playback.idle"),
                                                 QStringLiteral("no native source is open"));
  noteReason(QStringLiteral("native.playback.idle"), QStringLiteral("no native source is open"));
}

NativePlaybackController::Decision NativePlaybackController::attach() {
  if (!sourceOpen_) {
    return refuse(QStringLiteral("native.playback.noSource"), QStringLiteral("no source epoch is open"));
  }
  // A locked request is recorded, never delivered.
  if (planLocked()) return queueWhileLocked(NativePlaybackCommandKind::Attach, nullptr);
  QString reasonKey;
  QString reason;
  // The chain this attach binds is the one an admission will later be loaded
  // into, and the filter opens its inference shim only for an explicitly named
  // producer library. So the options the surface observed must reach here: a
  // binding made without the owning bundle's own shim can only pass frames
  // through and refuses every admission with a transport error that names
  // nothing. The controller names what it was told, never a guessed path.
  const QByteArray spec = nativeFilterSpec(attachOptions_, &reasonKey, &reason);
  if (spec.isEmpty()) {
    return refuse(reasonKey.isEmpty() ? QStringLiteral("native.filter.spec") : reasonKey, reason);
  }

  NativePlaybackCommand command;
  command.kind = NativePlaybackCommandKind::Attach;
  command.spec = spec;
  command.requestId = nextRequestId();
  command.epoch = sourceEpoch_;
  command.revision = lastObservedRevision_;

  if (status_.state == State::Idle || status_.state == State::Off || status_.state == State::Failed) {
    status_.state = State::Attached;
  }
  return deliver(command);
}

NativePlaybackController::Decision NativePlaybackController::requestAdmission(
    const PreparedAdmissionRequest& request, qint64 nowMs) {
  if (!sourceOpen_) {
    return refuse(QStringLiteral("native.playback.noSource"), QStringLiteral("no source epoch is open"));
  }
  // Before the binding check and any lease: a locked request takes no claim.
  if (planLocked()) return queueWhileLocked(NativePlaybackCommandKind::Admit, &request);
  if (status_.state == State::Idle || status_.state == State::Off) {
    return refuse(QStringLiteral("native.playback.notAttached"),
                  QStringLiteral("the native filter is not attached"));
  }
  if (request.requestId.isEmpty()) {
    return refuse(QStringLiteral("native.playback.requestId"),
                  QStringLiteral("an admission must carry a request id"));
  }
  if (sourceEpoch_ != 0 && request.epoch != sourceEpoch_) {
    return refuse(QStringLiteral("native.playback.staleEpoch"),
                  QStringLiteral("the admission was resolved for another source epoch"));
  }

  // The device claim is taken before any work is submitted, so a build cannot
  // start between this decision and the admission becoming effective.
  const bool alreadyHeld = inferenceLease_.has_value();
  if (!alreadyHeld) {
    std::optional<NativeWorkLease> lease = NativeWorkLease::acquire(NativeWorkKind::Inference);
    if (!lease.has_value()) {
      return refuse(QStringLiteral("native.playback.engineBusy"),
                    QStringLiteral("a preparation build is in progress, so inference cannot start"));
    }
    inferenceLease_ = std::move(*lease);
  }
  const bool running = admission_ && admission_->confirmed;

  QString reasonKey;
  QString reason;
  const std::optional<PreparedAdmission> prepared =
      resolvePreparedAdmission(request, &reasonKey, &reason);
  if (!prepared.has_value()) {
    if (!alreadyHeld && !running) inferenceLease_.reset();
    return refuse(reasonKey.isEmpty() ? QStringLiteral("native.playback.admissionRefused") : reasonKey,
                  reason);
  }

  clearPending();
  ++generation_;
  Pending next;
  next.kind = NativePlaybackCommandKind::Admit;
  next.requestId = request.requestId;
  next.epoch = request.epoch;
  next.revision = request.revision;
  next.generation = generation_;
  next.deadlineMs = nowMs + static_cast<qint64>(controlTimeoutMs_);
  next.leases = prepared->leases;
  next.buildKeys = artifactKeys(*prepared);
  next.request = request;
  next.plan = request.plan;
  next.selection = SelectionState{request.recipeId, request.backend};
  next.payload = compactJson(prepared->command);
  pending_ = std::move(next);

  status_.requested = SelectionState{request.recipeId, request.backend};
  status_.pendingRequestId = request.requestId;
  status_.sourceEpoch = sourceEpoch_;
  if (!running) {
    status_.state = State::Admitting;
    status_.confirmed = false;
  }
  noteReason(QStringLiteral("native.playback.admitting"),
             QStringLiteral("the admission is submitted and not yet observed"));

  NativePlaybackCommand command;
  command.kind = NativePlaybackCommandKind::Admit;
  command.spec = pending_->payload;
  command.requestId = pending_->requestId;
  command.epoch = pending_->epoch;
  command.revision = pending_->revision;
  command.timeoutMs = controlTimeoutMs_;
  return deliver(command);
}

bool NativePlaybackController::lockPlan() {
  if (!sourceOpen_) return false;
  // A transition in flight moves the epoch when it settles, so this is no freeze.
  if (pending_) return false;
  if (planLockDepth_ == 0) lockEpoch_ = sourceEpoch_;
  ++planLockDepth_;
  return true;
}

QString NativePlaybackController::queuedRequestReason() const {
  if (!queuedPlan_.has_value()) return QString();
  const char* action = "a native change";
  switch (queuedPlan_->kind) {
    case NativePlaybackCommandKind::Attach:
      action = "attaching the native filter";
      break;
    case NativePlaybackCommandKind::Admit:
      action = "the requested native recipe";
      break;
    case NativePlaybackCommandKind::Drain:
      action = "draining native processing";
      break;
    case NativePlaybackCommandKind::Suspend:
      action = "suspending native processing";
      break;
    case NativePlaybackCommandKind::Resume:
      action = "resuming native processing";
      break;
    case NativePlaybackCommandKind::Off:
      action = "turning native processing off";
      break;
  }
  return QStringLiteral("%1 is queued until the frozen capture closes").arg(QLatin1String(action));
}

NativePlaybackController::Decision NativePlaybackController::queueWhileLocked(
    NativePlaybackCommandKind kind, const PreparedAdmissionRequest* admission) {
  QueuedPlan queued;
  queued.kind = kind;
  if (admission != nullptr) queued.admission = *admission;
  queuedPlan_ = std::move(queued);
  // The published status has to move, or a caption reading it would never learn
  // that a request is waiting. Only the reason changes: the effective plan, its
  // identity, its claims, the requested selection and the source epoch stay put.
  const QString reason = queuedRequestReason();
  noteReason(QStringLiteral("native.playback.planLocked"), reason);
  return pendingOutcome(QStringLiteral("native.playback.planLocked"), reason);
}

NativePlaybackController::Decision NativePlaybackController::unlockPlan(qint64 nowMs) {
  if (planLockDepth_ <= 0) {
    return ignored(QStringLiteral("native.playback.noPlanLock"),
                   QStringLiteral("no native plan lock is held"));
  }
  --planLockDepth_;
  if (planLockDepth_ > 0) {
    // An outer lock still holds the request, so the queued reason stays.
    return ignored(QStringLiteral("native.playback.planLockRelease"),
                   QStringLiteral("an outer native plan lock is still held; the request stays "
                                  "queued"));
  }
  if (!queuedPlan_.has_value()) {
    // Nothing was waiting, so the queued reason has to be replaced: otherwise a
    // caption would keep claiming a request is queued behind a lock that is gone.
    noteReason(QStringLiteral("native.playback.planLockRelease"),
               QStringLiteral("the native plan lock was released with nothing queued"));
    return ignored(QStringLiteral("native.playback.planLockRelease"),
                   QStringLiteral("the native plan lock was released with nothing queued"));
  }
  if (sourceEpoch_ != lockEpoch_) {
    // A plan resolved for the frozen source says nothing about a new one.
    queuedPlan_.reset();
    return refuse(QStringLiteral("native.playback.lockSourceChanged"),
                  QStringLiteral("the source changed while the native plan was locked; the "
                                 "queued request was dropped"));
  }
  const QueuedPlan queued = std::move(*queuedPlan_);
  queuedPlan_.reset();
  if (queued.kind == NativePlaybackCommandKind::Attach) return attach();
  if (queued.kind == NativePlaybackCommandKind::Admit) {
    // The deferred admission kept no binding, so attach the prerequisite first.
    if (status_.state == State::Idle || status_.state == State::Off ||
        status_.state == State::Failed) {
      const Decision attached = attach();
      if (attached.outcome == Outcome::Refused) return attached;
    }
    return requestAdmission(queued.admission, nowMs);
  }
  switch (queued.kind) {
    case NativePlaybackCommandKind::Drain:
      return drain(nowMs);
    case NativePlaybackCommandKind::Suspend:
      return suspend(nowMs);
    case NativePlaybackCommandKind::Resume:
      return resume(nowMs);
    case NativePlaybackCommandKind::Off:
      return requestOff(nowMs);
    case NativePlaybackCommandKind::Attach:
    case NativePlaybackCommandKind::Admit:
      break;
  }
  return ignored(QStringLiteral("native.playback.planLockRelease"),
                 QStringLiteral("the queued native request is not one this controller can "
                                "submit"));
}

NativePlaybackController::Decision NativePlaybackController::startControl(
    NativePlaybackCommandKind kind, const char* action, bool removeAfterDrain, qint64 nowMs) {
  ++generation_;
  // The filter matches a control against the plan it stored, so the wire must
  // name the admitted plan's own epoch and revision - not the reader's last
  // observed revision, which advances independently and would be refused.
  const quint64 controlEpoch = admission_ ? admission_->request.epoch : sourceEpoch_;
  const quint64 controlRevision =
      admission_ ? admission_->request.revision : lastObservedRevision_;
  Pending next;
  next.kind = kind;
  next.requestId = nextRequestId();
  next.epoch = controlEpoch;
  next.revision = controlRevision;
  next.generation = generation_;
  next.deadlineMs = nowMs + static_cast<qint64>(controlTimeoutMs_);
  next.removeAfterDrain = removeAfterDrain;
  pending_ = std::move(next);

  status_.pendingRequestId = pending_->requestId;
  status_.state = State::Draining;

  NativePlaybackCommand command;
  command.kind = kind;
  command.spec = controlPayload(action, controlEpoch, controlRevision, controlTimeoutMs_);
  command.requestId = pending_->requestId;
  command.epoch = controlEpoch;
  command.revision = controlRevision;
  command.timeoutMs = controlTimeoutMs_;
  return deliver(command);
}

NativePlaybackController::Decision NativePlaybackController::drain(qint64 nowMs) {
  if (!sourceOpen_) {
    return refuse(QStringLiteral("native.playback.noSource"), QStringLiteral("no source epoch is open"));
  }
  if (planLocked()) return queueWhileLocked(NativePlaybackCommandKind::Drain, nullptr);
  if (!admission_) {
    return refuse(QStringLiteral("native.playback.nothingToDrain"),
                  QStringLiteral("no admitted plan is running"));
  }
  if (pending_) {
    return refuse(QStringLiteral("native.playback.transitionInFlight"),
                  QStringLiteral("another native transition is still in flight"));
  }
  noteReason(QStringLiteral("native.playback.draining"),
             QStringLiteral("a drain is submitted and not yet observed"));
  return startControl(NativePlaybackCommandKind::Drain, "drain", false, nowMs);
}

NativePlaybackController::Decision NativePlaybackController::suspend(qint64 nowMs) {
  if (!sourceOpen_) {
    return refuse(QStringLiteral("native.playback.noSource"), QStringLiteral("no source epoch is open"));
  }
  if (planLocked()) return queueWhileLocked(NativePlaybackCommandKind::Suspend, nullptr);
  if (!admission_ || !admission_->confirmed) {
    return refuse(QStringLiteral("native.playback.nothingToSuspend"),
                  QStringLiteral("no acknowledged plan is running"));
  }
  if (pending_) {
    return refuse(QStringLiteral("native.playback.transitionInFlight"),
                  QStringLiteral("another native transition is still in flight"));
  }
  noteReason(QStringLiteral("native.playback.suspending"),
             QStringLiteral("a suspend is submitted and not yet observed"));
  return startControl(NativePlaybackCommandKind::Suspend, "suspend", false, nowMs);
}

NativePlaybackController::Decision NativePlaybackController::resume(qint64 nowMs) {
  if (!sourceOpen_) {
    return refuse(QStringLiteral("native.playback.noSource"), QStringLiteral("no source epoch is open"));
  }
  if (planLocked()) return queueWhileLocked(NativePlaybackCommandKind::Resume, nullptr);
  if (!admission_) {
    return refuse(QStringLiteral("native.playback.notSuspended"),
                  QStringLiteral("no suspended plan is retained"));
  }
  if (status_.state != State::Suspended) {
    return refuse(QStringLiteral("native.playback.notSuspended"),
                  QStringLiteral("the native plan is not suspended"));
  }
  if (pending_) {
    return refuse(QStringLiteral("native.playback.transitionInFlight"),
                  QStringLiteral("another native transition is still in flight"));
  }

  // Suspended resources were released; revalidate the artifacts and device and
  // take the claims again before asking the filter to run.
  const bool alreadyHeld = inferenceLease_.has_value();
  if (!alreadyHeld) {
    std::optional<NativeWorkLease> lease = NativeWorkLease::acquire(NativeWorkKind::Inference);
    if (!lease.has_value()) {
      return refuse(QStringLiteral("native.playback.engineBusy"),
                    QStringLiteral("a preparation build is in progress, so inference cannot start"));
    }
    inferenceLease_ = std::move(*lease);
  }

  QString reasonKey;
  QString reason;
  const std::optional<PreparedAdmission> prepared =
      resolvePreparedAdmission(admission_->request, &reasonKey, &reason);
  if (!prepared.has_value()) {
    if (!alreadyHeld) inferenceLease_.reset();
    return refuse(reasonKey.isEmpty() ? QStringLiteral("native.playback.resumeRefused") : reasonKey,
                  reason);
  }
  admission_->leases = prepared->leases;
  admission_->buildKeys = artifactKeys(*prepared);
  status_.buildKeys = admission_->buildKeys;

  noteReason(QStringLiteral("native.playback.resuming"),
             QStringLiteral("a resume is submitted and not yet observed"));
  return startControl(NativePlaybackCommandKind::Resume, "resume", false, nowMs);
}

NativePlaybackController::Decision NativePlaybackController::requestOff(qint64 nowMs) {
  if (!sourceOpen_) {
    return refuse(QStringLiteral("native.playback.noSource"), QStringLiteral("no source epoch is open"));
  }
  if (planLocked()) return queueWhileLocked(NativePlaybackCommandKind::Off, nullptr);
  if (pending_) {
    return refuse(QStringLiteral("native.playback.transitionInFlight"),
                  QStringLiteral("another native transition is still in flight"));
  }

  if (admission_) {
    noteReason(QStringLiteral("native.playback.offDraining"),
               QStringLiteral("a drain precedes the requested removal"));
    return startControl(NativePlaybackCommandKind::Drain, "drain", true, nowMs);
  }
  if (status_.state == State::Idle) {
    return refuse(QStringLiteral("native.playback.nothingToRemove"),
                  QStringLiteral("no native filter is attached"));
  }
  // Attached, Failed, or a transition that ended without an acknowledgement: the
  // chain may still hold a filter or a plan nobody acknowledged stopping, so the
  // removal is tracked and its success is not inferred from local state.
  return startRemoval(nowMs);
}

NativePlaybackController::Decision NativePlaybackController::startRemoval(qint64 nowMs) {
  if (status_.availability.reasonKey == QLatin1String("native.status.missing")) {
    // The last observation this controller accepted already said the filter's
    // document is gone, so there is nothing left to ask for: that observation is
    // the acknowledgement, never a timeout read as success.
    releaseNativeClaims();
    status_.state = State::Off;
    noteReason(QStringLiteral("native.playback.removed"),
               QStringLiteral("the filter was already observed absent"));
    Decision decision;
    decision.outcome = Outcome::Removed;
    return decision;
  }
  QVector<ArtifactLease> carried = takeHeldClaims();
  const QStringList carriedKeys = status_.buildKeys;
  ++generation_;
  Pending next;
  next.kind = NativePlaybackCommandKind::Off;
  next.requestId = nextRequestId();
  next.epoch = sourceEpoch_;
  next.revision = lastObservedRevision_;
  next.generation = generation_;
  next.deadlineMs = nowMs + static_cast<qint64>(controlTimeoutMs_);
  next.leases = std::move(carried);
  next.buildKeys = carriedKeys;
  pending_ = std::move(next);
  // The plan bookkeeping is gone, but the claims it named are not: only the
  // observed removal releases them.
  admission_.reset();
  status_.pendingRequestId = pending_->requestId;
  status_.state = State::Draining;
  noteReason(QStringLiteral("native.playback.removing"),
             QStringLiteral("the filter removal is submitted and not yet observed"));
  NativePlaybackCommand command;
  command.kind = NativePlaybackCommandKind::Off;
  command.spec = QByteArray("@") + kNativeStatusLabel;
  command.requestId = pending_->requestId;
  command.epoch = sourceEpoch_;
  command.revision = lastObservedRevision_;
  return deliver(command);
}

NativePlaybackController::Decision NativePlaybackController::cancelPending() {
  if (!pending_) {
    return ignored(QStringLiteral("native.playback.noTransition"),
                   QStringLiteral("no native transition is in flight"));
  }
  const bool running = admission_ && admission_->confirmed;
  if (teardown_) retainPendingClaims();
  clearPending();
  ++generation_;
  status_.state = running ? State::Active : State::Attached;
  noteReason(QStringLiteral("native.playback.cancelled"),
             QStringLiteral("the transition in flight was abandoned"));
  Decision decision;
  decision.outcome = Outcome::Cancelled;
  decision.reasonKey = QStringLiteral("native.playback.cancelled");
  decision.reason = QStringLiteral("the transition in flight was abandoned");
  return decision;
}

bool NativePlaybackController::onSourceGeometry(const SourceGeometry& source) {
  if (!pending_ || pending_->kind != NativePlaybackCommandKind::Admit || !pending_->plan.has_value()) {
    return false;
  }
  const PreparationPlan& plan = *pending_->plan;
  const bool geometryMoved = plan.source.displaySize() != source.displaySize();
  const bool cadenceMoved = plan.source.fps != source.fps;
  if (!geometryMoved && !cadenceMoved) return false;

  const bool running = admission_ && admission_->confirmed;
  if (teardown_) retainPendingClaims();
  clearPending();
  ++generation_;
  status_.state = running ? State::Active : State::Attached;
  noteReason(QStringLiteral("native.playback.sourceChanged"),
             QStringLiteral("the source geometry or cadence changed; the pending admission was "
                            "dropped and no engine was loaded"));
  return true;
}

NativePlaybackController::Decision NativePlaybackController::observe(const NativeStatusView& view,
                                                                    quint64 sourceEpoch, qint64 nowMs) {
  if (!sourceOpen_ || sourceEpoch != sourceEpoch_) {
    return ignored(QStringLiteral("native.playback.staleEpoch"),
                   QStringLiteral("the observation belongs to a superseded source epoch"));
  }
  if (!view.snapshot.has_value()) return applyUnavailable(view);

  const StatusSnapshot& snapshot = *view.snapshot;
  QString consistencyKey;
  QString consistencyReason;
  if (!statusSnapshotIsConsistent(snapshot, &consistencyKey, &consistencyReason)) {
    return ignored(consistencyKey, consistencyReason);
  }
  if (lastObserved_.has_value()) {
    const StatusRevision revision = classifyStatusRevision(*lastObserved_, snapshot);
    if (revision == StatusRevision::Stale) {
      return ignored(QStringLiteral("native.playback.staleRevision"),
                     QStringLiteral("the observed revision is older than the last accepted one"));
    }
    if (revision == StatusRevision::Unchanged) {
      status_.availability = statusSnapshotAvailability(snapshot);
      return ignored(QStringLiteral("native.playback.unchangedRevision"),
                     QStringLiteral("the observed revision did not advance"));
    }
  }

  lastObserved_ = snapshot;
  lastObservedRevision_ = snapshot.revision;
  status_.sourceEpoch = sourceEpoch;
  status_.availability = statusSnapshotAvailability(snapshot);
  return applySnapshot(snapshot, nowMs);
}

NativePlaybackController::Decision NativePlaybackController::applyUnavailable(
    const NativeStatusView& view) {
  status_.availability = view.availability;
  // A removal is acknowledged by the filter's own document being gone, never by
  // a timeout or by local bookkeeping.
  if (pending_ && pending_->kind == NativePlaybackCommandKind::Off &&
      view.availability.reasonKey == QLatin1String("native.status.missing")) {
    clearPending();
    ++generation_;
    releaseNativeClaims();
    status_.state = State::Off;
    noteReason(QStringLiteral("native.playback.removed"),
               QStringLiteral("the filter acknowledged the removal by being absent"));
    Decision decision;
    decision.outcome = Outcome::Removed;
    return decision;
  }
  if (statusRefusalHaltsPolling(view.availability.reasonKey)) {
    if (pending_ && pending_->kind == NativePlaybackCommandKind::Off) {
      // A refusal about something else is not the removal's acknowledgement, and
      // the claims stay pinned while it is in doubt.
      return pendingOutcome(view.availability.reasonKey, view.availability.reason);
    }
    const bool running = admission_ && admission_->confirmed;
    if (pending_) {
      if (teardown_) retainPendingClaims();
      clearPending();
      ++generation_;
    }
    dropEffective();
    status_.state = running ? State::Failed : (status_.state == State::Off ? State::Off : State::Failed);
    return refuse(view.availability.reasonKey, view.availability.reason);
  }
  return ignored(view.availability.reasonKey, view.availability.reason);
}

NativePlaybackController::Decision NativePlaybackController::applySnapshot(
    const StatusSnapshot& snapshot, qint64 nowMs) {
  switch (snapshot.state) {
    case NativeState::Active: {
      if (!snapshot.effective.has_value()) {
        return ignored(QStringLiteral("native.playback.activeWithoutPlan"),
                       QStringLiteral("an Active snapshot carries no effective plan"));
      }
      if (pending_ && pending_->kind == NativePlaybackCommandKind::Admit &&
          pending_->plan.has_value() &&
          effectiveDescribesPlan(*snapshot.effective, *pending_->plan, nullptr, nullptr)) {
        // Acknowledgement: the whole admitted plan is the observed one.
        Admission next;
        next.request = std::move(*pending_->request);
        next.leases = std::move(pending_->leases);
        next.buildKeys = std::move(pending_->buildKeys);
        next.confirmed = true;
        admission_ = std::move(next);

        status_.requestId = admission_->request.requestId;
        status_.buildKeys = admission_->buildKeys;
        status_.effective = snapshot.effective;
        status_.counters = snapshot.counters;
        status_.confirmed = true;
        status_.state = State::Active;
        clearPending();
        ++generation_;
        noteReason(QString(), QString());
        Decision decision;
        decision.outcome = Outcome::Activated;
        return decision;
      }

      if (admission_ &&
          effectiveDescribesPlan(*snapshot.effective, admission_->request.plan, nullptr, nullptr)) {
        if (pending_) {
          return pendingOutcome(QStringLiteral("native.playback.replacementPending"),
                                QStringLiteral("the previous admitted plan is still the observed one"));
        }
        admission_->confirmed = true;
        status_.confirmed = true;
        status_.effective = snapshot.effective;
        status_.counters = snapshot.counters;
        status_.requestId = admission_->request.requestId;
        status_.buildKeys = admission_->buildKeys;
        status_.state = State::Active;
        noteReason(QString(), QString());
        Decision decision;
        decision.outcome = Outcome::Activated;
        return decision;
      }

      if (pending_) {
        return refuse(QStringLiteral("native.playback.planMismatch"),
                      QStringLiteral("the filter reports a plan this controller did not admit"));
      }
      return ignored(QStringLiteral("native.playback.foreignEffective"),
                     QStringLiteral("the observed plan belongs to no admission of this controller"));
    }

    case NativeState::Preparing:
    case NativeState::NeedsPreparation: {
      if (pending_) return pendingOutcome(QStringLiteral("native.playback.preparing"), snapshot.reason);
      return ignored(QStringLiteral("native.playback.preparing"), snapshot.reason);
    }

    case NativeState::Suspended: {
      if (pending_ && pending_->kind == NativePlaybackCommandKind::Suspend) {
        clearPending();
        ++generation_;
        inferenceLease_.reset();
        if (admission_) {
          admission_->leases.clear();
          admission_->buildKeys.clear();
        }
        dropEffective();
        status_.buildKeys.clear();
        status_.state = State::Suspended;
        noteReason(QStringLiteral("native.playback.suspended"),
                   QStringLiteral("the filter acknowledged suspension"));
        Decision decision;
        decision.outcome = Outcome::Suspended;
        return decision;
      }
      if (pending_) return pendingOutcome(QStringLiteral("native.playback.suspended"), snapshot.reason);
      dropEffective();
      status_.state = State::Suspended;
      return ignored(QStringLiteral("native.playback.suspended"), snapshot.reason);
    }

    case NativeState::Off:
    case NativeState::Probe: {
      if (pending_ && pending_->kind == NativePlaybackCommandKind::Drain) {
        const bool remove = pending_->removeAfterDrain;
        clearPending();
        ++generation_;
        if (remove) {
          // The drain is acknowledged, but the removal is its own transition:
          // Removed still needs its accepted transport plus the observed absence
          // of the filter, and the claims stay pinned until then.
          return startRemoval(nowMs);
        }
        inferenceLease_.reset();
        clearAdmission();
        status_.state = State::Off;
        noteReason(QStringLiteral("native.playback.drained"),
                   QStringLiteral("the filter acknowledged the drain"));
        Decision decision;
        decision.outcome = Outcome::Drained;
        return decision;
      }
      if (pending_) return pendingOutcome(QStringLiteral("native.playback.transitionPending"),
                                          snapshot.reason);
      if (admission_) dropEffective();
      status_.state = State::Off;
      return ignored(QStringLiteral("native.playback.offObserved"), snapshot.reason);
    }

    case NativeState::Failed:
    case NativeState::Unsupported:
    case NativeState::Deactivated: {
      const bool running = admission_ && admission_->confirmed;
      const QString key = QStringLiteral("native.playback.filterRefused");
      const QString reason = snapshot.reason.isEmpty()
                                ? QStringLiteral("the filter reported %1").arg(stableName(snapshot.state))
                                : snapshot.reason;
      if (pending_) {
        // A refused or failed filter is still not an acknowledged stop.
        if (teardown_ || pending_->kind == NativePlaybackCommandKind::Off) retainPendingClaims();
        clearPending();
        ++generation_;
        if (!running && !teardown_) inferenceLease_.reset();
        dropEffective();
        status_.state = State::Failed;
        return refuse(key, reason);
      }
      dropEffective();
      status_.state = State::Failed;
      return ignored(key, reason);
    }
  }
  return ignored(QStringLiteral("native.playback.unknownState"),
                 QStringLiteral("the observed state is not one this controller models"));
}

NativePlaybackController::Decision NativePlaybackController::onCommandCompleted(
    const QString& requestId, bool transportAccepted, const QString& reasonKey,
    const QString& reason) {
  if (!pending_ || pending_->requestId != requestId) {
    return ignored(QStringLiteral("native.playback.staleCompletion"),
                   QStringLiteral("the command completion belongs to a superseded transition"));
  }
  if (transportAccepted) {
    return pendingOutcome(QStringLiteral("native.playback.transportAccepted"),
                          QStringLiteral("the transport accepted the command; acknowledgement "
                                         "comes from the observation"));
  }

  const NativePlaybackCommandKind kind = pending_->kind;
  // A refused command is a named failure, never a stop: the claims stay pinned.
  if (teardown_ || kind == NativePlaybackCommandKind::Off) retainPendingClaims();
  clearPending();
  ++generation_;
  const QString key = reasonKey.isEmpty() ? QStringLiteral("native.playback.transportRefused") : reasonKey;
  const QString text = reason.isEmpty() ? QStringLiteral("the core refused the native command") : reason;
  if (kind == NativePlaybackCommandKind::Admit && !(admission_ && admission_->confirmed)) {
    status_.state = State::Failed;
    if (!teardown_) inferenceLease_.reset();
  } else if (kind == NativePlaybackCommandKind::Admit) {
    status_.state = State::Active;
  }
  return refuse(key, text);
}

NativePlaybackController::Decision NativePlaybackController::onDeadline(qint64 nowMs) {
  Q_UNUSED(nowMs);
  const NativePlaybackCommandKind kind = pending_->kind;
  // A transition that ends without an acknowledgement must not unpin an artifact
  // the filter may still be reading: the claims survive it, and a removal is
  // never reported as done from a timeout.
  if (teardown_ || kind == NativePlaybackCommandKind::Off) retainPendingClaims();
  clearPending();
  ++generation_;

  QString key;
  QString text;
  switch (kind) {
    case NativePlaybackCommandKind::Admit:
      key = QStringLiteral("native.playback.admissionTimeout");
      text = QStringLiteral("the filter did not acknowledge the admission within the bound");
      break;
    case NativePlaybackCommandKind::Drain:
      key = QStringLiteral("native.playback.drainTimeout");
      text = QStringLiteral("the filter did not acknowledge the drain within the bound");
      break;
    case NativePlaybackCommandKind::Off:
      key = QStringLiteral("native.playback.offTimeout");
      text = QStringLiteral("the filter did not acknowledge the drain before removal within the bound");
      break;
    case NativePlaybackCommandKind::Suspend:
      key = QStringLiteral("native.playback.suspendTimeout");
      text = QStringLiteral("the filter did not acknowledge suspension within the bound");
      break;
    case NativePlaybackCommandKind::Resume:
      key = QStringLiteral("native.playback.resumeTimeout");
      text = QStringLiteral("the filter did not acknowledge the resume within the bound");
      break;
    case NativePlaybackCommandKind::Attach:
      key = QStringLiteral("native.playback.attachTimeout");
      text = QStringLiteral("the attach did not complete within the bound");
      break;
  }

  const bool running = admission_ && admission_->confirmed;
  switch (kind) {
    case NativePlaybackCommandKind::Admit:
      if (!running) {
        status_.state = State::Failed;
        if (!teardown_) inferenceLease_.reset();
      }
      break;
    case NativePlaybackCommandKind::Resume:
      status_.state = State::Suspended;
      break;
    case NativePlaybackCommandKind::Suspend:
    case NativePlaybackCommandKind::Drain:
    case NativePlaybackCommandKind::Off:
    case NativePlaybackCommandKind::Attach:
      status_.state = running ? State::Active : State::Failed;
      break;
  }

  noteReason(key, text);
  Decision decision;
  decision.outcome = Outcome::TimedOut;
  decision.reasonKey = key;
  decision.reason = text;
  return decision;
}

NativePlaybackController::Decision NativePlaybackController::tick(qint64 nowMs) {
  if (pending_ && nowMs >= pending_->deadlineMs) return onDeadline(nowMs);
  return ignored();
}

}  // namespace native
}  // namespace mplayer
