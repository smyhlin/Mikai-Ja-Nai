#pragma once

#include <QtTest/QTest>

class TestNativePreparedPlan : public QObject {
  Q_OBJECT

 private slots:
  void aWholeSpatialPlanResolvesToTheAdmissionCommand();
  void cudaUuidPresentationDoesNotInvalidatePreparedArtifacts();
  void anAdmissionHoldsItsArtifactLeasesUntilDropped();

  void aTemporalStageCarriesItsOrderFactorAndPaddedShapes();
  void aWholeCombinedPlanAdmitsEveryOrderedStage();
  void aRefusedSecondTemporalStageAdmitsNothing();

  void aRequestWithoutARequestIdIsRefused();
  void anUnresolvedPlanIsRefused();
  void aSourceOrDeviceThatIsNotThePlannedOneIsRefused();
  void anUnobservedDeviceIsRefused();
  void aBuildIdentityThatIsNotThePlannedDeviceIsRefused();
  void aBuildIdentityWithoutARuntimeIsRefused();
  void aReorderedChainIsRefused();

  void aMissingArtifactIsRefused();
  void aMutatedEngineIsRefused();
  void aModelDigestThatNoLongerMatchesIsRefused();
  void aModelOutsideTheOwnedRootIsRefused();
  void aStaleArtifactManifestIsRefused();
  void aRefusedPartialAdmissionReleasesTheLeasesItHadTaken();
};
