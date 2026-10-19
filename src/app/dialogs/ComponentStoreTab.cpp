#include "dialogs/ComponentStoreTabInternal.hpp"
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
#include <native/components/NativeComponentProvisioning.hpp>
#include <native/components/NativeGpuProbe.hpp>
#include <native/models/NativeRife.hpp>
#include <native/models/NativeSpatialAssets.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QEvent>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>
#include <QtGui/QFont>
#include <QtGui/QMouseEvent>
#include <QtGui/QPalette>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QFrame>
#include <QtWidgets/QStyle>
#include <QtCore/QVariant>
#include <QtCore/QVariantMap>
#include <utility>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include "widgets/FlowLayout.hpp"
#include <QtWidgets/QVBoxLayout>

#include <stdexcept>
#include <optional>

namespace mikai::app {

using namespace mplayer::native;


namespace component_store_detail {
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


const StoredSpatialAsset* spatialInventoryRow(const QVector<StoredSpatialAsset>& stored,
                                             const QString& id) {
    for (const StoredSpatialAsset& row : stored) {
        if (row.asset.id == id) return &row;
    }
    return nullptr;
}

QVariantMap inventorySnapshot(const GpuComputeCapability& gpu, bool verifyRife,
                              bool rehashBackend, const std::atomic<bool>* cancelled) {
    const auto isCancelled = [cancelled] {
        return cancelled != nullptr && cancelled->load();
    };
    QVariantMap result;
    const ComponentStore store = componentStore();
    const RifeArchiveStore rifeStore = rifeArchiveStore();
    const ComponentPack* runtimePack = tensorRtRuntimePack();
    const ComponentPack* kernelsPack = tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
    const StoredPack runtimeStored = runtimePack != nullptr ? storedPack(store, *runtimePack)
                                                             : StoredPack{};
    const StoredPack kernelsStored = kernelsPack != nullptr ? storedPack(store, *kernelsPack)
                                                             : StoredPack{};
    const AnimeJanaiInstallation trt =
        describeAnimeJanaiInstallation(Backend::TensorRT, rehashBackend);
    const AnimeJanaiInstallation directMl =
        describeAnimeJanaiInstallation(Backend::DirectML, rehashBackend);
    const ComponentBinDirs dirs = installedComponentBinDirs(gpu, store);
    const bool componentDirsAvailable = dirs.reasonKey.isEmpty();
    const bool runtimeInstalled = trt.installed &&
                                  (runtimeStored.installed || componentDirsAvailable);
    const bool kernelsInstalled = kernelsPack == nullptr || kernelsStored.installed ||
                                  componentDirsAvailable;
    result.insert(QStringLiteral("trtInstalled"), trt.installed);
    result.insert(QStringLiteral("trtReasonKey"), trt.reasonKey);
    result.insert(QStringLiteral("trtReason"), trt.reason);
    result.insert(QStringLiteral("directMlInstalled"), directMl.installed);
    result.insert(QStringLiteral("directMlReasonKey"), directMl.reasonKey);
    result.insert(QStringLiteral("directMlReason"), directMl.reason);
    result.insert(QStringLiteral("runtimePackInstalled"), runtimeStored.installed);
    result.insert(QStringLiteral("kernelsPackInstalled"), kernelsStored.installed);
    result.insert(QStringLiteral("componentDirsAvailable"), componentDirsAvailable);
    result.insert(QStringLiteral("runtimeRecommended"), gpu.nvidia && !runtimeInstalled);
    result.insert(QStringLiteral("kernelsRecommended"),
                  gpu.nvidia && kernelsPack != nullptr && !kernelsInstalled);
    result.insert(QStringLiteral("tensorRtClosureBytes"), animeJanaiMissingDownloadBytes(Backend::TensorRT));
    result.insert(QStringLiteral("directMlClosureBytes"), animeJanaiMissingDownloadBytes(Backend::DirectML));
    result.insert(QStringLiteral("componentReasonKey"), dirs.reasonKey);
    result.insert(QStringLiteral("componentReason"), dirs.reason);
    QVariantList componentFailures;
    if (rehashBackend) {
        for (const ComponentPack* pack : compatiblePacksFor(gpu.smMajor, gpu.smMinor)) {
            if (isCancelled()) {
                result.insert(QStringLiteral("cancelled"), true);
                return result;
            }
            if (storedPack(store, *pack).installed && !verifyPack(store, *pack).installed)
                componentFailures.append(pack->name);
        }
    }
    result.insert(QStringLiteral("componentFailures"), componentFailures);

    bool rifeInstalled = !rifeModelPins().isEmpty();
    bool rifeVerified = true;
    QVariantList rifeFailures;
    for (const RifeModelPin& pin : rifeModelPins()) {
        if (isCancelled()) {
            result.insert(QStringLiteral("cancelled"), true);
            return result;
        }
        const StoredRifeGraph stored = storedRifeGraph(rifeStore, pin);
        rifeInstalled = rifeInstalled && stored.installed;
        if (!verifyRife) continue;
        if (!stored.installed) {
            rifeVerified = false;
            continue;
        }
        const StoredRifeGraph graph = verifyRifeGraph(rifeStore, pin);
        if (!graph.installed) {
            rifeVerified = false;
            rifeFailures.append(QVariantMap{{QStringLiteral("file"), pin.fileName},
                                            {QStringLiteral("reason"), graph.reason}});
        }
    }
    result.insert(QStringLiteral("rifeInstalled"), rifeInstalled);
    result.insert(QStringLiteral("rifeVerified"), rifeVerified);
    result.insert(QStringLiteral("rifeFailures"), rifeFailures);

    QVariantList spatialRows;
    for (const StoredSpatialAsset& row : storedSpatialAssets()) {
        if (isCancelled()) {
            result.insert(QStringLiteral("cancelled"), true);
            return result;
        }
        spatialRows.append(QVariantMap{{QStringLiteral("id"), row.asset.id},
                                       {QStringLiteral("path"), row.graphPath},
                                       {QStringLiteral("verified"), row.verified},
                                       {QStringLiteral("reasonKey"), row.reasonKey},
                                       {QStringLiteral("reason"), row.reason}});
    }
    result.insert(QStringLiteral("spatial"), spatialRows);
    result.insert(QStringLiteral("cancelled"), false);
    return result;
}

}  // namespace component_store_detail

using namespace component_store_detail;

ComponentStoreTab::ComponentStoreTab(QWidget* parent) : QWidget(parent) {
    alive_ = std::make_shared<std::atomic<bool>>(true);
    buildUi();
    refresh();
    scheduleInventoryRefresh();
}

ComponentStoreTab::~ComponentStoreTab() {
    // Every queued completion and every in-flight worker checks this before it
    // touches the tab, so destruction can never race a delivery: the flag goes
    // first, then the generation is bumped so a body already past the flag
    // still finds a run that can no longer be rendered. Closing Settings does
    // not destroy the app-owned tab; this runs when the process exits and when
    // a dialog that created its own tab (a probe) is destroyed. The Cancel
    // button is the viewer's stop.
    if (alive_) alive_->store(false);
    ++runGeneration_;
    verifying_ = false;
    if (provisioner_ != nullptr) provisioner_->cancel();
    if (acquisitionCheckCancelFlag_) acquisitionCheckCancelFlag_->store(true);
    if (animeJanaiCancelFlag_) animeJanaiCancelFlag_->store(true);
    if (rifeCancelFlag_) rifeCancelFlag_->store(true);
    if (modelCancelFlag_) modelCancelFlag_->store(true);
}

QString ComponentStoreTab::setupPrompt(qint64* bytes) const {
    if (bytes != nullptr) *bytes = 0;
    if (!inventoryReady_) return {};
    const GpuComputeCapability& gpu = observeComputeCapability();
    QStringList missing;
    qint64 totalBytes = 0;
    auto translate = [](const char* source) {
        return QCoreApplication::translate("mikai::app::ComponentStoreTab", source);
    };
    auto append = [&](const QString& title, qint64 size) {
        missing.append(QStringLiteral("<li>%1 — %2 MB</li>")
                           .arg(title.toHtmlEscaped())
                           .arg(QLocale().toString(size / 1048576)));
        totalBytes += size;
    };

    if (gpu.nvidia) {
        if (tensorRtClosureBytes_ > 0) {
            append(translate(QT_TRANSLATE_NOOP(
                       "mikai::app::ComponentStoreTab", "AnimeJaNai TensorRT closure")),
                   tensorRtClosureBytes_);
        } else if (!componentDirsAvailable_) {
            const ComponentPack* kernel =
                tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
            if (kernel != nullptr) append(kernel->name, kernel->assetBytes);
        }
#ifdef Q_OS_WIN
    } else if (directMlClosureBytes_ > 0) {
        append(translate(QT_TRANSLATE_NOOP(
                   "mikai::app::ComponentStoreTab", "AnimeJaNai DirectML closure")),
               directMlClosureBytes_);
#endif
    }

    if (!rifeInstalled_)
        append(translate(QT_TRANSLATE_NOOP(
                   "mikai::app::ComponentStoreTab", "RIFE interpolation models")),
               rifeArchivePack().assetBytes);
    qint64 missingSpatialBytes = 0;
    int missingSpatialCount = 0;
    const QVector<SpatialAsset> spatialCatalog = spatialAssetCatalog();
    for (const SpatialAsset& asset : spatialCatalog) {
        const StoredSpatialAsset* row = spatialInventoryRow(storedSpatial_, asset.id);
        if (row == nullptr || !row->verified) {
            missingSpatialBytes += asset.bytes;
            ++missingSpatialCount;
        }
    }
    if (missingSpatialCount > 0)
        append(translate(QT_TRANSLATE_NOOP(
                   "mikai::app::ComponentStoreTab",
                   "AnimeJaNai upscaling models (%1 of %2 missing)"))
                   .arg(missingSpatialCount).arg(spatialCatalog.size()),
               missingSpatialBytes);
    if (bytes != nullptr) *bytes = totalBytes;
    if (missing.isEmpty()) return {};
    return translate(QT_TRANSLATE_NOOP(
               "mikai::app::ComponentStoreTab",
               "Components recommended for this PC are not installed:"))
        + QStringLiteral("<ul>") + missing.join(QString()) + QStringLiteral("</ul>")
        + translate(QT_TRANSLATE_NOOP(
               "mikai::app::ComponentStoreTab",
               "Download and install them now (%1 MB)?"))
              .arg(QLocale().toString(totalBytes / 1048576));
}

void ComponentStoreTab::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(theme::kSpacingNone, theme::kSpacingNone,
                              theme::kSpacingNone, theme::kSpacingNone);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(theme::kSpacingSmall, theme::kSpacingMedium,
                             theme::kSpacingSmall, theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    auto* headerCard = new QFrame(content);
    headerCard->setObjectName(QStringLiteral("settingsCard"));
    auto* headerLayout = new QVBoxLayout(headerCard);
    headerLayout->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium,
                                     theme::kSpacingMedium, theme::kSpacingMedium);
    headerLayout->setSpacing(theme::kSpacingSmall);

    heading_ = new QLabel(headerCard);
    heading_->setObjectName(QStringLiteral("heading"));
    heading_->setWordWrap(true);
    heading_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    headerLayout->addWidget(heading_);

    hint_ = new QLabel(headerCard);
    hint_->setObjectName(QStringLiteral("muted"));
    hint_->setWordWrap(true);
    headerLayout->addWidget(hint_);

    gpuLabel_ = new QLabel(headerCard);
    gpuLabel_->setObjectName(QStringLiteral("descriptionPanel"));
    gpuLabel_->setWordWrap(true);
    QFont gpuFont = gpuLabel_->font();
    gpuFont.setBold(true);
    gpuLabel_->setFont(gpuFont);
    headerLayout->addWidget(gpuLabel_);

    auto* backendRow = new QHBoxLayout();
    backendRow->setSpacing(theme::kSpacingSmall);
    animeJanaiBackendLabel_ = new QLabel(headerCard);
    animeJanaiBackendLabel_->setObjectName(QStringLiteral("muted"));
    backendRow->addWidget(animeJanaiBackendLabel_, 0, Qt::AlignVCenter);
    animeJanaiBackendCombo_ = new QComboBox(headerCard);
    animeJanaiBackendCombo_->setObjectName(QStringLiteral("animeJanaiRuntimeBackend"));
    animeJanaiBackendCombo_->setMinimumHeight(theme::kSettingsControlSize);
    animeJanaiBackendCombo_->setAccessibleName(tr("AnimeJaNai backend used by Apply changes and Verify"));
    animeJanaiBackendCombo_->addItem(tr("TensorRT (CUDA / NVDEC)"),
                                     static_cast<int>(Backend::TensorRT));
    animeJanaiBackendCombo_->addItem(tr("DirectML (D3D11VA)"),
                                     static_cast<int>(Backend::DirectML));
    animeJanaiBackendLabel_->setBuddy(animeJanaiBackendCombo_);
    backendRow->addWidget(animeJanaiBackendCombo_, 1, Qt::AlignVCenter);
    headerLayout->addLayout(backendRow);

    root->addWidget(headerCard);

    recommendationBanner_ = new QLabel(content);
    recommendationBanner_->setObjectName(QStringLiteral("componentRecommendationBanner"));
    recommendationBanner_->setWordWrap(true);
    root->addWidget(recommendationBanner_);

    auto syncCardSelectionStyle = [](QGroupBox* card, bool selected) {
        if (card == nullptr) return;
        card->setProperty("selected", selected);
        card->style()->unpolish(card);
        card->style()->polish(card);
    };

    auto makeCard = [this, root, content](const QString& objectName, QCheckBox** checkBox,
                                          QLabel** stateLabel) {
        auto* card = new QGroupBox(content);
        card->setObjectName(QStringLiteral("nativeComponentPackCard"));
        card->setProperty("componentCardId", objectName);
        card->setCursor(Qt::PointingHandCursor);
        card->installEventFilter(this);
        auto* cardLayout = new QHBoxLayout(card);
        cardLayout->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium,
                                       theme::kSpacingMedium, theme::kSpacingMedium);
        cardLayout->setSpacing(theme::kSpacingMedium);
        *checkBox = new QCheckBox(card);
        (*checkBox)->setObjectName(objectName + QStringLiteral("Check"));
        (*checkBox)->setMinimumSize(theme::kSettingsControlSize, theme::kSettingsControlSize);
        cardLayout->addWidget(*checkBox, 0, Qt::AlignVCenter);
        auto* details = new QWidget(card);
        details->installEventFilter(this);
        auto* detailsLayout = new QVBoxLayout(details);
        detailsLayout->setContentsMargins(0, 0, 0, 0);
        detailsLayout->setSpacing(theme::kSpacingXs);
        auto* titleRow = new QHBoxLayout();
        titleRow->setSpacing(theme::kSpacingSmall);
        auto* title = new QLabel(details);
        title->setObjectName(objectName + QStringLiteral("Title"));
        title->setWordWrap(true);
        title->installEventFilter(this);
        QFont titleFont = title->font();
        titleFont.setBold(true);
        title->setFont(titleFont);
        titleRow->addWidget(title);
        *stateLabel = new QLabel(details);
        (*stateLabel)->setObjectName(objectName + QStringLiteral("State"));
        (*stateLabel)->setWordWrap(true);
        (*stateLabel)->installEventFilter(this);
        titleRow->addWidget(*stateLabel);
        titleRow->addStretch();
        detailsLayout->addLayout(titleRow);
        auto* description = new QLabel(details);
        description->setObjectName(objectName + QStringLiteral("Description"));
        description->setProperty("textRole", QStringLiteral("muted"));
        description->setWordWrap(true);
        description->installEventFilter(this);
        detailsLayout->addWidget(description);
        cardLayout->addWidget(details, 1);
        auto* size = new QLabel(card);
        size->setObjectName(objectName + QStringLiteral("Size"));
        size->setProperty("textRole", QStringLiteral("muted"));
        size->setWordWrap(true);
        size->installEventFilter(this);
        QFont sizeFont = size->font();
        sizeFont.setBold(true);
        size->setFont(sizeFont);
        cardLayout->addWidget(size, 0, Qt::AlignVCenter);
        root->addWidget(card);
        return card;
    };
    auto* runtimeCard = makeCard(QStringLiteral("tensorRtRuntimeCard"),
                                 &runtimeCheck_, &runtimeState_);
    auto* spatialCard = makeCard(QStringLiteral("animeJanaiModelsCard"),
                                 &spatialCheck_, &spatialState_);
    auto* rifeCard = makeCard(QStringLiteral("rifeModelsCard"), &rifeCheck_, &rifeState_);
    auto* kernelsCard = makeCard(QStringLiteral("tensorRtKernelsCard"),
                                 &kernelsCheck_, &kernelsState_);
    for (auto* card : {runtimeCard, spatialCard, rifeCard, kernelsCard}) {
        card->setProperty("componentCard", true);
    }
    auto setCardText = [](QGroupBox* card, const QString& titleText,
                          const QString& descriptionText) {
        const QString cardId = card->property("componentCardId").toString();
        card->findChild<QLabel*>(cardId + QStringLiteral("Title"))->setText(titleText);
        card->findChild<QLabel*>(cardId + QStringLiteral("Description"))
            ->setText(descriptionText);
    };
    setCardText(runtimeCard, tr("TensorRT runtime"),
                tr("Fastest NVIDIA upscaling engine; without it NVIDIA falls back to DirectML."));
    setCardText(rifeCard, tr("RIFE interpolation models"),
                tr("Frame interpolation, not needed for upscale only."));
    setCardText(spatialCard, tr("AnimeJaNai upscaling models"),
                tr("All five pinned HD and SD graphs required by the built-in AnimeJaNai profiles."));
    setCardText(kernelsCard, tr("TensorRT kernels for this GPU"),
                tr("Engine-builder kernels matched to this GPU generation."));
    auto updateRuntimeSelection = [this](bool checked) {
        if (checked) selectedPacks_.insert(QStringLiteral("component:runtime"));
        else selectedPacks_.remove(QStringLiteral("component:runtime"));
    };
    connect(runtimeCheck_, &QCheckBox::toggled, this,
            [this, runtimeCard, updateRuntimeSelection, syncCardSelectionStyle](bool checked) {
                updateRuntimeSelection(checked);
                syncCardSelectionStyle(runtimeCard, checked);
                updateControls();
            });
    connect(rifeCheck_, &QCheckBox::toggled, this,
            [this, rifeCard, syncCardSelectionStyle](bool checked) {
                if (checked) selectedPacks_.insert(QStringLiteral("rife:archive"));
                else selectedPacks_.remove(QStringLiteral("rife:archive"));
                syncCardSelectionStyle(rifeCard, checked);
                updateControls();
            });
    connect(spatialCheck_, &QCheckBox::toggled, this,
            [this, spatialCard, syncCardSelectionStyle](bool checked) {
                if (checked) selectedPacks_.insert(QStringLiteral("component:spatial"));
                else selectedPacks_.remove(QStringLiteral("component:spatial"));
                syncCardSelectionStyle(spatialCard, checked);
                updateControls();
            });
    connect(kernelsCheck_, &QCheckBox::toggled, this,
            [this, kernelsCard, syncCardSelectionStyle](bool checked) {
                if (checked) selectedPacks_.insert(QStringLiteral("component:kernels"));
                else selectedPacks_.remove(QStringLiteral("component:kernels"));
                syncCardSelectionStyle(kernelsCard, checked);
                updateControls();
            });

    auto* actionsCard = new QFrame(content);
    actionsCard->setObjectName(QStringLiteral("settingsCard"));
    auto* actions = new QVBoxLayout(actionsCard);
    actions->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium,
                                theme::kSpacingMedium, theme::kSpacingMedium);
    actions->setSpacing(theme::kSpacingSmall);
    auto* primaryActions = new FlowLayout(theme::kSpacingSmall, theme::kSpacingSmall);
    downloadButton_ = new QPushButton(actionsCard);
    downloadButton_->setObjectName(QStringLiteral("primary"));
    downloadButton_->setMinimumHeight(theme::kSettingsControlSize);
    primaryActions->addWidget(downloadButton_);
    auto* refreshButton = new QPushButton(actionsCard);
    refreshButton->setObjectName(QStringLiteral("refreshComponentsButton"));
    refreshButton->setMinimumHeight(theme::kSettingsControlSize);
    primaryActions->addWidget(refreshButton);
    actions->addLayout(primaryActions);
    auto* utilityActions = new FlowLayout(theme::kSpacingSmall, theme::kSpacingSmall);
    verifyButton_ = new QPushButton(actionsCard);
    verifyButton_->setObjectName(QStringLiteral("nativeComponentVerifyButton"));
    verifyButton_->setMinimumHeight(theme::kSettingsControlSize);
    utilityActions->addWidget(verifyButton_);
    importAnimeJanaiButton_ = new QPushButton(actionsCard);
    importAnimeJanaiButton_->setObjectName(QStringLiteral("nativeComponentImportAnimeJanaiButton"));
    importAnimeJanaiButton_->setMinimumHeight(theme::kSettingsControlSize);
    utilityActions->addWidget(importAnimeJanaiButton_);
    cancelButton_ = new QPushButton(actionsCard);
    cancelButton_->setObjectName(QStringLiteral("nativeComponentTabCancelButton"));
    cancelButton_->setMinimumHeight(theme::kSettingsControlSize);
    cancelButton_->setEnabled(false);
    utilityActions->addWidget(cancelButton_);
    actions->addLayout(utilityActions);

    progress_ = new QProgressBar(actionsCard);
    progress_->setObjectName(QStringLiteral("nativeComponentTabProgress"));
    progress_->setRange(0, 100);
    progress_->setTextVisible(false);
    progress_->hide();
    actions->addWidget(progress_);

    status_ = new QLabel(actionsCard);
    status_->setObjectName(QStringLiteral("nativeComponentStatus"));
    status_->setAccessibleName(tr("Components status"));
    status_->setWordWrap(true);
    actions->addWidget(status_);
    root->addWidget(actionsCard);

    root->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll);

    trackedBackend_ = animeJanaiBackendCombo_->currentData().toInt();
    connect(animeJanaiBackendCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index < 0) return;
                const QVariant data = animeJanaiBackendCombo_->itemData(index);
                if (!data.isValid()) return;
                const int backend = data.toInt();
                // Closing Settings reparents this tab, and showing it again can
                // emit this for the row that is already selected. That is not a
                // backend change: retiring the generation here hid the bar and
                // dropped every later progress update while the download ran on.
                if (backend == trackedBackend_) return;
                trackedBackend_ = backend;
                // A different backend answers a different question: any run still
                // in flight was hashing the previous one, so its results are
                // discarded instead of being rendered as fresh evidence. The
                // concurrent acquisition's callbacks carry the old generation
                // and are dropped, so its units are settled here instead of
                // leaving a run nobody can finish.
                if (animeJanaiCancelFlag_) animeJanaiCancelFlag_->store(true);
                if (rifeCancelFlag_) rifeCancelFlag_->store(true);
                if (modelCancelFlag_) modelCancelFlag_->store(true);
                acquisitionUnitsPending_ = 0;
                acquisitionSequenceActive_ = false;
                modelsDownloading_ = false;
                animeJanaiRuntimeDownloading_ = false;
                ++runGeneration_;
                verifying_ = false;
                cancelProgress();
                refresh();
            });
    connect(verifyButton_, &QPushButton::clicked, this, &ComponentStoreTab::verifyAll);
    connect(downloadButton_, &QPushButton::clicked, this,
            [this] { downloadMissing(false); });
    connect(findChild<QPushButton*>(QStringLiteral("refreshComponentsButton")),
            &QPushButton::clicked, this, &ComponentStoreTab::refresh);
    connect(importAnimeJanaiButton_, &QPushButton::clicked, this,
            &ComponentStoreTab::chooseAnimeJanaiRuntimeDirectory);
    connect(cancelButton_, &QPushButton::clicked, this, &ComponentStoreTab::cancelAcquisition);

    retranslateUi();
}

void ComponentStoreTab::retranslateUi() {
    heading_->setText(tr("Backend components"));
    hint_->setText(tr("Choose which recommended components, AnimeJaNai upscaling models and optional RIFE models to install."));
    auto setCardText = [this](const QString& objectName, const QString& title,
                              const QString& description) {
        findChild<QLabel*>(objectName + QStringLiteral("Title"))->setText(title);
        findChild<QLabel*>(objectName + QStringLiteral("Description"))->setText(description);
    };
    setCardText(QStringLiteral("tensorRtRuntimeCard"), tr("TensorRT runtime"),
                tr("Fastest NVIDIA upscaling engine; without it NVIDIA falls back to DirectML."));
    setCardText(QStringLiteral("rifeModelsCard"), tr("RIFE interpolation models"),
                tr("Frame interpolation, not needed for upscale only."));
    setCardText(QStringLiteral("animeJanaiModelsCard"), tr("AnimeJaNai upscaling models"),
                tr("All five pinned HD and SD graphs required by the built-in AnimeJaNai profiles."));
    setCardText(QStringLiteral("tensorRtKernelsCard"),
                tr("TensorRT kernels for this GPU"),
                tr("Engine-builder kernels matched to this GPU generation."));
    verifyButton_->setText(tr("Verify"));
    verifyButton_->setAccessibleName(tr("Re-check every installed element against its digest"));
    downloadButton_->setText(tr("Apply changes"));
    downloadButton_->setAccessibleName(tr("Apply selected component changes"));
    if (auto* refreshBtn = findChild<QPushButton*>(QStringLiteral("refreshComponentsButton"))) {
        refreshBtn->setText(tr("Refresh"));
        refreshBtn->setAccessibleName(tr("Refresh"));
    }
    importAnimeJanaiButton_->setText(tr("Install local runtime..."));
    importAnimeJanaiButton_->setAccessibleName(
        tr("Choose a local AnimeJaNai runtime directory and install its selected backend closure"));
    cancelButton_->setText(tr("Cancel"));
    cancelButton_->setAccessibleName(tr("Cancel the running download"));
    status_->setAccessibleName(tr("Components status"));
    animeJanaiBackendCombo_->setAccessibleName(
        tr("AnimeJaNai backend used by Apply changes and Verify"));
    animeJanaiBackendCombo_->setItemText(0, tr("TensorRT (CUDA / NVDEC)"));
    animeJanaiBackendCombo_->setItemText(1, tr("DirectML (D3D11VA)"));
    if (animeJanaiBackendLabel_ != nullptr) {
        animeJanaiBackendLabel_->setText(tr("AnimeJaNai backend:"));
    }
    recommendationBanner_->setText(
        QStringLiteral("!  ") +
        tr("Components recommended for this PC are not installed. Check the highlighted items below, then click Apply changes to install them."));
}

bool ComponentStoreTab::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonRelease) {
        const auto* mouseEvent = static_cast<const QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            QWidget* widget = qobject_cast<QWidget*>(watched);
            while (widget != nullptr && widget != this) {
                if (auto* card = qobject_cast<QGroupBox*>(widget);
                    card != nullptr && card->objectName() == QStringLiteral("nativeComponentPackCard")) {
                    const QString cardId = card->property("componentCardId").toString();
                    if (auto* check = card->findChild<QCheckBox*>(cardId + QStringLiteral("Check"));
                        check != nullptr && check->isEnabled() && check->isVisibleTo(card)) {
                        check->toggle();
                        return true;
                    }
                    break;
                }
                widget = widget->parentWidget();
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ComponentStoreTab::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
        renderCards();
        // The status line is app-owned text, not a static label: re-derive it
        // in the live language and let the existing notification restate it
        // outside this tab. No run, progress, selection or timer is touched.
        restateStatus();
    }
}

void ComponentStoreTab::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
}

void ComponentStoreTab::setRunner(Runner* runner) {
    // A different executor answers to a different lifetime: anything still in
    // flight from the previous one is discarded rather than rendered.
    if (runner_ != runner) {
        ++runGeneration_;
        verifying_ = false;
        cancelProgress();
    }
    runner_ = runner;
    updateControls();
    scheduleInventoryRefresh();
}

void ComponentStoreTab::setClient(backend::MikaiClient* client) {
    if (client_ != client) {
        ++runGeneration_;
        verifying_ = false;
        cancelProgress();
    }
    client_ = client;
    updateControls();
}

void ComponentStoreTab::setComponentProvisioner(NativeComponentProvisioner* provisioner) {
    if (provisioner_ == provisioner) {
        updateControls();
        return;
    }
    ++runGeneration_;
    verifying_ = false;
    if (provisioner_ != nullptr) disconnect(provisioner_, nullptr, this, nullptr);
    provisioner_ = provisioner;
    if (provisioner_ != nullptr) {
        connect(provisioner_, &NativeComponentProvisioner::progress, this,
                [this](const QString& pack, qint64 received, qint64 total) {
                    progress_->show();
                    progress_->setRange(0, 100);
                    progress_->setValue(total > 0 ? static_cast<int>(received * 100 / total) : 0);
                    status_->setText(tr("Downloading %1: %2 of %3")
                                         .arg(pack, QLocale().formattedDataSize(received),
                                              QLocale().formattedDataSize(total)));
                    publishAcquisition();
                });
        connect(provisioner_, &NativeComponentProvisioner::finished, this,
                [this](bool ok, bool cancelled, const QString& summary) {
                    if (progress_ != nullptr) progress_->hide();
                    if (cancelled) {
                        pendingModels_.clear();
                        finishAcquisitionUnit(QString(), true);
                    } else if (!ok) {
                        pendingModels_.clear();
                        finishAcquisitionUnit(summary, false,
                                              QStringLiteral("native.components.fetch"));
                    } else {
                        pendingModels_.clear();
                        finishAcquisitionUnit(QString(), false);
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
    updateCardStates();
    updateControls();
    scheduleInventoryRefresh();
}

void ComponentStoreTab::scheduleInventoryRefresh() {
    if (runner_ == nullptr || inventoryScanning_ || downloadActive() || verifying_) return;
    inventoryScanning_ = true;
    const bool acquisitionInventory = componentStagePending_;
    if (acquisitionInventory) {
        acquisitionSequenceActive_ = true;
        updateControls();
    }
    const quint64 generation = ++inventoryGeneration_;
    const auto cancelled = std::make_shared<std::atomic<bool>>(false);
    inventoryCancelFlag_ = cancelled;
    const GpuComputeCapability gpu = observeComputeCapability();
    const std::shared_ptr<std::atomic<bool>> live = alive_;
    runner_->runTransfer(
        [gpu, cancelled] {
            return inventorySnapshot(gpu, false, false, cancelled.get());
        },
        [this, live, generation, cancelled, acquisitionInventory](const QVariant& value) {
            if (!live->load() || generation != inventoryGeneration_) return;
            inventoryScanning_ = false;
            inventoryCancelFlag_.reset();
            if (!cancelled->load()) applyInventorySnapshot(value);
            if (recommendedInstallPending_) finishRecommendedInstall();
            if (componentStagePending_) {
                componentStagePending_ = false;
                downloadRemainingComponents();
            } else if (acquisitionInventory) {
                acquisitionSequenceActive_ = false;
                updateControls();
            }
        },
        [this, live, generation, acquisitionInventory](const backend::Failure& failure) {
            if (!live->load() || generation != inventoryGeneration_) return;
            inventoryScanning_ = false;
            inventoryCancelFlag_.reset();
            if (acquisitionInventory) {
                componentStagePending_ = false;
                acquisitionSequenceActive_ = false;
            }
            const QString cause = backend::failureLine(failure);
            applyStatus(QStringLiteral("native.components.fetch"),
                        StatusReason{.kind = StatusReason::Kind::InventoryFailed,
                                     .detail = cause});
            updateControls();
            emit inventoryUpdated();
        });
}

void ComponentStoreTab::applyInventorySnapshot(const QVariant& value) {
    const QVariantMap result = value.toMap();
    storedSpatial_.clear();
    for (const QVariant& item : result.value(QStringLiteral("spatial")).toList()) {
        const QVariantMap row = item.toMap();
        StoredSpatialAsset stored;
        stored.asset.id = row.value(QStringLiteral("id")).toString();
        stored.graphPath = row.value(QStringLiteral("path")).toString();
        stored.verified = row.value(QStringLiteral("verified")).toBool();
        stored.reasonKey = row.value(QStringLiteral("reasonKey")).toString();
        stored.reason = row.value(QStringLiteral("reason")).toString();
        if (const SpatialAsset* asset = findSpatialAsset(stored.asset.id)) stored.asset = *asset;
        storedSpatial_.append(std::move(stored));
    }
    animeJanaiTensorRtInstalled_ = result.value(QStringLiteral("trtInstalled")).toBool();
    animeJanaiDirectMlInstalled_ = result.value(QStringLiteral("directMlInstalled")).toBool();
    componentDirsAvailable_ = result.value(QStringLiteral("componentDirsAvailable")).toBool();
    rifeInstalled_ = result.value(QStringLiteral("rifeInstalled")).toBool();
    rifeVerified_ = result.value(QStringLiteral("rifeVerified")).toBool();
    tensorRtClosureBytes_ = result.value(QStringLiteral("tensorRtClosureBytes")).toLongLong();
    directMlClosureBytes_ = result.value(QStringLiteral("directMlClosureBytes")).toLongLong();
    runtimeRecommended_ = result.value(QStringLiteral("runtimeRecommended")).toBool();
    kernelsRecommended_ = result.value(QStringLiteral("kernelsRecommended")).toBool();
    spatialRecommended_ = false;
    for (const SpatialAsset& asset : spatialAssetCatalog()) {
        const StoredSpatialAsset* row = spatialInventoryRow(storedSpatial_, asset.id);
        if (row == nullptr || !row->verified) {
            spatialRecommended_ = true;
            break;
        }
    }
    rifeVerificationErrors_.clear();
    for (const QVariant& item : result.value(QStringLiteral("rifeFailures")).toList()) {
        const QVariantMap failure = item.toMap();
        rifeVerificationErrors_.insert(failure.value(QStringLiteral("file")).toString(),
                                       failure.value(QStringLiteral("reason")).toString());
    }
    inventoryReady_ = true;
    updateCardStates();
    updateControls();
    emit inventoryUpdated();
}

mplayer::native::Backend ComponentStoreTab::selectedBackend() const {
    if (animeJanaiBackendCombo_ == nullptr) return Backend::TensorRT;
    return static_cast<Backend>(animeJanaiBackendCombo_->currentData().toInt());
}

void ComponentStoreTab::renderCards() {
    updateCardStates();
}

void ComponentStoreTab::updateCardStates() {
    const GpuComputeCapability& gpu = observeComputeCapability();
    const ComponentPack* runtimePack = tensorRtRuntimePack();
    const StoredPack runtimeStored = runtimePack != nullptr
                                         ? storedPack(componentStore(), *runtimePack)
                                         : StoredPack{};
    const bool runtimeInstalled = gpu.nvidia
                                      ? animeJanaiTensorRtInstalled_ &&
                                            (runtimeStored.installed || componentDirsAvailable_)
                                      : animeJanaiTensorRtInstalled_;
    const bool runtimeRecommended = gpu.nvidia && runtimeRecommended_;
    const bool rifeInstalled = rifeInstalled_;
    const QVector<SpatialAsset> spatialCatalog = spatialAssetCatalog();
    const QVector<StoredSpatialAsset>& spatialStored = storedSpatial_;
    int installedSpatial = 0;
    int verifiedSpatial = 0;
    qint64 spatialBytes = 0;
    for (const SpatialAsset& asset : spatialCatalog) {
        spatialBytes += asset.bytes;
        const StoredSpatialAsset* row = spatialInventoryRow(spatialStored, asset.id);
        if (row != nullptr) {
            if (QFileInfo(row->graphPath).isFile()) ++installedSpatial;
            if (row->verified) ++verifiedSpatial;
        }
    }
    const bool spatialRecommended = spatialRecommended_;

    const ComponentPack* kernelsPack =
        tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
    const StoredPack kernelsStored = kernelsPack != nullptr
                                         ? storedPack(componentStore(), *kernelsPack)
                                         : StoredPack{};
    const bool kernelsInstalled = kernelsPack == nullptr || kernelsStored.installed ||
                                  componentDirsAvailable_;
    const bool kernelsRecommended = gpu.nvidia && kernelsPack != nullptr &&
                                    kernelsRecommended_;

    auto setState = [](QLabel* label, const QString& state) {
        if (label != nullptr) label->setText(state);
    };
    const qint64 runtimeBytes = runtimePack != nullptr ? runtimePack->assetBytes : 0;
    findChild<QLabel*>(QStringLiteral("tensorRtRuntimeCardSize"))
        ->setText(tr("%1 MB").arg(QLocale().toString(runtimeBytes / 1048576)));
    findChild<QLabel*>(QStringLiteral("rifeModelsCardSize"))
        ->setText(tr("%1 MB").arg(QLocale().toString(rifeArchivePack().assetBytes / 1048576)));
    const qint64 kernelsBytes = kernelsPack != nullptr ? kernelsPack->assetBytes : 0;
    findChild<QLabel*>(QStringLiteral("tensorRtKernelsCardSize"))
        ->setText(tr("%1 MB").arg(QLocale().toString(kernelsBytes / 1048576)));

    findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardSize"))
        ->setText(QLocale().formattedDataSize(spatialBytes));
    setState(spatialState_,
             tr("%1/%2 installed; %3/%2 verified")
                 .arg(installedSpatial).arg(spatialCatalog.size()).arg(verifiedSpatial));

    setState(runtimeState_,
             runtimeRecommended ? tr("recommended for this PC")
                                : (runtimeInstalled ? tr("installed") : tr("optional")));
    setState(rifeState_, rifeInstalled ? tr("OK") : tr("optional"));
    setState(kernelsState_,
             kernelsRecommended ? tr("recommended for this PC")
                                : (kernelsInstalled ? tr("installed") : tr("optional")));
    auto setBadgeRole = [](QLabel* badge, bool recommended, bool installed) {
        badge->setAutoFillBackground(false);
        badge->setStyleSheet(QString());
        badge->setProperty("textRole", recommended ? QStringLiteral("recommendation")
                                                   : (installed ? QString()
                                                                : QStringLiteral("muted")));
        badge->setProperty("badgeRole", recommended ? QString()
                                                    : (installed ? QStringLiteral("installed")
                                                                 : QStringLiteral("optional")));
        badge->style()->unpolish(badge);
        badge->style()->polish(badge);
    };
    setBadgeRole(runtimeState_, runtimeRecommended, runtimeInstalled);
    setBadgeRole(rifeState_, false, rifeInstalled);
    setBadgeRole(spatialState_, spatialRecommended,
                 !spatialCatalog.isEmpty() && verifiedSpatial == spatialCatalog.size());
    setBadgeRole(kernelsState_, kernelsRecommended, kernelsInstalled);

    runtimeCheck_->setAccessibleName(tr("TensorRT runtime"));
    rifeCheck_->setAccessibleName(tr("RIFE interpolation models"));
    spatialCheck_->setAccessibleName(tr("AnimeJaNai upscaling models"));
    spatialCheck_->setAccessibleDescription(
        tr("Select all five pinned upscaling graphs for download or repair."));
    kernelsCheck_->setAccessibleName(tr("TensorRT kernels for this GPU"));
    kernelsCheck_->setVisible(gpu.nvidia && kernelsPack != nullptr);
    if (!componentSelectionsInitialized_ && inventoryReady_) {
        componentSelectionsInitialized_ = true;
        if (runtimeRecommended) selectedPacks_.insert(QStringLiteral("component:runtime"));
        if (kernelsRecommended) selectedPacks_.insert(QStringLiteral("component:kernels"));
        if (!rifeInstalled) selectedPacks_.insert(QStringLiteral("rife:archive"));
        if (spatialRecommended) selectedPacks_.insert(QStringLiteral("component:spatial"));
    }
    auto showSelection = [this](QCheckBox* box, const QString& pack) {
        const bool checked = selectedPacks_.contains(pack);
        box->blockSignals(true);
        box->setChecked(checked);
        box->blockSignals(false);
        if (QWidget* card = box->parentWidget()) {
            card->setProperty("selected", checked && box->isVisibleTo(card));
        }
    };
    showSelection(runtimeCheck_, QStringLiteral("component:runtime"));
    showSelection(rifeCheck_, QStringLiteral("rife:archive"));
    showSelection(spatialCheck_, QStringLiteral("component:spatial"));
    showSelection(kernelsCheck_, QStringLiteral("component:kernels"));
    const bool showBanner = runtimeRecommended || kernelsRecommended || spatialRecommended;
    recommendationBanner_->setVisible(showBanner);
    auto restyleCard = [](QCheckBox* box, bool recommended) {
        if (QWidget* card = box != nullptr ? box->parentWidget() : nullptr) {
            card->setProperty("recommended", recommended);
            card->style()->unpolish(card);
            card->style()->polish(card);
        }
    };
    restyleCard(runtimeCheck_, runtimeRecommended);
    restyleCard(rifeCheck_, false);
    restyleCard(spatialCheck_, spatialRecommended);
    restyleCard(kernelsCheck_, kernelsRecommended);
}

/// The immutable input the off-GUI verification needs, copied here on the GUI
/// thread. Pointers into the pinned catalogues are stable for the process
/// lifetime; the stores, the spatial rows and the catalogue are values, so
/// nothing captured here can be mutated while the worker hashes.
ComponentStoreTab::VerifyRequest ComponentStoreTab::captureVerifyRequest() const {
    VerifyRequest request;
    request.store = componentStore();
    request.rifeStore = rifeArchiveStore();
    const GpuComputeCapability& gpu = observeComputeCapability();
    request.packs = compatiblePacksFor(gpu.smMajor, gpu.smMinor);
    request.rifePins = rifeModelPins();
    request.spatialCatalog = spatialAssetCatalog();
    request.backend = selectedBackend();
    return request;
}

void ComponentStoreTab::verifyAll() {
    // One run at a time. A second click while a run is hashing is refused by
    // the disabled control, and refused again here so no re-entrant path
    // (a keyboard activation, a shortcut, a test) can start a parallel run.
    if (verifying_) return;
    if (runner_ == nullptr) {
        applyStatus(QStringLiteral("native.components.noRunner"),
                    StatusReason{.kind = StatusReason::Kind::VerifyNeedsRunner});
        updateControls();
        return;
    }
    startVerify(captureVerifyRequest());
}

void ComponentStoreTab::applyProvisionProgress(
    const ComponentProvisionProgress& progress) {
    progress_->show();
    progress_->setRange(0, 100);
    progress_->setValue(progress.total > 0
                            ? static_cast<int>(progress.received * 100 / progress.total)
                            : 0);
    status_->setText(tr("Installing %1: %2 of %3")
                         .arg(progress.pack,
                              QLocale().formattedDataSize(progress.received),
                              QLocale().formattedDataSize(progress.total)));
    publishAcquisition();
}

void ComponentStoreTab::applyDownloadProgress(const ComponentProvisionProgress& progress) {
    progress_->show();
    progress_->setRange(0, 100);
    progress_->setValue(progress.total > 0
                            ? static_cast<int>(progress.received * 100 / progress.total)
                            : 0);
    status_->setText(tr("Downloading %1: %2 of %3")
                         .arg(progress.pack,
                              QLocale().formattedDataSize(progress.received),
                              QLocale().formattedDataSize(progress.total)));
    publishAcquisition();
}
void ComponentStoreTab::applyVerifyResult(int verified, int failed, int failedRife,
                                          int failedSpatial, const QStringList& failures,
                                          const QSet<QString>& provedPacks,
                                          const QSet<QString>& provedRifeGraphs,
                                          const std::optional<int>& provedBackend) {
    verifying_ = false;
    // "Verified" is claimed only from what the hashing actually proved: every
    // marker the previous run earned is cleared first, so a missing or corrupt
    // element can never keep a stale verified badge.
    verifiedPacks_ = provedPacks;
    verifiedRifeGraphs_ = provedRifeGraphs;
    verifiedAnimeJanaiBackends_.clear();
    if (provedBackend.has_value()) verifiedAnimeJanaiBackends_.insert(*provedBackend);
    // The cards are re-read on the GUI thread rather than trusting the worker's
    // copy: a verification that ran while a pack was still arriving must not
    // render a state that is already gone.
    renderCards();
    if (progress_ != nullptr) progress_->hide();
    if (failed == 0) {
        applyStatus(QString(),
                    StatusReason{.kind = StatusReason::Kind::VerifiedCount, .count = verified});
    } else {
        // The worker's raw per-element causes are copied, never referenced, and
        // only the app-owned framing around them is re-rendered later.
        applyStatus(QStringLiteral("native.components.digest"),
                    StatusReason{.kind = StatusReason::Kind::VerifyDigest,
                                 .causes = failures,
                                 .count = failed,
                                 .rifeFailures = failedRife,
                                 .spatialFailures = failedSpatial});
    }
    updateControls();
}

void ComponentStoreTab::startVerify(const VerifyRequest& request) {
    verifying_ = true;
    ++runGeneration_;
    const quint64 generation = runGeneration_;
    // The progress bar reports an unbounded activity rather than a percentage
    // nobody measured, so the GUI stays honest while the worker hashes.
    progress_->show();
    progress_->setRange(0, 0);
    status_->setText(tr("Verifying the installed components..."));
    updateControls();

    runner_->runTransfer(
        [request, alive = alive_]() -> QVariant {
            const ComponentStore store = request.store;
            const RifeArchiveStore rifeStore = request.rifeStore;
            const Backend backend = request.backend;
            int verified = 0;
            int failed = 0;
            QStringList failures;
            QSet<QString> provedPacks;
            QSet<QString> provedRifeGraphs;
            std::optional<int> provedBackend;
            for (const ComponentPack* pack : request.packs) {
                if (!alive->load()) return QVariant{};
                const StoredPack result = verifyPack(store, *pack);
                if (result.installed) {
                    ++verified;
                    provedPacks.insert(pack->name);
                    continue;
                }
                if (storedPack(store, *pack).installed) {
                    ++failed;
                    failures.append(QStringLiteral("%1: %2").arg(pack->name, result.reason));
                }
            }
            if (!alive->load()) return QVariant{};
            const AnimeJanaiInstallation installation =
                describeAnimeJanaiInstallation(backend, true);
            if (installation.reasonKey == QStringLiteral("animejanai.installation.recordMissing")) {
                // Nothing recorded is an honest absence, never a failure.
            } else if (installation.installed) {
                ++verified;
                provedBackend = static_cast<int>(backend);
            } else {
                ++failed;
                // Labels travel as stable identifiers; the GUI thread words
                // them through the same tr() literals the synchronous path
                // used, so the translation context never changes.
                failures.append(QStringLiteral("%1: %2").arg(
                    backend == Backend::TensorRT ? QStringLiteral("tensorrt")
                                                 : QStringLiteral("directml"),
                    installation.reason.isEmpty()
                        ? QStringLiteral("animejanai.installation.incomplete")
                        : installation.reason));
            }
            int failedRifeGraphs = 0;
            for (const RifeModelPin& pin : request.rifePins) {
                if (!alive->load()) return QVariant{};
                const StoredRifeGraph stored = storedRifeGraph(rifeStore, pin);
                if (!stored.installed) continue;
                const StoredRifeGraph result = verifyRifeGraph(rifeStore, pin);
                if (result.installed) {
                    ++verified;
                    provedRifeGraphs.insert(pin.fileName);
                } else {
                    ++failed;
                    ++failedRifeGraphs;
                    failures.append(QStringLiteral("%1: %2").arg(pin.fileName, result.reason));
                }
            }
            int failedSpatialModels = 0;
            const QVector<StoredSpatialAsset> storedSpatial = storedSpatialAssets();
            for (const SpatialAsset& asset : request.spatialCatalog) {
                if (!alive->load()) return QVariant{};
                const StoredSpatialAsset* model =
                    spatialInventoryRow(storedSpatial, asset.id);
                if (model != nullptr && model->verified) {
                    ++verified;
                } else {
                    ++failed;
                    ++failedSpatialModels;
                    failures.append(QStringLiteral("%1: %2").arg(
                        asset.name,
                        model == nullptr ? QStringLiteral("not installed") : model->reason));
                }
            }
            return QVariantMap{{QStringLiteral("verified"), verified},
                               {QStringLiteral("failed"), failed},
                               {QStringLiteral("rife"), failedRifeGraphs},
                               {QStringLiteral("spatial"), failedSpatialModels},
                               {QStringLiteral("failures"), failures},
                               {QStringLiteral("packs"), QStringList(provedPacks.cbegin(),
                                                                    provedPacks.cend())},
                               {QStringLiteral("rifeGraphs"),
                                QStringList(provedRifeGraphs.cbegin(), provedRifeGraphs.cend())},
                               {QStringLiteral("backend"),
                                static_cast<qulonglong>(provedBackend.value_or(-1))}};
        },
        [this, alive = alive_, generation](const QVariant& value) {
            if (!alive->load() || runGeneration_ != generation) return;
            const QVariantMap map = value.toMap();
            const qint64 backendKey = map.value(QStringLiteral("backend")).toLongLong();
            std::optional<int> provedBackend;
            if (backendKey >= 0) provedBackend = static_cast<int>(backendKey);
            // The proven markers travel as name lists; QSet's range constructor
            // keeps them exactly as the hashing proved them.
            const QStringList packs = map.value(QStringLiteral("packs")).toStringList();
            const QStringList rifeGraphs =
                map.value(QStringLiteral("rifeGraphs")).toStringList();
            applyVerifyResult(map.value(QStringLiteral("verified")).toInt(),
                              map.value(QStringLiteral("failed")).toInt(),
                              map.value(QStringLiteral("rife")).toInt(),
                              map.value(QStringLiteral("spatial")).toInt(),
                              map.value(QStringLiteral("failures")).toStringList(),
                              QSet<QString>(packs.cbegin(), packs.cend()),
                              QSet<QString>(rifeGraphs.cbegin(), rifeGraphs.cend()),
                              provedBackend);
        },
        [this, alive = alive_, generation](const backend::Failure& failure) {
            if (!alive->load() || runGeneration_ != generation) return;
            verifying_ = false;
            ++runGeneration_;
            if (progress_ != nullptr) progress_->hide();
            const QString cause = backend::failureLine(failure);
            applyStatus(QStringLiteral("native.components.fetch"),
                        StatusReason{.kind = StatusReason::Kind::NotVerified,
                                     .detail = cause});
            updateControls();
        });
}


}  // namespace mikai::app
