#pragma once

#include <native/components/AnimeJanaiRuntime.hpp>
#include <native/components/NativeComponentProvisioning.hpp>
#include <native/models/NativeSpatialAssets.hpp>
#include <state/NativeComponentProvisioner.hpp>

#include <QMap>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <QVariant>

#include <atomic>
#include <memory>
#include <optional>

class QComboBox;
class QEvent;
class QLabel;
class QProgressBar;
class QPushButton;
class QResizeEvent;
class QCheckBox;

namespace mikai::app {

class Runner;

}  // namespace mikai::app

namespace mikai::backend {
class MikaiClient;
}

namespace mikai::app {

/// The Components tab: downloadable Aji runtime/backend closures, pinned
/// GPU-matched TensorRT packs, RIFE graphs and AnimeJaNai upscaling graphs
/// presented as four selectable cards, with manual Verify and Apply changes actions.
///
/// Nothing listed here ships in a portable build or an installer. Every element
/// is either GPU-specific (a builder resource for one compute capability) or
/// carries third-party terms this project does not redistribute, so the bytes
/// are fetched on the machine that will use them and proved by digest before
/// they are stored. The panel only reads app-owned stores and hands user-initiated
/// downloads to the component provisioner or off-GUI worker.
class ComponentStoreTab : public QWidget {
    Q_OBJECT

public:
    explicit ComponentStoreTab(QWidget* parent = nullptr);
    ~ComponentStoreTab() override;

    /// The off-GUI executor used for the AnimeJaNai closure and model downloads.
    /// Without one, those downloads are refused with a named reason.
    void setRunner(Runner* runner);
    /// The backend client real graph downloads run through. Without one, model
    /// downloads are refused by name.
    void setClient(backend::MikaiClient* client);
    /// The TensorRT provisioner this tab drives for the component packs.
    /// Optional and null-safe.
    void setComponentProvisioner(NativeComponentProvisioner* provisioner);

    /// Re-reads both stores asynchronously and re-renders. Creates neither root
    /// and downloads nothing.
    void refresh();
    QString setupPrompt(qint64* bytes = nullptr) const;
    bool inventoryReady() const { return inventoryReady_; }
    bool acquisitionRunning() const { return acquisitionSequenceActive_ || acquisitionChecking_ ||
                                             inventoryScanning_ || downloadActive() || verifying_; }

    /// The manual check: re-hashes both AnimeJaNai closures and every installed
    /// component file, and re-proves every stored graph against its digest.
    /// The hashing runs on an off-GUI worker; only the rendering happens here.
    void verifyAll();

    /// Reviews and installs the selected runtime/backend closure, RIFE archive,
    /// and the complete pinned AnimeJaNai upscaling-model pack.
    void downloadMissing();
    /// Selects the GPU-matched backend and missing upscaling packs after explicit
    /// setup confirmation, without a second confirmation. Clears any RIFE
    /// selection to match that upscaling-only offer.
    /// The tab's Apply changes action instead preserves the user's selections.
    void installRecommended();
    /// Stops the download or verification the Cancel button stops. Explicit:
    /// closing Settings does not call this.
    void cancelAcquisition();
    /// Publishes the current download, if one is running, to whoever is showing
    /// progress outside this tab. A provisioner started without going through
    /// the tab's own buttons still has to reach that readout.
    void syncAcquisitionState();
    /// True while a pack or model download is running. Verification does not
    /// count: it stays inside Settings.
    bool downloadActive() const {
        return (provisioner_ != nullptr && provisioner_->running()) ||
               animeJanaiRuntimeDownloading_ || rifeDownloading_ || modelsDownloading_;
    }

signals:
    void inventoryUpdated();
    /// Aggregate acquisition activity for the window-level text hint.
    /// `started` is true only on the transition into a new acquisition.
    void acquisitionProgress(const QString& status, bool started);
    /// The acquisition that `acquisitionProgress` announced has left the running
    /// state. Verification alone does not emit this.
    void acquisitionIdle();

public:
    QString statusKey() const { return statusKey_; }
    /// The current status line in the live language. App-owned text is
    /// rendered from its typed reason on every read, so a language switch
    /// never has to guess a translation key back out of the previous string;
    /// a raw tool, network or error cause is returned exactly as it arrived.
    QString statusReason() const { return renderStatusReason(); }
    /// The generation every new off-GUI run carries. A delivery whose
    /// generation is no longer current is discarded rather than rendered, so an
    /// off-GUI publisher reads it on the GUI thread before it applies.
    quint64 runGeneration() const { return runGeneration_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void buildUi();
    void retranslateUi();
    void renderCards();
    void updateControls();
    mplayer::native::Backend selectedBackend() const;
    /// One app-owned status reason: an explicit kind plus the raw values that
    /// kind needs. The wording is looked up in the live translation when the
    /// line is read, so a language switch restates the same state, and no
    /// per-status formatter is retained.
    struct StatusReason {
        enum class Kind {
            None,
            /// Opaque text supplied by a caller; returned verbatim.
            Raw,
            InventoryFailed,
            VerifyNeedsRunner,
            VerifiedCount,
            VerifyDigest,
            NotVerified,
            CheckNeedsRunner,
            NothingToInstall,
            CheckFailed,
            ClosureNeedsRunner,
            RuntimeNeedsRunner,
            ImportFailed,
            RuntimeInstalled,
            VerificationCancelled,
            DownloadsCancelling,
            InstallFailed,
            MissingProvisioner,
            AllVerified,
        };

        Kind kind = Kind::None;
        /// A raw cause or imported refusal reason, preserved verbatim.
        QString detail{};
        /// Raw per-element cause lines for the digest form.
        QStringList causes{};
        int count = 0;
        mplayer::native::Backend backend = mplayer::native::Backend::TensorRT;
        int rifeFailures = 0;
        int spatialFailures = 0;
    };

    /// Raw, opaque text (a tool/network cause or a joined failure list) that is
    /// preserved verbatim, never retranslated.
    void applyStatus(const QString& key, const QString& reason);
    /// App-owned text: rendered from `reason` in whatever language is live when
    /// the line is read, so LanguageChange can restate it in place.
    void applyStatus(const QString& key, const StatusReason& reason);
    /// Renders the current typed reason in the live language.
    QString renderStatusReason() const;
    /// Re-derives the live status line and republishes the existing status
    /// notification so the window-level hint restates it. Touches no run state.
    void restateStatus();
    /// GUI-thread only: renders one off-GUI acquisition progress event.
    void applyProvisionProgress(const mplayer::native::ComponentProvisionProgress& progress);
    /// GUI-thread only: renders one off-GUI graph-archive download event.
    void applyDownloadProgress(const mplayer::native::ComponentProvisionProgress& progress);
    QString progressCardForPack(const QString& pack) const;
    QString progressDisplayName(const QString& pack) const;
    void applyProgressEvent(const mplayer::native::ComponentProvisionProgress& progress);
    void setProgressItem(const QString& identity, const QString& cardId,
                         const QString& displayName,
                         mplayer::native::ComponentProvisionStage stage,
                         qint64 received, qint64 total, bool queued = false);
    void settleProgressItem(const QString& identity, bool installed, bool cancelled,
                            const QString& detail = {});
    void clearProgressItem(const QString& identity);
    void settleProgressCard(const QString& cardId, bool installed, bool cancelled,
                            const QString& detail = {});
    void clearProgressCard(const QString& cardId);
    void renderCardProgress(const QString& cardId);
    void renderAllCardProgress();
    int activeProgressItemCount() const;
    /// Publishes one completed off-GUI verification. `verifiedPacks`,
    /// `verifiedRifeGraphs` and `verifiedBackend` are exactly what the hashing
    /// proved; a stale run never reaches this.
    void applyVerifyResult(int verified, int failed, int failedRife, int failedSpatial,
                           const QStringList& failures, const QSet<QString>& verifiedPacks,
                           const QSet<QString>& verifiedRifeGraphs,
                           const std::optional<int>& verifiedBackend);
    void downloadAnimeJanaiRuntime();
    void chooseAnimeJanaiRuntimeDirectory();
    void importAnimeJanaiRuntime(const QString& sourceDirectory);
    void downloadRemainingComponents();
    void downloadMissing(bool showReview);
    /// One independent acquisition unit finished: a backend closure, the RIFE
    /// archive, or the spatial graphs. Units run concurrently on the transfer
    /// pool; the last one to settle finalizes the run. `failure` empty and
    /// `cancelled` false means the unit installed what it was asked for.
    void finishAcquisitionUnit(const QString& failure, bool cancelled);
    /// The same, naming the stable failure key the joined status reports under.
    void finishAcquisitionUnit(const QString& failure, bool cancelled,
                               const QString& failureKey);
    /// Marks one stage as part of the current run so its own terminal callback
    /// settles a unit instead of a status line.
    void noteAcquisitionUnitStarted();
    /// Runs once every unit has settled: reports the joined failures, or
    /// re-verifies the installed elements before claiming success.
    void finalizeAcquisition();
    void startAcquisitionVerification();
    void downloadRifeArchive();
    void downloadNextModel();
    void scheduleInventoryRefresh();
    void applyInventorySnapshot(const QVariant& result);
    void finishRecommendedInstall();
    void finishModelDownloads();
    void updateCardStates();

    /// Everything the off-GUI Verify needs, copied on the GUI thread before the
    /// worker starts. No value here reaches back into a widget, and the copy is
    /// immutable while the worker hashes, so a late result cannot be mistaken
    /// for a fresh one.
    struct VerifyRequest {
        mplayer::native::ComponentStore store;
        mplayer::native::RifeArchiveStore rifeStore;
        QVector<const mplayer::native::ComponentPack*> packs;
        QVector<mplayer::native::RifeModelPin> rifePins;
        QVector<mplayer::native::StoredSpatialAsset> storedSpatial;
        QVector<mplayer::native::SpatialAsset> spatialCatalog;
        mplayer::native::Backend backend = mplayer::native::Backend::TensorRT;
    };
    VerifyRequest captureVerifyRequest() const;
    void startVerify(const VerifyRequest& request);

    /// Tells the window-level readout whether a download is running. Idle is
    /// emitted once, on the way out, and only after a download was announced.
    void publishAcquisition();

    Runner* runner_ = nullptr;
    backend::MikaiClient* client_ = nullptr;
    /// Borrowed, never owned. The provisioner is a QObject the window owns for
    /// the process, so its teardown can reach this tab before this tab reaches
    /// its destructor: a weak pointer turns that order into a null service
    /// instead of a cancel on freed state.
    QPointer<NativeComponentProvisioner> provisioner_;
    /// A standalone provisioner run starts with its first event; each affected
    /// old pack row is retired once while unrelated card rows remain intact.
    bool provisionerProgressSessionActive_ = false;
    QSet<QString> provisionerProgressItemsSeen_;
    /// Drops already-queued events from a provisioner replaced by setComponentProvisioner.
    quint64 provisionerBindingGeneration_ = 0;

    QLabel* heading_ = nullptr;
    QLabel* hint_ = nullptr;
    QLabel* gpuLabel_ = nullptr;
    QLabel* recommendationBanner_ = nullptr;
    QCheckBox* runtimeCheck_ = nullptr;
    QCheckBox* rifeCheck_ = nullptr;
    QCheckBox* spatialCheck_ = nullptr;
    QCheckBox* kernelsCheck_ = nullptr;
    QLabel* runtimeState_ = nullptr;
    QLabel* rifeState_ = nullptr;
    QLabel* spatialState_ = nullptr;
    QLabel* kernelsState_ = nullptr;
    QPushButton* verifyButton_ = nullptr;
    QPushButton* downloadButton_ = nullptr;
    QPushButton* importAnimeJanaiButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QLabel* status_ = nullptr;
    struct ProgressItem {
        enum class Outcome { Active, Installed, Failed, Cancelled };

        QString cardId;
        QString displayName;
        QString detail;
        mplayer::native::ComponentProvisionStage stage =
            mplayer::native::ComponentProvisionStage::Downloading;
        qint64 received = 0;
        qint64 total = 0;
        bool queued = false;
        Outcome outcome = Outcome::Active;
    };
    QMap<QString, QProgressBar*> cardProgressBars_;
    QMap<QString, QLabel*> cardProgressStatusLabels_;
    QMap<QString, ProgressItem> progressItems_;
    QSet<QString> currentProgressItems_;

    QString statusKey_;
    /// The current status line's typed source. Holds only the raw values its
    /// kind needs and is rendered on every read; the last rendered string is
    /// remembered only to tell a status line still showing that reason from a
    /// transient progress line.
    StatusReason statusReason_;
    QString statusRenderedReason_;
    /// Pack names whose bytes this session actually re-hashed. A status read
    /// only trusts the recorded size, so "Verified" is claimed for a component
    /// only after `verifyPack` proved it, never from a directory listing.
    QSet<QString> verifiedPacks_;
    /// RIFE archive graphs are a separate inventory, not NativeSpatialAssets.
    QSet<QString> verifiedRifeGraphs_;
    QSet<int> verifiedAnimeJanaiBackends_;
    QMap<int, QString> animeJanaiVerificationErrors_;
    QMap<QString, QString> rifeVerificationErrors_;
    /// Which catalogue rows the current download run still has to fetch, as
    /// indices into spatialAssetCatalog().
    QVector<int> pendingModels_;
    /// User-selected acquisition groups; graph IDs belong only to the catalogue.
    QSet<QString> selectedPacks_;
    std::shared_ptr<std::atomic<bool>> rifeCancelFlag_;
    std::shared_ptr<std::atomic<bool>> modelCancelFlag_;
    QString rifeFailure_;
    QString modelFailure_;
    QVector<mplayer::native::StoredSpatialAsset> storedSpatial_;
    bool rifeInstalled_ = false;
    bool rifeVerified_ = false;
    bool animeJanaiTensorRtInstalled_ = false;
    bool componentDirsAvailable_ = false;
    bool inventoryReady_ = false;
    bool inventoryScanning_ = false;
    bool acquisitionChecking_ = false;
    bool acquisitionSequenceActive_ = false;
    bool animeJanaiDirectMlInstalled_ = false;
    bool runtimeRecommended_ = false;
    bool kernelsRecommended_ = false;
    bool spatialRecommended_ = false;
    qint64 tensorRtClosureBytes_ = 0;
    qint64 directMlClosureBytes_ = 0;
    bool pendingRifeArchive_ = false;
    /// The selected backend closure still has to be installed by this run, and
    /// whether installing the AnimeJaNai runtime is what provides it. Copied
    /// from the off-GUI check so the concurrent launcher can start the backend
    /// unit without re-reading the inventory.
    bool pendingBackendWork_ = false;
    bool pendingRuntimeClosure_ = false;
    bool componentStagePending_ = false;
    bool recommendedInstallPending_ = false;
    bool modelCancelled_ = false;
    /// Independent units the current run started and has not settled yet. The
    /// backend closure, the RIFE archive and the spatial graphs run at the same
    /// time, so the run is over when this reaches zero rather than when a fixed
    /// chain of stages ends.
    int acquisitionUnitsPending_ = 0;
    /// The generation every unit of the current acquisition run shares. Units
    /// start concurrently, so a per-unit bump would invalidate the callbacks of
    /// the units that started first; one token is captured when the run begins
    /// and only an external invalidation (a backend change, a cancel, a new
    /// runner) moves `runGeneration_` past it.
    quint64 acquisitionGeneration_ = 0;
    /// True when any unit of the current run was cancelled; the run then ends
    /// with the cancelled status instead of a verification.
    bool acquisitionCancelled_ = false;
    /// The failures the settled units reported, joined into the terminal status
    /// when the last unit finishes.
    QStringList acquisitionFailures_;
    /// The stable failure key of the first failed unit, used for the severity.
    QString acquisitionFailureKey_;
    std::shared_ptr<std::atomic<bool>> acquisitionCheckCancelFlag_;
    std::shared_ptr<std::atomic<bool>> inventoryCancelFlag_;
    bool rifeDownloading_ = false;
    bool modelsDownloading_ = false;
    int modelDownloadsInFlight_ = 0;
    int modelDownloadsTotal_ = 0;
    int modelDownloadsCompleted_ = 0;
    bool animeJanaiRuntimeDownloading_ = false;
    bool componentSelectionsInitialized_ = false;
    std::shared_ptr<std::atomic<bool>> animeJanaiCancelFlag_;
    QComboBox* animeJanaiBackendCombo_ = nullptr;
    QLabel* animeJanaiBackendLabel_ = nullptr;
    /// The backend the combo is actually on. A reparent or a show can emit
    /// currentIndexChanged for the same row; that must not retire a download.
    int trackedBackend_ = 0;
    /// True after this tab has told the window a download is running, until
    /// that download leaves the running state. Progress ticks stay quiet.
    bool acquisitionAnnounced_ = false;
    /// Liveness of this tab for every off-GUI run. Cleared by the destructor, so
    /// a completion that was already queued reads a dead owner and never
    /// dereferences one. A plain atomic copy, never a widget QPointer.
    std::shared_ptr<std::atomic<bool>> alive_;
    /// Set while any off-GUI run may still publish. While true no second run may
    /// start and no result may be rendered: the controls stay disabled and each
    /// active card reports indeterminate activity.
    bool verifying_ = false;
    /// Bumped by every new run and by every backend/client/provisioner change.
    /// A delivery carrying an older generation is discarded rather than rendered.
    quint64 runGeneration_ = 0;
    quint64 inventoryGeneration_ = 0;
};

}  // namespace mikai::app
