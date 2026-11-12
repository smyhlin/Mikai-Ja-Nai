// Presentation host contract for the playback session.
//
// The session owns control and observed state on its QObject thread and never
// touches GL. A presenter owns every GL object, decides when a context is
// current, and marshals render work onto its own render lifecycle (the GUI
// thread for a QOpenGLWidget adapter, the scene-graph render thread for the
// native Quick item). Calls marked [GUI] are made on the session's thread and
// must be safe from any state path; calls marked [GL] are queued by the
// implementation and run with the owning context current.
//
// Nothing here exposes textures, backends or interop: only the facts and the
// operations the current widget player already performs.

#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QtGlobal>

#include <cstdint>
#include <functional>
#include <memory>

namespace mplayer {

class MpvHandle;

/// Actual color attachment precision of the render target, as reported by the
/// GL context that owns it. Only meaningful once `renderTargetFormatProbed`.
struct RenderTargetColorBits {
    int red = 0;
    int green = 0;
    int blue = 0;
};

/// Everything a render pass needs to know about the surface it draws into.
/// Captured by the presenter on its own thread; immutable afterwards.
struct RenderTargetFacts {
    /// Target size in device pixels, never negative and never zero-scaled.
    QSize devicePixelSize;
    qreal devicePixelRatio = 1.0;
    /// Sized GL internal format probed from the target, 0 before the probe.
    int colorInternalFormat = 0;
    RenderTargetColorBits colorBits;
    /// Zero lets mpv assume its documented 8-bit default.
    int depthBitsPerChannel = 0;
    /// A render context exists for the current GL context.
    bool renderContextReady = false;
    /// The target can composite a frame now (visible, exposed, not minimized).
    bool presentable = false;
};

/// Immutable per-pass state prepared on the session's thread. The renderer
/// reads this and MUST NOT touch a mutable VideoSession while it renders.
/// A threaded scene graph captures it on the session thread (for example when
/// the adapter requests an update) and only copies the cached struct in
/// synchronize(); it never calls back into the session from the render thread.
struct RenderSnapshot {
    RenderTargetFacts target;
    uint64_t sourceEpoch = 0;
    /// Opaque identity of the load this epoch belongs to; 0 when unnamed.
    quint64 mediaToken = 0;
    /// A video track exists for the loaded source.
    bool videoAvailable = false;
    /// A source is loaded and no frame of it has been presented yet.
    bool opening = false;
    /// The viewer's pause intent, not mpv's observed value.
    bool paused = false;
    /// The session wants one clean picture retained through a chain change.
    bool engineFrameHoldRequested = false;
    /// Playhead the session reports and whether it is meaningful. GUI-captured
    /// with the rest of this snapshot; the presenter uses it to supply the
    /// context-loss reload capture (`reloadPositionCaptured`). It is never a
    /// property read from, or a call into, the session on the render thread.
    double position = 0.0;
    bool positionValid = false;
};

/// Result of one presentation pass into the shared stable output. `pass` names
/// what the output now holds.
struct RenderResult {
    enum class Pass : int {
        None = 0,
        VideoFrame,
        HeldPicture,
        OutputReuse,
        TeardownPicture,
        Failed
    };

    Pass pass = Pass::None;
    /// mpv's render return code; 0 on success, negative on failure.
    int renderError = 0;
    /// The stable output received a new frame of the current source this pass.
    bool outputUpdated = false;
    /// This pass showed the retained transition picture.
    bool presentedHeldPicture = false;
    /// This pass reused the output that was already there (no re-render).
    bool reusedOutput = false;
    /// This pass consumed the frame the session/presenter was holding for it.
    bool frameConsumed = false;
    /// A frame of the loaded source really reached the target (not the hold).
    bool freshOutputFrame = false;
    /// The retained hold was released by this pass.
    bool holdReleased = false;
    /// Monotonic serial of this presentation (0 when nothing was presented).
    /// The presenter echoes it in the PresentationReport it pushes.
    quint64 presentationSerial = 0;
    /// Width/height actually rendered into, in device pixels.
    int targetWidth = 0;
    int targetHeight = 0;
};

/// What a paint pass may take from the state it found. A paint that presents no
/// new frame must neither consume the frame a pacing slot is holding nor stop
/// that slot: doing either is the burst-then-stall the pacing timer exists for.
struct PaintScheduleDecision {
    /// The frame (or the forced redraw) this pass presents.
    bool consumesFrame = false;
    /// The armed pacing slot survives this pass and presents that frame.
    bool keepsArmedSlot = false;
};

/// One composited presentation, pushed from the presenter to the session. A
/// presenter marshals it onto the session's thread before invoking the hook.
struct PresentationReport {
    /// Monotonic presentation serial.
    quint64 serial = 0;
    /// Source epoch the presented frame belonged to.
    uint64_t sourceEpoch = 0;
    /// A frame of the loaded source (not a hold) reached the target.
    bool freshOutputFrame = false;
    /// The transition picture stood in for the frame.
    bool heldPicture = false;
    /// The pass re-blitted the previous picture.
    bool reusedSnapshot = false;
};

/// Presentation-path counters a probe reads after a run (MIKAI_UI_PROFILE
/// gated). They are observations, never inputs: nothing in the player changes
/// behaviour because they were read.
struct UiProfileCounters {
    quint64 initializeGlCalls = 0;
    /// Render contexts created (monotonic), so a change across an activation
    /// means the context was recreated rather than painted.
    quint64 contextGeneration = 0;
    quint64 paintCalls = 0;
    /// Paints that stopped an armed pacing timer while no frame was pending.
    quint64 pacingDisarms = 0;
    /// schedulePacedPaint() -> paint delay of the latest paced paint; -1 when
    /// the latest paint was not released by the pacing timer.
    qint64 lastPaintDelayMs = -1;
    /// Steady-clock ns of the latest paint entry; 0 when counters are off.
    qint64 lastPaintEntryNs = 0;
};

/// Implemented by the Widgets adapter today and by the native Quick render
/// item later. The implementation owns ALL GL objects and the context
/// lifecycle; it never issues an mpv command.
class VideoPresenter {
public:
    virtual ~VideoPresenter() = default;

    // ── target facts [GUI] ───────────────────────────────────────────────
    /// Current target size/DPR/format/presentability. Cheap and non-blocking.
    virtual RenderTargetFacts targetFacts() const = 0;
    /// Whether Qt could composite a pass right now (visible, exposed, not
    /// minimized). Independently of whether a frame is owed.
    virtual bool canPresentNow() const = 0;
    /// QWidget::update() / QQuickItem::update(); coalescing allowed.
    virtual void requestRepaint() = 0;

    // ── retained pictures, GPU only, no per-frame readback [GL] ──────────
    /// Capture a video-only snapshot of the current output and keep it on
    /// screen. Idempotent: a picture already held is not recaptured.
    virtual void acquireRetainedPicture() = 0;
    /// Mark the held picture as replaceable by the next real output frame.
    /// The GPU storage is released when that frame is presented.
    virtual void releaseRetainedPicture() = 0;
    /// Capture + arm the landing gate for a source change: the held picture
    /// stands in until a frame of the *replacement* source is presented.
    virtual void retainAcrossLoad() = 0;
    /// Arm the landing gate for `sourceEpoch`; MUST be called before the load
    /// command is submitted.
    virtual void armLoadLanding(uint64_t sourceEpoch) = 0;
    /// The load `sourceEpoch` reported FILE_LOADED for this session's own
    /// request: a real frame may now release the cross-load hold.
    virtual void noteLoadLanded(uint64_t sourceEpoch) = 0;
    /// The load `sourceEpoch` was refused: the gate is dropped without ever
    /// waiting for a frame that will not come.
    virtual void noteLoadRefused(uint64_t sourceEpoch) = 0;
    /// Drop every retained picture and the presented snapshot. Used at a
    /// source boundary, an intentional clear and a close.
    virtual void invalidateRetainedPictures() = 0;
    /// True only after a successfully rendered output frame, never for a
    /// forced redraw of an empty renderer.
    virtual bool hasVisibleOutputFrame() const = 0;
    virtual bool hasRetainedPicture() const = 0;
    /// Bytes of off-screen pictures retained right now. Diagnostics only.
    virtual qint64 retainedPictureBytes() const = 0;
    /// Force one render-state advance and repaint of the current picture, for
    /// a chain swap under a possibly paused core.
    virtual void refreshRenderState() = 0;

    // ── on-demand CPU readback only ───────────────────────────────────────
    /// Video-only readback of what is on screen now (never chrome), with the
    /// owning context current. Synchronous; widget/probe use. A normal repaint
    /// is scheduled again before returning. `reason` receives a message on
    /// failure and is empty on success.
    virtual QImage readVideoFrame(QString* reason) = 0;
    /// Preferred asynchronous readback. Completion fires exactly once, on the
    /// session's (GUI) thread, with an empty reason on success. A second call
    /// while one is in flight completes immediately with a reason.
    virtual void readVideoFrameAsync(std::function<void(QImage, QString)> completion) = 0;

    // ── context lifecycle owned by the presenter ──────────────────────────
    /// Create the render context against `core` with the owning GL context
    /// current (the presenter makes it current). `core` is the session's
    /// shared lease: the implementation MUST keep it until the render context
    /// is freed, so the mpv core outlives every render context free. False with
    /// `reason` set when the context cannot be created. Idempotent when ready.
    virtual bool ensureRenderContext(std::shared_ptr<MpvHandle> core,
                                     QString* reason = nullptr) = 0;
    /// Release the render context with its creating context current. The
    /// implementation calls VideoRenderBridge::releaseRenderContext(), which
    /// frees mpv's render context BEFORE dropping the core lease, and MAY leak
    /// the render context rather than free it without a current context.
    virtual void releaseRenderContext() = 0;
    /// Blocks until a render context exists or `timeoutMs` elapses. [GUI only]
    /// MUST NOT wait on the scene-graph render thread or hold its mutex.
    virtual bool waitForContext(int timeoutMs) = 0;
    /// Bumped whenever a render context is created, so a probe can tell a
    /// repaint from the context recreation that reloads the source.
    virtual quint64 renderContextGeneration() const = 0;
    /// Presentation counters (see UiProfileCounters).
    virtual UiProfileCounters uiProfileCounters() const = 0;

    // ── notifications the presenter pushes to the session ────────────────
    // Assigned by VideoSession::setPresenter() and invoked on the session's
    // thread. Implementations MUST marshal off their render thread first.
    std::function<void(const PresentationReport&)> framePresented;
    /// The playhead the lost context was showing, captured BEFORE the render
    /// context is released (a freed output reports 0 for a file it no longer
    /// decodes). The session uses it to resume an internal reload.
    std::function<void(double)> reloadPositionCaptured;
    std::function<void(quint64)> renderContextReady;
    std::function<void()> renderContextLost;
    /// Size, DPR, format or presentability changed.
    std::function<void(const RenderTargetFacts&)> targetStateChanged;
    /// A render failure the viewer must see, as viewer-facing copy.
    std::function<void(const QString&)> renderError;
};

}  // namespace mplayer
