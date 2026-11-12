// GOAL-E / T-E1, T-E7: real Qt Quick source landing, reload and picture-retention evidence.
// Framebuffer reads are observations only; no paint is forced. Black-frame verdicts
// use this known-bright fixture's calibrated off-centre picture regions.
//
// Usage:
//   LoadTransitionProbe <video-file> --output-dir <absolute dir>
//                        [--delay-ms N] [--cycles N] [--no-motion] [--context-loss]

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
 #include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
 #include <QColor>
#include <QPointF>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <array>
#include <cstdio>
 #include <functional>
#include <vector>

#include "DelayedHttpSource.hpp"
#include "ProbeRuntime.hpp"
#include "PlayerController.hpp"
#include "PlayerWindow.hpp"
#include "VideoSession.hpp"
#include "MpvVideoItem.hpp"
  #include "PlayerHost.hpp"

using namespace mplayer;
using namespace mplayer::quick;

namespace {
int checks = 0;
int failures = 0;
QString outputDir;

void check(bool ok, const QString& label) {
    ++checks;
    std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", qPrintable(label));
    if (!ok) ++failures;
}

void pump(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    }
}

template <typename Ready>
bool waitFor(Ready ready, int timeoutMs, int stepMs = 16) {
    for (int waited = 0; waited < timeoutMs; waited += stepMs) {
        if (ready()) return true;
        pump(stepMs);
    }
    return ready();
}

constexpr std::array<QPointF, 6> spots{{{.10, .08}, {.16, .24}, {.30, .14},
                                         {.69, .23}, {.79, .14}, {.79, .42}}};
 struct StubHost : shell::PlayerHost {
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    void playbackFailed() override {}
};

struct Band {
    std::array<double, spots.size()> luma{};
    std::array<bool, spots.size()> bright{};
    int count = 0;
    bool present(const QImage& image) const {
        if (image.isNull()) return false;
        int judged = 0;
        for (size_t i = 0; i < spots.size(); ++i) {
            if (!bright[i]) continue;
            const QPoint p(qRound(spots[i].x() * image.width()),
                           qRound(spots[i].y() * image.height()));
            const QColor c = image.pixelColor(p);
            const double y = (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000.0;
            ++judged;
            if (y < std::max(10.0, luma[i] * .55)) return false;
        }
        return judged >= 3;
    }
    int darkRegions(const QImage& image) const {
        int mask = 0;
        if (image.isNull()) return 0;
        for (size_t i = 0; i < spots.size(); ++i) {
            if (!bright[i]) continue;
            const QPoint p(qRound(spots[i].x() * image.width()),
                           qRound(spots[i].y() * image.height()));
            const QColor c = image.pixelColor(p);
            const double y = (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000.0;
            if (y < std::max(10.0, luma[i] * .55)) mask |= 1 << i;
        }
        return mask;
    }
};

QImage readFrame(PlayerWindow* window) {
    QString reason;
    return window != nullptr ? window->grabVideoFrame(&reason) : QImage{};
}

Band calibrate(PlayerWindow* window) {
    Band band;
    std::array<std::vector<double>, spots.size()> samples;
    for (int n = 0; n < 24; ++n) {
        const QImage image = readFrame(window);
        if (!image.isNull()) {
            for (size_t i = 0; i < spots.size(); ++i) {
                const QPoint p(qRound(spots[i].x() * image.width()),
                               qRound(spots[i].y() * image.height()));
                const QColor c = image.pixelColor(p);
                samples[i].push_back((c.red() * 299 + c.green() * 587 + c.blue() * 114) /
                                     1000.0);
            }
        }
        pump(40);
    }
    for (size_t i = 0; i < spots.size(); ++i) {
        auto& values = samples[i];
        if (values.empty()) continue;
        std::sort(values.begin(), values.end());
        band.luma[i] = values[values.size() / 2];
        band.bright[i] = band.luma[i] > 20.0;
        if (band.bright[i]) ++band.count;
    }
    return band;
}

struct Transition {
    qint64 requestMs = -1;
    qint64 firstOutputMs = -1;
    quint64 epochBefore = 0;
    quint64 epochAfter = 0;
    int samples = 0;
    int blackSamples = 0;
    int darkRegionsAtOutput = 0;
    bool pictureAfter = false;
    bool sourceIdentity = false;
    bool landedAtZero = false;
    double positionAfter = -1.0;
    QJsonArray rows;
};

 Transition measure(PlayerController* controller, PlayerWindow* window, const Band& band,
                   QElapsedTimer& clock, const std::function<void()>& load,
                   const QString& expectedSource, int budgetMs) {
    Transition out;
    VideoSession* session = controller->session();
    if (session == nullptr || window == nullptr) return out;
    out.epochBefore = session->sourceEpoch();
    out.requestMs = clock.elapsed();
    load();
    const qint64 deadline = out.requestMs + budgetMs;
    while (clock.elapsed() < deadline) {
        pump(8);
        const QImage frame = readFrame(window);
        const quint64 epoch = session->sourceEpoch();
        const bool visible = session->hasVisibleOutputFrame();
        const bool picture = band.present(frame);
        ++out.samples;
        if (!picture) ++out.blackSamples;
        if (out.rows.size() < 1200) {
            out.rows.append(QJsonObject{{"runClockMs", static_cast<double>(clock.elapsed())},
                                        {"epoch", static_cast<double>(epoch)},
                                        {"opening", session->isOpening()},
                                        {"hasVideo", session->hasVideo()},
                                        {"visibleOutput", visible},
                                        {"position", session->position()},
                                        {"duration", session->duration()},
                                        {"framebufferPicture", picture},
                                        {"darkRegions", band.darkRegions(frame)}});
        }
        if (epoch != out.epochBefore && visible && picture) {
            out.epochAfter = epoch;
            out.firstOutputMs = clock.elapsed();
            out.darkRegionsAtOutput = band.darkRegions(frame);
            out.pictureAfter = true;
            out.sourceIdentity = session->sourceUrl() == expectedSource;
            out.positionAfter = session->position();
            out.landedAtZero = out.positionAfter >= 0.0 && out.positionAfter < 1.0;
            break;
        }
    }
    return out;
}

QJsonObject record(const Transition& t) {
    return QJsonObject{{"clockDomain", "run elapsed ms from probe start"},
                       {"requestMs", static_cast<double>(t.requestMs)},
                       {"firstOutputMs", static_cast<double>(t.firstOutputMs)},
                       {"gapMs", t.firstOutputMs >= 0 ? t.firstOutputMs - t.requestMs : -1},
                       {"samples", t.samples},
                       {"framebufferBlackSamples", t.blackSamples},
                       {"epochBefore", static_cast<double>(t.epochBefore)},
                       {"epochAfter", static_cast<double>(t.epochAfter)},
                       {"framebufferPictureAfter", t.pictureAfter},
                       {"sourceIdentity", t.sourceIdentity},
                       {"landedAtZero", t.landedAtZero},
                       {"positionAfter", t.positionAfter},
                       {"darkRegionsAtOutput", t.darkRegionsAtOutput},
                       {"samplesRows", t.rows}};
}

int run(int argc, char** argv) {
    QApplication app(argc, argv);
    QString video;
    int delayMs = 2500;
    int cycles = 1;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--output-dir" && i + 1 < argc) outputDir = QString::fromLocal8Bit(argv[++i]);
        else if (arg == "--delay-ms" && i + 1 < argc) delayMs = QString::fromLocal8Bit(argv[++i]).toInt();
        else if (arg == "--cycles" && i + 1 < argc) cycles = std::max(1, QString::fromLocal8Bit(argv[++i]).toInt());
        else if (arg.startsWith("--")) {
            std::fprintf(stderr, "FAIL unknown option %s\n", qPrintable(arg));
            return 3;
        } else if (video.isEmpty()) video = arg;
    }
    if (video.isEmpty() || !QFileInfo::exists(video)) {
        std::fprintf(stderr, "Usage: LoadTransitionProbe <video-file> --output-dir <absolute dir> [--delay-ms N] [--cycles N]\n");
        return 3;
    }
    constexpr qint64 maxFixtureBytes = 96LL * 1024 * 1024;
    if (QFileInfo(video).size() > maxFixtureBytes) {
        std::fprintf(stderr, "FAIL fixture exceeds the in-memory loopback origin limit\n");
        return 3;
    }
    if (!outputDir.isEmpty() && (!QFileInfo(outputDir).isAbsolute() || !QDir().mkpath(outputDir))) {
        std::fprintf(stderr, "FAIL --output-dir must be an absolute writable directory\n");
        return 3;
    }
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir settings;
    if (!settings.isValid()) return 1;
    QCoreApplication::setOrganizationName("MikaiLoadTransitionProbe");
    QCoreApplication::setApplicationName("LoadTransitionProbe");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    if (!mplayer::probes::selectRuntime()) return 2;

    QElapsedTimer clock;
    clock.start();
    const QString file = QFileInfo(video).absoluteFilePath();
    const QList<shell::QualityEntry> qualities = {{480, file}, {720, file}, {1080, file}};
    StubHost host;
    PlayerController controller(QStringLiteral("probe load transition"), file, qualities, 720,
                                &host);
    controller.startPlayer();
    PlayerWindow* window = controller.window();
    check(window != nullptr, "the controller owns the native player window");
    if (window == nullptr) return 1;
    window->showFullscreen();
    check(waitFor([window] { return window->videoItem() != nullptr; }, 10000),
          "the PlayerEntry scene owns the session's video item");
    check(waitFor([&controller] { return controller.session()->hasVisibleOutputFrame(); }, 20000),
          "the first source reaches a real output frame");
    const Band band = calibrate(window);
    check(band.count >= 3, "the outgoing known-bright fixture has calibrated picture regions");
    QJsonObject evidence{{"fixture", file}, {"delay_ms", delayMs},
                         {"observer", "overlay-free Qt Quick video-item capture; no forced paint"},
                         {"baseline", QJsonObject{{"judgedRegions", band.count}}}};

    // A paused real-frame capture is the zero-time landing baseline. This is
    // deliberately separate from live transition samples used to detect blinks.
    VideoSession* session = controller.session();
    session->setPaused(true);
    check(waitFor([session] { return session->pauseObserved(); }, 4000),
          "the source acknowledges pause before paused-frame evidence");
    pump(250);
    const QImage pausedBefore = readFrame(window);
    check(band.present(pausedBefore), "the paused source retains its known-bright picture");
    const double pausedPosition = session->position();
    session->setPaused(false);
    waitFor([session] { return !session->pauseObserved(); }, 4000);
    waitFor([session] { return session->hasVisibleOutputFrame(); }, 5000);

    QJsonArray transitions;
    const QString target = QFileInfo(file).absoluteFilePath();
    for (int cycle = 0; cycle < cycles; ++cycle) {
        mplayer::probes::DelayedHttpSource origin;
        origin.setDelayMs(delayMs);
        check(origin.serve(target), QStringLiteral("transition %1: loopback origin serves fixture").arg(cycle + 1));
        const QString slowUrl = origin.url(QStringLiteral("clip-b-%1.mp4").arg(cycle + 1));
        const Transition t = measure(&controller, window, band, clock, [&controller, slowUrl] {
            controller.applyVariant({{720, slowUrl}}, QStringLiteral("probe B"), QStringLiteral("e1"));
        }, slowUrl, 30000);
        check(t.firstOutputMs >= 0, QStringLiteral("transition %1: new source lands with real output").arg(cycle + 1));
        check(t.epochAfter != t.epochBefore, QStringLiteral("transition %1: source identity epoch advances").arg(cycle + 1));
        check(t.sourceIdentity, QStringLiteral("transition %1: output belongs to an identified source").arg(cycle + 1));
        check(t.pictureAfter, QStringLiteral("transition %1: known-bright framebuffer band is present").arg(cycle + 1));
        check(t.samples > 0 && t.blackSamples == 0,
              QStringLiteral("transition %1: no black samples during live load").arg(cycle + 1));
        QJsonObject item = record(t);
        item.insert("cycle", cycle + 1);
        item.insert("originRequests", origin.connections());
        transitions.append(item);
        check(origin.connections() > 0, QStringLiteral("transition %1: origin received the source request").arg(cycle + 1));
        if (cycle + 1 < cycles) {
            controller.applyRefreshedStreams({{720, file}}, 720);
            check(waitFor([session] { return session->hasVisibleOutputFrame(); }, 15000),
                  "local source reload lands before the next cycle");
            pump(250);
        }
    }
    evidence.insert("transitions", transitions);

    // A true stream-refresh reload also starts at zero and must advance the
    // source epoch; no seek or model substitution is used to manufacture landing.
    mplayer::probes::DelayedHttpSource expiry;
    expiry.setDelayMs(delayMs);
    check(expiry.serve(target), "stream expiry origin serves the fixture");
    const QString refreshed = expiry.url("clip-refresh.mp4");
    const Transition reload = measure(&controller, window, band, clock, [&controller, refreshed] {
        controller.applyRefreshedStreams({{720, refreshed}}, 720);
    }, refreshed, 30000);
    check(reload.firstOutputMs >= 0, "stream expiry: refreshed source lands with real output");
    check(reload.epochAfter != reload.epochBefore, "stream expiry: source epoch advances");
    check(reload.landedAtZero, "stream expiry: the replacement source lands at zero-time");
    check(reload.sourceIdentity, "stream expiry: source identity is available at landing");
    check(reload.pictureAfter, "stream expiry: framebuffer carries the bright source picture");
    check(reload.samples > 0 && reload.blackSamples == 0,
          "stream expiry: no black samples during live reload");
    evidence.insert("streamExpiry", record(reload));

    session->setPaused(true);
    waitFor([session] { return session->pauseObserved(); }, 4000);
    pump(250);
    const QImage pausedAfter = readFrame(window);
    check(band.present(pausedAfter), "paused source picture remains present after reload");
    check(pausedPosition >= 0.0, "paused-frame evidence records a valid pre-transition playhead");
    evidence.insert("pausedFrame", QJsonObject{{"positionBefore", pausedPosition},
                                               {"positionAfter", session->position()},
                                               {"picturePresent", band.present(pausedAfter)},
                                               {"pauseObserved", session->pauseObserved()}});
    if (!outputDir.isEmpty()) {
        QFile out(outputDir + QStringLiteral("/load-transition-evidence.json"));
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) check(false, "evidence file is writable");
        else out.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented));
    }
    controller.requestClose();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    std::fprintf(stderr, "RESULT: %s (%d checks, %d failed)\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) { return run(argc, argv); }
