// Rendering stays on the GUI thread with Qt's GL context current.
// Core requests are asynchronous: advanced control can deadlock a core wait.

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QMetaType>
#include <QOpenGLWidget>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Anime4K.hpp"
#include "MpvHandle.hpp"
#include "MediaTracks.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"
#include "native/components/AnimeJanaiBuildLog.hpp"
#include "PlaybackDiagnostics.hpp"
#include "native/prepare/NativeFrameFixture.hpp"
#include "native/status/NativePlaybackController.hpp"
#include "native/status/NativeTypes.hpp"
#include "native/prepare/NativePreparationContext.hpp"
#include "native/status/NativeStatus.hpp"
#include "native/prepare/NativeWorkLease.hpp"

// Declared where the view is complete: paintOverlay() hands the overlay code a
// painter, never an owned object.
class QPainter;
class QOpenGLFramebufferObject;

namespace mplayer {
class MpvHandle;
class MpvRenderContext;
}  // namespace mplayer

namespace mplayer {

/// Which shape an mpv property is read as. A manifest names it, because a
/// property read as the wrong shape is the manifest's mistake to see and not a
/// silent zero to accept.
enum class ProbePropertyKind { Number, Flag, Text };

/// One mpv property as the core reports it. A manifest asserts against the
/// value the core holds, not against a widget's cached mirror of it, so the
/// shapes mpv answers with are kept apart instead of coerced to one.
struct ProbePropertyValue {
    enum class Kind { Double, Flag, Integer, Text };
    Kind kind = Kind::Double;
    double number = 0.0;
    bool flag = false;
    int64_t integer = 0;
    QString text;

    bool isNumeric() const { return kind == Kind::Double || kind == Kind::Integer; }
    double asNumber() const { return kind == Kind::Integer ? static_cast<double>(integer) : number; }
};

/// Provenance for the legacy FPS value passed to the HUD; this is display
/// metadata only and says nothing about whether a native filter ran.
enum class StreamFpsSource : int {
    Unknown,
    MpvOutput,
    Container
};
struct ResolvedStreamFps {
    double framesPerSecond = 0.0;
    StreamFpsSource source = StreamFpsSource::Unknown;
};

/// Resolves current-source FPS observations for display; never infers Native
/// execution from an FPS value.
inline ResolvedStreamFps resolveStreamFps(
    const std::optional<double>& observedMpvOutputFps, double containerFps,
    bool sourceActive) {
    if (!sourceActive) {
        return {};
    }
    if (observedMpvOutputFps && std::isfinite(*observedMpvOutputFps) &&
        *observedMpvOutputFps > 0.0) {
        return {*observedMpvOutputFps, StreamFpsSource::MpvOutput};
    }
    if (std::isfinite(containerFps) && containerFps > 0.0) {
        return {containerFps, StreamFpsSource::Container};
    }
    return {};
}

/// Whether a snapshot in this state leaves the native-status reader polling.
///
/// Deliberately WIDER than native::nativeStateNeedsPolling, which asks a
/// narrower question - whether the producer advances that state by itself.
/// Probe does not advance by itself; it advances when the producer adopts a
/// decoded frame, and its own document says exactly that while the facts are
/// still missing ("probe passthrough: the decoder device identity is not
/// available"). So a reader that stopped at the first Probe would never read the
/// revision that names the device, which is the only thing an attached probe is
/// for. Every other state repeats identically until a new request arrives, and
/// halts.
inline bool nativeSnapshotKeepsPolling(native::NativeState state) {
    return state == native::NativeState::Active || state == native::NativeState::Preparing ||
           state == native::NativeState::Probe;
}

class VideoSurface : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit VideoSurface(QWidget* parent = nullptr);
    ~VideoSurface() override;

    // Loads url. Returns false only if the engine itself could not be created;
    // a stream that fails to open reports through errorOccurred instead.
    // `mediaToken` is an opaque identity the caller owns: it is echoed with the
    // load and end notifications for exactly this load, never for a load the
    // caller asked for later. 0 says the caller named none.
    bool open(const QString& url, quint64 mediaToken = 0);
    void close();

    const MediaTrackSnapshot& mediaTracks() const { return mediaTracks_; }
    MediaTrackSelection mediaTrackSelection() const { return selectedMediaTracks(mediaTracks_); }
    bool selectMediaTrack(MediaTrackType type, int64_t id, uint64_t expectedCoreIdentity,
                          uint64_t expectedSourceEpoch);
    void setNextMediaTrackSelection(MediaTrackSelection selection);

    void togglePause();
    /// Sets the paused state outright; same-value requests still express intent.
    void setPaused(bool paused);
    /// Reasserts an existing pause hold without expressing a newer user intent.
    void reassertPaused();
    bool pauseObserved() const { return observedPaused_; }
    /// Holds output paused during engine work, retaining explicit viewer intent.
    void setEnginePreparationPaused(bool active, bool restoreIntent = true);
    /// Retains one clean video snapshot through an engine/filter transition. A
    /// single video-only GPU snapshot is captured at activation and held until
    /// a fresh output frame arrives after deactivation; source and GL-context
    /// changes invalidate it.
    void setEngineFrameHold(bool active);
    bool playbackPauseIntent() const;
    /// Advances for every explicit pause/play request, including togglePause.
    quint64 pauseRequestGeneration() const { return pauseRequestGeneration_; }
    void seek(double seconds);
    void setVolume(double volume);   // 0..100
    void setMuted(bool muted);
    void setSpeed(double speed);

    // Swaps the Anime4K shader chain on the live handle, so the change lands
    // on the next rendered frame without reopening the file. Remembered, and
    // re-applied, because a mode picked before the engine exists still has to
    // take effect once it does.
    void setAnime4KMode(anime4k::Mode mode);
    void setAnimeJanaiPlan(const native::AnimeJanaiPlan& plan);
    /// Original decoded source facts observed asynchronously from mpv, never vf/display dimensions.
    native::AnimeJanaiSourceContext animeJanaiSourceContext() const;
    native::AnimeJanaiPlan animeJanaiPlan() const { return animeJanaiPlan_; }
    QString animeJanaiStatus() const { return animeJanaiStatus_; }
    bool animeJanaiEngineBuildActive() const { return animeJanaiEngineBuildActive_; }
    QString animeJanaiEngineBuildDetail() const { return animeJanaiEngineBuildDetail_; }
    bool animeJanaiProcessingObserved() const;
    anime4k::Mode anime4KMode() const { return anime4kMode_; }
    /// Swaps in an arbitrary chain by candidate id, which is what the compare
    /// window needs: it can name a single .glsl that has no Mode at all. The
    /// id is kept so the choice survives a re-resolve, never the resolved path
    /// list, which goes stale the moment the shaders folder moves.
    void setAnime4KChain(const QString& candidateId);
    /// The candidate id currently applied, empty for Off.
    QString anime4KChainId() const { return anime4kChainId_; }
    /// Changes on every public Anime4K mode/chain request, including same-value requests.
    quint64 anime4KRequestGeneration() const { return anime4KRequestGeneration_; }
    /// What mpv reports for glsl-shaders right now: the observed value, never a
    /// submission that has not landed. mpv does not validate a path list, so this
    /// cannot tell a working chain from a preset naming a deleted file - useful
    /// for Off vs On; the probe proves the rest with pixels.
    QString effectiveShaderChain() const;

    bool isContextReady() const { return contextReady_; }
    /// Spins the event loop until the render context exists. A surface that has
    /// been constructed but not yet shown never gets one, so a caller that
    /// needs a framebuffer has to be able to wait for it.
    bool waitForContext(int timeoutMs);

    /// The video readback for consumers that must not capture chrome: the same
    /// framebuffer Qt would hand grabFramebuffer(), rendered with this surface's
    /// overlay suppressed. A normal overlay repaint is scheduled before returning,
    /// but the widget is not composited again by this call. grabFramebuffer()
    /// itself stays the "what the user sees" readback. GUI thread only.
    QImage grabVideoFramebuffer();

    /// Probe-only: reads one mpv property so a manifest can assert on the core
    /// itself ("pause", "volume", "duration", "time-pos", "filename") rather
    /// than on the widget that mirrors it. Takes the mpv core lock, so it must
    /// not be called while native work holds that lock.
    std::optional<ProbePropertyValue> probeMpvProperty(const QString& name,
                                                        ProbePropertyKind want);
    /// Probe-only: asks the embedded core to cycle an audio or subtitle track.
    /// Returns whether the real core accepted the asynchronous command.
    bool probeCycleMpvTrack(const QString& property);
    /// Reads mpv's current playback timestamp and source epoch asynchronously.
    AsyncRequestId requestPlaybackTimestamp(
        std::function<void(uint64_t, std::optional<double>)> callback);

    bool isPlaying() const;
    /// Records the pause state and announces a change. Every local pause goes
    /// through here, because the state pump can only report what it did not
    /// already know.
    void setPlaying(bool playing);
    double position() const;
    double duration() const;
    int videoWidth() const;
    int videoHeight() const;
    bool hasVideo() const;
    bool hasVisibleOutputFrame() const { return hasVideo_ && hasVisibleOutputFrame_; }
    /// Current mpv source epoch; changes whenever the loaded source is replaced.
    uint64_t sourceEpoch() const;
    QString sourceUrl() const { return url_; }
    /// Monotonic generation of Aji plan requests, used to distinguish a preview
    /// owned by Compare from a newer explicit player request.
    quint64 animeJanaiGeneration() const { return animeJanaiGeneration_; }

    // ── native status ─────────────────────────────────────────────────────
    //
    // The surface owns one reader for its core and drives it from the existing
    // 50 ms pump, never from paintGL and never through a synchronous property
    // read. Polling is OFF by default: the parent that owns the native UI turns
    // it on only while native work is requested (Preparing, Switching or
    // Active), and turns it off once the filter reports anything else.
    void setNativeStatusPolling(bool enabled);
    bool nativeStatusPolling() const { return nativeStatusPolling_; }
    /// The last published native status. Unavailable with a named reason until
    /// the filter answers, and again whenever the filter is absent or its
    /// document is refused. Never synthesised: this is the reader's own view.
    const native::NativeStatusView& nativeStatus() const { return nativeStatus_.status(); }

    // ── native playback ───────────────────────────────────────────────────
    //
    // The surface owns the playback controller and is the one place its typed
    // commands become core calls. Every entry point is asynchronous: none waits
    // on the core lock, and a transport success is never an acknowledgement.
    // Attach is the probe binding reached only through a native request, so a
    // surface with native off issues none.

    /// Attach the probe filter so the producer can publish the observed source
    /// and device facts. Idempotent, carries no admission, and starts no engine;
    /// it is what makes nativePreparationContext() possible before any plan.
    ///
    /// `backend` is the DESIRED backend, not an observed one, and it is honoured
    /// before the filter can observe anything: the core is aligned to a decoder
    /// that backend can actually consume (nvdec for TensorRT, d3d11 for
    /// DirectML) and the alignment is then confirmed by reading the decoder back
    /// from the core itself. A backend whose decoder the core did not end up
    /// using is refused by name - DirectML never silently runs CUDA, TensorRT
    /// never runs on frames with no device, and nothing is attached so no
    /// observation is ever attributed to a decoder this backend cannot use.
    /// Requires a loaded Native runtime; otherwise it refuses as
    /// native.runtimeUnavailable without changing the decoder or filter chain.
    /// The default only keeps legacy callers compiling.
    void armNativeProbe(native::Backend backend = native::Backend::TensorRT);
    /// Resolve and submit an admission. The request is taken verbatim; the caller
    /// observes the verdict through nativePlaybackStatus()/nativePlaybackStatusChanged.
    /// Requires a loaded Native runtime; otherwise it refuses as
    /// native.runtimeUnavailable before changing controller or playback state.
    void requestNativeAdmission(const native::PreparedAdmissionRequest& request);
    void turnNativeOff();
    void suspendNativeProcessing();
    void resumeNativeProcessing();
    void drainNativeProcessing();

    /// Stops this core's native work with a REAL acknowledgement, on the GUI
    /// thread and before its core or GL context is destroyed. An in-flight
    /// transition is settled first (the filter refuses control while its own
    /// admit job runs), then an acknowledged plan is really suspended; no
    /// acknowledgement is ever faked from a cancelled local pending, and the
    /// claims survive anything that ends without one. The bound defaults past
    /// the controller's own control timeout, so its watchdog names a failure
    /// before this returns. True when the surface still exists and the
    /// controller has no transition left in flight.
    bool settleNativeForTeardown(
        int timeoutMs = static_cast<int>(native::NativePlaybackController::kDefaultControlTimeoutMs) +
                        1000);

    // ── native plan lock ──────────────────────────────────────────────────
    //
    // A frozen capture holds one of these so a Compare capture cannot be raced
    // by an activation; the freeze is on plan changes, never on status reads.

    /// Move-only scoped token; releasing is idempotent. MUST NOT outlive the
    /// surface.
    class NativePlanLock {
    public:
        NativePlanLock() = default;
        NativePlanLock(NativePlanLock&& other) noexcept;
        NativePlanLock& operator=(NativePlanLock&& other) noexcept;
        ~NativePlanLock();
        NativePlanLock(const NativePlanLock&) = delete;
        NativePlanLock& operator=(const NativePlanLock&) = delete;
        explicit operator bool() const { return !surface_.isNull(); }

    private:
        friend class VideoSurface;
        explicit NativePlanLock(VideoSurface* surface) : surface_(surface) {}
        void release();
        QPointer<VideoSurface> surface_;
    };

    /// nullopt with a reasonKey when no source epoch is open
    /// (`native.playback.noSource`) or a transition is already in flight
    /// (`native.playback.lockBusy`): an epoch that is already moving cannot be
    /// frozen. Nested acquires are counted.
    std::optional<NativePlanLock> acquireNativePlanLock(QString* reasonKey = nullptr,
                                                        QString* reason = nullptr);
    bool nativePlanLocked() const { return nativePlayback_.planLocked(); }
    /// 0 or 1: the latest request recorded behind the lock.
    int queuedNativeTransitions() const { return nativePlayback_.hasQueuedTransition() ? 1 : 0; }

    /// The controller's observed verdict. `effective`/`counters` are present only
    /// after the producer's own snapshot acknowledged them, never from a request.
    const native::NativePlaybackController::Status& nativePlaybackStatus() const {
        return nativePlayback_.status();
    }

    /// The live source/device facts a preparation caller needs, from the
    /// producer's own report. nullopt while either is unobserved or incomplete:
    /// the missing fact is never guessed here. The context also carries this
    /// core's optional factory observation when the very document that reported
    /// the source also published one; that observation is dropped with the
    /// binding and with every source-epoch change, so a caller can never read an
    /// old source's factory as this one's.
    std::optional<native::NativePreparationContext> nativePreparationContext() const;

    /// Requests ONE actual decoded source frame, asynchronously. The core runs it
    /// on its own thread; the surface copies the reply into a FrameFixture and
    /// answers with nativeReferenceFrameReady/Failed. Refused while a native plan
    /// could be what the chain holds, and while a preparation build holds the
    /// shared work lease. At most one capture is in flight.
    ///
    /// `frameIndex` is 0 for the first observation and 1 for the ADJACENT second of
    /// a temporal pair, and it changes what the surface does before capturing:
    ///
    ///  - index 0 is the frame at the position playback is parked at. Nothing
    ///    moves: no seek, no unpause, no decode of anything else. The position
    ///    returned is where the core actually was, which is what the caller
    ///    restores afterwards.
    ///  - index 1 is the NEXT source frame after the one index 0 captured. It is
    ///    obtained by an exact seek to that frame's timestamp, where the interval
    ///    is the OBSERVED source cadence (the filter's own reported fps rational,
    ///    never a guessed 24) - so the pair is two real adjacent frames of one
    ///    source, not one frame captured twice. The original paused position is
    ///    restored before the capture is reported, and the pause, speed, mute and
    ///    track selection are untouched throughout. A source that is not seekable,
    ///    or whose cadence was never observed, cannot produce a genuine neighbour
    ///    and is refused by name rather than answered with a second copy of frame
    ///    0. A neighbour that decodes to the same bytes as frame 0 is refused the
    ///    same way: the surface never hands over a duplicated reference.
    void requestNativeReferenceFrame(int frameIndex = 0);

    // ── raw playback diagnostics ──────────────────────────────────────────
    PlaybackDiagnostics playbackDiagnostics() const;

signals:
    void playingChanged(bool playing);
    /// A source actually opened on this core: mpv reported the file loaded -
    /// headers were read - for the entry this surface's own load created.
    /// `mediaToken` is the identity that load was requested with. A click, a
    /// loadfile command or a start-file alone does not emit it.
    void mediaLoaded(quint64 mediaToken);
    /// The file loaded under `mediaToken` reached its natural end. A stop, an
    /// error, a redirect, a replacement load, a renderer teardown or a core
    /// hand-off never emits it, and it is emitted while the ending file is
    /// still the one this core has loaded.
    void mediaEnded(quint64 mediaToken);
    /// The loaded source changed, invalidating any frame captured under the old epoch.
    void sourceEpochChanged(uint64_t epoch);
    /// A seek intent can precede, or never change, the observed position.
    void seekRequested(double seconds);
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void videoSizeChanged(int width, int height);
    void streamStatsChanged(double bytesPerSecond, double framesPerSecond,
                            StreamFpsSource fpsSource);
    void errorOccurred(const QString& message);
    void mediaTracksChanged();
    void mediaTracksError(const QString& reason);
    void nativeStatusChanged(const native::NativeStatusView& status);
    /// A native transition ended in a named non-Active verdict: a refused or
    /// timed-out command, or a filter that refused the admitted plan.
    void nativeProcessingFailure(const QString& reasonKey, const QString& reason);
    void animeJanaiStatusChanged(const QString& status);
    void animeJanaiFailure(const QString& reason);
    void animeJanaiEngineBuildChanged(bool active, bool succeeded, const QString& detail);
    /// The controller's status changed. This is how an asynchronous transition
    /// is acknowledged: read the status, never a transport return.
    void nativePlaybackStatusChanged(const native::NativePlaybackController::Status& status);
    /// One captured frame, as the core returned it, in interleaved RGB8 at the
    /// frame's own size. `framePosition` is where playback actually was when the
    /// core answered - a frame-exact reading of the core's own clock, and what the
    /// caller restores afterwards - not the position the request was made at.
    /// `frameIndex` echoes the index the request named. `frameFps` is the observed
    /// source cadence this capture's position was read at: absent means the cadence
    /// was never observed, and the interval between two adjacent frames is then
    /// unknown rather than assumed.
    void nativeReferenceFrameReady(const native::FrameFixture& frame, quint64 sourceEpoch,
                                   double framePosition, int frameIndex,
                                   std::optional<native::Rational> frameFps);
    void nativeReferenceFrameFailed(const QString& reasonKey, const QString& reason, int frameIndex);
    /// Emitted once per paintGL, after mpv's frame and Qt's GL state are
    /// restored and before the painter is ended: the painter is active and in
    /// logical widget coordinates. The receiver paints and does not keep it.
    void paintOverlay(QPainter* painter);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    static const char* nativePlaybackStateName(native::NativePlaybackController::State state);

    /// Creates the engine with vo=libmpv. Both open() and initializeGL() need it
    /// and whichever runs first wins, so the options cannot be set in only one.
    void ensureEngine();
    /// Issues the deferred load once the render context exists. `resumeAt` is
    /// the position the new file starts at, non-zero only when a recreated
    /// render context has to give mpv its video back.
    bool startPendingUrl(double resumeAt = 0.0);
    void pumpEvents();
    void handleState();
    void updateStreamStats();
    void cancelAnimeJanaiOperation();
    void processAnimeJanaiPlan();
    void animeJanaiFail(const QString& reasonKey, const QString& reason);
    void setAnimeJanaiStatus(const QString& status);
    void resetAnimeJanaiObservation();
    bool animeJanaiBackendLogsNeeded() const;
    void observeAnimeJanaiBackendLog(const QString& prefix, const QString& line);
    void publishAnimeJanaiObservation(const QString& status, uint64_t epoch,
                                      quint64 generation);
    void publishPendingAnimeJanaiObservation(uint64_t epoch, quint64 generation);
    void restoreAnimeJanaiHwdec(uint64_t epoch, quint64 generation);
    /// Subscribes to every property the HUD reads. The values arrive as events,
    /// so the 50 ms pump never takes the core lock to learn what it shows.
    void observePlaybackState();
    void publishObservedState();
    void resetMediaTracks();
    void refreshMediaTracks();
    /// One native-status pump step, driven from the 50 ms timer. The reader is
    /// ticked even while polling is off, so a timed-out request is still
    /// abandoned and the current source epoch keeps being observed.
    void pollNativeStatus();
    /// True while the reader should issue a request: polling was requested (or a
    /// native transition is in flight and must be observed), the filter has not
    /// been found absent, and the last state read is one that a poll can advance
    /// (or has not been read at all).
    bool nativeStatusShouldPoll() const;
    /// Drops the reader's binding, cancelling any in-flight request, and
    /// optionally rebinds it to the core that is alive now. MUST run before the
    /// borrowed core is destroyed, and before a new source is bound.
    void resetNativeStatusReader(bool reattach);

    /// Binds the controller to the core's current source epoch, if it moved. The
    /// old epoch's replies and observations are then discarded by the controller;
    /// the held claims are not released, because a source change is not a stop.
    void syncNativeSource();
    /// A native request is a request-state change: it re-arms a poll a terminal
    /// state or an absent filter had stopped, so the transition is observed.
    /// The accepted attach also calls this: it is the only moment the chain
    /// really changed, so a refusal latched before it stops being true. It does
    /// NOT re-enable polling on its own - a caller still has to ask for the
    /// status - it only lets a poll that was already asked for keep asking.
    void armNativeObservation();
    /// Carries one typed command to the core over the existing async transport.
    void deliverNativeCommand(const native::NativePlaybackCommand& command);
    /// Routes one controller verdict: names a failure, then publishes a real
    /// status change. Never promotes a transport success.
    void applyNativeDecision(const native::NativePlaybackController::Decision& decision);
    /// Emits nativePlaybackStatusChanged when the controller status actually
    /// moved, so a consumer never has to poll for a transition.
    void publishNativePlaybackStatus();
    /// The one release path for a NativePlanLock token: drops one lock, submits
    /// the queued transition under the controller's epoch guard, and announces
    /// the unlock. Idempotent for a token whose lock is already gone.
    void releaseNativePlanLock();
    /// Set for the whole of settleNativeForTeardown: the bounded wait spins the
    /// event loop, so a reentrant close must not start a second settlement.
    bool nativeTeardownSettling_ = false;
    qint64 nativePlaybackNowMs() const { return nativePlaybackClock_.elapsed(); }
    /// Ends a capture that can no longer be answered because the source moved.
    void abandonNativeReferenceFrame(const QString& reasonKey, const QString& reason);

    /// Set for the whole of one probeMpvProperty() call: the events that read
    /// drains are dispatched on this thread, and a probe that started inside one
    /// of them would be answered from a queue its caller is already draining.
    bool probeReadActive_ = false;

    // ── explicit probe alignment ───────────────────────────────────────────
    //
    // One alignment at a time, and it completes before the filter is attached:
    // a filter that has already seen frames cannot be told which decoder they
    // came from, so nothing is attached until the core reports the decoder the
    // requested backend can use. Every read is asynchronous and every answer is
    // checked against the request it answers, so an alignment cannot be completed
    // by a reply for a different backend or a different source.

    /// What the alignment is currently proving. Nothing is attached until one of
    /// these resolves, so no observation is ever attributed to the wrong decoder.
    struct NativeProbeAlignment {
        native::Backend backend = native::Backend::TensorRT;
        uint64_t epoch = 0;
        /// The exact hwdec value the core must be aligned to for `backend`.
        QString required;
    };
    /// True when the decoder the core reported can actually feed `backend`: the
    /// CUDA interop TensorRT consumes, or the D3D11 textures DirectML consumes.
    /// A software-decoded frame is compatible with neither, and `copy` variants
    /// are refused: they download to system memory, so the frames the backend
    /// would receive carry no device at all.
    static bool decoderServesBackend(const QString& decoder, native::Backend backend);
    /// The hwdec value that aligns a core to `backend`, or empty when this build
    /// names none - which is itself a refusal, never a fallback to whatever the
    /// core happened to pick.
    static QString hwdecForBackend(native::Backend backend);
    /// One asynchronous property read belonging to the alignment. It is a tiny
    /// tagged union because the alignment reads two kinds of property - a flag
    /// and a string - and each must be answered only against the request it was
    /// issued for.
    struct NativeProbeRead {
        enum class Kind { None, Seekable, Hwdec, TimePos };
        Kind kind = Kind::None;
        uint64_t epoch = 0;
        /// The backend this read answers, so a reply for another one is dropped.
        std::optional<native::Backend> backend;
        bool seekable = false;
        QString hwdec;
        double timePos = 0.0;
    };
    /// Starts the alignment: write the property, then read the decoder back.
    void beginNativeProbeAlignment(const NativeProbeAlignment& alignment);
    void applyNativeProbeAlignmentReply(const NativeProbeRead& read);
    /// Attaches the filter on a compatible decoder, or refuses the alignment by
    /// name and attaches nothing. `observedDecoder` is the value the core
    /// itself reported, and it is what gets recorded - never the value the
    /// alignment asked for.
    void finishNativeProbeAlignment(bool accepted, const QString& observedDecoder,
                                     QString* reasonKey, QString* reason);
    /// Drops an in-flight alignment, so a superseded answer cannot complete one.
    void cancelNativeProbeAlignment(const QString& reasonKey, const QString& reason);
    /// One alignment property read in flight. valid() is the in-flight flag.
    AsyncRequestId nativeProbeAlignRequest_;
    /// The alignment in flight, if any.
    std::optional<NativeProbeAlignment> nativeProbeAlign_;
    /// True once the core reported, for THIS source, a decoder this build can
    /// name. Nothing else may treat the alignment as done.
    bool nativeProbeAligned_ = false;
    /// The decoder the core reported for the last completed alignment. This is an
    /// observation of the core, never a factory identity, a preference, or the
    /// value the alignment asked for.
    QString nativeProbeDecoder_;
    /// The backend the completed alignment was for.
    std::optional<native::Backend> nativeProbeBackend_;

    // ── adjacent-frame capture ─────────────────────────────────────────────
    //
    // The second frame of a temporal pair is a real neighbour of the first, so it
    // costs one exact seek out and one back. Both are issued through the ordinary
    // asynchronous transport, and both are checked against the source epoch and
    // the request they belong to: a restore for a superseded source is dropped,
    // never issued.

    /// Issues the capture itself at the core's current position.
    void issueNativeReferenceCapture(uint64_t epoch, int frameIndex, double position);
    /// Reads the core's own clock so the position reported for a capture is the
    /// position the frame was actually decoded at.
    void readNativeReferencePosition(uint64_t epoch, int frameIndex);
    /// Seeks one source frame on from `restore`, captures there, and seeks back.
    /// The step is `restore + cadence`, where the cadence is the OBSERVED source
    /// rational, and the restore is COMPLETED before the capture is reported, so
    /// a caller never sees a position the surface has already moved away from.
    void stepToAdjacentNativeFrame(double restore, double adjacent, int frameIndex);
    /// Publishes a capture whose position has already been restored, and records
    /// its digest so a later request that observes the same frame can be refused.
    void emitNativeReferenceCaptureReady(const native::FrameFixture& frame, uint64_t epoch,
                                         double position, int frameIndex,
                                         const QByteArray& digest);
    /// Seeks the core back to `restore` and reads the core's own clock until it
    /// is really there, so a capture is never reported from a position the
    /// viewer has already left. Nothing is invented while waiting: a read the
    /// core will not answer, or one that never arrives, ends in a NAMED failure.
    ///
    /// The completion boundary is the core's own `time-pos`, NOT the seek's
    /// transport reply: mpv answers that reply when the command is ACCEPTED, one
    /// drain before the seek has moved the playhead, so a report built on it
    /// names a position the core has not reached. This is read back against the
    /// source epoch on every round, so a restore for a superseded source is
    /// dropped rather than waited for.
    void awaitNativeReferenceRestore(uint64_t epoch, int frameIndex, double restore,
                                     std::optional<native::Rational> cadence,
                                     std::shared_ptr<const native::FrameFixture> frame,
                                     std::optional<double> decodedPosition,
                                     std::optional<QByteArray> digest, qint64 deadlineMs);
    /// One round of that confirmation: reads `time-pos` back, publishes the held
    /// frame if it is within half the OBSERVED source frame of `restore`, and
    /// otherwise re-issues the seek while the deadline and the attempt bound
    /// allow. Settled means within half a frame and nothing looser: an unobserved
    /// or degenerate cadence has no honest bound at all, so such a read can never
    /// settle and the wait ends in a named refusal rather than a reported frame.
    void pollNativeReferenceRestore(uint64_t epoch, int frameIndex, double restore,
                                    std::optional<native::Rational> cadence,
                                    std::shared_ptr<const native::FrameFixture> frame,
                                    std::optional<double> decodedPosition,
                                    std::optional<QByteArray> digest, qint64 deadlineMs,
                                    int attempts);
    /// Moves the published position onto the core's CONFIRMED clock, taken from
    /// the very read that settled a restore, and announces it.
    ///
    /// position() is a cache that only the 50 ms pump refreshes, and the pump has
    /// not run between the core confirming the restore and the capture being
    /// reported: a caller that reads position() synchronously in the frame-ready
    /// handler would otherwise see the core one frame ahead of where it was put
    /// back, and conclude the restore did not land. The value written here is the
    /// core's own reading, not the position that was requested, and the observer
    /// is moved with it so the next pump does not bounce the cache backwards.
    /// Non-finite readings are refused: nothing is published from a position that
    /// was never confirmed. Pause, speed and track selection are untouched.
    void announceRestoredPosition(double confirmed);
    /// Names a restore that could not be completed or confirmed, without ever
    /// reporting a frame the surface is not sure belongs where it claims.
    void refuseNativeReferenceRestore(const QString& reasonKey, const QString& reason,
                                      int frameIndex, uint64_t epoch);
    /// Ends a capture that produced no usable frame, restoring the position this
    /// surface moved the core to before the refusal is published.
    void finishNativeReferenceCapture(const QString& reasonKey, const QString& reason,
                                      int frameIndex);
    /// Ends a capture that is no longer answerable - a superseded source, a
    /// refused seek, or a cadence that was never observed - with the same
    /// restore-before-reporting rule.
    void discardNativeReferenceStep(const QString& reasonKey, const QString& reason);
    /// Bytes of the frame already answering for this source epoch, so a request
    /// that observes the same decoded frame again is refused by name instead of
    /// delivered twice as a second reference.
    QByteArray nativeReferenceLastHash_;
    /// The source epoch the last captured bytes belong to.
    uint64_t nativeReferenceLastEpoch_ = 0;
    /// The position the neighbouring-frame step must put the core back at. Absent
    /// when nothing is owed.
    std::optional<double> nativeProbeResumePosition_;
    /// The one capture in flight. valid() is the in-flight flag.
    AsyncRequestId nativeReferenceRequest_;
    /// The one settle read in flight, i.e. the outstanding `time-pos` round that
    /// decides whether the held frame may be published. valid() is the in-flight
    /// flag; it is cancelled by refuseNativeReferenceRestore, so a named failure
    /// leaves no read that could still publish the frame it just refused.
    AsyncRequestId nativeReferenceRestoreRequest_;
    /// The index the in-flight capture was requested for.
    int nativeReferenceFrameIndex_ = 0;
    /// True while a neighbouring-frame seek is outstanding, so a source change
    /// retires the restore along with the capture.
    bool nativeReferenceStepping_ = false;
    /// Held for the life of a capture: while it is taken, no preparation build
    /// can start, and it is what refuses a capture during a build.
    std::optional<native::NativeWorkLease> nativeReferenceLease_;

    enum DiagnosticSlot { kDecoderDropSlot = 0, kVoDropSlot, kVfSlot };
    static constexpr int kDiagnosticSlotCount = 3;
    struct DiagnosticRequest {
        AsyncRequestId pending;   // valid() is the in-flight flag
        qint64 lastRequestMs = 0;
    };

    void pollDiagnostics();
    /// Cancels owned reads and drops every raw number to unknown.
    void resetDiagnostics();
    void issueDiagnostic(DiagnosticSlot slot, const char* property, uint64_t epoch);
    void applyDiagnosticReply(DiagnosticSlot slot, const AsyncReply& reply);
    void setDiagnosticUnknown(DiagnosticSlot slot);

    /// Creates the render context for the context that is current now. Its mpv
    /// callback only queues a generation-stamped hop to the GUI thread.
    void createRenderContext();
    /// Releases the render context with THIS widget's context current, which is
    /// what mpv requires for the free. Wired to
    /// QOpenGLContext::aboutToBeDestroyed and repeated from the destructor.
    void releaseRenderContext();
    /// The GUI-thread end of mpv's update callback: coalesced and generation
    /// checked. One step of mpv's render state, then a repaint request.
    void onRenderUpdate(quint64 generation);
    /// Advances mpv's render state with the context current - update(), and
    /// either a pending frame for the next paint or a skip that is acknowledged
    /// when no paint can happen. The only place render state moves outside
    /// paintGL, so every callback is answered exactly once.
    void advanceRenderState();
    /// Whether Qt will actually paint this widget now: an unexposed or minimized
    /// window gets no paint event, so a frame kept for one would never be
    /// presented - nor acknowledged.
    bool paintCanHappen() const;
    /// Consumes the swap report owed for the frame this paint rendered. Called
    /// from frameSwapped, so the report names an actual presentation.
    void onFrameSwapped();
    /// Sends the report still owed for a frame this surface painted that the
    /// widget will now never present, under this surface's context. mpv's VO
    /// waits for that acknowledgement before it releases the next frame.
    void acknowledgeDroppedFrame();
    /// The pause value this surface has submitted and mpv has not yet been
    /// observed to hold. Property events and command replies arrive in the order
    /// the core processed them, so an observation that contradicts this one is
    /// OLDER than the request and must not overwrite the viewer's intent.
    struct PauseWrite {
        quint64 generation = 0;
        /// The source it was issued against: a write for a source that has since
        /// been replaced can neither be confirmed nor block anything.
        uint64_t epoch = 0;
        bool target = false;
    };
    /// nullopt when nothing is in flight. A write mpv accepted is kept here
    /// until its value is observed, because the command reply can be drained
    /// before the property event that carries the value: retiring the write at
    /// the reply would let that event's stale predecessor overwrite the
    /// viewer's intent in between.
    std::optional<PauseWrite> pendingPauseWrite_;
    /// Advances for every pause write issued, including reassertions.
    quint64 pauseWriteGeneration_ = 0;
    /// Submits one pause value and tracks it until mpv's own property event
    /// confirms it. The command reply retires only a request for the value the
    /// core was already observed to hold, since that request owes no event.
    void requestPausedWrite(bool paused);
    /// The pause state the viewer has asked for: the write still in flight, else
    /// mpv's observed value.
    bool requestedPause() const {
        return pendingPauseWrite_.has_value() ? pendingPauseWrite_->target : observedPaused_;
    }
    /// Set while a readback is suppressing this surface's overlay, so a paint made
    /// for it stays out of the captured image.
    bool overlaySuppressed_ = false;
    /// Advances the render state on demand, for a caller that knows the core
    /// changed something (a shader chain swap) and needs a repaint of it.
    void refreshRenderState();
    /// Releases a retained picture without letting it cross a source boundary.
    void invalidateEngineFrameHold();
    /// Captures the current video-only output into a bounded GPU framebuffer
    /// once at transition-hold acquisition, never during ordinary playback.
    bool captureEngineFrameHold();
    /// Composites the retained image into Qt's current framebuffer, scaling after
    /// a resize while preserving the surface's device-pixel dimensions.
    bool drawEngineFrameHold(unsigned int target, int width, int height);
    /// Brings this widget's context current for render work done outside
    /// paintGL. False when there is none; on true, `previous` holds whatever was
    /// current before, which releaseRenderContextCurrent() puts back.
    bool makeRenderContextCurrent(QOpenGLContext** previous);
    void releaseRenderContextCurrent(QOpenGLContext* previous);
    /// Issues one core command without waiting for it: mpv's own commands block
    /// until the core takes them, which this surface must never do while native
    /// work can hold that core. `reportRefusal` names a refusal through
    /// errorOccurred; false only where the caller documented it as normal.
    /// `mediaToken` marks the command as a media load that identity belongs to,
    /// so a load the core refuses is retired from the pairing queue instead of
    /// claiming the next entry mpv starts.
    void issueCoreCommand(std::vector<std::string> args, const char* what, bool reportRefusal,
                          std::optional<quint64> mediaToken = std::nullopt);
    /// Submits one path list to glsl-shaders asynchronously. What the surface
    /// reports as the chain stays mpv's observed value, never the submission.
    void applyShaderChainRequest(const QString& shaders);

    // Held so the render context can be freed with it current at teardown.
    QOpenGLContext* context_ = nullptr;
    /// The context's own destruction signal, so it is not fired against a
    /// widget that already let the renderer go.
    QMetaObject::Connection contextDestroyedConnection_;
    std::unique_ptr<MpvHandle> mpv_;
    std::unique_ptr<MpvRenderContext> render_;
    QString url_;
    anime4k::Mode anime4kMode_ = anime4k::Mode::Off;
    QString anime4kChainId_;
    quint64 anime4KRequestGeneration_ = 0;
    quint64 pauseRequestGeneration_ = 0;
    native::AnimeJanaiPlan animeJanaiPlan_;
    QString animeJanaiStatus_;
    QString animeJanaiObservedStatus_;
    bool animeJanaiEngineBuildActive_ = false;
    QString animeJanaiEngineBuildDetail_;
    int animeJanaiAjiStageCount_ = 0;
    int animeJanaiConfiguredAjiStageCount_ = 0;
    native::AnimeJanaiBuildGate animeJanaiAggregateBuildGate_;
    int animeJanaiPendingAjiBuildCount_ = 0;
    int animeJanaiBuildPollStageIndex_ = 0;
    bool animeJanaiBackgroundBuildFinished_ = false;
    bool enginePreparationPaused_ = false;
    uint64_t enginePauseEpoch_ = 0;
    quint64 enginePauseRequestGeneration_ = 0;
    bool enginePauseIntent_ = true;
    AsyncRequestId animeJanaiBuildPollRequest_;
    qint64 animeJanaiBuildNextPollMs_ = 0;
    bool animeJanaiBuildPollingRefused_ = false;
    bool enginePauseHeld() const;
    void beginEnginePause();
    void endEnginePause(bool restoreIntent);
    void discardEnginePause();
    void pollAnimeJanaiEngineBuild();
    void finishAnimeJanaiEngineBuild(bool succeeded, const QString& detail);
    QString animeJanaiPendingObservedStatus_;
    uint64_t animeJanaiObservedEpoch_ = 0;
    quint64 animeJanaiObservedGeneration_ = 0;
    uint64_t animeJanaiPendingObservedEpoch_ = 0;
    quint64 animeJanaiPendingObservedGeneration_ = 0;
    bool animeJanaiObserved_ = false;
    bool animeJanaiReportPending_ = false;
    bool animeJanaiSourceRequested_ = false;
    AsyncRequestId animeJanaiRequest_;
    quint64 animeJanaiGeneration_ = 0;
    uint64_t animeJanaiEpoch_ = 0;
    QString animeJanaiPreviousHwdec_;
    /// The official @mikai_aji label only; independent of private mikai_native membership.
    bool animeJanaiFilterBound_ = false;
    bool animeJanaiBusy_ = false;
    bool animeJanaiReconfigureAfterRestore_ = false;
    QStringList animeJanaiBoundFilterLabels_;
    std::shared_ptr<native::AnimeJanaiStageBuildLease> animeJanaiStageBuildLease_;
    bool animeJanaiFailureCleanupActive_ = false;
    void removeAnimeJanaiFilters(uint64_t epoch, quint64 generation,
                                 std::function<void()> completion);

    // mpv keeps running whether or not we repaint, so the state pump cannot
    // live in paintGL: a paused video produces no frames, and the clock,
    // the duration and the pause state would all freeze with the last one.
    QTimer statePump_;
    QElapsedTimer statsClock_;
    // Monotonic, elapsed-from-construction: the reader's cadence must not be
    // read off a wall clock that can step.
    QElapsedTimer nativeStatusClock_;

    // The one native-status reader this surface owns, bound to its own core.
    native::NativeStatusConsumer nativeStatus_;
    bool nativeStatusPolling_ = false;
    /// Set when the filter is absent (the async property read returned an mpv
    /// error): polling stops until the request state changes or a new source is
    /// opened, so an absent filter is probed once, not hammered.
    bool nativeStatusHalted_ = false;
    /// True once this binding has published a real snapshot. It distinguishes a
    /// filter that has not answered yet (retry) from one that answered and then
    /// went away (stop).
    bool nativeSnapshotSeen_ = false;

    // The playback transaction owner: it never touches the core itself, and the
    // surface is its only sink. Its claims outlive a stop and are released only
    // when the core dies or a transition is acknowledged.
    native::NativePlaybackController nativePlayback_;
    // Monotonic, elapsed-from-construction: a control deadline must not be read
    // off a wall clock that can step.
    QElapsedTimer nativePlaybackClock_;
    /// Set once a source has been opened. Deliberately not cleared by close(): an
    /// unacknowledged stop keeps the claims, so the epoch stays bound until the
    /// core itself dies.
    bool nativeSourceActive_ = false;
    uint64_t nativeBoundEpoch_ = 0;
    /// True while the named filter may still be in the core's chain, including
    /// an add or removal in flight. The chain survives a source change, so the
    /// controller's attach transition does not re-issue `vf add` for a bound label;
    /// removal clears this only after the controller observes its absence.
    bool nativeFilterBound_ = false;
    bool nativeStatusPublished_ = false;
    native::NativePlaybackController::Status lastNativeStatus_;


    QElapsedTimer diagnosticsClock_;
    DiagnosticRequest diagnosticRequests_[kDiagnosticSlotCount];
    qint64 diagnosticsLastPollMs_ = 0;
    bool diagnosticsActive_ = false;
    PlaybackDiagnostics diagnostics_;

    bool contextReady_ = false;
    bool pendingOpen_ = false;
    bool framePending_ = false;   // set by mpv's update callback
    /// Bumped whenever a render context is created or released, so a hop queued
    /// by a released context's callback finds a different generation.
    quint64 renderGeneration_ = 0;
    /// The generation of the hop already queued: every callback mpv raises
    /// before it runs collapses into one repaint. Written from mpv's threads.
    std::atomic<quint64> queuedRenderCallback_{0};
    /// A frame really rendered that Qt has not composed yet, so the one swap
    /// report it is owed goes out once, from frameSwapped.
    bool videoFramePainted_ = false;
    /// Set while releasing the render context: mpv answers that by killing the old
    /// video output and ending the playing file with MPV_ERROR_VO_INIT_FAILED, an
    /// error that belongs to the renderer being replaced, not to the source. The
    /// next START_FILE or the first successful render closes the window, so a
    /// video output that really fails to initialize is still reported.
    bool rendererTeardownErrorPending_ = false;
    /// Playlist entry from the last START_FILE, used to reject stale END_FILE events.
    int64_t currentPlaylistEntryId_ = 0;
    bool havePlaylistEntryId_ = false;
    struct LoadIdentity {
        quint64 token = 0;
        uint64_t epoch = 0;
    };
    std::map<uint64_t, LoadIdentity> pendingLoadTokens_;
    std::map<int64_t, LoadIdentity> entryTokensById_;
    uint64_t entrySourceEpoch_ = 0;
    MediaTrackSnapshot mediaTracks_;
    AsyncRequestId mediaTracksRequest_;
    bool mediaTracksLoaded_ = false;
    bool mediaTracksDirty_ = false;
    std::optional<MediaTrackSelection> nextMediaTrackSelection_;
    MediaTrackSelection pendingMediaTrackSelection_;
    /// Identity of the load named by the last START_FILE: the file playing now.
    quint64 entryMediaToken_ = 0;
    /// Suppresses duplicate FILE_LOADED events for one entry.
    bool entryLoadReported_ = false;
    /// Token for the current request, retained until a deferred open runs.
    quint64 mediaToken_ = 0;
    /// Forces the next paint to render even with no freshly decoded frame:
    /// set when Qt hands the widget a new framebuffer (resize, re-show) and
    /// the previous picture is no longer there to composite.
    bool forceRender_ = true;
    bool hasRenderedFrame_ = false;
    /// True only after a successfully rendered output frame, not a forced redraw
    /// of an empty renderer. A hold never invents a picture before this point.
    bool hasVisibleOutputFrame_ = false;
    /// A transition keeps one snapshot only until mpv supplies a replacement.
    bool engineFrameHoldActive_ = false;
    bool engineFrameHoldAwaitingOutput_ = false;
    std::unique_ptr<QOpenGLFramebufferObject> engineFrameHoldFbo_;
    QSize lastRenderedFboSize_;
    bool playing_ = false;
    double position_ = 0.0;
    bool hasVideo_ = false;
    double duration_ = 0.0;
    int videoWidth_ = 0;
    int videoHeight_ = 0;
    double networkBytesPerSecond_ = 0.0;
    double streamFramesPerSecond_ = 0.0;
    StreamFpsSource streamFpsSource_ = StreamFpsSource::Unknown;

    // Observed values, written only by the property observers during
    // drainEvents. Everything the HUD reads comes from here.
    double observedDuration_ = 0.0;
    double observedPosition_ = 0.0;
    bool observedPaused_ = false;
    double observedVideoWidth_ = 0.0;
    double observedVideoHeight_ = 0.0;
    double observedCacheSpeed_ = 0.0;
    double observedContainerFps_ = 0.0;
    QString shaderChain_;
    /// The subscriptions this widget owns, released before the core.
    std::vector<uint64_t> observers_;
};

}  // namespace mplayer

// Declared exactly once, here, where the view is complete: the change signal
// carries it by const reference and a queued connection needs the type known
// to the meta-object system.
Q_DECLARE_METATYPE(mplayer::native::NativeStatusView)
Q_DECLARE_METATYPE(mplayer::native::NativePlaybackController::Status)
Q_DECLARE_METATYPE(mplayer::StreamFpsSource)
// NativePreparationContext is declared by its owning header and SelectionState /
// FrameFixture need no declaration at all: Qt6 instantiates QMetaTypeId for any
// complete type. Declaring them here would be a redefinition, because a
// translation unit that includes this header after one of those has already
// instantiated QMetaTypeId for the type.
