// Model parsing: the wire -> struct layer.
//
// Every fixture here is the shape the public API actually sends, because the
// bugs this suite exists for were all "the parser quietly dropped a field the UI
// needs": a ref that only exists under a prefixed id, a cover that only exists
// on a UA poster, an external link pointing at the wrong site.

#pragma once

#include <QtTest/QTest>

class TestModels : public QObject {
  Q_OBJECT

 private slots:
  // Ref resolution.
  void summaryRefPrefersMikaiId();
  void summaryRefFallsBackToPrefixedId_data();
  void summaryRefFallsBackToPrefixedId();
  void summaryRefIsEmptyWithoutAnyId();
  void summaryRefStringifiesNumericIds();
  void detailRefIsTheMikaiIdOnly();

  // Titles and artwork.
  void displayNamePrefersUaThenEnglishThenOriginal_data();
  void displayNamePrefersUaThenEnglishThenOriginal();
  void displayNameFallsBackToQuestionMark();
  void summaryThumbPrefersSmallWebp_data();
  void summaryThumbPrefersSmallWebp();
  void summaryPosterPrefersBigWebp_data();
  void summaryPosterPrefersBigWebp();
  void emptySummaryCarriesNoArtwork();
  void summaryCarriesPlannedEpisodesAndFormat();
  void summaryWithoutPlannedCountLeavesItAbsent();

  // Detail.
  void detailReadsEveryScalarField();
  void detailReadsEpisodeStatsAndScores();
  void detailPreservesFractionalClassificationAndLicenseMetadata();
  void episodeClassificationUsesKindsAndFractionalCanonicalRanges();
  void detailKeepsUnknownAndZeroCountsDistinct();
  void selectedUaPosterWinsCoverAndRetainsCredit();
  void aSelectedPosterWithoutAnImageIsNotTheCoverAndIsNotCredited();
  void theOriginalPosterIsCreditedToNobody();
  void detailWithoutScoresLeavesThemAbsent();
  void detailReadsGenresAndTags();
  void detailReadsUaPostersRelationsAndSimilar();
  void detailToleratesAMinimalPayload();

  // Cover.
  void coverUrlPrefersUaPosterOverApiPoster();
  void coverUrlWalksUaPostersThenSizesInOrder_data();
  void coverUrlWalksUaPostersThenSizesInOrder();
  void coverUrlPrefersWebpOverJpg();
  void webpIsDecodable();
  void coverUrlFallsBackToApiPoster();
  void emptyDetailHasNoCover();

  // External links.
  void externalLinksCoverEveryKnownSite();
  void externalLinksSkipAbsentIds();
  void externalLinksFollowTheFixedOrder();

  // Updates.
  void updateReadsTeamsAndEpisode();
  void updateRetainsEmbedUrlIdentity();
  void updateWithoutTeamsIsStillUsable();

  // Player.
  void playerReadsReleasesEpisodesAndSources();
  void playerPreservesFullLicenseObjectAndEpisodeKind();
  void playerToleratesMissingCollections();
  void playerDefaultsProviderToQuestionMark();
  void episodeNumberAcceptsEveryWireShape_data();
  void episodeNumberAcceptsEveryWireShape();

  // Public page URL.
  void animePageUrlAcceptsBareAndPrefixedRefs_data();
  void animePageUrlAcceptsBareAndPrefixedRefs();
};
