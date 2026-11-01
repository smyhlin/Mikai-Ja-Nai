#pragma once

#include <QtTest/QTest>

class TestSkipSegments : public QObject {
    Q_OBJECT

private slots:
    void thePositionDecidesWhichOverlappingSegmentsAreOffered();
    void eachOverlappingOfferIsActionableAndSeeksToItsOwnEnd();
    void aListThatArrivesLateIsOfferedAtTheCurrentPosition();
    void offersSurviveAnAutoHiddenHudAndStayAboveTheDock();
    void aMalformedSegmentIsNotAnOffer();
};
