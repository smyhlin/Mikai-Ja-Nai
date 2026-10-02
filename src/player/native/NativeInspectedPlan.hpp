// The actual inspected-profile adapter for M13: the one resolver that answers a
// dynamic stage's geometry from bytes this app owns.
//
// M13's planner takes a `DynamicProfileResolver` and never guesses - a resolver
// that answers from a file name has been wired wrongly. This is the real one. It
// reads the immutable copies under the app-owned `native/models` store, the bytes
// the inspector actually inspected, verifies each one against the identity it was
// published under, and resolves the stage through the same ONNX Runtime C API that
// read the declaration. A file whose name happens to contain `dyn-HW` gets no
// special treatment, and a scale is never defaulted to 1 or 2.
//
// Each answer is resolved fresh from the owned bytes: the import-time sample is
// never reused as a stage's geometry. A boundary the declaration cannot vouch for
// - an unresolved H/W, or a symbolic output batch - is answered only with the
// concrete N=1/C=3 boundary a fresh CPU run proves, and that proof must agree
// with the declared row. A fixed, strictly eligible graph is answered from its
// own declaration.
//
// Everything here is a refusal or an observed fact. Six refusals are named
// rather than papered over:
//
//   * `model.owned.missing`  - the catalogue names a model the owned store does
//                              not hold, or a row names no owned file.
//   * `model.owned.identity` - the owned row's digest and declared profile are not
//                              the model the plan asked to resolve.
//   * `model.owned.profile`  - the owned bytes do not declare the boundary the
//                              row claims, or the request describes another one.
//   * `model.owned.digest`   - the owned copy no longer hashes to its identity.
//   * `model.owned.changed`  - the owned copy on disk no longer matches the bytes
//                              the row was published under.
//   * `model.owned.reference`  - a boundary that needs a CPU reference run
//                              resolved without one, or the run's boundary
//                              disagrees with the declared row.
//
// The owned copy is re-proved on disk before every answer, so a copy mutated
// after it was cached is refused (`model.owned.changed`) rather than resolving
// its stale geometry. The original import path is provenance only and is never
// read. An absent runtime, an unreadable graph and a CPU reference that did not
// produce a uniform result are forwarded with their own keys; nothing here turns
// any of them into a Ready stage.
//
// Nothing here scans a foreign folder and nothing here borrows a caller's store:
// the root is copied into the adapter, the rows are the app-owned store's own, and
// the bytes that are read are the ones inside that root. The immutable copies are
// read, hashed and declared-read once per model and reused by every stage that
// names that model, while each copy is re-proved on disk before every answer.
//
// This header carries no ONNX Runtime type and no ORT include; only the
// implementation translation unit does, so it belongs with the model-inspection
// target and a machine without the ORT package still configures everything else.

#pragma once

#include <memory>
#include <optional>

#include <QtCore/QByteArray>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QVector>

#include "NativeCatalog.hpp"
#include "NativeModelInspect.hpp"
#include "NativeModelInventory.hpp"
#include "NativePreparationPlan.hpp"

namespace mplayer {
namespace native {

/// Owns an app-owned model store and answers M13's dynamic-stage geometry from it.
///
/// The store root is copied, never borrowed: the object that built this adapter
/// may die and `resolver()` still answers, because the returned callable shares the
/// loaded rows and the per-model byte cache rather than a reference to this object.
class InspectedPlanResolver {
 public:
  /// `storeRoot` must be inside the app-owned `native/models` hierarchy; that is
  /// checked on `refresh`, not here, so constructing is free and reads nothing.
  InspectedPlanResolver(QString storeRoot, InspectionRuntime runtime);

  /// Loads and re-verifies the owned rows. False, with a named reason, when the
  /// runtime is not present, the root is not app-owned, or the store cannot be
  /// read. An absent row stays absent: an unowned or unreadable store is not
  /// "unknown but probably fine".
  bool refresh(QString* reasonKey = nullptr, QString* reason = nullptr) const;

  /// The catalogue of owned rows, exactly as the store vouches for them. Loads it
  /// on first use. Each row keeps the backend support the store recorded for it,
  /// so the planner's model-compatibility check sees what the row was published
  /// with; which backends a runtime carries is a separate fact the planner
  /// supplies itself.
  CatalogView catalog() const;

  /// The owned rows themselves (identity, source, owned copy, import resolution).
  /// Loads the catalogue on first use.
  QVector<StoredModel> rows() const;

  /// The callable M13 consumes. Self-contained and safe to copy.
  DynamicProfileResolver resolver() const;

  /// The resolver's implementation, exposed for the CLI and for tests. `reasonKey`
  /// and `reason` carry the named failure; both are cleared on success.
  std::optional<ResolvedProfile> resolve(const ModelRecord& record, const QSize& stageInput,
                                         QString* reasonKey = nullptr, QString* reason = nullptr);

  /// The named reason of the most recent refusal, both empty after a success.
  /// This is how the specific cause is forwarded beside the planner's own
  /// `stage.dynamic.unresolved` blocker, whose resolver channel carries no reason.
  QString lastReasonKey() const;
  QString lastReason() const;

 private:
  struct State;

  /// The one resolution path, so the resolver and `resolve` cannot disagree.
  static std::optional<ResolvedProfile> resolveInto(State& state, const ModelRecord& record,
                                                    const QSize& stageInput, QString* reasonKey,
                                                    QString* reason);
  /// The one load path.
  static bool load(State& state, QString* reasonKey, QString* reason);

  std::shared_ptr<State> state_;
};

}  // namespace native
}  // namespace mplayer
