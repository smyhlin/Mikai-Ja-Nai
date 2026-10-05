// Shared tokens for the player shell: palette, type, geometry constants and
// the painted-icon contract.
//
// This is the single place the shell's colours and metrics live, so a token
// change is a one-line edit rather than a sweep through every paint routine.

#pragma once

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QRectF>

namespace mplayer {
namespace shell {

// ── geometry ───────────────────────────────────────────────────────────────

inline constexpr int kDragThresholdPx = 3;
inline constexpr int kAutohideMs = 2500;
inline constexpr int kMinWindowWidth = 720;
inline constexpr int kMinWindowHeight = 420;
inline constexpr int kFrameMargin = 8;  // letterbox padding around the surface
inline constexpr int kEdgeGrip = 8;     // frameless resize hit band
inline constexpr int kDockMaxWidth = 980;
inline constexpr int kDockHorizontalInset = 20;
inline constexpr int kDockHeight = 104;
inline constexpr int kDockBottomInset = 20;
inline constexpr int kDockPlaySize = 48;
inline constexpr int kDockPlayGlyph = 24;
inline constexpr int kDockActionSize = 40;
inline constexpr int kDockActionGlyph = 20;
// Six action buttons at kDockActionSize plus their spacing.
inline constexpr int kDockSideWidth = 6 * kDockActionSize + 5 * 4;
inline constexpr int kInfoChipHeight = 32;
// Holds the two chain lines plus the wrapped native-base and queued-change
// lines, so a frozen native recipe caption is never clipped.
inline constexpr int kCompareInfoHeight = 72;
inline constexpr double kAspectFallback = 16.0 / 9.0;

// ── palette ────────────────────────────────────────────────────────────────

inline QColor bg0() { return QColor(QStringLiteral("#05060a")); }
inline QColor dock() { return QColor(24, 25, 28, 230); }
inline QColor pill() { return QColor(32, 34, 37, 217); }
inline QColor flyout() { return QColor(35, 39, 45, 246); }
inline QColor blackSoft() { return QColor(0, 0, 0, 150); }
inline QColor accent() { return QColor(QStringLiteral("#5865F2")); }
inline QColor accentSoft() { return QColor(88, 101, 242, 70); }
inline QColor green() { return QColor(QStringLiteral("#3ba55d")); }
inline QColor red() { return QColor(QStringLiteral("#ed4245")); }

/// White with the given alpha, 0..255.
inline QColor white(int alpha) { return QColor(255, 255, 255, alpha); }

inline QColor w95() { return white(242); }
inline QColor w90() { return white(230); }
inline QColor w70() { return white(179); }
inline QColor w55() { return white(140); }
inline QColor w45() { return white(115); }
inline QColor w35() { return white(89); }
inline QColor w25() { return white(64); }
inline QColor w15() { return white(38); }
inline QColor w10() { return white(26); }

// ── type ───────────────────────────────────────────────────────────────────

inline QString systemMonoFamily() {
    return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
}

inline QString systemUiFamily() {
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
}

inline QFont monoFont(int pointSize = 10) {
    QFont font(systemMonoFamily());
    font.setPointSize(pointSize);
    font.setStyleHint(QFont::Monospace);
    return font;
}

inline QFont uiFont(int pointSize = 10) {
    QFont font(systemUiFamily());
    font.setPointSize(pointSize);
    font.setFamilies({systemUiFamily(), QStringLiteral("Inter"), QStringLiteral("Segoe UI"),
                      QStringLiteral("Noto Sans"), QStringLiteral("DejaVu Sans")});
    return font;
}

inline int textWidth(const QFont& font, const QString& text) {
    return QFontMetrics(font).horizontalAdvance(text);
}

// ── formatting ─────────────────────────────────────────────────────────────

/// mm:ss, clamping anything negative or non-finite to zero.
QString formatTime(double seconds);

/// formatTime with a leading minus, for the seek preview.
QString formatSigned(double seconds);

// ── painting helpers ───────────────────────────────────────────────────────

/// Paints a rounded rect. `border` is an optional stroke colour; a null
/// colour means no outline, which is the common case for filled chips.
void paintRounded(QPainter& painter, const QRectF& rect, double radius, const QColor& fill,
                  const QColor& border = QColor(), double borderWidth = 1.0);

/// Every icon draws into a 24-unit design grid, so strokes stay optically
/// even no matter what pixel size the caller asks for.
class IconPainter {
public:
    /// Stroke-style glyphs: volume, chevrons, window controls, transport.
    static void stroke(QPainter& painter, qreal x, qreal y, qreal size, qreal width = 1.8);
    /// Fill-style glyphs: play, pause.
    static void fill(QPainter& painter, qreal x, qreal y, qreal size);

    static void minimize(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
    static void restore(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
    static void close(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
    static void chevronLeft(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 18, const QColor& c = QColor());
    static void chevronRight(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 18, const QColor& c = QColor());
    static void play(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
    static void pause(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
    static void volume(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
    static void volumeLow(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
    static void mute(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
    static void expand(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
    static void contract(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
    static void pip(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
    static void gear(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
    static void folder(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 14, const QColor& c = QColor());
    static void download(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
    static void more(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
    static void reset(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 14, const QColor& c = QColor());
    static void spark(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 14, const QColor& c = QColor());
    static void keyboard(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 15, const QColor& c = QColor());
    /// Dub/audio track: a microphone capsule on a stand.
    static void microphone(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
    /// Subtitles: a caption frame with two letter C shapes.
    static void captions(QPainter& p, qreal x = 0, qreal y = 0, qreal s = 16, const QColor& c = QColor());
};

}  // namespace shell
}  // namespace mplayer
