#pragma once

#include <QtTest/QTest>

class TestCompareGif : public QObject {
    Q_OBJECT

private slots:
    void exportAlternatesBothImagesWithTimingAndInfiniteLoop();
    void adaptivePalettePreservesSourceColorsAndFrameDifferences();
    void differentlySizedImagesShareOneCanvasWithoutCropping();
    void cancellationPreservesExistingDestination();
    void invalidDestinationReportsFailure();
    void backgroundExportLetsGuiTimersRun();
};
