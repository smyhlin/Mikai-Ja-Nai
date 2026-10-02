// The RIFE capability table and factory. See NativeRife.hpp for the rules; this
// file is their one implementation.

#include "NativeRife.hpp"

#include <QtCore/QCoreApplication>

#include "NativeUserRecipes.hpp"

namespace mplayer {
namespace native {

namespace {

void fail(QString* reasonKey, QString* reason, const QString& key, const QString& text) {
    if (reasonKey != nullptr) *reasonKey = key;
    if (reason != nullptr) *reason = text;
}

/// The one fact every row carries: the weights exist and are hashed, but the
/// lineage hop and the prepared runtime graph that would attest them do not, so
/// the variant cannot be exposed as Ready. Named, never blank.
QString unavailableReason(const QString& modelId) {
    return QCoreApplication::translate(
               "NativeRife",
               "%1 is not available: the original v4.14 weights are acquired and hashed, but the "
               "converted-graph-to-original-weight lineage hop is unresolved and no prepared "
               "native runtime graph attesting their provenance has been provisioned. A "
               "configuration naming this variant is stored as a request; the picture keeps "
               "using the original pipeline until both the weight hop and that prepared runtime "
               "exist.")
        .arg(modelId);
}

/// The four original variants, exactly the axis the reference backend names.
struct VariantSpec {
    RifeVariant variant;
    bool ensemble;
};

const VariantSpec kVariants[] = {
    {RifeVariant::Full, false},
    {RifeVariant::Lite, false},
    {RifeVariant::Full, true},
    {RifeVariant::Lite, true},
};

}  // namespace

QVector<RifeCapability> rifeCapabilities() {
    QVector<RifeCapability> rows;
    rows.reserve(static_cast<int>(sizeof(kVariants) / sizeof(kVariants[0])));
    for (const VariantSpec& spec : kVariants) {
        RifeCapability row;
        row.version = RifeVersion::V4_14;
        row.variant = spec.variant;
        row.ensemble = spec.ensemble;
        row.modelId = rifeModelName(row.version, row.variant, row.ensemble);
        row.modelCode = rifeModelCode(row.variant);
        row.availability = unavailableAvailability(QString::fromLatin1(kRifeCapabilityReasonKey),
                                                   unavailableReason(row.modelId));
        rows.append(row);
    }
    return rows;
}

static const QVector<RifeCapability>& capabilityTable() {
    // One table for the process: the rows are immutable, so a lookup can return
    // a pointer into it without the caller having to own a copy.
    static const QVector<RifeCapability> rows = rifeCapabilities();
    return rows;
}

const RifeCapability* findRifeCapability(const QString& modelId) {
    for (const RifeCapability& row : capabilityTable()) {
        if (row.modelId == modelId) return &row;
    }
    return nullptr;
}

const RifeCapability* findRifeCapability(RifeVersion version, RifeVariant variant, bool ensemble) {
    for (const RifeCapability& row : capabilityTable()) {
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
