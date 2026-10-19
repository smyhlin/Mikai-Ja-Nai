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
    const QString accent = sideIndex == 0 ? QStringLiteral("#3b82f6") : QStringLiteral("#f59e0b");
    return QStringLiteral(
               "QWidget#compareSelectorBar {"
               "  background-color: #16181d;"
               "  border: none;"
               "  border-top: 1px solid #282c34;"
               "}"
               "QToolButton {"
               "  background: #16181d;"
               "  color: #e5e7eb;"
               "  border: none;"
               "  border-left: 3px solid %1;"
               "  border-radius: 0px;"
               "  padding: 0px %2px;"
               "  font-weight: 600;"
               "  text-align: left;"
               "}"
               "QToolButton:hover {"
               "  background-color: #1e2128;"
               "  color: #ffffff;"
               "}"
               "QToolButton::menu-indicator {"
               "  image: none;"
               "  width: 0px;"
               "}")
        .arg(accent, QString::number(kCompareSelectorInset));
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

ComparePane::ComparePane(QWidget* parent) : QGraphicsView(parent) {
    scene_ = new QGraphicsScene(this);
    item_ = new QGraphicsPixmapItem();
    item_->setTransformationMode(Qt::SmoothTransformation);
    scene_->addItem(item_);
    setScene(scene_);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setDragMode(QGraphicsView::NoDrag);
    setRenderHint(QPainter::Antialiasing, true);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setFrameShape(QFrame::NoFrame);
    setBackgroundBrush(QColor(11, 12, 14));
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
    badge_->setFont(uiFont(9, true));
    badge_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    badge_->setPopupMode(QToolButton::InstantPopup);
    badge_->setMinimumWidth(0);
    badge_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    badge_->installEventFilter(this);
    row->addWidget(badge_, 1);

    menu_ = new QMenu(badge_);
    badge_->setMenu(menu_);
    connect(menu_, &QMenu::triggered, this, [this](QAction* action) {
        if (action == nullptr) {
            return;
        }
        const QString id = action->data().toString();
        if (id.isEmpty() || id == current_) {
            return;
        }
        current_ = id;
        key_ = id;
        refreshBadge();
        syncBadgeMenu();
        emit choiceChosen(id);
    });

    // A drag pans the pane through its scrollbars; that is a user gesture, so
    // the shared view has to follow. A view this pane is applying itself must
    // not report back, or the grid would feed on its own update.
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        if (!syncing_ && !image_.isNull()) {
            lastNormalizedCenter_ = centerNormalized();
            emit viewChanged();
        }
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        if (!syncing_ && !image_.isNull()) {
            lastNormalizedCenter_ = centerNormalized();
            emit viewChanged();
        }
    });
}

void ComparePane::setSideIndex(int sideIndex) {
    if (sideIndex_ == sideIndex) return;
    sideIndex_ = sideIndex;
    if (selectorBar_ != nullptr) {
        selectorBar_->setStyleSheet(selectorBarStyle(sideIndex_));
    }
    refreshBadge();
    if (viewport() != nullptr) viewport()->update();
}

void ComparePane::setContent(const QString& key, const QImage& image, const QString& name) {
    key_ = key;
    if (!key.isEmpty()) {
        current_ = key;
    }
    image_ = image;
    name_ = name;
    item_->setPixmap(QPixmap::fromImage(image));
    scene_->setSceneRect(image.isNull() ? QRectF() : QRectF(QPointF(0, 0), image.size()));
    refreshBadge();
    syncBadgeMenu();
    fit();
    if (viewport() != nullptr) viewport()->update();
}

void ComparePane::setChoices(const QVector<ChoiceEntry>& choices, const QString& current) {
    choices_ = choices;
    if (!current.isEmpty()) {
        current_ = current;
        if (key_.isEmpty()) key_ = current;
    }
    refreshBadge();
    syncBadgeMenu();
}

void ComparePane::setLocalView(double zoom, const std::optional<QPointF>& center) {
    zoom_ = std::clamp(zoom, kCompareZoomMin, kCompareZoomMax);
    if (center.has_value()) {
        lastNormalizedCenter_ = *center;
    }
    const double scale = base_ * zoom_;
    QTransform transform;
    transform.scale(scale, scale);

    const bool wasSyncing = syncing_;
    syncing_ = true;
    setTransform(transform);
    if (item_ != nullptr && !item_->pixmap().isNull() && !imageSize().isEmpty()) {
        const QPointF target = center.value_or(lastNormalizedCenter_);
        const double cx = target.x() * imageSize().width();
        const double cy = target.y() * imageSize().height();
        centerOn(cx, cy);
    }
    syncing_ = wasSyncing;
    placeSelector();
    if (viewport() != nullptr) viewport()->update();
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
    setLocalView(zoom_, lastNormalizedCenter_);
}

void ComparePane::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        panning_ = true;
        lastPanPoint_ = event->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void ComparePane::mouseMoveEvent(QMouseEvent* event) {
    const QPoint point = event->position().toPoint();
    if (panning_) {
        const QPoint delta = point - lastPanPoint_;
        lastPanPoint_ = point;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        if (!image_.isNull()) {
            lastNormalizedCenter_ = centerNormalized();
        }
        event->accept();
        return;
    }
    if (!image_.isNull()) {
        const QPointF sp = mapToScene(point);
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
    if (panning_ && event->button() == Qt::LeftButton) {
        panning_ = false;
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
    setLocalView(target, centerNormalized());
    event->accept();
    emit viewChanged();
}

QPointF ComparePane::centerNormalized() const {
    if (item_ == nullptr || item_->pixmap().isNull() || viewport() == nullptr) {
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
    const QString sideTag = sideIndex_ == 0 ? QStringLiteral("A") : QStringLiteral("B");
    label_ = shown.isEmpty() ? sideTag : tr("%1 · %2").arg(sideTag, shown);
    fitSelectorLabel(badge_, label_);
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
        // A menu tree that keeps its entries only has new words: restating the
        // leaves keeps every action (and an open menu) alive. Anything that
        // changes the tree is built fresh.
        if (leaves_.size() == choices_.size() && sameChoiceShape(choices_, builtChoices_)) {
            restateChoiceLeaves(leaves_, choices_);
        } else {
            fillChoiceMenu(menu_, choices_, current_, &leaves_);
        }
        builtChoices_ = choices_;
    }
    markChoice(leaves_, choices_, current_);
}

QRect ComparePane::imageBarRect() const {
    if (width() <= 0 || height() <= 0) {
        return {};
    }
    return QRect(0, std::max(0, height() - kCompareSelectorBarHeight), width(),
                 kCompareSelectorBarHeight);
}

void ComparePane::layoutSelectorRow() {
    const bool show = !image_.isNull() || !choices_.isEmpty() || !name_.isEmpty();
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
    if (image_.isNull() || viewport() == nullptr) {
        return;
    }
    painter->save();
    painter->resetTransform();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QString sideTag = sideIndex_ == 0 ? QStringLiteral("A") : QStringLiteral("B");
    const QColor accent = sideIndex_ == 0 ? QColor(59, 130, 246) : QColor(245, 158, 11);
    const QString text =
        QStringLiteral("%1 · %2×%3").arg(sideTag).arg(image_.width()).arg(image_.height());

    const QFont font = uiFont(8, true);
    painter->setFont(font);
    const QFontMetrics fm(font);
    const int textW = fm.horizontalAdvance(text);
    const QRectF pillRect(8.0, 8.0, textW + 24.0, 22.0);

    painter->setPen(QPen(QColor(40, 44, 52, 220), 1.0));
    painter->setBrush(QColor(13, 14, 17, 210));
    painter->drawRoundedRect(pillRect, 4.0, 4.0);

    painter->setPen(Qt::NoPen);
    painter->setBrush(accent);
    painter->drawEllipse(QPointF(pillRect.left() + 9.0, pillRect.center().y()), 3.5, 3.5);

    painter->setPen(QColor(229, 231, 235));
    painter->drawText(pillRect.adjusted(17.0, 0.0, -6.0, 0.0), Qt::AlignVCenter | Qt::AlignLeft,
                      text);
    painter->restore();
}

bool ComparePane::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Wheel && (watched == selectorBar_ || watched == badge_)) {
        wheelEvent(static_cast<QWheelEvent*>(event));
        return true;
    }
    return QGraphicsView::eventFilter(watched, event);
}

void ComparePane::changeEvent(QEvent* event) {
    QGraphicsView::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void ComparePane::retranslateUi() {
    // The selector's label glue, tooltip and spoken name are this pane's
    // words. The shown candidate's name comes from the caller through
    // setChoices, so the old string is never re-translated. refreshBadge()
    // re-places the selector but leaves the picture, the fit, the shared
    // zoom/centre and the selector's menu alone: no setContent, no fit, no
    // refit of the view.
    refreshBadge();
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
