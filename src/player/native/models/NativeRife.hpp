// The RIFE capability table, the exact content pins those curated names resolve
// to, and the one typed factory that turns a user's RIFE configuration into a
// recipe.
//
// Three facts are load-bearing here.
//
//  * Identity is the model name, never an index. The pinned component has
//    versioned full/lite variants and, for v4.26, a heavy graph; names preserve
//    the archive spelling (`rife_v4.14_lite_ensemble`,
//    `rife_v4.26_heavy`). `rifeModelCode` is version-aware (414 full, 4141
//    lite); ensemble is a separate conf key and never changes it. Heavy remains
//    unavailable because pinned Aji v0.9.0 does not encode its `_heavy` suffix.
//
//  * A curated name is a REQUEST, not a graph. Each one carries an exact content
//    pin - the lower-case SHA-256 of the exact original graph bytes this build's
//    model space reaches - and nothing resolves that name to a runnable graph
//    except `resolveRifeModel`, which joins the pin to an owned temporal row in
//    the store the caller supplies. The pin is data, not an alias: there is no
//    second ModelRecord named after a curated variant, and a row that happens to
//    claim one of these names is never accepted as the model. A pin that is
//    absent, a row that does not carry it, a row of another role, a declaration
//    that is not the RIFE boundary, or a missing/disagreeing CPU-reference proof
//    each keep their own named reason.
//
//  * Provenance is still unresolved for redistribution. The four original v4.14
//    graphs have private local byte-level proof against the author's MIT v4.14
//    full/lite checkpoints, but the exporter identity, the non-weight graph
//    material and the applicable whole-graph permissions are unresolved. The
//    expanded archive pins are acquisition identities, not a license or runtime
//    proof. No publicly shippable claim is made here. A request remains
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

#include "native/models/NativeRecipe.hpp"
#include "native/status/NativeTypes.hpp"

namespace mplayer {
namespace native {

/// One pinned original temporal graph: the exact content digest a curated variant
/// resolves to, and the acquisition file name that digest was taken from (used
/// only so a refusal can name the row).
struct RifeModelPin {
    RifeVersion version = RifeVersion::V4_14;
    RifeVariant variant = RifeVariant::Full;
    bool ensemble = false;

    /// Lower-case SHA-256 of the exact original graph bytes. The content pin: a
    /// row is this variant exactly when its `onnxSha256` equals this value.
    QString graphSha256;

    /// The acquisition file name, for refusal sentences only. Never identity and
    /// never read.
    QString fileName;

    /// Exact byte size of the pinned acquisition graph.
    qint64 bytes = 0;
};

/// The exact pinned variants in the component archive, in stable filename
/// order. Data only; nothing here is a claim about what the store holds.
const QVector<RifeModelPin>& rifeModelPins();

/// The pin for the exact variant identity, or nullptr.
const RifeModelPin* rifeModelPin(RifeVersion version, RifeVariant variant, bool ensemble);

/// The pin whose content digest is `sha256` (case-insensitive), or nullptr. Two
/// variants can never share a digest; if that ever happened this returns the
/// first in table order and `resolveRifeModel` still refuses the ambiguous row.
const RifeModelPin* rifeModelPinForGraphSha(const QString& sha256);

/// The pin a curated model name (`rife_v4.14_lite_ensemble`, ...) names, or
/// nullptr. This is identity lookup only - never a resolution.
const RifeModelPin* rifeModelPinForName(const QString& modelId);

/// One row of the capability table: the exact original variant this build's
/// model space reaches, its content pin, and its honest availability.
struct RifeCapability {
    RifeVersion version = RifeVersion::V4_14;
    RifeVariant variant = RifeVariant::Full;
    bool ensemble = false;

    /// The stable `rife_vX.Y` name, including its exact variant/ensemble suffix.
    /// The identity, never an ordinal.
    QString modelId;

    /// The version-aware reference model number: 414 for v4.14 full, 4141 for
    /// v4.14 lite. Ensemble is a separate conf key and never changes it.
    int modelCode = 0;

    /// The exact graph digest this row resolves to. Never empty.
    QString graphSha256;

    /// The owned catalogue row's own content-addressed id, once the pin has been
    /// resolved against a store. Empty when it has not been resolved here. This
    /// is the identity planning and selection consume - never the curated name.
    QString resolvedModelId;

    /// Usability now, with the mandatory reason when it is not.
    AvailabilityState availability;
};

/// The outcome of resolving one curated variant against supplied owned rows.
struct RifeModelResolution {
    /// The pin the request named, or nullptr when the variant identity is not one
    /// this build reaches.
    const RifeModelPin* pin = nullptr;

    /// The owned row the pin resolved to, pointing into the `CatalogView` the
    /// resolver was given. Null on refusal. Its lifetime is the caller's.
    const ModelRecord* record = nullptr;

    /// Empty on success; on refusal the stable key and the sentence.
    QString reasonKey;
    QString reason;

    bool resolved() const { return record != nullptr; }
    /// The owned row's content-addressed id, or empty.
    QString modelId() const { return record != nullptr ? record->id : QString(); }
    /// The owned row's content identity, or empty.
    QString contentId() const { return record != nullptr ? record->contentId() : QString(); }
};

/// Resolves the exact pinned owned temporal graph a curated variant names.
///
/// Never substitutes: it walks `catalog` for the row whose `onnxSha256` equals
/// the variant's pin, then requires that the row is offered and usable, carries
/// the Temporal role, declares the RIFE 1xC11 -> 1xC3 fp16 boundary, and carries a
/// CPU-reference proof that agrees with that declaration. Any other outcome - an
/// unknown variant, an absent pinned row, a carried pin in a non-temporal row, a
/// corrupt/unready row, a wrong declared boundary, or a missing/disagreeing
/// reference - returns a resolution with its own named `reasonKey`/`reason` and a
/// null `record`. A row that merely claims the curated name is never accepted.
RifeModelResolution resolveRifeModel(RifeVersion version, RifeVariant variant, bool ensemble,
                                     const CatalogView& catalog);

/// As above for a recipe's temporal stage: version, variant and ensemble are the
/// only facts consulted; the stage's factor, order and threshold are preserved by
/// the caller, never by this join.
RifeModelResolution resolveRifeModel(const TemporalStage& stage, const CatalogView& catalog);

/// The curated variants with availability observed from `catalog`. Each row
/// resolves its pin; a row whose pinned owned graph is present and usable is
/// offered, and every other row carries the refusal `resolveRifeModel` produced.
QVector<RifeCapability> rifeCapabilities(const CatalogView& catalog);

/// Identity lookup only: the capability row naming `modelId`, or nullptr. Its
/// availability has not been observed against any store, so it is Preparing with
/// the reason that says so - a caller that needs usability must call
/// `rifeCapabilities(catalog)` or `resolveRifeModel`.
const RifeCapability* findRifeCapability(const QString& modelId);

/// The row for the exact variant identity, or nullptr. Same unresolved
/// availability as above.
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
