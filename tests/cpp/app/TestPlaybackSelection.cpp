#include "TestPlaybackSelection.hpp"

#include "app/support/PlaybackSelection.hpp"

#include <QtTest/QTest>

namespace {

using namespace mikai::app::selection;
using mikai::backend::Episode;
using mikai::backend::Release;
using mikai::backend::Source;
using mikai::backend::Team;

Release makeRelease(const QString& id, const QString& teamSlug, const QString& teamName,
                    const QString& provider, const QStringList& episodes) {
  Release r;
  r.id = id;
  r.kind = QStringLiteral("voice");
  if (!teamSlug.isEmpty()) {
    r.teams.push_back(Team{1, teamSlug, teamName});
  }
  for (const QString& ep : episodes) {
    Episode e;
    e.number = ep;
    e.sources.push_back(Source{provider, QStringLiteral("https://example.test/") + ep});
    r.episodes.push_back(e);
  }
  return r;
}

}  // namespace

void TestPlaybackSelection::emptyReleasesReturnsInvalid() {
  const SelectionResult res = resolve(SelectionInput{});
  QVERIFY(!res.valid);
  QCOMPARE(res.reason, SelectionReason::None);
}

void TestPlaybackSelection::sharedLinkOverridesFavouriteAndDefault() {
  SelectionInput in;
  in.releases = {
      makeRelease(QStringLiteral("r1"), QStringLiteral("team-a"), QStringLiteral("Team A"),
                  QStringLiteral("moon"), {QStringLiteral("1"), QStringLiteral("2")}),
      makeRelease(QStringLiteral("r2"), QStringLiteral("team-b"), QStringLiteral("Team B"),
                  QStringLiteral("ashdi"), {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")}),
  };
  in.defaultProvider = QStringLiteral("moon");
  in.favouriteProvider = QStringLiteral("moon");
  in.favouriteTeamSlug = QStringLiteral("team-a");
  in.sharedProvider = QStringLiteral("ashdi");
  in.sharedTeamSlug = QStringLiteral("team-b");
  in.sharedEpisode = QStringLiteral("3");
  in.sharedTimeSec = 142;

  const SelectionResult res = resolve(in);
  QVERIFY(res.valid);
  QCOMPARE(res.reason, SelectionReason::SharedLink);
  QCOMPARE(res.releaseIndex, 1);
  QCOMPARE(res.provider, QStringLiteral("ashdi"));
  QCOMPARE(res.teamSlug, QStringLiteral("team-b"));
  QCOMPARE(res.episodeNumber, QStringLiteral("3"));
  QCOMPARE(res.startTimeSec, std::optional<int>(142));
}

void TestPlaybackSelection::favouriteTeamWinsOverDefaultProvider() {
  SelectionInput in;
  in.releases = {
      makeRelease(QStringLiteral("r1"), QStringLiteral("team-a"), QStringLiteral("Team A"),
                  QStringLiteral("moon"), {QStringLiteral("1"), QStringLiteral("2")}),
      makeRelease(QStringLiteral("r2"), QStringLiteral("team-b"), QStringLiteral("Team B"),
                  QStringLiteral("tortuga"), {QStringLiteral("1"), QStringLiteral("2")}),
  };
  in.defaultProvider = QStringLiteral("moon");
  in.favouriteProvider = QStringLiteral("tortuga");
  in.favouriteTeamSlug = QStringLiteral("team-b");

  const SelectionResult res = resolve(in);
  QVERIFY(res.valid);
  QCOMPARE(res.reason, SelectionReason::FavouriteTeam);
  QCOMPARE(res.releaseIndex, 1);
  QCOMPARE(res.provider, QStringLiteral("tortuga"));
  QCOMPARE(res.teamSlug, QStringLiteral("team-b"));
}

void TestPlaybackSelection::missingFavouriteProviderFallsBackCleanly() {
  SelectionInput in;
  in.releases = {
      makeRelease(QStringLiteral("r1"), QStringLiteral("team-a"), QStringLiteral("Team A"),
                  QStringLiteral("moon"), {QStringLiteral("1"), QStringLiteral("2")}),
  };
  in.defaultProvider = QStringLiteral("moon");
  in.favouriteProvider = QStringLiteral("vanished-provider");
  in.favouriteTeamSlug = QStringLiteral("vanished-team");

  const SelectionResult res = resolve(in);
  QVERIFY(res.valid);
  QCOMPARE(res.reason, SelectionReason::PreferredProvider);
  QCOMPARE(res.releaseIndex, 0);
  QCOMPARE(res.provider, QStringLiteral("moon"));
}

void TestPlaybackSelection::watchesAdvanceToNextNumericOrSpecialEpisode() {
  SelectionInput in;
  in.releases = {
      makeRelease(QStringLiteral("r1"), QStringLiteral("team-a"), QStringLiteral("Team A"),
                  QStringLiteral("moon"),
                  {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("2.5"), QStringLiteral("OVA")}),
  };
  in.defaultProvider = QStringLiteral("moon");
  in.currentEpisode = QStringLiteral("2");
  in.watchedEpisodes = {QStringLiteral("1"), QStringLiteral("2")};

  const SelectionResult res = resolve(in);
  QVERIFY(res.valid);
  QCOMPARE(res.episodeNumber, QStringLiteral("2.5"));
}

void TestPlaybackSelection::episodeOrderingHandlesDecimalsAndOva() {
  QVERIFY(compareEpisodeNumbers(QStringLiteral("2"), QStringLiteral("10")) < 0);
  QVERIFY(compareEpisodeNumbers(QStringLiteral("12"), QStringLiteral("12.5")) < 0);
  QVERIFY(compareEpisodeNumbers(QStringLiteral("12.5"), QStringLiteral("13")) < 0);
  QVERIFY(compareEpisodeNumbers(QStringLiteral("13"), QStringLiteral("OVA")) < 0);
}
