#include "widgets/CarouselButton.hpp"

#include "theme/Appearance.hpp"
#include "state/AppearanceStore.hpp"
#include "theme/Palette.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QEvent>
#include <QPalette>

#include <algorithm>

namespace mikai::app {

CarouselButton::CarouselButton(Direction direction, QWidget* parent)
    : QPushButton(parent), direction_(direction) {
    setObjectName(QStringLiteral("carouselControl"));
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setFlat(true);
}

bool CarouselButton::event(QEvent* event) {
    // The paint reads hover, press and focus state directly, so each of those
    // has to ask for a repaint; a style would have done it implicitly.
    switch (event->type()) {
        case QEvent::Enter:
        case QEvent::Leave:
        case QEvent::FocusIn:
        case QEvent::FocusOut:
            update();
            break;
        default:
            break;
    }
    return QPushButton::event(event);
}

void CarouselButton::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const theme::Palette colors = theme::paletteFor(currentAppearance());
    const bool enabled = isEnabled();
    const bool hovered = underMouse() && enabled;
    const bool pressed = isDown() && enabled;

    QColor fill = QColor(colors.surfaceControlRaised);
    QColor border = QColor(colors.borderControl);
    QColor ink = enabled ? QColor(colors.text) : QColor(colors.textDisabled);
    if (pressed) {
        fill = QColor(colors.surfaceActive);
    } else if (hovered) {
        fill = QColor(colors.surfaceHover);
        border = QColor(colors.accent);
    }
    if (hasFocus() && enabled) border = QColor(colors.accent);
    if (!enabled) {
        fill.setAlpha(20);
        border.setAlpha(40);
    }

    const QRectF box(0.5, 0.5, width() - 1.0, height() - 1.0);
    const qreal radius = std::min(box.width(), box.height()) * 0.30;
    painter.setPen(QPen(border, 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(box, radius, radius);

    // The chevron is half the box and stroked with round caps, so the arrow keeps
    // the same weight as the painted glyphs elsewhere in the app.
    const qreal arm = std::min(box.width(), box.height()) * 0.18;
    const QPointF centre = box.center();
    QPainterPath path;
    if (direction_ == Direction::Left) {
        path.moveTo(centre.x() + arm * 0.6, centre.y() - arm);
        path.lineTo(centre.x() - arm * 0.6, centre.y());
        path.lineTo(centre.x() + arm * 0.6, centre.y() + arm);
    } else {
        path.moveTo(centre.x() - arm * 0.6, centre.y() - arm);
        path.lineTo(centre.x() + arm * 0.6, centre.y());
        path.lineTo(centre.x() - arm * 0.6, centre.y() + arm);
    }
    painter.setPen(QPen(ink, std::max<qreal>(1.6, box.height() * 0.07), Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
}

}  // namespace mikai::app
