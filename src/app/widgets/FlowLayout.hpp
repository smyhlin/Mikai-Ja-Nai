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
    int doLayout(const QRect& rect, bool testOnly) const {
        const QMargins margins = contentsMargins();
        const QRect content = rect.adjusted(margins.left(), margins.top(),
                                            -margins.right(), -margins.bottom());
        int x = content.x();
        int y = content.y();
        int lineHeight = 0;
        const int right = content.x() + content.width();

        for (QLayoutItem* item : items_) {
            const QSize size = item->sizeHint();
            if (x > content.x() && x + size.width() > right) {
                x = content.x();
                y += lineHeight + verticalSpacing_;
                lineHeight = 0;
            }
            if (!testOnly) {
                item->setGeometry(QRect(QPoint(x, y), size));
            }
            x += size.width() + horizontalSpacing_;
            lineHeight = qMax(lineHeight, size.height());
        }
        return y - rect.y() + lineHeight + margins.bottom();
    }

    QList<QLayoutItem*> items_;
    int horizontalSpacing_;
    int verticalSpacing_;
};

}  // namespace mikai::app
