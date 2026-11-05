// Painted application glyphs.
//
// Every glyph below is drawn on a 24-unit grid and rendered once into a
// device-pixel raster by GlyphIconEngine, so the authoring rules are about the
// stroke a viewer finally sees:
//
//   * A stroke never falls below 1.0 *device* pixel at the smallest size the
//     glyph ships at (18 px for the header and settings chips, 12 px for the
//     menu chevron). On the 24-unit grid that is 24 / smallest-size grid units
//     - 1.33 units at 18 px - so a hairline like 1.4 units is the floor, and a
//     dot or cap needs a radius of at least half of it.
//   * Anything with a corner is stroked with round caps and round joins: at
//     these sizes a miter join grows longer than the stroke and reads as a tear.
//   * Fine detail (a sawtooth, a dot row) is reduced to shapes that survive the
//     smallest size rather than trusting antialiasing to keep it legible.
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
#include <QPointF>
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

/// The outline gear: a ring, eight capsule teeth and a hub, all stroked with
/// round caps. The 32-vertex sawtooth this replaced could not survive 18 px -
/// its teeth were two thirds of a pixel wide and the polygon's own joins spiked
/// past them, so the settings chip showed a star instead of a gear.
QPixmap renderOutlineGear(int size, const QColor& color) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(size / 24.0, size / 24.0);

    constexpr int kTeeth = 8;
    constexpr qreal kCentre = 12.0;
    constexpr qreal kToothWidth = 2.4;
    // The teeth start inside the ring so the two strokes merge into one shape.
    constexpr qreal kToothInner = 6.6;
    constexpr qreal kToothOuter = 10.4;

    QPen toothPen(color, kToothWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(toothPen);
    for (int index = 0; index < kTeeth; ++index) {
        const qreal angle = 2.0 * M_PI * index / kTeeth;
        const qreal dx = qCos(angle);
        const qreal dy = qSin(angle);
        painter.drawLine(QPointF(kCentre + kToothInner * dx, kCentre + kToothInner * dy),
                         QPointF(kCentre + kToothOuter * dx, kCentre + kToothOuter * dy));
    }

    QPen ringPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(ringPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QPointF(kCentre, kCentre), 6.8, 6.8);
    painter.drawEllipse(QPointF(kCentre, kCentre), 2.9, 2.9);
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

    // Three keys rather than four dots: a dot that reads at all at 18 px needs a
    // radius of 1.4 grid units (1.05 px), and four of those do not fit the
    // keycap's 19 units without merging into a bar.
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    for (const double cx : {8.0, 12.0, 16.0}) {
        painter.drawEllipse(QPointF(cx, 10.0), 1.4, 1.4);
    }
    painter.drawRoundedRect(QRectF(8.0, 13.6, 8.0, 1.6), 0.8, 0.8);

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

/// A stroked magnifier: the search field's leading glyph and the primary
/// action's icon read as the same tool.
QPixmap renderSearch(int size, const QColor& color) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const double scale = size / 24.0;
    painter.scale(scale, scale);

    QPen pen(color);
    pen.setWidthF(1.9);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    painter.drawEllipse(QPointF(10.4, 10.4), 6.2, 6.2);
    painter.drawLine(QPointF(15.0, 15.0), QPointF(19.6, 19.6));

    painter.end();
    return pixmap;
}

QPixmap renderHome(int size, const QColor& color) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const double scale = size / 24.0;
    painter.scale(scale, scale);

    QPen pen(color);
    pen.setWidthF(1.8);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    QPainterPath roof;
    roof.moveTo(3.6, 10.8);
    roof.lineTo(12.0, 3.8);
    roof.lineTo(20.4, 10.8);
    painter.drawPath(roof);

    QPainterPath walls;
    walls.moveTo(5.8, 9.4);
    walls.lineTo(5.8, 20.0);
    walls.lineTo(18.2, 20.0);
    walls.lineTo(18.2, 9.4);
    painter.drawPath(walls);

    // The door is a single stroke pair rather than a filled shape: at the
    // sidebar's smallest glyph box a filled door closes into a blob.
    QPainterPath door;
    door.moveTo(10.0, 20.0);
    door.lineTo(10.0, 14.2);
    door.lineTo(14.0, 14.2);
    door.lineTo(14.0, 20.0);
    painter.drawPath(door);

    painter.end();
    return pixmap;
}

/// The catalogue filters' funnel.
QPixmap renderFilter(int size, const QColor& color) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const double scale = size / 24.0;
    painter.scale(scale, scale);

    QPen pen(color);
    pen.setWidthF(1.8);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    QPainterPath funnel;
    funnel.moveTo(4.0, 5.2);
    funnel.lineTo(20.0, 5.2);
    funnel.lineTo(13.6, 12.6);
    funnel.lineTo(13.6, 18.8);
    funnel.lineTo(10.4, 20.4);
    funnel.lineTo(10.4, 12.6);
    funnel.closeSubpath();
    painter.drawPath(funnel);

    painter.end();
    return pixmap;
}

/// Two figures for the team browser: a full one in front, a smaller one behind.
QPixmap renderTeams(int size, const QColor& color) {
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

    painter.drawEllipse(QPointF(9.1, 8.6), 3.3, 3.3);
    QPainterPath front;
    front.moveTo(3.4, 19.6);
    front.cubicTo(3.4, 15.6, 6.0, 13.6, 9.1, 13.6);
    front.cubicTo(12.2, 13.6, 14.8, 15.6, 14.8, 19.6);
    painter.drawPath(front);

    painter.drawEllipse(QPointF(16.8, 9.7), 2.5, 2.5);
    QPainterPath back;
    back.moveTo(15.4, 19.6);
    back.cubicTo(15.6, 16.6, 17.7, 14.9, 20.4, 15.6);
    painter.drawPath(back);

    painter.end();
    return pixmap;
}

/// The play triangle on the Open video menu action.
QPixmap renderPlay(int size, const QColor& color) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const double scale = size / 24.0;
    painter.scale(scale, scale);

    QPen pen(color);
    pen.setWidthF(1.9);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    QPainterPath triangle;
    triangle.moveTo(4.0, 4.0);
    triangle.lineTo(20.0, 12.0);
    triangle.lineTo(4.0, 20.0);
    triangle.closeSubpath();
    painter.drawPath(triangle);

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

/// First/last page: the chevron keeps its shape and stroke, and the bar at the
/// end of the run sits far enough away that the gap survives the smallest
/// pager button (16 px, where a 24-unit grid unit is two thirds of a pixel).
QPixmap renderPageSkip(int size, const QColor& color, bool left) {
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

    const qreal barX = left ? 6.5 : 17.5;
    painter.drawLine(QPointF(barX, 6.5), QPointF(barX, 17.5));

    QPainterPath chevron;
    chevron.moveTo(left ? 17.5 : 6.5, 6.5);
    chevron.lineTo(12.0, 12.0);
    chevron.lineTo(left ? 17.5 : 6.5, 17.5);
    painter.drawPath(chevron);
    painter.end();
    return pixmap;
}

/// The dismissal cross. Two strokes of the chevron's weight crossing at the
/// centre: the text glyph it replaces is drawn by whichever face carries U+2715,
/// which is not the face the rest of the chrome uses.
QPixmap renderClose(int size, const QColor& color) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const double scale = size / 24.0;
    painter.scale(scale, scale);

    QPen pen(color);
    pen.setWidthF(2.2);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    painter.drawLine(QPointF(6.5, 6.5), QPointF(17.5, 17.5));
    painter.drawLine(QPointF(17.5, 6.5), QPointF(6.5, 17.5));

    painter.end();
    return pixmap;
}

enum class Glyph {
    Gear,
    OutlineGear,
    Keyboard,
    Sliders,
    Package,
    ChevronLeft,
    ChevronRight,
    SkipLeft,
    SkipRight,
    Close,
    Search,
    Home,
    Filter,
    Teams,
    Play
};

/// The icon box a request asks for, in logical pixels: the glyph is square and
/// takes the box height, the trailing pad is transparent columns on its trailing
/// side and never widens the glyph.
QSize glyphLogicalBox(const QSize& requested, int trailingGap) {
    const int side = qMax(1, qMin(requested.width() - trailingGap, requested.height()));
    return QSize(qMax(1, qMin(requested.width(), side + trailingGap)), side);
}

/// Rounds a logical position onto the device-pixel grid so a blit lands on whole
/// device pixels instead of being resampled across two of them.
QPointF snapToDeviceGrid(QPointF logical, const QPaintDevice* device) {
    const qreal dpr = device != nullptr ? device->devicePixelRatioF() : 1.0;
    if (!(dpr > 0.0)) return logical;
    return QPointF(qRound(logical.x() * dpr) / dpr, qRound(logical.y() * dpr) / dpr);
}

QPixmap renderGlyph(Glyph glyph, int side, const QColor& color) {
    switch (glyph) {
        case Glyph::Gear: return renderGear(side, color);
        case Glyph::OutlineGear: return renderOutlineGear(side, color);
        case Glyph::Keyboard: return renderKeyboard(side, color);
        case Glyph::Sliders: return renderSliders(side, color);
        case Glyph::Package: return renderPackage(side, color);
        case Glyph::ChevronLeft: return renderChevron(side, color, true);
        case Glyph::ChevronRight: return renderChevron(side, color, false);
        case Glyph::SkipLeft: return renderPageSkip(side, color, true);
        case Glyph::SkipRight: return renderPageSkip(side, color, false);
        case Glyph::Close: return renderClose(side, color);
        case Glyph::Search: return renderSearch(side, color);
        case Glyph::Home: return renderHome(side, color);
        case Glyph::Filter: return renderFilter(side, color);
        case Glyph::Teams: return renderTeams(side, color);
        case Glyph::Play: return renderPlay(side, color);
    }
    return {};
}

class GlyphIconEngine final : public QIconEngine {
public:
    GlyphIconEngine(Glyph glyph, QColor normal, QColor active, QColor disabled,
                    int trailingGap = 0)
        : glyph_(glyph), normal_(std::move(normal)), active_(std::move(active)),
          disabled_(std::move(disabled)), trailingGap_(qMax(0, trailingGap)) {}

    QIconEngine* clone() const override { return new GlyphIconEngine(*this); }

    QString key() const override { return QStringLiteral("MikaiGlyph"); }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State state) override {
        if (painter == nullptr || !painter->isActive() || rect.isEmpty()) return;
        const QPaintDevice* device = painter->device();
        const qreal scale = device != nullptr ? device->devicePixelRatioF() : 1.0;
        const QPixmap pixmap = scaledPixmap(rect.size(), mode, state, scale);
        if (pixmap.isNull()) return;
        const qreal ratio = pixmap.devicePixelRatio() > 0.0 ? pixmap.devicePixelRatio() : 1.0;
        const qreal height = qreal(pixmap.height()) / ratio;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        // The raster is tagged with the painter's own ratio, so drawing it at its
        // natural size is a straight device-pixel copy. Smoothing stays as the
        // safety net for a style that hands a rect actualSize() did not ask for.
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        // Drawn at the raster's own logical size: the glyph keeps its square
        // aspect and the blit stays one raster pixel per screen pixel. Filling
        // `rect` instead would stretch the box - which is not square, because it
        // carries the trailing pad - across both axes by different factors.
        const QPointF at(rect.x(), rect.y() + (rect.height() - height) / 2.0);
        painter->drawPixmap(snapToDeviceGrid(at, device), pixmap);
        painter->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        // A caller that does not name a ratio gets the screen's own: hardcoding
        // 1.0 here returns a 1x raster that a HiDPI label upscales.
        const qreal scale = qApp != nullptr ? qApp->devicePixelRatio() : 1.0;
        return scaledPixmap(size, mode, state, scale);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override {
        const QColor color = mode == QIcon::Disabled ? disabled_
                            : mode == QIcon::Active || mode == QIcon::Selected ? active_
                                                                              : normal_;
        const qreal ratio = scale > 0.0 ? scale : 1.0;
        const QSize logical = glyphLogicalBox(size, trailingGap_);
        // The whole box is rasterised at the screen's own ratio, in whole device
        // pixels. Tagging the raster with that same ratio is what makes the blit
        // in paint() one raster pixel per screen pixel: the glyph is drawn at the
        // screen's resolution instead of being resampled from another one.
        const QSize device(qMax(1, qRound(logical.width() * ratio)),
                           qMax(1, qRound(logical.height() * ratio)));
        const QString cacheKey = QStringLiteral("mikai-glyph:%1:%2x%3:%4:%5:%6:%7:%8")
                                     .arg(static_cast<int>(glyph_))
                                     .arg(device.width())
                                     .arg(device.height())
                                     .arg(ratio, 0, 'g', 8)
                                     .arg(static_cast<int>(mode))
                                     .arg(static_cast<int>(state))
                                     .arg(color.name(QColor::HexArgb))
                                     .arg(trailingGap_);
        QPixmap pixmap;
        if (QPixmapCache::find(cacheKey, &pixmap)) return pixmap;

        // The glyph is square in physical pixels - the box's device height - so
        // no ratio can squash it; the remaining device columns are the pad.
        pixmap = renderGlyph(glyph_, device.height(), color);
        if (pixmap.width() != device.width() || pixmap.height() != device.height()) {
            // A button that draws icon and label itself owns the spacing between
            // them; the style's own gap is not adjustable, so the glyph carries
            // the header's difference as transparent columns on its trailing side.
            QPixmap padded(device);
            padded.fill(Qt::transparent);
            QPainter painter(&padded);
            painter.drawPixmap(0, 0, pixmap);
            painter.end();
            pixmap = padded;
        }
        pixmap.setDevicePixelRatio(ratio);
        QPixmapCache::insert(cacheKey, pixmap);
        return pixmap;
    }

    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override {
        return glyphLogicalBox(size, trailingGap_);
    }

private:
    Glyph glyph_;
    QColor normal_;
    QColor active_;
    QColor disabled_;
    int trailingGap_ = 0;
};

QIcon glyphIcon(Glyph glyph, int size, const QColor& normal, const QColor& active,
                const QColor& disabled, int trailingGap = 0) {
    Q_UNUSED(size);
    return QIcon(new GlyphIconEngine(glyph, normal, active, disabled, trailingGap));
}

}  // namespace

QIcon gearIcon(int size, const QString& color) {
    const QColor fill(color.isEmpty() ? QApplication::palette().color(QPalette::Text) : QColor(color));
    return glyphIcon(Glyph::Gear, size, fill, fill, fill);
}

QIcon outlineGearIcon(int size, const QString& color, int trailingGap) {
    const QColor fill(color.isEmpty() ? QApplication::palette().color(QPalette::Text) : QColor(color));
    return glyphIcon(Glyph::OutlineGear, size, fill, fill, fill, trailingGap);
}

QIcon keyboardIcon(int size, const QString& color, int trailingGap) {
    const QColor fill(color.isEmpty() ? QApplication::palette().color(QPalette::Text) : QColor(color));
    return glyphIcon(Glyph::Keyboard, size, fill, fill, fill, trailingGap);
}

QIcon slidersIcon(int size, const QString& color) {
    const QColor fill(color.isEmpty() ? QApplication::palette().color(QPalette::Text) : QColor(color));
    return glyphIcon(Glyph::Sliders, size, fill, fill, fill);
}

QIcon packageIcon(int size, const QString& color) {
    const QColor fill(color.isEmpty() ? QApplication::palette().color(QPalette::Text) : QColor(color));
    return glyphIcon(Glyph::Package, size, fill, fill, fill);
}

QIcon searchIcon(int size, const QString& color, int trailingGap) {
    const QColor fill(color.isEmpty() ? QApplication::palette().color(QPalette::Text) : QColor(color));
    return glyphIcon(Glyph::Search, size, fill, fill, fill, trailingGap);
}

QIcon homeIcon(int size, const QString& color, int trailingGap) {
    const QColor fill(color.isEmpty() ? QApplication::palette().color(QPalette::Text) : QColor(color));
    return glyphIcon(Glyph::Home, size, fill, fill, fill, trailingGap);
}

QIcon filterIcon(int size, const QString& color, int trailingGap) {
    const QColor fill(color.isEmpty() ? QApplication::palette().color(QPalette::Text) : QColor(color));
    const QColor disabled = QApplication::palette().color(QPalette::Disabled, QPalette::Text);
    return glyphIcon(Glyph::Filter, size, fill, fill, disabled, trailingGap);
}

QIcon teamsIcon(int size, const QString& color, int trailingGap) {
    const QColor fill(color.isEmpty() ? QApplication::palette().color(QPalette::Text) : QColor(color));
    return glyphIcon(Glyph::Teams, size, fill, fill, fill, trailingGap);
}

QIcon playIcon(int size, const QString& color, int trailingGap) {
    const QColor fill(color.isEmpty() ? QApplication::palette().color(QPalette::Text) : QColor(color));
    return glyphIcon(Glyph::Play, size, fill, fill, fill, trailingGap);
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

QIcon firstPageIcon(int size) {
    const QPalette palette = QApplication::palette();
    return glyphIcon(Glyph::SkipLeft, size, palette.color(QPalette::Text),
                     palette.color(QPalette::Highlight),
                     palette.color(QPalette::Disabled, QPalette::Text));
}

QIcon lastPageIcon(int size) {
    const QPalette palette = QApplication::palette();
    return glyphIcon(Glyph::SkipRight, size, palette.color(QPalette::Text),
                     palette.color(QPalette::Highlight),
                     palette.color(QPalette::Disabled, QPalette::Text));
}

QIcon closeIcon(int size) {
    const QPalette palette = QApplication::palette();
    return glyphIcon(Glyph::Close, size, palette.color(QPalette::Text),
                     palette.color(QPalette::Highlight),
                     palette.color(QPalette::Disabled, QPalette::Text));
}

}  // namespace mikai::app::theme
