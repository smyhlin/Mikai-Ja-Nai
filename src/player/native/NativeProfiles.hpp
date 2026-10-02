// The curated spatial profiles: the three stable request leaves this build
// offers, and the rule that turns a source's actual geometry and frame rate
// into the one model chain that profile names.
//
// A profile is a REQUEST TEMPLATE, not a recipe. Its id (`janai.profile.<name>.v1`)
// is what the preference store keeps; resolving it against a catalog produces
// the concrete one-stage spatial recipe for the source at hand. The reference
// preserves the requested template id as the recipe id so a stored request is
// never silently repointed to a different window's model.
//
// The window table below is reference DATA, not adopted source: it was observed
// from `src/aji_conf.cpp` lines 226-300 at pinned commit
// 738ce3b3020b3950c48ce3122a08f32af531d4fa of the upstream project, whose
// built-in slots are 1001 Quality, 1002 Balanced and 1003 Performance. The model
// digests are the byte-verified acquisitions recorded in
// `native/spatial-acquisition.md`. Nothing here is copied, built or executed;
// the graphs are read-only reference inputs and no engine exists in this build.
//
// A window matches on the source's pixel QUANTITY (w*h) and its frame rate,
// both bounds inclusive, first match in order. Frames per second are compared as
// an exact rational by cross-multiplication of positive integers - never as a
// double, so a boundary at 31 or 61 is never inferred by rounding.
//
// The chosen window's model is looked up in the supplied catalogue by its exact
// ONNX digest. When that row is absent or ineligible the resolution fails: a
// different window's model is never substituted, the same way a missing model is
// never quietly swapped for another chain. An unknown source - no positive
// geometry, or no valid frame rate - is named as needing a source rather than
// being given an invented 0 or 1 fps.

#pragma once

#include <optional>

#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QVector>

#include "NativeCatalog.hpp"
#include "NativePrefs.hpp"
#include "NativeRecipe.hpp"
#include "NativeTypes.hpp"

namespace mplayer {
namespace native {

/// One source window for a profile: the inclusive pixel-count and frame-rate
/// bounds a source must fall inside for this window's model to be chosen, and
/// the exact ONNX digest that identifies the model.
struct ProfileWindow {
  /// Inclusive pixel-quantity bounds, `width * height`. Zero is an open end.
  qint64 minPixels = 0;
  qint64 maxPixels = 0;
  /// Inclusive frame-rate bounds, `num/den` in whole frames per second. Zero is
  /// an open end, and a live rate is always positive so the lower bound is
  /// satisfied without inventing a rate.
  int minFps = 0;
  int maxFps = 0;
  /// The 64-character ONNX sha256 of the model this window resolves to.
  QString modelSha256;
  /// The acquired file name, used only to make a refusal sentence name the row.
  QString modelName;
};

/// A curated profile: the stable id the store keeps, the user-facing label, and
/// the ordered windows the model is chosen from.
struct ProfileTemplate {
  QString id;
  QString label;
  QVector<ProfileWindow> windows;
};

/// The three curated profiles, in the order they are offered. Ids and windows
/// are data; labels are translated with the `NativeProfiles` runtime context.
QVector<ProfileTemplate> profileTemplates();

/// The three curated leaves as settings-selector options, each carrying exactly
/// the supplied engine reason. Nothing is claimed Ready: this build has no engine
/// to run a profile, so the only honest availability is the caller's gap.
QVector<NativeRecipeOption> builtinProfileOptions(const QString& reasonKey,
                                                  const QString& reason);

/// Resolves `id` for a source of `sourceSize` pixels at `sourceRate` frames per
/// second, against the models `catalog` vouches for, for the requested
/// `backend`.
///
/// Returns the chosen chain exactly when the source is positive and known, a
/// window matches, and the window's model is a catalogue row that is offered,
/// usable now and carries `backend`. A row its provisioner marked Preparing or
/// Unavailable is refused with that row's own reason rather than promoted. The
/// row must be identified by its own content: a
/// content-addressed row whose id does not derive from its digest and declared
/// profile is refused as an alias. The recipe's id is the requested template id
/// and its one spatial stage names the catalogue's own id for that model. On
/// failure returns nothing with `reasonKey`/`reason` set; a missing chosen model
/// is never replaced by another window's.
std::optional<Recipe> resolveProfile(const QString& id, const QSize& sourceSize,
                                     const Rational& sourceRate, Backend backend,
                                     const CatalogView& catalog, QString* reasonKey = nullptr,
                                     QString* reason = nullptr);

}  // namespace native
}  // namespace mplayer
