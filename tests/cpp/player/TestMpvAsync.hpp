// The loader and the asynchronous core transport.
//
// Two kinds of test live here. The loader boundary runs against whatever state
// the process is in: with nothing pinned a bad descriptor is refused BY NAME,
// because bootstrap reports that string to the user and a generic failure would
// be indistinguishable from a missing runtime; with a module pinned the same
// calls are the hot-swap refusal, and the pinned module has to survive them
// untouched. That is what makes the boundary checks safe to run in either order.
//
// The transport tests need a real core, because the whole point of the
// correlation key is that a reply mpv actually produced is delivered to the
// request that asked for it and dropped otherwise. They select a runtime only
// from MIKAI_MPV_MODULE (an explicit path), and skip without it - a test that
// faked a core would pass whether or not the transport worked.

#pragma once

#include <QtCore/QString>
#include <QtTest/QTest>

class TestMpvAsync : public QObject {
    Q_OBJECT

private slots:
    // Selects this suite's runtime once, before any test runs. The shared
    // runner pins nothing, so the suite that needs a core has to ask for it.
    void initTestCase();

    // ── loader boundary ────────────────────────────────────────────────────
    void anEmptyDescriptorIsRefusedByName();
    void aRelativeModulePathIsRefusedByName();
    void aMissingModuleIsRefusedByName();
    void aModuleOutsideTheOwnedRootIsRefusedByName();
    void aDifferentModuleIsRefusedWhileOneIsPinned();
    void theHeaderApiVersionIsAlwaysReported();
    void aHandleWithoutASelectedRuntimeIsRefused();

    // ── public FPS provenance policy; these cases need no mpv runtime ───────
    void fpsSelectorPrefersValidMpvOutput();
    void fpsSelectorFallsBackToContainerForInvalidOutput_data();
    void fpsSelectorFallsBackToContainerForInvalidOutput();
    void fpsSelectorIgnoresStaleRatesWhenInactive_data();
    void fpsSelectorIgnoresStaleRatesWhenInactive();
    void fpsSelectorReturnsUnknownWithoutFinitePositiveRate_data();
    void fpsSelectorReturnsUnknownWithoutFinitePositiveRate();

    // ── native-status polling policy; these cases need no mpv runtime ──────
    //
    // The boundary that made an attached probe unreadable: a Probe document is
    // published before the producer has seen a decoded frame, and halting on it
    // meant the revision that names the device was never read.
    void probeSnapshotsKeepTheStatusReaderPolling_data();
    void probeSnapshotsKeepTheStatusReaderPolling();

    // ── transport against a real core ──────────────────────────────────────
    void anAsyncPropertyReplyCarriesItsOwnValue();
    void eventBacklogYieldsToTheGui();
    void nestedEventDrainsPreserveTheCurrentReply();
    void selfUnobservingCallbacksKeepTheirCapturesAlive();
    void selfReplacingEventSinksKeepTheirCapturesAlive();
    void anUnknownPropertyRepliesWithAnError();
    void anAsyncNodeReplySurvivesTheNextDrain();
    void aCancelledRequestNeverCallsBack();
    void cancellingAllDropsEveryCallback();
    void aForeignCoreCancelLeavesThisCoreAlone();
    void aPendingReplyIsDroppedWhenTheSourceChanges();
    void aPendingReplyIsDroppedWhenPlaybackStops();
    void observerIdsAreUniqueAndUnobserveStopsDelivery();
    void aMovedHandleStillDrivesTheSameCore();
    void twoCoresKeepTheirRepliesApart();
    void aRequestOnAReplacedCoreIsNeverDelivered();

    void onlyNewVideoFramesContributeToThroughput_data();
    void onlyNewVideoFramesContributeToThroughput();
    void preparationAndWarmupAreExcludedWhenMeasurementRestarts();

private:
    // True when a runtime is selected, so the calling test should continue.
    // Without a module to select it skips, and a selection that failed in
    // initTestCase fails here with the loader's own reason.
    bool requireRuntime();

    // Set when initTestCase could not select the named module, so the transport
    // tests report the loader's reason instead of a bare skip.
    QString selectionError_;
};
