#pragma once

#include <QtTest/QTest>

/// The M35 cache surface's proof rules against real temporary files, plus the
/// public RGB-to-fp16 NCHW frame conversion behavior. No engine is built and no
/// widget is shown.
class TestNativeAssets : public QObject {
  Q_OBJECT

 private slots:
  void onlyArtifactDirectoriesAreCacheEntries();
  void deletingAnUnknownCacheEntryTouchesNothing();
  void deletingAnUnprovenCacheEntryCarriesItsOwnReason();
  void anUnownedRootYieldsNoEntries();
  void cancellingBeforePublicationBlocksIt();
  void publicationHavingBegunRefusesCancellation();
  void rgbFrameComponentsArePreservedInNchwFp16Planes();
};
