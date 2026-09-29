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
    for (int index = 0; index < buttons.size(); ++index) {
        if (buttons.at(index) != nullptr) {
            grid->addWidget(buttons.at(index), index / resolved, index % resolved, Qt::AlignTop);
        }
    }
    for (int column = 0; column < resolved; ++column) {
        grid->setColumnStretch(column, 1);
    }
}

}  // namespace mikai::app
