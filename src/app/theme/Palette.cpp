#include "theme/Palette.hpp"

#include "theme/Colors.hpp"

#include <cmath>

namespace mikai::app::theme {
namespace {

QString text(const char* value) { return QString::fromLatin1(value); }

QString rgb(const QColor& color) { return rgbTriplet(color); }

double linearChannel(int value) {
    const double channel = value / 255.0;
    return channel <= 0.04045 ? channel / 12.92
                              : std::pow((channel + 0.055) / 1.055, 2.4);
}

double luminance(const QColor& color) {
    return 0.2126 * linearChannel(color.red()) + 0.7152 * linearChannel(color.green()) +
           0.0722 * linearChannel(color.blue());
}

}  // namespace

double contrastRatio(const QColor& a, const QColor& b) {
    const double first = luminance(a);
    const double second = luminance(b);
    const double lighter = std::max(first, second);
    const double darker = std::min(first, second);
    return (lighter + 0.05) / (darker + 0.05);
}

QColor contrastText(const QColor& background) {
    const double black = contrastRatio(background, QColor(Qt::black));
    const double white = contrastRatio(background, QColor(Qt::white));
    return black >= white ? QColor(Qt::black) : QColor(Qt::white);
}

Palette paletteFor(const Appearance& appearance) {
    Palette p;
    p.light = isLight(appearance);
    const QColor accent = accentColor(appearance);
    p.accentValue = accent;
    p.gradientValue = gradientColor(appearance);

    if (!p.light) {
        p.background = text(kBackground);
        p.text = text(kText);
        p.muted = text(kMuted);
        p.surface = text(kSurface);
        p.surfaceHover = text(kSurfaceHover);
        p.surfaceActive = text(kSurfaceActive);
        p.border = text(kBorder);
        p.danger = text(kDanger);
        p.videoBackground = text(kVideoBackground);
        p.wash = QStringLiteral("255, 255, 255");

        p.surfacePage = text(kSurfacePage);
        p.surfaceRaised = text(kSurfaceRaised);
        p.surfaceInset = text(kSurfaceInset);
        p.surfaceTag = text(kSurfaceTag);
        p.surfaceInput = text(kSurfaceInput);
        p.surfaceInputHover = text(kSurfaceInputHover);
        p.surfaceInputFocus = text(kSurfaceInputFocus);
        p.surfaceSubtle = text(kSurfaceSubtle);
        p.surfaceHoverStrong = text(kSurfaceHoverStrong);
        p.surfaceRow = text(kSurfaceRow);
        p.surfaceShortcutRow = text(kSurfaceShortcutRow);
        p.surfaceDeep = text(kSurfaceDeep);
        p.surfaceControl = text(kSurfaceControl);
        p.surfaceControlHover = text(kSurfaceControlHover);
        p.surfaceControlPressed = text(kSurfaceControlPressed);
        p.surfaceControlRaised = text(kSurfaceControlRaised);
        p.borderControl = text(kBorderControl);
        p.surfaceTileHover = text(kSurfaceTileHover);
        p.tableAlternateRow = text(kTableAlternateRow);
        p.surfaceControlRgb = text(kSurfaceControlRgb);
        p.surfaceDeepRgb = text(kSurfaceDeepRgb);

        p.textPrimary = text(kTextPrimary);
        p.textPrimaryRgb = text(kTextPrimaryRgb);
        p.textSecondary = text(kTextSecondary);
        p.textLabel = text(kTextLabel);
        p.textStrong = text(kTextStrong);
        p.textBright = text(kTextBright);
        p.textMutedSoft = text(kTextMutedSoft);
        p.textDisabled = text(kTextDisabled);
        p.textCheckLabel = text(kTextCheckLabel);
        p.textOnAccent = text(kTextOnAccent);

        p.successFill = text(kSuccessFill);
        p.successText = text(kSuccessText);
        p.successRgb = text(kSuccessRgb);
        p.warningFill = text(kWarningFill);
        p.warningText = text(kWarningText);
        p.warningRgb = text(kWarningRgb);
        p.dangerFill = text(kDangerFill);
        p.dangerText = text(kDangerText);
        p.dangerTextSoft = text(kDangerTextSoft);
        p.dangerRgb = text(kDangerRgb);

        p.titleBackground = text(kTitleBackground);
        p.titleSurface = text(kTitleSurface);
        p.titleSurfaceHover = text(kTitleSurfaceHover);
        p.titleText = text(kTitleText);
        p.titleMuted = text(kTitleMuted);
        p.titleBorder = text(kTitleBorder);
    } else {
        p.background = QStringLiteral("#f4f4f8");
        p.text = QStringLiteral("#17171d");
        p.muted = QStringLiteral("#5f5f6c");
        p.surface = QStringLiteral("#ffffff");
        p.surfaceHover = QStringLiteral("#ececf3");
        p.surfaceActive = QStringLiteral("#e2e2ec");
        p.border = QStringLiteral("#d6d6e0");
        p.danger = QStringLiteral("#cc3a48");
        p.videoBackground = QStringLiteral("#000000");
        p.wash = QStringLiteral("0, 0, 0");

        p.surfacePage = QStringLiteral("#f1f1f6");
        p.surfaceRaised = QStringLiteral("#ffffff");
        p.surfaceInset = QStringLiteral("#f6f6fa");
        p.surfaceTag = QStringLiteral("#f0f0f6");
        p.surfaceInput = QStringLiteral("#ffffff");
        p.surfaceInputHover = QStringLiteral("#fafafd");
        p.surfaceInputFocus = QStringLiteral("#ffffff");
        p.surfaceSubtle = QStringLiteral("#f4f4f9");
        p.surfaceHoverStrong = QStringLiteral("#e8e8f1");
        p.surfaceRow = QStringLiteral("#f7f7fb");
        p.surfaceShortcutRow = QStringLiteral("#f7f7fb");
        p.surfaceDeep = QStringLiteral("#eeeeF5");
        p.surfaceControl = QStringLiteral("#f2f2f8");
        p.surfaceControlHover = QStringLiteral("#e9e9f2");
        p.surfaceControlPressed = QStringLiteral("#e2e2ee");
        p.surfaceControlRaised = QStringLiteral("#ffffff");
        // The light pair carries the same 3:1 gate: 3.74:1 on the page bed and
        // 4.10:1 on a white settings card.
        p.borderControl = QStringLiteral("#7c7c8c");
        p.surfaceTileHover = QStringLiteral("#eeeef6");
        p.tableAlternateRow = QStringLiteral("#f7f7fb");
        p.surfaceControlRgb = QStringLiteral("242, 242, 248");
        p.surfaceDeepRgb = QStringLiteral("238, 238, 245");

        p.textPrimary = QStringLiteral("#17171d");
        p.textPrimaryRgb = QStringLiteral("23, 23, 29");
        p.textSecondary = QStringLiteral("#43434f");
        p.textLabel = QStringLiteral("#2c2c37");
        p.textStrong = QStringLiteral("#101015");
        p.textBright = QStringLiteral("#1b1b23");
        p.textMutedSoft = QStringLiteral("#6a6a78");
        p.textDisabled = QStringLiteral("#9b9ba6");
        p.textCheckLabel = QStringLiteral("#25252e");
        p.textOnAccent = QStringLiteral("#ffffff");

        p.successFill = QStringLiteral("#16a34a");
        p.successText = QStringLiteral("#15803d");
        p.successRgb = QStringLiteral("22, 163, 74");
        p.warningFill = QStringLiteral("#d97706");
        p.warningText = QStringLiteral("#b45309");
        p.warningRgb = QStringLiteral("217, 119, 6");
        p.dangerFill = QStringLiteral("#dc2626");
        p.dangerText = QStringLiteral("#b91c1c");
        p.dangerTextSoft = QStringLiteral("#b91c1c");
        p.dangerRgb = QStringLiteral("220, 38, 38");

        p.titleBackground = QStringLiteral("#f6f6fa");
        p.titleSurface = QStringLiteral("#ffffff");
        p.titleSurfaceHover = QStringLiteral("#ececf3");
        p.titleText = QStringLiteral("#111116");
        p.titleMuted = QStringLiteral("#5c5c69");
        p.titleBorder = QStringLiteral("#e2e2ea");
    }

    // The accent family is derived in both themes, so a chosen colour reaches
    // the primary action, focus rings, progress chunks and washes alike.
    p.accent = hexOf(accent);
    p.accentHover = hexOf(hoverOf(accent, p.light));
    p.accentInk = hexOf(contrastText(accent));
    p.accentSolid = p.accent;
    p.accentDeep = hexOf(p.light ? accent.darker(115) : accent.darker(112));
    p.accentDeepest = hexOf(p.light ? accent.darker(130) : accent.darker(122));
    p.accentSoft = hexOf(blend(accent, p.light ? QColor(Qt::black) : QColor(Qt::white),
                               p.light ? 0.2 : 0.32));
    p.accentSoftBright = hexOf(blend(accent, p.light ? QColor(Qt::black) : QColor(Qt::white),
                                     p.light ? 0.32 : 0.5));
    p.accentFocus = p.accentSoftBright;
    p.accentRgb = rgb(accent);
    p.accentSolidRgb = rgb(accent);
    p.accentDeepRgb = rgb(QColor(p.accentDeep));
    p.hairlineRgb = rgb(QColor(p.accentSoft));

    const QColor titleAccent = accent;
    p.titleAccent = hexOf(titleAccent);
    p.titleAccentHover = hexOf(hoverOf(titleAccent, p.light));
    p.titleAccentRgb = rgb(titleAccent);
    p.titleBackgroundRgb = rgb(QColor(p.titleBackground));
    p.titleMutedRgb = rgb(QColor(p.titleMuted));

    p.backgroundValue = QColor(p.background);
    p.surfaceValue = QColor(p.surface);
    p.titleBackgroundValue = QColor(p.titleBackground);
    return p;
}

}  // namespace mikai::app::theme
