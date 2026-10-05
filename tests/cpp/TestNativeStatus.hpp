#pragma once

#include <QtTest/QTest>

class TestNativeStatus : public QObject {
  Q_OBJECT

 private slots:
  void theStateVocabularyIsExactAndRefusesAnythingElse();
  void aTagMapAndItsSnapshotMustAgreeOnSchemaProtocolAndRevision();
  void anUnknownSchemaOrProtocolIsRefusedWithBothVersionsNamed();
  void aMalformedSnapshotIsRefusedRatherThanPartiallyAccepted();
  void exactIntegersSurviveTheWireAndOversizedOnesAreRefused();
  void theOuterFlatMapMustStateItsOwnTyping();
  void aOneToOneSpatialChainStillCarriesRequestedAndEffectiveSeparately();
  void aCadenceOrGeometryThatContradictsTheStageShapeIsRefused();
  void absentRatesAreRefusedForActiveAndNeverSerialisedAsZero();
  void countersMustAgreeWithTheEffectivePlan();
  void interpolationCountersFollowTheActualRationalFactor();
  void aFailedRequestKeepsTheNewRequestAndTheOldEffectivePlan();
  void theReasonIsEmptyExactlyForActive();
  void anAbsentBlockIsNeverSynthesisedFromAnother();
  void aRevisionRegressionIsStaleWhileANewSessionIsAccepted();
  void aSensitiveUrlOrCredentialIsRefusedAndNeverExported();
  void detachingClearsTheViewSoNoPlanOutlivesItsCore();
  void aRefusedSnapshotHaltsThePollInsteadOfRepeatingIt();
  void aPreparingSnapshotIsPublishedAsPreparingAndNeverAsReady();
  void anEffectivePlanUnderAStateThatRunsNothingIsRefused();
  void anObservedProbeRoundTripsAndIsNeverPromotedToReady();
  void aMalformedProbeIdentityIsRefusedByName();
  void aContradictingProbeGeometryIsRefusedByName();
  void aSensitiveProbeTagIsRefusedWithoutEcho();
  void aUsableFactoryIsEarnedByAnObservedAnswerOnAMeasuredDevice();
  void aFactoryClaimsARoleOnlyWhenItsAnswerWasObservedAndRefusalFree();
  void anUnavailableFactoryNamesItsReasonAndKeepsTheRestOfTheSnapshot();
  void aFactoryStatesItsOwnIdentityAndTypesInsteadOfBeingConflated();
  void aFactoryObservationNeverMovesTheStateOrTheCounters();
  void aSensitiveFactoryValueIsRefusedRawAndWithheldFromTheExport();
  void aFactoryAbsentFromTheNewDocumentIsNotCarriedOverFromTheOldOne();
  void aFactoryDeviceThatDoesNotMatchTheProbedSourceIsNeverRewrittenIntoOne();

  void aCoreWithoutTheFilterReportsUnavailableWithAReason();
  void oneOutstandingRequestIsReissuedAtTheBoundedCadence();
  void aLateReplyForASupersededEpochIsDropped();
  void detachingBeforeTeardownCancelsTheOutstandingRequest();

 private:
  bool requireRuntime();
};
