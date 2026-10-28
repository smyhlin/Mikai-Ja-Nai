#include "EpisodeNavButton.hpp"

#include <QEnterEvent>

namespace mplayer {
namespace shell {

namespace {

constexpr int kButtonSize = 48;
constexpr int kGlyphSize = 20;

}  // namespace

EpisodeNavButton::EpisodeNavButton(Direction direction, const QString& tooltip, QWidget* parent)
    : QPushButton(parent), direction_(direction) {
    setFixedSize(kButtonSize, kButtonSize);
    setToolTip(tooltip);
    if (!tooltip.isEmpty()) {
        setAccessibleName(tooltip);
    }
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setFlat(true);
}

void EpisodeNavButton::setDirection(Direction direction) {
    if (direction_ == direction) {
        return;
    }
    direction_ = direction;
    chromeUpdate(this);
}

void EpisodeNavButton::paintChrome(QPainter& painter) {
    const double radius = cornerRadius();
    const QRectF box(0.5, 0.5, width() - 1.0, height() - 1.0);

    QColor fill = blackSoft();
    if (!isEnabled()) {
        fill.setAlpha(fill.alpha() / 2);
    }
    paintRounded(painter, box, radius, fill);

    const bool lit = hover_ && isEnabled();
    if (lit) {
        paintRounded(painter, box, radius, w15());
    }

    QColor tint = lit ? w90() : w70();
    if (!isEnabled()) {
        tint.setAlpha(tint.alpha() / 2);
    }
    const int inset = (width() - kGlyphSize) / 2;
    if (direction_ == Direction::Left) {
        IconPainter::chevronLeft(painter, inset, inset, kGlyphSize, tint);
    } else {
        IconPainter::chevronRight(painter, inset, inset, kGlyphSize, tint);
    }
}

void EpisodeNavButton::enterEvent(QEnterEvent* event) {
    hover_ = true;
    chromeUpdate(this);
    QPushButton::enterEvent(event);
}

void EpisodeNavButton::leaveEvent(QEvent* event) {
    hover_ = false;
    chromeUpdate(this);
    QPushButton::leaveEvent(event);
}

void EpisodeNavButton::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

}  // namespace shell
}  // namespace mplayer
