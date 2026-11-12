#include "TestHoverPreview.hpp"

#include "MpvApi.hpp"
#include "player/quick/PlayerController.hpp"
#include "player/quick/PlayerWindow.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QThread>
#include <QUrl>

namespace {

mplayer::MpvBundle envBundle() {
    const QString module = qEnvironmentVariable("MIKAI_MPV_MODULE");
    if (module.isEmpty()) return mplayer::MpvBundle{};
    const QFileInfo info(module);
    mplayer::MpvBundle bundle;
    bundle.root = info.absolutePath();
    bundle.modulePath = info.absoluteFilePath();
    bundle.descriptorId = QStringLiteral("env-standard-runtime");
    bundle.protocolVersion = 1;
    return bundle;
}

QString fixtureClip() {
    return QDir::toNativeSeparators(QStringLiteral(
        MIKAI_SOURCE_DIR "/tests/fixtures/media/motion_320x180_24fps_8bit_hevc.mp4"));
}

template <typename Predicate>
bool settleUntil(Predicate done, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return done();
}

QPoint windowPoint(QQuickItem* item, qreal fraction) {
    const QPointF point = item->mapToScene(QPointF(item->width() * fraction,
                                                   item->height() / 2.0));
    return point.toPoint();
}

void moveWithButtons(QQuickWindow* window, const QPoint& point, Qt::MouseButtons buttons) {
    const QPointF local(point);
    const QPointF global(window->mapToGlobal(point));
    QMouseEvent move(QEvent::MouseMove, local, local, global, Qt::NoButton, buttons,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(window, &move);
}

}  // namespace

void TestHoverPreview::initTestCase() {
    if (mplayer::MpvApi::instance().loaded()) return;
    const mplayer::MpvBundle bundle = envBundle();
    if (bundle.modulePath.isEmpty()) return;
    QString reason;
    if (!mplayer::MpvApi::instance().load(bundle, &reason)) selectionError_ = reason;
}

bool TestHoverPreview::requireRuntime() {
    if (mplayer::MpvApi::instance().loaded()) return true;
    if (!selectionError_.isEmpty()) {
        QTest::qFail(qPrintable(QStringLiteral("cannot select %1: %2")
                                    .arg(envBundle().modulePath, selectionError_)),
                     __FILE__, __LINE__);
        return false;
    }
    QTest::qSkip("set MIKAI_MPV_MODULE to a real libmpv runtime to run this",
                 __FILE__, __LINE__);
    return false;
}

void TestHoverPreview::theQuickTimelineHoverDragLeaveAndReloadUseDecodedFrames() {
    if (!requireRuntime()) return;
    const QString clip = fixtureClip();
    QVERIFY2(QFileInfo::exists(clip), qPrintable(clip));

    mplayer::quick::PlayerController controller(QStringLiteral("hover fixture"), clip);
    controller.startPlayer();
    auto* window = controller.window();
    QVERIFY(window != nullptr);
    window->resize(1000, 620);
    window->showWindowed();
    QVERIFY2(settleUntil([&] { return controller.duration() > 0.0; }, 20000),
             "the real player session did not load the preview fixture");

    QObject* root = window->rootObject();
    QVERIFY(root != nullptr);
    auto* timeline = qobject_cast<QQuickItem*>(root->property("timeline").value<QObject*>());
    QVERIFY2(timeline != nullptr, "the Quick scene must expose its actual timeline item");
    QVERIFY(timeline->width() > 0.0);
    QVERIFY(timeline->height() > 0.0);
    QVERIFY(window->isExposed());

    // Hovering the real timeline drives the controller and publishes a URL only
    // after the worker has returned a decoded frame to the scene's image provider.
    QTest::mouseMove(window, windowPoint(timeline, 0.2));
    QVERIFY2(settleUntil([&] {
                 return controller.hoverPreviewVisible() &&
                        !controller.hoverPreviewSource().isEmpty();
             }, 20000),
             "timeline hover did not publish its decoded preview frame");
    QVERIFY(controller.hoverPreviewSource().toString().startsWith(
        QStringLiteral("image://mikai-hover/")));
    const QString hoveredTime = controller.hoverPreviewTime();
    QVERIFY(!hoveredTime.isEmpty());

    auto* preview = root->findChild<QQuickItem*>(QStringLiteral("player.timeline.preview"));
    QVERIFY(preview != nullptr);
    QVERIFY(preview->isVisible());
    const QPointF timelineTop = timeline->mapToScene(QPointF(0, 0));
    const QPointF previewTop = preview->mapToScene(QPointF(0, 0));
    const QPoint hoverPoint = windowPoint(timeline, 0.2);
    QVERIFY2(qAbs(previewTop.y() + preview->height() - timelineTop.y() + 10.0) < 2.0,
             "the preview must sit just above the timeline");
    QVERIFY2(qAbs(previewTop.x() + preview->width() / 2.0 - hoverPoint.x()) < 2.0,
             "the preview must follow the hovered timeline position");

    QTest::mouseMove(window, windowPoint(timeline, 0.95));
    QVERIFY2(settleUntil([&] {
                 const QPointF position = preview->mapToScene(QPointF(0, 0));
                 return preview->isVisible() &&
                        qAbs(position.x() + preview->width() / 2.0 -
                             windowPoint(timeline, 0.95).x()) < 2.0;
             }, 2000),
             "the preview must follow pointer movement along the timeline");
    const QPointF movedTop = preview->mapToScene(QPointF(0, 0));
    QVERIFY(movedTop.x() >= 0.0);
    QVERIFY(movedTop.x() + preview->width() <= window->width());

    // A drag requests previews at the moving playhead rather than retaining the
    // frame from the initial hover. The controller's time is the decoder's
    // reported frame time once the latest request settles.
    const QPoint start = windowPoint(timeline, 0.25);
    const QPoint finish = windowPoint(timeline, 0.75);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
    moveWithButtons(window, finish, Qt::LeftButton);
    QVERIFY(controller.hoverPreviewVisible());
    QVERIFY(!controller.hoverPreviewTime().isEmpty());
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, finish);
    QVERIFY2(settleUntil([&] {
                 return !controller.hoverPreviewSource().isEmpty() &&
                        controller.hoverPreviewTime() != hoveredTime;
             }, 20000),
             "the dragged timeline instant did not publish its own decoded frame");

    // Leaving the rail dismisses the controller-owned preview, including its
    // provider URL, rather than leaving a stale frame floating in the scene.
    QTest::mouseMove(window, QPoint(8, 8));
    QVERIFY2(settleUntil([&] {
                 return !controller.hoverPreviewVisible() &&
                        controller.hoverPreviewSource().isEmpty();
             }, 2000),
             "leaving the timeline did not dismiss the hover card");

    // Reloading the same URL creates a new source epoch; the previous thumbnail
    // must be invalidated even though the media path did not change.
    QTest::mouseMove(window, windowPoint(timeline, 0.2));
    QVERIFY2(settleUntil([&] {
                 return controller.hoverPreviewVisible() &&
                        !controller.hoverPreviewSource().isEmpty();
             }, 20000),
             "the final hover before reload did not decode");
    const uint64_t oldEpoch = controller.session()->sourceEpoch();
    const QUrl oldPreview = controller.hoverPreviewSource();
    QVERIFY(!oldPreview.isEmpty());
    controller.startPlayer();
    QVERIFY2(settleUntil([&] { return controller.session()->sourceEpoch() > oldEpoch; }, 2000),
             "reloading the same URL did not advance the source epoch");
    QVERIFY(!controller.hoverPreviewVisible());
    QVERIFY(controller.hoverPreviewSource().isEmpty());

    QVERIFY2(settleUntil([&] { return controller.duration() > 0.0; }, 20000),
             "the reloaded source did not become ready for preview");
    QTest::mouseMove(window, windowPoint(timeline, 0.2));
    QVERIFY2(settleUntil([&] {
                 return controller.hoverPreviewVisible() &&
                        !controller.hoverPreviewSource().isEmpty();
             }, 20000),
             "the reloaded source did not produce a fresh hover preview");
    QVERIFY(controller.hoverPreviewSource() != oldPreview);
    QVERIFY(!controller.hoverPreviewTime().isEmpty());
}
