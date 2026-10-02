#pragma once

#include <QtTest/QTest>

/// The M35 cache surface's proof rules, against real temporary files. No engine
/// is built and no runtime is loaded: what is exercised is which directories are
/// cache entries at all, and which of them may be deleted.
class TestNativeAssets : public QObject {
  Q_OBJECT

 private slots:
  void onlyArtifactDirectoriesAreCacheEntries();
  void deletingAnUnknownCacheEntryTouchesNothing();
  void deletingAnUnprovenCacheEntryCarriesItsOwnReason();
  void anUnownedRootYieldsNoEntries();
  void cancellingBeforePublicationBlocksIt();
  void publicationHavingBegunRefusesCancellation();
};
