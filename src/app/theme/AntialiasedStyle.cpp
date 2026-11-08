#include "AntialiasedStyle.hpp"

#include "state/AppearanceStore.hpp"
#include "theme/Appearance.hpp"

#include <QPainter>
#include <QPalette>
#include <QPainterPath>
#include <QPen>
#include <QStyleOption>

#include <cmath>

namespace mikai::app::theme {
namespace {

/// A chevron in the option's own rect: the same shape the sheet's own icons use,
/// scaled to whatever box the style hands over.
QPainterPath chevronPath(const QRectF& box, QStyle::PrimitiveElement element) {
    const qreal cx = box.center().x();
    const qreal cy = box.center().y();
    const qreal half = qMin(box.width(), box.height()) / 2.0 - 1.0;
    const qreal arm = qMax(2.5, half * 0.8);
    QPainterPath path;
    switch (element) {
        case QStyle::PE_IndicatorArrowUp:
            path.moveTo(cx - arm, cy + arm / 2.0);
            path.lineTo(cx, cy - arm / 2.0);
            path.lineTo(cx + arm, cy + arm / 2.0);
            break;
        case QStyle::PE_IndicatorArrowLeft:
            path.moveTo(cx + arm / 2.0, cy - arm);
            path.lineTo(cx - arm / 2.0, cy);
            path.lineTo(cx + arm / 2.0, cy + arm);
            break;
        case QStyle::PE_IndicatorArrowRight:
            path.moveTo(cx - arm / 2.0, cy - arm);
            path.lineTo(cx + arm / 2.0, cy);
            path.lineTo(cx - arm / 2.0, cy + arm);
            break;
        case QStyle::PE_IndicatorArrowDown:
            path.moveTo(cx - arm, cy - arm / 2.0);
            path.lineTo(cx, cy + arm / 2.0);
            path.lineTo(cx + arm, cy - arm / 2.0);
            break;
        default:
            break;
    }
    return path;
}

QColor primitiveInk(const QStyleOption* option, const QWidget* widget) {
    if (option != nullptr) {
        const QColor text = option->palette.color(QPalette::WindowText);
        if (text.isValid()) return text;
    }
    return widget != nullptr ? widget->palette().color(QPalette::WindowText) : QColor(Qt::white);
}

QRectF deviceAlignedRect(const QRectF& rect, const QPainter& painter) {
    const QTransform transform = painter.deviceTransform();
    bool invertible = false;
    const QTransform inverse = transform.inverted(&invertible);
    if (!invertible || !qFuzzyIsNull(transform.m12()) || !qFuzzyIsNull(transform.m21())) {
        return rect;
    }

    const QPointF topLeft = inverse.map(QPointF(qRound(transform.map(rect.topLeft()).x()),
                                                qRound(transform.map(rect.topLeft()).y())));
    const QPointF bottomRight =
        inverse.map(QPointF(qRound(transform.map(rect.bottomRight()).x()),
                            qRound(transform.map(rect.bottomRight()).y())));
    return QRectF(topLeft, bottomRight);
}

qreal deviceScale(const QPainter& painter, bool horizontal) {
    const QTransform transform = painter.deviceTransform();
    return horizontal ? std::hypot(transform.m11(), transform.m12())
                      : std::hypot(transform.m21(), transform.m22());
}

QPainterPath roundedPath(const QRectF& rect, qreal radius) {
    QPainterPath path;
    if (radius <= 0.0) {
        path.addRect(rect);
    } else {
        path.addRoundedRect(rect, radius, radius);
    }
    return path;
}

}  // namespace

AntialiasedStyle::AntialiasedStyle(const QString& baseStyle) : QProxyStyle(baseStyle) {}

void AntialiasedStyle::drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                                     QPainter* painter, const QWidget* widget) const {
    if (painter == nullptr || option == nullptr) {
        QProxyStyle::drawPrimitive(element, option, painter, widget);
        return;
    }

    const bool arrow = element == PE_IndicatorArrowDown || element == PE_IndicatorArrowUp ||
                       element == PE_IndicatorArrowLeft || element == PE_IndicatorArrowRight;
    const bool tick = element == PE_IndicatorCheckBox ||
                      element == PE_IndicatorItemViewItemCheck;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    if (!arrow && !tick) {
        QProxyStyle::drawPrimitive(element, option, painter, widget);
        painter->restore();
        return;
    }

    if (arrow) {
        const QColor ink = primitiveInk(option, widget);
        const bool enabled = option->state.testFlag(State_Enabled);
        QPen pen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        if (!enabled) pen.setColor(QColor(ink.red(), ink.green(), ink.blue(), 110));
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        const QRectF box = QRectF(option->rect).adjusted(1.0, 1.0, -1.0, -1.0);
        painter->drawPath(chevronPath(box, element));
        painter->restore();
        return;
    }

    // A checkbox indicator: the sheet does not style one, so this matches the
    // rounded control language instead of the platform's square raster tick.
    // Snap both edges to physical-pixel boundaries before drawing. At fractional
    // DPR, an integer logical indicator size can otherwise put opposite edges
    // on different pixel phases (for example, a 13 px box at 1.75 DPR).
    const QRectF box = deviceAlignedRect(QRectF(option->rect), *painter);
    const qreal scaleX = qMax<qreal>(1.0, deviceScale(*painter, true));
    const qreal scaleY = qMax<qreal>(1.0, deviceScale(*painter, false));
    const qreal borderWidth = qMax(1, qRound(scaleX)) / scaleX;
    const QRectF inner = box.adjusted(borderWidth, borderWidth, -borderWidth, -borderWidth);
    const qreal radius = qRound(qMin<qreal>(
        mikai::app::theme::radiusSmall(mikai::app::currentAppearance()), box.height() * 0.28) *
                                scaleY) /
                         scaleY;
    const qreal innerRadius = qMax<qreal>(0.0, radius - borderWidth);
    const bool checked = option->state.testFlag(State_On);
    const bool partial = option->state.testFlag(State_NoChange);
    const bool enabled = option->state.testFlag(State_Enabled);
    const bool hovered = option->state.testFlag(State_MouseOver);
    const bool pressed = option->state.testFlag(State_Sunken);
    const bool focused = option->state.testFlag(State_HasFocus);
    const QColor base = option->palette.color(QPalette::Base);
    const QColor accent = option->palette.color(QPalette::Highlight);
    const QColor border = focused || hovered ? accent : option->palette.color(QPalette::Mid);
    QColor fill = checked || partial ? accent : base;
    if (pressed && !checked && !partial) fill = option->palette.color(QPalette::AlternateBase);
    if (!enabled) {
        fill = checked || partial ? option->palette.color(QPalette::Disabled, QPalette::Mid) : base;
    }
    const QColor outline =
        enabled ? border : option->palette.color(QPalette::Disabled, QPalette::Mid);

    painter->setPen(Qt::NoPen);
    painter->setBrush(outline);
    painter->drawPath(roundedPath(box, radius));
    painter->setBrush(fill);
    painter->drawPath(roundedPath(inner, innerRadius));

    if (checked || partial) {
        const QColor ink =
            option->palette.color(enabled ? QPalette::Active : QPalette::Disabled,
                                  QPalette::HighlightedText);
        QPen pen(ink, qMax(1.6, box.height() * 0.16), Qt::SolidLine, Qt::RoundCap,
                 Qt::RoundJoin);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        QPainterPath tickPath;
        if (partial) {
            tickPath.moveTo(box.left() + box.width() * 0.28, box.center().y());
            tickPath.lineTo(box.right() - box.width() * 0.28, box.center().y());
        } else {
            tickPath.moveTo(box.left() + box.width() * 0.24, box.top() + box.height() * 0.52);
            tickPath.lineTo(box.left() + box.width() * 0.43, box.top() + box.height() * 0.72);
            tickPath.lineTo(box.left() + box.width() * 0.76, box.top() + box.height() * 0.30);
        }
        painter->drawPath(tickPath);
    }
    painter->restore();
}

}  // namespace mikai::app::theme
