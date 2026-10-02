#include "TestNativeRife.hpp"

#include "native/NativeRecipe.hpp"
#include "native/NativeRife.hpp"
#include "native/NativeUserRecipes.hpp"

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
                         TemporalOrder order, double threshold) {
    TemporalStage stage;
    stage.version = RifeVersion::V4_14;
    stage.variant = variant;
    stage.ensemble = ensemble;
    stage.factor = factor;
    stage.order = order;
    stage.sceneThreshold = threshold;
    return stage;
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
