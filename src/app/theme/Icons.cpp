#include "theme/Icons.hpp"

#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"

#include <QApplication>
#include <QColor>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPixmapCache>
#include <QtMath>
#include <QPolygonF>
#include <QRectF>
#include <QTransform>

#include <utility>

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

QPixmap renderSliders(int size, const QColor& color) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const double scale = size / 24.0;
    painter.scale(scale, scale);

    QPen pen(color);
    pen.setWidthF(1.8);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);

    // Three horizontal slider rails with staggered thumb gaps.
    painter.drawLine(QPointF(4.0, 7.0), QPointF(8.0, 7.0));
    painter.drawLine(QPointF(13.0, 7.0), QPointF(20.0, 7.0));

    painter.drawLine(QPointF(4.0, 12.0), QPointF(13.0, 12.0));
    painter.drawLine(QPointF(18.0, 12.0), QPointF(20.0, 12.0));

    painter.drawLine(QPointF(4.0, 17.0), QPointF(6.0, 17.0));
    painter.drawLine(QPointF(11.0, 17.0), QPointF(20.0, 17.0));

    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QPointF(10.5, 7.0), 2.1, 2.1);
    painter.drawEllipse(QPointF(15.5, 12.0), 2.1, 2.1);
    painter.drawEllipse(QPointF(8.5, 17.0), 2.1, 2.1);

    painter.end();
    return pixmap;
}

QPixmap renderPackage(int size, const QColor& color) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const double scale = size / 24.0;
    painter.scale(scale, scale);

    QPen pen(color);
    pen.setWidthF(1.7);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    QPolygonF hexagon;
    hexagon << QPointF(12.0, 3.2) << QPointF(19.8, 7.6) << QPointF(19.8, 16.4)
            << QPointF(12.0, 20.8) << QPointF(4.2, 16.4) << QPointF(4.2, 7.6);
    painter.drawPolygon(hexagon);

    painter.drawLine(QPointF(4.4, 7.8), QPointF(12.0, 12.0));
    painter.drawLine(QPointF(19.6, 7.8), QPointF(12.0, 12.0));
    painter.drawLine(QPointF(12.0, 12.0), QPointF(12.0, 20.6));
    painter.drawLine(QPointF(8.1, 5.4), QPointF(15.9, 9.8));

    painter.end();
    return pixmap;
}

QPixmap renderChevron(int size, const QColor& color, bool left) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const double scale = size / 24.0;
    painter.scale(scale, scale);

    QPen pen(color);
    pen.setWidthF(2.2);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    QPainterPath path;
    if (left) {
        path.moveTo(14.5, 6.5);
        path.lineTo(9.0, 12.0);
        path.lineTo(14.5, 17.5);
    } else {
        path.moveTo(9.5, 6.5);
        path.lineTo(15.0, 12.0);
        path.lineTo(9.5, 17.5);
    }
    painter.drawPath(path);
    painter.end();
    return pixmap;
}

enum class Glyph {
    Gear,
    Keyboard,
    Sliders,
    Package,
    ChevronLeft,
    ChevronRight
};

class GlyphIconEngine final : public QIconEngine {
public:
    GlyphIconEngine(Glyph glyph, QColor normal, QColor active, QColor disabled)
        : glyph_(glyph), normal_(std::move(normal)), active_(std::move(active)),
          disabled_(std::move(disabled)) {}

    QIconEngine* clone() const override { return new GlyphIconEngine(*this); }

    QString key() const override { return QStringLiteral("MikaiGlyph"); }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State state) override {
        if (painter == nullptr || !painter->isActive() || rect.isEmpty()) return;
        const qreal scale = painter->device() != nullptr
                                ? painter->device()->devicePixelRatioF()
                                : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, scale));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override {
        const QColor color = mode == QIcon::Disabled ? disabled_
                            : mode == QIcon::Active || mode == QIcon::Selected ? active_
                                                                              : normal_;
        const QString cacheKey = QStringLiteral("mikai-glyph:%1:%2x%3:%4:%5:%6:%7")
                                     .arg(static_cast<int>(glyph_))
                                     .arg(size.width())
                                     .arg(size.height())
                                     .arg(scale, 0, 'g', 8)
                                     .arg(static_cast<int>(mode))
                                     .arg(static_cast<int>(state))
                                     .arg(color.name(QColor::HexArgb));
        QPixmap pixmap;
        if (QPixmapCache::find(cacheKey, &pixmap)) return pixmap;

        const int logicalSize = qMax(1, qMin(size.width(), size.height()));
        const int pixelSize = qMax(1, qCeil(logicalSize * scale));
        switch (glyph_) {
            case Glyph::Gear: pixmap = renderGear(pixelSize, color); break;
            case Glyph::Keyboard: pixmap = renderKeyboard(pixelSize, color); break;
            case Glyph::Sliders: pixmap = renderSliders(pixelSize, color); break;
            case Glyph::Package: pixmap = renderPackage(pixelSize, color); break;
            case Glyph::ChevronLeft: pixmap = renderChevron(pixelSize, color, true); break;
            case Glyph::ChevronRight: pixmap = renderChevron(pixelSize, color, false); break;
        }
        pixmap.setDevicePixelRatio(scale);
        QPixmapCache::insert(cacheKey, pixmap);
        return pixmap;
    }

    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override {
        const int side = qMin(size.width(), size.height());
        return QSize(side, side);
    }

private:
    Glyph glyph_;
    QColor normal_;
    QColor active_;
    QColor disabled_;
};

QIcon glyphIcon(Glyph glyph, int size, const QColor& normal, const QColor& active,
                const QColor& disabled) {
    Q_UNUSED(size);
    return QIcon(new GlyphIconEngine(glyph, normal, active, disabled));
}

}  // namespace

QIcon gearIcon(int size, const QString& color) {
    const QColor fill(color.isEmpty() ? QApplication::palette().color(QPalette::Text) : QColor(color));
    return glyphIcon(Glyph::Gear, size, fill, fill, fill);
}

QIcon keyboardIcon(int size, const QString& color) {
    const QColor fill(color.isEmpty() ? QApplication::palette().color(QPalette::Text) : QColor(color));
    return glyphIcon(Glyph::Keyboard, size, fill, fill, fill);
}

QIcon slidersIcon(int size, const QString& color) {
    const QColor fill(color.isEmpty() ? QApplication::palette().color(QPalette::Text) : QColor(color));
    return glyphIcon(Glyph::Sliders, size, fill, fill, fill);
}

QIcon packageIcon(int size, const QString& color) {
    const QColor fill(color.isEmpty() ? QApplication::palette().color(QPalette::Text) : QColor(color));
    return glyphIcon(Glyph::Package, size, fill, fill, fill);
}

QIcon chevronLeftIcon(int size) {
    const QPalette palette = QApplication::palette();
    return glyphIcon(Glyph::ChevronLeft, size, palette.color(QPalette::Text),
                     palette.color(QPalette::Highlight),
                     palette.color(QPalette::Disabled, QPalette::Text));
}

QIcon chevronRightIcon(int size) {
    const QPalette palette = QApplication::palette();
    return glyphIcon(Glyph::ChevronRight, size, palette.color(QPalette::Text),
                     palette.color(QPalette::Highlight),
                     palette.color(QPalette::Disabled, QPalette::Text));
}

}  // namespace mikai::app::theme
