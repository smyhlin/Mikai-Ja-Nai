#include "TestTimelineMarks.hpp"

#include "../QuickPlayerTestSupport.hpp"
#include <player/quick/PlayerController.hpp>
#include <player/quick/PlayerWindow.hpp>

#include <QFileInfo>
#include <QQuickItem>

namespace {

QQuickItem* timelineItem(QQuickItem* item) {
    if (item == nullptr) return nullptr;
    const QMetaObject* meta = item->metaObject();
    if (meta->indexOfProperty("mediaDuration") >= 0 &&
        meta->indexOfProperty("positionFraction") >= 0) {
        return item;
    }
    for (QQuickItem* child : item->childItems()) {
        if (QQuickItem* match = timelineItem(child)) return match;
    }
    return nullptr;
}

QQuickItem* timelineItem(mplayer::quick::PlayerWindow* window) {
    return window == nullptr ? nullptr : timelineItem(window->contentItem());
}

QQuickItem* markAt(QQuickItem* timeline, double start) {
    if (timeline == nullptr) return nullptr;
    const QMetaObject* meta = timeline->metaObject();
    if (meta->indexOfProperty("startSeconds") >= 0 &&
        meta->indexOfProperty("endSeconds") >= 0 &&
        qAbs(timeline->property("startSeconds").toDouble() - start) < 0.001) {
        return timeline;
    }
    for (QQuickItem* child : timeline->childItems()) {
        if (QQuickItem* match = markAt(child, start)) return match;
    }
    return nullptr;
}

}  // namespace

void TestTimelineMarks::initTestCase() {
    QString error;
    if (mikai_test::quick::loadExplicitMpvRuntime(&error)) return;
    runtimeUnavailable_ = qEnvironmentVariableIsEmpty("MIKAI_MPV_MODULE");
    runtimeError_ = error;
}

bool TestTimelineMarks::requireRuntime() {
    if (runtimeUnavailable_) {
        QTest::qSkip("set MIKAI_MPV_MODULE to the real playback runtime to run this", __FILE__,
                     __LINE__);
        return false;
    }
    if (!runtimeError_.isEmpty()) {
        QTest::qFail(qPrintable(QStringLiteral("cannot load the explicit playback runtime: %1")
                                    .arg(runtimeError_)),
                     __FILE__, __LINE__);
        return false;
    }
    return true;
}

void TestTimelineMarks::quickTimelineProjectsControllerMarksAndKeepsTheirBoundaries() {
    if (!requireRuntime()) return;
    const QString media = mikai_test::quick::benchmarkVideoPath();
    QVERIFY2(QFileInfo(media).isFile(), qPrintable(media));

    mplayer::quick::PlayerController controller(QStringLiteral("Timeline marks test"), media);
    auto* window = controller.window();
    QVERIFY(window != nullptr);
    window->resize(1000, 620);
    window->showWindowed();
    controller.startPlayer();

    QTRY_VERIFY_WITH_TIMEOUT(controller.duration() > 0.0, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(timelineItem(window) != nullptr, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(timelineItem(window)->width() > 0.0, 5000);
    QQuickItem* timeline = timelineItem(window);

    const double duration = controller.duration();
    const double firstStart = duration * 0.2;
    const double boundary = duration * 0.4;
    const double secondEnd = duration * 0.6;
    const double clippedStart = duration * 0.9;
    const double outsideStart = duration + 1.0;
    const double leftClippedEnd = duration * 0.1;
    controller.setSkipSegments({
        {{QStringLiteral("type"), QStringLiteral("opening")},
         {QStringLiteral("start"), firstStart}, {QStringLiteral("end"), boundary}},
        {{QStringLiteral("type"), QStringLiteral("ending")},
         {QStringLiteral("start"), boundary}, {QStringLiteral("end"), secondEnd}},
        {{QStringLiteral("type"), QStringLiteral("ending")},
         {QStringLiteral("start"), clippedStart}, {QStringLiteral("end"), duration + 20.0}},
        {{QStringLiteral("type"), QStringLiteral("outside")},
         {QStringLiteral("start"), outsideStart}, {QStringLiteral("end"), duration + 20.0}},
        {{QStringLiteral("type"), QStringLiteral("opening")},
         {QStringLiteral("start"), -duration * 0.1}, {QStringLiteral("end"), leftClippedEnd}},
    });

    QTRY_VERIFY_WITH_TIMEOUT(markAt(timeline, firstStart) != nullptr, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(markAt(timeline, boundary) != nullptr, 5000);
    QQuickItem* first = markAt(timeline, firstStart);
    QQuickItem* second = markAt(timeline, boundary);
    QQuickItem* clipped = markAt(timeline, clippedStart);
    QQuickItem* outside = markAt(timeline, outsideStart);
    QQuickItem* leftClipped = markAt(timeline, -duration * 0.1);
    QVERIFY(first != nullptr);
    QVERIFY(second != nullptr);
    QVERIFY(clipped != nullptr);
    QVERIFY(outside != nullptr);
    QVERIFY(leftClipped != nullptr);

    const qreal width = timeline->width();
    const qreal expectedFirstX = qreal(firstStart / duration) * width;
    const qreal expectedBoundaryX = qreal(boundary / duration) * width;
    const qreal expectedSecondEndX = qreal(secondEnd / duration) * width;
    QVERIFY(first->isVisible());
    QVERIFY(second->isVisible());
    QVERIFY(qAbs(first->x() - expectedFirstX) <= 1.0);
    QVERIFY(qAbs(first->width() - (expectedBoundaryX - expectedFirstX)) <= 1.0);
    QVERIFY(qAbs(second->x() - expectedBoundaryX) <= 1.0);
    QVERIFY(qAbs(second->width() - (expectedSecondEndX - expectedBoundaryX)) <= 1.0);
    QVERIFY2(qAbs((first->x() + first->width()) - second->x()) <= 1.0,
             "adjacent provider ranges meet at their shared boundary");

    QVERIFY(clipped->isVisible());
    QVERIFY(qAbs(clipped->x() - qreal(clippedStart / duration) * width) <= 1.0);
    QVERIFY(qAbs(clipped->x() + clipped->width() - width) <= 1.0);
    QVERIFY(!outside->isVisible());
    QVERIFY(leftClipped->isVisible());
    QVERIFY(qAbs(leftClipped->x()) <= 1.0);
    QVERIFY(qAbs(leftClipped->width() - qreal(leftClippedEnd / duration) * width) <= 1.0);

    const qreal originalWidth = timeline->width();
    window->resize(1200, 620);
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(timeline->width() - originalWidth) > 1.0, 5000);
    const qreal resizedWidth = timeline->width();
    QVERIFY(qAbs(first->x() - qreal(firstStart / duration) * resizedWidth) <= 1.0);
    QVERIFY(qAbs(first->width() - qreal((boundary - firstStart) / duration) * resizedWidth) <= 1.0);
    QVERIFY(qAbs(second->x() - qreal(boundary / duration) * resizedWidth) <= 1.0);
    QVERIFY(qAbs(second->x() + second->width() -
                 qreal(secondEnd / duration) * resizedWidth) <= 1.0);
    QVERIFY(qAbs(clipped->x() + clipped->width() - resizedWidth) <= 1.0);
    QVERIFY(qAbs(leftClipped->x()) <= 1.0);
    QVERIFY(qAbs(leftClipped->width() -
                 qreal(leftClippedEnd / duration) * resizedWidth) <= 1.0);
}
