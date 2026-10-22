#pragma once

#include <QtTest/QTest>

class TestPublicApiUi : public QObject {
  Q_OBJECT

 private slots:
  void titleMetadataPreservesUnknownCountsBadgesAndFutureAiring();
  void aWatchedFractionalEpisodeKeepsItsClassificationBadge();
  void licensedTitleShowsLegalInformationAndSelectedPosterCredit();
  void anUnlicensedTitleShowsNoLegalInformation();
  void catalogueFiltersRetainCanonicalSelectionsAndReset();
  void catalogueFilterSearchNarrowsChoices();
  void teamBrowsingAndFilteringKeepSelectionsConsistent();
  void teamLinksShowServiceNamesAndOpenOnlyOnActivation();
  void homeAndEscapeReturnFromTeamAndSearch();
  void escapeCancelsPendingTeamNavigationAtHome();
  void titleHomeSkipsHistoryAndEscapeUnwindsDetailsAndHistory();
};

int runTestPublicApiUi(int argc, char** argv);
