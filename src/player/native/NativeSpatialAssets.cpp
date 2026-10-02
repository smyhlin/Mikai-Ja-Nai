// The app-owned source-asset store. See NativeSpatialAssets.hpp for the rules;
// this file is their one implementation.
//
// The catalogue is the acquisition record's, copied to the byte: five pinned
// graphs, their observed digests and lengths, and the licence those bytes carry.
// Nothing here fetches anything and nothing here executes anything - a graph is
// verified and stored, and that is the whole job.

#include "NativeSpatialAssets.hpp"

#include <cmath>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QSaveFile>
#include <QtCore/QUuid>

#include "NativeArtifactStore.hpp"
#include "NativePaths.hpp"

namespace mplayer {
namespace native {

namespace {

constexpr const char* kSchema = "mikai.native.spatial-asset/1";
constexpr const char* kManifestFileName = "manifest.json";
constexpr const char* kGraphFileName = "graph.onnx";
constexpr const char* kNoticeFileName = "NOTICE.txt";
constexpr const char* kStagingPrefix = ".staging-";
constexpr const char* kCreator = "the-database/mpv-AnimeJaNai";
constexpr const char* kRepositoryUrl = "https://github.com/the-database/mpv-AnimeJaNai";
constexpr const char* kCommit = "d9cbaae5e92a56e71fdf4da5e063ae3e2611e5f7";
constexpr const char* kOnnxPath = "BuildMpvUpscale2xAnimeJaNai/mpv-upscale-2x_animejanai/animejanai/onnx";

QString sha256HexOfBytes(const QByteArray& bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

void fail(QString* reasonKey, QString* reason, const QString& key, const QString& text) {
  if (reasonKey) *reasonKey = key;
  if (reason) *reason = text;
}

void clearReason(QString* reasonKey, QString* reason) {
  if (reasonKey) reasonKey->clear();
  if (reason) reason->clear();
}

QString pinnedUrl(const QString& name) {
  return QStringLiteral("https://raw.githubusercontent.com/the-database/mpv-AnimeJaNai/%1/%2/%3")
      .arg(QString::fromLatin1(kCommit), QString::fromLatin1(kOnnxPath), name);
}

QString assetIdFor(const QString& sha256) { return QStringLiteral("sha256:") + sha256; }

QString contentToken(const QString& id) {
  if (!id.startsWith(QLatin1String("sha256:"))) return {};
  const QString hex = id.mid(7);
  return isSha256Hex(hex) ? hex : QString();
}

SpatialAsset makeAsset(const QString& name, qint64 bytes, const QString& sha256) {
  SpatialAsset asset;
  asset.name = name;
  asset.bytes = bytes;
  asset.sha256 = sha256;
  asset.id = assetIdFor(sha256);
  asset.commit = QString::fromLatin1(kCommit);
  asset.sourcePath = QString::fromLatin1(kOnnxPath) + QLatin1Char('/') + name;
  asset.url = pinnedUrl(name);
  return asset;
}

// ------------------------------------------------------------- json reading

bool jsonInt64(const QJsonObject& object, const QString& key, qint64* out) {
  const QJsonValue value = object.value(key);
  if (!value.isDouble()) return false;
  const double d = value.toDouble();
  if (!std::isfinite(d) || d < 0.0) return false;
  *out = static_cast<qint64>(d);
  return true;
}

bool jsonString(const QJsonObject& object, const QString& key, QString* out) {
  const QJsonValue value = object.value(key);
  if (!value.isString()) return false;
  *out = value.toString();
  return true;
}

bool noUnknownKeys(const QJsonObject& object, const QStringList& known) {
  for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
    if (!known.contains(it.key())) return false;
  }
  return true;
}

QStringList sourceKeys() {
  return {QStringLiteral("commit"), QStringLiteral("url"), QStringLiteral("path")};
}

QStringList licenseKeys() {
  return {QStringLiteral("spdx"), QStringLiteral("name"), QStringLiteral("url"),
          QStringLiteral("legalCodeUrl"), QStringLiteral("notice"), QStringLiteral("useScope")};
}

QStringList manifestKeys() {
  return {QStringLiteral("schema"),  QStringLiteral("id"),       QStringLiteral("name"),
          QStringLiteral("sha256"),  QStringLiteral("bytes"),    QStringLiteral("source"),
          QStringLiteral("creator"), QStringLiteral("license"),  QStringLiteral("acquiredAt")};
}

QJsonObject sourceToJson(const SpatialAsset& asset) {
  QJsonObject source;
  source.insert(QStringLiteral("commit"), asset.commit);
  source.insert(QStringLiteral("url"), asset.url);
  source.insert(QStringLiteral("path"), asset.sourcePath);
  return source;
}

QJsonObject licenseToJson(const SpatialAssetLicense& license) {
  QJsonObject object;
  object.insert(QStringLiteral("spdx"), license.spdx);
  object.insert(QStringLiteral("name"), license.name);
  object.insert(QStringLiteral("url"), license.url);
  object.insert(QStringLiteral("legalCodeUrl"), license.legalCodeUrl);
  object.insert(QStringLiteral("notice"), license.notice);
  object.insert(QStringLiteral("useScope"), license.useScope);
  return object;
}

bool sourceFromJson(const QJsonObject& object, QString* commit, QString* url, QString* path,
                    QString* error) {
  if (!noUnknownKeys(object, sourceKeys())) {
    *error = QStringLiteral("the source record carries a key this schema does not define");
    return false;
  }
  if (!jsonString(object, QStringLiteral("commit"), commit) ||
      !jsonString(object, QStringLiteral("url"), url) ||
      !jsonString(object, QStringLiteral("path"), path) || commit->isEmpty() || url->isEmpty()) {
    *error = QStringLiteral("the source record is incomplete");
    return false;
  }
  return true;
}

bool licenseFromJson(const QJsonObject& object, QString* spdx, QString* error) {
  if (!noUnknownKeys(object, licenseKeys())) {
    *error = QStringLiteral("the licence record carries a key this schema does not define");
    return false;
  }
  if (!jsonString(object, QStringLiteral("spdx"), spdx) || spdx->isEmpty()) {
    *error = QStringLiteral("the licence record names no licence");
    return false;
  }
  return true;
}

// --------------------------------------------------------------- read/verify

/// Reads and verifies one published directory. `requirePublishedName` is true
/// for a directory the store owns; staging is read back before it is renamed, so
/// its name is not the digest yet.
bool readStored(const QString& root, const QString& directory, StoredSpatialAsset* out,
                QString* reasonKey, QString* reason, bool requirePublishedName) {
  const QFileInfo directoryInfo(directory);
  if (!directoryInfo.isDir() || !pathWithinRoot(root, directory) ||
      !pathHasNoReparsePoints(root, directory)) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.directory"),
         QStringLiteral("%1 is not an owned directory").arg(directory));
    return false;
  }

  const QString manifestPath = QDir(directory).filePath(QString::fromLatin1(kManifestFileName));
  QFile manifestFile(manifestPath);
  if (!manifestFile.open(QIODevice::ReadOnly)) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.manifest"),
         QStringLiteral("%1 cannot be read: %2").arg(manifestPath, manifestFile.errorString()));
    return false;
  }
  const QByteArray payload = manifestFile.readAll();
  if (manifestFile.error() != QFile::NoError) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.manifest"),
         QStringLiteral("%1 could not be read: %2").arg(manifestPath, manifestFile.errorString()));
    return false;
  }
  manifestFile.close();

  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.manifest"),
         QStringLiteral("%1 is not a JSON object: %2")
             .arg(manifestPath, parseError.errorString()));
    return false;
  }
  const QJsonObject object = document.object();
  if (!noUnknownKeys(object, manifestKeys())) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.manifest"),
         QStringLiteral("%1 carries a key this schema does not define").arg(manifestPath));
    return false;
  }

  QString schema;
  if (!jsonString(object, QStringLiteral("schema"), &schema) || schema != QLatin1String(kSchema)) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.schema"),
         QStringLiteral("%1 has schema '%2', not %3")
             .arg(manifestPath, schema, QString::fromLatin1(kSchema)));
    return false;
  }

  SpatialAsset asset;
  if (!jsonString(object, QStringLiteral("id"), &asset.id) || contentToken(asset.id).isEmpty()) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.identity"),
         QStringLiteral("%1 has no usable asset id").arg(manifestPath));
    return false;
  }
  if (!jsonString(object, QStringLiteral("name"), &asset.name) ||
      !jsonString(object, QStringLiteral("sha256"), &asset.sha256) ||
      !isSha256Hex(asset.sha256)) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.manifest"),
         QStringLiteral("%1 records no usable digest").arg(manifestPath));
    return false;
  }
  if (asset.id != assetIdFor(asset.sha256)) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.identity"),
         QStringLiteral("%1 records id %2 for digest %3")
             .arg(manifestPath, asset.id, asset.sha256));
    return false;
  }
  if (!jsonInt64(object, QStringLiteral("bytes"), &asset.bytes) || asset.bytes <= 0) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.manifest"),
         QStringLiteral("%1 records no usable length").arg(manifestPath));
    return false;
  }
  if (requirePublishedName) {
    const QString expected = SpatialAssetStore::directoryName(asset.id);
    if (expected.isEmpty() ||
        QFileInfo(directory).fileName().compare(expected, Qt::CaseInsensitive) != 0) {
      fail(reasonKey, reason, QStringLiteral("spatial.asset.identity"),
           QStringLiteral("%1 does not carry the digest's directory name").arg(directory));
      return false;
    }
  }

  const QJsonValue sourceValue = object.value(QStringLiteral("source"));
  QString commit;
  QString url;
  QString path;
  QString sourceError;
  if (!sourceValue.isObject() ||
      !sourceFromJson(sourceValue.toObject(), &commit, &url, &path, &sourceError)) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.manifest"),
         sourceError.isEmpty()
             ? QStringLiteral("%1 has no source record").arg(manifestPath)
             : QStringLiteral("%1: %2").arg(manifestPath, sourceError));
    return false;
  }
  asset.commit = commit;
  asset.url = url;
  asset.sourcePath = path;

  QString creator;
  if (!jsonString(object, QStringLiteral("creator"), &creator) || creator.isEmpty()) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.manifest"),
         QStringLiteral("%1 records no creator").arg(manifestPath));
    return false;
  }
  const QJsonValue licenseValue = object.value(QStringLiteral("license"));
  QString spdx;
  if (!licenseValue.isObject() ||
      !licenseFromJson(licenseValue.toObject(), &spdx, &sourceError)) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.manifest"),
         sourceError.isEmpty()
             ? QStringLiteral("%1 has no licence record").arg(manifestPath)
             : QStringLiteral("%1: %2").arg(manifestPath, sourceError));
    return false;
  }

  const QString graphPath = QDir(directory).filePath(QString::fromLatin1(kGraphFileName));
  const QFileInfo graphInfo(graphPath);
  if (!graphInfo.isFile() || !pathWithinRoot(root, graphPath) ||
      !pathHasNoReparsePoints(root, graphPath)) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.graph"),
         QStringLiteral("%1 is not an owned regular file").arg(graphPath));
    return false;
  }
  QString measureReason;
  const std::optional<FileDigest> measured = measureFile(graphPath, &measureReason);
  if (!measured.has_value()) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.graph"), measureReason);
    return false;
  }
  if (measured->sha256 != asset.sha256 || measured->bytes != asset.bytes) {
    fail(reasonKey, reason, QStringLiteral("spatial.asset.graph"),
         QStringLiteral("%1 is %2 bytes hashing %3, not %4 bytes hashing %5")
             .arg(graphPath)
             .arg(measured->bytes)
             .arg(measured->sha256)
             .arg(asset.bytes)
             .arg(asset.sha256));
    return false;
  }

  out->asset = asset;
  out->directory = directory;
  out->graphPath = graphPath;
  out->verified = true;
  clearReason(reasonKey, reason);
  return true;
}

}  // namespace

// ------------------------------------------------------------------ catalogue

QString spatialAssetCommit() { return QString::fromLatin1(kCommit); }

QString spatialAssetRepository() { return QString::fromLatin1(kRepositoryUrl); }

SpatialAssetLicense spatialAssetLicense() {
  SpatialAssetLicense license;
  license.spdx = QStringLiteral("CC-BY-NC-SA-4.0");
  license.name = QStringLiteral(
      "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International");
  license.url = QStringLiteral("https://creativecommons.org/licenses/by-nc-sa/4.0/");
  license.legalCodeUrl = QStringLiteral("https://creativecommons.org/licenses/by-nc-sa/4.0/legalcode");
  license.useScope = QStringLiteral(
      "Private, non-commercial reference on this machine. This application does not "
      "redistribute, re-host or ship these bytes.");
  license.notice = QStringLiteral(
      "AnimeJaNai spatial ONNX graph\n"
      "Creator: %1 <%2>\n"
      "Source: acquired anonymously from the pinned upstream commit %3\n"
      "Licence: %4 (%5)\n"
      "Licence text: %6\n"
      "\n"
      "Copyright notice: none was supplied with these bytes; the upstream repository "
      "licence at the pinned commit is the only rights evidence located.\n"
      "\n"
      "Attribution (CC BY-NC-SA 4.0 §3(a)(1)): you must give appropriate credit to the "
      "creator above, provide a link to the licence, and indicate if changes were made.\n"
      "NonCommercial (CC BY-NC-SA 4.0 §2(a)(1)): you may not use the material for "
      "commercial purposes.\n"
      "ShareAlike (CC BY-NC-SA 4.0 §3(b)): adapted material must be shared under the "
      "same licence terms.\n"
      "\n"
      "Disclaimer (CC BY-NC-SA 4.0 §5): Unless otherwise separately undertaken by the "
      "Licensor, to the extent possible, the Licensor offers the Licensed Material "
      "as-is and as-available, and makes no representations or warranties of any kind "
      "concerning the Licensed Material, whether express, implied, statutory or other.\n"
      "\n"
      "Held as a private, non-commercial reference on this machine only. Not "
      "redistributed or re-hosted by this application.")
                                .arg(QString::fromLatin1(kCreator),
                                     QString::fromLatin1(kRepositoryUrl),
                                     QString::fromLatin1(kCommit), license.name,
                                     QStringLiteral("CC BY-NC-SA 4.0"), license.legalCodeUrl);
  return license;
}

QVector<SpatialAsset> spatialAssetCatalog() {
  // Every row is the acquisition record's own measurement, never re-hashed here.
  return {
      makeAsset(QStringLiteral("2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx"),
                1971970,
                QStringLiteral("b308e93c3dda3c4f9f968295b2b7b8d1c0583a711bca53a08d51134d9d1cbd5c")),
      makeAsset(
          QStringLiteral("2x_AnimeJaNai_HD_V3.1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx"),
          740318,
          QStringLiteral("7b804d87320f37b7269d6e27796046fbd975e95e2aa1f65d5557f52a36282716")),
      makeAsset(
          QStringLiteral("2x_AnimeJaNai_HD_V3.1Sharp1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx"),
          1971970,
          QStringLiteral("dde763c8202cfebac5ec030ea6644f3ee339ea35bbda8a76e257405399552f0f")),
      makeAsset(
          QStringLiteral("2x_AnimeJaNai_HD_V3.1Sharp1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx"),
          740318,
          QStringLiteral("72246eded3aab52f8d1a71613c0a1d900734b358d1820b62eb36f1ed376e1cf4")),
      makeAsset(QStringLiteral(
                    "2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo.onnx"),
                1212903,
                QStringLiteral("ded46591ddfaeb22b3daaf84ad7b36838335aa3efca66acfa36177ce4ab21eb5")),
  };
}

const SpatialAsset* findSpatialAsset(const QString& id) {
  static const QVector<SpatialAsset> catalog = spatialAssetCatalog();
  for (const SpatialAsset& asset : catalog) {
    if (asset.id == id) return &asset;
  }
  return nullptr;
}

QString spatialAssetRoot() { return nativeSubdirectory(QStringLiteral("assets")); }

// ------------------------------------------------------------- SpatialAssetStore

SpatialAssetStore::SpatialAssetStore() : root_(spatialAssetRoot()) {}

SpatialAssetStore::SpatialAssetStore(QString root) : root_(std::move(root)), explicitRoot_(true) {}

QString SpatialAssetStore::directoryName(const QString& id) { return contentToken(id); }

QString SpatialAssetStore::manifestFileName() { return QString::fromLatin1(kManifestFileName); }

QString SpatialAssetStore::graphFileName() { return QString::fromLatin1(kGraphFileName); }

bool SpatialAssetStore::isOwned(QString* reason) const {
  if (root_.isEmpty()) {
    if (reason) *reason = QStringLiteral("no assets root was supplied");
    return false;
  }
  if (!QDir::isAbsolutePath(root_)) {
    if (reason)
      *reason = QStringLiteral("assets root %1 is not absolute, so it cannot be shown to be ours")
                    .arg(root_);
    return false;
  }
  // The app's own root must lie inside the app-owned assets hierarchy. An
  // explicit root is caller-vouched - a test's temporary directory is the reason
  // the constructor exists - so it is used as given, but never reparse-redirected.
  if (!explicitRoot_) {
    QString ownedReason;
    const QString ownedRoot = spatialAssetRoot();
    if (ownedRoot.isEmpty()) {
      if (reason)
        *reason = QStringLiteral("the app-owned assets directory cannot be resolved");
      return false;
    }
    const QString resolved = canonicalPath(root_);
    const QString compared = resolved.isEmpty() ? QDir::cleanPath(root_) : resolved;
    if (!pathWithinRoot(ownedRoot, compared)) {
      if (reason)
        *reason = QStringLiteral("assets root %1 is outside the app-owned %2")
                      .arg(compared, ownedRoot);
      return false;
    }
  }
  const QString canonical = canonicalPath(root_);
  if (canonical.isEmpty()) {
    // Absent. A root inside the owned hierarchy is created by `ensureOwned`, so
    // a not-yet-existing parent is expected there. An explicit root is
    // caller-vouched but still has to name a real place: its parent resolves, or
    // the root is refused.
    if (explicitRoot_ && canonicalPath(QFileInfo(root_).absolutePath()).isEmpty()) {
      if (reason)
        *reason = QStringLiteral("assets root %1 does not exist and its parent cannot be resolved")
                      .arg(root_);
      return false;
    }
    clearReason(nullptr, reason);
    return true;
  }
  if (!QFileInfo(canonical).isDir()) {
    if (reason) *reason = QStringLiteral("assets root %1 is not a directory").arg(canonical);
    return false;
  }
  if (!pathHasNoReparsePoints(canonical, canonical)) {
    if (reason) *reason = QStringLiteral("assets root %1 is a reparse point").arg(canonical);
    return false;
  }
  clearReason(nullptr, reason);
  return true;
}

bool SpatialAssetStore::ensureOwned(QString* reason) const {
  if (!isOwned(reason)) return false;
  if (QFileInfo(root_).isDir()) {
    clearReason(nullptr, reason);
    return true;
  }
  if (!QDir().mkpath(root_)) {
    if (reason) *reason = QStringLiteral("assets root %1 could not be created").arg(root_);
    return false;
  }
  clearReason(nullptr, reason);
  return true;
}

SpatialAssetPublication SpatialAssetStore::publish(const SpatialAsset& asset,
                                                   const QByteArray& bytes, QString* reasonKey,
                                                   QString* reason) {
  SpatialAssetPublication outcome;
  const auto refuse = [&outcome, reasonKey, reason](const QString& key, const QString& text) {
    outcome.ok = false;
    outcome.published = false;
    outcome.reasonKey = key;
    outcome.reason = text;
    if (reasonKey) *reasonKey = key;
    if (reason) *reason = text;
    return outcome;
  };
  clearReason(reasonKey, reason);

  if (!asset.isValid()) {
    return refuse(QStringLiteral("spatial.asset.invalid"),
                  QStringLiteral("the catalogue row has no usable identity"));
  }
  // Only a pinned catalogue row may be published, and it must be passed as the
  // record states it: this is not a general file store.
  const SpatialAsset* pinned = findSpatialAsset(asset.id);
  if (pinned == nullptr || pinned->sha256 != asset.sha256 || pinned->bytes != asset.bytes) {
    return refuse(QStringLiteral("spatial.asset.unknown"),
                  QStringLiteral("%1 is not a pinned catalogue row").arg(asset.id));
  }
  // The destination is proven before the payload is looked at: a root that is a
  // file, outside the app-owned tree, or otherwise unusable is refused as a root
  // problem, not as a payload problem.
  QString ownedReason;
  if (!ensureOwned(&ownedReason)) {
    return refuse(QStringLiteral("spatial.asset.root"),
                  QStringLiteral("assets root %1 is not app-owned: %2").arg(root_, ownedReason));
  }
  if (bytes.isEmpty()) {
    return refuse(QStringLiteral("spatial.asset.empty"),
                  QStringLiteral("there are no bytes to publish"));
  }
  // Digest and length are checked before anything is written: a payload that is
  // not exactly the pinned graph writes nothing.
  const QString digest = sha256HexOfBytes(bytes);
  if (digest != pinned->sha256 || bytes.size() != pinned->bytes) {
    return refuse(QStringLiteral("spatial.asset.digest"),
                  QStringLiteral("the payload is %1 bytes hashing %2, not %3 bytes hashing %4")
                      .arg(bytes.size())
                      .arg(digest)
                      .arg(pinned->bytes)
                      .arg(pinned->sha256));
  }

  const QString token = directoryName(pinned->id);
  const QString finalDirectory = QDir(root_).filePath(token);
  if (QFileInfo(finalDirectory).exists()) {
    StoredSpatialAsset existing;
    QString existingKey;
    QString existingReason;
    if (readStored(root_, finalDirectory, &existing, &existingKey, &existingReason, true) &&
        existing.asset.sha256 == pinned->sha256) {
      // Immutable: an identical copy is not rewritten.
      outcome.ok = true;
      outcome.published = false;
      outcome.id = existing.asset.id;
      outcome.directory = existing.directory;
      outcome.graphPath = existing.graphPath;
      return outcome;
    }
    return refuse(QStringLiteral("spatial.asset.existsUnverified"),
                  QStringLiteral("%1 already exists and does not verify: %2")
                      .arg(finalDirectory, existingReason));
  }

  const QString stagingDirectory =
      QDir(root_).filePath(QStringLiteral("%1%2").arg(QString::fromLatin1(kStagingPrefix),
                                                      QUuid::createUuid().toString(
                                                          QUuid::WithoutBraces)));
  if (!QDir().mkpath(stagingDirectory)) {
    return refuse(QStringLiteral("spatial.asset.staging"),
                  QStringLiteral("staging directory %1 could not be created")
                      .arg(stagingDirectory));
  }
  const auto abandon = [&outcome, &stagingDirectory, reasonKey, reason](const QString& key,
                                                                        const QString& text) {
    QDir(stagingDirectory).removeRecursively();
    outcome.ok = false;
    outcome.published = false;
    outcome.reasonKey = key;
    outcome.reason = text;
    if (reasonKey) *reasonKey = key;
    if (reason) *reason = text;
    return outcome;
  };

  const QString stagedGraph = QDir(stagingDirectory).filePath(QString::fromLatin1(kGraphFileName));
  {
    QFile file(stagedGraph);
    if (!file.open(QIODevice::WriteOnly)) {
      return abandon(QStringLiteral("spatial.asset.write"),
                     QStringLiteral("%1 could not be written: %2")
                         .arg(stagedGraph, file.errorString()));
    }
    if (file.write(bytes) != bytes.size() || !file.flush()) {
      return abandon(QStringLiteral("spatial.asset.write"),
                     QStringLiteral("%1 could not be written: %2")
                         .arg(stagedGraph, file.errorString()));
    }
    file.close();
    if (file.error() != QFile::NoError) {
      return abandon(QStringLiteral("spatial.asset.write"),
                     QStringLiteral("%1 could not be written: %2")
                         .arg(stagedGraph, file.errorString()));
    }
  }

  QString stagedReason;
  const std::optional<FileDigest> staged = measureFile(stagedGraph, &stagedReason);
  if (!staged.has_value()) {
    return abandon(QStringLiteral("spatial.asset.digest"), stagedReason);
  }
  if (staged->sha256 != pinned->sha256 || staged->bytes != pinned->bytes) {
    return abandon(
        QStringLiteral("spatial.asset.digest"),
        QStringLiteral("the staged copy is %1 bytes hashing %2, not %3 bytes hashing %4")
            .arg(staged->bytes)
            .arg(staged->sha256)
            .arg(pinned->bytes)
            .arg(pinned->sha256));
  }

  {
    QJsonObject manifest;
    manifest.insert(QStringLiteral("schema"), QString::fromLatin1(kSchema));
    manifest.insert(QStringLiteral("id"), pinned->id);
    manifest.insert(QStringLiteral("name"), pinned->name);
    manifest.insert(QStringLiteral("sha256"), pinned->sha256);
    manifest.insert(QStringLiteral("bytes"), QJsonValue(static_cast<double>(pinned->bytes)));
    manifest.insert(QStringLiteral("source"), sourceToJson(*pinned));
    manifest.insert(QStringLiteral("creator"), QString::fromLatin1(kCreator));
    manifest.insert(QStringLiteral("license"), licenseToJson(spatialAssetLicense()));
    manifest.insert(QStringLiteral("acquiredAt"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    const QByteArray payload = QJsonDocument(manifest).toJson(QJsonDocument::Compact);

    QSaveFile file(QDir(stagingDirectory).filePath(QString::fromLatin1(kManifestFileName)));
    if (!file.open(QIODevice::WriteOnly)) {
      return abandon(QStringLiteral("spatial.asset.manifest"),
                     QStringLiteral("the staged manifest could not be written"));
    }
    if (file.write(payload) != payload.size() || !file.commit()) {
      return abandon(QStringLiteral("spatial.asset.manifest"),
                     QStringLiteral("the staged manifest could not be written"));
    }
  }
  {
    // The retention notice is written as its own file so it survives beside the
    // bytes even if the manifest is read by another tool.
    const QByteArray notice = spatialAssetLicense().notice.toUtf8();
    QSaveFile file(QDir(stagingDirectory).filePath(QString::fromLatin1(kNoticeFileName)));
    if (!file.open(QIODevice::WriteOnly)) {
      return abandon(QStringLiteral("spatial.asset.notice"),
                     QStringLiteral("the staged licence notice could not be written"));
    }
    if (file.write(notice) != notice.size() || !file.commit()) {
      return abandon(QStringLiteral("spatial.asset.notice"),
                     QStringLiteral("the staged licence notice could not be written"));
    }
  }

  {
    StoredSpatialAsset readBack;
    QString readKey;
    QString readReason;
    if (!readStored(root_, stagingDirectory, &readBack, &readKey, &readReason, false)) {
      return abandon(QStringLiteral("spatial.asset.manifest"),
                     QStringLiteral("the staged manifest does not read back: %1").arg(readReason));
    }
  }

  if (!QDir().rename(stagingDirectory, finalDirectory)) {
    return abandon(QStringLiteral("spatial.asset.rename"),
                   QStringLiteral("staging directory %1 could not be moved into place")
                       .arg(stagingDirectory));
  }

  StoredSpatialAsset stored;
  QString storedKey;
  QString storedReason;
  if (!readStored(root_, finalDirectory, &stored, &storedKey, &storedReason, true)) {
    outcome.ok = false;
    outcome.published = true;
    outcome.reasonKey = storedKey;
    outcome.reason = storedReason;
    outcome.id = pinned->id;
    outcome.directory = finalDirectory;
    if (reasonKey) *reasonKey = storedKey;
    if (reason) *reason = storedReason;
    return outcome;
  }
  outcome.ok = true;
  outcome.published = true;
  outcome.id = stored.asset.id;
  outcome.directory = stored.directory;
  outcome.graphPath = stored.graphPath;
  clearReason(reasonKey, reason);
  return outcome;
}

QVector<StoredSpatialAsset> SpatialAssetStore::stored(QString* reason) const {
  QVector<StoredSpatialAsset> rows;
  if (reason) reason->clear();
  if (!QFileInfo(root_).isDir()) {
    if (reason) *reason = QStringLiteral("assets root %1 does not exist").arg(root_);
    return rows;
  }

  const QFileInfoList entries =
      QDir(root_).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo& entry : entries) {
    const QString token = entry.fileName();
    // Anything whose name is not a digest is not one of ours: a staging
    // directory left by a crashed write is not a row.
    if (!isSha256Hex(token)) continue;

    StoredSpatialAsset row;
    QString key;
    QString rowReason;
    if (readStored(root_, entry.absoluteFilePath(), &row, &key, &rowReason, true)) {
      rows.append(row);
      continue;
    }
    // A directory that no longer verifies is still reported, never dropped.
    row.asset.id = assetIdFor(token);
    row.asset.sha256 = token;
    row.directory = entry.absoluteFilePath();
    row.graphPath = QDir(entry.absoluteFilePath()).filePath(QString::fromLatin1(kGraphFileName));
    row.verified = false;
    row.reasonKey = key.isEmpty() ? QStringLiteral("spatial.asset.corrupt") : key;
    row.reason = rowReason;
    rows.append(row);
  }
  return rows;
}

SpatialAssetPublication publishSpatialAssetBytes(const SpatialAsset& asset, const QByteArray& bytes,
                                                 const QString& root) {
  if (root.isEmpty()) {
    // The app-owned root: containment in `native/assets` is still enforced.
    SpatialAssetStore store;
    return store.publish(asset, bytes, nullptr, nullptr);
  }
  // An explicit, caller-vouched root - the tests' temporary directory.
  SpatialAssetStore store(root);
  return store.publish(asset, bytes, nullptr, nullptr);
}

QVector<StoredSpatialAsset> storedSpatialAssets(const QString& root) {
  if (root.isEmpty()) {
    SpatialAssetStore store;
    return store.stored(nullptr);
  }
  SpatialAssetStore store(root);
  return store.stored(nullptr);
}

}  // namespace native
}  // namespace mplayer
