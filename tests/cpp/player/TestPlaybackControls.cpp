#include "TestPlaybackControls.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QThread>
#include <QWheelEvent>
#include <QWidget>

#include <cmath>
#include <limits>
#include <memory>
#include <optional>

#include "MpvApi.hpp"
#include "PipFloat.hpp"
#include "PlayerDialog.hpp"
#include "PlayerShell.hpp"
#include "ShellSliders.hpp"
#include "ShellWidgets.hpp"
#include "VideoSurface.hpp"

using mplayer::MpvApi;
using mplayer::MpvBundle;

namespace {

constexpr int kControlWidth = 220;
constexpr int kControlHeight = 24;

std::unique_ptr<QWidget> makeControl(int kind) {
    std::unique_ptr<QWidget> control = kind == 0
                                          ? std::unique_ptr<QWidget>(new mplayer::shell::RangeSlider())
                                          : std::unique_ptr<QWidget>(new mplayer::shell::VolumeTrack());
    control->resize(kControlWidth, kControlHeight);
    return control;
}

double valueOf(QWidget* control) {
    if (auto* range = qobject_cast<mplayer::shell::RangeSlider*>(control)) return range->value();
    if (auto* track = qobject_cast<mplayer::shell::VolumeTrack*>(control)) return track->value();
    return std::numeric_limits<double>::quiet_NaN();
}

bool draggingOf(QWidget* control) {
    if (auto* range = qobject_cast<mplayer::shell::RangeSlider*>(control)) return range->isDragging();
    if (auto* track = qobject_cast<mplayer::shell::VolumeTrack*>(control)) return track->isDragging();
    return false;
}

// The mapping belongs to the widget: a fresh control pressed at `x` answers
// with its own fraction, so no test restates the margin or knob inset.
double valueAtOnAFreshControl(int kind, qreal x) {
    const std::unique_ptr<QWidget> probe = makeControl(kind);
    const QPointF local(x, kControlHeight / 2.0);
    QMouseEvent press(QEvent::MouseButtonPress, local,
                      QPointF(probe->mapToGlobal(local.toPoint())), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(probe.get(), &press);
    return valueOf(probe.get());
}

void sendPointer(QWidget* control, QEvent::Type type, qreal x, Qt::MouseButton button,
                 Qt::MouseButtons held) {
    const QPointF local(x, kControlHeight / 2.0);
    QMouseEvent event(type, local, QPointF(control->mapToGlobal(local.toPoint())), button, held,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(control, &event);
}

bool sendWheel(QWidget* target, const QPoint& angle, const QPoint& pixels = {}) {
    const QPointF local = target->rect().center();
    QWheelEvent event(local, target->mapToGlobal(local.toPoint()), pixels, angle,
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    event.ignore();
    QCoreApplication::sendEvent(target, &event);
    return event.isAccepted();
}

void sendKey(QWidget* target, int key) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &press);
    QCoreApplication::sendEvent(target, &release);
}

// An explicit path, never a search: the tests must not provision a runtime.
MpvBundle envBundle() {
    const QString module = qEnvironmentVariable("MIKAI_MPV_MODULE");
    if (module.isEmpty()) return MpvBundle{};
    const QFileInfo info(module);
    MpvBundle bundle;
    bundle.root = info.absolutePath();
    bundle.modulePath = info.absoluteFilePath();
    bundle.descriptorId = QStringLiteral("env-standard-runtime");
    bundle.protocolVersion = 1;
    return bundle;
}

QString fixtureClip() {
    return QDir::toNativeSeparators(
        QStringLiteral(MIKAI_SOURCE_DIR "/tests/fixtures/media/motion_320x180_24fps_8bit_hevc.mp4"));
}

bool nativePlatformAvailable() {
    const QString platform = QGuiApplication::platformName();
    return platform != QStringLiteral("offscreen") && platform != QStringLiteral("minimal");
}

std::optional<double> coreNumber(mplayer::VideoSurface* surface, const char* name) {
    const auto value =
        surface->probeMpvProperty(QString::fromLatin1(name), mplayer::ProbePropertyKind::Number);
    if (!value || !value->isNumeric()) return std::nullopt;
    return value->asNumber();
}

// The surface's own 50 ms pump drains the core, so processEvents is what
// advances anything here.
template <typename Predicate>
bool settleUntil(Predicate predicate, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!predicate() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(10);
    }
    return predicate();
}

// Only a reading that stopped changing counts: a value read before the seeks
// ran would otherwise pass for a result.
double settledCorePosition(mplayer::VideoSurface* surface, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    QElapsedTimer stable;
    stable.start();
    double previous = std::numeric_limits<double>::quiet_NaN();
    while (clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(15);
        const std::optional<double> now = coreNumber(surface, "time-pos");
        if (!now) continue;
        if (!std::isfinite(previous) || std::abs(*now - previous) > 0.01) {
            previous = *now;
            stable.restart();
            continue;
        }
        if (stable.elapsed() > 600 && clock.elapsed() > 800) return *now;
    }
    return -1.0;
}

std::optional<QString> coreText(mplayer::VideoSurface* surface, const char* name) {
    const auto value =
        surface->probeMpvProperty(QString::fromLatin1(name), mplayer::ProbePropertyKind::Text);
    if (!value || value->text.isEmpty()) return std::nullopt;
    return value->text;
}

bool coreIsPaused(mplayer::VideoSurface* surface) {
    const auto paused =
        surface->probeMpvProperty(QStringLiteral("pause"), mplayer::ProbePropertyKind::Flag);
    return paused.has_value() && paused->flag;
}

}  // namespace

void TestPlaybackControls::initTestCase() {
    if (MpvApi::instance().loaded()) return;
    const MpvBundle bundle = envBundle();
    if (bundle.modulePath.isEmpty()) return;
    QString reason;
    if (!MpvApi::instance().load(bundle, &reason)) selectionError_ = reason;
}

bool TestPlaybackControls::requireRuntime() {
    if (MpvApi::instance().loaded()) return true;
    if (!selectionError_.isEmpty()) {
        QTest::qFail(qPrintable(QStringLiteral("cannot select %1: %2")
                                    .arg(envBundle().modulePath, selectionError_)),
                     __FILE__, __LINE__);
        return false;
    }
    QTest::qSkip("set MIKAI_MPV_MODULE to the libmpv-2.dll of a real runtime to run this",
                 __FILE__, __LINE__);
    return false;
}

void TestPlaybackControls::finalValueFollowsTheReleasePosition_data() {
    QTest::addColumn<int>("kind");
    QTest::addColumn<qreal>("pressAt");
    QTest::addColumn<qreal>("moveAt");
    QTest::addColumn<qreal>("releaseAt");

    // The two positions differ by design, so a control that keeps the last
    // move's value cannot answer with the release point by accident.
    QTest::newRow("range/last-move-left-of-release") << 0 << qreal(0.20) << qreal(0.20) << qreal(0.80);
    QTest::newRow("range/last-move-right-of-release") << 0 << qreal(0.10) << qreal(0.60) << qreal(0.25);
    QTest::newRow("range/release-clamped-left") << 0 << qreal(0.40) << qreal(0.50) << qreal(-0.5);
    QTest::newRow("range/release-clamped-right") << 0 << qreal(0.40) << qreal(0.30) << qreal(1.5);
    QTest::newRow("volume/last-move-left-of-release") << 1 << qreal(0.20) << qreal(0.20) << qreal(0.80);
    QTest::newRow("volume/last-move-right-of-release") << 1 << qreal(0.10) << qreal(0.60) << qreal(0.25);
    QTest::newRow("volume/release-clamped-left") << 1 << qreal(0.40) << qreal(0.50) << qreal(-0.5);
    QTest::newRow("volume/release-clamped-right") << 1 << qreal(0.40) << qreal(0.30) << qreal(1.5);
}

void TestPlaybackControls::finalValueFollowsTheReleasePosition() {
    QFETCH(int, kind);
    QFETCH(qreal, pressAt);
    QFETCH(qreal, moveAt);
    QFETCH(qreal, releaseAt);

    const std::unique_ptr<QWidget> control = makeControl(kind);
    const qreal width = control->width();
    const double expected = valueAtOnAFreshControl(kind, releaseAt * width);

    sendPointer(control.get(), QEvent::MouseButtonPress, pressAt * width, Qt::LeftButton,
                Qt::LeftButton);
    sendPointer(control.get(), QEvent::MouseMove, moveAt * width, Qt::NoButton, Qt::LeftButton);
    const double afterMove = valueOf(control.get());
    QVERIFY2(std::isfinite(afterMove), "the drag never took the press point");
    QVERIFY2(std::abs(afterMove - expected) > 0.05,
             "the row must release somewhere other than its last move");

    sendPointer(control.get(), QEvent::MouseButtonRelease, releaseAt * width, Qt::LeftButton,
                Qt::NoButton);

    QVERIFY2(std::abs(valueOf(control.get()) - expected) < 1e-3,
             qPrintable(QStringLiteral("the release point maps to %1, visible %2")
                            .arg(expected)
                            .arg(valueOf(control.get()))));
    QVERIFY2(!draggingOf(control.get()), "a left release ends the left drag");
}

void TestPlaybackControls::anUnrelatedButtonReleaseKeepsTheLeftDragAlive_data() {
    QTest::addColumn<int>("kind");
    QTest::newRow("range") << 0;
    QTest::newRow("volume") << 1;
}

void TestPlaybackControls::anUnrelatedButtonReleaseKeepsTheLeftDragAlive() {
    QFETCH(int, kind);

    const std::unique_ptr<QWidget> control = makeControl(kind);
    const qreal width = control->width();
    const double heldTarget = valueAtOnAFreshControl(kind, 0.75 * width);

    sendPointer(control.get(), QEvent::MouseButtonPress, 0.25 * width, Qt::LeftButton,
                Qt::LeftButton);
    sendPointer(control.get(), QEvent::MouseMove, 0.35 * width, Qt::NoButton, Qt::LeftButton);
    QVERIFY2(draggingOf(control.get()), "the left press started a drag");

    // A button this drag never owned, released elsewhere while left is held.
    sendPointer(control.get(), QEvent::MouseButtonRelease, 0.50 * width, Qt::RightButton,
                Qt::LeftButton);
    QVERIFY2(draggingOf(control.get()), "an unrelated release must not end the left drag");
    QVERIFY2(std::abs(valueOf(control.get()) - heldTarget) > 0.05,
             "the unrelated release must not move the control to its own position");

    sendPointer(control.get(), QEvent::MouseMove, 0.75 * width, Qt::NoButton, Qt::LeftButton);
    QVERIFY2(std::abs(valueOf(control.get()) - heldTarget) < 1e-3,
             qPrintable(QStringLiteral("the held pointer must still drive the value: expected %1, "
                                       "visible %2")
                            .arg(heldTarget)
                            .arg(valueOf(control.get()))));

    sendPointer(control.get(), QEvent::MouseButtonRelease, 0.75 * width, Qt::LeftButton,
                Qt::NoButton);
    QVERIFY2(!draggingOf(control.get()), "the left release ends the drag");
}

void TestPlaybackControls::aLostReleaseEndsTheDragWithoutCommittingTheHover_data() {
    QTest::addColumn<int>("kind");
    QTest::newRow("range") << 0;
    QTest::newRow("volume") << 1;
}

void TestPlaybackControls::aLostReleaseEndsTheDragWithoutCommittingTheHover() {
    QFETCH(int, kind);

    const std::unique_ptr<QWidget> control = makeControl(kind);
    const qreal width = control->width();
    const double heldValue = valueAtOnAFreshControl(kind, 0.30 * width);

    sendPointer(control.get(), QEvent::MouseButtonPress, 0.20 * width, Qt::LeftButton,
                Qt::LeftButton);
    sendPointer(control.get(), QEvent::MouseMove, 0.30 * width, Qt::NoButton, Qt::LeftButton);
    QVERIFY2(draggingOf(control.get()), "the left press started a drag");
    QVERIFY2(std::abs(valueOf(control.get()) - heldValue) < 1e-3,
             "the held move drives the visible value");

    // The release was lost: the pointer keeps moving with no button held, which
    // is not a position the drag may commit and not a drag either.
    sendPointer(control.get(), QEvent::MouseMove, 0.80 * width, Qt::NoButton, Qt::NoButton);
    QVERIFY2(!draggingOf(control.get()), "a move with no button held ends the drag");
    QVERIFY2(std::abs(valueOf(control.get()) - heldValue) < 1e-3,
             qPrintable(QStringLiteral("the lost release must keep the held value %1, visible %2")
                            .arg(heldValue)
                            .arg(valueOf(control.get()))));

    sendPointer(control.get(), QEvent::MouseMove, 0.60 * width, Qt::NoButton, Qt::NoButton);
    QVERIFY2(!draggingOf(control.get()), "hovering must not restart the drag");
    QVERIFY2(std::abs(valueOf(control.get()) - heldValue) < 1e-3,
             "hovering must not move the value");
}

void TestPlaybackControls::aPiPArrowStepIsResolvedByTheCoreWhenThePumpIsSuspended_data() {
    QTest::addColumn<int>("firstKey");
    QTest::addColumn<int>("secondKey");
    QTest::addColumn<double>("expected");

    // From 1.0 s the cached position cannot answer either row: +10 then -10
    // returns to 1 s, and a lone -10 clamps at the start. An absolute step
    // computed from the stale 1.0 s would resolve -9 against the media end.
    QTest::newRow("right-then-left/returns-to-1s")
        << static_cast<int>(Qt::Key_Right) << static_cast<int>(Qt::Key_Left) << 1.0;
    QTest::newRow("left/lands-on-zero") << static_cast<int>(Qt::Key_Left) << -1 << 0.0;
}

void TestPlaybackControls::aPiPArrowStepIsResolvedByTheCoreWhenThePumpIsSuspended() {
    QFETCH(int, firstKey);
    QFETCH(int, secondKey);
    QFETCH(double, expected);

    if (!nativePlatformAvailable()) {
        QSKIP("the PiP float needs a native window and a real GL context");
    }
    if (!requireRuntime()) return;

    const QString fixture = fixtureClip();
    QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));

    mplayer::shell::PipFloat pip;
    pip.resize(480, 270);
    pip.show();
    mplayer::VideoSurface* surface = pip.videoSurface();
    QVERIFY2(surface != nullptr, "the float owns a surface");
    // A surface that was never painted never gets a render context.
    if (!QTest::qWaitForWindowExposed(&pip, 15000)) {
        QSKIP("the float's window was never exposed on this platform");
    }
    if (!surface->waitForContext(20000)) {
        QSKIP("no OpenGL context for the float's surface");
    }

    pip.openStream(fixture, 1.0, true, 1);
    QVERIFY2(settleUntil([surface] { return surface->duration() > 10.0 && surface->position() > 0.5; },
                         30000),
             qPrintable(QStringLiteral("the float never loaded %1 (duration %2, position %3)")
                            .arg(fixture)
                            .arg(surface->duration())
                            .arg(surface->position())));
    // The deferred seek lands after the load; the steps below need a known start.
    QVERIFY2(settleUntil([surface] { return std::abs(surface->position() - 1.0) < 0.25; }, 15000),
             qPrintable(QStringLiteral("the float never reached 1.0 s (position %1)")
                            .arg(surface->position())));

    // Parked pump: the observed position stays cached while the core moves.
    surface->setPumpSuspended(true);
    QThread::msleep(150);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const double frozen = surface->position();
    QVERIFY2(std::abs(frozen - 1.0) < 0.25,
             qPrintable(QStringLiteral("the observed position must stay cached at 1.0 s, is %1")
                            .arg(frozen)));

    sendKey(&pip, firstKey);
    if (secondKey >= 0) sendKey(&pip, secondKey);

    surface->setPumpSuspended(false);

    const double settled = settledCorePosition(surface, 15000);
    QVERIFY2(settled >= 0.0, "the core never reported a stable time-pos after the steps");
    QVERIFY2(std::abs(settled - expected) < 0.4,
             qPrintable(QStringLiteral("a step the core resolves must land on %1 s; it reports %2")
                            .arg(expected)
                            .arg(settled)));
    QVERIFY2(coreIsPaused(surface), "seeking must not unpause the float");
}

void TestPlaybackControls::volumeWheelAdjustsTheTrackAndSpeaker() {
    mplayer::shell::VolumeGroup volume;
    volume.setVolume(0.5);
    auto* track = volume.findChild<mplayer::shell::VolumeTrack*>();
    QVERIFY(track != nullptr);

    QVERIFY(sendWheel(track, QPoint(0, 120)));
    QVERIFY(std::abs(volume.effective() - 0.55) < 0.0001);
    QVERIFY(sendWheel(&volume, QPoint(0, -240)));
    QVERIFY(std::abs(volume.effective() - 0.45) < 0.0001);
    QVERIFY(sendWheel(track, QPoint(0, 30)));
    QVERIFY(std::abs(volume.effective() - 0.4625) < 0.0001);
    QVERIFY(sendWheel(track, QPoint(), QPoint(0, 20)));
    QVERIFY(std::abs(volume.effective() - 0.4875) < 0.0001);

    const double beforeHorizontal = volume.effective();
    sendWheel(track, QPoint(120, 0));
    QVERIFY(std::abs(volume.effective() - beforeHorizontal) < 0.0001);
    volume.setVolume(0.99);
    QVERIFY(sendWheel(track, QPoint(0, 120)));
    QCOMPARE(volume.effective(), 1.0);
    volume.setVolume(0.01);
    QVERIFY(sendWheel(&volume, QPoint(0, -120)));
    QCOMPARE(volume.effective(), 0.0);
}

void TestPlaybackControls::volumeWheelFollowsPlaybackIntoPip() {
    if (!nativePlatformAvailable()) {
        QSKIP("the player needs a native window and a real GL context");
    }
    if (!requireRuntime()) return;

    const QString fixture = fixtureClip();
    QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));
    const std::unique_ptr<mplayer::shell::PlayerDialog> dialog(
        new mplayer::shell::PlayerDialog(QStringLiteral("volume wheel"), fixture));
    auto* shell = dialog->findChild<mplayer::shell::PlayerShell*>();
    QVERIFY(shell != nullptr);
    auto* surface = shell->findChild<mplayer::VideoSurface*>();
    auto* volume = shell->findChild<mplayer::shell::VolumeGroup*>();
    QVERIFY(surface != nullptr);
    QVERIFY(volume != nullptr);
    auto* track = volume->findChild<mplayer::shell::VolumeTrack*>();
    QVERIFY(track != nullptr);

    dialog->show();
    if (!QTest::qWaitForWindowExposed(dialog.get(), 15000)) {
        QSKIP("the player window was never exposed on this platform");
    }
    if (!surface->waitForContext(20000)) {
        QSKIP("no OpenGL context for the player surface");
    }
    dialog->startPlayer();
    QVERIFY2(settleUntil([surface] { return surface->duration() > 10.0; }, 30000),
             "the player did not load the fixture");

    shell->wake();
    QTRY_VERIFY(track->isVisible());
    const double initial = volume->effective();
    QTest::mouseMove(track, track->rect().center());
    QVERIFY(sendWheel(track, QPoint(0, -120)));
    const double lowered = volume->effective();
    QVERIFY(std::abs(lowered - (initial - 0.05)) < 0.001);
    auto* toast = shell->findChild<mplayer::shell::Toast*>();
    QVERIFY(toast != nullptr);
    auto* label = toast->findChild<QLabel*>();
    QVERIFY(label != nullptr);
    QVERIFY(label->text().contains(QString::number(qRound(lowered * 100.0))));
    QVERIFY2(settleUntil([surface, lowered] {
                 const auto actual = coreNumber(surface, "volume");
                 return actual && std::abs(*actual - lowered * 100.0) < 0.1;
             }, 10000), "the wheel did not change the playing core");

    sendPointer(volume, QEvent::MouseButtonPress, 10.0, Qt::LeftButton, Qt::LeftButton);
    QVERIFY(volume->isMuted());
    QVERIFY2(settleUntil([surface] {
                 const auto muted = surface->probeMpvProperty(QStringLiteral("mute"),
                                                               mplayer::ProbePropertyKind::Flag);
                 return muted && muted->flag;
             }, 10000), "the main core did not mute");
    QVERIFY(sendWheel(track, QPoint(0, 120)));
    QVERIFY(!volume->isMuted());
    QVERIFY2(settleUntil([surface, volume] {
                 const auto actual = coreNumber(surface, "volume");
                 const auto muted = surface->probeMpvProperty(QStringLiteral("mute"),
                                                               mplayer::ProbePropertyKind::Flag);
                 return actual && muted && !muted->flag &&
                        std::abs(*actual - volume->effective() * 100.0) < 0.1;
             }, 10000), "scrolling the muted control did not unmute the main core");

    QVERIFY(QMetaObject::invokeMethod(shell, "pipRequested", Qt::DirectConnection));
    mplayer::shell::PipFloat* pip = nullptr;
    QVERIFY2(settleUntil([&pip] {
                 for (QWidget* window : QApplication::topLevelWidgets()) {
                     if (auto* candidate = qobject_cast<mplayer::shell::PipFloat*>(window);
                         candidate != nullptr && candidate->isVisible()) {
                         pip = candidate;
                         return true;
                     }
                 }
                 return false;
             }, 15000), "PiP did not open");
    auto* pipSurface = pip->videoSurface();
    auto* pipTrack = pip->findChild<mplayer::shell::VolumeTrack*>(QStringLiteral("pipVolume"));
    auto* pipMute = pip->findChild<QPushButton*>(QStringLiteral("pipMute"));
    QVERIFY(pipSurface != nullptr);
    QVERIFY(pipTrack != nullptr);
    QVERIFY(pipMute != nullptr);
    QVERIFY2(settleUntil([pipSurface] { return pipSurface->duration() > 10.0; }, 30000),
             "PiP did not load the fixture");

    pipMute->click();
    QVERIFY2(settleUntil([pipSurface] {
                 const auto muted = pipSurface->probeMpvProperty(QStringLiteral("mute"),
                                                                  mplayer::ProbePropertyKind::Flag);
                 return muted && muted->flag;
             }, 10000), "the PiP core did not mute");
    QVERIFY(sendWheel(pipMute, QPoint(0, 120)));
    const double pipLevel = pipTrack->value();
    QVERIFY2(settleUntil([pipSurface, surface, volume, pipLevel] {
                 const auto pipVolume = coreNumber(pipSurface, "volume");
                 const auto mainVolume = coreNumber(surface, "volume");
                 const auto pipMuted = pipSurface->probeMpvProperty(QStringLiteral("mute"),
                                                                    mplayer::ProbePropertyKind::Flag);
                 const auto mainMuted = surface->probeMpvProperty(QStringLiteral("mute"),
                                                                  mplayer::ProbePropertyKind::Flag);
                 return pipVolume && mainVolume && pipMuted && mainMuted &&
                        !pipMuted->flag && !mainMuted->flag &&
                        std::abs(*pipVolume - pipLevel * 100.0) < 0.1 &&
                        std::abs(*mainVolume - pipLevel * 100.0) < 0.1 &&
                        std::abs(volume->effective() - pipLevel) < 0.001;
             }, 15000), "the PiP wheel did not synchronize volume and mute with the main player");
    QVERIFY(QMetaObject::invokeMethod(pip, "restoreRequested", Qt::DirectConnection));
    QVERIFY2(settleUntil([surface, volume, pipLevel] {
                 const auto actual = coreNumber(surface, "volume");
                 return actual && std::abs(*actual - pipLevel * 100.0) < 0.1 &&
                        std::abs(volume->effective() - pipLevel) < 0.001;
             }, 15000), "restoring the player lost the PiP volume");
}

void TestPlaybackControls::aNewPlayerStartsAtTheShownVolume() {
    if (!nativePlatformAvailable()) {
        QSKIP("the main player needs a native window and a real GL context");
    }
    if (!requireRuntime()) return;

    const QString fixture = fixtureClip();
    QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));
    const std::unique_ptr<mplayer::shell::PlayerDialog> dialog(
        new mplayer::shell::PlayerDialog(QStringLiteral("volume"), fixture));
    auto* shell = dialog->findChild<mplayer::shell::PlayerShell*>();
    QVERIFY(shell != nullptr);
    auto* surface = shell->findChild<mplayer::VideoSurface*>();
    auto* volume = shell->findChild<mplayer::shell::VolumeGroup*>();
    QVERIFY(surface != nullptr);
    QVERIFY(volume != nullptr);

    dialog->show();
    if (!QTest::qWaitForWindowExposed(dialog.get(), 15000)) {
        QSKIP("the player window was never exposed on this platform");
    }
    if (!surface->waitForContext(20000)) {
        QSKIP("no OpenGL context for the player surface");
    }
    dialog->startPlayer();
    QVERIFY2(settleUntil([surface] { return surface->duration() > 10.0; }, 30000),
             "the player did not load the fixture");
    const auto initialVolume = coreNumber(surface, "volume");
    QVERIFY(initialVolume.has_value());
    QVERIFY2(std::abs(*initialVolume - volume->effective() * 100.0) < 0.1,
             qPrintable(QStringLiteral("mpv volume %1 differs from displayed %2")
                            .arg(*initialVolume)
                            .arg(volume->effective() * 100.0)));

    volume->setVolume(0.25);
    QVERIFY2(settleUntil([surface, volume] {
                 const auto actual = coreNumber(surface, "volume");
                 return actual && std::abs(*actual - volume->effective() * 100.0) < 0.1;
             }, 10000),
             "the changed slider level did not reach mpv");
}

void TestPlaybackControls::aPipKeepsItsVolumeAndMuteBeforeOpening() {
    if (!nativePlatformAvailable()) {
        QSKIP("the PiP player needs a native window and a real GL context");
    }
    if (!requireRuntime()) return;

    const QString fixture = fixtureClip();
    QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));
    mplayer::shell::PipFloat pip;
    pip.setVolume(0.31);
    pip.setMuted(true);
    pip.resize(480, 270);
    pip.show();
    if (!QTest::qWaitForWindowExposed(&pip, 15000)) {
        QSKIP("the float's window was never exposed on this platform");
    }
    auto* surface = pip.videoSurface();
    QVERIFY(surface != nullptr);
    if (!surface->waitForContext(20000)) {
        QSKIP("no OpenGL context for the float's surface");
    }
    pip.openStream(fixture, 0.0, true, 1);
    QVERIFY2(settleUntil([surface] { return surface->duration() > 10.0; }, 30000),
             "the float did not load the fixture");

    const auto volume = coreNumber(surface, "volume");
    const auto muted = surface->probeMpvProperty(QStringLiteral("mute"),
                                                 mplayer::ProbePropertyKind::Flag);
    QVERIFY(volume.has_value());
    QVERIFY(std::abs(*volume - 31.0) < 0.1);
    QVERIFY(muted.has_value());
    QVERIFY(muted->flag);

    pip.setMuted(false);
    QVERIFY2(settleUntil([surface] {
                 const auto state = surface->probeMpvProperty(QStringLiteral("mute"),
                                                              mplayer::ProbePropertyKind::Flag);
                 return state && !state->flag;
             }, 10000),
             "the float did not unmute");
    const auto unmutedVolume = coreNumber(surface, "volume");
    QVERIFY(unmutedVolume.has_value());
    QVERIFY(std::abs(*unmutedVolume - 31.0) < 0.1);
}

void TestPlaybackControls::aPlayerDialogArrowStepIgnoresALateStaleProgressReport_data() {
    QTest::addColumn<int>("secondKey");
    QTest::addColumn<double>("stalePosition");
    QTest::addColumn<double>("expected");

    // Both rows start at 1.0 s and take one Right step to 6 s. The second step
    // must be resolved by the core, so a stale UI position cannot shorten,
    // invert or drop it: with 0 s cached a clamped delta would emit nothing.
    QTest::newRow("right/after-stale-1s") << static_cast<int>(Qt::Key_Right) << 1.0 << 11.0;
    QTest::newRow("left/after-stale-0s") << static_cast<int>(Qt::Key_Left) << 0.0 << 1.0;
}

void TestPlaybackControls::aPlayerDialogArrowStepIgnoresALateStaleProgressReport() {
    QFETCH(int, secondKey);
    QFETCH(double, stalePosition);
    QFETCH(double, expected);

    if (!nativePlatformAvailable()) {
        QSKIP("the main player needs a native window and a real GL context");
    }
    if (!requireRuntime()) return;

    const QString fixture = fixtureClip();
    QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));

    // The dialog deletes itself on close, so it is owned here and never closed.
    const std::unique_ptr<mplayer::shell::PlayerDialog> dialog(
        new mplayer::shell::PlayerDialog(QStringLiteral("controls"), fixture));
    dialog->show();
    if (!QTest::qWaitForWindowExposed(dialog.get(), 15000)) {
        QSKIP("the player window was never exposed on this platform");
    }
    auto* shell = dialog->findChild<mplayer::shell::PlayerShell*>();
    QVERIFY2(shell != nullptr, "the dialog hosts a shell");
    auto* surface = shell->findChild<mplayer::VideoSurface*>();
    QVERIFY2(surface != nullptr, "the shell hosts a surface");
    if (!surface->waitForContext(20000)) {
        QSKIP("no OpenGL context for the player surface");
    }

    dialog->startPlayer();
    QVERIFY2(settleUntil([surface] { return surface->duration() > 10.0; }, 30000),
             qPrintable(QStringLiteral("the player never loaded %1 (duration %2)")
                            .arg(fixture)
                            .arg(surface->duration())));
    surface->setPaused(true);
    surface->seek(1.0);
    QVERIFY2(settleUntil([surface] { return std::abs(surface->position() - 1.0) < 0.25; }, 15000),
             qPrintable(QStringLiteral("the player never reached 1.0 s (position %1)")
                            .arg(surface->position())));

    // Parked pump: the dialog's position feed and the shell's cached position
    // stop moving, so only the core can say where playback actually is.
    surface->setPumpSuspended(true);
    QThread::msleep(150);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QVERIFY2(std::abs(surface->position() - 1.0) < 0.25,
             qPrintable(QStringLiteral("the observed position must stay cached at 1.0 s, is %1")
                            .arg(surface->position())));

    // First arrow: 1.0 s in the core, so an honest step lands on 6 s. Waiting
    // for the core to arrive is what makes the second step unambiguous.
    sendKey(shell, Qt::Key_Right);
    QVERIFY2(settleUntil(
                 [surface] {
                     const std::optional<double> now = coreNumber(surface, "time-pos");
                     return now && std::abs(*now - 6.0) < 0.6;
                 },
                 10000),
             "the first arrow step never reached the core");

    // A late progress report for an older playhead.
    shell->setPosition(stalePosition);
    sendKey(shell, secondKey);
    surface->setPumpSuspended(false);

    const double settled = settledCorePosition(surface, 15000);
    QVERIFY2(settled >= 0.0, "the core never reported a stable time-pos after the steps");
    QVERIFY2(std::abs(settled - expected) < 0.5,
             qPrintable(QStringLiteral("a step resolved from the core must land on %1 s, not on "
                                       "the stale %2 s; the core reports %3")
                            .arg(expected)
                            .arg(stalePosition)
                            .arg(settled)));
    QVERIFY2(coreIsPaused(surface), "seeking must not unpause the player");
}

void TestPlaybackControls::aSkipForwardAtTheEndDoesNotRestartTheSource() {
    if (!nativePlatformAvailable()) {
        QSKIP("the main player needs a native window and a real GL context");
    }
    if (!requireRuntime()) return;

    const QString fixture = fixtureClip();
    QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));

    const std::unique_ptr<mplayer::shell::PlayerDialog> dialog(
        new mplayer::shell::PlayerDialog(QStringLiteral("controls"), fixture));
    dialog->show();
    if (!QTest::qWaitForWindowExposed(dialog.get(), 15000)) {
        QSKIP("the player window was never exposed on this platform");
    }
    auto* shell = dialog->findChild<mplayer::shell::PlayerShell*>();
    QVERIFY2(shell != nullptr, "the dialog hosts a shell");
    auto* surface = shell->findChild<mplayer::VideoSurface*>();
    QVERIFY2(surface != nullptr, "the shell hosts a surface");
    if (!surface->waitForContext(20000)) {
        QSKIP("no OpenGL context for the player surface");
    }

    dialog->startPlayer();
    QVERIFY2(settleUntil([surface] { return surface->duration() > 10.0; }, 30000),
             qPrintable(QStringLiteral("the player never loaded %1 (duration %2)")
                            .arg(fixture)
                            .arg(surface->duration())));
    surface->setPaused(true);
    surface->seek(1.0);
    QVERIFY2(settleUntil([surface] { return std::abs(surface->position() - 1.0) < 0.25; }, 15000),
             qPrintable(QStringLiteral("the player never reached 1.0 s (position %1)")
                            .arg(surface->position())));

    auto* skip = shell->findChild<QPushButton*>(QStringLiteral("playerSkipForward85"));
    QVERIFY2(skip != nullptr, "the shell exposes its 85 s skip action");

    const std::optional<double> duration = coreNumber(surface, "duration");
    const std::optional<QString> sourceBefore = coreText(surface, "filename");
    QVERIFY2(duration.has_value() && *duration > 10.0, "the core reports the loaded length");
    QVERIFY2(sourceBefore.has_value(), "the core names its loaded source");

    // 1 s + 85 s lands on the media end, where only the core may clamp.
    skip->click();
    const double firstEnd = settledCorePosition(surface, 15000);
    QVERIFY2(firstEnd >= 0.0, "the core never settled after the skip");
    QVERIFY2(firstEnd > *duration - 2.5,
             qPrintable(QStringLiteral("the skip must reach the end %1, core reports %2")
                            .arg(*duration)
                            .arg(firstEnd)));

    // A second skip at the end must be a no-op, not a reload or a rewind.
    skip->click();
    const double secondEnd = settledCorePosition(surface, 15000);
    QVERIFY2(secondEnd >= 0.0, "the core never settled after the second skip");
    QVERIFY2(secondEnd > *duration - 2.5,
             qPrintable(QStringLiteral("a skip at the end must stay at the end %1, core reports %2")
                            .arg(*duration)
                            .arg(secondEnd)));
    QVERIFY2(secondEnd >= firstEnd - 0.5,
             qPrintable(QStringLiteral("a skip at the end must not move backwards: %1 then %2")
                            .arg(firstEnd)
                            .arg(secondEnd)));
    QVERIFY2(coreIsPaused(surface), "the skip at the end must not unpause");

    const std::optional<double> durationAfter = coreNumber(surface, "duration");
    const std::optional<QString> sourceAfter = coreText(surface, "filename");
    QVERIFY2(sourceAfter.has_value() && *sourceAfter == *sourceBefore,
             "the skip at the end must not change the loaded source");
    QVERIFY2(durationAfter.has_value() && std::abs(*durationAfter - *duration) < 0.01,
             "the loaded source keeps its length");
}
