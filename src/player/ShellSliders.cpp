#include "ShellSliders.hpp"

#include <QEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>

namespace mplayer {
namespace shell {

// ── RangeSlider ────────────────────────────────────────────────────────────

RangeSlider::RangeSlider(const QColor& accent, int track, QWidget* parent, int margin)
    : QWidget(parent), accent_(accent), track_(track), margin_(margin) {
    setMouseTracking(true);
    setCursor(Qt::CursorShape::PointingHandCursor);
    setMinimumHeight(22);
    // The application stylesheet paints every QWidget with the app background,
    // and this widget paints its own track over it. Both are self-painting, so
    // the inherited slab is a grey rectangle behind the control, not a surface.
    setAttribute(Qt::WA_StyledBackground, false);
    setStyleSheet(QStringLiteral("background: transparent;"));
}

void RangeSlider::setValue(double v, bool notify) {
    v = qBound(0.0, v, 1.0);
    if (qAbs(v - v_) > 1e-4) {
        v_ = v;
        if (notify) {
            emit valueChanged(v_);
        }
        update();
    }
}

double RangeSlider::span() const {
    return qMax(1.0, static_cast<double>(width()) - 2.0 * margin_);
}

double RangeSlider::at(qreal x) const {
    return qBound(0.0, (x - margin_) / span(), 1.0);
}

void RangeSlider::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        drag_ = true;
        emit pressed();
        setValue(at(event->position().x()));
    }
}

void RangeSlider::mouseMoveEvent(QMouseEvent* event) {
    hover_ = at(event->position().x());
    if (drag_) {
        setValue(hover_);
    }
    update();
}

void RangeSlider::mouseReleaseEvent(QMouseEvent* event) {
    if (drag_) {
        drag_ = false;
        emit released();
    }
}

void RangeSlider::leaveEvent(QEvent* event) {
    hover_ = -1.0;
    update();
}

void RangeSlider::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const double m = margin_;
    const double w = span();
    const double y = height() / 2.0 - track_ / 2.0;
    const double half = track_ / 2.0;

    paintRounded(p, QRectF(m, y, w, track_), half, w25());
    if (v_ > 0) {
        paintRounded(p, QRectF(m, y, w * v_, track_), half, accent_);
    }
    if (hover_ >= 0.0 && hover_ <= 1.0) {
        paintRounded(p, QRectF(m, y, w * qMin(hover_, v_), track_), half, white(60));
    }

    const double tx = m + w * v_;
    const double cy = height() / 2.0;
    const double r = (underMouse() || drag_) ? 6.0 : 5.0;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 90));
    p.drawEllipse(QPointF(tx, cy + 1), r, r);
    p.setBrush(w95());
    p.drawEllipse(QPointF(tx, cy), r - 1, r - 1);
}

// ── ScrubBar ───────────────────────────────────────────────────────────────

ScrubBar::ScrubBar(QWidget* parent) : RangeSlider(w95(), 4, parent), font_(monoFont(9)) {}

void ScrubBar::setDuration(double d) { duration_ = qMax(0.0, d); }

void ScrubBar::paintEvent(QPaintEvent* event) {
    RangeSlider::paintEvent(event);
    if (duration_ <= 0.0 || !underMouse() || hoverFraction() < 0.0 || hoverFraction() > 1.0) {
        return;
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QString label = formatTime(hoverFraction() * duration_);
    const double tw = textWidth(font_, label) + 12;
    const double th = 17;
    const double m = margin();
    const double cx = m + span() * hoverFraction();
    const double x = qBound(0.0, cx - tw / 2.0, width() - tw);
    paintRounded(p, QRectF(x, 0, tw, th), 4, QColor(0, 0, 0, 205));
    p.setFont(font_);
    p.setPen(w90());
    p.drawText(QRectF(x, 0, tw, th), Qt::AlignCenter, label);
}

// ── VolumeGroup ────────────────────────────────────────────────────────────

namespace {
constexpr int kSpeakerW = 28;  // hit zone of the speaker glyph
constexpr int kTrackX = 30;    // left edge of the track
constexpr double kTrackY = 13;
constexpr double kKnobR = 4;
}  // namespace

VolumeGroup::VolumeGroup(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::CursorShape::PointingHandCursor);
    setMouseTracking(true);
    setFixedSize(kSpeakerW + kTrack, 32);
    setAttribute(Qt::WA_StyledBackground, false);
    setStyleSheet(QStringLiteral("background: transparent;"));
}

void VolumeGroup::setVolume(double v) {
    vol_ = qBound(0.0, v, 1.0);
    update();
}

void VolumeGroup::setMuted(bool m) {
    muted_ = m;
    update();
}

void VolumeGroup::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const double x = event->position().x();
    if (x < kSpeakerW) {
        emit muteClicked();
    } else if (x >= kTrackX && x <= kTrackX + kTrack) {
        drag_ = true;
        emit volumeChanged(qBound(0.0, (x - kTrackX) / kTrack, 1.0));
    }
}

void VolumeGroup::mouseMoveEvent(QMouseEvent* event) {
    if (drag_ && (event->buttons() & Qt::LeftButton)) {
        const double x = event->position().x();
        emit volumeChanged(qBound(0.0, (x - kTrackX) / kTrack, 1.0));
    }
}

void VolumeGroup::mouseReleaseEvent(QMouseEvent* event) {
    drag_ = false;
    QWidget::mouseReleaseEvent(event);
}

void VolumeGroup::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double eff = effective();

    if (eff == 0.0) {
        IconPainter::mute(p, 6, 7, 16);
    } else if (eff < 0.5) {
        IconPainter::volumeLow(p, 6, 7, 16);
    } else {
        IconPainter::volume(p, 6, 7, 16);
    }

    const double w = kTrack;
    paintRounded(p, QRectF(kTrackX, kTrackY, w, 4), 2, w25());
    if (eff > 0) {
        paintRounded(p, QRectF(kTrackX, kTrackY, w * eff, 4), 2, w95());
    }
    p.setPen(Qt::NoPen);
    p.setBrush(w95());
    p.drawEllipse(QPointF(kTrackX + w * eff, kTrackY + 2), kKnobR, kKnobR);
}

}  // namespace shell
}  // namespace mplayer
