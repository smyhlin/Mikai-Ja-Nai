#include "TestPublicApiUi.hpp"

#include <backend/MikaiClient.hpp>
#include <dialogs/SettingsDialog.hpp>
#include <widgets/PageGlow.hpp>
#include <state/AppearanceStore.hpp>
#include <pages/MainWindow.hpp>
#include <pages/SearchPage.hpp>
#include <pages/TitlePage.hpp>
#include <state/QualityCache.hpp>
#include <state/WatchHistory.hpp>
#include <support/I18n.hpp>
#include <theme/Theme.hpp>
#include <workers/Runner.hpp>
#include <gui/ImageDecode.hpp>

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QScopeGuard>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTemporaryFile>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtGui/QDesktopServices>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QTextBlock>
#include <QtGui/QTextDocument>
#include <QtGui/QTextFragment>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QSignalSpy>
#include <QtWidgets/QApplication>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QDialog>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSlider>
#include <cstdio>
#include <functional>

namespace {

const QString kFixtureKey = QStringLiteral("mk_public_api_ui_fixture");

class BrowserUrlCapture final : public QObject {
  Q_OBJECT

 public:
  BrowserUrlCapture() {
    QDesktopServices::setUrlHandler(QStringLiteral("http"), this, "capture");
    QDesktopServices::setUrlHandler(QStringLiteral("https"), this, "capture");
  }
  ~BrowserUrlCapture() override {
    QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
    QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
  }
  QList<QUrl> opened;

 public slots:
  void capture(const QUrl& url) { opened.append(url); }
};

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

bool primeBuiltCatalogForSwitching() {
  const QString catalog = builtCatalogPath();
  if (!QFileInfo::exists(catalog)) {
    return false;
  }
  return mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog) &&
         mikai::app::applyInterfaceLanguage(QStringLiteral("en"));
}

QJsonObject imageSet(const QString& url) {
  QJsonObject variant{{QStringLiteral("webp"), url}, {QStringLiteral("jpg"), url}};
  return QJsonObject{{QStringLiteral("big"), variant}, {QStringLiteral("medium"), variant},
                     {QStringLiteral("small"), variant}};
}

QJsonObject titlePayload(const QString& posterUrl, const QString& selectedPosterUrl) {
  QJsonObject posters{{QStringLiteral("poster"), imageSet(posterUrl)}};
  QJsonObject selected{{QStringLiteral("id"), 9}, {QStringLiteral("isSelected"), true},
                       {QStringLiteral("author"), QStringLiteral("Poster Artist")},
                       {QStringLiteral("team"),
                        QJsonObject{{QStringLiteral("id"), 17},
                                    {QStringLiteral("slug"), QStringLiteral("fixture-team")},
                                    {QStringLiteral("name"), QStringLiteral("Fixture Team")}}},
                       {QStringLiteral("images"), imageSet(selectedPosterUrl)}};
  QJsonObject detail{
      {QStringLiteral("ids"), QJsonObject{{QStringLiteral("mikai"), 7428}}},
      {QStringLiteral("titles"), QJsonObject{{QStringLiteral("ua"), QStringLiteral("Probe")},
                                              {QStringLiteral("english"), QStringLiteral("Probe")},
                                              {QStringLiteral("original"), QStringLiteral("Probe")}}},
      {QStringLiteral("description"), QStringLiteral("Fixture description")},
      {QStringLiteral("format"), QStringLiteral("tv")},
      {QStringLiteral("status"), QStringLiteral("ongoing")},
      {QStringLiteral("episodes"), QJsonObject{{QStringLiteral("total"), 0},
                                                 {QStringLiteral("aired"), 2},
                                                 {QStringLiteral("localized"), 0}}},
      {QStringLiteral("images"), posters},
      {QStringLiteral("uaPosters"), QJsonArray{selected}},
      {QStringLiteral("episodeTypes"),
       QJsonObject{{QStringLiteral("animeCanon"), QJsonArray{QJsonObject{
                        {QStringLiteral("from"), 1}, {QStringLiteral("to"), 1}}}}}},
      {QStringLiteral("nextEpisode"), QJsonObject{{QStringLiteral("episode"), 3},
                                                    {QStringLiteral("airing"), QStringLiteral("2099-06-01T12:00:00Z")}}}};
  return detail;
}

QJsonObject releasePayload() {
  QJsonObject team{{QStringLiteral("id"), 17}, {QStringLiteral("slug"), QStringLiteral("fixture-team")},
                   {QStringLiteral("name"), QStringLiteral("Fixture Team")}};
  QJsonArray teams{team};
  QJsonArray episodes;
  for (int n = 1; n <= 3; ++n) {
    const QJsonObject source{{QStringLiteral("provider"), QStringLiteral("ashdi")},
                             {QStringLiteral("embedUrl"), QStringLiteral("http://127.0.0.1:9/%1").arg(n)}};
    const QJsonObject episode{{QStringLiteral("number"), n == 2 ? QJsonValue(2.5) : QJsonValue(n)},
                              {QStringLiteral("label"), QStringLiteral("Fixture episode")},
                              {QStringLiteral("kind"), n == 2 ? QStringLiteral("filler") : QStringLiteral("animeCanon")},
                              {QStringLiteral("sources"), QJsonArray{source}}};
    episodes.append(episode);
  }
  QJsonObject release{{QStringLiteral("id"), QStringLiteral("release-fixture")},
                      {QStringLiteral("kind"), QStringLiteral("voice")},
                      {QStringLiteral("teams"), teams}, {QStringLiteral("episodes"), episodes},
                      {QStringLiteral("episodesCount"), 3}};
  return release;
}

QByteArray pngBytes() {
  static const QByteArray bytes = QByteArray::fromHex(
      "89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d24000000"
      "164944415478da63507098f0df0388191432800c200600440c085de2c4e0b40000000049"
      "454e44ae426082");
  return bytes;
}

class ApiFixture final : public QObject {
 public:
  explicit ApiFixture(QObject* parent = nullptr) : QObject(parent) {
    connect(&server_, &QTcpServer::newConnection, this, [this] {
      while (QTcpSocket* socket = server_.nextPendingConnection()) {
        // Per-socket buffers: the title page asks for the detail and the player
        // at the same time, so one shared buffer would lose one of the two.
        connections_.insert(socket, Connection{});
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
          const auto it = connections_.find(socket);
          if (it == connections_.end()) return;
          Connection& connection = *it;
          connection.buffer += socket->readAll();
          const int end = connection.buffer.indexOf("\r\n\r\n");
          if (end < 0 || connection.served) return;
          connection.served = true;
          const QByteArray line = connection.buffer.left(connection.buffer.indexOf("\r\n"));
          const QList<QByteArray> parts = line.split(' ');
          const QString target = QString::fromLatin1(parts.value(1));
          const bool poster = target.endsWith(QStringLiteral("/poster.png")) ||
                              target.endsWith(QStringLiteral("/ua-poster.png"));
          const bool selectedPoster = target.endsWith(QStringLiteral("/ua-poster.png"));
          const QByteArray body = poster ? (selectedPoster ? selectedPosterBody : posterBody)
                                         : response(target);
          QByteArray head = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: ");
          head += poster ? QByteArrayLiteral("image/png")
                         : QByteArrayLiteral("application/json");
          head += QByteArrayLiteral("\r\nContent-Length: ");
          head += QByteArray::number(body.size());
          head += QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
          if (holdTeamResponses && target.startsWith(QStringLiteral("/public/v1/teams/")) &&
              !target.section(QLatin1Char('?'), 0, 0).endsWith(QStringLiteral("/anime"))) {
            heldTeamResponses_.append({socket, head + body});
            return;
          }
          socket->write(head + body);
          socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
          // The record dies with the socket: a later connection can reuse the
          // address of a gone one, and stale state there would read the new
          // request as one already served.
          connections_.remove(socket);
        });
        // The socket belongs to the fixture, so it outlives the handlers that
        // hold its address.
        socket->setParent(this);
      }
    });
  }
  ~ApiFixture() override {
    for (QTcpSocket* socket : findChildren<QTcpSocket*>(QString(), Qt::FindDirectChildrenOnly)) {
      socket->disconnect();
      socket->abort();
    }
  }
  bool start() { return server_.listen(QHostAddress::LocalHost, 0); }
  QString base() const { return QStringLiteral("http://127.0.0.1:%1/public/v1").arg(server_.serverPort()); }

  QStringList catalogueTitles() const {
    return {QStringLiteral("Probe"), QStringLiteral("Second Probe"), teamCatalogueTitle()};
  }
  QString teamName() const { return QStringLiteral("Fixture Team"); }
  QString teamDescription() const {
    return QStringLiteral("Fixture team description <not markup>\nSecond description line & credits");
  }
  QString teamCatalogueTitle() const { return QStringLiteral("Team Probe"); }
  bool licensed = false;
  QByteArray posterBody = pngBytes();
  QByteArray selectedPosterBody = pngBytes();
  bool holdTeamResponses = false;
  bool teamResponsePending() const { return !heldTeamResponses_.isEmpty(); }
  void releaseTeamResponses() {
    while (!heldTeamResponses_.isEmpty()) {
      const auto pending = heldTeamResponses_.takeLast();
      if (!pending.first) continue;
      pending.first->write(pending.second);
      pending.first->disconnectFromHost();
    }
  }

 private:
  struct Connection {
    QByteArray buffer;
    bool served = false;
  };

  QJsonObject teamProfile() const {
    return QJsonObject{{QStringLiteral("id"), 17},
                       {QStringLiteral("slug"), QStringLiteral("fixture-team")},
                       {QStringLiteral("name"), teamName()},
                       {QStringLiteral("description"), teamDescription()},
                       {QStringLiteral("animeCount"), 1},
                       {QStringLiteral("hasVoice"), true},
                       {QStringLiteral("links"), QJsonObject{
                            {QStringLiteral("telegram"), QStringLiteral("https://t.me/fixture_team")},
                            {QStringLiteral("patreon"), QStringLiteral("https://www.patreon.com/fixture")},
                            {QStringLiteral("monobase"), QStringLiteral("https://base.monobank.ua/EtUwD5aeeKXKWD")},
                            {QStringLiteral("youtube"), QStringLiteral("https://www.youtube.com/@%D0%A0%D1%96%D0%B4%D0%BB%D0%B9%D0%93%D0%BE%D0%BB%D0%BE%D1%81")}}}};
  }

  QJsonObject otherTeamProfile() const {
    return QJsonObject{{QStringLiteral("id"), 18},
                       {QStringLiteral("slug"), QStringLiteral("other-team")},
                       {QStringLiteral("name"), QStringLiteral("Other Team")},
                       {QStringLiteral("description"), QStringLiteral("Other team description")},
                       {QStringLiteral("animeCount"), 1},
                       {QStringLiteral("hasVoice"), false}};
  }

  QByteArray response(const QString& target) {
    const QString path = target.section(QLatin1Char('?'), 0, 0).mid(QStringLiteral("/public/v1").size());
    QJsonObject result;
    if (path.endsWith(QStringLiteral("/player"))) {
      result.insert(QStringLiteral("licensed"), licensed);
      result.insert(QStringLiteral("license"), licensed ? QJsonObject{{QStringLiteral("text"), QStringLiteral("Licensed for this region")},
                                                                       {QStringLiteral("url"), QStringLiteral("https://example.invalid/legal")}}
                                                        : QJsonObject{});
      if (!licensed) {
        result.insert(QStringLiteral("releases"), QJsonArray{releasePayload()});
      }
    } else if (path.startsWith(QStringLiteral("/anime/"))) {
      result = titlePayload(QStringLiteral("http://127.0.0.1:%1/poster.png").arg(server_.serverPort()),
                            QStringLiteral("http://127.0.0.1:%1/ua-poster.png").arg(server_.serverPort()));
      if (path == QLatin1String("/anime/7429")) {
        result.insert(QStringLiteral("ids"), QJsonObject{{QStringLiteral("mikai"), 7429}});
        result.insert(QStringLiteral("titles"), QJsonObject{
            {QStringLiteral("ua"), QStringLiteral("Related Probe")},
            {QStringLiteral("english"), QStringLiteral("Related Probe")}});
      } else if (path == QLatin1String("/anime/7431")) {
        // The only fixture title rated 18+, used by the explicit-content gate.
        result.insert(QStringLiteral("ageRating"), QStringLiteral("r18"));
      } else if (path == QLatin1String("/anime/7428")) {
        result.insert(QStringLiteral("relations"), QJsonArray{QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("sequel")},
            {QStringLiteral("anime"), QJsonObject{
                {QStringLiteral("ids"), QJsonObject{{QStringLiteral("mikai"), 7429}}},
                {QStringLiteral("titles"), QJsonObject{
                    {QStringLiteral("ua"), QStringLiteral("Related Probe")},
                    {QStringLiteral("english"), QStringLiteral("Related Probe")}}}}}}});
      }
    } else if (path.startsWith(QStringLiteral("/teams/")) && path.endsWith(QStringLiteral("/anime"))) {
      QJsonArray rows;
      rows.append(QJsonObject{
          {QStringLiteral("ids"), QJsonObject{{QStringLiteral("mikai"), 7428}}},
          {QStringLiteral("titles"), QJsonObject{{QStringLiteral("english"), teamCatalogueTitle()}}},
          {QStringLiteral("format"), QStringLiteral("tv")}});
      return QJsonDocument(QJsonObject{{QStringLiteral("ok"), true},
                                       {QStringLiteral("result"), rows},
                                       {QStringLiteral("total"), 1},
                                       {QStringLiteral("page"), 1},
                                       {QStringLiteral("pages"), 1}})
          .toJson(QJsonDocument::Compact);
    } else if (path.endsWith(QStringLiteral("/anime"))) {
      QJsonArray rows;
      const QUrlQuery query{QUrl(target)};
      const QString teamFilter = query.queryItemValue(QStringLiteral("teams"));
      const QStringList formats = query.queryItemValue(QStringLiteral("formats"))
                                      .split(QLatin1Char(','), Qt::SkipEmptyParts);
      for (const QString& title : catalogueTitles()) {
        const QString format = title == QStringLiteral("Second Probe")
            ? QStringLiteral("movie") : QStringLiteral("tv");
        if (!formats.isEmpty() && !formats.contains(format)) continue;
        if (!teamFilter.isEmpty()) {
          const bool belongsToTheTeam = teamFilter == QLatin1String("17") &&
                                        title == teamCatalogueTitle();
          if (!belongsToTheTeam) continue;
        }
        rows.append(QJsonObject{
            {QStringLiteral("ids"), QJsonObject{{QStringLiteral("mikai"),
                title == QStringLiteral("Probe") ? 7428 :
                    title == QStringLiteral("Second Probe") ? 7429 : 7430}}},
            {QStringLiteral("titles"), QJsonObject{{QStringLiteral("english"), title}}},
            {QStringLiteral("format"), format},
            {QStringLiteral("year"), 2024}});
      }
      return QJsonDocument(QJsonObject{{QStringLiteral("ok"), true},
                                       {QStringLiteral("result"), rows},
                                       {QStringLiteral("total"), rows.size()},
                                       {QStringLiteral("page"), 1},
                                       {QStringLiteral("pages"), 1}})
          .toJson(QJsonDocument::Compact);
    } else if (path == QStringLiteral("/teams")) {
      const QUrlQuery query{QUrl(target)};
      const QString term = query.queryItemValue(QStringLiteral("search"));
      QJsonArray teams{teamProfile(), otherTeamProfile()};
      if (!term.isEmpty()) {
        teams = QJsonArray();
        for (const QJsonValue& value : QJsonArray{teamProfile(), otherTeamProfile()}) {
          const QJsonObject team = value.toObject();
          if (team.value(QStringLiteral("name")).toString().contains(term, Qt::CaseInsensitive)) {
            teams.append(value);
          }
        }
      }
      return QJsonDocument(QJsonObject{{QStringLiteral("ok"), true},
                                       {QStringLiteral("result"), teams},
                                       {QStringLiteral("total"), teams.size()},
                                       {QStringLiteral("page"), 1},
                                       {QStringLiteral("pages"), 1}})
          .toJson(QJsonDocument::Compact);
    } else if (path.startsWith(QStringLiteral("/teams/"))) {
      result = teamProfile();
    } else if (path == QStringLiteral("/meta")) {
      result = QJsonObject{{QStringLiteral("version"), QStringLiteral("fixture")},
                           {QStringLiteral("formats"), QJsonArray{QStringLiteral("tv"), QStringLiteral("movie")}},
                           {QStringLiteral("statuses"), QJsonArray{QStringLiteral("ongoing")}},
                           {QStringLiteral("seasons"), QJsonArray{QStringLiteral("spring")}},
                           {QStringLiteral("animeSortFields"), QJsonArray{QStringLiteral("name")}},
                           {QStringLiteral("sortOrders"), QJsonArray{QStringLiteral("asc")}},
                           {QStringLiteral("matchModes"), QJsonArray{QStringLiteral("all"), QStringLiteral("any")}}};
    } else if (path == QStringLiteral("/genres")) {
      return QJsonDocument(QJsonObject{{QStringLiteral("ok"), true},
                                       {QStringLiteral("result"), QJsonArray{
                                            QJsonObject{{QStringLiteral("name"), QStringLiteral("Action")},
                                                        {QStringLiteral("ua"), QStringLiteral("Бойовик")}},
                                            QJsonObject{{QStringLiteral("name"), QStringLiteral("Drama")},
                                                        {QStringLiteral("ua"), QStringLiteral("Драма")}}}}})
          .toJson(QJsonDocument::Compact);
    } else if (path == QStringLiteral("/tags")) {
      return QJsonDocument(QJsonObject{{QStringLiteral("ok"), true},
                                       {QStringLiteral("result"), QJsonArray{
                                            QJsonObject{{QStringLiteral("name"), QStringLiteral("school")},
                                                        {QStringLiteral("ua"), QStringLiteral("Школа")}}}}})
          .toJson(QJsonDocument::Compact);
    } else if (path == QStringLiteral("/studios")) {
      return QJsonDocument(QJsonObject{{QStringLiteral("ok"), true},
                                       {QStringLiteral("result"), QJsonArray{QStringLiteral("Fixture Studio")}}})
          .toJson(QJsonDocument::Compact);
    } else if (path == QStringLiteral("/updates")) {
      QJsonArray rows;
      rows.append(QJsonObject{
          {QStringLiteral("anime"), QJsonObject{
              {QStringLiteral("ids"), QJsonObject{{QStringLiteral("mikai"), 7428}}},
              {QStringLiteral("titles"), QJsonObject{{QStringLiteral("english"), QStringLiteral("Probe")}}}}},
          {QStringLiteral("provider"), QStringLiteral("ashdi")},
          {QStringLiteral("embedUrl"), QStringLiteral("http://127.0.0.1:9/embed")},
          {QStringLiteral("publishedAt"), QStringLiteral("2026-10-20T12:00:00Z")},
          {QStringLiteral("episode"), 3},
          {QStringLiteral("teamName"), QStringLiteral("Fixture Team")},
          {QStringLiteral("kind"), QStringLiteral("voice")}});
      return QJsonDocument(QJsonObject{{QStringLiteral("ok"), true},
                                       {QStringLiteral("result"), rows},
                                       {QStringLiteral("total"), 1},
                                       {QStringLiteral("page"), 1},
                                       {QStringLiteral("pages"), 1}})
          .toJson(QJsonDocument::Compact);
    } else if (path == QStringLiteral("/schedule")) {
      result = QJsonObject{};
    } else {
      result = QJsonObject{};
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("result"), result}}).toJson(QJsonDocument::Compact);
  }
  QTcpServer server_;
  QHash<QTcpSocket*, Connection> connections_;
  QList<QPair<QPointer<QTcpSocket>, QByteArray>> heldTeamResponses_;
};

struct IsolatedSettings {
  QTemporaryDir dir;
  IsolatedSettings() {
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, dir.path());
    // Without the override the client would find the repository's own .env and
    // the user's real key; this suite must only ever see its fake one.
    const QString envPath = QDir(dir.path()).filePath(QStringLiteral(".env"));
    QFile env(envPath);
    if (env.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
      env.write(QStringLiteral("X-API-Key: %1\n").arg(kFixtureKey).toUtf8());
      env.close();
    }
    mikai::backend::setEnvFilePathOverride(envPath);
  }
  ~IsolatedSettings() {
    mikai::app::WatchHistory::flushHistoryWrites();
    mikai::backend::setEnvFilePathOverride(QString());
  }
};

QString labelText(const QWidget& root, const char* name) {
  const auto* label = root.findChild<QLabel*>(QString::fromLatin1(name));
  return label ? label->text() : QString();
}

QPushButton* button(const QWidget& root, const char* name) {
  return root.findChild<QPushButton*>(QString::fromLatin1(name));
}

mikai::app::TitlePage* visibleTitle(const QWidget& root) {
  for (auto* page : root.findChildren<mikai::app::TitlePage*>()) {
    if (page->isVisible()) return page;
  }
  return nullptr;
}

QPushButton* relatedTitleButton(const QWidget& page) {
  for (auto* card : page.findChildren<QPushButton*>(QStringLiteral("animeCard"))) {
    if (card->toolTip() == QStringLiteral("Related Probe")) return card;
  }
  return nullptr;
}

bool catalogueShows(const QWidget& page, const QString& text) {
  auto* results = page.findChild<QListWidget*>(QStringLiteral("searchResults"));
  if (results == nullptr) return false;
  for (int row = 0; row < results->count(); ++row) {
    if (results->item(row)->text().contains(text)) return true;
  }
  return false;
}

// One modal session on the team picker. The modal dialog blocks the caller, so
// the session runs from a timer while it is open.
void runPickerSession(QWidget& page, const std::function<void(QDialog*, bool&)>& step) {
  QTimer driver;
  driver.setInterval(20);
  QObject::connect(&driver, &QTimer::timeout, &page, [&driver, &step] {
    auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (modal == nullptr) return;
    bool finished = false;
    step(modal, finished);
    if (finished) driver.stop();
  });
  driver.start();
  QTest::mouseClick(button(page, "catalogueTeamsButton"), Qt::LeftButton);
  driver.stop();
}


}  // namespace

void TestPublicApiUi::titleMetadataPreservesUnknownCountsBadgesAndFutureAiring() {
  IsolatedSettings settings;
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  mikai::app::QualityCache cache;
  mikai::app::TitlePage page(runner, client, cache, QStringLiteral("7428"));
  page.resize(1120, 800);
  page.show();

  QTRY_VERIFY_WITH_TIMEOUT(!labelText(page, "episodeAvailabilityLabel").isEmpty(), 20000);
  const QString availability = labelText(page, "episodeAvailabilityLabel");
  QStringList counts;
  auto numbers = QRegularExpression(QStringLiteral("\\d+")).globalMatch(availability);
  while (numbers.hasNext()) counts.append(numbers.next().captured());
  QCOMPARE(counts, QStringList({QStringLiteral("2"), QStringLiteral("0")}));

  QTRY_VERIFY_WITH_TIMEOUT(!labelText(page, "nextAiringLabel").isEmpty(), 20000);
  const QString airing = labelText(page, "nextAiringLabel");
  QVERIFY2(airing.contains(QStringLiteral("3")),
           qPrintable(QStringLiteral("the upcoming episode number is not on the label: %1").arg(airing)));

  QTRY_VERIFY_WITH_TIMEOUT(
      !page.findChildren<QPushButton*>(QStringLiteral("episodeButton")).isEmpty(), 20000);
  const auto buttons = page.findChildren<QPushButton*>(QStringLiteral("episodeButton"));
  bool sawFiller = false;
  bool sawBadgeAndMarkTogether = false;
  for (const QPushButton* pill : buttons) {
    const auto* badge = pill->findChild<QLabel*>(QStringLiteral("episodeTypeBadge"));
    const auto* mark = pill->findChild<QWidget*>(QStringLiteral("episodeWatchedMark"));
    if (badge == nullptr) continue;
    if (badge->text() == QStringLiteral("F")) sawFiller = true;
    if (mark != nullptr) sawBadgeAndMarkTogether = true;
  }
  QVERIFY2(sawFiller, "no episode carries the filler badge");
  QVERIFY2(sawBadgeAndMarkTogether,
           "no episode pill carries both its classification and its watch mark");

  QTRY_VERIFY_WITH_TIMEOUT(
      !page.findChildren<QPushButton*>(QStringLiteral("releaseChip")).isEmpty(), 20000);
  QPushButton* chip = nullptr;
  for (QPushButton* candidate : page.findChildren<QPushButton*>(QStringLiteral("releaseChip"))) {
    if (candidate->toolTip().contains(QStringLiteral("Fixture Team"))) {
      chip = candidate;
      break;
    }
  }
  QVERIFY2(chip != nullptr, "no release chip names the fixture team");
  QVERIFY2(page.findChildren<QPushButton*>(QStringLiteral("releaseTeamLink")).isEmpty(),
           "the release row still carries an inline team link");
  QVERIFY2(chip->toolTip().contains(QStringLiteral("Fixture Team")),
           qPrintable(QStringLiteral("the chip tooltip does not name the team: %1")
                          .arg(chip->toolTip())));
  const QString englishToolTip = chip->toolTip();

  QCOMPARE(chip->contextMenuPolicy(), Qt::CustomContextMenu);

  QVERIFY2(primeBuiltCatalogForSwitching(), "the Ukrainian catalog is not available");
  QSignalSpy teamRequests(&page, &mikai::app::TitlePage::teamRequested);
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  // A failing assertion below must not leave Ukrainian active for the next case.
  const auto restoreEnglish = qScopeGuard(
      [] { mikai::app::applyInterfaceLanguage(QStringLiteral("en")); });
  QTRY_VERIFY_WITH_TIMEOUT(chip->toolTip() != englishToolTip, 10000);
  QVERIFY2(chip->toolTip() != englishToolTip,
           qPrintable(QStringLiteral("the chip tooltip kept its English wording: %1")
                          .arg(chip->toolTip())));

  QString menuText;
  QTimer trigger;
  trigger.setInterval(20);
  QObject::connect(&trigger, &QTimer::timeout, &page, [&] {
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    if (menu == nullptr) return;
    if (!menu->actions().isEmpty()) {
      menuText = menu->actions().first()->text();
      menu->actions().first()->trigger();
    }
    menu->close();
  });
  trigger.start();
  // The menu blocks in exec(), so the press is delivered through the chip's own
  // signal; the policy above proves a right-click reaches it.
  QMetaObject::invokeMethod(chip, "customContextMenuRequested", Qt::DirectConnection,
                            Q_ARG(QPoint, chip->rect().center()));
  trigger.stop();
  QVERIFY2(menuText.contains(QStringLiteral("Fixture Team")),
           qPrintable(QStringLiteral("the chip menu does not name the team: %1").arg(menuText)));
  QVERIFY2(menuText != englishToolTip,
           qPrintable(QStringLiteral("the chip menu stayed in English: %1").arg(menuText)));
  QTRY_VERIFY_WITH_TIMEOUT(teamRequests.count() == 1, 5000);
  QCOMPARE(teamRequests.first().first().toString(), QStringLiteral("fixture-team"));
}

void TestPublicApiUi::aWatchedFractionalEpisodeKeepsItsClassificationBadge() {
  IsolatedSettings settings;
  mikai::app::WatchHistory history;
  history.setCurrent(QStringLiteral("7428"), QStringLiteral("2.5"));
  history.setWatched(QStringLiteral("7428"), QStringLiteral("2.5"), true);
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  mikai::app::QualityCache cache;
  mikai::app::TitlePage page(runner, client, cache, QStringLiteral("7428"));
  page.resize(1120, 800);
  page.show();

  QTRY_VERIFY_WITH_TIMEOUT(
      !page.findChildren<QPushButton*>(QStringLiteral("episodeButton")).isEmpty(), 20000);
  const auto buttons = page.findChildren<QPushButton*>(QStringLiteral("episodeButton"));
  QPushButton* fillerPill = nullptr;
  for (QPushButton* pill : buttons) {
    auto* badge = pill->findChild<QLabel*>(QStringLiteral("episodeTypeBadge"));
    if (badge != nullptr && badge->text() == QStringLiteral("F")) {
      fillerPill = pill;
      break;
    }
  }
  QVERIFY2(fillerPill != nullptr, "no episode pill carries the filler badge");

  QCOMPARE(fillerPill->text(), QStringLiteral("2.5"));
  QTRY_VERIFY_WITH_TIMEOUT(fillerPill->findChild<QWidget*>(QStringLiteral("episodeWatchedMark")) != nullptr &&
                                fillerPill->findChild<QWidget*>(QStringLiteral("episodeWatchedMark"))->isVisible(),
                            5000);
  auto* badge = fillerPill->findChild<QLabel*>(QStringLiteral("episodeTypeBadge"));
  QVERIFY2(badge != nullptr, "the classification badge was taken by the watch mark");
  QCOMPARE(badge->text(), QStringLiteral("F"));
}

void TestPublicApiUi::licensedTitleShowsLegalInformationAndSelectedPosterCredit() {
  IsolatedSettings settings;
  ApiFixture api;
  api.licensed = true;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  mikai::app::QualityCache cache;
  mikai::app::TitlePage page(runner, client, cache, QStringLiteral("7428"));
  page.resize(1120, 800);
  page.show();

  QTRY_VERIFY_WITH_TIMEOUT(labelText(page, "licenseInfoLabel").contains(QStringLiteral("Licensed")),
                            20000);
  const QString license = labelText(page, "licenseInfoLabel");
  QVERIFY2(license.contains(QStringLiteral("example.invalid")),
           qPrintable(QStringLiteral("the legal link is not on the page: %1").arg(license)));

  QTRY_VERIFY_WITH_TIMEOUT(
      labelText(page, "licenseInfoLabel").contains(QStringLiteral("Playback unavailable due to licensing")),
      20000);
  const QString licenseText = labelText(page, "licenseInfoLabel");
  QVERIFY2(!licenseText.contains(QStringLiteral("No releases"), Qt::CaseInsensitive),
           qPrintable(QStringLiteral("a licensed title is reported as having no releases: %1")
                          .arg(licenseText)));

  QTRY_VERIFY_WITH_TIMEOUT(labelText(page, "posterCreditLabel").contains(QStringLiteral("Poster Artist")),
                            20000);
}

void TestPublicApiUi::anUnlicensedTitleShowsNoLegalInformation() {
  IsolatedSettings settings;
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  mikai::app::QualityCache cache;
  mikai::app::TitlePage page(runner, client, cache, QStringLiteral("7428"));
  page.resize(1120, 800);
  page.show();

  QTRY_VERIFY_WITH_TIMEOUT(!labelText(page, "episodeAvailabilityLabel").isEmpty(), 20000);
  QVERIFY2(labelText(page, "licenseInfoLabel").isEmpty(),
           qPrintable(QStringLiteral("an unlicensed title claims a licence: %1")
                          .arg(labelText(page, "licenseInfoLabel"))));
}

void TestPublicApiUi::explicitTitleAsksBeforePlaybackUnlessAllowed() {
  IsolatedSettings settings;
  ApiFixture api;
  QVERIFY(api.start());
  const mikai::app::theme::Appearance saved = mikai::app::currentAppearance();
  const auto restore = qScopeGuard([saved] { mikai::app::setCurrentAppearance(saved); });
  mikai::app::theme::Appearance appearance = saved;
  appearance.showAdult = false;
  mikai::app::setCurrentAppearance(appearance);

  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  mikai::app::QualityCache cache;
  mikai::app::TitlePage page(runner, client, cache, QStringLiteral("7431"));
  page.resize(1120, 800);
  page.show();

  QPushButton* episode = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((episode = page.findChild<QPushButton*>(
                                QStringLiteral("episodeButton"))) != nullptr,
                           20000);

  QStringList warnings;
  QTimer driver;
  driver.setInterval(20);
  QObject::connect(&driver, &QTimer::timeout, [&warnings] {
    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
    if (box == nullptr) return;
    warnings.append(box->text());
    box->reject();
  });
  driver.start();
  QTest::mouseClick(episode, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(!warnings.isEmpty(), 5000);
  QVERIFY2(warnings.first().contains(QStringLiteral("18+")), qPrintable(warnings.first()));

  // The preference means "without a warning": the second attempt must not ask.
  appearance.showAdult = true;
  mikai::app::setCurrentAppearance(appearance);
  QTest::mouseClick(episode, Qt::LeftButton);
  QTest::qWait(600);
  QCOMPARE(warnings.size(), 1);
  driver.stop();
}

void TestPublicApiUi::liveCoverShadingChangesWithoutResizing() {
  IsolatedSettings settings;
  const auto saved = mikai::app::currentAppearance();
  const auto restore = qScopeGuard([saved] { mikai::app::setCurrentAppearance(saved); });
  auto appearance = saved;
  appearance.coverGradient = false;
  mikai::app::setCurrentAppearance(appearance);
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  mikai::app::QualityCache cache;
  mikai::app::TitlePage page(runner, client, cache, QStringLiteral("7428"));
  page.resize(1000, 740);
  page.show();
  auto* cover = page.findChild<QLabel*>(QStringLiteral("titleCover"));
  QVERIFY(cover != nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(!cover->pixmap().isNull(), 10000);
  const QImage original = cover->pixmap().toImage();
  const QPoint sample(original.width() / 2, original.height() * 9 / 10);
  const QSize size = cover->size();

  mikai::app::SettingsDialog dialog(client, runner, &page);
  dialog.show();
  auto* shade = dialog.findChild<QAbstractButton*>(QStringLiteral("coverGradientSwitch"));
  QVERIFY(shade != nullptr);
  shade->click();
  QCOMPARE(cover->size(), size);
  const QImage shaded = cover->pixmap().toImage();
  QVERIFY(shaded.pixelColor(sample).value() + 20 < original.pixelColor(sample).value());
  dialog.reject();
  QCOMPARE(cover->pixmap().toImage().pixelColor(sample), original.pixelColor(sample));
}

void TestPublicApiUi::invalidSelectedPosterFallsBackToHint() {
  IsolatedSettings settings;
  const auto saved = mikai::app::currentAppearance();
  const auto restore = qScopeGuard([saved] { mikai::app::setCurrentAppearance(saved); });
  auto appearance = saved;
  appearance.coverGradient = false;
  mikai::app::setCurrentAppearance(appearance);
  ApiFixture api;
  api.selectedPosterBody = QByteArrayLiteral("not an image");
  QImage poster(48, 64, QImage::Format_RGB32);
  const QColor expected(QStringLiteral("#e02040"));
  poster.fill(expected);
  QBuffer encoded(&api.posterBody);
  QVERIFY(encoded.open(QIODevice::WriteOnly));
  QVERIFY(poster.save(&encoded, "PNG"));
  encoded.close();
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  mikai::app::QualityCache cache;
  const QString hint = QUrl(api.base()).resolved(QUrl(QStringLiteral("/poster.png"))).toString();
  mikai::app::TitlePage page(runner, client, cache, QStringLiteral("7428"), hint);
  page.resize(1120, 800);
  page.show();
  auto* cover = page.findChild<QLabel*>(QStringLiteral("titleCover"));
  QVERIFY(cover != nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(!cover->pixmap().isNull(), 5000);
  const QImage shown = cover->pixmap().toImage();
  QCOMPARE(shown.pixelColor(shown.width() / 2, shown.height() / 2), expected);
  auto* credit = page.findChild<QLabel*>(QStringLiteral("posterCreditLabel"));
  QVERIFY(credit != nullptr);
  QVERIFY(!credit->isVisible());
}

void TestPublicApiUi::decodedPosterScalesAfterExifRotation() {
  QImage source(160, 80, QImage::Format_RGB32);
  source.fill(Qt::red);
  {
    QPainter painter(&source);
    painter.fillRect(QRect(80, 0, 80, 80), Qt::blue);
  }
  QByteArray bytes;
  QBuffer encoded(&bytes);
  QVERIFY(encoded.open(QIODevice::WriteOnly));
  QVERIFY(source.save(&encoded, "JPEG"));
  encoded.close();
  bytes.insert(2, QByteArray::fromHex(
                      "ffe100224578696600004d4d002a00000008000101120003000000010006000000000000"));

  const auto decoded = mikai::gui::decodeImage(bytes, QSize(40, 80));
  QVERIFY2(!decoded.image.isNull(), qPrintable(decoded.error));
  QCOMPARE(decoded.image.size(), QSize(40, 80));
  const QColor top = decoded.image.pixelColor(20, 10);
  const QColor bottom = decoded.image.pixelColor(20, 70);
  QVERIFY(top.red() > top.blue() + 100);
  QVERIFY(bottom.blue() > bottom.red() + 100);
}

void TestPublicApiUi::titleSectionHeadersLetTheGradientThrough() {
  IsolatedSettings settings;
  const auto saved = mikai::app::currentAppearance();
  const auto restore = qScopeGuard([saved] { mikai::app::setCurrentAppearance(saved); });
  auto appearance = saved;
  appearance.backgroundGradient = false;
  mikai::app::setCurrentAppearance(appearance);

  QWidget page;
  page.setObjectName(QStringLiteral("titlePage"));
  page.setAttribute(Qt::WA_StyledBackground, true);
  page.setStyleSheet(mikai::app::theme::buildStylesheet(appearance));
  auto* pageLayout = new QVBoxLayout(&page);
  pageLayout->setContentsMargins(0, 0, 0, 0);
  pageLayout->setSpacing(0);

  const auto addHeader = [&page, pageLayout](const QString& objectName,
                                             const QString& headingObjectName,
                                             const QString& text) {
    auto* header = new QWidget(&page);
    header->setObjectName(objectName);
    header->setAttribute(Qt::WA_StyledBackground, true);
    header->setFixedHeight(72);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 0, 16, 0);
    auto* heading = new QLabel(text, header);
    heading->setObjectName(headingObjectName);
    headerLayout->addWidget(heading);
    pageLayout->addWidget(header);
    return header;
  };
  QWidget* sectionHeader = addHeader(QStringLiteral("titleSectionHeader"),
                                     QStringLiteral("titleSectionHeading"),
                                     QStringLiteral("Related titles"));
  QWidget* episodeHeader = addHeader(QStringLiteral("titleEpisodeHeader"),
                                     QStringLiteral("episodeSectionHeading"),
                                     QStringLiteral("Episodes"));
  auto* glow = new mikai::app::PageGlow(&page);
  Q_UNUSED(glow);

  page.resize(500, 160);
  page.show();
  QCoreApplication::processEvents();

  const auto headerPixel = [&page](QWidget* header) {
    const QPoint local(header->width() / 2, header->height() / 2);
    return page.grab().toImage().pixelColor(header->mapTo(&page, local));
  };
  const QColor sectionWithoutGradient = headerPixel(sectionHeader);
  const QColor episodeWithoutGradient = headerPixel(episodeHeader);

  appearance.backgroundGradient = true;
  appearance.gradientColor = QStringLiteral("#ff3058");
  appearance.gradientIntensity = 100;
  appearance.gradientHeight = 100;
  mikai::app::setCurrentAppearance(appearance);
  QCoreApplication::processEvents();

  const QColor sectionWithGradient = headerPixel(sectionHeader);
  QVERIFY2(sectionWithGradient != sectionWithoutGradient,
           qPrintable(QStringLiteral("section header stayed %1")
                          .arg(sectionWithoutGradient.name())));
  const QColor episodeWithGradient = headerPixel(episodeHeader);
  QVERIFY2(episodeWithGradient != episodeWithoutGradient,
           qPrintable(QStringLiteral("episode header stayed %1")
                          .arg(episodeWithoutGradient.name())));
}
void TestPublicApiUi::gradientPreviewKeepsItsLatestColorAndCancelRestoresIt() {
  IsolatedSettings settings;
  const auto saved = mikai::app::currentAppearance();
  const auto restore = qScopeGuard([saved] { mikai::app::setCurrentAppearance(saved); });
  auto opening = saved;
  opening.backgroundGradient = true;
  opening.gradientColor = QStringLiteral("#ef6b78");
  opening.gradientIntensity = 30;
  opening.gradientHeight = 80;
  mikai::app::setCurrentAppearance(opening);
  QWidget host;
  host.resize(400, 300);
  host.setAutoFillBackground(true);
  QPalette palette = host.palette();
  palette.setColor(QPalette::Window, QColor(QStringLiteral("#181818")));
  host.setPalette(palette);
  auto* glow = new mikai::app::PageGlow(&host);
  host.show();
  QCoreApplication::processEvents();
  const QPoint sample(200, 5);
  const QColor original = host.grab().toImage().pixelColor(sample);
  const QRect originalGeometry = glow->geometry();

  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  mikai::app::SettingsDialog dialog(client, runner);
  dialog.show();
  QVERIFY(dialog.showPage(QStringLiteral("customization/appearance")));
  const auto sliders = dialog.findChildren<QSlider*>(QStringLiteral("appearanceSlider"));
  const auto colors = dialog.findChildren<QPushButton*>(QStringLiteral("gradientSwatch"));
  QVERIFY(sliders.size() == 2 && colors.size() > 2);
  for (int intensity : {40, 60, 80}) sliders.at(0)->setValue(intensity);
  colors.at(2)->click();
  QCOMPARE(glow->geometry(), originalGeometry);
  const QColor green = host.grab().toImage().pixelColor(sample);
  QVERIFY(green.green() > green.red());
  QVERIFY(green.green() > original.green());
  sliders.at(1)->setValue(60);
  QCOMPARE(glow->height(), 180);
  dialog.reject();
  QCOMPARE(glow->geometry(), originalGeometry);
  QCOMPARE(host.grab().toImage().pixelColor(sample), original);
}

void TestPublicApiUi::catalogueFiltersRetainCanonicalSelectionsAndReset() {
  qApp->setStyle(QStringLiteral("Fusion"));
  qApp->setStyleSheet(mikai::app::theme::buildStylesheet());
  IsolatedSettings settings;
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  mikai::app::SearchPage page(runner, client);
  page.resize(1120, 800);
  page.show();

  auto* heading = page.findChild<QLabel*>(QStringLiteral("sectionHeading"));
  QVERIFY(heading != nullptr);
  const QString initialHeading = heading->text();
  QTRY_VERIFY_WITH_TIMEOUT(heading->text() != initialHeading, 10000);

  QTRY_VERIFY_WITH_TIMEOUT(button(page, "catalogueFiltersButton") != nullptr, 10000);
  QTRY_VERIFY_WITH_TIMEOUT(button(page, "catalogueFiltersButton")->isEnabled(), 20000);

  // The filter dialog is modal, so it is driven from a timer.
  QTimer driver;
  driver.setInterval(20);
  QStringList genreNames;
  bool sawApply = false;
  bool compactChoices = false;
  bool sawReset = false;
  connect(&driver, &QTimer::timeout, &page, [&genreNames, &sawApply, &compactChoices] {
    auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (modal == nullptr) return;
    auto* genreList = modal->findChild<QListWidget*>(QStringLiteral("genreIncludeList"));
    if (genreList != nullptr) {
      for (int row = 0; row < genreList->count(); ++row) {
        genreNames << genreList->item(row)->data(Qt::UserRole).toString();
      }
      if (genreList->count() > 0) compactChoices = genreList->sizeHintForRow(0) <= 36;
    }
    auto* formatList = modal->findChild<QListWidget*>(QStringLiteral("formatFilterList"));
    if (formatList != nullptr && formatList->count() > 0) {
      formatList->item(0)->setCheckState(Qt::Checked);
    }
    auto* apply = modal->findChild<QPushButton*>(QStringLiteral("catalogueApplyButton"));
    if (apply != nullptr) {
      sawApply = true;
      QTest::mouseClick(apply, Qt::LeftButton);
    }
  });
  driver.start();

  QTest::mouseClick(button(page, "catalogueFiltersButton"), Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(sawApply, 10000);
  QTRY_VERIFY_WITH_TIMEOUT(QApplication::activeModalWidget() == nullptr, 10000);
  driver.stop();

  QVERIFY2(compactChoices, "catalogue choices still use the tall search-result row");
  QVERIFY2(!genreNames.isEmpty(), "the filter offered no genre at all");
  QCOMPARE(genreNames.first(), QStringLiteral("Action"));

  auto* results = page.findChild<QListWidget*>(QStringLiteral("searchResults"));
  QTRY_VERIFY_WITH_TIMEOUT(results != nullptr, 10000);
  QTRY_VERIFY_WITH_TIMEOUT(!results->isHidden(), 20000);
  QTRY_VERIFY_WITH_TIMEOUT(catalogueShows(page, QStringLiteral("Probe")), 20000);
  QVERIFY(!catalogueShows(page, QStringLiteral("Second Probe")));

  QStringList reopenedFormats;
  QStringList reopenedFormatCaptions;
  QTimer reopenDriver;
  reopenDriver.setInterval(20);
  connect(&reopenDriver, &QTimer::timeout, &page, [&reopenedFormats,
                                                    &reopenedFormatCaptions, &sawReset] {
    auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (modal == nullptr) return;
    auto* formatList = modal->findChild<QListWidget*>(QStringLiteral("formatFilterList"));
    if (formatList == nullptr) return;
    for (int row = 0; row < formatList->count(); ++row) {
      if (formatList->item(row)->checkState() == Qt::Checked) {
        reopenedFormats << formatList->item(row)->data(Qt::UserRole).toString();
        reopenedFormatCaptions << formatList->item(row)->text();
      }
    }
    auto* reset = modal->findChild<QPushButton*>(QStringLiteral("catalogueResetButton"));
    if (reset != nullptr) {
      sawReset = true;
      QTest::mouseClick(reset, Qt::LeftButton);
      return;
    }
    modal->reject();
  });
  reopenDriver.start();
  QTest::mouseClick(button(page, "catalogueFiltersButton"), Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(!reopenedFormats.isEmpty(), 10000);
  reopenDriver.stop();
  QTRY_VERIFY_WITH_TIMEOUT(QApplication::activeModalWidget() == nullptr, 10000);
  QVERIFY2(sawReset, "the filter dialog offers no way back to the unfiltered catalogue");
  QTRY_VERIFY_WITH_TIMEOUT(catalogueShows(page, QStringLiteral("Second Probe")), 20000);

  QVERIFY2(!reopenedFormatCaptions.isEmpty(), "the reopened filter shows no format at all");
  QVERIFY2(reopenedFormatCaptions.first() != QStringLiteral("tv"),
           qPrintable(QStringLiteral("the filter offers the raw API token: %1")
                          .arg(reopenedFormatCaptions.first())));
  QVERIFY2(reopenedFormatCaptions.first().contains(QStringLiteral("TV"), Qt::CaseInsensitive),
           qPrintable(QStringLiteral("the format caption is not the app's wording: %1")
                          .arg(reopenedFormatCaptions.first())));
}

void TestPublicApiUi::catalogueFilterSearchNarrowsChoices() {
  qApp->setStyle(QStringLiteral("Fusion"));
  qApp->setStyleSheet(mikai::app::theme::buildStylesheet());
  IsolatedSettings settings;
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Filter search workers did not stop");
  });
  mikai::app::SearchPage page(runner, client);
  page.resize(1120, 800);
  page.show();

  auto* heading = page.findChild<QLabel*>(QStringLiteral("sectionHeading"));
  QVERIFY(heading != nullptr);
  const QString initialHeading = heading->text();
  QTRY_VERIFY_WITH_TIMEOUT(heading->text() != initialHeading, 10000);
  QTRY_VERIFY_WITH_TIMEOUT(button(page, "catalogueFiltersButton") != nullptr, 10000);
  QTRY_VERIFY_WITH_TIMEOUT(button(page, "catalogueFiltersButton")->isEnabled(), 20000);

  int total = 0;
  int fullHeight = 0;
  bool narrowed = false;
  bool shrank = false;
  bool keptHiddenCheck = false;
  bool canonicalMatch = false;
  bool restored = false;
  int stage = 0;
  QTimer driver;
  driver.setInterval(20);
  connect(&driver, &QTimer::timeout, &page, [&] {
    auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (modal == nullptr) return;
    auto* search = modal->findChild<QLineEdit*>(QStringLiteral("genreFilterSearch"));
    auto* list = modal->findChild<QListWidget*>(QStringLiteral("genreIncludeList"));
    if (search == nullptr || list == nullptr || list->count() == 0) return;
    const auto visible = [list] {
      int shown = 0;
      for (int row = 0; row < list->count(); ++row) {
        if (!list->item(row)->isHidden()) ++shown;
      }
      return shown;
    };
    switch (stage) {
      case 0:
        total = list->count();
        fullHeight = list->height();
        list->item(0)->setCheckState(Qt::Checked);
        search->setText(QStringLiteral("zzz"));
        stage = 1;
        return;
      case 1:
        narrowed = visible() == 0;
        shrank = list->height() < fullHeight;
        keptHiddenCheck = list->item(0)->checkState() == Qt::Checked;
        search->setText(QStringLiteral("act"));
        stage = 2;
        return;
      case 2:
        // The fixture shows "Бойовик" but the API value is "Action": matching the
        // canonical value is what makes an English query find a localized row.
        canonicalMatch = visible() == 1 && !list->item(0)->isHidden();
        search->clear();
        stage = 3;
        return;
      default:
        restored = visible() == total;
        driver.stop();
        modal->reject();
        return;
    }
  });
  driver.start();
  QTest::mouseClick(button(page, "catalogueFiltersButton"), Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(stage == 3 && QApplication::activeModalWidget() == nullptr, 15000);
  driver.stop();

  QVERIFY2(narrowed, "searching the genre list left every genre on show");
  QVERIFY2(shrank, "the narrowed genre list kept its full height");
  QVERIFY2(keptHiddenCheck, "hiding a row cleared the genre it had checked");
  QVERIFY2(canonicalMatch, "a genre is not findable by its API value");
  QVERIFY2(restored, "clearing the search did not bring the genres back");
}

void TestPublicApiUi::teamBrowsingAndFilteringKeepSelectionsConsistent() {
  qApp->setStyle(QStringLiteral("Fusion"));
  qApp->setStyleSheet(mikai::app::theme::buildStylesheet());
  IsolatedSettings settings;
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  mikai::app::SearchPage page(runner, client);
  page.resize(1120, 800);
  page.show();

  auto* heading = page.findChild<QLabel*>(QStringLiteral("sectionHeading"));
  QVERIFY(heading != nullptr);
  const QString initialHeading = heading->text();
  QTRY_VERIFY_WITH_TIMEOUT(heading->text() != initialHeading, 10000);

  page.browseTeam(QStringLiteral("fixture-team"));
  QTRY_VERIFY_WITH_TIMEOUT(page.findChild<QLabel*>(QStringLiteral("teamDetailLabel")) != nullptr, 10000);
  auto* detail = page.findChild<QLabel*>(QStringLiteral("teamDetailLabel"));
  QTRY_VERIFY_WITH_TIMEOUT(detail->text().contains(api.teamName()), 20000);
  QTextDocument document;
  document.setHtml(detail->text());
  QVERIFY(document.toPlainText().contains(api.teamDescription()));

  QTRY_VERIFY_WITH_TIMEOUT(page.findChild<QListWidget*>(QStringLiteral("searchResults")) != nullptr, 10000);
  auto* results = page.findChild<QListWidget*>(QStringLiteral("searchResults"));
  QTRY_VERIFY_WITH_TIMEOUT(!results->isHidden(), 20000);
  QTRY_VERIFY_WITH_TIMEOUT(results->count() > 0, 20000);
  for (int row = 0; row < results->count(); ++row) {
    QVERIFY2(results->item(row)->text().contains(QStringLiteral("Team Probe")),
             qPrintable(QStringLiteral("a row outside the team's catalogue is shown: %1")
                            .arg(results->item(row)->text())));
  }

  QTRY_VERIFY_WITH_TIMEOUT(button(page, "catalogueTeamsButton") != nullptr, 10000);
  bool compactTeams = false;
  bool sawBrowseEnabled = false;
  bool sawBrowseEnabledWithoutARow = false;
  QString narrowedTeam;
  runPickerSession(page, [&sawBrowseEnabled, &sawBrowseEnabledWithoutARow,
                          &narrowedTeam, &compactTeams](QDialog* modal, bool& finished) {
    auto* rows = modal->findChild<QListWidget*>(QStringLiteral("teamResultsList"));
    auto* browse = modal->findChild<QPushButton*>(QStringLiteral("teamBrowseButton"));
    auto* search = modal->findChild<QLineEdit*>(QStringLiteral("teamSearchEdit"));
    if (rows == nullptr || browse == nullptr || search == nullptr) return;
    if (search->text().isEmpty()) {
      if (rows->count() == 0) return;
      compactTeams = rows->sizeHintForRow(0) <= 36;
      rows->setCurrentRow(0);
      if (!browse->isEnabled()) return;
      sawBrowseEnabled = true;
      search->setText(QStringLiteral("Other"));
      return;
    }
    if (rows->count() == 0) {
      if (browse->isEnabled()) sawBrowseEnabledWithoutARow = true;
      return;
    }
    if (rows->count() != 1) return;
    narrowedTeam = rows->item(0)->text();
    finished = true;
    modal->reject();
  });
  QVERIFY2(compactTeams, "team choices still use the tall search-result row");
  QVERIFY2(sawBrowseEnabled, "the picker never let a team be chosen");
  QCOMPARE(narrowedTeam, QStringLiteral("Other Team"));
  QVERIFY2(!sawBrowseEnabledWithoutARow,
           "the picker kept a team browsable after the search took it off the list");

  runPickerSession(page, [](QDialog* modal, bool& finished) {
    auto* rows = modal->findChild<QListWidget*>(QStringLiteral("teamResultsList"));
    if (rows == nullptr || rows->count() == 0) return;
    for (int row = 0; row < rows->count(); ++row) {
      if (rows->item(row)->text() == QStringLiteral("Fixture Team")) {
        rows->item(row)->setCheckState(Qt::Checked);
      }
    }
    auto* filter = modal->findChild<QPushButton*>(QStringLiteral("teamFilterButton"));
    if (filter == nullptr) return;
    finished = true;
    QTest::mouseClick(filter, Qt::LeftButton);
  });
  QTRY_VERIFY_WITH_TIMEOUT(QApplication::activeModalWidget() == nullptr, 10000);
  QTRY_VERIFY_WITH_TIMEOUT(catalogueShows(page, QStringLiteral("Team Probe")), 20000);
  QVERIFY2(!catalogueShows(page, QStringLiteral("Second Probe")),
           "the team filter left a title outside the team in the catalogue");

  bool sawTheAppliedTeamChecked = false;
  runPickerSession(page, [&sawTheAppliedTeamChecked](QDialog* modal, bool& finished) {
    auto* rows = modal->findChild<QListWidget*>(QStringLiteral("teamResultsList"));
    if (rows == nullptr || rows->count() == 0) return;
    auto* filter = modal->findChild<QPushButton*>(QStringLiteral("teamFilterButton"));
    if (filter == nullptr) return;
    for (int row = 0; row < rows->count(); ++row) {
      QListWidgetItem* item = rows->item(row);
      if (item->text() != QStringLiteral("Fixture Team")) continue;
      if (item->checkState() != Qt::Checked) return;
      sawTheAppliedTeamChecked = true;
      item->setCheckState(Qt::Unchecked);
      finished = true;
      QTest::mouseClick(filter, Qt::LeftButton);
      return;
    }
  });
  QVERIFY2(sawTheAppliedTeamChecked,
           "the picker did not offer the team that is already filtered by");
  QTRY_VERIFY_WITH_TIMEOUT(QApplication::activeModalWidget() == nullptr, 10000);
  QTRY_VERIFY_WITH_TIMEOUT(catalogueShows(page, QStringLiteral("Second Probe")), 20000);
}

void TestPublicApiUi::teamLinksShowServiceNamesAndOpenOnlyOnActivation() {
  BrowserUrlCapture browser;
  IsolatedSettings settings;
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Team-link fixture workers did not stop");
  });
  mikai::app::SearchPage page(runner, client);
  page.resize(1120, 800);
  page.show();
  auto* heading = page.findChild<QLabel*>(QStringLiteral("sectionHeading"));
  QVERIFY(heading != nullptr);
  const QString initialHeading = heading->text();
  QTRY_VERIFY_WITH_TIMEOUT(heading->text() != initialHeading, 10000);
  page.browseTeam(QStringLiteral("fixture-team"));

  QLabel* detail = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT(
      (detail = page.findChild<QLabel*>(QStringLiteral("teamDetailLabel"))) != nullptr, 10000);
  QTRY_VERIFY_WITH_TIMEOUT(detail->text().contains(api.teamName()), 10000);

  QTextDocument document;
  document.setHtml(detail->text());
  const QString visible = document.toPlainText();
  QVERIFY(visible.startsWith(api.teamName() + QLatin1Char('\n') + api.teamDescription()));
  const QStringList lines = visible.split(QLatin1Char('\n'));
  for (const QString& service : {QStringLiteral("Monobase"), QStringLiteral("Patreon"),
                                QStringLiteral("Telegram"), QStringLiteral("YouTube")}) {
    QVERIFY(lines.contains(service));
  }
  QVERIFY(!visible.contains(QStringLiteral("https://")));
  QVERIFY(!visible.contains(QStringLiteral("t.me")));
  QVERIFY(browser.opened.isEmpty());

  QMap<QString, QUrl> links;
  for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
    for (auto it = block.begin(); !it.atEnd(); ++it) {
      const QTextFragment fragment = it.fragment();
      if (fragment.isValid() && fragment.charFormat().isAnchor()) {
        links.insert(fragment.text(), QUrl(fragment.charFormat().anchorHref()));
      }
    }
  }
  QCOMPARE(links.value(QStringLiteral("YouTube")),
           QUrl(QStringLiteral("https://www.youtube.com/@%D0%A0%D1%96%D0%B4%D0%BB%D0%B9%D0%93%D0%BE%D0%BB%D0%BE%D1%81")));
  QCOMPARE(links.value(QStringLiteral("Telegram")), QUrl(QStringLiteral("https://t.me/fixture_team")));

  page.activateWindow();
  detail->setFocus();
  QTRY_VERIFY(detail->hasFocus());
  QTest::keyClick(detail, Qt::Key_Tab);
  QTest::keyClick(detail, Qt::Key_Return);
  QTRY_COMPARE(browser.opened,
               QList<QUrl>{QUrl(QStringLiteral("https://base.monobank.ua/EtUwD5aeeKXKWD"))});
}

void TestPublicApiUi::homeAndEscapeReturnFromTeamAndSearch() {
  BrowserUrlCapture browser;
  IsolatedSettings settings;
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Navigation fixture workers did not stop");
  });
  mikai::app::SearchPage page(runner, client);
  page.resize(1120, 800);
  page.show();
  page.activateWindow();
  auto* heading = page.findChild<QLabel*>(QStringLiteral("sectionHeading"));
  auto* discovery = page.findChild<QScrollArea*>(QStringLiteral("discoveryScroll"));
  auto* results = page.findChild<QListWidget*>(QStringLiteral("searchResults"));
  auto* query = page.findChild<QLineEdit*>(QStringLiteral("searchQuery"));
  auto* home = button(page, "homeButton");
  QVERIFY(heading && discovery && results && query && home);
  const QString initialHeading = heading->text();
  QTRY_VERIFY_WITH_TIMEOUT(heading->text() != initialHeading, 10000);

  page.browseTeam(QStringLiteral("fixture-team"));
  QLabel* detail = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT(
      (detail = page.findChild<QLabel*>(QStringLiteral("teamDetailLabel"))) != nullptr &&
          detail->isVisible() && detail->text().contains(api.teamName()), 10000);
  QVERIFY(!discovery->isVisible());

  auto* menu = page.findChild<QMenu*>(QStringLiteral("openMediaMenu"));
  QVERIFY(menu);
  menu->popup(home->mapToGlobal(QPoint(0, home->height())));
  QTRY_VERIFY(menu->isVisible());
  QTest::keyClick(menu, Qt::Key_Escape);
  QTRY_VERIFY(!menu->isVisible());
  QVERIFY(detail->isVisible());
  QVERIFY(!discovery->isVisible());

  auto* modal = new QDialog(&page);
  modal->open();
  QTRY_VERIFY(modal->isVisible());
  QTest::keyClick(modal, Qt::Key_Escape);
  QTRY_VERIFY(!modal->isVisible());
  QVERIFY(detail->isVisible());
  QVERIFY(!discovery->isVisible());

  QTest::mouseClick(home, Qt::LeftButton);
  QTRY_VERIFY(discovery->isVisible());
  QVERIFY(detail->isHidden());
  QVERIFY(results->isHidden());

  page.browseTeam(QStringLiteral("fixture-team"));
  QTRY_VERIFY(detail->isVisible());
  page.activateWindow();
  detail->setFocus();
  QTRY_VERIFY(detail->hasFocus());
  QTest::keyClick(detail, Qt::Key_Escape);
  QTRY_VERIFY(discovery->isVisible());
  QVERIFY(detail->isHidden());

  query->setText(QStringLiteral("Probe"));
  query->setFocus();
  QTest::keyClick(query, Qt::Key_Return);
  QTRY_VERIFY_WITH_TIMEOUT(results->isVisible() && results->count() > 0, 10000);
  results->setFocus();
  QTRY_VERIFY(results->hasFocus());
  QTest::keyClick(results, Qt::Key_Escape);
  QTRY_VERIFY(discovery->isVisible());
  QVERIFY(query->text().isEmpty());

  for (int press = 0; press < 3; ++press) {
    QTest::keyClick(home, Qt::Key_Escape);
    QVERIFY(page.isVisible());
    QVERIFY(discovery->isVisible());
    QVERIFY(results->isHidden());
  }
  QVERIFY(browser.opened.isEmpty());
}

void TestPublicApiUi::escapeCancelsPendingTeamNavigationAtHome() {
  BrowserUrlCapture browser;
  IsolatedSettings settings;
  ApiFixture api;
  api.holdTeamResponses = true;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Pending navigation workers did not stop");
  });
  mikai::app::SearchPage page(runner, client);
  page.resize(1120, 800);
  page.show();
  page.activateWindow();
  auto* heading = page.findChild<QLabel*>(QStringLiteral("sectionHeading"));
  auto* discovery = page.findChild<QScrollArea*>(QStringLiteral("discoveryScroll"));
  auto* query = page.findChild<QLineEdit*>(QStringLiteral("searchQuery"));
  QVERIFY(heading && discovery && query);
  const QString initialHeading = heading->text();
  QTRY_VERIFY_WITH_TIMEOUT(heading->text() != initialHeading, 10000);

  page.browseTeam(QStringLiteral("fixture-team"));
  QTRY_VERIFY_WITH_TIMEOUT(api.teamResponsePending(), 10000);
  query->setFocus();
  QTRY_VERIFY(query->hasFocus());
  QTest::keyClick(query, Qt::Key_Escape);
  QVERIFY(discovery->isVisible());

  api.releaseTeamResponses();
  QTRY_VERIFY_WITH_TIMEOUT(runner.waitForDone(0), 10000);
  QCoreApplication::processEvents();
  auto* detail = page.findChild<QLabel*>(QStringLiteral("teamDetailLabel"));
  QVERIFY(detail == nullptr || detail->isHidden());
  QVERIFY(discovery->isVisible());
  QVERIFY(page.isVisible());
  QVERIFY(browser.opened.isEmpty());
}

void TestPublicApiUi::titleHomeSkipsHistoryAndEscapeUnwindsDetailsAndHistory() {
  BrowserUrlCapture browser;
  IsolatedSettings settings;
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::MainWindow window(client);
  QTimer dismissSetupPrompt;
  dismissSetupPrompt.setInterval(10);
  connect(&dismissSetupPrompt, &QTimer::timeout, &window, [] {
    if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) modal->reject();
  });
  dismissSetupPrompt.start();
  auto* runner = window.findChild<mikai::app::Runner*>();
  QVERIFY(runner);
  const auto drainBeforeClientDestruction = qScopeGuard([runner] {
    runner->cancel();
    if (!runner->waitForDone(10000)) qFatal("Title navigation workers did not stop");
  });
  window.resize(1120, 800);
  window.show();
  window.activateWindow();
  auto* search = window.findChild<mikai::app::SearchPage*>();
  QVERIFY(search);
  auto* query = search->findChild<QLineEdit*>(QStringLiteral("searchQuery"));
  auto* results = search->findChild<QListWidget*>(QStringLiteral("searchResults"));
  auto* discovery = search->findChild<QScrollArea*>(QStringLiteral("discoveryScroll"));
  auto* heading = search->findChild<QLabel*>(QStringLiteral("sectionHeading"));
  QVERIFY(query && results && discovery && heading);
  const QString initialHeading = heading->text();
  QTRY_VERIFY_WITH_TIMEOUT(heading->text() != initialHeading, 10000);

  for (int route = 0; route < 2; ++route) {
    query->setText(QStringLiteral("Probe"));
    query->setFocus();
    QTest::keyClick(query, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(results->isVisible() && results->count() > 0, 10000);
    QTest::mouseClick(results->viewport(), Qt::LeftButton, Qt::NoModifier,
                      results->visualItemRect(results->item(0)).center());
    QTRY_VERIFY_WITH_TIMEOUT(visibleTitle(window) != nullptr, 10000);
    auto* title = visibleTitle(window);
    QTRY_COMPARE_WITH_TIMEOUT(labelText(*title, "titleHeading"), QStringLiteral("Probe"), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(relatedTitleButton(*title) != nullptr, 10000);
    auto* related = relatedTitleButton(*title);
    auto* overview = title->findChild<QScrollArea*>(QStringLiteral("titleOverview"));
    QVERIFY(overview);
    overview->ensureWidgetVisible(related);
    QTest::mouseClick(related, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(
        visibleTitle(window) != nullptr &&
            labelText(*visibleTitle(window), "titleHeading") == QStringLiteral("Related Probe"), 10000);
    title = visibleTitle(window);

    if (route == 0) {
      auto* home = button(*title, "titleHome");
      QVERIFY(home && home->isVisible());
      QTest::mouseClick(home, Qt::LeftButton);
      QTRY_VERIFY(discovery->isVisible());
      QVERIFY(visibleTitle(window) == nullptr);
      continue;
    }

    auto* actions = title->findChild<QWidget*>(QStringLiteral("titleDisclosureActions"));
    QVERIFY(actions);
    QPushButton* detailsAction = nullptr;
    for (auto* action : actions->findChildren<QPushButton*>(QStringLiteral("textAction"))) {
      if (action->isVisible() && action->isCheckable() && !action->isChecked()) detailsAction = action;
    }
    auto* overlay = title->findChild<QWidget*>(QStringLiteral("titleDetailsOverlay"));
    auto* close = button(*title, "titleDetailsClose");
    QVERIFY(detailsAction && overlay && close);
    auto* detailsOverview = title->findChild<QScrollArea*>(QStringLiteral("titleOverview"));
    QVERIFY(detailsOverview);
    detailsOverview->ensureWidgetVisible(detailsAction);
    QCoreApplication::processEvents();
    QTest::mouseClick(detailsAction, Qt::LeftButton);
    QTRY_VERIFY(overlay->isVisible());
    window.activateWindow();
    close->setFocus();
    QTRY_VERIFY(close->hasFocus());
    QTest::keyClick(close, Qt::Key_Escape);
    QTRY_VERIFY(!overlay->isVisible());
    QCOMPARE(labelText(*title, "titleHeading"), QStringLiteral("Related Probe"));

    QTest::keyClick(button(*title, "titleBack"), Qt::Key_Escape);
    QTRY_VERIFY_WITH_TIMEOUT(
        visibleTitle(window) != nullptr &&
            labelText(*visibleTitle(window), "titleHeading") == QStringLiteral("Probe"), 10000);
    QTest::keyClick(button(*visibleTitle(window), "titleBack"), Qt::Key_Escape);
    QTRY_VERIFY(results->isVisible());
    QTest::keyClick(results, Qt::Key_Escape);
    QTRY_VERIFY(discovery->isVisible());
  }

  QTest::keyClick(button(*search, "homeButton"), Qt::Key_Escape);
  QVERIFY(window.isVisible());
  QVERIFY(discovery->isVisible());
  QVERIFY(browser.opened.isEmpty());
}

int runTestPublicApiUi(int argc, char** argv) {
  TestPublicApiUi suite;
  QStringList arguments;
  arguments.reserve(argc + 1);
  arguments << QString::fromLocal8Bit(argv[0]);
  for (int i = 1; i < argc; ++i) arguments << QString::fromLocal8Bit(argv[i]);
  QTemporaryFile report;
  const bool capture = !arguments.contains(QStringLiteral("-o"));
  if (capture) {
    if (!report.open()) return 2;
    report.close();
    arguments << QStringLiteral("-o") << report.fileName() + QStringLiteral(",txt");
  }
  const int rc = QTest::qExec(&suite, arguments);
  if (capture && rc != 0) {
    QFile output(report.fileName());
    if (output.open(QIODevice::ReadOnly)) {
      const QByteArray text = output.readAll();
      std::fwrite(text.constData(), 1, static_cast<size_t>(text.size()), stderr);
    }
  }
  std::fflush(stderr);
  return rc;
}

#include "TestPublicApiUi.moc"
