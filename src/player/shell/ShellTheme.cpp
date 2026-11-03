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
/// The shipped application accent (#bdacff). The application pushes its own
/// token before any window exists; this default is what a probe or a bare
/// surface sees, and it must be the app's colour rather than a library's story.
QColor& accentStorage() {
    static QColor current(QStringLiteral("#bdacff"));
    return current;
}
double& cornerRadiusStorage() {
    static double current = 8.0;
    return current;
}

/// Amber, the semantic "side B" hue, and the complement a warm accent rotates
/// it to.
constexpr int kCompareAmberHue = 37;
constexpr int kCompareRotatedHue = 210;
constexpr int kCompareHueWindow = 30;
}  // namespace

QColor accent() { return accentStorage(); }

void setAccent(const QColor& color) {
    if (!color.isValid() || color == accentStorage()) return;
    const QColor previous = accentStorage();
    accentStorage() = color;
    Q_EMIT accentNotifier().changed(previous);
}

AccentNotifier& accentNotifier() {
    static AccentNotifier notifier;
    return notifier;
}

QColor compareSideB(const QColor& accentColor) {
    const int hue = accentColor.hue();
    const int distance = qAbs(((hue - kCompareAmberHue) % 360 + 540) % 360 - 180);
    if (distance > kCompareHueWindow) return QColor(245, 158, 11);  // #f59e0b
    // Amber would read as the accent at a glance, so side B moves to the
    // complement and keeps its saturation and value from the accent.
    float h = 0.0f;
    float s = 0.0f;
    float v = 0.0f;
    accentColor.getHsvF(&h, &s, &v);
    Q_UNUSED(h);
    return QColor::fromHsvF(kCompareRotatedHue / 360.0, qBound(0.45f, s, 0.9f),
                            qBound(0.55f, v, 0.95f));
}

QColor compareSide(int index) {
    return index == 0 ? accent() : compareSideB(accent());
}

QString rgbaTriplet(const QColor& color) {
    return QStringLiteral("%1, %2, %3").arg(color.red()).arg(color.green()).arg(color.blue());
}

QColor inkOn(const QColor& background) {
    const auto linear = [](int value) {
        const double channel = value / 255.0;
        return channel <= 0.04045 ? channel / 12.92
                                  : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    const double luminance = 0.2126 * linear(background.red()) +
                             0.7152 * linear(background.green()) +
                             0.0722 * linear(background.blue());
    const double onBlack = (luminance + 0.05) / 0.05;
    const double onWhite = 1.05 / (luminance + 0.05);
    return onBlack >= onWhite ? QColor(Qt::black) : QColor(Qt::white);
}

double cornerRadius() { return cornerRadiusStorage(); }

void setCornerRadius(double radius) {
    if (radius >= 0.0) cornerRadiusStorage() = radius;
}

namespace {

/// Glyphs take an optional override colour; an invalid QColor means "use the
/// token this glyph was designed with", so callers can pass W_90() blindly.
QColor resolve(const QColor& c, const QColor& token) { return c.isValid() ? c : token; }

/// Pen for a glyph: width is expressed in 24-grid units, and the painter is
/// already scaled by size/24, so dividing by `k` keeps the stroke optically
/// even at any requested pixel size. Joins are round like the caps: a miter at
/// a 90-degree vertex reaches past the stroke and reads as a spike at 16-24 px.
QPen glyphPen(const QColor& color, double width) {
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
    p.setPen(glyphPen(w90(), width / k));
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
    p.setPen(glyphPen(resolve(c, w90()), 2.25 * 24 / s));
    p.drawPolyline(QPolygonF({QPointF(15, 3), QPointF(21, 3), QPointF(21, 9)}));
    p.drawPolyline(QPolygonF({QPointF(9, 21), QPointF(3, 21), QPointF(3, 15)}));
    p.drawLine(QPointF(21, 3), QPointF(14, 10));
    p.drawLine(QPointF(3, 21), QPointF(10, 14));
    p.restore();
}

void IconPainter::contract(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    stroke(p, x, y, s);
    p.setPen(glyphPen(resolve(c, w90()), 2.25 * 24 / s));
    p.drawPolyline(QPolygonF({QPointF(4, 14), QPointF(10, 14), QPointF(10, 20)}));
    p.drawPolyline(QPolygonF({QPointF(20, 10), QPointF(14, 10), QPointF(14, 4)}));
    p.drawLine(QPointF(14, 10), QPointF(21, 3));
    p.drawLine(QPointF(3, 21), QPointF(10, 14));
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

void IconPainter::hdr(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    // HDR badge: rounded rect with "HDR" text
    const QColor color = resolve(c, w90());
    fill(p, x, y, s);
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(color));
    p.drawRoundedRect(QRectF(2, 4, 20, 16), 3, 3);
    p.setPen(glyphPen(resolve(c, QColor(0, 0, 0)), 1.4 * 24 / s));
    QFont font = p.font();
    font.setBold(true);
    font.setPixelSize(static_cast<int>(10 * s / 24));
    p.setFont(font);
    p.drawText(QRectF(2, 4, 20, 16), Qt::AlignCenter, QStringLiteral("HDR"));
    p.restore();
}

void IconPainter::dolbyVision(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    // Dolby Vision badge: "DV" in a rounded rect
    const QColor color = resolve(c, QColor(124, 58, 237));  // purple
    fill(p, x, y, s);
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(color));
    p.drawRoundedRect(QRectF(2, 4, 20, 16), 3, 3);
    p.setPen(glyphPen(white(255), 1.4 * 24 / s));
    QFont font = p.font();
    font.setBold(true);
    font.setPixelSize(static_cast<int>(11 * s / 24));
    p.setFont(font);
    p.drawText(QRectF(2, 4, 20, 16), Qt::AlignCenter, QStringLiteral("DV"));
    p.restore();
}

void IconPainter::dolbyAtmos(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    // Dolby Atmos badge: "A" in a circle. The circle is inset inside the 24-unit
    // box the way the HDR and DV plates are: a badge whose antialiased fringe
    // lands on the box edge is painted into the clip and loses that fringe.
    const QColor color = resolve(c, QColor(0, 156, 224));  // cyan-blue
    fill(p, x, y, s);
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(color));
    p.drawEllipse(QRectF(3, 4, 16, 16));
    p.setPen(glyphPen(white(255), 1.6 * 24 / s));
    QFont font = p.font();
    font.setBold(true);
    font.setPixelSize(static_cast<int>(12 * s / 24));
    p.setFont(font);
    p.drawText(QRectF(3, 4, 16, 16), Qt::AlignCenter, QStringLiteral("A"));
    p.restore();
}

void IconPainter::codecH264(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    // H.264 badge: "H.264" text
    const QColor color = resolve(c, w90());
    stroke(p, x, y, s);
    p.setPen(glyphPen(color, 1.4 * 24 / s));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(2, 5, 20, 14), 2.5, 2.5);
    QFont font = p.font();
    font.setPixelSize(static_cast<int>(8 * s / 24));
    p.setFont(font);
    p.setPen(QPen(color, 1.2 * 24 / s));
    p.drawText(QRectF(2, 5, 20, 14), Qt::AlignCenter, QStringLiteral("H.264"));
    p.restore();
}

void IconPainter::codecH265(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    // H.265/HEVC badge: "HEVC" text
    const QColor color = resolve(c, w90());
    stroke(p, x, y, s);
    p.setPen(glyphPen(color, 1.4 * 24 / s));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(2, 5, 20, 14), 2.5, 2.5);
    QFont font = p.font();
    font.setPixelSize(static_cast<int>(8 * s / 24));
    p.setFont(font);
    p.setPen(QPen(color, 1.2 * 24 / s));
    p.drawText(QRectF(2, 5, 20, 14), Qt::AlignCenter, QStringLiteral("HEVC"));
    p.restore();
}

void IconPainter::codecAv1(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    // AV1 badge: "AV1" text
    const QColor color = resolve(c, w90());
    stroke(p, x, y, s);
    p.setPen(glyphPen(color, 1.4 * 24 / s));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(2, 5, 20, 14), 2.5, 2.5);
    QFont font = p.font();
    font.setPixelSize(static_cast<int>(9 * s / 24));
    p.setFont(font);
    p.setPen(QPen(color, 1.2 * 24 / s));
    p.drawText(QRectF(2, 5, 20, 14), Qt::AlignCenter, QStringLiteral("AV1"));
    p.restore();
}

void IconPainter::codecVp9(QPainter& p, qreal x, qreal y, qreal s, const QColor& c) {
    // VP9 badge: "VP9" text
    const QColor color = resolve(c, w90());
    stroke(p, x, y, s);
    p.setPen(glyphPen(color, 1.4 * 24 / s));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(2, 5, 20, 14), 2.5, 2.5);
    QFont font = p.font();
    font.setPixelSize(static_cast<int>(9 * s / 24));
    p.setFont(font);
    p.setPen(QPen(color, 1.2 * 24 / s));
    p.drawText(QRectF(2, 5, 20, 14), Qt::AlignCenter, QStringLiteral("VP9"));
    p.restore();
}

}  // namespace shell
}  // namespace mplayer
