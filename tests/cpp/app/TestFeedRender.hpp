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
  void interpolationOnlyRunsWaitForTheirSelectedSlot_data();
  void interpolationOnlyRunsWaitForTheirSelectedSlot();
  void settingsClosingDuringAcquisitionRestoresMainWindowInput();

  // A title page's episode pills come from the release payload. When it answers,
  // every canned episode must have a button, and the heading must be the title's.
  void theTitlePageRendersItsEpisodes();

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
