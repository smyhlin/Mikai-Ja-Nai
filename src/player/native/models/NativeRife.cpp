// The RIFE capability table, the exact content pins and the resolver. See
// NativeRife.hpp for the rules; this file is their one implementation.

#include "NativeRife.hpp"

#include <QtCore/QCoreApplication>

#include "native/models/NativeModelInventory.hpp"
#include "native/models/NativeUserRecipes.hpp"

namespace mplayer {
namespace native {

namespace {

void fail(QString* reasonKey, QString* reason, const QString& key, const QString& text) {
    if (reasonKey != nullptr) *reasonKey = key;
    if (reason != nullptr) *reason = text;
}

/// The four original variants, exactly the axis the reference backend names,
/// each with the exact digest of the original graph bytes it reaches. The
/// digests are the content pins; the file names are only what a refusal prints.
const RifeModelPin kPins[] = {
    {RifeVersion::V4_14, RifeVariant::Full, false,
     QStringLiteral("a64b9cd0fb463d620688394437f35f02e33c0658c39ccce2b9637899fb520cbe"),
     QStringLiteral("rife_v4.14.onnx")},
    {RifeVersion::V4_14, RifeVariant::Lite, false,
     QStringLiteral("948019ffb5e7e54ac6b89a13542da8d100af9b4ade7374551c5f595ac3ff2ce7"),
     QStringLiteral("rife_v4.14_lite.onnx")},
    {RifeVersion::V4_14, RifeVariant::Full, true,
     QStringLiteral("ec64f92e31a1b61c034b4de6f1afc1c2aeaf478b93dbf0a6ce2e69a327385b53"),
     QStringLiteral("rife_v4.14_ensemble.onnx")},
    {RifeVersion::V4_14, RifeVariant::Lite, true,
     QStringLiteral("9486d87ca17a83a1594ac54b254b6fb1be6fa52f5dd11a31e4a602d124ea8b1b"),
     QStringLiteral("rife_v4.14_lite_ensemble.onnx")},
};

/// The pin is the identity; `fileName` only names the acquisition in a refusal.
/// Resolution is `resolveRifeModel` below.
const RifeModelPin* pinOf(RifeVersion version, RifeVariant variant, bool ensemble) {
    for (const RifeModelPin& pin : kPins) {
        if (pin.version == version && pin.variant == variant && pin.ensemble == ensemble) {
            return &pin;
        }
    }
    return nullptr;
}

QString rowLabel(const RifeModelPin& pin) {
    return pin.fileName.isEmpty() ? rifeModelName(pin.version, pin.variant, pin.ensemble)
                                  : pin.fileName;
}

QVector<RifeCapability> identityRows() {
    QVector<RifeCapability> rows;
    rows.reserve(static_cast<int>(sizeof(kPins) / sizeof(kPins[0])));
    for (const RifeModelPin& pin : kPins) {
        RifeCapability row;
        row.version = pin.version;
        row.variant = pin.variant;
        row.ensemble = pin.ensemble;
        row.modelId = rifeModelName(pin.version, pin.variant, pin.ensemble);
        row.modelCode = rifeModelCode(pin.variant);
        row.graphSha256 = pin.graphSha256;
        row.availability =
            preparingAvailability(
                QStringLiteral("rife.capability.unresolved"),
                QCoreApplication::translate(
                    "NativeRife",
                    "%1 carries the exact content pin %2, but its availability has not been "
                    "resolved against an owned model store.")
                    .arg(row.modelId, pin.graphSha256));
        rows.append(row);
    }
    return rows;
}

const QVector<RifeCapability>& identityTable() {
    // One table for the process: the rows are immutable, so a lookup can return
    // a pointer into it without the caller having to own a copy.
    static const QVector<RifeCapability> rows = identityRows();
    return rows;
}

/// The row's own content-addressed id is only an identity when it still derives
/// from the row's digest, role and declared profile. A row whose id is a curated
/// name, or any other string, is a wrongly pinned row, not the model.
bool rowIdIsContentAddressed(const ModelRecord& record) {
    return record.id == userModelIdFor(computeContentId(record.role, record.onnxSha256,
                                                       record.actualProfile));
}

}  // namespace

const QVector<RifeModelPin>& rifeModelPins() {
    static const QVector<RifeModelPin> pins(kPins, kPins + sizeof(kPins) / sizeof(kPins[0]));
    return pins;
}

const RifeModelPin* rifeModelPin(RifeVersion version, RifeVariant variant, bool ensemble) {
    return pinOf(version, variant, ensemble);
}

const RifeModelPin* rifeModelPinForGraphSha(const QString& sha256) {
    for (const RifeModelPin& pin : rifeModelPins()) {
        if (pin.graphSha256.compare(sha256, Qt::CaseInsensitive) == 0) return &pin;
    }
    return nullptr;
}

const RifeModelPin* rifeModelPinForName(const QString& modelId) {
    for (const RifeModelPin& pin : rifeModelPins()) {
        if (rifeModelName(pin.version, pin.variant, pin.ensemble) == modelId) return &pin;
    }
    return nullptr;
}

RifeModelResolution resolveRifeModel(RifeVersion version, RifeVariant variant, bool ensemble,
                                     const CatalogView& catalog) {
    RifeModelResolution result;
    result.pin = pinOf(version, variant, ensemble);
    const QString named = rifeModelName(version, variant, ensemble);
    if (result.pin == nullptr) {
        fail(&result.reasonKey, &result.reason, QStringLiteral("rife.pin.unknown"),
             QStringLiteral("no original %1 variant is pinned in this build").arg(named));
        return result;
    }

    // The pin is the content digest; the join is by digest, never by a name that
    // happens to look like the curated one, so no alias row can be substituted.
    const ModelRecord* pinned = nullptr;
    for (const ModelRecord& record : catalog.models) {
        if (record.onnxSha256.compare(result.pin->graphSha256, Qt::CaseInsensitive) == 0) {
            pinned = &record;
            break;
        }
    }
    if (pinned == nullptr) {
        fail(&result.reasonKey, &result.reason, QStringLiteral("rife.model.missing"),
             QCoreApplication::translate(
                 "NativeRife",
                 "%1 is pinned to the original graph %2 (%3), which the owned model store does "
                 "not hold; the stage is refused, not substituted.")
                 .arg(named, result.pin->graphSha256, rowLabel(*result.pin)));
        return result;
    }
    if (pinned->role != ModelRole::Temporal) {
        fail(&result.reasonKey, &result.reason, QStringLiteral("rife.model.role"),
             QCoreApplication::translate(
                 "NativeRife",
                 "the owned row carrying the %1 pin is a %2 model; the pinned original is a "
                 "temporal graph and no other row is substituted.")
                 .arg(named, stableName(pinned->role)));
        return result;
    }
    // A row whose id is not derived from its own content is an alias or a
    // tampered row, not the pinned graph.
    if (!rowIdIsContentAddressed(*pinned)) {
        fail(&result.reasonKey, &result.reason, QStringLiteral("rife.model.identity"),
             QCoreApplication::translate(
                 "NativeRife",
                 "the owned row carrying the %1 pin has an id that does not derive from its "
                 "digest, role and declared profile.")
                 .arg(named));
        return result;
    }

    QString offerKey;
    QString offerReason;
    pinned->refusal(&offerKey, &offerReason);
    if (!offerKey.isEmpty()) {
        fail(&result.reasonKey, &result.reason, offerKey, offerReason);
        return result;
    }
    if (!pinned->availability.isUsable()) {
        // The row is present but the store marked it unusable - corrupt bytes, a
        // broken manifest, or an unproved boundary. Its own reason is the fact.
        fail(&result.reasonKey, &result.reason,
             pinned->availability.reasonKey.isEmpty() ? QStringLiteral("rife.model.unavailable")
                                                      : pinned->availability.reasonKey,
             pinned->availability.reason.isEmpty()
                 ? QCoreApplication::translate("NativeRife",
                                               "%1 is not usable in the owned model store now.")
                       .arg(named)
                 : pinned->availability.reason);
        return result;
    }

    const QString boundary = temporalCandidateRefusal(pinned->actualProfile);
    if (!boundary.isEmpty()) {
        fail(&result.reasonKey, &result.reason, QStringLiteral("rife.model.boundary"),
             QCoreApplication::translate(
                 "NativeRife",
                 "the owned row for %1 does not declare the RIFE boundary: %2")
                 .arg(named, boundary));
        return result;
    }
    if (!pinned->verifiedReferenceProfile.has_value()) {
        fail(&result.reasonKey, &result.reason, QStringLiteral("rife.model.reference"),
             QCoreApplication::translate(
                 "NativeRife",
                 "the owned row for %1 declares the RIFE boundary but carries no CPU-reference "
                 "proof that a run produced it.")
                 .arg(named));
        return result;
    }
    const QString proof =
        temporalReferenceRefusal(pinned->actualProfile, *pinned->verifiedReferenceProfile);
    if (!proof.isEmpty()) {
        fail(&result.reasonKey, &result.reason, QStringLiteral("rife.model.reference"),
             QCoreApplication::translate(
                 "NativeRife",
                 "the CPU-reference proof carried by %1 does not prove its declared boundary: %2")
                 .arg(named, proof));
        return result;
    }

    result.record = pinned;
    result.reasonKey.clear();
    result.reason.clear();
    return result;
}

RifeModelResolution resolveRifeModel(const TemporalStage& stage, const CatalogView& catalog) {
    return resolveRifeModel(stage.version, stage.variant, stage.ensemble, catalog);
}

QVector<RifeCapability> rifeCapabilities(const CatalogView& catalog) {
    QVector<RifeCapability> rows;
    rows.reserve(rifeModelPins().size());
    for (const RifeModelPin& pin : rifeModelPins()) {
        RifeCapability row;
        row.version = pin.version;
        row.variant = pin.variant;
        row.ensemble = pin.ensemble;
        row.modelId = rifeModelName(pin.version, pin.variant, pin.ensemble);
        row.modelCode = rifeModelCode(pin.variant);
        row.graphSha256 = pin.graphSha256;

        const RifeModelResolution resolution =
            resolveRifeModel(pin.version, pin.variant, pin.ensemble, catalog);
        if (resolution.resolved()) {
            row.resolvedModelId = resolution.record->id;
            row.availability = readyAvailability();
        } else {
            row.availability = unavailableAvailability(resolution.reasonKey, resolution.reason);
        }
        rows.append(row);
    }
    return rows;
}

const RifeCapability* findRifeCapability(const QString& modelId) {
    for (const RifeCapability& row : identityTable()) {
        if (row.modelId == modelId) return &row;
    }
    return nullptr;
}

const RifeCapability* findRifeCapability(RifeVersion version, RifeVariant variant, bool ensemble) {
    for (const RifeCapability& row : identityTable()) {
        if (row.version == version && row.variant == variant && row.ensemble == ensemble) {
            return &row;
        }
    }
    return nullptr;
}

std::optional<Recipe> buildRifeRecipe(const RifeRequest& request, QString* errorKey,
                                      QString* error) {
    const auto reject = [errorKey, error](const QString& key, const QString& text) {
        fail(errorKey, error, key, text);
        return std::optional<Recipe>();
    };

    // The temporal stage is validated on its own first, so a malformed factor or
    // threshold is refused by its own key before any shape is built around it.
    const RecipeVerdict temporalVerdict = validateTemporalStage(request.temporal);
    if (!temporalVerdict.ok) {
        return reject(temporalVerdict.reasonKey, temporalVerdict.reason);
    }

    if (request.base.has_value()) {
        const Recipe& base = *request.base;
        // Combining is defined only over a spatial-only recipe: an empty, a
        // RIFE-only or an already-combined base would give two temporal stages or
        // none, and no chain is ever invented to fill the gap.
        if (base.shape() != RecipeShape::SpatialOnly) {
            return reject(
                QStringLiteral("rife.base.shape"),
                QCoreApplication::translate(
                    "NativeRife",
                    "A combined recipe can only be built from a spatial-only recipe; the supplied "
                    "base is %1, so it is refused rather than merged or dropped.")
                    .arg(base.shapeName()));
        }
        for (const SpatialStage& stage : base.spatial) {
            const RecipeVerdict verdict = validateSpatialStage(stage);
            if (!verdict.ok) {
                return reject(verdict.reasonKey, verdict.reason);
            }
        }
    }

    Recipe recipe;
    recipe.label = request.label;
    recipe.backend = request.backend;
    recipe.temporal = request.temporal;
    if (request.base.has_value()) {
        // Preserved exactly: execution order is load-bearing, and so is each
        // stage's pre-resize; neither is recomputed from anything.
        recipe.spatial = request.base->spatial;
    }
    // The store's own id for this body, so the result is a fully-formed recipe a
    // caller can name before it is written.
    recipe.id = storedRecipeIdFor(recipe);

    const RecipeVerdict verdict = validateRecipe(recipe);
    if (!verdict.ok) {
        return reject(verdict.reasonKey, verdict.reason);
    }
    return recipe;
}

}  // namespace native
}  // namespace mplayer
