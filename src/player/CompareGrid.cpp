#include "CompareGrid.hpp"

#include "ComparePane.hpp"
#include "ShellTheme.hpp"

#include <QGridLayout>

#include <algorithm>
#include <cmath>

namespace mplayer::shell {

CompareGrid::CompareGrid(QWidget* parent) : QWidget(parent) {
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(4);
    setAutoFillBackground(true);
    QPalette surface = palette();
    surface.setColor(QPalette::Window, QColor(13, 14, 17));
    setPalette(surface);
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
        ComparePane* pane = nullptr;
        if (i < static_cast<int>(panes_.size())) {
            pane = panes_.at(static_cast<size_t>(i));
        } else {
            pane = new ComparePane(this);
            pane->setSideIndex(i);
            connect(pane, &ComparePane::viewChanged, this, &CompareGrid::reconcile);
            connect(pane, &ComparePane::choiceChosen, this, [this, i](const QString& id) {
                if (i >= 0 && i < ids_.size()) {
                    ids_[i] = id;
                }
                emit choiceChosen(i, id);
            });
            connect(pane, &ComparePane::hoverPixel, this, &CompareGrid::hoverPixel);
            // One row, two columns: the reference's own side_h shape.
            grid_->addWidget(pane, 0, i);
            grid_->setColumnStretch(i, 1);
            panes_.push_back(pane);
        }
        pane->setSideIndex(i);
        pane->setChoices(choices_, ids.at(i));
        pane->setContent(ids.at(i), images.at(i), names.value(i));
        pane->setLocalView(zoom_, center_);
        pane->setSmoothScaling(smoothScaling_);
        pane->show();
    }
    emit zoomChanged(static_cast<int>(std::lround(zoom_ * 100.0)));
}

void CompareGrid::setSmoothScaling(bool smooth) {
    smoothScaling_ = smooth;
    for (ComparePane* pane : panes_) {
        pane->setSmoothScaling(smooth);
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
    emit zoomChanged(static_cast<int>(std::lround(zoom_ * 100.0)));
}

void CompareGrid::reconcile() {
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
    emit zoomChanged(static_cast<int>(std::lround(zoom_ * 100.0)));
}

}  // namespace mplayer::shell
