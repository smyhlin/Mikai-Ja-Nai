#include "TestNativePrepPlan.hpp"

#include "player/native/NativePreparationPlan.hpp"
#include "player/native/NativeRife.hpp"

#include <QtCore/QRect>
#include <QtCore/QSet>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QVector>

using namespace mplayer::native;

namespace {

const QString kShaA = QString(64, QLatin1Char('a'));

/// A fixed declared tensor. `geometry` drives both H and W.
TensorProfile fixedTensor(const QSize& geometry, qint64 channels = 3) {
  TensorProfile tensor;
  tensor.dtype = QString::fromLatin1(kEligibleIoType);
  tensor.rank = 4;
  tensor.batch = 1;
  tensor.channels = channels;
  tensor.height = geometry.height();
  tensor.width = geometry.width();
  return tensor;
}

/// A graph that leaves H and W dynamic. Not an error: it is what a dyn-HW export
/// declares, and its geometry only exists once a stage resolves it.
TensorProfile dynamicTensor(qint64 channels = 3) {
  TensorProfile tensor;
  tensor.dtype = QString::fromLatin1(kEligibleIoType);
  tensor.rank = 4;
  tensor.batch = 1;
  tensor.channels = channels;
  return tensor;
}

ModelProfile profile(const QSize& input, const QSize& output) {
  ModelProfile declared;
  declared.input = fixedTensor(input);
  declared.output = fixedTensor(output);
  return declared;
}

ModelProfile dynamicProfile() {
  ModelProfile declared;
  declared.input = dynamicTensor();
  declared.output = dynamicTensor();
  return declared;
}

ModelRecord spatialRow(const QString& id, const ModelProfile& declared,
                       const QVector<Backend>& backends = {Backend::TensorRT, Backend::DirectML},
                       const QString& sha = kShaA) {
  ModelRecord record;
  record.id = id;
  record.displayName = id;
  record.origin = ModelOrigin::Builtin;
  record.onnxPath = QStringLiteral("C:/models/%1.onnx").arg(id);
  record.onnxSha256 = sha;
  record.role = ModelRole::Spatial;
  record.actualProfile = declared;
  record.availableBackends = backends;
  record.availability = readyAvailability();
  return record;
}

/// The declared RIFE boundary: fp16, rank 4, N=1, C=11 in / C=3 out, H/W
/// dynamic. The temporal inspection settles this boundary, not the spatial rules.
ModelProfile temporalProfile() {
  ModelProfile declared;
  declared.input = dynamicTensor(11);
  declared.output = dynamicTensor(3);
  return declared;
}

/// The concrete boundary a real CPU run produced for the pinned graphs: a
/// 320x180 pair interpolated to 320x180, uniform factor 1.
ModelProfile temporalReference() {
  ModelProfile reference;
  reference.input = fixedTensor(QSize(320, 180), 11);
  reference.output = fixedTensor(QSize(320, 180), 3);
  return reference;
}

/// The owned temporal row the curated variant's exact pin resolves to. The id is
/// content-addressed from the pin digest, role and declared boundary - there is
/// no second row named after the curated variant.
ModelRecord rifeRow(RifeVariant variant = RifeVariant::Full, bool ensemble = false) {
  const RifeModelPin* pin = rifeModelPin(RifeVersion::V4_14, variant, ensemble);
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

SpatialStage stageOf(const QString& modelId) {
  SpatialStage stage;
  stage.modelId = modelId;
  return stage;
}

SpatialStage stageWithPreResize(const QString& modelId, const QSize& size) {
  SpatialStage stage;
  stage.modelId = modelId;
  stage.preResize = size;
  stage.preResizeEnabled = true;
  return stage;
}

TemporalStage temporalOf(TemporalOrder order, const Rational& factor = Rational{2, 1}) {
  TemporalStage stage;
  stage.version = RifeVersion::V4_14;
  stage.variant = RifeVariant::Full;
  stage.ensemble = false;
  stage.factor = factor;
  stage.order = order;
  stage.sceneThreshold = 0.150;
  return stage;
}

Recipe spatialRecipe(const QString& id, const QVector<SpatialStage>& stages) {
  Recipe recipe;
  recipe.id = id;
  recipe.label = id;
  recipe.spatial = stages;
  recipe.backend = Backend::TensorRT;
  return recipe;
}

Recipe rifeRecipe(const QString& id, const QVector<SpatialStage>& stages, const TemporalStage& order) {
  Recipe recipe = spatialRecipe(id, stages);
  recipe.temporal = order;
  return recipe;
}

CatalogView catalogOf(const QVector<ModelRecord>& records) {
  CatalogView view;
  view.models = records;
  return view;
}

/// The 1920x1080 fixture, limited BT.709 8-bit unless a test changes it.
SourceGeometry hdSource(int width = 1920, int height = 1080) {
  SourceGeometry source;
  source.codedSize = QSize(width, height);
  source.rotationDegrees = 0;
  source.fps = Rational{24, 1};
  source.csp = QStringLiteral("bt709");
  source.matrix = QStringLiteral("bt709");
  source.range = QStringLiteral("limited");
  source.siting = QStringLiteral("left");
  source.bitDepth = 8;
  return source;
}

DeviceIdentity knownDevice() {
  DeviceIdentity device;
  device.decoderDeviceUuid = QStringLiteral("GPU-decode-0000");
  device.computeDeviceUuid = QStringLiteral("GPU-compute-0001");
  device.adapterLuid = QStringLiteral("0x00000000:0x00001234");
  device.smMajor = 12;
  device.smMinor = 0;
  return device;
}

PlanRequest requestOf(const Recipe& recipe, const CatalogView& catalog, const SourceGeometry& source) {
  PlanRequest request;
  request.recipe = recipe;
  request.catalog = catalog;
  request.backends.available = {Backend::TensorRT, Backend::DirectML};
  request.source = source;
  request.device = knownDevice();
  request.window = ChainWindow{};
  return request;
}

bool mentions(const QVector<QString>& facts, const QString& needle) {
  for (const QString& fact : facts) {
    if (fact.contains(needle)) return true;
  }
  return false;
}

const ModelProfile kHd2x = profile(QSize(1920, 1080), QSize(3840, 2160));

}  // namespace

void TestNativePrepPlan::paddingMatchesTheReferenceMultipleOf64() {
  int padL = -1;
  int padT = -1;

  QCOMPARE(padToTensorAlignment(QSize(1920, 1080), &padL, &padT), QSize(1920, 1088));
  QCOMPARE(padL, 0);
  QCOMPARE(padT, 4);

  // The non-multiple-of-32 fixture height.
  QCOMPARE(padToTensorAlignment(QSize(1920, 1072), &padL, &padT), QSize(1920, 1088));
  QCOMPARE(padT, 8);

  // The portrait fixture width is the padded axis.
  QCOMPARE(padToTensorAlignment(QSize(1080, 1920), &padL, &padT), QSize(1088, 1920));
  QCOMPARE(padL, 4);
  QCOMPARE(padT, 0);
}

void TestNativePrepPlan::paddingIsCentredAndRoundedDownToEven() {
  int padL = -1;
  int padT = -1;

  QCOMPARE(padToTensorAlignment(QSize(1000, 1000), &padL, &padT), QSize(1024, 1024));
  QCOMPARE(padL, 12);
  QCOMPARE(padT, 12);

  // A border of 2 centres at 1 and rounds down to even, so the split stays even.
  QCOMPARE(padToTensorAlignment(QSize(1920, 1918), &padL, &padT), QSize(1920, 1920));
  QCOMPARE(padT, 0);

  QCOMPARE(padToTensorAlignment(QSize(62, 62), &padL, &padT), QSize(64, 64));
  QCOMPARE(padL, 0);
  QCOMPARE(padT, 0);
}

void TestNativePrepPlan::paddingExtentIsTheTotalAdded() {
  QCOMPARE(paddingExtent(QSize(1920, 1080)), QSize(0, 8));
  QCOMPARE(paddingExtent(QSize(1920, 1072)), QSize(0, 16));
  QCOMPARE(paddingExtent(QSize(1920, 1920)), QSize(0, 0));
}

void TestNativePrepPlan::aCadenceScalesToLowestTerms() {
  const std::optional<Rational> doubled = scaleCadence(Rational{24000, 1001}, 2, 1);
  QVERIFY(doubled.has_value());
  QCOMPARE(doubled.value(), (Rational{48000, 1001}));

  const std::optional<Rational> whole = scaleCadence(Rational{24, 1}, 2, 1);
  QVERIFY(whole.has_value());
  QCOMPARE(whole.value(), (Rational{48, 1}));

  const std::optional<Rational> reduced = scaleCadence(Rational{24000, 1001}, 3, 2);
  QVERIFY(reduced.has_value());
  QCOMPARE(reduced.value(), (Rational{36000, 1001}));
}

void TestNativePrepPlan::aCadenceOverflowIsRefusedNotWrapped() {
  QVERIFY(!scaleCadence(Rational{2147483647, 1}, 2147483647, 1).has_value());
  QVERIFY(!scaleCadence(Rational{24, 1}, 0, 1).has_value());
  QVERIFY(!scaleCadence(Rational{24, 1}, 2, 0).has_value());
  QVERIFY(!scaleCadence(Rational{0, 1}, 2, 1).has_value());
}

void TestNativePrepPlan::aShapeStringIsTheEngineBuildOrder() {
  QCOMPARE(stageShapeString(3, QSize(1920, 1080)), QStringLiteral("1x3x1080x1920"));
  QCOMPARE(stageShapeString(11, QSize(1920, 1088)), QStringLiteral("1x11x1088x1920"));
  QVERIFY(stageShapeString(0, QSize(1920, 1080)).isEmpty());
  QVERIFY(stageShapeString(3, QSize(0, 1080)).isEmpty());
  QVERIFY(stageShapeString(3, QSize(1920, 70000)).isEmpty());
}

void TestNativePrepPlan::onlyAUniformPositiveIntegerIsAScale() {
  QCOMPARE(uniformIntegerScale(QSize(1920, 1080), QSize(3840, 2160)).value_or(0), 2);
  QCOMPARE(uniformIntegerScale(QSize(1920, 1080), QSize(1920, 1080)).value_or(0), 1);
  QVERIFY(!uniformIntegerScale(QSize(1920, 1080), QSize(2880, 2160)).has_value());
  QVERIFY(!uniformIntegerScale(QSize(1920, 1080), QSize(3840, 1080)).has_value());
  QVERIFY(!uniformIntegerScale(QSize(3840, 2160), QSize(1920, 1080)).has_value());
}

void TestNativePrepPlan::geometryIsCroppedThenRotated() {
  QCOMPARE(displayGeometry(QSize(1920, 1080), QRect(0, 0, 1920, 1072), 0).value(),
           QSize(1920, 1072));
  QCOMPARE(displayGeometry(QSize(1920, 1080), std::nullopt, 90).value(), QSize(1080, 1920));
  QCOMPARE(displayGeometry(QSize(1920, 1080), QRect(0, 0, 1920, 1072), 90).value(),
           QSize(1072, 1920));
  QCOMPARE(displayGeometry(QSize(1920, 1080), std::nullopt, 180).value(), QSize(1920, 1080));
  QCOMPARE(displayGeometry(QSize(1920, 1080), std::nullopt, 270).value(), QSize(1080, 1920));
}

void TestNativePrepPlan::anAbsentRotationIsNotAConfirmedRotation() {
  // A rotation this build does not model leaves the geometry unknown.
  QVERIFY(!displayGeometry(QSize(1920, 1080), std::nullopt, 45).has_value());

  SourceGeometry source = hdSource();
  source.rotationDegrees = std::nullopt;
  QCOMPARE(source.displaySize().value(), QSize(1920, 1080));
  QVERIFY(!source.hasModelledRotation());

  source.rotationDegrees = 0;
  QVERIFY(source.hasModelledRotation());
  source.rotationDegrees = 90;
  QVERIFY(source.hasModelledRotation());

  source.rotationDegrees = 45;
  QVERIFY(!source.hasModelledRotation());
  QVERIFY(!source.isKnown());
}

void TestNativePrepPlan::aCropOutsideTheCodedFrameIsRefused() {
  QVERIFY(!displayGeometry(QSize(1920, 1080), QRect(100, 100, 1900, 1000), 0).has_value());
  QVERIFY(!displayGeometry(QSize(1920, 1080), QRect(0, 0, 1921, 1080), 0).has_value());
  QVERIFY(!displayGeometry(QSize(1920, 1080), QRect(-1, 0, 1920, 1080), 0).has_value());
  QVERIFY(!displayGeometry(QSize(1920, 1080), QRect(), 0).has_value());
}

void TestNativePrepPlan::aPortraitSourceStaysPortrait() {
  SourceGeometry portrait = hdSource(1080, 1920);
  QCOMPARE(portrait.displaySize().value(), QSize(1080, 1920));
  QVERIFY(portrait.isKnown());
  QCOMPARE(portrait.pixelCount().value(), static_cast<qint64>(1080) * 1920);
}

void TestNativePrepPlan::aChainWindowAdmitsItsOwnBounds() {
  ChainWindow window;
  window.minPixels = 100;
  window.maxPixels = 3000000;
  window.minFps = Rational{20, 1};
  window.maxFps = Rational{60, 1};

  QVERIFY(window.admits(2073600, Rational{24, 1}));
  QVERIFY(window.admits(2073600, Rational{24000, 1001}));
  QVERIFY(!window.admits(50, Rational{24, 1}));
  QVERIFY(!window.admits(4000000, Rational{24, 1}));
  QVERIFY(!window.admits(2073600, Rational{15, 1}));
  QVERIFY(!window.admits(2073600, Rational{120, 1}));

  ChainWindow unbounded;
  QVERIFY(unbounded.admits(1, Rational{24, 1}));
  QVERIFY(!unbounded.admits(1, Rational{0, 1}));
}

void TestNativePrepPlan::aSourceOutsideItsWindowIsRefusedByName() {
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), hdSource());
  request.window.maxPixels = 1000000;

  PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.availability.value, Availability::Unavailable);
  QCOMPARE(plan.blockers.size(), 1);
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("source.window.pixels"));
  QVERIFY(plan.stages.isEmpty());
  QVERIFY(plan.unknowns.isEmpty());

  request.window = ChainWindow{};
  request.window.minFps = Rational{30, 1};
  plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("source.window.fps"));
}

void TestNativePrepPlan::aSpatialChainResolvesToItsDeclaredOutput() {
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), hdSource());

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QVERIFY(plan.canPrepare());
  QVERIFY(plan.blockers.isEmpty());
  QVERIFY(plan.unknowns.isEmpty());
  QCOMPARE(plan.availability.value, Availability::Ready);
  QVERIFY(plan.id.startsWith(QStringLiteral("sha256:")));
  QVERIFY(plan.resizes.isEmpty());
  QCOMPARE(plan.stages.size(), 1);

  const ResolvedStage& stage = plan.stages.first();
  QCOMPARE(stage.kind, StageKind::Spatial);
  QCOMPARE(stage.role, ModelRole::Spatial);
  QCOMPARE(stage.recipeIndex, 0);
  QCOMPARE(stage.inputGeometry, QSize(1920, 1080));
  QCOMPARE(stage.outputGeometry, QSize(3840, 2160));
  QCOMPARE(stage.contentGeometry, QSize(3840, 2160));
  QCOMPARE(stage.scale.value(), 2);
  QCOMPARE(stage.inputShape(), QStringLiteral("1x3x1080x1920"));
  QCOMPARE(stage.outputShape(), QStringLiteral("1x3x2160x3840"));
  QCOMPARE(stage.padL, 0);
  QCOMPARE(stage.padT, 0);
  QCOMPARE(stage.ioFormats(), QStringLiteral("fp16:chw"));
  QVERIFY(!stage.geometryFromResolver);
  QCOMPARE(plan.outputGeometry().value(), QSize(3840, 2160));
  QCOMPARE(plan.outputCadence().value(), (Rational{24, 1}));
}

void TestNativePrepPlan::aTwoTimesRifeAfterSpatialDoublesTheCadence() {
  PlanRequest request = requestOf(
      rifeRecipe(QStringLiteral("janai.test.combined.v1"), {stageOf(QStringLiteral("builtin.hd"))},
                 temporalOf(TemporalOrder::AfterSpatial)),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x), rifeRow()}), hdSource());

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.stages.size(), 2);
  QCOMPARE(plan.stages.first().kind, StageKind::Spatial);

  const ResolvedStage& temporal = plan.stages.last();
  QCOMPARE(temporal.kind, StageKind::Temporal);
  QCOMPARE(temporal.role, ModelRole::Temporal);
  QCOMPARE(temporal.recipeIndex, -1);
  QCOMPARE(temporal.contentGeometry, QSize(3840, 2160));
  QCOMPARE(temporal.inputGeometry, QSize(3840, 2176));
  QCOMPARE(temporal.padT, 8);
  QCOMPARE(temporal.inputShape(), QStringLiteral("1x11x2176x3840"));
  QCOMPARE(temporal.outputShape(), QStringLiteral("1x3x2176x3840"));
  QCOMPARE(temporal.inputFps, (Rational{24, 1}));
  QCOMPARE(temporal.outputFps, (Rational{48, 1}));
  QCOMPARE(temporal.order, TemporalOrder::AfterSpatial);
  QCOMPARE(plan.outputGeometry().value(), QSize(3840, 2160));
  QCOMPARE(plan.outputCadence().value(), (Rational{48, 1}));
}

void TestNativePrepPlan::aTwoTimesRifeBeforeSpatialStillDoublesTheCadence() {
  PlanRequest request = requestOf(
      rifeRecipe(QStringLiteral("janai.test.combined.v1"), {stageOf(QStringLiteral("builtin.hd"))},
                 temporalOf(TemporalOrder::BeforeSpatial)),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x), rifeRow()}), hdSource());

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.stages.size(), 2);

  const ResolvedStage& temporal = plan.stages.first();
  QCOMPARE(temporal.kind, StageKind::Temporal);
  QCOMPARE(temporal.order, TemporalOrder::BeforeSpatial);
  QCOMPARE(temporal.contentGeometry, QSize(1920, 1080));
  QCOMPARE(temporal.inputGeometry, QSize(1920, 1088));
  QCOMPARE(temporal.outputFps, (Rational{48, 1}));

  // Everything after interpolation runs on the interpolated cadence.
  const ResolvedStage& spatial = plan.stages.last();
  QCOMPARE(spatial.kind, StageKind::Spatial);
  QCOMPARE(spatial.inputFps, (Rational{48, 1}));
  QCOMPARE(spatial.outputFps, (Rational{48, 1}));
  QCOMPARE(plan.outputGeometry().value(), QSize(3840, 2160));
  QCOMPARE(plan.outputCadence().value(), (Rational{48, 1}));
}

void TestNativePrepPlan::aHoistedPreResizeRunsBeforeInterpolation() {
  PlanRequest request = requestOf(
      rifeRecipe(QStringLiteral("janai.test.combined.v1"),
                 {stageWithPreResize(QStringLiteral("builtin.hd"), QSize(960, 540))},
                 temporalOf(TemporalOrder::BeforeSpatial)),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"),
                            profile(QSize(960, 540), QSize(1920, 1080))),
                 rifeRow()}),
      hdSource());

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.resizes.size(), 1);

  const PreResizeStep& hoisted = plan.resizes.first();
  QCOMPARE(hoisted.stageIndex, -1);
  QVERIFY(hoisted.beforeTemporal);
  QCOMPARE(hoisted.modelId, QStringLiteral("builtin.hd"));
  QCOMPARE(hoisted.from, QSize(1920, 1080));
  QCOMPARE(hoisted.to, QSize(960, 540));

  QCOMPARE(plan.stages.size(), 2);
  const ResolvedStage& temporal = plan.stages.first();
  QCOMPARE(temporal.kind, StageKind::Temporal);
  QCOMPARE(temporal.contentGeometry, QSize(960, 540));
  QCOMPARE(temporal.inputGeometry, QSize(960, 576));
  QCOMPARE(temporal.padT, 18);
  QCOMPARE(plan.stages.last().kind, StageKind::Spatial);
  QCOMPARE(plan.stages.last().inputGeometry, QSize(960, 540));
  QCOMPARE(plan.stages.last().outputGeometry, QSize(1920, 1080));
  QCOMPARE(plan.outputCadence().value(), (Rational{48, 1}));
}

void TestNativePrepPlan::aPreResizeAfterTemporalIsNotHoisted() {
  PlanRequest request = requestOf(
      rifeRecipe(QStringLiteral("janai.test.combined.v1"),
                 {stageWithPreResize(QStringLiteral("builtin.hd"), QSize(960, 540))},
                 temporalOf(TemporalOrder::AfterSpatial)),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"),
                            profile(QSize(960, 540), QSize(1920, 1080))),
                 rifeRow()}),
      hdSource());

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.resizes.size(), 1);
  QCOMPARE(plan.resizes.first().stageIndex, 0);
  QVERIFY(!plan.resizes.first().beforeTemporal);
  QCOMPARE(plan.resizes.first().to, QSize(960, 540));

  QCOMPARE(plan.stages.first().kind, StageKind::Spatial);
  QCOMPARE(plan.stages.first().inputGeometry, QSize(960, 540));
  QCOMPARE(plan.stages.first().outputGeometry, QSize(1920, 1080));
  QCOMPARE(plan.stages.last().kind, StageKind::Temporal);
  QCOMPARE(plan.stages.last().contentGeometry, QSize(1920, 1080));
}

void TestNativePrepPlan::anOddPreResizeIsRefusedByName() {
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"),
                    {stageWithPreResize(QStringLiteral("builtin.hd"), QSize(961, 540))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"),
                            profile(QSize(961, 540), QSize(1922, 1080)))}),
      hdSource());
  PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("stage.preresize.odd"));
  QVERIFY(plan.stages.isEmpty());

  request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"),
                    {stageWithPreResize(QStringLiteral("builtin.hd"), QSize(70000, 1080))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), hdSource());
  plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("stage.preresize.invalid"));
}

void TestNativePrepPlan::aRifeOnlyRecipeResolvesToOneTemporalStage() {
  PlanRequest request = requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{},
                 temporalOf(TemporalOrder::AfterSpatial)),
      catalogOf({rifeRow()}), hdSource());

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QVERIFY(plan.resizes.isEmpty());
  QCOMPARE(plan.stages.size(), 1);

  const ResolvedStage& stage = plan.stages.first();
  QCOMPARE(stage.kind, StageKind::Temporal);
  QCOMPARE(stage.recipeIndex, -1);
  QCOMPARE(stage.contentGeometry, QSize(1920, 1080));
  QCOMPARE(stage.inputGeometry, QSize(1920, 1088));
  QCOMPARE(stage.outputGeometry, QSize(1920, 1088));
  QCOMPARE(stage.inputChannels, 11);
  QCOMPARE(stage.outputChannels, 3);
  QCOMPARE(plan.outputGeometry().value(), QSize(1920, 1080));
  QCOMPARE(plan.outputCadence().value(), (Rational{48, 1}));
}

void TestNativePrepPlan::aMissingModelRefusesThePlanByName() {
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"),
                    {stageOf(QStringLiteral("builtin.absent"))}),
      CatalogView{}, hdSource());
  PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.availability.value, Availability::Unavailable);
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("model.missing"));
  QCOMPARE(plan.blockers.first().stageIndex, 0);
  QVERIFY(plan.stages.isEmpty());
  QVERIFY(plan.unknowns.isEmpty());

  // A temporal stage names a curated variant, so an empty store reports the
  // missing pinned graph rather than a missing catalogue id.
  request = requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{},
                 temporalOf(TemporalOrder::AfterSpatial)),
      CatalogView{}, hdSource());
  plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("rife.model.missing"));
}

void TestNativePrepPlan::anUnavailableBackendRefusesThePlanByName() {
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), hdSource());
  request.backends.available = {Backend::DirectML};

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("backend.unavailable"));
  QVERIFY(plan.stages.isEmpty());
}

void TestNativePrepPlan::aModelOnTheWrongBackendIsRefused() {
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x, {Backend::DirectML})}),
      hdSource());

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("model.backend.unsupported"));
}

void TestNativePrepPlan::aModelOfTheWrongRoleIsRefused() {
  // A spatial stage names the owned temporal row by its own content id, so the
  // role check - not a missing id - is what refuses it.
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(rifeRow().id)}),
      catalogOf({rifeRow()}), hdSource());

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("model.role.mismatch"));
  QVERIFY(plan.stages.isEmpty());
}

void TestNativePrepPlan::aDynamicStageWithoutAResolverIsRefused() {
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.dyn"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.dyn"), dynamicProfile())}), hdSource());

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("stage.dynamic.unresolved"));
  QVERIFY(plan.stages.isEmpty());
}

void TestNativePrepPlan::aDynamicStageUsesTheSuppliedResolver() {
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.dyn"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.dyn"), dynamicProfile())}), hdSource());
  request.dynamicResolver = [](const ModelRecord&, const QSize& at) {
    ResolvedProfile resolved;
    resolved.input = at;
    resolved.output = QSize(at.width() * 2, at.height() * 2);
    return std::optional<ResolvedProfile>(resolved);
  };

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.stages.size(), 1);

  const ResolvedStage& stage = plan.stages.first();
  QVERIFY(stage.geometryFromResolver);
  QCOMPARE(stage.inputGeometry, QSize(1920, 1080));
  QCOMPARE(stage.outputGeometry, QSize(3840, 2160));
  QCOMPARE(stage.scale.value(), 2);
  QVERIFY(!stage.declaredInput.hasGeometry());
}

void TestNativePrepPlan::aResolverAnsweringTheWrongGeometryIsRefused() {
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.dyn"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.dyn"), dynamicProfile())}), hdSource());
  request.dynamicResolver = [](const ModelRecord&, const QSize&) {
    ResolvedProfile resolved;
    resolved.input = QSize(1280, 720);
    resolved.output = QSize(2560, 1440);
    return std::optional<ResolvedProfile>(resolved);
  };

  PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("stage.dynamic.mismatch"));

  request.dynamicResolver = [](const ModelRecord&, const QSize&) {
    return std::optional<ResolvedProfile>();
  };
  plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("stage.dynamic.unresolved"));
}

void TestNativePrepPlan::aFixedGraphRunAtAnotherSizeIsRefused() {
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"),
                            profile(QSize(1280, 720), QSize(2560, 1440)))}),
      hdSource());

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("stage.geometry.mismatch"));
}

void TestNativePrepPlan::aTemporalCadenceOverflowRefusesThePlanByName() {
  SourceGeometry source = hdSource();
  source.fps = Rational{2147483647, 1};
  PlanRequest request = requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{},
                 temporalOf(TemporalOrder::AfterSpatial, Rational{2147483647, 1})),
      catalogOf({rifeRow()}), source);

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("stage.temporal.cadence.overflow"));
  QVERIFY(plan.stages.isEmpty());
}

void TestNativePrepPlan::anUnobservedGeometryStaysPreparing() {
  SourceGeometry source = hdSource();
  source.codedSize = std::nullopt;
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), source);

  PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QVERIFY(plan.blockers.isEmpty());
  QVERIFY(!plan.unknowns.isEmpty());
  QCOMPARE(plan.availability.value, Availability::Preparing);
  QVERIFY(plan.id.isEmpty());
  QVERIFY(plan.stages.isEmpty());
  QVERIFY(mentions(plan.unknowns, QStringLiteral("decoded source geometry")));
  QVERIFY(!plan.outputGeometry().has_value());

  // A coded size is known but the rotation is one this build does not model.
  source = hdSource();
  source.rotationDegrees = 45;
  plan = resolvePreparationPlan(requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), source));
  QVERIFY(!plan.canPrepare());
  QVERIFY(plan.blockers.isEmpty());
  QVERIFY(mentions(plan.unknowns, QStringLiteral("display geometry this build can honour")));

  // A size this build cannot multiply into a tensor shape is a named refusal.
  source = hdSource(70000, 1080);
  plan = resolvePreparationPlan(requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), source));
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("source.geometry.invalid"));
  QVERIFY(plan.stages.isEmpty());
}

void TestNativePrepPlan::anUnobservedCadenceStaysPreparing() {
  SourceGeometry source = hdSource();
  source.fps = std::nullopt;
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), source);

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QVERIFY(plan.blockers.isEmpty());
  QCOMPARE(plan.availability.value, Availability::Preparing);
  QVERIFY(mentions(plan.unknowns, QStringLiteral("the source cadence")));
}

void TestNativePrepPlan::anUnobservedDeviceStaysPreparing() {
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), hdSource());
  request.device = DeviceIdentity{};

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QVERIFY(plan.blockers.isEmpty());
  QCOMPARE(plan.availability.value, Availability::Preparing);
  QVERIFY(mentions(plan.unknowns, QStringLiteral("device identity")));
  // Nothing runs while the plan is not resolved.
  QVERIFY(plan.stages.isEmpty());
  QVERIFY(plan.id.isEmpty());
}

void TestNativePrepPlan::aResolvedPlanCarriesItsColourTagsUnchanged() {
  SourceGeometry source = hdSource();
  source.csp = QStringLiteral("bt2020nc");
  source.matrix = QStringLiteral("bt2020nc");
  source.range = QStringLiteral("full");
  source.siting = QStringLiteral("top");
  source.bitDepth = 10;

  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), source);

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.source.csp, QStringLiteral("bt2020nc"));
  QCOMPARE(plan.source.matrix, QStringLiteral("bt2020nc"));
  QCOMPARE(plan.source.range, QStringLiteral("full"));
  QCOMPARE(plan.source.siting, QStringLiteral("top"));
  QCOMPARE(plan.source.bitDepth, 10);
  QCOMPARE(plan.source.fps.value(), (Rational{24, 1}));
  QCOMPARE(plan.source.codedSize.value(), QSize(1920, 1080));
}

void TestNativePrepPlan::aResolvedPlanCarriesItsRealDeviceIdentity() {
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), hdSource());

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.device.decoderDeviceUuid, QStringLiteral("GPU-decode-0000"));
  QCOMPARE(plan.device.computeDeviceUuid, QStringLiteral("GPU-compute-0001"));
  QCOMPARE(plan.device.adapterLuid, QStringLiteral("0x00000000:0x00001234"));
  QCOMPARE(plan.device.smMajor, 12);
  QCOMPARE(plan.device.smMinor, 0);
}

void TestNativePrepPlan::aPartiallyObservedDeviceStaysPreparing() {
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), hdSource());
  request.device.computeDeviceUuid.clear();
  request.device.smMajor = 0;

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QVERIFY(plan.blockers.isEmpty());
  QCOMPARE(plan.availability.value, Availability::Preparing);
  QVERIFY(mentions(plan.unknowns, QStringLiteral("device identity")));
}

void TestNativePrepPlan::aNonMultipleOf32SourcePadsItsTemporalTensors() {
  PlanRequest request = requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{},
                 temporalOf(TemporalOrder::AfterSpatial)),
      catalogOf({rifeRow()}), hdSource(1920, 1072));

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.stages.first().contentGeometry, QSize(1920, 1072));
  QCOMPARE(plan.stages.first().inputGeometry, QSize(1920, 1088));
  QCOMPARE(plan.stages.first().padT, 8);
  QCOMPARE(plan.outputGeometry().value(), QSize(1920, 1072));
}

void TestNativePrepPlan::a23976SourceInterpolatesTo48000Over1001() {
  SourceGeometry source = hdSource();
  source.fps = Rational{24000, 1001};
  PlanRequest request = requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{},
                 temporalOf(TemporalOrder::AfterSpatial)),
      catalogOf({rifeRow()}), source);

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.stages.first().inputFps, (Rational{24000, 1001}));
  QCOMPARE(plan.outputCadence().value(), (Rational{48000, 1001}));
}

void TestNativePrepPlan::aCroppedSourcePlansAtTheCroppedSize() {
  SourceGeometry source = hdSource();
  source.crop = QRect(0, 0, 1920, 1072);
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"),
                            profile(QSize(1920, 1072), QSize(3840, 2144)))}),
      source);

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.source.crop.value(), QRect(0, 0, 1920, 1072));
  QCOMPARE(plan.stages.first().inputGeometry, QSize(1920, 1072));
  QCOMPARE(plan.stages.first().outputGeometry, QSize(3840, 2144));
  QCOMPARE(plan.outputGeometry().value(), QSize(3840, 2144));
}

void TestNativePrepPlan::aTenBitPortraitSourcePlansAsPortraitWithoutTransposing() {
  SourceGeometry portrait = hdSource(1080, 1920);
  portrait.bitDepth = 10;
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"),
                    {stageOf(QStringLiteral("builtin.portrait"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.portrait"),
                            profile(QSize(1080, 1920), QSize(2160, 3840)))}),
      portrait);

  PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.source.bitDepth, 10);
  QCOMPARE(plan.stages.first().inputGeometry, QSize(1080, 1920));
  QCOMPARE(plan.outputGeometry().value(), QSize(2160, 3840));

  // A landscape-declared graph is refused rather than transposed to fit.
  request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), portrait);
  plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("stage.geometry.mismatch"));
}

void TestNativePrepPlan::identicalGeometrySharesOneStagePayload() {
  SourceGeometry limited = hdSource();
  SourceGeometry full = hdSource();
  full.csp = QStringLiteral("bt2020nc");
  full.matrix = QStringLiteral("bt2020nc");
  full.range = QStringLiteral("full");
  full.bitDepth = 10;

  const PreparationPlan planA = resolvePreparationPlan(requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), limited));
  const PreparationPlan planB = resolvePreparationPlan(requestOf(
      spatialRecipe(QStringLiteral("janai.other.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), full));

  QVERIFY(planA.isResolved());
  QVERIFY(planB.isResolved());
  QCOMPARE(planA.stages.first().buildRequestPayload(),
           planB.stages.first().buildRequestPayload());
  // The payload carries no source fact: colour tags and cadence stay out of it.
  QVERIFY(!planA.stages.first().buildRequestPayload().contains(QStringLiteral("bt2020nc")));
  QVERIFY(!planA.stages.first().buildRequestPayload().contains(planA.id));
  QVERIFY(planA.id != planB.id);
}

void TestNativePrepPlan::thePlanIdIsStableAndCoversTheInterpolation() {
  const CatalogView catalog = catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x), rifeRow()});
  const PlanRequest doubled = requestOf(
      rifeRecipe(QStringLiteral("janai.test.combined.v1"), {stageOf(QStringLiteral("builtin.hd"))},
                 temporalOf(TemporalOrder::AfterSpatial, Rational{2, 1})),
      catalog, hdSource());

  const PreparationPlan first = resolvePreparationPlan(doubled);
  const PreparationPlan second = resolvePreparationPlan(doubled);
  QVERIFY(first.isResolved());
  QCOMPARE(first.id, second.id);

  // Same graph and geometry, a different interpolation factor: a different plan.
  const PreparationPlan tripled = resolvePreparationPlan(requestOf(
      rifeRecipe(QStringLiteral("janai.test.combined.v1"), {stageOf(QStringLiteral("builtin.hd"))},
                 temporalOf(TemporalOrder::AfterSpatial, Rational{3, 1})),
      catalog, hdSource()));
  QVERIFY(tripled.isResolved());
  QCOMPARE(tripled.outputCadence().value(), (Rational{72, 1}));
  QVERIFY(tripled.id != first.id);
}

void TestNativePrepPlan::aNinetyDegreeRotationPlansAsPortraitAndRefusesALandscapeGraph() {
  const CatalogView catalog = catalogOf(
      {spatialRow(QStringLiteral("builtin.portrait"),
                  profile(QSize(1080, 1920), QSize(2160, 3840))),
       spatialRow(QStringLiteral("builtin.hd"), kHd2x)});

  // 90 and 270 both present the coded frame as a portrait display geometry.
  SourceGeometry rotated = hdSource();
  rotated.rotationDegrees = 90;
  QCOMPARE(rotated.displaySize().value(), QSize(1080, 1920));

  PreparationPlan plan = resolvePreparationPlan(requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"),
                    {stageOf(QStringLiteral("builtin.portrait"))}),
      catalog, rotated));
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.stages.size(), 1);
  QCOMPARE(plan.stages.first().inputGeometry, QSize(1080, 1920));
  QCOMPARE(plan.stages.first().outputGeometry, QSize(2160, 3840));
  QCOMPARE(plan.outputGeometry().value(), QSize(2160, 3840));

  SourceGeometry turned = hdSource();
  turned.rotationDegrees = 270;
  QCOMPARE(turned.displaySize().value(), QSize(1080, 1920));
  plan = resolvePreparationPlan(requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"),
                    {stageOf(QStringLiteral("builtin.portrait"))}),
      catalog, turned));
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.stages.first().inputGeometry, QSize(1080, 1920));
  QCOMPARE(plan.outputGeometry().value(), QSize(2160, 3840));

  // A landscape-declared graph is refused rather than transposed to fit.
  plan = resolvePreparationPlan(requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"),
                    {stageOf(QStringLiteral("builtin.hd"))}),
      catalog, rotated));
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("stage.geometry.mismatch"));
  QVERIFY(plan.stages.isEmpty());
}

void TestNativePrepPlan::aPortraitTemporalStagePadsThePortraitWidth() {
  PlanRequest request = requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{},
                 temporalOf(TemporalOrder::AfterSpatial)),
      catalogOf({rifeRow()}), hdSource(1080, 1920));

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  const ResolvedStage& stage = plan.stages.first();
  QCOMPARE(stage.contentGeometry, QSize(1080, 1920));
  // The width, not the height, is the axis that needs padding here.
  QCOMPARE(stage.inputGeometry, QSize(1088, 1920));
  QCOMPARE(stage.padL, 4);
  QCOMPARE(stage.padT, 0);
  QCOMPARE(stage.inputShape(), QStringLiteral("1x11x1920x1088"));
  QCOMPARE(stage.outputShape(), QStringLiteral("1x3x1920x1088"));
  QCOMPARE(plan.outputGeometry().value(), QSize(1080, 1920));
}

void TestNativePrepPlan::aCroppedTemporalStagePadsTheCroppedSize() {
  SourceGeometry source = hdSource();
  source.crop = QRect(2, 4, 1916, 1064);

  PlanRequest request = requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{},
                 temporalOf(TemporalOrder::AfterSpatial)),
      catalogOf({rifeRow()}), source);

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.source.crop.value(), QRect(2, 4, 1916, 1064));

  const ResolvedStage& stage = plan.stages.first();
  QCOMPARE(stage.contentGeometry, QSize(1916, 1064));
  QCOMPARE(stage.inputGeometry, QSize(1920, 1088));
  QCOMPARE(stage.padL, 2);
  QCOMPARE(stage.padT, 12);
  QCOMPARE(plan.outputGeometry().value(), QSize(1916, 1064));
}

void TestNativePrepPlan::aNonZeroOriginCropPlansAtTheCropSize() {
  SourceGeometry source = hdSource();
  source.crop = QRect(64, 32, 1792, 1024);

  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"),
                    {stageOf(QStringLiteral("builtin.crop"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.crop"),
                            profile(QSize(1792, 1024), QSize(3584, 2048)))}),
      source);

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.source.crop.value(), QRect(64, 32, 1792, 1024));
  QCOMPARE(plan.source.displaySize().value(), QSize(1792, 1024));
  QCOMPARE(plan.stages.first().inputGeometry, QSize(1792, 1024));
  QCOMPARE(plan.stages.first().outputGeometry, QSize(3584, 2048));
  QCOMPARE(plan.stages.first().scale.value(), 2);
  QCOMPARE(plan.outputGeometry().value(), QSize(3584, 2048));

  // The origin is dropped from the geometry, and rotation applies after the crop.
  SourceGeometry rotated = source;
  rotated.rotationDegrees = 90;
  QCOMPARE(rotated.displaySize().value(), QSize(1024, 1792));
}

void TestNativePrepPlan::aFullRangePcSourceResolvesUnchanged() {
  SourceGeometry pc = hdSource();
  pc.range = QStringLiteral("full");

  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), pc);

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.source.range, QStringLiteral("full"));
  QCOMPARE(plan.stages.first().inputGeometry, QSize(1920, 1080));
  QCOMPARE(plan.outputGeometry().value(), QSize(3840, 2160));
  QVERIFY(plan.canonicalBody().contains(QStringLiteral("range=full")));

  // The range enters the plan's identity, not the engine's payload.
  const PreparationPlan limited = resolvePreparationPlan(requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.hd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.hd"), kHd2x)}), hdSource()));
  QVERIFY(plan.id != limited.id);
  QCOMPARE(plan.stages.first().buildRequestPayload(),
           limited.stages.first().buildRequestPayload());
}

void TestNativePrepPlan::aTenBitPortraitSourceAt23976Interpolates() {
  SourceGeometry portrait = hdSource(1080, 1920);
  portrait.bitDepth = 10;
  portrait.fps = Rational{24000, 1001};

  PlanRequest request = requestOf(
      rifeRecipe(QStringLiteral("janai.test.combined.v1"),
                 {stageOf(QStringLiteral("builtin.portrait"))},
                 temporalOf(TemporalOrder::AfterSpatial)),
      catalogOf({spatialRow(QStringLiteral("builtin.portrait"),
                            profile(QSize(1080, 1920), QSize(2160, 3840))),
                 rifeRow()}),
      portrait);

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.source.bitDepth, 10);
  QCOMPARE(plan.stages.size(), 2);

  const ResolvedStage& spatial = plan.stages.first();
  QCOMPARE(spatial.kind, StageKind::Spatial);
  QCOMPARE(spatial.recipeIndex, 0);
  QCOMPARE(spatial.inputGeometry, QSize(1080, 1920));
  QCOMPARE(spatial.outputGeometry, QSize(2160, 3840));
  QCOMPARE(spatial.inputFps, (Rational{24000, 1001}));
  QCOMPARE(spatial.outputFps, (Rational{24000, 1001}));

  const ResolvedStage& temporal = plan.stages.last();
  QCOMPARE(temporal.kind, StageKind::Temporal);
  QCOMPARE(temporal.recipeIndex, -1);
  QCOMPARE(temporal.contentGeometry, QSize(2160, 3840));
  QCOMPARE(temporal.inputGeometry, QSize(2176, 3840));
  // 2160 rounds up to 2176: a 16-pixel border halves to 8 and stays even on
  // the width axis, while 3840 is already a multiple of 64.
  QCOMPARE(temporal.padL, 8);
  QCOMPARE(temporal.padT, 0);
  QCOMPARE(temporal.inputFps, (Rational{24000, 1001}));
  QCOMPARE(temporal.outputFps, (Rational{48000, 1001}));
  QCOMPARE(plan.outputGeometry().value(), QSize(2160, 3840));
  QCOMPARE(plan.outputCadence().value(), (Rational{48000, 1001}));
}

void TestNativePrepPlan::anOddHeightSourcePadsAsymmetrically() {
  int padL = -1;
  int padT = -1;

  // A 7-pixel border halves to 3 and rounds down to 2, so the pad is asymmetric.
  QCOMPARE(padToTensorAlignment(QSize(1920, 1081), &padL, &padT), QSize(1920, 1088));
  QCOMPARE(paddingExtent(QSize(1920, 1081)), QSize(0, 7));
  QCOMPARE(padL, 0);
  QCOMPARE(padT, 2);

  QCOMPARE(padToTensorAlignment(QSize(1081, 1920), &padL, &padT), QSize(1088, 1920));
  QCOMPARE(paddingExtent(QSize(1081, 1920)), QSize(7, 0));
  QCOMPARE(padL, 2);
  QCOMPARE(padT, 0);

  // The odd height reaches the temporal stage and is padded there.
  PlanRequest request = requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{},
                 temporalOf(TemporalOrder::AfterSpatial)),
      catalogOf({rifeRow()}), hdSource(1920, 1081));

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.stages.first().contentGeometry, QSize(1920, 1081));
  QCOMPARE(plan.stages.first().inputGeometry, QSize(1920, 1088));
  QCOMPARE(plan.stages.first().padT, 2);
  QCOMPARE(plan.outputGeometry().value(), QSize(1920, 1081));
}

void TestNativePrepPlan::aTwoStageSpatialChainThreadsGeometryAndRecipeIndex() {
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"),
                    {stageWithPreResize(QStringLiteral("builtin.first"), QSize(1280, 720)),
                     stageWithPreResize(QStringLiteral("builtin.second"), QSize(1920, 1080))}),
      catalogOf({spatialRow(QStringLiteral("builtin.first"),
                            profile(QSize(1280, 720), QSize(2560, 1440))),
                 spatialRow(QStringLiteral("builtin.second"),
                            profile(QSize(1920, 1080), QSize(3840, 2160)))}),
      hdSource());

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.stages.size(), 2);
  QCOMPARE(plan.resizes.size(), 2);

  const ResolvedStage& stage0 = plan.stages.at(0);
  QCOMPARE(stage0.recipeIndex, 0);
  QCOMPARE(stage0.modelId, QStringLiteral("builtin.first"));
  QCOMPARE(stage0.inputGeometry, QSize(1280, 720));
  QCOMPARE(stage0.outputGeometry, QSize(2560, 1440));

  const ResolvedStage& stage1 = plan.stages.at(1);
  QCOMPARE(stage1.recipeIndex, 1);
  QCOMPARE(stage1.modelId, QStringLiteral("builtin.second"));
  QCOMPARE(stage1.inputGeometry, QSize(1920, 1080));
  QCOMPARE(stage1.outputGeometry, QSize(3840, 2160));
  QCOMPARE(stage1.inputFps, (Rational{24, 1}));

  // Each pre-resize is threaded to its own stage, and the second one consumes the
  // first stage's content before the second model runs.
  const PreResizeStep& first = plan.resizes.at(0);
  QCOMPARE(first.stageIndex, 0);
  QCOMPARE(first.modelId, QStringLiteral("builtin.first"));
  QCOMPARE(first.from, QSize(1920, 1080));
  QCOMPARE(first.to, QSize(1280, 720));
  QVERIFY(!first.beforeTemporal);

  const PreResizeStep& second = plan.resizes.at(1);
  QCOMPARE(second.stageIndex, 1);
  QCOMPARE(second.modelId, QStringLiteral("builtin.second"));
  QCOMPARE(second.from, stage0.contentGeometry);
  QCOMPARE(second.to, QSize(1920, 1080));
  QVERIFY(!second.beforeTemporal);

  QCOMPARE(plan.outputGeometry().value(), QSize(3840, 2160));
  QCOMPARE(plan.outputCadence().value(), (Rational{24, 1}));
}

void TestNativePrepPlan::anSdSourcePlansAndAnSdWindowRefusesBelowItsMinimum() {
  const SourceGeometry sd = hdSource(320, 180);

  PreparationPlan plan = resolvePreparationPlan(requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.sd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.sd"),
                            profile(QSize(320, 180), QSize(640, 360)))}),
      sd));
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.source.pixelCount().value(), static_cast<qint64>(57600));
  QCOMPARE(plan.stages.first().inputGeometry, QSize(320, 180));
  QCOMPARE(plan.stages.first().outputGeometry, QSize(640, 360));
  QCOMPARE(plan.outputGeometry().value(), QSize(640, 360));

  // SD RIFE tensors pad to the same centred even multiple of 64.
  plan = resolvePreparationPlan(requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{},
                 temporalOf(TemporalOrder::AfterSpatial)),
      catalogOf({rifeRow()}), sd));
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.stages.first().inputGeometry, QSize(320, 192));
  QCOMPARE(plan.stages.first().padT, 6);
  QCOMPARE(plan.outputGeometry().value(), QSize(320, 180));

  // A real SD window (1280x720 up to 1920x1080) refuses the 320x180 source below it.
  PlanRequest request = requestOf(
      spatialRecipe(QStringLiteral("janai.test.spatial.v1"), {stageOf(QStringLiteral("builtin.sd"))}),
      catalogOf({spatialRow(QStringLiteral("builtin.sd"),
                            profile(QSize(320, 180), QSize(640, 360)))}),
      sd);
  request.window.minPixels = 1280LL * 720LL;
  request.window.maxPixels = 1920LL * 1080LL;

  plan = resolvePreparationPlan(request);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("source.window.pixels"));
  QVERIFY(plan.blockers.first().reason.contains(QStringLiteral("57600")));
  QVERIFY(plan.stages.isEmpty());
}

void TestNativePrepPlan::aPinnedTemporalStageResolvesToItsOwnedContentId() {
  struct VariantCase {
    RifeVariant variant;
    bool ensemble;
  };
  const VariantCase cases[] = {
      {RifeVariant::Full, false},
      {RifeVariant::Lite, false},
      {RifeVariant::Full, true},
      {RifeVariant::Lite, true},
  };

  QSet<QString> ownedIds;
  for (const VariantCase& value : cases) {
    const ModelRecord owned = rifeRow(value.variant, value.ensemble);
    TemporalStage temporal = temporalOf(TemporalOrder::AfterSpatial);
    temporal.variant = value.variant;
    temporal.ensemble = value.ensemble;

    PlanRequest request = requestOf(
        rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{}, temporal),
        catalogOf({owned}), hdSource());

    const PreparationPlan plan = resolvePreparationPlan(request);
    QVERIFY(plan.isResolved());
    QCOMPARE(plan.stages.size(), 1);
    const ResolvedStage& stage = plan.stages.first();
    QCOMPARE(stage.kind, StageKind::Temporal);
    // The plan carries the owned graph's content-addressed id and content id -
    // not the curated variant name, which is only the request. Each curated
    // variant resolves to its own record, never a shared alias.
    QCOMPARE(stage.modelId, owned.id);
    QVERIFY(stage.modelId != rifeModelName(RifeVersion::V4_14, value.variant, value.ensemble));
    QCOMPARE(stage.modelContentId, owned.contentId());
    QVERIFY(!ownedIds.contains(owned.id));
    ownedIds.insert(owned.id);
  }
  QCOMPARE(ownedIds.size(), 4);
}

void TestNativePrepPlan::aDifferentPinnedVariantIsNeverSubstituted() {
  // Only lite's pinned graph is owned; a request for the full variant must not be
  // answered by lite's row or by any other.
  const ModelRecord lite = rifeRow(RifeVariant::Lite, false);
  PlanRequest fullRequest = requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{},
                 temporalOf(TemporalOrder::AfterSpatial)),
      catalogOf({lite}), hdSource());
  PreparationPlan plan = resolvePreparationPlan(fullRequest);
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("rife.model.missing"));
  QVERIFY(plan.blockers.first().reason.contains(
      rifeModelPin(RifeVersion::V4_14, RifeVariant::Full, false)->graphSha256));
  QVERIFY(plan.stages.isEmpty());

  // The exact lite pin does resolve, to lite's own row.
  TemporalStage liteStage = temporalOf(TemporalOrder::AfterSpatial);
  liteStage.variant = RifeVariant::Lite;
  plan = resolvePreparationPlan(requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{}, liteStage),
      catalogOf({lite}), hdSource()));
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.stages.first().modelId, lite.id);
}

void TestNativePrepPlan::aCuratedNameAliasIsNotThePinnedGraph() {
  // A row that merely claims a curated name carries another graph. It is never
  // the model: the join is by content digest.
  ModelRecord alias;
  alias.id = QStringLiteral("rife_v4.14_lite");
  alias.displayName = alias.id;
  alias.origin = ModelOrigin::Builtin;
  alias.onnxPath = QStringLiteral("C:/models/alias.onnx");
  alias.onnxSha256 = kShaA;
  alias.onnxBytes = 2048;
  alias.role = ModelRole::Temporal;
  alias.actualProfile = temporalProfile();
  alias.verifiedReferenceProfile = temporalReference();
  alias.availableBackends = {Backend::TensorRT};
  alias.availability = readyAvailability();

  const ModelRecord lite = rifeRow(RifeVariant::Lite, false);
  TemporalStage liteStage = temporalOf(TemporalOrder::AfterSpatial);
  liteStage.variant = RifeVariant::Lite;

  // With both rows present the pinned lite graph resolves; the alias is ignored.
  PreparationPlan plan = resolvePreparationPlan(requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{}, liteStage),
      catalogOf({alias, lite}), hdSource()));
  QVERIFY(plan.isResolved());
  QCOMPARE(plan.stages.first().modelId, lite.id);
  QVERIFY(plan.stages.first().modelId != alias.id);

  // With only the alias present the pin is missing, not silently satisfied.
  plan = resolvePreparationPlan(requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{}, liteStage),
      catalogOf({alias}), hdSource()));
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("rife.model.missing"));
}

void TestNativePrepPlan::aCorruptPinnedRowKeepsItsOwnReason() {
  ModelRecord corrupt = rifeRow();
  corrupt.availability = unavailableAvailability(
      QStringLiteral("model.store.corrupt"),
      QStringLiteral("the owned copy no longer matches the row's identity"));

  const PreparationPlan plan = resolvePreparationPlan(requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{},
                 temporalOf(TemporalOrder::AfterSpatial)),
      catalogOf({corrupt}), hdSource()));
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("model.store.corrupt"));
  QVERIFY(plan.stages.isEmpty());
}

void TestNativePrepPlan::aWrongRifeBoundaryIsRefusedByName() {
  ModelRecord wrong = rifeRow();
  ModelProfile notRife = temporalProfile();
  notRife.input.channels = 3;
  notRife.output.channels = 3;
  wrong.actualProfile = notRife;
  // The id must still derive from the declared boundary, so this is a genuine
  // row that declares the wrong thing, not a tampered id.
  wrong.id = userModelIdFor(
      computeContentId(ModelRole::Temporal, wrong.onnxSha256, notRife));

  const PreparationPlan plan = resolvePreparationPlan(requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{},
                 temporalOf(TemporalOrder::AfterSpatial)),
      catalogOf({wrong}), hdSource()));
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("rife.model.boundary"));
  QVERIFY(plan.stages.isEmpty());
}

void TestNativePrepPlan::aPinnedRowWithoutAReferenceProofIsRefused() {
  ModelRecord unproved = rifeRow();
  unproved.verifiedReferenceProfile.reset();

  const PreparationPlan plan = resolvePreparationPlan(requestOf(
      rifeRecipe(QStringLiteral("janai.test.rife.v1"), QVector<SpatialStage>{},
                 temporalOf(TemporalOrder::AfterSpatial)),
      catalogOf({unproved}), hdSource()));
  QVERIFY(!plan.canPrepare());
  QCOMPARE(plan.blockers.first().reasonKey, QStringLiteral("rife.model.reference"));
  QVERIFY(plan.stages.isEmpty());
}
