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

  // The epoch a slot belongs to. A key change starts a fresh window, and the
  // slot a request was granted has to say which window that is: without it, a
  // quota read answered by the old key would land in the new key's counter.
  void theFirstGenerationIsZeroAndASlotCarriesIt();
  void resettingTheGenerationReleasesEveryWindow();
  void aSlotGrantedBeforeAResetStillKnowsItsGeneration();
  void aQuotaUpdateFromAnEarlierGenerationDropsTheWindowAgain();

  // Cancellation: a parked caller must not wait out an advertised window while
  // a shutdown is already under way. The scope is a chain, so a cancelled outer
  // scope must not be masked by an uncancelled inner one.
  void aCancelledOuterScopeIsNotMaskedByAnInnerOne();
  void aCancelledScopeStopsAnsweringOnceItIsPopped();
  void twoUncancelledScopesAnswerNo();
  void aCancelledSlotIsRefusedRatherThanParked();
  void aCallerCancelledWhileParkedIsRefusedInsideTheWindow();
};
