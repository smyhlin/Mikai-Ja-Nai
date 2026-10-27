#pragma once

// Shared spacing and control dimensions, in device independent pixels. This is
// the only place in src/app a dimension may be written down.
namespace mikai::app::theme {

inline constexpr int kSpacingNone = 0;
inline constexpr int kSpacingXs = 4;
inline constexpr int kSpacingSmall = 8;
inline constexpr int kSpacingMedium = 12;
inline constexpr int kSpacingLarge = 20;

inline constexpr int kRadiusSmall = 8;

inline constexpr int kPrimaryControlSize = 44;
inline constexpr int kSecondaryControlSize = 36;
inline constexpr int kSettingsControlSize = 36;
// A settings form's label column: wide enough for the longest translated label
// the tabs ship, so the fields to its right line up down the page.
inline constexpr int kSettingsFormLabelMinWidth = 132;
inline constexpr int kSettingsDialogMinWidth = 580;
inline constexpr int kSettingsDialogMinHeight = 600;
inline constexpr int kSettingsDialogDefaultWidth = 940;
inline constexpr int kSettingsDialogDefaultHeight = 820;
inline constexpr int kSettingsNavWidth = 200;
inline constexpr int kCatalogueFiltersWidth = 760;
inline constexpr int kCatalogueFiltersHeight = 780;
inline constexpr int kSettingsSwatchSize = 34;
inline constexpr int kSettingsSwitchWidth = 40;
inline constexpr int kSettingsSwitchHeight = 22;
inline constexpr int kSettingsSliderValueWidth = 52;
inline constexpr int kCatalogueChoiceRowHeight = 28;
inline constexpr int kCatalogueYearFieldWidth = 120;
inline constexpr int kTeamPickerMinHeight = 420;
inline constexpr int kTeamPickerDefaultHeight = 560;
inline constexpr int kSettingsPresetListMaxHeight = 440;
// Characters a settings combo reserves for its widest entry, so a long stored
// identifier cannot widen the page; the popup still shows the entry in full.
inline constexpr int kSettingsChoiceLengthChars = 30;
inline constexpr int kSettingsCompactChoiceLengthChars = 12;
inline constexpr int kNativeAssetsTableMaxHeight = 160;
inline constexpr int kBenchmarkSelectorHeight = 192;
inline constexpr int kBenchmarkResultsMaxHeight = 240;
inline constexpr int kBenchmarkEmptyHeight = 96;
inline constexpr int kComponentTableKindColumnWidth = 116;
inline constexpr int kComponentTableSizeColumnWidth = 76;
inline constexpr int kComponentTableStatusColumnWidth = 116;
inline constexpr int kComponentTableActionColumnWidth = 92;
inline constexpr int kComponentTableNameMinimumWidth = 120;
inline constexpr int kComponentTableCompactWidth =
    kComponentTableNameMinimumWidth + kComponentTableSizeColumnWidth +
    kComponentTableStatusColumnWidth + kComponentTableActionColumnWidth;
inline constexpr int kComponentTableMinimalWidth =
    kComponentTableNameMinimumWidth + kComponentTableStatusColumnWidth +
    kComponentTableActionColumnWidth;
inline constexpr int kNativeDimensionFieldWidth = 72;
inline constexpr int kIconSize = 18;
inline constexpr int kMenuItemMinHeight = 34;

inline constexpr int kSearchPosterWidth = 56;
inline constexpr int kSearchPosterHeight = 78;
inline constexpr int kSearchRowMinHeight = 92;
// The API serves at most 50 `/anime` rows per request whatever `limit` asks
// for, and it strides pages at the size it served. Asking for anything larger
// only makes the client count pages the server will never agree with.
inline constexpr int kSearchResultsPerPage = 50;

inline constexpr int kPagerChipWindow = 7;

inline constexpr int kEpisodesPerPage = 140;
inline constexpr int kEpisodeButtonWidth = 64;
inline constexpr int kEpisodeButtonHeight = 32;
inline constexpr int kEpisodeSectionMaxHeight = 340;

inline constexpr int kTitleCoverWidth = 188;
inline constexpr int kTitleCoverHeight = 268;
inline constexpr int kTitleDescriptionPreviewChars = 240;

// Title-page-only compact rhythm: the cover/text column gap, the text-column
// stack gap, selector row height, quality-chip geometry and episode-pill shape.
inline constexpr int kTitleColumnSpacing = 18;
inline constexpr int kTitleStackSpacing = 6;
inline constexpr int kTitleReleaseChipHeight = 32;
inline constexpr int kTitleBadgeHeight = 20;
inline constexpr int kTitleBadgeHPadding = 8;
// Concentric step-down for title-page pills and the episode panel they sit
// in: the panel keeps the app radius, the pill steps one token down.
inline constexpr int kTitlePillRadius = 6;

// ── Title page redesign ─────────────────────────────────────────────────────
// The media detail page uses its own wide, editorial rhythm: a fixed 1180
// content column, a 320x480 hero poster and a right-hand details sheet. These
// are title-page only; the settings-led tokens above still drive every other
// page.
inline constexpr int kTitlePageMaxWidth = 1180;
inline constexpr int kTitlePageInset = 32;
inline constexpr int kTitlePageCompactInset = 20;
inline constexpr int kTitleHeroSpacing = 48;
inline constexpr int kTitleSectionSpacing = 64;
inline constexpr int kTitleCompactBreakpoint = 900;

inline constexpr int kTitleHeroCoverWidth = 320;
inline constexpr int kTitleHeroCoverHeight = 480;
inline constexpr int kTitleCompactCoverWidth = 280;
inline constexpr int kTitleCompactCoverHeight = 420;

inline constexpr int kTitleControlHeight = 40;
inline constexpr int kTitleDetailsWidth = 460;
inline constexpr int kTitleDetailsCloseSize = 36;
inline constexpr int kTitleDetailsThumbRadius = 8;

inline constexpr int kTitleEpisodeButtonWidth = 64;
inline constexpr int kTitleEpisodeButtonHeight = 44;
inline constexpr int kTitleWatchedMarkWidth = 12;
inline constexpr int kTitleWatchedMarkHeight = 2;
inline constexpr int kTitleWatchedMarkInset = 6;
inline constexpr int kTitleLegendSwatchSize = 10;
inline constexpr int kTitleLegendSwatchRadius = 4;

// Typography. Font sizes are pixels, matching the reference's editorial scale;
// the compact heading is the stacked-viewport size.
inline constexpr int kTitleHeroTitlePixelSize = 44;
inline constexpr int kTitleHeroTitlePixelSizeCompact = 34;
inline constexpr int kTitleSubtitlePixelSize = 18;
inline constexpr int kTitleBodyPixelSize = 15;
inline constexpr int kTitleMetaPixelSize = 14;
inline constexpr int kTitleSectionHeadingPixelSize = 22;
inline constexpr int kTitleDetailsHeadingPixelSize = 18;
inline constexpr int kTitleWatchEpisodePixelSize = 16;
inline constexpr int kTitleCaptionPixelSize = 13;
inline constexpr int kTitleTinyPixelSize = 12;
inline constexpr int kTitleSmallPixelSize = 11;

inline constexpr int kTitleAccentRuleHeight = 2;
inline constexpr int kTitleAccentRuleWidth = 48;
/// The backdrop's footprint, the small surface it is blurred on, and how much
/// of its right edge dissolves into the page bed.
inline constexpr int kTitleBackdropWidth = 420;
inline constexpr int kTitleBackdropHeight = 560;
inline constexpr int kTitleBackdropSourceWidth = 96;
inline constexpr int kTitleBackdropSourceHeight = 128;
inline constexpr int kTitleBackdropBlurRadius = 18;
inline constexpr int kTitleBackdropEdgeFade = 150;

// Rounded geometry shared by the title page. Pills are half the control height,
// the same as the reference's `rounded-full` 40px buttons.
inline constexpr int kTitlePillRadiusRound = kTitleControlHeight / 2;
inline constexpr int kTitleCardRadius = 16;
inline constexpr int kTitlePosterRadius = 12;
inline constexpr int kTitleEpisodeRadius = 12;
inline constexpr int kTitleCircleControlSize = 36;
inline constexpr int kTitleCircleRadius = kTitleCircleControlSize / 2;
inline constexpr int kTitleChipPaddingH = 14;
inline constexpr int kTitleButtonPaddingH = 16;
inline constexpr int kTitleTabPaddingV = 10;
inline constexpr int kTitleTabPaddingH = 16;
inline constexpr int kTitleTabSpacing = 8;

inline constexpr int kCoverBackdropDownscale = 8;
inline constexpr double kCoverZoomMin = 0.1;
inline constexpr double kCoverZoomMax = 8.0;
inline constexpr double kCoverZoomStep = 1.25;

inline constexpr int kCarouselCardWidth = 164;
inline constexpr int kCarouselCardHeight = 276;
inline constexpr int kCarouselPosterWidth = 144;
inline constexpr int kCarouselPosterHeight = 190;
inline constexpr int kCarouselScrollStep = 492;
inline constexpr int kCarouselTitleHeight = 40;

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
