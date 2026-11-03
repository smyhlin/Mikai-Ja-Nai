#pragma once

#include <QtCore/QString>
#include <QtTest/QTest>

/// The timeline's hover card: where it may stand, what a decoder reply has to
/// prove before it is painted, how a capture is unwrapped from mpv's declared
/// geometry, and - with a real runtime - that the decoder answers with the frame
/// at the hovered instant and never with a superseded one.
///
/// The decoder cases select a runtime only from MIKAI_MPV_MODULE and skip
/// without one; a module that is named but cannot be loaded fails.
class TestHoverPreview : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void theCardStandsAboveTheBarAndInsideTheWindow();
    void aPointerNearAnEdgeKeepsTheCardOnScreen();
    void aCardWithNoRoomAboveDropsBelowTheBar();
    void aTallFrameIsFittedByHeightNotCropped();
    void anImplausibleAspectFallsBackToWidescreen();
    void aReplyMustNameTheNewestRequestAndTheSameInstant();
    void aCaptureWithRowPaddingKeepsItsPixels();
    void aMalformedCaptureIsRefusedWithAReason();
    void thePreviewIsPaintedOnTheVideoChrome();
    void thePreviewStaysAboveThePlayerControls();
    void aScrubKeepsTheCardToItsTimestampOnly();
    void anUnavailableSourceLeavesNoFrameOnTheCard();
    void aHoverCardShowsTheDecodedFrameOfItsInstant();
    void aHoveredInstantIsDecodedAndOnlyTheNewestServes();

    // ── the store, the identity and the states a viewer waits in ─────────
    void theStoreKeepsSixtyFourInstantsAndEvictsTheOldest();
    void aStoredInstantIsBucketedByHalfSecondsAndBySourceEpoch();
    void aRepeatedInstantIsServedWithoutAskingTheDecoder();
    void aFinishedDecodeIsNeverThrownAwayWhenThePointerMovesOn();
    void theCardKeepsAnOlderPictureAsExplicitlyStale();
    void aDecodingCardNamesTheWaitAndARefusalStaysNamed();
    void aSameUrlReloadInvalidatesTheStoredFrames();
    void aPointerBetweenTheBarAndTheCardKeepsIt();
    void aParkedTimelineHoverKeepsTheChromeAwake();
    void aVanishedTimelineEndsTheCardWithoutGrace();

private:
    // True when a runtime is selected. Without a module it skips; a selection
    // that failed in initTestCase fails here with the loader's own reason.
    bool requireRuntime();

    QString selectionError_;
};
