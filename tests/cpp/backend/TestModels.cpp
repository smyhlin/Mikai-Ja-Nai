#include "TestModels.hpp"

#include "TestSupport.hpp"

#include <backend/Models.hpp>

#include <QtCore/QJsonArray>
#include <QtGui/QImageReader>
#include <QtCore/QJsonObject>

using mikai_test::asEpisodeText;
using mikai_test::asFlag;
using mikai_test::asInt;
using mikai_test::asIntPresent;
using mikai_test::asScore;
using mikai_test::asText;

using mikai::backend::AnimeDetail;
using mikai::backend::AnimeSummary;
using mikai::backend::AnimeUpdate;
using mikai::backend::Episode;
using mikai::backend::PlayerResult;
using mikai::backend::Release;
using mikai::backend::Titles;
using mikai::backend::animePageUrl;
using mikai::backend::coverUrl;
using mikai::backend::displayName;
using mikai::backend::externalLinks;
using mikai::backend::episodeKindFor;
using mikai::backend::parseDetail;
using mikai::backend::parsePlayer;
using mikai::backend::parseSummary;
using mikai::backend::parseUpdate;
using mikai::backend::posterUrl;
using mikai::backend::thumbUrl;

namespace {

QJsonObject variant(const QString& webp, const QString& jpg) {
  QJsonObject out;
  if (!webp.isEmpty()) {
    out.insert(QStringLiteral("webp"), webp);
  }
  if (!jpg.isEmpty()) {
    out.insert(QStringLiteral("jpg"), jpg);
  }
  return out;
}

QJsonObject posters(const QJsonObject& big, const QJsonObject& medium, const QJsonObject& small) {
  QJsonObject out;
  out.insert(QStringLiteral("big"), big);
  out.insert(QStringLiteral("medium"), medium);
  out.insert(QStringLiteral("small"), small);
  return out;
}

QJsonObject emptyPosters() {
  return posters({}, {}, {});
}

QJsonObject summaryPayload(const QJsonObject& ids) {
  QJsonObject entry;
  entry.insert(QStringLiteral("ids"), ids);
  QJsonObject titles;
  titles.insert(QStringLiteral("ua"), QStringLiteral("Чорний смолоскип"));
  entry.insert(QStringLiteral("titles"), titles);
  entry.insert(QStringLiteral("year"), 2022);
  entry.insert(QStringLiteral("format"), QStringLiteral("TV"));
  entry.insert(QStringLiteral("status"), QStringLiteral("ongoing"));
  entry.insert(QStringLiteral("episodes"), 12);
  QJsonObject images;
  images.insert(QStringLiteral("poster"),
                posters(variant(QStringLiteral("big.webp"), QString()),
                        variant(QStringLiteral("med.webp"), QString()),
                        variant(QStringLiteral("small.webp"), QString())));
  entry.insert(QStringLiteral("images"), images);
  return entry;
}

QJsonObject withIds(const QString& key, const QJsonValue& value) {
  QJsonObject ids;
  ids.insert(key, value);
  return summaryPayload(ids);
}

}  // namespace

void TestModels::summaryRefPrefersMikaiId() {
  QJsonObject ids;
  ids.insert(QStringLiteral("mikai"), 7428);
  ids.insert(QStringLiteral("mal"), 21);
  ids.insert(QStringLiteral("al"), 101);
  const AnimeSummary summary = parseSummary(summaryPayload(ids));
  QCOMPARE(asText(summary.ref), QStringLiteral("7428"));
}

void TestModels::summaryRefFallsBackToPrefixedId_data() {
  QTest::addColumn<QString>("idKey");
  QTest::addColumn<QJsonValue>("idValue");
  QTest::addColumn<QString>("expected");
  // The fallback order is the API's own identity order: any other database's id
  // is usable as a ref, but only once the bare Mikai id is known missing.
  QTest::newRow("mal") << QStringLiteral("mal") << QJsonValue(21) << QStringLiteral("mal:21");
  QTest::newRow("al") << QStringLiteral("al") << QJsonValue(101) << QStringLiteral("al:101");
  QTest::newRow("hikka") << QStringLiteral("hikka") << QJsonValue(QStringLiteral("shikimori-9"))
                         << QStringLiteral("hikka:shikimori-9");
  QTest::newRow("slug") << QStringLiteral("slug") << QJsonValue(QStringLiteral("kurokami"))
                        << QStringLiteral("slug:kurokami");
}

void TestModels::summaryRefFallsBackToPrefixedId() {
  QFETCH(QString, idKey);
  QFETCH(QJsonValue, idValue);
  QFETCH(QString, expected);
  const AnimeSummary summary = parseSummary(withIds(idKey, idValue));
  QCOMPARE(asText(summary.ref), expected);
}

void TestModels::summaryRefIsEmptyWithoutAnyId() {
  const AnimeSummary summary = parseSummary(summaryPayload({}));
  QVERIFY(asText(summary.ref).isEmpty());
}

void TestModels::summaryRefStringifiesNumericIds() {
  // The API sends a bare integer here; a ref is always text, so a client that
  // forwarded the number would print "mal:21" as "mal:21" only by accident of
  // QString's coercion and would fail on a leading-zero slug instead.
  const AnimeSummary summary = parseSummary(withIds(QStringLiteral("mal"), QJsonValue(21)));
  QCOMPARE(asText(summary.ref), QStringLiteral("mal:21"));
  const AnimeSummary slug = parseSummary(withIds(QStringLiteral("slug"), QJsonValue(QStringLiteral("007"))));
  QCOMPARE(asText(slug.ref), QStringLiteral("slug:007"));
}

void TestModels::detailRefIsTheMikaiIdOnly() {
  QJsonObject ids;
  ids.insert(QStringLiteral("mikai"), 7428);
  ids.insert(QStringLiteral("mal"), 21);
  QJsonObject payload;
  payload.insert(QStringLiteral("ids"), ids);
  const AnimeDetail detail = parseDetail(payload);
  // Unlike a summary, a detail is addressed by its own id: a prefixed fallback
  // here would silently open a different title than the one that was clicked.
  QCOMPARE(asText(detail.ref), QStringLiteral("7428"));
}

void TestModels::displayNamePrefersUaThenEnglishThenOriginal_data() {
  QTest::addColumn<QString>("ua");
  QTest::addColumn<QString>("english");
  QTest::addColumn<QString>("original");
  QTest::addColumn<QString>("expected");
  QTest::newRow("ua wins") << QStringLiteral("Чорний смолоскип") << QStringLiteral("BLACK TORCH")
                          << QStringLiteral("ブラックトーチ") << QStringLiteral("Чорний смолоскип");
  QTest::newRow("english when no ua") << QString() << QStringLiteral("BLACK TORCH")
                                      << QStringLiteral("ブラックトーチ")
                                      << QStringLiteral("BLACK TORCH");
  QTest::newRow("original last") << QString() << QString() << QStringLiteral("ブラックトーチ")
                                 << QStringLiteral("ブラックトーチ");
}

void TestModels::displayNamePrefersUaThenEnglishThenOriginal() {
  QFETCH(QString, ua);
  QFETCH(QString, english);
  QFETCH(QString, original);
  QFETCH(QString, expected);
  Titles titles;
  titles.ua = ua;
  titles.english = english;
  titles.original = original;
  QCOMPARE(displayName(titles), expected);
}

void TestModels::displayNameFallsBackToQuestionMark() {
  Titles titles;
  QCOMPARE(displayName(titles), QStringLiteral("?"));
}

void TestModels::summaryThumbPrefersSmallWebp_data() {
  QTest::addColumn<QString>("webp");
  QTest::addColumn<QString>("jpg");
  QTest::addColumn<QString>("expected");
  // Webp is the smaller of the two siblings, so it wins whenever it is there.
  QTest::newRow("both") << QStringLiteral("small.webp") << QStringLiteral("small.jpg")
                        << QStringLiteral("small.webp");
  QTest::newRow("webp only") << QStringLiteral("small.webp") << QString() << QStringLiteral("small.webp");
  QTest::newRow("jpg only") << QString() << QStringLiteral("small.jpg") << QStringLiteral("small.jpg");
  QTest::newRow("neither") << QString() << QString() << QString();
}

void TestModels::summaryThumbPrefersSmallWebp() {
  QFETCH(QString, webp);
  QFETCH(QString, jpg);
  QFETCH(QString, expected);
  QJsonObject payload = withIds(QStringLiteral("mikai"), QJsonValue(7428));
  QJsonObject images = payload.value(QStringLiteral("images")).toObject();
  images.insert(QStringLiteral("poster"),
                posters(variant(QStringLiteral("big.webp"), QString()),
                        variant(QStringLiteral("med.webp"), QString()), variant(webp, jpg)));
  payload.insert(QStringLiteral("images"), images);
  QCOMPARE(thumbUrl(parseSummary(payload)), expected);
}

void TestModels::summaryPosterPrefersBigWebp_data() {
  QTest::addColumn<QString>("webp");
  QTest::addColumn<QString>("jpg");
  QTest::addColumn<QString>("expected");
  QTest::newRow("both") << QStringLiteral("big.webp") << QStringLiteral("big.jpg")
                        << QStringLiteral("big.webp");
  QTest::newRow("webp only") << QStringLiteral("big.webp") << QString() << QStringLiteral("big.webp");
  QTest::newRow("jpg only") << QString() << QStringLiteral("big.jpg") << QStringLiteral("big.jpg");
  QTest::newRow("neither") << QString() << QString() << QString();
}

void TestModels::summaryPosterPrefersBigWebp() {
  QFETCH(QString, webp);
  QFETCH(QString, jpg);
  QFETCH(QString, expected);
  QJsonObject payload = withIds(QStringLiteral("mikai"), QJsonValue(7428));
  QJsonObject images = payload.value(QStringLiteral("images")).toObject();
  images.insert(QStringLiteral("poster"),
                posters(variant(webp, jpg), variant(QStringLiteral("med.webp"), QString()),
                        variant(QStringLiteral("small.webp"), QString())));
  payload.insert(QStringLiteral("images"), images);
  QCOMPARE(posterUrl(parseSummary(payload)), expected);
}

void TestModels::emptySummaryCarriesNoArtwork() {
  const AnimeSummary summary = parseSummary({});
  QVERIFY(thumbUrl(summary).isEmpty());
  QVERIFY(posterUrl(summary).isEmpty());
  QCOMPARE(displayName(summary.titles), QStringLiteral("?"));
}

void TestModels::summaryCarriesPlannedEpisodesAndFormat() {
  const AnimeSummary summary =
      parseSummary(withIds(QStringLiteral("mikai"), QJsonValue(7428)));
  // The card shows "11 / 12 серій" from these two before the detail arrives.
  QVERIFY(asIntPresent(summary.episodes));
  QCOMPARE(asInt(summary.episodes), 12);
  QCOMPARE(asText(summary.format), QStringLiteral("TV"));
  QCOMPARE(asText(summary.status), QStringLiteral("ongoing"));
  QVERIFY(asIntPresent(summary.year));
  QCOMPARE(asInt(summary.year), 2022);
}

void TestModels::summaryWithoutPlannedCountLeavesItAbsent() {
  QJsonObject payload = withIds(QStringLiteral("mikai"), QJsonValue(1));
  payload.remove(QStringLiteral("episodes"));
  payload.remove(QStringLiteral("year"));
  const AnimeSummary summary = parseSummary(payload);
  // Absent, not zero: the card reads "planned count unknown" from a missing
  // value and would otherwise claim a show has zero episodes planned.
  QVERIFY(!asIntPresent(summary.episodes));
  QVERIFY(!asIntPresent(summary.year));
}

void TestModels::detailReadsEveryScalarField() {
  QJsonObject ids;
  ids.insert(QStringLiteral("mikai"), 7428);
  QJsonObject titles;
  titles.insert(QStringLiteral("ua"), QStringLiteral("Чорний смолоскип"));
  titles.insert(QStringLiteral("english"), QStringLiteral("BLACK TORCH"));
  QJsonObject payload;
  payload.insert(QStringLiteral("ids"), ids);
  payload.insert(QStringLiteral("titles"), titles);
  payload.insert(QStringLiteral("description"), QStringLiteral("Тьма йде по світу."));
  payload.insert(QStringLiteral("format"), QStringLiteral("TV"));
  payload.insert(QStringLiteral("status"), QStringLiteral("ongoing"));
  payload.insert(QStringLiteral("startDate"), QStringLiteral("2022-07-08"));
  payload.insert(QStringLiteral("source"), QStringLiteral("WEB"));
  payload.insert(QStringLiteral("studio"), QStringLiteral("Studio Deen"));
  payload.insert(QStringLiteral("country"), QStringLiteral("JP"));
  payload.insert(QStringLiteral("ageRating"), QStringLiteral("16+"));
  const AnimeDetail detail = parseDetail(payload);
  QCOMPARE(asText(detail.description), QStringLiteral("Тьма йде по світу."));
  QCOMPARE(asText(detail.format), QStringLiteral("TV"));
  QCOMPARE(asText(detail.status), QStringLiteral("ongoing"));
  QCOMPARE(asText(detail.start_date), QStringLiteral("2022-07-08"));
  QCOMPARE(asText(detail.source), QStringLiteral("WEB"));
  QCOMPARE(asText(detail.studio), QStringLiteral("Studio Deen"));
  QCOMPARE(asText(detail.country), QStringLiteral("JP"));
  QCOMPARE(asText(detail.age_rating), QStringLiteral("16+"));
  QCOMPARE(detail.ids.value(QStringLiteral("mikai")).toInt(), 7428);
}

void TestModels::detailReadsEpisodeStatsAndScores() {
  QJsonObject episodes;
  episodes.insert(QStringLiteral("total"), 12);
  episodes.insert(QStringLiteral("aired"), 11);
  episodes.insert(QStringLiteral("localized"), 9);
  episodes.insert(QStringLiteral("durationMin"), 24);
  QJsonObject scores;
  scores.insert(QStringLiteral("mikai"), 8.4);
  scores.insert(QStringLiteral("mikaiCount"), 512);
  scores.insert(QStringLiteral("mal"), 7.91);
  scores.insert(QStringLiteral("malCount"), 12000);
  QJsonObject payload;
  payload.insert(QStringLiteral("episodes"), episodes);
  payload.insert(QStringLiteral("scores"), scores);
  const AnimeDetail detail = parseDetail(payload);
  QCOMPARE(asInt(detail.episodes.total), 12);
  QCOMPARE(asInt(detail.episodes.aired), 11);
  QCOMPARE(asInt(detail.episodes.localized), 9);
  QCOMPARE(asInt(detail.episodes.duration_min), 24);
  QVERIFY(asFlag(detail.scores.mikai.has_value()));
  QCOMPARE(asScore(detail.scores.mikai), 8.4);
  QCOMPARE(asInt(detail.scores.mikai_count), 512);
  QVERIFY(asFlag(detail.scores.mal.has_value()));
  QCOMPARE(asScore(detail.scores.mal), 7.91);
  QCOMPARE(asInt(detail.scores.mal_count), 12000);
}

void TestModels::detailPreservesFractionalClassificationAndLicenseMetadata() {
  QJsonObject payload;
  payload.insert(QStringLiteral("episodeTypes"),
                 QJsonObject{{QStringLiteral("filler"), QJsonArray{QJsonObject{
                                  {QStringLiteral("from"), 12.5}, {QStringLiteral("to"), 13.0}}}},
                             {QStringLiteral("canon"), QJsonArray{QJsonObject{
                                  {QStringLiteral("from"), 1.0}, {QStringLiteral("to"), 12.0}}}}});
  payload.insert(QStringLiteral("nextEpisode"),
                 QJsonObject{{QStringLiteral("episode"), 13.5},
                             {QStringLiteral("airing"), QStringLiteral("2026-10-22T12:00:00Z")}});
  payload.insert(QStringLiteral("licensed"), true);
  payload.insert(QStringLiteral("license"),
                 QJsonObject{{QStringLiteral("holder"), QStringLiteral("Example Studio")},
                             {QStringLiteral("terms"), QJsonArray{QStringLiteral("streaming")}}});
  const AnimeDetail detail = parseDetail(payload);
  QVERIFY(detail.licensed);
  // The legal terms are kept whole: the API's licence shape is not specified,
  // so a field the client does not know about still has to reach the surface
  // that shows it.
  QCOMPARE(detail.license.value(QStringLiteral("holder")).toString(), QStringLiteral("Example Studio"));
  QCOMPARE(detail.license.value(QStringLiteral("terms")).toArray().size(), 1);
  QCOMPARE(detail.license.value(QStringLiteral("terms")).toArray().first().toString(),
           QStringLiteral("streaming"));
  // A fractional range is a range like any other: the numbers are kept exactly.
  QCOMPARE(detail.episode_types.value(QStringLiteral("filler")).size(), 1);
  QCOMPARE(detail.episode_types.value(QStringLiteral("filler")).first().from, 12.5);
  QCOMPARE(detail.episode_types.value(QStringLiteral("filler")).first().to, 13.0);
  // The upcoming broadcast, not the episode the viewer is watching.
  QVERIFY(detail.next_episode.has_value());
  QCOMPARE(detail.next_episode->episode.toDouble(), 13.5);
  QCOMPARE(detail.next_episode->airing, QStringLiteral("2026-10-22T12:00:00Z"));
}

void TestModels::episodeClassificationUsesKindsAndFractionalCanonicalRanges() {
  const AnimeDetail detail = parseDetail(QJsonObject{
      {QStringLiteral("episodeTypes"),
       QJsonObject{{QStringLiteral("canon"), QJsonArray{QJsonObject{
                        {QStringLiteral("from"), 1.0}, {QStringLiteral("to"), 2.0}}}},
                   {QStringLiteral("filler"), QJsonArray{QJsonObject{
                        {QStringLiteral("from"), 3.0}, {QStringLiteral("to"), 3.0}}}}}}});
  Episode episode;
  episode.number = 1.5;
  QCOMPARE(episodeKindFor(detail, episode), QStringLiteral("canon"));
  episode.number = QStringLiteral("1.5");
  QCOMPARE(episodeKindFor(detail, episode), QStringLiteral("canon"));
  episode.number = 2.0;
  QCOMPARE(episodeKindFor(detail, episode), QStringLiteral("canon"));
  episode.number = 3.5;
  QCOMPARE(episodeKindFor(detail, episode), QStringLiteral("filler"));
  episode.kind = QStringLiteral("future-kind");
  QCOMPARE(episodeKindFor(detail, episode), QStringLiteral("future-kind"));
  episode.kind.clear();
  episode.number = QStringLiteral("OVA");
  QVERIFY(episodeKindFor(detail, episode).isEmpty());
  episode.number = 0.0;
  QVERIFY(episodeKindFor(detail, episode).isEmpty());
}

void TestModels::detailKeepsUnknownAndZeroCountsDistinct() {
  QJsonObject episodes{{QStringLiteral("total"), 0},
                       {QStringLiteral("aired"), 0},
                       {QStringLiteral("localized"), 0}};
  const AnimeDetail detail = parseDetail(QJsonObject{{QStringLiteral("episodes"), episodes}});
  QCOMPARE(detail.episodes.total.value_or(-1), 0);
  QCOMPARE(detail.episodes.aired, 0);
  QCOMPARE(detail.episodes.localized, 0);
  const AnimeDetail missing = parseDetail({});
  QVERIFY(!missing.episodes.total.has_value());
}

void TestModels::detailWithoutScoresLeavesThemAbsent() {
  const AnimeDetail detail = parseDetail({});
  QVERIFY(!asFlag(detail.scores.mikai.has_value()));
  QVERIFY(!asFlag(detail.scores.mal.has_value()));
  QCOMPARE(asInt(detail.scores.mikai_count), 0);
}

void TestModels::detailReadsGenresAndTags() {
  QJsonObject genreUa;
  genreUa.insert(QStringLiteral("name"), QStringLiteral("Action"));
  genreUa.insert(QStringLiteral("ua"), QStringLiteral("Бойовик"));
  QJsonObject genreBare;
  genreBare.insert(QStringLiteral("name"), QStringLiteral("Slice of Life"));
  QJsonObject payload;
  payload.insert(QStringLiteral("genres"), QJsonArray{genreUa, genreBare});
  payload.insert(QStringLiteral("tags"), QJsonArray{genreUa});
  const AnimeDetail detail = parseDetail(payload);
  QCOMPARE(static_cast<int>(detail.genres.size()), 2);
  QCOMPARE(asText(detail.genres[0].name), QStringLiteral("Action"));
  QCOMPARE(asText(detail.genres[0].ua), QStringLiteral("Бойовик"));
  // A genre nobody translated still carries its English name rather than a gap.
  QCOMPARE(asText(detail.genres[1].name), QStringLiteral("Slice of Life"));
  QVERIFY(asText(detail.genres[1].ua).isEmpty());
  QCOMPARE(static_cast<int>(detail.tags.size()), 1);
  QCOMPARE(asText(detail.tags[0].ua), QStringLiteral("Бойовик"));
}

void TestModels::detailReadsUaPostersRelationsAndSimilar() {
  QJsonObject uaPoster;
  uaPoster.insert(QStringLiteral("images"),
                  posters(variant(QStringLiteral("ua-big.webp"), QString()),
                          variant(QString(), QStringLiteral("ua-med.jpg")), variant(QString(), QString())));
  QJsonObject relation;
  relation.insert(QStringLiteral("kind"), QStringLiteral("sequel"));
  relation.insert(QStringLiteral("anime"), withIds(QStringLiteral("mikai"), QJsonValue(9999)));
  QJsonObject payload;
  payload.insert(QStringLiteral("uaPosters"), QJsonArray{uaPoster});
  payload.insert(QStringLiteral("relations"), QJsonArray{relation});
  payload.insert(QStringLiteral("similar"), QJsonArray{withIds(QStringLiteral("mikai"), QJsonValue(8888))});
  const AnimeDetail detail = parseDetail(payload);
  QCOMPARE(static_cast<int>(detail.ua_posters.size()), 1);
  QCOMPARE(asText(detail.ua_posters[0].images.big.webp), QStringLiteral("ua-big.webp"));
  QCOMPARE(asText(detail.ua_posters[0].images.medium.jpg), QStringLiteral("ua-med.jpg"));
  QCOMPARE(static_cast<int>(detail.relations.size()), 1);
  QCOMPARE(asText(detail.relations[0].kind), QStringLiteral("sequel"));
  QCOMPARE(asText(detail.relations[0].anime.ref), QStringLiteral("9999"));
  QCOMPARE(static_cast<int>(detail.similar.size()), 1);
  QCOMPARE(asText(detail.similar[0].ref), QStringLiteral("8888"));
}

void TestModels::selectedUaPosterWinsCoverAndRetainsCredit() {
  const QJsonObject unselected{{QStringLiteral("id"), 11},
                               {QStringLiteral("isSelected"), false},
                               {QStringLiteral("author"), QStringLiteral("Old artist")},
                               {QStringLiteral("images"), QJsonObject{{QStringLiteral("big"),
                                   QJsonObject{{QStringLiteral("webp"), QStringLiteral("old.webp")}}}}}};
  const QJsonObject selected{{QStringLiteral("id"), 912},
                             {QStringLiteral("isSelected"), true},
                             {QStringLiteral("author"), QStringLiteral("Poster artist")},
                             {QStringLiteral("team"), QJsonObject{{QStringLiteral("id"), 7},
                                 {QStringLiteral("slug"), QStringLiteral("poster-team")},
                                 {QStringLiteral("name"), QStringLiteral("Poster Team")}}},
                             {QStringLiteral("images"), QJsonObject{{QStringLiteral("big"),
                                 QJsonObject{{QStringLiteral("webp"), QStringLiteral("selected.webp")}}}}}};
  const AnimeDetail detail =
      parseDetail(QJsonObject{{QStringLiteral("uaPosters"), QJsonArray{unselected, selected}}});

  // The cover is the poster the site selected, and the credit is that poster's
  // author: the two cannot disagree.
  QCOMPARE(coverUrl(detail), QStringLiteral("selected.webp"));
  const mikai::backend::UaPoster* credited = mikai::backend::coverPoster(detail);
  QVERIFY(credited != nullptr);
  QCOMPARE(credited->id, qint64(912));
  QCOMPARE(credited->author, QStringLiteral("Poster artist"));
  QVERIFY(credited->is_selected);
  QVERIFY(credited->team.has_value());
  QCOMPARE(credited->team->slug, QStringLiteral("poster-team"));
  QCOMPARE(credited->team->name, QStringLiteral("Poster Team"));
}

void TestModels::aSelectedPosterWithoutAnImageIsNotTheCoverAndIsNotCredited() {
  // The selected poster carries no picture, so it is not on screen. Crediting
  // its author would name someone for a picture nobody sees, and showing its
  // (empty) address would leave the cover blank.
  const QJsonObject selectedWithoutImage{{QStringLiteral("id"), 5},
                                         {QStringLiteral("isSelected"), true},
                                         {QStringLiteral("author"), QStringLiteral("Absent artist")}};
  const QJsonObject usable{{QStringLiteral("id"), 6},
                           {QStringLiteral("isSelected"), false},
                           {QStringLiteral("author"), QStringLiteral("Shown artist")},
                           {QStringLiteral("images"), QJsonObject{{QStringLiteral("big"),
                               QJsonObject{{QStringLiteral("webp"), QStringLiteral("shown.webp")}}}}}};
  const AnimeDetail detail = parseDetail(
      QJsonObject{{QStringLiteral("uaPosters"), QJsonArray{selectedWithoutImage, usable}}});

  // The picture on screen is the one that can be shown, and the credit names
  // the artist of that picture - not the one the selection pointed at.
  QCOMPARE(coverUrl(detail), QStringLiteral("shown.webp"));
  const mikai::backend::UaPoster* credited = mikai::backend::coverPoster(detail);
  QVERIFY(credited != nullptr);
  QCOMPARE(credited->author, QStringLiteral("Shown artist"));
  QVERIFY(!credited->is_selected);
}

void TestModels::theOriginalPosterIsCreditedToNobody() {
  // No UA poster at all: the picture on screen is the original one, which is
  // not a UA poster, so nobody is credited for it.
  const AnimeDetail detail = parseDetail(QJsonObject{
      {QStringLiteral("images"), QJsonObject{{QStringLiteral("poster"), QJsonObject{
          {QStringLiteral("big"), QJsonObject{{QStringLiteral("webp"), QStringLiteral("original.webp")}}}}}}}});
  QCOMPARE(coverUrl(detail), QStringLiteral("original.webp"));
  QVERIFY(mikai::backend::coverPoster(detail) == nullptr);
}

void TestModels::detailToleratesAMinimalPayload() {
  const AnimeDetail detail = parseDetail({});
  QVERIFY(asText(detail.ref).isEmpty());
  QVERIFY(asText(detail.description).isEmpty());
  QVERIFY(asText(detail.studio).isEmpty());
  QVERIFY(coverUrl(detail).isEmpty());
  QCOMPARE(static_cast<int>(detail.genres.size()), 0);
  QCOMPARE(static_cast<int>(detail.ua_posters.size()), 0);
  QVERIFY(externalLinks(detail).isEmpty());
}

void TestModels::coverUrlPrefersUaPosterOverApiPoster() {
  QJsonObject uaPoster;
  uaPoster.insert(QStringLiteral("images"),
                  posters(variant(QStringLiteral("img:ua"), QString()), {}, {}));
  QJsonObject payload;
  QJsonObject images;
  images.insert(QStringLiteral("poster"), posters(variant(QStringLiteral("img:poster"), QString()), {}, {}));
  payload.insert(QStringLiteral("images"), images);
  payload.insert(QStringLiteral("uaPosters"), QJsonArray{uaPoster});
  // A UA poster is the art this site published for the title; the generic poster
  // is the fallback, so the cover must not use it while a UA one exists.
  QCOMPARE(coverUrl(parseDetail(payload)), QStringLiteral("img:ua"));
}

void TestModels::coverUrlWalksUaPostersThenSizesInOrder_data() {
  QTest::addColumn<QJsonObject>("payload");
  QTest::addColumn<QString>("expected");

  const auto detailWith = [](const QJsonObject& apiPoster, const QJsonArray& uaPosters) {
    QJsonObject payload;
    QJsonObject images;
    images.insert(QStringLiteral("poster"), apiPoster);
    payload.insert(QStringLiteral("images"), images);
    payload.insert(QStringLiteral("uaPosters"), uaPosters);
    return payload;
  };
  const auto uaWith = [](const QJsonObject& set) {
    QJsonObject poster;
    poster.insert(QStringLiteral("images"), set);
    return QJsonArray{poster};
  };
  const auto poster = [](const QString& name) {
    QJsonObject object;
    object.insert(QStringLiteral("images"), posters(variant(name, QString()), {}, {}));
    return object;
  };

  QTest::newRow("ua big webp beats ua big jpg")
      << detailWith(emptyPosters(), uaWith(posters(variant(QStringLiteral("w"), QStringLiteral("j")), {}, {})))
      << QStringLiteral("w");
  QTest::newRow("ua big jpg when there is no webp")
      << detailWith(emptyPosters(), uaWith(posters(variant(QString(), QStringLiteral("j")), {}, {})))
      << QStringLiteral("j");
  QTest::newRow("ua medium beats api big")
      << detailWith(posters(variant(QStringLiteral("api"), QString()), {}, {}),
                    uaWith(posters({}, variant(QStringLiteral("ua-med"), QString()), {})))
      << QStringLiteral("ua-med");
  QTest::newRow("api big when the ua poster is blank")
      << detailWith(posters(variant(QStringLiteral("api"), QString()), {}, {}), uaWith(emptyPosters()))
      << QStringLiteral("api");
  QTest::newRow("api medium when api big is blank")
      << detailWith(posters({}, variant(QStringLiteral("api-med"), QString()), {}), QJsonArray{})
      << QStringLiteral("api-med");
  QTest::newRow("first ua poster wins over the second")
      << detailWith(emptyPosters(), QJsonArray{poster(QStringLiteral("one")), poster(QStringLiteral("two"))})
      << QStringLiteral("one");
}

void TestModels::coverUrlWalksUaPostersThenSizesInOrder() {
  QFETCH(QJsonObject, payload);
  QFETCH(QString, expected);
  QCOMPARE(coverUrl(parseDetail(payload)), expected);
}

void TestModels::coverUrlPrefersWebpOverJpg() {
  QJsonObject payload;
  QJsonObject images;
  images.insert(QStringLiteral("poster"),
                posters(variant(QStringLiteral("big.webp"), QStringLiteral("big.jpg")),
                        variant(QStringLiteral("med.webp"), QStringLiteral("med.jpg")),
                        variant(QStringLiteral("small.webp"), QStringLiteral("small.jpg"))));
  payload.insert(QStringLiteral("images"), images);
  QCOMPARE(coverUrl(parseDetail(payload)), QStringLiteral("big.webp"));
}

void TestModels::webpIsDecodable() {
  // The poster helpers hand Qt a .webp URL, and a base Qt install has no WebP
  // decoder: QPixmap then answers with a null image and no error at all, which
  // is how every poster in the app went blank. Assert the plugin is really
  // there, so a build or deploy that loses it fails here instead of on screen.
  const QList<QByteArray> formats = QImageReader::supportedImageFormats();
  QVERIFY2(formats.contains(QByteArrayLiteral("webp")),
           "the imageformats plugin is missing qwebp.dll: Qt cannot decode a "
           "webp poster and every image will be blank");
}

void TestModels::coverUrlFallsBackToApiPoster() {
  QJsonObject payload;
  QJsonObject images;
  images.insert(QStringLiteral("poster"),
                posters(variant(QStringLiteral("img:poster"), QString()), {}, {}));
  payload.insert(QStringLiteral("images"), images);
  QCOMPARE(coverUrl(parseDetail(payload)), QStringLiteral("img:poster"));
}

void TestModels::emptyDetailHasNoCover() {
  QVERIFY(coverUrl(AnimeDetail{}).isEmpty());
}

void TestModels::externalLinksCoverEveryKnownSite() {
  QJsonObject ids;
  ids.insert(QStringLiteral("mal"), 21);
  ids.insert(QStringLiteral("al"), 101);
  ids.insert(QStringLiteral("hikka"), QStringLiteral("shikimori-9"));
  ids.insert(QStringLiteral("imdb"), QStringLiteral("tt1234567"));
  QJsonObject payload;
  payload.insert(QStringLiteral("ids"), ids);
  const auto links = externalLinks(parseDetail(payload));
  QCOMPARE(static_cast<int>(links.size()), 4);
  QMap<QString, QString> byName;
  for (const auto& link : links) {
    byName.insert(link.first, link.second);
  }
  QCOMPARE(byName.value(QStringLiteral("MyAnimeList")), QStringLiteral("https://myanimelist.net/anime/21"));
  QCOMPARE(byName.value(QStringLiteral("AniList")), QStringLiteral("https://anilist.co/anime/101"));
  QCOMPARE(byName.value(QStringLiteral("Shikimori")), QStringLiteral("https://shikimori.one/animes/shikimori-9"));
  QCOMPARE(byName.value(QStringLiteral("IMDb")), QStringLiteral("https://www.imdb.com/title/tt1234567/"));
}

void TestModels::externalLinksSkipAbsentIds() {
  QJsonObject ids;
  ids.insert(QStringLiteral("mal"), 21);
  QJsonObject payload;
  payload.insert(QStringLiteral("ids"), ids);
  const auto links = externalLinks(parseDetail(payload));
  QCOMPARE(static_cast<int>(links.size()), 1);
  QCOMPARE(links.first().first, QStringLiteral("MyAnimeList"));
}

void TestModels::externalLinksFollowTheFixedOrder() {
  QJsonObject ids;
  // Declared in reverse: the order on screen is the site's, not the JSON's.
  ids.insert(QStringLiteral("imdb"), QStringLiteral("tt1"));
  ids.insert(QStringLiteral("hikka"), QStringLiteral("h1"));
  ids.insert(QStringLiteral("al"), 2);
  ids.insert(QStringLiteral("mal"), 3);
  QJsonObject payload;
  payload.insert(QStringLiteral("ids"), ids);
  QStringList names;
  for (const auto& link : externalLinks(parseDetail(payload))) {
    names << link.first;
  }
  QCOMPARE(names, (QStringList{QStringLiteral("MyAnimeList"), QStringLiteral("AniList"),
                               QStringLiteral("Shikimori"), QStringLiteral("IMDb")}));
}

void TestModels::updateReadsTeamsAndEpisode() {
  QJsonObject team;
  team.insert(QStringLiteral("id"), 4);
  team.insert(QStringLiteral("slug"), QStringLiteral("fanvoxua"));
  team.insert(QStringLiteral("name"), QStringLiteral("FanVoxUA"));
  QJsonObject entry;
  entry.insert(QStringLiteral("anime"), withIds(QStringLiteral("mikai"), QJsonValue(7428)));
  entry.insert(QStringLiteral("teams"), QJsonArray{team});
  entry.insert(QStringLiteral("teamName"), QStringLiteral("FanVoxUA"));
  entry.insert(QStringLiteral("isCollab"), true);
  entry.insert(QStringLiteral("kind"), QStringLiteral("voice"));
  entry.insert(QStringLiteral("provider"), QStringLiteral("ashdi"));
  entry.insert(QStringLiteral("episode"), 13);
  entry.insert(QStringLiteral("publishedAt"), QStringLiteral("2026-09-24T17:30:00Z"));
  const auto update = parseUpdate(entry);
  QCOMPARE(asText(update.anime.ref), QStringLiteral("7428"));
  QCOMPARE(static_cast<int>(update.teams.size()), 1);
  QCOMPARE(asText(update.teams[0].name), QStringLiteral("FanVoxUA"));
  QCOMPARE(asText(update.teams[0].slug), QStringLiteral("fanvoxua"));
  QCOMPARE(asInt(update.teams[0].id), 4);
  QCOMPARE(asText(update.team_name), QStringLiteral("FanVoxUA"));
  QVERIFY(asFlag(update.is_collab));
  QCOMPARE(asText(update.kind), QStringLiteral("voice"));
  QCOMPARE(asText(update.provider), QStringLiteral("ashdi"));
  QCOMPARE(asEpisodeText(update.episode), QStringLiteral("13"));
  QCOMPARE(asText(update.published_at), QStringLiteral("2026-09-24T17:30:00Z"));
}

void TestModels::updateRetainsEmbedUrlIdentity() {
  // One series hosted by two providers is two events, so the address it can be
  // watched at is part of the event's identity and has to survive the parse.
  const AnimeUpdate first = parseUpdate(QJsonObject{
      {QStringLiteral("provider"), QStringLiteral("provider-a")},
      {QStringLiteral("embedUrl"), QStringLiteral("https://video.example/embed/1")}});
  const AnimeUpdate second = parseUpdate(QJsonObject{
      {QStringLiteral("provider"), QStringLiteral("provider-b")},
      {QStringLiteral("embedUrl"), QStringLiteral("https://video.example/embed/2")}});
  QCOMPARE(first.provider, QStringLiteral("provider-a"));
  QCOMPARE(first.embed_url, QStringLiteral("https://video.example/embed/1"));
  QCOMPARE(second.provider, QStringLiteral("provider-b"));
  QCOMPARE(second.embed_url, QStringLiteral("https://video.example/embed/2"));
  QVERIFY(first.embed_url != second.embed_url);
}

void TestModels::updateWithoutTeamsIsStillUsable() {
  QJsonObject entry;
  entry.insert(QStringLiteral("anime"), withIds(QStringLiteral("mikai"), QJsonValue(1)));
  const auto update = parseUpdate(entry);
  QVERIFY(update.teams.isEmpty());
  QVERIFY(asText(update.team_name).isEmpty());
  QVERIFY(!asFlag(update.is_collab));
  QVERIFY(asEpisodeText(update.episode).isEmpty());
}

void TestModels::playerReadsReleasesEpisodesAndSources() {
  QJsonObject team;
  team.insert(QStringLiteral("id"), 7);
  team.insert(QStringLiteral("slug"), QStringLiteral("ridnyi-holos"));
  team.insert(QStringLiteral("name"), QStringLiteral("Рідний Голос"));
  QJsonObject source;
  source.insert(QStringLiteral("provider"), QStringLiteral("ashdi"));
  source.insert(QStringLiteral("embedUrl"), QStringLiteral("https://ashdi.vip/vod/268104"));
  QJsonObject episode;
  episode.insert(QStringLiteral("number"), 12);
  episode.insert(QStringLiteral("label"), QStringLiteral("12"));
  episode.insert(QStringLiteral("sources"), QJsonArray{source});
  QJsonObject release;
  release.insert(QStringLiteral("id"), QStringLiteral("r1"));
  release.insert(QStringLiteral("kind"), QStringLiteral("voice"));
  release.insert(QStringLiteral("isCollab"), false);
  release.insert(QStringLiteral("teams"), QJsonArray{team});
  release.insert(QStringLiteral("episodesCount"), 12);
  release.insert(QStringLiteral("episodes"), QJsonArray{episode});
  QJsonObject payload;
  payload.insert(QStringLiteral("licensed"), true);
  payload.insert(QStringLiteral("releases"), QJsonArray{release});
  const PlayerResult player = parsePlayer(payload);
  QVERIFY(asFlag(player.licensed));
  QCOMPARE(static_cast<int>(player.releases.size()), 1);
  const Release& parsed = player.releases.first();
  QCOMPARE(asText(parsed.id), QStringLiteral("r1"));
  QCOMPARE(asText(parsed.kind), QStringLiteral("voice"));
  QVERIFY(!asFlag(parsed.is_collab));
  QCOMPARE(static_cast<int>(parsed.teams.size()), 1);
  QCOMPARE(asText(parsed.teams[0].name), QStringLiteral("Рідний Голос"));
  QCOMPARE(asInt(parsed.episodes_count), 12);
  QCOMPARE(static_cast<int>(parsed.episodes.size()), 1);
  const Episode& first = parsed.episodes.first();
  QCOMPARE(asEpisodeText(first.number), QStringLiteral("12"));
  QCOMPARE(asText(first.label), QStringLiteral("12"));
  QCOMPARE(static_cast<int>(first.sources.size()), 1);
  QCOMPARE(asText(first.sources[0].provider), QStringLiteral("ashdi"));
  QCOMPARE(asText(first.sources[0].embed_url), QStringLiteral("https://ashdi.vip/vod/268104"));
}

void TestModels::playerPreservesFullLicenseObjectAndEpisodeKind() {
  const QJsonObject episode{{QStringLiteral("kind"), QStringLiteral("filler")},
                            {QStringLiteral("number"), 12.5}};
  const QJsonObject release{{QStringLiteral("episodes"), QJsonArray{episode}}};
  const PlayerResult player = parsePlayer(QJsonObject{
      {QStringLiteral("licensed"), true},
      {QStringLiteral("license"),
       QJsonObject{{QStringLiteral("licensee"), QStringLiteral("Studio")},
                   {QStringLiteral("territories"),
                    QJsonArray{QStringLiteral("JP"), QStringLiteral("UA")}}}},
      {QStringLiteral("releases"), QJsonArray{release}}});
  QVERIFY(player.licensed);
  // Every legal field the server sent survives, including the ones the client
  // has no name for: the licence is shown as the API described it.
  QCOMPARE(player.license.value(QStringLiteral("licensee")).toString(), QStringLiteral("Studio"));
  QCOMPARE(player.license.value(QStringLiteral("territories")).toArray().size(), 2);
  QCOMPARE(player.releases.first().episodes.first().kind, QStringLiteral("filler"));
  QCOMPARE(player.releases.first().episodes.first().number.toDouble(), 12.5);
}

void TestModels::playerToleratesMissingCollections() {
  const PlayerResult player = parsePlayer({});
  QVERIFY(!asFlag(player.licensed));
  QVERIFY(player.releases.isEmpty());
}

void TestModels::playerDefaultsProviderToQuestionMark() {
  QJsonObject source;
  source.insert(QStringLiteral("embedUrl"), QStringLiteral("https://ashdi.vip/vod/1"));
  QJsonObject episode;
  episode.insert(QStringLiteral("number"), 1);
  episode.insert(QStringLiteral("sources"), QJsonArray{source});
  QJsonObject release;
  release.insert(QStringLiteral("episodes"), QJsonArray{episode});
  QJsonObject payload;
  payload.insert(QStringLiteral("releases"), QJsonArray{release});
  const PlayerResult player = parsePlayer(payload);
  // A source with no provider is still a source; "?" keeps the episode playable
  // instead of dropping it on the way to the player.
  QCOMPARE(asText(player.releases[0].episodes[0].sources[0].provider), QStringLiteral("?"));
}

void TestModels::episodeNumberAcceptsEveryWireShape_data() {
  QTest::addColumn<QVariant>("number");
  QTest::addColumn<QString>("expected");
  QTest::newRow("int") << QVariant(12) << QStringLiteral("12");
  QTest::newRow("fractional special") << QVariant(12.5) << QStringLiteral("12.5");
  QTest::newRow("string") << QVariant(QStringLiteral("OVA")) << QStringLiteral("OVA");
  QTest::newRow("null") << QVariant() << QString();
}

void TestModels::episodeNumberAcceptsEveryWireShape() {
  QFETCH(QVariant, number);
  QFETCH(QString, expected);
  QJsonObject episode;
  episode.insert(QStringLiteral("number"), QJsonValue::fromVariant(number));
  QJsonObject release;
  release.insert(QStringLiteral("episodes"), QJsonArray{episode});
  QJsonObject payload;
  payload.insert(QStringLiteral("releases"), QJsonArray{release});
  const PlayerResult player = parsePlayer(payload);
  QCOMPARE(asEpisodeText(player.releases[0].episodes[0].number), expected);
}

void TestModels::animePageUrlAcceptsBareAndPrefixedRefs_data() {
  QTest::addColumn<QString>("ref");
  QTest::addColumn<QString>("expected");
  QTest::newRow("bare") << QStringLiteral("7428") << QStringLiteral("https://mikai.me/anime/7428");
  QTest::newRow("mal") << QStringLiteral("mal:21") << QStringLiteral("https://mikai.me/anime/mal:21");
  QTest::newRow("al") << QStringLiteral("al:101") << QStringLiteral("https://mikai.me/anime/al:101");
  QTest::newRow("slug")
      << QStringLiteral("slug:kurokami") << QStringLiteral("https://mikai.me/anime/slug:kurokami");
}

void TestModels::animePageUrlAcceptsBareAndPrefixedRefs() {
  QFETCH(QString, ref);
  QFETCH(QString, expected);
  QCOMPARE(animePageUrl(ref), expected);
}
