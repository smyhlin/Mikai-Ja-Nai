#include "Anime4K.hpp"
#include "ProbeIsolation.hpp"
#include "ProbeRuntime.hpp"
#include "quick/PlayerController.hpp"
#include "quick/PlayerWindow.hpp"
#include "quick/PreparationWindow.hpp"
#include "../../player/core/VideoSession.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QMutex>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QQuickItem>
#include <QQuickWindow>
#include <QPointer>
#include <QRunnable>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVector>
#include <QtTest/QTest>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <utility>

#include "ForegroundPeer.hpp"
#include "gui/UiProfile.hpp"

#ifdef MIKAI_PROBE_HAS_APP
#include "WindowRaisePolicy.hpp"
#endif

#ifdef Q_OS_WIN
#include <dwmapi.h>
#include <windows.h>
#include <psapi.h>
#endif

namespace {

qint64 residentBytes() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<qint64>(counters.WorkingSetSize);
#endif
    return -1;
}

bool isBlack(const QImage& image) {
    if (image.isNull()) return false;
    int lit = 0;
    int count = 0;
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4) {
            const QColor color = image.pixelColor(x, y);
            lit += color.red() + color.green() + color.blue() > 24;
            ++count;
        }
    }
    return count > 0 && lit * 100 < count;
}
QString pointerId(const void* pointer) {
    return pointer == nullptr
               ? QStringLiteral("none")
               : QStringLiteral("0x%1").arg(
                     static_cast<qulonglong>(reinterpret_cast<quintptr>(pointer)), 0, 16);
}

// PNG encoding must not stall GUI sampling; the write state outlives the worker pool.
struct EvidenceWriteSnapshot {
    qint64 attempted = 0;
    qint64 completed = 0;
    qint64 failed = 0;
    qint64 skipped = 0;
    qint64 maxWriteMs = 0;
    QStringList failedArtifacts;

    qint64 pending() const { return attempted - completed - failed - skipped; }
};

class EvidenceWrites {
public:
    void noteAttempt() {
        QMutexLocker lock(&mutex_);
        ++attempted_;
    }

    void noteSkip() {
        QMutexLocker lock(&mutex_);
        ++skipped_;
    }

    void noteFinish(bool saved, qint64 elapsedMs, const QString& artifact) {
        QMutexLocker lock(&mutex_);
        maxWriteMs_ = std::max(maxWriteMs_, elapsedMs);
        if (saved) {
            ++completed_;
            return;
        }
        ++failed_;
        if (failedArtifacts_.size() < 8) failedArtifacts_.append(artifact);
    }

    qint64 maxWriteMs() {
        QMutexLocker lock(&mutex_);
        return maxWriteMs_;
    }

    qint64 failureCount() {
        QMutexLocker lock(&mutex_);
        return failed_;
    }

    EvidenceWriteSnapshot snapshot() {
        QMutexLocker lock(&mutex_);
        EvidenceWriteSnapshot copy;
        copy.attempted = attempted_;
        copy.completed = completed_;
        copy.failed = failed_;
        copy.skipped = skipped_;
        copy.maxWriteMs = maxWriteMs_;
        copy.failedArtifacts = failedArtifacts_;
        return copy;
    }

private:
    QMutex mutex_;
    qint64 attempted_ = 0;
    qint64 completed_ = 0;
    qint64 failed_ = 0;
    qint64 skipped_ = 0;
    qint64 maxWriteMs_ = 0;
    QStringList failedArtifacts_;
};

class ArtifactWriteTask final : public QRunnable {
public:
    ArtifactWriteTask(QImage image, QString path, QString artifact, EvidenceWrites* writes)
        : image_(std::move(image)), path_(std::move(path)), artifact_(std::move(artifact)),
          writes_(writes) {}

    void run() override {
        QElapsedTimer work;
        work.start();
        const bool saved = !image_.isNull() && image_.save(path_);
        writes_->noteFinish(saved, work.elapsed(), artifact_);
    }

private:
    QImage image_;
    QString path_;
    QString artifact_;
    EvidenceWrites* writes_;
};

#ifdef Q_OS_WIN
/// DWM's cumulative count of composition frames it has displayed. This is a
/// composition-side frame count, not a scanout timestamp and not a "first
/// present" clock: `cFramesDisplayed` is the documented cumulative total, while
/// `cRefreshesPresented` counts present operations (a different quantity) and
/// no DWM field reports physical presentation.
std::optional<quint64> dwmCompositionFramesDisplayed() {
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &timing))) return std::nullopt;
    return static_cast<quint64>(timing.cFramesDisplayed);
}

QJsonObject presentationMode() {
    QJsonObject info{{"directFlip", "unsupported"},
                     {"reason", "the player presents through a WGL OpenGL surface; this process "
                                "owns no DXGI swapchain to query, so DirectFlip cannot be "
                                "observed without PresentMon or a DXGI backend"}};
    BOOL composition = FALSE;
    if (SUCCEEDED(DwmIsCompositionEnabled(&composition)))
        info.insert(QStringLiteral("dwmCompositionEnabled"), composition != FALSE);
    return info;
}

/// True when any window of this process owns the native foreground. During PiP
/// the float is a separate top-level window of ours, so "not the dialog" must
/// not be read as "another application": its pixels are still our picture.
bool foregroundIsOurProcess() {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    return processId != 0 && processId == GetCurrentProcessId();
}

QString foregroundProcessImage() {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) return QStringLiteral("none");
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    if (processId == 0) return QStringLiteral("unavailable");
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) return QStringLiteral("unavailable");
    wchar_t path[1024]{};
    DWORD pathSize = 1024;
    QString name = QStringLiteral("unavailable");
    if (QueryFullProcessImageNameW(process, 0, path, &pathSize))
        name = QString::fromWCharArray(path, pathSize).section(QLatin1Char('\\'), -1);
    CloseHandle(process);
    return name;
}

/// Owns the foreground interval: Qt's activateWindow() alone is refused for a
/// process that does not already own the foreground, and a helper window from
/// another process (the parent's run saw Telegram.exe take it) would otherwise
/// silently invalidate an activation sample. Mirrors the shipping policy's
/// attach dance deliberately; these attaches are the probe's, not the app
/// policy's, and are not counted in the shipping-policy metric.
#else
std::optional<quint64> dwmCompositionFramesDisplayed() { return std::nullopt; }

QJsonObject presentationMode() {
    return QJsonObject{{"directFlip", "unsupported"},
                       {"reason", "no DirectFlip query exists for this platform's compositor"}};
}

bool foregroundIsOurProcess() {
    return QGuiApplication::applicationState() == Qt::ApplicationActive;
}

QString foregroundProcessImage() {
    return QStringLiteral("unavailable");
}

#endif

/// The shipping raise policy only exists, and only calls AttachThreadInput, on
/// Windows with the app library linked into this probe.
bool attachMetricSupported() {
#if defined(Q_OS_WIN) && defined(MIKAI_PROBE_HAS_APP)
    return true;
#else
    return false;
#endif
}

/// What the driver offers for the Quick scene's GL video renderer, measured on
/// a throwaway context before any player window is shown. A context that cannot
/// be created or is older than mpv's 2.1 requirement cannot present the scene.
/// The version and framebuffer-object support are both checked.
struct GlCapability {
    bool usable = false;
    QString version;
    bool framebuffers = false;
    QString failure;
};

GlCapability probeGlCapability() {
    QOffscreenSurface offscreen;
    offscreen.create();
    if (!offscreen.isValid()) {
        return {false, QString(), false,
                QStringLiteral("no offscreen surface for a GL probe")};
    }
    QOpenGLContext context;
    context.setFormat(offscreen.format());
    if (!context.create()) {
        return {false, QString(), false, QStringLiteral("no GL context could be created")};
    }
    if (!context.makeCurrent(&offscreen)) {
        return {false, QString(), false,
                QStringLiteral("the GL context could not be made current")};
    }
    const QSurfaceFormat format = context.format();
    const QString version =
        QStringLiteral("%1.%2").arg(format.majorVersion()).arg(format.minorVersion());
    const bool framebuffers = QOpenGLFramebufferObject::hasOpenGLFramebufferObjects();
    context.doneCurrent();
    // mpv needs at least OpenGL 2.1 (or OpenGL ES 2.0); the Quick video
    // renderer also requires framebuffer-object support.
    const bool versionOk = format.renderableType() == QSurfaceFormat::OpenGLES
                               ? format.version() >= qMakePair(2, 0)
                               : format.version() >= qMakePair(2, 1);
    if (!versionOk) {
        return {false, version, framebuffers,
                QStringLiteral("OpenGL %1 is older than the 2.1 the player needs").arg(version)};
    }
    if (!framebuffers) {
        return {false, version, framebuffers,
                QStringLiteral("OpenGL %1 has no framebuffer objects").arg(version)};
    }
    return {true, version, framebuffers, QString()};
}

void spinFor(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

double percentileMs(QVector<qint64> values, double percentile) {
    if (values.isEmpty()) return -1.0;
    std::sort(values.begin(), values.end());
    const int index = std::clamp(static_cast<int>(std::ceil(percentile * values.size())) - 1, 0,
                                 static_cast<int>(values.size()) - 1);
    return static_cast<double>(values.at(index));
}

/// One small crop of the real desktop, as the compositor shows it. Null means
/// the platform refused the grab; luma -1 means "not measured".
QImage desktopCrop(QScreen* screen, const QRect& globalRect) {
    if (screen == nullptr || globalRect.isEmpty()) return QImage();
    // grabWindow(0, ...) takes screen-relative x/y and adds the screen origin.
    const QRect local = globalRect.translated(-screen->geometry().topLeft());
    return screen->grabWindow(0, local.x(), local.y(), local.width(), local.height()).toImage();
}

double meanLuma(const QImage& image) {
    if (image.isNull()) return -1.0;
    double total = 0.0;
    int count = 0;
    for (int y = 0; y < image.height(); y += 2) {
        for (int x = 0; x < image.width(); x += 2) {
            const QColor color = image.pixelColor(x, y);
            total += 0.2126 * color.red() + 0.7152 * color.green() + 0.0722 * color.blue();
            ++count;
        }
    }
    return count > 0 ? total / count : -1.0;
}

QJsonObject surfaceState(mplayer::VideoSession* session) {
    if (session == nullptr) return QJsonObject{{"surface", "absent"}};
    const auto diagnostics = session->playbackDiagnostics();
    return QJsonObject{{"hasVideo", session->hasVideo()},
                       {"hasVisibleOutputFrame", session->hasVisibleOutputFrame()},
                       {"isPlaying", session->isPlaying()},
                       {"position", session->position()},
                       {"duration", session->duration()},
                       {"renderedOutputFrames",
                        static_cast<qint64>(diagnostics.renderedOutputFrames)},
                       {"sourceEpoch", static_cast<qint64>(session->sourceEpoch())}};
}

bool waitForVideo(mplayer::VideoSession* session, int timeoutMs) {
    if (session == nullptr) return false;
    QElapsedTimer wait;
    wait.start();
    while (!session->hasVideo() && wait.elapsed() < timeoutMs) spinFor(25);
    return session->hasVideo();
}

bool ensurePaused(mplayer::VideoSession* session, int timeoutMs, bool* corePaused) {
    if (corePaused != nullptr) *corePaused = false;
    if (session == nullptr) return false;
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < timeoutMs) {
        if (!session->isPlaying()) {
            const auto pause = session->probeMpvProperty(
                QStringLiteral("pause"), mplayer::ProbePropertyKind::Flag);
            if (pause && pause->kind == mplayer::ProbePropertyValue::Kind::Flag) {
                if (corePaused != nullptr) *corePaused = pause->flag;
                if (pause->flag) return true;
            }
        }
        session->setPaused(true);
        spinFor(120);
    }
    const auto pause = session->probeMpvProperty(
        QStringLiteral("pause"), mplayer::ProbePropertyKind::Flag);
    if (corePaused != nullptr && pause &&
        pause->kind == mplayer::ProbePropertyValue::Kind::Flag)
        *corePaused = pause->flag;
    return !session->isPlaying() && corePaused != nullptr && *corePaused;
}

bool foregroundIs(mplayer::quick::PlayerWindow* window) {
    if (window == nullptr) return false;
#ifdef Q_OS_WIN
    return GetForegroundWindow() == reinterpret_cast<HWND>(window->winId());
#else
    return window->isActive() &&
           QGuiApplication::applicationState() == Qt::ApplicationActive;
#endif
}

bool ensureForeground(mplayer::quick::PlayerWindow* window) {
    if (window == nullptr) return false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (foregroundIs(window)) return true;
        window->raiseWindow();
        window->requestActivate();
        spinFor(80);
    }
    return foregroundIs(window);
}

QRect videoSampleCrop(mplayer::quick::PlayerWindow* window) {
    if (window == nullptr || window->rootObject() == nullptr) return {};
    auto* video = window->rootObject()->findChild<QQuickItem*>(
        QStringLiteral("playerVideo"));
    if (video == nullptr || video->width() < 96 || video->height() < 64) return {};
    const QPointF scene = video->mapToScene(
        QPointF(video->width() * 0.28, video->height() * 0.33));
    const QPoint global = window->mapToGlobal(scene.toPoint());
    return QRect(global - QPoint(48, 32), QSize(96, 64));
}

QJsonObject mediaFacts(const QString& media) {
    QJsonObject facts{{"media", media}};
    if (media.contains(QLatin1String("://"))) {
        facts.insert(QStringLiteral("kind"), QStringLiteral("url"));
        facts.insert(QStringLiteral("scheme"), QUrl(media).scheme());
        facts.insert(QStringLiteral("host"), QUrl(media).host());
        return facts;
    }
    const QFileInfo info(media);
    facts.insert(QStringLiteral("kind"), QStringLiteral("file"));
    facts.insert(QStringLiteral("exists"), info.exists());
    facts.insert(QStringLiteral("bytes"), static_cast<qint64>(info.size()));
    return facts;
}

struct CanaryLogSummary {
    int paintLines = 0;
    int pacedSamples = 0;
    int unknownReasons = 0;
    int pacedWithoutDelay = 0;
    int pauseHeldLines = 0;
    int newFrameLines = 0;
    int chromeOnlyDisarms = 0;
    int frameOwningDisarms = 0;
    QVector<qint64> delays;
};

bool canaryTokenInt(const QString& line, const QString& token, qint64* value) {
    const qsizetype at = line.indexOf(token);
    if (at < 0 || value == nullptr) return false;
    qsizetype end = at + token.size();
    const qsizetype start = end;
    while (end < line.size() &&
           (line.at(end).isDigit() || (end == start && line.at(end) == QLatin1Char('-'))))
        ++end;
    bool ok = false;
    const qint64 parsed = line.mid(start, end - start).toLongLong(&ok);
    if (!ok) return false;
    *value = parsed;
    return true;
}

QString canaryTokenWord(const QString& line, const QString& token) {
    const qsizetype at = line.indexOf(token);
    if (at < 0) return {};
    qsizetype end = at + token.size();
    while (end < line.size() && !line.at(end).isSpace()) ++end;
    return line.mid(at + token.size(), end - at - token.size());
}

bool canarySkipReasonExplained(const QString& reason) {
    return reason.isEmpty() || reason == QLatin1String("none") ||
           reason == QLatin1String("paced") || reason == QLatin1String("forced") ||
           reason == QLatin1String("repeat") || reason == QLatin1String("redraw") ||
           reason == QLatin1String("elapsed") || reason == QLatin1String("no-info");
}

int runQuickHoverCanary(mplayer::quick::PlayerController* controller, int seconds,
                        const QString& outputPath,
                        const std::function<void(QJsonObject)>& record, QString* error) {
    auto* window = controller ? controller->window() : nullptr;
    auto* session = controller ? controller->session() : nullptr;
    if (!window || !session || seconds < 30 || !waitForVideo(session, 20000) ||
        !window->hasVisibleOutputFrame()) {
        *error = QStringLiteral("Quick hover canary requires a presented video for at least 30 s");
        record({{"event","hoverCanary"},{"result","INVALID"},{"reason",*error},
                {"surfaceState",surfaceState(session)}});
        return 2;
    }
    const QString frameLog=QDir(outputPath).filePath(QStringLiteral("hover-canary-frame-timing.jsonl"));
    const QString closeLog=QDir(outputPath).filePath(QStringLiteral("hover-canary-close.jsonl"));
    QFile::remove(frameLog); QFile::remove(closeLog);
    mplayer::VideoSession::setFrameTimingLogPath(frameLog);
    const quint64 epoch=session->sourceEpoch();
    const quint64 frames=session->playbackDiagnostics().renderedOutputFrames;
    const double startPosition=session->position();
    QElapsedTimer clock; clock.start();
    qint64 prev=0,maxGap=0;
    int gestures=0,ticks=0,slow=0,loopSeeks=0;
    while(clock.elapsed()<qint64(seconds)*1000) {
        const qint64 now=clock.elapsed();
        if(prev && now-prev>250) ++slow;
        if(prev) maxGap=std::max(maxGap,now-prev);
        prev=now;
        if(ticks%125==0) controller->noteUserActivity();
        if(session->duration()>3 && session->position()>session->duration()-2) {
            session->seek(0.5); ++loopSeeks;
        }
        if(window->width()>4 && window->height()>4) {
            const QPoint p((ticks%2?2:1)*window->width()/3,
                           window->height()-qBound(2,window->height()/14,28));
            QMouseEvent move(QEvent::MouseMove,QPointF(p),
                QPointF(window->mapToGlobal(p)),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
            QCoreApplication::sendEvent(window,&move); ++gestures;
        }
        spinFor(16); ++ticks;
    }
    CanaryLogSummary summary;
    QFile f(frameLog);
    if(f.open(QIODevice::ReadOnly|QIODevice::Text)) {
        qint64 previousDisarm=0;
        while(!f.atEnd()) {
            const QString line=QString::fromUtf8(f.readLine());
            if(!line.startsWith(QLatin1String("paint "))) continue;
            ++summary.paintLines;
            const QString reason=canaryTokenWord(line,QStringLiteral("skip_reason="));
            if(!canarySkipReasonExplained(reason)) ++summary.unknownReasons;
            qint64 delay=-1;
            if(canaryTokenInt(line,QStringLiteral("paint_delay_ms="),&delay)) {
                if(delay>=0) { ++summary.pacedSamples; summary.delays.append(delay); }
                else if(reason==QLatin1String("paced")) ++summary.pacedWithoutDelay;
            }
            qint64 held=0,frame=0,pending=0,forced=0,disarm=-1;
            if(canaryTokenInt(line,QStringLiteral("pause_held="),&held)&&held)
                ++summary.pauseHeldLines;
            if(canaryTokenInt(line,QStringLiteral("newframe="),&frame)&&frame)
                ++summary.newFrameLines;
            const bool owns=frame || (canaryTokenInt(line,QStringLiteral("pending="),&pending)&&pending) ||
                            (canaryTokenInt(line,QStringLiteral("forced="),&forced)&&forced);
            if(canaryTokenInt(line,QStringLiteral("disarm="),&disarm)) {
                const qint64 jump=std::max<qint64>(0,disarm-previousDisarm);
                if(owns) summary.frameOwningDisarms+=int(jump);
                else summary.chromeOnlyDisarms+=int(jump);
                previousDisarm=disarm;
            }
        }
    }
    const double p95=percentileMs(summary.delays,0.95);
    const int unexplained=summary.unknownReasons+summary.pacedWithoutDelay+summary.pauseHeldLines;
    const quint64 rendered=session->playbackDiagnostics().renderedOutputFrames-frames;
    const bool advanced=summary.newFrameLines>=seconds&&rendered>0&&session->position()>startPosition;
    const bool passed=summary.paintLines>0&&gestures>0&&advanced&&session->sourceEpoch()==epoch&&
        maxGap<=250&&slow==0&&summary.chromeOnlyDisarms==0&&summary.pacedSamples>0&&
        p95<=250.0&&unexplained==0;
    mplayer::VideoSession::setFrameTimingLogPath(closeLog);
    spinFor(150);
    controller->requestClose(); spinFor(200);
    mplayer::VideoSession::setFrameTimingLogPath(QString());
    if(!passed) *error=QStringLiteral("Quick hover canary did not satisfy playback/frame-timing criteria");
    record({{"event","hoverCanary"},{"result",passed?"PASS":"FAIL"},
        {"windowMs",clock.elapsed()},{"requestedSeconds",seconds},{"gestures",gestures},
        {"ticks",ticks},{"loopSeeks",loopSeeks},{"positionAtStart",startPosition},
        {"positionAtEnd",session->position()},{"renderedOutputFrames",static_cast<qint64>(rendered)},
        {"paintLines",summary.paintLines},{"newFramePaints",summary.newFrameLines},
        {"chromeOnlyDisarms",summary.chromeOnlyDisarms},{"frameOwningDisarms",summary.frameOwningDisarms},
        {"pacedSamples",summary.pacedSamples},{"paintDelayP95BudgetMs",250},
        {"paintDelayP95Ms",summary.pacedSamples?QJsonValue(p95):QJsonValue("unavailable")},
        {"unexplainedSkips",unexplained},{"maxGuiGapMs",maxGap},{"guiGapViolations",slow},
        {"sourceEpochStart",static_cast<qint64>(epoch)},
        {"sourceEpochEnd",static_cast<qint64>(session->sourceEpoch())},
        {"frameTimingLog",frameLog},{"criteria",QJsonObject{{"measured",summary.paintLines>0},
            {"playbackAdvanced",advanced},{"sourceStable",session->sourceEpoch()==epoch},
            {"guiGapBounded",maxGap<=250},{"chromeInteracted",gestures>0},
            {"noChromeOnlyDisarms",summary.chromeOnlyDisarms==0},
            {"skipsExplained",unexplained==0},{"paintDelayWithinBudget",
             summary.pacedSamples>0&&p95<=250.0}},
        {"surfaceAtEnd",surfaceState(session)},{"scope","real Quick QML input and live session sampling"},
        {"error",*error}});
    return passed?0:1;
}

int runQuickFocusSwitchMode(mplayer::quick::PlayerController* controller, bool knownBright,
                            int seconds, const std::function<void(QJsonObject)>& record,
                            QString* error);
int runQuickBaselineMode(mplayer::quick::PlayerController* controller, const QString& media,
                         bool knownBright, int seconds,
                         const std::function<void(QJsonObject)>& record, QString* error);
int runQuickTransitionMode(mplayer::quick::PlayerController* controller, int seconds,
                           const QString& outputPath, const QString& preset, bool knownBright,
                           bool steadyFullscreen,
                           const std::function<void(QJsonObject)>& record, QString* error);

int runQuickFocusSwitchMode(mplayer::quick::PlayerController* controller, bool knownBright,
                            int seconds, const std::function<void(QJsonObject)>& record,
                            QString* error) {
    auto* window = controller->window();
    auto* session = controller->session();
    if (!waitForVideo(session, 20000)) {
        *error = QStringLiteral("no video decoded before the focus-switch run");
        record({{"event","focusSwitchResult"},{"result","INCOMPLETE"},
                {"reason",*error},{"surfaceState",surfaceState(session)}});
        return 2;
    }
    controller->pauseIfPlaying();
    spinFor(300);
    bool corePaused=false;
    const bool paused=ensurePaused(session,5000,&corePaused);
    window->raiseWindow(); window->requestActivate();
    const bool foreground=ensureForeground(window);
    spinFor(300);
    const QImage fixture=window->grabVideoFrame();
    const double fixtureLuma=meanLuma(fixture);
    const bool bright=fixtureLuma>=40.0;
    const bool valid=paused&&corePaused&&foreground&&bright;
    const quint64 setupAttaches=mikai::gui::uiprofile::snapshot().threadInputAttaches;
    mikai::gui::uiprofile::resetCounters();
    record({{"event","focusModeStart"},{"fixtureMeanLuma",fixtureLuma},
            {"fixtureSampled",!fixture.isNull()},{"fixtureBright",bright},
            {"knownBrightFixture",knownBright},{"paused",paused},{"corePaused",corePaused},
            {"foregroundOwned",foreground},{"attachCallsBeforePhaseReset",
             static_cast<qint64>(setupAttaches)},{"profileCountersEnabled",
             mikai::gui::uiprofile::countersEnabled()},{"presentationMode",presentationMode()},
            {"surfaceState",surfaceState(session)}});
    if(!valid) {
        *error=QStringLiteral("focus mode requires a verified paused bright frame and owned foreground");
        record({{"event","focusSwitchResult"},{"result","INVALID"},{"reason",*error}});
        return 3;
    }
    const QRect crop=videoSampleCrop(window);
    mikai::probes::ForegroundPeer peer;
    QString peerError;
    if(!peer.start(QRect(crop.center()-QPoint(240,180),QSize(480,360)),&peerError)) {
        *error=peerError;
        record({{"event","focusSwitchResult"},{"result","UNAVAILABLE"},{"reason",peerError}});
        return 3;
    }
    int occlusionAttempts=0;
    double baseline=meanLuma(desktopCrop(window->screen(),crop));
    double occluded=-1;
    for(;occlusionAttempts<10;++occlusionAttempts) {
        peer.activate(); peer.raiseToFront(); spinFor(250);
        if(peer.ownsForeground()) {
            occluded=meanLuma(desktopCrop(window->screen(),crop));
            if(baseline>=40&&occluded>=0&&occluded<baseline*0.5) break;
        }
    }
    const bool peerReady=occlusionAttempts<10;
    record({{"event","foregroundPeerWarmup"},{"baselineLuma",baseline},
            {"occludedLuma",occluded},{"attempts",occlusionAttempts+1},
            {"occluded",peerReady}});
    if(!peerReady) {
        *error=QStringLiteral("the foreign helper never occluded the sample crop");
        peer.stop(); return 3;
    }
    QVector<qint64> frameLatencies;
    int lostCycles=0;
    int blackSamples=0;
    quint64 awayAttachViolations=0;
    const auto runPhase=[&](const QString& phase,int cycles) {
        for(int cycle=0;cycle<cycles;++cycle) {
            const quint64 attachBefore=mikai::gui::uiprofile::snapshot().threadInputAttaches;
            const double before=session->position();
            const bool requested=peer.activate();
            peer.raiseToFront(); spinFor(650);
            const bool away=peer.ownsForeground()&&
                QGuiApplication::applicationState()!=Qt::ApplicationActive;
            const double awayLuma=meanLuma(desktopCrop(window->screen(),crop));
            const quint64 attachWhileAway=mikai::gui::uiprofile::snapshot().threadInputAttaches-
                                          attachBefore;
            if(away&&attachWhileAway>0) awayAttachViolations+=attachWhileAway;
            bool presented=false; qint64 presentedNs=0;
            const auto connection=QObject::connect(window,
                &mplayer::quick::PlayerWindow::framePresented,window,[&]{
                    if(presented)return; presented=true;
                    presentedNs=mikai::gui::uiprofile::nowNs();
                },Qt::DirectConnection);
            const qint64 requestNs=mikai::gui::uiprofile::nowNs();
            window->raiseWindow(); window->requestActivate();
            for(int attempt=0;attempt<3&&!foregroundIs(window);++attempt) {
                spinFor(60); window->raiseWindow(); window->requestActivate();
            }
            const bool owns=foregroundIs(window);
            const qint64 ownedNs=mikai::gui::uiprofile::nowNs();
            peer.sendToBack(); window->requestRepaint();
            QElapsedTimer wait; wait.start();
            bool visible=false;
            while(owns&&wait.elapsed()<kVisibleCompositionDeadlineMs&&(!presented||!visible)) {
                if(!foregroundIs(window)) break;
                visible=meanLuma(desktopCrop(window->screen(),crop))>=std::max(30.0,baseline*0.6);
                spinFor(6);
            }
            while(!presented&&wait.elapsed()<kVisibleCompositionDeadlineMs) spinFor(5);
            QObject::disconnect(connection);
            const bool cycleValid=away&&owns&&foregroundIs(window);
            if(cycleValid&&presented) frameLatencies.append((presentedNs-ownedNs)/1000000);
            else if(!cycleValid) ++lostCycles;
            const QImage output=window->grabVideoFrame();
            const bool black=knownBright&&isBlack(output);
            if(black) ++blackSamples;
            record({{"event","focusSwitchAway"},{"phase",phase},{"cycle",cycle},
                {"foregroundOwnedInstantly",requested},{"foregroundOwnedAfterWait",away},
                {"awayQtInactive",QGuiApplication::applicationState()!=Qt::ApplicationActive},
                {"peerPid",static_cast<qint64>(peer.processId())},
                {"peerWindow",static_cast<qint64>(peer.windowId())},
                {"cropRect",QJsonArray{crop.x(),crop.y(),crop.width(),crop.height()}},
                {"foregroundProcess",foregroundProcessImage()},
                {"attachCallsWhileAway",static_cast<qint64>(attachWhileAway)},
                {"attachCallsObserved",static_cast<qint64>(
                    mikai::gui::uiprofile::snapshot().threadInputAttaches-attachBefore)},
                {"attachCallsCounted",static_cast<qint64>(away?attachWhileAway:0)},
                {"attachCounterScope","shipping WindowRaisePolicy only"},
                {"contextGeneration",static_cast<qint64>(
                    session->uiProfileCounters().contextGeneration)},
                {"initializeGlCalls",static_cast<qint64>(
                    session->uiProfileCounters().initializeGlCalls)},
                {"focusCycleKind","native foreground cycle"},
                {"focusCycleMechanism","separate helper process foreground"},
                {"desktopSampleWhilePeerForeground",away},{"peerPixelsExcluded",true},
                {"positionBefore",before}});
            record({{"event","focusSwitchBack"},{"phase",phase},{"cycle",cycle},
                {"activationToFirstSwapMs",presented&&cycleValid?
                    QJsonValue((presentedNs-ownedNs)/1000000):QJsonValue("unavailable")},
                {"paintObserved",presented},{"activationToVisibleCompositionMs",visible?
                    QJsonValue((mikai::gui::uiprofile::nowNs()-ownedNs)/1000000):
                    QJsonValue("unavailable")},{"visibleCompositionObserved",visible},
                {"visibleCompositionSupported",true},{"baselineLuma",baseline},
                {"occludedLuma",awayLuma},{"foregroundOwned",owns},
                {"foregroundOwnedAtEnd",foregroundIs(window)},
                {"foregroundLostDuringPoll",!cycleValid},{"activationRequestToOwnedMs",
                    (ownedNs-requestNs)/1000000},{"cycleValid",cycleValid},
                {"positionAfter",session->position()},
                {"positionDeltaSeconds",session->position()-before},
                {"positionAdvanced",session->position()>before},{"black",black},
                {"blackMeasured",knownBright&&!output.isNull()},{"cropLuma",meanLuma(output)},
                {"knownBrightFixture",knownBright},{"presentationMode",presentationMode()}});
        }
    };
    runPhase(QStringLiteral("plain"),std::max(1,std::min(10,seconds/3)));
    controller->beginEnginePreparation(QStringLiteral("Focus probe overlay"));
    spinFor(600);
    runPhase(QStringLiteral("engine-overlay"),std::max(1,std::min(5,seconds/6)));
    mplayer::quick::PreparationWindow* prep=nullptr;
    for(QWindow* candidate:QGuiApplication::topLevelWindows()) {
        auto* found=qobject_cast<mplayer::quick::PreparationWindow*>(candidate);
        if(found&&found->isVisible()&&found->transientParent()==window) prep=found;
    }
    const QString platform=QGuiApplication::platformName();
    const bool supported=platform==QLatin1String("windows")||platform==QLatin1String("xcb");
    const bool pollForeground=prep&&prep->isVisible()&&prep->stackingPollActive();
    const quint64 overlayAttachBefore = mikai::gui::uiprofile::snapshot().threadInputAttaches;
    const quint64 retryAbortsBefore = mikai::gui::uiprofile::snapshot().raiseRetriesAborted;
    peer.activate();
    peer.raiseToFront();
    spinFor(1400);
    const bool away=peer.ownsForeground()&&
        QGuiApplication::applicationState()!=Qt::ApplicationActive;
    const auto overlayPolicyAfter = mikai::gui::uiprofile::snapshot();
    const quint64 overlayAttachCalls =
        overlayPolicyAfter.threadInputAttaches - overlayAttachBefore;
    const quint64 retryAborts =
        overlayPolicyAfter.raiseRetriesAborted - retryAbortsBefore;
    const bool polling=prep&&prep->stackingPollActive();
    record({{"event","focusOverlayStacking"},{"pollSupported",supported},
        {"overlayVisible",prep&&prep->isVisible()},
        {"pollActiveWhileForeground",pollForeground},{"foregroundOwnedByPeer",peer.ownsForeground()},
        {"awayQtInactive",QGuiApplication::applicationState()!=Qt::ApplicationActive},
        {"pollActiveWhileAway",polling},{"attachCallsWhileAway",
            static_cast<qint64>(overlayAttachCalls)},
        {"awayStackingStops","not-applicable-QtQuick"},
        {"raiseRetriesAborted",static_cast<qint64>(retryAborts)},
        {"attachMetricSupported",attachMetricSupported()},
        {"countersEnabled",mikai::gui::uiprofile::countersEnabled()},{"awayWindowMs",1400}});
    controller->endEnginePreparation(true,QString());
    const QPoint hoverPoint(window->width()/2, window->height()-24);
    QMouseEvent hover(QEvent::MouseMove, QPointF(hoverPoint),
        QPointF(window->mapToGlobal(hoverPoint)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &hover);
    runPhase(QStringLiteral("dock-hover"), std::max(1, std::min(5, seconds/6)));
    const bool complete=!frameLatencies.isEmpty()&&lostCycles==0&&
                        (!attachMetricSupported()||awayAttachViolations==0);
    const bool pollVerified=prep&&prep->isVisible()&&pollForeground&&polling&&away&&supported&&
                            overlayAttachCalls==0;
    const bool passed=complete&&pollVerified&&(!knownBright||blackSamples==0);
    if(!passed) *error=QStringLiteral("Quick focus-switch coverage incomplete");
    record({{"event","focusSwitchResult"},{"result",!complete?"INCOMPLETE":
            passed?"PASS":"INCOMPLETE"},{"fixtureMeanLuma",fixtureLuma},
        {"fixtureSampled",!fixture.isNull()},{"knownBrightFixture",knownBright},
        {"attachCallsBeforePhaseReset",static_cast<qint64>(setupAttaches)},
        {"activationToVisibleCompositionP95LowerBoundMs",frameLatencies.isEmpty()?
            QJsonValue("unavailable"):QJsonValue(percentileMs(frameLatencies,0.95))},
        {"activationToVisibleCompositionObservedP95Ms",frameLatencies.isEmpty()?
            QJsonValue("unavailable"):QJsonValue(percentileMs(frameLatencies,0.95))},
        {"activationToVisibleCompositionSamples",frameLatencies.size()},
        {"visibleCompositionComplete",complete},{"visibleCompositionDeadlineMs",
            kVisibleCompositionDeadlineMs},{"visibleCompositionUnsupportedCycles",0},
        {"visibleCompositionTimeoutCycles",0},{"activationToFirstSwapP95Ms",
            frameLatencies.isEmpty()?QJsonValue("unavailable"):
                QJsonValue(percentileMs(frameLatencies,0.95))},
        {"activationToFirstSwapSamples",frameLatencies.size()},
        {"visibleCompositionDefinition","Quick scene frame presentation and desktop crop"},
        {"blackSamples",knownBright?blackSamples:0},{"blackSamplesMeasured",knownBright},
        {"foregroundLostCycles",lostCycles},{"attachCallsWhileAway",
            static_cast<qint64>(awayAttachViolations)},
        {"presentationMode",presentationMode()},
        {"surfaceState",surfaceState(session)},{"quickOverlayPollingVerified",pollVerified},
        {"attachCallsDuringAway",attachMetricSupported()?QJsonValue(
            static_cast<qint64>(mikai::gui::uiprofile::snapshot().threadInputAttaches)):
            QJsonValue("unavailable")},{"attachMetricSupported",attachMetricSupported()},
        {"error",*error}});
    peer.stop();
    return passed?0:3;
}

int runQuickBaselineMode(mplayer::quick::PlayerController* controller,const QString& media,
                         bool knownBright,int seconds,
                         const std::function<void(QJsonObject)>& record,QString* error) {
    auto* window=controller->window(); auto* session=controller->session();
    if(!waitForVideo(session,20000)) {
        *error=QStringLiteral("no video decoded before the baseline run");
        record({{"event","baselineResult"},{"result","INCOMPLETE"},{"reason",*error},
                {"mediaFacts",mediaFacts(media)},{"surfaceState",surfaceState(session)}});
        return 2;
    }
    controller->pauseIfPlaying(); spinFor(300);
    bool corePaused=false;
    const bool paused=ensurePaused(session,5000,&corePaused);
    window->raiseWindow(); window->requestActivate();
    const bool foreground=ensureForeground(window); spinFor(300);
    const QImage fixture=window->grabVideoFrame();
    const double luma=meanLuma(fixture);
    const bool preconditions=paused&&corePaused&&foreground&&luma>=40.0;
    const quint64 attachStart=mikai::gui::uiprofile::snapshot().threadInputAttaches;
    mikai::gui::uiprofile::resetCounters();
    record({{"event","baselineStart"},{"seconds",seconds},{"knownBrightFixture",knownBright},
        {"fixtureMeanLuma",luma},{"fixtureSampled",!fixture.isNull()},
        {"fixtureBright",luma>=40.0},{"paused",paused},{"corePaused",corePaused},
        {"foregroundOwned",foreground},{"preconditionsValid",preconditions},
        {"attachCallsBeforePhaseReset",static_cast<qint64>(attachStart)},
        {"profileCountersEnabled",mikai::gui::uiprofile::countersEnabled()},
        {"mediaFacts",mediaFacts(media)},{"surfaceState",surfaceState(session)},
        {"presentationMode",presentationMode()}});
    const int stormTarget=std::max(3000,std::min(30000,seconds*1000/2));
    int validMs=0,ticks=0,lost=0,sceneInput=0,frameEvents=0;
    const auto frameConn=QObject::connect(window,&mplayer::quick::PlayerWindow::framePresented,
        window,[&]{++frameEvents;});
    if(preconditions) {
        QElapsedTimer storm; storm.start();
        while(validMs<stormTarget&&storm.elapsed()<stormTarget*3) {
            if(!foregroundIs(window)) { ++lost; spinFor(16); continue; }
            const QPoint p((ticks%2?2:1)*window->width()/3,window->height()-24);
            QMouseEvent move(QEvent::MouseMove,QPointF(p),
                QPointF(window->mapToGlobal(p)),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
            QCoreApplication::sendEvent(window,&move); ++sceneInput;
            spinFor(16); ++ticks; validMs+=16;
        }
    }
    QObject::disconnect(frameConn);
    const bool stormValid=preconditions&&validMs>=stormTarget*9/10&&ticks>0&&
                          sceneInput>0&&!session->isPlaying();
    record({{"event","hoverStorm"},{"requestedDurationMs",stormTarget},
        {"validDurationMs",validMs},{"validTicks",ticks},{"foregroundLostTicks",lost},
        {"pausedAtEnd",!session->isPlaying()},{"valid",stormValid},
        {"quickSceneInputEvents",sceneInput},{"quickFramePresentedEvents",frameEvents},
        {"chromeDirtyRequests","not-applicable-QtQuick"},
        {"chromePaintCalls","not-applicable-QtQuick"},
        {"chromePaintDuration","not-applicable-QtQuick"}});
    mikai::probes::ForegroundPeer peer;
    QString peerError;
    const QRect crop=videoSampleCrop(window);
    bool peerReady=peer.start(QRect(crop.center()-QPoint(240,180),QSize(480,360)),&peerError);
    int activation=0,returnedFrames=0;
    quint64 awayAttachViolations=0;
    QVector<qint64> swaps;
    if(peerReady) for(int i=0;i<10;++i) {
        const quint64 attachBefore=mikai::gui::uiprofile::snapshot().threadInputAttaches;
        const bool awayNow=peer.activate(); peer.raiseToFront(); spinFor(250);
        const bool away=awayNow&&peer.ownsForeground();
        const quint64 attachWhileAway=mikai::gui::uiprofile::snapshot().threadInputAttaches-
                                      attachBefore;
        if(away&&attachWhileAway>0) awayAttachViolations+=attachWhileAway;
        bool frame=false; qint64 at=0;
        const auto connection=QObject::connect(window,
            &mplayer::quick::PlayerWindow::framePresented,window,[&]{
                if(!frame){frame=true;at=mikai::gui::uiprofile::nowNs();}
            },Qt::DirectConnection);
        const qint64 begin=mikai::gui::uiprofile::nowNs();
        window->raiseWindow();window->requestActivate();peer.sendToBack();
        QElapsedTimer wait;wait.start();
        while(!frame&&wait.elapsed()<2500) spinFor(5);
        QObject::disconnect(connection);
        const bool owns=ensureForeground(window);
        activation+=owns&&away;
        returnedFrames+=owns&&frame;
        if(owns&&frame) swaps.append((at-begin)/1000000);
        record({{"event","focusSwitchBack"},{"phase","plain"},{"cycle",i},
                {"foregroundOwnedAfterWait",away},{"foregroundOwnedAtEnd",owns},
                {"paintObserved",frame},{"positionAfter",session->position()},
                {"activationToFirstSwapMs",frame?QJsonValue((at-begin)/1000000):
                    QJsonValue("unavailable")}});
    }
    peer.stop();
    int fullscreen=0;
    for(int i=0;i<5;++i) {
        const bool wasFullscreen=window->visibility()==QWindow::FullScreen;
        const QSize beforeFullToggle=window->size();
        controller->toggleFullscreen();spinFor(700);
        const bool enteredWindowed=(window->visibility()==QWindow::FullScreen)!=wasFullscreen&&
                                   window->size()!=beforeFullToggle&&
                                   window->hasVisibleOutputFrame();
        const bool wasWindowed=window->visibility()==QWindow::FullScreen;
        const QSize beforeRestore=window->size();
        controller->toggleFullscreen();spinFor(700);
        const bool restoredFullscreen=(window->visibility()==QWindow::FullScreen)!=wasWindowed&&
                                      window->size()!=beforeRestore&&
                                      window->hasVisibleOutputFrame();
        fullscreen+=enteredWindowed&&restoredFullscreen&&
                    window->visibility()==QWindow::FullScreen;
    }
    record({{"event","fullscreenTransitions"},{"transitions",5},{"transitionsCompleted",fullscreen},
        {"layoutGeometryWrites","not-applicable-QtQuick"},
        {"quickWindowGeometry",QJsonArray{window->geometry().x(),window->geometry().y(),
                                           window->width(),window->height()}},
        {"setMaskApplications","not-applicable-QtQuick"}});
    int loaded=0,black=0,samples=0; QString loadError;
    for(int i=0;i<20;++i) {
        const quint64 epochBefore=session->sourceEpoch();
        if(!session->open(media)){loadError=QStringLiteral("open() refused at transition %1").arg(i);break;}
        QElapsedTimer wait;wait.start();
        while((!session->hasVideo()||session->sourceEpoch()==epochBefore)&&
              wait.elapsed()<5000)spinFor(20);
        QElapsedTimer interval;interval.start();
        while(interval.elapsed()<600) {
            const QImage image=window->grabVideoFrame();
            if(!image.isNull()){++samples;if(knownBright&&isBlack(image))++black;}
            spinFor(24);
        }
        if(session->hasVideo()&&session->sourceEpoch()!=epochBefore)++loaded;
        else if(loadError.isEmpty())
            loadError=QStringLiteral("source epoch did not advance at transition %1").arg(i);
    }
    record({{"event","loadfileTransitions"},{"transitionsRequested",20},
        {"transitionsCompleted",loaded},{"blackSamples",knownBright?QJsonValue(black):
            QJsonValue("not-applicable")},{"samplesMeasured",samples},
        {"sourceEpoch",static_cast<qint64>(session->sourceEpoch())},
        {"pacingDisarms",static_cast<qint64>(session->uiProfileCounters().pacingDisarms)},
        {"surfaceState",surfaceState(session)},{"loadError",loadError}});
    const bool measured=preconditions&&stormValid&&peerReady&&activation==10&&returnedFrames==10&&
                        (!attachMetricSupported()||awayAttachViolations==0)&&
                        fullscreen==5&&loaded==20&&samples>0;
    if(!measured)*error=QStringLiteral("Quick baseline measurements incomplete");
    record({{"event","baselineResult"},{"result",measured?"MEASURED":
        (!preconditions||!stormValid?"INVALID":"INCOMPLETE")},
        {"fixtureMeanLuma",luma},{"fixtureSampled",!fixture.isNull()},
        {"fixtureBright",luma>=40.0},{"knownBrightFixture",knownBright},
        {"preconditionsValid",preconditions},{"hoverStormValid",stormValid},
        {"hoverStormValidDurationMs",validMs},{"hoverStormLostTicks",lost},
        {"pacingDisarmsHoverStorm","not-applicable-QtQuick"},
        {"activationToVisibleCompositionP95LowerBoundMs","not-applicable-QtQuick"},
        {"activationToVisibleCompositionObservedP95Ms","not-applicable-QtQuick"},
        {"activationToVisibleCompositionSamples",0},{"visibleCompositionComplete",false},
        {"visibleCompositionDeadlineMs",kVisibleCompositionDeadlineMs},
        {"visibleCompositionUnsupportedCycles",0},{"visibleCompositionTimeoutCycles",0},
        {"activationToFirstSwapP95Ms",swaps.isEmpty()?QJsonValue("unavailable"):
            QJsonValue(percentileMs(swaps,0.95))},{"activationToFirstSwapSamples",swaps.size()},
        {"blackSamplesLoadfile20",knownBright?QJsonValue(black):QJsonValue("not-applicable")},
        {"loadfileSamplesMeasured",samples},{"loadfileTransitionsCompleted",loaded},
        {"attachCallsDuringAway",attachMetricSupported()?QJsonValue(
            static_cast<qint64>(mikai::gui::uiprofile::snapshot().threadInputAttaches)):
            QJsonValue("unavailable")},{"attachMetricSupported",attachMetricSupported()},
        {"activationForegroundLostCycles",10-activation},{"hoverStormForegroundLostTicks",lost},
        {"awayAttachViolations",static_cast<qint64>(awayAttachViolations)},
        {"contextGenerationChanges","not-applicable-QtQuick"},
        {"initializeGlCalls","not-applicable-QtQuick"},
        {"surfaceGeometryWritesPerFullscreenTransition","not-applicable-QtQuick"},
        {"setMaskApplicationsPerRun","not-applicable-QtQuick"},
        {"chromePaintMeanMicros","not-applicable-QtQuick"},{"chromePaintCalls","not-applicable-QtQuick"},
        {"quickSceneInputEvents",sceneInput},{"quickFramePresentedEvents",frameEvents},
        {"mediaFacts",mediaFacts(media)},{"surfaceState",surfaceState(session)},
        {"presentationMode",presentationMode()},
        {"profileCountersEnabled",mikai::gui::uiprofile::countersEnabled()},
        {"invalidReason",measured?QString():QStringLiteral("Quick baseline contract incomplete")},
        {"error",*error}});
    return measured?0:3;
}

/// Long-running QA coverage against the single Quick window/session. Black-frame
/// verdicts are enabled only for an explicitly declared known-bright workload.
int runQuickQaMatrix(mplayer::quick::PlayerController* controller, const QString& outputPath,
                     bool knownBright, int seconds,
                     const std::function<void(QJsonObject)>& record, QString* error) {
    constexpr int kChromeChangesRequired = 30;
    constexpr int kForegroundCyclesRequired = 10;
    constexpr int kMinimizeCyclesRequired = 5;
    constexpr int kFullscreenTransitionsRequired = 6;
    auto* window = controller != nullptr ? controller->window() : nullptr;
    auto* session = controller != nullptr ? controller->session() : nullptr;
    const auto fail = [&](const QString& reason, const char* result) {
        *error = reason;
        record({{"event", "qaMatrixResult"}, {"result", QString::fromLatin1(result)},
                {"reason", reason}, {"mode", "qa-matrix"}, {"requestedSeconds", seconds},
                {"knownBrightFixture", knownBright},
                {"surfaceState", session != nullptr ? surfaceState(session) : QJsonObject{}}});
        return 3;
    };
    if (window == nullptr || session == nullptr || window->rootObject() == nullptr)
        return fail(QStringLiteral("the Quick player scene or session is unavailable"), "INVALID");
    if (seconds < 300)
        return fail(QStringLiteral("the QA matrix is a 300 s long-playback matrix; requested %1 s")
                        .arg(seconds), "INVALID");
    if (!waitForVideo(session, 20000) || !session->isPlaying())
        return fail(QStringLiteral("the QA matrix requires a playing source with video"), "INVALID");

    const quint64 epochStart = session->sourceEpoch();
    const quint64 framesStart = session->playbackDiagnostics().renderedOutputFrames;
    const double positionStart = session->position();
    const auto contextStart = session->uiProfileCounters();
    const QSize initialSize = window->size();
    const QRect initialGeometry = window->geometry();
    int foregroundCycles = 0;
    int minimizeCycles = 0;
    int fullscreenTransitions = 0;
    int chromeChanges = 0;
    quint64 contextGenerationChanges = 0;
    quint64 contextRecoveryFrames = 0;
    quint64 contextRecoveryFailures = 0;
    quint64 lastContextGeneration = contextStart.contextGeneration;
    int blackArtifactsSaved = 0;
    EvidenceWrites artifactWrites;
    QThreadPool artifactPool;
    artifactPool.setMaxThreadCount(1);
    bool pipVerified = false;
    QElapsedTimer run;
    run.start();
    qint64 lastTick = 0;
    qint64 maxGapMs = 0;
    qint64 gapViolations = 0;
    qint64 brightSamples = 0;
    qint64 framebufferSamples = 0;
    qint64 framebufferUnmeasured = 0;
    qint64 desktopBrightSamples = 0;
    qint64 blackSamples = 0;
    qint64 desktopBlackSamples = 0;
    qint64 desktopSamples = 0;
    qint64 desktopUnmeasured = 0;
    qint64 sampleAttempts = 0;
    qint64 samples = 0;
    qint64 excludedMinimizedSamples = 0;
    qint64 excludedForeignForegroundSamples = 0;
    bool sourceStable = true;
    bool minimizedRestored = true;
    bool fullscreenStable = true;
    bool positionAdvancedDuringMinimize = true;
    int fullscreenFramesPresented = 0;
    int fullscreenFramesMissing = 0;

    auto* videoItem = window->rootObject()->findChild<QQuickItem*>(
        QStringLiteral("playerVideo"));
    const auto quickVideoCrop = [&]() {
        if (videoItem == nullptr || videoItem->width() < 96 || videoItem->height() < 64)
            return QRect();
        const QPointF scenePoint =
            videoItem->mapToScene(QPointF(videoItem->width() * 0.28,
                                          videoItem->height() * 0.33));
        const QPoint global = window->mapToGlobal(scenePoint.toPoint());
        return QRect(global - QPoint(48, 32), QSize(96, 64));
    };
    // Measure only a declared bright fixture; arbitrary film frames can be
    // intentionally dark and cannot support a black-flash verdict.
    const auto noteFrame = [&] {
        ++sampleAttempts;
        if (window->visibility() == QWindow::Minimized) {
            ++excludedMinimizedSamples;
            return;
        }
        if (!foregroundIsOurProcess()) {
            ++excludedForeignForegroundSamples;
            return;
        }
        ++samples;
        if (knownBright) {
            QString reason;
            const QImage frame = window->grabVideoFrame(&reason);
            if (frame.isNull()) {
                ++framebufferUnmeasured;
            } else {
                ++framebufferSamples;
                const double luma = meanLuma(frame);
                if (luma >= 40.0) ++brightSamples;
                if (isBlack(frame)) {
                    ++blackSamples;
                    if (blackArtifactsSaved < 8) {
                        const QString name = QStringLiteral("qa-matrix-black-fbo-%1.png")
                                                 .arg(run.elapsed());
                        const QString path = QDir(outputPath).filePath(name);
                        artifactWrites.noteAttempt();
                        auto* task = new ArtifactWriteTask(frame, path, name, &artifactWrites);
                        if (artifactPool.tryStart(task)) {
                            ++blackArtifactsSaved;
                        } else {
                            delete task;
                            artifactWrites.noteSkip();
                        }
                        record({{"event", "matrixBlackSample"},
                                {"elapsedMs", run.elapsed()}, {"kind", "qa-matrix-black-fbo"},
                                {"artifact", name}, {"luma", luma},
                                {"position", session->position()},
                                {"sourceEpoch", static_cast<qint64>(session->sourceEpoch())}});
                    }
                }
            }
            const QRect crop = quickVideoCrop();
            const QImage desktop = crop.isEmpty() ? QImage()
                                                  : desktopCrop(window->screen(), crop);
            if (desktop.isNull()) {
                ++desktopUnmeasured;
            } else {
                ++desktopSamples;
                const double luma = meanLuma(desktop);
                if (luma >= 40.0) ++desktopBrightSamples;
                if (isBlack(desktop)) {
                    ++desktopBlackSamples;
                    ++blackSamples;
                    if (blackArtifactsSaved < 8) {
                        const QString name = QStringLiteral("qa-matrix-black-desktop-%1.png")
                                                 .arg(run.elapsed());
                        const QString path = QDir(outputPath).filePath(name);
                        artifactWrites.noteAttempt();
                        auto* task = new ArtifactWriteTask(desktop, path, name, &artifactWrites);
                        if (artifactPool.tryStart(task)) {
                            ++blackArtifactsSaved;
                        } else {
                            delete task;
                            artifactWrites.noteSkip();
                        }
                        record({{"event", "matrixBlackSample"},
                                {"elapsedMs", run.elapsed()},
                                {"kind", "qa-matrix-black-desktop"},
                                {"artifact", name}, {"luma", luma},
                                {"position", session->position()},
                                {"sourceEpoch", static_cast<qint64>(session->sourceEpoch())}});
                    }
                }
            }
        }
    };
    QObject::connect(controller, &mplayer::quick::PlayerController::menuOpenRequested,
                     controller, [&] { ++chromeChanges; });
    QObject::connect(controller, &mplayer::quick::PlayerController::menusCloseRequested,
                     controller, [&] { ++chromeChanges; });
    QObject::connect(controller, &mplayer::quick::PlayerController::fullscreenChanged,
                     controller, [&] { ++chromeChanges; });
    QObject::connect(controller, &mplayer::quick::PlayerController::pipChanged,
                     controller, [&] { ++chromeChanges; });
    const auto pump = [&](int ms) {
        QElapsedTimer slice;
        slice.start();
        while (slice.elapsed() < ms) {
            const qint64 now = run.elapsed();
            if (lastTick > 0 && now - lastTick > 250) ++gapViolations;
            if (lastTick > 0) maxGapMs = std::max(maxGapMs, now - lastTick);
            lastTick = now;
            noteFrame();
            if (session->sourceEpoch() != epochStart) sourceStable = false;
            const quint64 generation = session->uiProfileCounters().contextGeneration;
            if (generation != lastContextGeneration) {
                ++contextGenerationChanges;
                lastContextGeneration = generation;
                const QImage recoveryFrame = window->grabVideoFrame();
                if (!recoveryFrame.isNull()) ++contextRecoveryFrames;
                else ++contextRecoveryFailures;
            }
            controller->noteUserActivity();
            spinFor(32);
        }
    };
    record({{"event", "qaMatrixCalibration"}, {"elapsedMs", run.elapsed()},
            {"knownBrightFixture", knownBright}, {"blackChecksEnabled", knownBright},
            {"sourceEpochStart", static_cast<qint64>(epochStart)},
            {"initialSize", QJsonArray{initialSize.width(), initialSize.height()}}});
    record({{"event", "qaMatrixChromeDevices"}, {"elapsedMs", run.elapsed()},
            {"quickWindow", true}, {"videoItem",
             window->rootObject()->findChild<QObject*>(QStringLiteral("playerVideo")) != nullptr},
            {"built", window->rootObject()->findChild<QObject*>(
                         QStringLiteral("playerVideo")) != nullptr}});

    // The separate-process peer and the player take turns owning the native
    // foreground; all reads during the peer interval are excluded from picture
    // accounting. We still sample the live player clock continuously.
    mikai::probes::ForegroundPeer peer;
    QString peerError;
    if (!peer.start(QRect(window->mapToGlobal(QPoint(window->width() / 2,
                                                      window->height() / 2)) -
                          QPoint(240, 180), QSize(480, 360)), &peerError))
        return fail(peerError, "UNAVAILABLE");
    int foregroundLostCycles = 0;
    int foregroundPositionAdvancedCycles = 0;
    for (int cycle = 0; cycle < kForegroundCyclesRequired; ++cycle) {
        const double before = session->position();
        peer.activate();
        peer.raiseToFront();
        pump(250);
        const bool peerOwned = peer.ownsForeground();
        if (peerOwned) ++foregroundLostCycles;
        const double whileAway = session->position();
        window->raiseWindow();
        window->requestActivate();
        pump(250);
        const bool returned = foregroundIsOurProcess();
        const bool advanced = whileAway > before;
        if (returned) ++foregroundCycles;
        if (returned && peerOwned && advanced) ++foregroundPositionAdvancedCycles;
    }
    const bool awayComplete = foregroundCycles == kForegroundCyclesRequired &&
                              foregroundLostCycles == kForegroundCyclesRequired &&
                              foregroundPositionAdvancedCycles == kForegroundCyclesRequired;
    record({{"event", "qaMatrixAwayBack"}, {"elapsedMs", run.elapsed()},
            {"nativeForegroundCycles", foregroundCycles},
            {"nativeForegroundCyclesRequired", kForegroundCyclesRequired},
            {"foregroundLostCycles", foregroundLostCycles},
            {"positionAdvancedCycles", foregroundPositionAdvancedCycles},
            {"complete", awayComplete},
            {"sameSession", controller->session() == session}});

    qint64 totalMinimizedMs = 0;
    qint64 maxMinimizedMs = 0;
    for (int cycle = 0; cycle < kMinimizeCyclesRequired; ++cycle) {
        const double before = session->position();
        QElapsedTimer minimizeClock;
        minimizeClock.start();
        window->showMinimized();
        pump(900);
        const bool observed = window->visibility() == QWindow::Minimized;
        const double positionWhileMinimized = session->position();
        window->showFullScreen();
        pump(300);
        const bool restored = window->isVisible() &&
                              window->visibility() == QWindow::FullScreen;
        const qint64 dwellMs = minimizeClock.elapsed();
        totalMinimizedMs += dwellMs;
        maxMinimizedMs = std::max(maxMinimizedMs, dwellMs);
        const bool advanced = positionWhileMinimized > before &&
                              session->position() > before;
        minimizeCycles += observed && restored && advanced;
        minimizedRestored = minimizedRestored && observed && restored;
        positionAdvancedDuringMinimize = positionAdvancedDuringMinimize && advanced;
    }
    record({{"event", "qaMatrixMinimizeSummary"}, {"cycles", kMinimizeCyclesRequired},
            {"verifiedCycles", minimizeCycles}, {"complete", minimizeCycles == kMinimizeCyclesRequired},
            {"totalMinimizedMs", totalMinimizedMs}, {"maxMinimizedDwellMs", maxMinimizedMs},
            {"maxMinimizedDwellLimitMs", 5000}, {"totalMinimizedLimitMs", 15000},
            {"minimizedSamplesExcluded", true},
            {"positionAdvancedWhileMinimized", positionAdvancedDuringMinimize}});

    // Exercise real QML menu-stack open/close requests. The signal count is the
    // observed scene transition count, not an iteration count.
    for (int cycle = 0; cycle < 15; ++cycle) {
        controller->openAudioMenu();
        pump(40);
        controller->closeMenus();
        pump(40);
    }

    for (int cycle = 0; cycle < kFullscreenTransitionsRequired; ++cycle) {
        const bool toFull = window->visibility() != QWindow::FullScreen;
        const QSize before = window->size();
        controller->toggleFullscreen();
        pump(300);
        const bool framePresented =
            window->hasVisibleOutputFrame() && !window->grabVideoFrame().isNull();
        if (framePresented) ++fullscreenFramesPresented;
        else ++fullscreenFramesMissing;
        const bool changed = (window->visibility() == QWindow::FullScreen) == toFull &&
                             window->size() != before && framePresented;
        fullscreenTransitions += changed;
        fullscreenStable = fullscreenStable && changed;
    }
    // Six transitions begin and end in fullscreen.
    pump(250);
    // PiP is the same Quick window and VideoSession. A second owner/core is an
    // invariant violation, not an alternate implementation accepted by this test.
    const double pipPosition = session->position();
    const QString pipUrl = session->sourceUrl();
    const quint64 pipEpoch = session->sourceEpoch();
    const quint64 pipPauseGeneration = session->pauseRequestGeneration();
    const double pipVolume = controller->volume();
    const bool pipMuted = controller->muted();
    const double pipRate = controller->rate();
    const auto pipTracks = session->mediaTrackSelection();
    const QString pipAnime4K = controller->anime4kChainId();
    const QString pipAnimeJanai = controller->animeJanaiPresetId();
    const auto* originalWindow = window;
    controller->togglePip();
    pump(500);
    const QRect compactGeometry = window->geometry();
    const bool compactPresentation =
        controller->isPipActive() && controller->session() == session &&
        controller->window() == originalWindow && window->compactPip() &&
        window->topmostIntent() &&
        compactGeometry.width() <= mplayer::quick::PlayerWindow::kCompactSize.width() &&
        compactGeometry.height() <= mplayer::quick::PlayerWindow::kCompactSize.height() &&
        session->position() > pipPosition && window->hasVisibleOutputFrame() &&
        !window->grabVideoFrame().isNull() && session->sourceUrl() == pipUrl &&
        session->sourceEpoch() == pipEpoch &&
        session->pauseRequestGeneration() == pipPauseGeneration &&
        controller->volume() == pipVolume && controller->muted() == pipMuted &&
        controller->rate() == pipRate && controller->anime4kChainId() == pipAnime4K &&
        controller->animeJanaiPresetId() == pipAnimeJanai &&
        session->mediaTrackSelection() == pipTracks;

    // Verify that the compact QML controls are actually present and route a
    // scene click through play/pause before testing controller transport APIs.
    // Direct controller calls alone cannot catch a missing/unbound control UI.
    auto* pipPlayPause = window->rootObject()->findChild<QQuickItem*>(
        QStringLiteral("player.pip.playPause"));
    auto* pipVolumeControl = window->rootObject()->findChild<QQuickItem*>(
        QStringLiteral("player.pip.volume"));
    auto* pipTimeline = window->rootObject()->findChild<QQuickItem*>(
        QStringLiteral("player.pip.timeline.scrub"));
    const bool pipControlSurfaceVisible =
        pipPlayPause != nullptr && pipPlayPause->isVisible() && pipPlayPause->isEnabled() &&
        pipVolumeControl != nullptr && pipVolumeControl->isVisible() &&
        pipTimeline != nullptr && pipTimeline->isVisible();
    bool pipPlayPauseUiVerified = false;
    bool pipControls = false;
    if (compactPresentation && pipControlSurfaceVisible) {
        const double start = session->position();
        const double target = std::min(session->duration() - 1.0, start + 2.0);
        const QPoint playPausePoint =
            pipPlayPause->mapToScene(pipPlayPause->boundingRect().center()).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, playPausePoint);
        pump(100);
        const bool paused = session->playbackPauseIntent();
        pipPlayPauseUiVerified = paused;
        controller->seekTo(target);
        pump(250);
        const bool absoluteSeek = std::abs(session->position() - target) < 2.0;
        controller->seekBy(-1.0);
        pump(250);
        const bool relativeSeek = session->position() < target;
        controller->setVolume(31.0);
        controller->toggleMute();
        controller->setRate(1.25);
        pump(150);
        const bool audioControls = std::abs(controller->volume() - 31.0) < 0.01 &&
                                   controller->muted() != pipMuted &&
                                   std::abs(controller->rate() - 1.25) < 0.01;
        controller->setVolume(pipVolume);
        if (controller->muted() != pipMuted) controller->toggleMute();
        controller->setRate(pipRate);
        controller->togglePlayPause();
        pump(250);
        pipControls = paused && absoluteSeek && relativeSeek && audioControls &&
                      !session->playbackPauseIntent() && session->isPlaying();
⟧
    }
    const quint64 returnPauseGeneration = session->pauseRequestGeneration();
    const double returnPosition = session->position();
    const bool returnPauseIntent = session->playbackPauseIntent();
    controller->togglePip();
    pump(500);
    const bool returned = !controller->isPipActive() && controller->window() == originalWindow &&
                          !window->compactPip() && !window->topmostIntent() &&
                          window->isVisible() &&
                          window->visibility() == QWindow::FullScreen &&
                          window->hasVisibleOutputFrame() && !window->grabVideoFrame().isNull() &&
                          session->sourceUrl() == pipUrl && session->sourceEpoch() == pipEpoch &&
                          session->pauseRequestGeneration() == returnPauseGeneration &&
                          session->playbackPauseIntent() == returnPauseIntent &&
                          std::abs(session->position() - returnPosition) < 1.0 &&
                          controller->volume() == pipVolume &&
                          controller->muted() == pipMuted &&
                          std::abs(controller->rate() - pipRate) < 0.01 &&
                          controller->anime4kChainId() == pipAnime4K &&
                          controller->animeJanaiPresetId() == pipAnimeJanai &&
                          session->mediaTrackSelection() == pipTracks;
⟧
    pipVerified = compactPresentation && pipControlSurfaceVisible &&
                  pipPlayPauseUiVerified && pipControls && returned;
    record({{"event", "qaMatrixPresentationSeams"}, {"fullscreenTransitions", fullscreenTransitions},
            {"fullscreenTransitionsRequired", kFullscreenTransitionsRequired},
            {"fullscreenFramesPresented", fullscreenFramesPresented},
            {"fullscreenFramesMissing", fullscreenFramesMissing},
            {"pipVerified", pipVerified}, {"pipControlsVerified", pipControls},
            {"pipControlSurfaceVisible", pipControlSurfaceVisible},
            {"pipPlayPauseUiVerified", pipPlayPauseUiVerified},
            {"pipCompactPresentationVerified", compactPresentation},
            {"pipReturnVerified", returned}, {"pipCompactGeometry",
             QJsonArray{compactGeometry.x(), compactGeometry.y(),
                        compactGeometry.width(), compactGeometry.height()}},
            {"sameWindowAndSession", true}, {"sameSourceUrl", session->sourceUrl() == pipUrl},
            {"sourceEpoch", static_cast<qint64>(session->sourceEpoch())},
            {"layoutGeometryWrites", "not-applicable-QtQuick"}});

    const qint64 deadline = qint64(seconds) * 1000;
    const qint64 idleStart = run.elapsed();
    const qint64 idleExposedStart = samples;
    while (run.elapsed() < deadline) pump(32);
    const qint64 idleMs = run.elapsed() - idleStart;
    const qint64 idleExposed = samples - idleExposedStart;
    const bool idleCadenceSatisfied =
        idleMs < 2000 || idleExposed * 10 >= (idleMs / 32) * 9;
    const double positionEnd = session->position();
    const quint64 framesEnd = session->playbackDiagnostics().renderedOutputFrames;
    const bool mediaAdvanced = positionEnd - positionStart >= 0.85 * (run.elapsed() / 1000.0);
    const bool motion = framesEnd > framesStart && positionEnd > positionStart;
    const bool sampleCoverage = samples > 0 &&
                                (!knownBright ||
                                 (framebufferSamples > 0 && framebufferUnmeasured == 0 &&
                                  desktopSamples > 0 && desktopUnmeasured == 0));
    const bool blackClean =
        !knownBright || (brightSamples > 0 && desktopBrightSamples > 0 && blackSamples == 0);
    const auto contextEnd = session->uiProfileCounters();
    const bool contextContinuous =
        contextEnd.contextGeneration >= contextStart.contextGeneration &&
        contextRecoveryFrames == contextGenerationChanges && contextRecoveryFailures == 0 &&
        window->hasVisibleOutputFrame();
    const bool chromeCovered = chromeChanges >= kChromeChangesRequired;
    const bool samplingCadenceSatisfied = sampleAttempts >= run.elapsed() / 36 &&
                                          idleCadenceSatisfied;
    QElapsedTimer artifactSettle;
    artifactSettle.start();
    const bool artifactWritesSettled = artifactPool.waitForDone(15000);
    const EvidenceWriteSnapshot artifactSnapshot = artifactWrites.snapshot();
    const bool passed = run.elapsed() >= deadline && mediaAdvanced && motion &&
                        sampleCoverage && samplingCadenceSatisfied && blackClean && sourceStable &&
                        awayComplete && minimizedRestored && positionAdvancedDuringMinimize &&
                        maxMinimizedMs <= 5000 && totalMinimizedMs <= 15000 &&
                        fullscreenStable && pipVerified && contextContinuous &&
                        gapViolations == 0 && chromeCovered && idleCadenceSatisfied &&
                        foregroundCycles == kForegroundCyclesRequired &&
                        minimizeCycles == kMinimizeCyclesRequired &&
                        fullscreenTransitions == kFullscreenTransitionsRequired;
    const QString result = passed ? QStringLiteral("PASS") : QStringLiteral("FAIL");
    if (!passed)
        *error = QStringLiteral("QA matrix %1: playback=%2 source=%3 foreground=%4/%5 "
                                "minimize=%6/%7 fullscreen=%8/%9 pip=%10 black=%11 gap=%12")
                     .arg(result).arg(mediaAdvanced).arg(sourceStable)
                     .arg(foregroundCycles).arg(kForegroundCyclesRequired)
                     .arg(minimizeCycles).arg(kMinimizeCyclesRequired)
                     .arg(fullscreenTransitions).arg(kFullscreenTransitionsRequired)
                     .arg(pipVerified).arg(blackSamples).arg(maxGapMs);
    record({{"event", "qaMatrixResult"}, {"result", result}, {"mode", "qa-matrix"},
            {"requestedSeconds", seconds}, {"elapsedMs", run.elapsed()},
            {"knownBrightFixture", knownBright}, {"blackChecksEnabled", knownBright},
            {"blackSamplesStrictWhileVisible", blackSamples},
            {"blackFramebufferSamples", blackSamples - desktopBlackSamples},
            {"blackDesktopSamples", desktopBlackSamples},
            {"framebufferSamples", framebufferSamples},
            {"framebufferUnmeasuredSamples", framebufferUnmeasured},
            {"brightFramebufferSamples", brightSamples},
            {"brightDesktopSamples", desktopBrightSamples},
            {"desktopSamples", desktopSamples},
            {"desktopUnmeasuredSamples", desktopUnmeasured},
            {"positionStart", positionStart}, {"positionEnd", positionEnd},
            {"positionAdvanceSeconds", positionEnd - positionStart},
            {"renderedOutputFramesStart", static_cast<qint64>(framesStart)},
            {"renderedOutputFramesEnd", static_cast<qint64>(framesEnd)},
            {"nativeForegroundCycles", foregroundCycles},
            {"foregroundLostCycles", foregroundLostCycles},
            {"positionAdvancedCycles", foregroundPositionAdvancedCycles},
            {"awayBackComplete", awayComplete},
            {"minimizeRestoreCyclesVerified", minimizeCycles},
            {"totalMinimizedMs", totalMinimizedMs},
            {"maxMinimizedDwellMs", maxMinimizedMs},
            {"fullscreenSeamTransitions", fullscreenTransitions},
            {"fullscreenFramesPresented", fullscreenFramesPresented},
            {"fullscreenFramesMissing", fullscreenFramesMissing},
            {"pipVerified", pipVerified},
            {"contextGenerationChanges", static_cast<qint64>(contextGenerationChanges)},
            {"contextRecoveryFrames", static_cast<qint64>(contextRecoveryFrames)},
            {"contextRecoveryFailures", static_cast<qint64>(contextRecoveryFailures)},
            {"contextContinuityVerified", contextContinuous},
            {"contextRecreatedAnywhere", contextGenerationChanges > 0},
            {"excludedMinimizedSamples", excludedMinimizedSamples},
            {"excludedForeignForegroundSamples", excludedForeignForegroundSamples},
            {"sampleAttempts", sampleAttempts}, {"idleSamplingMs", idleMs},
            {"idleSamplingSamples", idleExposed},
            {"idleSamplingExpected", idleMs / 32},
            {"idleCadenceSatisfied", idleCadenceSatisfied},
            {"samplingCoverageComplete", samplingCadenceSatisfied},
            {"artifactWritesAttempted", artifactSnapshot.attempted},
            {"artifactWritesCompleted", artifactSnapshot.completed},
            {"artifactWritesFailed", artifactSnapshot.failed},
            {"artifactWritesSkipped", artifactSnapshot.skipped},
            {"artifactWritesSettled", artifactWritesSettled},
            {"artifactSettleMs", artifactSettle.elapsed()},
            {"sourceEpochStart", static_cast<qint64>(epochStart)},
            {"sourceEpochEnd", static_cast<qint64>(session->sourceEpoch())},
            {"sourceEpochStable", sourceStable},
            {"contextGenerationStart", static_cast<qint64>(contextStart.contextGeneration)},
            {"contextGenerationEnd", static_cast<qint64>(contextEnd.contextGeneration)},
            {"maxGuiGapMs", maxGapMs}, {"guiGapViolations", gapViolations},
            {"exposedSamples", samples}, {"blackCleanWhileVisible", blackClean},
            {"chromeStateChanges", chromeChanges}, {"chromeSatisfied", chromeCovered},
            {"sameWindowAndSession", true}, {"initialGeometry",
                 QJsonArray{initialGeometry.x(), initialGeometry.y(),
                            initialGeometry.width(), initialGeometry.height()}},
            {"surfaceState", surfaceState(session)}, {"error", *error}});
    peer.stop();
    return passed ? 0 : 3;
}

int runQuickTransitionMode(mplayer::quick::PlayerController* controller, int seconds,
                           const QString& outputPath, const QString& preset, bool knownBright,
                           bool steadyFullscreen,
                           const std::function<void(QJsonObject)>& record, QString* error) {
    auto* window=controller->window(); auto* session=controller->session();
    QElapsedTimer clock; clock.start();
    quint64 frameEvents=0,black=0,desktopBlack=0,desktopSamples=0,sampleTicks=0;
    quint64 missing=0,validMain=0,validPip=0,stalled=0,fullscreenViolations=0;
    quint64 previousFrames=session->playbackDiagnostics().renderedOutputFrames;
    double previousPosition=session->position();
    quint64 sourceEpoch=session->sourceEpoch();
    quint64 context=session->uiProfileCounters().contextGeneration;
    quint64 sourceChanges=0,contextChanges=0;
    qint64 previousTick=0,maxGap=0,lastSummary=-1000,nextTransition=30000;
    qint64 lastProgressMs=0,lastStallAtMs=0,previousSampleWorkMs=0;
    qint64 maxSampleWorkMs=0,maxReadbackMs=0,maxDesktopGrabMs=0;
    EvidenceWrites artifactWrites;
    QThreadPool artifactPool;
    artifactPool.setMaxThreadCount(1);
    const auto saveImage=[&](const QImage& image,const QString& name) {
        const QString path=QDir(outputPath).filePath(name);
        artifactWrites.noteAttempt();
        auto* task=new ArtifactWriteTask(image,path,name,&artifactWrites);
        if(!artifactPool.tryStart(task)) {
            delete task;
            artifactWrites.noteSkip();
        }
    };
    int phase=0,pipTicks=0,completedPip=0;
    QString mediaError;
    QObject::connect(window,&mplayer::quick::PlayerWindow::framePresented,controller,
                     [&]{++frameEvents;});
    QObject::connect(session,&mplayer::VideoSession::errorOccurred,controller,
                     [&](const QString& text){mediaError=text;});
    record({{"event","runMode"},{"mode",steadyFullscreen?"steady-fullscreen":"transition"},
            {"durationSeconds",seconds},{"knownBrightFixture",knownBright}});
    if(QScreen* screen=window->screen()) {
        record({{"event","initialScreen"},{"screen",screen->name()},
                {"width",screen->geometry().width()},{"height",screen->geometry().height()},
                {"dpr",screen->devicePixelRatio()}});
        QObject::connect(screen,&QScreen::geometryChanged,controller,[&](const QRect& g){
            record({{"event","screenGeometryChanged"},{"elapsedMs",clock.elapsed()},
                    {"width",g.width()},{"height",g.height()}});
        });
    }
    while(clock.elapsed()<qint64(seconds)*1000&&mediaError.isEmpty()) {
        QElapsedTimer sampleWork;
        sampleWork.start();
        const qint64 now=clock.elapsed();
        if(!steadyFullscreen&&now>=nextTransition) {
            if(phase==0){controller->toggleFullscreen();phase=1;nextTransition=now+4000;}
            else if(phase==1){controller->toggleFullscreen();phase=2;nextTransition=now+26000;}
            else if(phase==2){controller->togglePip();phase=3;pipTicks=0;nextTransition=now+5000;}
            else {
                if(pipTicks<2) mediaError=QStringLiteral("PiP did not produce two advancing playback samples");
                ++completedPip;controller->togglePip();phase=0;nextTransition=now+30000;
            }
            record({{"event","presentationTransition"},{"elapsedMs",now},{"phase",phase},
                {"windowVisible",window->isVisible()},{"fullscreen",window->fullscreenIntent()},
                {"pipActive",controller->isPipActive()},{"hasVideo",session->hasVideo()},
                {"activePosition",session->position()},{"activePlaying",session->isPlaying()},
                {"activeFrames",static_cast<qint64>(session->playbackDiagnostics().renderedOutputFrames)},
                {"sameWindowAndSession",true}});
        }
        const qint64 gap=previousTick?now-previousTick:0;
        if(previousTick) {
            maxGap=std::max(maxGap,gap);
            if(now>5000&&gap>250)
                record({{"event","guiGap"},{"elapsedMs",now},{"gapMs",gap},
                        {"previousSampleWorkMs",previousSampleWorkMs},
                        {"maxReadbackMs",maxReadbackMs},
                        {"maxDesktopGrabMs",maxDesktopGrabMs},
                        {"maxImageSaveMs",artifactWrites.maxWriteMs()}});
        }
        previousTick=now;
        const bool foreground=foregroundIs(window)&&
            QGuiApplication::applicationState()==Qt::ApplicationActive;
        if(steadyFullscreen&&now>5000&&!foreground) {
            mediaError=QStringLiteral("Steady fullscreen is no longer foreground");
            record({{"event","foregroundLost"},{"elapsedMs",now},
                    {"foregroundProcess",foregroundProcessImage()}});
        }
        const quint64 frames=session->playbackDiagnostics().renderedOutputFrames;
        const double position=session->position();
        if(now>5000&&session->hasVideo()) {
            ++sampleTicks;
            QElapsedTimer readback;
            readback.start();
            const QImage frame=window->grabVideoFrame();
            maxReadbackMs=std::max(maxReadbackMs,readback.elapsed());
            if(frame.isNull())++missing;
            else {
                if(knownBright) {
                    if(isBlack(frame)) {
                        ++black;
                        if(black<=8) saveImage(frame,
                            QStringLiteral("black-composition-%1.png").arg(now));
                    } else if(frameEvents==1) {
                        saveImage(frame,QStringLiteral("composed-video.png"));
                    }
                    const QRect crop=videoSampleCrop(window);
                    QElapsedTimer desktopGrab;
                    desktopGrab.start();
                    const QImage desktop=crop.isEmpty()?QImage():
                        desktopCrop(window->screen(),crop);
                    maxDesktopGrabMs=std::max(maxDesktopGrabMs,desktopGrab.elapsed());
                    if(desktop.isNull()) ++missing;
                    else {
                        ++desktopSamples;
                        if(isBlack(desktop)) {
                            ++desktopBlack;
                            if(desktopBlack<=8)
                                saveImage(window->screen()->grabWindow(0).toImage(),
                                    QStringLiteral("black-%1.png").arg(now));
                        }
                    }
                }
                if(controller->isPipActive())++pipTicks;
            }
            if(frames>previousFrames&&position>previousPosition) {
                if(controller->isPipActive())++validPip;else ++validMain;
                lastProgressMs=now;
            } else if(now-lastProgressMs>1500&&now-lastStallAtMs>=1000) {
                ++stalled;
                lastStallAtMs=now;
            }
        }
        const quint64 epoch=session->sourceEpoch();
        const quint64 generation=session->uiProfileCounters().contextGeneration;
        if(epoch!=sourceEpoch){++sourceChanges;sourceEpoch=epoch;}
        if(generation!=context){++contextChanges;context=generation;}
        previousFrames=frames;previousPosition=position;
        if(now-lastSummary>=1000) {
            if(steadyFullscreen&&window->visibility()!=QWindow::FullScreen)
                ++fullscreenViolations;
            record({{"event","sample"},{"elapsedMs",now},{"position",position},
                {"frames",static_cast<qint64>(frames)},{"mainPosition",position},
                {"mainFrames",static_cast<qint64>(frames)},{"mainPlaying",session->isPlaying()},
                {"activeSurfaceFound",true},{"activeSurfaceIsPip",controller->isPipActive()},
                {"activePlaying",session->isPlaying()},{"activeHasVideo",session->hasVideo()},
                {"activePosition",position},{"activeFrames",static_cast<qint64>(frames)},
                {"mode",steadyFullscreen?"steady-fullscreen":"transition"},
                {"steadyFullscreen",steadyFullscreen},{"activeSurfaceId",pointerId(session)},
                {"activeGlContextId",QStringLiteral("QuickContext-%1").arg(generation)},
                {"activeSourceEpoch",static_cast<qint64>(epoch)},
                {"contextIdentityChanges",static_cast<qint64>(contextChanges)},
                {"sourceEpochChanges",static_cast<qint64>(sourceChanges)},
                {"fullscreenViolations",static_cast<qint64>(fullscreenViolations)},
                {"swaps",static_cast<qint64>(frameEvents)},{"maxGuiGapMs",maxGap},
                {"applicationActive",QGuiApplication::applicationState()==Qt::ApplicationActive},
                {"windowActive",window->isActive()},{"windowFocused",window->isActive()},
                {"pipActive",controller->isPipActive()},{"knownBrightFixture",knownBright},
                {"blackSamples",static_cast<qint64>(knownBright?desktopBlack:0)},
                {"compositions",static_cast<qint64>(frameEvents)},
                {"blackCompositions",static_cast<qint64>(knownBright?black:0)},
                {"missingOwnerSamples",static_cast<qint64>(missing)},
                {"desktopSamples",static_cast<qint64>(desktopSamples)},
                {"validMainSamples",static_cast<qint64>(validMain)},
                {"validPipSamples",static_cast<qint64>(validPip)},
                {"inactiveSamples",static_cast<qint64>(!foreground)},
                {"foreground",foreground},{"residentBytes",residentBytes()},
                {"playing",session->isPlaying()},{"fullscreen",window->fullscreenIntent()},
                {"error",mediaError}});
            lastSummary=now;
        }
        if(now>=60000&&now%60000<32&&window->screen()!=nullptr)
            saveImage(window->screen()->grabWindow(0).toImage(),
                      QStringLiteral("fullscreen-%1.png").arg(now));
        previousSampleWorkMs=sampleWork.elapsed();
        maxSampleWorkMs=std::max(maxSampleWorkMs,previousSampleWorkMs);
        spinFor(32);
    }
    const auto coreShaders=session->probeMpvProperty(
        QStringLiteral("glsl-shaders"),mplayer::ProbePropertyKind::Text);
    const auto coreTransfer=session->probeMpvProperty(
        QStringLiteral("video-params/gamma"),mplayer::ProbePropertyKind::Text);
    const QString expectedShaders=mplayer::anime4k::shaderStringForCandidate(preset);
    const bool shaderFilesComplete=preset==QLatin1String("off")||
        mplayer::anime4k::chainIsComplete(mplayer::anime4k::candidateForId(preset));
    const bool shaderStateVerified=coreShaders&&
        coreShaders->kind==mplayer::ProbePropertyValue::Kind::Text&&
        coreShaders->text==expectedShaders&&shaderFilesComplete;
    if(!shaderStateVerified&&mediaError.isEmpty())
        mediaError=QStringLiteral("The core shader list does not match preset '%1'").arg(preset);
    QElapsedTimer artifactSettle;
    artifactSettle.start();
    const bool artifactWritesSettled=artifactPool.waitForDone(15000);
    const EvidenceWriteSnapshot artifactSnapshot=artifactWrites.snapshot();
    if(!artifactWritesSettled&&mediaError.isEmpty())
        mediaError=QStringLiteral("Evidence artifacts were still being written after 15s");
    if(artifactSnapshot.failed>0&&mediaError.isEmpty())
        mediaError=QStringLiteral("Could not save evidence: %1")
            .arg(artifactSnapshot.failedArtifacts.join(QStringLiteral(", ")));
    const bool passed=mediaError.isEmpty()&&session->hasVideo()&&
        session->position()>=seconds-5&&frameEvents>0&&
        (!knownBright||(black==0&&desktopBlack==0))&&
        stalled==0&&maxGap<1000&&window->fullscreenIntent()&&
        session->anime4KChainId()==preset&&validMain>=2&&
        contextChanges==0&&sourceChanges==0&&
        (!steadyFullscreen||(fullscreenViolations==0&&missing==0&&
                             (!knownBright||desktopSamples>0)))&&
        (steadyFullscreen?foregroundIs(window):
         (seconds<66||(completedPip>0&&validPip>=2)));
    if(!passed&&error->isEmpty())
        *error=mediaError.isEmpty()?QStringLiteral("Quick fullscreen transition invariants failed"):
                                    mediaError;
    record({{"result",passed?"PASS":"FAIL"},{"elapsedMs",clock.elapsed()},
        {"requestedPreset",preset},{"appliedPreset",session->anime4KChainId()},
        {"coreGlslShadersAvailable",coreShaders.has_value()},
        {"coreGlslShaders",coreShaders?coreShaders->text:QString()},
        {"expectedGlslShaders",expectedShaders},{"shaderFilesComplete",shaderFilesComplete},
        {"shaderStateVerified",shaderStateVerified},
        {"coreSourceTransferAvailable",coreTransfer.has_value()},
        {"coreSourceTransfer",coreTransfer?coreTransfer->text:QString()},
        {"activeOwnerFoundAtFinish",true},{"activeOwnerIsPipAtFinish",controller->isPipActive()},
        {"activePositionAtFinish",session->position()},
        {"mode",steadyFullscreen?"steady-fullscreen":"transition"},
        {"steadyFullscreen",steadyFullscreen},{"finalSurfaceId",pointerId(session)},
        {"finalGlContextId",QStringLiteral("QuickContext-%1").arg(context)},
        {"finalSourceEpoch",static_cast<qint64>(session->sourceEpoch())},
        {"ownerTransitions",0},{"contextIdentityChanges",static_cast<qint64>(contextChanges)},
        {"sourceEpochChanges",static_cast<qint64>(sourceChanges)},
        {"fullscreenViolations",static_cast<qint64>(fullscreenViolations)},
        {"stableFullscreenOwnership",contextChanges==0&&sourceChanges==0&&
                                     fullscreenViolations==0},
        {"knownBrightFixture",knownBright},{"blackFramesEvaluated",knownBright},
        {"windowActiveAtFinish",window->isActive()},{"samples",static_cast<qint64>(sampleTicks)},
        {"blackSamples",static_cast<qint64>(knownBright?desktopBlack:0)},
        {"blackCompositions",static_cast<qint64>(knownBright?black:0)},
        {"desktopSamples",static_cast<qint64>(desktopSamples)},
        {"desktopCaptureAvailable",knownBright?QJsonValue(desktopSamples>0):
            QJsonValue(QStringLiteral("not-applicable"))},
        {"missingSamples",static_cast<qint64>(missing)},
        {"validMainSamples",static_cast<qint64>(validMain)},
        {"validPipSamples",static_cast<qint64>(validPip)},
        {"completedPipPhases",completedPip},{"inactiveSamples",0},
        {"foregroundAtFinish",foregroundIs(window)},
        {"compositions",static_cast<qint64>(frameEvents)},
        {"stalledSeconds",static_cast<qint64>(stalled)},
        {"slowTicks",static_cast<qint64>(stalled)},{"maxGuiGapMs",maxGap},
        {"maxSampleWorkMs",maxSampleWorkMs},{"maxReadbackMs",maxReadbackMs},
        {"maxDesktopGrabMs",maxDesktopGrabMs},
        {"maxImageSaveMs",artifactSnapshot.maxWriteMs},
        {"artifactWritesAttempted",artifactSnapshot.attempted},
        {"artifactWritesCompleted",artifactSnapshot.completed},
        {"artifactWritesFailed",artifactSnapshot.failed},
        {"artifactWritesPending",artifactSnapshot.pending()},
        {"artifactWritesSettled",artifactWritesSettled},
        {"artifactSettleMs",artifactSettle.elapsed()},
        {"skippedArtifacts",artifactSnapshot.skipped},
        {"failedArtifacts",QJsonArray::fromStringList(artifactSnapshot.failedArtifacts)},
        {"presentationMode",presentationMode()},{"error",*error}});
    return passed?0:1;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QApplication app(argc,argv);
    app.setQuitOnLastWindowClosed(false);
    if(QCoreApplication::arguments().contains(QStringLiteral("--foreground-helper")))
        return mikai::probes::runForegroundHelperMode(app,QCoreApplication::arguments());
    std::fprintf(stderr,"Platform: %s, screen: %s\n",
        qPrintable(QGuiApplication::platformName()),
        app.primaryScreen()?qPrintable(app.primaryScreen()->name()):"none");
    QStringList positional;
    bool knownBright=false,steadyFullscreen=false,focusSwitch=false,baseline=false;
    bool qaMatrix=false,hoverCanary=false;
    for(int i=1;i<argc;++i) {
        const QString arg=QString::fromLocal8Bit(argv[i]);
        if(arg==QLatin1String("--known-bright"))knownBright=true;
        else if(arg==QLatin1String("--steady-fullscreen"))steadyFullscreen=true;
        else if(arg==QLatin1String("--focus-switch"))focusSwitch=true;
        else if(arg==QLatin1String("--baseline"))baseline=true;
        else if(arg==QLatin1String("--qa-matrix"))qaMatrix=true;
        else if(arg==QLatin1String("--hover-canary"))hoverCanary=true;
        else positional.append(arg);
    }
    if(focusSwitch&&steadyFullscreen) {
        std::fprintf(stderr,"--focus-switch and --steady-fullscreen are separate modes\n");
        return 2;
    }
    const int selected=int(focusSwitch)+int(baseline)+int(qaMatrix)+
                       int(steadyFullscreen)+int(hoverCanary);
    if(selected>1) {
        std::fprintf(stderr,"probe modes are mutually exclusive\n");
        return 2;
    }
    if(positional.size()<3||positional.size()>4) {
        std::fprintf(stderr,"Usage: fullscreen_stability_probe <video> <seconds> "
            "<output-directory> [anime4k-preset] [--known-bright] [--steady-fullscreen] "
            "[--focus-switch] [--baseline] [--qa-matrix] [--hover-canary]\n");
        return 2;
    }
    if(selected) qputenv("MIKAI_UI_PROFILE",QByteArrayLiteral("1"));
#ifdef MIKAI_PROBE_HAS_APP
    if(focusSwitch||baseline||qaMatrix) mikai::app::WindowRaisePolicy::install();
#endif
    bool durationOk=false;
    const int seconds=positional.at(1).toInt(&durationOk);
    if(!durationOk||seconds<5||seconds>1800)return 2;
    const bool longMatrix=qaMatrix;
    const auto isolation=mikai::probes::isolation::enter(
        argc,argv,{"fullscreen_stability_probe",false,
                   seconds*1000+(longMatrix?240000:60000)});
    if(isolation!=mikai::probes::isolation::Gate::RunHere)
        return mikai::probes::isolation::exitCode();
    const QString output=positional.at(2);
    if(!QDir().mkpath(output))return 2;
    QFile evidence(QDir(output).filePath(QStringLiteral("samples.jsonl")));
    if(!evidence.open(QIODevice::WriteOnly|QIODevice::Truncate))return 2;
    QTemporaryDir settings;
    if(!settings.isValid())return 2;
    app.setOrganizationName(QStringLiteral("MikaiStabilityProbe"));
    app.setApplicationName(QStringLiteral("FullscreenStabilityProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings.path());
    const QString preset=positional.size()==4?positional.at(3):QStringLiteral("off");
    const QString module=qEnvironmentVariable("MIKAI_MPV_MODULE");
    if(module.isEmpty()) {
        if(!mplayer::probes::selectRuntime())return 2;
    } else {
        const auto runtime=mplayer::native::selectStandardRuntime(module);
        mplayer::probes::detail::reportSelected(runtime,"explicit module");
        if(!runtime.loaded) {
            std::fprintf(stderr,"Runtime refused: %s\n",qPrintable(runtime.reason));
            return 2;
        }
    }
    const QString media=positional.at(0);
    const auto record=[&](QJsonObject row) {
        evidence.write(QJsonDocument(row).toJson(QJsonDocument::Compact));
        evidence.write("\n");
        evidence.flush();
    };
    if(hoverCanary) {
        const QString platform=QGuiApplication::platformName();
        if(platform==QLatin1String("offscreen")||platform==QLatin1String("minimal")) {
            record({{"event","hoverCanary"},{"result","REFUSED"},
                    {"reason","no real display platform"},{"platform",platform}});
            return 77;
        }
        const GlCapability gl=probeGlCapability();
        if(!gl.usable) {
            record({{"event","hoverCanary"},{"result","REFUSED"},{"reason",gl.failure},
                    {"platform",platform},{"glVersion",gl.version},
                    {"glFramebuffers",gl.framebuffers},
                    {"required","OpenGL 2.1+ with framebuffer objects"}});
            return 77;
        }
    }
    auto* controller=new mplayer::quick::PlayerController(
        QStringLiteral("Fullscreen stability verification"),media);
    controller->applyAnime4KPreset(preset);
    auto* window=controller->window();
    if(window==nullptr) { delete controller; return 2; }
    if(hoverCanary) {
        window->setFlag(Qt::WindowDoesNotAcceptFocus,true);
        controller->toggleFullscreen();
        window->resize(960,540);
    } else {
        controller->showWindow();
        window->showFullscreen();
    }
    controller->startPlayer();
    if(!hoverCanary) {
        window->showFullscreen();
        window->raiseWindow();
        window->requestActivate();
        QCursor::setPos(window->mapToGlobal(QPoint(2,2)));
    }
    QString error;
    int result=0;
    if(focusSwitch) result=runQuickFocusSwitchMode(controller,knownBright,seconds,record,&error);
    else if(baseline) result=runQuickBaselineMode(controller,media,knownBright,seconds,record,&error);
    else if(hoverCanary) result=runQuickHoverCanary(controller,seconds,output,record,&error);
    else if(qaMatrix) result=runQuickQaMatrix(controller,output,knownBright,seconds,record,&error);
    else result=runQuickTransitionMode(controller,seconds,output,preset,knownBright,
                                       steadyFullscreen,record,&error);
    record({{"event","probeFinished"},{"result",result==0?"PASS":
             result==77?"REFUSED":"FAIL"},{"error",error}});
    controller->requestClose();
    delete controller;
    return result;
}
