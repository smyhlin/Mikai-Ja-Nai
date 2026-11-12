#pragma once

#include <QtTest/QTest>

class TestCompareLoading : public QObject {
    Q_OBJECT

private slots:
    void openingIndicatorEndsOnPresentedFrame();
    void openingIndicatorTracksSourceReplacement();
    void durationPositionAndPlayingFanOut();
    void failedOpenIsNamedOnceAndRecovered();
};
