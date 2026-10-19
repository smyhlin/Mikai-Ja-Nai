#pragma once

#include <QtTest/QTest>

class TestAnimeJanaiRuntime : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void init();
  void cleanup();
  void cleanupTestCase();
  void directRunRefusesExistingNativeTreeWithoutMutation();
  void missingInstallIsNotReady();
  void missingRequiredBackendDependencyIsNotReady();
  void storedFailedRequestReprobesAfterDependencyInstall();
  void backendMismatchIsNotReady();
  void sizeAndDigestCorruptionAreRejected();
  void badPeAndUnsafeRecordPathAreRejected();
  void wrongArchitectureIsRejected();
  void reparseDependencyIsRejected();
  void cancelledImportDoesNotPublish();
  void cancelledReplacementPreservesPriorInstall();
  void unsupportedDispatcherIsNotReady();
  void unknownModelAndDefaultPlanRemainPure();
  void customOnnxImportIsAtomicAndRemovable();
  void customModelsPrepareAndStagePinnedContent();
  void externalModelsRequireExplicitPinAndRepin();
  void malformedOnnxModelIsRejected();
  void managerConfigParsesAndSerializesMultiChainSlotsAndTrtSettings();
  void invalidConfigFilesReportErrorsWithoutReplacingStoredSettings();
  void mixedConfigStepsRoundTripAndRejectInvalidData();
  void mixedAnime4KPlanUsesSourceSelectionWithoutAji();
  void stageCacheLeasesRefuseOverlapAndReleaseAtTerminal();

 private:
  bool ownsFixture_ = false;
  bool ownsAnimeJanai_ = false;
};
