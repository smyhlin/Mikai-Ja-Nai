#pragma once

#include <QtTest/QTest>

#include <QString>

class TestSkipSegments : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void theClockControlsOverlappingSkipOffers();
    void aLateSkipListUsesTheCurrentPlaybackPosition();
    void activatingAnOfferSeeksTheRealSessionToItsEnd();

private:
    bool requireRuntime();
    QString runtimeError_;
    bool runtimeUnavailable_ = false;
};
