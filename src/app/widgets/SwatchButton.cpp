#include "widgets/SwatchButton.hpp"

#include "state/AppearanceStore.hpp"
#include "theme/Palette.hpp"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

namespace mikai::app {
namespace {

QColor gradientStop(int index) {
    static const QColor kSpectrum[] = {
        QColor(QStringLiteral("#f27bb0")), QColor(QStringLiteral("#f0a35e")),
        QColor(QStringLiteral("#5fd08a")), QColor(QStringLiteral("#67e4e4")),
        QColor(QStringLiteral("#6797e4")), QColor(QStringLiteral("#bdacff")),
    };
    return kSpectrum[qBound(0, index, 5)];
}

}  // namespace

SwatchButton::SwatchButton(const QColor& color, QWidget* parent)
    : QPushButton(parent), color_(color) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
}

void SwatchButton::setColor(const QColor& color) {
    color_ = color;
    update();
}

void SwatchButton::setSelected(bool selected) {
    if (selected == selected_) return;
    selected_ = selected;
    update();
}

void SwatchButton::setSpectrum(bool spectrum) {
    spectrum_ = spectrum;
    update();
}

void SwatchButton::paintEvent(QPaintEvent*) {
    const theme::Palette palette = theme::paletteFor(currentAppearance());
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (windowHandle()) {
        painter.setWorldTransform(painter.worldTransform() * windowHandle()->devicePixelRatio());
    }

    // The ring needs room outside the fill, so the chip body is inset by it.
    const QRectF body = QRectF(rect()).adjusted(3.0, 3.0, -3.0, -3.0);
    const qreal radius = body.height() / 2.0;

    if (spectrum_) {
        QLinearGradient sweep(body.topLeft(), body.bottomRight());
        sweep.setColorAt(0.0, gradientStop(0));
        sweep.setColorAt(0.2, gradientStop(1));
        sweep.setColorAt(0.4, gradientStop(2));
        sweep.setColorAt(0.6, gradientStop(3));
        sweep.setColorAt(0.8, gradientStop(4));
        sweep.setColorAt(1.0, gradientStop(5));
        painter.setBrush(sweep);
    } else {
        QColor fill = color_;
        if (!isEnabled()) fill.setAlpha(90);
        painter.setBrush(fill);
    }
    painter.setPen(QPen(QColor(palette.border), 1.0));
    painter.drawRoundedRect(body, radius, radius);

    QPen ring(selected_ ? QColor(palette.textPrimary) : QColor(palette.accentSoft));
    ring.setWidthF(2.0);
    if (selected_) {
        painter.setPen(ring);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(body.adjusted(-1.5, -1.5, 1.5, 1.5), radius + 1.5, radius + 1.5);
    } else if (hasFocus()) {
        painter.setPen(ring);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(body.adjusted(-1.5, -1.5, 1.5, 1.5), radius + 1.5, radius + 1.5);
    }
}

}  // namespace mikai::app
