#pragma once

#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

class TestUpdateFeed : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void refreshPaginatesAndDeduplicatesExactBoundaryEvents();
    void emptyRefreshRetainsThePublishedBuffer();
    void failedRefreshDoesNotMutateThePreviousState();
    void malformedTimestampFailsWithoutPublishingPartialState();

private:
    QTemporaryDir envDirectory_;
};

int runTestUpdateFeed(int argc, char** argv);
