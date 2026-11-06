#include <utility>
#include "ShellWidgets.hpp"

#include "ShellTheme.hpp"

#include <QCoreApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEngine>
#include <QPointF>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "ShellSliders.hpp"
#include "gui/SystemDefaultPlayer.hpp"
#include "gui/UiProfile.hpp"
#include "../core/VideoSurface.hpp"

namespace mplayer {
namespace shell {
namespace {

/// Device pixels the chrome raster cache may hold, the shared fallback raster
/// included. Every control carries its own image, so this is a ceiling on the
/// overlay as a whole rather than on one frame's allocation: past it a control
/// is rasterised into the shared scratch a tile at a time instead of the cache
/// growing to the size of the window.
constexpr qint64 kChromeCacheBudgetBytes = 8 * 1024 * 1024;

/// `rgba(r,g,b,a)` for a colour, so a stylesheet fragment is built from the
/// same tokens the paint routines use.
QString rgba(const QColor& color) {
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

/// Python's `%g`: up to six significant digits with trailing zeros stripped.
QString formatG(double value) {
    return QString::number(value, 'g', 6);
}

struct SpeedPreset {
    double speed;
    const char* label;
};

// SPEED_PRESETS — order matters, the selected row is picked by proximity.
const SpeedPreset kSpeedPresets[] = {
    {0.5, QT_TRANSLATE_NOOP("mplayer::shell::SettingsFlyout", "Slow")},
    {1.0, QT_TRANSLATE_NOOP("mplayer::shell::SettingsFlyout", "Normal")},
    {1.2, QT_TRANSLATE_NOOP("mplayer::shell::SettingsFlyout", "Slightly faster")},
    {1.5, QT_TRANSLATE_NOOP("mplayer::shell::SettingsFlyout", "Fast")},
    {1.7, QT_TRANSLATE_NOOP("mplayer::shell::SettingsFlyout", "Very fast")},
    {2.0, QT_TRANSLATE_NOOP("mplayer::shell::SettingsFlyout", "Double speed")},
};

/// Label for the preset at `index`. Rows are appended in `kSpeedPresets` order,
/// so the index is the whole lookup — `refresh` runs on every slider-drag frame.
QString presetLabel(int index) {
    return QCoreApplication::translate("mplayer::shell::SettingsFlyout",
                                       kSpeedPresets[index].label);
}

// ── skip pill metrics ──────────────────────────────────────────────────────
// The pill is measured by its text, so a longer translation grows it instead of
// being clipped. The height is fixed: a stack of offers reads as one column.
constexpr int kSkipPillHeight = 34;
constexpr int kSkipPillPadding = 14;
constexpr int kSkipPillGlyph = 13;
constexpr int kSkipPillGlyphGap = 7;
constexpr int kSkipPillMinText = 64;

/// paintRounded() with a brush instead of a flat colour, so an IconButton's
/// standing fill can be the gradient the float's play disc is drawn with.
void paintRoundedBrush(QPainter& painter, const QRectF& rect, double radius, const QBrush& fill,
                       const QColor& border, double borderWidth) {
    enableChromeAntialiasing(painter);
    painter.setPen(border.isValid() ? QPen(border, borderWidth) : QPen(Qt::NoPen));
    painter.setBrush(fill);
    painter.drawRoundedRect(rect, radius, radius);
}

/// True for a widget that paints itself and covers what is under it: the
/// popovers. They are the chrome's opaque cards, so they are not adopted into
/// the surface painter and neither is anything they own.
bool selfPaintedChrome(const QWidget* widget) {
    return widget->property("playerOverlay").toBool();
}

/// Keeps a popover that re-measured itself inside its host without moving a
/// popover that still fits.
void clampInsideHost(QWidget* popover) {
    QWidget* host = popover->parentWidget();
    if (host == nullptr) {
        return;
    }
    const QRect bounds = host->rect().adjusted(4, 4, -4, -4);
    const QPoint at = popover->pos();
    popover->move(qBound(bounds.left(), at.x(), bounds.right() - popover->width() + 1),
                  qBound(bounds.top(), at.y(), bounds.bottom() - popover->height() + 1));
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  PAINTED CHROME
// ═══════════════════════════════════════════════════════════════════════════

void ChromePaint::paintOwnBackingStore(QWidget* widget) {
    if (surfacePainted_) {
        return;
    }
    QPainter painter(widget);
    painter.setRenderHint(QPainter::Antialiasing);
    paintChrome(painter);
}

ChromeHost* chromeHostOf(QWidget* widget) {
    for (QWidget* parent = (widget != nullptr) ? widget->parentWidget() : nullptr;
         parent != nullptr; parent = parent->parentWidget()) {
        if (auto* host = dynamic_cast<ChromeHost*>(parent)) {
            return host;
        }
    }
    return nullptr;
}

bool deviceAlignedTransform(const QTransform& toDevice, qreal ratio) {
    return toDevice.isInvertible() && qFuzzyCompare(toDevice.m11(), ratio) &&
           qFuzzyCompare(toDevice.m22(), ratio) && qFuzzyIsNull(toDevice.m12()) &&
           qFuzzyIsNull(toDevice.m21());
}

QPointF deviceAlignedOrigin(const QTransform& toDevice, const QPointF& logicalOrigin) {
    const QPointF mapped = toDevice.map(logicalOrigin);
    return toDevice.inverted().map(QPointF(qRound(mapped.x()), qRound(mapped.y())));
}

void chromeUpdate(QWidget* widget, const QRect& rect) {
    if (widget == nullptr) {
        return;
    }
    if (rect.isNull()) {
        widget->update();
    } else {
        widget->update(rect);
    }
    auto* painted = dynamic_cast<ChromePaint*>(widget);
    if (painted == nullptr || !painted->surfacePainted()) {
        return;
    }
    if (ChromeHost* host = chromeHostOf(widget)) {
        mikai::gui::uiprofile::noteChromeDirtyRequest();
        host->chromeDirty(widget, rect);
    }
}

void ChromeList::adopt(QWidget* root, QObject* watcher) {
    entries_.clear();
    cachedBytes_ = 0;
    // The fallback raster survives: it is shared storage, not a control's.
    mikai::gui::uiprofile::noteChromeCacheHeld(scratchBytes_);
    addSubtree(root, watcher);
}

void ChromeList::addSubtree(QWidget* root, QObject* watcher) {
    if (root == nullptr) {
        return;
    }
    // The reference, not a copy: adopting never creates or destroys a widget,
    // and this runs once per host rather than per frame.
    const QObjectList& children = root->children();
    for (QObject* object : children) {
        auto* child = qobject_cast<QWidget*>(object);
        if (child == nullptr || child->isWindow() || selfPaintedChrome(child)) {
            continue;
        }
        if (auto* painted = dynamic_cast<ChromePaint*>(child)) {
            // A widget already adopted keeps its attributes and its filter: a
            // second pass only records it again.
            if (!painted->surfacePainted()) {
                painted->setSurfacePainted(true);
                // Translucent, so the window still paints the part of this
                // widget that falls outside the picture; the two background
                // attributes go with it, or Qt fills a slab behind the chrome.
                child->setAttribute(Qt::WA_TranslucentBackground, true);
                child->setAttribute(Qt::WA_StyledBackground, false);
                child->setAutoFillBackground(false);
                if (watcher != nullptr) {
                    child->installEventFilter(watcher);
                }
            }
            entries_.append(Entry{child, painted, {}, {}, {}});
        }
        addSubtree(child, watcher);
    }
}

void chromeRepaintEvent(ChromeHost* host, QObject* watched, QEvent* event) {
    if (host == nullptr || event == nullptr) {
        return;
    }
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget == nullptr) {
        return;
    }
    // Only an adopted widget: a surface that asked for its own repaint is
    // already painting (routing that back would be the repaint loop this
    // mechanism exists to avoid), and a popover covers what is under it.
    auto* adopted = dynamic_cast<ChromePaint*>(widget);
    if (adopted == nullptr || !adopted->surfacePainted()) {
        return;
    }
    // Deliberately not QEvent::UpdateRequest: a control over a QOpenGLWidget is
    // repainted again whenever the picture under it is, and that arrives as a
    // paint, not a request - routing requests too would hang a repaint of the
    // picture off every extra invalidation, which is the paused-picture loop to
    // avoid. Every state change the chrome has routes itself through
    // chromeUpdate() instead.
    switch (event->type()) {
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::EnabledChange:
        case QEvent::FontChange:
        case QEvent::PaletteChange:
        case QEvent::StyleChange:
            host->chromeDirty(widget, QRect());
            break;
        case QEvent::Move:
            host->chromeMoved(widget);
            break;
        case QEvent::Resize:
            host->chromeDirty(widget, QRect());
            break;
        default:
            break;
    }
}

void ChromeList::dropCache(Entry& entry) {
    entry.valid = false;
    entry.stamp = 0;
    if (entry.cache.isNull()) {
        return;
    }
    cachedBytes_ -= qint64(entry.cache.width()) * qint64(entry.cache.height()) * 4;
    entry.cache = QPixmap();
    mikai::gui::uiprofile::noteChromeCacheHeld(cachedBytes_ + scratchBytes_);
}

void ChromeList::markStale(Entry& entry) {
    // The buffer stays: a control whose own state changed is a repaint, and the
    // next paint renders it into the pixmap this entry already owns.
    entry.valid = false;
}

QRect ChromeList::damage(const QWidget* widget) const {
    for (const Entry& entry : entries_) {
        if (entry.widget == widget) return entry.paintedBox;
    }
    return QRect();
}

QRect ChromeList::invalidate(const QWidget* widget) {
    QRect vacated;
    for (Entry& entry : entries_) {
        if (entry.widget != widget) continue;
        // The box is paint history, not cache validity: a control painted
        // without a cache (budget fallback) still leaves pixels behind when it
        // moves.
        vacated = entry.paintedBox;
        markStale(entry);
    }
    return vacated;
}

void ChromeList::invalidateAll() {
    // Every control keeps its buffer and its box; the new generation is what
    // says that none of those pixels may be reused.
    ++generation_;
}

int ChromeList::fallbackTileSide() const {
    // The largest square tile the remaining budget can hold, capped at 1024
    // device pixels (4 MiB): a control too big for the caches is the one place a
    // whole-control raster would be an oversized allocation. The floor is the
    // reserve cache admission leaves, so this always returns something.
    const qint64 available = kChromeCacheBudgetBytes - cachedBytes_;
    int side = 1024;
    while (side > 64 && qint64(side) * side * 4 > available) {
        side /= 2;
    }
    return side;
}

void ChromeList::ensureScratch(int side, qreal ratio) {
    if (!scratch_.isNull() && scratch_.width() >= side) {
        if (!qFuzzyCompare(scratch_.devicePixelRatio(), ratio)) {
            scratch_.setDevicePixelRatio(ratio);
        }
        return;
    }
    // Released before the replacement is taken, so the pair never holds more
    // than one tile's worth.
    scratch_ = QImage();
    scratchBytes_ = 0;
    scratch_ = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
    scratch_.setDevicePixelRatio(ratio);
    scratchBytes_ = qint64(side) * side * 4;
    mikai::gui::uiprofile::noteChromeCacheHeld(cachedBytes_ + scratchBytes_);
}

void ChromeList::paintFallback(QPainter& painter, Entry& entry, const QRect& box,
                               const QPoint& at, const QRect& clip) {
    // Phase comes from the control's box, never from this repaint's damage clip.
    const QRect visible = box.intersected(clip);
    if (visible.isEmpty()) {
        return;
    }
    const QTransform toDevice = painter.deviceTransform();
    const qreal ratio =
        painter.device() != nullptr ? painter.device()->devicePixelRatio() : qreal(1.0);
    const auto paintDirect = [&]() {
        painter.save();
        painter.translate(at);
        painter.setClipRect(visible.translated(-at), Qt::IntersectClip);
        entry.painted->paintChrome(painter);
        painter.restore();
    };
    if (!toDevice.isInvertible() || !(ratio > 0.0) ||
        !deviceAlignedTransform(toDevice, ratio)) {
        // No device grid to map a tile by; the shell's painters never produce
        // this, and a copy cannot be made exact.
        paintDirect();
        return;
    }
    const int side = fallbackTileSide();
    ensureScratch(side, ratio);
    if (scratch_.isNull()) {
        // A failed allocation leaves no raster to copy; the direct path is all
        // that is left (the buffer is the size of one tile, so this is a
        // severely constrained machine, not a layout case).
        paintDirect();
        return;
    }
    const QTransform fromDevice = toDevice.inverted();

    // Invariant, the phase the cached blit uses: a tile's device pixel is
    // round(toDevice(box.topLeft())) + ratio * (local + at - box.topLeft()).
    const QPointF deviceBox = toDevice.map(QPointF(box.topLeft()));
    const QPoint deviceOrigin(qRound(deviceBox.x()), qRound(deviceBox.y()));
    const QRectF visibleDevice(deviceOrigin.x() + ratio * (visible.x() - box.x()),
                               deviceOrigin.y() + ratio * (visible.y() - box.y()),
                               ratio * visible.width(), ratio * visible.height());
    const QRect cover = visibleDevice.toAlignedRect();

    for (int top = cover.top(); top <= cover.bottom(); top += side) {
        const int tileHeight = std::min(side, cover.bottom() - top + 1);
        for (int left = cover.left(); left <= cover.right(); left += side) {
            const int tileWidth = std::min(side, cover.right() - left + 1);
            const QPoint tileOffset(left, top);
            const QPointF tileOrigin = fromDevice.map(QPointF(tileOffset));

            {
                QPainter tile(&scratch_);
                tile.setRenderHint(QPainter::Antialiasing, true);
                tile.setRenderHint(QPainter::TextAntialiasing, true);
                tile.setRenderHint(QPainter::SmoothPixmapTransform, true);
                // Only the tile's own pixels are cleared; the rest of the shared
                // buffer is never copied out.
                tile.setCompositionMode(QPainter::CompositionMode_Source);
                tile.fillRect(QRectF(0.0, 0.0, tileWidth / ratio, tileHeight / ratio),
                              Qt::transparent);
                tile.setCompositionMode(QPainter::CompositionMode_SourceOver);
                tile.setWorldTransform(QTransform::fromTranslate(
                    (at.x() - box.left()) + qreal(deviceOrigin.x() - left) / ratio,
                    (at.y() - box.top()) + qreal(deviceOrigin.y() - top) / ratio));
                // The transform's input is the control's own (widget-local)
                // coordinate system; the visible box is given in the root's.
                tile.setClipRect(QRectF(visible.translated(-at)), Qt::IntersectClip);
                entry.painted->paintChrome(tile);
            }
            painter.drawImage(tileOrigin, scratch_, QRectF(0, 0, tileWidth, tileHeight));
        }
    }
}

void ChromeList::paint(QPainter& painter, QWidget* root, const QRect& clip) {
    if (root == nullptr) return;
    const mikai::gui::uiprofile::ScopedTimer timer;
    const qreal ratio =
        painter.device() != nullptr ? painter.device()->devicePixelRatio() : qreal(1.0);
    // The blit transform, read from the painter rather than assumed: the
    // surface half of the shell paints through a painter translated by the
    // surface's own offset, and a cache copied at the wrong phase is a blurred
    // copy at every fractional ratio. In the aligned case the cache's ratio is
    // exactly the painter's scale, so the copy needs no filter at all; the
    // filter is kept only for a transform that would scale the copy.
    const QTransform toDevice = painter.deviceTransform();
    const bool oneToOne = deviceAlignedTransform(toDevice, ratio);
    // The GL painter resolves QPainter::Antialiasing for paths only through the
    // framebuffer's multisample, and these surfaces are single-sample: a control
    // that cannot be cached is rasterised on the CPU there, never painted
    // straight into the picture.
    const bool glDevice = painter.paintEngine() != nullptr &&
                          painter.paintEngine()->type() == QPaintEngine::OpenGL2;
    quint64 painted = 0;
    quint64 cached = 0;
    const bool counted = mikai::gui::uiprofile::countersEnabled();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, !oneToOne);
    for (Entry& entry : entries_) {
        QWidget* widget = entry.widget;
        if (widget == nullptr || !widget->isVisible()) {
            dropCache(entry);
            continue;
        }
        const QPoint at = widget->mapTo(root, QPoint(0, 0));
        QRect box(at, widget->size());
        for (QWidget* parent = widget->parentWidget(); parent != nullptr && parent != root;
             parent = parent->parentWidget()) {
            box = box.intersected(QRect(parent->mapTo(root, QPoint(0, 0)), parent->size()));
            if (box.isEmpty()) break;
        }
        if (box.isEmpty() || !box.intersects(clip)) continue;

        const QRect local = box.translated(-at);
        const QSize deviceSize(static_cast<int>(std::ceil(box.width() * ratio)),
                               static_cast<int>(std::ceil(box.height() * ratio)));
        int missReason = 0;
        if (entry.valid) {
            if (entry.localBox != local || entry.cache.size() != deviceSize) missReason = 1;
            else if (entry.devicePixelRatio != ratio) missReason = 2;
            else if (entry.stamp != generation_) missReason = 3;
        }
        const bool reusable = entry.valid && entry.localBox == local &&
                              entry.cache.size() == deviceSize &&
                              entry.devicePixelRatio == ratio && entry.stamp == generation_;
        if (!reusable) {
            const qint64 bytes = qint64(deviceSize.width()) * deviceSize.height() * 4;
            const qint64 held = qint64(entry.cache.width()) * entry.cache.height() * 4;
            if (!deviceSize.isEmpty() &&
                cachedBytes_ - held + bytes + cacheReservation() <= kChromeCacheBudgetBytes) {
                if (entry.cache.isNull() || entry.cache.size() != deviceSize) {
                    dropCache(entry);
                    entry.cache = QPixmap(deviceSize);
                    cachedBytes_ += bytes;
                }
                entry.cache.setDevicePixelRatio(ratio);
                entry.cache.fill(Qt::transparent);
                {
                    QPainter cachePainter(&entry.cache);
                    cachePainter.setRenderHint(QPainter::Antialiasing, true);
                    cachePainter.setRenderHint(QPainter::TextAntialiasing, true);
                    cachePainter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                    cachePainter.translate(at - box.topLeft());
                    cachePainter.setClipRect(box.translated(-at), Qt::IntersectClip);
                    const qint64 rasterStart = counted ? mikai::gui::uiprofile::nowNs() : 0;
                    entry.painted->paintChrome(cachePainter);
                    if (counted) {
                        mikai::gui::uiprofile::noteChromeCacheRaster(
                            (mikai::gui::uiprofile::nowNs() - rasterStart) / 1000);
                    }
                }
                if (counted) mikai::gui::uiprofile::noteChromeCacheMiss(missReason);
                mikai::gui::uiprofile::noteChromeCacheRender();
                mikai::gui::uiprofile::noteChromeCacheHeld(cachedBytes_ + scratchBytes_);
                entry.localBox = local;
                entry.devicePixelRatio = ratio;
                entry.stamp = generation_;
                entry.valid = true;
            } else {
                if (counted) mikai::gui::uiprofile::noteChromeCacheMiss(4);
                dropCache(entry);
            }
        }
        if (entry.valid) {
            // The cache is one device pixel per texel; only its origin can be
            // fractional, so the origin is what gets snapped. Both halves of
            // the shell paint through their own painter, so the snap follows
            // that painter's transform instead of assuming the window's.
            const QPointF origin = oneToOne ? deviceAlignedOrigin(toDevice, QPointF(box.topLeft()))
                                            : QPointF(box.topLeft());
            const qint64 blitStart = counted ? mikai::gui::uiprofile::nowNs() : 0;
            painter.drawPixmap(origin, entry.cache);
            if (counted) {
                mikai::gui::uiprofile::noteChromeCacheBlit(
                    (mikai::gui::uiprofile::nowNs() - blitStart) / 1000);
            }
            if (reusable) ++cached;
        } else if (glDevice) {
            // Past the budget on the picture's own device: the same pixels, but
            // rasterised where an antialiasing hint means something.
            paintFallback(painter, entry, box, at, clip);
        } else {
            // Past the budget on a raster device the paint routines rasterise
            // their own antialiasing, so the control can draw straight in. The
            // origin is snapped like the cached blit, so dropping below the
            // budget does not move the control by half a device pixel.
            painter.save();
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            const QPointF origin =
                oneToOne ? deviceAlignedOrigin(toDevice, QPointF(at)) : QPointF(at);
            painter.translate(origin);
            painter.setClipRect(QRectF(box).translated(-origin), Qt::IntersectClip);
            entry.painted->paintChrome(painter);
            painter.restore();
        }
        entry.paintedBox = box;
        entry.localBox = local;
        ++painted;
    }
    painter.restore();
    if (timer.measured()) {
        mikai::gui::uiprofile::noteChromePaint(painted, cached, timer.micros());
        if (mikai::gui::uiprofile::enabled()) {
            mikai::gui::uiprofile::log(
                "chrome.list.paint",
                QStringLiteral("entries=%1 painted=%2 cached=%3 bytes=%4 elapsed_us=%5")
                    .arg(static_cast<qulonglong>(entries_.size()))
                    .arg(painted)
                    .arg(cached)
                    .arg(cachedBytes_ + scratchBytes_)
                    .arg(timer.micros()));
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  PANEL
// ═══════════════════════════════════════════════════════════════════════════

Panel::Panel(const QColor& fill, double radius, const QColor& border, QWidget* parent)
    : QWidget(parent), fill_(fill), radius_(radius), border_(border) {
    setAttribute(Qt::WA_TranslucentBackground);
}

void Panel::setFill(const QColor& color) {
    if (fill_ == color) {
        return;
    }
    fill_ = color;
    chromeUpdate(this);
}

void Panel::paintChrome(QPainter& painter) {
    enableChromeAntialiasing(painter);
    paintRounded(painter, QRectF(0.5, 0.5, width() - 1, height() - 1), radius_, fill_, border_);
}

void Panel::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

// ═══════════════════════════════════════════════════════════════════════════
//  ICON BUTTON
// ═══════════════════════════════════════════════════════════════════════════

IconButton::IconButton(IconFn icon, const QString& tooltip, int size, int glyph, QWidget* parent,
                       const QColor& hoverFill)
    : QPushButton(parent), icon_(icon), glyph_(glyph) {
    chrome_.hoverFill = hoverFill.isValid() ? QBrush(hoverFill) : QBrush(white(28));
    setFixedSize(size, size);
    setToolTip(tooltip);
    if (!tooltip.isEmpty()) {
        setAccessibleName(tooltip);
    }
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setFlat(true);
}

void IconButton::setTint(const QColor& color) {
    const bool valid = color.isValid();
    if (tint_ == color && hasTint_ == valid) {
        return;
    }
    tint_ = color;
    hasTint_ = valid;
    chromeUpdate(this);
}

void IconButton::setIcon(IconFn icon) {
    if (icon_ == icon) {
        return;
    }
    icon_ = icon;
    chromeUpdate(this);
}

void IconButton::setButtonChrome(const ButtonChrome& chrome) {
    chrome_ = chrome;
    chromeUpdate(this);
}

void IconButton::enterEvent(QEnterEvent* event) {
    // The look is a pure function of hover_, isChecked() and isEnabled(), none of
    // which this event changed if the button was already hovered. A repeated
    // entry - a pointer sweeping back over a control whose leave never arrived, a
    // re-enter after a popup - asked for pixels that are already on screen, and a
    // cache miss for them costs a full re-raster of the control.
    if (!hover_) {
        hover_ = true;
        chromeUpdate(this);
    }
    QPushButton::enterEvent(event);
}

void IconButton::leaveEvent(QEvent* event) {
    if (hover_) {
        hover_ = false;
        chromeUpdate(this);
    }
    QPushButton::leaveEvent(event);
}

void IconButton::paintChrome(QPainter& painter) {
    enableChromeAntialiasing(painter);
    // Inset by the stroke width, so a bordered button's outline lands inside
    // its own rect rather than being clipped by it.
    const QRectF box(0.5, 0.5, width() - 1.0, height() - 1.0);
    const bool lit = hover_ || isChecked();
    if (chrome_.fill.style() != Qt::NoBrush) {
        paintRoundedBrush(painter, box, chrome_.radius, chrome_.fill, chrome_.border,
                          chrome_.borderWidth);
    }
    if (lit && chrome_.hoverFill.style() != Qt::NoBrush) {
        paintRoundedBrush(painter, box, chrome_.radius, chrome_.hoverFill, chrome_.hoverBorder,
                          chrome_.borderWidth);
    }
    QColor color = hasTint_ ? tint_ : w90();
    if (!isEnabled()) color.setAlpha(color.alpha() / 3);
    // Integer positioning for crisp rendering - align to pixel grid
    const int off = (width() - glyph_) / 2;
    if (icon_) {
        icon_(painter, off, off, glyph_, color);
    }
}

void IconButton::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

// ═══════════════════════════════════════════════════════════════════════════
//  SKIP BUTTON
// ═══════════════════════════════════════════════════════════════════════════

SkipButton::SkipButton(QWidget* parent) : QPushButton(parent) {
    // Text is painted by paintChrome, never by QPushButton's own style, so the
    // control is flat and carries no stylesheet of its own.
    setFlat(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setFont(uiFont(9, true));
    setFixedHeight(kSkipPillHeight);
}

void SkipButton::setLabel(const QString& label) {
    if (label_ == label) {
        return;
    }
    label_ = label;
    // The base class keeps the string too, so a screen reader that reads the
    // button's text and one that reads its accessible name agree.
    QPushButton::setText(label);
    resize(sizeHint());
    chromeUpdate(this);
}

QSize SkipButton::sizeHint() const {
    const QFontMetrics metrics(font());
    const int text = std::max(kSkipPillMinText, metrics.horizontalAdvance(label_));
    return {text + 2 * kSkipPillPadding + kSkipPillGlyphGap + kSkipPillGlyph, kSkipPillHeight};
}

void SkipButton::enterEvent(QEnterEvent* event) {
    // Same rule as IconButton: only a real hover change is a repaint. The pill,
    // its text colour and its chevron all read hover_ and nothing else that this
    // event touched.
    if (!hover_) {
        hover_ = true;
        chromeUpdate(this);
    }
    QPushButton::enterEvent(event);
}

void SkipButton::leaveEvent(QEvent* event) {
    if (hover_) {
        hover_ = false;
        chromeUpdate(this);
    }
    QPushButton::leaveEvent(event);
}

void SkipButton::paintChrome(QPainter& painter) {
    enableChromeAntialiasing(painter);
    // Inset by the stroke, so the outline lands inside the button's own rect.
    const QRectF box(0.5, 0.5, width() - 1.0, height() - 1.0);
    const double radius = std::min(cornerRadius() + 6.0, height() / 2.0);
    paintRounded(painter, box, radius, hover_ ? QColor(48, 51, 58, 240) : pill(),
                 hover_ ? w25() : w15());

    const int glyphX = width() - kSkipPillPadding - kSkipPillGlyph;
    const int textWidth = std::max(0, glyphX - kSkipPillGlyphGap - kSkipPillPadding);
    painter.setFont(font());
    painter.setPen(isEnabled() ? w95() : w45());
    painter.drawText(QRect(kSkipPillPadding, 0, textWidth, height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(font()).elidedText(label_, Qt::ElideRight, textWidth));
    // Integer placement, so the chevron stays crisp at any scale factor.
    IconPainter::chevronRight(painter, glyphX, (height() - kSkipPillGlyph) / 2, kSkipPillGlyph,
                              hover_ ? accent() : w70());
}

void SkipButton::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

// ═══════════════════════════════════════════════════════════════════════════
//  SETTINGS FLYOUT
// ═══════════════════════════════════════════════════════════════════════════

/// The shortcut card's keycaps: an accent wash with a visible edge, so a hint
/// reads as part of the same colour system as the rest of the chrome.
QString keyHintsQss() {
    return QStringLiteral(
               "KeyHints { background: transparent; }"
               "QLabel#playerKeyHintKey {"
               "  color: %1;"
               "  background: rgba(%2, 0.18);"
               "  border: 1px solid rgba(%2, 0.42);"
               "  border-radius: 4px;"
               "  padding: 2px 6px;"
               "  font-weight: 600;"
               "}")
        .arg(rgba(white(235)), rgbaTriplet(accent()));
}

/// The speed badge: the pushed accent behind a contrast-derived ink, so a very
/// dark accent still carries a legible label.
QString badgeQss() {
    return QStringLiteral("background:%1; color:%2; border-radius:4px; padding:1px 6px;"
                          " font-weight:700;")
        .arg(accent().name(QColor::HexRgb), inkOn(accent()).name(QColor::HexRgb));
}

SettingsFlyout::SettingsFlyout(QWidget* parent)
    : QFrame(parent), quality_(QStringLiteral("HD")) {
    setObjectName(QStringLiteral("playerSettingsFlyout"));
    setProperty("playerOverlay", true);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QStringLiteral("SettingsFlyout { background: transparent; }"));
    hide();

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(5);

    auto* head = new QHBoxLayout();
    speedCaption_ = new QLabel(tr("SPEED"));
    speedCaption_->setFont(monoFont(8));
    speedCaption_->setStyleSheet(QStringLiteral("color: #9a9aa6; font-weight: 700; background: transparent;"));
    head->addWidget(speedCaption_);
    head->addStretch();
    badge_ = new QLabel(QStringLiteral("1.0x"));
    badge_->setFont(monoFont(9));
    badge_->setStyleSheet(badgeQss());
    head->addWidget(badge_);
    lay->addLayout(head);

    slider_ = new RangeSlider(accent(), 4, this);
    slider_->setObjectName(QStringLiteral("playerSpeedSlider"));
    slider_->setAccessibleName(tr("Speed"));
    connect(slider_, &RangeSlider::valueChanged, this, &SettingsFlyout::onSlider);
    lay->addWidget(slider_);

    auto* sep = new QFrame(this);
    sep->setFixedHeight(1);
    sep->setStyleSheet(QStringLiteral("background: rgba(255, 255, 255, 0.10);"));
    lay->addWidget(sep);

    for (const SpeedPreset& preset : kSpeedPresets) {
        auto* btn = new QPushButton(this);
        btn->setFont(uiFont(10));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(26);
        btn->setFocusPolicy(Qt::NoFocus);
        const double speed = preset.speed;
        connect(btn, &QPushButton::clicked, this, [this, speed] { pick(speed); });
        lay->addWidget(btn);
        rows_.append(std::make_pair(speed, btn));
    }

    auto* sep2 = new QFrame(this);
    sep2->setFixedHeight(1);
    sep2->setStyleSheet(QStringLiteral("background: rgba(255, 255, 255, 0.10);"));
    lay->addWidget(sep2);

    // Quality lives here rather than in a chip over the picture: the gear is
    // where a viewer looks for stream settings, and the strip is for things
    // that act on the current episode.
    qualitySection_ = new QWidget(this);
    qualitySection_->setObjectName(QStringLiteral("playerQualitySection"));
    qualitySection_->setAccessibleName(tr("Quality"));
    qualitySection_->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* qlay = new QVBoxLayout(qualitySection_);
    qlay->setContentsMargins(0, 0, 0, 0);
    qlay->setSpacing(5);

    auto* qrow = new QHBoxLayout();
    qualityCaption_ = new QLabel(tr("QUALITY"));
    qualityCaption_->setObjectName(QStringLiteral("playerQualityCaption"));
    qualityCaption_->setFont(monoFont(8));
    qualityCaption_->setStyleSheet(QStringLiteral("color: #9a9aa6; font-weight: 700; background: transparent;"));
    qrow->addWidget(qualityCaption_);
    qrow->addStretch();
    qualityLabel_ = new QLabel(QStringLiteral("HD"));
    qualityLabel_->setObjectName(QStringLiteral("playerQualityLabel"));
    qualityLabel_->setFont(monoFont(9));
    qualityLabel_->setStyleSheet(
        QStringLiteral("color:%1; font-weight:700;").arg(green().name()));
    qrow->addWidget(qualityLabel_);
    qlay->addLayout(qrow);

    qualityBody_ = new QVBoxLayout();
    qualityBody_->setContentsMargins(0, 0, 0, 0);
    qualityBody_->setSpacing(2);
    qlay->addLayout(qualityBody_);
    lay->addWidget(qualitySection_);
    qualitySection_->setVisible(false);

    // The display mode belongs next to quality: both are "how the picture is
    // shown", and a viewer reaching for one is already looking at this card.
    // Original 1:1 stays a Settings-only choice - a quarter-screen picture is
    // not a quick switch, and Fit is the way back from every row here.
    scaleSection_ = new QWidget(this);
    scaleSection_->setObjectName(QStringLiteral("playerScaleSection"));
    scaleSection_->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* slay = new QVBoxLayout(scaleSection_);
    slay->setContentsMargins(0, 0, 0, 0);
    slay->setSpacing(2);
    scaleCaption_ = new QLabel(tr("SCALING"));
    scaleCaption_->setObjectName(QStringLiteral("playerScaleCaption"));
    scaleCaption_->setFont(monoFont(8));
    scaleCaption_->setStyleSheet(
        QStringLiteral("color: #9a9aa6; font-weight: 700; background: transparent;"));
    slay->addWidget(scaleCaption_);
    const mplayer::VideoScaleMode kQuickModes[] = {
        mplayer::VideoScaleMode::Fit, mplayer::VideoScaleMode::Fill,
        mplayer::VideoScaleMode::Stretch, mplayer::VideoScaleMode::Aspect16x9,
        mplayer::VideoScaleMode::Aspect4x3};
    for (const mplayer::VideoScaleMode mode : kQuickModes) {
        auto* btn = new QPushButton(this);
        btn->setFont(uiFont(10));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(26);
        btn->setFocusPolicy(Qt::NoFocus);
        const int value = static_cast<int>(mode);
        // The row carries the mode it selects, so the order of this list is not
        // also the meaning of the value the refresh loop reads back.
        btn->setProperty("videoScaleMode", value);
        connect(btn, &QPushButton::clicked, this, [this, value] {
            close();
            emit videoScaleModeSelected(value);
        });
        slay->addWidget(btn);
        scaleRows_.append(btn);
    }
    lay->addWidget(scaleSection_);

    auto* defaultSeparator = new QFrame(this);
    defaultSeparator->setFixedHeight(1);
    defaultSeparator->setStyleSheet(QStringLiteral("background: %1;").arg(rgba(w10())));
    lay->addWidget(defaultSeparator);

    defaultPlayerButton_ = new QPushButton(this);
    defaultPlayerButton_->setObjectName(QStringLiteral("playerDefaultVideoPlayerButton"));
    defaultPlayerButton_->setFont(uiFont(10));
    defaultPlayerButton_->setFocusPolicy(Qt::StrongFocus);
    defaultPlayerButton_->setCursor(Qt::PointingHandCursor);
    defaultPlayerButton_->setEnabled(mikai::gui::systemDefaultPlayerSupported());
    // A wrapping label keeps the full action readable in narrow windows and
    // longer translations. The button owns focus and the accessible name.
    auto* defaultLayout = new QVBoxLayout(defaultPlayerButton_);
    defaultLayout->setContentsMargins(10, 6, 10, 6);
    defaultPlayerLabel_ = new QLabel(defaultPlayerButton_);
    defaultPlayerLabel_->setFont(uiFont(10));
    defaultPlayerLabel_->setWordWrap(true);
    defaultPlayerLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    defaultPlayerLabel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    defaultLayout->addWidget(defaultPlayerLabel_);
    lay->addWidget(defaultPlayerButton_);
    connect(defaultPlayerButton_, &QPushButton::clicked, this, [this] {
        close();
        emit defaultPlayerRequested();
    });
    // The badge, the speed track and the selected rows carry the accent; a live
    // change restates them instead of waiting for the flyout to be rebuilt.
    connect(&accentNotifier(), &AccentNotifier::changed, this, [this] {
        badge_->setStyleSheet(badgeQss());
        slider_->setAccent(accent());
        refresh();
    });

    retranslate();
}

QString rowQss(bool current) {
    const QString accentRgb = rgbaTriplet(accent());
    return QStringLiteral(
               "QPushButton { color: %1; background: %2; border: 1px solid %3; border-radius: 6px;"
               " text-align: left; padding: 0 10px; font-size: 13px; font-weight: 500; min-height: 26px; }"
               "QPushButton:hover { background: rgba(255, 255, 255, 0.12); color: #ffffff;"
               " border-color: rgba(255, 255, 255, 0.20); }")
        .arg(current ? QStringLiteral("#ffffff") : QStringLiteral("#e0e0e6"),
             current ? QStringLiteral("rgba(%1, 0.40)").arg(accentRgb)
                     : QStringLiteral("rgba(255, 255, 255, 0.05)"),
             current ? QStringLiteral("rgba(%1, 0.60)").arg(accentRgb)
                     : QStringLiteral("transparent"));
}

void SettingsFlyout::setRate(double rate) {
    rate_ = qBound(0.25, rate, 2.0);
    refresh();
}

void SettingsFlyout::setQuality(const QString& quality) {
    quality_ = quality;
    refresh();
}

void SettingsFlyout::setQualityOptions(const QVector<QualityOption>& options) {
    qualityOptions_ = options;
    while (qualityBody_->count() > 0) {
        QLayoutItem* item = qualityBody_->takeAt(0);
        if (QWidget* widget = item->widget()) {
            // takeAt already detached it from the layout; reparenting to null
            // on top of that makes a visible row its own top-level window.
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    qualityRows_.clear();

    for (int i = 0; i < qualityOptions_.size(); ++i) {
        const int height = qualityOptions_[i].height;
        auto* btn = new QPushButton(this);
        btn->setFont(uiFont(10));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(26);
        btn->setFocusPolicy(Qt::NoFocus);
        connect(btn, &QPushButton::clicked, this, [this, height] {
            close();
            emit qualitySelected(height);
        });
        qualityBody_->addWidget(btn);
        qualityRows_.append(btn);
    }
    qualitySection_->setVisible(!qualityOptions_.isEmpty());
    refresh();
}

void SettingsFlyout::setVideoScaleMode(int mode) {
    videoScaleMode_ = mode;
    refresh();
}

void SettingsFlyout::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
}

void SettingsFlyout::retranslate() {
    speedCaption_->setText(tr("SPEED"));
    qualityCaption_->setText(tr("QUALITY"));
    scaleCaption_->setText(tr("SCALING"));
    // Spoken names never reach the painter, so only this handler refreshes them.
    slider_->setAccessibleName(tr("Speed"));
    qualitySection_->setAccessibleName(tr("Quality"));
    scaleSection_->setAccessibleName(tr("Video scaling"));
    const QString defaultText = tr("Set as default video player…");
    defaultPlayerLabel_->setText(defaultText);
    defaultPlayerButton_->setAccessibleName(defaultText);
#ifdef Q_OS_LINUX
    const QString defaultDescription = tr("Registers Mikai Ja Nai and opens desktop settings so you can choose it under Default Applications. Your defaults are not changed automatically.");
#else
    const QString defaultDescription = mikai::gui::systemDefaultPlayerSupported()
        ? tr("Opens Windows Default Apps so you can choose Mikai Ja Nai. Your defaults are not changed automatically.")
        : tr("Setting the default video player is only supported on Windows.");
#endif
    defaultPlayerButton_->setToolTip(defaultDescription);
    defaultPlayerButton_->setAccessibleDescription(defaultDescription);
    defaultPlayerButton_->setStyleSheet(rowQss(false)
        + QStringLiteral("QPushButton:focus { border-color: %1; }").arg(rgba(w70())));
    defaultPlayerLabel_->setStyleSheet(
        QStringLiteral("background: transparent; color: %1;")
            .arg(rgba(defaultPlayerButton_->isEnabled() ? w90() : w45())));
    // refresh() rewrites the translated preset rows from rate_ and the
    // caller-held quality options: neither selection moves.
    refresh();
    if (!isVisible()) {
        // popupNear() measures the new wording when it opens.
        return;
    }
    // popupNear() froze the size for the previous wording, and a longer caption
    // would be clipped by it. The anchor is gone by now, so the corner stays.
    resizeToContents();
    clampInsideHost(this);
    update();
}

void SettingsFlyout::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // 100% solid, fully opaque dark card background with crisp border
    p.setPen(QPen(QColor(255, 255, 255, 45), 1));
    p.setBrush(QColor(20, 21, 27));
    p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 10.0, 10.0);
}

void SettingsFlyout::resizeToContents() {
    // Drop the previous fixed bounds before measuring translated/wrapped text.
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    layout()->invalidate();
    layout()->activate();
    int targetWidth = layout()->sizeHint().width();
    if (parentWidget() != nullptr) {
        targetWidth = qMin(targetWidth, qMax(1, parentWidget()->width() - 8));
    }
    const int wrappedHeight = layout()->totalHeightForWidth(targetWidth);
    setFixedSize(targetWidth, wrappedHeight >= 0 ? wrappedHeight : layout()->sizeHint().height());
    layout()->activate();
}

void SettingsFlyout::popupNear(const QPoint& anchorGlobal) {
    show();
    refresh();
    ensurePolished();
    resizeToContents();
    QWidget* host = parentWidget();
    if (host == nullptr) {
        return;
    }

    const QPoint anchor = host->mapFromGlobal(anchorGlobal);
    const QRect bounds = host->rect().adjusted(4, 4, -4, -4);
    const int x = qBound(bounds.left(), anchor.x() + 14 - width(),
                         bounds.right() - width() + 1);
    const int y = qBound(bounds.top(), anchor.y() - height() - 10,
                         bounds.bottom() - height() + 1);
    move(x, y);
    raise();
    setFocus(Qt::OtherFocusReason);
}

void SettingsFlyout::keyPressEvent(QKeyEvent* event) {
    // The overlay owns focus while open, so Esc lands here before the player.
    if (event->key() == Qt::Key_Escape) {
        close();
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

void SettingsFlyout::pick(double speed) {
    rate_ = speed;
    refresh();
    emit speedSelected(speed);
    close();
}

void SettingsFlyout::onSlider(double fraction) {
    rate_ = 0.25 + fraction * 1.75;
    refresh();
    emit speedSelected(std::round(rate_ * 100.0) / 100.0);
}

void SettingsFlyout::refresh() {
    // refresh() runs on every slider tick, so a drag produces dozens of calls
    // in which only the badge and the slider move. Writing a QLabel/QPushButton
    // re-lays it out and invalidates it, and setStyleSheet re-parses the sheet
    // and re-polishes the button - so each row is written only when its text
    // or its selected state actually differs from what is on screen.
    const QString badgeText = formatG(rate_) + QStringLiteral("x");
    if (badge_->text() != badgeText) {
        badge_->setText(badgeText);
    }
    if (qualityLabel_->text() != quality_) {
        qualityLabel_->setText(quality_);
    }
    slider_->setValueSilent((rate_ - 0.25) / 1.75);

    for (int i = 0; i < qualityRows_.size() && i < qualityOptions_.size(); ++i) {
        const QualityOption& option = qualityOptions_[i];
        QPushButton* btn = qualityRows_[i];
        const QString text = (option.current ? QStringLiteral("✓  ") : QString()) + option.label;
        if (btn->text() != text) {
            btn->setText(text);
        }
        const QString sheet = rowQss(option.current);
        if (btn->styleSheet() != sheet) {
            btn->setStyleSheet(sheet);
        }
    }
    for (int i = 0; i < scaleRows_.size(); ++i) {
        QPushButton* btn = scaleRows_[i];
        // A mode outside this list (Original 1:1, chosen in Settings) simply
        // ticks nothing here.
        const int mode = btn->property("videoScaleMode").toInt();
        const bool selected = videoScaleMode_ == mode;
        const QString text = (selected ? QStringLiteral("✓  ") : QString())
            + mplayer::videoScaleModeLabel(static_cast<mplayer::VideoScaleMode>(mode));
        if (btn->text() != text) {
            btn->setText(text);
        }
        const QString sheet = rowQss(selected);
        if (btn->styleSheet() != sheet) {
            btn->setStyleSheet(sheet);
        }
    }
    for (int i = 0; i < rows_.size(); ++i) {
        const double speed = rows_[i].first;
        QPushButton* btn = rows_[i].second;
        const bool selected = std::abs(rate_ - speed) < 0.026;
        const QString text = (selected ? QStringLiteral("✓  ") : QString())
            + QStringLiteral("%1x   %2").arg(formatG(speed), presetLabel(i));
        if (btn->text() != text) {
            btn->setText(text);
        }
        const QString sheet = rowQss(selected);
        if (btn->styleSheet() != sheet) {
            btn->setStyleSheet(sheet);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  CHROME LABEL
// ═══════════════════════════════════════════════════════════════════════════

ChromeLabel::ChromeLabel(const QString& text, const QFont& font, const QColor& color,
                         QWidget* parent)
    : QLabel(text, parent), color_(color) {
    setFont(font);
    setAttribute(Qt::WA_StyledBackground, false);
}

void ChromeLabel::setText(const QString& text) {
    if (text == QLabel::text()) {
        return;
    }
    QLabel::setText(text);
    chromeUpdate(this);
}

void ChromeLabel::setChromeColor(const QColor& color) {
    if (color_ == color) {
        return;
    }
    color_ = color;
    chromeUpdate(this);
}

void ChromeLabel::paintChrome(QPainter& painter) {
    enableChromeAntialiasing(painter);
    painter.setFont(font());
    painter.setPen(color_);
    painter.drawText(rect(), alignment(), text());
}

void ChromeLabel::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

// ═══════════════════════════════════════════════════════════════════════════
//  CENTER GLYPH
// ═══════════════════════════════════════════════════════════════════════════

namespace {
/// The disc the glyph is painted into, and the halo margin the widget reserves
/// around it. One constant for both, because a disc larger than its widget is
/// clipped at the edges - which is what left only part of the circle showing.
constexpr qreal kCenterDisc = 78.0;
constexpr qreal kCenterHalo = 5.0;
constexpr qreal kCenterWidget = kCenterDisc + 2.0 * kCenterHalo;
}  // namespace

CenterGlyph::CenterGlyph(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFixedSize(static_cast<int>(kCenterWidget), static_cast<int>(kCenterWidget));
    spinnerTimer_.setInterval(40);
    connect(&spinnerTimer_, &QTimer::timeout, this, [this] {
        spinnerAngle_ = std::fmod(spinnerAngle_ + 15.0, 360.0);
        chromeUpdate(this);
    });
    hide();
}

void CenterGlyph::setKind(Kind kind) {
    kind_ = kind;
    if (loading_) {
        show();
        chromeUpdate(this);
        return;
    }
    if (kind == Kind::None) {
        hide();
        return;
    }
    show();
    chromeUpdate(this);
}

void CenterGlyph::setLoading(bool loading) {
    if (loading_ == loading) {
        return;
    }
    loading_ = loading;
    if (loading_) {
        setAccessibleName(tr("Loading comparison"));
        setAccessibleDescription(tr("A comparison is being prepared."));
        spinnerAngle_ = 0.0;
        spinnerTimer_.start();
        show();
    } else {
        spinnerTimer_.stop();
        setAccessibleName(QString());
        setAccessibleDescription(QString());
        setKind(kind_);
    }
    chromeUpdate(this);
}

void CenterGlyph::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

void CenterGlyph::paintChrome(QPainter& painter) {
    enableChromeAntialiasing(painter);
    if (!loading_ && kind_ == Kind::None) {
        return;
    }
    const QRectF disc(kCenterHalo + 0.5, kCenterHalo + 0.5, kCenterDisc - 1.0, kCenterDisc - 1.0);
    // A soft outer halo, then the disc, then a hairline rim. One flat fill read
    // as a sticker pasted on the picture; the halo is what makes it sit above
    // a white frame without a hard black edge.
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 55));
    painter.drawEllipse(disc.adjusted(-kCenterHalo + 2.0, -kCenterHalo + 2.0, kCenterHalo - 2.0,
                                      kCenterHalo - 2.0));
    painter.setBrush(QColor(0, 0, 0, 145));
    painter.drawEllipse(disc);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 255, 255, 38), 1.2));
    painter.drawEllipse(disc);
    // A top-lit gradient inside the disc: lighter where the light would fall.
    QLinearGradient sheen(disc.topLeft(), disc.bottomLeft());
    sheen.setColorAt(0.0, QColor(255, 255, 255, 26));
    sheen.setColorAt(0.55, QColor(255, 255, 255, 0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(sheen);
    painter.drawEllipse(disc);

    if (loading_) {
        const QPointF center(width() / 2.0, height() / 2.0);
        painter.setRenderHint(QPainter::Antialiasing);
        QPen track(QColor(255, 255, 255, 70), 4.0, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(track);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, 17.0, 17.0);
        QPen arc(w95(), 4.0, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(arc);
        painter.drawArc(QRectF(center.x() - 17.0, center.y() - 17.0, 34.0, 34.0),
                        static_cast<int>(spinnerAngle_ * 16.0), 245 * 16);
        return;
    }

    // Centred on the glyph's own bounds, not on the 24-grid cell: the triangle
    // spans 8..19.5 and the bars 7..17, so both sit a little right of centre in
    // the cell and need a per-shape correction to land optically centred.
    constexpr qreal kGlyph = 26.0;
    const qreal scale = kGlyph / 24.0;
    const qreal off = kCenterHalo + (kCenterDisc - kGlyph) / 2.0;
    if (kind_ == Kind::Play) {
        IconPainter::play(painter, off - 1.75 * scale, off, kGlyph, w95());
    } else {
        IconPainter::pause(painter, off, off, kGlyph, w95());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  TOAST
// ═══════════════════════════════════════════════════════════════════════════

Toast::Toast(QWidget* parent) : Panel(QColor(0, 0, 0, 205), 8.0, w15(), parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);

    label_ = new ChromeLabel(QString(), uiFont(10), white(235), this);
    label_->setAlignment(Qt::AlignCenter);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(14, 8, 14, 8);
    lay->addWidget(label_);
    hide();

    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &QWidget::hide);
}

void Toast::flash(const QString& text, int ms) {
    label_->setText(text);
    adjustSize();
    if (QWidget* parent = parentWidget()) {
        move((parent->width() - width()) / 2, static_cast<int>(parent->height() * 0.80));
    }
    show();
    timer_->start(ms);
}

// ═══════════════════════════════════════════════════════════════════════════
//  KEY HINTS
// ═══════════════════════════════════════════════════════════════════════════

QVector<std::pair<QString, QString>> KeyHints::shortcutTable() {
    return {
        {QStringLiteral("Space"), tr("Pause / play")},
        {QStringLiteral("F"), tr("Fullscreen / windowed")},
        {QStringLiteral("M"), tr("Mute / unmute audio")},
        {QStringLiteral("S"), tr("Episode list")},
        {QStringLiteral("T"), tr("Choose audio track")},
        {QStringLiteral("C"), tr("Choose subtitle track")},
        {QStringLiteral("Q"), tr("Quality and speed")},
        {QStringLiteral("←  →"), tr("Seek ∓5s")},
        {QStringLiteral("↑  ↓"), tr("Volume ±10%")},
        {QStringLiteral("Esc"), tr("Close panel / window")},
    };
}

KeyHints::KeyHints(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("playerKeyHints"));
    setProperty("playerOverlay", true);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(keyHintsQss());
    // The keycaps are an accent wash (the ghost role's "keyboard hints"), so a
    // live accent change restates them from one place.
    connect(&accentNotifier(), &AccentNotifier::changed, this,
            [this] { setStyleSheet(keyHintsQss()); });
    hide();

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(4);
    caption_ = new QLabel(tr("KEYBOARD SHORTCUTS"));
    caption_->setFont(monoFont(8));
    caption_->setStyleSheet(QStringLiteral("color: #9a9aa6; font-weight: 700; background: transparent; letter-spacing: 0.5px;"));
    lay->addWidget(caption_);
    lay->addSpacing(2);

    const QVector<std::pair<QString, QString>> table = shortcutTable();
    descriptions_.reserve(table.size());
    for (const auto& entry : table) {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);
        auto* key = new QLabel(entry.first);
        key->setFont(monoFont(8));
        key->setObjectName(QStringLiteral("playerKeyHintKey"));
        key->setAlignment(Qt::AlignCenter);
        key->setMinimumWidth(68);
        auto* action = new QLabel(entry.second);
        action->setFont(uiFont(9));
        action->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(rgba(white(180))));
        row->addWidget(key);
        descriptions_.append(action);
        row->addWidget(action, 1);
        lay->addLayout(row);
    }
}

void KeyHints::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
}

void KeyHints::retranslate() {
    caption_->setText(tr("KEYBOARD SHORTCUTS"));
    const auto table = shortcutTable();
    const qsizetype rows = qMin(descriptions_.size(), table.size());
    for (qsizetype row = 0; row < rows; ++row) {
        descriptions_.at(row)->setText(table.at(row).second);
    }
    if (!isVisible()) {
        // popupNear() measures the new wording when it opens.
        return;
    }
    // The card was sized for the previous wording; a longer row must not be
    // clipped by that frozen width.
    layout()->invalidate();
    layout()->activate();
    setFixedSize(layout()->sizeHint());
    layout()->activate();
    clampInsideHost(this);
    update();
}

void KeyHints::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 45), 1));
    p.setBrush(QColor(20, 21, 27));
    p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 10.0, 10.0);
}

void KeyHints::popupNear(const QPoint& anchorGlobal) {
    show();
    ensurePolished();
    layout()->invalidate();
    layout()->activate();
    setFixedSize(layout()->sizeHint());
    layout()->activate();
    QWidget* host = parentWidget();
    if (host == nullptr) {
        return;
    }

    const QPoint anchor = host->mapFromGlobal(anchorGlobal);
    const QRect bounds = host->rect().adjusted(4, 4, -4, -4);
    const int x = qBound(bounds.left(), anchor.x() + 14 - width(),
                         bounds.right() - width() + 1);
    const int y = qBound(bounds.top(), anchor.y() + 8,
                         bounds.bottom() - height() + 1);
    move(x, y);
    raise();
    setFocus(Qt::OtherFocusReason);
}

void KeyHints::keyPressEvent(QKeyEvent* event) {
    // Same reason as SettingsFlyout: the focused overlay owns Esc.
    if (event->key() == Qt::Key_Escape) {
        close();
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

}  // namespace shell
}  // namespace mplayer
