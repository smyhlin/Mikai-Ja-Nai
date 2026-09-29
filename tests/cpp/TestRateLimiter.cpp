#include "TestRateLimiter.hpp"

#include <backend/MikaiClient.hpp>

#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>

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
