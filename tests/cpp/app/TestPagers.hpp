// The range-chip pager: one chip per page, labelled with the entries that page
// actually holds.
//
// The two label paths are both exercised because they disagree on purpose: the
// search pager is positional (a 132-title answer at 55 a page reads
// "111 – 132"), while the episode pager takes real numbers so a cour starting
// at 110 reads "110 – 249" rather than lying about its own grid.

#pragma once

#include <QtTest/QTest>

class TestPagers : public QObject {
  Q_OBJECT

 private slots:
  // pageRangeLabel (the label math, no widget).
  void searchRangeLabelCountsFiftyFivePerPage();
  void searchRangeLabelClampsTheLastPageToTheTotal();
  void searchRangeLabelHandlesAnEmptyResultSet();
  void episodeRangeLabelCountsOneFortyPerPage();

  // RangePager widget.
  void pagerLabelsEachPageWithItsRange();
  void pagerUsesRealNumbersWhenGiven();
  void pagerWindowScrollsWithTheCurrentPage();
  void pagerWindowClampsAtTheEnd();
  void pagerWrapsRangeCaptionsInsteadOfWideningThePage();
  void pagerEmitsTheRequestedPage();
  void pagerArrowEnablesAtTheEdges();

  // EpisodeGrid: the layout a re-render rebuilds, at a width that did not
  // change. This is the regression the pill grid actually shipped with.
  void episodeGridLaysOutAPageOfButtons();
  void episodeGridRerendersAtTheSameColumnCount();
  void episodeGridSwapsTheVisiblePage();
  void episodeGridSurvivesANullPointerAndAZeroWidth();
};
