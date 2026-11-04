#include "Anime4K.hpp"
#include "PlayerDialog.hpp"
#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
#include "ProbeIsolation.hpp"
#include "ShellSliders.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSurface.hpp"

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

bool foregroundIsOurProcess() {
    return QGuiApplication::applicationState() == Qt::ApplicationActive;
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

/// A timeout is not "no measurement": it is at least this long. Every latency
/// quantile includes it as a lower bound so a worst cycle cannot vanish.
constexpr qint64 kVisibleCompositionDeadlineMs = 2500;

QRect videoSampleCrop(mplayer::VideoSurface* surface) {
    if (surface == nullptr) return QRect();
    const QPoint point = surface->mapToGlobal(
        QPoint(surface->width() * 28 / 100, surface->height() * 33 / 100));
    return QRect(point - QPoint(48, 32), QSize(96, 64));
}

/// Launch-scoped readiness: the first away interval can find the peer behind
/// the fullscreen player even though it owns the foreground. Settle z-order and
/// verify the black window actually occludes the sample crop before measuring.
bool settleForegroundPeer(mikai::probes::ForegroundPeer& peer,
                          mplayer::shell::PlayerDialog* dialog,
                          mplayer::VideoSurface* surface, const QRect& crop,
                          const std::function<void(QJsonObject)>& record,
                          QString* error) {
    QScreen* const screen = surface != nullptr ? surface->screen() : nullptr;
    const double baselineLuma = meanLuma(desktopCrop(screen, crop));
    int attempts = 0;
    bool occluded = false;
    double occludedLuma = -1.0;
    for (; attempts < 10; ++attempts) {
        peer.activate();
        peer.raiseToFront();
        spinFor(250);
        if (!peer.ownsForeground()) continue;
        occludedLuma = meanLuma(desktopCrop(screen, crop));
        if (baselineLuma >= 40.0 && occludedLuma >= 0.0 &&
            occludedLuma < baselineLuma * 0.5) {
            occluded = true;
            break;
        }
    }
    record(QJsonObject{{"event", "foregroundPeerWarmup"},
                       {"baselineLuma", baselineLuma},
                       {"occludedLuma", occludedLuma},
                       {"attempts", attempts + 1},
                       {"occluded", occluded}});
    if (!occluded) {
        *error = QStringLiteral("the foreign helper never occluded the sample crop "
                                "(baseline=%1 occluded=%2)")
                     .arg(baselineLuma, 0, 'f', 1)
                     .arg(occludedLuma, 0, 'f', 1);
        return false;
    }
    // Restore the pre-measurement state: player foreground and picture visible.
    forceForegroundNow(dialog);
    for (int attempt = 0; attempt < 40 && !foregroundIs(dialog); ++attempt) {
        spinFor(50);
        forceForegroundNow(dialog);
    }
    QElapsedTimer restore;
    restore.start();
    while (restore.elapsed() < 2000 &&
           meanLuma(desktopCrop(screen, crop)) < baselineLuma * 0.6) {
        spinFor(20);
    }
    return foregroundIs(dialog);
}

/// The surface that is compositing the picture right now: the main surface,
/// or the PiP float's surface while the float is up. A matrix sample attributed
/// to the wrong surface would report the floating window's pixels as a
/// main-window black frame (or the reverse).
mplayer::VideoSurface* activePlaybackSurface(mplayer::shell::PlayerDialog* dialog,
                                             mplayer::VideoSurface* fallback) {
    if (dialog == nullptr) return nullptr;
    if (!dialog->isPipActive()) return fallback;
    for (QWidget* window : QApplication::topLevelWidgets()) {
        auto* pip = qobject_cast<mplayer::shell::PipFloat*>(window);
        if (pip != nullptr && pip->isVisible() && pip->videoSurface() != nullptr)
            return pip->videoSurface();
    }
    return nullptr;
}

/// Fraction of pixels whose RGB sum moved by more than 24 between two composed
/// crops of the same surface. Not a frame counter: it is the picture itself
/// changing, which is what "the video output is still moving" has to mean.
double frameDifferenceFraction(const QImage& first, const QImage& second) {
    if (first.isNull() || second.isNull() || first.size() != second.size()) return -1.0;
    const QImage a = first.convertToFormat(QImage::Format_RGBA8888);
    const QImage b = second.convertToFormat(QImage::Format_RGBA8888);
    qint64 different = 0;
    qint64 total = 0;
    for (int y = 0; y < a.height(); ++y) {
        const uchar* rowA = a.constScanLine(y);
        const uchar* rowB = b.constScanLine(y);
        for (int x = 0; x < a.width(); ++x) {
            const int offset = x * 4;
            const int delta = std::abs(int(rowA[offset]) - int(rowB[offset])) +
                              std::abs(int(rowA[offset + 1]) - int(rowB[offset + 1])) +
                              std::abs(int(rowA[offset + 2]) - int(rowB[offset + 2]));
            if (delta > 24) ++different;
            ++total;
        }
    }
    return total > 0 ? static_cast<double>(different) / total : -1.0;
}

/// Why a matrix tick produced no picture. An excluded tick is counted under its
/// own reason and is never evaluated for black: the black helper's window, a
/// minimized player and another application's foreground are not this player's
/// picture, so treating their pixels as either "black" or "clean" would be
/// fabricated evidence in both directions.
enum class MatrixExposure { Exposed, PeerForeground, Minimized, NotExposed, ForeignForeground };

struct MatrixSampler {
    mplayer::shell::PlayerDialog* dialog = nullptr;
    mplayer::VideoSurface* mainSurface = nullptr;
    const mikai::probes::ForegroundPeer* peer = nullptr;
    std::function<void(QJsonObject)> record;
    std::function<void(const QImage&, const QString&)> saveImage;

    qint64 attempts = 0;
    qint64 exposedSamples = 0;
    qint64 fboSamples = 0;
    qint64 fboBlack = 0;
    qint64 fboUnmeasured = 0;
    qint64 desktopSamples = 0;
    qint64 desktopBlack = 0;
    qint64 desktopUnmeasured = 0;
    qint64 excludedPeerForeground = 0;
    qint64 excludedMinimized = 0;
    qint64 excludedNotExposed = 0;
    qint64 excludedForeignForeground = 0;
    qint64 excludedNoVisibleOutputFrame = 0;
    qint64 grabCalls = 0;
    qint64 grabTotalMicros = 0;
    qint64 grabMaxMicros = -1;
    qint64 readbackCalls = 0;
    qint64 readbackTotalMicros = 0;
    qint64 readbackMaxMicros = -1;
    qint64 maxWorkMs = 0;
    /// Work of the most recent tick. The GUI heartbeat reads it so a gap can be
    /// told apart from the probe's own readback/grab work.
    qint64 lastWorkMs = -1;
    qint64 firstSampleAtMs = -1;
    qint64 lastSampleAtMs = -1;
    double desktopLumaTotal = 0.0;
    qint64 desktopLumaCount = 0;
    int blackArtifactsSaved = 0;

    quint64 contextGenerationStart = 0;
    quint64 contextGenerationLast = 0;
    quint64 contextGenerationChanges = 0;
    quint64 initializeGlStart = 0;
    quint64 initializeGlLast = 0;
    bool contextObserved = false;
    quint64 sourceEpochStart = 0;
    quint64 sourceEpochLast = 0;
    quint64 sourceEpochChanges = 0;
    bool sourceEpochObserved = false;

    /// `composed` is the same tick's framebuffer crop. It is written beside the
    /// desktop crop so the receipt shows whether a black desktop rectangle was
    /// a painted picture the compositor had not carried yet, or a rectangle the
    /// active surface had no picture in at all.
    void noteBlack(const QImage& image, const QString& label, qint64 elapsedMs,
                   const QJsonObject& context, const QImage& composed = QImage()) {
        if (!saveImage || blackArtifactsSaved >= 8) return;
        ++blackArtifactsSaved;
        saveImage(image, QStringLiteral("%1-%2.png").arg(label).arg(elapsedMs));
        if (!composed.isNull()) {
            saveImage(composed,
                      QStringLiteral("%1-fbo-%2.png").arg(label).arg(elapsedMs));
        }
        if (record) {
            QJsonObject row = context;
            row.insert(QStringLiteral("event"), QStringLiteral("matrixBlackSample"));
            row.insert(QStringLiteral("elapsedMs"), elapsedMs);
            row.insert(QStringLiteral("kind"), label);
            row.insert(QStringLiteral("composedCropSaved"), !composed.isNull());
            record(row);
        }
    }

    /// Takes one tick. `elapsedMs` is the run clock, recorded so every count can
    /// be placed on the timeline.
    void tick(qint64 elapsedMs) {
        QElapsedTimer work;
        work.start();
        ++attempts;
        mplayer::VideoSurface* owner = activePlaybackSurface(dialog, mainSurface);
        const bool minimized = dialog != nullptr && dialog->isMinimized();
        const bool peerOwns =
            peer != nullptr && peer->supported() && peer->ownsForeground();
        MatrixExposure exposure = MatrixExposure::Exposed;
        if (minimized) {
            exposure = MatrixExposure::Minimized;
        } else if (peerOwns) {
            exposure = MatrixExposure::PeerForeground;
        } else if (!foregroundIsOurProcess()) {
            exposure = MatrixExposure::ForeignForeground;
        } else if (owner == nullptr || owner->context() == nullptr || !owner->isVisible() ||
                   !(dialog->isVisible() || dialog->isPipActive())) {
            // A surface without a render context has no composed picture to
            // judge; it is counted as an exclusion, never as a black frame.
            // The dialog deliberately hides itself while the float is up, so
            // `isPipActive()` - not dialog visibility - is what says the player
            // still owns a running picture there.
            exposure = MatrixExposure::NotExposed;
        }
        switch (exposure) {
            case MatrixExposure::Exposed: break;
            case MatrixExposure::PeerForeground: ++excludedPeerForeground; break;
            case MatrixExposure::Minimized: ++excludedMinimized; break;
            case MatrixExposure::NotExposed: ++excludedNotExposed; break;
            case MatrixExposure::ForeignForeground: ++excludedForeignForeground; break;
        }
        if (exposure != MatrixExposure::Exposed) {
            lastWorkMs = work.elapsed();
            maxWorkMs = std::max(maxWorkMs, lastWorkMs);
            return;
        }
        ++exposedSamples;
        if (firstSampleAtMs < 0) firstSampleAtMs = elapsedMs;
        lastSampleAtMs = elapsedMs;

        const mplayer::VideoSurface::UiProfileCounters counters =
            mainSurface != nullptr ? mainSurface->uiProfileCounters()
                                   : mplayer::VideoSurface::UiProfileCounters{};
        if (!contextObserved) {
            contextGenerationStart = contextGenerationLast = counters.contextGeneration;
            initializeGlStart = initializeGlLast = counters.initializeGlCalls;
            contextObserved = true;
        } else if (counters.contextGeneration != contextGenerationLast) {
            ++contextGenerationChanges;
            contextGenerationLast = counters.contextGeneration;
        }
        initializeGlLast = counters.initializeGlCalls;

        const quint64 epoch = owner->sourceEpoch();
        if (!sourceEpochObserved) {
            sourceEpochStart = sourceEpochLast = epoch;
            sourceEpochObserved = true;
        } else if (epoch != sourceEpochLast) {
            ++sourceEpochChanges;
            sourceEpochLast = epoch;
        }

        // What the composed half of this same tick showed. A black desktop crop
        // means one of two very different things, and the receipt has to say
        // which: the surface had a picture the compositor had not carried yet,
        // or the surface had no picture and the crop was a measuring artifact.
        QString fboVerdict = QStringLiteral("not-claimed");
        QImage fboCrop;
        // Only a surface that claims a visible output frame is asked for its
        // composed pixels. Before that claim there is no picture to judge.
        if (!owner->hasVisibleOutputFrame()) {
            ++excludedNoVisibleOutputFrame;
        } else {
            QElapsedTimer readback;
            readback.start();
            const QImage composed = readComposedCrop(owner);
            const qint64 elapsedMicros = readback.nsecsElapsed() / 1000;
            ++readbackCalls;
            readbackTotalMicros += elapsedMicros;
            readbackMaxMicros = std::max(readbackMaxMicros, elapsedMicros);
            if (composed.isNull()) {
                ++fboUnmeasured;
                fboVerdict = QStringLiteral("unmeasured");
            } else {
                ++fboSamples;
                fboCrop = composed;
                fboVerdict = isBlack(composed) ? QStringLiteral("black")
                                               : QStringLiteral("bright");
                if (isBlack(composed)) {
                    ++fboBlack;
                    noteBlack(composed, QStringLiteral("qa-matrix-black-fbo"), elapsedMs,
                              QJsonObject{{"surface", "main-or-pip"},
                                          {"contextGeneration",
                                           static_cast<qint64>(counters.contextGeneration)},
                                          {"sourceEpoch", static_cast<qint64>(epoch)},
                                          {"position", owner->position()}});
                }
            }
        }

        QElapsedTimer grab;
        grab.start();
        const QRect sampleRect = videoSampleCrop(owner);
        const QImage desktop = desktopCrop(owner->screen(), sampleRect);
        const qint64 elapsedMicros = grab.nsecsElapsed() / 1000;
        ++grabCalls;
        grabTotalMicros += elapsedMicros;
        grabMaxMicros = std::max(grabMaxMicros, elapsedMicros);
        if (desktop.isNull()) {
            ++desktopUnmeasured;
        } else {
            ++desktopSamples;
            const double luma = meanLuma(desktop);
            if (luma >= 0.0) {
                desktopLumaTotal += luma;
                ++desktopLumaCount;
            }
            if (isBlack(desktop)) {
                ++desktopBlack;
                QScreen* screen = owner->screen();
                const QPoint cropOrigin = sampleRect.topLeft();
                noteBlack(
                    desktop, QStringLiteral("qa-matrix-black-desktop"), elapsedMs,
                    QJsonObject{
                        {"luma", luma},
                        {"position", owner->position()},
                        {"contextGeneration",
                         static_cast<qint64>(counters.contextGeneration)},
                        // Everything below is attribution, not a verdict: it
                        // says which window the rectangle belongs to, what the
                        // same tick's framebuffer held, and what the surface
                        // geometry made the crop mean.
                        {"owner", owner == mainSurface ? QStringLiteral("main")
                                                       : QStringLiteral("pip")},
                        {"cropRect",
                         QJsonArray{cropOrigin.x(), cropOrigin.y(), sampleRect.width(),
                                    sampleRect.height()}},
                        {"surfaceSize",
                         QJsonArray{owner->width(), owner->height()}},
                        {"surfaceDevicePixelRatio", owner->devicePixelRatioF()},
                        {"surfaceIsVisible", owner->isVisible()},
                        {"hasVisibleOutputFrame", owner->hasVisibleOutputFrame()},
                        {"fboVerdictThisTick", fboVerdict},
                        {"dialogVisible", dialog != nullptr && dialog->isVisible()},
                        {"dialogMinimized", dialog != nullptr && dialog->isMinimized()},
                        {"dialogFullScreen", dialog != nullptr && dialog->isFullScreen()},
                        {"pipActive", dialog != nullptr && dialog->isPipActive()},
                        {"screen",
                         screen != nullptr ? screen->name() : QStringLiteral("none")},
                        {"screenGeometry",
                         screen != nullptr
                             ? QJsonArray{screen->geometry().x(), screen->geometry().y(),
                                          screen->geometry().width(),
                                          screen->geometry().height()}
                             : QJsonArray{}},
                        {"screenDevicePixelRatio",
                         screen != nullptr ? screen->devicePixelRatio() : -1.0}},
                    fboCrop);
            }
        }
        lastWorkMs = work.elapsed();
        maxWorkMs = std::max(maxWorkMs, lastWorkMs);
    }
};

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
    /// Observed values plus the deadline for each verified-occlusion timeout.
    QVector<qint64> activationToVisibleCompositionLowerBoundMs;
    int visibleCompositionObserved = 0;
    int visibleCompositionUnsupported = 0;
    int visibleCompositionTimeouts = 0;
    qint64 desktopGrabCalls = 0;
    qint64 desktopGrabTotalMicros = 0;
    qint64 desktopGrabMaxMicros = 0;
    /// Position (media seconds) gained across each occlusion: a cycle that lost
    /// the playhead is not a successful away/back, whatever the foreground said.
    QVector<double> positionDeltaSeconds;
    int positionAdvancedCycles = 0;
    int positionStalledCycles = 0;
    /// Composed-picture luma before the helper covered it and after the player
    /// took the picture back. A recreated render context has to show a bright
    /// picture again; a black or absent frame is a lost picture, not a redraw.
    QVector<double> fboLumaBeforeAway;
    QVector<double> fboLumaAfterBack;
    /// Desktop grabs taken while the peer owned the foreground: the helper's own
    /// pixels, counted here and excluded from the player's black accounting.
    int peerForegroundDesktopSamples = 0;
    int peerForegroundBlackDesktopSamples = 0;
};

ActivationRun runActivationCycles(mplayer::shell::PlayerDialog* dialog,
                                  mplayer::VideoSurface* surface,
                                  mikai::probes::ForegroundPeer& peer, const QString& phase,
                                  int cycles, bool knownBright,
                                  const std::function<void(QJsonObject)>& record,
                                  const std::function<void(int)>& idleWait = {}) {
    ActivationRun run;
    run.phase = phase;
    QScreen* const screen = surface->screen();
    const QRect cropRect = videoSampleCrop(surface);
    // A caller that samples (the QA matrix) passes its pump here, so the fixed
    // settles are covered by the same cadence. The latency poll below stays on
    // spinFor: sampling inside it would change the interval it measures.
    const auto settleFor = [&](int milliseconds) {
        if (idleWait) {
            idleWait(milliseconds);
        } else {
            spinFor(milliseconds);
        }
    };
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
        // Picture and playhead as this cycle finds them: the away interval has to
        // keep playing, and the picture it returns with has to be the same picture
        // (not a re-initialised, never-rendered target) when the context was
        // recreated underneath the cycle.
        const QImage pictureBeforeAway = readComposedCrop(surface);
        run.fboLumaBeforeAway.append(meanLuma(pictureBeforeAway));
        const double positionBeforeAway = surface->position();

        const bool awayOwned = peer.activate();
        settleFor(650);
        const bool awayQtInactive =
            QGuiApplication::applicationState() == Qt::ApplicationInactive;
        const bool stillAway = peer.ownsForeground() && awayQtInactive;
        const double occludedLuma = meanLuma(grabDesktop());
        const bool occlusionVerified =
            baselineLuma >= 40.0 && occludedLuma >= 0.0 && occludedLuma < baselineLuma * 0.5;
        if (stillAway && occludedLuma >= 0.0) {
            ++run.peerForegroundDesktopSamples;
            if (occludedLuma < 24.0) ++run.peerForegroundBlackDesktopSamples;
        }
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
                           {"cropRect", QJsonArray{cropRect.x(), cropRect.y(),
                                                   cropRect.width(), cropRect.height()}},
                           {"surfaceSize", QJsonArray{surface->width(), surface->height()}},
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
                                surface->uiProfileCounters().initializeGlCalls)},
                           {"focusCycleKind", "native foreground cycle"},
                           {"focusCycleMechanism",
                            "SetForegroundWindow on a separate helper process's window; no "
                            "synthetic keyboard input, so this is not an Alt-Tab reproduction"},
                           {"desktopSampleWhilePeerForeground", stillAway},
                           {"peerPixelsExcluded", true},
                           {"positionBefore", positionBeforeAway},
                           {"fboLumaBeforeAway", run.fboLumaBeforeAway.last()}});

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
            settleFor(60);
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
                // The matrix pumps here too, so the exposed 300 s is represented
                // continuously; a mode with no pump keeps the original 6 ms poll.
                settleFor(6);
            }
        }
        while (!paintSeen && wait.elapsed() < 2500) settleFor(5);
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
            run.activationToVisibleCompositionLowerBoundMs.append(activationToVisibleMs);
        } else if (!occlusionVerified) {
            ++run.visibleCompositionUnsupported;
        } else if (cycleValid) {
            ++run.visibleCompositionTimeouts;
            // The sample never appeared: the true latency is at least the deadline.
            run.activationToVisibleCompositionLowerBoundMs.append(
                kVisibleCompositionDeadlineMs);
        }
        settleFor(250);
        // Sampling the screen while another process owns the foreground would
        // report that process's pixels; the cycle names the loss instead.
        const QImage crop = cycleValid ? readComposedCrop(surface) : QImage();
        const double positionAfterBack = surface->position();
        const double positionDelta = positionAfterBack - positionBeforeAway;
        run.positionDeltaSeconds.append(positionDelta);
        if (positionDelta > 0.0) {
            ++run.positionAdvancedCycles;
        } else {
            ++run.positionStalledCycles;
        }
        run.fboLumaAfterBack.append(meanLuma(crop));
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
            {"focusCycleKind", "native foreground cycle"},
            {"positionAfter", positionAfterBack},
            {"positionDeltaSeconds", positionDelta},
            {"positionAdvanced", positionDelta > 0.0},
            {"fboLumaAfterBack", run.fboLumaAfterBack.last()},
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
    const qint64 visibleLowerP95 =
        static_cast<qint64>(percentileMs(run.activationToVisibleCompositionLowerBoundMs, 0.95));
    qint64 visibleLowerMaximum = -1;
    for (qint64 value : run.activationToVisibleCompositionLowerBoundMs)
        visibleLowerMaximum = std::max(visibleLowerMaximum, value);
    const bool visibleComplete =
        run.visibleCompositionTimeouts == 0 && run.visibleCompositionUnsupported == 0 &&
        run.visibleCompositionObserved == run.cycles - run.foregroundLostCycles;
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
        {"activationToVisibleCompositionObservedP50Ms",
         run.activationToVisibleCompositionMs.isEmpty()
             ? QJsonValue(QStringLiteral("unavailable"))
             : QJsonValue(visibleP50)},
        {"activationToVisibleCompositionObservedP95Ms",
         run.activationToVisibleCompositionMs.isEmpty()
             ? QJsonValue(QStringLiteral("unavailable"))
             : QJsonValue(visibleP95)},
        {"activationToVisibleCompositionObservedMaxMs",
         visibleMaximum < 0 ? QJsonValue(QStringLiteral("unavailable"))
                            : QJsonValue(visibleMaximum)},
        {"activationToVisibleCompositionP95LowerBoundMs",
         run.activationToVisibleCompositionLowerBoundMs.isEmpty()
             ? QJsonValue(QStringLiteral("unavailable"))
             : QJsonValue(visibleLowerP95)},
        {"activationToVisibleCompositionMaxLowerBoundMs",
         visibleLowerMaximum < 0 ? QJsonValue(QStringLiteral("unavailable"))
                                 : QJsonValue(visibleLowerMaximum)},
        {"visibleCompositionComplete", visibleComplete},
        {"visibleCompositionDeadlineMs", kVisibleCompositionDeadlineMs},
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
        {"focusCycleKind", "native foreground cycle"},
        {"positionAdvancedCycles", run.positionAdvancedCycles},
        {"positionStalledCycles", run.positionStalledCycles},
        {"peerForegroundDesktopSamples", run.peerForegroundDesktopSamples},
        {"peerForegroundBlackDesktopSamples", run.peerForegroundBlackDesktopSamples},
        {"peerPixelPolicy",
         "desktop grabs taken while the separate helper owned the foreground are counted "
         "here and excluded from the player's black accounting; they are the helper's "
         "pixels, not the player's picture"},
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
    const QRect sampleCrop = videoSampleCrop(surface);
    mikai::probes::ForegroundPeer peer;
    QString peerError;
    if (!peer.start(QRect(sampleCrop.center() - QPoint(240, 180), QSize(480, 360)),
                    &peerError)) {
        *error = peerError;
        record(QJsonObject{{"event", "focusSwitchResult"},
                           {"result", "UNAVAILABLE"},
                           {"reason", peerError},
                           {"surfaceState", surfaceState(surface)}});
        return 3;
    }
    if (!settleForegroundPeer(peer, dialog, surface, sampleCrop, record, &peerError)) {
        *error = peerError;
        record(QJsonObject{{"event", "focusSwitchResult"},
                           {"result", "INCOMPLETE"},
                           {"reason", peerError},
                           {"surfaceState", surfaceState(surface)}});
        peer.stop();
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

    // The overlay's stacking poll must be on while this application owns the
    // foreground and off the moment another application does: reasserting a tool
    // window from the background only fights the switch, and a raise retry from
    // the background must not borrow the other process's input queue.
    {
        const QString platform = QGuiApplication::platformName();
        const bool pollSupported = platform == QStringLiteral("windows") ||
                                   platform == QStringLiteral("xcb");
        auto* overlay = dialog->findChild<mplayer::shell::ProcessingOverlay*>();
        const bool overlayVisible = overlay != nullptr && overlay->isVisible();
        const bool pollActiveWhileForeground = overlay != nullptr && overlay->stackingPollActive();
        const quint64 stopsBefore = mikai::gui::uiprofile::snapshot().awayStackingStops;
        const quint64 abortsBefore = mikai::gui::uiprofile::snapshot().raiseRetriesAborted;
        const bool attachMetricSupported_here = attachMetricSupported();
        const bool countersOn = mikai::gui::uiprofile::countersEnabled();
        const bool peerActivated = peer.activate();
        // Taken the instant the peer owns the foreground and BEFORE the settle
        // spin: the window where the native foreground has moved while Qt's
        // applicationState() may still read active is exactly where a retry can
        // still call forceForeground(), so excluding it would measure nothing.
        // peer.activate() returns only once GetForegroundWindow() is the peer,
        // so an attach before the switch is still outside this delta.
        const quint64 attachesBefore = mikai::gui::uiprofile::snapshot().threadInputAttaches;
        spinFor(400);
        const bool awayOwned = peerActivated && peer.ownsForeground() &&
                               QGuiApplication::applicationState() != Qt::ApplicationActive;
        spinFor(1000);
        const bool pollActiveWhileAway = overlay != nullptr && overlay->stackingPollActive();
        const mikai::gui::uiprofile::Snapshot countersAway =
            mikai::gui::uiprofile::snapshot();
        const qint64 attachesWhileAway =
            static_cast<qint64>(countersAway.threadInputAttaches - attachesBefore);
        const qint64 stopsWhileAway =
            static_cast<qint64>(countersAway.awayStackingStops - stopsBefore);
        record(QJsonObject{{"event", "focusOverlayStacking"},
                           {"pollSupported", pollSupported},
                           {"overlayVisible", overlayVisible},
                           {"pollActiveWhileForeground", pollActiveWhileForeground},
                           {"foregroundOwnedByPeer", peer.ownsForeground()},
                           {"awayQtInactive",
                            QGuiApplication::applicationState() != Qt::ApplicationActive},
                           {"pollActiveWhileAway", pollActiveWhileAway},
                           {"attachCallsWhileAway", attachesWhileAway},
                           {"awayStackingStops", stopsWhileAway},
                           {"raiseRetriesAborted",
                            static_cast<qint64>(countersAway.raiseRetriesAborted -
                                                abortsBefore)},
                           {"attachMetricSupported", attachMetricSupported_here},
                           {"countersEnabled", countersOn},
                           {"awayWindowMs", 1400}});
        // A zero that cannot move is not evidence: without the shipping policy
        // linked, or with counters off, the attach counter can never increment
        // and the criterion would pass vacuously.
        const QString failure =
            !overlayVisible
                ? QStringLiteral("the engine overlay was not visible for the away check")
            : !countersOn
                ? QStringLiteral("profiling counters were off, so the away counters report "
                                 "nothing and zero attaches is not evidence")
            : !attachMetricSupported_here
                ? QStringLiteral("the shipping raise policy is not linked into this probe, so "
                                 "zero attaches is not evidence")
            : !pollSupported
                ? QStringLiteral("no stacking poll exists on this platform, so the away "
                                 "check is unobservable")
            : !pollActiveWhileForeground
                ? QStringLiteral("the overlay's stacking poll was not running while this "
                                 "application owned the foreground")
            : !awayOwned
                ? QStringLiteral("the foreground helper did not hold the foreground for the "
                                 "overlay check")
            : pollActiveWhileAway
                ? QStringLiteral("the overlay kept its stacking poll running while another "
                                 "application owned the foreground")
            : stopsWhileAway <= 0
                ? QStringLiteral("the stacking poll was running before the switch away but no "
                                 "away stop was counted, so the stop path is unproven")
            : attachesWhileAway != 0
                ? QStringLiteral("the overlay contacted another process's input queue while "
                                 "away (attaches=%1)")
                      .arg(attachesWhileAway)
                : QString();
        if (!failure.isEmpty()) {
            *error = failure;
            record(QJsonObject{{"event", "focusSwitchResult"},
                               {"result", "INCOMPLETE"},
                               {"reason", failure},
                               {"surfaceState", surfaceState(surface)}});
            peer.stop();
            return 3;
        }
        if (!ensureForeground(dialog)) {
            *error = QStringLiteral(
                "the player did not take the foreground back after the overlay away check");
            record(QJsonObject{{"event", "focusSwitchResult"},
                               {"result", "INCOMPLETE"},
                               {"reason", *error},
                               {"surfaceState", surfaceState(surface)}});
            peer.stop();
            return 3;
        }
        spinFor(300);
    }
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
    QVector<qint64> allVisibleLowerBoundMs;
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
        allVisibleLowerBoundMs += run.activationToVisibleCompositionLowerBoundMs;
        blackSamples += run.blackSamples;
        blackMeasured += run.blackMeasured;
        foregroundLost += run.foregroundLostCycles;
        visibleUnsupported += run.visibleCompositionUnsupported;
        visibleTimeouts += run.visibleCompositionTimeouts;
        grabCalls += run.desktopGrabCalls;
        grabMicros += run.desktopGrabTotalMicros;
        grabMaxMicros = std::max(grabMaxMicros, run.desktopGrabMaxMicros);
    }
    const bool measurementAvailable =
        !allVisibleLowerBoundMs.isEmpty() && blackMeasured > 0;
    const bool latencyComplete =
        visibleTimeouts == 0 && visibleUnsupported == 0 && measurementAvailable;
    const bool clean = measurementAvailable && blackSamples == 0;
    record(QJsonObject{{"event", "focusSwitchResult"},
                       {"result", !measurementAvailable ? "INCOMPLETE"
                                : (clean && latencyComplete) ? "PASS"
                                                             : "INCOMPLETE"},
                       {"phases", phaseRows},
                       {"fixtureMeanLuma", fixtureLuma},
                       {"fixtureSampled", !fixture.isNull()},
                       {"knownBrightFixture", knownBright},
                       {"attachCallsBeforePhaseReset", static_cast<qint64>(setupAttaches)},
                       {"activationToVisibleCompositionP95LowerBoundMs",
                        allVisibleLowerBoundMs.isEmpty()
                            ? QJsonValue(QStringLiteral("unavailable"))
                            : QJsonValue(percentileMs(allVisibleLowerBoundMs, 0.95))},
                       {"activationToVisibleCompositionObservedP95Ms",
                        allVisibleMs.isEmpty()
                            ? QJsonValue(QStringLiteral("unavailable"))
                            : QJsonValue(percentileMs(allVisibleMs, 0.95))},
                       {"activationToVisibleCompositionSamples",
                        static_cast<int>(allVisibleMs.size())},
                       {"activationToVisibleCompositionLowerBoundSamples",
                        static_cast<int>(allVisibleLowerBoundMs.size())},
                       {"visibleCompositionComplete", latencyComplete},
                       {"visibleCompositionDeadlineMs", kVisibleCompositionDeadlineMs},
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
    return (clean && latencyComplete) ? 0 : 3;
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
    const QRect activationCrop = videoSampleCrop(surface);
    mikai::probes::ForegroundPeer peer;
    QString peerError;
    bool peerStarted =
        peer.start(QRect(activationCrop.center() - QPoint(240, 180), QSize(480, 360)),
                   &peerError);
    ActivationRun activation;
    activation.phase = QStringLiteral("plain");
    if (peerStarted) {
        record(QJsonObject{{"event", "foregroundPeer"},
                           {"pid", static_cast<qint64>(peer.processId())},
                           {"window", static_cast<qint64>(peer.windowId())},
                           {"foreignPid",
                            peer.processId() != QCoreApplication::applicationPid()}});
        peerStarted =
            settleForegroundPeer(peer, dialog, surface, activationCrop, record, &peerError);
        if (peerStarted) {
            activation = runActivationCycles(dialog, surface, peer, QStringLiteral("plain"), 10,
                                             knownBright, record);
        }
        peer.stop();
    }
    if (!peerStarted) {
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
    const double visibleLowerP95 =
        percentileMs(activation.activationToVisibleCompositionLowerBoundMs, 0.95);
    qint64 maxVisible = -1;
    for (qint64 value : activation.activationToVisibleCompositionMs)
        maxVisible = std::max(maxVisible, value);
    const auto finalCounters = mikai::gui::uiprofile::snapshot();
    const bool swapMeasured = !activation.activationToFirstSwapMs.isEmpty();
    const bool visibleMeasured = !activation.activationToVisibleCompositionMs.isEmpty();
    const bool latencyComplete = visibleMeasured &&
                                 activation.visibleCompositionTimeouts == 0 &&
                                 activation.visibleCompositionUnsupported == 0;
    const bool blackMeasured = loadSampled > 0;
    const bool measured = preconditionsValid && stormValid && peerStarted &&
                          latencyComplete && blackMeasured;
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
        {"activationToVisibleCompositionP95LowerBoundMs",
         activation.activationToVisibleCompositionLowerBoundMs.isEmpty()
             ? QJsonValue(QStringLiteral("unavailable"))
             : QJsonValue(visibleLowerP95)},
        {"activationToVisibleCompositionObservedP95Ms",
         visibleMeasured ? QJsonValue(visibleP95)
                         : QJsonValue(QStringLiteral("unavailable"))},
        {"activationToVisibleCompositionSamples",
         static_cast<int>(activation.activationToVisibleCompositionMs.size())},
        {"activationToVisibleCompositionMaxObservedMs",
         maxVisible < 0 ? QJsonValue(QStringLiteral("unavailable"))
                        : QJsonValue(maxVisible)},
        {"visibleCompositionComplete", latencyComplete},
        {"visibleCompositionDeadlineMs", kVisibleCompositionDeadlineMs},
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
                                       : (!latencyComplete
                                              ? QStringLiteral("visible-composition "
                                                               "timeout; latency quantile "
                                                               "reported as a lower bound")
                                              : QStringLiteral("loadfile measurement was "
                                                               "unavailable")))))},
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

// ── the 30 s hover-playback canary ───────────────────────────────────────
//
// GOAL-F/T-F4's regression net: the embedded core plays the fixture for the whole
// requested window while the pointer sweeps real chrome, and the frame-timing log
// is the measurement. It takes no desktop grab, claims no composition and never
// takes the foreground or the system cursor: the gestures are delivered to the
// widgets themselves, so the run can be unattended.
//
// What it gates is the pacing policy, not a speed: a chrome repaint never takes
// an armed presentation slot (the disarm counter), every scheduling decision
// names a documented reason, no paint claims a held pause nobody holds, and the
// core really advanced frames. Paint-delay quantiles are reported with their
// sample count; the plan fixes no delay budget, and the only frozen paint number
// (343 us p95 duration) is a matched-host measurement owned by the load probe,
// so inventing a CI millisecond gate here would be a different claim.

struct CanaryLogSummary {
    int paintLines = 0;
    int pacedSamples = 0;
    int unknownReasons = 0;
    int pacedWithoutDelay = 0;
    int pauseHeldLines = 0;
    int newFrameLines = 0;
    QVector<qint64> delays;
};

/// The value after `token=` on one log line. -1/false when the token is absent,
/// which is what keeps a missing field from reading as a zero.
bool canaryTokenInt(const QString& line, const QString& token, qint64* value) {
    const qsizetype at = line.indexOf(token);
    if (at < 0 || value == nullptr) return false;
    qsizetype end = at + token.size();
    const qsizetype start = end;
    while (end < line.size() &&
           (line.at(end).isDigit() || (end == start && line.at(end) == QLatin1Char('-'))))
        ++end;
    if (end == start) return false;
    bool ok = false;
    const qint64 parsed = line.mid(start, end - start).toLongLong(&ok);
    if (!ok) return false;
    *value = parsed;
    return true;
}

QString canaryTokenWord(const QString& line, const QString& token) {
    const qsizetype at = line.indexOf(token);
    if (at < 0) return QString();
    qsizetype end = at + token.size();
    const qsizetype start = end;
    while (end < line.size() && !line.at(end).isSpace()) ++end;
    return line.mid(start, end - start);
}

/// One pointer gesture on a real chrome control: the events a pointer produces
/// arrive at the widget directly, so no window has to be active and the system
/// cursor is never moved. A hidden control is skipped rather than invented.
bool canaryGesture(QWidget* target, const QPoint& local) {
    if (target == nullptr || !target->isVisible() || target->width() <= 0 ||
        target->height() <= 0)
        return false;
    const QPointF localPos(local);
    const QPointF globalPos(target->mapToGlobal(local));
    QEnterEvent enter(localPos, localPos, globalPos);
    QCoreApplication::sendEvent(target, &enter);
    QMouseEvent move(QEvent::MouseMove, localPos, globalPos, Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(target, &move);
    return true;
}

bool canarySkipReasonExplained(const QString& reason) {
    return reason.isEmpty() || reason == QLatin1String("none") ||
           reason == QLatin1String("paced") || reason == QLatin1String("forced") ||
           reason == QLatin1String("repeat") || reason == QLatin1String("redraw") ||
           reason == QLatin1String("elapsed") || reason == QLatin1String("no-info");
}

int runHoverCanaryMode(mplayer::shell::PlayerDialog* dialog, mplayer::VideoSurface* surface,
                       mplayer::shell::PlayerShell* shell, const QString& media, int seconds,
                       const QString& outputPath,
                       const std::function<void(QJsonObject)>& record, QString* error) {
    // T-F4 names a 30 s window; a shorter one would be a different measurement
    // reported under this name.
    constexpr int kRequiredSeconds = 30;
    if (dialog == nullptr || surface == nullptr || shell == nullptr) {
        *error = QStringLiteral("the canary needs the real dialog, surface and shell");
        return 2;
    }
    const QString platform = QGuiApplication::platformName();
    if (seconds < kRequiredSeconds) {
        record(QJsonObject{{"event", "hoverCanary"},
                           {"result", "INVALID"},
                           {"reason", "window shorter than the 30 s the goal names"},
                           {"seconds", seconds},
                           {"platform", platform}});
        *error = QStringLiteral("the hover-playback canary is a %1 s window; %2 s is not it")
                     .arg(kRequiredSeconds)
                     .arg(seconds);
        return 2;
    }
    // An offscreen or minimal platform can still construct widgets, but not the
    // presented, hardware-painted window this canary is about. Saying so is the
    // difference between a refusal and a green run nobody can trust.
    if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal")) {
        record(QJsonObject{{"event", "hoverCanary"},
                           {"result", "REFUSED"},
                           {"reason", "no real display platform: the player cannot present or "
                                      "paint its surface here"},
                           {"platform", platform},
                           {"required", "windows, xcb, wayland or cocoa with a working GL context"}});
        *error = QStringLiteral("display prerequisite missing: platform '%1' cannot present the "
                                "player").arg(platform);
        return 77;
    }
    if (!waitForVideo(surface, 20000)) {
        record(QJsonObject{{"event", "hoverCanary"},
                           {"result", "FAILED"},
                           {"reason", "no video decoded"},
                           {"platform", platform},
                           {"surfaceState", surfaceState(surface)}});
        *error = QStringLiteral("the core decoded no frame of the fixture");
        return 1;
    }
    if (!surface->isContextReady() && !surface->waitForContext(10000)) {
        record(QJsonObject{{"event", "hoverCanary"},
                           {"result", "REFUSED"},
                           {"reason", "no OpenGL render context for the surface"},
                           {"platform", platform},
                           {"surfaceState", surfaceState(surface)}});
        *error = QStringLiteral("render-context prerequisite missing: the surface has no GL "
                                "context");
        return 77;
    }
    // Playback, verified in the core: the pause flag has to be false there, not
    // only in the widget's mirror.
    bool playing = false;
    for (int attempt = 0; attempt < 100 && !playing; ++attempt) {
        surface->setPaused(false);
        if (surface->isPlaying()) {
            const auto pause = surface->probeMpvProperty(QStringLiteral("pause"),
                                                         mplayer::ProbePropertyKind::Flag);
            playing = pause.has_value() &&
                      pause->kind == mplayer::ProbePropertyValue::Kind::Flag && !pause->flag;
        }
        spinFor(50);
    }
    QElapsedTimer pictureWait;
    pictureWait.start();
    while (!surface->hasVisibleOutputFrame() && pictureWait.elapsed() < 10000) spinFor(25);
    const bool pictureOnScreen = surface->hasVisibleOutputFrame();
    shell->wake();
    spinFor(250);
    QWidget* playToggle = shell->findChild<QWidget*>(QStringLiteral("playerPlayToggle"));
    QWidget* settingsButton = shell->findChild<QWidget*>(QStringLiteral("playerSettingsButton"));
    QWidget* scrub = shell->findChild<QWidget*>(QStringLiteral("playerScrubBar"));
    const bool chromePresent =
        playToggle != nullptr && settingsButton != nullptr && scrub != nullptr;
    record(QJsonObject{{"event", "hoverCanaryStart"},
                       {"seconds", seconds},
                       {"platform", platform},
                       {"platformDetail", QGuiApplication::platformName()},
                       {"playing", playing},
                       {"pictureOnScreen", pictureOnScreen},
                       {"chromePresent", chromePresent},
                       {"surfaceState", surfaceState(surface)},
                       {"mediaFacts", mediaFacts(media)},
                       {"countersEnabled", mikai::gui::uiprofile::countersEnabled()},
                       {"scope", "embedded core playback with pointer gestures on real chrome; "
                                 "no desktop pixels, no foreground ownership, no composition "
                                 "claim"}});
    if (!playing || !pictureOnScreen || !chromePresent) {
        *error = !playing ? QStringLiteral("the core did not start playing")
                          : (!pictureOnScreen ? QStringLiteral("no frame reached the surface")
                                              : QStringLiteral("the player chrome was not present"));
        record(QJsonObject{{"event", "hoverCanary"},
                           {"result", "INVALID"},
                           {"reason", *error},
                           {"surfaceState", surfaceState(surface)}});
        return 1;
    }

    // The window's own log: the path is set immediately before the storm, and
    // moved afterwards so the surface flushes exactly this window's lines into
    // the first file. Setting it also forces the counters on, so the numbers
    // below are collected even without MIKAI_UI_PROFILE.
    const QString windowLog =
        QDir(outputPath).filePath(QStringLiteral("hover-canary-frame-timing.jsonl"));
    const QString closeLog =
        QDir(outputPath).filePath(QStringLiteral("hover-canary-close.jsonl"));
    QFile::remove(windowLog);
    QFile::remove(closeLog);
    mikai::gui::uiprofile::resetCounters();
    const mikai::gui::uiprofile::Snapshot countersBefore = mikai::gui::uiprofile::snapshot();
    const auto profileBefore = surface->uiProfileCounters();
    const quint64 framesBefore = surface->playbackDiagnostics().renderedOutputFrames;
    const double positionAtStart = surface->position();
    mplayer::VideoSurface::setFrameTimingLogPath(windowLog);

    QElapsedTimer wall;
    wall.start();
    qint64 nextWakeMs = 2000;
    int gestures = 0;
    int loopSeeks = 0;
    int ticks = 0;
    while (wall.elapsed() < qint64(seconds) * 1000) {
        if (wall.elapsed() >= nextWakeMs) {
            // The dock auto-hides while playing; the canary keeps it up the way
            // a viewer does, by waking the chrome, not by pinning it open.
            shell->wake();
            nextWakeMs = wall.elapsed() + 2000;
        }
        // A tracked fixture is short. A backwards seek keeps the window playing
        // without a source reload (no load transition), and the count is stated
        // rather than hidden.
        const double duration = surface->duration();
        const double position = surface->position();
        if (duration > 3.0 && position > duration - 2.0) {
            surface->seek(0.5);
            ++loopSeeks;
        }
        QWidget* target = (ticks % 3 == 0) ? playToggle : (ticks % 3 == 1 ? scrub : settingsButton);
        const QPoint local =
            (target == scrub)
                ? QPoint(qBound(2, target->width() * ((ticks / 3) % 2 == 0 ? 35 : 62) / 100,
                                target->width() - 2),
                         target->height() / 2)
                : QPoint(target->width() / 2, target->height() / 2);
        if (canaryGesture(target, local)) ++gestures;
        spinFor(16);
        ++ticks;
    }
    const qint64 windowMs = wall.elapsed();
    const auto profileAfter = surface->uiProfileCounters();
    const mikai::gui::uiprofile::Snapshot countersAfter = mikai::gui::uiprofile::snapshot();
    const quint64 framesAfter = surface->playbackDiagnostics().renderedOutputFrames;
    const double positionAtEnd = surface->position();
    const QJsonObject stateAtEnd = surfaceState(surface);
    // End of the window: move the log so its buffer is flushed into windowLog,
    // then close the dialog, which flushes whatever is still buffered.
    mplayer::VideoSurface::setFrameTimingLogPath(closeLog);
    spinFor(150);
    dialog->close();
    spinFor(200);
    mplayer::VideoSurface::setFrameTimingLogPath(QString());

    CanaryLogSummary summary;
    QFile logFile(windowLog);
    if (logFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!logFile.atEnd()) {
            const QString line = QString::fromUtf8(logFile.readLine());
            if (!line.startsWith(QLatin1String("paint "))) continue;
            ++summary.paintLines;
            const QString reason = canaryTokenWord(line, QStringLiteral("skip_reason="));
            if (!canarySkipReasonExplained(reason)) ++summary.unknownReasons;
            qint64 delay = -1;
            if (canaryTokenInt(line, QStringLiteral("paint_delay_ms="), &delay)) {
                if (delay >= 0) {
                    ++summary.pacedSamples;
                    summary.delays.append(delay);
                } else if (reason == QLatin1String("paced")) {
                    // A pacing decision that recorded no delay is a skip with no
                    // explanation: the timer armed and no measurement followed.
                    ++summary.pacedWithoutDelay;
                }
            }
            qint64 pauseHeld = 0;
            if (canaryTokenInt(line, QStringLiteral("pause_held="), &pauseHeld) && pauseHeld != 0)
                ++summary.pauseHeldLines;
            qint64 newFrame = 0;
            if (canaryTokenInt(line, QStringLiteral("newframe="), &newFrame) && newFrame != 0)
                ++summary.newFrameLines;
        }
    }
    const quint64 windowPaints = profileAfter.paintCalls - profileBefore.paintCalls;
    const quint64 windowDisarms = profileAfter.pacingDisarms - profileBefore.pacingDisarms;
    const quint64 windowChromeDirty =
        countersAfter.chromeDirtyRequests - countersBefore.chromeDirtyRequests;
    const quint64 windowChromeApplied =
        countersAfter.chromeDirtyApplied - countersBefore.chromeDirtyApplied;
    const quint64 windowRenderedFrames =
        framesAfter >= framesBefore ? framesAfter - framesBefore : 0;
    const int unexplainedSkips =
        summary.unknownReasons + summary.pacedWithoutDelay + summary.pauseHeldLines;
    const double delayP50 = percentileMs(summary.delays, 0.50);
    const double delayP95 = percentileMs(summary.delays, 0.95);
    qint64 delayMax = -1;
    for (qint64 value : summary.delays) delayMax = std::max(delayMax, value);

    const bool measured = summary.paintLines > 0 && windowPaints > 0 && gestures > 0;
    const bool playbackAdvanced = summary.newFrameLines >= seconds && windowRenderedFrames > 0;
    const bool chromeInteracted = windowChromeDirty > 0;
    const bool noDisarms = windowDisarms == 0;
    const bool skipsExplained = unexplainedSkips == 0;
    const bool delayMeasured = summary.pacedSamples > 0;
    const bool green = measured && playbackAdvanced && chromeInteracted && noDisarms &&
                       skipsExplained && delayMeasured;

    record(QJsonObject{
        {"event", "hoverCanary"},
        {"result", green ? "PASS" : "FAIL"},
        {"platform", platform},
        {"windowMs", windowMs},
        {"requestedSeconds", seconds},
        {"loopSeeks", loopSeeks},
        {"gestures", gestures},
        {"ticks", ticks},
        {"paintLines", summary.paintLines},
        {"paintCalls", static_cast<qint64>(windowPaints)},
        {"newFramePaints", summary.newFrameLines},
        {"renderedOutputFrames", static_cast<qint64>(windowRenderedFrames)},
        {"positionAtStart", positionAtStart},
        {"positionAtEnd", positionAtEnd},
        {"chromeDirtyRequests", static_cast<qint64>(windowChromeDirty)},
        {"chromeDirtyApplied", static_cast<qint64>(windowChromeApplied)},
        {"pacingDisarms", static_cast<qint64>(windowDisarms)},
        {"pacedSamples", summary.pacedSamples},
        {"paintDelayP50Ms", delayMeasured ? QJsonValue(delayP50)
                                          : QJsonValue(QStringLiteral("unavailable"))},
        {"paintDelayP95Ms", delayMeasured ? QJsonValue(delayP95)
                                          : QJsonValue(QStringLiteral("unavailable"))},
        {"paintDelayMaxMs", delayMeasured ? QJsonValue(delayMax)
                                          : QJsonValue(QStringLiteral("unavailable"))},
        {"unexplainedSkips", unexplainedSkips},
        {"unknownSkipReasons", summary.unknownReasons},
        {"pacedWithoutDelay", summary.pacedWithoutDelay},
        {"pauseHeldPaints", summary.pauseHeldLines},
        {"criteria",
         QJsonObject{{"measured", measured},
                     {"playbackAdvanced", playbackAdvanced},
                     {"chromeInteracted", chromeInteracted},
                     {"noPacingDisarms", noDisarms},
                     {"skipsExplained", skipsExplained},
                     {"paintDelayMeasured", delayMeasured}}},
        {"frameTimingLog", windowLog},
        {"surfaceAtEnd", stateAtEnd},
        {"scope", "pacing and chrome-paint counters plus the frame-timing log from a real "
                  "embedded core; desktop composition is not observed and not claimed"},
        {"error", green ? QString() : *error}});
    if (!green) {
        *error = !measured
                     ? QStringLiteral("hover canary INVALID: no paints, no gestures or no log")
                     : (!playbackAdvanced
                            ? QStringLiteral("hover canary FAIL: the core did not advance frames "
                                             "for the whole window")
                            : (!chromeInteracted
                                   ? QStringLiteral("hover canary FAIL: no chrome invalidation "
                                                    "reached the shell")
                                   : (!noDisarms
                                          ? QStringLiteral("hover canary FAIL: a chrome repaint "
                                                           "took an armed pacing slot")
                                          : (!skipsExplained
                                                 ? QStringLiteral("hover canary FAIL: a paint "
                                                                  "skip had no documented reason")
                                                 : QStringLiteral("hover canary INCOMPLETE: no "
                                                                  "paced paint-delay sample")))));
        return 1;
    }
    return 0;
}

/// GOAL-E long-playback QA matrix.
///
/// One real playback, sampled from the composed framebuffer and from the real
/// desktop roughly every 32 ms while the player is exposed, exercised against a
/// fixed budget: >=30 observed chrome state changes, ten verified native
/// foreground away/back cycles, five minimize/restore cycles, and the
/// fullscreen/PiP seams the transition mode already watches. It never
/// synthesizes an engine build and never pauses playback: the media clock itself
/// has to advance for the whole window, and every observation is counted or
/// excluded by name rather than assumed.
int runQaMatrixMode(mplayer::shell::PlayerDialog* dialog, mplayer::VideoSurface* surface,
                    mplayer::shell::PlayerShell* shell, const QString& media,
                    const QString& outputPath, const PlaybackHost* host, bool knownBright,
                    int seconds, const std::function<void(QJsonObject)>& record,
                    QString* error) {
    constexpr int kChromeChangesRequired = 30;
    constexpr int kNativeForegroundCyclesRequired = 10;
    constexpr int kMinimizeRestoreCyclesRequired = 5;
    constexpr int kMinimizeDwellMs = 900;
    constexpr int kMaxMinimizeDwellMs = 5000;
    constexpr qint64 kMaxTotalMinimizedMs = 15000;
    constexpr int kFullscreenSeamTransitions = 3;

    const auto fail = [&](const QString& reason, const char* result) {
        *error = reason;
        record(QJsonObject{{"event", "qaMatrixResult"},
                           {"result", QString::fromLatin1(result)},
                           {"reason", reason},
                           {"mode", "qa-matrix"},
                           {"requestedSeconds", seconds},
                           {"knownBrightFixture", knownBright},
                           {"mediaFacts", mediaFacts(media)},
                           {"surfaceState", surfaceState(surface)},
                           {"presentationMode", presentationMode()}});
        return 3;
    };

    if (shell == nullptr) {
        return fail(QStringLiteral("the player shell was not built, so chrome state is "
                                   "unobservable"),
                    "INVALID");
    }
    if (seconds < 300) {
        return fail(QStringLiteral("the QA matrix is a 300 s long-playback matrix; requested %1 s")
                        .arg(seconds),
                    "INVALID");
    }
    if (!waitForVideo(surface, 20000)) {
        return fail(QStringLiteral("no video decoded before the QA matrix"), "INCOMPLETE");
    }
    // This mode is long playback: a paused core would make every position and
    // motion observation a statement about a still frame.
    const auto pauseFlag = [&]() -> std::optional<bool> {
        const auto value =
            surface->probeMpvProperty(QStringLiteral("pause"), mplayer::ProbePropertyKind::Flag);
        if (!value.has_value() || value->kind != mplayer::ProbePropertyValue::Kind::Flag)
            return std::nullopt;
        return value->flag;
    };
    if (!surface->isPlaying() || pauseFlag().value_or(true)) {
        dialog->resumePlayback();
        spinFor(600);
    }
    const std::optional<bool> pausedAtStart = pauseFlag();
    if (!pausedAtStart.has_value() || *pausedAtStart || !surface->isPlaying()) {
        return fail(QStringLiteral("the QA matrix requires a playing source; pause=%1 playing=%2")
                        .arg(pausedAtStart.has_value()
                                 ? (*pausedAtStart ? QStringLiteral("true")
                                                   : QStringLiteral("false"))
                                 : QStringLiteral("unavailable"))
                        .arg(surface->isPlaying() ? QStringLiteral("true")
                                                  : QStringLiteral("false")),
                    "INVALID");
    }
    if (!dialog->isFullScreen()) {
        dialog->showFullScreen();
        spinFor(700);
    }
    if (!ensureForeground(dialog)) {
        return fail(QStringLiteral("the player never owned the native foreground at start"),
                    "INVALID");
    }
    spinFor(300);

    // ── async evidence writer ─────────────────────────────────────────────
    // PNG encoding on the GUI thread would distort the same sampling this mode
    // is measuring, so the matrix reuses the writer the transition mode uses.
    EvidenceWrites artifactWrites;
    QThreadPool artifactPool;
    artifactPool.setMaxThreadCount(1);
    bool collectingArtifacts = true;
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
        delete task;
        artifactWrites.noteSkip();
        record({{"event", "artifactSkipped"}, {"artifact", name},
                {"reason", "evidence writer busy"}});
    };

    MatrixSampler sampler;
    sampler.dialog = dialog;
    sampler.mainSurface = surface;
    sampler.record = record;
    sampler.saveImage = saveImage;

    QElapsedTimer runClock;
    runClock.start();
    QString currentPhase = QStringLiteral("calibration");

    // ── calibration: a measured bright picture, not a declared one ────────
    // The gate has to survive a long run of real content, so it is built from
    // the MEAN of the bright samples, not their maximum: a bright opening would
    // otherwise set a bar that later, legitimately darker frames cannot clear,
    // and a correct picture would be reported as missing.
    double calibratedLumaTotal = 0.0;
    double calibratedDesktopTotal = 0.0;
    int brightFboSamples = 0;
    int brightDesktopSamples = 0;
    QElapsedTimer calibrationClock;
    calibrationClock.start();
    while (calibrationClock.elapsed() < 12000 &&
           (brightFboSamples < 8 || brightDesktopSamples < 4)) {
        if (!foregroundIs(dialog)) ensureForeground(dialog);
        const QImage composed = readComposedCrop(surface);
        const double luma = meanLuma(composed);
        if (!composed.isNull() && luma >= 40.0) {
            ++brightFboSamples;
            calibratedLumaTotal += luma;
        }
        const QImage desktop = desktopCrop(surface->screen(), videoSampleCrop(surface));
        const double desktopLuma = meanLuma(desktop);
        if (!desktop.isNull() && desktopLuma >= 40.0) {
            ++brightDesktopSamples;
            calibratedDesktopTotal += desktopLuma;
        }
        spinFor(32);
    }
    const double calibratedLuma =
        brightFboSamples > 0 ? calibratedLumaTotal / brightFboSamples : -1.0;
    const double calibratedDesktopLuma =
        brightDesktopSamples > 0 ? calibratedDesktopTotal / brightDesktopSamples : -1.0;
    // The desktop gate is the one post-action predicates use, so it is
    // calibrated from the desktop samples; the FBO mean is the fallback.
    const double pictureLuma =
        calibratedDesktopLuma >= 30.0 ? calibratedDesktopLuma : calibratedLuma;
    const double brightThreshold =
        pictureLuma >= 30.0 ? std::max(30.0, pictureLuma * 0.6) : -1.0;
    // Post-action picture test. The reference is the desktop luma measured just
    // before the action, so a bright opening cannot raise a fixed bar and a
    // legitimately darker (but not black) frame is still a picture. The opening
    // calibration is only the fallback when no per-action reference exists.
    const auto picturePresent = [&](const QImage& desktop, double referenceLuma) {
        if (desktop.isNull() || isBlack(desktop)) return false;
        const double luma = meanLuma(desktop);
        if (luma < 0.0) return false;
        const double reference = referenceLuma > 0.0 ? referenceLuma : brightThreshold;
        return luma >= std::max(30.0, reference * 0.5);
    };
    record(QJsonObject{{"event", "qaMatrixCalibration"},
                       {"elapsedMs", runClock.elapsed()},
                       {"calibratedFboLuma", calibratedLuma},
                       {"calibratedDesktopLuma", calibratedDesktopLuma},
                       {"brightFboSamples", brightFboSamples},
                       {"brightDesktopSamples", brightDesktopSamples},
                       {"brightThreshold", brightThreshold},
                       {"knownBrightFixture", knownBright},
                       {"fixtureMeanLuma", meanLuma(readComposedCrop(surface))},
                       {"corePaused", false},
                       {"playing", surface->isPlaying()},
                       {"surfaceState", surfaceState(surface)}});
    if (brightThreshold < 0.0) {
        return fail(QStringLiteral("the composed framebuffer never showed a calibrated bright "
                                   "frame (fbo samples=%1 desktop samples=%2)")
                        .arg(brightFboSamples)
                        .arg(brightDesktopSamples),
                    "INCOMPLETE");
    }
    // A bright framebuffer with a desktop crop that never showed a bright pixel
    // is not "the player was black": the screen grab is not returning this
    // desktop's pixels, so every later desktop verdict - including the
    // peer-occlusion settle, which needs a lit baseline - would be measuring a
    // rectangle that is already dark. Name that precondition here instead of
    // running the matrix and blaming the helper for an occlusion it cannot aim.
    if (brightDesktopSamples == 0 && brightFboSamples > 0) {
        const QRect preconditionCrop = videoSampleCrop(surface);
        const QImage preconditionDesktop = desktopCrop(surface->screen(), preconditionCrop);
        saveImage(preconditionDesktop, QStringLiteral("qa-matrix-desktop-precondition.png"));
        QScreen* screen = surface->screen();
        const QPoint surfaceOrigin = surface != nullptr ? surface->mapToGlobal(QPoint(0, 0))
                                                        : QPoint(0, 0);
        record(QJsonObject{
            {"event", "qaMatrixCapturePrecondition"},
            {"elapsedMs", runClock.elapsed()},
            {"calibrationWindowMs", calibrationClock.elapsed()},
            {"brightFboSamples", brightFboSamples},
            {"brightDesktopSamples", brightDesktopSamples},
            {"calibratedFboLuma", calibratedLuma},
            {"desktopCropNull", preconditionDesktop.isNull()},
            {"desktopCropLuma", meanLuma(preconditionDesktop)},
            {"desktopCropSize",
             QJsonArray{preconditionDesktop.width(), preconditionDesktop.height()}},
            {"cropRect", QJsonArray{preconditionCrop.x(), preconditionCrop.y(),
                                    preconditionCrop.width(), preconditionCrop.height()}},
            {"surfaceGeometry",
             QJsonArray{surfaceOrigin.x(), surfaceOrigin.y(), surface->width(),
                        surface->height()}},
            {"surfaceDevicePixelRatio", surface->devicePixelRatioF()},
            {"screen", screen != nullptr ? screen->name() : QStringLiteral("none")},
            {"screenGeometry",
             screen != nullptr ? QJsonArray{screen->geometry().x(), screen->geometry().y(),
                                            screen->geometry().width(),
                                            screen->geometry().height()}
                               : QJsonArray{}},
            {"screenDevicePixelRatio", screen != nullptr ? screen->devicePixelRatio() : -1.0},
            // Environment values that change what the crop means. Recorded as
            // values, never offered as an inferred cause.
            {"qtScaleFactor", qEnvironmentVariable("QT_SCALE_FACTOR")},
            {"note",
             QStringLiteral("the composed framebuffer was bright for the whole calibration "
                            "window while the desktop grab never returned a bright pixel; the "
                            "peer-occlusion settle was not attempted because a dark baseline "
                            "cannot show occlusion, and no matrix verdict can rest on desktop "
                            "pixels this process cannot read")}});
        return fail(QStringLiteral("the desktop grab never returned a bright pixel in the %1 ms "
                                   "calibration window although the composed framebuffer was "
                                   "bright %2 times (crop %3x%4 at %5,%6, screen %7, dpr %8); "
                                   "peer occlusion was not attempted")
                        .arg(calibrationClock.elapsed())
                        .arg(brightFboSamples)
                        .arg(preconditionCrop.width())
                        .arg(preconditionCrop.height())
                        .arg(preconditionCrop.x())
                        .arg(preconditionCrop.y())
                        .arg(screen != nullptr ? screen->name() : QStringLiteral("none"))
                        .arg(surface->devicePixelRatioF()),
                    "INCOMPLETE");
    }

    // ── the separate-process foreground peer (pre-monitor) ────────────────
    // Peer process start-up and the foreground settle are diagnostic setup, not
    // playback: they run BEFORE the monitored window so their blocking cannot
    // be charged to the GUI heartbeat as an application stall.
    currentPhase = QStringLiteral("peer-warmup");
    const QRect sampleCrop = videoSampleCrop(surface);
    mikai::probes::ForegroundPeer peer;
    QString peerError;
    if (!peer.start(QRect(sampleCrop.center() - QPoint(240, 180), QSize(480, 360)), &peerError)) {
        return fail(peerError, "UNAVAILABLE");
    }
    sampler.peer = &peer;
    if (!settleForegroundPeer(peer, dialog, surface, sampleCrop, record, &peerError)) {
        peer.stop();
        return fail(peerError, "INCOMPLETE");
    }
    record(QJsonObject{{"event", "foregroundPeer"},
                       {"pid", static_cast<qint64>(peer.processId())},
                       {"window", static_cast<qint64>(peer.windowId())},
                       {"foreignPid", peer.processId() != QCoreApplication::applicationPid()},
                       {"focusCycleKind", "native foreground cycle"},
                       {"focusCycleMechanism",
                        "SetForegroundWindow on a separate helper process's window; no "
                        "synthetic keyboard input, so it is not an Alt-Tab reproduction"}});

    // ── monitored window starts here ──────────────────────────────────────
    // Calibration and the peer start-up above are explicit pre-monitor work; the
    // GUI heartbeat, the media clock and every coverage claim start now.
    const qint64 monitoredStartMs = runClock.elapsed();
    const double positionStart = surface->position();
    const double durationAtStart = surface->duration();
    const quint64 framesStart = surface->playbackDiagnostics().renderedOutputFrames;
    const quint64 sourceEpochAtStart = surface->sourceEpoch();
    const auto countersAtStart = surface->uiProfileCounters();
    const auto profileAtStart = mikai::gui::uiprofile::snapshot();

    // ── sampling pump ─────────────────────────────────────────────────────
    qint64 lastSummaryElapsed = 0;
    const auto maybeSummary = [&] {
        const qint64 now = runClock.elapsed();
        if (now - lastSummaryElapsed < 10000) return;
        lastSummaryElapsed = now;
        record(QJsonObject{{"event", "matrixSamplingSummary"},
                           {"elapsedMs", now},
                           {"phase", currentPhase},
                           {"attempted", sampler.attempts},
                           {"exposedSamples", sampler.exposedSamples},
                           {"fboSamples", sampler.fboSamples},
                           {"fboBlackSamples", sampler.fboBlack},
                           {"fboUnmeasured", sampler.fboUnmeasured},
                           {"desktopSamples", sampler.desktopSamples},
                           {"desktopBlackSamples", sampler.desktopBlack},
                           {"desktopUnmeasured", sampler.desktopUnmeasured},
                           {"excludedPeerForeground", sampler.excludedPeerForeground},
                           {"excludedMinimized", sampler.excludedMinimized},
                           {"excludedNotExposed", sampler.excludedNotExposed},
                           {"excludedForeignForeground", sampler.excludedForeignForeground},
                           {"excludedNoVisibleOutputFrame",
                            sampler.excludedNoVisibleOutputFrame},
                           {"contextGenerationChanges",
                            static_cast<qint64>(sampler.contextGenerationChanges)},
                           {"sourceEpochChanges",
                            static_cast<qint64>(sampler.sourceEpochChanges)},
                           {"position", surface->position()},
                           {"frames", static_cast<qint64>(
                                          surface->playbackDiagnostics().renderedOutputFrames)},
                           {"maxSampleWorkMs", sampler.maxWorkMs}});
    };
    // One sample every ~32 ms of wall time: the tick's own work is inside that
    // budget, so a slow readback cannot silently halve the sampling density.
    const auto sampleTick = [&] {
        QElapsedTimer cadence;
        cadence.start();
        sampler.tick(runClock.elapsed());
        maybeSummary();
        const qint64 spent = cadence.elapsed();
        if (spent < 32) {
            spinFor(static_cast<int>(32 - spent));
        } else {
            // A slow grab already spent the cadence. Not returning to the loop
            // here stacks those grabs into one heartbeat gap, which is the
            // probe's own backlog rather than an application stall.
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        }
    };
    const auto pumpFor = [&](int milliseconds) {
        QElapsedTimer slice;
        slice.start();
        while (slice.elapsed() < milliseconds) sampleTick();
    };
    const auto pumpUntil = [&](const std::function<bool()>& done, int timeoutMs) -> bool {
        QElapsedTimer wait;
        wait.start();
        while (true) {
            sampleTick();
            if (done()) return true;
            if (wait.elapsed() >= timeoutMs) return false;
        }
    };

    // ── GUI heartbeat: the monitored interval's event-loop cadence ─────────
    // A 50 ms high-resolution timer is a heartbeat of the GUI thread itself: a
    // callback later than 250 ms means this process's UI stopped servicing its
    // event loop, whatever the reason. It starts with the monitored window
    // (peer start-up and calibration are pre-monitor) and is never paused or
    // filtered, so an application-side stall cannot be excluded.
    constexpr qint64 kGuiGapLimitMs = 250;
    constexpr int kGuiHeartbeatIntervalMs = 50;
    qint64 lastHeartbeatNs = mikai::gui::uiprofile::nowNs();
    qint64 maxGuiGapMs = 0;
    qint64 guiGapViolations = 0;
    qint64 heartbeatSamples = 0;
    qint64 firstHeartbeatAtMs = -1;
    qint64 lastHeartbeatAtMs = -1;
    // Attribution state for a gap: how much of the interval the probe itself
    // spent in sample ticks versus how many paints the player started.
    quint64 lastGapPaintCalls = surface->uiProfileCounters().paintCalls;
    qint64 lastGapAttempts = 0;
    QTimer guiHeartbeat;
    guiHeartbeat.setTimerType(Qt::PreciseTimer);
    guiHeartbeat.setInterval(kGuiHeartbeatIntervalMs);
    QObject::connect(&guiHeartbeat, &QTimer::timeout, dialog, [&] {
        const qint64 now = mikai::gui::uiprofile::nowNs();
        const qint64 gapMs = (now - lastHeartbeatNs) / 1000000;
        lastHeartbeatNs = now;
        const qint64 atMs = runClock.elapsed();
        if (firstHeartbeatAtMs < 0) firstHeartbeatAtMs = atMs;
        lastHeartbeatAtMs = atMs;
        ++heartbeatSamples;
        maxGuiGapMs = std::max(maxGuiGapMs, gapMs);
        if (gapMs > kGuiGapLimitMs) {
            ++guiGapViolations;
            if (guiGapViolations <= 8) {
                // The gap itself is the finding; these fields say what the
                // thread could have been doing, so the next run can tell an
                // application stall from the probe's own readback/grab work
                // without guessing at the phase name alone.
                const mplayer::VideoSurface::UiProfileCounters counters =
                    surface->uiProfileCounters();
                const quint64 paintsSince = counters.paintCalls - lastGapPaintCalls;
                const qint64 sincePaintMs =
                    counters.lastPaintEntryNs > 0
                        ? mikai::gui::uiprofile::elapsedMs(counters.lastPaintEntryNs)
                        : -1;
                lastGapPaintCalls = counters.paintCalls;
                record(QJsonObject{{"event", "matrixGuiGap"},
                                   {"elapsedMs", atMs},
                                   {"phase", currentPhase},
                                   {"gapMs", gapMs},
                                   {"limitMs", kGuiGapLimitMs},
                                   {"heartbeatIntervalMs", kGuiHeartbeatIntervalMs},
                                   {"position", surface->position()},
                                   {"sampleTicksBetweenHeartbeats",
                                    static_cast<qint64>(sampler.attempts - lastGapAttempts)},
                                   {"lastSampleWorkMs", sampler.lastWorkMs},
                                   {"paintCallsBetweenHeartbeats",
                                    static_cast<qint64>(paintsSince)},
                                   {"msSinceLastPaintEntry", sincePaintMs}});
                lastGapAttempts = sampler.attempts;
            }
        }
    });
    guiHeartbeat.start();

    auto* dock = dialog->findChild<QWidget*>(QStringLiteral("playerDock"));
    auto* settingsButton = dialog->findChild<QWidget*>(QStringLiteral("playerSettingsButton"));
    auto* flyout = dialog->findChild<QWidget*>(QStringLiteral("playerSettingsFlyout"));
    auto* pictureMenu = dialog->findChild<QWidget*>(QStringLiteral("playerPictureMenu"));
    auto* volume = dialog->findChild<mplayer::shell::VolumeGroup*>(
        QStringLiteral("playerVolumeGroup"));
    const bool chromeDevicesBuilt =
        dock != nullptr && settingsButton != nullptr && flyout != nullptr && volume != nullptr;
    record(QJsonObject{{"event", "qaMatrixChromeDevices"},
                       {"elapsedMs", runClock.elapsed()},
                       {"dock", dock != nullptr},
                       {"settingsButton", settingsButton != nullptr},
                       {"settingsFlyout", flyout != nullptr},
                       {"pictureMenu", pictureMenu != nullptr},
                       {"volumeGroup", volume != nullptr},
                       {"built", chromeDevicesBuilt}});
    if (!chromeDevicesBuilt) {
        peer.stop();
        return fail(QStringLiteral("the dock, settings control or volume control was not built, "
                                   "so the chrome budget is unobservable"),
                    "INCOMPLETE");
    }

    int chromeChanges = 0;
    int hoverReveals = 0;
    int hoverHides = 0;
    int panelOpens = 0;
    int panelCloses = 0;
    int controlChanges = 0;
    const auto noteChromeChange = [&](const QString& kind, const QString& detail,
                                      QJsonObject extra) {
        ++chromeChanges;
        extra.insert(QStringLiteral("event"), QStringLiteral("matrixChromeChange"));
        extra.insert(QStringLiteral("elapsedMs"), runClock.elapsed());
        extra.insert(QStringLiteral("phase"), currentPhase);
        extra.insert(QStringLiteral("kind"), kind);
        extra.insert(QStringLiteral("detail"), detail);
        extra.insert(QStringLiteral("chromeChanges"), chromeChanges);
        record(extra);
    };

    const QPoint dockPoint(shell->width() / 2, shell->height() - 42);
    const QPoint picturePoint(dialog->width() / 2, dialog->height() / 3);
    const auto aimAt = [&](QWidget* target, const QPoint& local) {
        if (target == nullptr) return;
        const QPoint global = target->mapToGlobal(local);
        QCursor::setPos(global);
        QCoreApplication::processEvents();
        QMouseEvent enter(QEvent::Enter, QPointF(local), QPointF(global), Qt::NoButton,
                          Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(target, &enter);
        QMouseEvent move(QEvent::MouseMove, QPointF(local), QPointF(global), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(target, &move);
        QCoreApplication::processEvents();
    };
    const auto dockUp = [&] {
        return dock->isVisible() && shell->state() !=
                                        mplayer::shell::PlayerShell::State::FullscreenAutoHidden;
    };
    const auto autoHidden = [&] {
        return shell->state() ==
                   mplayer::shell::PlayerShell::State::FullscreenAutoHidden &&
               !dock->isVisible();
    };

    // ── chrome hover reveal / auto-hide, observed on the shell's own state ─
    currentPhase = QStringLiteral("chrome-hover");
    for (int cycle = 0; cycle < 10; ++cycle) {
        // A predicate that is already true is a state, not a change. Start each
        // cycle from the auto-hidden state so the reveal below is a transition
        // the shell actually made.
        if (dockUp()) {
            QCursor::setPos(dialog->mapToGlobal(picturePoint));
            pumpUntil(autoHidden, 4500);
        }
        const bool dockWasUp = dockUp();
        aimAt(shell, dockPoint);
        const bool revealed = !dockWasUp && pumpUntil(dockUp, 2500) && dockUp();
        if (revealed) {
            ++hoverReveals;
            noteChromeChange(QStringLiteral("hover-reveal"),
                             QStringLiteral("pointer over the dock; the shell left its "
                                            "auto-hidden state and the dock became visible"),
                             QJsonObject{{"shellState", shell->stateName()},
                                         {"dockVisible", dock->isVisible()},
                                         {"stateBefore", dockWasUp},
                                         {"cycle", cycle}});
        }
        // Park over the picture and stop moving: only then can the 2.5 s
        // auto-hide timer fire, and only then is the hide a real state change.
        QCursor::setPos(dialog->mapToGlobal(picturePoint));
        if (revealed && pumpUntil(autoHidden, 4500) && autoHidden()) {
            ++hoverHides;
            noteChromeChange(QStringLiteral("hover-autohide"),
                             QStringLiteral("pointer parked off the chrome; the shell entered "
                                            "FULLSCREEN_AUTO_HIDDEN and the dock hid"),
                             QJsonObject{{"shellState", shell->stateName()},
                                         {"dockVisible", dock->isVisible()},
                                         {"cycle", cycle}});
        }
    }

    // ── settings popover open/close, from a real click on the gear ─────────
    currentPhase = QStringLiteral("chrome-panels");
    for (int cycle = 0; cycle < 4; ++cycle) {
        if (!dockUp()) {
            aimAt(shell, dockPoint);
            pumpUntil(dockUp, 2500);
        }
        // Only a visibility CHANGE is a panel event: a flyout left open by a
        // failed close would otherwise make the next "open" count for free.
        const bool flyoutWasVisible = flyout->isVisible();
        QTest::mouseClick(settingsButton, Qt::LeftButton, Qt::NoModifier,
                          settingsButton->rect().center());
        if (!flyoutWasVisible && pumpUntil([&] { return flyout->isVisible(); }, 2500) &&
            flyout->isVisible()) {
            ++panelOpens;
            noteChromeChange(QStringLiteral("panel-open"),
                             QStringLiteral("settings flyout visible after a click on the gear"),
                             QJsonObject{{"flyoutVisible", flyout->isVisible()},
                                         {"shellHasOverlay", shell->hasVisibleOverlay()},
                                         {"visibleBefore", flyoutWasVisible},
                                         {"cycle", cycle}});
        }
        const bool flyoutWasOpen = flyout->isVisible();
        const bool dismissed = shell->dismissOverlay();
        if (flyoutWasOpen && pumpUntil([&] { return !flyout->isVisible(); }, 2500) &&
            !flyout->isVisible()) {
            ++panelCloses;
            noteChromeChange(QStringLiteral("panel-close"),
                             QStringLiteral("settings flyout hidden by the shell's own dismiss "
                                            "path"),
                             QJsonObject{{"flyoutVisible", flyout->isVisible()},
                                         {"dismissReportedVisibleOverlay", dismissed},
                                         {"visibleBefore", flyoutWasOpen},
                                         {"cycle", cycle}});
        }
        if (!foregroundIs(dialog)) ensureForeground(dialog);
    }
    if (pictureMenu != nullptr) {
        for (int cycle = 0; cycle < 2; ++cycle) {
            aimAt(shell, dockPoint);
            pumpUntil(dockUp, 2500);
            const bool menuWasVisible = pictureMenu->isVisible();
            shell->openPictureMenu(shell->mapFromGlobal(dialog->mapToGlobal(dockPoint)));
            if (!menuWasVisible &&
                pumpUntil([&] { return pictureMenu->isVisible(); }, 2500) &&
                pictureMenu->isVisible()) {
                ++panelOpens;
                noteChromeChange(QStringLiteral("picture-menu-open"),
                                 QStringLiteral("picture context menu visible"),
                                 QJsonObject{{"menuVisible", pictureMenu->isVisible()},
                                             {"visibleBefore", menuWasVisible},
                                             {"cycle", cycle}});
            }
            const bool menuWasOpen = pictureMenu->isVisible();
            shell->dismissOverlay();
            if (menuWasOpen && pumpUntil([&] { return !pictureMenu->isVisible(); }, 2500) &&
                !pictureMenu->isVisible()) {
                ++panelCloses;
                noteChromeChange(QStringLiteral("picture-menu-close"),
                                 QStringLiteral("picture context menu hidden"),
                                 QJsonObject{{"menuVisible", pictureMenu->isVisible()},
                                             {"visibleBefore", menuWasOpen},
                                             {"cycle", cycle}});
            }
            if (!foregroundIs(dialog)) ensureForeground(dialog);
        }
    }

    // ── control state changes: the real speaker hit area toggles mute ──────
    currentPhase = QStringLiteral("chrome-controls");
    for (int cycle = 0; cycle < 6; ++cycle) {
        if (!dockUp()) {
            aimAt(shell, dockPoint);
            pumpUntil(dockUp, 2500);
        }
        const bool mutedBefore = volume->isMuted();
        QTest::mouseClick(volume, Qt::LeftButton, Qt::NoModifier,
                          QPoint(10, volume->height() / 2));
        if (pumpUntil([&] { return volume->isMuted() != mutedBefore; }, 1200)) {
            ++controlChanges;
            noteChromeChange(QStringLiteral("control-state"),
                             QStringLiteral("volume mute toggled by a click on the speaker "
                                            "hit area; the control's own state changed"),
                             QJsonObject{{"mutedBefore", mutedBefore},
                                         {"mutedAfter", volume->isMuted()},
                                         {"effectiveVolume", volume->effective()},
                                         {"cycle", cycle}});
        }
    }
    if (volume->isMuted()) {
        QTest::mouseClick(volume, Qt::LeftButton, Qt::NoModifier,
                          QPoint(10, volume->height() / 2));
        spinFor(120);
    }
    QCursor::setPos(dialog->mapToGlobal(picturePoint));

    // The hover cycles above stop counting once auto-hide no longer happens.
    // Mute toggles are still real control changes, so keep making them until
    // the budget is met instead of ending the section short.
    for (int extra = 0; chromeChanges < kChromeChangesRequired && extra < 12; ++extra) {
        if (!dockUp()) {
            aimAt(shell, dockPoint);
            pumpUntil(dockUp, 2500);
        }
        const bool mutedBefore = volume->isMuted();
        QTest::mouseClick(volume, Qt::LeftButton, Qt::NoModifier,
                          QPoint(10, volume->height() / 2));
        if (pumpUntil([&] { return volume->isMuted() != mutedBefore; }, 1200)) {
            ++controlChanges;
            noteChromeChange(QStringLiteral("control-state"),
                             QStringLiteral("volume mute toggled by a click on the speaker "
                                            "hit area; the control's own state changed"),
                             QJsonObject{{"mutedBefore", mutedBefore},
                                         {"mutedAfter", volume->isMuted()},
                                         {"cycle", 6 + extra}});
        }
    }

    // ── ten verified native foreground away/back cycles ───────────────────
    currentPhase = QStringLiteral("native-foreground");
    ActivationRun activation = runActivationCycles(
        dialog, surface, peer, QStringLiteral("native-foreground"),
        kNativeForegroundCyclesRequired, knownBright, record, pumpFor);
    const bool awayComplete =
        activation.cycles == kNativeForegroundCyclesRequired &&
        activation.foregroundLostCycles == 0 && activation.visibleCompositionTimeouts == 0 &&
        activation.visibleCompositionUnsupported == 0 &&
        activation.positionAdvancedCycles == kNativeForegroundCyclesRequired;
    const bool contextRecreated = activation.initializeGlCalls > 0 ||
                                  activation.contextGenerationChanges > 0;
    bool continuityVerified = true;
    if (contextRecreated) {
        for (int index = 0; index < activation.fboLumaAfterBack.size(); ++index) {
            const double before = index < activation.fboLumaBeforeAway.size()
                                      ? activation.fboLumaBeforeAway.at(index)
                                      : -1.0;
            const double after = activation.fboLumaAfterBack.at(index);
            continuityVerified = continuityVerified && before > 0.0 &&
                                 after >= std::max(30.0, before * 0.5);
        }
    }
    record(QJsonObject{{"event", "qaMatrixAwayBack"},
                       {"elapsedMs", runClock.elapsed()},
                       {"nativeForegroundCycles", activation.cycles},
                       {"nativeForegroundCyclesRequired", kNativeForegroundCyclesRequired},
                       {"foregroundLostCycles", activation.foregroundLostCycles},
                       {"positionAdvancedCycles", activation.positionAdvancedCycles},
                       {"positionStalledCycles", activation.positionStalledCycles},
                       {"peerForegroundDesktopSamples", activation.peerForegroundDesktopSamples},
                       {"peerForegroundBlackDesktopSamples",
                        activation.peerForegroundBlackDesktopSamples},
                       {"focusCycleKind", "native foreground cycle"},
                       {"contextRecreated", contextRecreated},
                       {"initializeGlCalls", static_cast<qint64>(activation.initializeGlCalls)},
                       {"contextGenerationChanges",
                        static_cast<qint64>(activation.contextGenerationChanges)},
                       {"heldPictureContinuityRequired", contextRecreated},
                       {"heldPictureContinuityVerified", continuityVerified},
                       {"complete", awayComplete}});

    // ── five minimize/restore cycles, each short and each observed ─────────
    currentPhase = QStringLiteral("minimize-restore");
    int minimizeCycles = 0;
    int minimizeVerified = 0;
    qint64 totalMinimizedMs = 0;
    qint64 maxMinimizedDwellMs = 0;
    QVector<qint64> restoreVisibleMs;
    for (int cycle = 0; cycle < kMinimizeRestoreCyclesRequired; ++cycle) {
        const double positionBefore = surface->position();
        const QImage pictureBefore = readComposedCrop(surface);
        const double lumaBefore = meanLuma(pictureBefore);
        const double desktopLumaBefore =
            meanLuma(desktopCrop(surface->screen(), videoSampleCrop(surface)));
        dialog->showMinimized();
        const bool minimizedObserved = pumpUntil([&] { return dialog->isMinimized(); }, 3000);
        const qint64 minimizedAt = runClock.elapsed();
        pumpFor(kMinimizeDwellMs);
        const bool stillMinimized = dialog->isMinimized();
        const double positionWhileMinimized = surface->position();

        dialog->showFullScreen();
        bool restored = pumpUntil(
            [&] { return !dialog->isMinimized() && dialog->isVisible(); }, 3000);
        if (!restored) {
            dialog->showNormal();
            pumpFor(200);
            dialog->showFullScreen();
            restored = pumpUntil(
                [&] { return !dialog->isMinimized() && dialog->isVisible(); }, 3000);
        }
        const qint64 restoreAt = runClock.elapsed();
        // Observed-minimized -> visible-again is the real minimized interval, so
        // the max/total dwell gates can actually trip on a window that refused to
        // come back. There is no 5-minute variant: the matrix asks for five short
        // cycles, not one long minimization.
        const qint64 dwellMs = restoreAt - minimizedAt;
        totalMinimizedMs += dwellMs;
        maxMinimizedDwellMs = std::max(maxMinimizedDwellMs, dwellMs);
        const bool foregroundRestored = ensureForeground(dialog);
        const bool pictureBack = pumpUntil(
            [&] {
                return picturePresent(
                    desktopCrop(surface->screen(), videoSampleCrop(surface)),
                    desktopLumaBefore);
            },
            2500);
        const qint64 visibleAfterMs = runClock.elapsed() - restoreAt;
        restoreVisibleMs.append(visibleAfterMs);
        pumpFor(200);
        const double positionAfter = surface->position();
        const QImage pictureAfter = readComposedCrop(surface);
        const double lumaAfter = meanLuma(pictureAfter);
        const bool fullscreenAfter = dialog->isFullScreen();
        const bool positionAdvanced = positionAfter > positionBefore &&
                                      positionWhileMinimized > positionBefore;
        // The FBO reference is this action's own pre-minimize picture, not the
        // opening calibration: a darker-but-valid frame after restore is still
        // a restored picture, and a black target still fails.
        const bool pictureRestored =
            !pictureAfter.isNull() && !isBlack(pictureAfter) &&
            lumaAfter >= std::max(30.0, lumaBefore * 0.5);
        const bool verified = minimizedObserved && stillMinimized && restored &&
                              foregroundRestored && pictureBack && fullscreenAfter &&
                              positionAdvanced && pictureRestored;
        ++minimizeCycles;
        if (verified) ++minimizeVerified;
        record(QJsonObject{{"event", "matrixMinimizeRestore"},
                           {"elapsedMs", runClock.elapsed()},
                           {"cycle", cycle},
                           {"minimizedObserved", minimizedObserved},
                           {"stillMinimizedAfterDwell", stillMinimized},
                           {"minimizedDwellMs", dwellMs},
                           {"restored", restored},
                           {"restoreVisibleMs", visibleAfterMs},
                           {"foregroundRestored", foregroundRestored},
                           {"pictureBack", pictureBack},
                           {"fullscreenAfter", fullscreenAfter},
                           {"positionBefore", positionBefore},
                           {"positionWhileMinimized", positionWhileMinimized},
                           {"positionAfter", positionAfter},
                           {"positionAdvanced", positionAdvanced},
                           {"lumaBefore", lumaBefore},
                           {"desktopLumaBefore", desktopLumaBefore},
                           {"lumaAfter", lumaAfter},
                           {"pictureRestored", pictureRestored},
                           {"excludedMinimizedSamplesSoFar", sampler.excludedMinimized},
                           {"minimizedSamplesExcludedFromPicture", true},
                           {"verified", verified}});
    }
    const bool minimizeComplete =
        minimizeCycles == kMinimizeRestoreCyclesRequired &&
        minimizeVerified == kMinimizeRestoreCyclesRequired &&
        maxMinimizedDwellMs <= kMaxMinimizeDwellMs && totalMinimizedMs <= kMaxTotalMinimizedMs;
    record(QJsonObject{{"event", "qaMatrixMinimizeSummary"},
                       {"elapsedMs", runClock.elapsed()},
                       {"cycles", minimizeCycles},
                       {"verifiedCycles", minimizeVerified},
                       {"requiredCycles", kMinimizeRestoreCyclesRequired},
                       {"totalMinimizedMs", totalMinimizedMs},
                       {"maxMinimizedDwellMs", maxMinimizedDwellMs},
                       {"maxMinimizedDwellLimitMs", kMaxMinimizeDwellMs},
                       {"minimizedDwellDefinition",
                        "observed minimized -> visible again; the max and total limits are "
                        "the assertions, there is no 5-minute variant"},
                       {"restoreVisibleMaxMs", restoreVisibleMs.isEmpty()
                                                   ? QJsonValue(QStringLiteral("unavailable"))
                                                   : QJsonValue(*std::max_element(
                                                         restoreVisibleMs.begin(),
                                                         restoreVisibleMs.end()))},
                       {"excludedMinimizedSamples", sampler.excludedMinimized},
                       {"complete", minimizeComplete}});

    // ── fullscreen seams and one geometry-write edge ───────────────────────
    currentPhase = QStringLiteral("presentation-seams");
    const qint64 seamBlackBefore = sampler.fboBlack + sampler.desktopBlack;
    const quint64 geometryWritesBefore = mikai::gui::uiprofile::snapshot().layoutGeometryWrites;
    const quint64 masksBefore = mikai::gui::uiprofile::snapshot().setMaskApplications;
    int seamTransitionsObserved = 0;
    int seamResizesObserved = 0;
    quint64 firstTransitionGeometryWrites = 0;
    bool geometryEdgeMeasured = false;
    QVector<qint64> seamStateMs;
    QVector<qint64> seamVisibleMs;
    QVector<quint64> seamGeometryWrites;
    for (int cycle = 0; cycle < kFullscreenSeamTransitions; ++cycle) {
        for (int phase = 0; phase < 2; ++phase) {
            const quint64 writesBefore =
                mikai::gui::uiprofile::snapshot().layoutGeometryWrites;
            // The pre-transition picture is already bright, so a luma-only test
            // would pass on the very first tick and measure nothing. First wait
            // for the actual window state AND the surface geometry change, then
            // start the visible-composition clock from that observed state.
            const QSize surfaceSizeBefore = surface->size();
            const QSize dialogSizeBefore = dialog->size();
            const double seamReferenceLuma =
                meanLuma(desktopCrop(surface->screen(), videoSampleCrop(surface)));
            QElapsedTimer seamClock;
            seamClock.start();
            if (phase == 0) {
                dialog->showNormal();
            } else {
                dialog->showFullScreen();
            }
            const bool stateAndGeometry = pumpUntil(
                [&] {
                    const bool rightState =
                        phase == 1 ? dialog->isFullScreen() : !dialog->isFullScreen();
                    return rightState &&
                           (surface->size() != surfaceSizeBefore ||
                            dialog->size() != dialogSizeBefore);
                },
                2500);
            seamStateMs.append(seamClock.elapsed());
            if (stateAndGeometry) ++seamResizesObserved;
            QElapsedTimer seamVisibleClock;
            seamVisibleClock.start();
            const bool composed = stateAndGeometry && pumpUntil(
                [&] {
                    return picturePresent(
                        desktopCrop(surface->screen(), videoSampleCrop(surface)),
                        seamReferenceLuma);
                },
                2500);
            seamVisibleMs.append(seamVisibleClock.elapsed());
            const quint64 writesAfter =
                mikai::gui::uiprofile::snapshot().layoutGeometryWrites;
            seamGeometryWrites.append(writesAfter - writesBefore);
            if (!geometryEdgeMeasured) {
                firstTransitionGeometryWrites = writesAfter - writesBefore;
                geometryEdgeMeasured = true;
            }
            if (composed) ++seamTransitionsObserved;
            if (!foregroundIs(dialog)) ensureForeground(dialog);
        }
    }
    const quint64 geometryWritesAfter = mikai::gui::uiprofile::snapshot().layoutGeometryWrites;
    const quint64 masksAfter = mikai::gui::uiprofile::snapshot().setMaskApplications;
    const qint64 seamBlackAfter = sampler.fboBlack + sampler.desktopBlack;
    // Exactly one surface geometry write per fullscreen edge, EVERY edge. A
    // mean or a first-edge-only check would allow one edge to pay two writes
    // while another pays none.
    QJsonArray seamGeometryWriteArray;
    bool geometryWritesExactlyOne =
        seamGeometryWrites.size() == 2 * kFullscreenSeamTransitions;
    for (quint64 writes : seamGeometryWrites) {
        seamGeometryWriteArray.append(static_cast<qint64>(writes));
        geometryWritesExactlyOne = geometryWritesExactlyOne && writes == 1;
    }

    // ── PiP round trip: the float's picture is sampled, not assumed ────────
    const double pipPositionBefore = surface->position();
    const double pipReferenceLuma =
        meanLuma(desktopCrop(surface->screen(), videoSampleCrop(surface)));
    dialog->togglePip();
    const bool pipSurfaceReady = pumpUntil(
        [&] {
            mplayer::VideoSurface* owner = activePlaybackSurface(dialog, surface);
            return dialog->isPipActive() && owner != nullptr && owner != surface &&
                   owner->hasVideo();
        },
        8000);
    mplayer::VideoSurface* pipOwner = activePlaybackSurface(dialog, surface);
    pumpFor(400);
    // The float's picture is an FBO crop, so its reference is an FBO crop of the
    // same float: comparing it against the main window's desktop luma would mix
    // crop kinds and regions and could fail a valid picture.
    const double pipFboReference =
        pipOwner != nullptr ? meanLuma(readComposedCrop(pipOwner)) : -1.0;
    const double pipPositionAtStart = pipOwner != nullptr ? pipOwner->position() : -1.0;
    // The float's own core starts at the main position and has to keep playing;
    // a fixed sleep would turn a slow first decode into a false "no motion".
    const bool pipAdvanced = pipPositionAtStart >= 0.0 &&
                             pumpUntil([&] {
                                 return pipOwner->position() > pipPositionAtStart + 0.05;
                             }, 6000);
    const double pipPositionAtEnd = pipOwner != nullptr ? pipOwner->position() : -1.0;
    const QImage pipPicture = pipOwner != nullptr ? readComposedCrop(pipOwner) : QImage();
    dialog->togglePip();
    const bool mainSurfaceBack = pumpUntil(
        [&] { return !dialog->isPipActive() && surface->isVisible(); }, 8000);
    ensureForeground(dialog);
    pumpFor(500);
    const double pipPositionAfter = surface->position();
    const bool pictureAfterPip = pumpUntil(
        [&] {
            return picturePresent(
                desktopCrop(surface->screen(), videoSampleCrop(surface)),
                pipReferenceLuma);
        },
        2500);
    const bool pipAdvance = pipAdvanced && pipPositionAtEnd > pipPositionAtStart;
    const bool pipPicturePresent = picturePresent(pipPicture, pipFboReference);
    const bool pipVerified = pipSurfaceReady && pipOwner != nullptr && pipOwner != surface &&
                             pipPicturePresent && pipAdvance && mainSurfaceBack &&
                             pictureAfterPip && dialog->isFullScreen() &&
                             pipPositionAfter >= pipPositionAtStart;
    record(QJsonObject{{"event", "qaMatrixPresentationSeams"},
                       {"elapsedMs", runClock.elapsed()},
                       {"fullscreenTransitions", 2 * kFullscreenSeamTransitions},
                       {"transitionsObserved", seamTransitionsObserved},
                       {"resizesObserved", seamResizesObserved},
                       {"seamStateChangeMaxMs",
                        seamStateMs.isEmpty()
                            ? QJsonValue(QStringLiteral("unavailable"))
                            : QJsonValue(*std::max_element(seamStateMs.begin(),
                                                           seamStateMs.end()))},
                       {"seamVisibleAfterStateMaxMs",
                        seamVisibleMs.isEmpty()
                            ? QJsonValue(QStringLiteral("unavailable"))
                            : QJsonValue(*std::max_element(seamVisibleMs.begin(),
                                                          seamVisibleMs.end()))},
                       {"seamDefinition",
                        "state (fullscreen flag) and surface/window geometry change are "
                        "waited for and required before the visible-composition clock starts; "
                        "the sample must then show a picture against the pre-action desktop "
                        "luma, not the opening calibration"},
                       {"seamBlackSamples", seamBlackAfter - seamBlackBefore},
                       {"seamSamplesExposed", sampler.exposedSamples},
                       {"layoutGeometryWrites",
                        static_cast<qint64>(geometryWritesAfter - geometryWritesBefore)},
                       {"layoutGeometryWritesFirstTransition",
                        static_cast<qint64>(firstTransitionGeometryWrites)},
                       {"layoutGeometryWritesPerTransition",
                        static_cast<double>(geometryWritesAfter - geometryWritesBefore) /
                            (2.0 * kFullscreenSeamTransitions)},
                       {"geometryWriteEdgeMeasured", geometryEdgeMeasured},
                       {"layoutGeometryWritesPerEdge", seamGeometryWriteArray},
                       {"layoutGeometryWritesEachEdgeExactlyOne",
                        geometryWritesExactlyOne},
                       {"layoutGeometryWritesPerEdgeRequired", 1},
                       {"setMaskApplications",
                        static_cast<qint64>(masksAfter - masksBefore)},
                       {"pipSurfaceReady", pipSurfaceReady},
                       {"pipPositionStart", pipPositionAtStart},
                       {"pipPositionEnd", pipPositionAtEnd},
                       {"pipPositionAfterExit", pipPositionAfter},
                       {"pipPictureBlack", pipPicture.isNull() ? QJsonValue(QStringLiteral("unmeasured"))
                                                              : QJsonValue(isBlack(pipPicture))},
                       {"pipPicturePresent", pipPicturePresent},
                       {"pipFboReferenceLuma", pipFboReference},
                       {"pipDesktopReferenceLuma", pipReferenceLuma},
                       {"pipPositionBeforeExit", pipPositionBefore},
                       {"mainSurfaceBack", mainSurfaceBack},
                       {"pictureAfterPip", pictureAfterPip},
                       {"pipVerified", pipVerified}});

    // ── finish the 300 s window with continuous idle sampling ──────────────
    currentPhase = QStringLiteral("idle-sampling");
    const qint64 idleStartMs = runClock.elapsed();
    const qint64 exposedAtIdleStart = sampler.exposedSamples;
    const qint64 deadlineMs = static_cast<qint64>(seconds) * 1000;
    while (runClock.elapsed() < deadlineMs) sampleTick();
    const qint64 idleMs = runClock.elapsed() - idleStartMs;
    const qint64 idleSamples = sampler.exposedSamples - exposedAtIdleStart;
    const qint64 idleSamplesExpected = idleMs / 32;
    const bool idleCadenceSatisfied =
        idleMs < 2000 || idleSamples * 10 >= idleSamplesExpected * 9;

    // ── final motion: the picture itself still changes ─────────────────────
    currentPhase = QStringLiteral("final-motion");
    pumpFor(150);
    mplayer::VideoSurface* finalOwner = activePlaybackSurface(dialog, surface);
    const QImage motionFirst =
        finalOwner != nullptr ? readComposedCrop(finalOwner) : QImage();
    const double motionPositionFirst = surface->position();
    const quint64 motionFramesFirst = surface->playbackDiagnostics().renderedOutputFrames;
    // One pair is not a motion sample. The fixture is 24 fps CFR and can hold a
    // single scene for many frames, so two consecutive reads are often the same
    // picture while the film plays: the 300 s matrix recorded +0.33 s of media
    // and twelve new output frames with a fraction of 0 from exactly that. Each
    // pair still spans a real output frame; up to kMotionPairs pairs are taken
    // inside a bounded window and every fraction is recorded, so "no motion"
    // rests on the whole window rather than on one unlucky pair. The 1 %
    // threshold, the black test and both advance requirements are unchanged.
    constexpr int kMotionPairs = 8;
    QJsonArray motionPairs;
    double motionFraction = -1.0;
    QImage motionSecond;
    double motionPositionSecond = motionPositionFirst;
    quint64 motionFramesSecond = motionFramesFirst;
    for (int pair = 0; pair < kMotionPairs; ++pair) {
        const QImage before = pair == 0 && !motionFirst.isNull()
                                  ? motionFirst
                                  : (finalOwner != nullptr ? readComposedCrop(finalOwner)
                                                           : QImage());
        const double positionBeforePair = surface->position();
        const quint64 framesBefore = surface->playbackDiagnostics().renderedOutputFrames;
        // Elapsed time alone does not establish that a new output frame arrived.
        pumpUntil([&] {
            return surface->playbackDiagnostics().renderedOutputFrames > framesBefore;
        }, 1000);
        const QImage after = finalOwner != nullptr ? readComposedCrop(finalOwner) : QImage();
        motionPositionSecond = surface->position();
        motionFramesSecond = surface->playbackDiagnostics().renderedOutputFrames;
        const double pairFraction = frameDifferenceFraction(before, after);
        motionPairs.append(QJsonObject{
            {"pair", pair},
            {"framesAdvanced", static_cast<qint64>(motionFramesSecond - framesBefore)},
            {"positionFrom", positionBeforePair},
            {"positionTo", motionPositionSecond},
            {"changedFraction", pairFraction}});
        motionFraction = std::max(motionFraction, pairFraction);
        motionSecond = after;
        if (motionFraction >= 0.01) break;
        if (motionFramesSecond <= motionFramesFirst) break;
    }
    const bool motionObserved = motionFraction >= 0.01 &&
                                motionPositionSecond > motionPositionFirst &&
                                motionFramesSecond > motionFramesFirst &&
                                !isBlack(motionSecond);

    // The monitored window ends with the final motion sample: stop the
    // heartbeat here so artifact settling is not charged to the UI cadence.
    guiHeartbeat.stop();
    const bool guiHeartbeatPresent = heartbeatSamples > 0;
    const bool guiGapBounded = maxGuiGapMs <= kGuiGapLimitMs && guiGapViolations == 0;
    collectingArtifacts = false;
    QElapsedTimer settle;
    settle.start();
    const bool artifactWritesSettled = artifactPool.waitForDone(15000);
    const qint64 artifactSettleMs = settle.elapsed();
    const EvidenceWriteSnapshot artifactSnapshot = artifactWrites.snapshot();

    const qint64 elapsedMs = runClock.elapsed();
    const double elapsedSeconds = elapsedMs / 1000.0;
    const double positionEnd = surface->position();
    const double positionAdvance = positionEnd - positionStart;
    const quint64 framesEnd = surface->playbackDiagnostics().renderedOutputFrames;
    const quint64 sourceEpochAtEnd = surface->sourceEpoch();
    const auto countersAtEnd = surface->uiProfileCounters();
    const auto profileAtEnd = mikai::gui::uiprofile::snapshot();
    const bool durationSatisfied = elapsedMs >= deadlineMs;
    const bool mediaAdvanceSatisfied =
        durationAtStart > 0.0 && positionAdvance >= 0.85 * elapsedSeconds;
    const bool chromeSatisfied = chromeChanges >= kChromeChangesRequired &&
                                 hoverReveals > 0 && hoverHides > 0 && panelOpens > 0 &&
                                 panelCloses > 0 && controlChanges > 0;
    const bool blackClean = sampler.fboBlack == 0 && sampler.desktopBlack == 0 &&
                            activation.blackSamples == 0 && sampler.fboSamples > 0 &&
                            sampler.desktopSamples > 0;
    const bool samplingCoverage = sampler.fboUnmeasured == 0 && sampler.desktopUnmeasured == 0 &&
                                  sampler.exposedSamples > 0 && idleCadenceSatisfied;
    QStringList coverageGaps;
    if (sampler.fboUnmeasured > 0)
        coverageGaps.append(QStringLiteral("composed framebuffer reads returned no pixels %1 "
                                           "times while the surface claimed a visible frame")
                                .arg(sampler.fboUnmeasured));
    if (sampler.desktopUnmeasured > 0)
        coverageGaps.append(QStringLiteral("desktop grabs returned no pixels %1 times")
                                .arg(sampler.desktopUnmeasured));
    if (!idleCadenceSatisfied)
        coverageGaps.append(QStringLiteral("idle sampling kept the 32 ms cadence for only %1 of "
                                           "%2 expected ticks")
                                .arg(idleSamples)
                                .arg(idleSamplesExpected));
    if (!geometryEdgeMeasured)
        coverageGaps.append(QStringLiteral("no fullscreen geometry edge was measured"));
    const bool passed = durationSatisfied && mediaAdvanceSatisfied && chromeSatisfied &&
                        awayComplete && continuityVerified && minimizeComplete &&
                        seamTransitionsObserved == 2 * kFullscreenSeamTransitions &&
                        geometryWritesExactlyOne && seamBlackAfter == seamBlackBefore &&
                        pipVerified && blackClean && samplingCoverage &&
                        coverageGaps.isEmpty() && motionObserved && guiHeartbeatPresent &&
                        guiGapBounded && (host == nullptr || !host->failed);
    const QString result = passed ? QStringLiteral("PASS")
                                  : (coverageGaps.isEmpty() ? QStringLiteral("FAIL")
                                                            : QStringLiteral("INCOMPLETE"));
    record(QJsonObject{
        {"event", "qaMatrixResult"},
        {"result", result},
        {"mode", "qa-matrix"},
        {"requestedSeconds", seconds},
        {"elapsedMs", elapsedMs},
        {"elapsedSeconds", elapsedSeconds},
        {"monitoredStartMs", monitoredStartMs},
        {"monitoredDurationMs", elapsedMs - monitoredStartMs},
        {"knownBrightFixture", knownBright},
        {"calibratedFboLuma", calibratedLuma},
        {"calibratedDesktopLuma", calibratedDesktopLuma},
        {"brightThreshold", brightThreshold},
        {"mediaFacts", mediaFacts(media)},
        {"durationSeconds", durationAtStart},
        {"positionStart", positionStart},
        {"positionEnd", positionEnd},
        {"positionAdvanceSeconds", positionAdvance},
        {"positionAdvanceRatio", elapsedSeconds > 0.0 ? positionAdvance / elapsedSeconds : -1.0},
        {"renderedOutputFramesStart", static_cast<qint64>(framesStart)},
        {"renderedOutputFramesEnd", static_cast<qint64>(framesEnd)},
        {"renderedOutputFramesAdvance", static_cast<qint64>(framesEnd - framesStart)},
        {"durationSatisfied", durationSatisfied},
        {"mediaAdvanceSatisfied", mediaAdvanceSatisfied},
        {"chromeStateChanges", chromeChanges},
        {"chromeStateChangesRequired", kChromeChangesRequired},
        {"chromeHoverReveals", hoverReveals},
        {"chromeHoverAutoHides", hoverHides},
        {"chromePanelOpens", panelOpens},
        {"chromePanelCloses", panelCloses},
        {"chromeControlStateChanges", controlChanges},
        {"chromeSatisfied", chromeSatisfied},
        {"nativeForegroundCycles", activation.cycles},
        {"nativeForegroundCyclesRequired", kNativeForegroundCyclesRequired},
        {"nativeForegroundCyclesVerified",
         activation.cycles - activation.foregroundLostCycles},
        {"nativeForegroundPositionAdvancedCycles", activation.positionAdvancedCycles},
        {"focusCycleKind", "native foreground cycle"},
        {"focusCycleMechanism",
         "SetForegroundWindow on a separate helper process's window; no synthetic keyboard "
         "input, so it is not an Alt-Tab reproduction"},
        {"peerForegroundDesktopSamples", activation.peerForegroundDesktopSamples},
        {"peerForegroundBlackDesktopSamples", activation.peerForegroundBlackDesktopSamples},
        {"peerPixelsExcludedFromPicture", true},
        {"awayBackComplete", awayComplete},
        {"minimizeRestoreCycles", minimizeCycles},
        {"minimizeRestoreCyclesVerified", minimizeVerified},
        {"minimizeRestoreCyclesRequired", kMinimizeRestoreCyclesRequired},
        {"totalMinimizedMs", totalMinimizedMs},
        {"maxMinimizedDwellMs", maxMinimizedDwellMs},
        {"minimizedSamplesExcluded", sampler.excludedMinimized},
        {"minimizedDwellDefinition",
         "observed minimized -> visible again; the max and total limits are the assertions, "
         "there is no 5-minute variant"},
        {"minimizeComplete", minimizeComplete},
        {"fullscreenSeamTransitions", seamTransitionsObserved},
        {"fullscreenSeamTransitionsRequired", 2 * kFullscreenSeamTransitions},
        {"seamBlackSamples", seamBlackAfter - seamBlackBefore},
        {"surfaceGeometryWritesPerFullscreenTransition",
         static_cast<double>(geometryWritesAfter - geometryWritesBefore) /
             (2.0 * kFullscreenSeamTransitions)},
        {"layoutGeometryWrites", static_cast<qint64>(geometryWritesAfter - geometryWritesBefore)},
        {"geometryWriteEdgeMeasured", geometryEdgeMeasured},
        {"firstTransitionGeometryWrites",
         static_cast<qint64>(firstTransitionGeometryWrites)},
        {"setMaskApplications", static_cast<qint64>(masksAfter - masksBefore)},
        {"pipVerified", pipVerified},
        {"exposedSamples", sampler.exposedSamples},
        {"exposedFboSamples", sampler.fboSamples},
        {"blackFboSamples", sampler.fboBlack},
        {"fboUnmeasuredSamples", sampler.fboUnmeasured},
        {"exposedDesktopSamples", sampler.desktopSamples},
        {"blackDesktopSamples", sampler.desktopBlack},
        {"desktopUnmeasuredSamples", sampler.desktopUnmeasured},
        {"excludedPeerForegroundSamples", sampler.excludedPeerForeground},
        {"excludedMinimizedSamples", sampler.excludedMinimized},
        {"excludedNotExposedSamples", sampler.excludedNotExposed},
        {"excludedForeignForegroundSamples", sampler.excludedForeignForeground},
        {"excludedNoVisibleOutputFrameSamples", sampler.excludedNoVisibleOutputFrame},
        {"blackSamplesStrictWhileVisible",
         static_cast<qint64>(sampler.fboBlack + sampler.desktopBlack +
                             activation.blackSamples)},
        {"peerBlackSamplesExcluded",
         static_cast<qint64>(activation.peerForegroundBlackDesktopSamples)},
        {"peerForegroundSamplesExcludedTotal",
         static_cast<qint64>(sampler.excludedPeerForeground +
                             activation.peerForegroundDesktopSamples)},
        {"enginePreparationSynthesized", false},
        {"longPlaybackOnly", true},
        {"pictureAccountingPolicy",
         "a tick made while the separate helper owned the foreground, while the player was "
         "minimized, while another application was foreground, or while the active surface "
         "claimed no visible output frame is counted under that reason and is never judged "
         "black or clean; only exposed ticks are"},
        {"desktopLumaMean", sampler.desktopLumaCount > 0
                                ? QJsonValue(sampler.desktopLumaTotal / sampler.desktopLumaCount)
                                : QJsonValue(QStringLiteral("unavailable"))},
        {"desktopGrabMeanMicros", sampler.grabCalls == 0
                                      ? QJsonValue(QStringLiteral("unavailable"))
                                      : QJsonValue(static_cast<double>(sampler.grabTotalMicros) /
                                                   sampler.grabCalls)},
        {"desktopGrabMaxMicros", sampler.grabMaxMicros},
        {"readbackMeanMicros", sampler.readbackCalls == 0
                                   ? QJsonValue(QStringLiteral("unavailable"))
                                   : QJsonValue(static_cast<double>(sampler.readbackTotalMicros) /
                                                sampler.readbackCalls)},
        {"readbackMaxMicros", sampler.readbackMaxMicros},
        {"maxSampleWorkMs", sampler.maxWorkMs},
        {"guiHeartbeatIntervalMs", kGuiHeartbeatIntervalMs},
        {"guiHeartbeatSamples", heartbeatSamples},
        {"guiHeartbeatFirstAtMs", firstHeartbeatAtMs},
        {"guiHeartbeatLastAtMs", lastHeartbeatAtMs},
        {"maxGuiGapMs", maxGuiGapMs},
        {"guiGapLimitMs", kGuiGapLimitMs},
        {"guiGapViolations", guiGapViolations},
        {"guiGapBounded", guiGapBounded},
        {"guiHeartbeatScope",
         "50 ms PreciseTimer on the GUI thread from monitoredStartMs to the final motion "
         "sample; peer start-up and pre-run calibration are before the monitored window, "
         "nothing else is paused or filtered, and the gap is event-loop cadence, not "
         "scanout"},
        {"firstSampleAtMs", sampler.firstSampleAtMs},
        {"lastSampleAtMs", sampler.lastSampleAtMs},
        {"idleSamplingMs", idleMs},
        {"idleSamplingSamples", idleSamples},
        {"idleSamplingExpected", idleSamplesExpected},
        {"idleCadenceSatisfied", idleCadenceSatisfied},
        {"samplingCoverageComplete", samplingCoverage},
        {"blackCleanWhileVisible", blackClean},
        {"contextGenerationStart", static_cast<qint64>(sampler.contextObserved
                                                          ? sampler.contextGenerationStart
                                                          : 0)},
        {"contextGenerationEnd",
         static_cast<qint64>(surface->uiProfileCounters().contextGeneration)},
        {"contextGenerationChanges", static_cast<qint64>(sampler.contextGenerationChanges)},
        {"initializeGlCalls", static_cast<qint64>(countersAtEnd.initializeGlCalls -
                                                  countersAtStart.initializeGlCalls)},
        {"sourceEpochStart", static_cast<qint64>(sourceEpochAtStart)},
        {"sourceEpochEnd", static_cast<qint64>(sourceEpochAtEnd)},
        {"sourceEpochChanges", static_cast<qint64>(sampler.sourceEpochChanges)},
        {"heldPictureContinuityRequired", contextRecreated},
        {"heldPictureContinuityVerified", continuityVerified},
        {"contextRecreatedAnywhere",
         contextRecreated || sampler.contextGenerationChanges > 0 ||
             countersAtEnd.contextGeneration != countersAtStart.contextGeneration},
        {"heldPictureContinuityEvidence",
         "away/back cycles compare the composed picture before the helper covers it with the "
         "picture the same surface presents after the player takes it back; the whole run "
         "also requires zero black on every exposed tick, so a recreated context that left "
         "the target blank could not pass"},
        {"layoutGeometryWritesProcessTotal",
         static_cast<qint64>(profileAtEnd.layoutGeometryWrites -
                             profileAtStart.layoutGeometryWrites)},
        {"setMaskApplicationsProcessTotal",
         static_cast<qint64>(profileAtEnd.setMaskApplications - profileAtStart.setMaskApplications)},
        {"chromePaintCalls", static_cast<qint64>(profileAtEnd.chromePaintCalls -
                                                 profileAtStart.chromePaintCalls)},
        {"finalMotionFraction", motionFraction},
        {"finalMotionObserved", motionObserved},
        {"finalMotionPairs", motionPairs},
        {"finalMotionPairLimit", kMotionPairs},
        {"finalMotionDefinition",
         "up to eight pairs, each spanning a real rendered output frame; the best pair's "
         "changed fraction is compared against 1 %, so a source that holds one scene still "
         "for a few frames cannot be reported as frozen on one unlucky read"},
        {"finalPositionDeltaSeconds", motionPositionSecond - motionPositionFirst},
        {"maxImageSaveMs", artifactSnapshot.maxWriteMs},
        {"artifactWritesAttempted", artifactSnapshot.attempted},
        {"artifactWritesCompleted", artifactSnapshot.completed},
        {"artifactWritesFailed", artifactSnapshot.failed},
        {"artifactWritesSkipped", artifactSnapshot.skipped},
        {"artifactWritesSettled", artifactWritesSettled},
        {"artifactSettleMs", artifactSettleMs},
        {"failedArtifacts", QJsonArray::fromStringList(artifactSnapshot.failedArtifacts)},
        {"coverageGaps", QJsonArray::fromStringList(coverageGaps)},
        {"samplingDefinition",
         "composed framebuffer centre crop plus a real desktop grab of the same rect, "
         "roughly every 32 ms on exposed ticks while the pump is running; excluded ticks "
         "are counted by reason and never judged for black"},
        {"samplingCadenceScope",
         "pumped through the away/back helper as well: its fixed settles, its foreground "
         "retries and its activation-latency poll all tick the same ~32 ms sampler, and the "
         "minimize-restore fallback spin does too. Calibration and the separate-process "
         "peer start-up run before monitoredStartMs and are pre-monitor, not monitored gaps; "
         "the only unsampled time inside the monitored window is a post-action foreground "
         "retry spin, named in unsampledWindows"},
        {"samplingCadenceGroups", QJsonArray{QStringLiteral("chrome"),
                                             QStringLiteral("seams"),
                                             QStringLiteral("minimize"),
                                             QStringLiteral("away-back"),
                                             QStringLiteral("pip"),
                                             QStringLiteral("idle")}},
        {"unsampledWindows", QJsonArray{QStringLiteral("post-action-foreground-retry-spins")}},
        {"preMonitorWindows", QJsonArray{QStringLiteral("calibration"),
                                         QStringLiteral("peer-warmup")}},
        {"samplingLatencyNote",
         "inside the away/back poll the sampler tick sets the poll granularity (~32 ms "
         "instead of 6 ms); the matrix does not gate on those latency quantiles"},
        {"presentationMode", presentationMode()},
        {"surfaceState", surfaceState(surface)},
        {"error", *error}});
    peer.stop();
    if (!passed && error != nullptr) {
        *error = QStringLiteral("QA matrix %1: chrome=%2/%3 away=%4/%5 minimize=%6/%7 "
                                "seams=%8/%9 pip=%10 black=%11 motion=%12 maxGuiGap=%13/%14 "
                                "edgeWritesExactlyOne=%15")
                     .arg(result)
                     .arg(chromeChanges)
                     .arg(kChromeChangesRequired)
                     .arg(activation.cycles - activation.foregroundLostCycles)
                     .arg(kNativeForegroundCyclesRequired)
                     .arg(minimizeVerified)
                     .arg(kMinimizeRestoreCyclesRequired)
                     .arg(seamTransitionsObserved)
                     .arg(2 * kFullscreenSeamTransitions)
                     .arg(pipVerified)
                     .arg(sampler.fboBlack + sampler.desktopBlack)
                     .arg(motionObserved)
                     .arg(maxGuiGapMs)
                     .arg(kGuiGapLimitMs)
                     .arg(geometryWritesExactlyOne);
    }
    return passed ? 0 : 3;
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
    bool qaMatrix = false;
    bool hoverCanary = false;
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
        } else if (argument == QLatin1String("--qa-matrix")) {
            qaMatrix = true;
        } else if (argument == QLatin1String("--hover-canary")) {
            hoverCanary = true;
        } else {
            positionalArguments.append(argument);
        }
    }
    if (focusSwitch && steadyFullscreen) {
        std::fprintf(stderr, "--focus-switch and --steady-fullscreen are separate modes\n");
        return 2;
    }
    // Each mode measures a different thing; a run that asked for two would
    // report one mode's numbers under the other's name.
    const int selectedModes = int(focusSwitch) + int(baseline) + int(qaMatrix) +
                              int(steadyFullscreen) + int(hoverCanary);
    if (selectedModes > 1) {
        std::fprintf(stderr, "--focus-switch, --baseline, --qa-matrix, --steady-fullscreen and "
                             "--hover-canary are separate modes\n");
        return 2;
    }
    if (positionalArguments.size() < 3 || positionalArguments.size() > 4) {
        std::fprintf(stderr,
                     "Usage: fullscreen_stability_probe <video> <seconds> <output-directory> "
                     "[anime4k-preset] [--known-bright] [--steady-fullscreen] "
                     "[--focus-switch] [--baseline] [--qa-matrix] [--hover-canary]\n");
        return 2;
    }
    if (focusSwitch || baseline || qaMatrix || hoverCanary) {
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
    // The QA matrix measures for the full requested window and then finalizes;
    // its mandatory interaction budget runs inside that window, so the isolation
    // watchdog only needs headroom for dialog start-up and the final motion
    // sample, not for stretching the 300 s itself.
    const int isolationTimeoutMs = seconds * 1000 + (qaMatrix ? 240000 : 60000);
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
    if (hoverCanary) {
        // A CI canary must not take the desktop: no raise, no activation and no
        // system-cursor move. It runs as an ordinary window whose gestures are
        // delivered to its widgets, so an unattended run cannot steal focus from
        // whoever owns the machine.
        dialog->showNormal();
        if (dialog->isFullScreen()) shell->windowModeToggled();
        dialog->resize(960, 540);
    } else {
        dialog->showFullScreen();
        dialog->raise();
        dialog->activateWindow();
        QCursor::setPos(dialog->mapToGlobal(QPoint(2, 2)));
    }

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
    if (focusSwitch || baseline || qaMatrix || hoverCanary) {
        QString modeError;
        const int modeExit =
            focusSwitch
                ? runFocusSwitchMode(dialog, surface, shell, knownBright, seconds, record,
                                     &modeError)
            : baseline
                ? runBaselineMode(dialog, surface, shell, media, knownBright, seconds, record,
                                  &modeError)
            : hoverCanary
                ? runHoverCanaryMode(dialog, surface, shell, media, seconds, outputPath, record,
                                     &modeError)
                : runQaMatrixMode(dialog, surface, shell, media, outputPath, &host, knownBright,
                                  seconds, record, &modeError);
        evidence.flush();
        // The canary closes its own dialog so its timing-log window is flushed
        // before it reads that window back; the other modes are closed here.
        if (!hoverCanary) dialog->close();
        // The mode's verdict is explicit: 0 measured, 3 incomplete/invalid.
        return modeExit;
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
