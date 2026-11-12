// Consumer-visible transport and source-transition regressions for the native
// Qt Quick player. Runtime cases use the explicitly selected real libmpv module.

#pragma once

#include <QtCore/QString>
#include <QtTest/QTest>

class TestPlaybackControls : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void transportActionsUpdateTypedControllerAndSessionState();
    void nestedEnhancementMenuActivatesTheSelectedPreset();
    void keyboardStepsAreResolvedByTheSessionCore();
    void enginePauseHoldRetainsExplicitPauseIntent();
    void compactPipKeepsTheSameWindowAndSession();
    void quickTimelinePointerSeeksAndTimeLabelsFormat();
    void variantTransitionPreservesPositionPauseRateAndFirstReport();
    void episodeRestartRetainsExplicitPauseIntent();
    void refreshedStreamStartsAtBeginningWithoutResuming();
    void skipForwardUsesEightyFiveSecondsWhenReachable();
    void skipAtEndDoesNotRestartTheSource();
    void eofEmitsFinishedOnceWithoutSteppingTheHost();
    void asynchronousUnfilteredCaptureUsesTheSessionCallback();

private:
    bool requireRuntime();
    QString selectionError_;
};
