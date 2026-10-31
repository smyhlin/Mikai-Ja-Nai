#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class QEvent;
class QLabel;
class QPushButton;
class QSpinBox;

namespace mikai::app {

class FlowLayout;

/// The chip label a page of `totalEntries` rows really holds.
///
/// Positional: entries 1..55 on page 1, 56..110 on page 2. A release that
/// numbers its episodes from anywhere uses the overload that takes the labels
/// instead, because a chip reading "1 - 140" above a grid starting at 110 is a
/// lie. One-based `page` throughout, as displayed.
QString pageRangeLabel(int page, int pageSize, int totalEntries);

/// `⏮ ←  [chips]  → ⏭` plus a page box.
///
/// A chip is one page, labelled with the entries that page holds. A window of
/// `kPagerChipWindow` chips follows the current page, the arrows step one page,
/// and the page box jumps straight to one.
///
/// The page count belongs to the caller: the server counts pages at its own page
/// size, so it has to be computed from the total and the page size before it
/// gets here.
class RangePager : public QWidget {
    Q_OBJECT

public:
    explicit RangePager(int pageSize, QWidget* parent = nullptr);
    ~RangePager() override;

    int pageSize() const { return pageSize_; }

    /// Point the pager at `page` of `pages` over a result set of `entries` rows,
    /// labelling chips positionally.
    void setState(int page, int pages, int entries);

    /// As above, with the labels the rows actually show, in order.
    void setState(int page, int pages, int entries, const QStringList& numbers);

    int page() const { return page_; }
    int pages() const { return pages_; }

Q_SIGNALS:
    /// The user asked for another page, 1-based. Never emitted for the page the
    /// pager is already showing.
    void pageRequested(int page);

protected:
    void changeEvent(QEvent* event) override;

private:
    /// Restates every static caption in place after a LanguageChange: the
    /// arrows and the page box, and the current page-total label with the chip
    /// tooltips. The page, the page count and the enabled arrows are read back
    /// from their own state, so a switch never moves the page or asks for one.
    void retranslateUi();
    void step(int delta);
    void jumpTo(int page);
    void renderChips();
    QString chipLabel(int target) const;
    void updateEnabled();

    int pageSize_ = 1;
    int page_ = 1;
    int pages_ = 1;
    int entries_ = 0;
    QStringList numbers_;
    bool syncing_ = false;
    /// False until the first renderChips, so the first setState always applies
    /// even when it repeats the constructor defaults.
    bool rendered_ = false;

    QPushButton* firstButton_ = nullptr;
    QPushButton* prevButton_ = nullptr;
    QWidget* chipsHost_ = nullptr;
    FlowLayout* chipLayout_ = nullptr;
    /// The fixed-size chip window, built once and re-labelled by renderChips.
    QList<QPushButton*> chips_;
    QPushButton* nextButton_ = nullptr;
    QPushButton* lastButton_ = nullptr;
    QLabel* jumpLabel_ = nullptr;
    QSpinBox* jump_ = nullptr;
};

}  // namespace mikai::app
