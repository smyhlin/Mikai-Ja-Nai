#pragma once

// Lays items out left to right and wraps onto the next line when the next item
// does not fit. A row of buttons or chips whose widest translation exceeds the
// window has to wrap: a layout that forces its own width makes the page pan
// sideways, and one that clips hides a control behind a border.
#include <QLayout>
#include <QList>
#include <QRect>
#include <QSize>

namespace mikai::app {

class FlowLayout final : public QLayout {
public:
    FlowLayout(int horizontalSpacing, int verticalSpacing)
        : horizontalSpacing_(horizontalSpacing), verticalSpacing_(verticalSpacing) {
        setContentsMargins(0, 0, 0, 0);
    }

    ~FlowLayout() override {
        while (QLayoutItem* item = takeAt(0)) {
            delete item;
        }
    }

    void addItem(QLayoutItem* item) override { items_.append(item); }

    int count() const override { return static_cast<int>(items_.size()); }

    QLayoutItem* itemAt(int index) const override { return items_.value(index); }

    QLayoutItem* takeAt(int index) override {
        if (index < 0 || index >= items_.size()) {
            return nullptr;
        }
        return items_.takeAt(index);
    }

    Qt::Orientations expandingDirections() const override { return {}; }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override {
        return doLayout(QRect(0, 0, width, 0), true);
    }

    QSize sizeHint() const override { return minimumSize(); }

    QSize minimumSize() const override {
        QSize size;
        for (const QLayoutItem* item : items_) {
            size = size.expandedTo(item->minimumSize());
        }
        const QMargins margins = contentsMargins();
        return size + QSize(margins.left() + margins.right(),
                            margins.top() + margins.bottom());
    }

    void setGeometry(const QRect& rect) override {
        QLayout::setGeometry(rect);
        doLayout(rect, false);
    }

private:
    /// The size this item is allowed to take. A size hint is a preference, and
    /// a stylesheet's box model can inflate it past the size the caller fixed
    /// (a colour chip is 34x34 because setFixedSize says so, not because a
    /// generic button rule agrees). Honouring the item's own bounds keeps a
    /// self-painted control at its intrinsic size and leaves every other
    /// control exactly at its hint, because their bounds are wider than it.
    static QSize fitted(const QLayoutItem* item) {
        const QSize hint = item->sizeHint();
        const QSize minimum = item->minimumSize();
        const QSize maximum = item->maximumSize();
        // A style can raise an item's minimum above the maximum its caller
        // fixed (a 36 px button box model on a 34 px chip). The cap is the
        // author's decision, so the minimum is pulled down to it instead of
        // letting the hint climb past the cap: the interval stays closed and
        // the item lands on the cap, not two pixels above it.
        const int widest = maximum.width() > 0 ? maximum.width() : QLAYOUTSIZE_MAX;
        const int tallest = maximum.height() > 0 ? maximum.height() : QLAYOUTSIZE_MAX;
        const int narrowest = qMin(minimum.width(), widest);
        const int shortest = qMin(minimum.height(), tallest);
        return QSize(qBound(narrowest, hint.width(), widest),
                     qBound(shortest, hint.height(), tallest));
    }

    int doLayout(const QRect& rect, bool testOnly) const {
        const QMargins margins = contentsMargins();
        const QRect content = rect.adjusted(margins.left(), margins.top(),
                                            -margins.right(), -margins.bottom());
        int y = content.y();
        int lineHeight = 0;
        const int right = content.x() + content.width();
        int first = 0;

        while (first < items_.size()) {
            int last = first;
            int rowWidth = 0;
            int rowHeight = 0;
            while (last < items_.size()) {
                const QSize size = fitted(items_.at(last));
                const int nextWidth =
                    last == first ? size.width() : rowWidth + horizontalSpacing_ + size.width();
                if (last > first && content.x() + nextWidth > right) {
                    break;
                }
                rowWidth = nextWidth;
                rowHeight = qMax(rowHeight, size.height());
                ++last;
            }

            int x = content.x();
            for (int index = first; index < last; ++index) {
                QLayoutItem* item = items_.at(index);
                const QSize size = fitted(item);
                const int verticalOffset =
                    item->alignment().testFlag(Qt::AlignVCenter)
                        ? (rowHeight - size.height()) / 2
                        : 0;
                if (!testOnly) {
                    item->setGeometry(QRect(QPoint(x, y + verticalOffset), size));
                }
                x += size.width() + horizontalSpacing_;
            }
            lineHeight += rowHeight;
            if (last < items_.size()) {
                lineHeight += verticalSpacing_;
            }
            y += rowHeight + verticalSpacing_;
            first = last;
        }
        return content.y() - rect.y() + lineHeight + margins.bottom();
    }

    QList<QLayoutItem*> items_;
    int horizontalSpacing_;
    int verticalSpacing_;
};

}  // namespace mikai::app
