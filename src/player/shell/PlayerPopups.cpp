#include "PlayerPopups.hpp"
#include "ShellTheme.hpp"

#include "gui/UiProfile.hpp"

#include <QAction>
#include <QApplication>
#include <QToolTip>
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
#include <QTimer>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

namespace mplayer {
namespace shell {
namespace {

constexpr int kSpacingXs = 4;
constexpr int kSpacingSmall = 8;
constexpr int kRowHeight = 28;

// Qt's QWIDGETSIZE_MAX is a C++ macro; `refresh` needs it to undo the fixed
// geometry a previous popupNear left behind.
constexpr int kUnlimited = 16777215;

const char* const kPopupText = "#f0f0f5";
const char* const kPopupMuted = "#9a9aa6";

/// The muted ink the popover's captions, headings and inactive rows share.
QColor popupMuted() { return QColor(0x9a, 0x9a, 0xa6); }

/// A caption/heading face: the pixel sizes, the bold weight and the tracking
/// the popover's caption QSS carried, on the same UI family as the chrome.
QFont popupFont(int pixelSize) {
    QFont font = uiFont(11, true);
    font.setPixelSize(pixelSize);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
    return font;
}

/// The choice menu's selection follows the pushed accent, so the player's menus
/// agree with the window's chips. Rebuilt per menu because the token can change
/// while a player window is open.
QString choiceMenuQss() {
    const QColor accent = shell::accent();
    return QStringLiteral(
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
               "  background-color: %1;"
               "  color: %2;"
               "}"
               "QMenu::item:disabled {"
               "  color: #6b7280;"
               "  background-color: transparent;"
               "}"
               "QMenu::separator {"
               "  height: 1px;"
               "  background: #282c34;"
               "  margin: 4px 6px;"
               "}")
        .arg(accent.name(QColor::HexRgb), shell::inkOn(accent).name(QColor::HexRgb));
}

QString rowQss(const QString& foreground, const QString& fill) {
    return QStringLiteral(
               "QPushButton {"
               "  color: %1;"
               "  background: %2;"
               "  border: 1px solid transparent; border-radius: 6px; text-align: left;"
               "  padding: 0 %3px; min-height: 28px; font-size: 13px; font-weight: 500; }"
               "QPushButton:hover {"
               "  background: rgba(255, 255, 255, 0.12); color: #ffffff;"
               "  border-color: rgba(255, 255, 255, 0.20); }")
        .arg(foreground, fill)
        .arg(kSpacingSmall);
}

/// The engine-build progress chunk, in the pushed accent.
QString progressQss() {
    return QStringLiteral("QProgressBar {"
                          "  background-color: #1e2128;"
                          "  border: none; border-radius: 3px; }"
                          "QProgressBar::chunk {"
                          "  background-color: %1; border-radius: 3px; }")
        .arg(shell::accent().name(QColor::HexRgb));
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
    const QString menuSheet = choiceMenuQss();
    menu->setStyleSheet(menuSheet);
    menu->setToolTipsVisible(true);

    // One submenu per distinct path, not one per entry: addMenu() always
    // creates a new menu, so walking the path per entry would put a second
    // "Anime4K" branch beside the first for every candidate in the list.
    QHash<QString, QMenu*> levels;
    const auto levelFor = [menu, &levels, &menuSheet](const QStringList& path) {
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
            created->setStyleSheet(menuSheet);
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
    setFocusPolicy(Qt::StrongFocus);
    // An open popup keeps painting its row washes from the previous accent
    // unless the rows are restated and their cached pixels dropped: a row reads
    // the accent at paint time, not at build time.
    connect(&accentNotifier(), &AccentNotifier::changed, this, [this] {
        paintRows();
        for (ChromeRow* row : rows_) {
            chromeUpdate(row);
        }
    });
    hide();

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kSpacingSmall, kSpacingSmall, kSpacingSmall, kSpacingXs);
    outer->setSpacing(kSpacingXs);

    caption_ = new ChromeLabel(title, popupFont(11), popupMuted(), this);
    caption_->setObjectName(QStringLiteral("playerMenuCaption"));
    outer->addWidget(caption_);

    body_ = new QVBoxLayout();
    body_->setContentsMargins(0, 0, 0, 0);
    body_->setSpacing(2);
    outer->addLayout(body_);

    // A long hint would set the popover's width, and a quality list two rows
    // high does not want to be 500 px wide.
    auto* hint = new ChromeLabel(QStringLiteral("↑↓ · Enter · Esc"), popupFont(10), popupMuted(),
                                 this);
    hint->setObjectName(QStringLiteral("playerMenuHint"));
    outer->addWidget(hint);

    setToolTip(tr("Controls: ↑ ↓ — select, Enter — confirm, Esc — close"));

    // A popover is created long after the host walked its tree, so the card and
    // its caption/hint are adopted here, in build order: the card has to be in
    // the paint list before the rows that sit on it. Rows are adopted by
    // setEntries() as it creates them.
    if (ChromeHost* host = chromeHostOf(this)) {
        host->chromeAdopt(this);
    }
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

    // The popover outlives its rows and the host walked it before they existed,
    // so every row built here is adopted explicitly, or it would paint its own
    // backing store over the picture again.
    ChromeHost* host = chromeHostOf(this);

    for (const MenuEntry& entry : entries) {
        if (!entry.heading.isEmpty()) {
            auto* label = new ChromeLabel(entry.heading, popupFont(11), popupMuted(), this);
            label->setObjectName(QStringLiteral("playerMenuHeading"));
            label->setChromePadding(6, 4);
            // The label paints inside that inset but measures without it, so
            // the box it is given has to carry the padding or the heading is
            // clipped by exactly the inset it was drawn with.
            const QSize hint = label->sizeHint();
            label->setMinimumSize(hint.width() + 12, hint.height() + 8);
            body_->addWidget(label);
            if (host != nullptr) host->chromeAdopt(label);
            label->show();
            continue;
        }
        const QString text = entry.text;
        auto* row = new ChromeRow(text, this);
        row->setObjectName(QStringLiteral("playerMenuRow"));
        row->setToolTip(entry.tooltip.isEmpty() ? text : entry.tooltip);
        row->setAccessibleName(text);
        row->setCursor(entry.disabled ? Qt::ArrowCursor : Qt::PointingHandCursor);
        row->setFixedHeight(kRowHeight);
        row->setRowPadding(kSpacingSmall);
        row->setSubmenu(entry.submenu);
        row->setEnabled(!entry.disabled);
        // A disabled row is still added to rows_ and rowEntries_ so the two
        // stay index-aligned; it is only the click that it does not have.
        if (!entry.disabled) {
            const int spot = rows_.size();
            connect(row, &ChromeRow::clicked, this, [this, spot] { activate(spot); });
        }
        body_->addWidget(row);
        rows_.append(row);
        rowEntries_.append(entry);
        if (host != nullptr) host->chromeAdopt(row);
        // Qt does not show a widget created after its parent is already visible,
        // and a drill-down rebuilds the rows of an open popover.
        row->show();
    }
    paintRows();
    fitToHint();
}

void MenuPopup::setTitle(const QString& title) {
    if (caption_ == nullptr || caption_->text() == title) {
        return;
    }
    caption_->setText(title);
    // The caption's font and tracking are what its size is tuned for, so the
    // popover is re-fitted: a longer title must not be clipped.
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

void MenuPopup::paintChrome(QPainter& p) {
    enableChromeAntialiasing(p);
    p.setPen(QPen(QColor(255, 255, 255, 45), 1));
    p.setBrush(QColor(20, 21, 27));
    p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 10.0, 10.0);
}

void MenuPopup::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

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
    // The row paints its own standing look from its role, and the keyboard
    // cursor outranks it; a disabled row is never highlighted.
    for (int index = 0; index < rows_.size(); ++index) {
        if (index >= rowEntries_.size())
            continue;
        const MenuEntry& entry = rowEntries_.at(index);
        ChromeRow* row = rows_.at(index);
        row->setRole(entry.disabled ? ChromeRow::Role::Disabled
                                    : (entry.current ? ChromeRow::Role::Current
                                                     : ChromeRow::Role::Choice));
        row->setHighlighted(!entry.disabled && index == highlight_);
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
    progress_->setStyleSheet(progressQss());
    // A build runs for minutes with this window on screen; the chunk it shows
    // must be the accent the viewer picked, including one changed mid-build.
    connect(&accentNotifier(), &AccentNotifier::changed, this,
            [this] { progress_->setStyleSheet(progressQss()); });
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
    syncStackingTimer();
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
    syncStackingTimer();
}

void ProcessingOverlay::hideEvent(QHideEvent* event) {
    stopStackingTimer();
    QFrame::hideEvent(event);
}

bool ProcessingOverlay::stackingPollActive() const {
    return stackingTimer_ != nullptr && stackingTimer_->isActive();
}

void ProcessingOverlay::stopStackingTimer() {
    if (stackingTimer_ == nullptr || !stackingTimer_->isActive()) return;
    stackingTimer_->stop();
}

void ProcessingOverlay::syncStackingTimer() {
    if (stackingTimer_ == nullptr) return;
    if (isVisible() && run_ != Run::Idle) {
        maintainTopmost();
        if (!stackingTimer_->isActive()) stackingTimer_->start();
        return;
    }
    stopStackingTimer();
}

void ProcessingOverlay::maintainTopmost() {
    if (!isVisible()) {
        stopStackingTimer();
        return;
    }
    // A popup the viewer is reading outranks this window's band. Re-asserting
    // topmost here while a menu or tooltip is on screen moves this window above
    // it, because the last topmost request wins - that is how a popup ends up
    // underneath the thing it explains. The timer keeps running, so the band is
    // taken back the moment the popup closes.
    if (QApplication::activePopupWidget() != nullptr || QToolTip::isVisible()) return;
    // A modal box is a child of the player and cannot outrank a topmost window,
    // so the band has to be given up while one is up - otherwise the question
    // the viewer must answer is drawn underneath this panel. The timer keeps
    // running and takes the band back as soon as the box is answered.
    if (QApplication::activeModalWidget() != nullptr) {
        yieldForModal();
        return;
    }
#ifdef Q_OS_WIN
    const HWND window = reinterpret_cast<HWND>(winId());
    if (window == nullptr) return;
    if (yieldedForModal_) {
        // Coming out of a modal box: the band was released, so it has to be
        // requested again rather than assumed.
        yieldedForModal_ = false;
    } else if ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
        // Already where this timer exists to keep it. A repeated SetWindowPos
        // for the same band is a compositor round-trip per tick, and on a GL
        // window that is a repaint the viewer sees.
        stackingFailureReported_ = false;
        return;
    }
    if (SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
        stackingFailureReported_ = false;
    } else if (!stackingFailureReported_) {
        const DWORD error = GetLastError();
        qWarning() << "ProcessingOverlay could not reassert HWND_TOPMOST for HWND"
                   << static_cast<void*>(window) << "; Win32 error" << error;
        stackingFailureReported_ = true;
    }
#else
    // QWidget::raise() is non-activating on X11; the timer is not created on
    // Wayland, where the compositor alone controls global stacking.
    raise();
#endif
}

void ProcessingOverlay::yieldForModal() {
    if (yieldedForModal_) return;
#ifdef Q_OS_WIN
    const HWND window = reinterpret_cast<HWND>(winId());
    if (window != nullptr) {
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
#else
    // X11 stacking is raised, not banded: leaving raise() out of the next tick
    // is the whole yield, and the modal's own window is above this tool window
    // as soon as its owner raises it.
#endif
    yieldedForModal_ = true;
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
        // The user-facing count is one-based; `stageIndex_` stays the zero-based
        // internal position, and a count that is not known keeps the old shape.
        const int displayedStage = stageIndex_ + 1;
        text = stageCount_ > 0
                   ? tr("Stage %1 of %2: %3")
                         .arg(displayedStage)
                         .arg(stageCount_)
                         .arg(stagePhase_)
                   : tr("Stage %1: %2").arg(displayedStage).arg(stagePhase_);
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
