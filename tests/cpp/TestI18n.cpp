#include "TestI18n.hpp"


#include <app/dialogs/SettingsDialog.hpp>
#include <app/I18n.hpp>
#include <app/workers/Runner.hpp>
#include <backend/MikaiClient.hpp>
#include <native/bundle/NativePrefs.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>
#include <player/PlayerShell.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtTest/QTest>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QWidget>

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

  auto* threshold = dialog.findChild<QLineEdit*>(QStringLiteral("nativeRifeSceneThresholdEdit"));
  auto* factorNum = dialog.findChild<QSpinBox*>(QStringLiteral("nativeRifeFactorNumSpin"));
  auto* factorDen = dialog.findChild<QSpinBox*>(QStringLiteral("nativeRifeFactorDenSpin"));
  auto* backend = dialog.findChild<QComboBox*>(QStringLiteral("nativeBackendCombo"));
  auto* recipe = dialog.findChild<QComboBox*>(QStringLiteral("nativeRecipeCombo"));
  auto* runtime = dialog.findChild<QComboBox*>(QStringLiteral("runtimeChoiceCombo"));
  auto* storeButton = dialog.findChild<QPushButton*>(QStringLiteral("nativeRifeStoreButton"));
  auto* status = dialog.findChild<QLabel*>(QStringLiteral("nativeRifeStatus"));
  auto* assetsPanel = dialog.findChild<QWidget*>(QStringLiteral("nativeAssetsPanel"));
  auto* tabs = dialog.findChild<QTabWidget*>();
  auto* scroll = dialog.findChild<QScrollArea*>(QStringLiteral("nativeProcessingScroll"));
  QVERIFY2(threshold != nullptr && factorNum != nullptr && factorDen != nullptr &&
               backend != nullptr && recipe != nullptr && runtime != nullptr &&
               storeButton != nullptr && status != nullptr && assetsPanel != nullptr &&
               tabs != nullptr && scroll != nullptr,
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
  tabs->setCurrentIndex(tabs->count() - 1);
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
                       tabs->currentIndex() == tabs->count() - 1 ? QStringLiteral("processing")
                                                                 : QStringLiteral("other")};
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

  QVERIFY2(storeButton->text() != englishSaveText,
           "the RIFE store button still shows the language the dialog opened in");
  QVERIFY2(status->text() != englishStatus, "the RIFE status was not retranslated");
  QVERIFY2(dialog.windowTitle() != englishTitle, "the dialog title was not retranslated");
  QCOMPARE(storedValues(), storedBefore);

  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();
  QCOMPARE(capture(), before);
  QCOMPARE(playButton->toolTip(), englishPlayTooltip);
  QCOMPARE(storedValues(), storedBefore);
  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));
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
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));

  QSettings settings;
  const QString stored = settings.value(QString::fromLatin1(mikai::app::kUiLanguageSettingKey),
                                        QStringLiteral("system"))
                              .toString();

  mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
  mikai::app::Runner runner;
  mikai::app::SettingsDialog dialog(client, runner);
  auto* language = dialog.findChild<QComboBox*>(QStringLiteral("uiLanguageCombo"));
  QVERIFY(language != nullptr);
  const int ukrainian = language->findData(QStringLiteral("uk"));
  QVERIFY(ukrainian >= 0);
  language->setCurrentIndex(ukrainian);
  dialog.reject();
  QCoreApplication::processEvents();

  QCOMPARE(mikai::app::activeInterfaceLanguage(), QStringLiteral("en"));
  QCOMPARE(settings.value(QString::fromLatin1(mikai::app::kUiLanguageSettingKey),
                          QStringLiteral("system"))
               .toString(),
           stored);
}

void TestI18n::cleanup() {
  // Unconditional English, not the stored preference: a Ukrainian system locale
  // would otherwise hand the next suite a translator and a comma-decimal locale
  // it never asked for. The default locale goes back to what the process found.
  mikai::app::applyInterfaceLanguage(QStringLiteral("en"));
  QLocale::setDefault(QLocale::system());
}
