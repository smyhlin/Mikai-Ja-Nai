// The prepared-artifact cache: where an artifact lives, how one is published
// atomically, how one is proved still good, and what may be deleted.
//
// Publishing stages under a name no lookup selects, so a cancelled or crashed
// build is never mistaken for a prepared one. Deletion is proven and path
// containment is decided by `NativePaths`, so there is one policy rather than
// two.

#pragma once

#include <memory>
#include <optional>

#include <QtCore/QString>

#include "native/bundle/NativeManifest.hpp"

namespace mplayer {
namespace native {

/// The bytes of one file as they actually are on disk.
struct FileDigest {
  QString sha256;
  qint64 bytes = 0;
};

/// Hashes `path` by streaming it, never reading it whole: a multi-gigabyte graph
/// is an ordinary input here. Returns nothing with a reason when the path is not
/// a regular file, cannot be opened, or fails mid-read.
std::optional<FileDigest> measureFile(const QString& path, QString* reason = nullptr);

/// One artifact that has been proved present and intact.
struct ArtifactRecord {
  /// The owned directory this artifact occupies, absolute and canonical.
  QString root;
  QString manifestPath;
  /// Absolute and canonical, inside `root`.
  QString enginePath;
  ArtifactManifest manifest;

  QString buildKey() const { return manifest.buildKey; }
};

/// An artifacts root. Production constructs the default root, which must lie
/// inside the app-owned `native/artifacts` hierarchy; an explicit root is
/// caller-vouched (a test's temporary directory) and is used as given.
class ArtifactStore {
 public:
  /// `<applicationDirPath>/native/artifacts`. Never created.
  ArtifactStore();
  /// An explicit root. Constructing does not validate or create anything;
  /// `isOwned()` does both.
  explicit ArtifactStore(QString root);

  const QString& root() const { return root_; }

  /// True when this root may be written and deleted in: absolute, no reparse
  /// point, creatable if absent, and - for the default root - inside the
  /// app-owned `native/artifacts` hierarchy, so an app-built store can never be
  /// pointed at a user's model folder.
  bool isOwned(QString* reason = nullptr) const;

  /// Creates the root if it is owned and absent. The only writer of it.
  bool ensureOwned(QString* reason = nullptr) const;

  /// The directory name for `buildKey`, and the manifest file name inside it.
  static QString artifactDirectoryName(const QString& buildKey);
  static QString artifactManifestFileName();

 private:
  QString root_;
  bool explicitRoot_ = false;
};

/// A held claim on one published artifact. Taking one proves the artifact
/// present and intact and pins the owned root and build key it was taken from;
/// copies share that one claim, and removal of that pair stays refused until the
/// last copy is destroyed.
class ArtifactLease {
 public:
  ArtifactLease() = default;
  ArtifactLease(const ArtifactLease&) = default;
  ArtifactLease(ArtifactLease&&) noexcept = default;
  ArtifactLease& operator=(const ArtifactLease&) = default;
  ArtifactLease& operator=(ArtifactLease&&) noexcept = default;
  ~ArtifactLease() = default;

  /// False for a default-constructed lease, which holds nothing.
  bool isValid() const;

  /// The canonical owned root and build key the claim is filed under, empty when
  /// invalid.
  const QString& root() const;
  const QString& buildKey() const;

 private:
  friend std::optional<ArtifactLease> acquireArtifactLease(const ArtifactStore&, const QString&,
                                                           QString*, QString*);
  struct Claim;
  std::shared_ptr<Claim> claim_;
};

/// Takes a lease on the artifact filed under `buildKey`, proving it present and
/// intact exactly as `readArtifact` does, or nothing with the reason. Acquiring
/// and removing are atomic against each other within this process.
std::optional<ArtifactLease> acquireArtifactLease(const ArtifactStore& store,
                                                  const QString& buildKey,
                                                  QString* reasonKey = nullptr,
                                                  QString* reason = nullptr);

/// Publishes a produced engine under its manifest, atomically.
///
/// The engine is copied into a `.staging-*` sibling and re-hashed there; the
/// staged hash must equal `manifest.engineSha256`, because publishing bytes the
/// manifest does not describe is the accumulation this cache exists to prevent.
/// Refuses an inconsistent manifest, an unowned root, an unsafe engine name, and
/// an existing directory that does not verify (removing it is `removeArtifact`'s
/// job, and only with proof). A failure part-way removes the staging directory
/// this call created and leaves published artifacts untouched.
std::optional<ArtifactRecord> publishArtifact(const ArtifactStore& store,
                                              const ArtifactManifest& manifest,
                                              const QString& scratchEnginePath,
                                              QString* reasonKey = nullptr,
                                              QString* reason = nullptr);

/// The artifact filed under `buildKey`, proved against its own recorded bytes,
/// or nothing with the reason it is not usable. Absent, unreadable, stale and
/// corrupt are different answers and the reason key names which.
std::optional<ArtifactRecord> readArtifact(const ArtifactStore& store, const QString& buildKey,
                                           QString* reasonKey = nullptr,
                                           QString* reason = nullptr);

/// Re-proves an already-read record against the bytes on disk. Use after
/// anything that might have touched the cache.
bool verifyArtifact(const ArtifactStore& store, const ArtifactRecord& record,
                    QString* reasonKey = nullptr, QString* reason = nullptr);

/// The artifact that satisfies `request`, or nothing with the reason. This is
/// the lookup a preparation step performs before deciding to rebuild: it is the
/// build key that selects, and the recorded product digest that proves.
std::optional<ArtifactRecord> findArtifactForRequest(const ArtifactStore& store,
                                                     const BuildRequest& request,
                                                     QString* reasonKey = nullptr,
                                                     QString* reason = nullptr);

/// Refuses a record whose recorded device/runtime facts disagree with what is
/// observed now, naming the first field that moved. A driver update is the case
/// this exists for: the artifact's build key no longer names a machine that can
/// load it.
bool verifyArtifactRuntime(const ArtifactRecord& record, const EngineBuildIdentity& observed,
                           QString* reasonKey = nullptr, QString* reason = nullptr);

/// Detects an in-place edit of the graph file an artifact was built from.
/// `onnxPath` is re-measured by streaming and compared with the manifest's
/// recorded digest and size; a difference refuses, because the artifact describes
/// bytes that no longer exist.
bool verifyArtifactSource(const ArtifactRecord& record, const QString& onnxPath,
                          QString* reasonKey = nullptr, QString* reason = nullptr);

/// Removes one artifact, and only with proof: its manifest must parse strictly,
/// its engine must be a regular non-reparse file inside the store root, and both
/// the recorded size and the recorded SHA-256 must match the bytes on disk. The
/// proved directory is first atomically quarantined so deletion failure cannot
/// leave a half-valid artifact under its content id. Only the engine it names
/// and the manifest are removed; unknown entries are preserved.
///
/// Refuses with `native.artifact.inUse` while a lease on the same owned root and
/// build key is still held, including one taken through another store instance.
/// A record that does not verify is left as it is, with the reason; there is
/// deliberately no way to clear the cache wholesale, since an unproven file is
/// not known to be ours.
bool removeArtifact(const ArtifactStore& store, const QString& buildKey,
                    QString* reasonKey = nullptr, QString* reason = nullptr);

/// Discards one staging directory left by a cancelled or crashed build. Only a
/// `.staging-*` directory that is directly inside the owned root qualifies, and
/// its contents must be regular non-reparse files inside it.
bool discardStaging(const ArtifactStore& store, const QString& stagingName,
                    QString* reasonKey = nullptr, QString* reason = nullptr);

/// The `.staging-*` directories currently directly inside the owned root, with
/// nothing inside any of them examined. A staging directory is never a candidate
/// artifact, so this is what recovery lists.
QStringList stagingDirectories(const ArtifactStore& store);

}  // namespace native
}  // namespace mplayer
