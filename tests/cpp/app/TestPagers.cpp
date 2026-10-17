#include "TestPagers.hpp"

#include <pages/EpisodeGrid.hpp>
#include <theme/Metrics.hpp>
#include <widgets/RangePager.hpp>

#include <QtCore/QStringList>
#include <QtTest/QSignalSpy>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QWidget>

using mikai::app::RangePager;
using mikai::app::layoutEpisodeGrid;
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
  // The API serves at most 50 rows a page and strides pages at the size it
  // served, so the client asks for 50 and its ranges must follow that.
  QCOMPARE(mikai::app::theme::kSearchResultsPerPage, 50);
  QCOMPARE(pageRangeLabel(1, 50, 132), QStringLiteral("1 – 50"));
  QCOMPARE(pageRangeLabel(2, 50, 132), QStringLiteral("51 – 100"));
  QCOMPARE(pageRangeLabel(3, 50, 132), QStringLiteral("101 – 132"));
}

void TestPagers::searchRangeLabelClampsTheLastPageToTheTotal() {
  // 132 rows in pages of 50: the third page holds 32, not 50, and the chip must
  // say so rather than claim rows the list will not show.
  QCOMPARE(pageRangeLabel(3, 50, 132), QStringLiteral("101 – 132"));
  QCOMPARE(pageRangeLabel(2, 50, 100), QStringLiteral("51 – 100"));
  QCOMPARE(pageRangeLabel(1, 50, 50), QStringLiteral("1 – 50"));
}

void TestPagers::searchRangeLabelHandlesAnEmptyResultSet() {
  // An empty result set collapses to the single row the page would hold rather
  // than the "1 - 0" range a naive start/end computation prints.
  QCOMPARE(pageRangeLabel(1, 50, 0), QStringLiteral("1"));
  QCOMPARE(pageRangeLabel(1, 50, 1), QStringLiteral("1 – 1"));
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
  RangePager pager(mikai::app::theme::kSearchResultsPerPage);
  pager.setState(1, 3, 132);
  QCOMPARE(chipCount(pager), 3);
  QCOMPARE(chipLabels(pager), (QStringList{QStringLiteral("1 – 50"), QStringLiteral("51 – 100"),
                                          QStringLiteral("101 – 132")}));
  // A 132-title answer is three pages at the size the API serves, whatever the
  // server itself counted pages at.
  QCOMPARE(pager.pageSize(), mikai::app::theme::kSearchResultsPerPage);
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
  RangePager pager(mikai::app::theme::kSearchResultsPerPage);
  // 4000 rows at 50 a page is 80 pages, so only a window of chips is shown and
  // it has to follow the current page with the active chip in the middle.
  pager.setState(40, 80, 4000);
  QCOMPARE(chipCount(pager), mikai::app::theme::kPagerChipWindow);
  QCOMPARE(mikai::app::theme::kPagerChipWindow, 7);
  const QStringList labels = chipLabels(pager);
  QCOMPARE(static_cast<int>(labels.size()), 7);
  QCOMPARE(labels.first(), QStringLiteral("1801 – 1850"));
  QCOMPARE(labels.last(), QStringLiteral("2101 – 2150"));
}

void TestPagers::pagerWindowClampsAtTheEnd() {
  RangePager pager(mikai::app::theme::kSearchResultsPerPage);
  // The last pages have no room to centre the active chip, so the window slides
  // back instead of running off the end of the page list.
  pager.setState(80, 80, 4000);
  QCOMPARE(chipCount(pager), 7);
  const QStringList labels = chipLabels(pager);
  QCOMPARE(labels.last(), QStringLiteral("3951 – 4000"));
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

// ── EpisodeGrid ─────────────────────────────────────────────────────────
//
// The bug these pin: reflowGrid used to open with `if (columns ==
// state.columns) return;` while renderTabPage had just emptied the layout and
// rebuilt the buttons. On any re-render at an unchanged width the guard fired,
// nothing was ever added back, and the grid went blank - the pill row vanished
// while the pager still advanced. The guard was reachable from two callers, only
// one of which had invalidated what it was guarding.
//
// These call the real layoutEpisodeGrid, so they go red if the "populate" step
// is ever made conditional again, however it comes to be spelled.

int rowOf(QGridLayout* grid, int index) {
  int row = -1, column = -1, rowSpan = -1, columnSpan = -1;
  grid->getItemPosition(index, &row, &column, &rowSpan, &columnSpan);
  return row;
}
int columnOf(QGridLayout* grid, int index) {
  int row = -1, column = -1, rowSpan = -1, columnSpan = -1;
  grid->getItemPosition(index, &row, &column, &rowSpan, &columnSpan);
  return column;
}

// A grid and `count` buttons, the way a tab builds one page of pills.
struct EpisodeGridFixture {
  QWidget host;
  QGridLayout* grid = nullptr;
  QVector<QWidget*> buttons;

  explicit EpisodeGridFixture(int count) : grid(new QGridLayout(&host)) {
    for (int i = 0; i < count; ++i) {
      auto* button = new QPushButton(QString::number(i + 1), &host);
      buttons.append(button);
    }
  }
};

void TestPagers::episodeGridLaysOutAPageOfButtons() {
  EpisodeGridFixture fixture(12);
  layoutEpisodeGrid(fixture.grid, fixture.buttons, 4);
  QCOMPARE(fixture.grid->count(), 12);
  // 4 per row means the twelfth pill sits on the third row, first column.
  QCOMPARE(rowOf(fixture.grid, 11), 2);
  QCOMPARE(columnOf(fixture.grid, 11), 3);
  // And the first row really is the first four, in order.
  QCOMPARE(rowOf(fixture.grid, 0), 0);
  QCOMPARE(columnOf(fixture.grid, 3), 3);
}

void TestPagers::episodeGridRerendersAtTheSameColumnCount() {
  // THE REGRESSION. Emptied and rebuilt at an unchanged width, the grid must
  // still end up holding the page. A guard that skips the work here is the
  // whole defect: before the fix this left count() at 0.
  EpisodeGridFixture fixture(12);
  layoutEpisodeGrid(fixture.grid, fixture.buttons, 4);
  QCOMPARE(fixture.grid->count(), 12);

  // What renderTabPage does between renders: drop the old pills, build new ones.
  while (QLayoutItem* stale = fixture.grid->takeAt(0)) {
    delete stale;
  }
  QCOMPARE(fixture.grid->count(), 0);
  QVector<QWidget*> fresh;
  for (QWidget* button : fixture.buttons) {
    fresh.append(new QPushButton(qobject_cast<QPushButton*>(button)->text(), &fixture.host));
  }

  // Same column count as last time. The populate must still happen.
  layoutEpisodeGrid(fixture.grid, fresh, 4);
  QCOMPARE(fixture.grid->count(), 12);
  QCOMPARE(rowOf(fixture.grid, 11), 2);
  // The pills on screen are this render's, not the ones it replaced.
  QCOMPARE(qobject_cast<QPushButton*>(fixture.grid->itemAt(0)->widget()), fresh.at(0));
  QCOMPARE(qobject_cast<QPushButton*>(fixture.grid->itemAt(11)->widget()), fresh.at(11));
}

void TestPagers::episodeGridSwapsTheVisiblePage() {
  // Paging hands the function a different slice of the same entry list. The
  // grid must show exactly that slice - not the union with the old page, which
  // is the other half of "a pill from the previous page must never still be a
  // click target".
  EpisodeGridFixture fixture(300);
  QVector<QWidget*> firstPage;
  for (int i = 0; i < 140; ++i) {
    firstPage.append(fixture.buttons.at(i));
  }
  layoutEpisodeGrid(fixture.grid, firstPage, 14);
  QCOMPARE(fixture.grid->count(), 140);

  QVector<QWidget*> secondPage;
  for (int i = 140; i < 280; ++i) {
    secondPage.append(fixture.buttons.at(i));
  }
  layoutEpisodeGrid(fixture.grid, secondPage, 14);
  QCOMPARE(fixture.grid->count(), 140);
  QCOMPARE(qobject_cast<QPushButton*>(fixture.grid->itemAt(0)->widget()), secondPage.at(0));
  QCOMPARE(qobject_cast<QPushButton*>(fixture.grid->itemAt(139)->widget()), secondPage.at(139));
  // A pill from the page that just left is gone from the layout.
  for (int i = 0; i < fixture.grid->count(); ++i) {
    QVERIFY(!firstPage.contains(fixture.grid->itemAt(i)->widget()));
  }
}

void TestPagers::episodeGridSurvivesANullPointerAndAZeroWidth() {
  // A grid that has not been sized yet computes a column count of 0, and a
  // page slice can contain a null button. Neither may divide by zero or leave
  // a half-built layout; the resolved column count clamps to 1.
  EpisodeGridFixture fixture(3);
  layoutEpisodeGrid(nullptr, fixture.buttons, 4);

  layoutEpisodeGrid(fixture.grid, fixture.buttons, 0);
  QCOMPARE(fixture.grid->count(), 3);
  for (int i = 0; i < 3; ++i) {
    QCOMPARE(columnOf(fixture.grid, i), 0);
  }

  QVector<QWidget*> withHole = {fixture.buttons.at(0), nullptr, fixture.buttons.at(2)};
  layoutEpisodeGrid(fixture.grid, withHole, 2);
  QCOMPARE(fixture.grid->count(), 2);
}
