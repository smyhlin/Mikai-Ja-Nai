#include "TestI18n.hpp"


#include <app/dialogs/SettingsDialog.hpp>
#include <app/dialogs/AnimeJanaiManagerPanel.hpp>
#include <app/support/Labels.hpp>
#include <app/state/AppearanceStore.hpp>
#include <app/theme/Metrics.hpp>
#include <app/support/I18n.hpp>
#include <app/workers/Runner.hpp>
#include <backend/Cancellation.hpp>
#include <backend/MikaiClient.hpp>
#include <native/bundle/NativePrefs.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>
#include <native/components/AnimeJanaiRuntime.hpp>
#include <native/prepare/NativePreparationContext.hpp>
#include <player/shell/PlayerPopups.hpp>
#include <player/shell/PlayerShell.hpp>
#include <player/shell/ShellTheme.hpp>
#include <player/shell/ShellWidgets.hpp>
#include <widgets/FailureView.hpp>
#include <widgets/RangePager.hpp>
#include <widgets/RateLimitLabel.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QPoint>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtGui/QColor>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QLocale>
#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>
#include <QtCore/QSize>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <atomic>
#include <optional>

namespace {

// The real lrelease output, never a stand-in: CMake defines its exact path and
// the same file also lands beside the test executable.
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

QString missingCatalogPath() {
  return QDir(QCoreApplication::applicationDirPath())
      .absoluteFilePath(QStringLiteral("mikai_absent_catalog_for_test.qm"));
}

// Load the real Ukrainian catalog and return to English, so the manager retains
// an explicit catalog for the rest of the test. Every live-widget test primes
// itself this way and therefore passes alone, in any order.
bool primeBuiltCatalogForSwitching() {
  const QString catalog = builtCatalogPath();
  if (!QFileInfo::exists(catalog)) {
    return false;
  }
  return mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog) &&
         mikai::app::applyInterfaceLanguage(QStringLiteral("en"));
}

// The four paging arrows carry glyph labels; every other button on the pager is
// a page chip. Reading them this way keeps the test off the widget's internals
// while still asserting the labels and names a user is given.
bool isPagerArrow(const QString& text) {
  return text == QStringLiteral("⏮") || text == QStringLiteral("←") ||
         text == QStringLiteral("→") || text == QStringLiteral("⏭");
}

QPushButton* pagerArrow(const QWidget& pager, const QString& glyph) {
  for (QPushButton* button : pager.findChildren<QPushButton*>()) {
    if (button->text() == glyph) {
      return button;
    }
  }
  return nullptr;
}

QList<QPushButton*> pagerChips(const QWidget& pager) {
  QList<QPushButton*> chips;
  for (QPushButton* button : pager.findChildren<QPushButton*>()) {
    if (!isPagerArrow(button->text())) {
      chips << button;
    }
  }
  return chips;
}

QStringList pagerChipLabels(const QWidget& pager) {
  QStringList labels;
  for (QPushButton* chip : pagerChips(pager)) {
    labels << chip->text();
  }
  return labels;
}

QLabel* labelContaining(const QWidget& widget, const QString& needle) {
  for (QLabel* label : widget.findChildren<QLabel*>()) {
    if (label->text().contains(needle)) {
      return label;
    }
  }
  return nullptr;
}

/// A loopback endpoint that answers every request with an empty successful
/// schedule and the quota headers the test hands it. The quota a label renders
/// therefore comes from a real HTTP answer read by MikaiClient, not from a
/// setter the production widget does not have.
class QuotaLoopback : public QObject {
 public:
  explicit QuotaLoopback(QObject* parent = nullptr) : QObject(parent) {}
  ~QuotaLoopback() override {
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
    connect(&server_, &QTcpServer::newConnection, this, &QuotaLoopback::onNewConnection);
    return true;
  }

  QString baseUrl() const {
    return QStringLiteral("http://127.0.0.1:%1/public/v1").arg(server_.serverPort());
  }

  void setQuota(int limit, int remaining, int reset) {
    QMutexLocker locker(&mutex_);
    limit_ = limit;
    remaining_ = remaining;
    reset_ = reset;
  }

  int requestCount() const {
    QMutexLocker locker(&mutex_);
    return requests_;
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
    // One readyRead can carry several pipelined requests, so every complete
    // head is answered in turn and a response never eats the next request.
    buffers_[socket].append(socket->readAll());
    for (;;) {
      QByteArray& buffered = buffers_[socket];
      const int end = buffered.indexOf("\r\n\r\n");
      if (end < 0) return;
      buffered = buffered.mid(end + 4);

      int limit = 0;
      int remaining = 0;
      int reset = 0;
      {
        QMutexLocker locker(&mutex_);
        ++requests_;
        limit = limit_;
        remaining = remaining_;
        reset = reset_;
      }

      const QByteArray body = QByteArrayLiteral("{\"ok\":true,\"result\":{}}");
      QByteArray head = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n");
      head += QByteArrayLiteral("X-RateLimit-Limit: ") + QByteArray::number(limit) + "\r\n";
      head +=
          QByteArrayLiteral("X-RateLimit-Remaining: ") + QByteArray::number(remaining) + "\r\n";
      head += QByteArrayLiteral("X-RateLimit-Reset: ") + QByteArray::number(reset) + "\r\n";
      head += QByteArrayLiteral("Content-Length: ") + QByteArray::number(body.size()) +
              QByteArrayLiteral("\r\nConnection: keep-alive\r\n\r\n");
      socket->write(head);
      socket->write(body);
      socket->flush();
    }
  }

  QTcpServer server_;
  QList<QTcpSocket*> sockets_;
  QHash<QTcpSocket*, QByteArray> buffers_;
  mutable QMutex mutex_;
  int limit_ = 0;
  int remaining_ = 0;
  int reset_ = 0;
  int requests_ = 0;
};
}  // namespace

void TestI18n::explicitPreferenceOverridesSystemLocale() {
  QCOMPARE(mikai::app::resolveLanguageCode(QStringLiteral("en"), QLocale::Ukrainian),
           QStringLiteral("en"));
  QCOMPARE(mikai::app::resolveLanguageCode(QStringLiteral("uk"), QLocale::English),
           QStringLiteral("uk"));
}

void TestI18n::systemPreferenceUsesUkrainianOnlyForUkrainianLocale() {
  QCOMPARE(mikai::app::resolveLanguageCode(QStringLiteral("system"), QLocale::Ukrainian),
           QStringLiteral("uk"));
  QCOMPARE(mikai::app::resolveLanguageCode(QStringLiteral("system"), QLocale::English),
           QStringLiteral("en"));
  QCOMPARE(mikai::app::resolveLanguageCode(QStringLiteral("system"), QLocale::German),
           QStringLiteral("en"));
}

void TestI18n::invalidPreferenceUsesSystemLocale() {
  QCOMPARE(mikai::app::resolveLanguageCode(QStringLiteral("xx"), QLocale::Ukrainian),
           QStringLiteral("uk"));
}

void TestI18n::realCatalogSwitchesLanguageRepeatedly() {
  const QString catalog = builtCatalogPath();
  QVERIFY2(QFileInfo::exists(catalog),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + catalog));

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog));
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("uk"));
  QCOMPARE(QLocale().language(), QLocale::Ukrainian);

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));
  QCOMPARE(QLocale().language(), QLocale::English);

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("uk"));
  QCOMPARE(QLocale().language(), QLocale::Ukrainian);

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));
}

void TestI18n::liveStringsFollowTheSwitchedLanguageAndRestore() {
  const QString catalog = builtCatalogPath();
  QVERIFY2(QFileInfo::exists(catalog),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + catalog));

  constexpr const char* kCodes[] = {"not_found", "invalid_api_key"};
  const auto readAll = [&]() {
    QStringList messages;
    for (const char* code : kCodes) {
      messages << mikai::app::localizedApiError(QString::fromLatin1(code));
    }
    return messages;
  };

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog));
  const QStringList ukrainian = readAll();
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  const QStringList english = readAll();

  for (int i = 0; i < 2; ++i) {
    QVERIFY2(ukrainian.at(i) != english.at(i),
             "switching language changed nothing the running application renders");
  }
  QVERIFY2(ukrainian.at(0) != ukrainian.at(1),
           "one wording came back for two different messages");

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  QCOMPARE(readAll(), ukrainian);
}

void TestI18n::contentNamesFollowTheirOwnLanguage() {
  // The interface stays English throughout: only the content vocabulary moves.
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  mikai::app::theme::Appearance saved = mikai::app::currentAppearance();
  const auto restore = qScopeGuard([saved] { mikai::app::setCurrentAppearance(saved); });
  mikai::app::theme::Appearance appearance = saved;

  appearance.nameLanguage = mikai::app::theme::NameLanguage::Interface;
  mikai::app::setCurrentAppearance(appearance);
  QCOMPARE(mikai::app::labels::status(QStringLiteral("finished")),
           QStringLiteral("Finished"));

  appearance.nameLanguage = mikai::app::theme::NameLanguage::Ukrainian;
  mikai::app::setCurrentAppearance(appearance);
  QCOMPARE(mikai::app::labels::status(QStringLiteral("finished")),
           QStringLiteral("Завершено"));
  QCOMPARE(mikai::app::labels::ageRating(QStringLiteral("r18")), QStringLiteral("18+"));

  appearance.nameLanguage = mikai::app::theme::NameLanguage::English;
  mikai::app::setCurrentAppearance(appearance);
  QCOMPARE(mikai::app::labels::status(QStringLiteral("finished")),
           QStringLiteral("Finished"));

  // An unknown token is returned as it arrived, in every language.
  QCOMPARE(mikai::app::labels::status(QStringLiteral("brand-new")),
           QStringLiteral("brand-new"));
}

void TestI18n::appearanceRoundTripsThroughSettings() {
  QTemporaryDir settingsDir;
  QVERIFY(settingsDir.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
  const mikai::app::theme::Appearance saved = mikai::app::currentAppearance();
  const auto restore = qScopeGuard([saved] { mikai::app::setCurrentAppearance(saved); });

  mikai::app::theme::Appearance wanted;
  wanted.siteTheme = mikai::app::theme::SiteTheme::Light;
  wanted.titleLanguage = mikai::app::theme::TitleLanguage::English;
  wanted.nameLanguage = mikai::app::theme::NameLanguage::Original;
  wanted.coverGradient = false;
  wanted.showAdult = true;
  wanted.accent = QStringLiteral("#123456");
  wanted.radiusStep = 1;
  wanted.backgroundGradient = false;
  wanted.gradientIntensity = 75;
  wanted.gradientHeight = 40;
  wanted.gradientColor = QStringLiteral("#abcdef");
  mikai::app::saveAppearance(wanted);

  const mikai::app::theme::Appearance reloaded = mikai::app::loadAppearance();
  QCOMPARE(reloaded.siteTheme, wanted.siteTheme);
  QCOMPARE(reloaded.titleLanguage, wanted.titleLanguage);
  QCOMPARE(reloaded.nameLanguage, wanted.nameLanguage);
  QCOMPARE(reloaded.coverGradient, wanted.coverGradient);
  QCOMPARE(reloaded.showAdult, wanted.showAdult);
  QCOMPARE(mikai::app::theme::accentColor(reloaded), QColor(wanted.accent));
  QCOMPARE(reloaded.radiusStep, wanted.radiusStep);
  QCOMPARE(reloaded.backgroundGradient, wanted.backgroundGradient);
  QCOMPARE(reloaded.gradientIntensity, wanted.gradientIntensity);
  QCOMPARE(reloaded.gradientHeight, wanted.gradientHeight);
  QCOMPARE(mikai::app::theme::gradientColor(reloaded), QColor(wanted.gradientColor));
}

void TestI18n::unsetRadiusAndResetUseXsWithoutOverwritingSavedChoices() {
  QSettings settings;
  QVariantMap prior;
  for (const QString& key : settings.allKeys()) prior.insert(key, settings.value(key));
  const auto savedLook = mikai::app::currentAppearance();
  const auto savedPalette = qApp->palette();
  const QString savedSheet = qApp->styleSheet();
  const auto savedAccent = mplayer::shell::accent();
  const auto restore = qScopeGuard([&] {
    settings.clear();
    for (auto it = prior.cbegin(); it != prior.cend(); ++it) settings.setValue(it.key(), it.value());
    settings.sync();
    mikai::app::setCurrentAppearance(savedLook);
    qApp->setPalette(savedPalette);
    qApp->setStyleSheet(savedSheet);
    mplayer::shell::setAccent(savedAccent);
  });

  settings.remove(QStringLiteral("customization/radiusStep"));
  settings.sync();
  const auto defaults = mikai::app::loadAppearance();
  QCOMPARE(mikai::app::theme::radiusNames().at(defaults.radiusStep), QStringLiteral("XS"));
  QCOMPARE(mikai::app::theme::radiusSmall(defaults), 4);

  settings.setValue(QStringLiteral("customization/radiusStep"), 4);
  settings.sync();
  const auto opening = mikai::app::loadAppearance();
  QCOMPARE(opening.radiusStep, 4);
  mikai::app::setCurrentAppearance(opening);

  mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
  mikai::app::Runner runner;
  mikai::app::SettingsDialog dialog(client, runner);
  dialog.show();
  QVERIFY(dialog.showPage(QStringLiteral("customization/appearance")));
  auto* reset = dialog.findChild<QPushButton*>(QStringLiteral("resetAppearanceButton"));
  QVERIFY(reset != nullptr);
  reset->click();
  QCOMPARE(mikai::app::theme::radiusSmall(mikai::app::currentAppearance()), 4);
  QCOMPARE(mikai::app::loadAppearance().radiusStep, 4);
  dialog.reject();
  QCOMPARE(mikai::app::currentAppearance().radiusStep, 4);

  dialog.show();
  QVERIFY(dialog.showPage(QStringLiteral("customization/appearance")));
  reset->click();
  auto* footer = dialog.findChild<QWidget*>(QStringLiteral("settingsFooter"));
  QVERIFY(footer != nullptr);
  auto* save = footer->findChild<QPushButton*>(QStringLiteral("primary"));
  QVERIFY(save != nullptr);
  save->click();
  QCOMPARE(dialog.result(), int(QDialog::Accepted));
  QCOMPARE(mikai::app::theme::radiusSmall(mikai::app::loadAppearance()), 4);
}

void TestI18n::appearanceSaveAndCancelKeepTheLatestBatchedStyle() {
  QSettings settings;
  QVariantMap prior;
  for (const QString& key : settings.allKeys()) prior.insert(key, settings.value(key));
  const auto savedLook = mikai::app::currentAppearance();
  const auto savedPalette = qApp->palette();
  const QString savedSheet = qApp->styleSheet();
  const auto savedAccent = mplayer::shell::accent();
  const auto restore = qScopeGuard([&] {
    settings.clear();
    for (auto it = prior.cbegin(); it != prior.cend(); ++it) settings.setValue(it.key(), it.value());
    settings.sync();
    mikai::app::setCurrentAppearance(savedLook);
    qApp->setPalette(savedPalette);
    qApp->setStyleSheet(savedSheet);
    mplayer::shell::setAccent(savedAccent);
  });

  mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
  mikai::app::Runner runner;
  mikai::app::SettingsDialog dialog(client, runner);
  dialog.show();
  QVERIFY(dialog.showPage(QStringLiteral("customization/appearance")));
  const auto swatches = dialog.findChildren<QPushButton*>(QStringLiteral("accentSwatch"));
  const auto presets = mikai::app::theme::accentPresets();
  QVERIFY(swatches.size() > 2);
  swatches.at(2)->click();
  swatches.at(1)->click();
  const QColor selected(presets.at(1).color);
  auto* footer = dialog.findChild<QWidget*>(QStringLiteral("settingsFooter"));
  QVERIFY(footer != nullptr);
  auto* save = footer->findChild<QPushButton*>(QStringLiteral("primary"));
  QVERIFY(save != nullptr);
  save->click();
  QCOMPARE(dialog.result(), int(QDialog::Accepted));
  QCOMPARE(mikai::app::theme::accentColor(mikai::app::loadAppearance()), selected);
  QCOMPARE(qApp->palette().color(QPalette::Highlight), selected);

  dialog.show();
  QVERIFY(dialog.showPage(QStringLiteral("customization/appearance")));
  swatches.at(0)->click();
  dialog.reject();
  QTest::qWait(100);
  QCOMPARE(mikai::app::theme::accentColor(mikai::app::currentAppearance()), selected);
  QCOMPARE(qApp->palette().color(QPalette::Highlight), selected);
  QCOMPARE(mikai::app::theme::accentColor(mikai::app::loadAppearance()), selected);

  dialog.show();
  QVERIFY(dialog.showPage(QStringLiteral("customization/appearance")));
  swatches.at(2)->click();
  QTRY_COMPARE(qApp->palette().color(QPalette::Highlight), QColor(presets.at(2).color));
  dialog.reject();
  QCOMPARE(qApp->palette().color(QPalette::Highlight), selected);
}

void TestI18n::unparsableAppearanceFallsBackToDefaults() {
  QTemporaryDir settingsDir;
  QVERIFY(settingsDir.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
  const mikai::app::theme::Appearance saved = mikai::app::currentAppearance();
  const auto restore = qScopeGuard([saved] { mikai::app::setCurrentAppearance(saved); });

  QSettings settings;
  settings.setValue(QStringLiteral("customization/accent"), QStringLiteral("not-a-colour"));
  settings.setValue(QStringLiteral("customization/gradientColor"), QStringLiteral("#12"));
  settings.setValue(QStringLiteral("customization/radiusStep"), 99);
  settings.setValue(QStringLiteral("customization/gradientIntensity"), -20);
  settings.setValue(QStringLiteral("customization/gradientHeight"), 3);
  settings.setValue(QStringLiteral("customization/nameLanguage"), QStringLiteral("klingon"));
  settings.sync();

  const mikai::app::theme::Appearance appearance = mikai::app::loadAppearance();
  const mikai::app::theme::Appearance defaults;
  // An unparsable colour is ignored, not adopted: the default stays readable.
  QCOMPARE(mikai::app::theme::accentColor(appearance),
           mikai::app::theme::accentColor(defaults));
  QCOMPARE(mikai::app::theme::gradientColor(appearance),
           mikai::app::theme::gradientColor(defaults));
  QCOMPARE(appearance.nameLanguage, defaults.nameLanguage);
  // Numbers are clamped to their documented bounds instead of polluting a sheet.
  QCOMPARE(appearance.radiusStep, mikai::app::theme::radiusStepCount() - 1);
  QCOMPARE(appearance.gradientIntensity, 0);
  QCOMPARE(appearance.gradientHeight, 10);
}

void TestI18n::appearancePreviewCancelRollsBackAndSavePersists() {
  QSettings settings;
  settings.sync();
  const QVariantMap priorSettings = [&settings] {
    QVariantMap values;
    for (const QString& storedKey : settings.allKeys()) values.insert(storedKey, settings.value(storedKey));
    return values;
  }();
  const auto restoreSettings = qScopeGuard([&settings, priorSettings] {
    settings.clear();
    for (auto it = priorSettings.constBegin(); it != priorSettings.constEnd(); ++it) {
      settings.setValue(it.key(), it.value());
    }
    settings.sync();
  });
  Q_UNUSED(restoreSettings);
  const mikai::app::theme::Appearance saved = mikai::app::currentAppearance();
  const auto restoreLook = qScopeGuard([saved] { mikai::app::setCurrentAppearance(saved); });

  mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
  mikai::app::Runner runner;
  mikai::app::SettingsDialog dialog(client, runner);
  dialog.show();
  QVERIFY(dialog.showPage(QStringLiteral("customization/appearance")));
  QCoreApplication::processEvents();

  auto chips = dialog.findChildren<QPushButton*>(QStringLiteral("radiusChip"));
  QVERIFY2(!chips.isEmpty(), "the appearance page has no radius chips");
  auto* footer = dialog.findChild<QWidget*>(QStringLiteral("settingsFooter"));
  auto* save = footer != nullptr ? footer->findChild<QPushButton*>(QStringLiteral("primary"))
                                 : nullptr;
  QVERIFY2(save != nullptr, "the settings footer has no Save button");

  // A preview is live while the dialog is open, so the chips show their effect
  // immediately; Cancel has to put the opening look back.
  chips.first()->click();
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::currentAppearance().radiusStep, 0);
  dialog.reject();
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::currentAppearance().radiusStep, saved.radiusStep);

  // Save keeps what the preview showed and writes it to the store.
  dialog.show();
  QVERIFY(dialog.showPage(QStringLiteral("customization/appearance")));
  QCoreApplication::processEvents();
  chips = dialog.findChildren<QPushButton*>(QStringLiteral("radiusChip"));
  QVERIFY(!chips.isEmpty());
  chips.first()->click();
  QCoreApplication::processEvents();
  save->click();
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::currentAppearance().radiusStep, 0);
  QCOMPARE(mikai::app::loadAppearance().radiusStep, 0);
  // The player overlay reads the same accent: saving a look must reach it.
  QCOMPARE(mplayer::shell::accent(),
           mikai::app::theme::accentColor(mikai::app::currentAppearance()));
}

void TestI18n::playerOverlayFollowsTheApplicationAccent() {
  const QColor previous = mplayer::shell::accent();
  const auto restoreAccent = qScopeGuard(
      [previous] { mplayer::shell::setAccent(previous); });
  Q_UNUSED(restoreAccent);

  const QColor chosen(QStringLiteral("#123456"));
  mplayer::shell::setAccent(chosen);
  QCOMPARE(mplayer::shell::accent(), chosen);

  QWidget host;
  auto* flyout = new mplayer::shell::SettingsFlyout(&host);
  flyout->resize(240, 280);
  flyout->show();
  QCoreApplication::processEvents();
  const QImage shot = flyout->grab().toImage();
  bool painted = false;
  for (int y = 4; y < qMin(40, shot.height()) && !painted; ++y) {
    for (int x = 0; x < shot.width(); ++x) {
      const QColor pixel = shot.pixelColor(x, y);
      if (qAbs(pixel.red() - chosen.red()) < 24 &&
          qAbs(pixel.green() - chosen.green()) < 24 &&
          qAbs(pixel.blue() - chosen.blue()) < 24) {
        painted = true;
        break;
      }
    }
  }
  QVERIFY2(painted, "the overlay did not paint the application accent");
}

void TestI18n::settingsPagesFitTheNarrowestAllowedWidth() {
  mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
  mikai::app::Runner runner;
  mikai::app::SettingsDialog dialog(client, runner);
  dialog.resize(mikai::app::theme::kSettingsDialogMinWidth,
                mikai::app::theme::kSettingsDialogMinHeight);
  dialog.show();
  QCoreApplication::processEvents();
  auto* stack = qobject_cast<QStackedWidget*>(
      dialog.findChild<QWidget*>(QStringLiteral("settingsStack")));
  QVERIFY2(stack != nullptr, "the settings dialog has no page stack");

  QStringList clipped;
  for (const QString& id : dialog.pageIds()) {
    if (!dialog.showPage(id)) continue;
    QCoreApplication::processEvents();
    auto* scroll = qobject_cast<QScrollArea*>(stack->currentWidget());
    if (scroll == nullptr || scroll->widget() == nullptr) continue;
    // A page wider than its viewport is silently unreachable: the tabs keep the
    // horizontal bar off unless the content genuinely cannot fit.
    if (scroll->widget()->width() > scroll->viewport()->width() + 1 &&
        scroll->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff) {
      clipped.append(QStringLiteral("%1 (%2>%3)")
                         .arg(id)
                         .arg(scroll->widget()->width())
                         .arg(scroll->viewport()->width()));
    }
  }
  QVERIFY2(clipped.isEmpty(), qPrintable(QStringLiteral("clipped pages: ") + clipped.join(
                                           QStringLiteral(", "))));
}

void TestI18n::titleNamesFollowTheirOwnLanguageAndFallBack() {
  mikai::backend::Titles titles;
  titles.ua = QStringLiteral("Українська");
  titles.english = QStringLiteral("English");
  titles.original = QStringLiteral("Nihongo");
  mikai::app::theme::Appearance saved = mikai::app::currentAppearance();
  const auto restore = qScopeGuard([saved] { mikai::app::setCurrentAppearance(saved); });
  mikai::app::theme::Appearance appearance = saved;

  appearance.titleLanguage = mikai::app::theme::TitleLanguage::Ukrainian;
  mikai::app::setCurrentAppearance(appearance);
  QCOMPARE(mikai::app::labels::titleName(titles), titles.ua);

  appearance.titleLanguage = mikai::app::theme::TitleLanguage::English;
  mikai::app::setCurrentAppearance(appearance);
  QCOMPARE(mikai::app::labels::titleName(titles), titles.english);

  appearance.titleLanguage = mikai::app::theme::TitleLanguage::Original;
  mikai::app::setCurrentAppearance(appearance);
  QCOMPARE(mikai::app::labels::titleName(titles), titles.original);

  // A payload missing the preferred form falls back instead of blanking.
  mikai::backend::Titles partial;
  partial.english = QStringLiteral("Only English");
  appearance.titleLanguage = mikai::app::theme::TitleLanguage::Ukrainian;
  mikai::app::setCurrentAppearance(appearance);
  QCOMPARE(mikai::app::labels::titleName(partial), partial.english);

  mikai::backend::Titles empty;
  QCOMPARE(mikai::app::labels::titleName(empty), QStringLiteral("?"));
}

void TestI18n::nativePlanErrorsFollowTheActiveLanguage() {
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  const auto english = mplayer::native::prepareAnimeJanaiPlan(
      QStringLiteral("unrecognized-preset"), mplayer::native::Backend::TensorRT);
  QVERIFY(!english.available);
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), builtCatalogPath()));
  const auto ukrainian = mplayer::native::prepareAnimeJanaiPlan(
      QStringLiteral("unrecognized-preset"), mplayer::native::Backend::TensorRT);
  QCOMPARE(ukrainian.reasonKey, english.reasonKey);
  QVERIFY(ukrainian.reason != english.reason);
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  const auto restored = mplayer::native::prepareAnimeJanaiPlan(
      QStringLiteral("unrecognized-preset"), mplayer::native::Backend::TensorRT);
  QCOMPARE(restored.reason, english.reason);
}

void TestI18n::failedReplacementPreservesWorkingLanguage() {
  const QString catalog = builtCatalogPath();
  QVERIFY2(QFileInfo::exists(catalog),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + catalog));

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QVERIFY(!mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), missingCatalogPath()));
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));
  QCOMPARE(QLocale().language(), QLocale::English);

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog));
  const QString ukrainianError = mikai::app::localizedApiError(QStringLiteral("not_found"));

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  const QString englishError = mikai::app::localizedApiError(QStringLiteral("not_found"));
  QVERIFY2(ukrainianError != englishError,
           "the catalog changed nothing, so preservation could not be observed");

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  QCOMPARE(mikai::app::localizedApiError(QStringLiteral("not_found")), ukrainianError);

  QVERIFY(!mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), missingCatalogPath()));
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("uk"));
  QCOMPARE(QLocale().language(), QLocale::Ukrainian);
  QCOMPARE(mikai::app::localizedApiError(QStringLiteral("not_found")), ukrainianError);
}

void TestI18n::appliedLanguageDrivesDefaultLocaleNumbers() {
  const QString catalog = builtCatalogPath();
  QVERIFY2(QFileInfo::exists(catalog),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + catalog));

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog));

  const auto parsesAsOneAndAHalf = [](bool ok, double value) {
    return ok && value > 1.49 && value < 1.51;
  };

  bool ok = false;
  const double commaValue = QLocale().toDouble(QStringLiteral("1,5"), &ok);
  QVERIFY2(parsesAsOneAndAHalf(ok, commaValue),
           "the Ukrainian default locale did not accept a comma decimal");
  ok = false;
  const double dotUnderUkrainian = QLocale().toDouble(QStringLiteral("1.5"), &ok);
  QVERIFY2(!parsesAsOneAndAHalf(ok, dotUnderUkrainian),
           "the Ukrainian default locale still accepted a dot decimal");

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));

  ok = false;
  const double dotValue = QLocale().toDouble(QStringLiteral("1.5"), &ok);
  QVERIFY2(parsesAsOneAndAHalf(ok, dotValue),
           "the English default locale did not accept a dot decimal");
  ok = false;
  const double commaUnderEnglish = QLocale().toDouble(QStringLiteral("1,5"), &ok);
  QVERIFY2(!parsesAsOneAndAHalf(ok, commaUnderEnglish),
           "the English default locale still accepted a comma decimal");
}

void TestI18n::openSettingsRetranslatesInPlaceAndKeepsUnsavedValues() {
  const QString catalog = builtCatalogPath();
  QVERIFY2(QFileInfo::exists(catalog),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + catalog));
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));

  mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
  mikai::app::Runner runner;
  mikai::app::SettingsDialog dialog(client, runner);
  dialog.resize(820, 940);
  dialog.show();
  QCoreApplication::processEvents();
  dialog.showProcessingTab();
  QCoreApplication::processEvents();

  auto* runtime = dialog.findChild<QComboBox*>(QStringLiteral("runtimeChoiceCombo"));
  auto* privateAssets = dialog.findChild<QWidget*>(QStringLiteral("nativeAssetsPanel"));
  QVERIFY(runtime != nullptr);
  QVERIFY(privateAssets != nullptr);
  runtime->setCurrentIndex(
      runtime->findData(static_cast<int>(mplayer::native::RuntimeChoice::Standard)));
  QCoreApplication::processEvents();
  QVERIFY(!privateAssets->isVisibleTo(&dialog));
  auto* manager = dialog.findChild<mikai::app::AnimeJanaiManagerPanel*>(
      QStringLiteral("animeJanaiManagerPanel"));
  QVERIFY(manager != nullptr && manager->isVisibleTo(&dialog));
  auto* slotSelector = manager->findChild<QComboBox*>(QStringLiteral("ajiSlotSelectCombo"));
  auto* profileName = manager->findChild<QLineEdit*>(QStringLiteral("ajiProfileNameEdit"));
  auto* engineArgs = manager->findChild<QLineEdit*>(
      QStringLiteral("ajiTrtEngineSettingsEdit"));
  QPushButton* activate = nullptr;
  activate = manager->findChild<QPushButton*>(QStringLiteral("ajiCreateSlotButton"));
  QVERIFY(slotSelector != nullptr && profileName != nullptr && engineArgs != nullptr &&
          activate != nullptr);
  QTRY_VERIFY(activate->isEnabled());
  const int editedSlot = slotSelector->findData(4);
  QVERIFY(editedSlot >= 0);
  slotSelector->setCurrentIndex(editedSlot);
  activate->click();
  profileName->setText(QStringLiteral("Unsaved multi-chain profile"));
  engineArgs->setText(QStringLiteral("--builderOptimizationLevel=4 --skipInference"));
  QCheckBox* chainRife = nullptr;
  for (QCheckBox* candidate : manager->findChildren<QCheckBox*>()) {
    if (candidate->text() ==
        mikai::app::AnimeJanaiManagerPanel::tr("RIFE interpolation")) {
      chainRife = candidate;
      break;
    }
  }
  QVERIFY(chainRife != nullptr);
  chainRife->setChecked(true);
  auto* addStep = manager->findChild<QPushButton*>(QStringLiteral("ajiAddStepButton"));
  QVERIFY(addStep != nullptr);
  addStep->click();
  auto* kind = manager->findChild<QComboBox*>(QStringLiteral("animeJanaiStepKind_1"));
  QVERIFY(kind != nullptr);
  kind->setCurrentIndex(kind->findData(
      static_cast<int>(mplayer::native::AnimeJanaiStepKind::Anime4K)));
  auto* scale = manager->findChild<QComboBox*>(QStringLiteral("animeJanaiStepScale_1"));
  QVERIFY(scale != nullptr);
  scale->setCurrentIndex(scale->findData(4));
  auto* moveDown = manager->findChild<QPushButton*>(QStringLiteral("animeJanaiStepMoveDown_1"));
  QVERIFY(moveDown != nullptr);
  moveDown->click();
  QVERIFY(manager->isDirty());
  const auto managerBefore = manager->currentConfig();
  const auto mixedChain = managerBefore.customSlots.last().chains.last();
  QCOMPARE(mixedChain.steps.size(), 2);
  QCOMPARE(mixedChain.steps.at(0).kind, mplayer::native::AnimeJanaiStepKind::OnnxModel);
  QCOMPARE(mixedChain.steps.at(1).kind, mplayer::native::AnimeJanaiStepKind::Anime4K);
  QCOMPARE(mixedChain.steps.at(1).id, QStringLiteral("fast_a"));
  QCOMPARE(mixedChain.steps.at(1).scaleFactor, 4);
  const QString activateEnglish = activate->text();

  runtime->setCurrentIndex(
      runtime->findData(static_cast<int>(mplayer::native::RuntimeChoice::Native)));
  QCoreApplication::processEvents();
  QVERIFY(privateAssets->isVisibleTo(&dialog));
  QVERIFY(!manager->isVisibleTo(&dialog));
  auto* threshold = dialog.findChild<QLineEdit*>(QStringLiteral("nativeRifeSceneThresholdEdit"));
  auto* factorNum = dialog.findChild<QSpinBox*>(QStringLiteral("nativeRifeFactorNumSpin"));
  auto* factorDen = dialog.findChild<QSpinBox*>(QStringLiteral("nativeRifeFactorDenSpin"));
  auto* backend = dialog.findChild<QComboBox*>(QStringLiteral("nativeBackendCombo"));
  auto* recipe = dialog.findChild<QComboBox*>(QStringLiteral("nativeRecipeCombo"));
  auto* storeButton = dialog.findChild<QPushButton*>(QStringLiteral("nativeRifeStoreButton"));
  auto* status = dialog.findChild<QLabel*>(QStringLiteral("nativeRifeStatus"));
  auto* assetsPanel = dialog.findChild<QWidget*>(QStringLiteral("nativeAssetsPanel"));
  auto* scroll = dialog.findChild<QScrollArea*>(QStringLiteral("nativeProcessingScroll"));
  QVERIFY2(threshold != nullptr && factorNum != nullptr && factorDen != nullptr &&
               backend != nullptr && recipe != nullptr && runtime != nullptr &&
               storeButton != nullptr && status != nullptr && assetsPanel != nullptr &&
               scroll != nullptr,
           "the real settings surface has every control this test drives");

  factorNum->setValue(5);
  factorDen->setValue(3);
  threshold->setText(QStringLiteral("0.375"));
  const int backendIndex =
      backend->findData(static_cast<int>(mplayer::native::Backend::DirectML));
  if (backendIndex >= 0) {
    backend->setCurrentIndex(backendIndex);
  }
  const int runtimeIndex =
      runtime->findData(static_cast<int>(mplayer::native::RuntimeChoice::Native));
  if (runtimeIndex >= 0) {
    runtime->setCurrentIndex(runtimeIndex);
  }
  QVERIFY(dialog.showPage(QStringLiteral("processing")));
  scroll->verticalScrollBar()->setValue(40);
  const QString englishStatus = status->text();
  const QString englishSaveText = storeButton->text();
  const QString englishTitle = dialog.windowTitle();
  const int englishScroll = scroll->verticalScrollBar()->value();
  QCOMPARE(dialog.nativePreparationDesiredBackend(),
           static_cast<mplayer::native::Backend>(backend->currentData().toInt()));

  const auto storedValues = [] {
    QSettings settings;
    QVariantMap values;
    for (const QString& key : settings.allKeys()) {
      values.insert(key, settings.value(key));
    }
    return values;
  };
  const QVariantMap storedBefore = storedValues();

  mplayer::shell::PlayerShell player;
  auto* playButton = player.findChild<QPushButton*>(QStringLiteral("playerPlayToggle"));
  QVERIFY(playButton != nullptr);
  const QString englishPlayTooltip = playButton->toolTip();

  // A language change replaces the decimal separator, not the entered number.
  const auto thresholdValue = [threshold]() {
    for (const QLocale& locale : {QLocale(), QLocale::c()}) {
      bool ok = false;
      const double value = locale.toDouble(threshold->text(), &ok);
      if (ok) return value;
    }
    return qQNaN();
  };
  const auto capture = [&]() {
    return QStringList{QString::number(thresholdValue(), 'g', 17),
                       QString::number(factorNum->value()),
                       QString::number(factorDen->value()),
                       backend->currentData().toString(),
                       recipe->currentData().toString(),
                       runtime->currentData().toString(),
                       dialog.currentPageId()};
  };
  const QStringList before = capture();
  QVERIFY2(qAbs(thresholdValue() - 0.375) < 1e-12,
           "the unsaved threshold was not the number the test typed");

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog));
  QCoreApplication::processEvents();

  QVERIFY2(playButton->toolTip() != englishPlayTooltip,
           "the existing player control was not retranslated");
  QVERIFY2(capture() == before,
           "an unsaved native value, stable id or tab did not survive the retranslation");
  QCOMPARE(scroll->verticalScrollBar()->value(), englishScroll);
  QVERIFY(manager->currentConfig() == managerBefore);
  QVERIFY(manager->isDirty());
  QCOMPARE(slotSelector->currentData().toInt(), 4);
  QVERIFY(activate->text() != activateEnglish);

  QVERIFY2(storeButton->text() != englishSaveText,
           "the RIFE store button still shows the language the dialog opened in");
  QVERIFY2(status->text() != englishStatus, "the RIFE status was not retranslated");
  QVERIFY2(dialog.windowTitle() != englishTitle, "the dialog title was not retranslated");
  QCOMPARE(storedValues(), storedBefore);

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();
  QCOMPARE(capture(), before);
  QCOMPARE(playButton->toolTip(), englishPlayTooltip);
  QVERIFY(manager->currentConfig() == managerBefore);
  QCOMPARE(activate->text(), activateEnglish);
  QCOMPARE(storedValues(), storedBefore);
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));

  runtime->setCurrentIndex(
      runtime->findData(static_cast<int>(mplayer::native::RuntimeChoice::Standard)));
  QCoreApplication::processEvents();
  QVERIFY(!privateAssets->isVisibleTo(&dialog));
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog));
  QCoreApplication::processEvents();
  QVERIFY(!privateAssets->isVisibleTo(&dialog));
  QCOMPARE(storedValues(), storedBefore);
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();
  QVERIFY(!privateAssets->isVisibleTo(&dialog));
  QVERIFY(manager->isVisibleTo(&dialog));
  QVERIFY(manager->currentConfig() == managerBefore);
  QVERIFY(manager->isDirty());
  QCOMPARE(storedValues(), storedBefore);
}

void TestI18n::nativeUnavailableReasonFollowsTheOpenSettingsLanguage() {
  const QString catalog = builtCatalogPath();
  QVERIFY2(QFileInfo::exists(catalog),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + catalog));
  // The selector only reuses an explicit catalog after one successful Ukrainian
  // load, so prime it and open the dialog in English.
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog));
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));

  QSettings settings;
  settings.sync();
  QVariantMap priorSettings;
  for (const QString& key : settings.allKeys()) {
    priorSettings.insert(key, settings.value(key));
  }
  const auto restoreSettings = qScopeGuard([&settings, priorSettings] {
    settings.clear();
    for (auto it = priorSettings.constBegin(); it != priorSettings.constEnd(); ++it) {
      settings.setValue(it.key(), it.value());
    }
    settings.sync();
  });
  Q_UNUSED(restoreSettings);

  // The Processing tab states a refusal only for a real request that is on, so
  // the request is written through the production patch rather than a test-only
  // setter.
  mplayer::native::clearNativePreferences();
  mplayer::native::NativePreferencePatch requestPatch;
  requestPatch.enabled = true;
  QString saveReason;
  QVERIFY2(mplayer::native::saveNativePreferences(requestPatch, &saveReason),
           qPrintable(saveReason));
  QVERIFY(mplayer::native::loadNativePreferences().enabled);

  mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
  mikai::app::Runner runner;
  mikai::app::SettingsDialog dialog(client, runner);
  dialog.show();
  QCoreApplication::processEvents();
  dialog.showProcessingTab();

  auto* runtime = dialog.findChild<QComboBox*>(QStringLiteral("runtimeChoiceCombo"));
  auto* backend = dialog.findChild<QComboBox*>(QStringLiteral("nativeBackendCombo"));
  auto* recipe = dialog.findChild<QComboBox*>(QStringLiteral("nativeRecipeCombo"));
  QVERIFY2(runtime != nullptr && backend != nullptr && recipe != nullptr,
           "the real settings surface has the runtime, backend and recipe controls");

  // The typed facts an application owner already holds for the live source:
  // observed geometry, device, module and decoder, and no factory observation.
  // Installing the provider reads these values; it probes nothing.
  mplayer::native::NativePreparationContext context;
  context.source.codedSize = QSize(1920, 1080);
  context.source.fps = mplayer::native::Rational{24000, 1001};
  context.source.csp = QStringLiteral("bt709");
  context.source.bitDepth = 10;
  context.device.decoderDeviceUuid = QStringLiteral("GPU-fixture-decoder");
  context.device.computeDeviceUuid = QStringLiteral("GPU-fixture-compute");
  context.device.smMajor = 8;
  context.device.smMinor = 6;
  context.modulePath = QStringLiteral("fixture/libmpv-2.dll");
  context.decoder = QStringLiteral("d3d11");
  int contextReads = 0;
  dialog.setNativePreparationContextProvider(
      [&context, &contextReads]() -> std::optional<mplayer::native::NativePreparationContext> {
        ++contextReads;
        return context;
      });
  QVERIFY2(contextReads > 0, "the installed typed context was never read");

  const int nativeRuntimeIndex =
      runtime->findData(static_cast<int>(mplayer::native::RuntimeChoice::Native));
  QVERIFY(nativeRuntimeIndex >= 0);
  runtime->setCurrentIndex(nativeRuntimeIndex);
  QCoreApplication::processEvents();

  // The same projection the dialog makes: the stored request against the typed
  // runtime state and the installed context (which carries no factory). The
  // test reads it through the public producer, never by probing.
  const auto projection = [&]() {
    mplayer::native::NativePreferences request = mplayer::native::loadNativePreferences();
    request.backend = static_cast<mplayer::native::Backend>(backend->currentData().toInt());
    request.recipeId = recipe->currentData().toString();
    return mplayer::native::evaluateNativeSelection(
        request, mplayer::native::currentNativeAvailability(nullptr));
  };

  const mplayer::native::NativeSelectionView englishView = projection();
  QVERIFY(englishView.requestedEnabled);
  QVERIFY2(!englishView.requestAvailable, "the fixture must exercise a refused request");
  QVERIFY(englishView.availability.hasMandatoryReason());
  QVERIFY2(englishView.availability.reasonKey ==
                   QStringLiteral("native.runtimeUnavailable") ||
               englishView.availability.reasonKey ==
                   QStringLiteral("native.capabilitiesUnconfirmed"),
           qPrintable(QStringLiteral("the refusal is not an app-owned reason: %1")
                          .arg(englishView.availability.reasonKey)));
  const QString englishReason = englishView.availability.reason;
  QVERIFY(!englishReason.isEmpty());

  const QString englishText =
      mikai::app::SettingsDialog::tr("Unavailable: %1").arg(englishReason);
  QLabel* nativeStatus = nullptr;
  for (QLabel* label : dialog.findChildren<QLabel*>()) {
    if (label->text() == englishText) {
      nativeStatus = label;
      break;
    }
  }
  QVERIFY2(nativeStatus != nullptr,
           "the open Processing tab does not state the refused request's reason");
  QVERIFY(nativeStatus->isVisibleTo(&dialog));

  const int chosenRuntime = runtime->currentData().toInt();
  const int chosenBackend = backend->currentData().toInt();
  const QString chosenRecipe = recipe->currentData().toString();
  const auto storedValues = [] {
    QSettings current;
    current.sync();
    QVariantMap values;
    for (const QString& key : current.allKeys()) {
      values.insert(key, current.value(key));
    }
    return values;
  };
  const QVariantMap storedBefore = storedValues();

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog));
  QCoreApplication::processEvents();

  const mplayer::native::NativeSelectionView ukrainianView = projection();
  QCOMPARE(ukrainianView.availability.reasonKey, englishView.availability.reasonKey);
  QVERIFY2(ukrainianView.availability.reason != englishReason,
           "the app-owned reason has no Ukrainian wording, so staleness could not be "
           "observed");
  QVERIFY2(!nativeStatus->text().contains(englishReason),
           "the open Processing tab still renders the reason text it captured before the "
           "language changed");
  QCOMPARE(nativeStatus->text(),
           mikai::app::SettingsDialog::tr("Unavailable: %1")
               .arg(ukrainianView.availability.reason));
  QVERIFY(nativeStatus->isVisibleTo(&dialog));
  QCOMPARE(runtime->currentData().toInt(), chosenRuntime);
  QCOMPARE(backend->currentData().toInt(), chosenBackend);
  QCOMPARE(recipe->currentData().toString(), chosenRecipe);
  QVERIFY(context.source.codedSize.has_value());
  QCOMPARE(*context.source.codedSize, QSize(1920, 1080));
  QCOMPARE(context.device.computeDeviceUuid, QStringLiteral("GPU-fixture-compute"));
  QCOMPARE(context.modulePath, QStringLiteral("fixture/libmpv-2.dll"));
  QVERIFY2(!context.factory.has_value(),
           "the typed context the test installed never grows a factory observation");
  QCOMPARE(storedValues(), storedBefore);

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();

  const mplayer::native::NativeSelectionView restoredView = projection();
  QCOMPARE(restoredView.availability.reasonKey, englishView.availability.reasonKey);
  QCOMPARE(restoredView.availability.reason, englishReason);
  QCOMPARE(nativeStatus->text(), englishText);
  QVERIFY(nativeStatus->isVisibleTo(&dialog));
  QCOMPARE(runtime->currentData().toInt(), chosenRuntime);
  QCOMPARE(backend->currentData().toInt(), chosenBackend);
  QCOMPARE(recipe->currentData().toString(), chosenRecipe);
  QCOMPARE(storedValues(), storedBefore);

  // A refused factory's own cause is not app wording, so no language may
  // re-word it. The same producer the dialog reads is given a typed Native
  // runtime and observation; the cause survives both languages verbatim.
  mplayer::native::RuntimeSelection nativeRuntime;
  nativeRuntime.loaded = true;
  nativeRuntime.kind = mplayer::native::BundleKind::Native;
  nativeRuntime.requested = mplayer::native::RuntimeChoice::Native;
  mplayer::native::NativeFactoryObservation refusedFactory;
  refusedFactory.observed = true;
  refusedFactory.reasonKey = QStringLiteral("native.factory.protocol");
  refusedFactory.reason =
      QStringLiteral("factory protocol 7 refused the adopted device (code 0x2a)");
  const mplayer::native::NativeAvailability externalEnglish =
      mplayer::native::nativeAvailabilityFor(nativeRuntime, &refusedFactory);
  QCOMPARE(externalEnglish.engineReasonKey, refusedFactory.reasonKey);
  QCOMPARE(externalEnglish.engineReason, refusedFactory.reason);
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog));
  const mplayer::native::NativeAvailability externalUkrainian =
      mplayer::native::nativeAvailabilityFor(nativeRuntime, &refusedFactory);
  QCOMPARE(externalUkrainian.engineReasonKey, externalEnglish.engineReasonKey);
  QCOMPARE(externalUkrainian.engineReason, externalEnglish.engineReason);
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
}

void TestI18n::processingFormsRemainReadableWhenResized_data() {
  QTest::addColumn<QString>("language");
  QTest::addColumn<int>("width");
  for (const QString& language : {QStringLiteral("en"), QStringLiteral("uk")}) {
    for (const int width : {560, 760, 1100}) {
      const QByteArray name = QStringLiteral("%1-%2").arg(language).arg(width).toLatin1();
      QTest::newRow(name.constData()) << language << width;
    }
  }
}

void TestI18n::processingFormsRemainReadableWhenResized() {
  QFETCH(QString, language);
  QFETCH(int, width);
  QVERIFY(mikai::app::applyInterfaceLanguage(language, builtCatalogPath()));
  mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
  mikai::app::Runner runner;
  mikai::app::SettingsDialog dialog(client, runner);
  QVERIFY(dialog.showPage(QStringLiteral("benchmark")));
  const QSize compactSize(qMax(width, dialog.minimumWidth()), dialog.minimumHeight());
  dialog.resize(compactSize);
  dialog.show();
  auto* footer = dialog.findChild<QWidget*>(QStringLiteral("settingsFooter"));
  QVERIFY(footer != nullptr);
  QTRY_VERIFY(dialog.rect().contains(QRect(footer->mapTo(&dialog, QPoint()), footer->size())));
  dialog.showProcessingTab();

  auto* runtime = dialog.findChild<QComboBox*>(QStringLiteral("runtimeChoiceCombo"));
  auto* manager = dialog.findChild<mikai::app::AnimeJanaiManagerPanel*>(
      QStringLiteral("animeJanaiManagerPanel"));
  auto* scroll = dialog.findChild<QScrollArea*>(QStringLiteral("nativeProcessingScroll"));
  QVERIFY(runtime != nullptr && manager != nullptr && scroll != nullptr);
  runtime->setCurrentIndex(
      runtime->findData(static_cast<int>(mplayer::native::RuntimeChoice::Standard)));

  QPushButton* activate =
      manager->findChild<QPushButton*>(QStringLiteral("ajiCreateSlotButton"));
  QPushButton* addStep =
      manager->findChild<QPushButton*>(QStringLiteral("ajiAddStepButton"));
  QVERIFY(activate != nullptr && addStep != nullptr);
  QTRY_VERIFY(activate->isEnabled());
  activate->click();
  QTRY_VERIFY(addStep->isEnabled());
  addStep->click();
  auto* stepKind = manager->findChild<QComboBox*>(QStringLiteral("animeJanaiStepKind_1"));
  QVERIFY(stepKind != nullptr);
  stepKind->setCurrentIndex(stepKind->findData(
      static_cast<int>(mplayer::native::AnimeJanaiStepKind::Anime4K)));
  auto* stepScale = manager->findChild<QComboBox*>(QStringLiteral("animeJanaiStepScale_1"));
  QVERIFY(stepScale != nullptr);
  stepScale->setCurrentIndex(stepScale->findData(2));
  QComboBox* engineType = nullptr;
  for (QLabel* label : manager->findChildren<QLabel*>()) {
    if (label->text() == mikai::app::AnimeJanaiManagerPanel::tr("TensorRT engine type")) {
      engineType = qobject_cast<QComboBox*>(label->buddy());
      break;
    }
  }
  QVERIFY(engineType != nullptr);
  const int dynamic = engineType->findData(QStringLiteral("dynamic"));
  QVERIFY(dynamic >= 0);
  engineType->setCurrentIndex(dynamic);
  for (QCheckBox* check : manager->findChildren<QCheckBox*>()) {
    if (check->text() == mikai::app::AnimeJanaiManagerPanel::tr("RIFE interpolation")) {
      check->setChecked(true);
    }
  }

  const auto layoutProblem = [&]() -> QString {
    if (scroll->widget()->width() > scroll->viewport()->width() &&
        scroll->horizontalScrollBar()->maximum() == 0) {
      return QStringLiteral("Processing content is unreachable: %1 px in a %2 px viewport.")
          .arg(scroll->widget()->width()).arg(scroll->viewport()->width());
    }
    for (QLabel* label : manager->findChildren<QLabel*>()) {
      QWidget* field = label->buddy();
      if (field == nullptr || !label->isVisibleTo(&dialog)) continue;
      const QRect caption(label->mapTo(&dialog, QPoint()), label->size());
      const QRect editor(field->mapTo(&dialog, QPoint()), field->size());
      if (caption.intersects(editor)) {
        return QStringLiteral("Caption overlaps its editor: %1").arg(label->text());
      }
      const int requiredHeight = label->heightForWidth(label->width());
      if (label->height() < requiredHeight) {
        return QStringLiteral("Clipped caption %1: %2x%3, requires %4 px height.")
            .arg(label->text()).arg(label->width()).arg(label->height()).arg(requiredHeight);
      }
    }
    return {};
  };
  for (const int resizedWidth : {width, 1100, width}) {
    dialog.resize(resizedWidth, 780);
    QTRY_VERIFY2(layoutProblem().isEmpty(), qPrintable(layoutProblem()));
  }
}

void TestI18n::unavailableLanguageIsRefusedByNameAndKeepsTheWorkingTranslation() {
  const QString catalog = builtCatalogPath();
  QVERIFY2(QFileInfo::exists(catalog),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + catalog));

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog));
  const QString working = mikai::app::localizedApiError(QStringLiteral("not_found"));

  QVERIFY(!mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), missingCatalogPath()));
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("uk"));
  QCOMPARE(QLocale().language(), QLocale::Ukrainian);
  QCOMPARE(mikai::app::localizedApiError(QStringLiteral("not_found")), working);
}

void TestI18n::cancelChangesNoLanguageAtAll() {
  const QString catalog = builtCatalogPath();
  QVERIFY2(QFileInfo::exists(catalog),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + catalog));
  // The manager retains an explicit catalog only after a successful Ukrainian
  // load. Prime it here so a preview never depends on the embedded resource,
  // which a test executable does not carry, and restore the language this test
  // opens in.
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog));
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));

  QSettings settings;
  const QString key = QString::fromLatin1(mikai::app::kUiLanguageSettingKey);
  const bool storedPresent = settings.contains(key);
  const QVariant storedValue = settings.value(key);

  mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
  mikai::app::Runner runner;
  mikai::app::SettingsDialog dialog(client, runner);
  dialog.show();
  QCoreApplication::processEvents();
  auto* language = dialog.findChild<QComboBox*>(QStringLiteral("uiLanguageCombo"));
  QVERIFY(language != nullptr);
  const int english = language->findData(QStringLiteral("en"));
  const int ukrainian = language->findData(QStringLiteral("uk"));
  QVERIFY(english >= 0 && ukrainian >= 0);
  language->setCurrentIndex(english);
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));

  language->setCurrentIndex(ukrainian);
  QCoreApplication::processEvents();

  // The choice is installed before anything is accepted; Cancel has to put the
  // opening language back without writing the preference.
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("uk"));
  QCOMPARE(language->currentData().toString(), QStringLiteral("uk"));
  settings.sync();
  QCOMPARE(settings.contains(key), storedPresent);
  QCOMPARE(settings.value(key), storedValue);

  dialog.reject();
  QCoreApplication::processEvents();

  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));
  QCOMPARE(language->currentData().toString(), mikai::app::languagePreference());
  settings.sync();
  QCOMPARE(settings.contains(key), storedPresent);
  QCOMPARE(settings.value(key), storedValue);
}

void TestI18n::dialogPreviewAndSavePersistWithoutRestart() {
  const QString catalog = builtCatalogPath();
  QVERIFY2(QFileInfo::exists(catalog),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + catalog));
  // Prime the explicit catalog so the preview inside the dialog has a real
  // replacement to load even when this test runs alone.
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog));
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));

  QSettings settings;
  settings.sync();
  const QString key = QString::fromLatin1(mikai::app::kUiLanguageSettingKey);
  // Save writes the whole preference block, so this test puts the shared store
  // back exactly as it found it for the suites that run after it.
  QVariantMap priorSettings;
  for (const QString& storedKey : settings.allKeys()) {
    priorSettings.insert(storedKey, settings.value(storedKey));
  }
  const auto restoreSettings = qScopeGuard([&settings, priorSettings] {
    settings.clear();
    for (auto it = priorSettings.constBegin(); it != priorSettings.constEnd(); ++it) {
      settings.setValue(it.key(), it.value());
    }
    settings.sync();
  });
  Q_UNUSED(restoreSettings);

  mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
  mikai::app::Runner runner;
  mikai::app::SettingsDialog dialog(client, runner);
  dialog.show();
  QCoreApplication::processEvents();
  auto* language = dialog.findChild<QComboBox*>(QStringLiteral("uiLanguageCombo"));
  auto* footer = dialog.findChild<QWidget*>(QStringLiteral("settingsFooter"));
  auto* save = footer != nullptr ? footer->findChild<QPushButton*>(QStringLiteral("primary"))
                                 : nullptr;
  QVERIFY2(language != nullptr && save != nullptr,
           "the real settings surface has the language selector and Save");
  const QString englishSaveText = save->text();
  const int english = language->findData(QStringLiteral("en"));
  const int ukrainian = language->findData(QStringLiteral("uk"));
  QVERIFY(english >= 0 && ukrainian >= 0);

  // en -> uk -> en, then back to uk: every step is the running language.
  language->setCurrentIndex(english);
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));
  language->setCurrentIndex(ukrainian);
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("uk"));
  QVERIFY2(save->text() != englishSaveText,
           "the open settings window did not retranslate before Save");
  language->setCurrentIndex(english);
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));
  QCOMPARE(save->text(), englishSaveText);
  language->setCurrentIndex(ukrainian);
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("uk"));

  save->click();
  QCoreApplication::processEvents();
  QVERIFY2(!dialog.isVisible(), "a successful Save did not close the dialog");
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("uk"));
  settings.sync();
  QCOMPARE(settings.value(key).toString(), QStringLiteral("uk"));

  // The committed choice survives this dialog going away and the next one
  // opening: neither destruction nor a fresh Cancel may roll a Save back.
  {
    mikai::app::SettingsDialog reopened(client, runner);
    reopened.show();
    QCoreApplication::processEvents();
    auto* reopenedLanguage =
        reopened.findChild<QComboBox*>(QStringLiteral("uiLanguageCombo"));
    QVERIFY(reopenedLanguage != nullptr);
    QCOMPARE(reopenedLanguage->currentData().toString(), QStringLiteral("uk"));
    reopened.reject();
    QCoreApplication::processEvents();
  }
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("uk"));
  settings.sync();
  QCOMPARE(settings.value(key).toString(), QStringLiteral("uk"));
}

void TestI18n::refusedSaveKeepsThePreviewAndCancelRollsBack() {
  const QString catalog = builtCatalogPath();
  QVERIFY2(QFileInfo::exists(catalog),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + catalog));
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog));
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));

  // A .env whose parent is a file: the atomic write cannot land, exactly the
  // shape of a full or unavailable disk. The key save is the first fallible
  // operation Save performs, so it refuses before any preference is committed.
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString blocker = QDir(directory.path()).filePath(QStringLiteral("blocker"));
  {
    QFile file(blocker);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write("not a directory\n") > 0);
  }
  mikai::backend::setEnvFilePathOverride(QDir(blocker).filePath(QStringLiteral(".env")));
  const auto restoreEnvironment =
      qScopeGuard([] { mikai::backend::setEnvFilePathOverride(QString()); });
  Q_UNUSED(restoreEnvironment);

  QSettings settings;
  const QString key = QString::fromLatin1(mikai::app::kUiLanguageSettingKey);
  const bool storedPresent = settings.contains(key);
  const QVariant storedValue = settings.value(key);

  mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
  mikai::app::Runner runner;
  mikai::app::SettingsDialog dialog(client, runner);
  dialog.show();
  QCoreApplication::processEvents();
  auto* language = dialog.findChild<QComboBox*>(QStringLiteral("uiLanguageCombo"));
  auto* token = dialog.findChild<QLineEdit*>(QStringLiteral("apiTokenEdit"));
  auto* footer = dialog.findChild<QWidget*>(QStringLiteral("settingsFooter"));
  auto* save = footer != nullptr ? footer->findChild<QPushButton*>(QStringLiteral("primary"))
                                 : nullptr;
  QVERIFY2(language != nullptr && token != nullptr && save != nullptr,
           "the real settings surface has every control this test drives");
  const int english = language->findData(QStringLiteral("en"));
  const int ukrainian = language->findData(QStringLiteral("uk"));
  QVERIFY(english >= 0 && ukrainian >= 0);
  language->setCurrentIndex(english);
  QCoreApplication::processEvents();
  language->setCurrentIndex(ukrainian);
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("uk"));

  token->setText(QStringLiteral("mk_candidate_that_cannot_be_written"));
  save->click();
  QCoreApplication::processEvents();

  QVERIFY2(dialog.isVisible(), "a refused Save closed the dialog");
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("uk"));
  auto* generalPage = dialog.findChild<QWidget*>(QStringLiteral("generalPage"));
  QVERIFY(generalPage != nullptr);
  auto* banner = generalPage->findChild<QLabel*>(QStringLiteral("status"));
  QVERIFY(banner != nullptr);
  QVERIFY2(banner->isVisibleTo(&dialog), "the refused Save was not explained");
  QVERIFY(!banner->text().isEmpty());
  settings.sync();
  QCOMPARE(settings.contains(key), storedPresent);
  QCOMPARE(settings.value(key), storedValue);

  dialog.reject();
  QCoreApplication::processEvents();

  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));
  QCOMPARE(language->currentData().toString(), mikai::app::languagePreference());
  settings.sync();
  QCOMPARE(settings.contains(key), storedPresent);
  QCOMPARE(settings.value(key), storedValue);
}

void TestI18n::refusedCatalogReplacementRestoresTheSelector() {
  const QString catalog = builtCatalogPath();
  QVERIFY2(QFileInfo::exists(catalog),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + catalog));

  // Point the shared manager at a catalog that is about to disappear. A later
  // switch to Ukrainian then takes the real replacement-failure path instead of
  // a no-op, while the built fixture stays untouched.
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString transient =
      QDir(directory.path()).filePath(QStringLiteral("mikai_uk_transient.qm"));
  QVERIFY2(QFile::copy(catalog, transient), "could not stage a temporary catalog");
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), transient));
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QVERIFY(QFile::remove(transient));

  QSettings settings;
  const QString key = QString::fromLatin1(mikai::app::kUiLanguageSettingKey);
  const bool storedPresent = settings.contains(key);
  const QVariant storedValue = settings.value(key);

  mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
  mikai::app::Runner runner;
  mikai::app::SettingsDialog dialog(client, runner);
  dialog.show();
  QCoreApplication::processEvents();
  auto* language = dialog.findChild<QComboBox*>(QStringLiteral("uiLanguageCombo"));
  QVERIFY(language != nullptr);
  const int english = language->findData(QStringLiteral("en"));
  const int ukrainian = language->findData(QStringLiteral("uk"));
  QVERIFY(english >= 0 && ukrainian >= 0);
  language->setCurrentIndex(english);
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));

  language->setCurrentIndex(ukrainian);
  QCoreApplication::processEvents();

  // The refused replacement changed nothing: the working translation stays and
  // the selector names the choice that is actually installed.
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));
  QCOMPARE(language->currentData().toString(), QStringLiteral("en"));
  auto* generalPage = dialog.findChild<QWidget*>(QStringLiteral("generalPage"));
  QVERIFY(generalPage != nullptr);
  auto* banner = generalPage->findChild<QLabel*>(QStringLiteral("status"));
  QVERIFY(banner != nullptr);
  QVERIFY2(banner->isVisibleTo(&dialog), "the refused catalog was not explained");
  QVERIFY(!banner->text().isEmpty());
  settings.sync();
  QCOMPARE(settings.contains(key), storedPresent);
  QCOMPARE(settings.value(key), storedValue);

  dialog.reject();
  QCoreApplication::processEvents();
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));
  settings.sync();
  QCOMPARE(settings.contains(key), storedPresent);
  QCOMPARE(settings.value(key), storedValue);

  // Leave the shared manager pointing at the real catalog for later tests.
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog));
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
}

void TestI18n::rangePagerRetranslatesWithoutMovingThePageOrAsking() {
  QVERIFY2(primeBuiltCatalogForSwitching(),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + builtCatalogPath()));

  mikai::app::RangePager pager(50);
  // A page away from the first one, with real entry numbers, so a caption that
  // "resets to page 1" or renumbers the window is visible.
  pager.setState(5, 8, 400);
  pager.show();
  QCoreApplication::processEvents();

  QPushButton* first = pagerArrow(pager, QStringLiteral("⏮"));
  QPushButton* previous = pagerArrow(pager, QStringLiteral("←"));
  QPushButton* next = pagerArrow(pager, QStringLiteral("→"));
  QPushButton* last = pagerArrow(pager, QStringLiteral("⏭"));
  auto* jump = pager.findChild<QSpinBox*>();
  auto* caption = pager.findChild<QLabel*>();
  QVERIFY2(first != nullptr && previous != nullptr && next != nullptr && last != nullptr &&
               jump != nullptr && caption != nullptr,
           "the real pager exposes its arrows, page box and caption");
  const QList<QPushButton*> chips = pagerChips(pager);
  QVERIFY2(!chips.isEmpty(), "the pager rendered no page chips");
  QVERIFY2(chips.first()->text() != QStringLiteral("1 – 50"),
           "the chip window did not follow the page, so a reset would be invisible");

  const QString englishFirstTip = first->toolTip();
  const QString englishFirstAccessible = first->accessibleName();
  const QStringList englishChips = pagerChipLabels(pager);
  const QString englishChipTip = chips.first()->toolTip();
  const QString englishChipAccessible = chips.first()->accessibleName();
  const QString englishJumpTip = jump->toolTip();
  const QString englishJumpAccessible = jump->accessibleName();
  const QString englishCaption = caption->text();
  const int pageBefore = pager.page();
  const int pagesBefore = pager.pages();
  const int jumpBefore = jump->value();
  QCOMPARE(pageBefore, 5);
  QCOMPARE(pagesBefore, 8);
  QCOMPARE(jumpBefore, 5);

  QSignalSpy requested(&pager, &mikai::app::RangePager::pageRequested);

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  QCoreApplication::processEvents();

  QVERIFY2(first->toolTip() != englishFirstTip,
           "the arrow caption still shows the language the pager opened in");
  QCOMPARE(first->toolTip(),
           QCoreApplication::translate("mikai::app::RangePager", "Go to first page"));
  QVERIFY2(first->accessibleName() != englishFirstAccessible,
           "the arrow's accessible name was not restated");
  QVERIFY2(chips.first()->toolTip() != englishChipTip, "the chip tooltip was not retranslated");
  QVERIFY2(chips.first()->accessibleName() != englishChipAccessible,
           "the chip's accessible name was not retranslated");
  QVERIFY2(jump->toolTip() != englishJumpTip, "the page box tooltip was not retranslated");
  QVERIFY2(jump->accessibleName() != englishJumpAccessible,
           "the page box accessible name was not retranslated");
  QVERIFY2(caption->text() != englishCaption, "the page caption still shows the old language");

  // The words changed; the page, the entry ranges and the selected page number
  // did not, and re-labelling never asked the owner to fetch a page.
  QCOMPARE(pagerChipLabels(pager), englishChips);
  QCOMPARE(pager.page(), pageBefore);
  QCOMPARE(pager.pages(), pagesBefore);
  QCOMPARE(jump->value(), jumpBefore);
  QCOMPARE(requested.count(), 0);

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();

  QCOMPARE(first->toolTip(), englishFirstTip);
  QCOMPARE(first->accessibleName(), englishFirstAccessible);
  QCOMPARE(chips.first()->toolTip(), englishChipTip);
  QCOMPARE(chips.first()->accessibleName(), englishChipAccessible);
  QCOMPARE(jump->toolTip(), englishJumpTip);
  QCOMPARE(jump->accessibleName(), englishJumpAccessible);
  QCOMPARE(caption->text(), englishCaption);
  QCOMPARE(pagerChipLabels(pager), englishChips);
  QCOMPARE(pager.page(), pageBefore);
  QCOMPARE(pager.pages(), pagesBefore);
  QCOMPARE(jump->value(), jumpBefore);
  QCOMPARE(requested.count(), 0);
}

void TestI18n::failureViewRephrasesOnlyItsOwnWordsAndKeepsBackendDetail() {
  QVERIFY2(primeBuiltCatalogForSwitching(),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + builtCatalogPath()));
  const QString englishCanonical = mikai::app::localizedApiError(QStringLiteral("not_found"));
  QVERIFY(!englishCanonical.isEmpty());

  mikai::app::FailureView canonicalView;
  QSignalSpy canonicalRetries(&canonicalView, &mikai::app::FailureView::retryRequested);
  auto* canonicalRetry =
      canonicalView.findChild<QPushButton*>(QStringLiteral("textAction"));
  QVERIFY(canonicalRetry != nullptr);
  const QString englishRetry = canonicalRetry->text();

  // The backend's line here is exactly the app's own wording for a known code,
  // which is the one case a language switch may re-word.
  mikai::backend::Failure canonical;
  canonical.code = QStringLiteral("not_found");
  canonical.status = 404;
  canonical.message = englishCanonical;
  canonicalView.showFailure(canonical, QStringLiteral("Loading home"));
  QCoreApplication::processEvents();

  QCOMPARE(canonicalView.messageText(),
           QStringLiteral("Loading home: ") + englishCanonical);
  QCOMPARE(canonicalView.codeText(), QStringLiteral("not_found · HTTP 404"));
  QVERIFY(canonicalView.isVisible());

  // The owner changes its clause in place. That must not re-classify the line
  // as the backend's own text, and it must not re-show or re-announce anything.
  canonicalView.setContext(QStringLiteral("Loading library"));
  QCOMPARE(canonicalView.messageText(),
           QStringLiteral("Loading library: ") + englishCanonical);
  QCOMPARE(canonicalView.codeText(), QStringLiteral("not_found · HTTP 404"));
  QVERIFY(canonicalView.isVisible());

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  QCoreApplication::processEvents();

  const QString ukrainianCanonical = mikai::app::localizedApiError(QStringLiteral("not_found"));
  QVERIFY2(ukrainianCanonical != englishCanonical,
           "the catalog translated this known code to the same line, so the re-wording is invisible");
  QCOMPARE(canonicalView.messageText(),
           QStringLiteral("Loading library: ") + ukrainianCanonical);
  QCOMPARE(canonicalView.codeText(), QStringLiteral("not_found · HTTP 404"));
  QVERIFY(canonicalView.isVisible());
  QVERIFY2(canonicalRetry->text() != englishRetry, "the retry caption was not retranslated");
  QCOMPARE(canonicalRetries.count(), 0);

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();
  QCOMPARE(canonicalView.messageText(),
           QStringLiteral("Loading library: ") + englishCanonical);
  QCOMPARE(canonicalRetry->text(), englishRetry);
  QCOMPARE(canonicalRetries.count(), 0);

  // Same known code, but this time the line is the server's own detail. It is
  // not the app's wording and must survive every switch verbatim.
  mikai::app::FailureView detailView;
  QSignalSpy detailRetries(&detailView, &mikai::app::FailureView::retryRequested);
  auto* detailRetry = detailView.findChild<QPushButton*>(QStringLiteral("textAction"));
  QVERIFY(detailRetry != nullptr);
  mikai::backend::Failure detail;
  detail.code = QStringLiteral("http_error");
  detail.status = 522;
  detail.message = QStringLiteral("Cloudflare could not reach the origin");
  detailView.showFailure(detail, QStringLiteral("Loading episode list"));
  detailView.setRetryVisible(false);
  QCoreApplication::processEvents();

  const QString detailLine =
      QStringLiteral("Loading episode list: Cloudflare could not reach the origin");
  QCOMPARE(detailView.messageText(), detailLine);
  QCOMPARE(detailView.codeText(), QStringLiteral("HTTP 522"));
  QVERIFY(detailView.isVisible());
  QVERIFY2(!detailRetry->isVisible(), "the host retired the built-in retry");

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  QCoreApplication::processEvents();
  QCOMPARE(detailView.messageText(), detailLine);
  QCOMPARE(detailView.codeText(), QStringLiteral("HTTP 522"));
  QVERIFY(detailView.isVisible());
  QVERIFY2(!detailRetry->isVisible(),
           "a language switch brought back a retry the host had retired");
  QCOMPARE(detailRetries.count(), 0);

  // A new owner clause still leaves the backend's line and the status chip
  // alone: only the clause the caller owns changes.
  detailView.setContext(QStringLiteral("Refreshing episodes"));
  QCOMPARE(detailView.messageText(),
           QStringLiteral("Refreshing episodes: Cloudflare could not reach the origin"));
  QCOMPARE(detailView.codeText(), QStringLiteral("HTTP 522"));
  QVERIFY(!detailRetry->isVisible());
  QCOMPARE(detailRetries.count(), 0);

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();
  QCOMPARE(detailView.messageText(),
           QStringLiteral("Refreshing episodes: Cloudflare could not reach the origin"));
  QCOMPARE(detailView.codeText(), QStringLiteral("HTTP 522"));
  QVERIFY(!detailRetry->isVisible());
  QCOMPARE(detailRetries.count(), 0);
}

void TestI18n::processingOverlayKeepsItsRunAcrossLanguageSwitches() {
  QVERIFY2(primeBuiltCatalogForSwitching(),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + builtCatalogPath()));

  mplayer::shell::ProcessingOverlay overlay;
  QSignalSpy cancels(&overlay, &mplayer::shell::ProcessingOverlay::cancelRequested);
  overlay.begin(QStringLiteral("Preparing the engine"));
  overlay.setStage(2, 5, QStringLiteral("Building engines"), QStringLiteral("first pass"));
  QCoreApplication::processEvents();

  auto* progress = overlay.findChild<QProgressBar*>();
  auto* action =
      overlay.findChild<QPushButton*>(QStringLiteral("processingOverlayCancel"));
  QVERIFY2(progress != nullptr && action != nullptr,
           "the real overlay exposes its bar and its single action");
  QLabel* title = labelContaining(overlay, QStringLiteral("Preparing the engine"));
  QLabel* detail = labelContaining(overlay, QStringLiteral("Building engines"));
  QVERIFY2(title != nullptr && detail != nullptr, "the overlay shows a title and the stage line");

  const QString runningTitle = title->text();
  const QString runningDetail = detail->text();
  const QString englishCancel = action->text();
  const QString englishCancelAccessible = action->accessibleName();
  const QString englishProgressAccessible = progress->accessibleName();
  QCOMPARE(progress->minimum(), 0);
  QCOMPARE(progress->maximum(), 5);
  QCOMPARE(progress->value(), 2);
  QVERIFY(action->isEnabled());

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  QCoreApplication::processEvents();

  // The caller's words are re-emitted verbatim; the app-owned "Stage n of m"
  // wrapper around them is the only part of the line that may change, and the
  // run's position does not.
  QCOMPARE(title->text(), runningTitle);
  QVERIFY2(detail->text() != runningDetail,
           "the app-owned stage wrapper was not retranslated");
  QVERIFY2(detail->text().contains(QStringLiteral("Building engines")),
           "the caller's phase text was replaced instead of re-wrapped");
  QVERIFY2(detail->text().contains(QStringLiteral("first pass")),
           "the caller's detail text was replaced instead of re-wrapped");
  QCOMPARE(progress->minimum(), 0);
  QCOMPARE(progress->maximum(), 5);
  QCOMPARE(progress->value(), 2);
  QVERIFY(action->isEnabled());
  QVERIFY2(action->text() != englishCancel, "the cancel caption was not retranslated");
  QVERIFY2(action->accessibleName() != englishCancelAccessible,
           "the cancel accessible name was not retranslated");
  QVERIFY2(progress->accessibleName() != englishProgressAccessible,
           "the progress accessible name was not retranslated");
  QCOMPARE(cancels.count(), 0);

  overlay.setOperationTitle(QStringLiteral("Rendering the preview"));
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();
  QCOMPARE(title->text(), QStringLiteral("Rendering the preview"));
  QCOMPARE(detail->text(), runningDetail);
  QCOMPARE(progress->maximum(), 5);
  QCOMPARE(progress->value(), 2);
  QCOMPARE(action->text(), englishCancel);
  QCOMPARE(action->accessibleName(), englishCancelAccessible);
  QCOMPARE(progress->accessibleName(), englishProgressAccessible);
  QCOMPARE(cancels.count(), 0);

  // A cancel already sent is a pending click, not a click the language switch
  // may hand back to the viewer.
  action->click();
  QCOMPARE(cancels.count(), 1);
  QVERIFY(!action->isEnabled());
  const QString pendingMark = action->text();
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  QCoreApplication::processEvents();
  QVERIFY(!action->isEnabled());
  QCOMPARE(action->text(), pendingMark);
  QCOMPARE(cancels.count(), 1);
  QCOMPARE(title->text(), QStringLiteral("Rendering the preview"));
  QVERIFY2(detail->text() != runningDetail,
           "the stage wrapper was not retranslated while a cancel was pending");
  QVERIFY(detail->text().contains(QStringLiteral("Building engines")));
  QVERIFY(detail->text().contains(QStringLiteral("first pass")));
  QCOMPARE(progress->value(), 2);

  // A settled failure stays up and stays settled: switching language rebuilds
  // its title and action, never the run.
  overlay.finish(false, QStringLiteral("The builder refused the plan"));
  QCoreApplication::processEvents();
  QVERIFY(overlay.isVisible());
  QCOMPARE(progress->minimum(), 0);
  QCOMPARE(progress->maximum(), 1);
  QCOMPARE(progress->value(), 0);
  QVERIFY(action->isEnabled());
  QCOMPARE(detail->text(), QStringLiteral("The builder refused the plan"));
  const QString failedTitle = title->text();

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();
  QVERIFY(overlay.isVisible());
  QCOMPARE(title->text(),
           QCoreApplication::translate("mplayer::shell::ProcessingOverlay",
                                       "Engine build stopped"));
  QVERIFY2(title->text() != failedTitle, "the failure title stayed in the previous language");
  QCOMPARE(detail->text(), QStringLiteral("The builder refused the plan"));
  QCOMPARE(action->text(),
           QCoreApplication::translate("mplayer::shell::ProcessingOverlay", "Dismiss"));
  QVERIFY(action->isEnabled());
  QCOMPARE(progress->maximum(), 1);
  QCOMPARE(progress->value(), 0);
  QCOMPARE(cancels.count(), 1);

  overlay.setSummary(QStringLiteral("Rejected at stage 2"));
  const QString englishDismiss = action->text();
  const QString englishDismissAccessible = action->accessibleName();
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  QCoreApplication::processEvents();
  QVERIFY(overlay.isVisible());
  QCOMPARE(detail->text(), QStringLiteral("Rejected at stage 2"));
  // A settled failure is re-worded by the switch: the caption the viewer just
  // read is replaced by this language's caption for the same action.
  QVERIFY2(action->text() != englishDismiss, "the dismiss caption was not retranslated");
  QCOMPARE(action->text(),
           QCoreApplication::translate("mplayer::shell::ProcessingOverlay", "Dismiss"));
  QVERIFY2(action->accessibleName() != englishDismissAccessible,
           "the dismiss accessible name was not retranslated");
  QVERIFY(action->isEnabled());
  QCOMPARE(progress->maximum(), 1);
  QCOMPARE(progress->value(), 0);
  QCOMPARE(cancels.count(), 1);

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();
}

void TestI18n::settingsFlyoutRetranslatesEverySpeedPresetWithoutReselecting() {
  QVERIFY2(primeBuiltCatalogForSwitching(),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + builtCatalogPath()));
  // The installed catalog is process state, so English goes back even when an
  // assertion below ends the test before its last line.
  const auto restoreLanguage =
      qScopeGuard([] { mikai::app::applyInterfaceLanguage(QStringLiteral("en")); });
  Q_UNUSED(restoreLanguage);

  // The popover is reached the way the player reaches it: a child of an overlay
  // host, so the language change arrives as a child event and not as the
  // top-level one.
  QWidget host;
  auto* hostLayout = new QVBoxLayout(&host);
  auto* flyout = new mplayer::shell::SettingsFlyout(&host);
  hostLayout->addWidget(flyout);
  host.show();
  QCoreApplication::processEvents();

  // Every preset the production table offers, in its own order. The numeric rate
  // is what identifies a row without reading the widget's internals, the English
  // is the source a skipped extraction leaves on screen, and the Ukrainian is
  // what the real catalogue must deliver for that row.
  struct Preset {
    const char* rate;
    const char* english;
  };
  static const Preset kPresets[] = {
      {"0.5", "Slow"},
      {"1", "Normal"},
      {"1.2", "Slightly faster"},
      {"1.5", "Fast"},
      {"1.7", "Very fast"},
      {"2", "Double speed"},
  };
  constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
  const auto at = [](int index) { return QString::fromLatin1(kPresets[index].rate); };
  // The table's fifth entry, "1.7x", is the rate this test opens the popover at.
  constexpr int kChecked = 4;

  // A row paints its rate first and its description after it; the checked row
  // carries a mark in front of that number. Dropping everything before the first
  // digit is all this test needs to read a row, and it is what keeps the search
  // below off the checked mark's exact glyph.
  const auto rowFromRate = [](const QPushButton* row) {
    QString text = row->text();
    while (!text.isEmpty() && !text.at(0).isDigit()) {
      text = text.mid(1);
    }
    return text;
  };
  // The speed rows are the popover's own buttons; the quality rows are nested in
  // the quality section. A parent test keeps the two apart without touching any
  // field the popover keeps to itself.
  const auto speedRowFor = [flyout, &rowFromRate](const QString& rate) -> QPushButton* {
    const QString prefix = rate + QStringLiteral("x");
    for (QPushButton* button : flyout->findChildren<QPushButton*>()) {
      if (button->parentWidget() != flyout) {
        continue;
      }
      if (rowFromRate(button).startsWith(prefix)) {
        return button;
      }
    }
    return nullptr;
  };

  // Set a concrete speed so retranslation must preserve the user's selected rate.
  flyout->setRate(1.7);
  flyout->setQuality(QStringLiteral("1080p"));
  struct QualityFixture {
    int height;
    const char* label;
  };
  static const QualityFixture kQualities[] = {
      {1080, "1080p"},
      {720, "720p"},
      {480, "480p"},
  };
  QVector<mplayer::shell::SettingsFlyout::QualityOption> options;
  for (const QualityFixture& entry : kQualities) {
    mplayer::shell::SettingsFlyout::QualityOption option;
    option.height = entry.height;
    option.label = QString::fromLatin1(entry.label);
    option.current = entry.height == 1080;
    options << option;
  }
  flyout->setQualityOptions(options);
  QCoreApplication::processEvents();

  flyout->popupNear(QPoint(240, 240));
  QCoreApplication::processEvents();
  QVERIFY2(flyout->isVisible(), "the popover was not open during the language switch");

  QSignalSpy speedPicks(flyout, &mplayer::shell::SettingsFlyout::speedSelected);
  QSignalSpy qualityPicks(flyout, &mplayer::shell::SettingsFlyout::qualitySelected);

  // The badge is the popover's only label that is exactly the numeric rate; the
  // checked row carries a mark before the same number.
  QLabel* rateBadge = nullptr;
  for (QLabel* label : flyout->findChildren<QLabel*>()) {
    if (label->text() == QStringLiteral("1.7x")) {
      rateBadge = label;
      break;
    }
  }
  auto* qualityLabel = flyout->findChild<QLabel*>(QStringLiteral("playerQualityLabel"));
  auto* qualitySection = flyout->findChild<QWidget*>(QStringLiteral("playerQualitySection"));
  QVERIFY2(rateBadge != nullptr && qualityLabel != nullptr && qualitySection != nullptr,
           "the popover exposes the numeric rate, the quality label and the quality section");
  const QString qualityChoice = qualityLabel->text();
  QCOMPARE(qualityChoice, QStringLiteral("1080p"));
  const auto qualityRowLabels = [qualitySection]() {
    QStringList labels;
    for (QPushButton* button : qualitySection->findChildren<QPushButton*>()) {
      labels << button->text();
    }
    return labels;
  };
  const QStringList qualityRows = qualityRowLabels();
  QCOMPARE(qualityRows.size(), options.size());

  QStringList englishRows;
  for (int i = 0; i < kPresetCount; ++i) {
    QPushButton* row = speedRowFor(at(i));
    QVERIFY2(row != nullptr,
             qPrintable(QStringLiteral("no speed row for %1x").arg(at(i))));
    QVERIFY2(row->text().contains(QString::fromUtf8(kPresets[i].english)),
             qPrintable(QStringLiteral("the English %1x row is not showing its own source: %2")
                            .arg(at(i))
                            .arg(row->text())));
    englishRows << row->text();
  }
  // Only the rate the popover opened at is checked, and it is checked while the
  // popover is still in English.
  QVERIFY2(englishRows.at(kChecked).startsWith(QStringLiteral("✓")),
           qPrintable(englishRows.at(kChecked)));
  for (int i = 0; i < englishRows.size(); ++i) {
    if (i == kChecked) {
      continue;
    }
    QVERIFY2(!englishRows.at(i).startsWith(QStringLiteral("✓")),
             qPrintable(englishRows.at(i)));
  }

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  QCoreApplication::processEvents();

  // Every preset description is restated. A row still carrying its
  // English source is exactly what a catalogue that missed the const char* table
  // produced, and it has to fail here rather than read as a translated row.
  for (int i = 0; i < kPresetCount; ++i) {
    QPushButton* row = speedRowFor(at(i));
    QVERIFY2(row != nullptr,
             qPrintable(QStringLiteral("the %1x row disappeared").arg(at(i))));
    const QString ukrainian = QCoreApplication::translate(
        "mplayer::shell::SettingsFlyout", kPresets[i].english);
    QVERIFY2(!row->text().contains(QString::fromUtf8(kPresets[i].english)),
             qPrintable(QStringLiteral("the %1x row is still English under Ukrainian: %2")
                            .arg(at(i))
                            .arg(row->text())));
    QVERIFY2(row->text().contains(ukrainian),
             qPrintable(
                 QStringLiteral("the %1x row does not carry the catalogue's Ukrainian (%2): %3")
                     .arg(at(i), ukrainian, row->text())));
  }

  // The words changed; the checked rate, the numeric rate, the quality choice
  // and the quality rows' own labels did not, and the restatement never reported
  // a selection to the owner.
  QStringList ukrainianRows;
  for (int i = 0; i < kPresetCount; ++i) {
    QPushButton* row = speedRowFor(at(i));
    QVERIFY(row != nullptr);
    ukrainianRows << row->text();
    QVERIFY2(rowFromRate(row).startsWith(at(i) + QStringLiteral("x")),
             qPrintable(row->text()));
  }
  QVERIFY2(ukrainianRows.at(kChecked).startsWith(QStringLiteral("✓")),
           qPrintable(ukrainianRows.at(kChecked)));
  for (int i = 0; i < ukrainianRows.size(); ++i) {
    if (i == kChecked) {
      continue;
    }
    QVERIFY2(!ukrainianRows.at(i).startsWith(QStringLiteral("✓")),
             qPrintable(ukrainianRows.at(i)));
  }
  QCOMPARE(rateBadge->text(), QStringLiteral("1.7x"));
  QCOMPARE(qualityLabel->text(), qualityChoice);
  QCOMPARE(qualityRowLabels(), qualityRows);
  QCOMPARE(speedPicks.count(), 0);
  QCOMPARE(qualityPicks.count(), 0);
  QVERIFY2(flyout->isVisible(), "the language switch closed the popover");

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();

  for (int i = 0; i < kPresetCount; ++i) {
    QPushButton* row = speedRowFor(at(i));
    QVERIFY(row != nullptr);
    QCOMPARE(row->text(), englishRows.at(i));
  }
  QCOMPARE(rateBadge->text(), QStringLiteral("1.7x"));
  QCOMPARE(qualityLabel->text(), qualityChoice);
  QCOMPARE(qualityRowLabels(), qualityRows);
  QCOMPARE(speedPicks.count(), 0);
  QCOMPARE(qualityPicks.count(), 0);
  QVERIFY2(flyout->isVisible(), "the popover did not survive the round trip");
}

void TestI18n::rateLimitLabelRetranslatesWithoutMovingTheQuotaOrAsking() {
  QVERIFY2(primeBuiltCatalogForSwitching(),
           qPrintable(QStringLiteral("built Ukrainian catalog not found at: ") + builtCatalogPath()));

  QuotaLoopback api;
  QString error;
  QVERIFY2(api.start(&error), qPrintable(error));
  // A fully used window that is still counting down: the tooltip states all
  // three quota facts, so rendering anything but the shown snapshot changes at
  // least one of them.
  api.setQuota(300, 0, 60);

  mikai::backend::MikaiClient client(QStringLiteral("fixture-key"), api.baseUrl());
  client.getSchedule();
  QCOMPARE(api.requestCount(), 1);
  const mikai::backend::RateLimitState answered = client.rateLimitState();
  QCOMPARE(answered.limit, 300);
  QCOMPARE(answered.used.value_or(-1), 300);
  QCOMPARE(answered.remaining.value_or(-1), 0);
  QVERIFY(answered.resetIn > 0);

  // One caller parks inside the limiter, so the queue depth the label reports
  // is a real waiter. The scope releases it at the end of the test instead of
  // waiting out the advertised window.
  std::atomic_bool cancel{false};
  QThread* parked = QThread::create([&client, &cancel] {
    mikai::backend::WorkCancellationScope scope(&cancel);
    try {
      client.getSchedule();
    } catch (const mikai::backend::MikaiApiError& failure) {
      if (!cancel.load()) {
        qFatal("Parked quota request failed before cancellation (%s): %s",
               qPrintable(failure.code), qPrintable(failure.message));
      }
    }
  });
  parked->start();
  const auto releaseParked = qScopeGuard([parked, &cancel] {
    cancel.store(true);
    if (!parked->wait(15000)) qFatal("Cancelled quota request did not unwind");
    delete parked;
  });
  QTRY_VERIFY_WITH_TIMEOUT(client.rateLimitState().queued >= 1, 15000);
  QCOMPARE(api.requestCount(), 1);

  // The chip opens inside a page-like host, the way TitlePage and the catalog
  // use it: the language change reaches it as a child, not as the top-level
  // window that received the event.
  QWidget host;
  auto* hostLayout = new QVBoxLayout(&host);
  auto* label = new mikai::app::RateLimitLabel(client, &host);
  hostLayout->addWidget(label);
  host.show();
  QCoreApplication::processEvents();
  QVERIFY2(label->isVisible(), "the quota label was not open during the switch");
  QLabel* chip = label->findChild<QLabel*>();
  QVERIFY(chip != nullptr);

  const QString englishText = chip->text();
  const QString englishTip = chip->toolTip();
  const QString englishAccessible = chip->accessibleName();
  const QString usedLine = QCoreApplication::translate(
      "mikai::app::RateLimitLabel",
      "Last API response: %1 of %2 requests used in the current window");
  QVERIFY2(englishText.contains(QStringLiteral("300/300")), qPrintable(englishText));
  QVERIFY2(englishTip.contains(usedLine.arg(300).arg(300)), qPrintable(englishTip));
  QVERIFY2(englishTip.contains(QCoreApplication::translate("mikai::app::RateLimitLabel",
                                                           "; %1 remaining").arg(0)),
           qPrintable(englishTip));
  QVERIFY2(englishTip.contains(QCoreApplication::translate("mikai::app::RateLimitLabel",
                                                           "; %1 queued").arg(1)),
           qPrintable(englishTip));

  const int requestsBefore = api.requestCount();
  const mikai::backend::RateLimitState beforeSwitch = client.rateLimitState();
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("uk")));
  QCoreApplication::processEvents();

  // The digits are the quota, not prose, so they stay; the words around them
  // are restated in place.
  QCOMPARE(chip->text(), englishText);
  QVERIFY2(chip->toolTip() != englishTip, "the quota tooltip stayed in the opening language");
  QVERIFY2(chip->toolTip().contains(QCoreApplication::translate(
               "mikai::app::RateLimitLabel",
               "Last API response: %1 of %2 requests used in the current window")
               .arg(300)
               .arg(300)),
           qPrintable(chip->toolTip()));
  QVERIFY2(chip->toolTip().contains(QCoreApplication::translate("mikai::app::RateLimitLabel",
                                                                "; %1 remaining").arg(0)),
           qPrintable(chip->toolTip()));
  QVERIFY2(chip->toolTip().contains(QCoreApplication::translate("mikai::app::RateLimitLabel",
                                                                "; %1 queued").arg(1)),
           qPrintable(chip->toolTip()));
  QVERIFY2(chip->accessibleName() != englishAccessible,
           "the quota accessible name was not restated");

  // Nothing was asked of the server, the quota and the queue did not move, and
  // the countdown kept its own pace rather than restarting or freezing.
  QCOMPARE(api.requestCount(), requestsBefore);
  const mikai::backend::RateLimitState afterSwitch = client.rateLimitState();
  QCOMPARE(afterSwitch.limit, beforeSwitch.limit);
  QCOMPARE(afterSwitch.used.value_or(-1), beforeSwitch.used.value_or(-1));
  QCOMPARE(afterSwitch.remaining.value_or(-1), beforeSwitch.remaining.value_or(-1));
  QCOMPARE(afterSwitch.queued, beforeSwitch.queued);
  QVERIFY(afterSwitch.resetIn > 0);
  QVERIFY(afterSwitch.resetIn <= beforeSwitch.resetIn);
  QVERIFY2(beforeSwitch.resetIn - afterSwitch.resetIn <= 5,
           "the language switch reset the advertised quota window");
  QVERIFY2(label->isVisible(), "the language switch hid the quota label");

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();

  const QString restored = chip->toolTip();
  QCOMPARE(chip->text(), englishText);
  QVERIFY2(restored.contains(usedLine.arg(300).arg(300)), qPrintable(restored));
  QVERIFY2(restored.contains(QCoreApplication::translate("mikai::app::RateLimitLabel",
                                                         "; %1 remaining").arg(0)),
           qPrintable(restored));
  QVERIFY2(restored.contains(QCoreApplication::translate("mikai::app::RateLimitLabel",
                                                         "; %1 queued").arg(1)),
           qPrintable(restored));
  QCOMPARE(api.requestCount(), requestsBefore);
}

void TestI18n::cleanup() {
  // Unconditional English, not the stored preference: a Ukrainian system locale
  // would otherwise hand the next suite a translator and a comma-decimal locale
  // it never asked for. The default locale goes back to what the process found.
  mikai::app::applyInterfaceLanguage(QStringLiteral("en"));
  QLocale::setDefault(QLocale::system());
}
