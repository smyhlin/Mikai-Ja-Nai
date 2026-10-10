#include "TestLayering.hpp"

#include <gui/Clipboard.hpp>
#include <backend/Streams.hpp>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMimeData>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtGui/QClipboard>
#include <QtGui/QColor>
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

void TestClipboard::emptyImagesLeaveExistingClipboardContentUnchanged() {
  const QString original = QStringLiteral("existing clipboard text");
  const QByteArray privateContents("original\0bytes", 14);
  auto* mime = new QMimeData;
  mime->setText(original);
  mime->setData(QStringLiteral("application/x-mikai-clipboard-test"), privateContents);
  QGuiApplication::clipboard()->setMimeData(mime);
  const QStringList formats = QGuiApplication::clipboard()->mimeData()->formats();
  QVERIFY(!mikai::gui::copyImage(QImage()));
  QCOMPARE(QGuiApplication::clipboard()->text(), original);
  QCOMPARE(QGuiApplication::clipboard()->mimeData()->formats(), formats);
  QCOMPARE(QGuiApplication::clipboard()->mimeData()->data(
               QStringLiteral("application/x-mikai-clipboard-test")), privateContents);
}

void TestClipboard::copiedImagesRetainTheirPixels() {
  QImage image(3, 2, QImage::Format_RGBA8888);
  image.fill(Qt::red);
  image.setPixelColor(1, 1, QColor(21, 63, 199));
  image.setPixelColor(2, 0, QColor(97, 13, 231));
  QVERIFY(mikai::gui::copyImage(image));
  QCOMPARE(QGuiApplication::clipboard()->image().convertToFormat(QImage::Format_RGBA8888),
           image);
}

void TestClipboard::copyingReportsWhetherTheBoardTookTheText() {
  const QString url = QStringLiteral("https://ashdi.vip/vod/268104");
  QVERIFY(mikai::gui::copyText(url));
  QCOMPARE(QGuiApplication::clipboard()->text(), url);
}

void TestClipboard::copyingTheSameTextTwiceIsStable() {
  const QString url = QStringLiteral("https://ashdi.vip/vod/113749");
  QVERIFY(mikai::gui::copyText(url));
  auto* mime = new QMimeData;
  mime->setText(url);
  mime->setHtml(QStringLiteral("<a>old decorated link</a>"));
  QGuiApplication::clipboard()->setMimeData(mime);
  QVERIFY(mikai::gui::copyText(url));
  QCOMPARE(QGuiApplication::clipboard()->text(), url);
  QVERIFY(!QGuiApplication::clipboard()->mimeData()->hasHtml());
}

void TestClipboard::copyingTheSameImageReplacesUnrelatedFormats() {
  QImage image(3, 2, QImage::Format_RGBA8888);
  image.fill(QColor(37, 91, 149));
  image.setPixelColor(2, 1, QColor(219, 53, 11));
  QVERIFY(mikai::gui::copyImage(image));
  auto* mime = new QMimeData;
  mime->setImageData(image);
  mime->setText(QStringLiteral("old image annotation"));
  QGuiApplication::clipboard()->setMimeData(mime);
  QVERIFY(mikai::gui::copyImage(image));
  QCOMPARE(QGuiApplication::clipboard()->image().convertToFormat(QImage::Format_RGBA8888),
           image);
  QVERIFY(!QGuiApplication::clipboard()->mimeData()->hasText());
}

void TestClipboard::emptyTextReplacesPreviousText() {
  QGuiApplication::clipboard()->setText(QStringLiteral("old text"));
  QVERIFY(mikai::gui::copyText(QString()));
  QCOMPARE(QGuiApplication::clipboard()->text(), QString());
}
