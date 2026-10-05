// Whole-plan prepared admission: the one resolver that turns a resolved
// preparation plan and the owned models it was resolved over into the compact
// admission command the native filter binds, holding a lease on every prepared
// artifact it names.
//
// The request is observed facts only. Nothing here invents a device, a digest,
// a shape, an order or a default: the plan is the authority for the chain, the
// owned model bytes are re-measured, and the artifact cache is re-proved against
// the observed build identity before any lease is taken. A refusal names the
// first fact that moved and leaves the caller's store, model tree and output
// exactly as it found them - no cache mutation and no source copy happens here.
//
// Prepared/Ready/Active are deliberately absent: successful resolution proves an
// ordered plan can be admitted, not that any engine loaded. The caller observes
// acknowledgement elsewhere, from the producer's own snapshots.

#pragma once

#include <optional>

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVector>

#include "native/prepare/NativeArtifactStore.hpp"
#include "native/models/NativeCatalog.hpp"
#include "native/bundle/NativeManifest.hpp"
#include "native/prepare/NativePreparationPlan.hpp"

namespace mplayer {
namespace native {

/// One admission resolution's whole input.
///
/// `plan` is the resolved ordered chain; `models` carries one owned row per
/// stage in the same order; `build` is the measured machine identity the plan's
/// stages are compiled for; `store` is the prepared-artifact cache the plan's
/// stages are looked up in. `modelRoot` is the owned models root the rows' copies
/// must live inside. `source` and `device` are the observed facts the plan was
/// resolved against; `recipeId`/`backend` are the requested selection. `requestId`,
/// `revision` and `epoch` are carried verbatim into the command so a superseded
/// reply can be discarded by its caller.
struct PreparedAdmissionRequest {
  PreparationPlan plan;
  QVector<ModelRecord> models;
  EngineBuildIdentity build;
  ArtifactStore store;
  QString modelRoot;
  SourceGeometry source;
  DeviceIdentity device;
  QString recipeId;
  Backend backend = Backend::TensorRT;
  QString requestId;
  quint64 revision = 0;
  quint64 epoch = 0;
};

/// One admitted plan: the admission command, the proved records and a held lease
/// per artifact. Every lease pins its owned root and build key until this value
/// is destroyed, so a plan in flight cannot be removed out from under it.
struct PreparedAdmission {
  QJsonObject command;
  QVector<ArtifactRecord> artifacts;
  QVector<ArtifactLease> leases;
};

/// Resolves the whole plan. Verifies the plan, source, device, build identity and
/// owned model bytes, resolves each stage's canonical `BuildRequest`, re-proves
/// its artifact against that request and the observed runtime, and acquires every
/// artifact lease. Any partial failure drops all leases taken so far and returns
/// nothing, so a refused admission never leaves a claim behind. `reasonKey` and
/// `reason` are written only on refusal; both are cleared on success.
std::optional<PreparedAdmission> resolvePreparedAdmission(const PreparedAdmissionRequest& request,
                                                          QString* reasonKey = nullptr,
                                                          QString* reason = nullptr);

/// "mikai.native.admission/1". The command schema this build writes.
QString preparedAdmissionSchema();

/// The protocol the admission command carries. Distinct from the ABI2 snapshot
/// protocol: this is the mpv command payload.
inline constexpr int kPreparedAdmissionProtocol = 1;

}  // namespace native
}  // namespace mplayer
