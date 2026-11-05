#pragma once

#include <QtTest/QTest>

class TestPublicApiUi : public QObject {
  Q_OBJECT

 private slots:
  void titleMetadataPreservesUnknownCountsBadgesAndFutureAiring();
  void aWatchedFractionalEpisodeKeepsItsClassificationBadge();
  void licensedTitleShowsLegalInformation();
  void anUnlicensedTitleShowsNoLegalInformation();
  void catalogueFiltersRetainCanonicalSelectionsAndReset();
  void catalogueFilterSearchNarrowsChoices();
  void teamBrowsingAndFilteringKeepSelectionsConsistent();
  void teamLinksShowServiceNamesAndOpenOnlyOnActivation();
  void homeAndEscapeReturnFromTeamAndSearch();
  void escapeCancelsPendingTeamNavigationAtHome();
  void titleHomeSkipsHistoryAndEscapeUnwindsDetailsAndHistory();
  void explicitTitleAsksBeforePlaybackUnlessAllowed();
  void liveCoverShadingChangesWithoutResizing();
  void headerLogoTracksLightAndDarkAppearance();
  void invalidSelectedPosterFallsBackToHint();
  void decodedPosterScalesAfterExifRotation();
  void titleSectionHeadersLetTheGradientThrough();
  void gradientPreviewKeepsItsLatestColorAndCancelRestoresIt();
  void aComponentsTabOutlivesTheProvisionerItsWindowOwns();
  void providerPreferenceLeadsMoonAndFallsBackDeterministically();
  void moonLeadsTheProviderTabsAndReleaseChips();
  void aTitleWithoutMoonKeepsItsFirstProvider();
  void anExplicitProviderChoiceOutlivesTheReleaseRebuild();
  void theReleaseEmbedResolvesToTheMoonSource();
  void aViewerChoiceLeadsTheProgrammaticWatchEmbed();
  void actionRowsShareOneControlHeightAndEqualWidths();
  void wrappedScoreLinesFillTheirTextColumn();
  void componentCardsKeepASingleLineBadgeAndASeparateCountLine();
  void theUpscalersPageListsEveryPinnedModel();
  void settingsPagesFitTheDialogMinimum();
  void managerActionRowsShareOneWidth();
};

int runTestPublicApiUi(int argc, char** argv);
