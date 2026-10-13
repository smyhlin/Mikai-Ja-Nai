#include "ComparePane.hpp"

#include <QAction>
#include <QFontMetrics>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSizePolicy>
#include <QToolButton>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "CompareLayout.hpp"
#include "ShellTheme.hpp"

namespace mplayer::shell {

namespace {

QString selectorBarStyle() {
    const QColor bar(dock().red(), dock().green(), dock().blue());
    return QStringLiteral(
               "QWidget#compareSelectorBar { background-color: %1; border: none;"
               " border-top: 1px solid %2; }"
               "QToolButton { background: transparent; color: %3; border: none;"
               " border-radius: 0px; padding: 0px %4px; padding-right: %5px;"
               " text-align: left; }"
               "QToolButton:hover { background-color: %6; }"
               "QToolButton::menu-indicator { subcontrol-position: right center;"
               " width: %7px; }")
        .arg(bar.name(), w15().name(QColor::HexArgb), w95().name(),
             QString::number(kCompareSelectorInset),
             QString::number(kCompareSelectorInset + kCompareSelectorArrow),
             white(26).name(QColor::HexArgb), QString::number(kCompareSelectorArrow));
}

void fitSelectorLabel(QToolButton* button, const QString& full) {
    const int pad = kCompareSelectorInset * 2 + kCompareSelectorArrow;
    const int width = std::max(0, button->width() - pad);
    button->setText(QFontMetrics(button->font()).elidedText(full, Qt::ElideRight, width));
}

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
    setBackgroundBrush(bg0());
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true);

    selectorBar_ = new QWidget(this);
    selectorBar_->setObjectName(QStringLiteral("compareSelectorBar"));
    selectorBar_->setAttribute(Qt::WA_StyledBackground, true);
    selectorBar_->setStyleSheet(selectorBarStyle());
    selectorBar_->hide();
    selectorBar_->installEventFilter(this);
    auto* row = new QHBoxLayout(selectorBar_);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    badge_ = new QToolButton(selectorBar_);
    badge_->setCursor(Qt::PointingHandCursor);
    badge_->setFont(uiFont(9));
    badge_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    badge_->setMinimumWidth(0);
    badge_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    badge_->installEventFilter(this);
    row->addWidget(badge_, 1);

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

void ComparePane::setChoices(const QVector<ChoiceEntry>& choices, const QString& current) {
    choices_ = choices;
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
    placeSelector();
}

void ComparePane::setSmoothScaling(bool smooth) {
    setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    if (viewport() != nullptr) {
        viewport()->update();
    }
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
        setCursor(Qt::ClosedHandCursor);
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
        unsetCursor();
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
    // The registry's own label for what this pane is showing is the truth; the
    // handed-in name is only the fallback for an id the registry does not carry.
    QString shown = name_;
    for (const ChoiceEntry& entry : choices_) {
        if (entry.id == current_) {
            shown = entry.label;
            break;
        }
    }
    if (shown.isEmpty()) {
        shown = current_;
    }
    label_ = shown;
    badge_->setText(label_);
    badge_->setToolTip(tr("Select comparison"));
    badge_->setAccessibleName(tr("Select %1").arg(shown));
    placeSelector();
}

void ComparePane::syncBadgeMenu() {
    if (choices_.size() < 2) {
        // One result on screen leaves no choice to offer, so the badge stays
        // the plain label it always was.
        clearChoiceMenu(menu_);
        builtChoices_.clear();
        leaves_.clear();
        badge_->setPopupMode(QToolButton::DelayedPopup);
        badge_->setMenu(nullptr);
        return;
    }
    badge_->setPopupMode(QToolButton::InstantPopup);
    badge_->setMenu(menu_);
    if (choices_ != builtChoices_ || leaves_.size() != choices_.size()) {
        fillChoiceMenu(menu_, choices_, current_, &leaves_);
        builtChoices_ = choices_;
    } else {
        markChoice(leaves_, choices_, current_);
    }
}

QRect ComparePane::imageBarRect() const {
    if (item_ == nullptr || item_->pixmap().isNull() || viewport() == nullptr) {
        return {};
    }
    const QRect visible =
        mapFromScene(item_->sceneBoundingRect()).boundingRect().intersected(viewport()->rect());
    if (visible.width() < 1) {
        return {};
    }
    return QRect(viewport()->pos().x() + visible.left(),
                 std::max(0, height() - kCompareSelectorBarHeight), visible.width(),
                 kCompareSelectorBarHeight);
}

void ComparePane::layoutSelectorRow() {
    const bool show = !image_.isNull();
    const int bar = show ? kCompareSelectorBarHeight : 0;
    if (viewportMargins().bottom() != bar) {
        setViewportMargins(0, 0, 0, bar);
    }
    if (selectorBar_ == nullptr) {
        return;
    }
    if (!show) {
        selectorBar_->hide();
        return;
    }
    const QRect span = imageBarRect();
    if (span.isEmpty()) {
        selectorBar_->hide();
        return;
    }
    selectorBar_->setGeometry(span);
    selectorBar_->show();
    selectorBar_->raise();
}

void ComparePane::placeSelector() {
    if (selectorBar_ == nullptr || badge_ == nullptr) {
        return;
    }
    layoutSelectorRow();
    if (image_.isNull() || selectorBar_->isHidden()) {
        badge_->hide();
        return;
    }
    badge_->setMaximumWidth(QWIDGETSIZE_MAX);
    badge_->show();
    if (QLayout* layout = selectorBar_->layout()) {
        layout->activate();
    }
    fitSelectorLabel(badge_, label_);
}

bool ComparePane::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Wheel && (watched == selectorBar_ || watched == badge_)) {
        wheelEvent(static_cast<QWheelEvent*>(event));
        return true;
    }
    return QGraphicsView::eventFilter(watched, event);
}

void ComparePane::resizeEvent(QResizeEvent* event) {
    // The base resize moves the scrollbars before fit() runs, and an unguarded
    // pane would report that transient position as its own view. The selector
    // row claims its margin before the fit, so the picture is fitted to the
    // area above the bar.
    const bool wasSyncing = syncing_;
    syncing_ = true;
    QGraphicsView::resizeEvent(event);
    layoutSelectorRow();
    syncing_ = wasSyncing;
    fit();
}

void ComparePane::scrollContentsBy(int dx, int dy) {
    QGraphicsView::scrollContentsBy(dx, dy);
    placeSelector();
}

}  // namespace mplayer::shell
