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
  void contentNamesFollowTheirOwnLanguage();
  void appearanceRoundTripsThroughSettings();
  void unsetRadiusAndResetUseXsWithoutOverwritingSavedChoices();
  void appearanceSaveAndCancelKeepTheLatestBatchedStyle();
  void unparsableAppearanceFallsBackToDefaults();
  void appearancePreviewCancelRollsBackAndSavePersists();
  void playerOverlayFollowsTheApplicationAccent();
  void settingsPagesFitTheNarrowestAllowedWidth();
  void titleNamesFollowTheirOwnLanguageAndFallBack();
  void nativePlanErrorsFollowTheActiveLanguage();
  void failedReplacementPreservesWorkingLanguage();
  void appliedLanguageDrivesDefaultLocaleNumbers();

  /// The behaviour a restart-only language switch cannot produce: an open
  /// settings dialog and its native surfaces restate themselves while every
  /// unsaved value, the selection and the scroll position survive, en -> uk ->
  /// en, and an unavailable choice is refused by name rather than downgraded.
  void openSettingsRetranslatesInPlaceAndKeepsUnsavedValues();
  /// The Processing tab's own refusal names an app-owned reason: with the
  /// private Native runtime unselected in this process the reason is the
  /// producer's wording, not the runtime's. An open Settings dialog must restate
  /// it en -> uk -> en from the typed facts it already holds - the stored
  /// request, the selected backend/recipe and the installed context provider -
  /// without re-reading the request, probing the source or touching the store.
  void nativeUnavailableReasonFollowsTheOpenSettingsLanguage();
  void processingFormsRemainReadableWhenResized_data();
  void processingFormsRemainReadableWhenResized();
  void unavailableLanguageIsRefusedByNameAndKeepsTheWorkingTranslation();
  void cancelChangesNoLanguageAtAll();

  /// The selector changes the running windows before Save, en -> uk -> en
  /// inside one open dialog, and Save commits the choice without a restart
  /// while a reopened dialog neither rolls it back nor needs to reload.
  void dialogPreviewAndSavePersistWithoutRestart();
  /// A Save refused by a fallible write leaves the preview live and writes no
  /// preference; Cancel then rolls the preview back to the opening language.
  void refusedSaveKeepsThePreviewAndCancelRollsBack();
  /// A replacement catalog that cannot be loaded leaves the working language
  /// installed, names the refusal and returns the selector to that choice.
  void refusedCatalogReplacementRestoresTheSelector();

  /// Live widgets own their wording, not a copy of it: switching language must
  /// restate captions and accessibility names while leaving the state the
  /// words describe - the page, the run, the failure - exactly where it was.
  void rangePagerRetranslatesWithoutMovingThePageOrAsking();
  void failureViewRephrasesOnlyItsOwnWordsAndKeepsBackendDetail();
  void processingOverlayKeepsItsRunAcrossLanguageSwitches();
  void pillButtonCaptionFollowsTheLanguage();

  /// The popover's speed presets are one `const char*` table, so the only thing
  /// that makes them translatable is the context-carrying extraction the
  /// catalogue is built from. A catalogue that skipped that table left every row
  /// English while the numbers beside them were already Ukrainian. An open
  /// popover, at a rate that is not the default, must restate all six
  /// descriptions en -> uk -> en and keep the checked rate and the quality
  /// choice where they were, without reporting either as a fresh selection.
  void settingsFlyoutRetranslatesEverySpeedPresetWithoutReselecting();

  /// The quota chip is fed by a real rate-limit answer over loopback and a
  /// genuinely parked request. An open label must restate its tooltip and
  /// accessible name in the new language while the quota, the reset window,
  /// the queue depth, its timer and the request count stay where they were.
  void rateLimitLabelRetranslatesWithoutMovingTheQuotaOrAsking();

  void cleanup();
};
