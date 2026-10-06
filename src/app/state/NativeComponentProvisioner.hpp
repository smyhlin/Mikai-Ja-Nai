#pragma once

#include <QObject>
#include <QString>

#include <QVector>

#include <atomic>
#include <memory>

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
    void progress(const QString& pack, qint64 received, qint64 total);
    void finished(bool ok, bool cancelled, const QString& summary);

private:
    bool startProvisioning(const mplayer::native::ComponentStore& store,
                           const QVector<mplayer::native::ComponentPack>& packs);

    Runner& runner_;
    bool running_ = false;
    bool cancelled_ = false;
    std::shared_ptr<std::atomic<bool>> cancelFlag_;
};

}  // namespace mikai::app
