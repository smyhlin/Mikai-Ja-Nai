#include "ComparePane.hpp"

#include <QAction>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QToolButton>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "CompareLayout.hpp"

namespace mplayer::shell {

namespace {
constexpr int kBadgeX = 6;
constexpr int kBadgeY = 4;
}  // namespace

ComparePane::ComparePane(QWidget* parent) : QGraphicsView(parent) {
    scene_ = new QGraphicsScene(this);
    item_ = new QGraphicsPixmapItem();
    scene_->addItem(item_);
    setScene(scene_);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setFrameShape(QFrame::NoFrame);
    setBackgroundBrush(Qt::black);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true);

    badge_ = new QToolButton(viewport());
    badge_->setCursor(Qt::PointingHandCursor);
    badge_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    badge_->setStyleSheet(QStringLiteral(
        "QToolButton { background: rgba(16,16,16,190); color: #E8EAED;"
        " border: 1px solid rgba(255,255,255,60); border-radius: 4px;"
        " padding: 2px 8px; text-align: left; }"
        "QToolButton:hover { border-color: #7AB7FF; }"));

    menu_ = new QMenu(badge_);
    badge_->setMenu(menu_);
    connect(menu_, &QMenu::triggered, this, [this](QAction* action) {
        const QString id = action->data().toString();
        if (id.isEmpty()) {
            return;
        }
        current_ = id;
        refreshBadge();
        emit choiceChosen(id);
    });

    // A drag pans the pane through its scrollbars; that is a user gesture, so
    // the shared view has to follow. A view this pane is applying itself must
    // not report back, or the grid would feed on its own update.
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        if (!syncing_) {
            emit viewChanged();
        }
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        if (!syncing_) {
            emit viewChanged();
        }
    });
}

void ComparePane::setContent(const QString& key, const QImage& image, const QString& name) {
    key_ = key;
    image_ = image;
    name_ = name;
    item_->setPixmap(QPixmap::fromImage(image));
    refreshBadge();
    fit();
}

void ComparePane::setChoices(const QStringList& ids, const QStringList& labels,
                             const QString& current) {
    choiceIds_ = ids;
    choiceLabels_ = labels;
    if (!current.isEmpty()) {
        current_ = current;
    }
    refreshBadge();
    syncBadgeMenu();
}

void ComparePane::setLocalView(double zoom, const std::optional<QPointF>& center) {
    zoom_ = std::clamp(zoom, kCompareZoomMin, kCompareZoomMax);
    const double scale = base_ * zoom_;
    QTransform transform;
    transform.scale(scale, scale);

    const bool wasSyncing = syncing_;
    syncing_ = true;
    setTransform(transform);
    if (center && item_ != nullptr && !imageSize().isEmpty()) {
        const double cx = center->x() * imageSize().width();
        const double cy = center->y() * imageSize().height();
        centerOn(cx, cy);
    }
    syncing_ = wasSyncing;
    anchorBadge();
}

void ComparePane::fit() {
    // The whole body is one silent step: fitInView moves both scrollbars, and a
    // pane that reports its own view on a scroll would hand the group the
    // position it was about to overwrite.
    const bool wasSyncing = syncing_;
    syncing_ = true;
    if (image_.isNull()) {
        resetTransform();
        base_ = 1.0;
    } else {
        fitInView(item_, Qt::KeepAspectRatio);
        const double scale = transform().m11();
        base_ = scale > 0.0 ? scale : 1.0;
    }
    syncing_ = wasSyncing;
    // Re-apply the shared numbers: the pane's own fit just rewrote the
    // transform, and zoom/centre are multiples of that fit.
    setLocalView(zoom_, centerNormalized());
}

void ComparePane::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier)) {
        altPanning_ = true;
        lastPanPoint_ = event->position().toPoint();
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void ComparePane::mouseMoveEvent(QMouseEvent* event) {
    if (altPanning_) {
        const QPoint point = event->position().toPoint();
        const QPoint delta = point - lastPanPoint_;
        lastPanPoint_ = point;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void ComparePane::mouseReleaseEvent(QMouseEvent* event) {
    if (altPanning_ && event->button() == Qt::LeftButton) {
        altPanning_ = false;
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void ComparePane::wheelEvent(QWheelEvent* event) {
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    const double target = zoom_ * std::pow(kCompareWheelStep, steps);
    setLocalView(target, centerNormalized());
    event->accept();
    emit viewChanged();
}

QPointF ComparePane::centerNormalized() const {
    const QSize size = imageSize();
    const QPointF c = mapToScene(viewport()->rect().center());
    return QPointF(c.x() / size.width(), c.y() / size.height());
}

std::optional<QPointF> ComparePane::center() const {
    if (item_->pixmap().isNull()) {
        return std::nullopt;
    }
    return centerNormalized();
}

QSize ComparePane::imageSize() const {
    return item_->pixmap().isNull() ? QSize(1, 1) : item_->pixmap().size();
}

void ComparePane::refreshBadge() {
    const int index = choiceIds_.indexOf(current_);
    const QString shown = index >= 0 && index < choiceLabels_.size()
                              ? choiceLabels_.at(index)
                              : (name_.isEmpty() ? current_ : name_);
    badge_->setText(shown);
    badge_->adjustSize();
    badge_->setToolTip(QStringLiteral("Select comparison"));
    badge_->setAccessibleName(QStringLiteral("Select %1").arg(shown));
    anchorBadge();
}

void ComparePane::syncBadgeMenu() {
    menu_->clear();
    if (choiceIds_.size() < 2) {
        // One result on screen leaves no choice to offer, so the badge stays
        // the plain label it always was.
        badge_->setPopupMode(QToolButton::DelayedPopup);
        badge_->setMenu(nullptr);
        return;
    }
    badge_->setPopupMode(QToolButton::InstantPopup);
    badge_->setMenu(menu_);
    for (qsizetype i = 0; i < choiceIds_.size(); ++i) {
        QAction* action = menu_->addAction(choiceLabels_.value(i));
        action->setData(choiceIds_.at(i));
        action->setCheckable(true);
        action->setChecked(choiceIds_.at(i) == current_);
    }
}

void ComparePane::anchorBadge() {
    if (badge_ != nullptr) {
        badge_->move(kBadgeX, kBadgeY);
        badge_->raise();
    }
}

void ComparePane::resizeEvent(QResizeEvent* event) {
    // The base resize moves the scrollbars before fit() runs, and an unguarded
    // pane would report that transient position as its own view.
    const bool wasSyncing = syncing_;
    syncing_ = true;
    QGraphicsView::resizeEvent(event);
    syncing_ = wasSyncing;
    fit();
}

void ComparePane::scrollContentsBy(int dx, int dy) {
    // The base moves every child of the viewport by the scroll delta, the badge
    // included; this is the one point where that happens for a pan and a fit
    // alike, so it is the one place to undo it.
    QGraphicsView::scrollContentsBy(dx, dy);
    anchorBadge();
}

}  // namespace mplayer::shell
