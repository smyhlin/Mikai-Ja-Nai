#include "ClipDialog.hpp"

#include "ClipExport.hpp"
#include "FramePreviewService.hpp"
#include "ShellTheme.hpp"
#include "ShellWidgets.hpp"
#include "../core/MediaTracks.hpp"
#include "../core/VideoSession.hpp"
#include "../core/VideoSurface.hpp"

#include <QCloseEvent>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QTimer>
#include <QPushButton>
#include <QResizeEvent>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace mplayer::shell {
namespace {

class ClipTranslation {
    Q_DECLARE_TR_FUNCTIONS(mplayer::shell::ClipDialog)
};

// Frozen +/-2 min window around the pause/click moment.
constexpr double kWindowRadiusSeconds = 120.0;
constexpr int kTileHeight = 64;
constexpr int kTileWidth = kTileHeight * 16 / 9;  // 113
constexpr int kStripHeight = 72;
constexpr int kTileWatchdogMs = 1200;
constexpr int kTileCacheMax = 128;
constexpr double kMinClipSeconds = 0.1;
constexpr int kHandleHitHalfWidth = 12;  // 24px hit area, surfaces.md

// ---------------------------------------------------------------------------
// Design tokens — the ONLY place colour / radius / spacing values live.
// Neutrals are the skill's Mikai tokens; accent and radii are live user
// preferences (accent(), cornerRadius()). Nothing below this struct
// contains a colour, radius or spacing literal.
// ---------------------------------------------------------------------------
struct ClipTokens {
    QColor bg;             // dialog bed
    QColor surface;        // cards and panels
    QColor surfaceRaised;  // interactive surfaces
    QColor fg;
    QColor muted;
    QColor subtle;
    QColor border;
    QColor borderStrong;
    QColor accent;    // live, user-customizable
    QColor accentFg;  // inkOn(accent()), contrast-checked by the theme
    int cardRadius;   // cornerRadius() + 4  (concentric)
    int innerRadius;  // cornerRadius(): controls, buttons, progress
    /// A chip or pill is round by definition and keeps height/2; only the
    /// rectangular controls follow the user's global corner radius.
    int chipRadius = 17;   // 34 px preset chip (32 content + 2 border)
    int qualityRadius = 14;  // 28 px quality chip
    int pillRadius = 18;     // 36 px quality pill
    /// One pill width for *all* presets: a fixed width is what makes a row of
    /// 1- and 2-digit labels read as a control instead of loose text.
    int presetChipWidth = 52;
};

ClipTokens clipTokens() {
    ClipTokens t;
    t.bg = QColor(13, 14, 19);
    t.surface = QColor(18, 20, 26);
    t.surfaceRaised = QColor(26, 29, 38);
    t.fg = QColor(244, 244, 245);
    t.muted = QColor(161, 161, 170);
    t.subtle = QColor(113, 113, 122);
    t.border = QColor(255, 255, 255, 20);       // white at 8%
    t.borderStrong = QColor(255, 255, 255, 38); // white at 15%
    t.accent = accent();
    t.accentFg = inkOn(t.accent);
    const int base = qRound(cornerRadius() > 0.0 ? cornerRadius() : 8.0);
    t.innerRadius = base;
    t.cardRadius = base + 4;
    return t;
}

QString cssRgba(const QColor& c) {
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(c.red()).arg(c.green()).arg(c.blue())
        .arg(QString::number(c.alphaF(), 'f', 2));
}

double encodeBitsPerSecond(int height) {
    if (height >= 1080) return 6.2e6;
    if (height >= 720) return 3.1e6;
    return 1.5e6;
}

QString formatSize(qint64 bytes) {
    const double mib = double(bytes) / (1024.0 * 1024.0);
    if (mib >= 1024.0) return ClipTranslation::tr("≈ %1 GB").arg(QLocale().toString(mib / 1024.0, 'f', 2));
    if (mib >= 9.95) return ClipTranslation::tr("≈ %1 MB").arg(QLocale().toString(mib, 'f', 0));
    return ClipTranslation::tr("≈ %1 MB").arg(QLocale().toString(mib, 'f', 1));
}

QString formatClipTime(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    const qint64 tenths = qint64(std::llround(seconds * 10.0));
    const qint64 totalSeconds = tenths / 10;
    const int tenth = int(tenths % 10);
    const qint64 minutes = totalSeconds / 60;
    const int secs = int(totalSeconds % 60);
    if (minutes >= 60) {
        return QStringLiteral("%1:%2:%3.%4")
            .arg(minutes / 60)
            .arg(minutes % 60, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'))
            .arg(tenth);
    }
    return QStringLiteral("%1:%2.%3").arg(minutes).arg(secs, 2, 10, QChar('0')).arg(tenth);
}

// Tabular numerals (typography.md): live counters must not jitter.
void applyTabularNumerals(QLabel* label) {
    if (!label) return;
    QFont font = label->font();
    font.setFeature(QFont::Tag("tnum"), 1);
    label->setFont(font);
}

// Snap a rect to physical device pixels (surfaces.md / invariant 6).
QRectF deviceAlignedRect(const QRectF& rect, qreal dpr) {
    if (!(dpr > 0.0)) return rect;
    return QRectF(qRound(rect.x() * dpr) / dpr, qRound(rect.y() * dpr) / dpr,
                  qRound(rect.width() * dpr) / dpr, qRound(rect.height() * dpr) / dpr);
}

QString clipDialogStyleSheet() {
    const ClipTokens t = clipTokens();
    // Two font sizes only (12 body, 14 heading). Radii and spacing come
    // from tokens; the accent and its ink come from the live theme.
    return QStringLiteral(
               "QDialog#clipDialog { background-color: %1; color: %2; }"
               "QFrame#clipCard { background-color: %3;"
               "  border: 1px solid %4; border-radius: %5px; }"
               "QLabel { color: %6; background: transparent; font-size: 12px; }"
               "QLabel#clipInfoTop { color: %2; font-size: 14px; font-weight: 600; padding-left: 0px; }"
               "QLabel#clipInfoBottom { color: %6; font-size: 12px; padding-left: 8px; }"
               "QLabel#clipStatus { color: %7; font-size: 12px; }"
               "QPushButton#clipGhost { background: transparent; color: %8; border: 1px solid %4;"
               "  border-radius: %15px; padding: 0px 12px; min-height: 32px; font-size: 12px;"
               "  font-weight: 500; }"
               "QPushButton#clipGhost:hover { background: %9; color: %2; }"
               "QPushButton#clipGhost:disabled { color: %7; }"
               "QPushButton#clipPreset { background: transparent; color: %6; border: 1px solid %4;"
               "  border-radius: %16px; padding: 0px 0px; min-width: %20px; min-height: 32px;"
               "  font-size: 12px; font-weight: 600; }"
               "QPushButton#clipPreset:hover { color: %2; background: %9; }"
               "QPushButton#clipPreset:checked { background: %10; color: %2; border-color: %11; }"
               "QPushButton#clipDownload { background: %12; color: %13; border: none;"
               "  border-radius: %15px; padding: 0px 16px 0px 14px; min-height: 40px;"
               "  font-size: 12px; font-weight: 600; }"
               "QPushButton#clipDownload:hover { background: %14; }"
               "QPushButton#clipDownload:disabled { background: %3; color: %7; }"
               "QPushButton#clipQuality { background: transparent; color: %6; border: none;"
               "  border-radius: %17px; padding: 0px 12px; min-height: 28px; font-size: 12px;"
               "  font-weight: 600; }"
               "QPushButton#clipQuality:checked { background: %10; color: %2; }"
               "QPushButton#clipQuality:hover:!checked { color: %2; }"
               "QFrame#clipQualityPill { background: %3; border-radius: %18px; }"
               "QProgressBar#clipProgress { background: %3; border: none; border-radius: %19px;"
               "  min-height: 4px; max-height: 4px; }"
               "QProgressBar#clipProgress::chunk { background: %12; border-radius: %19px; }")
        .arg(t.bg.name(QColor::HexRgb), t.fg.name(QColor::HexRgb), t.surface.name(QColor::HexRgb),
             cssRgba(t.border), QString::number(t.cardRadius), t.muted.name(QColor::HexRgb),
             t.subtle.name(QColor::HexRgb), t.fg.name(QColor::HexRgb),
             cssRgba(QColor(255, 255, 255, 20)), t.surfaceRaised.name(QColor::HexRgb),
             cssRgba(t.borderStrong), t.accent.name(QColor::HexRgb), t.accentFg.name(QColor::HexRgb),
             t.accent.lighter(115).name(QColor::HexRgb),
             // %15..%19: the radii the tokens now own. Controls follow the
             // user's global corner radius; chips stay pill by definition.
             QString::number(t.innerRadius), QString::number(t.chipRadius),
             QString::number(t.qualityRadius), QString::number(t.pillRadius),
             QString::number(qMax(2, t.innerRadius / 2)),
             // %20: one width for every preset, so "5s" and "60s" are the same
             // pill and the row stops looking ragged.
             QString::number(t.presetChipWidth));
}

// ---------------------------------------------------------------------------
// Vector icons — monochrome QPainter paths, no emoji / text glyphs.
// The play triangle is shifted +2px right (optical alignment).
// ---------------------------------------------------------------------------
enum class ClipIconKind { Play, Pause, Close, Download, SetStart, SetEnd, Volume, Mute };

void drawClipIcon(QPainter* p, ClipIconKind kind, const QRectF& rect, const QColor& color) {
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->setBrush(Qt::NoBrush);
    const QPointF c = rect.center();
    switch (kind) {
        case ClipIconKind::Play: {
            // Triangle, centre of mass sits left of its box — shift +2px.
            QPainterPath path;
            path.moveTo(c.x() - 4.0 + 2.0, c.y() - 7.0);
            path.lineTo(c.x() + 7.0 + 2.0, c.y());
            path.lineTo(c.x() - 4.0 + 2.0, c.y() + 7.0);
            path.closeSubpath();
            p->setBrush(color);
            p->setPen(Qt::NoPen);
            p->drawPath(path);
            break;
        }
        case ClipIconKind::Pause:
            p->setBrush(color);
            p->setPen(Qt::NoPen);
            p->drawRoundedRect(QRectF(c.x() - 6.0, c.y() - 7.0, 4.0, 14.0), 1.0, 1.0);
            p->drawRoundedRect(QRectF(c.x() + 2.0, c.y() - 7.0, 4.0, 14.0), 1.0, 1.0);
            break;
        case ClipIconKind::Close:
            p->drawLine(QPointF(c.x() - 6.0, c.y() - 6.0), QPointF(c.x() + 6.0, c.y() + 6.0));
            p->drawLine(QPointF(c.x() + 6.0, c.y() - 6.0), QPointF(c.x() - 6.0, c.y() + 6.0));
            break;
        case ClipIconKind::Download:
            p->drawLine(QPointF(c.x(), c.y() - 7.0), QPointF(c.x(), c.y() + 5.0));
            p->drawLine(QPointF(c.x() - 5.0, c.y()), QPointF(c.x(), c.y() + 5.0));
            p->drawLine(QPointF(c.x() + 5.0, c.y()), QPointF(c.x(), c.y() + 5.0));
            p->drawLine(QPointF(c.x() - 8.0, c.y() + 8.0), QPointF(c.x() + 8.0, c.y() + 8.0));
            break;
        case ClipIconKind::SetStart:
            p->drawLine(QPointF(c.x() - 2.0, c.y() - 7.0), QPointF(c.x() - 8.0, c.y() - 7.0));
            p->drawLine(QPointF(c.x() - 8.0, c.y() - 7.0), QPointF(c.x() - 8.0, c.y() + 7.0));
            p->drawLine(QPointF(c.x() - 8.0, c.y() + 7.0), QPointF(c.x() - 2.0, c.y() + 7.0));
            p->drawLine(QPointF(c.x() + 6.0, c.y() - 7.0), QPointF(c.x() + 6.0, c.y() + 7.0));
            break;
        case ClipIconKind::SetEnd:
            p->drawLine(QPointF(c.x() + 2.0, c.y() - 7.0), QPointF(c.x() + 8.0, c.y() - 7.0));
            p->drawLine(QPointF(c.x() + 8.0, c.y() - 7.0), QPointF(c.x() + 8.0, c.y() + 7.0));
            p->drawLine(QPointF(c.x() + 8.0, c.y() + 7.0), QPointF(c.x() + 2.0, c.y() + 7.0));
            p->drawLine(QPointF(c.x() - 6.0, c.y() - 7.0), QPointF(c.x() - 6.0, c.y() + 7.0));
            break;
        case ClipIconKind::Volume:
        case ClipIconKind::Mute: {
            // Speaker cone, filled like the transport glyphs so the mute
            // control reads at the same weight as play.
            QPainterPath cone;
            cone.moveTo(c.x() - 7.0, c.y() - 3.0);
            cone.lineTo(c.x() - 4.0, c.y() - 3.0);
            cone.lineTo(c.x() + 1.0, c.y() - 7.5);
            cone.lineTo(c.x() + 1.0, c.y() + 7.5);
            cone.lineTo(c.x() - 4.0, c.y() + 3.0);
            cone.lineTo(c.x() - 7.0, c.y() + 3.0);
            cone.closeSubpath();
            p->setPen(Qt::NoPen);
            p->setBrush(color);
            p->drawPath(cone);
            p->setBrush(Qt::NoBrush);
            if (kind == ClipIconKind::Volume) {
                p->setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p->drawArc(QRectF(c.x() + 1.5, c.y() - 4.0, 4.5, 8.0), -52 * 16, 104 * 16);
                p->drawArc(QRectF(c.x() + 4.0, c.y() - 7.5, 5.5, 15.0), -52 * 16, 104 * 16);
            } else {
                p->setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p->drawLine(QPointF(c.x() + 3.0, c.y() - 3.5), QPointF(c.x() + 8.0, c.y() + 3.5));
                p->drawLine(QPointF(c.x() + 8.0, c.y() - 3.5), QPointF(c.x() + 3.0, c.y() + 3.5));
            }
            break;
        }
    }
    p->restore();
}

QIcon makeVectorIcon(ClipIconKind kind, const QColor& color, QWidget* widget) {
    const qreal dpr = widget ? widget->devicePixelRatioF() : 1.0;
    const int px = qMax(16, qRound(20.0 * dpr));
    QPixmap pixmap(px, px);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    drawClipIcon(&p, kind, QRectF(0, 0, 20, 20), color);
    return QIcon(pixmap);
}

}  // namespace

// ---------------------------------------------------------------------------
// ClipIconButton — icon-only vector button (play / pause / close).
// Q_OBJECT first (skill section 3). Press scale 0.96 / 150ms OutQuad and
// icon swap 0.25 -> 1.0 / 300ms are retargeted QPropertyAnimations, so a
// mid-animation change of intent reverses smoothly instead of snapping.
// ---------------------------------------------------------------------------
class ClipIconButton final : public QPushButton {
    Q_OBJECT
    Q_PROPERTY(qreal swapScale READ swapScale WRITE setSwapScale)
    Q_PROPERTY(qreal pressScale READ pressScale WRITE setPressScale)
public:
    enum class Kind { Play, Pause, Close, Volume, Mute };
    explicit ClipIconButton(Kind kind, bool filled, QWidget* parent = nullptr)
        : QPushButton(parent), kind_(kind), filled_(filled) {
        setFlat(true);
        setFocusPolicy(Qt::StrongFocus);
        swapAnim_ = new QPropertyAnimation(this, "swapScale", this);
        pressAnim_ = new QPropertyAnimation(this, "pressScale", this);
    }

    void setKind(Kind kind, bool animate = true) {
        if (kind_ == kind) return;
        kind_ = kind;
        if (!animate) {
            swapScale_ = 1.0;
            update();
            return;
        }
        // Contextual icon swap (animations.md): 0.25 -> 1.0, 300ms, bounce 0.
        animateTo(swapAnim_, 0.25, 1.0, 300, QEasingCurve::OutCubic);
    }
    qreal swapScale() const { return swapScale_; }
    void setSwapScale(qreal v) {
        swapScale_ = v;
        update();
    }
    qreal pressScale() const { return pressScale_; }
    void setPressScale(qreal v) {
        pressScale_ = v;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        enableChromeAntialiasing(p);
        const ClipTokens t = clipTokens();
        const QRectF r = deviceAlignedRect(QRectF(rect()), devicePixelRatioF());
        // Follow the app's corner radius, clamped so a large setting still
        // reads as a disc: at radius 0 the control is square like every other
        // button in the app, at 8+ it is the circle the transport wants.
        const qreal radius = qMin<qreal>(t.innerRadius, r.height() / 2.0);
        if (filled_) {
            QColor bg = t.surfaceRaised;
            if (isDown()) bg = bg.darker(115);
            else if (underMouse()) bg = bg.lighter(115);
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawRoundedRect(r, radius, radius);
        } else if (underMouse() || hasFocus()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 26));
            p.drawRoundedRect(r, radius, radius);
        }
        // Opacity follows the swap scale: 0 at 0.25, 1 at 1.0.
        const qreal opacity = qBound(0.0, (swapScale_ - 0.25) / 0.75, 1.0);
        p.setOpacity(opacity);
        const qreal scale = swapScale_ * pressScale_;
        p.translate(r.center());
        p.scale(scale, scale);
        p.translate(-r.center());
        const ClipIconKind glyph = kind_ == Kind::Play      ? ClipIconKind::Play
                                   : kind_ == Kind::Pause   ? ClipIconKind::Pause
                                   : kind_ == Kind::Volume  ? ClipIconKind::Volume
                                   : kind_ == Kind::Mute    ? ClipIconKind::Mute
                                                            : ClipIconKind::Close;
        drawClipIcon(&p, glyph, r, t.fg);
    }
    void mousePressEvent(QMouseEvent* event) override {
        QPushButton::mousePressEvent(event);
        // Press scale 0.96, 150ms OutQuad, interruptible.
        animateTo(pressAnim_, pressScale_, 0.96, 150, QEasingCurve::OutQuad);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        QPushButton::mouseReleaseEvent(event);
        animateTo(pressAnim_, pressScale_, 1.0, 150, QEasingCurve::OutQuad);
    }

private:
    void animateTo(QPropertyAnimation* anim, qreal from, qreal to, int ms, QEasingCurve::Type type) {
        if (!anim) return;
        anim->stop();
        anim->setDuration(ms);
        anim->setStartValue(from);
        anim->setEndValue(to);
        anim->setEasingCurve(type);
        anim->start();
    }
    Kind kind_;
    bool filled_;
    qreal swapScale_ = 1.0;
    qreal pressScale_ = 1.0;
    QPropertyAnimation* swapAnim_ = nullptr;
    QPropertyAnimation* pressAnim_ = nullptr;
};

// ---------------------------------------------------------------------------
// ClipTimelineStrip — filmstrip with a draggable selection.
// Q_OBJECT first; signals use function-pointer connect() in the dialog.
// ---------------------------------------------------------------------------
class ClipTimelineStrip final : public QWidget {
    Q_OBJECT
public:
    explicit ClipTimelineStrip(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("clipStrip"));
        setMinimumHeight(kStripHeight);
        setFixedHeight(kStripHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

signals:
    void selectionChanged(double start, double end);
    void seekRequested(double seconds);
    void selectionFinished();
    void resized();

public:
    int tileCount() const {
        const int w = width();
        if (w <= 0) return 10;
        return std::clamp((w + kTileWidth - 1) / kTileWidth + 1, 8, 28);
    }
    bool isDragging() const { return dragMode_ != DragMode::None; }

    void setWindow(double start, double end) {
        windowStart_ = start;
        windowEnd_ = end;
        update();
    }
    void setSelection(double start, double end) {
        selStart_ = start;
        selEnd_ = end;
        update();
    }
    void setPlayhead(double seconds) {
        playhead_ = seconds;
        update();
    }
    void setTiles(const QVector<QPixmap>& tiles) {
        tiles_ = tiles;
        update();
    }
    void setTile(int index, const QPixmap& tile) {
        if (index < 0 || index >= tiles_.size()) return;
        tiles_[index] = tile;
        update();
    }
    int slotForTime(double seconds) const {
        if (tiles_.isEmpty() || windowEnd_ <= windowStart_) return -1;
        const double f = (seconds - windowStart_) / (windowEnd_ - windowStart_);
        return std::clamp(int(f * tiles_.size()), 0, int(tiles_.size()) - 1);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        // One helper, so the strip antialiases exactly like the rest of the
        // chrome instead of keeping its own pair of hints.
        enableChromeAntialiasing(p);
        const ClipTokens t = clipTokens();
        const qreal dpr = devicePixelRatioF();
        const int w = width();
        const int h = height();
        if (w <= 0 || h <= 0) return;
        QPainterPath clip;
        clip.addRoundedRect(QRectF(0, 0, w, h), t.cardRadius, t.cardRadius);
        p.setClipPath(clip);
        p.fillRect(QRect(0, 0, w, h), t.surface);

        const int count = tiles_.size();
        if (count > 0) {
            const double slotW = double(w) / count;
            for (int i = 0; i < count; ++i) {
                const QRectF r = deviceAlignedRect(QRectF(i * slotW, 0, slotW + 1, h), dpr);
                if (!tiles_.at(i).isNull()) {
                    p.drawPixmap(r.toRect(), tiles_.at(i));
                    // Inset 1px outline so thumbnails never bleed into
                    // the surface (surfaces.md).
                    p.setPen(QPen(QColor(255, 255, 255, 26), 1.0));
                    p.setBrush(Qt::NoBrush);
                    p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
                } else {
                    p.fillRect(r.toRect(), t.surfaceRaised);
                }
            }
        }
        if (windowEnd_ <= windowStart_) return;
        const auto xFor = [this, w](double time) {
            return (time - windowStart_) / (windowEnd_ - windowStart_) * w;
        };
        const QRectF sel = deviceAlignedRect(
            QRectF(xFor(selStart_), 0, qMax(2.0, xFor(selEnd_) - xFor(selStart_)), h), dpr);

        // Dim everything outside the selection.
        p.fillRect(QRectF(0, 0, sel.left(), h), QColor(0, 0, 0, 165));
        p.fillRect(QRectF(sel.right(), 0, w - sel.right(), h), QColor(0, 0, 0, 165));

        // Accent gradient interval fill (invariant 5), border in accent.
        QLinearGradient fillGradient(sel.topLeft(), sel.bottomLeft());
        fillGradient.setColorAt(0.0, QColor(t.accent.red(), t.accent.green(), t.accent.blue(), 72));
        fillGradient.setColorAt(1.0, QColor(t.accent.red(), t.accent.green(), t.accent.blue(), 48));
        p.setBrush(fillGradient);
        p.setPen(QPen(t.accent, 2.0));
        p.drawRoundedRect(sel.adjusted(1, 1, -1, -1), 10, 10);

        // Bracket handles: tactile vertical grip lines + focus halo
        // while that handle is being dragged.
        p.setPen(Qt::NoPen);
        p.setBrush(t.fg);
        const double gripH = 18.0;
        p.drawRoundedRect(QRectF(sel.left() + 4, (h - gripH) / 2, 3, gripH), 1.5, 1.5);
        p.drawRoundedRect(QRectF(sel.right() - 7, (h - gripH) / 2, 3, gripH), 1.5, 1.5);
        if (dragMode_ == DragMode::ResizeLeft || dragMode_ == DragMode::ResizeRight) {
            const double hx = dragMode_ == DragMode::ResizeLeft ? sel.left() : sel.right();
            p.setPen(QPen(t.accent, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(hx - 8, 4, 16, h - 8), 8, 8);
        }

        // White playhead: crisp line + circular pip.
        const double px = xFor(playhead_);
        if (px >= 0.0 && px <= w) {
            const double alignedX = qRound(px * dpr) / dpr;
            p.setPen(QPen(Qt::white, 2));
            p.drawLine(QPointF(alignedX, 0), QPointF(alignedX, h));
            p.setBrush(Qt::white);
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(alignedX, 7), 5, 5);
        }
    }

    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        emit resized();
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || windowEnd_ <= windowStart_) return;
        const double t = timeFromX(event->pos().x());
        const double x0 = xFromTime(selStart_);
        const double x1 = xFromTime(selEnd_);
        const double x = event->pos().x();
        dragAnchorTime_ = t;
        dragStartSelStart_ = selStart_;
        dragStartSelEnd_ = selEnd_;
        if (std::abs(x - x0) <= kHandleHitHalfWidth) dragMode_ = DragMode::ResizeLeft;
        else if (std::abs(x - x1) <= kHandleHitHalfWidth) dragMode_ = DragMode::ResizeRight;
        else if (x > x0 && x < x1) {
            dragMode_ = DragMode::Move;
            setCursor(Qt::ClosedHandCursor);
        } else {
            dragMode_ = DragMode::Create;
            selStart_ = t;
            selEnd_ = t;
            playhead_ = t;
            emit seekRequested(t);
            emit selectionChanged(selStart_, selEnd_);
        }
        update();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (windowEnd_ <= windowStart_) return;
        const double t = timeFromX(event->pos().x());
        if (dragMode_ == DragMode::None) {
            const double x0 = xFromTime(selStart_);
            const double x1 = xFromTime(selEnd_);
            const double x = event->pos().x();
            if (std::abs(x - x0) <= kHandleHitHalfWidth || std::abs(x - x1) <= kHandleHitHalfWidth)
                setCursor(Qt::SizeHorCursor);
            else if (x > x0 && x < x1) setCursor(Qt::OpenHandCursor);
            else setCursor(Qt::PointingHandCursor);
            return;
        }
        const double dt = t - dragAnchorTime_;
        switch (dragMode_) {
            case DragMode::Move: {
                double a = dragStartSelStart_ + dt;
                const double len = dragStartSelEnd_ - dragStartSelStart_;
                a = std::clamp(a, windowStart_, windowEnd_ - len);
                selStart_ = a;
                selEnd_ = a + len;
                playhead_ = std::clamp(playhead_, a, selEnd_);
                break;
            }
            case DragMode::ResizeLeft:
                selStart_ = std::clamp(t, windowStart_, selEnd_ - kMinClipSeconds);
                if (selEnd_ - selStart_ > kMaximumClipDuration) selStart_ = selEnd_ - kMaximumClipDuration;
                playhead_ = selStart_;
                break;
            case DragMode::ResizeRight:
                selEnd_ = std::clamp(t, selStart_ + kMinClipSeconds, windowEnd_);
                if (selEnd_ - selStart_ > kMaximumClipDuration) selEnd_ = selStart_ + kMaximumClipDuration;
                playhead_ = selEnd_;
                break;
            case DragMode::Create:
                selStart_ = std::min(dragAnchorTime_, t);
                selEnd_ = std::max(dragAnchorTime_, t);
                playhead_ = t;
                emit seekRequested(t);
                break;
            case DragMode::None:
                break;
        }
        emit selectionChanged(selStart_, selEnd_);
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        if (dragMode_ == DragMode::Create && selEnd_ - selStart_ < kMinClipSeconds) {
            selStart_ = dragStartSelStart_;
            selEnd_ = dragStartSelEnd_;
            emit selectionChanged(selStart_, selEnd_);
        }
        dragMode_ = DragMode::None;
        setCursor(Qt::PointingHandCursor);
        update();
        emit selectionFinished();
    }

private:
    enum class DragMode { None, Move, ResizeLeft, ResizeRight, Create };
    double timeFromX(int x) const {
        if (width() <= 0 || windowEnd_ <= windowStart_) return windowStart_;
        return windowStart_ + double(x) / width() * (windowEnd_ - windowStart_);
    }
    double xFromTime(double t) const {
        if (windowEnd_ <= windowStart_) return 0.0;
        return (t - windowStart_) / (windowEnd_ - windowStart_) * width();
    }
    QVector<QPixmap> tiles_;
    double windowStart_ = 0.0;
    double windowEnd_ = 0.0;
    double selStart_ = 0.0;
    double selEnd_ = 0.0;
    double playhead_ = 0.0;
    DragMode dragMode_ = DragMode::None;
    double dragAnchorTime_ = 0.0;
    double dragStartSelStart_ = 0.0;
    double dragStartSelEnd_ = 0.0;
};

}  // namespace mplayer::shell

namespace mplayer::shell {

// ClipDialog::Private declares no signals, slots or properties and uses
// only context-bound lambdas, so Qt does not require Q_OBJECT for it.
// The Q_OBJECT classes above (ClipIconButton, ClipTimelineStrip) do
// declare signals/properties and carry Q_OBJECT first.
class ClipDialog::Private final : public QObject {
public:
    explicit Private(ClipDialog* dialog, VideoSession* source, const QString& caption)
        : QObject(dialog), dialog(dialog), source(source), caption(caption) {
        // Freeze the anchor NOW: the position at pause / at the Cut button
        // is the centre of the +/-2 min window. Do NOT clamp to duration
        // here: duration is often still unknown (0) at construction.
        const double posAtOpen = source ? source->position() : 0.0;
        anchorSeconds = std::isfinite(posAtOpen) && posAtOpen > 0.0 ? posAtOpen : 0.0;
        build();
        if (!source || source->sourceUrl().isEmpty()) {
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog",
                                           "The current video source is unavailable."));
            return;
        }
        sourceUrl = source->sourceUrl();
        thumbnails.setSource(sourceUrl, source->sourceEpoch());
        connect(&thumbnails, &FramePreviewService::frameReady, this,
                [this](quint64 token, double, double, const QImage& image) {
                    onTileReady(token, image);
                });
        connect(&thumbnails, &FramePreviewService::frameUnavailable, this,
                [this](quint64 token, double, const QString&) { finishTile(token); });
        connect(source, &VideoSession::mediaInformationInvalidated, this, [this] {
            invalidated = true;
            exportWorker.cancel();
            thumbnails.shutdown();
            if (preview && preview->session()) preview->session()->close();
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog",
                                          "The video changed. Close this window and open Cut moment again."));
        });
        connect(source, &VideoSession::sourceEpochChanged, this, [this](uint64_t) {
            invalidated = true;
            exportWorker.cancel();
            thumbnails.shutdown();
            if (preview && preview->session()) preview->session()->close();
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog",
                                          "The video changed. Close this window and open Cut moment again."));
        });
        connect(source, &QObject::destroyed, this, [this] {
            invalidated = true;
            exportWorker.cancel();
            thumbnails.shutdown();
            if (preview && preview->session()) preview->session()->close();
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "The video player was closed."));
        });
        connect(source, &VideoSession::durationChanged, this, [this](double seconds) {
            if (invalidated || exporting || !std::isfinite(seconds) || seconds <= 0.0) return;
            duration = seconds;
            if (!windowInitialized) initializeWindow();
        });
        connect(source, &VideoSession::videoSizeChanged, this, [this](int width, int height) {
            if (invalidated || exporting || width <= 0 || height <= 0) return;
            size = QSize(width, height);
            refreshDownload();
        });
        connect(source, &VideoSession::mediaTracksChanged, this, [this] { captureTracks(); });
        connect(source, &VideoSession::streamStatsChanged, this,
                [this](double bytesPerSecond, double, StreamFpsSource) {
                    if (!std::isfinite(bytesPerSecond) || bytesPerSecond <= 0.0) return;
                    streamBytesPerSecond = bytesPerSecond;
                    refreshDownload();
                });

        tileWatchdog.setSingleShot(true);
        connect(&tileWatchdog, &QTimer::timeout, this, [this] {
            thumbnails.cancel();
            tileInFlight = false;
            pumpTile();
        });
        tileDebounce.setSingleShot(true);
        connect(&tileDebounce, &QTimer::timeout, this, [this] { requestTiles(); });
        tileRetry.setSingleShot(true);
        connect(&tileRetry, &QTimer::timeout, this, [this] { pumpTile(); });
        stillDebounce.setSingleShot(true);
        connect(&stillDebounce, &QTimer::timeout, this, [this] {
            queueStill(stillPendingTime);
            pumpTile();
        });

        connect(&exportWorker, &ClipExport::encodingStarted, this, [this](const QString& codec) {
            codecName = codec;
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Encoding MP4 with %1…"), {codecName});
        });
        connect(&exportWorker, &ClipExport::progressChanged, this, [this](double fraction) {
            const double clamped = std::clamp(fraction, 0.0, 1.0);
            exportProgress->setValue(int(clamped * 100.0));
            downloadButton->setText(ClipTranslation::tr("Encoding… %1%")
                                        .arg(QString::number(clamped * 100.0, 'f', 0)));
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Encoding MP4 with %1… %2%"),
                      {codecName.isEmpty() ? ClipTranslation::tr("video") : codecName,
                       QString::number(clamped * 100.0, 'f', 1)});
        });
        connect(&exportWorker, &ClipExport::finished, this,
                [this](bool succeeded, const QString& path, const QString& error) {
            exporting = false;
            exportProgress->setVisible(false);
            exportProgress->setValue(0);
            cancel->setVisible(false);
            downloadButton->setEnabled(!invalidated);
            if (!succeeded) {
                if (error.isEmpty()) setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Export failed."));
                else setExternalStatus(error);
                exportButtonState = ExportButtonState::Retry;
                retranslateUi();
                return;
            }
            completedPath = path;
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Saved %1"),
                      {QFileInfo(path).fileName()});
            exportButtonState = ExportButtonState::Another;
            retranslateUi();
            reveal->setVisible(true);
            copyPath->setVisible(true);
        });

        captureTracks();
        const QSize sourceSize(source->videoWidth(), source->videoHeight());
        if (sourceSize.isValid()) size = sourceSize;
        if (std::isfinite(source->duration()) && source->duration() > 0.0) duration = source->duration();
        initializeWindow();
        // No preview session open here on purpose: the dialog opens with a
        // still frame; the stream opens lazily on first Play.
        retranslateUi();
    }

    ~Private() override {
        tileWatchdog.stop();
        tileDebounce.stop();
        tileRetry.stop();
        stillDebounce.stop();
        exportWorker.cancel();
        thumbnails.shutdown();
    }

    enum class ExportButtonState { Default, Retry, Another };

    void retranslateUi() {
        dialog->setWindowTitle(caption.trimmed().isEmpty()
                                   ? ClipTranslation::tr("Cut moment")
                                   : ClipTranslation::tr("Cut moment — %1").arg(caption));
        dialog->setStyleSheet(clipDialogStyleSheet());
        updateIcons();
        cancel->setText(ClipTranslation::tr("Cancel"));
        reveal->setText(ClipTranslation::tr("Show in folder"));
        copyPath->setText(ClipTranslation::tr("Copy path"));
        setStartButton->setText(ClipTranslation::tr("Set start"));
        setEndButton->setText(ClipTranslation::tr("Set end"));
        for (int i = 0; i < 5; ++i) {
            if (presetButtons[i]) presetButtons[i]->setText(ClipTranslation::tr("%1s").arg(presetSeconds[i]));
        }
        updateMuteChrome();
        updateQualityButtons();
        refreshInfo();
        refreshDownload();
        if (!statusSource.isEmpty()) {
            QString text = ClipTranslation::tr(statusSource.toUtf8().constData());
            for (const QString& arg : statusArgs) text = text.arg(arg);
            setExternalStatus(text);
        }
    }

    // Vector icons are regenerated from the live tokens, so an accent
    // change repaints them in the new accent / ink colours.
    void updateIcons() {
        const ClipTokens t = clipTokens();
        if (downloadButton) downloadButton->setIcon(makeVectorIcon(ClipIconKind::Download, t.accentFg, downloadButton));
        if (setStartButton) setStartButton->setIcon(makeVectorIcon(ClipIconKind::SetStart, t.fg, setStartButton));
        if (setEndButton) setEndButton->setIcon(makeVectorIcon(ClipIconKind::SetEnd, t.fg, setEndButton));
    }

    void refreshInfo() {
        if (!infoTop || !infoBottom) return;
        const double length = std::max(0.0, selEnd_ - selStart_);
        infoTop->setText(ClipTranslation::tr("%1 s of 60").arg(QLocale().toString(length, 'f', 1)));
        infoBottom->setText(QStringLiteral("%1 – %2").arg(formatClipTime(selStart_), formatClipTime(selEnd_)));
        for (int i = 0; i < 5; ++i) {
            if (presetButtons[i]) presetButtons[i]->setChecked(std::abs(length - presetSeconds[i]) < 0.05);
        }
    }

    void refreshDownload() {
        if (!downloadButton) return;
        if (exporting) return;
        const double length = std::max(0.0, selEnd_ - selStart_);
        const int height = qualityHeight > 0 ? qualityHeight : 1080;
        const qint64 bytes = qint64(estimatedBitsPerSecond(height) * length / 8.0);
        const QString sizeText = length > 0.0 ? formatSize(bytes) : QString();
        const bool ready = validRange() && !invalidated;
        downloadButton->setText(
            exportButtonState == ExportButtonState::Retry ? ClipTranslation::tr("Try again")
            : exportButtonState == ExportButtonState::Another ? ClipTranslation::tr("Export another")
                                                              : ClipTranslation::tr("Download %1").arg(sizeText));
        downloadButton->setEnabled(ready);
        if (playButton) playButton->setEnabled(!invalidated);
        if (strip) strip->setEnabled(!invalidated);
    }

    void setStatus(const char* sourceText, std::initializer_list<QString> args = {}) {
        statusSource = QString::fromUtf8(sourceText);
        statusArgs = QStringList(args);
        QString text = ClipTranslation::tr(sourceText);
        for (const QString& arg : statusArgs) text = text.arg(arg);
        if (statusLabel != nullptr) statusLabel->setText(text);
    }

    void setExternalStatus(const QString& text) {
        statusSource.clear();
        statusArgs.clear();
        if (statusLabel != nullptr) statusLabel->setText(text);
    }

    bool handleKeyPress(QKeyEvent* event) {
        if (event->key() == Qt::Key_Space) {
            togglePlay();
            return true;
        }
        if (event->key() == Qt::Key_BracketLeft) {
            setStartToCurrent();
            return true;
        }
        if (event->key() == Qt::Key_BracketRight) {
            setEndToCurrent();
            return true;
        }
        if (event->key() == Qt::Key_Left) {
            stepSeconds(event->modifiers().testFlag(Qt::ShiftModifier) ? -0.1 : -1.0);
            return true;
        }
        if (event->key() == Qt::Key_Right) {
            stepSeconds(event->modifiers().testFlag(Qt::ShiftModifier) ? 0.1 : 1.0);
            return true;
        }
        if (event->key() == Qt::Key_M) {
            toggleMute();
            return true;
        }
        if (event->key() == Qt::Key_Escape) return false;
        return false;
    }

    void build() {
        dialog->setObjectName(QStringLiteral("clipDialog"));
        dialog->setWindowTitle(ClipTranslation::tr("Cut moment"));
        dialog->setModal(false);
        dialog->setMinimumWidth(760);
        dialog->resize(980, 720);
        dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);

        const ClipTokens t = clipTokens();
        auto* root = new QVBoxLayout(dialog);
        root->setContentsMargins(16, 16, 16, 16);
        root->setSpacing(12);

        // Preview card: radius = cornerRadius() + 4, inner = cornerRadius().
        previewCard = new QFrame(dialog);
        previewCard->setObjectName(QStringLiteral("clipCard"));
        auto* previewLayout = new QVBoxLayout(previewCard);
        previewLayout->setContentsMargins(8, 8, 8, 8);
        previewLayout->setSpacing(0);

        previewStack = new QWidget(previewCard);
        auto* stackLayout = new QGridLayout(previewStack);
        stackLayout->setContentsMargins(0, 0, 0, 0);
        stackLayout->setSpacing(0);

        preview = new VideoSurface(previewStack);
        preview->setObjectName(QStringLiteral("clipPreview"));
        preview->setMinimumHeight(240);
        preview->setDisplayCornerRadius(qMax(4.0, double(t.innerRadius)));
        preview->setFocusPolicy(Qt::StrongFocus);
        // Sound on by default: the preview is a check of the moment's audio
        // too, and a silent first press reads as a broken file.
        preview->session()->setMuted(previewMuted);
        preview->session()->setPaused(true);
        stackLayout->addWidget(preview, 0, 0);

        previewStill = new QLabel(previewStack);
        previewStill->setObjectName(QStringLiteral("clipStill"));
        previewStill->setAlignment(Qt::AlignCenter);
        previewStill->setScaledContents(true);
        previewStill->setMinimumHeight(240);
        stackLayout->addWidget(previewStill, 0, 0);
        previewStill->raise();
        previewLayout->addWidget(previewStack, 1);
        root->addWidget(previewCard, 1);  // invariant 3: only content has stretch 1

        strip = new ClipTimelineStrip(dialog);
        connect(strip, &ClipTimelineStrip::selectionChanged, this,
                [this](double a, double b) { setSelection(a, b, 0); });
        connect(strip, &ClipTimelineStrip::seekRequested, this, [this](double s) { seekTo(s); });
        connect(strip, &ClipTimelineStrip::selectionFinished, this, [this] { requestStillAt(selStart_); });
        connect(strip, &ClipTimelineStrip::resized, this, [this] { requestTilesSoon(); });
        root->addWidget(strip);

        // Preset row: fixed height (invariant 3), chips + boundary
        // capture + status. All hit targets >= 32px.
        auto* presetRow = new QWidget(dialog);
        // Fixed vertical policy keeps the row out of the preview's stretch
        // budget, but the height comes from the children: pinning it to 32 px
        // while the stylesheet box is 32 content + 2 border clipped the bottom
        // edge off every chip.
        presetRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* presetLayout = new QHBoxLayout(presetRow);
        presetLayout->setContentsMargins(0, 0, 0, 0);
        presetLayout->setSpacing(8);
        for (int i = 0; i < 5; ++i) {
            auto* chip = new QPushButton(presetRow);
            chip->setObjectName(QStringLiteral("clipPreset"));
            chip->setCheckable(true);
            chip->setText(ClipTranslation::tr("%1s").arg(presetSeconds[i]));
            chip->setToolTip(ClipTranslation::tr("Set clip length to %1 seconds").arg(presetSeconds[i]));
            chip->setMinimumWidth(52);
            chip->setMaximumWidth(52);
            // Tabular numerals so "5s", "10s" and "60s" keep the same digit
            // rhythm inside the shared pill width.
            QFont presetFont = chip->font();
            presetFont.setFeature(QFont::Tag("tnum"), 1);
            chip->setFont(presetFont);
            presetLayout->addWidget(chip);
            presetButtons[i] = chip;
            const int seconds = presetSeconds[i];
            connect(chip, &QPushButton::clicked, this, [this, seconds] { applyDurationPreset(double(seconds)); });
        }
        setStartButton = new QPushButton(presetRow);
        setStartButton->setObjectName(QStringLiteral("clipGhost"));
        setStartButton->setToolTip(ClipTranslation::tr("Set start to current playhead ( [ )"));
        setEndButton = new QPushButton(presetRow);
        setEndButton->setObjectName(QStringLiteral("clipGhost"));
        setEndButton->setToolTip(ClipTranslation::tr("Set end to current playhead ( ] )"));
        reveal = new QPushButton(presetRow);
        reveal->setObjectName(QStringLiteral("clipGhost"));
        reveal->setVisible(false);
        copyPath = new QPushButton(presetRow);
        copyPath->setObjectName(QStringLiteral("clipGhost"));
        copyPath->setVisible(false);
        cancel = new QPushButton(presetRow);
        cancel->setObjectName(QStringLiteral("clipGhost"));
        cancel->setVisible(false);
        presetLayout->addWidget(setStartButton);
        presetLayout->addWidget(setEndButton);
        presetLayout->addWidget(reveal);
        presetLayout->addWidget(copyPath);
        presetLayout->addWidget(cancel);
        presetLayout->addStretch(1);
        statusLabel = new QLabel(presetRow);
        statusLabel->setObjectName(QStringLiteral("clipStatus"));
        statusLabel->setWordWrap(false);
        presetLayout->addWidget(statusLabel);
        root->addWidget(presetRow);

        exportProgress = new QProgressBar(dialog);
        exportProgress->setObjectName(QStringLiteral("clipProgress"));
        exportProgress->setRange(0, 100);
        exportProgress->setValue(0);
        exportProgress->setTextVisible(false);
        exportProgress->setVisible(false);
        exportProgress->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        root->addWidget(exportProgress);

        // Bottom bar, like the screenshot.
        auto* bottom = new QHBoxLayout;
        bottom->setContentsMargins(0, 0, 0, 0);
        bottom->setSpacing(12);

        playButton = new ClipIconButton(ClipIconButton::Kind::Play, true, dialog);
        playButton->setFixedSize(44, 44);
        playButton->setToolTip(ClipTranslation::tr("Play / pause (Space)"));
        bottom->addWidget(playButton);

        // The preview's own audio. Unfilled and one step smaller, so it reads
        // as a secondary control beside the transport and the download action.
        muteButton = new ClipIconButton(ClipIconButton::Kind::Volume, false, dialog);
        muteButton->setFixedSize(36, 36);
        bottom->addWidget(muteButton);

        auto* info = new QVBoxLayout;
        info->setContentsMargins(0, 0, 0, 0);
        info->setSpacing(4);
        infoTop = new QLabel(dialog);
        infoTop->setObjectName(QStringLiteral("clipInfoTop"));
        QFont topFont = infoTop->font();
        topFont.setPixelSize(14);
        topFont.setWeight(QFont::DemiBold);
        infoTop->setFont(topFont);
        applyTabularNumerals(infoTop);
        infoBottom = new QLabel(dialog);
        infoBottom->setObjectName(QStringLiteral("clipInfoBottom"));
        // Typographic hierarchy: the large line is flush left, the small one
        // indents under it. QSS padding on a label without a frame is not
        // reliable, so the indent is on the widget.
        infoTop->setContentsMargins(0, 0, 0, 0);
        infoBottom->setContentsMargins(8, 0, 0, 0);
        applyTabularNumerals(infoBottom);
        info->addWidget(infoTop);
        info->addWidget(infoBottom);
        bottom->addLayout(info);
        bottom->addStretch(1);

        auto* qualityPill = new QFrame(dialog);
        qualityPill->setObjectName(QStringLiteral("clipQualityPill"));
        qualityPill->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        auto* qualityLayout = new QHBoxLayout(qualityPill);
        qualityLayout->setContentsMargins(4, 4, 4, 4);
        qualityLayout->setSpacing(0);
        const int heights[3] = {1080, 720, 480};
        for (int i = 0; i < 3; ++i) {
            auto* button = new QPushButton(qualityPill);
            button->setObjectName(QStringLiteral("clipQuality"));
            button->setCheckable(true);
            button->setText(QStringLiteral("%1p").arg(heights[i]));
            qualityLayout->addWidget(button);
            qualityButtons[i] = button;
            const int h = heights[i];
            connect(button, &QPushButton::clicked, this, [this, h] {
                qualityHeight = h;
                updateQualityButtons();
                refreshDownload();
            });
        }
        bottom->addWidget(qualityPill);

        downloadButton = new QPushButton(dialog);
        downloadButton->setObjectName(QStringLiteral("clipDownload"));
        downloadButton->setEnabled(false);
        bottom->addWidget(downloadButton);

        // No close button: the dialog keeps its native title bar, and a second
        // cross beside the download action only competes with it.

        root->addLayout(bottom);

        connect(playButton, &QPushButton::clicked, this, [this] { togglePlay(); });
        connect(muteButton, &QPushButton::clicked, this, [this] { toggleMute(); });
        connect(downloadButton, &QPushButton::clicked, this, [this] { chooseOutput(); });
        connect(setStartButton, &QPushButton::clicked, this, [this] { setStartToCurrent(); });
        connect(setEndButton, &QPushButton::clicked, this, [this] { setEndToCurrent(); });
        connect(cancel, &QPushButton::clicked, this, [this] {
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Canceling export…"));
            cancel->setEnabled(false);
            exportWorker.cancel();
        });
        connect(reveal, &QPushButton::clicked, this, [this] {
            if (!completedPath.isEmpty())
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(completedPath).absolutePath()));
        });
        connect(copyPath, &QPushButton::clicked, this, [this] {
            if (!completedPath.isEmpty()) QGuiApplication::clipboard()->setText(completedPath);
        });
        // Invariant 1: live accent + radius — restyle AND update in place.
        connect(&accentNotifier(), &AccentNotifier::changed, this, [this] {
            dialog->setStyleSheet(clipDialogStyleSheet());
            updateIcons();
            if (strip) strip->update();
            dialog->update();
        });

        connect(preview->session(), &VideoSession::errorOccurred, this, [this](const QString& reason) {
            if (!invalidated) setExternalStatus(ClipTranslation::tr("Preview unavailable: %1").arg(reason));
        });
        connect(preview->session(), &VideoSession::durationChanged, this, [this](double seconds) {
            if (!previewOpened) return;
            if (std::isfinite(seconds) && seconds > 0.0) {
                preview->session()->setPaused(true);
                preview->session()->seek(std::clamp(selStart_, windowStart, windowEnd));
            }
        });
        connect(preview->session(), &VideoSession::mediaTracksChanged, this, [this] { syncPreviewAudio(); });
        connect(preview->session(), &VideoSession::positionChanged, this, [this](double position) {
            previewPosition = position;
            previewPositionKnown = true;
            if (strip != nullptr) strip->setPlayhead(position);
            const bool playing = playingState || (preview != nullptr && preview->session()->isPlaying());
            if (playing && duration > 0.0 && position >= selEnd_ - 0.02) {
                preview->session()->setPaused(true);
                preview->session()->seek(selStart_);
                setPlaying(false);
            }
        });

        updateQualityButtons();
        refreshInfo();
        retranslateUi();
    }

    void updateQualityButtons() {
        for (int i = 0; i < 3; ++i) {
            if (!qualityButtons[i]) continue;
            const int heights[3] = {1080, 720, 480};
            qualityButtons[i]->setChecked(qualityHeight == heights[i]);
        }
    }

    double estimatedBitsPerSecond(int height) const {
        const double ceiling = encodeBitsPerSecond(height);
        if (!(streamBytesPerSecond > 0.0)) return ceiling;
        const double source = streamBytesPerSecond * 8.0;
        const double scale = std::min(1.0, ceiling / encodeBitsPerSecond(1080));
        return std::min(ceiling, source * scale);
    }

    /// The mute control and the core move together: the retained flag is what
    /// the glyph and the tooltip are rebuilt from, and setMuted() is the one
    /// call that tells the preview core.
    void toggleMute() {
        if (invalidated) return;
        previewMuted = !previewMuted;
        if (preview != nullptr) preview->session()->setMuted(previewMuted);
        updateMuteChrome();
    }

    void updateMuteChrome() {
        if (muteButton == nullptr) return;
        muteButton->setKind(previewMuted ? ClipIconButton::Kind::Mute
                                         : ClipIconButton::Kind::Volume,
                            true);
        const QString hint = previewMuted ? ClipTranslation::tr("Unmute preview (M)")
                                          : ClipTranslation::tr("Mute preview (M)");
        muteButton->setToolTip(hint);
        muteButton->setAccessibleName(hint);
    }

    void setPlaying(bool playing) {
        playingState = playing;
        if (playButton) {
            playButton->setKind(playing ? ClipIconButton::Kind::Pause : ClipIconButton::Kind::Play, true);
        }
    }

    void ensurePreviewOpened() {
        if (previewOpened || !preview || invalidated || sourceUrl.isEmpty()) return;
        previewOpened = true;
        if (previewStill) previewStill->setVisible(false);
        preview->session()->open(sourceUrl);
        preview->session()->setPaused(true);
        syncPreviewAudio();
    }

    void togglePlay() {
        if (invalidated) return;
        if (!previewOpened) {
            ensurePreviewOpened();
            preview->session()->seek(std::clamp(selStart_, windowStart, windowEnd));
            preview->session()->setPaused(false);
            setPlaying(true);
            return;
        }
        if (!preview) return;
        const bool willPlay = !preview->session()->isPlaying();
        if (willPlay && (previewPosition < selStart_ || previewPosition >= selEnd_ - 0.02)) {
            preview->session()->seek(selStart_);
        }
        preview->session()->setPaused(!willPlay);
        setPlaying(willPlay);
    }

    void seekTo(double seconds) {
        if (invalidated || !windowInitialized) return;
        const double target = std::clamp(seconds, windowStart, windowEnd);
        previewPosition = target;
        previewPositionKnown = true;
        if (strip) strip->setPlayhead(target);
        if (previewOpened && preview) {
            preview->session()->setPaused(true);
            preview->session()->seek(target);
            setPlaying(false);
        } else {
            requestStillAt(target);
        }
    }

    void stepSeconds(double delta) {
        if (invalidated || duration <= 0.0) return;
        const double base = previewPositionKnown ? previewPosition : selStart_;
        seekTo(base + delta);
    }

    void setStartToCurrent() {
        if (duration <= 0.0 || invalidated) return;
        const double current = std::clamp(previewPositionKnown ? previewPosition : selStart_,
                                          windowStart, windowEnd);
        setSelection(current, selEnd_, 1);
    }

    void setEndToCurrent() {
        if (duration <= 0.0 || invalidated) return;
        const double current = std::clamp(previewPositionKnown ? previewPosition : selEnd_,
                                          windowStart, windowEnd);
        setSelection(selStart_, current, 2);
    }

    void applyDurationPreset(double seconds) {
        if (duration <= 0.0 || invalidated) return;
        double targetEnd = selStart_ + seconds;
        if (targetEnd > windowEnd) {
            targetEnd = windowEnd;
            setSelection(std::max(windowStart, targetEnd - seconds), targetEnd, 0);
        } else {
            setSelection(selStart_, targetEnd, 2);
        }
    }

    void initializeWindow() {
        if (invalidated || exporting || !std::isfinite(duration) || duration <= 0.0) return;
        const double center = std::clamp(anchorSeconds, 0.0, duration);
        windowStart = std::max(0.0, center - kWindowRadiusSeconds);
        windowEnd = std::min(duration, center + kWindowRadiusSeconds);
        if (windowEnd - windowStart < 1.0) {
            windowStart = 0.0;
            windowEnd = duration;
        }
        windowInitialized = true;
        if (strip) {
            strip->setWindow(windowStart, windowEnd);
            strip->setEnabled(true);
        }
        const double a = std::clamp(center - 5.0, windowStart, std::max(windowStart, windowEnd - 0.2));
        const double b = std::min(windowEnd, a + 10.0);
        setSelection(a, b, 0);
        previewPosition = a;
        previewPositionKnown = true;
        if (strip) strip->setPlayhead(a);
        requestStillAt(a);
        requestTilesSoon();
        setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog",
                                    "Drag the selection, then download. The window stays fixed around where you paused."));
    }

    void requestTilesSoon() {
        if (invalidated || !windowInitialized) return;
        tileDebounce.start(60);
    }

    void requestTiles() {
        if (invalidated || !windowInitialized || windowEnd <= windowStart || !strip) return;
        const int count = strip->tileCount();
        if (count <= 0) return;
        // The preview's still must survive a rebuild: the strip re-lays out
        // within milliseconds of opening, and clearing the queue there is what
        // left the dialog black until the viewer scrubbed.
        QVector<TileRequest> carried;
        for (const TileRequest& request : tileQueue) {
            if (request.still) carried.append(request);
        }
        tileQueue.clear();
        // A still is not tied to a slot, so a rebuild neither invalidates it nor
        // may cancel it: cancelling here dropped the one frame the preview was
        // waiting for and left the dialog black.
        if (!(tileInFlight && tileInFlightStill)) {
            tileInFlight = false;
            tileWatchdog.stop();
            thumbnails.cancel();
        }
        strip->setTiles(QVector<QPixmap>(count));
        const double span = windowEnd - windowStart;
        const double anchor = selStart_;
        QVector<TileRequest> waiting;
        for (int i = 0; i < count; ++i) {
            const double seconds = windowStart + span * (i + 0.5) / count;
            const auto cached = tileCache.constFind(tileKey(seconds));
            if (cached != tileCache.constEnd()) {
                strip->setTile(i, *cached);
            } else {
                waiting.append({i, seconds, false});
            }
        }
        std::sort(waiting.begin(), waiting.end(),
                  [anchor](const TileRequest& left, const TileRequest& right) {
                      return std::abs(left.seconds - anchor) < std::abs(right.seconds - anchor);
                  });
        // The still goes to the front: it is the frame the preview is waiting for.
        for (auto it = carried.crbegin(); it != carried.crend(); ++it) waiting.prepend(*it);
        tileQueue = waiting;
        pumpTile();
    }

    static qint64 tileKey(double seconds) { return qint64(std::llround(seconds * 2.0)); }

    void requestStillAt(double seconds) {
        if (invalidated || !windowInitialized) return;
        stillPendingTime = std::clamp(seconds, windowStart, windowEnd);
        stillDebounce.start(70);
    }

    void queueStill(double seconds) {
        if (invalidated || !windowInitialized) return;
        for (const TileRequest& r : tileQueue) {
            if (!r.still && tileKey(r.seconds) == tileKey(seconds)) return;
        }
        tileQueue.prepend({-1, seconds, true});
    }

    void pumpTile() {
        if (tileInFlight || tileQueue.isEmpty() || invalidated) return;
        if (strip && strip->isDragging() && !tileQueue.first().still) {
            tileRetry.start(150);
            return;
        }
        const TileRequest next = tileQueue.takeFirst();
        const quint64 token = ++tileToken;
        tileSlots.insert(token, next.slot);
        tileTimes.insert(token, next.seconds);
        tileStill.insert(token, next.still);
        tileInFlight = true;
        tileInFlightStill = next.still;
        tileWatchdog.start(kTileWatchdogMs);
        thumbnails.requestFrame(token, next.seconds);
    }

    void finishTile(quint64 token) {
        if (!tileInFlight) return;
        tileSlots.remove(token);
        tileTimes.remove(token);
        tileStill.remove(token);
        tileInFlight = false;
        tileWatchdog.stop();
        pumpTile();
    }

    void onTileReady(quint64 token, const QImage& image) {
        if (invalidated || image.isNull()) {
            finishTile(token);
            return;
        }
        const auto slotIt = tileSlots.constFind(token);
        if (slotIt == tileSlots.constEnd()) return;
        const int index = *slotIt;
        const double seconds = tileTimes.take(token);
        const bool still = tileStill.take(token);
        finishTile(token);
        // Scaled ONCE, with SmoothTransformation (invariant 6). The
        // strip then paints cached pixmaps 1:1; no per-frame resampling.
        const QSize target(kTileWidth, kTileHeight);
        const QPixmap tile = QPixmap::fromImage(
            image.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        storeTile(tileKey(seconds), tile);
        if (index >= 0 && strip) strip->setTile(index, tile);
        if (still && previewStill && !previewOpened) {
            const QSize previewSize = previewStack ? previewStack->size() : QSize(960, 540);
            const QSize stillTarget(qMax(320, previewSize.width()), qMax(180, previewSize.height()));
            previewStill->setPixmap(QPixmap::fromImage(
                image.scaled(stillTarget, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            previewStill->setVisible(true);
        }
        if (still && strip) {
            const int slot = strip->slotForTime(seconds);
            if (slot >= 0) {
                strip->setTile(slot, tile);
                for (int i = tileQueue.size() - 1; i >= 0; --i) {
                    if (!tileQueue.at(i).still && tileQueue.at(i).slot == slot) tileQueue.removeAt(i);
                }
            }
        }
    }

    void storeTile(qint64 key, const QPixmap& tile) {
        if (key < 0 || tile.isNull()) return;
        if (tileCache.contains(key)) return;
        while (tileCacheOrder.size() >= kTileCacheMax) tileCache.remove(tileCacheOrder.takeFirst());
        tileCache.insert(key, tile);
        tileCacheOrder.append(key);
    }

    void captureTracks() {
        externalAudio.clear();
        audioTrack.reset();
        if (!source || !source->mediaTracks().ready) return;
        const MediaTrackSelection selection = source->mediaTrackSelection();
        audioTrack = selection.audio;
        if (selection.audio.has_value()) {
            for (const MediaTrack& track : source->mediaTracks().tracks) {
                if (track.type != MediaTrackType::Audio || track.id != *selection.audio) continue;
                if (track.external) {
                    externalAudio = track.externalFilename;
                } else {
                    embeddedAudioFallback = track.id;
                }
                break;
            }
        }
        if (!embeddedAudioFallback.has_value()) {
            for (const MediaTrack& track : source->mediaTracks().tracks) {
                if (track.type == MediaTrackType::Audio && !track.external) {
                    embeddedAudioFallback = track.id;
                    break;
                }
            }
        }
        syncPreviewAudio();
    }

    void syncPreviewAudio() {
        if (!preview || !preview->session() || !previewOpened ||
            !preview->session()->mediaTracks().ready || !audioTrack.has_value()) return;
        VideoSession* previewSession = preview->session();
        const MediaTrackSnapshot& snapshot = previewSession->mediaTracks();
        if (!externalAudio.isEmpty() && previewExternalAudioFile != externalAudio) {
            if (previewSession->addExternalTrack(externalAudio, false)) {
                previewExternalAudioFile = externalAudio;
            }
        }
        if (!externalAudio.isEmpty() && previewExternalAudioFile == externalAudio) {
            for (const MediaTrack& track : snapshot.tracks) {
                if (track.type == MediaTrackType::Audio && track.external &&
                    track.externalFilename == externalAudio) {
                    previewSession->selectMediaTrack(MediaTrackType::Audio, track.id,
                                                     snapshot.coreIdentity, snapshot.sourceEpoch);
                    return;
                }
            }
        }

        const MediaTrack* selectedSourceTrack = nullptr;
        if (source && source->mediaTracks().ready) {
            for (const MediaTrack& track : source->mediaTracks().tracks) {
                if (track.type == MediaTrackType::Audio && track.id == *audioTrack) {
                    selectedSourceTrack = &track;
                    break;
                }
            }
        }
        if (selectedSourceTrack != nullptr && !selectedSourceTrack->external) {
            for (const MediaTrack& track : snapshot.tracks) {
                if (track.type == MediaTrackType::Audio && !track.external &&
                    track.id == selectedSourceTrack->id) {
                    previewSession->selectMediaTrack(MediaTrackType::Audio, track.id,
                                                     snapshot.coreIdentity, snapshot.sourceEpoch);
                    return;
                }
            }
        }

        if (embeddedAudioFallback.has_value()) {
            for (const MediaTrack& track : snapshot.tracks) {
                if (track.type == MediaTrackType::Audio && !track.external &&
                    track.id == *embeddedAudioFallback) {
                    previewSession->selectMediaTrack(MediaTrackType::Audio, track.id,
                                                     snapshot.coreIdentity, snapshot.sourceEpoch);
                    return;
                }
            }
        }
        const QString language = selectedSourceTrack != nullptr ? selectedSourceTrack->language : QString();
        const QString title = selectedSourceTrack != nullptr ? selectedSourceTrack->title : QString();
        const MediaTrack* fallback = nullptr;
        for (const MediaTrack& track : snapshot.tracks) {
            if (track.type != MediaTrackType::Audio || track.external) continue;
            if (fallback == nullptr) fallback = &track;
            if ((!language.isEmpty() && track.language == language) ||
                (!title.isEmpty() && track.title == title)) {
                fallback = &track;
                break;
            }
        }
        if (fallback != nullptr) {
            previewSession->selectMediaTrack(MediaTrackType::Audio, fallback->id,
                                             snapshot.coreIdentity, snapshot.sourceEpoch);
        }
    }

    void setSelection(double a, double b, int editedField) {
        if (duration <= 0.0 || exporting || invalidated || !windowInitialized) return;
        a = std::clamp(a, windowStart, windowEnd);
        b = std::clamp(b, windowStart, windowEnd);
        if (b - a < kMinClipSeconds &&
            windowEnd - windowStart >= kMinClipSeconds) {
            if (editedField == 1) {
                a = std::max(windowStart, b - kMinClipSeconds);
                if (b - a < kMinClipSeconds) b = std::min(windowEnd, a + kMinClipSeconds);
            } else if (editedField == 2) {
                b = std::min(windowEnd, a + kMinClipSeconds);
                if (b - a < kMinClipSeconds) a = std::max(windowStart, b - kMinClipSeconds);
            } else {
                b = std::min(windowEnd, a + kMinClipSeconds);
                if (b - a < kMinClipSeconds) a = std::max(windowStart, b - kMinClipSeconds);
            }
        }
        if (b - a > kMaximumClipDuration) {
            if (editedField == 1) b = std::min(windowEnd, a + kMaximumClipDuration);
            else if (editedField == 2) a = std::max(windowStart, b - kMaximumClipDuration);
            else b = a + kMaximumClipDuration;
        }
        selStart_ = a;
        selEnd_ = b;
        // Always sync the strip: setSelection clamps to the window /
        // 60s max, and the strip must not drift past that clamp.
        // strip->setSelection() only sets fields + update(), no signal.
        if (strip) strip->setSelection(a, b);
        refreshInfo();
        refreshDownload();
    }

    bool validRange() const {
        if (!windowInitialized || exporting || invalidated || !size.isValid()) return false;
        return windowEnd - windowStart >= kMinClipSeconds &&
               selEnd_ - selStart_ >= kMinClipSeconds - 0.001 &&
               selEnd_ - selStart_ <= kMaximumClipDuration + 0.001 &&
               selStart_ >= windowStart - 0.001 && selEnd_ <= windowEnd + 0.001;
    }

    void chooseOutput() {
        if (exporting || !validRange() || !source) return;
        QPointer<Private> self(this);
        QPointer<ClipDialog> guardedDialog(dialog);
        QPointer<VideoSession> guardedSource(source);
        const uint64_t sourceEpoch = source->sourceEpoch();
        const QString selectedSource = sourceUrl;
        const double selectedStart = selStart_;
        const double selectedEnd = selEnd_;
        const QSize selectedSize = size;
        const int selectedHeight = qualityHeight > 0 ? qualityHeight : 1080;
        const std::optional<qint64> selectedAudioTrack = audioTrack;
        const QString selectedExternalAudio = externalAudio;
        QString suggestedName = QFileInfo(QUrl(selectedSource).path()).completeBaseName();
        if (suggestedName.isEmpty()) suggestedName = ClipTranslation::tr("clip");
        suggestedName += QStringLiteral(".mp4");
        QString path = QFileDialog::getSaveFileName(dialog, ClipTranslation::tr("Save clip"), suggestedName,
                                                    ClipTranslation::tr("MP4 video (*.mp4)"), nullptr,
                                                    QFileDialog::DontConfirmOverwrite);
        if (!self || !guardedDialog || !guardedSource) return;
        if (invalidated || guardedSource->sourceEpoch() != sourceEpoch ||
            sourceUrl != selectedSource || selStart_ != selectedStart || selEnd_ != selectedEnd ||
            size != selectedSize || qualityHeight != selectedHeight ||
            audioTrack != selectedAudioTrack || externalAudio != selectedExternalAudio ||
            !validRange()) return;
        if (path.isEmpty()) return;
        if (!path.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive)) path += QStringLiteral(".mp4");
        bool replaceExisting = false;
        if (QFileInfo::exists(path)) {
            const auto answer = QMessageBox::question(
                dialog, ClipTranslation::tr("Replace existing file?"),
                ClipTranslation::tr("%1 already exists. Replace it after the new clip is complete?")
                    .arg(QFileInfo(path).fileName()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (!self || !guardedDialog || !guardedSource) return;
            if (invalidated || guardedSource->sourceEpoch() != sourceEpoch ||
                sourceUrl != selectedSource || selStart_ != selectedStart || selEnd_ != selectedEnd ||
                size != selectedSize || qualityHeight != selectedHeight ||
                audioTrack != selectedAudioTrack || externalAudio != selectedExternalAudio ||
                !validRange()) return;
            if (answer != QMessageBox::Yes) return;
            replaceExisting = true;
        }
        if (invalidated || guardedSource->sourceEpoch() != sourceEpoch || !validRange() ||
            sourceUrl != selectedSource || selStart_ != selectedStart || selEnd_ != selectedEnd ||
            size != selectedSize || qualityHeight != selectedHeight) {
            return;
        }
        ClipExportRequest request;
        request.source = selectedSource;
        request.outputPath = path;
        request.startSeconds = selectedStart;
        request.endSeconds = selectedEnd;
        request.sourceSize = selectedSize;
        request.maximumHeight = selectedHeight;
        request.audioTrackId = selectedAudioTrack;
        request.externalAudioPath = selectedExternalAudio;
        request.replaceExisting = replaceExisting;
        request.sourceOptions = {
            {"user-agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                           "Chrome/142.0.0.0 Safari/537.36"},
            {"http-header-fields", "Accept-Language: uk-UA"}};
        exporting = true;
        exportProgress->setVisible(true);
        exportProgress->setValue(0);
        downloadButton->setText(ClipTranslation::tr("Preparing…"));
        downloadButton->setEnabled(false);
        cancel->setVisible(true);
        cancel->setEnabled(true);
        exportButtonState = ExportButtonState::Default;
        setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Preparing export…"));
        exportWorker.start(request);
    }

    void unavailable(const char* sourceText) {
        setStatus(sourceText);
        if (strip) strip->setEnabled(false);
        if (playButton) playButton->setEnabled(false);
        if (muteButton) muteButton->setEnabled(false);
        if (setStartButton) setStartButton->setEnabled(false);
        if (setEndButton) setEndButton->setEnabled(false);
        for (QPushButton* button : presetButtons) {
            if (button) button->setEnabled(false);
        }
        for (QPushButton* button : qualityButtons) {
            if (button) button->setEnabled(false);
        }
        refreshDownload();
    }

    struct TileRequest {
        int slot;
        double seconds;
        bool still;
    };

    ClipDialog* dialog;
    QPointer<VideoSession> source;
    QString caption;
    VideoSurface* preview = nullptr;
    QFrame* previewCard = nullptr;
    QWidget* previewStack = nullptr;
    QLabel* previewStill = nullptr;
    ClipTimelineStrip* strip = nullptr;
    QLabel* statusLabel = nullptr;
    QLabel* infoTop = nullptr;
    QLabel* infoBottom = nullptr;
    ClipIconButton* playButton = nullptr;
    ClipIconButton* muteButton = nullptr;
    QPushButton* downloadButton = nullptr;
    QPushButton* setStartButton = nullptr;
    QPushButton* setEndButton = nullptr;
    QPushButton* qualityButtons[3] = {nullptr, nullptr, nullptr};
    QPushButton* presetButtons[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    const int presetSeconds[5] = {5, 10, 15, 30, 60};
    QProgressBar* exportProgress = nullptr;
    ClipExport exportWorker;
    FramePreviewService thumbnails;
    QPushButton* cancel = nullptr;
    QPushButton* reveal = nullptr;
    QPushButton* copyPath = nullptr;
    QHash<quint64, int> tileSlots;
    QVector<TileRequest> tileQueue;
    QHash<qint64, QPixmap> tileCache;
    QVector<qint64> tileCacheOrder;
    QHash<quint64, double> tileTimes;
    QHash<quint64, bool> tileStill;
    QTimer tileWatchdog;
    QTimer tileDebounce;
    QTimer tileRetry;
    QTimer stillDebounce;
    quint64 tileToken = 0;
    bool tileInFlight = false;
    /// Whether the request in flight is the preview still (slot -1).
    bool tileInFlightStill = false;
    double stillPendingTime = 0.0;
    double streamBytesPerSecond = 0.0;
    QString sourceUrl;
    QString externalAudio;
    QString completedPath;
    std::optional<qint64> audioTrack;
    std::optional<qint64> embeddedAudioFallback;
    QSize size;
    double duration = 0.0;
    double anchorSeconds = 0.0;
    double windowStart = 0.0;
    double windowEnd = 0.0;
    double previewPosition = 0.0;
    double selStart_ = 0.0;
    double selEnd_ = 0.0;
    int qualityHeight = 1080;
    QString codecName;
    QString statusSource;
    QStringList statusArgs;
    ExportButtonState exportButtonState = ExportButtonState::Default;
    bool previewPositionKnown = false;
    bool previewMuted = false;  // sound on by default
    bool previewOpened = false;
    bool exporting = false;
    bool windowInitialized = false;
    QString previewExternalAudioFile;
    bool invalidated = false;
    bool playingState = false;
};

ClipDialog::ClipDialog(VideoSession* source, const QString& caption, QWidget* parent)
    : QDialog(parent), d_(new Private(this, source, caption)) {}

void ClipDialog::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && d_) d_->retranslateUi();
    QDialog::changeEvent(event);
}

void ClipDialog::keyPressEvent(QKeyEvent* event) {
    if (d_ && d_->handleKeyPress(event)) {
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

ClipDialog::~ClipDialog() = default;

void ClipDialog::closeEvent(QCloseEvent* event) {
    if (d_) {
        if (d_->preview && d_->preview->session()) d_->preview->session()->setPaused(true);
        if (d_->exporting) {
            d_->exportWorker.cancel();
            d_->setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Canceling export…"));
        }
    }
    QDialog::closeEvent(event);
}

}  // namespace mplayer::shell

#include "ClipDialog.moc"