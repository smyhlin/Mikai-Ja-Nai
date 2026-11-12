#pragma once

#include <QtTest/QTest>
#include <QString>

class TestTimelineMarks : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void quickTimelineProjectsControllerMarksAndKeepsTheirBoundaries();

private:
    bool requireRuntime();
    QString runtimeError_;
    bool runtimeUnavailable_ = false;
};
