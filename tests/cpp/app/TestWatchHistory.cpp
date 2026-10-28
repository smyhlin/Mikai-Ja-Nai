#include "TestWatchHistory.hpp"

#include <state/WatchHistory.hpp>

#include <QSettings>

using mikai::app::TitleWatchProgress;
using mikai::app::WatchHistory;

namespace {
constexpr auto kTestSettingsKey = "tests/cpp/watchHistory";
}  // namespace

void TestWatchHistory::init() {
  WatchHistory::flushHistoryWrites();
  QSettings settings;
  settings.remove(QString::fromLatin1(kTestSettingsKey));
  settings.sync();
}

void TestWatchHistory::cleanup() {
  WatchHistory::flushHistoryWrites();
  QSettings settings;
  settings.remove(QString::fromLatin1(kTestSettingsKey));
  settings.sync();
}

void TestWatchHistory::persistsCurrentAndWatchedAcrossInstances() {
  {
    WatchHistory store(QString::fromLatin1(kTestSettingsKey));
    store.setCurrent(QStringLiteral("7428"), QStringLiteral("7"));
    store.setWatched(QStringLiteral("7428"), QStringLiteral("3"), true);
    store.setWatched(QStringLiteral("7428"), QStringLiteral("3"), true);
    store.setWatched(QStringLiteral("7428"), QStringLiteral("4"), true);
    store.setWatched(QStringLiteral("7428"), QStringLiteral("4"), false);
  }
  WatchHistory reloaded(QString::fromLatin1(kTestSettingsKey));
  const TitleWatchProgress progress = reloaded.progress(QStringLiteral("7428"));
  QCOMPARE(progress.current, QStringLiteral("7"));
  QCOMPARE(progress.watched, QStringList{QStringLiteral("3")});
}

void TestWatchHistory::isolatesTitlesFromEachOther() {
  WatchHistory store(QString::fromLatin1(kTestSettingsKey));
  store.setCurrent(QStringLiteral("100"), QStringLiteral("2"));
  store.setWatched(QStringLiteral("100"), QStringLiteral("2"), true);
  store.setCurrent(QStringLiteral("200"), QStringLiteral("9"));

  QCOMPARE(store.current(QStringLiteral("100")), QStringLiteral("2"));
  QVERIFY(store.isWatched(QStringLiteral("100"), QStringLiteral("2")));
  QCOMPARE(store.current(QStringLiteral("200")), QStringLiteral("9"));
  QVERIFY(!store.isWatched(QStringLiteral("200"), QStringLiteral("2")));
  QVERIFY(store.current(QStringLiteral("300")).isEmpty());
}

void TestWatchHistory::keepsSpecialIdentifiersDistinctFromNumbers() {
  WatchHistory store(QString::fromLatin1(kTestSettingsKey));
  store.setWatched(QStringLiteral("42"), QStringLiteral("2"), true);
  store.setWatched(QStringLiteral("42"), QStringLiteral("OVA 2"), true);
  store.setWatched(QStringLiteral("42"), QStringLiteral("2"), false);

  QVERIFY(!store.isWatched(QStringLiteral("42"), QStringLiteral("2")));
  QVERIFY(store.isWatched(QStringLiteral("42"), QStringLiteral("OVA 2")));
}

void TestWatchHistory::recordingCurrentLeavesWatchedAlone() {
  WatchHistory store(QString::fromLatin1(kTestSettingsKey));
  store.setWatched(QStringLiteral("7"), QStringLiteral("1"), true);
  store.setCurrent(QStringLiteral("7"), QStringLiteral("4"));

  QCOMPARE(store.current(QStringLiteral("7")), QStringLiteral("4"));
  QVERIFY(store.isWatched(QStringLiteral("7"), QStringLiteral("1")));
  QVERIFY(!store.isWatched(QStringLiteral("7"), QStringLiteral("4")));
}

void TestWatchHistory::markingWatchedKeepsTheCurrentEpisode() {
  WatchHistory store(QString::fromLatin1(kTestSettingsKey));
  store.setCurrent(QStringLiteral("7"), QStringLiteral("4"));
  store.setWatched(QStringLiteral("7"), QStringLiteral("4"), true);
  store.setWatched(QStringLiteral("7"), QStringLiteral("4"), false);

  QCOMPARE(store.current(QStringLiteral("7")), QStringLiteral("4"));
  QVERIFY(!store.isWatched(QStringLiteral("7"), QStringLiteral("4")));
}

void TestWatchHistory::interleavedWritersPreserveUnrelatedProgress() {
  WatchHistory first(QString::fromLatin1(kTestSettingsKey));
  WatchHistory second(QString::fromLatin1(kTestSettingsKey));
  first.setWatched(QStringLiteral("100"), QStringLiteral("1"), true);
  second.setCurrent(QStringLiteral("200"), QStringLiteral("2"));
  first.setWatched(QStringLiteral("100"), QStringLiteral("3"), true);
  second.setWatched(QStringLiteral("100"), QStringLiteral("1"), false);
  first.setCurrent(QStringLiteral("100"), QStringLiteral("5"));

  WatchHistory reloaded(QString::fromLatin1(kTestSettingsKey));
  QCOMPARE(reloaded.current(QStringLiteral("100")), QStringLiteral("5"));
  QVERIFY(!reloaded.isWatched(QStringLiteral("100"), QStringLiteral("1")));
  QVERIFY(reloaded.isWatched(QStringLiteral("100"), QStringLiteral("3")));
  QCOMPARE(reloaded.current(QStringLiteral("200")), QStringLiteral("2"));
}
void TestWatchHistory::batchMarkingPreservesUnrelatedProgressAndPersists() {
  WatchHistory first(QString::fromLatin1(kTestSettingsKey));
  first.setCurrent(QStringLiteral("100"), QStringLiteral("5"));
  first.setWatched(QStringLiteral("100"), QStringLiteral("5"), true);
  first.setWatched(QStringLiteral("100"), QStringLiteral("8"), true);
  first.setCurrent(QStringLiteral("200"), QStringLiteral("2"));
  first.setWatched(QStringLiteral("200"), QStringLiteral("OVA 1"), true);

  WatchHistory second(QString::fromLatin1(kTestSettingsKey));
  second.setEpisodesWatched(QStringLiteral("100"),
                            {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("2"),
                             QStringLiteral("OVA 1")},
                            true);
  second.setEpisodesWatched(QStringLiteral("100"),
                            {QStringLiteral("2"), QStringLiteral("OVA 1")}, false);

  WatchHistory reloaded(QString::fromLatin1(kTestSettingsKey));
  QCOMPARE(reloaded.current(QStringLiteral("100")), QStringLiteral("5"));
  QVERIFY(reloaded.isWatched(QStringLiteral("100"), QStringLiteral("1")));
  QVERIFY(reloaded.isWatched(QStringLiteral("100"), QStringLiteral("5")));
  QVERIFY(reloaded.isWatched(QStringLiteral("100"), QStringLiteral("8")));
  QVERIFY(!reloaded.isWatched(QStringLiteral("100"), QStringLiteral("2")));
  QVERIFY(!reloaded.isWatched(QStringLiteral("100"), QStringLiteral("OVA 1")));
  QCOMPARE(reloaded.current(QStringLiteral("200")), QStringLiteral("2"));
  QVERIFY(reloaded.isWatched(QStringLiteral("200"), QStringLiteral("OVA 1")));
}

void TestWatchHistory::coalescedUpdatesFromSeparateInstancesKeepTheLatestMergedState() {
  WatchHistory first(QString::fromLatin1(kTestSettingsKey));
  WatchHistory second(QString::fromLatin1(kTestSettingsKey));
  const QString ref = QStringLiteral("7428");
  first.setCurrent(ref, QStringLiteral("start"));
  QStringList episodes;
  for (int index = 0; index < 40; ++index) {
    const QString episode = QStringLiteral("episode-%1").arg(index);
    episodes.append(episode);
    second.setWatched(ref, episode, true);
    first.setCurrent(ref, episode);
  }

  WatchHistory::flushHistoryWrites();
  WatchHistory reloaded(QString::fromLatin1(kTestSettingsKey));
  QCOMPARE(reloaded.current(ref), QStringLiteral("episode-39"));
  for (const QString& episode : episodes) {
    QVERIFY2(reloaded.isWatched(ref, episode), qPrintable(episode));
  }
}

void TestWatchHistory::writesAfterAFlushRemainDurable() {
  const QString ref = QStringLiteral("7428");
  WatchHistory store(QString::fromLatin1(kTestSettingsKey));
  store.setCurrent(ref, QStringLiteral("12"));
  WatchHistory::flushHistoryWrites();

  store.setWatched(ref, QStringLiteral("12"), true);
  WatchHistory::flushHistoryWrites();

  WatchHistory reloaded(QString::fromLatin1(kTestSettingsKey));
  QCOMPARE(reloaded.current(ref), QStringLiteral("12"));
  QVERIFY(reloaded.isWatched(ref, QStringLiteral("12")));
}

void TestWatchHistory::clearingHistoryRemovesThePersistedAndPendingSnapshot() {
  const QString ref = QStringLiteral("7428");
  WatchHistory store(QString::fromLatin1(kTestSettingsKey));
  store.setCurrent(ref, QStringLiteral("12"));
  store.setWatched(ref, QStringLiteral("12"), true);
  store.clear();
  WatchHistory::flushHistoryWrites();

  WatchHistory reloaded(QString::fromLatin1(kTestSettingsKey));
  QVERIFY(reloaded.current(ref).isEmpty());
  QVERIFY(!reloaded.isWatched(ref, QStringLiteral("12")));
}

void TestWatchHistory::anExternalSettingsRemovalIsNotShadowedByAnOldSnapshot() {
  const QString ref = QStringLiteral("7428");
  WatchHistory store(QString::fromLatin1(kTestSettingsKey));
  store.setCurrent(ref, QStringLiteral("12"));
  WatchHistory::flushHistoryWrites();

  QSettings settings;
  settings.remove(QString::fromLatin1(kTestSettingsKey));
  settings.sync();
  QCOMPARE(settings.status(), QSettings::NoError);

  WatchHistory reloaded(QString::fromLatin1(kTestSettingsKey));
  QVERIFY(reloaded.current(ref).isEmpty());
}

void TestWatchHistory::aCorruptedBlobReadsAsEmpty() {
  {
    QSettings settings;
    settings.setValue(QString::fromLatin1(kTestSettingsKey), QStringLiteral("{not json at all"));
    settings.sync();
  }
  WatchHistory store(QString::fromLatin1(kTestSettingsKey));
  QVERIFY(store.progress(QStringLiteral("7428")).watched.isEmpty());
  QVERIFY(store.current(QStringLiteral("7428")).isEmpty());

  store.setWatched(QStringLiteral("7428"), QStringLiteral("1"), true);
  WatchHistory reloaded(QString::fromLatin1(kTestSettingsKey));
  QVERIFY(reloaded.isWatched(QStringLiteral("7428"), QStringLiteral("1")));
}

void TestWatchHistory::storesAOffsetAndResumesFromIt() {
  {
    WatchHistory store(QString::fromLatin1(kTestSettingsKey));
    store.setOffset(QStringLiteral("500"), QStringLiteral("4"), 312.0, 1440.0);
    QCOMPARE(store.offset(QStringLiteral("500"), QStringLiteral("4")), 312.0);
    QCOMPARE(store.duration(QStringLiteral("500"), QStringLiteral("4")), 1440.0);
  }
  // A restart reads the offset back from the persisted document.
  WatchHistory reloaded(QString::fromLatin1(kTestSettingsKey));
  QCOMPARE(reloaded.offset(QStringLiteral("500"), QStringLiteral("4")), 312.0);
  QCOMPARE(reloaded.duration(QStringLiteral("500"), QStringLiteral("4")), 1440.0);
  QVERIFY(!reloaded.isWatched(QStringLiteral("500"), QStringLiteral("4")));

  // "Start over" drops the resume point but keeps the record of the episode.
  reloaded.clearOffset(QStringLiteral("500"), QStringLiteral("4"));
  QCOMPARE(reloaded.offset(QStringLiteral("500"), QStringLiteral("4")), 0.0);
  QCOMPARE(reloaded.duration(QStringLiteral("500"), QStringLiteral("4")), 0.0);
  WatchHistory afterClear(QString::fromLatin1(kTestSettingsKey));
  QCOMPARE(afterClear.offset(QStringLiteral("500"), QStringLiteral("4")), 0.0);
}

void TestWatchHistory::finishingAnEpisodeWatchedClearsItsOffset() {
  WatchHistory store(QString::fromLatin1(kTestSettingsKey));
  store.setOffset(QStringLiteral("600"), QStringLiteral("2"), 200.0, 1400.0);
  QCOMPARE(store.offset(QStringLiteral("600"), QStringLiteral("2")), 200.0);

  // Reaching the end marks the episode watched without any end-of-file event,
  // which is how a stream that simply stops used to lose the mark.
  store.setOffset(QStringLiteral("600"), QStringLiteral("2"), 1350.0, 1400.0);
  QVERIFY(store.isWatched(QStringLiteral("600"), QStringLiteral("2")));
  QCOMPARE(store.offset(QStringLiteral("600"), QStringLiteral("2")), 0.0);

  // Clearing the mark makes it resumable again from where it was stopped.
  store.setWatched(QStringLiteral("600"), QStringLiteral("2"), false);
  store.setOffset(QStringLiteral("600"), QStringLiteral("2"), 420.0, 1400.0);
  QCOMPARE(store.offset(QStringLiteral("600"), QStringLiteral("2")), 420.0);
}

void TestWatchHistory::aBarelyStartedEpisodeIsNotResumed() {
  WatchHistory store(QString::fromLatin1(kTestSettingsKey));
  store.setOffset(QStringLiteral("700"), QStringLiteral("1"), 12.0, 1440.0);
  QCOMPARE(store.offset(QStringLiteral("700"), QStringLiteral("1")), 0.0);
  // A live stream has no known duration; the offset still counts.
  store.setOffset(QStringLiteral("700"), QStringLiteral("2"), 600.0, 0.0);
  QCOMPARE(store.offset(QStringLiteral("700"), QStringLiteral("2")), 600.0);
}
