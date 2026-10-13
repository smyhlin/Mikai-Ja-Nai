#include "CompareCanvas.hpp"

#include "ShellTheme.hpp"

#include <QAction>
#include <QFontMetrics>
#include <QGraphicsItem>
#include <QGraphicsLineItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>

namespace mplayer::shell {

namespace {

QString selectorRowStyle() {
    const QColor bar(dock().red(), dock().green(), dock().blue());
    return QStringLiteral(
               "QWidget#compareSelectorRow { background-color: %1; border: none;"
               " border-top: 1px solid %2; }"
               "QToolButton { background: transparent; color: %3; border: none;"
               " border-radius: 0px; padding: 0px %4px; padding-right: %5px;"
               " text-align: left; }"
               "QToolButton:hover { background-color: %6; }"
               "QToolButton[prominent=\"false\"] { color: %7; }"
               "QToolButton::menu-indicator { subcontrol-position: right center;"
               " width: %8px; }")
        .arg(bar.name(), w15().name(QColor::HexArgb), w95().name(),
             QString::number(kCompareSelectorInset),
             QString::number(kCompareSelectorInset + kCompareSelectorArrow),
             white(26).name(QColor::HexArgb), w55().name(),
             QString::number(kCompareSelectorArrow));
}

void fitSelectorLabel(QToolButton* button, const QString& full) {
    const int pad = kCompareSelectorInset * 2 + kCompareSelectorArrow;
    const int width = std::max(0, button->width() - pad);
    button->setText(QFontMetrics(button->font()).elidedText(full, Qt::ElideRight, width));
}

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
    void setSmoothScaling(bool smooth) { smooth_ = smooth; }
    QRectF boundingRect() const override { return clip_; }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        if (image_.isNull() || clip_.isEmpty()) return;
        painter->setRenderHint(QPainter::SmoothPixmapTransform, smooth_);
        painter->drawImage(clip_, image_, clip_);
    }

private:
    QImage image_;
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
    setBackgroundBrush(bg0());
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true);

    itemA_ = new CompareImageItem;
    itemB_ = new CompareImageItem;
    itemDifference_ = new CompareImageItem;
    scene()->addItem(itemA_);
    scene()->addItem(itemB_);
    scene()->addItem(itemDifference_);
    divider_ = scene()->addLine(QLineF(), QPen(QColor(255, 255, 255), 2));
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
    badgeB_ = new QToolButton(selectorRow_);
    menuA_ = new QMenu(badgeA_);
    menuB_ = new QMenu(badgeB_);
    for (QToolButton* badge : {badgeA_, badgeB_}) {
        badge->setCursor(Qt::PointingHandCursor);
        badge->setFont(uiFont(9));
        badge->setToolButtonStyle(Qt::ToolButtonTextOnly);
        badge->setPopupMode(QToolButton::InstantPopup);
        badge->setMinimumWidth(0);
        badge->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        badge->installEventFilter(this);
        badge->setMenu(badge == badgeA_ ? menuA_ : menuB_);
        badge->hide();
        chips->addWidget(badge, 1);
        connect(badge->menu(), &QMenu::triggered, this, [this, badge](QAction* action) {
            emit candidateChosen(badge == badgeA_ ? 0 : 1, action->data().toString());
        });
    }
    status_ = new QLabel(viewport());
    status_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    status_->setAlignment(Qt::AlignCenter);
    status_->setWordWrap(true);
    status_->setFont(uiFont(11));
    status_->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(w90().name()));
    status_->hide();
    setChoices({}, QString(), QString());
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

void CompareCanvas::setFlickerRate(double hz) { camera_.flickerHz = std::clamp(hz, 1.0, 30.0); }

void CompareCanvas::setDiffGain(int gain) {
    camera_.diffGain = std::clamp(gain, kDiffGainMin, kDiffGainMax);
}

void CompareCanvas::applyLayout() {
    if (imageSize_.width() <= 0) {
        return;
    }
    if (status_ != nullptr) {
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
    const QString& selected = side == 0 ? selectedA_ : selectedB_;
    QString name = side == 0 ? nameA_ : nameB_;
    for (const ChoiceEntry& entry : choices_) {
        if (entry.id == selected) {
            name = entry.label;
            break;
        }
    }
    const QString sideName = side == 0 ? QStringLiteral("A") : QStringLiteral("B");
    QString& label = side == 0 ? labelA_ : labelB_;
    label = tr("%1 · %2").arg(sideName, name);
    badge->setText(label);
    badge->setAccessibleName(tr("Select %1").arg(name));
    badge->setToolTip(tr("Select what side %1 shows").arg(sideName));
    QMenu* menu = side == 0 ? menuA_ : menuB_;
    QVector<QAction*>& leaves = side == 0 ? leavesA_ : leavesB_;
    // One result on screen leaves no choice to offer, so the badge stays the
    // plain label it always was.
    if (choices_.size() < 2) {
        clearChoiceMenu(menu);
        builtChoices_.clear();
        leaves.clear();
        menu->setEnabled(false);
        return;
    }
    menu->setEnabled(true);
    if (choices_ != builtChoices_ || leaves.size() != choices_.size()) {
        fillChoiceMenu(menu, choices_, selected, &leaves);
        builtChoices_ = choices_;
    } else {
        markChoice(leaves, choices_, selected);
    }
}

QRect CompareCanvas::imageBarRect() const {
    if (imageSize_.isEmpty() || viewport() == nullptr) {
        return {};
    }
    const QRect visible = mapFromScene(QRectF(QPointF(0, 0), imageSize_)).boundingRect().intersected(
        viewport()->rect());
    if (visible.width() < 1) {
        return {};
    }
    return QRect(viewport()->pos().x() + visible.left(),
                 std::max(0, height() - kCompareSelectorBarHeight), visible.width(),
                 kCompareSelectorBarHeight);
}

void CompareCanvas::showStatus(const QString& text) {
    if (status_ == nullptr || viewport() == nullptr) {
        return;
    }
    status_->setText(text);
    status_->setGeometry(viewport()->rect().adjusted(24, 24, -24, -24));
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
    return distance <= kCompareDividerHit;
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
    if (imageSize_.isEmpty()) {
        badgeA_->hide();
        badgeB_->hide();
        return;
    }
    const bool flicker = mode_ == CompareMode::Flicker && heldSide_ < 0;
    placeBadge(badgeA_, labelA_, !flicker || flickerShowA_);
    placeBadge(badgeB_, labelB_, !flicker || !flickerShowA_);
    int leftWidth = std::max(1, selectorRow_->width() / 2);
    int rightWidth = std::max(1, selectorRow_->width() - leftWidth);
    if (heldSide_ < 0 && mode_ == CompareMode::SplitV && viewport() != nullptr) {
        const int cut = static_cast<int>(std::lround(imageSize_.width() * camera_.clampedSplit()));
        const int cutX = viewport()->pos().x() +
                         static_cast<int>(std::lround(mapFromScene(QPointF(cut, 0)).x()));
        const int spanLeft = selectorRow_->x();
        const int clamped = std::clamp(cutX, spanLeft, spanLeft + selectorRow_->width());
        leftWidth = std::max(1, clamped - spanLeft);
        rightWidth = std::max(1, selectorRow_->width() - leftWidth);
    }
    if (auto* row = qobject_cast<QHBoxLayout*>(selectorRow_->layout())) {
        row->setStretch(0, leftWidth);
        row->setStretch(1, rightWidth);
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
}

int CompareCanvas::imageWidth() const { return imageSize_.width(); }

void CompareCanvas::applyView() {
    setTransform(QTransform(camera_.clampedZoom(), 0.0, 0.0, camera_.clampedZoom(),
                            camera_.panX, camera_.panY));
    placeBadges();
    emit zoomChanged(static_cast<int>(std::lround(camera_.relativeZoom() * 100.0)));
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
    // The readout must sample what is actually shown: Difference hides both
    // items (nothing to read there), Flicker alternates, and the split modes
    // show each half. Choosing by visibility instead of always using A is what
    // keeps the hover colour honest in every layout.
    const QImage* img = nullptr;
    if (!imageA_.isNull() && itemA_ != nullptr && itemA_->isVisible()) {
        img = &imageA_;
    } else if (!imageB_.isNull() && itemB_ != nullptr && itemB_->isVisible()) {
        img = &imageB_;
    }
    if (img == nullptr) {
        return false;
    }
    // A fractional scene coordinate must not truncate toward zero: -0.4 would
    // read pixel 0 as valid. Flooring keeps a point left of the image out.
    x = static_cast<int>(std::floor(sp.x()));
    y = static_cast<int>(std::floor(sp.y()));
    if (x < 0 || y < 0 || x >= img->width() || y >= img->height()) {
        return false;
    }
    const QRgb px = img->pixel(x, y);
    if (argb != nullptr) *argb = px;
    r = static_cast<uchar>(qRed(px));
    g = static_cast<uchar>(qGreen(px));
    b = static_cast<uchar>(qBlue(px));
    return true;
}

void CompareCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier)) {
        panning_ = true;
        lastPanPoint_ = event->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && nearDivider(event->position().toPoint())) {
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
    const QPoint at = event->position().toPoint();
    if (!panning_) {
        if (nearDivider(at) || dragging_) {
            setCursor(mode_ == CompareMode::SplitV ? Qt::SplitHCursor : Qt::SplitVCursor);
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
    if (panning_ && event->button() == Qt::LeftButton) {
        panning_ = false;
        unsetCursor();
        event->accept();
        return;
    }
    if (wasDragging) {
        unsetCursor();
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
    return QGraphicsView::eventFilter(watched, event);
}

}  // namespace mplayer::shell
