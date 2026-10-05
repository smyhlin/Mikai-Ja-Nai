// The five original AnimeJaNai spatial ONNX graphs: their pinned identity, the
// licence that travels with them, and the app-owned `native/assets` store a
// verified download lands in.
//
// This module is QtCore only. It carries no QtNetwork and no QtWidgets type, no
// ONNX Runtime include and no engine type: fetching a graph and publishing it
// are separate steps, and this file owns only the second. A download is proved
// by digest *and* length before a single byte is written; a published graph is
// write-once, so an existing directory is never rewritten, and one that no
// longer verifies is refused rather than replaced.
//
// The rights position is the acquisition record's, not an invention: these bytes
// are CC BY-NC-SA 4.0, they are held privately and non-commercially on this
// machine, and every copy is stored with the creator, the pinned source commit
// and the licence notice that the licence requires to be retained.

#pragma once

#include <optional>

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace mplayer {
namespace native {

/// The licence facts that must travel with a copy of these bytes. CC BY-NC-SA
/// 4.0 is an attribution licence: the creator, the source, the licence link and
/// the disclaimer are retained beside every stored graph.
struct SpatialAssetLicense {
  /// "CC-BY-NC-SA-4.0".
  QString spdx;
  /// The licence's own long name, verbatim.
  QString name;
  /// The licence deed, and the legal code.
  QString url;
  QString legalCodeUrl;
  /// The retention notice this app writes beside every copy.
  QString notice;
  /// What this app does with the bytes, stated plainly.
  QString useScope;

  bool isEmpty() const { return spdx.isEmpty(); }
};

/// One catalogue row: a pinned original graph, identified by its own content
/// digest. The name is a label; the digest is the identity.
struct SpatialAsset {
  /// "sha256:<64 hex>" - the graph's own digest, and this store's stable id.
  QString id;
  /// The upstream file name.
  QString name;
  qint64 bytes = 0;
  /// Lowercase hex SHA-256, never the truncated form.
  QString sha256;
  /// The anonymous, pinned raw URL this graph was acquired from.
  QString url;
  /// The path inside the upstream repository.
  QString sourcePath;
  /// The upstream commit every URL is pinned to.
  QString commit;

  bool isValid() const { return !id.isEmpty() && !sha256.isEmpty() && bytes > 0; }
};

/// The pinned upstream commit (tag 3.6.3) every catalogue URL is built from.
QString spatialAssetCommit();
/// The upstream repository the graphs were acquired from.
QString spatialAssetRepository();

/// The licence record, including the retention notice written beside a copy.
SpatialAssetLicense spatialAssetLicense();

/// The five original graphs, in the acquisition record's order. This is the
/// whole catalogue: RIFE weights are a separate kind with an open lineage and
/// are deliberately not here.
QVector<SpatialAsset> spatialAssetCatalog();

/// The catalogue row whose stable id is `id`, or null. The id is the digest.
const SpatialAsset* findSpatialAsset(const QString& id);

/// `AppLocalDataLocation()/native/assets`. Never created by this call.
QString spatialAssetRoot();

/// One published copy: the catalogue row plus the immutable file on disk.
struct StoredSpatialAsset {
  SpatialAsset asset;
  QString directory;
  QString graphPath;
  bool verified = false;
  QString reasonKey;
  QString reason;
};

/// The outcome of publishing bytes as a catalogue row.
struct SpatialAssetPublication {
  bool ok = false;
  /// True only when this call actually created a new immutable copy.
  bool published = false;
  QString reasonKey;
  QString reason;
  QString id;
  QString directory;
  QString graphPath;
};

/// An app-owned assets root. Production constructs the default root, which must
/// lie inside the app-owned `native/assets` hierarchy; an explicit root is
/// caller-vouched - a test's temporary directory is why the constructor exists -
/// and is used as given, but it is still never reparse-redirected.
class SpatialAssetStore {
 public:
  /// The app-owned `native/assets` root. Never created by this call.
  SpatialAssetStore();
  /// An explicit, caller-vouched root. Constructing creates nothing.
  explicit SpatialAssetStore(QString root);

  const QString& root() const { return root_; }

  /// True when this root may be written in: absolute, creatable, not a reparse
  /// point, and - for the default root - inside the app-owned hierarchy.
  bool isOwned(QString* reason = nullptr) const;
  /// Creates the root if it is owned and absent. The only writer of it.
  bool ensureOwned(QString* reason = nullptr) const;

  /// The directory name for `id`: the bare digest, or empty when `id` is not a
  /// well-formed "sha256:<64 hex>".
  static QString directoryName(const QString& id);
  static QString manifestFileName();
  static QString graphFileName();

  /// Verifies `bytes` against `asset` and publishes an immutable copy. A digest
  /// or length mismatch writes nothing. `asset` must be a catalogue row: an
  /// unknown id is refused before the root is touched.
  SpatialAssetPublication publish(const SpatialAsset& asset, const QByteArray& bytes,
                                  QString* reasonKey = nullptr, QString* reason = nullptr);

  /// Every published copy in this root, each re-proved against its immutable
  /// file. A directory that no longer verifies is returned with `verified` false
  /// and a reason - never dropped, never deleted.
  QVector<StoredSpatialAsset> stored(QString* reason = nullptr) const;

 private:
  QString root_;
  bool explicitRoot_ = false;
};

/// The convenience the tests and the panel use: publish into `root` (empty means
/// the app-owned `native/assets`). Identical rules to `SpatialAssetStore::publish`.
SpatialAssetPublication publishSpatialAssetBytes(const SpatialAsset& asset, const QByteArray& bytes,
                                                const QString& root = QString());

/// Enumerate the published copies in `root` (empty means the app-owned root).
QVector<StoredSpatialAsset> storedSpatialAssets(const QString& root = QString());

}  // namespace native
}  // namespace mplayer
