#include "HoverPreview.hpp"

#include "FramePreviewService.hpp"
#include "ShellTheme.hpp"

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

void HoverPreview::setSource(const QString& url) {
    if (released_ || service_ == nullptr) return;
    if (url == service_->source()) return;
    service_->setSource(url);
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
    seconds_ = seconds;
    label_ = segmentLabel.trimmed();
    anchor_ = host->mapFromGlobal(anchorGlobal);
    bar_ = QRect(host->mapFromGlobal(barGlobal.topLeft()), barGlobal.size());

    // A frame decoded for another instant is not this one's; the timestamp and
    // label are known immediately, so the card waits instead of showing them.
    if (!frame_.isNull() && std::abs(frameSeconds_ - seconds) > kSameInstantSec) {
        frame_ = {};
        frameSeconds_ = -1.0;
        status_.clear();
    }

    ++token_;
    place();
    if (!isVisible()) {
        // Raised once: a raise per mouse move invalidates the region the card
        // covers, for a card that is already on top.
        show();
        raise();
    }
    chromeUpdate(this);

    if (!allowFrame) {
        // A scrub is a decode per pixel of travel with nothing to show for it.
        service_->cancel();
        return;
    }
    if (!frame_.isNull()) return;  // this instant is already on the card
    service_->requestFrame(token_, seconds);
}

void HoverPreview::dismiss() {
    ++token_;
    if (service_ != nullptr) service_->cancel();
    frame_ = {};
    frameSeconds_ = -1.0;
    status_.clear();
    label_.clear();
    hide();
}

void HoverPreview::release() {
    if (released_) return;
    dismiss();
    released_ = true;
    if (service_ != nullptr) service_->shutdown();
}

void HoverPreview::applyFrame(quint64 token, double seconds, const QImage& frame) {
    if (released_ || frame.isNull() || !isVisible()) return;
    if (!previewReplyMatches(token, token_, seconds, seconds_)) return;
    frame_ = frame;
    frameSeconds_ = seconds;
    status_.clear();
    if (frame.width() > 0 && frame.height() > 0) {
        aspect_ = static_cast<double>(frame.width()) / static_cast<double>(frame.height());
    }
    place();
    chromeUpdate(this);
}

void HoverPreview::applyUnavailable(quint64 token, double seconds, const QString& reason) {
    if (released_ || !isVisible()) return;
    if (!previewReplyMatches(token, token_, seconds, seconds_)) return;
    // Nothing is coming: no older frame may stay behind as if it were this one.
    frame_ = {};
    frameSeconds_ = -1.0;
    status_ = reason;
    place();
    chromeUpdate(this);
}

void HoverPreview::place() {
    QWidget* host = parentWidget();
    if (host == nullptr) return;
    const QRect card = previewCardRect(host->rect(), anchor_, bar_, aspect_);
    if (card.isValid() && card != geometry()) setGeometry(card);
}

void HoverPreview::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

void HoverPreview::paintChrome(QPainter& painter) {
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const double radius = std::clamp(cornerRadius() > 0.0 ? cornerRadius() : 12.0, 4.0, 20.0);
    QPainterPath shape;
    shape.addRoundedRect(card, radius, radius);
    painter.save();
    painter.setClipPath(shape, Qt::IntersectClip);

    const bool hasFrame = !frame_.isNull();
    painter.fillRect(rect(), hasFrame ? QColor(0, 0, 0) : flyout());
    if (hasFrame) {
        // The card is sized to the frame's aspect, so this covers it exactly.
        painter.drawImage(rect(), frame_);
        // A frame can be bright where the caption sits; the shade keeps the
        // timestamp readable without tinting the picture.
        QLinearGradient shade(0, height() * 0.5, 0, height());
        shade.setColorAt(0.0, QColor(0, 0, 0, 0));
        shade.setColorAt(1.0, QColor(0, 0, 0, 165));
        painter.fillRect(QRectF(0, height() * 0.5, width(), height() * 0.5), shade);
    }

    const QString timeText = formatTime(seconds_);
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
    painter.setPen(white(255));
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

    if (!hasFrame && !status_.isEmpty()) {
        painter.setFont(uiFont(9));
        painter.setPen(w45());
        const int statusHeight = std::max(1, timeTop - 20);
        painter.drawText(QRectF(10, 10, width() - 20, statusHeight),
                         Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, status_);
    }

    painter.restore();
    paintRounded(painter, card, radius, Qt::transparent, w15());
}

}  // namespace shell
}  // namespace mplayer
