#pragma once

#include <QDialog>
#include <QEvent>
#include <QList>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

QT_BEGIN_NAMESPACE
class QCloseEvent;
class QFrame;
class QGraphicsOpacityEffect;
class QKeyEvent;
class QLabel;
class QMouseEvent;
class QMoveEvent;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QScreen;
class QSlider;
class QTimer;
class QVariantAnimation;
class QWidget;
QT_END_NAMESPACE

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include "Anime4K.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"
#include "ProcessingSettingsIntent.hpp"
#include "native/prepare/NativeFrameFixture.hpp"
#include "native/status/NativeMenu.hpp"
#include "native/status/NativePlaybackController.hpp"
#include "native/prepare/NativePreparationContext.hpp"

namespace mplayer {
class VideoSurface;
}

namespace mplayer {
namespace shell {

class CompareWindow;

class PlayerShell;
class MenuPopup;
class ProcessingOverlay;
class SeriesDrawer;
struct MenuEntry;

/// One playable height. A quality map entry survives only when its url is a
/// non-blank string, which is what `usableQualities` enforces.
struct QualityEntry {
    int height = 0;
    QString url;
};

/// Everything the dialog cannot do for itself. Implemented by the C API layer
/// and non-owning: it must outlive the dialog.
class PlayerHost {
public:
    virtual ~PlayerHost() = default;
    virtual void requestVariant(const QString& embed, const QString& label) = 0;
    virtual void requestEpisode(int index) = 0;
    virtual void requestTrackProbe(const QString& embed) = 0;
    virtual void requestOpenOriginal() = 0;
    virtual void requestCopyStreamUrl() = 0;
    virtual void playerClosed() = 0;
    /// The stream could not be played: the host re-extracts and reloads.
    virtual void playbackFailed() = 0;
};

/// A frameless always-on-top float showing the same stream in its own
/// VideoSurface: mpv gives one core per surface, so the float decodes a second
/// time rather than sharing a handle it cannot have.
///
/// It is placed FREELY. A drag drops it exactly where the pointer leaves it -
/// no snap-to-corner on release, no re-pin to a corner on resize, show, or
/// re-entry. The single placement rule is reachability: some part of the float
/// must share pixels with a work area, or it cannot be clicked or dragged back.
/// A pooled float re-enters at the exact position and size the viewer last left
/// it; only an unreachable position (a monitor that went away) is corrected.
class PipFloat : public QWidget {
    Q_OBJECT

public:
    explicit PipFloat(QWidget* parent = nullptr);

    VideoSurface* videoSurface() const { return surface_; }
    /// Opens `url` and seeks to `position` once the stream is up.
    void openStream(const QString& url, double position, bool paused);
    /// Places a brand-new float against a screen work area, bottom-right by
    /// default. Used only for the float's first-ever placement: a pooled float
    /// keeps the exact position and size the viewer last left it, so this is
    /// never called on re-entry.
    void moveToCorner(QScreen* target);
    void closeStream();

    void setPlaying(bool playing);
    /// Display only: nothing is emitted, so answering with a seek could loop.
    void setTime(double position, double duration);
    void setMuted(bool muted);
    /// 0…1, matching the shell. The float runs a second mpv core, so the
    /// level has to be pushed to it rather than read off the main player.
    void setVolume(double volume);

    /// Where the float's own core is. The host reads it to restore the main
    /// player on exit, so both windows resume from what the viewer watched.
    double position() const;
    double duration() const;
    bool isPlaying() const;

signals:
    void muteChanged(bool muted);
    void volumeChanged(double volume);
    void restoreRequested();
    void closeRequested();

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildChrome();
    QPushButton* buildButton(const QString& name, const QString& hint, qreal size,
                                   QWidget* parent = nullptr);
    void updateVolumeChip();
    /// A press on the timeline seeks there and starts scrubbing. The
    /// overlay claims presses before the slider sees them, so this is the
    /// only path to click-to-seek.
    void timelinePress(QMouseEvent* event);
    /// Seeks the float's own core to the timeline position under `x`, in
    /// window coordinates, and moves the handle to match.
    void timelineSeekTo(double x);

    /// True when any part of the float overlaps any screen's work area. The
    /// float is placed freely, so this is the only placement rule: a float that
    /// shares no pixels with a work area cannot be clicked or dragged back.
    bool overlapsAnyWorkArea() const;
    /// Pulls the float back onto the nearest work area, keeping as much of its
    /// position as reachability allows. After a monitor change the pooled
    /// position can name a screen that is no longer there, and a window there
    /// cannot be clicked at all. Only the unreachable case is corrected; a
    /// viewer's chosen position is never overridden.
    void pullIntoWorkArea();
    void pointerPress(QMouseEvent* event);
    void pointerMove(QMouseEvent* event);
    void pointerRelease(QMouseEvent* event);
    bool handleKey(QKeyEvent* event);
    void fitSurfaces();
    void applyCornerMask();
    void revealControls();
    void hideControls();
    void setOverlayOpacity(double value);

    VideoSurface* surface_ = nullptr;
    QWidget* overlay_ = nullptr;
    QPushButton* muteButton_ = nullptr;
    QWidget* volumeChip_ = nullptr;
    QSlider* volumeSlider_ = nullptr;
    QPushButton* restoreButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
    QPushButton* playButton_ = nullptr;
    QSlider* scrub_ = nullptr;
    QLabel* elapsed_ = nullptr;
    QLabel* remaining_ = nullptr;

    QGraphicsOpacityEffect* overlayEffect_ = nullptr;
    QVariantAnimation* overlayFade_ = nullptr;
    QTimer* hideTimer_ = nullptr;

    double overlayOpacity_ = 1.0;
    double pendingSeek_ = -1.0;
    std::optional<bool> pendingLoadPause_;
    quint64 pendingLoadPauseGeneration_ = 0;
    bool muted_ = false;
    double volume_ = 0.7;
    bool draggingVolume_ = false;
    bool draggingTimeline_ = false;
    bool scrubbing_ = false;
    /// True while the pointer is over the float. The auto-hide has to stand
    /// down: fading the controls out from under a pointer that is reaching for
    /// one is the difference between a control and a guessing game.
    bool hovered_ = false;

    QPointF pressGlobal_;
    QPoint pressWindow_;
    QSize pressSize_;
    bool resizingRight_ = false;
    bool resizingBottom_ = false;
    bool pressed_ = false;
    bool dragging_ = false;
};

/// The composition root: hosts the shell, translates its intent to the engine
/// and carries the three panels the shell has no room for.
class PlayerDialog : public QDialog {
    Q_OBJECT

public:
    PlayerDialog(const QString& caption, const QString& url, QWidget* parent = nullptr);
    PlayerDialog(const QString& caption,
                 const QString& url,
                 const QList<QualityEntry>& qualities,
                 int initialHeight,
                 PlayerHost* host,
                 QWidget* parent = nullptr);
    ~PlayerDialog() override;

    // ── host → dialog ────────────────────────────────────────────────
    /// Steps to another episode in place: same window, same position.
    void applyEpisode(const QString& label,
                      const QString& embed,
                      int index,
                      const QList<QualityEntry>& qualities,
                      const QVector<QVariantMap>& variants);
    /// Swaps the dub/sub track; playback resumes at the same position.
    void applyVariant(const QList<QualityEntry>& qualities,
                      const QString& label,
                      const QString& embed = QString());
    /// Reloads from a re-extracted quality map after a failed stream load.
    void applyRefreshedStreams(const QList<QualityEntry>& qualities, int height = 0);
    void setPlaylist(const QVector<QVariantMap>& playlist, int index = 0);
    /// Publishes the dub/sub variants the popover lists and which embed is
    /// current. The track chip only appears once there is something to pick.
    void setVariants(const QVector<QVariantMap>& variants, const QString& currentEmbed);
    void stepEpisode(int delta);
    /// Badges one track with the quality a probe found for it.
    void setTrackQuality(const QString& embed, int quality);
    void showError(const QString& message);
    /// A requested AnimeJaNai preset is saved by stable id and prepared off the
    /// GUI thread; unavailable prerequisites remain visible as a failed request.
    void applyAnimeJanaiRequest(const QString& id);
    void applyAnimeJanaiBackend(const QString& backend);
    /// Monotonic generation of the last AnimeJaNai preparation result installed
    /// by this dialog's worker. Probe callers can distinguish a fresh async
    /// completion from a retained status for an older request.
    quint64 animeJanaiPlanGeneration() const { return animeJanaiPlanGeneration_; }
    /// One path a native request change takes from a menu row. The action is
    /// typed rather than spelled, so callers share one vocabulary. The request
    /// is persisted even when it cannot be applied, the effective value is
    /// re-derived from the filter's snapshot, and nothing here touches the URL,
    /// the stream, the quality cache or the Anime4K chain.
    void applyNativeRequest(native::menu::Action action, const QString& value);
    /// A native preparation or engine failure. It is separated from a media
    /// failure on purpose: it shows its own reason, offers no stream retry, and
    /// never re-extracts a signed URL or re-probes a quality.
    void reportNativeProcessingFailure(const QString& reason);
    /// The named form the surface's `nativeProcessingFailure` carries. A bare
    /// refusal still names itself when the reason text is empty.
    void reportNativeProcessingFailure(const QString& reasonKey, const QString& reason);
    /// Submits a whole resolved admission to the core that owns playback right
    /// now - the float's while PiP is up, the main core's otherwise. The epoch is
    /// stamped from that core's own observed source epoch before it goes, and a
    /// refused replacement leaves the previous plan exactly where it was.
    void submitNativeAdmission(const native::PreparedAdmissionRequest& request);
    /// Replaces the shell's four native inputs (request, producer availability,
    /// selected runtime, the filter's own status) with what the core that owns
    /// playback right now actually reported - the float's own while PiP is up.
    /// The availability is the pure projection of that core's observed factory,
    /// so a core with no document for its current source names the roles
    /// unavailable rather than inheriting another core's. This is the ownership
    /// path's half: it never announces, because a request sent while a claim
    /// hand-off is still moving would be refused on the lease the outgoing core
    /// has not released yet.
    void pushNativeInputs();
    /// `pushNativeInputs()` plus a request announcement. The host calls this
    /// after the settings dialog is accepted or an action changes the stored
    /// request; a status change arrives through `VideoSurface::nativeStatusChanged`
    /// instead.
    void refreshNativeInputs();
    /// Re-reads every stored player preference and installs what changed: the
    /// Anime4K preset bindings and stored native request, then the native inputs
    /// plus the deduplicated request
    /// announcement. This is what an application owner calls after a settings
    /// save is accepted, because the store is shared and only the running
    /// process can pick the save up. It is idempotent, so a save that changed
    /// nothing announces nothing; it never reopens the stream, never calls the
    /// API, never touches the quality cache and never replaces an effective
    /// native plan.
    void refreshStoredSettings();
    /// Opens the first stream; the engine exists from the shell's surface.
    void startPlayer();

    bool isPipActive() const;
    /// True while the core that owns playback - the float's while PiP is up -
    /// actually has a video loaded. The dialog hides itself for PiP, so this,
    /// not isVisible(), is what says it still owns a running stream.
    bool hasActiveSource() const;
    /// The observed source/device facts of the core that owns playback right
    /// now, for the preparation path. Nullopt - never a guessed UUID, cadence or
    /// module path - whenever that core has not actually observed them. Forwarded
    /// to the float's surface while PiP owns playback.
    std::optional<native::NativePreparationContext> nativePreparationContext() const;
    /// Arms the probe binding on the core that owns playback, so the producer can
    /// publish the observed source and device the preparation path needs. Armed
    /// only by an explicit native or preparation request - never at startup - and
    /// idempotent, so a repeated call costs nothing. `backend` is the DESIRED
    /// backend and is passed through to the core's alignment, so a preparation
    /// made from an unsaved provider choice aligns to that provider's decoder
    /// rather than to whatever the stored preference named.
    void armNativeProbe(native::Backend backend = native::Backend::TensorRT);
    /// Requests one actual decoded source frame from the core that owns playback
    /// right now, for the preparation reference path. Asynchronous, and refused
    /// by the surface while a native plan could be what the chain holds or while
    /// a preparation build holds the shared work lease.
    ///
    /// `frameIndex` is 0 for the first observation and 1 for the adjacent second
    /// of a temporal pair. Index 1 is a REAL neighbouring source frame, obtained
    /// at the observed source cadence and followed by a restore to the position
    /// playback was parked at; it is never the first frame captured twice, and a
    /// source that cannot produce a genuine neighbour is refused by name.
    void requestNativeReferenceFrame(int frameIndex = 0);
    /// The height the shell is currently labelled with, 0 when unknown.
    int currentHeight() const { return currentHeight_; }
    /// The embed the track popup marks as current, for the host to read back.
    QString currentEmbed() const { return currentEmbed_; }

    void toggleSeries();
    void setSeriesExpanded(bool expanded);
    /// The dub and subtitle pickers are separate, so a viewer picks what they
    /// want by kind instead of reading one mixed list.
    void openAudioMenu();
    void openSubtitlesMenu();
    void togglePip();

    /// Shows real engine work and holds the playback owner paused until end.
    /// Explicit play/pause requests are retained without advancing the video.
    void beginEnginePreparation(const QString& title);
    void updateEnginePreparation(int stageIndex, int stageCount, const QString& phase,
                                 const QString& detail);
    void endEnginePreparation(bool ok, const QString& summary);
    /// True while the preparation window is up.
    bool enginePreparationVisible() const;

signals:
    /// The viewer pressed Cancel on the preparation window. The host owns the
    /// build, so it is the host that decides whether the cancellation reaches it.
    void enginePreparationCancelRequested();
    /// The engine could not be created at all: the host should provision the
    /// mpv runtime and call startPlayer() again.
    void runtimeRequested();
    /// The viewer asked for native processing, and the core that owns playback
    /// has actually observed its own source and device. The assembly layer
    /// resolves this into a whole prepared admission and answers with
    /// submitNativeAdmission(). Never emitted from a preference alone, and never
    /// twice for the same observation.
    void nativeAdmissionRequested(const native::SelectionState& selection,
                                  const native::NativePreparationContext& context);
    /// The selection or the owning core changed while an assembly was in flight.
    /// Any request being resolved for the previous generation is stale.
    void nativeAdmissionCancelled();
    /// One captured reference frame, exactly as the core that owns playback
    /// returned it. `framePosition` is where playback actually was when the core
    /// answered, which is what a caller restores afterwards; `frameFps` is the
    /// observed source cadence, absent when it was never observed rather than
    /// defaulted to a guessed rate.
    void nativeReferenceFrameReady(const native::FrameFixture& frame, quint64 sourceEpoch,
                                   double framePosition, int frameIndex,
                                   std::optional<native::Rational> frameFps);
    /// The capture could not be produced, named, for `frameIndex`. Never a
    /// fabricated frame. The index is carried so a caller can drop a refusal that
    /// belongs to a request it has already abandoned.
    void nativeReferenceFrameFailed(const QString& reasonKey, const QString& reason,
                                    int frameIndex);
    /// A processing-failure remedy that needs the application to open its own
    /// existing Settings surface at a named place. The dialog never re-extracts a
    /// URL, never re-probes a quality and never starts preparation itself:
    /// Original needs no host at all and is answered inside the dialog.
    void processingSettingsRequested(ProcessingSettingsIntent intent);
    /// AnimeJaNai backend is missing the app-owned installation record.
    void componentSetupRequested();

protected:
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct VariantInfo {
        QString embed;
        QString label;
        QString display;
        QString track;
        QString kind;       // "voice" or "sub", straight from the release
        QString kindLabel;
        int quality = 0;
    };

    void copyStreamUrl();
    void buildStrip();
    void buildStatus();
    void connectShell();
    void connectEngine();

    void layoutPanels();
    void updateSeriesToggle();
    void publishQualityOptions();
    void syncStatusVisibility();
    void syncPipBar();
    void flashStatus(const QString& message, bool failure = false);
    void showErrorStatus(const QString& message);
    /// Central ownership for a failed open: keeps the real reason
    /// VideoSurface::open emitted synchronously through onEngineError and
    /// shows the generic text only when nothing was emitted. One-shot host
    /// reload guard plus runtimeRequested, so callers do nothing on false.
    void ensureOpenFailureVisible();
    /// A processing status: a named reason with no retry chip and no host
    /// callback. Used for a native refusal and for a native failure, never for a
    /// media load failure.
    void showProcessingStatus(const QString& reason);
    /// The actionable form of a processing failure: the named reason plus the
    /// three remedies that can actually change it. Kept separate from
    /// showProcessingStatus(), which is the generic native-status line that
    /// progress and an acknowledged Off also use - so remedies are never shown
    /// for a status that is not a failure, and a status update never retires
    /// remedies that are still the viewer's to act on.
    void showProcessingFailure(const QString& reasonKey, const QString& reason);
    /// True while this failure row is up. The remedies live exactly as long as
    /// it does; nothing else in the status panel consults it.
    bool hasProcessingFailure() const { return processingActive_; }
    void onProcessingPrepareRequested();
    void onProcessingProvidersRequested();
    void onProcessingOriginalRequested();
    /// True while the core that owns playback holds a confirmed admission or an
    /// effective plan. A refused replacement kept that plan, so "the Original
    /// picture" must not be claimed in that case.
    bool nativeEffectiveActive() const;
    /// States what is actually keeping the picture on screen.
    QString processingFailureMessage(const QString& reason) const;
    /// Retires only the processing row, leaving an outstanding media error and
    /// its retry chip exactly where they are.
    void retireProcessingRow();
    void retryPlayback();
    void watchPanels();
    void placeWindowed();
    void rememberWindowedGeometry();
    void fillQualityMenu();
    void probeUnknownTracks();
    void setVariantsVisibility();
    /// Rows for one kind of track, or every kind when `kind` is empty.
    QVector<MenuEntry> trackEntries(const QString& kind) const;
    static QString trackRowText(const VariantInfo& variant);
    static QList<QualityEntry> usableQualities(const QList<QualityEntry>& qualities);
    static VariantInfo variantFromMap(const QVariantMap& map);

    void onQualityPicked(const QVariant& payload);
    void onTrackPicked(const QVariant& payload);
    void onMediaDropped(const QString& path);
    /// Puts the frame currently on screen on the clipboard as an image.
    void copyCurrentFrame();
    /// Opens (or raises) the shader comparison window.
    void openCompareWindow();
    void onShellStateChanged(const QString& state);
    void onShellFullscreenChanged(bool fullscreen);
    /// A native row was chosen in the enhancement tree. One handler serves the
    /// menu, the settings dialog and the toggle, so the three cannot disagree
    /// about what was asked for.
    void onNativeActionRequested(native::menu::Action action, const QString& value);
    void onAnimeJanaiStatusChanged(const QString& status);
    void onAnimeJanaiFailure(const QString& reason);
    void onAnimeJanaiEngineBuildChanged(bool active, bool succeeded, const QString& detail);
    /// The filter's own status changed. The held inputs, the HUD and the
    /// availability follow it, and nothing here re-arms a poll the surface has
    /// already stopped. A document from a core that no longer owns playback is
    /// dropped: it says nothing about the source the viewer is watching.
    void onNativeStatusChanged(const native::NativeStatusView& status);
    /// The core's own transaction state changed. This is the acknowledgement
    /// channel for a PiP hand-off: the transition waits for the suspending core
    /// to settle here instead of polling a flag.
    void onNativePlaybackStatusChanged(const native::NativePlaybackController::Status& status);
    /// A native failure from a core's controller. During a PiP hand-off it
    /// settles the transition; otherwise it is reported as a processing failure
    /// that never touches the media path.
    void onNativeProcessingFailure(const QString& reasonKey, const QString& reason);
    /// The surface that owns playback now: the float's while PiP is up.
    VideoSurface* playbackSurface() const;
    /// True while the stored request asks for native processing. The request is
    /// never treated as effective; this only decides whether a suspend/resume
    /// hand-off is worth arming.
    bool nativeRequestEnabled() const;
    /// Announces the stored request to the assembly layer, but only when the
    /// owning core has ACTUALLY observed its source and device, and only when
    /// one of those facts or the selection is new. It never builds a plan: the
    /// assembly runs where the owned model store, the measured build identity
    /// and the observed runtime backends live.
    void announceNativeRequest();
    /// Activates native processing on the float's own core, once the main core
    /// has released the single process-wide inference claim.
    void activateFloatNative();
    /// Runs the deferred half of a PiP hand-off after the suspending core
    /// settled, or after it failed. A hand-off never waits forever.
    void finishNativeHandoff();
    void onNativeReferenceFrameReady(const native::FrameFixture& frame, quint64 sourceEpoch,
                                     double framePosition, int frameIndex,
                                     std::optional<native::Rational> frameFps);
    void onNativeReferenceFrameFailed(const QString& reasonKey, const QString& reason,
                                      int frameIndex);
    /// Names a capture that can no longer arrive - the owning core changed or
    /// went away - so a caller never waits on a reply from another core.
    void cancelPendingReferenceCapture(const QString& reasonKey, const QString& reason);
    /// Pushes the polling demand at the surface: the request is on and a core
    /// exists to read from. A demand, not a timer - the surface issues at most
    /// one outstanding request and halts itself after a terminal state or an
    /// absent filter.
    void syncNativeStatusPolling(bool requested);
    void onEngineError(const QString& message);
    void onEngineDuration(double seconds);
    void onEnginePosition(double seconds);
    void onEnginePlaying(bool playing);
    void onEngineVideoSize(int width, int height);
    /// Loads `url` and restores the position, pause, volume and speed the
    /// surface was carrying, so a stream swap continues instead of restarting.
    /// `height` is the stream's height in pixels, 0 when the caller has none:
    /// the label shown to the viewer is derived from it, never parsed back out
    /// of a "1080p" string. Returns false when the engine itself could not be
    /// created, which is the host's cue to provision the mpv runtime.
    bool openStream(const QString& url, int height, bool resume = true);
    /// Applies the saved resume point once the new stream reports a timeline.
    void restoreResumePoint();
    void applyPendingPause();
    void goToEpisode(int position, bool closeList = false);
    void setQualities(const QList<QualityEntry>& qualities);
    /// Picks a height: the requested one when it is on offer, else the best.
    int pickTargetHeight(int requested) const;
    bool onShellKey(QKeyEvent* event);
    /// Reads the stored Anime4K preset bindings and installs one window shortcut
    /// per usable one. Empty, unparsable, reserved and already-taken sequences
    /// are skipped rather than installed: a binding is a preference that can be
    /// stale, and a stale one must never take a key the player owns.
    void loadPresetShortcuts();
    QPoint settingsAnchor() const;
    void enterPip();
    void leavePip(bool closing = false);
    /// Acknowledges the parked float's native stop while its core and GL context
    /// are still alive, so releasePipPool() never destroys a pending transition.
    /// False when a stop never acknowledged within its bound.
    bool settleParkedNative();
    /// Destroys the pooled float, whichever state PiP is in. The float is
    /// parentless, so the dialog is the only thing that can release it.
    void releasePipPool();

    PlayerShell* shell_ = nullptr;
    VideoSurface* surface_ = nullptr;
    PlayerHost* host_ = nullptr;
    bool directMedia_ = false;

    MenuPopup* audioPopup_ = nullptr;
    MenuPopup* subtitlesPopup_ = nullptr;
    SeriesDrawer* seriesDrawer_ = nullptr;
    ProcessingOverlay* processingOverlay_ = nullptr;
    QPointer<VideoSurface> animeJanaiBuildingSurface_;
    QPointer<VideoSurface> enginePreparationSurface_;
    uint64_t enginePreparationSourceEpoch_ = 0;
    bool enginePreparationActive_ = false;
    void moveEnginePreparationPause(VideoSurface* owner);
    QWidget* strip_ = nullptr;
    QFrame* statusPanel_ = nullptr;
    QPushButton* seriesButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* retryButton_ = nullptr;
    QPushButton* processingPrepareButton_ = nullptr;
    QPushButton* processingProvidersButton_ = nullptr;
    QPushButton* processingOriginalButton_ = nullptr;
    QTimer* statusTimer_ = nullptr;
    /// The float on screen, or null when PiP is down. While PiP is up it names
    /// the same object as pipPool_; releasePipPool() is the one place either is
    /// destroyed, and it goes through the pool.
    PipFloat* pip_ = nullptr;

    QString caption_;
    QString url_;
    QList<QualityEntry> qualities_;
    QVector<VariantInfo> variants_;
    QVector<QVariantMap> playlist_;
    QString currentEmbed_;
    int currentHeight_ = 0;
    int episodeIndex_ = 0;
    double aspect_ = 16.0 / 9.0;
    double volume_ = 0.7;      // 0..1, what the shell shows
    double speed_ = 1.0;
    anime4k::Mode anime4kMode_ = anime4k::Mode::Off;
    /// The applied chain as a candidate id. A chain with no Mode still has to
    /// survive a restart, and an enum ordinal cannot name one.
    QString anime4kChainId_ = QStringLiteral("off");
    QString animeJanaiPresetId_;
    native::Backend animeJanaiBackend_ = native::Backend::TensorRT;
    native::AnimeJanaiPlan animeJanaiPlan_;
    quint64 animeJanaiSequence_ = 0;
    quint64 animeJanaiPlanGeneration_ = 0;
    std::shared_ptr<std::atomic_bool> animeJanaiCancellation_;
    /// The comparison window, created on first use and kept: it borrows the
    /// surface, and two of them would fight over the same core.
    CompareWindow* compare_ = nullptr;
    bool muted_ = false;
    QRect windowedSize_;
    bool fullscreen_ = true;
    bool transitioning_ = false;
    bool seriesExpanded_ = false;
    /// Processing-row liveness, deliberately separate from errorActive_: a
    /// processing failure never clears a media error's state or hides its retry
    /// chip, and a media failure never clears these remedies.
    bool processingActive_ = false;
    QString lastProcessingReasonKey_;
    QString lastProcessingReason_;
    bool componentSetupRequestedOnce_ = false;
    bool errorActive_ = false;
    bool refreshAttempted_ = false;
    bool closed_ = false;
    bool forwardingKey_ = false;
    bool pipWasFullscreen_ = false;
    /// Set for the whole of closeEvent: the native settle spins the event loop,
    /// so a reentrant close must not tear the same cores down twice.
    bool shuttingDown_ = false;
    /// True once closeEvent has really finished its stop and accepted the event.
    /// The owner closes the dialog again from `playerClosed()` (the page disposes
    /// the session), and that second close must be accepted, not refused.
    bool shutdownComplete_ = false;
    /// The native half of a PiP hand-off. There is one process-wide inference
    /// claim, so the incoming core may only admit after the outgoing core's
    /// suspend is acknowledged; this names which half is waiting.
    enum class NativeHandoff { None, SuspendingMain, SuspendingFloat };
    NativeHandoff nativeHandoff_ = NativeHandoff::None;
    /// The float a hand-off is waiting on, guarded: the pooled float can be
    /// released by the dialog while a transition it started is still in flight.
    QPointer<PipFloat> nativeHandoffFloat_;
    /// The last observation that was announced, so the request is reissued only
    /// when the selection or an observed source/device fact is new - never as a
    /// busy loop while the context is still being observed.
    bool nativeRequestAnnounced_ = false;
    native::SelectionState nativeRequestSelection_;
    std::optional<native::NativePreparationContext> nativeRequestContext_;
    /// The one reference capture in flight: which core it was asked of, and the
    /// source epoch it was asked under, so a reply from another core or another
    /// source is dropped rather than handed over as the current frame.
    QPointer<VideoSurface> nativeReferenceSurface_;
    quint64 nativeReferenceSourceEpoch_ = 0;
    /// The index the pending capture was asked for, so a reply belonging to a
    /// request this dialog has moved past is dropped instead of forwarded.
    int nativeReferenceFrameIndex_ = -1;
    bool nativeReferencePending_ = false;
    // The float is kept across a PiP session, not destroyed by leavePip(). Each
    // one owns a QOpenGLWidget with a live mpv render context, and building or
    // tearing one down while the other is on screen faults the display driver.
    // Re-entering PiP reuses this one; the dialog's destructor is what finally
    // releases it. While PiP is up this and pip_ name the same object.
    PipFloat* pipPool_ = nullptr;
    // What the main core was doing when PiP took over, so leaving PiP restores
    // the state rather than guessing it from a toggle.
    double pipResumePosition_ = 0.0;
    bool pipResumePlaying_ = true;
    // The resume point a stream swap holds until the new file reports a
    // duration: mpv cannot seek a stream it has not loaded yet.
    bool resumePending_ = false;
    double resumePosition_ = 0.0;
    bool resumePaused_ = false;
    // A position-reset swap - a new episode - still owes the viewer the pause
    // they set. A freshly loaded stream always comes up playing, so the pause
    // is re-asserted once the load lands rather than carried as a resume point.
    bool pauseAfterLoad_ = false;
    // False while the surface has no mpv core at all: a retry then asks the
    // host to provision the runtime instead of reloading a dead url.
    bool engineReady_ = false;
    QVector<QObject*> panelWatchers_;
};

}  // namespace shell
}  // namespace mplayer
