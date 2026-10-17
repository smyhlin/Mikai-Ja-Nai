#pragma once

#include <QtTest/QTest>

class TestI18n : public QObject {
  Q_OBJECT

 private slots:
  void explicitPreferenceOverridesSystemLocale();
  void systemPreferenceUsesUkrainianOnlyForUkrainianLocale();
  void invalidPreferenceUsesSystemLocale();

  void realCatalogSwitchesLanguageRepeatedly();
  void liveStringsFollowTheSwitchedLanguageAndRestore();
  void nativePlanErrorsFollowTheActiveLanguage();
  void failedReplacementPreservesWorkingLanguage();
  void appliedLanguageDrivesDefaultLocaleNumbers();

  /// The behaviour a restart-only language switch cannot produce: an open
  /// settings dialog and its native surfaces restate themselves while every
  /// unsaved value, the selection and the scroll position survive, en -> uk ->
  /// en, and an unavailable choice is refused by name rather than downgraded.
  void openSettingsRetranslatesInPlaceAndKeepsUnsavedValues();
  void unavailableLanguageIsRefusedByNameAndKeepsTheWorkingTranslation();
  void cancelChangesNoLanguageAtAll();

  void cleanup();
};
