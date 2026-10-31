#pragma once

#include <QtTest/QTest>

class TestCompareLoading : public QObject {
    Q_OBJECT

private slots:
    void centeredIndicatorRendersAndIsAccessible();
    void loadingSurvivesHudPlaybackAndMediaChangesThenRestoresLatestGlyph();
};
