#pragma once

#include <QtTest/QTest>

class TestCompareClip : public QObject {
    Q_OBJECT

private slots:
    void mp4ContainsTimedJpegSamples();
    void animatedWebPContainsTimedFrames();
    void cancellationPreservesExistingDestination();
    void rejectsClipsLongerThanTenSeconds();
};
