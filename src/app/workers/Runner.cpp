#include "workers/Runner.hpp"

#include "I18n.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QThread>
#include <QThreadPool>
#include <backend/Cancellation.hpp>
#include <backend/Errors.hpp>
#include <backend/Streams.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace mikai::app {

/// Lives on the GUI thread. Every completion is posted here, so a callback
/// always runs where the widgets it touches live.
class Runner::Dispatcher : public QObject {
    Q_OBJECT

public:
    explicit Dispatcher(QObject* parent) : QObject(parent) {}
};

namespace {

// The pools cap the kinds of work. Artwork gets a wide pool on purpose:
// poster downloads spend no API budget and only wait on the network, so sixty
// of them at four at a time is what made the home page look unfinished.
constexpr int kSharedThreads = 4;
constexpr int kBulkThreads = 3;
constexpr int kImageThreads = 12;
// Stream extraction is latency-critical and single-user: an episode click has
// to answer now, not after the feed fan-out sharing the bulk pool. Two threads
// is all one user clicking one episode at a time can spend.
constexpr int kStreamThreads = 2;
// Model transfers are independent of each other and of the shared pools, so
// several Components-tab downloads can run at once instead of queuing behind
// each other. Conflicting installs cannot happen anyway: per-component
// acquisition is single-flight in the UI/provisioner layer, and the native
// runtime and RIFE archive installers keep their own mutex guards. Sharing this
// pool with anything else is not allowed - it must not borrow a shared-pool
// thread that `getAnime` / `getPlayer` need while a pack downloads.
constexpr int kTransferThreads = 4;

/// The result of one task and the handlers waiting for it. Held by shared_ptr
/// so a completion that is still queued keeps the callbacks alive.
struct Delivery {
    Runner::OnOk onOk;
    Runner::OnErr onErr;
};

/// Everything a Runner, its pool tasks and its queued completions share.
///
/// It is shared rather than owned by the Runner because destroying a Runner must
/// not wait: ~QThreadPool blocks until the pool's work is done, and a quota wait
/// has no bound an owner can shorten. So the Runner drops its reference and the
/// last one retires the pools from a queued GUI call, never from a pool worker
/// (which would wait for a thread that can never finish).
struct Shared {
    /// The pools are deliberately unparented: a child of the Runner is destroyed
    /// by QObject's own destructor, which is exactly the freeze this shared state
    /// exists to avoid. It is a QObject, so retiring one must still happen on
    /// the thread it was created on - the GUI thread.
    std::shared_ptr<Runner::Dispatcher> dispatcher;
    std::vector<std::unique_ptr<QThreadPool>> pools;

    /// Set by cancel(), by destruction and by shutdownAll(). Every worker's
    /// blocking backend waits read it through WorkCancellationScope, and every
    /// queued completion re-checks it before touching an owner that is gone.
    std::atomic_bool cancelled{false};

    /// Pool tasks still outstanding, under `mutex`.
    int pending = 0;
    /// Tasks whose own reference to this state has not been handed back yet,
    /// under `mutex`. A task keeps this count until its callable and delivery
    /// captures are destroyed, because those destructors can block on the
    /// worker: the slot being back is not proof the runnable is retired.
    int liveTasks = 0;
    /// True once the Runner released its reference, or shutdownAll drained it.
    /// A task that finishes afterwards must not retire the pools from its own
    /// pool thread.
    bool abandoned = false;
    mutable std::mutex mutex;
    std::condition_variable idle;

    /// The gate, relaxed: every read is followed by a check the owner's own
    /// GUI-only guard repeats on the GUI thread, so ordering is not load-bearing.
    bool suppressed() const { return cancelled.load(std::memory_order_relaxed); }
};

std::shared_ptr<Shared> makeShared() {
    auto shared = std::make_shared<Shared>();
    shared->dispatcher = std::make_shared<Runner::Dispatcher>(nullptr);
    const auto add = [&shared](int threads) {
        auto pool = std::make_unique<QThreadPool>(nullptr);
        pool->setMaxThreadCount(threads);
        shared->pools.push_back(std::move(pool));
    };
    add(kSharedThreads);
    add(kBulkThreads);
    add(kStreamThreads);
    add(kImageThreads);
    add(kTransferThreads);
    return shared;
}

void noteFinished(const std::shared_ptr<Shared>& shared) {
    const std::lock_guard<std::mutex> guard(shared->mutex);
    if (--shared->pending == 0) {
        shared->idle.notify_all();
    }
}

/// Hands `keeper` to the GUI event queue to be released. Retiring a pool is a
/// QObject destruction, so it has to happen on the GUI thread the pools have
/// affinity to; posting it is what keeps destruction and shutdown from waiting
/// for work that is still unwinding.
void releaseOnGui(std::shared_ptr<Shared> keeper) {
    if (!keeper) {
        return;
    }
    Runner::Dispatcher* dispatcher = keeper->dispatcher.get();
    QMetaObject::invokeMethod(dispatcher,
                              [keeper = std::move(keeper)]() mutable { keeper.reset(); },
                              Qt::QueuedConnection);
}

/// Drops `reference` without letting this thread be the last owner of the
/// state. Under the lock a live owner or another live task still holds one;
/// otherwise only the GUI queue may retire the pools.
void dropReferenceOffWorker(std::shared_ptr<Shared>& reference) {
    {
        const std::lock_guard<std::mutex> guard(reference->mutex);
        if (!(reference->abandoned && reference->liveTasks == 0)) {
            reference.reset();
            return;
        }
    }
    releaseOnGui(std::move(reference));
}

/// Posts `callback` to the GUI thread. If the application object is already
/// gone the callback is DROPPED (and logged), never run on the calling
/// thread: a finished() callback after quit must never execute off the GUI
/// thread, where it could touch widgets that no longer exist.
void postToGui(std::function<void()> callback) {
    if (!callback) {
        return;
    }
    QObject* appRoot = QCoreApplication::instance();
    if (appRoot == nullptr) {
        qWarning("runner: dropping GUI callback after QCoreApplication is gone");
        return;
    }
    QMetaObject::invokeMethod(appRoot, std::move(callback), Qt::QueuedConnection);
}

/// Intentionally leaks one timed-out pool's shared state (tasks/11 Y-17).
/// Destroying it would run ~QThreadPool() — waitForDone(-1) — on whichever
/// thread drops the last reference, and the GUI thread must never be that
/// thread while a worker is still parked. The leaked state outlives the
/// process instead of freezing shutdown.
void leakTimedOutPool(std::shared_ptr<Shared> shared) {
    auto* leaked = new std::shared_ptr<Shared>(std::move(shared));
    (void)leaked;
}

std::mutex& runnerRegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::weak_ptr<Shared>>& runnerRegistry() {
    static std::vector<std::weak_ptr<Shared>> registry;
    return registry;
}

/// Cancels and empties one Runner's pools, from whatever thread asks.
void abandon(std::shared_ptr<Shared> shared) {
    if (!shared) {
        return;
    }
    shared->cancelled.store(true, std::memory_order_relaxed);
    // Drops the tasks that have not started. A task already popped by a worker
    // runs to its next cooperative check - which its WorkCancellationScope
    // bounds - and its completion is suppressed by the same gate.
    for (const auto& pool : shared->pools) {
        pool->clear();
    }
}

class Task : public QRunnable {
public:
    Task(std::shared_ptr<Shared> shared, Runner::Work work,
         std::shared_ptr<Delivery> delivery)
        : shared_(std::move(shared)), work_(std::move(work)),
          delivery_(std::move(delivery)) {
        setAutoDelete(true);
    }

    ~Task() override {
        // A task the pool cleared before it could run never posts a completion,
        // but it still has to give its slot back. A task whose run() already
        // gave its slot back must not give it back twice.
        if (!settled_) {
            noteFinished(shared_);
        }
        // Captures go before the shared reference: a callable's destructor can
        // block, and until it returns the owner must still see a live task
        // rather than one it may retire the pools under.
        work_ = Runner::Work();
        delivery_.reset();
        releaseShared();
    }

    void run() override {
        settled_ = true;
        QVariant result;
        bool ok = true;
        backend::Failure failure;
        try {
            if (work_) {
                // The gate is installed for this worker only. Every blocking
                // backend wait it performs - rate-limit parking, chunked
                // download polling - reads it through the thread-local chain
                // and unwinds within a slice instead of parking for a whole
                // quota window.
                const backend::WorkCancellationScope scope(&shared_->cancelled);
                result = work_();
            }
        } catch (const backend::MikaiApiError& error) {
            ok = false;
            qWarning().noquote() << "Mikai API error:" << error.code << error.message;
            failure.code = error.code;
            failure.status = error.status;
            failure.message = localizedApiError(error.code);
            if (failure.message.isEmpty()) {
                failure.message =
                    error.message.isEmpty() ? error.code : error.message;
            }
        } catch (const backend::NoSourceError& error) {
            // Tagged in the message rather than in `code`, because the title
            // page and the quality fetch both branch on that prefix and the tag
            // has to survive wherever the message goes.
            ok = false;
            failure.message = QStringLiteral("no_source: %1").arg(QString::fromUtf8(error.what()));
        } catch (const std::exception& error) {
            ok = false;
            failure.message = QString::fromUtf8(error.what());
        } catch (...) {
            ok = false;
            failure.message = QCoreApplication::translate("Runner", "Unknown error");
        }
        // A queued call on the dispatcher: the pool thread hands the result
        // over and returns immediately, and the body runs on the GUI thread.
        // The gate is re-read there rather than once here, because an owner the
        // work was submitted for may have been destroyed while this was queued.
        QMetaObject::invokeMethod(
            shared_->dispatcher.get(),
            [shared = shared_, delivery = delivery_, result, ok, failure]() {
                if (shared->suppressed()) {
                    return;
                }
                if (ok) {
                    if (delivery->onOk) {
                        delivery->onOk(result);
                    }
                } else if (delivery->onErr) {
                    delivery->onErr(failure);
                }
            },
            Qt::QueuedConnection);
        noteFinished(shared_);
    }

private:
    /// Hands this task's reference back, after its captures are gone. The owner
    /// may already have released its own; then this worker must not be the one
    /// that destroys the pools, because ~QThreadPool waits for its own thread.
    void releaseShared() {
        {
            const std::lock_guard<std::mutex> guard(shared_->mutex);
            if (--shared_->liveTasks == 0) {
                shared_->idle.notify_all();
            }
            // Reset under this lock: `abandoned` false means the owner keeps a
            // reference, and `liveTasks` > 0 means another task does. Anything
            // else could otherwise destroy the pools on this worker.
            if (!(shared_->abandoned && shared_->liveTasks == 0)) {
                shared_.reset();
                return;
            }
        }
        releaseOnGui(std::move(shared_));
    }

    std::shared_ptr<Shared> shared_;
    Runner::Work work_;
    std::shared_ptr<Delivery> delivery_;
    /// True once this task's slot has been counted back: set at the top of
    /// run(), and read in the destructor for a task the pool cleared instead.
    bool settled_ = false;
};

}  // namespace

struct Runner::Impl {
    explicit Impl() : shared(makeShared()) {}

    std::shared_ptr<Shared> shared;
};

Runner::Runner(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>()) {
    const std::lock_guard<std::mutex> guard(runnerRegistryMutex());
    runnerRegistry().push_back(impl_->shared);
}

Runner::~Runner() {
    {
        const std::lock_guard<std::mutex> guard(runnerRegistryMutex());
        auto& registry = runnerRegistry();
        std::erase_if(registry, [this](const std::weak_ptr<Shared>& entry) {
            return entry.expired() || entry.lock() == impl_->shared;
        });
    }
    // Unwinds running waits and suppresses a completion queued behind this
    // destructor.
    abandon(impl_->shared);
    bool outstanding = false;
    {
        // Same lock as the check: an empty `pending` is not proof that no
        // runnable is still retiring on a pool thread.
        const std::lock_guard<std::mutex> guard(impl_->shared->mutex);
        impl_->shared->abandoned = true;
        outstanding = impl_->shared->pending > 0 || impl_->shared->liveTasks > 0;
    }
    if (outstanding) {
        // Retire from the queue instead of waiting here. impl_ keeps its own
        // reference until that handoff is posted.
        releaseOnGui(impl_->shared);
    }
    // Every task still unwinding holds a reference, so the state outlives this.
    impl_.reset();
}

Runner::GuiDispatch Runner::post() {
    const std::weak_ptr<Shared> anchor = impl_->shared;
    return [anchor](OnGui callback) {
        if (!callback) {
            return;
        }
        std::shared_ptr<Shared> shared = anchor.lock();
        if (!shared) {
            return;
        }
        if (shared->suppressed()) {
            dropReferenceOffWorker(shared);
            return;
        }
        // The promoted reference moves into the queued body, so no strong copy
        // survives this call on the worker and the GUI thread releases it. The
        // target is read first: argument evaluation order is unspecified, so
        // the move below may empty `shared` before the dispatcher is fetched.
        Runner::Dispatcher* const dispatcher = shared->dispatcher.get();
        QMetaObject::invokeMethod(
            dispatcher,
            [shared = std::move(shared), callback = std::move(callback)]() mutable {
                // Re-read on the GUI thread: an owner the body closes between
                // this post and its delivery is a no-op, not a dangling
                // dereference.
                if (shared->suppressed()) {
                    return;
                }
                callback();
            },
            Qt::QueuedConnection);
    };
}

std::function<void()> Runner::post(OnGui callback) {
    const GuiDispatch dispatch = post();
    return [dispatch = std::move(dispatch), callback = std::move(callback)]() mutable {
        dispatch(std::move(callback));
    };
}

void Runner::cancel() {
    if (impl_ == nullptr) {
        return;
    }
    abandon(impl_->shared);
}

void Runner::shutdownAll(std::function<void()> finished) {
    std::vector<std::shared_ptr<Shared>> live;
    {
        const std::lock_guard<std::mutex> guard(runnerRegistryMutex());
        auto& registry = runnerRegistry();
        std::erase_if(registry, [](const std::weak_ptr<Shared>& entry) {
            return entry.expired();
        });
        for (const std::weak_ptr<Shared>& entry : registry) {
            const std::shared_ptr<Shared> shared = entry.lock();
            if (!shared) {
                continue;
            }
            abandon(shared);
            live.push_back(std::move(shared));
        }
    }
    if (live.empty()) {
        postToGui(std::move(finished));
        return;
    }
    // The drain runs on a QThread, never joined from the GUI thread and
    // owned until it finishes (deleteLater on `finished`): the drain waits
    // with a BOUNDED timeout per pool, so shutdown can never park behind a
    // worker that never unwinds. The 25 pool threads are not retuned here
    // (tasks/05 P0-05 non-goal).
    QThread* drain = QThread::create(
        [live = std::move(live), finished = std::move(finished)]() mutable {
            std::vector<std::shared_ptr<Shared>> quiescent;
            for (const std::shared_ptr<Shared>& shared : live) {
                bool drained = false;
                {
                    std::unique_lock<std::mutex> guard(shared->mutex);
                    shared->abandoned = true;
                    drained = shared->idle.wait_for(guard, std::chrono::seconds(5), [&shared] {
                        return shared->pending == 0 && shared->liveTasks == 0;
                    });
                }
                if (drained) {
                    quiescent.push_back(shared);
                    continue;
                }
                int pending = 0;
                int liveTasks = 0;
                {
                    const std::lock_guard<std::mutex> guard(shared->mutex);
                    pending = shared->pending;
                    liveTasks = shared->liveTasks;
                }
                qWarning("runner drain timed out; %d pending, %d live", pending, liveTasks);
                // A timed-out pool is leaked on purpose (tasks/11 Y-17):
                // handing it to the GUI thread's clear() below would destroy
                // it there, and ~QThreadPool() calls waitForDone(-1) — freezing
                // shutdown on the GUI thread behind the worker that never
                // unwound. Only quiescent pools are retired on the GUI thread.
                leakTimedOutPool(shared);
            }
            // Retiring a pool is a QObject destruction, so it waits for the GUI
            // thread - as do the Runners' own pending releases.
            postToGui([quiescent = std::move(quiescent),
                       finished = std::move(finished)]() mutable {
                quiescent.clear();
                if (finished) {
                    finished();
                }
            });
        });
    QObject::connect(drain, &QThread::finished, drain, &QObject::deleteLater);
    drain->start();
}

void Runner::start(QThreadPool* pool, Work work, OnOk onOk, OnErr onErr) {
    const std::shared_ptr<Shared> shared = impl_->shared;
    if (shared->suppressed()) {
        // The Runner has been cancelled or is going away: nothing new is
        // accepted, and no task here means no deferred pool retirement.
        return;
    }
    auto delivery = std::make_shared<Delivery>();
    delivery->onOk = std::move(onOk);
    delivery->onErr = std::move(onErr);
    {
        const std::lock_guard<std::mutex> guard(shared->mutex);
        ++shared->pending;
        ++shared->liveTasks;
    }
    pool->start(new Task(shared, std::move(work), std::move(delivery)));
}

void Runner::run(Work work, OnOk onOk, OnErr onErr) {
    start(impl_->shared->pools[0].get(), std::move(work), std::move(onOk), std::move(onErr));
}

void Runner::runBulk(Work work, OnOk onOk, OnErr onErr) {
    start(impl_->shared->pools[1].get(), std::move(work), std::move(onOk), std::move(onErr));
}

void Runner::runStream(Work work, OnOk onOk, OnErr onErr) {
    start(impl_->shared->pools[2].get(), std::move(work), std::move(onOk), std::move(onErr));
}

void Runner::runImage(Work work, OnOk onOk, OnErr onErr) {
    start(impl_->shared->pools[3].get(), std::move(work), std::move(onOk), std::move(onErr));
}

void Runner::runTransfer(Work work, OnOk onOk, OnErr onErr) {
    start(impl_->shared->pools[4].get(), std::move(work), std::move(onOk), std::move(onErr));
}

bool Runner::waitForDone(int timeoutMs) {
    QElapsedTimer budget;
    budget.start();
    // The budget stays the caller's: -1 is Qt's own "wait for the last thread
    // to exit", which is what a practical-infinite budget asks for, and 0 is
    // what a caller that only wants to poll gets. This is the explicit blocking
    // drain; destruction never calls it.
    const auto left = [&budget, timeoutMs] {
        if (timeoutMs < 0) {
            return -1;
        }
        return qMax(0, timeoutMs - static_cast<int>(budget.elapsed()));
    };
    const bool bulk = impl_->shared->pools[1]->waitForDone(left());
    const bool stream = impl_->shared->pools[2]->waitForDone(left());
    const bool image = impl_->shared->pools[3]->waitForDone(left());
    const bool transfer = impl_->shared->pools[4]->waitForDone(left());
    return impl_->shared->pools[0]->waitForDone(left()) && bulk && stream && image && transfer;
}

}  // namespace mikai::app

#include "Runner.moc"
