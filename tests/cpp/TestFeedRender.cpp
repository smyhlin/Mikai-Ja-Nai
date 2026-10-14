#include "TestFeedRender.hpp"

#include <backend/MikaiClient.hpp>
#include <dialogs/ComponentStoreTab.hpp>
#include <dialogs/SettingsDialog.hpp>
#include <pages/MainWindow.hpp>
#include <pages/SearchPage.hpp>
#include <pages/TitlePage.hpp>
#include <state/QualityCache.hpp>
#include <widgets/TitleCard.hpp>
#include <workers/Runner.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryFile>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <cstdio>

#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QWidget>

using mikai::app::ComponentStoreTab;
using mikai::app::MainWindow;
using mikai::app::QualityCache;
using mikai::app::Runner;
using mikai::app::SettingsDialog;
using mikai::app::SearchPage;
using mikai::app::TitleCard;
using mikai::app::TitlePage;
using mikai::backend::SearchResult;
using mikai::backend::MikaiClient;

namespace {

/// How many titles the canned /anime answers with, and how many episodes the
/// canned release carries. The assertions count exactly these.
constexpr int kCards = 6;
constexpr int kEpisodes = 12;
constexpr int kRef = 7428;

/// A 1x1 PNG, so artwork loads over the same loopback socket instead of leaving
/// the machine. Nothing in this file resolves a public name.
QByteArray onePixelPng() {
  static const unsigned char bytes[] = {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
      0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
      0x89, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00,
      0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
      0x42, 0x60, 0x82};
  return QByteArray(reinterpret_cast<const char*>(bytes), static_cast<int>(sizeof(bytes)));
}

QJsonObject imageVariant(const QString& url) {
  QJsonObject variant;
  variant.insert(QStringLiteral("webp"), url);
  variant.insert(QStringLiteral("jpg"), url);
  return variant;
}

QJsonObject posterSet(const QString& url) {
  QJsonObject posters;
  posters.insert(QStringLiteral("big"), imageVariant(url));
  posters.insert(QStringLiteral("medium"), imageVariant(url));
  posters.insert(QStringLiteral("small"), imageVariant(url));
  return posters;
}

QJsonObject titles(const QString& ua, const QString& english, const QString& original) {
  QJsonObject out;
  out.insert(QStringLiteral("ua"), ua);
  out.insert(QStringLiteral("english"), english);
  out.insert(QStringLiteral("original"), original);
  return out;
}

QJsonObject summary(int id, const QString& name, const QString& poster, const QString& format) {
  QJsonObject ids;
  ids.insert(QStringLiteral("mikai"), id);

  QJsonObject images;
  images.insert(QStringLiteral("poster"), posterSet(poster));

  QJsonObject entry;
  entry.insert(QStringLiteral("ids"), ids);
  entry.insert(QStringLiteral("titles"), titles(name, name, name));
  entry.insert(QStringLiteral("year"), 2026);
  entry.insert(QStringLiteral("format"), format);
  entry.insert(QStringLiteral("status"), QStringLiteral("ongoing"));
  entry.insert(QStringLiteral("episodes"), kEpisodes);
  entry.insert(QStringLiteral("images"), images);
  return entry;
}

/// The values one query parameter carried, so a canned answer can react to a
/// filter the way the real API does.
QStringList queryValue(const QString& target, const QString& key) {
  const int query = target.indexOf(QLatin1Char('?'));
  if (query < 0) {
    return {};
  }
  const QStringList pairs = target.mid(query + 1).split(QLatin1Char('&'));
  for (const QString& pair : pairs) {
    const int eq = pair.indexOf(QLatin1Char('='));
    if (eq > 0 && pair.left(eq) == key) {
      return pair.mid(eq + 1).split(QLatin1Char(','));
    }
  }
  return {};
}

QJsonObject detail(int id, const QString& name, const QString& poster) {
  QJsonObject out = summary(id, name, poster, QStringLiteral("tv"));
  out.insert(QStringLiteral("description"), QStringLiteral("Опис для тесту."));
  out.insert(QStringLiteral("startDate"), QStringLiteral("2026-01-04"));
  out.insert(QStringLiteral("source"), QStringLiteral("manga"));
  out.insert(QStringLiteral("studio"), QStringLiteral("Test Studio"));
  out.insert(QStringLiteral("country"), QStringLiteral("JP"));
  out.insert(QStringLiteral("ageRating"), QStringLiteral("PG-13"));

  QJsonObject episodes;
  episodes.insert(QStringLiteral("total"), kEpisodes);
  episodes.insert(QStringLiteral("aired"), kEpisodes);
  out.insert(QStringLiteral("episodes"), episodes);

  QJsonObject scores;
  scores.insert(QStringLiteral("average"), 8.5);
  scores.insert(QStringLiteral("count"), 1234);
  out.insert(QStringLiteral("scores"), scores);
  return out;
}

QJsonObject release(const QString& kind, const QString& teamName, const QString& embedBase) {
  QJsonObject team;
  team.insert(QStringLiteral("id"), 1);
  team.insert(QStringLiteral("slug"), QStringLiteral("test-team"));
  team.insert(QStringLiteral("name"), teamName);

  QJsonArray teams;
  teams.append(team);

  QJsonArray episodes;
  for (int number = 1; number <= kEpisodes; ++number) {
    QJsonObject source;
    source.insert(QStringLiteral("provider"), QStringLiteral("ashdi"));
    source.insert(QStringLiteral("embedUrl"),
                  embedBase + QStringLiteral("/embed/%1").arg(number));

    QJsonArray sources;
    sources.append(source);

    QJsonObject episode;
    episode.insert(QStringLiteral("number"), number);
    episode.insert(QStringLiteral("label"), QStringLiteral("Episode %1").arg(number));
    episode.insert(QStringLiteral("sources"), sources);
    episodes.append(episode);
  }

  QJsonObject out;
  out.insert(QStringLiteral("id"), QStringLiteral("release-") + kind);
  out.insert(QStringLiteral("kind"), kind);
  out.insert(QStringLiteral("isCollab"), false);
  out.insert(QStringLiteral("teams"), teams);
  out.insert(QStringLiteral("episodesCount"), kEpisodes);
  out.insert(QStringLiteral("episodes"), episodes);
  return out;
}

/// A loopback endpoint that answers the API routes the two pages use, from
/// canned payloads, or fails every request the way a dead origin does.
class CannedApi : public QObject {
 public:
  enum class Mode { Answers, Fails };

  explicit CannedApi(QObject* parent = nullptr) : QObject(parent) {}
  ~CannedApi() override {
    for (QTcpSocket* socket : sockets_) {
      socket->abort();
      socket->deleteLater();
    }
    server_.close();
  }

  bool start(QString* error) {
    if (!server_.listen(QHostAddress::LocalHost, 0)) {
      if (error != nullptr) {
        *error = server_.errorString();
      }
      return false;
    }
    connect(&server_, &QTcpServer::newConnection, this, &CannedApi::onNewConnection);
    return true;
  }

  QString baseUrl() const {
    return QStringLiteral("http://127.0.0.1:%1/public/v1").arg(server_.serverPort());
  }

  void setMode(Mode mode) { mode_ = mode; }

  int requestCount() const {
    QMutexLocker locker(&mutex_);
    return requests_;
  }

  QStringList requestedPaths() const {
    QMutexLocker locker(&mutex_);
    return paths_;
  }

 private:
  void onNewConnection() {
    while (QTcpSocket* socket = server_.nextPendingConnection()) {
      sockets_.append(socket);
      connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
      connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
        sockets_.removeOne(socket);
        socket->deleteLater();
      });
    }
  }

  void onReadyRead(QTcpSocket* socket) {
    // QNetworkAccessManager pipelines and reuses connections, so one readyRead
    // can hold several requests and one response must not eat the next
    // request's bytes. Every complete head is answered in turn; the connection
    // stays open until the test tears the server down.
    buffers_[socket].append(socket->readAll());
    for (;;) {
      QByteArray& buffered = buffers_[socket];
      const int end = buffered.indexOf("\r\n\r\n");
      if (end < 0) {
        return;
      }
      const QByteArray requestLine = buffered.left(buffered.indexOf("\r\n"));
      buffered = buffered.mid(end + 4);

      const QList<QByteArray> parts = requestLine.split(' ');
      const QString target = QString::fromLatin1(parts.value(1));
      {
        QMutexLocker locker(&mutex_);
        ++requests_;
        paths_.append(target);
      }
      answer(socket, target);
    }
  }

  void answer(QTcpSocket* socket, const QString& target) {
    if (mode_ == Mode::Fails) {
      send(socket, 522, QStringLiteral("text/plain"), QByteArrayLiteral("error code: 522"));
      return;
    }

    const QByteArray body = bodyFor(target);
    if (body.isEmpty()) {
      send(socket, 404, QStringLiteral("application/json"),
           QByteArrayLiteral("{\"ok\":false,\"error\":{\"errorCode\":\"not_found\","
                             "\"code\":\"not_found\",\"message\":\"no canned route\"}}"));
      return;
    }
    send(socket, 200, QStringLiteral("application/json"), body);
  }

  QByteArray bodyFor(const QString& target) const {
    const int query = target.indexOf(QLatin1Char('?'));
    const QString path = query >= 0 ? target.left(query) : target;
    const QString prefix = QStringLiteral("/public/v1");
    const QString rest = path.startsWith(prefix) ? path.mid(prefix.size()) : path;

    if (rest.startsWith(QStringLiteral("/img/"))) {
      return onePixelPng();
    }
    if (rest == QStringLiteral("/anime")) {
      // The canned rows are half TV and half MOVIE, so a `formats` filter is
      // answerable the way the real API answers it: half the set, and a page
      // count that counts the filtered set rather than everything.
      const QStringList fmt = queryValue(target, QStringLiteral("formats"));
      const bool filtered = !fmt.isEmpty() && fmt.constFirst() == QLatin1String("movie");
      const int served = filtered ? kCards / 2 : kCards;
      QJsonArray items;
      for (int index = 0; index < served; ++index) {
        items.append(summary(kRef + index, QStringLiteral("Canned %1").arg(index + 1), imageUrl(),
                             filtered ? QStringLiteral("movie") : QStringLiteral("tv")));
      }
      QJsonObject payload;
      payload.insert(QStringLiteral("ok"), true);
      payload.insert(QStringLiteral("result"), items);
      payload.insert(QStringLiteral("total"), served);
      payload.insert(QStringLiteral("page"), 1);
      payload.insert(QStringLiteral("pages"), served == 0 ? 0 : 1);
      return QJsonDocument(payload).toJson(QJsonDocument::Compact);
    }
    if (rest == QStringLiteral("/updates")) {
      QJsonObject entry;
      entry.insert(QStringLiteral("anime"),
                   summary(kRef, QStringLiteral("Canned 1"), imageUrl(), QStringLiteral("tv")));
      entry.insert(QStringLiteral("teamName"), QStringLiteral("Test Team"));
      entry.insert(QStringLiteral("kind"), QStringLiteral("voice"));
      entry.insert(QStringLiteral("provider"), QStringLiteral("ashdi"));
      entry.insert(QStringLiteral("episode"), 3);
      entry.insert(QStringLiteral("publishedAt"), QStringLiteral("2026-01-04T12:00:00Z"));

      QJsonArray items;
      items.append(entry);

      QJsonObject payload;
      payload.insert(QStringLiteral("ok"), true);
      payload.insert(QStringLiteral("result"), items);
      payload.insert(QStringLiteral("total"), items.size());
      return QJsonDocument(payload).toJson(QJsonDocument::Compact);
    }
    if (rest == QStringLiteral("/schedule")) {
      QJsonObject airing;
      airing.insert(QStringLiteral("anime"),
                   summary(kRef, QStringLiteral("Canned 1"), imageUrl(), QStringLiteral("tv")));
      airing.insert(QStringLiteral("episode"), 3);
      airing.insert(QStringLiteral("airing"), QStringLiteral("2026-01-04T12:00:00Z"));
      airing.insert(QStringLiteral("finished"), false);

      QJsonArray monday;
      monday.append(airing);

      QJsonObject result;
      result.insert(QStringLiteral("monday"), monday);

      QJsonObject payload;
      payload.insert(QStringLiteral("ok"), true);
      payload.insert(QStringLiteral("result"), result);
      return QJsonDocument(payload).toJson(QJsonDocument::Compact);
    }
    if (rest == QStringLiteral("/meta")) {
      QJsonObject result;
      result.insert(QStringLiteral("version"), QStringLiteral("test"));
      QJsonObject payload;
      payload.insert(QStringLiteral("ok"), true);
      payload.insert(QStringLiteral("result"), result);
      return QJsonDocument(payload).toJson(QJsonDocument::Compact);
    }
    if (rest.endsWith(QStringLiteral("/player"))) {
      QJsonArray releases;
      releases.append(release(QStringLiteral("voice"), QStringLiteral("Test Dub"), embedBase()));
      releases.append(release(QStringLiteral("sub"), QStringLiteral("Test Sub"), embedBase()));

      QJsonObject result;
      result.insert(QStringLiteral("licensed"), false);
      result.insert(QStringLiteral("releases"), releases);

      QJsonObject payload;
      payload.insert(QStringLiteral("ok"), true);
      payload.insert(QStringLiteral("result"), result);
      return QJsonDocument(payload).toJson(QJsonDocument::Compact);
    }
    if (rest.startsWith(QStringLiteral("/anime/"))) {
      const QString ref = rest.mid(QStringLiteral("/anime/").size());
      QJsonObject payload;
      payload.insert(QStringLiteral("ok"), true);
      payload.insert(QStringLiteral("result"),
                     detail(ref.toInt(), QStringLiteral("Canned 1"), imageUrl()));
      return QJsonDocument(payload).toJson(QJsonDocument::Compact);
    }
    return {};
  }

  QString imageUrl() const {
    return QStringLiteral("http://127.0.0.1:%1/img/poster.png").arg(server_.serverPort());
  }

  QString embedBase() const {
    return QStringLiteral("http://127.0.0.1:%1/embed").arg(server_.serverPort());
  }

  void send(QTcpSocket* socket, int status, const QString& contentType, const QByteArray& body) {
    QByteArray head = QByteArrayLiteral("HTTP/1.1 ");
    head += QByteArray::number(status);
    head += QByteArrayLiteral(" OK\r\nContent-Type: ");
    head += contentType.toLatin1();
    head += QByteArrayLiteral("\r\nContent-Length: ");
    head += QByteArray::number(body.size());
    head += QByteArrayLiteral("\r\nConnection: keep-alive\r\n\r\n");
    socket->write(head);
    socket->write(body);
    socket->flush();
  }

  QTcpServer server_;
  QList<QTcpSocket*> sockets_;
  QHash<QTcpSocket*, QByteArray> buffers_;
  Mode mode_ = Mode::Answers;
  mutable QMutex mutex_;
  int requests_ = 0;
  QStringList paths_;
};

/// The carousel's own failure affordance: a "Retry" button it keeps hidden until
/// a load fails.
///
/// It is found by its label rather than its object name on purpose: the updates
/// and schedule rows carry an always-visible "Refresh" action under the same
/// `textAction` name, so an object-name query would count a button that is
/// supposed to be on screen.
int visibleCarouselRetries(const SearchPage& page) {
  const QString retry = QCoreApplication::translate("mikai::app::SearchPage", "Retry");
  int visible = 0;
  for (QPushButton* button : page.findChildren<QPushButton*>()) {
    if (button->isVisible() && button->text() == retry) {
      ++visible;
    }
  }
  return visible;
}

/// The visible code chip inside a page's failure surface: what names the cause
/// ("HTTP 522"), as opposed to the line that says something failed.
QString visibleFailureCode(const QWidget& page) {
  for (QLabel* label : page.findChildren<QLabel*>(QStringLiteral("failureCode"))) {
    if (label->isVisible() && !label->text().isEmpty()) {
      return label->text();
    }
  }
  return {};
}

/// What a failing feed assertion needs to say: what every visible block state
/// label reads, and what the page actually asked the backend for.
QString describeBlockStates(const SearchPage& page) {
  QStringList parts;
  for (QLabel* label : page.findChildren<QLabel*>(QStringLiteral("muted"))) {
    if (label->isVisible() && !label->text().isEmpty()) {
      parts.append(label->text());
    }
  }
  return parts.join(QStringLiteral(" | "));
}

}  // namespace

void TestFeedRender::theFormatFilterIsAskedOfTheServer() {
  CannedApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));

  MikaiClient client(QString(), api.baseUrl());

  // Without the filter the full canned set comes back.
  const SearchResult everything = client.search(QStringLiteral("canned"), 1, 20);
  QCOMPARE(everything.total.value_or(-1), kCards);
  QCOMPARE(static_cast<int>(everything.items.size()), kCards);

  // With it, the server's own answer: half the set, and a page count that counts
  // the filtered result rather than everything. A client-side filter would have
  // reported the unfiltered total, so this is the assertion that catches it.
  const SearchResult filtered =
      client.search(QStringLiteral("canned"), 1, 20, QStringLiteral("movie"));
  QCOMPARE(filtered.total.value_or(-1), kCards / 2);
  QCOMPARE(static_cast<int>(filtered.items.size()), kCards / 2);
  QCOMPARE(filtered.pages.value_or(-1), 1);
  for (const mikai::backend::AnimeSummary& item : filtered.items) {
    QCOMPARE(item.format, QStringLiteral("movie"));
  }

  // The filter reached the transport: the request that answered it carried it.
  const QStringList filteredRequests = api.requestedPaths().filter(QLatin1String("formats=movie"));
  QVERIFY2(!filteredRequests.isEmpty(),
           qPrintable(QStringLiteral("no request carried the filter; requests: %1")
                          .arg(api.requestedPaths().join(QLatin1Char('|')))));
}

void TestFeedRender::thePagerTrustsTheServersOwnPageCount() {
  CannedApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));

  MikaiClient client(QString(), api.baseUrl());

  // The canned API answers `pages` as its own count at the size it served. A
  // client that recomputed that count from its own page size used to disagree
  // with the server and lost the tail of the catalog.
  const SearchResult first = client.search(QStringLiteral("canned"), 1, 50);
  QVERIFY2(first.pages.has_value(), "the payload carried no page count to trust");
  QCOMPARE(first.pages.value_or(-1), 1);

  // A page request the server agrees with returns its own rows.
  const SearchResult served = client.search(QStringLiteral("canned"), first.pages.value_or(1), 50);
  QCOMPARE(served.page.value_or(-1), first.pages.value_or(1));
}

void TestFeedRender::settingsClosingDuringAcquisitionRestoresMainWindowInput() {
  CannedApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));

  MikaiClient client(QString(), api.baseUrl());
  MainWindow window(client);
  QTimer dismissSetupPrompt;
  dismissSetupPrompt.setInterval(10);
  connect(&dismissSetupPrompt, &QTimer::timeout, &window, [&window] {
    QDialog* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (modal != nullptr && qobject_cast<SettingsDialog*>(modal) == nullptr) modal->reject();
  });
  dismissSetupPrompt.start();
  window.resize(900, 700);
  window.show();
  QApplication::processEvents();

  QPushButton* settingsButton = nullptr;
  for (QPushButton* button : window.findChildren<QPushButton*>(QStringLiteral("settingsChip"))) {
    if (button->accessibleName() == QStringLiteral("API token settings")) {
      settingsButton = button;
      break;
    }
  }
  QVERIFY(settingsButton != nullptr);
  // The shortcuts overlay steals focus and never closes itself. Open it first
  // so the regression covers the real stuck state: Settings opening and closing
  // over a live overlay.
  QPushButton* keysButton = nullptr;
  for (QPushButton* button : window.findChildren<QPushButton*>(QStringLiteral("settingsChip"))) {
    if (button->accessibleName() == QStringLiteral("Keyboard shortcuts")) {
      keysButton = button;
      break;
    }
  }
  QVERIFY(keysButton != nullptr);
  QTest::mouseClick(keysButton, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(
      window.findChild<QWidget*>(QStringLiteral("playerKeyHints")) != nullptr, 5000);
  QPointer<SettingsDialog> settings;
  bool acquisitionStarted = false;
  QTimer::singleShot(100, &window, [&window, &settings, &acquisitionStarted] {
    settings = window.findChild<SettingsDialog*>();
    auto* components = window.findChild<ComponentStoreTab*>();
    acquisitionStarted = !settings.isNull() && components != nullptr;
    if (acquisitionStarted) {
      QMetaObject::invokeMethod(components, "acquisitionProgress", Qt::DirectConnection,
                                Q_ARG(QString, QStringLiteral("Downloading test pack")),
                                Q_ARG(int, 42), Q_ARG(bool, true));
    }
  });
  QTest::mouseClick(settingsButton, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(acquisitionStarted, 5000);
  QTRY_VERIFY_WITH_TIMEOUT(settings.isNull(), 5000);
  dismissSetupPrompt.stop();
  auto* acquisitionBar = window.findChild<QWidget*>(QStringLiteral("componentAcquisitionBar"));
  QVERIFY(acquisitionBar != nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(acquisitionBar->isVisible(), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(QApplication::activeModalWidget() == nullptr, 5000);
  QTRY_VERIFY_WITH_TIMEOUT(window.findChild<SettingsDialog*>() == nullptr, 5000);
  if (QWidget* hints = window.findChild<QWidget*>(QStringLiteral("playerKeyHints"))) {
    QVERIFY(!hints->isVisible());
  }
  QTest::qWait(50);
  QVERIFY(window.isEnabled());
  QVERIFY(window.findChild<QWidget*>(QStringLiteral("componentAcquisitionBar")) != nullptr);
  QVERIFY(settingsButton->isEnabled());
#ifdef Q_OS_WIN
  // A real HWND only exists on the windows platform. Under offscreen/minimal
  // winId() is not a real window and IsWindowEnabled on it is meaningless, so
  // the native check runs only where the handle is real. The Qt-level
  // isEnabled plus the search interaction below already prove input works.
  if (QGuiApplication::platformName() == QStringLiteral("windows")) {
    const HWND windowHandle = reinterpret_cast<HWND>(window.winId());
    QVERIFY(windowHandle != nullptr);
    QVERIFY2(IsWindowEnabled(windowHandle) != FALSE,
             "closing Settings left the native MainWindow disabled");
  }
#endif

  auto* query = window.findChild<QLineEdit*>();
  QVERIFY(query != nullptr);
  QTest::mouseClick(query, Qt::LeftButton);
  QTest::keyClicks(query, QStringLiteral("interactive"));
  QTest::keyClick(query, Qt::Key_Return);
  QTRY_VERIFY_WITH_TIMEOUT(api.requestedPaths().join(QLatin1Char('|')).contains(
                               QStringLiteral("search")),
                           5000);
}

void TestFeedRender::theHomeFeedRendersItsCards() {
  CannedApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));

  MikaiClient client(QString(), api.baseUrl());
  Runner runner;
  SearchPage page(runner, client);
  page.resize(1280, 900);
  page.show();

  // The feed fans out over the client's rate limiter, so this is a real wait for
  // the pool, not a single event turn.
  QTRY_VERIFY_WITH_TIMEOUT(page.findChildren<TitleCard*>().size() >= kCards, 30000);
  QVERIFY2(api.requestCount() > 0, "the page never asked the backend for anything");
  QVERIFY2(visibleCarouselRetries(page) == 0,
           qPrintable(QStringLiteral("a carousel is still offering its retry; block states: %1; "
                                     "requested: %2")
                          .arg(describeBlockStates(page),
                               api.requestedPaths().join(QLatin1Char('|')))));
}

void TestFeedRender::theTitlePageRendersItsEpisodes() {
  CannedApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));

  MikaiClient client(QString(), api.baseUrl());
  Runner runner;
  QualityCache cache;
  TitlePage page(runner, client, cache, QString::number(kRef));
  page.resize(1280, 900);
  page.show();

  // One release tab carries every episode of the canned release, and the page
  // paints a pill per episode.
  QTRY_VERIFY_WITH_TIMEOUT(
      page.findChildren<QWidget*>(QStringLiteral("episodeButton")).size() >= kEpisodes, 30000);

  // The pills must be the release's, not an artefact of a leftover state: every
  // canned episode number has exactly one button.
  const QList<QWidget*> buttons = page.findChildren<QWidget*>(QStringLiteral("episodeButton"));
  QSet<QString> labels;
  for (QWidget* button : buttons) {
    const auto* push = qobject_cast<QPushButton*>(button);
    QVERIFY(push != nullptr);
    labels.insert(push->text());
  }
  QCOMPARE(labels.size(), kEpisodes);
}

void TestFeedRender::aFailedBackendShowsAVisibleFailureInsteadOfAnEmptyPage() {
  CannedApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  api.setMode(CannedApi::Mode::Fails);

  MikaiClient client(QString(), api.baseUrl());
  Runner runner;
  QualityCache cache;

  // The feed: no content, and the block's retry affordance on screen instead of
  // a silently empty page.
  SearchPage feed(runner, client);
  feed.resize(1280, 900);
  feed.show();
  QTRY_VERIFY_WITH_TIMEOUT(visibleCarouselRetries(feed) > 0, 30000);
  QCOMPARE(feed.findChildren<TitleCard*>().size(), 0);

  // "Something failed" is not an answer: the surface has to name the cause, and
  // a 522 has to read 522. This is the assertion the old suite had no way to
  // make, because the status never reached a widget.
  QTRY_VERIFY_WITH_TIMEOUT(!visibleFailureCode(feed).isEmpty(), 30000);
  QVERIFY2(visibleFailureCode(feed).contains(QStringLiteral("522")),
           qPrintable(QStringLiteral("the feed's failure code reads: %1")
                          .arg(visibleFailureCode(feed))));

  // The title page: the status line has to move off its loading text and name
  // something. A page that sits on "Loading…" forever is the failure this
  // asserts against.
  TitlePage page(runner, client, cache, QString::number(kRef));
  page.resize(1280, 900);
  page.show();

  QWidget* status = page.findChild<QWidget*>(QStringLiteral("status"));
  QVERIFY2(status != nullptr, "the title page has no status line to report a failure on");
  auto* label = qobject_cast<QLabel*>(status);
  QVERIFY(label != nullptr);
  const QString loading = label->text();
  QVERIFY(!loading.isEmpty());

  QTRY_VERIFY_WITH_TIMEOUT(label->text() != loading, 30000);
  QVERIFY2(!label->text().isEmpty(), "the title page cleared its status instead of naming a failure");
  QCOMPARE(page.findChildren<QWidget*>(QStringLiteral("episodeButton")).size(), 0);

  // And the page's own surface names the cause too, not just its corner status.
  QTRY_VERIFY_WITH_TIMEOUT(!visibleFailureCode(page).isEmpty(), 30000);
  QVERIFY2(visibleFailureCode(page).contains(QStringLiteral("522")),
           qPrintable(QStringLiteral("the title page's failure code reads: %1")
                          .arg(visibleFailureCode(page))));
}

void TestFeedRender::everyRouteReportsTheStatusUnderADeadOrigin() {
  CannedApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  api.setMode(CannedApi::Mode::Fails);

  MikaiClient client(QString(), api.baseUrl());

  struct Attempt {
    QString name;
    std::function<void()> call;
  };
  const QList<Attempt> attempts = {
      {QStringLiteral("ongoing"), [&] { client.browseOngoing(1, 6); }},
      {QStringLiteral("popular"), [&] { client.browsePopular(1, 6); }},
      {QStringLiteral("updates"), [&] { client.getUpdates(1, 12); }},
      {QStringLiteral("schedule"), [&] { client.getSchedule(); }},
      {QStringLiteral("detail"), [&] { client.getAnime(QStringLiteral("7428")); }},
      {QStringLiteral("player"), [&] { client.getPlayer(QStringLiteral("7428")); }},
  };
  for (const Attempt& attempt : attempts) {
    try {
      attempt.call();
      QFAIL(qPrintable(QStringLiteral("a dead origin answered %1").arg(attempt.name)));
    } catch (const mikai::backend::MikaiApiError& failure) {
      QVERIFY2(failure.status == 522,
               qPrintable(QStringLiteral("%1 lost its status: code=%2 status=%3")
                              .arg(attempt.name, failure.code)
                              .arg(failure.status)));
      QCOMPARE(failure.code, QStringLiteral("http_error"));
    }
  }
}

int runTestFeedRender(int argc, char** argv) {
  // Same capture pattern as runTestDownloads: without it a crash or deadline
  // kill under the launcher discards QTest's console output and only the
  // bare FAIL line survives.
  TestFeedRender suite;
  QStringList arguments;
  arguments.reserve(argc + 1);
  arguments << QString::fromLocal8Bit(argv[0]);
  for (int i = 1; i < argc; ++i) arguments << QString::fromLocal8Bit(argv[i]);
  QTemporaryFile report;
  const bool captureReport = !arguments.contains(QStringLiteral("-o"));
  if (captureReport) {
    if (!report.open()) {
      std::fprintf(stderr, "[mikai_tests] cannot create report for TestFeedRender\n");
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
      std::fprintf(stderr, "[mikai_tests] cannot read report for TestFeedRender\n");
    }
  }
  std::fflush(stderr);
  return rc;
}
