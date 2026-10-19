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
               "QToolButton#compareBadgeA {"
               "  background: #16181d;"
               "  color: #e5e7eb;"
               "  border: none;"
               "  border-left: 3px solid #3b82f6;"
               "  border-right: 1px solid #282c34;"
               "  border-radius: 0px;"
               "  padding: 0px %1px;"
               "  font-weight: 600;"
               "  text-align: left;"
               "}"
               "QToolButton#compareBadgeB {"
               "  background: #16181d;"
               "  color: #e5e7eb;"
               "  border: none;"
               "  border-left: 3px solid #f59e0b;"
               "  border-radius: 0px;"
               "  padding: 0px %1px;"
               "  font-weight: 600;"
               "  text-align: left;"
               "}"
               "QToolButton:hover {"
               "  background-color: #1e2128;"
               "  color: #ffffff;"
               "}"
               "QToolButton[prominent=\"false\"] {"
               "  color: #9ca3af;"
               "}"
               "QToolButton::menu-indicator {"
               "  image: none;"
               "  width: 0px;"
               "}")
        .arg(QString::number(kCompareSelectorInset));
}

void fitSelectorLabel(QToolButton* button, const QString& full) {
    if (button == nullptr) return;
    const QString suffix = QStringLiteral("  ▾");
    const int pad = kCompareSelectorInset * 2 + 24;
    const int avail = button->width() - pad;
    if (avail <= 24) {
        button->setText(full + suffix);
        return;
    }
    const QFontMetrics fm(button->font());
    const QString elided = fm.elidedText(full, Qt::ElideRight, avail);
    button->setText((elided.isEmpty() ? full : elided) + suffix);
}

/// True when two choice lists describe the same menu tree: same entries in the
/// same order under the same paths, with the same availability. Only the words
/// (label, tooltip) differ, which is exactly what a retranslation changes.
bool sameChoiceShape(const QVector<ChoiceEntry>& a, const QVector<ChoiceEntry>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (a.at(i).id != b.at(i).id || a.at(i).path != b.at(i).path ||
            a.at(i).disabled != b.at(i).disabled) {
            return false;
        }
    }
    return true;
}

/// Rewrites the leaves of an existing fill in place. No action is created or
/// destroyed, so an open menu keeps its rows and a handler that triggered the
/// refresh keeps running on a live action.
void restateChoiceLeaves(const QVector<QAction*>& leaves, const QVector<ChoiceEntry>& entries) {
    if (leaves.size() != entries.size()) {
        return;
    }
    for (qsizetype i = 0; i < entries.size(); ++i) {
        QAction* action = leaves.at(i);
        if (action == nullptr) {
            continue;
        }
        if (action->text() != entries.at(i).label) {
            action->setText(entries.at(i).label);
        }
        action->setEnabled(!entries.at(i).disabled);
        if (action->toolTip() != entries.at(i).tooltip) {
            action->setToolTip(entries.at(i).tooltip);
        }
    }
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
    setBackgroundBrush(QColor(11, 12, 14));
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
    QPen dividerPen(QColor(255, 255, 255, 220), 2.0);
    dividerPen.setCosmetic(true);
    divider_ = scene()->addLine(QLineF(), dividerPen);
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
        badge->setCursor(Qt::PointingHandCursor);
        badge->setFont(uiFont(9, true));
        badge->setToolButtonStyle(Qt::ToolButtonTextOnly);
        badge->setPopupMode(QToolButton::InstantPopup);
        badge->setMinimumWidth(0);
        badge->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        badge->installEventFilter(this);
        badge->setMenu(badge == badgeA_ ? menuA_ : menuB_);
        badge->hide();
        chips->addWidget(badge, 1);
        connect(badge->menu(), &QMenu::triggered, this, [this, badge](QAction* action) {
            if (action == nullptr) {
                return;
            }
            const QString id = action->data().toString();
            if (id.isEmpty()) return;
            const int side = badge == badgeA_ ? 0 : 1;
            const QString current = side == 0 ? selectedA_ : selectedB_;
            if (id == current) return;
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
    status_->setStyleSheet(QStringLiteral("color: #e5e7eb; background: transparent;"));
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
    if (!a.isNull()) {
        imageSize_ = a.size();
    } else if (!b.isNull()) {
        imageSize_ = b.size();
    } else if (!difference.isNull()) {
        imageSize_ = difference.size();
    } else {
        imageSize_ = QSize();
    }
    applyLayout();
    if (viewport() != nullptr) viewport()->update();
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
        if (viewport() != nullptr) viewport()->update();
    }
}

void CompareCanvas::setHeldSide(int side) {
    if (side == heldSide_) {
        return;
    }
    heldSide_ = side;
    applyLayout();
    if (viewport() != nullptr) viewport()->update();
}

void CompareCanvas::setSplit(double fraction) {
    camera_.split = std::clamp(fraction, 0.0, 1.0);
    applyLayout();
    if (viewport() != nullptr) viewport()->update();
}

void CompareCanvas::setFlickerRate(double hz) { camera_.flickerHz = std::clamp(hz, 1.0, 30.0); }

void CompareCanvas::setDiffGain(int gain) {
    camera_.diffGain = std::clamp(gain, kDiffGainMin, kDiffGainMax);
}

void CompareCanvas::applyLayout() {
    if (imageSize_.width() <= 0) {
        placeBadges();
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
    fitSelectorLabel(badge, label);
    badge->setAccessibleName(tr("Select %1").arg(name));
    badge->setToolTip(tr("Select what side %1 shows").arg(sideName));
    QMenu* menu = side == 0 ? menuA_ : menuB_;
    QVector<QAction*>& leaves = side == 0 ? leavesA_ : leavesB_;
    QVector<ChoiceEntry>& builtChoices = side == 0 ? builtChoicesA_ : builtChoicesB_;
    // One result on screen leaves no choice to offer, so the badge stays the
    // plain label it always was.
    if (choices_.size() < 2) {
        clearChoiceMenu(menu);
        builtChoices.clear();
        leaves.clear();
        menu->setEnabled(false);
        return;
    }
    menu->setEnabled(true);
    if (choices_ != builtChoices || leaves.size() != choices_.size()) {
        // A menu tree that keeps its entries only has new words: restating the
        // leaves keeps every action (and an open menu) alive. Anything that
        // changes the tree is built fresh.
        if (leaves.size() == choices_.size() && sameChoiceShape(choices_, builtChoices)) {
            restateChoiceLeaves(leaves, choices_);
        } else {
            fillChoiceMenu(menu, choices_, selected, &leaves);
        }
        builtChoices = choices_;
    }
    markChoice(leaves, choices_, selected);
}

QRect CompareCanvas::imageBarRect() const {
    if (width() <= 0 || height() <= 0) {
        return {};
    }
    return QRect(0, std::max(0, height() - kCompareSelectorBarHeight), width(),
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
    return distance <= std::max(kCompareDividerHit, 12);
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
    // Lock Side A and Side B selectors to a stable 50/50 split across the bottom
    // bar so dragging the split line never squishes or shifts the buttons.
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
    if (viewport() != nullptr) viewport()->update();
}

int CompareCanvas::imageWidth() const { return imageSize_.width(); }

void CompareCanvas::applyView() {
    setTransform(QTransform(camera_.clampedZoom(), 0.0, 0.0, camera_.clampedZoom(),
                            camera_.panX, camera_.panY));
    placeBadges();
    emit zoomChanged(static_cast<int>(std::lround(camera_.relativeZoom() * 100.0)));
    if (viewport() != nullptr) viewport()->update();
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
    // The readout must sample what is actually shown: Difference shows the
    // difference map, Flicker alternates, and the split modes clip each half.
    const QImage* img = nullptr;
    if (itemDifference_ != nullptr && itemDifference_->isVisible() &&
        !imageDifference_.isNull() && itemDifference_->boundingRect().contains(sp)) {
        img = &imageDifference_;
    } else if (!imageA_.isNull() && itemA_ != nullptr && itemA_->isVisible() &&
               itemA_->boundingRect().contains(sp)) {
        img = &imageA_;
    } else if (!imageB_.isNull() && itemB_ != nullptr && itemB_->isVisible() &&
               itemB_->boundingRect().contains(sp)) {
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
    const QPoint pt = event->position().toPoint();
    if (event->button() == Qt::LeftButton && nearDivider(pt)) {
        dragging_ = true;
        setCursor(mode_ == CompareMode::SplitV ? Qt::SplitHCursor : Qt::SplitVCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && !imageSize_.isEmpty()) {
        panning_ = true;
        lastPanPoint_ = pt;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void CompareCanvas::mouseMoveEvent(QMouseEvent* event) {
    const QPoint at = event->position().toPoint();
    if (panning_) {
        const QPoint delta = at - lastPanPoint_;
        lastPanPoint_ = at;
        camera_.panX += delta.x();
        camera_.panY += delta.y();
        viewTouched_ = true;
        applyView();
        event->accept();
    } else if (dragging_ && !imageSize_.isEmpty()) {
        const QPointF sp = mapToScene(at);
        if (mode_ == CompareMode::SplitV && imageSize_.width() > 0) {
            camera_.split = std::clamp(sp.x() / imageSize_.width(), 0.0, 1.0);
        } else if (mode_ == CompareMode::SplitH && imageSize_.height() > 0) {
            camera_.split = std::clamp(sp.y() / imageSize_.height(), 0.0, 1.0);
        }
        applyLayout();
        emit splitDragged(camera_.clampedSplit());
    }
    if (!panning_) {
        if (nearDivider(at) || dragging_) {
            setCursor(mode_ == CompareMode::SplitV ? Qt::SplitHCursor : Qt::SplitVCursor);
        } else if (!imageSize_.isEmpty()) {
            setCursor(Qt::OpenHandCursor);
        } else {
            unsetCursor();
        }
    }
    int x = 0;
    int y = 0;
    uchar b = 0;
    uchar g = 0;
    uchar r = 0;
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
        if (!imageSize_.isEmpty()) {
            setCursor(Qt::OpenHandCursor);
        } else {
            unsetCursor();
        }
        event->accept();
        return;
    }
    if (wasDragging) {
        if (nearDivider(event->position().toPoint())) {
            setCursor(mode_ == CompareMode::SplitV ? Qt::SplitHCursor : Qt::SplitVCursor);
        } else if (!imageSize_.isEmpty()) {
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

void CompareCanvas::drawForeground(QPainter* painter, const QRectF&) {
    if (imageSize_.isEmpty() || viewport() == nullptr) {
        return;
    }
    painter->save();
    painter->resetTransform();
    painter->setRenderHint(QPainter::Antialiasing, true);

    // Draw interactive split handle at the midpoint of the visible divider line
    if (heldSide_ < 0 && (mode_ == CompareMode::SplitV || mode_ == CompareMode::SplitH) &&
        divider_ != nullptr && divider_->isVisible()) {
        const QLineF sceneLine = divider_->line();
        const QPointF p0 = mapFromScene(sceneLine.p1());
        const QPointF p1 = mapFromScene(sceneLine.p2());
        const QPointF mid((p0.x() + p1.x()) * 0.5,
                          std::clamp((p0.y() + p1.y()) * 0.5, 28.0,
                                     std::max(28.0, viewport()->height() - 28.0)));
        const QRectF handleRect(mid.x() - 15.0, mid.y() - 15.0, 30.0, 30.0);
        painter->setPen(QPen(QColor(255, 255, 255, 220), 1.5));
        painter->setBrush(QColor(22, 24, 29, 235));
        painter->drawEllipse(handleRect);
        painter->setFont(uiFont(8, true));
        painter->setPen(QColor(243, 244, 246));
        painter->drawText(handleRect, Qt::AlignCenter,
                          mode_ == CompareMode::SplitV ? QStringLiteral("‹ ›")
                                                       : QStringLiteral("▲▼"));
    }

    // Top-left status pill
    QString pillText;
    QColor pillAccent(59, 130, 246);
    if (heldSide_ == 0) {
        pillText = tr("PEEK · SIDE A (%1×%2)").arg(imageSize_.width()).arg(imageSize_.height());
        pillAccent = QColor(59, 130, 246);
    } else if (heldSide_ == 1) {
        pillText = tr("PEEK · SIDE B (%1×%2)").arg(imageSize_.width()).arg(imageSize_.height());
        pillAccent = QColor(245, 158, 11);
    } else if (mode_ == CompareMode::Flicker) {
        pillText = flickerShowA_
                       ? tr("FLICKER · SIDE A (%1×%2)").arg(imageSize_.width()).arg(imageSize_.height())
                       : tr("FLICKER · SIDE B (%1×%2)").arg(imageSize_.width()).arg(imageSize_.height());
        pillAccent = flickerShowA_ ? QColor(59, 130, 246) : QColor(245, 158, 11);
    } else if (mode_ == CompareMode::Difference) {
        pillText = tr("DIFFERENCE · %1×%2").arg(imageSize_.width()).arg(imageSize_.height());
        pillAccent = QColor(16, 185, 129);
    } else if (mode_ == CompareMode::SplitV || mode_ == CompareMode::SplitH) {
        pillText = tr("A / B · %1×%2").arg(imageSize_.width()).arg(imageSize_.height());
        pillAccent = QColor(59, 130, 246);
    }

    if (!pillText.isEmpty()) {
        const QFont font = uiFont(8, true);
        painter->setFont(font);
        const QFontMetrics fm(font);
        const int textW = fm.horizontalAdvance(pillText);
        const QRectF pillRect(10.0, 10.0, textW + 24.0, 22.0);
        painter->setPen(QPen(QColor(40, 44, 52, 220), 1.0));
        painter->setBrush(QColor(13, 14, 17, 210));
        painter->drawRoundedRect(pillRect, 4.0, 4.0);
        painter->setPen(Qt::NoPen);
        painter->setBrush(pillAccent);
        painter->drawEllipse(QPointF(pillRect.left() + 9.0, pillRect.center().y()), 3.5, 3.5);
        painter->setPen(QColor(229, 231, 235));
        painter->drawText(pillRect.adjusted(17.0, 0.0, -6.0, 0.0),
                          Qt::AlignVCenter | Qt::AlignLeft, pillText);
    }

    painter->restore();
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

void CompareCanvas::changeEvent(QEvent* event) {
    QGraphicsView::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void CompareCanvas::retranslateUi() {
    // The badge label, its tooltip and its spoken name are this view's words,
    // and the status pill is painted from tr() on the next repaint. The side
    // names and candidate labels are the caller's and arrive through
    // setSideNames/setChoices on the same event, so old text is never re-read.
    // updateBadge() restates the menu tree only when the caller's choices
    // changed, so an open menu keeps its actions and its handler.
    updateBadge(badgeA_, 0);
    updateBadge(badgeB_, 1);
    placeBadges();
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

}  // namespace mplayer::shell
