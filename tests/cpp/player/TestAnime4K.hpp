#pragma once

#include <QtTest/QTest>

class TestAnime4K : public QObject {
  Q_OBJECT

 private slots:
  void originalPresetChainsPreserveHistoricalOrder();
  void autoDownscalePreSitsBetweenTheUpscaleStages();
  void experimentalPresetsHaveExactCompleteChains();
  void presetIdsRemainUniqueAndRestorableAfterMenuReordering();
  void menuCategoriesExposeTheirPresetMembership();
  void rifeLevelLivesInNativeMenuNotCatalogue();
  void offAndUnknownIdsResolveNothing();
  void missingStageMakesTheEntireChainIncomplete();
  void individualShaderIdsResolveAndDownscaleStagesAreNotStandalone();
  void filterShaderProgramReportsOffAndUnknownCandidates();
  void filterShaderProgramRejectsIncompleteAndUnreadableSources();
};
