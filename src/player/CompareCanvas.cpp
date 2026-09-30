#include "CompareCanvas.hpp"

#include "ShellTheme.hpp"

#include <QAction>
#include <QGraphicsItem>
#include <QGraphicsLineItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QToolButton>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>

namespace mplayer::shell {

namespace {
constexpr int kBadgeInset = 8;
}  // namespace

/// A picture that can be clipped. Split is drawn by clipping each side of the
/// *same* frozen frame, so the divide costs no recomposite and no re-render.
class CompareImageItem final : public QGraphicsItem {
public:
    CompareImageItem() { setAcceptedMouseButtons(Qt::NoButton); }

    void setImage(const QImage& image) {
        prepareGeometryChange();
        image_ = image;
        clip_ = QRectF(image.rect());
        update();
    }
    void setClipRect(const QRectF& clip) {
        prepareGeometryChange();
        clip_ = clip;
        update();
    }
    QRectF boundingRect() const override { return clip_; }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        if (!image_.isNull() && !clip_.isEmpty()) painter->drawImage(clip_, image_, clip_);
    }

private:
    QImage image_;
    QRectF clip_;
};

CompareCanvas::CompareCanvas(QWidget* parent) : QGraphicsView(parent) {
    setScene(new QGraphicsScene(this));
    setRenderHint(QPainter::Antialiasing, true);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setDragMode(QGraphicsView::NoDrag);
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setResizeAnchor(QGraphicsView::NoAnchor);
    setFrameShape(QFrame::NoFrame);
    setBackgroundBrush(QColor(5, 6, 10));
    setMouseTracking(true);

    itemA_ = new CompareImageItem;
    itemB_ = new CompareImageItem;
    itemDifference_ = new CompareImageItem;
    scene()->addItem(itemA_);
    scene()->addItem(itemB_);
    scene()->addItem(itemDifference_);
    divider_ = scene()->addLine(QLineF(), QPen(QColor(255, 255, 255), 2));
    message_ = scene()->addSimpleText(QString());
    message_->setBrush(QBrush(Qt::white));
    itemDifference_->setZValue(2);
    divider_->setZValue(10);

    const QString badgeStyle = QStringLiteral(
        "QToolButton { background: rgba(16,16,16,190); color: #E8EAED;"
        " border: 1px solid rgba(255,255,255,60); border-radius: 4px;"
        " padding: 2px 8px; text-align: left; }"
        "QToolButton:hover { border-color: #7AB7FF; }");
    badgeA_ = new QToolButton(viewport());
    badgeB_ = new QToolButton(viewport());
    menuA_ = new QMenu(badgeA_);
    menuB_ = new QMenu(badgeB_);
    for (QToolButton* badge : {badgeA_, badgeB_}) {
        badge->setCursor(Qt::PointingHandCursor);
        badge->setStyleSheet(badgeStyle);
        badge->setToolButtonStyle(Qt::ToolButtonTextOnly);
        badge->setPopupMode(QToolButton::InstantPopup);
        badge->setMenu(badge == badgeA_ ? menuA_ : menuB_);
        connect(badge->menu(), &QMenu::triggered, this, [this, badge](QAction* action) {
            emit candidateChosen(badge == badgeA_ ? 0 : 1, action->data().toString());
        });
    }
    setCandidates({}, {}, QString(), QString());
}

void CompareCanvas::setMode(CompareMode mode) {
    if (mode == mode_) {
        return;
    }
    mode_ = mode;
    applyLayout();
}

void CompareCanvas::setSideImages(const QImage& a, const QImage& b, const QImage& difference) {
    imageA_ = a;
    imageB_ = b;
    imageDifference_ = difference;
    itemA_->setImage(a);
    itemB_->setImage(b);
    itemDifference_->setImage(difference);
    imageSize_ = a.size();
    applyLayout();
}

void CompareCanvas::setSideNames(const QString& a, const QString& b) {
    nameA_ = a;
    nameB_ = b;
    updateBadge(badgeA_, 0);
    updateBadge(badgeB_, 1);
}

void CompareCanvas::setCandidates(const QStringList& ids, const QStringList& labels,
                                  const QString& selectedA, const QString& selectedB) {
    candidateIds_ = ids;
    candidateLabels_ = labels;
    selectedA_ = selectedA;
    selectedB_ = selectedB;
    updateBadge(badgeA_, 0);
    updateBadge(badgeB_, 1);
    placeBadges();
}

void CompareCanvas::setFlickerShowA(bool showA) {
    if (showA == flickerShowA_) {
        return;
    }
    flickerShowA_ = showA;
    if (mode_ == CompareMode::Flicker) {
        applyLayout();
    }
}

void CompareCanvas::setHeldSide(int side) {
    if (side == heldSide_) {
        return;
    }
    heldSide_ = side;
    applyLayout();
}

void CompareCanvas::setSplit(double fraction) {
    camera_.split = std::clamp(fraction, 0.0, 1.0);
    applyLayout();
}

void CompareCanvas::setOpacity(double fraction) {
    camera_.opacity = std::clamp(fraction, 0.0, 1.0);
    if (mode_ == CompareMode::Overlay) {
        applyLayout();
    }
}

void CompareCanvas::setFlickerRate(double hz) { camera_.flickerHz = std::clamp(hz, 1.0, 30.0); }

void CompareCanvas::setDiffGain(int gain) {
    camera_.diffGain = std::clamp(gain, kDiffGainMin, kDiffGainMax);
}

void CompareCanvas::applyLayout() {
    if (imageSize_.width() <= 0) {
        return;
    }
    const CompareLayout layout =
        buildLayout(mode_, imageSize_, camera_.clampedSplit(), camera_.clampedOpacity(),
                    flickerShowA_, heldSide_);

    const auto apply = [this](CompareImageItem* item, const ItemState& state) {
        if (!state.visible) {
            item->hide();
            return;
        }
        item->show();
        item->setPos(state.x, state.y);
        item->setOpacity(state.opacity);
        if (state.hasClip) {
            item->setClipRect(QRectF(state.clipX, state.clipY, state.clipW, state.clipH));
        } else {
            item->setClipRect(QRectF(QPointF(0, 0), imageSize_));
        }
    };
    apply(itemA_, layout.a);
    apply(itemB_, layout.b);
    apply(itemDifference_, layout.difference);

    if (layout.divider.visible) {
        divider_->setLine(QLineF(layout.divider.x0, layout.divider.y0, layout.divider.x1,
                                  layout.divider.y1));
        divider_->show();
    } else {
        divider_->hide();
    }
    badgeA_->setVisible(layout.a.visible);
    badgeB_->setVisible(layout.b.visible);
    scene()->setSceneRect(QRectF(QPointF(0, 0), layout.scene));
    placeBadges();
    if (!viewTouched_) {
        camera_.fit(layout.scene, viewport()->width(), viewport()->height());
    }
    applyView();
    placeBadges();
}

void CompareCanvas::updateBadge(QToolButton* badge, int side) {
    const QStringList& names = candidateLabels_;
    const QString& selected = side == 0 ? selectedA_ : selectedB_;
    const int index = candidateIds_.indexOf(selected);
    QString name = side == 0 ? nameA_ : nameB_;
    if (index >= 0 && index < names.size()) {
        name = names.at(index);
    }
    badge->setText(QStringLiteral("%1 · %2").arg(side == 0 ? QStringLiteral("A")
                                                            : QStringLiteral("B"), name));
    badge->setAccessibleName(QStringLiteral("Select %1").arg(name));
    badge->setToolTip(QStringLiteral("Select what side %1 shows").arg(side == 0 ? "A" : "B"));
    QMenu* menu = side == 0 ? menuA_ : menuB_;
    menu->clear();
    // One result on screen leaves no choice to offer, so the badge stays the
    // plain label it always was.
    if (candidateIds_.size() < 2) {
        menu->setEnabled(false);
        return;
    }
    menu->setEnabled(true);
    for (qsizetype i = 0; i < candidateIds_.size(); ++i) {
        QAction* action = menu->addAction(candidateLabels_.value(i));
        action->setData(candidateIds_.at(i));
        action->setCheckable(true);
        action->setChecked(candidateIds_.at(i) == selected);
    }
}

void CompareCanvas::placeBadges() {
    if (badgeA_ == nullptr || badgeB_ == nullptr || imageSize_.isEmpty()) {
        return;
    }
    badgeA_->adjustSize();
    badgeB_->adjustSize();
    const QPointF rightEdge = mode_ == CompareMode::SideBySide
                                  ? QPointF(imageSize_.width() * 2 + kSideBySideGap, 0)
                                  : QPointF(imageSize_.width(), 0);
    const QPoint left = mapFromScene(QPointF(0, 0)) + QPoint(kBadgeInset, kBadgeInset);
    const QPoint right =
        mapFromScene(rightEdge) + QPoint(-badgeB_->width() - kBadgeInset, kBadgeInset);
    badgeA_->move(left);
    badgeB_->move(right);
    badgeA_->raise();
    badgeB_->raise();
}

void CompareCanvas::clear(const QString& message) {
    itemA_->hide();
    itemB_->hide();
    itemDifference_->hide();
    divider_->hide();
    badgeA_->hide();
    badgeB_->hide();
    imageSize_ = QSize();
    message_->setText(message);
    message_->show();
    scene()->setSceneRect(message_->boundingRect());
    camera_.fit(scene()->sceneRect().size().toSize(), viewport()->width(), viewport()->height());
    applyView();
}

int CompareCanvas::imageWidth() const { return imageSize_.width(); }

void CompareCanvas::applyView() {
    setTransform(QTransform(camera_.clampedZoom(), 0.0, 0.0, camera_.clampedZoom(),
                            camera_.panX, camera_.panY));
    emit zoomChanged(static_cast<int>(camera_.clampedZoom() * 100.0));
}

void CompareCanvas::fitToContent() {
    viewTouched_ = false;
    camera_.fit(scene()->sceneRect().size().toSize(), viewport()->width(), viewport()->height());
    applyView();
    placeBadges();
}

QPointF CompareCanvas::scenePointAt(const QPoint& widgetPoint) const {
    return mapToScene(widgetPoint);
}

bool CompareCanvas::pixelAt(const QPoint& widgetPoint, int& x, int& y, uchar& b, uchar& g,
                            uchar& r) const {
    const QPointF sp = scenePointAt(widgetPoint);
    const CompareImageItem* item = itemA_->isVisible() ? itemA_ : (itemB_->isVisible() ? itemB_ : nullptr);
    if (item == nullptr || imageA_.isNull()) {
        return false;
    }
    const QImage& img = imageA_;
    x = static_cast<int>(sp.x());
    y = static_cast<int>(sp.y());
    if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) {
        return false;
    }
    const QRgb px = img.pixel(x, y);
    r = static_cast<uchar>(qRed(px));
    g = static_cast<uchar>(qGreen(px));
    b = static_cast<uchar>(qBlue(px));
    return true;
}

void CompareCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier)) {
        panning_ = true;
        lastPanPoint_ = event->position().toPoint();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton &&
        (mode_ == CompareMode::SplitH || mode_ == CompareMode::SplitV)) {
        dragging_ = true;
    }
    QGraphicsView::mousePressEvent(event);
}

void CompareCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (panning_) {
        const QPoint point = event->position().toPoint();
        const QPoint delta = point - lastPanPoint_;
        lastPanPoint_ = point;
        camera_.panX += delta.x();
        camera_.panY += delta.y();
        viewTouched_ = true;
        applyView();
        placeBadges();
        event->accept();
    } else if (dragging_ && !imageSize_.isEmpty()) {
        const QPointF sp = mapToScene(event->position().toPoint());
        if (mode_ == CompareMode::SplitV && imageSize_.width() > 0) {
            camera_.split = std::clamp(sp.x() / imageSize_.width(), 0.0, 1.0);
        } else if (mode_ == CompareMode::SplitH && imageSize_.height() > 0) {
            camera_.split = std::clamp(sp.y() / imageSize_.height(), 0.0, 1.0);
        }
        applyLayout();
        emit splitDragged(camera_.clampedSplit());
    }
    int x = 0;
    int y = 0;
    uchar b = 0;
    uchar g = 0;
    uchar r = 0;
    if (pixelAt(event->position().toPoint(), x, y, b, g, r)) {
        emit hoverPixel(x, y, true);
    } else {
        emit hoverPixel(0, 0, false);
    }
    QGraphicsView::mouseMoveEvent(event);
}

void CompareCanvas::mouseReleaseEvent(QMouseEvent* event) {
    dragging_ = false;
    if (panning_ && event->button() == Qt::LeftButton) {
        panning_ = false;
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void CompareCanvas::wheelEvent(QWheelEvent* event) {
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    const QPointF anchor = mapToScene(event->position().toPoint());
    camera_.zoomBySteps(steps, anchor.x(), anchor.y(), viewport()->width(), viewport()->height());
    viewTouched_ = true;
    applyView();
    placeBadges();
    event->accept();
}

void CompareCanvas::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    if (!viewTouched_) {
        camera_.fit(scene()->sceneRect().size().toSize(), viewport()->width(), viewport()->height());
    }
    applyView();
    placeBadges();
}

void CompareCanvas::scrollContentsBy(int dx, int dy) {
    // The base moves every child of the viewport by the scroll delta, the badges
    // included; this is the one point where that happens for a fit and a pan
    // alike, so it is the one place to undo it.
    QGraphicsView::scrollContentsBy(dx, dy);
    placeBadges();
}

}  // namespace mplayer::shell
