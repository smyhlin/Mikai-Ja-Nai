#pragma once

#include "theme/Appearance.hpp"

#include <QColor>
#include <QString>

namespace mikai::app::theme {

/// Every colour the sheets and the painted surfaces use, resolved for one
/// appearance. The names mirror Colors.hpp, which stays the source of the dark
/// values; a light appearance substitutes its own values and the accent family
/// is derived from the chosen accent.
struct Palette {
    QString background;
    QString text;
    QString muted;
    QString surface;
    QString surfaceHover;
    QString surfaceActive;
    QString border;
    QString accent;
    QString accentHover;
    QString danger;
    QString videoBackground;
    /// The triplet a low-contrast wash is composed from: white on a dark bed,
    /// black on a light one.
    QString wash;

    QString surfacePage;
    QString surfaceRaised;
    QString surfaceInset;
    QString surfaceTag;
    QString surfaceInput;
    QString surfaceInputHover;
    QString surfaceInputFocus;
    QString surfaceSubtle;
    QString surfaceHoverStrong;
    QString surfaceRow;
    QString surfaceShortcutRow;
    QString surfaceDeep;
    QString surfaceControl;
    QString surfaceControlHover;
    QString surfaceControlPressed;
    QString surfaceTileHover;
    QString tableAlternateRow;

    QString accentFocus;
    QString accentSolid;
    QString accentDeep;
    QString accentDeepest;
    QString accentSoft;
    QString accentSoftBright;
    QString accentOnFill;
    QString accentRgb;
    QString accentSolidRgb;
    QString accentDeepRgb;
    QString hairlineRgb;

    QString textPrimary;
    QString textPrimaryRgb;
    QString textSecondary;
    QString textLabel;
    QString textStrong;
    QString textBright;
    QString textMutedSoft;
    QString textDisabled;
    QString textCheckLabel;
    QString textOnAccent;

    QString successFill;
    QString successText;
    QString successRgb;
    QString warningFill;
    QString warningText;
    QString warningRgb;
    QString dangerFill;
    QString dangerText;
    QString dangerTextSoft;
    QString dangerRgb;
    QString surfaceControlRgb;
    QString surfaceDeepRgb;

    QString titleBackground;
    QString titleSurface;
    QString titleSurfaceHover;
    QString titleText;
    QString titleMuted;
    QString titleBorder;
    QString titleAccent;
    QString titleAccentHover;
    QString titleAccentRgb;
    QString titleBackgroundRgb;
    QString titleMutedRgb;

    QColor backgroundValue;
    QColor surfaceValue;
    QColor titleBackgroundValue;
    QColor accentValue;
    QColor gradientValue;

    bool light = false;
};

Palette paletteFor(const Appearance& appearance);

}  // namespace mikai::app::theme
