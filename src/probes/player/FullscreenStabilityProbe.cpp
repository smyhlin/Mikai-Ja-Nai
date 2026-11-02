#include "Anime4K.hpp"
#include "PlayerDialog.hpp"
#include "PlayerShell.hpp"
#include "ProbeIsolation.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSurface.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
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

class PlaybackHost final : public mplayer::shell::PlayerHost {
public:
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    void playbackFailed() override { failed = true; }
    bool failed = false;
};

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

bool foregroundIs(QWidget* window) {
    return window != nullptr &&
           GetForegroundWindow() == reinterpret_cast<HWND>(window->winId());
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
bool forceForegroundNow(QWidget* window) {
    if (window == nullptr) return false;
    const HWND target = reinterpret_cast<HWND>(window->winId());
    if (target == nullptr) return false;
    if (SetForegroundWindow(target)) return true;
    const HWND current = GetForegroundWindow();
    const DWORD targetThread = GetWindowThreadProcessId(target, nullptr);
    const DWORD currentThread =
        current != nullptr ? GetWindowThreadProcessId(current, nullptr) : 0;
    const DWORD thisThread = GetCurrentThreadId();
    if (currentThread == 0 || targetThread == 0) return false;
    bool attachedCurrent = false;
    bool attachedTarget = false;
    if (currentThread != thisThread)
        attachedCurrent = AttachThreadInput(currentThread, thisThread, TRUE) != FALSE;
    if (targetThread != thisThread)
        attachedTarget = AttachThreadInput(currentThread, targetThread, TRUE) != FALSE;
    window->raise();
    window->activateWindow();
    SetForegroundWindow(target);
    SetActiveWindow(target);
    if (attachedTarget) AttachThreadInput(currentThread, targetThread, FALSE);
    if (attachedCurrent) AttachThreadInput(currentThread, thisThread, FALSE);
    return GetForegroundWindow() == target;
}
#else
std::optional<quint64> dwmCompositionFramesDisplayed() { return std::nullopt; }

QJsonObject presentationMode() {
    return QJsonObject{{"directFlip", "unsupported"},
                       {"reason", "no DirectFlip query exists for this platform's compositor"}};
}

bool foregroundIs(QWidget* window) {
    return window != nullptr && window->isActiveWindow();
}

QString foregroundProcessImage() {
    return QStringLiteral("unavailable");
}

bool forceForegroundNow(QWidget* window) {
    if (window == nullptr) return false;
    window->raise();
    window->activateWindow();
    return window->isActiveWindow();
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

/// Reads the composed picture back from the surface's own framebuffer, the same
/// path the transition mode samples. Null means "not measured", never "black".
QImage readComposedCrop(mplayer::VideoSurface* surface) {
    if (surface == nullptr || surface->context() == nullptr) return QImage();
    QOpenGLContext* previous = QOpenGLContext::currentContext();
    QSurface* previousSurface = previous != nullptr ? previous->surface() : nullptr;
    QImage image;
    surface->makeCurrent();
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (context == surface->context()) {
        auto* gl = context->functions();
        GLint framebuffer = 0;
        GLint alignment = 0;
        gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        gl->glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, surface->defaultFramebufferObject());
        gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
        const QSize deviceSize = surface->size() * surface->devicePixelRatioF();
        if (deviceSize.width() >= 96 && deviceSize.height() >= 64) {
            QImage capture(96, 64, QImage::Format_RGBA8888);
            gl->glReadPixels(deviceSize.width() / 2 - 48, deviceSize.height() / 2 - 32, 96, 64,
                             GL_RGBA, GL_UNSIGNED_BYTE, capture.bits());
            image = capture;
        }
        gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        gl->glPixelStorei(GL_PACK_ALIGNMENT, alignment);
    }
    if (previous != nullptr && previousSurface != nullptr)
        previous->makeCurrent(previousSurface);
    else
        surface->doneCurrent();
    return image;
}

QJsonObject surfaceState(mplayer::VideoSurface* surface) {
    if (surface == nullptr) return QJsonObject{{"surface", "absent"}};
    const mplayer::PlaybackDiagnostics diagnostics = surface->playbackDiagnostics();
    return QJsonObject{
        {"hasVideo", surface->hasVideo()},
        {"hasVisibleOutputFrame", surface->hasVisibleOutputFrame()},
        {"isPlaying", surface->isPlaying()},
        {"position", surface->position()},
        {"duration", surface->duration()},
        {"renderedOutputFrames", static_cast<qint64>(diagnostics.renderedOutputFrames)},
        {"sourceEpoch", static_cast<qint64>(surface->sourceEpoch())}};
}

bool waitForVideo(mplayer::VideoSurface* surface, int timeoutMs) {
    QElapsedTimer wait;
    wait.start();
    while (!surface->hasVideo() && wait.elapsed() < timeoutMs) spinFor(25);
    return surface->hasVideo();
}

/// Pauses through the surface and verifies it in the core itself. The local
/// playing flag and the core's `pause` property must agree: a command that was
/// not observed would otherwise let the "paused" baseline run on moving video.
bool ensurePaused(mplayer::VideoSurface* surface, int timeoutMs, bool* corePaused) {
    if (corePaused != nullptr) *corePaused = false;
    if (surface == nullptr) return false;
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < timeoutMs) {
        if (!surface->isPlaying()) {
            const auto pause = surface->probeMpvProperty(QStringLiteral("pause"),
                                                         mplayer::ProbePropertyKind::Flag);
            if (pause.has_value() && pause->kind == mplayer::ProbePropertyValue::Kind::Flag) {
                if (corePaused != nullptr) *corePaused = pause->flag;
                if (pause->flag) return true;
            }
        }
        surface->setPaused(true);
        spinFor(120);
    }
    const auto pause = surface->probeMpvProperty(QStringLiteral("pause"),
                                                 mplayer::ProbePropertyKind::Flag);
    if (corePaused != nullptr && pause.has_value() &&
        pause->kind == mplayer::ProbePropertyValue::Kind::Flag)
        *corePaused = pause->flag;
    return !surface->isPlaying() && corePaused != nullptr && *corePaused;
}

/// Takes and verifies native foreground ownership, not just Qt's active flag.
bool ensureForeground(QWidget* window) {
    if (window == nullptr) return false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (foregroundIs(window)) return true;
        forceForegroundNow(window);
        spinFor(80);
    }
    return foregroundIs(window);
}

struct ActivationRun {
    QString phase;
    int cycles = 0;
    int paints = 0;
    int foregroundLostCycles = 0;
    int blackSamples = 0;
    int blackMeasured = 0;
    quint64 attachCalls = 0;
    quint64 contextGenerationChanges = 0;
    quint64 initializeGlCalls = 0;
    quint64 dwmFrames = 0;
    bool dwmMeasured = false;
    /// Owned activation -> the next `frameSwapped` emission (swap completion),
    /// not a physical-present timestamp; JSON labels say so.
    QVector<qint64> activationToFirstSwapMs;
    /// Owned activation -> the first desktop sample that shows the paused bright
    /// picture again after the occluding helper was removed. This is the
    /// observable "visible composition" interval the <=300 ms criterion uses.
    QVector<qint64> activationToVisibleCompositionMs;
    int visibleCompositionObserved = 0;
    int visibleCompositionUnsupported = 0;
    int visibleCompositionTimeouts = 0;
    qint64 desktopGrabCalls = 0;
    qint64 desktopGrabTotalMicros = 0;
    qint64 desktopGrabMaxMicros = 0;
};

ActivationRun runActivationCycles(mplayer::shell::PlayerDialog* dialog,
                                  mplayer::VideoSurface* surface,
                                  mikai::probes::ForegroundPeer& peer, const QString& phase,
                                  int cycles, bool knownBright,
                                  const std::function<void(QJsonObject)>& record) {
    ActivationRun run;
    run.phase = phase;
    QScreen* const screen = surface->screen();
    const QPoint centerGlobal = surface->mapToGlobal(surface->rect().center());
    const QRect cropRect(centerGlobal - QPoint(48, 32), QSize(96, 64));
    const auto grabDesktop = [&]() -> QImage {
        const qint64 started = mikai::gui::uiprofile::nowNs();
        const QImage image = desktopCrop(screen, cropRect);
        const qint64 took = mikai::gui::uiprofile::nowNs() - started;
        ++run.desktopGrabCalls;
        run.desktopGrabTotalMicros += took / 1000;
        run.desktopGrabMaxMicros = std::max(run.desktopGrabMaxMicros, took / 1000);
        return image;
    };
    for (int cycle = 0; cycle < cycles; ++cycle) {
        const quint64 attachBefore =
            mikai::gui::uiprofile::snapshot().threadInputAttaches;
        const auto countersBefore = surface->uiProfileCounters();
        const std::optional<quint64> dwmBefore = dwmCompositionFramesDisplayed();
        // The picture before the helper covers it, and the helper's own pixels
        // while it owns the foreground: a visible-composition sample is only
        // meaningful when the two differ.
        const double baselineLuma = meanLuma(grabDesktop());

        const bool awayOwned = peer.activate();
        spinFor(650);
        const bool awayQtInactive =
            QGuiApplication::applicationState() == Qt::ApplicationInactive;
        const bool stillAway = peer.ownsForeground() && awayQtInactive;
        const double occludedLuma = meanLuma(grabDesktop());
        const bool occlusionVerified =
            baselineLuma >= 40.0 && occludedLuma >= 0.0 && occludedLuma < baselineLuma * 0.5;
        const quint64 attachAfterAway =
            mikai::gui::uiprofile::snapshot().threadInputAttaches;
        // Away-only: an attach made while the player still held the foreground
        // is not a switch-away cost.
        if (stillAway) run.attachCalls += attachAfterAway - attachBefore;
        record(QJsonObject{{"event", "focusSwitchAway"},
                           {"phase", phase},
                           {"cycle", cycle},
                           {"foregroundOwnedInstantly", awayOwned},
                           {"foregroundOwnedAfterWait", stillAway},
                           {"awayQtInactive", awayQtInactive},
                           {"peerPid", static_cast<qint64>(peer.processId())},
                           {"peerWindow", static_cast<qint64>(peer.windowId())},
                           {"foregroundProcess", foregroundProcessImage()},
                           {"applicationState",
                            static_cast<int>(QGuiApplication::applicationState())},
                           {"playerActive", dialog->isActiveWindow()},
                           {"attachCallsObserved",
                            static_cast<qint64>(attachAfterAway - attachBefore)},
                           {"attachCallsCounted",
                            static_cast<qint64>(stillAway ? attachAfterAway - attachBefore
                                                          : 0)},
                           {"attachCounterScope",
                            "shipping WindowRaisePolicy only; the probe's own attach "
                            "dance is not counted"},
                           {"contextGeneration",
                            static_cast<qint64>(
                                surface->uiProfileCounters().contextGeneration)},
                           {"initializeGlCalls",
                            static_cast<qint64>(
                                surface->uiProfileCounters().initializeGlCalls)}});

        bool paintSeen = false;
        qint64 paintAtNs = 0;
        QMetaObject::Connection connection = QObject::connect(
            surface, &QOpenGLWidget::frameSwapped, surface,
            [&paintSeen, &paintAtNs] {
                if (paintSeen) return;
                paintSeen = true;
                paintAtNs = mikai::gui::uiprofile::nowNs();
            },
            Qt::DirectConnection);
        const qint64 activationRequestedNs = mikai::gui::uiprofile::nowNs();
        forceForegroundNow(dialog);
        // A foreign window (the parent's run saw Telegram.exe take the
        // foreground) would make the latency belong to that window, so the
        // interval is retried and then declared invalid rather than measured.
        for (int attempt = 0; attempt < 3 && !foregroundIs(dialog); ++attempt) {
            spinFor(60);
            forceForegroundNow(dialog);
        }
        const bool foregroundOwned = foregroundIs(dialog);
        // The measured interval is owned activation -> visible composition, so
        // the clock starts when this process actually owns the foreground.
        const qint64 activationAtNs = mikai::gui::uiprofile::nowNs();
        const qint64 activationRequestToOwnedMs =
            (activationAtNs - activationRequestedNs) / 1000000;
        dialog->raise();
        dialog->activateWindow();
        peer.sendToBack();
        dialog->update();
        surface->update();
        QElapsedTimer wait;
        wait.start();
        // Composition is observed on the real desktop: the crop shows the black
        // helper until the compositor has actually put the player's picture back
        // on screen. The swap signal is collected in the same loop as the
        // secondary diagnostic.
        qint64 activationToVisibleMs = -1;
        bool visibleObserved = false;
        bool foregroundLostDuringPoll = !foregroundOwned;
        const double visibleThreshold = std::max(30.0, baselineLuma * 0.6);
        if (occlusionVerified && foregroundOwned) {
            while (wait.elapsed() < 2500 && (!paintSeen || !visibleObserved)) {
                // Another process can take the foreground mid-poll; its pixels
                // must never be accepted as this player's composition.
                if (!foregroundIs(dialog)) {
                    foregroundLostDuringPoll = true;
                    break;
                }
                if (!visibleObserved) {
                    const QImage composed = grabDesktop();
                    if (!composed.isNull() && meanLuma(composed) >= visibleThreshold &&
                        foregroundIs(dialog)) {
                        visibleObserved = true;
                        activationToVisibleMs =
                            (mikai::gui::uiprofile::nowNs() - activationAtNs) / 1000000;
                    }
                }
                spinFor(6);
            }
        }
        while (!paintSeen && wait.elapsed() < 2500) spinFor(5);
        QObject::disconnect(connection);
        const bool foregroundOwnedAtEnd = foregroundIs(dialog);
        const bool cycleValid = foregroundOwned && stillAway && !foregroundLostDuringPoll &&
                                foregroundOwnedAtEnd;
        if (!cycleValid) visibleObserved = false;
        qint64 activationToFirstSwapMs = -1;
        if (paintSeen && cycleValid) {
            activationToFirstSwapMs = (paintAtNs - activationAtNs) / 1000000;
            ++run.paints;
            run.activationToFirstSwapMs.append(activationToFirstSwapMs);
        } else if (!cycleValid) {
            ++run.foregroundLostCycles;
        }
        if (visibleObserved && cycleValid) {
            ++run.visibleCompositionObserved;
            run.activationToVisibleCompositionMs.append(activationToVisibleMs);
        } else if (!occlusionVerified) {
            ++run.visibleCompositionUnsupported;
        } else if (cycleValid) {
            ++run.visibleCompositionTimeouts;
        }
        spinFor(250);
        // Sampling the screen while another process owns the foreground would
        // report that process's pixels; the cycle names the loss instead.
        const QImage crop = cycleValid ? readComposedCrop(surface) : QImage();
        bool black = false;
        if (!crop.isNull()) {
            ++run.blackMeasured;
            black = isBlack(crop);
            if (black) ++run.blackSamples;
        }
        const auto countersAfter = surface->uiProfileCounters();
        run.contextGenerationChanges +=
            countersAfter.contextGeneration - countersBefore.contextGeneration;
        run.initializeGlCalls +=
            countersAfter.initializeGlCalls - countersBefore.initializeGlCalls;
        const std::optional<quint64> dwmAfter = dwmCompositionFramesDisplayed();
        const bool dwmDeltaMeasured = dwmBefore.has_value() && dwmAfter.has_value();
        const quint64 dwmDelta = dwmDeltaMeasured ? *dwmAfter - *dwmBefore : 0;
        if (dwmDeltaMeasured) {
            run.dwmMeasured = true;
            run.dwmFrames += dwmDelta;
        }
        record(QJsonObject{
            {"event", "focusSwitchBack"},
            {"phase", phase},
            {"cycle", cycle},
            {"activationToFirstSwapMs", activationToFirstSwapMs},
            {"paintObserved", paintSeen},
            {"activationToVisibleCompositionMs",
             visibleObserved ? QJsonValue(activationToVisibleMs)
                             : QJsonValue(QStringLiteral("unavailable"))},
            {"visibleCompositionObserved", visibleObserved},
            {"visibleCompositionSupported", occlusionVerified},
            {"baselineLuma", baselineLuma},
            {"occludedLuma", occludedLuma},
            {"foregroundOwned", foregroundOwned},
            {"foregroundOwnedAtEnd", foregroundOwnedAtEnd},
            {"foregroundLostDuringPoll", foregroundLostDuringPoll},
            {"activationRequestToOwnedMs", activationRequestToOwnedMs},
            {"foregroundProcess", foregroundProcessImage()},
            {"cycleValid", cycleValid},
            {"black", black},
            {"blackMeasured", !crop.isNull()},
            {"cropLuma", meanLuma(crop)},
            {"contextGeneration",
             static_cast<qint64>(countersAfter.contextGeneration)},
            {"initializeGlCalls", static_cast<qint64>(countersAfter.initializeGlCalls)},
            {"pacingDisarms", static_cast<qint64>(countersAfter.pacingDisarms)},
            {"dwmCompositionFramesDisplayedDelta",
             dwmDeltaMeasured ? QJsonValue(static_cast<qint64>(dwmDelta))
                              : QJsonValue(QStringLiteral("unavailable"))},
            {"applicationState", static_cast<int>(QGuiApplication::applicationState())},
            {"playerActive", dialog->isActiveWindow()},
            {"knownBrightFixture", knownBright},
            {"presentationMode", presentationMode()}});
        ++run.cycles;
    }
    return run;
}

QJsonObject activationSummary(const ActivationRun& run) {
    const double p50 = percentileMs(run.activationToFirstSwapMs, 0.50);
    const double p95 = percentileMs(run.activationToFirstSwapMs, 0.95);
    qint64 maximum = -1;
    for (qint64 value : run.activationToFirstSwapMs) maximum = std::max(maximum, value);
    const double visibleP50 =
        percentileMs(run.activationToVisibleCompositionMs, 0.50);
    const double visibleP95 =
        percentileMs(run.activationToVisibleCompositionMs, 0.95);
    qint64 visibleMaximum = -1;
    for (qint64 value : run.activationToVisibleCompositionMs)
        visibleMaximum = std::max(visibleMaximum, value);
    return QJsonObject{
        {"phase", run.phase},
        {"cycles", run.cycles},
        {"paintsObserved", run.paints},
        {"cyclesWithoutPaint", run.cycles - run.paints},
        {"foregroundLostCycles", run.foregroundLostCycles},
        {"cyclesValid", run.cycles - run.foregroundLostCycles},
        {"activationToFirstSwapP50Ms",
         run.activationToFirstSwapMs.isEmpty() ? QJsonValue(QStringLiteral("unavailable"))
                                               : QJsonValue(p50)},
        {"activationToFirstSwapP95Ms",
         run.activationToFirstSwapMs.isEmpty() ? QJsonValue(QStringLiteral("unavailable"))
                                               : QJsonValue(p95)},
        {"activationToFirstSwapMaxMs",
         maximum < 0 ? QJsonValue(QStringLiteral("unavailable")) : QJsonValue(maximum)},
        {"activationToVisibleCompositionP50Ms",
         run.activationToVisibleCompositionMs.isEmpty()
             ? QJsonValue(QStringLiteral("unavailable"))
             : QJsonValue(visibleP50)},
        {"activationToVisibleCompositionP95Ms",
         run.activationToVisibleCompositionMs.isEmpty()
             ? QJsonValue(QStringLiteral("unavailable"))
             : QJsonValue(visibleP95)},
        {"activationToVisibleCompositionMaxMs",
         visibleMaximum < 0 ? QJsonValue(QStringLiteral("unavailable"))
                            : QJsonValue(visibleMaximum)},
        {"visibleCompositionSamples", static_cast<int>(run.activationToVisibleCompositionMs.size())},
        {"visibleCompositionObservedCycles", run.visibleCompositionObserved},
        {"visibleCompositionUnsupportedCycles", run.visibleCompositionUnsupported},
        {"visibleCompositionTimeoutCycles", run.visibleCompositionTimeouts},
        {"desktopGrabMeanMicros",
         run.desktopGrabCalls == 0
             ? QJsonValue(QStringLiteral("unavailable"))
             : QJsonValue(static_cast<double>(run.desktopGrabTotalMicros) /
                          run.desktopGrabCalls)},
        {"desktopGrabMaxMicros", run.desktopGrabMaxMicros},
        {"visibleCompositionDefinition",
         "activation (owned foreground) to the first desktop screen-crop sample whose "
         "brightness matches the pre-switch picture after a verified occluding helper; "
         "an observable composited-pixels interval, not a scanout timestamp"},
        {"latencyDefinition",
         "activation (owned foreground) to the next QOpenGLWidget::frameSwapped; this "
         "is a swap-completion latency, not a physical scanout/present timestamp"},
        {"attachCallsDuringAway",
         attachMetricSupported() ? QJsonValue(static_cast<qint64>(run.attachCalls))
                                 : QJsonValue(QStringLiteral("unavailable"))},
        {"attachMetricSupported", attachMetricSupported()},
        {"contextGenerationChanges", static_cast<qint64>(run.contextGenerationChanges)},
        {"initializeGlCalls", static_cast<qint64>(run.initializeGlCalls)},
        {"dwmCompositionFramesDisplayed",
         run.dwmMeasured ? QJsonValue(static_cast<qint64>(run.dwmFrames))
                         : QJsonValue(QStringLiteral("unavailable"))},
        {"blackSamples", run.blackSamples},
        {"blackSamplesMeasured", run.blackMeasured},
        {"presentationMode", presentationMode()}};
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

int runFocusSwitchMode(mplayer::shell::PlayerDialog* dialog, mplayer::VideoSurface* surface,
                       mplayer::shell::PlayerShell* shell, bool knownBright, int seconds,
                       const std::function<void(QJsonObject)>& record, QString* error) {
    if (!waitForVideo(surface, 20000)) {
        *error = QStringLiteral("no video decoded before the focus-switch run");
        record(QJsonObject{{"event", "focusSwitchResult"},
                           {"result", "INCOMPLETE"},
                           {"reason", "no video decoded"},
                           {"surfaceState", surfaceState(surface)},
                           {"error", *error}});
        return 2;
    }
    dialog->pauseIfPlaying();
    spinFor(300);
    bool corePaused = false;
    const bool paused = ensurePaused(surface, 5000, &corePaused);
    const bool foregroundOwnedAtStart = ensureForeground(dialog);
    spinFor(300);
    const QImage fixture = readComposedCrop(surface);
    const double fixtureLuma = meanLuma(fixture);
    const bool fixtureBright = fixtureLuma >= 40.0;
    const bool preconditionsValid = paused && corePaused && fixtureBright &&
                                    foregroundOwnedAtStart;
    const quint64 setupAttaches = mikai::gui::uiprofile::snapshot().threadInputAttaches;
    mikai::gui::uiprofile::resetCounters();
    record(QJsonObject{{"event", "focusModeStart"},
                       {"fixtureMeanLuma", fixtureLuma},
                       {"fixtureSampled", !fixture.isNull()},
                       {"fixtureBright", fixtureBright},
                       {"knownBrightFixture", knownBright},
                       {"paused", paused},
                       {"corePaused", corePaused},
                       {"foregroundOwned", foregroundOwnedAtStart},
                       {"attachCallsBeforePhaseReset", static_cast<qint64>(setupAttaches)},
                       {"profileCountersEnabled", mikai::gui::uiprofile::countersEnabled()},
                       {"presentationMode", presentationMode()},
                       {"surfaceState", surfaceState(surface)}});
    if (!preconditionsValid) {
        *error = QStringLiteral("focus mode requires a verified paused bright frame with "
                                "owned foreground (paused=%1 corePaused=%2 luma=%3 foreground=%4)")
                     .arg(paused)
                     .arg(corePaused)
                     .arg(fixtureLuma, 0, 'f', 1)
                     .arg(foregroundOwnedAtStart);
        record(QJsonObject{{"event", "focusSwitchResult"},
                           {"result", "INVALID"},
                           {"reason", *error},
                           {"surfaceState", surfaceState(surface)}});
        return 3;
    }

    // A separate process owns the black helper: its window is another
    // application's, so native activation and Qt's inactive state are real.
    const QPoint center = dialog->mapToGlobal(dialog->rect().center());
    mikai::probes::ForegroundPeer peer;
    QString peerError;
    if (!peer.start(QRect(center - QPoint(240, 180), QSize(480, 360)), &peerError)) {
        *error = peerError;
        record(QJsonObject{{"event", "focusSwitchResult"},
                           {"result", "UNAVAILABLE"},
                           {"reason", peerError},
                           {"surfaceState", surfaceState(surface)}});
        return 3;
    }
    record(QJsonObject{{"event", "foregroundPeer"},
                       {"pid", static_cast<qint64>(peer.processId())},
                       {"window", static_cast<qint64>(peer.windowId())},
                       {"foreignPid", peer.processId() != QCoreApplication::applicationPid()}});
    QVector<ActivationRun> runs;
    runs.append(runActivationCycles(dialog, surface, peer, QStringLiteral("plain"),
                                    std::max(1, std::min(10, seconds / 3)), knownBright, record));
    dialog->beginEnginePreparation(QStringLiteral("Focus probe overlay"));
    spinFor(600);
    runs.append(runActivationCycles(dialog, surface, peer, QStringLiteral("engine-overlay"),
                                    std::max(1, std::min(5, seconds / 6)), knownBright, record));
    dialog->endEnginePreparation(true, QString());
    spinFor(300);

    if (shell != nullptr) {
        const QPoint overDock(shell->width() / 2, shell->height() - 70);
        QCursor::setPos(shell->mapToGlobal(overDock));
        QTest::mouseMove(shell, overDock);
        spinFor(400);
    }
    runs.append(runActivationCycles(dialog, surface, peer, QStringLiteral("dock-hover"),
                                    std::max(1, std::min(5, seconds / 6)), knownBright, record));
    QCursor::setPos(dialog->mapToGlobal(QPoint(2, 2)));

    QJsonArray phaseRows;
    QVector<qint64> allSwapMs;
    QVector<qint64> allVisibleMs;
    int blackSamples = 0;
    int blackMeasured = 0;
    int foregroundLost = 0;
    int visibleUnsupported = 0;
    int visibleTimeouts = 0;
    qint64 grabCalls = 0;
    qint64 grabMicros = 0;
    qint64 grabMaxMicros = -1;
    for (const ActivationRun& run : runs) {
        phaseRows.append(activationSummary(run));
        allSwapMs += run.activationToFirstSwapMs;
        allVisibleMs += run.activationToVisibleCompositionMs;
        blackSamples += run.blackSamples;
        blackMeasured += run.blackMeasured;
        foregroundLost += run.foregroundLostCycles;
        visibleUnsupported += run.visibleCompositionUnsupported;
        visibleTimeouts += run.visibleCompositionTimeouts;
        grabCalls += run.desktopGrabCalls;
        grabMicros += run.desktopGrabTotalMicros;
        grabMaxMicros = std::max(grabMaxMicros, run.desktopGrabMaxMicros);
    }
    const bool measurementAvailable = !allVisibleMs.isEmpty() && blackMeasured > 0;
    const bool clean = measurementAvailable && blackSamples == 0;
    record(QJsonObject{{"event", "focusSwitchResult"},
                       {"result", !measurementAvailable ? "INCOMPLETE"
                                : clean             ? "PASS"
                                                    : "OBSERVED_BLACK"},
                       {"phases", phaseRows},
                       {"fixtureMeanLuma", fixtureLuma},
                       {"fixtureSampled", !fixture.isNull()},
                       {"knownBrightFixture", knownBright},
                       {"attachCallsBeforePhaseReset", static_cast<qint64>(setupAttaches)},
                       {"activationToVisibleCompositionP95Ms",
                        allVisibleMs.isEmpty()
                            ? QJsonValue(QStringLiteral("unavailable"))
                            : QJsonValue(percentileMs(allVisibleMs, 0.95))},
                       {"activationToVisibleCompositionSamples",
                        static_cast<int>(allVisibleMs.size())},
                       {"visibleCompositionUnsupportedCycles", visibleUnsupported},
                       {"visibleCompositionTimeoutCycles", visibleTimeouts},
                       {"activationToFirstSwapP95Ms",
                        allSwapMs.isEmpty() ? QJsonValue(QStringLiteral("unavailable"))
                                            : QJsonValue(percentileMs(allSwapMs, 0.95))},
                       {"activationToFirstSwapSamples", static_cast<int>(allSwapMs.size())},
                       {"desktopGrabMeanMicros",
                        grabCalls == 0
                            ? QJsonValue(QStringLiteral("unavailable"))
                            : QJsonValue(static_cast<double>(grabMicros) / grabCalls)},
                       {"desktopGrabMaxMicros", grabMaxMicros},
                       {"visibleCompositionDefinition",
                        "activation (owned foreground) to the first desktop screen-crop "
                        "sample matching the pre-switch picture after a verified "
                        "occluding helper; observable composited pixels, not scanout"},
                       {"blackSamples", blackSamples},
                       {"blackSamplesMeasured", blackMeasured},
                       {"foregroundLostCycles", foregroundLost},
                       {"presentationMode", presentationMode()},
                       {"surfaceState", surfaceState(surface)},
                       {"error", *error}});
    peer.stop();
    return 0;
}

int runBaselineMode(mplayer::shell::PlayerDialog* dialog, mplayer::VideoSurface* surface,
                    mplayer::shell::PlayerShell* shell, const QString& media, bool knownBright,
                    int seconds, const std::function<void(QJsonObject)>& record, QString* error) {
    if (!waitForVideo(surface, 20000)) {
        *error = QStringLiteral("no video decoded before the baseline run");
        record(QJsonObject{{"event", "baselineResult"},
                           {"result", "INCOMPLETE"},
                           {"reason", "no video decoded"},
                           {"mediaFacts", mediaFacts(media)},
                           {"surfaceState", surfaceState(surface)},
                           {"error", *error}});
        return 2;
    }
    dialog->pauseIfPlaying();
    spinFor(300);
    bool corePaused = false;
    const bool paused = ensurePaused(surface, 5000, &corePaused);
    const bool foregroundOwnedAtStart = ensureForeground(dialog);
    spinFor(300);
    const QImage fixture = readComposedCrop(surface);
    const double fixtureLuma = meanLuma(fixture);
    const bool fixtureBright = fixtureLuma >= 40.0;
    const bool preconditionsValid =
        paused && corePaused && fixtureBright && foregroundOwnedAtStart;
    const quint64 setupAttaches = mikai::gui::uiprofile::snapshot().threadInputAttaches;
    mikai::gui::uiprofile::resetCounters();
    record(QJsonObject{{"event", "baselineStart"},
                       {"seconds", seconds},
                       {"knownBrightFixture", knownBright},
                       {"fixtureMeanLuma", fixtureLuma},
                       {"fixtureSampled", !fixture.isNull()},
                       {"fixtureBright", fixtureBright},
                       {"paused", paused},
                       {"corePaused", corePaused},
                       {"foregroundOwned", foregroundOwnedAtStart},
                       {"preconditionsValid", preconditionsValid},
                       {"attachCallsBeforePhaseReset", static_cast<qint64>(setupAttaches)},
                       {"profileCountersEnabled", mikai::gui::uiprofile::countersEnabled()},
                       {"mediaFacts", mediaFacts(media)},
                       {"surfaceState", surfaceState(surface)},
                       {"presentationMode", presentationMode()}});

    // ── metric 1 (pacing disarms) + 5 (chrome paint cost) ──────────────────
    // Only ticks made while this process owns the native foreground count: a
    // synthetic hover in somebody else's active window is not the requested
    // visible hover storm, and its paints are not baseline evidence.
    const int stormMs = std::max(3000, std::min(30000, seconds * 1000 / 2));
    int validStormMs = 0;
    int validStormTicks = 0;
    int stormForegroundLost = 0;
    qint64 stormPacingDisarms = 0;
    quint64 stormPaintCalls = 0;
    quint64 stormChromeDirtyRequests = 0;
    quint64 stormChromeDirtyApplied = 0;
    quint64 stormChromePaintCalls = 0;
    quint64 stormChromePaintEntries = 0;
    qint64 stormChromePaintMicros = 0;
    qint64 stormChromePaintMax = -1;
    const bool stormAttempted = shell != nullptr && preconditionsValid;
    if (stormAttempted) {
        const QPoint dock(shell->width() / 2, shell->height() - 62);
        const QPoint scrub(shell->width() / 2, shell->height() - 104);
        const QPoint volume(shell->width() / 2 - 140, shell->height() - 62);
        QElapsedTimer wall;
        wall.start();
        while (validStormMs < stormMs && wall.elapsed() < stormMs * 3) {
            if (!foregroundIs(dialog)) {
                ++stormForegroundLost;
                ensureForeground(dialog);
                spinFor(16);
                continue;
            }
            const auto profileBefore = surface->uiProfileCounters();
            const auto countersBefore = mikai::gui::uiprofile::snapshot();
            const QPoint point = validStormTicks % 3 == 0
                                     ? dock
                                     : (validStormTicks % 3 == 1 ? scrub : volume);
            QCursor::setPos(shell->mapToGlobal(point));
            QTest::mouseMove(shell, point);
            spinFor(16);
            if (!foregroundIs(dialog)) {
                // The tick started under our window and ended under somebody
                // else's: its paints cannot be attributed to the storm.
                ++stormForegroundLost;
                continue;
            }
            const auto profileAfter = surface->uiProfileCounters();
            const auto countersAfter = mikai::gui::uiprofile::snapshot();
            ++validStormTicks;
            validStormMs += 16;
            stormPacingDisarms +=
                profileAfter.pacingDisarms - profileBefore.pacingDisarms;
            stormPaintCalls += profileAfter.paintCalls - profileBefore.paintCalls;
            stormChromeDirtyRequests += countersAfter.chromeDirtyRequests -
                                        countersBefore.chromeDirtyRequests;
            stormChromeDirtyApplied += countersAfter.chromeDirtyApplied -
                                       countersBefore.chromeDirtyApplied;
            const quint64 paintCalls =
                countersAfter.chromePaintCalls - countersBefore.chromePaintCalls;
            stormChromePaintCalls += paintCalls;
            stormChromePaintEntries += countersAfter.chromePaintEntries -
                                       countersBefore.chromePaintEntries;
            stormChromePaintMicros +=
                static_cast<qint64>(countersAfter.chromePaintMicrosTotal -
                                    countersBefore.chromePaintMicrosTotal);
            stormChromePaintMax = std::max(stormChromePaintMax,
                                           countersAfter.chromePaintMicrosMax);
        }
        QCursor::setPos(dialog->mapToGlobal(QPoint(2, 2)));
    }
    const bool stormPausedAtEnd = !surface->isPlaying();
    const bool stormValid = stormAttempted && validStormMs >= stormMs * 9 / 10 &&
                            validStormTicks > 0 && stormChromeDirtyRequests > 0 &&
                            stormPausedAtEnd;
    record(QJsonObject{
        {"event", "hoverStorm"},
        {"requestedDurationMs", stormMs},
        {"validDurationMs", validStormMs},
        {"validTicks", validStormTicks},
        {"foregroundLostTicks", stormForegroundLost},
        {"pausedAtEnd", stormPausedAtEnd},
        {"valid", stormValid},
        {"invalidReason", stormValid ? QString()
                                     : (shell == nullptr
                                            ? QStringLiteral("no shell")
                                            : (!preconditionsValid
                                                   ? QStringLiteral("preconditions failed")
                                                   : (!stormPausedAtEnd
                                                          ? QStringLiteral("playback resumed")
                                                          : QStringLiteral("foreground not "
                                                                           "owned for the "
                                                                           "requested interval"))))},
        {"pacingDisarms", stormPacingDisarms},
        {"paintCalls", static_cast<qint64>(stormPaintCalls)},
        {"chromeDirtyRequests", static_cast<qint64>(stormChromeDirtyRequests)},
        {"chromeDirtyApplied", static_cast<qint64>(stormChromeDirtyApplied)},
        {"chromePaintCalls", static_cast<qint64>(stormChromePaintCalls)},
        {"chromePaintEntries", static_cast<qint64>(stormChromePaintEntries)},
        {"chromePaintMeanMicros",
         stormChromePaintCalls == 0
             ? QJsonValue(QStringLiteral("unavailable"))
             : QJsonValue(static_cast<double>(stormChromePaintMicros) /
                          stormChromePaintCalls)},
        {"chromePaintMaxMicros",
         stormChromePaintMax < 0 ? QJsonValue(QStringLiteral("unavailable"))
                                 : QJsonValue(stormChromePaintMax)}});

    // ── metric 2: away/back against another process, ten cycles ────────────
    const QPoint activationCenter = dialog->mapToGlobal(dialog->rect().center());
    mikai::probes::ForegroundPeer peer;
    QString peerError;
    const bool peerStarted =
        peer.start(QRect(activationCenter - QPoint(240, 180), QSize(480, 360)), &peerError);
    ActivationRun activation;
    activation.phase = QStringLiteral("plain");
    if (peerStarted) {
        record(QJsonObject{{"event", "foregroundPeer"},
                           {"pid", static_cast<qint64>(peer.processId())},
                           {"window", static_cast<qint64>(peer.windowId())},
                           {"foreignPid",
                            peer.processId() != QCoreApplication::applicationPid()}});
        activation = runActivationCycles(dialog, surface, peer, QStringLiteral("plain"), 10,
                                         knownBright, record);
        peer.stop();
    } else {
        record(QJsonObject{{"event", "foregroundPeerUnavailable"},
                           {"reason", peerError},
                           {"activationMetric", "unavailable"}});
    }
    spinFor(300);

    // ── metric 4: surface-geometry writes per fullscreen transition ────────
    const quint64 geometryBefore = mikai::gui::uiprofile::snapshot().layoutGeometryWrites;
    const quint64 masksBefore = mikai::gui::uiprofile::snapshot().setMaskApplications;
    constexpr int kFullscreenTransitions = 5;
    for (int index = 0; index < kFullscreenTransitions; ++index) {
        dialog->showNormal();
        spinFor(700);
        dialog->showFullScreen();
        spinFor(700);
    }
    const quint64 geometryAfter = mikai::gui::uiprofile::snapshot().layoutGeometryWrites;
    const quint64 masksAfter = mikai::gui::uiprofile::snapshot().setMaskApplications;
    record(QJsonObject{
        {"event", "fullscreenTransitions"},
        {"transitions", kFullscreenTransitions},
        {"layoutGeometryWrites", static_cast<qint64>(geometryAfter - geometryBefore)},
        {"layoutGeometryWritesPerTransition",
         static_cast<double>(geometryAfter - geometryBefore) / kFullscreenTransitions},
        {"setMaskApplications", static_cast<qint64>(masksAfter - masksBefore)}});

    // ── metric 3: black samples across twenty loadfile transitions ─────────
    constexpr int kLoadTransitions = 20;
    int completedLoads = 0;
    int loadBlack = 0;
    int loadSampled = 0;
    QString lastLoadError;
    for (int index = 0; index < kLoadTransitions; ++index) {
        if (!surface->open(media)) {
            lastLoadError = QStringLiteral("open() refused at transition %1").arg(index);
            break;
        }
        QElapsedTimer loadWait;
        loadWait.start();
        while (!surface->hasVideo() && loadWait.elapsed() < 5000) spinFor(20);
        QElapsedTimer sampleClock;
        sampleClock.start();
        while (sampleClock.elapsed() < 600) {
            const QImage crop = readComposedCrop(surface);
            if (!crop.isNull()) {
                ++loadSampled;
                if (isBlack(crop)) ++loadBlack;
            }
            spinFor(24);
        }
        ++completedLoads;
    }
    const auto loadCounters = surface->uiProfileCounters();
    record(QJsonObject{{"event", "loadfileTransitions"},
                       {"transitionsRequested", kLoadTransitions},
                       {"transitionsCompleted", completedLoads},
                       {"blackSamples", loadBlack},
                       {"samplesMeasured", loadSampled},
                       {"sourceEpoch", static_cast<qint64>(surface->sourceEpoch())},
                       {"pacingDisarms", static_cast<qint64>(loadCounters.pacingDisarms)},
                       {"surfaceState", surfaceState(surface)},
                       {"loadError", lastLoadError}});

    const double swapP95 = percentileMs(activation.activationToFirstSwapMs, 0.95);
    qint64 maxSwap = -1;
    for (qint64 value : activation.activationToFirstSwapMs)
        maxSwap = std::max(maxSwap, value);
    const double visibleP95 =
        percentileMs(activation.activationToVisibleCompositionMs, 0.95);
    qint64 maxVisible = -1;
    for (qint64 value : activation.activationToVisibleCompositionMs)
        maxVisible = std::max(maxVisible, value);
    const auto finalCounters = mikai::gui::uiprofile::snapshot();
    const bool swapMeasured = !activation.activationToFirstSwapMs.isEmpty();
    const bool visibleMeasured = !activation.activationToVisibleCompositionMs.isEmpty();
    const bool blackMeasured = loadSampled > 0;
    const bool measured =
        preconditionsValid && stormValid && peerStarted && visibleMeasured && blackMeasured;
    record(QJsonObject{
        {"event", "baselineResult"},
        {"result", measured ? "MEASURED"
                            : (!preconditionsValid || !stormValid ? "INVALID" : "INCOMPLETE")},
        {"fixtureMeanLuma", fixtureLuma},
        {"fixtureSampled", !fixture.isNull()},
        {"fixtureBright", fixtureBright},
        {"knownBrightFixture", knownBright},
        {"preconditionsValid", preconditionsValid},
        {"hoverStormValid", stormValid},
        {"hoverStormValidDurationMs", validStormMs},
        {"hoverStormLostTicks", stormForegroundLost},
        {"pacingDisarmsHoverStorm", stormPacingDisarms},
        {"activationToVisibleCompositionP95Ms",
         visibleMeasured ? QJsonValue(visibleP95)
                         : QJsonValue(QStringLiteral("unavailable"))},
        {"activationToVisibleCompositionSamples",
         static_cast<int>(activation.activationToVisibleCompositionMs.size())},
        {"activationToVisibleCompositionMaxMs",
         maxVisible < 0 ? QJsonValue(QStringLiteral("unavailable"))
                        : QJsonValue(maxVisible)},
        {"visibleCompositionUnsupportedCycles", activation.visibleCompositionUnsupported},
        {"visibleCompositionTimeoutCycles", activation.visibleCompositionTimeouts},
        {"desktopGrabMeanMicros",
         activation.desktopGrabCalls == 0
             ? QJsonValue(QStringLiteral("unavailable"))
             : QJsonValue(static_cast<double>(activation.desktopGrabTotalMicros) /
                          activation.desktopGrabCalls)},
        {"desktopGrabMaxMicros", activation.desktopGrabMaxMicros},
        {"visibleCompositionDefinition",
         "activation (owned foreground) to the first desktop screen-crop sample matching "
         "the pre-switch picture after a verified occluding helper; observable "
         "composited pixels, not scanout"},
        {"activationToFirstSwapP95Ms",
         swapMeasured ? QJsonValue(swapP95) : QJsonValue(QStringLiteral("unavailable"))},
        {"activationToFirstSwapSamples",
         static_cast<int>(activation.activationToFirstSwapMs.size())},
        {"activationToFirstSwapMaxMs",
         maxSwap < 0 ? QJsonValue(QStringLiteral("unavailable"))
                     : QJsonValue(maxSwap)},
        {"latencyDefinition",
         "activation (owned foreground) to the next QOpenGLWidget::frameSwapped; this "
         "is a swap-completion latency, not a physical scanout/present timestamp"},
        {"blackSamplesLoadfile20", blackMeasured ? QJsonValue(loadBlack)
                                                 : QJsonValue(QStringLiteral("unavailable"))},
        {"loadfileSamplesMeasured", loadSampled},
        {"loadfileTransitionsCompleted", completedLoads},
        {"attachCallsDuringAway",
         attachMetricSupported() ? QJsonValue(static_cast<qint64>(activation.attachCalls))
                                 : QJsonValue(QStringLiteral("unavailable"))},
        {"attachMetricSupported", attachMetricSupported()},
        {"activationForegroundLostCycles", activation.foregroundLostCycles},
        {"hoverStormForegroundLostTicks", stormForegroundLost},
        {"contextGenerationChanges", static_cast<qint64>(activation.contextGenerationChanges)},
        {"initializeGlCalls", static_cast<qint64>(activation.initializeGlCalls)},
        {"surfaceGeometryWritesPerFullscreenTransition",
         static_cast<double>(geometryAfter - geometryBefore) / kFullscreenTransitions},
        {"setMaskApplicationsPerRun", static_cast<qint64>(masksAfter - masksBefore)},
        {"chromePaintMeanMicros",
         stormChromePaintCalls == 0
             ? QJsonValue(QStringLiteral("unavailable"))
             : QJsonValue(static_cast<double>(stormChromePaintMicros) / stormChromePaintCalls)},
        {"chromePaintCalls", static_cast<qint64>(finalCounters.chromePaintCalls)},
        {"blackSamplesActivationPhases", activation.blackSamples},
        {"mediaFacts", mediaFacts(media)},
        {"surfaceState", surfaceState(surface)},
        {"presentationMode", presentationMode()},
        {"profileCountersEnabled", mikai::gui::uiprofile::countersEnabled()},
        {"invalidReason",
         measured ? QString()
                  : (!preconditionsValid
                         ? QStringLiteral("paused bright frame and owned foreground were not "
                                          "verified before the storm")
                         : (!stormValid
                                ? QStringLiteral("hover storm was not valid for the requested "
                                                 "interval")
                                : (!peerStarted
                                       ? QStringLiteral("foreign foreground helper was "
                                                        "unavailable")
                                       : QStringLiteral("activation or loadfile measurement "
                                                        "was unavailable"))))},
        {"error", *error}});
    if (!measured && error != nullptr) {
        *error = !preconditionsValid
                     ? QStringLiteral("baseline INVALID: paused bright frame/foreground not "
                                      "verified")
                     : (!stormValid ? QStringLiteral("baseline INVALID: hover storm was not "
                                                     "valid for the requested interval")
                                    : QStringLiteral("baseline INCOMPLETE: activation or "
                                                     "loadfile measurement unavailable"));
    }
    return measured ? 0 : 3;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (QCoreApplication::arguments().contains(QStringLiteral("--foreground-helper")))
        return mikai::probes::runForegroundHelperMode(app, QCoreApplication::arguments());
    std::fprintf(stderr, "Platform: %s, screen: %s\n",
                 qPrintable(QGuiApplication::platformName()),
                 app.primaryScreen() != nullptr ? qPrintable(app.primaryScreen()->name()) : "none");
    QStringList positionalArguments;
    bool knownBright = false;
    bool steadyFullscreen = false;
    bool focusSwitch = false;
    bool baseline = false;
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QLatin1String("--known-bright")) {
            knownBright = true;
        } else if (argument == QLatin1String("--steady-fullscreen")) {
            steadyFullscreen = true;
        } else if (argument == QLatin1String("--focus-switch")) {
            focusSwitch = true;
        } else if (argument == QLatin1String("--baseline")) {
            baseline = true;
        } else {
            positionalArguments.append(argument);
        }
    }
    if (focusSwitch && steadyFullscreen) {
        std::fprintf(stderr, "--focus-switch and --steady-fullscreen are separate modes\n");
        return 2;
    }
    if (positionalArguments.size() < 3 || positionalArguments.size() > 4) {
        std::fprintf(stderr,
                     "Usage: fullscreen_stability_probe <video> <seconds> <output-directory> "
                     "[anime4k-preset] [--known-bright] [--steady-fullscreen] "
                     "[--focus-switch] [--baseline]\n");
        return 2;
    }
    if (focusSwitch || baseline) {
        // The counters are the measurement; the env var caches on first use, so
        // it is set before any widget exists.
        qputenv("MIKAI_UI_PROFILE", QByteArrayLiteral("1"));
#ifdef MIKAI_PROBE_HAS_APP
        // The shipping raise policy, so AttachThreadInput counts are the ones
        // the application would make and not a probe-local imitation.
        mikai::app::WindowRaisePolicy::install();
#endif
    }
    bool validDuration = false;
    const int seconds = positionalArguments.at(1).toInt(&validDuration);
    if (!validDuration || seconds < 5 || seconds > 1800) return 2;
    const int isolationTimeoutMs = seconds * 1000 + 60000;
    const auto isolation = mikai::probes::isolation::enter(
        argc, argv, {"fullscreen_stability_probe", false, isolationTimeoutMs});
    if (isolation != mikai::probes::isolation::Gate::RunHere)
        return mikai::probes::isolation::exitCode();
    const QString outputPath = positionalArguments.at(2);
    if (!QDir().mkpath(outputPath)) return 2;
    QFile evidence(QDir(outputPath).filePath(QStringLiteral("samples.jsonl")));
    if (!evidence.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
    QTemporaryDir settings;
    if (!settings.isValid()) return 2;
    app.setOrganizationName(QStringLiteral("MikaiStabilityProbe"));
    app.setApplicationName(QStringLiteral("FullscreenStabilityProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    const QString preset = positionalArguments.size() == 4 ? positionalArguments.at(3)
                                                            : QStringLiteral("off");
    const QString module = qEnvironmentVariable("MIKAI_MPV_MODULE");
    if (module.isEmpty()) {
        if (!mplayer::probes::selectRuntime()) return 2;
    } else {
        const auto runtime = mplayer::native::selectStandardRuntime(module);
        mplayer::probes::detail::reportSelected(runtime, "explicit module");
        if (!runtime.loaded) {
            std::fprintf(stderr, "Runtime refused: %s\n", qPrintable(runtime.reason));
            return 2;
        }
    }

    PlaybackHost host;
    const QString media = positionalArguments.at(0);
    auto* dialog = new mplayer::shell::PlayerDialog(
        QStringLiteral("Fullscreen stability verification"), media,
        {{1080, media}}, 1080, &host);
    auto* surface = dialog->findChild<mplayer::VideoSurface*>();
    auto* shell = dialog->findChild<mplayer::shell::PlayerShell*>();
    if (surface == nullptr || shell == nullptr) return 2;
    shell->applyAnime4KPreset(preset);
    const auto activeSurface = [&]() -> mplayer::VideoSurface* {
        if (!dialog->isPipActive()) return surface;
        for (QWidget* window : QApplication::topLevelWidgets()) {
            auto* pip = qobject_cast<mplayer::shell::PipFloat*>(window);
            if (pip != nullptr && pip->isVisible() && pip->videoSurface() != nullptr)
                return pip->videoSurface();
        }
        return nullptr;
    };
    QString mediaError;
    dialog->startPlayer();
    dialog->showFullScreen();
    dialog->raise();
    dialog->activateWindow();
    QCursor::setPos(dialog->mapToGlobal(QPoint(2, 2)));

    QElapsedTimer clock;
    clock.start();
    qint64 previousTick = 0;
    qint64 maxGap = 0;
    qint64 previousSampleWorkMs = 0;
    qint64 maxSampleWorkMs = 0;
    qint64 maxReadbackMs = 0;
    qint64 maxDesktopGrabMs = 0;
    qint64 lastSummary = 0;
    qint64 lastInteraction = 0;
    qint64 lastScreenshot = -60000;
    qint64 nextPresentationTransition = 30000;
    int presentationPhase = 0;
    quint64 swaps = 0;
    quint64 samples = 0;
    quint64 blackSamples = 0;
    quint64 composedSamples = 0;
    quint64 blackCompositions = 0;
    quint64 missingSamples = 0;
    quint64 missingOwnerSamples = 0;
    quint64 stalledSeconds = 0;
    quint64 slowTicks = 0;
    quint64 previousFrames = 0;
    double previousPosition = 0;
    mplayer::VideoSurface* previousOwner = nullptr;
    mplayer::VideoSurface* lastObservedOwner = nullptr;
    qint64 ownerTransitionAtMs = -1;
    qint64 activeOwnerUnavailableSinceMs = -1;
    QString lastSurfaceId;
    QString lastContextId;
    quint64 lastSourceEpoch = 0;
    quint64 ownerTransitions = 0;
    quint64 contextIdentityChanges = 0;
    quint64 sourceEpochChanges = 0;
    quint64 fullscreenViolations = 0;
    quint64 validMainSamples = 0;
    quint64 validPipSamples = 0;
    quint64 currentPipSamples = 0;
    quint64 completedPipPhases = 0;
    quint64 inactiveSamples = 0;
    qint64 contextAgeStartMs = -1;
    // Cleared when the run finalizes: a composition hook firing in the quit window
    // must not queue artifacts after the final counts are recorded.
    bool collectingArtifacts = true;
    const auto record = [&](QJsonObject row) {
        evidence.write(QJsonDocument(row).toJson(QJsonDocument::Compact));
        evidence.write("\n");
        evidence.flush();
    };
    if (focusSwitch || baseline) {
        QString modeError;
        const int modeExit = focusSwitch
                                 ? runFocusSwitchMode(dialog, surface, shell, knownBright, seconds,
                                                      record, &modeError)
                                 : runBaselineMode(dialog, surface, shell, media, knownBright,
                                                   seconds, record, &modeError);
        evidence.flush();
        dialog->close();
        // A completed run is success even when a metric is unavailable: the row
        // names that metric as unavailable instead of inventing a zero.
        return modeExit == 0 ? 0 : 2;
    }
    EvidenceWrites artifactWrites;
    QThreadPool artifactPool;
    artifactPool.setMaxThreadCount(1);
    const auto artifactFailure = [&] {
        if (artifactWrites.failureCount() == 0) return QString();
        const EvidenceWriteSnapshot snapshot = artifactWrites.snapshot();
        return QStringLiteral("Could not save evidence: %1")
            .arg(snapshot.failedArtifacts.join(QStringLiteral(", ")));
    };
    const auto saveImage = [&](const QImage& image, const QString& name) {
        if (!collectingArtifacts) return;
        const QString path = QDir(outputPath).filePath(name);
        artifactWrites.noteAttempt();
        if (image.isNull()) {
            artifactWrites.noteFinish(false, 0, name);
            return;
        }
        auto* task = new ArtifactWriteTask(image, path, name, &artifactWrites);
        if (artifactPool.tryStart(task)) return;
        // Refuse queued full-screen images; sampled failures still fail independently.
        delete task;
        artifactWrites.noteSkip();
        record({{"event", "artifactSkipped"}, {"elapsedMs", clock.elapsed()},
                {"artifact", name}, {"reason", "evidence writer busy"}});
    };
    const auto desktopImage = [&](QScreen* screen, const QRect& region = QRect()) {
        QElapsedTimer work;
        work.start();
        QImage image = region.isEmpty()
                           ? screen->grabWindow(0).toImage()
                           : screen->grabWindow(0, region.x(), region.y(),
                                                region.width(), region.height()).toImage();
        maxDesktopGrabMs = std::max(maxDesktopGrabMs, work.elapsed());
        return image;
    };
    if (QScreen* screen = dialog->screen()) {
        record({{"event", "initialScreen"}, {"elapsedMs", clock.elapsed()},
                {"screen", screen->name()}, {"width", screen->geometry().width()},
                {"height", screen->geometry().height()}, {"dpr", screen->devicePixelRatio()}});
        QObject::connect(screen, &QScreen::geometryChanged, &app, [&](const QRect& geometry) {
            record({{"event", "screenGeometryChanged"}, {"elapsedMs", clock.elapsed()},
                    {"width", geometry.width()}, {"height", geometry.height()}});
        });
    }
    record({{"event", "runMode"},
            {"mode", steadyFullscreen ? "steady-fullscreen" : "transition"},
            {"durationSeconds", seconds}, {"knownBrightFixture", knownBright}});
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &app,
                     [&](Qt::ApplicationState state) {
        record({{"event", "applicationStateChanged"}, {"elapsedMs", clock.elapsed()},
                {"state", static_cast<int>(state)}});
    });
    QSet<mplayer::VideoSurface*> watchedSurfaces;
    bool compositionCaptureActive = false;
    const auto captureComposition = [&](mplayer::VideoSurface* composedSurface) {
        if (!collectingArtifacts || compositionCaptureActive || clock.elapsed() < 5000 ||
            composedSurface != activeSurface() || !composedSurface->hasVideo()) return;
        compositionCaptureActive = true;
        QElapsedTimer work;
        work.start();
        QOpenGLContext* previous = QOpenGLContext::currentContext();
        QSurface* previousSurface = previous != nullptr ? previous->surface() : nullptr;
        composedSurface->makeCurrent();
        QOpenGLContext* context = QOpenGLContext::currentContext();
        if (context == composedSurface->context()) {
            auto* gl = context->functions();
            GLint framebuffer = 0;
            GLint alignment = 0;
            gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
            gl->glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
            gl->glBindFramebuffer(GL_FRAMEBUFFER, composedSurface->defaultFramebufferObject());
            gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
            const QSize deviceSize =
                composedSurface->size() * composedSurface->devicePixelRatioF();
            QImage image(96, 64, QImage::Format_RGBA8888);
            gl->glReadPixels(deviceSize.width() / 2 - 48, deviceSize.height() / 2 - 32,
                             96, 64, GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
            ++composedSamples;
            if (isBlack(image)) {
                ++blackCompositions;
                if (blackCompositions <= 8)
                    saveImage(image, QStringLiteral("black-composition-%1.png").arg(clock.elapsed()));
            } else if (composedSamples == 1) {
                saveImage(image, QStringLiteral("composed-video.png"));
            }
            gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
            gl->glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        }
        if (previous != nullptr && previousSurface != nullptr)
            previous->makeCurrent(previousSurface);
        else
            composedSurface->doneCurrent();
        maxReadbackMs = std::max(maxReadbackMs, work.elapsed());
        compositionCaptureActive = false;
    };
    const auto watchSurface = [&](mplayer::VideoSurface* candidate) {
        if (candidate == nullptr || watchedSurfaces.contains(candidate)) return;
        watchedSurfaces.insert(candidate);
        QObject::connect(candidate, &mplayer::VideoSurface::errorOccurred, &app,
                         [&](const QString& error) { mediaError = error; });
        QObject::connect(candidate, &QOpenGLWidget::frameSwapped, &app, [&] { ++swaps; });
        QObject::connect(candidate, &QOpenGLWidget::aboutToCompose, &app,
                         [&, candidate] { captureComposition(candidate); }, Qt::DirectConnection);
        QObject::connect(candidate, &QObject::destroyed, &app,
                         [&, candidate] { watchedSurfaces.remove(candidate); });
    };
    watchSurface(surface);
    QTimer sample;
    sample.setTimerType(Qt::PreciseTimer);
    sample.setInterval(32);
    QObject::connect(&sample, &QTimer::timeout, &app, [&] {
        QElapsedTimer work;
        work.start();
        const qint64 now = clock.elapsed();
        if (!steadyFullscreen && now >= nextPresentationTransition) {
            if (presentationPhase == 0) {
                dialog->showNormal();
                presentationPhase = 1;
                nextPresentationTransition = now + 4000;
            } else if (presentationPhase == 1) {
                dialog->showFullScreen();
                presentationPhase = 2;
                nextPresentationTransition = now + 26000;
            } else if (presentationPhase == 2) {
                currentPipSamples = 0;
                dialog->togglePip();
                presentationPhase = 3;
                nextPresentationTransition = now + 5000;
            } else {
                if (currentPipSamples < 2)
                    mediaError = QStringLiteral("PiP did not produce two advancing playback samples");
                ++completedPipPhases;
                dialog->togglePip();
                dialog->showFullScreen();
                presentationPhase = 0;
                nextPresentationTransition = now + 30000;
            }
            auto* transitionedOwner = activeSurface();
            watchSurface(transitionedOwner);
            record({{"event", "presentationTransition"}, {"elapsedMs", now},
                    {"phase", presentationPhase}, {"dialogVisible", dialog->isVisible()},
                    {"fullscreen", dialog->isFullScreen()}, {"pipActive", dialog->isPipActive()},
                    {"hasVideo", transitionedOwner != nullptr && transitionedOwner->hasVideo()},
                    {"activePosition", transitionedOwner != nullptr
                                           ? transitionedOwner->position() : -1.0},
                    {"activePlaying", transitionedOwner != nullptr &&
                                          transitionedOwner->isPlaying()},
                    {"activeFrames", static_cast<qint64>(
                                         transitionedOwner != nullptr
                                             ? transitionedOwner->playbackDiagnostics()
                                                   .renderedOutputFrames : 0)}});
        }
        auto* owner = activeSurface();
        watchSurface(owner);
        if (owner != lastObservedOwner) {
            lastObservedOwner = owner;
            ownerTransitionAtMs = now;
            activeOwnerUnavailableSinceMs =
                owner != nullptr && owner->hasVideo() ? -1 : now;
        } else if (owner != nullptr && owner->hasVideo()) {
            if (activeOwnerUnavailableSinceMs >= 0) ownerTransitionAtMs = now;
            activeOwnerUnavailableSinceMs = -1;
        } else if (activeOwnerUnavailableSinceMs < 0) {
            activeOwnerUnavailableSinceMs = now;
        }
        bool foreground = dialog->isActiveWindow() &&
                          QGuiApplication::applicationState() == Qt::ApplicationActive;
#ifdef Q_OS_WIN
        foreground = foreground &&
                     GetForegroundWindow() == reinterpret_cast<HWND>(dialog->winId());
#endif
        if (steadyFullscreen && now > 5000 && !foreground) {
            ++inactiveSamples;
            QJsonObject foregroundEvent{{"event", "foregroundLost"}, {"elapsedMs", now}};
#ifdef Q_OS_WIN
            const HWND foregroundWindow = GetForegroundWindow();
            DWORD foregroundProcessId = 0;
            GetWindowThreadProcessId(foregroundWindow, &foregroundProcessId);
            QString foregroundImage = QStringLiteral("unavailable");
            if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                             foregroundProcessId)) {
                wchar_t imagePath[1024]{};
                DWORD imagePathSize = 1024;
                if (QueryFullProcessImageNameW(process, 0, imagePath, &imagePathSize))
                    foregroundImage = QString::fromWCharArray(imagePath, imagePathSize)
                                          .section(QLatin1Char('\\'), -1);
                CloseHandle(process);
            }
            foregroundEvent.insert(QStringLiteral("foregroundProcessId"),
                                   static_cast<qint64>(foregroundProcessId));
            foregroundEvent.insert(QStringLiteral("foregroundImage"), foregroundImage);
            foregroundEvent.insert(QStringLiteral("foregroundHandle"),
                                   pointerId(foregroundWindow));
#endif
            record(foregroundEvent);
            if (mediaError.isEmpty())
                mediaError = QStringLiteral("Steady fullscreen is no longer foreground");
        }
        const qint64 gap = previousTick == 0 ? 0 : now - previousTick;
        previousTick = now;
        if (now > 5000) {
            maxGap = std::max(maxGap, gap);
            slowTicks += gap > 250;
            if (gap > 250) {
                record({{"event", "guiGap"}, {"elapsedMs", now}, {"gapMs", gap},
                        {"previousSampleWorkMs", previousSampleWorkMs},
                        {"maxReadbackMs", maxReadbackMs}, {"maxDesktopGrabMs", maxDesktopGrabMs},
                        {"maxImageSaveMs", artifactWrites.maxWriteMs()}});
            }
        }
        if (now > 5000) {
            if (owner == nullptr || !owner->hasVideo()) {
                ++missingOwnerSamples;
            } else {
                const QPoint center = owner->mapToGlobal(owner->rect().center());
                QScreen* screen = owner->screen();
                const QImage image =
                    desktopImage(screen, QRect(center - QPoint(48, 32), QSize(96, 64)));
                ++samples;
                missingSamples += image.isNull();
                if (isBlack(image)) {
                    ++blackSamples;
                    if (blackSamples <= 8) {
                        saveImage(desktopImage(screen), QStringLiteral("black-%1.png").arg(now));
                        record({{"blackAtMs", now}, {"position", owner->position()},
                                {"pipActive", dialog->isPipActive()},
                                {"knownBrightFixture", knownBright},
                                {"applicationActive", QGuiApplication::applicationState() ==
                                                          Qt::ApplicationActive},
                                {"windowActive", dialog->isActiveWindow()},
                                {"focusException", !dialog->isActiveWindow() ||
                                                       QGuiApplication::applicationState() !=
                                                           Qt::ApplicationActive}});
                    }
                }
            }
        }
        if (now - lastSummary >= 1000) {
            const auto mainDiagnostics = surface->playbackDiagnostics();
            const auto ownerDiagnostics =
                owner != nullptr ? owner->playbackDiagnostics() : mplayer::PlaybackDiagnostics{};
            const quint64 ownerFrames = ownerDiagnostics.renderedOutputFrames;
            const double ownerPosition = owner != nullptr ? owner->position() : -1.0;
            const QString ownerId = pointerId(owner);
            const QString contextId = pointerId(owner != nullptr ? owner->context() : nullptr);
            const quint64 ownerSourceEpoch = owner != nullptr ? owner->sourceEpoch() : 0;
            if (owner != nullptr && owner->hasVideo()) {
                if (!lastSurfaceId.isEmpty() && ownerId != lastSurfaceId) ++ownerTransitions;
                lastSurfaceId = ownerId;
                if (owner->context() != nullptr) {
                    if (!lastContextId.isEmpty() && contextId != lastContextId) {
                        ++contextIdentityChanges;
                        contextAgeStartMs = now;
                    } else if (lastContextId.isEmpty()) {
                        contextAgeStartMs = now;
                    }
                    lastContextId = contextId;
                }
                if (lastSourceEpoch != 0 && ownerSourceEpoch != lastSourceEpoch)
                    ++sourceEpochChanges;
                lastSourceEpoch = ownerSourceEpoch;
            }
            if (steadyFullscreen && !dialog->isFullScreen()) ++fullscreenViolations;
            const qint64 contextAgeMs =
                contextAgeStartMs >= 0 ? now - contextAgeStartMs : -1;
            if (owner != previousOwner) {
                previousOwner = owner;
            } else if (owner != nullptr && owner->hasVideo() && owner->isPlaying() &&
                       ownerFrames > previousFrames && ownerPosition > previousPosition) {
                if (owner == surface) {
                    ++validMainSamples;
                } else {
                    ++validPipSamples;
                    ++currentPipSamples;
                }
            } else if (now > 5000 && ownerTransitionAtMs >= 0 &&
                       now - ownerTransitionAtMs > 1500 &&
                       (owner == nullptr ? dialog->hasActiveSource()
                                         : !owner->hasVideo() || !owner->isPlaying() ||
                                               ownerFrames == previousFrames ||
                                               ownerPosition <= previousPosition)) {
                ++stalledSeconds;
            }
            previousFrames = ownerFrames;
            previousPosition = ownerPosition;
            record({{"elapsedMs", now}, {"position", ownerPosition},
                    {"frames", static_cast<qint64>(ownerFrames)},
                    {"mainPosition", surface->position()},
                    {"mainFrames", static_cast<qint64>(
                                       mainDiagnostics.renderedOutputFrames)},
                    {"mainPlaying", surface->isPlaying()},
                    {"activeSurfaceFound", owner != nullptr},
                    {"activeSurfaceIsPip", owner != nullptr && owner != surface},
                    {"activePlaying", owner != nullptr && owner->isPlaying()},
                    {"activeHasVideo", owner != nullptr && owner->hasVideo()},
                    {"activePosition", ownerPosition},
                    {"activeFrames", static_cast<qint64>(ownerFrames)},
                    {"mode", steadyFullscreen ? "steady-fullscreen" : "transition"},
                    {"steadyFullscreen", steadyFullscreen},
                    {"activeSurfaceId", ownerId}, {"activeGlContextId", contextId},
                    {"activeSourceEpoch", static_cast<qint64>(ownerSourceEpoch)},
                    {"contextAgeMs", contextAgeMs},
                    {"ownerTransitions", static_cast<qint64>(ownerTransitions)},
                    {"contextIdentityChanges",
                     static_cast<qint64>(contextIdentityChanges)},
                    {"sourceEpochChanges", static_cast<qint64>(sourceEpochChanges)},
                    {"fullscreenViolations", static_cast<qint64>(fullscreenViolations)},
                    {"swaps", static_cast<qint64>(swaps)}, {"maxGuiGapMs", maxGap},
                    {"applicationActive", QGuiApplication::applicationState() ==
                                              Qt::ApplicationActive},
                    {"windowActive", dialog->isActiveWindow()},
                    {"windowFocused", dialog->isActiveWindow() && dialog->hasFocus()},
                    {"pipActive", dialog->isPipActive()},
                    {"knownBrightFixture", knownBright},
                    {"blackSamples", static_cast<qint64>(blackSamples)},
                    {"compositions", static_cast<qint64>(composedSamples)},
                    {"blackCompositions", static_cast<qint64>(blackCompositions)},
                    {"missingOwnerSamples", static_cast<qint64>(missingOwnerSamples)},
                    {"validMainSamples", static_cast<qint64>(validMainSamples)},
                    {"validPipSamples", static_cast<qint64>(validPipSamples)},
                    {"inactiveSamples", static_cast<qint64>(inactiveSamples)},
                    {"foreground", foreground},
                    {"residentBytes", residentBytes()}, {"playing", owner != nullptr &&
                                                                        owner->isPlaying()},
                    {"fullscreen", dialog->isFullScreen()}, {"error", mediaError}});
            lastSummary = now;
        }
        if (now > 10000 && now - lastInteraction >= 17000) {
            QTest::mouseMove(shell, QPoint(shell->width() / 2, shell->height() / 2));
            QTest::mouseMove(shell, QPoint(2, 2));
            lastInteraction = now;
        }
        if (now > 5000 && now - lastScreenshot >= 60000) {
            QScreen* screen = owner != nullptr ? owner->screen() : dialog->screen();
            saveImage(desktopImage(screen), QStringLiteral("fullscreen-%1.png").arg(now));
            lastScreenshot = now;
        }
        if (activeOwnerUnavailableSinceMs >= 0 &&
            now - activeOwnerUnavailableSinceMs > 15000 && mediaError.isEmpty()) {
            mediaError = QStringLiteral("No active playback surface/video after 15s wait");
        }
        previousSampleWorkMs = work.elapsed();
        maxSampleWorkMs = std::max(maxSampleWorkMs, previousSampleWorkMs);
        if (mediaError.isEmpty()) mediaError = artifactFailure();
        if (now < seconds * 1000LL && mediaError.isEmpty() && !host.failed) return;
        // Freeze collection before the join below: frames still compose while the
        // dialog closes, and their artifacts would land after the reported counts.
        collectingArtifacts = false;
        sample.stop();
        // Join evidence writes after sampling so PNG work cannot distort GUI-gap measurements.
        QElapsedTimer settle;
        settle.start();
        const bool artifactWritesSettled = artifactPool.waitForDone(15000);
        const qint64 artifactSettleMs = settle.elapsed();
        const EvidenceWriteSnapshot artifactSnapshot = artifactWrites.snapshot();
        if (!artifactWritesSettled && mediaError.isEmpty())
            mediaError = QStringLiteral("Evidence artifacts were still being written after 15s");
        if (artifactSnapshot.failed > 0 && mediaError.isEmpty())
            mediaError = QStringLiteral("Could not save evidence: %1")
                             .arg(artifactSnapshot.failedArtifacts.join(QStringLiteral(", ")));
        auto* finalOwner = activeSurface();
        const auto coreShaders =
            finalOwner != nullptr
                ? finalOwner->probeMpvProperty(QStringLiteral("glsl-shaders"),
                                              mplayer::ProbePropertyKind::Text)
                : std::optional<mplayer::ProbePropertyValue>{};
        const auto coreTransfer =
            finalOwner != nullptr
                ? finalOwner->probeMpvProperty(QStringLiteral("video-params/gamma"),
                                              mplayer::ProbePropertyKind::Text)
                : std::optional<mplayer::ProbePropertyValue>{};
        const QString expectedShaders = mplayer::anime4k::shaderStringForCandidate(preset);
        const bool shaderFilesComplete =
            preset == QLatin1String("off") ||
            mplayer::anime4k::chainIsComplete(mplayer::anime4k::candidateForId(preset));
        const bool shaderStateVerified =
            coreShaders && coreShaders->kind == mplayer::ProbePropertyValue::Kind::Text &&
            coreShaders->text == expectedShaders && shaderFilesComplete;
        if (!shaderStateVerified && mediaError.isEmpty())
            mediaError = QStringLiteral("The core shader list does not match preset '%1'").arg(preset);
        const QString finalSurfaceId = pointerId(finalOwner);
        const QString finalContextId =
            pointerId(finalOwner != nullptr ? finalOwner->context() : nullptr);
        const quint64 finalSourceEpoch =
            finalOwner != nullptr ? finalOwner->sourceEpoch() : 0;
        const bool stableFullscreenOwnership =
            !steadyFullscreen ||
            (ownerTransitions == 0 && contextIdentityChanges == 0 &&
             sourceEpochChanges == 0 && fullscreenViolations == 0 &&
             (lastSurfaceId.isEmpty() || finalSurfaceId == lastSurfaceId) &&
             (lastContextId.isEmpty() || finalContextId == lastContextId) &&
             (lastSourceEpoch == 0 || finalSourceEpoch == lastSourceEpoch));
        const bool passed = mediaError.isEmpty() && !host.failed && finalOwner != nullptr &&
            finalOwner->hasVideo() && finalOwner->position() >= seconds - 5 &&
            composedSamples > 0 && (!knownBright || blackCompositions == 0) &&
            stalledSeconds == 0 && maxGap < 1000 && dialog->isFullScreen() &&
            finalOwner->anime4KChainId() == preset && stableFullscreenOwnership &&
            validMainSamples >= 2 &&
            (steadyFullscreen
                 ? foreground && inactiveSamples == 0 && samples > 0 &&
                       missingSamples == 0 && (!knownBright || blackSamples == 0)
                 : (seconds < 66 || (completedPipPhases > 0 && validPipSamples >= 2)));
        record({{"result", passed ? "PASS" : "FAIL"}, {"elapsedMs", now},
                {"requestedPreset", preset},
                {"appliedPreset", finalOwner != nullptr ? finalOwner->anime4KChainId()
                                                        : QStringLiteral("unavailable")},
                {"coreGlslShadersAvailable", coreShaders.has_value()},
                {"coreGlslShaders", coreShaders ? coreShaders->text : QString()},
                {"expectedGlslShaders", expectedShaders},
                {"shaderFilesComplete", shaderFilesComplete},
                {"shaderStateVerified", shaderStateVerified},
                {"coreSourceTransferAvailable", coreTransfer.has_value()},
                {"coreSourceTransfer", coreTransfer ? coreTransfer->text : QString()},
                {"activeOwnerFoundAtFinish", finalOwner != nullptr},
                {"activeOwnerIsPipAtFinish", finalOwner != nullptr && finalOwner != surface},
                {"activePositionAtFinish",
                 finalOwner != nullptr ? finalOwner->position() : -1.0},
                {"mode", steadyFullscreen ? "steady-fullscreen" : "transition"},
                {"steadyFullscreen", steadyFullscreen},
                {"finalSurfaceId", finalSurfaceId},
                {"finalGlContextId", finalContextId},
                {"finalSourceEpoch", static_cast<qint64>(finalSourceEpoch)},
                {"contextAgeMs", contextAgeStartMs >= 0 ? now - contextAgeStartMs : -1},
                {"ownerTransitions", static_cast<qint64>(ownerTransitions)},
                {"contextIdentityChanges",
                 static_cast<qint64>(contextIdentityChanges)},
                {"sourceEpochChanges", static_cast<qint64>(sourceEpochChanges)},
                {"fullscreenViolations", static_cast<qint64>(fullscreenViolations)},
                {"stableFullscreenOwnership", stableFullscreenOwnership},
                {"knownBrightFixture", knownBright},
                {"blackFramesEvaluated", knownBright},
                {"applicationActiveAtFinish", QGuiApplication::applicationState() ==
                                                  Qt::ApplicationActive},
                {"windowActiveAtFinish", dialog->isActiveWindow()},
                {"windowFocusedAtFinish", dialog->isActiveWindow() && dialog->hasFocus()},
                {"samples", static_cast<qint64>(samples)},
                {"blackSamples", static_cast<qint64>(blackSamples)},
                {"missingSamples", static_cast<qint64>(missingSamples)},
                {"missingOwnerSamples", static_cast<qint64>(missingOwnerSamples)},
                {"validMainSamples", static_cast<qint64>(validMainSamples)},
                {"validPipSamples", static_cast<qint64>(validPipSamples)},
                {"completedPipPhases", static_cast<qint64>(completedPipPhases)},
                {"inactiveSamples", static_cast<qint64>(inactiveSamples)},
                {"foregroundAtFinish", foreground},
                {"compositions", static_cast<qint64>(composedSamples)},
                {"blackCompositions", static_cast<qint64>(blackCompositions)},
                {"desktopCaptureAvailable", samples > 0 && blackSamples < samples},
                {"stalledSeconds", static_cast<qint64>(stalledSeconds)},
                {"slowTicks", static_cast<qint64>(slowTicks)}, {"maxGuiGapMs", maxGap},
                {"maxSampleWorkMs", maxSampleWorkMs}, {"maxReadbackMs", maxReadbackMs},
                {"maxDesktopGrabMs", maxDesktopGrabMs},
                {"maxImageSaveMs", artifactSnapshot.maxWriteMs},
                {"artifactWritesAttempted", artifactSnapshot.attempted},
                {"artifactWritesCompleted", artifactSnapshot.completed},
                {"artifactWritesFailed", artifactSnapshot.failed},
                {"artifactWritesPending", artifactSnapshot.pending()},
                {"artifactWritesSettled", artifactWritesSettled},
                {"artifactSettleMs", artifactSettleMs},
                {"skippedArtifacts", artifactSnapshot.skipped},
                {"failedArtifacts", QJsonArray::fromStringList(artifactSnapshot.failedArtifacts)},
                {"error", mediaError}});
        std::fprintf(stderr, "fullscreen stability: %s, elapsed=%lldms compositions=%llu black=%llu stalled=%llu maxGuiGap=%lldms\n",
                     passed ? "PASS" : "FAIL", static_cast<long long>(now),
                     static_cast<unsigned long long>(composedSamples), static_cast<unsigned long long>(blackCompositions),
                     static_cast<unsigned long long>(stalledSeconds), static_cast<long long>(maxGap));
        dialog->close();
        QTimer::singleShot(500, &app, [&app, passed] { app.exit(passed ? 0 : 1); });
    });
    sample.start();
    return app.exec();
}
