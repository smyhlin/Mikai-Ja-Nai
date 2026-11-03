#pragma once

#include "theme/Metrics.hpp"
#include "theme/Theme.hpp"

#include <QAbstractButton>
#include <QList>
#include <QLayout>
#include <QLayoutItem>
#include <QSize>
#include <QWidget>

namespace mikai::app {

/// One group of actions on one width, re-flowed into as many columns as the
/// available width holds so a long translation adds a row instead of clipping.
class ActionGrid final : public QLayout {
 public:
    explicit ActionGrid(int spacing = theme::kSpacingSmall, QWidget* parent = nullptr)
        : QLayout(parent), spacing_(spacing) {
        setContentsMargins(0, 0, 0, 0);
    }
    ~ActionGrid() override {
        while (!items_.isEmpty()) delete items_.takeFirst();
    }

    /// Registers the button like QLayout::addWidget does, so a button built
    /// against another parent is reparented and its item reaches addItem once.
    void addButton(QAbstractButton* button) {
        if (button != nullptr) addWidget(button);
    }

    /// Measures the group again from the font its captions now render in.
    void recalculate(int minimum = 0) {
        theme::equalizeWidths(std::span<QAbstractButton* const>(buttons_.data(), buttons_.size()),
                              minimum);
        invalidate();
        if (parentWidget() != nullptr) parentWidget()->updateGeometry();
    }

    int count() const override { return static_cast<int>(items_.size()); }

    QLayoutItem* itemAt(int index) const override {
        return index >= 0 && index < items_.size() ? items_.at(index) : nullptr;
    }

    QLayoutItem* takeAt(int index) override {
        if (index < 0 || index >= items_.size()) return nullptr;
        QLayoutItem* item = items_.takeAt(index);
        if (item != nullptr && item->widget() != nullptr) {
            buttons_.removeAll(qobject_cast<QAbstractButton*>(item->widget()));
        }
        return item;
    }

    void addItem(QLayoutItem* item) override {
        if (item == nullptr) return;
        items_.append(item);
        QAbstractButton* button = qobject_cast<QAbstractButton*>(item->widget());
        if (button != nullptr && !buttons_.contains(button)) buttons_.append(button);
    }

    Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override {
        if (items_.isEmpty()) return 0;
        const int rows = rowCount(width);
        return rows * rowHeight() + (rows - 1) * spacing_;
    }

    QSize sizeHint() const override {
        const int column = minimumColumnWidth();
        return QSize(column, heightForWidth(column));
    }

    QSize minimumSize() const override {
        return QSize(minimumColumnWidth(), rowHeight());
    }

    void setGeometry(const QRect& rect) override {
        QLayout::setGeometry(rect);
        if (items_.isEmpty()) return;
        const QRect area = contentsRect();
        const int columns = columnCount(area.width());
        const int cellWidth = qMax(0, (area.width() - (columns - 1) * spacing_) / columns);
        const int cellHeight = rowHeight();
        for (int index = 0; index < items_.size(); ++index) {
            QLayoutItem* item = items_.at(index);
            if (item == nullptr) continue;
            const int x = area.x() + (index % columns) * (cellWidth + spacing_);
            const int y = area.y() + (index / columns) * (cellHeight + spacing_);
            item->setGeometry(QRect(QPoint(x, y), QSize(cellWidth, cellHeight)));
        }
    }

 private:
    int rowHeight() const {
        int height = 0;
        for (const QLayoutItem* item : items_) {
            height = qMax(height, item->sizeHint().height());
        }
        return qMax(height, theme::kControlHeight);
    }

    /// The widest caption's minimum: below it the group wraps instead of
    /// squeezing a button under the text it has to show.
    int minimumColumnWidth() const {
        int width = 0;
        for (const QAbstractButton* button : buttons_) {
            width = qMax(width, button->minimumWidth());
        }
        return qMax(width, theme::kControlHeight);
    }

    int columnCount(int width) const {
        const int column = minimumColumnWidth();
        if (column <= 0) return 1;
        return qBound(1, (width + spacing_) / (column + spacing_), qMax(1, count()));
    }

    int rowCount(int width) const {
        const int columns = columnCount(width);
        return (count() + columns - 1) / columns;
    }

    QList<QLayoutItem*> items_;
    QList<QAbstractButton*> buttons_;
    int spacing_ = theme::kSpacingSmall;
};

}  // namespace mikai::app
