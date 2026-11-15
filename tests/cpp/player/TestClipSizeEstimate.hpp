#pragma once

#include <QtTest/QTest>

class TestClipSizeEstimate : public QObject {
    Q_OBJECT

private slots:
    void analyzesImageAndSummarizesAdjacentMotion();
    void ignoresSpacedSamplesForTemporalMotion();
    void rejectsUnknownAndNonFiniteInputs();
    void estimatesActualEncoderProfilesAndAudioChanges();
    void tinyVideoGetsBroadUncalibratedAudioBounds();
    void sharesExactOutputGeometry();
};
