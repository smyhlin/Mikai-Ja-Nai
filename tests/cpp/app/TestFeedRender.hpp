// The home feed and a title page, rendered from canned payloads over a real
// loopback socket.
//
// Every other suite here tests a boundary in isolation, and every probe that
// drives a page talks to the live API. That left the one question a user
// actually asks - "does the app show the feed, and does a title page show its
// episodes?" - with no test at all: a page that rendered nothing, or that sat on
// its loading text forever, passed everything. These slots answer it without the
// network, so a client-side render regression fails here even when the API is
// down.
//
// The server is a QTcpServer on 127.0.0.1 inside this process. It serves the
// envelope the parsers expect, or a Cloudflare-style 522 when the slot wants the
// failure direction; nothing reaches the internet.

#pragma once

#include <QtTest/QTest>

class TestFeedRender : public QObject {
  Q_OBJECT

 private slots:
  // The feed's blocks are driven by /anime. When it answers, the page must hold
  // real cards, not a retry affordance.
  void theHomeFeedRendersItsCards();
  void benchmarkTabKeepsGroupedModelAndClipSelectionsConsistent();
  // The benchmark's model search filters by model and by group, case
  // insensitively, and clearing it restores every row and every selection.
  void benchmarkModelSearchFiltersRowsAndRestoresSelections();
  void buildLogDetectionDistinguishesCompilationFromOtherBackendWork();
  void theEngineBuildGateWithholdsMeasurementUntilTheSlotIsConfigured();
  void anAuthoritativeBackgroundBuildFailureSurfacesItsBoundedLogTail();
  void interpolationOnlyRunsWaitForTheirSelectedSlot_data();
  void interpolationOnlyRunsWaitForTheirSelectedSlot();
  void settingsClosingDuringAcquisitionRestoresMainWindowInput();

  // A title page's episode pills come from the release payload and live in the
  // page's one body scroll. When it answers, every canned episode must have a
  // button, the heading must be the title's, and the episodes section must be
  // inside the same scroll as the hero instead of a detached inner viewport.
  void theTitlePageRendersItsEpisodes();

  /// The one body scroll, at both the compact and the wide width: every section
  /// fits the content column without horizontal clipping, and the related
  /// carousels, the episodes heading and the episode pills are all reachable by
  /// scrolling it. A section trapped in the hero's text column or a body wider
  /// than its viewport fails here.
  void titlePageBodyScrollsItsSectionsWithoutHorizontalClipping_data();
  void titlePageBodyScrollsItsSectionsWithoutHorizontalClipping();

  /// The content column fills the viewport until the page's max width and is
  /// centred beyond it, and the header row inside the full-width header follows
  /// the same cap. A column given a stretch share of the width fails at both
  /// widths.
  void titlePageContentColumnFillsItsViewportUpToTheCap_data();
  void titlePageContentColumnFillsItsViewportUpToTheCap();

  /// The details sheet is dismissed where it opened: Escape, its close action and
  /// its scrim each hide it without asking the page to go back. With the sheet
  /// open and the description expanded over a selected release, a live language
  /// switch re-words the app's captions but keeps the sheet, the release
  /// selection and the expanded text, and asks the backend for nothing.
  void titleDetailsSheetDismissesInPlaceAndKeepsStateAcrossLanguageSwitch();

  /// Watch is a real action on the active provider: after switching to the
  /// second canned release, pressing it resolves that release's first episode
  /// embed and not the first release's.
  void theTitlePageWatchActionFollowsTheSelectedRelease();

  void episodeMarksPersistAcrossReopenAndViewChanges();
  void episodeContextMenuActionsUpdateOnlyTheOrderedPreviousEpisodes();
  void episodeContextMenuHandlesPagingAndSpecialIdentifiers();
  void titlePageRendersDeclaredEpisodesTheReleaseLacks();
  void titlePageKeepsSpecialEpisodeIdentifiers();
  void clickingAnEpisodeDoesNotAdvanceTheCurrentEpisode();
  void episodeMarksSurvivePagingAndLanguageSwitch();

  /// A loaded search result set, switched en -> uk -> en from the real Settings
  /// selector: the captions and the status line restate themselves, while the
  /// query, the active format filter, the result rows, the selected row and the
  /// scroll position stay where they were and the canned server is not asked
  /// again. A filtered answer keeps its bar and its All escape on screen; only
  /// pressing All issues the one new unfiltered search. Titles and formats are
  /// API data and must read the same in both languages.
  void loadedResultsRetranslateWithoutRefetchingOrLosingViewState();

  /// A loaded title page, switched en -> uk -> en: the description and details
  /// disclosures stay open, the release chip and the episode page keep their
  /// selections, the body scroll does not move, the API's own description and
  /// title stay verbatim, and no request is issued merely to translate.
  void loadedTitlePageRetranslatesWithoutRefetchingOrLosingDisclosureState();

  // The other direction, and the reason this suite exists: a backend that fails
  // must produce a visible failure on both pages. A page that stays on its
  // loading text - which is what a dead API looked like - fails here.
  void aFailedBackendShowsAVisibleFailureInsteadOfAnEmptyPage();

  // The chip's contract at the transport boundary: under a dead origin every
  // request the pages make must come back as an http_error that keeps the
  // status, so a 522 reads 522 however the block reached the backend.
  void everyRouteReportsTheStatusUnderADeadOrigin();

  // The format tab must ask the server, so the result set, its total and its
  // page count are the filtered ones rather than the one page in hand.
  void theFormatFilterIsAskedOfTheServer();

  // The pager must count pages the way the server strides them, not the way the
  // client asked, or the tail of a result set has no page to sit on.
  void thePagerTrustsTheServersOwnPageCount();
};

/// Standalone entry point for the mikai_tests runner; the class above carries
/// the suite so it can also be driven by QTest directly.
int runTestFeedRender(int argc, char** argv);
