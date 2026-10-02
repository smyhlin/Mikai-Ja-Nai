// The app-owned model store. See NativeModelInventory.hpp for the rules; this
// file is their one implementation.
//
// The manifest is a versioned document that a reader which does not know this
// exact schema rejects. It records the identity facts the row is rebuilt from -
// digest, declared profile, availability - so a hand-edited manifest cannot
// silently repoint a row at different bytes: the id is re-derived on every read
// and must still agree with the digest, the profile and the directory name.

#include "NativeModelInventory.hpp"

#include <cmath>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QSaveFile>
#include <QtCore/QStringList>
#include <QtCore/QUuid>

#include "NativeArtifactStore.hpp"
#include "NativePaths.hpp"

namespace mplayer {
namespace native {

namespace {

constexpr const char* kSchema = "mikai.native.model/2";
constexpr const char* kManifestFileName = "record.json";
constexpr const char* kModelFileName = "model.onnx";
constexpr const char* kStagingPrefix = ".staging-";

void fail(QString* reasonKey, QString* reason, const QString& key, const QString& text) {
  if (reasonKey) *reasonKey = key;
  if (reason) *reason = text;
}

void clearReason(QString* reasonKey, QString* reason) {
  if (reasonKey) reasonKey->clear();
  if (reason) reason->clear();
}

/// The 64 hex characters of "sha256:<hex>", or empty.
QString contentToken(const QString& contentId) {
  if (!contentId.startsWith(QLatin1String("sha256:"))) return {};
  const QString hex = contentId.mid(7);
  return isSha256Hex(hex) ? hex : QString();
}

QString originName(ModelOrigin origin) {
  return origin == ModelOrigin::Builtin ? QStringLiteral("builtin") : QStringLiteral("user");
}

std::optional<ModelOrigin> originFromName(const QString& name) {
  if (name == QLatin1String("builtin")) return ModelOrigin::Builtin;
  if (name == QLatin1String("user")) return ModelOrigin::UserImported;
  return std::nullopt;
}

// ------------------------------------------------------------- json reading

/// A whole number that survives the trip through JSON's double. 2^53 is the
/// largest integer a double represents exactly, so a value beyond it is refused
/// rather than rounded into a different geometry.
bool jsonIsInteger(const QJsonValue& value) {
  if (!value.isDouble()) return false;
  const double d = value.toDouble();
  if (!std::isfinite(d)) return false;
  if (std::floor(d) != d) return false;
  return d >= -9007199254740992.0 && d <= 9007199254740992.0;
}

bool jsonInt64(const QJsonObject& object, const QString& key, qint64* out) {
  const QJsonValue value = object.value(key);
  if (!jsonIsInteger(value)) return false;
  *out = static_cast<qint64>(value.toDouble());
  return true;
}

bool jsonBool(const QJsonObject& object, const QString& key, bool* out) {
  const QJsonValue value = object.value(key);
  if (!value.isBool()) return false;
  *out = value.toBool();
  return true;
}

bool jsonString(const QJsonObject& object, const QString& key, QString* out) {
  const QJsonValue value = object.value(key);
  if (!value.isString()) return false;
  *out = value.toString();
  return true;
}

/// Rejects a member this schema version does not define. A silently ignored
/// typo would leave a row describing something the document never said.
bool noUnknownKeys(const QJsonObject& object, const QStringList& known) {
  for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
    if (!known.contains(it.key())) return false;
  }
  return true;
}

QStringList tensorKeys() {
  return {QStringLiteral("dtype"), QStringLiteral("rank"), QStringLiteral("n"),
          QStringLiteral("c"),     QStringLiteral("h"),    QStringLiteral("w")};
}

QStringList profileKeys() { return {QStringLiteral("input"), QStringLiteral("output")}; }

QStringList availabilityKeys() {
  return {QStringLiteral("value"), QStringLiteral("reasonKey"), QStringLiteral("reason")};
}
/// The row's recorded backend support, as the stable names this build knows.
/// Model compatibility, not runtime presence: it says what the model was
/// published as runnable on, and is rehydrated on every read rather than
/// re-derived from whatever runtime happens to open the store.
QJsonArray backendsToJson(const QVector<Backend>& backends) {
  // A name supplied twice is written once: the reader refuses a duplicated name,
  // so writing one would produce a document the store itself would reject. The
  // claim is a set either way.
  QJsonArray array;
  QVector<Backend> written;
  for (const Backend backend : backends) {
    if (written.contains(backend)) continue;
    written.append(backend);
    array.append(stableName(backend));
  }
  return array;
}

/// The inverse. An element that is not a string, a name this build does not
/// know, or a name that appears twice is refused: a row that claims a backend
/// this build cannot name, or claims one twice, is describing something other
/// than what it recorded.
bool backendsFromJson(const QJsonValue& value, QVector<Backend>* backends, QString* error) {
  if (!value.isArray()) {
    *error = QStringLiteral("the recorded backend support is not a list");
    return false;
  }
  QVector<Backend> parsed;
  for (const QJsonValue& element : value.toArray()) {
    if (!element.isString()) {
      *error = QStringLiteral("the recorded backend support carries a value that is not a name");
      return false;
    }
    const std::optional<Backend> backend = backendFromName(element.toString());
    if (!backend.has_value()) {
      *error = QStringLiteral("the recorded backend support names '%1', which this build does not "
                              "know")
                   .arg(element.toString());
      return false;
    }
    if (parsed.contains(*backend)) {
      *error = QStringLiteral("the recorded backend support names '%1' twice")
                   .arg(element.toString());
      return false;
    }
    parsed.append(*backend);
  }
  *backends = parsed;
  return true;
}

QStringList sourceKeys() {
  return {QStringLiteral("path"), QStringLiteral("sha256"), QStringLiteral("bytes")};
}

QStringList geometryKeys() { return {QStringLiteral("w"), QStringLiteral("h")}; }

QStringList referenceKeys() {
  return {QStringLiteral("ran"),     QStringLiteral("in"),   QStringLiteral("out"),
          QStringLiteral("scale"),   QStringLiteral("profile")};
}

QStringList manifestKeys() {
  return {QStringLiteral("schema"),       QStringLiteral("id"),        QStringLiteral("displayName"),
          QStringLiteral("origin"),       QStringLiteral("role"),      QStringLiteral("source"),
          QStringLiteral("storedFile"),   QStringLiteral("declared"),  QStringLiteral("termsNote"),
          QStringLiteral("backends"),     QStringLiteral("availability"),
          QStringLiteral("reference"),    QStringLiteral("importedAt")};
}

QJsonObject tensorToJson(const TensorProfile& tensor) {
  QJsonObject object;
  object.insert(QStringLiteral("dtype"), tensor.dtype);
  object.insert(QStringLiteral("rank"), tensor.rank);
  object.insert(QStringLiteral("n"), QJsonValue(static_cast<double>(tensor.batch)));
  object.insert(QStringLiteral("c"), QJsonValue(static_cast<double>(tensor.channels)));
  if (tensor.height.has_value()) {
    object.insert(QStringLiteral("h"), QJsonValue(static_cast<double>(*tensor.height)));
  }
  if (tensor.width.has_value()) {
    object.insert(QStringLiteral("w"), QJsonValue(static_cast<double>(*tensor.width)));
  }
  return object;
}

QJsonObject profileToJson(const ModelProfile& profile) {
  QJsonObject object;
  object.insert(QStringLiteral("input"), tensorToJson(profile.input));
  object.insert(QStringLiteral("output"), tensorToJson(profile.output));
  return object;
}

bool tensorFromJson(const QJsonObject& object, TensorProfile* tensor, QString* error) {
  if (!noUnknownKeys(object, tensorKeys())) {
    *error = QStringLiteral("a declared tensor carries a key this schema does not define");
    return false;
  }
  TensorProfile parsed;
  if (!jsonString(object, QStringLiteral("dtype"), &parsed.dtype) || parsed.dtype.isEmpty()) {
    *error = QStringLiteral("a declared tensor has no dtype");
    return false;
  }
  qint64 rank = 0;
  qint64 batch = 0;
  qint64 channels = 0;
  if (!jsonInt64(object, QStringLiteral("rank"), &rank) || rank < 0) {
    *error = QStringLiteral("a declared tensor has no usable rank");
    return false;
  }
  if (!jsonInt64(object, QStringLiteral("n"), &batch) ||
      !jsonInt64(object, QStringLiteral("c"), &channels)) {
    *error = QStringLiteral("a declared tensor has no usable N/C");
    return false;
  }
  parsed.rank = static_cast<int>(rank);
  parsed.batch = batch;
  parsed.channels = channels;
  if (object.contains(QStringLiteral("h"))) {
    qint64 height = 0;
    if (!jsonInt64(object, QStringLiteral("h"), &height)) {
      *error = QStringLiteral("a declared tensor has an unusable H");
      return false;
    }
    parsed.height = height;
  }
  if (object.contains(QStringLiteral("w"))) {
    qint64 width = 0;
    if (!jsonInt64(object, QStringLiteral("w"), &width)) {
      *error = QStringLiteral("a declared tensor has an unusable W");
      return false;
    }
    parsed.width = width;
  }
  *tensor = parsed;
  return true;
}

bool profileFromJson(const QJsonObject& object, ModelProfile* profile, QString* error) {
  if (!noUnknownKeys(object, profileKeys())) {
    *error = QStringLiteral("the declared profile carries a key this schema does not define");
    return false;
  }
  const QJsonValue inputValue = object.value(QStringLiteral("input"));
  const QJsonValue outputValue = object.value(QStringLiteral("output"));
  if (!inputValue.isObject() || !outputValue.isObject()) {
    *error = QStringLiteral("the declared profile is missing its input or output");
    return false;
  }
  ModelProfile parsed;
  if (!tensorFromJson(inputValue.toObject(), &parsed.input, error)) return false;
  if (!tensorFromJson(outputValue.toObject(), &parsed.output, error)) return false;
  *profile = parsed;
  return true;
}

// ------------------------------------------------------------ store helpers

/// A regular file that is inside the owned root and is not reached through a
/// reparse point. Every file this module hashes or trusts goes through this.
bool isRegularOwnedFile(const QString& path, const QString& root, QString* reasonKey,
                        QString* reason) {
  const QFileInfo info(path);
  if (!info.exists()) {
    fail(reasonKey, reason, QStringLiteral("model.store.missing"),
         QStringLiteral("%1 does not exist").arg(path));
    return false;
  }
  if (info.isSymLink() || !info.isFile()) {
    fail(reasonKey, reason, QStringLiteral("model.store.notRegular"),
         QStringLiteral("%1 is not a regular file").arg(path));
    return false;
  }
  if (!pathWithinRoot(root, path) || !pathHasNoReparsePoints(root, path)) {
    fail(reasonKey, reason, QStringLiteral("model.store.escape"),
         QStringLiteral("%1 is outside %2 or reached through a reparse point").arg(path, root));
    return false;
  }
  return true;
}

/// The two support sets as sets: the order a provisioner names backends in is not
/// part of what a row records, so a re-import naming the same backends in another
/// order is the same claim and rewrites nothing.
bool sameBackendSet(const QVector<Backend>& recorded, const QVector<Backend>& supplied) {
  if (recorded.size() != supplied.size()) return false;
  for (const Backend backend : supplied) {
    if (!recorded.contains(backend)) return false;
  }
  return true;
}

/// Rewrites only the recorded backend support of an already published row.
///
/// This is the one update an owned row admits after it is written: a provisioner
/// that later vouches for a backend the row was not published with raises that
/// claim without a new graph identity, a second copy or a re-inspection. Nothing
/// else is reachable from here - the graph bytes, the content identity, the
/// declared profile, the source provenance and the CPU reference proof stay
/// exactly as they were - because the document is re-read and re-checked against
/// the row it must still describe (schema, id, digest) before only its `backends`
/// member is replaced, atomically, through a save file.
bool updateRecordedSupport(const QString& directory, const StoredModel& row,
                           const QVector<Backend>& backends, QString* reasonKey,
                           QString* reason) {
  const QString manifestPath = QDir(directory).filePath(QString::fromLatin1(kManifestFileName));
  QFile file(manifestPath);
  if (!file.open(QIODevice::ReadOnly)) {
    fail(reasonKey, reason, QStringLiteral("model.publish.manifest"),
         QStringLiteral("%1 cannot be read: %2").arg(manifestPath, file.errorString()));
    return false;
  }
  const QByteArray payload = file.readAll();
  if (file.error() != QFile::NoError) {
    fail(reasonKey, reason, QStringLiteral("model.publish.manifest"),
         QStringLiteral("%1 could not be read: %2").arg(manifestPath, file.errorString()));
    return false;
  }
  file.close();

  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 is not a JSON object: %2").arg(manifestPath, parseError.errorString()));
    return false;
  }
  QJsonObject object = document.object();
  QString schema;
  QString id;
  QString digest;
  const QJsonValue sourceValue = object.value(QStringLiteral("source"));
  const bool stillThisRow =
      noUnknownKeys(object, manifestKeys()) &&
      jsonString(object, QStringLiteral("schema"), &schema) && schema == QLatin1String(kSchema) &&
      jsonString(object, QStringLiteral("id"), &id) && id == row.record.id &&
      sourceValue.isObject() && noUnknownKeys(sourceValue.toObject(), sourceKeys()) &&
      jsonString(sourceValue.toObject(), QStringLiteral("sha256"), &digest) &&
      digest == row.record.onnxSha256 && object.contains(QStringLiteral("backends"));
  if (!stillThisRow) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 no longer describes the row its support is being updated for")
             .arg(manifestPath));
    return false;
  }
  object.insert(QStringLiteral("backends"), backendsToJson(backends));

  const QByteArray updated = QJsonDocument(object).toJson(QJsonDocument::Compact);
  QSaveFile save(manifestPath);
  if (!save.open(QIODevice::WriteOnly)) {
    fail(reasonKey, reason, QStringLiteral("model.publish.manifest"),
         QStringLiteral("%1 could not be written: %2").arg(manifestPath, save.errorString()));
    return false;
  }
  if (save.write(updated) != updated.size() || !save.commit()) {
    fail(reasonKey, reason, QStringLiteral("model.publish.manifest"),
         QStringLiteral("%1 could not be written").arg(manifestPath));
    return false;
  }
  clearReason(reasonKey, reason);
  return true;
}

QJsonObject manifestFor(const ModelRecord& record, const QString& sourcePath,
                        const std::optional<StageResolution>& resolution) {
  QJsonObject manifest;
  manifest.insert(QStringLiteral("schema"), QString::fromLatin1(kSchema));
  manifest.insert(QStringLiteral("id"), record.id);
  manifest.insert(QStringLiteral("displayName"), record.displayName);
  manifest.insert(QStringLiteral("origin"), originName(record.origin));
  manifest.insert(QStringLiteral("role"), stableName(record.role));

  QJsonObject source;
  source.insert(QStringLiteral("path"), sourcePath);
  source.insert(QStringLiteral("sha256"), record.onnxSha256);
  source.insert(QStringLiteral("bytes"), QJsonValue(static_cast<double>(record.onnxBytes)));
  manifest.insert(QStringLiteral("source"), source);

  manifest.insert(QStringLiteral("storedFile"), QString::fromLatin1(kModelFileName));
  manifest.insert(QStringLiteral("declared"), profileToJson(record.actualProfile));
  manifest.insert(QStringLiteral("termsNote"), record.termsNote);

  QJsonObject availability;
  availability.insert(QStringLiteral("value"), stableName(record.availability.value));
  if (!record.availability.reasonKey.isEmpty()) {
    availability.insert(QStringLiteral("reasonKey"), record.availability.reasonKey);
    availability.insert(QStringLiteral("reason"), record.availability.reason);
  }
  manifest.insert(QStringLiteral("availability"), availability);
  // The row's recorded backend support travels with the row: the store's reader
  // rebuilds the record from the document, so support that is not written is
  // support that would be lost on the next read.
  manifest.insert(QStringLiteral("backends"), backendsToJson(record.availableBackends));

  // A reference block exists exactly when a genuine CPU run produced the row's
  // boundary. `in`/`out` are the geometry the import resolved; `profile` is the
  // concrete dtype/rank/N/C/H/W the run produced, kept as its own evidence
  // beside the declared profile rather than folded into it.
  if (resolution.has_value() && resolution->fromCpuReference &&
      resolution->verifiedReferenceProfile.has_value()) {
    QJsonObject reference;
    reference.insert(QStringLiteral("ran"), true);
    QJsonObject in;
    in.insert(QStringLiteral("w"), resolution->input.width());
    in.insert(QStringLiteral("h"), resolution->input.height());
    reference.insert(QStringLiteral("in"), in);
    QJsonObject out;
    out.insert(QStringLiteral("w"), resolution->output.width());
    out.insert(QStringLiteral("h"), resolution->output.height());
    reference.insert(QStringLiteral("out"), out);
    reference.insert(QStringLiteral("scale"), resolution->scale);
    reference.insert(QStringLiteral("profile"),
                     profileToJson(*resolution->verifiedReferenceProfile));
    manifest.insert(QStringLiteral("reference"), reference);
  }

  manifest.insert(QStringLiteral("importedAt"),
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
  return manifest;
}

bool readGeometry(const QJsonObject& object, QSize* size, QString* error) {
  if (!noUnknownKeys(object, geometryKeys())) {
    *error = QStringLiteral("a geometry carries a key this schema does not define");
    return false;
  }
  qint64 width = 0;
  qint64 height = 0;
  if (!jsonInt64(object, QStringLiteral("w"), &width) ||
      !jsonInt64(object, QStringLiteral("h"), &height) || width <= 0 || height <= 0) {
    *error = QStringLiteral("a geometry has no usable positive W/H");
    return false;
  }
  *size = QSize(static_cast<int>(width), static_cast<int>(height));
  return true;
}

/// The concrete boundary a genuine CPU reference run produced, or nothing with a
/// named reason when the run did not happen or the evidence does not agree with
/// the declaration. The declaration alone decides whether a run is required; a
/// symbolic output batch or an unresolved geometry is only usable once this
/// answers.
std::optional<ModelProfile> referenceProofFrom(const ModelProfile& declared,
                                               const InspectionResult& inspection,
                                               QString* reasonKey, QString* reason) {
  const auto fail = [reasonKey, reason](const QString& key,
                                        const QString& text) -> std::optional<ModelProfile> {
    if (reasonKey) *reasonKey = key;
    if (reason) *reason = text;
    return std::nullopt;
  };
  if (!inspection.referenceRan) {
    // A run that was attempted and refused is its own named failure. A run that
    // simply did not happen - nothing offered, or no geometry to run at - means
    // the row still needs one, which is a different fact from a run failing.
    const bool runWasRefused =
        !inspection.referenceKey.isEmpty() &&
        inspection.referenceKey != QLatin1String("inspect.reference.geometry");
    if (runWasRefused) {
      return fail(QStringLiteral("model.reference.failed"),
                  inspection.referenceReason.isEmpty()
                      ? QStringLiteral("the CPU reference did not run")
                      : inspection.referenceReason);
    }
    // No run happened: the declared boundary cannot vouch for itself.
    return fail(QStringLiteral("model.reference.needed"),
                QStringLiteral("the declared boundary can only be offered once a real CPU "
                               "reference run proves a concrete N=1/C=3 output at the requested "
                               "geometry"));
  }
  if (!inspection.resolvedGeometry.has_value() || !inspection.resolvedScale.has_value()) {
    return fail(QStringLiteral("model.reference.uniform"),
                inspection.referenceReason.isEmpty()
                    ? QStringLiteral("the CPU reference produced no uniform positive integer geometry")
                    : inspection.referenceReason);
  }
  if (!inspection.verifiedReferenceProfile.has_value()) {
    return fail(QStringLiteral("model.reference.inconsistent"),
                QStringLiteral("the CPU reference ran but reported no concrete verified boundary"));
  }
  const ModelProfile& proof = *inspection.verifiedReferenceProfile;
  const QString proofRefusal = spatialReferenceRefusal(declared, proof);
  if (!proofRefusal.isEmpty()) {
    return fail(QStringLiteral("model.reference.inconsistent"), proofRefusal);
  }
  // The profile and the geometry the run reported must be the same fact.
  if (!proof.input.hasGeometry() || !proof.output.hasGeometry() ||
      QSize(static_cast<int>(*proof.input.width), static_cast<int>(*proof.input.height)) !=
          inspection.runGeometry ||
      QSize(static_cast<int>(*proof.output.width), static_cast<int>(*proof.output.height)) !=
          *inspection.resolvedGeometry ||
      !proof.derivedUniformScale().has_value() ||
      *proof.derivedUniformScale() != *inspection.resolvedScale) {
    return fail(QStringLiteral("model.reference.inconsistent"),
                QStringLiteral("the verified reference boundary and the geometry the run reported "
                               "are not the same fact"));
  }
  return proof;
}

/// Reads and verifies one published directory. `requirePublishedName` is true for
/// a directory the store owns; staging is read back before it is renamed, so its
/// name is not the content token yet.
bool readStored(const QString& root, const QString& directory, StoredModel* model,
                QString* reasonKey, QString* reason, bool requirePublishedName) {
  const QFileInfo directoryInfo(directory);
  if (!directoryInfo.isDir() || !pathWithinRoot(root, directory) ||
      !pathHasNoReparsePoints(root, directory)) {
    fail(reasonKey, reason, QStringLiteral("model.store.directory"),
         QStringLiteral("%1 is not an owned directory").arg(directory));
    return false;
  }

  const QString manifestPath = QDir(directory).filePath(QString::fromLatin1(kManifestFileName));
  QFile manifestFile(manifestPath);
  if (!manifestFile.open(QIODevice::ReadOnly)) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 cannot be read: %2").arg(manifestPath, manifestFile.errorString()));
    return false;
  }
  const QByteArray payload = manifestFile.readAll();
  if (manifestFile.error() != QFile::NoError) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 could not be read: %2").arg(manifestPath, manifestFile.errorString()));
    return false;
  }
  manifestFile.close();

  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 is not a JSON object: %2")
             .arg(manifestPath, parseError.errorString()));
    return false;
  }
  const QJsonObject object = document.object();
  if (!noUnknownKeys(object, manifestKeys())) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 carries a key this schema does not define").arg(manifestPath));
    return false;
  }

  QString schema;
  if (!jsonString(object, QStringLiteral("schema"), &schema) ||
      schema != QLatin1String(kSchema)) {
    fail(reasonKey, reason, QStringLiteral("model.store.schema"),
         QStringLiteral("%1 has schema '%2', not %3")
             .arg(manifestPath, schema, QString::fromLatin1(kSchema)));
    return false;
  }

  StoredModel parsed;
  if (!jsonString(object, QStringLiteral("id"), &parsed.record.id) ||
      !isValidModelId(parsed.record.id)) {
    fail(reasonKey, reason, QStringLiteral("model.store.identity"),
         QStringLiteral("%1 has no usable model id").arg(manifestPath));
    return false;
  }
  if (!jsonString(object, QStringLiteral("displayName"), &parsed.record.displayName)) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 has no display name").arg(manifestPath));
    return false;
  }
  QString origin;
  const std::optional<ModelOrigin> parsedOrigin =
      jsonString(object, QStringLiteral("origin"), &origin) ? originFromName(origin) : std::nullopt;
  if (!parsedOrigin.has_value()) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 has no usable origin").arg(manifestPath));
    return false;
  }
  parsed.record.origin = *parsedOrigin;

  QString roleName;
  const std::optional<ModelRole> parsedRole =
      jsonString(object, QStringLiteral("role"), &roleName) ? modelRoleFromName(roleName)
                                                            : std::nullopt;
  if (!parsedRole.has_value()) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 has no usable role").arg(manifestPath));
    return false;
  }
  parsed.record.role = *parsedRole;
  parsed.record.termsNote = object.value(QStringLiteral("termsNote")).toString();

  const QJsonValue sourceValue = object.value(QStringLiteral("source"));
  if (!sourceValue.isObject()) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 has no source record").arg(manifestPath));
    return false;
  }
  const QJsonObject sourceObject = sourceValue.toObject();
  if (!noUnknownKeys(sourceObject, sourceKeys()) ||
      !jsonString(sourceObject, QStringLiteral("path"), &parsed.sourcePath) ||
      !jsonString(sourceObject, QStringLiteral("sha256"), &parsed.record.onnxSha256) ||
      !jsonInt64(sourceObject, QStringLiteral("bytes"), &parsed.record.onnxBytes)) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 has an unusable source record").arg(manifestPath));
    return false;
  }
  if (!isSha256Hex(parsed.record.onnxSha256) || parsed.record.onnxBytes <= 0) {
    fail(reasonKey, reason, QStringLiteral("model.store.identity"),
         QStringLiteral("%1 has no usable ONNX digest or size").arg(manifestPath));
    return false;
  }

  const QJsonValue declaredValue = object.value(QStringLiteral("declared"));
  if (!declaredValue.isObject()) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 has no declared profile").arg(manifestPath));
    return false;
  }
  QString profileError;
  if (!profileFromJson(declaredValue.toObject(), &parsed.record.actualProfile, &profileError)) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1: %2").arg(manifestPath, profileError));
    return false;
  }

  const QJsonValue availabilityValue = object.value(QStringLiteral("availability"));
  if (!availabilityValue.isObject()) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 has no availability").arg(manifestPath));
    return false;
  }
  const QJsonObject availabilityObject = availabilityValue.toObject();
  QString availabilityName;
  if (!noUnknownKeys(availabilityObject, availabilityKeys()) ||
      !jsonString(availabilityObject, QStringLiteral("value"), &availabilityName)) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 has an unusable availability").arg(manifestPath));
    return false;
  }
  const std::optional<Availability> parsedAvailability = availabilityFromName(availabilityName);
  if (!parsedAvailability.has_value()) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 has an unknown availability '%2'")
             .arg(manifestPath, availabilityName));
    return false;
  }
  if (*parsedAvailability == Availability::Ready) {
    parsed.record.availability = readyAvailability();
  } else {
    QString availabilityKey;
    QString availabilityReason;
    if (!jsonString(availabilityObject, QStringLiteral("reasonKey"), &availabilityKey) ||
        !jsonString(availabilityObject, QStringLiteral("reason"), &availabilityReason)) {
      fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
           QStringLiteral("%1 is %2 without a reason").arg(manifestPath, availabilityName));
      return false;
    }
    parsed.record.availability = unavailableAvailability(availabilityKey, availabilityReason);
  }

  // The recorded backend support is required, and is rehydrated here rather than
  // re-derived: a row whose document is silent about where it can run is not a
  // row this reader can rebuild, and one whose list names a backend this build
  // does not know is refused rather than read as some other set.
  if (!object.contains(QStringLiteral("backends"))) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 records no backend support for its row").arg(manifestPath));
    return false;
  }
  QString backendsError;
  if (!backendsFromJson(object.value(QStringLiteral("backends")), &parsed.record.availableBackends,
                        &backendsError)) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1: %2").arg(manifestPath, backendsError));
    return false;
  }

  QString storedFile;
  if (!jsonString(object, QStringLiteral("storedFile"), &storedFile)) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 names no stored file").arg(manifestPath));
    return false;
  }
  QString nameReason;
  const QString safeName = safeRelativePath(storedFile, &nameReason);
  if (safeName.isEmpty()) {
    fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
         QStringLiteral("%1 stored file '%2' is not safe: %3")
             .arg(manifestPath, storedFile, nameReason));
    return false;
  }
  const QString candidate = QDir(directory).filePath(safeName);
  if (!pathWithinRoot(root, candidate)) {
    fail(reasonKey, reason, QStringLiteral("model.store.escape"),
         QStringLiteral("%1 stored file resolves outside the store").arg(manifestPath));
    return false;
  }

  if (!isRegularOwnedFile(candidate, root, reasonKey, reason)) return false;
  const QString canonicalStored = canonicalPath(candidate);
  if (canonicalStored.isEmpty()) {
    fail(reasonKey, reason, QStringLiteral("model.store.missing"),
         QStringLiteral("%1 could not be resolved").arg(candidate));
    return false;
  }

  QString hashReason;
  const std::optional<FileDigest> measured = measureFile(canonicalStored, &hashReason);
  if (!measured.has_value()) {
    fail(reasonKey, reason, QStringLiteral("model.store.digest"), hashReason);
    return false;
  }
  if (measured->sha256 != parsed.record.onnxSha256 || measured->bytes != parsed.record.onnxBytes) {
    fail(reasonKey, reason, QStringLiteral("model.store.digest"),
         QStringLiteral("%1 is %2 bytes hashing %3, but the manifest records %4 bytes hashing %5")
             .arg(canonicalStored)
             .arg(measured->bytes)
             .arg(measured->sha256)
             .arg(parsed.record.onnxBytes)
             .arg(parsed.record.onnxSha256));
    return false;
  }

  // The identity is re-derived from the facts on disk. A manifest whose id,
  // digest and declared profile no longer agree is a manifest that describes
  // something other than this row, and it is refused rather than trusted.
  const QString derivedId = userModelIdFor(
      computeContentId(parsed.record.role, parsed.record.onnxSha256, parsed.record.actualProfile));
  if (derivedId != parsed.record.id) {
    fail(reasonKey, reason, QStringLiteral("model.store.identity"),
         QStringLiteral("%1 claims id %2, but its digest and declared profile derive %3")
             .arg(manifestPath, parsed.record.id, derivedId));
    return false;
  }
  if (requirePublishedName) {
    const QString token = contentToken(computeContentId(
        parsed.record.role, parsed.record.onnxSha256, parsed.record.actualProfile));
    if (token.isEmpty() || directoryInfo.fileName().compare(token, Qt::CaseInsensitive) != 0) {
      fail(reasonKey, reason, QStringLiteral("model.store.identity"),
           QStringLiteral("%1 is not the directory its content digest names").arg(directory));
      return false;
    }
  }

  if (object.contains(QStringLiteral("reference"))) {
    const QJsonValue referenceValue = object.value(QStringLiteral("reference"));
    if (!referenceValue.isObject()) {
      fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
           QStringLiteral("%1 has an unusable reference").arg(manifestPath));
      return false;
    }
    const QJsonObject referenceObject = referenceValue.toObject();
    bool ran = false;
    qint64 scale = 0;
    QSize input;
    QSize output;
    if (!noUnknownKeys(referenceObject, referenceKeys()) ||
        !jsonBool(referenceObject, QStringLiteral("ran"), &ran) || !ran ||
        !jsonInt64(referenceObject, QStringLiteral("scale"), &scale) || scale <= 0 ||
        !referenceObject.value(QStringLiteral("in")).isObject() ||
        !referenceObject.value(QStringLiteral("out")).isObject() ||
        !referenceObject.value(QStringLiteral("profile")).isObject()) {
      fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
           QStringLiteral("%1 has an unusable reference block").arg(manifestPath));
      return false;
    }
    QString geometryError;
    if (!readGeometry(referenceObject.value(QStringLiteral("in")).toObject(), &input,
                      &geometryError) ||
        !readGeometry(referenceObject.value(QStringLiteral("out")).toObject(), &output,
                      &geometryError)) {
      fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
           QStringLiteral("%1: %2").arg(manifestPath, geometryError));
      return false;
    }
    // The concrete boundary the run produced, separate evidence from the
    // declaration. It must satisfy the candidate rule and agree with the
    // declaration it was published for, and its geometry must be the geometry
    // the import resolved. A tampered N/C or geometry is refused here rather
    // than trusted.
    QString profileError;
    ModelProfile reference;
    if (!profileFromJson(referenceObject.value(QStringLiteral("profile")).toObject(), &reference,
                         &profileError)) {
      fail(reasonKey, reason, QStringLiteral("model.store.manifest"),
           QStringLiteral("%1: %2").arg(manifestPath, profileError));
      return false;
    }
    const QString proofRefusal = spatialReferenceRefusal(parsed.record.actualProfile, reference);
    if (!proofRefusal.isEmpty()) {
      fail(reasonKey, reason, QStringLiteral("model.store.proof"),
           QStringLiteral("%1: the reference profile is not consistent with the declared "
                          "boundary: %2")
               .arg(manifestPath, proofRefusal));
      return false;
    }
    if (!reference.input.hasGeometry() || !reference.output.hasGeometry() ||
        QSize(static_cast<int>(*reference.input.width),
              static_cast<int>(*reference.input.height)) != input ||
        QSize(static_cast<int>(*reference.output.width),
              static_cast<int>(*reference.output.height)) != output) {
      fail(reasonKey, reason, QStringLiteral("model.store.proof"),
           QStringLiteral("%1: the reference profile geometry is not the geometry the import "
                          "resolved")
               .arg(manifestPath));
      return false;
    }
    const std::optional<int> proofScale = reference.derivedUniformScale();
    if (!proofScale.has_value() || *proofScale != scale) {
      fail(reasonKey, reason, QStringLiteral("model.store.proof"),
           QStringLiteral("%1: the reference profile's scale is not the recorded scale")
               .arg(manifestPath));
      return false;
    }
    StageResolution resolution;
    resolution.input = input;
    resolution.output = output;
    resolution.scale = static_cast<int>(scale);
    resolution.fromCpuReference = true;
    resolution.verifiedReferenceProfile = reference;
    parsed.importResolution = resolution;
    parsed.record.verifiedReferenceProfile = reference;
  }

  parsed.record.onnxPath = canonicalStored;
  parsed.storedPath = canonicalStored;
  *model = parsed;
  clearReason(reasonKey, reason);
  return true;
}

}  // namespace

QString sha256HexOf(const QByteArray& bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString ModelInventory::manifestFileName() { return QString::fromLatin1(kManifestFileName); }

QString ModelInventory::modelFileName() { return QString::fromLatin1(kModelFileName); }

QString ModelInventory::defaultRoot() { return nativeSubdirectory(QStringLiteral("models")); }

ModelInventory::ModelInventory(QString root) : root_(std::move(root)) {}

bool ModelInventory::isOwned(QString* reason) const {
  // Ownership is containment in the app-owned models hierarchy, decided on
  // canonical paths. A root anywhere else - a user's model folder, a shared
  // drive, a borrowed reference tree - is not writable and not a cache, and no
  // argument turns it into one.
  QString ownedReason;
  const QString ownedRoot = nativeSubdirectory(QStringLiteral("models"), &ownedReason);
  if (ownedRoot.isEmpty()) {
    if (reason) {
      *reason =
          QStringLiteral("the app-owned models directory cannot be resolved: %1").arg(ownedReason);
    }
    return false;
  }
  if (root_.isEmpty()) {
    if (reason) *reason = QStringLiteral("no model store root was supplied");
    return false;
  }
  // The lexical half: the root must at least be *named* inside the app-owned
  // models tree. A store pointed at a user's model folder, a shared drive or a
  // borrowed reference tree fails here, whatever it resolves to.
  if (!pathWithinRoot(ownedRoot, root_)) {
    if (reason) {
      *reason = QStringLiteral("model store root %1 is outside the app-owned %2")
                    .arg(root_, ownedRoot);
    }
    return false;
  }

  const QString canonical = canonicalPath(root_);
  if (!canonical.isEmpty()) {
    // The canonical half for a root that exists: containment is decided on the
    // resolved path, and the resolved root itself may not be a reparse point.
    if (!QFileInfo(root_).isDir()) {
      if (reason) *reason = QStringLiteral("model store root %1 is not a directory").arg(root_);
      return false;
    }
    if (!pathWithinRoot(ownedRoot, canonical)) {
      if (reason) {
        *reason = QStringLiteral("model store root %1 is outside the app-owned %2")
                      .arg(canonical, ownedRoot);
      }
      return false;
    }
    if (!pathHasNoReparsePoints(canonical, canonical)) {
      if (reason) *reason = QStringLiteral("model store root %1 is a reparse point").arg(canonical);
      return false;
    }
    clearReason(nullptr, reason);
    return true;
  }

  // The root does not exist yet - the default owned store before its first write.
  // That is still an owned root when the deepest ancestor that does exist is an
  // ancestor of the app-owned models tree, resolves, and is not a reparse point,
  // and no existing component between it and the root is one. The missing tail can
  // then only be created inside the app-owned tree, while a root whose existing
  // part resolves elsewhere - a junction inside a user's folder, say - is refused
  // exactly as an existing foreign root is.
  const QString existing = nearestExistingAncestor(root_);
  const QString ownedExisting = nearestExistingAncestor(ownedRoot);
  const QString canonicalExisting = canonicalPath(existing);
  const bool underOwnedTree = !existing.isEmpty() && !ownedExisting.isEmpty() &&
                              !canonicalExisting.isEmpty() &&
                              pathWithinRoot(ownedExisting, existing) &&
                              pathHasNoReparsePoints(ownedExisting, existing) &&
                              pathHasNoReparsePoints(ownedRoot, root_);
  if (!underOwnedTree) {
    if (reason) {
      *reason = QStringLiteral("model store root %1 does not exist and its existing part is not "
                               "inside the app-owned %2")
                    .arg(root_, ownedRoot);
    }
    return false;
  }
  clearReason(nullptr, reason);
  return true;
}

bool ModelInventory::ensureOwned(QString* reason) const {
  if (!isOwned(reason)) return false;
  if (QFileInfo(root_).isDir()) {
    clearReason(nullptr, reason);
    return true;
  }
  if (!QDir().mkpath(root_)) {
    if (reason) *reason = QStringLiteral("model store root %1 could not be created").arg(root_);
    return false;
  }
  clearReason(nullptr, reason);
  return true;
}

ModelImportOutcome ModelInventory::importFile(const QString& sourcePath,
                                              const InspectionRuntime& runtime,
                                              const QSize& stageGeometry,
                                              const BackendAvailability& backends,
                                              const QString& termsNote,
                                              ImportCancellation* cancellation) {
  ModelImportOutcome outcome;
  const auto refuse = [&outcome](const QString& key, const QString& text) {
    outcome.ok = false;
    outcome.published = false;
    outcome.reasonKey = key;
    outcome.reason = text;
    return outcome;
  };

  const QFileInfo sourceInfo(QDir::cleanPath(sourcePath));
  if (sourcePath.isEmpty() || !sourceInfo.isAbsolute()) {
    return refuse(QStringLiteral("model.source.absolute"),
                  QStringLiteral("the model path '%1' is not an absolute path").arg(sourcePath));
  }
  if (!sourceInfo.isFile()) {
    return refuse(QStringLiteral("model.source.missing"),
                  QStringLiteral("%1 does not exist or is not a regular file").arg(sourcePath));
  }
  const QString canonicalSource = sourceInfo.canonicalFilePath();
  if (canonicalSource.isEmpty()) {
    return refuse(QStringLiteral("model.source.missing"),
                  QStringLiteral("%1 could not be resolved").arg(sourcePath));
  }

  QFile file(canonicalSource);
  if (!file.open(QIODevice::ReadOnly)) {
    return refuse(QStringLiteral("model.source.unreadable"),
                  QStringLiteral("%1 cannot be read: %2")
                      .arg(canonicalSource, file.errorString()));
  }
  const QByteArray bytes = file.readAll();
  if (file.error() != QFile::NoError) {
    return refuse(QStringLiteral("model.source.unreadable"),
                  QStringLiteral("%1 could not be read: %2")
                      .arg(canonicalSource, file.errorString()));
  }
  file.close();

  if (cancellation != nullptr && cancellation->isCancelled()) {
    // A cancel that arrived before the graph was inspected: the CPU reference is
    // never run and the store is never touched.
    return refuse(QStringLiteral("native.import.cancelled"),
                  QStringLiteral("the import was cancelled before inspection began"));
  }

  const InspectionResult inspection = inspectGraph(runtime, bytes, stageGeometry);
  if (!inspection.ok) {
    // A refusal publishes nothing: a truncated graph, two inputs, a rank-5
    // boundary or a non-uniform declared scale leaves the store untouched.
    return refuse(inspection.reasonKey, inspection.reason);
  }

  // The publication gate: the one atomic step that commits the import. If a
  // cancel won the race while the graph was being inspected, nothing is written
  // and the outcome says so; if this call passes, the copy is committed and a
  // later cancel is refused rather than re-labelling a committed row.
  if (cancellation != nullptr && !cancellation->beginPublication()) {
    return refuse(QStringLiteral("native.import.cancelled"),
                  QStringLiteral("the import was cancelled before anything was published"));
  }

  const QString displayName = sourceInfo.completeBaseName();
  return publish(bytes, inspection, canonicalSource, displayName, backends, termsNote);
}

ModelImportOutcome ModelInventory::publish(const QByteArray& bytes,
                                           const InspectionResult& inspection,
                                           const QString& sourcePath, const QString& displayName,
                                           const BackendAvailability& backends,
                                           const QString& termsNote) {
  ModelImportOutcome outcome;
  const auto refuse = [&outcome](const QString& key, const QString& text) {
    outcome.ok = false;
    outcome.published = false;
    outcome.reasonKey = key;
    outcome.reason = text;
    return outcome;
  };

  if (!inspection.ok) return refuse(inspection.reasonKey, inspection.reason);
  if (bytes.isEmpty()) {
    return refuse(QStringLiteral("model.bytes.empty"),
                  QStringLiteral("there are no graph bytes to publish"));
  }

  const QString digest = sha256HexOf(bytes);
  const ModelProfile declared = inspection.declared;
  const ModelRole role = ModelRole::Spatial;

  const bool fp16 = isEligibleIoType(declared.input.dtype) &&
                    isEligibleIoType(declared.output.dtype);
  AvailabilityState availability;
  std::optional<StageResolution> importResolution;
  std::optional<ModelProfile> verifiedReferenceProfile;
  if (!fp16) {
    // The dtype is the only thing wrong with this boundary: record the row as
    // unsupported, naming the observed dtype, and run nothing. The candidate
    // rule is used so a symbolic output batch in an unsupported dtype is still
    // recorded as unsupported rather than refused.
    ModelProfile fp16Sample = declared;
    fp16Sample.input.dtype = QString::fromLatin1(kEligibleIoType);
    fp16Sample.output.dtype = QString::fromLatin1(kEligibleIoType);
    // Only the dtype may be wrong: every other boundary rule must still hold, so
    // the sample carrying the eligible dtype passes the candidate rule. The
    // recorded reason names the dtype the graph actually declared.
    if (!spatialCandidateRefusal(fp16Sample).isEmpty()) {
      return refuse(QStringLiteral("model.profile.ineligible"), spatialCandidateRefusal(declared));
    }
    availability = unavailableAvailability(QStringLiteral("model.io.unsupported"),
                                            spatialCandidateRefusal(declared));
  } else {
    const QString candidateRefusal = spatialCandidateRefusal(declared);
    if (!candidateRefusal.isEmpty()) {
      return refuse(QStringLiteral("model.profile.ineligible"), candidateRefusal);
    }
    // The strict rule, and whether the declaration alone can vouch for the row.
    // A symbolic output batch passes only the candidate rule; an unresolved
    // geometry knows no H/W at import time. Both need the concrete boundary a
    // real CPU reference run produced before the row may be offered.
    const bool strictEligible = spatialEligibilityRefusal(declared).isEmpty();
    const bool needsReference = declared.isDynamicGeometry() || !strictEligible;
    if (!needsReference) {
      availability = readyAvailability();
    } else {
      QString referenceKey;
      QString referenceReason;
      const std::optional<ModelProfile> reference =
          referenceProofFrom(declared, inspection, &referenceKey, &referenceReason);
      if (!reference.has_value()) {
        availability = unavailableAvailability(referenceKey, referenceReason);
      } else {
        availability = readyAvailability();
        verifiedReferenceProfile = reference;
        StageResolution resolution;
        resolution.input = QSize(static_cast<int>(*reference->input.width),
                                 static_cast<int>(*reference->input.height));
        resolution.output = QSize(static_cast<int>(*reference->output.width),
                                  static_cast<int>(*reference->output.height));
        resolution.scale = *reference->derivedUniformScale();
        resolution.fromCpuReference = true;
        resolution.verifiedReferenceProfile = reference;
        importResolution = resolution;
      }
    }
  }

  const QString contentId = computeContentId(role, digest, declared);
  const QString token = contentToken(contentId);
  if (token.isEmpty()) {
    return refuse(QStringLiteral("model.identity.invalid"),
                  QStringLiteral("the graph's content identity did not compute"));
  }
  // Nothing has been created yet: a refused graph above this point leaves the
  // filesystem exactly as it was, including a store root that did not exist.
  QString ownedReason;
  if (!ensureOwned(&ownedReason)) {
    return refuse(QStringLiteral("model.store.unowned"),
                  QStringLiteral("model store root %1 is not app-owned: %2")
                      .arg(root_, ownedReason));
  }

  ModelRecord record;
  record.id = userModelIdFor(contentId);
  record.displayName = displayName.isEmpty() ? record.id : displayName;
  record.origin = ModelOrigin::UserImported;
  record.role = role;
  record.onnxSha256 = digest;
  record.onnxBytes = bytes.size();
  record.actualProfile = declared;
  // The concrete boundary the run produced is separate evidence; the id and the
  // declared profile above remain exactly what the graph stated.
  record.verifiedReferenceProfile = verifiedReferenceProfile;
  // Model compatibility, kept apart from runtime presence. What the provisioner
  // supplies is the only claim about this model that exists, so it is exactly what
  // the row records: an empty supply is not a licence to claim this build's
  // vocabulary, and a model vouched for on no backend is planned on none. A later
  // import of these same bytes may raise the claim (see updateRecordedSupport);
  // which backends the runtime actually carries is a separate fact the planner is
  // given, and a backend it no longer carries is refused there.
  record.availableBackends = backends.available;
  record.termsNote = termsNote;
  record.availability = availability;

  const QString finalDirectory = QDir(root_).filePath(token);
  if (QFileInfo(finalDirectory).exists()) {
    StoredModel existing;
    QString existingKey;
    QString existingReason;
    if (readStored(root_, finalDirectory, &existing, &existingKey, &existingReason, true) &&
        existing.record.onnxSha256 == digest) {
      // These exact bytes are already published under this identity, so nothing is
      // written again. A provisioner may, however, now vouch for a backend the row
      // was not published with, and that single claim is updated in the row's own
      // document rather than frozen at whatever was known first. A supply that
      // names no backend says nothing new, so it leaves the recorded support alone
      // instead of erasing it.
      if (!backends.available.isEmpty() &&
          !sameBackendSet(existing.record.availableBackends, backends.available)) {
        QString updateKey;
        QString updateReason;
        if (!updateRecordedSupport(finalDirectory, existing, backends.available, &updateKey,
                                   &updateReason)) {
          return refuse(updateKey, updateReason);
        }
        StoredModel reread;
        QString rereadKey;
        QString rereadReason;
        if (!readStored(root_, finalDirectory, &reread, &rereadKey, &rereadReason, true)) {
          return refuse(rereadKey, rereadReason);
        }
        existing = reread;
      }
      outcome.ok = existing.record.availability.isUsable();
      outcome.published = false;
      outcome.model = existing;
      outcome.reasonKey = existing.record.availability.reasonKey;
      outcome.reason = existing.record.availability.reason;
      return outcome;
    }
    // An unverified directory is never overwritten or removed: proof is
    // required to delete, and this module does not delete at all.
    return refuse(QStringLiteral("model.store.existsUnverified"),
                  QStringLiteral("%1 already exists and does not verify: %2")
                      .arg(finalDirectory, existingReason));
  }

  const QString stagingDirectory = QDir(root_).filePath(
      QStringLiteral("%1%2").arg(QString::fromLatin1(kStagingPrefix),
                                 QUuid::createUuid().toString(QUuid::WithoutBraces)));
  if (!QDir().mkpath(stagingDirectory)) {
    return refuse(QStringLiteral("model.publish.staging"),
                  QStringLiteral("staging directory %1 could not be created").arg(stagingDirectory));
  }
  const auto abandon = [&outcome, &stagingDirectory](const QString& key, const QString& text) {
    QDir(stagingDirectory).removeRecursively();
    outcome.ok = false;
    outcome.published = false;
    outcome.reasonKey = key;
    outcome.reason = text;
    return outcome;
  };

  // The published copy is written from the bytes that were inspected, never by
  // copying the source path: a file that changes mid-import cannot make the store
  // and the identity disagree.
  const QString stagedModel = QDir(stagingDirectory).filePath(modelFileName());
  {
    QFile file(stagedModel);
    if (!file.open(QIODevice::WriteOnly)) {
      return abandon(QStringLiteral("model.publish.write"),
                     QStringLiteral("%1 could not be written: %2")
                         .arg(stagedModel, file.errorString()));
    }
    if (file.write(bytes) != bytes.size() || !file.flush()) {
      return abandon(QStringLiteral("model.publish.write"),
                     QStringLiteral("%1 could not be written: %2")
                         .arg(stagedModel, file.errorString()));
    }
    file.close();
    if (file.error() != QFile::NoError) {
      return abandon(QStringLiteral("model.publish.write"),
                     QStringLiteral("%1 could not be written: %2")
                         .arg(stagedModel, file.errorString()));
    }
  }

  QString stagedReason;
  const std::optional<FileDigest> staged = measureFile(stagedModel, &stagedReason);
  if (!staged.has_value()) {
    return abandon(QStringLiteral("model.publish.digest"), stagedReason);
  }
  if (staged->sha256 != digest || staged->bytes != bytes.size()) {
    return abandon(QStringLiteral("model.publish.digest"),
                   QStringLiteral("the staged copy is %1 bytes hashing %2, not %3 bytes hashing %4")
                       .arg(staged->bytes)
                       .arg(staged->sha256)
                       .arg(bytes.size())
                       .arg(digest));
  }

  const QString stagedManifest = QDir(stagingDirectory).filePath(manifestFileName());
  {
    const QByteArray payload =
        QJsonDocument(manifestFor(record, sourcePath, importResolution)).toJson(QJsonDocument::Compact);
    QSaveFile file(stagedManifest);
    if (!file.open(QIODevice::WriteOnly)) {
      return abandon(QStringLiteral("model.publish.manifest"),
                     QStringLiteral("%1 could not be written: %2")
                         .arg(stagedManifest, file.errorString()));
    }
    if (file.write(payload) != payload.size() || !file.commit()) {
      return abandon(QStringLiteral("model.publish.manifest"),
                     QStringLiteral("%1 could not be written").arg(stagedManifest));
    }
  }

  {
    StoredModel readBack;
    QString readKey;
    QString readReason;
    if (!readStored(root_, stagingDirectory, &readBack, &readKey, &readReason, false)) {
      return abandon(QStringLiteral("model.publish.manifest"),
                     QStringLiteral("the staged manifest does not read back: %1").arg(readReason));
    }
  }

  if (!QDir().rename(stagingDirectory, finalDirectory)) {
    return abandon(QStringLiteral("model.publish.rename"),
                   QStringLiteral("staging directory %1 could not be moved into place")
                       .arg(stagingDirectory));
  }

  StoredModel stored;
  QString storedKey;
  QString storedReason;
  if (!readStored(root_, finalDirectory, &stored, &storedKey, &storedReason, true)) {
    // The copy is on disk but does not verify; that is reported, not hidden.
    outcome.ok = false;
    outcome.published = true;
    outcome.reasonKey = storedKey;
    outcome.reason = storedReason;
    return outcome;
  }
  outcome.ok = stored.record.availability.isUsable();
  outcome.published = true;
  outcome.model = stored;
  outcome.reasonKey = stored.record.availability.reasonKey;
  outcome.reason = stored.record.availability.reason;
  return outcome;
}

QVector<StoredModel> ModelInventory::stored(QString* reason) const {
  QVector<StoredModel> rows;
  if (reason) reason->clear();
  // The app-owned store is owned before its first write, so a root that does not
  // exist yet reads as the empty inventory it is - not as a missing one, and
  // never with an error the user cannot act on. Only a root that cannot be an
  // owned store at all - foreign, walking out of the tree, reparse-pointed, or an
  // existing non-directory - is refused, and its ownership reason names which.
  if (!isOwned(reason)) return rows;
  // An owned root the app has not created yet is the empty inventory it is, not
  // a missing store: clearing the reason here keeps that decision local, so no
  // ownership text can ride out of a root that is merely uninitialized.
  if (!QFileInfo(root_).isDir()) {
    clearReason(nullptr, reason);
    return rows;
  }

  const QFileInfoList entries =
      QDir(root_).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo& entry : entries) {
    const QString token = entry.fileName();
    // Anything whose name is not a content digest is not one of ours: a staging
    // directory left by a crashed write is simply not a row.
    if (!isSha256Hex(token)) continue;

    StoredModel model;
    QString key;
    QString rowReason;
    if (readStored(root_, entry.absoluteFilePath(), &model, &key, &rowReason, true)) {
      rows.append(model);
      continue;
    }

    // A directory that no longer verifies is still reported, as a row that is
    // unavailable for that reason. It is never dropped and never deleted.
    StoredModel broken;
    broken.record.id = userModelIdFor(QStringLiteral("sha256:%1").arg(token));
    broken.record.displayName = token;
    broken.record.origin = ModelOrigin::UserImported;
    broken.record.role = ModelRole::Spatial;
    const QString candidate = QDir(entry.absoluteFilePath()).filePath(modelFileName());
    QString ignoredReason;
    if (isRegularOwnedFile(candidate, root_, nullptr, &ignoredReason)) {
      const std::optional<FileDigest> measured = measureFile(candidate, nullptr);
      if (measured.has_value()) {
        broken.record.onnxSha256 = measured->sha256;
        broken.record.onnxBytes = measured->bytes;
      }
    }
    broken.storedPath = candidate;
    broken.record.availability = unavailableAvailability(
        key.isEmpty() ? QStringLiteral("model.store.corrupt") : key, rowReason);
    rows.append(broken);
  }
  return rows;
}

bool ModelInventory::verifyOwnedModelIntegrity(const StoredModel& model, QString* reasonKey,
                                               QString* reason) const {
  // The original import path is provenance and may be gone; only the immutable
  // copy inside the owned store can be proved. Its bytes must still hash to the
  // identity the row was published under, or a mutated copy would keep resolving
  // the geometry it no longer contains.
  if (model.storedPath.isEmpty()) {
    fail(reasonKey, reason, QStringLiteral("model.owned.missing"),
         QStringLiteral("model %1 names no owned copy to verify").arg(model.record.id));
    return false;
  }
  if (!isSha256Hex(model.record.onnxSha256)) {
    fail(reasonKey, reason, QStringLiteral("model.identity.missing"),
         QStringLiteral("model %1 has no recorded digest to compare against").arg(model.record.id));
    return false;
  }
  if (!isRegularOwnedFile(model.storedPath, root_, reasonKey, reason)) return false;

  QString hashReason;
  const std::optional<FileDigest> measured = measureFile(model.storedPath, &hashReason);
  if (!measured.has_value()) {
    fail(reasonKey, reason, QStringLiteral("model.owned.changed"), hashReason);
    return false;
  }
  if (measured->sha256 != model.record.onnxSha256 || measured->bytes != model.record.onnxBytes) {
    fail(reasonKey, reason, QStringLiteral("model.owned.changed"),
         QStringLiteral("%1 now is %2 bytes hashing %3, but it was published as %4 bytes hashing %5")
             .arg(model.storedPath)
             .arg(measured->bytes)
             .arg(measured->sha256)
             .arg(model.record.onnxBytes)
             .arg(model.record.onnxSha256));
    return false;
  }
  // The id must still derive from the digest and declared profile on disk, so a
  // hand-edited manifest cannot repoint verification at another record.
  const QString derivedId = userModelIdFor(
      computeContentId(model.record.role, model.record.onnxSha256, model.record.actualProfile));
  if (derivedId != model.record.id) {
    fail(reasonKey, reason, QStringLiteral("model.owned.changed"),
         QStringLiteral("%1 claims id %2, but its digest and declared profile derive %3")
             .arg(model.storedPath, model.record.id, derivedId));
    return false;
  }
  clearReason(reasonKey, reason);
  return true;
}

}  // namespace native
}  // namespace mplayer
