#include "TestQualityCache.hpp"

#include <state/QualityCache.hpp>

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QDateTime>
#include <QtCore/QSettings>

using mikai::app::QualityCache;
using mikai::app::kQualityCacheMaxEntries;
using mikai::app::kQualityCacheTtlMs;

namespace {

constexpr auto kTestSettingsKey = "tests/cpp/qualityCache";

// Every stamp in these tests hangs off the current clock, so an entry is always
// "fresh" unless the test says otherwise. A fixed epoch would have aged out
// against a 30-minute TTL before the first assertion ran.
qint64 stamp(qint64 offsetMs = 0) {
  return QDateTime::currentMSecsSinceEpoch() + offsetMs;
}

QMap<int, QString> qualities(int height, const QString& url) {
  QMap<int, QString> map;
  map.insert(height, url);
  return map;
}

QString urlFor(int index) {
  return QStringLiteral("https://s.example/%1/index.m3u8").arg(index);
}

QString embedFor(int index) {
  return QStringLiteral("https://ashdi.vip/vod/%1").arg(index);
}

// qInstallMessageHandler takes a plain function pointer, so a capturing
// lambda cannot collect warnings; a file-scope sink pointer can.
QStringList* g_capturedQualityWarnings = nullptr;

void captureQualityWarnings(QtMsgType, const QMessageLogContext&, const QString& message) {
  if (g_capturedQualityWarnings != nullptr) {
    g_capturedQualityWarnings->append(message);
  }
}

}  // namespace

void TestQualityCache::init() {
  QSettings settings;
  settings.remove(QString::fromLatin1(kTestSettingsKey));
  settings.sync();
}

void TestQualityCache::cleanup() {
  QSettings settings;
  settings.remove(QString::fromLatin1(kTestSettingsKey));
  settings.sync();
}

void TestQualityCache::remembersAndReadsBackAQualityMap() {
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  const QString embed = QStringLiteral("https://ashdi.vip/vod/268104");
  cache.remember(embed, qualities(720, urlFor(720)));
  const QMap<int, QString> stored = cache.qualities(embed);
  QCOMPARE(static_cast<int>(stored.size()), 1);
  QCOMPARE(stored.value(720), urlFor(720));
  QCOMPARE(cache.size(), 1);
}

void TestQualityCache::anUnknownEmbedHasNoQualities() {
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  // A miss is an empty map, not a null, so the caller can iterate it directly.
  const QMap<int, QString> stored = cache.qualities(QStringLiteral("https://ashdi.vip/vod/0"));
  QVERIFY(stored.isEmpty());
}

void TestQualityCache::anEntryOlderThanTheTtlIsTreatedAsMissing() {
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  const QString embed = QStringLiteral("https://ashdi.vip/vod/268104");
  // Signed URLs expire; replaying a half-hour-old entry would hand the player a
  // link the CDN has already revoked, so it has to read as absent.
  cache.remember(embed, qualities(720, urlFor(720)), stamp(-kQualityCacheTtlMs - 1));
  QVERIFY(cache.qualities(embed).isEmpty());
}

void TestQualityCache::anExpiredEntryIsPrunedRatherThanFlushed() {
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  cache.remember(QStringLiteral("https://ashdi.vip/vod/268104"), qualities(720, urlFor(720)),
                 stamp(-kQualityCacheTtlMs - 1));
  // A stale entry must not be written back out, or the file grows with URLs
  // that can never work again.
  cache.flushNow();
  QualityCache reloaded(QString::fromLatin1(kTestSettingsKey));
  reloaded.load();
  QVERIFY(reloaded.qualities(QStringLiteral("https://ashdi.vip/vod/268104")).isEmpty());
  QCOMPARE(reloaded.size(), 0);
}

void TestQualityCache::anEntryJustInsideTheTtlSurvives() {
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  const QString embed = QStringLiteral("https://ashdi.vip/vod/268104");
  // One minute short of the TTL is still usable; an off-by-one that expired
  // early would re-extract on every reopen.
  cache.remember(embed, qualities(720, urlFor(720)), stamp(-kQualityCacheTtlMs + 60'000));
  QCOMPARE(cache.qualities(embed).value(720), urlFor(720));
}

void TestQualityCache::anEntryWithAnEmptyMapIsNotRemembered() {
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  const QString embed = QStringLiteral("https://moonanime.art/iframe/dead/");
  cache.remember(embed, {});
  // An empty map carries no usable URL, so storing it would only grow the file.
  QVERIFY(cache.qualities(embed).isEmpty());
  QCOMPARE(cache.size(), 0);
}

void TestQualityCache::anEntryWithoutAnEmbedIsNotRemembered() {
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  cache.remember(QString(), qualities(720, urlFor(720)));
  QCOMPARE(cache.size(), 0);
}

void TestQualityCache::evictsTheOldestBeyondTheEntryCap() {
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  const int total = kQualityCacheMaxEntries + 5;
  for (int i = 0; i < total; ++i) {
    cache.remember(embedFor(i), qualities(720, urlFor(i)), stamp(i));
  }
  QCOMPARE(cache.size(), kQualityCacheMaxEntries);
  // The five oldest went; the newest survived. An unbounded cache grows the
  // settings blob until every load is slow.
  QVERIFY(cache.qualities(embedFor(0)).isEmpty());
  QVERIFY(cache.qualities(embedFor(4)).isEmpty());
  QCOMPARE(cache.qualities(embedFor(5)).value(720), urlFor(5));
  QCOMPARE(cache.qualities(embedFor(total - 1)).value(720), urlFor(total - 1));
}

void TestQualityCache::staysAtTheCapAfterManyInserts() {
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  for (int i = 0; i < kQualityCacheMaxEntries * 2; ++i) {
    cache.remember(embedFor(i), qualities(720, urlFor(i)), stamp(i));
  }
  QCOMPARE(cache.size(), kQualityCacheMaxEntries);
}

void TestQualityCache::clearDropsEveryEntry() {
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  const QString embed = QStringLiteral("https://ashdi.vip/vod/268104");
  cache.remember(embed, qualities(720, urlFor(720)));
  cache.clear();
  QVERIFY(cache.qualities(embed).isEmpty());
  QCOMPARE(cache.size(), 0);
  // And it is gone from disk, not only from memory.
  QualityCache reloaded(QString::fromLatin1(kTestSettingsKey));
  reloaded.load();
  QCOMPARE(reloaded.size(), 0);
}

void TestQualityCache::freshEntriesSurviveAFlushAndReload() {
  const QString embed = QStringLiteral("https://ashdi.vip/vod/268104");
  {
    QualityCache cache(QString::fromLatin1(kTestSettingsKey));
    cache.load();
    cache.remember(embed, qualities(720, urlFor(720)));
    cache.flushNow();
  }
  QualityCache reloaded(QString::fromLatin1(kTestSettingsKey));
  reloaded.load();
  // Replaying an episode has to reuse the store instead of re-extracting a
  // signed URL that is still valid.
  QCOMPARE(reloaded.qualities(embed).value(720), urlFor(720));
  QCOMPARE(reloaded.size(), 1);
}

void TestQualityCache::loadingPrunesExpiredEntriesBeforePersisting() {
  const QString expired = embedFor(0);
  const QString fresh = embedFor(1);
  const auto record = [](qint64 fetchedAt, const QString& url) {
    return QJsonObject{
        {QStringLiteral("at"), fetchedAt / 1000.0},
        {QStringLiteral("heights"), QJsonObject{{QStringLiteral("720"), url}}}};
  };
  const QJsonObject entries{
      {expired, record(stamp(-kQualityCacheTtlMs - 1), urlFor(0))},
      {fresh, record(stamp(), urlFor(1))}};
  const QJsonObject root{{QStringLiteral("entries"), entries}};
  {
    QSettings settings;
    settings.setValue(QString::fromLatin1(kTestSettingsKey),
                      QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    settings.sync();
  }

  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  QVERIFY(cache.qualities(expired).isEmpty());
  QCOMPARE(cache.qualities(fresh).value(720), urlFor(1));
  cache.flushNow();

  QualityCache reloaded(QString::fromLatin1(kTestSettingsKey));
  reloaded.load();
  QCOMPARE(reloaded.size(), 1);
  QVERIFY(reloaded.qualities(expired).isEmpty());
  QCOMPARE(reloaded.qualities(fresh).value(720), urlFor(1));
}

void TestQualityCache::loadingEnforcesTheEntryCap() {
  QJsonObject entries;
  const int total = kQualityCacheMaxEntries + 5;
  for (int index = 0; index < total; ++index) {
    const QJsonObject record{
        {QStringLiteral("at"), stamp(index) / 1000.0},
        {QStringLiteral("heights"), QJsonObject{{QStringLiteral("720"), urlFor(index)}}}};
    entries.insert(embedFor(index), record);
  }
  const QJsonObject root{{QStringLiteral("entries"), entries}};
  {
    QSettings settings;
    settings.setValue(QString::fromLatin1(kTestSettingsKey),
                      QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    settings.sync();
  }

  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  QCOMPARE(cache.size(), kQualityCacheMaxEntries);
  QVERIFY(cache.qualities(embedFor(0)).isEmpty());
  QCOMPARE(cache.qualities(embedFor(total - 1)).value(720), urlFor(total - 1));
  cache.flushNow();

  QualityCache reloaded(QString::fromLatin1(kTestSettingsKey));
  reloaded.load();
  QCOMPARE(reloaded.size(), kQualityCacheMaxEntries);
}

void TestQualityCache::loadingDoesNotDiscardPendingChanges() {
  const QString embed = QStringLiteral("https://ashdi.vip/vod/268104");
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  cache.remember(embed, qualities(720, urlFor(720)));

  cache.load();
  cache.flushNow();

  QualityCache reloaded(QString::fromLatin1(kTestSettingsKey));
  reloaded.load();
  QCOMPARE(reloaded.qualities(embed).value(720), urlFor(720));
}

void TestQualityCache::noSourceIsRemembered() {
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  const QString embed = QStringLiteral("https://moonanime.art/iframe/dead/");
  cache.markNoSource(embed);
  QVERIFY(cache.isNoSource(embed));
  QVERIFY(!cache.isNoSource(QStringLiteral("https://moonanime.art/iframe/other/")));
  // A dead provider is a negative result, not a quality entry.
  QVERIFY(cache.qualities(embed).isEmpty());
  QCOMPARE(cache.size(), 0);
}

void TestQualityCache::noSourceIsNotWrittenToDisk() {
  const QString embed = QStringLiteral("https://moonanime.art/iframe/dead/");
  {
    QualityCache cache(QString::fromLatin1(kTestSettingsKey));
    cache.load();
    cache.markNoSource(embed);
    cache.flushNow();
  }
  QualityCache reloaded(QString::fromLatin1(kTestSettingsKey));
  reloaded.load();
  // The mark is page-lifetime on purpose: a provider that gains the episode
  // later must not stay blocked for the rest of the session by a file.
  QVERIFY(!reloaded.isNoSource(embed));
  QVERIFY(reloaded.qualities(embed).isEmpty());
  QCOMPARE(reloaded.size(), 0);
}

void TestQualityCache::forgetDropsBothTheEntryAndTheNegativeMark() {
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  const QString embed = QStringLiteral("https://ashdi.vip/vod/268104");
  cache.remember(embed, qualities(720, urlFor(720)));
  cache.markNoSource(embed);
  QCOMPARE(cache.size(), 1);
  QVERIFY(cache.isNoSource(embed));
  // The recovery path: a stream that failed to play is dropped from both the
  // quality store and the dead-provider list so the next click re-extracts.
  cache.forget(embed);
  QVERIFY(cache.qualities(embed).isEmpty());
  QVERIFY(!cache.isNoSource(embed));
  QCOMPARE(cache.size(), 0);
}

void TestQualityCache::forgetSurvivesAFlushAndReload() {
  const QString embed = QStringLiteral("https://ashdi.vip/vod/268104");
  {
    QualityCache cache(QString::fromLatin1(kTestSettingsKey));
    cache.load();
    cache.remember(embed, qualities(720, urlFor(720)));
    cache.flushNow();
  }
  {
    QualityCache cache(QString::fromLatin1(kTestSettingsKey));
    cache.load();
    cache.forget(embed);
    cache.flushNow();
  }
  QualityCache reloaded(QString::fromLatin1(kTestSettingsKey));
  reloaded.load();
  // A forgotten entry must not come back on the next launch.
  QVERIFY(reloaded.qualities(embed).isEmpty());
  QCOMPARE(reloaded.size(), 0);
}

void TestQualityCache::forgettingAnUnknownEmbedIsHarmless() {
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  cache.forget(QStringLiteral("https://ashdi.vip/vod/999999"));
  QCOMPARE(cache.size(), 0);
  QVERIFY(!cache.isNoSource(QStringLiteral("https://ashdi.vip/vod/999999")));
}

void TestQualityCache::theTtlIsThirtyMinutes() {
  QCOMPARE(kQualityCacheTtlMs, 1'800'000LL);
}

void TestQualityCache::theEntryCapIsTwoHundredAndForty() {
  QCOMPARE(kQualityCacheMaxEntries, 240);
}

void TestQualityCache::capsThreeHundredEntriesAtTwoHundredAndForty() {
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  for (int i = 0; i < 300; ++i) {
    cache.remember(embedFor(i), qualities(720, urlFor(i)), stamp(i));
  }
  QCOMPARE(cache.size(), kQualityCacheMaxEntries);
  // The sixty oldest went; the newest survived. Asserted by size and by
  // which entries remain, not by timing.
  for (int i = 0; i < 60; ++i) {
    QVERIFY(cache.qualities(embedFor(i)).isEmpty());
  }
  QCOMPARE(cache.qualities(embedFor(60)).value(720), urlFor(60));
  QCOMPARE(cache.qualities(embedFor(299)).value(720), urlFor(299));
}

void TestQualityCache::aCorruptBlobWarnsInsteadOfSilentlyDropping() {
  {
    QSettings settings;
    settings.setValue(QString::fromLatin1(kTestSettingsKey), QStringLiteral("{not json at all"));
    settings.sync();
  }
  QStringList warnings;
  g_capturedQualityWarnings = &warnings;
  const QtMessageHandler previous = qInstallMessageHandler(&captureQualityWarnings);
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  qInstallMessageHandler(previous);
  g_capturedQualityWarnings = nullptr;

  // load() returns without crashing and leaves an empty cache.
  QCOMPARE(cache.size(), 0);
  QVERIFY(cache.qualities(embedFor(0)).isEmpty());
  bool warned = false;
  for (const QString& warning : warnings) {
    if (warning.contains(QStringLiteral("quality cache"))) {
      warned = true;
    }
  }
  QVERIFY2(warned, "expected a qWarning naming the quality cache on a corrupt blob");
}

void TestQualityCache::fetchedAtRoundTripsToTheMillisecond() {
  const QString embed = QStringLiteral("https://ashdi.vip/vod/268104");
  // A stamp with a non-zero millisecond component: the old double-seconds
  // form could not have carried it exactly.
  const qint64 stampMs = (QDateTime::currentMSecsSinceEpoch() / 1000) * 1000 + 456;
  {
    QualityCache cache(QString::fromLatin1(kTestSettingsKey));
    cache.load();
    cache.remember(embed, qualities(720, urlFor(720)), stampMs);
    cache.flushNow();
  }
  // The persisted form itself must be integer milliseconds, not seconds.
  {
    QSettings settings;
    const QString raw = settings.value(QString::fromLatin1(kTestSettingsKey)).toString();
    const QJsonObject root = QJsonDocument::fromJson(raw.toUtf8()).object();
    const QJsonObject record =
        root.value(QStringLiteral("entries")).toObject().value(embed).toObject();
    const double atValue = record.value(QStringLiteral("at")).toDouble(-1.0);
    QVERIFY2(atValue >= 1e12, "expected fetchedAt persisted as milliseconds, not seconds");
    QCOMPARE(static_cast<qint64>(atValue), stampMs);
  }
  QualityCache reloaded(QString::fromLatin1(kTestSettingsKey));
  reloaded.load();
  QCOMPARE(reloaded.qualities(embed).value(720), urlFor(720));
  QCOMPARE(reloaded.size(), 1);
}

void TestQualityCache::anOldDoubleSecondsBlobStillLoads() {
  // One-release compat: an "at" below 1e12 is the old double-seconds form.
  const QString embed = QStringLiteral("https://ashdi.vip/vod/268104");
  const qint64 stampMs = (QDateTime::currentMSecsSinceEpoch() / 1000) * 1000 + 123;
  const QJsonObject record{
      {QStringLiteral("at"), stampMs / 1000.0},
      {QStringLiteral("heights"), QJsonObject{{QStringLiteral("720"), urlFor(720)}}}};
  const QJsonObject root{
      {QStringLiteral("entries"), QJsonObject{{embed, record}}}};
  {
    QSettings settings;
    settings.setValue(QString::fromLatin1(kTestSettingsKey),
                      QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    settings.sync();
  }
  QualityCache cache(QString::fromLatin1(kTestSettingsKey));
  cache.load();
  QCOMPARE(cache.qualities(embed).value(720), urlFor(720));
  QCOMPARE(cache.size(), 1);
}
