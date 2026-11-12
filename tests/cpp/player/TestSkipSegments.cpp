#include "TestSkipSegments.hpp"

#include "../QuickPlayerTestSupport.hpp"
#include "player/quick/PlayerController.hpp"
#include "player/quick/PlayerMenuModel.hpp"
#include "player/quick/PlayerWindow.hpp"
#include "player/core/VideoSession.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QStringList>
#include <QThread>
#include <QtTest/QTest>
#include <QVariantMap>
#include <QVector>

#include <cmath>

namespace {
using mplayer::quick::PlayerController;
using mplayer::quick::PlayerMenuModel;
using mplayer::VideoSession;

QVariantMap segment(const char* type, double start, double end) {
    return {{QStringLiteral("type"), QString::fromLatin1(type)},
            {QStringLiteral("start"), start}, {QStringLiteral("end"), end}};
}

class PlayerFixture final {
public:
    PlayerFixture()
        : controller(QStringLiteral("Skip-segment test"),
                     mikai_test::quick::benchmarkVideoPath()) {}

    bool open(QString* error) {
        mplayer::quick::PlayerWindow* window = controller.window();
        if (window == nullptr) {
            if (error != nullptr) *error = QStringLiteral("Quick player window is unavailable");
            return false;
        }
        controller.showWindow();
        if (!QTest::qWaitForWindowExposed(window, 15000)) {
            if (error != nullptr) *error = QStringLiteral("Quick player window was not exposed");
            return false;
        }
        if (!window->session()->waitForContext(20000)) {
            if (error != nullptr) *error = QStringLiteral("Quick player render context is unavailable");
            return false;
        }
        controller.startPlayer();
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 30000 && controller.session()->duration() <= 2.0) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(10);
        }
        if (controller.session()->duration() <= 2.0) {
            if (error != nullptr) *error = QStringLiteral("Quick player did not load the benchmark video");
            return false;
        }
        controller.session()->setPaused(true);
        return true;
    }

    PlayerController controller;
};

bool seekAndSettle(PlayerController& controller, double seconds) {
    controller.seekTo(seconds);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 8000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::abs(controller.session()->position() - seconds) < 0.25) return true;
        QThread::msleep(10);
    }
    return false;
}

bool seekExactlyAndSettle(PlayerController& controller, double seconds) {
    controller.session()->seekExact(seconds);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 8000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::abs(controller.session()->position() - seconds) < 0.10) return true;
        QThread::msleep(10);
    }
    return false;
}

QVector<PlayerMenuModel::Row> offers(const PlayerController& controller) {
    const PlayerMenuModel* model = controller.skipOffers();
    return model != nullptr ? model->rows() : QVector<PlayerMenuModel::Row>{};
}

QStringList offerValues(const PlayerController& controller) {
    QStringList values;
    for (const PlayerMenuModel::Row& row : offers(controller)) values.append(row.value);
    return values;
}

}  // namespace

void TestSkipSegments::initTestCase() {
    QString error;
    if (mikai_test::quick::loadExplicitMpvRuntime(&error)) return;
    runtimeUnavailable_ = qEnvironmentVariableIsEmpty("MIKAI_MPV_MODULE");
    runtimeError_ = error;
}

bool TestSkipSegments::requireRuntime() {
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

void TestSkipSegments::theClockControlsOverlappingSkipOffers() {
    if (!requireRuntime()) return;
    PlayerFixture fixture;
    QString error;
    QVERIFY2(fixture.open(&error), qPrintable(error));
    VideoSession* session = fixture.controller.session();
    QTRY_VERIFY_WITH_TIMEOUT(session->duration() > 2.0, 15000);
    const double duration = session->duration();
    const double openingStart = duration * 0.15;
    const double openingEnd = duration * 0.65;
    const double mixedStart = duration * 0.30;
    const double mixedEnd = duration * 0.80;
    const double recapStart = duration * 0.40;
    const double recapEnd = duration * 0.60;
    const double endingStart = duration * 0.85;
    const double endingEnd = duration * 0.95;
    fixture.controller.setSkipSegments({segment("opening", openingStart, openingEnd),
                                        segment("mixed_opening", mixedStart, mixedEnd),
                                        segment("recap", recapStart, recapEnd),
                                        segment("ending", endingStart, endingEnd)});

    QVERIFY(seekExactlyAndSettle(fixture.controller, openingStart));
    QCOMPARE(offerValues(fixture.controller), QStringList{QStringLiteral("0")});
    QVERIFY(seekAndSettle(fixture.controller, duration * 0.20));
    QCOMPARE(offerValues(fixture.controller), QStringList{QStringLiteral("0")});
    QVERIFY(seekAndSettle(fixture.controller, duration * 0.50));
    QCOMPARE(offerValues(fixture.controller), (QStringList{QStringLiteral("0"),
                                                           QStringLiteral("1"),
                                                           QStringLiteral("2")}));
    const QVector<PlayerMenuModel::Row> overlapping = offers(fixture.controller);
    QCOMPARE(overlapping.at(0).id, QStringLiteral("skip.0"));
    QCOMPARE(overlapping.at(0).action, PlayerMenuModel::Action::SkipSegment);
    QCOMPARE(overlapping.at(0).value, QStringLiteral("0"));
    QCOMPARE(overlapping.at(1).id, QStringLiteral("skip.1"));
    QCOMPARE(overlapping.at(1).action, PlayerMenuModel::Action::SkipSegment);
    QCOMPARE(overlapping.at(2).id, QStringLiteral("skip.2"));
    QCOMPARE(overlapping.at(2).action, PlayerMenuModel::Action::SkipSegment);
    QVERIFY(seekExactlyAndSettle(fixture.controller, recapEnd));
    QCOMPARE(offerValues(fixture.controller),
             (QStringList{QStringLiteral("0"), QStringLiteral("1")}));
    QVERIFY(seekAndSettle(fixture.controller, duration * 0.62));
    QCOMPARE(offerValues(fixture.controller),
             (QStringList{QStringLiteral("0"), QStringLiteral("1")}));
    QVERIFY(seekExactlyAndSettle(fixture.controller, openingEnd));
    QCOMPARE(offerValues(fixture.controller), QStringList{QStringLiteral("1")});
    QVERIFY(seekAndSettle(fixture.controller, duration * 0.70));
    QCOMPARE(offerValues(fixture.controller), QStringList{QStringLiteral("1")});
    QVERIFY(seekAndSettle(fixture.controller, duration * 0.82));
    QVERIFY(offers(fixture.controller).isEmpty());
    QVERIFY(seekAndSettle(fixture.controller, duration * 0.90));
    QCOMPARE(offerValues(fixture.controller), QStringList{QStringLiteral("3")});
    QVERIFY(seekAndSettle(fixture.controller, duration * 0.97));
    QVERIFY(offers(fixture.controller).isEmpty());
}

void TestSkipSegments::aLateSkipListUsesTheCurrentPlaybackPosition() {
    if (!requireRuntime()) return;
    PlayerFixture fixture;
    QString error;
    QVERIFY2(fixture.open(&error), qPrintable(error));
    VideoSession* session = fixture.controller.session();
    QTRY_VERIFY_WITH_TIMEOUT(session->duration() > 2.0, 15000);
    const double duration = session->duration();
    QVERIFY(seekAndSettle(fixture.controller, duration * 0.50));
    QVERIFY(offers(fixture.controller).isEmpty());

    fixture.controller.setSkipSegments(
        {segment("recap", duration * 0.30, duration * 0.70)});
    const QVector<PlayerMenuModel::Row> current = offers(fixture.controller);
    QCOMPARE(current.size(), 1);
    QCOMPARE(current.constFirst().value, QStringLiteral("0"));
}

void TestSkipSegments::activatingAnOfferSeeksTheRealSessionToItsEnd() {
    if (!requireRuntime()) return;
    PlayerFixture fixture;
    QString error;
    QVERIFY2(fixture.open(&error), qPrintable(error));
    VideoSession* session = fixture.controller.session();
    QTRY_VERIFY_WITH_TIMEOUT(session->duration() > 2.0, 15000);
    const double duration = session->duration();
    const double firstEnd = duration * 0.70;
    const double secondEnd = duration * 0.60;
    fixture.controller.setSkipSegments({segment("mixed_opening", duration * 0.20, firstEnd),
                                        segment("recap", duration * 0.30, secondEnd)});
    QVERIFY(seekAndSettle(fixture.controller, duration * 0.40));
    QCOMPARE(offers(fixture.controller).size(), 2);

    fixture.controller.activateSkipOffer(1);
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(session->position() - secondEnd) < 0.25, 8000);
    QVERIFY(std::abs(session->position() - secondEnd) < 0.25);
}
