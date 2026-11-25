#pragma once

#include <QObject>

class TestPlaybackLink : public QObject {
  Q_OBJECT

 private slots:
  void roundTripFullLinkWithSpecialEpisodeAndTime();
  void partialPlaybackParamsDegradeToTitleOnly();
  void rejectsInvalidSchemesAndBadTimecodes();
};
