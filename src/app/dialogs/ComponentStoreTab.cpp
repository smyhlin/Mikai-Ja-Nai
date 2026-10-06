#include "ComponentStoreTab.hpp"

#include "state/NativeComponentProvisioner.hpp"
#include "workers/Runner.hpp"
#include "theme/Metrics.hpp"

#include <backend/MikaiClient.hpp>

#include <native/bundle/NativeBundle.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>
#include <native/components/NativeComponentCatalog.hpp>
#include <native/components/NativeComponentStore.hpp>
#include <native/components/NativeComponentToolchain.hpp>
#include <native/components/NativeGpuProbe.hpp>
#include <native/models/NativeSpatialAssets.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QEvent>
#include <QtCore/QLocale>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <stdexcept>

namespace mikai::app {

using namespace mplayer::native;

namespace {
/// The component decision for the first installed Native bundle this build can
/// find (the loaded selection when it is Native, otherwise discovery). Lives
/// here rather than beside `componentBinDirs` on purpose: discovery reaches
/// `NativeRuntimeSelection`, which reaches the mpv loader, and the domain module
/// must stay linkable without the player - the probe that links it alone has no
/// `MpvApi` to give. Empty directories and no reasonKey when no such bundle
/// exists; creates nothing, so it is safe on every repaint.
ComponentBinDirs installedComponentBinDirs(const GpuComputeCapability& gpu,
                                           const ComponentStore& store) {
    QString bundleRoot;
    std::optional<Bundle> bundle;
    const RuntimeSelection& selection = currentSelection();
    if (selection.loaded && selection.kind == BundleKind::Native && !selection.root.isEmpty()) {
        bundleRoot = selection.root;
        bundle = selection.resolvedBundle;
    }
    if (bundleRoot.isEmpty()) {
        for (const Bundle& candidate : discoverBundles()) {
            if (candidate.kind == BundleKind::Native) {
                bundleRoot = candidate.root;
                bundle = candidate;
                break;
            }
        }
    }
    if (!bundle.has_value() || !bundle->preparation.has_value()) {
        return ComponentBinDirs{};
    }

    const BundlePreparation& preparation = *bundle->preparation;
    QStringList declaredTrtFiles;
    const QString prefix = preparation.runtime.trt + QLatin1Char('/');
    for (const BundleFile& file : bundle->files) {
        if (file.path.startsWith(prefix)) {
            declaredTrtFiles.append(file.path);
        }
    }
    return componentBinDirs(bundleRoot, preparation.runtime.trt, preparation.runtime.cuda,
                            declaredTrtFiles, gpu, store);
}

/// A model graph is ~2 MB but the link may be slow; 20 s was an artwork budget.
constexpr int kModelDownloadTimeoutMs = 120000;
}  // namespace

ComponentStoreTab::ComponentStoreTab(QWidget* parent) : QWidget(parent) {
    buildUi();
    refresh();
}

void ComponentStoreTab::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(theme::kSpacingLarge, theme::kSpacingLarge, theme::kSpacingLarge,
                             theme::kSpacingLarge);
    root->setSpacing(theme::kSpacingMedium);

    heading_ = new QLabel(this);
    heading_->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(heading_);

    hint_ = new QLabel(this);
    hint_->setObjectName(QStringLiteral("muted"));
    hint_->setWordWrap(true);
    root->addWidget(hint_);

    gpuLabel_ = new QLabel(this);
    gpuLabel_->setObjectName(QStringLiteral("descriptionPanel"));
    gpuLabel_->setWordWrap(true);
    root->addWidget(gpuLabel_);

    table_ = new QTableWidget(this);
    table_->setObjectName(QStringLiteral("nativeComponentTable"));
    table_->setColumnCount(4);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    root->addWidget(table_, 1);

    auto* row = new QHBoxLayout();
    row->setSpacing(theme::kSpacingSmall);
    verifyButton_ = new QPushButton(this);
    verifyButton_->setObjectName(QStringLiteral("nativeComponentVerifyButton"));
    row->addWidget(verifyButton_);
    downloadButton_ = new QPushButton(this);
    downloadButton_->setObjectName(QStringLiteral("primary"));
    row->addWidget(downloadButton_);
    cancelButton_ = new QPushButton(this);
    cancelButton_->setObjectName(QStringLiteral("nativeComponentTabCancelButton"));
    cancelButton_->setEnabled(false);
    row->addWidget(cancelButton_);
    row->addStretch();
    root->addLayout(row);

    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("nativeComponentTabProgress"));
    progress_->setRange(0, 100);
    progress_->setTextVisible(false);
    progress_->hide();
    root->addWidget(progress_);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("muted"));
    status_->setWordWrap(true);
    root->addWidget(status_);

    connect(verifyButton_, &QPushButton::clicked, this, &ComponentStoreTab::verifyAll);
    connect(downloadButton_, &QPushButton::clicked, this, &ComponentStoreTab::downloadMissing);
    connect(cancelButton_, &QPushButton::clicked, this, [this] {
        if (provisioner_ != nullptr) provisioner_->cancel();
        modelsDownloading_ = false;
        pendingModels_.clear();
        if (progress_ != nullptr) progress_->hide();
        applyStatus(QStringLiteral("native.components.cancelled"), QString());
        updateControls();
    });

    retranslateUi();
}

void ComponentStoreTab::retranslateUi() {
    heading_->setText(tr("Backend components"));
    hint_->setText(
        tr("The GPU-specific TensorRT builder resource, its runtime, and the pinned model graphs "
           "are downloaded on this machine and verified by SHA-256 before they are stored. They "
           "are deliberately not part of a portable build or an installer."));
    table_->setAccessibleName(tr("Backend component and model store"));
    table_->setHorizontalHeaderLabels({tr("Element"), tr("Kind"), tr("Size"), tr("Status")});
    verifyButton_->setText(tr("Verify"));
    verifyButton_->setAccessibleName(tr("Re-check every installed element against its digest"));
    downloadButton_->setText(tr("Download missing"));
    downloadButton_->setAccessibleName(tr("Download the elements this machine is missing"));
    cancelButton_->setText(tr("Cancel"));
    cancelButton_->setAccessibleName(tr("Cancel the running download"));
}

void ComponentStoreTab::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
        renderTable();
    }
}

void ComponentStoreTab::setRunner(Runner* runner) {
    runner_ = runner;
    updateControls();
}

void ComponentStoreTab::setClient(backend::MikaiClient* client) {
    client_ = client;
    updateControls();
}

void ComponentStoreTab::setComponentProvisioner(NativeComponentProvisioner* provisioner) {
    if (provisioner_ == provisioner) {
        updateControls();
        return;
    }
    if (provisioner_ != nullptr) disconnect(provisioner_, nullptr, this, nullptr);
    provisioner_ = provisioner;
    if (provisioner_ != nullptr) {
        connect(provisioner_, &NativeComponentProvisioner::progress, this,
                [this](const QString& pack, qint64 received, qint64 total) {
                    progress_->setRange(0, 100);
                    progress_->setValue(total > 0 ? static_cast<int>(received * 100 / total) : 0);
                    status_->setText(tr("Downloading %1: %2 of %3")
                                         .arg(pack, QLocale().formattedDataSize(received),
                                              QLocale().formattedDataSize(total)));
                });
        connect(provisioner_, &NativeComponentProvisioner::finished, this,
                [this](bool ok, bool cancelled, const QString& summary) {
                    if (progress_ != nullptr) progress_->hide();
                    if (cancelled) {
                        applyStatus(QStringLiteral("native.components.cancelled"), summary);
                    } else if (!ok) {
                        applyStatus(QStringLiteral("native.components.fetch"), summary);
                    } else {
                        applyStatus(QString(), summary);
                        downloadNextModel();
                    }
                    refresh();
                });
    }
    updateControls();
}

void ComponentStoreTab::refresh() {
    if (gpuLabel_ != nullptr) {
        gpuLabel_->setText(tr("GPU: %1").arg(computeCapabilityText(observeComputeCapability())));
    }
    renderTable();
    updateControls();
}

QVector<ComponentStoreTab::ElementRow> ComponentStoreTab::componentRows() const {
    const GpuComputeCapability& gpu = observeComputeCapability();
    const ComponentStore store = componentStore();
    const ComponentBinDirs available = installedComponentBinDirs(gpu, store);
    const bool bundleProvides =
        gpu.smMajor > 0 && available.reasonKey.isEmpty() && !available.fromStore;

    QVector<ElementRow> rows;
    for (const ComponentPack* pack : compatiblePacksFor(gpu.smMajor, gpu.smMinor)) {
        ElementRow row;
        row.name = pack->name;
        row.kind = tr("TensorRT component");
        row.bytes = pack->assetBytes;
        row.needed = gpu.smMajor > 0;
        const StoredPack stored = storedPack(store, *pack);
        row.installed = stored.installed;
        row.verified = stored.installed && verifiedPacks_.contains(pack->name);
        row.detail = stored.installed
                         ? (row.verified ? tr("re-hashed against the record")
                                         : tr("in the component store; not re-checked yet"))
                         : stored.reason;
        if (bundleProvides) {
            row.fromBundle = true;
            row.installed = true;
            row.detail = tr("from the installed bundle");
        }
        rows.append(row);
    }
    return rows;
}

QVector<ComponentStoreTab::ElementRow> ComponentStoreTab::modelRows() const {
    const QVector<StoredSpatialAsset> stored = storedSpatialAssets();
    QVector<ElementRow> rows;
    for (const SpatialAsset& asset : spatialAssetCatalog()) {
        ElementRow row;
        row.name = asset.name;
        row.kind = tr("Model graph");
        row.bytes = asset.bytes;
        row.needed = true;
        for (const StoredSpatialAsset& candidate : stored) {
            if (candidate.asset.id != asset.id) continue;
            row.installed = true;
            row.verified = candidate.verified;
            row.detail = candidate.verified ? tr("verified on disk") : candidate.reason;
            break;
        }
        if (!row.installed) row.detail = tr("not downloaded");
        rows.append(row);
    }
    return rows;
}

void ComponentStoreTab::renderTable() {
    if (table_ == nullptr) return;
    const QVector<ElementRow> rows = componentRows() + modelRows();
    table_->setRowCount(rows.size());
    for (int index = 0; index < rows.size(); ++index) {
        const ElementRow& row = rows.at(index);
        QString status;
        if (row.verified) {
            status = tr("Verified");
        } else if (row.fromBundle) {
            status = tr("Available from the bundle");
        } else if (row.installed) {
            status = tr("Present (not re-checked)");
        } else {
            status = tr("Not installed");
        }
        if (row.needed && !row.installed && !row.fromBundle) status += tr(" - needed here");

        table_->setItem(index, 0, new QTableWidgetItem(row.name));
        table_->setItem(index, 1, new QTableWidgetItem(row.kind));
        table_->setItem(index, 2,
                        new QTableWidgetItem(QLocale().formattedDataSize(row.bytes)));
        auto* statusItem = new QTableWidgetItem(status);
        statusItem->setToolTip(row.detail);
        table_->setItem(index, 3, statusItem);
    }
    table_->resizeColumnsToContents();
}

void ComponentStoreTab::verifyAll() {
    const ComponentStore store = componentStore();
    int verified = 0;
    int failed = 0;
    QStringList failures;
    const GpuComputeCapability& gpu = observeComputeCapability();
    for (const ComponentPack* pack : compatiblePacksFor(gpu.smMajor, gpu.smMinor)) {
        const StoredPack result = verifyPack(store, *pack);
        if (result.installed) {
            verifiedPacks_.insert(pack->name);
            ++verified;
            continue;
        }
        verifiedPacks_.remove(pack->name);
        if (storedPack(store, *pack).installed) {
            ++failed;
            failures.append(QStringLiteral("%1: %2").arg(pack->name, result.reason));
        }
    }
    const QVector<StoredSpatialAsset> models = storedSpatialAssets();
    for (const StoredSpatialAsset& model : models) {
        if (model.verified) {
            ++verified;
        } else {
            ++failed;
            failures.append(model.reason);
        }
    }

    renderTable();
    if (failed == 0) {
        applyStatus(QString(), tr("%1 elements verified, none changed.").arg(verified));
    } else {
        applyStatus(QStringLiteral("native.components.digest"),
                    tr("%1 elements failed verification: %2")
                        .arg(failed)
                        .arg(failures.join(QStringLiteral("; "))));
    }
    updateControls();
}

void ComponentStoreTab::downloadMissing() {
    const ComponentBinDirs verdict =
        installedComponentBinDirs(observeComputeCapability(), componentStore());
    const bool componentMissing =
        observeComputeCapability().smMajor > 0 && !verdict.reasonKey.isEmpty();

    pendingModels_.clear();
    const QVector<StoredSpatialAsset> stored = storedSpatialAssets();
    const QVector<SpatialAsset> catalog = spatialAssetCatalog();
    for (int index = 0; index < catalog.size(); ++index) {
        bool present = false;
        for (const StoredSpatialAsset& candidate : stored) {
            if (candidate.asset.id == catalog.at(index).id && candidate.verified) present = true;
        }
        if (!present) pendingModels_.append(index);
    }

    if (componentMissing && provisioner_ != nullptr) {
        QString reason;
        if (!provisioner_->ensureStore(&reason)) {
            applyStatus(QStringLiteral("native.components.missing"), reason);
            downloadNextModel();
            return;
        }
        progress_->show();
        progress_->setValue(0);
        updateControls();
        return;
    }
    downloadNextModel();
}

void ComponentStoreTab::downloadNextModel() {
    if (pendingModels_.isEmpty()) {
        modelsDownloading_ = false;
        if (progress_ != nullptr) progress_->hide();
        applyStatus(QString(), tr("Every element this machine needs is present."));
        refresh();
        return;
    }
    if (runner_ == nullptr || client_ == nullptr) {
        applyStatus(QStringLiteral("native.components.noClient"),
                    tr("A model download needs the backend client and a runner."));
        pendingModels_.clear();
        updateControls();
        return;
    }
    const SpatialAsset asset = spatialAssetCatalog().at(pendingModels_.takeFirst());
    modelsDownloading_ = true;
    progress_->show();
    progress_->setRange(0, 0);
    status_->setText(tr("Downloading %1...").arg(asset.name));
    updateControls();

    backend::MikaiClient* client = client_;
    QPointer<ComponentStoreTab> self(this);
    runner_->run(
        [client, asset]() -> QVariant {
            const QByteArray bytes = client->fetchBytes(asset.url, kModelDownloadTimeoutMs);
            const SpatialAssetPublication published =
                publishSpatialAssetBytes(asset, bytes, QString());
            if (!published.ok) {
                throw std::runtime_error(
                    QStringLiteral("%1: %2").arg(published.reasonKey, published.reason).toStdString());
            }
            return QVariant{};
        },
        [self](const QVariant&) {
            if (!self) return;
            self->downloadNextModel();
        },
        [self](const QString& error) {
            if (!self) return;
            self->modelsDownloading_ = false;
            self->pendingModels_.clear();
            self->applyStatus(QStringLiteral("native.components.fetch"), error);
            self->updateControls();
        });
}

void ComponentStoreTab::updateControls() {
    if (downloadButton_ == nullptr) return;
    const bool provisioning = provisioner_ != nullptr && provisioner_->running();
    const bool busy = provisioning || modelsDownloading_;
    downloadButton_->setEnabled(!busy);
    verifyButton_->setEnabled(!busy);
    cancelButton_->setEnabled(busy);
    if (!busy && progress_ != nullptr) progress_->hide();
}

void ComponentStoreTab::applyStatus(const QString& key, const QString& reason) {
    statusKey_ = key;
    statusReason_ = reason;
    if (status_ != nullptr) status_->setText(reason);
    if (!key.isEmpty()) {
        qWarning().noquote() << "native components:" << key << reason;
    }
}

}  // namespace mikai::app
