#include "TestPublicApiUi.hpp"

#include <backend/MikaiClient.hpp>
#include <backend/Models.hpp>
#include <dialogs/AnimeJanaiManagerPanel.hpp>
#include <dialogs/ComponentStoreTab.hpp>
#include <dialogs/SettingsDialog.hpp>
#include <dialogs/UpscalersPanel.hpp>
#include <native/models/NativeUpscalers.hpp>
#include <dialogs/settings/SettingsUi.hpp>
#include <widgets/PageGlow.hpp>
#include <state/AppearanceStore.hpp>
#include <state/NativeComponentProvisioner.hpp>
#include <pages/MainWindow.hpp>
#include <pages/SearchPage.hpp>
#include <pages/TitlePage.hpp>
#include <player/quick/PlayerController.hpp>
#include <player/shell/PlayerHost.hpp>
#include <player/core/VideoSession.hpp>
#include <state/QualityCache.hpp>
#include <state/WatchHistory.hpp>
#include <support/I18n.hpp>
#include <support/Labels.hpp>
#include <theme/Metrics.hpp>
#include <theme/Theme.hpp>
#include <workers/Runner.hpp>
#include <gui/ImageDecode.hpp>
#include <gui/Win32Support.hpp>

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
#include <QtCore/QStandardPaths>
#include <QtCore/QScopeGuard>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTemporaryFile>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtGui/QDesktopServices>
#include <QtGui/QFontDatabase>
#include <QtGui/QContextMenuEvent>
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
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QSlider>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTabWidget>
#include <cstdio>
#include <functional>

namespace {

const QString kFixtureKey = QStringLiteral("mk_public_api_ui_fixture");
class QuickPlayerHost final : public mplayer::shell::PlayerHost {
 public:
  void requestVariant(const QString&, const QString&) override {}
  void requestEpisode(int) override {}
  void requestTrackProbe(const QString&) override {}
  void requestCopyStreamUrl() override {}
  void playerClosed() override {}
  void playbackFailed() override {}
};


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

/// Polishes and lays out a whole page with the style applied, so the geometry
/// read below is the one the style produces rather than the pre-style one.
void settleLayout(QWidget* page) {
  page->ensurePolished();
  const QList<QWidget*> widgets = page->findChildren<QWidget*>();
  for (QWidget* widget : widgets) {
    widget->ensurePolished();
  }
  for (int round = 0; round < 3; ++round) {
    for (QWidget* widget : widgets) {
      if (QLayout* layout = widget->layout()) layout->activate();
    }
    if (QLayout* layout = page->layout()) layout->activate();
    QCoreApplication::processEvents();
  }
}

/// Any scroll area on the surface that needs a horizontal bar: a page wider
/// than its own viewport can only be read by panning it sideways.
QString horizontalOverflow(QWidget* surface) {
  QList<QScrollArea*> scrolls = surface->findChildren<QScrollArea*>();
  if (auto* self = qobject_cast<QScrollArea*>(surface)) scrolls.prepend(self);
  for (QScrollArea* scroll : scrolls) {
    if (scroll != surface && !scroll->isVisibleTo(surface)) continue;
    QScrollBar* bar = scroll->horizontalScrollBar();
    if (bar->minimum() == bar->maximum()) continue;
    return QStringLiteral("%1 needs %2 px of horizontal panning")
        .arg(scroll->objectName().isEmpty() ? QStringLiteral("a scroll area")
                                            : scroll->objectName())
        .arg(bar->maximum() - bar->minimum());
  }
  return QString();
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
      // The scores object the API serves, with the keys the parser reads, so a
      // page under test renders the real two-source score line.
      {QStringLiteral("scores"),
       QJsonObject{{QStringLiteral("mikai"), 8.6},
                   {QStringLiteral("mikaiCount"), 33},
                   {QStringLiteral("mal"), 8.56},
                   {QStringLiteral("malCount"), 158700}}},
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

/// The provider's own URL shape, kept as a path so the fixture can serve it
/// while `detectSite` still recognises the provider.
QString providerUrlShape(const QString& provider) {
  if (provider == QLatin1String("moon")) {
    return QStringLiteral("moonanime.art/iframe");
  }
  if (provider == QLatin1String("tortuga")) {
    return QStringLiteral("tortuga.tw/vod");
  }
  return QStringLiteral("ashdi.vip/vod");
}

/// One episode source naming `provider`, pointed at the fixture itself so the
/// quality probe's request for it is observable.
QJsonObject providerSource(const QString& provider, int port, int episode) {
  return QJsonObject{
      {QStringLiteral("provider"), provider},
      {QStringLiteral("embedUrl"), QStringLiteral("http://127.0.0.1:%1/%2/%3")
                                       .arg(port)
                                       .arg(providerUrlShape(provider))
                                       .arg(episode)}};
}

/// A dub release served by exactly one provider.
QJsonObject providerRelease(const QString& id, const QString& provider,
                            const QString& teamName, int port) {
  QJsonArray episodes;
  for (int n = 1; n <= 2; ++n) {
    const QJsonObject episode{
        {QStringLiteral("number"), n},
        {QStringLiteral("label"), QStringLiteral("Fixture episode")},
        {QStringLiteral("kind"), QStringLiteral("animeCanon")},
        {QStringLiteral("sources"), QJsonArray{providerSource(provider, port, n)}}};
    episodes.append(episode);
  }
  const QJsonObject team{{QStringLiteral("id"), 17},
                         {QStringLiteral("slug"), QStringLiteral("fixture-team")},
                         {QStringLiteral("name"), teamName}};
  return QJsonObject{{QStringLiteral("id"), id},
                     {QStringLiteral("kind"), QStringLiteral("voice")},
                     {QStringLiteral("teams"), QJsonArray{team}},
                     {QStringLiteral("episodes"), episodes},
                     {QStringLiteral("episodesCount"), 2}};
}

/// A dub release whose every episode carries all three providers, listed in
/// payload order (ashdi first), so only a preference can pick Moon.
QJsonObject multiProviderRelease(const QString& id, const QString& teamName, int port) {
  QJsonArray episodes;
  for (int n = 1; n <= 2; ++n) {
    const QJsonArray sources{providerSource(QStringLiteral("ashdi"), port, n),
                             providerSource(QStringLiteral("moon"), port, n),
                             providerSource(QStringLiteral("tortuga"), port, n)};
    const QJsonObject episode{{QStringLiteral("number"), n},
                              {QStringLiteral("label"), QStringLiteral("Fixture episode")},
                              {QStringLiteral("kind"), QStringLiteral("animeCanon")},
                              {QStringLiteral("sources"), sources}};
    episodes.append(episode);
  }
  const QJsonObject team{{QStringLiteral("id"), 17},
                         {QStringLiteral("slug"), QStringLiteral("fixture-team")},
                         {QStringLiteral("name"), teamName}};
  return QJsonObject{{QStringLiteral("id"), id},
                     {QStringLiteral("kind"), QStringLiteral("voice")},
                     {QStringLiteral("teams"), QJsonArray{team}},
                     {QStringLiteral("episodes"), episodes},
                     {QStringLiteral("episodesCount"), 2}};
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
          requests.append(target);
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
  /// Which release list the player endpoint answers with: empty for the
  /// original single-release fixture, "providers" for one release per provider,
  /// "without-moon" for the pair that has no Moon source, "shared" for two
  /// releases each carrying all three providers.
  QString releaseLayout;
  /// Every request target the fixture served, in arrival order.
  QStringList requests;
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
        if (releaseLayout == QLatin1String("providers")) {
          const int port = server_.serverPort();
          result.insert(QStringLiteral("releases"),
                        QJsonArray{providerRelease(QStringLiteral("release-ashdi"),
                                                   QStringLiteral("ashdi"),
                                                   QStringLiteral("Ashdi Team"), port),
                                   providerRelease(QStringLiteral("release-moon"),
                                                   QStringLiteral("moon"),
                                                   QStringLiteral("Moon Team"), port),
                                   providerRelease(QStringLiteral("release-tortuga"),
                                                   QStringLiteral("tortuga"),
                                                   QStringLiteral("Tortuga Team"), port)});
        } else if (releaseLayout == QLatin1String("without-moon")) {
          const int port = server_.serverPort();
          result.insert(QStringLiteral("releases"),
                        QJsonArray{providerRelease(QStringLiteral("release-ashdi"),
                                                   QStringLiteral("ashdi"),
                                                   QStringLiteral("Ashdi Team"), port),
                                   providerRelease(QStringLiteral("release-tortuga"),
                                                   QStringLiteral("tortuga"),
                                                   QStringLiteral("Tortuga Team"), port)});
        } else if (releaseLayout == QLatin1String("shared")) {
          const int port = server_.serverPort();
          result.insert(QStringLiteral("releases"),
                        QJsonArray{multiProviderRelease(QStringLiteral("release-ashdi-row"),
                                                        QStringLiteral("Ashdi Team"), port),
                                   multiProviderRelease(QStringLiteral("release-moon-row"),
                                                        QStringLiteral("Moon Team"), port)});
        } else {
          result.insert(QStringLiteral("releases"), QJsonArray{releasePayload()});
        }
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

QAbstractButton* button(const QWidget& root, const char* name) {
  return root.findChild<QAbstractButton*>(QString::fromLatin1(name));
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
bool indicatorChangesRendering(QWidget* surface, QWidget* indicator) {
  if (surface == nullptr || indicator == nullptr || !indicator->isVisible()) return false;
  const auto renderSurface = [surface] {
    QImage image(surface->size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    surface->render(&painter);
    return image;
  };
  const QImage withIndicator = renderSurface();
  indicator->hide();
  const QImage withoutIndicator = renderSurface();
  indicator->show();
  return withIndicator != withoutIndicator;
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
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Title metadata workers did not stop");
  });
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

  // Pills are created hidden and shown by the next event-loop pass, so a
  // wait for their mere existence can snapshot them before Qt shows them.
  // Wait until one is actually visible instead.
  const auto anyPillVisible = [&page] {
    for (const QPushButton* pill :
         page.findChildren<QPushButton*>(QStringLiteral("episodeButton"))) {
      if (pill->isVisible()) {
        return true;
      }
    }
    return false;
  };
  QTRY_VERIFY_WITH_TIMEOUT(anyPillVisible(), 20000);
  const auto buttons = page.findChildren<QPushButton*>(QStringLiteral("episodeButton"));
  bool sawFillerClassification = false;
  bool sawVisibleFillerIndicator = false;
  for (QPushButton* pill : buttons) {
    auto* classification = pill->findChild<QLabel*>(QStringLiteral("episodeTypeBadge"));
    if (classification == nullptr ||
        !classification->toolTip().contains(QStringLiteral("Filler"), Qt::CaseInsensitive) ||
        !pill->toolTip().contains(QStringLiteral("Filler"), Qt::CaseInsensitive)) {
      continue;
    }
    sawFillerClassification = true;
    sawVisibleFillerIndicator =
        indicatorChangesRendering(pill, classification) || sawVisibleFillerIndicator;
  }
  QVERIFY2(sawFillerClassification,
           "no episode visibly identifies its filler classification");
  QVERIFY2(sawVisibleFillerIndicator,
           "the filler classification has no visible color indicator");

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
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Fractional episode workers did not stop");
  });
  mikai::app::QualityCache cache;
  mikai::app::TitlePage page(runner, client, cache, QStringLiteral("7428"));
  page.resize(1120, 800);
  page.show();

  // Pills are created hidden and shown by the next event-loop pass, so a
  // wait for their mere existence can snapshot them before Qt shows them.
  // Wait until one is actually visible instead.
  const auto anyPillVisible = [&page] {
    for (const QPushButton* pill :
         page.findChildren<QPushButton*>(QStringLiteral("episodeButton"))) {
      if (pill->isVisible()) {
        return true;
      }
    }
    return false;
  };
  QTRY_VERIFY_WITH_TIMEOUT(anyPillVisible(), 20000);
  const auto buttons = page.findChildren<QPushButton*>(QStringLiteral("episodeButton"));
  QPushButton* fractionalFiller = nullptr;
  for (QPushButton* pill : buttons) {
    auto* classification = pill->findChild<QLabel*>(QStringLiteral("episodeTypeBadge"));
    if (pill->text() == QStringLiteral("2.5") && classification != nullptr &&
        classification->toolTip().contains(QStringLiteral("Filler"), Qt::CaseInsensitive)) {
      fractionalFiller = pill;
      break;
    }
  }
  QVERIFY2(fractionalFiller != nullptr,
           "the fractional episode does not visibly identify its filler classification");

  auto* fractionalClassification =
      fractionalFiller->findChild<QLabel*>(QStringLiteral("episodeTypeBadge"));
  QVERIFY(fractionalClassification != nullptr);
  QVERIFY2(indicatorChangesRendering(fractionalFiller, fractionalClassification),
           "the fractional episode's filler indicator does not affect its appearance");
  auto* watchedMark =
      fractionalFiller->findChild<QWidget*>(QStringLiteral("episodeWatchedMark"));
  QVERIFY(watchedMark != nullptr);
  QVERIFY(!watchedMark->isVisible());
  QVERIFY(!fractionalFiller->toolTip().contains(QStringLiteral("watched episode"),
                                                 Qt::CaseInsensitive));

  QTimer menuDriver;
  menuDriver.setInterval(20);
  QObject::connect(&menuDriver, &QTimer::timeout, &page, [&menuDriver] {
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    if (menu == nullptr) return;
    for (QAction* action : menu->actions()) {
      if (action->text().contains(QStringLiteral("Mark as watched"),
                                  Qt::CaseInsensitive) &&
          action->isEnabled()) {
        action->trigger();
        menu->close();
        menuDriver.stop();
        return;
      }
    }
  });
  menuDriver.start();
  const QPoint menuPosition = fractionalFiller->rect().center();
  QContextMenuEvent contextMenuEvent(
      QContextMenuEvent::Mouse, menuPosition, fractionalFiller->mapToGlobal(menuPosition));
  QApplication::sendEvent(fractionalFiller, &contextMenuEvent);
  menuDriver.stop();

  QTRY_VERIFY_WITH_TIMEOUT(watchedMark->isVisible(), 5000);
  QVERIFY2(fractionalFiller->toolTip().contains(QStringLiteral("watched episode"),
                                                 Qt::CaseInsensitive),
           qPrintable(fractionalFiller->toolTip()));
  QVERIFY2(fractionalFiller->toolTip().contains(QStringLiteral("Filler"),
                                                 Qt::CaseInsensitive),
           qPrintable(fractionalFiller->toolTip()));
  QVERIFY2(fractionalFiller->findChild<QLabel*>(QStringLiteral("episodeTypeBadge"))->toolTip()
               .contains(QStringLiteral("Filler"), Qt::CaseInsensitive),
           "the classification is no longer presented after marking the episode watched");
  QVERIFY2(indicatorChangesRendering(fractionalFiller, fractionalClassification),
           "the filler indicator disappeared after marking the episode watched");
}

void TestPublicApiUi::licensedTitleShowsLegalInformation() {
  IsolatedSettings settings;
  ApiFixture api;
  api.licensed = true;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Licensed title workers did not stop");
  });
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
}

void TestPublicApiUi::anUnlicensedTitleShowsNoLegalInformation() {
  IsolatedSettings settings;
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Unlicensed title workers did not stop");
  });
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
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Explicit title workers did not stop");
  });
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

void TestPublicApiUi::headerLogoTracksLightAndDarkAppearance() {
  IsolatedSettings settings;
  const auto saved = mikai::app::currentAppearance();
  const auto restore = qScopeGuard([saved] {
    mikai::app::setCurrentAppearance(saved);
    mikai::app::theme::applyPalette(saved);
    qApp->setStyleSheet(mikai::app::theme::buildStylesheet(saved));
  });
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Header logo workers did not stop");
  });
  mikai::app::SearchPage page(runner, client);
  page.resize(900, 640);
  page.show();

  auto* logo = page.findChild<QLabel*>(QStringLiteral("headerLogo"));
  QVERIFY(logo != nullptr);
  QTRY_VERIFY(logo->isVisible());

  auto hasTintedAlpha = [logo](const QColor& tint) {
    QImage image(logo->size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    logo->render(&image);
    bool transparent = false;
    bool tinted = false;
    for (int y = 0; y < image.height(); ++y) {
      for (int x = 0; x < image.width(); ++x) {
        const QColor pixel = image.pixelColor(x, y);
        transparent |= pixel.alpha() == 0;
        tinted |= pixel.alpha() > 0 && pixel.red() == tint.red() &&
                  pixel.green() == tint.green() && pixel.blue() == tint.blue();
      }
    }
    return transparent && tinted;
  };

  auto appearance = mikai::app::currentAppearance();
  appearance.siteTheme = mikai::app::theme::SiteTheme::Dark;
  mikai::app::setCurrentAppearance(appearance);
  mikai::app::theme::applyPalette(appearance);
  qApp->setStyleSheet(mikai::app::theme::buildStylesheet(appearance));
  QTRY_VERIFY(hasTintedAlpha(logo->palette().color(QPalette::WindowText)));
  const QColor darkTint = logo->palette().color(QPalette::WindowText);

  appearance.siteTheme = mikai::app::theme::SiteTheme::Light;
  mikai::app::setCurrentAppearance(appearance);
  mikai::app::theme::applyPalette(appearance);
  qApp->setStyleSheet(mikai::app::theme::buildStylesheet(appearance));
  QTRY_VERIFY(hasTintedAlpha(logo->palette().color(QPalette::WindowText)));
  QVERIFY(logo->palette().color(QPalette::WindowText) != darkTint);
  QVERIFY(logo->isVisible());
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
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Cover shading workers did not stop");
  });
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
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Poster fallback workers did not stop");
  });
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
  // Auto is the default source and it paints the accent, so the colour picker is
  // disabled until the viewer asks for a custom gradient. The switch goes
  // through the page's own control, exactly as a viewer would.
  auto* mode = dialog.findChild<QComboBox*>(QStringLiteral("gradientModeCombo"));
  QVERIFY(mode != nullptr);
  const int custom = mode->findData(QStringLiteral("custom"));
  QVERIFY2(custom >= 0, "the gradient source offers a custom entry");
  mode->setCurrentIndex(custom);
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

void TestPublicApiUi::aComponentsTabOutlivesTheProvisionerItsWindowOwns() {
  IsolatedSettings settings;
  mikai::app::Runner runner;
  auto* provisioner = new mikai::app::NativeComponentProvisioner(runner);
  mikai::app::ComponentStoreTab tab;
  tab.setRunner(&runner);
  tab.setComponentProvisioner(provisioner);
  auto* verify = tab.findChild<QPushButton*>(QStringLiteral("nativeComponentVerifyButton"));
  auto* cancel = tab.findChild<QPushButton*>(QStringLiteral("nativeComponentTabCancelButton"));
  QVERIFY(verify != nullptr && cancel != nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(!tab.acquisitionRunning(), 20000);
  QVERIFY(!tab.downloadActive());
  QVERIFY(verify->isEnabled());
  QVERIFY(!cancel->isEnabled());

  // The window owns the provisioner for the process and the tab only borrows it,
  // so the owner can be torn down first: MainWindow's children are deleted in
  // creation order - Runner, then the provisioner, then this tab.
  delete provisioner;
  provisioner = nullptr;

  // Each of these recomputes the running state from the borrowed service, so a
  // surface that reached freed state would show it here.
  tab.refresh();
  tab.cancelAcquisition();
  QVERIFY(!tab.downloadActive());
  QVERIFY(!tab.acquisitionRunning());
  QVERIFY2(verify->isEnabled(),
           "the tab stayed busy after its provisioner owner went away");
  QVERIFY2(!cancel->isEnabled(), "Cancel stayed armed with no provisioner behind it");

  // A live replacement must rebind the borrowed reference, not inherit the old one.
  mikai::app::NativeComponentProvisioner replacement(runner);
  tab.setComponentProvisioner(&replacement);
  tab.refresh();
  tab.cancelAcquisition();
  QVERIFY(!tab.downloadActive());
  QVERIFY(!tab.acquisitionRunning());
  QVERIFY(verify->isEnabled());
  QVERIFY(!cancel->isEnabled());
  tab.setComponentProvisioner(nullptr);
}

void TestPublicApiUi::catalogueFiltersRetainCanonicalSelectionsAndReset() {
  qApp->setStyle(QStringLiteral("Fusion"));
  qApp->setStyleSheet(mikai::app::theme::buildStylesheet());
  IsolatedSettings settings;
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Catalogue filter workers did not stop");
  });
  mikai::app::SearchPage page(runner, client);
  page.resize(1120, 800);
  page.show();

  auto* heading = page.findChild<QLabel*>(QStringLiteral("pageHeading"));
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

  auto* heading = page.findChild<QLabel*>(QStringLiteral("pageHeading"));
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
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Team browsing workers did not stop");
  });
  mikai::app::SearchPage page(runner, client);
  page.resize(1120, 800);
  page.show();

  auto* heading = page.findChild<QLabel*>(QStringLiteral("pageHeading"));
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
  auto* heading = page.findChild<QLabel*>(QStringLiteral("pageHeading"));
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
  auto* heading = page.findChild<QLabel*>(QStringLiteral("pageHeading"));
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
  auto* heading = page.findChild<QLabel*>(QStringLiteral("pageHeading"));
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
  QCOMPARE(window.windowTitle(), QStringLiteral("Mikai Ja Nai"));
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
  auto* heading = search->findChild<QLabel*>(QStringLiteral("pageHeading"));
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

    // The details control is a named hero action, not one of a subset of generic
    // disclosure chips: it has to exist, be offered by the loaded page, and be
    // closed before the click means anything.
    auto* detailsAction = title->findChild<QPushButton*>(QStringLiteral("titleDetailsToggle"));
    QVERIFY(detailsAction != nullptr);
    QVERIFY2(detailsAction->isVisible(), "the loaded title offers no visible details action");
    QVERIFY2(detailsAction->isCheckable() && !detailsAction->isChecked(),
             "the details action must open the sheet from its closed state");
    auto* overlay = title->findChild<QWidget*>(QStringLiteral("titleDetailsOverlay"));
    auto* close = button(*title, "titleDetailsClose");
    QVERIFY(overlay != nullptr && close != nullptr);
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

namespace {

/// A one-source release, enough for the pure selection helpers.
mikai::backend::Release providerReleaseModel(const QString& provider) {
  mikai::backend::Release release;
  release.id = provider;
  release.kind = QStringLiteral("voice");
  mikai::backend::Episode episode;
  episode.number = 1;
  mikai::backend::Source source;
  source.provider = provider;
  episode.sources.append(source);
  release.episodes.append(episode);
  return release;
}

QPushButton* chipForTeam(const QWidget& page, const QString& team) {
  for (QPushButton* chip : page.findChildren<QPushButton*>(QStringLiteral("releaseChip"))) {
    if (chip->toolTip().contains(team)) {
      return chip;
    }
  }
  return nullptr;
}

int tabForText(const QTabWidget* tabs, const QString& text) {
  for (int index = 0; index < tabs->count(); ++index) {
    if (tabs->tabText(index) == text) {
      return index;
    }
  }
  return -1;
}

}  // namespace

void TestPublicApiUi::providerPreferenceLeadsMoonAndFallsBackDeterministically() {
  IsolatedSettings settings;
  namespace labels = mikai::app::labels;
  QCOMPARE(labels::providerKey(QStringLiteral(" Moon_Anime ")), QStringLiteral("moonanime"));
  QCOMPARE(labels::providerKey(QStringLiteral("MOON")), QStringLiteral("moon"));
  QCOMPARE(labels::providerKey(QStringLiteral("Ashdi")), QStringLiteral("ashdi"));
  QCOMPARE(labels::preferredProviderRank(QStringLiteral("moon"), QStringLiteral("moon")), 0);
  QCOMPARE(labels::preferredProviderRank(QStringLiteral("MoonAnime"), QStringLiteral("moon")), 2);
  QCOMPARE(labels::preferredProviderRank(QStringLiteral("moon"), QStringLiteral("tortuga")), 1);
  QCOMPARE(labels::preferredProviderRank(QStringLiteral("ashdi"), QStringLiteral("moon")), 3);

  const QStringList providers{QStringLiteral("ashdi"), QStringLiteral("moon"),
                              QStringLiteral("tortuga")};
  QCOMPARE(labels::orderedProviders(providers, QStringLiteral("moon")),
           QStringList({QStringLiteral("moon"), QStringLiteral("ashdi"),
                        QStringLiteral("tortuga")}));
  QCOMPARE(labels::orderedProviders(
               QStringList({QStringLiteral("ashdi"), QStringLiteral("tortuga")}),
               QStringLiteral("moon")),
           QStringList({QStringLiteral("ashdi"), QStringLiteral("tortuga")}));
  QCOMPARE(labels::orderedProviders(providers, QStringLiteral("tortuga")),
           QStringList({QStringLiteral("tortuga"), QStringLiteral("moon"),
                        QStringLiteral("ashdi")}));

  const QVector<mikai::backend::Release> allThree{
      providerReleaseModel(QStringLiteral("fenix")), providerReleaseModel(QStringLiteral("moon")),
      providerReleaseModel(QStringLiteral("starly"))};
  QCOMPARE(labels::preferredReleaseIndex(allThree, QStringLiteral("moon")), 1);
  const QVector<mikai::backend::Release> withoutMoon{
      providerReleaseModel(QStringLiteral("fenix")), providerReleaseModel(QStringLiteral("starly"))};
  QCOMPARE(labels::preferredReleaseIndex(withoutMoon, QStringLiteral("moon")), 0);
  QCOMPARE(labels::preferredReleaseIndex(withoutMoon, QStringLiteral("tortuga")), 0);
  QCOMPARE(labels::preferredReleaseIndex({}, QStringLiteral("moon")), -1);

  // Nothing stored means Moon; a stored preference is normalised rather than
  // trusted verbatim, because the tab path compares normalised keys.
  QCOMPARE(labels::preferredProvider(), QStringLiteral("moon"));
  QSettings().setValue(QStringLiteral("playback/preferredProvider"),
                       QStringLiteral(" Tortuga_"));
  QCOMPARE(labels::preferredProvider(), QStringLiteral("tortuga"));
  QSettings().remove(QStringLiteral("playback/preferredProvider"));
}

void TestPublicApiUi::moonLeadsTheProviderTabsAndReleaseChips() {
  IsolatedSettings settings;
  ApiFixture api;
  api.releaseLayout = QStringLiteral("providers");
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Moon default workers did not stop");
  });
  mikai::app::QualityCache cache;
  mikai::app::TitlePage page(runner, client, cache, QStringLiteral("7428"));
  page.resize(1120, 800);
  page.show();

  auto* tabs = page.findChild<QTabWidget*>(QStringLiteral("episodeTabs"));
  QVERIFY2(tabs != nullptr, "the title page exposes no provider tabs");
  // Each fixture release is served by one provider, so the opened release has a
  // single tab: it has to be the Moon release, not the Ashdi one.
  QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 20000);
  QTRY_COMPARE_WITH_TIMEOUT(tabs->tabText(tabs->currentIndex()),
                            QStringLiteral("MoonAnime"), 10000);
  QCOMPARE(tabs->currentIndex(), 0);

  QPushButton* moonChip = chipForTeam(page, QStringLiteral("Moon Team"));
  QPushButton* ashdiChip = chipForTeam(page, QStringLiteral("Ashdi Team"));
  QPushButton* tortugaChip = chipForTeam(page, QStringLiteral("Tortuga Team"));
  QVERIFY2(moonChip != nullptr && ashdiChip != nullptr && tortugaChip != nullptr,
           "the fixture's three provider releases did not reach the chip row");
  QTRY_VERIFY_WITH_TIMEOUT(moonChip->isChecked(), 5000);
  QVERIFY(!ashdiChip->isChecked());
  QVERIFY(!tortugaChip->isChecked());

  // The watch button follows the selected tab, so the watch panel's meta line has
  // to name the same provider the tab does. The hero's duplicate caption line was
  // removed; the panel that owns the state is the one that states it.
  auto* watchMeta = page.findChild<QLabel*>(QStringLiteral("titleWatchMeta"));
  QVERIFY(watchMeta != nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(watchMeta->text().contains(QStringLiteral("MoonAnime")), 5000);
}

void TestPublicApiUi::aTitleWithoutMoonKeepsItsFirstProvider() {
  IsolatedSettings settings;
  ApiFixture api;
  api.releaseLayout = QStringLiteral("without-moon");
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Fallback workers did not stop");
  });
  mikai::app::QualityCache cache;
  mikai::app::TitlePage page(runner, client, cache, QStringLiteral("7428"));
  page.resize(1120, 800);
  page.show();

  auto* tabs = page.findChild<QTabWidget*>(QStringLiteral("episodeTabs"));
  QVERIFY(tabs != nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 20000);
  QTRY_COMPARE_WITH_TIMEOUT(tabs->tabText(tabs->currentIndex()), QStringLiteral("Ashdi"), 10000);
  QCOMPARE(tabs->currentIndex(), 0);

  QPushButton* ashdiChip = chipForTeam(page, QStringLiteral("Ashdi Team"));
  QPushButton* tortugaChip = chipForTeam(page, QStringLiteral("Tortuga Team"));
  QVERIFY(ashdiChip != nullptr && tortugaChip != nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(ashdiChip->isChecked(), 5000);
  QVERIFY(!tortugaChip->isChecked());
}

void TestPublicApiUi::anExplicitProviderChoiceOutlivesTheReleaseRebuild() {
  IsolatedSettings settings;
  ApiFixture api;
  api.releaseLayout = QStringLiteral("shared");
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Provider choice workers did not stop");
  });
  mikai::app::QualityCache cache;
  mikai::app::TitlePage page(runner, client, cache, QStringLiteral("7428"));
  page.resize(1120, 800);
  page.show();

  auto* overview = page.findChild<QScrollArea*>(QStringLiteral("titleOverview"));
  auto* tabs = page.findChild<QTabWidget*>(QStringLiteral("episodeTabs"));
  QVERIFY2(overview != nullptr && tabs != nullptr, "the title page exposes no provider tabs");
  QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 3, 20000);
  QTRY_COMPARE_WITH_TIMEOUT(tabs->tabText(tabs->currentIndex()),
                            QStringLiteral("MoonAnime"), 10000);
  QPushButton* moonChip = chipForTeam(page, QStringLiteral("Moon Team"));
  QPushButton* ashdiChip = chipForTeam(page, QStringLiteral("Ashdi Team"));
  QVERIFY(moonChip != nullptr && ashdiChip != nullptr);
  // Both releases carry Moon; stable release order wins the tie.
  QTRY_VERIFY_WITH_TIMEOUT(ashdiChip->isChecked(), 5000);

  tabs->tabBar()->setFocus(Qt::OtherFocusReason);
  QTest::keyClick(tabs->tabBar(), Qt::Key_Right);
  QTRY_VERIFY_WITH_TIMEOUT(tabs->currentIndex() == 1, 5000);
  const QString keyboardChoice = tabs->tabText(tabs->currentIndex());
  QVERIFY2(keyboardChoice != QStringLiteral("MoonAnime"),
           "one tab right of the opening tab is still the preferred provider");

  QTest::mouseClick(moonChip, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(moonChip->isChecked(), 5000);
  QTRY_COMPARE_WITH_TIMEOUT(tabs->tabText(tabs->currentIndex()), keyboardChoice, 10000);

  const int tortuga = tabForText(tabs, QStringLiteral("Tortuga"));
  QVERIFY(tortuga > 0);
  overview->ensureWidgetVisible(tabs->tabBar());
  QCoreApplication::processEvents();
  QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier,
                    tabs->tabBar()->tabRect(tortuga).center());
  QTRY_COMPARE_WITH_TIMEOUT(tabs->currentIndex(), tortuga, 5000);

  QTest::mouseClick(ashdiChip, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(ashdiChip->isChecked(), 5000);
  QTRY_COMPARE_WITH_TIMEOUT(tabs->tabText(tabs->currentIndex()), QStringLiteral("Tortuga"),
                            10000);
}

void TestPublicApiUi::theReleaseEmbedResolvesToTheMoonSource() {
  IsolatedSettings settings;
  ApiFixture api;
  api.releaseLayout = QStringLiteral("shared");
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Embed probe workers did not stop");
  });
  mikai::app::QualityCache cache;
  mikai::app::TitlePage page(runner, client, cache, QStringLiteral("7428"));
  page.resize(1120, 800);
  page.show();

  auto* tabs = page.findChild<QTabWidget*>(QStringLiteral("episodeTabs"));
  QVERIFY(tabs != nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 3, 20000);
  QPushButton* moonChip = chipForTeam(page, QStringLiteral("Moon Team"));
  QVERIFY(moonChip != nullptr);

  // A chip click starts the quality probe for every release; the page it
  // fetches is firstEmbed()'s answer, not the first-listed source.
  QTest::mouseClick(moonChip, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(
      api.requests.join(QLatin1Char('|')).contains(QStringLiteral("moonanime.art/iframe/1")),
      15000);
  QVERIFY2(!api.requests.join(QLatin1Char('|')).contains(QStringLiteral("ashdi.vip/vod/1")),
           qPrintable(QStringLiteral("the release resolved its first-listed source: %1")
                          .arg(api.requests.join(QLatin1Char('|')))));
}

void TestPublicApiUi::aViewerChoiceLeadsTheProgrammaticWatchEmbed() {
  IsolatedSettings settings;
  ApiFixture api;
  api.releaseLayout = QStringLiteral("shared");
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Session choice workers did not stop");
  });
  mikai::app::QualityCache cache;
  mikai::app::TitlePage page(runner, client, cache, QStringLiteral("7428"));
  page.resize(1120, 800);
  page.show();

  auto* overview = page.findChild<QScrollArea*>(QStringLiteral("titleOverview"));
  auto* tabs = page.findChild<QTabWidget*>(QStringLiteral("episodeTabs"));
  QVERIFY2(overview != nullptr && tabs != nullptr, "the title page exposes no provider tabs");
  QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 3, 20000);
  QTRY_COMPARE_WITH_TIMEOUT(tabs->tabText(tabs->currentIndex()),
                            QStringLiteral("MoonAnime"), 10000);

  const int tortuga = tabForText(tabs, QStringLiteral("Tortuga"));
  QVERIFY(tortuga >= 0);
  overview->ensureWidgetVisible(tabs->tabBar());
  QCoreApplication::processEvents();
  QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier,
                    tabs->tabBar()->tabRect(tortuga).center());
  QTRY_COMPARE_WITH_TIMEOUT(tabs->currentIndex(), tortuga, 5000);

  // The viewer picked Tortuga, so the embed the page resolves for a quality
  // probe (and hands to the player) has to be Tortuga's, not Moon's.
  QPushButton* ashdiChip = chipForTeam(page, QStringLiteral("Ashdi Team"));
  QVERIFY(ashdiChip != nullptr);
  QTest::mouseClick(ashdiChip, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(
      api.requests.join(QLatin1Char('|')).contains(QStringLiteral("tortuga.tw/vod/1")), 15000);
  QVERIFY2(!api.requests.join(QLatin1Char('|')).contains(
               QStringLiteral("moonanime.art/iframe/1")),
           qPrintable(QStringLiteral("the session choice did not lead the embed: %1")
                          .arg(api.requests.join(QLatin1Char('|')))));
  QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("Tortuga"));
}

// The design contract: 40 px actions, 44 px watch CTAs, one 24 px card badge
// line, and the buttons of one action row at one width.
void TestPublicApiUi::actionRowsShareOneControlHeightAndEqualWidths() {
  // The style object dies with the style it is replaced by, so its name is
  // captured rather than the pointer.
  const QString savedStyle = qApp->style() != nullptr ? qApp->style()->objectName() : QString();
  const QString savedSheet = qApp->styleSheet();
  const auto restoreStyle = qScopeGuard([savedStyle, savedSheet] {
    if (!savedStyle.isEmpty()) qApp->setStyle(savedStyle);
    qApp->setStyleSheet(savedSheet);
  });
  qApp->setStyle(QStringLiteral("Fusion"));
  qApp->setStyleSheet(mikai::app::theme::buildStylesheet());
  IsolatedSettings settings;
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Geometry workers did not stop");
  });

  // The title header is one bar: every control sits on the control height and
  // never grows past the track it is laid out in.
  mikai::app::QualityCache cache;
  mikai::app::TitlePage page(runner, client, cache, QStringLiteral("7428"));
  page.resize(1280, 900);
  page.show();
  QTRY_VERIFY_WITH_TIMEOUT(button(page, "titleWatchButton") != nullptr, 20000);
  QTRY_VERIFY_WITH_TIMEOUT(button(page, "titleWatchButton")->isEnabled(), 20000);

  auto* back = button(page, "titleBack");
  auto* home = button(page, "titleHome");
  auto* settingsChip = button(page, "settingsChip");
  auto* header = page.findChild<QWidget*>(QStringLiteral("titleHeader"));
  QVERIFY2(back != nullptr && home != nullptr && settingsChip != nullptr,
           "the title header lost one of its controls");
  QVERIFY2(header != nullptr, "the title header is missing");
  QCOMPARE(header->height(),
           40 + 2 * mikai::app::theme::kSpacingMedium);
  for (QAbstractButton* control : {back, home, settingsChip}) {
    QVERIFY2(control->height() == 40,
             qPrintable(QStringLiteral("%1 is %2 px tall inside a %3 px track")
                            .arg(control->objectName())
                            .arg(control->height())
                            .arg(header->height())));
  }
  QVERIFY2(settingsChip->width() <= 40,
           qPrintable(QStringLiteral("the settings chip is %1x%2 inside a %3 px track")
                          .arg(settingsChip->width())
                          .arg(settingsChip->height())
                          .arg(header->height())));
  QVERIFY2(back->width() == home->width(),
           qPrintable(QStringLiteral("the header pills are %1 / %2 px wide")
                          .arg(back->width())
                          .arg(home->width())));

  // Hero and watch actions: one control height, the documented CTA height, and
  // one width per pair.
  auto* watch = button(page, "titleWatchButton");
  auto* heroWatch = button(page, "titleHeroWatchButton");
  auto* watched = button(page, "titleWatchWatchedToggle");
  QVERIFY(watch != nullptr && heroWatch != nullptr && watched != nullptr);
  QCOMPARE(watch->height(), 44);
  QCOMPARE(heroWatch->height(), 44);
  QCOMPARE(watched->height(), 40);
  QVERIFY2(watch->width() == watched->width(),
           qPrintable(QStringLiteral("the watch pair is %1 / %2 px wide")
                          .arg(watch->width())
                          .arg(watched->width())));

  // The settings action rows hold the same contract at the dialog minimum.
  mikai::app::ComponentStoreTab tab;
  tab.setRunner(&runner);
  tab.setStyleSheet(mikai::app::settingsStylesheet(mikai::app::currentAppearance()));
  tab.resize(mikai::app::theme::kSettingsDialogMinWidth, 900);
  tab.show();
  const QStringList actionNames{QStringLiteral("componentDownloadButton"),
                                QStringLiteral("nativeComponentVerifyButton"),
                                QStringLiteral("nativeComponentTabCancelButton")};
  QList<QPushButton*> settingsActions;
  for (const QString& name : actionNames) {
    auto* control = tab.findChild<QPushButton*>(name);
    QVERIFY2(control != nullptr, qPrintable(QStringLiteral("missing %1").arg(name)));
    settingsActions.append(control);
  }
  QCoreApplication::processEvents();
  for (QPushButton* control : settingsActions) {
    // One control height for the row, taken from the theme token so the row and
    // the token cannot drift apart.
    QCOMPARE(control->height(), mikai::app::theme::kControlHeight);
  }
  for (QPushButton* control : settingsActions) {
    // Caption-sized, never narrower than the text it paints.
    QVERIFY2(control->width() >= control->fontMetrics().horizontalAdvance(control->text()),
             qPrintable(QStringLiteral("%1 clips its caption").arg(control->objectName())));
  }

  // A row is only equal if the width it agreed on is at least what every member
  // paints at: the style rounds its own box model for the device, so a control
  // can ask for a pixel more than the row measured from the font alone, and
  // then paints wider than its equals.
  QList<QAbstractButton*> equalRow{back, home, watch, heroWatch, watched};
  for (QAbstractButton* control : equalRow) {
    QVERIFY2(control->sizeHint().width() <= control->width(),
             qPrintable(QStringLiteral("%1 paints %2 px wide inside its %3 px row")
                            .arg(control->objectName())
                            .arg(control->sizeHint().width())
                            .arg(control->width())));
  }
}

void TestPublicApiUi::wrappedScoreLinesFillTheirTextColumn() {
  const QString savedStyle = qApp->style() != nullptr ? qApp->style()->objectName() : QString();
  const QString savedSheet = qApp->styleSheet();
  const auto restoreStyle = qScopeGuard([savedStyle, savedSheet] {
    if (!savedStyle.isEmpty()) qApp->setStyle(savedStyle);
    qApp->setStyleSheet(savedSheet);
  });
  qApp->setStyle(QStringLiteral("Fusion"));
  qApp->setStyleSheet(mikai::app::theme::buildStylesheet());
  IsolatedSettings settings;
  ApiFixture api;
  QVERIFY(api.start());
  mikai::backend::MikaiClient client(kFixtureKey, api.base());
  mikai::app::Runner runner;
  const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Score workers did not stop");
  });
  mikai::app::QualityCache cache;
  mikai::app::TitlePage page(runner, client, cache, QStringLiteral("7428"));
  page.resize(1280, 900);
  page.show();

  auto* heroScores = page.findChild<QLabel*>(QStringLiteral("titleScores"));
  QVERIFY(heroScores != nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(heroScores->isVisible(), 20000);
  QVERIFY2(heroScores->text().contains(QStringLiteral("MAL")),
           qPrintable(QStringLiteral("the fixture score line is %1").arg(heroScores->text())));
  auto* hero = page.findChild<QWidget*>(QStringLiteral("titleHero"));
  auto* facts = page.findChild<QLabel*>(QStringLiteral("titleFacts"));
  QVERIFY(hero != nullptr && facts != nullptr && facts->isVisible());

  // The details sheet only counts on screen, so its opener must be on screen
  // too: wait for it, then click it like a viewer.
  auto* meta = button(page, "titleDetailsToggle");
  auto* overlay = page.findChild<QWidget*>(QStringLiteral("titleDetailsOverlay"));
  auto* detailsTitle = page.findChild<QLabel*>(QStringLiteral("titleDetailsTitle"));
  QVERIFY(meta != nullptr && overlay != nullptr && detailsTitle != nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(meta->isVisible(), 20000);
  QTest::mouseClick(meta, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(overlay->isVisible(), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(detailsTitle->isVisible(), 5000);
  QLabel* detailsScores = nullptr;
  for (QLabel* label : page.findChildren<QLabel*>(QStringLiteral("titleScores"))) {
    if (label != heroScores) detailsScores = label;
  }
  QVERIFY(detailsScores != nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(detailsScores->isVisible(), 5000);

  const auto lineProblem = [](QLabel* line, QLabel* columnSibling, QWidget* surface) {
    if (!line->isVisible()) return QStringLiteral("not on screen");
    if (line->width() != columnSibling->width()) {
      return QStringLiteral("%1 px beside a %2 px column")
          .arg(line->width())
          .arg(columnSibling->width());
    }
    if (line->height() < line->heightForWidth(line->width())) {
      return QStringLiteral("%1 px tall, %2 px needed at width %3")
          .arg(line->height())
          .arg(line->heightForWidth(line->width()))
          .arg(line->width());
    }
    const QRect bounds(line->mapTo(surface, QPoint(0, 0)), line->size());
    if (!surface->rect().contains(bounds)) {
      return QStringLiteral("overflows its surface: %1,%2 %3x%4")
          .arg(bounds.x())
          .arg(bounds.y())
          .arg(bounds.width())
          .arg(bounds.height());
    }
    return QString();
  };

  for (const int width : {1280, 860}) {
    page.resize(width, 900);
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(heroScores->isVisible(), 5000);
    const QString heroProblem = lineProblem(heroScores, facts, hero);
    QVERIFY2(heroProblem.isEmpty(),
             qPrintable(QStringLiteral("hero score line at %1 px: %2").arg(width).arg(heroProblem)));
    const QString detailsProblem =
        lineProblem(detailsScores, detailsTitle, detailsScores->parentWidget());
    QVERIFY2(detailsProblem.isEmpty(), qPrintable(QStringLiteral(
                                            "details score line at %1 px: %2")
                                            .arg(width)
                                            .arg(detailsProblem)));
  }

  if (primeBuiltCatalogForSwitching()) {
    QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
    const auto restoreEnglish =
        qScopeGuard([] { mikai::app::applyInterfaceLanguage(QStringLiteral("en")); });
    page.resize(860, 900);
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(heroScores->isVisible(), 5000);
    const QString heroProblem = lineProblem(heroScores, facts, hero);
    QVERIFY2(heroProblem.isEmpty(),
             qPrintable(QStringLiteral("Ukrainian hero score line: %1").arg(heroProblem)));
    const QString detailsProblem =
        lineProblem(detailsScores, detailsTitle, detailsScores->parentWidget());
    QVERIFY2(detailsProblem.isEmpty(),
             qPrintable(QStringLiteral("Ukrainian details score line: %1").arg(detailsProblem)));
  }
}

void TestPublicApiUi::componentCardsKeepASingleLineBadgeAndASeparateCountLine() {
  const QString savedStyle = qApp->style() != nullptr ? qApp->style()->objectName() : QString();
  const QString savedSheet = qApp->styleSheet();
  const auto restoreStyle = qScopeGuard([savedStyle, savedSheet] {
    if (!savedStyle.isEmpty()) qApp->setStyle(savedStyle);
    qApp->setStyleSheet(savedSheet);
  });
  qApp->setStyle(QStringLiteral("Fusion"));
  qApp->setStyleSheet(mikai::app::theme::buildStylesheet());
  IsolatedSettings settings;
  mikai::app::Runner runner;
  const auto drain = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Component workers did not stop");
  });
  mikai::app::ComponentStoreTab tab;
  tab.setRunner(&runner);
  tab.setStyleSheet(mikai::app::settingsStylesheet(mikai::app::currentAppearance()));
  tab.resize(860, 900);
  tab.show();
  QCoreApplication::processEvents();

  const QStringList cardIds{QStringLiteral("tensorRtRuntimeCard"),
                            QStringLiteral("animeJanaiModelsCard"),
                            QStringLiteral("rifeModelsCard"),
                            QStringLiteral("tensorRtKernelsCard"),
                            QStringLiteral("anime4kShadersCard"),
                            QStringLiteral("upscalerModelsCard")};
  for (const QString& cardId : cardIds) {
    auto* badge = tab.findChild<QLabel*>(cardId + QStringLiteral("State"));
    auto* title = tab.findChild<QLabel*>(cardId + QStringLiteral("Title"));
    auto* description = tab.findChild<QLabel*>(cardId + QStringLiteral("Description"));
    auto* counts = tab.findChild<QLabel*>(cardId + QStringLiteral("Count"));
    if (cardId == QStringLiteral("upscalerModelsCard")) {
      // The card is the download entry point for these graphs: a disabled or
      // hidden box is a row a viewer cannot act on, which is what it used to be.
      auto* box = tab.findChild<QCheckBox*>(cardId + QStringLiteral("Check"));
      QVERIFY2(box != nullptr, "the upscaler card has no selection box");
      QVERIFY2(box->isEnabled(), "the upscaler card cannot be selected");
      QVERIFY2(!box->isHidden(), "the upscaler card selection is hidden");
      QVERIFY2(description->text().contains(QStringLiteral("Install selected")),
               qPrintable(description->text()));
    }
    QVERIFY2(badge != nullptr && title != nullptr && description != nullptr,
             qPrintable(QStringLiteral("%1 is missing a header element").arg(cardId)));
    QVERIFY2(counts != nullptr, qPrintable(QStringLiteral("%1 has no count line").arg(cardId)));
    QTRY_VERIFY_WITH_TIMEOUT(badge->isVisible() && !badge->text().isEmpty(), 10000);
    QVERIFY2(badge->height() <= 24,
             qPrintable(QStringLiteral("%1 badge is %2 px tall: %3")
                            .arg(cardId)
                            .arg(badge->height())
                            .arg(badge->text())));
    QVERIFY2(badge->width() <= 120,
             qPrintable(QStringLiteral("%1 badge is %2 px wide")
                            .arg(cardId)
                            .arg(badge->width())));
    const QRect badgeRect(badge->mapTo(&tab, QPoint(0, 0)), badge->size());
    const QRect titleRect(title->mapTo(&tab, QPoint(0, 0)), title->size());
    QVERIFY2(!badgeRect.intersects(titleRect),
             qPrintable(QStringLiteral("%1 badge covers its title").arg(cardId)));
  }

  // The spatial card's counts sit on their own line under its description.
  auto* spatialCounts = tab.findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardCount"));
  auto* spatialDescription =
      tab.findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardDescription"));
  QVERIFY(spatialCounts != nullptr && spatialDescription != nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(spatialCounts->isVisible(), 10000);
  QVERIFY2(spatialCounts->text().contains(QLatin1Char('/')),
           qPrintable(QStringLiteral("the spatial count line is %1").arg(spatialCounts->text())));
  QVERIFY2(spatialCounts->geometry().top() >= spatialDescription->geometry().bottom(),
           "the spatial counts share the description line");

  // A heading is a label, so no action-row check covered it: at the wide width
  // it shows its whole first line, and at the components viewport of the 580 px
  // dialog it still shows its widest word and every wrapped line, inside its
  // card, with the badge moved to its own row beneath.
  for (const int width : {860, 312}) {
    tab.resize(width, 900);
    // The reflow follows each card's own width, so the layout has to settle
    // before the heading is measured; a single event round can leave the last
    // card at its previous width.
    settleLayout(&tab);
    QCoreApplication::processEvents();
    for (const QString& cardId : cardIds) {
      auto* title = tab.findChild<QLabel*>(cardId + QStringLiteral("Title"));
      auto* badge = tab.findChild<QLabel*>(cardId + QStringLiteral("State"));
      QVERIFY2(title != nullptr && badge != nullptr,
               qPrintable(QStringLiteral("%1 is missing a heading").arg(cardId)));
      QVERIFY2(title->width() >= title->minimumSizeHint().width(),
               qPrintable(QStringLiteral("%1 heading is %2 px wide, its widest word needs %3")
                              .arg(cardId)
                              .arg(title->width())
                              .arg(title->minimumSizeHint().width())));
      QVERIFY2(title->height() >= title->heightForWidth(title->width()),
               qPrintable(QStringLiteral("%1 heading is %2 px tall, %3 px needed at width %4")
                              .arg(cardId)
                              .arg(title->height())
                              .arg(title->heightForWidth(title->width()))
                              .arg(title->width())));
      QWidget* cardHost = title->parentWidget() != nullptr
                              ? title->parentWidget()->parentWidget()
                              : nullptr;
      QVERIFY2(cardHost != nullptr, qPrintable(QStringLiteral("%1 has no card").arg(cardId)));
      const QRect titleInCard(title->mapTo(cardHost, QPoint(0, 0)), title->size());
      QVERIFY2(cardHost->rect().contains(titleInCard),
               qPrintable(QStringLiteral("%1 heading leaves its card: %2,%3 %4x%5 in %6x%7")
                              .arg(cardId)
                              .arg(titleInCard.x())
                              .arg(titleInCard.y())
                              .arg(titleInCard.width())
                              .arg(titleInCard.height())
                              .arg(cardHost->width())
                              .arg(cardHost->height())));
      // Either arrangement is acceptable - the header may keep the state on the
      // heading's line - as long as the state never covers the heading and
      // stays inside the card.
      const QRect badgeInCard(badge->mapTo(cardHost, QPoint(0, 0)), badge->size());
      QVERIFY2(!badgeInCard.intersects(titleInCard),
               qPrintable(QStringLiteral("%1 state covers the heading at %2 px")
                              .arg(cardId)
                              .arg(width)));
      QVERIFY2(cardHost->rect().contains(badgeInCard),
               qPrintable(QStringLiteral("%1 state leaves its card at %2 px")
                              .arg(cardId)
                              .arg(width)));
    }
  }
}

void TestPublicApiUi::theUpscalersPageListsEveryPinnedModel() {
  mikai::backend::MikaiClient client(QString(), QStringLiteral("http://127.0.0.1:1"));
  mikai::app::Runner runner;
  const auto drain = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Upscaler workers did not stop");
  });
  mikai::app::SettingsDialog dialog(client, runner);
  dialog.resize(dialog.minimumSize());
  dialog.show();
  QCoreApplication::processEvents();
  QVERIFY(dialog.showPage(QStringLiteral("upscalers")));
  QCoreApplication::processEvents();

  auto* panel = dialog.findChild<mikai::app::UpscalersPanel*>();
  QVERIFY2(panel != nullptr, "the upscalers page is missing");
  QCOMPARE(panel->rowIds().size(), mplayer::native::upscalerCatalog().size());
  QVERIFY(!panel->busy());
  for (const mplayer::native::UpscalerModel& model : mplayer::native::upscalerCatalog()) {
    auto* state = dialog.findChild<QLabel*>(QStringLiteral("upscalerState_%1").arg(model.fileName));
    auto* action =
        dialog.findChild<QPushButton*>(QStringLiteral("upscalerAction_%1").arg(model.fileName));
    QVERIFY2(state != nullptr, qPrintable(model.fileName));
    QVERIFY2(action != nullptr, qPrintable(model.fileName));
    // A placeholder is a state, not an empty row: the action says what can be
    // done now, and never reports a download that already happened.
    const bool installed = !mplayer::native::installedUpscalerPath(model).isEmpty();
    QCOMPARE(state->text(), installed ? QStringLiteral("installed")
                                      : QStringLiteral("not installed"));
    QCOMPARE(action->text(), installed ? QStringLiteral("Remove") : QStringLiteral("Download"));
  }
}

void TestPublicApiUi::settingsPagesFitTheDialogMinimum() {
  const QString savedStyle = qApp->style() != nullptr ? qApp->style()->objectName() : QString();
  const QString savedSheet = qApp->styleSheet();
  const auto restoreStyle = qScopeGuard([savedStyle, savedSheet] {
    if (!savedStyle.isEmpty()) qApp->setStyle(savedStyle);
    qApp->setStyleSheet(savedSheet);
  });
  qApp->setStyle(QStringLiteral("Fusion"));
  qApp->setStyleSheet(mikai::app::theme::buildStylesheet());
  IsolatedSettings settings;
  mikai::backend::MikaiClient client(QString(), QStringLiteral("http://127.0.0.1:1"));
  mikai::app::Runner runner;
  const auto drain = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Settings workers did not stop");
  });
#ifdef Q_OS_WIN
  QVERIFY2(QFontDatabase::families().contains(QStringLiteral("Segoe UI")),
           "the offscreen run loaded no platform fonts; QT_QPA_FONTDIR must name the "
           "system font directory");
#endif

  mikai::app::SettingsDialog dialog(client, runner);
  dialog.resize(dialog.minimumSize());
  dialog.show();
  QCoreApplication::processEvents();

  auto* stack = dialog.findChild<QStackedWidget*>(QStringLiteral("settingsStack"));
  QVERIFY2(stack != nullptr, "the settings stack is missing");

  const QStringList pages{QStringLiteral("customization/general"), QStringLiteral("components"),
                          QStringLiteral("benchmark")};
  const auto auditPages = [&](const QString& language) {
    for (const QString& id : pages) {
      QVERIFY2(dialog.showPage(id), qPrintable(QStringLiteral("%1 is not a page").arg(id)));
      QCoreApplication::processEvents();
      QWidget* host = stack->currentWidget();
      QVERIFY2(host != nullptr, qPrintable(QStringLiteral("%1 has no host").arg(id)));
      settleLayout(host);
      const QString overflow = horizontalOverflow(host);
      QVERIFY2(overflow.isEmpty(),
               qPrintable(QStringLiteral("%1 in %2: %3").arg(id, language, overflow)));
    }
  };
  auditPages(QStringLiteral("en"));

  // The five component actions used to be one equal width wider than the page
  // they live on, so the row could only be read by panning sideways.
  QVERIFY(dialog.showPage(QStringLiteral("components")));
  QCoreApplication::processEvents();
  auto* componentsScroll = dialog.findChild<QScrollArea*>(QStringLiteral("componentsScroll"));
  QVERIFY2(componentsScroll != nullptr, "the components page exposes no scroll");
  if (QWidget* page = componentsScroll->widget()) settleLayout(page);
  for (const QString& name : {QStringLiteral("componentDownloadButton"),
                              QStringLiteral("nativeComponentVerifyButton"),
                              QStringLiteral("nativeComponentTabCancelButton")}) {
    auto* control = dialog.findChild<QPushButton*>(name);
    QVERIFY2(control != nullptr, qPrintable(QStringLiteral("%1 is missing").arg(name)));
    QVERIFY2(control->width() <= componentsScroll->viewport()->width(),
             qPrintable(QStringLiteral("%1 is %2 px wide inside a %3 px viewport")
                            .arg(name)
                            .arg(control->width())
                            .arg(componentsScroll->viewport()->width())));
  }

  // A whole-sentence check box caption only fits when it is allowed to wrap:
  // the box must then stand as tall as its own wrapped caption needs.
  QVERIFY(dialog.showPage(QStringLiteral("customization/general")));
  QCoreApplication::processEvents();
  auto* general = dialog.findChild<QWidget*>(QStringLiteral("customizationGeneralPage"));
  QVERIFY2(general != nullptr, "the general page is missing");
  settleLayout(general);
  for (const QString& name : {QStringLiteral("systemStartupCheck"),
                              QStringLiteral("systemContextMenuCheck")}) {
    auto* check = dialog.findChild<QWidget*>(name);
    QVERIFY2(check != nullptr, qPrintable(QStringLiteral("%1 is missing").arg(name)));
    QVERIFY2(check->height() >= check->heightForWidth(check->width()),
             qPrintable(QStringLiteral("%1 is %2 px tall, %3 px needed at width %4")
                            .arg(name)
                            .arg(check->height())
                            .arg(check->heightForWidth(check->width()))
                            .arg(check->width())));
  }

  if (!primeBuiltCatalogForSwitching()) return;
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  const auto restoreEnglish =
      qScopeGuard([] { mikai::app::applyInterfaceLanguage(QStringLiteral("en")); });
  QCoreApplication::processEvents();
  auditPages(QStringLiteral("uk"));
}

void TestPublicApiUi::managerActionRowsShareOneWidth() {
  const QString savedSheet = qApp->styleSheet();
  const auto restoreStyle = qScopeGuard([savedSheet] { qApp->setStyleSheet(savedSheet); });
  IsolatedSettings settings;
  mikai::app::Runner runner;
  const auto drain = qScopeGuard([&runner] {
    runner.cancel();
    if (!runner.waitForDone(10000)) qFatal("Manager workers did not stop");
  });
  mikai::app::AnimeJanaiManagerPanel panel(nullptr, &runner);
  panel.setStyleSheet(mikai::app::settingsStylesheet(mikai::app::currentAppearance()));
  panel.resize(320, 1200);
  panel.show();
  QCoreApplication::processEvents();

  const QList<QStringList> rows{
      {QStringLiteral("ajiImportModelButton"), QStringLiteral("ajiRemoveModelButton"),
       QStringLiteral("ajiOpenModelsFolderButton")},
      {QStringLiteral("ajiBrowseExternalDirButton"), QStringLiteral("ajiClearExternalDirButton")},
      {QStringLiteral("ajiImportConfButton"), QStringLiteral("ajiExportConfButton"),
       QStringLiteral("ajiResetDefaultsButton")},
      {QStringLiteral("ajiCreateSlotButton"), QStringLiteral("ajiCloneSlotButton"),
       QStringLiteral("ajiDeleteSlotButton"), QStringLiteral("ajiImportPconfButton"),
       QStringLiteral("ajiExportPconfButton")},
  };
  // The active-chain row lives in the slot editor, which stays hidden until a
  // saved slot exists; the native probe measures it on a configured profile.
  for (const QStringList& row : rows) {
    int width = -1;
    for (const QString& name : row) {
      auto* control = panel.findChild<QPushButton*>(name);
      QVERIFY2(control != nullptr, qPrintable(QStringLiteral("%1 is missing").arg(name)));
      QCOMPARE(control->height(), mikai::app::theme::kControlHeight);
      if (width < 0) width = control->width();
      QVERIFY2(control->width() == width,
               qPrintable(QStringLiteral("%1 is %2 px wide beside %3 px in its row")
                              .arg(name)
                              .arg(control->width())
                              .arg(width)));
    }
  }
}

void TestPublicApiUi::quickResumeIsOfferedOncePerEpisodeToken() {
  const QString fixture = QDir::toNativeSeparators(
      QStringLiteral(MIKAI_SOURCE_DIR "/resources/benchmarks/1920x1080.mp4"));
  QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));

  QuickPlayerHost host;
  mplayer::quick::PlayerController controller(
      QStringLiteral("Resume fixture"), fixture, {}, 0, &host);
  controller.setPlaylist({QVariantMap{{QStringLiteral("label"), QStringLiteral("Episode 1")}}});
  controller.setEpisodeResumePoint(3.0);
  controller.showWindow();
  bool insideOpen = false;
  bool resumeRaisedInsideOpen = false;
  connect(&controller, &mplayer::quick::PlayerController::resumeSheetChanged, &controller,
          [&controller, &insideOpen, &resumeRaisedInsideOpen] {
            if (insideOpen && controller.resumeSheetVisible()) resumeRaisedInsideOpen = true;
          });
  mplayer::VideoSession* session = controller.session();
  QVERIFY(session != nullptr);
  QSignalSpy loaded(session, &mplayer::VideoSession::mediaLoaded);
  insideOpen = true;
  controller.startPlayer();
  insideOpen = false;
  QVERIFY2(!resumeRaisedInsideOpen, "the resume prompt was raised synchronously inside open()");
  QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
  QTRY_VERIFY_WITH_TIMEOUT(session->duration() > 10.0, 30000);
  QTRY_VERIFY_WITH_TIMEOUT(controller.resumeSheetVisible(), 10000);
  const quint64 generation = controller.resumeSheetGeneration();
  const uint64_t firstEpoch = session->sourceEpoch();
  const quint64 mediaToken = loaded.constFirst().constFirst().toULongLong();

  controller.answerResume(generation, mplayer::quick::PlayerController::ResumeChoice::StartOver);
  QVERIFY(!controller.resumeSheetVisible());
  controller.setEpisodeResumePoint(3.0);
  controller.startPlayer();
  QTRY_VERIFY_WITH_TIMEOUT(session->sourceEpoch() != firstEpoch, 10000);
  QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 30000);
  QCOMPARE(loaded.constLast().constFirst().toULongLong(), mediaToken);
  QTRY_VERIFY_WITH_TIMEOUT(session->duration() > 10.0, 30000);
  QTest::qWait(300);
  QVERIFY2(!controller.resumeSheetVisible(),
           "the same episode token raised another resume sheet after reopening");
  QCOMPARE(controller.resumeSheetGeneration(), generation);
}

void TestPublicApiUi::staleQuickResumeAnswerCannotAffectTheNextEpisode() {
  const QString fixture = QDir::toNativeSeparators(
      QStringLiteral(MIKAI_SOURCE_DIR "/resources/benchmarks/1920x1080.mp4"));
  QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));

  QuickPlayerHost host;
  mplayer::quick::PlayerController controller(
      QStringLiteral("Resume fixture"), fixture, {}, 0, &host);
  const QVector<QVariantMap> playlist{
      QVariantMap{{QStringLiteral("label"), QStringLiteral("Episode 1")}},
      QVariantMap{{QStringLiteral("label"), QStringLiteral("Episode 2")}}};
  controller.setPlaylist(playlist);
  controller.setEpisodeResumePoint(3.0);
  controller.showWindow();
  mplayer::VideoSession* session = controller.session();
  QVERIFY(session != nullptr);
  QSignalSpy loaded(session, &mplayer::VideoSession::mediaLoaded);
  controller.startPlayer();
  QTRY_VERIFY_WITH_TIMEOUT(controller.resumeSheetVisible(), 30000);
  QCOMPARE(loaded.count(), 1);
  const quint64 oldGeneration = controller.resumeSheetGeneration();
  const quint64 firstToken = loaded.constFirst().constFirst().toULongLong();

  controller.applyEpisode(QStringLiteral("Episode 2"), fixture, 1,
                          {mplayer::shell::QualityEntry{0, fixture}}, {});
  QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 30000);
  const quint64 nextToken = loaded.constLast().constFirst().toULongLong();
  QVERIFY2(firstToken != 0 && nextToken != 0 && firstToken != nextToken,
           "different episode loads must carry different media identity tokens");
  QTRY_VERIFY_WITH_TIMEOUT(session->duration() > 10.0, 10000);
  session->setPaused(true);
  session->seek(8.0);
  QTRY_VERIFY_WITH_TIMEOUT(qAbs(session->position() - 8.0) < 0.75, 10000);
  const double nextPosition = session->position();
  const bool nextPauseIntent = session->playbackPauseIntent();

  controller.answerResume(oldGeneration, mplayer::quick::PlayerController::ResumeChoice::Continue);
  QCoreApplication::processEvents();
  QVERIFY(!controller.resumeSheetVisible());
  QVERIFY2(qAbs(session->position() - nextPosition) < 0.75,
           "the prior episode's answer sought the newly loaded episode");
  QCOMPARE(session->playbackPauseIntent(), nextPauseIntent);
}


void TestPublicApiUi::openSubtitlesApiKeyMigratesOutOfQSettings() {
#ifndef Q_OS_WIN
  // P0-03: the key lives in a 0600 file under AppDataLocation, never in
  // plaintext QSettings. XDG_DATA_HOME redirects AppDataLocation at a temp
  // dir so the test never touches the real app data.
  // Qt test mode (set for the whole test binary) makes AppDataLocation
  // ignore XDG_DATA_HOME and resolve under ~/.qttest instead, so the mode
  // is switched off for this test; XDG_CONFIG_HOME is redirected too, so
  // the QSettings side of the migration stays inside the temp dir as well.
  QTemporaryDir dataHome;
  QVERIFY(dataHome.isValid());
  struct RestoreEnv {
    const char* name;
    bool present;
    QByteArray value;
    ~RestoreEnv() {
      if (present) {
        qputenv(name, value);
      } else {
        qunsetenv(name);
      }
    }
  };
  const bool hadData = qEnvironmentVariableIsSet("XDG_DATA_HOME");
  const QByteArray previousData = qgetenv("XDG_DATA_HOME");
  RestoreEnv restoreData{"XDG_DATA_HOME", hadData, previousData};
  const bool hadConfig = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
  const QByteArray previousConfig = qgetenv("XDG_CONFIG_HOME");
  RestoreEnv restoreConfig{"XDG_CONFIG_HOME", hadConfig, previousConfig};
  struct RestoreTestMode {
    bool wasEnabled;
    ~RestoreTestMode() { QStandardPaths::setTestModeEnabled(wasEnabled); }
  } restoreTestMode{QStandardPaths::isTestModeEnabled()};
  QStandardPaths::setTestModeEnabled(false);
  QVERIFY(qputenv("XDG_DATA_HOME", dataHome.path().toUtf8()));
  QVERIFY(qputenv("XDG_CONFIG_HOME",
                  (dataHome.path() + QStringLiteral("/config")).toUtf8()));
  // The store must resolve inside the redirected home before anything is
  // written, or the test would touch the real app data dir.
  const QString path = mikai::gui::openSubtitlesApiKeyFilePath();
  QVERIFY2(path.startsWith(dataHome.path()),
           qPrintable(QStringLiteral("credential store escaped the redirected home: %1")
                          .arg(path)));

  QSettings settings;
  settings.setValue(QStringLiteral("access/openSubtitlesApiKey"),
                    QStringLiteral("mk_test_key_synthetic"));
  QCOMPARE(mikai::gui::loadOpenSubtitlesApiKey(), QStringLiteral("mk_test_key_synthetic"));
  QVERIFY2(!settings.contains(QStringLiteral("access/openSubtitlesApiKey")),
           "migration left the plaintext QSettings key behind");
  QVERIFY2(QFileInfo::exists(path), "migration wrote no credential file");
  const QFile::Permissions permissions = QFile::permissions(path);
  QVERIFY(permissions.testFlag(QFile::ReadOwner));
  QVERIFY(permissions.testFlag(QFile::WriteOwner));
  QVERIFY(!(permissions & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                           QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)));

  // Round-trip through the new store, then clearing removes the file.
  QVERIFY(mikai::gui::saveOpenSubtitlesApiKey(QStringLiteral("mk_test_key_2_synthetic")));
  QCOMPARE(mikai::gui::loadOpenSubtitlesApiKey(),
           QStringLiteral("mk_test_key_2_synthetic"));
  QVERIFY(mikai::gui::saveOpenSubtitlesApiKey(QString()));
  QVERIFY(!QFileInfo::exists(path));
  QCOMPARE(mikai::gui::loadOpenSubtitlesApiKey(), QString());
#else
  QSKIP("the 0600-file fallback is the non-Windows store; Windows uses DPAPI");
#endif
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
