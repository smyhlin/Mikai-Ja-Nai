#include "widgets/RangePager.hpp"

#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "widgets/FlowLayout.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QSize>
#include <QSizePolicy>
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

/// The arrows are painted glyphs, not caption text: a symbol such as U+23EE is
/// resolved by font fallback, so on Windows it can arrive from an emoji face at
/// a different weight and colour than the chevrons drawn from the theme engine.
/// With no caption left to read, the button states its role in a property: the
/// tooltip and the accessible name are translated, the role is not.
QPushButton* makeArrow(const QIcon& icon, const char* role, const QString& tooltip) {
    auto* button = new QPushButton();
    button->setObjectName(QStringLiteral("pagerControl"));
    button->setProperty("pagerRole", QString::fromLatin1(role));
    button->setIcon(icon);
    button->setIconSize(QSize(16, 16));
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip);
    return button;
}

/// The range chips, wrapped rather than widened.
///
/// Its size hint is the natural one-line width of the chips, so a pager with room
/// keeps them beside the arrows; its minimum is a single chip, so a narrow pager
/// wraps the captions inside this row instead of forcing the page wider than its
/// viewport. The flow layout reports height-for-width, which is how the row grows
/// a line taller instead of clipping a chip.
class ChipRow final : public QWidget {
public:
    explicit ChipRow(int spacing, QWidget* parent = nullptr)
        : QWidget(parent), spacing_(spacing), flow_(new FlowLayout(spacing, theme::kSpacingXs)) {
        setLayout(flow_);
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }

    FlowLayout* flow() const { return flow_; }

    QSize sizeHint() const override {
        int width = 0;
        int height = 0;
        int visible = 0;
        for (int index = 0; index < flow_->count(); ++index) {
            const QLayoutItem* item = flow_->itemAt(index);
            const QWidget* chip = item != nullptr ? item->widget() : nullptr;
            if (chip == nullptr || chip->isHidden()) {
                continue;
            }
            width += chip->sizeHint().width();
            height = qMax(height, chip->sizeHint().height());
            ++visible;
        }
        if (visible > 1) {
            width += spacing_ * (visible - 1);
        }
        return QSize(width, height);
    }

private:
    int spacing_ = 0;
    FlowLayout* flow_ = nullptr;
};

}  // namespace

RangePager::RangePager(int pageSize, QWidget* parent)
    : QWidget(parent), pageSize_(qMax(1, pageSize)) {
    // The row declares height-for-width so a wrapped chip row makes the pager a
    // line taller instead of a column wider.
    QSizePolicy pagerPolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    pagerPolicy.setHeightForWidth(true);
    setSizePolicy(pagerPolicy);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(theme::kSpacingSmall, theme::kSpacingXs, theme::kSpacingSmall,
                            theme::kSpacingXs);
    row->setSpacing(theme::kSpacingSmall);

    firstButton_ = makeArrow(theme::firstPageIcon(), "first", tr("Go to first page"));
    prevButton_ = makeArrow(theme::chevronLeftIcon(), "previous", tr("Previous page"));

    auto* chips = new ChipRow(theme::kSpacingSmall, this);
    chipsHost_ = chips;
    chipLayout_ = chips->flow();

    nextButton_ = makeArrow(theme::chevronRightIcon(), "next", tr("Next page"));
    lastButton_ = makeArrow(theme::lastPageIcon(), "last", tr("Go to last page"));

    jumpLabel_ = new QLabel(this);
    jumpLabel_->setObjectName(QStringLiteral("muted"));

    jump_ = new QSpinBox(this);
    jump_->setRange(1, 1);
    jump_->setToolTip(tr("Jump to page"));
    jump_->setAccessibleName(tr("Page number"));

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

void RangePager::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    } else if (event->type() == QEvent::PaletteChange && firstButton_ != nullptr &&
               lastButton_ != nullptr) {
        // The glyph engine captures the palette colour it was built with, so the
        // arrows restate their icons when a theme switch moves that colour. The
        // page, the chips and the enabled state are untouched by the restate.
        firstButton_->setIcon(theme::firstPageIcon());
        prevButton_->setIcon(theme::chevronLeftIcon());
        nextButton_->setIcon(theme::chevronRightIcon());
        lastButton_->setIcon(theme::lastPageIcon());
    }
}

void RangePager::retranslateUi() {
    // Every caption here describes state the pager already holds. The page, the
    // range of chips and the enabled arrows are left exactly as they were, so a
    // switch never moves the page nor reports one.
    const auto restate = [](QPushButton* button, const QString& text) {
        button->setToolTip(text);
        button->setAccessibleName(text);
    };
    restate(firstButton_, tr("Go to first page"));
    restate(prevButton_, tr("Previous page"));
    restate(nextButton_, tr("Next page"));
    restate(lastButton_, tr("Go to last page"));

    jump_->setToolTip(tr("Jump to page"));
    jump_->setAccessibleName(tr("Page number"));

    if (!rendered_) {
        return;
    }
    jumpLabel_->setText(tr("page of %1").arg(pages_));
    // Re-labels the same chips, window and active role through the stored page
    // state; chip labels themselves are entry numbers, not prose.
    renderChips();
}

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
    jumpLabel_->setText(tr("page of %1").arg(pages_));
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
        for (int index = 0; index < chipLayout_->count(); ++index) {
            QLayoutItem* item = chipLayout_->itemAt(index);
            if (item != nullptr && item->widget() == extra) {
                delete chipLayout_->takeAt(index);
                break;
            }
        }
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
        chip->setToolTip(tr("Page %1").arg(target));
        chip->setAccessibleName(tr("Page %1").arg(target));
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
    // The captions changed, so the row's natural one-line width did too: the
    // host has to be re-measured before the pager decides whether they fit
    // beside the arrows.
    chipLayout_->invalidate();
    chipsHost_->updateGeometry();
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
