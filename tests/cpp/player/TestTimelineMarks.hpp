#pragma once

#include <QtTest/QTest>

class TestTimelineMarks : public QObject {
    Q_OBJECT

private slots:
    void theTintFollowsTheSegmentTimesAndTheKnownDuration();
    void anOverlapIsOneTintAndNotTwo();
    void aRangePastTheKnownEndIsClippedToTheTimeline();
    void aRangeAppearsWhenTheLengthBecomesKnown();
    void labelsNameEverySegmentUnderTheInstant();
    void aBoundaryBelongsToTheRangeThatStartsThere();
    void anUnknownTypeReadsAsTheProvidersOwnWord();
    void theHoverReportsTheTimeUnderThePointer();
    void leavingTheBarEndsTheHover();
    void hidingTheBarEndsTheHoverToo();
    void noLengthMeansNoTimeButStillAnEnd();
    void aNewLengthReprojectsTheHover();
    void aPressStillSeeksAndReportsTheHover();
};
