#pragma once

#include <QtTest/QTest>

class TestNativeBuilder : public QObject {
  Q_OBJECT

 private slots:
  void anUnobservedDecoderCannotStartPreparation();
  void cancellationPreventsPublication();
  void committedPublicationCannotBeReportedCancelled();
  void concurrentCancellationAndPublicationHaveOneWinner();
  void preparationExcludesOtherGpuWorkUntilItsMovedLeaseEnds();
  void everyInferenceOwnerMustReleaseBeforePreparation();
  void probeRequestParsesEveryObservedFact();
  void probeRequestRefusesAnIncompleteBuildIdentity();
  void probeRequestRefusesPlaceholderIdentity();
  void probeRequestRefusesRelativePaths();
  void probeRequestRequiresAnExplicitWorkspace();
  void probeRequestRejectsAnUnknownMember();
  void probeRequestRefusesTwoReferenceModes();
  void probeRequestAcceptsOptimizationLevelBounds();
  void probeRequestRefusesAnInvalidOptimizationLevel();
  void probeRequestRefusesAMissingOptimizationLevel();
  void probeRequestRefusesARepeatedOptimizationLevel();
  void aContiguousStridePastTheExtentBoundIsAccepted();
  void aNonContiguousStrideIsRefused();
  void aStridePastTheTensorIsRefused();
  void aStrideArrayThatIsNotRank4IsRefused();
};
