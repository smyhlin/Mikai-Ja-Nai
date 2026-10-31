#pragma once

#include <QDialog>
#include <QEvent>
#include <QList>
#include <QMetaObject>
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
class QKeyEvent;
class QLabel;
class QMouseEvent;
class QMoveEvent;
class QPainter;
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
#include "ShellWidgets.hpp"
#include "native/prepare/NativeFrameFixture.hpp"
#include "native/status/NativeMenu.hpp"
#include "native/status/NativePlaybackController.hpp"
#include "native/prepare/NativePreparationContext.hpp"
#include "PipFloat.hpp"

namespace mplayer {
class VideoSurface;
}

namespace mplayer {
namespace shell {

class CompareWindow;
class MediaInformationDialog;

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
    virtual void requestCopyStreamUrl() = 0;
    virtual void playerClosed() = 0;
    /// The stream could not be played: the host re-extracts and reloads.
    virtual void playbackFailed() = 0;
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
    /// Offset, in seconds, that was stored for the episode about to be opened.
    /// Playback always starts at 00:00; the offset only raises a "continue from"
    /// prompt once the length is known. Set it before applyEpisode() or
    /// startPlayer(); 0 means there is nothing to offer.
    void setEpisodeResumePoint(double seconds);
    /// Publishes the dub/sub variants the popover lists and which embed is
    /// current. The track chip only appears once there is something to pick.
    void setVariants(const QVector<QVariantMap>& variants, const QString& currentEmbed);
    void stepEpisode(int delta);
    /// Badges one track with the quality a probe found for it.
    void setTrackQuality(const QString& embed, int quality);
    void showError(const QString& message);
    /// The one informational line the dialog shows while the app is still
    /// resolving the mpv runtime. It is not a failure: no alert is raised for
    /// assistive technology, no retry chip is offered and no re-extraction is
    /// started, because nothing has failed yet. What the pending resolution is
    /// actually doing is the runtime layer's own sentence, shown verbatim.
    void showRuntimePendingStatus();
    /// A requested AnimeJaNai preset is saved by stable id and prepared off the
    /// GUI thread; unavailable prerequisites remain visible as a failed request.
    void applyAnimeJanaiRequest(const QString& id);
    void applyAnimeJanaiBackend(const QString& backend);
    /// Retry deferred mixed-chain preparation when the owning core publishes source metadata.
    void onAnimeJanaiSourceContextChanged();
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
    /// Components persist immediately even when Settings is cancelled; retry
    /// settled unavailable requests without replacing healthy or pending plans.
    void refreshStoredSettings();
    /// Opens the first stream; the engine exists from the shell's surface.
    void startPlayer();

    bool isPipActive() const;
    /// True while the core that owns playback - the float's while PiP is up -
    /// actually has a video loaded. The dialog hides itself for PiP, so this,
    /// not isVisible(), is what says it still owns a running stream.
    bool hasActiveSource() const;
    /// Pauses the core that owns playback when it is actually playing. False when
    /// nothing was playing, including a core an engine build is already holding
    /// paused, so the caller does not resume a pause it did not take.
    bool pauseIfPlaying();
    /// Starts the owning core again. A no-op when that core is already playing
    /// or there is no core, so a pause the viewer made themselves is left alone
    /// and nothing starts that was not playing.
    void resumePlayback();
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
    void retitleEnginePreparation(const QString& title);
    void updateEnginePreparationSummary(const QString& summary);
    /// True while the preparation window is up.
    bool enginePreparationVisible() const;
    /// Replays the owning surface's retained builder state after its signal has
    /// been connected, so a build already in flight still gets a visible tool.
    void syncAnimeJanaiEngineBuildState(VideoSurface* owner = nullptr);

signals:
    /// A real file loaded for `playlistIndex` on the core that owns playback:
    /// the core reported the file loaded, and the index is the playlist entry
    /// that load was requested for. A click, a startPlayer() call, a playlist
    /// update or a start-file alone is not a start. A retry or a track/quality
    /// swap of the same entry can report the same index again.
    void episodePlaybackStarted(int playlistIndex);
    /// The file loaded for `playlistIndex` played to its natural end. Emitted
    /// while that file is still the one the core has loaded, so the index is
    /// the OUTGOING episode, before anything steps to the next one. A stop, a
    /// close, an error, a redirect, a replaced source, a quality or track swap
    /// or a PiP hand-off never emits it.
    void episodePlaybackFinished(int playlistIndex);
    /// Playback reached `position` of `duration` in `playlistIndex`. Emitted on
    /// the first frame and then whenever the position moves by a few seconds, so
    /// the host can store a resume point without polling the core.
    void episodeProgress(int playlistIndex, double position, double duration);
    /// The viewer chose to start the episode over, so the stored offset for
    /// `playlistIndex` no longer means anything.
    void episodeResumeDeclined(int playlistIndex);
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
    void watchMediaTracks(VideoSurface* source);
    void refreshTrackMenus();
    QVector<MenuEntry> mediaTrackEntries(const QString& kind) const;
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
    void openMediaFolder();
    void openMediaInformation();
    void closeMediaInformation();
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
    void handleAnimeJanaiEngineBuildChanged(VideoSurface* owner, bool active, bool succeeded,
                                            const QString& detail);
    /// Restates this dialog's own words for the running AnimeJaNai build - the
    /// title, the phase and the facts wrapped around the filter's own detail -
    /// in the language now installed. It only re-emits text through the overlay's
    /// non-resetting setters: no build signal, frame hold, command, generation,
    /// timer, cancel or playback is replayed or restarted.
    void restateAnimeJanaiBuildLanguage();
    /// The running build's detail line: the filter's raw diagnostic text kept
    /// verbatim, followed by the facts this dialog names. Rebuilt on every call,
    /// so only the wrapper follows the installed language.
    QString animeJanaiBuildDetailText() const;
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
    /// Captures the requested playlist index; zero means no episode.
    quint64 episodeMediaToken() const;
    /// Decodes a captured load token; -1 means it did not name an episode.
    static int episodeIndexFromToken(quint64 mediaToken);
    /// Accepts lifecycle events only from the current playback owner.
    void onMediaLoaded(VideoSurface* source, quint64 mediaToken);
    void onMediaEnded(VideoSurface* source, quint64 mediaToken);
    void onEngineError(const QString& message);
    void onEngineDuration(double seconds);
    /// Asks whether to jump to a stored offset; playback already runs from 0.
    void offerResume(double seconds);
    void onEnginePosition(double seconds);
    void onEnginePlaying(bool playing);
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
    // T-404: the status-flash cadence and the backend spelling used to exist
    // as duplicated file-local copies in PlayerDialog.cpp AND
    // PlayerDialogProcessing.cpp; they live on the class now so the two TUs
    // cannot drift apart.
    static constexpr int kStatusMs = 1900;
    /// The persisted spelling of a backend name ("tensorrt"/"directml"): one
    /// writer for the settings key and the shell's selector, defined once.
    static QString animeJanaiBackendName(native::Backend backend);
    /// Reads the stored Anime4K preset bindings and installs one window shortcut
    /// per usable one. Empty, unparsable, reserved and already-taken sequences
    /// are skipped rather than installed: a binding is a preference that can be
    /// stale, and a stale one must never take a key the player owns.
    void loadPresetShortcuts();
    QPoint settingsAnchor() const;
    void enterPip();
    void leavePip(bool closing = false);

    /// Which window is waiting to be replaced by the other one.
    enum class WindowHandoff { None, PlayerToFloat, FloatToPlayer };

    // Arm before show(); slow successors must not uncover an empty window.
    void beginWindowHandoff(WindowHandoff kind, PipFloat* floatWindow,
                            bool leftFullscreen = false);
    void cancelWindowHandoff();
    void finishWindowHandoff();
    void onHandoffPainted();
    void onHandoffFailed(const QString& reason);
    /// Acknowledges the parked float's native stop while its core and GL context
    /// are still alive, so releasePipPool() never destroys a pending transition.
    /// False when a stop never acknowledged within its bound.
    bool settleParkedNative();
    /// Destroys the pooled float, whichever state PiP is in. The float is
    /// parentless, so the dialog is the only thing that can release it.
    void releasePipPool();
    void finishCompareLoading();
    void scheduleCompareStart(quint64 generation, int delayMs);
    void startCompareWindow(quint64 generation);
    void checkCompareLoading(quint64 generation);

    PlayerShell* shell_ = nullptr;
    VideoSurface* surface_ = nullptr;
    PlayerHost* host_ = nullptr;
    bool directMedia_ = false;
    QPointer<MediaInformationDialog> mediaInformation_;

    MenuPopup* audioPopup_ = nullptr;
    MenuPopup* subtitlesPopup_ = nullptr;
    SeriesDrawer* seriesDrawer_ = nullptr;
    ProcessingOverlay* processingOverlay_ = nullptr;
    QPointer<VideoSurface> animeJanaiBuildingSurface_;
    QPointer<VideoSurface> animeJanaiPlanWaitSurface_;
    /// The core the running engine operation holds paused, and the source epoch
    /// that hold was taken under: 0 means the hold predates any source, so a
    /// completion restores the intent instead of being treated as stale.
    QPointer<VideoSurface> enginePreparationSurface_;
    uint64_t enginePreparationSourceEpoch_ = 0;
    bool enginePreparationActive_ = false;
    /// Identifies a REAL operation: only beginEnginePreparation() bumps it, so a
    /// source replacement keeps the same generation and the same operation's
    /// terminal still ends the hold it re-established. A genuinely newer
    /// operation supersedes an older one's terminal this way.
    quint64 enginePreparationGeneration_ = 0;
    /// The generation the surface's own engine build was started under, so its
    /// completion is applied only while it is still the operation on screen.
    quint64 animeJanaiBuildGeneration_ = 0;
    /// What the window is showing for this build, retained as typed state rather
    /// than display text: a language switch restates the wrapper from it. The raw
    /// detail is the filter's own, kept verbatim and never translated. The preset
    /// is retained as its catalogue id - the semantic identity - and the label is
    /// that id's catalogue rendering, first resolved when the operation started
    /// and derived again on every restatement. App-owned builtin names follow the
    /// installed language while user and model names come back unchanged. An
    /// identity the catalogue no longer lists keeps the last rendering instead of
    /// being guessed at.
    QString animeJanaiBuildDetailRaw_;
    QString animeJanaiBuildPresetId_;
    QString animeJanaiBuildPresetLabel_;
    int animeJanaiBuildWidth_ = 0;
    int animeJanaiBuildHeight_ = 0;
    /// The stage shape this build last supplied, so a restatement keeps the same
    /// index and count instead of inventing progress.
    int animeJanaiBuildStageIndex_ = -1;
    int animeJanaiBuildStageCount_ = 0;
    /// Moves the pause hold to the core playing now, releasing the hold it
    /// leaves behind outright and carrying the viewer intent the hold kept to
    /// the incoming owner. `carriedIntent` overrides the read-back intent, which
    /// is the only way to name it once the outgoing hold is already gone.
    void moveEnginePreparationPause(VideoSurface* owner,
                                    std::optional<bool> carriedIntent = std::nullopt);
    void applyAnimeJanaiPlanToFloat(VideoSurface* target);
    void settleAnimeJanaiPlanOnSurface(VideoSurface* completed, bool succeeded);
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
    /// Prevents duplicate main/PiP completions; an actual load rearms it.
    int lastFinishedEpisodeIndex_ = -1;
    double volume_ = 0.7;      // 0..1, what the shell shows
    double speed_ = 1.0;
    anime4k::Mode anime4kMode_ = anime4k::Mode::Off;
    // Custom shader chains have no Mode; PiP must retain their candidate ID.
    QString anime4kChainId_ = QStringLiteral("off");
    QString animeJanaiPresetId_;
    native::Backend animeJanaiBackend_ = native::Backend::TensorRT;
    native::AnimeJanaiPlan animeJanaiPlan_;
    quint64 animeJanaiSequence_ = 0;
    quint64 animeJanaiPlanGeneration_ = 0;
    std::shared_ptr<std::atomic_bool> animeJanaiCancellation_;
    // T-306: the AnimeJaNai worker task carries THIS slot instead of a raw
    // dialog address. The QPointer inside is created, read and cleared on the
    // GUI thread only, so nothing thread-affine ever crosses into the pool;
    // the shared_ptr travels instead, and the queued body resolves the dialog
    // through it - a destroyed dialog reads as a null pointer by construction
    // rather than by every supersede/destruction path remembering a flag.
    struct AnimeJanaiDialogSlot {
        QPointer<PlayerDialog> dialog;
    };
    std::shared_ptr<AnimeJanaiDialogSlot> animeJanaiDialogSlot_;
    bool animeJanaiWaitingForSourceContext_ = false;
    uint64_t animeJanaiPreparationSourceEpoch_ = 0;
    uint64_t animeJanaiPendingPreparationSourceEpoch_ = 0;
    native::AnimeJanaiSourceContext animeJanaiLastPreparedSource_;
    /// The comparison window, created on first use and kept: it borrows the
    /// surface, and two of them would fight over the same core.
    CompareWindow* compare_ = nullptr;
    bool compareLoadingActive_ = false;
    quint64 compareLoadingGeneration_ = 0;
    uint64_t compareLoadingSourceEpoch_ = 0;
    QMetaObject::Connection compareLoadingFrameSwapped_;
    bool compareLoadingWasVisible_ = false;
    bool compareLoadingStartScheduled_ = false;
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
    /// True while the informational "runtime is being resolved" line is the
    /// dialog's status. Kept apart from `errorActive_`: a resolution in flight
    /// is not a media failure, so it never arms the retry chip and a real
    /// failure keeps its own reason when it arrives.
    bool runtimePendingVisible_ = false;
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
    WindowHandoff windowHandoff_ = WindowHandoff::None;
    QPointer<PipFloat> windowHandoffFloat_;
    QPointer<VideoSurface> windowHandoffSurface_;
    QMetaObject::Connection windowHandoffFrameSwapped_;
    QMetaObject::Connection windowHandoffFailed_;
    bool windowHandoffLeftFullscreen_ = false;
    // Pooled floats retain identity and source epochs across distinct transitions.
    quint64 pipTransitionGeneration_ = 0;
    quint64 windowHandoffGeneration_ = 0;
    bool nativePlaybackRequested_ = false;
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
    // The resume point a stream swap holds until the new file reports a
    // duration: mpv cannot seek a stream it has not loaded yet.
    bool resumePending_ = false;
    // A stored offset for the episode being applied, seeked once its duration is
    // known. Kept apart from resumePending_, which carries a position across a
    // stream swap inside one episode.
    double episodeResumeSeconds_ = 0.0;
    double episodeSeekPending_ = 0.0;
    // Throttles episodeProgress: the core reports position far too often to
    // persist, and every write costs a settings sync.
    double lastProgressSeconds_ = -1.0;
    int lastProgressEpisode_ = -1;
    double resumePosition_ = 0.0;
    bool resumePaused_ = false;
    // A position-reset swap - a new episode - still owes the viewer the pause
    // they set. A freshly loaded stream always comes up playing, so the pause
    // is re-asserted once the load lands rather than carried as a resume point.
    bool pauseAfterLoad_ = false;
    // The pause generation `resumePaused_`/`pauseAfterLoad_` were recorded
    // under, so a newer explicit Play/Pause on the loading stream is never
    // overwritten by the pause recorded before it was up.
    quint64 resumePauseGeneration_ = 0;
    // False while the surface has no mpv core at all: a retry then asks the
    // host to provision the runtime instead of reloading a dead url.
    bool engineReady_ = false;
    QVector<QObject*> panelWatchers_;
};

}  // namespace shell
}  // namespace mplayer
