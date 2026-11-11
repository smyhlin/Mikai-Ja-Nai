#include "dialogs/ComponentStoreTabInternal.hpp"
#include "state/SettingsKeys.hpp"
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
#include <native/models/NativeUpscalers.hpp>
#include <native/models/NativeSpatialAssets.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QEvent>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
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
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include "widgets/FlowLayout.hpp"
#include <QtWidgets/QVBoxLayout>

#include <stdexcept>
#include <optional>
#include <functional>

namespace mikai::app {

namespace native = mplayer::native;


namespace {
/// A model graph is ~2 MB but the link may be slow; 20 s was an artwork budget.
constexpr int kModelDownloadTimeoutMs = 120000;

/// The selected backend's own installation answer, read from the off-GUI
/// inventory snapshot. Every enum value answers for itself; no value can borrow
/// another backend's record.
bool installationInstalledIn(const QVariantMap& inventory, native::Backend backend) {
    switch (backend) {
        case native::Backend::TensorRT:
            return inventory.value(QStringLiteral("trtInstalled")).toBool();
        case native::Backend::DirectML:
            return inventory.value(QStringLiteral("directMlInstalled")).toBool();
        case native::Backend::TensorRTRTX:
            return inventory.value(QStringLiteral("rtxInstalled")).toBool();
    }
    return false;
}

/// The bytes the selected backend's missing closure still needs.
qint64 closureBytesIn(const QVariantMap& inventory, native::Backend backend) {
    switch (backend) {
        case native::Backend::TensorRT:
            return inventory.value(QStringLiteral("tensorRtClosureBytes")).toLongLong();
        case native::Backend::DirectML:
            return inventory.value(QStringLiteral("directMlClosureBytes")).toLongLong();
        case native::Backend::TensorRTRTX:
            return inventory.value(QStringLiteral("rtxClosureBytes")).toLongLong();
    }
    return 0;
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


void ComponentStoreTab::downloadMissing() {
    downloadMissing(true);
}

void ComponentStoreTab::installRecommended() {
    if (acquisitionChecking_ || downloadActive() || verifyActive()) return;
    const native::GpuComputeCapability& gpu = native::observeComputeCapability();
    // The player persists its backend before it can fail and ask for setup, so
    // the stored value is the exact product this flow has to install. Prefer
    // it, including an explicit RTX choice: the classic default must never
    // overwrite the reason the setup was opened. The key is read only; this
    // tab writes nothing.
    const auto restoredBackend = [&]() -> std::optional<native::Backend> {
        QSettings settings;
        auto b = native::backendFromName(
            settings.value(QString::fromUtf8(kAnimeJanaiBackend)).toString());
        if (native::isNvidiaRtx(gpu) && (!b.has_value() || (*b == native::Backend::TensorRT && !animeJanaiTensorRtInstalled_))) {
            b = native::Backend::TensorRTRTX;
        }
        return b;
    }();
    bool preferRestored = restoredBackend.has_value();
    // An RTX row already selected in this tab is just as explicit as the stored
    // value; never downgrade it to a non-RTX stored choice. On an RTX GPU with no
    // classic TensorRT installed, never install classic TensorRT.
    if (selectedBackend() == native::Backend::TensorRTRTX ||
        (native::isNvidiaRtx(gpu) && !animeJanaiTensorRtInstalled_)) {
        preferRestored = false;
    }
#ifdef Q_OS_WIN
    if (preferRestored && animeJanaiBackendCombo_->findData(
                              static_cast<int>(*restoredBackend)) < 0)
        preferRestored = false;
#else
    // DirectML is not a Linux Standard backend, and the restored RTX row is
    // added when the platform does not offer it so the refusal stays visible.
    if (preferRestored && *restoredBackend == native::Backend::DirectML) preferRestored = false;
    if (preferRestored && *restoredBackend == native::Backend::TensorRTRTX &&
        animeJanaiBackendCombo_->findData(static_cast<int>(native::Backend::TensorRTRTX)) < 0) {
        animeJanaiBackendCombo_->addItem(backendChoiceLabel(native::Backend::TensorRTRTX),
                                         static_cast<int>(native::Backend::TensorRTRTX));
    }
#endif
    native::Backend preferredBackend = selectedBackend();
    if (preferRestored && restoredBackend.has_value()) {
        preferredBackend = *restoredBackend;
    }
    const int backendIndex = animeJanaiBackendCombo_->findData(
        static_cast<int>(preferredBackend));
    // A different backend makes the combo handler bump the generation
    // synchronously, and a bump clears the pending flag. Claim it only after
    // the change, and let the refresh the handler started settle it.
    if (backendIndex >= 0) animeJanaiBackendCombo_->setCurrentIndex(backendIndex);
    recommendedInstallPending_ = true;
    if (inventoryReady_ && !inventoryScanning_) finishRecommendedInstall();
    else scheduleInventoryRefresh();
}

void ComponentStoreTab::finishRecommendedInstall() {
    if (!recommendedInstallPending_ || !inventoryReady_) return;
    recommendedInstallPending_ = false;
    refresh();
    bool runtimeSelected = false;
    switch (selectedBackend()) {
        case native::Backend::TensorRT: runtimeSelected = runtimeRecommended_; break;
        case native::Backend::DirectML: runtimeSelected = !animeJanaiDirectMlInstalled_; break;
        case native::Backend::TensorRTRTX:
            // The RTX closure is a mandatory part of its own product, and this
            // path is only reached with RTX explicitly selected.
            runtimeSelected = !animeJanaiTensorRtRtxInstalled_;
            break;
    }
    runtimeCheck_->setChecked(runtimeSelected);
    // RTX has no GPU-matched builder pack; the classic recommendation must not
    // be ticked for it even though the card is hidden.
    kernelsCheck_->setChecked(kernelsRecommended_ &&
                              selectedBackend() != native::Backend::TensorRTRTX);
    spatialCheck_->setChecked(spatialRecommended_);
    // This explicit setup approval advertises upscaling only. Reset a prior
    // manual RIFE choice here, never during refresh, retranslation or Apply.
    rifeCheck_->setChecked(false);
    upscalerCheck_->setChecked(false);
    downloadMissing(false);
}

void ComponentStoreTab::downloadMissing(bool showReview) {
    if (acquisitionChecking_ || acquisitionSequenceActive_ || downloadActive() ||
        verifyActive()) {
        return;
    }
    if (runner_ == nullptr) {
        applyStatus(QStringLiteral("native.components.noClient"),
                    StatusReason{.kind = StatusReason::Kind::CheckNeedsRunner});
        return;
    }
    if (inventoryScanning_) {
        if (inventoryCancelFlag_) inventoryCancelFlag_->store(true);
        ++inventoryGeneration_;
        inventoryScanning_ = false;
    }
    bumpRunGeneration(QStringLiteral("component check started"));
    acquisitionChecking_ = true;
    acquisitionCheckCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelled = acquisitionCheckCancelFlag_;
    const quint64 generation = runGeneration_;
    const native::Backend backend = selectedBackend();
    const native::GpuComputeCapability gpu = native::observeComputeCapability();
    const QSet<QString> selected = selectedPacks_;
    pendingUpscalers_.clear();
    applyStatus(QString(), StatusReason{.kind = StatusReason::Kind::Busy,
                                        .busy = StatusReason::Busy::Checking});
    updateControls();
    acquisitionSequenceActive_ = true;
    updateControls();
    const std::shared_ptr<std::atomic<bool>> live = alive_;
    runner_->runTransfer(
        [backend, gpu, cancelled] {
            return component_store_detail::inventorySnapshot(gpu, true, false, cancelled.get());
        },
        [this, live, generation, cancelled, backend, selected, showReview,
         gpu](const QVariant& value) {
            if (!live->load() || generation != runGeneration_) return;
            if (cancelled->load()) {
                acquisitionChecking_ = false;
                acquisitionSequenceActive_ = false;
                acquisitionCheckCancelFlag_.reset();
                pendingModels_.clear();
                pendingUpscalers_.clear();
                applyStatus(QStringLiteral("native.components.cancelled"), QString());
                updateControls();
                return;
            }
            applyInventorySnapshot(value);
            const QVariantMap inventory = value.toMap();
            const QVector<native::SpatialAsset> catalog = native::spatialAssetCatalog();
            pendingModels_.clear();
            pendingUpscalers_.clear();
            if (selected.contains(QStringLiteral("component:spatial"))) {
                for (int index = 0; index < catalog.size(); ++index) {
                    const native::StoredSpatialAsset* row = component_store_detail::spatialInventoryRow(storedSpatial_, catalog.at(index).id);
                    if (row == nullptr || !row->verified) pendingModels_.append(index);
                }
            }
            const QVector<mplayer::native::UpscalerModel> upscalers =
                mplayer::native::upscalerCatalog();
            if (selected.contains(QStringLiteral("upscaler:all"))) {
                for (const mplayer::native::UpscalerModel& model : upscalers) {
                    if (mplayer::native::installedUpscalerPath(model).isEmpty())
                        pendingUpscalers_.append(model);
                }
            }
            const bool runtimeSelected = selected.contains(QStringLiteral("component:runtime"));
            const bool kernelsSelected = selected.contains(QStringLiteral("component:kernels"));
            // The GPU-matched builder packs belong to classic TensorRT only.
            const bool componentMissing = backend == native::Backend::TensorRT &&
                (runtimeSelected || kernelsSelected) && gpu.smMajor > 0 &&
                !inventory.value(QStringLiteral("componentDirsAvailable")).toBool();
            const bool backendInstalled = installationInstalledIn(inventory, backend);
            const bool backendWork = (runtimeSelected && !backendInstalled) || componentMissing;
            const bool selectedRifeMissing = selected.contains(QStringLiteral("rife:archive")) &&
                !inventory.value(QStringLiteral("rifeVerified")).toBool();
            QStringList review;
            if (backendWork) {
                review.append(tr("%1 backend closure").arg(backendProductName(backend)));
            }
            for (int index : pendingModels_) review.append(catalog.at(index).name);
            for (const mplayer::native::UpscalerModel& model : pendingUpscalers_)
                review.append(model.displayName);
            if (selectedRifeMissing)
                review.append(tr("RIFE 3.7.0 archive pack (all 47 graphs)"));
            if (review.isEmpty()) {
                acquisitionChecking_ = false;
                acquisitionSequenceActive_ = false;
                acquisitionCheckCancelFlag_.reset();
                applyStatus(QString(), StatusReason{.kind = StatusReason::Kind::NothingToInstall});
                updateControls();
                return;
            }
            qint64 bytes = 0;
            for (int index : pendingModels_) bytes += catalog.at(index).bytes;
            for (const mplayer::native::UpscalerModel& model : pendingUpscalers_)
                bytes += model.bytes;
            if (selectedRifeMissing) bytes += native::rifeArchivePack().assetBytes;
            // The closure's own queue is reviewable too, so the prompt is not
            // hiding an unknown beside the model sizes.
            if (backendWork) bytes += closureBytesIn(inventory, backend);
            acquisitionChecking_ = false;
            acquisitionCheckCancelFlag_.reset();
            updateControls();
            if (showReview) {
                const QString prompt = tr("The following selected packs are missing or need repair:\n%1\n\nEstimated download size: %2\n\nContinue?")
                    .arg(review.join(QStringLiteral("\n")),
                         bytes > 0 ? QLocale().formattedDataSize(bytes) : tr("Unknown"));
                if (QMessageBox::question(this, tr("Review installation"), prompt,
                                          QMessageBox::Yes | QMessageBox::Cancel,
                                          QMessageBox::Cancel) != QMessageBox::Yes) {
                    pendingModels_.clear();
                pendingUpscalers_.clear();
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
            // RTX has no separate store packs: the runtime installer owns its
            // whole closure, so it never takes the provisioner pack path.
            pendingRuntimeClosure_ =
                backend == native::Backend::TensorRTRTX
                    ? backendWork
                    : (runtimeSelected && !backendInstalled);
            pendingRifeArchive_ = selectedRifeMissing;
            downloadRemainingComponents();
        },
        [this, live, generation](const backend::Failure& failure) {
            if (!live->load() || generation != runGeneration_) return;
            acquisitionChecking_ = false;
            acquisitionSequenceActive_ = false;
            acquisitionCheckCancelFlag_.reset();
            pendingModels_.clear();
            pendingUpscalers_.clear();
            const QString cause = backend::failureLine(failure);
            applyStatus(QStringLiteral("native.components.fetch"),
                        StatusReason{.kind = StatusReason::Kind::CheckFailed, .detail = cause});
            updateControls();
        });
}

void ComponentStoreTab::downloadAnimeJanaiRuntime() {
    if (runner_ == nullptr) {
        const native::Backend backend = selectedBackend();
        applyStatus(QStringLiteral("native.components.noRunner"),
                    StatusReason{.kind = StatusReason::Kind::ClosureNeedsRunner,
                                 .backend = backend});
        pendingModels_.clear();
        updateControls();
        return;
    }

    animeJanaiRuntimeDownloading_ = true;
    animeJanaiCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelFlag = animeJanaiCancelFlag_;
    const native::Backend backend = selectedBackend();
    const quint64 generation = acquisitionGeneration_ == 0 ? runGeneration_
                                                           : acquisitionGeneration_;
    applyStatus(QString(), StatusReason{.kind = StatusReason::Kind::Busy,
                                        .busy = StatusReason::Busy::Runtime});
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
            const native::ComponentProvisionResult result = native::provisionAnimeJanaiRuntime(
                backend,
                [publish](const native::ComponentProvisionProgress& progress) {
                    const native::ComponentProvisionProgress at = progress;
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
                settleProgressCard(QStringLiteral("tensorRtRuntimeCard"), false, true);
                settleProgressCard(QStringLiteral("tensorRtKernelsCard"), false, true);
                finishAcquisitionUnit(QString(), true);
            } else if (!value.toMap().value(QStringLiteral("ok")).toBool()) {
                const QString reason = value.toMap().value(QStringLiteral("reason")).toString();
                const QString reasonKey =
                    value.toMap().value(QStringLiteral("reasonKey")).toString();
                const QString backendName = backendProductName(backend);
                const QString detail =
                    reason.isEmpty() ? tr("AnimeJaNai runtime installation failed.") : reason;
                settleProgressCard(QStringLiteral("tensorRtRuntimeCard"), false, false, detail);
                settleProgressCard(QStringLiteral("tensorRtKernelsCard"), false, false, detail);
                finishAcquisitionUnit(
                    tr("%1 installation failed: %2").arg(backendName, detail),
                    false, reasonKey);
            } else {
                settleProgressCard(QStringLiteral("tensorRtRuntimeCard"), true, false);
                settleProgressCard(QStringLiteral("tensorRtKernelsCard"), true, false);
                verifiedAnimeJanaiBackends_.remove(static_cast<int>(backend));
                finishAcquisitionUnit(QString(), false);
            }
        },
        [this, alive = alive_, generation, cancelFlag, backend](const backend::Failure& failure) {
            if (!alive->load() || runGeneration_ != generation) return;
            animeJanaiRuntimeDownloading_ = false;
            animeJanaiCancelFlag_.reset();
            if (cancelFlag->load()) {
                settleProgressCard(QStringLiteral("tensorRtRuntimeCard"), false, true);
                settleProgressCard(QStringLiteral("tensorRtKernelsCard"), false, true);
                finishAcquisitionUnit(QString(), true);
                return;
            }
            const QString backendName = backendProductName(backend);
            const QString detail = backend::failureLine(failure);
            settleProgressCard(QStringLiteral("tensorRtRuntimeCard"), false, false, detail);
            settleProgressCard(QStringLiteral("tensorRtKernelsCard"), false, false, detail);
            finishAcquisitionUnit(tr("%1 installation failed: %2").arg(backendName, detail),
                                  false);
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
        selectedBackend() == native::Backend::TensorRT &&
        (selectedPacks_.contains(QStringLiteral("component:runtime")) ||
         selectedPacks_.contains(QStringLiteral("component:kernels"))) &&
        native::observeComputeCapability().smMajor > 0 && !componentDirsAvailable_;
    if (componentMissing && provisioner_ == nullptr) {
        pendingModels_.clear();
        acquisitionSequenceActive_ = false;
        applyStatus(QStringLiteral("native.components.missing"),
                    StatusReason{.kind = StatusReason::Kind::MissingProvisioner});
        updateControls();
        return;
    }
    currentProgressItems_.clear();
    if (pendingBackendWork_) {
        clearProgressCard(QStringLiteral("tensorRtKernelsCard"));
        // One place decides the closure queue: the two RTX archives with their
        // file names, or the classic pack rows. No classic pack is ever listed
        // for RTX.
        queueRuntimeClosureRows();
    }
    if (pendingRifeArchive_) {
        clearProgressCard(QStringLiteral("rifeModelsCard"));
        const native::RifeArchivePack& pack = native::rifeArchivePack();
        setProgressItem(pack.name, QStringLiteral("rifeModelsCard"),
                        QStringLiteral("%1 (%2)").arg(pack.name, pack.asset),
                        native::ComponentProvisionStage::Downloading, 0, pack.assetBytes, true);
    }
    if (!pendingModels_.isEmpty()) {
        clearProgressCard(QStringLiteral("animeJanaiModelsCard"));
        const QVector<native::SpatialAsset> catalog = native::spatialAssetCatalog();
        for (const int index : pendingModels_) {
            const native::SpatialAsset& asset = catalog.at(index);
            setProgressItem(asset.id, QStringLiteral("animeJanaiModelsCard"), asset.name,
                            native::ComponentProvisionStage::Downloading, 0, asset.bytes, true);
        }
    }
    if (!pendingUpscalers_.isEmpty()) {
        clearProgressCard(QStringLiteral("upscalerModelsCard"));
        for (const mplayer::native::UpscalerModel& model : pendingUpscalers_) {
            setProgressItem(model.sha256, QStringLiteral("upscalerModelsCard"),
                            model.displayName, native::ComponentProvisionStage::Downloading, 0,
                            model.bytes, true);
        }
    }
    // The independent units run at the same time: the backend closure, the RIFE
    // archive and the spatial graphs share no scratch, no destination and no
    // record, so serialising them only made the whole acquisition as slow as the
    // sum of its parts. The run is over when the last unit settles, which is
    // what keeps the window's aggregate activity hint honest while several
    // transfers run at once.
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
    if (!pendingUpscalers_.isEmpty()) noteAcquisitionUnitStarted();
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
                settleProgressCard(QStringLiteral("tensorRtRuntimeCard"), false, false, reason);
                settleProgressCard(QStringLiteral("tensorRtKernelsCard"), false, false, reason);
                finishAcquisitionUnit(reason, false);
            } else {
                updateControls();
            }
        }
    }
    if (pendingRifeArchive_) downloadRifeArchive();
    if (!pendingModels_.isEmpty()) downloadNextModel();
    if (!pendingUpscalers_.isEmpty()) downloadNextUpscaler();
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
    const native::RifeArchiveStore store = native::rifeArchiveStore();
    const QVector<native::RifeModelPin> pins = mplayer::native::rifeModelPins();
    if (runner_ == nullptr) {
        finishAcquisitionUnit(tr("The RIFE archive download needs a runner."), false,
                              QStringLiteral("native.components.noClient"));
        return;
    }

    rifeDownloading_ = true;
    rifeCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelFlag = rifeCancelFlag_;
    const quint64 generation = acquisitionGeneration_ == 0 ? runGeneration_
                                                           : acquisitionGeneration_;
    applyStatus(QString(), StatusReason{.kind = StatusReason::Kind::Busy,
                                        .busy = StatusReason::Busy::Rife});
    updateControls();

    // The binder is taken on the GUI thread, before the work starts; the
    // worker only ever calls it.
    ProgressPublisher publish{runner_->post(), this, alive_, generation};
    runner_->runTransfer(
        [store, pins, cancelFlag, publish]() -> QVariant {
            const native::ComponentProvisionResult result = native::provisionRifeArchive(
                store, pins,
                [publish](const native::ComponentProvisionProgress& progress) {
                    const native::ComponentProvisionProgress at = progress;
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
            settleProgressCard(QStringLiteral("rifeModelsCard"), true, false);
            for (const native::RifeModelPin& pin : pins) {
                verifiedRifeGraphs_.insert(pin.fileName);
                rifeVerificationErrors_.remove(pin.fileName);
            }
            pendingRifeArchive_ = false;
            schedulePresenceRefresh();
            finishAcquisitionUnit(QString(), false);
        },
        [this, alive = alive_, generation, cancelFlag](const backend::Failure& failure) {
            if (!alive->load() || runGeneration_ != generation) return;
            rifeDownloading_ = false;
            rifeCancelFlag_.reset();
            if (cancelFlag->load()) {
                settleProgressCard(QStringLiteral("rifeModelsCard"), false, true);
                finishAcquisitionUnit(QString(), true);
                return;
            }
            rifeFailure_ = backend::failureLine(failure);
            settleProgressCard(QStringLiteral("rifeModelsCard"), false, false, rifeFailure_);
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
    const QVector<native::SpatialAsset> catalog = native::spatialAssetCatalog();
    if (!modelsDownloading_) {
        modelsDownloading_ = true;
        modelCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
        modelDownloadsInFlight_ = 0;
        modelDownloadsCompleted_ = 0;
        modelDownloadsTotal_ = pendingModels_.size();
        modelFailure_.clear();
        modelCancelled_ = false;
    }
    const quint64 generation = acquisitionGeneration_ == 0 ? runGeneration_
                                                           : acquisitionGeneration_;
    const auto cancelFlag = modelCancelFlag_;
    applyStatus(QString(), StatusReason{.kind = StatusReason::Kind::Busy,
                                        .count = modelDownloadsTotal_,
                                        .busy = StatusReason::Busy::Models});
    updateControls();
    backend::MikaiClient* client = client_;
    const QVector<int> batch = std::exchange(pendingModels_, {});
    modelDownloadsInFlight_ += batch.size();
    const ProgressPublisher publish{runner_->post(), this, alive_, generation};
    for (int index : batch) {
        const native::SpatialAsset asset = catalog.at(index);
        runner_->runTransfer(
            [client, asset, cancelFlag, publish]() -> QVariant {
                if (cancelFlag->load()) return QVariant{};
                // The fetch's blocking wait observes a nested scope over the
                // tab's own gate, so Cancel pierces the transfer on the same
                // clock the runtime/RIFE downloads use instead of waiting for
                // the whole file or the artwork timeout.
                const backend::WorkCancellationScope tabCancel(cancelFlag.get());
                const QByteArray bytes = client->fetchBytes(
                    asset.url, kModelDownloadTimeoutMs,
                    [publish, asset](qint64 received, qint64 total) {
                        const native::ComponentProvisionProgress progress{
                            asset.id, received, total, native::ComponentProvisionStage::Downloading};
                        publish([progress](ComponentStoreTab& tab) {
                            tab.applyDownloadProgress(progress);
                        });
                    },
                    backend::MikaiClient::Redirects::FollowSafe);
                if (cancelFlag->load()) return QVariant{};
                const native::ComponentProvisionProgress installing{
                    asset.id, static_cast<qint64>(bytes.size()), asset.bytes,
                    native::ComponentProvisionStage::Installing};
                publish([installing](ComponentStoreTab& tab) {
                    tab.applyDownloadProgress(installing);
                });
                const native::SpatialAssetPublication published =
                    native::publishSpatialAssetBytes(asset, bytes, QString());
                if (!published.ok) {
                    throw std::runtime_error(
                        QStringLiteral("%1: %2").arg(published.reasonKey, published.reason)
                            .toStdString());
                }
                const native::ComponentProvisionProgress installed{
                    asset.id, asset.bytes, asset.bytes, native::ComponentProvisionStage::Installed};
                publish([installed](ComponentStoreTab& tab) {
                    tab.applyDownloadProgress(installed);
                });
                return QVariant{};
            },
            [this, alive = alive_, generation, cancelFlag, asset](const QVariant&) {
                if (!alive->load() || runGeneration_ != generation) return;
                --modelDownloadsInFlight_;
                if (cancelFlag->load()) {
                    settleProgressItem(asset.id, false, true);
                    modelCancelled_ = true;
                } else {
                    settleProgressItem(asset.id, true, false);
                    ++modelDownloadsCompleted_;
                    // Publish the new presence now: the badge counts up
                    // 1/5 ... 5/5 instead of jumping when the run ends.
                    schedulePresenceRefresh();
                }
                if (modelDownloadsInFlight_ == 0) finishModelDownloads();
                else publishAcquisition();
            },
            [this, alive = alive_, generation, cancelFlag, asset](const backend::Failure& failure) {
                if (!alive->load() || runGeneration_ != generation) return;
                --modelDownloadsInFlight_;
                if (cancelFlag->load()) {
                    settleProgressItem(asset.id, false, true);
                    modelCancelled_ = true;
                } else {
                    const QString detail = backend::failureLine(failure);
                    settleProgressItem(asset.id, false, false, detail);
                    if (modelFailure_.isEmpty()) modelFailure_ = detail;
                }
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
                    StatusReason{.kind = StatusReason::Kind::VerifyNeedsRunner});
        refresh();
        return;
    }
    // A proof run is not a transfer: it sets its own flag, never
    // `modelsDownloading_`, so `downloadActive()` stays false and the card
    // labels cannot report a download while hashing runs.
    bumpRunGeneration(QStringLiteral("acquisition verification started"));
    verificationActive_ = true;
    modelCancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    const quint64 generation = runGeneration_;
    const auto cancelled = modelCancelFlag_;
    const native::GpuComputeCapability gpu = native::observeComputeCapability();
    const bool spatialSelected =
        selectedPacks_.contains(QStringLiteral("component:spatial"));
    if (spatialSelected) {
        applyStatus(QString(),
                    StatusReason{.kind = StatusReason::Kind::VerificationProgress,
                                 .downloaded = qMax(modelInstalledCount(),
                                                    downloadedModelItemCount()),
                                 .verified = 0,
                                 .total = static_cast<int>(native::spatialAssetCatalog().size())});
    } else {
        applyStatus(QString(), StatusReason{.kind = StatusReason::Kind::Busy,
                                            .busy = StatusReason::Busy::Verification});
    }
    updateControls();
    // A completed transfer may have left a confirmed 100 % bar; the proof run
    // must render indeterminate from its first frame.
    renderAllCardProgress();
    const std::shared_ptr<std::atomic<bool>> live = alive_;
    runner_->runTransfer(
        [gpu, cancelled] {
            return component_store_detail::inventorySnapshot(gpu, true, true, cancelled.get());
        },
        [this, live, generation, cancelled, gpu](const QVariant& value) {
            if (!live->load() || generation != runGeneration_) return;
            const QVariantMap result = value.toMap();
            verificationActive_ = false;
            modelCancelFlag_.reset();
            acquisitionSequenceActive_ = false;
            if (cancelled->load() || result.value(QStringLiteral("cancelled")).toBool()) {
                applyStatus(QStringLiteral("native.components.cancelled"), QString());
                renderAllCardProgress();
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
            const native::Backend backend = selectedBackend();
            // The selected backend answers from its own fields; RTX is never
            // proved by a classic record and vice versa.
            if (runtimeSelected && !installationInstalledIn(result, backend)) {
                const QString reasonKey =
                    backend == native::Backend::TensorRT
                        ? QStringLiteral("trtReason")
                        : (backend == native::Backend::DirectML ? QStringLiteral("directMlReason")
                                                        : QStringLiteral("rtxReason"));
                const QString reason = result.value(reasonKey).toString();
                failures.append(reason.isEmpty()
                                    ? tr("The requested backend installation could not be verified.")
                                    : reason);
            }
            if (backend == native::Backend::TensorRT && gpu.smMajor > 0) {
                native::TensorRtClosureStatus closure;
                closure.runtimeStored =
                    result.value(QStringLiteral("closureRuntimeStored")).toBool();
                closure.kernelsStored =
                    result.value(QStringLiteral("closureKernelsStored")).toBool();
                closure.bundleProvidesDirs =
                    result.value(QStringLiteral("closureBundleDirs")).toBool();
                closure.dirsAvailable =
                    result.value(QStringLiteral("componentDirsAvailable")).toBool();
                QString kernelsName = result.value(QStringLiteral("kernelsName")).toString();
                if (kernelsName.isEmpty())
                    kernelsName = QStringLiteral("trt-sm%1%2").arg(gpu.smMajor).arg(gpu.smMinor);
                for (const QString& line : component_store_detail::tensorRtClosureFailureLines(
                         closure, runtimeSelected, kernelsSelected, kernelsName)) {
                    if (!failures.contains(line)) failures.append(line);
                }
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
                for (const native::SpatialAsset& asset : native::spatialAssetCatalog()) {
                    const native::StoredSpatialAsset* row = component_store_detail::spatialInventoryRow(storedSpatial_, asset.id);
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
                if (spatialSelected) {
                    applyStatus(QString(),
                                StatusReason{.kind = StatusReason::Kind::InventoryCounts,
                                             .verified = modelVerifiedCount(),
                                             .installed = modelInstalledCount(),
                                             .total = static_cast<int>(native::spatialAssetCatalog().size())});
                } else {
                    applyStatus(QString(), StatusReason{.kind = StatusReason::Kind::AllVerified});
                }
            } else if (spatialSelected) {
                applyStatus(QStringLiteral("native.components.digest"),
                            StatusReason{.kind = StatusReason::Kind::FailureCounts,
                                         .causes = failures,
                                         .installed = modelInstalledCount(),
                                         .total = static_cast<int>(native::spatialAssetCatalog().size())});
            } else {
                applyStatus(QStringLiteral("native.components.digest"),
                            failures.join(QStringLiteral("; ")));
            }
            refresh();
        },
        [this, live, generation, cancelled](const backend::Failure& failure) {
            if (!live->load() || generation != runGeneration_) return;
            verificationActive_ = false;
            modelCancelFlag_.reset();
            acquisitionSequenceActive_ = false;
            if (cancelled->load()) {
                applyStatus(QStringLiteral("native.components.cancelled"), QString());
            } else {
                applyStatus(QStringLiteral("native.components.fetch"),
                            StatusReason{.kind = StatusReason::Kind::NotVerified,
                                         .detail = backend::failureLine(failure)});
            }
            renderAllCardProgress();
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

void ComponentStoreTab::updateControls() {
    if (downloadButton_ == nullptr) return;
    const bool provisioning = provisioner_ != nullptr && provisioner_->running();
    // Every long-running state, including the verification itself, disables the
    // actions that would start another one: the surface is non-reentrant while
    // any worker is alive.
    const bool busy = provisioning || animeJanaiRuntimeDownloading_ || rifeDownloading_ ||
                      modelsDownloading_ || verifyActive() || acquisitionChecking_ ||
                      inventoryScanning_ || acquisitionSequenceActive_;
    refreshSelectionSummary();
    // Nothing selected is nothing to install: the emphasised action dims instead
    // of starting a run that would find no work.
    const bool hasSelection = downloadButton_->property("hasSelection").toBool();
    downloadButton_->setEnabled(!busy && hasSelection);
    verifyButton_->setEnabled(!busy);
    cancelButton_->setEnabled(busy);
    // Visible only while there is something to stop: the dialog's own footer
    // already offers Cancel, and a permanently visible second one reads as a
    // duplicate rather than as "stop this download".
    cancelButton_->setVisible(busy);
    if (runtimeCheck_ != nullptr) runtimeCheck_->setEnabled(!busy);
    if (rifeCheck_ != nullptr) rifeCheck_->setEnabled(!busy);
    if (spatialCheck_ != nullptr) spatialCheck_->setEnabled(!busy);
    if (kernelsCheck_ != nullptr) kernelsCheck_->setEnabled(!busy);
    if (animeJanaiBackendCombo_ != nullptr) animeJanaiBackendCombo_->setEnabled(!busy);
    publishAcquisition();
}

void ComponentStoreTab::refreshSelectionSummary() {
    int checked = 0;
    int selectable = 0;
    for (QGroupBox* card : findChildren<QGroupBox*>(QStringLiteral("nativeComponentPackCard"))) {
        const QString cardId = card->property("componentCardId").toString();
        auto* check = card->findChild<QCheckBox*>(cardId + QStringLiteral("Check"));
        if (check == nullptr || !check->isEnabled()) continue;
        ++selectable;
        if (check->isChecked()) ++checked;
    }
    if (selectAllCheck_ != nullptr) {
        selectAllCheck_->setEnabled(selectable > 0);
        const QSignalBlocker block(selectAllCheck_);
        selectAllCheck_->setCheckState(checked == 0                       ? Qt::Unchecked
                                       : checked == selectable            ? Qt::Checked
                                                                          : Qt::PartiallyChecked);
    }
    if (selectionSummary_ != nullptr) {
        // A count, not a plural form: the catalogue stores one numerus form per
        // language here, and a language with three forms is not worth a crash
        // for a five-word label.
        selectionSummary_->setText(checked == 0 ? tr("Nothing selected")
                                                : tr("Selected: %1").arg(checked));
    }
    if (downloadButton_ != nullptr) downloadButton_->setProperty("hasSelection", checked > 0);
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
    if (verifyActive()) {
        // The in-flight hash run's delivery is discarded rather than
        // published: nothing proves a verdict from a run nobody waited for,
        // and no partial result may be mistaken for the answer. The bump also
        // releases every latch the abandoned run held.
        if (modelCancelFlag_) modelCancelFlag_->store(true);
        modelCancelFlag_.reset();
        bumpRunGeneration(QStringLiteral("verification cancelled"));
        applyStatus(QStringLiteral("native.components.cancelled"),
                    StatusReason{.kind = StatusReason::Kind::VerificationCancelled});
        updateControls();
        return;
    }
    // The units run at the same time, so one Cancel arms every unit's gate: the
    // run stays in flight until each worker observes it and settles, which is
    // what keeps the aggregate activity hint honest while several transfers
    // unwind.
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
                    StatusReason{.kind = StatusReason::Kind::DownloadsCancelling});
        updateControls();
        return;
    }
    modelsDownloading_ = false;
    acquisitionUnitsPending_ = 0;
    pendingModels_.clear();
    for (const QString& cardId : {QStringLiteral("tensorRtRuntimeCard"),
                                  QStringLiteral("tensorRtKernelsCard"),
                                  QStringLiteral("rifeModelsCard"),
                                  QStringLiteral("animeJanaiModelsCard")}) {
        settleProgressCard(cardId, false, true);
    }
    applyStatus(QStringLiteral("native.components.cancelled"), QString());
    updateControls();
}

void ComponentStoreTab::syncAcquisitionState() {
    publishAcquisition();
}

void ComponentStoreTab::publishAcquisition() {
    const bool downloading = (provisioner_ != nullptr && provisioner_->running()) ||
                             animeJanaiRuntimeDownloading_ || rifeDownloading_ ||
                             modelsDownloading_ || verifyActive() || acquisitionChecking_ ||
                             acquisitionSequenceActive_;
    if (!downloading) {
        if (!acquisitionAnnounced_) return;
        acquisitionAnnounced_ = false;
        emit acquisitionIdle();
        return;
    }
    const bool started = !acquisitionAnnounced_;
    acquisitionAnnounced_ = true;
    const int activeItems = activeProgressItemCount();
    // Bytes the active transfers have actually received, so the hint answers
    // "is it moving?" with a moving number instead of an item count that stays
    // constant for the whole run.
    qint64 received = 0;
    qint64 total = 0;
    for (auto item = progressItems_.cbegin(); item != progressItems_.cend(); ++item) {
        if (item->outcome != ProgressItem::Outcome::Active || item->queued) continue;
        received += item->received;
        total += item->total;
    }
    QString status;
    if (activeItems > 0 && total > 0) {
        status = tr("Downloading %1 items: %2 of %3")
                     .arg(activeItems)
                     .arg(QLocale().formattedDataSize(received),
                          QLocale().formattedDataSize(total));
    } else if (activeItems > 0) {
        status = tr("%1 component items active").arg(activeItems);
    } else if (acquisitionChecking_) {
        status = tr("Checking selected components...");
    } else if (verifyActive()) {
        status = tr("Verifying installed components...");
    } else {
        status = tr("Installing selected components...");
    }
    const int percent =
        total > 0 ? static_cast<int>(qBound<qint64>(qint64(0), received * 100 / total, qint64(100)))
                  : -1;
    emit acquisitionProgress(status, started, percent);
}

void ComponentStoreTab::downloadNextUpscaler() {
    if (pendingUpscalers_.isEmpty()) {
        upscalersDownloading_ = false;
        // X-C4/X-M6: success — no failure, not cancelled — so
        // finalizeAcquisition() proceeds to startAcquisitionVerification().
        // The old call passed a message as a failure plus cancelled=true,
        // which reported a successful install as cancelled and skipped
        // verification (and swallowed the other units' verification).
        finishAcquisitionUnit({}, false);
        return;
    }
    if (runner_ == nullptr || client_ == nullptr) {
        upscalersDownloading_ = false;
        pendingUpscalers_.clear();
        finishAcquisitionUnit(tr("An upscaler download needs the backend client and a runner."),
                              false, QStringLiteral("native.components.noClient"));
        return;
    }
    upscalersDownloading_ = true;
    backend::MikaiClient* client = client_;
    const mplayer::native::UpscalerModel model = pendingUpscalers_.takeFirst();
    const quint64 generation = acquisitionGeneration_ == 0 ? runGeneration_
                                                           : acquisitionGeneration_;
    const auto cancelFlag = modelCancelFlag_;
    applyStatus(QString(), StatusReason{.kind = StatusReason::Kind::Busy,
                                        .count = 1,
                                        .busy = StatusReason::Busy::Models});
    updateControls();
    const ProgressPublisher publish{runner_->post(), this, alive_, generation};
    runner_->runTransfer(
        [client, model, cancelFlag, publish]() -> QVariant {
            if (cancelFlag != nullptr && cancelFlag->load()) return QVariant{};
            const backend::WorkCancellationScope scope(cancelFlag.get());
            const QByteArray bytes = client->fetchBytes(
                model.url, kModelDownloadTimeoutMs,
                [publish, model](qint64 received, qint64 total) {
                    const native::ComponentProvisionProgress progress{
                        model.sha256, received, total, native::ComponentProvisionStage::Downloading};
                    publish([progress](ComponentStoreTab& tab) {
                        tab.applyDownloadProgress(progress);
                    });
                },
                backend::MikaiClient::Redirects::FollowSafe);
            if (cancelFlag != nullptr && cancelFlag->load()) return QVariant{};
            const native::ComponentProvisionProgress installing{
                model.sha256, static_cast<qint64>(bytes.size()), model.bytes,
                native::ComponentProvisionStage::Installing};
            publish([installing](ComponentStoreTab& tab) {
                tab.applyDownloadProgress(installing);
            });
            QString reason;
            const QString registered = mplayer::native::installUpscalerBytes(model, bytes, &reason);
            if (registered.isEmpty()) {
                throw std::runtime_error(reason.isEmpty() ? "not installed" : reason.toStdString());
            }
            return QVariant(registered);
        },
        [this, alive = alive_, model, generation, cancelFlag](const QVariant&) {
            if (!alive->load() || runGeneration_ != generation) return;
            if (cancelFlag != nullptr && cancelFlag->load()) {
                settleProgressItem(model.sha256, false, true);
                pendingUpscalers_.clear();
                upscalersDownloading_ = false;
                finishAcquisitionUnit(tr("Upscale model download cancelled."), true);
                return;
            }
            settleProgressItem(model.sha256, true, false);
            updateCardStates();
            downloadNextUpscaler();
        },
        [this, alive = alive_, model, generation](const backend::Failure& failure) {
            if (!alive->load() || runGeneration_ != generation) return;
            settleProgressItem(model.sha256, false, false);
            pendingUpscalers_.clear();
            upscalersDownloading_ = false;
            finishAcquisitionUnit(backend::failureLine(failure), false);
        });
}

QString ComponentStoreTab::renderStatusReason() const {
    const auto backendName = [this](native::Backend backend) {
        return backendProductName(backend);
    };
    switch (statusReason_.kind) {
        case StatusReason::Kind::None:
            return QString();
        case StatusReason::Kind::Raw:
            return statusReason_.detail;
        case StatusReason::Kind::InventoryFailed:
            return tr("Component inventory failed: %1").arg(statusReason_.detail);
        case StatusReason::Kind::VerifyNeedsRunner:
            return tr("Verifying the installed components needs a runner.");
        case StatusReason::Kind::VerifiedCount:
            return tr("%1 elements verified, none changed.").arg(statusReason_.count);
        case StatusReason::Kind::VerifyDigest: {
            QStringList reported;
            for (const QString& entry : statusReason_.causes) {
                const int separator = entry.indexOf(QLatin1Char(':'));
                const QString label = entry.left(separator);
                const QString entryDetail = entry.mid(separator + 1);
                if (label == QLatin1String("tensorrt"))
                    reported.append(tr("TensorRT / CUDA 13 closure: %1")
                                        .arg(entryDetail ==
                                                     QLatin1String(
                                                         "animejanai.installation.incomplete")
                                                 ? tr("one or more required dependencies are "
                                                      "missing or corrupt")
                                                 : entryDetail));
                else if (label == QLatin1String("directml"))
                    reported.append(tr("DirectML closure: %1")
                                        .arg(entryDetail ==
                                                     QLatin1String(
                                                         "animejanai.installation.incomplete")
                                                 ? tr("one or more required dependencies are "
                                                      "missing or corrupt")
                                                 : entryDetail));
                else if (label == QLatin1String("tensorrt-rtx"))
                    reported.append(tr("TensorRT for RTX closure: %1")
                                        .arg(entryDetail ==
                                                     QLatin1String(
                                                         "animejanai.installation.incomplete")
                                                 ? tr("one or more required dependencies are "
                                                      "missing or corrupt")
                                                 : entryDetail));
                else
                    reported.append(entry);
            }
            if (statusReason_.rifeFailures > 0)
                reported.append(tr("%1 RIFE interpolation model(s) failed verification.")
                                    .arg(statusReason_.rifeFailures));
            if (statusReason_.spatialFailures > 0)
                reported.append(tr("%1 spatial model(s) failed verification.")
                                    .arg(statusReason_.spatialFailures));
            return tr("%1 elements failed verification: %2")
                .arg(statusReason_.count)
                .arg(reported.join(QStringLiteral("; ")));
        }
        case StatusReason::Kind::NotVerified:
            return tr("The components could not be verified: %1").arg(statusReason_.detail);
        case StatusReason::Kind::CheckNeedsRunner:
            return tr("Checking selected component state needs a runner.");
        case StatusReason::Kind::NothingToInstall:
            return tr("No selected missing packs need installation.");
        case StatusReason::Kind::CheckFailed:
            return tr("Component check failed: %1").arg(statusReason_.detail);
        case StatusReason::Kind::ClosureNeedsRunner:
            return tr("Installing the selected AnimeJaNai %1 closure needs a runner.")
                .arg(backendName(statusReason_.backend));
        case StatusReason::Kind::VerificationCancelled:
            return tr("Verification was cancelled.");
        case StatusReason::Kind::DownloadsCancelling:
            return tr("Cancelling the running downloads...");
        case StatusReason::Kind::MissingProvisioner:
            return tr("The compatible TensorRT component pack is missing, but no component "
                      "provisioner is available.");
        case StatusReason::Kind::AllVerified:
            return tr("Every selected pack is installed and verified.");
        case StatusReason::Kind::Busy:
            switch (statusReason_.busy) {
                case StatusReason::Busy::None:
                    return QString();
                case StatusReason::Busy::Checking:
                    return tr("Checking selected components...");
                case StatusReason::Busy::Runtime:
                    return tr("Installing AnimeJaNai runtime and selected backend dependencies...");
                case StatusReason::Busy::Rife:
                    return tr("Downloading the RIFE 3.7.0 graph archive...");
                case StatusReason::Busy::Models:
                    return tr("Downloading %1 upscaling models...").arg(statusReason_.count);
                case StatusReason::Busy::Verification:
                    return tr("Verifying installed components...");
                case StatusReason::Busy::InstalledVerification:
                    return tr("Verifying the installed components...");
            }
            return QString();
        case StatusReason::Kind::VerificationProgress:
            // Transferred is not proved: the counts say downloaded until the
            // hashing that is running now returns.
            return tr("Verifying installed components... Downloaded: %1/%2 · Verified: %3/%2")
                .arg(statusReason_.downloaded)
                .arg(statusReason_.total)
                .arg(statusReason_.verified);
        case StatusReason::Kind::InventoryCounts:
            return tr("Installed: %1/%2 · Verified: %3/%2")
                .arg(statusReason_.installed)
                .arg(statusReason_.total)
                .arg(statusReason_.verified);
        case StatusReason::Kind::FailureCounts:
            return tr("Installed: %1/%2 · Failed: %3")
                .arg(statusReason_.installed)
                .arg(statusReason_.total)
                .arg(statusReason_.causes.join(QStringLiteral("; ")));
    }
    return QString();
}

void ComponentStoreTab::applyStatus(const QString& key, const QString& reason) {
    applyStatus(key, StatusReason{.kind = StatusReason::Kind::Raw, .detail = reason});
}

void ComponentStoreTab::applyStatus(const QString& key,
                                    const StatusReason& reason) {
    statusKey_ = key;
    statusReason_ = reason;
    const QString rendered = renderStatusReason();
    statusRenderedReason_ = rendered;
    if (status_ != nullptr) {
        const bool busy =
            reason.kind == StatusReason::Kind::Busy ||
            reason.kind == StatusReason::Kind::VerificationProgress;
        status_->setText(rendered);
        const QString severity =
            busy ? QStringLiteral("busy")
                 : (key == QStringLiteral("native.components.cancelled")
                        ? QStringLiteral("cancelled")
                        : (key.isEmpty() ? (rendered.isEmpty() ? QStringLiteral("neutral")
                                                               : QStringLiteral("success"))
                                         : QStringLiteral("failure")));
        status_->setProperty("statusSeverity", severity);
        // Muted, never red or green: a busy line describes work, not a verdict.
        status_->setProperty("textRole", busy ? QStringLiteral("muted") : QString());
        status_->setAccessibleDescription(severity);
        status_->style()->unpolish(status_);
        status_->style()->polish(status_);
    }
    if (!key.isEmpty()) {
        qWarning().noquote() << "native components:" << key << rendered;
    }
}

void ComponentStoreTab::restateStatus() {
    // A progress or stage line that took the label over is not this status:
    // only a label still showing the last-rendered reason is restated, so a
    // language switch never overwrites a live transfer readout.
    if (status_ != nullptr && status_->text() == statusRenderedReason_) {
        // Re-apply the typed reason instead of assigning text: the same
        // pipeline re-derives the wording, severity and muted role in the live
        // language, so a language switch cannot leave a raw line behind.
        applyStatus(statusKey_, statusReason_);
    }
    // Reuse the existing notification: a run that was already announced gets
    // its progress line restated, anything else is restated through the idle
    // path, which redraws the retained failure reason. Neither starts, stops
    // nor resets the run, its progress, or its timers.
    if (acquisitionAnnounced_) {
        publishAcquisition();
    } else if (!acquisitionRunning()) {
        // A run that never announced itself has no activity hint to restate;
        // idle here would show a stale retained failure over it.
        emit acquisitionIdle();
    }
}

}  // namespace mikai::app
