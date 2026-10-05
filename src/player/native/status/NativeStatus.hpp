// Two strictly validated layers (the `vf-metadata/mikai_native` tag map and its
// `snapshot` document): any disagreement is a NAMED refusal. The decode device
// identity belongs to the probe protocol; the borrowed MpvHandle reader polls one
// request per 500 ms at most and MUST be detached before the borrowed core dies.
//
// Only the outer tag map is string-typed; inside `snapshot` every scalar keeps its
// real JSON type, so `revision`, `epoch` and the counter fields are numbers there.
// The writer emits them as 64-bit integers and the reader takes them back exactly,
// never through a double; an integer beyond +/-2^53 is refused by name rather than
// silently rounded into a different revision, epoch or count.

#pragma once

#include <functional>
#include <optional>

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVector>

#include "MpvHandle.hpp"
#include "native/status/NativeFilterCommand.hpp"
#include "native/prepare/NativePreparationPlan.hpp"
#include "native/status/NativeTypes.hpp"

namespace mplayer {
namespace native {

/// The ordinary mpv property the Qt side reads; it never resolves an `aji`
/// symbol. The filter's `GET_META` label itself is owned by
/// `NativeFilterCommand.hpp`, the one QtCore-only place the producer and this
/// reader can share it from, and is taken from there rather than repeated here.
inline constexpr const char* kNativeStatusProperty = "vf-metadata/mikai_native";

/// The one schema this build writes and reads; an unknown tag is a named refusal,
/// never guessed at.
inline constexpr const char* kNativeStatusSchema = "mikai.native.status/1";

/// Both layers must declare this protocol; a mismatch is refused with both
/// versions named, never half-working.
inline constexpr int kNativeStatusProtocol = 1;

/// The optional factory observation carries its own identity, kept distinct from
/// the status protocol above and from the producer's snapshot protocol/schema
/// and AJI ABI: the filter must state each exactly, and a conflation of the
/// status wire with the producer's ABI is refused by name rather than read as
/// availability.
inline constexpr int kNativeFactoryQueryProtocol = 1;
inline constexpr int kNativeFactorySnapshotProtocol = 2;
inline constexpr int kNativeFactorySnapshotSchema = 2;
inline constexpr int kNativeFactorySourceAbi = 8;

/// At most one request every 500 ms; an unanswered one is abandoned after this
/// so a lost reply cannot stop the poll forever.
inline constexpr qint64 kNativeStatusPollIntervalMs = 500;
inline constexpr qint64 kNativeStatusRequestTimeoutMs = 5000;

/// The bound that keeps the exact rational comparisons below inside 64-bit
/// arithmetic; anything above it is not a frame rate.
inline constexpr int kNativeStatusMaxRateComponent = 1000000;

/// The largest integer the snapshot document carries exactly. A revision, epoch or
/// count past it cannot be told apart from a rounded one, so it is refused by name
/// rather than silently accepted as a different number.
inline constexpr qint64 kNativeStatusMaxExactInteger = 9007199254740992;  // 2^53

QString nativeStatusProperty();

QString nativeStatusSchema();

/// `Off` is native processing off with a filter still attached; a core with no
/// `mikai_native` filter reads Unavailable. `Probe` is PROBE-ONLY PASSTHROUGH, never Active.
enum class NativeState {
  Off,
  Probe,
  Preparing,
  NeedsPreparation,
  Active,
  Suspended,
  Failed,
  Unsupported,
  Deactivated
};

QString stableName(NativeState state);
std::optional<NativeState> nativeStateFromName(const QString& name);
QVector<NativeState> allNativeStates();

/// Poll Active and Preparing: only a poll discovers the return from either, so
/// polling stops once the filter says neither.
///
/// This asks whether the producer advances the state ON ITS OWN, which is not
/// the same question a reader asks: mplayer::nativeSnapshotKeepsPolling also
/// keeps reading Probe, because that state completes only when the producer
/// adopts a decoded frame.
bool nativeStateNeedsPolling(NativeState state);

/// Both sides are positive; a zero side is a refusal, not something to clamp.
struct NativeSize {
  int w = 0;
  int h = 0;

  bool isValid() const { return w > 0 && h > 0; }
  bool operator==(const NativeSize& other) const { return w == other.w && h == other.h; }
  bool operator!=(const NativeSize& other) const { return !(*this == other); }
};

/// `factor` is the rational the filter publishes; `model` names the graph actually
/// running, never the one requested.
struct NativeInterpolation {
  Rational factor;
  QString model;

  bool operator==(const NativeInterpolation& other) const {
    return factor == other.factor && model == other.model;
  }
  bool operator!=(const NativeInterpolation& other) const { return !(*this == other); }
};

/// What the filter reports it is running. `provider` is a string, not the requested
/// `Backend`, so a requested-but-not-applied choice stays visible as such.
struct NativeEffectivePlan {
  QString recipe;
  QString provider;
  QString model;
  int spatialStages = 0;
  /// Absent for a spatial-only chain; when present, `outputFps` equals `sourceFps`
  /// scaled by this factor.
  std::optional<NativeInterpolation> interpolation;
  /// The filter's own order label; never inferred from which stages exist.
  QString order;
  NativeSize sourceSize;
  NativeSize outputSize;
  /// An absent rate stays absent through validation and serialisation, never a
  /// synthesised 1/1 or 0/1.
  std::optional<Rational> sourceFps;
  std::optional<Rational> outputFps;
  QString decoder;
  QString csp;

  bool isSpatialOnly() const { return !interpolation.has_value(); }

  bool operator==(const NativeEffectivePlan& other) const {
    return recipe == other.recipe && provider == other.provider && model == other.model &&
           spatialStages == other.spatialStages && interpolation == other.interpolation &&
           order == other.order && sourceSize == other.sourceSize &&
           outputSize == other.outputSize && sourceFps == other.sourceFps &&
           outputFps == other.outputFps && decoder == other.decoder && csp == other.csp;
  }
  bool operator!=(const NativeEffectivePlan& other) const { return !(*this == other); }
};

/// The filter's own cadence accounting; a spatial-only chain keeps framesOut ==
/// framesIn and interpolated == 0. `interpolated` counts frames synthesized at the
/// output pin, not the net cadence gain, so it is bounded by the factor's
/// per-source synthesis rate and by the frames actually emitted.
struct NativeCadenceCounters {
  qint64 framesIn = 0;
  qint64 framesOut = 0;
  qint64 dropped = 0;
  qint64 interpolated = 0;
  qint64 sceneCuts = 0;

  bool operator==(const NativeCadenceCounters& other) const {
    return framesIn == other.framesIn && framesOut == other.framesOut &&
           dropped == other.dropped && interpolated == other.interpolated &&
           sceneCuts == other.sceneCuts;
  }
  bool operator!=(const NativeCadenceCounters& other) const { return !(*this == other); }
};

/// Observed facts, never requested ones. Each member is absent when nothing
/// was observed; unknown fields stay absent, never a placeholder.
struct NativeProbePayload {
  std::optional<SourceGeometry> source;
  std::optional<DeviceIdentity> device;

  bool operator==(const NativeProbePayload& other) const;
  bool operator!=(const NativeProbePayload& other) const { return !(*this == other); }
};

bool sourceGeometryEqual(const SourceGeometry& a, const SourceGeometry& b);
bool deviceIdentityEqual(const DeviceIdentity& a, const DeviceIdentity& b);

/// The OPTIONAL factory observation the filter publishes for the source it is
/// actually attached to. It is an availability fact only, never Prepared, Active
/// or artifact-ready: `observed` is true only when the filter executed the
/// producer's own query against the adopted context, and `spatial`/`rife` are
/// true only for a successful answer whose compiled roles name them. An
/// unavailable or unobserved factory always carries a non-empty `reasonKey`, and
/// such an observation can never claim a supported role.
struct NativeFactoryObservation {
  bool observed = false;
  /// The backend actually checked, or empty when nothing was checked.
  QString backend;
  bool spatial = false;
  bool rife = false;
  /// The runtime the producer actually initialised and checked.
  QString runtimeVersion;
  /// The adopted device UUID the factory was checked against, in the ABI's hex
  /// form; it belongs to the same source/epoch as the snapshot carrying it.
  QString deviceIdentity;
  /// 0 is unknown; there is no default compute capability.
  int smMajor = 0;
  int smMinor = 0;
  /// Non-empty exactly when the factory is not usable, naming why in a stable key.
  QString reasonKey;
  QString reason;

  /// True only for an executed query that answered success with at least one
  /// compiled role and no refusal.
  bool isUsable() const { return observed && (spatial || rife) && reasonKey.isEmpty(); }

  bool operator==(const NativeFactoryObservation& other) const {
    return observed == other.observed && backend == other.backend && spatial == other.spatial &&
           rife == other.rife && runtimeVersion == other.runtimeVersion &&
           deviceIdentity == other.deviceIdentity && smMajor == other.smMajor &&
           smMinor == other.smMinor && reasonKey == other.reasonKey && reason == other.reason;
  }
  bool operator!=(const NativeFactoryObservation& other) const { return !(*this == other); }
};

/// `requested` uses the preference contract's vocabulary and `effective` is the
/// filter's own report; every optional member is absent when omitted, never synthesised.
struct StatusSnapshot {
  int version = 0;
  qint64 revision = 0;
  NativeState state = NativeState::Off;
  std::optional<qint64> epoch;
  std::optional<SelectionState> requested;
  std::optional<NativeEffectivePlan> effective;
  std::optional<NativeCadenceCounters> counters;
  /// Observed source/device; absent when nothing observed.
  std::optional<NativeProbePayload> probe;
  /// The optional producer factory observation; absent when the producer
  /// published none. It never influences the state above.
  std::optional<NativeFactoryObservation> factory;
  /// Non-empty exactly when the state is not Active, empty for Active.
  QString reason;

  bool operator==(const StatusSnapshot& other) const {
    return version == other.version && revision == other.revision && state == other.state &&
           epoch == other.epoch && requested == other.requested &&
           effective == other.effective && counters == other.counters && probe == other.probe &&
           factory == other.factory && reason == other.reason;
  }
  bool operator!=(const StatusSnapshot& other) const { return !(*this == other); }
};

/// `revision` is the filter's monotonic change counter: unchanged is not a new
/// status, and a backward one is stale - unless a new epoch restarted the count.
enum class StatusRevision { Advanced, Unchanged, Stale };

StatusRevision classifyStatusRevision(const StatusSnapshot& previous,
                                      const StatusSnapshot& next);

/// Shape and self-consistency rules; returns false naming the first disagreement
/// (the caller's `reasonKey`/`reason` may be null).
bool statusSnapshotIsConsistent(const StatusSnapshot& snapshot, QString* reasonKey = nullptr,
                                QString* reason = nullptr);

/// The availability a parsed snapshot earns from its own state: `Ready` only for
/// Active, `Preparing` for Preparing/NeedsPreparation, `Unavailable` for every
/// other state - each carrying the snapshot's own reason. A state that names a
/// reason is by definition not running, so it is never Ready.
AvailabilityState statusSnapshotAvailability(const StatusSnapshot& snapshot);

/// True when a refusal the reader published cannot be repaired by reading again:
/// no filter is attached, the core would not take the request, or the filter's
/// document has drifted from this build's schema, protocol, tag vocabulary or
/// declared revision (including a value that must never be echoed). A caller
/// polling on a bounded timer uses this to stop after one read instead of
/// repeating the same refusal for the life of the source; a request-state change
/// or a new source is what re-arms it.
///
/// A refusal about one value inside the snapshot document (an unknown state, a
/// cadence that does not add up) is deliberately NOT one of these: those
/// describe a document the filter can still re-publish correctly.
bool statusRefusalHaltsPolling(const QString& reasonKey);

/// All-or-nothing: `snapshot` is written only on success, so ignoring the return
/// value cannot act on a half-parsed document.
bool parseStatusSnapshot(const QJsonObject& object, StatusSnapshot* snapshot,
                         QString* errorKey = nullptr, QString* error = nullptr);
bool parseStatusSnapshot(const QByteArray& payload, StatusSnapshot* snapshot,
                         QString* errorKey = nullptr, QString* error = nullptr);

/// Parses the tag map and the `snapshot` document; both layers must state the same
/// schema, protocol and revision, and any malformed or unknown tag is named.
bool parseStatusTags(const NodeValue& tags, StatusSnapshot* snapshot,
                     QString* errorKey = nullptr, QString* error = nullptr);

/// The raw document writer: writes exactly the members present, never substituting
/// a default for an absent one, and keeps every string verbatim. A hostile filter's
/// URL must reach `parseStatusSnapshot` unchanged so it is refused by name rather
/// than quietly redacted into an accepted document, so this output is not safe to
/// log or export.
QJsonObject statusSnapshotToJson(const StatusSnapshot& snapshot);
/// The export/writer boundary: the same document with every free-text member
/// redacted first, so no credential leaves in the clear.
QByteArray serializeStatusSnapshot(const StatusSnapshot& snapshot);

/// The tag map as an app-allocated node (freed with the app's allocator, never
/// `mpv_free_node_contents`); lets a caller build and read back both layers.
NodeValue statusTags(const StatusSnapshot& snapshot);

/// What the HUD reads: whether the surface could be read at all (Unavailable when
/// absent or malformed) plus the last accepted snapshot, whose `state` is the filter's.
struct NativeStatusView {
  AvailabilityState availability;
  std::optional<StatusSnapshot> snapshot;

  bool operator==(const NativeStatusView& other) const {
    return availability.value == other.availability.value &&
           availability.reasonKey == other.availability.reasonKey &&
           availability.reason == other.availability.reason && snapshot == other.snapshot;
  }
  bool operator!=(const NativeStatusView& other) const { return !(*this == other); }
};

/// One reader per core, bound to a borrowed handle; the caller drives `tick()` and
/// owns the cadence, and the change callback never fires re-entrantly or from repaint.
class NativeStatusConsumer {
 public:
  NativeStatusConsumer();
  ~NativeStatusConsumer();

  NativeStatusConsumer(const NativeStatusConsumer&) = delete;
  NativeStatusConsumer& operator=(const NativeStatusConsumer&) = delete;

  /// Binds the handle to poll (never owned; null is `detach()`). Rebinding resets the
  /// view: another core's snapshot says nothing about this one.
  void attach(MpvHandle* handle);
  /// Unbinds and cancels an outstanding request; safe twice and on a null binding,
  /// and MUST run before the borrowed core is destroyed.
  void detach();
  bool attached() const { return handle_ != nullptr; }

  /// One pump step. Issues at most one request per `kNativeStatusPollIntervalMs`,
  /// never while one is outstanding; superseded-epoch or changed-binding replies drop.
  void tick(qint64 nowMs, quint64 currentEpoch, bool pollActivePreparingSwitching);

  /// The last published view. Unavailable until the first read and whenever the filter
  /// is refused; the snapshot clears then, because a stale plan read as current is worse.
  const NativeStatusView& status() const { return published_; }
  const AvailabilityState& availability() const { return published_.availability; }
  const std::optional<StatusSnapshot>& snapshot() const { return published_.snapshot; }

  void setChangeCallback(std::function<void(const NativeStatusView&)> callback);

  /// Diagnostics for the polling proof.
  quint64 requestCount() const { return requests_; }
  quint64 acceptedCount() const { return accepted_; }
  quint64 droppedCount() const { return dropped_; }

 private:
  /// Publishes and notifies only on an actual change.
  void publish(NativeStatusView next);
  void publishUnavailable(const QString& reasonKey, const QString& reason);
  void accept(const AsyncReply& reply);

  MpvHandle* handle_ = nullptr;
  /// Bumped on attach/detach so an in-flight reply for a previous binding is dropped.
  quint64 token_ = 0;
  quint64 currentEpoch_ = 0;
  bool outstanding_ = false;
  bool requested_ = false;
  qint64 lastRequestMs_ = 0;
  AsyncRequestId pending_;
  NativeStatusView published_;
  std::function<void(const NativeStatusView&)> callback_;
  quint64 requests_ = 0;
  quint64 accepted_ = 0;
  quint64 dropped_ = 0;
};

}  // namespace native
}  // namespace mplayer
