// The owned store's consumer-visible behaviour, exercised without the inspector.
//
// The point of the split is that this file links and runs with the ONNX Runtime
// inspector off: it publishes real bytes into a real owned store, reads the rows
// back from disk, and assembles them into the catalogue a pure consumer plans
// from. No runtime is loaded, no inference is run, and nothing is mocked: the
// store's own documents and bytes are the evidence. The import entry points that
// need the inspector are a different translation unit and are not named here.

#include "TestNativeStoredModels.hpp"

#include "native/NativeModelInventory.hpp"
#include "native/NativePaths.hpp"
#include "native/NativePreparationPlan.hpp"
#include "native/NativeRecipe.hpp"

#include <optional>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <QtCore/QVector>

using namespace mplayer::native;

namespace {

/// An isolated store root under the app-owned models tree, removed afterwards.
/// It is named inside the tree but created by the first publish, exactly as the
/// default root is.
class StoreFixture {
 public:
  StoreFixture() {
    QString reason;
    const QString modelsRoot = nativeSubdirectory(QStringLiteral("models"), &reason);
    if (modelsRoot.isEmpty() || !QDir().mkpath(modelsRoot)) return;
    root_ = QDir(modelsRoot).filePath(
        QStringLiteral("unit-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  }
  ~StoreFixture() {
    if (!root_.isEmpty()) QDir(root_).removeRecursively();
  }
  bool isValid() const { return !root_.isEmpty(); }
  QString root() const { return root_; }

 private:
  QString root_;
};

TensorProfile tensor(const QString& dtype, qint64 batch, qint64 channels,
                     std::optional<qint64> height, std::optional<qint64> width) {
  TensorProfile profile;
  profile.dtype = dtype;
  profile.rank = 4;
  profile.batch = batch;
  profile.channels = channels;
  profile.height = height;
  profile.width = width;
  return profile;
}

/// A fully declared spatial boundary. `width`/`height` are the geometry QSize
/// uses, so a caller reads the same way the planner does.
ModelProfile fixedProfile(const QString& dtype, int inWidth, int inHeight, int outWidth,
                          int outHeight) {
  ModelProfile profile;
  profile.input = tensor(dtype, 1, 3, inHeight, inWidth);
  profile.output = tensor(dtype, 1, 3, outHeight, outWidth);
  return profile;
}

/// A dynamic RIFE boundary: 1xC11 in, 1xC3 out, H/W left to the run.
ModelProfile dynamicTemporalProfile() {
  ModelProfile profile;
  profile.input = tensor(QString::fromLatin1(kEligibleIoType), 1, 11, std::nullopt, std::nullopt);
  profile.output = tensor(QString::fromLatin1(kEligibleIoType), 1, 3, std::nullopt, std::nullopt);
  return profile;
}

/// The concrete boundary a CPU run produced, kept beside the declaration.
ModelProfile temporalProof(int width, int height) {
  ModelProfile proof;
  proof.input = tensor(QString::fromLatin1(kEligibleIoType), 1, 11, height, width);
  proof.output = tensor(QString::fromLatin1(kEligibleIoType), 1, 3, height, width);
  return proof;
}

/// A successful declared read, exactly the shape `publish` consumes: the store
/// publishes bytes that were already inspected and never inspects them itself.
InspectionResult inspected(const ModelProfile& declared) {
  InspectionResult result;
  result.ok = true;
  result.inputName = QStringLiteral("X");
  result.outputName = QStringLiteral("Y");
  result.declared = declared;
  return result;
}

bool writeFile(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  return file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return QByteArray();
  return file.readAll();
}

/// "user:<token>" -> "<token>", the directory the content digest names.
QString tokenOf(const QString& modelId) {
  if (!modelId.startsWith(QLatin1String("user:"))) return QString();
  return modelId.mid(static_cast<int>(sizeof("user:") - 1));
}

const StoredModel* rowWithReasonKey(const QVector<StoredModel>& rows, const QString& key) {
  for (const StoredModel& row : rows) {
    if (row.record.availability.reasonKey == key) return &row;
  }
  return nullptr;
}

SourceGeometry hdSource() {
  SourceGeometry source;
  source.codedSize = QSize(1920, 1080);
  source.rotationDegrees = 0;
  source.fps = Rational{24, 1};
  return source;
}

DeviceIdentity knownDevice() {
  DeviceIdentity device;
  device.decoderDeviceUuid = QStringLiteral("GPU-decode-0000");
  device.computeDeviceUuid = QStringLiteral("GPU-compute-0001");
  device.smMajor = 12;
  device.smMinor = 0;
  return device;
}

}  // namespace

void TestNativeStoredModels::aPublishedOwnedRowReadsBackAndVerifiesWithoutRuntime() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  const QByteArray bytes = QByteArrayLiteral("mikai-stored-fixture-fixed-a");
  const ModelProfile profile = fixedProfile(QString::fromLatin1(kEligibleIoType), 64, 64, 128, 128);
  BackendAvailability backends;
  backends.available = {Backend::TensorRT};

  const ModelImportOutcome outcome =
      inventory.publish(bytes, inspected(profile), QStringLiteral("C:/imports/fixed-a.onnx"),
                        QStringLiteral("fixed-a"), backends);
  QVERIFY2(outcome.ok, qPrintable(outcome.reasonKey));
  QVERIFY(outcome.published);
  QVERIFY(outcome.model.record.availability.isUsable());
  QVERIFY(outcome.model.record.id.startsWith(QLatin1String("user:")));
  QCOMPARE(outcome.model.record.onnxSha256, sha256HexOf(bytes));
  QCOMPARE(outcome.model.record.onnxBytes, static_cast<qint64>(bytes.size()));
  QVERIFY(outcome.model.record.supportsBackend(Backend::TensorRT));

  QString reason;
  const QVector<StoredModel> rows = inventory.stored(&reason);
  QVERIFY2(reason.isEmpty(), qPrintable(reason));
  QCOMPARE(rows.size(), 1);
  const StoredModel& row = rows.first();
  QCOMPARE(row.record.id, outcome.model.record.id);
  QCOMPARE(row.record.onnxSha256, sha256HexOf(bytes));
  QCOMPARE(row.record.onnxBytes, static_cast<qint64>(bytes.size()));
  QCOMPARE(row.sourcePath, QStringLiteral("C:/imports/fixed-a.onnx"));
  QVERIFY(row.record.availability.isUsable());
  QVERIFY(row.record.isOffered());
  // A fully declared, strictly eligible graph has nothing for a run to settle.
  QVERIFY(!row.importResolution.has_value());
  QVERIFY(!row.record.verifiedReferenceProfile.has_value());
  QVERIFY(QFileInfo::exists(row.storedPath));
  QCOMPARE(QFileInfo(row.storedPath).dir().dirName().compare(tokenOf(row.record.id),
                                                            Qt::CaseInsensitive),
           0);
  QVERIFY(!QDir(fixture.root()).relativeFilePath(row.storedPath).startsWith(QLatin1String("..")));

  QString key;
  QString integrityReason;
  QVERIFY2(inventory.verifyOwnedModelIntegrity(row, &key, &integrityReason),
           qPrintable(integrityReason));
  QVERIFY(key.isEmpty());
}

void TestNativeStoredModels::anUnsupportedDtypeRowIsRecordedAndReadBackUnavailable() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  const QByteArray bytes = QByteArrayLiteral("mikai-stored-fixture-fp32-b");
  const ModelProfile profile = fixedProfile(QStringLiteral("fp32"), 64, 64, 128, 128);

  const ModelImportOutcome outcome =
      inventory.publish(bytes, inspected(profile), QStringLiteral("C:/imports/fp32-b.onnx"),
                        QStringLiteral("fp32-b"));
  // The row is published - seeing it is the point - but it is not usable, and the
  // reason names the dtype the graph actually declared.
  QVERIFY(!outcome.ok);
  QVERIFY(outcome.published);
  QCOMPARE(outcome.model.record.availability.reasonKey, QStringLiteral("model.io.unsupported"));
  QVERIFY(outcome.model.record.availability.reason.contains(QStringLiteral("fp32")));

  const QVector<StoredModel> rows = inventory.stored();
  QCOMPARE(rows.size(), 1);
  const StoredModel& row = rows.first();
  QCOMPARE(row.record.availability.reasonKey, QStringLiteral("model.io.unsupported"));
  QVERIFY(row.record.availability.reason.contains(QStringLiteral("fp32")));
  QVERIFY(!row.record.availability.isUsable());
  // The declaration itself is still refused by the boundary rule, so a consumer
  // that tries to offer the row is told why rather than handed a usable model.
  QVERIFY(!row.record.isOffered());
  QString offerKey;
  QString offerReason;
  row.record.refusal(&offerKey, &offerReason);
  QCOMPARE(offerKey, QStringLiteral("model.profile.ineligible"));

  // Recording it did not convert, re-export or execute it: the bytes are intact.
  QString key;
  QString integrityReason;
  QVERIFY2(inventory.verifyOwnedModelIntegrity(row, &key, &integrityReason),
           qPrintable(integrityReason));
}

void TestNativeStoredModels::aTemporalRowIsOfferedOnlyWithItsRecordedProof() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  const ModelProfile declared = dynamicTemporalProfile();
  const ModelProfile proof = temporalProof(64, 64);

  BackendAvailability tensorRt;
  tensorRt.available = {Backend::TensorRT};
  BackendAvailability directMl;
  directMl.available = {Backend::DirectML};

  // With the CPU run's concrete boundary beside the declaration, the row is Ready
  // and keeps the proof it was published under.
  InspectionResult proved = inspected(declared);
  proved.referenceRan = true;
  proved.runGeometry = QSize(64, 64);
  proved.resolvedGeometry = QSize(64, 64);
  proved.resolvedScale = 1;
  proved.verifiedReferenceProfile = proof;
  const ModelImportOutcome provedOutcome = inventory.publish(
      QByteArrayLiteral("mikai-stored-fixture-rife-proved"), proved,
      QStringLiteral("C:/imports/rife-proved.onnx"), QStringLiteral("rife-proved"), tensorRt,
      QString(), ModelRole::Temporal);
  QVERIFY2(provedOutcome.ok, qPrintable(provedOutcome.reasonKey));
  QVERIFY(provedOutcome.published);
  QVERIFY(provedOutcome.model.importResolution.has_value());
  QVERIFY(provedOutcome.model.importResolution->fromCpuReference);
  QCOMPARE(provedOutcome.model.importResolution->input, QSize(64, 64));
  QCOMPARE(provedOutcome.model.importResolution->output, QSize(64, 64));
  QCOMPARE(provedOutcome.model.importResolution->scale, 1);

  // Without a run, the same declaration is recorded but not offered: no
  // declaration vouches for a dynamic boundary on its own.
  const ModelImportOutcome unprovedOutcome = inventory.publish(
      QByteArrayLiteral("mikai-stored-fixture-rife-unproved"), inspected(declared),
      QStringLiteral("C:/imports/rife-unproved.onnx"), QStringLiteral("rife-unproved"), tensorRt,
      QString(), ModelRole::Temporal);
  QVERIFY(!unprovedOutcome.ok);
  QVERIFY(unprovedOutcome.published);
  QCOMPARE(unprovedOutcome.model.record.availability.reasonKey,
           QStringLiteral("model.reference.needed"));

  // A provisioner that cannot run the boundary leaves the row unavailable rather
  // than selectable, even though the run itself succeeded.
  const ModelImportOutcome directMlOutcome = inventory.publish(
      QByteArrayLiteral("mikai-stored-fixture-rife-directml"), proved,
      QStringLiteral("C:/imports/rife-directml.onnx"), QStringLiteral("rife-directml"), directMl,
      QString(), ModelRole::Temporal);
  QVERIFY(!directMlOutcome.ok);
  QVERIFY(directMlOutcome.published);
  QCOMPARE(directMlOutcome.model.record.availability.reasonKey,
           QStringLiteral("model.backend.unsupported"));
  QVERIFY(directMlOutcome.model.record.availableBackends.isEmpty());

  const QVector<StoredModel> rows = inventory.stored();
  QCOMPARE(rows.size(), 3);
  const StoredModel* provedRow = rowWithReasonKey(rows, QString());
  QVERIFY(provedRow != nullptr);
  QVERIFY(provedRow->record.availability.isUsable());
  QCOMPARE(provedRow->record.role, ModelRole::Temporal);
  QVERIFY(provedRow->record.isOffered());
  QVERIFY(provedRow->importResolution.has_value());
  QVERIFY(provedRow->importResolution->fromCpuReference);
  QVERIFY(provedRow->record.verifiedReferenceProfile.has_value());
  QVERIFY(provedRow->record.verifiedReferenceProfile->input.height.has_value());
  QCOMPARE(*provedRow->record.verifiedReferenceProfile->input.height, qint64(64));
  QVERIFY(provedRow->record.supportsBackend(Backend::TensorRT));

  const StoredModel* needed = rowWithReasonKey(rows, QStringLiteral("model.reference.needed"));
  QVERIFY(needed != nullptr);
  QVERIFY(!needed->record.availability.isUsable());
  QVERIFY(!needed->importResolution.has_value());

  const StoredModel* unsupported = rowWithReasonKey(rows, QStringLiteral("model.backend.unsupported"));
  QVERIFY(unsupported != nullptr);
  QVERIFY(!unsupported->record.availability.isUsable());
  QVERIFY(unsupported->record.availableBackends.isEmpty());
}

void TestNativeStoredModels::hostileCopyAndManifestTransitionsAreReportedNotDropped() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  const ModelProfile profile = fixedProfile(QString::fromLatin1(kEligibleIoType), 64, 64, 128, 128);
  const QByteArray bytes = QByteArrayLiteral("mikai-stored-fixture-hostile-copy");
  const ModelImportOutcome published =
      inventory.publish(bytes, inspected(profile), QStringLiteral("C:/imports/hostile.onnx"),
                        QStringLiteral("hostile"));
  QVERIFY2(published.ok, qPrintable(published.reasonKey));
  const QString publishedId = published.model.record.id;

  // A directory whose name is not a content digest is not one of ours, so it is
  // simply not a row.
  QVERIFY(QDir().mkpath(QDir(fixture.root()).filePath(QStringLiteral("not-a-content-token"))));

  // A mutated owned copy is reported as an unavailable row, not dropped and not
  // deleted.
  QVERIFY(writeFile(published.model.storedPath,
                    QByteArrayLiteral("mikai-stored-fixture-hostile-copy-mutated")));
  QString reason;
  QVector<StoredModel> rows = inventory.stored(&reason);
  QVERIFY2(reason.isEmpty(), qPrintable(reason));
  QCOMPARE(rows.size(), 1);
  QCOMPARE(rows.first().record.availability.reasonKey, QStringLiteral("model.store.digest"));
  QVERIFY(!rows.first().record.availability.isUsable());
  QVERIFY(QFileInfo::exists(rows.first().storedPath));

  QString key;
  QString integrityReason;
  QVERIFY(!inventory.verifyOwnedModelIntegrity(rows.first(), &key, &integrityReason));
  QCOMPARE(key, QStringLiteral("model.owned.changed"));
  QVERIFY(!integrityReason.isEmpty());

  // A second, intact row whose manifest identity is edited is refused as that row
  // rather than read as another one.
  const QByteArray secondBytes = QByteArrayLiteral("mikai-stored-fixture-hostile-manifest");
  const ModelImportOutcome second =
      inventory.publish(secondBytes, inspected(profile), QStringLiteral("C:/imports/manifest.onnx"),
                        QStringLiteral("manifest"));
  QVERIFY2(second.ok, qPrintable(second.reasonKey));
  const QString secondId = second.model.record.id;
  const QString manifestPath =
      QDir(QFileInfo(second.model.storedPath).absolutePath())
          .filePath(ModelInventory::manifestFileName());
  QJsonObject manifest = QJsonDocument::fromJson(readFile(manifestPath)).object();
  QVERIFY(!manifest.isEmpty());
  manifest.insert(QStringLiteral("id"),
                  QStringLiteral("user:0000000000000000000000000000000000000000000000000000000000000000"));
  QVERIFY(writeFile(manifestPath, QJsonDocument(manifest).toJson(QJsonDocument::Compact)));

  rows = inventory.stored(&reason);
  QVERIFY2(reason.isEmpty(), qPrintable(reason));
  // The mutated copy and the edited manifest are both still rows.
  QCOMPARE(rows.size(), 2);
  const StoredModel* edited = rowWithReasonKey(rows, QStringLiteral("model.store.identity"));
  QVERIFY(edited != nullptr);
  QVERIFY(!edited->record.availability.isUsable());
  QCOMPARE(QFileInfo(edited->storedPath).dir().dirName().compare(tokenOf(secondId), Qt::CaseInsensitive),
           0);
  // The first row is still reported too, with its own reason.
  QVERIFY(rowWithReasonKey(rows, QStringLiteral("model.store.digest")) != nullptr);
  QVERIFY(publishedId != secondId);
}

void TestNativeStoredModels::aForeignRootIsRefusedAndAFreshOwnedRootReadsEmpty() {
  QTemporaryDir foreign;
  QVERIFY(foreign.isValid());
  ModelInventory outside(foreign.path());
  QString reason;
  QVERIFY(!outside.isOwned(&reason));
  QVERIFY(!reason.isEmpty());
  QVERIFY(outside.stored(&reason).isEmpty());
  QVERIFY(!reason.isEmpty());

  // The owned root is eligible before it exists, and reads as the empty inventory
  // it is rather than a missing store.
  const QString freshRoot = QDir(ModelInventory::defaultRoot())
                                .filePath(QStringLiteral("unit-%1").arg(
                                    QUuid::createUuid().toString(QUuid::WithoutBraces)));
  QVERIFY(!QFileInfo::exists(freshRoot));
  ModelInventory fresh(freshRoot);
  QVERIFY2(fresh.isOwned(&reason), qPrintable(reason));
  reason.clear();
  QVERIFY(fresh.stored(&reason).isEmpty());
  QVERIFY2(reason.isEmpty(), qPrintable(reason));
}

void TestNativeStoredModels::ownedRowsAssembleIntoAPureCatalogAndPlanWithoutRuntime() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  const ModelProfile profile = fixedProfile(QString::fromLatin1(kEligibleIoType), 1920, 1080, 3840,
                                            2160);
  BackendAvailability backends;
  backends.available = {Backend::TensorRT};
  const ModelImportOutcome published =
      inventory.publish(QByteArrayLiteral("mikai-stored-fixture-catalog"), inspected(profile),
                        QStringLiteral("C:/imports/catalog-2x.onnx"), QStringLiteral("catalog-2x"),
                        backends);
  QVERIFY2(published.ok, qPrintable(published.reasonKey));

  // The pure assembly a consumer without the inspector performs: the store's own
  // rows become the catalogue, with the backend support the row recorded.
  CatalogView catalog;
  const QVector<StoredModel> rows = inventory.stored();
  QCOMPARE(rows.size(), 1);
  for (const StoredModel& row : rows) catalog.models.append(row.record);

  const ModelRecord* record = catalog.find(published.model.record.id);
  QVERIFY(record != nullptr);
  QVERIFY(record->availability.isUsable());
  QVERIFY(record->isOffered());
  QVERIFY(record->supportsBackend(Backend::TensorRT));
  QCOMPARE(record->contentId(), published.model.record.contentId());

  Recipe recipe;
  recipe.id = QStringLiteral("janai.pure.consumer.v1");
  recipe.label = QStringLiteral("Pure consumer 2x");
  recipe.backend = Backend::TensorRT;
  SpatialStage stage;
  stage.modelId = record->id;
  recipe.spatial = {stage};

  PlanRequest request;
  request.recipe = recipe;
  request.catalog = catalog;
  request.backends = backends;
  request.source = hdSource();
  request.device = knownDevice();
  // No dynamic resolver: the model is fully declared, so nothing needs one.

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY2(plan.isResolved(), qPrintable(plan.blockers.isEmpty()
                                             ? QStringLiteral("plan left unknowns")
                                             : plan.blockers.first().reasonKey));
  QCOMPARE(plan.stages.size(), 1);
  const ResolvedStage& resolved = plan.stages.first();
  QCOMPARE(resolved.modelId, record->id);
  QCOMPARE(resolved.modelContentId, record->contentId());
  QCOMPARE(resolved.inputGeometry, QSize(1920, 1080));
  QCOMPARE(resolved.outputGeometry, QSize(3840, 2160));
  QVERIFY(resolved.scale.has_value());
  QCOMPARE(*resolved.scale, 2);
  QVERIFY(!resolved.geometryFromResolver);
  QVERIFY(resolved.declaredInput == profile.input);
  QVERIFY(resolved.declaredOutput == profile.output);
}
