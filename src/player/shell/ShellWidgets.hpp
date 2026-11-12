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
// cannot stack. The popovers are opaque cards, but they still route their own
// pixels through the same interface: a host that adopts them gives the card one
// paint path with the rest of the chrome, and a card nobody adopts falls back to
// its own backing store.

#pragma once

#include <QBrush>
#include <QColor>
#include <QFrame>
#include <QLabel>
#include <QRect>
#include <utility>
#include <QPoint>
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

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

class ChromeLabel;
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
    /// Adopts a control that did not exist when the host last walked its tree -
    /// a menu row created after the panel is already up. A control built in a
    /// constructor is adopted by the host's own walk and never needs this.
    virtual void chromeAdopt(QWidget* widget) = 0;
    /// Puts `widget` and everything it owns at the end of the paint order. A
    /// panel that appears has to be drawn over the chrome that was adopted
    /// after it - the dock, for one - which Qt z-order gave for free.
    virtual void chromeRaise(QWidget* widget) = 0;
};

ChromeHost* chromeHostOf(QWidget* widget);

/// What an adopted widget calls where it used to call update(): its own
/// update() covers only the part of it that falls outside the picture.
void chromeUpdate(QWidget* widget, const QRect& rect = QRect());

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

/// One row of a list the player shows over the picture. Painted by the
/// surface, never by the widget's own backing store: a list drawn as ordinary
/// Qt widgets over the GL surface makes the compositor repaint the picture
/// every time a row appears, which is what showed as a black flash.
class ChromeRow : public QPushButton, public ChromePaint {
    Q_OBJECT
public:
    enum class Role {
        /// A choice the viewer can pick: muted ink on a faint wash.
        Choice,
        /// The row the core is already using: white ink on the accent wash.
        Current,
        /// Shown but unchoosable: muted ink, no hover, no highlight.
        Disabled,
    };

    explicit ChromeRow(const QString& text, QWidget* parent = nullptr);

    void setRole(Role role);
    /// The keyboard cursor, which outranks the row's standing look.
    void setHighlighted(bool highlighted);
    /// Draws the chevron that says this row opens another level.
    void setSubmenu(bool submenu);
    /// Text inset, matching the padding the rows were laid out with.
    void setRowPadding(int horizontal);

    void paintChrome(QPainter& painter) override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    Role role_ = Role::Choice;
    bool highlighted_ = false;
    bool submenu_ = false;
    int padX_ = 10;
};

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
    bool useThemeRadius = false;
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
class SettingsFlyout : public QFrame, public ChromePaint {
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

    void paintChrome(QPainter& painter) override;

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
    ChromeLabel* speedCaption_ = nullptr;
    ChromeLabel* qualityCaption_ = nullptr;
    ChromeLabel* badge_ = nullptr;
    ChromeLabel* qualityLabel_ = nullptr;
    RangeSlider* slider_ = nullptr;
    QVector<std::pair<double, ChromeRow*>> rows_;

    QWidget* qualitySection_ = nullptr;
    QVBoxLayout* qualityBody_ = nullptr;
    QVector<QualityOption> qualityOptions_;
    QVector<ChromeRow*> qualityRows_;
    QWidget* scaleSection_ = nullptr;
    ChromeLabel* scaleCaption_ = nullptr;
    QVector<ChromeRow*> scaleRows_;
    /// Hairlines between the sections; the parent's painter draws them, so a
    /// line never becomes a widget that paints its own strip over the picture.
    QVector<QWidget*> separators_;
    int videoScaleMode_ = 0;
    ChromeRow* defaultPlayerButton_ = nullptr;
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

    /// Insets the drawn text inside the widget, for the panels whose rows and
    /// headings were laid out with a padding a QSS rule used to give.
    void setChromePadding(int horizontal, int vertical = 0);

    /// Wraps the text inside the box instead of drawing one line. The label has
    /// already been measured with hasHeightForWidth by then.
    void setChromeWordWrap(bool wrap);

    void paintChrome(QPainter& painter) override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor color_;
    int padX_ = 0;
    int padY_ = 0;
    bool wrap_ = false;
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
class KeyHints : public QFrame, public ChromePaint {
    Q_OBJECT
public:
    explicit KeyHints(QWidget* parent = nullptr);

    void popupNear(const QPoint& anchorGlobal);

    /// The shortcut table, in display order.
    static QVector<std::pair<QString, QString>> shortcutTable();

    void paintChrome(QPainter& painter) override;

protected:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void retranslate();

    /// The rows live in a plain layout widget: no scroll area, so nothing in
    /// this card paints a viewport over the picture.
    ChromeLabel* caption_ = nullptr;
    QWidget* content_ = nullptr;
    /// The key columns, whose accent-wash keycap the card's own painter draws.
    QVector<ChromeLabel*> keys_;
    QVector<ChromeLabel*> descriptions_;
};

}  // namespace shell
}  // namespace mplayer
