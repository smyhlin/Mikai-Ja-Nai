#include "TestNativeRecipe.hpp"

#include "native/NativeRecipe.hpp"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSize>

using namespace mplayer::native;

namespace {

constexpr const char* kHdBalancedSha =
    "b308e93c3dda3c4f9f968295b2b7b8d1c0583a711bca53a08d51134d9d1cbd5c";
constexpr const char* kHdPerformanceSha =
    "7b804d87320f37b7269d6e27796046fbd975e95e2aa1f65d5557f52a36282716";
constexpr const char* kSdCompactSha =
    "ded46591ddfaeb22b3daaf84ad7b36838335aa3efca66acfa36177ce4ab21eb5";

TensorProfile spatialTensor(qint64 h, qint64 w) {
  TensorProfile profile;
  profile.dtype = QStringLiteral("fp16");
  profile.rank = 4;
  profile.batch = 1;
  profile.channels = 3;
  profile.height = h;
  profile.width = w;
  return profile;
}

ModelProfile upscale2x(qint64 h, qint64 w) {
  ModelProfile profile;
  profile.input = spatialTensor(h, w);
  profile.output = spatialTensor(h * 2, w * 2);
  return profile;
}

ModelRecord spatialRow(const QString& id, const ModelProfile& profile,
                       const QString& sha = QString::fromLatin1(kHdBalancedSha)) {
  ModelRecord record;
  record.id = id;
  record.displayName = id;
  record.origin = ModelOrigin::Builtin;
  record.onnxPath = QStringLiteral("C:/models/%1.onnx").arg(id);
  record.onnxSha256 = sha;
  record.role = ModelRole::Spatial;
  record.actualProfile = profile;
  record.availableBackends = {Backend::TensorRT, Backend::DirectML};
  record.availability = readyAvailability();
  return record;
}

/// A temporal row declares a different boundary from a spatial one, which is
/// exactly why the spatial NCHW rules must not be applied to it.
ModelRecord rifeRow(RifeVariant variant, bool ensemble) {
  ModelRecord record;
  record.id = rifeModelName(RifeVersion::V4_14, variant, ensemble);
  record.displayName = record.id;
  record.origin = ModelOrigin::Builtin;
  record.onnxPath = QStringLiteral("C:/models/%1.pkl").arg(record.id);
  record.onnxSha256 = QString::fromLatin1(kSdCompactSha);
  record.role = ModelRole::Temporal;
  record.actualProfile = ModelProfile{};
  record.availableBackends = {Backend::TensorRT, Backend::DirectML};
  record.availability = readyAvailability();
  return record;
}

SpatialStage stage(const QString& modelId) {
  SpatialStage result;
  result.modelId = modelId;
  return result;
}

TemporalStage temporal(TemporalOrder order, Rational factor = Rational(2, 1)) {
  TemporalStage result;
  result.factor = factor;
  result.order = order;
  result.sceneThreshold = 0.150;
  return result;
}

Recipe spatialOnlyRecipe() {
  Recipe recipe;
  recipe.id = QStringLiteral("janai.spatial.hd2x.v1");
  recipe.label = QStringLiteral("HD 2x");
  recipe.spatial = {stage(QStringLiteral("builtin.hd.balanced"))};
  return recipe;
}

Recipe rifeOnlyRecipe() {
  Recipe recipe;
  recipe.id = QStringLiteral("janai.rife.2x.v1");
  recipe.label = QStringLiteral("RIFE 2x");
  recipe.temporal = temporal(TemporalOrder::AfterSpatial);
  return recipe;
}

Recipe combinedRecipe(TemporalOrder order) {
  Recipe recipe;
  recipe.id = QStringLiteral("janai.combined.v1");
  recipe.label = QStringLiteral("Balanced + RIFE 2x");
  recipe.spatial = {stage(QStringLiteral("builtin.hd.balanced"))};
  recipe.temporal = temporal(order);
  return recipe;
}

/// Exactly what the recipes above name, and nothing else.
CatalogView fullCatalog() {
  CatalogView catalog;
  catalog.models = {
      spatialRow(QStringLiteral("builtin.hd.balanced"), upscale2x(1080, 1920),
                 QString::fromLatin1(kHdBalancedSha)),
      spatialRow(QStringLiteral("builtin.hd.performance"), upscale2x(1080, 1920),
                 QString::fromLatin1(kHdPerformanceSha)),
      spatialRow(QStringLiteral("builtin.sd.compact"), upscale2x(720, 1280),
                 QString::fromLatin1(kSdCompactSha)),
      rifeRow(RifeVariant::Full, false),
      rifeRow(RifeVariant::Lite, false),
      rifeRow(RifeVariant::Full, true),
      rifeRow(RifeVariant::Lite, true),
  };
  return catalog;
}

BackendAvailability bothBackends() {
  return BackendAvailability{{Backend::TensorRT, Backend::DirectML}};
}

/// Parses a serialized recipe, asserting success and reporting the error text.
Recipe roundTrip(const Recipe& original) {
  Recipe parsed;
  QString key;
  QString error;
  const bool ok = parseRecipe(serializeRecipe(original), &parsed, &key, &error);
  if (!ok) {
    qWarning("parseRecipe failed: %s (%s)", qPrintable(key), qPrintable(error));
  }
  return ok ? parsed : Recipe{};
}

/// The stable reason key a document was refused with. QVERIFY cannot be used in
/// a helper returning a value - it expands to a bare `return` - so the failure is
/// reported explicitly.
QString refusalKey(const QJsonObject& document) {
  Recipe parsed;
  QString key;
  QString error;
  const bool ok = parseRecipe(document, &parsed, &key, &error);
  if (ok || key.isEmpty() || error.isEmpty() || !parsed.id.isEmpty()) {
    qWarning("expected a refusal with a reason, got ok=%d key=%s error=%s", ok ? 1 : 0,
             qPrintable(key), qPrintable(error));
    return QStringLiteral("<unexpected success or empty reason>");
  }
  return key;
}

/// Asserts two recipes agree on every field, including stage order.
void compareRecipes(const Recipe& actual, const Recipe& expected) {
  QCOMPARE(actual.id, expected.id);
  QCOMPARE(actual.label, expected.label);
  QCOMPARE(actual.backend, expected.backend);
  QCOMPARE(actual.shape(), expected.shape());
  QCOMPARE(actual.spatial.size(), expected.spatial.size());
  for (int i = 0; i < expected.spatial.size(); ++i) {
    const SpatialStage& a = actual.spatial.at(i);
    const SpatialStage& b = expected.spatial.at(i);
    QCOMPARE(a.modelId, b.modelId);
    QCOMPARE(a.preResizeEnabled, b.preResizeEnabled);
    QCOMPARE(a.preResize, b.preResize);
  }
  QCOMPARE(actual.temporal.has_value(), expected.temporal.has_value());
  if (expected.temporal.has_value() && actual.temporal.has_value()) {
    const TemporalStage& a = *actual.temporal;
    const TemporalStage& b = *expected.temporal;
    QCOMPARE(a.version, b.version);
    QCOMPARE(a.variant, b.variant);
    QCOMPARE(a.ensemble, b.ensemble);
    QCOMPARE(a.modelId(), b.modelId());
    // Compared as written, not as reduced: see the rational test below.
    QCOMPARE(a.factor.num, b.factor.num);
    QCOMPARE(a.factor.den, b.factor.den);
    QCOMPARE(a.order, b.order);
    QCOMPARE(a.sceneThreshold, b.sceneThreshold);
  }
}

}  // namespace

// ---------------------------------------------------------------- shapes

void TestNativeRecipe::aSpatialOnlyRecipeIsLegal() {
  const Recipe recipe = spatialOnlyRecipe();
  QCOMPARE(recipe.shape(), RecipeShape::SpatialOnly);
  QVERIFY(recipe.isValidShape());
  QVERIFY(validateRecipe(recipe).ok);
  QVERIFY(!recipe.temporal.has_value());
}

void TestNativeRecipe::aRifeOnlyRecipeIsLegalAndDistinctFromAnEmptySpatialChain() {
  const Recipe rife = rifeOnlyRecipe();
  QCOMPARE(rife.shape(), RecipeShape::RifeOnly);
  QVERIFY(rife.spatial.isEmpty());
  QVERIFY(rife.temporal.has_value());
  QVERIFY(validateRecipe(rife).ok);

  // "Temporal only" and "spatial configured to zero stages" are different
  // things, and only the first is a recipe. The filter's passthrough default
  // would make the other a silent no-op while the UI still showed native on.
  Recipe spatialZero;
  spatialZero.id = QStringLiteral("janai.spatial.zero.v1");
  QCOMPARE(spatialZero.shape(), RecipeShape::Empty);
  QVERIFY(!spatialZero.isValidShape());

  const RecipeVerdict verdict = validateRecipe(spatialZero);
  QVERIFY(!verdict.ok);
  QCOMPARE(verdict.reasonKey, QStringLiteral("recipe.empty"));
  QVERIFY(!verdict.reason.isEmpty());
}

void TestNativeRecipe::aCombinedRecipeIsLegalWithBothOrders() {
  const Recipe before = combinedRecipe(TemporalOrder::BeforeSpatial);
  const Recipe after = combinedRecipe(TemporalOrder::AfterSpatial);
  QCOMPARE(before.shape(), RecipeShape::Combined);
  QCOMPARE(after.shape(), RecipeShape::Combined);
  QVERIFY(validateRecipe(before).ok);
  QVERIFY(validateRecipe(after).ok);
  QVERIFY(before.temporal->order != after.temporal->order);

  // Both halves are really present, so "combined" is not a label on one of the
  // single-stage shapes.
  QCOMPARE(before.spatial.size(), 1);
  QVERIFY(before.temporal.has_value());
}

void TestNativeRecipe::anEmptySpatialChainWithNoTemporalIsRejected() {
  Recipe recipe;
  recipe.id = QStringLiteral("janai.empty.v1");
  QCOMPARE(validateRecipe(recipe).reasonKey, QStringLiteral("recipe.empty"));

  // Neither an id nor stages: refused for the missing id first.
  QCOMPARE(validateRecipe(Recipe{}).reasonKey, QStringLiteral("recipe.id.missing"));

  Recipe blankId;
  blankId.id = QStringLiteral("has space");
  blankId.spatial = {stage(QStringLiteral("builtin.hd.balanced"))};
  QCOMPARE(validateRecipe(blankId).reasonKey, QStringLiteral("recipe.id.invalid"));

  Recipe noModel;
  noModel.id = QStringLiteral("janai.v1");
  noModel.spatial = {SpatialStage{}};
  QCOMPARE(validateRecipe(noModel).reasonKey, QStringLiteral("stage.model.missing"));

  // An enabled pre-resize with a non-positive side is a refusal, not a clamp.
  Recipe badGeometry;
  badGeometry.id = QStringLiteral("janai.geometry.v1");
  SpatialStage badStage = stage(QStringLiteral("builtin.hd.balanced"));
  badStage.preResize = QSize(0, 1080);
  badStage.preResizeEnabled = true;
  badGeometry.spatial = {badStage};
  QCOMPARE(validateRecipe(badGeometry).reasonKey, QStringLiteral("stage.preresize.invalid"));
}

// ---------------------------------------------------------------- round trip

void TestNativeRecipe::aRecipeRoundTripsThroughItsDocumentUnchanged() {
  Recipe original = spatialOnlyRecipe();
  original.spatial[0].preResize = QSize(1920, 1080);
  original.spatial[0].preResizeEnabled = true;

  compareRecipes(roundTrip(original), original);
}

void TestNativeRecipe::orderingOfSpatialStagesSurvivesTheRoundTrip() {
  // A chain is an execution order, not a set: two recipes naming the same models
  // in opposite order must not serialize to the same document.
  Recipe forward;
  forward.id = QStringLiteral("janai.chain.v1");
  forward.spatial = {stage(QStringLiteral("builtin.sd.compact")),
                     stage(QStringLiteral("builtin.hd.balanced"))};

  Recipe reversed = forward;
  reversed.spatial = {stage(QStringLiteral("builtin.hd.balanced")),
                      stage(QStringLiteral("builtin.sd.compact"))};

  const Recipe parsedForward = roundTrip(forward);
  const Recipe parsedReversed = roundTrip(reversed);
  compareRecipes(parsedForward, forward);
  compareRecipes(parsedReversed, reversed);

  QCOMPARE(parsedForward.spatial.at(0).modelId, QStringLiteral("builtin.sd.compact"));
  QCOMPARE(parsedForward.spatial.at(1).modelId, QStringLiteral("builtin.hd.balanced"));
  QCOMPARE(parsedReversed.spatial.at(0).modelId, QStringLiteral("builtin.hd.balanced"));
  QCOMPARE(parsedReversed.spatial.at(1).modelId, QStringLiteral("builtin.sd.compact"));
  QVERIFY(serializeRecipe(forward) != serializeRecipe(reversed));

  // Three stages, with a pre-resize only on the middle one.
  Recipe three;
  three.id = QStringLiteral("janai.chain3.v1");
  three.spatial = {stage(QStringLiteral("builtin.sd.compact")),
                   stage(QStringLiteral("builtin.hd.performance")),
                   stage(QStringLiteral("builtin.hd.balanced"))};
  three.spatial[1].preResize = QSize(1280, 720);
  three.spatial[1].preResizeEnabled = true;

  const Recipe parsedThree = roundTrip(three);
  QCOMPARE(parsedThree.spatial.size(), 3);
  QCOMPARE(parsedThree.spatial.at(0).modelId, QStringLiteral("builtin.sd.compact"));
  QCOMPARE(parsedThree.spatial.at(1).modelId, QStringLiteral("builtin.hd.performance"));
  QCOMPARE(parsedThree.spatial.at(1).preResize, QSize(1280, 720));
  QVERIFY(parsedThree.spatial.at(1).preResizeEnabled);
  QVERIFY(!parsedThree.spatial.at(0).preResizeEnabled);
  QCOMPARE(parsedThree.spatial.at(2).modelId, QStringLiteral("builtin.hd.balanced"));
}

void TestNativeRecipe::anUnreducedFactorSurvivesTheRoundTripUnchanged() {
  Recipe recipe = rifeOnlyRecipe();
  recipe.temporal->factor = Rational(4, 2);

  const Recipe parsed = roundTrip(recipe);

  // 4/2 and 2/1 are the same frame rate, but only 4/2 is what was asked for. The
  // document keeps the request; reduction is something a caller opts into.
  QCOMPARE(parsed.temporal->factor.num, 4);
  QCOMPARE(parsed.temporal->factor.den, 2);
  QVERIFY(!(parsed.temporal->factor == Rational(2, 1)));
  QCOMPARE(parsed.temporal->factor.reduced(), Rational(2, 1));

  Recipe combined = combinedRecipe(TemporalOrder::BeforeSpatial);
  combined.temporal->factor = Rational(3, 1);
  const Recipe parsedCombined = roundTrip(combined);
  QCOMPARE(parsedCombined.temporal->factor, Rational(3, 1));
  QCOMPARE(parsedCombined.temporal->order, TemporalOrder::BeforeSpatial);
}

void TestNativeRecipe::allThreeShapesSurviveTheirOwnRoundTrip() {
  const Recipe spatial = spatialOnlyRecipe();
  const Recipe parsedSpatial = roundTrip(spatial);
  compareRecipes(parsedSpatial, spatial);
  QVERIFY(!parsedSpatial.temporal.has_value());

  const Recipe rife = rifeOnlyRecipe();
  const Recipe parsedRife = roundTrip(rife);
  compareRecipes(parsedRife, rife);
  QVERIFY(parsedRife.spatial.isEmpty());

  for (const TemporalOrder order : {TemporalOrder::BeforeSpatial, TemporalOrder::AfterSpatial}) {
    const Recipe combined = combinedRecipe(order);
    compareRecipes(roundTrip(combined), combined);
  }

  // Each variant and the ensemble flag are named separately in the document, so
  // a stage can never read back as a different variant than it was written as.
  for (const RifeVariant variant : {RifeVariant::Full, RifeVariant::Lite}) {
    for (const bool ensemble : {false, true}) {
      Recipe recipe = rifeOnlyRecipe();
      recipe.temporal->variant = variant;
      recipe.temporal->ensemble = ensemble;
      const Recipe parsed = roundTrip(recipe);
      QCOMPARE(parsed.temporal->variant, variant);
      QCOMPARE(parsed.temporal->ensemble, ensemble);
      QCOMPARE(parsed.temporal->modelId(),
               rifeModelName(RifeVersion::V4_14, variant, ensemble));
      QCOMPARE(rifeModelCode(parsed.temporal->variant),
               variant == RifeVariant::Lite ? 4141 : 414);
    }
  }

  // A threshold other than the default survives as written.
  Recipe threshold = rifeOnlyRecipe();
  threshold.temporal->sceneThreshold = 0.250;
  QCOMPARE(roundTrip(threshold).temporal->sceneThreshold, 0.250);

  // The backend is a real member, not an implicit default.
  Recipe directml = spatialOnlyRecipe();
  directml.backend = Backend::DirectML;
  QCOMPARE(roundTrip(directml).backend, Backend::DirectML);
}

// ---------------------------------------------------------------- parsing

void TestNativeRecipe::anUnknownSchemaIsRejectedNotGuessed() {
  QJsonObject document = recipeToJson(spatialOnlyRecipe());
  QCOMPARE(document.value(QStringLiteral("schema")).toString(), recipeSchema());

  document.insert(QStringLiteral("schema"), QStringLiteral("mikai.native.recipe/2"));
  QCOMPARE(refusalKey(document), QStringLiteral("recipe.schema.unknown"));

  document.insert(QStringLiteral("schema"), QString());
  QCOMPARE(refusalKey(document), QStringLiteral("recipe.schema.unknown"));

  // Not a JSON document at all fails the same way an unknown schema does.
  for (const QByteArray& payload :
       {QByteArrayLiteral("{not json"), QByteArrayLiteral("[]"), QByteArray()}) {
    Recipe parsed;
    QString key;
    QString error;
    QVERIFY(!parseRecipe(payload, &parsed, &key, &error));
    QCOMPARE(key, QStringLiteral("recipe.json.malformed"));
    QVERIFY(!error.isEmpty());
    QVERIFY(parsed.id.isEmpty());
  }

  // A refused parse leaves the output default-constructed rather than half set.
  Recipe untouched{QStringLiteral("previous"), QStringLiteral("Previous"),
                   {stage(QStringLiteral("builtin.hd.balanced"))}, std::nullopt,
                   Backend::DirectML};
  QJsonObject wrong = recipeToJson(spatialOnlyRecipe());
  wrong.insert(QStringLiteral("schema"), QStringLiteral("nope"));
  QString key;
  QString error;
  QVERIFY(!parseRecipe(wrong, &untouched, &key, &error));
  QVERIFY(untouched.id.isEmpty());
  QVERIFY(untouched.spatial.isEmpty());
  QVERIFY(!untouched.temporal.has_value());
  QCOMPARE(untouched.backend, Backend::TensorRT);
}

void TestNativeRecipe::anUnknownEnumValueIsRejectedNotDefaulted() {
  QJsonObject document = recipeToJson(spatialOnlyRecipe());
  document.insert(QStringLiteral("backend"), QStringLiteral("cuda"));
  QCOMPARE(refusalKey(document), QStringLiteral("recipe.backend.unknown"));

  // A backend name differing only in case is still unknown.
  document = recipeToJson(spatialOnlyRecipe());
  document.insert(QStringLiteral("backend"), QStringLiteral("TensorRT"));
  QCOMPARE(refusalKey(document), QStringLiteral("recipe.backend.unknown"));

  // An unknown variant, with the model, version and ensemble all agreeing so the
  // variant is the only defect in the document.
  document = recipeToJson(rifeOnlyRecipe());
  QJsonObject temporal = document.value(QStringLiteral("temporal")).toObject();
  temporal.insert(QStringLiteral("variant"), QStringLiteral("ensemble"));
  document.insert(QStringLiteral("temporal"), temporal);
  QCOMPARE(refusalKey(document),
           QStringLiteral("stage.temporal.variant.unknown"));

  // An unknown order, in a fresh fixture left consistent with its own model: the
  // order is the independent defect, not a contradiction to report first.
  document = recipeToJson(rifeOnlyRecipe());
  temporal = document.value(QStringLiteral("temporal")).toObject();
  temporal.insert(QStringLiteral("order"), QStringLiteral("before"));
  document.insert(QStringLiteral("temporal"), temporal);
  QCOMPARE(refusalKey(document),
           QStringLiteral("stage.temporal.order.unknown"));

  // An unknown revision, again the only defect in a reset fixture.
  document = recipeToJson(rifeOnlyRecipe());
  temporal = document.value(QStringLiteral("temporal")).toObject();
  temporal.insert(QStringLiteral("version"), QStringLiteral("4.6"));
  document.insert(QStringLiteral("temporal"), temporal);
  QCOMPARE(refusalKey(document),
           QStringLiteral("stage.temporal.version.unknown"));
}

void TestNativeRecipe::anUnknownMemberIsRejectedNotIgnored() {
  // A misspelled pre-resize key that were ignored would leave the stage
  // configured as something the document never said.
  QJsonObject document = recipeToJson(spatialOnlyRecipe());
  QJsonArray stages = document.value(QStringLiteral("spatial")).toArray();
  QJsonObject stageObject = stages.at(0).toObject();
  stageObject.insert(QStringLiteral("preRsize"), QJsonObject{{QStringLiteral("w"), 1920},
                                                              {QStringLiteral("h"), 1080}});
  stages.replace(0, stageObject);
  document.insert(QStringLiteral("spatial"), stages);
  QCOMPARE(refusalKey(document), QStringLiteral("stage.unknown.member"));

  // A prepared-artifact identity at the top level is refused rather than stored:
  // a recipe carries models only.
  document = recipeToJson(spatialOnlyRecipe());
  document.insert(QStringLiteral("preparedPlanId"), QStringLiteral("build-1"));
  QCOMPARE(refusalKey(document), QStringLiteral("recipe.unknown.member"));

  document = recipeToJson(spatialOnlyRecipe());
  document.insert(QStringLiteral("engineSha256"), QString::fromLatin1(kHdBalancedSha));
  QCOMPARE(refusalKey(document), QStringLiteral("recipe.unknown.member"));

  // Inside a temporal member, where a typo would silently disable a field.
  document = recipeToJson(rifeOnlyRecipe());
  QJsonObject temporal = document.value(QStringLiteral("temporal")).toObject();
  temporal.insert(QStringLiteral("sceneThreshhold"), 0.3);
  document.insert(QStringLiteral("temporal"), temporal);
  QCOMPARE(refusalKey(document),
           QStringLiteral("recipe.temporal.unknown.member"));
}

void TestNativeRecipe::aTemporalStageWithoutAnOrderIsRejected() {
  QJsonObject document = recipeToJson(combinedRecipe(TemporalOrder::BeforeSpatial));
  QJsonObject temporal = document.value(QStringLiteral("temporal")).toObject();
  temporal.remove(QStringLiteral("order"));
  document.insert(QStringLiteral("temporal"), temporal);
  // Defaulting the order would pick an execution order the document never chose.
  QCOMPARE(refusalKey(document),
           QStringLiteral("stage.temporal.order.missing"));

  // Present but of the wrong type is the same refusal.
  temporal = document.value(QStringLiteral("temporal")).toObject();
  temporal.insert(QStringLiteral("order"), 1);
  document.insert(QStringLiteral("temporal"), temporal);
  QCOMPARE(refusalKey(document),
           QStringLiteral("stage.temporal.order.missing"));

  // Both orders parse, and parse to themselves.
  for (const TemporalOrder order : {TemporalOrder::BeforeSpatial, TemporalOrder::AfterSpatial}) {
    Recipe parsed;
    QString key;
    QString error;
    QVERIFY(parseRecipe(recipeToJson(combinedRecipe(order)), &parsed, &key, &error));
    QCOMPARE(parsed.temporal->order, order);
    QCOMPARE(parsed.spatial.size(), 1);
  }
}

void TestNativeRecipe::aMalformedRationalIsRejected() {
  QJsonObject document = recipeToJson(rifeOnlyRecipe());
  QJsonObject base = document.value(QStringLiteral("temporal")).toObject();

  const auto refuseFactor = [&base, &document](const QJsonValue& factor) {
    QJsonObject temporal = base;
    temporal.insert(QStringLiteral("factor"), factor);
    QJsonObject candidate = document;
    candidate.insert(QStringLiteral("temporal"), temporal);
    return refusalKey(candidate);
  };

  QCOMPARE(refuseFactor(QJsonObject{{QStringLiteral("num"), 0}, {QStringLiteral("den"), 1}}),
           QStringLiteral("stage.temporal.factor.invalid"));
  QCOMPARE(refuseFactor(QJsonObject{{QStringLiteral("num"), 2}, {QStringLiteral("den"), 0}}),
           QStringLiteral("stage.temporal.factor.invalid"));
  QCOMPARE(refuseFactor(QJsonObject{{QStringLiteral("num"), -2}, {QStringLiteral("den"), 1}}),
           QStringLiteral("stage.temporal.factor.invalid"));
  // A fraction-shaped factor is not a rational pair.
  QCOMPARE(refuseFactor(QStringLiteral("2/1")),
           QStringLiteral("stage.temporal.factor.malformed"));
  // A non-integer numerator and a missing member.
  QCOMPARE(refuseFactor(QJsonObject{{QStringLiteral("num"), 2.5}, {QStringLiteral("den"), 1}}),
           QStringLiteral("stage.temporal.factor.malformed"));
  QCOMPARE(refuseFactor(QJsonObject{{QStringLiteral("num"), 2}}),
           QStringLiteral("stage.temporal.factor.malformed"));
  QCOMPARE(refuseFactor(QJsonArray{2, 1}), QStringLiteral("stage.temporal.factor.malformed"));
}

void TestNativeRecipe::anOutOfRangeThresholdIsRejected() {
  QJsonObject document = recipeToJson(rifeOnlyRecipe());
  const QJsonObject base = document.value(QStringLiteral("temporal")).toObject();

  // Outside [0,1] the threshold would either never detect a cut or always take
  // one, and NaN compares false against everything and silently disables it.
  for (const double threshold : {1.5, -0.1, 2.0}) {
    QJsonObject temporal = base;
    temporal.insert(QStringLiteral("sceneThreshold"), threshold);
    QJsonObject candidate = document;
    candidate.insert(QStringLiteral("temporal"), temporal);
    QCOMPARE(refusalKey(candidate),
             QStringLiteral("stage.temporal.threshold.invalid"));
  }

  QJsonObject temporal = base;
  temporal.insert(QStringLiteral("sceneThreshold"), QStringLiteral("0.15"));
  QJsonObject candidate = document;
  candidate.insert(QStringLiteral("temporal"), temporal);
  QCOMPARE(refusalKey(candidate),
           QStringLiteral("stage.temporal.threshold.malformed"));
}

void TestNativeRecipe::aMalformedPreResizeIsRejected() {
  QJsonObject document = recipeToJson(spatialOnlyRecipe());

  const auto refuseStage = [&document](const QJsonValue& stageValue) {
    QJsonObject candidate = document;
    candidate.insert(QStringLiteral("spatial"), QJsonArray{stageValue});
    return refusalKey(candidate);
  };
  const auto geometry = [](int w, int h) {
    return QJsonObject{{QStringLiteral("w"), w}, {QStringLiteral("h"), h}};
  };
  const QJsonObject modelOnly{
      {QStringLiteral("model"), QStringLiteral("builtin.hd.balanced")}};

  QJsonObject stageObject = modelOnly;
  stageObject.insert(QStringLiteral("preResize"), geometry(0, 1080));
  QCOMPARE(refuseStage(stageObject), QStringLiteral("stage.preresize.invalid"));

  stageObject = modelOnly;
  stageObject.insert(QStringLiteral("preResize"), geometry(-1920, 1080));
  QCOMPARE(refuseStage(stageObject), QStringLiteral("stage.preresize.invalid"));

  stageObject = modelOnly;
  stageObject.insert(QStringLiteral("preResize"),
                     QJsonObject{{QStringLiteral("w"), 1920.5}, {QStringLiteral("h"), 1080}});
  QCOMPARE(refuseStage(stageObject), QStringLiteral("stage.preresize.malformed"));

  stageObject = modelOnly;
  stageObject.insert(QStringLiteral("preResize"), QJsonObject{{QStringLiteral("w"), 1920}});
  QCOMPARE(refuseStage(stageObject), QStringLiteral("stage.preresize.malformed"));

  stageObject = modelOnly;
  stageObject.insert(QStringLiteral("preResize"), QStringLiteral("1920x1080"));
  QCOMPARE(refuseStage(stageObject), QStringLiteral("stage.preresize.malformed"));

  // A stage that is not an object at all.
  QCOMPARE(refuseStage(QStringLiteral("builtin.hd.balanced")),
           QStringLiteral("stage.malformed"));

  // A spatial member that is not an array.
  QJsonObject candidate = document;
  candidate.insert(QStringLiteral("spatial"), QJsonObject{});
  QCOMPARE(refusalKey(candidate),
           QStringLiteral("recipe.spatial.malformed"));

  // An empty array and no temporal member is the rejected shape, not a silent
  // pass-through to the core.
  candidate = document;
  candidate.insert(QStringLiteral("spatial"), QJsonArray{});
  QCOMPARE(refusalKey(candidate), QStringLiteral("recipe.empty"));

  // A valid pre-resize parses and is preserved.
  stageObject = modelOnly;
  stageObject.insert(QStringLiteral("preResize"), geometry(1920, 1080));
  candidate = document;
  candidate.insert(QStringLiteral("spatial"), QJsonArray{stageObject});
  Recipe parsed;
  QString key;
  QString error;
  QVERIFY(parseRecipe(candidate, &parsed, &key, &error));
  QVERIFY(parsed.spatial.at(0).preResizeEnabled);
  QCOMPARE(parsed.spatial.at(0).preResize, QSize(1920, 1080));
}

void TestNativeRecipe::aTemporalModelContradictingItsOwnFieldsIsRejected() {
  QJsonObject document = recipeToJson(rifeOnlyRecipe());
  const QJsonObject base = document.value(QStringLiteral("temporal")).toObject();

  const auto refuse = [&document](QJsonObject temporal) {
    QJsonObject candidate = document;
    candidate.insert(QStringLiteral("temporal"), temporal);
    return refusalKey(candidate);
  };

  // The model name, the version, the variant and the ensemble flag all describe
  // the same model. A document where they disagree is ambiguous, and picking one
  // would be a guess about which RIFE was meant.
  QJsonObject temporal = base;
  temporal.insert(QStringLiteral("variant"), QStringLiteral("lite"));
  QCOMPARE(refuse(temporal), QStringLiteral("stage.temporal.model.mismatch"));

  temporal = base;
  temporal.insert(QStringLiteral("ensemble"), true);
  QCOMPARE(refuse(temporal), QStringLiteral("stage.temporal.model.mismatch"));

  temporal = base;
  temporal.insert(QStringLiteral("model"), QString());
  QCOMPARE(refuse(temporal), QStringLiteral("stage.temporal.model.missing"));

  temporal = base;
  temporal.insert(QStringLiteral("model"), QStringLiteral("rife_v3"));
  QCOMPARE(refuse(temporal), QStringLiteral("stage.temporal.model.mismatch"));

  temporal = base;
  temporal.insert(QStringLiteral("version"), QStringLiteral("4.6"));
  QCOMPARE(refuse(temporal), QStringLiteral("stage.temporal.version.unknown"));

  temporal = base;
  temporal.insert(QStringLiteral("ensemble"), QStringLiteral("yes"));
  QCOMPARE(refuse(temporal), QStringLiteral("stage.temporal.ensemble.malformed"));

  // A document whose four fields agree parses, and the identity is exact.
  temporal = base;
  temporal.insert(QStringLiteral("model"), QStringLiteral("rife_v4.14_lite_ensemble"));
  temporal.insert(QStringLiteral("variant"), QStringLiteral("lite"));
  temporal.insert(QStringLiteral("ensemble"), true);
  QJsonObject candidate = document;
  candidate.insert(QStringLiteral("temporal"), temporal);
  Recipe parsed;
  QString key;
  QString error;
  QVERIFY2(parseRecipe(candidate, &parsed, &key, &error), qPrintable(error));
  QCOMPARE(parsed.temporal->modelId(), QStringLiteral("rife_v4.14_lite_ensemble"));
  QCOMPARE(parsed.temporal->variant, RifeVariant::Lite);
  QCOMPARE(parsed.temporal->ensemble, true);
}

// ------------------------------------------------------------ eligibility

void TestNativeRecipe::aRecipeNamingAnAbsentModelIsUnavailableWithAReason() {
  Recipe recipe = spatialOnlyRecipe();
  recipe.spatial = {stage(QStringLiteral("builtin.not.installed"))};

  const RecipeEligibility result = evaluateEligibility(recipe, fullCatalog(), bothBackends());
  QCOMPARE(result.availability.value, Availability::Unavailable);
  QCOMPARE(result.availability.reasonKey, QStringLiteral("model.missing"));
  QVERIFY(result.availability.reason.contains(QStringLiteral("builtin.not.installed")));
  QVERIFY(result.availability.hasMandatoryReason());

  // A catalogue that never held the row is not a licence to run something else.
  QCOMPARE(result.requested.recipeId, recipe.id);
  QVERIFY(result.effective.isOriginal());
}

void TestNativeRecipe::aRecipeNamingAnUnavailableBackendIsUnavailableWithAReason() {
  Recipe recipe = spatialOnlyRecipe();
  recipe.backend = Backend::DirectML;

  const CatalogView catalog = fullCatalog();
  // The models do support DirectML; the runtime does not carry it.
  const RecipeEligibility missing =
      evaluateEligibility(recipe, catalog, BackendAvailability{{Backend::TensorRT}});
  QCOMPARE(missing.availability.value, Availability::Unavailable);
  QCOMPARE(missing.availability.reasonKey, QStringLiteral("backend.unavailable"));
  QVERIFY(missing.availability.reason.contains(QStringLiteral("directml")));
  QVERIFY(missing.effective.isOriginal());
  // Requested stays what was asked for, even though it is not in effect.
  QCOMPARE(missing.requested.backend, Backend::DirectML);

  // With no backend at all, the same refusal - and never a fallback to the other.
  const RecipeEligibility none = evaluateEligibility(recipe, catalog, BackendAvailability{});
  QCOMPARE(none.availability.reasonKey, QStringLiteral("backend.unavailable"));
  QVERIFY(none.effective.isOriginal());

  // A TensorRT-only runtime with a TensorRT recipe is ready.
  const Recipe trt = spatialOnlyRecipe();
  const RecipeEligibility ok =
      evaluateEligibility(trt, catalog, BackendAvailability{{Backend::TensorRT}});
  QCOMPARE(ok.availability.value, Availability::Ready);
  QCOMPARE(ok.effective.backend, Backend::TensorRT);
}

void TestNativeRecipe::aRecipeNamingAModelOnTheWrongBackendIsUnavailable() {
  CatalogView catalog = fullCatalog();
  ModelRecord* hd = nullptr;
  for (ModelRecord& record : catalog.models) {
    if (record.id == QStringLiteral("builtin.hd.balanced")) hd = &record;
  }
  QVERIFY(hd != nullptr);
  hd->availableBackends = {Backend::TensorRT};

  Recipe recipe = spatialOnlyRecipe();
  recipe.backend = Backend::DirectML;

  const RecipeEligibility result = evaluateEligibility(recipe, catalog, bothBackends());
  QCOMPARE(result.availability.value, Availability::Unavailable);
  QCOMPARE(result.availability.reasonKey, QStringLiteral("model.backend.unsupported"));
  QVERIFY(result.effective.isOriginal());

  // The same recipe on the backend the model does carry is ready.
  recipe.backend = Backend::TensorRT;
  QCOMPARE(evaluateEligibility(recipe, catalog, bothBackends()).availability.value,
           Availability::Ready);
}

void TestNativeRecipe::readinessIsAllOrNothingAcrossTheWholeChain() {
  // Every stage but one is present and offered. Readiness is the entire recipe
  // or nothing: a chain cannot run half of itself.
  Recipe recipe;
  recipe.id = QStringLiteral("janai.chain.v1");
  recipe.spatial = {stage(QStringLiteral("builtin.hd.performance")),
                    stage(QStringLiteral("builtin.not.installed")),
                    stage(QStringLiteral("builtin.hd.balanced"))};

  RecipeEligibility result = evaluateEligibility(recipe, fullCatalog(), bothBackends());
  QCOMPARE(result.availability.value, Availability::Unavailable);
  QCOMPARE(result.availability.reasonKey, QStringLiteral("model.missing"));
  QVERIFY(result.effective.isOriginal());
  QCOMPARE(result.stageReasons.size(), 1);

  // Drop the absent stage and the chain is ready - the good stages on their own
  // were never going to be applied without it.
  recipe.spatial = {stage(QStringLiteral("builtin.hd.performance")),
                    stage(QStringLiteral("builtin.hd.balanced"))};
  const RecipeEligibility ok = evaluateEligibility(recipe, fullCatalog(), bothBackends());
  QCOMPARE(ok.availability.value, Availability::Ready);
  QCOMPARE(ok.effective.recipeId, recipe.id);
  QVERIFY(ok.stageReasons.isEmpty());

  // A combined recipe is refused entirely when only its temporal half is
  // unavailable: the spatial stages are not left running on their own.
  Recipe combined = combinedRecipe(TemporalOrder::AfterSpatial);
  QCOMPARE(evaluateEligibility(combined, fullCatalog(), bothBackends()).availability.value,
           Availability::Ready);

  CatalogView withoutRife = fullCatalog();
  withoutRife.models.removeIf([](const ModelRecord& record) {
    return record.role == ModelRole::Temporal;
  });
  const RecipeEligibility noRife = evaluateEligibility(combined, withoutRife, bothBackends());
  QCOMPARE(noRife.availability.value, Availability::Unavailable);
  QCOMPARE(noRife.availability.reasonKey, QStringLiteral("model.missing"));
  QVERIFY(noRife.effective.isOriginal());

  // A spatial-only recipe over the same catalogue is unaffected: it never named
  // a temporal row.
  QCOMPARE(evaluateEligibility(spatialOnlyRecipe(), withoutRife, bothBackends())
               .availability.value,
           Availability::Ready);

  // An empty catalogue refuses everything with a reason rather than passing it.
  result = evaluateEligibility(spatialOnlyRecipe(), CatalogView{}, bothBackends());
  QCOMPARE(result.availability.value, Availability::Unavailable);
  QVERIFY(result.availability.hasMandatoryReason());
}

void TestNativeRecipe::anAvailableRecipeReportsRequestedAndEffectiveSeparately() {
  const Recipe recipe = combinedRecipe(TemporalOrder::BeforeSpatial);
  const RecipeEligibility result = evaluateEligibility(recipe, fullCatalog(), bothBackends());
  QCOMPARE(result.availability.value, Availability::Ready);
  QVERIFY(result.availability.reason.isEmpty());

  QCOMPARE(result.requested.recipeId, recipe.id);
  QCOMPARE(result.requested.backend, recipe.backend);
  QCOMPARE(result.effective.recipeId, recipe.id);
  QCOMPARE(result.effective.backend, recipe.backend);
  QCOMPARE(result.effective, result.requested);
  QVERIFY(!result.effective.isOriginal());
}

void TestNativeRecipe::aRefusedRecipeLeavesEffectiveOnTheOriginalPipeline() {
  Recipe recipe = combinedRecipe(TemporalOrder::AfterSpatial);
  recipe.temporal->variant = RifeVariant::Lite;
  QCOMPARE(evaluateEligibility(recipe, fullCatalog(), bothBackends()).availability.value,
           Availability::Ready);

  // Remove just the lite row: the recipe names a variant nothing supplies.
  CatalogView catalog = fullCatalog();
  catalog.models.removeIf([](const ModelRecord& record) {
    return record.id == QStringLiteral("rife_v4.14_lite");
  });

  const RecipeEligibility refused = evaluateEligibility(recipe, catalog, bothBackends());
  QCOMPARE(refused.availability.value, Availability::Unavailable);
  QCOMPARE(refused.availability.reasonKey, QStringLiteral("model.missing"));

  // Requested is exactly what was asked for; effective is Original. There is no
  // third state in which the full variant quietly ran instead.
  QCOMPARE(refused.requested.recipeId, recipe.id);
  QCOMPARE(refused.requested.backend, Backend::TensorRT);
  QVERIFY(refused.effective.isOriginal());
  QVERIFY(refused.effective != refused.requested);
  QVERIFY(refused.effective.recipeId.isEmpty());
}

void TestNativeRecipe::aRifeOnlyRecipeStillNeedsItsTemporalModelInTheCatalogue() {
  const Recipe recipe = rifeOnlyRecipe();
  // RIFE-only is legal, but legal is not ready: the variant's weights still have
  // to exist, and nothing here assumes they do.
  QCOMPARE(evaluateEligibility(recipe, fullCatalog(), bothBackends()).availability.value,
           Availability::Ready);

  // The reference package ships no RIFE weights, so an empty catalogue is the
  // honest starting state and the refusal is the expected one.
  const RecipeEligibility none =
      evaluateEligibility(recipe, CatalogView{}, bothBackends());
  QCOMPARE(none.availability.value, Availability::Unavailable);
  QCOMPARE(none.availability.reasonKey, QStringLiteral("model.missing"));
  QVERIFY(none.availability.reason.contains(QStringLiteral("rife_v4.14")));

  // Every variant is a distinct name, so a catalogue holding one never satisfies
  // a stage naming another.
  for (const RifeVariant variant : {RifeVariant::Full, RifeVariant::Lite}) {
    for (const bool ensemble : {false, true}) {
      Recipe asked = rifeOnlyRecipe();
      asked.temporal->variant = variant;
      asked.temporal->ensemble = ensemble;

      CatalogView onlyFull;
      onlyFull.models = {rifeRow(RifeVariant::Full, false)};
      const bool same = (variant == RifeVariant::Full) && !ensemble;
      QCOMPARE(evaluateEligibility(asked, onlyFull, bothBackends()).availability.value,
               same ? Availability::Ready : Availability::Unavailable);
    }
  }
}

void TestNativeRecipe::anUnavailableModelRowIsRefusedWithItsOwnReason() {
  CatalogView catalog = fullCatalog();
  ModelRecord* sd = nullptr;
  ModelRecord* hd = nullptr;
  for (ModelRecord& record : catalog.models) {
    if (record.id == QStringLiteral("builtin.sd.compact")) sd = &record;
    if (record.id == QStringLiteral("builtin.hd.balanced")) hd = &record;
  }
  QVERIFY(sd != nullptr);
  QVERIFY(hd != nullptr);

  // Preparing is not ready: the reason is carried and the recipe does not run.
  hd->availability = preparingAvailability(QStringLiteral("model.building"),
                                           QStringLiteral("engine is being built"));
  RecipeEligibility result = evaluateEligibility(spatialOnlyRecipe(), catalog, bothBackends());
  QCOMPARE(result.availability.value, Availability::Unavailable);
  QCOMPARE(result.availability.reasonKey, QStringLiteral("model.building"));
  QVERIFY(result.availability.reason.contains(QStringLiteral("being built")));
  QVERIFY(result.effective.isOriginal());

  // An unavailable row reports the asset fact that is missing.
  hd->availability = unavailableAvailability(
      QStringLiteral("model.assets.missing"),
      QStringLiteral("the model file named by builtin.hd.balanced is not on disk"));
  result = evaluateEligibility(spatialOnlyRecipe(), catalog, bothBackends());
  QCOMPARE(result.availability.reasonKey, QStringLiteral("model.assets.missing"));
  hd->availability = readyAvailability();

  // A spatial row cannot stand in for a temporal stage.
  Recipe combined = combinedRecipe(TemporalOrder::AfterSpatial);
  combined.spatial = {stage(QStringLiteral("rife_v4.14"))};
  result = evaluateEligibility(combined, catalog, bothBackends());
  QCOMPARE(result.availability.value, Availability::Unavailable);
  QCOMPARE(result.availability.reasonKey, QStringLiteral("model.role.mismatch"));
  QVERIFY(result.availability.reason.contains(QStringLiteral("rife_v4.14")));

  // A refused stage carries a reason that points at it.
  sd->actualProfile.output.dtype = QStringLiteral("fp32");
  Recipe chain;
  chain.id = QStringLiteral("janai.chain.dtype.v1");
  chain.spatial = {stage(QStringLiteral("builtin.hd.balanced")),
                   stage(QStringLiteral("builtin.sd.compact"))};
  result = evaluateEligibility(chain, catalog, bothBackends());
  QCOMPARE(result.availability.value, Availability::Unavailable);
  QCOMPARE(result.availability.reasonKey, QStringLiteral("model.profile.ineligible"));
  QCOMPARE(result.stageReasons.size(), 1);
  QVERIFY(result.stageReasons.at(0).contains(QStringLiteral("builtin.sd.compact")));
  QVERIFY(result.availability.reason.contains(QStringLiteral("fp32")));
  QVERIFY(result.effective.isOriginal());

  // With the dtype restored the same chain is ready.
  sd->actualProfile.output.dtype = QStringLiteral("fp16");
  QCOMPARE(evaluateEligibility(chain, catalog, bothBackends()).availability.value,
           Availability::Ready);

  // A row that is not ready without saying why is refused on that alone: an
  // unexplained "unavailable" is a lie about why nothing works.
  hd->availability = AvailabilityState{};
  result = evaluateEligibility(spatialOnlyRecipe(), catalog, bothBackends());
  QCOMPARE(result.availability.reasonKey, QStringLiteral("model.reason.missing"));
  QVERIFY(result.availability.reason.contains(QStringLiteral("builtin.hd.balanced")));
}