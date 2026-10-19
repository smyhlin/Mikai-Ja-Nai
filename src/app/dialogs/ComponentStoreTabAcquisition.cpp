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
#include <functional>

namespace mikai::app {

using namespace mplayer::native;


using namespace component_store_detail;

namespace {
/// A model graph is ~2 MB but the link may be slow; 20 s was an artwork budget.
constexpr int kModelDownloadTimeoutMs = 120000;


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
                    StatusReason{.kind = StatusReason::Kind::CheckNeedsRunner});
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
                applyStatus(QString(), StatusReason{.kind = StatusReason::Kind::NothingToInstall});
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
            const QString cause = backend::failureLine(failure);
            applyStatus(QStringLiteral("native.components.fetch"),
                        StatusReason{.kind = StatusReason::Kind::CheckFailed, .detail = cause});
            updateControls();
        });
}

void ComponentStoreTab::downloadAnimeJanaiRuntime() {
    if (runner_ == nullptr) {
        const Backend backend = selectedBackend();
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
                    StatusReason{.kind = StatusReason::Kind::RuntimeNeedsRunner});
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
                applyStatus(
                    reasonKey.isEmpty() ? QStringLiteral("native.components.fetch") : reasonKey,
                    StatusReason{.kind = StatusReason::Kind::ImportFailed,
                                 .detail = reason,
                                 .backend = backend});
            } else {
                verifiedAnimeJanaiBackends_.remove(static_cast<int>(backend));
                animeJanaiVerificationErrors_.remove(static_cast<int>(backend));
                applyStatus(QString(), StatusReason{.kind = StatusReason::Kind::RuntimeInstalled});
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
                const QString cause = backend::failureLine(failure);
                applyStatus(QStringLiteral("native.components.fetch"),
                            StatusReason{.kind = StatusReason::Kind::InstallFailed,
                                         .detail = cause,
                                         .backend = backend});
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
                    StatusReason{.kind = StatusReason::Kind::MissingProvisioner});
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
                    StatusReason{.kind = StatusReason::Kind::VerifyNeedsRunner});
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
                applyStatus(QString(), StatusReason{.kind = StatusReason::Kind::AllVerified});
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
                    StatusReason{.kind = StatusReason::Kind::VerificationCancelled});
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
                    StatusReason{.kind = StatusReason::Kind::DownloadsCancelling});
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

QString ComponentStoreTab::renderStatusReason() const {
    const auto backendName = [this](Backend backend) {
        return backend == Backend::TensorRT ? tr("TensorRT / CUDA 13") : tr("DirectML");
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
        case StatusReason::Kind::RuntimeNeedsRunner:
            return tr("Installing a local AnimeJaNai runtime needs a runner.");
        case StatusReason::Kind::ImportFailed:
            return tr("%1 installation failed: %2")
                .arg(backendName(statusReason_.backend),
                     statusReason_.detail.isEmpty()
                         ? tr("Local AnimeJaNai runtime installation failed.")
                         : statusReason_.detail);
        case StatusReason::Kind::RuntimeInstalled:
            return tr("Local AnimeJaNai runtime installed.");
        case StatusReason::Kind::VerificationCancelled:
            return tr("Verification was cancelled.");
        case StatusReason::Kind::DownloadsCancelling:
            return tr("Cancelling the running downloads...");
        case StatusReason::Kind::InstallFailed:
            return tr("%1 installation failed: %2")
                .arg(backendName(statusReason_.backend), statusReason_.detail);
        case StatusReason::Kind::MissingProvisioner:
            return tr("The compatible TensorRT component pack is missing, but no component "
                      "provisioner is available.");
        case StatusReason::Kind::AllVerified:
            return tr("Every selected pack is installed and verified.");
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
        status_->setText(rendered);
        const QString severity =
            key == QStringLiteral("native.components.cancelled")
                ? QStringLiteral("cancelled")
                : (key.isEmpty() ? (rendered.isEmpty() ? QStringLiteral("neutral")
                                                       : QStringLiteral("success"))
                                 : QStringLiteral("failure"));
        status_->setProperty("statusSeverity", severity);
        status_->setAccessibleDescription(severity);
        status_->style()->unpolish(status_);
        status_->style()->polish(status_);
    }
    if (!key.isEmpty()) {
        qWarning().noquote() << "native components:" << key << rendered;
    }
}

void ComponentStoreTab::restateStatus() {
    const QString reason = statusReason();
    // A progress or stage line that took the label over is not this status:
    // only a label still showing the last-rendered reason is restated, so a
    // language switch never overwrites a live transfer readout.
    if (status_ != nullptr && status_->text() == statusRenderedReason_) {
        status_->setText(reason);
        statusRenderedReason_ = reason;
    }
    // Reuse the existing notification: a run that was already announced gets
    // its progress line restated, anything else is restated through the idle
    // path, which redraws the retained failure reason. Neither starts, stops
    // nor resets the run, its progress, or its timers.
    if (acquisitionAnnounced_) {
        publishAcquisition();
    } else if (!acquisitionRunning()) {
        // A run that never announced itself has no bar to restate; emitting
        // idle here would show a stale retained failure over it.
        emit acquisitionIdle();
    }
}

}  // namespace mikai::app
