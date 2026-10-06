#pragma once

#include <QtTest/QTest>

/// The native preference block, the availability the request is evaluated
/// against, and the shared shortcut collision check. All pure and CPU-testable:
/// the settings group is isolated by the runner, so these assert the persisted
/// form, the per-key fallbacks and the named availability exactly.
class TestNativePrefs : public QObject {
  Q_OBJECT

 private slots:
  void init();
  void cleanup();

  void aNativeRequestRoundTripsThroughItsGroup();
  void theSavedBlockCarriesTheSchemaThisBuildWrites();
  void loadingAnEmptyGroupWritesNothingAndReadsDefaults();
  void eachCorruptKeyFallsBackAloneAndReportsTheReason();
  void anUnknownSchemaReadsAsOffAndLeavesTheStoredBlockAlone();
  void aSaveRefusesToOverwriteAnUnknownSchema();
  void anEmptyPatchSaveWritesNothing();
  void anUntouchedUnknownSchemaSurvivesAGeneralSave();
  void anUnknownRecipeIsKeptAndReportedUnavailable();
  void anOfferedRecipeOnAnUnavailableBackendStaysRequested();
  void anAvailableRecipeOnItsBackendIsRequestedButNeverActive();
  void aClearedToggleIsStoredAsExplicitlyUnbound();
  void aRequestedTickPersistsWhileUnavailableAndKeepsTheRuntimeChoice();
  void aNativeSaveLeavesForeignKeysAlone();
  void clearingNativePreferencesLeavesTheRuntimeChoiceAlone();
  void theModelsDirectoryIsValidatedAndCanonicalised();
  void anUnknownStoredModelsDirectoryIsIgnoredAndReported();
  void aMissingNativeRuntimeNamesTheRuntimeGap();
  void aLoadedNativeRuntimeWithoutConfirmedCapabilitiesNamesThatGap();
  void aRefusedNativeRequestKeepsItsNamedReason();
  void theAvailabilityProducerNeverClaimsAnEngine();
  void aUsableFactoryObservationPermitsOnlyItsObservedBackend();
  void anObservedFactoryRefusalKeepsItsOwnNamedReason();
  void aRecipeNeedingAnUnsupportedRoleStaysUnavailable();
  void aStandardModuleIgnoresAUsableFactoryObservation();

  void theSharedCheckRefusesReservedKeysInEveryForm();
  void theSharedCheckRefusesADuplicateAndNamesTheOwner();
  void aNativeClaimBlocksAPresetAndNamesTheNativeOwner();
  void anEmptySequenceIsRefusedAsReserved();
  void theComponentsPromptStartsUndismissedAndStaysDismissed();
};
