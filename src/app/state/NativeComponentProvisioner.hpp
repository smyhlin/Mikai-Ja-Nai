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
    /// The one provisioner the current run delivers for, or null once its owner
    /// has gone away. GUI-thread only: it is read from the bodies the Runner's
    /// dispatcher delivered, which is the one place the owner is known to be
    /// readable. The slot itself is a QPointer guarded by a QMutex (defined in
    /// the .cpp): a weak, auto-nulling observer — it never keeps the
    /// provisioner alive — and the lock serialises claim, read and clear, so
    /// two provisioners (Settings dialog + startup prompt) cannot interleave
    /// on a raw pointer. Runner::runnerRegistry() was checked first and does
    /// NOT fit: it is a registry of Runner pool states, not a named
    /// provisioner slot, so it is not forced (tasks/05 P1-06).
    static NativeComponentProvisioner* currentRun();
    /// Frees the shared run slot when a run settles — only if the slot still
    /// points at the provisioner that settled, so a second run that re-claimed
    /// the slot is not yanked and left with running_ stuck true. GUI thread
    /// only, for the same reason as `currentRun()`.
    static void runFinished(NativeComponentProvisioner* finished);
    /// Applies one settled run. GUI-thread only, for the same reason.
    void finish(bool ok, bool cancelled, const QString& summary);

    bool startProvisioning(const mplayer::native::ComponentStore& store,
                           const QVector<mplayer::native::ComponentPack>& packs);

    Runner& runner_;
    bool running_ = false;
    std::shared_ptr<std::atomic<bool>> cancelFlag_;
};

}  // namespace mikai::app
