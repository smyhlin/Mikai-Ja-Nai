#pragma once

#include <QtTest/QTest>

class TestWatchHistory : public QObject {
  Q_OBJECT

 private slots:
  void init();
  void cleanup();
  void persistsCurrentAndWatchedAcrossInstances();
  void isolatesTitlesFromEachOther();
  void keepsSpecialIdentifiersDistinctFromNumbers();
  void recordingCurrentLeavesWatchedAlone();
  void markingWatchedKeepsTheCurrentEpisode();
  void interleavedWritersPreserveUnrelatedProgress();
  void batchMarkingPreservesUnrelatedProgressAndPersists();
  void coalescedUpdatesFromSeparateInstancesKeepTheLatestMergedState();
  void writesAfterAFlushRemainDurable();
  void anExternalSettingsRemovalIsNotShadowedByAnOldSnapshot();
  void clearingHistoryRemovesThePersistedAndPendingSnapshot();
  void aCorruptedBlobReadsAsEmpty();
  void storesAOffsetAndResumesFromIt();
  void finishingAnEpisodeWatchedClearsItsOffset();
  void aBarelyStartedEpisodeIsNotResumed();
};
