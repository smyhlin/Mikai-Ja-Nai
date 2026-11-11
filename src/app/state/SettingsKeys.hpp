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
// Deliberately NOT migrated to this header, by layering (the player library
// must not include src/app/ state, and these keys already have a single
// named home there): player/anime4k, playback/*, player/native/* and
// player/presetShortcuts/* — owned by VideoSurface.hpp, PlayerDialog.cpp /
// PlayerDialogProcessing.cpp, NativePrefs.cpp and PresetShortcuts.hpp
// respectively. player/animejanai/backend IS recorded above because app
// files read it; the player keeps a file-local constant with the identical
// value, and the two spellings must stay identical.

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

// OpenSubtitles credential (SettingsGeneralTab / SettingsDialog read it;
// PlayerDialog keeps a file-local spelling of the same value — the player
// layer cannot include this header).
inline constexpr std::string_view kOpenSubtitlesApiKey = "access/openSubtitlesApiKey";

// AnimeJanai backend selection (ComponentStoreTab / SettingsBenchmarkTab).
inline constexpr std::string_view kAnimeJanaiBackend = "player/animejanai/backend";

// Home feeds (Feeds.cpp) and recent titles (SearchPage.cpp).
inline constexpr std::string_view kHomeFeeds = "home/feeds";
inline constexpr std::string_view kHomeRecentTitles = "home/recentTitles";

// Quality cache backing key (QualityCache.cpp).
inline constexpr std::string_view kQualityUrls = "quality/urls";

// Interface language (I18n.hpp; kept name so existing users, including
// probes, resolve).
inline constexpr std::string_view kUiLanguageSettingKey = "ui/language";

}  // namespace mikai::app
