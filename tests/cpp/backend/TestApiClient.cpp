#include "TestApiClient.hpp"

#include "TestSupport.hpp"

#include <backend/Cancellation.hpp>
#include <backend/Errors.hpp>
#include <backend/MikaiClient.hpp>
#include <backend/Models.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <utility>

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDeadlineTimer>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QMutex>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryFile>
#include <QtCore/QThread>
#include <QtCore/QVariant>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
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
using mikai::backend::AnimeQuery;
using mikai::backend::ApiMeta;
using mikai::backend::GenreTag;
using mikai::backend::TeamProfile;
using mikai::backend::TeamsResult;
using mikai::backend::UpdatesResult;
using mikai::backend::setEnvFilePathOverride;
using mikai_test::asText;

namespace {

struct CapturedRequest {
  QString target;
  QMap<QString, QString> headers;
};

struct WireReply {
  int status = 200;
  QByteArray body;
  QMap<QString, QString> headers;
};

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

  // Remaining = 0 closes the limiter window: the next caller parks before it
  // sends, without a socket held open.
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

  void setRoute(std::function<WireReply(const CapturedRequest&)> route) {
    QMutexLocker locker(&mutex_);
    route_ = std::move(route);
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
  // QNetworkAccessManager normalises the canonical header case ("X-Api-Key"), so
  // a raw lookup by the documented spelling misses the credential that was sent.
  static QString valueOf(const CapturedRequest& request, const QString& name) {
    for (auto it = request.headers.cbegin(); it != request.headers.cend(); ++it) {
      if (it.key().compare(name, Qt::CaseInsensitive) == 0) return it.value();
    }
    return {};
  }
  QString targetOf(int index) const {
    QMutexLocker locker(&mutex_);
    return index >= 0 && index < captured_.size() ? captured_.at(index).target : QString();
  }

 private:
  struct Reply {
    int status = 200;
    QByteArray body;
    QMap<QString, QString> headers;
  };

  // Keyed by socket only while it lives: a destroyed socket's address can be
  // reused, and leftover state would read the new request as already served.
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
    if (!lines.isEmpty()) {
      const QList<QByteArray> first = lines.first().split(' ');
      if (first.size() >= 2) request.target = QString::fromLatin1(first.at(1));
    }
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
      if (route_) {
        const WireReply routed = route_(request);
        reply = Reply{routed.status, routed.body, routed.headers};
      } else if (pending_.isEmpty()) {
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
  std::function<WireReply(const CapturedRequest&)> route_;
  QVector<CapturedRequest> captured_;
  QHash<QTcpSocket*, Connection> connections_;
  QList<QTcpSocket*> sockets_;
};

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

struct CatalogueRow {
  QString title;
  int year = 0;
  QString format;
  QString status;
  QString season;
  QString studio;
  QStringList genres;
  QStringList tags;
  qint64 team = 0;
  int views = 0;
};

const QVector<CatalogueRow>& catalogueRows() {
  static const QVector<CatalogueRow> rows = {
      {QStringLiteral("Action School"), 2020, QStringLiteral("tv"), QStringLiteral("ongoing"),
       QStringLiteral("winter"), QStringLiteral("Alpha"), {QStringLiteral("Action")},
       {QStringLiteral("school")}, 1, 30},
      {QStringLiteral("Comedy Spoiler"), 2024, QStringLiteral("movie"), QStringLiteral("finished"),
       QStringLiteral("summer"), QStringLiteral("Beta"), {QStringLiteral("Comedy")},
       {QStringLiteral("spoiler")}, 2, 10},
      {QStringLiteral("Action Comedy School"), 2022, QStringLiteral("tv"),
       QStringLiteral("ongoing"), QStringLiteral("winter"), QStringLiteral("Alpha"),
       {QStringLiteral("Action"), QStringLiteral("Comedy")},
       {QStringLiteral("school"), QStringLiteral("spoiler")}, 1, 20},
  };
  return rows;
}

QStringList csvValues(const QString& value) {
  QStringList values;
  for (const QString& part : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    const QString trimmed = part.trimmed();
    if (!trimmed.isEmpty()) values << trimmed;
  }
  return values;
}

bool anyOf(const QStringList& values, const QStringList& wanted) {
  for (const QString& value : wanted) {
    if (values.contains(value)) return true;
  }
  return false;
}

bool allOf(const QStringList& values, const QStringList& wanted) {
  for (const QString& value : wanted) {
    if (!values.contains(value)) return false;
  }
  return true;
}

WireReply catalogueRoute(const CapturedRequest& request) {
  const QUrlQuery query(QUrl(request.target).query());
  const QString search = query.queryItemValue(QStringLiteral("search")).trimmed();
  const QStringList genres = csvValues(query.queryItemValue(QStringLiteral("genres")));
  const QStringList genresExclude = csvValues(query.queryItemValue(QStringLiteral("genresExclude")));
  const QString genresMatch = query.queryItemValue(QStringLiteral("genresMatch"));
  const QStringList tags = csvValues(query.queryItemValue(QStringLiteral("tags")));
  const QStringList tagsExclude = csvValues(query.queryItemValue(QStringLiteral("tagsExclude")));
  const QString tagsMatch = query.queryItemValue(QStringLiteral("tagsMatch"));
  const QStringList years = csvValues(query.queryItemValue(QStringLiteral("years")));
  const int yearFrom = query.queryItemValue(QStringLiteral("yearFrom")).toInt();
  const int yearTo = query.queryItemValue(QStringLiteral("yearTo")).toInt();
  const QStringList seasons = csvValues(query.queryItemValue(QStringLiteral("seasons")));
  const QStringList formats = csvValues(query.queryItemValue(QStringLiteral("formats")));
  const QStringList statuses = csvValues(query.queryItemValue(QStringLiteral("statuses")));
  const QStringList studios = csvValues(query.queryItemValue(QStringLiteral("studios")));
  const QStringList teams = csvValues(query.queryItemValue(QStringLiteral("teams")));

  QVector<const CatalogueRow*> matched;
  for (const CatalogueRow& row : catalogueRows()) {
    if (!search.isEmpty() && !row.title.contains(search, Qt::CaseInsensitive)) continue;
    if (!genres.isEmpty()) {
      const bool keep = genresMatch == QLatin1String("any") ? anyOf(row.genres, genres)
                                                             : allOf(row.genres, genres);
      if (!keep) continue;
    }
    if (anyOf(row.genres, genresExclude)) continue;
    if (!tags.isEmpty()) {
      const bool keep = tagsMatch == QLatin1String("any") ? anyOf(row.tags, tags) : allOf(row.tags, tags);
      if (!keep) continue;
    }
    if (anyOf(row.tags, tagsExclude)) continue;
    if (!years.isEmpty() && !years.contains(QString::number(row.year))) continue;
    if (yearFrom > 0 && row.year < yearFrom) continue;
    if (yearTo > 0 && row.year > yearTo) continue;
    if (!seasons.isEmpty() && !seasons.contains(row.season)) continue;
    if (!formats.isEmpty() && !formats.contains(row.format)) continue;
    if (!statuses.isEmpty() && !statuses.contains(row.status)) continue;
    if (!studios.isEmpty() && !studios.contains(row.studio)) continue;
    if (!teams.isEmpty() && !teams.contains(QString::number(row.team))) continue;
    matched.append(&row);
  }

  const QString sort = query.queryItemValue(QStringLiteral("sort"), QUrl::PrettyDecoded);
  const bool descending = query.queryItemValue(QStringLiteral("order")) == QLatin1String("desc");
  std::stable_sort(matched.begin(), matched.end(), [&](const CatalogueRow* left, const CatalogueRow* right) {
    int order = 0;
    if (sort == QLatin1String("year")) {
      order = left->year - right->year;
    } else if (sort == QLatin1String("views")) {
      order = left->views - right->views;
    } else {
      order = left->title.compare(right->title);
    }
    return descending ? order > 0 : order < 0;
  });

  const int page = qMax(1, query.queryItemValue(QStringLiteral("page")).toInt());
  const int limit = qMax(1, query.queryItemValue(QStringLiteral("limit")).toInt());
  const int total = matched.size();
  const int pages = (total + limit - 1) / limit;
  QJsonArray items;
  for (int index = (page - 1) * limit; index < qMin(page * limit, total); ++index) {
    const CatalogueRow& row = *matched.at(index);
    items.append(QJsonObject{
        {QStringLiteral("ids"), QJsonObject{{QStringLiteral("mikai"), index + 1}}},
        {QStringLiteral("titles"), QJsonObject{{QStringLiteral("english"), row.title}}},
        {QStringLiteral("year"), row.year},
        {QStringLiteral("format"), row.format},
        {QStringLiteral("status"), row.status}});
  }
  QJsonObject payload{{QStringLiteral("ok"), true},
                      {QStringLiteral("result"), items},
                      {QStringLiteral("total"), total},
                      {QStringLiteral("page"), page},
                      {QStringLiteral("pages"), pages}};
  return WireReply{200, QJsonDocument(payload).toJson(QJsonDocument::Compact), {}};
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

class CallOutcome {
 public:
  explicit CallOutcome(MikaiClient* client, const QString& query, bool validateKey = false)
      : client_(client), query_(query), validateKey_(validateKey) {}

  CallOutcome(const CallOutcome&) = delete;
  CallOutcome& operator=(const CallOutcome&) = delete;

  ~CallOutcome() {
    // The worker points at a client on the test's stack, so it must not outlive
    // the test: joining here ends the call instead of leaving a dangling client.
    if (thread_ != nullptr) {
      thread_->wait();
      delete thread_;
    }
  }

  void start() {
    thread_ = QThread::create([this] { run(); });
    thread_->start();
  }

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
      if (validateKey_) {
        const QMap<QString, QVariant> result = client_->validateApiKey(query_);
        threw_ = !result.value(QStringLiteral("valid")).toBool();
        code_ = result.value(QStringLiteral("error_code")).toString();
      } else {
        mikai::backend::AnimeQuery namedQuery;
        namedQuery.search = query_;
        client_->search(namedQuery);
      }
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
  bool validateKey_ = false;
  QThread* thread_ = nullptr;
  std::atomic<bool> finished_{false};
  bool threw_ = false;
  QString code_;
};

}  // namespace

void TestApiClient::initTestCase() {
  // The client walks up from the executable directory to find a .env, which
  // would find the repository's: point it at a scratch file for the suite.
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
    mikai::backend::AnimeQuery query;
    query.search = QStringLiteral("torch");
    client.search(query);
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
  api.enqueue(200, QByteArrayLiteral("<html>503 service unavailable</html>"));

  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  try {
    mikai::backend::AnimeQuery query;
    query.search = QStringLiteral("torch");
    client.search(query);
    QFAIL("a non-envelope body must not report success");
  } catch (const MikaiApiError& caught) {
    QVERIFY2(!caught.code.isEmpty(), "a malformed reply must still name a reason");
    QVERIFY2(!caught.message.isEmpty(), "a malformed reply must still carry a message");
  }
}

void TestApiClient::catalogueFiltersNarrowTheReturnedRows() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  api.setRoute([](const CapturedRequest& request) { return catalogueRoute(request); });
  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  const auto titles = [&client](const AnimeQuery& query) {
    QStringList found;
    for (const mikai::backend::AnimeSummary& entry : client.search(query).items) {
      found << entry.titles.english;
    }
    return found;
  };
  AnimeQuery query;
  QCOMPARE(titles(query), QStringList({QStringLiteral("Action Comedy School"),
                                      QStringLiteral("Action School"),
                                      QStringLiteral("Comedy Spoiler")}));
  query.search = QStringLiteral("Spoiler");
  QCOMPARE(titles(query), QStringList({QStringLiteral("Comedy Spoiler")}));
  query = AnimeQuery();
  query.genres = {QStringLiteral("Action")};
  QCOMPARE(titles(query), QStringList({QStringLiteral("Action Comedy School"),
                                      QStringLiteral("Action School")}));
  query = AnimeQuery();
  query.genres = {QStringLiteral("Action"), QStringLiteral("Comedy")};
  query.genres_match = QStringLiteral("all");
  QCOMPARE(titles(query), QStringList({QStringLiteral("Action Comedy School")}));
  query = AnimeQuery();
  query.genres = {QStringLiteral("Action"), QStringLiteral("Comedy")};
  query.genres_match = QStringLiteral("any");
  QCOMPARE(titles(query), QStringList({QStringLiteral("Action Comedy School"),
                                      QStringLiteral("Action School"),
                                      QStringLiteral("Comedy Spoiler")}));
  query = AnimeQuery();
  query.genres_exclude = {QStringLiteral("Comedy")};
  QCOMPARE(titles(query), QStringList({QStringLiteral("Action School")}));
  query = AnimeQuery();
  query.tags = {QStringLiteral("school")};
  QCOMPARE(titles(query), QStringList({QStringLiteral("Action Comedy School"),
                                      QStringLiteral("Action School")}));
  query = AnimeQuery();
  query.tags_exclude = {QStringLiteral("spoiler")};
  QCOMPARE(titles(query), QStringList({QStringLiteral("Action School")}));
  query = AnimeQuery();
  query.years = {2020, 2024};
  QCOMPARE(titles(query), QStringList({QStringLiteral("Action School"),
                                      QStringLiteral("Comedy Spoiler")}));
  query = AnimeQuery();
  query.year_from = 2021;
  query.year_to = 2023;
  QCOMPARE(titles(query), QStringList({QStringLiteral("Action Comedy School")}));
  query = AnimeQuery();
  query.seasons = {QStringLiteral("summer")};
  QCOMPARE(titles(query), QStringList({QStringLiteral("Comedy Spoiler")}));
  query = AnimeQuery();
  query.formats = {QStringLiteral("movie")};
  QCOMPARE(titles(query), QStringList({QStringLiteral("Comedy Spoiler")}));
  query = AnimeQuery();
  query.statuses = {QStringLiteral("ongoing")};
  QCOMPARE(titles(query), QStringList({QStringLiteral("Action Comedy School"),
                                      QStringLiteral("Action School")}));
  query = AnimeQuery();
  query.studios = {QStringLiteral("Alpha")};
  QCOMPARE(titles(query), QStringList({QStringLiteral("Action Comedy School"),
                                      QStringLiteral("Action School")}));
  query = AnimeQuery();
  query.teams = {2};
  QCOMPARE(titles(query), QStringList({QStringLiteral("Comedy Spoiler")}));
  query = AnimeQuery();
  query.sort = QStringLiteral("year");
  query.order = QStringLiteral("desc");
  QCOMPARE(titles(query), QStringList({QStringLiteral("Comedy Spoiler"),
                                      QStringLiteral("Action Comedy School"),
                                      QStringLiteral("Action School")}));
  query = AnimeQuery();
  query.sort = QStringLiteral("views");
  query.order = QStringLiteral("desc");
  QCOMPARE(titles(query), QStringList({QStringLiteral("Action School"),
                                      QStringLiteral("Action Comedy School"),
                                      QStringLiteral("Comedy Spoiler")}));
  query = AnimeQuery();
  query.page = 2;
  query.limit = 1;
  const SearchResult paged = client.search(query);
  QCOMPARE(paged.items.size(), 1);
  QCOMPARE(paged.items.first().titles.english, QStringLiteral("Action School"));
  QCOMPARE(paged.page.value_or(0), 2);
}

void TestApiClient::dictionariesAndMetaKeepEveryValueTheServerOffers() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  api.setRoute([](const CapturedRequest& request) {
    const QString path = QUrl(request.target).path();
    QJsonValue result;
    if (path.endsWith(QStringLiteral("/meta"))) {
      result = QJsonObject{
          {QStringLiteral("version"), QStringLiteral("v1")},
          {QStringLiteral("formats"), QJsonArray{QStringLiteral("TV"), QStringLiteral("ONA"),
                                                  QStringLiteral("future-format")}},
          {QStringLiteral("statuses"), QJsonArray{QStringLiteral("ongoing")}},
          {QStringLiteral("seasons"), QJsonArray{QStringLiteral("winter")}},
          {QStringLiteral("animeSortFields"), QJsonArray{QStringLiteral("name"),
                                                         QStringLiteral("views"),
                                                         QStringLiteral("future-sort")}},
          {QStringLiteral("sortOrders"), QJsonArray{QStringLiteral("asc"), QStringLiteral("desc")}},
          {QStringLiteral("matchModes"), QJsonArray{QStringLiteral("all"), QStringLiteral("any"),
                                                    QStringLiteral("future-mode")}}};
    } else if (path.endsWith(QStringLiteral("/genres"))) {
      const QJsonObject translated{{QStringLiteral("name"), QStringLiteral("Action")},
                                  {QStringLiteral("ua"), QStringLiteral("Бойовик")}};
      const QJsonObject untranslated{{QStringLiteral("name"), QStringLiteral("Slice of Life")}};
      result = QJsonArray{translated, untranslated};
    } else if (path.endsWith(QStringLiteral("/tags"))) {
      const QJsonObject tag{{QStringLiteral("name"), QStringLiteral("school")},
                            {QStringLiteral("ua"), QStringLiteral("Школа")}};
      result = QJsonArray{tag};
    } else if (path.endsWith(QStringLiteral("/studios"))) {
      result = QJsonArray{QStringLiteral("Studio A"), QStringLiteral("Studio B")};
    } else {
      return WireReply{404, failurePayload(QStringLiteral("not_found"), QStringLiteral("no route")), {}};
    }
    const QJsonObject payload{{QStringLiteral("ok"), true}, {QStringLiteral("result"), result}};
    return WireReply{200, QJsonDocument(payload).toJson(QJsonDocument::Compact), {}};
  });

  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  const ApiMeta meta = client.getMeta();
  QCOMPARE(meta.version, QStringLiteral("v1"));
  // Unknown enum members stay selectable: the server may add them at any time.
  QCOMPARE(meta.formats, QStringList({QStringLiteral("TV"), QStringLiteral("ONA"),
                                      QStringLiteral("future-format")}));
  QCOMPARE(meta.anime_sort_fields, QStringList({QStringLiteral("name"), QStringLiteral("views"),
                                                QStringLiteral("future-sort")}));
  QCOMPARE(meta.match_modes, QStringList({QStringLiteral("all"), QStringLiteral("any"),
                                          QStringLiteral("future-mode")}));
  const QVector<GenreTag> genres = client.getGenres();
  QCOMPARE(genres.size(), 2);
  QCOMPARE(genres.first().name, QStringLiteral("Action"));
  QCOMPARE(genres.first().ua, QStringLiteral("Бойовик"));
  QCOMPARE(genres.at(1).name, QStringLiteral("Slice of Life"));
  QVERIFY(genres.at(1).ua.isEmpty());
  const QVector<GenreTag> tags = client.getTags();
  QCOMPARE(tags.size(), 1);
  QCOMPARE(tags.first().name, QStringLiteral("school"));
  QCOMPARE(tags.first().ua, QStringLiteral("Школа"));
  QCOMPARE(client.getStudios(), QStringList({QStringLiteral("Studio A"), QStringLiteral("Studio B")}));
}

void TestApiClient::teamCatalogueAndProfileCarryTheirOwnData() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  api.setRoute([](const CapturedRequest& request) {
    const QString path = QUrl(request.target).path();
    const QJsonObject team{{QStringLiteral("id"), 17},
                           {QStringLiteral("slug"), QStringLiteral("team-a")},
                           {QStringLiteral("name"), QStringLiteral("Team A")}};
    QJsonObject payload;
    if (path.endsWith(QStringLiteral("/teams/team-a/anime"))) {
      const QJsonObject row{{QStringLiteral("ids"), QJsonObject{{QStringLiteral("mikai"), 7428}}},
                            {QStringLiteral("titles"),
                             QJsonObject{{QStringLiteral("english"), QStringLiteral("Team Show")}}}};
      payload = QJsonObject{{QStringLiteral("ok"), true},
                            {QStringLiteral("result"), QJsonArray{row}},
                            {QStringLiteral("total"), 1},
                            {QStringLiteral("page"), 1},
                            {QStringLiteral("pages"), 1}};
    } else if (path.endsWith(QStringLiteral("/teams/team-a"))) {
      const QJsonObject links{{QStringLiteral("site"), QStringLiteral("https://team.example")}};
      QJsonObject profile = team;
      profile.insert(QStringLiteral("description"), QStringLiteral("profile description"));
      profile.insert(QStringLiteral("animeCount"), 9);
      profile.insert(QStringLiteral("hasVoice"), true);
      profile.insert(QStringLiteral("links"), links);
      payload = QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("result"), profile}};
    } else if (path.endsWith(QStringLiteral("/teams"))) {
      const QJsonObject avatar{{QStringLiteral("big"),
                               QJsonObject{{QStringLiteral("webp"), QStringLiteral("avatar.webp")}}}};
      const QJsonObject views{{QStringLiteral("total"), 1200},
                              {QStringLiteral("last30Days"), 42}};
      QJsonObject profile = team;
      profile.insert(QStringLiteral("description"), QStringLiteral("details"));
      profile.insert(QStringLiteral("birthday"), QStringLiteral("2001-01-01"));
      profile.insert(QStringLiteral("avatar"), avatar);
      profile.insert(QStringLiteral("hasVoice"), true);
      profile.insert(QStringLiteral("hasSubs"), false);
      profile.insert(QStringLiteral("animeCount"), 9);
      profile.insert(QStringLiteral("views"), views);
      payload = QJsonObject{{QStringLiteral("ok"), true},
                            {QStringLiteral("result"), QJsonArray{profile}},
                            {QStringLiteral("total"), 120},
                            {QStringLiteral("page"), 2},
                            {QStringLiteral("pages"), 6}};

    } else {
      return WireReply{404, failurePayload(QStringLiteral("not_found"), QStringLiteral("no route")), {}};
    }
    return WireReply{200, QJsonDocument(payload).toJson(QJsonDocument::Compact), {}};
  });

  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  const TeamsResult teams = client.getTeams(QString(), 2, 50);
  QCOMPARE(teams.total.value_or(0), 120);
  QCOMPARE(teams.page.value_or(0), 2);
  QCOMPARE(teams.pages.value_or(0), 6);
  QCOMPARE(teams.items.size(), 1);
  QCOMPARE(teams.items.first().team.slug, QStringLiteral("team-a"));
  QCOMPARE(teams.items.first().team.name, QStringLiteral("Team A"));
  QCOMPARE(teams.items.first().description, QStringLiteral("details"));
  QCOMPARE(teams.items.first().birthday, QStringLiteral("2001-01-01"));
  QCOMPARE(teams.items.first().avatar.big.webp, QStringLiteral("avatar.webp"));
  QCOMPARE(teams.items.first().anime_count, 9);
  QVERIFY(teams.items.first().has_voice);
  QVERIFY(!teams.items.first().has_subs);
  QCOMPARE(teams.items.first().views_total, qint64(1200));
  QCOMPARE(teams.items.first().views_last_30_days, qint64(42));

  const TeamProfile profile = client.getTeam(QStringLiteral("team-a"));
  QCOMPARE(profile.team.name, QStringLiteral("Team A"));
  QCOMPARE(profile.description, QStringLiteral("profile description"));
  QCOMPARE(profile.links.value(QStringLiteral("site")), QStringLiteral("https://team.example"));

  const SearchResult teamAnime = client.getTeamAnime(QStringLiteral("team-a"), 1, 20);
  QCOMPARE(teamAnime.items.size(), 1);
  QCOMPARE(teamAnime.items.first().titles.english, QStringLiteral("Team Show"));
  QCOMPARE(teamAnime.items.first().ref, QStringLiteral("7428"));
}

void TestApiClient::updatesCarryTheFeedIdentityAndPagingTheServerReported() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  const QString watermark = QStringLiteral("2026-10-20T11:59:59Z");
  api.setRoute([watermark](const CapturedRequest& request) {
    const QUrlQuery query(QUrl(request.target).query());
    // The server validates the watermark, so the answer depends on the client's own forwarding.
    if (query.queryItemValue(QStringLiteral("since")) != watermark) {
      return WireReply{422, failurePayload(QStringLiteral("invalid_parameter"),
                                           QStringLiteral("since is not a known watermark")),
                       {}};
    }
    const QJsonObject anime{{QStringLiteral("ids"), QJsonObject{{QStringLiteral("mikai"), 1523}}},
                            {QStringLiteral("titles"),
                             QJsonObject{{QStringLiteral("english"), QStringLiteral("Feed Show")}}}};
    const QJsonObject row{{QStringLiteral("anime"), anime},
                          {QStringLiteral("provider"), QStringLiteral("provider-a")},
                          {QStringLiteral("embedUrl"), QStringLiteral("https://video.example/embed/1")},
                          {QStringLiteral("publishedAt"), QStringLiteral("2026-10-20T12:00:00Z")},
                          {QStringLiteral("episode"), 2.5},
                          {QStringLiteral("teamName"), QStringLiteral("Feed Team")}};
    const QJsonObject payload{{QStringLiteral("ok"), true},
                              {QStringLiteral("result"), QJsonArray{row}},
                              {QStringLiteral("total"), 29},
                              {QStringLiteral("page"), 4},
                              {QStringLiteral("pages"), 3}};
    return WireReply{200, QJsonDocument(payload).toJson(QJsonDocument::Compact), {}};
  });

  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  const UpdatesResult updates = client.getUpdates(4, 12, watermark);
  QCOMPARE(updates.total.value_or(0), 29);
  QCOMPARE(updates.page.value_or(0), 4);
  QCOMPARE(updates.pages.value_or(0), 3);
  QCOMPARE(updates.items.size(), 1);
  const mikai::backend::AnimeUpdate& item = updates.items.first();
  QCOMPARE(item.anime.ref, QStringLiteral("1523"));
  QCOMPARE(item.anime.titles.english, QStringLiteral("Feed Show"));
  QCOMPARE(item.provider, QStringLiteral("provider-a"));
  QCOMPARE(item.team_name, QStringLiteral("Feed Team"));
  QCOMPARE(item.published_at, QStringLiteral("2026-10-20T12:00:00Z"));
  // The watch address belongs to the event: the same series on another provider is another event.
  QCOMPARE(item.embed_url, QStringLiteral("https://video.example/embed/1"));
  QCOMPARE(item.episode.toDouble(), 2.5);

  try {
    client.getUpdates(1, 12, QStringLiteral("2020-01-01T00:00:00Z"));
    QFAIL("a watermark the server refused must not become an empty feed");
  } catch (const MikaiApiError& caught) {
    QCOMPARE(caught.code, QStringLiteral("invalid_parameter"));
    QCOMPARE(caught.status, 422);
  }
}

void TestApiClient::everyApiRequestNamesTheApplicationAndItsVersion() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  api.setRoute([](const CapturedRequest&) {
    return WireReply{200, okListPayload(), {}};
  });

  const QString name = QCoreApplication::applicationName().simplified()
                           .replace(QLatin1Char(' '), QLatin1Char('-'));
  const QString version = QCoreApplication::applicationVersion();
  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  AnimeQuery uaQuery;
  uaQuery.search = QStringLiteral("ua-check");
  client.search(uaQuery);
  const QString agent = api.headerOf(0, QStringLiteral("User-Agent"));
  QVERIFY2(!agent.isEmpty(), "the request carried no User-Agent at all");
  QVERIFY2(agent.startsWith(name + QLatin1Char('/')),
           qPrintable(QStringLiteral("the User-Agent does not name the application: %1").arg(agent)));

  const QString sentinel = QStringLiteral("9.9.7-ua-probe");
  QCoreApplication::setApplicationVersion(sentinel);
  MikaiClient second(QString::fromLatin1(kOldKey), api.baseUrl());
  AnimeQuery versionQuery;
  versionQuery.search = QStringLiteral("ua-version");
  second.search(versionQuery);
  const QString versioned = api.headerOf(1, QStringLiteral("User-Agent"));
  QVERIFY2(versioned.contains(sentinel),
           qPrintable(QStringLiteral("the User-Agent did not follow the version the application "
                                     "declared: %1")
                          .arg(versioned)));
  QVERIFY2(versioned.startsWith(name + QLatin1Char('/')),
           qPrintable(QStringLiteral("the User-Agent lost the application name: %1").arg(versioned)));
  QCoreApplication::setApplicationVersion(version);
}

void TestApiClient::anInvalidCatalogueFilterSurfacesTheApiValidationResponse() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  api.enqueue(422, failurePayload(QStringLiteral("invalid_parameter"),
                                  QStringLiteral("sort is not a supported catalogue field")));
  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  AnimeQuery query;
  query.sort = QStringLiteral("future-sort");
  try {
    client.search(query);
    QFAIL("an invalid server-side filter must not become an empty success result");
  } catch (const MikaiApiError& caught) {
    QCOMPARE(caught.code, QStringLiteral("invalid_parameter"));
    QCOMPARE(caught.status, 422);
    QVERIFY(caught.message.contains(QStringLiteral("sort")));
  }
}

QByteArray catalogueBody(const QString& title) {
  const QJsonObject row{{QStringLiteral("ids"), QJsonObject{{QStringLiteral("mikai"), 7428}}},
                        {QStringLiteral("titles"), QJsonObject{{QStringLiteral("english"), title}}}};
  const QJsonObject payload{{QStringLiteral("ok"), true},
                            {QStringLiteral("total"), 1},
                            {QStringLiteral("page"), 1},
                            {QStringLiteral("pages"), 1},
                            {QStringLiteral("result"), QJsonArray{row}}};
  return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

void TestApiClient::aFreshResponseIsServedWithoutAnotherRoundTrip() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  int revision = 0;
  api.setRoute([&revision](const CapturedRequest&) {
    const QString title = ++revision == 1 ? QStringLiteral("Fresh title")
                                          : QStringLiteral("Changed title");
    return WireReply{200, catalogueBody(title),
                     {{QStringLiteral("Cache-Control"), QStringLiteral("public, max-age=60")}}};
  });
  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  AnimeQuery query;
  query.search = QStringLiteral("cache-fresh");
  const SearchResult first = client.search(query);
  const SearchResult second = client.search(query);
  QCOMPARE(first.items.first().titles.english, QStringLiteral("Fresh title"));
  QCOMPARE(second.items.first().titles.english, QStringLiteral("Fresh title"));
}

void TestApiClient::aStaleResponseIsRevalidatedAndTheNewBodyIsServed() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  int call = 0;
  api.setRoute([&call](const CapturedRequest& request) {
    ++call;
    if (call == 1) {
      return WireReply{200, catalogueBody(QStringLiteral("Old title")),
                       {{QStringLiteral("Cache-Control"), QStringLiteral("max-age=0")},
                        {QStringLiteral("ETag"), QStringLiteral("\"rev-1\"")}}};
    }
    const QString validator = LoopbackApi::valueOf(request, QStringLiteral("If-None-Match"));
    if (validator == QLatin1String("\"rev-1\"")) {
      return WireReply{200, catalogueBody(QStringLiteral("New title")),
                       {{QStringLiteral("Cache-Control"), QStringLiteral("max-age=60")},
                        {QStringLiteral("ETag"), QStringLiteral("\"rev-2\"")}}};
    }
    return WireReply{500, failurePayload(QStringLiteral("temporary"), QStringLiteral("retry")), {}};
  });
  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  AnimeQuery query;
  query.search = QStringLiteral("cache-etag");
  QCOMPARE(client.search(query).items.first().titles.english, QStringLiteral("Old title"));
  QCOMPARE(client.search(query).items.first().titles.english, QStringLiteral("New title"));
  QCOMPARE(client.search(query).items.first().titles.english, QStringLiteral("New title"));
}

void TestApiClient::aStaleResponseTheServerStillHoldsIsReusedUnchanged() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  int call = 0;
  api.setRoute([&call](const CapturedRequest& request) {
    ++call;
    if (call == 1) {
      return WireReply{200, catalogueBody(QStringLiteral("Held title")),
                       {{QStringLiteral("Cache-Control"), QStringLiteral("max-age=0")},
                        {QStringLiteral("ETag"), QStringLiteral("\"held\"")}}};
    }
    if (call == 2 &&
        LoopbackApi::valueOf(request, QStringLiteral("If-None-Match")) == QLatin1String("\"held\"")) {
      return WireReply{304, {},
                       {{QStringLiteral("Cache-Control"), QStringLiteral("max-age=60")},
                        {QStringLiteral("ETag"), QStringLiteral("\"held\"")}}};
    }
    return WireReply{200, catalogueBody(QStringLiteral("Changed title")),
                     {{QStringLiteral("Cache-Control"), QStringLiteral("max-age=60")}}};
  });
  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  AnimeQuery query;
  query.search = QStringLiteral("cache-304");
  QCOMPARE(client.search(query).items.first().titles.english, QStringLiteral("Held title"));
  QCOMPARE(client.search(query).items.first().titles.english, QStringLiteral("Held title"));
  QCOMPARE(client.search(query).items.first().titles.english, QStringLiteral("Held title"));
}

void TestApiClient::aRevalidatedBodySurvivesEvictionWhileItsRequestIsInFlight() {
  LoopbackApi api;
  LoopbackApi fillerApi;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  QVERIFY2(fillerApi.start(&error), qPrintable(error));
  fillerApi.setRoute([](const CapturedRequest&) {
    return WireReply{200, catalogueBody(QStringLiteral("Other catalogue entry")),
                     {{QStringLiteral("Cache-Control"), QStringLiteral("max-age=60")}}};
  });
  MikaiClient filler(QString::fromLatin1(kNewKey), fillerApi.baseUrl());
  int call = 0;
  bool conditional = false;
  api.setRoute([&](const CapturedRequest& request) {
    if (++call == 1) {
      return WireReply{200, catalogueBody(QStringLiteral("Validated title")),
                       {{QStringLiteral("Cache-Control"), QStringLiteral("max-age=0")},
                        {QStringLiteral("ETag"), QStringLiteral("\"original\"")}}};
    }
    conditional = LoopbackApi::valueOf(request, QStringLiteral("If-None-Match")) ==
                  QLatin1String("\"original\"");
    // The original reply is still in flight. Populate more than the bounded
    // cache can hold through another endpoint before sending its 304.
    for (int index = 0; index < 129; ++index) {
      AnimeQuery query;
      query.search = QStringLiteral("eviction-entry-%1").arg(index);
      filler.search(query);
    }
    return WireReply{304, {},
                     {{QStringLiteral("Cache-Control"), QStringLiteral("max-age=60")}}};
  });
  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  AnimeQuery query;
  query.search = QStringLiteral("cache-evicted-validator");
  QCOMPARE(client.search(query).items.first().titles.english, QStringLiteral("Validated title"));
  QCOMPARE(client.search(query).items.first().titles.english, QStringLiteral("Validated title"));
  QVERIFY(conditional);
  QCOMPARE(api.requestCount(), 2);
  QCOMPARE(fillerApi.requestCount(), 129);
}

void TestApiClient::aNoCacheResponseIsRevalidatedEveryTime() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  int call = 0;
  api.setRoute([&call](const CapturedRequest&) {
    ++call;
    // A distinct title per answer, so a replayed cache entry would be visible.
    const QString title = QStringLiteral("Revision %1").arg(call);
    return WireReply{200, catalogueBody(title),
                     {{QStringLiteral("Cache-Control"), QStringLiteral("no-cache")},
                      {QStringLiteral("ETag"), QStringLiteral("\"must-revalidate\"")}}};
  });
  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  AnimeQuery query;
  query.search = QStringLiteral("cache-no-cache");
  QCOMPARE(client.search(query).items.first().titles.english, QStringLiteral("Revision 1"));
  QCOMPARE(client.search(query).items.first().titles.english, QStringLiteral("Revision 2"));
  QCOMPARE(client.search(query).items.first().titles.english, QStringLiteral("Revision 3"));
}

void TestApiClient::aNoStoreResponseIsNeverKeptAndTheCredentialDecidesWhatIsKept() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  // The body names the credential and its serve count, so a replayed body and
  // another credential's body are both visible in the data.
  QMap<QString, int> served;
  api.setRoute([&served](const CapturedRequest& request) {
    const QString key = LoopbackApi::valueOf(request, QStringLiteral("X-API-Key"));
    const int count = ++served[key];
    const bool store = key != QLatin1String("mk_third_key_synthetic");
    return WireReply{200, catalogueBody(QStringLiteral("%1|%2").arg(key).arg(count)),
                     {{QStringLiteral("Cache-Control"),
                       store ? QStringLiteral("max-age=60") : QStringLiteral("no-store")}}};
  });
  MikaiClient cached(QString::fromLatin1(kOldKey), api.baseUrl());
  MikaiClient other(QString::fromLatin1(kNewKey), api.baseUrl());
  MikaiClient unstorable(QStringLiteral("mk_third_key_synthetic"), api.baseUrl());
  AnimeQuery query;
  query.search = QStringLiteral("cache-isolation");

  QCOMPARE(cached.search(query).items.first().titles.english,
           QStringLiteral("mk_old_key_synthetic|1"));
  QCOMPARE(cached.search(query).items.first().titles.english,
           QStringLiteral("mk_old_key_synthetic|1"));
  QCOMPARE(other.search(query).items.first().titles.english,
           QStringLiteral("mk_new_key_synthetic|1"));
  QCOMPARE(other.search(query).items.first().titles.english,
           QStringLiteral("mk_new_key_synthetic|1"));
  QCOMPARE(unstorable.search(query).items.first().titles.english,
           QStringLiteral("mk_third_key_synthetic|1"));
  QCOMPARE(unstorable.search(query).items.first().titles.english,
           QStringLiteral("mk_third_key_synthetic|2"));
}

void TestApiClient::aFailureIsNotCachedAndACancelledCallReadsNothing() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  int call = 0;
  api.setRoute([&call](const CapturedRequest&) {
    ++call;
    if (call == 1) {
      return WireReply{500, failurePayload(QStringLiteral("temporary"), QStringLiteral("retry")), {}};
    }
    return WireReply{200, catalogueBody(QStringLiteral("Recovered title")),
                     {{QStringLiteral("Cache-Control"), QStringLiteral("max-age=60")}}};
  });
  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  AnimeQuery query;
  query.search = QStringLiteral("cache-failure");
  try {
    client.search(query);
    QFAIL("an HTTP failure must not be cached as a result");
  } catch (const MikaiApiError& caught) {
    QCOMPARE(caught.code, QStringLiteral("temporary"));
  }
  QCOMPARE(client.search(query).items.first().titles.english, QStringLiteral("Recovered title"));
  QCOMPARE(client.search(query).items.first().titles.english, QStringLiteral("Recovered title"));

  std::atomic_bool cancelled{true};
  mikai::backend::WorkCancellationScope scope(&cancelled);
  try {
    client.search(query);
    QFAIL("a cancelled operation must not read even a fresh cache entry");
  } catch (const MikaiApiError& caught) {
    QCOMPARE(caught.code, QStringLiteral("cancelled"));
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
void TestApiClient::anArtworkTimeoutDoesNotWaitForAnAlreadyFinishedAbort() {
  QTcpServer server;
  QVERIFY(server.listen(QHostAddress::LocalHost, 0));
  MikaiClient client(QString::fromLatin1(kOldKey));
  QElapsedTimer elapsed;
  elapsed.start();
  bool failed = false;
  try {
    client.fetchBytes(QStringLiteral("http://127.0.0.1:%1/quiet").arg(server.serverPort()), 100);
  } catch (const MikaiApiError& caught) {
    failed = true;
    QCOMPARE(caught.code, QStringLiteral("network_error"));
    QVERIFY2(caught.message.contains(QStringLiteral("timed out"), Qt::CaseInsensitive),
             qPrintable(caught.message));
  }
  QVERIFY(failed);
  QVERIFY2(elapsed.elapsed() < 1500, "a completed abort was followed by a second event-loop wait");
}

void TestApiClient::aPlainEnvelopeSuccessParsesIntoTheResult() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  api.enqueue(200, okListPayload());

  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  mikai::backend::AnimeQuery query;
  query.search = QStringLiteral("torch");
  const SearchResult result = client.search(query);
  QCOMPARE(result.items.size(), 1);
  QCOMPARE(asText(result.items.first().titles.english), QStringLiteral("Black Torch"));
}

void TestApiClient::aValidateCallKeepsTheEnvelopeItWasGiven() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  api.enqueue(403, failurePayload(QStringLiteral("api_key_disabled"), QString::fromUtf8("\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87 \xD0\xB2\xD0\xB8\xD0\xBC\xD0\xBA\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xBE")));

  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  const QMap<QString, QVariant> verdict = client.validateApiKey(QStringLiteral("mk_candidate_synthetic"));
  QCOMPARE(verdict.value(QStringLiteral("valid")).toBool(), false);
  QCOMPARE(verdict.value(QStringLiteral("error_code")).toString(),
           QStringLiteral("api_key_disabled"));
  QCOMPARE(verdict.value(QStringLiteral("error")).toString(), QString::fromUtf8("\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87 \xD0\xB2\xD0\xB8\xD0\xBC\xD0\xBA\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xBE"));
}

void TestApiClient::aTransportFailureIsNotAnApiFailure() {
  MikaiClient client(QString::fromLatin1(kOldKey), QStringLiteral("http://127.0.0.1:1/public/v1"));
  try {
    mikai::backend::AnimeQuery query;
    query.search = QStringLiteral("torch");
    client.search(query);
    QFAIL("an unreachable server must not report success");
  } catch (const MikaiApiError& caught) {
    QCOMPARE(caught.code, QStringLiteral("network_error"));
    QCOMPARE(caught.status, 0);
    QVERIFY2(!caught.message.isEmpty(), "a transport failure must still carry a message");
  }
}

void TestApiClient::aCrossOriginRedirectDoesNotForwardTheApiCredential() {
  LoopbackApi source;
  LoopbackApi destination;
  QString error;
  QVERIFY2(source.start(&error), qPrintable(error));
  QVERIFY2(destination.start(&error), qPrintable(error));
  source.setRoute([&destination](const CapturedRequest&) {
    return WireReply{302, {}, {{QStringLiteral("Location"),
                                destination.baseUrl() + QStringLiteral("/anime")}}};
  });

  MikaiClient client(QString::fromLatin1(kOldKey), source.baseUrl());
  try {
    AnimeQuery query;
    query.search = QStringLiteral("redirect");
    client.search(query);
    QFAIL("a cross-origin redirect must not be followed with the API credential");
  } catch (const MikaiApiError& caught) {
    QVERIFY(caught.code == QStringLiteral("network_error") ||
            caught.code == QStringLiteral("http_error"));
  }
  QCOMPARE(source.requestCount(), 1);
  QCOMPARE(source.headerOf(0, QStringLiteral("X-API-Key")), QString::fromLatin1(kOldKey));
  QCOMPARE(destination.requestCount(), 0);
}

void TestApiClient::aKeyChangedWhileARequestWasParkedDoesNotSendTheOldKey() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));

  // The first answer closes the quota window, so the next caller parks in the
  // limiter before it sends anything: that is when the key changes underneath it.
  api.enqueue(200, okListPayload(), LoopbackApi::quota(300, 0, 1));
  api.enqueue(200, okListPayload(), LoopbackApi::quota(300, 299, 60));

  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());

  CallOutcome first(&client, QStringLiteral("torch"));
  first.start();
  QTRY_VERIFY_WITH_TIMEOUT(api.requestCount() >= 1, 15000);
  QCOMPARE(api.headerOf(0, QStringLiteral("X-API-Key")), QString::fromLatin1(kOldKey));

  // The parking is observed through the limiter, not a socket held open, so
  // the ordering does not depend on any sleep.
  first.waitForFinished();
  QVERIFY2(!first.threw(), qPrintable(first.code()));
  CallOutcome parked(&client, QStringLiteral("torch"));
  parked.start();
  QTRY_VERIFY_WITH_TIMEOUT(client.rateLimitState().queued >= 1, 15000);
  QVERIFY2(api.requestCount() < 2, "a parked caller was sent instead of being parked");

  const ApiKeySaveResult saved = client.setApiKey(QString::fromLatin1(kNewKey));
  QVERIFY2(saved.ok, qPrintable(saved.reason));
  QCOMPARE(client.apiKey(), QString::fromLatin1(kNewKey));

  // The parked call resumes in the new epoch and sends with the credential that
  // is current when the request goes out.
  QTRY_VERIFY_WITH_TIMEOUT(api.requestCount() >= 2, 15000);
  QCOMPARE(api.headerOf(1, QStringLiteral("X-API-Key")), QString::fromLatin1(kNewKey));
  parked.waitForFinished();
  QVERIFY2(parked.finished(), "the parked request never completed");
  QVERIFY2(!parked.threw(), qPrintable(parked.code()));
  QTRY_VERIFY_WITH_TIMEOUT(client.rateLimitState().queued == 0, 5000);
}

void TestApiClient::validatingAParkedOldKeyDoesNotChangeTheNewKeysQuota() {
  LoopbackApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  api.enqueue(200, okListPayload(), LoopbackApi::quota(300, 0, 2));
  api.enqueue(200, QByteArrayLiteral(R"({"ok":true,"result":{"authenticated":true}})"),
              LoopbackApi::quota(300, 0, 60));
  MikaiClient client(QString::fromLatin1(kOldKey), api.baseUrl());
  AnimeQuery query;
  query.search = QStringLiteral("prime-validation-window");
  client.search(query);

  CallOutcome validation(&client, QString::fromLatin1(kOldKey), true);
  validation.start();
  QTRY_VERIFY_WITH_TIMEOUT(client.rateLimitState().queued == 1, 1500);
  const ApiKeySaveResult saved = client.setApiKey(QString::fromLatin1(kNewKey));
  QVERIFY2(saved.ok, qPrintable(saved.reason));

  validation.waitForFinished();
  QVERIFY(validation.finished());
  QVERIFY2(!validation.threw(), qPrintable(validation.code()));
  QCOMPARE(api.requestCount(), 2);
  QCOMPARE(api.headerOf(1, QStringLiteral("X-API-Key")), QString::fromLatin1(kOldKey));
  QCOMPARE(client.apiKey(), QString::fromLatin1(kNewKey));
  const mikai::backend::RateLimitState state = client.rateLimitState();
  QVERIFY2(!state.remaining.has_value(), "the candidate's old quota contaminated the saved key");
  QCOMPARE(state.resetIn, 0);
  QCOMPARE(state.inFlight, 0);
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
  // temporary sibling nor commit, the shape of an unavailable disk.
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
