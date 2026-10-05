#include "TestNativeManifest.hpp"

#include "native/NativeArtifactStore.hpp"
#include "native/NativeManifest.hpp"
#include "native/NativePaths.hpp"

#include <optional>
#include <utility>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>
#include <QtCore/QDirIterator>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace mplayer;
using namespace mplayer::native;

// Pure identity, manifest and cache logic against real temporary files. No
// engine is built, serialized or deserialized and no native runtime is loaded,
// so the full prepare/activate acceptance is deliberately not claimed here.
namespace {

const QString kCoreSha = QStringLiteral("63d127174d8f70336c82a22408929e2bc301edcb");
const QString kCreatedAt = QStringLiteral("2026-10-01T00:00:00Z");

QString sha256Hex(const QByteArray& data) {
  return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool writeBytes(const QString& path, const QByteArray& data) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  return file.write(data) == data.size();
}

QByteArray readBytes(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return file.readAll();
}

ModelProfile spatialProfile() {
  ModelProfile profile;
  profile.input.dtype = QString::fromLatin1(kEligibleIoType);
  profile.input.rank = 4;
  profile.input.batch = 1;
  profile.input.channels = 3;
  profile.input.height = 1080;
  profile.input.width = 1920;
  profile.output.dtype = QString::fromLatin1(kEligibleIoType);
  profile.output.rank = 4;
  profile.output.batch = 1;
  profile.output.channels = 3;
  profile.output.height = 2160;
  profile.output.width = 3840;
  return profile;
}

GraphContentIdentity graphFor(const QByteArray& graphBytes, ModelRole role = ModelRole::Spatial) {
  GraphContentIdentity graph;
  graph.role = role;
  graph.graphSha256 = sha256Hex(graphBytes);
  graph.declaredProfile = spatialProfile();
  return graph;
}

EngineBuildIdentity buildFor(const QString& driver = QStringLiteral("616.92"),
                             const QString& uuid = QStringLiteral("GPU-9f2c1a44-0b7d-4e21-9c3a-1d5e"),
                             const QString& cuda = QStringLiteral("13.4.92")) {
  EngineBuildIdentity build;
  build.provider = Backend::TensorRT;
  build.runtimeVersion = QStringLiteral("11.3.0.99");
  build.pluginVersion = QStringLiteral("1.2.3");
  build.coreSha = kCoreSha;
  build.sourceAbi = 8;
  build.precision = EnginePrecision::Fp16;
  build.tf32 = false;
  build.stronglyTyped = true;
  build.skipInference = true;
  build.deviceUuid = uuid;
  build.smMajor = 12;
  build.smMinor = 0;
  build.cudaVersion = cuda;
  build.driverVersion = driver;
  build.options = QVector<BuildOption>{
      BuildOption{QStringLiteral("stronglyTyped"), QString(), false},
      BuildOption{QStringLiteral("skipInference"), QString(), false},
      BuildOption{QStringLiteral("tacticSources"), QStringLiteral("-CUDNN,-CUBLAS,-CUBLAS_LT"), true},
      BuildOption{QStringLiteral("optShapes"), QStringLiteral("1x3x1088x1920,1x3x2176x3840"), true}};
  return build;
}

StageBuildRequest stageFor() {
  StageBuildRequest stage;
  stage.role = ModelRole::Spatial;
  stage.inputShape = QStringLiteral("1x3x1088x1920");
  stage.outputShape = QStringLiteral("1x3x2176x3840");
  stage.ioFormat = QStringLiteral("fp16:chw");
  stage.padLeft = 4;
  stage.padTop = 4;
  return stage;
}

BuildRequest requestFor(const QByteArray& graphBytes, const EngineBuildIdentity& build) {
  BuildRequest request;
  request.graph = graphFor(graphBytes);
  request.stage = stageFor();
  request.build = build;
  return request;
}

ArtifactManifest manifestFor(const BuildRequest& request, const QByteArray& graphBytes,
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
  manifest.createdAt = kCreatedAt;
  return manifest;
}

struct Sample {
  QByteArray graphBytes;
  QByteArray engineBytes;
  BuildRequest request;
  ArtifactManifest manifest;
};

Sample makeSample(const EngineBuildIdentity& build = buildFor()) {
  Sample sample;
  sample.graphBytes = QByteArrayLiteral("onnx-graph-bytes-v1");
  sample.engineBytes = QByteArray(4096, char(0x42));
  sample.request = requestFor(sample.graphBytes, build);
  sample.manifest = manifestFor(sample.request, sample.graphBytes, sample.engineBytes);
  return sample;
}

std::optional<ArtifactRecord> publishSample(const ArtifactStore& store, const Sample& sample,
                                            const QString& scratchDir, QString* reason = nullptr) {
  const QString scratchEngine = QDir(scratchDir).filePath(QStringLiteral("produced.engine"));
  if (!writeBytes(scratchEngine, sample.engineBytes)) return std::nullopt;
  return publishArtifact(store, sample.manifest, scratchEngine, nullptr, reason);
}

/// Publishes `produced` - which need not be the bytes the manifest records -
/// through a real scratch file, so the refusal key is available to the caller.
std::optional<ArtifactRecord> publishProduced(const ArtifactStore& store,
                                              const ArtifactManifest& manifest,
                                              const QByteArray& produced,
                                              const QString& scratchDir, QString* reasonKey,
                                              QString* reason) {
  const QString scratchEngine = QDir(scratchDir).filePath(QStringLiteral("produced.engine"));
  if (!writeBytes(scratchEngine, produced)) return std::nullopt;
  return publishArtifact(store, manifest, scratchEngine, reasonKey, reason);
}

}  // namespace

// --------------------------------------------------------------- identities

void TestNativeManifest::aGraphKeepsOneContentIdentityAcrossTwoMachines() {
  const QByteArray graphBytes = QByteArrayLiteral("same-graph");
  const GraphContentIdentity first = graphFor(graphBytes);
  const GraphContentIdentity second = graphFor(graphBytes);
  QCOMPARE(first.contentId(), second.contentId());

  const BuildRequest onOneMachine = requestFor(graphBytes, buildFor(QStringLiteral("616.92")));
  const BuildRequest onAnother =
      requestFor(graphBytes, buildFor(QStringLiteral("617.00"), QStringLiteral("GPU-other")));
  QCOMPARE(onOneMachine.graph.contentId(), onAnother.graph.contentId());
  QVERIFY(onOneMachine.buildKey() != onAnother.buildKey());
  QCOMPARE(onOneMachine.payload().section(QLatin1Char('\n'), 1, 1),
           onAnother.payload().section(QLatin1Char('\n'), 1, 1));
}

void TestNativeManifest::twoFilesWithTheSameBasenameHaveDifferentIdentities() {
  const GraphContentIdentity one = graphFor(QByteArrayLiteral("bytes-one"));
  const GraphContentIdentity two = graphFor(QByteArrayLiteral("bytes-two"));
  QVERIFY(one.contentId() != two.contentId());
  QVERIFY(one.payload() != two.payload());
}

void TestNativeManifest::aChangedDriverChangesTheBuildKey() {
  const QByteArray graphBytes = QByteArrayLiteral("graph");
  const QString before = requestFor(graphBytes, buildFor(QStringLiteral("616.92"))).buildKey();
  const QString after = requestFor(graphBytes, buildFor(QStringLiteral("617.00"))).buildKey();
  QVERIFY(before != after);
}

void TestNativeManifest::theManifestRecomputesItsOwnBuildKey() {
  const Sample sample = makeSample();
  QCOMPARE(sample.manifest.request().buildKey(), sample.manifest.buildKey);

  ArtifactManifest parsed;
  QString key;
  QString reason;
  QVERIFY2(parseArtifactManifest(serializeArtifactManifest(sample.manifest), &parsed, &key, &reason),
           qPrintable(reason));
  QCOMPARE(parsed.buildKey, sample.manifest.buildKey);
  QCOMPARE(parsed.request().buildKey(), parsed.buildKey);
  QCOMPARE(parsed.engineSha256, sample.manifest.engineSha256);
}

void TestNativeManifest::anUnknownMemberIsRefused() {
  QJsonObject object = artifactManifestToJson(makeSample().manifest);
  object.insert(QStringLiteral("surprise"), QStringLiteral("guessed"));
  ArtifactManifest parsed;
  QString key;
  QString reason;
  QVERIFY(!parseArtifactManifest(object, &parsed, &key, &reason));
  QCOMPARE(key, QStringLiteral("manifest.unknownMember"));
}

void TestNativeManifest::aMissingRequiredMemberIsRefused() {
  QJsonObject object = artifactManifestToJson(makeSample().manifest);
  object.remove(QStringLiteral("createdAt"));
  ArtifactManifest parsed;
  QString key;
  QString reason;
  QVERIFY(!parseArtifactManifest(object, &parsed, &key, &reason));
  QCOMPARE(key, QStringLiteral("manifest.member"));
}

void TestNativeManifest::aFutureSchemaIsRefused() {
  {
    QJsonObject object = artifactManifestToJson(makeSample().manifest);
    object.insert(QStringLiteral("schema"), QStringLiteral("mikai.native.manifest/2"));
    ArtifactManifest parsed;
    QString key;
    QString reason;
    QVERIFY(!parseArtifactManifest(object, &parsed, &key, &reason));
    QCOMPARE(key, QStringLiteral("manifest.schema"));
  }
  {
    QJsonObject object = artifactManifestToJson(makeSample().manifest);
    object.insert(QStringLiteral("mikaiFormat"), 2);
    ArtifactManifest parsed;
    QString key;
    QString reason;
    QVERIFY(!parseArtifactManifest(object, &parsed, &key, &reason));
    QCOMPARE(key, QStringLiteral("manifest.mikaiFormat"));
  }
}

void TestNativeManifest::aBuildKeyThatIsNotTheRequestsKeyIsRefused() {
  QJsonObject object = artifactManifestToJson(makeSample().manifest);
  object.insert(QStringLiteral("buildKey"),
                QStringLiteral("sha256:") + QString(64, QLatin1Char('0')));
  ArtifactManifest parsed;
  QString key;
  QString reason;
  QVERIFY(!parseArtifactManifest(object, &parsed, &key, &reason));
  QCOMPARE(key, QStringLiteral("manifest.buildKey"));
}

void TestNativeManifest::theEngineDigestIsRecordedSeparatelyFromTheBuildKey() {
  Sample one = makeSample();
  Sample two = makeSample();
  two.engineBytes = QByteArray(4096, char(0x43));
  two.manifest = manifestFor(two.request, two.graphBytes, two.engineBytes);

  QCOMPARE(one.manifest.buildKey, two.manifest.buildKey);
  QVERIFY(one.manifest.engineSha256 != two.manifest.engineSha256);

  ArtifactManifest parsed;
  QString key;
  QString reason;
  QVERIFY2(parseArtifactManifest(serializeArtifactManifest(two.manifest), &parsed, &key, &reason),
           qPrintable(reason));
  QCOMPARE(parsed.engineSha256, two.manifest.engineSha256);
}

void TestNativeManifest::optionValuesRoundTripAndKeepTheirOrder() {
  QString reason;
  const auto parsed = parseBuildOptions(QStringLiteral("--alpha=1 --beta=\"two words\""), &reason);
  QVERIFY2(parsed.has_value(), qPrintable(reason));
  QCOMPARE(int(parsed->size()), 2);
  QCOMPARE(parsed->at(1).name, QStringLiteral("beta"));
  QCOMPARE(parsed->at(1).value, QStringLiteral("two words"));
  QCOMPARE(canonicalizeBuildOptions(*parsed), QStringLiteral("--alpha=1 --beta=\"two words\""));

  const QVector<BuildOption> ordered{BuildOption{QStringLiteral("beta"), QString(), false},
                                     BuildOption{QStringLiteral("alpha"), QStringLiteral("1"), true}};
  QCOMPARE(canonicalizeBuildOptions(ordered), QStringLiteral("--beta --alpha=1"));

  QCOMPARE(canonicalBuildOptionString(QStringLiteral("--tacticSources=-CUDNN,-CUBLAS,-CUBLAS_LT")),
           QStringLiteral("--tacticSources=-CUBLAS,-CUBLAS_LT,-CUDNN"));
}

void TestNativeManifest::theOptionParserRefusesATokenThatIsNotAnOption() {
  QString reason;
  QVERIFY(!parseBuildOptions(QStringLiteral("--ok bare"), &reason).has_value());
  QVERIFY(!reason.isEmpty());
  QVERIFY(!parseBuildOptions(QStringLiteral("--dangling=\"open"), &reason).has_value());
}

void TestNativeManifest::precisionAndTf32ArePartOfTheBuildKey() {
  const QByteArray graphBytes = QByteArrayLiteral("graph");
  EngineBuildIdentity fp32 = buildFor();
  fp32.precision = EnginePrecision::Fp32;
  EngineBuildIdentity tf32 = buildFor();
  tf32.tf32 = true;

  const QString base = requestFor(graphBytes, buildFor()).buildKey();
  QVERIFY(base != requestFor(graphBytes, fp32).buildKey());
  QVERIFY(base != requestFor(graphBytes, tf32).buildKey());
}

void TestNativeManifest::aPlaceholderOrMalformedFieldLeavesTheRequestIncomplete() {
  EngineBuildIdentity noDriver = buildFor();
  noDriver.driverVersion.clear();
  QCOMPARE(noDriver.firstMissingRequiredField(), QStringLiteral("driverVersion"));

  EngineBuildIdentity placeholder = buildFor();
  placeholder.cudaVersion = QStringLiteral("unknown");
  QCOMPARE(placeholder.firstMissingRequiredField(), QStringLiteral("cudaVersion"));

  EngineBuildIdentity badCore = buildFor();
  badCore.coreSha = QStringLiteral("not-a-source-sha");
  QCOMPARE(badCore.firstMissingRequiredField(), QStringLiteral("coreSha"));

  const BuildRequest incomplete =
      requestFor(QByteArrayLiteral("graph"), buildFor(QString(), QStringLiteral("GPU-x")));
  QVERIFY(!incomplete.isComplete());

  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  QString key;
  QString reason;
  QVERIFY(!findArtifactForRequest(store, incomplete, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("request.incomplete"));
}

void TestNativeManifest::aMalformedGraphDigestIsRefused() {
  QJsonObject object = artifactManifestToJson(makeSample().manifest);
  QJsonObject source = object.value(QStringLiteral("source")).toObject();
  source.insert(QStringLiteral("onnxSha256"), QString(63, QLatin1Char('a')));
  object.insert(QStringLiteral("source"), source);
  ArtifactManifest parsed;
  QString key;
  QString reason;
  QVERIFY(!parseArtifactManifest(object, &parsed, &key, &reason));
  QCOMPARE(key, QStringLiteral("manifest.source.onnxSha256"));
}

void TestNativeManifest::aStageShapeWithAZeroDimensionIsRefused() {
  QJsonObject object = artifactManifestToJson(makeSample().manifest);
  QJsonObject stage = object.value(QStringLiteral("stage")).toObject();
  stage.insert(QStringLiteral("input"), QStringLiteral("1x3x0x1920"));
  object.insert(QStringLiteral("stage"), stage);
  ArtifactManifest parsed;
  QString key;
  QString reason;
  QVERIFY(!parseArtifactManifest(object, &parsed, &key, &reason));
  QCOMPARE(key, QStringLiteral("manifest.stage.shape"));
}

void TestNativeManifest::aSensitiveRuntimeIsRefusedAndNeverExported() {
  const QString url = QStringLiteral("https://build.example.invalid/trt?token=Zm9vYmFy");
  const QString key = QStringLiteral("mk_live_9f3c2a7d4b6e");

  // A runtime identity that is really a signed URL is not a runtime. The build
  // key is computed for the planted value, so the document is otherwise a valid
  // request and only the credential rule can refuse it.
  Sample planted = makeSample();
  planted.manifest.build.runtime = url;
  planted.manifest.buildKey = planted.manifest.request().buildKey();
  QCOMPARE(planted.manifest.request().build.firstSensitiveField(),
           QStringLiteral("runtimeVersion"));
  QCOMPARE(planted.manifest.request().build.firstMissingRequiredField(),
           QStringLiteral("runtimeVersion"));
  QVERIFY(!planted.manifest.request().isComplete());

  QJsonObject object = artifactManifestToJson(planted.manifest);
  QJsonObject buildBlock = object.value(QStringLiteral("build")).toObject();
  // The writer redacted on the way out; put the raw value back so the parser is
  // what is exercised.
  buildBlock.insert(QStringLiteral("runtime"), url);
  object.insert(QStringLiteral("build"), buildBlock);
  ArtifactManifest parsed;
  QString reasonKey;
  QString reason;
  QVERIFY(!parseArtifactManifest(object, &parsed, &reasonKey, &reason));
  QCOMPARE(reasonKey, QStringLiteral("artifact.runtime.sensitiveData"));
  QVERIFY(!reason.contains(url));
  QVERIFY(!reason.contains(QStringLiteral("Zm9vYmFy")));
  QVERIFY(!planted.manifest.isSelfConsistent(&reasonKey, &reason));
  QCOMPARE(reasonKey, QStringLiteral("artifact.runtime.sensitiveData"));

  // The same rule catches the project's own key shape in the same field.
  Sample keyed = makeSample();
  keyed.manifest.build.runtime = key;
  keyed.manifest.buildKey = keyed.manifest.request().buildKey();
  QJsonObject keyedObject = artifactManifestToJson(keyed.manifest);
  QJsonObject keyedBuild = keyedObject.value(QStringLiteral("build")).toObject();
  keyedBuild.insert(QStringLiteral("runtime"), key);
  keyedObject.insert(QStringLiteral("build"), keyedBuild);
  QVERIFY(!parseArtifactManifest(keyedObject, &parsed, &reasonKey, &reason));
  QCOMPARE(reasonKey, QStringLiteral("artifact.runtime.sensitiveData"));

  // The export is the last boundary: a directly constructed manifest carrying a
  // credential emits no plaintext, while the ordinary hashes are still exactly
  // what was recorded.
  const QByteArray exported = serializeArtifactManifest(planted.manifest);
  QVERIFY(!exported.contains(url.toUtf8()));
  QVERIFY(!exported.contains(key.toUtf8()));
  QVERIFY(exported.contains(planted.manifest.buildKey.toUtf8()));
  QVERIFY(exported.contains(planted.manifest.engineSha256.toUtf8()));
  QVERIFY(exported.contains(planted.manifest.source.modelContentId.toUtf8()));

  // A member name planted with a secret is not echoed by the refusal that names
  // it as unknown.
  QJsonObject named = artifactManifestToJson(makeSample().manifest);
  named.insert(url, QStringLiteral("1"));
  ArtifactManifest namedParsed;
  QVERIFY(!parseArtifactManifest(named, &namedParsed, &reasonKey, &reason));
  QCOMPARE(reasonKey, QStringLiteral("manifest.unknownMember"));
  QVERIFY(!reason.contains(url));
  QVERIFY(!reason.contains(QStringLiteral("Zm9vYmFy")));

  // An ordinary document is untouched by either rule.
  Sample clean = makeSample();
  QString cleanKey;
  QString cleanReason;
  QVERIFY(clean.manifest.isSelfConsistent(&cleanKey, &cleanReason));
  ArtifactManifest roundTripped;
  QVERIFY2(parseArtifactManifest(serializeArtifactManifest(clean.manifest), &roundTripped,
                                 &cleanKey, &cleanReason),
           qPrintable(cleanReason));
  QCOMPARE(roundTripped.buildKey, clean.manifest.buildKey);
  QCOMPARE(roundTripped.build.runtime, clean.manifest.build.runtime);
}

// ------------------------------------------------------------------- store

void TestNativeManifest::publishingWritesAnArtifactThatVerifies() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample sample = makeSample();

  QString reason;
  const auto record = publishSample(store, sample, temp.path(), &reason);
  QVERIFY2(record.has_value(), qPrintable(reason));
  QCOMPARE(record->buildKey(), sample.manifest.buildKey);
  QVERIFY(QFileInfo::exists(record->enginePath));

  QString key;
  const auto reread = readArtifact(store, sample.manifest.buildKey, &key, &reason);
  QVERIFY2(reread.has_value(), qPrintable(reason));
  QVERIFY2(verifyArtifact(store, *reread, &key, &reason), qPrintable(reason));

  const auto found = findArtifactForRequest(store, sample.request, &key, &reason);
  QVERIFY2(found.has_value(), qPrintable(reason));
  QCOMPARE(found->manifest.engineSha256, sample.manifest.engineSha256);
}

void TestNativeManifest::publishingLeavesNoStagingDirectoryBehind() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample sample = makeSample();
  const auto record = publishSample(store, sample, temp.path());
  QVERIFY(record.has_value());

  QVERIFY(stagingDirectories(store).isEmpty());
}

void TestNativeManifest::aCancelledBuildsStagingIsNeverSelectable() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample sample = makeSample();

  const QString staging = QDir(temp.path()).filePath(QStringLiteral(".staging-abc-engine"));
  QVERIFY(QDir().mkpath(staging));
  QVERIFY(writeBytes(QDir(staging).filePath(QStringLiteral("engine.plan")),
                     sample.engineBytes.left(64)));
  QCOMPARE(stagingDirectories(store), QStringList{QStringLiteral(".staging-abc-engine")});

  QString key;
  QString reason;
  QVERIFY(!readArtifact(store, sample.manifest.buildKey, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("artifact.missing"));
  QVERIFY(!findArtifactForRequest(store, sample.request, &key, &reason).has_value());

  QVERIFY2(discardStaging(store, QStringLiteral(".staging-abc-engine"), &key, &reason),
           qPrintable(reason));
  QVERIFY(stagingDirectories(store).isEmpty());
}

void TestNativeManifest::aTruncatedEngineIsRefusedBeforeUse() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample sample = makeSample();
  const auto record = publishSample(store, sample, temp.path());
  QVERIFY(record.has_value());

  QVERIFY(writeBytes(record->enginePath, sample.engineBytes.left(100)));
  QString key;
  QString reason;
  QVERIFY(!readArtifact(store, sample.manifest.buildKey, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("artifact.engineBytes"));
}

void TestNativeManifest::aTamperedEngineIsRefusedByDigest() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample sample = makeSample();
  const auto record = publishSample(store, sample, temp.path());
  QVERIFY(record.has_value());

  QByteArray tampered = sample.engineBytes;
  tampered[0] = char(tampered.at(0) ^ 0xFF);
  QVERIFY(writeBytes(record->enginePath, tampered));
  QString key;
  QString reason;
  QVERIFY(!readArtifact(store, sample.manifest.buildKey, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("artifact.engineSha256"));
}

void TestNativeManifest::aChangedSourceFileInvalidatesTheArtifact() {
  QTemporaryDir temp;
  const ArtifactStore store(QDir(temp.path()).filePath(QStringLiteral("store")));
  const Sample sample = makeSample();
  const auto record = publishSample(store, sample, temp.path());
  QVERIFY(record.has_value());

  const QString onnxPath =
      QDir(temp.path()).filePath(QStringLiteral("model/animejanai.onnx"));
  QVERIFY(writeBytes(onnxPath, sample.graphBytes));
  QString key;
  QString reason;
  QVERIFY2(verifyArtifactSource(*record, onnxPath, &key, &reason), qPrintable(reason));

  QVERIFY(writeBytes(onnxPath, sample.graphBytes + QByteArrayLiteral("edited")));
  QVERIFY(!verifyArtifactSource(*record, onnxPath, &key, &reason));
  QCOMPARE(key, QStringLiteral("source.edited"));
}

void TestNativeManifest::aChangedDriverInvalidatesTheArtifact() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample sample = makeSample();
  const auto record = publishSample(store, sample, temp.path());
  QVERIFY(record.has_value());

  EngineBuildIdentity observed = sample.request.build;
  QString key;
  QString reason;
  QVERIFY2(verifyArtifactRuntime(*record, observed, &key, &reason), qPrintable(reason));

  observed.driverVersion = QStringLiteral("617.00");
  QVERIFY(!verifyArtifactRuntime(*record, observed, &key, &reason));
  QCOMPARE(key, QStringLiteral("runtime.driver"));
}

void TestNativeManifest::anExternalModelDirectorySentinelSurvivesTheLifecycle() {
  QTemporaryDir temp;
  const ArtifactStore store(QDir(temp.path()).filePath(QStringLiteral("store")));
  const Sample sample = makeSample();
  const auto record = publishSample(store, sample, temp.path());
  QVERIFY(record.has_value());

  const QString modelDir = QDir(temp.path()).filePath(QStringLiteral("models"));
  const QString onnxPath = QDir(modelDir).filePath(QStringLiteral("animejanai.onnx"));
  const QString sentinelPath = QDir(modelDir).filePath(QStringLiteral("keep-me.bin"));
  const QByteArray sentinel = QByteArrayLiteral("SENTINEL-DO-NOT-TOUCH");
  QVERIFY(writeBytes(onnxPath, sample.graphBytes));
  QVERIFY(writeBytes(sentinelPath, sentinel));

  QString key;
  QString reason;
  QVERIFY2(verifyArtifactSource(*record, onnxPath, &key, &reason), qPrintable(reason));
  QVERIFY2(removeArtifact(store, sample.manifest.buildKey, &key, &reason), qPrintable(reason));

  QCOMPARE(readBytes(sentinelPath), sentinel);
  QCOMPARE(readBytes(onnxPath), sample.graphBytes);
}

void TestNativeManifest::removalRequiresProofAndLeavesForeignFiles() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample sample = makeSample();
  const auto record = publishSample(store, sample, temp.path());
  QVERIFY(record.has_value());

  const QString foreignPath = QDir(record->root).filePath(QStringLiteral("foreign.bin"));
  const QByteArray foreign = QByteArrayLiteral("not published by this cache");
  QVERIFY(writeBytes(foreignPath, foreign));

  QByteArray tampered = sample.engineBytes;
  tampered[16] = char(tampered.at(16) ^ 0xFF);
  QVERIFY(writeBytes(record->enginePath, tampered));

  QString key;
  QString reason;
  QVERIFY(!removeArtifact(store, sample.manifest.buildKey, &key, &reason));
  QCOMPARE(key, QStringLiteral("artifact.unproven"));
  QVERIFY(QFileInfo::exists(record->enginePath));

  QVERIFY(writeBytes(record->enginePath, sample.engineBytes));
  QVERIFY2(removeArtifact(store, sample.manifest.buildKey, &key, &reason), qPrintable(reason));
  QVERIFY(!QFileInfo::exists(record->enginePath));
  QVERIFY(!QFileInfo::exists(record->manifestPath));
  QCOMPARE(readBytes(foreignPath), foreign);
}

void TestNativeManifest::aLockedManifestRemovalDoesNotExposeAPartialArtifact() {
#ifdef Q_OS_WIN
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample sample = makeSample();
  const auto record = publishSample(store, sample, temp.path());
  QVERIFY(record.has_value());

  const QByteArray engineBytes = readBytes(record->enginePath);
  const QByteArray manifestBytes = readBytes(record->manifestPath);
  QVERIFY(!engineBytes.isEmpty());
  QVERIFY(!manifestBytes.isEmpty());

  const QString foreignPath = QDir(record->root).filePath(QStringLiteral("foreign-sentinel.bin"));
  const QByteArray foreignBytes = QByteArrayLiteral("foreign bytes must survive artifact removal");
  QVERIFY(writeBytes(foreignPath, foreignBytes));

  // Permit verification reads, but deny delete sharing on the real manifest file.
  const HANDLE lock = CreateFileW(
      reinterpret_cast<LPCWSTR>(record->manifestPath.utf16()), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  QVERIFY(lock != INVALID_HANDLE_VALUE);

  QString key;
  QString reason;
  const bool removedWhileLocked = removeArtifact(store, sample.manifest.buildKey, &key, &reason);
  const QString removalKey = key;
  const QString removalReason = reason;
  const BOOL lockClosed = CloseHandle(lock);
  QVERIFY(lockClosed != FALSE);
  QVERIFY(!removedWhileLocked);
  QCOMPARE(removalKey, QStringLiteral("artifact.removeFailed"));
  QVERIFY(!removalReason.isEmpty());

  QString lookupKey;
  QString lookupReason;
  const auto retained = readArtifact(store, sample.manifest.buildKey, &lookupKey, &lookupReason);
  if (retained.has_value()) {
    QCOMPARE(readBytes(retained->enginePath), engineBytes);
    QCOMPARE(readBytes(retained->manifestPath), manifestBytes);

    QVERIFY2(removeArtifact(store, sample.manifest.buildKey, &key, &reason), qPrintable(reason));
    QVERIFY(!QFileInfo::exists(record->enginePath));
    QVERIFY(!QFileInfo::exists(record->manifestPath));
    QCOMPARE(readBytes(foreignPath), foreignBytes);
  } else {
    QVERIFY(removalReason.contains(QStringLiteral("manifest")));
    QVERIFY(removalReason.contains(QStringLiteral("unselectable")));
    bool foreignPayloadFound = false;
    QDirIterator files(store.root(), QDir::Files | QDir::NoDotAndDotDot,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
      const QString path = files.next();
      if (QFileInfo(path).fileName() == QFileInfo(foreignPath).fileName() &&
          readBytes(path) == foreignBytes) {
        foreignPayloadFound = true;
        break;
      }
    }
    QVERIFY(foreignPayloadFound);
  }
#else
  QSKIP("Windows share-lock semantics are required for this deletion behavior");
#endif
}

void TestNativeManifest::anArtifactDirectoryWithoutAManifestIsNotRemoved() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample sample = makeSample();

  const QString buildKey = QStringLiteral("sha256:") + sha256Hex(QByteArrayLiteral("orphan"));
  const QString directory = QDir(temp.path()).filePath(
      ArtifactStore::artifactDirectoryName(buildKey));
  QVERIFY(QDir().mkpath(directory));
  QVERIFY(writeBytes(QDir(directory).filePath(QStringLiteral("engine.plan")), sample.engineBytes));

  QString key;
  QString reason;
  QVERIFY(!removeArtifact(store, buildKey, &key, &reason));
  QCOMPARE(key, QStringLiteral("artifact.unproven"));
  QVERIFY(QFileInfo::exists(QDir(directory).filePath(QStringLiteral("engine.plan"))));
}

void TestNativeManifest::anEngineNameThatEscapesTheRootIsRefused() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  Sample sample = makeSample();
  sample.manifest.engine.file = QStringLiteral("../escape.plan");

  QString reason;
  const auto record = publishSample(store, sample, temp.path(), &reason);
  QVERIFY(!record.has_value());
  QVERIFY(!reason.isEmpty());
  QVERIFY(!QFileInfo::exists(QDir(temp.path()).filePath(QStringLiteral("escape.plan"))));
}

void TestNativeManifest::aStoreRootThatIsNotADirectoryIsNotOwned() {
  QTemporaryDir temp;
  QString reason;

  const ArtifactStore empty{QString()};
  QVERIFY(!empty.isOwned(&reason));

  const QString filePath = QDir(temp.path()).filePath(QStringLiteral("not-a-dir"));
  QVERIFY(writeBytes(filePath, QByteArrayLiteral("x")));
  const ArtifactStore fileStore(filePath);
  QVERIFY(!fileStore.isOwned(&reason));

  const ArtifactStore missingParent(
      QDir(temp.path()).filePath(QStringLiteral("absent/child")));
  QVERIFY(!missingParent.isOwned(&reason));

  const ArtifactStore valid(QDir(temp.path()).filePath(QStringLiteral("new-store")));
  QVERIFY2(valid.isOwned(&reason), qPrintable(reason));
}

void TestNativeManifest::acquiringALeaseRequiresARealArtifact() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample sample = makeSample();

  QString key;
  QString reason;
  QVERIFY(!acquireArtifactLease(store, sample.manifest.buildKey, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("artifact.missing"));

  const auto record = publishSample(store, sample, temp.path());
  QVERIFY(record.has_value());

  QByteArray tampered = sample.engineBytes;
  tampered[0] = char(tampered.at(0) ^ 0xFF);
  QVERIFY(writeBytes(record->enginePath, tampered));
  QVERIFY(!acquireArtifactLease(store, sample.manifest.buildKey, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("artifact.engineSha256"));

  QVERIFY(writeBytes(record->enginePath, sample.engineBytes));
  const auto lease = acquireArtifactLease(store, sample.manifest.buildKey, &key, &reason);
  QVERIFY2(lease.has_value(), qPrintable(reason));
}

void TestNativeManifest::aLeaseHeldThroughAnotherStoreRefusesRemoval() {
  QTemporaryDir temp;
  const ArtifactStore first(temp.path());
  const ArtifactStore second(temp.path());
  const Sample sample = makeSample();
  const auto record = publishSample(first, sample, temp.path());
  QVERIFY(record.has_value());

  const QString userFile = QDir(temp.path()).filePath(QStringLiteral("keep-me.bin"));
  const QByteArray userBytes = QByteArrayLiteral("NOT-A-CACHED-ARTIFACT");
  QVERIFY(writeBytes(userFile, userBytes));

  QString key;
  QString reason;
  auto lease = acquireArtifactLease(first, sample.manifest.buildKey, &key, &reason);
  QVERIFY2(lease.has_value(), qPrintable(reason));
  QVERIFY(lease->isValid());
  QVERIFY(!lease->root().isEmpty());
  QCOMPARE(lease->buildKey(), sample.manifest.buildKey);

  // A copy shares the one claim, and the claim is visible from a second store
  // instance over the same root.
  ArtifactLease copy = *lease;
  QVERIFY(copy.isValid());
  QVERIFY(!removeArtifact(second, sample.manifest.buildKey, &key, &reason));
  QCOMPARE(key, QStringLiteral("native.artifact.inUse"));
  QVERIFY(QFileInfo::exists(record->enginePath));

  lease.reset();
  QVERIFY(!removeArtifact(second, sample.manifest.buildKey, &key, &reason));
  QCOMPARE(key, QStringLiteral("native.artifact.inUse"));

  ArtifactLease moved = std::move(copy);
  QVERIFY(moved.isValid());
  QVERIFY(!copy.isValid());
  QVERIFY(!removeArtifact(second, sample.manifest.buildKey, &key, &reason));
  QCOMPARE(key, QStringLiteral("native.artifact.inUse"));

  // The final release permits removing exactly the artifact that was proved.
  moved = ArtifactLease();
  QVERIFY(!moved.isValid());
  QVERIFY2(removeArtifact(second, sample.manifest.buildKey, &key, &reason), qPrintable(reason));
  QVERIFY(!QFileInfo::exists(record->enginePath));
  QVERIFY(!QFileInfo::exists(record->manifestPath));
  QCOMPARE(readBytes(userFile), userBytes);
}

void TestNativeManifest::aLeaseOnOneArtifactLeavesAnotherRemovable() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample held = makeSample(buildFor(QStringLiteral("616.92")));
  const Sample other = makeSample(buildFor(QStringLiteral("617.00")));
  QVERIFY(held.manifest.buildKey != other.manifest.buildKey);
  QVERIFY(publishSample(store, held, temp.path()).has_value());
  QVERIFY(publishSample(store, other, temp.path()).has_value());

  QString key;
  QString reason;
  const auto lease = acquireArtifactLease(store, held.manifest.buildKey, &key, &reason);
  QVERIFY2(lease.has_value(), qPrintable(reason));

  // The claim covers one (root, key) pair only.
  QVERIFY2(removeArtifact(store, other.manifest.buildKey, &key, &reason), qPrintable(reason));
  QVERIFY(!removeArtifact(store, held.manifest.buildKey, &key, &reason));
  QCOMPARE(key, QStringLiteral("native.artifact.inUse"));
}

// --------------------------------------------- identity agreement (hostile)

void TestNativeManifest::declaredProfileAndBuildKeyAgree() {
  const QByteArray graphBytes = QByteArrayLiteral("graph-with-a-declared-boundary");
  const BuildRequest baseline = requestFor(graphBytes, buildFor());
  BuildRequest other = baseline;
  // Same bytes, a different declared output boundary: the content id and the
  // build key must both move, because both commit to what the graph declares.
  other.graph.declaredProfile.output.height = 1088;
  QVERIFY(baseline.graph.contentId() != other.graph.contentId());
  QVERIFY(baseline.buildKey() != other.buildKey());

  // An identity with no declared boundary is incomplete, not merely different.
  GraphContentIdentity bare;
  bare.role = ModelRole::Spatial;
  bare.graphSha256 = sha256Hex(graphBytes);
  QVERIFY(!bare.isComplete());
  QVERIFY(baseline.graph.isComplete());

  // A manifest whose recorded boundary disagrees with its recorded content id is
  // refused rather than served under a key describing a different graph.
  Sample sample = makeSample();
  sample.manifest.source.declaredProfile.output.height = 1234;
  ArtifactManifest parsed;
  QString key;
  QString reason;
  QVERIFY(!parseArtifactManifest(serializeArtifactManifest(sample.manifest), &parsed, &key,
                                 &reason));
  QCOMPARE(key, QStringLiteral("manifest.source.modelContentId"));

  // A manifest with no populated boundary cannot be checked at all.
  sample = makeSample();
  sample.manifest.source.declaredProfile = ModelProfile();
  QVERIFY(!parseArtifactManifest(serializeArtifactManifest(sample.manifest), &parsed, &key,
                                 &reason));
  QCOMPARE(key, QStringLiteral("manifest.source.profile"));
}

void TestNativeManifest::aManifestNameCollisionIsRefused() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  Sample sample = makeSample();
  sample.manifest.engine.file = ArtifactStore::artifactManifestFileName();

  QString key;
  QString reason;
  QVERIFY(!publishProduced(store, sample.manifest, sample.engineBytes, temp.path(), &key, &reason)
               .has_value());
  QCOMPARE(key, QStringLiteral("publish.engineName"));
  // A refusal leaves no staging directory and publishes nothing.
  QVERIFY(stagingDirectories(store).isEmpty());

  // A nested engine name is decided explicitly: refused, because it would be
  // removed separately from its manifest and leave a directory behind.
  Sample nested = makeSample();
  nested.manifest.engine.file = QStringLiteral("sub/engine.plan");
  ArtifactManifest parsed;
  QVERIFY(!parseArtifactManifest(serializeArtifactManifest(nested.manifest), &parsed, &key,
                                 &reason));
  QCOMPARE(key, QStringLiteral("manifest.enginePath"));
}

void TestNativeManifest::aCopiedManifestUnderAnotherKeyIsRefused() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample sample = makeSample();
  QVERIFY(publishSample(store, sample, temp.path()).has_value());

  const QString otherKey = QStringLiteral("sha256:") + sha256Hex(QByteArrayLiteral("elsewhere"));
  const QString copied =
      QDir(temp.path()).filePath(ArtifactStore::artifactDirectoryName(otherKey));
  QVERIFY(QDir().rename(
      QDir(temp.path()).filePath(ArtifactStore::artifactDirectoryName(sample.manifest.buildKey)),
      copied));

  QString key;
  QString reason;
  QVERIFY(!readArtifact(store, otherKey, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("artifact.buildKeyMismatch"));
}

void TestNativeManifest::anOnDiskManifestEditIsCaught() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample sample = makeSample();
  const auto record = publishSample(store, sample, temp.path());
  QVERIFY(record.has_value());

  // Move only members outside the build key, keeping the document itself
  // self-consistent, so the record-versus-disk comparison is the only check that
  // can catch the edit.
  ArtifactManifest edited = record->manifest;
  edited.engineSha256 = QString(64, QLatin1Char('b'));
  edited.engine.sha256 = edited.engineSha256;
  QVERIFY(writeBytes(record->manifestPath, serializeArtifactManifest(edited)));

  QString key;
  QString reason;
  QVERIFY(!verifyArtifact(store, *record, &key, &reason));
  QCOMPARE(key, QStringLiteral("artifact.manifestChanged"));
}

void TestNativeManifest::anEngineShaMismatchIsRefused() {
  QJsonObject object = artifactManifestToJson(makeSample().manifest);
  object.insert(QStringLiteral("engineSha256"), QString(64, QLatin1Char('c')));
  ArtifactManifest parsed;
  QString key;
  QString reason;
  QVERIFY(!parseArtifactManifest(object, &parsed, &key, &reason));
  QCOMPARE(key, QStringLiteral("manifest.engineShaMismatch"));
}

void TestNativeManifest::aNonCanonicalOptionStringIsRefused() {
  QJsonObject object = artifactManifestToJson(makeSample().manifest);
  QJsonObject build = object.value(QStringLiteral("build")).toObject();
  // An unordered tacticSources set in its unsorted spelling: a hand-edit that
  // would otherwise quietly name a different request than the key it carries.
  build.insert(QStringLiteral("options"),
               QStringLiteral("--tacticSources=-CUDNN,-CUBLAS,-CUBLAS_LT"));
  object.insert(QStringLiteral("build"), build);
  ArtifactManifest parsed;
  QString key;
  QString reason;
  QVERIFY(!parseArtifactManifest(object, &parsed, &key, &reason));
  QCOMPARE(key, QStringLiteral("manifest.build.options"));
}

void TestNativeManifest::everyRuntimeFieldDisagreementIsNamed() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample sample = makeSample();
  const auto record = publishSample(store, sample, temp.path());
  QVERIFY(record.has_value());

  using Mutation = void (*)(EngineBuildIdentity&);
  const QVector<std::pair<QString, Mutation>> cases = {
      {QStringLiteral("runtime.provider"),
       [](EngineBuildIdentity& b) {
         b.provider = Backend::DirectML;
         b.adapterLuid = QStringLiteral("LUID-0001");
       }},
      {QStringLiteral("runtime.runtime"),
       [](EngineBuildIdentity& b) { b.runtimeVersion = QStringLiteral("11.4.0.1"); }},
      {QStringLiteral("runtime.plugin"),
       [](EngineBuildIdentity& b) { b.pluginVersion = QStringLiteral("9.9.9"); }},
      {QStringLiteral("runtime.coreSha"),
       [](EngineBuildIdentity& b) { b.coreSha = QString(40, QLatin1Char('a')); }},
      {QStringLiteral("runtime.sourceAbi"), [](EngineBuildIdentity& b) { b.sourceAbi = 9; }},
      {QStringLiteral("runtime.driver"),
       [](EngineBuildIdentity& b) { b.driverVersion = QStringLiteral("617.00"); }},
      {QStringLiteral("runtime.cuda"),
       [](EngineBuildIdentity& b) { b.cudaVersion = QStringLiteral("14.0.1"); }},
      {QStringLiteral("runtime.uuid"),
       [](EngineBuildIdentity& b) { b.deviceUuid = QStringLiteral("GPU-other"); }},
      {QStringLiteral("runtime.sm"), [](EngineBuildIdentity& b) { b.smMajor = 13; }},
      {QStringLiteral("runtime.luid"),
       [](EngineBuildIdentity& b) { b.adapterLuid = QStringLiteral("LUID-9f"); }},
      {QStringLiteral("runtime.precision"),
       [](EngineBuildIdentity& b) { b.precision = EnginePrecision::Fp32; }},
      {QStringLiteral("runtime.tf32"), [](EngineBuildIdentity& b) { b.tf32 = true; }},
      {QStringLiteral("runtime.options"),
       [](EngineBuildIdentity& b) {
         b.options.append(
             BuildOption{QStringLiteral("optShapes"), QStringLiteral("1x3x512x512"), true});
       }},
  };

  for (const auto& one : cases) {
    EngineBuildIdentity observed = sample.request.build;
    one.second(observed);
    QString key;
    QString reason;
    QVERIFY2(!verifyArtifactRuntime(*record, observed, &key, &reason), qPrintable(one.first));
    QCOMPARE(key, one.first);
  }

  // A credential-looking option value is refused before it can be compared or
  // echoed, not reported as a mere disagreement.
  EngineBuildIdentity hostile = sample.request.build;
  hostile.options.append(BuildOption{QStringLiteral("downloadUrl"),
                                     QStringLiteral("https://build.example.invalid/x?token=abc"),
                                     true});
  QString key;
  QString reason;
  QVERIFY(!verifyArtifactRuntime(*record, hostile, &key, &reason));
  QCOMPARE(key, QStringLiteral("runtime.unobserved"));
  QVERIFY(!reason.contains(QStringLiteral("build.example.invalid")));
}

void TestNativeManifest::aDirectMlLuidBuildVerifiesAndInvalidates() {
  EngineBuildIdentity directml = buildFor();
  directml.provider = Backend::DirectML;
  directml.adapterLuid = QStringLiteral("LUID-0x1a2b3c");
  QCOMPARE(directml.firstMissingRequiredField(), QString());

  const Sample sample = makeSample(directml);
  QVERIFY(sample.manifest.isSelfConsistent());
  QVERIFY(sample.manifest.device.luid == directml.adapterLuid);

  ArtifactManifest parsed;
  QString key;
  QString reason;
  QVERIFY2(parseArtifactManifest(serializeArtifactManifest(sample.manifest), &parsed, &key, &reason),
           qPrintable(reason));
  QCOMPARE(parsed.buildIdentity().adapterLuid, directml.adapterLuid);

  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const auto record = publishSample(store, sample, temp.path());
  QVERIFY(record.has_value());
  QVERIFY2(verifyArtifactRuntime(*record, directml, &key, &reason), qPrintable(reason));

  EngineBuildIdentity changed = directml;
  changed.adapterLuid = QStringLiteral("LUID-other");
  QVERIFY(!verifyArtifactRuntime(*record, changed, &key, &reason));
  QCOMPARE(key, QStringLiteral("runtime.luid"));

  // Dropping the LUID leaves the DirectML identity incomplete, not silently
  // comparable.
  EngineBuildIdentity missing = directml;
  missing.adapterLuid.clear();
  QCOMPARE(missing.firstMissingRequiredField(), QStringLiteral("adapterLuid"));
}

// ------------------------------------------------------- ownership (hostile)

void TestNativeManifest::aStoreCannotBePointedAtAModelDirectory() {
  const QString modelsRoot = nativeSubdirectory(QStringLiteral("models"));
  if (modelsRoot.isEmpty() || !QDir().mkpath(modelsRoot)) {
    QSKIP("this machine has no app-owned model directory to prove against");
  }
  const QString modelDir = QDir(modelsRoot).filePath(
      QStringLiteral("unit-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  QVERIFY(QDir().mkpath(modelDir));
  const QString sentinelPath = QDir(modelDir).filePath(QStringLiteral("keep-me.bin"));
  const QByteArray sentinel = QByteArrayLiteral("MODEL-SENTINEL-DO-NOT-TOUCH");
  QVERIFY(writeBytes(sentinelPath, sentinel));

  const ArtifactStore store(modelDir);
  QString key;
  QString reason;
  QVERIFY(!store.isOwned(&reason));
  QVERIFY(!reason.isEmpty());
  QVERIFY(!store.ensureOwned(&reason));

  QTemporaryDir scratch;
  const Sample sample = makeSample();
  QVERIFY(!publishProduced(store, sample.manifest, sample.engineBytes, scratch.path(), &key,
                           &reason)
               .has_value());
  QCOMPARE(key, QStringLiteral("store.unowned"));
  QVERIFY(!readArtifact(store, sample.manifest.buildKey, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("store.unowned"));
  QVERIFY(!removeArtifact(store, sample.manifest.buildKey, &key, &reason));
  QCOMPARE(key, QStringLiteral("store.unowned"));
  QVERIFY(!discardStaging(store, QStringLiteral(".staging-foreign"), &key, &reason));
  QCOMPARE(key, QStringLiteral("store.unowned"));
  QVERIFY(stagingDirectories(store).isEmpty());

  // Nothing in the model directory moved - not one byte, not one entry.
  QCOMPARE(readBytes(sentinelPath), sentinel);
  QVERIFY(QFileInfo(modelDir).isDir());
  QVERIFY(QDir(modelDir).removeRecursively());
}

void TestNativeManifest::everyEntryPointRefusesAnUnownedRoot() {
  QTemporaryDir temp;
  // A relative root can never be shown to be inside an owned location.
  const ArtifactStore store(QStringLiteral("relative/not-owned"));
  const Sample sample = makeSample();

  QString key;
  QString reason;
  QVERIFY(!store.isOwned(&reason));
  QVERIFY(!reason.isEmpty());
  QVERIFY(!store.ensureOwned(&reason));

  QVERIFY(!publishProduced(store, sample.manifest, sample.engineBytes, temp.path(), &key, &reason)
               .has_value());
  QCOMPARE(key, QStringLiteral("store.unowned"));
  QVERIFY(!readArtifact(store, sample.manifest.buildKey, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("store.unowned"));
  QVERIFY(!findArtifactForRequest(store, sample.request, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("store.unowned"));
  QVERIFY(!acquireArtifactLease(store, sample.manifest.buildKey, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("store.unowned"));
  QVERIFY(!removeArtifact(store, sample.manifest.buildKey, &key, &reason));
  QCOMPARE(key, QStringLiteral("store.unowned"));
  QVERIFY(!discardStaging(store, QStringLiteral(".staging-x"), &key, &reason));
  QCOMPARE(key, QStringLiteral("store.unowned"));

  ArtifactRecord bogus;
  bogus.root = temp.path();
  bogus.manifestPath = QDir(temp.path()).filePath(ArtifactStore::artifactManifestFileName());
  bogus.manifest = sample.manifest;
  QVERIFY(!verifyArtifact(store, bogus, &key, &reason));
  QCOMPARE(key, QStringLiteral("store.unowned"));
  QVERIFY(stagingDirectories(store).isEmpty());
}

// ---------------------------------------------------- hostile filesystem

void TestNativeManifest::aSymlinkOrNonRegularEngineIsRefused() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample sample = makeSample();
  const auto record = publishSample(store, sample, temp.path());
  QVERIFY(record.has_value());

  // The engine's name resolves to a directory, not the file its manifest names.
  QVERIFY(QFile::remove(record->enginePath));
  QVERIFY(QDir().mkpath(record->enginePath));
  QString key;
  QString reason;
  QVERIFY(!readArtifact(store, sample.manifest.buildKey, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("artifact.notRegular"));
  QVERIFY(QDir(record->enginePath).removeRecursively());

#ifdef Q_OS_WIN
  // A junction in the engine's place is refused, never followed.
  const QString outside = QDir(temp.path()).filePath(QStringLiteral("outside"));
  QVERIFY(QDir().mkpath(outside));
  QVERIFY(writeBytes(QDir(outside).filePath(QStringLiteral("engine.plan")), sample.engineBytes));
  QProcess mklink;
  mklink.start(QStringLiteral("cmd.exe"),
               {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                QDir::toNativeSeparators(record->enginePath), QDir::toNativeSeparators(outside)});
  if (!mklink.waitForFinished(10000) || mklink.exitCode() != 0) {
    QSKIP("this machine cannot create a junction without elevation");
  }
  QVERIFY(!readArtifact(store, sample.manifest.buildKey, &key, &reason).has_value());
  QVERIFY(key == QStringLiteral("artifact.symlink") || key == QStringLiteral("artifact.reparse"));
#endif
}

void TestNativeManifest::aNestedOrUnknownManifestMemberIsRefused() {
  ArtifactManifest parsed;
  QString key;
  QString reason;
  {
    QJsonObject object = artifactManifestToJson(makeSample().manifest);
    QJsonObject source = object.value(QStringLiteral("source")).toObject();
    source.insert(QStringLiteral("surprise"), QStringLiteral("1"));
    object.insert(QStringLiteral("source"), source);
    QVERIFY(!parseArtifactManifest(object, &parsed, &key, &reason));
    QCOMPARE(key, QStringLiteral("manifest.source.unknownMember"));
  }
  {
    QJsonObject object = artifactManifestToJson(makeSample().manifest);
    QJsonObject source = object.value(QStringLiteral("source")).toObject();
    QJsonObject profile = source.value(QStringLiteral("profile")).toObject();
    profile.insert(QStringLiteral("surprise"), QStringLiteral("1"));
    source.insert(QStringLiteral("profile"), profile);
    object.insert(QStringLiteral("source"), source);
    QVERIFY(!parseArtifactManifest(object, &parsed, &key, &reason));
    QCOMPARE(key, QStringLiteral("manifest.source.member"));
  }
  {
    QJsonObject object = artifactManifestToJson(makeSample().manifest);
    QJsonObject stage = object.value(QStringLiteral("stage")).toObject();
    stage.remove(QStringLiteral("io"));
    object.insert(QStringLiteral("stage"), stage);
    QVERIFY(!parseArtifactManifest(object, &parsed, &key, &reason));
    QCOMPARE(key, QStringLiteral("manifest.stage.member"));
  }
  {
    QJsonObject object = artifactManifestToJson(makeSample().manifest);
    QJsonObject engine = object.value(QStringLiteral("engine")).toObject();
    engine.remove(QStringLiteral("bytes"));
    object.insert(QStringLiteral("engine"), engine);
    QVERIFY(!parseArtifactManifest(object, &parsed, &key, &reason));
    QCOMPARE(key, QStringLiteral("manifest.engine.bytes"));
  }
}

void TestNativeManifest::publishRefusesAMismatchedOrExistingArtifact() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  const Sample sample = makeSample();

  QString key;
  QString reason;
  // The produced bytes are not the bytes the manifest describes.
  QByteArray different = sample.engineBytes;
  different[0] = char(different.at(0) ^ 0x01);
  QVERIFY(!publishProduced(store, sample.manifest, different, temp.path(), &key, &reason)
               .has_value());
  QCOMPARE(key, QStringLiteral("publish.engineMismatch"));
  QVERIFY(stagingDirectories(store).isEmpty());

  // An existing directory under the key that does not verify blocks a fresh
  // publish instead of being replaced.
  const QString directory =
      QDir(temp.path()).filePath(ArtifactStore::artifactDirectoryName(sample.manifest.buildKey));
  QVERIFY(QDir().mkpath(directory));
  QVERIFY(writeBytes(QDir(directory).filePath(ArtifactStore::artifactManifestFileName()),
                     QByteArrayLiteral("not-a-manifest")));
  QVERIFY(!publishProduced(store, sample.manifest, sample.engineBytes, temp.path(), &key, &reason)
               .has_value());
  QCOMPARE(key, QStringLiteral("publish.exists"));
}

void TestNativeManifest::discardStagingRefusesForeignNamesAndTrees() {
  QTemporaryDir temp;
  const ArtifactStore store(temp.path());
  QString key;
  QString reason;

  QVERIFY(!discardStaging(store, QStringLiteral("not-staging"), &key, &reason));
  QCOMPARE(key, QStringLiteral("artifact.notStaging"));
  // A ".." segment is refused by the path policy before containment is even
  // considered, so the refusal is a name refusal.
  QVERIFY(!discardStaging(store, QStringLiteral(".staging-x/.."), &key, &reason));
  QCOMPARE(key, QStringLiteral("artifact.notStaging"));
  QVERIFY(!discardStaging(store, QStringLiteral(".staging-absent"), &key, &reason));
  QCOMPARE(key, QStringLiteral("artifact.missing"));

  // A staging directory holding a subdirectory is not provably ours and is left
  // entirely alone rather than partially removed.
  const QString staging = QDir(temp.path()).filePath(QStringLiteral(".staging-tree"));
  QVERIFY(QDir().mkpath(QDir(staging).filePath(QStringLiteral("sub"))));
  QVERIFY(!discardStaging(store, QStringLiteral(".staging-tree"), &key, &reason));
  QCOMPARE(key, QStringLiteral("artifact.unproven"));
  QVERIFY(QFileInfo(staging).isDir());
  QVERIFY(QFileInfo(QDir(staging).filePath(QStringLiteral("sub"))).isDir());
}

void TestNativeManifest::pathWithinRootFollowsPlatformCaseRules() {
  const QString root = QStringLiteral("C:/Root/Dir");
  QVERIFY(pathWithinRoot(root, root));
  QVERIFY(pathWithinRoot(root, QStringLiteral("C:/Root/Dir/engine.plan")));
  QVERIFY(!pathWithinRoot(root, QStringLiteral("C:/Root/Other")));
  QVERIFY(!pathWithinRoot(root, QStringLiteral("C:/Root/DirX")));
  const bool insensitive = pathCaseSensitivity() == Qt::CaseInsensitive;
  QCOMPARE(pathWithinRoot(root, QStringLiteral("c:/root/dir/engine.plan")), insensitive);
}
