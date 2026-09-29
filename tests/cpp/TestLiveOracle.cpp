#include "TestLiveOracle.hpp"

#include "TestSupport.hpp"

#include <backend/MikaiClient.hpp>
#include <backend/Models.hpp>
#include <backend/Releases.hpp>
#include <backend/Streams.hpp>

#include <QtCore/QSet>
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
  const SearchResult result = client.search(QStringLiteral("BLACK TORCH"));
  QVERIFY(!result.items.isEmpty());
  // Both spellings must land on the same title, or the English search the
  // wrapper advertises silently opens a different show.
  QCOMPARE(asText(result.items.first().ref), QString::fromLatin1(kRef));
  const SearchResult ukrainian = client.search(QString::fromUtf8(kTitle));
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
