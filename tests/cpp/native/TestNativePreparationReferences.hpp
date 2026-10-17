#pragma once

#include <QtTest/QTest>

class TestNativePreparationReferences : public QObject {
  Q_OBJECT

 private slots:
  void aSpatialChainThreadsEachStagesOutputForward();
  void aNamedPreResizeIsTheOnlyResizeTheChainApplies();
  void interpolationBeforeSpatialConsumesTheCapturedPair();
  void interpolationAfterSpatialConsumesBothChainOutputs();
  void twoTemporalStagesAreRefused();
  void aTemporalPlanNeedsTwoActualFrames();
  void aBrokenGeometryChainIsRefused();
  void anUnresolvedPlanIsRefusedByName();
  void cropAndRotationApplyOnlyToUnprocessedFrames();
  void aNonModelledRotationIsRefused();
  void theElevenChannelInputCarriesTheOriginalSemantics();
  void theTemporalOutputIsCroppedBackToItsContentWindow();
  void theResamplerReproducesTheOriginalSpline36();
  void theFirstIntermediateTimestepIsTheOnlyOneBuilt();
  void anAbsentInspectorIsRefusedByName();
  void aCancelledRunSendsNoReferences();
};
