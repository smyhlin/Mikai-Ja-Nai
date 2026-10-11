// M35's assets surface: the owned-model inventory, prepared-artifact cache,
// and one-shot preparation action.
//
// Four rules are load-bearing here.
//
//  * The inventory is the store's own rows, proved against their immutable
//    copies. Its visible columns are model, role and the store's readiness
//    status; the full model facts remain available to data consumers.
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
//  * Downloads live in the Components tab, not here. The source-asset data API
//    below stays for the callers that resolve a catalogue row by digest; it
//    drives no widget of its own.
//
// This header carries no ONNX Runtime type and no ORT include; the inventory
// listing is compiled in only when the model-inspection target is built, and a
// build without it reports the named dependency rather than a fake import.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>
#include <QtWidgets/QWidget>

#include <native/prepare/NativeArtifactStore.hpp>
#include <native/prepare/NativeBuilder.hpp>
#include <native/models/NativeCatalog.hpp>
#include <native/prepare/NativeFrameFixture.hpp>
#include <native/models/NativeModelInspect.hpp>
#include <native/models/NativeModelInventory.hpp>
#include <native/prepare/NativePreparationContext.hpp>
#include <native/models/NativeSpatialAssets.hpp>

class QEvent;
class QShowEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace mikai::app {
class Runner;
}

namespace mplayer::native {
/// The worker's shared cancel gate. Declared here so the panel can hold the
/// control without pulling the inventory header into this one.
class ImportCancellation;
}  // namespace mplayer::native

namespace mikai::app {

/// The observed preparation toolchain: the real build executables, the provider
/// runtimes and the bundle provenance a preparation job builds against.
///
/// Every field comes from the validated selected native bundle's own preparation
/// block; nothing is synthesised and nothing is searched for elsewhere. Each
/// declared path is resolved against that bundle's root. `available` is true only
/// when the three tool executables, an ONNX Runtime directory and at least one
/// provider runtime the block declares are present; otherwise
/// `reasonKey`/`reason` name the first missing fact. The machine identity is
/// measured off the GUI by the toolchain's own describe mode at submit time -
/// never copied from a cache and never defaulted.
struct NativePreparationToolchain {
  bool available = false;
  mplayer::native::BuilderToolPaths tools;
  mplayer::native::BackendAvailability backends;
  QString modulePath;
  /// The prepared provider plugin's content revision, "sha256:<hex>" over the
  /// validated bundle's own aji_trt.dll / aji_dml.dll bytes. A version identifier,
  /// not a SemVer string: it ties a cache entry to the actual compiled plugin the
  /// bundle carries. Empty when the bundle does not carry that backend.
  QString pluginShaTensorRt;
  QString pluginShaDirectMl;
  /// The inference core's source SHA and ABI, from validated bundle provenance
  /// and the actual C ABI. Empty/0 means the bundle did not carry them, and the
  /// worker refuses by name rather than inventing a digest.
  QString coreSha;
  int sourceAbi = 0;
  QString reasonKey;
  QString reason;
};

/// Observes the installed toolchain from the one place this build validates: the
/// selected native bundle's own preparation block, whose executables, provider
/// runtime directories, provider types and core provenance are resolved against
/// that bundle's root. Nothing is created, copied, downloaded or found on PATH;
/// a bundle that declares no preparation provenance leaves `available` false
/// with that named reason.
NativePreparationToolchain observeNativePreparationToolchain();

/// The context provider the panel pulls at readiness and click time. It returns
/// the active playback core's observed source geometry and device, or nothing
/// when nothing has been observed; the panel never caches the answer.
using NativePreparationContextProvider =
    std::function<std::optional<mplayer::native::NativePreparationContext>()>;

/// True when an actual playing or paused video surface exists that a preparation
/// job may probe and capture from. It is the readiness fact distinct from a
/// fully observed context: the device facts come from the filter probe, which is
/// only armed by an explicit preparation, so requiring them up front would make
/// the successful path unreachable. Wired by the application owner; absent,
/// preparation refuses by name.
using NativePreparationSourcePresence =
    std::function<bool(QString* reasonKey, QString* reason)>;

/// Asks the application to arm the live core's native probe for `generation` and
/// deliver the observed context asynchronously via `deliverPreparationContext`
/// or `failPreparationContext`. The panel calls it only after a Prepare click, so
/// opening the settings page never attaches or probes the native path.
using NativePreparationContextRequest = std::function<void(quint64 generation)>;

/// Asks the owner to start one asynchronous capture of `frameIndex` for the
/// preparation job identified by `generation`. The owner answers on the GUI
/// thread with `deliverPreparationFrame` or `failPreparationFrame`; nothing is
/// captured unless a preparation is actually clicked, so opening the settings
/// page never attaches or probes the native path.
using NativePreparationFrameRequest =
    std::function<void(int frameIndex, quint64 generation)>;

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
  QVector<mplayer::native::Backend> backends;
  QString terms;
  /// True only for a row the store offers (availability usable).
  bool usable = false;
  /// The mandatory reason exactly when `usable` is false.
  QString statusKey;
  QString statusReason;
};

/// Builds the view from a row exactly as the store vouched for it. `bytes` is
/// the recorded graph size, never a directory size.
NativeModelView describeStoredModel(const mplayer::native::StoredModel& model);

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
QVector<NativeCacheEntry> enumerateNativeCache(const mplayer::native::ArtifactStore& store);

/// Deletes one entry of an enumerated cache. Only a build key that `entries`
/// proved may be named; an unknown or unproven key is refused without touching
/// the filesystem. A proved entry goes through `removeArtifact`, so a live
/// artifact lease or a directory that stopped verifying is refused by name.
bool deleteNativeCacheEntry(const mplayer::native::ArtifactStore& store,
                            const QVector<NativeCacheEntry>& entries,
                            const QString& buildKey, QString* reasonKey = nullptr,
                            QString* reason = nullptr);

/// The assets panel. It owns no roots: the default constructor uses the
/// app-owned `native/models` and `native/artifacts` locations, and nothing is
/// created until an import actually publishes.
class NativeAssetsPanel : public QWidget {
  Q_OBJECT

public:
  explicit NativeAssetsPanel(QWidget* parent = nullptr);

  /// Requests cancellation of any in-flight import or preparation before this
  /// widget is destroyed, and drops the deliveries that were still queued for
  /// it. The GUI thread never waits for the worker to observe it.
  ~NativeAssetsPanel() override;

  /// The off-GUI executor imports are submitted to. Without one, importing is
  /// refused by name: this widget never runs an import on the GUI thread.
  void setRunner(mikai::app::Runner* runner);

  /// Re-reads the owned store and the artifact cache. Creates neither root.
  /// The first read is deferred until this panel is shown.
  void refresh();

  /// Whether the owned store has been read for this panel yet. Hidden panels
  /// defer that work until first shown.
  bool hasRefreshed() const { return initialRefreshDone_; }

  /// The rows the table currently shows, in store order.
  const QVector<NativeModelView>& models() const { return models_; }
  /// The owned rows exactly as the store returned them, for a consumer that must
  /// resolve a content pin against the real catalogue - the RIFE capability join
  /// - rather than re-reading the store itself. Valid until the next refresh().
  const QVector<mplayer::native::StoredModel>& storedModels() const { return storedModels_; }
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

  /// The pinned spatial-asset catalogue, in the record's order.
  const QVector<mplayer::native::SpatialAsset>& sourceAssets() const { return sourceAssets_; }
  /// The copies actually proved present in the app-owned asset store.
  const QVector<mplayer::native::StoredSpatialAsset>& publishedAssets() const {
    return publishedAssets_;
  }

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

  /// The live source context provider. Wired by the settings dialog to the
  /// active playback core's own observed getter; absent, preparation refuses by
  /// name rather than guessing a geometry or a device.
  void setPreparationContextProvider(NativePreparationContextProvider provider);
  /// The source-presence predicate. Wired by the settings dialog; absent,
  /// preparation refuses by name.
  void setPreparationSourcePresence(NativePreparationSourcePresence presence);

  /// The probe requester. Wired by the settings dialog to the active core's own
  /// native probe; the panel calls it only after a Prepare click, and only when
  /// no context has been observed yet.
  void setPreparationContextRequest(NativePreparationContextRequest request);

  /// The application's answer to one probe request: the context the live core
  /// observed. Ignored unless it belongs to the current job.
  void deliverPreparationContext(const mplayer::native::NativePreparationContext& context);
  /// The application's refusal of one probe request, named. Ignored unless it
  /// belongs to the current job.
  void failPreparationContext(const QString& reasonKey, const QString& reason);

  /// The asynchronous capture requester. Wired by the settings dialog to the
  /// active playback core's own raw-frame capture; absent, preparation refuses by
  /// name. The panel calls it only after a Prepare click.
  void setPreparationFrameRequest(NativePreparationFrameRequest request);

  /// The owner's answer to one requested capture: the actual decoded frame the
  /// core observed, with its own source epoch and observed position. Ignored
  /// unless it belongs to the current job and the frame the job is waiting for.
  void deliverPreparationFrame(int frameIndex, const mplayer::native::FrameFixture& frame,
                               quint64 sourceEpoch, double observedPosition);

  /// The owner's refusal of one requested capture, named. Ignored unless it
  /// belongs to the current job.
  void failPreparationFrame(int frameIndex, const QString& reasonKey, const QString& reason);

  /// The recipe the user selected for preparation, by its stable stored id. An
  /// empty id is the untouched Original pipeline and is never prepared.
  void setRequestedRecipe(const QString& recipeId);
  QString requestedRecipe() const { return requestedRecipeId_; }

  /// Submits the whole selected recipe's preparation to the runner. Returns
  /// false, with `prepareStatusKey()`/`prepareStatusReason()` set, when the
  /// request cannot be submitted at all: no inspector, no runner, no observed
  /// source context, no observed toolchain/identity, no recipe this store holds,
  /// or a job already in flight. Completion is asynchronous and arrives via
  /// `preparationFinished`.
  bool prepareArtifact();

  /// Asks the in-flight preparation to stop. The decision reaches the worker's
  /// own build gate; the GUI thread never waits for it. A build that already
  /// began publishing is refused by name (`native.prepare.commitInProgress`) and
  /// its committed outcome is still reported, never concealed.
  void cancelPrepare();

  bool prepareInFlight() const { return prepareInFlight_; }
  QString prepareStatusKey() const { return prepareStatusKey_; }
  QString prepareStatusReason() const { return prepareStatusReason_; }

  /// The named reason preparation cannot be submitted right now, or both empty
  /// when every prerequisite is observed. Readiness is not a claim that a build
  /// will succeed - only that its prerequisites are present.
  QString preparationReadinessKey() const;
  QString preparationReadinessReason() const;

  /// The backend a registered recipe is built for. It comes from the settings
  /// dialog's own backend choice, never from a runtime fallback: a different
  /// backend is a different, explicit recipe id.
  void setRequestedBackend(mplayer::native::Backend backend);
  mplayer::native::Backend requestedBackend() const { return requestedBackend_; }

  /// The backend a preparation should align the live core to, read fresh at
  /// request time rather than captured. It is the UNSAVED provider selection the
  /// settings dialog is showing, which is not the same thing as
  /// `requestedBackend_`: that one is the backend a recipe is registered for,
  /// while this one is what the core must be probed for. Absent, the stored
  /// preference answers.
  void setPreparationDesiredBackend(std::function<mplayer::native::Backend()> provider);

  /// Registers the usable model `modelId` as a single-spatial recipe for the
  /// requested backend through the owned user-recipe store, and emits
  /// `recipeRequested` with the store's stable id. Only a row the store offers
  /// is accepted; the store re-validates it. Writes no settings - the caller
  /// persists the request through its own existing save path.
  bool selectModelAsRecipe(const QString& modelId);

signals:
  /// The owned model catalogue changed after a refresh.
  void catalogRefreshed();

  /// Delivered on the GUI thread once the submitted import settled - accepted,
  /// refused or cancelled. `ok` is the store's own `ModelImportOutcome::ok`.
  void importFinished(bool ok);

  /// The stable id of a recipe the user just registered. The settings dialog
  /// rebuilds its recipe choices from it and selects it; only its Save writes
  /// the request.
  void recipeRequested(const QString& recipeId);

  /// Delivered on the GUI thread as the real builder reports a stage/phase, so a
  /// surface can show the work actually happening rather than a synthesised
  /// percentage.
  /// One real build event. `stageCount` is the resolved plan's stage count, so a
  /// listener can show "stage 2 of 3" without inventing a percentage; it is 0
  /// before the plan resolves.
  void preparationProgress(int stageIndex, int stageCount, const QString& phase,
                           const QString& detail);
  /// A preparation request was accepted and is now in flight. Emitted before the
  /// plan resolves, so a progress surface can open immediately.
  void preparationStarted();

  /// Delivered on the GUI thread once the submitted preparation settled - the
  /// whole recipe published, or it was refused or cancelled. `ok` is the
  /// builder's own verdict; a cancelled or failed run published nothing.
  void preparationFinished(bool ok);

protected:
  void changeEvent(QEvent* event) override;
  void showEvent(QShowEvent* event) override;

private:
  struct ImportResult;
  struct PrepareResult;

  /// The one panel a worker's bodies deliver to, or null once it is gone.
  ///
  /// A worker captures no widget pointer, so there is nothing off-thread to
  /// check. Every body a dispatch delivered is read here instead, on the GUI
  /// thread, which is the one moment a widget is known to be readable.
  static NativeAssetsPanel* livePanel();

  QSize minimumSizeHint() const override;
  void buildUi();
  /// Restates every static caption in place after a LanguageChange: the section
  /// headings, the hints, the buttons, the accessible names, the two tables'
  /// column headers and the stage fields' captions.
  ///
  /// The model rows are not re-read to retranslate their visible label, role and
  /// status: doing so would move the user's selection and drop an in-flight job's
  /// visible state. What is restated is the panel's own chrome.
  ///
  /// An import or preparation in flight is left running: its worker, its
  /// cancellation gate and its generation are untouched, and its completion
  /// still paints itself in the language that is active when it arrives.
  void retranslateUi();
  void refreshModels();
  void refreshCache();
  /// Re-reads the pinned catalogue and the published copies without touching a
  /// widget; imports and probes resolve those records by digest.
  void refreshSourceData();
  void applyStatus(const QString& key, const QString& reason, QLabel* target);
  void updateImportControls();
  void updateSummaries();
  void finishImport(const std::shared_ptr<ImportResult>& result, quint64 generation);
  void finishPrepare(const std::shared_ptr<PrepareResult>& result, quint64 generation);
  /// The context this instant's provider returns, pulled fresh and never cached.
  std::optional<mplayer::native::NativePreparationContext> currentContext() const;
  /// Starts the next capture the job needs, or submits the worker once every
  /// frame has arrived.
  void requestNextPreparationFrame();
  /// Submits the heavy worker with the frames the core actually delivered.
  void submitPreparationJob(const QVector<mplayer::native::FrameFixture>& frames,
                            quint64 generation);
  /// Refreshes the Prepare button's enablement and its named reason from the
  /// observed context, toolchain, recipe and owned models. Called on every
  /// selection change and refresh; it never starts work.
  void updatePrepareControls();
  /// The named reason preparation cannot be submitted, or both empty when every
  /// prerequisite is observed.
  void prepareRefusal(QString* reasonKey, QString* reason) const;
  /// Applies one builder progress report on the GUI thread.
  void applyPrepareProgress(int stageIndex, int stageCount, const QString& phase,
                            const QString& detail);
  QString inspectionRuntimeReason() const;
  /// True when this translation unit is compiled with the model inspector.
  bool inspectorBuilds() const;

  mikai::app::Runner* runner_ = nullptr;
  /// Cleared in this widget's destructor, read by a delivered GUI body. Paired
  /// with the binder, it is what keeps a queued completion from reaching an
  /// owner that no longer exists.
  std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
  /// The panel the current bodies deliver to, null after the destructor.
  static NativeAssetsPanel* livePanel_;
  mplayer::native::InspectionRuntime runtime_;
  QString modelRoot_;
  mplayer::native::ArtifactStore artifacts_;
  bool initialRefreshDone_ = false;

  QVector<mplayer::native::SpatialAsset> sourceAssets_;
  QVector<mplayer::native::StoredSpatialAsset> publishedAssets_;

  QVector<NativeModelView> models_;
  QVector<mplayer::native::StoredModel> storedModels_;
  QVector<NativeCacheEntry> cache_;
  QStringList staging_;
  mplayer::native::Backend requestedBackend_ = mplayer::native::Backend::TensorRT;
  /// The desired backend, pulled at request time. Never cached: the user can
  /// change the combo while a job is being armed, and the probe must be asked
  /// for what the surface says now.
  std::function<mplayer::native::Backend()> preparationDesiredBackendProvider_;

  bool importInFlight_ = false;
  quint64 importGeneration_ = 0;
  std::shared_ptr<mplayer::native::ImportCancellation> importCancel_;
  QString statusKey_;
  QString statusReason_;

  // Preparation. The context and environment are pulled from their providers,
  // never cached; the job's own cancel gate is the builder's publication CAS.
  NativePreparationContextProvider contextProvider_;
  NativePreparationSourcePresence sourcePresence_;
  NativePreparationContextRequest contextRequest_;
  NativePreparationFrameRequest frameRequest_;
  /// The context the current job resolved (observed or probed), and whether a
  /// probe is still outstanding.
  std::optional<mplayer::native::NativePreparationContext> jobContext_;
  bool contextPending_ = false;
  /// Capturing state: the frames the current job has, how many it needs, and the
  /// index whose capture is outstanding.
  QVector<mplayer::native::FrameFixture> capturedFrames_;
  int framesNeeded_ = 1;
  int frameIndexRequested_ = -1;
  bool capturePending_ = false;
  /// The panel's own observation, taken on the GUI thread at refresh time so a
  /// selection change never re-walks the toolchain.
  NativePreparationToolchain observedToolchain_;
  QString requestedRecipeId_;
  bool prepareInFlight_ = false;
  quint64 prepareGeneration_ = 0;
  std::shared_ptr<mplayer::native::NativeBuildCancellation> prepareCancel_;
  QString prepareStatusKey_;
  QString prepareStatusReason_;

  QTableWidget* modelTable_ = nullptr;
  QTableWidget* cacheTable_ = nullptr;
  QListWidget* stagingList_ = nullptr;
  QLabel* modelStatus_ = nullptr;
  // Retranslation handles: the section headings and hints this panel writes,
  // which are locals in buildUi() and would otherwise only be reachable by
  // walking the layout.
  QLabel* inventoryHeading_ = nullptr;
  QLabel* preparationHeading_ = nullptr;
  QLabel* cacheHeading_ = nullptr;
  QLabel* stagingHeading_ = nullptr;
  QLabel* cacheDeleteHint_ = nullptr;
  QLabel* stagingDiscardHint_ = nullptr;
  QLabel* stageWidthLabel_ = nullptr;
  QLabel* stageHeightLabel_ = nullptr;
  QLabel* selectRecipeReason_ = nullptr;
  QLabel* importStatus_ = nullptr;
  QLabel* preparationReason_ = nullptr;
  QLabel* cacheStatus_ = nullptr;
  QPushButton* importButton_ = nullptr;
  QPushButton* cancelImportButton_ = nullptr;
  QPushButton* selectRecipeButton_ = nullptr;
  QPushButton* prepareButton_ = nullptr;
  QPushButton* cancelPrepareButton_ = nullptr;
  QLabel* prepareProgress_ = nullptr;
  QPushButton* deleteCacheButton_ = nullptr;
  QPushButton* discardStagingButton_ = nullptr;
  QPushButton* importDetailsToggle_ = nullptr;
  QPushButton* cacheDetailsToggle_ = nullptr;
  QWidget* importDetails_ = nullptr;
  QWidget* cacheDetails_ = nullptr;
  QLineEdit* stageWidthEdit_ = nullptr;
  QLineEdit* stageHeightEdit_ = nullptr;
  QLabel* modelSummary_ = nullptr;
  QLabel* stagingNote_ = nullptr;
};

}  // namespace mikai::app
