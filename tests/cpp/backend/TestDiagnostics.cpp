#include "TestDiagnostics.hpp"

#include <backend/Diagnostics.hpp>
#include <app/support/CrashLog.hpp>

#include <cstdio>
#include <utility>

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLibraryInfo>
#include <QtCore/QList>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStandardPaths>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTemporaryFile>
#include <QtCore/QUuid>
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

// The cross-process handles the fallback case shares with its child run: the
// child reports where the log contract put it, and writes the same two lines
// the parent then reads back from a different process.
constexpr auto kLogPathReport = "mikai-log-path:";
constexpr auto kMessageMarkerPrefix = "qt-message-preservation-";
constexpr auto kFallbackBoundaryMarker = "fallback-boundary";
// The per-user identity the child adopts, so the folder a fallback run creates
// is one this test owns outright: nothing else writes there, and the whole
// folder can be removed afterwards.
constexpr auto kLogIdentityVariable = "MIKAI_DIAGNOSTICS_LOG_IDENTITY";

/// Whether `path` names something inside `folder`. The two strings come from
/// different processes, so a Windows drive-letter case difference must not read
/// as a different folder.
bool isUnder(const QString& path, const QString& folder) {
  QString candidate = QDir::cleanPath(path);
  QString root = QDir::cleanPath(folder) + QLatin1Char('/');
#ifdef Q_OS_WIN
  candidate = candidate.toLower();
  root = root.toLower();
#endif
  return candidate.startsWith(root);
}

/// Releases a per-user folder a fallback run created. The machine must be left
/// as it was found even when an assertion fails halfway, so the folder goes away
/// with the scope rather than at the end of the test body.
class ScopedFolderRemoval final {
 public:
  explicit ScopedFolderRemoval(QString path) : path_(std::move(path)) {}
  ~ScopedFolderRemoval() { QDir(path_).removeRecursively(); }
  ScopedFolderRemoval(const ScopedFolderRemoval&) = delete;
  ScopedFolderRemoval& operator=(const ScopedFolderRemoval&) = delete;

 private:
  QString path_;
};

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

void TestDiagnostics::qtMessageHandlerPreservesTheOriginalMessage() {
  // The fallback case runs this slot in a child process whose preferred log
  // folder refuses writes. That child names the per-user identity whose folder
  // it owns, so the parent can inspect and remove exactly that folder and
  // nothing else; an ordinary in-suite run carries no identity and keeps the
  // runner's.
  const QString identity = qEnvironmentVariable(kLogIdentityVariable);
  if (!identity.isEmpty()) {
    QCoreApplication::setApplicationName(identity);
  }

  mikai::app::installCrashLog();
  const QString path = mikai::app::logPath();
  QVERIFY(!path.isEmpty());

  // Where the public log contract actually put this run's log. The parent reads
  // this rather than re-deriving the platform's per-user folder on its own.
  std::fprintf(stdout, "%s%s\n", kLogPathReport, path.toLocal8Bit().constData());
  std::fflush(stdout);

  const QString marker = QStringLiteral("%1%2")
                             .arg(QString::fromLatin1(kMessageMarkerPrefix))
                             .arg(QDateTime::currentMSecsSinceEpoch());
  qWarning().noquote() << marker;
  // A credential in the same run, so the file the parent reads is evidence for
  // both halves of the boundary: the message survives, the token does not.
  mikai::app::writeLogLine(QStringLiteral("%1 GET %2")
                               .arg(QString::fromLatin1(kFallbackBoundaryMarker),
                                    QString::fromLatin1(kSignedUrl)));

  QFile file(path);
  QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
  QVERIFY(QString::fromUtf8(file.readAll()).contains(marker));
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

void TestDiagnostics::aBlockedPreferredLogFolderFallsBackToThePerUserFolder() {
  // The preferred log folder is the executable's own, and the AppImage case is
  // that mount refusing writes. Permission bits do not stop the owner on
  // Windows, so the fixture blocks the preferred path itself: a directory where
  // the run would write makes every open of that path fail on both platforms,
  // with no ACL dependency and no change to how the product decides.
  QTemporaryDir sandbox;
  QVERIFY(sandbox.isValid());
  const QString exeDir = QDir(sandbox.path()).filePath(QStringLiteral("bin"));
  QVERIFY(QDir().mkpath(exeDir));
  const QString childExe = QDir(exeDir).filePath(
      QFileInfo(QCoreApplication::applicationFilePath()).fileName());
  QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), childExe));
  QVERIFY(QFile::setPermissions(childExe, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                             QFileDevice::ExeOwner));
  // Both names a run could write there: the log file itself, and the write probe
  // the folder decision takes before it writes anything. A directory at either
  // name refuses the open, and neither leaves a log behind to mistake for one.
  for (const QString& blocked :
       {QStringLiteral("mikai.log"), QStringLiteral(".mikai-log-probe")}) {
    QVERIFY2(QDir().mkpath(QDir(exeDir).filePath(blocked)),
             qPrintable(QStringLiteral("cannot block the preferred log path %1").arg(blocked)));
  }
  // The block is the fixture, so it is checked rather than assumed: a platform
  // where an open of a directory succeeded would silently stop exercising the
  // fallback at all.
  QFile blockedProbe(QDir(exeDir).filePath(QStringLiteral(".mikai-log-probe")));
  QVERIFY2(!blockedProbe.open(QIODevice::WriteOnly),
           "the fixture did not block the preferred log path");

  // The child names the per-user identity whose folder it owns, and the parent
  // derives that same folder from the same public setting, so what follows is
  // about the folder this run created and not about another run's log.
  const QString identity =
      QStringLiteral("log-fallback-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
  const QString applicationName = QCoreApplication::applicationName();
  QCoreApplication::setApplicationName(identity);
  const QString ownerFolder =
      QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  QCoreApplication::setApplicationName(applicationName);
  QVERIFY2(!ownerFolder.isEmpty(), "the platform named no per-user data folder");
  QVERIFY2(ownerFolder.contains(identity),
           qPrintable(QStringLiteral("the per-user folder is not the one this run owns: %1")
                          .arg(ownerFolder)));
  const ScopedFolderRemoval ownerCleanup(ownerFolder);

  QProcess child;
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
  environment.insert(QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"),
                     QDir(QLibraryInfo::path(QLibraryInfo::PluginsPath))
                         .filePath(QStringLiteral("platforms")));
  environment.insert(QStringLiteral("PATH"),
                     QLibraryInfo::path(QLibraryInfo::BinariesPath) + QDir::listSeparator() +
                         environment.value(QStringLiteral("PATH")));
  environment.insert(QString::fromLatin1(kLogIdentityVariable), identity);
  child.setProcessEnvironment(environment);
  child.setWorkingDirectory(sandbox.path());
  child.start(childExe, {QStringLiteral("TestDiagnostics"),
                         QStringLiteral("qtMessageHandlerPreservesTheOriginalMessage")});
  QVERIFY2(child.waitForStarted(), qPrintable(child.errorString()));
  QVERIFY2(child.waitForFinished(30000), "child log-fallback run did not finish");
  const QByteArray childOutput = child.readAllStandardOutput();
  const QByteArray childError = child.readAllStandardError();
  QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
           qPrintable(QStringLiteral("child fallback run failed (%1): %2")
                          .arg(child.exitCode())
                          .arg(QString::fromLocal8Bit(childError))));

  // Where the contract put the log, reported by the process that resolved it
  // rather than guessed from a data folder this test would have to re-derive.
  QString loggedPath;
  const QByteArray pathReport(kLogPathReport);
  const QList<QByteArray> childLines = childOutput.split('\n');
  for (const QByteArray& line : childLines) {
    if (line.startsWith(pathReport)) {
      loggedPath = QString::fromLocal8Bit(line.mid(pathReport.size())).trimmed();
      break;
    }
  }
  QVERIFY2(!loggedPath.isEmpty(),
           qPrintable(QStringLiteral("the child reported no log path:\n%1%2")
                          .arg(QString::fromLocal8Bit(childOutput),
                               QString::fromLocal8Bit(childError))));

  // The preferred folder refused writes, so the log must be in the per-user
  // folder this run owns - never beside the exe that could not take it.
  QVERIFY2(!isUnder(loggedPath, exeDir),
           qPrintable(QStringLiteral("the log landed in the blocked folder: %1").arg(loggedPath)));
  QVERIFY2(isUnder(loggedPath, ownerFolder),
           qPrintable(QStringLiteral("the log did not land in the per-user folder %1: %2")
                          .arg(ownerFolder, loggedPath)));

  // The file at that path, not just the path: the fallback folder has to receive
  // the lines, with the message intact and the credential already gone.
  QFile fallbackLog(loggedPath);
  QVERIFY2(fallbackLog.open(QIODevice::ReadOnly | QIODevice::Text),
           qPrintable(QStringLiteral("nothing was written to the fallback log %1: %2")
                          .arg(loggedPath, fallbackLog.errorString())));
  const QString stored = QString::fromUtf8(fallbackLog.readAll());
  fallbackLog.close();

  // The child's own message arrived verbatim, in the folder the fallback chose.
  QVERIFY2(stored.contains(QString::fromLatin1(kMessageMarkerPrefix)),
           qPrintable(QStringLiteral("the message never reached the fallback log: %1").arg(stored)));
  // And the signed line it wrote is there, redacted, exactly as it is beside a
  // writable exe: the fallback moves the file, never the boundary.
  QVERIFY2(stored.contains(QString::fromLatin1(kFallbackBoundaryMarker)),
           qPrintable(QStringLiteral("the boundary line never reached the fallback log: %1")
                          .arg(stored)));
  QVERIFY2(!stored.contains(QStringLiteral("SE.MARKER_TOKEN")) &&
               !stored.contains(QStringLiteral("index.m3u8")),
           qPrintable(QStringLiteral("the signed URL reached the fallback log: %1").arg(stored)));
  QVERIFY2(stored.contains(QString::fromLatin1(kMarker)),
           qPrintable(QStringLiteral("nothing was redacted in the fallback log: %1").arg(stored)));
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
