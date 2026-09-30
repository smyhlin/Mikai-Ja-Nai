#include "widgets/RangePager.hpp"

#include "theme/Metrics.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>

namespace mikai::app {

QString pageRangeLabel(int page, int pageSize, int totalEntries) {
    const int size = pageSize > 0 ? pageSize : 1;
    const int total = totalEntries > 0 ? totalEntries : 0;
    const int start = (page - 1) * size;
    const int end = qMin(page * size, total);
    if (end <= start) {
        return QString::number(start + 1);
    }
    return QStringLiteral("%1 – %2").arg(start + 1).arg(end);
}

namespace {

/// The page a chip points at lives in its property, so one connection built
/// once at construction survives every re-label.
const char* kChipPageProperty = "pagerPage";

QPushButton* makeArrow(const QString& text, const QString& tooltip) {
    auto* button = new QPushButton(text);
    button->setObjectName(QStringLiteral("pagerControl"));
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip);
    return button;
}

}  // namespace

RangePager::RangePager(int pageSize, QWidget* parent)
    : QWidget(parent), pageSize_(qMax(1, pageSize)) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(theme::kSpacingSmall, theme::kSpacingXs, theme::kSpacingSmall,
                            theme::kSpacingXs);
    row->setSpacing(theme::kSpacingSmall);

    firstButton_ = makeArrow(QStringLiteral("⏮"), tr("На першу сторінку"));
    prevButton_ = makeArrow(QStringLiteral("←"), tr("Попередня сторінка"));

    chipsHost_ = new QWidget(this);
    chipLayout_ = new QHBoxLayout(chipsHost_);
    chipLayout_->setContentsMargins(0, 0, 0, 0);
    chipLayout_->setSpacing(theme::kSpacingSmall);

    nextButton_ = makeArrow(QStringLiteral("→"), tr("Наступна сторінка"));
    lastButton_ = makeArrow(QStringLiteral("⏭"), tr("На останню сторінку"));

    jumpLabel_ = new QLabel(this);
    jumpLabel_->setObjectName(QStringLiteral("muted"));

    jump_ = new QSpinBox(this);
    jump_->setRange(1, 1);
    jump_->setToolTip(tr("Перейти на сторінку"));
    jump_->setAccessibleName(tr("Номер сторінки"));

    connect(firstButton_, &QPushButton::clicked, this, [this] { jumpTo(1); });
    connect(prevButton_, &QPushButton::clicked, this, [this] { step(-1); });
    connect(nextButton_, &QPushButton::clicked, this, [this] { step(1); });
    connect(lastButton_, &QPushButton::clicked, this, [this] { jumpTo(pages_); });
    connect(jump_, &QSpinBox::valueChanged, this, [this](int value) {
        if (!syncing_) {
            jumpTo(value);
        }
    });

    row->addWidget(firstButton_);
    row->addWidget(prevButton_);
    row->addStretch();
    row->addWidget(chipsHost_);
    row->addStretch();
    row->addWidget(nextButton_);
    row->addWidget(lastButton_);
    row->addSpacing(theme::kSpacingSmall);
    row->addWidget(jumpLabel_);
    row->addWidget(jump_);
}

RangePager::~RangePager() = default;

void RangePager::setState(int page, int pages, int entries) { setState(page, pages, entries, {}); }

void RangePager::setState(int page, int pages, int entries, const QStringList& numbers) {
    const int clampedPages = qMax(1, pages);
    const int clampedPage = qBound(1, page, clampedPages);
    const int clampedEntries = qMax(0, entries);
    // Every episode page render and every search page change lands here, and
    // re-running the whole update for an unchanged state would re-label, re-role
    // and re-enable the same chips for nothing.
    if (rendered_ && clampedPage == page_ && clampedPages == pages_ &&
        clampedEntries == entries_ && numbers == numbers_) {
        return;
    }
    pages_ = clampedPages;
    page_ = clampedPage;
    entries_ = clampedEntries;
    numbers_ = numbers;
    renderChips();
    updateEnabled();
    jumpLabel_->setText(tr("сторінка з %1").arg(pages_));
    syncing_ = true;
    jump_->setRange(1, pages_);
    jump_->setValue(page_);
    syncing_ = false;
}

void RangePager::step(int delta) { jumpTo(page_ + delta); }

void RangePager::jumpTo(int page) {
    if (page >= 1 && page <= pages_ && page != page_) {
        Q_EMIT pageRequested(page);
    }
}

void RangePager::updateEnabled() {
    const bool hasPrevious = page_ > 1;
    firstButton_->setEnabled(hasPrevious);
    prevButton_->setEnabled(hasPrevious);
    nextButton_->setEnabled(page_ < pages_);
    lastButton_->setEnabled(page_ < pages_);
}

void RangePager::renderChips() {
    // A window of chips follows the current page, centred on it and never
    // running off either end of the range. The buttons are kept between calls
    // and re-labelled: setState lands on every episode page render and every
    // search page change, and building the window afresh there meant a fresh
    // QSS polish and a fresh connect per chip per page.
    const int window = qMin(theme::kPagerChipWindow, pages_);
    // The window shrinks with the page count, and a chip past the window is not
    // part of the pager at all, so it goes now rather than lingering as a hidden
    // button the layout and any child lookup still have to walk past.
    while (chips_.size() > window) {
        QPushButton* extra = chips_.takeLast();
        chipLayout_->removeWidget(extra);
        delete extra;
    }
    while (chips_.size() < window) {
        const int target = chips_.size() + 1;
        auto* chip = new QPushButton(chipsHost_);
        chip->setObjectName(QStringLiteral("pagerChip"));
        chip->setProperty(kChipPageProperty, target);
        connect(chip, &QPushButton::clicked, this, [this, chip] {
            jumpTo(chip->property(kChipPageProperty).toInt());
        });
        chipLayout_->addWidget(chip);
        chips_.append(chip);
    }
    const int first = qBound(0, page_ - 1 - window / 2, qMax(0, pages_ - window));
    for (int offset = 0; offset < window; ++offset) {
        QPushButton* chip = chips_.at(offset);
        const int target = first + offset + 1;
        chip->setProperty(kChipPageProperty, target);
        chip->setText(chipLabel(target));
        chip->setToolTip(tr("Сторінка %1").arg(target));
        const bool active = target == page_;
        if (active != (chip->objectName() == QLatin1String("pagerChipActive"))) {
            chip->setObjectName(active ? QStringLiteral("pagerChipActive")
                                       : QStringLiteral("pagerChip"));
            // The object name selects the QSS rule, so only the chips that
            // actually change role are worth re-polishing.
            chip->style()->unpolish(chip);
            chip->style()->polish(chip);
        }
    }
    rendered_ = true;
}

QString RangePager::chipLabel(int target) const {
    if (numbers_.isEmpty()) {
        return pageRangeLabel(target, pageSize_, entries_);
    }
    const int start = (target - 1) * pageSize_;
    if (start >= numbers_.size()) {
        return numbers_.last();
    }
    const int end = qMin(target * pageSize_, numbers_.size());
    const QString first = numbers_.at(start);
    if (end <= start) {
        return first;
    }
    return QStringLiteral("%1 – %2").arg(first, numbers_.at(end - 1));
}

}  // namespace mikai::app
