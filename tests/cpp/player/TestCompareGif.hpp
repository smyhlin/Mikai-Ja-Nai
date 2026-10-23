#pragma once

#include <QtTest/QTest>

class TestCompareGif : public QObject {
    Q_OBJECT

private slots:
    void exportAlternatesBothImagesWithTimingAndInfiniteLoop();
    void differentlySizedImagesShareOneCanvasWithoutCropping();
    void cancellationPreservesExistingDestination();
    void invalidDestinationReportsFailure();
    void backgroundExportLetsGuiTimersRun();
};
