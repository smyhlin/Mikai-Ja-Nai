// The typed recipe: an ordered spatial chain, an optional temporal stage, and the
// backend it asks for - plus the versioned document that carries it.
//
// A recipe names MODELS ONLY. It carries no prepared-artifact or engine identity:
// which build a stage resolves to is a later join against the prepared-artifact
// set on the M14 build key, with `engineSha256` as the separate post-serialization
// integrity value. An artifact id here is what would let a stale engine look
// prepared, so the document refuses one as an unknown member.
//
// "RIFE-only" and "spatial configured to zero stages" are different things and
// only the first is legal. The second is the failure where the filter's
// passthrough default turns the feature into a silent no-op while the UI still
// shows it enabled, so it is rejected by name instead of reaching a core as a
// configure with nothing to do.

#pragma once

#include <optional>

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVector>

#include "NativeCatalog.hpp"
#include "NativeTypes.hpp"

namespace mplayer {
namespace native {

/// One model in the spatial chain, in execution order. `preResize` is the
/// per-model work size before the upscale (the reference `resizefactor` /
/// `resizeheight` pair): a target size, not a factor, and an invalid one is a
/// refusal rather than something to clamp.
struct SpatialStage {
  QString modelId;
  QSize preResize;
  bool preResizeEnabled = false;
};

/// The interpolation stage. Revision, variant and ensemble all appear in the
/// model name, so a stage can never read back as "some RIFE" and resolve to
/// whatever happens to be installed.
struct TemporalStage {
  RifeVersion version = RifeVersion::V4_14;
  RifeVariant variant = RifeVariant::Full;
  bool ensemble = false;

  Rational factor{2, 1};
  TemporalOrder order = TemporalOrder::AfterSpatial;
  /// Above this normalized scene difference, a frame pair is treated as a cut.
  double sceneThreshold = 0.150;

  QString modelId() const { return rifeModelName(version, variant, ensemble); }
};

enum class RecipeShape { SpatialOnly, RifeOnly, Combined, Empty };

struct Recipe {
  /// The stable preference key. Never an ordinal, so a recipe added later cannot
  /// repoint a selection already made.
  QString id;
  QString label;

  /// Execution order. Load-bearing and preserved exactly: a chain is not a set.
  QVector<SpatialStage> spatial;

  std::optional<TemporalStage> temporal;

  Backend backend = Backend::TensorRT;

  RecipeShape shape() const;
  bool isValidShape() const { return shape() != RecipeShape::Empty; }
  QString shapeName() const;
};

/// `ok` is the only success signal; there is no partial pass.
struct RecipeVerdict {
  bool ok = true;
  QString reasonKey;
  QString reason;

  static RecipeVerdict accepted() { return RecipeVerdict{}; }
  static RecipeVerdict rejected(const QString& reasonKey, const QString& reason);
};

/// All-or-nothing: readiness is the entire recipe, or nothing. Nothing here
/// substitutes another chain, model, variant or backend for what was asked for.
struct RecipeEligibility {
  AvailabilityState availability;
  SelectionState requested;
  SelectionState effective;
  /// Per-stage reasons in recipe order, so a UI can point at the offending stage
  /// rather than at the recipe as a whole.
  QVector<QString> stageReasons;
};

RecipeVerdict validateRecipe(const Recipe& recipe);
RecipeVerdict validateSpatialStage(const SpatialStage& stage);
RecipeVerdict validateTemporalStage(const TemporalStage& stage);

/// `effective` is the requested selection only when availability is Ready, and the
/// untouched Original pipeline otherwise.
///
/// Every row the recipe names must be usable *now*: a model that the catalogue
/// carries but its provisioner marked Preparing or Unavailable is refused with
/// that row's own reason, never promoted and never substituted.
RecipeEligibility evaluateEligibility(const Recipe& recipe, const CatalogView& catalog,
                                      const BackendAvailability& backends);

/// A reader that does not know this exact tag rejects the document rather than
/// guessing at its fields.
QString recipeSchema();

QJsonObject recipeToJson(const Recipe& recipe);
QByteArray serializeRecipe(const Recipe& recipe);

/// Rejects an unknown schema, an unknown enum string, a missing or wrongly typed
/// member, a stage that is not an object, an out-of-range geometry, a temporal
/// member with no explicit order, and any key this schema does not define.
///
/// Returns true on success. On failure `recipe` is left default-constructed and
/// `errorKey`/`error` carry a stable key and the sentence. `recipe` must not be
/// null; `errorKey` and `error` may be.
bool parseRecipe(const QJsonObject& object, Recipe* recipe, QString* errorKey, QString* error);
bool parseRecipe(const QByteArray& payload, Recipe* recipe, QString* errorKey, QString* error);

}  // namespace native
}  // namespace mplayer