#pragma once

#include <QtTest/QTest>

class TestNativePlaybackController : public QObject {
  Q_OBJECT

 private slots:
  void aRequestAloneIsNeverEffective();
  void anAcknowledgedWholePlanBecomesEffective();
  void anObservationFromASupersededEpochIsDropped();
  void aLateCompletionForASupersededTransitionIsDropped();
  void aRefusedReplacementKeepsTheRunningPlanAndItsClaims();
  void aPreparationBuildIsBlockedWhileAnAdmissionIsPending();
  void anAcknowledgedOffReleasesTheDeviceClaim();
  void aCancelledTransitionReleasesItsArtifactClaims();
  void aChangedSourceGeometryDropsThePendingAdmission();
  void aDeadlineWithoutAcknowledgementIsANamedNonActiveTransition();
  void aFailedFilterRefusesThePendingAdmission();
  void aLoadedPlanIsNotActivationUntilAnOutputCompletes();
  void aLockedPlanQueuesTheLatestRequestAndAppliesItOnUnlock();
  void aLockedPlanDropsTheQueuedRequestWhenTheSourceMoved();
  void aPlanLockIsRefusedWhileATransitionIsInFlight();
  void aDeferredAdmissionRefusedOnUnlockKeepsTheRunningPlan();
  void aTeardownNeverFakesAStopAcknowledgement();
  void aTeardownTimeoutRetainsTheCoreAndItsClaims();
  void aLockedRequestPublishesAQueuedReason();
  void aRemovalFromAnAttachedProbeIsTrackedUntilObserved();
  void aRemovalTimeoutKeepsTheClaimsAndIsNeverRemoved();
  void aPreparedStopIsRemovedWithoutBeingActive();
};
