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

    const int height = theme::kSettingsSwitchHeight;
    const int width = theme::kSettingsSwitchWidth;
    const QRectF track(0.5, 0.5, width - 1.0, height - 1.0);
    const QColor off = QColor(palette.surfaceActive);
    const QColor on = palette.accentValue;
    QColor fill = isChecked() ? on : off;
    if (!isEnabled()) fill.setAlpha(120);

    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(track, track.height() / 2.0, track.height() / 2.0);

    // The knob carries the accent when the switch is off so an inactive track
    // still shows where the control is; on a light bed a white knob would
    // vanish into the track.
    const double knob = height - 6.0;
    const double left = isChecked() ? track.right() - knob - 2.0 : track.left() + 2.0;
    painter.setBrush(isChecked() ? QColor(Qt::white) : QColor(palette.muted));
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
