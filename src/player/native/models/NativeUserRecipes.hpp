// The user recipe store: where a typed recipe - an ordered spatial chain, an
// optional interpolation stage, and the backend it asks for - is persisted under
// a name derived from its own bytes, and how it is read back.
//
// Identity is the recipe's body, never its file name and never an ordinal. A
// stored recipe's id and its document name both encode a SHA-256 fingerprint of
// everything that decides what runs: the ordered spatial stages with their
// models and pre-resize targets, the temporal stage with its revision, variant,
// ensemble, factor, order and scene threshold, and the backend. The display
// label and the id are the only members left out of that fingerprint, so
// renaming a recipe never changes which document it is, and two recipes that
// differ in anything that runs can never collide.
//
// The store owns exactly one directory beneath `native/` (`native/recipes`) and
// writes one small JSON document per recipe through `QSaveFile`, so a document
// is either the old one or the new one and never a half-written file. A
// document that does not parse, does not validate, or whose id, backend or body
// fingerprint disagrees with its own file name is skipped on a read and refused
// on a write: a hand-edited or truncated document is never read as a recipe.
//
// Reading never creates the directory, and a status tick does not re-read every
// document: verified recipes are cached per root and consulted only while that
// directory's name/size/stamp fingerprint is unchanged, so anything added,
// removed or rewritten re-reads. `find` bypasses the cache as an explicit read
// that verifies the document before it is used, and a mutation refreshes it.

#pragma once

#include <optional>

#include <QtCore/QString>
#include <QtCore/QVector>

#include "native/models/NativeCatalog.hpp"
#include "native/models/NativeRecipe.hpp"
#include "native/status/NativeTypes.hpp"

namespace mplayer {
namespace native {

/// The lowercase SHA-256 of a recipe's canonical body: everything that decides
/// what runs, with the id and the display label left out. The id and document
/// name a store issues are both built from this value, so a caller can name a
/// recipe before writing one and a reader can tell whether a stored document is
/// still the one its own name describes.
QString recipeBodyHash(const Recipe& recipe);

/// The stable id a stored recipe for this exact body carries:
/// "recipe:sha256:<body hash>:<backend>". Named so a caller can point at a
/// stored recipe without reading it back; it is never an ordinal and never a
/// basename.
QString storedRecipeIdFor(const Recipe& recipe);

/// The stable id the single-stage spatial recipe built from `model` on `backend`
/// stores under. It is the fingerprint of exactly the same body
/// `addSpatialModel` would persist, or empty when `model` carries no usable
/// model id to name.
QString spatialRecipeIdFor(const ModelRecord& model, Backend backend);

class UserRecipeStore {
 public:
  /// `AppLocalDataLocation()/native/recipes`. Never created by this call.
  static QString defaultRoot();

  /// The app-owned default root. Never created by this call.
  UserRecipeStore();

  /// An explicit root. Constructing validates and creates nothing; an explicit
  /// root is caller-vouched, but still has to be absolute and reachable without
  /// a reparse point.
  explicit UserRecipeStore(QString root);

  const QString& root() const { return root_; }

  /// True when this root may be written: inside the app-owned `native/recipes`
  /// hierarchy, or an explicit absolute root that resolves without a reparse
  /// point. A store pointed at a user's own model folder is the failure this
  /// stops, and no argument turns it into this store's root.
  bool isOwned(QString* reason = nullptr) const;

  /// Creates the root if it is owned and absent. The only writer of it.
  bool ensureOwned(QString* reason = nullptr) const;

  /// Persists one typed recipe - spatial-only, RIFE-only or combined - as an
  /// immutable document named by its own body, or returns the recipe already
  /// stored under that body's id. The id is derived from the body; any id the
  /// caller left on `recipe` is ignored, and the label is stored as given but
  /// never hashed. The recipe must validate in full; anything else is refused by
  /// name and nothing is written. Saving the same body twice never rewrites the
  /// stored document.
  std::optional<Recipe> saveRecipe(const Recipe& recipe, QString* reasonKey = nullptr,
                                   QString* reason = nullptr);

  /// Persists the single-stage spatial recipe for `model` on `backend`, or
  /// returns the recipe already stored under the same id. `model` must be a
  /// spatial row whose declared profile is eligible and ready and whose
  /// content-addressed id matches its own content hash; anything else is
  /// refused by name and nothing is written. This is the one-stage case of
  /// `saveRecipe`, which it delegates to.
  std::optional<Recipe> addSpatialModel(const ModelRecord& model, Backend backend,
                                        QString* reasonKey = nullptr,
                                        QString* reason = nullptr);

  /// Reads and verifies one stored recipe by id, whatever its shape. An id this
  /// naming scheme does not encode, or a document whose id, backend or body
  /// fingerprint disagrees with its own file name, is a named refusal.
  std::optional<Recipe> find(const QString& id, QString* reasonKey = nullptr,
                             QString* reason = nullptr) const;

  /// Every stored recipe of any shape that verifies, in file-name order. A
  /// document that does not verify is left out rather than repaired or reported
  /// as usable; an empty result is not an error.
  QVector<Recipe> recipes(QString* reason = nullptr) const;

 private:
  QString root_;
  bool explicitRoot_ = false;
};

}  // namespace native
}  // namespace mplayer
