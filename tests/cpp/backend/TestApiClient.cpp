#include "TestApiClient.hpp"

#include "TestSupport.hpp"

#include <backend/Errors.hpp>
#include <backend/MikaiClient.hpp>
#include <backend/Models.hpp>

#include <atomic>
#include <cstdio>

#include <QtCore/QByteArray>
#include <QtCore/QDeadlineTimer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMap>
#include <QtCore/QMutex>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryFile>
#include <QtCore/QThread>
#include <QtCore/QVariant>
#include <QtCore/QtGlobal>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

using mikai::backend::ApiErrorEnvelope;
using mikai::backend::ApiKeySaveResult;
using mikai::backend::isHttpStatusError;
using mikai::backend::MikaiApiError;
using mikai::backend::MikaiClient;
using mikai::backend::parseApiErrorEnvelope;
using mikai::backend::SearchResult;
using mikai::backend::setEnvFilePathOverride;
using mikai_test::asText;

namespace {

// One request the server has seen, read as it arrived. The header spellings
// matter to a credential assertion, so the test reads the request header-by-
// header rather than a normalised copy of it.
struct CapturedRequest {
  QMap<QString, QString> headers;
};

// A loopback endpoint that answers one request at a time with a queued reply.
// The queue is what makes the ordering questions testable: a caller can be left
// in flight while the test changes a key underneath it, without any guess about
// how long the server takes.
class LoopbackApi : public QObject {
 public:
  LoopbackApi(QObject* parent = nullptr) : QObject(parent) {}

  ~LoopbackApi() override {
    for (QTcpSocket* socket : sockets_) {
      socket->abort();
      socket->deleteLater();
    }
    server_.close();
  }

  bool start(QString* error) {
    if (!server_.listen(QHostAddress::LocalHost, 0)) {
      if (error != nullptr) *error = server_.errorString();
      return false;
    }
    connect(&server_, &QTcpServer::newConnection, this, &LoopbackApi::onNewConnection);
    return true;
  }

  QString baseUrl() const {
    return QStringLiteral("http://127.0.0.1:%1/public/v1").arg(server_.serverPort());
  }

  // The quota headers a reply carries: they are how the limiter is told the
  // window is closed, which is what parks a caller without holding a socket
  // open.
  static QMap<QString, QString> quota(int limit, int remaining, int reset) {
    QMap<QString, QString> headers;
    headers.insert(QStringLiteral("X-RateLimit-Limit"), QString::number(limit));
    headers.insert(QStringLiteral("X-RateLimit-Remaining"), QString::number(remaining));
    headers.insert(QStringLiteral("X-RateLimit-Reset"), QString::number(reset));
    return headers;
  }

  void enqueue(int status, const QByteArray& body, const QMap<QString, QString>& headers = {}) {
    QMutexLocker locker(&mutex_);
    pending_.push_back(Reply{status, body, headers});
  }

  int requestCount() const {
    QMutexLocker locker(&mutex_);
    return captured_.size();
  }

  QString headerOf(int index, const QString& name) const {
    QMutexLocker locker(&mutex_);
    if (index < 0 || index >= captured_.size()) return {};
    const CapturedRequest request = captured_.at(index);
    for (auto it = request.headers.cbegin(); it != request.headers.cend(); ++it) {
      if (it.key().compare(name, Qt::CaseInsensitive) == 0) return it.value();
    }
    return {};
  }

 private:
  struct Reply {
    int status = 200;
    QByteArray body;
    QMap<QString, QString> headers;
  };

  // Everything that belongs to one TCP connection, keyed by its socket only
  // while that socket is alive. The record is dropped the moment the connection
  // ends: the allocator can hand a later connection the address of a socket
  // that has already been destroyed, and state left under that address would
  // read the new request as one already served, so it would never be answered.
  struct Connection {
    QByteArray buffer;
    bool served = false;
  };

  void onNewConnection() {
    while (QTcpSocket* socket = server_.nextPendingConnection()) {
      sockets_.append(socket);
      connections_.insert(socket, Connection{});
      connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
      connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
        sockets_.removeOne(socket);
        connections_.remove(socket);
        socket->deleteLater();
      });
    }
  }

  void onReadyRead(QTcpSocket* socket) {
    const auto it = connections_.find(socket);
    if (it == connections_.end()) return;
    Connection& connection = *it;
    connection.buffer += socket->readAll();
    const int headerEnd = connection.buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0 || connection.served) return;
    connection.served = true;

    CapturedRequest request;
    const QList<QByteArray> lines = connection.buffer.left(headerEnd).split('\n');
    for (const QByteArray& line : lines.mid(1)) {
      const int colon = line.indexOf(':');
      if (colon <= 0) continue;
      const QString name = QString::fromLatin1(line.left(colon)).trimmed();
      const QString value = QString::fromLatin1(line.mid(colon + 1)).trimmed();
      if (!name.isEmpty()) request.headers.insert(name, value);
    }

    Reply reply;
    {
      QMutexLocker locker(&mutex_);
      captured_.append(request);
      if (pending_.isEmpty()) {
        reply = Reply{404,
                      "{\"ok\":false,\"error\":{\"code\":\"not_found\","
                      "\"message\":\"no reply queued\"}}",
                      {}};
      } else {
        reply = pending_.takeFirst();
      }
    }

    QByteArray response;
    response += "HTTP/1.1 " + QByteArray::number(reply.status) + " ";
    response += reasonPhrase(reply.status).toLatin1() + "\r\n";
    response += "Content-Type: application/json\r\n";
    for (auto it = reply.headers.cbegin(); it != reply.headers.cend(); ++it) {
      response += it.key().toLatin1() + ": " + it.value().toLatin1() + "\r\n";
    }
    response += "Content-Length: " + QByteArray::number(reply.body.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += reply.body;
    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
  }

  static QString reasonPhrase(int status) {
    switch (status) {
      case 200: return QStringLiteral("OK");
      case 400: return QStringLiteral("Bad Request");
      case 403: return QStringLiteral("Forbidden");
      case 404: return QStringLiteral("Not Found");
      case 422: return QStringLiteral("Unprocessable Entity");
      case 429: return QStringLiteral("Too Many Requests");
      case 500: return QStringLiteral("Internal Server Error");
      default: return QStringLiteral("Status");
    }
  }

  QTcpServer server_;
  mutable QMutex mutex_;
  QList<Reply> pending_;
  QVector<CapturedRequest> captured_;
  QHash<QTcpSocket*, Connection> connections_;
  QList<QTcpSocket*> sockets_;
};

// The envelope the API answers a normal list request with, shaped the way the
// parser reads it: an "ok" window around a "result" array.
QByteArray okListPayload() {
  return QByteArrayLiteral(R"({"ok":true,"page":1,"pages":1,"total":1,"result":[
      {"ids":{"mikai":"7428"},
       "titles":{"english":"Black Torch"},
       "year":2023,"format":"tv","status":"released","episodes":12}
    ]})");
}

QByteArray failurePayload(const QString& code, const QString& message) {
  return QByteArrayLiteral(R"({"ok":false,"error":{"code":")") +
         code.toUtf8() + QByteArrayLiteral(R"(","message":")") + message.toUtf8() +
         QByteArrayLiteral("\"}}");
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

constexpr auto kOldKey = "mk_old_key_synthetic";
constexpr auto kNewKey = "mk_new_key_synthetic";

// Runs a client call on its own thread and reports how it ended, so a parked
// caller can be observed while the test thread keeps its event loop running.
class CallOutcome {
 public:
  explicit CallOutcome(MikaiClient* client, const QString& query)
      : client_(client), query_(query) {}

  CallOutcome(const CallOutcome&) = delete;
  CallOutcome& operator=(const CallOutcome&) = delete;

  ~CallOutcome() {
    // The worker points at a client that lives on the test's stack, so it must
    // not outlive the test function. A failing assertion returns early, while
    // the call may still be parked or in flight; joining here ends that call
    // (the client bounds each request with its own deadline) instead of leaving
    // a thread behind with a dangling client.
    if (thread_ != nullptr) {
      thread_->wait();
      delete thread_;
    }
  }

  void start() {
    thread_ = QThread::create([this] { run(); });
    thread_->start();
  }

  // Waits for the call to end. Used to prove the first request completed before
  // the second one parks, without guessing how long the server takes.
  void waitForFinished(int timeoutMs = 15000) {
    QDeadlineTimer deadline(timeoutMs);
    while (!finished_ && deadline.remainingTime() > 0) {
      QTest::qWait(5);
    }
  }

  bool finished() const { return finished_; }
  bool threw() const { return threw_; }
  const QString& code() const { return code_; }

 private:
  void run() {
    try {
      client_->search(query_);
    } catch (const MikaiApiError& error) {
      threw_ = true;
      code_ = error.code;
    } catch (...) {
      threw_ = true;
      code_ = QStringLiteral("(not a MikaiApiError)");
    }
    finished_ = true;
  }

  MikaiClient* client_ = nullptr;
  QString query_;
  QThread* thread_ = nullptr;
  std::atomic<bool> finished_{false};
  bool threw_ = false;
  QString code_;
};

}  // namespace

void TestApiClient::initTestCase() {
  // The suite writes a .env; the client walks up from the executable directory
  // to find one, which would find the repository's. Point it at a scratch file
  // for the whole suite so nothing real is read or written.
  QVERIFY2(envDirectory_.isValid(), "cannot create the isolated .env directory");
  setEnvFilePathOverride(QDir(envDirectory_.path()).filePath(QStringLiteral(".env")));
}

void TestApiClient::cleanupTestCase() { setEnvFilePathOverride(QString()); }

void TestApiClient::aDisabledKeyFailureKeepsItsOwnCodeAndMessage() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  api.enqueue(403, failurePayload(QStringLiteral("api_key_disabled"), QString::fromUtf8("\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87 \xD0\xB2\xD0\xB8\xD0\xBC\xD0\xBA\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xBE")));

  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  try {
    client.search(QStringLiteral("torch"));
    QFAIL("a disabled-key reply must not report success");
  } catch (const MikaiApiError& caught) {
    QCOMPARE(caught.code, QStringLiteral("api_key_disabled"));
    QCOMPARE(caught.status, 403);
    QCOMPARE(caught.message, QString::fromUtf8("\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87 \xD0\xB2\xD0\xB8\xD0\xBC\xD0\xBA\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xBE"));
  }
}

void TestApiClient::aMissingResourceFailureKeepsItsStatus() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  api.enqueue(404, failurePayload(QStringLiteral("not_found"), QString::fromUtf8("\xD1\x80\xD0\xB5\xD1\x81\xD1\x83\xD1\x80\xD1\x81 \xD0\xBD\xD0\xB5 \xD0\xB7\xD0\xBD\xD0\xB0\xD0\xB9\xD0\xB4\xD0\xB5\xD0\xBD\xD0\xBE")));

  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  try {
    client.getAnime(QStringLiteral("0"));
    QFAIL("a missing resource must not report success");
  } catch (const MikaiApiError& caught) {
    QCOMPARE(caught.code, QStringLiteral("not_found"));
    QCOMPARE(caught.status, 404);
    QCOMPARE(caught.message, QString::fromUtf8("\xD1\x80\xD0\xB5\xD1\x81\xD1\x83\xD1\x80\xD1\x81 \xD0\xBD\xD0\xB5 \xD0\xB7\xD0\xBD\xD0\xB0\xD0\xB9\xD0\xB4\xD0\xB5\xD0\xBD\xD0\xBE"));
  }
}

void TestApiClient::aMalformedSuccessEnvelopeFailsRatherThanParsingGarbage() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  // The server answered 200 with something that is not the envelope. The client
  // has to refuse, not invent an empty result that reads as "nothing found".
  api.enqueue(200, QByteArrayLiteral("<html>503 service unavailable</html>"));

  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  try {
    client.search(QStringLiteral("torch"));
    QFAIL("a non-envelope body must not report success");
  } catch (const MikaiApiError& caught) {
    QVERIFY2(!caught.code.isEmpty(), "a malformed reply must still name a reason");
    QVERIFY2(!caught.message.isEmpty(), "a malformed reply must still carry a message");
  }
}

void TestApiClient::fetchBytesReportsLoopbackDownloadProgress() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  const QByteArray body(4 * 1024 * 1024, 'x');
  api.enqueue(200, body);
  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  QVector<QPair<qint64, qint64>> updates;
  const QByteArray received = client.fetchBytes(
      api.baseUrl() + QStringLiteral("/large"), -1,
      [&updates](qint64 bytesReceived, qint64 bytesTotal) {
        updates.append(qMakePair(bytesReceived, bytesTotal));
      });
  QCOMPARE(received, body);
  QVERIFY2(!updates.isEmpty(), "fetchBytes did not forward QNetworkReply download progress");
  qint64 previous = 0;
  for (const auto& update : updates) {
    QVERIFY(update.first >= previous);
    QVERIFY(update.first <= body.size());
    QCOMPARE(update.second, qint64(body.size()));
    previous = update.first;
  }
  QCOMPARE(previous, qint64(body.size()));
}
void TestApiClient::aPlainEnvelopeSuccessParsesIntoTheResult() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  api.enqueue(200, okListPayload());

  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  const SearchResult result = client.search(QStringLiteral("torch"));
  QCOMPARE(result.items.size(), 1);
  QCOMPARE(asText(result.items.first().titles.english), QStringLiteral("Black Torch"));
}

void TestApiClient::aValidateCallKeepsTheEnvelopeItWasGiven() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  api.enqueue(403, failurePayload(QStringLiteral("api_key_disabled"), QString::fromUtf8("\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87 \xD0\xB2\xD0\xB8\xD0\xBC\xD0\xBA\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xBE")));

  // A candidate key is validated against the server; the reply is an envelope
  // failure, so the reported reason is the API's own rather than a transport
  // failure the settings dialog cannot act on.
  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  const QMap<QString, QVariant> verdict = client.validateApiKey(QStringLiteral("mk_candidate_synthetic"));
  QCOMPARE(verdict.value(QStringLiteral("valid")).toBool(), false);
  QCOMPARE(verdict.value(QStringLiteral("error_code")).toString(),
           QStringLiteral("api_key_disabled"));
  QCOMPARE(verdict.value(QStringLiteral("error")).toString(), QString::fromUtf8("\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87 \xD0\xB2\xD0\xB8\xD0\xBC\xD0\xBA\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xBE"));
}

void TestApiClient::aTransportFailureIsNotAnApiFailure() {
  // A port with nothing listening is the transport failure shape the client has
  // to keep distinct from "the API answered no".
  MikaiClient client(QString::fromLatin1(kOldKey), QStringLiteral("http://127.0.0.1:1/public/v1"));
  try {
    client.search(QStringLiteral("torch"));
    QFAIL("an unreachable server must not report success");
  } catch (const MikaiApiError& caught) {
    QCOMPARE(caught.code, QStringLiteral("network_error"));
    QCOMPARE(caught.status, 0);
    QVERIFY2(!caught.message.isEmpty(), "a transport failure must still carry a message");
  }
}

void TestApiClient::aKeyChangedWhileARequestWasParkedDoesNotSendTheOldKey() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));

  // The first answer closes the quota window for a second, which parks the next
  // caller inside the limiter - before it has built or sent anything. That is
  // the moment the key is changed underneath it.
  api.enqueue(200, okListPayload(), LoopbackApi::quota(300, 0, 1));
  api.enqueue(200, okListPayload(), LoopbackApi::quota(300, 299, 60));

  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());

  CallOutcome first(&client, QStringLiteral("torch"));
  first.start();
  QTRY_VERIFY_WITH_TIMEOUT(api.requestCount() >= 1, 15000);
  QCOMPARE(api.headerOf(0, QStringLiteral("X-API-Key")), QString::fromLatin1(kOldKey));

  // The window is now closed for a second, so the next caller parks before it
  // builds or sends anything. That is the moment the key is changed underneath
  // it, and the parking is observed through the limiter rather than a socket
  // held open, so the ordering does not depend on any sleep.
  first.waitForFinished();
  QVERIFY2(!first.threw(), qPrintable(first.code()));
  CallOutcome parked(&client, QStringLiteral("torch"));
  parked.start();
  QTRY_VERIFY_WITH_TIMEOUT(client.rateLimitState().queued >= 1, 15000);
  QVERIFY2(api.requestCount() < 2, "a parked caller was sent instead of being parked");

  const ApiKeySaveResult saved = client.setApiKey(QString::fromLatin1(kNewKey));
  QVERIFY2(saved.ok, qPrintable(saved.reason));
  QCOMPARE(client.apiKey(), QString::fromLatin1(kNewKey));

  // The parked call resumes in the new epoch. It sends with the credential that
  // is current when the request goes out, and not with the one it arrived with.
  QTRY_VERIFY_WITH_TIMEOUT(api.requestCount() >= 2, 15000);
  QCOMPARE(api.headerOf(1, QStringLiteral("X-API-Key")), QString::fromLatin1(kNewKey));
  parked.waitForFinished();
  QVERIFY2(parked.finished(), "the parked request never completed");
  QVERIFY2(!parked.threw(), qPrintable(parked.code()));
  QTRY_VERIFY_WITH_TIMEOUT(client.rateLimitState().queued == 0, 5000);
}

void TestApiClient::aSavedKeyIsReportedAsSaved() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = QDir(directory.path()).filePath(QStringLiteral(".env"));
  setEnvFilePathOverride(path);

  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  const ApiKeySaveResult result = client.setApiKey(QString::fromLatin1(kNewKey));
  QVERIFY2(result.ok, qPrintable(result.reason));
  QVERIFY2(result.reason.isEmpty(), qPrintable(result.reason));
  QCOMPARE(client.apiKey(), QString::fromLatin1(kNewKey));

  // The point of the whole exercise: the key is on disk, so it survives the
  // restart a dialog user assumed it survived.
  QVERIFY2(QFileInfo::exists(path), "the save reported success but wrote no file");
  const QByteArray contents = readFile(path);
  QVERIFY2(contents.contains(QByteArrayLiteral("mk_new_key_synthetic")),
           qPrintable(QStringLiteral("the saved .env does not carry the key: %1")
                          .arg(QString::fromUtf8(contents))));
  setEnvFilePathOverride(QString());
}

void TestApiClient::aKeyThatCouldNotBeSavedNamesTheReasonAndKeepsTheOldOne() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  // A path whose parent is a file: the atomic write can neither create its
  // temporary sibling nor commit, which is the shape of a full or unavailable
  // disk.
  const QString blocker = QDir(directory.path()).filePath(QStringLiteral("blocker"));
  QVERIFY(writeFile(blocker, QByteArrayLiteral("not a directory\n")));
  const QString unreachable = QDir(blocker).filePath(QStringLiteral(".env"));
  setEnvFilePathOverride(unreachable);

  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  const ApiKeySaveResult result = client.setApiKey(QString::fromLatin1(kNewKey));
  QVERIFY2(!result.ok, "a save that could not write reported success");
  QVERIFY2(!result.reason.isEmpty(), "a failed save carries no reason");
  QVERIFY2(result.reason.contains(QStringLiteral(".env")),
           qPrintable(QStringLiteral("the reason does not name the file: %1").arg(result.reason)));
  // The credential in force is the old one: a failed save is not a save.
  QCOMPARE(client.apiKey(), QString::fromLatin1(kOldKey));
  QVERIFY2(readFile(blocker) == QByteArrayLiteral("not a directory\n"),
           "a failed save damaged the file that blocked it");
  setEnvFilePathOverride(QString());
}

void TestApiClient::clearingAKeyEmptiesTheStoredFileAndReportsIt() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = QDir(directory.path()).filePath(QStringLiteral(".env"));
  setEnvFilePathOverride(path);
  QVERIFY(writeFile(path, QByteArrayLiteral("X-API-Key: mk_previous_key_synthetic\n")));

  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  const ApiKeySaveResult result = client.setApiKey(QString());
  QVERIFY2(result.ok, qPrintable(result.reason));
  QVERIFY2(client.apiKey().isEmpty(), "clearing the key left a key in memory");
  QCOMPARE(readFile(path), QByteArray());
}

void TestApiClient::anEnvelopeFailureDecodesItsNamedReason() {
  ApiErrorEnvelope envelope;
  QVERIFY2(parseApiErrorEnvelope(
               failurePayload(QStringLiteral("api_key_disabled"), QString::fromUtf8("\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87 \xD0\xB2\xD0\xB8\xD0\xBC\xD0\xBA\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xBE")),
               403, &envelope),
           "a valid envelope did not decode");
  QCOMPARE(envelope.code, QStringLiteral("api_key_disabled"));
  QCOMPARE(envelope.status, 403);
  QCOMPARE(envelope.message, QString::fromUtf8("\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87 \xD0\xB2\xD0\xB8\xD0\xBC\xD0\xBA\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xBE"));
  QVERIFY2(isHttpStatusError(403), "a 403 is not an error");
  QVERIFY2(!isHttpStatusError(200), "a 200 is an error");

  ApiErrorEnvelope garbage;
  QVERIFY2(!parseApiErrorEnvelope(QByteArrayLiteral("<html>oops</html>"), 500, &garbage),
           "a non-envelope body decoded as an envelope");
}

int runTestApiClient(int argc, char** argv) {
  TestApiClient suite;
  QStringList arguments;
  arguments.reserve(argc + 1);
  arguments << QString::fromLocal8Bit(argv[0]);
  for (int i = 1; i < argc; ++i) arguments << QString::fromLocal8Bit(argv[i]);
  QTemporaryFile report;
  const bool captureReport = !arguments.contains(QStringLiteral("-o"));
  if (captureReport) {
    if (!report.open()) {
      std::fprintf(stderr, "[mikai_tests] cannot create report for TestApiClient\n");
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
      std::fprintf(stderr, "[mikai_tests] cannot read report for TestApiClient\n");
    }
  }
  std::fflush(stderr);
  return rc;
}
