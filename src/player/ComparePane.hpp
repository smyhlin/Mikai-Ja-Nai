// One cell of the comparison grid.
//
// Adapted from AnimeInterp's ComparePane. The pane owns its picture and its
// own fit (which may upscale: the pane fills its cell); the shared view is a
// normalized centre, so a 480p pane and a 1080p pane agree on which region of
// the frame is centred. The scale is that fit and nothing else: the wheel only
// scrolls the pane and the drag only pans it, so neither can rescale a
// comparison out from under the eye.
//
// The model selector is a row under the picture, in the view's bottom margin.
// It is exactly as wide as the picture and flush with its left and right
// edges, and the name fills that row. Zoom never lays it over the image. The
// menu offers every candidate the grid feeds it, with the shown one checked.
#pragma once

#include <QGraphicsView>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <optional>

#include "CompareLayout.hpp"
#include "PlayerPopups.hpp"

class QAction;
class QGraphicsPixmapItem;
class QGraphicsScene;
class QMenu;
class QToolButton;
class QWidget;

namespace mplayer::shell {

class ComparePane : public QGraphicsView {
    Q_OBJECT

public:
    explicit ComparePane(QWidget* parent = nullptr);

    QString key() const { return key_; }

    void setContent(const QString& key, const QImage& image, const QString& name);
    void setChoices(const QVector<ChoiceEntry>& choices, const QString& current);

    /// Adopt a shared view: `zoom` is a multiple of this pane's own fit and the
    /// centre is normalized (fractions of its own picture).
    void setLocalView(double zoom, const std::optional<QPointF>& center);
    /// Re-fit to the picture; the shared zoom and centre survive it.
    void fit();
    /// Bilinear when true, nearest neighbor when false.
    void setSmoothScaling(bool smooth);

    double zoom() const { return zoom_; }
    std::optional<QPointF> center() const;

    QSize sizeHint() const override { return {480, 320}; }

signals:
    /// The user panned here; the grid reconciles the shared centre.
    void viewChanged();
    /// The selector asked for another candidate in this pane.
    void choiceChosen(const QString& id);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refreshBadge();
    void syncBadgeMenu();
    void placeSelector();
    void layoutSelectorRow();
    QRect imageBarRect() const;
    QSize imageSize() const;
    QPointF centerNormalized() const;

    QGraphicsScene* scene_ = nullptr;
    QGraphicsPixmapItem* item_ = nullptr;
    QWidget* selectorBar_ = nullptr;
    QToolButton* badge_ = nullptr;
    QMenu* menu_ = nullptr;
    QString label_;

    QString key_;
    QImage image_;
    QString name_;
    QVector<ChoiceEntry> choices_;
    /// What this pane's menu was built from and the actions it holds: a refill
    /// that changes nothing only moves the check marks, so no action is
    /// destroyed from inside its own handler.
    QVector<ChoiceEntry> builtChoices_;
    QVector<QAction*> leaves_;
    QString current_;

    /// True while this pane is writing its own view, so the scroll it causes is
    /// not reported back to the group as if the user had dragged it.
    bool syncing_ = false;
    bool altPanning_ = false;
    QPoint lastPanPoint_;

    /// The fit scale of the picture, recorded by fit().
    double base_ = 1.0;
    /// Fit-relative zoom, the shared camera's number.
    double zoom_ = 1.0;
};

}  // namespace mplayer::shell
