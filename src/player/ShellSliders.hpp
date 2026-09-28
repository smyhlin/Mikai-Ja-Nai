// The two slider families of the bottom dock: the 0..1 RangeSlider (with the
// ScrubBar timeline built on it) and the VolumeGroup, which is a speaker glyph
// that expands into a 45 px track on hover.
//
// Both are painted from scratch in paintEvent, so neither has a stylesheet or a
// style-sheet dependency: every pixel is the palette in ShellTheme.hpp.

#pragma once

#include <QColor>
#include <QPainter>
#include <QVariantAnimation>
#include <QWidget>

#include "ShellTheme.hpp"

namespace mplayer {
namespace shell {

/// Normalised (0..1) horizontal slider with hover preview.
///
/// `span()`/`at()` are the only geometry hooks; subclasses that map the fraction
/// to a different quantity override those and inherit press/move behaviour.
class RangeSlider : public QWidget {
    Q_OBJECT

public:
    explicit RangeSlider(const QColor& accent = w95(), int track = 4, QWidget* parent = nullptr,
                         int margin = 7);

    double value() const { return v_; }

    /// Clamps to 0..1 and emits valueChanged past a 1e-4 dead zone.
    void setValue(double v, bool notify = true);
    void setValueSilent(double v) { setValue(v, false); }

    bool isDragging() const { return drag_; }

signals:
    void valueChanged(double v);
    void pressed();
    void released();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

    /// Usable track length in px; never smaller than 1 so a collapsed widget
    /// cannot divide by zero.
    virtual double span() const;
    /// x position (px, widget-local) to clamped 0..1 fraction.
    virtual double at(qreal x) const;

    double hoverFraction() const { return hover_; }
    int margin() const { return margin_; }

private:
    double v_ = 0.0;
    double hover_ = -1.0;
    QColor accent_;
    int track_ = 4;
    int margin_ = 7;
    bool drag_ = false;
};

/// Timeline with a mono timestamp tooltip on hover.
class ScrubBar : public RangeSlider {
    Q_OBJECT

public:
    explicit ScrubBar(QWidget* parent = nullptr);

    void setDuration(double d);
    double duration() const { return duration_; }

    /// Fraction of the timeline to seconds.
    double timeAt(double frac) const { return frac * duration_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    double duration_ = 0.0;
    QFont font_;
};

/// Speaker button + slider that expands on hover (45 px track).
class VolumeGroup : public QWidget {
    Q_OBJECT

public:
    static constexpr int kTrack = 45;

    explicit VolumeGroup(QWidget* parent = nullptr);

    void setVolume(double v);
    void setMuted(bool m);
    double effective() const { return muted_ ? 0.0 : vol_; }

    bool isDragging() const { return drag_; }

signals:
    void volumeChanged(double v);
    void muteClicked();

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void onAnimated(const QVariant& value);
    void animateTo(double target);

    double vol_ = 0.7;
    bool muted_ = false;
    double t_ = 0.0;  // 0 = collapsed, 1 = expanded
    bool drag_ = false;
    QVariantAnimation anim_;
};

}  // namespace shell
}  // namespace mplayer
