#pragma once

#include <QtTest/QTest>

class TestAnimeJanaiRuntime : public QObject {
  Q_OBJECT

 private slots:
  void missingInstallIsNotReady();
  void missingRequiredBackendDependencyIsNotReady();
  void backendMismatchIsNotReady();
  void sizeAndDigestCorruptionAreRejected();
  void badPeAndUnsafeRecordPathAreRejected();
  void wrongArchitectureIsRejected();
  void reparseDependencyIsRejected();
  void cancelledImportDoesNotPublish();
  void cancelledReplacementPreservesPriorInstall();
  void unsupportedDispatcherIsNotReady();
  void unknownModelAndDefaultPlanRemainPure();
};
