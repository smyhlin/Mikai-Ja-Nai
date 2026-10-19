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
#include <QtCore/QEvent>
#include <QtGui/QShowEvent>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QList>
#include <QtCore/QLocale>
#include <QtCore/QMetaObject>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTemporaryDir>
#include <QtCore/QVariant>
#include <QtGui/QIntValidator>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <backend/Errors.hpp>

#include <native/bundle/NativeBundle.hpp>
#include <native/models/NativeInspectedPlan.hpp>
#include <native/bundle/NativeManifest.hpp>
#include <native/bundle/NativePaths.hpp>
#include <native/prepare/NativePreparationPlan.hpp>
#include <native/prepare/NativePreparationReferences.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>
#include <native/models/NativeSpatialAssets.hpp>
#include <native/models/NativeUserRecipes.hpp>
#include <native/components/NativeComponentStore.hpp>
#include <native/components/NativeComponentToolchain.hpp>
#include <native/components/NativeGpuProbe.hpp>
#include "state/NativeComponentProvisioner.hpp"
#include "workers/Runner.hpp"
#include "theme/Metrics.hpp"

namespace mikai::app {

// The panel owns the app-side surface; the model, recipe and artifact
// vocabulary it reads stays in mplayer::native and is named through this
// file-local using-directive rather than re-homed.
using namespace mplayer::native;

namespace {

constexpr const char* kArtifactDirectoryPrefix = "artifact-";

QString humanBytes(qint64 bytes) {
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
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

QString nativePreparationToolchainReason(const QString& reasonKey, const QString& reason) {
    if (reasonKey == QLatin1String("native.prepare.provenance")) {
        return QCoreApplication::translate(
            "NativeAssetsPanel",
            "The selected native bundle declares no preparation provenance, so its toolchain is "
            "not a validated one this build will build against.");
    }
    if (reasonKey == QLatin1String("native.prepare.toolsUnavailable")) {
        return QCoreApplication::translate(
            "NativeAssetsPanel",
            "The preparation toolchain is not installed: the inspect, prep and verify "
            "executables and an ONNX Runtime directory must all be present before an engine can "
            "be built.");
    }
    if (reasonKey == QLatin1String("native.prepare.backendUnavailable")) {
        return QCoreApplication::translate(
            "NativeAssetsPanel",
            "The observed toolchain carries no provider runtime this build can prepare for.");
    }
    // A component store's or a provider's own reason is not this panel's to reword.
    return reason;
}

NativePreparationToolchain observeNativePreparationToolchain() {
    NativePreparationToolchain toolchain;
    const auto refuse = [&toolchain](const QString& key, const QString& reason) {
        toolchain.available = false;
        toolchain.reasonKey = key;
        toolchain.reason = reason;
        return toolchain;
    };
    // The reasons this panel words itself are recorded through the same helper a
    // LanguageChange reads, so the observation and its later restatement can never
    // drift; a provider's own reason still passes through `refuse` verbatim.
    const auto refuseWithKey = [&toolchain](const QString& key) {
        toolchain.available = false;
        toolchain.reasonKey = key;
        toolchain.reason = nativePreparationToolchainReason(key, QString());
        return toolchain;
    };

    // The selected native bundle is where a locally built toolchain and its
    // provider runtimes live. Nothing is created, copied or downloaded here.
    QString bundleRoot;
    std::optional<Bundle> validated;
    const RuntimeSelection& selection = currentSelection();
    if (selection.loaded && selection.kind == BundleKind::Native && !selection.root.isEmpty()) {
        bundleRoot = selection.root;
        toolchain.modulePath = selection.modulePath;
        validated = selection.resolvedBundle;
    }
    if (bundleRoot.isEmpty()) {
        const QList<Bundle> bundles = discoverBundles();
        for (const Bundle& candidate : bundles) {
            if (candidate.kind == BundleKind::Native) {
                bundleRoot = candidate.root;
                toolchain.modulePath = candidate.modulePath;
                validated = candidate;
                break;
            }
        }
    }

    // The selected bundle's own preparation provenance is the only source of the
    // toolchain: the declared executables and provider runtime directories, the
    // inference core's source SHA and ABI, and the provider types it carries. Each
    // path is resolved against the validated bundle root - absolute wherever the
    // block already states one - and nothing is searched for on PATH or beside the
    // binary: an SDK nobody validated is not this build's toolchain.
    if (!validated.has_value() || !validated->preparation.has_value()) {
        return refuseWithKey(QStringLiteral("native.prepare.provenance"));
    }
    const BundlePreparation& preparation = *validated->preparation;
    toolchain.tools.inspectExecutable = QDir(bundleRoot).filePath(preparation.tools.inspect);
    toolchain.tools.prepExecutable = QDir(bundleRoot).filePath(preparation.tools.prep);
    toolchain.tools.verifierExecutable = QDir(bundleRoot).filePath(preparation.tools.verifier);
    toolchain.tools.ortRuntimeDir = QDir(bundleRoot).filePath(preparation.runtime.ort);

    // The TensorRT and CUDA bin directories are the bundle's own, unless this
    // machine's GPU needs a builder resource the bundle does not carry: then
    // both come from the app-owned component store, so the runtime and the
    // resource are always the same upstream pack set.
    QStringList declaredTrtFiles;
    const QString trtPrefix = preparation.runtime.trt + QLatin1Char('/');
    for (const BundleFile& file : validated->files) {
        if (file.path.startsWith(trtPrefix)) {
            declaredTrtFiles.append(file.path);
        }
    }
    const ComponentBinDirs componentDirs = componentBinDirs(
        bundleRoot, preparation.runtime.trt, preparation.runtime.cuda, declaredTrtFiles,
        observeComputeCapability(), componentStore());
    if (!componentDirs.reasonKey.isEmpty()) {
        return refuse(componentDirs.reasonKey, componentDirs.reason);
    }
    toolchain.tools.trtBinDir = componentDirs.trtBinDir;
    toolchain.tools.cudaBinDir = componentDirs.cudaBinDir;
    toolchain.coreSha = preparation.coreSha;
    toolchain.sourceAbi = preparation.sourceAbi;
    // The block's declared provider types are the bundle's own statement of which
    // backends it carries; a backend the block does not declare is not offered
    // even if a DLL happens to be present.
    for (const QString& type : preparation.types) {
        const std::optional<Backend> declared = backendFromName(type);
        if (declared.has_value() && !toolchain.backends.available.contains(*declared)) {
            toolchain.backends.available.append(*declared);
        }
    }

    // The prepared provider plugin's content revision: the digest of the actual
    // compiled plugin the validated bundle carries, so a cache entry is tied to
    // real bytes rather than to a version string nobody measured.
    const auto digestOf = [&validated](const QString& declaredPath) -> QString {
        for (const BundleFile& file : validated->files) {
            if (file.path == declaredPath) return QStringLiteral("sha256:") + file.sha256;
        }
        return {};
    };
    toolchain.pluginShaTensorRt =
        digestOf(bundlePreparationBackendLibrary(QStringLiteral("tensorrt")));
    toolchain.pluginShaDirectMl =
        digestOf(bundlePreparationBackendLibrary(QStringLiteral("directml")));

    if (toolchain.tools.inspectExecutable.isEmpty() ||
        toolchain.tools.prepExecutable.isEmpty() ||
        toolchain.tools.verifierExecutable.isEmpty() ||
        toolchain.tools.ortRuntimeDir.isEmpty()) {
        return refuseWithKey(QStringLiteral("native.prepare.toolsUnavailable"));
    }

    // The provider runtimes the validated bundle declared. A backend it does not
    // name is not offered even if a DLL happens to be present: the block is the
    // statement of what this toolchain carries.
    if (toolchain.backends.available.isEmpty()) {
        return refuseWithKey(QStringLiteral("native.prepare.backendUnavailable"));
    }

    toolchain.available = true;
    return toolchain;
}


namespace {

/// One-line summaries for each actionable section, using the store's own facts.
QString modelsSummaryLine(const QVector<NativeModelView>& models) {
    int usable = 0;
    for (const NativeModelView& view : models) {
        if (view.usable) ++usable;
    }
    if (models.isEmpty()) {
        return QCoreApplication::translate("NativeAssetsPanel", "No owned models yet");
    }
    return QCoreApplication::translate("NativeAssetsPanel", "%1 model(s), %2 ready")
        .arg(models.size())
        .arg(usable);
}

QString cacheSummaryLine(const QVector<NativeCacheEntry>& entries) {
    int proven = 0;
    for (const NativeCacheEntry& entry : entries) {
        if (entry.proven) ++proven;
    }
    if (entries.isEmpty()) {
        return QCoreApplication::translate("NativeAssetsPanel", "No prepared artifacts");
    }
    return QCoreApplication::translate("NativeAssetsPanel", "%1 artifact(s), %2 proven")
        .arg(entries.size())
        .arg(proven);
}

}  // namespace

NativeAssetsPanel::NativeAssetsPanel(QWidget* parent)
    : QWidget(parent), artifacts_(ArtifactStore()) {
    // The single address a delivered worker body may hand a result to. One panel
    // owns the import/preparation surface at a time; the destructor clears it.
    livePanel_ = this;
    modelRoot_ = nativeSubdirectory(QStringLiteral("models"));
#ifdef MIKAI_WITH_MODEL_INSPECTION
    runtime_ = defaultInspectionRuntime();
#endif
    buildUi();
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

    inventoryHeading_ = new QLabel(tr("Owned models"), this);
    inventoryHeading_->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(inventoryHeading_);

    modelSummary_ = new QLabel(this);
    modelSummary_->setObjectName(QStringLiteral("muted"));
    modelSummary_->setWordWrap(true);
    modelSummary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(modelSummary_);

    modelTable_ = new QTableWidget(this);
    modelTable_->setObjectName(QStringLiteral("nativeModelTable"));
    modelTable_->setAccessibleName(tr("Owned model inventory"));
    modelTable_->setColumnCount(3);
    modelTable_->setHorizontalHeaderLabels({tr("Model"), tr("Role"), tr("Status")});
    modelTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    modelTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    modelTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    modelTable_->setAlternatingRowColors(true);
    modelTable_->setShowGrid(false);
    modelTable_->verticalHeader()->setVisible(false);
    modelTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    modelTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    modelTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
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
    importDetailsToggle_ = new QPushButton(tr("Import a file…"), this);
    importDetailsToggle_->setObjectName(QStringLiteral("nativeImportDetailsToggle"));
    importDetailsToggle_->setAccessibleName(tr("Show the file-import controls"));
    importDetailsToggle_->setCheckable(true);
    importDetailsToggle_->setChecked(false);
    importRow->addWidget(importDetailsToggle_);
    importRow->addStretch();
    root->addLayout(importRow);

    // The file import is the rare path: it hides behind the toggle above, so
    // the common model and preparation actions own the first screen.
    importDetails_ = new QWidget(this);
    auto* importBox = new QVBoxLayout(importDetails_);
    importBox->setContentsMargins(0, 0, 0, 0);
    importBox->setSpacing(mikai::app::theme::kSpacingSmall);
    auto* importFields = new QHBoxLayout();
    importFields->setSpacing(mikai::app::theme::kSpacingSmall);
    importButton_ = new QPushButton(tr("Import"), importDetails_);
    importButton_->setObjectName(QStringLiteral("nativeImportButton"));
    importButton_->setAccessibleName(tr("Import a model"));
    importFields->addWidget(importButton_);

    stageWidthLabel_ = new QLabel(tr("Stage W"), importDetails_);
    // The field's own caption yields width too, but it stays a single line: it is
    // short by design and wrapping it would stack "Stage" over "W".
    stageWidthLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    importFields->addWidget(stageWidthLabel_);
    stageWidthEdit_ = new QLineEdit(importDetails_);
    stageWidthEdit_->setObjectName(QStringLiteral("nativeStageWidthEdit"));
    // The visible label is the field's accessible name, and the field is that
    // label's buddy: the two dimension boxes were the only actionable controls
    // here without an accessible name, and the label above them carries none of
    // its own. Both reuse the label's already-translated text, so no new string
    // enters the catalogue.
    stageWidthEdit_->setAccessibleName(stageWidthLabel_->text());
    stageWidthLabel_->setBuddy(stageWidthEdit_);
    stageWidthEdit_->setValidator(new QIntValidator(1, 100000, stageWidthEdit_));
    stageWidthEdit_->setFixedWidth(mikai::app::theme::kNativeDimensionFieldWidth);
    importFields->addWidget(stageWidthEdit_);

    stageHeightLabel_ = new QLabel(tr("H"), importDetails_);
    stageHeightLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    importFields->addWidget(stageHeightLabel_);
    stageHeightEdit_ = new QLineEdit(importDetails_);
    stageHeightEdit_->setObjectName(QStringLiteral("nativeStageHeightEdit"));
    stageHeightEdit_->setAccessibleName(stageHeightLabel_->text());
    stageHeightLabel_->setBuddy(stageHeightEdit_);
    stageHeightEdit_->setValidator(new QIntValidator(1, 100000, stageHeightEdit_));
    stageHeightEdit_->setFixedWidth(mikai::app::theme::kNativeDimensionFieldWidth);
    importFields->addWidget(stageHeightEdit_);

    cancelImportButton_ = new QPushButton(tr("Cancel"), importDetails_);
    cancelImportButton_->setObjectName(QStringLiteral("nativeImportCancelButton"));
    cancelImportButton_->setEnabled(false);
    importFields->addWidget(cancelImportButton_);
    importFields->addStretch();
    importBox->addLayout(importFields);

    importStatus_ = new QLabel(importDetails_);
    importStatus_->setObjectName(QStringLiteral("descriptionPanel"));
    importStatus_->setWordWrap(true);
    importStatus_->setMinimumHeight(mikai::app::theme::kSecondaryControlSize);
    importBox->addWidget(importStatus_);
    importDetails_->hide();
    root->addWidget(importDetails_);

    selectRecipeButton_ = new QPushButton(tr("Select as recipe"), this);
    selectRecipeButton_->setObjectName(QStringLiteral("nativeSelectRecipeButton"));
    selectRecipeButton_->setEnabled(false);
    root->addWidget(selectRecipeButton_);

    selectRecipeReason_ = new QLabel(this);
    selectRecipeReason_->setObjectName(QStringLiteral("muted"));
    selectRecipeReason_->setWordWrap(true);
    selectRecipeReason_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(selectRecipeReason_);

    preparationHeading_ = new QLabel(tr("Preparation"), this);
    preparationHeading_->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(preparationHeading_);

    auto* preparationRow = new QHBoxLayout();
    preparationRow->setSpacing(mikai::app::theme::kSpacingSmall);
    prepareButton_ = new QPushButton(tr("Prepare artifact"), this);
    prepareButton_->setObjectName(QStringLiteral("nativePrepareButton"));
    prepareButton_->setAccessibleName(tr("Prepare the selected recipe"));
    prepareButton_->setEnabled(false);
    preparationRow->addWidget(prepareButton_);
    cancelPrepareButton_ = new QPushButton(tr("Cancel"), this);
    cancelPrepareButton_->setObjectName(QStringLiteral("nativePrepareCancelButton"));
    cancelPrepareButton_->setEnabled(false);
    preparationRow->addWidget(cancelPrepareButton_);
    preparationRow->addStretch();
    root->addLayout(preparationRow);

    prepareProgress_ = new QLabel(this);
    prepareProgress_->setObjectName(QStringLiteral("muted"));
    prepareProgress_->setWordWrap(true);
    root->addWidget(prepareProgress_);

    preparationReason_ = new QLabel(this);
    preparationReason_->setObjectName(QStringLiteral("muted"));
    preparationReason_->setWordWrap(true);
    root->addWidget(preparationReason_);

    cacheHeading_ = new QLabel(tr("Prepared artifacts"), this);
    cacheHeading_->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(cacheHeading_);

    // The cache table is optional detail; the summary row remains visible.
    auto* cacheSummaryRow = new QHBoxLayout();
    cacheSummaryRow->setSpacing(mikai::app::theme::kSpacingSmall);
    cacheStatus_ = new QLabel(this);
    cacheStatus_->setObjectName(QStringLiteral("muted"));
    cacheStatus_->setWordWrap(true);
    cacheStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    cacheSummaryRow->addWidget(cacheStatus_, 1);
    cacheDetailsToggle_ = new QPushButton(tr("Details…"), this);
    cacheDetailsToggle_->setObjectName(QStringLiteral("nativeCacheDetailsToggle"));
    cacheDetailsToggle_->setAccessibleName(tr("Show the prepared-artifact details"));
    cacheDetailsToggle_->setCheckable(true);
    cacheDetailsToggle_->setChecked(false);
    cacheSummaryRow->addWidget(cacheDetailsToggle_);
    root->addLayout(cacheSummaryRow);

    cacheDetails_ = new QWidget(this);
    auto* cacheBox = new QVBoxLayout(cacheDetails_);
    cacheBox->setContentsMargins(0, 0, 0, 0);
    cacheBox->setSpacing(mikai::app::theme::kSpacingSmall);
    cacheTable_ = new QTableWidget(cacheDetails_);
    cacheTable_->setObjectName(QStringLiteral("nativeCacheTable"));
    cacheTable_->setAccessibleName(tr("Prepared artifact cache"));
    cacheTable_->setColumnCount(3);
    cacheTable_->setHorizontalHeaderLabels({tr("Engine"), tr("Size"), tr("Status")});
    cacheTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    cacheTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    cacheTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    cacheTable_->setAlternatingRowColors(true);
    cacheTable_->setShowGrid(false);
    cacheTable_->verticalHeader()->setVisible(false);
    cacheTable_->horizontalHeader()->setStretchLastSection(true);
    cacheTable_->horizontalHeader()->setMinimumSectionSize(0);
    cacheTable_->setMaximumHeight(mikai::app::theme::kNativeAssetsTableMaxHeight);
    cacheTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    cacheTable_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    cacheBox->addWidget(cacheTable_);

    auto* cacheRow = new QHBoxLayout();
    deleteCacheButton_ = new QPushButton(tr("Delete"), cacheDetails_);
    deleteCacheButton_->setObjectName(QStringLiteral("nativeCacheDeleteButton"));
    deleteCacheButton_->setEnabled(false);
    cacheRow->addWidget(deleteCacheButton_);
    cacheDeleteHint_ = new QLabel(tr("Remove the selected cache artifact."), cacheDetails_);
    cacheDeleteHint_->setObjectName(QStringLiteral("muted"));
    cacheDeleteHint_->setWordWrap(true);
    cacheDeleteHint_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // Bound to the delete action it explains, like the scratch legend below.
    cacheDeleteHint_->setBuddy(deleteCacheButton_);
    cacheRow->addWidget(cacheDeleteHint_, 1);
    cacheBox->addLayout(cacheRow);
    cacheDetails_->hide();
    root->addWidget(cacheDetails_);

    stagingHeading_ = new QLabel(tr("Incomplete scratch"), this);
    stagingHeading_->setObjectName(QStringLiteral("sectionHeading"));
    stagingHeading_->hide();
    root->addWidget(stagingHeading_);

    stagingList_ = new QListWidget(this);
    stagingList_->setObjectName(QStringLiteral("nativeStagingList"));
    stagingList_->setAccessibleName(tr("Incomplete scratch directories"));
    stagingList_->setMaximumHeight(mikai::app::theme::kNativeAssetsTableMaxHeight / 2);
    // Absolute paths are the items here, so the list scrolls rather than voting
    // for a width no window can hold.
    stagingList_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    stagingList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    stagingList_->hide();
    root->addWidget(stagingList_);

    auto* stagingRow = new QHBoxLayout();
    discardStagingButton_ = new QPushButton(tr("Discard"), this);
    discardStagingButton_->setObjectName(QStringLiteral("nativeStagingDiscardButton"));
    discardStagingButton_->setEnabled(false);
    stagingRow->addWidget(discardStagingButton_);
    stagingDiscardHint_ = new QLabel(tr("Remove the selected incomplete scratch directory."), this);
    stagingDiscardHint_->setObjectName(QStringLiteral("muted"));
    stagingDiscardHint_->setWordWrap(true);
    stagingDiscardHint_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // Bound to the discard action it explains, like the other row legends.
    stagingDiscardHint_->setBuddy(discardStagingButton_);
    stagingRow->addWidget(stagingDiscardHint_, 1);
    stagingRow->addStretch();
    // The scratch cleanup hides with the list: nothing incomplete, nothing to show.
    stagingRow->setEnabled(false);
    root->addLayout(stagingRow);
    stagingNote_ = new QLabel(this);
    stagingNote_->setObjectName(QStringLiteral("muted"));
    stagingNote_->hide();
    root->addWidget(stagingNote_);

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
        const QString directory = staging_.at(row);
        // Removing scratch is destructive and cannot be undone, so it is
        // confirmed by the exact directory the user selected. Only a path
        // `stagingDirectories` proved to be an owned `.staging-*` directory is
        // ever offered here; a proven artifact is never a candidate.
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, tr("Discard incomplete scratch"),
            tr("Remove the incomplete scratch directory %1?\n\n"
               "This removes only work a cancelled or crashed build left behind; a proven "
               "artifact is never affected.")
                .arg(directory),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
        QString reasonKey;
        QString reason;
        if (discardStaging(artifacts_, directory, &reasonKey, &reason)) {
            applyStatus({}, tr("Discarded scratch directory %1.").arg(directory), stagingNote_);
        } else {
            applyStatus(reasonKey, reason, stagingNote_);
        }
        refreshCache();
    });
    connect(modelTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        updateImportControls();
    });
    connect(selectRecipeButton_, &QPushButton::clicked, this, [this] {
        const int row = modelTable_->currentRow();
        if (row < 0 || row >= models_.size()) return;
        selectModelAsRecipe(models_.at(row).id);
    });
    connect(importDetailsToggle_, &QPushButton::toggled, this, [this](bool on) {
        if (importDetails_ != nullptr) importDetails_->setVisible(on);
        importDetailsToggle_->setText(on ? tr("Hide import") : tr("Import a file…"));
        importDetailsToggle_->setAccessibleName(
            on ? tr("Hide the file-import controls")
               : tr("Show the file-import controls"));
    });
    connect(cacheDetailsToggle_, &QPushButton::toggled, this, [this](bool on) {
        if (cacheDetails_ != nullptr) cacheDetails_->setVisible(on);
        cacheDetailsToggle_->setText(on ? tr("Hide details") : tr("Details…"));
        cacheDetailsToggle_->setAccessibleName(
            on ? tr("Hide the prepared-artifact details")
               : tr("Show the prepared-artifact details"));
    });
    connect(cacheTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = cacheTable_->currentRow();
        const bool proven = row >= 0 && row < cache_.size() && cache_.at(row).proven;
        deleteCacheButton_->setEnabled(proven);
        if (row >= 0 && row < cache_.size() && !proven) {
            cacheStatus_->setText(cache_.at(row).reason);
        }
    });
    connect(prepareButton_, &QPushButton::clicked, this, [this] { prepareArtifact(); });
    connect(cancelPrepareButton_, &QPushButton::clicked, this,
            &NativeAssetsPanel::cancelPrepare);

    // Every caption yields its width instead of voting for the page's minimum: a
    // long translated heading, hint or provenance path wraps or scrolls, and the
    // panel never pushes the settings page wider than the smallest window.
    const QList<QLabel*> captions = {
        modelSummary_, inventoryHeading_, preparationHeading_,
        preparationReason_, prepareProgress_, cacheHeading_, cacheDeleteHint_,
        cacheStatus_, stagingDiscardHint_, stagingNote_};
    for (QLabel* caption : captions) {
        if (caption == nullptr) continue;
        caption->setWordWrap(true);
        caption->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    // The visible captions are the concise verb, because a QPushButton cannot
    // wrap; the accessible name keeps the full, specific action so a screen
    // reader never loses what the button does in any state.
    cancelImportButton_->setAccessibleName(tr("Cancel the running model import"));
    selectRecipeButton_->setAccessibleName(
        tr("Register the selected model as the requested recipe"));
    deleteCacheButton_->setAccessibleName(tr("Delete the selected prepared artifact"));
    discardStagingButton_->setAccessibleName(
        tr("Discard the selected incomplete scratch directory"));
    cancelPrepareButton_->setAccessibleName(tr("Cancel the running preparation"));

}

void NativeAssetsPanel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void NativeAssetsPanel::retranslateUi() {
    // In place. Nothing is re-read: the owned model rows, the artifact cache
    // and the prepared pipeline are the store's own facts, and re-reading them
    // to say the same thing in a new language would move the user's selection,
    // reset a revealed scratch directory and hide whatever an in-flight import
    // or preparation was reporting. A worker already running keeps running: its
    // gate, its generation and its callbacks are untouched, and its completion
    // paints itself in whatever language is active when it lands.
    inventoryHeading_->setText(tr("Owned models"));
    modelTable_->setAccessibleName(tr("Owned model inventory"));
    modelTable_->setHorizontalHeaderLabels({tr("Model"), tr("Role"), tr("Status")});

    stageWidthLabel_->setText(tr("Stage W"));
    stageHeightLabel_->setText(tr("H"));
    stageWidthEdit_->setAccessibleName(stageWidthLabel_->text());
    stageHeightEdit_->setAccessibleName(stageHeightLabel_->text());
    importButton_->setText(tr("Import"));
    importButton_->setAccessibleName(tr("Import a model"));
    cancelImportButton_->setText(tr("Cancel"));
    cancelImportButton_->setAccessibleName(tr("Cancel the running model import"));
    importDetailsToggle_->setText(importDetails_ != nullptr && importDetails_->isVisible()
                                      ? tr("Hide import")
                                      : tr("Import a file…"));
    importDetailsToggle_->setAccessibleName(importDetails_ != nullptr && importDetails_->isVisible()
                                                ? tr("Hide the file-import controls")
                                                : tr("Show the file-import controls"));
    selectRecipeButton_->setText(tr("Select as recipe"));
    selectRecipeButton_->setAccessibleName(
        tr("Register the selected model as the requested recipe"));

    preparationHeading_->setText(tr("Preparation"));
    cancelPrepareButton_->setText(tr("Cancel"));
    cancelPrepareButton_->setAccessibleName(tr("Cancel the running preparation"));

    cacheHeading_->setText(tr("Prepared artifacts"));
    cacheTable_->setAccessibleName(tr("Prepared artifact cache"));
    cacheDetailsToggle_->setText(cacheDetailsToggle_->isChecked() ? tr("Hide details") : tr("Details…"));
    cacheDetailsToggle_->setAccessibleName(
        cacheDetailsToggle_->isChecked() ? tr("Hide the prepared-artifact details")
                                        : tr("Show the prepared-artifact details"));
    cacheTable_->setHorizontalHeaderLabels({tr("Engine"), tr("Size"), tr("Status")});
    deleteCacheButton_->setText(tr("Delete"));
    deleteCacheButton_->setAccessibleName(tr("Delete the selected prepared artifact"));
    cacheDeleteHint_->setText(tr("Remove the selected cache artifact."));

    stagingHeading_->setText(tr("Incomplete scratch"));
    stagingList_->setAccessibleName(tr("Incomplete scratch directories"));
    discardStagingButton_->setText(tr("Discard"));
    discardStagingButton_->setAccessibleName(
        tr("Discard the selected incomplete scratch directory"));
    stagingDiscardHint_->setText(tr("Remove the selected incomplete scratch directory."));

    // The control refreshes restate only this panel's own captions and
    // tooltips; the per-row verdicts keep whatever the store and the producer
    // last said about them, because they are the record rather than chrome.
    updateImportControls();
    updateSummaries();
    updatePrepareControls();

    // The preparation captions are typed state, not chrome: the probe, capture,
    // stage or settled verdict already on screen is re-worded from what the panel
    // holds, and the builder's own phase, detail, reason and tool output stay
    // verbatim. Nothing is re-observed, re-probed, re-captured, submitted,
    // cancelled or refreshed here.
    renderPrepareProgress();
    if (prepareInFlight_ && prepareProgressState_.kind == PrepareProgress::Kind::Stage) {
        // The stage on screen is re-announced with the same index, count, phase and
        // detail, so a listener restates only its own wrapper around the same work.
        emit preparationProgress(prepareProgressState_.stageIndex,
                                 prepareProgressState_.stageCount, prepareProgressState_.phase,
                                 prepareProgressState_.detail);
    }
}

void NativeAssetsPanel::setRunner(mikai::app::Runner* runner) {
    runner_ = runner;
    updateImportControls();
}

void NativeAssetsPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!initialRefreshDone_) {
        initialRefreshDone_ = true;
        refresh();
    }
}

void NativeAssetsPanel::refresh() {
    initialRefreshDone_ = true;
    refreshModels();
    refreshCache();
    refreshSourceData();
    // One observation per refresh, on the GUI thread: the toolchain and the
    // provider runtimes the validated bundle and the application directory
    // really carry. A selection change reads this value instead of re-walking it.
    observedToolchain_ = observeNativePreparationToolchain();
    updateImportControls();
    updateSummaries();
    updatePrepareControls();
    emit catalogRefreshed();
}

void NativeAssetsPanel::updateSummaries() {
    if (modelSummary_ != nullptr) modelSummary_->setText(modelsSummaryLine(models_));
    if (cacheStatus_ != nullptr) cacheStatus_->setText(cacheSummaryLine(cache_));
    cacheDetailsToggle_->setVisible(!cache_.isEmpty());
    cacheDetails_->setVisible(cacheDetailsToggle_->isChecked() && !cache_.isEmpty());
    const bool hasScratch = !staging_.isEmpty();
    if (stagingHeading_ != nullptr) stagingHeading_->setVisible(hasScratch);
    if (stagingList_ != nullptr) stagingList_->setVisible(hasScratch);
    discardStagingButton_->setVisible(hasScratch);
    stagingDiscardHint_->setVisible(hasScratch);
    if (stagingNote_ != nullptr) {
        stagingNote_->setVisible(hasScratch);
        if (hasScratch) {
            stagingNote_->setText(tr("%1 incomplete scratch director(ies) left by a cancelled or crashed build.").arg(staging_.size()));
        }
    }
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

    // The owned store's read path is unconditional: listing and describing the
    // app-owned rows needs no ONNX Runtime, so the catalogue is readable even in
    // a build without the inspector. Importing a new graph still is not.
    QString catalogKey;
    QString catalogReason;
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

    modelTable_->setRowCount(models_.size());
    modelTable_->setVisible(!models_.isEmpty());
    for (int row = 0; row < models_.size(); ++row) {
        const NativeModelView& view = models_.at(row);
        const QStringList cells = {view.label, view.role, statusCell(view)};
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

    // The read's named failure remains visible; a valid empty store is represented
    // once by the existing model summary.
    const bool emptyStore = catalogKey == QLatin1String("native.models.empty");
    modelStatus_->setText(catalogReason.isEmpty() ? catalogKey : catalogReason);
    modelStatus_->setVisible(!emptyStore && !modelStatus_->text().isEmpty());
    // Re-selecting by id restores the user's choice without changing its status.
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
    updateSummaries();
}

void NativeAssetsPanel::refreshSourceData() {
    // Widget-free: the pinned catalogue and the published copies the data API
    // resolves by digest. The Components tab owns the visible inventory.
    sourceAssets_ = spatialAssetCatalog();
    publishedAssets_ = SpatialAssetStore().stored();
}
QString NativeAssetsPanel::publishedAssetPath(const QString& id) const {
    for (const StoredSpatialAsset& row : publishedAssets_) {
        if (row.verified && row.asset.id == id) return row.graphPath;
    }
    return {};
}

bool NativeAssetsPanel::importPublishedAsset(const QString& id, const QSize& stageGeometry) {
    const QString path = publishedAssetPath(id);
    if (path.isEmpty()) {
        applyStatus(QStringLiteral("native.import.downloadedMissing"),
                    tr("That source asset is not present and verified in the owned store yet; "
                       "get and verify it in Components first."),
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


}  // namespace mikai::app
