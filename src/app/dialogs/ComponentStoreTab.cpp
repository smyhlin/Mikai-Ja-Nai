#include "dialogs/ComponentStoreTabInternal.hpp"
#include "state/SettingsKeys.hpp"
#include "ComponentStoreTab.hpp"

#include "dialogs/settings/SettingsUi.hpp"
#include "state/NativeComponentProvisioner.hpp"
#include "workers/Runner.hpp"
#include "theme/Metrics.hpp"
#include "theme/Theme.hpp"
#include "widgets/ActionGrid.hpp"

#include <backend/MikaiClient.hpp>
#include <backend/Cancellation.hpp>

#include <native/bundle/NativeBundle.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>
#include <native/components/AnimeJanaiRuntime.hpp>
#include <native/components/NativeComponentCatalog.hpp>
#include <native/components/NativeComponentStore.hpp>
#include <native/components/NativeComponentToolchain.hpp>
#include <native/components/NativeComponentProvisioning.hpp>
#include <native/components/NativeGpuProbe.hpp>
#include <native/models/NativeRife.hpp>
#include <native/models/NativeUpscalers.hpp>
#include <native/models/NativeSpatialAssets.hpp>
#include <shaders/Anime4K.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QEvent>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
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

namespace mikai::app {

namespace native = mplayer::native;


namespace component_store_detail {
/// The component decision for the first installed Native bundle this build can
/// find (the loaded selection when it is Native, otherwise discovery). Lives
/// here rather than beside `componentBinDirs` on purpose: discovery reaches
/// `NativeRuntimeSelection`, which reaches the mpv loader, and the domain module
/// must stay linkable without the player - the probe that links it alone has no
/// `MpvApi` to give. Empty directories and no reasonKey when no such bundle
/// exists; creates nothing, so it is safe on every repaint.
std::optional<native::Bundle> installedNativeBundle() {
    const native::RuntimeSelection& selection = native::currentSelection();
    if (selection.loaded && selection.kind == native::BundleKind::Native && !selection.root.isEmpty()) {
        if (selection.resolvedBundle.has_value()) return selection.resolvedBundle;
    }
    for (const native::Bundle& candidate : native::discoverBundles()) {
        if (candidate.kind == native::BundleKind::Native) return candidate;
    }
    return std::nullopt;
}

native::ComponentBinDirs installedComponentBinDirs(const native::GpuComputeCapability& gpu,
                                           const native::ComponentStore& store) {
    const std::optional<native::Bundle> bundle = installedNativeBundle();
    if (!bundle.has_value() || !bundle->preparation.has_value()) {
        // A machine whose bundle predates the preparation block can still be
        // served by the two store packs alone. `componentBinDirs` resolves that
        // store-only path, so do not answer "no directories" before it runs:
        // that early return is what let a stored closure look uninstalled.
        if (gpu.smMajor <= 0) return native::ComponentBinDirs{};
        return native::componentBinDirs(QString(), QString(), QString(), {}, gpu, store);
    }

    const QString& bundleRoot = bundle->root;
    const native::BundlePreparation& preparation = *bundle->preparation;
    QStringList declaredTrtFiles;
    const QString prefix = preparation.runtime.trt + QLatin1Char('/');
    for (const native::BundleFile& file : bundle->files) {
        if (file.path.startsWith(prefix)) {
            declaredTrtFiles.append(file.path);
        }
    }
    return native::componentBinDirs(bundleRoot, preparation.runtime.trt, preparation.runtime.cuda,
                            declaredTrtFiles, gpu, store);
}


const native::StoredSpatialAsset* spatialInventoryRow(const QVector<native::StoredSpatialAsset>& stored,
                                             const QString& id) {
    for (const native::StoredSpatialAsset& row : stored) {
        if (row.asset.id == id) return &row;
    }
    return nullptr;
}

QVariantMap inventorySnapshot(const native::GpuComputeCapability& gpu, bool verifyRife,
                              bool rehashBackend, const std::atomic<bool>* cancelled) {
    const auto isCancelled = [cancelled] {
        return (cancelled != nullptr && cancelled->load()) ||
               backend::WorkCancellationScope::cancellationRequested();
    };
    QVariantMap result;
    if (isCancelled()) {
        result.insert(QStringLiteral("cancelled"), true);
        return result;
    }
    const native::ComponentStore store = native::componentStore();
    const native::RifeArchiveStore rifeStore = native::rifeArchiveStore();
    const native::ComponentPack* runtimePack = native::tensorRtRuntimePack();
    const native::ComponentPack* kernelsPack = tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
    const native::StoredPack runtimeStored = runtimePack != nullptr ? native::storedPack(store, *runtimePack)
                                                             : native::StoredPack{};
    const native::StoredPack kernelsStored = kernelsPack != nullptr ? native::storedPack(store, *kernelsPack)
                                                             : native::StoredPack{};
    const native::AnimeJanaiInstallation trt =
        native::describeAnimeJanaiInstallation(native::Backend::TensorRT, rehashBackend);
    const native::AnimeJanaiInstallation directMl =
        native::describeAnimeJanaiInstallation(native::Backend::DirectML, rehashBackend);
    // Its own record under its own backend directory: an RTX answer is never
    // derived from the classic or DirectML records. On a platform without the
    // RTX binary this reports the named refusal instead of an installed state.
    const native::AnimeJanaiInstallation rtx =
        native::describeAnimeJanaiInstallation(native::Backend::TensorRTRTX, rehashBackend);
    const native::ComponentBinDirs dirs = installedComponentBinDirs(gpu, store);
    const std::optional<native::Bundle> bundle = installedNativeBundle();
    const native::TensorRtClosureStatus closure = native::tensorRtClosureStatus(
        store, bundle.has_value() ? &*bundle : nullptr, gpu.smMajor, gpu.smMinor);
    // One predicate: the closure is usable when the store holds both packs or
    // the installed bundle already declares this GPU's builder resource.
    const bool componentDirsAvailable = closure.dirsAvailable;
    const bool runtimeInstalled = trt.installed &&
                                  (runtimeStored.installed || componentDirsAvailable);
    const bool kernelsInstalled =
        native::gpuResourceInstalledForThisDevice(kernelsPack, kernelsStored);
    result.insert(QStringLiteral("trtInstalled"), trt.installed);
    result.insert(QStringLiteral("trtReasonKey"), trt.reasonKey);
    result.insert(QStringLiteral("trtReason"), trt.reason);
    result.insert(QStringLiteral("directMlInstalled"), directMl.installed);
    result.insert(QStringLiteral("directMlReasonKey"), directMl.reasonKey);
    result.insert(QStringLiteral("directMlReason"), directMl.reason);
    result.insert(QStringLiteral("rtxInstalled"), rtx.installed);
    result.insert(QStringLiteral("rtxReason"), rtx.reason);
    result.insert(QStringLiteral("runtimePackInstalled"), runtimeStored.installed);
    result.insert(QStringLiteral("kernelsPackInstalled"), kernelsStored.installed);
    result.insert(QStringLiteral("componentDirsAvailable"), componentDirsAvailable);
    result.insert(QStringLiteral("closureRuntimeStored"), closure.runtimeStored);
    result.insert(QStringLiteral("closureKernelsStored"), closure.kernelsStored);
    result.insert(QStringLiteral("closureBundleDirs"), closure.bundleProvidesDirs);
    result.insert(QStringLiteral("closureMissing"), closure.missing);
    result.insert(QStringLiteral("kernelsName"),
                  kernelsPack != nullptr ? kernelsPack->name : QString());
    result.insert(QStringLiteral("runtimeRecommended"), gpu.nvidia && !runtimeInstalled);
    result.insert(QStringLiteral("kernelsRecommended"),
                  gpu.nvidia && kernelsPack != nullptr && !kernelsInstalled);
    result.insert(QStringLiteral("tensorRtClosureBytes"), native::animeJanaiMissingDownloadBytes(native::Backend::TensorRT));
    result.insert(QStringLiteral("directMlClosureBytes"), native::animeJanaiMissingDownloadBytes(native::Backend::DirectML));
    // The dispatcher plus NVIDIA's overlay archive only: no classic TensorRT
    // pack, CUDA pack or builder resource belongs to this queue.
    result.insert(QStringLiteral("rtxClosureBytes"),
                  native::animeJanaiMissingDownloadBytes(native::Backend::TensorRTRTX));
    result.insert(QStringLiteral("componentReasonKey"), dirs.reasonKey);
    result.insert(QStringLiteral("componentReason"), dirs.reason);
    QVariantList componentFailures;
    if (rehashBackend) {
        for (const native::ComponentPack* pack : native::compatiblePacksFor(gpu.smMajor, gpu.smMinor)) {
            if (isCancelled()) {
                result.insert(QStringLiteral("cancelled"), true);
                return result;
            }
            if (native::storedPack(store, *pack).installed && !native::verifyPack(store, *pack).installed)
                componentFailures.append(pack->name);
        }
    }
    result.insert(QStringLiteral("componentFailures"), componentFailures);

    bool rifeInstalled = !native::rifeModelPins().isEmpty();
    bool rifeVerified = true;
    QVariantList rifeFailures;
    for (const native::RifeModelPin& pin : native::rifeModelPins()) {
        if (isCancelled()) {
            result.insert(QStringLiteral("cancelled"), true);
            return result;
        }
        const native::StoredRifeGraph stored = native::storedRifeGraph(rifeStore, pin);
        rifeInstalled = rifeInstalled && stored.installed;
        if (!verifyRife) continue;
        if (!stored.installed) {
            rifeVerified = false;
            continue;
        }
        const native::StoredRifeGraph graph = native::verifyRifeGraph(rifeStore, pin);
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
    for (const native::StoredSpatialAsset& row : native::storedSpatialAssets()) {
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

QStringList tensorRtClosureFailureLines(const native::TensorRtClosureStatus& closure,
                                        bool runtimeSelected, bool kernelsSelected,
                                        const QString& kernelsName) {
    QStringList failures;
    if (!runtimeSelected && !kernelsSelected) return failures;
    const auto translated = [](const char* message) {
        return QCoreApplication::translate("mikai::app::ComponentStoreTab", message);
    };
    const QString runtimeMissing = translated(
        QT_TRANSLATE_NOOP("ComponentStoreTab", "TensorRT environment (trt-runtime) is not installed — select it and apply changes."));
    const QString kernelsMissing =
        translated(QT_TRANSLATE_NOOP("ComponentStoreTab", "GPU kernels for this device (%1) are not installed."))
            .arg(kernelsName.isEmpty() ? QStringLiteral("trt-sm") : kernelsName);
    const auto appendUnique = [&failures](const QString& line) {
        if (!line.isEmpty() && !failures.contains(line)) failures.append(line);
    };
    // Each selected element answers for itself; the runtime prerequisite is
    // reported separately instead of one generic orphan sentence.
    if (runtimeSelected && !closure.runtimeStored && !closure.bundleProvidesDirs)
        appendUnique(runtimeMissing);
    if (runtimeSelected && !closure.dirsAvailable && closure.runtimeStored &&
        !closure.kernelsStored) {
        appendUnique(kernelsMissing);
    }
    if (kernelsSelected && !closure.kernelsStored && !closure.bundleProvidesDirs)
        appendUnique(kernelsMissing);
    if (kernelsSelected && !closure.runtimeStored && !closure.bundleProvidesDirs)
        appendUnique(runtimeMissing);
    return failures;
}

}  // namespace component_store_detail


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
    // button is the viewer's stop. The provisioner is borrowed: a window that
    // tears its service down first leaves a null weak pointer here, so this
    // stop reaches a live provisioner or nothing at all.
    if (alive_) alive_->store(false);
    bumpRunGeneration(QStringLiteral("destruction"));
    if (provisioner_ != nullptr) provisioner_->cancel();
    if (inventoryCancelFlag_) inventoryCancelFlag_->store(true);
    if (acquisitionCheckCancelFlag_) acquisitionCheckCancelFlag_->store(true);
    if (presenceCancelFlag_) presenceCancelFlag_->store(true);
}

void ComponentStoreTab::bumpRunGeneration(const QString& reason) {
    // One invalidation path. Every queued delivery compares the generation it
    // captured, so raising it once is what makes a stale progress tick, settle
    // or verification result a no-op instead of a render.
    //
    // An inventory snapshot in flight is cancelled by the same bump. That is
    // correct for the discarded delivery but not for the state it would have
    // published: a setter that invalidates the only scan (client and
    // provisioner replacement both bump) must not leave the tab unable to
    // report the store, so the scan is re-requested once the caller returns.
    // Deferring coalesces a burst of setters into one scan, and
    // `scheduleInventoryRefresh` itself skips a request while one is running.
    const bool inventoryScanInterrupted = inventoryScanning_;
    ++runGeneration_;
    ++presenceGeneration_;
    // (a) cancel pending units.
    if (animeJanaiCancelFlag_) animeJanaiCancelFlag_->store(true);
    if (rifeCancelFlag_) rifeCancelFlag_->store(true);
    if (modelCancelFlag_) modelCancelFlag_->store(true);
    if (acquisitionCheckCancelFlag_) acquisitionCheckCancelFlag_->store(true);
    // (b) reset every in-flight latch, so a bump between a progress delivery
    // and its settling continuation cannot leave the controls disabled.
    acquisitionUnitsPending_ = 0;
    acquisitionSequenceActive_ = false;
    acquisitionGeneration_ = 0;
    acquisitionCancelled_ = false;
    acquisitionFailures_.clear();
    acquisitionFailureKey_.clear();
    acquisitionChecking_ = false;
    modelsDownloading_ = false;
    modelDownloadsInFlight_ = 0;
    animeJanaiRuntimeDownloading_ = false;
    rifeDownloading_ = false;
    verifying_ = false;
    verificationActive_ = false;
    pendingModels_.clear();
    // A continuation that is waiting for an inventory snapshot would otherwise
    // start a run the user no longer asked for, under a generation nobody owns.
    pendingBackendWork_ = false;
    pendingRuntimeClosure_ = false;
    pendingRifeArchive_ = false;
    componentStagePending_ = false;
    recommendedInstallPending_ = false;
    if (inventoryCancelFlag_) inventoryCancelFlag_->store(true);
    ++inventoryGeneration_;
    inventoryScanning_ = false;
    // The stale presence callback is discarded by generation, so its own
    // "scan finished" reset would never run; release the coalescing latch here.
    presenceScanning_ = false;
    presenceRefreshPending_ = false;
    presenceCancelFlag_.reset();
    // (c) a busy line describes a run that no longer exists.
    if (statusReason_.kind == StatusReason::Kind::Busy ||
        statusReason_.kind == StatusReason::Kind::VerificationProgress ||
        statusReason_.kind == StatusReason::Kind::InventoryCounts ||
        statusReason_.kind == StatusReason::Kind::FailureCounts) {
        applyStatus(QString(), StatusReason{});
    }
    qInfo().noquote() << "native components: new run generation:" << reason;
    // (d) re-render from the reset axes. A tab already being destroyed has no
    // surface left to repaint and no announcement to make.
    if (alive_ != nullptr && alive_->load()) {
        renderCards();
        renderAllCardProgress();
        updateControls();
    }
    if (inventoryScanInterrupted) {
        QTimer::singleShot(0, this, [this] {
            if (alive_ == nullptr || !alive_->load()) return;
            scheduleInventoryRefresh();
        });
    }
}

void ComponentStoreTab::schedulePresenceRefresh() {
    // Presence, not proof: `inventorySnapshot` with hashing disabled still
    // reads bounded metadata off the GUI thread. It runs during an acquisition
    // so the spatial badge counts up 1/5 ... 5/5 instead of jumping at the end.
    if (runner_ == nullptr) return;
    if (presenceScanning_) {
        // A unit settled while a presence scan is in flight: run one more when
        // it lands instead of dropping the newer disk state.
        presenceRefreshPending_ = true;
        return;
    }
    presenceScanning_ = true;
    presenceRefreshPending_ = false;
    const quint64 generation = ++presenceGeneration_;
    const auto cancelled = std::make_shared<std::atomic<bool>>(false);
    presenceCancelFlag_ = cancelled;
    const native::GpuComputeCapability gpu = native::observeComputeCapability();
    const std::shared_ptr<std::atomic<bool>> live = alive_;
    runner_->runTransfer(
        [gpu, cancelled] {
            return component_store_detail::inventorySnapshot(gpu, false, false, cancelled.get());
        },
        [this, live, generation, cancelled](const QVariant& value) {
            if (!live->load() || generation != presenceGeneration_) return;
            presenceScanning_ = false;
            presenceCancelFlag_.reset();
            if (!cancelled->load()) applyInventorySnapshot(value);
            if (presenceRefreshPending_) {
                presenceRefreshPending_ = false;
                schedulePresenceRefresh();
            }
        },
        [this, live, generation](const backend::Failure&) {
            if (!live->load() || generation != presenceGeneration_) return;
            presenceScanning_ = false;
            presenceCancelFlag_.reset();
        });
}

QString ComponentStoreTab::setupPrompt(qint64* bytes) const {
    if (bytes != nullptr) *bytes = 0;
    if (!inventoryReady_) return {};
    const native::GpuComputeCapability& gpu = native::observeComputeCapability();
    QStringList missing;
    qint64 totalBytes = 0;
    auto append = [&](const QString& title, qint64 size) {
        missing.append(QStringLiteral("<li>%1 — %2</li>")
                           .arg(title.toHtmlEscaped(),
                                QLocale().formattedDataSize(size).toHtmlEscaped()));
        totalBytes += size;
    };

    // Match the upscaling-only recommendation, not every absent store pack:
    // an installed bundle can already supply the GPU components. The selected
    // product decides whose closure is quoted and at what size. On RTX GPUs,
    // always target the RTX runtime unless classic TensorRT is already installed.
    const native::Backend promptBackend =
        (native::isNvidiaRtx(gpu) && !animeJanaiTensorRtInstalled_)
            ? native::Backend::TensorRTRTX
            : selectedBackend();

    if (promptBackend == native::Backend::TensorRTRTX) {
        const qint64 rtxBytes = tensorRtRtxClosureBytes_ > 0
                                    ? tensorRtRtxClosureBytes_
                                    : native::animeJanaiMissingDownloadBytes(native::Backend::TensorRTRTX);
        if (!animeJanaiTensorRtRtxInstalled_ && rtxBytes > 0)
            append(tr("TensorRT for RTX runtime"), rtxBytes);
    } else if (gpu.nvidia) {
        if ((runtimeRecommended_ || kernelsRecommended_) && tensorRtClosureBytes_ > 0) {
            append(tr("Upscaling runtime and GPU components"), tensorRtClosureBytes_);
        }
#ifdef Q_OS_WIN
    } else if (!animeJanaiDirectMlInstalled_ && directMlClosureBytes_ > 0) {
        append(tr("Upscaling runtime and GPU components"), directMlClosureBytes_);
#endif
    }
    qint64 missingSpatialBytes = 0;
    int missingSpatialCount = 0;
    const QVector<native::SpatialAsset> spatialCatalog = native::spatialAssetCatalog();
    for (const native::SpatialAsset& asset : spatialCatalog) {
        const native::StoredSpatialAsset* row = component_store_detail::spatialInventoryRow(storedSpatial_, asset.id);
        if (row == nullptr || !row->verified) {
            missingSpatialBytes += asset.bytes;
            ++missingSpatialCount;
        }
    }
    if (missingSpatialCount > 0)
        append(tr("AnimeJaNai spatial models (%1 of %2 missing)")
                   .arg(missingSpatialCount).arg(spatialCatalog.size()),
               missingSpatialBytes);
    if (bytes != nullptr) *bytes = totalBytes;
    if (missing.isEmpty()) return {};
    return tr("Anime4K shaders are included with the app. RIFE and other upscale models are available separately in Components.")
        + QStringLiteral("<br><br>")
        + tr("For AI upscaling, download these optional components. Normal playback works without them.")
        + QStringLiteral("<ul>") + missing.join(QString()) + QStringLiteral("</ul>")
        + tr("Download these optional components (%1)?")
              .arg(QLocale().formattedDataSize(totalBytes));
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
    // A combo reserves its widest entry by default; the page keeps the row and
    // the selected entry elides into the closed field instead.
    animeJanaiBackendCombo_->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    animeJanaiBackendCombo_->setMinimumContentsLength(theme::kSettingsCompactChoiceLengthChars);
    QSizePolicy backendPolicy = animeJanaiBackendCombo_->sizePolicy();
    backendPolicy.setHorizontalPolicy(QSizePolicy::Ignored);
    animeJanaiBackendCombo_->setSizePolicy(backendPolicy);
    animeJanaiBackendCombo_->setAccessibleName(tr("AnimeJaNai backend used by Install selected and Verify"));
    animeJanaiBackendCombo_->addItem(backendChoiceLabel(native::Backend::TensorRT),
                                     static_cast<int>(native::Backend::TensorRT));
#ifdef Q_OS_WIN
    animeJanaiBackendCombo_->addItem(backendChoiceLabel(native::Backend::DirectML),
                                     static_cast<int>(native::Backend::DirectML));
    // Offered before it is installed: acquiring the closure is what this tab
    // is for. Windows-only, because no RTX binary is published for Linux.
    animeJanaiBackendCombo_->addItem(backendChoiceLabel(native::Backend::TensorRTRTX),
                                     static_cast<int>(native::Backend::TensorRTRTX));
#endif
    const auto& gpu = native::observeComputeCapability();
    // A restored selection is shown, but on an RTX GPU an uninstalled classic
    // TensorRT selection is automatically promoted to the hardware-matched RTX runtime.
    const auto restoredBackend = [&]() -> std::optional<native::Backend> {
        QSettings settings;
        auto b = native::backendFromName(
            settings.value(QString::fromUtf8(kAnimeJanaiBackend)).toString());
        if (native::isNvidiaRtx(gpu) && (!b.has_value() || (*b == native::Backend::TensorRT && !animeJanaiTensorRtInstalled_))) {
            b = native::Backend::TensorRTRTX;
            settings.setValue(QString::fromUtf8(kAnimeJanaiBackend),
                              mplayer::native::stableName(native::Backend::TensorRTRTX));
        }
        return b;
    }();
    if (restoredBackend.has_value()) {
        int restoredIndex = animeJanaiBackendCombo_->findData(
            static_cast<int>(*restoredBackend));
#ifndef Q_OS_WIN
        if (*restoredBackend == native::Backend::TensorRTRTX) {
            // Linux has no RTX closure. Keep the choice visible with its own
            // unavailable wording instead of clamping it to classic TensorRT.
            animeJanaiBackendCombo_->addItem(backendChoiceLabel(native::Backend::TensorRTRTX),
                                             static_cast<int>(native::Backend::TensorRTRTX));
            restoredIndex = animeJanaiBackendCombo_->count() - 1;
        }
#endif
        if (restoredIndex >= 0) animeJanaiBackendCombo_->setCurrentIndex(restoredIndex);
    } else {
        const native::Backend defBackend = native::defaultAnimeJanaiBackend(gpu);
        int defIndex = animeJanaiBackendCombo_->findData(static_cast<int>(defBackend));
        if (defIndex >= 0) animeJanaiBackendCombo_->setCurrentIndex(defIndex);
    }
    animeJanaiBackendLabel_->setBuddy(animeJanaiBackendCombo_);
    backendRow->addWidget(animeJanaiBackendCombo_, 1, Qt::AlignVCenter);
    headerLayout->addLayout(backendRow);

    root->addWidget(headerCard);

    // The page's actions belong with the page's content, not stacked on top of
    // the window's Save/Cancel: one compact row directly under the backend
    // selector, with the selection context on its left where the card's check
    // column already sits.
    auto* actionBar = new QFrame(content);
    actionBar->setObjectName(QStringLiteral("componentActionBar"));
    auto* barLayout = new QHBoxLayout(actionBar);
    barLayout->setContentsMargins(theme::kSpacingMedium, 12, theme::kSpacingMedium, 12);
    barLayout->setSpacing(theme::kSpacingSmall);
    selectAllCheck_ = new QCheckBox(tr("Select all"), actionBar);
    selectAllCheck_->setObjectName(QStringLiteral("componentSelectAll"));
    selectAllCheck_->setTristate(true);
    barLayout->addWidget(selectAllCheck_, 0, Qt::AlignVCenter);
    selectionSummary_ = new QLabel(actionBar);
    selectionSummary_->setObjectName(QStringLiteral("componentSelectionSummary"));
    selectionSummary_->setProperty("textRole", QStringLiteral("muted"));
    barLayout->addWidget(selectionSummary_, 0, Qt::AlignVCenter);
    barLayout->addStretch(1);

    verifyButton_ = new QPushButton(actionBar);
    verifyButton_->setObjectName(QStringLiteral("nativeComponentVerifyButton"));
    // Verify reads the installed set; it is the tertiary action of the page.
    theme::applyActionRole(verifyButton_, theme::ActionRole::Ghost);
    verifyButton_->setFixedHeight(theme::kControlHeight);
    barLayout->addWidget(verifyButton_, 0, Qt::AlignVCenter);

    downloadButton_ = new QPushButton(actionBar);
    downloadButton_->setObjectName(QStringLiteral("componentDownloadButton"));
    // The page's one emphasised action. It dims to disabled while nothing is
    // selected, so "Install selected" can never be clicked with nothing chosen -
    // the footer's Save stays the only accent that is always actionable.
    theme::applyActionRole(downloadButton_, theme::ActionRole::Primary);
    downloadButton_->setFixedHeight(theme::kControlHeight);
    barLayout->addWidget(downloadButton_, 0, Qt::AlignVCenter);

    cancelButton_ = new QPushButton(actionBar);
    cancelButton_->setObjectName(QStringLiteral("nativeComponentTabCancelButton"));
    theme::applyActionRole(cancelButton_, theme::ActionRole::Ghost);
    cancelButton_->setFixedHeight(theme::kControlHeight);
    cancelButton_->setEnabled(false);
    cancelButton_->setVisible(false);
    barLayout->addWidget(cancelButton_, 0, Qt::AlignVCenter);
    root->addWidget(actionBar);

    // Created here, with the bar: updateControls() runs while the cards below are
    // still being built and publishes its state into this label.
    status_ = new QLabel(content);
    status_->setObjectName(QStringLiteral("nativeComponentStatus"));
    status_->setAccessibleName(tr("Components status"));
    status_->setWordWrap(true);
    root->addWidget(status_);

    connect(selectAllCheck_, &QCheckBox::clicked, this, [this](bool checked) {
        for (QGroupBox* card : findChildren<QGroupBox*>(QStringLiteral("nativeComponentPackCard"))) {
            const QString cardId = card->property("componentCardId").toString();
            auto* check = card->findChild<QCheckBox*>(cardId + QStringLiteral("Check"));
            if (check != nullptr && check->isEnabled()) check->setChecked(checked);
        }
        refreshSelectionSummary();
        updateControls();
    });

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
        (*checkBox)->setMinimumSize(theme::kComponentCheckColumn,
                                    theme::kComponentCheckColumn);
        cardLayout->addWidget(*checkBox, 0, Qt::AlignVCenter);
        auto* details = new QWidget(card);
        details->setObjectName(objectName + QStringLiteral("Details"));
        details->installEventFilter(this);
        auto* detailsLayout = new QVBoxLayout(details);
        detailsLayout->setContentsMargins(0, 0, 0, 0);
        detailsLayout->setSpacing(theme::kSpacingXs);
        auto* titleLine = new QHBoxLayout();
        titleLine->setObjectName(objectName + QStringLiteral("TitleLine"));
        titleLine->setSpacing(theme::kSpacingSmall);
        auto* title = new QLabel(details);
        title->setObjectName(objectName + QStringLiteral("Title"));
        // A heading wraps, but its longest word stays a floor: the card reflow
        // moves the badge and the size off this line before it can be squeezed
        // under a readable width.
        title->setWordWrap(true);
        title->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        title->installEventFilter(this);
        QFont titleFont = title->font();
        titleFont.setBold(true);
        title->setFont(titleFont);
        titleLine->addWidget(title, 1);
        detailsLayout->addLayout(titleLine);
        // The trailing row the reflow moves the state and the size onto; empty
        // (a bare stretch) while the heading shares its line with them.
        auto* stateLine = new QHBoxLayout();
        stateLine->setObjectName(objectName + QStringLiteral("StateLine"));
        stateLine->setSpacing(theme::kSpacingSmall);
        stateLine->addStretch(1);
        detailsLayout->addLayout(stateLine);
        *stateLabel = new QLabel(details);
        (*stateLabel)->setObjectName(objectName + QStringLiteral("State"));
        // One line, one width: a longer state elides into its tooltip rather
        // than growing a second line over the title.
        (*stateLabel)->setWordWrap(false);
        (*stateLabel)->setFixedWidth(theme::kComponentBadgeWidth);
        (*stateLabel)->setAlignment(Qt::AlignCenter);
        (*stateLabel)->installEventFilter(this);
        titleLine->addWidget(*stateLabel, 0, Qt::AlignVCenter);
        auto* description = new QLabel(details);
        description->setObjectName(objectName + QStringLiteral("Description"));
        description->setProperty("textRole", QStringLiteral("muted"));
        yieldCaptionWidth(description);
        description->installEventFilter(this);
        detailsLayout->addWidget(description);
        // The counts a badge used to carry live on their own line under the
        // description, where they cannot crowd the state.
        auto* counts = new QLabel(details);
        counts->setObjectName(objectName + QStringLiteral("Count"));
        counts->setProperty("textRole", QStringLiteral("muted"));
        yieldCaptionWidth(counts);
        counts->setMinimumWidth(0);
        counts->hide();
        detailsLayout->addWidget(counts);
        auto* cardProgress = new QProgressBar(details);
        cardProgress->setObjectName(objectName + QStringLiteral("Progress"));
        cardProgress->setRange(0, 100);
        cardProgress->setTextVisible(false);
        cardProgress->setMinimumWidth(0);
        cardProgress->hide();
        detailsLayout->addWidget(cardProgress);
        auto* cardProgressStatus = new QLabel(details);
        cardProgressStatus->setObjectName(objectName + QStringLiteral("ProgressStatus"));
        cardProgressStatus->setProperty("textRole", QStringLiteral("muted"));
        cardProgressStatus->setWordWrap(true);
        cardProgressStatus->setMinimumWidth(0);
        cardProgressStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        cardProgressStatus->hide();
        detailsLayout->addWidget(cardProgressStatus);
        cardProgressBars_.insert(objectName, cardProgress);
        cardProgressStatusLabels_.insert(objectName, cardProgressStatus);
        cardLayout->addWidget(details, 1);
        auto* size = new QLabel(details);
        size->setObjectName(objectName + QStringLiteral("Size"));
        size->setProperty("textRole", QStringLiteral("muted"));
        size->setWordWrap(false);
        size->setFixedWidth(theme::kComponentSizeWidth);
        size->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        size->installEventFilter(this);
        QFont sizeFont = size->font();
        sizeFont.setBold(true);
        size->setFont(sizeFont);
        titleLine->addWidget(size, 0, Qt::AlignVCenter);
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
    // The shaders are built into every artifact from one pinned upstream
    // commit, so this card reports an inventory and offers nothing to download.
    auto* anime4kCard = makeCard(QStringLiteral("anime4kShadersCard"),
                                 &anime4kCheck_, &anime4kState_);
    anime4kCheck_->hide();
    anime4kCheck_->setEnabled(false);
    // The optional ONNX upscalers are downloaded on their own page; this card is
    // the inventory line the download settings need to show for them.
    auto* upscalerCard = makeCard(QStringLiteral("upscalerModelsCard"),
                                  &upscalerCheck_, &upscalerState_);
    for (auto* card :
         {runtimeCard, spatialCard, rifeCard, kernelsCard, anime4kCard, upscalerCard}) {
        card->setProperty("componentCard", true);
    }
    auto setCardText = [](QGroupBox* card, const QString& titleText,
                          const QString& descriptionText) {
        const QString cardId = card->property("componentCardId").toString();
        card->findChild<QLabel*>(cardId + QStringLiteral("Title"))->setText(titleText);
        card->findChild<QLabel*>(cardId + QStringLiteral("Description"))
            ->setText(descriptionText);
    };
    setCardText(rifeCard, tr("RIFE interpolation models"),
                tr("Frame interpolation, not needed for upscale only."));
    setCardText(spatialCard, tr("AnimeJaNai upscaling models"),
                tr("All five pinned HD and SD graphs required by the built-in AnimeJaNai profiles."));
    setCardText(kernelsCard, tr("TensorRT kernels for this GPU"),
                tr("Engine-builder kernels matched to this GPU generation."));
    setCardText(anime4kCard, tr("Anime4K shaders"),
                tr("GLSL presets bundled with this build. No download is needed."));
    setCardText(upscalerCard, tr("Upscale models"),
                tr("Optional ONNX upscalers. Selected models download with Install selected; "
                   "the Upscalers page manages them one by one."));
    // The runtime card names the selected backend's product, not a fixed one.
    updateRuntimeCardText();
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
    connect(upscalerCheck_, &QCheckBox::toggled, this,
            [this, upscalerCard, syncCardSelectionStyle](bool checked) {
                if (checked) selectedPacks_.insert(QStringLiteral("upscaler:all"));
                else selectedPacks_.remove(QStringLiteral("upscaler:all"));
                syncCardSelectionStyle(upscalerCard, checked);
                updateControls();
            });

    root->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll);
    // The card's own width is clamped by its minimum, so the viewport is the
    // width a reader actually has; the header decisions use it.
    scrollViewport_ = scroll->viewport();
    scrollContent_ = content;
    scrollViewport_->installEventFilter(this);

    trackedBackend_ = animeJanaiBackendCombo_->currentData().toInt();
    connect(animeJanaiBackendCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index < 0) return;
                const QVariant data = animeJanaiBackendCombo_->itemData(index);
                if (!data.isValid()) return;
                const int backend = data.toInt();
                // Closing Settings reparents this tab, and showing it again can
                // emit this for the row that is already selected. That is not a
                // backend change: retiring the generation here would drop later
                // progress updates while the download ran on.
                if (backend == trackedBackend_) return;
                trackedBackend_ = backend;
                // The Components selection *is* the global engine: the player's
                // backend menu and the benchmark both read this key, so a choice
                // made here must not stay local to this tab. A product this
                // platform cannot install is left unpersisted - storing it would
                // switch playback into a backend with no closure.
                if (static_cast<native::Backend>(backend) != native::Backend::TensorRTRTX ||
                    tensorRtRtxOffered()) {
                    QSettings settings;
                    settings.setValue(QString::fromUtf8(kAnimeJanaiBackend),
                                      mplayer::native::stableName(static_cast<native::Backend>(backend)));
                }
                // A different backend answers a different question: any run still
                // in flight was hashing the previous one, so its results are
                // discarded instead of being rendered as fresh evidence. The
                // concurrent acquisition's callbacks carry the old generation
                // and are dropped, so its units are settled here instead of
                // leaving a run nobody can finish.
                bumpRunGeneration(QStringLiteral("backend changed"));
                // Closure rows belong to the backend they were queued for; a
                // switch must not leave the previous product's rows pretending
                // to describe this one.
                clearProgressCard(QStringLiteral("tensorRtRuntimeCard"));
                clearProgressCard(QStringLiteral("tensorRtKernelsCard"));
                refresh();
                // Selecting is not acquiring. The only thing a switch does
                // besides re-reading the store is name a choice this platform
                // cannot install, so the user is never left guessing.
                if (static_cast<native::Backend>(backend) == native::Backend::TensorRTRTX &&
                    !tensorRtRtxOffered()) {
                    applyStatus(QStringLiteral("native.components.rtxUnavailable"),
                                animeJanaiTensorRtRtxReason_.isEmpty()
                                    ? tr("TensorRT for RTX is available only on Windows.")
                                    : animeJanaiTensorRtRtxReason_);
                } else if (statusKey_ ==
                           QStringLiteral("native.components.rtxUnavailable")) {
                    // The refusal described a row that is no longer selected.
                    applyStatus(QString(), QString());
                }
            });
    connect(verifyButton_, &QPushButton::clicked, this, &ComponentStoreTab::verifyAll);
    connect(downloadButton_, &QPushButton::clicked, this,
            [this] { downloadMissing(false); });
    connect(cancelButton_, &QPushButton::clicked, this, &ComponentStoreTab::cancelAcquisition);

    retranslateUi();
}

void ComponentStoreTab::retranslateUi() {
    heading_->setText(tr("native::Backend components"));
    hint_->setText(tr("Choose which recommended components, AnimeJaNai upscaling models and optional RIFE models to install."));
    auto setCardText = [this](const QString& objectName, const QString& title,
                              const QString& description) {
        findChild<QLabel*>(objectName + QStringLiteral("Title"))->setText(title);
        findChild<QLabel*>(objectName + QStringLiteral("Description"))->setText(description);
    };
    // The runtime card carries the selected product's wording; asking the
    // backend keeps a language switch from restating another backend's text.
    updateRuntimeCardText();
    setCardText(QStringLiteral("rifeModelsCard"), tr("RIFE interpolation models"),
                tr("Frame interpolation, not needed for upscale only."));
    setCardText(QStringLiteral("animeJanaiModelsCard"), tr("AnimeJaNai upscaling models"),
                tr("All five pinned HD and SD graphs required by the built-in AnimeJaNai profiles."));
    setCardText(QStringLiteral("tensorRtKernelsCard"),
                tr("TensorRT kernels for this GPU"),
                tr("Engine-builder kernels matched to this GPU generation."));
    verifyButton_->setText(tr("Verify"));
    verifyButton_->setAccessibleName(tr("Re-check every installed element against its digest"));
    downloadButton_->setText(tr("Install selected"));
    downloadButton_->setAccessibleName(tr("Install the selected components"));
    renderAllCardProgress();
    cancelButton_->setText(tr("Cancel"));
    cancelButton_->setAccessibleName(tr("Cancel the running download"));
    status_->setAccessibleName(tr("Components status"));
    animeJanaiBackendCombo_->setAccessibleName(
        tr("AnimeJaNai backend used by Install selected and Verify"));
    // Each row is addressed by its enum value, so adding the RTX row cannot
    // shift another row's text onto the wrong product.
    for (int row = 0; row < animeJanaiBackendCombo_->count(); ++row) {
        animeJanaiBackendCombo_->setItemText(
            row, backendChoiceLabel(
                     static_cast<native::Backend>(animeJanaiBackendCombo_->itemData(row).toInt())));
    }
    if (animeJanaiBackendLabel_ != nullptr) {
        animeJanaiBackendLabel_->setText(tr("AnimeJaNai backend:"));
    }
    recommendationBanner_->setText(
        QStringLiteral("!  ") +
        tr("Components recommended for this PC are not installed. Check the highlighted items below, then click Install selected to install them."));
    reflowComponentHeaders();
}

bool ComponentStoreTab::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Resize) {
        auto* card = qobject_cast<QGroupBox*>(watched);
        const bool cardResized =
            card != nullptr && card->objectName() == QStringLiteral("nativeComponentPackCard");
        if (cardResized || watched == scrollViewport_) reflowComponentHeaders();
        return QWidget::eventFilter(watched, event);
    }
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

bool ComponentStoreTab::event(QEvent* event) {
    // QWidget::event() consumes both of these without forwarding them to
    // changeEvent(), so this override is the only place the tab is told the
    // ratio changed. The base handler runs first: it installs the device font
    // the equalised widths and the header measurement below are taken from.
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::DevicePixelRatioChange ||
        event->type() == QEvent::ScreenChangeInternal) {
        reflowComponentHeaders();
    }
    return handled;
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
    } else if (event->type() == QEvent::FontChange ||
               event->type() == QEvent::ApplicationFontChange ||
               event->type() == QEvent::StyleChange) {
        reflowComponentHeaders();
    }
}

void ComponentStoreTab::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    reflowComponentHeaders();
}

void ComponentStoreTab::reflowComponentHeaders() {
    // While the header is wide, the card's minimum is the widest arrangement, so
    // the scroll content never gets narrower than that and the card's own width
    // cannot report the space a reader has. The viewport is that space.
    int available = 0;
    if (scrollViewport_ != nullptr) {
        available = scrollViewport_->width();
        if (scrollContent_ != nullptr && scrollContent_->layout() != nullptr) {
            const QMargins margins = scrollContent_->layout()->contentsMargins();
            available -= margins.left() + margins.right();
        }
    }
    for (QGroupBox* card : findChildren<QGroupBox*>(QStringLiteral("nativeComponentPackCard"))) {
        const QString id = card->property("componentCardId").toString();
        auto* cardLayout = card->layout();
        auto* titleLine = card->findChild<QHBoxLayout*>(id + QStringLiteral("TitleLine"));
        auto* stateLine = card->findChild<QHBoxLayout*>(id + QStringLiteral("StateLine"));
        auto* title = card->findChild<QLabel*>(id + QStringLiteral("Title"));
        auto* badge = card->findChild<QLabel*>(id + QStringLiteral("State"));
        auto* size = card->findChild<QLabel*>(id + QStringLiteral("Size"));
        if (cardLayout == nullptr || titleLine == nullptr || stateLine == nullptr ||
            title == nullptr || badge == nullptr || size == nullptr) {
            continue;
        }
        // The heading keeps its line while it can still show its longest word
        // beside the state and the size; below that they take the second line so
        // the heading owns the width instead of collapsing under it.
        const QMargins margins = cardLayout->contentsMargins();
        const int shared = margins.left() + margins.right() + theme::kComponentCheckColumn +
                           cardLayout->spacing() + titleLine->spacing() * 2 +
                           theme::kComponentBadgeWidth + theme::kComponentSizeWidth;
        const bool compact = available <= 0 ||
                             qMin(card->width(), available) - shared <
                                 title->minimumSizeHint().width();
        if (card->property("compactHeader").toBool() == compact) continue;
        card->setProperty("compactHeader", compact);
        titleLine->removeWidget(badge);
        titleLine->removeWidget(size);
        stateLine->removeWidget(badge);
        stateLine->removeWidget(size);
        if (compact) {
            stateLine->addWidget(badge, 0, Qt::AlignVCenter);
            stateLine->addWidget(size, 0, Qt::AlignVCenter);
        } else {
            titleLine->addWidget(badge, 0, Qt::AlignVCenter);
            titleLine->addWidget(size, 0, Qt::AlignVCenter);
        }
        cardLayout->invalidate();
        if (QWidget* details = card->findChild<QWidget*>(id + QStringLiteral("Details"))) {
            details->updateGeometry();
        }
    }
}

void ComponentStoreTab::setRunner(Runner* runner) {
    // A different executor answers to a different lifetime: anything still in
    // flight from the previous one is discarded rather than rendered.
    const bool rebinding = runner_ != runner;
    const bool wasBound = runner_ != nullptr;
    if (rebinding) bumpRunGeneration(QStringLiteral("runner changed"));
    runner_ = runner;
    // The tab outlives the dialog that borrows the executor, so a scan posted
    // just before the previous dialog closed can end with its delivery dropped:
    // the latch it set would then keep the engine selector disabled for every
    // later open ("stuck, the list does not even open"). Binding an executor is
    // exactly the moment that latch can no longer belong to anything, so it is
    // released here whether or not the pointer changed.
    if (inventoryScanning_ && wasBound) {
        if (inventoryCancelFlag_) inventoryCancelFlag_->store(true);
        ++inventoryGeneration_;
        inventoryScanning_ = false;
    }
    updateControls();
    scheduleInventoryRefresh();
}

void ComponentStoreTab::setClient(backend::MikaiClient* client) {
    if (client_ != client) bumpRunGeneration(QStringLiteral("client changed"));
    client_ = client;
    updateControls();
}

void ComponentStoreTab::setComponentProvisioner(NativeComponentProvisioner* provisioner) {
    if (provisioner_ == provisioner) {
        updateControls();
        return;
    }
    bumpRunGeneration(QStringLiteral("provisioner changed"));
    ++provisionerBindingGeneration_;
    provisionerProgressSessionActive_ = false;
    provisionerProgressItemsSeen_.clear();
    if (provisioner_ != nullptr) disconnect(provisioner_, nullptr, this, nullptr);
    provisioner_ = provisioner;
    if (provisioner_ != nullptr) {
        const quint64 bindingGeneration = provisionerBindingGeneration_;
        connect(provisioner_, &NativeComponentProvisioner::progress, this,
                [this, bindingGeneration](const native::ComponentProvisionProgress& progress) {
                    if (bindingGeneration != provisionerBindingGeneration_) return;
                    if (!provisionerProgressSessionActive_) {
                        provisionerProgressSessionActive_ = true;
                        provisionerProgressItemsSeen_.clear();
                    }
                    if (!acquisitionSequenceActive_ &&
                        !provisionerProgressItemsSeen_.contains(progress.pack)) {
                        clearProgressItem(progress.pack);
                        provisionerProgressItemsSeen_.insert(progress.pack);
                    }
                    applyProvisionProgress(progress);
                });
        connect(provisioner_, &NativeComponentProvisioner::finished, this,
                [this, bindingGeneration](bool ok, bool cancelled, const QString& summary) {
                    if (bindingGeneration != provisionerBindingGeneration_) return;
                    provisionerProgressSessionActive_ = false;
                    provisionerProgressItemsSeen_.clear();
                    settleProgressCard(QStringLiteral("tensorRtRuntimeCard"), ok, cancelled,
                                       summary);
                    settleProgressCard(QStringLiteral("tensorRtKernelsCard"), ok, cancelled,
                                       summary);
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
        gpuLabel_->setText(tr("GPU: %1").arg(native::computeCapabilityText(native::observeComputeCapability())));
    }
    updateCardStates();
    updateControls();
    scheduleInventoryRefresh();
}

void ComponentStoreTab::scheduleInventoryRefresh() {
    if (runner_ == nullptr || inventoryScanning_ || downloadActive() || verifyActive()) return;
    inventoryScanning_ = true;
    const bool acquisitionInventory = componentStagePending_;
    if (acquisitionInventory) {
        acquisitionSequenceActive_ = true;
        updateControls();
    }
    const quint64 generation = ++inventoryGeneration_;
    const auto cancelled = std::make_shared<std::atomic<bool>>(false);
    inventoryCancelFlag_ = cancelled;
    const native::GpuComputeCapability gpu = native::observeComputeCapability();
    const std::shared_ptr<std::atomic<bool>> live = alive_;
    runner_->runTransfer(
        [gpu, cancelled] {
            return component_store_detail::inventorySnapshot(gpu, false, false, cancelled.get());
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
        native::StoredSpatialAsset stored;
        stored.asset.id = row.value(QStringLiteral("id")).toString();
        stored.graphPath = row.value(QStringLiteral("path")).toString();
        stored.verified = row.value(QStringLiteral("verified")).toBool();
        stored.reasonKey = row.value(QStringLiteral("reasonKey")).toString();
        stored.reason = row.value(QStringLiteral("reason")).toString();
        if (const native::SpatialAsset* asset = native::findSpatialAsset(stored.asset.id)) stored.asset = *asset;
        storedSpatial_.append(std::move(stored));
    }
    animeJanaiTensorRtInstalled_ = result.value(QStringLiteral("trtInstalled")).toBool();
    animeJanaiDirectMlInstalled_ = result.value(QStringLiteral("directMlInstalled")).toBool();
    animeJanaiTensorRtRtxInstalled_ = result.value(QStringLiteral("rtxInstalled")).toBool();
    animeJanaiTensorRtRtxReason_ = result.value(QStringLiteral("rtxReason")).toString();
    tensorRtRtxClosureBytes_ = result.value(QStringLiteral("rtxClosureBytes")).toLongLong();
    componentDirsAvailable_ = result.value(QStringLiteral("componentDirsAvailable")).toBool();
    runtimePackInstalled_ = result.value(QStringLiteral("runtimePackInstalled")).toBool();
    kernelsPackInstalled_ = result.value(QStringLiteral("kernelsPackInstalled")).toBool();
    rifeInstalled_ = result.value(QStringLiteral("rifeInstalled")).toBool();
    rifeVerified_ = result.value(QStringLiteral("rifeVerified")).toBool();
    installedSpatialCount_ = 0;
    verifiedSpatialCount_ = 0;
    for (const native::SpatialAsset& asset : native::spatialAssetCatalog()) {
        const native::StoredSpatialAsset* row = component_store_detail::spatialInventoryRow(storedSpatial_, asset.id);
        if (row == nullptr) continue;
        if (QFileInfo(row->graphPath).isFile()) ++installedSpatialCount_;
        if (row->verified) ++verifiedSpatialCount_;
    }
    tensorRtClosureBytes_ = result.value(QStringLiteral("tensorRtClosureBytes")).toLongLong();
    directMlClosureBytes_ = result.value(QStringLiteral("directMlClosureBytes")).toLongLong();
    runtimeRecommended_ = result.value(QStringLiteral("runtimeRecommended")).toBool();
    kernelsRecommended_ = result.value(QStringLiteral("kernelsRecommended")).toBool();
    spatialRecommended_ = false;
    for (const native::SpatialAsset& asset : native::spatialAssetCatalog()) {
        const native::StoredSpatialAsset* row = component_store_detail::spatialInventoryRow(storedSpatial_, asset.id);
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
    // Disk is the authority a card reports from: drop every terminal claim the
    // fresh inventory no longer confirms, then re-render the bars through the
    // same decision function every other path uses.
    reconcileProgressWithInventory();
    updateCardStates();
    renderAllCardProgress();
    updateControls();
    emit inventoryUpdated();
}

int ComponentStoreTab::modelInstalledCount() const { return installedSpatialCount_; }

int ComponentStoreTab::modelVerifiedCount() const { return verifiedSpatialCount_; }

int ComponentStoreTab::downloadedModelItemCount() const {
    int downloaded = 0;
    for (auto item = progressItems_.cbegin(); item != progressItems_.cend(); ++item) {
        if (item->cardId != QLatin1String("animeJanaiModelsCard")) continue;
        if (item->outcome != ProgressItem::Outcome::Installed) continue;
        if (native::findSpatialAsset(item.key()) != nullptr) ++downloaded;
    }
    return downloaded;
}

bool ComponentStoreTab::runtimeClosureInstalled() const {
    // Each backend answers from its own record; RTX never borrows a classic
    // pack's presence and is never satisfied by a DirectML closure. A stored
    // runtime pack is a download, not readiness: on an NVIDIA GPU the classic
    // card needs both the pack and the backend's own verified closure, which is
    // what makes a half-provisioned store read as "not installed" everywhere
    // instead of "installed" in the progress line and "not installed" in the
    // badge at the same time.
    const native::GpuComputeCapability& gpu = native::observeComputeCapability();
    switch (selectedBackend()) {
        case native::Backend::TensorRT:
            return gpu.nvidia ? animeJanaiTensorRtInstalled_ &&
                                    (runtimePackInstalled_ || componentDirsAvailable_)
                              : animeJanaiTensorRtInstalled_;
        case native::Backend::DirectML:
            return animeJanaiDirectMlInstalled_;
        case native::Backend::TensorRTRTX:
            return animeJanaiTensorRtRtxInstalled_;
    }
    return false;
}

int ComponentStoreTab::runtimeCardInstalledOnDisk() const {
    return runtimeClosureInstalled() ? 1 : 0;
}

void ComponentStoreTab::reconcileProgressWithInventory() {
    if (!inventoryReady_) return;
    const auto eraseInstalledWhere = [this](const QString& cardId, const auto& stale) {
        for (auto item = progressItems_.begin(); item != progressItems_.end();) {
            if (item->cardId == cardId &&
                item->outcome == ProgressItem::Outcome::Installed && stale(item.key())) {
                currentProgressItems_.remove(item.key());
                item = progressItems_.erase(item);
            } else {
                ++item;
            }
        }
    };

    // Spatial models are separate files, so presence is answered per identity:
    // a deleted model must not hide behind four unrelated files still present.
    eraseInstalledWhere(QStringLiteral("animeJanaiModelsCard"), [this](const QString& id) {
        const native::StoredSpatialAsset* row = component_store_detail::spatialInventoryRow(storedSpatial_, id);
        return row == nullptr || !QFileInfo(row->graphPath).isFile();
    });

    // Every closure row - the classic runtime pack, the dispatcher archive and
    // the RTX overlay alike - survives only while the selected backend's own
    // closure is installed. A stored pack whose backend record is missing is
    // not readiness: keeping its row would print "Installed: …" under a badge
    // that already says the card is not installed.
    eraseInstalledWhere(QStringLiteral("tensorRtRuntimeCard"),
                        [this](const QString&) { return !runtimeClosureInstalled(); });
    eraseInstalledWhere(QStringLiteral("tensorRtKernelsCard"),
                        [this](const QString&) { return !kernelsPackInstalled_; });
    eraseInstalledWhere(QStringLiteral("rifeModelsCard"),
                        [this](const QString&) { return !rifeInstalled_; });
}

mplayer::native::Backend ComponentStoreTab::selectedBackend() const {
    if (animeJanaiBackendCombo_ == nullptr) return native::Backend::TensorRT;
    switch (animeJanaiBackendCombo_->currentData().toInt()) {
        case static_cast<int>(native::Backend::TensorRT): return native::Backend::TensorRT;
        case static_cast<int>(native::Backend::DirectML): return native::Backend::DirectML;
        case static_cast<int>(native::Backend::TensorRTRTX): return native::Backend::TensorRTRTX;
    }
    // The combo only ever carries enum data; anything else is the classic
    // default the tab has always used rather than a third undefined state.
    return native::Backend::TensorRT;
}

bool ComponentStoreTab::tensorRtRtxOffered() {
#ifdef Q_OS_WIN
    return true;
#else
    // No RTX binary is published for Linux. The choice is not offered; a
    // restored one is still shown and refused by its own name.
    return false;
#endif
}

QString ComponentStoreTab::backendProductName(native::Backend backend) const {
    switch (backend) {
        case native::Backend::TensorRT: return tr("TensorRT / CUDA 13");
        case native::Backend::DirectML: return tr("DirectML");
        case native::Backend::TensorRTRTX: return tr("TensorRT for RTX");
    }
    return QString();
}

QString ComponentStoreTab::backendChoiceLabel(native::Backend backend) const {
    switch (backend) {
        case native::Backend::TensorRT: return tr("TensorRT (CUDA / NVDEC)");
        case native::Backend::DirectML: return tr("DirectML (D3D11VA)");
        case native::Backend::TensorRTRTX:
            return tensorRtRtxOffered()
                       ? tr("TensorRT for RTX (in-process)")
                       : tr("TensorRT for RTX (unavailable on this platform)");
    }
    return QString();
}

void ComponentStoreTab::updateRuntimeCardText() {
    const native::Backend backend = selectedBackend();
    const QString cardId = QStringLiteral("tensorRtRuntimeCard");
    QString title;
    QString description;
    switch (backend) {
        case native::Backend::TensorRT:
            title = tr("TensorRT runtime");
#ifdef Q_OS_WIN
            description = tr("Fastest NVIDIA upscaling engine; without it NVIDIA falls back "
                             "to DirectML.");
#else
            description = tr("Required for AnimeJaNai on Linux with an NVIDIA GPU; DirectML "
                             "is Windows-only.");
#endif
            break;
        case native::Backend::DirectML:
            title = tr("DirectML runtime");
            description = tr("Windows GPU acceleration for AnimeJaNai through DirectML and "
                             "ONNX Runtime.");
            break;
        case native::Backend::TensorRTRTX:
            title = tr("TensorRT for RTX runtime");
            description = tr("Runs AnimeJaNai in-process with NVIDIA TensorRT for RTX 1.7; "
                             "installs the Aji dispatcher and NVIDIA's RTX overlay. No classic "
                             "CUDA or kernel pack is used.");
            break;
    }
    if (auto* titleLabel = findChild<QLabel*>(cardId + QStringLiteral("Title")))
        titleLabel->setText(title);
    if (auto* descriptionLabel =
            findChild<QLabel*>(cardId + QStringLiteral("Description")))
        descriptionLabel->setText(description);
    if (runtimeCheck_ != nullptr) {
        runtimeCheck_->setAccessibleName(title);
        runtimeCheck_->setAccessibleDescription(description);
    }
}

void ComponentStoreTab::queueRuntimeClosureRows() {
    const QString cardId = QStringLiteral("tensorRtRuntimeCard");
    clearProgressCard(cardId);
    if (selectedBackend() == native::Backend::TensorRTRTX) {
        // Exactly the two archives this product installs, each with its file
        // name and size. No classic runtime, kernel or CUDA pack is queued.
        // An empty queue on a platform without the binary is not an installed
        // closure and not a transfer: the install attempt names the refusal in
        // the status line instead of inventing a row nobody can download.
        for (const native::AnimeJanaiMissingDownload& download :
             native::animeJanaiMissingDownloads(native::Backend::TensorRTRTX)) {
            if (download.bytes <= 0) continue;
            setProgressItem(download.label, cardId, progressDisplayName(download.label),
                            native::ComponentProvisionStage::Downloading, 0, download.bytes, true);
        }
        return;
    }
    if (pendingRuntimeClosure_) {
        setProgressItem(QStringLiteral("pending:tensorRtRuntimeCard"), cardId,
                        tr("AnimeJaNai runtime and backend dependencies"),
                        native::ComponentProvisionStage::Downloading, 0, 0, true);
        return;
    }
    const QVector<const native::ComponentPack*> packs =
        native::compatiblePacksFor(native::observeComputeCapability().smMajor,
                           native::observeComputeCapability().smMinor);
    for (const native::ComponentPack* pack : packs) {
        if (pack == nullptr || native::storedPack(native::componentStore(), *pack).installed) continue;
        const QString packCardId = pack->name == QLatin1String("trt-runtime")
                                       ? cardId
                                       : QStringLiteral("tensorRtKernelsCard");
        const QString identity = QStringLiteral("pending:") + pack->name;
        setProgressItem(identity, packCardId,
                        QStringLiteral("%1 (%2)").arg(pack->name, pack->asset),
                        native::ComponentProvisionStage::Downloading, 0, pack->assetBytes, true);
    }
}

void ComponentStoreTab::renderCards() {
    updateCardStates();
}

ComponentStoreTab::ShaderInventory ComponentStoreTab::shaderInventory() const {
    ShaderInventory inventory;
    const QString dir = mplayer::anime4k::findShadersDir();
    if (dir.isEmpty()) return inventory;
    // "Off" carries no files and is not a preset that can be incomplete.
    for (const mplayer::anime4k::ModeInfo& mode : mplayer::anime4k::availableModes()) {
        if (mode.files.isEmpty()) continue;
        ++inventory.chains;
        if (mplayer::anime4k::chainIsComplete(
                mplayer::anime4k::candidateForId(mode.id, dir), dir)) {
            ++inventory.complete;
        }
    }
    const QDir shaders(dir);
    const QStringList files =
        shaders.entryList({QStringLiteral("*.glsl")}, QDir::Files | QDir::Readable);
    inventory.files = static_cast<int>(files.size());
    for (const QString& name : files) inventory.bytes += QFileInfo(shaders.filePath(name)).size();
    return inventory;
}

void ComponentStoreTab::updateCardStates() {
    const native::GpuComputeCapability& gpu = native::observeComputeCapability();
    const native::ComponentPack* runtimePack = native::tensorRtRuntimePack();
    // The badge answers for the backend the user selected: on an NVIDIA GPU
    // with DirectML chosen it reports the DirectML runtime, not TensorRT.
    const native::Backend backend = selectedBackend();
    // Readiness is asked of the selected backend's own record, through the same
    // predicate the inventory counts and the progress line use: one card can
    // never read installed in one place and not installed in another.
    const bool runtimeInstalled = runtimeClosureInstalled();
    // The recommendation is an offer for the selected product. RTX is only
    // recommended where it can actually be installed.
    const bool runtimeRecommended =
        backend == native::Backend::TensorRTRTX
            ? (tensorRtRtxOffered() && gpu.nvidia && !animeJanaiTensorRtRtxInstalled_)
            : (gpu.nvidia && runtimeRecommended_);
    const bool rifeInstalled = rifeInstalled_;
    const QVector<native::SpatialAsset> spatialCatalog = native::spatialAssetCatalog();
    const QVector<native::StoredSpatialAsset>& spatialStored = storedSpatial_;
    int installedSpatial = 0;
    int verifiedSpatial = 0;
    qint64 spatialBytes = 0;
    for (const native::SpatialAsset& asset : spatialCatalog) {
        spatialBytes += asset.bytes;
        const native::StoredSpatialAsset* row = component_store_detail::spatialInventoryRow(spatialStored, asset.id);
        if (row != nullptr) {
            if (QFileInfo(row->graphPath).isFile()) ++installedSpatial;
            if (row->verified) ++verifiedSpatial;
        }
    }
    const bool spatialRecommended = spatialRecommended_;

    const native::ComponentPack* kernelsPack =
        tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
    const native::StoredPack kernelsStored = kernelsPack != nullptr
                                         ? native::storedPack(native::componentStore(), *kernelsPack)
                                         : native::StoredPack{};
    // Store truth only. Whether a downloaded pack is actually usable is a
    // second question, answered by the closure, and the badge says so instead
    // of letting an unusable pack read as installed.
    const bool kernelsInstalled =
        native::gpuResourceInstalledForThisDevice(kernelsPack, kernelsStored);
    const bool kernelsUsable = kernelsInstalled && componentDirsAvailable_;
    // RTX builds in-process and has no GPU-matched builder-resource pack, so
    // the classic kernel card is neither offered nor recommended for it.
    const bool kernelsOffered = gpu.nvidia && kernelsPack != nullptr &&
                                backend != native::Backend::TensorRTRTX;
    const bool kernelsRecommended = kernelsOffered && kernelsRecommended_;
    if (!kernelsOffered) selectedPacks_.remove(QStringLiteral("component:kernels"));
    // Reuse the one runtime card with the selected product's own wording.
    updateRuntimeCardText();

    auto setState = [](QLabel* label, const QString& state, const QString& detail = QString()) {
        if (label == nullptr) return;
        // The badge is one line inside its fixed width: the ink elides, and the
        // whole sentence - including anything the badge does not have room for -
        // stays in the tooltip and the accessible name.
        const int available = theme::kComponentBadgeWidth - 2 * theme::kSpacingSmall;
        label->setText(label->fontMetrics().elidedText(state, Qt::ElideRight, available));
        const QString spoken = detail.isEmpty() ? state : detail;
        label->setToolTip(spoken);
        label->setAccessibleName(spoken);
    };
    // The size beside a card is the download that product would still need, so
    // it belongs to an offer and not to something already present: an installed
    // runtime must not keep advertising its archive weight beside a badge that
    // says "installed". For RTX that weight is the dispatcher plus the overlay.
    qint64 runtimeBytes = 0;
    switch (backend) {
        case native::Backend::TensorRT:
            if (runtimePack != nullptr) runtimeBytes = runtimePack->assetBytes;
            break;
        case native::Backend::DirectML: runtimeBytes = directMlClosureBytes_; break;
        case native::Backend::TensorRTRTX: runtimeBytes = tensorRtRtxClosureBytes_; break;
    }
    const auto downloadSize = [](qint64 bytes, bool installed) {
        return bytes > 0 && !installed
                   ? tr("%1 MB").arg(QLocale().toString(bytes / 1048576))
                   : QString();
    };
    findChild<QLabel*>(QStringLiteral("tensorRtRuntimeCardSize"))
        ->setText(downloadSize(runtimeBytes, runtimeInstalled));
    findChild<QLabel*>(QStringLiteral("rifeModelsCardSize"))
        ->setText(downloadSize(native::rifeArchivePack().assetBytes, rifeInstalled));
    const qint64 kernelsBytes = kernelsPack != nullptr ? kernelsPack->assetBytes : 0;
    findChild<QLabel*>(QStringLiteral("tensorRtKernelsCardSize"))
        ->setText(downloadSize(kernelsBytes, kernelsUsable));

    findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardSize"))
        ->setText(QLocale().formattedDataSize(spatialBytes));
    // The badge states the presence; the counts it used to carry moved to the
    // card's own line, where a 0/5 can never read as an installed card.
    setState(spatialState_,
             installedSpatial == 0 ? tr("not installed") : tr("installed"));
    if (auto* counts = findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardCount"));
        counts != nullptr) {
        counts->setText(tr("%1/%2 installed · %3/%2 verified")
                            .arg(installedSpatial)
                            .arg(spatialCatalog.size())
                            .arg(verifiedSpatial));
        counts->setVisible(!spatialCatalog.isEmpty());
    }

    // Presence is the badge's one fact. "recommended for this PC" is an offer,
    // never a substitute for the truth about what is installed, and it is not
    // squeezed into the same 104 px badge: at that width the sentence elided to
    // a broken half-word ("not installed …") that read as two opposing states.
    // The offer therefore travels in the tooltip and the accessible name, where
    // a screen reader and a hover both get it in full.
    struct Offer {
        QString badge;
        QString detail;
    };
    const auto offerState = [this](bool installed, bool recommended) {
        if (installed && recommended)
            return Offer{tr("installed"), tr("Installed · recommended for this PC")};
        if (!installed && recommended)
            return Offer{tr("not installed"), tr("Not installed · recommended for this PC")};
        if (installed) return Offer{tr("installed"), QString()};
        return Offer{tr("optional"), QString()};
    };
    if (backend == native::Backend::TensorRTRTX && !tensorRtRtxOffered()) {
        // A restored RTX choice on a platform without the binary is reported as
        // unavailable with the named reason, never repainted as TensorRT.
        const QString reason = animeJanaiTensorRtRtxReason_.isEmpty()
                                   ? tr("TensorRT for RTX is available only on Windows.")
                                   : animeJanaiTensorRtRtxReason_;
        setState(runtimeState_, tr("unavailable"), reason);
    } else {
        const Offer runtimeOffer = offerState(runtimeInstalled, runtimeRecommended);
        setState(runtimeState_, runtimeOffer.badge, runtimeOffer.detail);
    }
    setState(rifeState_, rifeInstalled ? tr("OK") : tr("optional"));
    if (kernelsInstalled && !kernelsUsable) {
        setState(kernelsState_, tr("downloaded"),
                 tr("Downloaded, but the runtime closure it needs is missing."));
    } else {
        const Offer kernelsOffer = offerState(kernelsInstalled, kernelsRecommended);
        setState(kernelsState_, kernelsOffer.badge, kernelsOffer.detail);
    }
    auto setBadgeRole = [](QLabel* badge, bool recommended, bool installed) {
        badge->setAutoFillBackground(false);
        badge->setStyleSheet(QString());
        // Installed wins the styling: an item that is present must never be
        // painted as only a recommendation.
        badge->setProperty("textRole",
                           installed ? QString()
                                     : (recommended ? QStringLiteral("recommendation")
                                                    : QStringLiteral("muted")));
        badge->setProperty("badgeRole",
                           installed ? QStringLiteral("installed")
                                     : (recommended ? QString()
                                                    : QStringLiteral("optional")));
        badge->style()->unpolish(badge);
        badge->style()->polish(badge);
    };
    setBadgeRole(runtimeState_, runtimeRecommended, runtimeInstalled);
    setBadgeRole(rifeState_, false, rifeInstalled);
    setBadgeRole(spatialState_, spatialRecommended,
                 !spatialCatalog.isEmpty() && verifiedSpatial == spatialCatalog.size());
    setBadgeRole(kernelsState_, kernelsRecommended, kernelsUsable);

    // The shader card is an inventory: every preset chain either has all of its
    // files in the shipped tree or it does not, and nothing can be downloaded to
    // change that. "installed" here means the presets the catalogue offers are
    // actually servable.
    const ShaderInventory shaders = shaderInventory();
    const bool shadersComplete = shaders.chains > 0 && shaders.complete == shaders.chains;
    setState(anime4kState_, shadersComplete ? tr("installed") : tr("incomplete"));
    setBadgeRole(anime4kState_, false, shadersComplete);
    if (auto* size = findChild<QLabel*>(QStringLiteral("anime4kShadersCardSize"))) {
        size->setText(tr("%1 MB").arg(QLocale().toString(shaders.bytes / 1048576)));
    }
    if (auto* counts = findChild<QLabel*>(QStringLiteral("anime4kShadersCardCount"))) {
        counts->setText(tr("%1 of %2 presets complete · %3 files")
                            .arg(shaders.complete)
                            .arg(shaders.chains)
                            .arg(shaders.files));
        counts->setVisible(true);
    }
    anime4kCheck_->setAccessibleName(tr("Anime4K shaders"));

    // The upscaler inventory: how many of the pinned optional graphs are in the
    // store. An empty list of them is a state, not a failure to download.
    int upscalerInstalled = 0;
    const QVector<mplayer::native::UpscalerModel> upscalers =
        mplayer::native::upscalerCatalog();
    for (const mplayer::native::UpscalerModel& model : upscalers) {
        if (!mplayer::native::installedUpscalerPath(model).isEmpty()) ++upscalerInstalled;
    }
    const bool allUpscalers = !upscalers.isEmpty() && upscalerInstalled == upscalers.size();
    setState(upscalerState_, allUpscalers ? tr("installed")
                                          : (upscalerInstalled > 0 ? tr("partial")
                                                                   : tr("optional")));
    setBadgeRole(upscalerState_, false, allUpscalers);
    upscalerCheck_->setAccessibleName(tr("Upscale models"));
    if (auto* counts = findChild<QLabel*>(QStringLiteral("upscalerModelsCardCount"))) {
        counts->setText(tr("%1 of %2 upscale models installed")
                            .arg(upscalerInstalled)
                            .arg(upscalers.size()));
        counts->setVisible(true);
    }
    if (auto* size = findChild<QLabel*>(QStringLiteral("upscalerModelsCardSize"))) {
        size->clear();
    }

    runtimeCheck_->setAccessibleName(backendProductName(backend));
    rifeCheck_->setAccessibleName(tr("RIFE interpolation models"));
    spatialCheck_->setAccessibleName(tr("AnimeJaNai upscaling models"));
    spatialCheck_->setAccessibleDescription(
        tr("Select all five pinned upscaling graphs for download or repair."));
    kernelsCheck_->setAccessibleName(tr("TensorRT kernels for this GPU"));
    if (QWidget* kernelsCard = kernelsCheck_->parentWidget())
        kernelsCard->setVisible(kernelsOffered);
    kernelsCheck_->setVisible(kernelsOffered);
    if (!componentSelectionsInitialized_ && inventoryReady_) {
        componentSelectionsInitialized_ = true;
        if (runtimeRecommended) selectedPacks_.insert(QStringLiteral("component:runtime"));
        if (kernelsRecommended) selectedPacks_.insert(QStringLiteral("component:kernels"));
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
    // The optional upscalers are downloadable from here as one group; the
    // Upscalers page still manages them one by one.
    showSelection(upscalerCheck_, QStringLiteral("upscaler:all"));
    // The banner is an offer for an idle surface. While a run is up it would
    // describe work already in flight, and once the selected backend and every
    // pinned model are present it would contradict the cards.
    const bool acquisitionInProgress =
        acquisitionChecking_ || acquisitionSequenceActive_ || inventoryScanning_ ||
        downloadActive() || verifyActive();
    // The selected product's own installation decides the banner; a classic
    // closure never stands in for RTX and vice versa.
    const bool backendInstalledForSelection = runtimeInstalled;
    const bool selectionSatisfied =
        inventoryReady_ && backendInstalledForSelection &&
        installedSpatial == spatialCatalog.size() && !spatialCatalog.isEmpty() &&
        (!kernelsOffered || kernelsUsable);
    const bool showBanner = inventoryReady_ &&
                            (runtimeRecommended || kernelsRecommended || spatialRecommended) &&
                            !acquisitionInProgress && !selectionSatisfied;
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
    request.store = native::componentStore();
    request.rifeStore = native::rifeArchiveStore();
    const native::GpuComputeCapability& gpu = native::observeComputeCapability();
    const native::Backend backend = selectedBackend();
    // The GPU-matched builder packs belong to classic TensorRT only. An RTX or
    // DirectML verification reports on its own closure, never on packs the
    // selected product does not use.
    if (backend == native::Backend::TensorRT)
        request.packs = native::compatiblePacksFor(gpu.smMajor, gpu.smMinor);
    request.rifePins = native::rifeModelPins();
    request.spatialCatalog = native::spatialAssetCatalog();
    request.backend = backend;
    return request;
}

void ComponentStoreTab::verifyAll() {
    // One run at a time. A second click while a run is hashing is refused by
    // the disabled control, and refused again here so no re-entrant path
    // (a keyboard activation, a shortcut, a test) can start a parallel run.
    if (verifyActive()) return;
    if (runner_ == nullptr) {
        applyStatus(QStringLiteral("native.components.noRunner"),
                    StatusReason{.kind = StatusReason::Kind::VerifyNeedsRunner});
        updateControls();
        return;
    }
    startVerify(captureVerifyRequest());
}

void ComponentStoreTab::applyProvisionProgress(
    const native::ComponentProvisionProgress& progress) {
    applyProgressEvent(progress);
}

QString ComponentStoreTab::progressCardForPack(const QString& pack) const {
    if (native::findSpatialAsset(pack) != nullptr) return QStringLiteral("animeJanaiModelsCard");
    // Upscaler rows are keyed by their digest, which no other pack uses.
    for (const mplayer::native::UpscalerModel& model : mplayer::native::upscalerCatalog()) {
        if (pack == model.sha256) return QStringLiteral("upscalerModelsCard");
    }

    for (const native::ComponentPack& component : native::tensorRtComponentCatalog()) {
        if (pack == component.name || pack == component.asset ||
            component.files.contains(pack)) {
            return component.name == QLatin1String("trt-runtime")
                       ? QStringLiteral("tensorRtRuntimeCard")
                       : QStringLiteral("tensorRtKernelsCard");
        }
    }

    const native::RifeArchivePack& rife = native::rifeArchivePack();
    if (pack == rife.name || pack == rife.asset)
        return QStringLiteral("rifeModelsCard");

    // The RTX overlay is part of the runtime closure, not a GPU-kernel pack,
    // so it renders on the runtime card like the dispatcher it accompanies.
    if (pack == QLatin1String("aji-v0.24.0") ||
        pack == QLatin1String("aji-trtrtx-v0.24.0") ||
        pack == QLatin1String("onnxruntime-directml-1.24.4") ||
        pack == QLatin1String("directml-1.15.4") ||
        pack == QLatin1String("trt-runtime-trtexec") ||
        pack == QLatin1String("animejanai-local-runtime")) {
        return QStringLiteral("tensorRtRuntimeCard");
    }
    return {};
}

QString ComponentStoreTab::progressDisplayName(const QString& pack) const {
    if (const native::SpatialAsset* asset = native::findSpatialAsset(pack)) return asset->name;

    for (const native::ComponentPack& component : native::tensorRtComponentCatalog()) {
        if (pack == component.name)
            return QStringLiteral("%1 (%2)").arg(component.name, component.asset);
        if (pack == component.asset) return component.asset;
        if (component.files.contains(pack)) return pack;
    }

    if (pack == QLatin1String("aji-v0.24.0")) {
#ifdef Q_OS_WIN
        return QStringLiteral("aji-v0.24.0 (aji-windows-x64.zip)");
#else
        return QStringLiteral("aji-v0.24.0 (aji-linux-x64.tar.zst)");
#endif
    }
    if (pack == QLatin1String("aji-trtrtx-v0.24.0"))
        return QStringLiteral("aji-trtrtx-v0.24.0 (aji-trtrtx-windows-x64.zip)");
    if (pack == QLatin1String("onnxruntime-directml-1.24.4"))
        return QStringLiteral("Microsoft.ML.OnnxRuntime.DirectML/1.24.4 "
                              "(microsoft.ml.onnxruntime.directml.1.24.4.nupkg)");
    if (pack == QLatin1String("directml-1.15.4"))
        return QStringLiteral("Microsoft.AI.DirectML/1.15.4 "
                              "(microsoft.ai.directml.1.15.4.nupkg)");
    if (pack == QLatin1String("trt-runtime-trtexec"))
        return progressDisplayName(QStringLiteral("trt-runtime")) +
               QStringLiteral(" (trtexec)");
    if (pack == QLatin1String("animejanai-local-runtime"))
        return tr("Local AnimeJaNai runtime files");

    const native::RifeArchivePack& rife = native::rifeArchivePack();
    if (pack == rife.name)
        return QStringLiteral("%1 (%2)").arg(rife.name, rife.asset);
    if (pack == rife.asset) return rife.asset;
    return pack;
}

void ComponentStoreTab::applyProgressEvent(const native::ComponentProvisionProgress& progress) {
    const QString cardId = progressCardForPack(progress.pack);
    if (cardId.isEmpty()) return;

    for (const QString& pendingIdentity :
         {QStringLiteral("pending:") + cardId,
          QStringLiteral("pending:") + progress.pack}) {
        progressItems_.remove(pendingIdentity);
        currentProgressItems_.remove(pendingIdentity);
    }
    setProgressItem(progress.pack, cardId, progressDisplayName(progress.pack), progress.stage,
                    progress.received, progress.total);
    publishAcquisition();
}

void ComponentStoreTab::setProgressItem(
    const QString& identity, const QString& cardId, const QString& displayName,
    native::ComponentProvisionStage stage, qint64 received, qint64 total, bool queued) {
    auto existing = progressItems_.find(identity);
    if (existing != progressItems_.end() &&
        existing->outcome != ProgressItem::Outcome::Active) {
        return;
    }

    ProgressItem item;
    if (existing != progressItems_.end()) item = existing.value();
    item.cardId = cardId;
    item.displayName = displayName;
    item.stage = stage;
    const bool phaseOnly = existing != progressItems_.end() &&
                           stage != native::ComponentProvisionStage::Downloading &&
                           received == 0 && total == 0;
    if (!phaseOnly) item.received = qMax<qint64>(0, received);
    if (total > 0 || existing == progressItems_.end() || existing->total <= 0)
        item.total = qMax<qint64>(0, total);
    item.queued = queued;
    item.detail.clear();
    item.outcome = stage == native::ComponentProvisionStage::Installed
                       ? ProgressItem::Outcome::Installed
                       : ProgressItem::Outcome::Active;
    if (item.outcome == ProgressItem::Outcome::Installed && item.total > 0)
        item.received = item.total;
    progressItems_.insert(identity, item);
    currentProgressItems_.insert(identity);
    renderCardProgress(cardId);
}

void ComponentStoreTab::settleProgressItem(const QString& identity, bool installed,
                                           bool cancelled, const QString& detail) {
    auto item = progressItems_.find(identity);
    if (item == progressItems_.end() ||
        item->outcome != ProgressItem::Outcome::Active) {
        return;
    }
    if (installed) {
        item->outcome = ProgressItem::Outcome::Installed;
        item->stage = native::ComponentProvisionStage::Installed;
        if (item->total > 0) item->received = item->total;
    } else if (cancelled) {
        item->outcome = ProgressItem::Outcome::Cancelled;
        // A cancelled transfer contributes no bytes: keeping received==total
        // is how an abandoned run used to compute a full bar.
        item->received = 0;
        item->total = 0;
    } else {
        item->outcome = ProgressItem::Outcome::Failed;
        item->detail = detail;
        item->received = 0;
        item->total = 0;
    }
    renderCardProgress(item->cardId);
    publishAcquisition();
}

void ComponentStoreTab::settleProgressCard(const QString& cardId, bool installed,
                                           bool cancelled, const QString& detail) {
    bool changed = false;
    for (const QString& identity : currentProgressItems_) {
        auto item = progressItems_.find(identity);
        if (item == progressItems_.end() || item->cardId != cardId ||
            item->outcome != ProgressItem::Outcome::Active) {
            continue;
        }
        if (installed) {
            item->outcome = ProgressItem::Outcome::Installed;
            item->stage = native::ComponentProvisionStage::Installed;
            if (item->total > 0) item->received = item->total;
        } else if (cancelled) {
            item->outcome = ProgressItem::Outcome::Cancelled;
            item->received = 0;
            item->total = 0;
        } else {
            item->outcome = ProgressItem::Outcome::Failed;
            item->detail = detail;
            item->received = 0;
            item->total = 0;
        }
        changed = true;
    }
    if (changed) renderCardProgress(cardId);
    publishAcquisition();
}

void ComponentStoreTab::clearProgressItem(const QString& identity) {
    auto item = progressItems_.find(identity);
    if (item == progressItems_.end()) return;
    const QString cardId = item->cardId;
    currentProgressItems_.remove(identity);
    progressItems_.erase(item);
    renderCardProgress(cardId);
}

void ComponentStoreTab::clearProgressCard(const QString& cardId) {
    for (auto item = progressItems_.begin(); item != progressItems_.end();) {
        if (item->cardId == cardId) {
            currentProgressItems_.remove(item.key());
            item = progressItems_.erase(item);
        } else {
            ++item;
        }
    }
    renderCardProgress(cardId);
}

components::CardState ComponentStoreTab::cardStateFor(const QString& cardId) const {
    components::CardState state;
    for (auto item = progressItems_.cbegin(); item != progressItems_.cend(); ++item) {
        if (item->cardId != cardId) continue;
        switch (item->outcome) {
            case ProgressItem::Outcome::Active:
                if (item->queued) {
                    ++state.transfer.queued;
                    // A queued unit is part of the run's total even before its
                    // first byte: otherwise the first active unit would report
                    // 100 % while four siblings were still queued.
                    if (item->total > 0) state.transfer.total += item->total;
                    else ++state.transfer.unmetered;
                    break;
                }
                ++state.transfer.active;
                if (item->stage == native::ComponentProvisionStage::Verifying)
                    state.verificationRunning = true;
                if (item->total > 0) {
                    state.transfer.total += item->total;
                    state.transfer.received += qMin(item->received, item->total);
                } else {
                    // A byte-less phase event: the label line is the honest
                    // report, not an invented percentage.
                    ++state.transfer.unmetered;
                }
                break;
            case ProgressItem::Outcome::Installed:
                ++state.transfer.installed;
                if (item->total > 0) {
                    state.transfer.total += item->total;
                    state.transfer.received += qMin(item->received, item->total);
                }
                break;
            case ProgressItem::Outcome::Failed:
                ++state.transfer.failed;
                break;
            case ProgressItem::Outcome::Cancelled:
                ++state.transfer.cancelled;
                break;
        }
    }

    state.inventory.known = inventoryReady_;
    const native::GpuComputeCapability& gpu = native::observeComputeCapability();
    if (cardId == QLatin1String("animeJanaiModelsCard")) {
        state.inventory.total = native::spatialAssetCatalog().size();
        state.inventory.installed = installedSpatialCount_;
        state.inventory.verified = verifiedSpatialCount_;
    } else if (cardId == QLatin1String("tensorRtKernelsCard")) {
        const native::ComponentPack* pack = tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
        state.inventory.total = pack != nullptr ? 1 : 0;
        state.inventory.installed = kernelsPackInstalled_ ? 1 : 0;
        state.inventory.verified =
            pack != nullptr && verifiedPacks_.contains(pack->name) ? 1 : 0;
        if (pack != nullptr && !componentDirsAvailable_ && !kernelsPackInstalled_)
            state.missing.append(pack->name);
    } else if (cardId == QLatin1String("tensorRtRuntimeCard")) {
        state.inventory.total = inventoryReady_ ? 1 : 0;
        state.inventory.installed = runtimeCardInstalledOnDisk();
        const QString runtimeName =
            native::tensorRtRuntimePack() != nullptr ? native::tensorRtRuntimePack()->name : QString();
        // The classic runtime pack proves the classic closure only; RTX and
        // DirectML are proved by their own installation record.
        const bool packProves =
            selectedBackend() == native::Backend::TensorRT && !runtimeName.isEmpty() &&
            verifiedPacks_.contains(runtimeName);
        state.inventory.verified =
            (packProves || verifiedAnimeJanaiBackends_.contains(
                               static_cast<int>(selectedBackend())))
                ? 1
                : 0;
    } else if (cardId == QLatin1String("anime4kShadersCard")) {
        // The shaders ship with the build: present is installed, and a chain
        // whose files are all there is the only verification this can offer
        // without re-hashing the staged tree.
        const ShaderInventory shaders = shaderInventory();
        state.inventory.known = true;
        state.inventory.total = shaders.chains;
        state.inventory.installed = shaders.complete;
        state.inventory.verified = shaders.complete;
    } else if (cardId == QLatin1String("rifeModelsCard")) {
        state.inventory.total = native::rifeModelPins().isEmpty() ? 0 : 1;
        state.inventory.installed = rifeInstalled_ ? 1 : 0;
        state.inventory.verified = rifeVerified_ ? 1 : 0;
    }

    // A live proof renders indeterminate, never through the transfer path.
    const bool hasItems = state.transfer.active > 0 || state.transfer.queued > 0 ||
                          state.transfer.failed > 0 || state.transfer.cancelled > 0 ||
                          state.transfer.installed > 0;
    if ((verificationActive_ || verifying_) && (hasItems || state.inventory.installed > 0))
        state.verificationRunning = true;
    return state;
}

void ComponentStoreTab::renderCardProgress(const QString& cardId) {
    QProgressBar* const bar = cardProgressBars_.value(cardId, nullptr);
    QLabel* const label = cardProgressStatusLabels_.value(cardId, nullptr);
    if (bar == nullptr || label == nullptr) return;

    QStringList activeLines;
    QStringList failedLines;
    QStringList cancelledLines;
    QStringList installedNames;
    for (auto item = progressItems_.cbegin(); item != progressItems_.cend(); ++item) {
        if (item->cardId != cardId) continue;
        QString displayName = item->displayName;
        if (item.key() == QLatin1String("pending:tensorRtRuntimeCard")) {
            displayName = tr("AnimeJaNai runtime and backend dependencies");
        } else if (item.key() == QLatin1String("pending:animejanai-local-runtime")) {
            displayName = tr("Local AnimeJaNai runtime and backend dependencies");
        } else if (item.key() == QLatin1String("animejanai-local-runtime")) {
            displayName = tr("Local AnimeJaNai runtime files");
        }

        switch (item->outcome) {
            case ProgressItem::Outcome::Active:
                if (item->queued) {
                    activeLines.append(tr("Queued: %1").arg(displayName));
                } else {
                    const QString byteVerb =
                        item.key() == QLatin1String("animejanai-local-runtime")
                            ? tr("copied")
                            : tr("downloaded");
                    switch (item->stage) {
                        case native::ComponentProvisionStage::Downloading:
                            activeLines.append(
                                item->total > 0
                                    ? tr("Downloading %1: %2 of %3")
                                          .arg(displayName,
                                               QLocale().formattedDataSize(item->received),
                                               QLocale().formattedDataSize(item->total))
                                    : tr("Downloading %1...").arg(displayName));
                            break;
                        case native::ComponentProvisionStage::Extracting:
                            activeLines.append(
                                item->total > 0
                                    ? tr("Extracting %1 (%2 of %3 %4)...")
                                          .arg(displayName,
                                               QLocale().formattedDataSize(item->received),
                                               QLocale().formattedDataSize(item->total),
                                               byteVerb)
                                    : tr("Extracting %1...").arg(displayName));
                            break;
                        case native::ComponentProvisionStage::Installing:
                            activeLines.append(
                                item->total > 0
                                    ? tr("Installing %1 (%2 of %3 %4)...")
                                          .arg(displayName,
                                               QLocale().formattedDataSize(item->received),
                                               QLocale().formattedDataSize(item->total),
                                               byteVerb)
                                    : tr("Installing %1...").arg(displayName));
                            break;
                        case native::ComponentProvisionStage::Verifying:
                            activeLines.append(
                                item->total > 0
                                    ? tr("Verifying %1 (%2 of %3 %4)...")
                                          .arg(displayName,
                                               QLocale().formattedDataSize(item->received),
                                               QLocale().formattedDataSize(item->total),
                                               byteVerb)
                                    : tr("Verifying %1...").arg(displayName));
                            break;
                        case native::ComponentProvisionStage::Installed:
                            activeLines.append(tr("Finishing %1...").arg(displayName));
                            break;
                    }
                }
                break;
            case ProgressItem::Outcome::Installed:
                installedNames.append(displayName);
                break;
            case ProgressItem::Outcome::Failed:
                failedLines.append(item->detail.isEmpty()
                                       ? tr("Failed %1").arg(displayName)
                                       : tr("Failed %1: %2").arg(displayName, item->detail));
                break;
            case ProgressItem::Outcome::Cancelled:
                cancelledLines.append(tr("Cancelled %1").arg(displayName));
                break;
        }
    }

    const components::CardState state = cardStateFor(cardId);
    // A card's own inventory answers for its "Installed:" line. Terminal rows
    // are a session log, and a log entry outlives the state it described: after
    // a backend switch, or when a pack was stored without the closure it needs,
    // the badge already reads "not installed" while the log line still claims
    // "Installed: …" - two opposing states printed on one card. While nothing
    // is live on this card and the inventory finds nothing installed, the line
    // and the bar are withdrawn; the badge is the surviving statement of truth.
    const bool inventoryContradictsInstalled =
        state.inventory.known && state.inventory.installed == 0 &&
        activeLines.isEmpty() && failedLines.isEmpty() && cancelledLines.isEmpty();
    if (inventoryContradictsInstalled) {
        bar->hide();
        label->clear();
        label->hide();
        return;
    }
    const bool hasItems = state.transfer.active > 0 || state.transfer.queued > 0 ||
                          state.transfer.failed > 0 || state.transfer.cancelled > 0 ||
                          state.transfer.installed > 0;
    if (!hasItems) {
        bar->hide();
        label->clear();
        label->hide();
        return;
    }
    if (activeLines.isEmpty() && failedLines.isEmpty() && cancelledLines.isEmpty()) {
        label->setText(tr("Installed: %1").arg(installedNames.join(QStringLiteral(", "))));
    } else {
        QStringList lines = activeLines;
        lines.append(failedLines);
        lines.append(cancelledLines);
        if (!installedNames.isEmpty())
            lines.append(tr("Installed: %1").arg(installedNames.join(QStringLiteral(", "))));
        label->setText(lines.join(QStringLiteral("; ")));
    }

    // One decision, shared by every path: an active transfer reports its own
    // bytes, an unmeasured or verifying run is indeterminate, and 100 % needs a
    // disk inventory that confirms the terminal items. Failed and cancelled
    // transfers keep the failure property and never read as complete.
    const components::ProgressBarDecision decision = components::componentProgressBar(state);
    if (decision.indeterminate) {
        bar->setRange(0, 0);
    } else {
        bar->setRange(0, 100);
        bar->setValue(decision.percent);
    }
    const QString runState =
        decision.failed ? QStringLiteral("failure")
                        : (decision.indeterminate ? QStringLiteral("running") : QString());
    if (bar->property("runState").toString() != runState) {
        bar->setProperty("runState", runState);
        bar->style()->unpolish(bar);
        bar->style()->polish(bar);
    }
    bar->setVisible(decision.visible);
    label->setVisible(true);
}

void ComponentStoreTab::renderAllCardProgress() {
    for (auto bar = cardProgressBars_.cbegin(); bar != cardProgressBars_.cend(); ++bar)
        renderCardProgress(bar.key());
}

int ComponentStoreTab::activeProgressItemCount() const {
    int active = 0;
    for (const QString& identity : currentProgressItems_) {
        const auto item = progressItems_.constFind(identity);
        if (item != progressItems_.cend() &&
            item->outcome == ProgressItem::Outcome::Active) {
            ++active;
        }
    }
    return active;
}

void ComponentStoreTab::applyDownloadProgress(const native::ComponentProvisionProgress& progress) {
    applyProgressEvent(progress);
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
    renderAllCardProgress();
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
    bumpRunGeneration(QStringLiteral("verification started"));
    verifying_ = true;
    const quint64 generation = runGeneration_;
    modelCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelled = modelCancelFlag_;
    applyStatus(QString(), StatusReason{.kind = StatusReason::Kind::Busy,
                                        .busy = StatusReason::Busy::InstalledVerification});
    updateControls();
    // The bars must leave 100 % for the whole proof, not just when it ends.
    renderAllCardProgress();

    runner_->runTransfer(
        [request, alive = alive_, cancelled]() -> QVariant {
            const backend::WorkCancellationScope verifyCancel(cancelled.get());
            const auto stop = [&] {
                return !alive->load() ||
                       backend::WorkCancellationScope::cancellationRequested();
            };
            const native::ComponentStore store = request.store;
            const native::RifeArchiveStore rifeStore = request.rifeStore;
            const native::Backend backend = request.backend;
            int verified = 0;
            int failed = 0;
            QStringList failures;
            QSet<QString> provedPacks;
            QSet<QString> provedRifeGraphs;
            std::optional<int> provedBackend;
            for (const native::ComponentPack* pack : request.packs) {
                if (stop()) return QVariant{};
                const native::StoredPack result = native::verifyPack(store, *pack);
                if (result.installed) {
                    ++verified;
                    provedPacks.insert(pack->name);
                    continue;
                }
                if (native::storedPack(store, *pack).installed) {
                    ++failed;
                    failures.append(QStringLiteral("%1: %2").arg(pack->name, result.reason));
                }
            }
            if (stop()) return QVariant{};
            const native::AnimeJanaiInstallation installation =
                native::describeAnimeJanaiInstallation(backend, true);
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
                    native::stableName(backend),
                    installation.reason.isEmpty()
                        ? QStringLiteral("animejanai.installation.incomplete")
                        : installation.reason));
            }
            int failedRifeGraphs = 0;
            for (const native::RifeModelPin& pin : request.rifePins) {
                if (stop()) return QVariant{};
                const native::StoredRifeGraph stored = native::storedRifeGraph(rifeStore, pin);
                if (!stored.installed) continue;
                const native::StoredRifeGraph result = native::verifyRifeGraph(rifeStore, pin);
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
            const QVector<native::StoredSpatialAsset> storedSpatial = native::storedSpatialAssets();
            for (const native::SpatialAsset& asset : request.spatialCatalog) {
                if (stop()) return QVariant{};
                const native::StoredSpatialAsset* model =
                    component_store_detail::spatialInventoryRow(storedSpatial, asset.id);
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
            modelCancelFlag_.reset();
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
            modelCancelFlag_.reset();
            bumpRunGeneration(QStringLiteral("verification failed"));
            const QString cause = backend::failureLine(failure);
            applyStatus(QStringLiteral("native.components.fetch"),
                        StatusReason{.kind = StatusReason::Kind::NotVerified,
                                     .detail = cause});
            updateControls();
        });
}


}  // namespace mikai::app
