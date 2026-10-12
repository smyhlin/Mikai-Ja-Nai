#pragma once

#include "backend/Errors.hpp"

#include <QObject>
#include <QVariant>

#include <functional>
#include <memory>

class QThreadPool;

namespace mikai::app {

/// Off-thread work for the UI. Nothing here ever blocks the GUI thread.
///
/// Four pools, because the work has four different shapes:
///  * the shared pool bounds concurrent API calls,
///  * the bulk pool carries feed fan-out (one task per home card), so a title the
///    user just opened is not queued behind the whole feed,
///  * the stream pool carries stream extraction only. It is latency-critical and
///    single-user, and it holds its thread across two sequential GETs, so it must
///    not queue behind the feed,
///  * the image pool carries artwork only. `fetchBytes` bypasses the client's
///    rate limiter entirely, so posters cost no API budget and are pure latency.
///
/// Lifetime, which is the whole point of this class:
///
///  * Destroying a Runner never waits. It cancels the work, drops the
///    completions that were still queued, and leaves the dispatcher and the
///    pools to whoever last referenced them. Retiring a pool waits for work
///    that is still unwinding, so the last reference does it on the GUI event
///    queue rather than on the GUI thread directly, and never on one of the
///    pool's own workers, which could not finish waiting for itself.
///  * Queued completions are gated on a flag the owner's destruction sets, so
///    nothing is ever delivered to an owner that is gone. A completion already
///    written into the GUI event queue still runs, but its gate is re-checked
///    there, so it becomes a no-op instead of a dangling receiver.
///  * Work is wrapped in `backend::WorkCancellationScope`, so every blocking
///    backend wait a worker performs - rate-limit parking, chunked download
///    polling - observes the same gate and unwinds promptly instead of parking
///    for a whole quota window.
///  * `shutdownAll()` is the explicit asynchronous drain for application
///    shutdown. It cancels and drains on a background thread so the GUI event
///    loop keeps answering, and reports on the GUI thread once every pool has
///    actually been retired.
class Runner : public QObject {
    Q_OBJECT

public:
    /// Work to run on a pool thread. Whatever it returns lands on the GUI thread.
    using Work = std::function<QVariant()>;
    using OnOk = std::function<void(const QVariant&)>;
    /// The failure callback. It carries the backend's code and HTTP status
    /// alongside the line, so a surface can say *why* something failed instead
    /// of only that it did.
    using OnErr = std::function<void(const backend::Failure&)>;
    /// A GUI-thread callback: it runs where the widgets it touches live.
    using OnGui = std::function<void()>;
    /// The copyable GUI dispatch binder `post()` returns. A worker holds one of
    /// these and calls it from the pool thread; it never touches a widget and
    /// never dereferences the owner.
    using GuiDispatch = std::function<void(OnGui)>;

    explicit Runner(QObject* parent = nullptr);
    ~Runner() override;

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    /// A copyable GUI dispatch closure anchored by a strongly retained
    /// dispatcher plus this Runner's cancellation state. Copying it never
    /// copies a widget pointer: the worker hands GUI bodies to the dispatcher,
    /// which posts them, and the body then supplies its own GUI-thread guard -
    /// an alive flag plus an address it resolves only once it is running on the
    /// GUI thread - before touching a widget.
    ///
    /// Invoking the binder - or invoking the closure `post(OnGui)` returns -
    /// after the Runner is gone is a no-op, never a dangling dereference.
    GuiDispatch post();

    /// A one-shot dispatch closure for `callback`, same guarantees as `post()`.
    std::function<void()> post(OnGui callback);

    /// Cancels this Runner's queued and active work: queued tasks are dropped,
    /// every worker's backend waits unwind, and completions are suppressed.
    /// Never blocks. Queue-only cancellation is the owner's own gate; this is
    /// the whole-Runner gate, used by destruction and shutdown.
    void cancel();

    /// Asynchronously cancels and drains every Runner's pools, keeping the GUI
    /// event loop responsive, and invokes `finished` on the GUI thread once
    /// every pool has been retired. Safe with no Runner alive. Use it before
    /// the application quits, so backend objects a worker still references
    /// outlive the last task.
    static void shutdownAll(std::function<void()> finished);

    void run(Work work, OnOk onOk, OnErr onErr);
    void runBulk(Work work, OnOk onOk, OnErr onErr);
    void runStream(Work work, OnOk onOk, OnErr onErr);
    void runImage(Work work, OnOk onOk, OnErr onErr);

    /// Drains the bulk, stream and image pools first, then the shared one, and
    /// reports whether the shared pool finished inside the budget.
    ///
    /// Blocking by design: this is what an explicit probe asks for.
    /// `-1` waits for the last thread to exit (practically infinite); `0` does
    /// not wait at all. It is not a shutdown bound - destruction never calls
    /// it, and the pool destructors would block anyway.
    bool waitForDone(int timeoutMs);

    /// Thread-affinity anchor for result delivery. Public only so the pool task
    /// in the implementation can name it; not part of the usable surface.
    class Dispatcher;

private:
    void start(QThreadPool* pool, Work work, OnOk onOk, OnErr onErr);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mikai::app
