#include "TestNativeRife.hpp"

#include "native/models/NativeModelInventory.hpp"
#include "native/models/NativeRecipe.hpp"
#include "native/models/NativeRife.hpp"
#include "native/models/NativeUserRecipes.hpp"

#include <QtCore/QSet>
#include <QtCore/QSize>

using namespace mplayer::native;

namespace {

SpatialStage spatialStage(const QString& modelId, const QSize& preResize = QSize()) {
    SpatialStage stage;
    stage.modelId = modelId;
    if (preResize.isValid()) {
        stage.preResize = preResize;
        stage.preResizeEnabled = true;
    }
    return stage;
}

/// A real spatial-only base: two ordered stages, the first with a pre-resize.
Recipe spatialBase() {
    Recipe recipe;
    recipe.id = QStringLiteral("base.spatial.v1");
    recipe.label = QStringLiteral("Spatial base");
    recipe.backend = Backend::TensorRT;
    recipe.spatial = {spatialStage(QStringLiteral("builtin.hd"), QSize(960, 540)),
                      spatialStage(QStringLiteral("builtin.sd"))};
    return recipe;
}

TemporalStage temporalOf(RifeVariant variant, bool ensemble, const Rational& factor,
                         TemporalOrder order, double threshold,
                         RifeVersion version = RifeVersion::V4_14) {
    TemporalStage stage;
    stage.version = version;
    stage.variant = variant;
    stage.ensemble = ensemble;
    stage.factor = factor;
    stage.order = order;
    stage.sceneThreshold = threshold;
    return stage;
}

TensorProfile dynamicTensor(qint64 channels) {
    TensorProfile tensor;
    tensor.dtype = QString::fromLatin1(kEligibleIoType);
    tensor.rank = 4;
    tensor.batch = 1;
    tensor.channels = channels;
    return tensor;
}

ModelProfile temporalProfile() {
    ModelProfile profile;
    profile.input = dynamicTensor(11);
    profile.output = dynamicTensor(3);
    return profile;
}

TensorProfile fixedTensor180x320(qint64 channels) {
    TensorProfile tensor;
    tensor.dtype = QString::fromLatin1(kEligibleIoType);
    tensor.rank = 4;
    tensor.batch = 1;
    tensor.channels = channels;
    tensor.height = 180;
    tensor.width = 320;
    return tensor;
}

ModelProfile temporalReference() {
    ModelProfile profile;
    profile.input = fixedTensor180x320(11);
    profile.output = fixedTensor180x320(3);
    return profile;
}

/// The owned temporal row the curated variant's pin resolves to.
ModelRecord pinnedRow(RifeVariant variant, bool ensemble,
                      RifeVersion version = RifeVersion::V4_14) {
    const RifeModelPin* pin = rifeModelPin(version, variant, ensemble);
    Q_ASSERT(pin != nullptr);
    ModelRecord record;
    record.id = userModelIdFor(
        computeContentId(ModelRole::Temporal, pin->graphSha256, temporalProfile()));
    record.displayName = rifeModelName(RifeVersion::V4_14, variant, ensemble);
    record.origin = ModelOrigin::UserImported;
    record.onnxPath = QStringLiteral("C:/models/%1.onnx").arg(record.id);
    record.onnxSha256 = pin->graphSha256;
    record.onnxBytes = 4096;
    record.role = ModelRole::Temporal;
    record.actualProfile = temporalProfile();
    record.verifiedReferenceProfile = temporalReference();
    record.availableBackends = {Backend::TensorRT};
    record.availability = readyAvailability();
    return record;
}

CatalogView catalogOf(const QVector<ModelRecord>& records) {
    CatalogView view;
    view.models = records;
    return view;
}

}  // namespace

void TestNativeRife::unknownRifeVariantsAreRejected() {
    QVERIFY(findRifeCapability(QStringLiteral("rife_v9.9")) == nullptr);
    QVERIFY(findRifeCapability(QStringLiteral("rife_v4.14_full")) == nullptr);
}

void TestNativeRife::requestedVariantsResolveTheExactOriginalModelNames() {
    struct VariantCase {
        RifeVariant variant;
        bool ensemble;
        const char* modelId;
    };
    const VariantCase cases[] = {
        {RifeVariant::Full, false, "rife_v4.14"},
        {RifeVariant::Lite, false, "rife_v4.14_lite"},
        {RifeVariant::Full, true, "rife_v4.14_ensemble"},
        {RifeVariant::Lite, true, "rife_v4.14_lite_ensemble"},
    };
    for (const VariantCase& value : cases) {
        RifeRequest request;
        request.temporal = temporalOf(value.variant, value.ensemble, Rational{3, 2},
                                      TemporalOrder::BeforeSpatial, 0.15);
        const std::optional<Recipe> recipe = buildRifeRecipe(request);
        QVERIFY(recipe.has_value());
        QCOMPARE(recipe->temporal->modelId(), QString::fromLatin1(value.modelId));
    }
}

void TestNativeRife::aRifeOnlyRequestBuildsARifeOnlyRecipe() {
    RifeRequest request;
    request.temporal = temporalOf(RifeVariant::Lite, true, Rational{3, 2},
                                  TemporalOrder::BeforeSpatial, 0.4);
    request.backend = Backend::DirectML;
    request.label = QStringLiteral("Requested");

    const std::optional<Recipe> recipe = buildRifeRecipe(request);
    QVERIFY(recipe.has_value());
    QCOMPARE(recipe->shape(), RecipeShape::RifeOnly);
    QVERIFY(recipe->spatial.isEmpty());
    QVERIFY(recipe->temporal.has_value());
    QCOMPARE(recipe->backend, Backend::DirectML);
    QCOMPARE(recipe->temporal->variant, RifeVariant::Lite);
    QCOMPARE(recipe->temporal->ensemble, true);
    QCOMPARE(recipe->temporal->factor.num, 3);
    QCOMPARE(recipe->temporal->factor.den, 2);
    QCOMPARE(recipe->temporal->order, TemporalOrder::BeforeSpatial);
    QCOMPARE(recipe->temporal->sceneThreshold, 0.4);
}

void TestNativeRife::theFactoryPreservesTheExactRationalFactorAndOrder() {
    for (const TemporalOrder order : {TemporalOrder::BeforeSpatial, TemporalOrder::AfterSpatial}) {
        RifeRequest request;
        request.temporal = temporalOf(RifeVariant::Full, false, Rational{4, 2}, order, 0.150);
        const std::optional<Recipe> recipe = buildRifeRecipe(request);
        QVERIFY(recipe.has_value());
        // 4/2 and 2/1 are the same frame rate but only one of them is the
        // request: reduction is not applied on the way in.
        QCOMPARE(recipe->temporal->factor.num, 4);
        QCOMPARE(recipe->temporal->factor.den, 2);
        QVERIFY(!recipe->temporal->factor.isCanonical());
        QCOMPARE(recipe->temporal->order, order);
    }
}

void TestNativeRife::aCombinedRequestPreservesTheOrderedSpatialChainAndItsResizes() {
    RifeRequest request;
    request.temporal = temporalOf(RifeVariant::Full, false, Rational{2, 1},
                                  TemporalOrder::AfterSpatial, 0.150);
    request.base = spatialBase();

    const std::optional<Recipe> recipe = buildRifeRecipe(request);
    QVERIFY(recipe.has_value());
    QCOMPARE(recipe->shape(), RecipeShape::Combined);
    QCOMPARE(recipe->spatial.size(), 2);
    QCOMPARE(recipe->spatial.at(0).modelId, QStringLiteral("builtin.hd"));
    QVERIFY(recipe->spatial.at(0).preResizeEnabled);
    QCOMPARE(recipe->spatial.at(0).preResize, QSize(960, 540));
    QCOMPARE(recipe->spatial.at(1).modelId, QStringLiteral("builtin.sd"));
    QVERIFY(!recipe->spatial.at(1).preResizeEnabled);
}

void TestNativeRife::theBackendIsTheExplicitOneNeverInferredFromTheBase() {
    Recipe base = spatialBase();
    base.backend = Backend::TensorRT;

    RifeRequest request;
    request.temporal = temporalOf(RifeVariant::Full, false, Rational{2, 1},
                                  TemporalOrder::AfterSpatial, 0.150);
    request.backend = Backend::DirectML;
    request.base = base;

    const std::optional<Recipe> recipe = buildRifeRecipe(request);
    QVERIFY(recipe.has_value());
    QCOMPARE(recipe->backend, Backend::DirectML);
}

void TestNativeRife::aCombinedRequestRoundTripsThroughItsDocumentUnchanged() {
    RifeRequest request;
    request.temporal = temporalOf(RifeVariant::Lite, true, Rational{3, 2},
                                  TemporalOrder::BeforeSpatial, 0.275);
    request.backend = Backend::TensorRT;
    request.base = spatialBase();
    request.label = QStringLiteral("Round trip");

    const std::optional<Recipe> recipe = buildRifeRecipe(request);
    QVERIFY(recipe.has_value());

    const QByteArray payload = serializeRecipe(*recipe);
    Recipe parsed;
    QString errorKey;
    QString error;
    QVERIFY2(parseRecipe(payload, &parsed, &errorKey, &error), qPrintable(error));
    QCOMPARE(parsed.shape(), RecipeShape::Combined);
    QCOMPARE(parsed.id, recipe->id);
    QCOMPARE(parsed.backend, Backend::TensorRT);
    QCOMPARE(parsed.spatial.size(), 2);
    QCOMPARE(parsed.spatial.at(0).modelId, QStringLiteral("builtin.hd"));
    QCOMPARE(parsed.spatial.at(0).preResize, QSize(960, 540));
    QVERIFY(parsed.temporal.has_value());
    QCOMPARE(parsed.temporal->variant, RifeVariant::Lite);
    QCOMPARE(parsed.temporal->ensemble, true);
    QCOMPARE(parsed.temporal->factor.num, 3);
    QCOMPARE(parsed.temporal->factor.den, 2);
    QCOMPARE(parsed.temporal->order, TemporalOrder::BeforeSpatial);
    QCOMPARE(parsed.temporal->sceneThreshold, 0.275);
}

void TestNativeRife::combiningWithANonSpatialBaseIsRefusedByName() {
    Recipe rifeOnly;
    rifeOnly.spatial = {};
    rifeOnly.temporal = temporalOf(RifeVariant::Full, false, Rational{2, 1},
                                   TemporalOrder::AfterSpatial, 0.150);

    RifeRequest request;
    request.temporal = temporalOf(RifeVariant::Full, false, Rational{2, 1},
                                  TemporalOrder::AfterSpatial, 0.150);
    request.base = rifeOnly;
    QString key;
    QString reason;
    QVERIFY(!buildRifeRecipe(request, &key, &reason).has_value());
    QCOMPARE(key, QStringLiteral("rife.base.shape"));
    QVERIFY(!reason.isEmpty());

    Recipe combined = spatialBase();
    combined.temporal = rifeOnly.temporal;
    request.base = combined;
    QVERIFY(!buildRifeRecipe(request, &key, &reason).has_value());
    QCOMPARE(key, QStringLiteral("rife.base.shape"));
}

void TestNativeRife::anEmptyBaseIsRefusedRatherThanTreatedAsRifeOnly() {
    RifeRequest request;
    request.temporal = temporalOf(RifeVariant::Full, false, Rational{2, 1},
                                  TemporalOrder::AfterSpatial, 0.150);
    request.base = Recipe{};
    QString key;
    QString reason;
    QVERIFY(!buildRifeRecipe(request, &key, &reason).has_value());
    QCOMPARE(key, QStringLiteral("rife.base.shape"));
    QVERIFY(!reason.isEmpty());
}

void TestNativeRife::aMalformedFactorIsRefusedByName() {
    for (const Rational factor : {Rational{0, 1}, Rational{2, 0}, Rational{-2, 1}}) {
        RifeRequest request;
        request.temporal = temporalOf(RifeVariant::Full, false, factor,
                                      TemporalOrder::AfterSpatial, 0.150);
        QString key;
        QString reason;
        QVERIFY(!buildRifeRecipe(request, &key, &reason).has_value());
        QCOMPARE(key, QStringLiteral("stage.temporal.factor.invalid"));
        QVERIFY(!reason.isEmpty());
    }
}

void TestNativeRife::anOutOfRangeThresholdIsRefusedByName() {
    for (const double threshold : {1.5, -0.1}) {
        RifeRequest request;
        request.temporal = temporalOf(RifeVariant::Full, false, Rational{2, 1},
                                      TemporalOrder::AfterSpatial, threshold);
        QString key;
        QString reason;
        QVERIFY(!buildRifeRecipe(request, &key, &reason).has_value());
        QCOMPARE(key, QStringLiteral("stage.temporal.threshold.invalid"));
        QVERIFY(!reason.isEmpty());
    }
}

void TestNativeRife::theLabelNeverChangesTheRecipeIdentity() {
    RifeRequest first;
    first.temporal = temporalOf(RifeVariant::Lite, false, Rational{3, 2},
                                TemporalOrder::BeforeSpatial, 0.15);
    first.label = QStringLiteral("One label");
    RifeRequest second = first;
    second.label = QStringLiteral("Another label");

    const std::optional<Recipe> a = buildRifeRecipe(first);
    const std::optional<Recipe> b = buildRifeRecipe(second);
    QVERIFY(a.has_value());
    QVERIFY(b.has_value());
    QCOMPARE(a->id, b->id);
    QCOMPARE(a->id, storedRecipeIdFor(*a));
}

void TestNativeRife::allPinnedGraphsHaveDistinctExactDigests() {
    const QVector<RifeModelPin>& pins = rifeModelPins();
    QCOMPARE(pins.size(), 47);

    QSet<QString> digests;
    QSet<QString> names;
    for (const RifeModelPin& pin : pins) {
        QCOMPARE(pin.graphSha256.size(), 64);
        QVERIFY(!digests.contains(pin.graphSha256.toLower()));
        digests.insert(pin.graphSha256.toLower());
        const QString name = rifeModelName(pin.version, pin.variant, pin.ensemble);
        QVERIFY(!names.contains(name));
        names.insert(name);

        // Both lookups are the pin's own identity, round-tripping exactly.
        const RifeModelPin* byName = rifeModelPinForName(name);
        QVERIFY(byName != nullptr);
        QCOMPARE(byName->graphSha256, pin.graphSha256);
        const RifeModelPin* byDigest = rifeModelPinForGraphSha(pin.graphSha256.toUpper());
        QVERIFY(byDigest != nullptr);
        QCOMPARE(rifeModelName(byDigest->version, byDigest->variant, byDigest->ensemble), name);
    }
    QCOMPARE(pins.size(), 47);
    QCOMPARE(digests.size(), 47);
    QCOMPARE(names.size(), 47);
    QVERIFY(rifeModelPin(RifeVersion::V4_14, RifeVariant::Lite, true) != nullptr);
    QCOMPARE(rifeModelPin(RifeVersion::V4_14, RifeVariant::Lite, true)->graphSha256,
             QStringLiteral("9486d87ca17a83a1594ac54b254b6fb1be6fa52f5dd11a31e4a602d124ea8b1b"));
    QCOMPARE(rifeModelName(RifeVersion::V4_7, RifeVariant::Full, false),
             QStringLiteral("rife_v4.7"));
    QCOMPARE(rifeModelName(RifeVersion::V4_14, RifeVariant::Lite, true),
             QStringLiteral("rife_v4.14_lite_ensemble"));
    QCOMPARE(rifeModelName(RifeVersion::V4_26, RifeVariant::Heavy, false),
             QStringLiteral("rife_v4.26_heavy"));
    QVERIFY(rifeModelPinForName(QStringLiteral("rife_v4.14_full")) == nullptr);
}

void TestNativeRife::thePinnedOwnedRowResolvesToItsContentIdentity() {
    const ModelRecord lite = pinnedRow(RifeVariant::Lite, true);
    TemporalStage stage = temporalOf(RifeVariant::Lite, true, Rational{2, 1},
                                     TemporalOrder::AfterSpatial, 0.15);

    const CatalogView catalog = catalogOf({lite});
    const RifeModelResolution resolution = resolveRifeModel(stage, catalog);
    QVERIFY(resolution.resolved());
    QVERIFY(resolution.pin != nullptr);
    QCOMPARE(resolution.record->id, lite.id);
    QCOMPARE(resolution.modelId(), lite.id);
    QCOMPARE(resolution.contentId(), lite.contentId());
    QCOMPARE(resolution.modelId(),
             userModelIdFor(computeContentId(ModelRole::Temporal, lite.onnxSha256,
                                             lite.actualProfile)));
    QVERIFY(resolution.modelId() != QStringLiteral("rife_v4.14_lite_ensemble"));

    // The capability table reports the pinned row as offered, joined to its own id.
    const QVector<RifeCapability> rows = rifeCapabilities(catalog);
    QCOMPARE(rows.size(), 47);
    for (const RifeCapability& row : rows) {
        if (row.version == RifeVersion::V4_14 && row.variant == RifeVariant::Lite &&
            row.ensemble) {
            QVERIFY(row.availability.isUsable());
            QCOMPARE(row.resolvedModelId, lite.id);
        } else {
            QVERIFY(!row.availability.isUsable());
            QVERIFY(!row.availability.reason.isEmpty());
        }
    }
}

void TestNativeRife::thePinnedHeavyGraphResolvesOnlyWithOwnedProof() {
    const RifeVersion version = RifeVersion::V4_26;
    const RifeModelPin* pin = rifeModelPin(version, RifeVariant::Heavy, false);
    QVERIFY(pin != nullptr);
    QCOMPARE(pin->fileName, QStringLiteral("rife_v4.26_heavy.onnx"));

    const RifeCapability* unresolved =
        findRifeCapability(version, RifeVariant::Heavy, false);
    QVERIFY(unresolved != nullptr);
    QVERIFY(!unresolved->availability.isUsable());
    QCOMPARE(unresolved->availability.reasonKey, QStringLiteral("rife.capability.unresolved"));

    const TemporalStage stage = temporalOf(RifeVariant::Heavy, false, Rational{2, 1},
                                            TemporalOrder::AfterSpatial, 0.15, version);
    const ModelRecord heavy = pinnedRow(RifeVariant::Heavy, false, version);
    QVERIFY(heavy.supportsBackend(Backend::TensorRT));
    QCOMPARE(heavy.onnxSha256, pin->graphSha256);
    QCOMPARE(heavy.role, ModelRole::Temporal);
    QCOMPARE(temporalCandidateRefusal(heavy.actualProfile), QString());
    QVERIFY(heavy.verifiedReferenceProfile.has_value());
    QCOMPARE(temporalReferenceRefusal(heavy.actualProfile,
                                      *heavy.verifiedReferenceProfile),
             QString());

    // The resolution points into the supplied catalog; keep that owner alive while
    // asserting the resolved owned row and its content identity.
    const CatalogView heavyCatalog = catalogOf({heavy});
    RifeModelResolution resolution = resolveRifeModel(stage, heavyCatalog);
    QVERIFY2(resolution.resolved(), qPrintable(resolution.reason));
    QCOMPARE(resolution.record->id, heavy.id);
    QCOMPARE(resolution.modelId(),
             userModelIdFor(computeContentId(ModelRole::Temporal, pin->graphSha256,
                                             heavy.actualProfile)));
    QCOMPARE(resolution.contentId(), heavy.contentId());

    // Neither absence nor a different graph can stand in for the exact pin.
    const CatalogView empty;
    resolution = resolveRifeModel(stage, empty);
    QVERIFY(!resolution.resolved());
    QCOMPARE(resolution.reasonKey, QStringLiteral("rife.model.missing"));
    ModelRecord wrongGraph = heavy;
    wrongGraph.onnxSha256 = QStringLiteral("0123456789abcdef0123456789abcdef"
                                           "0123456789abcdef0123456789abcdef");
    wrongGraph.id = userModelIdFor(
        computeContentId(ModelRole::Temporal, wrongGraph.onnxSha256,
                         wrongGraph.actualProfile));
    resolution = resolveRifeModel(stage, catalogOf({wrongGraph}));
    QVERIFY(!resolution.resolved());
    QCOMPARE(resolution.reasonKey, QStringLiteral("rife.model.missing"));

    // The inspected temporal boundary and CPU-reference run remain required.
    ModelRecord wrongBoundary = heavy;
    wrongBoundary.actualProfile.input.channels = 3;
    wrongBoundary.actualProfile.output.channels = 3;
    wrongBoundary.id = userModelIdFor(
        computeContentId(ModelRole::Temporal, wrongBoundary.onnxSha256,
                         wrongBoundary.actualProfile));
    resolution = resolveRifeModel(stage, catalogOf({wrongBoundary}));
    QVERIFY(!resolution.resolved());
    QCOMPARE(resolution.reasonKey, QStringLiteral("rife.model.boundary"));
    ModelRecord unproved = heavy;
    unproved.verifiedReferenceProfile.reset();
    resolution = resolveRifeModel(stage, catalogOf({unproved}));
    QVERIFY(!resolution.resolved());
    QCOMPARE(resolution.reasonKey, QStringLiteral("rife.model.reference"));

    // Heavy ensemble has no exact graph pin and is refused by identity.
    resolution = resolveRifeModel(version, RifeVariant::Heavy, true,
                                  catalogOf({heavy}));
    QVERIFY(!resolution.resolved());
    QVERIFY(resolution.pin == nullptr);
    QCOMPARE(resolution.reasonKey, QStringLiteral("rife.pin.unknown"));
    QVERIFY(resolution.reason.contains(QStringLiteral("rife_v4.26_heavy_ensemble")));
}

void TestNativeRife::aSubstitutedOrWrongRowIsRefusedByItsOwnReason() {
    TemporalStage stage = temporalOf(RifeVariant::Full, false, Rational{2, 1},
                                     TemporalOrder::AfterSpatial, 0.15);

    // An empty store names the missing pinned graph.
    const CatalogView empty;
    RifeModelResolution resolution = resolveRifeModel(stage, empty);
    QVERIFY(!resolution.resolved());
    QCOMPARE(resolution.reasonKey, QStringLiteral("rife.model.missing"));

    // A row that claims the curated name but carries another graph is not it.
    ModelRecord alias = pinnedRow(RifeVariant::Full, false);
    alias.onnxSha256 = QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef"
                                      "0123456789abcdef");
    alias.id = userModelIdFor(
        computeContentId(ModelRole::Temporal, alias.onnxSha256, alias.actualProfile));
    resolution = resolveRifeModel(stage, catalogOf({alias}));
    QVERIFY(!resolution.resolved());
    QCOMPARE(resolution.reasonKey, QStringLiteral("rife.model.missing"));

    // The pin's digest carried by a non-temporal row is a wrong role.
    ModelRecord spatial = pinnedRow(RifeVariant::Full, false);
    spatial.role = ModelRole::Spatial;
    resolution = resolveRifeModel(stage, catalogOf({spatial}));
    QVERIFY(!resolution.resolved());
    QCOMPARE(resolution.reasonKey, QStringLiteral("rife.model.role"));

    // A temporal row that declares the wrong boundary is refused by name.
    ModelRecord wrongBoundary = pinnedRow(RifeVariant::Full, false);
    wrongBoundary.actualProfile.input.channels = 3;
    wrongBoundary.actualProfile.output.channels = 3;
    wrongBoundary.id = userModelIdFor(computeContentId(ModelRole::Temporal,
                                                       wrongBoundary.onnxSha256,
                                                       wrongBoundary.actualProfile));
    resolution = resolveRifeModel(stage, catalogOf({wrongBoundary}));
    QVERIFY(!resolution.resolved());
    QCOMPARE(resolution.reasonKey, QStringLiteral("rife.model.boundary"));

    // A declared boundary with no CPU-reference proof is not resolved.
    ModelRecord unproved = pinnedRow(RifeVariant::Full, false);
    unproved.verifiedReferenceProfile.reset();
    resolution = resolveRifeModel(stage, catalogOf({unproved}));
    QVERIFY(!resolution.resolved());
    QCOMPARE(resolution.reasonKey, QStringLiteral("rife.model.reference"));

    // A corrupt/unready row keeps the store's own reason rather than being run.
    ModelRecord corrupt = pinnedRow(RifeVariant::Full, false);
    corrupt.availability = unavailableAvailability(
        QStringLiteral("model.store.corrupt"),
        QStringLiteral("the owned copy no longer matches the row's identity"));
    resolution = resolveRifeModel(stage, catalogOf({corrupt}));
    QVERIFY(!resolution.resolved());
    QCOMPARE(resolution.reasonKey, QStringLiteral("model.store.corrupt"));
}
