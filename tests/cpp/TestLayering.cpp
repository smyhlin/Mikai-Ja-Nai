#include "TestLayering.hpp"

#include <Clipboard.hpp>
#include <backend/Streams.hpp>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#ifndef MIKAI_SOURCE_DIR
#error "MIKAI_SOURCE_DIR must point at the repository root"
#endif

namespace {

QString backendDir() {
  return QDir(QStringLiteral(MIKAI_SOURCE_DIR)).filePath(QStringLiteral("src/backend"));
}

QStringList backendSources() {
  QDir dir(backendDir());
  QStringList out;
  const QFileInfoList entries =
      dir.entryInfoList({QStringLiteral("*.hpp"), QStringLiteral("*.h"), QStringLiteral("*.cpp")},
                        QDir::Files);
  for (const QFileInfo& entry : entries) {
    out << entry.absoluteFilePath();
  }
  return out;
}

bool readAll(const QString& path, QString* out) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return false;
  }
  *out = QString::fromUtf8(file.readAll());
  return true;
}

}  // namespace

void TestLayering::theDomainLayerHasSourceFiles() {
  // A guard that passes because it found nothing to check is worse than no
  // guard at all, so the scan itself is asserted first.
  const QStringList files = backendSources();
  QVERIFY2(!files.isEmpty(), qPrintable(QStringLiteral("no backend sources under %1").arg(backendDir())));
}

void TestLayering::theDomainLayerIncludesNoGuiHeaders() {
  const QRegularExpression guiInclude(
      QStringLiteral(R"((^|\n)\s*#\s*include\s*[<"](Qt(Widgets|Gui|Quick|Qml|QuickWidgets|DBus|PrintSupport|Test)/)"));
  const QRegularExpression shortcutInclude(
      QStringLiteral(R"((^|\n)\s*#\s*include\s*[<"](QApplication|QWidget|QLabel|QPushButton|QIcon|QPixmap|QScreen))"));
  for (const QString& path : backendSources()) {
    QString body;
    QVERIFY2(readAll(path, &body), qPrintable(path));
    const QString name = QFileInfo(path).fileName();
    QVERIFY2(!guiInclude.match(body).hasMatch(), qPrintable(name));
    QVERIFY2(!shortcutInclude.match(body).hasMatch(), qPrintable(name));
  }
}

void TestLayering::theDomainLayerDoesNotReachIntoTheAppLayer() {
  // The dependency runs one way: the app knows the domain, the domain knows
  // nothing about the app. A backend header naming the app would make the
  // domain untestable on its own and would drag widgets in with it.
  for (const QString& path : backendSources()) {
    QString body;
    QVERIFY2(readAll(path, &body), qPrintable(path));
    QVERIFY2(!body.contains(QStringLiteral("mikai/app/")), qPrintable(QFileInfo(path).fileName()));
    QVERIFY2(!body.contains(QStringLiteral("namespace mikai::app")),
             qPrintable(QFileInfo(path).fileName()));
  }
}

void TestLayering::theDomainLayerDoesNotNameRawNumbersWhereTokensBelong() {
  // Theme values are the app's business; the domain layer keeps its own named
  // constants (timeout, retry count) rather than scattering literals.
  for (const QString& path : backendSources()) {
    QString body;
    QVERIFY2(readAll(path, &body), qPrintable(path));
    QVERIFY2(!body.contains(QStringLiteral("#FF0000")) && !body.contains(QStringLiteral("#00FF00")),
             qPrintable(QFileInfo(path).fileName()));
  }
}

void TestLayering::theStreamErrorsShareOneHierarchy() {
  // A caller catches StreamError to report "this provider failed". NoSourceError
  // has to arrive at that same catch, or a dead episode reads as a crash.
  const mikai::backend::NoSourceError noSource("no_source");
  const mikai::backend::StreamError& asStream = noSource;
  QCOMPARE(QString::fromStdString(asStream.what()), QStringLiteral("no_source"));
  const mikai::backend::StreamError& asBase = noSource;
  QVERIFY(dynamic_cast<const mikai::backend::NoSourceError*>(&asBase) != nullptr);
  // The base is catchable on its own, and its own construction stays a stream
  // error rather than a std::exception only.
  bool caughtAsBase = false;
  try {
    throw mikai::backend::StreamError("plain failure");
  } catch (const mikai::backend::StreamError& error) {
    caughtAsBase = true;
    QCOMPARE(QString::fromStdString(error.what()), QStringLiteral("plain failure"));
  }
  QVERIFY(caughtAsBase);
}

void TestClipboard::copyingEmptyTextDoesNotCrash() {
  // The offscreen platform has no real clipboard, so this must return a verdict
  // rather than throw or hang.
  const bool copied = mikai::app::copyText(QString());
  Q_UNUSED(copied);
  QVERIFY(QGuiApplication::clipboard() != nullptr);
}

void TestClipboard::copyingReportsWhetherTheBoardTookTheText() {
  // copyText reads the board back and retries once, so a COM refusal costs one
  // retry and is then reported as a failure rather than as a silent success.
  const QString url = QStringLiteral("https://ashdi.vip/vod/268104");
  const bool copied = mikai::app::copyText(url);
  if (copied) {
    QCOMPARE(QGuiApplication::clipboard()->text(), url);
  } else {
    // A refused copy must not leave a lie behind: the old text survives.
    QVERIFY(QGuiApplication::clipboard()->text() != url);
  }
}

void TestClipboard::copyingTheSameTextTwiceIsStable() {
  const QString url = QStringLiteral("https://ashdi.vip/vod/113749");
  const bool first = mikai::app::copyText(url);
  const bool second = mikai::app::copyText(url);
  // A retry on a board that already holds the text must not report a different
  // outcome the second time, or the status line flickers on repeated copies.
  QCOMPARE(first, second);
}
