// Presentation-path profiling, gated by MIKAI_UI_PROFILE=1.
//
// Logging only. Call sites guard string construction with enabled(); counters
// are process-wide because the surface, shell and raise policy all contribute.
// Output is `[ui-profile] <scope> key=value ...` on stderr.

#pragma once

#include <QByteArray>
#include <QString>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

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
inline std::atomic<qint64> g_chromePaintLastMicros{-1};
inline std::atomic<quint64> g_chromePaintMicrosTotal{0};
inline std::atomic<qint64> g_chromePaintMicrosMax{-1};
inline std::atomic<qint64> g_lastActivationToPaintMs{-1};

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

inline void noteThreadInputAttach() {
    if (!countersEnabled()) return;
    g_threadInputAttaches.fetch_add(1, std::memory_order_relaxed);
}

inline void noteChromePaint(quint64 entries, qint64 micros) {
    if (!countersEnabled()) return;
    g_chromePaintCalls.fetch_add(1, std::memory_order_relaxed);
    g_chromePaintEntries.fetch_add(entries, std::memory_order_relaxed);
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

/// Counters a probe reports. -1 means the value was never measured in this run.
struct Snapshot {
    quint64 chromeDirtyRequests = 0;
    quint64 chromeDirtyApplied = 0;
    quint64 setMaskApplications = 0;
    quint64 layoutGeometryWrites = 0;
    quint64 threadInputAttaches = 0;
    quint64 chromePaintCalls = 0;
    quint64 chromePaintEntries = 0;
    quint64 chromePaintMicrosTotal = 0;
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
    out.chromePaintMicrosTotal = g_chromePaintMicrosTotal.load(std::memory_order_relaxed);
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
    g_chromePaintMicrosTotal.store(0, std::memory_order_relaxed);
    g_chromePaintMicrosMax.store(-1, std::memory_order_relaxed);
    g_chromePaintLastMicros.store(-1, std::memory_order_relaxed);
    g_lastActivationToPaintMs.store(-1, std::memory_order_relaxed);
}

} // namespace mikai::gui::uiprofile
