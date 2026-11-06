#include "HoverPreview.hpp"

#include "FramePreviewService.hpp"
#include "ShellTheme.hpp"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace mplayer {
namespace shell {

namespace {

/// Slack between the card and the shell's edge, kept when it is clamped.
constexpr int kCardInset = 8;
constexpr int kCardGap = 10;
constexpr int kCardMinWidth = 176;
constexpr int kCardMaxWidth = 340;
/// A card covering the picture is no longer a preview of it.
constexpr int kCardMaxHeightPercent = 55;
constexpr double kFallbackAspect = 16.0 / 9.0;
/// The share of the shell's width the card asks for before the limits above.
constexpr int kCardWidthPercent = 28;
/// Two instants this close are the same frame.
constexpr double kSameInstantSec = 0.04;
/// How far a stale picture is dimmed. Enough that it cannot be mistaken for the
/// instant under the pointer, not so much that it stops being a preview.
constexpr int kStaleDimAlpha = 130;

bool plausibleAspect(double aspect) {
    return std::isfinite(aspect) && aspect >= 0.4 && aspect <= 4.5;
}

}  // namespace

QRect previewCardRect(const QRect& bounds, const QPoint& anchor, const QRect& bar,
                      double aspect) {
    const QRect area = bounds.adjusted(kCardInset, kCardInset, -kCardInset, -kCardInset);
    if (area.width() <= 0 || area.height() <= 0) return QRect();

    const double ratio = plausibleAspect(aspect) ? aspect : kFallbackAspect;
    int width = std::clamp(bounds.width() * kCardWidthPercent / 100, kCardMinWidth, kCardMaxWidth);
    width = std::clamp(width, 1, area.width());
    int height = std::max(1, static_cast<int>(std::lround(width / ratio)));
    const int maxHeight =
        std::max(1, area.height() * kCardMaxHeightPercent / 100);
    if (height > maxHeight) {
        // Fitted by height with the width following the aspect: never cropped.
        height = maxHeight;
        width = std::clamp(static_cast<int>(std::lround(height * ratio)), 1, area.width());
    }
    height = std::clamp(height, 1, area.height());

    int x = anchor.x() - width / 2;
    int y = bar.top() - kCardGap - height;
    if (y < area.top()) {
        // No room above the bar: move under it rather than clamp over it.
        y = bar.bottom() + kCardGap;
    }
    const int maxX = std::max(area.left(), area.right() - width + 1);
    const int maxY = std::max(area.top(), area.bottom() - height + 1);
    x = std::clamp(x, area.left(), maxX);
    y = std::clamp(y, area.top(), maxY);
    return QRect(x, y, width, height);
}

bool previewReplyMatches(quint64 replyToken, quint64 currentToken, double replySeconds,
                         double currentSeconds) {
    if (replyToken == 0 || replyToken != currentToken) return false;
    if (!std::isfinite(replySeconds) || !std::isfinite(currentSeconds)) return false;
    return std::abs(replySeconds - currentSeconds) <= kSameInstantSec;
}

HoverPreview::HoverPreview(QWidget* host)
    : QWidget(host), service_(new FramePreviewService(this)) {
    setObjectName(QStringLiteral("playerHoverPreview"));
    // Translucent backing store plus rounded clipping: corners without a mask.
    // The card is therefore never an opaque widget: it paints its own rounded
    // fill and lets the window show through the corners.
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
    // The pointer belongs to the timeline underneath.
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFocusPolicy(Qt::NoFocus);
    hide();
    connect(service_, &FramePreviewService::frameReady, this, &HoverPreview::applyFrame);
    connect(service_, &FramePreviewService::frameUnavailable, this,
            &HoverPreview::applyUnavailable);
}

HoverPreview::~HoverPreview() { release(); }

void HoverPreview::setSource(const QString& url, quint64 sourceEpoch) {
    if (released_ || service_ == nullptr) return;
    if (url == service_->source() && sourceEpoch == service_->sourceEpoch()) return;
    service_->setSource(url, sourceEpoch);
    // A frame of the previous source must never describe the new one.
    dismiss();
    aspect_ = kFallbackAspect;
}

void HoverPreview::present(double seconds, const QPoint& anchorGlobal, const QRect& barGlobal,
                           const QString& segmentLabel, bool allowFrame) {
    if (released_ || service_ == nullptr) return;
    QWidget* host = parentWidget();
    if (host == nullptr || !std::isfinite(seconds) || seconds < 0.0) {
        dismiss();
        return;
    }
    const QString label = segmentLabel.trimmed();
    const bool labelChanged = label_ != label;
    const bool sameRequest = isVisible() && seconds_ == seconds && frameAllowed_ == allowFrame;
    seconds_ = seconds;
    label_ = label;
    frameAllowed_ = allowFrame;
    anchor_ = host->mapFromGlobal(anchorGlobal);
    bar_ = QRect(host->mapFromGlobal(barGlobal.topLeft()), barGlobal.size());

    place();
    if (sameRequest) {
        // An unchanged hover must not retire the decoder's in-flight reply.
        if (labelChanged) chromeUpdate(this);
        service_->noteActivity();
        return;
    }
    ++token_;
    pendingToken_ = 0;
    if (!isVisible()) {
        // Raised once: a raise per mouse move invalidates the region the card
        // covers, for a card that is already on top.
        show();
        raise();
    }

    if (!allowFrame) {
        // A scrub is a decode per pixel of travel with nothing to show for it.
        service_->cancel();
        frame_ = {};
        frameSeconds_ = -1.0;
        status_.clear();
        detail_.clear();
        refused_ = false;
        chromeUpdate(this);
        return;
    }

    // A frame decoded for another instant stays on the card, dimmed and named as
    // stale, instead of being replaced by a grey void for the length of the
    // decode. A picture whose own timestamp is this instant's needs nothing.
    if (!frame_.isNull() && std::abs(frameSeconds_ - seconds) <= kSameInstantSec) {
        const bool changed = labelChanged || !status_.isEmpty() || !detail_.isEmpty() || refused_;
        status_.clear();
        detail_.clear();
        refused_ = false;
        if (changed) chromeUpdate(this);
        return;
    }
    const QString decoding = tr("Decoding preview…");
    const bool changed = labelChanged || frame_.isNull() || status_ != decoding ||
                         !detail_.isEmpty() || refused_;
    status_ = decoding;
    detail_.clear();
    refused_ = false;
    if (changed) chromeUpdate(this);
    pendingToken_ = token_;
    service_->requestFrame(token_, seconds);
}

void HoverPreview::prewarm(double seconds) {
    if (released_ || service_ == nullptr) return;
    service_->prewarm(seconds);
}

void HoverPreview::noteActivity() {
    if (released_ || service_ == nullptr) return;
    service_->noteActivity();
}

void HoverPreview::dismiss() {
    ++token_;
    pendingToken_ = 0;
    if (service_ != nullptr) service_->cancel();
    frame_ = {};
    frameSeconds_ = -1.0;
    status_.clear();
    detail_.clear();
    refused_ = false;
    label_.clear();
    hide();
}

void HoverPreview::release() {
    if (released_) return;
    dismiss();
    released_ = true;
    if (service_ != nullptr) service_->shutdown();
}

FramePreviewStats HoverPreview::previewStats() const {
    return service_ != nullptr ? service_->stats() : FramePreviewStats{};
}

void HoverPreview::applyFrame(quint64 token, double requestedSeconds, double decodedSeconds,
                              const QImage& frame) {
    if (released_ || frame.isNull() || !isVisible() || pendingToken_ == 0) return;
    const bool newest = previewReplyMatches(token, pendingToken_, requestedSeconds, seconds_);
    if (newest) {
        pendingToken_ = 0;
        // The answer settles the card before it is weighed as a picture: a cache
        // hit for the instant under the pointer is often the same frame the card
        // already shows, and leaving "Decoding preview…" over it is a wait that
        // is over.
        status_.clear();
        detail_.clear();
        refused_ = false;
    }
    // A picture already closer to the instant under the pointer than this one is
    // the better placeholder; a decode that finished late must not walk the card
    // backwards to an older instant.
    if (!frame_.isNull() && std::isfinite(decodedSeconds) &&
        std::abs(frameSeconds_ - seconds_) <= std::abs(decodedSeconds - seconds_)) {
        if (newest) chromeUpdate(this);
        return;
    }
    frame_ = frame;
    // The decoder's own reading, never the pointer's instant: this is the time
    // the caption prints and the only claim the picture makes.
    frameSeconds_ = decodedSeconds;
    if (frame.width() > 0 && frame.height() > 0) {
        aspect_ = static_cast<double>(frame.width()) / static_cast<double>(frame.height());
    }
    if (!newest) {
        status_ = tr("Decoding preview…");
        detail_.clear();
        refused_ = false;
    }
    place();
    chromeUpdate(this);
}

void HoverPreview::applyUnavailable(quint64 token, double seconds, const QString& reason) {
    if (released_ || !isVisible()) return;
    if (!previewReplyMatches(token, pendingToken_, seconds, seconds_)) return;
    pendingToken_ = 0;
    // Nothing is coming: no older frame may stay behind as if it were this one.
    frame_ = {};
    frameSeconds_ = -1.0;
    status_ = tr("Preview unavailable");
    detail_ = reason;
    refused_ = true;
    place();
    chromeUpdate(this);
}

void HoverPreview::place() {
    QWidget* host = parentWidget();
    if (host == nullptr) return;
    const QRect card = previewCardRect(host->rect(), anchor_, bar_, aspect_);
    if (!card.isValid() || card == geometry()) return;
    const QRect vacated = geometry();
    const bool wasVisible = isVisible();
    // Only a translation keeps the card's own pixels; a size change re-lays it.
    const bool onlyMoved = vacated.size() == card.size();
    setGeometry(card);
    // The card is a translucent child: the area it covered is not repainted by
    // moving it, so somebody has to be asked for it. A plain parent update() is
    // clipped away wherever the card stood over the picture, so the paint goes
    // through the chrome host, which owns both devices - expressed as the
    // widget-local rect that maps onto the vacated one.
    if (wasVisible && !vacated.isEmpty()) {
        if (ChromeHost* chromeHost = chromeHostOf(this)) {
            if (onlyMoved) {
                chromeHost->chromeMoved(this);
            } else {
                chromeHost->chromeDirty(this, QRect(vacated.topLeft() - pos(), vacated.size()));
            }
        } else {
            host->update(vacated);
        }
    }
}

void HoverPreview::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

void HoverPreview::paintChrome(QPainter& painter) {
    enableChromeAntialiasing(painter);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const double radius = std::clamp(cornerRadius() > 0.0 ? cornerRadius() : 12.0, 4.0, 20.0);
    QPainterPath shape;
    shape.addRoundedRect(card, radius, radius);
    painter.save();
    painter.setClipPath(shape, Qt::IntersectClip);

    const bool hasFrame = !frame_.isNull();
    const bool stale = hasFrame && !status_.isEmpty() && !refused_;
    // The whole rect gets a defined fill before anything else is drawn, so no
    // pixel of the card is left to whatever the parent last had there.
    painter.fillRect(rect(), hasFrame ? QColor(0, 0, 0) : flyout());
    if (hasFrame) {
        // The card is sized to the frame's aspect, so this covers it exactly.
        painter.drawImage(rect(), frame_);
        if (stale) {
            painter.fillRect(rect(), QColor(0, 0, 0, kStaleDimAlpha));
        }
        // A frame can be bright where the caption sits; the shade keeps the
        // timestamp readable without tinting the picture.
        QLinearGradient shade(0, height() * 0.5, 0, height());
        shade.setColorAt(0.0, QColor(0, 0, 0, 0));
        shade.setColorAt(1.0, QColor(0, 0, 0, 165));
        painter.fillRect(QRectF(0, height() * 0.5, width(), height() * 0.5), shade);
    }

    // The time on the card is the picture's own: an instant decoded for another
    // pointer position keeps that instant's clock rather than borrowing this
    // one's.
    const QString timeText = formatTime(hasFrame && frameSeconds_ >= 0.0 ? frameSeconds_ : seconds_);
    const QFont timeFont = monoFont(13, true);
    const QFont labelFont = uiFont(11, true);
    const int pad = 10;
    const int timeHeight = QFontMetrics(timeFont).height();
    const int timeTop = height() - pad - timeHeight;
    const QRectF timeRect(0, timeTop, width(), timeHeight);
    // Drawn twice: the shadow is what keeps white text legible on a bright
    // frame the card cannot see.
    painter.setFont(timeFont);
    painter.setPen(QColor(0, 0, 0, 150));
    painter.drawText(timeRect.translated(0, 1), Qt::AlignCenter, timeText);
    painter.setPen(white(stale ? 205 : 255));
    painter.drawText(timeRect, Qt::AlignCenter, timeText);

    if (!label_.isEmpty()) {
        const int labelHeight = QFontMetrics(labelFont).height();
        const QRectF labelRect(0, timeTop - labelHeight - 2, width(), labelHeight);
        painter.setFont(labelFont);
        painter.setPen(QColor(0, 0, 0, 150));
        painter.drawText(labelRect.translated(0, 1), Qt::AlignCenter, label_);
        painter.setPen(white(255));
        painter.drawText(labelRect, Qt::AlignCenter, label_);
    }

    if (!status_.isEmpty()) {
        // The state is text, not a shade: "a decode is running" and "there will
        // never be a frame" must not look the same, and neither may look like a
        // picture that is simply not there.
        painter.setFont(uiFont(9, true));
        painter.setPen(stale ? white(200) : w45());
        const int statusHeight = std::max(1, timeTop - 16);
        painter.drawText(QRectF(10, 10, width() - 20, statusHeight),
                         Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, status_);
        if (!detail_.isEmpty() && !stale) {
            painter.setFont(uiFont(8));
            painter.setPen(w35());
            const int detailHeight = std::max(1, statusHeight - QFontMetrics(uiFont(9, true)).height() - 2);
            painter.drawText(QRectF(10, 10 + QFontMetrics(uiFont(9, true)).height() + 2,
                                    width() - 20, detailHeight),
                             Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, detail_);
        }
    }

    painter.restore();
    paintRounded(painter, card, radius, Qt::transparent, w15());
}

}  // namespace shell
}  // namespace mplayer
