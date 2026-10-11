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
}

ComponentStoreTab::~ComponentStoreTab() {
    // Every queued completion and every in-flight worker checks this before it
    // touches the tab, so destruction can never race a delivery: the flag goes
    // first, then the generation is bumped so a body already past the flag
    // still finds a run that can no longer be rendered.
    if (alive_) alive_->store(false);
    ++runGeneration_;
    verifying_ = false;
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
    /// The one lookup with this file's context. Every literal handed here MUST
    /// be wrapped in QT_TRANSLATE_NOOP, which is what lupdate reads; at run
    /// time the macro is the literal itself.
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
        // The closure acquisition is one queue, so it is priced by the very
        // function the installer derives that queue from. It is never re-summed
        // here: the shared runtime archive serves both the pack install and the
        // trtexec extraction, so a second sum would double-count the 186 MB
        // runtime, and it always includes the mandatory Aji release the previous
        // estimate omitted. Warm state prices at exactly nothing.
        const qint64 closureBytes = animeJanaiMissingDownloadBytes(Backend::TensorRT);
        if (closureBytes > 0) {
            append(translate(QT_TRANSLATE_NOOP(
                       "mikai::app::ComponentStoreTab", "AnimeJaNai TensorRT closure")),
                   closureBytes);
        } else {
            // The closure is installed; a still-missing builder resource keeps
            // showing as its own row, exactly as the pack store reports it.
            const ComponentBinDirs dirs = installedComponentBinDirs(gpu, componentStore());
            if (!dirs.reasonKey.isEmpty()) {
                const ComponentPack* kernel =
                    tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
                if (kernel != nullptr) append(kernel->name, kernel->assetBytes);
            }
        }
    } else {
        const qint64 closureBytes = animeJanaiMissingDownloadBytes(Backend::DirectML);
        if (closureBytes > 0) {
            append(translate(QT_TRANSLATE_NOOP(
                       "mikai::app::ComponentStoreTab", "AnimeJaNai DirectML closure")),
                   closureBytes);
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
    if (rifeMissing) append(translate(QT_TRANSLATE_NOOP(
                                "mikai::app::ComponentStoreTab",
                                "RIFE interpolation models")),
                            rifeArchivePack().assetBytes);
    const QVector<StoredSpatialAsset> spatialStored = storedSpatialAssets();
    const QVector<SpatialAsset> spatialCatalog = spatialAssetCatalog();
    qint64 missingSpatialBytes = 0;
    int missingSpatialCount = 0;
    for (const SpatialAsset& asset : spatialCatalog) {
        const StoredSpatialAsset* row = spatialInventoryRow(spatialStored, asset.id);
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
        (*checkBox)->setObjectName(objectName + QStringLiteral("Check"));
        cardLayout->addWidget(*checkBox, 0, Qt::AlignVCenter);
        auto* details = new QWidget(card);
        auto* detailsLayout = new QVBoxLayout(details);
        detailsLayout->setContentsMargins(0, 0, 0, 0);
        detailsLayout->setSpacing(theme::kSpacingSmall);
        auto* titleRow = new QHBoxLayout();
        titleRow->setSpacing(theme::kSpacingSmall);
        auto* title = new QLabel(details);
        title->setObjectName(objectName + QStringLiteral("Title"));
        title->setWordWrap(true);
        QFont titleFont = title->font();
        titleFont.setBold(true);
        title->setFont(titleFont);
        titleRow->addWidget(title);
        *stateLabel = new QLabel(details);
        (*stateLabel)->setObjectName(objectName + QStringLiteral("State"));
        (*stateLabel)->setWordWrap(true);
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
        size->setWordWrap(true);
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
    connect(runtimeCheck_, &QCheckBox::toggled, this, [this, updateRuntimeSelection](bool checked) {
        updateRuntimeSelection(checked);
        updateControls();
    });
    connect(rifeCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) selectedPacks_.insert(QStringLiteral("rife:archive"));
        else selectedPacks_.remove(QStringLiteral("rife:archive"));
        updateControls();
    });
    connect(spatialCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) selectedPacks_.insert(QStringLiteral("component:spatial"));
        else selectedPacks_.remove(QStringLiteral("component:spatial"));
        updateControls();
    });
    connect(kernelsCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) selectedPacks_.insert(QStringLiteral("component:kernels"));
        else selectedPacks_.remove(QStringLiteral("component:kernels"));
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
            [this](int) {
                // A different backend answers a different question: any run still
                // in flight was hashing the previous one, so its results are
                // discarded instead of being rendered as fresh evidence.
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
    connect(cancelButton_, &QPushButton::clicked, this, [this] {
        if (provisioner_ != nullptr) provisioner_->cancel();
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
    findChild<QPushButton*>(QStringLiteral("refreshComponentsButton"))->setText(tr("Refresh"));
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
    const QVector<SpatialAsset> spatialCatalog = spatialAssetCatalog();
    const QVector<StoredSpatialAsset> spatialStored = storedSpatialAssets();
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
    const bool spatialRecommended = verifiedSpatial != spatialCatalog.size();

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
    setBadgeRole(spatialState_, spatialRecommended);
    setBadgeRole(kernelsState_, kernelsRecommended);

    runtimeCheck_->setAccessibleName(tr("TensorRT runtime"));
    rifeCheck_->setAccessibleName(tr("RIFE interpolation models"));
    spatialCheck_->setAccessibleName(tr("AnimeJaNai upscaling models"));
    spatialCheck_->setAccessibleDescription(
        tr("Select all five pinned upscaling graphs for download or repair."));
    kernelsCheck_->setAccessibleName(tr("TensorRT kernels for this GPU"));
    kernelsCheck_->setVisible(gpu.nvidia && kernelsPack != nullptr);
    if (!componentSelectionsInitialized_) {
        componentSelectionsInitialized_ = true;
        if (runtimeRecommended) selectedPacks_.insert(QStringLiteral("component:runtime"));
        if (kernelsRecommended) selectedPacks_.insert(QStringLiteral("component:kernels"));
        if (!rifeInstalled) selectedPacks_.insert(QStringLiteral("rife:archive"));
        if (spatialRecommended) selectedPacks_.insert(QStringLiteral("component:spatial"));
    }
    auto showSelection = [this](QCheckBox* box, const QString& pack) {
        box->blockSignals(true);
        box->setChecked(selectedPacks_.contains(pack));
        box->blockSignals(false);
    };
    showSelection(runtimeCheck_, QStringLiteral("component:runtime"));
    showSelection(rifeCheck_, QStringLiteral("rife:archive"));
    showSelection(spatialCheck_, QStringLiteral("component:spatial"));
    showSelection(kernelsCheck_, QStringLiteral("component:kernels"));
    const bool showBanner = runtimeRecommended || kernelsRecommended || spatialRecommended;
    recommendationBanner_->setVisible(showBanner);
    runtimeCheck_->parentWidget()->setProperty("recommended", runtimeRecommended);
    spatialCheck_->parentWidget()->setProperty("recommended", spatialRecommended);
    kernelsCheck_->parentWidget()->setProperty("recommended", kernelsRecommended);
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
    request.storedSpatial = storedSpatialAssets();
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
    progress_->setRange(0, 100);
    progress_->setValue(progress.total > 0
                            ? static_cast<int>(progress.received * 100 / progress.total)
                            : 0);
    status_->setText(tr("Installing %1: %2 of %3")
                         .arg(progress.pack,
                              QLocale().formattedDataSize(progress.received),
                              QLocale().formattedDataSize(progress.total)));
}

void ComponentStoreTab::applyDownloadProgress(const ComponentProvisionProgress& progress) {
    progress_->setRange(0, 100);
    progress_->setValue(progress.total > 0
                            ? static_cast<int>(progress.received * 100 / progress.total)
                            : 0);
    status_->setText(tr("Downloading %1: %2 of %3")
                         .arg(progress.pack,
                              QLocale().formattedDataSize(progress.received),
                              QLocale().formattedDataSize(progress.total)));
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

    runner_->run(
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
            for (const SpatialAsset& asset : request.spatialCatalog) {
                if (!alive->load()) return QVariant{};
                const StoredSpatialAsset* model =
                    spatialInventoryRow(request.storedSpatial, asset.id);
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
        [this, alive = alive_, generation](const QString& error) {
            if (!alive->load() || runGeneration_ != generation) return;
            verifying_ = false;
            ++runGeneration_;
            if (progress_ != nullptr) progress_->hide();
            applyStatus(QStringLiteral("native.components.fetch"),
                        tr("The components could not be verified: %1").arg(error));
            updateControls();
        });
}

void ComponentStoreTab::downloadMissing() {
    downloadMissing(true);
}

void ComponentStoreTab::installRecommended() {
    const GpuComputeCapability& gpu = observeComputeCapability();
    const int backendIndex = animeJanaiBackendCombo_->findData(
        static_cast<int>(gpu.nvidia ? Backend::TensorRT : Backend::DirectML));
    if (backendIndex >= 0) animeJanaiBackendCombo_->setCurrentIndex(backendIndex);
    refresh();

    // Explicit setup confirmation is distinct from Apply changes, which keeps
    // the user's checked cards and backend unchanged.
    runtimeCheck_->setChecked(
        !describeAnimeJanaiInstallation(selectedBackend(), false).installed ||
        runtimeCheck_->parentWidget()->property("recommended").toBool());
    kernelsCheck_->setChecked(
        kernelsCheck_->parentWidget()->property("recommended").toBool());
    spatialCheck_->setChecked(
        spatialCheck_->parentWidget()->property("recommended").toBool());
    bool rifeMissing = false;
    const RifeArchiveStore archive = rifeArchiveStore();
    for (const RifeModelPin& pin : rifeModelPins()) {
        if (!storedRifeGraph(archive, pin).installed) {
            rifeMissing = true;
            break;
        }
    }
    rifeCheck_->setChecked(rifeMissing);

    downloadMissing(false);
}

void ComponentStoreTab::downloadMissing(bool showReview) {
    rifeFailure_.clear();

    pendingModels_.clear();
    const QVector<StoredSpatialAsset> stored = storedSpatialAssets();
    const QVector<SpatialAsset> catalog = spatialAssetCatalog();
    if (selectedPacks_.contains(QStringLiteral("component:spatial"))) {
        for (int index = 0; index < catalog.size(); ++index) {
            const StoredSpatialAsset* row = spatialInventoryRow(stored, catalog.at(index).id);
            if (row == nullptr || !row->verified) pendingModels_.append(index);
        }
    }

    const Backend backend = selectedBackend();
    const AnimeJanaiInstallation animeJanai =
        describeAnimeJanaiInstallation(backend, false);
    const GpuComputeCapability& gpu = observeComputeCapability();
    const bool runtimeSelected =
        selectedPacks_.contains(QStringLiteral("component:runtime"));
    const bool kernelsSelected =
        selectedPacks_.contains(QStringLiteral("component:kernels"));
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
    if (selectedPacks_.contains(QStringLiteral("rife:archive"))) {
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
    ++runGeneration_;
    const quint64 generation = runGeneration_;
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
    runner_->run(
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
                pendingModels_.clear();
                applyStatus(QStringLiteral("native.components.cancelled"), QString());
            } else if (!value.toMap().value(QStringLiteral("ok")).toBool()) {
                const QString reason = value.toMap().value(QStringLiteral("reason")).toString();
                const QString reasonKey =
                    value.toMap().value(QStringLiteral("reasonKey")).toString();
                pendingModels_.clear();
                const QString backendName = backend == Backend::TensorRT
                                                ? tr("TensorRT / CUDA 13")
                                                : tr("DirectML");
                applyStatus(
                    reasonKey.isEmpty() ? QStringLiteral("native.components.fetch") : reasonKey,
                    tr("%1 installation failed: %2")
                        .arg(backendName,
                             reason.isEmpty()
                                 ? tr("AnimeJaNai runtime installation failed.")
                                 : reason));
            } else {
                verifiedAnimeJanaiBackends_.remove(static_cast<int>(backend));
                animeJanaiVerificationErrors_.remove(static_cast<int>(backend));
                refresh();
                downloadRemainingComponents();
            }
            refresh();
        },
        [this, alive = alive_, generation, cancelFlag, backend](const QString& error) {
            if (!alive->load() || runGeneration_ != generation) return;
            animeJanaiRuntimeDownloading_ = false;
            animeJanaiCancelFlag_.reset();
            pendingModels_.clear();
            if (cancelFlag->load()) {
                applyStatus(QStringLiteral("native.components.cancelled"), QString());
            } else {
                const QString backendName = backend == Backend::TensorRT
                                                ? tr("TensorRT / CUDA 13")
                                                : tr("DirectML");
                applyStatus(QStringLiteral("native.components.fetch"),
                            tr("%1 installation failed: %2").arg(backendName, error));
            }
            refresh();
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
    runner_->run(
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
        [this, alive = alive_, generation, cancelFlag, backend](const QString& error) {
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
                            tr("%1 installation failed: %2").arg(backendName, error));
            }
            refresh();
        });
}

void ComponentStoreTab::downloadRemainingComponents() {
    const ComponentBinDirs verdict =
        installedComponentBinDirs(observeComputeCapability(), componentStore());
    const bool componentMissing =
        selectedBackend() == Backend::TensorRT &&
        (selectedPacks_.contains(QStringLiteral("component:runtime")) ||
         selectedPacks_.contains(QStringLiteral("component:kernels"))) &&
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
    if (selectedPacks_.contains(QStringLiteral("rife:archive"))) {
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
    ++runGeneration_;
    const quint64 generation = runGeneration_;
    progress_->show();
    progress_->setRange(0, 100);
    progress_->setValue(0);
    status_->setText(tr("Downloading the RIFE 3.7.0 graph archive..."));
    updateControls();

    // The binder is taken on the GUI thread, before the work starts; the
    // worker only ever calls it.
    ProgressPublisher publish{runner_->post(), this, alive_, generation};
    runner_->run(
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
            applyStatus(QString(), tr("The RIFE 3.7.0 archive graphs are installed."));
            refresh();
            if (!cancelFlag->load()) downloadNextModel();
        },
        [this, alive = alive_, generation, cancelFlag](const QString& error) {
            if (!alive->load() || runGeneration_ != generation) return;
            rifeDownloading_ = false;
            rifeCancelFlag_.reset();
            if (cancelFlag->load()) {
                pendingModels_.clear();
                applyStatus(QStringLiteral("native.components.cancelled"), QString());
            } else {
                rifeFailure_ = error;
                pendingModels_.clear();
                applyStatus(QStringLiteral("native.components.fetch"), error);
                updateControls();
            }
            refresh();
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
            selectedPacks_.contains(QStringLiteral("component:runtime"));
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
        if ((selectedPacks_.contains(QStringLiteral("component:runtime")) ||
             selectedPacks_.contains(QStringLiteral("component:kernels"))) &&
            selectedBackend() == Backend::TensorRT && gpu.smMajor > 0 &&
            !installedComponentBinDirs(gpu, componentStore()).reasonKey.isEmpty()) {
            applyStatus(QStringLiteral("native.components.missing"),
                        tr("The requested TensorRT component pack is not installed."));
            refresh();
            return;
        }
        const RifeArchiveStore archiveStore = rifeArchiveStore();
        if (selectedPacks_.contains(QStringLiteral("rife:archive"))) {
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
        if (selectedPacks_.contains(QStringLiteral("component:spatial"))) {
            const QVector<StoredSpatialAsset> stored = storedSpatialAssets();
            for (const SpatialAsset& asset : spatialAssetCatalog()) {
                const StoredSpatialAsset* row = spatialInventoryRow(stored, asset.id);
                if (row == nullptr || !row->verified) {
                    applyStatus(QStringLiteral("native.components.digest"),
                                row == nullptr
                                    ? tr("The requested upscaling model is not installed: %1")
                                          .arg(asset.name)
                                    : QStringLiteral("%1: %2").arg(asset.name, row->reason));
                    refresh();
                    return;
                }
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
    ++runGeneration_;
    const quint64 generation = runGeneration_;
    progress_->show();
    progress_->setRange(0, 0);
    status_->setText(tr("Downloading %1...").arg(asset.name));
    updateControls();

    backend::MikaiClient* client = client_;
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
        [this, alive = alive_, generation, cancelFlag](const QVariant&) {
            if (!alive->load() || runGeneration_ != generation) return;
            if (cancelFlag->load()) {
                modelsDownloading_ = false;
                modelCancelFlag_.reset();
                pendingModels_.clear();
                applyStatus(QStringLiteral("native.components.cancelled"), QString());
                refresh();
                return;
            }
            downloadNextModel();
        },
        [this, alive = alive_, generation, cancelFlag](const QString& error) {
            if (!alive->load() || runGeneration_ != generation) return;
            modelsDownloading_ = false;
            modelCancelFlag_.reset();
            pendingModels_.clear();
            applyStatus(cancelFlag->load()
                            ? QStringLiteral("native.components.cancelled")
                            : QStringLiteral("native.components.fetch"),
                        cancelFlag->load() ? QString() : error);
            refresh();
        });
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
                      modelsDownloading_ || verifying_;
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
