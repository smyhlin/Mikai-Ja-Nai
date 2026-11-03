#pragma once

#include <QtTest/QTest>

class TestPacingContract : public QObject {
    Q_OBJECT

private slots:
    // A paint Qt asked for (a hover, a mask change) is not a presentation: the
    // frame a pacing slot is holding stays owed and the slot stays armed.
    void aRepaintOverAPendingFrameKeepsTheSlotAndTheFrame();
    void aRepaintOverPausedVideoKeepsTheArmedSlot();
    // Only these three present, and each of them ends the slot it found.
    void thePacingTickConsumesTheFrameItPresented();
    void aForcedRedrawConsumesWithoutASlot();
    void anArrivalWithNoSlotConsumesImmediately();
    // A hold taken across a source change is released by the frame it waits for.
    void aHoldAwaitingOutputConsumesTheFrameThatReplacesIt();
};
