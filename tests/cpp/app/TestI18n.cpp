#include "TestI18n.hpp"


#include <app/dialogs/SettingsDialog.hpp>
#include <app/dialogs/AnimeJanaiManagerPanel.hpp>
#include <app/support/I18n.hpp>
#include <app/workers/Runner.hpp>
#include <backend/MikaiClient.hpp>
#include <native/bundle/NativePrefs.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>
#include <native/components/AnimeJanaiRuntime.hpp>
#include <player/shell/PlayerShell.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtTest/QTest>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>
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
  auto* managerScroll = dialog.findChild<QScrollArea*>(QStringLiteral("nativeProcessingScroll"));
  QVERIFY(managerScroll != nullptr);
  dialog.resize(dialog.minimumWidth(), dialog.height());
  QCoreApplication::processEvents();
  QTRY_VERIFY2(managerScroll->widget()->width() <= managerScroll->viewport()->width(),
               qPrintable(QStringLiteral("Processing content is %1 px wide in a %2 px viewport.")
                              .arg(managerScroll->widget()->width())
                              .arg(managerScroll->viewport()->width())));
  auto* customModels = manager->findChild<QTableWidget*>(
      QStringLiteral("animeJanaiCustomModelsTable"));
  QVERIFY(customModels != nullptr);
  QTRY_COMPARE(customModels->horizontalScrollBar()->maximum(), 0);

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
  // The Processing tab is found by identity, not by being last: a fourth tab
  // added after it would otherwise be selected while the assertions below still
  // describe the processing surface, and the scroll comparison would then be
  // reading a page the test never meant to open.
  int processingIndex = -1;
  for (int index = 0; index < tabs->count(); ++index) {
    if (tabs->widget(index) == scroll) {
      processingIndex = index;
      break;
    }
  }
  QVERIFY2(processingIndex >= 0, "the processing scroll area is the page of exactly one tab");
  tabs->setCurrentIndex(processingIndex);
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
                       tabs->currentIndex() == processingIndex ? QStringLiteral("processing")
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
  QTRY_VERIFY(managerScroll->widget()->width() <= managerScroll->viewport()->width());
  QTRY_COMPARE(customModels->horizontalScrollBar()->maximum(), 0);
  QCOMPARE(storedValues(), storedBefore);
  QVERIFY(mikai::app::applyInterfaceLanguage(QStringLiteral("en")));
  QCoreApplication::processEvents();
  QVERIFY(!privateAssets->isVisibleTo(&dialog));
  QVERIFY(manager->isVisibleTo(&dialog));
  QTRY_VERIFY2(managerScroll->widget()->width() <= managerScroll->viewport()->width(),
               qPrintable(QStringLiteral("Processing content is %1 px wide in a %2 px viewport.")
                              .arg(managerScroll->widget()->width())
                              .arg(managerScroll->viewport()->width())));
  QVERIFY(manager->currentConfig() == managerBefore);
  QVERIFY(manager->isDirty());
  QCOMPARE(storedValues(), storedBefore);
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
  auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("settingsTabs"));
  QVERIFY(tabs != nullptr);
  for (int index = 0; index < tabs->count(); ++index) {
    if (tabs->tabText(index) == mikai::app::SettingsDialog::tr("Benchmark")) {
      tabs->setCurrentIndex(index);
      break;
    }
  }
  const QSize compactSize(width, dialog.minimumHeight());
  dialog.resize(compactSize);
  dialog.show();
  QTRY_COMPARE(dialog.size(), compactSize);
  auto* footer = dialog.findChild<QWidget*>(QStringLiteral("settingsFooter"));
  QVERIFY(footer != nullptr);
  QVERIFY(dialog.rect().contains(QRect(footer->mapTo(&dialog, QPoint()), footer->size())));
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
    if (scroll->widget()->width() > scroll->viewport()->width()) {
      return QStringLiteral("Processing content is %1 px wide in a %2 px viewport.")
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
