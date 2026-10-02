#include "NativeArtifactStore.hpp"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QSaveFile>
#include <QtCore/QUuid>

#include "NativeDiagnostics.hpp"
#include "NativePaths.hpp"

namespace mplayer {
namespace native {

namespace {

constexpr const char* kManifestFileName = "manifest.json";
constexpr const char* kStagingPrefix = ".staging-";
constexpr int kHashChunkBytes = 1 << 16;

void fail(QString* reasonKey, QString* reason, const QString& key, const QString& text) {
  if (reasonKey) *reasonKey = key;
  // Every refusal in this module leaves through here, so a manifest-supplied
  // name, option string or device value that looks like a URL or a credential is
  // redacted at the one boundary rather than echoed by whichever check happened
  // to quote it.
  if (reason) *reason = redactNativeDiagnosticText(text);
}

/// Sets a caller-facing message, redacting a URL- or credential-shaped value at
/// the same boundary `fail()` uses, for refusals that quote a caller-supplied
/// path rather than a manifest field.
void setReason(QString* reason, const QString& text) {
  if (reason) *reason = redactNativeDiagnosticText(text);
}

void clearReason(QString* reasonKey, QString* reason) {
  if (reasonKey) reasonKey->clear();
  if (reason) reason->clear();
}

/// "sha256:<hex>" becomes a directory name of our own. A colon is not usable in a
/// Windows name, and letting the raw key reach the filesystem would mean the
/// thing that selects the directory is no longer the digest.
QString buildKeyToken(const QString& buildKey) {
  if (!buildKey.startsWith(QLatin1String("sha256:"))) return {};
  const QString hex = buildKey.mid(7);
  if (!isSha256Hex(hex)) return {};
  return hex;
}

/// A regular file: not a link, not a directory, not a device, and not reachable
/// through a junction. Every file this module hashes, publishes or deletes goes
/// through this, so "the file the manifest describes" cannot be a redirect to
/// somewhere else.
bool isRegularOwnedFile(const QString& path, const QString& root, QString* reasonKey,
                        QString* reason) {
  const QFileInfo info(path);
  if (!info.exists()) {
    fail(reasonKey, reason, QStringLiteral("artifact.missing"),
         QStringLiteral("%1 does not exist").arg(path));
    return false;
  }
  if (info.isSymLink()) {
    fail(reasonKey, reason, QStringLiteral("artifact.symlink"),
         QStringLiteral("%1 is a symbolic link, not the file its manifest names").arg(path));
    return false;
  }
  if (!info.isFile()) {
    fail(reasonKey, reason, QStringLiteral("artifact.notRegular"),
         QStringLiteral("%1 is not a regular file").arg(path));
    return false;
  }
  if (!pathHasNoReparsePoints(root, path)) {
    fail(reasonKey, reason, QStringLiteral("artifact.reparse"),
         QStringLiteral("%1 is reached through a reparse point inside %2").arg(path, root));
    return false;
  }
  clearReason(reasonKey, reason);
  return true;
}

/// The engine's recorded name, resolved against the artifact's own owned
/// directory - the `artifact-*`/staging directory the manifest was published
/// into - and never against the process working directory. The recorded name is
/// relative by construction, so joining it to the store root would look for the
/// file in the wrong (parent) directory and miss every real artifact. Refuses an
/// absolute name, a traversal, a name that escapes that directory or the owned
/// artifacts root, and a name whose bytes are reached through a junction: a
/// manifest is data, and data does not get to say where it lives.
bool resolveEnginePath(const QString& artifactRoot, const QString& ownedRoot,
                       const ArtifactManifest& manifest, QString* path, QString* reasonKey,
                       QString* reason) {
  QString nameReason;
  const QString safe = safeRelativePath(manifest.engine.file, &nameReason);
  if (safe.isEmpty()) {
    fail(reasonKey, reason, QStringLiteral("artifact.enginePath"),
         QStringLiteral("engine file name '%1' is not a safe relative path: %2")
             .arg(manifest.engine.file, nameReason));
    return false;
  }
  // The name the manifest chose is checked before it is resolved: a name whose
  // path is a link or is reached through one is refused even when it resolves
  // back inside the root, because the file that is hashed would then not be the
  // file the manifest names.
  const QString joined = QDir(artifactRoot).filePath(safe);
  const QFileInfo joinedInfo(joined);
  if (!joinedInfo.exists() && !joinedInfo.isSymLink()) {
    fail(reasonKey, reason, QStringLiteral("artifact.engineMissing"),
         QStringLiteral("engine file %1 does not exist in %2")
             .arg(manifest.engine.file, artifactRoot));
    return false;
  }
  if (joinedInfo.isSymLink()) {
    fail(reasonKey, reason, QStringLiteral("artifact.symlink"),
         QStringLiteral("engine file %1 is a symbolic link, not the file its manifest names")
             .arg(manifest.engine.file));
    return false;
  }
  if (!pathHasNoReparsePoints(artifactRoot, joined)) {
    fail(reasonKey, reason, QStringLiteral("artifact.reparse"),
         QStringLiteral("engine file %1 is reached through a reparse point inside %2")
             .arg(manifest.engine.file, artifactRoot));
    return false;
  }
  const QString canonical = canonicalPath(joined);
  if (canonical.isEmpty()) {
    fail(reasonKey, reason, QStringLiteral("artifact.engineMissing"),
         QStringLiteral("engine file %1 does not exist in %2")
             .arg(manifest.engine.file, artifactRoot));
    return false;
  }
  const QString ownedCanonical = canonicalPath(ownedRoot);
  const QString owned = ownedCanonical.isEmpty() ? ownedRoot : ownedCanonical;
  if (!pathWithinRoot(artifactRoot, canonical) || !pathWithinRoot(owned, canonical)) {
    fail(reasonKey, reason, QStringLiteral("artifact.engineEscape"),
         QStringLiteral("engine file '%1' resolves outside the artifact directory %2")
             .arg(manifest.engine.file, artifactRoot));
    return false;
  }
  if (!pathHasNoReparsePoints(owned, canonical)) {
    fail(reasonKey, reason, QStringLiteral("artifact.reparse"),
         QStringLiteral("engine file '%1' is reached through a reparse point inside the "
                        "artifacts root")
             .arg(manifest.engine.file));
    return false;
  }
  *path = canonical;
  clearReason(reasonKey, reason);
  return true;
}

/// Reads and parses an artifact manifest. The manifest path is itself checked
/// before it is opened: it must be a regular file inside `root`, not a link, and
/// not reached through a reparse point, because a manifest read through a
/// redirect would describe a different artifact than the one at this path.
bool readManifestFile(const QString& manifestPath, const QString& root, ArtifactManifest* manifest,
                      QString* reasonKey, QString* reason) {
  const QFileInfo info(manifestPath);
  if (!info.exists()) {
    fail(reasonKey, reason, QStringLiteral("artifact.manifestUnreadable"),
         QStringLiteral("%1 does not exist, so the artifact has no manifest to read")
             .arg(manifestPath));
    return false;
  }
  if (info.isSymLink() || !info.isFile()) {
    fail(reasonKey, reason, QStringLiteral("artifact.manifestUnreadable"),
         QStringLiteral("%1 is not a regular manifest file").arg(manifestPath));
    return false;
  }
  if (!pathHasNoReparsePoints(root, manifestPath)) {
    fail(reasonKey, reason, QStringLiteral("artifact.reparse"),
         QStringLiteral("%1 is reached through a reparse point inside %2")
             .arg(manifestPath, root));
    return false;
  }
  QFile file(manifestPath);
  if (!file.open(QIODevice::ReadOnly)) {
    fail(reasonKey, reason, QStringLiteral("artifact.manifestUnreadable"),
         QStringLiteral("%1 cannot be read: %2").arg(manifestPath, file.errorString()));
    return false;
  }
  const QByteArray payload = file.readAll();
  if (file.error() != QFile::NoError) {
    fail(reasonKey, reason, QStringLiteral("artifact.manifestUnreadable"),
         QStringLiteral("%1 failed while reading: %2").arg(manifestPath, file.errorString()));
    return false;
  }
  return parseArtifactManifest(payload, manifest, reasonKey, reason);
}

/// Compares a record's recorded engine against the bytes, streaming them. The
/// resolved absolute path comes back so the caller can report one location rather
/// than recomputing it from a name that might resolve differently.
bool engineMatchesRecord(const ArtifactStore& store, const ArtifactRecord& record,
                         QString* enginePathOut, QString* reasonKey, QString* reason) {
  if (!pathWithinRoot(store.root(), record.root)) {
    fail(reasonKey, reason, QStringLiteral("artifact.escape"),
         QStringLiteral("artifact directory %1 is outside the owned artifacts root %2")
             .arg(record.root, store.root()));
    return false;
  }
  QString enginePath;
  if (!resolveEnginePath(record.root, store.root(), record.manifest, &enginePath, reasonKey,
                         reason)) {
    return false;
  }
  if (!isRegularOwnedFile(enginePath, record.root, reasonKey, reason)) return false;

  const QFileInfo info(enginePath);
  if (info.size() != record.manifest.engine.bytes) {
    fail(reasonKey, reason, QStringLiteral("artifact.engineBytes"),
         QStringLiteral("engine file is %1 bytes, the manifest recorded %2 - it is truncated or "
                        "was replaced")
             .arg(info.size())
             .arg(record.manifest.engine.bytes));
    return false;
  }
  const auto measured = measureFile(enginePath, reason);
  if (!measured) return false;
  if (measured->sha256 != record.manifest.engine.sha256) {
    fail(reasonKey, reason, QStringLiteral("artifact.engineSha256"),
         QStringLiteral("engine file hashes to %1, the manifest recorded %2")
             .arg(measured->sha256, record.manifest.engine.sha256));
    return false;
  }
  if (enginePathOut) *enginePathOut = enginePath;
  clearReason(reasonKey, reason);
  return true;
}

/// Removes a directory only when every entry inside it is a regular non-reparse
/// file. One entry that is a directory or a link means part of the tree is not
/// known to be ours, and the whole directory is left alone.
///
/// An artifact's engine is one flat file beside its manifest, which
/// `ArtifactManifest::isSelfConsistent` enforces, so a staging directory this
/// cache wrote holds only regular files and this proof accepts it - which is
/// what lets a failed publish remove its own staging instead of leaking it. A
/// subdirectory can only be something this cache did not write, and refusing it
/// is the point.
bool removeTreeWithProof(const QString& directory, QString* reasonKey, QString* reason) {
  QDir dir(directory);
  const QFileInfoList entries =
      dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QFileInfo& entry : entries) {
    if (entry.isSymLink() || entry.isDir()) {
      fail(reasonKey, reason, QStringLiteral("artifact.unproven"),
           QStringLiteral("%1 inside %2 is not a regular file this cache published, so nothing "
                          "inside is removed")
               .arg(entry.fileName(), directory));
      return false;
    }
    if (!pathHasNoReparsePoints(directory, entry.absoluteFilePath())) {
      fail(reasonKey, reason, QStringLiteral("artifact.reparse"),
           QStringLiteral("%1 is reached through a reparse point inside %2")
               .arg(entry.absoluteFilePath(), directory));
      return false;
    }
  }
  for (const QFileInfo& entry : entries) {
    if (!QFile::remove(entry.absoluteFilePath())) {
      fail(reasonKey, reason, QStringLiteral("artifact.removeFailed"),
           QStringLiteral("%1 could not be removed").arg(entry.absoluteFilePath()));
      return false;
    }
  }
  QDir().rmdir(directory);
  clearReason(reasonKey, reason);
  return true;
}

/// One process-wide count per (canonical owned root, build key). Acquisition and
/// removal take this lock, and removal holds it across its proof and deletion, so
/// a claim cannot be granted for bytes that call is about to remove.
struct LeaseRegistry {
  QMutex mutex;
  QHash<QString, int> counts;
};

LeaseRegistry& leaseRegistry() {
  static LeaseRegistry registry;
  return registry;
}

QString canonicalLeaseRoot(const QString& root) {
  const QString canonical = canonicalPath(root);
  if (!canonical.isEmpty()) return canonical;
  return QDir::cleanPath(QFileInfo(root).absoluteFilePath());
}

QString leaseRegistryKey(const QString& canonicalRoot, const QString& buildKey) {
  return canonicalRoot + QLatin1Char('\n') + buildKey;
}

}  // namespace

std::optional<FileDigest> measureFile(const QString& path, QString* reason) {
  const QFileInfo info(path);
  if (!info.exists() || !info.isFile() || info.isSymLink()) {
    if (reason) {
      *reason = redactNativeDiagnosticText(
          QStringLiteral("%1 is not a regular file, so its bytes cannot be identified").arg(path));
    }
    return std::nullopt;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    if (reason) {
      *reason =
          redactNativeDiagnosticText(QStringLiteral("%1 cannot be read: %2").arg(path, file.errorString()));
    }
    return std::nullopt;
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  qint64 total = 0;
  char buffer[kHashChunkBytes];
  for (;;) {
    const qint64 read = file.read(buffer, sizeof(buffer));
    if (read < 0) {
      if (reason) {
        *reason = redactNativeDiagnosticText(
            QStringLiteral("%1 failed while reading: %2").arg(path, file.errorString()));
      }
      return std::nullopt;
    }
    if (read == 0) break;
    total += read;
    hash.addData(QByteArrayView(buffer, read));
  }
  if (file.error() != QFile::NoError) {
    if (reason) {
      *reason = redactNativeDiagnosticText(
          QStringLiteral("%1 failed while reading: %2").arg(path, file.errorString()));
    }
    return std::nullopt;
  }
  FileDigest digest;
  digest.sha256 = QString::fromLatin1(hash.result().toHex());
  digest.bytes = total;
  if (reason) reason->clear();
  return digest;
}

// -------------------------------------------------------------- ArtifactStore

ArtifactStore::ArtifactStore() : root_(nativeSubdirectory(QStringLiteral("artifacts"))) {}

// An explicit root. Unlike a caller-vouched path, it is still confined by
// `isOwned()` to a location this app can prove it owns: the app-owned artifacts
// hierarchy or the process temporary directory (the probe/test root). A model,
// user, shared or installed directory is inside neither, so an explicit root
// cannot turn one into an ordinary writable cache.
ArtifactStore::ArtifactStore(QString root) : root_(std::move(root)), explicitRoot_(true) {}

bool ArtifactStore::isOwned(QString* reason) const {
  if (root_.isEmpty()) {
    setReason(reason, QStringLiteral("no artifacts root was supplied"));
    return false;
  }
  if (!QDir::isAbsolutePath(root_)) {
    if (reason) {
      setReason(reason, QStringLiteral("artifacts root %1 is not absolute, so it cannot be shown "
                                       "to be ours").arg(root_));
    }
    return false;
  }
  // Every store root has to fall inside a location this app can prove it owns.
  // The default root must lie inside the app-owned artifacts hierarchy. An
  // explicit root is confined to that same hierarchy or to the process temporary
  // directory, which is the probe/test root: a model, user, shared or installed
  // directory is inside neither, so no argument can point an ordinary writable
  // store - one that may create, publish into and delete from - at one.
  QString proofRoot;
  if (explicitRoot_) {
    if (QFileInfo(root_).isSymLink()) {
      if (reason) {
        setReason(reason, QStringLiteral("artifacts root %1 is a symbolic link, so it is not used "
                                         "as an owned cache").arg(root_));
      }
      return false;
    }
    QString ownedReason;
    const QString artifactsRoot = nativeSubdirectory(QStringLiteral("artifacts"), &ownedReason);
    const QString probeRoot = QDir::tempPath();
    if (artifactsRoot.isEmpty() || probeRoot.isEmpty()) {
      if (reason) {
        setReason(reason,
                  QStringLiteral("no owned location can be resolved for %1: %2")
                      .arg(root_, ownedReason));
      }
      return false;
    }
    const QString resolved = canonicalPath(root_);
    const QString compared = resolved.isEmpty() ? root_ : resolved;
    const bool underArtifacts = pathWithinRoot(artifactsRoot, compared);
    const bool underProbe = pathWithinRoot(probeRoot, compared);
    if (!underArtifacts && !underProbe) {
      if (reason) {
        setReason(reason,
                  QStringLiteral("artifacts root %1 is outside the app-owned %2 and the probe "
                                 "root %3, so it is not a cache this app may write or clean")
                      .arg(compared, artifactsRoot, probeRoot));
      }
      return false;
    }
    proofRoot = underArtifacts ? artifactsRoot : probeRoot;
  } else {
    QString ownedReason;
    proofRoot = nativeSubdirectory(QStringLiteral("artifacts"), &ownedReason);
    if (proofRoot.isEmpty()) {
      if (reason) {
        setReason(reason,
                  QStringLiteral("the app-owned artifacts directory cannot be resolved: %1")
                      .arg(ownedReason));
      }
      return false;
    }
    const QString resolved = canonicalPath(root_);
    const QString compared = resolved.isEmpty() ? root_ : resolved;
    if (!pathWithinRoot(proofRoot, compared)) {
      if (reason) {
        setReason(reason,
                  QStringLiteral("artifacts root %1 is outside the app-owned %2, so it is not a "
                                 "cache this app may write or clean").arg(compared, proofRoot));
      }
      return false;
    }
  }
  const QString canonical = canonicalPath(root_);
  if (canonical.isEmpty()) {
    // Absent, but creatable only under a resolvable parent and with no reparse
    // point between the proved root and the root; otherwise a junction inside
    // the probe root could create the store somewhere else entirely.
    if (canonicalPath(QFileInfo(root_).absolutePath()).isEmpty() ||
        !pathHasNoReparsePoints(proofRoot, root_)) {
      if (reason) {
        setReason(reason,
                  QStringLiteral("artifacts root %1 does not exist and cannot be shown to be "
                                 "inside %2, so it cannot be owned")
                      .arg(root_, proofRoot));
      }
      return false;
    }
    clearReason(nullptr, reason);
    return true;
  }
  if (!QFileInfo(canonical).isDir()) {
    setReason(reason, QStringLiteral("artifacts root %1 is not a directory").arg(canonical));
    return false;
  }
  if (!pathHasNoReparsePoints(canonical, canonical)) {
    setReason(reason, QStringLiteral("artifacts root %1 is a reparse point").arg(canonical));
    return false;
  }
  clearReason(nullptr, reason);
  return true;
}

bool ArtifactStore::ensureOwned(QString* reason) const {
  if (!isOwned(reason)) return false;
  if (QFileInfo(root_).isDir()) {
    clearReason(nullptr, reason);
    return true;
  }
  if (!QDir().mkpath(root_)) {
    setReason(reason, QStringLiteral("artifacts root %1 could not be created").arg(root_));
    return false;
  }
  clearReason(nullptr, reason);
  return true;
}

QString ArtifactStore::artifactDirectoryName(const QString& buildKey) {
  const QString token = buildKeyToken(buildKey);
  if (token.isEmpty()) return {};
  return QStringLiteral("artifact-%1").arg(token);
}

QString ArtifactStore::artifactManifestFileName() { return QString::fromLatin1(kManifestFileName); }

// ----------------------------------------------------------------------- lease

struct ArtifactLease::Claim {
  QString root;
  QString buildKey;

  /// The last copy going away is what makes the pair removable again.
  ~Claim() {
    LeaseRegistry& registry = leaseRegistry();
    QMutexLocker locker(&registry.mutex);
    const auto it = registry.counts.find(leaseRegistryKey(root, buildKey));
    if (it == registry.counts.end()) return;
    if (--it.value() <= 0) registry.counts.erase(it);
  }
};

bool ArtifactLease::isValid() const { return claim_ != nullptr; }

const QString& ArtifactLease::root() const {
  static const QString empty;
  return claim_ ? claim_->root : empty;
}

const QString& ArtifactLease::buildKey() const {
  static const QString empty;
  return claim_ ? claim_->buildKey : empty;
}

std::optional<ArtifactLease> acquireArtifactLease(const ArtifactStore& store,
                                                  const QString& buildKey, QString* reasonKey,
                                                  QString* reason) {
  // Proving the artifact and registering the claim share the removal lock, so
  // nothing can delete these bytes in between.
  LeaseRegistry& registry = leaseRegistry();
  QMutexLocker locker(&registry.mutex);
  if (!readArtifact(store, buildKey, reasonKey, reason)) return std::nullopt;

  auto claim = std::make_shared<ArtifactLease::Claim>();
  claim->root = canonicalLeaseRoot(store.root());
  claim->buildKey = buildKey;
  ++registry.counts[leaseRegistryKey(claim->root, claim->buildKey)];

  ArtifactLease lease;
  lease.claim_ = std::move(claim);
  clearReason(reasonKey, reason);
  return lease;
}

// -------------------------------------------------------------------- publish

std::optional<ArtifactRecord> publishArtifact(const ArtifactStore& store,
                                              const ArtifactManifest& manifest,
                                              const QString& scratchEnginePath,
                                              QString* reasonKey, QString* reason) {
  const auto refuse = [&](const QString& key, const QString& text) {
    fail(reasonKey, reason, key, text);
    return std::optional<ArtifactRecord>();
  };

  if (!store.isOwned(reason)) {
    fail(reasonKey, reason, QStringLiteral("store.unowned"),
         QStringLiteral("artifacts root %1 is not app-owned: %2")
             .arg(store.root(), reason ? *reason : QString()));
    return std::nullopt;
  }
  if (!manifest.isSelfConsistent(reasonKey, reason)) return std::nullopt;

  const QString directoryName = ArtifactStore::artifactDirectoryName(manifest.buildKey);
  if (directoryName.isEmpty()) {
    return refuse(QStringLiteral("manifest.buildKey"),
                  QStringLiteral("build key %1 does not name a directory").arg(manifest.buildKey));
  }

  // The manifest's engine name must be a safe relative path before anything is
  // written: an unsafe name must not reach the filesystem even inside staging.
  QString engineNameReason;
  const QString engineName = safeRelativePath(manifest.engine.file, &engineNameReason);
  if (engineName.isEmpty()) {
    return refuse(QStringLiteral("manifest.enginePath"),
                  QStringLiteral("engine file name '%1' is not a safe relative path: %2")
                      .arg(manifest.engine.file, engineNameReason));
  }
  // The engine and its manifest cannot share one name. The manifest is written
  // into the same directory after the engine, so a collision would overwrite the
  // engine and the published artifact could not describe the bytes it holds.
  if (QString::compare(engineName, QLatin1String(kManifestFileName), pathCaseSensitivity()) == 0) {
    return refuse(QStringLiteral("publish.engineName"),
                  QStringLiteral("engine file name '%1' is the manifest file name, so the engine "
                                 "and its manifest cannot both be published under it")
                      .arg(manifest.engine.file));
  }

  const auto produced = measureFile(scratchEnginePath, reason);
  if (!produced) {
    fail(reasonKey, reason, QStringLiteral("publish.engineMissing"),
         QStringLiteral("the produced engine %1 cannot be measured: %2")
             .arg(scratchEnginePath, reason ? *reason : QString()));
    return std::nullopt;
  }
  // The producer's bytes must be the bytes the manifest describes. Publishing a
  // file the manifest does not hash would make the manifest a claim rather than a
  // record, which is the accumulation this cache exists to prevent.
  if (produced->sha256 != manifest.engineSha256 || produced->bytes != manifest.engine.bytes) {
    return refuse(QStringLiteral("publish.engineMismatch"),
                  QStringLiteral("the produced engine hashes to %1 (%2 bytes) but the manifest "
                                 "records %3 (%4 bytes)")
                      .arg(produced->sha256)
                      .arg(produced->bytes)
                      .arg(manifest.engineSha256)
                      .arg(manifest.engine.bytes));
  }

  if (!store.ensureOwned(reason)) return std::nullopt;

  const QString finalDirectory = QDir(store.root()).filePath(directoryName);
  if (QFileInfo(finalDirectory).exists()) {
    // Never silently replace. An existing artifact is either the same one - in
    // which case re-publishing is a no-op - or something that needs
    // `removeArtifact`'s proof before it goes.
    const auto existing = readArtifact(store, manifest.buildKey, reasonKey, reason);
    if (existing) return existing;
    return refuse(QStringLiteral("publish.exists"),
                  QStringLiteral("an artifact is already published under this build key and does "
                                 "not verify: %1")
                      .arg(reason ? *reason : QString()));
  }

  // Staging is a sibling directory, so publishing is one rename on one volume.
  // Nothing can observe a half-written artifact: a staging name is never a lookup
  // candidate, and the manifest inside it is written last.
  const QString stagingName =
      QStringLiteral("%1%2-%3")
          .arg(QLatin1String(kStagingPrefix), directoryName,
               QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
  const QString stagingDirectory = QDir(store.root()).filePath(stagingName);
  if (!QDir().mkpath(stagingDirectory)) {
    return refuse(QStringLiteral("publish.staging"),
                  QStringLiteral("staging directory %1 could not be created").arg(stagingDirectory));
  }
  const auto abandon = [&](const QString& key, const QString& text) {
    removeTreeWithProof(stagingDirectory, nullptr, nullptr);
    return refuse(key, text);
  };

  const QString stagedEngine = QDir(stagingDirectory).filePath(engineName);
  if (!QDir().mkpath(QFileInfo(stagedEngine).absolutePath())) {
    return abandon(QStringLiteral("publish.staging"),
                   QStringLiteral("the staging directory for the engine could not be created"));
  }
  if (!QFile::copy(scratchEnginePath, stagedEngine)) {
    return abandon(QStringLiteral("publish.copy"),
                   QStringLiteral("the produced engine could not be copied into staging"));
  }

  // The staged copy is read back and re-hashed here, not trusted from the
  // measurement above: what ships is what these bytes hash to.
  QString pathReason;
  if (!isRegularOwnedFile(stagedEngine, stagingDirectory, reasonKey, &pathReason)) {
    return abandon(QStringLiteral("publish.stagedFile"), pathReason);
  }
  const auto stagedDigest = measureFile(stagedEngine, &pathReason);
  if (!stagedDigest) {
    return abandon(QStringLiteral("publish.stagedFile"), pathReason);
  }
  if (stagedDigest->sha256 != manifest.engineSha256 || stagedDigest->bytes != manifest.engine.bytes) {
    return abandon(QStringLiteral("publish.engineMismatch"),
                   QStringLiteral("the staged engine hashes to %1 but the manifest records %2")
                       .arg(stagedDigest->sha256, manifest.engineSha256));
  }

  {
    QSaveFile stagedManifest(
        QDir(stagingDirectory).filePath(ArtifactStore::artifactManifestFileName()));
    if (!stagedManifest.open(QIODevice::WriteOnly)) {
      return abandon(QStringLiteral("publish.manifest"),
                     QStringLiteral("the manifest could not be written into staging"));
    }
    const QByteArray payload = serializeArtifactManifest(manifest);
    if (stagedManifest.write(payload) != payload.size() || !stagedManifest.commit()) {
      return abandon(QStringLiteral("publish.manifest"),
                     QStringLiteral("the manifest could not be written into staging"));
    }
  }

  // The staged manifest is parsed back from disk and must describe what was
  // published, so a serializer that drops a member cannot ship a document this
  // build cannot read.
  ArtifactManifest reread;
  if (!readManifestFile(QDir(stagingDirectory).filePath(ArtifactStore::artifactManifestFileName()),
                        stagingDirectory, &reread, reasonKey, reason)) {
    return abandon(QStringLiteral("publish.manifestReread"),
                   QStringLiteral("the staged manifest does not parse back: %1")
                       .arg(reason ? *reason : QString()));
  }
  if (reread.engineSha256 != manifest.engineSha256 || reread.buildKey != manifest.buildKey ||
      reread.engine.file != manifest.engine.file) {
    return abandon(QStringLiteral("publish.manifestReread"),
                   QStringLiteral("the staged manifest does not describe what was published"));
  }

  if (!QDir().rename(stagingDirectory, finalDirectory)) {
    return abandon(QStringLiteral("publish.rename"),
                   QStringLiteral("staging directory %1 could not be moved into place")
                       .arg(stagingDirectory));
  }

  ArtifactRecord record;
  record.root = canonicalPath(finalDirectory);
  if (record.root.isEmpty()) record.root = finalDirectory;
  record.manifestPath =
      QDir(record.root).filePath(ArtifactStore::artifactManifestFileName());
  record.manifest = manifest;
  if (!resolveEnginePath(record.root, store.root(), manifest, &record.enginePath, reasonKey,
                         reason)) {
    return std::nullopt;
  }
  clearReason(reasonKey, reason);
  return record;
}

// ----------------------------------------------------------------- read/verify

std::optional<ArtifactRecord> readArtifact(const ArtifactStore& store, const QString& buildKey,
                                           QString* reasonKey, QString* reason) {
  if (!store.isOwned(reason)) {
    fail(reasonKey, reason, QStringLiteral("store.unowned"),
         QStringLiteral("artifacts root %1 is not app-owned").arg(store.root()));
    return std::nullopt;
  }
  const QString directoryName = ArtifactStore::artifactDirectoryName(buildKey);
  if (directoryName.isEmpty()) {
    fail(reasonKey, reason, QStringLiteral("artifact.buildKey"),
         QStringLiteral("build key %1 is not a usable key").arg(buildKey));
    return std::nullopt;
  }
  const QString directory = QDir(store.root()).filePath(directoryName);
  if (!pathWithinRoot(store.root(), directory)) {
    fail(reasonKey, reason, QStringLiteral("artifact.escape"),
         QStringLiteral("the artifact directory for %1 resolves outside the artifacts root")
             .arg(buildKey));
    return std::nullopt;
  }
  if (!QFileInfo(directory).isDir()) {
    fail(reasonKey, reason, QStringLiteral("artifact.missing"),
         QStringLiteral("no prepared artifact is published under %1").arg(buildKey));
    return std::nullopt;
  }
  if (!pathHasNoReparsePoints(store.root(), directory)) {
    fail(reasonKey, reason, QStringLiteral("artifact.reparse"),
         QStringLiteral("artifact directory %1 is reached through a reparse point").arg(directory));
    return std::nullopt;
  }

  ArtifactRecord record;
  record.root = canonicalPath(directory);
  if (record.root.isEmpty()) record.root = directory;
  record.manifestPath =
      QDir(record.root).filePath(ArtifactStore::artifactManifestFileName());
  if (!readManifestFile(record.manifestPath, record.root, &record.manifest, reasonKey, reason)) {
    return std::nullopt;
  }
  // The directory name is the key, so a manifest filed under a different one -
  // copied, hand-edited, moved - is refused rather than served.
  if (record.manifest.buildKey != buildKey) {
    fail(reasonKey, reason, QStringLiteral("artifact.buildKeyMismatch"),
         QStringLiteral("the manifest in %1 records build key %2, not %3")
             .arg(directory, record.manifest.buildKey, buildKey));
    return std::nullopt;
  }
  if (!engineMatchesRecord(store, record, &record.enginePath, reasonKey, reason)) {
    return std::nullopt;
  }
  clearReason(reasonKey, reason);
  return record;
}

bool verifyArtifact(const ArtifactStore& store, const ArtifactRecord& record, QString* reasonKey,
                    QString* reason) {
  if (!store.isOwned(reason)) {
    fail(reasonKey, reason, QStringLiteral("store.unowned"),
         QStringLiteral("artifacts root %1 is not app-owned").arg(store.root()));
    return false;
  }
  if (!pathWithinRoot(store.root(), record.root)) {
    fail(reasonKey, reason, QStringLiteral("artifact.escape"),
         QStringLiteral("artifact directory %1 is outside the artifacts root").arg(record.root));
    return false;
  }
  if (!record.manifest.isSelfConsistent(reasonKey, reason)) return false;

  // The manifest is re-read rather than trusted from the record, so an edit made
  // since the record was built is caught here.
  ArtifactManifest onDisk;
  if (!readManifestFile(record.manifestPath, record.root, &onDisk, reasonKey, reason)) return false;
  if (onDisk.buildKey != record.manifest.buildKey ||
      onDisk.engineSha256 != record.manifest.engineSha256 ||
      onDisk.engine.file != record.manifest.engine.file ||
      onDisk.source.onnxSha256 != record.manifest.source.onnxSha256) {
    fail(reasonKey, reason, QStringLiteral("artifact.manifestChanged"),
         QStringLiteral("the manifest on disk no longer describes the artifact the record holds"));
    return false;
  }
  return engineMatchesRecord(store, record, nullptr, reasonKey, reason);
}

std::optional<ArtifactRecord> findArtifactForRequest(const ArtifactStore& store,
                                                     const BuildRequest& request,
                                                     QString* reasonKey, QString* reason) {
  // An incomplete request has no key to look up. Filling the gap with a
  // placeholder would look up an artifact under a key naming no real machine.
  const QString missing = request.firstMissingRequiredField();
  if (!missing.isEmpty()) {
    fail(reasonKey, reason, QStringLiteral("request.incomplete"),
         QStringLiteral("the build request records no %1, so it cannot name a prepared artifact")
             .arg(missing));
    return std::nullopt;
  }
  return readArtifact(store, request.buildKey(), reasonKey, reason);
}

bool verifyArtifactRuntime(const ArtifactRecord& record, const EngineBuildIdentity& observed,
                           QString* reasonKey, QString* reason) {
  const QString observedMissing = observed.firstMissingRequiredField();
  if (!observedMissing.isEmpty()) {
    fail(reasonKey, reason, QStringLiteral("runtime.unobserved"),
         QStringLiteral("the observed build identity records no %1, so it cannot be compared "
                        "against the artifact")
             .arg(observedMissing));
    return false;
  }
  const ManifestBuild recorded = record.manifest.build;
  const ManifestDevice device = record.manifest.device;

  const auto disagree = [&](const QString& field, const QString& was, const QString& now) {
    fail(reasonKey, reason, QStringLiteral("runtime.%1").arg(field),
         QStringLiteral("the artifact was built with %1 = %2, this machine now reports %3")
             .arg(field, was, now));
    return false;
  };

  if (recorded.provider != observed.provider) {
    return disagree(QStringLiteral("provider"), stableName(recorded.provider),
                    stableName(observed.provider));
  }
  if (recorded.runtime != observed.runtimeVersion) {
    return disagree(QStringLiteral("runtime"), recorded.runtime, observed.runtimeVersion);
  }
  if (recorded.plugin != observed.pluginVersion) {
    return disagree(QStringLiteral("plugin"), recorded.plugin, observed.pluginVersion);
  }
  if (recorded.coreSha.toLower() != observed.coreSha.toLower()) {
    return disagree(QStringLiteral("coreSha"), recorded.coreSha, observed.coreSha);
  }
  if (recorded.sourceAbi != observed.sourceAbi) {
    return disagree(QStringLiteral("sourceAbi"), QString::number(recorded.sourceAbi),
                    QString::number(observed.sourceAbi));
  }
  // A driver update is the case this exists for: the engine was compiled against
  // one and cannot be assumed to load on the next.
  if (device.driver != observed.driverVersion) {
    return disagree(QStringLiteral("driver"), device.driver, observed.driverVersion);
  }
  if (device.cuda != observed.cudaVersion) {
    return disagree(QStringLiteral("cuda"), device.cuda, observed.cudaVersion);
  }
  if (device.uuid != observed.deviceUuid) {
    return disagree(QStringLiteral("uuid"), device.uuid, observed.deviceUuid);
  }
  if (device.sm != observed.smText()) {
    return disagree(QStringLiteral("sm"), device.sm, observed.smText());
  }
  if (device.luid != observed.adapterLuid) {
    return disagree(QStringLiteral("luid"), device.luid, observed.adapterLuid);
  }
  if (recorded.precision != observed.precision) {
    return disagree(QStringLiteral("precision"), stableName(recorded.precision),
                    stableName(observed.precision));
  }
  if (recorded.tf32 != observed.tf32) {
    return disagree(QStringLiteral("tf32"), QString::number(recorded.tf32 ? 1 : 0),
                    QString::number(observed.tf32 ? 1 : 0));
  }
  if (recorded.canonicalOptions != observed.canonicalOptions()) {
    return disagree(QStringLiteral("options"), recorded.canonicalOptions,
                    observed.canonicalOptions());
  }
  clearReason(reasonKey, reason);
  return true;
}

bool verifyArtifactSource(const ArtifactRecord& record, const QString& onnxPath,
                          QString* reasonKey, QString* reason) {
  // The manifest records no absolute source path on purpose: the same document has
  // to stay meaningful on another machine, so the caller supplies the path and
  // this compares the bytes.
  const auto measured = measureFile(onnxPath, reason);
  if (!measured) {
    fail(reasonKey, reason, QStringLiteral("source.unreadable"),
         QStringLiteral("the graph file %1 cannot be measured: %2")
             .arg(onnxPath, reason ? *reason : QString()));
    return false;
  }
  if (measured->sha256 != record.manifest.source.onnxSha256) {
    fail(reasonKey, reason, QStringLiteral("source.edited"),
         QStringLiteral("the graph file now hashes to %1 but the artifact was built from %2 - it "
                        "changed in place")
             .arg(measured->sha256, record.manifest.source.onnxSha256));
    return false;
  }
  if (measured->bytes != record.manifest.source.onnxBytes) {
    fail(reasonKey, reason, QStringLiteral("source.edited"),
         QStringLiteral("the graph file is now %1 bytes but the artifact was built from %2")
             .arg(measured->bytes)
             .arg(record.manifest.source.onnxBytes));
    return false;
  }
  clearReason(reasonKey, reason);
  return true;
}

// --------------------------------------------------------------------- remove

bool removeArtifact(const ArtifactStore& store, const QString& buildKey, QString* reasonKey,
                    QString* reason) {
  if (!store.isOwned(reason)) {
    fail(reasonKey, reason, QStringLiteral("store.unowned"),
         QStringLiteral("artifacts root %1 is not app-owned").arg(store.root()));
    return false;
  }
  const QString directoryName = ArtifactStore::artifactDirectoryName(buildKey);
  if (directoryName.isEmpty()) {
    fail(reasonKey, reason, QStringLiteral("artifact.buildKey"),
         QStringLiteral("build key %1 is not a usable key").arg(buildKey));
    return false;
  }
  // The in-use check and the deletion below share the acquisition lock, so a
  // lease can never be granted for bytes this call is about to remove.
  LeaseRegistry& registry = leaseRegistry();
  QMutexLocker locker(&registry.mutex);
  if (registry.counts.value(leaseRegistryKey(canonicalLeaseRoot(store.root()), buildKey), 0) > 0) {
    fail(reasonKey, reason, QStringLiteral("native.artifact.inUse"),
         QStringLiteral("an artifact lease is still held for %1, so it is not removed")
             .arg(buildKey));
    return false;
  }
  const QString directory = QDir(store.root()).filePath(directoryName);
  if (!pathWithinRoot(store.root(), directory) ||
      !pathHasNoReparsePoints(store.root(), directory)) {
    fail(reasonKey, reason, QStringLiteral("artifact.escape"),
         QStringLiteral("artifact directory for %1 is not plainly inside the owned artifacts "
                        "root")
             .arg(buildKey));
    return false;
  }
  if (!QFileInfo(directory).isDir()) {
    fail(reasonKey, reason, QStringLiteral("artifact.missing"),
         QStringLiteral("no artifact is published under %1").arg(buildKey));
    return false;
  }

  const QString manifestPath = QDir(directory).filePath(ArtifactStore::artifactManifestFileName());
  ArtifactManifest manifest;
  if (!readManifestFile(manifestPath, directory, &manifest, reasonKey, reason)) {
    // Without a manifest naming the file and its bytes, nothing here is provably
    // ours, so nothing is removed.
    fail(reasonKey, reason, QStringLiteral("artifact.unproven"),
         QStringLiteral("%1 has no readable manifest, so its contents are not known to be this "
                        "cache's and are left in place")
             .arg(directory));
    return false;
  }
  if (manifest.buildKey != buildKey) {
    fail(reasonKey, reason, QStringLiteral("artifact.buildKeyMismatch"),
         QStringLiteral("the manifest in %1 records build key %2, not %3")
             .arg(directory, manifest.buildKey, buildKey));
    return false;
  }

  ArtifactRecord record;
  record.root = directory;
  record.manifestPath = manifestPath;
  record.manifest = manifest;
  QString enginePath;
  if (!engineMatchesRecord(store, record, &enginePath, reasonKey, reason)) {
    fail(reasonKey, reason, QStringLiteral("artifact.unproven"),
         QStringLiteral("the engine under %1 does not match the bytes its manifest records, so it "
                        "is not removed")
             .arg(directory));
    return false;
  }

  // Only the engine the manifest names, proved byte-for-byte above, and the
  // manifest itself are removed. Anything else the directory holds was not
  // published by this cache and is left alone; the directory goes only when that
  // leaves it empty. A flat engine name - enforced by the manifest invariant -
  // is what makes removing those two files leave exactly an empty directory, so
  // no leftover nested directory can block re-publication.
  if (!QFile::remove(enginePath)) {
    fail(reasonKey, reason, QStringLiteral("artifact.removeFailed"),
         QStringLiteral("the engine %1 could not be removed").arg(enginePath));
    return false;
  }
  if (!QFile::remove(manifestPath)) {
    fail(reasonKey, reason, QStringLiteral("artifact.removeFailed"),
         QStringLiteral("the manifest %1 could not be removed").arg(manifestPath));
    return false;
  }
  QDir().rmdir(directory);
  clearReason(reasonKey, reason);
  return true;
}

bool discardStaging(const ArtifactStore& store, const QString& stagingName, QString* reasonKey,
                    QString* reason) {
  if (!store.isOwned(reason)) {
    fail(reasonKey, reason, QStringLiteral("store.unowned"),
         QStringLiteral("artifacts root %1 is not app-owned").arg(store.root()));
    return false;
  }
  if (!stagingName.startsWith(QLatin1String(kStagingPrefix))) {
    fail(reasonKey, reason, QStringLiteral("artifact.notStaging"),
         QStringLiteral("'%1' is not a staging directory name").arg(stagingName));
    return false;
  }
  QString nameReason;
  if (safeRelativePath(stagingName, &nameReason).isEmpty()) {
    fail(reasonKey, reason, QStringLiteral("artifact.notStaging"),
         QStringLiteral("'%1' is not a safe directory name: %2").arg(stagingName, nameReason));
    return false;
  }
  const QString directory = QDir(store.root()).filePath(stagingName);
  if (!pathWithinRoot(store.root(), directory)) {
    fail(reasonKey, reason, QStringLiteral("artifact.escape"),
         QStringLiteral("'%1' resolves outside the owned artifacts root").arg(stagingName));
    return false;
  }
  if (!QFileInfo(directory).isDir()) {
    fail(reasonKey, reason, QStringLiteral("artifact.missing"),
         QStringLiteral("%1 does not exist").arg(stagingName));
    return false;
  }
  if (!pathHasNoReparsePoints(store.root(), directory)) {
    fail(reasonKey, reason, QStringLiteral("artifact.reparse"),
         QStringLiteral("%1 is reached through a reparse point").arg(stagingName));
    return false;
  }
  return removeTreeWithProof(directory, reasonKey, reason);
}

QStringList stagingDirectories(const ArtifactStore& store) {
  QStringList found;
  if (!store.isOwned(nullptr)) return found;
  const QFileInfoList entries =
      QDir(store.root()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo& entry : entries) {
    if (!entry.fileName().startsWith(QLatin1String(kStagingPrefix))) continue;
    found.append(entry.fileName());
  }
  return found;
}

}  // namespace native
}  // namespace mplayer
