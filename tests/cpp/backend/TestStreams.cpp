#include "TestStreams.hpp"

#include <backend/Streams.hpp>
#include <backend/Cancellation.hpp>

#include <stdexcept>
#include <atomic>
#include <chrono>
#include <thread>

#include <QtCore/QByteArray>
#include <QtCore/QStringList>
#include <QtCore/QElapsedTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

using mikai::backend::QualityVariant;
using mikai::backend::StreamError;
using mikai::backend::detectSite;
using mikai::backend::isSafeStreamUrl;
using mikai::backend::kFetchRetries;
using mikai::backend::kTimeoutMs;
using mikai::backend::kUserAgent;
using mikai::backend::parseMoonSkipSegments;
using mikai::backend::parseMoonSourceList;
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

// MoonAnime's _0xd is a plain XOR against a key carried in the page script.
QByteArray moonObfuscate(const QByteArray& plain, const QByteArray& key) {
  QByteArray out(plain.size(), Qt::Uninitialized);
  for (int i = 0; i < plain.size(); ++i) {
    out[i] = static_cast<char>(static_cast<unsigned char>(plain.at(i)) ^
                               static_cast<unsigned char>(key.at(i % key.size())));
  }
  return out;
}

// The outer blob is seeded base64: byte 0 is the rolling seed, bytes 1..32 the
// key, the rest the ciphertext.
QString moonPageFromJavascript(const QString& javascript) {
  const QByteArray key = QByteArrayLiteral("moon-test-outer-key-32-bytes!!!!");
  const unsigned char seed = 0x5c;
  const QByteArray plain = javascript.toUtf8();
  QByteArray encrypted(plain.size(), Qt::Uninitialized);
  unsigned char rolling = seed;
  for (int i = 0; i < plain.size(); ++i) {
    const unsigned char kb = static_cast<unsigned char>(key.at(i % key.size()));
    const unsigned char e =
        static_cast<unsigned char>(plain.at(i)) ^ kb ^ rolling;
    encrypted[i] = static_cast<char>(e);
    rolling = static_cast<unsigned char>((e + kb) & 255);
  }
  QByteArray cipher;
  cipher.append(static_cast<char>(seed));
  cipher.append(key);
  cipher.append(encrypted);
  return QStringLiteral("<html><script>!function(){var c=atob(\"%1\");}();</script></html>")
      .arg(QString::fromLatin1(cipher.toBase64()));
}

QString moonIframeHtml(const QString& skipJson,
                       const QString& obfuscationKey = QStringLiteral("sTH7MjkXZiar"),
                       bool includeSkipBlock = true) {
  const QByteArray key = obfuscationKey.toUtf8();
  const QByteArray master = moonObfuscate(
      QByteArrayLiteral("https://cdn.example/moon/master.m3u8"), key);
  QString javascript = QStringLiteral(
                           "function _0xd(e){var k=\"%1\";var b=atob(e);"
                           "var r=\"\";for(var i=0;i<b.length;i++){"
                           "r+=String.fromCharCode(b.charCodeAt(i)^k.charCodeAt(i%k.length));}"
                           "return r;}\n"
                           "var rawVideo = _0xd(\"%2\");\n")
                           .arg(obfuscationKey, QString::fromLatin1(master.toBase64()));
  if (includeSkipBlock) {
    const QByteArray payload = moonObfuscate(skipJson.toUtf8(), key);
    javascript += QStringLiteral(
                      "var __skipSegments = (function(){ try { "
                      "return JSON.parse(_0xd(\"%1\")); } catch(_) { return []; } })();\n")
                      .arg(QString::fromLatin1(payload.toBase64()));
  }
  return moonPageFromJavascript(javascript);
}

QStringList skipSegmentTypes(const QVector<QVariantMap>& segments) {
  QStringList types;
  for (const QVariantMap& segment : segments) {
    types << segment.value(QStringLiteral("type")).toString();
  }
  return types;
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

void TestStreams::parsesMoonLabelledSourceList() {
  // The payload MoonAnime writes into rawVideo: quality labels in front of
  // direct media URLs, comma separated.
  const QVector<QualityVariant> variants = parseMoonSourceList(QStringLiteral(
      "[720p]https://s.example/content/v/abc/720/?expires=1&sig=a,"
      "[1080p]https://s.example/content/v/abc/1080/?expires=1&sig=b"));

  QCOMPARE(static_cast<int>(variants.size()), 2);
  QCOMPARE(variants.at(0).height, 720);
  QCOMPARE(variants.at(0).url, QStringLiteral("https://s.example/content/v/abc/720/?expires=1&sig=a"));
  QCOMPARE(variants.at(1).height, 1080);
  QCOMPARE(variants.at(1).url, QStringLiteral("https://s.example/content/v/abc/1080/?expires=1&sig=b"));
}

void TestStreams::keepsTheFirstMoonSourceForARepeatedHeight() {
  // A repeated label must not replace the earlier entry, which is the one the
  // provider listed first.
  const QVector<QualityVariant> variants = parseMoonSourceList(QStringLiteral(
      "[720p]https://s.example/first,"
      "[720p]https://s.example/second,"
      "[1080p]https://s.example/third"));

  QCOMPARE(static_cast<int>(variants.size()), 2);
  QCOMPARE(urlAtHeight(variants, 720), QStringLiteral("https://s.example/first"));
}

void TestStreams::aMoonMasterUrlIsNotALabelledSourceList() {
  // The older payload shape: one playlist URL and no labels. It must not be
  // reported as a variant with no height, or the caller would treat a playlist
  // as a playable file.
  const QVector<QualityVariant> variants = parseMoonSourceList(
      QStringLiteral("https://s.example/hls/master.m3u8"));
  QVERIFY(variants.isEmpty());
}

void TestStreams::ignoresMalformedMoonSourceEntries() {
  const QVector<QualityVariant> variants = parseMoonSourceList(QStringLiteral(
      "[720p],"
      "[0p]https://s.example/zero,"
      "[1080p60]https://s.example/suffixed?x=1,"
      "https://s.example/unlabelled"));

  QCOMPARE(static_cast<int>(variants.size()), 1);
  QCOMPARE(variants.first().height, 1080);
  QCOMPARE(variants.first().url, QStringLiteral("https://s.example/suffixed?x=1"));
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

void TestStreams::parsesMoonSkipSegmentsInPageOrder() {
  const QString json = QStringLiteral(
      "[{\"t\":\"mixed_opening\",\"s\":64.21,\"e\":154.82,"
      "\"n\":\"Пропустити змішаний опенінг\"},"
      "{\"t\":\"recap\",\"s\":90.0,\"e\":180.0,\"n\":\"Пропустити рекап\"},"
      "{\"t\":\"opening\",\"s\":512.91,\"e\":602.91,\"n\":\"Пропустити опенінг\"},"
      "{\"t\":\"ending\",\"s\":1206.66,\"e\":1296.91,\"n\":\"Пропустити ендінг\"},"
      "{\"t\":\"mixed_ending\",\"s\":1298.09,\"e\":1386.0,"
      "\"n\":\"Пропустити змішаний ендінг\"}]");
  const QVector<QVariantMap> segments = parseMoonSkipSegments(moonIframeHtml(json));

  QCOMPARE(static_cast<int>(segments.size()), 5);
  QCOMPARE(skipSegmentTypes(segments),
           (QStringList{QStringLiteral("mixed_opening"), QStringLiteral("recap"),
                        QStringLiteral("opening"), QStringLiteral("ending"),
                        QStringLiteral("mixed_ending")}));
  QCOMPARE(segments.at(0).value(QStringLiteral("start")).toDouble(), 64.21);
  QCOMPARE(segments.at(0).value(QStringLiteral("end")).toDouble(), 154.82);
  QCOMPARE(segments.at(1).value(QStringLiteral("start")).toDouble(), 90.0);
  QVERIFY(segments.at(0).value(QStringLiteral("end")).toDouble() >
          segments.at(1).value(QStringLiteral("start")).toDouble());
  QCOMPARE(segments.at(4).value(QStringLiteral("end")).toDouble(), 1386.0);
}

void TestStreams::ignoresInvalidMoonSegmentsWithoutDiscardingValidOnes() {
  const QString json = QStringLiteral(
      "[{\"t\":\"opening\",\"s\":10,\"e\":20},"
      "{\"t\":\"ending\",\"s\":-1,\"e\":5},"
      "{\"t\":\"recap\",\"s\":20,\"e\":20},"
      "{\"t\":\"ending\",\"s\":5,\"e\":10},"
      "{\"t\":\"promo\",\"s\":30,\"e\":40},"
      "{\"t\":\"recap\",\"s\":\"50\",\"e\":60},"
      "{\"t\":\"recap\",\"s\":70},"
      "\"not an object\"]");
  const QVector<QVariantMap> segments = parseMoonSkipSegments(moonIframeHtml(json));

  QCOMPARE(static_cast<int>(segments.size()), 2);
  QCOMPARE(skipSegmentTypes(segments),
           (QStringList{QStringLiteral("opening"), QStringLiteral("ending")}));
  QCOMPARE(segments.at(0).value(QStringLiteral("start")).toDouble(), 10.0);
  QCOMPARE(segments.at(1).value(QStringLiteral("start")).toDouble(), 5.0);
  QCOMPARE(segments.at(1).value(QStringLiteral("end")).toDouble(), 10.0);
}

void TestStreams::missingMoonSkipSegmentsYieldsAnEmptyList() {
  const QVector<QVariantMap> segments =
      parseMoonSkipSegments(moonIframeHtml({}, QStringLiteral("sTH7MjkXZiar"), false));
  QVERIFY(segments.isEmpty());

  QVERIFY(parseMoonSkipSegments(QStringLiteral("<html><body>maintenance</body></html>"))
              .isEmpty());
}

void TestStreams::malformedMoonSkipSegmentsYieldAnEmptyList() {
  QVERIFY(parseMoonSkipSegments(moonIframeHtml(QStringLiteral("{not json"))).isEmpty());
  QVERIFY(parseMoonSkipSegments(moonIframeHtml(QStringLiteral("{\"t\":\"opening\"}")))
              .isEmpty());
  QVERIFY(parseMoonSkipSegments(moonIframeHtml(QStringLiteral("[{\"t\":\"opening\","))).isEmpty());
}

void TestStreams::honoursAnArbitraryMoonObfuscationKey() {
  const QString json =
      QStringLiteral("[{\"t\":\"ending\",\"s\":12.5,\"e\":42.25}]");
  const QVector<QVariantMap> segments =
      parseMoonSkipSegments(moonIframeHtml(json, QStringLiteral("Qz9-AbCd_Efgh.Ijkl")));
  QCOMPARE(static_cast<int>(segments.size()), 1);
  QCOMPARE(skipSegmentTypes(segments), (QStringList{QStringLiteral("ending")}));
  QCOMPARE(segments.at(0).value(QStringLiteral("start")).toDouble(), 12.5);
  QCOMPARE(segments.at(0).value(QStringLiteral("end")).toDouble(), 42.25);
}

void TestStreams::cancellingAQuietProviderFetchReturnsPromptly() {
  QTcpServer server;
  QVERIFY(server.listen(QHostAddress::LocalHost, 0));
  std::atomic_bool cancelled{false};
  bool requested = false;
  connect(&server, &QTcpServer::newConnection, &server, [&] {
    while (QTcpSocket* socket = server.nextPendingConnection()) {
      connect(socket, &QTcpSocket::readyRead, &server, [&, socket] {
        socket->readAll();
        requested = true;
        cancelled.store(true);
      });
    }
  });
  QNetworkAccessManager manager;
  const mikai::backend::WorkCancellationScope scope(&cancelled);
  QElapsedTimer elapsed;
  elapsed.start();
  bool failed = false;
  QString failure;
  try {
    mikai::backend::fetchText(
        &manager, QStringLiteral("http://127.0.0.1:%1/quiet").arg(server.serverPort()), {});
  } catch (const StreamError& caught) {
    failed = true;
    failure = QString::fromUtf8(caught.what());
  }
  QVERIFY(requested);
  QVERIFY(failed);
  QVERIFY2(failure.contains(QStringLiteral("cancelled"), Qt::CaseInsensitive),
           qPrintable(failure));
  QVERIFY2(elapsed.elapsed() < 2000, "cancellation waited for the provider timeout");
}

void TestStreams::cancellingDuringRetryBackoffDoesNotRetry() {
  QTcpServer server;
  QVERIFY(server.listen(QHostAddress::LocalHost, 0));
  int requests = 0;
  std::atomic_bool responseSent{false};
  connect(&server, &QTcpServer::newConnection, &server, [&] {
    while (QTcpSocket* socket = server.nextPendingConnection()) {
      connect(socket, &QTcpSocket::readyRead, &server, [&, socket] {
        socket->readAll();
        ++requests;
        socket->write("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 2\r\n"
                      "Content-Length: 0\r\nConnection: close\r\n\r\n");
        socket->flush();
        responseSent.store(true);
      });
    }
  });

  std::atomic_bool cancelled{false};
  QNetworkAccessManager manager;
  const mikai::backend::WorkCancellationScope scope(&cancelled);
  std::atomic<qint64> cancelledAt{-1};
  QElapsedTimer elapsed;
  elapsed.start();
  std::thread cancelThread([&] {
    for (int i = 0; i < 100 && !responseSent.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (responseSent.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      cancelledAt.store(elapsed.elapsed());
      cancelled.store(true);
    }
  });
  bool failed = false;
  QString failure;
  try {
    mikai::backend::fetchText(
        &manager, QStringLiteral("http://127.0.0.1:%1/retry").arg(server.serverPort()), {});
  } catch (const StreamError& caught) {
    failed = true;
    failure = QString::fromUtf8(caught.what());
  }
  cancelThread.join();

  QVERIFY(failed);
  QVERIFY2(failure.contains(QStringLiteral("cancelled"), Qt::CaseInsensitive),
           qPrintable(failure));
  QCOMPARE(requests, 1);
  QVERIFY(cancelledAt.load() >= 0);
  QVERIFY2(elapsed.elapsed() - cancelledAt.load() < 1000,
           "cancellation did not interrupt retry backoff promptly");
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

void TestStreams::safeStreamUrlAllowsOnlyHttpAndHttps_data() {
  QTest::addColumn<QString>("rawUrl");
  QTest::addColumn<bool>("safe");
  QTest::newRow("https") << QStringLiteral("https://example.com/x") << true;
  QTest::newRow("http") << QStringLiteral("http://example.com/x") << true;
  QTest::newRow("https uppercase scheme") << QStringLiteral("HTTPS://example.com/x") << true;
  QTest::newRow("file") << QStringLiteral("file:///etc/passwd") << false;
  QTest::newRow("ms-msdt") << QStringLiteral("ms-msdt://x") << false;
  QTest::newRow("javascript") << QStringLiteral("javascript:alert(1)") << false;
  QTest::newRow("data") << QStringLiteral("data:text/html,<script>alert(1)</script>") << false;
  QTest::newRow("ftp") << QStringLiteral("ftp://example.com/x") << false;
  QTest::newRow("empty") << QString() << false;
  QTest::newRow("schemeless") << QStringLiteral("example.com/x") << false;
  QTest::newRow("https without host") << QStringLiteral("https:///x") << false;
}

void TestStreams::safeStreamUrlAllowsOnlyHttpAndHttps() {
  QFETCH(QString, rawUrl);
  QFETCH(bool, safe);
  QCOMPARE(isSafeStreamUrl(rawUrl), safe);
}

void TestStreams::aSignedUrlNeverReachesAnExceptionMessage() {
  // detectSite refuses the URL, and the refusal must not echo the token that
  // rode in its query.
  bool raised = false;
  QString message;
  try {
    detectSite(QStringLiteral("https://cdn.example.com/v.mp4?token=SECRET"));
  } catch (const std::invalid_argument& caught) {
    raised = true;
    message = QString::fromUtf8(caught.what());
  }
  QVERIFY(raised);
  QVERIFY2(!message.contains(QStringLiteral("SECRET")), qPrintable(message));
  QVERIFY2(!message.contains(QStringLiteral("token=")), qPrintable(message));
  QVERIFY2(message.contains(QStringLiteral("cdn.example.com")), qPrintable(message));
}

void TestStreams::aFailedFetchDoesNotLeakItsSignedUrl() {
  QTcpServer server;
  QVERIFY(server.listen(QHostAddress::LocalHost, 0));
  connect(&server, &QTcpServer::newConnection, &server, [&] {
    while (QTcpSocket* socket = server.nextPendingConnection()) {
      connect(socket, &QTcpSocket::readyRead, &server, [&, socket] {
        socket->readAll();
        socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                      "Connection: close\r\n\r\n");
        socket->flush();
      });
    }
  });
  QNetworkAccessManager manager;
  bool failed = false;
  QString failure;
  try {
    mikai::backend::fetchText(
        &manager,
        QStringLiteral("http://127.0.0.1:%1/v.mp4?token=SECRET").arg(server.serverPort()), {});
  } catch (const StreamError& caught) {
    failed = true;
    failure = QString::fromUtf8(caught.what());
  }
  QVERIFY(failed);
  QVERIFY2(!failure.contains(QStringLiteral("SECRET")), qPrintable(failure));
  QVERIFY2(!failure.contains(QStringLiteral("token=")), qPrintable(failure));
  QVERIFY2(failure.contains(QStringLiteral("HTTP 404")), qPrintable(failure));
}
