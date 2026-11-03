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
// The gap between the ring and the first letter, part of the column the words
// must be laid out beside rather than under.
constexpr int kRingGap = 12;
constexpr int kDetailGap = 4;
constexpr int kPanelMinWidth = 200;
constexpr int kTickMs = 90;

/// The card one headline and detail need, measured from the words themselves.
///
/// `availableWidth` is the window the card stands in, or 0 for the intrinsic
/// size. The width carries the ring column and the padding on both sides before
/// the words get what is left, which is the whole point: a width taken from the
/// text advance alone leaves the words about a hundred pixels short and cuts
/// the end off a headline.
struct CardMeasure {
    QSize size;
    int textWidth = 0;
    int headlineHeight = 0;
    int detailHeight = 0;
};

CardMeasure measureCard(const QString& headline, const QString& detail, int availableWidth) {
    const QFontMetrics headlineMetrics(uiFont(12, true));
    const QFontMetrics detailMetrics(uiFont(10));
    const int ringColumn = kRingRadius * 2 + kRingGap;
    const int naturalText =
        std::max(headlineMetrics.horizontalAdvance(headline),
                 detail.isEmpty() ? 0 : detailMetrics.horizontalAdvance(detail));
    int width = kPanelPadding * 2 + ringColumn + naturalText;
    width = std::min(width, kPanelMaxWidth);
    if (availableWidth > 0) width = std::min(width, availableWidth);
    width = std::max(width, kPanelMinWidth);

    CardMeasure measure;
    measure.textWidth = std::max(1, width - kPanelPadding * 2 - ringColumn);
    // A tall bound on purpose: the question is how many lines the words take at
    // this width, and a bound that could clip would answer with its own height.
    constexpr int kAnyHeight = 10000;
    measure.headlineHeight =
        headlineMetrics
            .boundingRect(QRect(0, 0, measure.textWidth, kAnyHeight), Qt::TextWordWrap, headline)
            .height();
    measure.detailHeight =
        detail.isEmpty()
            ? 0
            : detailMetrics
                  .boundingRect(QRect(0, 0, measure.textWidth, kAnyHeight), Qt::TextWordWrap, detail)
                  .height();
    const int textHeight = measure.detailHeight > 0
                               ? measure.headlineHeight + kDetailGap + measure.detailHeight
                               : measure.headlineHeight;
    measure.size = QSize(width, kPanelPadding * 2 + std::max(textHeight, kRingRadius * 2));
    return measure;
}
}  // namespace

QSize OpeningOverlay::sizeHint() const {
    // The card's own size for the words on it: the number a caller would need to
    // place this overlay, and the one paintEvent centres inside the window. Both
    // come from the same measurement, so they cannot drift apart.
    return measureCard(headline_, detail_, 0).size;
}

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
    // Measured from the words, inside the window the card stands in: past the
    // card's own limit the lines wrap, they are never elided or cut.
    const CardMeasure measure = measureCard(headline_, detail_, width());
    const QRectF panel((this->width() - measure.size.width()) / 2.0,
                       (this->height() - measure.size.height()) / 2.0, measure.size.width(),
                       measure.size.height());

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

    // The column the measurement reserved: the ring, the gap and the padding
    // leave the words exactly this much, and the same numbers decided the card's
    // width - so a line either fits or wraps, and neither is cut.
    const double textLeft = panel.left() + kPanelPadding + kRingRadius * 2.0 + kRingGap;
    const double textWidth = measure.textWidth;
    painter.setPen(white(235));
    painter.setFont(headlineFont);
    painter.drawText(QRectF(textLeft, panel.top() + kPanelPadding, textWidth,
                            measure.headlineHeight + 2),
                     Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, headline_);
    if (!detail_.isEmpty()) {
        painter.setPen(w55());
        painter.setFont(detailFont);
        // Two pixels of slack into the padding: a hinting difference between the
        // measuring and the drawing path must not cost a descender.
        painter.drawText(QRectF(textLeft,
                                panel.top() + kPanelPadding + measure.headlineHeight + kDetailGap,
                                textWidth, measure.detailHeight + 2),
                         Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, detail_);
    }
}

}  // namespace shell
}  // namespace mplayer
