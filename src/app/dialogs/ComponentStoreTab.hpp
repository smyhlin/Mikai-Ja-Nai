#pragma once

#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

class QEvent;
class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace mikai::app {

class Runner;
class NativeComponentProvisioner;

}  // namespace mikai::app

namespace mikai::backend {
class MikaiClient;
}

namespace mikai::app {

/// The Components tab: every downloadable backend element this build knows -
/// the pinned TensorRT component packs and the pinned model graphs - with its
/// install state, a manual Verify that re-hashes the bytes, and a Download
/// missing that fills in whatever this machine needs.
///
/// Nothing listed here ships in a portable build or an installer. Every element
/// is either GPU-specific (a builder resource for one compute capability) or
/// carries third-party terms this project does not redistribute, so the bytes
/// are fetched on the machine that will use them and proved by digest before
/// they are stored. The panel only ever reads the two app-owned stores and asks
/// the provisioner and the backend client to download; it writes nothing itself.
class ComponentStoreTab : public QWidget {
    Q_OBJECT

public:
    explicit ComponentStoreTab(QWidget* parent = nullptr);

    /// The off-GUI executor model downloads run on. Without one, downloading a
    /// model is refused by name; the TensorRT packs go through the provisioner
    /// and need no runner here.
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

    /// The manual check: re-hashes every installed component file against the
    /// record and re-proves every stored graph against its catalogue digest.
    void verifyAll();

    /// Downloads whatever this machine is missing: the TensorRT runtime and
    /// builder resource for the observed GPU when absent, then every pinned
    /// model graph not yet stored.
    void downloadMissing();

    QString statusKey() const { return statusKey_; }
    QString statusReason() const { return statusReason_; }

protected:
    void changeEvent(QEvent* event) override;

private:
    struct ElementRow {
        QString name;
        QString kind;
        qint64 bytes = 0;
        bool installed = false;
        bool verified = false;
        bool needed = false;
        /// Satisfied by the installed bundle instead of the app's own store.
        bool fromBundle = false;
        QString detail;
    };

    void buildUi();
    void retranslateUi();
    void renderTable();
    void updateControls();
    void applyStatus(const QString& key, const QString& reason);
    void downloadNextModel();
    QVector<ElementRow> componentRows() const;
    QVector<ElementRow> modelRows() const;

    Runner* runner_ = nullptr;
    backend::MikaiClient* client_ = nullptr;
    NativeComponentProvisioner* provisioner_ = nullptr;

    QLabel* heading_ = nullptr;
    QLabel* hint_ = nullptr;
    QLabel* gpuLabel_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* verifyButton_ = nullptr;
    QPushButton* downloadButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;

    QString statusKey_;
    QString statusReason_;
    /// Pack names whose bytes this session actually re-hashed. A status read
    /// only trusts the recorded size, so "Verified" is claimed for a component
    /// only after `verifyPack` proved it, never from a directory listing.
    QSet<QString> verifiedPacks_;
    /// Which catalogue rows the current download run still has to fetch, as
    /// indices into spatialAssetCatalog().
    QVector<int> pendingModels_;
    bool modelsDownloading_ = false;
};

}  // namespace mikai::app
