
#include <QApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <cmath>
#include <cstdio>
#include <functional>

#include "MpvApi.hpp"
#include "quick/PlayerController.hpp"
#include "quick/PlayerWindow.hpp"
#include "quick/PreparationWindow.hpp"
#include "../../player/core/VideoSession.hpp"
#include "ProbePicture.hpp"
#include "ProbeRuntime.hpp"

struct RefusalHost : public mplayer::shell::PlayerHost {
    int variantRequests = 0;
    int episodeRequests = 0;
    int trackProbes = 0;
    int copyRequests = 0;
    int playbackFailures = 0;
    int closed = 0;

    void requestVariant(const QString&, const QString&) override { ++variantRequests; }
    void requestEpisode(int) override { ++episodeRequests; }
    void requestTrackProbe(const QString&) override { ++trackProbes; }
    void requestCopyStreamUrl() override { ++copyRequests; }
    void playerClosed() override { ++closed; }
    void playbackFailed() override { ++playbackFailures; }
};

const QStringList& successWords() {
    static const QStringList words{QStringLiteral("Playing local file"), QStringLiteral("Episode: "),
                                   QStringLiteral("Track: "), QStringLiteral("copied")};
    return words;
}

using namespace mplayer::shell;

namespace {

class StubHost : public PlayerHost {
public:
    int closed = 0;

    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override { ++closed; }
    void playbackFailed() override {}
};

void pump(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
}

bool waitFor(const std::function<bool()>& ready, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents();
        if (ready()) return true;
        QThread::msleep(10);
    }
    return ready();
}

bool corePaused(mplayer::VideoSession* session, bool paused) {
    const auto value = session->probeMpvProperty(QStringLiteral("pause"),
                                                mplayer::ProbePropertyKind::Flag);
    return value && value->kind == mplayer::ProbePropertyValue::Kind::Flag &&
           value->flag == paused;
}

void requireHeld(mplayer::probes::Verdict& verdict, mplayer::VideoSession* session,
                 mplayer::quick::PlayerWindow* window, const QString& scenario) {
    const bool acknowledged = waitFor([session] { return corePaused(session, true); }, 2000);
    verdict.require(acknowledged, scenario + QStringLiteral(": actual core pause acknowledged"));
    QImage frame;
    int stableFrames = 0;
    for (int attempt = 0; attempt < 25 && stableFrames < 4; ++attempt) {
        pump(80);
        const QImage next = window->grabVideoFrame();
        const auto picture = mplayer::probes::measurePicture(next, next.width(), next.height());
        stableFrames = picture.litFraction() >= mplayer::probes::kMinPictureLitFraction &&
                               next == frame ? stableFrames + 1 : 0;
        frame = next;
    }
    verdict.require(stableFrames >= 4,
                    scenario + QStringLiteral(": paused Quick output settles on a picture"));
    const auto start = session->probeMpvProperty(QStringLiteral("time-pos"),
                                                 mplayer::ProbePropertyKind::Number);
    bool frozen = start && start->isNumeric();
    for (int attempt = 0; attempt < 5; ++attempt) {
        pump(80);
        const auto position = session->probeMpvProperty(
            QStringLiteral("time-pos"), mplayer::ProbePropertyKind::Number);
        frozen = frozen && position && position->isNumeric() &&
                 std::abs(position->asNumber() - start->asNumber()) < 0.000001;
    }
    verdict.require(frozen, scenario + QStringLiteral(": actual media clock stays frozen"));
}

}  // namespace

/// The loader-refusal mode. `clip` is a real local media file the player is
/// handed as direct media; the refusal comes from the runtime, never from the
/// clip. Returns the verdict's own exit code.
int runRuntimeRefusal(const QString& clip) {
    mplayer::probes::Verdict verdict;

    if (mplayer::MpvApi::instance().loaded()) {
        std::fprintf(stderr, "FAIL a module is already pinned before refusal selection\n");
        return 1;
    }
    if (mplayer::native::runtimeSelected()) {
        std::fprintf(stderr, "FAIL a runtime is already recorded as selected\n");
        return 1;
    }

    const QString missingRoot = QStringLiteral("Z:/mikai-engine-popup-probe-no-such-runtime-root");
    qputenv("MIKAI_PROBE_BUNDLE", missingRoot.toUtf8());
    const bool missingRefused = !mplayer::probes::selectRuntime();
    verdict.require(missingRefused,
                    QStringLiteral("an isolated package that is not there is refused by name"));
    verdict.require(!mplayer::MpvApi::instance().loaded(),
                    QStringLiteral("the refused isolated package pins no module"));
    qunsetenv("MIKAI_PROBE_BUNDLE");

    const QString missingModule =
        QStringLiteral("Z:/mikai-engine-popup-probe-no-such-libmpv-2.dll");
    mplayer::MpvBundle missing;
    missing.root = QStringLiteral("Z:/mikai-engine-popup-probe-no-such-runtime-root");
    missing.modulePath = missingModule;
    missing.descriptorId = QStringLiteral("engine-popup-probe-missing-module");
    missing.protocolVersion = 1;
    QString missingReason;
    const bool missingModuleRefused = !mplayer::MpvApi::instance().load(missing, &missingReason);
    verdict.require(missingModuleRefused,
                    QStringLiteral("an explicit module file that is not there is refused by name"));
    verdict.require(!missingReason.trimmed().isEmpty(),
                    QStringLiteral("the module refusal names its own reason"));
    verdict.require(!mplayer::MpvApi::instance().loaded(),
                    QStringLiteral("a refused loader leaves no core behind it"));

    RefusalHost host;
    const QList<QualityEntry> qualities = {{360, clip}};
    auto* controller = new mplayer::quick::PlayerController(
        QStringLiteral("probe 360p"), clip, qualities, 360, &host);
    int alerts = 0;
    int runtimeRequests = 0;
    QObject::connect(controller, &mplayer::quick::PlayerController::alertRequested, controller,
                     [&alerts](const QString&) { ++alerts; });
    controller->startPlayer();
    controller->showWindow();
    auto* session = controller->session();
    const bool refusalVisible = waitFor([controller] {
        return controller->errorActive() && !controller->statusText().trimmed().isEmpty();
    });
    QObject::connect(controller, &mplayer::quick::PlayerController::runtimeRequested, controller,
                     [&runtimeRequests] { ++runtimeRequests; });
    verdict.require(session != nullptr, QStringLiteral("the Quick player owns its session"));
    verdict.require(session == nullptr || !session->hasVideo(),
                    QStringLiteral("no core means hasVideo is false"));
    verdict.require(session == nullptr || !session->isPlaying(),
                    QStringLiteral("no core means nothing reports playing"));
    verdict.require(!controller->enginePreparationVisible(),
                    QStringLiteral("no operation window appears for a runtime that never loaded"));
    verdict.require(refusalVisible && controller->statusMode() ==
                        mplayer::quick::PlayerController::StatusMode::Error,
                    QStringLiteral("the unavailable runtime is readable in the Quick status row"));
    verdict.require(controller->statusText().contains(missingRoot) ||
                        controller->statusText().contains(missingModule),
                    QStringLiteral("the player preserves the loader's missing-runtime cause"));
    verdict.require(controller->isWindowVisible(),
                    QStringLiteral("the Quick player itself stays on screen after refusal"));
    verdict.require(controller->retryVisible(),
                    QStringLiteral("the failure keeps its Retry action reachable"));
    verdict.require(!controller->statusText().trimmed().isEmpty(),
                    QStringLiteral("the status row carries a named failure reason"));

    verdict.require(alerts > 0,
                    QStringLiteral("the missing runtime raises the Quick accessible alert"));
    int alertsBeforeRetry = alerts;
    controller->retryPlayback();
    verdict.require(session == nullptr || !session->hasVideo(),
                    QStringLiteral("Retry without a runtime still opens no stream"));
    verdict.require(runtimeRequests == 1,
                    QStringLiteral("Retry without a runtime asks for provisioning exactly once"));
    verdict.require(host.playbackFailures == 1,
                    QStringLiteral("Retry does not re-report a failure already reported"));
    verdict.require(controller->errorActive() && !controller->statusText().trimmed().isEmpty(),
                    QStringLiteral("the failed retry leaves the refusal readable"));
    verdict.require(alerts == alertsBeforeRetry,
                    QStringLiteral("runtime provisioning retry does not duplicate its accessible alert"));

    for (const QString& word : successWords()) {
        verdict.require(!controller->statusText().contains(word, Qt::CaseInsensitive),
                        QStringLiteral("the status never claims success: %1").arg(word));
    }
    verdict.require(controller->currentHeight() == 360,
                    QStringLiteral("the player still names the height the caller offered"));
    verdict.require(session == nullptr || !session->isPlaying(),
                    QStringLiteral("the play control never reports a running stream"));

    controller->setPlaylist({
        {{QStringLiteral("label"), QStringLiteral("Series 1")}},
        {{QStringLiteral("label"), QStringLiteral("Series 2")}},
        {{QStringLiteral("label"), QStringLiteral("Series 3")}},
    });
    controller->toggleSeries();
    if (controller->seriesMenu() != nullptr) {
        for (int index = 0; index < controller->seriesMenu()->rows().size(); ++index) {
            const auto& row = controller->seriesMenu()->rows().at(index);
            if (row.action == mplayer::quick::PlayerMenuModel::Action::SelectEpisode) {
                controller->activateMenuRow(QStringLiteral("series"), index);
                break;
            }
        }
    }
    verdict.require(host.episodeRequests == 1,
                    QStringLiteral("picking an episode asks the host exactly once"));
    const int failuresBeforeEpisode = host.playbackFailures;
    controller->applyEpisode(QStringLiteral("Series 2"), QStringLiteral("embed-2"), 1,
                             QList<QualityEntry>{{360, clip}}, {});
    pump(1000);
    verdict.require(session == nullptr || !session->hasVideo(),
                    QStringLiteral("an episode swap on a refused runtime opens no stream"));
    verdict.require(!controller->statusText().contains(QStringLiteral("Episode: ")),
                    QStringLiteral("no episode success is claimed without a core"));
    verdict.require(host.playbackFailures >= failuresBeforeEpisode,
                    QStringLiteral("the refused episode swap reports its own failure"));

    QVector<QVariantMap> variants;
    for (int index = 0; index < 2; ++index) {
        QVariantMap variant;
        variant.insert(QStringLiteral("embed"), QStringLiteral("embed-%1").arg(index + 1));
        variant.insert(QStringLiteral("label"),
                       index == 0 ? QStringLiteral("Ukrainian dub") : QStringLiteral("English dub"));
        variant.insert(QStringLiteral("kind"), QStringLiteral("voice"));
        variant.insert(QStringLiteral("kind_label"), QStringLiteral("DUB"));
        variant.insert(QStringLiteral("quality"), index == 0 ? 1080 : 0);
        variants.append(variant);
    }
    controller->setVariants(variants, QStringLiteral("embed-1"));
    const int probesBefore = host.trackProbes;
    controller->openAudioMenu();
    pump(100);
    verdict.require(controller->audioMenu() != nullptr &&
                        controller->audioMenu()->rowCount() >= 2,
                    QStringLiteral("the Quick dub picker lists published tracks"));
    verdict.require(host.trackProbes == probesBefore + 1,
                    QStringLiteral("an unmeasured track is probed through the host"));
    const int variantsBefore = host.variantRequests;
    if (controller->audioMenu() != nullptr) {
        for (int index = 0; index < controller->audioMenu()->rows().size(); ++index) {
            const auto& row = controller->audioMenu()->rows().at(index);
            if (row.action == mplayer::quick::PlayerMenuModel::Action::SelectTrack &&
                row.value == QLatin1String("embed-2")) {
                controller->activateMenuRow(QStringLiteral("audio"), index);
                break;
            }
        }
    }
    verdict.require(host.variantRequests == variantsBefore + 1,
                    QStringLiteral("picking a track asks the host for that variant once"));
    controller->applyVariant(QList<QualityEntry>{{360, clip}}, QStringLiteral("English dub"),
                             QStringLiteral("embed-2"));
    pump(1000);
    verdict.require(host.variantRequests >= variantsBefore,
                    QStringLiteral("the Quick variant swap reaches the host contract"));
    verdict.require(session == nullptr || !session->hasVideo(),
                    QStringLiteral("a variant swap on a refused runtime opens no stream"));
    verdict.require(!controller->statusText().contains(QStringLiteral("Track: ")),
                    QStringLiteral("no track success is claimed without a core"));
    verdict.require(host.playbackFailures >= failuresBeforeEpisode,
                    QStringLiteral("the refused variant swap is not silently dropped"));

    const int failuresBeforeDrop = host.playbackFailures;
    controller->handleDroppedUrls({QUrl::fromLocalFile(clip)});
    pump(1000);
    verdict.require(session == nullptr || !session->hasVideo(),
                    QStringLiteral("a dropped file loads nothing without a runtime"));
    verdict.require(!controller->statusText().contains(QStringLiteral("Playing local file")),
                    QStringLiteral("a dropped file is not announced as local playback"));
    verdict.require(controller->errorActive() &&
                        (controller->statusText().contains(missingRoot) ||
                         controller->statusText().contains(missingModule)),
                    QStringLiteral("the dropped file preserves the real runtime failure cause"));
    verdict.require(host.playbackFailures == failuresBeforeDrop,
                    QStringLiteral("a direct drop does not ask the host to re-extract"));

    const bool closeAccepted = controller->requestClose();
    verdict.require(closeAccepted && waitFor([controller] { return !controller->isWindowVisible(); }),
                    QStringLiteral("closing the refused Quick player tears down its window"));
    verdict.require(host.closed == 1,
                    QStringLiteral("the host hears the accepted close exactly once"));
    const int exitCode = verdict.report("engine_popup_runtime_refusal");
    std::fprintf(stderr,
                 "[runtime-refusal] isolatedRoot=%s missingModule=%s refusal=%d alerts=%d "
                 "host: variant=%d episode=%d trackProbe=%d copy=%d "
                 "playbackFailed=%d runtimeRequested=%d closed=%d\n",
                 qPrintable(missingRoot), qPrintable(missingModule), missingRefused ? 1 : 0,
                 alerts, host.variantRequests, host.episodeRequests, host.trackProbes,
                 host.copyRequests, host.playbackFailures, runtimeRequests, host.closed);
    return exitCode;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) return 1;
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiEnginePopupProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("EnginePopupProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());

    // The refusal mode reaches the real Quick player with NO module loaded,
    // before the normal runtime-selection guard, and still obtains the refusal
    // from the production selection helper and loader.
    //
    //   engine_popup_probe --runtime-refusal <real clip file>
    //
    // The caller must run it on a process with no mpv installed for it to find:
    // the parent launches this stage with no mpv and its own per-stage QSettings,
    // and the probe refuses an environment that has already selected a module.
    // The preparation-window mode also requires the real runtime, enforced by
    // the normal selection below.
    const QStringList arguments = QCoreApplication::arguments();
    const int refusalIndex = arguments.indexOf(QStringLiteral("--runtime-refusal"));
    if (refusalIndex >= 0) {
        if (refusalIndex + 1 >= arguments.size()) {
            std::fprintf(stderr, "usage: engine_popup_probe --runtime-refusal <real clip file>\n");
            return 2;
        }
        for (const QString& argument : arguments.mid(1)) {
            if (argument.startsWith(QLatin1Char('-')) &&
                argument != QLatin1String("--runtime-refusal")) {
                std::fprintf(stderr, "engine_popup_probe: unknown option %s\n",
                             qPrintable(argument));
                return 2;
            }
        }
        const QString refusalClip = arguments.at(refusalIndex + 1);
        // No inherited selection: an explicit module variable here would let a
        // populated stage stand in for the missing install under test.
        qunsetenv("MIKAI_PROBE_BUNDLE");
        qunsetenv("MIKAI_MPV_MODULE");
        if (mplayer::native::runtimeSelected() || mplayer::MpvApi::instance().loaded()) {
            std::fprintf(
                stderr, "FAIL --runtime-refusal requires a process with no selected module\n");
            return 2;
        }
        std::fprintf(stderr, "CLI: engine_popup_probe --runtime-refusal %s\n",
                     qPrintable(refusalClip));
        return runRuntimeRefusal(refusalClip);
    }

    if (!mplayer::probes::selectRuntime()) return 2;

    using mplayer::quick::PlayerController;
    using mplayer::quick::PreparationWindow;
    StubHost host;
    const QString url = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                 : QStringLiteral("av://lavfi:testsrc2=size=640x360:rate=25");
    const QList<QualityEntry> qualities = {{360, url}};
    auto* controller = new PlayerController(QStringLiteral("probe 360p"), url, qualities, 360,
                                            &host);
    auto* window = controller->window();
    controller->startPlayer();
    controller->showWindow();
    mplayer::probes::Verdict verdict;
    auto* session = controller->session();
    const bool loaded = window != nullptr && session != nullptr && waitFor([session] {
        return session->hasVideo() && session->position() > 0.2 && session->isPlaying();
    });
    verdict.require(loaded, QStringLiteral("real media is playing before preparation"));
    if (!loaded) {
        std::fprintf(stderr,
                     "startup: session=%d visible=%d video=%d playing=%d position=%.3f\n",
                     session != nullptr, controller->isWindowVisible(),
                     session != nullptr && session->hasVideo(),
                     session != nullptr && session->isPlaying(),
                     session != nullptr ? session->position() : -1.0);
        controller->requestClose();
        return verdict.report("engine_popup_probe");
    }

    if (controller->fullscreen()) controller->toggleFullscreen();
    verdict.require(waitFor([controller] { return !controller->fullscreen(); }),
                    QStringLiteral("windowed presentation is active before preparation"));

    PreparationWindow* preparation = nullptr;
    const auto findPreparation = [&preparation] {
        for (QWindow* candidate : QGuiApplication::topLevelWindows()) {
            if (auto* typed = qobject_cast<PreparationWindow*>(candidate)) {
                preparation = typed;
                return true;
            }
        }
        return false;
    };
    waitFor(findPreparation);
    verdict.require(preparation != nullptr && !preparation->isVisible() &&
                        !controller->enginePreparationVisible(),
                    QStringLiteral("preparation window is hidden before work starts"));
    if (preparation == nullptr) {
        controller->requestClose();
        return verdict.report("engine_popup_probe");
    }
    verdict.require(preparation != nullptr &&
                        (preparation->flags() & Qt::WindowDoesNotAcceptFocus),
                    QStringLiteral("preparation window cannot activate or steal player focus"));
    verdict.require((preparation->flags() & Qt::WindowStaysOnTopHint) &&
                        !(window->flags() & Qt::WindowStaysOnTopHint),
                    QStringLiteral("preparation alone is topmost; the player is not raised above dialogs"));

    bool cancelSeen = false;
    QObject::connect(controller, &PlayerController::enginePreparationCancelRequested, controller,
                     [&cancelSeen] { cancelSeen = true; });
    const quint64 originalGeneration = session->pauseRequestGeneration();
    controller->beginEnginePreparation(QStringLiteral("Preparing engine"));
    verdict.require(waitFor([&] {
        return preparation != nullptr && preparation->isVisible() &&
               controller->enginePreparationVisible();
    }), QStringLiteral("actual Quick preparation window presents during work"));
    verdict.require(preparation != nullptr && preparation->rootObject() != nullptr,
                    QStringLiteral("preparation status uses its registered Quick scene"));
    verdict.require(session->pauseRequestGeneration() == originalGeneration,
                    QStringLiteral("operation pause does not masquerade as viewer intent"));
    requireHeld(verdict, session, window, QStringLiteral("originally playing preparation"));
    session->setPaused(false);
    requireHeld(verdict, session, window, QStringLiteral("Play requested during preparation"));
    session->togglePause();
    session->togglePause();
    requireHeld(verdict, session, window, QStringLiteral("successive toggles retain latest Play intent"));
    controller->updateEnginePreparation(2, 3, QStringLiteral("Builder phase"),
                                        QStringLiteral("Real stage detail"));
    verdict.require(controller->preparationStageCount() == 3 &&
                        controller->preparationStageIndex() == 2,
                    QStringLiteral("Quick status reports supplied stage progress"));
    controller->endEnginePreparation(true, QStringLiteral("Complete"));
    verdict.require(waitFor([session] { return !corePaused(session, true); }),
                    QStringLiteral("success settles the preparation pause"));
    pump(1800);
    verdict.require(!preparation->isVisible() && !preparation->stackingPollActive(),
                    QStringLiteral("successful status auto-hides and stops hidden stacking work"));

    session->setPaused(true);
    verdict.require(!preparation->isVisible() && !preparation->stackingPollActive(),
                    QStringLiteral("successful status auto-hides and stops hidden stacking work"));

    // A preparation pause is independent of presentation. Exercise it while
    // fullscreen and while the same player window is in compact PiP.
    controller->toggleFullscreen();
    verdict.require(waitFor([controller] { return controller->fullscreen(); }),
                    QStringLiteral("fullscreen presentation is active before preparation"));
    controller->beginEnginePreparation(QStringLiteral("Fullscreen preparation"));
    requireHeld(verdict, session, window, QStringLiteral("fullscreen preparation"));
    controller->toggleFullscreen();
    verdict.require(waitFor([controller] { return !controller->fullscreen(); }),
                    QStringLiteral("fullscreen intent changes without replacing the player"));
    controller->endEnginePreparation(true, QStringLiteral("Complete"));
    verdict.require(waitFor([session] { return !corePaused(session, true); }),
                    QStringLiteral("fullscreen preparation restores playing intent"));

    const auto* const sessionBeforePip = session;
    const uint64_t epochBeforePip = session->sourceEpoch();
    const double positionBeforePip = session->position();
    controller->togglePip();
    verdict.require(waitFor([controller] { return controller->isPipActive(); }),
                    QStringLiteral("compact PiP presentation becomes active"));
    verdict.require(controller->session() == sessionBeforePip &&
                        session->sourceEpoch() == epochBeforePip,
                    QStringLiteral("PiP uses the same playback session and source epoch"));
    controller->beginEnginePreparation(QStringLiteral("Compact PiP preparation"));
    requireHeld(verdict, session, window, QStringLiteral("compact PiP preparation"));
    controller->endEnginePreparation(true, QStringLiteral("Complete"));
    verdict.require(waitFor([session] { return !corePaused(session, true); }),
                    QStringLiteral("compact PiP preparation restores playing intent"));
    controller->leavePip();
    verdict.require(waitFor([controller] { return !controller->isPipActive(); }),
                    QStringLiteral("the same player returns from compact PiP"));
    verdict.require(session->sourceEpoch() == epochBeforePip &&
                        session->position() >= positionBeforePip,
                    QStringLiteral("presentation transitions do not reopen or rewind media"));

    session->setPaused(true);
    waitFor([session] { return corePaused(session, true); });
    controller->beginEnginePreparation(QStringLiteral("Paused input"));
    controller->beginEnginePreparation(QStringLiteral("Updated operation"));
    requireHeld(verdict, session, window, QStringLiteral("repeated begin on paused input"));
    controller->endEnginePreparation(true, QStringLiteral("Complete"));
    verdict.require(corePaused(session, true) && !session->isPlaying(),
                    QStringLiteral("repeated begin preserves the original paused intent"));

    controller->beginEnginePreparation(QStringLiteral("Paused input with Play"));
    session->togglePause();
    requireHeld(verdict, session, window, QStringLiteral("toggle Play on originally paused input"));
    controller->endEnginePreparation(false, QStringLiteral("native.build.toolFailed"));
    verdict.require(waitFor([session] { return !corePaused(session, true); }),
                    QStringLiteral("failure honors the latest explicit Play intent"));
    verdict.require(controller->preparationSummary() == QStringLiteral("native.build.toolFailed") &&
                        preparation->isVisible(),
                    QStringLiteral("failure remains readable in the Quick preparation window"));

    controller->beginEnginePreparation(QStringLiteral("Newer Pause wins"));
    session->setPaused(false);
    session->setPaused(true);
    requireHeld(verdict, session, window, QStringLiteral("newer explicit Pause"));
    controller->endEnginePreparation(true, QStringLiteral("Complete"));
    verdict.require(corePaused(session, true) && !session->isPlaying(),
                    QStringLiteral("completion cannot override a newer explicit Pause"));

    session->setPaused(false);
    waitFor([session] { return corePaused(session, false); });
    controller->beginEnginePreparation(QStringLiteral("Cancelled operation"));
    controller->cancelEnginePreparation();
    verdict.require(cancelSeen, QStringLiteral("Quick preparation Cancel reaches its builder owner"));
    requireHeld(verdict, session, window, QStringLiteral("Cancel awaiting builder acknowledgement"));
    controller->endEnginePreparation(false, QStringLiteral("Cancelled"));
    verdict.require(waitFor([session] { return !corePaused(session, true); }),
                    QStringLiteral("acknowledged cancellation restores surviving playing intent"));
    verdict.require(controller->preparationSummary() == QStringLiteral("Cancelled"),
                    QStringLiteral("cancellation terminal detail remains readable"));

    controller->beginEnginePreparation(QStringLiteral("Source-bound operation"));
    const uint64_t oldEpoch = session->sourceEpoch();
    session->open(url);
    verdict.require(waitFor([session, oldEpoch] {
        return session->sourceEpoch() != oldEpoch && session->hasVideo();
    }), QStringLiteral("replacement loads a genuinely newer source epoch"));
    session->setPaused(true);
    controller->endEnginePreparation(true, QStringLiteral("Old source complete"));
    verdict.require(corePaused(session, true),
                    QStringLiteral("old completion never resumes replacement media"));
    session->setPaused(false);
    waitFor([session] { return corePaused(session, false); });

    // The OpenSubtitles sheet and cascading menu are both part of PlayerEntry's
    // scene, not native popups; check modal focus and Escape dismissal separately.
    controller->openSubtitleSearch();
    pump(100);
    verdict.require(controller->subtitleSearchVisible() &&
                        window->rootObject() != nullptr && window->activeFocusItem() != nullptr,
                    QStringLiteral("modal search owns focus inside the player's Quick scene"));
    verdict.require(controller->dismissOverlay() && !controller->subtitleSearchVisible(),
                    QStringLiteral("same-scene modal receives dismissal priority"));

    // Exercise the real same-scene menu route: the controller request is handled
    // by OverlayRoot in PlayerEntry, then Escape is consumed by that stack.
    int menuCloseRequests = 0;
    QObject::connect(controller, &PlayerController::menusCloseRequested, controller,
                     [&menuCloseRequests] { ++menuCloseRequests; });
    controller->openAudioMenu();
    pump(100);
    verdict.require(window->rootObject() != nullptr && controller->menuStackVisible(),
                    QStringLiteral("audio menu is presented in the player's Quick scene"));
    const bool menuDismissed = controller->dismissOverlay();
    verdict.require(menuDismissed && menuCloseRequests == 1,
                    QStringLiteral("same-scene menu focus consumes its dismissal request"));

    session->setPaused(false);
    controller->beginEnginePreparation(QStringLiteral("Close during preparation"));
    verdict.require(preparation->isVisible(),
                    QStringLiteral("preparation is visible when acknowledged close begins"));
    const bool closeAcknowledged = controller->requestClose();
    verdict.require(closeAcknowledged &&
                        waitFor([controller] { return !controller->isWindowVisible(); }),
                    QStringLiteral("acknowledged close tears down the Quick player window"));
    verdict.require(!preparation->isVisible() && !preparation->stackingPollActive(),
                    QStringLiteral("acknowledged close dismisses preparation and stops its stacking poll"));
    verdict.require(!session->hasVideo(),
                    QStringLiteral("acknowledged close closes the playback session"));
    verdict.require(host.closed == 1,
                    QStringLiteral("the host receives exactly one acknowledged close"));
    return verdict.report("engine_popup_probe");
}
