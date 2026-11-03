#include "TestPacingContract.hpp"

#include "player/core/VideoSurface.hpp"

using mplayer::VideoSurface;

namespace {

VideoSurface::PaintScheduleDecision decide(bool pacedRelease, bool forced, bool armedSlot,
                                           bool frameOwed, bool holdOwesOutput = false) {
    return VideoSurface::decidePaintSchedule(pacedRelease, forced, armedSlot, frameOwed,
                                             holdOwesOutput);
}

}  // namespace

void TestPacingContract::aRepaintOverAPendingFrameKeepsTheSlotAndTheFrame() {
    // The static defect: this pass used to stop the timer, clear its target and
    // consume the pending frame, so the frame rendered ahead of its slot and the
    // frames after it waited.
    const auto decision = decide(false, false, true, true);
    QVERIFY(!decision.consumesFrame);
    QVERIFY(decision.keepsArmedSlot);
}

void TestPacingContract::aRepaintOverPausedVideoKeepsTheArmedSlot() {
    const auto decision = decide(false, false, true, false);
    QVERIFY(!decision.consumesFrame);
    QVERIFY(decision.keepsArmedSlot);
}

void TestPacingContract::thePacingTickConsumesTheFrameItPresented() {
    // onPacingTick() clears the armed flags before it asks for the paint, so the
    // released paint arrives with no slot of its own and must present the frame.
    const auto decision = decide(true, false, false, true);
    QVERIFY(decision.consumesFrame);
    QVERIFY(!decision.keepsArmedSlot);
}

void TestPacingContract::aForcedRedrawConsumesWithoutASlot() {
    const auto decision = decide(false, true, false, false);
    QVERIFY(decision.consumesFrame);
    QVERIFY(!decision.keepsArmedSlot);
}

void TestPacingContract::anArrivalWithNoSlotConsumesImmediately() {
    const auto decision = decide(false, false, false, true);
    QVERIFY(decision.consumesFrame);
    QVERIFY(!decision.keepsArmedSlot);
}

void TestPacingContract::aHoldAwaitingOutputConsumesTheFrameThatReplacesIt() {
    const auto decision = decide(false, false, true, true, true);
    QVERIFY(decision.consumesFrame);
    QVERIFY(!decision.keepsArmedSlot);
}
