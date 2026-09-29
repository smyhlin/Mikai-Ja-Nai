// The three panels the shell has no room for: the quality list, the dub/sub
// picker and the episode drawer.
//
// Unlike the rest of the chrome these are stylesheet-driven rather than
// self-painted, because the strip and the status panel around them are too.

#pragma once

#include <QFrame>
#include <QPoint>
#include <QStringList>
#include <QVariant>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QKeyEvent;
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
};

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
    int rowCount() const;
    /// Re-fits to the size hint, after the host changed something out of band.
    void refresh();

    /// Places the popover above the anchor, flipping below when needed, then
    /// clamps both axes to its parent.
    void popupNear(const QPoint& anchorGlobal, bool preferAbove = true);

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

}  // namespace shell
}  // namespace mplayer
