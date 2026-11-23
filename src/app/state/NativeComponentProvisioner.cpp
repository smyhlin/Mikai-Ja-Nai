#include "app/state/NativeComponentProvisioner.hpp"

#include "app/workers/Runner.hpp"

#include "native/components/NativeComponentCatalog.hpp"
#include "native/components/NativeComponentProvisioning.hpp"
#include "native/components/NativeComponentStore.hpp"
#include "native/components/NativeComponentToolchain.hpp"
#include "native/components/NativeGpuProbe.hpp"
#include "native/bundle/NativeBundle.hpp"
#include "native/bundle/NativePaths.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QMutex>
#include <QPointer>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <stdexcept>

namespace mikai::app {

namespace native = mplayer::native;

namespace {

QString translated(const char* message) {
    return QCoreApplication::translate("NativeComponentProvisioner", message);
}

struct ActiveRun {
    QPointer<NativeComponentProvisioner> owner;
    quint64 id = 0;
};

/// The one provisioner and run ID currently in flight, or empty once settled or
/// its owner has gone away. One run per process, so this is a class-wide slot
/// rather than per-instance state: delivered bodies resolve their owner through
/// it on the GUI thread. A QPointer — a weak, auto-nulling observer that never
/// keeps the provisioner alive — so destroying the owner nulls the slot even if
/// the destructor's own clear never runs.
ActiveRun& activeRun() {
    static ActiveRun slot;
    return slot;
}

quint64& nextRunId() {
    static quint64 counter = 0;
    return counter;
}

/// Guards activeRun() and nextRunId() (and the running_ check in the
/// destructor): the slot is claimed, read and cleared only under this lock.
QMutex& currentRunSlotMutex() {
    static QMutex mutex;
    return mutex;
}

/// The declared files of one bundle directory: every file whose path starts
/// with `<dirName>/`. One definition for the three call sites that used to
/// repeat this scan (ensure() / needsDownload()).
QStringList declaredFilesForDir(const native::Bundle& bundle, const QString& dirName) {
    QStringList declared;
    const QString prefix = dirName + QLatin1Char('/');
    for (const native::BundleFile& file : bundle.files) {
        if (file.path.startsWith(prefix)) {
            declared.append(file.path);
        }
    }
    return declared;
}

}  // namespace

NativeComponentProvisioner::NativeComponentProvisioner(Runner& runner, QObject* parent)
    : QObject(parent), runner_(runner) {}

bool NativeComponentProvisioner::hasActiveRun() {
    const QMutexLocker lock(&currentRunSlotMutex());
    return !activeRun().owner.isNull() && activeRun().id != 0;
}

NativeComponentProvisioner* NativeComponentProvisioner::currentRun(quint64 runId) {
    // Only ever called from a body the Runner's dispatcher delivered, which is
    // on the GUI thread, in the one moment the owner is known to be readable.
    const QMutexLocker lock(&currentRunSlotMutex());
    if (runId == 0 || activeRun().id != runId || activeRun().owner.isNull()) {
        return nullptr;
    }
    return activeRun().owner.data();
}

void NativeComponentProvisioner::runFinished(NativeComponentProvisioner* finished, quint64 runId) {
    // One settled run frees the slot so the same or another owner can run again
    // — but only if the slot still points at the run that settled. A second run
    // that re-claimed the slot must not have it yanked from under it, or its
    // completion would find no owner and its running_ would stay true forever.
    // GUI thread only: every delivery arrives through the Runner's dispatcher.
    const QMutexLocker lock(&currentRunSlotMutex());
    if (activeRun().owner == finished && activeRun().id == runId) {
        activeRun() = {};
    }
}

void NativeComponentProvisioner::finish(quint64 runId, bool ok, bool cancelled,
                                        const QString& summary) {
    if (runId == 0 || activeRunId_ != runId) {
        return;
    }
    runFinished(this, runId);
    activeRunId_ = 0;
    running_ = false;
    emit finished(ok, cancelled, summary);
}

bool NativeComponentProvisioner::ensure(QString* reason) {
    auto refuse = [&](const char* key, const QString& text) {
        if (reason) *reason = text;
        qInfo().noquote() << "native components:" << key << text;
        return false;
    };

    if (native::storedRuntimeChoice() != native::RuntimeChoice::Native) {
        return refuse("native.components.notNative",
                      translated(QT_TRANSLATE_NOOP("NativeComponentProvisioner", "The selected runtime is not Native.")));
    }

    QString bundleRoot;
    std::optional<native::Bundle> bundle;
    const native::RuntimeSelection& selection = native::currentSelection();
    if (selection.loaded && selection.kind == native::BundleKind::Native && !selection.root.isEmpty()) {
        bundleRoot = selection.root;
        bundle = selection.resolvedBundle;
    }
    if (bundleRoot.isEmpty()) {
        for (const native::Bundle& candidate : native::discoverBundles()) {
            if (candidate.kind == native::BundleKind::Native) {
                bundleRoot = candidate.root;
                bundle = candidate;
                break;
            }
        }
    }
    if (!bundle.has_value() || !bundle->preparation.has_value()) {
        return refuse("native.components.noBundle",
                      translated(QT_TRANSLATE_NOOP("NativeComponentProvisioner", "No native bundle with preparation provenance was found.")));
    }

    const native::BundlePreparation& preparation = *bundle->preparation;
    const native::GpuComputeCapability& gpu = native::observeComputeCapability();
    if (gpu.smMajor <= 0) {
        return refuse("native.components.noGpu",
                      translated(QT_TRANSLATE_NOOP("NativeComponentProvisioner", "No NVIDIA device was observed: %1")).arg(gpu.reason));
    }

    const QString trtDirName = preparation.runtime.trt;
    const QStringList declaredTrtFiles = declaredFilesForDir(*bundle, trtDirName);

    const native::ComponentStore store = native::componentStore();
    const native::ComponentBinDirs resolved =
        native::componentBinDirs(bundleRoot, trtDirName, preparation.runtime.cuda, declaredTrtFiles, gpu,
                         store);
    if (resolved.reasonKey.isEmpty()) {
        return refuse("native.components.satisfied",
                      translated(QT_TRANSLATE_NOOP("NativeComponentProvisioner", "The components for %1 are already available.")).arg(native::computeCapabilityText(gpu)));
    }

    if (running_ || hasActiveRun()) {
        return refuse("native.components.busy", translated(QT_TRANSLATE_NOOP("NativeComponentProvisioner", "A component download is already running.")));
    }

    const native::ComponentPack* runtimePack = native::tensorRtRuntimePack();
    const native::ComponentPack* resourcePack = native::tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
    if (runtimePack == nullptr || resourcePack == nullptr) {
        return refuse("native.components.missing",
                      translated(QT_TRANSLATE_NOOP("NativeComponentProvisioner", "The release publishes no component pack for this GPU.")));
    }

    QVector<native::ComponentPack> packs;
    packs.append(*runtimePack);
    if (resourcePack->name != runtimePack->name) {
        packs.append(*resourcePack);
    }
    return startProvisioning(store, packs);
}

bool NativeComponentProvisioner::ensureStore(QString* reason) {
    auto refuse = [&](const char* key, const QString& text) {
        if (reason) *reason = text;
        qInfo().noquote() << "native components:" << key << text;
        return false;
    };
    if (running_ || hasActiveRun()) {
        return refuse("native.components.busy",
                      translated(QT_TRANSLATE_NOOP("NativeComponentProvisioner", "A component download is already running.")));
    }
    const native::GpuComputeCapability& gpu = native::observeComputeCapability();
    if (gpu.smMajor <= 0) {
        return refuse("native.components.noGpu",
                      translated(QT_TRANSLATE_NOOP("NativeComponentProvisioner", "No NVIDIA device was observed: %1")).arg(gpu.reason));
    }
    const native::ComponentPack* runtimePack = native::tensorRtRuntimePack();
    const native::ComponentPack* resourcePack = native::tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
    if (runtimePack == nullptr || resourcePack == nullptr) {
        return refuse("native.components.missing",
                      translated(QT_TRANSLATE_NOOP("NativeComponentProvisioner", "The release publishes no component pack for this GPU.")));
    }
    const native::ComponentStore store = native::componentStore();
    const bool haveRuntime = native::storedPack(store, *runtimePack).installed;
    const bool haveResource = native::storedPack(store, *resourcePack).installed;
    if (haveRuntime && haveResource) {
        return refuse("native.components.satisfied",
                      translated(QT_TRANSLATE_NOOP("NativeComponentProvisioner", "The component store already holds the packs for %1."))
                          .arg(native::computeCapabilityText(gpu)));
    }
    QVector<native::ComponentPack> packs;
    if (!haveRuntime) packs.append(*runtimePack);
    if (!haveResource && resourcePack->name != runtimePack->name) packs.append(*resourcePack);
    return startProvisioning(store, packs);
}

bool NativeComponentProvisioner::needsDownload(QString* reason) const {
    // Cheap and side-effect free: the same reads `ensure()` walks, minus the
    // submit. `Native` unset means the Standard runtime never consults the
    // native providers, so there is nothing to warn about; a satisfied toolchain
    // (bundle or store) likewise needs no prompt.
    if (native::storedRuntimeChoice() != native::RuntimeChoice::Native) {
        return false;
    }

    QString bundleRoot;
    QString trtDirName;
    QString cudaDirName;
    QStringList declaredTrtFiles;
    const native::RuntimeSelection& selection = native::currentSelection();
    if (selection.loaded && selection.kind == native::BundleKind::Native && !selection.root.isEmpty() &&
        selection.resolvedBundle.has_value() &&
        selection.resolvedBundle->preparation.has_value()) {
        bundleRoot = selection.root;
        trtDirName = selection.resolvedBundle->preparation->runtime.trt;
        cudaDirName = selection.resolvedBundle->preparation->runtime.cuda;
        declaredTrtFiles = declaredFilesForDir(*selection.resolvedBundle, trtDirName);
    } else {
        for (const native::Bundle& candidate : native::discoverBundles()) {
            if (candidate.kind != native::BundleKind::Native || !candidate.preparation.has_value()) {
                continue;
            }
            bundleRoot = candidate.root;
            trtDirName = candidate.preparation->runtime.trt;
            cudaDirName = candidate.preparation->runtime.cuda;
            declaredTrtFiles = declaredFilesForDir(candidate, trtDirName);
            break;
        }
    }
    if (bundleRoot.isEmpty()) {
        return false;
    }

    const native::GpuComputeCapability& gpu = native::observeComputeCapability();
    if (gpu.smMajor <= 0) {
        if (reason) *reason = gpu.reason;
        return false;
    }

    const native::ComponentBinDirs resolved = native::componentBinDirs(bundleRoot, trtDirName, cudaDirName,
                                                       declaredTrtFiles, gpu, native::componentStore());
    if (!resolved.reasonKey.isEmpty()) {
        if (reason) *reason = resolved.reason;
        return true;
    }
    return false;
}

bool NativeComponentProvisioner::startProvisioning(const native::ComponentStore& store,
                                                   const QVector<native::ComponentPack>& packs) {
    // Claim the shared delivery slot atomically under the mutex and mint a
    // unique run ID so stale callbacks from any earlier run cannot settle a
    // newer run.
    quint64 runId = 0;
    {
        const QMutexLocker lock(&currentRunSlotMutex());
        if (!activeRun().owner.isNull() && activeRun().id != 0) {
            return false;
        }
        runId = ++nextRunId();
        activeRun() = {this, runId};
    }
    activeRunId_ = runId;
    running_ = true;
    cancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    const QString scratchRoot = native::nativeSubdirectory(QStringLiteral("scratch"));
    const std::shared_ptr<std::atomic<bool>> flag = cancelFlag_;

    // One GUI dispatch binder, built on the GUI thread before the worker starts.
    // It is the only route from the worker back to an owner, so no widget
    // pointer and no raw `this` is ever captured across the thread boundary.
    const Runner::GuiDispatch post = runner_.post();
    // The worker captures no owner pointer at all: the binder is its only route
    // back, and every body it posts resolves the owner on the GUI thread.
    runner_.runTransfer(
        [store, packs, scratchRoot, flag, post, runId]() -> QVariant {
            QDir().mkpath(scratchRoot);
            QTemporaryDir scratchDir(
                QDir(scratchRoot).filePath(QStringLiteral("components-XXXXXX")));
            if (!scratchDir.isValid()) {
                throw std::runtime_error(
                    "native.components.install: the scratch directory is unavailable");
            }
            const native::ComponentProvisionResult outcome = native::provisionTensorRtComponents(
                store, packs, scratchDir.path(),
                [post, runId](const native::ComponentProvisionProgress& update) {
                    // A binder call never touches a widget and never reads the
                    // owner: it hands this body to the GUI thread, where the
                    // owner is resolved and the delivery either lands or is
                    // dropped.
                    post([update, runId]() {
                        NativeComponentProvisioner* target = currentRun(runId);
                        if (target == nullptr) return;
                        emit target->progress(update);
                    });
                },
                [flag] { return flag->load(); });
            if (outcome.cancelled) {
                throw std::runtime_error(QStringLiteral("native.components.cancelled: %1")
                                             .arg(outcome.reason)
                                             .toStdString());
            }
            if (!outcome.ok) {
                throw std::runtime_error(
                    QStringLiteral("%1: %2").arg(outcome.reasonKey, outcome.reason).toStdString());
            }
            return QVariant{};
        },
        [runId](const QVariant&) {
            NativeComponentProvisioner* target = currentRun(runId);
            if (target == nullptr) return;
            target->finish(runId, true, false,
                           translated(QT_TRANSLATE_NOOP("NativeComponentProvisioner",
                                                        "Components installed.")));
        },
        [runId](const backend::Failure& failure) {
            // The downloader tags a cancelled run in its own message, so this
            // stays a message check rather than a code one.
            const bool cancelled =
                failure.message.startsWith(QStringLiteral("native.components.cancelled"));
            NativeComponentProvisioner* target = currentRun(runId);
            if (target == nullptr) return;
            target->finish(runId, false, cancelled, failure.message);
        });
    return true;
}

bool NativeComponentProvisioner::running() const { return running_; }

void NativeComponentProvisioner::cancel() {
    if (cancelFlag_) {
        cancelFlag_->store(true);
    }
    // Cancel stops the transfer; the run still settles itself through its own
    // completion, which is what frees the shared run slot and tells the owner
    // the download is really over.
}

NativeComponentProvisioner::~NativeComponentProvisioner() {
    // An owner going away cancels its own download: the worker's gate is set
    // here and the Runner's cancellation scope pulls it out of blocking backend
    // waits. The Runner itself is borrowed, never owned - cancelling it here
    // would retire the shared pools' queues for every other owner, so this
    // destructor only sets the run's own gate.
    //
    // The cancel is gated on running_ under the slot lock: a run that already
    // settled must not have its flag set after the fact. The slot is cleared
    // only if it still points at this provisioner and run ID, so destroying one
    // owner never yanks the slot from a second run that re-claimed it.
    {
        const QMutexLocker lock(&currentRunSlotMutex());
        if (running_) {
            cancel();
        }
        if (activeRun().owner == this && activeRun().id == activeRunId_) {
            activeRun() = {};
        }
    }
}

}  // namespace mikai::app
