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

// ── Control geometry ────────────────────────────────────────────────────────
// One height for every action, filter and settings action. Qt's box model adds
// the border to a min-height, so the sheets receive the content height.
inline constexpr int kControlHeight = 40;
inline constexpr int kControlContentHeight = kControlHeight - 2;
// A watch CTA is the one action allowed to stand taller.
inline constexpr int kWatchCtaHeight = 44;
inline constexpr int kWatchCtaContentHeight = kWatchCtaHeight - 2;
inline constexpr int kWatchCtaInsetH = 20;
// Symmetric insets: a centred label must not sit off-axis.
inline constexpr int kControlInsetH = 12;
inline constexpr int kControlInsetHPrimary = 14;
// The style owns part of the glyph/label gap and pads the icon's planned box
// with the rest, inside the size the layout already reserved.
inline constexpr int kIconTextGap = 8;
inline constexpr int kIconGlyphPad = kIconTextGap / 2;
// The height every home/feed list row keeps, thumbnail or not.
inline constexpr int kRowHeight = 56;
// A status badge is one line; its counts live on the progress line below.
inline constexpr int kBadgeHeight = 24;
// Component card header: [check][title/description stretch][badge][size].
inline constexpr int kComponentCheckColumn = kControlHeight;
inline constexpr int kComponentBadgeWidth = 120;
inline constexpr int kComponentSizeWidth = 72;

// Non-action rows: table rows, switches and tabs keep the compact step.
inline constexpr int kSecondaryControlSize = 36;
inline constexpr int kSettingsControlSize = 36;
// A settings form's label column: wide enough for the longest translated label
// the tabs ship, so the fields to its right line up down the page.
inline constexpr int kSettingsFormLabelMinWidth = 132;
// The floor also has to hold the widest translated control a page puts in one
// row: the components tab's action captions need 322 px inside the page
// viewport, which is 312 px at 580. A platform whose font metrics run wider than
// Windows' would otherwise force horizontal panning at the minimum size.
inline constexpr int kSettingsDialogMinWidth = 600;
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

inline constexpr int kTitleDescriptionPreviewLines = 3;

// Title-page-only spacing for compact text groups.
inline constexpr int kTitleStackSpacing = 8;

// ── Title page redesign ─────────────────────────────────────────────────────
// The title page uses a capped editorial column, a compact 224x336 hero poster
// and a right-hand details sheet. These dimensions are title-page-only.
inline constexpr int kTitlePageMaxWidth = 1180;
inline constexpr int kTitlePageInset = 32;
inline constexpr int kTitlePageCompactInset = 20;
inline constexpr int kTitleHeroSpacing = 32;
inline constexpr int kTitleWorkspaceGroupGap = 16;
inline constexpr int kTitleSectionSpacing = 32;
inline constexpr int kTitleCompactBreakpoint = 900;
inline constexpr int kTitleHeroStackBreakpoint = 640;

inline constexpr int kTitleHeroCoverWidth = 224;
inline constexpr int kTitleHeroCoverHeight = 336;
inline constexpr int kTitleCompactCoverWidth = 168;
inline constexpr int kTitleCompactCoverHeight = 252;
inline constexpr int kTitleNarrowCoverWidth = 120;
inline constexpr int kTitleNarrowCoverHeight = 180;

inline constexpr int kTitleDetailsWidth = 460;
inline constexpr int kTitleDetailsCloseSize = 36;
inline constexpr int kTitleDetailsThumbRadius = 8;
inline constexpr int kTitleWatchPanelPadding = 16;
inline constexpr int kTitleWatchPanelGap = 12;

inline constexpr int kTitleEpisodeButtonWidth = 52;
inline constexpr int kTitleEpisodeButtonHeight = 36;
inline constexpr int kTitleWatchedMarkWidth = 12;
inline constexpr int kTitleWatchedMarkHeight = 2;
inline constexpr int kTitleWatchedMarkInset = 6;
inline constexpr int kTitleLegendSwatchSize = 10;
inline constexpr int kTitleLegendSwatchRadius = 4;

// Typography. Font sizes are pixels, matching the reference's editorial scale;
// the compact heading is the stacked-viewport size.
inline constexpr int kTitleHeroTitlePixelSize = 34;
inline constexpr int kTitleHeroTitlePixelSizeCompact = 28;
inline constexpr int kTitleHeroTitlePixelSizeNarrow = 24;
inline constexpr int kTitleSubtitlePixelSize = 14;
inline constexpr int kTitleBodyPixelSize = 14;
inline constexpr int kTitleMetaPixelSize = 13;
inline constexpr int kTitleSectionHeadingPixelSize = 22;
inline constexpr int kTitleDetailsHeadingPixelSize = 18;
inline constexpr int kTitleWatchEpisodePixelSize = 16;
inline constexpr int kTitleCaptionPixelSize = 13;
inline constexpr int kTitleTinyPixelSize = 12;
inline constexpr int kTitleSmallPixelSize = 11;

/// The backdrop's maximum hero-relative artwork footprint and its cached blur source.
inline constexpr int kTitleBackdropWidth = 420;
inline constexpr int kTitleBackdropHeight = 560;
inline constexpr int kTitleBackdropSourceWidth = 96;
inline constexpr int kTitleBackdropSourceHeight = 128;
inline constexpr int kTitleBackdropBlurRadius = 18;

// Decorative label pills and content surfaces keep their independent geometry.
inline constexpr int kTitlePillRadiusRound = kControlHeight / 2;
inline constexpr int kTitleCardRadius = 16;
inline constexpr int kTitlePosterRadius = 12;
inline constexpr int kTitleCircleControlSize = 36;
inline constexpr int kTitleChipPaddingH = 14;
inline constexpr int kTitleButtonPaddingH = kControlInsetHPrimary;
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
inline constexpr int kHomeScrollBarExtent = kSpacingMedium;
inline constexpr int kHomeScrollHandleInset = 2;
inline constexpr int kHomeScrollHandleMinHeight = 36;
inline constexpr int kHomeScrollContentGap = kSpacingSmall;
inline constexpr int kHomeFeedPosterWidth = 34;
inline constexpr int kHomeFeedPosterHeight = 48;
inline constexpr int kHomeCarouselMaxTitles = 18;
inline constexpr int kHomeFeedMaxRows = 5;
inline constexpr int kHomeRecentMaxTitles = 12;
inline constexpr int kRelatedCarouselMax = 6;

inline constexpr int kCarouselCardSpacing = 10;
inline constexpr int kCarouselTrackInset = 2;

inline constexpr int kSearchSidebarWidth = 208;
inline constexpr int kSearchSidebarRailWidth = 64;
inline constexpr int kSearchSidebarInset = 16;
inline constexpr int kSearchSidebarRailInset = kSpacingSmall;
inline constexpr int kSearchSidebarBrandIconSize = 28;
inline constexpr int kSearchSidebarActionIconSize = 20;
inline constexpr int kSearchSidebarBrandFontSize = 16;
inline constexpr int kSearchSidebarIndicatorWidth = 3;
inline constexpr int kSearchSidebarExpandedMinWidth =
    kSearchSidebarWidth + 2 * kCarouselTrackInset + 2 * kHomeCardWidth +
    kCarouselCardSpacing + 2 * kSpacingLarge + kHomeScrollBarExtent +
    kHomeScrollContentGap;

// ── Search page header ──────────────────────────────────────────────────────
inline constexpr int kSearchHeaderSpacing = kSpacingSmall;
inline constexpr int kSearchKeycapRadius = 6;
// One hairline, drawn by the keycap itself: the same width on every edge.
inline constexpr int kSearchKeycapBorder = 1;
inline constexpr int kSearchFieldInset = 12;
inline constexpr int kSearchFieldTrailingInset = 6;
inline constexpr int kSearchKeycapPaddingH = 8;
inline constexpr int kSearchKeycapContentHeight = kBadgeHeight;
inline constexpr int kSearchKeycapMinFieldWidth = 240;
// The quota pill's status dot: a small filled disc, never an icon.
inline constexpr int kSearchDotSize = 6;
inline constexpr int kSearchDotRadius = kSearchDotSize / 2;
inline constexpr int kSearchDotSpacing = 6;
inline constexpr int kSearchPillPaddingLeft = 10;
inline constexpr int kSearchPillPaddingRight = kSearchPillPaddingLeft;

}  // namespace mikai::app::theme
