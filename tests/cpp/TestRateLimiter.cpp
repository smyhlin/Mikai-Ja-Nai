#include "TestRateLimiter.hpp"

#include <backend/Cancellation.hpp>
#include <backend/MikaiClient.hpp>

#include <atomic>

#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>

using mikai::backend::MikaiApiError;
using mikai::backend::RateLimitState;
using mikai::backend::RateLimiter;

namespace {

QMap<QString, QString> headers(int limit, int remaining, int resetSeconds,
                               const QString& retryAfter = QString()) {
  QMap<QString, QString> out;
  out.insert(QStringLiteral("X-RateLimit-Limit"), QString::number(limit));
  out.insert(QStringLiteral("X-RateLimit-Remaining"), QString::number(remaining));
  out.insert(QStringLiteral("X-RateLimit-Reset"), QString::number(resetSeconds));
  if (!retryAfter.isEmpty()) {
    out.insert(QStringLiteral("Retry-After"), retryAfter);
  }
  return out;
}

}  // namespace

void TestRateLimiter::reportsTheDefaultQuotaForKeyedAndAnonymous() {
  const RateLimiter limiter;
  // The server never answered yet, so the local default stands: a keyed client
  // is on the 300/min plan, an anonymous one on 60.
  QCOMPARE(limiter.state(true).limit, 300);
  QCOMPARE(limiter.state(false).limit, 60);
}

void TestRateLimiter::reportsNothingUsedBeforeAnyCall() {
  const RateLimiter limiter;
  const RateLimitState state = limiter.state(true);
  // "used" is unknown, not zero: reporting 0 would render a full bar before the
  // first request ever landed.
  QVERIFY(!state.used.has_value());
  QVERIFY(!state.remaining.has_value());
  QCOMPARE(state.inFlight, 0);
  QCOMPARE(state.queued, 0);
}

void TestRateLimiter::aFreshCallReleasesItsSlot() {
  RateLimiter limiter;
  const int generation = limiter.acquireSlot();
  QCOMPARE(limiter.state(true).inFlight, 1);
  limiter.finishCall(headers(300, 300, 60), generation, 200);
  QCOMPARE(limiter.state(true).inFlight, 0);
}

void TestRateLimiter::readsTheAdvertisedQuotaFromHeaders() {
  RateLimiter limiter;
  const int generation = limiter.acquireSlot();
  limiter.finishCall(headers(300, 299, 60), generation, 200);
  const RateLimitState state = limiter.state(true);
  QCOMPARE(state.limit, 300);
  QVERIFY(state.remaining.has_value());
  QCOMPARE(state.remaining.value(), 299);
  QVERIFY(state.used.has_value());
  QCOMPARE(state.used.value(), 1);
  QCOMPARE(state.resetIn, 60);
}

void TestRateLimiter::keepsTheConservativeRemainingWhileTheWindowIsOpen() {
  RateLimiter limiter;
  // Headers from concurrent requests arrive out of order: the second call here
  // started later but reports fewer remaining. Taking it at face value would
  // hand out budget the server has already spent.
  const int first = limiter.acquireSlot();
  const int second = limiter.acquireSlot();
  limiter.finishCall(headers(300, 290, 60), second, 200);
  limiter.finishCall(headers(300, 299, 60), first, 200);
  QCOMPARE(limiter.state(true).remaining.value(), 290);
  QCOMPARE(limiter.state(true).used.value(), 10);
}

void TestRateLimiter::aStaleGenerationDoesNotTouchTheCounters() {
  RateLimiter limiter;
  const int generation = limiter.acquireSlot();
  // The key changed under an in-flight call; its headers describe a quota that
  // no longer applies to this client.
  limiter.finishCall(headers(300, 5, 60), generation + 1, 200);
  const RateLimitState state = limiter.state(true);
  QVERIFY(!state.remaining.has_value());
  QCOMPARE(state.limit, 300);
}

void TestRateLimiter::countsQueuedCallersWhileTheyWait() {
  RateLimiter limiter;
  // Drain the advertised quota so the next caller has to wait.
  const int generation = limiter.acquireSlot();
  limiter.finishCall(headers(300, 0, 2), generation, 200);
  QCOMPARE(limiter.state(true).inFlight, 0);

  QThread* waiter = QThread::create([&limiter] { limiter.acquireSlot(); });
  waiter->start();
  QTRY_VERIFY_WITH_TIMEOUT(limiter.state(true).queued >= 1, 2000);
  QCOMPARE(limiter.state(true).inFlight, 0);
  QVERIFY(waiter->wait(4000));
  delete waiter;
}

void TestRateLimiter::parksTheNextCallerUntilRetryAfterOnA429() {
  RateLimiter limiter;
  // A 429 parks callers for the advertised window instead of failing them: the
  // title the user opened should still load, just a second later.
  const int generation = limiter.acquireSlot();
  limiter.finishCall(headers(300, 0, 1, QStringLiteral("1")), generation, 429);

  QElapsedTimer timer;
  timer.start();
  const int next = limiter.acquireSlot();
  const qint64 waited = timer.elapsed();
  limiter.finishCall(headers(300, 299, 60), next, 200);

  QVERIFY2(waited >= 900, qPrintable(QStringLiteral("waited only %1ms").arg(waited)));
  QVERIFY(waited < 3000);
}

void TestRateLimiter::aLaterSuccessClearsThePark() {
  RateLimiter limiter;
  const int generation = limiter.acquireSlot();
  limiter.finishCall(headers(300, 0, 1, QStringLiteral("1")), generation, 429);
  const int next = limiter.acquireSlot();
  limiter.finishCall(headers(300, 299, 60), next, 200);
  // The retry is over, so a third caller walks straight through.
  QElapsedTimer timer;
  timer.start();
  const int third = limiter.acquireSlot();
  const qint64 waited = timer.elapsed();
  limiter.finishCall(headers(300, 298, 60), third, 200);
  QVERIFY2(waited < 500, qPrintable(QStringLiteral("still parked after %1ms").arg(waited)));
  QCOMPARE(limiter.state(true).remaining.value(), 298);
}

void TestRateLimiter::forgetsTheQuotaOnceTheResetWindowElapses() {
  RateLimiter limiter;
  const int generation = limiter.acquireSlot();
  limiter.finishCall(headers(300, 1, 0), generation, 200);
  // A zero-second reset window is already over by the time we look.
  QTRY_VERIFY_WITH_TIMEOUT(!limiter.state(true).remaining.has_value(), 3000);
  QVERIFY(!limiter.state(true).used.has_value());
}

void TestRateLimiter::forgetTheQuotaHappensInTheStateSnapshot() {
  RateLimiter limiter;
  const int generation = limiter.acquireSlot();
  limiter.finishCall(headers(300, 42, 0), generation, 200);
  // The snapshot is what the RPM label and the settings quota panel read, so
  // the stale window has to be cleared there and not only on the next call.
  const RateLimitState state = limiter.state(true);
  QVERIFY(!state.remaining.has_value());
  QVERIFY(!state.used.has_value());
  QCOMPARE(state.resetIn, 0);
}

void TestRateLimiter::theFirstGenerationIsZeroAndASlotCarriesIt() {
  RateLimiter limiter;
  QCOMPARE(limiter.generation(), 0);
  const int slot = limiter.acquireSlot();
  // A request is told which window it was granted inside: that is what tells a
  // quota reply whether it belongs here at all.
  QCOMPARE(slot, limiter.generation());
  QCOMPARE(slot, 0);
  limiter.finishCall(headers(300, 299, 60), slot, 200);
  QCOMPARE(limiter.generation(), 0);
}

void TestRateLimiter::resettingTheGenerationReleasesEveryWindow() {
  RateLimiter limiter;
  const int first = limiter.acquireSlot();
  limiter.finishCall(headers(300, 299, 60), first, 200);
  limiter.reset();

  QCOMPARE(limiter.generation(), 1);
  const RateLimitState state = limiter.state(true);
  // The new key gets a fresh window rather than the previous key's count.
  QVERIFY(!state.remaining.has_value());
  QVERIFY(!state.used.has_value());
  QCOMPARE(state.limit, 300);
  QCOMPARE(state.resetIn, 0);
}

void TestRateLimiter::aSlotGrantedBeforeAResetStillKnowsItsGeneration() {
  RateLimiter limiter;
  const int beforeReset = limiter.acquireSlot();
  // The key changes while this call is in flight. Whatever the reply says about
  // quota, it arrived for the window this slot was granted in - and only that
  // window may accept it.
  limiter.reset();
  QCOMPARE(beforeReset, 0);
  QVERIFY(beforeReset < limiter.generation());

  limiter.finishCall(headers(300, 1, 60), beforeReset, 200);
  const RateLimitState state = limiter.state(true);
  QVERIFY2(!state.remaining.has_value(),
           "quota answered under a superseded key was applied to the new one");
  QVERIFY2(!state.used.has_value(), "the old window's usage leaked into the new one");
  QCOMPARE(state.resetIn, 0);
  QCOMPARE(state.limit, 300);
}

void TestRateLimiter::aQuotaUpdateFromAnEarlierGenerationDropsTheWindowAgain() {
  RateLimiter limiter;
  const int stale = limiter.acquireSlot();
  limiter.reset();
  const int fresh = limiter.acquireSlot();

  // The reply of the pre-reset call lands after the reset: it describes a
  // window the new key never had.
  limiter.finishCall(headers(300, 7, 60), stale, 200);
  QVERIFY(!limiter.state(true).remaining.has_value());

  // The reply of the new-epoch call still counts, so a reset does not blind the
  // limiter to every later answer.
  limiter.finishCall(headers(300, 5, 60), fresh, 200);
  QCOMPARE(limiter.state(true).remaining.value(), 5);
  QCOMPARE(limiter.state(true).used.value(), 295);
}

// The scope chain is a stack, and the question it answers is "is any unit of
// work on this thread cancelled", not "is the innermost one". A runner nests
// units, so the inner scope must not mask a cancellation the outer one raised:
// that is the shape of a shutdown pulling out of a nested call.
void TestRateLimiter::aCancelledOuterScopeIsNotMaskedByAnInnerOne() {
  using mikai::backend::WorkCancellationScope;
  std::atomic<bool> outerCancel{true};
  std::atomic<bool> innerCancel{false};

  QVERIFY(!WorkCancellationScope::cancellationRequested());
  {
    const WorkCancellationScope outer(&outerCancel);
    // The outer one is cancelled and there is nothing installed in front of it.
    QVERIFY(WorkCancellationScope::cancellationRequested());
    {
      const WorkCancellationScope inner(&innerCancel);
      // The inner scope is NOT cancelled. It must not answer "no" on behalf of a
      // chain whose outer scope is: this is the masking that was fixed.
      QVERIFY2(WorkCancellationScope::cancellationRequested(),
               "an uncancelled inner scope masked the cancelled outer one");
    }
    // The inner one is gone, so the chain is the outer one again.
    QVERIFY(WorkCancellationScope::cancellationRequested());
  }
  // Nothing is installed at all now, so the answer is "no" rather than stale.
  QVERIFY(!WorkCancellationScope::cancellationRequested());
}

void TestRateLimiter::aCancelledScopeStopsAnsweringOnceItIsPopped() {
  using mikai::backend::WorkCancellationScope;
  std::atomic<bool> cancelled{false};

  {
    const WorkCancellationScope scope(&cancelled);
    // Not yet cancelled: an installed-but-idle scope is not a cancellation.
    QVERIFY(!WorkCancellationScope::cancellationRequested());
    cancelled = true;
    QVERIFY(WorkCancellationScope::cancellationRequested());
  }
  // The scope only borrows the flag; it does not own the answer. Once it is
  // popped, its cancellation is nobody's business.
  QVERIFY(!WorkCancellationScope::cancellationRequested());
}

void TestRateLimiter::twoUncancelledScopesAnswerNo() {
  using mikai::backend::WorkCancellationScope;
  std::atomic<bool> first{false};
  std::atomic<bool> second{false};
  const WorkCancellationScope outer(&first);
  const WorkCancellationScope inner(&second);
  QVERIFY(!WorkCancellationScope::cancellationRequested());
}

void TestRateLimiter::aCancelledSlotIsRefusedRatherThanParked() {
  RateLimiter limiter;
  const int draining = limiter.acquireSlot();
  limiter.finishCall(headers(300, 0, 30), draining, 200);
  const int generation = limiter.generation();

  // A shutdown is already under way. A request that would now wait out a
  // 30-second window is told to stop rather than being parked for it, so the
  // pool it belongs to can drain without it.
  std::atomic<bool> cancelled{true};
  const mikai::backend::WorkCancellationScope scope(&cancelled);
  QVERIFY(mikai::backend::WorkCancellationScope::cancellationRequested());
  try {
    const int granted = limiter.acquireSlot();
    QFAIL(qPrintable(QStringLiteral("a cancelled caller was granted generation %1 instead of being "
                                    "refused")
                         .arg(granted)));
  } catch (const MikaiApiError& error) {
    QCOMPARE(error.code, QStringLiteral("cancelled"));
    QVERIFY2(!error.message.isEmpty(), "a cancellation carries no message");
  }
  // Nothing was handed out, so nothing is owed a quota update either.
  QCOMPARE(limiter.generation(), generation);
  QCOMPARE(limiter.state(true).inFlight, 0);
  QCOMPARE(limiter.state(true).queued, 0);
}

void TestRateLimiter::aCallerCancelledWhileParkedIsRefusedInsideTheWindow() {
  RateLimiter limiter;
  // Close the window for a full half minute: that is the window a caller that
  // is cancelled only after it has already parked would otherwise sit out.
  const int first = limiter.acquireSlot();
  limiter.finishCall(headers(300, 0, 30), first, 200);

  // The scope belongs to the caller's own thread; the flag it reads is shared,
  // so the test can raise it once that caller is verifiably parked. This is the
  // case a single pre-set flag cannot reach: cancellation arriving mid-park.
  std::atomic<bool> cancelled{false};
  std::atomic<bool> sawCancellation{false};
  QThread* worker = QThread::create([&limiter, &cancelled, &sawCancellation] {
    const mikai::backend::WorkCancellationScope scope(&cancelled);
    try {
      limiter.acquireSlot();
    } catch (const MikaiApiError&) {
      sawCancellation = true;
    }
  });
  QElapsedTimer clock;
  clock.start();
  worker->start();
  QTRY_VERIFY_WITH_TIMEOUT(limiter.state(true).queued >= 1, 10000);
  // Parked. The shutdown raises the flag it is reading and nothing else.
  cancelled = true;

  QVERIFY2(worker->wait(30000), "the mid-park cancelled caller never returned");
  delete worker;
  QVERIFY2(sawCancellation.load(), "a caller cancelled mid-park was not refused");
  // The sliced wait is the whole point: a caller must not outlive a half-minute
  // window just because the shutdown started one of its slices ago.
  QVERIFY2(clock.elapsed() < 5000,
           qPrintable(QStringLiteral("a cancelled caller waited out %1ms").arg(clock.elapsed())));
  QCOMPARE(limiter.state(true).queued, 0);
  QCOMPARE(limiter.state(true).inFlight, 0);
}
