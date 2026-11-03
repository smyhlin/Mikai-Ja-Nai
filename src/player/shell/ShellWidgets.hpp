// Player-shell chrome: the containers and popovers that sit around, and over,
// the video surface.
//
// The chrome is painted from scratch instead of leaning on stylesheets, so the
// shell has one rendering path and no theme bleed from the host application.
//
// Controls that stand over the picture are painted by the video surface's own
// painter, in the same paintGL that carried mpv's frame (ChromePaint below):
// they keep the hit target, tooltip, accessible name and layout slot they
// always had and give up only their pixels, because a translucent control
// painting above a QOpenGLWidget is a second raster pass per frame the shell
// cannot stack. The popovers are opaque cards that cover what is under them,
// so they keep painting themselves.

#pragma once

#include <QBrush>
#include <QColor>
#include <QFrame>
#include <QLabel>
#include <QPixmap>
#include <QRect>
#include <utility>
#include <QPoint>
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <QWindow>

#include "ShellTheme.hpp"

QT_BEGIN_NAMESPACE
class QEnterEvent;
class QEvent;
class QMouseEvent;
class QPainter;
class QVBoxLayout;
QT_END_NAMESPACE

namespace mplayer {
namespace shell {

class RangeSlider;

/// Plain function pointer so any `IconPainter::` glyph can be handed to a
/// button without a lambda trampoline.
using IconFn = void (*)(QPainter&, qreal, qreal, qreal, const QColor&);

// ── painted chrome ─────────────────────────────────────────────────────────

/// A chrome widget whose pixels belong to the video surface's painter. The
/// painter arrives translated to this widget's top-left and clipped to its
/// rect. A widget nobody adopts keeps painting its own backing store, which is
/// what the popovers, the settings dialog and the compare window rely on.
class ChromePaint {
public:
    virtual ~ChromePaint() = default;

    virtual void paintChrome(QPainter& painter) = 0;

    bool surfacePainted() const { return surfacePainted_; }
    void setSurfacePainted(bool painted) { surfacePainted_ = painted; }

protected:
    /// The body of a paintEvent: into the widget's own backing store, unless
    /// the surface is drawing this widget.
    void paintOwnBackingStore(QWidget* widget);

private:
    bool surfacePainted_ = false;
};

/// The widget that owns the painter the chrome goes into. `rect` is in the
/// widget's own coordinates; a null rect means all of it.
class ChromeHost {
public:
    virtual ~ChromeHost() = default;

    virtual void chromeDirty(QWidget* widget, const QRect& rect) = 0;
    /// Drops every cached control: a font, palette, language, accent or
    /// device-pixel-ratio change can alter any of them. A move or a resize is
    /// one control's business and goes through `chromeDirty()` instead.
    virtual void chromeDirtyAll() = 0;
    /// The control moved without changing its content: repaint the box it left
    /// and the box it now occupies, but keep its cached pixels - a translation
    /// does not change what the control looks like.
    virtual void chromeMoved(QWidget* widget) = 0;
};

ChromeHost* chromeHostOf(QWidget* widget);

/// What a widget's corner mask currently describes: the shape, in the widget's
/// own LOCAL coordinates, and the radius it was cut at. An empty shape means no
/// mask stands.
struct CornerMask {
    QRect shape;
    int radius = 0;
};

/// Rounds `widget`'s corner by cutting a mask out of it, in `widget`'s LOCAL
/// coordinates - never its parent's, which is what a path built from the parent
/// rect got wrong. A radius of zero or less clears the mask.
///
/// This is the only setMask() call site in the shell, and `state` is why: an
/// identical region is not re-applied, because re-setting one churns the
/// widget's backing store for no new pixels, and the callers run on every input
/// event and every resize.
void applyRoundedMask(QWidget* widget, int radius, CornerMask* state);

/// What an adopted widget calls where it used to call update(): its own
/// update() covers only the part of it that falls outside the picture.
void chromeUpdate(QWidget* widget, const QRect& rect = QRect());

/// The chrome one host has adopted, in paint order: what the surface draws,
/// resolved once where the chrome is built, so painting a frame walks this list
/// instead of the widget tree - no per-frame widget discovery, no snapshot. The
/// order is a depth-first walk in Qt's child order, which is the stacking these
/// widgets had when they painted themselves.
class ChromeList {
public:
    /// Adopts every ChromePaint descendant of `root` (windows and popovers
    /// excepted): the surface draws them, so they stop painting themselves and
    /// `watcher` is put on their state changes. Re-adopting after a structural
    /// change only attaches what is new.
    void adopt(QWidget* root, QObject* watcher);

    bool isEmpty() const { return entries_.isEmpty(); }

    /// Paints the adopted controls inside `clip`, with the painter already
    /// translated so that `root`'s top-left is its origin. `clip` bounds the
    /// work: the surface's paintGL and the shell's own paintEvent each cover
    /// the part of the window the other cannot.
    ///
    /// Controls use separate rasters: composing them into another raster changes
    /// their sampling phase at fractional DPR and can shift unchanged glyphs.
    void paint(QPainter& painter, QWidget* root, const QRect& clip);

    /// Marks one control's cached pixels stale: its own state changed, and the
    /// next paint renders it again into the buffer it already holds. Returns the
    /// box it was last blitted into, in `root` coordinates (empty if it was never
    /// painted), because an adopted control is translucent and Qt does not
    /// repaint the area one vacates - the host has to ask for those pixels.
    QRect invalidate(const QWidget* widget);
    /// The same box, without clearing anything: the control's pixels are still
    /// good, only where they sit on screen changed.
    QRect damage(const QWidget* widget) const;
    /// Marks every cached control stale: a font, palette, language, accent or
    /// device-pixel-ratio change can alter any of them. Buffers are kept.
    void invalidateAll();

private:
    struct Entry {
        QWidget* widget = nullptr;
        ChromePaint* painted = nullptr;
        QPixmap cache;
        /// Where it was last painted, in root coordinates: paint history for
        /// damage, never the cache key.
        QRect paintedBox;
        /// The visible rect in the control's own coordinates - the cache key, so
        /// a pure move reuses the pixels it already holds.
        QRect localBox;
        qreal devicePixelRatio = 0.0;
        quint64 stamp = 0;
        bool valid = false;
    };

    void addSubtree(QWidget* root, QObject* watcher);
    /// Frees one control's buffer: it is gone, or it is over the budget.
    void dropCache(Entry& entry);
    void markStale(Entry& entry);

    QVector<Entry> entries_;

    /// Bumped by invalidateAll(), so every entry is stale without walking them.
    quint64 generation_ = 0;
    qint64 cachedBytes_ = 0;
};

/// Routes an adopted widget's notifications to the surface. Called first in
/// the host's eventFilter; the event is never consumed.
void chromeRepaintEvent(ChromeHost* host, QObject* watched, QEvent* event);

// ── containers ─────────────────────────────────────────────────────────────

/// Rounded container that paints its own background.
class Panel : public QWidget, public ChromePaint {
    Q_OBJECT
public:
    Panel(const QColor& fill = dock(), double radius = 10.0, const QColor& border = w15(),
          QWidget* parent = nullptr);

    void setFill(const QColor& color);

    void paintChrome(QPainter& painter) override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor fill_;
    double radius_;
    QColor border_;
};

// ── buttons ────────────────────────────────────────────────────────────────

/// The shape of an IconButton: a standing fill and the highlight under the
/// pointer. A null fill paints nothing, which is what a bare glyph is - the
/// dock's buttons take the default and the float's own controls are the ones
/// that need a standing shape, radius or border of their own. A brush rather
/// than a colour, because one of them is a gradient.
struct ButtonChrome {
    QBrush fill;
    QColor border;
    QBrush hoverFill;
    QColor hoverBorder;
    double radius = 10.0;
    double borderWidth = 1.0;
};

/// Flat square icon button; the glyph is painted, never a pixmap.
class IconButton : public QPushButton, public ChromePaint {
    Q_OBJECT
public:
    explicit IconButton(IconFn icon, const QString& tooltip = QString(), int size = 30,
                        int glyph = 16, QWidget* parent = nullptr,
                        const QColor& hoverFill = QColor());

    /// A null colour restores the default tint.
    void setTint(const QColor& color);
    void setIcon(IconFn icon);
    /// Replaces the whole look. The default is a glyph that gains a faint
    /// highlight when hovered.
    void setButtonChrome(const ButtonChrome& chrome);
    /// The glyph currently installed. Both the play and the pause triangle are
    /// painted by this class, so the function pointer is the only handle on
    /// which one is up - a grab cannot tell them apart at dock scale.
    IconFn icon() const { return icon_; }

    void paintChrome(QPainter& painter) override;

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    IconFn icon_;
    int glyph_;
    ButtonChrome chrome_;
    QColor tint_;
    bool hover_ = false;
    bool hasTint_ = false;
};

/// A provider-announced skip segment as one pill: the segment's name in the
/// installed language and a forward chevron, in the same fill and corner as the
/// rest of the chrome. It stands over the picture, so its pixels belong to the
/// surface painter like every adopted control; the widget itself is still the
/// hit target, the tooltip and the accessible name.
///
/// The label is supplied already localized by the shell - this class knows
/// nothing about openings or recaps, and nothing about a playhead.
class SkipButton : public QPushButton, public ChromePaint {
    Q_OBJECT
public:
    explicit SkipButton(QWidget* parent = nullptr);

    /// The words this button offers. An unchanged label is not a state change
    /// and repaints nothing; a longer one re-measures the pill.
    void setLabel(const QString& label);
    QString label() const { return label_; }

    void paintChrome(QPainter& painter) override;
    QSize sizeHint() const override;

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QString label_;
    bool hover_ = false;
};

// ── settings flyout ────────────────────────────────────────────────────────

/// Player settings popover: speed, stream quality and system video defaults.
class SettingsFlyout : public QFrame {
    Q_OBJECT
public:
    /// One selectable stream height. `label` is the caller's text, because the
    /// values are mikai's, not any particular provider's naming.
    struct QualityOption {
        int height = 0;
        QString label;
        bool current = false;
    };

    explicit SettingsFlyout(QWidget* parent = nullptr);

    void setRate(double rate);
    void setQuality(const QString& quality);
    /// Rebuilds the quality list. Empty hides the section entirely.
    void setQualityOptions(const QVector<QualityOption>& options);
    /// The display mode the rows tick, as a mplayer::VideoScaleMode value. The
    /// flyout never owns the choice: it reports a click and is told the result.
    void setVideoScaleMode(int mode);
    /// Places the popover above-right of a global anchor point and shows it.
    void popupNear(const QPoint& anchorGlobal);

protected:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

signals:
    void speedSelected(double speed);
    void qualitySelected(int height);
    /// A display mode row was clicked; the value is a mplayer::VideoScaleMode.
    void videoScaleModeSelected(int mode);
    void defaultPlayerRequested();

private:
    void retranslate();
    void pick(double speed);
    void onSlider(double fraction);
    void refresh();
    void resizeToContents();

    double rate_ = 1.0;
    QString quality_;
    QLabel* speedCaption_ = nullptr;
    QLabel* qualityCaption_ = nullptr;
    QLabel* badge_ = nullptr;
    QLabel* qualityLabel_ = nullptr;
    RangeSlider* slider_ = nullptr;
    QVector<std::pair<double, QPushButton*>> rows_;

    QWidget* qualitySection_ = nullptr;
    QVBoxLayout* qualityBody_ = nullptr;
    QVector<QualityOption> qualityOptions_;
    QVector<QPushButton*> qualityRows_;
    QWidget* scaleSection_ = nullptr;
    QLabel* scaleCaption_ = nullptr;
    QVector<QPushButton*> scaleRows_;
    int videoScaleMode_ = 0;
    QPushButton* defaultPlayerButton_ = nullptr;
    QLabel* defaultPlayerLabel_ = nullptr;
};

// ── overlays ───────────────────────────────────────────────────────────────

/// A label whose text is painted by the surface. The string, the font and the
/// colour stay here - the string is what a screen reader reads and what the
/// layout measures by - because a QLabel cannot hand its styled text to a
/// foreign painter, and a pixmap of it would be a snapshot.
class ChromeLabel : public QLabel, public ChromePaint {
    Q_OBJECT
public:
    ChromeLabel(const QString& text, const QFont& font, const QColor& color,
                QWidget* parent = nullptr);

    /// Shadows QLabel's so the string and its pixels move together: a widget
    /// update no longer repaints anything. An unchanged string is not a state
    /// change and must not repaint the picture - the clock writes every tick.
    void setText(const QString& text);

    void setChromeColor(const QColor& color);

    void paintChrome(QPainter& painter) override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor color_;
};

/// Big translucent play/pause disc shown when paused.
class CenterGlyph : public QWidget, public ChromePaint {
    Q_OBJECT
public:
    enum class Kind { None, Play, Pause };

    explicit CenterGlyph(QWidget* parent = nullptr);

    void setKind(Kind kind);
    void setLoading(bool loading);
    bool isLoading() const { return loading_; }

    void paintChrome(QPainter& painter) override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Kind kind_ = Kind::Play;
    QTimer spinnerTimer_;
    qreal spinnerAngle_ = 0.0;
    bool loading_ = false;
};

/// Transient centred notification.
///
/// Deliberately without a QGraphicsOpacityEffect: widget effects are rendered
/// into the parent's backing store, which does not compose the way chrome over
/// the picture has to. A deterministic show/raise plus a timed hide is what
/// actually shows up on screen.
class Toast : public Panel {
    Q_OBJECT
public:
    explicit Toast(QWidget* parent = nullptr);

    void flash(const QString& text, int ms = 1900);

private:
    ChromeLabel* label_ = nullptr;
    QTimer* timer_ = nullptr;
};

/// Popup listing the supported input modalities.
class KeyHints : public QFrame {
    Q_OBJECT
public:
    explicit KeyHints(QWidget* parent = nullptr);

    void popupNear(const QPoint& anchorGlobal);

    /// The shortcut table, in display order.
    static QVector<std::pair<QString, QString>> shortcutTable();

protected:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void retranslate();

    QLabel* caption_ = nullptr;
    QVector<QLabel*> descriptions_;
};

}  // namespace shell
}  // namespace mplayer
