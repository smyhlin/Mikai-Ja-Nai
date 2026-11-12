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
  void defaultGradientUsesBlueAccentWhileCustomGradientRemainsDistinct();
  void unsetRadiusAndResetUseXsWithoutOverwritingSavedChoices();
  void appearanceSaveAndCancelKeepTheLatestBatchedStyle();
  void unparsableAppearanceFallsBackToDefaults();
  void appearancePreviewCancelRollsBackAndSavePersists();
  void settingsPagesFitTheNarrowestAllowedWidth();
  void titleNamesFollowTheirOwnLanguageAndFallBack();
  void nativePlanErrorsFollowTheActiveLanguage();
  void failedReplacementPreservesWorkingLanguage();
  void hdrPlaybackSaveAndCancelRespectDeferredChanges();
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

  /// The canonical Quick player restates its live controls en -> uk -> en while
  /// the real VideoSession remains paused at the same position and duration.
  void playerQuickRetranslatesWithoutResettingPlayback();

  /// The quota chip is fed by a real rate-limit answer over loopback and a
  /// genuinely parked request. An open label must restate its tooltip and
  /// accessible name in the new language while the quota, the reset window,
  /// the queue depth, its timer and the request count stay where they were.
  void rateLimitLabelRetranslatesWithoutMovingTheQuotaOrAsking();

  /// The main page presents the same quota snapshot as a pill with a status
  /// dot. The dot may only report health once an answer carried quota figures;
  /// a default limit with no answer behind it stays neutral, and a spent window
  /// never reads healthy. The shared widget's inline default stays dot-free for
  /// the catalogue and title page.
  void rateLimitPillSeparatesNoDataHealthAndAnExhaustedWindow();

  void cleanup();
};
