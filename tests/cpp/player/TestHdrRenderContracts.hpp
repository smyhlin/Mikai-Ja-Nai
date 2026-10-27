#pragma once

#include <QtTest/QTest>

class TestHdrRenderContracts : public QObject {
    Q_OBJECT

private slots:
    void unknownTransferRequiresSdrGate();
    void pqAndHlgRequireSdrGate();
    void knownSdrTransferDoesNotRequireSdrGate();
    void primariesDoNotMisclassifySdrAsHdr();
    void pacingConvertsRawRenderNanosecondsToMpvClock();
    void pacingRendersImmediatelyForElapsedOrDistantTargets();
};
