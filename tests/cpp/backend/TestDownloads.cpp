#include "TestDownloads.hpp"

#include <backend/Downloads.hpp>
#include <backend/Cancellation.hpp>

#include <cstdio>
#include <functional>
#include <stdexcept>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryFile>
#include <QtCore/QTimer>
#include <QtCore/QtGlobal>
#include <QtNetwork/QAbstractSocket>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

using mikai::backend::downloadToFile;

namespace {

QString sha256Of(const QByteArray& bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool writeFile(const QString& path, const QByteArray& bytes) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  return file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return file.readAll();
}

// A loopback server that hands one payload to one request. Paced mode sends the
// body in small chunks on a timer, which is the only way a single-threaded test
// can have a transfer still in flight while it changes its mind.
class LoopbackServer : public QObject {
 public:
  LoopbackServer(const QByteArray& body, QObject* parent = nullptr)
      : QObject(parent), body_(body) {}

  ~LoopbackServer() override { stop(); }

  bool start(QString* error) {
    if (!server_.listen(QHostAddress::LocalHost, 0)) {
      if (error) *error = server_.errorString();
      return false;
    }
    connect(&server_, &QTcpServer::newConnection, this, &LoopbackServer::onNewConnection);
    return true;
  }

  QUrl url(const QString& path) const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1%2")
                    .arg(server_.serverPort())
                    .arg(path));
  }

  QUrl url() const { return url(QStringLiteral("/payload.bin")); }

  void setChunkBytes(int bytes) { chunkBytes_ = qMax(1, bytes); }
  void setDelayMs(int ms) { delayMs_ = ms; }

  int chunksServed() const { return chunks_; }
  int requestsServed() const { return requests_; }

  void stop() {
    for (QTimer* timer : timers_) {
      timer->stop();
      timer->deleteLater();
    }
    timers_.clear();
    for (QTcpSocket* socket : sockets_) {
      socket->abort();
      socket->deleteLater();
    }
    sockets_.clear();
    server_.close();
  }

 private:
  void onNewConnection() {
    while (QTcpSocket* socket = server_.nextPendingConnection()) {
      sockets_.append(socket);
      socket->setProperty("sent", 0);
      connect(socket, &QTcpSocket::readyRead, this, &LoopbackServer::onReadyRead);
      connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        sockets_.removeOne(socket);
        socket->deleteLater();
      });
    }
  }

  void onReadyRead() {
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (socket == nullptr || served_.contains(socket)) return;
    served_.insert(socket, true);
    serve(socket);
  }

  void serve(QTcpSocket* socket) {
    ++requests_;
    const QByteArray header = QByteArrayLiteral("HTTP/1.1 200 OK\r\n") +
                              QByteArrayLiteral("Content-Type: application/octet-stream\r\n") +
                              QByteArrayLiteral("Content-Length: ") +
                              QByteArray::number(body_.size()) +
                              QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
    socket->write(header);
    if (delayMs_ <= 0) {
      socket->write(body_);
      socket->flush();
      ++chunks_;
      socket->disconnectFromHost();
      return;
    }
    sendChunk(socket);
    QTimer* timer = new QTimer(this);
    timers_.append(timer);
    connect(timer, &QTimer::timeout, this, [this, socket, timer] {
      // A socket the transfer gave up on is gone by now; serving it would write
      // into a socket nobody is reading and count chunks nobody received.
      if (socket->state() != QAbstractSocket::ConnectedState || !socket->property("paced").toBool()) {
        retire(timer);
        return;
      }
      if (!sendChunk(socket)) {
        retire(timer);
      }
    });
    timer->start(delayMs_);
  }

  void retire(QTimer* timer) {
    timer->stop();
    timers_.removeOne(timer);
    timer->deleteLater();
  }

  bool sendChunk(QTcpSocket* socket) {
    socket->setProperty("paced", true);
    const qint64 already = static_cast<qint64>(socket->property("sent").toLongLong());
    if (already >= body_.size()) {
      socket->flush();
      socket->disconnectFromHost();
      return false;
    }
    const qint64 want = qMin<qint64>(chunkBytes_, body_.size() - already);
    const qint64 written = socket->write(body_.constData() + already, want);
    socket->flush();
    if (written != want) return false;
    socket->setProperty("sent", static_cast<qlonglong>(already + want));
    ++chunks_;
    return true;
  }

  QTcpServer server_;
  QByteArray body_;
  int chunkBytes_ = 1024;
  int delayMs_ = 0;
  int chunks_ = 0;
  int requests_ = 0;
  QList<QTcpSocket*> sockets_;
  QHash<QTcpSocket*, bool> served_;
  QList<QTimer*> timers_;
};

// One payload: 4 KiB of deterministic bytes, big enough that a paced transfer is
// still in flight after the first few chunks and small enough that a digest is
// computed instantly.
QByteArray payload() {
  QByteArray bytes;
  bytes.resize(4096);
  for (int i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<char>((i * 31 + (i >> 8)) & 0xFF);
  }
  return bytes;
}

}  // namespace

void TestDownloads::aFileThatIsAlreadyThereSurvivesTheRefusal() {
  const QByteArray body = payload();
  LoopbackServer server(body);
  QString error;
  QVERIFY2(server.start(&error), qPrintable(error));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString destination = QDir(directory.path()).filePath(QStringLiteral("already.bin"));
  const QByteArray existing = QByteArrayLiteral("the caller's file, not ours to delete\n");
  QVERIFY(writeFile(destination, existing));
  const QDateTime modifiedBefore = QFileInfo(destination).lastModified();

  QString reason;
  const bool ok = downloadToFile(server.url(), destination, body.size(), sha256Of(body), {},
                                 {}, &reason);

  QVERIFY2(!ok, qPrintable(reason));
  QVERIFY2(!reason.isEmpty(), "a refusal has to say why");
  QVERIFY2(QFileInfo::exists(destination),
           "the destination this invocation did not create was destroyed");
  QCOMPARE(readFile(destination), existing);
  QCOMPARE(QFileInfo(destination).lastModified(), modifiedBefore);
}

void TestDownloads::theRefusalNamesWhatItRefusedToOverwrite() {
  const QByteArray body = payload();
  LoopbackServer server(body);
  QString error;
  QVERIFY2(server.start(&error), qPrintable(error));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString destination = QDir(directory.path()).filePath(QStringLiteral("named.bin"));
  QVERIFY(writeFile(destination, QByteArrayLiteral("contents\n")));

  QString reason;
  const bool ok = downloadToFile(server.url(), destination, body.size(), sha256Of(body), {},
                                 {}, &reason);
  QVERIFY2(!ok, qPrintable(reason));
  QVERIFY2(reason.contains(QStringLiteral("refus"), Qt::CaseInsensitive),
           qPrintable(QStringLiteral("refusal text is not a refusal: %1").arg(reason)));
  QVERIFY2(reason.contains(QStringLiteral("named.bin")),
           qPrintable(QStringLiteral("refusal does not name the file: %1").arg(reason)));
  // Whatever the reason says, the file it refused to overwrite is still there.
  QVERIFY(QFileInfo::exists(destination));
  QCOMPARE(readFile(destination), QByteArrayLiteral("contents\n"));
}

void TestDownloads::aDownloadedFileIsTheBytesThatWereSentAndWritten() {
  const QByteArray body = payload();
  LoopbackServer server(body);
  QString error;
  QVERIFY2(server.start(&error), qPrintable(error));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString destination = QDir(directory.path()).filePath(QStringLiteral("payload.bin"));

  QString reason;
  const bool ok = downloadToFile(server.url(), destination, body.size(), sha256Of(body), {},
                                 {}, &reason);
  QVERIFY2(ok, qPrintable(reason));
  const QByteArray onDisk = readFile(destination);
  QCOMPARE(onDisk.size(), body.size());
  QCOMPARE(onDisk, body);
  // The file on disk hashes to what the caller was told to expect, which is the
  // only thing "verified" can honestly mean.
  QCOMPARE(sha256Of(onDisk), sha256Of(body));
  QCOMPARE(server.chunksServed(), 1);
}

void TestDownloads::theProgressCallbackEndsOnTheWrittenSize() {
  const QByteArray body = payload();
  LoopbackServer server(body);
  server.setChunkBytes(512);
  server.setDelayMs(2);
  QString error;
  QVERIFY2(server.start(&error), qPrintable(error));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString destination = QDir(directory.path()).filePath(QStringLiteral("paced.bin"));

  qint64 lastReceived = -1;
  qint64 lastTotal = -1;
  qint64 previous = 0;
  bool monotonic = true;
  const auto progress = [&](qint64 received, qint64 total) {
    if (received < previous) monotonic = false;
    previous = received;
    lastReceived = received;
    lastTotal = total;
  };

  QString reason;
  const bool ok = downloadToFile(server.url(), destination, body.size(), sha256Of(body), progress,
                                 {}, &reason);
  QVERIFY2(ok, qPrintable(reason));
  QVERIFY2(lastReceived >= 0, "progress was never reported");
  QCOMPARE(lastReceived, static_cast<qint64>(body.size()));
  QCOMPARE(lastTotal, static_cast<qint64>(body.size()));
  QVERIFY2(monotonic, "progress went backwards");
  // The reported size is what the file actually holds: a callback that reported
  // the stream's byte would still be a lie if the disk disagreed.
  QCOMPARE(readFile(destination).size(), lastReceived);
}

void TestDownloads::anUnexpectedDigestRefusesTheFileAndRemovesIt() {
  const QByteArray body = payload();
  LoopbackServer server(body);
  QString error;
  QVERIFY2(server.start(&error), qPrintable(error));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString destination = QDir(directory.path()).filePath(QStringLiteral("badDigest.bin"));

  QString reason;
  const bool ok = downloadToFile(server.url(), destination, body.size(),
                                 QStringLiteral("0").repeated(64), {}, {}, &reason);

  QVERIFY2(!ok, qPrintable(reason));
  QVERIFY2(!reason.isEmpty(), "a digest mismatch has to be named");
  QVERIFY2(reason.contains(QStringLiteral("sha"), Qt::CaseInsensitive) ||
               reason.contains(QStringLiteral("digest"), Qt::CaseInsensitive),
           qPrintable(QStringLiteral("unexpected failure reason: %1").arg(reason)));
  QVERIFY2(!QFileInfo::exists(destination),
           "a file whose digest did not verify was left on disk");
}

void TestDownloads::anUnexpectedSizeRefusesTheFileAndRemovesIt() {
  const QByteArray body = payload();
  LoopbackServer server(body);
  QString error;
  QVERIFY2(server.start(&error), qPrintable(error));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString destination = QDir(directory.path()).filePath(QStringLiteral("badSize.bin"));

  QString reason;
  const bool ok = downloadToFile(server.url(), destination, body.size() + 7, sha256Of(body), {}, {},
                                 &reason);

  QVERIFY2(!ok, qPrintable(reason));
  QVERIFY2(!reason.isEmpty(), "a size mismatch has to be named");
  QVERIFY2(reason.contains(QStringLiteral("size"), Qt::CaseInsensitive),
           qPrintable(QStringLiteral("unexpected failure reason: %1").arg(reason)));
  QVERIFY2(!QFileInfo::exists(destination),
           "a short transfer left a file behind that claims to be complete");
}

void TestDownloads::anUnwritableDestinationFailsWithoutClaimingSuccess() {
  const QByteArray body = payload();
  LoopbackServer server(body);
  QString error;
  QVERIFY2(server.start(&error), qPrintable(error));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());

  // A destination whose parent is a file: nothing can be created there, and the
  // failure has to come back as a failure rather than a success with no bytes.
  const QString blocker = QDir(directory.path()).filePath(QStringLiteral("blocker"));
  QVERIFY(writeFile(blocker, QByteArrayLiteral("not a directory\n")));
  const QString underAFile = QDir(blocker).filePath(QStringLiteral("payload.bin"));
  QString firstReason;
  const bool firstOk = downloadToFile(server.url(), underAFile, body.size(), sha256Of(body), {}, {},
                                      &firstReason);
  QVERIFY2(!firstOk, qPrintable(firstReason));
  QVERIFY2(!firstReason.isEmpty(), "an unwritable destination has to be explained");
  QVERIFY2(!QFileInfo::exists(underAFile), "a failed destination produced a file");
  QVERIFY2(readFile(blocker) == QByteArrayLiteral("not a directory\n"),
           "the failure damaged the file that blocked the write");

  // A destination whose parent directory was never made: same contract, and the
  // reason names the path a caller would have to fix.
  const QString missingParent =
      QDir(directory.path()).filePath(QStringLiteral("absent/folder/payload.bin"));
  QString secondReason;
  const bool secondOk = downloadToFile(server.url(), missingParent, body.size(), sha256Of(body),
                                       {}, {}, &secondReason);
  QVERIFY2(!secondOk, qPrintable(secondReason));
  QVERIFY2(!secondReason.isEmpty(), "a missing parent directory has to be explained");
  QVERIFY2(secondReason.contains(QStringLiteral("payload.bin")),
           qPrintable(QStringLiteral("the reason does not name the destination: %1")
                          .arg(secondReason)));
  QVERIFY2(!QFileInfo::exists(missingParent), "a failed destination produced a file");

  // A destination that is itself a directory: whatever guard catches it first,
  // the refusal has to name the path and the directory has to survive. The
  // destination here was not created by this invocation, so nothing it owns is
  // removable, and a create that truncates in place is exactly the bug.
  const QString occupied = QDir(directory.path()).filePath(QStringLiteral("occupied"));
  QVERIFY(QDir().mkpath(occupied));
  const QByteArray insideIt = QByteArrayLiteral("the directory's own contents\n");
  const QString insideFile = QDir(occupied).filePath(QStringLiteral("inner.txt"));
  QVERIFY(writeFile(insideFile, insideIt));

  QString thirdReason;
  const bool thirdOk =
      downloadToFile(server.url(), occupied, body.size(), sha256Of(body), {}, {}, &thirdReason);
  QVERIFY2(!thirdOk, qPrintable(thirdReason));
  QVERIFY2(!thirdReason.isEmpty(), "a destination that is a directory has to be explained");
  QVERIFY2(thirdReason.contains(QStringLiteral("occupied")),
           qPrintable(QStringLiteral("the reason does not name the destination: %1")
                          .arg(thirdReason)));
  QVERIFY2(QFileInfo(occupied).isDir(), "the refused destination was replaced");
  QCOMPARE(readFile(insideFile), insideIt);
}

void TestDownloads::cancellingMidTransferRemovesWhatWasWritten() {
  const QByteArray body = payload();
  LoopbackServer server(body);
  server.setChunkBytes(256);
  server.setDelayMs(20);
  QString error;
  QVERIFY2(server.start(&error), qPrintable(error));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString destination = QDir(directory.path()).filePath(QStringLiteral("cancelled.bin"));

  // Cancel only once real bytes are in flight, so the transfer being abandoned is
  // the one under test rather than one that had not started.
  const auto cancelled = [&server] { return server.chunksServed() >= 3; };

  QString reason;
  const bool ok = downloadToFile(server.url(), destination, body.size(), sha256Of(body), {},
                                 cancelled, &reason);

  QVERIFY2(!ok, qPrintable(reason));
  QVERIFY2(!QFileInfo::exists(destination),
           "cancelling left a partial file that reads as a complete download");
  QCOMPARE(server.requestsServed(), 1);
}

void TestDownloads::aCancelledDownloadReportsThatItWasCancelled() {
  const QByteArray body = payload();
  LoopbackServer server(body);
  server.setChunkBytes(256);
  server.setDelayMs(20);
  QString error;
  QVERIFY2(server.start(&error), qPrintable(error));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString destination = QDir(directory.path()).filePath(QStringLiteral("stopped.bin"));

  bool cancelNow = false;
  const auto cancelled = [&cancelNow] { return cancelNow; };

  QString reason;
  // Arm the cancellation from a timer on the same event loop the transfer runs
  // on: the poll inside downloadToFile observes it without any test-side sleep.
  QTimer arm;
  arm.setSingleShot(true);
  QObject::connect(&arm, &QTimer::timeout, [&cancelNow] { cancelNow = true; });
  arm.start(120);

  const bool ok = downloadToFile(server.url(), destination, body.size(), sha256Of(body), {},
                                 cancelled, &reason);
  arm.stop();

  QVERIFY2(!ok, qPrintable(reason));
  QVERIFY2(reason.contains(QStringLiteral("cancel"), Qt::CaseInsensitive),
           qPrintable(QStringLiteral("a cancelled transfer is reported as: %1").arg(reason)));
  QVERIFY2(!QFileInfo::exists(destination), "cancelling left the partial file behind");
}

void TestDownloads::cancellingTheFinalLocalChunkRemovesTheCopy() {
  const QByteArray body = payload();
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString source = QDir(directory.path()).filePath(QStringLiteral("source.bin"));
  const QString destination = QDir(directory.path()).filePath(QStringLiteral("copy.bin"));
  QVERIFY(writeFile(source, body));

  std::atomic_bool cancelled{false};
  const mikai::backend::WorkCancellationScope scope(&cancelled);
  QString reason;
  const bool ok = downloadToFile(
      QUrl::fromLocalFile(source), destination, body.size(), sha256Of(body),
      [&cancelled](qint64 received, qint64 total) {
        if (received == total) cancelled.store(true);
      },
      {}, &reason);

  QVERIFY(!ok);
  QVERIFY(reason.contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
  QVERIFY(!QFileInfo::exists(destination));
  QCOMPARE(readFile(source), body);
}

void TestDownloads::cancellingAQuietFetchReturnsWithoutWaitingForAnotherSignal() {
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
        // Intentionally no response: cancellation must stop a quiet reply.
      });
    }
  });
  const mikai::backend::WorkCancellationScope scope(&cancelled);
  QElapsedTimer elapsed;
  elapsed.start();
  bool failed = false;
  try {
    mikai::backend::fetchUrlBytes(
        QUrl(QStringLiteral("http://127.0.0.1:%1/quiet").arg(server.serverPort())), {}, 5000);
  } catch (const std::runtime_error&) {
    failed = true;
  }
  QVERIFY(requested);
  QVERIFY(failed);
  QVERIFY2(elapsed.elapsed() < 2000, "cancellation waited for the original request timeout");
}

int runTestDownloads(int argc, char** argv) {
  TestDownloads suite;
  QStringList arguments;
  arguments.reserve(argc + 1);
  arguments << QString::fromLocal8Bit(argv[0]);
  for (int i = 1; i < argc; ++i) arguments << QString::fromLocal8Bit(argv[i]);
  QTemporaryFile report;
  const bool captureReport = !arguments.contains(QStringLiteral("-o"));
  if (captureReport) {
    if (!report.open()) {
      std::fprintf(stderr, "[mikai_tests] cannot create report for TestDownloads\n");
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
      std::fprintf(stderr, "[mikai_tests] cannot read report for TestDownloads\n");
    }
  }
  std::fflush(stderr);
  return rc;
}
