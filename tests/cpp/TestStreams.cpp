#include "TestStreams.hpp"

#include <backend/Streams.hpp>

#include <stdexcept>

#include <QtCore/QStringList>

using mikai::backend::QualityVariant;
using mikai::backend::StreamError;
using mikai::backend::detectSite;
using mikai::backend::kFetchRetries;
using mikai::backend::kTimeoutMs;
using mikai::backend::kUserAgent;
using mikai::backend::parseVariants;
using mikai::backend::qualityUrl;
using mikai::backend::tortugaDecode;

namespace {

// The master playlist the test_units suite used, with one variant whose URL is
// relative so urljoin-style resolution is exercised.
QString masterPlaylist() {
  return QStringLiteral(
      "#EXTM3U\n"
      "#EXT-X-STREAM-INF:RESOLUTION=1920x1080,BANDWIDTH=2128000\n"
      "https://a.example/hls/1080/index.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=6211940,RESOLUTION=1280x720,CODECS=\"avc1.640029\"\n"
      "https://a.example/hls/720/index.m3u8\n"
      "#EXT-X-STREAM-INF:RESOLUTION=854x480,BANDWIDTH=500000\n"
      "https://a.example/hls/480/index.m3u8\n"
      "#EXT-X-STREAM-INF:RESOLUTION=640x360,BANDWIDTH=300000\n"
      "media_360.m3u8");
}

QString urlAtHeight(const QVector<QualityVariant>& variants, int height) {
  for (const QualityVariant& variant : variants) {
    if (variant.height == height) {
      return variant.url;
    }
  }
  return {};
}

}  // namespace

void TestStreams::detectsEveryKnownSite_data() {
  QTest::addColumn<QString>("pageUrl");
  QTest::addColumn<QString>("site");
  QTest::newRow("ashdi a") << QStringLiteral("https://ashdi.vip/vod/268104") << QStringLiteral("ashdi");
  QTest::newRow("ashdi b") << QStringLiteral("https://ashdi.vip/vod/113749") << QStringLiteral("ashdi");
  QTest::newRow("moon")
      << QStringLiteral("https://moonanime.art/iframe/n7p0wo428uufidanqjswp1u8/") << QStringLiteral("moon");
  QTest::newRow("tortuga") << QStringLiteral("https://tortuga.tw/vod/98249") << QStringLiteral("tortuga");
}

void TestStreams::detectsEveryKnownSite() {
  QFETCH(QString, pageUrl);
  QFETCH(QString, site);
  QCOMPARE(detectSite(pageUrl), site);
}

void TestStreams::rejectsAnUnsupportedPageUrl_data() {
  QTest::addColumn<QString>("pageUrl");
  // Anything not one of the three known hosts is refused rather than guessed at:
  // a wrong guess sends a request to a site whose markup we cannot read.
  QTest::newRow("unrelated") << QStringLiteral("https://example.com/episode");
  QTest::newRow("ashdi root") << QStringLiteral("https://ashdi.vip/");
  QTest::newRow("moon without iframe") << QStringLiteral("https://moonanime.art/watch/x");
  QTest::newRow("lookalike") << QStringLiteral("https://ashdi.vip.evil.example/vod/1");
  QTest::newRow("empty") << QString();
}

void TestStreams::rejectsAnUnsupportedPageUrl() {
  QFETCH(QString, pageUrl);
  // detect_site raises rather than guessing: a wrong guess would send a request
  // to a site whose markup cannot be read.
  bool raised = false;
  try {
    detectSite(pageUrl);
  } catch (const std::invalid_argument&) {
    raised = true;
  }
  QVERIFY2(raised, qPrintable(QStringLiteral("detectSite accepted %1").arg(pageUrl)));
}

void TestStreams::parsesVariantsInPlaylistOrder() {
  const QVector<QualityVariant> variants = parseVariants(masterPlaylist(),
                                                         QStringLiteral("https://a.example/hls/master.m3u8"));
  QCOMPARE(static_cast<int>(variants.size()), 4);
  QList<int> heights;
  for (const QualityVariant& variant : variants) {
    heights << variant.height;
  }
  QCOMPARE(heights, (QList<int>{1080, 720, 480, 360}));
}

void TestStreams::resolvesRelativeVariantUrlsAgainstTheMaster() {
  const QVector<QualityVariant> variants = parseVariants(masterPlaylist(),
                                                         QStringLiteral("https://a.example/hls/master.m3u8"));
  // "media_360.m3u8" sits beside the master, not at the site root.
  QCOMPARE(urlAtHeight(variants, 360), QStringLiteral("https://a.example/hls/media_360.m3u8"));
}

void TestStreams::keepsAnAbsoluteVariantUrlUntouched() {
  const QVector<QualityVariant> variants = parseVariants(masterPlaylist(),
                                                         QStringLiteral("https://a.example/hls/master.m3u8"));
  QCOMPARE(urlAtHeight(variants, 1080), QStringLiteral("https://a.example/hls/1080/index.m3u8"));
  QCOMPARE(urlAtHeight(variants, 720), QStringLiteral("https://a.example/hls/720/index.m3u8"));
}

void TestStreams::ignoresLinesThatAreNotVariants() {
  // A media playlist has no STREAM-INF lines; feeding one in must not invent
  // variants, or a dead provider would look like a playable quality ladder.
  const QVector<QualityVariant> variants = parseVariants(
      QStringLiteral("#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:6.0,\nseg1.ts\n#EXTINF:6.0,\nseg2.ts\n"),
      QStringLiteral("https://a.example/hls/media.m3u8"));
  QVERIFY(variants.isEmpty());
}

void TestStreams::aStreamInfWithoutAResolutionHasNoHeight() {
  const QVector<QualityVariant> variants = parseVariants(
      QStringLiteral("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=900000\nhttps://a.example/hls/index.m3u8\n"),
      QStringLiteral("https://a.example/hls/master.m3u8"));
  QCOMPARE(static_cast<int>(variants.size()), 1);
  // A variant with no RESOLUTION has no height, so a height-keyed quality map
  // must skip it rather than file it under 0.
  QCOMPARE(variants[0].height, 0);
}

void TestStreams::qualityUrlPrefersAnExactHeight() {
  const QVector<QualityVariant> variants = parseVariants(masterPlaylist(),
                                                         QStringLiteral("https://a.example/hls/master.m3u8"));
  QCOMPARE(qualityUrl(variants, 720), QStringLiteral("https://a.example/hls/720/index.m3u8"));
}

void TestStreams::qualityUrlFallsBackToTheNearestLowerHeight() {
  const QVector<QualityVariant> variants = parseVariants(masterPlaylist(),
                                                         QStringLiteral("https://a.example/hls/master.m3u8"));
  // 1000 is between 720 and 1080; the player downscales, it does not upscale.
  QCOMPARE(qualityUrl(variants, 1000), QStringLiteral("https://a.example/hls/720/index.m3u8"));
}

void TestStreams::qualityUrlClimbsToTheTallestWhenAskedBelowAllOfThem() {
  const QVector<QualityVariant> variants = parseVariants(masterPlaylist(),
                                                         QStringLiteral("https://a.example/hls/master.m3u8"));
  QCOMPARE(qualityUrl(variants, 240), QStringLiteral("https://a.example/hls/1080/index.m3u8"));
}

void TestStreams::qualityUrlPrefersTheFirstOfTwoEqualHeights() {
  // A provider that publishes the same height twice is malformed but happens;
  // picking the first is the deterministic answer a re-probe must reproduce.
  QVector<QualityVariant> variants{QualityVariant{720, QStringLiteral("https://a.example/first.m3u8")},
                                   QualityVariant{720, QStringLiteral("https://a.example/second.m3u8")}};
  QCOMPARE(qualityUrl(variants, 720), QStringLiteral("https://a.example/first.m3u8"));
}

void TestStreams::qualityUrlRejectsAnEmptyVariantList() {
  // A master with no variants is a failed extraction, not a playable stream.
  bool raised = false;
  try {
    qualityUrl(QVector<QualityVariant>{}, 720);
  } catch (const StreamError&) {
    raised = true;
  }
  QVERIFY(raised);
}

void TestStreams::decodesATortugaToken() {
  const QString token = QStringLiteral(
      "dOn8++bunoSd2qGrt6WvjMSFl41yeHN6DF1HGFYpP3wpBBoGFxH3pPb4zs3B25Kur6KwtsmexMot"
      "bXF5dEpCHVckOyc/cxdbQxywtqD08cLE3t/esaz6tpSGr87GNzgqNUlEXBlUKi83IU4KXQBE==");
  QCOMPARE(tortugaDecode(token),
           QStringLiteral("https://calypso.tortuga.tw/hls/serials/danjon.meshi/s01/"
                          "danjon.meshi.s01e01.amanogawa.mvo_98249/hls/index.m3u8"));
}

void TestStreams::rejectsATortugaTokenThatIsNotBase64() {
  // A page whose token was truncated by a proxy must surface as a StreamError
  // the player can report, never as a garbage URL handed to mpv.
  for (const QString& broken : {QStringLiteral("@@@@"), QString()}) {
    bool raised = false;
    try {
      tortugaDecode(broken);
    } catch (const StreamError&) {
      raised = true;
    }
    QVERIFY2(raised, qPrintable(QStringLiteral("accepted token %1").arg(broken)));
  }
}

void TestStreams::theTimeoutIsTwentySeconds() {
  QCOMPARE(kTimeoutMs, 20000);
}

void TestStreams::theFetchRetriesTwice() {
  // Three attempts in total: the first plus kFetchRetries retries, which is
  // what carries a title through a provider's 502 blip.
  QCOMPARE(kFetchRetries, 2);
  QVERIFY(!kUserAgent.isEmpty());
}
