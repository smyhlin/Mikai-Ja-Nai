#pragma once

#include <QObject>

class TestPlaybackSelection : public QObject {
  Q_OBJECT

 private slots:
  void emptyReleasesReturnsInvalid();
  void sharedLinkOverridesFavouriteAndDefault();
  void favouriteTeamWinsOverDefaultProvider();
  void missingFavouriteProviderFallsBackCleanly();
  void watchesAdvanceToNextNumericOrSpecialEpisode();
  void episodeOrderingHandlesDecimalsAndOva();
  void teamlessSharedLinkResolvesAcrossTeamBackedReleases();
};
