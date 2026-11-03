// Presentation-path profiling, gated by MIKAI_UI_PROFILE=1.
//
// Logging only. Call sites guard string construction with enabled(); counters
// are process-wide because the surface, shell and raise policy all contribute.
// Output is `[ui-profile] <scope> key=value ...` on stderr.

#pragma once

#include <QByteArray>
#include <QString>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace mikai::gui::uiprofile {

/// True when MIKAI_UI_PROFILE is set to anything but an empty string or "0".
/// Read once per process: the switch is a diagnostic launch option, and a value
/// that changed mid-run would make the log say two different things.
inline bool enabled() {
    static const bool on = [] {
        const QString value = qEnvironmentVariable("MIKAI_UI_PROFILE");
        return !value.isEmpty() && value != QStringLiteral("0");
    }();
    return on;
}

/// Counters are collected when profiling is on, and also while a diagnostic
/// frame-timing log is being written: that log carries the same counters and
/// must not print a fabricated zero just because MIKAI_UI_PROFILE was omitted.
inline std::atomic<bool> g_countersForced{false};

inline bool countersEnabled() {
    return enabled() || g_countersForced.load(std::memory_order_relaxed);
}

inline void forceCounters(bool forced) {
    g_countersForced.store(forced, std::memory_order_relaxed);
}

inline qint64 nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Milliseconds between `startNs` and now. A start of 0 means "not measured",
/// which is reported as -1 rather than as a plausible zero.
inline qint64 elapsedMs(qint64 startNs) {
    return startNs == 0 ? -1 : (nowNs() - startNs) / 1000000;
}

/// A duration whose clock only starts when profiling is on.
class ScopedTimer {
public:
    ScopedTimer() : started_(countersEnabled() ? nowNs() : 0) {}
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    qint64 ms() const { return elapsedMs(started_); }
    qint64 micros() const { return started_ == 0 ? -1 : (nowNs() - started_) / 1000; }
    bool measured() const { return started_ != 0; }

private:
    qint64 started_;
};

/// Writes one `[ui-profile] <scope> <message>` line to stderr. Callers must
/// check enabled() themselves so the message is never built when profiling is
/// off; the check here is a belt-and-braces guard for a stray call.
inline void log(const char* scope, const QString& message) {
    if (!enabled()) return;
    const QByteArray line = message.toUtf8();
    std::fprintf(stderr, "[ui-profile] %s %s\n", scope, line.constData());
    std::fflush(stderr);
}

// ── process-wide counters ─────────────────────────────────────────────────
// Each counter counts the event only while profiling (or a forced diagnostic
// log) is on; the guard lives in the call site (see the note above). Relaxed
// ordering: these are counters a probe reads after the run, never a
// synchronization device.

inline std::atomic<quint64> g_chromeDirtyRequests{0};
inline std::atomic<quint64> g_chromeDirtyApplied{0};
inline std::atomic<quint64> g_setMaskApplications{0};
inline std::atomic<quint64> g_layoutGeometryWrites{0};
inline std::atomic<quint64> g_threadInputAttaches{0};
inline std::atomic<quint64> g_chromePaintCalls{0};
inline std::atomic<quint64> g_chromePaintEntries{0};
/// Controls served from the cached raster layer instead of being rasterised
/// again. The difference between the entry count and this one is the work a
/// painted video frame still pays for chrome.
inline std::atomic<quint64> g_chromePaintCached{0};
inline std::atomic<qint64> g_chromePaintLastMicros{-1};
inline std::atomic<quint64> g_chromePaintMicrosTotal{0};
inline std::atomic<qint64> g_chromePaintMicrosMax{-1};
inline std::atomic<qint64> g_lastActivationToPaintMs{-1};
/// GOAL-E counters. Each answers one criterion that would otherwise be a claim:
/// the render-target probe budget across a resize storm, the chrome raster
/// cache's renders and held bytes, the stacking poll's stops while another
/// application is foreground, aborted raise retries, teardown pictures kept,
/// the player background's rebuilds, and how many widgets one TitlePage
/// repolish pass touched.
inline std::atomic<quint64> g_renderTargetProbes{0};
inline std::atomic<quint64> g_renderTargetFormatResets{0};
inline std::atomic<quint64> g_chromeCacheRenders{0};
/// Where the chrome pass spends its time, and why a control was rasterised again.
/// A cache meant to replace rasterisation has to prove which half of the cost it
/// moved: blitting a cached pixmap and rasterising a control are different
/// symptoms, and a miss has a reason.
inline std::atomic<quint64> g_chromeBlitMicros{0};
inline std::atomic<quint64> g_chromeRasterMicros{0};
inline std::atomic<quint64> g_chromeCacheMissDirty{0};
inline std::atomic<quint64> g_chromeCacheMissGeometry{0};
inline std::atomic<quint64> g_chromeCacheMissRatio{0};
inline std::atomic<quint64> g_chromeCacheMissGeneration{0};
inline std::atomic<quint64> g_chromeCacheMissBudget{0};
/// A paintGL pass is one number in the paint-pass ring, but it is three different
/// costs: mpv's render into the target, the held-picture or teardown-picture
/// blit, and the chrome overlay. Blaming one of them for a class p95 without
/// splitting them is how a cache gets accused of a copy. Phases are
/// kPaintPhaseEngine, kPaintPhaseRetention, kPaintPhaseOverlay.
inline std::atomic<quint64> g_paintPhaseEngineMicros{0};
inline std::atomic<quint64> g_paintPhaseRetentionMicros{0};
inline std::atomic<quint64> g_paintPhaseOverlayMicros{0};
inline std::atomic<quint64> g_paintPhaseMargins{0};
/// Occupancy of the ChromeList that published LAST, not a process total. Each
/// list has its own 8 MiB budget, and the shipping player plus the pooled float
/// can both hold a cache at once (up to 16 MiB), so this is one list's number and
/// a historical sample: resetCounters() zeroes it without dropping any cache, so
/// a reset only means something when a cache event follows it. A destroyed list
/// never publishes, which leaves its last sample standing.
inline std::atomic<quint64> g_chromeCacheBytes{0};
inline std::atomic<quint64> g_awayStackingStops{0};
inline std::atomic<quint64> g_raiseRetriesAborted{0};
inline std::atomic<quint64> g_teardownPicturesCaptured{0};
inline std::atomic<quint64> g_backgroundRenders{0};
inline std::atomic<quint64> g_titleRepolishWidgets{0};

inline void noteChromeDirtyRequest() {
    if (!countersEnabled()) return;
    g_chromeDirtyRequests.fetch_add(1, std::memory_order_relaxed);
}

inline void noteChromeDirtyApplied() {
    if (!countersEnabled()) return;
    g_chromeDirtyApplied.fetch_add(1, std::memory_order_relaxed);
}

inline void noteSetMask() {
    if (!countersEnabled()) return;
    g_setMaskApplications.fetch_add(1, std::memory_order_relaxed);
}

inline void noteLayoutGeometryWrite() {
    if (!countersEnabled()) return;
    g_layoutGeometryWrites.fetch_add(1, std::memory_order_relaxed);
}

inline void noteRenderTargetProbe() {
    if (!countersEnabled()) return;
    g_renderTargetProbes.fetch_add(1, std::memory_order_relaxed);
}

inline void noteRenderTargetFormatReset() {
    if (!countersEnabled()) return;
    g_renderTargetFormatResets.fetch_add(1, std::memory_order_relaxed);
}

/// One control was rasterised into its cache. A render is not a hit: the hit
/// count is `chromePaintCached`, and the two only add up if they are kept apart.
inline void noteChromeCacheRender() {
    if (!countersEnabled()) return;
    g_chromeCacheRenders.fetch_add(1, std::memory_order_relaxed);
}

inline void noteChromeCacheBlit(qint64 micros) {
    if (!countersEnabled()) return;
    g_chromeBlitMicros.fetch_add(static_cast<quint64>(std::max<qint64>(micros, 0)),
                                 std::memory_order_relaxed);
}

inline void noteChromeCacheRaster(qint64 micros) {
    if (!countersEnabled()) return;
    g_chromeRasterMicros.fetch_add(static_cast<quint64>(std::max<qint64>(micros, 0)),
                                   std::memory_order_relaxed);
}

/// Why a control could not be served from cache: 1 geometry, 2 ratio,
/// 3 generation, 4 budget, anything else an explicit invalidation.
inline void noteChromeCacheMiss(int reason) {
    if (!countersEnabled()) return;
    std::atomic<quint64>* counter = &g_chromeCacheMissDirty;
    if (reason == 1) counter = &g_chromeCacheMissGeometry;
    else if (reason == 2) counter = &g_chromeCacheMissRatio;
    else if (reason == 3) counter = &g_chromeCacheMissGeneration;
    else if (reason == 4) counter = &g_chromeCacheMissBudget;
    counter->fetch_add(1, std::memory_order_relaxed);
}

/// That list's occupancy in device pixels after an add, drop or adopt - not the
/// size of the control just rendered, and not a sum over lists. Called only by
/// ChromeList, which is the only place that knows its own total; per-list 8 MiB
/// is the budget, so do not read this as a process-wide ceiling.
inline void noteChromeCacheHeld(qint64 bytes) {
    if (!countersEnabled()) return;
    g_chromeCacheBytes.store(bytes > 0 ? static_cast<quint64>(bytes) : 0,
                             std::memory_order_relaxed);
}

inline void noteAwayStackingStop() {
    if (!countersEnabled()) return;
    g_awayStackingStops.fetch_add(1, std::memory_order_relaxed);
}

inline void noteRaiseRetryAborted() {
    if (!countersEnabled()) return;
    g_raiseRetriesAborted.fetch_add(1, std::memory_order_relaxed);
}

inline void noteTeardownPictureCaptured() {
    if (!countersEnabled()) return;
    g_teardownPicturesCaptured.fetch_add(1, std::memory_order_relaxed);
}

inline void noteBackgroundRender() {
    if (!countersEnabled()) return;
    g_backgroundRenders.fetch_add(1, std::memory_order_relaxed);
}

inline void noteTitleRepolishWidgets(int widgets) {
    if (!countersEnabled()) return;
    if (widgets > 0) {
        g_titleRepolishWidgets.fetch_add(static_cast<quint64>(widgets),
                                        std::memory_order_relaxed);
    }
}

inline void noteThreadInputAttach() {
    if (!countersEnabled()) return;
    g_threadInputAttaches.fetch_add(1, std::memory_order_relaxed);
}

inline void noteChromePaint(quint64 entries, quint64 cached, qint64 micros) {
    if (!countersEnabled()) return;
    g_chromePaintCalls.fetch_add(1, std::memory_order_relaxed);
    g_chromePaintEntries.fetch_add(entries, std::memory_order_relaxed);
    g_chromePaintCached.fetch_add(cached, std::memory_order_relaxed);
    g_chromePaintMicrosTotal.fetch_add(static_cast<quint64>(std::max<qint64>(micros, 0)),
                                       std::memory_order_relaxed);
    g_chromePaintLastMicros.store(micros, std::memory_order_relaxed);
    qint64 current = g_chromePaintMicrosMax.load(std::memory_order_relaxed);
    while (micros > current &&
           !g_chromePaintMicrosMax.compare_exchange_weak(current, micros,
                                                         std::memory_order_relaxed)) {
    }
}

inline void noteActivationToPaint(qint64 ms) {
    if (!countersEnabled()) return;
    g_lastActivationToPaintMs.store(ms, std::memory_order_relaxed);
}

// ── paint cost, by what the pass actually did ─────────────────────────────
//
// GOAL-00 recorded visible-composition and first-swap latency, but never how
// long paintGL itself took, so a "30 % lower p95 chrome repaint" criterion had
// no baseline to be 30 % of. Every classified pass appends its own duration to
// a bounded ring. The GUI thread is the only writer and the only reader, so the
// ring needs no atomics, and reading it costs one copy at the end of a run.
//
// This is observation only: it never reads a pixel and never asks for a paint.
enum PaintPass : int {
    kPaintUnrendered = 0,  // no engine: opening placeholder or the defined black
    kPaintVideoFrame,      // this pass rendered a freshly decoded output frame
    kPaintRetained,        // the held picture was blitted instead of a new frame
    kPaintChromeOnly,      // the engine ran with no new frame and nothing forced
    kPaintPassCount
};

// 2048 keeps a 30 s window at 60 Hz (~1800 passes) inside the ring, which is the
// shortest stretch a p95 can honestly describe here.
inline constexpr int kPaintSamplesPerPass = 2048;

struct PaintPassProfile {
    std::array<qint32, kPaintSamplesPerPass> micros{};
    /// Every pass of this kind, including the ones the ring has since overwritten.
    quint64 samples = 0;
    int next = 0;
};

inline std::array<PaintPassProfile, kPaintPassCount> g_paintPass{};

/// Records one classified pass. Guarded by the caller like every other counter;
/// a negative duration is clamped rather than stored as a bogus sample.
inline void recordPaintPass(int pass, qint64 micros) {
    if (pass < 0 || pass >= kPaintPassCount) return;
    PaintPassProfile& profile = g_paintPass[static_cast<std::size_t>(pass)];
    profile.micros[static_cast<std::size_t>(profile.next)] = static_cast<qint32>(
        std::clamp<qint64>(micros, 0, std::numeric_limits<qint32>::max()));
    profile.next = (profile.next + 1) % kPaintSamplesPerPass;
    ++profile.samples;
}

/// The recorded durations for one pass, oldest first. A snapshot: the ring keeps
/// moving while the caller looks at this.
inline std::vector<qint32> paintPassMicros(int pass) {
    std::vector<qint32> out;
    if (pass < 0 || pass >= kPaintPassCount) return out;
    const PaintPassProfile& profile = g_paintPass[static_cast<std::size_t>(pass)];
    const int stored = static_cast<int>(std::min<quint64>(profile.samples, kPaintSamplesPerPass));
    out.reserve(static_cast<std::size_t>(stored));
    for (int i = 0; i < stored; ++i) {
        const int at = (profile.next - stored + i + kPaintSamplesPerPass) % kPaintSamplesPerPass;
        out.push_back(profile.micros[static_cast<std::size_t>(at)]);
    }
    return out;
}

inline quint64 paintPassSamples(int pass) {
    if (pass < 0 || pass >= kPaintPassCount) return 0;
    return g_paintPass[static_cast<std::size_t>(pass)].samples;
}

/// The same segments as the global phase counters, attributed to the pass class
/// that paid them. The globals answer "what did the surface spend"; this answers
/// "what did a paused chrome-only repaint spend in the engine blit, the retained
/// picture, the chrome overlay and the bookkeeping", which is the only form that
/// can rank a fix. Fixed size, relaxed atomics, written once per classified pass:
/// no allocation and no container growth on the paint path.
inline constexpr int kPaintPhaseCount = 3;

struct PaintPhaseTotals {
    std::array<std::atomic<quint64>, kPaintPhaseCount> micros{};
    std::atomic<quint64> margins{0};
    /// Classified passes this class recorded; the divisor for its segment means.
    std::atomic<quint64> passes{0};
};

inline std::array<PaintPhaseTotals, kPaintPassCount> g_paintPhaseByPass{};

/// Records one phase of one classified pass. The global totals keep exactly the
/// meaning they had; the class totals are the same numbers with the pass that
/// paid them attached. A negative duration is clamped rather than stored, and the
/// per-pass margin is attached to the engine segment the call site passes it
/// with, so one pass still contributes exactly one margin.
inline void notePaintPhase(int pass, int phase, qint64 micros, qint64 marginMicros) {
    if (!countersEnabled()) return;
    const quint64 value = static_cast<quint64>(std::max<qint64>(micros, 0));
    std::atomic<quint64>* global = &g_paintPhaseOverlayMicros;
    if (phase == 0) global = &g_paintPhaseEngineMicros;
    else if (phase == 1) global = &g_paintPhaseRetentionMicros;
    global->fetch_add(value, std::memory_order_relaxed);
    g_paintPhaseMargins.fetch_add(static_cast<quint64>(std::max<qint64>(marginMicros, 0)),
                                  std::memory_order_relaxed);
    if (pass < 0 || pass >= kPaintPassCount) return;
    PaintPhaseTotals& totals = g_paintPhaseByPass[static_cast<std::size_t>(pass)];
    if (phase == 0) {
        totals.passes.fetch_add(1, std::memory_order_relaxed);
        totals.margins.fetch_add(static_cast<quint64>(std::max<qint64>(marginMicros, 0)),
                                 std::memory_order_relaxed);
    }
    const int slot = phase == 0 ? 0 : (phase == 1 ? 1 : 2);
    totals.micros[static_cast<std::size_t>(slot)].fetch_add(value, std::memory_order_relaxed);
}

inline void resetPaintPassProfile() {
    for (PaintPassProfile& profile : g_paintPass) {
        profile = PaintPassProfile{};
    }
}

/// One pass class's share of the phase counters. `passes` is the number of
/// classified passes of that class that contributed, so a per-pass mean is only
/// ever computed from a non-zero divisor - and a class that never painted reads
/// as absent rather than as a fast zero.
struct PaintPhaseClassSummary {
    quint64 passes = 0;
    quint64 engineMicros = 0;
    quint64 retentionMicros = 0;
    quint64 overlayMicros = 0;
    quint64 marginsMicros = 0;
};

/// Counters a probe reports. -1 means the value was never measured in this run.
struct Snapshot {
    quint64 chromeDirtyRequests = 0;
    quint64 chromeDirtyApplied = 0;
    quint64 setMaskApplications = 0;
    quint64 layoutGeometryWrites = 0;
    quint64 threadInputAttaches = 0;
    quint64 chromePaintCalls = 0;
    quint64 chromePaintEntries = 0;
    quint64 chromePaintCached = 0;
    quint64 chromePaintMicrosTotal = 0;
    quint64 renderTargetProbes = 0;
    quint64 renderTargetFormatResets = 0;
    quint64 chromeCacheRenders = 0;
    quint64 chromeCacheBytes = 0;
    quint64 chromeBlitMicros = 0;
    quint64 chromeRasterMicros = 0;
    quint64 chromeCacheMissDirty = 0;
    quint64 chromeCacheMissGeometry = 0;
    quint64 chromeCacheMissRatio = 0;
    quint64 chromeCacheMissGeneration = 0;
    quint64 chromeCacheMissBudget = 0;
    quint64 paintPhaseEngineMicros = 0;
    quint64 paintPhaseRetentionMicros = 0;
    quint64 paintPhaseOverlayMicros = 0;
    quint64 paintPhaseMargins = 0;
    /// The same phases split by the pass class that paid them, indexed by
    /// PaintPass. The sum over the classes equals the global fields above.
    std::array<PaintPhaseClassSummary, kPaintPassCount> paintPhaseByPass{};
    quint64 awayStackingStops = 0;
    quint64 raiseRetriesAborted = 0;
    quint64 teardownPicturesCaptured = 0;
    quint64 backgroundRenders = 0;
    quint64 titleRepolishWidgets = 0;
    qint64 chromePaintMicrosMax = -1;
    qint64 chromePaintLastMicros = -1;
    qint64 lastActivationToPaintMs = -1;
};

inline Snapshot snapshot() {
    Snapshot out;
    if (!countersEnabled()) return out;
    out.chromeDirtyRequests = g_chromeDirtyRequests.load(std::memory_order_relaxed);
    out.chromeDirtyApplied = g_chromeDirtyApplied.load(std::memory_order_relaxed);
    out.setMaskApplications = g_setMaskApplications.load(std::memory_order_relaxed);
    out.layoutGeometryWrites = g_layoutGeometryWrites.load(std::memory_order_relaxed);
    out.threadInputAttaches = g_threadInputAttaches.load(std::memory_order_relaxed);
    out.chromePaintCalls = g_chromePaintCalls.load(std::memory_order_relaxed);
    out.chromePaintEntries = g_chromePaintEntries.load(std::memory_order_relaxed);
    out.chromePaintCached = g_chromePaintCached.load(std::memory_order_relaxed);
    out.chromePaintMicrosTotal = g_chromePaintMicrosTotal.load(std::memory_order_relaxed);
    out.renderTargetProbes = g_renderTargetProbes.load(std::memory_order_relaxed);
    out.renderTargetFormatResets = g_renderTargetFormatResets.load(std::memory_order_relaxed);
    out.chromeCacheRenders = g_chromeCacheRenders.load(std::memory_order_relaxed);
    out.chromeCacheBytes = g_chromeCacheBytes.load(std::memory_order_relaxed);
    out.chromeBlitMicros = g_chromeBlitMicros.load(std::memory_order_relaxed);
    out.chromeRasterMicros = g_chromeRasterMicros.load(std::memory_order_relaxed);
    out.chromeCacheMissDirty = g_chromeCacheMissDirty.load(std::memory_order_relaxed);
    out.chromeCacheMissGeometry = g_chromeCacheMissGeometry.load(std::memory_order_relaxed);
    out.chromeCacheMissRatio = g_chromeCacheMissRatio.load(std::memory_order_relaxed);
    out.chromeCacheMissGeneration = g_chromeCacheMissGeneration.load(std::memory_order_relaxed);
    out.chromeCacheMissBudget = g_chromeCacheMissBudget.load(std::memory_order_relaxed);
    out.paintPhaseEngineMicros = g_paintPhaseEngineMicros.load(std::memory_order_relaxed);
    out.paintPhaseRetentionMicros = g_paintPhaseRetentionMicros.load(std::memory_order_relaxed);
    out.paintPhaseOverlayMicros = g_paintPhaseOverlayMicros.load(std::memory_order_relaxed);
    out.paintPhaseMargins = g_paintPhaseMargins.load(std::memory_order_relaxed);
    for (int pass = 0; pass < kPaintPassCount; ++pass) {
        const PaintPhaseTotals& totals = g_paintPhaseByPass[static_cast<std::size_t>(pass)];
        PaintPhaseClassSummary& summary = out.paintPhaseByPass[static_cast<std::size_t>(pass)];
        summary.passes = totals.passes.load(std::memory_order_relaxed);
        summary.engineMicros = totals.micros[0].load(std::memory_order_relaxed);
        summary.retentionMicros = totals.micros[1].load(std::memory_order_relaxed);
        summary.overlayMicros = totals.micros[2].load(std::memory_order_relaxed);
        summary.marginsMicros = totals.margins.load(std::memory_order_relaxed);
    }
    out.awayStackingStops = g_awayStackingStops.load(std::memory_order_relaxed);
    out.raiseRetriesAborted = g_raiseRetriesAborted.load(std::memory_order_relaxed);
    out.teardownPicturesCaptured = g_teardownPicturesCaptured.load(std::memory_order_relaxed);
    out.backgroundRenders = g_backgroundRenders.load(std::memory_order_relaxed);
    out.titleRepolishWidgets = g_titleRepolishWidgets.load(std::memory_order_relaxed);
    out.chromePaintMicrosMax = g_chromePaintMicrosMax.load(std::memory_order_relaxed);
    out.chromePaintLastMicros = g_chromePaintLastMicros.load(std::memory_order_relaxed);
    out.lastActivationToPaintMs = g_lastActivationToPaintMs.load(std::memory_order_relaxed);
    return out;
}

inline void resetCounters() {
    g_chromeDirtyRequests.store(0, std::memory_order_relaxed);
    g_chromeDirtyApplied.store(0, std::memory_order_relaxed);
    g_setMaskApplications.store(0, std::memory_order_relaxed);
    g_layoutGeometryWrites.store(0, std::memory_order_relaxed);
    g_threadInputAttaches.store(0, std::memory_order_relaxed);
    g_chromePaintCalls.store(0, std::memory_order_relaxed);
    g_chromePaintEntries.store(0, std::memory_order_relaxed);
    g_chromePaintCached.store(0, std::memory_order_relaxed);
    g_chromePaintMicrosTotal.store(0, std::memory_order_relaxed);
    g_renderTargetProbes.store(0, std::memory_order_relaxed);
    g_renderTargetFormatResets.store(0, std::memory_order_relaxed);
    g_chromeCacheRenders.store(0, std::memory_order_relaxed);
    g_chromeCacheBytes.store(0, std::memory_order_relaxed);
    g_chromeBlitMicros.store(0, std::memory_order_relaxed);
    g_chromeRasterMicros.store(0, std::memory_order_relaxed);
    g_chromeCacheMissDirty.store(0, std::memory_order_relaxed);
    g_chromeCacheMissGeometry.store(0, std::memory_order_relaxed);
    g_chromeCacheMissRatio.store(0, std::memory_order_relaxed);
    g_chromeCacheMissGeneration.store(0, std::memory_order_relaxed);
    g_chromeCacheMissBudget.store(0, std::memory_order_relaxed);
    g_paintPhaseEngineMicros.store(0, std::memory_order_relaxed);
    g_paintPhaseRetentionMicros.store(0, std::memory_order_relaxed);
    g_paintPhaseOverlayMicros.store(0, std::memory_order_relaxed);
    g_paintPhaseMargins.store(0, std::memory_order_relaxed);
    for (PaintPhaseTotals& totals : g_paintPhaseByPass) {
        for (std::atomic<quint64>& micros : totals.micros) {
            micros.store(0, std::memory_order_relaxed);
        }
        totals.margins.store(0, std::memory_order_relaxed);
        totals.passes.store(0, std::memory_order_relaxed);
    }
    g_awayStackingStops.store(0, std::memory_order_relaxed);
    g_raiseRetriesAborted.store(0, std::memory_order_relaxed);
    g_teardownPicturesCaptured.store(0, std::memory_order_relaxed);
    g_backgroundRenders.store(0, std::memory_order_relaxed);
    g_titleRepolishWidgets.store(0, std::memory_order_relaxed);
    g_chromePaintMicrosMax.store(-1, std::memory_order_relaxed);
    g_chromePaintLastMicros.store(-1, std::memory_order_relaxed);
    g_lastActivationToPaintMs.store(-1, std::memory_order_relaxed);
}

} // namespace mikai::gui::uiprofile
