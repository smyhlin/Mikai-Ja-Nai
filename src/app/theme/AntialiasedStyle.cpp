#include "AntialiasedStyle.hpp"

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
    if (!arrow && !tick) {
        QProxyStyle::drawPrimitive(element, option, painter, widget);
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

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
    const QRectF box = QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = qMin<qreal>(4.0, box.height() * 0.28);
    const bool checked = option->state.testFlag(State_On);
    const bool enabled = option->state.testFlag(State_Enabled);
    const QColor base = option->palette.color(QPalette::Base);
    const QColor accent = option->palette.color(QPalette::Highlight);
    const QColor border = option->palette.color(QPalette::Mid);

    painter->setPen(QPen(enabled ? border : QColor(border.red(), border.green(), border.blue(), 90),
                         1.0));
    painter->setBrush(checked ? accent : base);
    painter->drawRoundedRect(box, radius, radius);

    if (checked) {
        const QColor ink = option->palette.color(QPalette::HighlightedText);
        QPen pen(ink, qMax(1.6, box.height() * 0.16), Qt::SolidLine, Qt::RoundCap,
                 Qt::RoundJoin);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        QPainterPath tickPath;
        tickPath.moveTo(box.left() + box.width() * 0.24, box.top() + box.height() * 0.52);
        tickPath.lineTo(box.left() + box.width() * 0.43, box.top() + box.height() * 0.72);
        tickPath.lineTo(box.left() + box.width() * 0.76, box.top() + box.height() * 0.30);
        painter->drawPath(tickPath);
    }
    painter->restore();
}

}  // namespace mikai::app::theme
