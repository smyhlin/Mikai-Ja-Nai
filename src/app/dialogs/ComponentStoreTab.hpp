#pragma once

#include <native/components/AnimeJanaiRuntime.hpp>

#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

#include <atomic>
#include <memory>

class QComboBox;
class QEvent;
class QLabel;
class QProgressBar;
class QPushButton;
class QResizeEvent;
class QCheckBox;

namespace mikai::app {

QString animeJanaiSetupPrompt(qint64* bytes = nullptr);

class Runner;
class NativeComponentProvisioner;

}  // namespace mikai::app

namespace mikai::backend {
class MikaiClient;
}

namespace mikai::app {

/// The Components tab: downloadable Aji runtime/backend closures, pinned
/// GPU-matched TensorRT packs and RIFE graphs presented as three selectable
/// cards, with manual Verify and Apply changes actions.
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

    /// Re-reads both stores and re-renders. Creates neither root and downloads
    /// nothing.
    void refresh();

    /// The manual check: re-hashes both AnimeJaNai closures and every installed
    /// component file, and re-proves every stored graph against its digest.
    void verifyAll();

    /// Reviews and installs the selected runtime/backend closure, RIFE archive,
    /// and any selected model downloads.
    void downloadMissing();
    /// Selects the GPU-matched backend and applies the checked cards without a
    /// second confirmation.
    void installRecommended();

    QString statusKey() const { return statusKey_; }
    QString statusReason() const { return statusReason_; }

protected:
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void buildUi();
    void retranslateUi();
    void renderCards();
    void updateControls();
    mplayer::native::Backend selectedBackend() const;
    void applyStatus(const QString& key, const QString& reason);
    void downloadAnimeJanaiRuntime();
    void chooseAnimeJanaiRuntimeDirectory();
    void importAnimeJanaiRuntime(const QString& sourceDirectory);
    void downloadRemainingComponents();
    void downloadMissing(bool showReview);
    void downloadRifeArchive();
    void downloadNextModel();
    void updateCardStates();

    Runner* runner_ = nullptr;
    backend::MikaiClient* client_ = nullptr;
    NativeComponentProvisioner* provisioner_ = nullptr;

    QLabel* heading_ = nullptr;
    QLabel* hint_ = nullptr;
    QLabel* gpuLabel_ = nullptr;
    QLabel* recommendationBanner_ = nullptr;
    QCheckBox* runtimeCheck_ = nullptr;
    QCheckBox* rifeCheck_ = nullptr;
    QCheckBox* kernelsCheck_ = nullptr;
    QLabel* runtimeState_ = nullptr;
    QLabel* rifeState_ = nullptr;
    QLabel* kernelsState_ = nullptr;
    QPushButton* verifyButton_ = nullptr;
    QPushButton* downloadButton_ = nullptr;
    QPushButton* importAnimeJanaiButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;

    QString statusKey_;
    QString statusReason_;
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
    QSet<QString> selectedModelIds_;
    std::shared_ptr<std::atomic<bool>> rifeCancelFlag_;
    std::shared_ptr<std::atomic<bool>> modelCancelFlag_;
    QString rifeFailure_;
    bool rifeDownloading_ = false;
    bool modelsDownloading_ = false;
    bool animeJanaiRuntimeDownloading_ = false;
    bool componentSelectionsInitialized_ = false;
    std::shared_ptr<std::atomic<bool>> animeJanaiCancelFlag_;
    QComboBox* animeJanaiBackendCombo_ = nullptr;
    QLabel* animeJanaiBackendLabel_ = nullptr;
};

}  // namespace mikai::app
