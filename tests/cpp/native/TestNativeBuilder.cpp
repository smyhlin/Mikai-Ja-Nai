#include "TestNativeBuilder.hpp"

#include <barrier>
#include <thread>
#include <utility>

#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QScopeGuard>
#include <QtCore/QUuid>

#include "native/bundle/NativePaths.hpp"
#include "native/models/NativeModelInventory.hpp"
#include "native/prepare/NativeBuilder.hpp"
#include "native/prepare/NativeWorkLease.hpp"

using namespace mplayer::native;

void TestNativeBuilder::anUnobservedDecoderCannotStartPreparation() {
  BuilderRequest request;
  request.plan.unknowns.append(QStringLiteral("decoder device has not been observed"));
  const BuilderResult result = runNativeBuild(request, nullptr, {});
  QVERIFY(!result.ok);
  QCOMPARE(result.reasonKey, QStringLiteral("plan.preparing"));
  QVERIFY(result.stages.isEmpty());
}

void TestNativeBuilder::aStageValidationFailureReturnsItsActualKeyWithoutLaunchingTools() {
  QString pathReason;
  const QString modelsBase = nativeSubdirectory(QStringLiteral("models"), &pathReason);
  QVERIFY2(!modelsBase.isEmpty(), qPrintable(pathReason));
  const QString artifactsBase = nativeSubdirectory(QStringLiteral("artifacts"), &pathReason);
  QVERIFY2(!artifactsBase.isEmpty(), qPrintable(pathReason));
  const QString token = QStringLiteral("unit-%1").arg(
      QUuid::createUuid().toString(QUuid::WithoutBraces));
  const QString modelRoot = QDir(modelsBase).filePath(token);
  const QString artifactRoot = QDir(artifactsBase).filePath(token);
  QVERIFY(QDir().mkpath(modelRoot));
  QVERIFY(QDir().mkpath(artifactRoot));
  const auto cleanup = qScopeGuard([&] {
    QDir(modelRoot).removeRecursively();
    QDir(artifactRoot).removeRecursively();
  });

  TensorProfile input;
  input.dtype = QStringLiteral("fp16");
  input.rank = 4;
  input.batch = 1;
  input.channels = 3;
  input.height = 64;
  input.width = 64;
  TensorProfile output = input;
  output.height = 128;
  output.width = 128;
  ModelProfile profile;
  profile.input = input;
  profile.output = output;
  InspectionResult inspection;
  inspection.ok = true;
  inspection.inputName = QStringLiteral("X");
  inspection.outputName = QStringLiteral("Y");
  inspection.declared = profile;
  BackendAvailability backends;
  backends.available = {Backend::TensorRT};
  const QByteArray modelBytes = QByteArrayLiteral("fixed builder validation fixture");
  ModelInventory inventory(modelRoot);
  const ModelImportOutcome published =
      inventory.publish(modelBytes, inspection, QStringLiteral("fixture.onnx"),
                        QStringLiteral("fixture"), backends);
  QVERIFY2(published.ok, qPrintable(published.reasonKey));

  DeviceIdentity device;
  device.decoderDeviceUuid = QStringLiteral("GPU-dec0");
  device.computeDeviceUuid = QStringLiteral("GPU-cmp0");
  device.smMajor = 12;
  device.smMinor = 0;

  BuilderRequest request;
  request.plan.id = QStringLiteral("sha256:") + QString(64, QLatin1Char('a'));
  request.plan.backend = Backend::TensorRT;
  request.plan.device = device;
  ResolvedStage stage;
  stage.kind = StageKind::Spatial;
  stage.role = ModelRole::Spatial;
  stage.modelId = published.model.record.id;
  stage.modelContentId = QStringLiteral("sha256:") + QString(64, QLatin1Char('b'));
  stage.inputGeometry = QSize(64, 64);
  stage.outputGeometry = QSize(128, 128);
  stage.inputChannels = 3;
  stage.outputChannels = 3;
  request.plan.stages = {stage};
  request.models = {published.model};
  request.references = {BuilderStageReference{}};
  request.modelRoot = modelRoot;
  request.store = ArtifactStore(artifactRoot);
  request.workspaceBytes = 268435456;
  request.build.provider = Backend::TensorRT;
  request.build.runtimeVersion = QStringLiteral("10.7.0");
  request.build.pluginVersion = QStringLiteral("10.7.0");
  request.build.coreSha = QString(40, QLatin1Char('a'));
  request.build.sourceAbi = 1;
  request.build.deviceUuid = device.computeDeviceUuid;
  request.build.smMajor = device.smMajor;
  request.build.smMinor = device.smMinor;
  request.build.cudaVersion = QStringLiteral("12.8");
  request.build.driverVersion = QStringLiteral("561.09");
  request.build.stronglyTyped = true;
  request.build.skipInference = true;
  request.build.options = {
      BuildOption{QStringLiteral("builderOptimizationLevel"), QStringLiteral("3"), true}};
  const QString appExecutable = QCoreApplication::applicationFilePath();
  request.tools.inspectExecutable = appExecutable;
  request.tools.prepExecutable = appExecutable;
  request.tools.verifierExecutable = appExecutable;
  QTemporaryDir runtime;
  QVERIFY(runtime.isValid());
  QFile ort(QDir(runtime.path()).filePath(QStringLiteral("onnxruntime.dll")));
  QVERIFY(ort.open(QIODevice::WriteOnly));
  ort.close();
  request.tools.ortRuntimeDir = runtime.path();

  QVector<QString> phases;
  const BuilderResult result = runNativeBuild(
      request, nullptr, [&phases](const BuilderProgress& progress) {
        phases.append(progress.phase);
      });
  QVERIFY(!result.ok);
  QCOMPARE(result.reasonKey, QStringLiteral("model.owned.identity"));
  QVERIFY(!phases.contains(QStringLiteral("reference")));
  QVERIFY(!phases.contains(QStringLiteral("build")));
}

void TestNativeBuilder::cancellationPreventsPublication() {
  NativeBuildCancellation cancellation;
  QCOMPARE(cancellation.cancel(), NativeBuildCancellation::CancelResult::CancelledNow);
  QVERIFY(!cancellation.beginPublication());
  QCOMPARE(cancellation.cancel(), NativeBuildCancellation::CancelResult::AlreadyCancelled);
}

void TestNativeBuilder::committedPublicationCannotBeReportedCancelled() {
  NativeBuildCancellation cancellation;
  QVERIFY(cancellation.beginPublication());
  QCOMPARE(cancellation.cancel(), NativeBuildCancellation::CancelResult::TooLate);
  QVERIFY(!cancellation.beginPublication());
}

void TestNativeBuilder::concurrentCancellationAndPublicationHaveOneWinner() {
  for (int attempt = 0; attempt < 32; ++attempt) {
    NativeBuildCancellation cancellation;
    std::barrier start(3);
    bool publication = false;
    NativeBuildCancellation::CancelResult cancelled;
    std::thread publisher([&] {
      start.arrive_and_wait();
      publication = cancellation.beginPublication();
    });
    std::thread canceller([&] {
      start.arrive_and_wait();
      cancelled = cancellation.cancel();
    });
    start.arrive_and_wait();
    publisher.join();
    canceller.join();
    QCOMPARE(cancelled, publication ? NativeBuildCancellation::CancelResult::TooLate
                                    : NativeBuildCancellation::CancelResult::CancelledNow);
  }
}

void TestNativeBuilder::preparationExcludesOtherGpuWorkUntilItsMovedLeaseEnds() {
  {
    auto preparation = NativeWorkLease::acquire(NativeWorkKind::Preparation);
    QVERIFY(preparation.has_value());
    NativeWorkLease moved(std::move(*preparation));
    preparation.reset();
    QVERIFY(!NativeWorkLease::acquire(NativeWorkKind::Inference).has_value());
    QVERIFY(!NativeWorkLease::acquire(NativeWorkKind::Preparation).has_value());
  }
  QVERIFY(NativeWorkLease::acquire(NativeWorkKind::Inference).has_value());
}

void TestNativeBuilder::everyInferenceOwnerMustReleaseBeforePreparation() {
  auto main = NativeWorkLease::acquire(NativeWorkKind::Inference);
  auto pip = NativeWorkLease::acquire(NativeWorkKind::Inference);
  QVERIFY(main.has_value());
  QVERIFY(pip.has_value());
  main.reset();
  QVERIFY(!NativeWorkLease::acquire(NativeWorkKind::Preparation).has_value());
  pip.reset();
  QVERIFY(NativeWorkLease::acquire(NativeWorkKind::Preparation).has_value());
}

namespace {

// One complete, observed probe document. Every test below is this document with
// exactly one member changed, so a refusal names the changed member.
QJsonObject builderProbeDocument(const QString& root) {
  const QJsonObject source{{QStringLiteral("width"), 1920},
                           {QStringLiteral("height"), 1080},
                           {QStringLiteral("fps"),
                            QJsonObject{{QStringLiteral("num"), 24}, {QStringLiteral("den"), 1}}}};
  const QJsonObject device{{QStringLiteral("decoderUuid"), QStringLiteral("GPU-dec0")},
                           {QStringLiteral("computeUuid"), QStringLiteral("GPU-cmp0")},
                           {QStringLiteral("smMajor"), 12},
                           {QStringLiteral("smMinor"), 0}};
  const QJsonObject build{
      {QStringLiteral("provider"), QStringLiteral("tensorrt")},
      {QStringLiteral("runtimeVersion"), QStringLiteral("10.7.0")},
      {QStringLiteral("pluginVersion"), QStringLiteral("10.7.0")},
      {QStringLiteral("coreSha"), QString(40, QLatin1Char('a'))},
      {QStringLiteral("sourceAbi"), 1},
      {QStringLiteral("precision"), QStringLiteral("fp16")},
      {QStringLiteral("tf32"), false},
      {QStringLiteral("stronglyTyped"), true},
      {QStringLiteral("skipInference"), true},
      {QStringLiteral("deviceUuid"), QStringLiteral("GPU-cmp0")},
      {QStringLiteral("smMajor"), 12},
      {QStringLiteral("smMinor"), 0},
      {QStringLiteral("cudaVersion"), QStringLiteral("12.8")},
      {QStringLiteral("driverVersion"), QStringLiteral("561.09")},
      {QStringLiteral("options"),
       QJsonArray{QStringLiteral("--builderFlags=65"),
                  QStringLiteral("--tacticSources=-CUDNN,-CUBLAS,-CUBLAS_LT"),
                  QStringLiteral("--networkFlags=1"),
                  QStringLiteral("--builderOptimizationLevel=3"),
                  QStringLiteral("--engineCapability=standard")}}};
  const QDir base(root);
  const QJsonObject tools{
      {QStringLiteral("inspect"), base.filePath(QStringLiteral("tools/native_inspect.exe"))},
      {QStringLiteral("prep"), base.filePath(QStringLiteral("tools/ajinative.exe"))},
      {QStringLiteral("verifier"), base.filePath(QStringLiteral("tools/ajiverify.exe"))},
      {QStringLiteral("ortRuntimeDir"), base.filePath(QStringLiteral("ort"))}};
  return QJsonObject{
      {QStringLiteral("recipe"), base.filePath(QStringLiteral("recipes/default.json"))},
      {QStringLiteral("modelRoot"), base.filePath(QStringLiteral("models"))},
      {QStringLiteral("store"), base.filePath(QStringLiteral("artifacts"))},
      {QStringLiteral("workspaceBytes"), 268435456},
      {QStringLiteral("source"), source},
      {QStringLiteral("device"), device},
      {QStringLiteral("backends"), QJsonArray{QStringLiteral("tensorrt")}},
      {QStringLiteral("build"), build},
      {QStringLiteral("tools"), tools},
      {QStringLiteral("references"),
       QJsonArray{QJsonObject{{QStringLiteral("fixture"),
                               base.filePath(QStringLiteral("frames/frame.bin"))}}}}};
}

// Replaces the declared builderOptimizationLevel entries with `levels`, so a
// test can set exactly zero, one or several of them.
QJsonObject withOptimizationLevels(QJsonObject document, const QJsonArray& levels) {
  QJsonObject build = document.value(QStringLiteral("build")).toObject();
  QJsonArray options;
  for (const QJsonValue& entry : build.value(QStringLiteral("options")).toArray()) {
    if (!entry.toString().startsWith(QStringLiteral("--builderOptimizationLevel"))) {
      options.append(entry);
    }
  }
  for (const QJsonValue& level : levels) {
    options.append(QStringLiteral("--builderOptimizationLevel=%1").arg(level.toInt()));
  }
  build.insert(QStringLiteral("options"), options);
  document.insert(QStringLiteral("build"), build);
  return document;
}

}  // namespace

void TestNativeBuilder::probeRequestParsesEveryObservedFact() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  const QDir base(temp.path());
  BuilderProbeRequest request;
  QString key;
  QString reason;
  QVERIFY(parseBuilderProbeRequest(builderProbeDocument(temp.path()), &request, &key, &reason));
  QVERIFY(key.isEmpty());
  QVERIFY(reason.isEmpty());
  QCOMPARE(request.recipePath, base.filePath(QStringLiteral("recipes/default.json")));
  QCOMPARE(request.request.modelRoot, base.filePath(QStringLiteral("models")));
  QCOMPARE(request.request.store.root(), base.filePath(QStringLiteral("artifacts")));
  QCOMPARE(request.request.workspaceBytes, static_cast<quint64>(268435456));
  QVERIFY(request.source.codedSize.has_value());
  QCOMPARE(*request.source.codedSize, QSize(1920, 1080));
  QVERIFY(request.source.fps.has_value());
  QCOMPARE(request.source.fps->num, 24);
  QCOMPARE(request.source.fps->den, 1);
  QCOMPARE(request.device.decoderDeviceUuid, QStringLiteral("GPU-dec0"));
  QCOMPARE(request.device.computeDeviceUuid, QStringLiteral("GPU-cmp0"));
  QCOMPARE(request.device.smMajor, 12);
  QCOMPARE(request.device.smMinor, 0);
  QCOMPARE(request.request.build.provider, Backend::TensorRT);
  QCOMPARE(request.request.build.deviceUuid, QStringLiteral("GPU-cmp0"));
  QCOMPARE(request.request.build.smMajor, 12);
  QCOMPARE(request.request.build.precision, EnginePrecision::Fp16);
  QVERIFY(!request.request.build.tf32);
  QVERIFY(request.request.build.stronglyTyped);
  QVERIFY(request.request.build.canonicalOptions().contains(QStringLiteral("builderFlags=65")));
  QVERIFY(request.request.build.canonicalOptions().contains(
      QStringLiteral("builderOptimizationLevel=3")));
  QCOMPARE(request.request.tools.prepExecutable, base.filePath(QStringLiteral("tools/ajinative.exe")));
  QCOMPARE(request.request.tools.ortRuntimeDir, base.filePath(QStringLiteral("ort")));
  QCOMPARE(request.backends.available.size(), 1);
  QVERIFY(request.backends.available.contains(Backend::TensorRT));
  QCOMPARE(request.request.references.size(), 1);
  QCOMPARE(request.request.references.first().fixturePath,
           base.filePath(QStringLiteral("frames/frame.bin")));
  // The plan and the owned rows are the probe's to fill after resolution; the
  // parser never invents either.
  QVERIFY(request.request.plan.stages.isEmpty());
  QVERIFY(request.request.models.isEmpty());

  // The ABI-8 producer carries TensorRT and DirectML only. A probe naming the
  // Standard-only RTX backend is refused by name, in either the offer list or
  // the build provider, instead of being offered to the planner or mapped onto
  // classic TensorRT.
  QJsonObject rtxOffers = builderProbeDocument(temp.path());
  rtxOffers.insert(QStringLiteral("backends"),
                   QJsonArray{QStringLiteral("tensorrt"), QStringLiteral("tensorrt-rtx")});
  QVERIFY(!parseBuilderProbeRequest(rtxOffers, &request, &key, &reason));
  QCOMPARE(key, QStringLiteral("probe.backend"));

  QJsonObject rtxProvider = builderProbeDocument(temp.path());
  QJsonObject rtxBuild = rtxProvider.value(QStringLiteral("build")).toObject();
  rtxBuild.insert(QStringLiteral("provider"), QStringLiteral("tensorrt-rtx"));
  rtxProvider.insert(QStringLiteral("build"), rtxBuild);
  QVERIFY(!parseBuilderProbeRequest(rtxProvider, &request, &key, &reason));
  QCOMPARE(key, QStringLiteral("probe.backend"));
}

void TestNativeBuilder::probeRequestRefusesAnIncompleteBuildIdentity() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  QJsonObject document = builderProbeDocument(temp.path());
  QJsonObject build = document.value(QStringLiteral("build")).toObject();
  build.remove(QStringLiteral("cudaVersion"));
  document.insert(QStringLiteral("build"), build);
  BuilderProbeRequest request;
  QString key;
  QString reason;
  QVERIFY(!parseBuilderProbeRequest(document, &request, &key, &reason));
  QCOMPARE(key, QStringLiteral("probe.member"));
  QVERIFY(!reason.isEmpty());
  QVERIFY(request.recipePath.isEmpty());
}

void TestNativeBuilder::probeRequestRefusesPlaceholderIdentity() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  QJsonObject document = builderProbeDocument(temp.path());
  QJsonObject build = document.value(QStringLiteral("build")).toObject();
  build.insert(QStringLiteral("runtimeVersion"), QStringLiteral("unknown"));
  document.insert(QStringLiteral("build"), build);
  BuilderProbeRequest request;
  QString key;
  QString reason;
  QVERIFY(!parseBuilderProbeRequest(document, &request, &key, &reason));
  QCOMPARE(key, QStringLiteral("probe.build"));
  QVERIFY(reason.contains(QStringLiteral("runtimeVersion")));
  QVERIFY(request.request.build.runtimeVersion.isEmpty());
}

void TestNativeBuilder::probeRequestRefusesRelativePaths() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  QJsonObject document = builderProbeDocument(temp.path());
  QJsonObject tools = document.value(QStringLiteral("tools")).toObject();
  tools.insert(QStringLiteral("prep"), QStringLiteral("ajinative.exe"));
  document.insert(QStringLiteral("tools"), tools);
  BuilderProbeRequest request;
  QString key;
  QString reason;
  QVERIFY(!parseBuilderProbeRequest(document, &request, &key, &reason));
  QCOMPARE(key, QStringLiteral("probe.path"));
  QVERIFY(request.request.tools.prepExecutable.isEmpty());
}

void TestNativeBuilder::probeRequestRequiresAnExplicitWorkspace() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  QJsonObject document = builderProbeDocument(temp.path());
  document.remove(QStringLiteral("workspaceBytes"));
  BuilderProbeRequest request;
  QString key;
  QString reason;
  QVERIFY(!parseBuilderProbeRequest(document, &request, &key, &reason));
  QCOMPARE(key, QStringLiteral("probe.member"));
}

void TestNativeBuilder::probeRequestRejectsAnUnknownMember() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  QJsonObject document = builderProbeDocument(temp.path());
  document.insert(QStringLiteral("deviceName"), QStringLiteral("RTX 5090"));
  BuilderProbeRequest request;
  QString key;
  QString reason;
  QVERIFY(!parseBuilderProbeRequest(document, &request, &key, &reason));
  QCOMPARE(key, QStringLiteral("probe.unknown"));
  QVERIFY(reason.contains(QStringLiteral("deviceName")));
}

void TestNativeBuilder::probeRequestRefusesTwoReferenceModes() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  QJsonObject document = builderProbeDocument(temp.path());
  QJsonObject both{{QStringLiteral("fixture"),
                    QDir(temp.path()).filePath(QStringLiteral("frames/frame.bin"))},
                   {QStringLiteral("raw"),
                    QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("input")},
                                           {QStringLiteral("dtype"), QStringLiteral("fp16")},
                                           {QStringLiteral("shape"), QStringLiteral("1x3x64x64")},
                                           {QStringLiteral("path"),
                                            QDir(temp.path()).filePath(
                                                QStringLiteral("frames/input.bin"))}}}}};
  document.insert(QStringLiteral("references"), QJsonArray{both});
  BuilderProbeRequest request;
  QString key;
  QString reason;
  QVERIFY(!parseBuilderProbeRequest(document, &request, &key, &reason));
  QCOMPARE(key, QStringLiteral("probe.reference.mode"));
}

void TestNativeBuilder::probeRequestAcceptsOptimizationLevelBounds() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  for (const int level : {0, 5}) {
    BuilderProbeRequest request;
    QString key;
    QString reason;
    const QJsonObject document =
        withOptimizationLevels(builderProbeDocument(temp.path()), QJsonArray{level});
    QVERIFY2(parseBuilderProbeRequest(document, &request, &key, &reason), qPrintable(reason));
    QVERIFY(request.request.build.canonicalOptions().contains(
        QStringLiteral("builderOptimizationLevel=%1").arg(level)));
  }
}

void TestNativeBuilder::probeRequestRefusesAnInvalidOptimizationLevel() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  for (const int level : {6, -1}) {
    BuilderProbeRequest request;
    QString key;
    QString reason;
    const QJsonObject document =
        withOptimizationLevels(builderProbeDocument(temp.path()), QJsonArray{level});
    QVERIFY(!parseBuilderProbeRequest(document, &request, &key, &reason));
    QCOMPARE(key, QStringLiteral("build.options.optimizationLevel"));
  }
}

void TestNativeBuilder::probeRequestRefusesAMissingOptimizationLevel() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  BuilderProbeRequest request;
  QString key;
  QString reason;
  QVERIFY(!parseBuilderProbeRequest(
      withOptimizationLevels(builderProbeDocument(temp.path()), QJsonArray{}), &request, &key, &reason));
  QCOMPARE(key, QStringLiteral("build.options.optimizationLevel"));
}

void TestNativeBuilder::probeRequestRefusesARepeatedOptimizationLevel() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  BuilderProbeRequest request;
  QString key;
  QString reason;
  QVERIFY(!parseBuilderProbeRequest(
      withOptimizationLevels(builderProbeDocument(temp.path()), QJsonArray{3, 3}), &request, &key, &reason));
  QCOMPARE(key, QStringLiteral("build.options.optimizationLevel"));
}

namespace {

// The real 640x360 spatial output of this recipe: rank-4, with an outermost
// contiguous stride of 3*360*640 = 691200 elements, well past the 65536 per-axis
// extent bound a shape is held to.
QJsonArray hd360Shape() { return QJsonArray{1, 3, 360, 640}; }

QJsonArray stridesOf(qint64 n, qint64 c, qint64 h, qint64 w) {
  return QJsonArray{c * h * w, h * w, w, 1};
}

}  // namespace

void TestNativeBuilder::aContiguousStridePastTheExtentBoundIsAccepted() {
  QVERIFY(tensorStridesAreContiguous(stridesOf(3, 3, 360, 640), hd360Shape()));
  QVERIFY(tensorStridesAreContiguous(stridesOf(3, 3, 180, 320), QJsonArray{1, 3, 180, 320}));
}

void TestNativeBuilder::aNonContiguousStrideIsRefused() {
  // Channel-last order: right strides, wrong memory order for a raw NCHW tensor.
  QVERIFY(!tensorStridesAreContiguous(QJsonArray{1, 640, 1920, 3}, hd360Shape()));
  // The correct vector with one axis transposed is still not contiguous NCHW.
  QVERIFY(!tensorStridesAreContiguous(QJsonArray{691200, 230400, 640, 2}, hd360Shape()));
}

void TestNativeBuilder::aStridePastTheTensorIsRefused() {
  QVERIFY(!tensorStridesAreContiguous(QJsonArray{691201, 230400, 640, 1}, hd360Shape()));
  QVERIFY(!tensorStridesAreContiguous(QJsonArray{691200, 230400, 640, 0}, hd360Shape()));
  QVERIFY(!tensorStridesAreContiguous(QJsonArray{-691200, 230400, 640, 1}, hd360Shape()));
}

void TestNativeBuilder::aStrideArrayThatIsNotRank4IsRefused() {
  QVERIFY(!tensorStridesAreContiguous(QJsonArray{691200, 230400, 640}, hd360Shape()));
  QVERIFY(!tensorStridesAreContiguous(QJsonArray{691200, 230400, 640, 1, 1}, hd360Shape()));
  QVERIFY(!tensorStridesAreContiguous(QJsonArray{}, hd360Shape()));
  // A rank-4 stride array against a shape that is not rank-4 positive is refused.
  QVERIFY(!tensorStridesAreContiguous(stridesOf(3, 3, 360, 640), QJsonArray{1, 3, 360}));
}
