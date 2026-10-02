#pragma once

#include <QtTest/QTest>

class TestNativePrepPlan : public QObject {
  Q_OBJECT

 private slots:
  void paddingMatchesTheReferenceMultipleOf64();
  void paddingIsCentredAndRoundedDownToEven();
  void paddingExtentIsTheTotalAdded();

  void aCadenceScalesToLowestTerms();
  void aCadenceOverflowIsRefusedNotWrapped();

  void aShapeStringIsTheEngineBuildOrder();
  void onlyAUniformPositiveIntegerIsAScale();

  void geometryIsCroppedThenRotated();
  void anAbsentRotationIsNotAConfirmedRotation();
  void aCropOutsideTheCodedFrameIsRefused();
  void aPortraitSourceStaysPortrait();

  void aChainWindowAdmitsItsOwnBounds();
  void aSourceOutsideItsWindowIsRefusedByName();

  void aSpatialChainResolvesToItsDeclaredOutput();
  void aTwoTimesRifeAfterSpatialDoublesTheCadence();
  void aTwoTimesRifeBeforeSpatialStillDoublesTheCadence();
  void aHoistedPreResizeRunsBeforeInterpolation();
  void aPreResizeAfterTemporalIsNotHoisted();
  void anOddPreResizeIsRefusedByName();
  void aRifeOnlyRecipeResolvesToOneTemporalStage();

  void aMissingModelRefusesThePlanByName();
  void anUnavailableBackendRefusesThePlanByName();
  void aModelOnTheWrongBackendIsRefused();
  void aModelOfTheWrongRoleIsRefused();
  void aDynamicStageWithoutAResolverIsRefused();
  void aDynamicStageUsesTheSuppliedResolver();
  void aResolverAnsweringTheWrongGeometryIsRefused();
  void aFixedGraphRunAtAnotherSizeIsRefused();
  void aTemporalCadenceOverflowRefusesThePlanByName();

  void anUnobservedGeometryStaysPreparing();
  void anUnobservedCadenceStaysPreparing();
  void anUnobservedDeviceStaysPreparing();
  void aPartiallyObservedDeviceStaysPreparing();
  void aResolvedPlanCarriesItsColourTagsUnchanged();
  void aResolvedPlanCarriesItsRealDeviceIdentity();
  void aNonMultipleOf32SourcePadsItsTemporalTensors();
  void a23976SourceInterpolatesTo48000Over1001();
  void aCroppedSourcePlansAtTheCroppedSize();
  void aTenBitPortraitSourcePlansAsPortraitWithoutTransposing();

  void identicalGeometrySharesOneStagePayload();
  void thePlanIdIsStableAndCoversTheInterpolation();

  void aNinetyDegreeRotationPlansAsPortraitAndRefusesALandscapeGraph();
  void aPortraitTemporalStagePadsThePortraitWidth();
  void aCroppedTemporalStagePadsTheCroppedSize();
  void aNonZeroOriginCropPlansAtTheCropSize();
  void aFullRangePcSourceResolvesUnchanged();
  void aTenBitPortraitSourceAt23976Interpolates();
  void anOddHeightSourcePadsAsymmetrically();
  void aTwoStageSpatialChainThreadsGeometryAndRecipeIndex();
  void anSdSourcePlansAndAnSdWindowRefusesBelowItsMinimum();
};
