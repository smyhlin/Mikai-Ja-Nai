// The native branch's pure contract: which rows exist, which are choosable,
// what the current mark follows, and what the status line is allowed to claim.
//
// Everything here is consumer-visible behaviour. No test asserts a wording, a
// signal, or that a function was called: it asserts a boundary - Off stays
// choosable, an unavailable provider or model is disabled and says why, the
// current mark survives a reorder, an eligible request is still not Active
// until the filter says so, and a geometry the filter did not publish is not
// rendered as zero.

#pragma once

#include <QtTest/QTest>

class TestNativeMenu : public QObject {
  Q_OBJECT

 private slots:
  void offStaysSelectableWhenEverythingElseIsUnavailable();
  void unavailableProvidersAndModelsAreDisabledWithAReason();
  void unavailableModelsAreDisabledEvenWhenTheEngineIsReady();
  void emptyCataloguePresentsNoInventedModels();
  void currentMarkFollowsStableIdAcrossReorder();
  void offRowKeepsTheStoredRequestUntouched();
  void unknownRequestStaysVisibleAsTheCurrentUnavailableChoice();
  void offeredRecipeIsNotEchoedAsAnUnknownRow();
  void requestOffInventsNoUnknownRowOrCurrentOn();
  void effectiveOnlyWhenTheSnapshotSaysActive();
  void rootRowsOwnsNativeAndRifeLevels();
  void rifeRootStaysAvailableWithNoConfiguredRecipe();
  void usableRifeRecipeIsChoosableWithItsExactId();
  void unavailableRifeRecipeStaysDisabledWithItsOwnReason();
  void rifeVariantsNeverCrossRouteAndDuplicatesStaySelectable();
  void rifeCurrentMarkSurvivesReordering();
  void statusStaysHiddenForDefaultOff();
  void standardRuntimeHidesPrivateRequestStatus();
  void nativeRuntimeShowsUnobservedRequestHonestly();
  void statusRendersOnlyWhatTheSnapshotStates();
  void snapshotWithoutGeometryNeverRendersZero();
};
