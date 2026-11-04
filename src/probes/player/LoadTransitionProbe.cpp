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
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QScopeGuard>
#include <QSettings>
#include <QStringList>
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
/// Names the per-transition band artifacts without reusing a cycle's label.
int g_transitionArtifacts = 0;

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
//
// "Clear of the chrome" is a geometry fact, not a hope. PlayerShell paints its
// adopted controls INTO this surface's own painter (paintSurfaceOverlay ->
// chrome_.paint), so the dock, the info chip and the timeline's hover-preview
// card are framebuffer pixels this band reads. The card is the one that moves:
// previewCardRect() puts it kCardGap above the dock with the pointer's x as its
// centre, so it can stand over the lower third of the picture, and
// onTimelineHoverLeft() only starts a 120 ms grace timer - a pointer swept back
// onto the timeline inside that window keeps the card up for the whole sweep.
// The spots therefore stay above the dock band (55% of the surface height at
// most, plus the gap, measured from the dock's top) and off the exact centre
// column where the paused play glyph sits; auditRegions() re-checks that on
// every read instead of trusting this comment.
constexpr int kRegionCount = 6;
struct RegionSpot {
    double fx;
    double fy;
};
constexpr std::array<RegionSpot, kRegionCount> kSpots{{
    {0.22, 0.26},
    {0.50, 0.16},
    {0.78, 0.26},
    {0.22, 0.44},
    {0.50, 0.34},
    {0.78, 0.44},
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

/// Names the chrome standing over a judged region at the instant of a read.
///
/// PlayerShell draws its adopted controls with the surface's own painter, so a
/// region under the dock or under the timeline's hover card reads chrome, not
/// video - and a verdict built from that crop is about the wrong pixels. The
/// audit turns that from a silent mis-measurement into a named failure, and its
/// hits are part of the evidence.
class ChromeAudit {
public:
    void bind(PlayerDialog* dialog, VideoSurface* surface) {
        surface_ = surface;
        host_ = dialog;
        widgets_.clear();
        if (dialog == nullptr) return;
        // The framed controls only: a popover or a widget that paints itself
        // belongs to another device and cannot stand in this framebuffer.
        static const std::array<const char*, 13> kNames{{
            "playerDock", "playerHoverPreview", "playerTitleBar", "playerInfoChip",
            "playerEpisodeLabel", "playerPauseHoldChip", "playerNativeStatus",
            "playerAnimeJanaiStatus", "playerHdrBadge", "playerDolbyVisionBadge",
            "playerDolbyAtmosBadge", "playerCodecBadge", "playerNetworkStats",
        }};
        for (const char* name : kNames) {
            const QString objectName = QString::fromLatin1(name);
            for (QWidget* widget : dialog->findChildren<QWidget*>(objectName)) {
                if (widget != nullptr) widgets_.push_back(widget);
            }
        }
    }

    /// Adopts the region rects of one read. The picture geometry does not move
    /// inside a stage, so the audit can then run on widget geometry alone - no
    /// extra framebuffer read per pointer round.
    void observe(const PictureRead& read) {
        rects_.clear();
        for (const RegionRead& region : read.regions) {
            if (region.valid && !region.rect.isEmpty()) rects_.push_back(region.rect);
        }
    }

    /// Empty when every judged region is clear of the chrome the surface would
    /// paint over it. Recomputed per call: the hover card moves with the pointer.
    QStringList overlaps() const {
        QStringList out;
        if (surface_ == nullptr || host_ == nullptr || rects_.empty()) return out;
        const QPoint origin = surface_->mapTo(host_, QPoint(0, 0));
        const qreal ratio = surface_->devicePixelRatioF();
        for (QWidget* widget : widgets_) {
            if (widget == nullptr || !widget->isVisible()) continue;
            const QRect shellRect(widget->mapTo(host_, QPoint(0, 0)), widget->size());
            const QRect device(QPoint(qRound((shellRect.left() - origin.x()) * ratio),
                                      qRound((shellRect.top() - origin.y()) * ratio)),
                               QSize(qRound(shellRect.width() * ratio),
                                     qRound(shellRect.height() * ratio)));
            if (device.isEmpty()) continue;
            for (std::size_t index = 0; index < rects_.size(); ++index) {
                if (!device.intersects(rects_[index])) continue;
                out << QStringLiteral("region %1 under %2")
                           .arg(index + 1)
                           .arg(widget->objectName());
            }
        }
        out.removeDuplicates();
        return out;
    }

private:
    PlayerDialog* host_ = nullptr;
    VideoSurface* surface_ = nullptr;
    std::vector<QWidget*> widgets_;
    std::vector<QRect> rects_;
};

/// Collects audit hits over a stage without growing without bound.
void noteChromeHits(const QStringList& hits, QStringList* into) {
    if (into == nullptr || hits.isEmpty()) return;
    for (const QString& hit : hits) {
        if (!into->contains(hit)) into->append(hit);
    }
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

/// Writes the six judged crops as one strip: a band that disagrees with the
/// calibration can then be read off the artifact instead of guessed at from a
/// luma average. Diagnostics only - nothing is graded from this file.
bool saveRegionStrip(const QString& name, const PictureRead& read) {
    if (g_outputDir.isEmpty() || !read.measured) return false;
    constexpr int gap = 6;
    const int cell = kRegionSide + 2 * gap;
    QImage strip(cell * kRegionCount, cell, QImage::Format_RGB888);
    strip.fill(QColor(24, 24, 24));
    QPainter painter(&strip);
    for (int index = 0; index < kRegionCount; ++index) {
        const RegionRead& region = read.regions[static_cast<std::size_t>(index)];
        if (region.pixels.isNull()) continue;
        painter.drawImage(gap + index * cell, gap, region.pixels);
    }
    painter.end();
    return strip.save(artifactPath(name));
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

    /// Which judged regions fell below their own floor in this read, as a bit
    /// mask. "The band does not carry the content" is unactionable on its own;
    /// this says which crop disagreed, which is what tells a covered region
    /// from a dark frame.
    int darkRegionMask(const PictureRead& read) const {
        int mask = 0;
        if (!read.measured) return mask;
        for (int index = 0; index < kRegionCount; ++index) {
            const auto spot = static_cast<std::size_t>(index);
            if (!meaningful[spot] || !read.regions[spot].valid) continue;
            if (read.regions[spot].luma < floorFor(index)) mask |= 1 << index;
        }
        return mask;
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

/// One class's share of the phase counters. `passes` is the divisor for every
/// mean, so a class the window never painted reports -1 rather than a fast zero;
/// the raw totals are kept beside the means because a share of a stage is not a
/// per-paint cost.
QJsonObject phaseClassObject(int pass, quint64 passes, quint64 engineMicros,
                             quint64 retentionMicros, quint64 overlayMicros,
                             quint64 marginsMicros) {
    const auto perPass = [passes](quint64 micros) {
        return passes > 0 ? double(micros) / double(passes) : -1.0;
    };
    return QJsonObject{{"pass", QString::fromLatin1(passName(pass))},
                       {"passes", static_cast<qint64>(passes)},
                       {"engineUs", static_cast<qint64>(engineMicros)},
                       {"retentionUs", static_cast<qint64>(retentionMicros)},
                       {"overlayUs", static_cast<qint64>(overlayMicros)},
                       {"marginsUs", static_cast<qint64>(marginsMicros)},
                       {"engineUsPerPass", perPass(engineMicros)},
                       {"retentionUsPerPass", perPass(retentionMicros)},
                       {"overlayUsPerPass", perPass(overlayMicros)},
                       {"marginsUsPerPass", perPass(marginsMicros)}};
}

QJsonArray phaseByPassArray(const mikai::gui::uiprofile::Snapshot& counters) {
    QJsonArray classes;
    for (int pass = 0; pass < mikai::gui::uiprofile::kPaintPassCount; ++pass) {
        const auto& summary = counters.paintPhaseByPass[static_cast<std::size_t>(pass)];
        classes.append(phaseClassObject(pass, summary.passes, summary.engineMicros,
                                        summary.retentionMicros, summary.overlayMicros,
                                        summary.marginsMicros));
    }
    return classes;
}

/// Prints the attribution itself, so a run states which pass class spent the time
/// instead of leaving the reader to derive it from a mixed global sum.
void reportPhaseClasses(const QString& window,
                        const std::array<mikai::gui::uiprofile::PaintPhaseClassSummary,
                                         mikai::gui::uiprofile::kPaintPassCount>& classes) {
    for (int pass = 0; pass < mikai::gui::uiprofile::kPaintPassCount; ++pass) {
        const auto& summary = classes[static_cast<std::size_t>(pass)];
        if (summary.passes == 0) continue;
        const auto perPass = [&summary](quint64 micros) {
            return double(micros) / double(summary.passes);
        };
        metric(QStringLiteral("%1 phases %2: passes=%3 engine=%4us/%5 retention=%6us/%7 "
                              "overlay=%8us/%9 margins=%10us/%11")
                   .arg(window, QString::fromLatin1(passName(pass)))
                   .arg(summary.passes)
                   .arg(perPass(summary.engineMicros), 0, 'f', 1)
                   .arg(summary.engineMicros)
                   .arg(perPass(summary.retentionMicros), 0, 'f', 1)
                   .arg(summary.retentionMicros)
                   .arg(perPass(summary.overlayMicros), 0, 'f', 1)
                   .arg(summary.overlayMicros)
                   .arg(perPass(summary.marginsMicros), 0, 'f', 1)
                   .arg(summary.marginsMicros));
    }
}

/// One class's share of one measurement stage: a difference of two snapshots, so
/// the stage is not contaminated by the passes that ran before it.
mikai::gui::uiprofile::PaintPhaseClassSummary phaseWindowClass(
    const mikai::gui::uiprofile::Snapshot& before,
    const mikai::gui::uiprofile::Snapshot& after, int pass) {
    mikai::gui::uiprofile::PaintPhaseClassSummary out;
    if (pass < 0 || pass >= mikai::gui::uiprofile::kPaintPassCount) return out;
    const auto& from = before.paintPhaseByPass[static_cast<std::size_t>(pass)];
    const auto& to = after.paintPhaseByPass[static_cast<std::size_t>(pass)];
    const auto delta = [](quint64 first, quint64 second) {
        return second >= first ? second - first : 0;
    };
    out.passes = delta(from.passes, to.passes);
    out.engineMicros = delta(from.engineMicros, to.engineMicros);
    out.retentionMicros = delta(from.retentionMicros, to.retentionMicros);
    out.overlayMicros = delta(from.overlayMicros, to.overlayMicros);
    out.marginsMicros = delta(from.marginsMicros, to.marginsMicros);
    return out;
}

/// The same window as one line, so the run states its own attribution.
QString phaseWindowSummary(const mikai::gui::uiprofile::Snapshot& before,
                           const mikai::gui::uiprofile::Snapshot& after) {
    QStringList parts;
    for (int pass = 0; pass < mikai::gui::uiprofile::kPaintPassCount; ++pass) {
        const mikai::gui::uiprofile::PaintPhaseClassSummary summary =
            phaseWindowClass(before, after, pass);
        if (summary.passes == 0) continue;
        parts << QStringLiteral("%1 passes=%2 engine=%3us retention=%4us overlay=%5us "
                                "margins=%6us")
                     .arg(QString::fromLatin1(passName(pass)))
                     .arg(summary.passes)
                     .arg(summary.engineMicros)
                     .arg(summary.retentionMicros)
                     .arg(summary.overlayMicros)
                     .arg(summary.marginsMicros);
    }
    return parts.isEmpty() ? QStringLiteral("no classified pass") : parts.join(QStringLiteral("; "));
}

/// What one measurement stage paid, by class: a difference of two snapshots, so
/// the stage is not contaminated by the passes that ran before it.
QJsonObject phaseWindowReport(const mikai::gui::uiprofile::Snapshot& before,
                              const mikai::gui::uiprofile::Snapshot& after,
                              const QString& window) {
    QJsonArray classes;
    for (int pass = 0; pass < mikai::gui::uiprofile::kPaintPassCount; ++pass) {
        const mikai::gui::uiprofile::PaintPhaseClassSummary summary =
            phaseWindowClass(before, after, pass);
        classes.append(phaseClassObject(pass, summary.passes, summary.engineMicros,
                                        summary.retentionMicros, summary.overlayMicros,
                                        summary.marginsMicros));
    }
    return QJsonObject{{"window", window}, {"classes", classes}};
}

/// The profile counters the verdicts lean on, averaged per painted pass and
/// printed once per window: a class p95 with no phase split names no cost, and a
/// cache hit ratio with no miss reason names no defect.
QJsonObject profileReport(const mikai::gui::uiprofile::Snapshot& counters,
                          const QString& window) {
    qint64 paints = 0;
    for (int pass = 0; pass < mikai::gui::uiprofile::kPaintPassCount; ++pass) {
        paints += static_cast<qint64>(mikai::gui::uiprofile::paintPassSamples(pass));
    }
    const auto perPaint = [paints](quint64 micros) {
        return paints > 0 ? double(micros) / double(paints) : -1.0;
    };
    const quint64 misses = counters.chromeCacheMissDirty + counters.chromeCacheMissGeometry +
                           counters.chromeCacheMissRatio + counters.chromeCacheMissGeneration +
                           counters.chromeCacheMissBudget;
    metric(QStringLiteral("profile[%1]: paints=%2 engine=%3us retention=%4us overlay=%5us "
                          "margins=%6us chrome_paint_calls=%7 chrome_micros_avg=%8 "
                          "blit=%9us raster=%10us hit=%11 entries=%12 misses(dirty=%13 "
                          "geom=%14 ratio=%15 gen=%16 budget=%17) cache_bytes=%18")
               .arg(window)
               .arg(paints)
               .arg(perPaint(counters.paintPhaseEngineMicros), 0, 'f', 1)
               .arg(perPaint(counters.paintPhaseRetentionMicros), 0, 'f', 1)
               .arg(perPaint(counters.paintPhaseOverlayMicros), 0, 'f', 1)
               .arg(perPaint(counters.paintPhaseMargins), 0, 'f', 1)
               .arg(counters.chromePaintCalls)
               .arg(counters.chromePaintCalls > 0
                        ? double(counters.chromePaintMicrosTotal) /
                              double(counters.chromePaintCalls)
                        : -1.0,
                    0, 'f', 1)
               .arg(counters.chromeBlitMicros)
               .arg(counters.chromeRasterMicros)
               .arg(counters.chromePaintCached)
               .arg(counters.chromePaintEntries)
               .arg(counters.chromeCacheMissDirty)
               .arg(counters.chromeCacheMissGeometry)
               .arg(counters.chromeCacheMissRatio)
               .arg(counters.chromeCacheMissGeneration)
               .arg(counters.chromeCacheMissBudget)
               .arg(counters.chromeCacheBytes));
    reportPhaseClasses(window, counters.paintPhaseByPass);
    return QJsonObject{
        {"window", window},
        {"paints", paints},
        {"engineUsPerPaint", perPaint(counters.paintPhaseEngineMicros)},
        {"retentionUsPerPaint", perPaint(counters.paintPhaseRetentionMicros)},
        {"overlayUsPerPaint", perPaint(counters.paintPhaseOverlayMicros)},
        {"marginsUsPerPaint", perPaint(counters.paintPhaseMargins)},
        {"chromePaintCalls", static_cast<qint64>(counters.chromePaintCalls)},
        {"chromePaintUsPerCall",
         counters.chromePaintCalls > 0
             ? double(counters.chromePaintMicrosTotal) / double(counters.chromePaintCalls)
             : -1.0},
        {"chromePaintUsMax", static_cast<qint64>(counters.chromePaintMicrosMax)},
        {"chromeBlitUs", static_cast<qint64>(counters.chromeBlitMicros)},
        {"chromeRasterUs", static_cast<qint64>(counters.chromeRasterMicros)},
        {"chromeCached", static_cast<qint64>(counters.chromePaintCached)},
        {"chromeEntries", static_cast<qint64>(counters.chromePaintEntries)},
        {"misses", static_cast<qint64>(misses)},
        {"missDirty", static_cast<qint64>(counters.chromeCacheMissDirty)},
        {"missGeometry", static_cast<qint64>(counters.chromeCacheMissGeometry)},
        {"missRatio", static_cast<qint64>(counters.chromeCacheMissRatio)},
        {"missGeneration", static_cast<qint64>(counters.chromeCacheMissGeneration)},
        {"missBudget", static_cast<qint64>(counters.chromeCacheMissBudget)},
        {"chromeCacheBytes", static_cast<qint64>(counters.chromeCacheBytes)},
        {"phaseByPass", phaseByPassArray(counters)}};
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

/// The scale facts a paint-cost number is only comparable inside: the surface's
/// own DPR and the logical/device size it paints into, the screen Qt resolved it
/// on, and the launch-time QT_SCALE_FACTOR. Read from the widget and the display,
/// never by asking for a paint and never by resizing anything - a historical
/// before-side that did not record these cannot be told from this one without
/// them, and matching a host at a different DPR is guesswork.
QJsonObject scaleEnvironment(const QWidget* widget) {
    QJsonObject out;
    if (widget == nullptr) return out;
    const qreal ratio = widget->devicePixelRatioF();
    const QSize logical = widget->size();
    out.insert(QStringLiteral("surfaceDevicePixelRatio"), ratio);
    out.insert(QStringLiteral("surfaceLogicalWidth"), logical.width());
    out.insert(QStringLiteral("surfaceLogicalHeight"), logical.height());
    // The surface paints with NoPartialUpdate into a default framebuffer sized by
    // the widget times its DPR; this is that size, not a measured GL query.
    out.insert(QStringLiteral("surfaceDeviceWidth"), qRound(logical.width() * ratio));
    out.insert(QStringLiteral("surfaceDeviceHeight"), qRound(logical.height() * ratio));
    if (const QScreen* screen = widget->screen()) {
        const QRect geometry = screen->geometry();
        out.insert(QStringLiteral("screenName"), screen->name());
        out.insert(QStringLiteral("screenLogicalX"), geometry.x());
        out.insert(QStringLiteral("screenLogicalY"), geometry.y());
        out.insert(QStringLiteral("screenLogicalWidth"), geometry.width());
        out.insert(QStringLiteral("screenLogicalHeight"), geometry.height());
        // Qt's effective screen ratio: on Qt 6 this can already include a global
        // (QT_SCALE_FACTOR) factor as well as the platform's own scale, so it is
        // recorded as what Qt reports and never as a "native" base.
        out.insert(QStringLiteral("screenDevicePixelRatio"), screen->devicePixelRatio());
    }
    const QString scaleFactor = qEnvironmentVariable("QT_SCALE_FACTOR");
    out.insert(QStringLiteral("qtScaleFactor"),
               scaleFactor.isEmpty() ? QStringLiteral("unset") : scaleFactor);
    return out;
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
///
/// `afterRound` runs once per round and never inside a timed paint: it is where
/// a stage checks that the chrome it just woke did not stand over the band it
/// is about to grade.
int sweepChrome(QWidget* first, QWidget* second, QWidget* scrub, int durationMs,
                const std::function<void()>& afterRound = {}) {
    QElapsedTimer clock;
    clock.start();
    int moves = 0;
    while (clock.elapsed() < durationMs) {
        moves += hoverRound(first, second, scrub);
        if (afterRound) afterRound();
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
int runContextLoss(PlayerDialog* dialog, VideoSurface* surface, const Baseline& baseline,
                   const QElapsedTimer& runClock) {
    if (dialog == nullptr || surface == nullptr) return 3;
    const bool wasPlaying = surface->isPlaying();
    const QPointer<VideoSurface> guardedSurface(surface);
    const auto restorePlayback = qScopeGuard([guardedSurface, wasPlaying] {
        if (guardedSurface && wasPlaying) guardedSurface->setPaused(false);
    });
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
    if (!pausedBefore) {
        metric(QStringLiteral("context recreation: the initial pause was not acknowledged"));
        return 3;
    }

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
    // Wait for the asynchronous reload's seek before judging retained position.
    const qint64 reloadClockStart = runClock.elapsed();
    bool positionRestored = positionBefore <= 0.0 ||
                            surface->position() + 1.5 >= positionBefore;
    while (!positionRestored && runClock.elapsed() - reloadClockStart < 12000) {
        pump(120);
        positionRestored = surface->position() + 1.5 >= positionBefore;
    }
    const qint64 reloadWaitMs = runClock.elapsed() - reloadClockStart;
    const PictureRead after =
        readPicture(surface, baseline.device, baseline.videoWidth, baseline.videoHeight);
    noteObserverRead(after.micros);
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
                              "recreation (waited %1 ms for the reload to report it)")
                   .arg(reloadWaitMs));
        ++failures;
    }
    // Display proof, not memory proof: a CPU copy that never reaches the surface
    // is not "the picture survived", so the framebuffer has to carry it.
    if (!baseline.present(after)) {
        metric(QStringLiteral("context recreation: the framebuffer does not carry the picture "
                              "after the recreation (retained_bytes=%1 is only a memory sample)")
                   .arg(surface->retainedPictureBytes()));
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
    /// The device size the crops were read out of: a region read against a
    /// stale fallback size is a measurement fault, not a dark picture.
    QSize framebufferDevice;
    int framebufferDarkRegions = 0;
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
    /// The last epoch any sample reported, whether or not a real output was
    /// ever recognised. `epochAfter` is only the epoch of the accepted first
    /// output, so a run with no output must not print it as "the epoch reset".
    quint64 epochLastObserved = 0;
    /// The first sample of the new epoch that reported a visible output frame,
    /// accepted or not: it separates "no frame ever arrived" from "a frame
    /// arrived and the band did not carry it".
    qint64 firstVisibleOutputMs = -1;
    int firstVisibleDarkRegions = 0;
    QSize firstVisibleDevice;
    /// The new source reached its own end before a real output was recognised.
    /// Blackness after that instant is the source having ended, not a gap.
    bool sourceEndedWithoutOutput = false;
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
                                    bool movePointerDuringGap, ChromeAudit* audit = nullptr,
                                    QStringList* chromeHits = nullptr) {
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
            // Geometry only, no framebuffer read: the band and the chrome are
            // both known, so an overlap is answerable between two samples.
            if (audit != nullptr) noteChromeHits(audit->overlaps(), chromeHits);
        }
        const PictureRead read =
            readPicture(surface, calibration.device, calibration.videoWidth,
                        calibration.videoHeight, /*keepPixels=*/false);
        noteObserverRead(read.micros);
        if (audit != nullptr) {
            audit->observe(read);
            noteChromeHits(audit->overlaps(), chromeHits);
        }
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
        sample.framebufferDevice = read.framebuffer;
        sample.framebufferDarkRegions = calibration.darkRegionMask(read);
        outcome.epochLastObserved = sample.epoch;
        if (sample.epoch != outcome.epochBefore && sample.hasVisibleOutput &&
            outcome.firstVisibleOutputMs < 0) {
            outcome.firstVisibleOutputMs = sample.ms;
            outcome.firstVisibleDarkRegions = sample.framebufferDarkRegions;
            outcome.firstVisibleDevice = read.framebuffer;
            if (!sample.framebufferPicture && !g_outputDir.isEmpty()) {
                // Keep the crops of the decisive read: a band that disagreed
                // with the calibration is only explainable from its pixels.
                saveRegionStrip(QStringLiteral("transition-%1-band.png")
                                    .arg(++g_transitionArtifacts),
                                readPicture(surface, calibration.device, calibration.videoWidth,
                                            calibration.videoHeight));
            }
        }
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
                {"framebufferDarkRegions", sample.framebufferDarkRegions},
                {"framebufferDeviceWidth", sample.framebufferDevice.width()},
                {"framebufferDeviceHeight", sample.framebufferDevice.height()},
                {"framebufferPicture", sample.framebufferPicture}});
        }
        // "Real output of the new source": the epoch moved, the surface reports
        // a visible output frame of it, and the framebuffer band carries the
        // calibrated content. All three, so a held picture cannot answer for it.
        // A held picture is lit too, so it must NOT count as the new output: the
        // first output is the first sample that is lit, of the new epoch, and no
        // longer showing the retained snapshot.
        if (sample.epoch != outcome.epochBefore && sample.hasVisibleOutput &&
            sample.framebufferPicture && !sample.retained) {
            outcome.firstOutputMs = sample.ms;
            outcome.epochAfter = sample.epoch;
            outcome.framebufferPictureAfter = true;
            break;
        }
        // The NEW source reached its own end without ever being recognised. The
        // gap this stage measures ends there: every sample after it is the
        // player showing a finished source, so counting those as "black in the
        // gap" would blame the load for the clip running out. The epoch test
        // keeps the outgoing source's own end from being read as the new one's.
        // A frame of the new epoch has to have been seen first: the outgoing
        // source's own duration and position are still readable for an instant
        // after the switch, and those must not be mistaken for the new one's
        // end.
        const bool newSourceEnded = outcome.firstVisibleOutputMs >= 0 &&
                                    sample.epoch != outcome.epochBefore && !sample.opening &&
                                    !sample.hasVideo && sample.duration > 0.0 &&
                                    sample.position >= sample.duration - 0.25 &&
                                    !sample.retained;
        if (newSourceEnded) {
            outcome.sourceEndedWithoutOutput = outcome.firstOutputMs < 0;
            break;
        }
    }
    outcome.measured = true;
    if (outcome.firstOutputMs >= 0) outcome.gapMs = outcome.firstOutputMs - outcome.requestMs;
    // No accepted output means `epochAfter` was never written: report the epoch
    // the samples actually ended on, so a summary cannot claim the source epoch
    // collapsed to zero when it only ever advanced.
    if (outcome.epochAfter == 0) outcome.epochAfter = outcome.epochLastObserved;

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
        {"epochLastObserved", static_cast<double>(outcome.epochLastObserved)},
        {"firstVisibleOutputMs", static_cast<double>(outcome.firstVisibleOutputMs)},
        {"firstVisibleDarkRegions", outcome.firstVisibleDarkRegions},
        {"firstVisibleDevice", QStringLiteral("%1x%2")
                                   .arg(outcome.firstVisibleDevice.width())
                                   .arg(outcome.firstVisibleDevice.height())},
        {"sourceEndedWithoutOutput", outcome.sourceEndedWithoutOutput},
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
    if (outcome.firstOutputMs < 0) {
        // No accepted output: say which question failed. A visible frame the
        // band rejected names the regions that disagreed; a source that ended
        // instead names the end. Neither is left for the reader to guess.
        metric(QStringLiteral("%1: no accepted output; first visible frame=%2ms dark_regions=0x%3 "
                              "device=%4x%5 source_ended=%6 epoch_last=%7")
                   .arg(label)
                   .arg(outcome.firstVisibleOutputMs)
                   .arg(outcome.firstVisibleDarkRegions, 0, 16)
                   .arg(outcome.firstVisibleDevice.width())
                   .arg(outcome.firstVisibleDevice.height())
                   .arg(int(outcome.sourceEndedWithoutOutput))
                   .arg(static_cast<qulonglong>(outcome.epochLastObserved)));
    }
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
    // Recorded before any measured stage and outside every timed paint: the
    // hovering numbers are only comparable to a run on the same surface DPR and
    // the same screen geometry.
    evidence.insert(QStringLiteral("scaleEnvironment"), scaleEnvironment(surface));
    {
        const QJsonObject environment = scaleEnvironment(surface);
        metric(QStringLiteral("scale: surface_dpr=%1 logical=%2x%3 device=%4x%5 "
                              "screen=%6 %7x%8 screen_dpr=%9 qt_scale_factor=%10")
                   .arg(environment.value(QStringLiteral("surfaceDevicePixelRatio")).toDouble())
                   .arg(environment.value(QStringLiteral("surfaceLogicalWidth")).toInt())
                   .arg(environment.value(QStringLiteral("surfaceLogicalHeight")).toInt())
                   .arg(environment.value(QStringLiteral("surfaceDeviceWidth")).toInt())
                   .arg(environment.value(QStringLiteral("surfaceDeviceHeight")).toInt())
                   .arg(environment.value(QStringLiteral("screenName")).toString())
                   .arg(environment.value(QStringLiteral("screenLogicalWidth")).toInt())
                   .arg(environment.value(QStringLiteral("screenLogicalHeight")).toInt())
                   .arg(environment.value(QStringLiteral("screenDevicePixelRatio")).toDouble())
                   .arg(environment.value(QStringLiteral("qtScaleFactor")).toString()));
    }
    // The band's own geometry and the chrome that shares this framebuffer, bound
    // once: every stage below audits its reads against it, so a crop covered by
    // the dock or by the timeline's hover card is named instead of graded.
    ChromeAudit chromeAudit;
    chromeAudit.bind(dialog, surface);
    QStringList chromeHits;
    {
        const PictureRead settled =
            readPicture(surface, baseline.device, baseline.videoWidth, baseline.videoHeight);
        noteObserverRead(settled.micros);
        QJsonArray regionLuma;
        for (const RegionRead& region : settled.regions) regionLuma.append(region.luma);
        evidence.insert(QStringLiteral("outgoingRegionLuma"), regionLuma);
        chromeAudit.observe(settled);
        QStringList settledHits;
        noteChromeHits(chromeAudit.overlaps(), &settledHits);
        metric(QStringLiteral("band: regions=%1 device=%2x%3 chrome_overlaps=%4")
                   .arg(kRegionCount)
                   .arg(settled.framebuffer.width())
                   .arg(settled.framebuffer.height())
                   .arg(settledHits.isEmpty() ? QStringLiteral("none")
                                              : settledHits.join(QStringLiteral("; "))));
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
        // The phase counters are not reset by the pass ring, so the window is a
        // difference of two snapshots: what this paused sweep paid, by class.
        const mikai::gui::uiprofile::Snapshot pausedPhaseBefore =
            mikai::gui::uiprofile::snapshot();
        // The hover stage's own scale facts, read once before the sweep: the same
        // surface, outside every timed paint, with no geometry touched.
        const QJsonObject pausedScaleEnvironment = scaleEnvironment(surface);
        // The band this stage will grade, audited against the chrome the sweep
        // is about to wake: a card standing over a crop turns the pixel verdict
        // into a statement about chrome, so the overlap is named here.
        chromeAudit.observe(pausedBefore);
        noteChromeHits(chromeAudit.overlaps(), &chromeHits);
        const int hoverMoves = sweepChrome(
            playToggle, settingsButton, scrub, hoverSeconds * 1000,
            [&chromeAudit, &chromeHits] {
                noteChromeHits(chromeAudit.overlaps(), &chromeHits);
            });
        const mikai::gui::uiprofile::Snapshot pausedPhaseAfter =
            mikai::gui::uiprofile::snapshot();
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
        int comparedRegions = 0;
        QStringList differing;
        QJsonArray regionDeltas;
        for (int index = 0; index < kRegionCount; ++index) {
            const auto spot = static_cast<std::size_t>(index);
            const int exact = changedPixels(pausedBefore.regions[spot].pixels,
                                            pausedAfter.regions[spot].pixels, 0);
            const int visible = changedPixels(pausedBefore.regions[spot].pixels,
                                              pausedAfter.regions[spot].pixels, 24);
            if (exact < 0 || visible < 0) continue;
            ++comparedRegions;
            compared += pausedBefore.regions[spot].pixels.width() *
                        pausedBefore.regions[spot].pixels.height();
            changedExact += exact;
            changedVisible += visible;
            // Which crop disagreed, with both lumas: a number alone cannot say
            // whether the video moved or something was painted over it.
            const QRect rect = pausedBefore.regions[spot].rect;
            regionDeltas.append(QJsonObject{
                {"region", index + 1},
                {"rect", QStringLiteral("%1,%2 %3x%4")
                             .arg(rect.left())
                             .arg(rect.top())
                             .arg(rect.width())
                             .arg(rect.height())},
                {"lumaBefore", pausedBefore.regions[spot].luma},
                {"lumaAfter", pausedAfter.regions[spot].luma},
                {"changedExact", exact},
                {"changedVisible", visible}});
            if (exact > 0) {
                differing << QStringLiteral("region %1 %2px (luma %3->%4)")
                                 .arg(index + 1)
                                 .arg(exact)
                                 .arg(pausedBefore.regions[spot].luma, 0, 'f', 1)
                                 .arg(pausedAfter.regions[spot].luma, 0, 'f', 1);
            }
        }
        if (!g_outputDir.isEmpty()) {
            saveRegionStrip(QStringLiteral("load-transition-paused-before.png"), pausedBefore);
            saveRegionStrip(QStringLiteral("load-transition-paused-after.png"), pausedAfter);
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
        const QJsonObject pausedPhases =
            phaseWindowReport(pausedPhaseBefore, pausedPhaseAfter,
                              QStringLiteral("paused-hover"));
        const mikai::gui::uiprofile::PaintPhaseClassSummary pausedChromePhase =
            phaseWindowClass(pausedPhaseBefore, pausedPhaseAfter,
                             mikai::gui::uiprofile::kPaintChromeOnly);
        metric(QStringLiteral("paused-hover phase window: chrome-only passes=%1 engine=%2us "
                              "retention=%3us overlay=%4us margins=%5us")
                   .arg(pausedChromePhase.passes)
                   .arg(pausedChromePhase.engineMicros)
                   .arg(pausedChromePhase.retentionMicros)
                   .arg(pausedChromePhase.overlayMicros)
                   .arg(pausedChromePhase.marginsMicros));
        check(hoverMoves > 0, "the paused measurement actually moved the pointer over chrome");
        check(chromeOnly.samples > 0,
              "a paused chrome repaint is classified as a chrome-only pass");
        // Six compared crops, or the verdict describes fewer pixels than it
        // claims: a size change between the two reads used to drop a region
        // silently, which turned "nothing changed" into "nothing was compared".
        check(comparedRegions == kRegionCount,
              QStringLiteral("every judged region could be compared across the hover "
                             "(compared=%1 of %2)")
                  .arg(comparedRegions)
                  .arg(kRegionCount));
        check(changedExact == 0,
              QStringLiteral("a paused picture is pixel-identical across chrome hovers "
                             "(exact=%1 visible=%2 of %3; %4)")
                  .arg(changedExact)
                  .arg(changedVisible)
                  .arg(compared)
                  .arg(differing.isEmpty() ? QStringLiteral("no region differed")
                                           : differing.join(QStringLiteral(", "))));
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
                                    {"regionDeltas", regionDeltas},
                                    {"comparedRegions", comparedRegions},
                                    {"chromeOnly", passObject(chromeOnly)},
                                    {"retained", passObject(retained)},
                                    {"videoFrame", passObject(videoFrame)},
                                    {"phaseByPass", pausedPhases},
                                    {"scaleEnvironment", pausedScaleEnvironment}});
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
        const mikai::gui::uiprofile::Snapshot activePhaseBefore =
            mikai::gui::uiprofile::snapshot();
        const int hoverMoves =
            sweepChrome(playToggle, settingsButton, scrub, hoverSeconds * 1000);
        const mikai::gui::uiprofile::Snapshot activePhaseAfter =
            mikai::gui::uiprofile::snapshot();
        const PassSummary chromeOnly = summarize(mikai::gui::uiprofile::kPaintChromeOnly);
        const PassSummary videoFrame = summarize(mikai::gui::uiprofile::kPaintVideoFrame);
        metric(QStringLiteral("active hover: moves=%1 chrome_paints=%2 p95=%3us video_p95=%4us")
                   .arg(hoverMoves)
                   .arg(chromeOnly.samples)
                   .arg(chromeOnly.p95, 0, 'f', 1)
                   .arg(videoFrame.p95, 0, 'f', 1));
        reportPasses(QStringLiteral("active-hover"));
        metric(QStringLiteral("active-hover phase window: %1")
                   .arg(phaseWindowSummary(activePhaseBefore, activePhaseAfter)));
        evidence.insert(QStringLiteral("activeHover"),
                        QJsonObject{{"moves", hoverMoves},
                                    {"chromeOnly", passObject(chromeOnly)},
                                    {"videoFrame", passObject(videoFrame)},
                                    {"phaseByPass", phaseWindowReport(activePhaseBefore,
                                                                      activePhaseAfter,
                                                                      QStringLiteral("active-hover"))}});
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
        const bool countersOn = mikai::gui::uiprofile::countersEnabled();
        const QSize surfaceBefore = surface->size();
        // The paused and active stages are the profile window this run exists
        // for; the format stage below resets the counters, so that window is read
        // out here rather than lost.
        const QJsonObject pausedActiveProfile =
            profileReport(mikai::gui::uiprofile::snapshot(), QStringLiteral("paused+active"));
        evidence.insert(QStringLiteral("profilePausedActive"), pausedActiveProfile);
        // These counters describe this stage only; the earlier stages keep
        // their own numbers.
        mikai::gui::uiprofile::resetCounters();
        int distinctSizes = 0;
        QSize lastSurface = surfaceBefore;
        for (int index = 0; index < 200; ++index) {
            const bool wider = index % 2 == 0;
            dialog->resize(wider ? std::max(320, base.width() - 96) : base.width() - 32,
                           wider ? std::max(200, base.height() - 72) : base.height() - 24);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            const QSize now = surface->size();
            if (now != lastSurface) {
                ++distinctSizes;
                lastSurface = now;
            }
        }
        pump(400);
        const auto counters = mikai::gui::uiprofile::snapshot();
        metric(QStringLiteral("format probe: resizes=200 base=%1x%2 surface=%3x%4->%5x%6 "
                              "distinct_sizes=%7 probes=%8 format_resets=%9 counters=%10")
                   .arg(base.width())
                   .arg(base.height())
                   .arg(surfaceBefore.width())
                   .arg(surfaceBefore.height())
                   .arg(surface->size().width())
                   .arg(surface->size().height())
                   .arg(distinctSizes)
                   .arg(counters.renderTargetProbes)
                   .arg(counters.renderTargetFormatResets)
                   .arg(int(countersOn)));
        // A zero is only evidence when the sweep really resized the surface and
        // the counters could move: otherwise "no re-probe" describes a window
        // that never resized anything.
        const bool observable = countersOn && distinctSizes > 0;
        if (observable) {
            check(counters.renderTargetProbes <= 2,
                  QStringLiteral("200 resizes re-probe the render target at most twice "
                                 "(probes=%1)")
                      .arg(counters.renderTargetProbes));
        } else {
            metric(QStringLiteral("format probe: unobservable this run (counters=%1 "
                                  "distinct_sizes=%2), so no probe-budget verdict is claimed")
                       .arg(int(countersOn))
                       .arg(distinctSizes));
        }
        evidence.insert(QStringLiteral("formatProbe"),
                        QJsonObject{{"resizes", 200},
                                    {"baseWidth", base.width()},
                                    {"baseHeight", base.height()},
                                    {"countersEnabled", countersOn},
                                    {"distinctSurfaceSizes", distinctSizes},
                                    {"observable", observable},
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
        const int result = runContextLoss(dialog, surface, baseline, runClock);
        // 0 measured, 1 failed, 3 unobservable. Unobservable is not a pass: the
        // environment could not recreate the context, so nothing supports the
        // retention claim.
        check(result == 0,
              QStringLiteral("a paused picture and its pause intent survive a real context "
                             "recreation (result=%1)")
                  .arg(result));
        // The next stages measure a transition, so they must start from a playing
        // source: this stage left it wherever the viewer's intent was.
        surface->setPaused(false);
        if (!waitFor([surface] {
                return surface->isPlaying() && surface->hasVisibleOutputFrame();
            }, 8000)) {
            metric(QStringLiteral("context recreation: the source did not resume before the "
                                  "transition stages"));
        }
        pump(300);
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
            30000, /*movePointerDuringGap=*/true, &chromeAudit, &chromeHits);
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
        check(reported.framebufferMoved,
              QStringLiteral("%1: the new output actually moves, so a frozen retained image "
                             "cannot answer for it")
                  .arg(label));
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
            30000, /*movePointerDuringGap=*/true, &chromeAudit, &chromeHits);
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

    // The chrome shares this framebuffer with the picture, so a crop under it is
    // not a video pixel. Every measured read above was audited against the
    // chrome standing over the band; a hit means the pixel verdict above is
    // about chrome, which this says out loud instead of leaving it implied.
    metric(QStringLiteral("chrome audit: overlaps=%1")
               .arg(chromeHits.isEmpty() ? QStringLiteral("none")
                                         : chromeHits.join(QStringLiteral("; "))));
    check(chromeHits.isEmpty(),
          QStringLiteral("the judged band stays clear of the chrome painted into the picture "
                         "(%1)")
              .arg(chromeHits.isEmpty() ? QStringLiteral("no overlap")
                                        : chromeHits.join(QStringLiteral("; "))));
    evidence.insert(QStringLiteral("chromeAudit"),
                    QJsonObject{{"overlaps", QJsonArray::fromStringList(chromeHits)},
                                {"auditedChromeWidgets",
                                 QStringLiteral("dock, hover card, title bar, info chip and the "
                                                "badges the shell paints into the surface")}});

    const QJsonObject observer = observerObject();
    metric(QStringLiteral("observer: reads=%1 total=%2us p50=%3us p95=%4us max=%5us")
               .arg(observer.value(QStringLiteral("framebufferReads")).toInt())
               .arg(observer.value(QStringLiteral("readTotalUs")).toDouble(), 0, 'f', 0)
               .arg(observer.value(QStringLiteral("readP50Us")).toDouble(), 0, 'f', 1)
               .arg(observer.value(QStringLiteral("readP95Us")).toDouble(), 0, 'f', 1)
               .arg(observer.value(QStringLiteral("readMaxUs")).toDouble(), 0, 'f', 1));
    evidence.insert(QStringLiteral("observerCost"), observer);
    evidence.insert(QStringLiteral("profileAfterFormatReset"),
                    profileReport(mikai::gui::uiprofile::snapshot(),
                                  QStringLiteral("after format reset")));
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
