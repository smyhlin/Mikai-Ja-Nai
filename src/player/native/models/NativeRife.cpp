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

// Both verified 3.11.0 platform archives contain these identical graph bytes.
const RifeModelPin kPins[] = {
    {RifeVersion::V4_7, RifeVariant::Full, false, QStringLiteral("51bf5efb8e004c4a07e54ab99ef02de97623db40fe061766f789342dd4c1dbd7"), QStringLiteral("rife_v4.7.onnx"), 10699110},
    {RifeVersion::V4_7, RifeVariant::Full, true, QStringLiteral("c8debbdcea3945377b1e0b8ac7d695204c5807f13133db863e4d1c070d34b597"), QStringLiteral("rife_v4.7_ensemble.onnx"), 10705871},
    {RifeVersion::V4_8, RifeVariant::Full, false, QStringLiteral("7be3240c709598f5a0085ebfdb456d18259f69aca7e5fea4e26c778db091a9ab"), QStringLiteral("rife_v4.8.onnx"), 10699112},
    {RifeVersion::V4_8, RifeVariant::Full, true, QStringLiteral("45751ba166880adb0865d0c6fb0d1282c9cffb2ea127f0459ffb6cd69fe71b50"), QStringLiteral("rife_v4.8_ensemble.onnx"), 10705871},
    {RifeVersion::V4_9, RifeVariant::Full, false, QStringLiteral("3c93ad87c40f661a3e59b398599a5c7905f35d302535192138644bc498e85479"), QStringLiteral("rife_v4.9.onnx"), 10699112},
    {RifeVersion::V4_9, RifeVariant::Full, true, QStringLiteral("8230d28933f4c999b4b7dbb9420bccfc59135660481e25033501464bf96c9547"), QStringLiteral("rife_v4.9_ensemble.onnx"), 10705871},
    {RifeVersion::V4_10, RifeVariant::Full, false, QStringLiteral("c942963b522bd431005c328d242b7c860184cac0a1cb9495af5e6b2878e3cd96"), QStringLiteral("rife_v4.10.onnx"), 10778668},
    {RifeVersion::V4_10, RifeVariant::Full, true, QStringLiteral("a4f74c036ab1255629aabb431379ffd4c8289a34eb88fa3a93310297ec074092"), QStringLiteral("rife_v4.10_ensemble.onnx"), 10785427},
    {RifeVersion::V4_11, RifeVariant::Full, false, QStringLiteral("213b03a3f74182110ca478cc6a293b97d48e50dd1b7875c345dc7fc88b353d07"), QStringLiteral("rife_v4.11.onnx"), 10778668},
    {RifeVersion::V4_11, RifeVariant::Full, true, QStringLiteral("ec9bca1a881c1fcc91e81bee05dca1a2c78f01e78740ec60a0152e4d0dd7e73b"), QStringLiteral("rife_v4.11_ensemble.onnx"), 10785427},
    {RifeVersion::V4_12, RifeVariant::Full, false, QStringLiteral("68a72c25f7bfb99ba7ace42bda1723f1ca1efe686730e9ba3b01d4d86830d1eb"), QStringLiteral("rife_v4.12.onnx"), 10778668},
    {RifeVersion::V4_12, RifeVariant::Lite, false, QStringLiteral("ee57a6e29779f3628b168d485d0f78e78ea5eb30c67ff168a24de8bd58b97a13"), QStringLiteral("rife_v4.12_lite.onnx"), 5313595},
    {RifeVersion::V4_12, RifeVariant::Full, true, QStringLiteral("c215194b75d77257a1a9781c27eb19be21c7c0ead6676e43d705a5d4a24ebcd5"), QStringLiteral("rife_v4.12_ensemble.onnx"), 10785427},
    {RifeVersion::V4_12, RifeVariant::Lite, true, QStringLiteral("64800a7508e67cf8a0532001ac771a5f7f16dc00fc5dd76a335ff24ce566aa9e"), QStringLiteral("rife_v4.12_lite_ensemble.onnx"), 5320354},
    {RifeVersion::V4_13, RifeVariant::Full, false, QStringLiteral("056fa6c1a1d072563fe487d8c51e82ed0ce1343b9d99f7b4dcd0ebc553dca585"), QStringLiteral("rife_v4.13.onnx"), 10778668},
    {RifeVersion::V4_13, RifeVariant::Lite, false, QStringLiteral("825463032e9350ec8283e8500170c8bc7d343961fc2a9c17ae7b4aae97a182e3"), QStringLiteral("rife_v4.13_lite.onnx"), 5313595},
    {RifeVersion::V4_13, RifeVariant::Full, true, QStringLiteral("5952ca741d89c6652afaf048da0cc122c88535ded945efa79cf836e3feee6539"), QStringLiteral("rife_v4.13_ensemble.onnx"), 10785427},
    {RifeVersion::V4_13, RifeVariant::Lite, true, QStringLiteral("59cd5ab14d2dbda36a2cec545c1c84e65226f5006ea6cc027838631aaf7e05f3"), QStringLiteral("rife_v4.13_lite_ensemble.onnx"), 5320354},
    {RifeVersion::V4_14, RifeVariant::Full, false, QStringLiteral("26bd6edb9e08da001700a1faca092cca170b6c5a8dac3fe534b9cdb8455540be"), QStringLiteral("rife_v4.14.onnx"), 10778640},
    {RifeVersion::V4_14, RifeVariant::Lite, false, QStringLiteral("88edb77667bfa741a363ca118d7477ca59a7ed3db60cd15f502d98ea49e359ce"), QStringLiteral("rife_v4.14_lite.onnx"), 5986304},
    {RifeVersion::V4_14, RifeVariant::Full, true, QStringLiteral("2d7716878f75a272083ce7de352b2daacd5c648c377429d0badf8dd005eb0ead"), QStringLiteral("rife_v4.14_ensemble.onnx"), 10785399},
    {RifeVersion::V4_14, RifeVariant::Lite, true, QStringLiteral("d8834c6a34959936c2ece52e80cb49216ce653b158a3b80e7282c7a75cc6620f"), QStringLiteral("rife_v4.14_lite_ensemble.onnx"), 5993063},
    {RifeVersion::V4_15, RifeVariant::Full, false, QStringLiteral("46ecd3b8017f3a3e090c53d1d6369a28ef83b12f050eb563949b9ffe0386df7f"), QStringLiteral("rife_v4.15.onnx"), 10778640},
    {RifeVersion::V4_15, RifeVariant::Lite, false, QStringLiteral("3df7ab5ff549578d8d6c2d1f929eaba501390884cbf11ee6958c19ecf5d60a50"), QStringLiteral("rife_v4.15_lite.onnx"), 5282907},
    {RifeVersion::V4_15, RifeVariant::Full, true, QStringLiteral("9f5caeca327e66330bcd85c6a802525cee08168d0121b28ddc71c3be61e112fc"), QStringLiteral("rife_v4.15_ensemble.onnx"), 10785399},
    {RifeVersion::V4_15, RifeVariant::Lite, true, QStringLiteral("753b7839cf978098f2a00e6ed0654667b45ff9303c792149d8d56dba065d3404"), QStringLiteral("rife_v4.15_lite_ensemble.onnx"), 5289666},
    {RifeVersion::V4_16, RifeVariant::Lite, false, QStringLiteral("bf46120fbabb4365ca20feca200e41b578d22b1710be3279752b1576a87e7388"), QStringLiteral("rife_v4.16_lite.onnx"), 5282907},
    {RifeVersion::V4_16, RifeVariant::Lite, true, QStringLiteral("a83cbe7b4c552293e26c794c0e12ba406a5382676deefa26369d9d81f0d16d57"), QStringLiteral("rife_v4.16_lite_ensemble.onnx"), 5289666},
    {RifeVersion::V4_17, RifeVariant::Full, false, QStringLiteral("ce4fb8308ca9afa84ba3900b46b3254a94cb520ef85e70e05644c0d5111f3396"), QStringLiteral("rife_v4.17.onnx"), 10778640},
    {RifeVersion::V4_17, RifeVariant::Lite, false, QStringLiteral("417d2319c236f4b2d5166436981da92f6b1b2db0d169126bf5cfec660f7443cc"), QStringLiteral("rife_v4.17_lite.onnx"), 5282907},
    {RifeVersion::V4_17, RifeVariant::Full, true, QStringLiteral("3ad369facc0e27fd12216752bf74088d99c4a3eb31b33379d3ef76a9eb35a0e3"), QStringLiteral("rife_v4.17_ensemble.onnx"), 10785399},
    {RifeVersion::V4_17, RifeVariant::Lite, true, QStringLiteral("62a7dcae912b6e0b43342c4f770ffdc9d2bf26b000a77725106fa02523479645"), QStringLiteral("rife_v4.17_lite_ensemble.onnx"), 5289666},
    {RifeVersion::V4_18, RifeVariant::Full, false, QStringLiteral("03682b1ab121970ec529fee54bf1a1d5e5eda0291d7c8b1d87ad25f879d33c6d"), QStringLiteral("rife_v4.18.onnx"), 10778640},
    {RifeVersion::V4_18, RifeVariant::Full, true, QStringLiteral("9bc09d476a685b2f24f3cf5dbf439157e4e5c6d553694e6426772fa63d066850"), QStringLiteral("rife_v4.18_ensemble.onnx"), 10785399},
    {RifeVersion::V4_19, RifeVariant::Full, false, QStringLiteral("eaf46199ecc262e585500967efe335d6f45e5809707fb5750d3b20225aaf95c4"), QStringLiteral("rife_v4.19.onnx"), 10778640},
    {RifeVersion::V4_19, RifeVariant::Full, true, QStringLiteral("b4efe5e6016729d1146f14ae6c5dc1cd7954d4d48d9a0e2ef06623210036bb08"), QStringLiteral("rife_v4.19_ensemble.onnx"), 10785399},
    {RifeVersion::V4_20, RifeVariant::Full, false, QStringLiteral("58255bdfec3dedd22fd48afddeb7c1fa9964fa4efb5a7eb45752b32606528cc5"), QStringLiteral("rife_v4.20.onnx"), 30803041},
    {RifeVersion::V4_20, RifeVariant::Full, true, QStringLiteral("5c32969c1e568d41c8d721d8c28bed792d35fd1e9bb198d5cc5e54a37b9d0dfa"), QStringLiteral("rife_v4.20_ensemble.onnx"), 30809800},
    {RifeVersion::V4_21, RifeVariant::Full, false, QStringLiteral("0ed4d68c41c97bbee497d66eaaa062bc34c353373cea79715854c39c73792578"), QStringLiteral("rife_v4.21.onnx"), 18694909},
    {RifeVersion::V4_22, RifeVariant::Full, false, QStringLiteral("77700a2395ac91d88bf1b886fb144e14cfbca9daf9312f2a460ddd2e7e0d0f8e"), QStringLiteral("rife_v4.22.onnx"), 18694909},
    {RifeVersion::V4_22, RifeVariant::Lite, false, QStringLiteral("a1e22fb7e6efdfdbdf1179c7f203428a1d808083479f0b23fb923ed6467b3995"), QStringLiteral("rife_v4.22_lite.onnx"), 9782154},
    {RifeVersion::V4_23, RifeVariant::Full, false, QStringLiteral("f3430c9fa59e796edac32996dbc5f8a9101afe80f7f9baad541598f54fbd3ea9"), QStringLiteral("rife_v4.23.onnx"), 18694909},
    {RifeVersion::V4_24, RifeVariant::Full, false, QStringLiteral("453f51021aa427d5ee7c5e90dce34c1b4b9c285e9e6febf7e4a08ee4bdb8c9ee"), QStringLiteral("rife_v4.24.onnx"), 10778640},
    {RifeVersion::V4_24, RifeVariant::Full, true, QStringLiteral("2449ce05ad8d96b5b585f6863fb77a7e144f99ace455e879bd532999bceefb7e"), QStringLiteral("rife_v4.24_ensemble.onnx"), 10785399},
    {RifeVersion::V4_25, RifeVariant::Full, false, QStringLiteral("137b84ddb2eb7ef814afd418a2d0e681395a401372743b2d192436a03b758216"), QStringLiteral("rife_v4.25.onnx"), 11391702},
    {RifeVersion::V4_26, RifeVariant::Full, false, QStringLiteral("3a683f91682a46021f715d7fdf3da46e0a37ee933c176efb0175728aafc977d0"), QStringLiteral("rife_v4.26.onnx"), 11391702},
    {RifeVersion::V4_26, RifeVariant::Heavy, false, QStringLiteral("418351da11dbb2ea39309fca0f444356e2a403e3854928d119777f9ac772c72f"), QStringLiteral("rife_v4.26_heavy.onnx"), 11508464},
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
