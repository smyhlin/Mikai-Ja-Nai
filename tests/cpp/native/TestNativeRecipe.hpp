#pragma once

#include <QtTest/QTest>

class TestNativeRecipe : public QObject {
  Q_OBJECT

 private slots:
  void aSpatialOnlyRecipeIsLegal();
  void aRifeOnlyRecipeIsLegalAndDistinctFromAnEmptySpatialChain();
  void aCombinedRecipeIsLegalWithBothOrders();
  void anEmptySpatialChainWithNoTemporalIsRejected();

  void aRecipeRoundTripsThroughItsDocumentUnchanged();
  void orderingOfSpatialStagesSurvivesTheRoundTrip();
  void anUnreducedFactorSurvivesTheRoundTripUnchanged();
  void allThreeShapesSurviveTheirOwnRoundTrip();

  void anUnknownSchemaIsRejectedNotGuessed();
  void anUnknownEnumValueIsRejectedNotDefaulted();
  void anUnknownMemberIsRejectedNotIgnored();
  void aTemporalStageWithoutAnOrderIsRejected();
  void aMalformedRationalIsRejected();
  void anOutOfRangeThresholdIsRejected();
  void aMalformedPreResizeIsRejected();
  void aTemporalModelContradictingItsOwnFieldsIsRejected();

  void aRecipeNamingAnAbsentModelIsUnavailableWithAReason();
  void aRecipeNamingAnUnavailableBackendIsUnavailableWithAReason();
  void aRecipeNamingAModelOnTheWrongBackendIsUnavailable();
  void readinessIsAllOrNothingAcrossTheWholeChain();
  void anAvailableRecipeReportsRequestedAndEffectiveSeparately();
  void aRefusedRecipeLeavesEffectiveOnTheOriginalPipeline();
  void aRifeOnlyRecipeStillNeedsItsTemporalModelInTheCatalogue();
  void anUnavailableModelRowIsRefusedWithItsOwnReason();
};