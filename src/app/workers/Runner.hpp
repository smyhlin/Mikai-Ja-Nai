#pragma once

#include <QObject>
#include <QThreadPool>
#include <QVariant>

#include <functional>
#include <memory>

namespace mikai::app {

/// Off-thread work for the UI. Nothing here ever blocks the GUI thread.
///
/// Three pools, because the work has three different shapes:
///  * the shared pool bounds concurrent API calls,
///  * the bulk pool carries feed fan-out (one task per home card), so a title the
///    user just opened is not queued behind the whole feed,
///  * the image pool carries artwork only. `fetchBytes` bypasses the client's
///    rate limiter entirely, so posters cost no API budget and are pure latency.
class Runner : public QObject {
    Q_OBJECT

public:
    /// Work to run on a pool thread. Whatever it returns lands on the GUI thread.
    using Work = std::function<QVariant()>;
    using OnOk = std::function<void(const QVariant&)>;
    using OnErr = std::function<void(const QString&)>;

    explicit Runner(QObject* parent = nullptr);
    ~Runner() override;

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    void run(Work work, OnOk onOk, OnErr onErr);
    void runBulk(Work work, OnOk onOk, OnErr onErr);
    void runImage(Work work, OnOk onOk, OnErr onErr);

    /// Drains the bulk and image pools first, then the shared one, and reports
    /// whether the shared pool finished inside the budget.
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
