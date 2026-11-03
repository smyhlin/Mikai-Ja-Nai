#pragma once

#include <QtTest/QTest>

/// One truth per component card.
///
/// The pure decisions are exercised as a table (no widgets): a card may only
/// show a percentage its transfer actually reached, 100 % needs a disk
/// inventory that confirms the terminal items, and failed, cancelled or
/// verifying states are never complete. The consumer tests then drive the real
/// ComponentStoreTab through a provisioner and a Runner to prove the widget
/// renders those decisions: an all-failed run after a full transfer shows no
/// full bar, deleting the store empties a stale bar on Refresh, verification is
/// not a download, and a generation bump mid-check releases every latch.
class TestComponentState : public QObject {
  Q_OBJECT

 private slots:
  void progressDecisionNeverFabricatesCompletion();
  void failedFullTransferNeverShowsAFullBar();
  void deletedFilesClearTheStaleBarOnRefresh();
  void staleModelItemIsClearedWhenUnrelatedModelsRemain();
  void initialInventorySurvivesClientInjection();
  void verificationTurnsAConfirmedBarIndeterminate();
  void verificationIsNotADownload();
  void generationBumpMidCheckReleasesEveryLatch();
};
