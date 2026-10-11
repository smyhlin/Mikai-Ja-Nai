// Runner lifetime regressions: pending-owner deletion, cancellation and safe
// delivery, driven through the public post/cancel/shutdownAll surface only.
//
// Three behaviours are being pinned, all of them consequences of KB-06 and KB-11:
//
//  1. Deleting a Runner whose pools still hold queued work must not walk away
//     from the work or from the completion: the owner goes away promptly, and
//     nothing is delivered to it afterwards. The old destructor blocked the
//     caller for up to thirty seconds while the pools drained, and then let Qt
//     block again for the same work while destroying them - so "promptly" and
//     "not delivered" are both part of the claim.
//  2. cancel() retires work that has not started, so a task queued behind two
//     occupied pool threads is dropped rather than run and suppressed.
//  3. shutdownAll() drains asynchronously while the GUI thread keeps answering,
//     which is what lets application shutdown keep backend references alive
//     until the work really has finished.
//
// Everything here is observed through the public API with real work on the real
// pools. No private state, no source text and no call counts are consulted: a
// regression in any of the three behaviours shows up as a delivery that should
// not exist, a delivery that never arrives, or a GUI thread that stopped
// answering while work was outstanding. Each case releases its gates and drains
// its pools before it returns, so a failing run reports a failure rather than
// hanging the suite.

#pragma once

#include <QtTest/QTest>

class TestRunner : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void cleanupTestCase();

  // ── delivery through the public surface ────────────────────────────────
  void workRunsOffTheGuiThreadAndDeliversOnIt();
  void eachPoolAnswersTheCallerThatQueuedOnIt();
  void anErrorFromWorkReachesTheErrorCallback();
  void theTwoTaggedErrorsReachTheirHandlerAndLaterTasksStillRun();
  void aCancelledDeliveryNeverReachesTheGuiThread();
  void workPostedFromAWorkerThreadIsDeliveredOnTheGuiThread();
  void aPostFromAWorkerThreadOutlivesTheTaskThatPostedIt();

  // ── cancellation ───────────────────────────────────────────────────────
  void cancelRetiresWorkThatHasNotStarted();
  void cancelStillAllowsAnExplicitDrainToFinish();
  void cancelIsIdempotentWhileWorkIsOutstanding();
  void cancelDuringWorkStopsLaterStagesOfTheSameTask();
  void anUncancelledInnerScopeDoesNotMaskACancelledOuterOne();

  // ── owner deletion (KB-06 / KB-11) ─────────────────────────────────────
  void deletingAnOwnerReturnsPromptlyWhileWorkIsStillRunning();
  void deletingAnOwnerNeverDeliversToTheDeadOwner();
  void deletingAnOwnerLeavesOtherOwnersAnswering();
  void aSecondOwnerSeesAResponsiveGuiThreadWhileTheFirstOneWorkIs();

  // ── shutdownAll (KB-11) ────────────────────────────────────────────────
  void shutdownAllKeepsTheGuiThreadAnsweringWhileWorkDrains();
  void shutdownAllRunsItsFinishCallbackOnceTheWorkIsReallyGone();
  void workCancelledBeforeShutdownNeverDelivers();
};

/// Standalone entry point for the mikai_tests runner; the class above carries
/// the suite so it can also be driven by QTest directly.
int runTestRunner(int argc, char** argv);
