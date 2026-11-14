#pragma once

#include <QtTest/QTest>

class TestUpdateCheck : public QObject {
    Q_OBJECT
private slots:
    void comparesNumericVersionComponents();
    void rejectsMalformedAndUnsafeResponses();
    void ignoresPrereleaseAndDrafts();
};

int runTestUpdateCheck(int argc, char** argv);
