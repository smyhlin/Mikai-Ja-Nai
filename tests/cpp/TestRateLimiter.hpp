// The rate-limit gate: one shared counter, and a 429 that parks rather than fails.

#pragma once

#include <QtTest/QTest>

class TestRateLimiter : public QObject {
  Q_OBJECT

 private slots:
  void reportsTheDefaultQuotaForKeyedAndAnonymous();
  void reportsNothingUsedBeforeAnyCall();
  void aFreshCallReleasesItsSlot();
  void readsTheAdvertisedQuotaFromHeaders();
  void keepsTheConservativeRemainingWhileTheWindowIsOpen();
  void aStaleGenerationDoesNotTouchTheCounters();
  void countsQueuedCallersWhileTheyWait();
  void parksTheNextCallerUntilRetryAfterOnA429();
  void aLaterSuccessClearsThePark();
  void forgetsTheQuotaOnceTheResetWindowElapses();
  void forgetTheQuotaHappensInTheStateSnapshot();
};
