// The quality cache: signed HLS URLs, and the rules about how long they live.

#pragma once

#include <QtTest/QTest>

class TestQualityCache : public QObject {
  Q_OBJECT

 private slots:
  void init();
  void cleanup();

  void remembersAndReadsBackAQualityMap();
  void anUnknownEmbedHasNoQualities();
  void anEntryOlderThanTheTtlIsTreatedAsMissing();
  void anExpiredEntryIsPrunedRatherThanFlushed();
  void anEntryJustInsideTheTtlSurvives();
  void anEntryWithAnEmptyMapIsNotRemembered();
  void anEntryWithoutAnEmbedIsNotRemembered();
  void evictsTheOldestBeyondTheEntryCap();
  void staysAtTheCapAfterManyInserts();
  void clearDropsEveryEntry();
  void freshEntriesSurviveAFlushAndReload();

  void noSourceIsRemembered();
  void noSourceIsNotWrittenToDisk();

  void forgetDropsBothTheEntryAndTheNegativeMark();
  void forgetSurvivesAFlushAndReload();
  void forgettingAnUnknownEmbedIsHarmless();

  void theTtlIsThirtyMinutes();
  void theEntryCapIsTwoHundredAndForty();
};
