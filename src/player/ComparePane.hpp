// One cell of the comparison grid.
//
// Adapted from AnimeInterp's ComparePane. The pane owns its picture and its
// own fit (which may upscale: the pane fills its cell); the shared view is a
// normalized centre, so a 480p pane and a 1080p pane agree on which region of
// the frame is centred. The scale is that fit and nothing else: the wheel only
// scrolls the pane and the drag only pans it, so neither can rescale a
// comparison out from under the eye.
//
// The badge is a viewport child, re-anchored after every scroll: QAbstract-
// ScrollArea moves the viewport's children along with the scene, so a badge
// placed once would ride out of view on the first pan - which is how a pane
// ends up labelled with a picture it is not showing. The badge menu offers
// every candidate the grid feeds it, with the shown one checked.
#pragma once

#include <QGraphicsView>
#include <QImage>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <optional>

#include "CompareLayout.hpp"

class QGraphicsPixmapItem;
class QGraphicsScene;
class QMenu;
class QToolButton;

namespace mplayer::shell {

class ComparePane : public QGraphicsView {
    Q_OBJECT

public:
    explicit ComparePane(QWidget* parent = nullptr);

    QString key() const { return key_; }

    void setContent(const QString& key, const QImage& image, const QString& name);
    void setChoices(const QStringList& ids, const QStringList& labels, const QString& current);

    /// Adopt a shared view: `zoom` is a multiple of this pane's own fit and the
    /// centre is normalized (fractions of its own picture).
    void setLocalView(double zoom, const std::optional<QPointF>& center);
    /// Re-fit to the picture; the shared zoom and centre survive it.
    void fit();

    double zoom() const { return zoom_; }
    std::optional<QPointF> center() const;

    QSize sizeHint() const override { return {480, 320}; }

signals:
    /// The user panned here; the grid reconciles the shared centre.
    void viewChanged();
    /// The badge menu asked for another candidate in this pane.
    void choiceChosen(const QString& id);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    void refreshBadge();
    void syncBadgeMenu();
    void anchorBadge();
    QSize imageSize() const;
    QPointF centerNormalized() const;

    QGraphicsScene* scene_ = nullptr;
    QGraphicsPixmapItem* item_ = nullptr;
    QToolButton* badge_ = nullptr;
    QMenu* menu_ = nullptr;

    QString key_;
    QImage image_;
    QString name_;
    QStringList choiceIds_;
    QStringList choiceLabels_;
    QString current_;

    /// True while this pane is writing its own view, so the scroll it causes is
    /// not reported back to the group as if the user had dragged it.
    bool syncing_ = false;

    /// The fit scale of the picture, recorded by fit().
    double base_ = 1.0;
    /// Fit-relative zoom, the shared camera's number.
    double zoom_ = 1.0;
};

}  // namespace mplayer::shell
