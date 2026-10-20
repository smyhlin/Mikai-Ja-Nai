#include "TestFeedRender.hpp"

#include <backend/MikaiClient.hpp>
#include <dialogs/ComponentStoreTab.hpp>
#include <dialogs/SettingsDialog.hpp>
#include <native/components/AnimeJanaiBuildLog.hpp>
#include <pages/MainWindow.hpp>
#include <pages/SearchPage.hpp>
#include <pages/TitlePage.hpp>
#include <state/QualityCache.hpp>
#include <support/I18n.hpp>
#include <widgets/FailureView.hpp>
#include <widgets/RangePager.hpp>
#include <widgets/TitleCard.hpp>
#include <workers/Runner.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QPointer>
#include <QtCore/QScopeGuard>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTemporaryFile>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <algorithm>
#include <cstdio>

#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QWidget>

using mikai::app::ComponentStoreTab;
using mikai::app::FailureView;
using mikai::app::MainWindow;
using mikai::app::QualityCache;
using mikai::app::RangePager;
using mikai::app::Runner;
using mikai::app::SettingsDialog;
using mikai::app::SearchPage;
using mikai::app::TitleCard;
using mikai::app::TitlePage;
using mikai::backend::SearchResult;
using mikai::backend::MikaiClient;
using mplayer::native::AnimeJanaiBuildGate;
using mplayer::native::AnimeJanaiBuildLogEvent;
using mplayer::native::Backend;
using mplayer::native::classifyAnimeJanaiBuildLog;

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

/// The title page's description, long enough to be truncated behind its
/// "Read more" action. It is API data, so both language switches must show the
/// same bytes: only the app's own caption next to it is translated.
QString cannedDescription() {
  return QStringLiteral(
             "Опис для тесту. Ця сторінка перевіряє, що дані, які повернув сервер, "
             "залишаються незмінними, коли мова інтерфейсу перемикається. "
             "Текст опису не є підписом застосунку, тому його не можна перекладати "
             "чи скорочувати без дії читача. ")
      + QStringLiteral(
             "Довгий опис також перевіряє, що кнопка розгортання не зникає під час "
             "зміни мови, а вже відкритий текст не згортається назад у попередній стан.");
}

QJsonObject detail(int id, const QString& name, const QString& poster) {
  QJsonObject out = summary(id, name, poster, QStringLiteral("tv"));
  out.insert(QStringLiteral("description"), cannedDescription());
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

QJsonObject release(const QString& kind, const QString& teamName, const QString& embedBase,
                    int episodeCount = kEpisodes) {
  QJsonObject team;
  team.insert(QStringLiteral("id"), 1);
  team.insert(QStringLiteral("slug"), QStringLiteral("test-team"));
  team.insert(QStringLiteral("name"), teamName);

  QJsonArray teams;
  teams.append(team);

  QJsonArray episodes;
  for (int number = 1; number <= episodeCount; ++number) {
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
  out.insert(QStringLiteral("episodesCount"), episodeCount);
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

  /// How many episodes each canned release carries. The default is the twelve
  /// the render tests count; a test that needs a second episode page raises it.
  void setReleaseEpisodeCount(int count) { releaseEpisodeCount_ = qMax(1, count); }

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
      // The page-state test asks for a set tall enough to have a real scroll
      // range; the ordinary fixtures keep the six rows every other slot counts.
      const int rows = queryValue(target, QStringLiteral("search"))
                               .contains(QStringLiteral("canned-many"))
                           ? 30
                           : kCards;
      const int served = filtered ? rows / 2 : rows;
      QJsonArray items;
      for (int index = 0; index < served; ++index) {
        // The unfiltered set is half TV and half MOVIE, so the format bar has a
        // second tab to offer; a filtered answer carries only the format asked
        // for, the way the real endpoint does.
        const QString format = filtered
                                   ? QStringLiteral("movie")
                                   : (index % 2 == 0 ? QStringLiteral("tv")
                                                     : QStringLiteral("movie"));
        items.append(summary(kRef + index, QStringLiteral("Canned %1").arg(index + 1), imageUrl(),
                             format));
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
      releases.append(release(QStringLiteral("voice"), QStringLiteral("Test Dub"), embedBase(),
                              releaseEpisodeCount_));
      releases.append(release(QStringLiteral("sub"), QStringLiteral("Test Sub"), embedBase(),
                              releaseEpisodeCount_));

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
  int releaseEpisodeCount_ = kEpisodes;
  mutable QMutex mutex_;
  int requests_ = 0;
  QStringList paths_;
};

/// The carousel's own failure affordance: the "Retry" button a FailureView keeps
/// hidden until a load fails.
///
/// It is found through the view that owns it, not through a caption: FailureView
/// translates that caption under its own context, so a lookup built from this
/// page's context reads "Retry" in English but "Retry" again in Ukrainian while
/// the button itself reads "Повторити" - a visible affordance the slot would
/// miss depending on the language a previous slot happened to leave active. The
/// updates and schedule rows reuse FailureView with its retry suppressed, so a
/// visible view with a visible `textAction` is exactly a carousel failure.
int visibleCarouselRetries(const SearchPage& page) {
  int visible = 0;
  for (FailureView* view : page.findChildren<FailureView*>()) {
    if (!view->isVisible()) {
      continue;
    }
    for (QPushButton* button : view->findChildren<QPushButton*>()) {
      if (button->isVisible() && button->objectName() == QLatin1String("textAction")) {
        ++visible;
      }
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

QStringList benchmarkModelIds(const QTreeWidget& tree) {
  QStringList ids;
  for (int groupIndex = 0; groupIndex < tree.topLevelItemCount(); ++groupIndex) {
    const QTreeWidgetItem* group = tree.topLevelItem(groupIndex);
    for (int index = 0; index < group->childCount(); ++index) {
      ids.append(group->child(index)->data(0, Qt::UserRole + 1).toString());
    }
  }
  return ids;
}

QStringList visibleBenchmarkModelIds(const QTreeWidget& tree) {
  QStringList ids;
  for (int groupIndex = 0; groupIndex < tree.topLevelItemCount(); ++groupIndex) {
    const QTreeWidgetItem* group = tree.topLevelItem(groupIndex);
    if (group->isHidden()) continue;
    for (int index = 0; index < group->childCount(); ++index) {
      const QTreeWidgetItem* model = group->child(index);
      if (model->isHidden()) continue;
      ids.append(model->data(0, Qt::UserRole + 1).toString());
    }
  }
  return ids;
}

QString visibleBenchmarkGroups(const QTreeWidget& tree) {
  QStringList marks;
  for (int groupIndex = 0; groupIndex < tree.topLevelItemCount(); ++groupIndex) {
    marks.append(tree.topLevelItem(groupIndex)->isHidden()
                     ? QStringLiteral("0") : QStringLiteral("1"));
  }
  return marks.join(QStringLiteral(""));
}

QStringList checkedBenchmarkModelIds(const QTreeWidget& tree) {
  QStringList ids;
  for (int groupIndex = 0; groupIndex < tree.topLevelItemCount(); ++groupIndex) {
    const QTreeWidgetItem* group = tree.topLevelItem(groupIndex);
    for (int index = 0; index < group->childCount(); ++index) {
      const QTreeWidgetItem* model = group->child(index);
      if (model->checkState(0) == Qt::Checked) {
        ids.append(model->data(0, Qt::UserRole + 1).toString());
      }
    }
  }
  return ids;
}

// The real lrelease output, never a stand-in: CMake defines its exact path and
// the same file also lands beside the test executable. The page tests prime the
// catalog independently so they pass alone and in any order.
QString builtCatalogPath() {
  QStringList candidates;
#ifdef MIKAI_UK_CATALOG_PATH
  candidates << QString::fromUtf8(MIKAI_UK_CATALOG_PATH);
#endif
  const QString applicationDirectory = QCoreApplication::applicationDirPath();
  if (!applicationDirectory.isEmpty()) {
    candidates << applicationDirectory + QStringLiteral("/mikai_uk.qm");
  }
  for (const QString& candidate : candidates) {
    if (QFileInfo::exists(candidate)) {
      return QFileInfo(candidate).absoluteFilePath();
    }
  }
  return candidates.isEmpty() ? QString() : candidates.first();
}

/// Load the real Ukrainian catalog and return to English, so the manager retains
/// an explicit replacement for the rest of the test.
bool primeBuiltCatalogForSwitching() {
  const QString catalog = builtCatalogPath();
  if (!QFileInfo::exists(catalog)) {
    return false;
  }
  return mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog) &&
         mikai::app::applyInterfaceLanguage(QStringLiteral("en"));
}

/// How many requests the canned server has answered once its current burst has
/// drained. A language switch must not move this number: the pages relabel what
/// they hold instead of asking the backend again.
int quietRequestCount(CannedApi& api) {
  QTest::qWait(250);
  int stable = api.requestCount();
  int quietWindows = 0;
  for (int poll = 0; poll < 60 && quietWindows < 3; ++poll) {
    QTest::qWait(50);
    const int current = api.requestCount();
    if (current == stable) {
      ++quietWindows;
    } else {
      stable = current;
      quietWindows = 0;
    }
  }
  return stable;
}

QPushButton* visibleButtonWithText(const QWidget& root, const QString& text) {
  for (QPushButton* button : root.findChildren<QPushButton*>()) {
    if (button->isVisible() && button->text() == text) {
      return button;
    }
  }
  return nullptr;
}

QPushButton* pagerArrow(const QWidget& pager, const QString& glyph) {
  for (QPushButton* button : pager.findChildren<QPushButton*>()) {
    if (button->text() == glyph) {
      return button;
    }
  }
  return nullptr;
}

/// The tab carrying the API's own filter value, or -1. Read from the public tab
/// data so an assertion never depends on the caption the active language uses.
int tabIndexWithData(const QTabBar& bar, const QString& data) {
  for (int index = 0; index < bar.count(); ++index) {
    if (bar.tabData(index).toString() == data) {
      return index;
    }
  }
  return -1;
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

void TestFeedRender::theEngineBuildGateWithholdsMeasurementUntilTheSlotIsConfigured() {
  constexpr int kPlanSlot = 1001;

  AnimeJanaiBuildGate gate;
  QVERIFY(!gate.readyToMeasure());
  QVERIFY(!gate.buildActive());
  QVERIFY(!gate.failed());

  // Build time must not contribute to the measured throughput window.
  const QString buildStart = QStringLiteral("building TensorRT engine for rife:v4.14");
  QCOMPARE(classifyAnimeJanaiBuildLog(QStringLiteral("vfi"), buildStart, Backend::TensorRT,
                                      kPlanSlot),
           AnimeJanaiBuildLogEvent::TensorRtBuildStarted);
  QCOMPARE(gate.observeLog(QStringLiteral("vfi"), buildStart, Backend::TensorRT, kPlanSlot),
           AnimeJanaiBuildLogEvent::TensorRtBuildStarted);
  QVERIFY(gate.buildActive());
  QVERIFY(!gate.readyToMeasure());

  QCOMPARE(gate.observeLog(QStringLiteral("vfi"), QStringLiteral("loading model graph"),
                           Backend::TensorRT, kPlanSlot),
            AnimeJanaiBuildLogEvent::None);
  QVERIFY(gate.buildActive());
  QVERIFY(!gate.readyToMeasure());

  const QString configured = QStringLiteral("configured slot %1: 1920x1080 -> 3840x2160")
                                .arg(kPlanSlot);
  QCOMPARE(gate.observeLog(QStringLiteral("aji"), configured, Backend::DirectML, kPlanSlot),
           AnimeJanaiBuildLogEvent::None);
  QVERIFY(gate.buildActive());
  QVERIFY(!gate.readyToMeasure());
  QCOMPARE(gate.observeLog(QStringLiteral("aji"), configured, Backend::TensorRT, kPlanSlot),
            AnimeJanaiBuildLogEvent::SlotConfigured);
  QVERIFY(gate.readyToMeasure());
  QVERIFY(!gate.buildActive());

  // A second build-start on the same core revokes it: a new engine is being
  // built for a different chain, and measuring now would time the compiler.
  QCOMPARE(gate.observeLog(QStringLiteral("aji"),
                           QStringLiteral("background engine build: spanf3 balanced"),
                           Backend::TensorRT, kPlanSlot),
            AnimeJanaiBuildLogEvent::TensorRtBuildStarted);
  QVERIFY(!gate.readyToMeasure());
  QVERIFY(gate.buildActive());
  QCOMPARE(gate.observeLog(QStringLiteral("aji"), configured, Backend::TensorRT, kPlanSlot),
            AnimeJanaiBuildLogEvent::SlotConfigured);
  QVERIFY(gate.readyToMeasure());

  // Once a build fails, later configuration messages cannot authorize a run.
  gate.fail();
  QVERIFY(gate.failed());
  QVERIFY(!gate.readyToMeasure());
  QVERIFY(!gate.buildActive());
  QCOMPARE(gate.observeLog(QStringLiteral("aji"), configured, Backend::TensorRT, kPlanSlot),
            AnimeJanaiBuildLogEvent::None);
  QVERIFY(gate.failed());
  QVERIFY(!gate.readyToMeasure());
}

void TestFeedRender::anAuthoritativeBackgroundBuildFailureSurfacesItsBoundedLogTail() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString enginePath =
      QDir(directory.path()).filePath(QStringLiteral("aji-71de4197.trt.engine"));
  const QString buildLogPath = enginePath + QStringLiteral(".build.log");
  QFile file(buildLogPath);
  QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
  const QByteArray log =
      "[10/18/2026-14:28:57] [E] Static model does not take explicit shapes since the shape of inference tensors "
      "will be determined by the model itself\n"
      "[10/18/2026-14:28:57] [E] Network And Config setup failed\n"
      "[10/18/2026-14:28:57] [E] Building engine failed\n"
      "[10/18/2026-14:28:57] [E] Building engine failed\n"
      "[10/18/2026-14:28:57] [E] Unable to create a tactic for node 42\n"
      "[aji] build process exit code: 1 (0x1)\n";
  QCOMPARE(file.write(log), static_cast<qint64>(log.size()));
  file.close();

  constexpr int slot = 1001;
  AnimeJanaiBuildGate gate;
  const auto observe = [&gate](const QString& prefix, const QString& line) {
    return gate.observeLog(prefix, line, Backend::TensorRT, slot);
  };
  QCOMPARE(observe(QStringLiteral("aji"), QStringLiteral("background engine build: spatial")),
           AnimeJanaiBuildLogEvent::TensorRtBuildStarted);
  QCOMPARE(observe(QStringLiteral("ffmpeg"), QStringLiteral("error while selecting stream")),
           AnimeJanaiBuildLogEvent::None);
  QVERIFY(gate.buildActive());
  QVERIFY(!gate.failed());

  const QString failure =
      QStringLiteral("[aji] background engine build failed: %1 (see .build.log)").arg(enginePath);
  QCOMPARE(observe(QStringLiteral("animejanai"), failure),
           AnimeJanaiBuildLogEvent::TensorRtBuildFailed);
  QVERIFY(gate.failed());
  QVERIFY(!gate.buildActive());
  QCOMPARE(gate.failureLogPath(), buildLogPath);

  const QStringList tail =
      mplayer::native::readAnimeJanaiBuildLogTail(gate.failureLogPath(), directory.path());
  QCOMPARE(tail.size(), 2);
  QCOMPARE(tail.at(0),
           QStringLiteral("[E] Static model does not take explicit shapes since the shape of "
                          "inference tensors will be determined by the model itself"));
  QCOMPARE(tail.at(1), QStringLiteral("[E] Unable to create a tactic for node 42"));
  QCOMPARE(tail.count(QStringLiteral("[E] Building engine failed")), 0);
  QVERIFY(mplayer::native::readAnimeJanaiBuildLogTail(
              gate.failureLogPath(), QDir(directory.path()).filePath(QStringLiteral("elsewhere")))
              .isEmpty());

  AnimeJanaiBuildGate diagnosticOnly;
  QCOMPARE(diagnosticOnly.observeLog(
               QStringLiteral("aji"), QStringLiteral("error: transient cplayer message"),
               Backend::TensorRT, slot),
           AnimeJanaiBuildLogEvent::None);
  QVERIFY(!diagnosticOnly.failed());
}

void TestFeedRender::interpolationOnlyRunsWaitForTheirSelectedSlot_data() {
  QTest::addColumn<bool>("coldBuild");
  QTest::newRow("cached-engine") << false;
  QTest::newRow("background-build") << true;
}

void TestFeedRender::interpolationOnlyRunsWaitForTheirSelectedSlot() {
  QFETCH(bool, coldBuild);
  constexpr int slot = 2101;
  AnimeJanaiBuildGate gate;
  const auto observe = [&gate](const QString& line, Backend backend = Backend::TensorRT) {
    return gate.observeLog(QStringLiteral("aji"), line, backend, slot);
  };
  const QString building = QStringLiteral("background engine build: rife_v4.7.onnx");
  const QString active = QStringLiteral("RIFE active: 2/1 interpolation");
  const QString selected =
      QStringLiteral("No chain active for 1280x720@23.976 slot 2101 (passthrough)");

  if (coldBuild) {
    observe(building);
    observe(active);
    observe(selected);
    QVERIFY(!gate.readyToMeasure());
  }
  observe(selected);
  observe(QStringLiteral("background engine build finished; reconfiguring"));
  QVERIFY(!gate.readyToMeasure());
  QCOMPARE(gate.buildActive(), coldBuild);

  observe(active, Backend::DirectML);
  observe(selected);
  QVERIFY(!gate.readyToMeasure());

  observe(active);
  observe(QStringLiteral("No chain active for 1280x720@23.976 slot 2102 (passthrough)"));
  QVERIFY(!gate.readyToMeasure());
  QCOMPARE(observe(selected), AnimeJanaiBuildLogEvent::SlotConfigured);
  QVERIFY(gate.readyToMeasure());
  QVERIFY(!gate.buildActive());

  observe(building);
  observe(active);
  observe(selected);
  QVERIFY(gate.buildActive());
  QVERIFY(!gate.readyToMeasure());
  observe(QStringLiteral("background engine build finished; reconfiguring"));
  observe(selected);
  QVERIFY(!gate.readyToMeasure());
  observe(active);
  observe(selected);
  QVERIFY(gate.readyToMeasure());

  gate.fail();
  observe(active);
  observe(selected);
  QVERIFY(gate.failed());
  QVERIFY(!gate.readyToMeasure());
}

void TestFeedRender::benchmarkTabKeepsGroupedModelAndClipSelectionsConsistent() {
  MikaiClient client(QString(), QStringLiteral("http://127.0.0.1:1"));
  Runner runner;
  SettingsDialog dialog(client, runner);

  auto* tabs = dialog.findChild<QTabWidget*>();
  auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("benchmarkModelTree"));
  QVERIFY(tabs != nullptr);
  QVERIFY(tree != nullptr);
  QCOMPARE(tabs->count(), 5);
  tabs->setCurrentIndex(4);

  QCheckBox* firstClip = nullptr;
  for (QCheckBox* check :
       dialog.findChildren<QCheckBox*>(QStringLiteral("benchmarkClipCheck"))) {
    if (check->property("benchmarkClipName").toString() == QStringLiteral("480x360.mp4")) {
      firstClip = check;
      break;
    }
  }
  QVERIFY(firstClip != nullptr);
  QVERIFY(firstClip->isChecked());

  QTRY_COMPARE(tree->topLevelItemCount(), 4);
  int totalModels = 0;
  int initiallyChecked = 0;
  for (int groupIndex = 0; groupIndex < tree->topLevelItemCount(); ++groupIndex) {
    const QTreeWidgetItem* group = tree->topLevelItem(groupIndex);
    totalModels += group->childCount();
    for (int index = 0; index < group->childCount(); ++index) {
      const QTreeWidgetItem* model = group->child(index);
      QVERIFY(model->flags() & Qt::ItemIsUserCheckable);
      initiallyChecked += model->checkState(0) == Qt::Checked ? 1 : 0;
    }
  }
  const QStringList ids = benchmarkModelIds(*tree);
  QVERIFY(ids.contains(QStringLiteral("anime4k:fast_a")));
  QVERIFY(!ids.contains(QStringLiteral("anime4k:off")));
  QCOMPARE(initiallyChecked, 3);

  QTreeWidgetItem* spatial = tree->topLevelItem(2);
  spatial->setCheckState(0, Qt::Checked);
  for (int index = 0; index < spatial->childCount(); ++index) {
    QCOMPARE(spatial->child(index)->checkState(0), Qt::Checked);
  }
  QCOMPARE(spatial->checkState(0), Qt::Checked);

  spatial->child(0)->setCheckState(0, Qt::Unchecked);
  QCOMPARE(spatial->checkState(0), Qt::PartiallyChecked);

  QPushButton* selectAllModels = nullptr;
  QPushButton* selectAllClips = nullptr;
  QPushButton* run = nullptr;
  for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
    const QString control = button->property("benchmarkSelectionControl").toString();
    if (control == QLatin1String("models")) selectAllModels = button;
    if (control == QLatin1String("clips")) selectAllClips = button;
    if (button->property("benchmarkRunAction").toBool()) run = button;
  }
  QVERIFY(selectAllModels != nullptr);
  QVERIFY(selectAllClips != nullptr);

  selectAllModels->click();
  int allModelsChecked = 0;
  for (int groupIndex = 0; groupIndex < tree->topLevelItemCount(); ++groupIndex) {
    QTreeWidgetItem* group = tree->topLevelItem(groupIndex);
    QCOMPARE(group->checkState(0), Qt::Checked);
    for (int index = 0; index < group->childCount(); ++index) {
      allModelsChecked += group->child(index)->checkState(0) == Qt::Checked ? 1 : 0;
    }
  }
  QCOMPARE(allModelsChecked, totalModels);

  selectAllModels->click();
  int noModelsChecked = 0;
  for (int groupIndex = 0; groupIndex < tree->topLevelItemCount(); ++groupIndex) {
    QTreeWidgetItem* group = tree->topLevelItem(groupIndex);
    QCOMPARE(group->checkState(0), Qt::Unchecked);
    for (int index = 0; index < group->childCount(); ++index) {
      noModelsChecked += group->child(index)->checkState(0) == Qt::Checked ? 1 : 0;
    }
  }
  QCOMPARE(noModelsChecked, 0);

  QCheckBox* raw = dialog.findChild<QCheckBox*>(QStringLiteral("benchmarkRawBox"));
  QVERIFY(raw != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(raw->isChecked());
  QVERIFY(run->isEnabled());

  selectAllClips->click();
  int checkedClips = 0;
  for (QCheckBox* clip : dialog.findChildren<QCheckBox*>(QStringLiteral("benchmarkClipCheck"))) {
    checkedClips += clip->isChecked() ? 1 : 0;
  }
  QCOMPARE(checkedClips, 0);
  QVERIFY(!run->isEnabled());

  selectAllClips->click();
  raw->setChecked(false);
  QVERIFY(!run->isEnabled());
  raw->setChecked(true);
  QVERIFY(run->isEnabled());
}

void TestFeedRender::benchmarkModelSearchFiltersRowsAndRestoresSelections() {
  MikaiClient client(QString(), QStringLiteral("http://127.0.0.1:1"));
  Runner runner;
  SettingsDialog dialog(client, runner);

  auto* tabs = dialog.findChild<QTabWidget*>();
  auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("benchmarkModelTree"));
  auto* search = dialog.findChild<QLineEdit*>(QStringLiteral("benchmarkModelSearch"));
  QVERIFY(tabs != nullptr);
  QVERIFY(tree != nullptr);
  QVERIFY2(search != nullptr, "the benchmark models card carries no search box to filter by");
  tabs->setCurrentIndex(4);

  QTRY_COMPARE(tree->topLevelItemCount(), 4);

  const QStringList allIds = benchmarkModelIds(*tree);
  const QStringList defaultChecked = checkedBenchmarkModelIds(*tree);
  QCOMPARE(defaultChecked.size(), 3);
  QVERIFY2(defaultChecked.contains(QStringLiteral("builtin1001")),
           qPrintable(QStringLiteral("the built-in defaults read: %1")
                          .arg(defaultChecked.join(QLatin1Char('|')))));

  QCOMPARE(visibleBenchmarkGroups(*tree), QStringLiteral("1111"));
  QCOMPARE(visibleBenchmarkModelIds(*tree), allIds);

  // A group name filters by group: the RIFE rows are the only ones whose text
  // says RIFE, and the search must keep that group's whole contents.
  search->setText(QStringLiteral("rife"));
  const QStringList rifeIds = visibleBenchmarkModelIds(*tree);
  QCOMPARE(rifeIds.size(), 47);
  QVERIFY2(std::all_of(rifeIds.begin(), rifeIds.end(),
                       [](const QString& id) {
                         return id.startsWith(QLatin1String("rife:"));
                       }),
           qPrintable(QStringLiteral("a non-RIFE row survived a RIFE filter: %1")
                          .arg(rifeIds.join(QLatin1Char('|')))));
  QCOMPARE(visibleBenchmarkGroups(*tree), QStringLiteral("0001"));

  // Case-insensitive: the same query in the other case must keep those rows.
  search->setText(QStringLiteral("RIFE"));
  QCOMPARE(visibleBenchmarkModelIds(*tree).size(), 47);

  // A model name filters by model rather than by group, and may match rows in
  // more than one group. Every row the spatial catalogue offers carries
  // "AnimeJaNai" in its own file name, as do the AnimeJaNai built-ins, so this
  // query must keep the AnimeJaNai group and the Spatial group and hide the
  // other two.
  search->setText(QStringLiteral("ANIMEJANAI"));
  const QStringList animeJaNaiIds = visibleBenchmarkModelIds(*tree);
  QCOMPARE(animeJaNaiIds.size(), 8);
  for (const QString& id : animeJaNaiIds) {
    QVERIFY2(id.startsWith(QLatin1String("builtin")) || id.startsWith(QLatin1String("spatial:")),
             qPrintable(QStringLiteral("a non-AnimeJaNai row survived: %1").arg(id)));
  }
  QCOMPARE(visibleBenchmarkGroups(*tree), QStringLiteral("0110"));

  // A query matching nothing hides every group and leaves the catalogue intact
  // underneath, so the count stays the whole catalogue rather than dropping.
  search->setText(QStringLiteral("zzzz-no-such-model"));
  QCOMPARE(visibleBenchmarkModelIds(*tree).size(), 0);
  QCOMPARE(visibleBenchmarkGroups(*tree), QStringLiteral("0000"));
  QCOMPARE(benchmarkModelIds(*tree), allIds);

  // Ticking a row a filter had hidden is the other half of the contract: a
  // selection made under a filter must survive having the filter removed, so
  // the run that follows measures what the viewer picked, not what is showing.
  search->setText(QStringLiteral("rife"));
  QCOMPARE(visibleBenchmarkModelIds(*tree).size(), 47);
  QTreeWidgetItem* rifeGroup = tree->topLevelItem(3);
  QVERIFY(rifeGroup != nullptr);
  QVERIFY(!rifeGroup->isHidden());
  const QString pickedUnderFilter = QStringLiteral("rife:rife_v4.14.onnx");
  QTreeWidgetItem* pickedRow = nullptr;
  for (int index = 0; index < rifeGroup->childCount(); ++index) {
    if (rifeGroup->child(index)->data(0, Qt::UserRole + 1).toString() == pickedUnderFilter) {
      pickedRow = rifeGroup->child(index);
      break;
    }
  }
  QVERIFY2(pickedRow != nullptr,
           qPrintable(QStringLiteral("the RIFE catalogue carries no %1 row").arg(pickedUnderFilter)));
  pickedRow->setCheckState(0, Qt::Checked);
  QCOMPARE(checkedBenchmarkModelIds(*tree).size(), 4);

  search->clear();
  QCOMPARE(visibleBenchmarkGroups(*tree), QStringLiteral("1111"));
  QCOMPARE(visibleBenchmarkModelIds(*tree), allIds);
  const QStringList restored = checkedBenchmarkModelIds(*tree);
  QCOMPARE(restored.size(), 4);
  for (const QString& id : defaultChecked) {
    QVERIFY2(restored.contains(id),
             qPrintable(QStringLiteral("clearing the search lost the %1 selection").arg(id)));
  }
  QVERIFY2(restored.contains(pickedUnderFilter),
           qPrintable(QStringLiteral("clearing the search lost a row ticked under a filter: %1")
                          .arg(restored.join(QLatin1Char('|')))));
}

void TestFeedRender::buildLogDetectionDistinguishesCompilationFromOtherBackendWork() {
  QCOMPARE(classifyAnimeJanaiBuildLog(QStringLiteral("vfi"),
                                      QStringLiteral("building tensorrt engine for "
                                                    "2x_AnimeJaNai_HD_V3.1_Balanced"),
                                      Backend::TensorRT, 1001),
           AnimeJanaiBuildLogEvent::TensorRtBuildStarted);
  QCOMPARE(classifyAnimeJanaiBuildLog(QStringLiteral("aji"),
                                      QStringLiteral("building engine: spanf3 balanced"),
                                      Backend::TensorRT, 1001),
           AnimeJanaiBuildLogEvent::TensorRtBuildStarted);
  QCOMPARE(classifyAnimeJanaiBuildLog(QStringLiteral("aji"),
                                      QStringLiteral("background engine build: rife v4.14"),
                                      Backend::TensorRT, 1001),
           AnimeJanaiBuildLogEvent::TensorRtBuildStarted);

  QCOMPARE(classifyAnimeJanaiBuildLog(QStringLiteral("aji"),
                                      QStringLiteral("building engine: spanf3 balanced"),
                                      Backend::DirectML, 1001),
           AnimeJanaiBuildLogEvent::None);
  QCOMPARE(classifyAnimeJanaiBuildLog(QStringLiteral("aji"),
                                      QStringLiteral("background engine build: rife v4.14"),
                                      Backend::DirectML, 1001),
           AnimeJanaiBuildLogEvent::None);

  QCOMPARE(classifyAnimeJanaiBuildLog(QStringLiteral("vfi"),
                                      QStringLiteral("estimated-frame-number: 120"),
                                      Backend::TensorRT, 1001),
           AnimeJanaiBuildLogEvent::None);
  QCOMPARE(classifyAnimeJanaiBuildLog(QStringLiteral("aji"),
                                      QStringLiteral("engine loaded from cache"),
                                      Backend::TensorRT, 1001),
           AnimeJanaiBuildLogEvent::None);
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
                                Q_ARG(bool, true));
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
  QVERIFY(acquisitionBar->findChild<QProgressBar*>() == nullptr);
  auto* acquisitionStatus = acquisitionBar->findChild<QLabel*>(QStringLiteral("status"));
  QVERIFY(acquisitionStatus != nullptr);
  QVERIFY(acquisitionStatus->wordWrap());
  QVERIFY(acquisitionBar->findChild<QPushButton*>(
              QStringLiteral("componentAcquisitionDetails")) != nullptr);
  QVERIFY(acquisitionBar->findChild<QPushButton*>(
              QStringLiteral("componentAcquisitionCancel")) != nullptr);
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

void TestFeedRender::loadedResultsRetranslateWithoutRefetchingOrLosingViewState() {
  const QString openingLanguage = mikai::app::activeInterfaceLanguage();
  QSettings settings;
  const QString languageKey = QString::fromLatin1(mikai::app::kUiLanguageSettingKey);
  settings.sync();
  const bool storedLanguagePresent = settings.contains(languageKey);
  const QVariant storedLanguage = settings.value(languageKey);
  const auto restoreState = qScopeGuard([&settings, languageKey, storedLanguagePresent,
                                         storedLanguage, openingLanguage] {
    if (storedLanguagePresent) {
      settings.setValue(languageKey, storedLanguage);
    } else {
      settings.remove(languageKey);
    }
    settings.sync();
    mikai::app::applyInterfaceLanguage(openingLanguage);
    QCoreApplication::processEvents();
  });
  Q_UNUSED(restoreState);

  QVERIFY2(primeBuiltCatalogForSwitching(),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") +
                      builtCatalogPath()));
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));

  CannedApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));

  MikaiClient client(QString(), api.baseUrl());
  Runner runner;
  SearchPage page(runner, client);
  page.resize(1000, 560);
  page.show();

  // The home feed has to finish before the search replaces the discovery view,
  // so the assertions below cover both an already-loaded feed and an already
  // loaded result list.
  QTRY_VERIFY_WITH_TIMEOUT(page.findChildren<TitleCard*>().size() >= kCards, 30000);

  auto* query = page.findChild<QLineEdit*>(QStringLiteral("searchQuery"));
  auto* results = page.findChild<QListWidget*>(QStringLiteral("searchResults"));
  auto* searchButton = page.findChild<QPushButton*>(QStringLiteral("primary"));
  auto* status = page.findChild<QLabel*>(QStringLiteral("status"));
  QVERIFY2(query != nullptr && results != nullptr && searchButton != nullptr &&
               status != nullptr,
           "the search page does not expose its query, results, action and status");

  QTest::keyClicks(query, QStringLiteral("canned-many"));
  QTest::keyClick(query, Qt::Key_Return);
  QTRY_COMPARE_WITH_TIMEOUT(results->count(), 30, 30000);
  QTRY_VERIFY_WITH_TIMEOUT(results->isVisible(), 5000);

  // The canned answer carries two formats, so the page offers a real filter.
  auto* formatBar = page.findChild<QTabBar*>();
  QVERIFY2(formatBar != nullptr, "the search page has no format bar");
  QTRY_VERIFY_WITH_TIMEOUT(formatBar->isVisible(), 5000);

  // The bar's own "All (n)" caption is app wording, while the counts and the
  // selected filter are state. One switch here proves the visible filter tab
  // survives; the filtered pass below proves its result set does.
  QVERIFY(formatBar->count() >= 3);
  QCOMPARE(formatBar->tabData(formatBar->currentIndex()).toString(), QStringLiteral("all"));
  const int formatTabsBefore = formatBar->count();
  const QString allTabEnglish = formatBar->tabText(0);
  const int requestsBeforeBarSwitch = quietRequestCount(api);
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  QCoreApplication::processEvents();
  QCOMPARE(formatBar->count(), formatTabsBefore);
  QCOMPARE(formatBar->tabData(formatBar->currentIndex()).toString(), QStringLiteral("all"));
  QVERIFY2(formatBar->tabText(0) != allTabEnglish,
           "the format bar kept the language the result set was searched in");
  QCOMPARE(formatBar->tabText(0),
           QCoreApplication::translate("mikai::app::SearchPage", "All (%1)").arg(30));
  QCOMPARE(results->count(), 30);
  QCOMPARE(api.requestCount(), requestsBeforeBarSwitch);
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();
  QCOMPARE(formatBar->tabText(0), allTabEnglish);
  QCOMPARE(formatBar->tabData(formatBar->currentIndex()).toString(), QStringLiteral("all"));
  QCOMPARE(results->count(), 30);
  QCOMPARE(api.requestCount(), requestsBeforeBarSwitch);

  int movieTab = -1;
  for (int index = 0; index < formatBar->count(); ++index) {
    if (formatBar->tabData(index).toString() == QLatin1String("movie")) {
      movieTab = index;
      break;
    }
  }
  QVERIFY2(movieTab >= 0, "the format bar did not offer the canned movie rows");
  formatBar->setCurrentIndex(movieTab);

  // The filter is the server's: the filtered answer replaces the set, and its
  // rows are the format that was asked for.
  QTRY_COMPARE_WITH_TIMEOUT(results->count(), 15, 30000);
  for (int row = 0; row < results->count(); ++row) {
    QVERIFY2(results->item(row)->text().contains(QStringLiteral("MOVIE")),
             qPrintable(QStringLiteral("row %1 is not from the filtered set: %2")
                            .arg(row)
                            .arg(results->item(row)->text())));
  }

  // A filtered answer carries one format, but the control that produced it must
  // stay on screen: without its All tab the only way back to the whole set is a
  // new search the reader cannot ask for. The active tab is the filter itself.
  QTRY_VERIFY_WITH_TIMEOUT(formatBar->isVisible(), 5000);
  QCOMPARE(formatBar->tabData(formatBar->currentIndex()).toString(), QStringLiteral("movie"));
  QVERIFY2(tabIndexWithData(*formatBar, QStringLiteral("all")) >= 0,
           "the filtered format bar has no All escape");

  results->setCurrentRow(6);
  QCOMPARE(results->currentRow(), 6);
  QScrollBar* resultsBar = results->verticalScrollBar();
  // Fifteen filtered rows can fit the whole result viewport on a tall window,
  // and a bar with no range would let a language switch reset the scroll
  // invisibly. Cap the real list to a handful of its own rows - the rows keep
  // the height the view gives them - and let the layout apply the cap, so the
  // assertions below read a scrollbar with an actual range to preserve.
  int resultRowHeight = results->sizeHintForRow(0);
  if (resultRowHeight <= 0 && results->item(0) != nullptr) {
    resultRowHeight = results->visualItemRect(results->item(0)).height();
  }
  QVERIFY2(resultRowHeight > 0, "the result rows have no measurable height");
  results->setMaximumHeight(results->frameWidth() * 2 + resultRowHeight * 5);
  QCoreApplication::processEvents();
  QTRY_VERIFY_WITH_TIMEOUT(resultsBar->maximum() > 0, 5000);
  QVERIFY2(resultsBar->maximum() > 0,
           "the filtered result set does not fill the view, so a scroll reset would be invisible");
  resultsBar->setValue(qMin(180, resultsBar->maximum()));
  const int resultsScrollBefore = resultsBar->value();
  QVERIFY(resultsScrollBefore > 0);

  const QString englishSelected = results->item(6)->text();
  const QString englishSearch = searchButton->text();
  const QString englishPlaceholder = query->placeholderText();
  QLabel* resultsHeading = nullptr;
  for (QLabel* label : page.findChildren<QLabel*>(QStringLiteral("sectionHeading"))) {
    if (label->text() == QStringLiteral("Search results")) {
      resultsHeading = label;
      break;
    }
  }
  QVERIFY2(resultsHeading != nullptr, "the search page did not label the result mode");

  // From here on only words may move. The settings dialog is the real surface:
  // its selector installs the preview into the running page before Save.
  SettingsDialog dialog(client, runner);
  dialog.show();
  QCoreApplication::processEvents();
  auto* language = dialog.findChild<QComboBox*>(QStringLiteral("uiLanguageCombo"));
  QVERIFY2(language != nullptr, "the settings surface has no language selector");
  const int english = language->findData(QStringLiteral("en"));
  const int ukrainian = language->findData(QStringLiteral("uk"));
  QVERIFY(english >= 0 && ukrainian >= 0);
  language->setCurrentIndex(english);
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));

  const int requestsBeforeSwitch = quietRequestCount(api);
  const int cardsBefore = page.findChildren<TitleCard*>().size();
  QVERIFY(cardsBefore > 0);

  language->setCurrentIndex(ukrainian);
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("uk"));

  const QString ukrainianSearch =
      QCoreApplication::translate("mikai::app::SearchPage", "Search");
  QVERIFY2(ukrainianSearch != englishSearch,
           "the catalog did not translate the search action, so this slot proves nothing");
  QCOMPARE(searchButton->text(), ukrainianSearch);
  QCOMPARE(query->placeholderText(),
           QCoreApplication::translate("mikai::app::SearchPage",
                                       "Search by title — Enter or the button"));
  QCOMPARE(resultsHeading->text(),
           QCoreApplication::translate("mikai::app::SearchPage", "Search results"));
  QCOMPARE(status->text(),
           QCoreApplication::translate("mikai::app::SearchPage", "Found: %1").arg(15));

  // State, not wording: the query, the filtered set, the selected row and the
  // scroll stay exactly where the reader left them.
  QCOMPARE(query->text(), QStringLiteral("canned-many"));
  QCOMPARE(results->count(), 15);
  QCOMPARE(results->currentRow(), 6);
  QCOMPARE(resultsBar->value(), resultsScrollBefore);
  QCOMPARE(page.findChildren<TitleCard*>().size(), cardsBefore);
  for (int row = 0; row < results->count(); ++row) {
    QVERIFY2(results->item(row)->text().contains(QStringLiteral("MOVIE")),
             qPrintable(QStringLiteral("the filter was lost on row %1: %2")
                            .arg(row)
                            .arg(results->item(row)->text())));
  }
  const QString ukrainianSelected = results->item(6)->text();
  QVERIFY2(ukrainianSelected != englishSelected,
           "the selected row still shows the language the page was searched in");
  QVERIFY2(ukrainianSelected.startsWith(QStringLiteral("Canned 7")),
           qPrintable(QStringLiteral("the API title was rewritten: %1").arg(ukrainianSelected)));
  QVERIFY(ukrainianSelected.contains(QStringLiteral("MOVIE")));
  QVERIFY2(ukrainianSelected.contains(QCoreApplication::translate(
               "mikai::app::SearchPage", "%n episode(s)", nullptr, kEpisodes)),
           qPrintable(QStringLiteral("the row's episode caption was not restated: %1")
                          .arg(ukrainianSelected)));

  // The visible control and the format it names survive the switch too.
  QVERIFY(formatBar->isVisible());
  QCOMPARE(formatBar->tabData(formatBar->currentIndex()).toString(), QStringLiteral("movie"));
  QVERIFY(tabIndexWithData(*formatBar, QStringLiteral("all")) >= 0);

  // Nothing was fetched to translate: the filtered result set is still the one
  // in hand.
  QCOMPARE(api.requestCount(), requestsBeforeSwitch);

  language->setCurrentIndex(english);
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));
  QCOMPARE(searchButton->text(), englishSearch);
  QCOMPARE(query->placeholderText(), englishPlaceholder);
  QCOMPARE(resultsHeading->text(), QStringLiteral("Search results"));
  QCOMPARE(status->text(), QStringLiteral("Found: 15"));
  QCOMPARE(results->item(6)->text(), englishSelected);
  QCOMPARE(results->currentRow(), 6);
  QCOMPARE(resultsBar->value(), resultsScrollBefore);
  QVERIFY(formatBar->isVisible());
  QCOMPARE(formatBar->tabData(formatBar->currentIndex()).toString(), QStringLiteral("movie"));
  QVERIFY(tabIndexWithData(*formatBar, QStringLiteral("all")) >= 0);
  QCOMPARE(api.requestCount(), requestsBeforeSwitch);

  dialog.reject();
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));
  settings.sync();
  QCOMPARE(settings.contains(languageKey), storedLanguagePresent);
  QCOMPARE(settings.value(languageKey), storedLanguage);

  // The escape has to work, and it is the one place this slot expects a new
  // query: the reader explicitly asked for the unfiltered set. Everything a
  // language switch did before this point issued nothing.
  QVERIFY2(formatBar->isVisible(),
           "the filter control disappeared, so the All escape could not be pressed");
  QCOMPARE(formatBar->tabData(formatBar->currentIndex()).toString(), QStringLiteral("movie"));
  const int requestsBeforeAll = quietRequestCount(api);
  QCOMPARE(requestsBeforeAll, requestsBeforeSwitch);
  const int pathsBeforeAll = api.requestedPaths().size();
  const int allTab = tabIndexWithData(*formatBar, QStringLiteral("all"));
  QVERIFY2(allTab >= 0, "the visible format bar offers no All tab");
  formatBar->setCurrentIndex(allTab);

  // The server's unfiltered answer is back, and the bar names it as active.
  QTRY_COMPARE_WITH_TIMEOUT(results->count(), 30, 30000);
  QTRY_VERIFY_WITH_TIMEOUT(formatBar->isVisible(), 5000);
  QCOMPARE(formatBar->tabData(formatBar->currentIndex()).toString(), QStringLiteral("all"));
  int tvRows = 0;
  int movieRows = 0;
  for (int row = 0; row < results->count(); ++row) {
    const QString text = results->item(row)->text();
    if (text.contains(QStringLiteral(" · TV · "))) {
      ++tvRows;
    }
    if (text.contains(QStringLiteral(" · MOVIE · "))) {
      ++movieRows;
    }
  }
  QVERIFY2(tvRows > 0, "All did not return the TV rows");
  QVERIFY2(movieRows > 0, "All did not return the MOVIE rows");
  QCOMPARE(query->text(), QStringLiteral("canned-many"));

  // Exactly one unfiltered search was issued for that click; the page's own
  // per-format counts and poster requests are not searches.
  const int requestsAfterAll = quietRequestCount(api);
  int newUnfilteredSearches = 0;
  const QStringList newPaths = api.requestedPaths();
  for (int index = pathsBeforeAll; index < newPaths.size(); ++index) {
    const QString& path = newPaths.at(index);
    if (path.contains(QStringLiteral("search=canned-many")) &&
        !path.contains(QStringLiteral("formats="))) {
      ++newUnfilteredSearches;
    }
  }
  QCOMPARE(newUnfilteredSearches, 1);

  // A switch after All is still only words: the unfiltered set and the All
  // selection stay, and no request follows.
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  QCoreApplication::processEvents();
  QCOMPARE(api.requestCount(), requestsAfterAll);
  QVERIFY(formatBar->isVisible());
  QCOMPARE(formatBar->tabData(formatBar->currentIndex()).toString(), QStringLiteral("all"));
  QCOMPARE(results->count(), 30);

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();
  QCOMPARE(api.requestCount(), requestsAfterAll);
  QVERIFY(formatBar->isVisible());
  QCOMPARE(formatBar->tabData(formatBar->currentIndex()).toString(), QStringLiteral("all"));
  QCOMPARE(results->count(), 30);
  QCOMPARE(query->text(), QStringLiteral("canned-many"));
}

void TestFeedRender::loadedTitlePageRetranslatesWithoutRefetchingOrLosingDisclosureState() {
  const QString openingLanguage = mikai::app::activeInterfaceLanguage();
  QSettings settings;
  const QString languageKey = QString::fromLatin1(mikai::app::kUiLanguageSettingKey);
  settings.sync();
  const bool storedLanguagePresent = settings.contains(languageKey);
  const QVariant storedLanguage = settings.value(languageKey);
  const auto restoreState = qScopeGuard([&settings, languageKey, storedLanguagePresent,
                                         storedLanguage, openingLanguage] {
    if (storedLanguagePresent) {
      settings.setValue(languageKey, storedLanguage);
    } else {
      settings.remove(languageKey);
    }
    settings.sync();
    mikai::app::applyInterfaceLanguage(openingLanguage);
    QCoreApplication::processEvents();
  });
  Q_UNUSED(restoreState);

  QVERIFY2(primeBuiltCatalogForSwitching(),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") +
                      builtCatalogPath()));
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));

  CannedApi api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  // Three pages of pills, so the episode pager has a selection to preserve.
  api.setReleaseEpisodeCount(300);

  MikaiClient client(QString(), api.baseUrl());
  Runner runner;
  QualityCache cache;
  TitlePage page(runner, client, cache, QString::number(kRef));
  page.resize(1100, 620);
  page.show();

  auto* heading = page.findChild<QLabel*>(QStringLiteral("titleHeading"));
  auto* description = page.findChild<QLabel*>(QStringLiteral("titleDescription"));
  auto* details = page.findChild<QWidget*>(QStringLiteral("titleDetails"));
  auto* episodesHeading = page.findChild<QLabel*>(QStringLiteral("episodeSectionHeading"));
  auto* overview = page.findChild<QScrollArea*>(QStringLiteral("titleOverview"));
  auto* tabs = page.findChild<QTabWidget*>(QStringLiteral("episodeTabs"));
  QVERIFY2(heading != nullptr && description != nullptr && details != nullptr &&
               episodesHeading != nullptr && overview != nullptr && tabs != nullptr,
           "the title page does not expose its heading, description, details or episodes");

  QTRY_COMPARE_WITH_TIMEOUT(heading->text(), QStringLiteral("Canned 1"), 30000);
  QTRY_VERIFY_WITH_TIMEOUT(page.findChildren<QWidget*>(QStringLiteral("episodeButton")).size() > 0,
                           30000);

  // The API's description is longer than its preview, so its disclosure starts
  // collapsed and can be opened for real.
  QTRY_VERIFY_WITH_TIMEOUT(visibleButtonWithText(page, QStringLiteral("Read more")) != nullptr,
                           5000);
  QPushButton* descriptionButton = visibleButtonWithText(page, QStringLiteral("Read more"));
  QVERIFY(descriptionButton != nullptr);
  QVERIFY2(description->text() != cannedDescription(),
           "the canned description fits its preview, so expanding it would be invisible");
  QVERIFY(description->text().endsWith(QStringLiteral("…")));
  // The overview is scrollable; the action has to be inside its viewport before
  // a real click can reach it.
  overview->ensureWidgetVisible(descriptionButton, 0, 0);
  QCoreApplication::processEvents();
  QTest::mouseClick(descriptionButton, Qt::LeftButton);
  QCoreApplication::processEvents();
  QCOMPARE(description->text(), cannedDescription());
  QCOMPARE(descriptionButton->text(), QStringLiteral("Collapse description"));

  // The metadata disclosure, open, with its own caption.
  QPushButton* metaButton = visibleButtonWithText(page, QStringLiteral("More details"));
  QVERIFY(metaButton != nullptr);
  overview->ensureWidgetVisible(metaButton, 0, 0);
  QCoreApplication::processEvents();
  QTest::mouseClick(metaButton, Qt::LeftButton);
  QCoreApplication::processEvents();
  QTRY_VERIFY_WITH_TIMEOUT(details->isVisible(), 5000);
  QCOMPARE(metaButton->text(), QStringLiteral("Hide details"));

  // A release other than the first is selected, and its chip is what the
  // language switch has to keep checked.
  const QList<QPushButton*> chips =
      page.findChildren<QPushButton*>(QStringLiteral("releaseChip"));
  QCOMPARE(chips.size(), 2);
  QPushButton* firstChip = chips.at(0);
  QPushButton* secondChip = chips.at(1);
  QVERIFY(firstChip->isChecked());
  QTest::mouseClick(secondChip, Qt::LeftButton);
  QCoreApplication::processEvents();
  QVERIFY(secondChip->isChecked());
  QVERIFY(!firstChip->isChecked());

  QWidget* currentTab = tabs->currentWidget();
  QVERIFY(currentTab != nullptr);
  RangePager* pager = currentTab->findChild<RangePager*>();
  QVERIFY2(pager != nullptr, "the episode tab has no pager");
  // Three or more pages means the page after the first is a full one, so its
  // pill count follows from the first page instead of a stride pinned here.
  QTRY_VERIFY_WITH_TIMEOUT(pager->pages() >= 3, 5000);

  // The visible pills are the only honest source for the page stride: the grid
  // decides how many fit and the payload decides what they read. The fixture
  // labels its episodes "Episode N", so the pill that follows the first page is
  // derived from what is on screen instead of a page-size constant pinned here.
  const auto pillTexts = [&page] {
    QStringList texts;
    for (QWidget* widget : page.findChildren<QWidget*>(QStringLiteral("episodeButton"))) {
      auto* pill = qobject_cast<QPushButton*>(widget);
      Q_ASSERT(pill != nullptr);
      texts.append(pill->text());
    }
    texts.sort();
    return texts;
  };
  // A render that follows a release switch leaves the previous pills queued for
  // deletion, so the grid is read once its contents stop moving rather than at
  // whatever count this slot happened to catch.
  QStringList firstPagePills = pillTexts();
  for (int stable = 0; stable < 3;) {
    QTest::qWait(20);
    const QStringList now = pillTexts();
    stable = (now == firstPagePills) ? stable + 1 : 0;
    firstPagePills = now;
  }
  QVERIFY2(!firstPagePills.isEmpty(), "the title page rendered no episode pills");
  const int pageSize = firstPagePills.size();
  QVERIFY2(pageSize > 0 && pageSize < 300,
           qPrintable(QStringLiteral("the canned release no longer needs a second episode page "
                                     "(%1 pills on the first)")
                          .arg(pageSize)));
  const QString secondPageStart = QStringLiteral("Episode %1").arg(pageSize + 1);

  // The second episode page: a page selection, not just the first page.
  QPushButton* nextPage = pagerArrow(*pager, QStringLiteral("→"));
  QVERIFY(nextPage != nullptr);
  QTest::mouseClick(nextPage, Qt::LeftButton);
  QTRY_COMPARE_WITH_TIMEOUT(pager->page(), 2, 5000);
  // The departing pills are deleted through the event loop, so wait for the
  // grid to hold exactly one page again before reading it.
  QTRY_COMPARE_WITH_TIMEOUT(pillTexts().size(), pageSize, 10000);
  QTRY_VERIFY_WITH_TIMEOUT(pillTexts().contains(secondPageStart), 10000);
  const QStringList pillTextsBefore = pillTexts();
  QVERIFY2(!pillTextsBefore.contains(firstPagePills.constFirst()),
           "the second page still shows the first page's pills");

  QCoreApplication::processEvents();
  QTest::qWait(100);
  auto* episodeScroll = currentTab->findChild<QScrollArea*>(QStringLiteral("episodeScroll"));
  QVERIFY(episodeScroll != nullptr);
  QWidget* episodeGrid = episodeScroll->widget();
  QVERIFY2(episodeGrid != nullptr, "the episode scroll holds no grid to measure");
  // The section is clamped to a fixed maximum height and the grid is sized to
  // its content, so a wide window can show a whole page of pills and leave the
  // bar with no range for a language switch to reset. Constrain the real
  // viewport to a few pill rows and keep the grid taller than any viewport the
  // page can ask for, using the pills' own geometry and the layout's spacing.
  const QList<QPushButton*> pagePills =
      currentTab->findChildren<QPushButton*>(QStringLiteral("episodeButton"));
  QVERIFY2(!pagePills.isEmpty(), "the episode grid holds no pill to measure");
  int pillRowSpan = pagePills.constFirst()->sizeHint().height();
  if (auto* gridLayout = qobject_cast<QGridLayout*>(episodeGrid->layout())) {
    pillRowSpan += qMax(0, gridLayout->verticalSpacing());
  }
  QVERIFY2(pillRowSpan > 0, "the episode pills have no measurable row height");
  const int episodeFrame = episodeScroll->frameWidth();
  const int gridHeight = qMax(episodeGrid->height(), episodeGrid->sizeHint().height());
  episodeScroll->setFixedHeight(episodeFrame * 2 + pillRowSpan * 3);
  episodeGrid->setMinimumHeight(gridHeight + pillRowSpan * 4);
  QCoreApplication::processEvents();
  QScrollBar* episodeBar = episodeScroll->verticalScrollBar();
  QTRY_VERIFY_WITH_TIMEOUT(episodeBar->maximum() > 0, 5000);
  QVERIFY2(episodeBar->maximum() > 0,
           "the episode grid does not fill its view, so a scroll reset would be invisible");
  episodeBar->setValue(qMin(40, episodeBar->maximum()));
  const int episodeScrollBefore = episodeBar->value();
  QVERIFY(episodeScrollBefore > 0);

  QScrollBar* overviewBar = overview->verticalScrollBar();
  QVERIFY2(overviewBar->maximum() > 0,
           "the title overview does not fill its view, so a scroll reset would be invisible");
  overviewBar->setValue(qMin(60, overviewBar->maximum()));
  const int overviewScrollBefore = overviewBar->value();
  QVERIFY(overviewScrollBefore > 0);

  // Everything the switch must leave alone, captured in the opening language
  // after the release's quality probe has settled.
  const int requestsBeforeSwitch = quietRequestCount(api);
  const int episodePagesBeforeSwitch = pager->pages();
  const QString chipLabelBefore = secondChip->text();
  const QString chipTipBefore = secondChip->toolTip();
  const QString descriptionBefore = description->text();
  const QString descriptionActionBefore = descriptionButton->text();
  const QString metaActionBefore = metaButton->text();
  const QString episodesHeadingBefore = episodesHeading->text();
  QCOMPARE(episodesHeadingBefore, QStringLiteral("Episodes"));
  QCOMPARE(descriptionActionBefore, QStringLiteral("Collapse description"));
  QCOMPARE(metaActionBefore, QStringLiteral("Hide details"));

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  QCoreApplication::processEvents();

  // The app's own captions are restated...
  const QString ukrainianDescriptionAction =
      QCoreApplication::translate("mikai::app::TitlePage", "Collapse description");
  const QString ukrainianMetaAction =
      QCoreApplication::translate("mikai::app::TitlePage", "Hide details");
  const QString ukrainianEpisodes =
      QCoreApplication::translate("mikai::app::TitlePage", "Episodes");
  QVERIFY2(ukrainianDescriptionAction != descriptionActionBefore,
           "the catalog did not translate the description action, so this slot proves nothing");
  QCOMPARE(descriptionButton->text(), ukrainianDescriptionAction);
  QCOMPARE(metaButton->text(), ukrainianMetaAction);
  QCOMPARE(episodesHeading->text(), ukrainianEpisodes);

  // ...while the title, the description and the release's team name are API
  // data and read exactly as they arrived.
  QCOMPARE(heading->text(), QStringLiteral("Canned 1"));
  QCOMPARE(description->text(), descriptionBefore);
  QVERIFY(details->isVisible());
  QVERIFY(secondChip->isChecked());
  QVERIFY(!firstChip->isChecked());
  QCOMPARE(secondChip->text(), chipLabelBefore);
  QVERIFY2(secondChip->toolTip() != chipTipBefore,
           "the release chip's own kind caption was not restated");
  QVERIFY2(secondChip->text().contains(QStringLiteral("Test Sub")),
           "the release's team name was rewritten");
  QVERIFY2(secondChip->toolTip().contains(QStringLiteral("Test Sub")),
           "the release tooltip lost the team name");
  QVERIFY2(secondChip->toolTip().contains(
               QCoreApplication::translate("Labels", "Subtitled")),
           qPrintable(QStringLiteral("the release tooltip kept its old kind: %1")
                          .arg(secondChip->toolTip())));

  // The episode page, its pills and both scrolls are where they were.
  QCOMPARE(pager->page(), 2);
  QCOMPARE(pager->pages(), episodePagesBeforeSwitch);
  QStringList pillTextsAfter;
  for (QWidget* widget : page.findChildren<QWidget*>(QStringLiteral("episodeButton"))) {
    auto* pill = qobject_cast<QPushButton*>(widget);
    QVERIFY(pill != nullptr);
    pillTextsAfter.append(pill->text());
  }
  pillTextsAfter.sort();
  QCOMPARE(pillTextsAfter, pillTextsBefore);
  QCOMPARE(episodeBar->value(), episodeScrollBefore);
  QCOMPARE(overviewBar->value(), overviewScrollBefore);
  QCOMPARE(api.requestCount(), requestsBeforeSwitch);

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();

  QCOMPARE(descriptionButton->text(), descriptionActionBefore);
  QCOMPARE(metaButton->text(), metaActionBefore);
  QCOMPARE(episodesHeading->text(), episodesHeadingBefore);
  QCOMPARE(heading->text(), QStringLiteral("Canned 1"));
  QCOMPARE(description->text(), descriptionBefore);
  QVERIFY(details->isVisible());
  QVERIFY(secondChip->isChecked());
  QCOMPARE(secondChip->text(), chipLabelBefore);
  QCOMPARE(secondChip->toolTip(), chipTipBefore);
  QCOMPARE(pager->page(), 2);
  QCOMPARE(pager->pages(), episodePagesBeforeSwitch);
  QCOMPARE(episodeBar->value(), episodeScrollBefore);
  QCOMPARE(overviewBar->value(), overviewScrollBefore);
  QCOMPARE(api.requestCount(), requestsBeforeSwitch);

  settings.sync();
  QCOMPARE(settings.contains(languageKey), storedLanguagePresent);
  QCOMPARE(settings.value(languageKey), storedLanguage);
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
