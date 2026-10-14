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
#include <QtWidgets/QVBoxLayout>

#include <stdexcept>
#include <optional>
#include <functional>

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

/// One off-GUI run's GUI publisher.
///
/// `post` is the Runner's dispatch binder, taken on the GUI thread before the
/// work starts. It holds only shared state, never a widget, so copying it onto a
/// pool thread and calling it there is safe by construction, and invoking it
/// after the Runner died or was cancelled is a no-op rather than a dangling
/// receive. The posted body is where the target is resolved at all: it checks
/// the tab's own liveness flag first, then creates a QPointer IN the body, on
/// the GUI thread, and checks it before any widget is touched, and finally the
/// run's generation decides whether this delivery still answers the request the
/// user is waiting on. A QPointer is never copied off-thread, and a destructed
/// owner is never dereferenced.
struct ProgressPublisher {
    Runner::GuiDispatch post;
    ComponentStoreTab* owner = nullptr;
    std::shared_ptr<std::atomic<bool>> alive;
    quint64 generation = 0;

    void operator()(const std::function<void(ComponentStoreTab&)>& apply) const {
        if (!post) return;
        const std::shared_ptr<std::atomic<bool>> live = alive;
        if (live == nullptr || !live->load()) return;
        ComponentStoreTab* const ownerAddress = owner;
        const quint64 runGeneration = generation;
        post([ownerAddress, live, runGeneration, apply]() {
            if (live == nullptr || !live->load()) return;
            QPointer<ComponentStoreTab> guard(ownerAddress);
            if (guard.isNull() || guard->runGeneration() != runGeneration) return;
            apply(*guard);
        });
    }
};

}  // namespace

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
    auto* primaryActions = new QHBoxLayout();
    primaryActions->setSpacing(theme::kSpacingSmall);
    downloadButton_ = new QPushButton(actionsCard);
    downloadButton_->setObjectName(QStringLiteral("primary"));
    downloadButton_->setMinimumHeight(theme::kSettingsControlSize);
    primaryActions->addWidget(downloadButton_);
    auto* refreshButton = new QPushButton(actionsCard);
    refreshButton->setObjectName(QStringLiteral("refreshComponentsButton"));
    refreshButton->setMinimumHeight(theme::kSettingsControlSize);
    primaryActions->addWidget(refreshButton);
    primaryActions->addStretch();
    actions->addLayout(primaryActions);
    auto* utilityActions = new QHBoxLayout();
    utilityActions->setSpacing(theme::kSpacingSmall);
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
    utilityActions->addStretch();
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
            applyStatus(QStringLiteral("native.components.fetch"),
                        tr("Component inventory failed: %1")
                            .arg(backend::failureLine(failure)));
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
                    tr("Verifying the installed components needs a runner."));
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
        applyStatus(QString(), tr("%1 elements verified, none changed.").arg(verified));
    } else {
        QStringList reported;
        for (const QString& entry : failures) {
            const int separator = entry.indexOf(QLatin1Char(':'));
            const QString label = entry.left(separator);
            const QString detail = entry.mid(separator + 1);
            if (label == QLatin1String("tensorrt"))
                reported.append(tr("TensorRT / CUDA 13 closure: %1").arg(
                    detail == QLatin1String("animejanai.installation.incomplete")
                        ? tr("one or more required dependencies are missing or corrupt")
                        : detail));
            else if (label == QLatin1String("directml"))
                reported.append(tr("DirectML closure: %1").arg(
                    detail == QLatin1String("animejanai.installation.incomplete")
                        ? tr("one or more required dependencies are missing or corrupt")
                        : detail));
            else
                reported.append(entry);
        }
        if (failedRife > 0)
            reported.append(tr("%1 RIFE interpolation model(s) failed verification.")
                                .arg(failedRife));
        if (failedSpatial > 0)
            reported.append(tr("%1 spatial model(s) failed verification.")
                                .arg(failedSpatial));
        applyStatus(QStringLiteral("native.components.digest"),
                    tr("%1 elements failed verification: %2")
                        .arg(failed)
                        .arg(reported.join(QStringLiteral("; "))));
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
            applyStatus(QStringLiteral("native.components.fetch"),
                        tr("The components could not be verified: %1")
                            .arg(backend::failureLine(failure)));
            updateControls();
        });
}

void ComponentStoreTab::downloadMissing() {
    downloadMissing(true);
}

void ComponentStoreTab::installRecommended() {
    if (acquisitionChecking_ || downloadActive() || verifying_) return;
    recommendedInstallPending_ = true;
    const GpuComputeCapability& gpu = observeComputeCapability();
#ifdef Q_OS_WIN
    const Backend preferredBackend = gpu.nvidia ? Backend::TensorRT : Backend::DirectML;
#else
    Q_UNUSED(gpu);
    const Backend preferredBackend = Backend::TensorRT;
#endif
    const int backendIndex = animeJanaiBackendCombo_->findData(
        static_cast<int>(preferredBackend));
    if (backendIndex >= 0) animeJanaiBackendCombo_->setCurrentIndex(backendIndex);
    if (inventoryReady_) finishRecommendedInstall();
    else scheduleInventoryRefresh();
}

void ComponentStoreTab::finishRecommendedInstall() {
    if (!recommendedInstallPending_ || !inventoryReady_) return;
    recommendedInstallPending_ = false;
    refresh();
    runtimeCheck_->setChecked(
        selectedBackend() == Backend::TensorRT ? runtimeRecommended_ :
                                                   !animeJanaiDirectMlInstalled_);
    kernelsCheck_->setChecked(kernelsRecommended_);
    spatialCheck_->setChecked(spatialRecommended_);
    rifeCheck_->setChecked(!rifeInstalled_ || !rifeVerified_);
    downloadMissing(false);
}

void ComponentStoreTab::downloadMissing(bool showReview) {
    if (acquisitionChecking_ || acquisitionSequenceActive_ || downloadActive() || verifying_) return;
    if (runner_ == nullptr) {
        applyStatus(QStringLiteral("native.components.noClient"),
                    tr("Checking selected component state needs a runner."));
        return;
    }
    if (inventoryScanning_) {
        if (inventoryCancelFlag_) inventoryCancelFlag_->store(true);
        ++inventoryGeneration_;
        inventoryScanning_ = false;
    }
    acquisitionChecking_ = true;
    acquisitionCheckCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelled = acquisitionCheckCancelFlag_;
    const quint64 generation = ++runGeneration_;
    const Backend backend = selectedBackend();
    const GpuComputeCapability gpu = observeComputeCapability();
    const QSet<QString> selected = selectedPacks_;
    progress_->show();
    progress_->setRange(0, 0);
    status_->setText(tr("Checking selected components..."));
    updateControls();
    acquisitionSequenceActive_ = true;
    updateControls();
    const std::shared_ptr<std::atomic<bool>> live = alive_;
    runner_->runTransfer(
        [backend, gpu, cancelled] {
            return inventorySnapshot(gpu, true, false, cancelled.get());
        },
        [this, live, generation, cancelled, backend, selected, showReview,
         gpu](const QVariant& value) {
            if (!live->load() || generation != runGeneration_) return;
            if (cancelled->load()) {
                acquisitionChecking_ = false;
                acquisitionSequenceActive_ = false;
                acquisitionCheckCancelFlag_.reset();
                pendingModels_.clear();
                applyStatus(QStringLiteral("native.components.cancelled"), QString());
                updateControls();
                return;
            }
            applyInventorySnapshot(value);
            const QVariantMap inventory = value.toMap();
            const QVector<SpatialAsset> catalog = spatialAssetCatalog();
            pendingModels_.clear();
            if (selected.contains(QStringLiteral("component:spatial"))) {
                for (int index = 0; index < catalog.size(); ++index) {
                    const StoredSpatialAsset* row = spatialInventoryRow(storedSpatial_, catalog.at(index).id);
                    if (row == nullptr || !row->verified) pendingModels_.append(index);
                }
            }
            const bool runtimeSelected = selected.contains(QStringLiteral("component:runtime"));
            const bool kernelsSelected = selected.contains(QStringLiteral("component:kernels"));
            const bool componentMissing = backend == Backend::TensorRT &&
                (runtimeSelected || kernelsSelected) && gpu.smMajor > 0 &&
                !inventory.value(QStringLiteral("componentDirsAvailable")).toBool();
            const bool backendWork = (runtimeSelected &&
                !inventory.value(backend == Backend::TensorRT ? QStringLiteral("trtInstalled")
                                                               : QStringLiteral("directMlInstalled")).toBool()) ||
                componentMissing;
            const bool selectedRifeMissing = selected.contains(QStringLiteral("rife:archive")) &&
                !inventory.value(QStringLiteral("rifeVerified")).toBool();
            QStringList review;
            if (backendWork) {
                review.append(backend == Backend::TensorRT
                                  ? tr("AnimeJaNai TensorRT / CUDA backend closure")
                                  : tr("AnimeJaNai DirectML backend closure"));
            }
            for (int index : pendingModels_) review.append(catalog.at(index).name);
            if (selectedRifeMissing)
                review.append(tr("RIFE 3.7.0 archive pack (all 47 graphs)"));
            if (review.isEmpty()) {
                acquisitionChecking_ = false;
                acquisitionSequenceActive_ = false;
                acquisitionCheckCancelFlag_.reset();
                if (progress_ != nullptr) progress_->hide();
                applyStatus(QString(), tr("No selected missing packs need installation."));
                updateControls();
                return;
            }
            qint64 bytes = 0;
            for (int index : pendingModels_) bytes += catalog.at(index).bytes;
            if (selectedRifeMissing) bytes += rifeArchivePack().assetBytes;
            acquisitionChecking_ = false;
            acquisitionCheckCancelFlag_.reset();
            if (progress_ != nullptr) progress_->hide();
            updateControls();
            if (showReview) {
                const QString prompt = tr("The following selected packs are missing or need repair:\n%1\n\nEstimated download size: %2\n\nContinue?")
                    .arg(review.join(QStringLiteral("\n")),
                         backendWork ? tr("Unknown") : QLocale().formattedDataSize(bytes));
                if (QMessageBox::question(this, tr("Review installation"), prompt,
                                          QMessageBox::Yes | QMessageBox::Cancel,
                                          QMessageBox::Cancel) != QMessageBox::Yes) {
                    pendingModels_.clear();
                    acquisitionSequenceActive_ = false;
                    // The transient "Checking selected components..." label was
                    // set before the check ran; a cancelled review must clear it
                    // so the surface reads idle, not stuck mid-check.
                    applyStatus(QString(), QString());
                    updateControls();
                    return;
                }
            }
            modelCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
            acquisitionSequenceActive_ = true;
            acquisitionGeneration_ = runGeneration_;
            acquisitionCancelled_ = false;
            acquisitionFailures_.clear();
            acquisitionFailureKey_.clear();
            pendingBackendWork_ = backendWork;
            pendingRuntimeClosure_ =
                runtimeSelected &&
                !inventory.value(backend == Backend::TensorRT ? QStringLiteral("trtInstalled")
                                                              : QStringLiteral("directMlInstalled"))
                     .toBool();
            pendingRifeArchive_ = selectedRifeMissing;
            downloadRemainingComponents();
        },
        [this, live, generation](const backend::Failure& failure) {
            if (!live->load() || generation != runGeneration_) return;
            acquisitionChecking_ = false;
            acquisitionSequenceActive_ = false;
            acquisitionCheckCancelFlag_.reset();
            pendingModels_.clear();
            applyStatus(QStringLiteral("native.components.fetch"),
                        tr("Component check failed: %1").arg(backend::failureLine(failure)));
            updateControls();
        });
}

void ComponentStoreTab::downloadAnimeJanaiRuntime() {
    if (runner_ == nullptr) {
        const QString backend = selectedBackend() == Backend::TensorRT
                                    ? tr("TensorRT / CUDA 13")
                                    : tr("DirectML");
        applyStatus(QStringLiteral("native.components.noRunner"),
                    tr("Installing the selected AnimeJaNai %1 closure needs a runner.")
                        .arg(backend));
        pendingModels_.clear();
        updateControls();
        return;
    }

    animeJanaiRuntimeDownloading_ = true;
    animeJanaiCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelFlag = animeJanaiCancelFlag_;
    const Backend backend = selectedBackend();
    const quint64 generation = acquisitionGeneration_ == 0 ? ++runGeneration_
                                                           : acquisitionGeneration_;
    progress_->show();
    progress_->setRange(0, 100);
    progress_->setValue(0);
    status_->setText(tr("Installing AnimeJaNai runtime and selected backend dependencies..."));
    updateControls();

    // A plain owner address and a plain liveness flag are the only things the
    // worker copies. Every publication goes through the publisher, which creates
    // its QPointer on the GUI thread, checks it before any widget is touched,
    // and re-checks the run's generation so a late delivery is discarded.
    // The binder is taken on the GUI thread, before the work starts; the
    // worker only ever calls it.
    ProgressPublisher publish{runner_->post(), this, alive_, generation};
    runner_->runTransfer(
        [backend, cancelFlag, publish]() -> QVariant {
            const ComponentProvisionResult result = provisionAnimeJanaiRuntime(
                backend,
                [publish](const ComponentProvisionProgress& progress) {
                    const ComponentProvisionProgress at = progress;
                    publish([at](ComponentStoreTab& tab) {
                        tab.applyProvisionProgress(at);
                    });
                },
                [cancelFlag] { return cancelFlag->load(); });
            return QVariantMap{{QStringLiteral("ok"), result.ok},
                               {QStringLiteral("cancelled"), result.cancelled},
                               {QStringLiteral("reason"), result.reason},
                               {QStringLiteral("reasonKey"), result.reasonKey}};
        },
        [this, alive = alive_, generation, cancelFlag, backend](const QVariant& value) {
            if (!alive->load() || runGeneration_ != generation) return;
            animeJanaiRuntimeDownloading_ = false;
            animeJanaiCancelFlag_.reset();
            if (cancelFlag->load() || value.toMap().value(QStringLiteral("cancelled")).toBool()) {
                finishAcquisitionUnit(QString(), true);
            } else if (!value.toMap().value(QStringLiteral("ok")).toBool()) {
                const QString reason = value.toMap().value(QStringLiteral("reason")).toString();
                const QString reasonKey =
                    value.toMap().value(QStringLiteral("reasonKey")).toString();
                const QString backendName = backend == Backend::TensorRT
                                                ? tr("TensorRT / CUDA 13")
                                                : tr("DirectML");
                finishAcquisitionUnit(
                    tr("%1 installation failed: %2")
                        .arg(backendName,
                             reason.isEmpty()
                                 ? tr("AnimeJaNai runtime installation failed.")
                                 : reason),
                    false, reasonKey);
            } else {
                verifiedAnimeJanaiBackends_.remove(static_cast<int>(backend));
                animeJanaiVerificationErrors_.remove(static_cast<int>(backend));
                finishAcquisitionUnit(QString(), false);
            }
        },
        [this, alive = alive_, generation, cancelFlag, backend](const backend::Failure& failure) {
            if (!alive->load() || runGeneration_ != generation) return;
            animeJanaiRuntimeDownloading_ = false;
            animeJanaiCancelFlag_.reset();
            if (cancelFlag->load()) {
                finishAcquisitionUnit(QString(), true);
                return;
            }
            const QString backendName =
                backend == Backend::TensorRT ? tr("TensorRT / CUDA 13") : tr("DirectML");
            finishAcquisitionUnit(tr("%1 installation failed: %2")
                                      .arg(backendName, backend::failureLine(failure)),
                                  false);
        });
}

void ComponentStoreTab::chooseAnimeJanaiRuntimeDirectory() {
    const QString sourceDirectory = QFileDialog::getExistingDirectory(
        this, tr("Select AnimeJaNai runtime directory"), QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (sourceDirectory.isEmpty()) return;
    importAnimeJanaiRuntime(sourceDirectory);
}

void ComponentStoreTab::importAnimeJanaiRuntime(const QString& sourceDirectory) {
    if (runner_ == nullptr) {
        applyStatus(QStringLiteral("native.components.noRunner"),
                    tr("Installing a local AnimeJaNai runtime needs a runner."));
        return;
    }

    animeJanaiRuntimeDownloading_ = true;
    animeJanaiCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelFlag = animeJanaiCancelFlag_;
    const Backend backend = selectedBackend();
    ++runGeneration_;
    const quint64 generation = runGeneration_;
    progress_->show();
    progress_->setRange(0, 100);
    progress_->setValue(0);
    status_->setText(tr("Installing local AnimeJaNai runtime and selected backend dependencies..."));
    updateControls();

    // The binder is taken on the GUI thread, before the work starts; the
    // worker only ever calls it.
    ProgressPublisher publish{runner_->post(), this, alive_, generation};
    runner_->runTransfer(
        [sourceDirectory, backend, cancelFlag, publish]() -> QVariant {
            const ComponentProvisionResult result = mplayer::native::importAnimeJanaiRuntime(
                sourceDirectory, backend,
                [publish](const ComponentProvisionProgress& progress) {
                    const ComponentProvisionProgress at = progress;
                    publish([at](ComponentStoreTab& tab) { tab.applyProvisionProgress(at); });
                },
                [cancelFlag] { return cancelFlag->load(); });
            return QVariantMap{{QStringLiteral("ok"), result.ok},
                               {QStringLiteral("cancelled"), result.cancelled},
                               {QStringLiteral("reason"), result.reason},
                               {QStringLiteral("reasonKey"), result.reasonKey}};
        },
        [this, alive = alive_, generation, cancelFlag, backend](const QVariant& value) {
            if (!alive->load() || runGeneration_ != generation) return;
            animeJanaiRuntimeDownloading_ = false;
            animeJanaiCancelFlag_.reset();
            if (cancelFlag->load() || value.toMap().value(QStringLiteral("cancelled")).toBool()) {
                applyStatus(QStringLiteral("native.components.cancelled"), QString());
            } else if (!value.toMap().value(QStringLiteral("ok")).toBool()) {
                const QString reason = value.toMap().value(QStringLiteral("reason")).toString();
                const QString reasonKey =
                    value.toMap().value(QStringLiteral("reasonKey")).toString();
                const QString backendName = backend == Backend::TensorRT
                                                ? tr("TensorRT / CUDA 13")
                                                : tr("DirectML");
                applyStatus(
                    reasonKey.isEmpty() ? QStringLiteral("native.components.fetch") : reasonKey,
                    tr("%1 installation failed: %2")
                        .arg(backendName,
                             reason.isEmpty()
                                 ? tr("Local AnimeJaNai runtime installation failed.")
                                 : reason));
            } else {
                verifiedAnimeJanaiBackends_.remove(static_cast<int>(backend));
                animeJanaiVerificationErrors_.remove(static_cast<int>(backend));
                applyStatus(QString(), tr("Local AnimeJaNai runtime installed."));
            }
            refresh();
        },
        [this, alive = alive_, generation, cancelFlag, backend](const backend::Failure& failure) {
            if (!alive->load() || runGeneration_ != generation) return;
            animeJanaiRuntimeDownloading_ = false;
            animeJanaiCancelFlag_.reset();
            if (cancelFlag->load()) {
                applyStatus(QStringLiteral("native.components.cancelled"), QString());
            } else {
                const QString backendName = backend == Backend::TensorRT
                                                ? tr("TensorRT / CUDA 13")
                                                : tr("DirectML");
                applyStatus(QStringLiteral("native.components.fetch"),
                            tr("%1 installation failed: %2")
                                .arg(backendName, backend::failureLine(failure)));
            }
            refresh();
        });
}

void ComponentStoreTab::downloadRemainingComponents() {
    if (!inventoryReady_) {
        componentStagePending_ = true;
        scheduleInventoryRefresh();
        return;
    }
    acquisitionUnitsPending_ = 0;
    acquisitionFailures_.clear();
    const bool componentMissing =
        selectedBackend() == Backend::TensorRT &&
        (selectedPacks_.contains(QStringLiteral("component:runtime")) ||
         selectedPacks_.contains(QStringLiteral("component:kernels"))) &&
        observeComputeCapability().smMajor > 0 && !componentDirsAvailable_;
    if (componentMissing && provisioner_ == nullptr) {
        pendingModels_.clear();
        acquisitionSequenceActive_ = false;
        applyStatus(QStringLiteral("native.components.missing"),
                    tr("The compatible TensorRT component pack is missing, but no component provisioner is available."));
        updateControls();
        return;
    }
    // The independent units run at the same time: the backend closure, the RIFE
    // archive and the spatial graphs share no scratch, no destination and no
    // record, so serialising them only made the whole acquisition as slow as the
    // sum of its parts. The run is over when the last unit settles, which is
    // what lets the window bar stay honest while several transfers run at once.
    //
    // The backend closure is a single unit, not two: the AnimeJaNai runtime
    // installer and the component provisioner both drive
    // `provisionTensorRtComponents`, whose process-wide gate allows one run, so
    // the runtime path owns the packs whenever it has to fetch them.
    // Every unit is counted before any starts: a unit that refuses synchronously
    // settles its own count immediately, and counting first keeps that refusal
    // from ending a run whose other units have not been handed their work yet.
    if (pendingBackendWork_) noteAcquisitionUnitStarted();
    if (pendingRifeArchive_) noteAcquisitionUnitStarted();
    if (!pendingModels_.isEmpty()) noteAcquisitionUnitStarted();
    if (pendingBackendWork_) {
        if (pendingRuntimeClosure_) {
            downloadAnimeJanaiRuntime();
        } else if (provisioner_ == nullptr) {
            finishAcquisitionUnit(
                tr("The compatible TensorRT component pack is missing, but no component "
                   "provisioner is available."),
                false);
        } else {
            QString reason;
            if (!provisioner_->ensureStore(&reason)) {
                finishAcquisitionUnit(reason, false);
            } else {
                progress_->show();
                progress_->setValue(0);
                updateControls();
            }
        }
    }
    if (pendingRifeArchive_) downloadRifeArchive();
    if (!pendingModels_.isEmpty()) downloadNextModel();
    // A unit that refused synchronously already settled itself (and finalized
    // when it brought the count to zero), so only an empty count that no
    // callback consumed can start the cycle here.
    if (acquisitionUnitsPending_ == 0 && acquisitionSequenceActive_)
        finalizeAcquisition();
}

void ComponentStoreTab::downloadRifeArchive() {
    if (!pendingRifeArchive_) {
        downloadNextModel();
        return;
    }
    const RifeArchiveStore store = rifeArchiveStore();
    const QVector<RifeModelPin> pins = rifeModelPins();
    if (runner_ == nullptr) {
        finishAcquisitionUnit(tr("The RIFE archive download needs a runner."), false,
                              QStringLiteral("native.components.noClient"));
        return;
    }

    rifeDownloading_ = true;
    rifeCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelFlag = rifeCancelFlag_;
    const quint64 generation = acquisitionGeneration_ == 0 ? ++runGeneration_
                                                           : acquisitionGeneration_;
    progress_->show();
    progress_->setRange(0, 100);
    progress_->setValue(0);
    status_->setText(tr("Downloading the RIFE 3.7.0 graph archive..."));
    updateControls();

    // The binder is taken on the GUI thread, before the work starts; the
    // worker only ever calls it.
    ProgressPublisher publish{runner_->post(), this, alive_, generation};
    runner_->runTransfer(
        [store, pins, cancelFlag, publish]() -> QVariant {
            const ComponentProvisionResult result = provisionRifeArchive(
                store, pins,
                [publish](const ComponentProvisionProgress& progress) {
                    const ComponentProvisionProgress at = progress;
                    publish([at](ComponentStoreTab& tab) {
                        tab.applyDownloadProgress(at);
                    });
                },
                [cancelFlag] { return cancelFlag->load(); });
            if (!result.ok) {
                throw std::runtime_error(result.reason.toStdString());
            }
            return QVariant{};
        },
        [this, alive = alive_, generation, cancelFlag, pins](const QVariant&) {
            if (!alive->load() || runGeneration_ != generation) return;
            rifeDownloading_ = false;
            rifeCancelFlag_.reset();
            for (const RifeModelPin& pin : pins) {
                verifiedRifeGraphs_.insert(pin.fileName);
                rifeVerificationErrors_.remove(pin.fileName);
            }
            pendingRifeArchive_ = false;
            finishAcquisitionUnit(QString(), false);
        },
        [this, alive = alive_, generation, cancelFlag](const backend::Failure& failure) {
            if (!alive->load() || runGeneration_ != generation) return;
            rifeDownloading_ = false;
            rifeCancelFlag_.reset();
            if (cancelFlag->load()) {
                finishAcquisitionUnit(QString(), true);
                return;
            }
            rifeFailure_ = backend::failureLine(failure);
            finishAcquisitionUnit(rifeFailure_, false);
        });
}

void ComponentStoreTab::downloadNextModel() {
    if (pendingModels_.isEmpty()) {
        finishModelDownloads();
        return;
    }
    if (runner_ == nullptr || client_ == nullptr) {
        modelsDownloading_ = false;
        modelCancelFlag_.reset();
        pendingModels_.clear();
        finishAcquisitionUnit(tr("A model download needs the backend client and a runner."),
                              false, QStringLiteral("native.components.noClient"));
        return;
    }
    const QVector<SpatialAsset> catalog = spatialAssetCatalog();
    if (!modelsDownloading_) {
        modelsDownloading_ = true;
        modelCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
        modelDownloadsInFlight_ = 0;
        modelDownloadsCompleted_ = 0;
        modelDownloadsTotal_ = pendingModels_.size();
        modelFailure_.clear();
        modelCancelled_ = false;
    }
    const quint64 generation = acquisitionGeneration_ == 0 ? ++runGeneration_
                                                           : acquisitionGeneration_;
    const auto cancelFlag = modelCancelFlag_;
    progress_->show();
    progress_->setRange(0, modelDownloadsTotal_);
    progress_->setValue(modelDownloadsCompleted_);
    updateControls();
    backend::MikaiClient* client = client_;
    const QVector<int> batch = std::exchange(pendingModels_, {});
    modelDownloadsInFlight_ += batch.size();
    for (int index : batch) {
        const SpatialAsset asset = catalog.at(index);
        status_->setText(tr("Downloading %1 (%2 of %3)...")
                             .arg(asset.name)
                             .arg(modelDownloadsCompleted_ + modelDownloadsInFlight_)
                             .arg(modelDownloadsTotal_));
        runner_->runTransfer(
            [client, asset, cancelFlag]() -> QVariant {
                if (cancelFlag->load()) return QVariant{};
                // The fetch's blocking wait observes a nested scope over the
                // tab's own gate, so Cancel pierces the transfer on the same
                // clock the runtime/RIFE downloads use instead of waiting for
                // the whole file or the artwork timeout.
                const backend::WorkCancellationScope tabCancel(cancelFlag.get());
                const QByteArray bytes =
                    client->fetchBytes(asset.url, kModelDownloadTimeoutMs);
                if (cancelFlag->load()) return QVariant{};
                const SpatialAssetPublication published =
                    publishSpatialAssetBytes(asset, bytes, QString());
                if (!published.ok) {
                    throw std::runtime_error(
                        QStringLiteral("%1: %2").arg(published.reasonKey, published.reason)
                            .toStdString());
                }
                return QVariant{};
            },
            [this, alive = alive_, generation, cancelFlag, asset](const QVariant&) {
                if (!alive->load() || runGeneration_ != generation) return;
                --modelDownloadsInFlight_;
                if (cancelFlag->load()) {
                    modelCancelled_ = true;
                } else {
                    ++modelDownloadsCompleted_;
                    status_->setText(tr("Installed %1 (%2 of %3)")
                                         .arg(asset.name)
                                         .arg(modelDownloadsCompleted_)
                                         .arg(modelDownloadsTotal_));
                    progress_->setValue(modelDownloadsCompleted_);
                }
                if (modelDownloadsInFlight_ == 0) finishModelDownloads();
                else publishAcquisition();
            },
            [this, alive = alive_, generation, cancelFlag](const backend::Failure& failure) {
                if (!alive->load() || runGeneration_ != generation) return;
                --modelDownloadsInFlight_;
                if (cancelFlag->load()) modelCancelled_ = true;
                else if (modelFailure_.isEmpty()) modelFailure_ = backend::failureLine(failure);
                if (modelDownloadsInFlight_ == 0) finishModelDownloads();
            });
    }
}

void ComponentStoreTab::finishModelDownloads() {
    if (modelDownloadsInFlight_ > 0) return;
    modelsDownloading_ = false;
    modelCancelFlag_.reset();
    if (!modelFailure_.isEmpty()) {
        pendingModels_.clear();
        finishAcquisitionUnit(modelFailure_, false);
        return;
    }
    if (modelCancelled_) {
        pendingModels_.clear();
        finishAcquisitionUnit(QString(), true);
        return;
    }
    pendingModels_.clear();
    finishAcquisitionUnit(QString(), false);
}

void ComponentStoreTab::startAcquisitionVerification() {
    if (runner_ == nullptr) {
        acquisitionSequenceActive_ = false;
        applyStatus(QStringLiteral("native.components.noRunner"),
                    tr("Verifying the installed components needs a runner."));
        refresh();
        return;
    }
    modelsDownloading_ = true;
    modelCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    const quint64 generation = ++runGeneration_;
    const auto cancelled = modelCancelFlag_;
    const GpuComputeCapability gpu = observeComputeCapability();
    progress_->show();
    progress_->setRange(0, 0);
    status_->setText(tr("Verifying installed components..."));
    updateControls();
    const std::shared_ptr<std::atomic<bool>> live = alive_;
    runner_->runTransfer(
        [gpu, cancelled] {
            return inventorySnapshot(gpu, true, true, cancelled.get());
        },
        [this, live, generation, cancelled, gpu](const QVariant& value) {
            if (!live->load() || generation != runGeneration_) return;
            const QVariantMap result = value.toMap();
            modelsDownloading_ = false;
            modelCancelFlag_.reset();
            acquisitionSequenceActive_ = false;
            if (cancelled->load() || result.value(QStringLiteral("cancelled")).toBool()) {
                applyStatus(QStringLiteral("native.components.cancelled"), QString());
                refresh();
                return;
            }
            applyInventorySnapshot(value);
            const bool runtimeSelected =
                selectedPacks_.contains(QStringLiteral("component:runtime"));
            const bool kernelsSelected =
                selectedPacks_.contains(QStringLiteral("component:kernels"));
            const bool rifeSelected = selectedPacks_.contains(QStringLiteral("rife:archive"));
            const bool spatialSelected =
                selectedPacks_.contains(QStringLiteral("component:spatial"));
            QStringList failures = acquisitionFailures_;
            const Backend backend = selectedBackend();
            if (runtimeSelected && !result.value(
                    backend == Backend::TensorRT ? QStringLiteral("trtInstalled")
                                                 : QStringLiteral("directMlInstalled")).toBool()) {
                const QString reason = result.value(
                    backend == Backend::TensorRT ? QStringLiteral("trtReason")
                                                 : QStringLiteral("directMlReason")).toString();
                failures.append(reason.isEmpty()
                                    ? tr("The requested backend installation could not be verified.")
                                    : reason);
            }
            if ((runtimeSelected || kernelsSelected) && backend == Backend::TensorRT &&
                gpu.smMajor > 0 &&
                !result.value(QStringLiteral("componentDirsAvailable")).toBool()) {
                failures.append(tr("The requested TensorRT component pack is not installed."));
            }
            if (rifeSelected && !result.value(QStringLiteral("rifeVerified")).toBool()) {
                for (const QVariant& entry : result.value(QStringLiteral("rifeFailures")).toList()) {
                    const QVariantMap failure = entry.toMap();
                    failures.append(QStringLiteral("%1: %2")
                                        .arg(failure.value(QStringLiteral("file")).toString(),
                                             failure.value(QStringLiteral("reason")).toString()));
                }
                if (failures.isEmpty()) failures.append(tr("The RIFE archive could not be verified."));
            }
            if (spatialSelected) {
                for (const SpatialAsset& asset : spatialAssetCatalog()) {
                    const StoredSpatialAsset* row = spatialInventoryRow(storedSpatial_, asset.id);
                    if (row == nullptr || !row->verified)
                        failures.append(row == nullptr
                                            ? tr("The requested upscaling model is not installed: %1")
                                                  .arg(asset.name)
                                            : QStringLiteral("%1: %2").arg(asset.name, row->reason));
                }
            }
            acquisitionFailures_.clear();
            if (failures.isEmpty()) {
                if (rifeSelected) rifeVerified_ = true;
                applyStatus(QString(), tr("Every selected pack is installed and verified."));
            } else {
                applyStatus(QStringLiteral("native.components.digest"),
                            failures.join(QStringLiteral("; ")));
            }
            refresh();
        },
        [this, live, generation, cancelled](const backend::Failure& failure) {
            if (!live->load() || generation != runGeneration_) return;
            modelsDownloading_ = false;
            modelCancelFlag_.reset();
            acquisitionSequenceActive_ = false;
            applyStatus(cancelled->load()
                            ? QStringLiteral("native.components.cancelled")
                            : QStringLiteral("native.components.digest"),
                        cancelled->load() ? QString() : backend::failureLine(failure));
            refresh();
        });
}

void ComponentStoreTab::finalizeAcquisition() {
    acquisitionSequenceActive_ = false;
    if (acquisitionCancelled_) {
        acquisitionFailures_.clear();
        applyStatus(QStringLiteral("native.components.cancelled"), QString());
        refresh();
        return;
    }
    if (!acquisitionFailures_.isEmpty()) {
        const QString key = acquisitionFailureKey_.isEmpty()
                                ? QStringLiteral("native.components.fetch")
                                : acquisitionFailureKey_;
        applyStatus(key, acquisitionFailures_.join(QStringLiteral("; ")));
        acquisitionFailures_.clear();
        acquisitionFailureKey_.clear();
        refresh();
        return;
    }
    acquisitionFailureKey_.clear();
    startAcquisitionVerification();
}

void ComponentStoreTab::noteAcquisitionUnitStarted() { ++acquisitionUnitsPending_; }

void ComponentStoreTab::finishAcquisitionUnit(const QString& failure, bool cancelled) {
    finishAcquisitionUnit(failure, cancelled, QString());
}

void ComponentStoreTab::finishAcquisitionUnit(const QString& failure, bool cancelled,
                                              const QString& failureKey) {
    if (cancelled) acquisitionCancelled_ = true;
    if (!failure.isEmpty()) {
        acquisitionFailures_.append(failure);
        if (acquisitionFailureKey_.isEmpty()) acquisitionFailureKey_ = failureKey;
    }
    if (acquisitionUnitsPending_ > 0) --acquisitionUnitsPending_;
    updateControls();
    if (acquisitionUnitsPending_ == 0 && acquisitionSequenceActive_) finalizeAcquisition();
}

void ComponentStoreTab::cancelProgress() {
    if (progress_ != nullptr) {
        progress_->setRange(0, 100);
        progress_->setValue(0);
        progress_->hide();
    }
}
void ComponentStoreTab::updateControls() {
    if (downloadButton_ == nullptr) return;
    const bool provisioning = provisioner_ != nullptr && provisioner_->running();
    // Every long-running state, including the verification itself, disables the
    // actions that would start another one: the surface is non-reentrant while
    // any worker is alive.
    const bool busy = provisioning || animeJanaiRuntimeDownloading_ || rifeDownloading_ ||
                      modelsDownloading_ || verifying_ || acquisitionChecking_ || inventoryScanning_ ||
                      acquisitionSequenceActive_;
    downloadButton_->setEnabled(!busy);
    verifyButton_->setEnabled(!busy);
    cancelButton_->setEnabled(busy);
    if (runtimeCheck_ != nullptr) runtimeCheck_->setEnabled(!busy);
    if (rifeCheck_ != nullptr) rifeCheck_->setEnabled(!busy);
    if (spatialCheck_ != nullptr) spatialCheck_->setEnabled(!busy);
    if (kernelsCheck_ != nullptr) kernelsCheck_->setEnabled(!busy);
    if (auto* refreshButton = findChild<QPushButton*>(QStringLiteral("refreshComponentsButton")))
        refreshButton->setEnabled(!busy);
    if (animeJanaiBackendCombo_ != nullptr) animeJanaiBackendCombo_->setEnabled(!busy);
    if (importAnimeJanaiButton_ != nullptr) importAnimeJanaiButton_->setEnabled(!busy);
    if (progress_ == nullptr) return;
    // Reopening Settings shows this same tab. A download that outlived the
    // dialog must put its bar back; idle is the only state that hides it.
    if (busy) progress_->show();
    else progress_->hide();
    publishAcquisition();
}

void ComponentStoreTab::cancelAcquisition() {
    if (provisioner_ != nullptr) provisioner_->cancel();
    if (inventoryScanning_) {
        if (inventoryCancelFlag_) inventoryCancelFlag_->store(true);
        ++inventoryGeneration_;
        inventoryScanning_ = false;
        componentStagePending_ = false;
        acquisitionSequenceActive_ = false;
        if (recommendedInstallPending_) recommendedInstallPending_ = false;
        updateControls();
        return;
    }
    if (acquisitionChecking_) {
        if (acquisitionCheckCancelFlag_) acquisitionCheckCancelFlag_->store(true);
        acquisitionChecking_ = false;
        acquisitionSequenceActive_ = false;
        pendingModels_.clear();
        applyStatus(QStringLiteral("native.components.cancelled"), QString());
        updateControls();
        return;
    }
    if (verifying_) {
        // The in-flight hash run's delivery is discarded rather than
        // published: nothing proves a verdict from a run nobody waited for,
        // and no partial result may be mistaken for the answer.
        ++runGeneration_;
        verifying_ = false;
        cancelProgress();
        applyStatus(QStringLiteral("native.components.cancelled"),
                    tr("Verification was cancelled."));
        updateControls();
        return;
    }
    // The units run at the same time, so one Cancel arms every unit's gate: the
    // run stays in flight until each worker observes it and settles, which is
    // what keeps the bar honest while several transfers unwind.
    bool anyArmed = false;
    if (animeJanaiCancelFlag_) {
        animeJanaiCancelFlag_->store(true);
        anyArmed = true;
    }
    if (rifeCancelFlag_) {
        rifeCancelFlag_->store(true);
        anyArmed = true;
    }
    if (modelCancelFlag_) {
        modelCancelFlag_->store(true);
        anyArmed = true;
    }
    pendingModels_.clear();
    if (anyArmed) {
        applyStatus(QStringLiteral("native.components.cancelled"),
                    tr("Cancelling the running downloads..."));
        updateControls();
        return;
    }
    modelsDownloading_ = false;
    acquisitionUnitsPending_ = 0;
    pendingModels_.clear();
    if (progress_ != nullptr) progress_->hide();
    applyStatus(QStringLiteral("native.components.cancelled"), QString());
    updateControls();
}

void ComponentStoreTab::syncAcquisitionState() {
    publishAcquisition();
}

void ComponentStoreTab::publishAcquisition() {
    const bool downloading = (provisioner_ != nullptr && provisioner_->running()) ||
                             animeJanaiRuntimeDownloading_ || rifeDownloading_ ||
                             modelsDownloading_ || acquisitionSequenceActive_;
    if (!downloading) {
        if (!acquisitionAnnounced_) return;
        acquisitionAnnounced_ = false;
        emit acquisitionIdle();
        return;
    }
    const bool started = !acquisitionAnnounced_;
    acquisitionAnnounced_ = true;
    int percent = -1;
    if (progress_ != nullptr && progress_->maximum() > progress_->minimum()) {
        percent = progress_->value();
    }
    QString status = status_ != nullptr ? status_->text() : QString();
    if (status.isEmpty()) status = tr("Downloading components…");
    emit acquisitionProgress(status, percent, started);
}

void ComponentStoreTab::applyStatus(const QString& key, const QString& reason) {
    statusKey_ = key;
    statusReason_ = reason;
    if (status_ != nullptr) {
        status_->setText(reason);
        const QString severity =
            key == QStringLiteral("native.components.cancelled")
                ? QStringLiteral("cancelled")
                : (key.isEmpty() ? (reason.isEmpty() ? QStringLiteral("neutral")
                                                      : QStringLiteral("success"))
                                 : QStringLiteral("failure"));
        status_->setProperty("statusSeverity", severity);
        status_->setAccessibleDescription(severity);
        status_->style()->unpolish(status_);
        status_->style()->polish(status_);
    }
    if (!key.isEmpty()) {
        qWarning().noquote() << "native components:" << key << reason;
    }
}

}  // namespace mikai::app
