#include "ComparePane.hpp"

#include <QAction>
#include <QEvent>
#include <QFontMetrics>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
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

QString selectorBarStyle(int sideIndex) {
    const QColor accent = sideIndex == 1 ? QColor(245, 158, 11) : QColor(59, 130, 246);
    return QStringLiteral(
               "QWidget#compareSelectorBar {"
               "  background-color: #16181d;"
               "  border: none;"
               "  border-top: 1px solid #282c34;"
               "  border-left: 3px solid %1;"
               "}"
               "QToolButton {"
               "  background: transparent;"
               "  color: #e8eaed;"
               "  border: none;"
               "  border-radius: 0px;"
               "  padding: 0px %2px;"
               "  font-weight: 600;"
               "  text-align: left;"
               "}"
               "QToolButton:hover {"
               "  background-color: #22262e;"
               "  color: #ffffff;"
               "}"
               "QToolButton:pressed {"
               "  background-color: #2a2f3a;"
               "}"
               "QToolButton::menu-indicator {"
               "  image: none;"
               "  width: 0px;"
               "}")
        .arg(accent.name(), QString::number(kCompareSelectorInset));
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

ComparePane::ComparePane(QWidget* parent) : QGraphicsView(parent) {
    scene_ = new QGraphicsScene(this);
    item_ = new QGraphicsPixmapItem();
    item_->setTransformationMode(Qt::SmoothTransformation);
    scene_->addItem(item_);
    setScene(scene_);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setRenderHint(QPainter::Antialiasing, true);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setFrameShape(QFrame::NoFrame);
    setBackgroundBrush(QColor(15, 17, 21));
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true);
    if (viewport() != nullptr) {
        viewport()->setMouseTracking(true);
    }

    selectorBar_ = new QWidget(this);
    selectorBar_->setObjectName(QStringLiteral("compareSelectorBar"));
    selectorBar_->setAttribute(Qt::WA_StyledBackground, true);
    selectorBar_->setStyleSheet(selectorBarStyle(sideIndex_));
    selectorBar_->hide();
    selectorBar_->installEventFilter(this);
    auto* row = new QHBoxLayout(selectorBar_);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    badge_ = new QToolButton(selectorBar_);
    badge_->setCursor(Qt::PointingHandCursor);
    badge_->setFont(uiFont(10));
    badge_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    badge_->setPopupMode(QToolButton::InstantPopup);
    badge_->setMinimumWidth(0);
    badge_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    badge_->installEventFilter(this);
    row->addWidget(badge_, 1);

    menu_ = new QMenu(badge_);
    badge_->setMenu(menu_);
    connect(menu_, &QMenu::triggered, this, [this](QAction* action) {
        if (action == nullptr ||
            (action->property("_mplayer_forwarded_leaf").toBool() &&
             !action->property("_mplayer_leaf_emitting").toBool())) {
            return;
        }
        const QString id = action->data().toString();
        if (id.isEmpty()) {
            return;
        }
        current_ = id;
        key_ = id;
        refreshBadge();
        syncBadgeMenu();
        emit choiceChosen(id);
    });

    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        if (!syncing_) {
            if (!item_->pixmap().isNull()) {
                lastNormalizedCenter_ = centerNormalized();
            }
            emit viewChanged();
        }
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        if (!syncing_) {
            if (!item_->pixmap().isNull()) {
                lastNormalizedCenter_ = centerNormalized();
            }
            emit viewChanged();
        }
    });
}

void ComparePane::setSideIndex(int sideIndex) {
    if (sideIndex_ == sideIndex) {
        return;
    }
    sideIndex_ = sideIndex;
    if (selectorBar_ != nullptr) {
        selectorBar_->setStyleSheet(selectorBarStyle(sideIndex_));
    }
    refreshBadge();
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

void ComparePane::setStatusText(const QString& status) {
    if (statusText_ == status) {
        return;
    }
    statusText_ = status;
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

void ComparePane::setContent(const QString& key, const QImage& image, const QString& name) {
    key_ = key;
    if (!key.isEmpty()) {
        current_ = key;
    }
    image_ = image;
    name_ = name;
    const bool wasSyncing = syncing_;
    syncing_ = true;
    item_->setPixmap(QPixmap::fromImage(image));
    if (!image.isNull()) {
        scene_->setSceneRect(QRectF(QPointF(0, 0), image.size()));
    } else {
        scene_->setSceneRect(QRectF());
    }
    syncing_ = wasSyncing;
    refreshBadge();
    syncBadgeMenu();
    fit();
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

void ComparePane::setChoices(const QVector<ChoiceEntry>& choices, const QString& current) {
    choices_ = choices;
    if (!current.isEmpty()) {
        current_ = current;
        if (key_.isEmpty()) {
            key_ = current;
        }
    }
    refreshBadge();
    syncBadgeMenu();
}

void ComparePane::setLocalView(double zoom, const std::optional<QPointF>& center) {
    zoom_ = std::clamp(zoom, kCompareZoomMin, kCompareZoomMax);
    if (center.has_value() && std::isfinite(center->x()) && std::isfinite(center->y())) {
        lastNormalizedCenter_ = QPointF(std::clamp(center->x(), 0.0, 1.0),
                                        std::clamp(center->y(), 0.0, 1.0));
    }
    const double scale = base_ * zoom_;
    QTransform transform;
    transform.scale(scale, scale);

    const bool wasSyncing = syncing_;
    syncing_ = true;
    setTransform(transform);
    if (item_ != nullptr && !item_->pixmap().isNull() && !imageSize().isEmpty()) {
        const QPointF targetCenter = center.has_value() ? lastNormalizedCenter_ : QPointF(0.5, 0.5);
        const double cx = targetCenter.x() * imageSize().width();
        const double cy = targetCenter.y() * imageSize().height();
        centerOn(cx, cy);
    }
    syncing_ = wasSyncing;
    placeSelector();
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

void ComparePane::setSmoothScaling(bool smooth) {
    setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    if (item_ != nullptr) {
        item_->setTransformationMode(smooth ? Qt::SmoothTransformation
                                            : Qt::FastTransformation);
    }
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

void ComparePane::fit() {
    const bool wasSyncing = syncing_;
    syncing_ = true;
    layoutSelectorRow();
    if (image_.isNull() || item_->pixmap().isNull()) {
        resetTransform();
        base_ = 1.0;
    } else {
        fitInView(item_, Qt::KeepAspectRatio);
        const double scale = transform().m11();
        base_ = scale > 0.0 ? scale : 1.0;
    }
    syncing_ = wasSyncing;
    setLocalView(zoom_, lastNormalizedCenter_);
}

void ComparePane::mousePressEvent(QMouseEvent* event) {
    if ((event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier)) ||
        event->button() == Qt::MiddleButton) {
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
    if (!image_.isNull()) {
        const QPointF sp = mapToScene(event->position().toPoint());
        const int x = static_cast<int>(std::floor(sp.x()));
        const int y = static_cast<int>(std::floor(sp.y()));
        if (x >= 0 && y >= 0 && x < image_.width() && y < image_.height()) {
            emit hoverPixel(x, y, true, image_.pixel(x, y));
        } else {
            emit hoverPixel(0, 0, false, 0);
        }
    } else {
        emit hoverPixel(0, 0, false, 0);
    }
    QGraphicsView::mouseMoveEvent(event);
}

void ComparePane::mouseReleaseEvent(QMouseEvent* event) {
    if (altPanning_ && (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton)) {
        altPanning_ = false;
        unsetCursor();
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void ComparePane::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        zoom_ = 1.0;
        lastNormalizedCenter_ = QPointF(0.5, 0.5);
        fit();
        emit viewChanged();
        event->accept();
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void ComparePane::leaveEvent(QEvent* event) {
    emit hoverPixel(0, 0, false, 0);
    QGraphicsView::leaveEvent(event);
}

void ComparePane::wheelEvent(QWheelEvent* event) {
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    const double target = zoom_ * std::pow(kCompareWheelStep, steps);
    setLocalView(target, center());
    event->accept();
    emit viewChanged();
}

QPointF ComparePane::centerNormalized() const {
    if (item_ == nullptr || item_->pixmap().isNull()) {
        return lastNormalizedCenter_;
    }
    const QSize size = imageSize();
    if (size.width() <= 0 || size.height() <= 0) {
        return lastNormalizedCenter_;
    }
    const QPointF c = mapToScene(viewport()->rect().center());
    return QPointF(std::clamp(c.x() / size.width(), 0.0, 1.0),
                   std::clamp(c.y() / size.height(), 0.0, 1.0));
}

std::optional<QPointF> ComparePane::center() const {
    if (item_ == nullptr || item_->pixmap().isNull()) {
        return lastNormalizedCenter_;
    }
    return centerNormalized();
}

QSize ComparePane::imageSize() const {
    return (item_ == nullptr || item_->pixmap().isNull()) ? QSize(1, 1) : item_->pixmap().size();
}

void ComparePane::refreshBadge() {
    QString shown = name_;
    const QString lookupId = !current_.isEmpty() ? current_ : key_;
    for (const ChoiceEntry& entry : choices_) {
        if (entry.id == lookupId) {
            shown = entry.label;
            break;
        }
    }
    if (shown.isEmpty()) {
        shown = lookupId;
    }
    if (sideIndex_ == 0 || sideIndex_ == 1) {
        const QString sidePrefix = sideIndex_ == 0 ? QStringLiteral("A") : QStringLiteral("B");
        label_ = tr("%1 · %2").arg(sidePrefix, shown);
    } else {
        label_ = shown;
    }
    badge_->setText(label_);
    badge_->setToolTip(tr("Click to choose comparison model (%1)").arg(shown));
    badge_->setAccessibleName(tr("Select %1").arg(shown));
    placeSelector();
}

void ComparePane::syncBadgeMenu() {
    if (choices_.size() < 2) {
        clearChoiceMenu(menu_);
        builtChoices_.clear();
        leaves_.clear();
        badge_->setPopupMode(QToolButton::DelayedPopup);
        badge_->setMenu(nullptr);
        return;
    }
    badge_->setPopupMode(QToolButton::InstantPopup);
    badge_->setMenu(menu_);
    const QString activeId = !current_.isEmpty() ? current_ : key_;
    if (choices_ != builtChoices_ || leaves_.size() != choices_.size()) {
        fillChoiceMenu(menu_, choices_, activeId, &leaves_);
        builtChoices_ = choices_;
    } else {
        markChoice(leaves_, choices_, activeId);
    }
}

QRect ComparePane::imageBarRect() const {
    if (viewport() == nullptr) {
        return {};
    }
    const int barY = std::max(0, height() - kCompareSelectorBarHeight);
    const int vpLeft = viewport()->pos().x();
    const int vpWidth = std::max(1, viewport()->width());
    return QRect(vpLeft, barY, vpWidth, kCompareSelectorBarHeight);
}

void ComparePane::layoutSelectorRow() {
    const bool show = !image_.isNull() || !current_.isEmpty() || !key_.isEmpty() ||
                      !name_.isEmpty() || !choices_.isEmpty();
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
    if (selectorBar_->isHidden()) {
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

void ComparePane::drawForeground(QPainter* painter, const QRectF&) {
    if (painter == nullptr || viewport() == nullptr) {
        return;
    }
    painter->save();
    painter->resetTransform();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QRect vp = viewport()->rect();
    if (image_.isNull()) {
        const QString msg = !statusText_.isEmpty()
                                ? statusText_
                                : (!label_.isEmpty() ? tr("Rendering %1…").arg(label_)
                                                     : tr("No image"));
        QFont f = uiFont(10);
        f.setWeight(QFont::DemiBold);
        painter->setFont(f);
        painter->setPen(QColor(210, 214, 222));
        painter->drawText(vp.adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap, msg);
    } else if (sideIndex_ == 0 || sideIndex_ == 1) {
        const QString sideTag = sideIndex_ == 0 ? QStringLiteral("A") : QStringLiteral("B");
        const QString resTag = QStringLiteral("%1×%2").arg(image_.width()).arg(image_.height());
        const QString pillText = QStringLiteral("%1  %2").arg(sideTag, resTag);

        QFont pillFont = uiFont(8);
        pillFont.setWeight(QFont::Bold);
        painter->setFont(pillFont);
        const QFontMetrics fm(pillFont);
        const int textW = fm.horizontalAdvance(pillText);
        const QRectF pillRect(8.0, 8.0, textW + 16.0, 20.0);

        painter->setPen(QPen(QColor(46, 51, 61, 220), 1.0));
        painter->setBrush(QColor(18, 20, 25, 215));
        painter->drawRoundedRect(pillRect, 4.0, 4.0);

        const QColor sideAccent = sideIndex_ == 1 ? QColor(245, 158, 11) : QColor(59, 130, 246);
        painter->setPen(sideAccent);
        painter->drawText(QRectF(pillRect.left() + 7.0, pillRect.top(), 14.0, pillRect.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, sideTag);
        painter->setPen(QColor(220, 224, 232));
        painter->drawText(QRectF(pillRect.left() + 19.0, pillRect.top(),
                                 pillRect.width() - 24.0, pillRect.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, resTag);
    }

    painter->restore();
}

bool ComparePane::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Wheel && (watched == selectorBar_ || watched == badge_)) {
        wheelEvent(static_cast<QWheelEvent*>(event));
        return true;
    }
    if ((event->type() == QEvent::Resize || event->type() == QEvent::Show) &&
        (watched == selectorBar_ || watched == badge_)) {
        fitSelectorLabel(badge_, label_);
    }
    return QGraphicsView::eventFilter(watched, event);
}

void ComparePane::resizeEvent(QResizeEvent* event) {
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
    if (viewport() != nullptr) {
        viewport()->update();
    }
}

}  // namespace mplayer::shell
