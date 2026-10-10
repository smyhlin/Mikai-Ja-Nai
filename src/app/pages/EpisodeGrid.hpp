#pragma once

#include <QGridLayout>
#include <QLayoutItem>
#include <QVector>

class QWidget;

namespace mikai::app {

/// Puts the current page's buttons into the grid, `columns` per row.
inline void layoutEpisodeGrid(QGridLayout* grid, const QVector<QWidget*>& buttons, int columns) {
    if (grid == nullptr) {
        return;
    }
    while (QLayoutItem* stale = grid->takeAt(0)) {
        delete stale;
    }
    const int resolved = columns <= 0 ? 1 : columns;
    const int buttonCount = static_cast<int>(buttons.size());
    for (int column = 0; column < grid->columnCount(); ++column) {
        grid->setColumnStretch(column, 0);
    }
    for (int index = 0; index < buttonCount; ++index) {
        if (buttons.at(index) == nullptr) {
            continue;
        }
        const int row = index / resolved;
        const int firstIndex = row * resolved;
        const int buttonsInRow = qMin(resolved, buttonCount - firstIndex);
        const int firstColumn = (resolved - buttonsInRow) / 2;
        grid->addWidget(buttons.at(index), row, firstColumn + index % resolved,
                        Qt::AlignHCenter | Qt::AlignTop);
    }
    for (int column = 0; column < resolved; ++column) {
        grid->setColumnStretch(column, 1);
    }
}

}  // namespace mikai::app
