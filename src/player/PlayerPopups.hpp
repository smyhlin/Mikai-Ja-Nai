// The three panels the shell has no room for: the quality list, the dub/sub
// picker and the episode drawer.
//
// Unlike the rest of the chrome these are stylesheet-driven rather than
// self-painted, because the strip and the status panel around them are too.

#pragma once

#include <QFrame>
#include <QPoint>
#include <QRect>
#include <QStringList>
#include <QVariant>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QAction;
class QKeyEvent;
class QLabel;
class QMenu;
class QProgressBar;
class QPushButton;
class QVBoxLayout;
QT_END_NAMESPACE

namespace mplayer {
namespace shell {

/// One row of a popover. A non-empty `heading` is a section label, not a
/// selectable row.
struct MenuEntry {
    QString heading;
    QString text;
    QString tooltip;
    QVariant payload;
    bool current = false;
    /// A row that leads to another level: activating it leaves the popover
    /// open, so the caller can place that level's panel beside this one. A
    /// plain row closes the popover, which is what a terminal choice does.
    bool submenu = false;
    /// A row that is shown but cannot be chosen - used for the models that are
    /// only planned, so the menu says so instead of hiding them. A disabled row
    /// never emits, so it can never change the selection.
    bool disabled = false;
};

/// One entry of a candidate chooser with the menu path it sits under, outermost
/// level first (empty path is a flat list).
struct ChoiceEntry {
    QString id;
    QString label;
    QStringList path;

    bool operator==(const ChoiceEntry& other) const {
        return id == other.id && label == other.label && path == other.path;
    }
    bool operator!=(const ChoiceEntry& other) const { return !(*this == other); }
};

/// Builds `menu` as one checkable action per entry, nested one submenu per path
/// level, and hands back the leaf actions in entry order. Building destroys the
/// previous tree, so callers keep those leaves and use `markChoice` while the
/// list is unchanged.
void fillChoiceMenu(QMenu* menu, const QVector<ChoiceEntry>& entries, const QString& selected,
                    QVector<QAction*>* leaves = nullptr);

/// Moves only the check marks, using the leaves of a previous fill: no action
/// is created or destroyed, which is what makes it safe to call from a menu
/// action's own handler.
void markChoice(const QVector<QAction*>& leaves, const QVector<ChoiceEntry>& entries,
                const QString& selected);

/// In-window popover listing a handful of choices.
///
/// It remains a child of the fullscreen player instead of becoming a native
/// window, because Windows can composite a separate popup behind its fullscreen
/// owner even while Qt reports it visible.
///
/// Its rows are `Qt::NoFocus` on purpose: a focused QPushButton swallows Space,
/// and Space belongs to the shell as play/pause. The popover takes focus itself
/// and drives the rows with the arrow keys, so it stays keyboard reachable
/// without taking a key the player cannot spare.
class MenuPopup : public QFrame {
    Q_OBJECT
public:
    explicit MenuPopup(const QString& title, QWidget* parent = nullptr);

    /// Rebuilds from scratch: a swapped stream replaces the whole packet behind
    /// a row, so repainting the standing rows would show the qualities of
    /// whatever was playing before.
    void setEntries(const QVector<MenuEntry>& entries);
    /// Retitles the caption. One popover serves several menus - the enhancement
    /// drill-down is the Anime4K menu at one level and a planned-model page at
    /// another - so the title has to follow the level.
    void setTitle(const QString& title);
    int rowCount() const;
    /// The widget behind a row, for a host that needs per-row gestures such as
    /// a hover that opens a submenu. Null for a spot that is not a row.
    QWidget* rowWidget(int spot) const;
    /// The entry behind a row. Indices count rows only - headings are skipped -
    /// so they are the same indices rowWidget() takes.
    MenuEntry entryAt(int spot) const;
    /// Re-fits to the size hint, after the host changed something out of band.
    void refresh();

    /// Places the popover above the anchor, flipping below when needed, then
    /// clamps both axes to its parent.
    void popupNear(const QPoint& anchorGlobal, bool preferAbove = true);

    /// Places the popover beside an anchor rectangle given in its parent's own
    /// coordinates: top-aligned to it, to the left when `preferLeft` is set and
    /// to the right otherwise, then clamped to the parent. This is what lets a
    /// whole tree of levels stand on screen at the same time.
    void popupBeside(const QRect& anchor, bool preferLeft, int gap = 6);

signals:
    void picked(const QVariant& payload);

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void fitToHint();
    void moveHighlight(int delta);
    void setHighlight(int spot);
    void activate(int spot);
    void paintRows();

    QLabel* caption_ = nullptr;
    QVBoxLayout* body_ = nullptr;
    QVector<QPushButton*> rows_;
    QVector<MenuEntry> rowEntries_;
    int highlight_ = -1;
};

class SeriesDrawer : public QFrame {
    Q_OBJECT
public:
    explicit SeriesDrawer(QWidget* parent = nullptr);

    void setEntries(const QStringList& labels, int current);
    /// Natural height of the scrolled list, for the dialog to size the drawer.
    int contentHeight() const;

signals:
    void picked(int index);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVBoxLayout* rowsLayout_ = nullptr;
    QVector<QPushButton*> rows_;
};

/// The engine-preparation window: what the player shows while a TensorRT engine
/// is being built.
///
/// It is a native tool window rather than an in-window child, unlike every other
/// popover here, because a build is started from the settings dialog and that
/// dialog is modal: a child of the player would sit behind it and the work would
/// be invisible for the whole build. Staying on top of the player is what makes
/// a long compile visible while the viewer keeps watching.
///
/// No percentage is invented. The bar steps over the plan's own stage count -
/// `BuilderProgress::stageIndex` against `PreparationPlan::stages` - and a stage
/// count of zero leaves it indeterminate rather than guessing.
class ProcessingOverlay : public QFrame {
    Q_OBJECT
public:
    explicit ProcessingOverlay(QWidget* parent = nullptr);

    /// Titles the window and starts the indeterminate phase, before the plan has
    /// resolved and therefore before any stage count exists.
    void begin(const QString& title);
    /// One real progress event. `stageCount` is the plan's stage count; 0 means
    /// it is not known yet and the bar stays indeterminate.
    void setStage(int stageIndex, int stageCount, const QString& phase, const QString& detail);
    /// The build settled. `summary` is shown; a failure keeps the window up so a
    /// refusal cannot vanish unread.
    void finish(bool ok, const QString& summary);

    /// Centres the window near the top of `anchor`'s screen.
    void anchorTo(const QWidget* anchor);

signals:
    void cancelRequested();

private:
    QLabel* title_ = nullptr;
    QLabel* detail_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* cancel_ = nullptr;
    bool settled_ = false;
};

}  // namespace shell
}  // namespace mplayer
