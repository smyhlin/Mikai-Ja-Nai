// Release ordering: the rules that decide which track a user hears first.

#pragma once

#include <QtTest/QTest>

class TestReleases : public QObject {
  Q_OBJECT

 private slots:
  void playableCountUsesEpisodesCountWhenNoEpisodesListed();
  void playableCountCountsEpisodesWithASource();
  void playableCountSkipsEpisodesWithEmptyEmbed();
  void playableCountIsZeroForAnEmptyRelease();

  void sortPutsVoiceBeforeSub();
  void sortPutsLoudestDubFirstWithinVoice();
  void sortPutsLoudestSubFirstWithinSub();
  void sortTieBreaksByTeamName();
  void sortPlacesUnknownKindAfterKnownOnes();
  void sortIsStableForIdenticalReleases();
  void sortDoesNotMutateTheInput();
  void sortHandlesReleasesWithoutTeams();
};
