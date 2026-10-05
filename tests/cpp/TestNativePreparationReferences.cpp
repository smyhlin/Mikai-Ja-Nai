// The pure boundaries the whole-plan reference lineage is built on: the ordered
// geometry transitions between stages, the original's eleven-channel timestep /
// mesh / multiplier packing, the centred pad and its crop, and the Spline36
// resampler. These are held to the original's own bytes and arithmetic, not to a
// wiring echo: no test here asserts that a file was written for its own sake.

#include "TestNativePreparationReferences.hpp"

#include <cmath>
#include <cstring>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/QtEndian>

#include "src/player/native/NativePreparationReferences.hpp"

using namespace mplayer::native;

namespace {

ResolvedStage spatialStage(const QString& id, const QSize& input, const QSize& output) {
  ResolvedStage stage;
  stage.kind = StageKind::Spatial;
  stage.role = ModelRole::Spatial;
  stage.modelId = id;
  stage.inputGeometry = input;
  stage.outputGeometry = output;
  stage.contentGeometry = output;
  stage.inputChannels = 3;
  stage.outputChannels = 3;
  stage.scale = output.width() / input.width();
  return stage;
}

ResolvedStage temporalStage(const QString& id, const QSize& content, TemporalOrder order,
                            const Rational& factor) {
  ResolvedStage stage;
  stage.kind = StageKind::Temporal;
  stage.role = ModelRole::Temporal;
  stage.modelId = id;
  stage.contentGeometry = content;
  stage.inputGeometry = padToTensorAlignment(content, &stage.padL, &stage.padT);
  stage.outputGeometry = stage.inputGeometry;
  stage.inputChannels = 11;
  stage.outputChannels = 3;
  stage.order = order;
  stage.factor = factor;
  return stage;
}

SourceGeometry sourceAt(const QSize& coded) {
  SourceGeometry source;
  source.codedSize = coded;
  source.fps = Rational{24, 1};
  return source;
}

/// A planar fp16 buffer built from raw half bit patterns. The packers under test
/// only read and copy these bits, so the test names the exact halves it feeds.
QByteArray planarHalves(const QSize& size, int channels, const QVector<quint16>& halves) {
  QByteArray bytes(static_cast<qsizetype>(size.width()) * size.height() * channels * 2, '\0');
  for (qsizetype index = 0; index < halves.size(); ++index) {
    const quint16 little = qToLittleEndian<quint16>(halves.at(index));
    memcpy(bytes.data() + index * 2, &little, 2);
  }
  return bytes;
}

quint16 halfAt(const QByteArray& bytes, int channel, const QSize& size, int x, int y) {
  const qint64 plane = static_cast<qint64>(size.width()) * size.height();
  const qint64 index =
      static_cast<qint64>(channel) * plane + static_cast<qint64>(y) * size.width() + x;
  return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(bytes.constData()) + index * 2);
}

FrameFixture solidFrame(const QSize& size) {
  FrameFixture fixture;
  fixture.size = size;
  fixture.rgb.resize(static_cast<qsizetype>(size.width()) * size.height() * 3);
  for (int y = 0; y < size.height(); ++y) {
    for (int x = 0; x < size.width(); ++x) {
      const int value = y * size.width() + x;
      const qsizetype offset = (static_cast<qsizetype>(y) * size.width() + x) * 3;
      fixture.rgb[offset] = static_cast<char>(value);
      fixture.rgb[offset + 1] = static_cast<char>(value);
      fixture.rgb[offset + 2] = static_cast<char>(value);
    }
  }
  return fixture;
}

/// The halves of n/16 for n in 0..15, each exactly representable in fp16.
const quint16 kSixteenths[16] = {0x0000, 0x2C00, 0x3000, 0x3200, 0x3400, 0x3500, 0x3600, 0x3700,
                                 0x3800, 0x3880, 0x3900, 0x3980, 0x3A00, 0x3A80, 0x3B00, 0x3B80};

PlanReferenceRequest requestFor(const PreparationPlan& plan, const QSize& frameSize,
                                int frameCount) {
  PlanReferenceRequest request;
  request.plan = plan;
  request.source = plan.source;
  for (int index = 0; index < frameCount; ++index) {
    ReferenceFrame frame;
    frame.fixture = solidFrame(frameSize);
    frame.unprocessed = true;
    frame.sourceIndex = index;
    request.frames.append(frame);
  }
  for (const ResolvedStage& stage : plan.stages) {
    StoredModel model;
    model.record.id = stage.modelId;
    request.models.append(model);
  }
  return request;
}

}  // namespace

void TestNativePreparationReferences::aSpatialChainThreadsEachStagesOutputForward() {
  PreparationPlan plan;
  plan.source = sourceAt(QSize(320, 180));
  plan.stages = {spatialStage(QStringLiteral("a"), QSize(320, 180), QSize(640, 360)),
                 spatialStage(QStringLiteral("b"), QSize(640, 360), QSize(1280, 720))};

  QVector<StageInputPlan> inputs;
  QString key;
  QString reason;
  QVERIFY(planStageInputs(plan, 1, &inputs, &key, &reason));
  QCOMPARE(inputs.size(), 2);

  QCOMPARE(inputs.at(0).source, StageInputSource::CapturedFrameA);
  QCOMPARE(inputs.at(0).contentGeometry, QSize(320, 180));
  QCOMPARE(inputs.at(0).tensorGeometry, QSize(320, 180));
  QCOMPARE(inputs.at(0).channels, 3);
  QVERIFY(!inputs.at(0).resizeNamed);

  // The second stage consumes the first stage's real output, not the source.
  QCOMPARE(inputs.at(1).source, StageInputSource::PreviousStage);
  QCOMPARE(inputs.at(1).contentGeometry, QSize(640, 360));
  QCOMPARE(inputs.at(1).tensorGeometry, QSize(640, 360));
  QVERIFY(!inputs.at(1).resizeNamed);
}

void TestNativePreparationReferences::aNamedPreResizeIsTheOnlyResizeTheChainApplies() {
  PreparationPlan plan;
  plan.source = sourceAt(QSize(320, 180));
  plan.stages = {spatialStage(QStringLiteral("a"), QSize(160, 90), QSize(320, 180)),
                 spatialStage(QStringLiteral("b"), QSize(320, 180), QSize(640, 360))};
  PreResizeStep step;
  step.stageIndex = 0;
  step.modelId = QStringLiteral("a");
  step.from = QSize(320, 180);
  step.to = QSize(160, 90);
  step.beforeTemporal = false;
  plan.resizes = {step};

  QVector<StageInputPlan> inputs;
  QString key;
  QString reason;
  QVERIFY(planStageInputs(plan, 1, &inputs, &key, &reason));
  QVERIFY(inputs.at(0).resizeNamed);
  QCOMPARE(inputs.at(0).resizeFrom, QSize(320, 180));
  QCOMPARE(inputs.at(0).resizeTo, QSize(160, 90));
  // The second stage names no resize, so the first stage's output flows in
  // unchanged rather than the source being resized again.
  QVERIFY(!inputs.at(1).resizeNamed);
  QCOMPARE(inputs.at(1).contentGeometry, QSize(320, 180));
}

void TestNativePreparationReferences::interpolationBeforeSpatialConsumesTheCapturedPair() {
  PreparationPlan plan;
  plan.source = sourceAt(QSize(320, 180));
  plan.stages = {temporalStage(QStringLiteral("rife"), QSize(160, 90),
                               TemporalOrder::BeforeSpatial, Rational{2, 1}),
                 spatialStage(QStringLiteral("a"), QSize(160, 90), QSize(320, 180))};
  PreResizeStep hoisted;
  hoisted.stageIndex = -1;
  hoisted.modelId = QStringLiteral("a");
  hoisted.from = QSize(320, 180);
  hoisted.to = QSize(160, 90);
  hoisted.beforeTemporal = true;
  plan.resizes = {hoisted};

  QVector<StageInputPlan> inputs;
  QString key;
  QString reason;
  QVERIFY(planStageInputs(plan, 2, &inputs, &key, &reason));

  QCOMPARE(inputs.at(0).source, StageInputSource::CapturedPair);
  QCOMPARE(inputs.at(0).channels, 11);
  QCOMPARE(inputs.at(0).contentGeometry, QSize(160, 90));
  QCOMPARE(inputs.at(0).tensorGeometry, QSize(192, 128));
  QCOMPARE(inputs.at(0).padL, 16);
  QCOMPARE(inputs.at(0).padT, 18);
  QVERIFY(inputs.at(0).resizeNamed);
  QCOMPARE(inputs.at(0).resizeFrom, QSize(320, 180));
  QCOMPARE(inputs.at(0).resizeTo, QSize(160, 90));

  // The upscale chain starts at the interpolation output, at the content
  // geometry, with no second resize.
  QCOMPARE(inputs.at(1).source, StageInputSource::PreviousStage);
  QCOMPARE(inputs.at(1).contentGeometry, QSize(160, 90));
  QCOMPARE(inputs.at(1).channels, 3);
  QVERIFY(!inputs.at(1).resizeNamed);
}

void TestNativePreparationReferences::interpolationAfterSpatialConsumesBothChainOutputs() {
  PreparationPlan plan;
  plan.source = sourceAt(QSize(320, 180));
  plan.stages = {spatialStage(QStringLiteral("a"), QSize(320, 180), QSize(640, 360)),
                 temporalStage(QStringLiteral("rife"), QSize(640, 360),
                               TemporalOrder::AfterSpatial, Rational{2, 1})};

  QVector<StageInputPlan> inputs;
  QString key;
  QString reason;
  QVERIFY(planStageInputs(plan, 2, &inputs, &key, &reason));

  QCOMPARE(inputs.at(0).source, StageInputSource::CapturedFrameA);
  QCOMPARE(inputs.at(1).source, StageInputSource::ChainPair);
  QCOMPARE(inputs.at(1).channels, 11);
  QCOMPARE(inputs.at(1).contentGeometry, QSize(640, 360));
  QCOMPARE(inputs.at(1).tensorGeometry, QSize(640, 384));
  QCOMPARE(inputs.at(1).padL, 0);
  QCOMPARE(inputs.at(1).padT, 12);
  QCOMPARE(inputs.at(1).outputContentGeometry, QSize(640, 360));
}

void TestNativePreparationReferences::twoTemporalStagesAreRefused() {
  PreparationPlan plan;
  plan.source = sourceAt(QSize(320, 180));
  plan.stages = {temporalStage(QStringLiteral("rife"), QSize(320, 180),
                               TemporalOrder::BeforeSpatial, Rational{2, 1}),
                 temporalStage(QStringLiteral("rife2"), QSize(320, 180),
                               TemporalOrder::AfterSpatial, Rational{2, 1})};

  QVector<StageInputPlan> inputs;
  QString key;
  QString reason;
  QVERIFY(!planStageInputs(plan, 2, &inputs, &key, &reason));
  QCOMPARE(key, QStringLiteral("reference.temporal.multiple"));
  QVERIFY(inputs.isEmpty());
}

void TestNativePreparationReferences::aTemporalPlanNeedsTwoActualFrames() {
  PreparationPlan plan;
  plan.source = sourceAt(QSize(320, 180));
  plan.stages = {temporalStage(QStringLiteral("rife"), QSize(320, 180),
                               TemporalOrder::BeforeSpatial, Rational{2, 1})};

  QVector<StageInputPlan> inputs;
  QString key;
  QString reason;
  QVERIFY(!planStageInputs(plan, 1, &inputs, &key, &reason));
  QCOMPARE(key, QStringLiteral("reference.frames.pair"));
}

void TestNativePreparationReferences::aBrokenGeometryChainIsRefused() {
  PreparationPlan plan;
  plan.source = sourceAt(QSize(320, 180));
  // The first stage doubles the frame; the second declares a source-sized input
  // and names no resize, so the chain cannot be fed.
  plan.stages = {spatialStage(QStringLiteral("a"), QSize(320, 180), QSize(640, 360)),
                 spatialStage(QStringLiteral("b"), QSize(320, 180), QSize(640, 360))};

  QVector<StageInputPlan> inputs;
  QString key;
  QString reason;
  QVERIFY(!planStageInputs(plan, 1, &inputs, &key, &reason));
  QCOMPARE(key, QStringLiteral("reference.stage.chain"));
}

void TestNativePreparationReferences::anUnresolvedPlanIsRefusedByName() {
  PreparationPlan plan;
  plan.source = sourceAt(QSize(320, 180));
  plan.unknowns.append(QStringLiteral("the compute device has not been observed"));

  QVector<StageInputPlan> inputs;
  QString key;
  QString reason;
  QVERIFY(!planStageInputs(plan, 1, &inputs, &key, &reason));
  QCOMPARE(key, QStringLiteral("plan.preparing"));
}

void TestNativePreparationReferences::cropAndRotationApplyOnlyToUnprocessedFrames() {
  SourceGeometry source = sourceAt(QSize(4, 3));
  source.crop = QRect(1, 0, 2, 3);
  source.rotationDegrees = 90;

  ReferenceFrame frame;
  frame.fixture = solidFrame(QSize(4, 3));
  frame.unprocessed = true;

  FrameFixture applied;
  QString key;
  QString reason;
  QVERIFY(applySourceGeometryToFrame(frame, source, &applied, &key, &reason));
  QCOMPARE(applied.size, QSize(3, 2));
  // Crop first, then a clockwise rotation, exactly the order mpv applies them.
  const QByteArray expected = QByteArray::fromHex("0909090505050101010a0a0a060606020202");
  QCOMPARE(applied.rgb, expected);

  // A frame that says it is already display geometry is passed through only when
  // it is the display size; it is never stretched.
  ReferenceFrame processed;
  processed.fixture = solidFrame(QSize(3, 2));
  processed.unprocessed = false;
  QVERIFY(applySourceGeometryToFrame(processed, source, &applied, &key, &reason));
  QCOMPARE(applied.size, QSize(3, 2));

  processed.fixture = solidFrame(QSize(4, 3));
  QVERIFY(!applySourceGeometryToFrame(processed, source, &applied, &key, &reason));
  QCOMPARE(key, QStringLiteral("reference.frame.geometry"));
}

void TestNativePreparationReferences::aNonModelledRotationIsRefused() {
  SourceGeometry source = sourceAt(QSize(4, 3));
  source.rotationDegrees = 45;

  ReferenceFrame frame;
  frame.fixture = solidFrame(QSize(4, 3));
  frame.unprocessed = true;

  FrameFixture applied;
  QString key;
  QString reason;
  QVERIFY(!applySourceGeometryToFrame(frame, source, &applied, &key, &reason));
  QCOMPARE(key, QStringLiteral("reference.source.geometry"));
}

void TestNativePreparationReferences::theElevenChannelInputCarriesTheOriginalSemantics() {
  const QSize content(2, 2);
  const QSize padded(4, 4);
  const QVector<quint16> aHalves{0x3400, 0x3800, 0x3A00, 0x3C00, 0x3000, 0x3400, 0x3600, 0x3800,
                                 0x2C00, 0x3000, 0x3200, 0x3400};
  const QVector<quint16> bHalves(12, 0x0000);
  const QByteArray a = planarHalves(content, 3, aHalves);
  const QByteArray b = planarHalves(content, 3, bHalves);

  QByteArray packed;
  QString key;
  QString reason;
  QVERIFY(packRifeElevenChannel(a, b, content, 1, 1, padded, 0.5, &packed, &key, &reason));
  QCOMPARE(packed.size(), qsizetype(11 * 16 * 2));

  // Channel 6 is the timestep, exactly one half.
  for (int y = 0; y < padded.height(); ++y) {
    for (int x = 0; x < padded.width(); ++x) {
      QCOMPARE(halfAt(packed, 6, padded, x, y), quint16(0x3800));
    }
  }

  // Channels 7..10 are the original's constants: 2x/(w-1)-1, 2y/(h-1)-1 and the
  // two multipliers, over the padded plane.
  const quint16 mesh[4] = {0xBC00, 0xB555, 0x3555, 0x3C00};
  for (int x = 0; x < padded.width(); ++x) {
    QCOMPARE(halfAt(packed, 7, padded, x, 0), mesh[x]);
    QCOMPARE(halfAt(packed, 9, padded, x, 0), quint16(0x3955));
  }
  for (int y = 0; y < padded.height(); ++y) {
    QCOMPARE(halfAt(packed, 8, padded, 0, y), mesh[y]);
    QCOMPARE(halfAt(packed, 10, padded, 0, y), quint16(0x3955));
  }

  // The border is black and the content sits centred at (1, 1).
  for (int y = 0; y < padded.height(); ++y) {
    for (int x = 0; x < padded.width(); ++x) {
      const bool inside = x >= 1 && x <= 2 && y >= 1 && y <= 2;
      if (!inside) {
        QCOMPARE(halfAt(packed, 0, padded, x, y), quint16(0));
        QCOMPARE(halfAt(packed, 3, padded, x, y), quint16(0));
      }
    }
  }
  QCOMPARE(halfAt(packed, 0, padded, 1, 1), halfAt(a, 0, content, 0, 0));
  QCOMPARE(halfAt(packed, 0, padded, 2, 2), halfAt(a, 0, content, 1, 1));
  QCOMPARE(halfAt(packed, 5, padded, 1, 1), halfAt(b, 2, content, 0, 0));
}

void TestNativePreparationReferences::theTemporalOutputIsCroppedBackToItsContentWindow() {
  const QSize padded(4, 4);
  const QSize content(2, 2);
  QVector<quint16> halves;
  for (int channel = 0; channel < 3; ++channel) {
    for (int y = 0; y < padded.height(); ++y) {
      for (int x = 0; x < padded.width(); ++x) {
        halves.append(static_cast<quint16>(channel * 100 + y * 4 + x));
      }
    }
  }
  const QByteArray source = planarHalves(padded, 3, halves);

  QByteArray cropped;
  QString key;
  QString reason;
  QVERIFY(cropFp16Planar(source, padded, 1, 1, content, 3, &cropped, &key, &reason));
  QCOMPARE(cropped.size(), qsizetype(3 * 4 * 2));
  QCOMPARE(halfAt(cropped, 0, content, 0, 0), halfAt(source, 0, padded, 1, 1));
  QCOMPARE(halfAt(cropped, 0, content, 1, 1), halfAt(source, 0, padded, 2, 2));
  QCOMPARE(halfAt(cropped, 2, content, 1, 0), halfAt(source, 2, padded, 2, 1));

  // A window that leaves the padded tensor is refused rather than clamped.
  QVERIFY(!cropFp16Planar(source, padded, 3, 1, content, 3, &cropped, &key, &reason));
  QCOMPARE(key, QStringLiteral("reference.crop.window"));
}

void TestNativePreparationReferences::theResamplerReproducesTheOriginalSpline36() {
  const QSize from(4, 4);
  const QSize to(2, 2);
  QVector<quint16> halves;
  for (int channel = 0; channel < 3; ++channel) {
    for (int index = 0; index < 16; ++index) {
      halves.append(kSixteenths[index]);
    }
  }
  const QByteArray source = planarHalves(from, 3, halves);

  QByteArray resized;
  QString key;
  QString reason;
  QVERIFY(resampleFp16Planar(source, from, to, 3, &resized, &key, &reason));
  QCOMPARE(resized.size(), qsizetype(3 * 4 * 2));
  const quint16 expected[2][2] = {{0x2FCE, 0x342C}, {0x396A, 0x3A86}};
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      QCOMPARE(halfAt(resized, 0, to, x, y), expected[y][x]);
      QCOMPARE(halfAt(resized, 2, to, x, y), expected[y][x]);
    }
  }

  // An identity resize is exact: the spline36 weights collapse to the identity
  // kernel, so nothing is re-quantised.
  QByteArray same;
  QVERIFY(resampleFp16Planar(source, from, from, 3, &same, &key, &reason));
  QCOMPARE(same, source);

  // A source whose bytes do not match its geometry is refused, not read past.
  QVERIFY(!resampleFp16Planar(source, QSize(8, 8), to, 3, &resized, &key, &reason));
  QCOMPARE(key, QStringLiteral("reference.resize.bytes"));
}

void TestNativePreparationReferences::theFirstIntermediateTimestepIsTheOnlyOneBuilt() {
  QCOMPARE(rifeFirstTimestep(Rational{2, 1}).value_or(-1.0), 0.5);
  QCOMPARE(rifeFirstTimestep(Rational{4, 2}).value_or(-1.0), 0.5);
  QVERIFY(std::abs(rifeFirstTimestep(Rational{3, 1}).value_or(-1.0) - 1.0 / 3.0) < 1e-12);
  // A 1:1 "interpolation" has no intermediate frame to build.
  QVERIFY(!rifeFirstTimestep(Rational{1, 1}).has_value());
  QVERIFY(!rifeFirstTimestep(Rational{1, 2}).has_value());
  QVERIFY(!rifeFirstTimestep(Rational{0, 1}).has_value());
}

void TestNativePreparationReferences::anAbsentInspectorIsRefusedByName() {
  PreparationPlan plan;
  plan.source = sourceAt(QSize(320, 180));
  plan.stages = {spatialStage(QStringLiteral("a"), QSize(320, 180), QSize(640, 360))};

  PlanReferenceRequest request = requestFor(plan, QSize(320, 180), 1);
  request.tools.inspectExecutable = QStringLiteral("C:/definitely/not/here/native_inspect.exe");
  request.tools.ortRuntimeDir = QStringLiteral("C:/definitely/not/here");

  QVector<BuilderStageReference> references;
  QString key;
  QString reason;
  QVERIFY(!buildPlanReferenceLineage(request, &references, &key, &reason));
  QCOMPARE(key, QStringLiteral("tool.missing"));
  QVERIFY(references.isEmpty());
  QVERIFY(!reason.isEmpty());
}

void TestNativePreparationReferences::aCancelledRunSendsNoReferences() {
  QTemporaryDir scratch;
  QVERIFY(scratch.isValid());
  const QString ortDirectory = scratch.filePath(QStringLiteral("ort"));
  QVERIFY(QDir().mkpath(ortDirectory));
  QFile module(QDir(ortDirectory).filePath(QStringLiteral("onnxruntime.dll")));
  QVERIFY(module.open(QIODevice::WriteOnly));
  module.write("not a real runtime", 18);
  module.close();
  const QString inspector = scratch.filePath(QStringLiteral("native_inspect.exe"));
  QFile inspectorFile(inspector);
  QVERIFY(inspectorFile.open(QIODevice::WriteOnly));
  inspectorFile.write("stub", 4);
  inspectorFile.close();

  PreparationPlan plan;
  plan.source = sourceAt(QSize(320, 180));
  plan.stages = {spatialStage(QStringLiteral("a"), QSize(320, 180), QSize(640, 360))};

  PlanReferenceRequest request = requestFor(plan, QSize(320, 180), 1);
  request.tools.inspectExecutable = inspector;
  request.tools.ortRuntimeDir = ortDirectory;
  request.scratchDirectory = scratch.path();

  NativeBuildCancellation cancellation;
  QCOMPARE(cancellation.cancel(), NativeBuildCancellation::CancelResult::CancelledNow);
  request.cancellation = &cancellation;

  QVector<BuilderStageReference> references;
  QString key;
  QString reason;
  QVERIFY(!buildPlanReferenceLineage(request, &references, &key, &reason));
  QCOMPARE(key, QStringLiteral("native.prepare.cancelled"));
  QVERIFY(references.isEmpty());
  // Nothing was started and nothing was written.
  QVERIFY(!QFileInfo::exists(scratch.filePath(QStringLiteral("stage-0-input-fp16.bin"))));
  QVERIFY(!QFileInfo::exists(scratch.filePath(QStringLiteral("stage-0-reference"))));
}
