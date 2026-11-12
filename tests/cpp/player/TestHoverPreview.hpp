#pragma once

#include <QtTest/QTest>

/// End-to-end hover preview coverage through the Quick player scene and its
/// PlayerController-owned decoder. Runtime-backed assertions skip unless an
/// explicit MIKAI_MPV_MODULE is selected; a selected but unloadable module fails.
class TestHoverPreview : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void theQuickTimelineHoverDragLeaveAndReloadUseDecodedFrames();

private:
    bool requireRuntime();
    QString selectionError_;
};
