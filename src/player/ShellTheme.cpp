// Implementation of the shared shell tokens: time formatting, the rounded-rect
// primitive and the 24-unit painted icon set. See ShellTheme.hpp for the
// contract; the tokens themselves are inline in that header.

#include "ShellTheme.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <cmath>

namespace mplayer {
namespace shell {

namespace {

/// Glyphs take an optional override colour; an invalid QColor means "use the
/// token this glyph was designed with", so callers can pass W_90() blindly.
QColor resolve(const QColor& c, const QColor& token) { return c.isValid() ? c : token; }

/// Pen for a glyph: width is expressed in 24-grid units, and the painter is
/// already scaled by size/24, so dividing by `k` keeps the stroke optically
/// even at any requested pixel size.
QPen glyphPen(const QColor& color, double width) {
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap);
}

/// Same, plus the round joins used by the base stroke state.
QPen strokePen(const QColor& color, double width) {
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

}  // namespace

QString formatTime(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        seconds = 0.0;
    }
    const long long total = static_cast<long long>(seconds);
    const long long m = total / 60;
    const long long s = total % 60;
    return QStringLiteral("%1:%2")
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

QString formatSigned(double seconds) { return QStringLiteral("-") + formatTime(seconds); }

void paintRounded(QPainter& painter, const QRectF& rect, double radius, const QColor& fill,
                  const QColor& border, double borderWidth) {
    if (border.isValid()) {
        painter.setPen(QPen(border, borderWidth));
    } else {
        painter.setPen(Qt::NoPen);
    }
    painter.setBrush(QBrush(fill));
    painter.drawRoundedRect(rect, radius, radius);
}

// ── IconPainter ────────────────────────────────────────────────────────────

void IconPainter::stroke(QPainter& p, qreal x, qreal y, qreal size, qreal width) {
    p.save();
    p.translate(x, y);
    const qreal k = size / 24.0;
    p.scale(k, k);
    p.setBrush(Qt::NoBrush);
    p.setPen(strokePen(w90(), width / k));
}

void IconPainter::fill(QPainter& p, qreal x, qreal y, qreal size) {
    p.save();
    p.translate(x, y);
    const qreal k = size / 24.0;
    p.scale(k, k);
    p.setPen(Qt::NoPen);
}

void IconPainter::minimize(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    stroke(p, x, y, s);
    p.setPen(glyphPen(resolve(c, w90()), 1.8 * 24 / s));
    p.drawLine(QPointF(7, 12), QPointF(17, 12));
    p.restore();
}

void IconPainter::restore(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    stroke(p, x, y, s);
    const QPen pen = glyphPen(resolve(c, w90()), 1.8 * 24 / s);
    p.setPen(pen);
    p.drawRoundedRect(QRectF(4, 8, 12, 12), 2, 2);
    p.drawLine(QPointF(8, 8), QPointF(8, 4));
    p.drawLine(QPointF(8, 4), QPointF(20, 4));
    p.drawLine(QPointF(20, 4), QPointF(20, 16));
    p.drawLine(QPointF(20, 16), QPointF(16, 16));
    p.restore();
}

void IconPainter::close(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    stroke(p, x, y, s);
    p.setPen(glyphPen(resolve(c, w90()), 1.8 * 24 / s));
    p.drawLine(QPointF(7.5, 7.5), QPointF(16.5, 16.5));
    p.drawLine(QPointF(16.5, 7.5), QPointF(7.5, 16.5));
    p.restore();
}

void IconPainter::chevronLeft(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    stroke(p, x, y, s, 2.2);
    p.setPen(glyphPen(resolve(c, w90()), 2.2 * 24 / s));
    p.drawLine(QPointF(14.5, 6), QPointF(8, 12));
    p.drawLine(QPointF(8, 12), QPointF(14.5, 18));
    p.restore();
}

void IconPainter::chevronRight(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    stroke(p, x, y, s, 2.2);
    p.setPen(glyphPen(resolve(c, w90()), 2.2 * 24 / s));
    p.drawLine(QPointF(9.5, 6), QPointF(16, 12));
    p.drawLine(QPointF(16, 12), QPointF(9.5, 18));
    p.restore();
}

void IconPainter::play(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    fill(p, x, y, s);
    p.setBrush(QBrush(resolve(c, w95())));
    p.drawPolygon(QPolygonF({QPointF(8, 5), QPointF(8, 19), QPointF(19.5, 12)}));
    p.restore();
}

void IconPainter::pause(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    fill(p, x, y, s);
    p.setBrush(QBrush(resolve(c, w95())));
    p.drawRoundedRect(QRectF(7, 5, 3.7, 14), 1.3, 1.3);
    p.drawRoundedRect(QRectF(13.3, 5, 3.7, 14), 1.3, 1.3);
    p.restore();
}

namespace {

/// Shared speaker cone, drawn in the already-transformed 24-grid space.
void drawSpeaker(QPainter& p, const QColor& c) {
    QPainterPath path;
    path.moveTo(4.5, 9.5);
    path.lineTo(7.5, 9.5);
    path.lineTo(11.5, 5.8);
    path.lineTo(11.5, 18.2);
    path.lineTo(7.5, 14.5);
    path.lineTo(4.5, 14.5);
    path.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(c));
    p.drawPath(path);
}

}  // namespace

void IconPainter::volume(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    const QColor color = resolve(c, w90());
    stroke(p, x, y, s);
    drawSpeaker(p, color);
    p.setBrush(Qt::NoBrush);
    p.setPen(glyphPen(color, 1.6 * 24 / s));
    p.drawArc(QRectF(13, 8, 4.5, 8), -52 * 16, 104 * 16);
    p.drawArc(QRectF(15.5, 5.5, 5.5, 13), -52 * 16, 104 * 16);
    p.restore();
}

void IconPainter::volumeLow(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    const QColor color = resolve(c, w90());
    stroke(p, x, y, s);
    drawSpeaker(p, color);
    p.setBrush(Qt::NoBrush);
    p.setPen(glyphPen(color, 1.6 * 24 / s));
    p.drawArc(QRectF(13, 8, 4.5, 8), -52 * 16, 104 * 16);
    p.restore();
}

void IconPainter::mute(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    const QColor color = resolve(c, w90());
    stroke(p, x, y, s);
    drawSpeaker(p, color);
    p.setBrush(Qt::NoBrush);
    p.setPen(glyphPen(color, 1.8 * 24 / s));
    p.drawLine(QPointF(14.5, 9.5), QPointF(19.5, 14.5));
    p.drawLine(QPointF(19.5, 9.5), QPointF(14.5, 14.5));
    p.restore();
}

void IconPainter::expand(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    stroke(p, x, y, s);
    p.setPen(glyphPen(resolve(c, w90()), 1.8 * 24 / s));
    p.drawLine(QPointF(14.5, 4.5), QPointF(19.5, 4.5));
    p.drawLine(QPointF(19.5, 4.5), QPointF(19.5, 9.5));
    p.drawLine(QPointF(19.5, 4.5), QPointF(13, 11));
    p.drawLine(QPointF(9.5, 19.5), QPointF(4.5, 19.5));
    p.drawLine(QPointF(4.5, 19.5), QPointF(4.5, 14.5));
    p.drawLine(QPointF(4.5, 19.5), QPointF(11, 13));
    p.restore();
}

void IconPainter::contract(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    stroke(p, x, y, s);
    p.setPen(glyphPen(resolve(c, w90()), 1.8 * 24 / s));
    p.drawLine(QPointF(19.5, 9.5), QPointF(14.5, 9.5));
    p.drawLine(QPointF(14.5, 9.5), QPointF(19.5, 4.5));
    p.drawLine(QPointF(4.5, 14.5), QPointF(9.5, 14.5));
    p.drawLine(QPointF(9.5, 14.5), QPointF(4.5, 19.5));
    p.restore();
}

void IconPainter::pip(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    stroke(p, x, y, s);
    p.setPen(glyphPen(resolve(c, w90()), 1.8 * 24 / s));
    p.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2);
    // The inset window is always the dimmer token: the outline takes the
    // caller's colour, the fill keeps the glyph's contrast against it.
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(w70()));
    p.drawRoundedRect(QRectF(12.5, 11.5, 6.5, 5.5), 1.2, 1.2);
    p.restore();
}

void IconPainter::gear(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    stroke(p, x, y, s);
    p.setPen(glyphPen(resolve(c, w90()), 1.6 * 24 / s));
    p.drawEllipse(QRectF(8.9, 8.9, 6.2, 6.2));
    for (int deg = 0; deg < 360; deg += 45) {
        const double rad = deg * M_PI / 180.0;
        p.drawLine(QPointF(12 + 4.6 * std::cos(rad), 12 + 4.6 * std::sin(rad)),
                   QPointF(12 + 6.6 * std::cos(rad), 12 + 6.6 * std::sin(rad)));
    }
    p.restore();
}

void IconPainter::folder(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    const QColor color = resolve(c, w70());
    stroke(p, x, y, s);
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(color));
    p.drawRoundedRect(QRectF(3.5, 5.5, 7.5, 3), 1, 1);
    p.drawRoundedRect(QRectF(3.5, 7.5, 17, 11.5), 1.6, 1.6);
    p.restore();
}

void IconPainter::download(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    const QColor color = resolve(c, w90());
    stroke(p, x, y, s);
    const QPen pen = glyphPen(color, 1.8 * 24 / s);
    p.setPen(pen);
    p.drawLine(QPointF(12, 4), QPointF(12, 12.5));
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(color));
    p.drawPolygon(QPolygonF({QPointF(7.8, 12), QPointF(16.2, 12), QPointF(12, 16.5)}));
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(4, 18, 16, 2.5), 1.25, 1.25);
    p.restore();
}

void IconPainter::more(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    fill(p, x, y, s);
    p.setBrush(QBrush(resolve(c, w90())));
    for (const double cy : {6.0, 12.0, 18.0}) {
        p.drawEllipse(QPointF(12, cy), 1.8, 1.8);
    }
    p.restore();
}

void IconPainter::reset(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    const QColor color = resolve(c, w90());
    stroke(p, x, y, s, 1.9);
    p.setPen(glyphPen(color, 1.9 * 24 / s));
    p.drawArc(QRectF(5, 5, 14, 14), 35 * 16, 290 * 16);
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(color));
    p.drawPolygon(QPolygonF({QPointF(17.6, 6.4), QPointF(17.6, 11.4), QPointF(13, 8.6)}));
    p.restore();
}

void IconPainter::spark(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    fill(p, x, y, s);
    p.setBrush(QBrush(resolve(c, w90())));
    p.drawPolygon(QPolygonF({QPointF(12, 3.5), QPointF(13.6, 10.4), QPointF(20.5, 12), QPointF(13.6, 13.6),
                             QPointF(12, 20.5), QPointF(10.4, 13.6), QPointF(3.5, 12), QPointF(10.4, 10.4)}));
    p.restore();
}

void IconPainter::microphone(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    const QColor color = resolve(c, w90());
    stroke(p, x, y, s);
    p.setPen(glyphPen(color, 1.8 * 24 / s));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(9.2, 3.2, 5.6, 10.4), 2.8, 2.8);
    // the cradle, stem and base: without them the capsule alone reads as a pill
    p.drawArc(QRectF(5.6, 8.4, 12.8, 11), 180 * 16, 180 * 16);
    p.drawLine(QPointF(12, 19.4), QPointF(12, 21.6));
    p.drawLine(QPointF(8.6, 21.6), QPointF(15.4, 21.6));
    p.restore();
}

void IconPainter::captions(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    const QColor color = resolve(c, w90());
    stroke(p, x, y, s);
    p.setPen(glyphPen(color, 1.7 * 24 / s));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(2.6, 5.6, 18.8, 12.8), 2.4, 2.4);
    // two C shapes, opening right, so it reads as closed captions rather than
    // as a generic box
    p.drawArc(QRectF(5.6, 8.6, 5.4, 6.8), 45 * 16, 270 * 16);
    p.drawArc(QRectF(12.4, 8.6, 5.4, 6.8), 45 * 16, 270 * 16);
    p.restore();
}

void IconPainter::keyboard(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    const QColor color = resolve(c, w90());
    stroke(p, x, y, s);
    p.setPen(glyphPen(color, 1.8 * 24 / s));
    p.drawRoundedRect(QRectF(2.5, 6.5, 19, 11), 2, 2);
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(color));
    for (const double cx : {6.5, 9.7, 12.9, 16.1}) {
        p.drawEllipse(QPointF(cx, 10), 0.75, 0.75);
    }
    p.drawRoundedRect(QRectF(8, 13.8, 8, 1.4), 0.7, 0.7);
    p.restore();
}

}  // namespace shell
}  // namespace mplayer
