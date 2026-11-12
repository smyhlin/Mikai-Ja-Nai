// Shared OpenGL presentation implementation for every player view.
//
// The bridge owns the MpvRenderContext, ONE stable overlay-free output
// framebuffer/texture, the transition hold and the teardown readback, the
// context-creation/destruction order and every presentation acknowledgement.
// The Widgets adapter blits the stable output into its backing FBO; the native
// Quick item wraps the output texture directly. Neither copies decoded frames.
//
// Threading: every GL method and every mutable field belongs to the rendering
// (context-owning) thread: the GUI thread for a QOpenGLWidget adapter, the
// scene-graph render thread for the native Quick item. The ONLY exceptions are
// the atomic mpv-wakeup relay, the atomic cached facts/counters below, and
// `setDestroying`. A GUI-thread presenter queues commands; it publishes cached
// facts and profile copies instead of mutating this object.
//
// An OpenGL widget driving the basic (single-threaded) render loop simply runs
// on the GUI thread, which then is the rendering thread.

#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QtGlobal>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>

#include "MpvRenderContext.hpp"
#include "VideoPresenter.hpp"
#include "RenderDiagnostics.hpp"

class QOpenGLFramebufferObject;

namespace mplayer {

class MpvHandle;

/// Process-wide frame-timing log path. Empty disables logging. Declared here so
/// the renderer and the GUI writer share one storage; VideoSession's static
/// setter forwards to it. Render-thread lines are marshalled to the GUI writer.
void setRenderFrameTimingLogPath(const QString& path);
QString renderFrameTimingLogPath();
/// Appends one line to the process-wide timing log; a no-op when the path is
/// empty. Thread-safe: render-thread lines and the GUI writer share one
/// buffered writer. Overlong input is dropped rather than truncated.
void appendRenderFrameTimingLog(const QString& line);

/// mpv's client pacing delay for a frame notification, in milliseconds.
/// API 2.2+ reports target_time in nanoseconds; API 2.1 in microseconds.
inline std::optional<int> clientPacingDelayMilliseconds(
    int64_t rawTargetTime, int64_t nowUs, double renderLeadMs, uint64_t apiVersion) {
    if (rawTargetTime <= 0 || !std::isfinite(renderLeadMs) || renderLeadMs < 0.0)
        return std::nullopt;
    const uint64_t major = apiVersion >> 16;
    const uint64_t minor = apiVersion & 0xffffu;
    const bool targetInNanoseconds = major > 2 || (major == 2 && minor >= 2);
    const int64_t targetUs = targetInNanoseconds ? rawTargetTime / 1000 : rawTargetTime;
    if (targetUs <= nowUs ||
        (nowUs < 0 && targetUs > std::numeric_limits<int64_t>::max() + nowUs))
        return std::nullopt;
    const int64_t remainingUs =
        targetUs - nowUs - static_cast<int64_t>(renderLeadMs * 1000.0);
    if (remainingUs <= 0 || remainingUs > 100000) return std::nullopt;
    return static_cast<int>((remainingUs + 999) / 1000);
}

/// Common OpenGL renderer. One instance per view; never shared between views.
class VideoRenderBridge {
public:
    /// Marshals a callable onto the bridge's owning GUI thread. Required when
    /// the bridge runs on a threaded scene graph; may be empty when the
    /// rendering thread already is the GUI thread.
    using GuiInvoker = std::function<void(std::function<void()>)>;

    /// Constructs an idle bridge. No GL object is created before
    /// createRenderContext()/ensureOutput().
    explicit VideoRenderBridge(GuiInvoker toGuiThread = {});
    ~VideoRenderBridge();

    VideoRenderBridge(const VideoRenderBridge&) = delete;
    VideoRenderBridge& operator=(const VideoRenderBridge&) = delete;

    /// [before first use] Installs the GUI marshaller.
    void setGuiInvoker(GuiInvoker toGuiThread);
    /// [GUI-safe, atomic] Set from the session destructor path: a teardown
    /// picture read back for a view that is going away is skipped.
    void setDestroying(bool destroying) { destroying_.store(destroying); }

    // ── context lifecycle ─────────────────────────────────────────────────
    // Called on the thread that owns the GL context, with the render thread
    // quiescent. The adapter guarantees that the SG/context lifecycle performs
    // this cleanup with the ORIGINAL context current.
    //
    /// Creates MpvRenderContext against `core`, with the owning GL context
    /// current. `core` is held as a shared lease for as long as a render
    /// context exists, so the core cannot die before the render context is
    /// freed even when the session is destroyed first.
    ///
    /// `owner` guards `onUpdate`: mpv raises it on its own threads, it is never
    /// invoked after `owner` expired, and it must only schedule work - no GL,
    /// no session access. The bridge coalesces it internally.
    /// Throws MpvError when mpv refuses; returns false with `reason` when no
    /// context is current.
    bool createRenderContext(std::shared_ptr<MpvHandle> core,
                             std::weak_ptr<const void> owner,
                             std::function<void()> onUpdate,
                             QString* reason = nullptr);
    /// The original GL context is being destroyed
    /// (`QOpenGLContext::aboutToBeDestroyed`, direct connection): capture the
    /// teardown picture and free the render context while that context is still
    /// current. The core lease is KEPT so a recreated context can attach again.
    /// True when the render context was freed; false only if no context is
    /// current, in which case the render wrapper AND the core lease are BOTH
    /// retained (`releasePending()`).
    bool contextAboutToBeDestroyed();
    /// Frees MpvRenderContext with its creating context current, then drops the
    /// core lease - in that order. True on success (or when nothing was held).
    ///
    /// PROGRAMMING INVARIANT: this must succeed. If no creating context can be
    /// made current the render wrapper and the core lease are BOTH retained,
    /// `releasePending()` stays true and a critical diagnostic is logged; the
    /// core is never dropped under a live render context. The adapter must
    /// retry from its SG/context teardown with the original context current.
    bool releaseRenderContext();
    /// [GUI-safe, atomic] A failed release left the wrapper and core lease held.
    bool releasePending() const { return releasePending_.load(); }
    /// [GUI-safe, atomic] A render context exists for the current GL context.
    bool contextReady() const { return contextReady_.load(); }
    /// [GUI-safe, atomic] Render contexts created (monotonic).
    quint64 contextGeneration() const { return generation_.load(); }
    /// [rendering thread ONLY] The core this bridge keeps alive. `core_` is a
    /// plain shared_ptr mutated by create/release on that thread, so this MUST
    /// NOT be called from the GUI thread; the GUI-held lease is
    /// VideoSession::coreLease() on the session thread.
    std::shared_ptr<MpvHandle> coreLease() const { return core_; }

    // ── stable output ─────────────────────────────────────────────────────
    // The ONE overlay-free GPU output every consumer shares. mpv renders
    // directly into it; holds replace its content only during a transition.
    /// [rendering thread] Creates/resizes the output FBO/texture for `facts`.
    /// A size or format change reallocates it once, never per frame.
    bool ensureOutput(const RenderTargetFacts& facts);
    /// [rendering thread] The output texture a Quick scene-graph node wraps.
    unsigned int outputTextureId() const;
    /// [rendering thread] The output framebuffer name mpv renders into.
    int outputFbo() const;
    /// [rendering thread] Current output size in device pixels.
    QSize outputSize() const;
    /// [rendering thread] Sized internal format of the output attachment.
    int outputInternalFormat() const;

    // ── render pass ───────────────────────────────────────────────────────
    // Called on the rendering thread with the owning context current.
    /// Advances mpv's render state and renders into the stable output when a
    /// frame is owed. This is the ONLY entry point a Quick item needs; its
    /// scene-graph node then wraps outputTextureId() and reports presentation
    /// through reportPresentedFrame(). Handles the hold, pacing classification
    /// and queued readbacks.
    RenderResult renderToOutput(const RenderSnapshot& snapshot);
    /// Widget adapter: renderToOutput() then blitOutput() into its backing FBO.
    RenderResult render(const RenderSnapshot& snapshot, int targetFbo, int targetWidth,
                        int targetHeight);
    /// Widget adapter: draws the stable output into `targetFbo` (1:1, no CPU
    /// copy). A UI-only repaint calls only this.
    void blitOutput(int targetFbo, int targetWidth, int targetHeight);
    /// Advances mpv's render state without painting: update(), then either a
    /// pending frame kept for the next paint or a skip acknowledged at once
    /// when nothing can composite. Exactly one answer per mpv callback.
    void advanceRenderState(const RenderSnapshot& snapshot, bool canPresent);
    /// Queued by the presenter when the chain changed under a possibly paused
    /// core: forceRender_ = true + advanceRenderState once.
    void refresh(const RenderSnapshot& snapshot, bool canPresent);
    bool hasPendingFrame() const { return framePending_; }
    bool hasForceRender() const { return forceRender_; }
    /// The frame this bridge painted will never be composited: send the owed
    /// presentation report as the drop it is.
    void acknowledgeDroppedFrame();

    // ── frame pacing ──────────────────────────────────────────────────────
    /// What a paint may take from the state it found.
    static PaintScheduleDecision decidePaintSchedule(bool pacedRelease, bool forced,
                                                     bool armedSlot, bool frameOwed,
                                                     bool holdOwesOutput);
    /// Arms (or drops) the presentation slot for the pending frame. Returns the
    /// delay in ms before the paint should run, or -1 when no paint is owed.
    /// A forced repaint returns 0.
    int schedulePacedPaint(bool canPresent, int64_t targetTimeUs, int64_t nowUs,
                           uint64_t apiVersion);
    /// True when the paint about to run was released by the pacing slot; the
    /// flag is consumed by that paint.
    bool pacedPaintReleased() const { return pacedPaintReleased_; }
    void notePacedPaintStarted();
    /// Drops any armed slot and its measurement window.
    void stopPacing();

    // ── retained pictures ─────────────────────────────────────────────────
    // GPU-only, allocated/copied ONLY at a transition; ordinary playback
    // retains no extra video framebuffer.
    /// Capture a video-only snapshot of the output and keep it up. Idempotent.
    void setEngineFrameHold(bool active);
    /// Capture + arm the landing gate for a source change.
    void retainAcrossLoad();
    /// Arm the landing gate for `sourceEpoch` BEFORE the load is submitted.
    void armLoadLanding(uint64_t sourceEpoch);
    /// FILE_LOADED for this session's own load request: a real frame may now
    /// release the cross-load hold.
    void noteLoadLanded(uint64_t sourceEpoch);
    /// The load was refused: drop the gate without waiting for a frame.
    void noteLoadRefused(uint64_t sourceEpoch);
    /// Drop the hold, the output content marker and the teardown readback.
    void invalidateRetainedPictures();
    /// Drop the hold but keep the teardown readback a pending reload shows.
    void clearRetainedPictureKeepingTeardown();
    bool hasVisibleOutputFrame() const { return hasVisibleOutputFrame_; }
    bool hasRetainedPicture() const;
    /// Bytes of off-screen pictures retained right now, at their own precision.
    qint64 retainedPictureBytes() const;

    // ── target format ─────────────────────────────────────────────────────
    /// Queries `targetFbo`'s color attachment precision with it bound. Cached
    /// until resetTargetFormat().
    void probeTargetFormat(int targetFbo);
    /// Forgets the probed format after a context or framebuffer change.
    void resetTargetFormat();
    RenderTargetColorBits targetColorBits() const { return renderTargetColorBits_; }
    int targetInternalFormat() const { return renderInternalFormat_; }
    int targetDepthBitsPerChannel() const { return renderDepthBitsPerChannel_; }
    bool targetFormatProbed() const { return renderFormatProbed_; }

    // ── on-demand readback ────────────────────────────────────────────────
    /// Video-only readback of the stable output (never chrome). Null image with
    /// `reason` set when no output exists. The owning context must be current.
    QImage readVideoFrame(QString* reason);
    RenderDiagnosticsResult captureDiagnostics(const RenderDiagnosticsRequest& request,
                                                const RenderSnapshot& snapshot);
    void captureDiagnosticsAsync(const RenderDiagnosticsRequest& request,
                                 const RenderSnapshot& snapshot,
                                 std::function<void(RenderDiagnosticsResult)> completion);
    void postDiagnosticsResult(RenderDiagnosticsResult result,
                               std::function<void(RenderDiagnosticsResult)> completion);
    /// Queues ONE readback (an API level, not an unbounded queue) delivered on
    /// the next successful render pass, on the GUI thread via the invoker.
    /// A second call while one is queued completes immediately with a reason.
    /// The presenter requests a repaint after queueing.
    void readVideoFrameAsync(std::function<void(QImage, QString)> completion);
    bool readbackQueued() const { return pendingReadback_ != nullptr; }

    // ── presentation acknowledgement ──────────────────────────────────────
    /// Tells mpv the frame it handed out reached the screen. Consumes the owed
    /// report for the last painted frame, once.
    void reportPresentedFrame();

    /// [GUI-safe, lock-free] Published by the rendering thread; a GUI probe
    /// reads the cached copy without touching mutable render state.
    UiProfileCounters uiProfileCounters() const;

private:
    bool captureEngineFrameHold();
    bool captureTeardownPicture();
    bool drawRetainedPicture(unsigned int target, int width, int height);
    bool drawEngineFrameHold(unsigned int target, int width, int height);
    bool paintTeardownPicture();
    void invalidatePresentedOutput();
    void deliverReadback(const QImage& image, const QString& reason);
    void publishProfilePaint(const RenderResult& result, qint64 paintStartNs,
                             qint64 engineMicros, qint64 retentionMicros);
    void clearPacingProfile();
    void logFrameTiming(const QString& line);

    GuiInvoker toGuiThread_;
    /// Held from createRenderContext() until a successful release. While a
    /// render wrapper exists this lease MUST be non-empty.
    std::shared_ptr<MpvHandle> core_;
    /// The receiver of `onUpdate`; invocation stops when it expires.
    std::weak_ptr<const void> updateOwner_;
    std::unique_ptr<MpvRenderContext> render_;

    std::atomic<quint64> generation_{0};
    std::atomic<bool> contextReady_{false};
    std::atomic<bool> releasePending_{false};
    std::atomic<bool> destroying_{false};
    int flipY_ = 1;

    /// The generation of the hop already queued: every update callback mpv
    /// raises before it runs collapses into one. Written from mpv's threads.
    std::atomic<quint64> queuedUpdate_{0};

    // ── stable output ─────────────────────────────────────────────────────
    std::unique_ptr<QOpenGLFramebufferObject> outputFbo_;
    QSize outputSize_;
    int outputInternalFormat_ = 0;
    int outputDepthBitsPerChannel_ = 0;
    /// Set when the output holds a real output frame of the current source.
    bool outputHasFrame_ = false;

    bool framePending_ = false;
    bool forceRender_ = true;
    bool hasRenderedFrame_ = false;
    bool hasVisibleOutputFrame_ = false;
    /// A frame really rendered that the compositor has not acknowledged yet, so
    /// the one presentation report it is owed goes out once.
    bool videoFramePainted_ = false;
    quint64 presentationSerial_ = 0;

    // ── transition hold (allocated/copied only at a transition) ───────────
    bool engineFrameHoldActive_ = false;
    bool engineFrameHoldAwaitingOutput_ = false;
    /// A hold taken across a source change waits for FILE_LOADED for this
    /// session's load request before a real frame may release it.
    bool engineFrameHoldNeedsLoadLanding_ = false;
    bool engineFrameHoldLandingSeen_ = false;
    uint64_t engineFrameHoldEpoch_ = 0;
    std::unique_ptr<QOpenGLFramebufferObject> engineFrameHoldFbo_;

    /// The picture read back before a context was released, at the precision it
    /// was read at. One copy per context, never per paint.
    QImage teardownPicture_;
    int teardownPictureFormat_ = 0;
    /// One error report per streak of failed renders, never one per paint.
    bool renderFailureStreak_ = false;

    int renderInternalFormat_ = 0;
    RenderTargetColorBits renderTargetColorBits_;
    int renderDepthBitsPerChannel_ = 0;
    bool renderFormatProbed_ = false;

    /// Consecutive advanceRenderState rounds that could not paint. Skip/drop
    /// mode is latched only after this exceeds the grace bound.
    int unpaintableStreak_ = 0;
    /// The return from skip mode forces one redraw so no frame is stranded.
    bool skippingFrames_ = false;

    // ── pacing ────────────────────────────────────────────────────────────
    /// Raw target_time in the loaded API's units; 0 with no slot armed, and
    /// also for a grace retry, which deliberately has no mpv slot behind it.
    qint64 pacingTargetTime_ = 0;
    bool pacingTimerArmed_ = false;
    /// The armed run is the bounded unpaintable-grace retry, not a presentation
    /// slot: no target_time, no lead measurement.
    bool pacingRetryOnly_ = false;
    double renderLeadMs_ = 1.5;
    qint64 pacingFiredAtNs_ = 0;
    bool pacingMeasurePending_ = false;
    /// Set by notePacedPaintStarted() and consumed by the paint it released.
    bool pacedPaintReleased_ = false;

    // ── MIKAI_UI_PROFILE, published atomically for GUI probes ─────────────
    std::atomic<quint64> profileInitializeGlCalls_{0};
    std::atomic<quint64> profilePaintCalls_{0};
    std::atomic<quint64> profilePacingDisarms_{0};
    std::atomic<qint64> profileLastPaintDelayMs_{-1};
    std::atomic<qint64> profileLastPaintEntryNs_{0};
    qint64 profilePacingArmedAtNs_ = 0;
    bool profilePacingAwaitingPaint_ = false;
    /// Fixed literal, never built per frame.
    const char* profilePacingReason_ = "none";
    bool profilePacingFired_ = false;

    /// At most one readback at a time: the API level is one, so no queue.
    std::function<void(QImage, QString)> pendingReadback_;
};

}  // namespace mplayer
