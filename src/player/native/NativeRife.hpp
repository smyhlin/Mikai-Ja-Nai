// The RIFE capability table and the one typed factory that turns a user's RIFE
// configuration into a recipe.
//
// Two facts are load-bearing here.
//
//  * Identity is the model name, never an index. The four original v4.14
//    variants - full and lite, each with and without the ensemble flag - are
//    named exactly as the reference backend asks for them (`rife_v4.14`,
//    `rife_v4.14_lite`, `rife_v4.14_ensemble`, `rife_v4.14_lite_ensemble`), so a
//    persisted request can never be repointed at another variant by reordering a
//    combo box. `rifeModelCode` is the model number the reference conf uses
//    (414 full, 4141 lite); ensemble is a separate conf key and never changes it.
//
//  * Every capability in this build is Unavailable, and every row says why: the
//    original v4.14 weights are acquired and hashed, but the
//    converted-graph-to-original-weight lineage hop is unresolved and no
//    prepared native runtime graph has been provisioned, so nothing can attest
//    the graph's provenance. The configuration is still a legal, storable
//    request; the picture stays on the original pipeline until a real source/
//    graph right and a prepared native pipeline exist. Requesting is
//    configuration, not execution - the two are kept apart on purpose.
//
// The factory builds only what was asked for: a RIFE-only recipe when no base is
// named, or a Combined recipe that preserves the selected spatial recipe's
// ordered stages and per-stage pre-resizes exactly. It never substitutes a
// variant, model, chain or backend.

#pragma once

#include <optional>

#include <QtCore/QString>
#include <QtCore/QVector>

#include "NativeRecipe.hpp"
#include "NativeTypes.hpp"

namespace mplayer {
namespace native {

/// The stable key every capability row carries while the weights' lineage and
/// the prepared runtime graph are unresolved. A row's reason is never empty.
inline constexpr char kRifeCapabilityReasonKey[] = "rife.capability.unavailable";

/// One row of the capability table: the exact original variant this build's
/// model space reaches, and its honest availability.
struct RifeCapability {
    RifeVersion version = RifeVersion::V4_14;
    RifeVariant variant = RifeVariant::Full;
    bool ensemble = false;

    /// `rife_v4.14[_lite][_ensemble]`. The identity, never an ordinal.
    QString modelId;

    /// The reference model number: 414 for full, 4141 for lite. Ensemble is a
    /// separate conf key and never changes it.
    int modelCode = 0;

    /// Unavailable in this build, with the fact that blocks it.
    AvailabilityState availability;
};

/// The four v4.14 variants, in a stable order. Every row is Unavailable.
QVector<RifeCapability> rifeCapabilities();

/// The row naming `modelId`, or nullptr. Identity is the model name.
const RifeCapability* findRifeCapability(const QString& modelId);

/// The row for the exact variant, or nullptr.
const RifeCapability* findRifeCapability(RifeVersion version, RifeVariant variant, bool ensemble);

/// Everything the user asked a RIFE stage to be, plus the surface it sits on.
struct RifeRequest {
    /// The interpolation stage exactly as typed: variant, ensemble, the exact
    /// (unreduced) rational factor, the order and the scene threshold.
    TemporalStage temporal;

    /// The explicit backend the request names. Never inferred from a base.
    Backend backend = Backend::TensorRT;

    /// The stored spatial recipe to combine with, or nothing for RIFE-only. Its
    /// ordered stages and per-stage pre-resizes are copied verbatim.
    std::optional<Recipe> base;

    /// The label the stored document carries. Never part of the recipe's
    /// identity - the store's id is derived from the body alone.
    QString label;
};

/// Builds the exact recipe the request names.
///
/// RIFE-only when no base is supplied; Combined, with the base's ordered spatial
/// chain preserved, otherwise. Fails by name when the temporal stage is
/// malformed (a non-positive factor, a threshold outside [0, 1]) or when the
/// base is not a spatial-only recipe - an empty, RIFE-only or already-combined
/// base names its own refusal instead of being silently dropped or merged.
///
/// The returned recipe carries the store's stable id for its body, so a caller
/// can name it before it is written; saving it through `UserRecipeStore` is
/// idempotent for that id.
std::optional<Recipe> buildRifeRecipe(const RifeRequest& request, QString* errorKey = nullptr,
                                      QString* error = nullptr);

}  // namespace native
}  // namespace mplayer
