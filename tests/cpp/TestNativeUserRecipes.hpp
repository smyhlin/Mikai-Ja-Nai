#pragma once

#include <QtTest/QTest>

/// The user recipe store: identity from the recipe's own body rather than a
/// basename, the round-trip of every recipe shape through its owned documents,
/// and the refusals that keep a stored recipe honest. Fully CPU-testable
/// against explicit temporary roots; no engine, no ORT and no settings store is
/// involved.
class TestNativeUserRecipes : public QObject {
  Q_OBJECT

 private slots:
  void aSingleSpatialRecipeRoundTripsThroughItsOwnedDocument();
  void addingTheSameModelAndBackendAgainKeepsTheStoredRecipe();
  void twoFilesSharingABasenameStayTwoRecipes();
  void theSameContentOnTwoBackendsIsTwoExplicitRecipes();

  void aRifeOnlyRecipePersistsItsExactInterpolationSettings();
  void aCombinedRecipeKeepsItsOrderedPreResizeChain();
  void aTamperedStageIsRefusedAndNeverRead();

  void anIneligibleProfileIsRefusedAndNothingIsWritten();
  void aNonSpatialOrUnreadyRowIsRefusedWithItsOwnReason();
  void aModelIdThatDoesNotMatchItsOwnHashIsRefused();
  void aRecipeIdThisStoreNeverIssuedIsRefused();

  void aDocumentContradictingItsOwnNameIsSkippedAndRefused();
  void anUnownedStoreRootIsRefused();
};
