#include "widgets/ToggleSwitch.hpp"

#include "state/AppearanceStore.hpp"
#include "theme/Metrics.hpp"
#include "theme/Palette.hpp"

#include <QPainter>
#include <QPainterPath>

namespace mikai::app {

ToggleSwitch::ToggleSwitch(QWidget* parent) : QAbstractButton(parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setFixedSize(sizeHint());
    connect(this, &QAbstractButton::toggled, this, [this](bool) { update(); });
}

QSize ToggleSwitch::sizeHint() const {
    return {theme::kSettingsSwitchWidth, theme::kSettingsSwitchHeight};
}

bool ToggleSwitch::hitButton(const QPoint& position) const {
    return rect().contains(position);
}

void ToggleSwitch::paintEvent(QPaintEvent*) {
    const theme::Palette palette = theme::paletteFor(currentAppearance());
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // Qt already applies DPR; scalar matrix multiplication also changes m33.

    const int height = theme::kSettingsSwitchHeight;
    const int width = theme::kSettingsSwitchWidth;
    // Inset by 2 px so the 1.5 px focus ring drawn outside the track never clips
    // against the widget's bounding rect.
    const QRectF track(2.0, 2.0, width - 4.0, height - 4.0);
    const QColor off = QColor(palette.surfaceActive);
    const QColor on = palette.accentValue;
    QColor fill = isChecked() ? on : off;
    if (!isEnabled()) fill.setAlpha(120);

    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(track, track.height() / 2.0, track.height() / 2.0);

    const double knob = track.height() - 4.0;
    const double left = isChecked() ? track.right() - knob - 2.0 : track.left() + 2.0;
    painter.setBrush(isChecked() ? QColor(palette.accentInk) : QColor(palette.muted));
    painter.drawEllipse(QRectF(left, track.top() + 2.0, knob, knob));

    if (hasFocus()) {
        QPen ring(QColor(palette.accentSoft));
        ring.setWidthF(1.5);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(ring);
        painter.drawRoundedRect(track.adjusted(-1.0, -1.0, 1.0, 1.0), track.height() / 2.0 + 1.0,
                                track.height() / 2.0 + 1.0);
    }
}

}  // namespace mikai::app
