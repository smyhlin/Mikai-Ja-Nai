#pragma once

#include <QObject>
#include <QString>

#include <QVector>

#include <atomic>
#include <memory>

#include "native/components/NativeComponentProvisioning.hpp"

namespace mplayer::native {
struct ComponentPack;
struct ComponentStore;
}  // namespace mplayer::native

namespace mikai::app {

class Runner;

/// Provisions the TensorRT components a GPU needs when the installed bundle
/// does not carry the builder resource that machine requires. One run per
/// process; the check is cheap and satisfied on a machine whose bundle already
/// fits, which is the common case.
class NativeComponentProvisioner : public QObject {
    Q_OBJECT

public:
    explicit NativeComponentProvisioner(Runner& runner, QObject* parent = nullptr);
    /// An owner that goes away cancels its own download. The worker's gate is
    /// set here, the Runner's gate then pulls the worker out of its blocking
    /// backend waits, and every queued completion is suppressed before an owner
    /// that no longer exists is touched. Nothing here waits for the worker.
    ~NativeComponentProvisioner() override;

    bool ensure(QString* reason);

    /// Downloads the TensorRT runtime and the builder resource this GPU needs
    /// into the app-owned component store, regardless of whether the installed
    /// bundle already carries a fitting resource. This is the components tab's
    /// autofix: it refuses when there is no NVIDIA device or the store already
    /// holds both packs, and never touches the bundle.
    bool ensureStore(QString* reason);

    /// Side-effect-free missing-component check: true when the Native runtime is
    /// selected, the machine has a usable NVIDIA device, and either the runtime
    /// or the builder resource for this GPU is absent from the component store.
    /// Never starts a download; the startup prompt uses this before asking.
    bool needsDownload(QString* reason = nullptr) const;
    bool running() const;
    void cancel();

signals:
    void progress(const mplayer::native::ComponentProvisionProgress& progress);
    void finished(bool ok, bool cancelled, const QString& summary);

private:
    /// True while any provisioner instance in this process has an active run in
    /// flight. Checked under the shared run-slot mutex so a second provisioner
    /// (Settings dialog vs. startup prompt) cannot hijack an in-flight run.
    static bool hasActiveRun();
    /// The one provisioner the run `runId` delivers for, or null once its owner
    /// has gone away or `runId` is no longer the active run. GUI-thread only:
    /// it is read from the bodies the Runner's dispatcher delivered, which is
    /// the one place the owner is known to be readable.
    static NativeComponentProvisioner* currentRun(quint64 runId);
    /// Frees the shared run slot when a run settles — only if the slot still
    /// points at the provisioner and run ID that settled, so a second run that
    /// re-claimed the slot is not yanked and left with running_ stuck true.
    static void runFinished(NativeComponentProvisioner* finished, quint64 runId);
    /// Applies one settled run. GUI-thread only, for the same reason.
    void finish(quint64 runId, bool ok, bool cancelled, const QString& summary);

    bool startProvisioning(const mplayer::native::ComponentStore& store,
                           const QVector<mplayer::native::ComponentPack>& packs);

    Runner& runner_;
    bool running_ = false;
    quint64 activeRunId_ = 0;
    std::shared_ptr<std::atomic<bool>> cancelFlag_;
};

}  // namespace mikai::app
