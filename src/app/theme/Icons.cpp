#include "theme/Icons.hpp"

#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QRectF>
#include <QTransform>

#include <cmath>
#include <map>

namespace mikai::app::theme {
namespace {

QPixmap renderGear(int size, const QColor& color) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);

    // A gear is a toothed ring: the ring is one path, the teeth are rounded
    // rectangles rotated around it, and the hub is punched out with an even-odd
    // fill so the hole is a real hole rather than a background-coloured patch.
    constexpr int kTeeth = 8;
    const double centre = size / 2.0;
    const double outer = size * 0.46;
    const double ringInner = size * 0.30;
    const double hubRadius = size * 0.16;
    const double toothLength = outer - ringInner + size * 0.06;
    const double toothWidth = size * 0.16;

    for (int index = 0; index < kTeeth; ++index) {
        const double angle = 2.0 * M_PI * index / kTeeth;
        QTransform transform;
        transform.translate(centre, centre);
        transform.rotate(angle / M_PI * 180.0);
        painter.save();
        painter.setTransform(transform, true);
        painter.drawRoundedRect(
            QRectF(-toothWidth / 2.0, -outer, toothWidth, toothLength), toothWidth / 3.0,
            toothWidth / 3.0);
        painter.restore();
    }

    QPainterPath ring;
    ring.addEllipse(QPointF(centre, centre), outer, outer);
    ring.addEllipse(QPointF(centre, centre), ringInner, ringInner);
    ring.setFillRule(Qt::OddEvenFill);
    painter.drawPath(ring);

    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.setBrush(Qt::black);
    painter.drawEllipse(QPointF(centre, centre), hubRadius, hubRadius);
    painter.end();

    return pixmap;
}

QPixmap renderKeyboard(int size, const QColor& color) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const double scale = size / 24.0;
    painter.scale(scale, scale);

    QPen pen(color);
    pen.setWidthF(1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(2.5, 6.5, 19.0, 11.0), 2.0, 2.0);

    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    for (const double cx : {6.5, 9.7, 12.9, 16.1}) {
        painter.drawEllipse(QPointF(cx, 10.0), 0.85, 0.85);
    }
    painter.drawRoundedRect(QRectF(8.0, 13.8, 8.0, 1.4), 0.7, 0.7);

    painter.end();
    return pixmap;
}

}  // namespace

QIcon gearIcon(int size, const QString& color) {
    const int pixels = size > 0 ? size : kIconSize;
    const QColor fill(color.isEmpty() ? QString::fromLatin1(kText) : color);
    // The transport re-picks its glyphs on every state change elsewhere in the
    // app, so rasterising the same gear repeatedly is pure waste; one pixmap
    // serves every widget, and QIcon is value-shared.
    static const std::map<QString, QIcon> kCache = [] {
        std::map<QString, QIcon> cache;
        for (const char* hex : {kText, kAccent, kMuted}) {
            const QIcon icon(renderGear(kIconSize * 2, QColor(QString::fromLatin1(hex))));
            cache.emplace(QString::fromLatin1(hex), icon);
        }
        return cache;
    }();
    const auto it = kCache.find(fill.name());
    if (it != kCache.end()) {
        return it->second;
    }
    return QIcon(renderGear(pixels * 2, fill));
}

QIcon keyboardIcon(int size, const QString& color) {
    const int pixels = size > 0 ? size : kIconSize;
    const QColor fill(color.isEmpty() ? QString::fromLatin1(kText) : color);
    static const std::map<QString, QIcon> kCache = [] {
        std::map<QString, QIcon> cache;
        for (const char* hex : {kText, kAccent, kMuted}) {
            const QIcon icon(renderKeyboard(kIconSize * 2, QColor(QString::fromLatin1(hex))));
            cache.emplace(QString::fromLatin1(hex), icon);
        }
        return cache;
    }();
    const auto it = kCache.find(fill.name());
    if (it != kCache.end()) {
        return it->second;
    }
    return QIcon(renderKeyboard(pixels * 2, fill));
}

}  // namespace mikai::app::theme
