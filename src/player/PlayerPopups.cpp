#include "PlayerPopups.hpp"

#include <QAction>
#include <QCursor>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QTimer>
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

const char* const kText = "#e8eaed";
const char* const kMuted = "#8b929e";

constexpr auto kPanelQss =
    "QFrame[chromePanel=\"true\"] {"
    "  background-color: transparent;"
    "  border: none; }";

QString choiceMenuQss() {
    return QStringLiteral(
        "QMenu {"
        "  background-color: #16181d;"
        "  color: #e8eaed;"
        "  border: 1px solid #2e333d;"
        "  border-radius: 6px;"
        "  padding: 4px;"
        "  font-size: 12px;"
        "  font-weight: 500;"
        "}"
        "QMenu::item {"
        "  padding: 5px 28px 5px 24px;"
        "  border-radius: 4px;"
        "  margin: 1px 2px;"
        "  background: transparent;"
        "  color: #dcdfe4;"
        "}"
        "QMenu::item:selected {"
        "  background-color: #2563eb;"
        "  color: #ffffff;"
        "}"
        "QMenu::item:checked {"
        "  background-color: rgba(59, 130, 246, 0.18);"
        "  color: #ffffff;"
        "  font-weight: 600;"
        "}"
        "QMenu::item:checked:selected {"
        "  background-color: #2563eb;"
        "  color: #ffffff;"
        "}"
        "QMenu::item:disabled {"
        "  color: #5a606d;"
        "  background: transparent;"
        "}"
        "QMenu::separator {"
        "  height: 1px;"
        "  background: #282c34;"
        "  margin: 4px 6px;"
        "}");
}

QString rowQss(const char* foreground, const char* fill) {
    return QStringLiteral(
               "QPushButton {"
               "  color: %1;"
               "  background: %2;"
               "  border: 1px solid transparent; border-radius: 5px; text-align: left;"
               "  padding: 0 %3px; min-height: 28px; font-size: 13px; font-weight: 500; }"
               "QPushButton:hover {"
               "  background: #262a33; color: #ffffff;"
               "  border-color: #383e4c; }")
        .arg(QLatin1String(foreground), QLatin1String(fill))
        .arg(kSpacingSmall);
}

/// A row that is shown but cannot be chosen. No hover rule on purpose: a
/// highlight promises a click that this row will not answer.
QString disabledRowQss() {
    return QStringLiteral(
               "QPushButton {"
               "  color: %1;"
               "  background: rgba(255, 255, 255, 0.02);"
               "  border: 1px solid transparent; border-radius: 5px; text-align: left;"
               "  padding: 0 %3px; min-height: 28px; font-size: 13px; font-weight: 500; }")
        .arg(QLatin1String(kMuted))
        .arg(kSpacingSmall);
}

QString captionQss(int size) {
    return QStringLiteral("color: %1; font-size: %2px; font-weight: 700; background: transparent; letter-spacing: 0.4px;")
        .arg(QLatin1String(kMuted))
        .arg(size);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  GROUPED CHOICE MENUS
// ═══════════════════════════════════════════════════════════════════════════

void clearChoiceMenu(QMenu* menu) {
    if (menu == nullptr) {
        return;
    }
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
    if (menu == nullptr) {
        return;
    }
    // Rebuilding destroys actions, so the caller keeps what this returns and
    // moves the checks with markChoice() while its list is unchanged.
    if (leaves != nullptr) {
        leaves->clear();
        leaves->reserve(entries.size());
    }
    clearChoiceMenu(menu);
    menu->setToolTipsVisible(true);
    menu->setStyleSheet(choiceMenuQss());

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
            created->setToolTipsVisible(true);
            created->setStyleSheet(choiceMenuQss());
            levels.insert(key, created);
            level = created;
        }
        return level;
    };

    for (const ChoiceEntry& entry : entries) {
        QMenu* level = levelFor(entry.path);
        QAction* action = level->addAction(entry.label);
        action->setData(entry.id);
        action->setEnabled(!entry.disabled);
        action->setCheckable(!entry.disabled);
        action->setChecked(!entry.disabled && entry.id == selected);
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
    // A leaf triggered programmatically (or when Qt's causedPopup chain is not
    // populated) fires `triggered` only on the submenu that owns it, whereas a
    // mouse click in an open submenu fires `QAction::triggered` first and then
    // walks `activateCausedStack` to emit `root->triggered(action)`. We mark
    // forwarded leaves and tag the active emission on the action itself so
    // callers connected to `root->triggered` hear every pick (including nested
    // or back-to-back picks) exactly once.
    if (root == nullptr || level == root || action == nullptr) return;
    action->setProperty("_mplayer_forwarded_leaf", true);
    QObject::connect(action, &QAction::triggered, root, [root, action] {
        if (action->property("_mplayer_leaf_emitting").toBool()) {
            return;
        }
        QPointer<QMenu> rootGuard(root);
        QPointer<QAction> actionGuard(action);
        action->setProperty("_mplayer_leaf_emitting", true);
        Q_EMIT root->triggered(action);
        if (actionGuard != nullptr) {
            actionGuard->setProperty("_mplayer_leaf_emitting", false);
        }
        Q_UNUSED(rootGuard);
    });
}

void markChoice(const QVector<QAction*>& leaves, const QVector<ChoiceEntry>& entries,
                const QString& selected) {
    if (leaves.size() != entries.size()) {
        return;
    }
    for (qsizetype i = 0; i < entries.size(); ++i) {
        if (leaves.at(i) != nullptr) {
            leaves.at(i)->setChecked(!entries.at(i).disabled && entries.at(i).id == selected);
        }
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
    p.setPen(QPen(QColor(46, 51, 61), 1));
    p.setBrush(QColor(22, 24, 29));
    p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 8.0, 8.0);
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
    if (isVisible()) {
        setFixedSize(layout()->sizeHint());
        layout()->activate();
    }
}

void MenuPopup::moveHighlight(int delta) {
    if (rows_.isEmpty())
        return;
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
    if (entry.disabled)
        return;
    if (!entry.submenu)
        close();
    emit picked(entry.payload);
}

void MenuPopup::paintRows() {
    for (int index = 0; index < rows_.size(); ++index) {
        if (index < rowEntries_.size() && rowEntries_.at(index).disabled)
            rows_.at(index)->setStyleSheet(disabledRowQss());
        else if (index < rowEntries_.size() && rowEntries_.at(index).current)
            rows_.at(index)->setStyleSheet(rowQss("#ffffff", "rgba(37, 99, 235, 0.38)"));
        else if (index == highlight_)
            rows_.at(index)->setStyleSheet(rowQss("#ffffff", "#262a33"));
        else
            rows_.at(index)->setStyleSheet(rowQss("#dcdfe4", "rgba(255, 255, 255, 0.04)"));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  SERIES DRAWER
// ═══════════════════════════════════════════════════════════════════════════

void SeriesDrawer::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(46, 51, 61), 1));
    p.setBrush(QColor(22, 24, 29));
    p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 8.0, 8.0);
}

SeriesDrawer::SeriesDrawer(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("playerSeriesDrawer"));
    setProperty("chromePanel", true);
    setStyleSheet(QString::fromLatin1(kPanelQss));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kSpacingXs, kSpacingXs, kSpacingXs, kSpacingXs);
    outer->setSpacing(kSpacingXs);

    auto* caption = new QLabel(tr("SERIES"), this);
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
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedHeight(kRowHeight);
        connect(button, &QPushButton::clicked, this, [this, spot] { emit picked(spot); });
        rowsLayout_->addWidget(button);
        rows_.append(button);
        button->setStyleSheet(spot == current ? rowQss(kText, "rgba(59, 130, 246, 0.24)")
                                              : rowQss(kMuted, "transparent"));
    }
    rowsLayout_->addStretch(1);
}

int SeriesDrawer::contentHeight() const {
    return rows_.size() * (kRowHeight + 2) + 3 * kSpacingXs + 20;
}

ProcessingOverlay::ProcessingOverlay(QWidget* parent)
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
    setAttribute(Qt::WA_ShowWithoutActivating);
    setObjectName(QStringLiteral("processingOverlay"));
    setStyleSheet(QStringLiteral("QFrame#processingOverlay {"
                                 "  background-color: #16181d;"
                                 "  border: 1px solid #3b82f6;"
                                 "  border-radius: 8px; }"));
    setFixedWidth(380);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);

    title_ = new QLabel(this);
    title_->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 700;").arg(kText));
    title_->setWordWrap(true);
    layout->addWidget(title_);

    detail_ = new QLabel(this);
    detail_->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(kMuted));
    detail_->setWordWrap(true);
    layout->addWidget(detail_);

    progress_ = new QProgressBar(this);
    progress_->setTextVisible(false);
    progress_->setFixedHeight(6);
    progress_->setRange(0, 0);
    progress_->setAccessibleName(tr("Preparation progress"));
    progress_->setStyleSheet(QStringLiteral("QProgressBar {"
                                            "  background-color: #242830;"
                                            "  border: none; border-radius: 3px; }"
                                            "QProgressBar::chunk {"
                                            "  background-color: #3b82f6; border-radius: 3px; }"));
    layout->addWidget(progress_);

    elapsedLabel_ = new QLabel(this);
    elapsedLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px; font-family: 'Consolas', 'JetBrains Mono', monospace;")
            .arg(kMuted));
    layout->addWidget(elapsedLabel_);
    elapsedTimer_ = new QTimer(this);
    elapsedTimer_->setInterval(1000);
    connect(elapsedTimer_, &QTimer::timeout, this, [this] {
        const qint64 seconds = elapsed_.elapsed() / 1000;
        elapsedLabel_->setText(tr("Elapsed: %1:%2:%3")
                                  .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
                                  .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
                                  .arg(seconds % 60, 2, 10, QLatin1Char('0')));
    });
    finishTimer_ = new QTimer(this);
    finishTimer_->setSingleShot(true);
    connect(finishTimer_, &QTimer::timeout, this, &QWidget::hide);

    cancel_ = new QPushButton(this);
    cancel_->setObjectName(QStringLiteral("processingOverlayCancel"));
    cancel_->setCursor(Qt::PointingHandCursor);
    cancel_->setStyleSheet(rowQss(kText, "#22262e"));
    cancel_->setAccessibleName(tr("Cancel preparation"));
    connect(cancel_, &QPushButton::clicked, this, [this] {
        if (settled_) {
            hide();
            return;
        }
        cancel_->setEnabled(false);
        cancel_->setText(QStringLiteral("…"));
        emit cancelRequested();
    });
    layout->addWidget(cancel_);
}

void ProcessingOverlay::begin(const QString& title) {
    finishTimer_->stop();
    elapsed_.restart();
    elapsedLabel_->setText(tr("Elapsed: %1:%2:%3")
                              .arg(QStringLiteral("00"), QStringLiteral("00"),
                                   QStringLiteral("00")));
    elapsedTimer_->start();
    settled_ = false;
    title_->setText(title);
    detail_->clear();
    progress_->setRange(0, 0);
    cancel_->setEnabled(true);
    cancel_->setText(tr("Cancel"));
    adjustSize();
    show();
    raise();
}

void ProcessingOverlay::setStage(int stageIndex, int stageCount, const QString& phase,
                                 const QString& detail) {
    if (settled_) return;
    if (stageCount > 0 && stageIndex >= 0) {
        progress_->setRange(0, stageCount);
        progress_->setValue(qBound(0, stageIndex, stageCount));
    } else {
        progress_->setRange(0, 0);
    }
    QString text = phase;
    if (stageIndex >= 0) {
        text = stageCount > 0 ? tr("Stage %1 of %2: %3").arg(stageIndex).arg(stageCount).arg(phase)
                              : tr("Stage %1: %2").arg(stageIndex).arg(phase);
    }
    if (!detail.isEmpty()) text += QStringLiteral(" — ") + detail;
    detail_->setText(text);
    adjustSize();
}

void ProcessingOverlay::finish(bool ok, const QString& summary) {
    settled_ = true;
    elapsedTimer_->stop();
    finishTimer_->stop();
    cancel_->setEnabled(false);
    progress_->setRange(0, 1);
    progress_->setValue(ok ? 1 : 0);
    detail_->setText(summary);
    title_->setText(ok ? tr("Engine ready") : tr("Engine build stopped"));
    if (ok) {
        finishTimer_->start(1500);
    } else {
        cancel_->setText(tr("Dismiss"));
        cancel_->setEnabled(true);
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
