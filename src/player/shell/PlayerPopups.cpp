#include "PlayerPopups.hpp"

#include <QAction>
#include <QCursor>
#include <QDebug>
#include <QEvent>
#include <QGuiApplication>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

const char* const kPopupText = "#f0f0f5";
const char* const kPopupMuted = "#9a9aa6";

constexpr auto kPanelQss =
    "QFrame[chromePanel=\"true\"] {"
    "  background-color: transparent;"
    "  border: none; }";

constexpr auto kChoiceMenuQss =
    "QMenu {"
    "  background-color: #16181d;"
    "  color: #e5e7eb;"
    "  border: 1px solid #282c34;"
    "  border-radius: 6px;"
    "  padding: 4px;"
    "}"
    "QMenu::item {"
    "  padding: 5px 24px 5px 24px;"
    "  border-radius: 4px;"
    "  font-size: 12px;"
    "}"
    "QMenu::item:selected {"
    "  background-color: #2563eb;"
    "  color: #ffffff;"
    "}"
    "QMenu::item:disabled {"
    "  color: #6b7280;"
    "  background-color: transparent;"
    "}"
    "QMenu::separator {"
    "  height: 1px;"
    "  background: #282c34;"
    "  margin: 4px 6px;"
    "}";

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
               "  padding: 0 %2px; min-height: 28px; font-size: 13px; font-weight: 500; }")
        .arg(QLatin1String(kPopupMuted))
        .arg(kSpacingSmall);
}

QString captionQss(int size) {
    return QStringLiteral("color: %1; font-size: %2px; font-weight: 700; background: transparent; letter-spacing: 0.5px;")
        .arg(QLatin1String(kPopupMuted))
        .arg(size);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  GROUPED CHOICE MENUS
// ═══════════════════════════════════════════════════════════════════════════

void clearChoiceMenu(QMenu* menu) {
    // The submenus are widgets owned by this menu: clear() drops the actions,
    // so they are deleted explicitly or a rebuild/shrink would leak a tree.
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
}

void fillChoiceMenu(QMenu* menu, const QVector<ChoiceEntry>& entries, const QString& selected,
                    QVector<QAction*>* leaves) {
    // Rebuilding destroys actions, so the caller keeps what this returns and
    // moves the checks with markChoice() while its list is unchanged.
    if (leaves != nullptr) {
        leaves->clear();
        leaves->reserve(entries.size());
    }
    clearChoiceMenu(menu);
    menu->setStyleSheet(QString::fromLatin1(kChoiceMenuQss));
    menu->setToolTipsVisible(true);

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
            created->setStyleSheet(QString::fromLatin1(kChoiceMenuQss));
            created->setToolTipsVisible(true);
            levels.insert(key, created);
            level = created;
        }
        return level;
    };

    for (const ChoiceEntry& entry : entries) {
        QMenu* level = levelFor(entry.path);
        QAction* action = level->addAction(entry.label);
        action->setData(entry.id);
        action->setCheckable(true);
        action->setChecked(entry.id == selected);
        action->setEnabled(!entry.disabled);
        if (!entry.tooltip.isEmpty()) {
            action->setToolTip(entry.tooltip);
        }
        forwardChoiceLeaf(menu, level, action);
        if (leaves != nullptr) {
            leaves->append(action);
        }
    }
}

void forwardChoiceLeaf(QMenu* root, QMenu* level, QAction* action) {
    if (root == nullptr || action == nullptr) return;
    if (action->property("_mplayer_forwarded_leaf").toBool()) return;
    action->setProperty("_mplayer_forwarded_leaf", true);
    QObject::connect(action, &QAction::triggered, root, [root, level, action] {
        if (level != nullptr) {
            level->close();
            level->hide();
        }
        if (root != nullptr) {
            root->close();
            root->hide();
        }
        Q_EMIT root->triggered(action);
    });
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

    setToolTip(tr("Controls: ↑ ↓ — select, Enter — confirm, Esc — close"));
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

void MenuPopup::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void MenuPopup::retranslateUi() {
    // The caption, the headings and every row are the caller's words; the shell
    // and the dialog re-supply those from their own state on the same event.
    // What this popover words itself is the hint below the rows - key names
    // only, which this UI keeps literal, as the shortcut card does - so the
    // sentence that reads them aloud is the tooltip, and that is restated.
    setToolTip(tr("Controls: ↑ ↓ — select, Enter — confirm, Esc — close"));
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
            for (int i = 0; i < rowEntries_.size(); ++i) {
                if (!rowEntries_.at(i).disabled) {
                    setHighlight(i);
                    break;
                }
            }
            break;
        case Qt::Key_End:
            for (int i = rowEntries_.size() - 1; i >= 0; --i) {
                if (!rowEntries_.at(i).disabled) {
                    setHighlight(i);
                    break;
                }
            }
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

    caption_ = new QLabel(tr("SERIES"), this);
    caption_->setObjectName(QStringLiteral("playerSeriesCaption"));
    caption_->setStyleSheet(captionQss(11));
    outer->addWidget(caption_);

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
    while (QLayoutItem* item = rowsLayout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
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
        button->setStyleSheet(spot == current ? rowQss(kPopupText, "rgba(189, 172, 255, 0.20)")
                                              : rowQss(kPopupMuted, "transparent"));
    }
    rowsLayout_->addStretch(1);
}

int SeriesDrawer::contentHeight() const {
    return rows_.size() * (kRowHeight + 2) + 3 * kSpacingXs + 20;
}

void SeriesDrawer::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void SeriesDrawer::retranslateUi() {
    // Only the drawer's own caption: the episodes are the playlist's labels and
    // stay exactly as they arrived.
    if (caption_ != nullptr) {
        caption_->setText(tr("SERIES"));
    }
}

ProcessingOverlay::ProcessingOverlay(QWidget* parent)
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
    setAttribute(Qt::WA_ShowWithoutActivating);
    setObjectName(QStringLiteral("processingOverlay"));
    setStyleSheet(QStringLiteral("QFrame#processingOverlay {"
                                 "  background-color: #16181d;"
                                 "  border: 1px solid #282c34;"
                                 "  border-radius: 8px; }"));
    setFixedWidth(360);

    auto* layout = new QVBoxLayout(this);
    title_ = new QLabel(this);
    title_->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(kPopupText));
    title_->setWordWrap(true);
    layout->addWidget(title_);

    detail_ = new QLabel(this);
    detail_->setStyleSheet(QStringLiteral("color: %1;").arg(kPopupMuted));
    detail_->setWordWrap(true);
    layout->addWidget(detail_);

    progress_ = new QProgressBar(this);
    progress_->setTextVisible(false);
    progress_->setFixedHeight(6);
    progress_->setRange(0, 0);
    // The bar paints no text, so its accessible name is the only place the
    // indeterminate-versus-staged fact can be read: a silent bar reads as
    // nothing happening at all.
    progress_->setAccessibleName(tr("Preparation progress"));
    progress_->setStyleSheet(QStringLiteral("QProgressBar {"
                                            "  background-color: #1e2128;"
                                            "  border: none; border-radius: 3px; }"
                                            "QProgressBar::chunk {"
                                            "  background-color: #3b82f6; border-radius: 3px; }"));
    layout->addWidget(progress_);

    elapsedLabel_ = new QLabel(this);
    elapsedLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(kPopupMuted));
    layout->addWidget(elapsedLabel_);
    elapsedTimer_ = new QTimer(this);
    elapsedTimer_->setInterval(1000);
    connect(elapsedTimer_, &QTimer::timeout, this, [this] {
        // The count is kept as state so a language switch can redraw the very
        // same elapsed time rather than restarting or advancing the clock.
        elapsedSeconds_ = elapsed_.elapsed() / 1000;
        renderElapsed();
    });
    finishTimer_ = new QTimer(this);
    finishTimer_->setSingleShot(true);
    connect(finishTimer_, &QTimer::timeout, this, &QWidget::hide);

    // Windows can let a competing topmost window cover this tool after its
    // initial show; X11 window managers can do the same. Reassert only on
    // platforms where raising is observable and non-activating. Wayland owns
    // stacking and must not be polled with futile raise requests.
    const QString platform = QGuiApplication::platformName();
#ifdef Q_OS_WIN
    const bool canReassertStacking = platform == QStringLiteral("windows");
#else
    const bool canReassertStacking = platform == QStringLiteral("xcb");
#endif
    if (canReassertStacking) {
        stackingTimer_ = new QTimer(this);
        stackingTimer_->setInterval(250);
        connect(stackingTimer_, &QTimer::timeout, this, &ProcessingOverlay::maintainTopmost);
    }

    cancel_ = new QPushButton(this);
    cancel_->setObjectName(QStringLiteral("processingOverlayCancel"));
    cancel_->setStyleSheet(rowQss(kPopupText, "rgba(255, 255, 255, 0.08)"));
    cancel_->setAccessibleName(tr("Cancel preparation"));
    // One connection for the window's whole life, dispatched on what the window
    // is showing: rewiring it per run is how a second build inherited the
    // previous failure's Dismiss handler and could no longer be cancelled.
    connect(cancel_, &QPushButton::clicked, this, [this] {
        if (run_ == Run::Succeeded || run_ == Run::Failed) {
            hide();
            return;
        }
        cancelPending_ = true;
        cancel_->setEnabled(false);
        cancel_->setText(QStringLiteral("…"));
        emit cancelRequested();
    });
    layout->addWidget(cancel_);
}

void ProcessingOverlay::begin(const QString& title) {
    finishTimer_->stop();
    elapsed_.restart();
    elapsedSeconds_ = 0;
    renderElapsed();
    elapsedTimer_->start();
    run_ = Run::Running;
    cancelPending_ = false;
    startedTitle_ = title;
    stageKnown_ = false;
    stagePhase_.clear();
    stageDetail_.clear();
    stageIndex_ = -1;
    stageCount_ = 0;
    title_->setText(title);
    detail_->clear();
    progress_->setRange(0, 0);
    cancel_->setEnabled(true);
    cancel_->setText(tr("Cancel"));
    adjustSize();
    show();
    raise();
    if (stackingTimer_ != nullptr) {
        maintainTopmost();
        stackingTimer_->start();
    }
}

void ProcessingOverlay::setStage(int stageIndex, int stageCount, const QString& phase,
                                 const QString& detail) {
    if (run_ != Run::Idle && run_ != Run::Running) return;
    // Kept typed, not as display text: a language switch rebuilds the wrapper
    // around these words, and only the wrapper is this window's to translate.
    stageKnown_ = true;
    stageIndex_ = stageIndex;
    stageCount_ = stageCount;
    stagePhase_ = phase;
    stageDetail_ = detail;
    if (stageCount > 0 && stageIndex >= 0) {
        progress_->setRange(0, stageCount);
        progress_->setValue(qBound(0, stageIndex, stageCount));
    } else {
        progress_->setRange(0, 0);
    }
    renderStage();
    adjustSize();
}

void ProcessingOverlay::setOperationTitle(const QString& title) {
    if (run_ != Run::Running) return;
    startedTitle_ = title;
    title_->setText(title);
    adjustSize();
}

void ProcessingOverlay::setSummary(const QString& summary) {
    if (run_ != Run::Succeeded && run_ != Run::Failed) return;
    detail_->setText(summary);
    adjustSize();
}

void ProcessingOverlay::finish(bool ok, const QString& summary) {
    run_ = ok ? Run::Succeeded : Run::Failed;
    elapsedTimer_->stop();
    finishTimer_->stop();
    cancel_->setEnabled(false);
    progress_->setRange(0, 1);
    progress_->setValue(ok ? 1 : 0);
    detail_->setText(summary);
    title_->setText(ok ? tr("Engine ready") : tr("Engine build stopped"));
    // A success has nothing left to read, so it leaves on its own; a failure
    // stays until the viewer dismisses it, because a refusal that disappears is
    // a refusal nobody saw.
    if (ok) {
        finishTimer_->start(1500);
    } else {
        cancel_->setText(tr("Dismiss"));
        cancel_->setEnabled(true);
    }
    adjustSize();
}

void ProcessingOverlay::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void ProcessingOverlay::showEvent(QShowEvent* event) {
    QFrame::showEvent(event);
    if (run_ == Run::Idle) return;
    if (stackingTimer_ != nullptr) {
        maintainTopmost();
        stackingTimer_->start();
    }
}

void ProcessingOverlay::hideEvent(QHideEvent* event) {
    if (stackingTimer_ != nullptr) stackingTimer_->stop();
    QFrame::hideEvent(event);
}

void ProcessingOverlay::maintainTopmost() {
    if (!isVisible()) {
        if (stackingTimer_ != nullptr) stackingTimer_->stop();
        return;
    }
#ifdef Q_OS_WIN
    const HWND window = reinterpret_cast<HWND>(winId());
    if (window != nullptr) {
        if (SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
            stackingFailureReported_ = false;
        } else if (!stackingFailureReported_) {
            const DWORD error = GetLastError();
            qWarning() << "ProcessingOverlay could not reassert HWND_TOPMOST for HWND"
                       << static_cast<void*>(window) << "; Win32 error" << error;
            stackingFailureReported_ = true;
        }
    }
#else
    // QWidget::raise() is non-activating on X11; the timer is not created on
    // Wayland, where the compositor alone controls global stacking.
    raise();
#endif
}

void ProcessingOverlay::renderElapsed() {
    const qint64 seconds = elapsedSeconds_;
    elapsedLabel_->setText(tr("Elapsed: %1:%2:%3")
                              .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
                              .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
                              .arg(seconds % 60, 2, 10, QLatin1Char('0')));
}

void ProcessingOverlay::renderStage() {
    if (!stageKnown_) {
        detail_->clear();
        return;
    }
    QString text = stagePhase_;
    if (stageIndex_ >= 0) {
        text = stageCount_ > 0
                   ? tr("Stage %1 of %2: %3").arg(stageIndex_).arg(stageCount_).arg(stagePhase_)
                   : tr("Stage %1: %2").arg(stageIndex_).arg(stagePhase_);
    }
    if (!stageDetail_.isEmpty()) text += QStringLiteral(" — ") + stageDetail_;
    detail_->setText(text);
}

void ProcessingOverlay::retranslateUi() {
    progress_->setAccessibleName(tr("Preparation progress"));
    cancel_->setAccessibleName(tr("Cancel preparation"));
    if (run_ == Run::Idle) {
        // Nothing has been worded yet; begin() supplies every line of the run.
        return;
    }
    // The elapsed count and the bar are state, not words: they are redrawn from
    // what the run already holds, so the timer, the stage and the progress stay
    // exactly where they were.
    renderElapsed();
    if (run_ == Run::Running) {
        // The title, the phase and the detail are the caller's words, re-emitted
        // verbatim. A caller that words them itself restates them through
        // setOperationTitle()/setStage(), which only replace text; this pass
        // never translates them by guesswork, begins or restarts a run.
        title_->setText(startedTitle_);
        renderStage();
    } else if (run_ == Run::Succeeded) {
        title_->setText(tr("Engine ready"));
        // detail_ holds the caller's summary verbatim.
    } else {
        title_->setText(tr("Engine build stopped"));
    }
    // The action follows the same state machine as the click handler: a settled
    // failure offers Dismiss, a pending cancel keeps its mark and stays disabled,
    // and a running build still offers Cancel.
    if (run_ == Run::Failed) {
        cancel_->setText(tr("Dismiss"));
        cancel_->setEnabled(true);
    } else if (cancelPending_) {
        cancel_->setText(QStringLiteral("…"));
        cancel_->setEnabled(false);
    } else {
        cancel_->setText(tr("Cancel"));
        cancel_->setEnabled(run_ == Run::Running);
    }
    adjustSize();
}

void ProcessingOverlay::anchorTo(const QWidget* anchor) {
    if (anchor == nullptr) return;
    const QRect area = anchor->screen() != nullptr ? anchor->screen()->availableGeometry()
                                                   : QRect();
    adjustSize();
    const QPoint centre = area.isNull() ? QCursor::pos() : area.center();
    const int x = area.isNull() ? centre.x() : area.x() + (area.width() - width()) / 2;
    const int y = area.isNull() ? centre.y() : area.y() + 80;
    move(qMax(0, x), qMax(0, y));
}

}  // namespace shell
}  // namespace mplayer
