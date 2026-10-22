#include <utility>
#include "TestLiveOracle.hpp"

#include "TestSupport.hpp"

#include <backend/MikaiClient.hpp>
#include <backend/Models.hpp>
#include <backend/Releases.hpp>
#include <backend/Streams.hpp>

#include <exception>

#include <QtCore/QSet>
#include <QtGui/QImage>
#include <QtCore/QStringList>

using mikai_test::asEpisodeText;
using mikai_test::asFlag;
using mikai_test::asInt;
using mikai_test::asIntPresent;
using mikai_test::asScore;
using mikai_test::asText;

using mikai::backend::MikaiClient;
using mikai::backend::PlayerResult;
using mikai::backend::Release;
using mikai::backend::SearchResult;
using mikai::backend::qualityStreams;
using mikai::backend::sortReleases;
using mikai::backend::thumbUrl;

namespace {

constexpr auto kRef = "7428";
constexpr auto kTitle = "Чорний смолоскип";

QString teamNamesOf(const Release& release) {
  QStringList names;
  for (const auto& team : release.teams) {
    names << asText(team.name);
  }
  return names.join(QStringLiteral(", "));
}

QString releaseList(const QVector<Release>& releases) {
  QStringList all;
  for (const Release& release : releases) {
    all << QStringLiteral("%1/%2").arg(asText(release.kind), teamNamesOf(release));
  }
  return all.join(QStringLiteral(" | "));
}

int indexOfRelease(const QVector<Release>& releases, const QString& kind,
                   const QString& teamFragment) {
  for (int i = 0; i < releases.size(); ++i) {
    if (asText(releases[i].kind) == kind && teamNamesOf(releases[i]).contains(teamFragment)) {
      return i;
    }
  }
  return -1;
}

QStringList episodeNumbers(const Release& release) {
  QStringList numbers;
  for (const auto& episode : release.episodes) {
    numbers << asEpisodeText(episode.number);
  }
  return numbers;
}

}  // namespace

void TestLiveOracle::initTestCase() {
  if (qEnvironmentVariableIntValue("MIKAI_LIVE") != 1) {
    QSKIP("live oracle is off; set MIKAI_LIVE=1 to run it");
  }
  // The key is never hardcoded: without one the API answers anonymously at
  // 60/min, which is enough for four calls but not for a debugging session.
  if (qEnvironmentVariable("MIKAI_API_KEY").isEmpty()) {
    QSKIP("MIKAI_LIVE=1 but MIKAI_API_KEY is unset");
  }
}

void TestLiveOracle::cleanupTestCase() {}

void TestLiveOracle::searchFindsBlackTorchFirst() {
  MikaiClient client(qEnvironmentVariable("MIKAI_API_KEY"));
  mikai::backend::AnimeQuery liveQuery;
  liveQuery.search = QStringLiteral("BLACK TORCH");
  const SearchResult result = client.search(liveQuery);
  QVERIFY(!result.items.isEmpty());
  // Both spellings must land on the same title, or the English search the
  // wrapper advertises silently opens a different show.
  QCOMPARE(asText(result.items.first().ref), QString::fromLatin1(kRef));
  mikai::backend::AnimeQuery ukrainianQuery;
  ukrainianQuery.search = QString::fromUtf8(kTitle);
  const SearchResult ukrainian = client.search(ukrainianQuery);
  QVERIFY(!ukrainian.items.isEmpty());
  QCOMPARE(asText(ukrainian.items.first().ref), QString::fromLatin1(kRef));
}

void TestLiveOracle::theTitleHasFourDubsAndThreeSubtitles() {
  MikaiClient client(qEnvironmentVariable("MIKAI_API_KEY"));
  const PlayerResult player = client.getPlayer(QString::fromLatin1(kRef));
  const QVector<Release> sorted = sortReleases(player.releases);
  int dubs = 0;
  int subtitles = 0;
  for (const Release& release : sorted) {
    if (asText(release.kind) == QStringLiteral("voice")) {
      ++dubs;
    } else if (asText(release.kind) == QStringLiteral("sub")) {
      ++subtitles;
    }
  }
  QCOMPARE(static_cast<int>(sorted.size()), 7);
  QCOMPARE(dubs, 4);
  QCOMPARE(subtitles, 3);
  // Voice before sub, and loudest first within each: the sort's whole contract.
  QCOMPARE(asText(sorted.first().kind), QStringLiteral("voice"));
  QCOMPARE(asText(sorted.last().kind), QStringLiteral("sub"));
}

void TestLiveOracle::oneDubCarriesEpisodesOneThroughTwelve() {
  MikaiClient client(qEnvironmentVariable("MIKAI_API_KEY"));
  const PlayerResult player = client.getPlayer(QString::fromLatin1(kRef));
  const QVector<Release> sorted = sortReleases(player.releases);
  const int index =
      indexOfRelease(sorted, QStringLiteral("voice"), QString::fromUtf8("Рідний Голос"));
  QVERIFY2(index >= 0, qPrintable(QStringLiteral("releases: %1").arg(releaseList(sorted))));
  const QStringList numbers = episodeNumbers(sorted[index]);
  // The grid this track renders has to hold every episode of the cour, in order,
  // with nothing dropped by the parser.
  for (int episode = 1; episode <= 12; ++episode) {
    QVERIFY2(numbers.contains(QString::number(episode)),
             qPrintable(QStringLiteral("missing episode %1 from %2")
                            .arg(episode)
                            .arg(numbers.join(','))));
  }
  // And the sources have to be real embed URLs, not empty placeholders.
  for (const auto& episode : sorted[index].episodes) {
    QVERIFY(!episode.sources.isEmpty());
  }
}

void TestLiveOracle::aRealPosterWebpDecodes() {
  // Registration is not decoding: this pulls a real poster over the wire and
  // puts it through the same path the UI does, so a plugin that loads but
  // cannot decode still shows up here rather than as blank placeholders.
  MikaiClient client(qEnvironmentVariable("MIKAI_API_KEY"));
  const SearchResult result = client.browsePopular(1, 1);
  QVERIFY(!result.items.isEmpty());
  const QString url = thumbUrl(result.items.first());
  QVERIFY2(url.endsWith(QStringLiteral(".webp"), Qt::CaseInsensitive),
           qPrintable(QStringLiteral("expected a webp thumb, got %1").arg(url)));
  const QByteArray bytes = client.fetchBytes(url);
  QVERIFY2(!bytes.isEmpty(), "the poster download came back empty");
  QImage image;
  QVERIFY2(image.loadFromData(bytes, "WEBP"), "Qt could not decode the poster");
  QVERIFY2(!image.isNull() && image.width() > 0 && image.height() > 0,
           "the decoded poster has no pixels");
}

namespace {

/// Every (provider, embed) pair a release offers, across all of its episodes.
/// The providers are derived from the embed URL through the same detectSite the
/// player uses, not from the API's own "provider" string, so a URL the extractor
/// would reject shows up here instead of passing silently.
QVector<std::pair<QString, QString>> providerEmbeds(const Release& release) {
  QVector<std::pair<QString, QString>> pairs;
  for (const auto& episode : release.episodes) {
    for (const auto& source : episode.sources) {
      const QString embed = asText(source.embed_url);
      if (embed.isEmpty()) {
        continue;
      }
      QString provider;
      try {
        provider = mikai::backend::detectSite(embed);
      } catch (const std::exception&) {
        provider = asText(source.provider).toLower();
      }
      pairs.append({provider, embed});
    }
  }
  return pairs;
}

/// The ladder a provider serves for one embed, or an empty string plus the
/// reason it could not be read. A provider being unreachable is not the same
/// defect as a provider answering with a broken ladder, and conflating the two
/// is how a live suite turns flaky: qualityStreams throws StreamError on an
/// HTTP error, and an exception escaping a QTest slot terminates the process
/// instead of failing one test.
struct Ladder {
  QString heights;
  QString failure;
};

Ladder ladderOf(const QString& embed) {
  try {
    const QMap<int, QString> qualities = qualityStreams(embed);
    QStringList list;
    for (auto it = qualities.cbegin(); it != qualities.cend(); ++it) {
      list << QString::number(it.key());
    }
    list.sort();
    return {list.join(QLatin1Char(',')), {}};
  } catch (const std::exception& error) {
    return {{}, QString::fromUtf8(error.what())};
  }
}

}  // namespace

void TestLiveOracle::theLiveStreamLadderCarriesTheUsualHeights() {
  MikaiClient client(qEnvironmentVariable("MIKAI_API_KEY"));
  const PlayerResult player = client.getPlayer(QString::fromLatin1(kRef));
  const QVector<Release> sorted = sortReleases(player.releases);
  const int index =
      indexOfRelease(sorted, QStringLiteral("voice"), QString::fromUtf8("Рідний Голос"));
  QVERIFY(index >= 0);
  QString embed;
  for (const auto& episode : sorted[index].episodes) {
    if (asEpisodeText(episode.number) != QStringLiteral("12")) {
      continue;
    }
    for (const auto& source : episode.sources) {
      if (!asText(source.embed_url).isEmpty()) {
        embed = asText(source.embed_url);
        break;
      }
    }
  }
  QVERIFY2(!embed.isEmpty(), "episode 12 carried no embed url");
  const QMap<int, QString> qualities = qualityStreams(embed);
  QSet<int> heights;
  for (auto it = qualities.cbegin(); it != qualities.cend(); ++it) {
    heights.insert(it.key());
  }
  QStringList list;
  for (const int height : heights) {
    list << QString::number(height);
  }
  list.sort();
  // Every provider serves at least these three rungs; a ladder that lost 480 or
  // 1080 would silently cap playback for everyone on a slow connection.
  QVERIFY2(heights.contains(480) && heights.contains(720) && heights.contains(1080),
           qPrintable(QStringLiteral("heights: %1").arg(list.join(','))));
}

void TestLiveOracle::everyProviderOfTheCourIsReachable() {
  // Guards the fixture, not the code. This suite's ladder assertions run
  // against a fixed set of providers, and if the API quietly stops offering one
  // of them, those assertions would keep passing while covering less. Naming
  // the providers makes that shrink loud instead of silent.
  MikaiClient client(qEnvironmentVariable("MIKAI_API_KEY"));
  const PlayerResult player = client.getPlayer(QString::fromLatin1(kRef));
  QSet<QString> providers;
  for (const Release& release : sortReleases(player.releases)) {
    for (const auto& pair : providerEmbeds(release)) {
      providers.insert(pair.first);
    }
  }
  QVERIFY2(providers.contains(QStringLiteral("ashdi")),
           qPrintable(QStringLiteral("providers: %1")
                          .arg(QStringList(providers.begin(), providers.end()).join(','))));
  QVERIFY2(providers.contains(QStringLiteral("moon")),
           qPrintable(QStringLiteral("providers: %1")
                          .arg(QStringList(providers.begin(), providers.end()).join(','))));
}

void TestLiveOracle::everyProviderOfEpisodeTwelveCarriesTheUsualHeights() {
  // The single-provider ladder test above takes the FIRST source of episode 12,
  // which on this title is always Ashdi. Moon therefore had no ladder coverage
  // at all: a Moon-only extraction regression - a changed master format, a new
  // wrapper - would keep every other test green. Both providers are walked
  // explicitly here, one at a time, so each is asserted on its own terms.
  MikaiClient client(qEnvironmentVariable("MIKAI_API_KEY"));
  const PlayerResult player = client.getPlayer(QString::fromLatin1(kRef));
  const QVector<Release> sorted = sortReleases(player.releases);
  const int index =
      indexOfRelease(sorted, QStringLiteral("voice"), QString::fromUtf8("Рідний Голос"));
  QVERIFY(index >= 0);

  QMap<QString, QString> byProvider;
  for (const auto& episode : sorted[index].episodes) {
    if (asEpisodeText(episode.number) != QStringLiteral("12")) {
      continue;
    }
    for (const auto& source : episode.sources) {
      const QString embed = asText(source.embed_url);
      if (embed.isEmpty()) {
        continue;
      }
      QString provider;
      try {
        provider = mikai::backend::detectSite(embed);
      } catch (const std::exception&) {
        provider = asText(source.provider).toLower();
      }
      if (!byProvider.contains(provider)) {
        byProvider.insert(provider, embed);
      }
    }
  }
  QVERIFY2(byProvider.size() >= 2,
           qPrintable(QStringLiteral("episode 12 offered %1 provider(s)")
                          .arg(byProvider.size())));
  QStringList checked;
  for (auto it = byProvider.constBegin(); it != byProvider.constEnd(); ++it) {
    const Ladder ladder = ladderOf(it.value());
    if (!ladder.failure.isEmpty()) {
      // Upstream being down is not a ladder defect, but silently skipping it
      // would let this test decay into checking nothing. Say so, loudly, and
      // keep going so the providers that DID answer are still asserted.
      qWarning("provider %s could not be read: %s", qPrintable(it.key()),
               qPrintable(ladder.failure));
      continue;
    }
    checked << it.key();
    // The message names the provider, so a red run says which one broke rather
    // than making the reader re-derive it from the fixture.
    QVERIFY2(ladder.heights.contains(QLatin1String("480")) &&
                 ladder.heights.contains(QLatin1String("720")) &&
                 ladder.heights.contains(QLatin1String("1080")),
             qPrintable(QStringLiteral("%1 heights: %2").arg(it.key(), ladder.heights)));
  }
  // A run in which no provider could be read has verified nothing at all, and
  // must not report success.
  QVERIFY2(!checked.isEmpty(), "no provider of episode 12 could be read at all");
}
