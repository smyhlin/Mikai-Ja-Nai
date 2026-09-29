#include "widgets/RangePager.hpp"

#include "theme/Metrics.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>

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
    pages_ = qMax(1, pages);
    page_ = qBound(1, page, pages_);
    entries_ = qMax(0, entries);
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
    while (chipLayout_->count() > 0) {
        QLayoutItem* item = chipLayout_->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
    }
    // A window of chips follows the current page, centred on it and never
    // running off either end of the range.
    const int window = qMin(theme::kPagerChipWindow, pages_);
    const int first = qBound(0, page_ - 1 - window / 2, qMax(0, pages_ - window));
    for (int index = first; index < first + window; ++index) {
        const int target = index + 1;
        auto* button = new QPushButton(chipLabel(target), chipsHost_);
        button->setObjectName(target == page_ ? QStringLiteral("pagerChipActive")
                                             : QStringLiteral("pagerChip"));
        button->setToolTip(tr("Сторінка %1").arg(target));
        connect(button, &QPushButton::clicked, this, [this, target] { jumpTo(target); });
        chipLayout_->addWidget(button);
    }
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
