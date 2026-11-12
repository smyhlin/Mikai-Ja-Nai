// Playback session: one mpv core, its asynchronous control, and all observed
// state, on a QObject GUI thread. It owns no GL object and never draws; the
// presenter implementation owns the render context and every GPU resource and
// is reached only through the narrow VideoPresenter contract.
//
// Core requests stay asynchronous: a synchronous call can block the GUI thread
// while native work holds the core lock.

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QMetaType>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

QT_BEGIN_NAMESPACE
class QSettings;
QT_END_NAMESPACE

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
#include "MediaInformation.hpp"
#include "VideoPresenter.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"
#include "native/components/AnimeJanaiBuildLog.hpp"
#include "PlaybackDiagnostics.hpp"
#include "native/prepare/NativeFrameFixture.hpp"
#include "native/status/NativePlaybackController.hpp"
#include "native/status/NativeTypes.hpp"
#include "native/prepare/NativePreparationContext.hpp"
#include "native/status/NativeStatus.hpp"
#include "native/prepare/NativeWorkLease.hpp"

namespace mplayer {

class VideoSession;

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

/// Display-level video scaling (GOAL-H2). A mode owns all seven mpv options the
/// picture geometry is made of, not just its own, so switching from Fill to Fit
/// cannot keep the previous mode's crop alive.
enum class VideoScaleMode : int {
    Fit = 0,
    Fill = 1,
    Stretch = 2,
    Aspect16x9 = 3,
    Aspect4x3 = 4,
    Original = 5
};

struct VideoScalePolicy {
    VideoScaleMode mode = VideoScaleMode::Fit;
    int zoomPercent = 0;
    QString upscale = QStringLiteral("lanczos");
    /// Empty means Auto: mpv's own default, read from the core, because the
    /// pinned runtime refuses the literal "auto" for dscale.
    QString downscale;
    bool operator==(const VideoScalePolicy&) const = default;
};

/// One mpv option a display mode owns.
struct VideoScaleOption {
    std::string name;
    std::string value;
};

/// mpv's video-zoom argument for a viewer-facing percentage: log2(1 + pct/100),
/// rendered with Qt's locale-independent formatting.
QString videoZoomArgument(int zoomPercent);
/// The card's range, applied wherever a percentage enters the core.
int clampedVideoZoomPercent(int zoomPercent);
/// Stretch and Original 1:1 are unscaled pictures: their stored percentage is
/// remembered but never reaches mpv.
int effectiveVideoZoomPercent(VideoScaleMode mode, int zoomPercent);
/// Every option a mode owns, in apply order. An empty `downscaleDefault` keeps
/// Auto as "leave mpv's default alone" rather than guessing an id.
std::vector<VideoScaleOption> videoScaleOptions(const VideoScalePolicy& policy,
                                                const std::string& downscaleDefault);
/// The name a viewer-facing control shows for a mode.
QString videoScaleModeLabel(VideoScaleMode mode);
/// The stored viewing preference, read from the application's own settings
/// store; the caller supplies the QSettings it already owns.
VideoScalePolicy videoScalePolicyFromSettings(const QSettings& settings);

/// Provenance for the legacy FPS value passed to the HUD; display metadata
/// only, and it says nothing about whether a native filter ran.
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
/// Deliberately WIDER than native::nativeStateNeedsPolling: Probe advances when
/// the producer adopts a decoded frame, and a reader that stopped at the first
/// Probe would never read the revision that names the device.
inline bool nativeSnapshotKeepsPolling(native::NativeState state) {
    return state == native::NativeState::Active || state == native::NativeState::Preparing ||
           state == native::NativeState::Probe;
}

/// Holds a session at Fit for as long as it lives. Compare borrows a session to
/// capture panes; the session's own policy is restored on release. Weakly held,
/// so a token outliving its session releases nothing.
class VideoScaleFitLock {
public:
    VideoScaleFitLock() = default;
    ~VideoScaleFitLock() { release(); }
    VideoScaleFitLock(VideoScaleFitLock&& other) noexcept
        : session_(other.session_), held_(other.held_) {
        other.held_ = false;
    }
    VideoScaleFitLock& operator=(VideoScaleFitLock&& other) noexcept {
        if (this == &other) return *this;
        release();
        session_ = other.session_;
        held_ = other.held_;
        other.held_ = false;
        return *this;
    }
    VideoScaleFitLock(const VideoScaleFitLock&) = delete;
    VideoScaleFitLock& operator=(const VideoScaleFitLock&) = delete;
    explicit operator bool() const { return !session_.isNull(); }

private:
    friend class VideoSession;
    explicit VideoScaleFitLock(QObject* session, bool held)
        : session_(session), held_(held) {}
    void release();
    QPointer<QObject> session_;
    bool held_ = false;
};

class VideoSession : public QObject {
    Q_OBJECT

public:
    /// The GUI-side budget for a player-close native settle. The stop keeps
    /// running after it; only this wait is bounded.
    static constexpr int kTeardownSettleBudgetMs = 2000;

    explicit VideoSession(QObject* parent = nullptr);
    ~VideoSession() override;

    // ── presentation host ─────────────────────────────────────────────────
    //
    // The session holds a non-owning presenter. Setting it wires the
    // presenter's notification hooks to the handlers below; the hooks are
    // outputs of the presenter, never inputs. Setting nullptr clears those
    // hooks first, and a presenter MUST detach itself this way before it is
    // destroyed: no callback may reach a destroyed session or presenter.
    void setPresenter(VideoPresenter* presenter);
    VideoPresenter* presenter() const { return presenter_; }
    /// The core this session owns, for GUI-thread reads only. Never hand this
    /// raw pointer to a renderer or across a thread: the presenter receives the
    /// shared lease below so the core outlives every render-context free.
    MpvHandle* core() const { return mpv_.get(); }
    /// Shared lifetime lease of the mpv core. The presenter holds it for as long
    /// as its render context can exist, so destroying the session first can
    /// never leave a render context freeing against a dead core. Empty before
    /// the engine exists and after the destructor released it.
    std::shared_ptr<MpvHandle> coreLease() const { return mpv_; }
    /// Immutable state for one render pass, captured on the session's thread.
    /// A renderer MUST NOT read the session while it renders.
    RenderSnapshot renderSnapshot() const;
    /// Presentation reached the target. [session thread]
    void handleFramePresented(const PresentationReport& report);
    /// The playhead the presenter saw BEFORE releasing a dying render context.
    void handleReloadPositionCaptured(double position);
    /// A render context now exists for the current GL context.
    void handleRenderContextReady(quint64 generation);
    /// The render context is gone; a later ready drives a reload or reattach.
    void handleRenderContextLost();
    /// Target size/DPR/format/presentability changed.
    void handleTargetStateChanged(const RenderTargetFacts& facts);
    /// The renderer failed with viewer-facing copy.
    void handleRenderError(const QString& message);

    // ── source ────────────────────────────────────────────────────────────
    /// Loads url. Returns false only if the engine itself could not be created;
    /// a stream that fails to open reports through errorOccurred instead.
    /// `mediaToken` is an opaque identity echoed with the load and end
    /// notifications for exactly this load; 0 names none.
    bool open(const QString& url, quint64 mediaToken = 0);
    void close();

    // ── media tracks / information ────────────────────────────────────────
    const MediaTrackSnapshot& mediaTracks() const { return mediaTracks_; }
    MediaTrackSelection mediaTrackSelection() const { return selectedMediaTracks(mediaTracks_); }
    bool selectMediaTrack(MediaTrackType type, int64_t id, uint64_t expectedCoreIdentity,
                          uint64_t expectedSourceEpoch);
    void setNextMediaTrackSelection(MediaTrackSelection selection);
    bool requestMediaInformation(std::function<void(const MediaInformation&)> callback);
    void cancelMediaInformation();

    // ── transport ─────────────────────────────────────────────────────────
    void togglePause();
    void setPaused(bool paused);
    void reassertPaused();
    bool pauseObserved() const { return observedPaused_; }
    /// Holds output paused during engine work, retaining explicit viewer intent.
    void setEnginePreparationPaused(bool active, bool restoreIntent = true);
    /// Retains one clean video snapshot through an engine/filter transition.
    /// The presenter captures it at activation and releases it only when a
    /// fresh output frame arrives after deactivation.
    void setEngineFrameHold(bool active);
    bool playbackPauseIntent() const;
    quint64 pauseRequestGeneration() const { return pauseRequestGeneration_; }
    void seek(double seconds);
    /// Seek that lands on the requested timestamp instead of the nearest
    /// keyframe. Used to put back a position the engine moved on its own.
    void seekExact(double seconds);
    void seekRelative(double delta);
    void setVolume(double volume);   // 0..100
    void setMuted(bool muted);
    void setSpeed(double speed);

    // ── enhancement chains ────────────────────────────────────────────────
    void setAnime4KMode(anime4k::Mode mode);
    void setAnimeJanaiPlan(const native::AnimeJanaiPlan& plan);
    native::AnimeJanaiSourceContext animeJanaiSourceContext() const;
    native::AnimeJanaiPlan animeJanaiPlan() const { return animeJanaiPlan_; }
    QString animeJanaiStatus() const { return animeJanaiStatus_; }
    QString animeJanaiPlanNote() const;
    bool animeJanaiEngineBuildActive() const { return animeJanaiEngineBuildActive_; }
    QString animeJanaiEngineBuildDetail() const { return animeJanaiEngineBuildDetail_; }
    bool animeJanaiProcessingObserved() const;
    anime4k::Mode anime4KMode() const { return anime4kMode_; }
    void setAnime4KChain(const QString& candidateId);
    QString anime4KChainId() const { return anime4kChainId_; }
    quint64 anime4KRequestGeneration() const { return anime4KRequestGeneration_; }
    QString effectiveShaderChain() const;

    enum class HdrToneMappingPolicy {
        Auto,
        Stable,
        Vivid
    };
    static void setHdrToneMappingPolicy(HdrToneMappingPolicy policy);
    static HdrToneMappingPolicy hdrToneMappingPolicy();
    inline static constexpr char kHdrToneMappingSettingKey[] = "playback/hdrToneMapping";
    inline static constexpr char kAnimeJanaiHdrOverrideSettingKey[] = "playback/nativeOnHdr";
    inline static constexpr char kAutoAdvanceSettingKey[] = "playback/autoAdvance";

    // ── display scaling policy ────────────────────────────────────────────
    static void setVideoScalePolicy(const VideoScalePolicy& policy);
    static VideoScalePolicy videoScalePolicy();
    static void setVideoScaleMode(VideoScaleMode mode);
    static int videoScalePolicyRevision();
    inline static constexpr char kVideoScaleModeSettingKey[] = "playback/videoScaleModeNativeDefault";
    inline static constexpr char kVideoZoomSettingKey[] = "playback/videoZoomPercent";
    inline static constexpr char kVideoUpscaleSettingKey[] = "playback/videoScale";
    inline static constexpr char kVideoDownscaleSettingKey[] = "playback/videoDownscale";

    std::optional<VideoScaleFitLock> acquireVideoScaleFitLock();
    bool compareFitHeld() const { return compareFitActive_; }

    /// The observed color profile of the current source (T-102), read from
    /// mpv's own `video-params` notifications and reset at each source epoch.
    struct SourceColorProfile {
        QString gamma;        ///< video-params/gamma ("bt.1886", "pq", "hlg", ...)
        QString primaries;   ///< video-params/primaries ("bt.709", "bt.2020", ...)
        QString pixelformat;  ///< video-params/pixelformat ("yuv420p", "p010", ...)

        bool hasTransferMetadata() const { return !gamma.isEmpty(); }
        bool isHdr() const {
            return gamma == QStringLiteral("pq") || gamma == QStringLiteral("hlg") ||
                   gamma == QStringLiteral("smpte2084");
        }
        bool hasKnownSdrTransfer() const {
            return gamma == QStringLiteral("bt.1886") || gamma == QStringLiteral("srgb") ||
                   gamma == QStringLiteral("linear") || gamma == QStringLiteral("gamma1.8") ||
                   gamma == QStringLiteral("gamma2.0") || gamma == QStringLiteral("gamma2.2") ||
                   gamma == QStringLiteral("gamma2.4") || gamma == QStringLiteral("gamma2.6") ||
                   gamma == QStringLiteral("prophoto") || gamma == QStringLiteral("st428");
        }
        bool requiresSdrGate() const { return !hasKnownSdrTransfer(); }
    };

    /// Video metadata extracted from mpv properties for UI display.
    struct VideoMetadata {
        QString videoCodec;
        QString audioCodec;
        bool isHdr = false;
        bool isDolbyVision = false;
        bool hasDolbyAtmos = false;
        QString hdrFormat;

        bool hasMetadata() const { return !videoCodec.isEmpty(); }
        void clear() { *this = VideoMetadata(); }
    };
    const SourceColorProfile& sourceColorProfile() const { return sourceColorProfile_; }

    static void setAnimeJanaiHdrOverride(bool allowed);
    static bool animeJanaiHdrOverride();
    static bool hdrRenderingFixesEnabled();

    /// Optional timing diagnostics; the application supplies the log path. The
    /// renderer shares the same process-wide storage.
    static void setFrameTimingLogPath(const QString& path);
    static QString frameTimingLogPath();

    // ── render facts, forwarded to the presenter ──────────────────────────
    // The value types are the namespace-level VideoPresenter.hpp structs; no
    // nested aliases here, so earlier declarations cannot change meaning.
    RenderTargetColorBits renderTargetColorBits() const;
    int renderTargetInternalFormat() const;
    int renderTargetDepthBitsPerChannel() const;
    bool renderTargetFormatProbed() const;
    bool isContextReady() const;
    /// Spins the event loop until the render context exists. A session that has
    /// no presenter or has not been shown never gets one.
    bool waitForContext(int timeoutMs);
    quint64 renderContextGeneration() const;
    bool hasVisibleOutputFrame() const;
    bool hasRetainedPicture() const;
    qint64 retainedPictureBytes() const;
    UiProfileCounters uiProfileCounters() const;

    /// One frame as the decoder produced it, with this session's own video
    /// filter and shader list removed for the capture and restored afterwards.
    /// The playhead returns to where it was. GUI thread only; requires a paused
    /// frame. The synchronous path is legacy; new code uses the async form.
    QImage grabUnfilteredFrame(QString* reason = nullptr);
    /// Asynchronous unfiltered capture: returns at once and completes on the
    /// GUI thread, once, with an empty reason on success. Each seek waits for
    /// a source-valid frame presentation. One capture at a time; a second call
    /// while one is in flight completes at once with a reason. If the session
    /// is destroyed mid-capture the completion never fires.
    void grabUnfilteredFrameAsync(std::function<void(QImage, QString)> completion);
    /// The video readback for consumers that must not capture chrome: the
    /// presented picture with no overlay. GUI thread only.
    QImage grabVideoFramebuffer();

    /// Probe-only property read; takes the mpv core lock, so it must not run
    /// while native work holds that lock.
    std::optional<ProbePropertyValue> probeMpvProperty(const QString& name,
                                                        ProbePropertyKind want);
    bool probeCycleMpvTrack(const QString& property);
    bool addExternalTrack(const QString& file, bool subtitle, QString* reason = nullptr);
    void setSubtitleDelay(double seconds);
    AsyncRequestId requestPlaybackTimestamp(
        std::function<void(uint64_t, std::optional<double>)> callback);

    bool isPlaying() const;
    void setPlaying(bool playing);
    double position() const;
    double duration() const;
    int videoWidth() const;
    int videoHeight() const;
    bool hasVideo() const;
    bool isOpening() const { return opening_; }
    /// Current mpv source epoch; changes whenever the loaded source is replaced.
    uint64_t sourceEpoch() const;
    QString sourceUrl() const { return url_; }
    quint64 animeJanaiGeneration() const { return animeJanaiGeneration_; }

    // ── native status ─────────────────────────────────────────────────────
    void setNativeStatusPolling(bool enabled);
    bool nativeStatusPolling() const { return nativeStatusPolling_; }
    const native::NativeStatusView& nativeStatus() const { return nativeStatus_.status(); }

    // ── native playback ───────────────────────────────────────────────────
    void armNativeProbe(native::Backend backend = native::Backend::TensorRT);
    void requestNativeAdmission(const native::PreparedAdmissionRequest& request);
    void turnNativeOff();
    void suspendNativeProcessing();
    void resumeNativeProcessing();
    void drainNativeProcessing();
    bool settleNativeForTeardown(int timeoutMs = kTeardownSettleBudgetMs);
    bool nativeTeardownSettleAbandoned() const { return nativeTeardownSettleAbandoned_; }

    /// Move-only scoped plan lock; releasing is idempotent. MUST NOT outlive
    /// the session.
    class NativePlanLock {
    public:
        NativePlanLock() = default;
        NativePlanLock(NativePlanLock&& other) noexcept;
        NativePlanLock& operator=(NativePlanLock&& other) noexcept;
        ~NativePlanLock();
        NativePlanLock(const NativePlanLock&) = delete;
        NativePlanLock& operator=(const NativePlanLock&) = delete;
        explicit operator bool() const { return !session_.isNull(); }

    private:
        friend class VideoSession;
        explicit NativePlanLock(VideoSession* session) : session_(session) {}
        void release();
        QPointer<VideoSession> session_;
    };

    std::optional<NativePlanLock> acquireNativePlanLock(QString* reasonKey = nullptr,
                                                        QString* reason = nullptr);
    bool nativePlanLocked() const { return nativePlayback_.planLocked(); }
    int queuedNativeTransitions() const { return nativePlayback_.hasQueuedTransition() ? 1 : 0; }
    const native::NativePlaybackController::Status& nativePlaybackStatus() const {
        return nativePlayback_.status();
    }
    std::optional<native::NativePreparationContext> nativePreparationContext() const;
    void requestNativeReferenceFrame(int frameIndex = 0);

    // ── diagnostics ───────────────────────────────────────────────────────
    PlaybackDiagnostics playbackDiagnostics() const;
    void setDiagnosticsPolling(bool enabled);

    /// T-207: stops (or restarts) the 50 ms state pump. Only call this for
    /// sessions nothing is actively watching.
    void setPumpSuspended(bool suspended);

signals:
    void playingChanged(bool playing);
    void mediaLoaded(quint64 mediaToken);
    void mediaEnded(quint64 mediaToken);
    void sourceEpochChanged(uint64_t epoch);
    void sourceColorProfileChanged(const mplayer::VideoSession::SourceColorProfile& profile);
    void videoMetadataChanged(const mplayer::VideoSession::VideoMetadata& metadata);
    void anime4kGated(const QString& notice);
    void videoScaleStateChanged();
    void displayScalingRefused(const QString& reason);
    void seekRequested();
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void videoSizeChanged(int width, int height);
    void displayAspectRatioChanged(double ratio);
    void streamStatsChanged(double bytesPerSecond, double framesPerSecond,
                            StreamFpsSource fpsSource);
    void errorOccurred(const QString& message);
    void mediaTracksChanged();
    void mediaInformationInvalidated();
    void mediaTracksError(const QString& reason);
    void nativeStatusChanged(const native::NativeStatusView& status);
    void nativeProcessingFailure(const QString& reasonKey, const QString& reason);
    void animeJanaiStatusChanged(const QString& status);
    void animeJanaiFailure(const QString& reason);
    void animeJanaiEngineBuildChanged(bool active, bool succeeded, const QString& detail);
    void enginePauseHoldChanged(bool held);
    void nativePlaybackStatusChanged(const native::NativePlaybackController::Status& status);
    void nativeReferenceFrameReady(const native::FrameFixture& frame, quint64 sourceEpoch,
                                   double framePosition, int frameIndex,
                                   std::optional<native::Rational> frameFps);
    void nativeReferenceFrameFailed(const QString& reasonKey, const QString& reason, int frameIndex);
    void openingChanged(bool opening);

private:
    static const char* nativePlaybackStateName(native::NativePlaybackController::State state);

    /// Creates the engine with vo=libmpv. Both open() and a context-ready
    /// session need it, and whichever runs first wins.
    void ensureEngine();
    /// Issues the deferred load now that a render context exists. `resumeAt` is
    /// the position the new file starts at: non-zero only for the internal
    /// reload of a source whose render context was lost.
    bool startPendingUrl(double resumeAt = 0.0);
    void pumpEvents();
    void publishPlayingState(bool playing);
    void handleState();
    void updateStreamStats();
    void cancelAnimeJanaiOperation();
    void processAnimeJanaiPlan();
    void beginAnimeJanaiReconfigureHold(std::function<void()> onCaptured = {});
    void settleAnimeJanaiReconfigurePosition(std::function<void()> onSettled = {});
    /// The single exact seek is not settlement: bounded async time-pos reads,
    /// each queued after the seek on mpv's own command queue (no seek here, no
    /// generic retry layer), complete the settle continuation only when the
    /// confirmed core clock is within tolerance. A source change or a viewer
    /// seek finishes immediately without seeking; deadline/read bound releases
    /// the hold and writes one timing-log diagnostic instead of blocking.
    void confirmAnimeJanaiReconfigureSettled(double target, uint64_t epoch,
                                             qint64 deadlineMs, int reads);
    /// `configurationChange`/`slotOnlyChange` are per-operation values computed
    /// BEFORE cancelAnimeJanaiOperation()/animeJanaiPlan_ replacement; they are
    /// carried into the async continuation rather than recomputed against the
    /// already-replaced plan.
    void applyAnimeJanaiPlanChange(native::Backend previousBackend,
                                   bool configurationChange,
                                   bool slotOnlyChange);
    void applyAnimeJanaiFilterPlan();
    /// Viewer-seek override: bumps animeJanaiReconfigureDiscardSerial_, clears
    /// the frozen target/epoch/generation, cancels only an in-flight settle read
    /// (which also owns the hold release), and leaves a capture read running to
    /// re-base on the viewer's position.
    void discardAnimeJanaiReconfigurePosition();
    void animeJanaiFail(const QString& reasonKey, const QString& reason);
    void setOpening(bool opening);
    void setAnimeJanaiStatus(const QString& status);
    void resetAnimeJanaiObservation();
    bool animeJanaiBackendLogsNeeded() const;
    void observeAnimeJanaiBackendLog(const QString& prefix, const QString& line);
    void reportAnimeJanaiBackendError(const QString& line);
    void publishAnimeJanaiObservation(const QString& status, uint64_t epoch,
                                      quint64 generation);
    void publishPendingAnimeJanaiObservation(uint64_t epoch, quint64 generation);
    void restoreAnimeJanaiHwdec(uint64_t epoch, quint64 generation);
    void observePlaybackState();
    void publishObservedState();
    void resetMediaTracks();
    void refreshMediaTracks();
    void pollNativeStatus();
    bool nativeStatusShouldPoll() const;
    void resetNativeStatusReader(bool reattach);
    void syncNativeSource();
    void armNativeObservation();
    void deliverNativeCommand(const native::NativePlaybackCommand& command);
    void applyNativeDecision(const native::NativePlaybackController::Decision& decision);
    void publishNativePlaybackStatus();
    void releaseNativePlanLock();
    bool nativeTeardownSettling_ = false;
    bool nativeTeardownSettleAbandoned_ = false;
    qint64 nativePlaybackNowMs() const { return nativePlaybackClock_.elapsed(); }
    void abandonNativeReferenceFrame(const QString& reasonKey, const QString& reason);
    bool probeReadActive_ = false;

    // ── explicit probe alignment ───────────────────────────────────────────
    struct NativeProbeAlignment {
        native::Backend backend = native::Backend::TensorRT;
        uint64_t epoch = 0;
        QString required;
    };
    static QString hwdecForBackend(native::Backend backend);
    struct NativeProbeRead {
        enum class Kind { None, Seekable, Hwdec, TimePos };
        Kind kind = Kind::None;
        uint64_t epoch = 0;
        std::optional<native::Backend> backend;
        bool seekable = false;
        QString hwdec;
        double timePos = 0.0;
    };
    void beginNativeProbeAlignment(const NativeProbeAlignment& alignment);
    void applyNativeProbeAlignmentReply(const NativeProbeRead& read);
    void finishNativeProbeAlignment(bool accepted, const QString& observedDecoder,
                                     QString* reasonKey, QString* reason);
    void cancelNativeProbeAlignment(const QString& reasonKey, const QString& reason);
    AsyncRequestId nativeProbeAlignRequest_;
    std::optional<NativeProbeAlignment> nativeProbeAlign_;
    bool nativeProbeAligned_ = false;
    QString nativeProbeDecoder_;
    std::optional<native::Backend> nativeProbeBackend_;

    // ── adjacent-frame capture ─────────────────────────────────────────────
    void issueNativeReferenceCapture(uint64_t epoch, int frameIndex, double position);
    void readNativeReferencePosition(uint64_t epoch, int frameIndex);
    void stepToAdjacentNativeFrame(double restore, double adjacent, int frameIndex);
    void emitNativeReferenceCaptureReady(const native::FrameFixture& frame, uint64_t epoch,
                                         double position, int frameIndex,
                                         const QByteArray& digest);
    void awaitNativeReferenceRestore(uint64_t epoch, int frameIndex, double restore,
                                     std::optional<native::Rational> cadence,
                                     std::shared_ptr<const native::FrameFixture> frame,
                                     std::optional<double> decodedPosition,
                                     std::optional<QByteArray> digest, qint64 deadlineMs);
    void pollNativeReferenceRestore(uint64_t epoch, int frameIndex, double restore,
                                    std::optional<native::Rational> cadence,
                                    std::shared_ptr<const native::FrameFixture> frame,
                                    std::optional<double> decodedPosition,
                                    std::optional<QByteArray> digest, qint64 deadlineMs,
                                    int attempts);
    void announceRestoredPosition(double confirmed);
    void refuseNativeReferenceRestore(const QString& reasonKey, const QString& reason,
                                      int frameIndex, uint64_t epoch);
    void finishNativeReferenceCapture(const QString& reasonKey, const QString& reason,
                                      int frameIndex);
    void discardNativeReferenceStep(const QString& reasonKey, const QString& reason);
    QByteArray nativeReferenceLastHash_;
    uint64_t nativeReferenceLastEpoch_ = 0;
    std::optional<double> nativeProbeResumePosition_;
    AsyncRequestId nativeReferenceRequest_;
    AsyncRequestId nativeReferenceReadRequest_;
    AsyncRequestId nativeReferenceRestoreRequest_;
    int nativeReferenceFrameIndex_ = 0;
    bool nativeReferenceStepping_ = false;
    std::optional<native::NativeWorkLease> nativeReferenceLease_;

    // ── raw diagnostics ───────────────────────────────────────────────────
    enum DiagnosticSlot { kDecoderDropSlot = 0, kVoDropSlot, kVoDelayedSlot, kVfSlot };
    static constexpr int kDiagnosticSlotCount = 4;
    struct DiagnosticRequest {
        AsyncRequestId pending;
        qint64 lastRequestMs = 0;
    };
    void pollDiagnostics();
    void resetDiagnostics();
    void issueDiagnostic(DiagnosticSlot slot, const char* property, uint64_t epoch);
    void applyDiagnosticReply(DiagnosticSlot slot, const AsyncReply& reply);
    void setDiagnosticUnknown(DiagnosticSlot slot);
    /// Appends one timing-log line through the render bridge's single writer.
    void logFrameTiming(const QString& line);

    /// Advances the published aspect, dropping anything a consumer cannot use.
    void publishDisplayAspectRatio(double ratio);

    /// The pause value submitted and not yet observed to be held by mpv.
    struct PauseWrite {
        quint64 generation = 0;
        uint64_t epoch = 0;
        bool target = false;
    };
    /// nullopt when nothing is in flight. A write mpv accepted stays here until
    /// its value is observed: a command reply can drain before the property
    /// event carrying the value, and retiring it there would let a stale event
    /// overwrite the viewer's intent.
    std::optional<PauseWrite> pendingPauseWrite_;
    quint64 pauseWriteGeneration_ = 0;
    void requestPausedWrite(bool paused);
    bool requestedPause() const {
        return pendingPauseWrite_.has_value() ? pendingPauseWrite_->target : observedPaused_;
    }

    /// P1-01(a): one async unfiltered capture shared by its phases. The phases
    /// run on the GUI thread only, chained off mpv async replies, presentation
    /// reports, position changes and a per-phase timeout - never a poll loop.
    /// The session pointer is weak: a capture abandoned by a destroyed session
    /// still completes its caller instead of hanging it.
    struct UnfilteredCapture {
        QPointer<VideoSession> session;
        std::function<void(QImage, QString)> completion;
        uint64_t epoch = 0;
        double target = 0.0;
        QString filter;
        QString shaders;
        bool clearedFilter = false;
        bool clearedShaders = false;
        int readsPending = 0;
        int setsPending = 0;
        bool readFailed = false;
        bool setFailed = false;
        double expectedPosition = 0.0;
        uint64_t framesBeforeSeek = 0;
        int waitPhase = 0;
        QPointer<QTimer> timer;
        bool finished = false;
    };
    std::shared_ptr<UnfilteredCapture> unfilteredCapture_;
    void onUnfilteredFiltersRead(const std::shared_ptr<UnfilteredCapture>& state);
    void onUnfilteredFiltersCleared(const std::shared_ptr<UnfilteredCapture>& state);
    void waitUnfilteredFrame(const std::shared_ptr<UnfilteredCapture>& state,
                             double seekPosition);
    void checkUnfilteredFramePresented(const std::shared_ptr<UnfilteredCapture>& state);
    void captureUnfilteredFrame(const std::shared_ptr<UnfilteredCapture>& state);
    void finishUnfilteredCapture(const std::shared_ptr<UnfilteredCapture>& state,
                                 const QImage& image, const QString& reason);
    void restoreUnfilteredFilters(const std::shared_ptr<UnfilteredCapture>& state);
    bool unfilteredCaptureActive(const std::shared_ptr<UnfilteredCapture>& state) const;

    /// Asks a recreated render context, over a bounded time window, whether the
    /// live core still delivers output; a reload happens only when it does not.
    void beginContextReattach();
    void finishContextReattach();
    /// Keeps the picture the viewer is looking at across a source change.
    void holdPictureAcrossLoad();
    /// Forces one render-state advance and repaint of the current picture, for
    /// a chain swap under a possibly paused core; forwards to the presenter.
    void refreshRenderState();
    /// The offset an internal reload of the current source resumes at, or 0.
    double resumePositionForReload() const {
        return pendingResumePositionValid_ ? pendingResumePosition_ : 0.0;
    }
    /// Issues one core command without waiting for it. `reportRefusal` names a
    /// refusal through errorOccurred; `mediaToken` marks the command as a media
    /// load so a refused load is retired from the pairing queue.
    void issueCoreCommand(std::vector<std::string> args, const char* what, bool reportRefusal,
                          std::optional<quint64> mediaToken = std::nullopt);
    /// Submits one path list to glsl-shaders asynchronously.
    void applyShaderChainRequest(const QString& shaders);
    QString resolvedChainForCurrentSource(const QString& candidateId,
                                          QString* reason = nullptr) const;
    void handleSourceColorProfileChanged();
    void updateVideoMetadata();
    void enforceAnimeJanaiHdrGate();
    void applyHdrPolicyToEngine();
    void applyVideoScaleToEngine(bool queryCoreDefault = false);
    std::optional<std::string> downscaleDefaultForCore();
    /// Announces enginePauseHeld() when it differs from what was last published.
    void publishEnginePauseHold();
    bool enginePauseHeld() const;
    void beginEnginePause();
    void endEnginePause(bool restoreIntent);
    void discardEnginePause();
    void pollAnimeJanaiEngineBuild();
    void finishAnimeJanaiEngineBuild(bool succeeded, const QString& detail);
    void removeAnimeJanaiFilters(uint64_t epoch, quint64 generation,
                                 std::function<void()> completion);
    /// Wires the presenter's notification hooks to this session's handlers.
    void attachPresenterHooks();

    // ── core / source ─────────────────────────────────────────────────────
    /// Shared so the render bridge can hold a lease: the render context is freed
    /// BEFORE the last core release, even when the view outlives the session.
    std::shared_ptr<MpvHandle> mpv_;
    VideoPresenter* presenter_ = nullptr;
    double requestedVolume_ = 100.0;
    bool requestedMuted_ = false;
    uint64_t mpvApiVersion_ = 0;
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
    /// Playhead captured when a plan reconfigure starts, and put back once the
    /// engine stops moving it (hwdec re-init re-seeks to a keyframe).
    double animeJanaiReconfigurePosition_ = 0.0;
    bool animeJanaiReconfigurePositionValid_ = false;
    /// The confirmed core-clock read that captures the reconfigure playhead.
    AsyncRequestId animeJanaiReconfigureRequest_;
    uint64_t animeJanaiReconfigureEpoch_ = 0;
    quint64 animeJanaiReconfigureGeneration_ = 0;
    /// Bumped by discardAnimeJanaiReconfigurePosition(): a capture reply that
    /// sees it moved (same source/generation) suppresses the automatic playhead
    /// restore for this operation instead of storing a pre-seek target.
    quint64 animeJanaiReconfigureDiscardSerial_ = 0;
    /// Set by that capture reply; the configuration then applies the chain with
    /// no target, the hold is released, and no corrective seek is issued. A new
    /// setAnimeJanaiPlan() request clears it so a later reconfigure captures.
    bool animeJanaiReconfigureRestoreSuppressed_ = false;
    /// Continuation released only after the settle has decided the position.
    std::function<void()> animeJanaiReconfigureSettleDone_;
    bool animeJanaiFilterBound_ = false;
    bool animeJanaiBusy_ = false;
    bool animeJanaiReconfigureAfterRestore_ = false;
    QStringList animeJanaiBoundFilterLabels_;
    std::shared_ptr<native::AnimeJanaiStageBuildLease> animeJanaiStageBuildLease_;
    bool animeJanaiFailureCleanupActive_ = false;

    // mpv keeps running whether or not the view repaints, so the state pump
    // cannot live in a paint: a paused video produces no frames.
    QTimer statePump_;
    QElapsedTimer statsClock_;
    /// Monotonic, elapsed-from-construction: the reader's cadence must not be
    /// read off a wall clock that can step.
    QElapsedTimer nativeStatusClock_;

    bool enginePauseHoldPublished_ = false;
    /// Output geometry mpv reports after its filters: aspect-corrected size and
    /// the intended clockwise rotation not yet applied to that output.
    double observedOutputWidth_ = 0.0;
    double observedOutputHeight_ = 0.0;
    double observedOutputRotation_ = 0.0;
    /// Last published display aspect; 0 when no usable geometry is known.
    double displayAspectRatio_ = 0.0;

    // The one native-status reader this session owns, bound to its own core.
    native::NativeStatusConsumer nativeStatus_;
    bool nativeStatusPolling_ = false;
    bool nativeStatusHalted_ = false;
    bool nativeSnapshotSeen_ = false;

    // The playback transaction owner: it never touches the core itself, and the
    // session is its only sink.
    native::NativePlaybackController nativePlayback_;
    QElapsedTimer nativePlaybackClock_;
    bool nativeSourceActive_ = false;
    uint64_t nativeBoundEpoch_ = 0;
    bool nativeFilterBound_ = false;
    bool nativeStatusPublished_ = false;
    native::NativePlaybackController::Status lastNativeStatus_;

    QElapsedTimer diagnosticsClock_;
    DiagnosticRequest diagnosticRequests_[kDiagnosticSlotCount];
    qint64 diagnosticsLastPollMs_ = 0;
    bool diagnosticsActive_ = false;
    /// Probes keep the constructor default (on) because their verdicts assert
    /// on the polled counters; the shipped app switches it off per session.
    bool diagnosticsPolling_ = true;
    PlaybackDiagnostics diagnostics_;

    /// A load is queued because there is no render context yet.
    bool pendingOpen_ = false;
    /// A source has been opened and its first frame has not been presented yet.
    bool opening_ = false;
    /// A source is loaded and the reattach window is being observed.
    bool reattachPending_ = false;
    int reattachTicksLeft_ = 0;
    QTimer reattachTimer_;

    /// The offset an internal reload of the SAME source resumes at, captured
    /// while the outgoing video output was still trustworthy.
    double pendingResumePosition_ = 0.0;
    bool pendingResumePositionValid_ = false;
    /// Set once the render context is gone, so a playback error that follows is
    /// marked as the renderer's doing rather than the source's.
    bool rendererTeardownErrorPending_ = false;
    /// A renderer that fails on every pass reports once per failing streak.
    bool renderErrorReported_ = false;
    /// Mirrors the hold the presenter was asked for, for renderSnapshot().
    bool engineFrameHoldRequested_ = false;

    /// Playlist entry from the last START_FILE, used to reject stale END_FILE.
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
    std::vector<AsyncRequestId> mediaInformationRequests_;
    uint64_t mediaInformationGeneration_ = 0;
    bool mediaTracksLoaded_ = false;
    bool mediaTracksDirty_ = false;
    std::optional<MediaTrackSelection> nextMediaTrackSelection_;
    MediaTrackSelection pendingMediaTrackSelection_;
    quint64 entryMediaToken_ = 0;
    bool entryLoadReported_ = false;
    quint64 mediaToken_ = 0;
    /// Monotonic count of composited presentations, for capture-frame waits.
    quint64 presentedFrames_ = 0;

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
    bool observedIdle_ = true;
    double observedVideoWidth_ = 0.0;
    double observedVideoHeight_ = 0.0;
    double observedCacheSpeed_ = 0.0;
    double observedContainerFps_ = 0.0;
    QString shaderChain_;
    int appliedHdrPolicyRevision_ = 0;
    int appliedAnimeJanaiOverrideRevision_ = 0;
    friend class VideoScaleFitLock;
    int appliedVideoScaleRevision_ = 0;
    std::optional<std::string> downscaleDefault_;
    std::optional<std::string> downscaleDefaultError_;
    bool downscaleWasExplicit_ = false;
    quint64 videoScaleBatch_ = 0;
    int videoScaleBatchPending_ = 0;
    bool compareFitActive_ = false;
    SourceColorProfile sourceColorProfile_;
    VideoMetadata videoMetadata_;
    SourceColorProfile observedColorProfile_;
    bool animeJanaiSourceGated_ = false;
    /// The subscriptions this session owns, released before the core.
    std::vector<uint64_t> observers_;
    /// Last target facts pushed by the presenter, for the read-only forwarders.
    RenderTargetFacts lastTargetFacts_;
};

}  // namespace mplayer

// Declared exactly once, here: the change signal carries it by const reference
// and a queued connection needs the type known to the meta-object system.
Q_DECLARE_METATYPE(mplayer::native::NativeStatusView)
Q_DECLARE_METATYPE(mplayer::native::NativePlaybackController::Status)
Q_DECLARE_METATYPE(mplayer::StreamFpsSource)
Q_DECLARE_METATYPE(mplayer::VideoSession::SourceColorProfile)
