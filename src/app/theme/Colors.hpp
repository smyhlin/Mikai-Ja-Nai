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
// A neutral action's fill and boundary. The fill is deliberately close to the
// bed (1.28:1); WCAG 1.4.11 measures the component against what surrounds it,
// and the boundary is what carries it: kBorderControl is 3.32:1 on both the
// application bed and the settings page bed and 3.15:1 on a settings card.
// Do not dim either one - a neutral control below 3:1 is the defect this pair
// exists to fix.
inline constexpr const char* kSurfaceControlRaised = "#222430";
inline constexpr const char* kBorderControl = "#63636f";
inline constexpr const char* kSurfaceInputHover = "#120f1c";
inline constexpr const char* kSurfaceInputFocus = "#141120";
inline constexpr const char* kSurfaceTileHover = "#211c30";
inline constexpr const char* kTableAlternateRow = "#14111f";

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
// alpha without restating the hue. The accent's own triplet is not here: it is
// resolved per appearance in Palette, because the accent is a preference.
inline constexpr const char* kSuccessRgb = "52, 211, 153";
inline constexpr const char* kWarningRgb = "251, 191, 36";
inline constexpr const char* kDangerRgb = "248, 113, 113";
inline constexpr const char* kSurfaceControlRgb = "29, 25, 43";
inline constexpr const char* kSurfaceDeepRgb = "15, 13, 23";
inline constexpr const char* kTextPrimaryRgb = "243, 241, 248";

// ── Title page surfaces ─────────────────────────────────────────────────────
// The media detail page keeps the reference's neutral near-black bed. These
// aliases are scoped to the title page stylesheet; the application palette
// above is unchanged, so search, settings and the player keep their colours.
// The page's accent is not a token: Palette derives it from the chosen accent,
// so the page cannot stay steel-blue while the rest of the app changes.
inline constexpr const char* kTitleBackground = "#0b0b0e";
inline constexpr const char* kTitleSurface = "#16161d";
inline constexpr const char* kTitleSurfaceHover = "#1e1e26";
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

// ── Search header ───────────────────────────────────────────────────────────
// The main page's two-row header keeps its own quiet surfaces: an input bed, a
// secondary-control fill, the passive quota pill, the Enter keycap, and the
// primary action. The rules that use them are scoped to QWidget#searchPage, so
// the catalogue, title page, settings and player keep the palette above.
//
// The primary action is the chosen accent like everywhere else; there is no
// shipped-app exception. The input hairline is a white alpha because it is an
// input edge, not a control boundary - controls take kBorderControl.
inline constexpr const char* kSearchInput = "#101014";
inline constexpr const char* kSearchSecondaryHover = "#22222c";
inline constexpr const char* kSearchInputHairline = "rgba(255, 255, 255, 0.10)";
inline constexpr const char* kSearchPillHairline = "rgba(255, 255, 255, 0.08)";
inline constexpr const char* kSearchKeycapHairline = "rgba(255, 255, 255, 0.14)";
inline constexpr const char* kSearchPill = "#14141b";
inline constexpr const char* kSearchPillText = "#a1a1aa";
inline constexpr const char* kSearchKeycap = "#22222b";
inline constexpr const char* kSearchDotHealthy = "#22c55e";
inline constexpr const char* kSearchDotUnknown = "#5c5c68";

}  // namespace mikai::app::theme
