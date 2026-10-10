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

/// Exact graph pins from the SHA-verified AnimeJaNai 3.7.0 Windows component.
const RifeModelPin kPins[] = {
    {RifeVersion::V4_7, RifeVariant::Full, false, QStringLiteral("8ec8fc7516e64a6f70b17dade22845b6cfa10a1624f10b77ee5def3a780a3c97"), QStringLiteral("rife_v4.7.onnx"), 10699117},
    {RifeVersion::V4_7, RifeVariant::Full, true, QStringLiteral("49a144c7dc24d26411cf8252007e94029f242b55c84d82fda5477f8ef790354a"), QStringLiteral("rife_v4.7_ensemble.onnx"), 10705878},
    {RifeVersion::V4_8, RifeVariant::Full, false, QStringLiteral("a51289c5f8301b0fad0e85c38dd944f749eb4d7d2a7e1c3653ac81463c1d11ef"), QStringLiteral("rife_v4.8.onnx"), 10699119},
    {RifeVersion::V4_8, RifeVariant::Full, true, QStringLiteral("95111db7349bd30cfacfff0086cd01860cfbd7479411de8a427cf8607466e5ba"), QStringLiteral("rife_v4.8_ensemble.onnx"), 10705878},
    {RifeVersion::V4_9, RifeVariant::Full, false, QStringLiteral("c206243e8a79cff8dd699d52f42005bd2bb9e0639ca2494e4dc299d533317fb1"), QStringLiteral("rife_v4.9.onnx"), 10699119},
    {RifeVersion::V4_9, RifeVariant::Full, true, QStringLiteral("a1755c4f5bb39546c42244d5bd20910712a8487c73725f99d1b322965aa1a5e3"), QStringLiteral("rife_v4.9_ensemble.onnx"), 10705878},
    {RifeVersion::V4_10, RifeVariant::Full, false, QStringLiteral("b567f8caaf282a7d14ad8ae364b29aa38670fd53e541b7c983dccf3aaf4f79ed"), QStringLiteral("rife_v4.10.onnx"), 10778675},
    {RifeVersion::V4_10, RifeVariant::Full, true, QStringLiteral("f3faa93a7c7166a2750c6a7a208c1f90e54d8982f136e1c2febf66eebb98d346"), QStringLiteral("rife_v4.10_ensemble.onnx"), 10785434},
    {RifeVersion::V4_11, RifeVariant::Full, false, QStringLiteral("bf42e5f078278662d934c9dcd57e5dfc30218b1782df2285f7b9588ebcb4f734"), QStringLiteral("rife_v4.11.onnx"), 10778675},
    {RifeVersion::V4_11, RifeVariant::Full, true, QStringLiteral("7a41cc43d1f0d685a9625b52dbd2a40b9e07b05b2a746b05ceb6e644838831d9"), QStringLiteral("rife_v4.11_ensemble.onnx"), 10785434},
    {RifeVersion::V4_12, RifeVariant::Full, false, QStringLiteral("72851c06ca6b9e94e648e93d1ec4dbfb155650e4e0b2a78190bee96ffc973b68"), QStringLiteral("rife_v4.12.onnx"), 10778675},
    {RifeVersion::V4_12, RifeVariant::Lite, false, QStringLiteral("6682d23260c0e1c6b50c5aae2f96df78c871bd502df84d565a2e7bd21ff6a74b"), QStringLiteral("rife_v4.12_lite.onnx"), 5313602},
    {RifeVersion::V4_12, RifeVariant::Full, true, QStringLiteral("1adeaff6d10795866067f4b2c4b26d704871487155cb7330686d857f5dcf3785"), QStringLiteral("rife_v4.12_ensemble.onnx"), 10785434},
    {RifeVersion::V4_12, RifeVariant::Lite, true, QStringLiteral("4bb929960d07d717d964d5152ce506275cbf75277afab18cc092cc8469136bdc"), QStringLiteral("rife_v4.12_lite_ensemble.onnx"), 5320361},
    {RifeVersion::V4_13, RifeVariant::Full, false, QStringLiteral("1ff6495fc6640a1b2b12bd940e5769f617f3be185b58d9fd5dcc69ee0ba84601"), QStringLiteral("rife_v4.13.onnx"), 10778675},
    {RifeVersion::V4_13, RifeVariant::Lite, false, QStringLiteral("53ba1b982f81c2d0644c7131e565b122c11bb6f9435b2ca33f05697d663a94a5"), QStringLiteral("rife_v4.13_lite.onnx"), 5313602},
    {RifeVersion::V4_13, RifeVariant::Full, true, QStringLiteral("4cfc921b277d12a8ac6df747a3e89deec43e42159e4455852fab2a18e7fa00bd"), QStringLiteral("rife_v4.13_ensemble.onnx"), 10785434},
    {RifeVersion::V4_13, RifeVariant::Lite, true, QStringLiteral("b770105149bce79a08ce13458bcdf5d1ea6f47db1b434e637346354d6b5213f3"), QStringLiteral("rife_v4.13_lite_ensemble.onnx"), 5320361},
    {RifeVersion::V4_14, RifeVariant::Full, false, QStringLiteral("a64b9cd0fb463d620688394437f35f02e33c0658c39ccce2b9637899fb520cbe"), QStringLiteral("rife_v4.14.onnx"), 10778647},
    {RifeVersion::V4_14, RifeVariant::Lite, false, QStringLiteral("948019ffb5e7e54ac6b89a13542da8d100af9b4ade7374551c5f595ac3ff2ce7"), QStringLiteral("rife_v4.14_lite.onnx"), 5986311},
    {RifeVersion::V4_14, RifeVariant::Full, true, QStringLiteral("ec64f92e31a1b61c034b4de6f1afc1c2aeaf478b93dbf0a6ce2e69a327385b53"), QStringLiteral("rife_v4.14_ensemble.onnx"), 10785406},
    {RifeVersion::V4_14, RifeVariant::Lite, true, QStringLiteral("9486d87ca17a83a1594ac54b254b6fb1be6fa52f5dd11a31e4a602d124ea8b1b"), QStringLiteral("rife_v4.14_lite_ensemble.onnx"), 5993070},
    {RifeVersion::V4_15, RifeVariant::Full, false, QStringLiteral("372dbaae89e53f592669396117b0ab33cc0f713e1a020b001ed9d452518f10f5"), QStringLiteral("rife_v4.15.onnx"), 10778647},
    {RifeVersion::V4_15, RifeVariant::Lite, false, QStringLiteral("31d065d3ba29c673ef04b3c75b4432b2d301a96ee18f78f79daa8bc6ae6d6913"), QStringLiteral("rife_v4.15_lite.onnx"), 5282914},
    {RifeVersion::V4_15, RifeVariant::Full, true, QStringLiteral("0bf88a4a72348a0031d74c8da7c7a39c97b6af65c06b3b510acdb14e9b474e6c"), QStringLiteral("rife_v4.15_ensemble.onnx"), 10785406},
    {RifeVersion::V4_15, RifeVariant::Lite, true, QStringLiteral("17134647c2bb111fb1e28cc99e701d15e0e02f0443933c799d1c3135814c8657"), QStringLiteral("rife_v4.15_lite_ensemble.onnx"), 5289673},
    {RifeVersion::V4_16, RifeVariant::Lite, false, QStringLiteral("180413d88e77edc37c676d8e3b3d489c9828dac21cef2616521057731252026c"), QStringLiteral("rife_v4.16_lite.onnx"), 5282914},
    {RifeVersion::V4_16, RifeVariant::Lite, true, QStringLiteral("e46683f0e21ae07bb17433d7f9a3a7222514bdb608a338e26966097d3dd55067"), QStringLiteral("rife_v4.16_lite_ensemble.onnx"), 5289673},
    {RifeVersion::V4_17, RifeVariant::Full, false, QStringLiteral("9692216a3e43861e96111dd696fb4ead0caea09a9d9410b84b9922ec3fe44c9b"), QStringLiteral("rife_v4.17.onnx"), 10778647},
    {RifeVersion::V4_17, RifeVariant::Lite, false, QStringLiteral("1aeadfdc7c054b3a7ff802c7494c5d358c3f6920f7e869b1450d62b9d9ebb046"), QStringLiteral("rife_v4.17_lite.onnx"), 5282914},
    {RifeVersion::V4_17, RifeVariant::Full, true, QStringLiteral("ff99cded67be06c29997bffb855bb388353baf828a3a98ae6f5dfd965459b420"), QStringLiteral("rife_v4.17_ensemble.onnx"), 10785406},
    {RifeVersion::V4_17, RifeVariant::Lite, true, QStringLiteral("948c7e3b4d193de9383241e90cec3330114f7f9f7897f433ddca3402faf1363b"), QStringLiteral("rife_v4.17_lite_ensemble.onnx"), 5289673},
    {RifeVersion::V4_18, RifeVariant::Full, false, QStringLiteral("abab532a174473cc7f7dd2657012af4fadf54905ba280440e3fb996bf571d295"), QStringLiteral("rife_v4.18.onnx"), 10778647},
    {RifeVersion::V4_18, RifeVariant::Full, true, QStringLiteral("69c46eb0cf3c1874ca5278758a8e7c4df1b70d0feaa682ce895459b7a6fa9716"), QStringLiteral("rife_v4.18_ensemble.onnx"), 10785406},
    {RifeVersion::V4_19, RifeVariant::Full, false, QStringLiteral("7e8942df65e1bfd3b0f5cfc8b299ab3f56e6ecfc7994cb7a8acdf855f49be90f"), QStringLiteral("rife_v4.19.onnx"), 10778647},
    {RifeVersion::V4_19, RifeVariant::Full, true, QStringLiteral("295d2c9722bac5de250a71d36a976c4f100d4b03722c44268a961534c791218d"), QStringLiteral("rife_v4.19_ensemble.onnx"), 10785406},
    {RifeVersion::V4_20, RifeVariant::Full, false, QStringLiteral("f09535cc9a29ea764dab5522e930987090d8ff57b7f6f9bff60d9733342ecf42"), QStringLiteral("rife_v4.20.onnx"), 30803048},
    {RifeVersion::V4_20, RifeVariant::Full, true, QStringLiteral("83b3778a6feb3125d874a10f97793c7e9357e24ab7691dc548424ac63a30bf64"), QStringLiteral("rife_v4.20_ensemble.onnx"), 30809807},
    {RifeVersion::V4_21, RifeVariant::Full, false, QStringLiteral("868be99514e542105659b2f8e6be3ff4854500add94296a0ca9f1fd49c843c81"), QStringLiteral("rife_v4.21.onnx"), 18694916},
    {RifeVersion::V4_22, RifeVariant::Full, false, QStringLiteral("7fdb67dcf02d23cd573b5269bb41c600b7d4241cfda366099e736938d8f7abc6"), QStringLiteral("rife_v4.22.onnx"), 18694916},
    {RifeVersion::V4_22, RifeVariant::Lite, false, QStringLiteral("8b30b93ef8ab0b3cd1bb8905ad8b911eb887ff4b2900558eb1914d99568e2ca5"), QStringLiteral("rife_v4.22_lite.onnx"), 9782161},
    {RifeVersion::V4_23, RifeVariant::Full, false, QStringLiteral("712a97ce24619492a13eb8925d91a41608b06d204c3d2f8461e8cbe5eccf3933"), QStringLiteral("rife_v4.23.onnx"), 18694916},
    {RifeVersion::V4_24, RifeVariant::Full, false, QStringLiteral("3cb30d8cb2670533b6e2d7efa6f5cae0a133f3dc674d905fbe0bf839aeb169aa"), QStringLiteral("rife_v4.24.onnx"), 10778647},
    {RifeVersion::V4_24, RifeVariant::Full, true, QStringLiteral("8b9c888a86d22aada6febce163d158866e84d3b10487fdc525b3500d2edef42e"), QStringLiteral("rife_v4.24_ensemble.onnx"), 10785406},
    {RifeVersion::V4_25, RifeVariant::Full, false, QStringLiteral("de0fb71ae50fe082eb3903fd5cf752063dea5e9fd61e8bbf3c4577e11b877c9c"), QStringLiteral("rife_v4.25.onnx"), 11391709},
    {RifeVersion::V4_26, RifeVariant::Full, false, QStringLiteral("fd6b06538898c9a94d20f0fe55febceadbcde5ad64952240733b7275ef4c9d59"), QStringLiteral("rife_v4.26.onnx"), 11391709},
    {RifeVersion::V4_26, RifeVariant::Heavy, false, QStringLiteral("ae45000d804282764554820a665588352ff08025d0b5761023aad52425766928"), QStringLiteral("rife_v4.26_heavy.onnx"), 11508471},
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
        row.modelCode = rifeModelCode(pin.version, pin.variant);
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
        row.modelCode = rifeModelCode(pin.version, pin.variant);
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
