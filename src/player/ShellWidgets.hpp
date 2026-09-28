// Player-shell chrome: the self-painting containers and popovers that sit
// around the video surface.
//
// Everything here paints itself instead of leaning on stylesheets, so the shell
// has one rendering path and no theme bleed from the host application.

#pragma once

#include <QColor>
#include <QFrame>
#include <QPair>
#include <QPoint>
#include <QPushButton>
#include <QString>
#include <QVector>
#include <QWidget>

#include "ShellTheme.hpp"

QT_BEGIN_NAMESPACE
class QEnterEvent;
class QLabel;
class QMouseEvent;
class QPainter;
class QTimer;
class QVBoxLayout;
QT_END_NAMESPACE

namespace mplayer {
namespace shell {

class RangeSlider;

/// Plain function pointer so any `IconPainter::` glyph can be handed to a
/// button without a lambda trampoline.
using IconFn = void (*)(QPainter&, qreal, qreal, qreal, const QColor&);

// ── containers ─────────────────────────────────────────────────────────────

/// Rounded container that paints its own background.
class Panel : public QWidget {
    Q_OBJECT
public:
    Panel(const QColor& fill = dock(), double radius = 10.0, const QColor& border = w15(),
          QWidget* parent = nullptr);

    void setFill(const QColor& color);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor fill_;
    double radius_;
    QColor border_;
};

// ── buttons ────────────────────────────────────────────────────────────────

/// Flat square icon button; the glyph is painted, never a pixmap.
class IconButton : public QPushButton {
    Q_OBJECT
public:
    explicit IconButton(IconFn icon, const QString& tooltip = QString(), int size = 30,
                        int glyph = 16, QWidget* parent = nullptr,
                        const QColor& hoverFill = QColor());

    /// A null colour restores the default tint.
    void setTint(const QColor& color);
    void setIcon(IconFn icon);

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    IconFn icon_;
    int glyph_;
    QColor hoverFill_;
    QColor tint_;
    bool hover_ = false;
    bool hasTint_ = false;
};

// ── settings flyout ────────────────────────────────────────────────────────

/// 190 px popover: continuous speed slider, preset list, quality row.
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
    /// Places the popover above-right of a global anchor point and shows it.
    void popupNear(const QPoint& anchorGlobal);

signals:
    void speedSelected(double speed);
    void qualitySelected(int height);

private:
    void pick(double speed);
    void onSlider(double fraction);
    void refresh();

    double rate_ = 1.0;
    QString quality_;
    QLabel* badge_ = nullptr;
    QLabel* qualityLabel_ = nullptr;
    RangeSlider* slider_ = nullptr;
    QVector<QPair<double, QPushButton*>> rows_;

    QWidget* qualitySection_ = nullptr;
    QVBoxLayout* qualityBody_ = nullptr;
    QVector<QualityOption> qualityOptions_;
    QVector<QPushButton*> qualityRows_;
};

// ── overlays ───────────────────────────────────────────────────────────────

/// Small pill showing the live viewer state id.
class StateBadge : public QWidget {
    Q_OBJECT
public:
    explicit StateBadge(QWidget* parent = nullptr);

    void setState(const QString& state);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void resizeToText();

    QString state_;
    QFont font_;
};

/// Live manifest readout (scale, offset, cursor, position…).
class StateHUD : public Panel {
    Q_OBJECT
public:
    explicit StateHUD(QWidget* parent = nullptr);

    void updateData(const QString& state, const QVector<QPair<QString, QString>>& data);

private:
    QLabel* title_ = nullptr;
    QVector<QPair<QLabel*, QLabel*>> rows_;
};

/// Big translucent play/pause disc shown when paused.
class CenterGlyph : public QWidget {
    Q_OBJECT
public:
    enum class Kind { None, Play, Pause };

    explicit CenterGlyph(QWidget* parent = nullptr);

    void setKind(Kind kind);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Kind kind_ = Kind::Play;
};

/// "Original File" — folder glyph plus label, click-emitting.
class PillButton : public QWidget {
    Q_OBJECT
public:
    explicit PillButton(QWidget* parent = nullptr);

signals:
    void clicked();

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QString text_;
    QFont font_;
    bool hover_ = false;
};

/// Transient centred notification.
///
/// Deliberately without a QGraphicsOpacityEffect: widget effects are rendered
/// into the parent's backing store, which does not compose the way fading
/// chrome has to. A deterministic show/raise plus a timed hide is what
/// actually shows up on screen.
class Toast : public Panel {
    Q_OBJECT
public:
    explicit Toast(QWidget* parent = nullptr);

    void flash(const QString& text, int ms = 1900);

private:
    QLabel* label_ = nullptr;
    QTimer* timer_ = nullptr;
};

/// Popup listing the supported input modalities.
class KeyHints : public QFrame {
    Q_OBJECT
public:
    explicit KeyHints(QWidget* parent = nullptr);

    void popupNear(const QPoint& anchorGlobal);

    /// The shortcut table, in display order.
    static QVector<QPair<QString, QString>> shortcutTable();
};

}  // namespace shell
}  // namespace mplayer
