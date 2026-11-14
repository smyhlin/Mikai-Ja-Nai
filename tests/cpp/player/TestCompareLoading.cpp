#include "TestCompareLoading.hpp"

#include "../QuickPlayerTestSupport.hpp"

#include <player/core/VideoSession.hpp>
#include <player/quick/PlayerController.hpp>
#include <player/quick/PlayerWindow.hpp>

#include <QFileInfo>
#include <QSignalSpy>
#include <QStringList>

namespace {
bool requireMpvRuntime(QString* error) {
    return mikai_test::quick::loadExplicitMpvRuntime(error);
}
}  // namespace

#define REQUIRE_MPV_RUNTIME()                                                    \
    do {                                                                         \
        QString runtimeError;                                                    \
        if (!requireMpvRuntime(&runtimeError)) {                                 \
            if (qEnvironmentVariableIsEmpty("MIKAI_MPV_MODULE")) {              \
                QSKIP(qPrintable(runtimeError));                                \
            }                                                                    \
            QFAIL(qPrintable(runtimeError));                                     \
        }                                                                        \
    } while (false)

void TestCompareLoading::openingIndicatorEndsOnPresentedFrame() {
    REQUIRE_MPV_RUNTIME();
    const QString media = mikai_test::quick::benchmarkVideoPath();
    QVERIFY2(QFileInfo(media).isFile(), qPrintable(media));

    mplayer::quick::PlayerController controller(QStringLiteral("Loading test"), media);
    auto* window = controller.window();
    QVERIFY(window != nullptr);

    QSignalSpy presented(window, &mplayer::quick::PlayerWindow::framePresented);
    QSignalSpy loadingChanged(&controller, &mplayer::quick::PlayerController::openingChanged);
    QStringList eventOrder;
    connect(&controller, &mplayer::quick::PlayerController::openingChanged, &controller,
            [&] {
                if (controller.openingLoading()) eventOrder.append(QStringLiteral("opening"));
            });
    connect(window, &mplayer::quick::PlayerWindow::framePresented, &controller,
            [&] { eventOrder.append(QStringLiteral("frame")); });
    window->showWindowed();
    controller.startPlayer();

    QVERIFY(controller.openingLoading());
    QCOMPARE(controller.openingHeadline(), QStringLiteral("Opening stream…"));
    QTRY_VERIFY_WITH_TIMEOUT(!presented.isEmpty(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.openingLoading(), 1000);
    QVERIFY(loadingChanged.size() >= 2);
    QCOMPARE(controller.session()->sourceUrl(), media);
    QVERIFY(controller.session()->hasVideo());
    QVERIFY(eventOrder.indexOf(QStringLiteral("opening")) >= 0);
    QVERIFY(eventOrder.indexOf(QStringLiteral("frame")) >
            eventOrder.indexOf(QStringLiteral("opening")));
}
void TestCompareLoading::openingIndicatorTracksSourceReplacement() {
    REQUIRE_MPV_RUNTIME();
    const QString first = mikai_test::quick::benchmarkVideoPath();
    const QString replacement = QDir::cleanPath(
        QStringLiteral(MIKAI_SOURCE_DIR "/resources/benchmarks/1280x720.mp4"));
    QVERIFY(QFileInfo(first).isFile());
    QVERIFY(QFileInfo(replacement).isFile());

    mplayer::quick::PlayerController controller(QStringLiteral("Loading test"), first);
    auto* window = controller.window();
    QVERIFY(window != nullptr);
    QSignalSpy presented(window, &mplayer::quick::PlayerWindow::framePresented);
    QSignalSpy loadingChanged(&controller, &mplayer::quick::PlayerController::openingChanged);
    window->showWindowed();
    controller.startPlayer();
    QTRY_VERIFY_WITH_TIMEOUT(!presented.isEmpty(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.openingLoading(), 1000);

    presented.clear();
    loadingChanged.clear();
    const uint64_t previousEpoch = controller.session()->sourceEpoch();
    QVERIFY(controller.session()->open(replacement));
    QTRY_VERIFY_WITH_TIMEOUT(controller.session()->sourceEpoch() != previousEpoch, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!presented.isEmpty(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.openingLoading(), 1000);
    QVERIFY(loadingChanged.size() >= 2);
}

void TestCompareLoading::durationPositionAndPlayingFanOut() {
    REQUIRE_MPV_RUNTIME();
    const QString media = mikai_test::quick::benchmarkVideoPath();
    QVERIFY(QFileInfo(media).isFile());

    mplayer::quick::PlayerController controller(QStringLiteral("Loading test"), media);
    auto* window = controller.window();
    QVERIFY(window != nullptr);
    QSignalSpy durationChanged(&controller, &mplayer::quick::PlayerController::durationChanged);
    QSignalSpy positionChanged(&controller, &mplayer::quick::PlayerController::positionChanged);
    QSignalSpy playingChanged(&controller, &mplayer::quick::PlayerController::playingChanged);
    window->showWindowed();
    controller.startPlayer();

    QTRY_VERIFY_WITH_TIMEOUT(controller.duration() > 0.0, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(controller.position() > 0.0, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(controller.playing(), 15000);
    QVERIFY(durationChanged.size() >= 1);
    QVERIFY(positionChanged.size() >= 1);
    QVERIFY(playingChanged.size() >= 1);
    QCOMPARE(controller.duration(), controller.session()->duration());
    QVERIFY(qAbs(controller.position() - controller.session()->position()) < 0.5);
    QCOMPARE(controller.playing(), controller.session()->isPlaying());
}

void TestCompareLoading::failedOpenIsNamedOnceAndRecovered() {
    REQUIRE_MPV_RUNTIME();
    const QString missing =
        QFileInfo(QStringLiteral("resources/benchmarks/missing-loading-test.mp4"))
            .absoluteFilePath();
    const QString valid = mikai_test::quick::benchmarkVideoPath();
    mplayer::quick::PlayerController controller(QStringLiteral("Loading test"), missing);
    auto* window = controller.window();
    QVERIFY(window != nullptr);
    QSignalSpy alerts(&controller, &mplayer::quick::PlayerController::alertRequested);
    QSignalSpy presented(window, &mplayer::quick::PlayerWindow::framePresented);
    QSignalSpy loaded(controller.session(), &mplayer::VideoSession::mediaLoaded);
    window->showWindowed();
    controller.startPlayer();

    QTRY_VERIFY_WITH_TIMEOUT(controller.errorActive(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.openingLoading(), 1000);
    QCOMPARE(alerts.size(), 1);
    QVERIFY(controller.retryVisible());
    QVERIFY(!controller.statusText().isEmpty());

    presented.clear();
    QVERIFY(controller.session()->open(valid));
    QTRY_VERIFY_WITH_TIMEOUT(!presented.isEmpty(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.errorActive(), 1000);
    QVERIFY(!controller.retryVisible());
    QCOMPARE(alerts.size(), 1);
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(controller.session()->sourceUrl(), valid);
    QVERIFY(controller.session()->hasVideo());
}
