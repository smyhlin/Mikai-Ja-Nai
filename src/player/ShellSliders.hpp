// The two slider families of the bottom dock: the 0..1 RangeSlider (with the
// ScrubBar timeline built on it) and the VolumeGroup, which is a speaker glyph
// that expands into a 45 px track on hover.
//
// Both are painted from scratch in paintEvent, so neither has a stylesheet or a
// style-sheet dependency: every pixel is the palette in ShellTheme.hpp.

#pragma once

#include <QColor>
#include <QPainter>
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
    // Exposed so a probe manifest can drive the control by identity rather than
    // by coordinates; the write is the real setter, so it emits like a drag does.
    Q_PROPERTY(double value READ value WRITE setValue)

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

/// Speaker button plus an always-visible volume track.
class VolumeGroup : public QWidget {
    Q_OBJECT
    Q_PROPERTY(double volume READ effective WRITE setVolume)
    Q_PROPERTY(bool muted READ isMuted WRITE setMuted)

public:
    static constexpr int kTrack = 152;

    explicit VolumeGroup(QWidget* parent = nullptr);

    void setVolume(double v);
    void setMuted(bool m);
    double effective() const { return muted_ ? 0.0 : vol_; }
    bool isMuted() const { return muted_; }

    bool isDragging() const { return drag_; }

signals:
    void volumeChanged(double v);
    void muteClicked();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    double vol_ = 0.7;
    bool muted_ = false;
    bool drag_ = false;
};

}  // namespace shell
}  // namespace mplayer
