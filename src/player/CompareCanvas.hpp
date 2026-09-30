// The QGraphicsView the comparison is drawn into.
//
// Adapted from AnimeInterp's CompareCanvas. It owns the scene, the camera and
// the gesture rules, and two of those rules are easy to get wrong:
//
//  * **The wheel does not zoom.** The view is the fit and nothing else: a
//    comparison judged on a picture the viewer has silently rescaled is a
//    comparison of the interpolation, not of the chain.
//  * **Fit never upscales past 1:1.** A 480p frame blown up to a large window
//    shows interpolation artefacts, not detail.
//  * **The side badges are viewport children, re-anchored after every scroll.**
//    QAbstractScrollArea moves the viewport's children along with the scene, so
//    a badge placed once rides out of view on the first pan - and, once the
//    offset is large, clean off the edge entirely, which is how a pane ends up
//    labelled with a different picture than it is showing.
#pragma once

#include <QGraphicsView>
#include <QImage>
#include <QPoint>
#include <QString>
#include <QStringList>

#include "CompareLayout.hpp"

class QGraphicsLineItem;
class QGraphicsSimpleTextItem;
class QMenu;
class QToolButton;

namespace mplayer::shell {

class CompareImageItem;

class CompareCanvas : public QGraphicsView {
    Q_OBJECT

public:
    explicit CompareCanvas(QWidget* parent = nullptr);

    void setMode(CompareMode mode);
    CompareMode mode() const { return mode_; }

    void setSideImages(const QImage& a, const QImage& b, const QImage& difference);
    void setSideNames(const QString& a, const QString& b);
    /// The ids and labels behind the two badges; each badge opens its own menu.
    void setCandidates(const QStringList& ids, const QStringList& labels,
                       const QString& selectedA, const QString& selectedB);
    void setFlickerShowA(bool showA);
    /// -1 for neither, 0 for A, 1 for B: the hold-to-peek override.
    void setHeldSide(int side);

    Camera& camera() { return camera_; }
    const Camera& camera() const { return camera_; }
    void setSplit(double fraction);
    void setOpacity(double fraction);
    void setFlickerRate(double hz);
    void setDiffGain(int gain);

    void clear(const QString& message);
    int imageWidth() const;

    void fitToContent();
    void applyView();

    QPointF scenePointAt(const QPoint& widgetPoint) const;
    bool pixelAt(const QPoint& widgetPoint, int& x, int& y, uchar& b, uchar& g, uchar& r) const;

signals:
    void zoomChanged(int percent);
    void splitDragged(double fraction);
    void hoverPixel(int x, int y, bool valid);
    void candidateChosen(int side, const QString& id);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    bool viewTouched_ = false;
    void applyLayout();
    void placeBadges();
    void updateBadge(QToolButton* badge, int side);

    CompareImageItem* itemA_ = nullptr;
    CompareImageItem* itemB_ = nullptr;
    CompareImageItem* itemDifference_ = nullptr;
    QGraphicsLineItem* divider_ = nullptr;
    QGraphicsSimpleTextItem* message_ = nullptr;
    QToolButton* badgeA_ = nullptr;
    QToolButton* badgeB_ = nullptr;
    QMenu* menuA_ = nullptr;
    QMenu* menuB_ = nullptr;

    QImage imageA_;
    QImage imageB_;
    QImage imageDifference_;
    QString nameA_;
    QString nameB_;
    QStringList candidateIds_;
    QStringList candidateLabels_;
    QString selectedA_;
    QString selectedB_;

    Camera camera_;
    CompareMode mode_ = CompareMode::Original;
    QSize imageSize_;
    int heldSide_ = -1;
    bool flickerShowA_ = true;
    bool dragging_ = false;
    bool panning_ = false;
    QPoint lastPanPoint_;
};

}  // namespace mplayer::shell
