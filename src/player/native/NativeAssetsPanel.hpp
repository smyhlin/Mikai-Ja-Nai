// M35's assets surface: the owned-model inventory, the prepared-artifact cache,
// and the pinned source-asset store this build can honestly offer downloads into.
//
// Four rules are load-bearing here.
//
//  * The inventory is the store's own rows, proved against their immutable
//    copies. A non-FP16 graph is shown with `model.io.unsupported` and its
//    observed dtype, and it is NOT selectable - recording why a graph cannot be
//    used is the point, converting it or claiming it runs is not.
//
//  * Importing is never done on the GUI thread. The inspection and the publish
//    run on the runner's pool, cancellation is a shared atomic gate the worker
//    reads and the completion re-checks, and an outcome is applied only if this
//    panel still exists and the request is still current. A stale completion
//    changes nothing; a cancel that arrives after publishing has begun is
//    refused by name (`native.import.commitInProgress`) rather than reported as
//    a cancellation, so a committed publication is never dropped.
//
//  * Deleting from the cache is proved by `readArtifact`/`removeArtifact`: only
//    a directory that verifies is deletable, and only when no artifact lease
//    holds it (that refusal is `removeArtifact`'s, and is surfaced verbatim). No
//    staging directory, foreign file or unowned path is ever removed.
//
//  * A source asset is downloaded, verified and published, in that order, off
//    the GUI thread. The five rows are the pinned original graphs and a row is
//    selected by its digest, never its label; the payload is accepted only when
//    it hashes and measures exactly as the catalogue says, and only then does
//    `publishSpatialAssetBytes` write it into the app-owned store. A cancelled
//    download that lost the publication race is still reported as committed. The
//    prepared engine remains unavailable - a downloaded graph is an input, not a
//    Ready pipeline - and RIFE weights are not offered: their lineage is open.
//
// This header carries no ONNX Runtime type and no ORT include; the inventory
// listing is compiled in only when the model-inspection target is built, and a
// build without it reports the named dependency rather than a fake import.

#pragma once

#include <atomic>
#include <memory>

#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>
#include <QtWidgets/QWidget>

#include "NativeArtifactStore.hpp"
#include "NativeModelInspect.hpp"
#include "NativeModelInventory.hpp"
#include "NativeSpatialAssets.hpp"

class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace mikai::app {
class Runner;
}

namespace mikai::backend {
class MikaiClient;
}

namespace mplayer::native {

/// The inventory fields M35 shows for one owned model, in one place so the table
/// and any consumer read the same values instead of re-deriving them.
struct NativeModelView {
  QString id;
  QString label;
  QString contentId;
  QString role;
  QString dtype;
  qint64 bytes = 0;
  /// The backends the store actually recorded for this row, in the store's own
  /// vocabulary - never re-derived here from the dtype or the machine.
  QVector<Backend> backends;
  QString terms;
  /// True only for a row the store offers (availability usable).
  bool usable = false;
  /// The mandatory reason exactly when `usable` is false.
  QString statusKey;
  QString statusReason;
};

/// Builds the view from a row exactly as the store vouched for it. `bytes` is
/// the recorded graph size, never a directory size.
NativeModelView describeStoredModel(const StoredModel& model);

/// One entry of the prepared-artifact cache as it actually is on disk. A
/// directory that does not verify is still listed - read-only - so the user can
/// see it; it is never deletable, and its reason names why.
struct NativeCacheEntry {
  QString buildKey;
  QString directory;
  QString enginePath;
  qint64 engineBytes = 0;
  bool proven = false;
  QString reasonKey;
  QString reason;
};

/// Enumerates the artifact directories directly inside an owned root, and proves
/// each one with a strict `readArtifact`. A `.staging-*` directory, a foreign
/// directory and anything that does not verify is reported unproven; nothing is
/// removed here.
QVector<NativeCacheEntry> enumerateNativeCache(const ArtifactStore& store);

/// Deletes one entry of an enumerated cache. Only a build key that `entries`
/// proved may be named; an unknown or unproven key is refused without touching
/// the filesystem. A proved entry goes through `removeArtifact`, so a live
/// artifact lease or a directory that stopped verifying is refused by name.
bool deleteNativeCacheEntry(const ArtifactStore& store, const QVector<NativeCacheEntry>& entries,
                            const QString& buildKey, QString* reasonKey = nullptr,
                            QString* reason = nullptr);

/// The worker's shared cancel gate. Declared here so the panel can hold the
/// control without pulling the inventory header into this one.
class ImportCancellation;

/// The assets panel. It owns no roots: the default constructor uses the
/// app-owned `native/models` and `native/artifacts` locations, and nothing is
/// created until an import actually publishes.
class NativeAssetsPanel : public QWidget {
  Q_OBJECT

public:
  explicit NativeAssetsPanel(QWidget* parent = nullptr);

  /// Requests cancellation of any in-flight import before this widget is
  /// destroyed. The GUI thread never waits for the worker to observe it.
  ~NativeAssetsPanel() override;

  /// The off-GUI executor imports are submitted to. Without one, importing is
  /// refused by name: this widget never runs an import on the GUI thread.
  void setRunner(mikai::app::Runner* runner);

  /// Re-reads the owned store and the artifact cache. Creates neither root.
  void refresh();

  /// The rows the table currently shows, in store order.
  const QVector<NativeModelView>& models() const { return models_; }
  /// The cache entries the table currently shows, in name order.
  const QVector<NativeCacheEntry>& cache() const { return cache_; }

  /// Submits one import of `sourcePath` at `stageGeometry` to the runner. The
  /// geometry is the stage size a dynamic graph's CPU reference resolves at; it
  /// is never defaulted and never read from the file name. Returns false, with
  /// `statusKey`/`statusReason` set, when the request cannot be submitted at all
  /// (no runner, no inspector, no runtime, an import already in flight, or a
  /// non-positive geometry). Completion is asynchronous and arrives via
  /// `importFinished`.
  bool importModel(const QString& sourcePath, const QSize& stageGeometry);

  /// Asks the in-flight import to stop. The decision reaches the worker thread;
  /// the GUI thread never waits for it.
  void cancelImport();

  /// Deletes one proved cache entry by build key, through `removeArtifact`, so
  /// an in-use lease or an unverifiable directory is refused by name. Returns
  /// false with `statusKey`/`statusReason` set on any refusal.
  bool deleteCacheEntry(const QString& buildKey);

  /// The backend client real downloads run through. The caller owns it (the
  /// settings dialog passes its own); this panel only borrows it. `fetchBytes`
  /// is the anonymous artwork path and bypasses the API limiter, so a download
  /// costs no API budget and needs no credential. Without one, downloading is
  /// refused by name.
  void setClient(mikai::backend::MikaiClient* client);

  /// The pinned catalogue this panel offers, in the record's order.
  const QVector<SpatialAsset>& sourceAssets() const { return sourceAssets_; }
  /// The copies actually proved present in the app-owned asset store.
  const QVector<StoredSpatialAsset>& publishedAssets() const { return publishedAssets_; }

  /// Selects a catalogue row by its stable id (`sha256:<hex>`). Returns false,
  /// with `statusKey`/`statusReason` set, for an id this catalogue does not
  /// carry. The id, never the label, is the selection.
  bool selectSourceAsset(const QString& id);
  QString selectedSourceAssetId() const { return selectedSourceAssetId_; }

  /// Submits one real download of the selected pinned asset through the runner
  /// and the backend client. The payload is verified against the catalogue's
  /// digest and length before any publication. Returns false, with
  /// `statusKey`/`statusReason` set, when it cannot be submitted at all (no
  /// selection, no client, no runner, or a download already in flight).
  /// Completion is asynchronous and arrives via `downloadFinished`.
  bool downloadSelectedSourceAsset();

  /// Asks an in-flight download to stop before publication begins. The GUI
  /// thread never waits for the worker. A download that already began publishing
  /// is refused by name (`native.download.commitInProgress`) and its committed
  /// result is still reported, never concealed.
  void cancelDownload();

  bool downloadInFlight() const { return downloadInFlight_; }

  /// Absolute path of a copy `publishedAssets()` proved, or empty.
  QString publishedAssetPath(const QString& id) const;

  /// Imports a verified published graph through the existing ONNX inspector with
  /// an explicit stage geometry, off the GUI thread, exactly like `importModel`.
  /// It publishes into the owned model store only; it does not build, prepare or
  /// mark any pipeline Ready. A missing or unverified copy is refused by name.
  bool importPublishedAsset(const QString& id, const QSize& stageGeometry);

  bool importInFlight() const { return importInFlight_; }
  QString statusKey() const { return statusKey_; }
  QString statusReason() const { return statusReason_; }

  /// The backend a registered recipe is built for. It comes from the settings
  /// dialog's own backend choice, never from a runtime fallback: a different
  /// backend is a different, explicit recipe id.
  void setRequestedBackend(Backend backend);
  Backend requestedBackend() const { return requestedBackend_; }

  /// Registers the usable model `modelId` as a single-spatial recipe for the
  /// requested backend through the owned user-recipe store, and emits
  /// `recipeRequested` with the store's stable id. Only a row the store offers
  /// is accepted; the store re-validates it. Writes no settings - the caller
  /// persists the request through its own existing save path.
  bool selectModelAsRecipe(const QString& modelId);

signals:
  /// Delivered on the GUI thread once the submitted import settled - accepted,
  /// refused or cancelled. `ok` is the store's own `ModelImportOutcome::ok`.
  void importFinished(bool ok);

  /// Delivered on the GUI thread once the submitted download settled - the
  /// asset was verified and published, or it was refused or cancelled. `ok` is
  /// the store's own verdict.
  void downloadFinished(bool ok);

  /// The stable id of a recipe the user just registered. The settings dialog
  /// rebuilds its recipe choices from it and selects it; only its Save writes
  /// the request.
  void recipeRequested(const QString& recipeId);

private:
  struct ImportResult;
  struct DownloadResult;

  QSize minimumSizeHint() const override;
  void buildUi();
  void refreshModels();
  void refreshCache();
  void refreshSourceAssets();
  void applyStatus(const QString& key, const QString& reason, QLabel* target);
  /// Shows as much of the real store path as actually fits: elided in the middle
  /// only when the whole of it does not, so the caption never dictates the
  /// page's minimum width and nothing about the store is concealed.
  void updateStoreRootCaption();
  void resizeEvent(QResizeEvent* event) override;
  void updateImportControls();
  void updateDownloadControls();
  void finishImport(const std::shared_ptr<ImportResult>& result, quint64 generation);
  void finishDownload(const std::shared_ptr<DownloadResult>& result, quint64 generation);
  QString inspectionRuntimeReason() const;
  /// True when this translation unit is compiled with the model inspector.
  bool inspectorBuilds() const;

  mikai::app::Runner* runner_ = nullptr;
  mikai::backend::MikaiClient* client_ = nullptr;
  InspectionRuntime runtime_;
  QString modelRoot_;
  ArtifactStore artifacts_;

  QVector<SpatialAsset> sourceAssets_;
  QVector<StoredSpatialAsset> publishedAssets_;
  QString selectedSourceAssetId_;
  bool downloadInFlight_ = false;
  quint64 downloadGeneration_ = 0;
  std::shared_ptr<ImportCancellation> downloadCancel_;

  QVector<NativeModelView> models_;
  QVector<StoredModel> storedModels_;
  QVector<NativeCacheEntry> cache_;
  QStringList staging_;
  Backend requestedBackend_ = Backend::TensorRT;

  bool importInFlight_ = false;
  quint64 importGeneration_ = 0;
  std::shared_ptr<ImportCancellation> importCancel_;
  QString statusKey_;
  QString statusReason_;

  QTableWidget* modelTable_ = nullptr;
  QTableWidget* cacheTable_ = nullptr;
  QListWidget* stagingList_ = nullptr;
  QLabel* storeRootLabel_ = nullptr;
  QLabel* modelStatus_ = nullptr;
  QLabel* selectRecipeReason_ = nullptr;
  QLabel* importStatus_ = nullptr;
  QLabel* preparationReason_ = nullptr;
  QLabel* cacheStatus_ = nullptr;
  QLabel* stagingStatus_ = nullptr;
  QPushButton* importButton_ = nullptr;
  QPushButton* cancelImportButton_ = nullptr;
  QPushButton* selectRecipeButton_ = nullptr;
  QPushButton* prepareButton_ = nullptr;
  QPushButton* downloadButton_ = nullptr;
  QPushButton* deleteCacheButton_ = nullptr;
  QPushButton* discardStagingButton_ = nullptr;
  QLineEdit* stageWidthEdit_ = nullptr;
  QLineEdit* stageHeightEdit_ = nullptr;

  QTableWidget* sourceTable_ = nullptr;
  QLabel* sourceStatus_ = nullptr;
  QLabel* licenseLabel_ = nullptr;
  QProgressBar* downloadProgress_ = nullptr;
  QPushButton* downloadCancelButton_ = nullptr;
  QPushButton* importSourceButton_ = nullptr;
};

}  // namespace mplayer::native
