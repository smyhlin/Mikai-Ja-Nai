// The two slider families of the bottom dock: the 0..1 RangeSlider (with the
// ScrubBar timeline built on it) and the VolumeGroup, a speaker glyph beside
// an always-visible track.
//
// Painted from scratch, so there is no stylesheet: every pixel is the palette
// in ShellTheme.hpp. The volume knob is inset by its own radius, so a level of
// 0 or 1 still draws the whole disc.
//
// These are ChromePaint widgets: the dock draws them into the video surface's
// painter, and paintChrome() - not paintEvent - is where their pixels come
// from. A host that never adopts them (the settings flyout's speed slider) gets
// the same pixels through the default paintEvent.

#pragma once

#include <QColor>
#include <QPainter>
#include <QPoint>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

#include "ShellTheme.hpp"
#include "ShellWidgets.hpp"

namespace mplayer {
namespace shell {

/// Normalised (0..1) horizontal slider with hover preview.
///
/// `span()`/`at()` are the only geometry hooks; subclasses that map the fraction
/// to a different quantity override those and inherit press/move behaviour.
class RangeSlider : public QWidget, public ChromePaint {
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

    /// Replaces the colour the filled part is painted with. A slider that lives
    /// in a long-lived flyout follows setAccent() through this instead of
    /// keeping the colour it was constructed with.
    void setAccent(const QColor& accent);

    void paintChrome(QPainter& painter) override;

signals:
    void valueChanged(double v);
    void pressed();
    void released();

protected:
    /// The colour of the filled part. The base returns the stored accent; a
    /// timeline overrides it to read the pushed token at paint time.
    virtual QColor playedColor() const { return accent_; }

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
    /// Groove thickness in px; a subclass drawing around the track needs it.
    int trackThickness() const { return track_; }
    /// Drops the hover fraction. Subclasses call it when the pointer is no
    /// longer meaningfully over the control but no leave event will follow.
    void clearHover() {
        hover_ = -1.0;
        chromeUpdate(this);
    }
    /// Records the hover fraction for a pointer that arrived without a move -
    /// a press lands where no move has been reported yet.
    void setHoverFraction(double fraction) { hover_ = fraction; }

private:
    double v_ = 0.0;
    double hover_ = -1.0;
    QColor accent_;
    int track_ = 4;
    int margin_ = 7;
    bool drag_ = false;
};

/// Timeline with the provider's opening / ending / recap ranges tinted on the
/// track.
///
/// The bar itself only knows seconds and fractions, and it paints no hover card:
/// the reading under the pointer is a signal, so whoever owns the preview (frame
/// and segment name over the picture) draws it exactly once. A consumer that
/// only wants the ranges drawn has to do nothing but hand over the segment list.
class ScrubBar : public RangeSlider {
    Q_OBJECT

public:
    explicit ScrubBar(QWidget* parent = nullptr);

    void setDuration(double d);
    double duration() const { return duration_; }

    /// Fraction of the timeline to seconds.
    double timeAt(double frac) const { return frac * duration_; }

    /// Provider segments (`type`, `start`, `end` in seconds), the same shape the
    /// shell validates and forwards. Entries that are unreadable, non-finite,
    /// reversed or zero-length are ignored, and a range is clipped to the known
    /// duration at paint and label time - so a list that arrives before the
    /// media is measured still renders once the length is known.
    void setSkipSegments(const QVector<QVariantMap>& segments);

    /// Seconds under the pointer, or -1 when the pointer is off the bar or the
    /// duration is not known yet.
    double hoverTime() const;
    bool isHovering() const { return hoverTime() >= 0.0; }

    /// Every segment covering `seconds`, named in the UI language and joined in
    /// list order - overlaps keep both names rather than the last one winning.
    /// Empty when the instant is outside every range. Half-open like the clock
    /// that offers a skip: `start <= seconds < end`.
    QString segmentLabelAt(double seconds) const;

    /// The UI name of one provider segment type; an unknown identifier is shown
    /// as the provider's own word rather than a name invented for it.
    static QString segmentTypeLabel(const QString& type);

    /// RangeSlider's bar with the segment ranges painted behind its groove.
    void paintChrome(QPainter& painter) override;

signals:
    /// The pointer is over the timeline at `seconds`, in widget-global
    /// coordinates for a popup to anchor on. Emitted for a move and for the
    /// press that seeks, and not while the duration is unknown - there is no
    /// time under the pointer to report.
    void hoverTimeChanged(double seconds, const QPoint& globalPosition);
    /// The pointer left the timeline.
    void hoverLeft();

protected:
    /// The played part is the pushed accent, read when the bar paints: the
    /// scrub control outlives an accent change and must not keep the colour it
    /// was built with.
    QColor playedColor() const override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    /// The one place the hover signal is derived from the current fraction.
    void reportHover(const QPoint& globalPosition);
    /// x position (px, widget-local) of a time in seconds, clamped to the track.
    double xAtTime(double seconds) const;
    /// The segment ranges as pixel columns, unioned so an overlap is one tint.
    const QVector<QRect>& segmentBands() const;

    double duration_ = 0.0;
    QVector<QVariantMap> segments_;
    mutable QVector<QRect> bands_;
    mutable QSize bandsSize_;
    mutable bool bandsDirty_ = true;
};

/// A horizontal level. The knob travels inside the widget, so at 0 and at full
/// the whole disc is painted, not the half that used to fall past the edge.
class VolumeTrack : public QWidget, public ChromePaint {
    Q_OBJECT
    Q_PROPERTY(double volume READ value WRITE setValue)

public:
    explicit VolumeTrack(QWidget* parent = nullptr);

    double value() const { return v_; }
    void setValue(double v) { setValue(v, true); }
    void setValueSilent(double v) { setValue(v, false); }
    bool isDragging() const { return drag_; }

    void paintChrome(QPainter& painter) override;

signals:
    void valueChanged(double v);
    void pressed();
    void released();

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void setValue(double v, bool notify);
    double span() const;
    double at(qreal x) const;

    double v_ = 0.7;
    bool drag_ = false;
};

/// Speaker button plus an always-visible volume track.
class VolumeGroup : public QWidget, public ChromePaint {
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

    bool isDragging() const;

    void paintChrome(QPainter& painter) override;

signals:
    void volumeChanged(double v);
    void muteClicked();

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void showLevel();

    VolumeTrack* track_ = nullptr;
    double vol_ = 0.7;
    bool muted_ = false;
};

}  // namespace shell
}  // namespace mplayer
