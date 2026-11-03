#pragma once

#include <QtTest/QTest>

class TestVideoScaling : public QObject {
    Q_OBJECT

private slots:
    void eachModeOwnsAllSevenOptions();
    void fillAndStretchAdjustKeepaspectAndPanscan();
    void aspectOverridesAndOriginalSize();
    void zoomUsesLog2OfTheFactor();
    void zoomIsClampedToTheCardRange();
    void stretchAndOriginalKeepTheStoredZoomOutOfMpv();
    void zoomFormattingIgnoresTheLocale();
    void autoDownscaleLeavesTheCoreDefaultAlone();
    void hiddenPlaybackChangesThePicturePerMode_data();
    void hiddenPlaybackChangesThePicturePerMode();
};
