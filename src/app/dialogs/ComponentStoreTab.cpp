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
#include <QtCore/QLocale>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtGui/QFont>
#include <QtGui/QPalette>
#include <QtGui/QResizeEvent>
#include <QtCore/QVariant>
#include <QtCore/QVariantMap>
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

ComponentStoreTab::~ComponentStoreTab() {
    if (provisioner_ != nullptr) provisioner_->cancel();
    if (animeJanaiCancelFlag_) animeJanaiCancelFlag_->store(true);
    if (rifeCancelFlag_) rifeCancelFlag_->store(true);
    if (modelCancelFlag_) modelCancelFlag_->store(true);
}

QString animeJanaiSetupPrompt(qint64* bytes) {
    if (bytes != nullptr) *bytes = 0;
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
        const AnimeJanaiInstallation installation =
            describeAnimeJanaiInstallation(Backend::TensorRT, false);
        if (!installation.installed) {
            const ComponentPack* runtime = tensorRtRuntimePack();
            const ComponentPack* kernel = tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
            if (runtime != nullptr) append(translate("TensorRT runtime"), runtime->assetBytes);
            if (kernel != nullptr) append(kernel->name, kernel->assetBytes);
        } else {
            const ComponentBinDirs dirs = installedComponentBinDirs(gpu, componentStore());
            if (!dirs.reasonKey.isEmpty()) {
                const ComponentPack* kernel =
                    tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
                if (kernel != nullptr) append(kernel->name, kernel->assetBytes);
            }
        }
    } else {
        const AnimeJanaiInstallation installation =
            describeAnimeJanaiInstallation(Backend::DirectML, false);
        if (!installation.installed) {
            append(translate("DirectML backend"), 12458649 + 202292617);
        }
    }

    const RifeArchiveStore archive = rifeArchiveStore();
    bool rifeMissing = false;
    for (const RifeModelPin& pin : rifeModelPins()) {
        if (!storedRifeGraph(archive, pin).installed) {
            rifeMissing = true;
            break;
        }
    }
    if (rifeMissing) append(translate("RIFE interpolation models"),
                            rifeArchivePack().assetBytes);
    if (bytes != nullptr) *bytes = totalBytes;
    if (missing.isEmpty()) return {};
    return translate("Components recommended for this PC are not installed:")
        + QStringLiteral("<ul>") + missing.join(QString()) + QStringLiteral("</ul>")
        + translate("Download and install them now (%1 MB)?")
              .arg(QLocale().toString(totalBytes / 1048576));
}

void ComponentStoreTab::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(theme::kSpacingNone, theme::kSpacingNone,
                              theme::kSpacingNone, theme::kSpacingNone);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(theme::kSpacingLarge, theme::kSpacingLarge,
                             theme::kSpacingLarge, theme::kSpacingLarge);
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
    QFont gpuFont = gpuLabel_->font();
    gpuFont.setBold(true);
    gpuLabel_->setFont(gpuFont);
    root->addWidget(gpuLabel_);

    auto* backendRow = new QHBoxLayout();
    backendRow->setSpacing(theme::kSpacingSmall);
    animeJanaiBackendLabel_ = new QLabel(this);
    animeJanaiBackendLabel_->setObjectName(QStringLiteral("muted"));
    backendRow->addWidget(animeJanaiBackendLabel_);
    animeJanaiBackendCombo_ = new QComboBox(this);
    animeJanaiBackendCombo_->setObjectName(QStringLiteral("animeJanaiRuntimeBackend"));
    animeJanaiBackendCombo_->setAccessibleName(tr("AnimeJaNai backend used by Apply changes and Verify"));
    animeJanaiBackendCombo_->addItem(tr("TensorRT (CUDA / NVDEC)"),
                                     static_cast<int>(Backend::TensorRT));
    animeJanaiBackendCombo_->addItem(tr("DirectML (D3D11VA)"),
                                     static_cast<int>(Backend::DirectML));
    backendRow->addWidget(animeJanaiBackendCombo_);
    backendRow->addStretch();
    root->addLayout(backendRow);

    recommendationBanner_ = new QLabel(this);
    recommendationBanner_->setObjectName(QStringLiteral("muted"));
    recommendationBanner_->setWordWrap(true);
    root->addWidget(recommendationBanner_);
    auto makeCard = [this, root](const QString& objectName, QCheckBox** checkBox,
                                  QLabel** stateLabel) {
        auto* card = new QGroupBox(this);
        card->setObjectName(QStringLiteral("nativeComponentPackCard"));
        card->setProperty("componentCardId", objectName);
        auto* cardLayout = new QHBoxLayout(card);
        cardLayout->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium,
                                       theme::kSpacingMedium, theme::kSpacingMedium);
        cardLayout->setSpacing(theme::kSpacingMedium);
        *checkBox = new QCheckBox(card);
        (*checkBox)->setFocusPolicy(Qt::NoFocus);
        (*checkBox)->setAttribute(Qt::WA_TransparentForMouseEvents);
        cardLayout->addWidget(*checkBox, 0, Qt::AlignVCenter);
        auto* details = new QWidget(card);
        auto* detailsLayout = new QVBoxLayout(details);
        detailsLayout->setContentsMargins(0, 0, 0, 0);
        detailsLayout->setSpacing(theme::kSpacingSmall);
        auto* titleRow = new QHBoxLayout();
        titleRow->setSpacing(theme::kSpacingSmall);
        auto* title = new QLabel(details);
        title->setObjectName(objectName + QStringLiteral("Title"));
        QFont titleFont = title->font();
        titleFont.setBold(true);
        title->setFont(titleFont);
        titleRow->addWidget(title);
        *stateLabel = new QLabel(details);
        (*stateLabel)->setObjectName(objectName + QStringLiteral("State"));
        titleRow->addWidget(*stateLabel);
        titleRow->addStretch();
        detailsLayout->addLayout(titleRow);
        auto* description = new QLabel(details);
        description->setObjectName(objectName + QStringLiteral("Description"));
        description->setProperty("textRole", QStringLiteral("muted"));
        description->setWordWrap(true);
        detailsLayout->addWidget(description);
        cardLayout->addWidget(details, 1);
        auto* size = new QLabel(card);
        size->setObjectName(objectName + QStringLiteral("Size"));
        cardLayout->addWidget(size, 0, Qt::AlignVCenter);
        root->addWidget(card);
        return card;
    };
    auto* runtimeCard = makeCard(QStringLiteral("tensorRtRuntimeCard"),
                                 &runtimeCheck_, &runtimeState_);
    auto* rifeCard = makeCard(QStringLiteral("rifeModelsCard"), &rifeCheck_, &rifeState_);
    auto* kernelsCard = makeCard(QStringLiteral("tensorRtKernelsCard"),
                                 &kernelsCheck_, &kernelsState_);
    for (auto* card : {runtimeCard, rifeCard, kernelsCard}) {
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
    setCardText(kernelsCard, tr("TensorRT kernels for this GPU"),
                tr("Engine-builder kernels matched to this GPU generation."));
    auto updateRuntimeSelection = [this](bool checked) {
        if (checked) selectedModelIds_.insert(QStringLiteral("component:runtime"));
        else selectedModelIds_.remove(QStringLiteral("component:runtime"));
    };
    connect(runtimeCheck_, &QCheckBox::toggled, this, [this, updateRuntimeSelection](bool checked) {
        updateRuntimeSelection(checked);
        updateControls();
    });
    connect(rifeCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) selectedModelIds_.insert(QStringLiteral("rife:archive"));
        else selectedModelIds_.remove(QStringLiteral("rife:archive"));
        updateControls();
    });
    connect(kernelsCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) selectedModelIds_.insert(QStringLiteral("component:kernels"));
        else selectedModelIds_.remove(QStringLiteral("component:kernels"));
        updateControls();
    });

    auto* actions = new QVBoxLayout();
    actions->setSpacing(theme::kSpacingSmall);
    auto* primaryActions = new QHBoxLayout();
    primaryActions->setSpacing(theme::kSpacingSmall);
    downloadButton_ = new QPushButton(this);
    downloadButton_->setObjectName(QStringLiteral("primary"));
    primaryActions->addWidget(downloadButton_);
    auto* refreshButton = new QPushButton(this);
    refreshButton->setObjectName(QStringLiteral("refreshComponentsButton"));
    primaryActions->addWidget(refreshButton);
    primaryActions->addStretch();
    actions->addLayout(primaryActions);
    auto* utilityActions = new QHBoxLayout();
    utilityActions->setSpacing(theme::kSpacingSmall);
    verifyButton_ = new QPushButton(this);
    verifyButton_->setObjectName(QStringLiteral("nativeComponentVerifyButton"));
    utilityActions->addWidget(verifyButton_);
    importAnimeJanaiButton_ = new QPushButton(this);
    importAnimeJanaiButton_->setObjectName(QStringLiteral("nativeComponentImportAnimeJanaiButton"));
    utilityActions->addWidget(importAnimeJanaiButton_);
    cancelButton_ = new QPushButton(this);
    cancelButton_->setObjectName(QStringLiteral("nativeComponentTabCancelButton"));
    cancelButton_->setEnabled(false);
    utilityActions->addWidget(cancelButton_);
    utilityActions->addStretch();
    actions->addLayout(utilityActions);
    root->addLayout(actions);

    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("nativeComponentTabProgress"));
    progress_->setRange(0, 100);
    progress_->setTextVisible(false);
    progress_->hide();
    root->addWidget(progress_);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("nativeComponentStatus"));
    status_->setAccessibleName(tr("Components status"));
    status_->setWordWrap(true);
    root->addWidget(status_);
    root->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll);

    connect(animeJanaiBackendCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { refresh(); });
    connect(verifyButton_, &QPushButton::clicked, this, &ComponentStoreTab::verifyAll);
    connect(downloadButton_, &QPushButton::clicked, this, &ComponentStoreTab::installRecommended);
    connect(findChild<QPushButton*>(QStringLiteral("refreshComponentsButton")),
            &QPushButton::clicked, this, &ComponentStoreTab::refresh);
    connect(importAnimeJanaiButton_, &QPushButton::clicked, this,
            &ComponentStoreTab::chooseAnimeJanaiRuntimeDirectory);
    connect(cancelButton_, &QPushButton::clicked, this, [this] {
        if (provisioner_ != nullptr) provisioner_->cancel();
        if (animeJanaiCancelFlag_) {
            animeJanaiCancelFlag_->store(true);
            pendingModels_.clear();
            applyStatus(QStringLiteral("native.components.cancelled"),
                        tr("Cancelling AnimeJaNai runtime installation..."));
            updateControls();
            return;
        }
        if (rifeCancelFlag_) {
            rifeCancelFlag_->store(true);
            pendingModels_.clear();
            applyStatus(QStringLiteral("native.components.cancelled"), QString());
            updateControls();
            return;
        }
        if (modelCancelFlag_) {
            modelCancelFlag_->store(true);
            pendingModels_.clear();
            applyStatus(QStringLiteral("native.components.cancelled"), QString());
            updateControls();
            return;
        }
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
    hint_->setText(tr("Choose which recommended components and optional RIFE models to install."));
    auto setCardText = [this](const QString& objectName, const QString& title,
                              const QString& description) {
        findChild<QLabel*>(objectName + QStringLiteral("Title"))->setText(title);
        findChild<QLabel*>(objectName + QStringLiteral("Description"))->setText(description);
    };
    setCardText(QStringLiteral("tensorRtRuntimeCard"), tr("TensorRT runtime"),
                tr("Fastest NVIDIA upscaling engine; without it NVIDIA falls back to DirectML."));
    setCardText(QStringLiteral("rifeModelsCard"), tr("RIFE interpolation models"),
                tr("Frame interpolation, not needed for upscale only."));
    setCardText(QStringLiteral("tensorRtKernelsCard"),
                tr("TensorRT kernels for this GPU"),
                tr("Engine-builder kernels matched to this GPU generation."));
    verifyButton_->setText(tr("Verify"));
    verifyButton_->setAccessibleName(tr("Re-check every installed element against its digest"));
    downloadButton_->setText(tr("Apply changes"));
    downloadButton_->setAccessibleName(tr("Apply selected component changes"));
    findChild<QPushButton*>(QStringLiteral("refreshComponentsButton"))->setText(tr("Refresh"));
    importAnimeJanaiButton_->setText(tr("Install local runtime..."));
    importAnimeJanaiButton_->setAccessibleName(
        tr("Choose a local AnimeJaNai runtime directory and install its selected backend closure"));
    cancelButton_->setText(tr("Cancel"));
    cancelButton_->setAccessibleName(tr("Cancel the running download"));
    if (animeJanaiBackendLabel_ != nullptr) {
        animeJanaiBackendLabel_->setText(tr("AnimeJaNai backend:"));
    }
    recommendationBanner_->setText(
        QStringLiteral("!  ") +
        tr("Components recommended for this PC are not installed. Check the highlighted items below, then click Apply changes to install them."));
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
                        pendingModels_.clear();
                        applyStatus(QStringLiteral("native.components.fetch"), summary);
                    } else {
                        refresh();
                        downloadRifeArchive();
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
    const ComponentStore store = componentStore();
    const ComponentPack* runtimePack = tensorRtRuntimePack();
    const StoredPack runtimeStored = runtimePack != nullptr
                                         ? storedPack(store, *runtimePack)
                                         : StoredPack{};
    const AnimeJanaiInstallation trt =
        describeAnimeJanaiInstallation(Backend::TensorRT, false);
    const ComponentBinDirs dirs = installedComponentBinDirs(gpu, store);
    const bool runtimeInstalled = trt.installed &&
                                  (runtimeStored.installed || dirs.reasonKey.isEmpty());
    const bool runtimeRecommended = gpu.nvidia && !runtimeInstalled;

    const QVector<RifeModelPin>& pins = rifeModelPins();
    const RifeArchiveStore rifeStore = rifeArchiveStore();
    bool rifeInstalled = !pins.isEmpty();
    for (const RifeModelPin& pin : pins) {
        if (!storedRifeGraph(rifeStore, pin).installed) {
            rifeInstalled = false;
            break;
        }
    }
    const ComponentPack* kernelsPack =
        tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
    const StoredPack kernelsStored = kernelsPack != nullptr
                                         ? storedPack(store, *kernelsPack)
                                         : StoredPack{};
    const bool kernelsInstalled = kernelsPack == nullptr ||
                                  kernelsStored.installed || dirs.reasonKey.isEmpty();
    const bool kernelsRecommended = gpu.nvidia && kernelsPack != nullptr &&
                                    !kernelsInstalled;

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

    setState(runtimeState_,
             runtimeRecommended ? tr("recommended for this PC")
                                : (runtimeInstalled ? tr("installed") : tr("optional")));
    setState(rifeState_, rifeInstalled ? tr("OK") : tr("optional"));
    setState(kernelsState_,
             kernelsRecommended ? tr("recommended for this PC")
                                : (kernelsInstalled ? tr("installed") : tr("optional")));
    auto setBadgeRole = [](QLabel* badge, bool recommended) {
        badge->setAutoFillBackground(false);
        badge->setStyleSheet(QString());
        badge->setProperty("textRole", recommended ? QStringLiteral("recommendation")
                                                   : QStringLiteral("muted"));
        badge->style()->unpolish(badge);
        badge->style()->polish(badge);
    };
    setBadgeRole(runtimeState_, runtimeRecommended);
    setBadgeRole(rifeState_, false);
    setBadgeRole(kernelsState_, kernelsRecommended);

    runtimeCheck_->setAccessibleName(tr("TensorRT runtime"));
    rifeCheck_->setAccessibleName(tr("RIFE interpolation models"));
    kernelsCheck_->setAccessibleName(tr("TensorRT kernels for this GPU"));
    kernelsCheck_->setVisible(gpu.nvidia && kernelsPack != nullptr);
    if (!componentSelectionsInitialized_) {
        componentSelectionsInitialized_ = true;
        if (runtimeRecommended) selectedModelIds_.insert(QStringLiteral("component:runtime"));
        if (kernelsRecommended) selectedModelIds_.insert(QStringLiteral("component:kernels"));
        if (!rifeInstalled) selectedModelIds_.insert(QStringLiteral("rife:archive"));
    }
    auto showInstalled = [](QCheckBox* box, bool installed) {
        if (box == nullptr) return;
        box->blockSignals(true);
        box->setChecked(installed);
        box->blockSignals(false);
    };
    showInstalled(runtimeCheck_, runtimeInstalled);
    showInstalled(rifeCheck_, rifeInstalled);
    showInstalled(kernelsCheck_, kernelsInstalled);
    const bool showBanner = runtimeRecommended || kernelsRecommended;
    recommendationBanner_->setVisible(showBanner);
    runtimeCheck_->parentWidget()->setProperty("recommended", runtimeRecommended);
    kernelsCheck_->parentWidget()->setProperty("recommended", kernelsRecommended);
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
    const Backend backend = selectedBackend();
    const int backendKey = static_cast<int>(backend);
    const AnimeJanaiInstallation result = describeAnimeJanaiInstallation(backend, true);
    if (result.reasonKey == QStringLiteral("animejanai.installation.recordMissing")) {
        verifiedAnimeJanaiBackends_.remove(backendKey);
        animeJanaiVerificationErrors_.remove(backendKey);
    } else if (result.installed) {
        verifiedAnimeJanaiBackends_.insert(backendKey);
        animeJanaiVerificationErrors_.remove(backendKey);
        ++verified;
    } else {
        verifiedAnimeJanaiBackends_.remove(backendKey);
        const QString label = backend == Backend::TensorRT
                                  ? tr("TensorRT / CUDA 13 closure")
                                  : tr("DirectML closure");
        const QString reason = result.reason.isEmpty()
                                   ? tr("one or more required dependencies are missing or corrupt")
                                   : result.reason;
        animeJanaiVerificationErrors_.insert(backendKey, reason);
        ++failed;
        failures.append(QStringLiteral("%1: %2").arg(label, reason));
    }
    int failedRifeGraphs = 0;
    for (const RifeModelPin& pin : rifeModelPins()) {
        const StoredRifeGraph stored = storedRifeGraph(rifeArchiveStore(), pin);
        if (!stored.installed) {
            verifiedRifeGraphs_.remove(pin.fileName);
            rifeVerificationErrors_.remove(pin.fileName);
            continue;
        }
        const StoredRifeGraph result = verifyRifeGraph(rifeArchiveStore(), pin);
        if (result.installed) {
            verifiedRifeGraphs_.insert(pin.fileName);
            rifeVerificationErrors_.remove(pin.fileName);
            ++verified;
        } else {
            verifiedRifeGraphs_.remove(pin.fileName);
            rifeVerificationErrors_.insert(pin.fileName, result.reason);
            ++failed;
            ++failedRifeGraphs;
        }
    }
    if (failedRifeGraphs > 0)
        failures.append(tr("%1 RIFE interpolation model(s) failed verification.")
                            .arg(failedRifeGraphs));

    int failedSpatialModels = 0;
    const QVector<StoredSpatialAsset> models = storedSpatialAssets();
    for (const StoredSpatialAsset& model : models) {
        if (model.verified) {
            ++verified;
        } else {
            ++failed;
            ++failedSpatialModels;
        }
    }
    if (failedSpatialModels > 0)
        failures.append(tr("%1 spatial model(s) failed verification.")
                            .arg(failedSpatialModels));

    renderCards();
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
    downloadMissing(true);
}

void ComponentStoreTab::installRecommended() {
    const GpuComputeCapability& gpu = observeComputeCapability();
    const int backendIndex = animeJanaiBackendCombo_->findData(
        static_cast<int>(gpu.nvidia ? Backend::TensorRT : Backend::DirectML));
    if (backendIndex >= 0) animeJanaiBackendCombo_->setCurrentIndex(backendIndex);

    downloadMissing(false);
}

void ComponentStoreTab::downloadMissing(bool showReview) {
    rifeFailure_.clear();

    pendingModels_.clear();
    const QVector<StoredSpatialAsset> stored = storedSpatialAssets();
    const QVector<SpatialAsset> catalog = spatialAssetCatalog();
    for (int index = 0; index < catalog.size(); ++index) {
        bool present = false;
        for (const StoredSpatialAsset& candidate : stored) {
            if (candidate.asset.id == catalog.at(index).id && candidate.verified) present = true;
        }
        if (selectedModelIds_.contains(catalog.at(index).id) && !present)
            pendingModels_.append(index);
    }

    const Backend backend = selectedBackend();
    const AnimeJanaiInstallation animeJanai =
        describeAnimeJanaiInstallation(backend, false);
    const GpuComputeCapability& gpu = observeComputeCapability();
    const bool runtimeSelected =
        selectedModelIds_.contains(QStringLiteral("component:runtime"));
    const bool kernelsSelected =
        selectedModelIds_.contains(QStringLiteral("component:kernels"));
    const bool componentMissing = backend == Backend::TensorRT &&
        (runtimeSelected || kernelsSelected) && gpu.smMajor > 0 &&
        !installedComponentBinDirs(gpu, componentStore()).reasonKey.isEmpty();
    const bool backendWork = (runtimeSelected && !animeJanai.installed) ||
                             componentMissing;
    QStringList review;
    if (backendWork) {
        review.append(backend == Backend::TensorRT
                          ? tr("AnimeJaNai TensorRT / CUDA backend closure")
                          : tr("AnimeJaNai DirectML backend closure"));
    }
    for (int index : pendingModels_) review.append(catalog.at(index).name);
    bool selectedRifeMissing = false;
    if (selectedModelIds_.contains(QStringLiteral("rife:archive"))) {
        const RifeArchiveStore archive = rifeArchiveStore();
        for (const RifeModelPin& pin : rifeModelPins()) {
            const StoredRifeGraph graph = verifyRifeGraph(archive, pin);
            if (graph.installed) {
                verifiedRifeGraphs_.insert(pin.fileName);
                rifeVerificationErrors_.remove(pin.fileName);
            } else {
                selectedRifeMissing = true;
                verifiedRifeGraphs_.remove(pin.fileName);
                rifeVerificationErrors_.insert(pin.fileName, graph.reason);
            }
        }
    }
    if (selectedRifeMissing) review.append(tr("RIFE 3.7.0 archive pack (all 47 graphs)"));
    if (review.isEmpty()) {
        applyStatus(QString(), tr("No selected missing packs need installation."));
        return;
    }
    qint64 bytes = 0;
    for (int index : pendingModels_) bytes += catalog.at(index).bytes;
    if (selectedRifeMissing) bytes += rifeArchivePack().assetBytes;
    if (showReview) {
        const QString prompt = tr("The following selected packs are missing or need repair:\n%1\n\nEstimated download size: %2\n\nContinue?")
            .arg(review.join(QStringLiteral("\n")),
                 backendWork
                     ? tr("Unknown")
                     : QLocale().formattedDataSize(bytes));
        if (QMessageBox::question(this, tr("Review installation"), prompt,
                                  QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Yes) {
            pendingModels_.clear();
            return;
        }
    }
    modelCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    if (runtimeSelected && !animeJanai.installed) {
        downloadAnimeJanaiRuntime();
        return;
    }
    if (componentMissing && provisioner_ == nullptr) {
        applyStatus(QStringLiteral("native.components.missing"),
                    tr("The compatible TensorRT component pack is missing, but no component provisioner is available."));
        pendingModels_.clear();
        return;
    }
    downloadRemainingComponents();
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
    progress_->show();
    progress_->setRange(0, 100);
    progress_->setValue(0);
    status_->setText(tr("Installing AnimeJaNai runtime and selected backend dependencies..."));
    updateControls();

    QPointer<ComponentStoreTab> self(this);
    runner_->run(
        [backend, cancelFlag, self]() -> QVariant {
            const ComponentProvisionResult result = provisionAnimeJanaiRuntime(
                backend,
                [self](const ComponentProvisionProgress& progress) {
                    if (!self) return;
                    QMetaObject::invokeMethod(
                        self,
                        [self, progress] {
                            if (!self) return;
                            self->progress_->setRange(0, 100);
                            self->progress_->setValue(
                                progress.total > 0
                                    ? static_cast<int>(progress.received * 100 / progress.total)
                                    : 0);
                            self->status_->setText(
                                ComponentStoreTab::tr("Installing %1: %2 of %3")
                                    .arg(progress.pack,
                                         QLocale().formattedDataSize(progress.received),
                                         QLocale().formattedDataSize(progress.total)));
                        },
                        Qt::QueuedConnection);
                },
                [cancelFlag] { return cancelFlag->load(); });
            return QVariantMap{{QStringLiteral("ok"), result.ok},
                               {QStringLiteral("cancelled"), result.cancelled},
                               {QStringLiteral("reason"), result.reason},
                               {QStringLiteral("reasonKey"), result.reasonKey}};
        },
        [self, cancelFlag, backend](const QVariant& value) {
            if (!self) return;
            self->animeJanaiRuntimeDownloading_ = false;
            self->animeJanaiCancelFlag_.reset();
            if (cancelFlag->load() || value.toMap().value(QStringLiteral("cancelled")).toBool()) {
                self->pendingModels_.clear();
                self->applyStatus(QStringLiteral("native.components.cancelled"), QString());
            } else if (!value.toMap().value(QStringLiteral("ok")).toBool()) {
                const QString reason = value.toMap().value(QStringLiteral("reason")).toString();
                const QString reasonKey =
                    value.toMap().value(QStringLiteral("reasonKey")).toString();
                self->pendingModels_.clear();
                const QString backendName = backend == Backend::TensorRT
                                                ? ComponentStoreTab::tr("TensorRT / CUDA 13")
                                                : ComponentStoreTab::tr("DirectML");
                self->applyStatus(
                    reasonKey.isEmpty() ? QStringLiteral("native.components.fetch") : reasonKey,
                    ComponentStoreTab::tr("%1 installation failed: %2")
                        .arg(backendName,
                             reason.isEmpty()
                                 ? ComponentStoreTab::tr("AnimeJaNai runtime installation failed.")
                                 : reason));
            } else {
                self->verifiedAnimeJanaiBackends_.remove(static_cast<int>(backend));
                self->animeJanaiVerificationErrors_.remove(static_cast<int>(backend));
                self->refresh();
                self->downloadRemainingComponents();
            }
            self->refresh();
        },
        [self, cancelFlag, backend](const QString& error) {
            if (!self) return;
            self->animeJanaiRuntimeDownloading_ = false;
            self->animeJanaiCancelFlag_.reset();
            self->pendingModels_.clear();
            if (cancelFlag->load()) {
                self->applyStatus(QStringLiteral("native.components.cancelled"), QString());
            } else {
                const QString backendName = backend == Backend::TensorRT
                                                ? ComponentStoreTab::tr("TensorRT / CUDA 13")
                                                : ComponentStoreTab::tr("DirectML");
                self->applyStatus(QStringLiteral("native.components.fetch"),
                                  ComponentStoreTab::tr("%1 installation failed: %2")
                                      .arg(backendName, error));
            }
            self->refresh();
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
    progress_->show();
    progress_->setRange(0, 100);
    progress_->setValue(0);
    status_->setText(tr("Installing local AnimeJaNai runtime and selected backend dependencies..."));
    updateControls();

    QPointer<ComponentStoreTab> self(this);
    runner_->run(
        [sourceDirectory, backend, cancelFlag, self]() -> QVariant {
            const ComponentProvisionResult result = mplayer::native::importAnimeJanaiRuntime(
                sourceDirectory, backend,
                [self](const ComponentProvisionProgress& progress) {
                    if (!self) return;
                    QMetaObject::invokeMethod(
                        self,
                        [self, progress] {
                            if (!self) return;
                            self->progress_->setRange(0, 100);
                            self->progress_->setValue(
                                progress.total > 0
                                    ? static_cast<int>(progress.received * 100 / progress.total)
                                    : 0);
                            self->status_->setText(
                                ComponentStoreTab::tr("Installing %1: %2 of %3")
                                    .arg(progress.pack,
                                         QLocale().formattedDataSize(progress.received),
                                         QLocale().formattedDataSize(progress.total)));
                        },
                        Qt::QueuedConnection);
                },
                [cancelFlag] { return cancelFlag->load(); });
            return QVariantMap{{QStringLiteral("ok"), result.ok},
                               {QStringLiteral("cancelled"), result.cancelled},
                               {QStringLiteral("reason"), result.reason},
                               {QStringLiteral("reasonKey"), result.reasonKey}};
        },
        [self, cancelFlag, backend](const QVariant& value) {
            if (!self) return;
            self->animeJanaiRuntimeDownloading_ = false;
            self->animeJanaiCancelFlag_.reset();
            if (cancelFlag->load() || value.toMap().value(QStringLiteral("cancelled")).toBool()) {
                self->applyStatus(QStringLiteral("native.components.cancelled"), QString());
            } else if (!value.toMap().value(QStringLiteral("ok")).toBool()) {
                const QString reason = value.toMap().value(QStringLiteral("reason")).toString();
                const QString reasonKey =
                    value.toMap().value(QStringLiteral("reasonKey")).toString();
                const QString backendName = backend == Backend::TensorRT
                                                ? ComponentStoreTab::tr("TensorRT / CUDA 13")
                                                : ComponentStoreTab::tr("DirectML");
                self->applyStatus(
                    reasonKey.isEmpty() ? QStringLiteral("native.components.fetch") : reasonKey,
                    ComponentStoreTab::tr("%1 installation failed: %2")
                        .arg(backendName,
                             reason.isEmpty()
                                 ? ComponentStoreTab::tr("Local AnimeJaNai runtime installation failed.")
                                 : reason));
            } else {
                self->verifiedAnimeJanaiBackends_.remove(static_cast<int>(backend));
                self->animeJanaiVerificationErrors_.remove(static_cast<int>(backend));
                self->applyStatus(QString(), ComponentStoreTab::tr("Local AnimeJaNai runtime installed."));
            }
            self->refresh();
        },
        [self, cancelFlag, backend](const QString& error) {
            if (!self) return;
            self->animeJanaiRuntimeDownloading_ = false;
            self->animeJanaiCancelFlag_.reset();
            if (cancelFlag->load()) {
                self->applyStatus(QStringLiteral("native.components.cancelled"), QString());
            } else {
                const QString backendName = backend == Backend::TensorRT
                                                ? ComponentStoreTab::tr("TensorRT / CUDA 13")
                                                : ComponentStoreTab::tr("DirectML");
                self->applyStatus(QStringLiteral("native.components.fetch"),
                                  ComponentStoreTab::tr("%1 installation failed: %2")
                                      .arg(backendName, error));
            }
            self->refresh();
        });
}

void ComponentStoreTab::downloadRemainingComponents() {
    const ComponentBinDirs verdict =
        installedComponentBinDirs(observeComputeCapability(), componentStore());
    const bool componentMissing =
        selectedBackend() == Backend::TensorRT &&
        (selectedModelIds_.contains(QStringLiteral("component:runtime")) ||
         selectedModelIds_.contains(QStringLiteral("component:kernels"))) &&
        observeComputeCapability().smMajor > 0 && !verdict.reasonKey.isEmpty();
    if (componentMissing && provisioner_ == nullptr) {
        pendingModels_.clear();
        applyStatus(QStringLiteral("native.components.missing"),
                    tr("The compatible TensorRT component pack is missing, but no component provisioner is available."));
        updateControls();
        return;
    }
    if (componentMissing && provisioner_ != nullptr) {
        QString reason;
        if (!provisioner_->ensureStore(&reason)) {
            pendingModels_.clear();
            applyStatus(QStringLiteral("native.components.missing"), reason);
            updateControls();
            return;
        }
        progress_->show();
        progress_->setValue(0);
        updateControls();
        return;
    }
    downloadRifeArchive();
}

void ComponentStoreTab::downloadRifeArchive() {
    bool missing = false;
    const RifeArchiveStore store = rifeArchiveStore();
    const QVector<RifeModelPin> pins = rifeModelPins();
    if (selectedModelIds_.contains(QStringLiteral("rife:archive"))) {
        for (const RifeModelPin& pin : pins) {
            const StoredRifeGraph stored = storedRifeGraph(store, pin);
            if (!stored.installed || stored.bytes != pin.bytes ||
                rifeVerificationErrors_.contains(pin.fileName)) {
                missing = true;
                break;
            }
        }
    }
    if (!missing) {
        downloadNextModel();
        return;
    }
    if (runner_ == nullptr) {
        applyStatus(QStringLiteral("native.components.noClient"),
                    tr("The RIFE archive download needs a runner."));
        pendingModels_.clear();
        updateControls();
        return;
    }

    rifeDownloading_ = true;
    rifeCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelFlag = rifeCancelFlag_;
    progress_->show();
    progress_->setRange(0, 100);
    progress_->setValue(0);
    status_->setText(tr("Downloading the RIFE 3.7.0 graph archive..."));
    updateControls();

    QPointer<ComponentStoreTab> self(this);
    runner_->run(
        [store, pins, cancelFlag, self]() -> QVariant {
            const ComponentProvisionResult result = provisionRifeArchive(
                store, pins,
                [self](const ComponentProvisionProgress& progress) {
                    if (!self) return;
                    QMetaObject::invokeMethod(
                        self,
                        [self, progress] {
                            if (!self) return;
                            self->progress_->setRange(0, 100);
                            self->progress_->setValue(
                                progress.total > 0
                                    ? static_cast<int>(progress.received * 100 / progress.total)
                                    : 0);
                            self->status_->setText(
                                ComponentStoreTab::tr("Downloading %1: %2 of %3")
                                    .arg(progress.pack,
                                         QLocale().formattedDataSize(progress.received),
                                         QLocale().formattedDataSize(progress.total)));
                        },
                        Qt::QueuedConnection);
                },
                [cancelFlag] { return cancelFlag->load(); });
            if (!result.ok) {
                throw std::runtime_error(result.reason.toStdString());
            }
            return QVariant{};
        },
        [self, cancelFlag, pins](const QVariant&) {
            if (!self) return;
            self->rifeDownloading_ = false;
            self->rifeCancelFlag_.reset();
            for (const RifeModelPin& pin : pins) {
                self->verifiedRifeGraphs_.insert(pin.fileName);
                self->rifeVerificationErrors_.remove(pin.fileName);
            }
            self->applyStatus(QString(), ComponentStoreTab::tr("The RIFE 3.7.0 archive graphs are installed."));
            self->refresh();
            if (!cancelFlag->load()) self->downloadNextModel();
        },
        [self, cancelFlag](const QString& error) {
            if (!self) return;
            self->rifeDownloading_ = false;
            self->rifeCancelFlag_.reset();
            if (cancelFlag->load()) {
                self->pendingModels_.clear();
                self->applyStatus(QStringLiteral("native.components.cancelled"), QString());
            } else {
                self->rifeFailure_ = error;
                self->pendingModels_.clear();
                self->applyStatus(QStringLiteral("native.components.fetch"), error);
                self->updateControls();
            }
            self->refresh();
        });
}

void ComponentStoreTab::downloadNextModel() {
    if (pendingModels_.isEmpty()) {
        modelsDownloading_ = false;
        modelCancelFlag_.reset();
        if (progress_ != nullptr) progress_->hide();
        if (!rifeFailure_.isEmpty()) {
            applyStatus(QStringLiteral("native.components.fetch"), rifeFailure_);
            refresh();
            return;
        }
        const bool runtimeSelected =
            selectedModelIds_.contains(QStringLiteral("component:runtime"));
        const AnimeJanaiInstallation backend =
            describeAnimeJanaiInstallation(selectedBackend(), true);
        if (runtimeSelected && !backend.installed) {
            applyStatus(backend.reasonKey.isEmpty()
                            ? QStringLiteral("native.components.fetch")
                            : backend.reasonKey,
                        backend.reason.isEmpty()
                            ? tr("The requested backend installation could not be verified.")
                            : backend.reason);
            refresh();
            return;
        }
        const GpuComputeCapability& gpu = observeComputeCapability();
        if ((selectedModelIds_.contains(QStringLiteral("component:runtime")) ||
             selectedModelIds_.contains(QStringLiteral("component:kernels"))) &&
            selectedBackend() == Backend::TensorRT && gpu.smMajor > 0 &&
            !installedComponentBinDirs(gpu, componentStore()).reasonKey.isEmpty()) {
            applyStatus(QStringLiteral("native.components.missing"),
                        tr("The requested TensorRT component pack is not installed."));
            refresh();
            return;
        }
        const RifeArchiveStore archiveStore = rifeArchiveStore();
        if (selectedModelIds_.contains(QStringLiteral("rife:archive"))) {
            for (const RifeModelPin& pin : rifeModelPins()) {
                const StoredRifeGraph graph = verifyRifeGraph(archiveStore, pin);
                if (!graph.installed) {
                    applyStatus(QStringLiteral("native.components.digest"), graph.reason);
                    refresh();
                    return;
                }
                verifiedRifeGraphs_.insert(pin.fileName);
                rifeVerificationErrors_.remove(pin.fileName);
            }
        }
        for (const StoredSpatialAsset& asset : storedSpatialAssets()) {
            if (!selectedModelIds_.contains(asset.asset.id)) continue;
            if (!asset.verified) {
                applyStatus(QStringLiteral("native.components.digest"), asset.reason);
                refresh();
                return;
            }
        }
        applyStatus(QString(), tr("Every selected pack is installed and verified."));
        refresh();
        return;
    }
    if (runner_ == nullptr || client_ == nullptr) {
        modelsDownloading_ = false;
        modelCancelFlag_.reset();
        applyStatus(QStringLiteral("native.components.noClient"),
                    tr("A model download needs the backend client and a runner."));
        pendingModels_.clear();
        updateControls();
        return;
    }
    const SpatialAsset asset = spatialAssetCatalog().at(pendingModels_.takeFirst());
    modelsDownloading_ = true;
    if (!modelCancelFlag_) modelCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelFlag = modelCancelFlag_;
    progress_->show();
    progress_->setRange(0, 0);
    status_->setText(tr("Downloading %1...").arg(asset.name));
    updateControls();

    backend::MikaiClient* client = client_;
    QPointer<ComponentStoreTab> self(this);
    runner_->run(
        [client, asset, cancelFlag]() -> QVariant {
            const QByteArray bytes = client->fetchBytes(asset.url, kModelDownloadTimeoutMs);
            if (cancelFlag->load()) return QVariant{};
            const SpatialAssetPublication published =
                publishSpatialAssetBytes(asset, bytes, QString());
            if (!published.ok) {
                throw std::runtime_error(
                    QStringLiteral("%1: %2").arg(published.reasonKey, published.reason).toStdString());
            }
            return QVariant{};
        },
        [self, cancelFlag](const QVariant&) {
            if (!self) return;
            if (cancelFlag->load()) {
                self->modelsDownloading_ = false;
                self->modelCancelFlag_.reset();
                self->pendingModels_.clear();
                self->applyStatus(QStringLiteral("native.components.cancelled"), QString());
                self->updateControls();
                return;
            }
            self->downloadNextModel();
        },
        [self, cancelFlag](const QString& error) {
            if (!self) return;
            self->modelsDownloading_ = false;
            self->modelCancelFlag_.reset();
            self->pendingModels_.clear();
            self->applyStatus(cancelFlag->load()
                                  ? QStringLiteral("native.components.cancelled")
                                  : QStringLiteral("native.components.fetch"),
                              cancelFlag->load() ? QString() : error);
            self->updateControls();
        });
}

void ComponentStoreTab::updateControls() {
    if (downloadButton_ == nullptr) return;
    const bool provisioning = provisioner_ != nullptr && provisioner_->running();
    const bool busy =
        provisioning || animeJanaiRuntimeDownloading_ || rifeDownloading_ || modelsDownloading_;
    downloadButton_->setEnabled(!busy);
    verifyButton_->setEnabled(!busy);
    cancelButton_->setEnabled(busy);
    if (runtimeCheck_ != nullptr) runtimeCheck_->setEnabled(!busy);
    if (rifeCheck_ != nullptr) rifeCheck_->setEnabled(!busy);
    if (kernelsCheck_ != nullptr) kernelsCheck_->setEnabled(!busy);
    if (auto* refreshButton = findChild<QPushButton*>(QStringLiteral("refreshComponentsButton")))
        refreshButton->setEnabled(!busy);
    if (animeJanaiBackendCombo_ != nullptr) animeJanaiBackendCombo_->setEnabled(!busy);
    if (importAnimeJanaiButton_ != nullptr) importAnimeJanaiButton_->setEnabled(!busy);
    if (!busy && progress_ != nullptr) progress_->hide();
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
    }
    if (!key.isEmpty()) {
        qWarning().noquote() << "native components:" << key << reason;
    }
}

}  // namespace mikai::app
