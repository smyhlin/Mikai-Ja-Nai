#include "TestDiagnostics.hpp"

#include <backend/Diagnostics.hpp>
#include <app/support/CrashLog.hpp>

#include <cstdio>

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryFile>
#include <QtCore/QtGlobal>

using mikai::backend::redactDiagnostic;

namespace {

// A signed media URL the way a CDN hands one back: credentials in the userinfo,
// the token in the path, and the hashed query a player echoes back. None of the
// three may survive redaction.
constexpr auto kSignedUrl =
    "http://user:pass@cdn.example.invalid/media/7428/anime/1080/"
    "index.m3u8?token=SE.MARKER_TOKEN.abc123&expires=1893456000&hdnts=ip=10.0.0.1~exp=1893456000";

// The shapes the transport produces for a credential: a bearer, an X-API-Key, a
// query parameter and a JSON body. None is a real secret.
constexpr auto kBearerLine = "Authorization: Bearer mk_MARKER_TOKEN_NOT_REAL_0001";
constexpr auto kApiKeyLine = "X-API-Key: mk_MARKER_TOKEN_NOT_REAL_0002";
constexpr auto kApiKeyQuery =
    "https://api.example.invalid/public/v1/anime?api_key=SE.MARKER_KEY_0003";
constexpr auto kJsonSecret =
    R"({"result":{"url":"http://host.invalid/a.mp4","api_key":"SE.MARKER_JSON_0004"}})";

// The one marker redaction leaves behind. It is fixed, so what a report says is
// "something was here", never which something.
constexpr auto kMarker = "<redacted>";

}  // namespace

void TestDiagnostics::aSignedUrlLosesItsPathItsQueryAndItsUserinfo() {
  const QString line =
      QStringLiteral("GET %1 failed: connection reset").arg(QString::fromLatin1(kSignedUrl));
  const QString redacted = redactDiagnostic(line);

  QVERIFY2(!redacted.contains(QStringLiteral("SE.MARKER_TOKEN")),
           qPrintable(QStringLiteral("the signed token survived: %1").arg(redacted)));
  QVERIFY2(!redacted.contains(QStringLiteral("index.m3u8")),
           qPrintable(QStringLiteral("the signed path survived: %1").arg(redacted)));
  QVERIFY2(!redacted.contains(QStringLiteral("hdnts=ip=10.0.0.1")),
           qPrintable(QStringLiteral("the signed query survived: %1").arg(redacted)));
  QVERIFY2(!redacted.contains(QStringLiteral("user:pass")),
           qPrintable(QStringLiteral("the userinfo survived: %1").arg(redacted)));
  // The host stays, because a report that does not name the endpoint that
  // failed is a report nobody can act on.
  QVERIFY2(redacted.contains(QStringLiteral("http://cdn.example.invalid")),
           qPrintable(QStringLiteral("the endpoint was removed too: %1").arg(redacted)));
  QVERIFY2(redacted.contains(QStringLiteral("http://cdn.example.invalid") +
                             QString::fromLatin1(kMarker)),
           qPrintable(QStringLiteral("the URL redaction marker is missing: %1").arg(redacted)));
  // The reason a report is worth keeping at all: what the request was for.
  QVERIFY2(redacted.contains(QStringLiteral("failed: connection reset")),
           qPrintable(QStringLiteral("the surrounding sentence was eaten: %1").arg(redacted)));
}

void TestDiagnostics::aBareHostAndPathWithoutSecretsIsLeftAlone() {
  // A remote address with no path, query or userinfo is already the whole
  // answer, so it is returned exactly as it came in. Anything more than the
  // authority is what a signed URL needs, so that part goes.
  const QString authorityOnly = QStringLiteral("http://localhost:8080");
  QCOMPARE(redactDiagnostic(authorityOnly), authorityOnly);

  const QString plainHost = QStringLiteral("see example.invalid");
  QCOMPARE(redactDiagnostic(plainHost), plainHost);
}

void TestDiagnostics::theSchemeAndHostSurviveSoTheReportIsStillUseable() {
  // A remote path is redacted, and what remains names the endpoint: the report
  // still answers "which server refused".
  const QString redacted = redactDiagnostic(
      QStringLiteral("request failed: GET http://plain.example.invalid/a/b/index.m3u8 (%1s)")
          .arg(QStringLiteral("2.1")));
  QVERIFY2(redacted.contains(QStringLiteral("http://plain.example.invalid")),
           qPrintable(QStringLiteral("the endpoint was removed: %1").arg(redacted)));
  QVERIFY2(redacted.contains(QString::fromLatin1(kMarker)),
           qPrintable(QStringLiteral("no redaction marker: %1").arg(redacted)));
  QVERIFY2(redacted.contains(QStringLiteral("request failed: GET")),
           qPrintable(QStringLiteral("the surrounding sentence was eaten: %1").arg(redacted)));
  QVERIFY2(!redacted.contains(QStringLiteral("index.m3u8")),
           qPrintable(QStringLiteral("the path survived: %1").arg(redacted)));
}

void TestDiagnostics::anAuthorizationHeaderLosesItsCredential() {
  const QString redacted = redactDiagnostic(
      QStringLiteral("reply headers: %1; %2")
          .arg(QString::fromLatin1(kBearerLine), QString::fromLatin1(kApiKeyLine)));

  QVERIFY2(!redacted.contains(QStringLiteral("mk_MARKER_TOKEN_NOT_REAL_0001")),
           qPrintable(QStringLiteral("the bearer credential survived: %1").arg(redacted)));
  QVERIFY2(!redacted.contains(QStringLiteral("mk_MARKER_TOKEN_NOT_REAL_0002")),
           qPrintable(QStringLiteral("the api key credential survived: %1").arg(redacted)));
  QVERIFY2(redacted.contains(QString::fromLatin1(kMarker)),
           qPrintable(QStringLiteral("no secret marker: %1").arg(redacted)));
  // The name is what the value was rejected by, so it stays readable.
  QVERIFY2(redacted.contains(QStringLiteral("Authorization")),
           qPrintable(QStringLiteral("the credential name was eaten: %1").arg(redacted)));
  QVERIFY2(redacted.contains(QStringLiteral("X-API-Key")),
           qPrintable(QStringLiteral("the credential name was eaten: %1").arg(redacted)));
  QVERIFY2(redacted.contains(QStringLiteral("reply headers:")),
           qPrintable(QStringLiteral("the surrounding sentence was eaten: %1").arg(redacted)));
}

void TestDiagnostics::anApiKeyQueryParameterLosesItsValue() {
  const QString redacted =
      redactDiagnostic(QStringLiteral("failed: %1").arg(QString::fromLatin1(kApiKeyQuery)));

  QVERIFY2(!redacted.contains(QStringLiteral("SE.MARKER_KEY_0003")),
           qPrintable(QStringLiteral("the query key survived: %1").arg(redacted)));
  QVERIFY2(redacted.contains(QString::fromLatin1(kMarker)),
           qPrintable(QStringLiteral("no secret marker: %1").arg(redacted)));
  QVERIFY2(redacted.contains(QStringLiteral("failed:")),
           qPrintable(QStringLiteral("the label was eaten: %1").arg(redacted)));
}

void TestDiagnostics::aJsonSecretLosesItsValue() {
  const QString redacted = redactDiagnostic(QString::fromLatin1(kJsonSecret));

  QVERIFY2(!redacted.contains(QStringLiteral("SE.MARKER_JSON_0004")),
           qPrintable(QStringLiteral("the JSON secret survived: %1").arg(redacted)));
  QVERIFY2(redacted.contains(QString::fromLatin1(kMarker)),
           qPrintable(QStringLiteral("no secret marker: %1").arg(redacted)));
}

void TestDiagnostics::everyKeyNameSpellingIsCovered_data() {
  QTest::addColumn<QString>("line");
  QTest::addColumn<QString>("forbidden");

  QTest::newRow("api_key equals")
      << QStringLiteral("api_key=SE.KEY_A") << QStringLiteral("SE.KEY_A");
  QTest::newRow("apikey equals")
      << QStringLiteral("apikey = SE.KEY_B") << QStringLiteral("SE.KEY_B");
  QTest::newRow("key colon") << QStringLiteral("key: SE.KEY_C") << QStringLiteral("SE.KEY_C");
  QTest::newRow("access_key colon")
      << QStringLiteral("access_key: SE.KEY_D") << QStringLiteral("SE.KEY_D");
  QTest::newRow("secret colon") << QStringLiteral("secret: SE.KEY_E") << QStringLiteral("SE.KEY_E");
  QTest::newRow("token colon") << QStringLiteral("token: SE.KEY_F") << QStringLiteral("SE.KEY_F");
  QTest::newRow("access_token colon")
      << QStringLiteral("Access-Token: SE.KEY_G") << QStringLiteral("SE.KEY_G");
  QTest::newRow("refresh_token colon")
      << QStringLiteral("Refresh_Token: SE.KEY_H") << QStringLiteral("SE.KEY_H");
  QTest::newRow("auth colon") << QStringLiteral("auth: SE.KEY_I") << QStringLiteral("SE.KEY_I");
  QTest::newRow("authorization colon")
      << QStringLiteral("Authorization: SE.KEY_J") << QStringLiteral("SE.KEY_J");
  QTest::newRow("x-api-key colon")
      << QStringLiteral("X-API-Key: SE.KEY_K") << QStringLiteral("SE.KEY_K");
  QTest::newRow("x-api-token colon")
      << QStringLiteral("X-Api-Token: SE.KEY_L") << QStringLiteral("SE.KEY_L");
  QTest::newRow("session colon") << QStringLiteral("session: SE.KEY_M") << QStringLiteral("SE.KEY_M");
  QTest::newRow("password colon")
      << QStringLiteral("password: SE.KEY_N") << QStringLiteral("SE.KEY_N");
  QTest::newRow("passwd colon") << QStringLiteral("passwd: SE.KEY_O") << QStringLiteral("SE.KEY_O");
  QTest::newRow("credentials colon")
      << QStringLiteral("credentials: SE.KEY_P") << QStringLiteral("SE.KEY_P");
  QTest::newRow("signature colon")
      << QStringLiteral("signature: SE.KEY_Q") << QStringLiteral("SE.KEY_Q");
  QTest::newRow("sig colon") << QStringLiteral("sig: SE.KEY_R") << QStringLiteral("SE.KEY_R");
  QTest::newRow("bearer word")
      << QStringLiteral("using bearer SE.KEY_S") << QStringLiteral("SE.KEY_S");
  QTest::newRow("json quoted name")
      << QStringLiteral("\"api_key\": \"SE.KEY_T\"") << QStringLiteral("SE.KEY_T");
  QTest::newRow("an api key shape")
      << QStringLiteral("token mk_LIVE_abcdef0123 delivered")
      << QStringLiteral("mk_LIVE_abcdef0123");
}

void TestDiagnostics::everyKeyNameSpellingIsCovered() {
  QFETCH(QString, line);
  QFETCH(QString, forbidden);
  const QString redacted = redactDiagnostic(line);
  QVERIFY2(!redacted.contains(forbidden),
           qPrintable(
               QStringLiteral("the secret in \"%1\" survived as \"%2\"").arg(line, redacted)));
}

void TestDiagnostics::ordinaryTextIsPreservedVerbatim() {
  const QString text =
      QStringLiteral("mpv [player] Playing: D:/media/anime/7428/episode-01.mkv");
  QCOMPARE(redactDiagnostic(text), text);

  // A local model path and a percent label are what a real log line carries, and
  // none of it may be damaged by the boundary that protects it.
  const QString local = QStringLiteral("engine C:\\models\\rtx.onnx loaded (98%) at 640x480");
  QCOMPARE(redactDiagnostic(local), local);
}

void TestDiagnostics::aWordContainingKeyIsNotASecret() {
  // "monkey" contains "key"; only a real key name in name=value form is a secret.
  const QString text = QStringLiteral("monkey=12 and donkey: 34 are fine");
  QCOMPARE(redactDiagnostic(text), text);
}

void TestDiagnostics::anEmptyStringIsStillEmpty() {
  QVERIFY(redactDiagnostic(QString()).isEmpty());
}

void TestDiagnostics::aMediaLogLineCarriesNoSignedUrl() {
  // The shape an mpv log line has when it reaches Qt logging: a prefixed text
  // field naming the file it is playing. Redaction has to happen to that whole
  // line, because that is exactly the string the boundary receives.
  const QString mpvLine =
      QStringLiteral("[mpv] Playing: %1").arg(QString::fromLatin1(kSignedUrl));
  const QString redacted = redactDiagnostic(mpvLine);

  QVERIFY2(!redacted.contains(QStringLiteral("SE.MARKER_TOKEN")),
           qPrintable(QStringLiteral("the media URL token survived: %1").arg(redacted)));
  QVERIFY2(!redacted.contains(QStringLiteral("index.m3u8")),
           qPrintable(QStringLiteral("the media URL path survived: %1").arg(redacted)));
  QVERIFY2(!redacted.contains(QStringLiteral("user:pass")),
           qPrintable(QStringLiteral("the media URL userinfo survived: %1").arg(redacted)));
  QVERIFY2(redacted.contains(QStringLiteral("[mpv] Playing:")),
           qPrintable(QStringLiteral("the mpv label was eaten: %1").arg(redacted)));

  // The raw line itself still carries the token, so this test can only pass by
  // the redaction doing the work.
  QVERIFY(mpvLine.contains(QStringLiteral("SE.MARKER_TOKEN")));
  QVERIFY(redacted.contains(QStringLiteral("<redacted")));
}

void TestDiagnostics::aSignedUrlWrittenToTheLogFileIsRedactedOnDisk() {
  // The session log is what a crash report is cut from, so a token that redacted
  // only inside the helper and was written raw here would still be a
  // disclosure. This reads the file back, which is the only evidence there is.
  // installCrashLog decides the folder from the loader, and logging before it is
  // a no-op by design, so there is nothing to assert until it has run.
  mikai::app::installCrashLog();
  const QString path = mikai::app::logPath();
  QVERIFY2(!path.isEmpty(), "the session log has no path before installCrashLog ran");
  // A read-only or missing folder makes the log a no-op by design; there is
  // nothing to read back, so the boundary cannot be exercised. Skipping is the
  // honest verdict here, not a pass.
  const QFileInfo logDir(QFileInfo(path).dir().absolutePath());
  if (!logDir.exists() || !logDir.isWritable()) {
      QSKIP("the session log folder is not writable");
  }

  // A run-unique marker, so the assertions below are about the line this test
  // wrote rather than whatever a previous run or suite left in the log.
  const QString marker =
      QStringLiteral("boundary-%1").arg(QDateTime::currentMSecsSinceEpoch());
  const QString line = QStringLiteral("GET %1 %2").arg(marker, QString::fromLatin1(kSignedUrl));
  const QString bearer =
      QStringLiteral("%1 %2").arg(marker, QString::fromLatin1(kBearerLine));
  mikai::app::writeLogLine(line);
  mikai::app::writeLogLine(bearer);

  QFile file(path);
  QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(file.errorString()));
  const QString stored = QString::fromUtf8(file.readAll());
  file.close();

  // This test's own line reached the file, so what follows is about that line
  // and not about a log nobody wrote.
  QVERIFY2(stored.contains(marker),
           qPrintable(QStringLiteral("the boundary line never reached the log at %1").arg(path)));

  // The tokens are gone from the file, not merely from the return value.
  QVERIFY2(!stored.contains(QStringLiteral("SE.MARKER_TOKEN")),
           qPrintable(QStringLiteral("the signed token reached the log: %1").arg(stored)));
  QVERIFY2(!stored.contains(QStringLiteral("index.m3u8")),
           qPrintable(QStringLiteral("the signed path reached the log: %1").arg(stored)));
  QVERIFY2(!stored.contains(QStringLiteral("user:pass")),
           qPrintable(QStringLiteral("the userinfo reached the log: %1").arg(stored)));
  QVERIFY2(!stored.contains(QStringLiteral("mk_MARKER_TOKEN_NOT_REAL_0001")),
           qPrintable(QStringLiteral("the bearer credential reached the log: %1").arg(stored)));
  QVERIFY2(stored.contains(QStringLiteral("<redacted")),
           qPrintable(QStringLiteral("nothing was redacted on disk: %1").arg(stored)));
  // What is still worth reading: what the request was for, in the order it was
  // written. The pair lines carry the marker before their own label.
  QVERIFY2(stored.contains(QStringLiteral("GET %1 ").arg(marker)) &&
               stored.contains(QStringLiteral("%1 Authorization: ").arg(marker)),
           qPrintable(QStringLiteral("the log lost the request it was reporting: %1")
                          .arg(stored)));
}

int runTestDiagnostics(int argc, char** argv) {
  TestDiagnostics suite;
  QStringList arguments;
  arguments.reserve(argc + 1);
  arguments << QString::fromLocal8Bit(argv[0]);
  for (int i = 1; i < argc; ++i) arguments << QString::fromLocal8Bit(argv[i]);
  QTemporaryFile report;
  const bool captureReport = !arguments.contains(QStringLiteral("-o"));
  if (captureReport) {
    if (!report.open()) {
      std::fprintf(stderr, "[mikai_tests] cannot create report for TestDiagnostics\n");
      return 2;
    }
    report.close();
    arguments << QStringLiteral("-o") << report.fileName() + QStringLiteral(",txt");
  }
  const int rc = QTest::qExec(&suite, arguments);
  if (captureReport && rc != 0) {
    QFile output(report.fileName());
    if (output.open(QIODevice::ReadOnly)) {
      const QByteArray text = output.readAll();
      std::fwrite(text.constData(), 1, static_cast<size_t>(text.size()), stderr);
    } else {
      std::fprintf(stderr, "[mikai_tests] cannot read report for TestDiagnostics\n");
    }
  }
  std::fflush(stderr);
  return rc;
}
