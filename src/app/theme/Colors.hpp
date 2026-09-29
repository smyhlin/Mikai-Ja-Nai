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

}  // namespace mikai::app::theme
