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
        chromeUpdate(this);
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
    // A move with the left button up is not scrubbing: the release happened
    // where this widget could not see it. Ending the drag here, without taking
    // the pointer's position as a value, is what keeps an ordinary hover from
    // seeking a bar the user already let go of.
    if (drag_) {
        if ((event->buttons() & Qt::LeftButton) != 0) {
            setValue(hover_);
        } else {
            drag_ = false;
            emit released();
        }
    }
    chromeUpdate(this);
}

void RangeSlider::mouseReleaseEvent(QMouseEvent* event) {
    // Another button's release is not the end of a left drag, and the pointer
    // can be released somewhere its last move never reported - so the value is
    // taken from the release itself rather than from the stale hover fraction.
    if (event->button() != Qt::LeftButton || !drag_) {
        return;
    }
    hover_ = at(event->position().x());
    // Sampled while the drag still reads true, so a consumer listening to
    // valueChanged sees the same drag released() is about to end.
    setValue(hover_);
    drag_ = false;
    emit released();
}

void RangeSlider::leaveEvent(QEvent* event) {
    hover_ = -1.0;
    chromeUpdate(this);
}

void RangeSlider::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

void RangeSlider::paintChrome(QPainter& p) {
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

void ScrubBar::paintChrome(QPainter& p) {
    RangeSlider::paintChrome(p);
    if (duration_ <= 0.0 || !underMouse() || hoverFraction() < 0.0 || hoverFraction() > 1.0) {
        return;
    }

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

// ── VolumeTrack ────────────────────────────────────────────────────────────

namespace {
// The disc is this wide. The pad is one disc-radius plus a pixel, on both
// ends, so a knob sitting on 0 or on 1 is still entirely inside the widget.
constexpr int kKnobR = 5;
constexpr int kKnobRHot = 6;
constexpr int kEndPad = kKnobRHot + 2;
constexpr int kGroove = 4;
constexpr int kSpeakerW = 28;
constexpr int kSpeakerGap = 6;
}  // namespace

VolumeTrack::VolumeTrack(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setCursor(Qt::CursorShape::PointingHandCursor);
    setMinimumHeight(kKnobRHot * 2 + 4);
    setAttribute(Qt::WA_StyledBackground, false);
    setStyleSheet(QStringLiteral("background: transparent;"));
}

void VolumeTrack::setValue(double v, bool notify) {
    v = qBound(0.0, v, 1.0);
    if (qAbs(v - v_) <= 1e-4) {
        return;
    }
    v_ = v;
    if (notify) {
        emit valueChanged(v_);
    }
    chromeUpdate(this);
}

double VolumeTrack::span() const {
    return qMax(1.0, static_cast<double>(width()) - 2.0 * kEndPad);
}

double VolumeTrack::at(qreal x) const {
    return qBound(0.0, (x - kEndPad) / span(), 1.0);
}

void VolumeTrack::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    drag_ = true;
    emit pressed();
    setValue(at(event->position().x()), true);
}

void VolumeTrack::mouseMoveEvent(QMouseEvent* event) {
    // The same lost-release rule: a move without the left button ends a drag
    // whose release never arrived here, and the level stays where the last
    // genuine move put it. A held left button still tracks the pointer.
    if (drag_) {
        if ((event->buttons() & Qt::LeftButton) != 0) {
            setValue(at(event->position().x()), true);
        } else {
            drag_ = false;
            emit released();
        }
    }
    chromeUpdate(this);
}

void VolumeTrack::mouseReleaseEvent(QMouseEvent* event) {
    // Same ownership rule as the timeline: only the button that started the
    // drag can end it, and the level settles on the release position.
    if (event->button() != Qt::LeftButton || !drag_) {
        return;
    }
    setValue(at(event->position().x()), true);
    drag_ = false;
    emit released();
    chromeUpdate(this);
}

void VolumeTrack::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    chromeUpdate(this);
}

void VolumeTrack::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

void VolumeTrack::paintChrome(QPainter& p) {
    const double travel = span();
    const double y = height() / 2.0 - kGroove / 2.0;
    const double cy = height() / 2.0;
    const double x = kEndPad + travel * v_;
    const double r = (underMouse() || drag_) ? kKnobRHot : kKnobR;

    paintRounded(p, QRectF(kEndPad, y, travel, kGroove), kGroove / 2.0, w25());
    if (v_ > 0.0) {
        paintRounded(p, QRectF(kEndPad, y, travel * v_, kGroove), kGroove / 2.0, w95());
    }

    // Shadow, then a dark ring, then the face. The fill is the same white as
    // the face, so without the ring a full track swallows the knob.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 110));
    p.drawEllipse(QPointF(x, cy + 1.0), r, r);
    p.setPen(QPen(QColor(0, 0, 0, 150), 1.0));
    p.setBrush(w95());
    p.drawEllipse(QPointF(x, cy), r - 0.5, r - 0.5);
}

// ── VolumeGroup ────────────────────────────────────────────────────────────

VolumeGroup::VolumeGroup(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::CursorShape::PointingHandCursor);
    setMouseTracking(true);
    setFixedSize(kSpeakerW + kSpeakerGap + kTrack, 32);
    setAttribute(Qt::WA_StyledBackground, false);
    setStyleSheet(QStringLiteral("background: transparent;"));
    track_ = new VolumeTrack(this);
    track_->setGeometry(kSpeakerW + kSpeakerGap, 0, kTrack, height());
    track_->setValueSilent(effective());
    connect(track_, &VolumeTrack::valueChanged, this, [this](double v) {
        vol_ = v;
        emit volumeChanged(vol_);
        chromeUpdate(this);
    });
}

void VolumeGroup::showLevel() {
    if (track_ != nullptr) {
        track_->setValueSilent(effective());
    }
}

void VolumeGroup::setVolume(double v) {
    const double next = qBound(0.0, v, 1.0);
    // Emits like a drag, so a programmatic set reaches the core the same way a
    // pointer does instead of silently rewriting only this widget. The dead zone
    // is what makes that safe in the other direction too: the shell pushes its
    // own volume back down here, and without it every push would echo straight
    // into onVolume and on into mpv.
    if (qAbs(next - vol_) <= 1e-4) {
        showLevel();
        return;
    }
    vol_ = next;
    showLevel();
    emit volumeChanged(vol_);
    chromeUpdate(this);
}

// Silent by design, unlike setVolume: muted_ travels shell -> widget only, and
// the widget's own contribution travels back as the muteClicked *request*, which
// the shell answers by toggling. Emitting that here would ask for the opposite
// of what was just set.
void VolumeGroup::setMuted(bool m) {
    if (muted_ == m) {
        return;
    }
    muted_ = m;
    showLevel();
    chromeUpdate(this);
}

bool VolumeGroup::isDragging() const {
    return track_ != nullptr && track_->isDragging();
}

void VolumeGroup::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && event->position().x() < kSpeakerW) {
        emit muteClicked();
    }
}

void VolumeGroup::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

void VolumeGroup::paintChrome(QPainter& p) {
    const double eff = effective();
    const int iconY = (height() - 16) / 2;
    if (eff == 0.0) {
        IconPainter::mute(p, 6, iconY, 16);
    } else if (eff < 0.5) {
        IconPainter::volumeLow(p, 6, iconY, 16);
    } else {
        IconPainter::volume(p, 6, iconY, 16);
    }
}

}  // namespace shell
}  // namespace mplayer
