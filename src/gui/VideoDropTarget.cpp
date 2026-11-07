#include "VideoDropTarget.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDropEvent>
#include <QKeyEvent>
#include <QFontMetrics>
#include <QMimeData>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QStringView>
#include <QTransform>
#include <QUrl>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace mikai::gui {
namespace {

constexpr int kMargin = 24;
constexpr int kPadding = 24;
constexpr int kGap = 12;
constexpr int kSmallGap = 4;
constexpr int kIconSize = 48;
constexpr int kCardWidth = 520;
constexpr int kRadius = 12;
// How far the dashed rounded perimeter reaches past its own geometry: half the
// 2 px pen plus one pixel of coverage, and the same coverage outside the corner
// arcs' radius.
constexpr int kStrokeReach = 3;
constexpr int kCornerReach = kRadius + 3;
// The largest side a layer may have, in device pixels. Qt's OpenGL paint engine
// hands a blitted pixmap to a texture cache that refuses anything past
// GL_MAX_TEXTURE_SIZE, and the smallest limit a desktop context may report is
// 2048 - the bound every tile is kept inside so the ring survives the surface's
// paint sink on any GPU.
constexpr int kMaxLayerExtent = 2048;

/// True when `toDevice` maps logical units to device pixels at exactly `ratio`,
/// without rotation, shear or a second scale: the only case where a layer
/// tagged with `ratio` lands one texel per device pixel, so the blit may turn
/// its filter off instead of softening an exact copy. This is the rule the
/// shell blits its cached chrome by (ShellWidgets.cpp, deviceAlignedTransform);
/// it is restated here because gui/ is the lower layer of the two and must not
/// include the player's shell headers.
bool deviceAlignedTransform(const QTransform& toDevice, qreal ratio) {
    return toDevice.isInvertible() && qFuzzyCompare(toDevice.m11(), ratio) &&
           qFuzzyCompare(toDevice.m22(), ratio) && qFuzzyIsNull(toDevice.m12()) &&
           qFuzzyIsNull(toDevice.m21());
}

/// `logicalOrigin` snapped so its device position is a whole pixel. A fractional
/// ratio (1.25, 1.5, 1.75) puts a logical origin between device pixels, and a
/// layer copied at that phase is resampled even at one texel per pixel; the snap
/// moves it by at most half a device pixel and keeps the copy exact.
QPointF deviceAlignedOrigin(const QTransform& toDevice, const QPointF& logicalOrigin) {
    const QPointF mapped = toDevice.map(logicalOrigin);
    return toDevice.inverted().map(QPointF(qRound(mapped.x()), qRound(mapped.y())));
}

/// One element of the indicator as a device-resolution alpha layer. `body` draws
/// in the indicator's logical coordinates; the layer receives the pixels inside
/// `deviceBox`, expressed in device pixels so that two neighbouring layers
/// always meet on a pixel boundary. A seam in the middle of a pixel would draw
/// that pixel's coverage twice, or drop it.
template <typename Body>
QPixmap renderLayer(const QRect& deviceBox, qreal ratio, Body&& body) {
    QPixmap layer(deviceBox.size());
    layer.fill(Qt::transparent);
    QPainter painter(&layer);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    // One explicit transform rather than scale() then translate(), whose
    // composition order is easy to invert: this is exactly device = ratio *
    // logical - layerOrigin, so the coverage lands on the same device pixel grid
    // it would have been drawn on directly.
    painter.setTransform(QTransform(ratio, 0.0, 0.0, ratio,
                                    -qreal(deviceBox.left()), -qreal(deviceBox.top())),
                         true);
    body(painter);
    return layer;
}

/// Blit one layer back at the size and alignment it was rendered for. The layer
/// already carries its own coverage, so the filter is only there for a transform
/// that would actually scale the copy.
void blitLayer(QPainter& painter, const QPixmap& layer, const QPointF& logicalOrigin,
               qreal ratio, bool oneToOne, const QTransform& toDevice) {
    if (layer.isNull()) return;
    const QPointF origin =
        oneToOne ? deviceAlignedOrigin(toDevice, logicalOrigin) : logicalOrigin;
    const QRectF target(origin, QSizeF(layer.width() / ratio, layer.height() / ratio));
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, !oneToOne);
    painter.drawPixmap(target, layer, QRectF(layer.rect()));
    painter.restore();
}

class DropOverlay final : public QWidget {
public:
    DropOverlay(QWidget* host, VideoDropTarget* target) : QWidget(host), target_(target) {
        setObjectName(QStringLiteral("videoDropOverlay"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
        setFocusPolicy(Qt::NoFocus);
        hide();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (target_.isNull()) return;
        QPainter painter(this);
        target_->paintIndicator(painter, rect());
    }

private:
    QPointer<VideoDropTarget> target_;
};

} // namespace

VideoDropTarget::VideoDropTarget(QWidget* host, OverlayMode mode) : QObject(host), host_(host) {
    host->setAcceptDrops(true);
    host->window()->setAcceptDrops(true);
    if (mode == OverlayMode::Widget) overlay_ = new DropOverlay(host, this);
    leaveTimer_.setSingleShot(true);
    leaveTimer_.setInterval(0);
    connect(&leaveTimer_, &QTimer::timeout, this, [this] {
        if (leaveGeneration_ == dragGeneration_) clearIndicator();
    });
    qApp->installEventFilter(this);
}

VideoDropTarget::~VideoDropTarget() {
    if (qApp != nullptr) qApp->removeEventFilter(this);
    leaveTimer_.stop();
    delete overlay_.data();
}

QString VideoDropTarget::classify(const QMimeData* mime, bool allowTracks, DropKind* kind) {
    if (kind != nullptr) *kind = DropKind::None;
    if (mime == nullptr || !mime->hasUrls()) return {};
    static constexpr QStringView videoSuffixes[]{
        u"mp4", u"mkv", u"webm", u"avi", u"mov", u"m4v",
        u"ts", u"flv", u"m2ts", u"mpeg", u"mpg"};
    static constexpr QStringView subtitleSuffixes[]{
        u"srt", u"ass", u"ssa", u"vtt", u"sub", u"sup"};
    static constexpr QStringView audioSuffixes[]{
        u"m4a", u"aac", u"mp3", u"opus", u"flac", u"wav", u"ac3", u"eac3", u"dts", u"ogg"};
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        const QStringView view(path);
        const qsizetype separator = std::max(view.lastIndexOf(u'/'), view.lastIndexOf(u'\\'));
        const qsizetype dot = view.lastIndexOf(u'.');
        if (dot <= separator || dot + 1 == view.size()) continue;
        const QStringView suffix = view.sliced(dot + 1);
        const auto matches = [&suffix](const auto& table) {
            for (const QStringView supported : table) {
                if (suffix.compare(supported, Qt::CaseInsensitive) == 0) return true;
            }
            return false;
        };
        if (matches(videoSuffixes)) {
            if (kind != nullptr) *kind = DropKind::Video;
            return path;
        }
        if (!allowTracks) continue;
        if (matches(subtitleSuffixes)) {
            if (kind != nullptr) *kind = DropKind::Subtitle;
            return path;
        }
        if (matches(audioSuffixes)) {
            if (kind != nullptr) *kind = DropKind::Audio;
            return path;
        }
    }
    return {};
}

QString VideoDropTarget::videoPath(const QMimeData* mime) {
    DropKind kind = DropKind::None;
    const QString path = classify(mime, false, &kind);
    return kind == DropKind::Video ? path : QString();
}

void VideoDropTarget::refreshAppearance() {
    if (host_.isNull()) return;
    switch (indicatorKind_) {
        case DropKind::Video:
            title_ = QCoreApplication::translate("VideoDropTarget", "Release to open video");
            hint_ = QCoreApplication::translate(
                "VideoDropTarget", "Drop the file anywhere in this window");
            break;
        case DropKind::Subtitle:
            title_ = QCoreApplication::translate("VideoDropTarget", "Release to attach subtitles");
            hint_ = QCoreApplication::translate(
                "VideoDropTarget", "Drop a subtitle file anywhere in this window");
            break;
        case DropKind::Audio:
            title_ = QCoreApplication::translate("VideoDropTarget", "Release to add audio track");
            hint_ = QCoreApplication::translate(
                "VideoDropTarget", "Drop an audio file anywhere in this window");
            break;
        case DropKind::None:
            return;
    }
    bodyFont_ = host_->font();
    titleFont_ = bodyFont_;
    titleFont_.setBold(true);
    if (titleFont_.pointSizeF() > 0.0) titleFont_.setPointSizeF(titleFont_.pointSizeF() + 4.0);
    else titleFont_.setPixelSize(titleFont_.pixelSize() + 6);
    const QPalette palette = host_->palette();
    background_ = palette.color(QPalette::Window);
    foreground_ = palette.color(QPalette::WindowText);
    border_ = foreground_;
    border_.setAlpha(140);
    muted_ = foreground_;
    muted_.setAlpha(180);
    scrim_ = background_;
    scrim_.setAlpha(210);
    titleHeight_ = QFontMetrics(titleFont_).height();
    bodyHeight_ = QFontMetrics(bodyFont_).height();
    textWidth_ = -1;
    // Colours, fonts and the elided strings are layer pixels: a new appearance
    // is a new raster, not a repaint of the old one.
    dropLayers();
    if (!overlay_.isNull()) {
        overlay_->setAccessibleName(title_);
        overlay_->setAccessibleDescription(fileName_ + QStringLiteral(". ") + hint_);
        overlay_->update();
    }
}

void VideoDropTarget::beginIndicator(const QString& path, DropKind kind) {
    ++dragGeneration_;
    leaveTimer_.stop();
    if (path_ == path && indicatorKind_ == kind) return;
    path_ = path;
    indicatorKind_ = kind;
    const qsizetype separator = std::max(path.lastIndexOf(u'/'), path.lastIndexOf(u'\\'));
    fileName_ = path.sliced(separator + 1);
    refreshAppearance();
    if (!overlay_.isNull()) {
        overlay_->setGeometry(host_->rect());
        overlay_->show();
        overlay_->raise();
    }
    emit indicatorChanged();
}

void VideoDropTarget::clearIndicator() {
    ++dragGeneration_;
    leaveTimer_.stop();
    if (path_.isEmpty()) return;
    path_.clear();
    indicatorKind_ = DropKind::None;
    dropLayers();
    if (!overlay_.isNull()) overlay_->hide();
    emit indicatorChanged();
}

bool VideoDropTarget::eventFilter(QObject* watched, QEvent* event) {
    if (host_.isNull()) return false;
    const auto type = event->type();
    if (watched == host_ && type == QEvent::Destroy) {
        leaveTimer_.stop();
        path_.clear();
        host_.clear();
        return false;
    }
    if (type == QEvent::ApplicationDeactivate ||
        (type == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)) {
        clearIndicator();
        return false;
    }
    QWidget* const window = host_->window();
    if (watched == window) {
        if (type == QEvent::WindowBlocked) windowBlocked_ = true;
        if (type == QEvent::WindowUnblocked) windowBlocked_ = false;
        if (type == QEvent::WindowBlocked || type == QEvent::WindowDeactivate ||
            type == QEvent::Hide) {
            clearIndicator();
            return false;
        }
    }
    if (watched == host_) {
        if (type == QEvent::Hide || type == QEvent::EnabledChange) {
            if (!host_->isVisible() || !host_->isEnabled()) clearIndicator();
            return false;
        }
        if (type == QEvent::ParentChange) {
            windowBlocked_ = false;
            host_->window()->setAcceptDrops(true);
            clearIndicator();
            return false;
        }
        if (type == QEvent::Resize && !overlay_.isNull()) overlay_->setGeometry(host_->rect());
        if (!path_.isEmpty() && (type == QEvent::LanguageChange || type == QEvent::FontChange ||
                                type == QEvent::PaletteChange || type == QEvent::StyleChange)) {
            refreshAppearance();
            emit indicatorChanged();
            return false;
        }
    }
    if (!path_.isEmpty() && !overlay_.isNull() && watched != overlay_ &&
        (type == QEvent::Show || type == QEvent::ZOrderChange)) {
        auto* child = qobject_cast<QWidget*>(watched);
        if (child != nullptr && host_->isAncestorOf(child)) overlay_->raise();
    }
    if (type != QEvent::DragEnter && type != QEvent::DragMove &&
        type != QEvent::DragLeave && type != QEvent::Drop) return false;
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget == nullptr) return false;
    QWidget* const modal = QApplication::activeModalWidget();
    if (!host_->isVisible() || !host_->isEnabled() || windowBlocked_ ||
        (modal != nullptr && modal->window() != window) ||
        widget->window() != window) {
        clearIndicator();
        return false;
    }
    if (type == QEvent::DragLeave) {
        if (!active()) return false;
        // Qt leaves the old child before entering its sibling; keep the indicator
        // until that handoff has had a chance to deliver another enter or move.
        leaveGeneration_ = dragGeneration_;
        leaveTimer_.start();
        event->accept();
        return true;
    }
    auto* drop = static_cast<QDropEvent*>(event);
    if (!drop->possibleActions().testFlag(Qt::CopyAction)) {
        clearIndicator();
        return false;
    }
    DropKind kind = DropKind::None;
    const QString path = classify(drop->mimeData(), acceptsTracks_, &kind);
    if (path.isEmpty() || kind == DropKind::None) {
        clearIndicator();
        return false;
    }
    drop->setDropAction(Qt::CopyAction);
    drop->accept();
    if (type != QEvent::Drop) {
        beginIndicator(path, kind);
        return true;
    }
    QPointer<VideoDropTarget> guard(this);
    clearIndicator();
    if (!guard.isNull()) {
        switch (kind) {
            case DropKind::Video:
                emit guard->videoDropped(path);
                break;
            case DropKind::Subtitle:
                emit guard->subtitleDropped(path);
                break;
            case DropKind::Audio:
                emit guard->audioDropped(path);
                break;
            case DropKind::None:
                break;
        }
    }
    return true;
}

void VideoDropTarget::paintIndicator(QPainter& painter, const QRect& bounds) const {
    if (path_.isEmpty() || bounds.isEmpty()) return;
    const qreal ratio =
        (painter.device() != nullptr) ? painter.device()->devicePixelRatio() : qreal(1.0);
    // Read from the painter instead of assumed: the shell paints one half of
    // this overlay through a painter translated by the surface's own offset, and
    // a layer copied at the wrong phase is a blurred copy at every fractional
    // ratio.
    const QTransform toDevice = painter.deviceTransform();
    const bool oneToOne = deviceAlignedTransform(toDevice, ratio);
    painter.save();
    painter.setClipRect(bounds, Qt::IntersectClip);
    // The scrim is the one primitive with nothing to antialias: a flat fill whose
    // only edges are the window's. It is painted straight and never cached - a
    // full-window layer would be a window-area allocation, which is exactly what
    // an overlay that appears mid-drag must not make. Every other primitive is a
    // device-resolution layer below, so this painter needs no filter or
    // antialiasing hint of its own.
    painter.fillRect(bounds, scrim_);
    const int margin = std::min({kMargin, bounds.width() / 8, bounds.height() / 8});
    const QRect available = bounds.adjusted(margin, margin, -margin, -margin);
    if (available.isEmpty()) {
        painter.restore();
        return;
    }
    const int padding = std::min(kPadding, available.width() / 8);
    const int fullHeight = 2 * padding + kIconSize + 2 * kGap + kSmallGap +
                           titleHeight_ + 2 * bodyHeight_;
    const QSize size(std::min(kCardWidth, available.width()), std::min(fullHeight, available.height()));
    const QRect card(available.center() - QPoint(size.width() / 2, size.height() / 2), size);
    if (ensureLayers(available, card, padding, ratio)) {
        // The dashed perimeter is a band of tiles rather than one window-sized
        // image: each is only as thick as the corner arc it carries, so what this
        // overlay holds follows the window's perimeter and never its area.
        for (const Layer& layer : layers_.ring) {
            blitLayer(painter, layer.pixmap, layer.origin, ratio, oneToOne, toDevice);
        }
        blitLayer(painter, layers_.card.pixmap, layers_.card.origin, ratio, oneToOne, toDevice);
    }
    painter.restore();
}

void VideoDropTarget::dropLayers() const {
    // The layers belong to the drag, not to the window: releasing them when the
    // indicator clears keeps an idle overlay at zero bytes.
    layers_ = OverlayLayers{};
}

bool VideoDropTarget::ensureLayers(const QRect& available, const QRect& card, int padding,
                                   qreal ratio) const {
    if (layers_.built && layers_.available == available && layers_.cardBox == card &&
        qFuzzyCompare(layers_.ratio, ratio)) {
        return true;
    }
    dropLayers();
    if (card.isEmpty() || ratio <= 0.0) return false;

    layers_.available = available;
    layers_.cardBox = card;
    layers_.ratio = ratio;

    // The geometry crosses to device pixels once, so every layer edge and every
    // seam between layers is a whole pixel.
    const auto toDevicePixels = [ratio](int logical) {
        return static_cast<int>(std::lround(logical * ratio));
    };
    const auto toLogical = [ratio](int device) { return device / ratio; };

    const int left = toDevicePixels(available.left());
    const int top = toDevicePixels(available.top());
    const int right = toDevicePixels(available.left() + available.width());
    const int bottom = toDevicePixels(available.top() + available.height());
    const int reach = std::max(1, toDevicePixels(kStrokeReach));
    const int corner = std::max(1, toDevicePixels(kCornerReach));

    // Four abutting strips: the top and bottom ones carry both corner arcs, the
    // side ones only the straight edges, so each is as thick as the arc it has
    // to hold rather than as the window.
    const QRect strips[] = {
        QRect(left - reach, top - reach, right - left + 2 * reach, reach + corner),
        QRect(left - reach, bottom - corner, right - left + 2 * reach, corner + reach),
        QRect(left - reach, top + corner, 2 * reach, bottom - top - 2 * corner),
        QRect(right - reach, top + corner, 2 * reach, bottom - top - 2 * corner)};

    // Every tile draws the whole path and is clipped to its own bounds by the
    // layer it renders into, so the dash sequence - which is part of the stroke
    // geometry - stays continuous across the blits and nothing is drawn twice.
    const auto ringTile = [this, available, ratio](const QRect& deviceBox) {
        return renderLayer(deviceBox, ratio, [this, available](QPainter& painter) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(border_, 2, Qt::DashLine));
            painter.drawRoundedRect(QRectF(available), kRadius, kRadius);
        });
    };
    for (const QRect& strip : strips) {
        // QRect's right() and bottom() are inclusive, and these strips tile with
        // half-open edges so two neighbours never share a pixel.
        const int stripRight = strip.x() + strip.width();
        const int stripBottom = strip.y() + strip.height();
        for (int y = strip.y(); y < stripBottom; y += kMaxLayerExtent) {
            for (int x = strip.x(); x < stripRight; x += kMaxLayerExtent) {
                const QRect box(x, y, std::min(kMaxLayerExtent, stripRight - x),
                                std::min(kMaxLayerExtent, stripBottom - y));
                layers_.ring.push_back(
                    Layer{ringTile(box), QPointF(toLogical(box.left()), toLogical(box.top()))});
            }
        }
    }
    const QRect cardBox(toDevicePixels(card.left()), toDevicePixels(card.top()),
                        toDevicePixels(card.left() + card.width()) -
                            toDevicePixels(card.left()),
                        toDevicePixels(card.top() + card.height()) -
                            toDevicePixels(card.top()));
    if (cardBox.isEmpty()) return false;
    layers_.card = Layer{renderLayer(cardBox, ratio, [this, card, padding](QPainter& painter) {
                             paintCard(painter, card, padding);
                         }),
                         QPointF(toLogical(cardBox.left()), toLogical(cardBox.top()))};
    layers_.built = true;
    return true;
}

void VideoDropTarget::paintCard(QPainter& painter, const QRect& card, int padding) const {
    painter.setPen(QPen(border_, 1));
    painter.setBrush(background_);
    painter.drawRoundedRect(QRectF(card), kRadius, kRadius);
    const QRect content = card.adjusted(padding, padding, -padding, -padding);
    const int textWidth = content.width();
    if (textWidth <= 0 || content.height() < bodyHeight_) return;
    if (textWidth_ != textWidth) {
        textWidth_ = textWidth;
        displayedTitle_ = QFontMetrics(titleFont_).elidedText(title_, Qt::ElideRight, textWidth);
        const QFontMetrics bodyMetrics(bodyFont_);
        displayedFileName_ = bodyMetrics.elidedText(fileName_, Qt::ElideMiddle, textWidth);
        displayedHint_ = bodyMetrics.elidedText(hint_, Qt::ElideRight, textWidth);
    }
    const bool showTitle = content.height() >= titleHeight_ + kSmallGap + bodyHeight_;
    const bool showHint = content.height() >= titleHeight_ + kSmallGap + 2 * bodyHeight_ + kGap;
    const int textHeight = (showTitle ? titleHeight_ + kSmallGap : 0) + bodyHeight_ +
                           (showHint ? kGap + bodyHeight_ : 0);
    const bool showIcon = content.height() >= textHeight + kIconSize + kGap;
    const int totalHeight = textHeight + (showIcon ? kIconSize + kGap : 0);
    int y = content.top() + (content.height() - totalHeight) / 2;
    if (showIcon) {
        painter.save();
        painter.translate(card.center().x() - kIconSize / 2, y);
        painter.scale(kIconSize, kIconSize);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(foreground_, 0.04, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        constexpr QPointF file[]{QPointF(0.2, 0.08), QPointF(0.62, 0.08), QPointF(0.8, 0.27),
                                QPointF(0.8, 0.92), QPointF(0.2, 0.92)};
        constexpr QPointF fold[]{QPointF(0.62, 0.08), QPointF(0.62, 0.27), QPointF(0.8, 0.27)};
        constexpr QPointF play[]{QPointF(0.42, 0.42), QPointF(0.42, 0.72), QPointF(0.65, 0.57)};
        painter.drawPolygon(file, 5);
        painter.drawPolyline(fold, 3);
        if (indicatorKind_ == DropKind::Video) {
            painter.setBrush(foreground_);
            painter.drawPolygon(play, 3);
        } else if (indicatorKind_ == DropKind::Subtitle) {
            painter.drawLine(QPointF(0.32, 0.46), QPointF(0.68, 0.46));
            painter.drawLine(QPointF(0.32, 0.57), QPointF(0.68, 0.57));
            painter.drawLine(QPointF(0.32, 0.68), QPointF(0.58, 0.68));
        } else if (indicatorKind_ == DropKind::Audio) {
            painter.drawLine(QPointF(0.32, 0.53), QPointF(0.32, 0.63));
            painter.drawLine(QPointF(0.42, 0.42), QPointF(0.42, 0.74));
            painter.drawLine(QPointF(0.52, 0.32), QPointF(0.52, 0.84));
            painter.drawLine(QPointF(0.62, 0.42), QPointF(0.62, 0.74));
            painter.drawLine(QPointF(0.72, 0.53), QPointF(0.72, 0.63));
        }
        painter.restore();
        y += kIconSize + kGap;
    }
    painter.setPen(foreground_);
    if (showTitle) {
        painter.setFont(titleFont_);
        painter.drawText(QRect(content.left(), y, textWidth, titleHeight_),
                         Qt::AlignCenter | Qt::TextSingleLine, displayedTitle_);
        y += titleHeight_ + kSmallGap;
    }
    painter.setFont(bodyFont_);
    painter.drawText(QRect(content.left(), y, textWidth, bodyHeight_),
                     Qt::AlignCenter | Qt::TextSingleLine, displayedFileName_);
    if (showHint) {
        painter.setPen(muted_);
        painter.drawText(QRect(content.left(), y + bodyHeight_ + kGap, textWidth, bodyHeight_),
                         Qt::AlignCenter | Qt::TextSingleLine, displayedHint_);
    }
}

} // namespace mikai::gui
