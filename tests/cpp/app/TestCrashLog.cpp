#include "TestCrashLog.hpp"

#include <app/support/CrashLog.hpp>

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QString>

namespace {

// installCrashLog() is process-global: once any suite in this blob has run
// it, the log path is set for the rest of the process. The pre-install slots
// below can only observe the pristine state, so they skip honestly when
// another suite got there first instead of asserting on a state that is gone.
bool crashLogAlreadyInstalled() { return !mikai::app::logPath().isEmpty(); }

}  // namespace

void TestCrashLog::theLogPathIsEmptyUntilInstallRuns() {
  if (crashLogAlreadyInstalled()) {
    QSKIP("another suite already installed the crash log in this process");
  }
  QVERIFY(mikai::app::logPath().isEmpty());
}

void TestCrashLog::writeLogLineBeforeInstallIsANoOp() {
  if (crashLogAlreadyInstalled()) {
    QSKIP("another suite already installed the crash log in this process");
  }
  // Logging before install is a no-op by design (CrashLog.cpp: appendToLog
  // returns the message unchanged while g_logPath is empty): no file is
  // created, nothing crashes, and the log path stays empty.
  mikai::app::writeLogLine(QStringLiteral("before-install marker"));
  QVERIFY(mikai::app::logPath().isEmpty());
}

void TestCrashLog::installCrashLogSetsTheLogPathAndWritesTheBanner() {
  mikai::app::installCrashLog();
  const QString path = mikai::app::logPath();
  QVERIFY(!path.isEmpty());
  QVERIFY2(path.endsWith(QStringLiteral("mikai.log")),
           qPrintable(QStringLiteral("unexpected session log path: %1").arg(path)));

  QFile file(path);
  QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
           qPrintable(file.errorString()));
  QVERIFY2(QString::fromUtf8(file.readAll()).contains(QStringLiteral("---- mikai started ----")),
           qPrintable(QStringLiteral("no start banner in the session log at %1").arg(path)));
}

void TestCrashLog::installCrashLogIsIdempotent() {
  // The report block is allocated once (CrashLog.cpp:943) and intentionally
  // never freed; a second install must reuse the same state, not allocate a
  // second block or crash. The pointer itself is not observable outside its
  // translation unit, so the stable log path is the evidence.
  mikai::app::installCrashLog();
  const QString firstPath = mikai::app::logPath();
  QVERIFY(!firstPath.isEmpty());
  mikai::app::installCrashLog();
  QCOMPARE(mikai::app::logPath(), firstPath);
}

void TestCrashLog::writeLogLineAppendsToTheSessionLog() {
  mikai::app::installCrashLog();
  const QString path = mikai::app::logPath();
  QVERIFY(!path.isEmpty());
  // A read-only or missing folder makes the log a no-op by design; there is
  // nothing to read back, so the boundary cannot be exercised. Skipping is
  // the honest verdict here, not a pass.
  const QFileInfo logDir(QFileInfo(path).dir().absolutePath());
  if (!logDir.exists() || !logDir.isWritable()) {
    QSKIP("the session log folder is not writable");
  }

  const QString marker =
      QStringLiteral("crashlog-%1").arg(QDateTime::currentMSecsSinceEpoch());
  mikai::app::writeLogLine(marker);

  QFile file(path);
  QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
           qPrintable(file.errorString()));
  QVERIFY2(QString::fromUtf8(file.readAll()).contains(marker),
           qPrintable(QStringLiteral("the line never reached the log at %1").arg(path)));
}
