// Bounded blocking preparation worker: complete recipe or nothing.
// A cancelled/failed run returns no Ready claim.
#pragma once
#include <atomic>
#include <functional>
#include <optional>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>
#include "native/prepare/NativeArtifactStore.hpp"
#include "native/bundle/NativeDiagnostics.hpp"
#include "native/bundle/NativeManifest.hpp"
#include "native/models/NativeModelInventory.hpp"
#include "native/prepare/NativePreparationPlan.hpp"
namespace mplayer {
namespace native {
// CAS gate: once publication begins a cancel is TooLate and cannot unwrite.
class NativeBuildCancellation {
 public:
  enum class State { Working, Cancelled, Publishing };
  enum class CancelResult { CancelledNow, AlreadyCancelled, TooLate, };
  CancelResult cancel() {
    State expected = State::Working;
    if (state_.compare_exchange_strong(expected, State::Cancelled)) {
      return CancelResult::CancelledNow;
    }
    return expected == State::Cancelled ? CancelResult::AlreadyCancelled
                                         : CancelResult::TooLate;
  }
  bool beginPublication() {
    State expected = State::Working;
    return state_.compare_exchange_strong(expected, State::Publishing);
  }
  State state() const { return state_.load(); }
  bool isCancelled() const { return state() == State::Cancelled; }
 private:
  std::atomic<State> state_{State::Working};
};
struct BuilderRawInput {
  QString name;
  QString dtype;
  QString shape;
  QString filePath;
};
struct BuilderStageReference {
  QString fixturePath;
  QVector<BuilderRawInput> rawInputs;
};
struct BuilderToolPaths {
  QString inspectExecutable;
  QString prepExecutable;
  QString verifierExecutable;
  QString ortRuntimeDir;
  // Provider runtime dirs observed from the bundle; child inherits only these.
  QString trtBinDir;
  QString cudaBinDir;
};
// Measured prep facts: every present flag is required; refused when absent.
struct BuilderPrepIdentity {
  quint64 builderFlags = 0;
  bool builderFlagsPresent = false;
  bool tf32Allowed = true;
  bool tf32AllowedPresent = false;
  QString engineCapability;
  bool engineCapabilityPresent = false;
  int builderOptimizationLevel = 0;
  bool builderOptimizationLevelPresent = false;
  quint64 tacticSources = 0;
  bool tacticSourcesPresent = false;
  quint64 networkFlags = 0;
  bool networkFlagsPresent = false;
  bool stronglyTyped = false;
  bool stronglyTypedPresent = false;
  int tensorRtLibVersion = 0;
  bool tensorRtLibVersionPresent = false;
  int cudaRuntimeVersion = 0;
  bool cudaRuntimeVersionPresent = false;
  int cudaDriverApiVersion = 0;
  bool cudaDriverApiVersionPresent = false;
  QString cudaDriverFileVersion;
  quint64 workspaceBytes = 0;
  bool workspaceBytesPresent = false;
  QString workspaceSource;
};
// Observed facts only; no synthesised percentages.
struct BuilderProgress {
  int stageIndex = -1;
  QString modelId;
  QString phase;
  QString detail;
  QString log;
};
using BuilderProgressCallback = std::function<void(const BuilderProgress&)>;
struct BuilderRequest {
  PreparationPlan plan;
  QVector<StoredModel> models;
  EngineBuildIdentity build;
  BuilderToolPaths tools;
  QVector<BuilderStageReference> references;
  ArtifactStore store;
  // Explicit owned models root; the stored copy is proved inside it.
  QString modelRoot;
  quint64 workspaceBytes = 0;
};
struct BuilderStageOutcome {
  BuildRequest request;
  ArtifactRecord record;
  bool cacheHit = false;
  std::optional<BuilderPrepIdentity> prepIdentity;
};
struct BuilderResult {
  bool ok = false;
  bool cancelled = false;
  QString reasonKey;
  QString reason;
  int stageIndex = -1;
  QString phase;
  QString toolError;
  QVector<BuilderStageOutcome> stages;
  QVector<QString> publishedBuildKeys;
  // Loud cleanup/rollback notes; a success may still carry warnings.
  QStringList warnings;
};
// Blocking on the calling worker thread; holds a Preparation lease across builds.
BuilderResult runNativeBuild(const BuilderRequest& request,
                             NativeBuildCancellation* cancellation,
                             const BuilderProgressCallback& progress);

/// True when `strides` is the contiguous row-major NCHW stride vector of the
/// rank-4 `shape`. A stride is an offset, not an extent: the outermost stride of
/// a contiguous 1x3x360x640 tensor is 691200, past any per-axis extent bound, so
/// it is held to the tensor's own size rather than to the shape's bound.
bool tensorStridesAreContiguous(const QJsonArray& strides, const QJsonArray& shape);

// The whole input of the hand-run builder probe: the observed facts a plan has
// to be resolved against, and the build inputs a real `runNativeBuild` is invoked
// with.
//
// This is not a plan the document invents. `request.plan`, `request.models` and
// `request.references` are filled by the probe after it resolves the plan through
// the real planner over the owned model store at `request.modelRoot` and the
// recipe at `recipePath`; everything below is a fact somebody observed or a path
// that already exists. Nothing is defaulted: a document that omits a device UUID,
// an SM, a CUDA/driver version, a compiler option, a workspace bound or a tool
// path refuses the whole request by name, because a build key assembled from
// plausible constants would name a machine the engine was never compiled for.
struct BuilderProbeRequest {
  BuilderRequest request;
  /// The ordered recipe resolved against the owned store. A real path.
  QString recipePath;
  /// The decoded source, exactly as the decoder reported it.
  SourceGeometry source;
  /// The observed decoder/compute device the plan and the build identity share.
  DeviceIdentity device;
  /// The backends the runtime actually carries; never assumed.
  BackendAvailability backends;
};

/// Parses one probe request document. Strict: an absent required member, a wrong
/// type, a member this build does not know, a path that is not absolute, or a
/// build identity any part of which is missing or placeholder-like refuses the
/// whole document with `reasonKey`/`reason`. `out` is written only on success, so
/// a caller that ignores the return value cannot act on a half-built request.
bool parseBuilderProbeRequest(const QJsonObject& object, BuilderProbeRequest* out,
                              QString* reasonKey = nullptr, QString* reason = nullptr);
}  // namespace native
}  // namespace mplayer
