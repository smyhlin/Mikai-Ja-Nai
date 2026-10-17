#pragma once

#include <QtTest/QTest>

/// The three curated profile templates, their window rules, the resolved
/// one-stage recipe and the settings-selector leaves the availability producer
/// offers. Pure and CPU-testable: all of it is decided from the source's actual
/// geometry and frame rate and the catalogue the caller supplies.
class TestNativeProfiles : public QObject {
  Q_OBJECT

 private slots:
  void theLowWindowOverlapResolvesToHdFirst();
  void thePixelAndFrameRateBoundsAreInclusive();
  void aboveThirtyOneBalancedAndPerformanceRejectWhileQualityUsesItsThirdWindow();
  void qualityReachesSixtyOneFpsAndNoFurther();
  void aLowSourceResolvesSdAtANormalRateAndHdAtSixty();
  void anExactFractionalRateIsComparedWithoutRounding();
  void aMissingChosenModelIsRefusedWithoutSubstitution();
  void aRowWhoseIdDoesNotDeriveFromItsContentIsRefused();
  void anUnusableChosenModelIsRefused();
  void aBackendTheChosenModelDoesNotOfferIsRefused();
  void anUnknownTemplateAndAnUnknownSourceAreRefusedByName();
  void theResolvedRecipeIsOneStageWithNoResizeAndTheRequestedBackend();
};
