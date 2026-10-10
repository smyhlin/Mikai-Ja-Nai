// The multi-pane surface the Side-by-side mode shows.
//
// Adapted from AnimeInterp's CompareGrid, bounded to what this window uses:
// one or two panes and one shared view. Pane i shows picture i; every badge
// offers every candidate, so a result that is not on screen is still one click
// away. The shared view is what two panes must agree on: a normalized centre,
// so 480p and 1080p panes look at the same region at their own fits.
#pragma once

#include <QImage>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <optional>
#include <vector>

#include "PlayerPopups.hpp"

class QGridLayout;

namespace mplayer::shell {

class ComparePane;

class CompareGrid : public QWidget {
    Q_OBJECT

public:
    explicit CompareGrid(QWidget* parent = nullptr);

    /// Rebuild the panes for `ids` and their pictures. Badge i names `names[i]`
    /// and its menu offers the candidates from the last setChoices call.
    void setItems(const QStringList& ids, const std::vector<QImage>& images,
                  const QStringList& names);
    /// What every badge's menu offers, ahead of (or refreshed between)
    /// setItems calls.
    void setChoices(const QVector<ChoiceEntry>& choices);
    void fitToContent();

signals:
    /// A badge chose another candidate for pane `index`.
    void choiceChosen(int index, const QString& id);

private slots:
    void reconcile();

private:
    ComparePane* paneAt(int index) const;

    QGridLayout* grid_ = nullptr;
    std::vector<ComparePane*> panes_;
    QStringList ids_;
    QVector<ChoiceEntry> choices_;
    std::optional<QPointF> center_;
    double zoom_ = 1.0;
};

}  // namespace mplayer::shell
