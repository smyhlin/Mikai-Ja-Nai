// GOAL-E / T-E1, T-E7: the load-transition content and chrome-paint baseline.
//
// Observation only: verdicts force no paint, samples are paint-level readbacks
// of the surface's own framebuffer (not proof of scanned-out pixels), and the
// six judged regions are off-centre so a bright opening placeholder cannot pass.
// A bounded resize stage then reports how many times 200 resizes of the steady
// picture re-query the render target's attachment format.
//
// Usage:
//   LoadTransitionProbe <video-file> --output-dir <absolute dir>
//                        [--delay-ms N] [--cycles N] [--no-motion]

#include <QApplication>
#include <QCoreApplication>
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
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "DelayedHttpSource.hpp"
#include "ProbeRuntime.hpp"
#include "PlayerDialog.hpp"
#include "VideoSurface.hpp"
#include "gui/UiProfile.hpp"

using namespace mplayer;
using namespace mplayer::shell;
using mplayer::probes::DelayedHttpSource;

namespace {

int g_checks = 0;
int g_failures = 0;
QString g_outputDir;

void check(bool ok, const QString& what) {
    ++g_checks;
    std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok) ++g_failures;
}

void metric(const QString& text) { std::fprintf(stderr, "     %s\n", qPrintable(text)); }

QString artifactPath(const QString& name) {
    if (g_outputDir.isEmpty() || QFileInfo(name).isAbsolute()) return name;
    return g_outputDir + QLatin1Char('/') + name;
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

struct StubHost : PlayerHost {
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    void playbackFailed() override {}
};

// ── the picture band, read without asking for a paint ─────────────────────

// Off-centre and clear of the chrome that legitimately changes: the opening
// placeholder is bright in the middle, so a centre-only check could pass it.
constexpr int kRegionCount = 6;
struct RegionSpot {
    double fx;
    double fy;
};
constexpr std::array<RegionSpot, kRegionCount> kSpots{{
    {0.24, 0.28},
    {0.76, 0.28},
    {0.24, 0.62},
    {0.76, 0.62},
    {0.50, 0.20},
    {0.50, 0.70},
}};
// Small enough that six of them stay a bounded observer, large enough to carry
// a picture rather than a gradient.
constexpr int kRegionSide = 64;

struct RegionRead {
    QRect rect;
    QImage pixels;
    double luma = 0.0;
    double saturated = 0.0;
    quint64 hash = 0;
    bool valid = false;
};

/// The video rectangle inside a framebuffer, letterboxed the way mpv draws it.
QRect pictureRectIn(const QSize& framebuffer, int videoWidth, int videoHeight) {
    if (framebuffer.isEmpty() || videoWidth <= 0 || videoHeight <= 0) return {};
    const QSize fitted = QSize(videoWidth, videoHeight)
                             .scaled(framebuffer, Qt::KeepAspectRatio)
                             .expandedTo(QSize(1, 1));
    return QRect(QPoint((framebuffer.width() - fitted.width()) / 2,
                        (framebuffer.height() - fitted.height()) / 2),
                 fitted);
}

struct PictureRead {
    bool measured = false;
    qint64 micros = -1;
    QSize framebuffer;
    QRect picture;
    std::array<RegionRead, kRegionCount> regions{};

    double meanLuma() const {
        double sum = 0.0;
        int counted = 0;
        for (const RegionRead& region : regions) {
            if (!region.valid) continue;
            sum += region.luma;
            ++counted;
        }
        return counted > 0 ? sum / counted : 0.0;
    }

    int sampledRegions() const {
        int counted = 0;
        for (const RegionRead& region : regions) {
            if (region.valid) ++counted;
        }
        return counted;
    }
};

quint64 hashBytes(const QImage& image) {
    if (image.isNull()) return 0;
    quint64 hash = 1469598103934665603ULL;
    const int bytes = static_cast<int>(image.sizeInBytes());
    const auto* data = image.constBits();
    for (int i = 0; i < bytes; ++i) {
        hash ^= static_cast<quint64>(data[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

/// Reads the six regions out of the surface's own framebuffer. Paint-level, not
/// proof of scanned-out pixels; buffers are reused between polls.
///
/// The fallbacks are the last known device and source size, because a load
/// clears the surface's dimensions before the new source reports its own.
class PictureSampler {
public:
    PictureRead read(VideoSurface* surface, const QSize& deviceFallback, int videoWidthFallback,
                     int videoHeightFallback, bool keepPixels = true) {
        PictureRead out;
        if (surface == nullptr || surface->context() == nullptr) return out;
        QElapsedTimer clock;
        clock.start();
        QOpenGLContext* previous = QOpenGLContext::currentContext();
        QSurface* previousSurface = previous != nullptr ? previous->surface() : nullptr;
        surface->makeCurrent();
        QOpenGLContext* context = QOpenGLContext::currentContext();
        if (context == surface->context()) {
            QOpenGLFunctions* gl = context->functions();
            GLint framebuffer = 0;
            GLint alignment = 0;
            gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
            gl->glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
            gl->glBindFramebuffer(GL_FRAMEBUFFER, surface->defaultFramebufferObject());
            gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
            const QSize device = surface->size().isEmpty()
                                     ? deviceFallback
                                     : surface->size() * surface->devicePixelRatioF();
            const int videoWidth =
                surface->videoWidth() > 0 ? surface->videoWidth() : videoWidthFallback;
            const int videoHeight =
                surface->videoHeight() > 0 ? surface->videoHeight() : videoHeightFallback;
            const QRect picture = pictureRectIn(device, videoWidth, videoHeight);
            if (!picture.isEmpty() && device.width() > 0 && device.height() > 0) {
                out.measured = true;
                out.framebuffer = device;
                out.picture = picture;
                const int side = std::max(
                    8, std::min(kRegionSide, std::min(picture.width(), picture.height()) / 3));
                for (int index = 0; index < kRegionCount; ++index) {
                    const RegionSpot& spot = kSpots[static_cast<std::size_t>(index)];
                    const int centreX = picture.left() + qRound(spot.fx * picture.width());
                    const int centreY = picture.top() + qRound(spot.fy * picture.height());
                    QRect box(centreX - side / 2, centreY - side / 2, side, side);
                    box.moveLeft(
                        std::clamp(box.left(), picture.left(), picture.right() + 1 - box.width()));
                    box.moveTop(
                        std::clamp(box.top(), picture.top(), picture.bottom() + 1 - box.height()));
                    RegionRead& region = out.regions[static_cast<std::size_t>(index)];
                    region.rect = box;
                    // One buffer per region, resized only when the band changes:
                    // the gap poll asks every few milliseconds and must not
                    // allocate an image per sample.
                    QImage& capture = scratch_[static_cast<std::size_t>(index)];
                    if (capture.size() != box.size()) {
                        capture = QImage(box.size(), QImage::Format_RGBA8888);
                    }
                    // Qt measures from the top, GL from the bottom of the buffer.
                    gl->glReadPixels(box.left(), device.height() - (box.top() + box.height()),
                                     box.width(), box.height(), GL_RGBA, GL_UNSIGNED_BYTE,
                                     capture.bits());
                    region.hash = hashBytes(capture);
                    double lumaSum = 0.0;
                    int saturated = 0;
                    int counted = 0;
                    for (int y = 0; y < capture.height(); ++y) {
                        const uchar* line = capture.constScanLine(y);
                        for (int x = 0; x < capture.width(); ++x) {
                            const uchar r = line[x * 4 + 0];
                            const uchar g = line[x * 4 + 1];
                            const uchar b = line[x * 4 + 2];
                            lumaSum += (r * 299 + g * 587 + b * 114) / 1000.0;
                            const int high = std::max({r, g, b});
                            const int low = std::min({r, g, b});
                            if (high > 90 && high - low > 40) ++saturated;
                            ++counted;
                        }
                    }
                    region.luma = counted > 0 ? lumaSum / counted : 0.0;
                    region.saturated = counted > 0 ? double(saturated) / double(counted) : 0.0;
                    region.valid = true;
                    // Only the two stable-point comparisons need their own copy;
                    // the scratch buffer is reused by the next poll.
                    if (keepPixels) region.pixels = capture.copy();
                }
            }
            gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
            gl->glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        }
        if (previous != nullptr && previousSurface != nullptr) {
            previous->makeCurrent(previousSurface);
        } else {
            surface->doneCurrent();
        }
        out.micros = clock.nsecsElapsed() / 1000;
        return out;
    }

private:
    std::array<QImage, kRegionCount> scratch_;
};

/// One sampler per process, so the scratch buffers survive between polls.
PictureSampler g_pictureSampler;

PictureRead readPicture(VideoSurface* surface, const QSize& deviceFallback, int videoWidthFallback,
                        int videoHeightFallback, bool keepPixels = true) {
    return g_pictureSampler.read(surface, deviceFallback, videoWidthFallback, videoHeightFallback,
                                 keepPixels);
}

/// Pixels that visibly differ, not pixels that differ by the downscaler's noise
/// floor: two re-renders of one still frame are not bit-identical.
int changedPixels(const QImage& before, const QImage& after, int threshold = 8) {
    if (before.isNull() || after.isNull() || before.size() != after.size()) return -1;
    int changed = 0;
    for (int y = 0; y < before.height(); ++y) {
        const uchar* a = before.constScanLine(y);
        const uchar* b = after.constScanLine(y);
        for (int x = 0; x < before.width(); ++x) {
            const int delta = std::abs(int(a[x * 4]) - int(b[x * 4])) +
                              std::abs(int(a[x * 4 + 1]) - int(b[x * 4 + 1])) +
                              std::abs(int(a[x * 4 + 2]) - int(b[x * 4 + 2]));
            if (delta > threshold) ++changed;
        }
    }
    return changed;
}

/// Recorded at every framebuffer read, defined with the summaries below.
void noteObserverRead(qint64 micros);

// ── what "the picture is really there" means, measured not assumed ────────

struct Baseline {
    bool valid = false;
    QSize device;
    int videoWidth = 0;
    int videoHeight = 0;
    std::array<double, kRegionCount> luma{};
    std::array<bool, kRegionCount> meaningful{};

    double floorFor(int index) const {
        const double base = luma[static_cast<std::size_t>(index)];
        return std::max(10.0, base * 0.55);
    }

    bool present(const PictureRead& read) const {
        if (!read.measured) return false;
        int judged = 0;
        for (int index = 0; index < kRegionCount; ++index) {
            const auto spot = static_cast<std::size_t>(index);
            if (!meaningful[spot] || !read.regions[spot].valid) continue;
            ++judged;
            if (read.regions[spot].luma < floorFor(index)) return false;
        }
        return judged >= 3;
    }

    int litRegions(const PictureRead& read) const {
        int lit = 0;
        for (int index = 0; index < kRegionCount; ++index) {
            const auto spot = static_cast<std::size_t>(index);
            if (!meaningful[spot] || !read.regions[spot].valid) continue;
            if (read.regions[spot].luma >= floorFor(index)) ++lit;
        }
        return lit;
    }

    int judgedRegions() const {
        int judged = 0;
        for (bool expected : meaningful) {
            if (expected) ++judged;
        }
        return judged;
    }
};

/// Median of the six regions over a settled real picture. A region that is
/// indistinguishable from the black clear is not judged at all.
Baseline calibrate(VideoSurface* surface, int samples = 24, int intervalMs = 40) {
    Baseline baseline;
    std::array<std::vector<double>, kRegionCount> history;
    for (int taken = 0; taken < samples; ++taken) {
        const PictureRead read =
            readPicture(surface, baseline.device, baseline.videoWidth, baseline.videoHeight);
        noteObserverRead(read.micros);
        if (read.measured) {
            baseline.device = read.framebuffer;
            baseline.videoWidth = surface != nullptr ? surface->videoWidth() : 0;
            baseline.videoHeight = surface != nullptr ? surface->videoHeight() : 0;
            for (int index = 0; index < kRegionCount; ++index) {
                const auto spot = static_cast<std::size_t>(index);
                if (read.regions[spot].valid) history[spot].push_back(read.regions[spot].luma);
            }
        }
        pump(intervalMs);
    }
    int meaningful = 0;
    for (int index = 0; index < kRegionCount; ++index) {
        const auto spot = static_cast<std::size_t>(index);
        if (history[spot].empty()) continue;
        std::vector<double>& values = history[spot];
        std::sort(values.begin(), values.end());
        const double median = values[values.size() / 2];
        baseline.luma[spot] = median;
        baseline.meaningful[spot] = median > 20.0;
        if (baseline.meaningful[spot]) ++meaningful;
    }
    baseline.valid = meaningful >= 3 && baseline.videoWidth > 0 && baseline.videoHeight > 0;
    return baseline;
}

// ── paint cost, per classified pass ──────────────────────────────────────

const char* passName(int pass) {
    switch (pass) {
        case mikai::gui::uiprofile::kPaintUnrendered:
            return "unrendered";
        case mikai::gui::uiprofile::kPaintVideoFrame:
            return "video-frame";
        case mikai::gui::uiprofile::kPaintRetained:
            return "retained";
        case mikai::gui::uiprofile::kPaintChromeOnly:
            return "chrome-only";
        default:
            return "unknown";
    }
}

double percentileOf(std::vector<qint32> values, double fraction) {
    if (values.empty()) return -1.0;
    std::sort(values.begin(), values.end());
    const double raw = fraction * static_cast<double>(values.size());
    const auto at = static_cast<std::size_t>(std::ceil(raw));
    const std::size_t index = at == 0 ? 0 : std::min(at - 1, values.size() - 1);
    return static_cast<double>(values[index]);
}

struct PassSummary {
    quint64 samples = 0;   // every pass of this kind in the stage
    int ringHolds = 0;     // how many of them the ring still describes
    int ringCapacity = 0;
    double p50 = -1.0;
    double p95 = -1.0;
    double max = -1.0;
};

PassSummary summarize(int pass) {
    PassSummary summary;
    summary.samples = mikai::gui::uiprofile::paintPassSamples(pass);
    const std::vector<qint32> micros = mikai::gui::uiprofile::paintPassMicros(pass);
    summary.ringHolds = static_cast<int>(micros.size());
    summary.ringCapacity = mikai::gui::uiprofile::kPaintSamplesPerPass;
    if (!micros.empty()) {
        summary.p50 = percentileOf(micros, 0.50);
        summary.p95 = percentileOf(micros, 0.95);
        summary.max = static_cast<double>(*std::max_element(micros.begin(), micros.end()));
    }
    return summary;
}

// ── observer cost, recorded because it perturbs ──────────────────────────
// Every sample is six glReadPixels on the surface's framebuffer, which costs a
// GPU sync. The cost is reported rather than assumed negligible.
std::vector<qint64> g_observerMicros;

void noteObserverRead(qint64 micros) {
    if (micros < 0) return;
    g_observerMicros.push_back(micros);
}

QJsonObject observerObject() {
    QJsonObject out;
    out.insert(QStringLiteral("framebufferReads"), static_cast<int>(g_observerMicros.size()));
    qint64 total = 0;
    qint64 maxMicros = 0;
    std::vector<qint32> values;
    values.reserve(g_observerMicros.size());
    for (qint64 value : g_observerMicros) {
        total += value;
        maxMicros = std::max(maxMicros, value);
        values.push_back(static_cast<qint32>(std::min<qint64>(value, 2147483647LL)));
    }
    out.insert(QStringLiteral("readTotalUs"), static_cast<double>(total));
    out.insert(QStringLiteral("readMaxUs"), static_cast<double>(maxMicros));
    out.insert(QStringLiteral("readP50Us"), percentileOf(values, 0.50));
    out.insert(QStringLiteral("readP95Us"), percentileOf(values, 0.95));
    out.insert(QStringLiteral("note"),
               QStringLiteral("one read = six off-centre region glReadPixels; no paint forced"));
    return out;
}

QJsonObject passObject(const PassSummary& summary) {
    return QJsonObject{{"samples", static_cast<double>(summary.samples)},
                       {"ringHolds", summary.ringHolds},
                       {"ringCapacity", summary.ringCapacity},
                       {"p50_us", summary.p50},
                       {"p95_us", summary.p95},
                       {"max_us", summary.max}};
}

void reportPasses(const QString& scope) {
    for (int pass = 0; pass < mikai::gui::uiprofile::kPaintPassCount; ++pass) {
        const PassSummary summary = summarize(pass);
        if (summary.samples == 0 && summary.p95 < 0.0) continue;
        metric(QStringLiteral("%1 %2: samples=%3 ring=%4/%5 p50=%6us p95=%7us max=%8us")
                   .arg(scope, QString::fromLatin1(passName(pass)))
                   .arg(summary.samples)
                   .arg(summary.ringHolds)
                   .arg(summary.ringCapacity)
                   .arg(summary.p50, 0, 'f', 1)
                   .arg(summary.p95, 0, 'f', 1)
                   .arg(summary.max, 0, 'f', 1));
    }
}

// ── pointer gestures over real chrome ────────────────────────────────────

bool aimPointerAt(QWidget* target, const QPoint& local) {
    if (target == nullptr || !target->isVisible()) return false;
    const QPointF localPos(local);
    const QPointF globalPos(target->mapToGlobal(local));
    // An Enter event must be a QEnterEvent: QWidget::event() casts it, and a
    // QMouseEvent tagged Enter is the wrong dynamic type.
    QEnterEvent enter(localPos, localPos, globalPos);
    QApplication::sendEvent(target, &enter);
    QMouseEvent move(QEvent::MouseMove, localPos, globalPos, Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QApplication::sendEvent(target, &move);
    pump(4);
    return true;
}

/// One round of a viewer moving the pointer across chrome: two controls, so the
/// repaint is a hover state changing rather than a single control's animation.
int hoverRound(QWidget* first, QWidget* second, QWidget* scrub) {
    int moved = 0;
    if (aimPointerAt(first, QPoint(first != nullptr ? first->width() / 2 : 0, 8))) ++moved;
    pump(12);
    if (scrub != nullptr) {
        const QPoint local(static_cast<int>(scrub->width() * 0.35), scrub->height() / 2);
        if (aimPointerAt(scrub, local)) ++moved;
        pump(12);
        if (aimPointerAt(scrub, QPoint(static_cast<int>(scrub->width() * 0.62),
                                       scrub->height() / 2))) {
            ++moved;
        }
        pump(12);
    }
    if (aimPointerAt(second, QPoint(second != nullptr ? second->width() / 2 : 0, 8))) ++moved;
    pump(12);
    return moved;
}

/// Sweeps the pointer over chrome for a wall-clock window: a p95 of paint cost
/// has to describe a real stretch of interaction, not a handful of events.
int sweepChrome(QWidget* first, QWidget* second, QWidget* scrub, int durationMs) {
    QElapsedTimer clock;
    clock.start();
    int moves = 0;
    while (clock.elapsed() < durationMs) {
        moves += hoverRound(first, second, scrub);
    }
    return moves;
}

// ── retention across a real context recreation ───────────────────────────

/// Moves the surface onto another owned window and back, which is what makes Qt
/// destroy and recreate its context for real (the documented route - no signal
/// is mocked and no private method is called). Then it asks the question the
/// retention exists for: is the picture still on screen, and is the source still
/// paused where the viewer left it.
/// Answers 0 measured, 1 failed, 3 unobservable.
int runContextLoss(PlayerDialog* dialog, VideoSurface* surface, const Baseline& baseline) {
    if (dialog == nullptr || surface == nullptr) return 3;
    surface->setPaused(true);
    if (!waitFor([surface] { return !surface->isPlaying(); }, 4000)) return 3;
    pump(320);
    const PictureRead before =
        readPicture(surface, baseline.device, baseline.videoWidth, baseline.videoHeight);
    noteObserverRead(before.micros);
    const quint64 generationBefore = surface->renderContextGeneration();
    const double positionBefore = surface->position();
    const auto corePaused = [surface] {
        const auto pause = surface->probeMpvProperty(QStringLiteral("pause"),
                                                     ProbePropertyKind::Flag);
        return pause.has_value() && pause->kind == ProbePropertyValue::Kind::Flag && pause->flag;
    };
    const bool pausedBefore = corePaused();

    QWidget* shell = surface->parentWidget();
    QWidget host;
    host.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    host.setAttribute(Qt::WA_ShowWithoutActivating);
    host.resize(dialog->size());
    host.show();
    pump(160);
    surface->setParent(&host);
    surface->show();
    pump(500);
    const quint64 generationAway = surface->renderContextGeneration();
    if (shell != nullptr) {
        surface->setParent(shell);
        surface->show();
    }
    // A reparent is a layout change: ask the dialog for its size again so the
    // shell re-places the surface instead of leaving it at the host's rect.
    dialog->resize(dialog->size());
    pump(700);
    const quint64 generationBack = surface->renderContextGeneration();
    const PictureRead after =
        readPicture(surface, baseline.device, baseline.videoWidth, baseline.videoHeight);
    noteObserverRead(after.micros);
    const bool retained = surface->retainedPictureBytes() > 0;
    const bool pausedAfter = corePaused();
    const double positionAfter = surface->position();

    metric(QStringLiteral("context recreation: generation=%1->%2->%3 retained_bytes=%4 "
                          "paused=%5->%6 position=%7->%8 framebuffer_picture=%9->%10")
               .arg(static_cast<qulonglong>(generationBefore))
               .arg(static_cast<qulonglong>(generationAway))
               .arg(static_cast<qulonglong>(generationBack))
               .arg(surface->retainedPictureBytes())
               .arg(int(pausedBefore))
               .arg(int(pausedAfter))
               .arg(positionBefore, 0, 'f', 3)
               .arg(positionAfter, 0, 'f', 3)
               .arg(int(baseline.present(before)))
               .arg(int(baseline.present(after))));
    if (generationBack == generationBefore) {
        metric(QStringLiteral("context recreation: the context was never recreated, so the "
                              "retention claim is unobservable in this environment"));
        return 3;
    }
    int failures = 0;
    if (!pausedAfter) {
        metric(QStringLiteral("context recreation: the source is playing after the recreation, "
                              "but the viewer had paused it"));
        ++failures;
    }
    if (pausedBefore && positionAfter + 1.5 < positionBefore) {
        metric(QStringLiteral("context recreation: the position moved backwards across the "
                              "recreation"));
        ++failures;
    }
    if (!retained && !baseline.present(after)) {
        metric(QStringLiteral("context recreation: neither a retained picture nor a framebuffer "
                              "picture survived the recreation"));
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}

// ── one measured load transition ─────────────────────────────────────────

struct GapSample {
    qint64 ms = 0;
    quint64 epoch = 0;
    quint64 contextGeneration = 0;
    bool opening = false;
    bool hasVideo = false;
    bool hasVisibleOutput = false;
    bool retained = false;
    double position = -1.0;
    double duration = -1.0;
    int videoWidth = 0;
    int videoHeight = 0;
    qint64 observerMicros = -1;
    double framebufferLuma = 0.0;
    int framebufferLitRegions = 0;
    bool framebufferPicture = false;
};

/// Every instant below is on the run's one clock, and every origin counter
/// belongs to the origin that served THIS transition.
struct TransitionOutcome {
    bool measured = false;
    int originRequests = 0;
    int originResponses = 0;
    qint64 originFirstReplyMs = -1;
    qint64 requestMs = -1;
    qint64 firstOutputMs = -1;
    qint64 gapMs = -1;
    int samples = 0;
    int framebufferBlackSamples = 0;
    int framebufferOffCentreBlackSamples = 0;
    int retainedSamples = 0;
    quint64 epochBefore = 0;
    quint64 epochAfter = 0;
    bool framebufferPictureAfter = false;
    bool framebufferMoved = false;
    bool observedFramebufferBlack = false;
    QJsonArray rows;
};

/// Drives a real switch and samples from the request to the first real output.
///
/// The pointer keeps moving over chrome during the gap: a viewer's hand does not
/// stop for a load, and that repaint is the pass this goal is about.
TransitionOutcome measureTransition(PlayerDialog* dialog, VideoSurface* surface,
                                    const Baseline& baseline, const QElapsedTimer& runClock,
                                    const std::function<void()>& switchTo, int budgetMs,
                                    bool movePointerDuringGap) {
    TransitionOutcome outcome;
    if (dialog == nullptr || surface == nullptr) return outcome;

    QWidget* first = dialog->findChild<QWidget*>(QStringLiteral("playerPlayToggle"));
    QWidget* second = dialog->findChild<QWidget*>(QStringLiteral("playerSettingsButton"));
    QWidget* scrub = dialog->findChild<QWidget*>(QStringLiteral("playerScrubBar"));

    const Baseline& calibration = baseline;
    outcome.epochBefore = surface->sourceEpoch();

    outcome.requestMs = runClock.elapsed();
    switchTo();

    int lastPointerMoveMs = -1000;
    while (runClock.elapsed() - outcome.requestMs < budgetMs) {
        pump(8);
        if (movePointerDuringGap && runClock.elapsed() - lastPointerMoveMs >= 140) {
            lastPointerMoveMs = runClock.elapsed();
            hoverRound(first, second, scrub);
        }
        const PictureRead read =
            readPicture(surface, calibration.device, calibration.videoWidth,
                        calibration.videoHeight, /*keepPixels=*/false);
        noteObserverRead(read.micros);
        GapSample sample;
        sample.ms = runClock.elapsed();
        sample.epoch = surface->sourceEpoch();
        sample.contextGeneration = surface->renderContextGeneration();
        sample.opening = surface->isOpening();
        sample.hasVideo = surface->hasVideo();
        sample.hasVisibleOutput = surface->hasVisibleOutputFrame();
        sample.retained = surface->hasRetainedPicture();
        sample.position = surface->position();
        sample.duration = surface->duration();
        sample.videoWidth = surface->videoWidth();
        sample.videoHeight = surface->videoHeight();
        sample.observerMicros = read.micros;
        sample.framebufferLuma = read.meanLuma();
        sample.framebufferLitRegions = calibration.litRegions(read);
        sample.framebufferPicture = calibration.present(read);
        ++outcome.samples;
        if (!sample.framebufferPicture) {
            ++outcome.framebufferBlackSamples;
            if (sample.framebufferLitRegions == 0) ++outcome.framebufferOffCentreBlackSamples;
        }
        if (sample.retained) ++outcome.retainedSamples;
        if (sample.ms - outcome.requestMs <= 20000 && outcome.rows.size() < 1200) {
            outcome.rows.append(QJsonObject{
                {"runClockMs", static_cast<double>(sample.ms)},
                {"epoch", static_cast<double>(sample.epoch)},
                {"contextGeneration", static_cast<double>(sample.contextGeneration)},
                {"opening", sample.opening},
                {"hasVideo", sample.hasVideo},
                {"hasVisibleOutput", sample.hasVisibleOutput},
                {"retained", sample.retained},
                {"position", sample.position},
                {"duration", sample.duration},
                {"videoWidth", sample.videoWidth},
                {"videoHeight", sample.videoHeight},
                {"observerUs", static_cast<double>(sample.observerMicros)},
                {"framebufferLuma", sample.framebufferLuma},
                {"framebufferLitRegions", sample.framebufferLitRegions},
                {"framebufferPicture", sample.framebufferPicture}});
        }
        // "Real output of the new source": the epoch moved, the surface reports
        // a visible output frame of it, and the framebuffer band carries the
        // calibrated content. All three, so a held picture cannot answer for it.
        if (sample.epoch != outcome.epochBefore && sample.hasVisibleOutput &&
            sample.framebufferPicture) {
            outcome.firstOutputMs = sample.ms;
            outcome.epochAfter = sample.epoch;
            outcome.framebufferPictureAfter = true;
            break;
        }
    }
    outcome.measured = true;
    if (outcome.firstOutputMs >= 0) outcome.gapMs = outcome.firstOutputMs - outcome.requestMs;

    // A held picture is lit as well: motion in the band is what separates a live
    // new output from the snapshot the surface kept.
    if (outcome.framebufferPictureAfter) {
        PictureRead first = readPicture(surface, baseline.device, baseline.videoWidth,
                                        baseline.videoHeight);
        pump(220);
        PictureRead later = readPicture(surface, baseline.device, baseline.videoWidth,
                                        baseline.videoHeight);
        noteObserverRead(first.micros);
        noteObserverRead(later.micros);
        int changed = 0;
        int counted = 0;
        for (int index = 0; index < kRegionCount; ++index) {
            const auto spot = static_cast<std::size_t>(index);
            const int delta = changedPixels(first.regions[spot].pixels, later.regions[spot].pixels);
            if (delta < 0) continue;
            counted += first.regions[spot].pixels.width() * first.regions[spot].pixels.height();
            changed += delta;
        }
        outcome.framebufferMoved = counted > 0 && double(changed) > 0.002 * double(counted);
    }
    outcome.observedFramebufferBlack = outcome.framebufferBlackSamples > 0;
    return outcome;
}

QJsonObject outcomeObject(const TransitionOutcome& outcome) {
    return QJsonObject{
        {"clockDomain", QStringLiteral("run elapsed ms from probe start")},
        {"originRequests", outcome.originRequests},
        {"originResponses", outcome.originResponses},
        {"originFirstReplyMs", static_cast<double>(outcome.originFirstReplyMs)},
        {"requestMs", static_cast<double>(outcome.requestMs)},
        {"firstOutputMs", static_cast<double>(outcome.firstOutputMs)},
        {"gapMs", static_cast<double>(outcome.gapMs)},
        {"samples", outcome.samples},
        {"framebufferBlackSamples", outcome.framebufferBlackSamples},
        {"framebufferOffCentreBlackSamples", outcome.framebufferOffCentreBlackSamples},
        {"retainedSamples", outcome.retainedSamples},
        {"epochBefore", static_cast<double>(outcome.epochBefore)},
        {"epochAfter", static_cast<double>(outcome.epochAfter)},
        {"framebufferPictureAfter", outcome.framebufferPictureAfter},
        {"framebufferMoved", outcome.framebufferMoved},
        {"observedFramebufferBlack", outcome.observedFramebufferBlack}};
}

void printOutcome(const QString& label, const TransitionOutcome& outcome) {
    metric(QStringLiteral("%1: originReq=%2 resp=%3 reply=%4ms request=%5ms first_output=%6ms "
                          "gap=%7ms samples=%8 fb_black=%9 fb_off_centre_black=%10 retained=%11 "
                          "epoch=%12->%13 fb_moved=%14")
               .arg(label)
               .arg(outcome.originRequests)
               .arg(outcome.originResponses)
               .arg(outcome.originFirstReplyMs)
               .arg(outcome.requestMs)
               .arg(outcome.firstOutputMs)
               .arg(outcome.gapMs)
               .arg(outcome.samples)
               .arg(outcome.framebufferBlackSamples)
               .arg(outcome.framebufferOffCentreBlackSamples)
               .arg(outcome.retainedSamples)
               .arg(static_cast<qulonglong>(outcome.epochBefore))
               .arg(static_cast<qulonglong>(outcome.epochAfter))
               .arg(int(outcome.framebufferMoved)));
}

int run(int argc, char** argv) {
    QApplication app(argc, argv);
    QString videoArgument;
    QString outputDir = g_outputDir;
    int delayMs = 2500;
    int cycles = 1;
    // The shortest stretch a paint p95 can honestly describe: a handful of events
    // is a sample of one interaction, not a distribution.
    int hoverSeconds = 30;
    bool motion = true;
    bool contextLoss = false;
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QLatin1String("--output-dir") && index + 1 < argc) {
            outputDir = QString::fromLocal8Bit(argv[++index]);
        } else if (argument == QLatin1String("--delay-ms") && index + 1 < argc) {
            delayMs = QString::fromLocal8Bit(argv[++index]).toInt();
        } else if (argument == QLatin1String("--cycles") && index + 1 < argc) {
            cycles = std::max(1, QString::fromLocal8Bit(argv[++index]).toInt());
        } else if (argument == QLatin1String("--hover-seconds") && index + 1 < argc) {
            hoverSeconds = std::max(1, QString::fromLocal8Bit(argv[++index]).toInt());
        } else if (argument == QLatin1String("--no-motion")) {
            motion = false;
        } else if (argument == QLatin1String("--context-loss")) {
            contextLoss = true;
        } else if (argument.startsWith(QLatin1String("--"))) {
            std::fprintf(stderr, "FAIL unknown option %s\n", qPrintable(argument));
            return 3;
        } else if (videoArgument.isEmpty()) {
            videoArgument = argument;
        }
    }
    if (videoArgument.isEmpty() || !QFileInfo::exists(videoArgument)) {
        std::fprintf(stderr,
                     "Usage: LoadTransitionProbe <known-duration-video-file> --output-dir "
                     "<absolute dir> [--delay-ms N] [--cycles N] [--hover-seconds N] "
                     "[--no-motion] [--context-loss]\n");
        return 3;
    }
    // One clock for the whole run: every timestamp in the evidence is elapsed
    // milliseconds from this point, and a per-origin reply is converted onto it.
    QElapsedTimer runClock;
    runClock.start();
    // The origin keeps the whole file in memory and answers from it, so a long
    // high-bitrate fixture is read into RAM before a single byte is served.
    constexpr qint64 kMaxFixtureBytes = 96LL * 1024 * 1024;
    const qint64 fixtureBytes = QFileInfo(videoArgument).size();
    if (fixtureBytes > kMaxFixtureBytes) {
        std::fprintf(stderr,
                     "FAIL fixture is %lld bytes; the delayed origin holds it whole in memory. "
                     "Pass the small motion clip (hover-motion-48s.mp4): the transition measures "
                     "the load gap, not decode throughput.\n",
                     static_cast<long long>(fixtureBytes));
        return 3;
    }
    if (!outputDir.isEmpty()) {
        if (!QFileInfo(outputDir).isAbsolute() || !QDir().mkpath(outputDir)) {
            std::fprintf(stderr,
                         "FAIL --output-dir must be an absolute directory this run may write: %s\n",
                         qPrintable(outputDir));
            return 3;
        }
        g_outputDir = outputDir;
    }
    app.setQuitOnLastWindowClosed(false);

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiLoadTransitionProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("LoadTransitionProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    if (!mplayer::probes::selectRuntime()) return 2;

    // Bounded observation, not logging: the counters below never ask for a
    // paint, so the pacing they report is the pacing playback chose.
    mikai::gui::uiprofile::forceCounters(true);

    static StubHost host;
    const QString file = QFileInfo(videoArgument).absoluteFilePath();
    const QList<QualityEntry> qualities = {{480, file}, {720, file}, {1080, file}};

    QJsonObject evidence;
    evidence.insert(QStringLiteral("fixture"), file);
    evidence.insert(QStringLiteral("delay_ms"), delayMs);
    evidence.insert(
        QStringLiteral("observer"),
        QStringLiteral("no forced paint; paint-level readback of the surface framebuffer"));

    auto* dialog = new PlayerDialog(QStringLiteral("probe load transition"), file, qualities, 720,
                                    &host);
    QPointer<PlayerDialog> guard(dialog);
    dialog->startPlayer();
    dialog->showFullScreen();
    pump(120);
    auto* surface = dialog->findChild<VideoSurface*>();
    check(surface != nullptr, "the player window owns a video surface");
    if (surface == nullptr) {
        std::fprintf(stderr, "FAIL no surface: nothing to measure\n");
        return 1;
    }

    const bool opened = waitFor([surface] { return surface->hasVisibleOutputFrame(); }, 20000);
    check(opened, "the first source reaches a real output frame");
    Baseline baseline = calibrate(surface);
    double baselineLuma = 0.0;
    int baselineRegions = 0;
    for (int index = 0; index < kRegionCount; ++index) {
        const auto spot = static_cast<std::size_t>(index);
        if (!baseline.meaningful[spot]) continue;
        baselineLuma += baseline.luma[spot];
        ++baselineRegions;
    }
    if (baselineRegions > 0) baselineLuma /= double(baselineRegions);
    metric(QStringLiteral("baseline: device=%1x%2 source=%3x%4 meaningful_regions=%5 mean_luma=%6")
               .arg(baseline.device.width())
               .arg(baseline.device.height())
               .arg(baseline.videoWidth)
               .arg(baseline.videoHeight)
               .arg(baseline.judgedRegions())
               .arg(baselineLuma, 0, 'f', 1));
    check(baseline.valid, "the outgoing source has a calibrated, lit picture band");
    evidence.insert(QStringLiteral("baseline"),
                    QJsonObject{{"device", QStringLiteral("%1x%2")
                                             .arg(baseline.device.width())
                                             .arg(baseline.device.height())},
                                {"videoWidth", baseline.videoWidth},
                                {"videoHeight", baseline.videoHeight},
                                {"judgedRegions", baseline.judgedRegions()}});
    {
        const PictureRead settled =
            readPicture(surface, baseline.device, baseline.videoWidth, baseline.videoHeight);
        noteObserverRead(settled.micros);
        QJsonArray regionLuma;
        for (const RegionRead& region : settled.regions) regionLuma.append(region.luma);
        evidence.insert(QStringLiteral("outgoingRegionLuma"), regionLuma);
    }

    QWidget* playToggle = dialog->findChild<QWidget*>(QStringLiteral("playerPlayToggle"));
    QWidget* settingsButton = dialog->findChild<QWidget*>(QStringLiteral("playerSettingsButton"));
    QWidget* scrub = dialog->findChild<QWidget*>(QStringLiteral("playerScrubBar"));
    check(playToggle != nullptr && settingsButton != nullptr && scrub != nullptr,
          "the chrome the pointer sweeps over exists");

    // ── paused hover: does moving over chrome cost a picture, or change one ──
    if (motion) {
        surface->setPaused(true);
        const bool paused = waitFor([surface] { return !surface->isPlaying(); }, 4000);
        check(paused, "the picture can be paused for the chrome-only measurement");
        pump(320);
        const PictureRead pausedBefore =
            readPicture(surface, baseline.device, baseline.videoWidth, baseline.videoHeight);
        noteObserverRead(pausedBefore.micros);
        mikai::gui::uiprofile::resetPaintPassProfile();
        const int hoverMoves =
            sweepChrome(playToggle, settingsButton, scrub, hoverSeconds * 1000);
        const PassSummary chromeOnly = summarize(mikai::gui::uiprofile::kPaintChromeOnly);
        const PassSummary retained = summarize(mikai::gui::uiprofile::kPaintRetained);
        const PassSummary videoFrame = summarize(mikai::gui::uiprofile::kPaintVideoFrame);
        const PictureRead pausedAfter =
            readPicture(surface, baseline.device, baseline.videoWidth, baseline.videoHeight);
        noteObserverRead(pausedAfter.micros);
        // The verdict is the goal's own: not one video pixel changes. The
        // visible-delta count is diagnostic only - it is not a relaxed pass.
        int changedExact = 0;
        int changedVisible = 0;
        int compared = 0;
        for (int index = 0; index < kRegionCount; ++index) {
            const auto spot = static_cast<std::size_t>(index);
            const int exact = changedPixels(pausedBefore.regions[spot].pixels,
                                            pausedAfter.regions[spot].pixels, 0);
            const int visible = changedPixels(pausedBefore.regions[spot].pixels,
                                              pausedAfter.regions[spot].pixels, 24);
            if (exact < 0 || visible < 0) continue;
            compared += pausedBefore.regions[spot].pixels.width() *
                        pausedBefore.regions[spot].pixels.height();
            changedExact += exact;
            changedVisible += visible;
        }
        metric(QStringLiteral("paused hover: moves=%1 chrome_paints=%2 ring=%3/%4 p95=%5us "
                              "max=%6us changed_exact=%7 changed_visible=%8/%9")
                   .arg(hoverMoves)
                   .arg(chromeOnly.samples)
                   .arg(chromeOnly.ringHolds)
                   .arg(chromeOnly.ringCapacity)
                   .arg(chromeOnly.p95, 0, 'f', 1)
                   .arg(chromeOnly.max, 0, 'f', 1)
                   .arg(changedExact)
                   .arg(changedVisible)
                   .arg(compared));
        reportPasses(QStringLiteral("paused-hover"));
        check(hoverMoves > 0, "the paused measurement actually moved the pointer over chrome");
        check(chromeOnly.samples > 0,
              "a paused chrome repaint is classified as a chrome-only pass");
        check(changedExact == 0,
              QStringLiteral("a paused picture is pixel-identical across chrome hovers "
                             "(exact=%1 visible=%2 of %3)")
                  .arg(changedExact)
                  .arg(changedVisible)
                  .arg(compared));
        QJsonArray hashesBefore;
        QJsonArray hashesAfter;
        for (int index = 0; index < kRegionCount; ++index) {
            const auto spot = static_cast<std::size_t>(index);
            hashesBefore.append(QString::number(pausedBefore.regions[spot].hash, 16));
            hashesAfter.append(QString::number(pausedAfter.regions[spot].hash, 16));
        }
        evidence.insert(QStringLiteral("pausedHover"),
                        QJsonObject{{"moves", hoverMoves},
                                    {"changedPixelsExact", changedExact},
                                    {"changedPixelsVisible", changedVisible},
                                    {"comparedPixels", compared},
                                    {"regionHashesBefore", hashesBefore},
                                    {"regionHashesAfter", hashesAfter},
                                    {"chromeOnly", passObject(chromeOnly)},
                                    {"retained", passObject(retained)},
                                    {"videoFrame", passObject(videoFrame)}});
        if (!g_outputDir.isEmpty()) {
            dialog->grab().save(artifactPath(QStringLiteral("load-transition-paused.png")));
        }
        surface->setPaused(false);
        waitFor([surface] { return surface->isPlaying(); }, 4000);
        waitFor([surface] { return surface->hasVisibleOutputFrame(); }, 4000);
        pump(200);
    }

    // ── active hover: the same sweep against a moving picture ───────────────
    {
        mikai::gui::uiprofile::resetPaintPassProfile();
        const int hoverMoves =
            sweepChrome(playToggle, settingsButton, scrub, hoverSeconds * 1000);
        const PassSummary chromeOnly = summarize(mikai::gui::uiprofile::kPaintChromeOnly);
        const PassSummary videoFrame = summarize(mikai::gui::uiprofile::kPaintVideoFrame);
        metric(QStringLiteral("active hover: moves=%1 chrome_paints=%2 p95=%3us video_p95=%4us")
                   .arg(hoverMoves)
                   .arg(chromeOnly.samples)
                   .arg(chromeOnly.p95, 0, 'f', 1)
                   .arg(videoFrame.p95, 0, 'f', 1));
        reportPasses(QStringLiteral("active-hover"));
        evidence.insert(QStringLiteral("activeHover"),
                        QJsonObject{{"moves", hoverMoves},
                                    {"chromeOnly", passObject(chromeOnly)},
                                    {"videoFrame", passObject(videoFrame)}});
    }

    // ── format probe: a resize must not re-query the render target ──────────
    //
    // The attachment format belongs to the context and the surface format, not
    // to the size, so 200 resizes of the steady picture should cost no more than
    // the one probe a fresh target needs. A fullscreen window ignores resize(),
    // so the stage leaves fullscreen for the sweep and restores it after.
    {
        dialog->showNormal();
        pump(250);
        const QSize base = dialog->size();
        // These counters describe this stage only; the earlier stages keep
        // their own numbers.
        mikai::gui::uiprofile::resetCounters();
        for (int index = 0; index < 200; ++index) {
            const bool wider = index % 2 == 0;
            dialog->resize(wider ? std::max(320, base.width() - 96) : base.width() - 32,
                           wider ? std::max(200, base.height() - 72) : base.height() - 24);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        }
        pump(400);
        const auto counters = mikai::gui::uiprofile::snapshot();
        metric(QStringLiteral("format probe: resizes=200 base=%1x%2 probes=%3 "
                              "format_resets=%4")
                   .arg(base.width())
                   .arg(base.height())
                   .arg(counters.renderTargetProbes)
                   .arg(counters.renderTargetFormatResets));
        check(counters.renderTargetProbes <= 2,
              QStringLiteral("200 resizes re-probe the render target at most twice "
                             "(probes=%1)")
                  .arg(counters.renderTargetProbes));
        evidence.insert(QStringLiteral("formatProbe"),
                        QJsonObject{{"resizes", 200},
                                    {"baseWidth", base.width()},
                                    {"baseHeight", base.height()},
                                    {"renderTargetProbes", static_cast<qint64>(
                                                               counters.renderTargetProbes)},
                                    {"renderTargetFormatResets",
                                     static_cast<qint64>(counters.renderTargetFormatResets)}});
        dialog->showFullScreen();
        pump(300);
        waitFor([surface] { return surface->hasVisibleOutputFrame(); }, 5000);
        pump(200);
    }

    // ── retention across a real context recreation ──────────────────────────
    if (contextLoss) {
        const int result = runContextLoss(dialog, surface, baseline);
        // 0 measured, 1 failed, 3 unobservable. Unobservable is not a pass: the
        // environment could not recreate the context, so nothing supports the
        // retention claim.
        check(result == 0,
              QStringLiteral("a paused picture and its pause intent survive a real context "
                             "recreation (result=%1)")
                  .arg(result));
    }

    // ── the load transition itself ──────────────────────────────────────────
    const QString target = QFileInfo(file).absoluteFilePath();

    QJsonArray transitions;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        // A fresh origin per cycle: its reply stamp is then this transition's,
        // not a borrowing of the first one the process ever served.
        DelayedHttpSource origin;
        origin.setDelayMs(delayMs);
        const bool served = origin.serve(target);
        check(served, QStringLiteral("transition %1: the loopback origin serves the fixture")
                          .arg(cycle + 1));
        const QString slowUrl =
            origin.url(QStringLiteral("clip-b-%1.mp4").arg(cycle + 1));
        check(!slowUrl.isEmpty(),
              QStringLiteral("transition %1: the loopback origin listens before the switch")
                  .arg(cycle + 1));
        const qint64 originStartMs = runClock.elapsed();
        const TransitionOutcome outcome = measureTransition(
            dialog, surface, baseline, runClock,
            [dialog, slowUrl] {
                dialog->applyVariant(QList<QualityEntry>{{720, slowUrl}},
                                     QStringLiteral("probe B"), QStringLiteral("e1"));
            },
            30000, /*movePointerDuringGap=*/true);
        TransitionOutcome reported = outcome;
        reported.originRequests = origin.connections();
        reported.originResponses = origin.replyAtMs() >= 0 ? 1 : 0;
        reported.originFirstReplyMs =
            origin.replyAtMs() >= 0 ? originStartMs + origin.replyAtMs() : -1;
        const QString label = QStringLiteral("transition %1").arg(cycle + 1);
        printOutcome(label, reported);
        check(reported.originRequests > 0 && reported.originResponses > 0,
              QStringLiteral("%1: this cycle's origin received and answered the request "
                             "(requests=%2 responses=%3)")
                  .arg(label)
                  .arg(reported.originRequests)
                  .arg(reported.originResponses));
        check(reported.firstOutputMs >= 0,
              QStringLiteral("%1: the new source produced a real output").arg(label));
        check(reported.framebufferPictureAfter,
              QStringLiteral("%1: the new output's framebuffer band carries the calibrated "
                             "content")
                  .arg(label));
        check(reported.epochAfter != reported.epochBefore,
              QStringLiteral("%1: the source epoch advanced").arg(label));
        check(reported.samples >= 3,
              QStringLiteral("%1: sampling covered the request-to-output window").arg(label));
        if (reported.samples >= 3) {
            check(reported.framebufferBlackSamples == 0,
                  QStringLiteral("%1: the framebuffer is never black between the last outgoing "
                                 "frame and the first new output (black=%2)")
                      .arg(label)
                      .arg(reported.framebufferBlackSamples));
            check(reported.framebufferOffCentreBlackSamples == 0,
                  QStringLiteral("%1: the off-centre framebuffer band never went fully dark "
                                 "(off_centre_black=%2)")
                      .arg(label)
                      .arg(reported.framebufferOffCentreBlackSamples));
        }
        QJsonObject record = outcomeObject(reported);
        record.insert(QStringLiteral("cycle"), cycle + 1);
        record.insert(QStringLiteral("samplesRows"), reported.rows);
        transitions.append(record);
        // Back to the fast local source so the next cycle starts from a real
        // playing picture rather than from the previous cycle's tail.
        if (cycle + 1 < cycles) {
            dialog->applyVariant(QList<QualityEntry>{{720, file}},
                                 QStringLiteral("probe A"), QStringLiteral("e1"));
            waitFor([surface] { return surface->hasVisibleOutputFrame(); }, 15000);
            pump(400);
        }
    }
    evidence.insert(QStringLiteral("transitions"), transitions);

    // ── stream-expiry reload: the path a dead link takes ─────────────────────
    {
        DelayedHttpSource expiryOrigin;
        expiryOrigin.setDelayMs(delayMs);
        expiryOrigin.serve(target);
        const qint64 originStartMs = runClock.elapsed();
        const QString expiryUrl = expiryOrigin.url(QStringLiteral("clip-refresh.mp4"));
        const TransitionOutcome outcome = measureTransition(
            dialog, surface, baseline, runClock,
            [dialog, expiryUrl] {
                dialog->applyRefreshedStreams(QList<QualityEntry>{{720, expiryUrl}}, 720);
            },
            30000, /*movePointerDuringGap=*/true);
        TransitionOutcome reported = outcome;
        reported.originRequests = expiryOrigin.connections();
        reported.originResponses = expiryOrigin.replyAtMs() >= 0 ? 1 : 0;
        reported.originFirstReplyMs =
            expiryOrigin.replyAtMs() >= 0 ? originStartMs + expiryOrigin.replyAtMs() : -1;
        printOutcome(QStringLiteral("stream expiry"), reported);
        check(reported.originRequests > 0 && reported.originResponses > 0,
              QStringLiteral("stream expiry: this origin received and answered the request "
                             "(requests=%1)")
                  .arg(reported.originRequests));
        check(reported.framebufferPictureAfter,
              "stream expiry: the refreshed source's framebuffer band carries the calibrated "
              "content");
        check(reported.framebufferBlackSamples == 0,
              QStringLiteral("stream expiry: the framebuffer keeps a picture instead of black "
                             "(black=%1)")
                  .arg(reported.framebufferBlackSamples));
        evidence.insert(QStringLiteral("streamExpiry"), outcomeObject(reported));
    }

    const QJsonObject observer = observerObject();
    metric(QStringLiteral("observer: reads=%1 total=%2us p50=%3us p95=%4us max=%5us")
               .arg(observer.value(QStringLiteral("framebufferReads")).toInt())
               .arg(observer.value(QStringLiteral("readTotalUs")).toDouble(), 0, 'f', 0)
               .arg(observer.value(QStringLiteral("readP50Us")).toDouble(), 0, 'f', 1)
               .arg(observer.value(QStringLiteral("readP95Us")).toDouble(), 0, 'f', 1)
               .arg(observer.value(QStringLiteral("readMaxUs")).toDouble(), 0, 'f', 1));
    evidence.insert(QStringLiteral("observerCost"), observer);
    if (!g_outputDir.isEmpty()) {
        QFile out(artifactPath(QStringLiteral("load-transition-evidence.json")));
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented));
        }
    }

    if (!guard.isNull()) {
        guard->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    mikai::gui::uiprofile::forceCounters(false);
    std::fprintf(stderr, "RESULT: %s (%d checks, %d failed)\n",
                 g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) { return run(argc, argv); }
