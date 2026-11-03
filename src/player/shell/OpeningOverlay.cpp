#include "OpeningOverlay.hpp"

#include "ShellTheme.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include <algorithm>

namespace mplayer {
namespace shell {

namespace {
// The panel is a card, not a screen: it must leave the picture's edges visible
// so the window still reads as a player rather than as a splash page.
constexpr int kPanelMaxWidth = 420;
constexpr int kPanelPadding = 18;
constexpr int kRingRadius = 13;
constexpr int kTickMs = 90;
}  // namespace

OpeningOverlay::OpeningOverlay(QWidget* parent) : QWidget(parent) {
    // Above the picture, never in front of the pointer: a viewer may still
    // click, drag and hover while the stream opens.
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::NoFocus);
    timer_ = new QTimer(this);
    timer_->setInterval(kTickMs);
    connect(timer_, &QTimer::timeout, this, [this] {
        phase_ = (phase_ + 1) % 40;
        update();
    });
    hide();
}

void OpeningOverlay::showOpening(const QString& headline, const QString& detail) {
    headline_ = headline;
    detail_ = detail;
    shown_ = true;
    // Not restated as text properties for a test: the diagnostic locator is the
    // object name plus the live headline, which is what a consumer can see.
    setProperty("openingText", headline_);
    setProperty("openingDetail", detail_);
    if (!timer_->isActive()) {
        phase_ = 0;
        timer_->start();
    }
    show();
    raise();
    update();
}

void OpeningOverlay::setDetail(const QString& detail) {
    if (!shown_ || detail == detail_) return;
    detail_ = detail;
    setProperty("openingDetail", detail_);
    update();
}

void OpeningOverlay::hideOpening() {
    if (!shown_) return;
    shown_ = false;
    timer_->stop();
    hide();
}

void OpeningOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Nothing behind the panel: the surface below is the picture's own device.
    painter.fillRect(rect(), Qt::transparent);

    const QFont headlineFont = uiFont(12, true);
    const QFont detailFont = uiFont(10);
    const QFontMetrics headlineMetrics(headlineFont);
    const QFontMetrics detailMetrics(detailFont);
    int textWidth = headlineMetrics.horizontalAdvance(headline_);
    if (!detail_.isEmpty()) {
        textWidth = std::max(textWidth, detailMetrics.horizontalAdvance(detail_));
    }
    const int width = std::clamp(textWidth, 120, std::max(120, kPanelMaxWidth));
    const int headlineHeight = headlineMetrics.height();
    const int detailHeight = detail_.isEmpty() ? 0 : detailMetrics.height() + 4;
    const int height = kPanelPadding * 2 + std::max(headlineHeight + detailHeight,
                                                    kRingRadius * 2 + 4);
    const QRectF panel((this->width() - width) / 2.0, (this->height() - height) / 2.0, width,
                       height);

    const double radius = std::clamp(cornerRadius() > 0.0 ? cornerRadius() : 12.0, 4.0, 20.0);
    QPainterPath shape;
    shape.addRoundedRect(panel, radius, radius);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(8, 9, 13, 214));
    painter.drawPath(shape);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(w15(), 1.0));
    painter.drawPath(shape);

    // The arc: an indeterminate sweep. Nothing here claims a percentage the
    // window does not know.
    const QRectF ring(panel.left() + kPanelPadding,
                      panel.top() + (panel.height() - kRingRadius * 2.0) / 2.0,
                      kRingRadius * 2.0, kRingRadius * 2.0);
    QPen track(w25());
    track.setWidthF(2.5);
    painter.setPen(track);
    painter.drawEllipse(ring);
    QPen sweep(accent());
    sweep.setWidthF(2.5);
    sweep.setCapStyle(Qt::RoundCap);
    painter.setPen(sweep);
    painter.drawArc(ring, (phase_ * 12) * 16, 100 * 16);

    const double textLeft = ring.right() + kPanelPadding * 0.75;
    const double textWidthAvailable = panel.right() - kPanelPadding - textLeft;
    painter.setPen(white(235));
    painter.setFont(headlineFont);
    painter.drawText(QRectF(textLeft, panel.top() + kPanelPadding, textWidthAvailable,
                            headlineHeight),
                     Qt::AlignLeft | Qt::AlignVCenter, headline_);
    if (!detail_.isEmpty()) {
        painter.setPen(w55());
        painter.setFont(detailFont);
        painter.drawText(QRectF(textLeft, panel.top() + kPanelPadding + headlineHeight + 4,
                                textWidthAvailable, detailHeight),
                         Qt::AlignLeft | Qt::AlignVCenter, detail_);
    }
}

}  // namespace shell
}  // namespace mplayer
