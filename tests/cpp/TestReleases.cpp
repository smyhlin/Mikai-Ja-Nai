#include "TestReleases.hpp"

#include "TestSupport.hpp"

#include <backend/Releases.hpp>

#include <QtCore/QStringList>

using mikai_test::asEpisodeText;
using mikai_test::asFlag;
using mikai_test::asInt;
using mikai_test::asIntPresent;
using mikai_test::asScore;
using mikai_test::asText;

using mikai::backend::Episode;
using mikai::backend::Release;
using mikai::backend::Source;
using mikai::backend::Team;
using mikai::backend::playableEpisodeCount;
using mikai::backend::sortReleases;


namespace {

Team team(const QString& name) {
  Team out;
  out.name = name;
  return out;
}

// A release built the way the player payload describes it: `count` episodes,
// each playable unless `playable` says how many lead nowhere.
Release makeRelease(const QString& kind, const QString& teamName, int count, int playable = -1) {
  Release release;
  release.kind = kind;
  release.teams = QVector<Team>{team(teamName)};
  release.episodes_count = count;
  if (playable >= 0) {
    for (int i = 0; i < count; ++i) {
      Episode episode;
      episode.number = i + 1;
      if (i < playable) {
        Source source;
        source.provider = QStringLiteral("ashdi");
        source.embed_url = QStringLiteral("https://ashdi.vip/vod/%1").arg(i + 1);
        episode.sources = QVector<Source>{source};
      }
      release.episodes.push_back(episode);
    }
  }
  return release;
}

QStringList teamNames(const QVector<Release>& releases) {
  QStringList names;
  for (const Release& release : releases) {
    names << (release.teams.isEmpty() ? QStringLiteral("?") : asText(release.teams[0].name));
  }
  return names;
}

}  // namespace

void TestReleases::playableCountUsesEpisodesCountWhenNoEpisodesListed() {
  // A release that lists no episodes still reports a count, and the count is the
  // only thing the selector can sort on.
  const Release release = makeRelease(QStringLiteral("voice"), QStringLiteral("Loud"), 24);
  QVERIFY(release.episodes.isEmpty());
  QCOMPARE(playableEpisodeCount(release), 24);
}

void TestReleases::playableCountCountsEpisodesWithASource() {
  const Release release = makeRelease(QStringLiteral("voice"), QStringLiteral("Loud"), 5, 3);
  QCOMPARE(playableEpisodeCount(release), 3);
}

void TestReleases::playableCountSkipsEpisodesWithEmptyEmbed() {
  // An episode that carries a provider but no URL cannot be opened, so counting
  // it would put a dub at the top of the list that dies on the first click.
  Release release;
  release.kind = QStringLiteral("voice");
  release.teams = QVector<Team>{team(QStringLiteral("Mid"))};
  Episode usable;
  usable.number = 1;
  Source good;
  good.provider = QStringLiteral("ashdi");
  good.embed_url = QStringLiteral("https://ashdi.vip/vod/1");
  usable.sources = QVector<Source>{good};
  Episode dead;
  dead.number = 2;
  Source empty;
  empty.provider = QStringLiteral("moon");
  empty.embed_url = QString();
  dead.sources = QVector<Source>{empty};
  Episode none;
  none.number = 3;
  release.episodes = QVector<Episode>{usable, dead, none};
  release.episodes_count = 99;
  QCOMPARE(playableEpisodeCount(release), 1);
}

void TestReleases::playableCountIsZeroForAnEmptyRelease() {
  QCOMPARE(playableEpisodeCount(Release{}), 0);
}

void TestReleases::sortPutsVoiceBeforeSub() {
  // A dub is the reason most people open a title at all; subtitles are the
  // fallback, so a subtitle track may never sit above a dub.
  QVector<Release> releases{makeRelease(QStringLiteral("sub"), QStringLiteral("SubTeam"), 6),
                            makeRelease(QStringLiteral("voice"), QStringLiteral("Loud"), 1)};
  const QVector<Release> sorted = sortReleases(releases);
  QCOMPARE(asText(sorted[0].kind), QStringLiteral("voice"));
  QCOMPARE(asText(sorted[1].kind), QStringLiteral("sub"));
}

void TestReleases::sortPutsLoudestDubFirstWithinVoice() {
  QVector<Release> releases{makeRelease(QStringLiteral("voice"), QStringLiteral("Quiet"), 2),
                            makeRelease(QStringLiteral("voice"), QStringLiteral("Loud"), 24),
                            makeRelease(QStringLiteral("voice"), QStringLiteral("Mid"), 4)};
  const QVector<Release> sorted = sortReleases(releases);
  QCOMPARE(teamNames(sorted), (QStringList{QStringLiteral("Loud"), QStringLiteral("Mid"),
                                           QStringLiteral("Quiet")}));
}

void TestReleases::sortPutsLoudestSubFirstWithinSub() {
  QVector<Release> releases{makeRelease(QStringLiteral("sub"), QStringLiteral("Few"), 3),
                            makeRelease(QStringLiteral("sub"), QStringLiteral("Many"), 9)};
  const QVector<Release> sorted = sortReleases(releases);
  QCOMPARE(teamNames(sorted), (QStringList{QStringLiteral("Many"), QStringLiteral("Few")}));
}

void TestReleases::sortTieBreaksByTeamName() {
  const QVector<Release> sorted = sortReleases({makeRelease(QStringLiteral("voice"), QStringLiteral("Zed"), 1),
                                               makeRelease(QStringLiteral("voice"), QStringLiteral("Amy"), 1)});
  QCOMPARE(teamNames(sorted), (QStringList{QStringLiteral("Amy"), QStringLiteral("Zed")}));
}

void TestReleases::sortPlacesUnknownKindAfterKnownOnes() {
  // A kind the app has never heard of must not be sorted into the dub column.
  QVector<Release> releases{makeRelease(QStringLiteral("mystery"), QStringLiteral("Weird"), 99),
                            makeRelease(QStringLiteral("sub"), QStringLiteral("Sub"), 1),
                            makeRelease(QStringLiteral("voice"), QStringLiteral("Dub"), 1)};
  const QVector<Release> sorted = sortReleases(releases);
  QCOMPARE(asText(sorted[0].kind), QStringLiteral("voice"));
  QCOMPARE(asText(sorted[1].kind), QStringLiteral("sub"));
  QCOMPARE(asText(sorted[2].kind), QStringLiteral("mystery"));
}

void TestReleases::sortIsStableForIdenticalReleases() {
  const QVector<Release> sorted = sortReleases({makeRelease(QStringLiteral("voice"), QStringLiteral("Same"), 3),
                                                makeRelease(QStringLiteral("voice"), QStringLiteral("Same"), 3)});
  QCOMPARE(static_cast<int>(sorted.size()), 2);
  QCOMPARE(asText(sorted[0].teams[0].name), asText(sorted[1].teams[0].name));
}

void TestReleases::sortDoesNotMutateTheInput() {
  QVector<Release> releases{makeRelease(QStringLiteral("sub"), QStringLiteral("SubTeam"), 6),
                            makeRelease(QStringLiteral("voice"), QStringLiteral("Loud"), 1)};
  const QString firstBefore = asText(releases[0].kind);
  const QString secondBefore = asText(releases[1].kind);
  sortReleases(releases);
  QCOMPARE(asText(releases[0].kind), firstBefore);
  QCOMPARE(asText(releases[1].kind), secondBefore);
}

void TestReleases::sortHandlesReleasesWithoutTeams() {
  // Both carry the same episode count, so the team name is the only tiebreak
  // left and the empty name sorts ahead of "A".
  const QVector<Release> sorted = sortReleases({makeRelease(QStringLiteral("voice"), QStringLiteral("A"), 1),
                                                makeRelease(QStringLiteral("voice"), QString(), 1)});
  QCOMPARE(static_cast<int>(sorted.size()), 2);
  // An empty team name sorts ahead of "A" rather than being pushed to the end:
  // a release whose team the API did not name must still have a stable place.
  QVERIFY(asText(sorted[0].teams[0].name).isEmpty());
  QCOMPARE(asText(sorted[1].teams[0].name), QStringLiteral("A"));
}
