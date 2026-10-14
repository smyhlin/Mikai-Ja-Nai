// The QGraphicsView the comparison is drawn into.
//
// Adapted from AnimeInterp's CompareCanvas. It owns the scene, the camera and
// the gesture rules, and two of those rules are easy to get wrong:
//
//  * **The wheel zooms about the cursor.** Fit never upscales past 1:1: a 480p
//    frame blown up to a large window shows interpolation, not detail.
//  * **The selectors are a row under the picture.** The row is as wide as the
//    picture and flush with its left and right edges. Each name fills the
//    width of the side it belongs to.
#pragma once

#include <QGraphicsView>
#include <QImage>
#include <QPoint>
#include <QRgb>
#include <QString>
#include <QStringList>

#include "CompareLayout.hpp"
#include "PlayerPopups.hpp"

class QAction;
class QGraphicsLineItem;
class QGraphicsSimpleTextItem;
class QLabel;
class QMenu;
class QToolButton;
class QWidget;

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
    /// What the two selectors offer; each opens its own menu, grouped by the
    /// entry's own menu path.
    void setChoices(const QVector<ChoiceEntry>& choices, const QString& selectedA,
                    const QString& selectedB);
    void setFlickerShowA(bool showA);
    /// -1 for neither, 0 for A, 1 for B: the hold-to-peek override.
    void setHeldSide(int side);
    int heldSide() const { return heldSide_; }

    Camera& camera() { return camera_; }
    const Camera& camera() const { return camera_; }
    void setSplit(double fraction);
    void setFlickerRate(double hz);
    void setDiffGain(int gain);

    void clear(const QString& message);
    int imageWidth() const;

    void fitToContent();
    void applyView();
    /// Bilinear when true, nearest neighbor when false.
    void setSmoothScaling(bool smooth);

    QPointF scenePointAt(const QPoint& widgetPoint) const;
    bool pixelAt(const QPoint& widgetPoint, int& x, int& y, uchar& b, uchar& g, uchar& r,
                 QRgb* argb = nullptr) const;

signals:
    void zoomChanged(int percent);
    void splitDragged(double fraction);
    void hoverPixel(int x, int y, bool valid, QRgb rgb);
    void candidateChosen(int side, const QString& id);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void drawForeground(QPainter* painter, const QRectF& rect) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool viewTouched_ = false;
    void applyLayout();
    void placeBadges();
    void layoutSelectorRow();
    QRect imageBarRect() const;
    void updateBadge(QToolButton* badge, int side);
    void placeBadge(QToolButton* badge, const QString& label, bool prominent);
    void showStatus(const QString& text);
    bool nearDivider(const QPoint& viewportPos) const;

    CompareImageItem* itemA_ = nullptr;
    CompareImageItem* itemB_ = nullptr;
    CompareImageItem* itemDifference_ = nullptr;
    QGraphicsLineItem* divider_ = nullptr;
    QGraphicsSimpleTextItem* message_ = nullptr;
    QLabel* status_ = nullptr;
    QWidget* selectorRow_ = nullptr;
    QToolButton* badgeA_ = nullptr;
    QToolButton* badgeB_ = nullptr;
    QString labelA_;
    QString labelB_;
    QMenu* menuA_ = nullptr;
    QMenu* menuB_ = nullptr;

    QImage imageA_;
    QImage imageB_;
    QImage imageDifference_;
    QString nameA_;
    QString nameB_;
    QVector<ChoiceEntry> choices_;
    /// What the badge menus were built from and the actions they hold, tracked
    /// per side so updating A never skips rebuilding B.
    QVector<ChoiceEntry> builtChoicesA_;
    QVector<ChoiceEntry> builtChoicesB_;
    QVector<QAction*> leavesA_;
    QVector<QAction*> leavesB_;
    QString selectedA_;
    QString selectedB_;

    Camera camera_;
    CompareMode mode_ = CompareMode::SideBySide;
    QSize imageSize_;
    int heldSide_ = -1;
    bool flickerShowA_ = true;
    bool dragging_ = false;
    bool panning_ = false;
    QPoint lastPanPoint_;
};

}  // namespace mplayer::shell
