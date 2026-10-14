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
#include <QTemporaryDir>
#include <QVariant>

#include <stdexcept>

namespace mikai::app {

using namespace mplayer::native;

namespace {

QString translated(const char* message) {
    return QCoreApplication::translate("NativeComponentProvisioner", message);
}

}  // namespace

NativeComponentProvisioner* NativeComponentProvisioner::currentRun_ = nullptr;

NativeComponentProvisioner::NativeComponentProvisioner(Runner& runner, QObject* parent)
    : QObject(parent), runner_(runner) {
    if (currentRun_ == nullptr) {
        currentRun_ = this;
    }
}

NativeComponentProvisioner* NativeComponentProvisioner::currentRun() {
    // Only ever called from a body the Runner's dispatcher delivered, which is
    // on the GUI thread, in the one moment the owner is known to be readable.
    return currentRun_;
}

void NativeComponentProvisioner::runFinished() {
    // One settled run frees the slot so the same owner can run again. GUI thread
    // only: every delivery arrives through the Runner's dispatcher.
    currentRun_ = nullptr;
}

void NativeComponentProvisioner::finish(bool ok, bool cancelled, const QString& summary) {
    runFinished();
    running_ = false;
    emit finished(ok, cancelled, summary);
}

bool NativeComponentProvisioner::ensure(QString* reason) {
    auto refuse = [&](const char* key, const QString& text) {
        if (reason) *reason = text;
        qInfo().noquote() << "native components:" << key << text;
        return false;
    };

    if (storedRuntimeChoice() != RuntimeChoice::Native) {
        return refuse("native.components.notNative",
                      translated("The selected runtime is not Native."));
    }

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
        return refuse("native.components.noBundle",
                      translated("No native bundle with preparation provenance was found."));
    }

    const BundlePreparation& preparation = *bundle->preparation;
    const GpuComputeCapability& gpu = observeComputeCapability();
    if (gpu.smMajor <= 0) {
        return refuse("native.components.noGpu",
                      translated("No NVIDIA device was observed: %1").arg(gpu.reason));
    }

    const QString trtDirName = preparation.runtime.trt;
    QStringList declaredTrtFiles;
    const QString prefix = trtDirName + QLatin1Char('/');
    for (const BundleFile& file : bundle->files) {
        if (file.path.startsWith(prefix)) {
            declaredTrtFiles.append(file.path);
        }
    }

    const ComponentStore store = componentStore();
    const ComponentBinDirs resolved =
        componentBinDirs(bundleRoot, trtDirName, preparation.runtime.cuda, declaredTrtFiles, gpu,
                         store);
    if (resolved.reasonKey.isEmpty()) {
        return refuse("native.components.satisfied",
                      translated("The components for %1 are already available.").arg(computeCapabilityText(gpu)));
    }

    if (running_) {
        return refuse("native.components.busy", translated("A component download is already running."));
    }

    const ComponentPack* runtimePack = tensorRtRuntimePack();
    const ComponentPack* resourcePack = tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
    if (runtimePack == nullptr || resourcePack == nullptr) {
        return refuse("native.components.missing",
                      translated("The release publishes no component pack for this GPU."));
    }

    QVector<ComponentPack> packs;
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
    if (running_) {
        return refuse("native.components.busy",
                      translated("A component download is already running."));
    }
    const GpuComputeCapability& gpu = observeComputeCapability();
    if (gpu.smMajor <= 0) {
        return refuse("native.components.noGpu",
                      translated("No NVIDIA device was observed: %1").arg(gpu.reason));
    }
    const ComponentPack* runtimePack = tensorRtRuntimePack();
    const ComponentPack* resourcePack = tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
    if (runtimePack == nullptr || resourcePack == nullptr) {
        return refuse("native.components.missing",
                      translated("The release publishes no component pack for this GPU."));
    }
    const ComponentStore store = componentStore();
    const bool haveRuntime = storedPack(store, *runtimePack).installed;
    const bool haveResource = storedPack(store, *resourcePack).installed;
    if (haveRuntime && haveResource) {
        return refuse("native.components.satisfied",
                      translated("The component store already holds the packs for %1.")
                          .arg(computeCapabilityText(gpu)));
    }
    QVector<ComponentPack> packs;
    if (!haveRuntime) packs.append(*runtimePack);
    if (!haveResource && resourcePack->name != runtimePack->name) packs.append(*resourcePack);
    return startProvisioning(store, packs);
}

bool NativeComponentProvisioner::needsDownload(QString* reason) const {
    // Cheap and side-effect free: the same reads `ensure()` walks, minus the
    // submit. `Native` unset means the Standard runtime never consults the
    // native providers, so there is nothing to warn about; a satisfied toolchain
    // (bundle or store) likewise needs no prompt.
    if (storedRuntimeChoice() != RuntimeChoice::Native) {
        return false;
    }

    QString bundleRoot;
    QString trtDirName;
    QString cudaDirName;
    QStringList declaredTrtFiles;
    const RuntimeSelection& selection = currentSelection();
    if (selection.loaded && selection.kind == BundleKind::Native && !selection.root.isEmpty() &&
        selection.resolvedBundle.has_value() &&
        selection.resolvedBundle->preparation.has_value()) {
        bundleRoot = selection.root;
        trtDirName = selection.resolvedBundle->preparation->runtime.trt;
        cudaDirName = selection.resolvedBundle->preparation->runtime.cuda;
        const QString prefix = trtDirName + QLatin1Char('/');
        for (const BundleFile& file : selection.resolvedBundle->files) {
            if (file.path.startsWith(prefix)) {
                declaredTrtFiles.append(file.path);
            }
        }
    } else {
        for (const Bundle& candidate : discoverBundles()) {
            if (candidate.kind != BundleKind::Native || !candidate.preparation.has_value()) {
                continue;
            }
            bundleRoot = candidate.root;
            trtDirName = candidate.preparation->runtime.trt;
            cudaDirName = candidate.preparation->runtime.cuda;
            const QString prefix = trtDirName + QLatin1Char('/');
            for (const BundleFile& file : candidate.files) {
                if (file.path.startsWith(prefix)) {
                    declaredTrtFiles.append(file.path);
                }
            }
            break;
        }
    }
    if (bundleRoot.isEmpty()) {
        return false;
    }

    const GpuComputeCapability& gpu = observeComputeCapability();
    if (gpu.smMajor <= 0) {
        if (reason) *reason = gpu.reason;
        return false;
    }

    const ComponentBinDirs resolved = componentBinDirs(bundleRoot, trtDirName, cudaDirName,
                                                       declaredTrtFiles, gpu, componentStore());
    if (!resolved.reasonKey.isEmpty()) {
        if (reason) *reason = resolved.reason;
        return true;
    }
    return false;
}

bool NativeComponentProvisioner::startProvisioning(const ComponentStore& store,
                                                   const QVector<ComponentPack>& packs) {
    running_ = true;
    cancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    // Each run re-claims the shared delivery slot: `finish()` clears it when a
    // run settles, so without this re-claim the second run's completion would
    // find no owner and `running_` would stay true forever.
    currentRun_ = this;
    const QString scratchRoot = nativeSubdirectory(QStringLiteral("scratch"));
    const std::shared_ptr<std::atomic<bool>> flag = cancelFlag_;

    // One GUI dispatch binder, built on the GUI thread before the worker starts.
    // It is the only route from the worker back to an owner, so no widget
    // pointer and no raw `this` is ever captured across the thread boundary.
    const Runner::GuiDispatch post = runner_.post();
    // The worker captures no owner pointer at all: the binder is its only route
    // back, and every body it posts resolves the owner on the GUI thread.
    runner_.runTransfer(
        [store, packs, scratchRoot, flag, post]() -> QVariant {
            QDir().mkpath(scratchRoot);
            QTemporaryDir scratchDir(
                QDir(scratchRoot).filePath(QStringLiteral("components-XXXXXX")));
            if (!scratchDir.isValid()) {
                throw std::runtime_error(
                    "native.components.install: the scratch directory is unavailable");
            }
            const ComponentProvisionResult outcome = provisionTensorRtComponents(
                store, packs, scratchDir.path(),
                [post](const ComponentProvisionProgress& update) {
                    // A binder call never touches a widget and never reads the
                    // owner: it hands this body to the GUI thread, where the
                    // owner is resolved and the delivery either lands or is
                    // dropped.
                    post([update]() {
                        NativeComponentProvisioner* target = currentRun();
                        if (target == nullptr) return;
                        emit target->progress(update.pack, update.received, update.total);
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
        [](const QVariant&) {
            NativeComponentProvisioner* target = currentRun();
            if (target == nullptr) return;
            target->finish(true, false, translated("Components installed."));
        },
        [](const backend::Failure& failure) {
            // The downloader tags a cancelled run in its own message, so this
            // stays a message check rather than a code one.
            const bool cancelled =
                failure.message.startsWith(QStringLiteral("native.components.cancelled"));
            NativeComponentProvisioner* target = currentRun();
            if (target == nullptr) return;
            target->finish(false, cancelled, failure.message);
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
    cancel();
    if (currentRun_ == this) currentRun_ = nullptr;
}

}  // namespace mikai::app
