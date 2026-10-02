// M35's assets panel. See NativeAssetsPanel.hpp for the rules; this file is their
// one implementation.
//
// Everything the panel shows is read from the app-owned store and the
// artifact cache through their own APIs. Nothing here scans a foreign folder,
// invents availability, runs an inspection on the GUI thread, or deletes a file
// that did not first prove itself.

#include "NativeAssetsPanel.hpp"

#include <atomic>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QPointer>
#include <QtCore/QVariant>
#include <QtGui/QFontMetrics>
#include <QtGui/QIntValidator>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <backend/Errors.hpp>
#include <backend/MikaiClient.hpp>

#include "NativePaths.hpp"
#include "NativeSpatialAssets.hpp"
#include "NativeUserRecipes.hpp"
#include "workers/Runner.hpp"
#include "theme/Metrics.hpp"

namespace mplayer {
namespace native {

namespace {

constexpr const char* kArtifactDirectoryPrefix = "artifact-";

QString humanBytes(qint64 bytes) {
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QString backendList(const QVector<Backend>& backends) {
    if (backends.isEmpty()) return QStringLiteral("—");
    QStringList names;
    names.reserve(backends.size());
    for (const Backend backend : backends) {
        names.append(backend == Backend::TensorRT ? QStringLiteral("TensorRT")
                                                  : QStringLiteral("DirectML"));
    }
    return names.join(QStringLiteral(", "));
}

QString statusCell(const NativeModelView& view) {
    if (view.usable) return QCoreApplication::translate("NativeAssetsPanel", "Ready");
    return QCoreApplication::translate("NativeAssetsPanel", "Unavailable");
}

}  // namespace

NativeModelView describeStoredModel(const StoredModel& model) {
    NativeModelView view;
    view.id = model.record.id;
    view.label = model.record.displayName.isEmpty() ? model.record.id : model.record.displayName;
    view.contentId = model.record.contentId();
    view.role = model.record.role == ModelRole::Spatial
                    ? QCoreApplication::translate("NativeAssetsPanel", "Spatial")
                    : QCoreApplication::translate("NativeAssetsPanel", "Temporal");
    view.dtype = model.record.actualProfile.input.dtype;
    view.bytes = model.record.onnxBytes;
    view.backends = model.record.availableBackends;
    view.terms = model.record.termsNote;
    view.usable = model.record.availability.isUsable();
    if (!view.usable) {
        view.statusKey = model.record.availability.reasonKey;
        view.statusReason = model.record.availability.reason;
    }
    return view;
}

QVector<NativeCacheEntry> enumerateNativeCache(const ArtifactStore& store) {
    QVector<NativeCacheEntry> entries;
    // An unowned root is never walked: nothing beside a user directory is ours.
    if (!store.isOwned(nullptr)) return entries;

    const QString prefix = QString::fromLatin1(kArtifactDirectoryPrefix);
    const QFileInfoList directories = QDir(store.root()).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& directory : directories) {
        const QString name = directory.fileName();
        // A `.staging-*` directory and any foreign name are not cache entries;
        // they are never listed for deletion and never removed here.
        if (!name.startsWith(prefix)) continue;
        const QString token = name.mid(prefix.size());
        if (!isSha256Hex(token)) continue;

        NativeCacheEntry entry;
        entry.buildKey = QStringLiteral("sha256:") + token;
        entry.directory = directory.absoluteFilePath();

        QString reasonKey;
        QString reason;
        const std::optional<ArtifactRecord> record =
            readArtifact(store, entry.buildKey, &reasonKey, &reason);
        if (record.has_value()) {
            entry.proven = true;
            entry.enginePath = record->enginePath;
            entry.engineBytes = record->manifest.engine.bytes;
        } else {
            entry.reasonKey = reasonKey;
            entry.reason = reason;
        }
        entries.append(entry);
    }
    return entries;
}

bool deleteNativeCacheEntry(const ArtifactStore& store, const QVector<NativeCacheEntry>& entries,
                            const QString& buildKey, QString* reasonKey, QString* reason) {
    const auto refuse = [reasonKey, reason](const QString& key, const QString& text) {
        if (reasonKey != nullptr) *reasonKey = key;
        if (reason != nullptr) *reason = text;
        return false;
    };

    const NativeCacheEntry* found = nullptr;
    for (const NativeCacheEntry& entry : entries) {
        if (entry.buildKey == buildKey) {
            found = &entry;
            break;
        }
    }
    if (found == nullptr) {
        return refuse(QStringLiteral("native.cache.unknown"),
                      QCoreApplication::translate("NativeAssetsPanel",
                                                  "Build key %1 is not an entry of this cache.")
                          .arg(buildKey));
    }
    if (!found->proven) {
        return refuse(found->reasonKey.isEmpty() ? QStringLiteral("native.cache.unproven")
                                                 : found->reasonKey,
                      found->reason.isEmpty()
                          ? QCoreApplication::translate(
                                "NativeAssetsPanel",
                                "This cache entry does not verify, so it is not deleted.")
                          : found->reason);
    }
    return removeArtifact(store, buildKey, reasonKey, reason);
}

struct NativeAssetsPanel::ImportResult {
    bool ok = false;
    bool published = false;
    bool cancelled = false;
    QString reasonKey;
    QString reason;
    QString modelId;
};

struct NativeAssetsPanel::DownloadResult {
    bool ok = false;
    bool published = false;
    bool cancelled = false;
    QString reasonKey;
    QString reason;
    QString id;
    QString path;
};

NativeAssetsPanel::NativeAssetsPanel(QWidget* parent)
    : QWidget(parent), artifacts_(ArtifactStore()) {
    modelRoot_ = nativeSubdirectory(QStringLiteral("models"));
#ifdef MIKAI_WITH_MODEL_INSPECTION
    runtime_ = defaultInspectionRuntime();
#endif
    buildUi();
    refresh();
}

QSize NativeAssetsPanel::minimumSizeHint() const {
    // The layout's own minimum counts every child, including table headers and
    // unbroken strings that scroll or wrap instead of voting width. The panel
    // lives inside a page that scrolls as a whole, so its floor is the
    // narrowest usable content: the import row's buttons and fixed fields.
    // Reporting the layout sum would push the page past the smallest window in
    // any language with longer captions, which is exactly the panning the
    // minimum-size probe guards against.
    return {200, 200};
}

void NativeAssetsPanel::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(mikai::app::theme::kSpacingSmall);

    auto* inventoryHeading = new QLabel(tr("Owned models"), this);
    inventoryHeading->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(inventoryHeading);

    storeRootLabel_ = new QLabel(this);
    storeRootLabel_->setObjectName(QStringLiteral("muted"));
    storeRootLabel_->setWordWrap(true);
    auto captionPolicy = storeRootLabel_->sizePolicy();
    captionPolicy.setHorizontalPolicy(QSizePolicy::Ignored);
    storeRootLabel_->setSizePolicy(captionPolicy);
    root->addWidget(storeRootLabel_);

    modelTable_ = new QTableWidget(this);
    modelTable_->setObjectName(QStringLiteral("nativeModelTable"));
    modelTable_->setAccessibleName(tr("Owned model inventory"));
    modelTable_->setColumnCount(6);
    modelTable_->setHorizontalHeaderLabels(
        {tr("Model"), tr("Role"), tr("Dtype"), tr("Size"), tr("Backends"), tr("Status")});
    modelTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    modelTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    modelTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    modelTable_->verticalHeader()->setVisible(false);
    modelTable_->horizontalHeader()->setStretchLastSection(true);
    modelTable_->horizontalHeader()->setMinimumSectionSize(0);
    modelTable_->setMaximumHeight(mikai::app::theme::kNativeAssetsTableMaxHeight);
    modelTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    modelTable_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(modelTable_);

    modelStatus_ = new QLabel(this);
    modelStatus_->setObjectName(QStringLiteral("muted"));
    modelStatus_->setWordWrap(true);
    root->addWidget(modelStatus_);

    auto* importRow = new QHBoxLayout();
    importRow->setSpacing(mikai::app::theme::kSpacingSmall);
    importButton_ = new QPushButton(tr("Import model…"), this);
    importButton_->setObjectName(QStringLiteral("nativeImportButton"));
    importButton_->setAccessibleName(tr("Import a model"));
    importRow->addWidget(importButton_);

    auto* widthLabel = new QLabel(tr("Stage W"), this);
    importRow->addWidget(widthLabel);
    stageWidthEdit_ = new QLineEdit(this);
    stageWidthEdit_->setObjectName(QStringLiteral("nativeStageWidthEdit"));
    stageWidthEdit_->setValidator(new QIntValidator(1, 100000, stageWidthEdit_));
    stageWidthEdit_->setFixedWidth(mikai::app::theme::kNativeDimensionFieldWidth);
    importRow->addWidget(stageWidthEdit_);

    auto* heightLabel = new QLabel(tr("H"), this);
    importRow->addWidget(heightLabel);
    stageHeightEdit_ = new QLineEdit(this);
    stageHeightEdit_->setObjectName(QStringLiteral("nativeStageHeightEdit"));
    stageHeightEdit_->setValidator(new QIntValidator(1, 100000, stageHeightEdit_));
    stageHeightEdit_->setFixedWidth(mikai::app::theme::kNativeDimensionFieldWidth);
    importRow->addWidget(stageHeightEdit_);

    cancelImportButton_ = new QPushButton(tr("Cancel"), this);
    cancelImportButton_->setObjectName(QStringLiteral("nativeImportCancelButton"));
    cancelImportButton_->setEnabled(false);
    importRow->addWidget(cancelImportButton_);
    importRow->addStretch();
    root->addLayout(importRow);

    importStatus_ = new QLabel(this);
    importStatus_->setObjectName(QStringLiteral("descriptionPanel"));
    importStatus_->setWordWrap(true);
    importStatus_->setMinimumHeight(mikai::app::theme::kSecondaryControlSize);
    root->addWidget(importStatus_);

    selectRecipeButton_ = new QPushButton(
        QStringList{
            tr("Use the selected model"),
            tr("as the requested recipe"),
        }.join(QLatin1Char('\n')),
        this);
    selectRecipeButton_->setObjectName(QStringLiteral("nativeSelectRecipeButton"));
    selectRecipeButton_->setEnabled(false);
    root->addWidget(selectRecipeButton_);

    selectRecipeReason_ = new QLabel(this);
    selectRecipeReason_->setObjectName(QStringLiteral("muted"));
    selectRecipeReason_->setWordWrap(true);
    root->addWidget(selectRecipeReason_);

    auto* sourceHeading = new QLabel(tr("Source assets"), this);
    sourceHeading->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(sourceHeading);

    auto* sourceHint = new QLabel(
        tr("The five pinned original AnimeJaNai spatial graphs (upstream 3.6.3, commit %1). "
           "A download is verified against its recorded SHA-256 and byte count before anything "
           "is written, and lands in the app-owned source store.")
            .arg(spatialAssetCommit()),
        this);
    sourceHint->setObjectName(QStringLiteral("muted"));
    sourceHint->setWordWrap(true);
    sourceHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(sourceHint);

    sourceTable_ = new QTableWidget(this);
    sourceTable_->setObjectName(QStringLiteral("nativeSourceTable"));
    sourceTable_->setAccessibleName(tr("Pinned source assets"));
    sourceTable_->setColumnCount(3);
    sourceTable_->setHorizontalHeaderLabels({tr("Asset"), tr("Size"), tr("Status")});
    sourceTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    sourceTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    sourceTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sourceTable_->verticalHeader()->setVisible(false);
    sourceTable_->horizontalHeader()->setStretchLastSection(true);
    sourceTable_->horizontalHeader()->setMinimumSectionSize(0);
    sourceTable_->setMaximumHeight(mikai::app::theme::kNativeAssetsTableMaxHeight);
    sourceTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    sourceTable_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(sourceTable_);

    licenseLabel_ = new QLabel(this);
    licenseLabel_->setObjectName(QStringLiteral("muted"));
    licenseLabel_->setWordWrap(true);
    licenseLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    {
        const SpatialAssetLicense license = spatialAssetLicense();
        licenseLabel_->setText(
            tr("Creator: %1. Licence: %2 (%3).\n%4")
                .arg(spatialAssetRepository(), license.name, license.url, license.useScope));
    }
    root->addWidget(licenseLabel_);

    auto* sourceRow = new QHBoxLayout();
    sourceRow->setSpacing(mikai::app::theme::kSpacingSmall);
    downloadButton_ = new QPushButton(tr("Download selected asset"), this);
    downloadButton_->setObjectName(QStringLiteral("nativeDownloadButton"));
    downloadButton_->setEnabled(false);
    sourceRow->addWidget(downloadButton_);
    downloadCancelButton_ = new QPushButton(tr("Cancel"), this);
    downloadCancelButton_->setObjectName(QStringLiteral("nativeDownloadCancelButton"));
    downloadCancelButton_->setEnabled(false);
    sourceRow->addWidget(downloadCancelButton_);
    importSourceButton_ = new QPushButton(tr("Import selected graph…"), this);
    importSourceButton_->setObjectName(QStringLiteral("nativeSourceImportButton"));
    importSourceButton_->setEnabled(false);
    sourceRow->addWidget(importSourceButton_);
    sourceRow->addStretch();
    root->addLayout(sourceRow);

    downloadProgress_ = new QProgressBar(this);
    downloadProgress_->setObjectName(QStringLiteral("nativeDownloadProgress"));
    // Indeterminate: the only honest progress this transport exposes. It shows
    // that a fetch is in flight, never a percentage that was not measured.
    downloadProgress_->setRange(0, 0);
    downloadProgress_->setTextVisible(false);
    downloadProgress_->hide();
    root->addWidget(downloadProgress_);

    sourceStatus_ = new QLabel(this);
    sourceStatus_->setObjectName(QStringLiteral("descriptionPanel"));
    sourceStatus_->setWordWrap(true);
    sourceStatus_->setMinimumHeight(mikai::app::theme::kSecondaryControlSize);
    root->addWidget(sourceStatus_);

    auto* preparationHeading = new QLabel(tr("Preparation"), this);
    preparationHeading->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(preparationHeading);

    auto* preparationRow = new QHBoxLayout();
    preparationRow->setSpacing(mikai::app::theme::kSpacingSmall);
    prepareButton_ = new QPushButton(tr("Prepare artifact"), this);
    prepareButton_->setObjectName(QStringLiteral("nativePrepareButton"));
    prepareButton_->setEnabled(false);
    preparationRow->addWidget(prepareButton_);
    preparationRow->addStretch();
    root->addLayout(preparationRow);

    preparationReason_ = new QLabel(this);
    preparationReason_->setObjectName(QStringLiteral("muted"));
    preparationReason_->setWordWrap(true);
    root->addWidget(preparationReason_);

    auto* cacheHeading = new QLabel(tr("Prepared artifacts"), this);
    cacheHeading->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(cacheHeading);

    cacheTable_ = new QTableWidget(this);
    cacheTable_->setObjectName(QStringLiteral("nativeCacheTable"));
    cacheTable_->setAccessibleName(tr("Prepared artifact cache"));
    cacheTable_->setColumnCount(4);
    cacheTable_->setHorizontalHeaderLabels({tr("Build key"), tr("Engine"), tr("Size"), tr("Status")});
    cacheTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    cacheTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    cacheTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    cacheTable_->verticalHeader()->setVisible(false);
    cacheTable_->horizontalHeader()->setStretchLastSection(true);
    cacheTable_->horizontalHeader()->setMinimumSectionSize(0);
    cacheTable_->setMaximumHeight(mikai::app::theme::kNativeAssetsTableMaxHeight);
    cacheTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    cacheTable_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(cacheTable_);

    auto* cacheRow = new QHBoxLayout();
    deleteCacheButton_ = new QPushButton(tr("Delete selected artifact"), this);
    deleteCacheButton_->setObjectName(QStringLiteral("nativeCacheDeleteButton"));
    deleteCacheButton_->setEnabled(false);
    cacheRow->addWidget(deleteCacheButton_);
    cacheRow->addStretch();
    root->addLayout(cacheRow);

    cacheStatus_ = new QLabel(this);
    cacheStatus_->setObjectName(QStringLiteral("muted"));
    cacheStatus_->setWordWrap(true);
    root->addWidget(cacheStatus_);

    auto* stagingHeading = new QLabel(tr("Incomplete scratch"), this);
    stagingHeading->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(stagingHeading);

    stagingList_ = new QListWidget(this);
    stagingList_->setObjectName(QStringLiteral("nativeStagingList"));
    stagingList_->setAccessibleName(tr("Incomplete scratch directories"));
    stagingList_->setMaximumHeight(mikai::app::theme::kNativeAssetsTableMaxHeight / 2);
    root->addWidget(stagingList_);

    auto* stagingRow = new QHBoxLayout();
    discardStagingButton_ = new QPushButton(tr("Discard selected scratch"), this);
    discardStagingButton_->setObjectName(QStringLiteral("nativeStagingDiscardButton"));
    discardStagingButton_->setEnabled(false);
    stagingRow->addWidget(discardStagingButton_);
    stagingRow->addStretch();
    root->addLayout(stagingRow);

    stagingStatus_ = new QLabel(this);
    stagingStatus_->setObjectName(QStringLiteral("muted"));
    stagingStatus_->setWordWrap(true);
    root->addWidget(stagingStatus_);

    root->addStretch();

    connect(importButton_, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Import ONNX model"), QString(), tr("ONNX graphs (*.onnx)"));
        if (path.isEmpty()) return;
        bool okWidth = false;
        bool okHeight = false;
        const int width = stageWidthEdit_->text().toInt(&okWidth);
        const int height = stageHeightEdit_->text().toInt(&okHeight);
        const QSize geometry = (okWidth && okHeight && width > 0 && height > 0)
                                   ? QSize(width, height)
                                   : QSize();
        importModel(path, geometry);
    });
    connect(cancelImportButton_, &QPushButton::clicked, this, &NativeAssetsPanel::cancelImport);
    connect(deleteCacheButton_, &QPushButton::clicked, this, [this] {
        const int row = cacheTable_->currentRow();
        if (row < 0 || row >= cache_.size()) return;
        deleteCacheEntry(cache_.at(row).buildKey);
    });
    connect(stagingList_, &QListWidget::currentRowChanged, this, [this](int row) {
        discardStagingButton_->setEnabled(row >= 0);
    });
    connect(discardStagingButton_, &QPushButton::clicked, this, [this] {
        const int row = stagingList_->currentRow();
        if (row < 0 || row >= staging_.size()) return;
        QString reasonKey;
        QString reason;
        if (discardStaging(artifacts_, staging_.at(row), &reasonKey, &reason)) {
            stagingStatus_->setText(tr("Discarded scratch directory %1.").arg(staging_.at(row)));
        } else {
            stagingStatus_->setText(reason);
        }
        refreshCache();
    });
    connect(modelTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = modelTable_->currentRow();
        if (row >= 0 && row < models_.size()) {
            const NativeModelView& view = models_.at(row);
            modelStatus_->setText(
                tr("Content id: %1\nBackends: %2\nTerms: %3")
                    .arg(view.contentId.isEmpty() ? QStringLiteral("—") : view.contentId,
                         view.backends.isEmpty() ? QStringLiteral("—")
                                                 : backendList(view.backends),
                         view.terms.isEmpty() ? QStringLiteral("—") : view.terms));
        } else {
            modelStatus_->clear();
        }
        updateImportControls();
    });
    connect(selectRecipeButton_, &QPushButton::clicked, this, [this] {
        const int row = modelTable_->currentRow();
        if (row < 0 || row >= models_.size()) return;
        selectModelAsRecipe(models_.at(row).id);
    });
    connect(sourceTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = sourceTable_->currentRow();
        if (row >= 0 && row < sourceAssets_.size()) {
            const SpatialAsset& asset = sourceAssets_.at(row);
            selectedSourceAssetId_ = asset.id;
            const QString published = publishedAssetPath(asset.id);
            sourceStatus_->setText(
                tr("%1\n%2 bytes · sha256:%3%4")
                    .arg(asset.name)
                    .arg(asset.bytes)
                    .arg(asset.sha256)
                    .arg(published.isEmpty() ? QString()
                                             : tr("\nStored at %1").arg(published)));
        } else {
            selectedSourceAssetId_.clear();
            sourceStatus_->clear();
        }
        updateDownloadControls();
    });
    connect(downloadButton_, &QPushButton::clicked, this, [this] {
        downloadSelectedSourceAsset();
    });
    connect(downloadCancelButton_, &QPushButton::clicked, this, &NativeAssetsPanel::cancelDownload);
    connect(importSourceButton_, &QPushButton::clicked, this, [this] {
        bool okWidth = false;
        bool okHeight = false;
        const int width = stageWidthEdit_->text().toInt(&okWidth);
        const int height = stageHeightEdit_->text().toInt(&okHeight);
        const QSize geometry = (okWidth && okHeight && width > 0 && height > 0)
                                   ? QSize(width, height)
                                   : QSize();
        importPublishedAsset(selectedSourceAssetId_, geometry);
    });
    connect(cacheTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = cacheTable_->currentRow();
        const bool proven = row >= 0 && row < cache_.size() && cache_.at(row).proven;
        deleteCacheButton_->setEnabled(proven);
        if (row >= 0 && row < cache_.size() && !proven) {
            cacheStatus_->setText(cache_.at(row).reason);
        }
    });

    updateStoreRootCaption();
}

void NativeAssetsPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (event->size().width() != event->oldSize().width()) updateStoreRootCaption();
}

void NativeAssetsPanel::updateStoreRootCaption() {
    if (storeRootLabel_ == nullptr) return;
    if (modelRoot_.isEmpty()) {
        storeRootLabel_->setText(tr("Store: %1").arg(tr("not available")));
        storeRootLabel_->setToolTip(QString());
        storeRootLabel_->setAccessibleDescription(QString());
        return;
    }
    const QFontMetrics metrics(storeRootLabel_->font());
    const QString pattern = tr("Store: %1");
    const qsizetype marker = pattern.indexOf(QStringLiteral("%1"));
    const QString prefix = marker < 0 ? QString() : pattern.left(marker);
    const QString suffix = marker < 0 ? QString() : pattern.mid(marker + 2);
    const int around = metrics.horizontalAdvance(prefix) + metrics.horizontalAdvance(suffix);
    const int available = storeRootLabel_->width() - around;
    storeRootLabel_->setText(pattern.arg(metrics.elidedText(
        modelRoot_, Qt::ElideMiddle, qMax(available, 0))));
    storeRootLabel_->setToolTip(modelRoot_);
    storeRootLabel_->setAccessibleDescription(modelRoot_);
}

void NativeAssetsPanel::setRunner(mikai::app::Runner* runner) {
    runner_ = runner;
    updateImportControls();
    updateDownloadControls();
}

void NativeAssetsPanel::setClient(mikai::backend::MikaiClient* client) {
    // Borrowed, never owned: the settings dialog keeps its client and passes it
    // so real downloads run through the app's one backend transport.
    client_ = client;
    updateDownloadControls();
}

void NativeAssetsPanel::refresh() {
    refreshModels();
    refreshCache();
    refreshSourceAssets();
    updateImportControls();
    updateDownloadControls();
}

void NativeAssetsPanel::refreshModels() {
    // The user's chosen model is a stable id, never a row index: rebuilding the
    // table must not move their choice.
    QString selectedModelId;
    const int previousRow = modelTable_->currentRow();
    if (previousRow >= 0 && previousRow < models_.size()) {
        selectedModelId = models_.at(previousRow).id;
    }

    models_.clear();
    storedModels_.clear();

    QString catalogKey;
    QString catalogReason;
#ifdef MIKAI_WITH_MODEL_INSPECTION
    const ModelInventory inventory(modelRoot_);
    QString ownedReason;
    if (!inventory.isOwned(&ownedReason)) {
        catalogKey = QStringLiteral("model.store.unowned");
        catalogReason = ownedReason.isEmpty()
                            ? tr("The model store root is not app-owned, so it is not read.")
                            : ownedReason;
    } else {
        QString reason;
        const QVector<StoredModel> rows = inventory.stored(&reason);
        storedModels_ = rows;
        for (const StoredModel& row : rows) models_.append(describeStoredModel(row));
        if (!reason.isEmpty()) {
            // A genuine read failure names itself; it is never hidden behind an
            // empty table.
            catalogKey = QStringLiteral("model.store.unreadable");
            catalogReason = reason;
        } else if (models_.isEmpty()) {
            // An owned store nothing has been imported into yet is honestly
            // empty, not missing: first use carries no diagnostic about a root
            // the app owns and will create on its first write.
            catalogKey = QStringLiteral("native.models.empty");
            catalogReason = tr("No models are stored yet.");
        }
    }
#else
    catalogKey = QStringLiteral("native.inspect.unavailable");
    catalogReason = tr("This build was made without the ONNX Runtime inspector "
                       "(MIKAI_WITH_MODEL_INSPECTION is off), so the owned model store cannot be "
                       "listed or imported into.");
#endif

    modelTable_->setRowCount(models_.size());
    for (int row = 0; row < models_.size(); ++row) {
        const NativeModelView& view = models_.at(row);
        const QStringList cells = {view.label,
                                   view.role,
                                   view.dtype.isEmpty() ? QStringLiteral("—") : view.dtype,
                                   humanBytes(view.bytes),
                                   backendList(view.backends),
                                   statusCell(view)};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells.at(column));
            if (view.usable) {
                item->setToolTip(cells.at(column));
            } else {
                item->setToolTip(tr("Reason: %1\n%2").arg(view.statusKey, view.statusReason));
            }
            modelTable_->setItem(row, column, item);
        }
    }

    // The status is this read's verdict, applied after the table is rebuilt:
    // rebuilding the rows changes the selection, and a refresh must never leave
    // a previous read's diagnostic standing under a table it no longer
    // describes. Both empty means there is nothing to report - a populated
    // table speaks for itself - and clears whatever stood there before.
    modelStatus_->setText(catalogReason.isEmpty() ? catalogKey : catalogReason);
    // Re-selecting by id restores the user's choice and its detail; with no
    // selection the catalogue's own verdict stays showing.
    if (!selectedModelId.isEmpty()) {
        for (int row = 0; row < models_.size(); ++row) {
            if (models_.at(row).id == selectedModelId) {
                modelTable_->setCurrentCell(row, 0);
                break;
            }
        }
    }
}

void NativeAssetsPanel::refreshCache() {
    cache_ = enumerateNativeCache(artifacts_);
    staging_ = stagingDirectories(artifacts_);

    cacheTable_->setRowCount(cache_.size());
    for (int row = 0; row < cache_.size(); ++row) {
        const NativeCacheEntry& entry = cache_.at(row);
        const QStringList cells = {
            entry.buildKey,
            entry.proven ? QFileInfo(entry.enginePath).fileName() : QStringLiteral("—"),
            entry.proven ? humanBytes(entry.engineBytes) : QStringLiteral("—"),
            entry.proven ? tr("Proven") : entry.reason};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells.at(column));
            item->setToolTip(entry.proven ? entry.enginePath : entry.reason);
            cacheTable_->setItem(row, column, item);
        }
    }
    deleteCacheButton_->setEnabled(false);

    stagingList_->clear();
    for (const QString& directory : staging_) stagingList_->addItem(directory);
    discardStagingButton_->setEnabled(false);
    stagingStatus_->setText(staging_.isEmpty()
                                ? tr("No incomplete scratch directories.")
                                : tr("%1 incomplete scratch director(ies) left by a cancelled or "
                                     "crashed build.")
                                      .arg(staging_.size()));
    if (cache_.isEmpty()) {
        cacheStatus_->setText(tr("No prepared artifacts in this build."));
    }
}

void NativeAssetsPanel::refreshSourceAssets() {
    sourceAssets_ = spatialAssetCatalog();
    publishedAssets_ = SpatialAssetStore().stored();

    sourceTable_->setRowCount(sourceAssets_.size());
    for (int row = 0; row < sourceAssets_.size(); ++row) {
        const SpatialAsset& asset = sourceAssets_.at(row);
        const QString path = publishedAssetPath(asset.id);
        const QString status =
            path.isEmpty() ? tr("Not downloaded") : tr("Downloaded and verified");
        const QStringList cells = {asset.name, humanBytes(asset.bytes), status};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells.at(column));
            item->setToolTip(path.isEmpty() ? asset.url : path);
            sourceTable_->setItem(row, column, item);
        }
    }

    // Keep the selection on the stable id, never the row index: a refresh must
    // not move the user's choice.
    if (!selectedSourceAssetId_.isEmpty()) {
        for (int row = 0; row < sourceAssets_.size(); ++row) {
            if (sourceAssets_.at(row).id == selectedSourceAssetId_) {
                sourceTable_->setCurrentCell(row, 0);
                break;
            }
        }
    }
    updateDownloadControls();
}

QString NativeAssetsPanel::publishedAssetPath(const QString& id) const {
    for (const StoredSpatialAsset& row : publishedAssets_) {
        if (row.verified && row.asset.id == id) return row.graphPath;
    }
    return {};
}

bool NativeAssetsPanel::selectSourceAsset(const QString& id) {
    for (int row = 0; row < sourceAssets_.size(); ++row) {
        if (sourceAssets_.at(row).id == id) {
            selectedSourceAssetId_ = id;
            sourceTable_->setCurrentCell(row, 0);
            updateDownloadControls();
            return true;
        }
    }
    applyStatus(QStringLiteral("native.download.unknown"),
                tr("%1 is not one of the pinned source assets.").arg(id), sourceStatus_);
    return false;
}

void NativeAssetsPanel::updateDownloadControls() {
    const SpatialAsset* selected = findSpatialAsset(selectedSourceAssetId_);
    const bool canDownload =
        selected != nullptr && client_ != nullptr && runner_ != nullptr && !downloadInFlight_;
    downloadButton_->setEnabled(canDownload);
    downloadCancelButton_->setEnabled(downloadInFlight_);
    downloadProgress_->setVisible(downloadInFlight_);

    if (selected == nullptr) {
        downloadButton_->setToolTip(tr("Select one of the pinned source assets."));
    } else if (client_ == nullptr) {
        downloadButton_->setToolTip(
            tr("No backend client is available, so nothing can be fetched."));
    } else if (runner_ == nullptr) {
        downloadButton_->setToolTip(
            tr("No off-thread runner is available, so no download can be submitted."));
    } else {
        downloadButton_->setToolTip(
            tr("Download %1 and verify it before storing it.").arg(selected->name));
    }

    bool runtimePresent = false;
#ifdef MIKAI_WITH_MODEL_INSPECTION
    runtimePresent = inspectionRuntimeIsPresent(runtime_, nullptr);
#endif
    const QString path = publishedAssetPath(selectedSourceAssetId_);
    const bool canImport = !path.isEmpty() && runtimePresent && !importInFlight_;
    importSourceButton_->setEnabled(canImport);
    importSourceButton_->setToolTip(
        path.isEmpty()
            ? tr("Download the selected source asset first; only a verified copy is imported.")
            : (runtimePresent
                   ? tr("Inspect and publish this graph into the owned model store at the stage "
                        "geometry above.")
                   : tr("The ONNX Runtime inspector is not available in this build, so the "
                        "downloaded graph cannot be imported.")));
}

bool NativeAssetsPanel::downloadSelectedSourceAsset() {
    applyStatus({}, {}, sourceStatus_);
    if (downloadInFlight_) {
        applyStatus(QStringLiteral("native.download.busy"),
                    tr("A download is already running; wait for it or cancel it first."),
                    sourceStatus_);
        return false;
    }
    const SpatialAsset* selected = findSpatialAsset(selectedSourceAssetId_);
    if (selected == nullptr) {
        applyStatus(QStringLiteral("native.download.selection"),
                    tr("Select one of the pinned source assets first."), sourceStatus_);
        return false;
    }
    if (client_ == nullptr) {
        applyStatus(QStringLiteral("native.download.client"),
                    tr("No backend client is available, so nothing can be fetched."),
                    sourceStatus_);
        return false;
    }
    if (runner_ == nullptr) {
        applyStatus(QStringLiteral("native.download.runner"),
                    tr("No off-thread runner is available, so no download can be submitted."),
                    sourceStatus_);
        return false;
    }

    auto cancellation = std::make_shared<ImportCancellation>();
    downloadCancel_ = cancellation;
    auto result = std::make_shared<DownloadResult>();
    downloadInFlight_ = true;
    const quint64 generation = ++downloadGeneration_;
    updateDownloadControls();

    const SpatialAsset asset = *selected;
    mikai::backend::MikaiClient* client = client_;

    runner_->run(
        [asset, client, cancellation, result]() -> QVariant {
            if (cancellation->isCancelled()) {
                result->cancelled = true;
                result->reasonKey = QStringLiteral("native.download.cancelled");
                result->reason = QCoreApplication::translate(
                    "NativeAssetsPanel", "The download was cancelled before it started.");
                return QVariant();
            }
            QByteArray bytes;
            try {
                bytes = client->fetchBytes(asset.url);
            } catch (const mikai::backend::MikaiApiError& error) {
                result->reasonKey = QStringLiteral("native.download.fetch");
                result->reason = QCoreApplication::translate(
                                     "NativeAssetsPanel", "The download of %1 failed: %2")
                                     .arg(asset.name, error.message.isEmpty() ? error.code
                                                                              : error.message);
                return QVariant();
            } catch (...) {
                result->reasonKey = QStringLiteral("native.download.fetch");
                result->reason = QCoreApplication::translate(
                                     "NativeAssetsPanel",
                                     "The download of %1 failed with an unknown transport error.")
                                     .arg(asset.name);
                return QVariant();
            }
            if (bytes.isEmpty()) {
                result->reasonKey = QStringLiteral("native.download.empty");
                result->reason = QCoreApplication::translate(
                                     "NativeAssetsPanel",
                                     "%1 returned no bytes, so nothing was written.")
                                     .arg(asset.name);
                return QVariant();
            }
            // The publication gate: the one atomic step that commits the asset.
            // A cancel that won while the bytes were in flight writes nothing.
            if (!cancellation->beginPublication()) {
                result->cancelled = true;
                result->reasonKey = QStringLiteral("native.download.cancelled");
                result->reason = QCoreApplication::translate(
                    "NativeAssetsPanel",
                    "The download was cancelled; nothing was written.");
                return QVariant();
            }
            const SpatialAssetPublication publication =
                publishSpatialAssetBytes(asset, bytes, QString());
            result->ok = publication.ok;
            result->published = publication.published;
            result->reasonKey = publication.reasonKey;
            result->reason = publication.reason;
            result->id = publication.id;
            result->path = publication.graphPath;
            // Only a cancel that won the publication gate is a cancellation; once
            // publishing began this stays false, so the committed result is not
            // concealed.
            if (cancellation->isCancelled()) result->cancelled = true;
            return QVariant();
        },
        [guard = QPointer<NativeAssetsPanel>(this), result, generation](const QVariant&) {
            if (guard) guard->finishDownload(result, generation);
        },
        [guard = QPointer<NativeAssetsPanel>(this), result, generation](const QString& message) {
            result->reasonKey = QStringLiteral("native.download.failed");
            result->reason = message;
            if (guard) guard->finishDownload(result, generation);
        });
    return true;
}

void NativeAssetsPanel::cancelDownload() {
    if (!downloadInFlight_ || !downloadCancel_) return;
    const ImportCancellation::CancelResult result = downloadCancel_->cancel();
    if (result == ImportCancellation::CancelResult::TooLate) {
        applyStatus(QStringLiteral("native.download.commitInProgress"),
                    tr("The download has already begun storing the asset, so it can no longer be "
                       "cancelled; its committed result will be reported."),
                    sourceStatus_);
        return;
    }
    applyStatus(QStringLiteral("native.download.cancelling"),
                tr("Cancelling the download; the worker decides at its next check."),
                sourceStatus_);
    updateDownloadControls();
}

void NativeAssetsPanel::finishDownload(const std::shared_ptr<DownloadResult>& result,
                                       quint64 generation) {
    if (generation != downloadGeneration_) return;
    downloadInFlight_ = false;
    downloadCancel_.reset();

    if (result->published) {
        // The worker passed the publication gate, so the copy is committed and
        // is reported as such, never re-labelled as a cancellation.
        applyStatus({}, {}, sourceStatus_);
    } else if (result->cancelled) {
        applyStatus(QStringLiteral("native.download.cancelled"),
                    tr("The download was cancelled; nothing was written."), sourceStatus_);
    } else if (result->ok) {
        applyStatus({}, {}, sourceStatus_);
    } else {
        applyStatus(result->reasonKey.isEmpty() ? QStringLiteral("native.download.failed")
                                                : result->reasonKey,
                    result->reason.isEmpty()
                        ? tr("The asset was refused and nothing was written.")
                        : result->reason,
                    sourceStatus_);
    }

    refreshSourceAssets();
    emit downloadFinished(result->ok);
}

bool NativeAssetsPanel::importPublishedAsset(const QString& id, const QSize& stageGeometry) {
    const QString path = publishedAssetPath(id);
    if (path.isEmpty()) {
        applyStatus(QStringLiteral("native.import.downloadedMissing"),
                    tr("That source asset is not present and verified in the owned store yet; "
                       "download it first."),
                    importStatus_);
        return false;
    }
    // The one import path: the existing ONNX inspector with an explicit geometry,
    // off the GUI thread. It publishes a model row and nothing else - no build,
    // no preparation, no Ready pipeline.
    return importModel(path, stageGeometry);
}

void NativeAssetsPanel::applyStatus(const QString& key, const QString& reason, QLabel* target) {
    statusKey_ = key;
    statusReason_ = reason;
    if (target == nullptr) return;
    target->setText(reason.isEmpty() ? key : reason);
}

void NativeAssetsPanel::updateImportControls() {
    bool runtimePresent = false;
#ifdef MIKAI_WITH_MODEL_INSPECTION
    runtimePresent = inspectionRuntimeIsPresent(runtime_, nullptr);
#endif
    const bool canImport = runtimePresent && runner_ != nullptr && !importInFlight_;
    importButton_->setEnabled(canImport);
    cancelImportButton_->setEnabled(importInFlight_);

    if (!runtimePresent) {
#ifdef MIKAI_WITH_MODEL_INSPECTION
        const QString reason = inspectionRuntimeReason();
#else
        const QString reason = tr("This build was made without the ONNX Runtime inspector "
                                  "(MIKAI_WITH_MODEL_INSPECTION is off).");
#endif
        importButton_->setToolTip(reason);
    } else if (runner_ == nullptr) {
        importButton_->setToolTip(
            tr("No off-thread runner is available, so no import can be submitted."));
    } else {
        importButton_->setToolTip(tr("Inspect and publish a graph into the owned store."));
    }

    const int selectedRow = modelTable_ != nullptr ? modelTable_->currentRow() : -1;
    const bool usableSelected =
        selectedRow >= 0 && selectedRow < models_.size() && models_.at(selectedRow).usable;
    selectRecipeButton_->setEnabled(usableSelected);
    selectRecipeReason_->setText(
        usableSelected
            ? tr("Registers this model as a single-spatial recipe for the current backend. "
                 "The request is written only when Save is pressed.")
            : tr("Select a model this build offers (Ready) to register it as a recipe."));

    prepareButton_->setText(tr("Prepare artifact"));
    prepareButton_->setToolTip(
        tr("Preparing an artifact needs the licensed AnimeJaNai source grant and its build "
           "toolchain, which has not been provided to this build."));
    preparationReason_->setText(
        tr("Preparation is unavailable: building a prepared engine needs the explicit AnimeJaNai "
           "source grant and its build toolchain, which have not been provided to this build. "
           "The five pinned original source graphs above can still be downloaded for private, "
           "non-commercial inspection, and a downloaded graph can be imported through the ONNX "
           "inspector; neither enables preparation. RIFE weights are not offered for download: "
           "their lineage is still open."));
    updateDownloadControls();
}

QString NativeAssetsPanel::inspectionRuntimeReason() const {
#ifdef MIKAI_WITH_MODEL_INSPECTION
    QString reason;
    if (!inspectionRuntimeIsPresent(runtime_, &reason)) {
        return reason.isEmpty() ? tr("The ONNX Runtime is not present.") : reason;
    }
#endif
    return {};
}

NativeAssetsPanel::~NativeAssetsPanel() {
    // Ask an in-flight worker to stop before this widget goes away. The worker
    // holds its own shared control, so the request is issued here and the GUI
    // thread never waits for the pool to observe it.
    if (importCancel_) importCancel_->cancel();
}

bool NativeAssetsPanel::importModel(const QString& sourcePath, const QSize& stageGeometry) {
    applyStatus({}, {}, importStatus_);
    if (!inspectorBuilds()) {
        applyStatus(QStringLiteral("native.inspect.unavailable"),
                    tr("This build was made without the ONNX Runtime inspector "
                       "(MIKAI_WITH_MODEL_INSPECTION is off), so no graph can be inspected or "
                       "imported."),
                    importStatus_);
        return false;
    }
    if (importInFlight_) {
        applyStatus(QStringLiteral("native.import.busy"),
                    tr("An import is already running; wait for it or cancel it first."),
                    importStatus_);
        return false;
    }
    if (sourcePath.trimmed().isEmpty() || !QFileInfo(sourcePath).isAbsolute()) {
        applyStatus(QStringLiteral("native.import.source"),
                    tr("Choose an absolute path to a regular ONNX file."), importStatus_);
        return false;
    }
    if (!stageGeometry.isValid() || stageGeometry.width() <= 0 || stageGeometry.height() <= 0) {
        applyStatus(QStringLiteral("native.import.geometry"),
                    tr("A positive stage geometry (WxH) is required: a dynamic graph's CPU "
                       "reference resolves at exactly this size, and it is never defaulted."),
                    importStatus_);
        return false;
    }
    if (runner_ == nullptr) {
        applyStatus(QStringLiteral("native.import.runner"),
                    tr("No off-thread runner is available, so no import can be submitted."),
                    importStatus_);
        return false;
    }
#ifdef MIKAI_WITH_MODEL_INSPECTION
    if (!inspectionRuntimeIsPresent(runtime_, nullptr)) {
        applyStatus(QStringLiteral("native.inspect.runtimeUnavailable"),
                    inspectionRuntimeReason(), importStatus_);
        return false;
    }

    auto cancellation = std::make_shared<ImportCancellation>();
    importCancel_ = cancellation;
    auto result = std::make_shared<ImportResult>();
    importInFlight_ = true;
    const quint64 generation = ++importGeneration_;
    updateImportControls();

    const QString root = modelRoot_;
    const InspectionRuntime runtime = runtime_;
    const QString path = QFileInfo(sourcePath).absoluteFilePath();

    runner_->run(
        [root, path, stageGeometry, runtime, cancellation, result]() -> QVariant {
            if (cancellation->isCancelled()) {
                result->cancelled = true;
                result->reasonKey = QStringLiteral("native.import.cancelled");
                result->reason = QCoreApplication::translate(
                    "NativeAssetsPanel", "The import was cancelled before it started.");
                return QVariant();
            }
            ModelInventory inventory(root);
            const ModelImportOutcome outcome =
                inventory.importFile(path, runtime, stageGeometry, BackendAvailability(),
                                     QString(), cancellation.get());
            result->ok = outcome.ok;
            result->published = outcome.published;
            result->reasonKey = outcome.reasonKey;
            result->reason = outcome.reason;
            result->modelId = outcome.model.record.id;
            // Only a cancel that won the publication gate is reported as one.
            // Once publishing has begun this stays false, so the committed
            // outcome is delivered unchanged.
            if (cancellation->isCancelled()) result->cancelled = true;
            return QVariant();
        },
        [guard = QPointer<NativeAssetsPanel>(this), result, generation](const QVariant&) {
            if (guard) guard->finishImport(result, generation);
        },
        [guard = QPointer<NativeAssetsPanel>(this), result, generation](const QString& message) {
            result->reasonKey = QStringLiteral("native.import.failed");
            result->reason = message;
            if (guard) guard->finishImport(result, generation);
        });
    return true;
#else
    return false;
#endif
}

void NativeAssetsPanel::cancelImport() {
    if (!importInFlight_ || !importCancel_) return;
    const ImportCancellation::CancelResult result = importCancel_->cancel();
    if (result == ImportCancellation::CancelResult::TooLate) {
        // The worker already passed the publication gate. Nothing can stop the
        // write, so the import is not reported as cancelled and its committed
        // outcome is still delivered by the completion.
        applyStatus(QStringLiteral("native.import.commitInProgress"),
                    tr("The import has already begun publishing, so it can no longer be "
                       "cancelled; its committed outcome will be reported."),
                    importStatus_);
        return;
    }
    applyStatus(QStringLiteral("native.import.cancelling"),
                tr("Cancelling the import; the worker decides at its next check."),
                importStatus_);
    updateImportControls();
}

void NativeAssetsPanel::finishImport(const std::shared_ptr<ImportResult>& result,
                                     quint64 generation) {
    if (generation != importGeneration_) return;
    importInFlight_ = false;
    importCancel_.reset();

    if (result->published) {
        // The worker passed the publication gate, so the row is committed and
        // the outcome is reported as such, never re-labelled as a cancellation.
        applyStatus({}, {}, importStatus_);
    } else if (result->cancelled) {
        applyStatus(QStringLiteral("native.import.cancelled"),
                    tr("The import was cancelled; nothing was published."), importStatus_);
    } else if (result->ok) {
        applyStatus({}, {}, importStatus_);
    } else {
        applyStatus(result->reasonKey.isEmpty() ? QStringLiteral("native.import.failed")
                                                : result->reasonKey,
                    result->reason.isEmpty()
                        ? tr("The graph was refused and nothing was published.")
                        : result->reason,
                    importStatus_);
    }

    refresh();
    emit importFinished(result->ok);
}

bool NativeAssetsPanel::deleteCacheEntry(const QString& buildKey) {
    QString reasonKey;
    QString reason;
    const bool removed = deleteNativeCacheEntry(artifacts_, cache_, buildKey, &reasonKey, &reason);
    applyStatus(reasonKey, reason, cacheStatus_);
    refreshCache();
    return removed;
}

void NativeAssetsPanel::setRequestedBackend(Backend backend) {
    requestedBackend_ = backend;
    updateImportControls();
}

bool NativeAssetsPanel::selectModelAsRecipe(const QString& modelId) {
    applyStatus({}, {}, selectRecipeReason_);
    const StoredModel* found = nullptr;
    for (const StoredModel& model : storedModels_) {
        if (model.record.id == modelId) {
            found = &model;
            break;
        }
    }
    if (found == nullptr) {
        applyStatus(QStringLiteral("native.recipe.modelUnknown"),
                    tr("That model is not one of the rows this store offers."),
                    selectRecipeReason_);
        return false;
    }
    if (!found->record.availability.isUsable()) {
        applyStatus(found->record.availability.reasonKey.isEmpty()
                        ? QStringLiteral("native.recipe.modelUnusable")
                        : found->record.availability.reasonKey,
                    found->record.availability.reason, selectRecipeReason_);
        return false;
    }

    // The store owns the recipe document and its stable id; this panel only asks
    // for it and reports the id. It never writes the requested selection.
    UserRecipeStore store;
    QString reasonKey;
    QString reason;
    const std::optional<Recipe> recipe =
        store.addSpatialModel(found->record, requestedBackend_, &reasonKey, &reason);
    if (!recipe.has_value()) {
        applyStatus(reasonKey, reason, selectRecipeReason_);
        return false;
    }
    selectRecipeReason_->setText(tr("Registered recipe %1 for %2.")
                                     .arg(recipe->id, stableName(requestedBackend_)));
    emit recipeRequested(recipe->id);
    return true;
}

bool NativeAssetsPanel::inspectorBuilds() const {
#ifdef MIKAI_WITH_MODEL_INSPECTION
    return true;
#else
    return false;
#endif
}

}  // namespace native
}  // namespace mplayer
