// The owned-store implementation of M13's `DynamicProfileResolver`.
//
// Every answer here is traceable to the immutable copy under the app-owned store
// root: its sha256, what the ORT C API says it declares, and - for a dynamic graph
// - the shape a genuine CPU run produces. Nothing is read from the record's file
// name, its `onnxPath`, or a default factor, and a model the store does not hold
// is refused by name instead of being resolved from somewhere else.
//
// The per-model cache holds the bytes a copy was read as. The copy is re-proved
// on disk before every answer, so a cache entry can never outlive a change to the
// file it came from: a mutated copy is refused rather than serving its stale
// geometry. The original import path is provenance only and is never read.

#include "NativeInspectedPlan.hpp"

#include <QtCore/QFile>
#include <QtCore/QHash>

namespace mplayer {
namespace native {

namespace {

void refuse(QString* reasonKey, QString* reason, const QString& key, const QString& text) {
  if (reasonKey) *reasonKey = key;
  if (reason) *reason = text;
}

/// ModelProfile has no operator== of its own; the two tensors it is made of do.
bool profilesMatch(const ModelProfile& left, const ModelProfile& right) {
  return left.input == right.input && left.output == right.output;
}

}  // namespace

struct InspectedPlanResolver::State {
  ModelInventory inventory;
  InspectionRuntime runtime;

  bool loaded = false;
  QString loadKey;
  QString loadReason;
  QVector<StoredModel> rows;

  /// The immutable copies that have already been read, hashed and declared,
  /// keyed by the row's identity so two same-named files are two entries rather
  /// than one shared answer.
  QHash<QString, QByteArray> verified;

  QString lastKey;
  QString lastReason;

  State(QString root, InspectionRuntime rt)
      : inventory(std::move(root)), runtime(std::move(rt)) {}
};

InspectedPlanResolver::InspectedPlanResolver(QString storeRoot, InspectionRuntime runtime)
    : state_(std::make_shared<State>(std::move(storeRoot), std::move(runtime))) {}

bool InspectedPlanResolver::load(State& state, QString* reasonKey, QString* reason) {
  state.loaded = true;
  state.rows.clear();
  state.verified.clear();
  state.loadKey.clear();
  state.loadReason.clear();

  const auto fail = [&state, reasonKey, reason](const QString& key, const QString& text) {
    state.loadKey = key;
    state.loadReason = text;
    refuse(reasonKey, reason, key, text);
    return false;
  };

  // Every answer is an inspected one, so an absent runtime is the answer for the
  // whole store rather than a per-model surprise.
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(state.runtime, &runtimeReason)) {
    return fail(QStringLiteral("inspect.runtime.unavailable"), runtimeReason);
  }
  QString ownedReason;
  if (!state.inventory.isOwned(&ownedReason)) {
    return fail(QStringLiteral("model.store.unowned"),
                QStringLiteral("model store root %1 is not app-owned: %2")
                    .arg(state.inventory.root(), ownedReason));
  }
  QString listReason;
  const QVector<StoredModel> rows = state.inventory.stored(&listReason);
  if (!listReason.isEmpty()) {
    return fail(QStringLiteral("model.store.unreadable"), listReason);
  }
  state.rows = rows;
  refuse(reasonKey, reason, QString(), QString());
  return true;
}

bool InspectedPlanResolver::refresh(QString* reasonKey, QString* reason) const {
  return load(*state_, reasonKey, reason);
}

CatalogView InspectedPlanResolver::catalog() const {
  if (!state_->loaded) refresh(nullptr, nullptr);
  CatalogView view;
  view.models.reserve(state_->rows.size());
  for (const StoredModel& row : state_->rows) view.models.append(row.record);
  return view;
}

QVector<StoredModel> InspectedPlanResolver::rows() const {
  if (!state_->loaded) refresh(nullptr, nullptr);
  return state_->rows;
}

DynamicProfileResolver InspectedPlanResolver::resolver() const {
  std::shared_ptr<State> state = state_;
  return [state](const ModelRecord& record, const QSize& stageInput) {
    return resolveInto(*state, record, stageInput, nullptr, nullptr);
  };
}

QString InspectedPlanResolver::lastReasonKey() const { return state_->lastKey; }

QString InspectedPlanResolver::lastReason() const { return state_->lastReason; }

std::optional<ResolvedProfile> InspectedPlanResolver::resolve(const ModelRecord& record,
                                                              const QSize& stageInput,
                                                              QString* reasonKey, QString* reason) {
  std::optional<ResolvedProfile> resolved =
      resolveInto(*state_, record, stageInput, reasonKey, reason);
  if (reasonKey) *reasonKey = state_->lastKey;
  if (reason) *reason = state_->lastReason;
  return resolved;
}

std::optional<ResolvedProfile> InspectedPlanResolver::resolveInto(State& state,
                                                                  const ModelRecord& record,
                                                                  const QSize& stageInput,
                                                                  QString* reasonKey,
                                                                  QString* reason) {
  const auto fail = [&state, reasonKey, reason](const QString& key,
                                                const QString& text) -> std::optional<ResolvedProfile> {
    state.lastKey = key;
    state.lastReason = text;
    refuse(reasonKey, reason, key, text);
    return std::nullopt;
  };
  state.lastKey.clear();
  state.lastReason.clear();

  if (!state.loaded && !load(state, nullptr, nullptr)) {
    return fail(state.loadKey, state.loadReason);
  }
  if (state.loaded && !state.loadKey.isEmpty()) {
    return fail(state.loadKey, state.loadReason);
  }

  // The row is found by identity, never by display name: two files sharing a
  // basename are two rows, and the name decides nothing.
  const StoredModel* owned = nullptr;
  for (const StoredModel& row : state.rows) {
    if (row.record.id == record.id) {
      owned = &row;
      break;
    }
  }
  if (owned == nullptr) {
    return fail(QStringLiteral("model.owned.missing"),
                QStringLiteral("model %1 is not in the owned store, so there are no inspected bytes "
                               "to resolve")
                    .arg(record.id));
  }
  // The catalogue's claim and the owned row must be the same model. A stale or
  // forged record is refused rather than resolved against whatever the store has.
  if (owned->record.onnxSha256 != record.onnxSha256 ||
      !profilesMatch(owned->record.actualProfile, record.actualProfile)) {
    return fail(QStringLiteral("model.owned.identity"),
                QStringLiteral("model %1 is described as %2 bytes hashing %3 declaring %4->%5, but "
                               "the owned row is %6 bytes hashing %7 declaring %8->%9")
                    .arg(record.id)
                    .arg(record.onnxBytes)
                    .arg(record.onnxSha256)
                    .arg(record.actualProfile.input.describe(), record.actualProfile.output.describe())
                    .arg(owned->record.onnxBytes)
                    .arg(owned->record.onnxSha256)
                    .arg(owned->record.actualProfile.input.describe(),
                         owned->record.actualProfile.output.describe()));
  }
  if (!owned->record.availability.isUsable()) {
    return fail(owned->record.availability.reasonKey.isEmpty()
                    ? QStringLiteral("model.owned.unavailable")
                    : owned->record.availability.reasonKey,
                owned->record.availability.reason.isEmpty()
                    ? QStringLiteral("the owned row for %1 is not usable").arg(record.id)
                    : owned->record.availability.reason);
  }
  if (owned->storedPath.isEmpty()) {
    return fail(QStringLiteral("model.owned.missing"),
                QStringLiteral("the owned row for %1 names no stored copy to resolve").arg(record.id));
  }

  const QString cacheKey = owned->record.id;
  QHash<QString, QByteArray>::const_iterator cached = state.verified.constFind(cacheKey);
  if (cached == state.verified.constEnd()) {
    // Read the immutable copy once. Every stage that names this model reuses the
    // same bytes rather than reading and hashing the file again.
    const QString storedPath = owned->storedPath;
    QFile file(storedPath);
    if (!file.open(QIODevice::ReadOnly)) {
      return fail(QStringLiteral("model.owned.unreadable"),
                  QStringLiteral("the owned copy %1 cannot be read: %2")
                      .arg(storedPath, file.errorString()));
    }
    QByteArray measured = file.readAll();
    if (file.error() != QFile::NoError) {
      return fail(QStringLiteral("model.owned.unreadable"),
                  QStringLiteral("the owned copy %1 could not be read: %2")
                      .arg(storedPath, file.errorString()));
    }
    file.close();

    const QString digest = sha256HexOf(measured);
    if (digest != owned->record.onnxSha256) {
      return fail(QStringLiteral("model.owned.digest"),
                  QStringLiteral("the owned copy %1 hashes to %2, but the row's identity is %3")
                      .arg(storedPath, digest, owned->record.onnxSha256));
    }

    // The graph's own declaration is the authority. A row whose manifest says a
    // boundary the bytes do not declare is refused, not trusted.
    const InspectionResult declared = inspectDeclaredProfile(state.runtime, measured);
    if (!declared.ok) {
      return fail(declared.reasonKey.isEmpty() ? QStringLiteral("model.owned.unreadable")
                                               : declared.reasonKey,
                  declared.reason);
    }
    if (!profilesMatch(declared.declared, record.actualProfile)) {
      return fail(QStringLiteral("model.owned.profile"),
                  QStringLiteral("the owned copy of %1 declares %2->%3, but the row describes "
                                 "%4->%5")
                      .arg(record.id)
                      .arg(declared.declared.input.describe(), declared.declared.output.describe())
                      .arg(record.actualProfile.input.describe(),
                           record.actualProfile.output.describe()));
    }

    cached = state.verified.insert(cacheKey, measured);
  }

  // The cache holds the bytes the copy was read as; re-prove the copy on disk
  // before answering so a file mutated after it was cached cannot keep resolving
  // its old geometry.
  QString ownedKey;
  QString ownedReason;
  if (!state.inventory.verifyOwnedModelIntegrity(*owned, &ownedKey, &ownedReason)) {
    return fail(ownedKey.isEmpty() ? QStringLiteral("model.owned.changed") : ownedKey, ownedReason);
  }

  QString resolveKey;
  QString resolveReason;
  const std::optional<StageResolution> resolution =
      resolveStageGeometry(record.actualProfile, stageInput, cached.value(), state.runtime,
                           &resolveKey, &resolveReason);
  if (!resolution.has_value()) {
    return fail(resolveKey.isEmpty() ? QStringLiteral("model.owned.unresolved") : resolveKey,
                resolveReason.isEmpty()
                    ? QStringLiteral("the owned graph declared no resolvable geometry at %1x%2")
                          .arg(stageInput.width())
                          .arg(stageInput.height())
                    : resolveReason);
  }
  // The answer must be for the geometry this stage actually runs at. The planner
  // checks this again; refusing here is what keeps the adapter from being the one
  // place a mismatched answer could slip through.
  if (resolution->input != stageInput) {
    return fail(QStringLiteral("model.owned.input"),
                QStringLiteral("the owned graph %1 resolved a %2x%3 input, but this stage runs at "
                               "%4x%5")
                    .arg(record.id)
                    .arg(resolution->input.width())
                    .arg(resolution->input.height())
                    .arg(stageInput.width())
                    .arg(stageInput.height()));
  }

  // A boundary the declared row cannot vouch for - an unresolved geometry, or a
  // symbolic output batch - is usable only with the concrete boundary a real CPU
  // run produced at this stage, resolved fresh from the owned bytes and never
  // from the import-time sample. That proof must be the same fact as the declared
  // row. A fixed, strictly eligible graph is answered from its own declaration
  // and carries no proof.
  const bool strictEligible = spatialEligibilityRefusal(record.actualProfile).isEmpty();
  if (record.actualProfile.isDynamicGeometry() || !strictEligible) {
    if (!resolution->fromCpuReference || !resolution->verifiedReferenceProfile.has_value()) {
      return fail(QStringLiteral("model.owned.reference"),
                  QStringLiteral("the owned graph %1 resolved without the concrete CPU reference "
                                 "boundary its declaration requires")
                      .arg(record.id));
    }
  }
  if (resolution->verifiedReferenceProfile.has_value()) {
    const ModelProfile& proof = *resolution->verifiedReferenceProfile;
    const QString proofRefusal = spatialReferenceRefusal(record.actualProfile, proof);
    if (!proofRefusal.isEmpty()) {
      return fail(QStringLiteral("model.owned.reference"), proofRefusal);
    }
    if (!proof.input.hasGeometry() || !proof.output.hasGeometry() ||
        QSize(static_cast<int>(*proof.input.width), static_cast<int>(*proof.input.height)) !=
            resolution->input ||
        QSize(static_cast<int>(*proof.output.width), static_cast<int>(*proof.output.height)) !=
            resolution->output ||
        !proof.derivedUniformScale().has_value() ||
        *proof.derivedUniformScale() != resolution->scale) {
      return fail(QStringLiteral("model.owned.reference"),
                  QStringLiteral("the owned graph %1 resolved a boundary its verified CPU "
                                 "reference does not describe")
                      .arg(record.id));
    }
  }

  ResolvedProfile profile;
  profile.input = resolution->input;
  profile.output = resolution->output;
  profile.inputChannels = static_cast<int>(record.actualProfile.input.channels);
  profile.outputChannels = static_cast<int>(record.actualProfile.output.channels);
  if (profile.inputChannels <= 0 || profile.outputChannels <= 0 ||
      isRefusableGeometry(profile.output)) {
    return fail(QStringLiteral("model.owned.geometry"),
                QStringLiteral("the owned graph %1 resolved to %2x%3 at a %4x%5 input, which is not "
                               "a usable tensor shape")
                    .arg(record.id)
                    .arg(profile.output.width())
                    .arg(profile.output.height())
                    .arg(stageInput.width())
                    .arg(stageInput.height()));
  }
  return profile;
}

}  // namespace native
}  // namespace mplayer
