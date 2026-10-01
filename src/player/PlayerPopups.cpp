#include "PlayerPopups.hpp"

#include <QAction>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace mplayer {
namespace shell {
namespace {

// ── mikai theme tokens, as the app defines them ────────────────────────────
// The app keeps its palette in Python; the popovers reproduce the exact values
// so a stylesheet here matches the rest of the client.

constexpr int kSpacingXs = 4;
constexpr int kSpacingSmall = 8;
constexpr int kRowHeight = 28;

// Qt's QWIDGETSIZE_MAX is a C++ macro; `refresh` needs it to undo the fixed
// geometry a previous popupNear left behind.
constexpr int kUnlimited = 16777215;

const char* const kText = "#f0f0f5";
const char* const kMuted = "#9a9aa6";

constexpr auto kPanelQss =
    "QFrame[chromePanel=\"true\"] {"
    "  background-color: transparent;"
    "  border: none; }";

QString rowQss(const char* foreground, const char* fill) {
    return QStringLiteral(
               "QPushButton {"
               "  color: %1;"
               "  background: %2;"
               "  border: 1px solid transparent; border-radius: 6px; text-align: left;"
               "  padding: 0 %3px; min-height: 28px; font-size: 13px; font-weight: 500; }"
               "QPushButton:hover {"
               "  background: rgba(255, 255, 255, 0.12); color: #ffffff;"
               "  border-color: rgba(255, 255, 255, 0.20); }")
        .arg(QLatin1String(foreground), QLatin1String(fill))
        .arg(kSpacingSmall);
}

/// A row that is shown but cannot be chosen. No hover rule on purpose: a
/// highlight promises a click that this row will not answer.
QString disabledRowQss() {
    return QStringLiteral(
               "QPushButton {"
               "  color: %1;"
               "  background: rgba(255, 255, 255, 0.03);"
               "  border: 1px solid transparent; border-radius: 6px; text-align: left;"
               "  padding: 0 %3px; min-height: 28px; font-size: 13px; font-weight: 500; }")
        .arg(QLatin1String(kMuted))
        .arg(kSpacingSmall);
}

QString captionQss(int size) {
    return QStringLiteral("color: %1; font-size: %2px; font-weight: 700; background: transparent; letter-spacing: 0.5px;")
        .arg(QLatin1String(kMuted))
        .arg(size);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  GROUPED CHOICE MENUS
// ═══════════════════════════════════════════════════════════════════════════

void fillChoiceMenu(QMenu* menu, const QVector<ChoiceEntry>& entries, const QString& selected,
                    QVector<QAction*>* leaves) {
    // Rebuilding destroys actions, so the caller keeps what this returns and
    // moves the checks with markChoice() while its list is unchanged.
    if (leaves != nullptr) {
        leaves->clear();
        leaves->reserve(entries.size());
    }
    // The submenus are widgets owned by this menu: clear() drops the actions,
    // so they are deleted explicitly or a rebuild would leak a tree each time.
    QVector<QMenu*> stale;
    for (QAction* action : menu->actions()) {
        if (QMenu* submenu = action->menu()) {
            stale.append(submenu);
        }
    }
    for (QMenu* submenu : stale) {
        delete submenu;
    }
    menu->clear();

    // One submenu per distinct path, not one per entry: addMenu() always
    // creates a new menu, so walking the path per entry would put a second
    // "Anime4K" branch beside the first for every candidate in the list.
    QHash<QString, QMenu*> levels;
    const auto levelFor = [menu, &levels](const QStringList& path) {
        QMenu* level = menu;
        QString key;
        for (const QString& name : path) {
            if (name.isEmpty()) {
                continue;
            }
            key += QLatin1Char('\n') + name;
            const auto found = levels.constFind(key);
            if (found != levels.constEnd()) {
                level = found.value();
                continue;
            }
            QMenu* created = level->addMenu(name);
            levels.insert(key, created);
            level = created;
        }
        return level;
    };

    for (const ChoiceEntry& entry : entries) {
        QAction* action = levelFor(entry.path)->addAction(entry.label);
        action->setData(entry.id);
        action->setCheckable(true);
        action->setChecked(entry.id == selected);
        if (leaves != nullptr) {
            leaves->append(action);
        }
    }
}

void markChoice(const QVector<QAction*>& leaves, const QVector<ChoiceEntry>& entries,
                const QString& selected) {
    if (leaves.size() != entries.size()) {
        return;
    }
    for (qsizetype i = 0; i < entries.size(); ++i) {
        leaves.at(i)->setChecked(entries.at(i).id == selected);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  MENU POPUP
// ═══════════════════════════════════════════════════════════════════════════

MenuPopup::MenuPopup(const QString& title, QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("playerMenuPopup"));
    setProperty("chromePanel", true);
    setProperty("playerOverlay", true);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QString::fromLatin1(kPanelQss));
    hide();

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kSpacingSmall, kSpacingSmall, kSpacingSmall, kSpacingXs);
    outer->setSpacing(kSpacingXs);

    caption_ = new QLabel(title, this);
    caption_->setObjectName(QStringLiteral("playerMenuCaption"));
    caption_->setStyleSheet(captionQss(11));
    outer->addWidget(caption_);

    body_ = new QVBoxLayout();
    body_->setContentsMargins(0, 0, 0, 0);
    body_->setSpacing(2);
    outer->addLayout(body_);

    // A long hint would set the popover's width, and a quality list two rows
    // high does not want to be 500 px wide.
    auto* hint = new QLabel(QStringLiteral("↑↓ · Enter · Esc"), this);
    hint->setObjectName(QStringLiteral("playerMenuHint"));
    hint->setStyleSheet(captionQss(10));
    outer->addWidget(hint);

    setToolTip(QStringLiteral("Керування: ↑ ↓ — вибір, Enter — підтвердити, Esc — закрити"));
}

int MenuPopup::rowCount() const {
    return rows_.size();
}

QWidget* MenuPopup::rowWidget(int spot) const {
    if (spot < 0 || spot >= rows_.size()) {
        return nullptr;
    }
    return rows_.at(spot);
}

MenuEntry MenuPopup::entryAt(int spot) const {
    if (spot < 0 || spot >= rowEntries_.size()) {
        return MenuEntry();
    }
    return rowEntries_.at(spot);
}

void MenuPopup::setEntries(const QVector<MenuEntry>& entries) {
    while (body_->count()) {
        QLayoutItem* item = body_->takeAt(0);
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        // takeAt hands the item over, and the widget keeps its parent: the item
        // is deleted here, the row is only retired.
        delete item;
    }
    rows_.clear();
    rowEntries_.clear();
    highlight_ = -1;

    for (const MenuEntry& entry : entries) {
        if (!entry.heading.isEmpty()) {
            auto* label = new QLabel(entry.heading, this);
            label->setObjectName(QStringLiteral("playerMenuHeading"));
            label->setStyleSheet(captionQss(11) + QStringLiteral(" padding: 4px 6px;"));
            body_->addWidget(label);
            label->show();
            continue;
        }
        const QString text = entry.text;
        auto* button = new QPushButton(text, this);
        button->setObjectName(QStringLiteral("playerMenuRow"));
        button->setToolTip(entry.tooltip.isEmpty() ? text : entry.tooltip);
        button->setAccessibleName(text);
        button->setCursor(entry.disabled ? Qt::ArrowCursor : Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedHeight(kRowHeight);
        button->setEnabled(!entry.disabled);
        // A disabled row is still added to rows_ and rowEntries_ so the two
        // stay index-aligned; it is only the click that it does not have.
        if (!entry.disabled) {
            const int spot = rows_.size();
            connect(button, &QPushButton::clicked, this, [this, spot] { activate(spot); });
        }
        body_->addWidget(button);
        rows_.append(button);
        rowEntries_.append(entry);
        // Qt does not show a widget created after its parent is already visible,
        // and a drill-down rebuilds the rows of an open popover.
        button->show();
    }
    paintRows();
    fitToHint();
}

void MenuPopup::setTitle(const QString& title) {
    if (caption_ == nullptr || caption_->text() == title) {
        return;
    }
    caption_->setText(title);
    // The caption is what the captionQss letter-spacing and size are tuned for,
    // so the popover is re-fitted: a longer title must not be clipped.
    fitToHint();
}

void MenuPopup::refresh() {
    fitToHint();
}

void MenuPopup::popupNear(const QPoint& anchorGlobal, bool preferAbove) {
    show();
    fitToHint();
    QWidget* host = parentWidget();
    if (host == nullptr) {
        return;
    }

    const QPoint anchor = host->mapFromGlobal(anchorGlobal);
    const QRect bounds = host->rect().adjusted(4, 4, -4, -4);
    const int above = anchor.y() - height() - 8;
    int x = anchor.x() - width();
    int y = preferAbove ? above : anchor.y() + 8;
    if (preferAbove && above < bounds.top()) {
        y = anchor.y() + 8;
    }
    x = qBound(bounds.left(), x, bounds.right() - width() + 1);
    y = qBound(bounds.top(), y, bounds.bottom() - height() + 1);
    move(x, y);
    raise();
    setFocus(Qt::OtherFocusReason);
}

void MenuPopup::popupBeside(const QRect& anchor, bool preferLeft, int gap) {
    show();
    fitToHint();
    QWidget* host = parentWidget();
    if (host == nullptr) {
        return;
    }

    const QRect bounds = host->rect().adjusted(4, 4, -4, -4);
    // The preferred side is taken as given: a tree that flipped sides per level
    // would stack its own panels on top of each other.
    const int beside = preferLeft ? anchor.left() - gap - width() : anchor.right() + 1 + gap;
    const int x = qBound(bounds.left(), beside, bounds.right() - width() + 1);
    const int y = qBound(bounds.top(), anchor.top(), bounds.bottom() - height() + 1);
    move(x, y);
    raise();
    setFocus(Qt::OtherFocusReason);
}

void MenuPopup::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 45), 1));
    p.setBrush(QColor(20, 21, 27));
    p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 10.0, 10.0);
}

void MenuPopup::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Escape:
            close();
            break;
        case Qt::Key_Down:
        case Qt::Key_Right:
            moveHighlight(1);
            break;
        case Qt::Key_Up:
        case Qt::Key_Left:
            moveHighlight(-1);
            break;
        case Qt::Key_Home:
            setHighlight(0);
            break;
        case Qt::Key_End:
            setHighlight(rows_.size() - 1);
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (highlight_ >= 0)
                activate(highlight_);
            break;
        default:
            QFrame::keyPressEvent(event);
            return;
    }
    event->accept();
}

void MenuPopup::fitToHint() {
    setMinimumSize(0, 0);
    setMaximumSize(kUnlimited, kUnlimited);
    ensurePolished();
    layout()->invalidate();
    layout()->activate();
    // A hidden widget has no geometry yet, so its layout can report a hint
    // smaller than the content it will hold once shown - and setFixedSize
    // would make that the popup's permanent size, stacking every row into it.
    // The visible path re-fits after show(), so there is nothing to cache here.
    if (isVisible()) {
        setFixedSize(layout()->sizeHint());
        layout()->activate();
    }
}

void MenuPopup::moveHighlight(int delta) {
    if (rows_.isEmpty())
        return;
    // Disabled rows are stepped over: a highlight on a row that cannot be
    // activated reads as a broken keyboard.
    const int step = delta >= 0 ? 1 : -1;
    int spot = highlight_ < 0 ? (step > 0 ? -1 : rows_.size()) : highlight_;
    for (int guard = 0; guard < rows_.size(); ++guard) {
        spot = (spot + step + rows_.size()) % rows_.size();
        if (!rowEntries_.at(spot).disabled) {
            setHighlight(spot);
            return;
        }
    }
}

void MenuPopup::setHighlight(int spot) {
    if (rows_.isEmpty())
        return;
    highlight_ = qBound(0, spot, rows_.size() - 1);
    paintRows();
}

void MenuPopup::activate(int spot) {
    if (spot < 0 || spot >= rowEntries_.size())
        return;
    const MenuEntry& entry = rowEntries_.at(spot);
    // A disabled row is informational: it neither closes the popover nor emits,
    // so it cannot change what is selected.
    if (entry.disabled)
        return;
    // A submenu row stays open: the caller places the next level's panel beside
    // this one, which is what makes the tree navigable without a Back row.
    if (!entry.submenu)
        close();
    emit picked(entry.payload);
}

void MenuPopup::paintRows() {
    for (int index = 0; index < rows_.size(); ++index) {
        if (index < rowEntries_.size() && rowEntries_.at(index).disabled)
            rows_.at(index)->setStyleSheet(disabledRowQss());
        else if (index < rowEntries_.size() && rowEntries_.at(index).current)
            rows_.at(index)->setStyleSheet(rowQss("#ffffff", "rgba(88, 101, 242, 0.40)"));
        else if (index == highlight_)
            rows_.at(index)->setStyleSheet(rowQss("#ffffff", "rgba(255, 255, 255, 0.12)"));
        else
            rows_.at(index)->setStyleSheet(rowQss("#e0e0e6", "rgba(255, 255, 255, 0.05)"));
    }
}



// ═══════════════════════════════════════════════════════════════════════════
//  SERIES DRAWER
// ═══════════════════════════════════════════════════════════════════════════

void SeriesDrawer::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 45), 1));
    p.setBrush(QColor(20, 21, 27));
    p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 10.0, 10.0);
}

SeriesDrawer::SeriesDrawer(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("playerSeriesDrawer"));
    setProperty("chromePanel", true);
    setStyleSheet(QString::fromLatin1(kPanelQss));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kSpacingXs, kSpacingXs, kSpacingXs, kSpacingXs);
    outer->setSpacing(kSpacingXs);

    auto* caption = new QLabel(QStringLiteral("СЕРІЇ"), this);
    caption->setObjectName(QStringLiteral("playerSeriesCaption"));
    caption->setStyleSheet(captionQss(11));
    outer->addWidget(caption);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("playerSeriesScroll"));
    scroll->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget();
    content->setObjectName(QStringLiteral("playerSeriesList"));
    content->setStyleSheet(QStringLiteral("background: transparent;"));
    rowsLayout_ = new QVBoxLayout(content);
    rowsLayout_->setContentsMargins(0, 0, 0, 0);
    rowsLayout_->setSpacing(2);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
}

void SeriesDrawer::setEntries(const QStringList& labels, int current) {
    while (rowsLayout_->count()) {
        if (QWidget* widget = rowsLayout_->takeAt(0)->widget()) {
            widget->hide();
            widget->deleteLater();
        }
    }
    rows_.clear();
    for (int spot = 0; spot < labels.size(); ++spot) {
        const QString label = labels.at(spot);
        auto* button = new QPushButton(label, this);
        button->setObjectName(spot == current ? QStringLiteral("playerSeriesItemCurrent")
                                              : QStringLiteral("playerSeriesItem"));
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(label);
        button->setAccessibleName(label);
        // A focused button would swallow Space, and Space is play/pause.
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedHeight(kRowHeight);
        connect(button, &QPushButton::clicked, this, [this, spot] { emit picked(spot); });
        rowsLayout_->addWidget(button);
        rows_.append(button);
        button->setStyleSheet(spot == current ? rowQss(kText, "rgba(189, 172, 255, 0.20)")
                                              : rowQss(kMuted, "transparent"));
    }
    rowsLayout_->addStretch(1);
}

int SeriesDrawer::contentHeight() const {
    return rows_.size() * (kRowHeight + 2) + 3 * kSpacingXs + 20;
}

}  // namespace shell
}  // namespace mplayer
