// The live oracle: one test against the real API, off by default.
//
// BLACK TORCH (ref 7428) is the fixture because its shape is known and stable —
// four dub tracks, three subtitle tracks, a 12-episode cour served by two
// different providers. A parser that loses a release or an episode shows up here
// as a count, not as a stack trace three screens later.
//
// Gated on MIKAI_LIVE=1 because it needs the network and spends API quota.

#pragma once

#include <QtTest/QTest>

class TestLiveOracle : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void cleanupTestCase();

  void searchFindsBlackTorchFirst();
  void theTitleHasFourDubsAndThreeSubtitles();
  void oneDubCarriesEpisodesOneThroughTwelve();
  void aRealPosterWebpDecodes();
  void theLiveStreamLadderCarriesTheUsualHeights();
};
