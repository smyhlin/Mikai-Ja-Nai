// GOAL-E / T-E1, T-E2, T-E7: what a viewer actually sees between two real
// sources, and what a chrome-only pass costs.
//
// This probe is observation only. It forces no paint for its verdicts: the
// content samples read the surface's own framebuffer through its context, the
// same way FullscreenStabilityProbe does, so a "black composition" is the
// pixels Qt would composite and not a repaint the observer invented. The one
// forced readback is the saved screenshot, labelled in the evidence.
//
// Two real measurements live here, both required before any production change:
//
//  1. A load transition A -> B where B is served by the deliberately slow
//     loopback origin. Sampling starts at the request and stops at the first
//     real output of the new source, at a cadence of a few milliseconds, in six
//     off-centre regions of the picture band. The spinner/opening placeholder is
//     bright at the centre, so no centre-only check can pass this.
//  2. The paintGL duration distribution for a paused picture and for a playing
//     one while the pointer moves over chrome. GOAL-00 recorded visible
//     composition and first-swap latency but never paintGL itself, so a later
//     "30 % lower p95 chrome repaint" claim had nothing to be 30 % of.
//
// Usage:
//   LoadTransitionProbe <video-file> --output-dir <absolute dir>
//                        [--delay-ms N] [--cycles N] [--no-motion]

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

// Six regions inside the picture, all off the centre third that the opening
// placeholder's ring and arc occupy. The dock sits at the bottom, the window
// controls at the top right and the flank chevrons at mid-height on the edges,
// so these stay clear of chrome that legitimately changes.
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

/// Reads the six regions out of the surface's presented framebuffer.
///
/// `geometry` is the caller's last known device size and source size: a load
/// resets the surface's own dimensions before the new source reports any, and
/// the band that is being judged is still the one the outgoing picture filled.
PictureRead readPicture(VideoSurface* surface, const QSize& deviceFallback, int videoWidthFallback,
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
        const int videoWidth = surface->videoWidth() > 0 ? surface->videoWidth() : videoWidthFallback;
        const int videoHeight =
            surface->videoHeight() > 0 ? surface->videoHeight() : videoHeightFallback;
        const QRect picture = pictureRectIn(device, videoWidth, videoHeight);
        if (!picture.isEmpty() && device.width() > 0 && device.height() > 0) {
            out.measured = true;
            out.framebuffer = device;
            out.picture = picture;
            const int side =
                std::max(8, std::min(kRegionSide, std::min(picture.width(), picture.height()) / 3));
            for (int index = 0; index < kRegionCount; ++index) {
                const RegionSpot& spot = kSpots[static_cast<std::size_t>(index)];
                const int centreX = picture.left() + qRound(spot.fx * picture.width());
                const int centreY = picture.top() + qRound(spot.fy * picture.height());
                QRect box(centreX - side / 2, centreY - side / 2, side, side);
                box.moveLeft(std::clamp(box.left(), picture.left(),
                                        picture.right() + 1 - box.width()));
                box.moveTop(std::clamp(box.top(), picture.top(),
                                       picture.bottom() + 1 - box.height()));
                RegionRead& region = out.regions[static_cast<std::size_t>(index)];
                region.rect = box;
                QImage capture(box.size(), QImage::Format_RGBA8888);
                // Qt measures from the top, GL from the bottom of the buffer.
                gl->glReadPixels(box.left(), device.height() - (box.top() + box.height()),
                                 box.width(), box.height(), GL_RGBA, GL_UNSIGNED_BYTE,
                                 capture.bits());
                // The gap runs at a few milliseconds a sample; only the
                // comparisons that need pixels ask for them.
                if (keepPixels) region.pixels = capture;
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

/// Median of the six regions over a settled stretch of a real picture. A region
/// whose median is indistinguishable from the black clear is not judged at all,
/// because no honest verdict can be read from it.
Baseline calibrate(VideoSurface* surface, int samples = 24, int intervalMs = 40) {
    Baseline baseline;
    std::array<std::vector<double>, kRegionCount> history;
    for (int taken = 0; taken < samples; ++taken) {
        const PictureRead read =
            readPicture(surface, baseline.device, baseline.videoWidth, baseline.videoHeight);
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
    quint64 samples = 0;
    double p50 = -1.0;
    double p95 = -1.0;
    double max = -1.0;
};

PassSummary summarize(int pass) {
    PassSummary summary;
    summary.samples = mikai::gui::uiprofile::paintPassSamples(pass);
    const std::vector<qint32> micros = mikai::gui::uiprofile::paintPassMicros(pass);
    if (!micros.empty()) {
        summary.p50 = percentileOf(micros, 0.50);
        summary.p95 = percentileOf(micros, 0.95);
        summary.max = static_cast<double>(*std::max_element(micros.begin(), micros.end()));
    }
    return summary;
}

QJsonObject passObject(const PassSummary& summary) {
    return QJsonObject{{"samples", static_cast<double>(summary.samples)},
                       {"p50_us", summary.p50},
                       {"p95_us", summary.p95},
                       {"max_us", summary.max}};
}

void reportPasses(const QString& scope) {
    for (int pass = 0; pass < mikai::gui::uiprofile::kPaintPassCount; ++pass) {
        const PassSummary summary = summarize(pass);
        if (summary.samples == 0 && summary.p95 < 0.0) continue;
        metric(QStringLiteral("%1 %2: samples=%3 p50=%4us p95=%5us max=%6us")
                   .arg(scope, QString::fromLatin1(passName(pass)))
                   .arg(summary.samples)
                   .arg(summary.p50, 0, 'f', 1)
                   .arg(summary.p95, 0, 'f', 1)
                   .arg(summary.max, 0, 'f', 1));
    }
}

// ── pointer gestures over real chrome ────────────────────────────────────

bool aimPointerAt(QWidget* target, const QPoint& local) {
    if (target == nullptr || !target->isVisible()) return false;
    const QPoint global = target->mapToGlobal(local);
    QMouseEvent enter(QEvent::Enter, QPointF(local), QPointF(global), Qt::NoButton, Qt::NoButton,
                      Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    QMouseEvent move(QEvent::MouseMove, QPointF(local), QPointF(global), Qt::NoButton, Qt::NoButton,
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
    qint64 readMicros = -1;
    double meanLuma = 0.0;
    int litRegions = 0;
    bool picture = false;
};

struct TransitionOutcome {
    bool measured = false;
    bool originAnswered = false;
    qint64 originReplyMs = -1;
    qint64 requestMs = -1;
    qint64 firstOutputMs = -1;
    qint64 gapMs = -1;
    int samples = 0;
    int blackSamples = 0;
    int retainedSamples = 0;
    int offCentreBlackSamples = 0;
    quint64 epochBefore = 0;
    quint64 epochAfter = 0;
    bool pictureAfter = false;
    bool pictureMoved = false;
    bool observedBlack = false;
    QJsonArray rows;
};

/// Drives a real switch and samples continuously from the request to the first
/// real output of the new source.
///
/// `switchTo` is the production gesture (a quality/variant swap or a refreshed
/// stream). The pointer keeps moving over chrome while it runs, because a
/// viewer's hand does not stop for a load and a chrome repaint during the gap is
/// exactly the pass this goal is about.
TransitionOutcome measureTransition(PlayerDialog* dialog, VideoSurface* surface,
                                    const Baseline& baseline, const std::function<void()>& switchTo,
                                    int budgetMs, bool movePointerDuringGap) {
    TransitionOutcome outcome;
    if (dialog == nullptr || surface == nullptr) return outcome;

    QWidget* first = dialog->findChild<QWidget*>(QStringLiteral("playerPlayToggle"));
    QWidget* second = dialog->findChild<QWidget*>(QStringLiteral("playerSettingsButton"));
    QWidget* scrub = dialog->findChild<QWidget*>(QStringLiteral("playerScrubBar"));

    QElapsedTimer clock;
    clock.start();
    const Baseline& calibration = baseline;
    outcome.epochBefore = surface->sourceEpoch();

    outcome.requestMs = clock.elapsed();
    switchTo();

    int lastPointerMoveMs = -1000;
    while (clock.elapsed() < budgetMs) {
        pump(8);
        if (movePointerDuringGap && clock.elapsed() - lastPointerMoveMs >= 140) {
            lastPointerMoveMs = clock.elapsed();
            hoverRound(first, second, scrub);
        }
        const PictureRead read =
            readPicture(surface, calibration.device, calibration.videoWidth,
                        calibration.videoHeight, /*keepPixels=*/false);
        GapSample sample;
        sample.ms = clock.elapsed();
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
        sample.readMicros = read.micros;
        sample.meanLuma = read.meanLuma();
        sample.litRegions = calibration.litRegions(read);
        sample.picture = calibration.present(read);
        ++outcome.samples;
        if (!sample.picture) {
            ++outcome.blackSamples;
            if (sample.litRegions == 0) ++outcome.offCentreBlackSamples;
        }
        if (sample.retained) ++outcome.retainedSamples;
        if (sample.ms <= 20000 && outcome.rows.size() < 1200) {
            outcome.rows.append(QJsonObject{
                {"ms", static_cast<double>(sample.ms)},
                {"epoch", static_cast<double>(sample.epoch)},
                {"context", static_cast<double>(sample.contextGeneration)},
                {"opening", sample.opening},
                {"hasVideo", sample.hasVideo},
                {"visible", sample.hasVisibleOutput},
                {"retained", sample.retained},
                {"position", sample.position},
                {"duration", sample.duration},
                {"vw", sample.videoWidth},
                {"vh", sample.videoHeight},
                {"read_us", static_cast<double>(sample.readMicros)},
                {"luma", sample.meanLuma},
                {"lit", sample.litRegions},
                {"picture", sample.picture}});
        }
        // The new source has really produced output: the epoch moved, the band
        // carries the calibrated picture, and the surface says a frame of this
        // source reached it.
        if (sample.epoch != outcome.epochBefore && sample.hasVisibleOutput && sample.picture) {
            outcome.firstOutputMs = sample.ms;
            outcome.epochAfter = sample.epoch;
            outcome.pictureAfter = true;
            break;
        }
    }
    outcome.measured = true;
    if (outcome.firstOutputMs >= 0) outcome.gapMs = outcome.firstOutputMs - outcome.requestMs;

    // A frozen outgoing picture is lit as well: motion is what tells a live new
    // output from the snapshot the surface was holding.
    if (outcome.pictureAfter) {
        PictureRead first = readPicture(surface, baseline.device, baseline.videoWidth,
                                        baseline.videoHeight);
        pump(220);
        PictureRead later = readPicture(surface, baseline.device, baseline.videoWidth,
                                        baseline.videoHeight);
        int changed = 0;
        int counted = 0;
        for (int index = 0; index < kRegionCount; ++index) {
            const auto spot = static_cast<std::size_t>(index);
            const int delta = changedPixels(first.regions[spot].pixels, later.regions[spot].pixels);
            if (delta < 0) continue;
            counted += first.regions[spot].pixels.width() * first.regions[spot].pixels.height();
            changed += delta;
        }
        outcome.pictureMoved = counted > 0 && double(changed) > 0.002 * double(counted);
    }
    outcome.observedBlack = outcome.blackSamples > 0;
    return outcome;
}

QJsonObject outcomeObject(const TransitionOutcome& outcome) {
    return QJsonObject{
        {"originAnswered", outcome.originAnswered},
        {"originReplyMs", static_cast<double>(outcome.originReplyMs)},
        {"requestMs", static_cast<double>(outcome.requestMs)},
        {"firstOutputMs", static_cast<double>(outcome.firstOutputMs)},
        {"gapMs", static_cast<double>(outcome.gapMs)},
        {"samples", outcome.samples},
        {"blackSamples", outcome.blackSamples},
        {"offCentreBlackSamples", outcome.offCentreBlackSamples},
        {"retainedSamples", outcome.retainedSamples},
        {"epochBefore", static_cast<double>(outcome.epochBefore)},
        {"epochAfter", static_cast<double>(outcome.epochAfter)},
        {"pictureAfter", outcome.pictureAfter},
        {"pictureMoved", outcome.pictureMoved},
        {"observedBlack", outcome.observedBlack}};
}

void printOutcome(const QString& label, const TransitionOutcome& outcome) {
    metric(QStringLiteral("%1: request=%2ms first_output=%3ms gap=%4ms samples=%5 "
                          "black=%6 off_centre_black=%7 retained=%8 epoch=%9->%10 moved=%11")
               .arg(label)
               .arg(outcome.requestMs)
               .arg(outcome.firstOutputMs)
               .arg(outcome.gapMs)
               .arg(outcome.samples)
               .arg(outcome.blackSamples)
               .arg(outcome.offCentreBlackSamples)
               .arg(outcome.retainedSamples)
               .arg(static_cast<qulonglong>(outcome.epochBefore))
               .arg(static_cast<qulonglong>(outcome.epochAfter))
               .arg(int(outcome.pictureMoved)));
}

int run(int argc, char** argv) {
    QApplication app(argc, argv);
    QString videoArgument;
    QString outputDir = g_outputDir;
    int delayMs = 2500;
    int cycles = 1;
    bool motion = true;
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QLatin1String("--output-dir") && index + 1 < argc) {
            outputDir = QString::fromLocal8Bit(argv[++index]);
        } else if (argument == QLatin1String("--delay-ms") && index + 1 < argc) {
            delayMs = QString::fromLocal8Bit(argv[++index]).toInt();
        } else if (argument == QLatin1String("--cycles") && index + 1 < argc) {
            cycles = std::max(1, QString::fromLocal8Bit(argv[++index]).toInt());
        } else if (argument == QLatin1String("--no-motion")) {
            motion = false;
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
                     "<absolute dir> [--delay-ms N] [--cycles N] [--no-motion]\n");
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
    evidence.insert(QStringLiteral("observer"),
                    QStringLiteral("no forced paint; region reads through the surface context"));

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
        mikai::gui::uiprofile::resetPaintPassProfile();
        int hoverMoves = 0;
        for (int round = 0; round < 10; ++round) {
            hoverMoves += hoverRound(playToggle, settingsButton, scrub);
        }
        const PassSummary chromeOnly = summarize(mikai::gui::uiprofile::kPaintChromeOnly);
        const PassSummary retained = summarize(mikai::gui::uiprofile::kPaintRetained);
        const PassSummary videoFrame = summarize(mikai::gui::uiprofile::kPaintVideoFrame);
        const PictureRead pausedAfter =
            readPicture(surface, baseline.device, baseline.videoWidth, baseline.videoHeight);
        // Re-rendering one still frame is not bit-identical (mpv re-scales it on
        // every exposure, measured at ~5/255 here), so "not one video pixel
        // changed" has to mean "no visible change": both are reported, and the
        // verdict is on the one a viewer could see.
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
        metric(QStringLiteral("paused hover: moves=%1 chrome_paints=%2 p95=%3us max=%4us "
                              "changed_exact=%5 changed_visible=%6/%7")
                   .arg(hoverMoves)
                   .arg(chromeOnly.samples)
                   .arg(chromeOnly.p95, 0, 'f', 1)
                   .arg(chromeOnly.max, 0, 'f', 1)
                   .arg(changedExact)
                   .arg(changedVisible)
                   .arg(compared));
        reportPasses(QStringLiteral("paused-hover"));
        check(hoverMoves > 0, "the paused measurement actually moved the pointer over chrome");
        check(chromeOnly.samples > 0,
              "a paused chrome repaint is classified as a chrome-only pass");
        check(changedVisible == 0,
              QStringLiteral("a paused picture shows no visible change across chrome hovers "
                             "(visible=%1 exact=%2 of %3)")
                  .arg(changedVisible)
                  .arg(changedExact)
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
        int hoverMoves = 0;
        for (int round = 0; round < 10; ++round) {
            hoverMoves += hoverRound(playToggle, settingsButton, scrub);
        }
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

    // ── the load transition itself ──────────────────────────────────────────
    const QString target = QFileInfo(file).absoluteFilePath();
    DelayedHttpSource origin;
    origin.setDelayMs(delayMs);
    const bool served = origin.serve(target);
    check(served, "the loopback origin serves the same motion fixture");
    const QString slowUrl = origin.url(QStringLiteral("clip-b.mp4"));
    check(!slowUrl.isEmpty(), "the loopback origin listens before the switch");

    QJsonArray transitions;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        const TransitionOutcome outcome = measureTransition(
            dialog, surface, baseline,
            [dialog, slowUrl] {
                dialog->applyVariant(QList<QualityEntry>{{720, slowUrl}},
                                     QStringLiteral("probe B"), QStringLiteral("e1"));
            },
            30000, /*movePointerDuringGap=*/true);
        TransitionOutcome reported = outcome;
        reported.originAnswered = origin.replyAtMs() >= 0;
        reported.originReplyMs = origin.replyAtMs();
        const QString label = QStringLiteral("transition %1").arg(cycle + 1);
        printOutcome(label, reported);
        check(reported.originAnswered,
              QStringLiteral("%1: the delayed origin answered the request").arg(label));
        check(reported.firstOutputMs >= 0,
              QStringLiteral("%1: the new source produced a real output").arg(label));
        check(reported.pictureAfter,
              QStringLiteral("%1: the new output carries the calibrated picture").arg(label));
        check(reported.epochAfter != reported.epochBefore,
              QStringLiteral("%1: the source epoch advanced").arg(label));
        check(reported.samples >= 3,
              QStringLiteral("%1: sampling covered the request-to-output window").arg(label));
        if (reported.samples >= 3) {
            check(reported.blackSamples == 0,
                  QStringLiteral("%1: no black composition between the last outgoing frame and "
                                 "the first new output (black=%2)")
                      .arg(label)
                      .arg(reported.blackSamples));
            check(reported.offCentreBlackSamples == 0,
                  QStringLiteral("%1: the off-centre band never went fully dark "
                                 "(off_centre_black=%2)")
                      .arg(label)
                      .arg(reported.offCentreBlackSamples));
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
        const QString expiryUrl = expiryOrigin.url(QStringLiteral("clip-refresh.mp4"));
        const TransitionOutcome outcome = measureTransition(
            dialog, surface, baseline,
            [dialog, expiryUrl] {
                dialog->applyRefreshedStreams(QList<QualityEntry>{{720, expiryUrl}}, 720);
            },
            30000, /*movePointerDuringGap=*/true);
        TransitionOutcome reported = outcome;
        reported.originAnswered = expiryOrigin.replyAtMs() >= 0;
        reported.originReplyMs = expiryOrigin.replyAtMs();
        printOutcome(QStringLiteral("stream expiry"), reported);
        check(reported.originAnswered, "stream expiry: the delayed origin answered the request");
        check(reported.pictureAfter,
              "stream expiry: the refreshed source produced a calibrated picture");
        check(reported.blackSamples == 0,
              QStringLiteral("stream expiry: the frozen picture is shown instead of black "
                             "(black=%1)")
                  .arg(reported.blackSamples));
        evidence.insert(QStringLiteral("streamExpiry"), outcomeObject(reported));
    }

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
