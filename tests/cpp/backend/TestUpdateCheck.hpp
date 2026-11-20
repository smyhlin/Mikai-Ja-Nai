#pragma once

#include <QtTest/QTest>

class TestUpdateCheck : public QObject {
    Q_OBJECT
private slots:
    void comparesNumericVersionComponents();
    void rejectsMalformedAndUnsafeResponses();
    void ignoresPrereleaseAndDrafts();
  void parsesReleaseAssetsAndSha256Sums();
  void parsesNightlyPrereleaseAndChannelSwitch();


};

int runTestUpdateCheck(int argc, char** argv);
