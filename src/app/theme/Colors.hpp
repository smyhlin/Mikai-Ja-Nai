#pragma once

// Central color palette for the desktop client. This header's tokens are the
// sanctioned path for every colour src/app paints or styles. Raw hex literals
// do still exist in a few deliberate, documented spots: Palette.cpp's light
// theme value table, the fixed accent presets in Appearance.cpp/.hpp and
// SwatchButton.cpp's accent swatches, and the RGB literal Icons.cpp paints a
// chevron's disabled state with - everywhere else goes through a theme token.
namespace mikai::app::theme {

inline constexpr const char* kBackground = "#0b0b0f";
inline constexpr const char* kSurface = "#16161d";
inline constexpr const char* kSurfaceHover = "#22222b";
inline constexpr const char* kSurfaceActive = "#2b2b35";
inline constexpr const char* kAccent = "#bdacff";
inline constexpr const char* kAccentHover = "#cfc4ff";
inline constexpr const char* kDanger = "#ff6b78";
inline constexpr const char* kText = "#e7e7ea";
inline constexpr const char* kMuted = "#a0a0ab";
inline constexpr const char* kBorder = "#30303a";
inline constexpr const char* kVideoBackground = "#000000";

// The cover viewer floats a sharp image over a blurred copy of itself and
// dims the result; the dim is an alpha, not a fill, or the blur is erased.
inline constexpr int kCoverBackdropAlpha = 150;

// ── Settings surfaces ───────────────────────────────────────────────────────
// The settings pages layer their own elevations and control states over the
// application palette: a raised card on a darker page, an inset sub-panel, the
// input fill shared by fields, trees and result tables, and the accent ramp the
// primary action, checked marks and progress chunks are drawn from. Values are
// adopted from the redesigned settings surface.
//
// The settings pages bed their own background a shade below the application
// page colour, so the cards stacked on it read as raised.
inline constexpr const char* kSurfacePage = "#0c0a12";
inline constexpr const char* kSurfaceRaised = "#13111c";
inline constexpr const char* kSurfaceInset = "#191625";
inline constexpr const char* kSurfaceTag = "#181524";
inline constexpr const char* kSurfaceInput = "#0e0c16";
inline constexpr const char* kSurfaceSubtle = "#1c1829";
inline constexpr const char* kSurfaceHoverStrong = "#231e33";
inline constexpr const char* kSurfaceRow = "#161321";
// The preset shortcut rows sit between the page bed and a raised card.
inline constexpr const char* kSurfaceShortcutRow = "#161320";
inline constexpr const char* kSurfaceDeep = "#0f0d17";
inline constexpr const char* kSurfaceControl = "#1d192b";
inline constexpr const char* kSurfaceControlHover = "#262038";
inline constexpr const char* kSurfaceControlPressed = "#181424";
inline constexpr const char* kSurfaceInputHover = "#120f1c";
inline constexpr const char* kSurfaceInputFocus = "#141120";
inline constexpr const char* kSurfaceTileHover = "#211c30";
inline constexpr const char* kTableAlternateRow = "#14111f";

inline constexpr const char* kAccentFocus = "#a78bfa";
inline constexpr const char* kAccentSolid = "#8b5cf6";
inline constexpr const char* kAccentDeep = "#7c3aed";
inline constexpr const char* kAccentDeepest = "#6d28d9";
inline constexpr const char* kAccentSoft = "#c4b5fd";
inline constexpr const char* kAccentSoftBright = "#ddd6fe";
inline constexpr const char* kAccentOnFill = "#f8f7fc";

inline constexpr const char* kTextPrimary = "#f3f1f8";
inline constexpr const char* kTextSecondary = "#c4bdd6";
inline constexpr const char* kTextLabel = "#d8d2e8";
inline constexpr const char* kTextStrong = "#f5f3ff";
inline constexpr const char* kTextBright = "#ede9fe";
inline constexpr const char* kTextMutedSoft = "#968fa8";
inline constexpr const char* kTextDisabled = "#69627a";
// The label of a checkable control, a touch lighter than a plain form label.
inline constexpr const char* kTextCheckLabel = "#e4dff2";
inline constexpr const char* kTextOnAccent = "#ffffff";

inline constexpr const char* kSuccessFill = "#34d399";
inline constexpr const char* kSuccessText = "#6ee7b7";
inline constexpr const char* kWarningFill = "#fbbf24";
inline constexpr const char* kWarningText = "#fde68a";
inline constexpr const char* kDangerFill = "#f87171";
inline constexpr const char* kDangerText = "#fca5a5";
inline constexpr const char* kDangerTextSoft = "#fecaca";

// Hue triplets, for the washes QSS composes with an alpha channel:
// `rgba(%1, 0.12)`. Keeping the components here is what lets a rule vary its
// alpha without restating the hue.
inline constexpr const char* kAccentRgb = "167, 139, 250";
inline constexpr const char* kAccentSolidRgb = "139, 92, 246";
inline constexpr const char* kAccentDeepRgb = "124, 58, 237";
inline constexpr const char* kHairlineRgb = "196, 181, 253";
inline constexpr const char* kSuccessRgb = "52, 211, 153";
inline constexpr const char* kWarningRgb = "251, 191, 36";
inline constexpr const char* kDangerRgb = "248, 113, 113";
inline constexpr const char* kSurfaceControlRgb = "29, 25, 43";
inline constexpr const char* kSurfaceDeepRgb = "15, 13, 23";
inline constexpr const char* kTextPrimaryRgb = "243, 241, 248";

// ── Title page surfaces ─────────────────────────────────────────────────────
// The media detail page keeps the reference's neutral near-black bed and a
// desaturated steel-blue accent, distinct from the violet settings ramp. These
// aliases are scoped to the title page stylesheet; the application palette
// above is unchanged, so search, settings and the player keep their colours.
inline constexpr const char* kTitleBackground = "#0b0b0e";
inline constexpr const char* kTitleSurface = "#16161d";
inline constexpr const char* kTitleSurfaceHover = "#1e1e26";
inline constexpr const char* kTitleAccent = "#7b8ab8";
inline constexpr const char* kTitleAccentHover = "#98a6d2";
inline constexpr const char* kTitleText = "#ffffff";
inline constexpr const char* kTitleMuted = "#8a8a98";
// The border and the raised hover fill are the same value in the reference;
// the two names keep the roles legible at the call site.
inline constexpr const char* kTitleBorder = "#1e1e26";

// The backdrop is an alpha, like the cover viewer's dim: the poster reads as
// atmosphere behind the text. The fade midpoint veils it a little further before
// the page bed takes over at the bottom edge.
inline constexpr int kTitleBackdropAlpha = 77;
inline constexpr int kTitleBackdropFadeAlpha = 60;

inline constexpr const char* kTitleAccentRgb = "123, 138, 184";
inline constexpr const char* kTitleBackgroundRgb = "11, 11, 14";
inline constexpr const char* kTitleMutedRgb = "138, 138, 152";

}  // namespace mikai::app::theme
