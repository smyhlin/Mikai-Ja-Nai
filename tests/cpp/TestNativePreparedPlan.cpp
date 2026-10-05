#include "TestNativePreparedPlan.hpp"

#include "native/prepare/NativeArtifactStore.hpp"
#include "native/bundle/NativeManifest.hpp"
#include "native/bundle/NativePaths.hpp"
#include "native/prepare/NativePreparedPlan.hpp"

#include <optional>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>

using namespace mplayer::native;

// Whole-plan admission against real temporary files: real owned model bytes, real
// published artifacts, real leases. No engine is built or loaded and no runtime
// is loaded, so nothing here claims Prepared/Ready/Active.
namespace {

QString sha256Hex(const QByteArray& data) {
  return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool writeBytes(const QString& path, const QByteArray& data) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  return file.write(data) == data.size();
}

ModelProfile profileFor(const QSize& input, const QSize& output,
                        qint64 inputChannels = 3, qint64 outputChannels = 3) {
  ModelProfile profile;
  profile.input.dtype = QString::fromLatin1(kEligibleIoType);
  profile.input.rank = 4;
  profile.input.batch = 1;
  profile.input.channels = inputChannels;
  profile.input.height = input.height();
  profile.input.width = input.width();
  profile.output.dtype = QString::fromLatin1(kEligibleIoType);
  profile.output.rank = 4;
  profile.output.batch = 1;
  profile.output.channels = outputChannels;
  profile.output.height = output.height();
  profile.output.width = output.width();
  return profile;
}

SourceGeometry hdSource() {
  SourceGeometry source;
  source.codedSize = QSize(1920, 1080);
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

EngineBuildIdentity buildFor(const DeviceIdentity& device) {
  EngineBuildIdentity build;
  build.provider = Backend::TensorRT;
  build.runtimeVersion = QStringLiteral("11.3.0.99");
  build.pluginVersion = QStringLiteral("1.2.3");
  build.coreSha = QStringLiteral("63d127174d8f70336c82a22408929e2bc301edcb");
  build.sourceAbi = 8;
  build.precision = EnginePrecision::Fp16;
  build.tf32 = false;
  build.stronglyTyped = true;
  build.skipInference = true;
  build.deviceUuid = device.computeDeviceUuid;
  build.adapterLuid = device.adapterLuid;
  build.smMajor = device.smMajor;
  build.smMinor = device.smMinor;
  build.cudaVersion = QStringLiteral("13.4.92");
  build.driverVersion = QStringLiteral("616.92");
  build.options = QVector<BuildOption>{
      BuildOption{QStringLiteral("stronglyTyped"), QString(), false},
      BuildOption{QStringLiteral("skipInference"), QString(), false},
      BuildOption{QStringLiteral("tacticSources"), QStringLiteral("-CUDNN,-CUBLAS"), true}};
  return build;
}

struct Fixture {
  Fixture() : store(temp.path()) {
    modelRoot = QDir(temp.path()).filePath(QStringLiteral("models"));
    scratch = QDir(temp.path()).filePath(QStringLiteral("scratch"));
    QDir().mkpath(modelRoot);
    QDir().mkpath(scratch);
  }

  QTemporaryDir temp;
  ArtifactStore store;
  QString modelRoot;
  QString scratch;
};

ModelRecord makeRecord(const Fixture& fixture, ModelRole role, const ModelProfile& declared,
                       const QByteArray& bytes) {
  const QString digest = sha256Hex(bytes);
  const QString directory = QDir(fixture.modelRoot).filePath(digest);
  QDir().mkpath(directory);
  const QString path = QDir(directory).filePath(QStringLiteral("model.onnx"));
  writeBytes(path, bytes);

  ModelRecord record;
  record.id = userModelIdFor(computeContentId(role, digest, declared));
  record.displayName = QStringLiteral("fixture");
  record.origin = ModelOrigin::UserImported;
  record.onnxPath = path;
  record.onnxSha256 = digest;
  record.onnxBytes = bytes.size();
  record.role = role;
  record.actualProfile = declared;
  record.availableBackends = QVector<Backend>{Backend::TensorRT};
  record.availability = readyAvailability();
  return record;
}

ResolvedStage spatialStage(const ModelRecord& record, const QSize& input, const QSize& output,
                           const Rational& fps, int recipeIndex) {
  ResolvedStage stage;
  stage.kind = StageKind::Spatial;
  stage.role = ModelRole::Spatial;
  stage.modelId = record.id;
  stage.modelContentId = record.contentId();
  stage.recipeIndex = recipeIndex;
  stage.inputGeometry = input;
  stage.outputGeometry = output;
  stage.contentGeometry = output;
  stage.scale = uniformIntegerScale(input, output);
  stage.declaredInput = record.actualProfile.input;
  stage.declaredOutput = record.actualProfile.output;
  stage.geometryFromResolver = false;
  stage.inputChannels = 3;
  stage.outputChannels = 3;
  stage.padL = 0;
  stage.padT = 0;
  stage.inputFps = fps;
  stage.outputFps = fps;
  return stage;
}

ResolvedStage temporalStage(const ModelRecord& record, const QSize& content, const Rational& inFps,
                            const Rational& factor, TemporalOrder order) {
  ResolvedStage stage;
  stage.kind = StageKind::Temporal;
  stage.role = ModelRole::Temporal;
  stage.modelId = record.id;
  stage.modelContentId = record.contentId();
  stage.recipeIndex = -1;
  stage.inputChannels = 11;
  stage.outputChannels = 3;
  stage.declaredInput = record.actualProfile.input;
  stage.declaredOutput = record.actualProfile.output;
  stage.contentGeometry = content;
  int padL = -1;
  int padT = -1;
  stage.inputGeometry = padToTensorAlignment(content, &padL, &padT);
  stage.outputGeometry = stage.inputGeometry;
  stage.padL = padL;
  stage.padT = padT;
  stage.inputFps = inFps;
  stage.factor = factor;
  stage.outputFps = scaleCadence(inFps, factor.num, factor.den).value_or(Rational{1, 1});
  stage.order = order;
  stage.sceneThreshold = 0.15;
  return stage;
}

PreparationPlan makePlan(const QVector<ResolvedStage>& stages, const SourceGeometry& source,
                         const DeviceIdentity& device, const QString& recipeId) {
  PreparationPlan plan;
  plan.id = QStringLiteral("sha256:") + QString(64, QLatin1Char('c'));
  plan.recipeId = recipeId;
  plan.backend = Backend::TensorRT;
  plan.source = source;
  plan.device = device;
  plan.stages = stages;
  plan.unknowns.clear();
  plan.blockers.clear();
  plan.availability = readyAvailability();
  return plan;
}

BuildRequest requestForStage(const ResolvedStage& stage, const ModelRecord& record,
                             const EngineBuildIdentity& build) {
  BuildRequest request;
  request.graph = graphIdentityOf(record);
  StageBuildRequest stageRequest;
  stageRequest.role = stage.role;
  stageRequest.inputShape = stage.inputShape();
  stageRequest.outputShape = stage.outputShape();
  stageRequest.ioFormat = stage.ioFormats();
  stageRequest.padLeft = stage.padL;
  stageRequest.padTop = stage.padT;
  request.stage = stageRequest;
  request.build = build;
  return request;
}

std::optional<ArtifactRecord> publishStage(const Fixture& fixture, const BuildRequest& request,
                                           const QByteArray& graphBytes,
                                           const QByteArray& engineBytes) {
  ArtifactManifest manifest;
  manifest.schema = artifactManifestSchema();
  manifest.mikaiFormat = 1;
  manifest.buildKey = request.buildKey();
  manifest.engineSha256 = sha256Hex(engineBytes);
  manifest.source.modelContentId = request.graph.contentId();
  manifest.source.onnxSha256 = request.graph.graphSha256;
  manifest.source.onnxBytes = graphBytes.size();
  manifest.source.declaredProfile = request.graph.declaredProfile;
  manifest.stage.role = request.stage.role;
  manifest.stage.inputShape = request.stage.inputShape;
  manifest.stage.outputShape = request.stage.outputShape;
  manifest.stage.ioFormat = request.stage.ioFormat;
  manifest.stage.padLeft = request.stage.padLeft;
  manifest.stage.padTop = request.stage.padTop;
  manifest.build.provider = request.build.provider;
  manifest.build.runtime = request.build.runtimeVersion;
  manifest.build.plugin = request.build.pluginVersion;
  manifest.build.coreSha = request.build.coreSha;
  manifest.build.sourceAbi = request.build.sourceAbi;
  manifest.build.precision = request.build.precision;
  manifest.build.tf32 = request.build.tf32;
  manifest.build.stronglyTyped = request.build.stronglyTyped;
  manifest.build.skipInference = request.build.skipInference;
  manifest.build.canonicalOptions = request.build.canonicalOptions();
  manifest.device.uuid = request.build.deviceUuid;
  manifest.device.sm = request.build.smText();
  manifest.device.driver = request.build.driverVersion;
  manifest.device.cuda = request.build.cudaVersion;
  manifest.device.luid = request.build.adapterLuid;
  manifest.engine.file = QStringLiteral("engine.plan");
  manifest.engine.bytes = engineBytes.size();
  manifest.engine.sha256 = manifest.engineSha256;
  manifest.createdAt = QStringLiteral("2026-10-01T00:00:00Z");

  const QString scratchPath = QDir(fixture.scratch)
                                  .filePath(QStringLiteral("produced-%1.engine")
                                                .arg(QUuid::createUuid().toString(
                                                    QUuid::WithoutBraces)));
  if (!writeBytes(scratchPath, engineBytes)) return std::nullopt;
  return publishArtifact(fixture.store, manifest, scratchPath, nullptr, nullptr);
}

QJsonObject stageCommand(const PreparedAdmission& admission, int index) {
  return admission.command.value(QStringLiteral("stages")).toArray().at(index).toObject();
}

}  // namespace

// One whole spatial plan resolves, its command names the stage and its machine,
// and both artifact record and lease come back.
void TestNativePreparedPlan::cudaUuidPresentationDoesNotInvalidatePreparedArtifacts() {
  Fixture fixture;
  DeviceIdentity device = knownDevice();
  device.computeDeviceUuid = QStringLiteral("f76e436f-c519-8f5a-95f7-ce9481b54b5b");
  device.decoderDeviceUuid = device.computeDeviceUuid;
  const SourceGeometry source = hdSource();
  EngineBuildIdentity build = buildFor(device);
  build.deviceUuid = QStringLiteral("GPU-F76E436F-C519-8F5A-95F7-CE9481B54B5B");
  const QByteArray graphBytes = QByteArrayLiteral("onnx-device-identity");
  const ModelRecord record =
      makeRecord(fixture, ModelRole::Spatial,
                 profileFor(QSize(1920, 1080), QSize(3840, 2160)), graphBytes);
  const ResolvedStage stage =
      spatialStage(record, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  QVERIFY(publishStage(fixture, requestForStage(stage, record, build), graphBytes,
                       QByteArray(1024, char(0x41))).has_value());

  PreparedAdmissionRequest request;
  request.plan = makePlan({stage}, source, device, QStringLiteral("recipe.device"));
  request.models = {record};
  request.build = build;
  request.store = fixture.store;
  request.modelRoot = fixture.modelRoot;
  request.source = source;
  request.device = device;
  request.recipeId = QStringLiteral("recipe.device");
  request.backend = Backend::TensorRT;
  request.requestId = QStringLiteral("request.device");

  QString key;
  QString reason;
  const auto admission = resolvePreparedAdmission(request, &key, &reason);
  QVERIFY2(admission.has_value(), qPrintable(key + QStringLiteral(": ") + reason));

  request.build.deviceUuid = QStringLiteral("GPU-f76e436f-c519-8f5a-95f7-ce9481b54b5c");
  QVERIFY(!resolvePreparedAdmission(request, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("build.device.mismatch"));
}

void TestNativePreparedPlan::aWholeSpatialPlanResolvesToTheAdmissionCommand() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);
  const QByteArray graphBytes = QByteArrayLiteral("onnx-graph-a");
  const QByteArray engineBytes = QByteArray(2048, char(0x41));

  const ModelRecord record =
      makeRecord(fixture, ModelRole::Spatial, profileFor(QSize(1920, 1080), QSize(3840, 2160)),
                 graphBytes);
  const ResolvedStage stage =
      spatialStage(record, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  const PreparationPlan plan =
      makePlan(QVector<ResolvedStage>{stage}, source, device, QStringLiteral("recipe.test"));
  const BuildRequest request = requestForStage(stage, record, build);
  QVERIFY(publishStage(fixture, request, graphBytes, engineBytes).has_value());

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan = plan;
  admissionRequest.models = QVector<ModelRecord>{record};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.test");
  admissionRequest.backend = Backend::TensorRT;
  admissionRequest.requestId = QStringLiteral("req-1");
  admissionRequest.revision = 7;
  admissionRequest.epoch = 3;

  QString reasonKey;
  QString reason;
  const std::optional<PreparedAdmission> admission =
      resolvePreparedAdmission(admissionRequest, &reasonKey, &reason);
  QVERIFY2(admission.has_value(), qPrintable(reasonKey + QStringLiteral(": ") + reason));
  QVERIFY(reasonKey.isEmpty());
  QVERIFY(reason.isEmpty());

  QCOMPARE(admission->artifacts.size(), 1);
  QCOMPARE(admission->leases.size(), 1);
  QVERIFY(admission->leases.at(0).isValid());
  QCOMPARE(admission->command.value(QStringLiteral("schema")).toString(),
           preparedAdmissionSchema());
  QCOMPARE(admission->command.value(QStringLiteral("protocol")).toInt(), 1);
  QCOMPARE(admission->command.value(QStringLiteral("requestId")).toString(),
           QStringLiteral("req-1"));
  QCOMPARE(admission->command.value(QStringLiteral("revision")).toInt(), 7);
  QCOMPARE(admission->command.value(QStringLiteral("epoch")).toInt(), 3);
  QCOMPARE(admission->command.value(QStringLiteral("requested")).toObject()
               .value(QStringLiteral("recipe"))
               .toString(),
           QStringLiteral("recipe.test"));
  QCOMPARE(admission->command.value(QStringLiteral("requested")).toObject()
               .value(QStringLiteral("backend"))
               .toString(),
           QStringLiteral("tensorrt"));

  const QJsonObject entry = stageCommand(*admission, 0);
  QCOMPARE(entry.value(QStringLiteral("ordinal")).toInt(), 0);
  QCOMPARE(entry.value(QStringLiteral("role")).toString(), QStringLiteral("spatial"));
  QCOMPARE(entry.value(QStringLiteral("modelId")).toString(), record.id);
  QCOMPARE(entry.value(QStringLiteral("modelContentId")).toString(), record.contentId());
  QCOMPARE(entry.value(QStringLiteral("modelSha256")).toString(), record.onnxSha256);
  QCOMPARE(entry.value(QStringLiteral("engineSha256")).toString(), sha256Hex(engineBytes));
  QCOMPARE(entry.value(QStringLiteral("buildKey")).toString(), request.buildKey());
  QVERIFY(QDir::isAbsolutePath(entry.value(QStringLiteral("modelPath")).toString()));
  QVERIFY(QDir::isAbsolutePath(entry.value(QStringLiteral("enginePath")).toString()));
  QCOMPARE(entry.value(QStringLiteral("inputDtype")).toString(),
           QString::fromLatin1(kEligibleIoType));
  QCOMPARE(entry.value(QStringLiteral("layout")).toString(), QStringLiteral("chw"));
  const QJsonArray inputShape = entry.value(QStringLiteral("inputShape")).toArray();
  QCOMPARE(inputShape.size(), 4);
  QCOMPARE(inputShape.at(0).toInt(), 1);
  QCOMPARE(inputShape.at(1).toInt(), 3);
  QCOMPARE(inputShape.at(2).toInt(), 1080);
  QCOMPARE(inputShape.at(3).toInt(), 1920);
  const QJsonArray outputShape = entry.value(QStringLiteral("outputShape")).toArray();
  QCOMPARE(outputShape.at(2).toInt(), 2160);
  QCOMPARE(outputShape.at(3).toInt(), 3840);
  const QJsonObject content = entry.value(QStringLiteral("contentSize")).toObject();
  QCOMPARE(content.value(QStringLiteral("w")).toInt(), 3840);
  QCOMPARE(content.value(QStringLiteral("h")).toInt(), 2160);
  QCOMPARE(entry.value(QStringLiteral("padLeft")).toInt(), 0);
  QCOMPARE(entry.value(QStringLiteral("padTop")).toInt(), 0);
}

void TestNativePreparedPlan::anAdmissionHoldsItsArtifactLeasesUntilDropped() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);
  const ModelRecord record = makeRecord(fixture, ModelRole::Spatial,
                                        profileFor(QSize(1920, 1080), QSize(3840, 2160)),
                                        QByteArrayLiteral("onnx-graph-a"));
  const ResolvedStage stage =
      spatialStage(record, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  const BuildRequest request = requestForStage(stage, record, build);
  QVERIFY(publishStage(fixture, request, QByteArrayLiteral("onnx-graph-a"),
                       QByteArray(1024, char(0x42)))
              .has_value());

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan =
      makePlan(QVector<ResolvedStage>{stage}, source, device, QStringLiteral("recipe.test"));
  admissionRequest.models = QVector<ModelRecord>{record};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.test");
  admissionRequest.requestId = QStringLiteral("req-lease");

  std::optional<PreparedAdmission> admission = resolvePreparedAdmission(admissionRequest);
  QVERIFY(admission.has_value());

  QString key;
  QString reason;
  QVERIFY(!removeArtifact(fixture.store, request.buildKey(), &key, &reason));
  QCOMPARE(key, QStringLiteral("native.artifact.inUse"));

  admission.reset();
  QVERIFY2(removeArtifact(fixture.store, request.buildKey(), &key, &reason), qPrintable(reason));
}

void TestNativePreparedPlan::aTemporalStageCarriesItsOrderFactorAndPaddedShapes() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);
  const QByteArray graphBytes = QByteArrayLiteral("onnx-rife");
  const QByteArray engineBytes = QByteArray(1024, char(0x52));

  const ModelRecord record =
      makeRecord(fixture, ModelRole::Temporal,
                 profileFor(QSize(1920, 1080), QSize(1920, 1080), 11, 3), graphBytes);
  const ResolvedStage stage = temporalStage(record, QSize(1920, 1080), Rational{24, 1},
                                            Rational{2, 1}, TemporalOrder::AfterSpatial);
  const PreparationPlan plan =
      makePlan(QVector<ResolvedStage>{stage}, source, device, QStringLiteral("recipe.rife"));
  const BuildRequest request = requestForStage(stage, record, build);
  QVERIFY(publishStage(fixture, request, graphBytes, engineBytes).has_value());

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan = plan;
  admissionRequest.models = QVector<ModelRecord>{record};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.rife");
  admissionRequest.requestId = QStringLiteral("req-rife");

  QString reasonKey;
  QString reason;
  const std::optional<PreparedAdmission> admission =
      resolvePreparedAdmission(admissionRequest, &reasonKey, &reason);
  QVERIFY2(admission.has_value(), qPrintable(reasonKey + QStringLiteral(": ") + reason));

  const QJsonObject entry = stageCommand(*admission, 0);
  QCOMPARE(entry.value(QStringLiteral("role")).toString(), QStringLiteral("temporal"));
  QCOMPARE(entry.value(QStringLiteral("order")).toString(), QStringLiteral("after_spatial"));
  const QJsonArray inputShape = entry.value(QStringLiteral("inputShape")).toArray();
  QCOMPARE(inputShape.at(1).toInt(), 11);
  QCOMPARE(inputShape.at(2).toInt(), 1088);
  QCOMPARE(inputShape.at(3).toInt(), 1920);
  const QJsonArray outputShape = entry.value(QStringLiteral("outputShape")).toArray();
  QCOMPARE(outputShape.at(1).toInt(), 3);
  QCOMPARE(outputShape.at(2).toInt(), 1088);
  QCOMPARE(entry.value(QStringLiteral("padTop")).toInt(), 4);
  QCOMPARE(entry.value(QStringLiteral("padLeft")).toInt(), 0);
  const QJsonObject content = entry.value(QStringLiteral("contentSize")).toObject();
  QCOMPARE(content.value(QStringLiteral("w")).toInt(), 1920);
  QCOMPARE(content.value(QStringLiteral("h")).toInt(), 1080);
  const QJsonObject factor = entry.value(QStringLiteral("factor")).toObject();
  QCOMPARE(factor.value(QStringLiteral("num")).toInt(), 2);
  QCOMPARE(factor.value(QStringLiteral("den")).toInt(), 1);
  QVERIFY(qFuzzyCompare(entry.value(QStringLiteral("sceneThreshold")).toDouble(), 0.15));
}

void TestNativePreparedPlan::aWholeCombinedPlanAdmitsEveryOrderedStage() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);

  const ModelRecord spatial =
      makeRecord(fixture, ModelRole::Spatial, profileFor(QSize(1920, 1080), QSize(3840, 2160)),
                 QByteArrayLiteral("onnx-spatial"));
  const ModelRecord temporal =
      makeRecord(fixture, ModelRole::Temporal,
                 profileFor(QSize(3840, 2176), QSize(3840, 2176), 11, 3),
                 QByteArrayLiteral("onnx-rife"));
  const ResolvedStage spatialStage =
      ::spatialStage(spatial, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  const ResolvedStage temporalStage = ::temporalStage(temporal, QSize(3840, 2160), Rational{24, 1},
                                                      Rational{2, 1}, TemporalOrder::AfterSpatial);

  const BuildRequest spatialRequest = requestForStage(spatialStage, spatial, build);
  const BuildRequest temporalRequest = requestForStage(temporalStage, temporal, build);
  QVERIFY(publishStage(fixture, spatialRequest, QByteArrayLiteral("onnx-spatial"),
                       QByteArray(1280, char(0x31)))
              .has_value());
  QVERIFY(publishStage(fixture, temporalRequest, QByteArrayLiteral("onnx-rife"),
                       QByteArray(1536, char(0x32)))
              .has_value());

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan = makePlan(QVector<ResolvedStage>{spatialStage, temporalStage}, source,
                                   device, QStringLiteral("recipe.combined"));
  admissionRequest.models = QVector<ModelRecord>{spatial, temporal};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.combined");
  admissionRequest.requestId = QStringLiteral("req-combined");

  QString reasonKey;
  QString reason;
  const std::optional<PreparedAdmission> admission =
      resolvePreparedAdmission(admissionRequest, &reasonKey, &reason);
  QVERIFY2(admission.has_value(), qPrintable(reasonKey + QStringLiteral(": ") + reason));

  QCOMPARE(admission->artifacts.size(), 2);
  QCOMPARE(admission->leases.size(), 2);
  QCOMPARE(admission->leases.at(0).isValid(), true);
  QCOMPARE(admission->leases.at(1).isValid(), true);
  const QJsonArray stages =
      admission->command.value(QStringLiteral("stages")).toArray();
  QCOMPARE(stages.size(), 2);
  QCOMPARE(stages.at(0).toObject().value(QStringLiteral("role")).toString(),
           QStringLiteral("spatial"));
  QCOMPARE(stages.at(1).toObject().value(QStringLiteral("role")).toString(),
           QStringLiteral("temporal"));
  QCOMPARE(stages.at(1).toObject().value(QStringLiteral("order")).toString(),
           QStringLiteral("after_spatial"));
  QCOMPARE(stages.at(1).toObject().value(QStringLiteral("modelId")).toString(), temporal.id);
  QCOMPARE(stages.at(1).toObject().value(QStringLiteral("buildKey")).toString(),
           temporalRequest.buildKey());
}

void TestNativePreparedPlan::aRefusedSecondTemporalStageAdmitsNothing() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);

  const ModelRecord spatial =
      makeRecord(fixture, ModelRole::Spatial, profileFor(QSize(1920, 1080), QSize(3840, 2160)),
                 QByteArrayLiteral("onnx-spatial"));
  const ModelRecord temporal =
      makeRecord(fixture, ModelRole::Temporal,
                 profileFor(QSize(3840, 2176), QSize(3840, 2176), 11, 3),
                 QByteArrayLiteral("onnx-rife"));
  const ResolvedStage spatialStage =
      ::spatialStage(spatial, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  const ResolvedStage temporalStage = ::temporalStage(temporal, QSize(3840, 2160), Rational{24, 1},
                                                      Rational{2, 1}, TemporalOrder::AfterSpatial);
  const BuildRequest spatialRequest = requestForStage(spatialStage, spatial, build);

  // Only the first stage's engine exists: the temporal stage is the one that
  // cannot be admitted.
  QVERIFY(publishStage(fixture, spatialRequest, QByteArrayLiteral("onnx-spatial"),
                       QByteArray(1280, char(0x41)))
              .has_value());

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan = makePlan(QVector<ResolvedStage>{spatialStage, temporalStage}, source,
                                   device, QStringLiteral("recipe.combined"));
  admissionRequest.models = QVector<ModelRecord>{spatial, temporal};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.combined");
  admissionRequest.requestId = QStringLiteral("req-combined-missing");

  QString reasonKey;
  QString reason;
  QVERIFY(!resolvePreparedAdmission(admissionRequest, &reasonKey, &reason).has_value());
  QCOMPARE(reasonKey, QStringLiteral("artifact.missing"));

  // No partial chain is published: the lease the refusal took on the admitted
  // first stage is gone, so that artifact is removable again.
  QString removeKey;
  QString removeReason;
  QVERIFY2(removeArtifact(fixture.store, spatialRequest.buildKey(), &removeKey, &removeReason),
           qPrintable(removeReason));
}

void TestNativePreparedPlan::aRequestWithoutARequestIdIsRefused() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);
  const ModelRecord record = makeRecord(fixture, ModelRole::Spatial,
                                        profileFor(QSize(1920, 1080), QSize(3840, 2160)),
                                        QByteArrayLiteral("onnx-graph-a"));
  const ResolvedStage stage =
      spatialStage(record, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  const BuildRequest request = requestForStage(stage, record, build);
  QVERIFY(publishStage(fixture, request, QByteArrayLiteral("onnx-graph-a"),
                       QByteArray(512, char(0x11)))
              .has_value());

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan =
      makePlan(QVector<ResolvedStage>{stage}, source, device, QStringLiteral("recipe.test"));
  admissionRequest.models = QVector<ModelRecord>{record};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.test");
  admissionRequest.requestId.clear();

  QString reasonKey;
  QString reason;
  QVERIFY(!resolvePreparedAdmission(admissionRequest, &reasonKey, &reason).has_value());
  QVERIFY(!reasonKey.isEmpty());
  QVERIFY(!reason.isEmpty());
}

void TestNativePreparedPlan::anUnresolvedPlanIsRefused() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);
  const ModelRecord record = makeRecord(fixture, ModelRole::Spatial,
                                        profileFor(QSize(1920, 1080), QSize(3840, 2160)),
                                        QByteArrayLiteral("onnx-graph-a"));
  const ResolvedStage stage =
      spatialStage(record, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);

  PreparationPlan plan =
      makePlan(QVector<ResolvedStage>{stage}, source, device, QStringLiteral("recipe.test"));
  plan.blockers.append(PlanBlocker{QStringLiteral("source.window.pixels"),
                                   QStringLiteral("outside the window"), -1});

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan = plan;
  admissionRequest.models = QVector<ModelRecord>{record};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.test");
  admissionRequest.requestId = QStringLiteral("req-unresolved");

  QString reasonKey;
  QString reason;
  QVERIFY(!resolvePreparedAdmission(admissionRequest, &reasonKey, &reason).has_value());
  QVERIFY(!reasonKey.isEmpty());
}

void TestNativePreparedPlan::aSourceOrDeviceThatIsNotThePlannedOneIsRefused() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);
  const ModelRecord record = makeRecord(fixture, ModelRole::Spatial,
                                        profileFor(QSize(1920, 1080), QSize(3840, 2160)),
                                        QByteArrayLiteral("onnx-graph-a"));
  const ResolvedStage stage =
      spatialStage(record, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  const BuildRequest request = requestForStage(stage, record, build);
  QVERIFY(publishStage(fixture, request, QByteArrayLiteral("onnx-graph-a"),
                       QByteArray(512, char(0x21)))
              .has_value());

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan =
      makePlan(QVector<ResolvedStage>{stage}, source, device, QStringLiteral("recipe.test"));
  admissionRequest.models = QVector<ModelRecord>{record};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.test");
  admissionRequest.requestId = QStringLiteral("req-source");

  PreparedAdmissionRequest otherSource = admissionRequest;
  otherSource.source.codedSize = QSize(1280, 720);
  QString key;
  QString reason;
  QVERIFY(!resolvePreparedAdmission(otherSource, &key, &reason).has_value());
  QVERIFY(!key.isEmpty());

  PreparedAdmissionRequest otherDevice = admissionRequest;
  otherDevice.device.computeDeviceUuid = QStringLiteral("GPU-other");
  QVERIFY(!resolvePreparedAdmission(otherDevice, &key, &reason).has_value());
  QVERIFY(!key.isEmpty());
}

void TestNativePreparedPlan::anUnobservedDeviceIsRefused() {
  Fixture fixture;
  DeviceIdentity device = knownDevice();
  device.decoderDeviceUuid.clear();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);
  const ModelRecord record = makeRecord(fixture, ModelRole::Spatial,
                                        profileFor(QSize(1920, 1080), QSize(3840, 2160)),
                                        QByteArrayLiteral("onnx-graph-a"));
  const ResolvedStage stage =
      spatialStage(record, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  const BuildRequest request = requestForStage(stage, record, build);
  QVERIFY(publishStage(fixture, request, QByteArrayLiteral("onnx-graph-a"),
                       QByteArray(512, char(0x31)))
              .has_value());

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan =
      makePlan(QVector<ResolvedStage>{stage}, source, device, QStringLiteral("recipe.test"));
  admissionRequest.models = QVector<ModelRecord>{record};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.test");
  admissionRequest.requestId = QStringLiteral("req-device");

  QString key;
  QString reason;
  QVERIFY(!resolvePreparedAdmission(admissionRequest, &key, &reason).has_value());
  QVERIFY(!key.isEmpty());
}

void TestNativePreparedPlan::aBuildIdentityThatIsNotThePlannedDeviceIsRefused() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);
  const ModelRecord record = makeRecord(fixture, ModelRole::Spatial,
                                        profileFor(QSize(1920, 1080), QSize(3840, 2160)),
                                        QByteArrayLiteral("onnx-graph-a"));
  const ResolvedStage stage =
      spatialStage(record, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  const BuildRequest request = requestForStage(stage, record, build);
  QVERIFY(publishStage(fixture, request, QByteArrayLiteral("onnx-graph-a"),
                       QByteArray(512, char(0x41)))
              .has_value());

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan =
      makePlan(QVector<ResolvedStage>{stage}, source, device, QStringLiteral("recipe.test"));
  admissionRequest.models = QVector<ModelRecord>{record};
  admissionRequest.build = build;
  admissionRequest.build.deviceUuid = QStringLiteral("GPU-not-the-planned-one");
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.test");
  admissionRequest.requestId = QStringLiteral("req-builddevice");

  QString key;
  QString reason;
  QVERIFY(!resolvePreparedAdmission(admissionRequest, &key, &reason).has_value());
  QVERIFY(!key.isEmpty());
}

void TestNativePreparedPlan::aBuildIdentityWithoutARuntimeIsRefused() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  EngineBuildIdentity build = buildFor(device);
  build.runtimeVersion.clear();
  const ModelRecord record = makeRecord(fixture, ModelRole::Spatial,
                                        profileFor(QSize(1920, 1080), QSize(3840, 2160)),
                                        QByteArrayLiteral("onnx-graph-a"));
  const ResolvedStage stage =
      spatialStage(record, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan =
      makePlan(QVector<ResolvedStage>{stage}, source, device, QStringLiteral("recipe.test"));
  admissionRequest.models = QVector<ModelRecord>{record};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.test");
  admissionRequest.requestId = QStringLiteral("req-no-runtime");

  QString key;
  QString reason;
  QVERIFY(!resolvePreparedAdmission(admissionRequest, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("request.incomplete"));
  QVERIFY(!reason.isEmpty());
}

void TestNativePreparedPlan::aReorderedChainIsRefused() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);

  const ModelRecord first =
      makeRecord(fixture, ModelRole::Spatial, profileFor(QSize(1920, 1080), QSize(3840, 2160)),
                 QByteArrayLiteral("onnx-graph-1"));
  const ModelRecord second =
      makeRecord(fixture, ModelRole::Spatial, profileFor(QSize(3840, 2160), QSize(7680, 4320)),
                 QByteArrayLiteral("onnx-graph-2"));
  const ResolvedStage firstStage =
      spatialStage(first, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  const ResolvedStage secondStage =
      spatialStage(second, QSize(3840, 2160), QSize(7680, 4320), Rational{24, 1}, 1);
  const QVector<ResolvedStage> ordered{firstStage, secondStage};
  const BuildRequest firstRequest = requestForStage(firstStage, first, build);
  const BuildRequest secondRequest = requestForStage(secondStage, second, build);
  QVERIFY(publishStage(fixture, firstRequest, QByteArrayLiteral("onnx-graph-1"),
                       QByteArray(512, char(0x51)))
              .has_value());
  QVERIFY(publishStage(fixture, secondRequest, QByteArrayLiteral("onnx-graph-2"),
                       QByteArray(512, char(0x52)))
              .has_value());

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan = makePlan(ordered, source, device, QStringLiteral("recipe.chain"));
  admissionRequest.models = QVector<ModelRecord>{first, second};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.chain");
  admissionRequest.requestId = QStringLiteral("req-chain");
  QVERIFY(resolvePreparedAdmission(admissionRequest).has_value());

  // Swapping the stages breaks the threaded geometry even though the artifacts
  // still exist under their own keys.
  QVector<ResolvedStage> swapped{secondStage, firstStage};
  swapped[0].recipeIndex = 0;
  swapped[1].recipeIndex = 1;
  PreparedAdmissionRequest reordered = admissionRequest;
  reordered.plan = makePlan(swapped, source, device, QStringLiteral("recipe.chain"));
  QString key;
  QString reason;
  QVERIFY(!resolvePreparedAdmission(reordered, &key, &reason).has_value());
  QVERIFY(!key.isEmpty());

  // A recipe index that does not ascend in execution order is refused too.
  QVector<ResolvedStage> misindexed{firstStage, secondStage};
  misindexed[0].recipeIndex = 1;
  misindexed[1].recipeIndex = 0;
  PreparedAdmissionRequest misordered = admissionRequest;
  misordered.plan = makePlan(misindexed, source, device, QStringLiteral("recipe.chain"));
  QVERIFY(!resolvePreparedAdmission(misordered, &key, &reason).has_value());
  QVERIFY(!key.isEmpty());
}

void TestNativePreparedPlan::aMissingArtifactIsRefused() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);

  const ModelRecord first =
      makeRecord(fixture, ModelRole::Spatial, profileFor(QSize(1920, 1080), QSize(3840, 2160)),
                 QByteArrayLiteral("onnx-graph-1"));
  const ModelRecord second =
      makeRecord(fixture, ModelRole::Spatial, profileFor(QSize(3840, 2160), QSize(7680, 4320)),
                 QByteArrayLiteral("onnx-graph-2"));
  const ResolvedStage firstStage =
      spatialStage(first, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  const ResolvedStage secondStage =
      spatialStage(second, QSize(3840, 2160), QSize(7680, 4320), Rational{24, 1}, 1);
  // Only the first stage's artifact exists.
  QVERIFY(publishStage(fixture, requestForStage(firstStage, first, build),
                       QByteArrayLiteral("onnx-graph-1"), QByteArray(512, char(0x61)))
              .has_value());

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan = makePlan(QVector<ResolvedStage>{firstStage, secondStage}, source, device,
                                   QStringLiteral("recipe.chain"));
  admissionRequest.models = QVector<ModelRecord>{first, second};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.chain");
  admissionRequest.requestId = QStringLiteral("req-missing");

  QString key;
  QString reason;
  QVERIFY(!resolvePreparedAdmission(admissionRequest, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("artifact.missing"));
  QVERIFY(!reason.isEmpty());
}

void TestNativePreparedPlan::aMutatedEngineIsRefused() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);
  const QByteArray graphBytes = QByteArrayLiteral("onnx-graph-a");
  const ModelRecord record = makeRecord(fixture, ModelRole::Spatial,
                                        profileFor(QSize(1920, 1080), QSize(3840, 2160)),
                                        graphBytes);
  const ResolvedStage stage =
      spatialStage(record, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  const BuildRequest request = requestForStage(stage, record, build);
  const std::optional<ArtifactRecord> published =
      publishStage(fixture, request, graphBytes, QByteArray(1024, char(0x71)));
  QVERIFY(published.has_value());

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan =
      makePlan(QVector<ResolvedStage>{stage}, source, device, QStringLiteral("recipe.test"));
  admissionRequest.models = QVector<ModelRecord>{record};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.test");
  admissionRequest.requestId = QStringLiteral("req-mutated");

  QVERIFY(writeBytes(published->enginePath, QByteArray(1024, char(0x72))));
  QString key;
  QString reason;
  QVERIFY(!resolvePreparedAdmission(admissionRequest, &key, &reason).has_value());
  QVERIFY(key.startsWith(QStringLiteral("artifact.")));
}

void TestNativePreparedPlan::aModelDigestThatNoLongerMatchesIsRefused() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);
  const QByteArray graphBytes = QByteArrayLiteral("onnx-graph-a");
  ModelRecord record = makeRecord(fixture, ModelRole::Spatial,
                                  profileFor(QSize(1920, 1080), QSize(3840, 2160)), graphBytes);
  const ResolvedStage stage =
      spatialStage(record, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  const BuildRequest request = requestForStage(stage, record, build);
  QVERIFY(publishStage(fixture, request, graphBytes, QByteArray(1024, char(0x81))).has_value());

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan =
      makePlan(QVector<ResolvedStage>{stage}, source, device, QStringLiteral("recipe.test"));
  admissionRequest.models = QVector<ModelRecord>{record};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.test");
  admissionRequest.requestId = QStringLiteral("req-mutated-model");

  QVERIFY(writeBytes(record.onnxPath, QByteArrayLiteral("onnx-graph-a-edited")));
  QString key;
  QString reason;
  QVERIFY(!resolvePreparedAdmission(admissionRequest, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("model.owned.changed"));
}

void TestNativePreparedPlan::aModelOutsideTheOwnedRootIsRefused() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);
  const QByteArray graphBytes = QByteArrayLiteral("onnx-graph-a");
  ModelRecord record = makeRecord(fixture, ModelRole::Spatial,
                                  profileFor(QSize(1920, 1080), QSize(3840, 2160)), graphBytes);
  const ResolvedStage stage =
      spatialStage(record, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  const BuildRequest request = requestForStage(stage, record, build);
  QVERIFY(publishStage(fixture, request, graphBytes, QByteArray(1024, char(0x91))).has_value());

  // The same bytes, but outside the owned models root.
  const QString outside = QDir(fixture.temp.path()).filePath(QStringLiteral("outside.onnx"));
  QVERIFY(writeBytes(outside, graphBytes));
  record.onnxPath = outside;

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan =
      makePlan(QVector<ResolvedStage>{stage}, source, device, QStringLiteral("recipe.test"));
  admissionRequest.models = QVector<ModelRecord>{record};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.test");
  admissionRequest.requestId = QStringLiteral("req-unsafe-path");

  QString key;
  QString reason;
  QVERIFY(!resolvePreparedAdmission(admissionRequest, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("model.owned.missing"));
}

void TestNativePreparedPlan::aStaleArtifactManifestIsRefused() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);
  const QByteArray graphBytes = QByteArrayLiteral("onnx-graph-a");
  const ModelRecord record = makeRecord(fixture, ModelRole::Spatial,
                                        profileFor(QSize(1920, 1080), QSize(3840, 2160)),
                                        graphBytes);
  const ResolvedStage stage =
      spatialStage(record, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  const BuildRequest request = requestForStage(stage, record, build);
  const std::optional<ArtifactRecord> published =
      publishStage(fixture, request, graphBytes, QByteArray(1024, char(0xa1)));
  QVERIFY(published.has_value());

  // The manifest is edited after its key was computed, so it no longer describes
  // the request it is filed under.
  ArtifactManifest stale = published->manifest;
  stale.build.runtime = QStringLiteral("0.0.0-stale");
  QVERIFY(writeBytes(published->manifestPath, serializeArtifactManifest(stale)));

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan =
      makePlan(QVector<ResolvedStage>{stage}, source, device, QStringLiteral("recipe.test"));
  admissionRequest.models = QVector<ModelRecord>{record};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.test");
  admissionRequest.requestId = QStringLiteral("req-stale");

  QString key;
  QString reason;
  QVERIFY(!resolvePreparedAdmission(admissionRequest, &key, &reason).has_value());
  QVERIFY(key.startsWith(QStringLiteral("manifest.")));
  QVERIFY(!reason.isEmpty());
}

void TestNativePreparedPlan::aRefusedPartialAdmissionReleasesTheLeasesItHadTaken() {
  Fixture fixture;
  const DeviceIdentity device = knownDevice();
  const SourceGeometry source = hdSource();
  const EngineBuildIdentity build = buildFor(device);

  const ModelRecord first =
      makeRecord(fixture, ModelRole::Spatial, profileFor(QSize(1920, 1080), QSize(3840, 2160)),
                 QByteArrayLiteral("onnx-graph-1"));
  const ModelRecord second =
      makeRecord(fixture, ModelRole::Spatial, profileFor(QSize(3840, 2160), QSize(7680, 4320)),
                 QByteArrayLiteral("onnx-graph-2"));
  const ResolvedStage firstStage =
      spatialStage(first, QSize(1920, 1080), QSize(3840, 2160), Rational{24, 1}, 0);
  const ResolvedStage secondStage =
      spatialStage(second, QSize(3840, 2160), QSize(7680, 4320), Rational{24, 1}, 1);
  // Only the first stage's artifact exists, so resolution takes its lease and
  // then refuses on the second stage.
  const BuildRequest firstRequest = requestForStage(firstStage, first, build);
  QVERIFY(publishStage(fixture, firstRequest, QByteArrayLiteral("onnx-graph-1"),
                       QByteArray(1024, char(0xb1)))
              .has_value());

  PreparedAdmissionRequest admissionRequest;
  admissionRequest.plan = makePlan(QVector<ResolvedStage>{firstStage, secondStage}, source, device,
                                   QStringLiteral("recipe.chain"));
  admissionRequest.models = QVector<ModelRecord>{first, second};
  admissionRequest.build = build;
  admissionRequest.store = fixture.store;
  admissionRequest.modelRoot = fixture.modelRoot;
  admissionRequest.source = source;
  admissionRequest.device = device;
  admissionRequest.recipeId = QStringLiteral("recipe.chain");
  admissionRequest.requestId = QStringLiteral("req-partial");

  QString key;
  QString reason;
  QVERIFY(!resolvePreparedAdmission(admissionRequest, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("artifact.missing"));

  // The lease the refusal took on the first stage must not outlive it, so the
  // valid first artifact is removable.
  QString removeKey;
  QString removeReason;
  QVERIFY2(removeArtifact(fixture.store, firstRequest.buildKey(), &removeKey, &removeReason),
           qPrintable(removeReason));
}
