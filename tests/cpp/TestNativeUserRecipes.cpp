#include "TestNativeUserRecipes.hpp"

#include "native/NativeCatalog.hpp"
#include "native/NativePaths.hpp"
#include "native/NativePrefs.hpp"
#include "native/NativeRecipe.hpp"
#include "native/NativeUserRecipes.hpp"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSize>
#include <QtCore/QTemporaryDir>

using namespace mplayer::native;

namespace {

constexpr const char* kShaA =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kShaB =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

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

/// A user-imported spatial row, exactly as the inventory publishes one: its id
/// is the content hash, never a basename.
ModelRecord userSpatialRow(const QString& sha, const QString& display,
                           const ModelProfile& profile = upscale2x(1080, 1920)) {
  ModelRecord record;
  record.displayName = display;
  record.origin = ModelOrigin::UserImported;
  record.onnxPath = QStringLiteral("C:/in/%1.onnx").arg(display);
  record.onnxSha256 = sha;
  record.role = ModelRole::Spatial;
  record.actualProfile = profile;
  record.availability = readyAvailability();
  record.id = userModelIdFor(record.contentId());
  return record;
}

/// The document the store gives a body: the fingerprint of what runs, the
/// backend, and the suffix. `recipeBodyHash` is the store's own definition of
/// that fingerprint, so a test writes exactly the file the store would read.
QString storePath(const UserRecipeStore& store, const Recipe& recipe) {
  return QDir(store.root()).filePath(recipeBodyHash(recipe) + QStringLiteral("-") +
                                     stableName(recipe.backend) + QStringLiteral(".json"));
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  const bool ok = file.write(bytes) == bytes.size();
  file.close();
  return ok;
}

}  // namespace

void TestNativeUserRecipes::aSingleSpatialRecipeRoundTripsThroughItsOwnedDocument() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  UserRecipeStore store(QDir(temp.path()).filePath(QStringLiteral("recipes")));

  const ModelRecord model = userSpatialRow(QString::fromLatin1(kShaA),
                                           QStringLiteral("animejanai-x2"));
  QString key;
  QString reason;
  const std::optional<Recipe> added =
      store.addSpatialModel(model, Backend::TensorRT, &key, &reason);
  QVERIFY2(added.has_value(), qPrintable(reason));

  QCOMPARE(added->id, spatialRecipeIdFor(model, Backend::TensorRT));
  QCOMPARE(added->backend, Backend::TensorRT);
  QCOMPARE(added->shape(), RecipeShape::SpatialOnly);
  QCOMPARE(added->spatial.size(), 1);
  QCOMPARE(added->spatial.first().modelId, model.id);
  QVERIFY(!added->temporal.has_value());

  // One document, inside the owned root, that parses back to exactly this.
  const QString path = storePath(store, *added);
  QVERIFY2(QFileInfo::exists(path), qPrintable(path));
  QCOMPARE(QDir(store.root()).entryList(QDir::Files).size(), 1);

  QFile file(path);
  QVERIFY(file.open(QIODevice::ReadOnly));
  Recipe parsed;
  QString errorKey;
  QString error;
  QVERIFY2(parseRecipe(file.readAll(), &parsed, &errorKey, &error), qPrintable(error));
  QCOMPARE(parsed.id, added->id);

  const std::optional<Recipe> read = store.find(added->id, &key, &reason);
  QVERIFY2(read.has_value(), qPrintable(reason));
  QCOMPARE(read->id, added->id);
  QCOMPARE(read->label, added->label);
  QCOMPARE(read->backend, added->backend);
  QCOMPARE(read->spatial.first().modelId, added->spatial.first().modelId);

  const QVector<Recipe> all = store.recipes(&reason);
  QCOMPARE(all.size(), 1);
  QCOMPARE(all.first().id, added->id);
}

void TestNativeUserRecipes::addingTheSameModelAndBackendAgainKeepsTheStoredRecipe() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  UserRecipeStore store(QDir(temp.path()).filePath(QStringLiteral("recipes")));
  const ModelRecord model = userSpatialRow(QString::fromLatin1(kShaA),
                                           QStringLiteral("animejanai-x2"));

  QString key;
  QString reason;
  const std::optional<Recipe> first =
      store.addSpatialModel(model, Backend::TensorRT, &key, &reason);
  QVERIFY2(first.has_value(), qPrintable(reason));

  // A stored recipe is never silently rewritten: hand it a different label and
  // the store must still answer with the document it already has.
  const QString path = storePath(store, *first);
  Recipe handEdited = *first;
  handEdited.label = QStringLiteral("Hand edited");
  QVERIFY(writeBytes(path, serializeRecipe(handEdited)));

  const std::optional<Recipe> second =
      store.addSpatialModel(model, Backend::TensorRT, &key, &reason);
  QVERIFY2(second.has_value(), qPrintable(reason));
  QCOMPARE(second->id, first->id);
  QCOMPARE(second->label, QStringLiteral("Hand edited"));
  QCOMPARE(QDir(store.root()).entryList(QDir::Files).size(), 1);
}

void TestNativeUserRecipes::twoFilesSharingABasenameStayTwoRecipes() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  UserRecipeStore store(QDir(temp.path()).filePath(QStringLiteral("recipes")));

  // Same display name and same declared profile, different bytes: two rows.
  const ModelRecord first = userSpatialRow(QString::fromLatin1(kShaA),
                                           QStringLiteral("animejanai-x2"));
  const ModelRecord second = userSpatialRow(QString::fromLatin1(kShaB),
                                            QStringLiteral("animejanai-x2"));
  QVERIFY(first.id != second.id);

  QString key;
  QString reason;
  const std::optional<Recipe> a = store.addSpatialModel(first, Backend::TensorRT, &key, &reason);
  const std::optional<Recipe> b = store.addSpatialModel(second, Backend::TensorRT, &key, &reason);
  QVERIFY2(a.has_value(), qPrintable(reason));
  QVERIFY2(b.has_value(), qPrintable(reason));
  QVERIFY(a->id != b->id);
  QCOMPARE(QDir(store.root()).entryList(QDir::Files).size(), 2);
  QCOMPARE(store.recipes(&reason).size(), 2);
  QVERIFY(store.find(a->id, &key, &reason).has_value());
  QVERIFY(store.find(b->id, &key, &reason).has_value());
}

void TestNativeUserRecipes::theSameContentOnTwoBackendsIsTwoExplicitRecipes() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  UserRecipeStore store(QDir(temp.path()).filePath(QStringLiteral("recipes")));
  const ModelRecord model = userSpatialRow(QString::fromLatin1(kShaA),
                                           QStringLiteral("animejanai-x2"));

  QString key;
  QString reason;
  const std::optional<Recipe> tensorrt =
      store.addSpatialModel(model, Backend::TensorRT, &key, &reason);
  const std::optional<Recipe> directml =
      store.addSpatialModel(model, Backend::DirectML, &key, &reason);
  QVERIFY2(tensorrt.has_value(), qPrintable(reason));
  QVERIFY2(directml.has_value(), qPrintable(reason));
  QVERIFY(tensorrt->id != directml->id);
  QCOMPARE(store.recipes(&reason).size(), 2);

  // Storing the second backend never mutates the first.
  const std::optional<Recipe> readTensorrt = store.find(tensorrt->id, &key, &reason);
  const std::optional<Recipe> readDirectml = store.find(directml->id, &key, &reason);
  QVERIFY2(readTensorrt.has_value(), qPrintable(reason));
  QVERIFY2(readDirectml.has_value(), qPrintable(reason));
  QCOMPARE(readTensorrt->backend, Backend::TensorRT);
  QCOMPARE(readDirectml->backend, Backend::DirectML);
}

void TestNativeUserRecipes::anIneligibleProfileIsRefusedAndNothingIsWritten() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  UserRecipeStore store(QDir(temp.path()).filePath(QStringLiteral("recipes")));

  ModelProfile notFp16 = upscale2x(1080, 1920);
  notFp16.output.dtype = QStringLiteral("fp32");
  const ModelRecord model = userSpatialRow(QString::fromLatin1(kShaA),
                                           QStringLiteral("wide"), notFp16);
  QVERIFY(!model.isOffered());

  QString key;
  QString reason;
  const std::optional<Recipe> added =
      store.addSpatialModel(model, Backend::TensorRT, &key, &reason);
  QVERIFY(!added.has_value());
  QCOMPARE(key, QStringLiteral("model.profile.ineligible"));
  QVERIFY(!QFileInfo::exists(store.root()));
  QVERIFY(store.recipes(&reason).isEmpty());
}

void TestNativeUserRecipes::aNonSpatialOrUnreadyRowIsRefusedWithItsOwnReason() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  UserRecipeStore store(QDir(temp.path()).filePath(QStringLiteral("recipes")));

  ModelRecord temporal;
  temporal.id = rifeModelName(RifeVersion::V4_14, RifeVariant::Full, false);
  temporal.displayName = temporal.id;
  temporal.origin = ModelOrigin::Builtin;
  temporal.onnxSha256 = QString::fromLatin1(kShaB);
  temporal.role = ModelRole::Temporal;
  temporal.availability = readyAvailability();
  QVERIFY(temporal.isOffered());

  QString key;
  QString reason;
  QVERIFY(!store.addSpatialModel(temporal, Backend::TensorRT, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("recipe.model.role"));

  ModelRecord unready = userSpatialRow(QString::fromLatin1(kShaA), QStringLiteral("unready"));
  unready.availability = unavailableAvailability(QStringLiteral("model.io.unsupported"),
                                                 QStringLiteral("declared fp32; not eligible"));
  QVERIFY(unready.isOffered());
  QVERIFY(!store.addSpatialModel(unready, Backend::TensorRT, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("recipe.model.notReady"));
  QVERIFY(!QFileInfo::exists(store.root()));
}

void TestNativeUserRecipes::aModelIdThatDoesNotMatchItsOwnHashIsRefused() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  UserRecipeStore store(QDir(temp.path()).filePath(QStringLiteral("recipes")));

  // A content-addressed id that names some other content is not an identity.
  ModelRecord liar = userSpatialRow(QString::fromLatin1(kShaA), QStringLiteral("liar"));
  liar.id = userModelIdFor(QStringLiteral("sha256:") + QString::fromLatin1(kShaB));
  QVERIFY(liar.isOffered());
  QString key;
  QString reason;
  QVERIFY(!store.addSpatialModel(liar, Backend::TensorRT, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("recipe.model.idInconsistent"));

  // A basename is a legal id but never identity for a user import.
  ModelRecord basename = userSpatialRow(QString::fromLatin1(kShaA), QStringLiteral("by name"));
  basename.id = QStringLiteral("animejanai-x2");
  QVERIFY(!basename.isOffered());
  QVERIFY(!store.addSpatialModel(basename, Backend::TensorRT, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("model.id.builtin.not.user"));
  QVERIFY(!QFileInfo::exists(store.root()));
}

void TestNativeUserRecipes::aRecipeIdThisStoreNeverIssuedIsRefused() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  UserRecipeStore store(QDir(temp.path()).filePath(QStringLiteral("recipes")));
  const ModelRecord model = userSpatialRow(QString::fromLatin1(kShaA),
                                           QStringLiteral("animejanai-x2"));

  QString key;
  QString reason;
  // Not a store id at all.
  QVERIFY(!store.find(QStringLiteral("janai.spatial.hd2x.v1"), &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("recipe.id.invalid"));
  // A store-shaped id whose backend does not exist.
  QVERIFY(!store
               .find(QStringLiteral("recipe:sha256:") + QString::fromLatin1(kShaA) +
                         QStringLiteral(":cuda"),
                     &key, &reason)
               .has_value());
  QCOMPARE(key, QStringLiteral("recipe.id.invalid"));
  // A traversal hidden where the backend name would be is refused, not resolved.
  QVERIFY(!store
               .find(QStringLiteral("recipe:sha256:") + QString::fromLatin1(kShaA) +
                         QStringLiteral(":../../evil"),
                     &key, &reason)
               .has_value());
  QCOMPARE(key, QStringLiteral("recipe.id.invalid"));
  // A well-formed id with no stored document is named as missing.
  QVERIFY(!store.find(spatialRecipeIdFor(model, Backend::TensorRT), &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("recipe.missing"));
}

void TestNativeUserRecipes::aDocumentContradictingItsOwnNameIsSkippedAndRefused() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  UserRecipeStore store(QDir(temp.path()).filePath(QStringLiteral("recipes")));
  QString reason;
  QVERIFY2(store.ensureOwned(&reason), qPrintable(reason));

  const ModelRecord model = userSpatialRow(QString::fromLatin1(kShaA),
                                           QStringLiteral("animejanai-x2"));

  // A valid recipe whose id does not match the document's own file name.
  Recipe impostor;
  impostor.id = spatialRecipeIdFor(model, Backend::DirectML);
  impostor.label = QStringLiteral("impostor");
  impostor.backend = Backend::TensorRT;
  SpatialStage stage;
  stage.modelId = model.id;
  impostor.spatial = {stage};
  QVERIFY(validateRecipe(impostor).ok);
  QVERIFY(writeBytes(storePath(store, impostor), serializeRecipe(impostor)));

  QVERIFY(store.recipes(&reason).isEmpty());
  QString key;
  QVERIFY(!store.find(spatialRecipeIdFor(model, Backend::TensorRT), &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("recipe.id.mismatch"));

  // A truncated document is skipped too, never read as a recipe.
  const ModelRecord other = userSpatialRow(QString::fromLatin1(kShaB), QStringLiteral("other"));
  Recipe truncated;
  truncated.backend = Backend::DirectML;
  SpatialStage truncatedStage;
  truncatedStage.modelId = other.id;
  truncated.spatial = {truncatedStage};
  QVERIFY(writeBytes(storePath(store, truncated), QByteArrayLiteral("{ not json")));
  QVERIFY(store.recipes(&reason).isEmpty());
}

void TestNativeUserRecipes::anUnownedStoreRootIsRefused() {
  QString reason;
  UserRecipeStore none{QString()};
  QVERIFY(!none.isOwned(&reason));

  const UserRecipeStore relative(QStringLiteral("recipes"));
  QVERIFY(!relative.isOwned(&reason));

  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  const QString filePath = QDir(temp.path()).filePath(QStringLiteral("not-a-dir"));
  QVERIFY(writeBytes(filePath, QByteArrayLiteral("x")));
  const UserRecipeStore fileStore(filePath);
  QVERIFY(!fileStore.isOwned(&reason));

  const UserRecipeStore missingParent(
      QDir(temp.path()).filePath(QStringLiteral("absent/child")));
  QVERIFY(!missingParent.isOwned(&reason));

  const ModelRecord model = userSpatialRow(QString::fromLatin1(kShaA),
                                           QStringLiteral("animejanai-x2"));
  QString key;
  QVERIFY(!none.addSpatialModel(model, Backend::TensorRT, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("recipe.store.unowned"));
}

void TestNativeUserRecipes::aRifeOnlyRecipePersistsItsExactInterpolationSettings() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  UserRecipeStore store(QDir(temp.path()).filePath(QStringLiteral("recipes")));

  QString key;
  QString reason;
  // Zero spatial stages and no interpolation stage is the silent no-op this
  // store exists to refuse; it is not "some RIFE".
  const Recipe empty;
  QVERIFY(!store.saveRecipe(empty, &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("recipe.empty"));
  QVERIFY(!QFileInfo::exists(store.root()));

  Recipe body;
  body.backend = Backend::DirectML;
  body.label = QStringLiteral("RIFE 4.14 lite ensemble, 3/2 before spatial");
  TemporalStage temporal;
  temporal.version = RifeVersion::V4_14;
  temporal.variant = RifeVariant::Lite;
  temporal.ensemble = true;
  temporal.factor = Rational{3, 2};
  temporal.order = TemporalOrder::BeforeSpatial;
  temporal.sceneThreshold = 0.2;
  body.temporal = temporal;

  const std::optional<Recipe> saved = store.saveRecipe(body, &key, &reason);
  QVERIFY2(saved.has_value(), qPrintable(reason));
  QCOMPARE(saved->shape(), RecipeShape::RifeOnly);
  QCOMPARE(saved->id, storedRecipeIdFor(body));
  QCOMPARE(QFileInfo(storePath(store, *saved)).fileName(),
           recipeBodyHash(body) + QStringLiteral("-directml.json"));
  QCOMPARE(QDir(store.root()).entryList(QDir::Files).size(), 1);

  const std::optional<Recipe> read = store.find(saved->id, &key, &reason);
  QVERIFY2(read.has_value(), qPrintable(reason));
  QVERIFY(read->spatial.isEmpty());
  QVERIFY(read->temporal.has_value());
  QCOMPARE(read->temporal->version, RifeVersion::V4_14);
  QCOMPARE(read->temporal->variant, RifeVariant::Lite);
  QVERIFY(read->temporal->ensemble);
  QCOMPARE(read->temporal->factor.num, 3);
  QCOMPARE(read->temporal->factor.den, 2);
  QCOMPARE(read->temporal->order, TemporalOrder::BeforeSpatial);
  QCOMPARE(read->temporal->sceneThreshold, 0.2);

  // The label is display only: a different one names the same body, so the
  // stored document is read back rather than rewritten or duplicated.
  Recipe relabelled = body;
  relabelled.label = QStringLiteral("Something else entirely");
  const std::optional<Recipe> again = store.saveRecipe(relabelled, &key, &reason);
  QVERIFY2(again.has_value(), qPrintable(reason));
  QCOMPARE(again->id, saved->id);
  QCOMPARE(again->label, saved->label);
  QCOMPARE(QDir(store.root()).entryList(QDir::Files).size(), 1);

  // A different revision, variant or ensemble is a different body, so it is a
  // different recipe rather than a later resolution of the same request.
  Recipe plain = body;
  TemporalStage full;
  full.factor = temporal.factor;
  full.order = temporal.order;
  full.sceneThreshold = temporal.sceneThreshold;
  plain.temporal = full;
  const std::optional<Recipe> alsoSaved = store.saveRecipe(plain, &key, &reason);
  QVERIFY2(alsoSaved.has_value(), qPrintable(reason));
  QVERIFY(alsoSaved->id != saved->id);
  QCOMPARE(QDir(store.root()).entryList(QDir::Files).size(), 2);
  QCOMPARE(store.recipes(&reason).size(), 2);
}

void TestNativeUserRecipes::aCombinedRecipeKeepsItsOrderedPreResizeChain() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  UserRecipeStore store(QDir(temp.path()).filePath(QStringLiteral("recipes")));

  SpatialStage first;
  first.modelId = QStringLiteral("janai.spatial.hd2x.v1");
  first.preResizeEnabled = true;
  first.preResize = QSize(1920, 1080);
  SpatialStage second;
  second.modelId = QStringLiteral("janai.spatial.x2.v1");
  second.preResizeEnabled = false;

  Recipe body;
  body.backend = Backend::TensorRT;
  body.label = QStringLiteral("two spatial stages then RIFE");
  body.spatial = {first, second};
  TemporalStage temporal;
  temporal.factor = Rational{2, 1};
  temporal.order = TemporalOrder::AfterSpatial;
  temporal.sceneThreshold = 0.15;
  body.temporal = temporal;

  QString key;
  QString reason;
  const std::optional<Recipe> saved = store.saveRecipe(body, &key, &reason);
  QVERIFY2(saved.has_value(), qPrintable(reason));
  QCOMPARE(saved->shape(), RecipeShape::Combined);

  const std::optional<Recipe> read = store.find(saved->id, &key, &reason);
  QVERIFY2(read.has_value(), qPrintable(reason));
  QCOMPARE(read->spatial.size(), 2);
  QCOMPARE(read->spatial.at(0).modelId, QStringLiteral("janai.spatial.hd2x.v1"));
  QCOMPARE(read->spatial.at(1).modelId, QStringLiteral("janai.spatial.x2.v1"));
  QVERIFY(read->spatial.at(0).preResizeEnabled);
  QCOMPARE(read->spatial.at(0).preResize, QSize(1920, 1080));
  QVERIFY(!read->spatial.at(1).preResizeEnabled);
  QVERIFY(read->temporal.has_value());
  QCOMPARE(read->temporal->order, TemporalOrder::AfterSpatial);
  QCOMPARE(read->temporal->factor.toString(), QStringLiteral("2/1"));

  // Order is load bearing: the reversed chain is a different recipe, with its
  // own id and its own document, never the same one read back.
  Recipe reversed = body;
  reversed.spatial = {second, first};
  QVERIFY(storedRecipeIdFor(reversed) != saved->id);
  const std::optional<Recipe> alsoSaved = store.saveRecipe(reversed, &key, &reason);
  QVERIFY2(alsoSaved.has_value(), qPrintable(reason));
  QCOMPARE(store.recipes(&reason).size(), 2);
}

void TestNativeUserRecipes::aTamperedStageIsRefusedAndNeverRead() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  UserRecipeStore store(QDir(temp.path()).filePath(QStringLiteral("recipes")));

  Recipe body;
  body.backend = Backend::TensorRT;
  body.label = QStringLiteral("RIFE 4.14");
  TemporalStage temporal;
  temporal.factor = Rational{2, 1};
  body.temporal = temporal;

  QString key;
  QString reason;
  const std::optional<Recipe> saved = store.saveRecipe(body, &key, &reason);
  QVERIFY2(saved.has_value(), qPrintable(reason));

  // Someone edits the interpolation factor in place. The id and the file name
  // still describe the body that was stored, so the document is refused rather
  // than read as a recipe with a factor it never agreed to.
  const QString path = storePath(store, *saved);
  QVERIFY(QFileInfo::exists(path));
  Recipe edited = *saved;
  edited.temporal->factor = Rational{4, 1};
  QVERIFY(writeBytes(path, serializeRecipe(edited)));

  QString findKey;
  QVERIFY(!store.find(saved->id, &findKey, &reason).has_value());
  QCOMPARE(findKey, QStringLiteral("recipe.body.mismatch"));
  QVERIFY(store.recipes(&reason).isEmpty());

  // Saving the same body again finds the tampered document and refuses it too:
  // a corrupt document is never silently overwritten with a matching one.
  const std::optional<Recipe> again = store.saveRecipe(body, &key, &reason);
  QVERIFY(!again.has_value());
  QCOMPARE(key, QStringLiteral("recipe.corrupt"));
  QVERIFY(store.recipes(&reason).isEmpty());
}
