#include "CompareCanvas.hpp"

#include "ShellTheme.hpp"

#include <QAction>
#include <QEvent>
#include <QFontMetrics>
#include <QGraphicsItem>
#include <QGraphicsLineItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace mplayer::shell {

namespace {

QString selectorRowStyle() {
    return QStringLiteral(
        "QWidget#compareSelectorRow {"
        "  background-color: #16181d;"
        "  border: none;"
        "  border-top: 1px solid #282c34;"
        "}"
        "QToolButton {"
        "  background: transparent;"
        "  color: #e8eaed;"
        "  border: none;"
        "  border-radius: 0px;"
        "  padding: 0px %1px;"
        "  font-weight: 600;"
        "  text-align: left;"
        "}"
        "QToolButton#compareBadgeA {"
        "  border-left: 3px solid #3b82f6;"
        "  border-right: 1px solid #282c34;"
        "}"
        "QToolButton#compareBadgeB {"
        "  border-left: 3px solid #f59e0b;"
        "}"
        "QToolButton:hover {"
        "  background-color: #22262e;"
        "  color: #ffffff;"
        "}"
        "QToolButton:pressed {"
        "  background-color: #2a2f3a;"
        "}"
        "QToolButton[prominent=\"false\"] {"
        "  color: #7d8594;"
        "  background-color: rgba(0, 0, 0, 0.22);"
        "}"
        "QToolButton::menu-indicator {"
        "  image: none;"
        "  width: 0px;"
        "}")
        .arg(QString::number(kCompareSelectorInset));
}

void fitSelectorLabel(QToolButton* button, const QString& full) {
    if (button == nullptr) {
        return;
    }
    const QString suffix = QStringLiteral("  ▾");
    const QFontMetrics fm(button->font());
    const int pad = kCompareSelectorInset * 2 + fm.horizontalAdvance(suffix) + 8;
    const int avail = button->width() - pad;
    if (avail <= 32) {
        button->setText(full + suffix);
        return;
    }
    button->setText(fm.elidedText(full, Qt::ElideRight, avail) + suffix);
}

}  // namespace

/// A picture that can be clipped. Split is drawn by clipping each side of the
/// *same* frozen frame, so the divide costs no recomposite and no re-render.
class CompareImageItem final : public QGraphicsItem {
public:
    CompareImageItem() { setAcceptedMouseButtons(Qt::NoButton); }

    void setImage(const QImage& image, const QSize& targetSize = QSize()) {
        prepareGeometryChange();
        image_ = image;
        targetSize_ = targetSize.isEmpty() ? image.size() : targetSize;
        clip_ = QRectF(QPointF(0, 0), targetSize_);
        update();
    }
    void setClipRect(const QRectF& clip) {
        prepareGeometryChange();
        clip_ = clip;
        update();
    }
    void setSmoothScaling(bool smooth) { smooth_ = smooth; }
    QRectF boundingRect() const override { return clip_; }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        if (image_.isNull() || clip_.isEmpty()) return;
        painter->setRenderHint(QPainter::SmoothPixmapTransform, smooth_);
        if (targetSize_.isEmpty() || targetSize_ == image_.size()) {
            painter->drawImage(clip_, image_, clip_);
        } else {
            const double sx = static_cast<double>(image_.width()) / std::max(1, targetSize_.width());
            const double sy = static_cast<double>(image_.height()) / std::max(1, targetSize_.height());
            const QRectF src(clip_.x() * sx, clip_.y() * sy, clip_.width() * sx, clip_.height() * sy);
            painter->drawImage(clip_, image_, src);
        }
    }

private:
    QImage image_;
    QSize targetSize_;
    QRectF clip_;
    bool smooth_ = true;
};

CompareCanvas::CompareCanvas(QWidget* parent) : QGraphicsView(parent) {
    setScene(new QGraphicsScene(this));
    setRenderHint(QPainter::Antialiasing, true);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setDragMode(QGraphicsView::NoDrag);
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setResizeAnchor(QGraphicsView::NoAnchor);
    setFrameShape(QFrame::NoFrame);
    setBackgroundBrush(QColor(17, 19, 23));
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true);
    if (viewport() != nullptr) {
        viewport()->setMouseTracking(true);
    }

    itemA_ = new CompareImageItem;
    itemB_ = new CompareImageItem;
    itemDifference_ = new CompareImageItem;
    scene()->addItem(itemA_);
    scene()->addItem(itemB_);
    scene()->addItem(itemDifference_);
    divider_ = scene()->addLine(QLineF(), QPen(QColor(255, 255, 255, 230), 1.5));
    message_ = scene()->addSimpleText(QString());
    message_->hide();
    itemDifference_->setZValue(2);
    divider_->setZValue(10);

    selectorRow_ = new QWidget(this);
    selectorRow_->setObjectName(QStringLiteral("compareSelectorRow"));
    selectorRow_->setAttribute(Qt::WA_StyledBackground, true);
    selectorRow_->setStyleSheet(selectorRowStyle());
    selectorRow_->hide();
    selectorRow_->installEventFilter(this);
    auto* chips = new QHBoxLayout(selectorRow_);
    chips->setContentsMargins(0, 0, 0, 0);
    chips->setSpacing(0);
    badgeA_ = new QToolButton(selectorRow_);
    badgeA_->setObjectName(QStringLiteral("compareBadgeA"));
    badgeB_ = new QToolButton(selectorRow_);
    badgeB_->setObjectName(QStringLiteral("compareBadgeB"));
    menuA_ = new QMenu(badgeA_);
    menuB_ = new QMenu(badgeB_);
    for (QToolButton* badge : {badgeA_, badgeB_}) {
        const int side = badge == badgeA_ ? 0 : 1;
        QMenu* menu = side == 0 ? menuA_ : menuB_;
        badge->setCursor(Qt::PointingHandCursor);
        badge->setFont(uiFont(10));
        badge->setToolButtonStyle(Qt::ToolButtonTextOnly);
        badge->setPopupMode(QToolButton::InstantPopup);
        badge->setMinimumWidth(0);
        badge->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        badge->installEventFilter(this);
        badge->setMenu(menu);
        badge->hide();
        chips->addWidget(badge, 1);
        connect(menu, &QMenu::triggered, this, [this, badge, side](QAction* action) {
            if (action == nullptr ||
                (action->property("_mplayer_forwarded_leaf").toBool() &&
                 !action->property("_mplayer_leaf_emitting").toBool())) {
                return;
            }
            const QString id = action->data().toString();
            if (id.isEmpty()) {
                return;
            }
            if (side == 0) {
                selectedA_ = id;
            } else {
                selectedB_ = id;
            }
            updateBadge(badge, side);
            placeBadges();
            emit candidateChosen(side, id);
        });
    }
    status_ = new QLabel(viewport());
    status_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    status_->setAlignment(Qt::AlignCenter);
    status_->setWordWrap(true);
    status_->setFont(uiFont(11));
    status_->setStyleSheet(
        QStringLiteral("color: #e8eaed; background: rgba(22, 24, 29, 0.88);"
                       " border: 1px solid #2e333d;"
                       " border-radius: 6px; padding: 14px 22px;"));
    status_->hide();
    setChoices({}, QString(), QString());
}

void CompareCanvas::setMode(CompareMode mode) {
    if (mode == mode_) {
        return;
    }
    mode_ = mode;
    applyLayout();
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

void CompareCanvas::setSideImages(const QImage& a, const QImage& b, const QImage& difference) {
    imageA_ = a;
    imageB_ = b;
    imageDifference_ = difference;

    QSize effectiveSize;
    if (!a.isNull()) {
        effectiveSize = a.size();
    }
    if (!b.isNull()) {
        effectiveSize = effectiveSize.expandedTo(b.size());
    }
    if (!difference.isNull()) {
        effectiveSize = effectiveSize.expandedTo(difference.size());
    }
    if (!effectiveSize.isEmpty()) {
        imageSize_ = effectiveSize;
    }

    itemA_->setImage(a, imageSize_);
    itemB_->setImage(b, imageSize_);
    itemDifference_->setImage(difference, imageSize_);
    applyLayout();
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

void CompareCanvas::setSideNames(const QString& a, const QString& b) {
    nameA_ = a;
    nameB_ = b;
    updateBadge(badgeA_, 0);
    updateBadge(badgeB_, 1);
    placeBadges();
}

void CompareCanvas::setChoices(const QVector<ChoiceEntry>& choices, const QString& selectedA,
                               const QString& selectedB) {
    choices_ = choices;
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
        if (viewport() != nullptr) {
            viewport()->update();
        }
    }
}

void CompareCanvas::setHeldSide(int side) {
    if (side == heldSide_) {
        return;
    }
    heldSide_ = side;
    applyLayout();
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

void CompareCanvas::setSplit(double fraction) {
    camera_.split = std::clamp(fraction, 0.0, 1.0);
    applyLayout();
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

void CompareCanvas::setFlickerRate(double hz) { camera_.flickerHz = std::clamp(hz, 1.0, 30.0); }

void CompareCanvas::setDiffGain(int gain) {
    camera_.diffGain = std::clamp(gain, kDiffGainMin, kDiffGainMax);
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

void CompareCanvas::applyLayout() {
    if (imageSize_.width() <= 0 || imageSize_.height() <= 0) {
        placeBadges();
        return;
    }
    if (status_ != nullptr && (!imageA_.isNull() || !imageB_.isNull() || !imageDifference_.isNull())) {
        status_->hide();
    }
    const CompareLayout layout =
        buildLayout(mode_, imageSize_, camera_.clampedSplit(), flickerShowA_, heldSide_);

    const auto apply = [this](CompareImageItem* item, const ItemState& state) {
        if (!state.visible) {
            item->hide();
            return;
        }
        item->show();
        item->setPos(state.x, state.y);
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
    scene()->setSceneRect(QRectF(QPointF(0, 0), layout.scene));
    placeBadges();
    if (!viewTouched_) {
        camera_.fit(layout.scene, viewport()->width(), viewport()->height());
    }
    applyView();
    placeBadges();
}

void CompareCanvas::updateBadge(QToolButton* badge, int side) {
    if (badge == nullptr) {
        return;
    }
    const QString& selected = side == 0 ? selectedA_ : selectedB_;
    QString name = side == 0 ? nameA_ : nameB_;
    for (const ChoiceEntry& entry : choices_) {
        if (entry.id == selected) {
            name = entry.label;
            break;
        }
    }
    if (name.isEmpty()) {
        name = selected;
    }
    const QString sideName = side == 0 ? QStringLiteral("A") : QStringLiteral("B");
    QString& label = side == 0 ? labelA_ : labelB_;
    label = tr("%1 · %2").arg(sideName, name);
    badge->setText(label);
    badge->setAccessibleName(tr("Select %1").arg(name));
    badge->setToolTip(tr("Select what side %1 shows (%2)").arg(sideName, name));
    QMenu* menu = side == 0 ? menuA_ : menuB_;
    QVector<ChoiceEntry>& builtChoices = side == 0 ? builtChoicesA_ : builtChoicesB_;
    QVector<QAction*>& leaves = side == 0 ? leavesA_ : leavesB_;
    if (choices_.size() < 2) {
        clearChoiceMenu(menu);
        builtChoices.clear();
        leaves.clear();
        menu->setEnabled(false);
        return;
    }
    menu->setEnabled(true);
    if (choices_ != builtChoices || leaves.size() != choices_.size()) {
        fillChoiceMenu(menu, choices_, selected, &leaves);
        builtChoices = choices_;
    } else {
        markChoice(leaves, choices_, selected);
    }
}

QRect CompareCanvas::imageBarRect() const {
    if (imageSize_.isEmpty() || viewport() == nullptr) {
        return {};
    }
    const int barY = std::max(0, height() - kCompareSelectorBarHeight);
    const int vpLeft = viewport()->pos().x();
    const int vpWidth = std::max(1, viewport()->width());
    return QRect(vpLeft, barY, vpWidth, kCompareSelectorBarHeight);
}

void CompareCanvas::showStatus(const QString& text) {
    if (status_ == nullptr || viewport() == nullptr) {
        return;
    }
    status_->setText(text);
    const QRect vp = viewport()->rect();
    const int boxW = std::min(std::max(280, vp.width() - 64), 520);
    const int boxH = std::min(std::max(64, status_->sizeHint().height() + 24), std::max(64, vp.height() - 48));
    status_->setGeometry((vp.width() - boxW) / 2, (vp.height() - boxH) / 2, boxW, boxH);
    status_->setVisible(!text.isEmpty());
    status_->raise();
}

bool CompareCanvas::nearDivider(const QPoint& viewportPos) const {
    if (imageSize_.isEmpty() || heldSide_ >= 0 ||
        (mode_ != CompareMode::SplitH && mode_ != CompareMode::SplitV)) {
        return false;
    }
    const CompareLayout layout =
        buildLayout(mode_, imageSize_, camera_.clampedSplit(), flickerShowA_, heldSide_);
    if (!layout.divider.visible) {
        return false;
    }
    const QPointF a = mapFromScene(QPointF(layout.divider.x0, layout.divider.y0));
    const QPointF b = mapFromScene(QPointF(layout.divider.x1, layout.divider.y1));
    const double dx = b.x() - a.x();
    const double dy = b.y() - a.y();
    const double lengthSquared = dx * dx + dy * dy;
    double distance = 0.0;
    if (lengthSquared < 1.0) {
        distance = std::hypot(viewportPos.x() - a.x(), viewportPos.y() - a.y());
    } else {
        const double t = std::clamp(
            ((viewportPos.x() - a.x()) * dx + (viewportPos.y() - a.y()) * dy) / lengthSquared, 0.0,
            1.0);
        const double px = a.x() + t * dx;
        const double py = a.y() + t * dy;
        distance = std::hypot(viewportPos.x() - px, viewportPos.y() - py);
    }
    return distance <= std::max(kCompareDividerHit, 10);
}

void CompareCanvas::layoutSelectorRow() {
    const bool show = !imageSize_.isEmpty();
    const int bar = show ? kCompareSelectorBarHeight : 0;
    if (viewportMargins().bottom() != bar) {
        setViewportMargins(0, 0, 0, bar);
    }
    if (selectorRow_ == nullptr) {
        return;
    }
    if (!show) {
        selectorRow_->hide();
        return;
    }
    const QRect span = imageBarRect();
    if (span.isEmpty()) {
        selectorRow_->hide();
        return;
    }
    selectorRow_->setGeometry(span);
    selectorRow_->show();
    selectorRow_->raise();
}

void CompareCanvas::placeBadge(QToolButton* badge, const QString& label, bool prominent) {
    if (badge == nullptr || selectorRow_ == nullptr) {
        return;
    }
    Q_UNUSED(label);
    badge->setMaximumWidth(QWIDGETSIZE_MAX);
    const QString prominence = prominent ? QStringLiteral("true") : QStringLiteral("false");
    if (badge->property("prominent").toString() != prominence) {
        badge->setProperty("prominent", prominence);
        badge->style()->unpolish(badge);
        badge->style()->polish(badge);
    }
    badge->show();
}

void CompareCanvas::placeBadges() {
    if (badgeA_ == nullptr || badgeB_ == nullptr) {
        return;
    }
    layoutSelectorRow();
    if (imageSize_.isEmpty() || selectorRow_->isHidden()) {
        badgeA_->hide();
        badgeB_->hide();
        return;
    }
    const bool flicker = mode_ == CompareMode::Flicker && heldSide_ < 0;
    placeBadge(badgeA_, labelA_, !flicker || flickerShowA_);
    placeBadge(badgeB_, labelB_, !flicker || !flickerShowA_);
    if (auto* row = qobject_cast<QHBoxLayout*>(selectorRow_->layout())) {
        row->setStretch(0, 1);
        row->setStretch(1, 1);
        row->activate();
    }
    fitSelectorLabel(badgeA_, labelA_);
    fitSelectorLabel(badgeB_, labelB_);
}

void CompareCanvas::clear(const QString& message) {
    itemA_->hide();
    itemB_->hide();
    itemDifference_->hide();
    divider_->hide();
    badgeA_->hide();
    badgeB_->hide();
    imageSize_ = QSize();
    layoutSelectorRow();
    message_->hide();
    showStatus(message);
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

int CompareCanvas::imageWidth() const { return imageSize_.width(); }

void CompareCanvas::applyView() {
    setTransform(QTransform(camera_.clampedZoom(), 0.0, 0.0, camera_.clampedZoom(),
                            camera_.panX, camera_.panY));
    placeBadges();
    emit zoomChanged(static_cast<int>(std::lround(camera_.relativeZoom() * 100.0)));
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

void CompareCanvas::setSmoothScaling(bool smooth) {
    setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    for (CompareImageItem* item : {itemA_, itemB_, itemDifference_}) {
        if (item != nullptr) item->setSmoothScaling(smooth);
    }
    if (viewport() != nullptr) {
        viewport()->update();
    }
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
                            uchar& r, QRgb* argb) const {
    const QPointF sp = scenePointAt(widgetPoint);
    if (imageSize_.isEmpty()) {
        return false;
    }
    x = static_cast<int>(std::floor(sp.x()));
    y = static_cast<int>(std::floor(sp.y()));
    if (x < 0 || y < 0 || x >= imageSize_.width() || y >= imageSize_.height()) {
        return false;
    }

    const QImage* img = nullptr;
    if (heldSide_ == 0 && !imageA_.isNull()) {
        img = &imageA_;
    } else if (heldSide_ == 1 && !imageB_.isNull()) {
        img = &imageB_;
    } else if (mode_ == CompareMode::Difference && !imageDifference_.isNull() &&
               itemDifference_ != nullptr && itemDifference_->isVisible()) {
        img = &imageDifference_;
    } else if (mode_ == CompareMode::SplitV) {
        const int cut = static_cast<int>(imageSize_.width() * camera_.clampedSplit());
        img = (x < cut) ? &imageA_ : &imageB_;
    } else if (mode_ == CompareMode::SplitH) {
        const int cut = static_cast<int>(imageSize_.height() * camera_.clampedSplit());
        img = (y < cut) ? &imageA_ : &imageB_;
    } else if (!imageA_.isNull() && itemA_ != nullptr && itemA_->isVisible()) {
        img = &imageA_;
    } else if (!imageB_.isNull() && itemB_ != nullptr && itemB_->isVisible()) {
        img = &imageB_;
    }
    if (img == nullptr || img->isNull()) {
        return false;
    }
    const int ix = std::clamp(x * img->width() / std::max(1, imageSize_.width()), 0, img->width() - 1);
    const int iy = std::clamp(y * img->height() / std::max(1, imageSize_.height()), 0, img->height() - 1);
    const QRgb px = img->pixel(ix, iy);
    if (argb != nullptr) *argb = px;
    r = static_cast<uchar>(qRed(px));
    g = static_cast<uchar>(qGreen(px));
    b = static_cast<uchar>(qBlue(px));
    return true;
}

void CompareCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && nearDivider(event->position().toPoint())) {
        dragging_ = true;
        setCursor(mode_ == CompareMode::SplitV ? Qt::SplitHCursor : Qt::SplitVCursor);
        event->accept();
        return;
    }
    if ((event->button() == Qt::LeftButton && !imageSize_.isEmpty()) ||
        event->button() == Qt::MiddleButton) {
        panning_ = true;
        lastPanPoint_ = event->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
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
        event->accept();
    }
    int x = 0;
    int y = 0;
    uchar b = 0;
    uchar g = 0;
    uchar r = 0;
    const QPoint at = event->position().toPoint();
    if (!panning_) {
        if (nearDivider(at) || dragging_) {
            setCursor(mode_ == CompareMode::SplitV ? Qt::SplitHCursor : Qt::SplitVCursor);
        } else if (!imageSize_.isEmpty()) {
            setCursor(Qt::OpenHandCursor);
        } else {
            unsetCursor();
        }
    }
    QRgb px = 0;
    if (pixelAt(at, x, y, b, g, r, &px)) {
        emit hoverPixel(x, y, true, px);
    } else {
        emit hoverPixel(0, 0, false, 0);
    }
    QGraphicsView::mouseMoveEvent(event);
}

void CompareCanvas::mouseReleaseEvent(QMouseEvent* event) {
    const bool wasDragging = dragging_;
    dragging_ = false;
    if (panning_ && (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton)) {
        panning_ = false;
        if (!imageSize_.isEmpty()) {
            setCursor(Qt::OpenHandCursor);
        } else {
            unsetCursor();
        }
        event->accept();
        return;
    }
    if (wasDragging) {
        if (!imageSize_.isEmpty()) {
            setCursor(Qt::OpenHandCursor);
        } else {
            unsetCursor();
        }
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void CompareCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && !imageSize_.isEmpty()) {
        if (nearDivider(event->position().toPoint())) {
            setSplit(0.5);
            emit splitDragged(0.5);
        } else {
            fitToContent();
        }
        event->accept();
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void CompareCanvas::leaveEvent(QEvent* event) {
    emit hoverPixel(0, 0, false, 0);
    QGraphicsView::leaveEvent(event);
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
    event->accept();
}

void CompareCanvas::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    if (!viewTouched_) {
        camera_.fit(scene()->sceneRect().size().toSize(), viewport()->width(), viewport()->height());
    }
    applyView();
    placeBadges();
    if (status_ != nullptr && status_->isVisible()) {
        showStatus(status_->text());
    }
}

void CompareCanvas::drawForeground(QPainter* painter, const QRectF&) {
    if (painter == nullptr || viewport() == nullptr || imageSize_.isEmpty()) {
        return;
    }
    painter->save();
    painter->resetTransform();
    painter->setRenderHint(QPainter::Antialiasing, true);

    // Draw interactive split grab handle in SplitH / SplitV modes.
    if (heldSide_ < 0 && (mode_ == CompareMode::SplitV || mode_ == CompareMode::SplitH)) {
        const CompareLayout layout =
            buildLayout(mode_, imageSize_, camera_.clampedSplit(), flickerShowA_, heldSide_);
        if (layout.divider.visible) {
            const QPointF p0 = mapFromScene(QPointF(layout.divider.x0, layout.divider.y0));
            const QPointF p1 = mapFromScene(QPointF(layout.divider.x1, layout.divider.y1));

            painter->setPen(QPen(QColor(0, 0, 0, 155), 3.5));
            painter->drawLine(p0, p1);
            painter->setPen(QPen(QColor(235, 238, 244, 230), 1.5));
            painter->drawLine(p0, p1);

            const QPointF mid((p0.x() + p1.x()) * 0.5, (p0.y() + p1.y()) * 0.5);
            const QRectF knobRect(mid.x() - 13.0, mid.y() - 13.0, 26.0, 26.0);
            painter->setPen(QPen(QColor(220, 224, 232, 210), 1.2));
            painter->setBrush(QColor(20, 22, 27, 235));
            painter->drawEllipse(knobRect);

            QFont arrowFont = uiFont(8);
            arrowFont.setWeight(QFont::Bold);
            painter->setFont(arrowFont);
            painter->setPen(QColor(240, 242, 246));
            const QString glyph = mode_ == CompareMode::SplitV ? QStringLiteral("‹ ›")
                                                               : QStringLiteral("▲▼");
            painter->drawText(knobRect, Qt::AlignCenter, glyph);
        }
    }

    // Status message if one or both sides are currently rendering/unavailable.
    if (imageA_.isNull() || imageB_.isNull()) {
        const QString missingLabel =
            (imageA_.isNull() && imageB_.isNull())
                ? tr("Rendering comparison…")
                : (imageA_.isNull() ? tr("Side A (%1)").arg(nameA_)
                                    : tr("Side B (%1)").arg(nameB_));
        QFont f = uiFont(9);
        f.setWeight(QFont::DemiBold);
        painter->setFont(f);
        const QFontMetrics fm(f);
        const int tw = fm.horizontalAdvance(missingLabel) + 24;
        const QRect vp = viewport()->rect();
        const QRectF box((vp.width() - tw) * 0.5, vp.height() - 40.0, tw, 26.0);
        painter->setPen(QPen(QColor(46, 51, 61, 220), 1.0));
        painter->setBrush(QColor(18, 20, 25, 220));
        painter->drawRoundedRect(box, 4.0, 4.0);
        painter->setPen(QColor(210, 214, 222));
        painter->drawText(box, Qt::AlignCenter, missingLabel);
    }

    // Top-left mode/side status pill.
    QString badgeText;
    QColor accentColor(59, 130, 246);
    if (heldSide_ == 0 || heldSide_ == 1) {
        badgeText = heldSide_ == 0 ? tr("PEEK · A (%1)").arg(nameA_)
                                   : tr("PEEK · B (%1)").arg(nameB_);
        accentColor = heldSide_ == 0 ? QColor(59, 130, 246) : QColor(245, 158, 11);
    } else if (mode_ == CompareMode::Flicker) {
        badgeText = flickerShowA_ ? tr("FLICKER · A (%1)").arg(nameA_)
                                  : tr("FLICKER · B (%1)").arg(nameB_);
        accentColor = flickerShowA_ ? QColor(59, 130, 246) : QColor(245, 158, 11);
    } else if (mode_ == CompareMode::Difference) {
        badgeText = camera_.diffGain > 1
                        ? tr("DIFF · %1× Grayscale").arg(camera_.diffGain)
                        : tr("DIFF · 1× JET Heatmap");
        accentColor = QColor(16, 185, 129);
    }

    if (!badgeText.isEmpty()) {
        QFont pillFont = uiFont(8);
        pillFont.setWeight(QFont::Bold);
        painter->setFont(pillFont);
        const QFontMetrics fm(pillFont);
        const int textW = fm.horizontalAdvance(badgeText);
        const QRectF pillRect(10.0, 10.0, textW + 22.0, 20.0);

        painter->setPen(QPen(QColor(46, 51, 61, 220), 1.0));
        painter->setBrush(QColor(18, 20, 25, 215));
        painter->drawRoundedRect(pillRect, 4.0, 4.0);

        painter->setPen(Qt::NoPen);
        painter->setBrush(accentColor);
        painter->drawEllipse(QPointF(pillRect.left() + 9.0, pillRect.center().y()), 3.0, 3.0);

        painter->setPen(QColor(232, 234, 237));
        painter->drawText(QRectF(pillRect.left() + 16.0, pillRect.top(),
                                 pillRect.width() - 20.0, pillRect.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, badgeText);
    }

    painter->restore();
}

bool CompareCanvas::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Wheel &&
        (watched == selectorRow_ || watched == badgeA_ || watched == badgeB_)) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        const QPointF local(viewport()->rect().center());
        QWheelEvent mapped(local, wheel->globalPosition(), wheel->pixelDelta(), wheel->angleDelta(),
                           wheel->buttons(), wheel->modifiers(), wheel->phase(), wheel->inverted(),
                           wheel->source());
        wheelEvent(&mapped);
        return true;
    }
    if ((event->type() == QEvent::Resize || event->type() == QEvent::Show) &&
        (watched == selectorRow_ || watched == badgeA_ || watched == badgeB_)) {
        fitSelectorLabel(badgeA_, labelA_);
        fitSelectorLabel(badgeB_, labelB_);
    }
    return QGraphicsView::eventFilter(watched, event);
}

}  // namespace mplayer::shell
