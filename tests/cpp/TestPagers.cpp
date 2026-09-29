#include "TestPagers.hpp"

#include <theme/Metrics.hpp>
#include <widgets/RangePager.hpp>

#include <QtCore/QStringList>
#include <QtTest/QSignalSpy>
#include <QtWidgets/QPushButton>

using mikai::app::RangePager;
using mikai::app::pageRangeLabel;

namespace {

// The four paging arrows carry glyph labels; every other button on the pager is
// a range chip. Reading chips this way keeps the test off the widget's internals
// while still asserting the labels a user reads.
bool isArrowLabel(const QString& text) {
  return text == QStringLiteral("⏮") || text == QStringLiteral("←") ||
         text == QStringLiteral("→") || text == QStringLiteral("⏭");
}

QPushButton* arrowButton(const RangePager& pager, const QString& glyph) {
  const QList<QPushButton*> buttons = pager.findChildren<QPushButton*>();
  for (QPushButton* button : buttons) {
    if (button->text() == glyph) {
      return button;
    }
  }
  return nullptr;
}

QPushButton* chipAt(const RangePager& pager, int index) {
  QList<QPushButton*> chips;
  const QList<QPushButton*> buttons = pager.findChildren<QPushButton*>();
  for (QPushButton* button : buttons) {
    if (!isArrowLabel(button->text())) {
      chips << button;
    }
  }
  return index < chips.size() ? chips.at(index) : nullptr;
}

int chipCount(const RangePager& pager) {
  int count = 0;
  const QList<QPushButton*> buttons = pager.findChildren<QPushButton*>();
  for (QPushButton* button : buttons) {
    if (!isArrowLabel(button->text())) {
      ++count;
    }
  }
  return count;
}

QStringList chipLabels(const RangePager& pager) {
  QStringList labels;
  const QList<QPushButton*> buttons = pager.findChildren<QPushButton*>();
  for (QPushButton* button : buttons) {
    if (!isArrowLabel(button->text())) {
      labels << button->text();
    }
  }
  return labels;
}

}  // namespace

void TestPagers::searchRangeLabelCountsFiftyFivePerPage() {
  // The search pager holds SEARCH_RESULTS_PER_PAGE rows; a 132-title answer at
  // the live API's own page size must still be cut into 55-row pages.
  QCOMPARE(mikai::app::theme::kSearchResultsPerPage, 55);
  QCOMPARE(pageRangeLabel(1, 55, 132), QStringLiteral("1 – 55"));
  QCOMPARE(pageRangeLabel(2, 55, 132), QStringLiteral("56 – 110"));
  QCOMPARE(pageRangeLabel(3, 55, 132), QStringLiteral("111 – 132"));
}

void TestPagers::searchRangeLabelClampsTheLastPageToTheTotal() {
  // 132 rows in pages of 55: the third page holds 22, not 55, and the chip must
  // say so rather than claim rows the list will not show.
  QCOMPARE(pageRangeLabel(3, 55, 132), QStringLiteral("111 – 132"));
  QCOMPARE(pageRangeLabel(2, 55, 100), QStringLiteral("56 – 100"));
  QCOMPARE(pageRangeLabel(1, 55, 55), QStringLiteral("1 – 55"));
}

void TestPagers::searchRangeLabelHandlesAnEmptyResultSet() {
  // An empty result set collapses to the single row the page would hold rather
  // than the "1 - 0" range a naive start/end computation prints.
  QCOMPARE(pageRangeLabel(1, 55, 0), QStringLiteral("1"));
  QCOMPARE(pageRangeLabel(1, 55, 1), QStringLiteral("1 – 1"));
}

void TestPagers::episodeRangeLabelCountsOneFortyPerPage() {
  // The episode grid holds EPISODES_PER_PAGE pills, so a 24-episode season is a
  // single page and the pager stays hidden.
  QCOMPARE(mikai::app::theme::kEpisodesPerPage, 140);
  QCOMPARE(pageRangeLabel(1, 140, 24), QStringLiteral("1 – 24"));
  QCOMPARE(pageRangeLabel(1, 140, 300), QStringLiteral("1 – 140"));
  QCOMPARE(pageRangeLabel(2, 140, 300), QStringLiteral("141 – 280"));
  QCOMPARE(pageRangeLabel(3, 140, 300), QStringLiteral("281 – 300"));
}

void TestPagers::pagerLabelsEachPageWithItsRange() {
  RangePager pager(55);
  pager.setState(1, 3, 132);
  QCOMPARE(chipCount(pager), 3);
  QCOMPARE(chipLabels(pager), (QStringList{QStringLiteral("1 – 55"), QStringLiteral("56 – 110"),
                                          QStringLiteral("111 – 132")}));
  // A 132-title answer is three pages at 55 a page, whatever the server counted.
  QCOMPARE(pager.pageSize(), 55);
}

void TestPagers::pagerUsesRealNumbersWhenGiven() {
  RangePager pager(140);
  // A later cour can start at 110; a chip reading "1 – 140" above a grid
  // starting at 110 is a lie about what is on screen.
  QStringList numbers;
  for (int i = 0; i < 280; ++i) {
    numbers << QString::number(110 + i);
  }
  pager.setState(1, 2, 280, numbers);
  QCOMPARE(chipLabels(pager), (QStringList{QStringLiteral("110 – 249"), QStringLiteral("250 – 389")}));
}

void TestPagers::pagerWindowScrollsWithTheCurrentPage() {
  RangePager pager(55);
  // 4000 rows at 55 a page is 73 pages, so only a window of chips is shown and
  // it has to follow the current page with the active chip in the middle.
  pager.setState(40, 73, 4000);
  QCOMPARE(chipCount(pager), mikai::app::theme::kPagerChipWindow);
  QCOMPARE(mikai::app::theme::kPagerChipWindow, 7);
  const QStringList labels = chipLabels(pager);
  QCOMPARE(static_cast<int>(labels.size()), 7);
  QCOMPARE(labels.first(), QStringLiteral("1981 – 2035"));
  QCOMPARE(labels.last(), QStringLiteral("2311 – 2365"));
}

void TestPagers::pagerWindowClampsAtTheEnd() {
  RangePager pager(55);
  // The last pages have no room to centre the active chip, so the window slides
  // back instead of running off the end of the page list.
  pager.setState(73, 73, 4000);
  QCOMPARE(chipCount(pager), 7);
  const QStringList labels = chipLabels(pager);
  QCOMPARE(labels.last(), QStringLiteral("3961 – 4000"));
}

void TestPagers::pagerEmitsTheRequestedPage() {
  RangePager pager(55);
  QSignalSpy spy(&pager, &RangePager::pageRequested);
  pager.setState(1, 3, 132);
  QPushButton* third = chipAt(pager, 2);
  QVERIFY(third);
  third->click();
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.takeFirst().at(0).toInt(), 3);
}

void TestPagers::pagerArrowEnablesAtTheEdges() {
  RangePager pager(55);
  pager.setState(1, 3, 132);
  QPushButton* first = arrowButton(pager, QStringLiteral("⏮"));
  QPushButton* previous = arrowButton(pager, QStringLiteral("←"));
  QPushButton* next = arrowButton(pager, QStringLiteral("→"));
  QPushButton* last = arrowButton(pager, QStringLiteral("⏭"));
  QVERIFY(first && previous && next && last);
  QVERIFY(!first->isEnabled());
  QVERIFY(!previous->isEnabled());
  QVERIFY(next->isEnabled());
  QVERIFY(last->isEnabled());

  pager.setState(3, 3, 132);
  QVERIFY(first->isEnabled());
  QVERIFY(previous->isEnabled());
  QVERIFY(!next->isEnabled());
  QVERIFY(!last->isEnabled());
}
