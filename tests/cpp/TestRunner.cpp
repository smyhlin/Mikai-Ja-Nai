#include "TestRunner.hpp"

#include <app/workers/Runner.hpp>
#include <backend/Cancellation.hpp>
#include <backend/Errors.hpp>
#include <backend/Streams.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDeadlineTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryFile>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QThread>
#include <QtCore/QTimerEvent>
#include <QtCore/QWaitCondition>
#include <QtTest/QTest>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>

using mikai::app::Runner;

namespace backend = mikai::backend;

namespace {

// ── budgets ────────────────────────────────────────────────────────────────
//
// Every wait here is bounded. A predicate that never becomes true has to end in
// a reported failure rather than a hung suite, and a worker parked at a gate has
// to be released by its case rather than by a timeout alone.
constexpr int kSpinMs = 5;
constexpr int kBudgetMs = 20000;
constexpr int kShortBudgetMs = 5000;
// How long a worker will hold its pool thread at a gate if nobody releases it.
// Short enough that a failing original teardown reports quickly, long enough
// that a healthy case never reaches it.
constexpr int kHoldMs = 8000;

// ── bounded spinning ───────────────────────────────────────────────────────
//
// Deliveries arrive as queued GUI events, so waiting means pumping the loop.
// The predicate is evaluated after the pump, and once more on the way out, so a
// state that becomes true between the last pump and the return is still seen.
bool spinUntil(const std::function<bool()>& ready, int timeoutMs = kBudgetMs) {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < timeoutMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, kSpinMs);
    if (ready()) return true;
  }
  return ready();
}

void spinFor(int milliseconds) {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < milliseconds) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, kSpinMs);
    QThread::msleep(1);
  }
}

// ── a gate a worker parks on ───────────────────────────────────────────────
//
// One release lifts every waiter, which is what "release what you armed before
// you return" needs: a case that fails an assertion still frees its pool
// threads on the way out.
class Gate {
public:
  void release() {
    QMutexLocker<QMutex> lock(&mutex_);
    released_ = true;
    armed_.wakeAll();
  }

  // Bounded. Milliseconds, so a case that never releases cannot pin a thread
  // for the lifetime of the process.
  bool waitUntilReleased(int timeoutMs = kHoldMs) {
    QMutexLocker<QMutex> lock(&mutex_);
    while (!released_) {
      if (!armed_.wait(&mutex_, QDeadlineTimer(timeoutMs))) return false;
    }
    return true;
  }

private:
  mutable QMutex mutex_;
  QWaitCondition armed_;
  bool released_ = false;
};

// ── observations, plain and free of private state ───────────────────────────
//
// What actually happened, counted with atomics because the cases read them from
// the GUI thread while workers may still be finishing. Nothing here reaches
// into the runner, counts its calls or reads its source text.
struct Ledger {
  std::atomic_int deliveries{0};
  std::atomic_int failures{0};
  std::atomic_int workEntries{0};
  std::atomic_int workCompletions{0};
  std::atomic_int offGuiDeliveries{0};
  std::atomic_bool deliveredValueWasInt{false};
};

// ── GUI heartbeat ──────────────────────────────────────────────────────────
//
// A timer on the GUI thread is the observable proof that the loop still
// answers: a blocked or saturated GUI thread drops the ticks. This is the
// responsiveness claim of KB-11, watched rather than asserted.
class Heartbeat : public QObject {
public:
  explicit Heartbeat(QObject* parent = nullptr)
      : QObject(parent), timer_(startTimer(std::chrono::milliseconds(10))) {}

  ~Heartbeat() override {
    if (timer_ != 0) killTimer(timer_);
  }

  int ticks() const { return ticks_.load(); }

protected:
  void timerEvent(QTimerEvent* event) override {
    if (event->timerId() == timer_) ++ticks_;
    QObject::timerEvent(event);
  }

private:
  int timer_ = 0;
  std::atomic_int ticks_{0};
};

// ── task bodies and deliveries ─────────────────────────────────────────────
//
// The body records that it began and optionally parks on a gate. The counter
// lives in the caller's Ledger, so "this ran" is an observation rather than a
// wiring check.
Runner::Work countedWork(Ledger* ledger, Gate* hold) {
  return [ledger, hold]() -> QVariant {
    if (ledger != nullptr) ++ledger->workEntries;
    if (hold != nullptr) hold->waitUntilReleased();
    if (ledger != nullptr) ++ledger->workCompletions;
    return QVariant::fromValue(1);
  };
}

Runner::OnOk okDelivery(Ledger* ledger) {
  return [ledger](const QVariant& value) {
    ++ledger->deliveries;
    ledger->deliveredValueWasInt = value.canConvert<int>() && value.toInt() == 1;
    if (QCoreApplication::instance() == nullptr ||
        QThread::currentThread() != QCoreApplication::instance()->thread()) {
      ++ledger->offGuiDeliveries;
    }
  };
}

Runner::OnErr errDelivery(Ledger* ledger) {
  return [ledger](const backend::Failure&) { ++ledger->failures; };
}

// ── the post() contract under test ─────────────────────────────────────────
//
// `post()` returns a copyable binder that holds no widget and no owner, so the
// GUI thread copies it before the work starts and the pool thread hands GUI
// bodies to it. The cases copy the binder exactly the way a widget owner does,
// which is the shape the safe-delivery contract is about.
Runner::GuiDispatch copiedDispatch(Runner& runner) {
  return runner.post();
}

}  // namespace

// ── fixture ────────────────────────────────────────────────────────────────

void TestRunner::initTestCase() {
  // Deliveries are queued GUI events, so a case started without an application
  // object would fail for the wrong reason.
  QVERIFY2(QCoreApplication::instance() != nullptr,
           "the runner suite needs an application object to deliver on");
  QVERIFY2(QCoreApplication::instance()->thread() == QThread::currentThread(),
           "these cases must be started on the GUI thread");
}

void TestRunner::cleanupTestCase() {
  // Every case releases its own gates and drains its own pools. This sweep is
  // the belt-and-braces bound that guarantees nothing is left parked by an
  // earlier assertion failure.
  Runner::shutdownAll([] {
    // The drain finished: nothing is running anywhere.
  });
  spinFor(kShortBudgetMs);
}

// ── delivery through the public surface ────────────────────────────────────

void TestRunner::workRunsOffTheGuiThreadAndDeliversOnIt() {
  Runner runner;
  Ledger ledger;
  std::atomic<Qt::HANDLE> workerThread{nullptr};

  runner.run(
      [&workerThread]() -> QVariant {
        workerThread = QThread::currentThreadId();
        return QVariant::fromValue(7);
      },
      [&ledger](const QVariant& value) {
        ++ledger.deliveries;
        ledger.deliveredValueWasInt = value.toInt() == 7;
      },
      errDelivery(&ledger));

  QVERIFY2(spinUntil([&ledger] { return ledger.deliveries.load() == 1; }),
           "queued work delivered its result through the public API");
  QCOMPARE(ledger.deliveries.load(), 1);
  QCOMPARE(ledger.failures.load(), 0);
  QVERIFY2(ledger.deliveredValueWasInt.load(),
           "the value the work returned reached the success callback");
  QVERIFY2(workerThread.load() != nullptr &&
               workerThread.load() != QThread::currentThreadId(),
           "the work itself ran off the GUI thread");
  QCOMPARE(ledger.offGuiDeliveries.load(), 0);
}

void TestRunner::eachPoolAnswersTheCallerThatQueuedOnIt() {
  // Five pools, five entry points. Each one has to submit work rather than
  // quietly routing everything through one of them, and all five are awaited
  // on the same loop so they are observed running, not merely constructed.
  Runner runner;
  Ledger ledger;

  runner.run(countedWork(&ledger, nullptr), okDelivery(&ledger), errDelivery(&ledger));
  runner.runBulk(countedWork(&ledger, nullptr), okDelivery(&ledger), errDelivery(&ledger));
  runner.runStream(countedWork(&ledger, nullptr), okDelivery(&ledger), errDelivery(&ledger));
  runner.runImage(countedWork(&ledger, nullptr), okDelivery(&ledger), errDelivery(&ledger));
  runner.runTransfer(countedWork(&ledger, nullptr), okDelivery(&ledger), errDelivery(&ledger));

  QVERIFY2(spinUntil([&ledger] { return ledger.deliveries.load() == 5; }, kBudgetMs),
           "all five pools answered the work queued on them");
  QCOMPARE(ledger.workEntries.load(), 5);
  QCOMPARE(ledger.deliveries.load(), 5);
  QCOMPARE(ledger.failures.load(), 0);
}

void TestRunner::anErrorFromWorkReachesTheErrorCallback() {
  Runner runner;
  Ledger ledger;
  QString message;

  runner.run(
      []() -> QVariant { throw std::runtime_error("work refused"); },
      okDelivery(&ledger),
      [&message, &ledger](const backend::Failure& failure) {
        ++ledger.failures;
        message = failure.message;
      });

  QVERIFY2(spinUntil([&ledger] { return ledger.failures.load() == 1; }),
           "an exception from work reached the error callback rather than the process");
  QCOMPARE(ledger.deliveries.load(), 0);
  QVERIFY2(!message.isEmpty(), "the failure carried a message");
}

void TestRunner::theTwoTaggedErrorsReachTheirHandlerAndLaterTasksStillRun() {
  // The two failures the player branches on differently, plus one ordinary task
  // after them. A task that never ran, or a task that never reached its
  // handler, shows up here as a missing delivery.
  Runner runner;
  Ledger ledger;
  QString apiMessage;
  QString apiCode;
  QString noSourceMessage;

  runner.run(
      []() -> QVariant {
        backend::MikaiApiError error;
        error.code = QStringLiteral("rate_limited");
        error.message = QStringLiteral("slow down");
        throw error;
      },
      [](const QVariant&) {},
      [&apiMessage, &apiCode](const backend::Failure& failure) {
        apiMessage = failure.message;
        apiCode = failure.code;
      });

  runner.run(
      []() -> QVariant { throw backend::NoSourceError("this provider has no video"); },
      [](const QVariant&) {},
      [&noSourceMessage](const backend::Failure& failure) { noSourceMessage = failure.message; });

  runner.run(countedWork(&ledger, nullptr), okDelivery(&ledger), errDelivery(&ledger));

  QVERIFY2(spinUntil([&ledger] { return ledger.deliveries.load() == 1; }),
           "a later task still ran after two error paths");
  QVERIFY2(spinUntil([&apiMessage] { return !apiMessage.isEmpty(); }, kShortBudgetMs),
           "the API error reached its own handler");
  // The code is what a surface needs to say why, so it has to survive the
  // boundary that used to flatten it into the message.
  QCOMPARE(apiCode, QStringLiteral("rate_limited"));
  QVERIFY2(spinUntil([&noSourceMessage] { return !noSourceMessage.isEmpty(); }, kShortBudgetMs),
           "the no-source error reached its own handler");
  QCOMPARE(ledger.deliveries.load(), 1);
  QCOMPARE(ledger.failures.load(), 0);
}

void TestRunner::aCancelledDeliveryNeverReachesTheGuiThread() {
  // One task parks on a gate, so its completion has not been queued yet when
  // cancel() lands. Cancellation has to retire that work: neither the success
  // nor the error callback may fire once it finishes. A separate runner is
  // then drained on the same loop, which is what separates "this work was
  // cancelled" from "nothing delivers any more".
  Runner runner;
  Runner other;
  Ledger ledger;
  Ledger otherLedger;
  Gate hold;

  runner.run(countedWork(&ledger, &hold), okDelivery(&ledger), errDelivery(&ledger));
  QVERIFY2(spinUntil([&ledger] { return ledger.workEntries.load() >= 1; }),
           "the held task entered its work");

  runner.cancel();
  hold.release();

  other.run(countedWork(&otherLedger, nullptr), okDelivery(&otherLedger),
            errDelivery(&otherLedger));
  QVERIFY2(spinUntil([&otherLedger] { return otherLedger.deliveries.load() >= 1; }),
           "an independent runner still delivers after another one was cancelled");
  QVERIFY2(runner.waitForDone(kBudgetMs), "the cancelled runner's pools drained");
  spinFor(150);
  QCOMPARE(ledger.deliveries.load(), 0);
  QCOMPARE(ledger.failures.load(), 0);
  QCOMPARE(ledger.offGuiDeliveries.load(), 0);
  QCOMPARE(otherLedger.offGuiDeliveries.load(), 0);
}

void TestRunner::workPostedFromAWorkerThreadIsDeliveredOnTheGuiThread() {
  // The contract the widget owners depend on: the binder is copied on the GUI
  // thread before the work starts, the pool thread hands it a GUI body, and the
  // body still runs on the GUI thread. Nothing here copies a widget or a
  // QPointer off-thread, which is the bug the contract exists to prevent.
  Runner runner;
  Ledger ledger;
  std::atomic_int bodies{0};
  std::atomic_int bodiesOffGui{0};
  const Qt::HANDLE guiThread = QThread::currentThreadId();

  // Copied on the GUI thread, as an owner does, and never dereferenced there.
  const Runner::GuiDispatch post = copiedDispatch(runner);

  runner.run(
      [post, &bodies, &bodiesOffGui, guiThread]() -> QVariant {
        // Handed the body from the pool thread; the binder posts it, so the
        // worker never dispatches or touches a widget itself.
        post([&bodies, &bodiesOffGui, guiThread] {
          ++bodies;
          // Where the BODY ran is what the contract is about, so it is measured
          // inside the body rather than where the work started.
          if (QThread::currentThreadId() != guiThread) ++bodiesOffGui;
        });
        return QVariant::fromValue(1);
      },
      okDelivery(&ledger), errDelivery(&ledger));

  QVERIFY2(spinUntil([&bodies] { return bodies.load() >= 1; }),
           "a body posted from a pool thread reached the GUI thread");
  QCOMPARE(bodies.load(), 1);
  QCOMPARE(bodiesOffGui.load(), 0);
  QCOMPARE(ledger.deliveries.load(), 1);
  QCOMPARE(ledger.offGuiDeliveries.load(), 0);
}

void TestRunner::aPostFromAWorkerThreadOutlivesTheTaskThatPostedIt() {
  // The publication shape a real owner uses: several bodies posted from inside
  // one task, none of them tied to the task still running. Each has to arrive
  // on the GUI thread, in the order it was posted.
  Runner runner;
  std::atomic_int arrivals{0};
  int order[3] = {-1, -1, -1};

  const Runner::GuiDispatch post = copiedDispatch(runner);

  runner.run(
      [post, &arrivals, &order]() -> QVariant {
        for (int index = 0; index < 3; ++index) {
          const int payload = index + 1;
          post([&arrivals, &order, payload] {
            const int slot = arrivals.load();
            if (slot >= 0 && slot < 3) order[slot] = payload;
            ++arrivals;
          });
        }
        return QVariant::fromValue(1);
      },
      [](const QVariant&) {}, [](const backend::Failure&) {});

  QVERIFY2(spinUntil([&arrivals] { return arrivals.load() >= 3; }),
           "every body posted from inside one task arrived");
  QCOMPARE(arrivals.load(), 3);
  QCOMPARE(order[0], 1);
  QCOMPARE(order[1], 2);
  QCOMPARE(order[2], 3);
}

// ── cancellation ───────────────────────────────────────────────────────────

void TestRunner::cancelRetiresWorkThatHasNotStarted() {
  // The pools are wide, so the only safe way to hold a task queued rather than
  // running is to occupy the pool first and keep those threads busy. Far more
  // parked tasks than any pool can have threads guarantees the final submission
  // is still in the queue, so what this measures is the name of the case:
  // cancelling retires work that has NOT started, rather than running it and
  // suppressing only its delivery.
  Runner runner;
  Ledger ledger;
  Gate hold;
  std::atomic_bool queuedRan{false};
  std::atomic_int queuedDelivered{0};

  // Far more parked tasks than any pool can have threads: they queue.
  for (int i = 0; i < 64; ++i) {
    runner.runImage(countedWork(&ledger, &hold), okDelivery(&ledger), errDelivery(&ledger));
  }
  QVERIFY2(spinUntil([&ledger] { return ledger.workEntries.load() >= 1; }),
           "the parked batch was running on the image pool");

  runner.runImage(
      [&queuedRan]() -> QVariant {
        queuedRan = true;
        return QVariant::fromValue(1);
      },
      [&queuedDelivered](const QVariant&) { ++queuedDelivered; },
      [&queuedDelivered](const backend::Failure&) { ++queuedDelivered; });

  runner.cancel();
  hold.release();

  QVERIFY2(runner.waitForDone(kBudgetMs), "the pools drained inside the test budget");
  spinFor(200);
  QCOMPARE(queuedRan.load(), false);
  QCOMPARE(queuedDelivered.load(), 0);
  QCOMPARE(ledger.deliveries.load(), 0);
  QCOMPARE(ledger.failures.load(), 0);
}

void TestRunner::cancelStillAllowsAnExplicitDrainToFinish() {
  // The probes drain a runner by hand, so waitForDone has to keep working after
  // a cancellation: a cancelled batch that still occupies its threads must not
  // make the drain time out.
  Runner runner;
  Ledger ledger;
  Gate hold;

  runner.run(countedWork(&ledger, &hold), okDelivery(&ledger), errDelivery(&ledger));
  QVERIFY2(spinUntil([&ledger] { return ledger.workEntries.load() >= 1; }),
           "the held task entered its work");

  runner.cancel();
  hold.release();
  QVERIFY2(runner.waitForDone(kBudgetMs),
           "an explicit drain still completes after a cancellation");
  spinFor(100);
  QCOMPARE(ledger.deliveries.load(), 0);
  QCOMPARE(ledger.failures.load(), 0);
}

void TestRunner::cancelIsIdempotentWhileWorkIsOutstanding() {
  // A close path cancels while several owners still hold references, so the
  // repeated call has to be harmless: the work is retired once, and neither
  // callback fires when it finishes. The second and third calls land while the
  // first is still retiring work, which is the window a real close path hits.
  Runner runner;
  Ledger ledger;
  Gate hold;

  runner.run(countedWork(&ledger, &hold), okDelivery(&ledger), errDelivery(&ledger));
  QVERIFY2(spinUntil([&ledger] { return ledger.workEntries.load() == 1; }),
           "the held task entered its work");

  runner.cancel();
  runner.cancel();
  runner.cancel();

  hold.release();
  QVERIFY2(runner.waitForDone(kBudgetMs),
           "the cancelled runner's pools drained after repeated cancellation");
  spinFor(150);
  QCOMPARE(ledger.deliveries.load(), 0);
  QCOMPARE(ledger.failures.load(), 0);
  QCOMPARE(ledger.offGuiDeliveries.load(), 0);
}

void TestRunner::anUncancelledInnerScopeDoesNotMaskACancelledOuterOne() {
  // With a stack of scopes, only the innermost one being uncancelled must not
  // hide a cancelled enclosing one. This discriminates against the single-flag
  // spelling, where the inner unit of work would silently re-enable work the
  // caller already asked to stop - the shutdown path parks a worker whose own
  // gate is dead while the owner's is not.
  std::atomic_bool outer{false};
  std::atomic_bool inner{false};

  QVERIFY2(!backend::WorkCancellationScope::cancellationRequested(),
           "nothing is cancelled before any scope is installed");

  {
    backend::WorkCancellationScope outerScope(&outer);
    QVERIFY2(!backend::WorkCancellationScope::cancellationRequested(),
             "an installed scope is not cancelled until its flag is set");

    // The discriminating case: the inner scope is untouched, and its mere
    // presence must not make the answer come back false.
    outer.store(true);
    {
      backend::WorkCancellationScope innerScope(&inner);
      QVERIFY2(backend::WorkCancellationScope::cancellationRequested(),
               "an uncancelled inner scope must not mask a cancelled outer one");
    }
    // The inner scope is gone: the outer one is still cancelled.
    QVERIFY2(backend::WorkCancellationScope::cancellationRequested(),
             "destroying the inner scope restores the outer one's cancelled state");
  }
  // The outer scope is gone too, and nothing is cancelled again.
  QVERIFY2(!backend::WorkCancellationScope::cancellationRequested(),
           "destroying the cancelled outer scope clears the state");

  // And the same shape the other way round, so "true" is not simply sticky.
  std::atomic_bool inner2{false};
  std::atomic_bool outer2{false};
  inner2.store(true);
  {
    backend::WorkCancellationScope outerScope2(&outer2);
    {
      backend::WorkCancellationScope innerScope2(&inner2);
      QVERIFY2(backend::WorkCancellationScope::cancellationRequested(),
               "a cancelled inner scope is observed while both are installed");
    }
    QVERIFY2(!backend::WorkCancellationScope::cancellationRequested(),
             "destroying the cancelled inner scope leaves the outer one uncancelled");
  }
  QVERIFY2(!backend::WorkCancellationScope::cancellationRequested(),
           "no state survives the outermost scope");
}

void TestRunner::cancelDuringWorkStopsLaterStagesOfTheSameTask() {
  // A long task that checks the backend gate between stages must be able to
  // stop itself. The gate is the real one the backend's blocking waits honour,
  // so this is the aborting-wait contract rather than a re-implementation.
  Runner runner;
  std::shared_ptr<std::atomic_bool> gate = std::make_shared<std::atomic_bool>(false);
  std::atomic_int stages{0};
  std::atomic_int reachedMidpoint{0};
  std::atomic_int outcomes{0};
  Gate hold;

  QVERIFY2(!backend::WorkCancellationScope::cancellationRequested(),
           "no cancellation scope is installed on the GUI thread");

  runner.run(
      [gate, &stages, &reachedMidpoint, &outcomes, &hold]() -> QVariant {
        backend::WorkCancellationScope scope(gate.get());
        for (int stage = 0; stage < 50; ++stage) {
          ++stages;
          if (backend::WorkCancellationScope::cancellationRequested()) {
            ++outcomes;
            return QVariant::fromValue(0);
          }
          // Park part-way through, so the cancellation lands mid-task rather
          // than before the task starts or after it has already finished. The
          // park is announced BEFORE it blocks: a flag raised only after the
          // wait returns cannot be observed while the task is still parked, and
          // the canceller would be racing the next stage's check.
          if (stage == 25 && reachedMidpoint.load() == 0) {
            reachedMidpoint = 1;
            hold.waitUntilReleased(kHoldMs);
          }
        }
        ++outcomes;
        return QVariant::fromValue(1);
      },
      [&outcomes](const QVariant&) { ++outcomes; }, [](const backend::Failure&) {});

  QVERIFY2(spinUntil([&reachedMidpoint] { return reachedMidpoint.load() != 0; }),
           "the task parked part-way through its stages");
  QVERIFY2(stages.load() >= 26,
           "the task had already completed stages before the cancellation landed");

  // Set the gate before lifting it, so the worker cannot slip past the check.
  gate->store(true);
  hold.release();

  QVERIFY2(spinUntil([&outcomes] { return outcomes.load() >= 1; }),
           "the cancelled task finished rather than parking the pool");
  QVERIFY2(stages.load() < 50,
           "the task stopped at the cancellation gate instead of running every stage");
  QVERIFY2(!backend::WorkCancellationScope::cancellationRequested(),
           "the scope the worker installed did not leak onto the GUI thread");
}

// ── owner deletion (KB-06 / KB-11) ─────────────────────────────────────────

void TestRunner::deletingAnOwnerReturnsPromptlyWhileWorkIsStillRunning() {
  // The regression: the destructor used to block for up to thirty seconds while
  // the pools drained, and Qt then blocked again destroying them. Deleting has
  // to return while work is still outstanding.
  Ledger ledger;
  Gate hold;
  auto* runner = new Runner();

  runner->run(countedWork(&ledger, &hold), okDelivery(&ledger), errDelivery(&ledger));
  runner->run(countedWork(&ledger, &hold), okDelivery(&ledger), errDelivery(&ledger));
  QVERIFY2(spinUntil([&ledger] { return ledger.workEntries.load() == 2; }),
           "both held tasks entered their work");

  QElapsedTimer clock;
  clock.start();
  delete runner;
  const qint64 elapsed = clock.elapsed();

  QVERIFY2(elapsed < 2000,
           qPrintable(QStringLiteral("deleting an owner with work outstanding took %1 ms")
                          .arg(elapsed)));

  // The work outlives the deleted owner: ~Runner removed it from the shutdown
  // registry, so shutdownAll would drain nothing here. The last task's own
  // shared_ptr keeps the pools and the completions alive until it really ends,
  // which is exactly what is being proved - the two parked bodies still finish
  // after their owner is gone, so the thread exits on its own and this case's
  // stack locals cannot outlive it.
  hold.release();
  QVERIFY2(spinUntil([&ledger] { return ledger.workCompletions.load() == 2; }, kBudgetMs),
           "the work outlived the deleted owner and ran to completion");
  // The queued completion also has to be allowed to execute on the GUI thread,
  // because that is where its abandonment gate is re-read. Giving it the loop is
  // what makes the zero-delivery assertion meaningful rather than premature.
  spinFor(250);
  QCOMPARE(ledger.deliveries.load(), 0);
  QCOMPARE(ledger.failures.load(), 0);

  Runner other;
  std::atomic_int survived{0};
  other.run([]() -> QVariant { return QVariant::fromValue(1); },
             [&survived](const QVariant&) { ++survived; }, [](const backend::Failure&) {});
  QVERIFY2(spinUntil([&survived] { return survived.load() >= 1; }),
           "a fresh runner still answers after an owner was deleted mid-work");
  QCOMPARE(survived.load(), 1);
}

void TestRunner::deletingAnOwnerNeverDeliversToTheDeadOwner() {
  // The KB-06 half: a completion that has not been queued yet must not be handed
  // to an owner that has just been destroyed. The callbacks record into state
  // that outlives the runner, which is the window a raw-widget owner used to
  // close on.
  Ledger ledger;
  Gate hold;
  std::atomic_int late{0};

  {
    auto runner = std::make_unique<Runner>();
    runner->run(
        countedWork(&ledger, &hold),
        [&late](const QVariant&) { ++late; },
        [&late](const backend::Failure&) { ++late; });
    QVERIFY2(spinUntil([&ledger] { return ledger.workEntries.load() == 1; }),
             "the held task entered its work");
    // The owner goes while its task is still parked on the gate.
    runner.reset();
  }

  // Now the task finishes, behind an owner who is already gone. ~Runner set the
  // abandonment gate, and the gate is re-read by the queued body when it runs on
  // the GUI thread, so the loop has to run long enough for that body to execute
  // for the assertion below to be a real result rather than a premature zero.
  hold.release();
  QVERIFY2(spinUntil([&ledger] { return ledger.workCompletions.load() == 1; }, kBudgetMs),
           "the orphaned work still ran to completion behind its dead owner");
  spinFor(250);

  QCOMPARE(late.load(), 0);
  QCOMPARE(ledger.deliveries.load(), 0);
  QCOMPARE(ledger.failures.load(), 0);
  QCOMPARE(ledger.offGuiDeliveries.load(), 0);
}

void TestRunner::deletingAnOwnerLeavesOtherOwnersAnswering() {
  std::atomic_int keeperDelivered{0};

  // The doomed ledger must outlive its work, so it lives in this scope and is
  // drained before the block ends.
  {
    Ledger doomed;
    Gate hold;
    auto runner = std::make_unique<Runner>();
    runner->run(countedWork(&doomed, &hold), okDelivery(&doomed), errDelivery(&doomed));
    QVERIFY2(spinUntil([&doomed] { return doomed.workEntries.load() == 1; }),
             "the doomed runner's task entered its work");
    runner.reset();
    // ~Runner left the shutdown registry, so the drain is the task's own
    // completion: the last task's shared_ptr keeps the pools alive until it ends.
    hold.release();
    QVERIFY2(spinUntil([&doomed] { return doomed.workCompletions.load() == 1; }, kBudgetMs),
             "the orphaned work ran to completion before the doomed ledger went away");
    spinFor(150);
    QCOMPARE(doomed.deliveries.load(), 0);
  }

  Runner keeper;
  keeper.run([]() -> QVariant { return QVariant::fromValue(1); },
             [&keeperDelivered](const QVariant&) { ++keeperDelivered; },
             [](const backend::Failure&) {});
  QVERIFY2(spinUntil([&keeperDelivered] { return keeperDelivered.load() >= 1; }),
           "a surviving runner still answers after its neighbour was deleted");
  QCOMPARE(keeperDelivered.load(), 1);
}

void TestRunner::aSecondOwnerSeesAResponsiveGuiThreadWhileTheFirstOneWorkIs() {
  // Two runners, real work outstanding on the first, and a GUI heartbeat that
  // keeps ticking. This is the responsiveness half of KB-11: queued work must
  // not stall the loop, and a busy owner must not either.
  Runner busy;
  Runner idle;
  Ledger busyLedger;
  Gate hold;

  busy.run(countedWork(&busyLedger, &hold), okDelivery(&busyLedger), errDelivery(&busyLedger));
  busy.run(countedWork(&busyLedger, &hold), okDelivery(&busyLedger), errDelivery(&busyLedger));
  QVERIFY2(spinUntil([&busyLedger] { return busyLedger.workEntries.load() == 2; }),
           "the busy runner had two tasks outstanding");

  std::atomic_int idleDelivered{0};
  idle.run([]() -> QVariant { return QVariant::fromValue(1); },
           [&idleDelivered](const QVariant&) { ++idleDelivered; },
           [](const backend::Failure&) {});
  QVERIFY2(spinUntil([&idleDelivered] { return idleDelivered.load() >= 1; }),
           "a second runner delivers while the first one is held");
  QCOMPARE(busyLedger.deliveries.load(), 0);

  // Declared inside this scope on purpose: the heartbeat's lifetime is the
  // window being measured, and it is destroyed before `busy` and `idle` are.
  {
    Heartbeat heartbeat;
    const int before = heartbeat.ticks();
    spinFor(300);
    QVERIFY2(heartbeat.ticks() > before,
             "the GUI thread kept answering while work was outstanding on another runner");
  }

  hold.release();
  QVERIFY2(busy.waitForDone(kBudgetMs), "the busy runner drained after its work was released");
}

// ── shutdownAll (KB-11) ────────────────────────────────────────────────────

void TestRunner::shutdownAllKeepsTheGuiThreadAnsweringWhileWorkDrains() {
  // The call has to return rather than block: that is what lets the application
  // keep its loop alive until the drain is really over.
  Runner first;
  Runner second;
  Ledger ledger;
  Gate hold;
  Heartbeat heartbeat;

  first.run(countedWork(&ledger, &hold), okDelivery(&ledger), errDelivery(&ledger));
  second.run(countedWork(&ledger, &hold), okDelivery(&ledger), errDelivery(&ledger));
  QVERIFY2(spinUntil([&ledger] { return ledger.workEntries.load() == 2; }),
           "both runners had work outstanding");

  std::atomic_bool drained{false};
  Runner::shutdownAll([&drained] { drained = true; });

  const int before = heartbeat.ticks();
  spinFor(250);
  QVERIFY2(heartbeat.ticks() > before,
           "shutdownAll returned while work was outstanding, and the GUI thread kept answering");
  QCOMPARE(drained.load(), false);

  hold.release();
  QVERIFY2(spinUntil([&drained] { return drained.load(); }),
           "the finish callback ran once the work was really gone");
  QCOMPARE(ledger.deliveries.load(), 0);
}

void TestRunner::shutdownAllRunsItsFinishCallbackOnceTheWorkIsReallyGone() {
  Runner runner;
  Ledger ledger;
  Gate hold;

  runner.run(countedWork(&ledger, &hold), okDelivery(&ledger), errDelivery(&ledger));
  QVERIFY2(spinUntil([&ledger] { return ledger.workEntries.load() == 1; }),
           "the held task entered its work");

  std::atomic_int finishCalls{0};
  Runner::shutdownAll([&finishCalls] { ++finishCalls; });

  // The work is still held, so the drain cannot be over.
  spinFor(200);
  QCOMPARE(finishCalls.load(), 0);

  hold.release();
  QVERIFY2(spinUntil([&finishCalls] { return finishCalls.load() >= 1; }),
           "the finish callback ran after the held work finished");
  QCOMPARE(finishCalls.load(), 1);
  QCOMPARE(ledger.deliveries.load(), 0);
}

void TestRunner::workCancelledBeforeShutdownNeverDelivers() {
  // Cancellation followed by a drain: the cancelled work must be retired by the
  // drain itself, not delivered on the way out. This is the late-callback bug
  // in its shutdown form.
  Runner runner;
  Ledger ledger;
  Gate hold;
  std::atomic_int late{0};

  // One task carries callbacks of its own so the delivery being watched is
  // specific to this case, and one uses the shared ledger counters.
  runner.run(countedWork(&ledger, &hold),
             [&late](const QVariant&) { ++late; }, [&late](const backend::Failure&) { ++late; });
  runner.run(countedWork(&ledger, &hold), okDelivery(&ledger), errDelivery(&ledger));
  QVERIFY2(spinUntil([&ledger] { return ledger.workEntries.load() == 2; }),
           "both held tasks entered their work");
  QCOMPARE(late.load(), 0);

  runner.cancel();
  std::atomic_bool drained{false};
  Runner::shutdownAll([&drained] { drained = true; });
  hold.release();

  QVERIFY2(spinUntil([&drained] { return drained.load(); }), "the shutdown drain completed");
  spinFor(200);
  QCOMPARE(late.load(), 0);
  QCOMPARE(ledger.deliveries.load(), 0);
  QCOMPARE(ledger.failures.load(), 0);
}

// ── the suite entry point the shared runner registers ──────────────────────

int runTestRunner(int argc, char** argv) {
  TestRunner suite;
  QStringList arguments;
  arguments << (argc > 0 ? QString::fromLocal8Bit(argv[0]) : QStringLiteral("mikai_tests"));
  for (int i = 1; i < argc; ++i) arguments << QString::fromLocal8Bit(argv[i]);
  QTemporaryFile report;
  const bool captureReport = !arguments.contains(QStringLiteral("-o"));
  if (captureReport) {
    if (!report.open()) {
      std::fprintf(stderr, "[mikai_tests] cannot create report for TestRunner\n");
      return 2;
    }
    report.close();
    arguments << QStringLiteral("-o") << report.fileName() + QStringLiteral(",txt");
  }
  const int rc = QTest::qExec(&suite, arguments);
  if (captureReport && rc != 0) {
    QFile output(report.fileName());
    if (output.open(QIODevice::ReadOnly)) {
      const QByteArray text = output.readAll();
      std::fwrite(text.constData(), 1, static_cast<size_t>(text.size()), stderr);
    } else {
      std::fprintf(stderr, "[mikai_tests] cannot read report for TestRunner\n");
    }
  }
  std::fflush(stderr);
  return rc;
}
