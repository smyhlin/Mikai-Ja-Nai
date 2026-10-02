#pragma once

// Shared spacing and control dimensions, in device independent pixels. This is
// the only place in src/app a dimension may be written down.
namespace mikai::app::theme {

inline constexpr int kSpacingXs = 4;
inline constexpr int kSpacingSmall = 8;
inline constexpr int kSpacingMedium = 12;
inline constexpr int kSpacingLarge = 20;

inline constexpr int kRadiusSmall = 8;
inline constexpr int kRadius = 12;
inline constexpr int kRadiusLarge = 16;

inline constexpr int kPrimaryControlSize = 44;
inline constexpr int kSecondaryControlSize = 36;
inline constexpr int kSettingsControlSize = 36;
inline constexpr int kSettingsDialogMinWidth = 560;
inline constexpr int kSettingsDialogMinHeight = 600;
inline constexpr int kSettingsDialogDefaultWidth = 760;
inline constexpr int kSettingsDialogDefaultHeight = 780;
inline constexpr int kSettingsPresetListMaxHeight = 440;
// Characters a settings combo reserves for its widest entry, so a long stored
// identifier cannot widen the page; the popup still shows the entry in full.
inline constexpr int kSettingsChoiceLengthChars = 30;
inline constexpr int kNativeAssetsTableMaxHeight = 160;
inline constexpr int kNativeDimensionFieldWidth = 72;
inline constexpr int kIconSize = 18;
inline constexpr int kMenuItemMinHeight = 34;

inline constexpr int kSearchPosterWidth = 56;
inline constexpr int kSearchPosterHeight = 78;
inline constexpr int kSearchRowMinHeight = 92;
inline constexpr int kSearchResultsPerPage = 55;

inline constexpr int kPagerChipWindow = 7;

inline constexpr int kEpisodesPerPage = 140;
inline constexpr int kEpisodeButtonWidth = 72;
inline constexpr int kEpisodeButtonHeight = 40;
inline constexpr int kEpisodeSectionMaxHeight = 420;

inline constexpr int kTitleCoverWidth = 188;
inline constexpr int kTitleCoverHeight = 268;
inline constexpr int kTitleDescriptionPreviewChars = 210;

inline constexpr int kCoverBackdropDownscale = 26;
inline constexpr double kCoverZoomMin = 0.1;
inline constexpr double kCoverZoomMax = 8.0;
inline constexpr double kCoverZoomStep = 1.25;

inline constexpr int kCarouselCardWidth = 156;
inline constexpr int kCarouselCardHeight = 248;
inline constexpr int kCarouselPosterWidth = 132;
inline constexpr int kCarouselPosterHeight = 174;
inline constexpr int kCarouselScrollStep = 468;
inline constexpr int kCarouselTitleHeight = 38;

inline constexpr int kHomeCardWidth = 360;
inline constexpr int kHomeCardHeight = 140;
inline constexpr int kHomePosterWidth = 80;
inline constexpr int kHomePosterHeight = 114;
inline constexpr int kHomeCarouselScrollStep = 740;
inline constexpr int kHomeFeedPosterWidth = 34;
inline constexpr int kHomeFeedPosterHeight = 48;
inline constexpr int kHomeCarouselMaxTitles = 18;
inline constexpr int kHomeFeedMaxRows = 5;
inline constexpr int kHomeRecentMaxTitles = 12;
inline constexpr int kRelatedCarouselMax = 6;

// Carousel card spacing between entries, in the track layout.
inline constexpr int kCarouselCardSpacing = 10;

}  // namespace mikai::app::theme
