#pragma once

// Central color palette for the desktop client. Every color the app paints or
// styles comes from here; no raw hex literal is allowed anywhere else in src/app.
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

}  // namespace mikai::app::theme
