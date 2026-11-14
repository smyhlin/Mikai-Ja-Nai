#include "TestPlaybackControls.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QThread>
#include <QQuickItem>
#include <QQuickWindow>

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <QVariantMap>
#include <QVector>

#include "MpvApi.hpp"
#include "PlayerController.hpp"
#include "PlayerMenuModel.hpp"
#include "PlayerWindow.hpp"
#include "VideoSession.hpp"

using mplayer::MpvApi;
using mplayer::MpvBundle;
using mplayer::VideoSession;
using mplayer::quick::PlayerController;
using mplayer::quick::PlayerWindow;

namespace {

MpvBundle envBundle() {
    const QString module = qEnvironmentVariable("MIKAI_MPV_MODULE");
    if (module.isEmpty()) return {};
    const QFileInfo info(module);
    MpvBundle bundle;
    bundle.root = info.absolutePath();
    bundle.modulePath = info.absoluteFilePath();
    bundle.descriptorId = QStringLiteral("env-standard-runtime");
    bundle.protocolVersion = 1;
    return bundle;
}

QString fixtureClip(const char* name = "motion_320x180_24fps_8bit_hevc.mp4") {
    return QDir::toNativeSeparators(QStringLiteral(MIKAI_SOURCE_DIR "/tests/fixtures/media/") +
                                    QString::fromLatin1(name));
}

bool nativePlatformAvailable() {
    const QString platform = QGuiApplication::platformName();
    return platform != QStringLiteral("offscreen") && platform != QStringLiteral("minimal");
}

std::optional<double> coreNumber(VideoSession* session, const char* name) {
    const auto value = session->probeMpvProperty(QString::fromLatin1(name),
                                                 mplayer::ProbePropertyKind::Number);
    if (!value || !value->isNumeric()) return std::nullopt;
    return value->asNumber();
}

std::optional<QString> coreText(VideoSession* session, const char* name) {
    const auto value = session->probeMpvProperty(QString::fromLatin1(name),
                                                 mplayer::ProbePropertyKind::Text);
    if (!value || value->text.isEmpty()) return std::nullopt;
    return value->text;
}

bool coreIsPaused(VideoSession* session) {
    const auto paused = session->probeMpvProperty(QStringLiteral("pause"),
                                                  mplayer::ProbePropertyKind::Flag);
    return paused.has_value() && paused->flag;
}

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

double settledCorePosition(VideoSession* session, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    QElapsedTimer stable;
    stable.start();
    double previous = std::numeric_limits<double>::quiet_NaN();
    while (clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(15);
        const auto now = coreNumber(session, "time-pos");
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

bool openPlayer(PlayerController* controller) {
    PlayerWindow* window = controller->window();
    if (window == nullptr) return false;
    controller->showWindow();
    if (!QTest::qWaitForWindowExposed(window, 15000)) return false;
    controller->startPlayer();
    if (!window->session()->waitForContext(20000)) return false;
    return settleUntil([controller] { return controller->duration() > 10.0; }, 30000);
}
bool startPlaying(PlayerController* controller) {
    if (!controller->playing()) controller->togglePlayPause();
    return settleUntil([controller] { return controller->playing(); }, 10000);
}

class RecordingHost : public mplayer::shell::PlayerHost {
public:
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int index) override { requestedEpisodes.append(index); }
    void requestTrackProbe(const QString&) override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    void playbackFailed() override {}

    QList<int> requestedEpisodes;
};

QVector<QVariantMap> threeEpisodePlaylist() {
    QVector<QVariantMap> playlist;
    for (int episode = 1; episode <= 3; ++episode) {
        QVariantMap item;
        item.insert(QStringLiteral("label"), QStringLiteral("Episode %1").arg(episode));
        item.insert(QStringLiteral("number"), QString::number(episode));
        item.insert(QStringLiteral("embed"), QStringLiteral("https://example.invalid/ep%1").arg(episode));
        playlist.append(item);
    }
    return playlist;
}

QQuickItem* timelineItem(QQuickItem* item) {
    if (item == nullptr) return nullptr;
    if (item->objectName() == QStringLiteral("player.timeline.scrub")) return item;
    for (QQuickItem* child : item->childItems()) {
        if (QQuickItem* match = timelineItem(child)) return match;
    }
    return nullptr;
}

QPoint windowPoint(QQuickItem* item, qreal fraction) {
    return item->mapToScene(QPointF(item->width() * fraction, item->height() / 2.0)).toPoint();
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

void TestPlaybackControls::nestedEnhancementMenuActivatesTheSelectedPreset() {
    QSettings settings;
    const QString settingKey = QStringLiteral("player/anime4k");
    const bool hadStoredPreset = settings.contains(settingKey);
    const QVariant storedPreset = settings.value(settingKey);
    const auto restoreSetting = qScopeGuard([&settings, settingKey, hadStoredPreset, storedPreset] {
        if (hadStoredPreset) {
            settings.setValue(settingKey, storedPreset);
        } else {
            settings.remove(settingKey);
        }
        settings.sync();
    });
    settings.setValue(settingKey, QStringLiteral("off"));
    settings.sync();

    PlayerController controller(QStringLiteral("enhancement menu"), QString());
    const auto findValue = [](mplayer::quick::PlayerMenuModel* model, const QString& value) {
        if (model == nullptr) return -1;
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->row(row).value == value) return row;
        }
        return -1;
    };

    auto* root = controller.menuModel(QStringLiteral("enhancements"));
    QVERIFY(root != nullptr);
    const int restoreRow = findValue(root, QStringLiteral("processing:restore"));
    QVERIFY(restoreRow >= 0);
    QCOMPARE(root->row(restoreRow).action, mplayer::quick::PlayerMenuModel::Action::OpenMenu);

    auto* restore = controller.submenuModel(QStringLiteral("enhancements"), restoreRow);
    QVERIFY(restore != nullptr);
    const int anime4kRow = findValue(restore, QStringLiteral("anime4k"));
    QVERIFY(anime4kRow >= 0);
    auto* anime4k = controller.submenuModel(QStringLiteral("processing:restore"), anime4kRow);
    QVERIFY(anime4k != nullptr);
    const int qualityRow = findValue(anime4k, QStringLiteral("anime4k/quality"));
    QVERIFY(qualityRow >= 0);
    auto* quality = controller.submenuModel(QStringLiteral("anime4k"), qualityRow);
    QVERIFY(quality != nullptr);

    const int hqARow = quality->indexOfId(QStringLiteral("anime4k.hq_a"));
    QVERIFY(hqARow >= 0);
    const auto preset = quality->row(hqARow);
    QCOMPARE(preset.action, mplayer::quick::PlayerMenuModel::Action::SetEnhancement);
    QCOMPARE(preset.value, QStringLiteral("anime4k|hq_a"));

    controller.activateMenuRow(QStringLiteral("anime4k/quality"), hqARow);
    QCOMPARE(controller.anime4kChainId(), QStringLiteral("hq_a"));
    QCOMPARE(controller.session()->anime4KChainId(), QStringLiteral("hq_a"));
    QVERIFY(quality->row(hqARow).selected);
}

void TestPlaybackControls::qualityMenuSortsAvailableHeightsDescendingAndMaintainsSelection() {
    const QList<mplayer::shell::QualityEntry> qualities = {
        {480, QStringLiteral("stream-480")},
        {1080, QStringLiteral("stream-1080")},
        {720, QStringLiteral("stream-720")},
        {1080, QStringLiteral("duplicate-stream-1080")},
        {360, QStringLiteral("stream-360")},
    };
    RecordingHost host;
    PlayerController controller(QStringLiteral("quality menu"), QStringLiteral("stream-720"),
                                qualities, 720, &host);

    const auto* menu = controller.qualityMenu();
    QVERIFY(menu != nullptr);
    QCOMPARE(menu->rowCount(), 5);

    const auto autoRow = menu->row(0);
    QCOMPARE(autoRow.id, QStringLiteral("quality.auto"));
    QCOMPARE(autoRow.selected, true);
    QCOMPARE(autoRow.detail, QStringLiteral("720p"));

    const int expected[] = {1080, 720, 480, 360};
    for (int i = 0; i < 4; ++i) {
        const auto row = menu->row(i + 1);
        QCOMPARE(row.id, QStringLiteral("quality.%1").arg(expected[i]));
        QCOMPARE(row.label, QStringLiteral("%1p").arg(expected[i]));
        QCOMPARE(row.action, mplayer::quick::PlayerMenuModel::Action::SelectQuality);
        QCOMPARE(row.selected, false);
    }

    controller.selectQuality(480);
    QCOMPARE(controller.autoQuality(), false);
    QCOMPARE(controller.currentHeight(), 480);
    QCOMPARE(menu->row(0).selected, false);
    QCOMPARE(menu->row(3).selected, true);

    controller.selectQuality(0);
    QCOMPARE(controller.autoQuality(), true);
    QCOMPARE(menu->row(0).selected, true);
    QCOMPARE(menu->row(3).selected, false);
}

void TestPlaybackControls::transportActionsUpdateTypedControllerAndSessionState() {
    if (!nativePlatformAvailable()) QSKIP("the Quick player needs a native window and GL context");
    if (!requireRuntime()) return;
    const QString fixture = fixtureClip();
    QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));

    PlayerController controller(QStringLiteral("transport"), fixture);
    QVERIFY(openPlayer(&controller));
    VideoSession* const session = controller.session();
    QVERIFY(session != nullptr);
    QVERIFY(startPlaying(&controller));
 
    controller.setVolume(0.37);
    controller.setRate(1.25);
    controller.toggleMute();
    QVERIFY2(settleUntil([&] {
                 const auto volume = coreNumber(session, "volume");
                 const auto speed = coreNumber(session, "speed");
                 const auto muted = session->probeMpvProperty(QStringLiteral("mute"),
                                                              mplayer::ProbePropertyKind::Flag);
                 return volume && speed && muted && std::abs(*volume - 37.0) < 0.1 &&
                        std::abs(*speed - 1.25) < 0.01 && muted->flag;
             }, 10000), "typed Quick transport actions did not reach the session core");
    QCOMPARE(controller.volume(), 0.37);
    QCOMPARE(controller.rate(), 1.25);
    QVERIFY(controller.muted());

    controller.seekTo(1.0);
    QVERIFY(settleUntil([&] {
        return std::abs(controller.position() - 1.0) < 0.25 &&
               std::abs(session->position() - 1.0) < 0.25;
    }, 10000));
    controller.togglePlayPause();
    QVERIFY(settleUntil([&] { return !controller.playing() && coreIsPaused(session); }, 10000));
    QVERIFY(settleUntil([&] {
        return std::abs(controller.position() - session->position()) < 0.25;
    }, 5000));
}

void TestPlaybackControls::keyboardStepsAreResolvedByTheSessionCore() {
    if (!nativePlatformAvailable()) QSKIP("the Quick player needs a native window and GL context");
    if (!requireRuntime()) return;
    const QString fixture = fixtureClip();
    QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));

    PlayerController controller(QStringLiteral("transport"), fixture);
    QVERIFY(openPlayer(&controller));
    VideoSession* const session = controller.session();
    session->setPaused(true);
    session->seek(1.0);
    QVERIFY(settleUntil([&] { return std::abs(session->position() - 1.0) < 0.25; }, 10000));

    session->setPumpSuspended(true);
    QThread::msleep(150);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QVERIFY(std::abs(controller.position() - 1.0) < 0.25);
    QVERIFY(controller.handleKey(Qt::Key_Right, 0));
    QVERIFY(settleUntil([&] {
                 const auto now = coreNumber(session, "time-pos");
                 return now && std::abs(*now - 6.0) < 0.6;
             }, 10000));
    QVERIFY(controller.handleKey(Qt::Key_Left, 0));
    session->setPumpSuspended(false);

    const double settled = settledCorePosition(session, 15000);
    QVERIFY2(settled >= 0.0, "the session core did not settle after typed key actions");
    QVERIFY2(std::abs(settled - 1.0) < 0.5,
             qPrintable(QStringLiteral("two core-relative steps should return to 1 s; got %1")
                            .arg(settled)));
    QVERIFY(coreIsPaused(session));
}

void TestPlaybackControls::enginePauseHoldRetainsExplicitPauseIntent() {
    if (!nativePlatformAvailable()) QSKIP("the Quick player needs a native window and GL context");
    if (!requireRuntime()) return;
    const QString fixture = fixtureClip();
    QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));

    PlayerController controller(QStringLiteral("transport"), fixture);
    QVERIFY(openPlayer(&controller));
    VideoSession* const session = controller.session();
    QVERIFY(startPlaying(&controller));
    QVERIFY(!session->playbackPauseIntent());
 
    controller.beginEnginePreparation(QStringLiteral("Preparing"));
    QVERIFY(controller.enginePauseHold());
    QVERIFY(settleUntil([&] { return coreIsPaused(session); }, 10000));
    controller.togglePlayPause();
    QVERIFY(session->playbackPauseIntent());
    controller.endEnginePreparation(true, QStringLiteral("Ready"));
    QVERIFY(settleUntil([&] {
                 return !controller.enginePauseHold() && !controller.playing() && coreIsPaused(session);
             }, 10000));
 
    controller.togglePlayPause();
    QVERIFY(settleUntil([&] {
                 return controller.playing() && !session->playbackPauseIntent() && !coreIsPaused(session);
             }, 10000));
}

void TestPlaybackControls::compactPipKeepsTheSameWindowAndSession() {
    if (!nativePlatformAvailable()) QSKIP("the Quick player needs a native window and GL context");
    if (!requireRuntime()) return;
    const QString fixture = fixtureClip();
    QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));

    PlayerController controller(QStringLiteral("transport"), fixture);
    QVERIFY(openPlayer(&controller));
    PlayerWindow* const window = controller.window();
    VideoSession* const session = controller.session();
    const uint64_t epoch = session->sourceEpoch();
    const QString source = session->sourceUrl();
    controller.togglePip();
    QVERIFY(window->compactPip());
    QCOMPARE(controller.window(), window);
    QCOMPARE(controller.session(), session);
    QCOMPARE(session->sourceEpoch(), epoch);
    QCOMPARE(session->sourceUrl(), source);
    QVERIFY(controller.isPipActive());
    controller.togglePip();
    QVERIFY(!window->compactPip());
    QCOMPARE(session->sourceEpoch(), epoch);
}

void TestPlaybackControls::quickTimelinePointerSeeksAndTimeLabelsFormat() {
    if (!nativePlatformAvailable()) QSKIP("the Quick player needs a native window and GL context");
    if (!requireRuntime()) return;
    const QString fixture = fixtureClip();
    QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));

    PlayerController controller(QStringLiteral("timeline"), fixture);
    QVERIFY(openPlayer(&controller));
    controller.session()->setPaused(true);
    QVERIFY2(settleUntil([&] { return coreIsPaused(controller.session()); }, 5000),
             "the playback core did not pause before timeline interaction");
    auto* window = controller.window();
    QVERIFY(window != nullptr);
    auto* timeline = timelineItem(window->contentItem());
    QVERIFY2(timeline != nullptr, "the visible Quick scene must expose its actual timeline item");
    QVERIFY(timeline->width() > 0.0);
    QVERIFY(timeline->height() > 0.0);
    const double target = controller.duration() * 0.5;

    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, windowPoint(timeline, 0.5));
    QVERIFY2(settleUntil([&] {
                 return std::abs(controller.position() - target) < 0.75 &&
                        std::abs(controller.session()->position() - target) < 0.75;
             }, 10000),
             "clicking the Quick timeline did not seek the session");
    QTest::keyClick(window, Qt::Key_Right);
    QVERIFY2(settleUntil([&] { return std::abs(controller.position() - (target + 5.0)) < 0.75; },
                         10000),
             "the focused Quick timeline did not handle its relative keyboard seek");
    QVERIFY(coreIsPaused(controller.session()));
    QTest::keyClick(window, Qt::Key_Space);
    QVERIFY(settleUntil([&] { return controller.playing(); }, 10000));
    QTest::keyClick(window, Qt::Key_Space);
    QVERIFY(settleUntil([&] { return !controller.playing() && coreIsPaused(controller.session()); },
                        10000));

    QCOMPARE(controller.formatTime(3661.9), QStringLiteral("1:01:01"));
    QCOMPARE(controller.formatRemaining(0.0),
             QStringLiteral("-%1").arg(controller.formatTime(controller.duration())));
    QCOMPARE(controller.formatRemaining(controller.position()),
             QStringLiteral("-%1").arg(controller.formatTime(controller.duration() -
                                                              controller.position())));
}

void TestPlaybackControls::variantTransitionPreservesPositionPauseRateAndFirstReport() {
    if (!nativePlatformAvailable()) QSKIP("the Quick player needs a native window and GL context");
    if (!requireRuntime()) return;
    const QString first = fixtureClip();
    const QString second = fixtureClip("motion_320x180_23976fps_8bit_hevc.mp4");
    QVERIFY2(QFileInfo::exists(first), qPrintable(first));
    QVERIFY2(QFileInfo::exists(second), qPrintable(second));

    RecordingHost host;
    const QList<mplayer::shell::QualityEntry> qualities{
        mplayer::shell::QualityEntry{720, first}};
    PlayerController controller(QStringLiteral("Episode · 720p"), first, qualities, 720, &host);
    QVERIFY(openPlayer(&controller));
    VideoSession* const session = controller.session();
    controller.setRate(1.25);
    session->setPaused(true);
    session->seek(2.0);
    QVERIFY(settleUntil([&] { return std::abs(session->position() - 2.0) < 0.25; }, 10000));
    const uint64_t oldEpoch = session->sourceEpoch();
    QList<double> reportedPositions;
    connect(&controller, &PlayerController::positionChanged, &controller,
            [&] { reportedPositions.append(controller.position()); });

    controller.applyVariant({mplayer::shell::QualityEntry{480, second}},
                            QStringLiteral("Episode · 480p"), second);
    QVERIFY2(settleUntil([&] {
                 return session->sourceEpoch() != oldEpoch &&
                        session->sourceUrl().contains(QStringLiteral("23976fps")) &&
                        session->duration() > 10.0;
             }, 30000), "the Quick quality transition did not load the selected source");
    QVERIFY2(settleUntil([&] { return std::abs(session->position() - 2.0) < 0.35; }, 15000),
             qPrintable(QStringLiteral("variant swap lost the playhead: %1")
                            .arg(session->position())));
    QVERIFY(coreIsPaused(session));
    QVERIFY2(settleUntil([&] {
                 const auto speed = coreNumber(session, "speed");
                 return speed && std::abs(*speed - 1.25) < 0.01;
             }, 10000), "variant swap lost the playback rate");
    QVERIFY(!reportedPositions.isEmpty());
    QVERIFY2(reportedPositions.first() >= 1.5,
             qPrintable(QStringLiteral("controller reported the new timeline before restoring "
                                       "the carried playhead: %1")
                            .arg(reportedPositions.first())));

    QVERIFY(startPlaying(&controller));
    QVERIFY(settleUntil([&] { return session->position() > 2.2; }, 5000));
    const double playingPosition = session->position();
    const uint64_t pausedSwapEpoch = session->sourceEpoch();
    controller.applyVariant({mplayer::shell::QualityEntry{720, first}},
                            QStringLiteral("Episode · 720p"), first);
    QVERIFY2(settleUntil([&] {
                 return session->sourceEpoch() != pausedSwapEpoch &&
                        session->sourceUrl().contains(QStringLiteral("24fps")) &&
                        session->duration() > 10.0 &&
                        std::abs(session->position() - playingPosition) < 0.5;
             }, 30000), "playing variant swap did not restore its current position");
    QVERIFY2(settleUntil([&] { return controller.playing() && !coreIsPaused(session); }, 10000),
             "variant swap failed to continue playback");
}

void TestPlaybackControls::episodeRestartRetainsExplicitPauseIntent() {
    if (!nativePlatformAvailable()) QSKIP("the Quick player needs a native window and GL context");
    if (!requireRuntime()) return;
    const QString first = fixtureClip();
    const QString second = fixtureClip("motion_320x180_23976fps_8bit_hevc.mp4");
    QVERIFY2(QFileInfo::exists(first), qPrintable(first));
    QVERIFY2(QFileInfo::exists(second), qPrintable(second));

    RecordingHost host;
    PlayerController controller(QStringLiteral("Episode 1 · 720p"), first,
                                {mplayer::shell::QualityEntry{720, first}}, 720, &host);
    QVERIFY(openPlayer(&controller));
    VideoSession* const session = controller.session();
    session->setPaused(true);
    session->seek(2.0);
    QVERIFY(settleUntil([&] { return std::abs(session->position() - 2.0) < 0.25; }, 10000));
    const uint64_t oldEpoch = session->sourceEpoch();

    controller.applyEpisode(QStringLiteral("Episode 2"), QStringLiteral("episode-2"), 1,
                            {mplayer::shell::QualityEntry{480, second}}, {});
    QVERIFY2(settleUntil([&] {
                 return session->sourceEpoch() != oldEpoch &&
                        session->sourceUrl().contains(QStringLiteral("23976fps")) &&
                        session->duration() > 10.0;
             }, 30000), "episode restart did not load the selected source");
    QVERIFY2(settleUntil([&] { return coreIsPaused(session); }, 10000),
             "episode restart discarded the viewer's explicit pause intent");
    QVERIFY2(settleUntil([&] { return session->position() < 0.5; }, 10000),
             "a new episode must start from the beginning, not the prior playhead");
}

void TestPlaybackControls::refreshedStreamStartsAtBeginningWithoutResuming() {
    if (!nativePlatformAvailable()) QSKIP("the Quick player needs a native window and GL context");
    if (!requireRuntime()) return;
    const QString first = fixtureClip();
    const QString refreshed = fixtureClip("motion_320x180_23976fps_8bit_hevc.mp4");
    QVERIFY2(QFileInfo::exists(first), qPrintable(first));
    QVERIFY2(QFileInfo::exists(refreshed), qPrintable(refreshed));

    RecordingHost host;
    PlayerController controller(QStringLiteral("Episode · 720p"), first,
                                {mplayer::shell::QualityEntry{720, first}}, 720, &host);
    QVERIFY(openPlayer(&controller));
    VideoSession* const session = controller.session();
    session->setPaused(true);
    session->seek(3.0);
    QVERIFY(settleUntil([&] { return std::abs(session->position() - 3.0) < 0.25; }, 10000));
    const uint64_t oldEpoch = session->sourceEpoch();

    controller.applyRefreshedStreams({mplayer::shell::QualityEntry{720, refreshed}}, 720);
    QVERIFY2(settleUntil([&] {
                 return session->sourceEpoch() != oldEpoch &&
                        session->sourceUrl().contains(QStringLiteral("23976fps")) &&
                        session->duration() > 10.0;
             }, 30000), "refreshed stream did not load");
    QVERIFY2(settleUntil([&] { return session->position() < 0.5; }, 10000),
             qPrintable(QStringLiteral("refreshed stream incorrectly resumed at %1")
                            .arg(session->position())));
    QVERIFY(coreIsPaused(session));
}

void TestPlaybackControls::skipForwardUsesEightyFiveSecondsWhenReachable() {
    if (!nativePlatformAvailable()) QSKIP("the Quick player needs a native window and GL context");
    if (!requireRuntime()) return;
    const QString fixture = QDir::toNativeSeparators(
        QStringLiteral(MIKAI_SOURCE_DIR "/resources/benchmarks/1920x1080.mp4"));
    QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));

    PlayerController controller(QStringLiteral("skip forward"), fixture);
    QVERIFY(openPlayer(&controller));
    VideoSession* const session = controller.session();
    const auto duration = coreNumber(session, "duration");
    QVERIFY(duration && *duration > 10.0);
    if (*duration <= 86.0) QSKIP("the selected real benchmark cannot reach the 85-second target");
    session->setPaused(true);
    session->seek(1.0);
    QVERIFY(settleUntil([&] { return std::abs(session->position() - 1.0) < 0.25; }, 10000));
    QVERIFY2(settleUntil([&] { return coreIsPaused(session); }, 5000),
             "the paused seek intent did not reach the playback core");

    controller.skipForward();
    QVERIFY2(settleUntil([&] { return std::abs(session->position() - 86.0) < 0.75; }, 10000),
             qPrintable(QStringLiteral("skip-forward did not seek 85 seconds: %1")
                            .arg(session->position())));
    QVERIFY(coreIsPaused(session));
}

void TestPlaybackControls::skipAtEndDoesNotRestartTheSource() {
    if (!nativePlatformAvailable()) QSKIP("the Quick player needs a native window and GL context");
    if (!requireRuntime()) return;
    const QString fixture = fixtureClip();
    QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));

    PlayerController controller(QStringLiteral("transport"), fixture);
    QVERIFY(openPlayer(&controller));
    VideoSession* const session = controller.session();
    session->setPaused(true);
    session->seek(1.0);
    QVERIFY(settleUntil([&] { return std::abs(session->position() - 1.0) < 0.25; }, 10000));
    QVERIFY2(settleUntil([&] { return coreIsPaused(session); }, 5000),
             "the paused seek intent did not reach the playback core");
    const auto duration = coreNumber(session, "duration");
    const auto sourceBefore = coreText(session, "filename");
    QVERIFY(duration && *duration > 10.0);
    QVERIFY(sourceBefore);

    controller.skipForward();
    const double firstEnd = settledCorePosition(session, 15000);
    QVERIFY(firstEnd >= *duration - 2.5);
    controller.skipForward();
    const double secondEnd = settledCorePosition(session, 15000);
    QVERIFY(secondEnd >= *duration - 2.5);
    QVERIFY(secondEnd >= firstEnd - 0.5);
    QVERIFY(coreIsPaused(session));
    QCOMPARE(coreText(session, "filename"), sourceBefore);
    const auto durationAfter = coreNumber(session, "duration");
    QVERIFY(durationAfter && std::abs(*durationAfter - *duration) < 0.01);
}

void TestPlaybackControls::eofEmitsFinishedOnceWithoutSteppingTheHost() {
    RecordingHost host;
    const QList<mplayer::shell::QualityEntry> qualities{
        mplayer::shell::QualityEntry{720, fixtureClip()}};
    PlayerController controller(QStringLiteral("Episode 2 · 720p"), fixtureClip(), qualities, 720,
                                &host);
    controller.setPlaylist(threeEpisodePlaylist(), 1);
    QCOMPARE(controller.currentEpisodeIndex(), 1);
    QSignalSpy finished(&controller, &PlayerController::episodePlaybackFinished);
    QVERIFY(QMetaObject::invokeMethod(controller.session(), "mediaEnded", Q_ARG(quint64, 2)));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.at(0).at(0).toInt(), 1);
    QVERIFY(host.requestedEpisodes.isEmpty());

    QVERIFY(QMetaObject::invokeMethod(controller.session(), "mediaEnded", Q_ARG(quint64, 2)));
    QCOMPARE(finished.count(), 1);
    QVERIFY(host.requestedEpisodes.isEmpty());
    controller.setPlaylist(threeEpisodePlaylist(), 2);
    QVERIFY(QMetaObject::invokeMethod(controller.session(), "mediaEnded", Q_ARG(quint64, 3)));
    QCOMPARE(finished.count(), 2);
    QCOMPARE(finished.at(1).at(0).toInt(), 2);
    QVERIFY(host.requestedEpisodes.isEmpty());
}

void TestPlaybackControls::asynchronousUnfilteredCaptureUsesTheSessionCallback() {
    if (!nativePlatformAvailable()) QSKIP("the Quick player needs a native window and GL context");
    if (!requireRuntime()) return;
    const QString fixture = fixtureClip();
    QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));

    PlayerController controller(QStringLiteral("transport"), fixture);
    QVERIFY(openPlayer(&controller));
    VideoSession* const session = controller.session();
    session->setPaused(true);
    session->seek(1.0);
    QVERIFY(settleUntil([&] { return std::abs(session->position() - 1.0) < 0.25; }, 10000));
    const auto filtersBefore = coreText(session, "vf");
    const auto shadersBefore = coreText(session, "glsl-shaders");

    QElapsedTimer callClock;
    callClock.start();
    bool completed = false;
    QImage captured;
    QString captureReason;
    session->grabUnfilteredFrameAsync([&](QImage image, QString reason) {
        captured = std::move(image);
        captureReason = std::move(reason);
        completed = true;
    });
    QVERIFY2(callClock.elapsed() < 100,
             qPrintable(QStringLiteral("async capture blocked for %1 ms").arg(callClock.elapsed())));
    QTRY_VERIFY_WITH_TIMEOUT(completed, 15000);
    QVERIFY2(captureReason.isEmpty(), qPrintable(captureReason));
    QVERIFY(!captured.isNull());
    QCOMPARE(captured.size(), QSize(320, 180));
    QVERIFY(settleUntil([&] { return std::abs(session->position() - 1.0) < 0.25; }, 10000));
    QCOMPARE(coreText(session, "vf"), filtersBefore);
    QCOMPARE(coreText(session, "glsl-shaders"), shadersBefore);
    QVERIFY(coreIsPaused(session));
}
