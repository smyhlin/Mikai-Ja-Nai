// The one registry of QSettings keys (P2-01 / M17).
//
// Every QSettings key the app layer reads or writes by name lives here, as a
// `constexpr std::string_view`, so a typo'd literal cannot silently orphan a
// setting and a rename touches one line. Key VALUES never change: renaming a
// key orphans every stored user setting.
//
// Use `QString::fromUtf8(<constant>)` at the call site (all keys are ASCII,
// so Latin-1 and UTF-8 agree; fromUtf8 is the conversion Qt documents for
// a string view).
//
// Player-owned settings stay with their declarations to preserve app/player
// layering. The AnimeJanai backend key is listed here because app code reads it.

#pragma once

#include <string_view>

namespace mikai::app {

// Appearance (AppearanceStore.cpp).
inline constexpr std::string_view kCustomizationTheme = "customization/theme";
inline constexpr std::string_view kCustomizationTitleLanguage = "customization/titleLanguage";
inline constexpr std::string_view kCustomizationNameLanguage = "customization/nameLanguage";
inline constexpr std::string_view kCustomizationCoverGradient = "customization/coverGradient";
inline constexpr std::string_view kCustomizationAdultContent = "customization/adultContent";
inline constexpr std::string_view kCustomizationAccent = "customization/accent";
inline constexpr std::string_view kCustomizationRadiusStep = "customization/radiusStep";
inline constexpr std::string_view kCustomizationBackgroundGradient =
    "customization/backgroundGradient";
inline constexpr std::string_view kCustomizationGradientIntensity =
    "customization/gradientIntensity";
inline constexpr std::string_view kCustomizationGradientHeight = "customization/gradientHeight";
inline constexpr std::string_view kCustomizationGradientMode = "customization/gradientMode";
inline constexpr std::string_view kCustomizationGradientColor = "customization/gradientColor";

// Content names (Labels.cpp).
inline constexpr std::string_view kPlaybackPreferredProvider = "playback/preferredProvider";

// OpenSubtitles credential.
inline constexpr std::string_view kOpenSubtitlesApiKey = "access/openSubtitlesApiKey";

// AnimeJanai backend selection (ComponentStoreTab / SettingsBenchmarkTab).
inline constexpr std::string_view kAnimeJanaiBackend = "player/animejanai/backend";

// Home feeds (Feeds.cpp) and recent titles (SearchPage.cpp).
inline constexpr std::string_view kHomeFeeds = "home/feeds";
inline constexpr std::string_view kHomeRecentTitles = "home/recentTitles";

// Watch history (WatchHistory.cpp).
inline constexpr std::string_view kWatchHistoryKey = "title/watchHistory";

// Quality cache backing key (QualityCache.cpp).
inline constexpr std::string_view kQualityUrls = "quality/urls";

// Interface language (I18n.hpp; kept name so existing users, including
// probes, resolve).
inline constexpr std::string_view kUiLanguageSettingKey = "ui/language";

}  // namespace mikai::app
