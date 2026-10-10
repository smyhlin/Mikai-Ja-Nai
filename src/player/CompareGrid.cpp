#include "CompareGrid.hpp"

#include "ComparePane.hpp"

#include <QGridLayout>

#include <algorithm>

namespace mplayer::shell {

CompareGrid::CompareGrid(QWidget* parent) : QWidget(parent) {
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(2);
}

void CompareGrid::setItems(const QStringList& ids, const std::vector<QImage>& images,
                           const QStringList& names) {
    const int count = std::min<int>(ids.size(), images.size());
    ids_ = ids.size() > count ? ids.mid(0, count) : ids;

    while (static_cast<int>(panes_.size()) > count) {
        delete panes_.back();
        panes_.pop_back();
    }
    for (int i = 0; i < count; ++i) {
        ComparePane* pane = paneAt(i);
        if (pane == nullptr) {
            pane = new ComparePane(this);
            connect(pane, &ComparePane::viewChanged, this, &CompareGrid::reconcile);
            connect(pane, &ComparePane::choiceChosen, this, [this, i](const QString& id) {
                emit choiceChosen(i, id);
            });
            // One row, two columns: the reference's own side_h shape.
            grid_->addWidget(pane, 0, i);
            panes_.push_back(pane);
        }
        pane->setContent(ids.at(i), images.at(i), names.value(i));
        pane->setChoices(choices_, ids.at(i));
        pane->setLocalView(zoom_, center_);
    }
}

void CompareGrid::setChoices(const QVector<ChoiceEntry>& choices) {
    choices_ = choices;
    for (int i = 0; i < static_cast<int>(panes_.size()); ++i) {
        panes_.at(i)->setChoices(choices, ids_.value(i));
    }
}

void CompareGrid::fitToContent() {
    zoom_ = 1.0;
    center_ = QPointF(0.5, 0.5);
    for (ComparePane* pane : panes_) {
        pane->fit();
        pane->setLocalView(zoom_, center_);
    }
}

ComparePane* CompareGrid::paneAt(int index) const {
    if (index < 0 || index >= static_cast<int>(panes_.size())) {
        return nullptr;
    }
    return panes_.at(index);
}

void CompareGrid::reconcile() {
    // One pane moved; every other pane follows the same region. The centre is
    // normalized and the zoom is fit-relative, so the same numbers mean the
    // same region in every pane whatever their resolutions.
    auto* sender = qobject_cast<ComparePane*>(QObject::sender());
    if (sender == nullptr) {
        return;
    }
    zoom_ = sender->zoom();
    if (const auto c = sender->center(); c.has_value()) {
        center_ = c;
    }
    for (ComparePane* pane : panes_) {
        if (pane != sender) {
            pane->setLocalView(zoom_, center_);
        }
    }
}

}  // namespace mplayer::shell
