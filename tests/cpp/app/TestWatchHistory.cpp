#include "TestWatchHistory.hpp"

#include <state/WatchHistory.hpp>

#include <QSettings>

using mikai::app::TitleWatchProgress;
using mikai::app::WatchHistory;

namespace {
constexpr auto kTestSettingsKey = "tests/cpp/watchHistory";
}  // namespace

void TestWatchHistory::init() {
  QSettings settings;
  settings.remove(QString::fromLatin1(kTestSettingsKey));
  settings.sync();
}

void TestWatchHistory::cleanup() {
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
