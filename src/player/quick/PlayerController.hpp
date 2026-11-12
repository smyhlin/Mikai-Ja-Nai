
#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include "../native/components/AnimeJanaiRuntime.hpp"
#include "../native/prepare/NativeFrameFixture.hpp"
#include "../native/prepare/NativePreparedPlan.hpp"
#include "../native/prepare/NativePreparationContext.hpp"
#include "../native/status/NativeMenu.hpp"
#include "../native/status/NativePlaybackController.hpp"
#include "../shell/OpenSubtitlesClient.hpp"
#include "../shell/PlayerHost.hpp"
#include "../shell/ProcessingSettingsIntent.hpp"
#include "../shaders/Anime4K.hpp"
#include "PlayerMenuModel.hpp"

#include <QtQml/qqmlregistration.h>

QT_BEGIN_NAMESPACE
class QEvent;
class QScreen;
class QTemporaryDir;
class QTimer;
class QWidget;
QT_END_NAMESPACE

namespace mplayer {
class VideoSession;

namespace shell {
class FramePreviewService;
}

namespace quick {

class PlayerWindow;
class PreparationWindow;

class PlayerController : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(PlayerController)
    QML_UNCREATABLE("PlayerController is created by the app and injected into PlayerEntry")

public:
    enum class ResumeChoice { StartOver = 0, Continue = 1, Dismiss = 2 };
    Q_ENUM(ResumeChoice)

    enum class StatusMode { None, Transient, Error, RuntimePending, Background, Processing };
    Q_ENUM(StatusMode)

    /// String names ("DEFAULT_FIT"/…) stay an external API.
    enum class ChromeState { DefaultFit, FullscreenActive, FullscreenAutoHidden };
    Q_ENUM(ChromeState)

    Q_PROPERTY(mplayer::VideoSession* session READ session CONSTANT)

    Q_PROPERTY(QString caption READ caption NOTIFY mediaChanged)
    Q_PROPERTY(QString windowTitle READ windowTitle NOTIFY mediaChanged)
    Q_PROPERTY(QString mediaPath READ mediaPath NOTIFY mediaChanged)
    Q_PROPERTY(QString mediaOrigin READ mediaOrigin NOTIFY mediaChanged)
    Q_PROPERTY(bool hasMedia READ hasMedia NOTIFY mediaChanged)
    Q_PROPERTY(bool directMedia READ isDirectMedia NOTIFY mediaChanged)
    Q_PROPERTY(bool localMedia READ localMedia NOTIFY mediaChanged)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY mediaChanged)
    Q_PROPERTY(int sourceWidth READ sourceWidth NOTIFY sourceSizeChanged)
    Q_PROPERTY(int sourceHeight READ sourceHeight NOTIFY sourceSizeChanged)
    Q_PROPERTY(double aspectRatio READ aspectRatio NOTIFY aspectRatioChanged)
    Q_PROPERTY(QStringList videoBadges READ videoBadges NOTIFY videoBadgesChanged)
    Q_PROPERTY(QString episodeLabel READ episodeLabel NOTIFY episodeLabelChanged)
    Q_PROPERTY(int episodeIndex READ currentEpisodeIndex NOTIFY episodeChanged)
    Q_PROPERTY(bool hasPreviousEpisode READ hasPreviousEpisode NOTIFY episodeSteppingChanged)
    Q_PROPERTY(bool hasNextEpisode READ hasNextEpisode NOTIFY episodeSteppingChanged)
    Q_PROPERTY(bool seriesExpanded READ seriesExpanded NOTIFY seriesExpandedChanged)

    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(bool enginePauseHold READ enginePauseHold NOTIFY enginePauseHoldChanged)
    Q_PROPERTY(QString pauseHoldText READ pauseHoldText NOTIFY enginePauseHoldChanged)
    Q_PROPERTY(double volume READ volume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    Q_PROPERTY(double rate READ rate NOTIFY rateChanged)
    Q_PROPERTY(QVariantList speedPresets READ speedPresets CONSTANT)
    Q_PROPERTY(int videoScaleMode READ videoScaleMode NOTIFY videoScaleModeChanged)
    Q_PROPERTY(QVariantList timelineMarks READ timelineMarks NOTIFY skipSegmentsChanged)
    Q_PROPERTY(mplayer::quick::PlayerMenuModel* skipOffers READ skipOffers NOTIFY skipSegmentsChanged)
    Q_PROPERTY(double networkBytesPerSecond READ networkBytesPerSecond NOTIFY streamStatsChanged)
    Q_PROPERTY(double streamFramesPerSecond READ streamFramesPerSecond NOTIFY streamStatsChanged)

    Q_PROPERTY(QString qualityLabel READ qualityLabel NOTIFY qualityChanged)
    Q_PROPERTY(int currentHeight READ currentHeight NOTIFY qualityChanged)
    Q_PROPERTY(QString currentEmbed READ currentEmbed NOTIFY variantsChanged)
    Q_PROPERTY(bool audioTracksAvailable READ audioTracksAvailable NOTIFY trackAvailabilityChanged)
    Q_PROPERTY(bool subtitleTracksAvailable READ subtitleTracksAvailable NOTIFY trackAvailabilityChanged)
    Q_PROPERTY(double subtitleDelay READ subtitleDelay NOTIFY subtitleDelayChanged)

    Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY fullscreenChanged)
    Q_PROPERTY(ChromeState chromeState READ chromeState NOTIFY chromeStateChanged)
    Q_PROPERTY(QString stateName READ stateName NOTIFY chromeStateChanged)
    Q_PROPERTY(bool autoHidden READ autoHidden NOTIFY chromeStateChanged)
    Q_PROPERTY(bool pipActive READ isPipActive NOTIFY pipChanged)
    Q_PROPERTY(bool hasActiveSource READ hasActiveSource NOTIFY activeSourceChanged)
    Q_PROPERTY(bool interactionActive READ interactionActive NOTIFY interactionChanged)
    Q_PROPERTY(bool menuStackVisible READ menuStackVisible NOTIFY interactionChanged)

    Q_PROPERTY(mplayer::quick::PlayerMenuModel* pictureMenu READ pictureMenu NOTIFY menusChanged)
    Q_PROPERTY(mplayer::quick::PlayerMenuModel* pictureSubtitlesMenu READ pictureSubtitlesMenu NOTIFY menusChanged)
    Q_PROPERTY(mplayer::quick::PlayerMenuModel* audioMenu READ audioMenu NOTIFY menusChanged)
    Q_PROPERTY(mplayer::quick::PlayerMenuModel* subtitlesMenu READ subtitlesMenu NOTIFY menusChanged)
    Q_PROPERTY(mplayer::quick::PlayerMenuModel* enhancementMenu READ enhancementMenu NOTIFY menusChanged)
    Q_PROPERTY(mplayer::quick::PlayerMenuModel* qualityMenu READ qualityMenu NOTIFY menusChanged)
    Q_PROPERTY(mplayer::quick::PlayerMenuModel* settingsMenu READ settingsMenu NOTIFY menusChanged)
    Q_PROPERTY(mplayer::quick::PlayerMenuModel* seriesMenu READ seriesMenu NOTIFY menusChanged)
    Q_PROPERTY(mplayer::quick::PlayerMenuModel* keyHintsMenu READ keyHintsMenu NOTIFY menusChanged)
    Q_PROPERTY(mplayer::quick::PlayerMenuModel* remedyMenu READ remedyMenu NOTIFY menusChanged)
    Q_PROPERTY(mplayer::quick::PlayerMenuModel* subtitleResults READ subtitleResults NOTIFY subtitleSearchChanged)

    Q_PROPERTY(int anime4kMode READ anime4kMode NOTIFY anime4kChanged)
    Q_PROPERTY(QString anime4kChainId READ anime4kChainId NOTIFY anime4kChanged)
    Q_PROPERTY(QString animeJanaiPresetId READ animeJanaiPresetId NOTIFY animeJanaiChanged)
    Q_PROPERTY(QString animeJanaiStatus READ animeJanaiStatus NOTIFY animeJanaiChanged)
    Q_PROPERTY(QString animeJanaiNote READ animeJanaiNote NOTIFY animeJanaiChanged)
    Q_PROPERTY(QString animeJanaiBackend READ animeJanaiBackendName NOTIFY animeJanaiChanged)
    Q_PROPERTY(QString animeJanaiChipText READ animeJanaiChipText NOTIFY animeJanaiChanged)
    Q_PROPERTY(QString animeJanaiChipTooltip READ animeJanaiChipTooltip NOTIFY animeJanaiChanged)
    Q_PROPERTY(QString enhancementChipText READ enhancementChipText NOTIFY anime4kChanged)
    Q_PROPERTY(QString enhancementChipTooltip READ enhancementChipTooltip NOTIFY anime4kChanged)
    Q_PROPERTY(bool nativeProcessingRequested READ nativeProcessingRequested NOTIFY nativeHudChanged)
    Q_PROPERTY(bool nativeHudVisible READ nativeHudVisible NOTIFY nativeHudChanged)
    Q_PROPERTY(bool nativeHudActive READ nativeHudActive NOTIFY nativeHudChanged)
    Q_PROPERTY(QString nativeHudHeadline READ nativeHudHeadline NOTIFY nativeHudChanged)
    Q_PROPERTY(QString nativeHudState READ nativeHudState NOTIFY nativeHudChanged)
    Q_PROPERTY(QString nativeHudProvider READ nativeHudProvider NOTIFY nativeHudChanged)
    Q_PROPERTY(QString nativeHudModel READ nativeHudModel NOTIFY nativeHudChanged)
    Q_PROPERTY(QString nativeHudGeometry READ nativeHudGeometry NOTIFY nativeHudChanged)
    Q_PROPERTY(QString nativeHudFps READ nativeHudFps NOTIFY nativeHudChanged)
    Q_PROPERTY(QString nativeHudInterpolation READ nativeHudInterpolation NOTIFY nativeHudChanged)
    Q_PROPERTY(QString nativeHudDropped READ nativeHudDropped NOTIFY nativeHudChanged)
    Q_PROPERTY(QString nativeHudReason READ nativeHudReason NOTIFY nativeHudChanged)

    Q_PROPERTY(bool processingActive READ processingActive NOTIFY processingChanged)
    Q_PROPERTY(QString processingReasonKey READ processingReasonKey NOTIFY processingChanged)
    Q_PROPERTY(QString processingReason READ processingReason NOTIFY processingChanged)
    Q_PROPERTY(QString processingMessage READ processingMessage NOTIFY processingChanged)
    Q_PROPERTY(bool retryVisible READ retryVisible NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(StatusMode statusMode READ statusMode NOTIFY statusChanged)
    Q_PROPERTY(bool errorActive READ errorActive NOTIFY statusChanged)
    Q_PROPERTY(bool runtimePending READ runtimePending NOTIFY statusChanged)
    Q_PROPERTY(bool openingLoading READ openingLoading NOTIFY openingChanged)
    Q_PROPERTY(QString openingHeadline READ openingHeadline NOTIFY openingChanged)
    Q_PROPERTY(QString openingDetail READ openingDetail NOTIFY openingChanged)
    Q_PROPERTY(bool compareLoading READ compareLoading NOTIFY compareLoadingChanged)

    Q_PROPERTY(bool preparationVisible READ enginePreparationVisible NOTIFY preparationChanged)
    Q_PROPERTY(QString preparationTitle READ preparationTitle NOTIFY preparationChanged)
    Q_PROPERTY(int preparationStageIndex READ preparationStageIndex NOTIFY preparationChanged)
    Q_PROPERTY(int preparationStageCount READ preparationStageCount NOTIFY preparationChanged)
    Q_PROPERTY(QString preparationPhase READ preparationPhase NOTIFY preparationChanged)
    Q_PROPERTY(QString preparationDetail READ preparationDetail NOTIFY preparationChanged)
    Q_PROPERTY(QString preparationSummary READ preparationSummary NOTIFY preparationChanged)
    Q_PROPERTY(bool preparationCancelPending READ preparationCancelPending NOTIFY preparationChanged)

    Q_PROPERTY(bool resumeSheetVisible READ resumeSheetVisible NOTIFY resumeSheetChanged)
    Q_PROPERTY(QString resumeSheetLabel READ resumeSheetLabel NOTIFY resumeSheetChanged)
    Q_PROPERTY(double resumeSheetSeconds READ resumeSheetSeconds NOTIFY resumeSheetChanged)
    Q_PROPERTY(quint64 resumeSheetGeneration READ resumeSheetGeneration NOTIFY resumeSheetChanged)

    Q_PROPERTY(bool subtitleSearchVisible READ subtitleSearchVisible NOTIFY subtitleSearchChanged)
    Q_PROPERTY(QString subtitleSearchSeed READ subtitleSearchSeed NOTIFY subtitleSearchChanged)
    Q_PROPERTY(QString subtitleSearchQuery READ subtitleSearchQuery NOTIFY subtitleSearchChanged)
    Q_PROPERTY(QString subtitleSearchLanguage READ subtitleSearchLanguage NOTIFY subtitleSearchChanged)
    Q_PROPERTY(QString subtitleSearchStatus READ subtitleSearchStatus NOTIFY subtitleSearchChanged)
    Q_PROPERTY(bool subtitleSearchBusy READ subtitleSearchBusy NOTIFY subtitleSearchChanged)
    Q_PROPERTY(bool subtitleDownloadBusy READ subtitleDownloadBusy NOTIFY subtitleSearchChanged)

    Q_PROPERTY(bool hoverPreviewVisible READ hoverPreviewVisible NOTIFY hoverPreviewChanged)
    Q_PROPERTY(QUrl hoverPreviewSource READ hoverPreviewSource NOTIFY hoverPreviewChanged)
    Q_PROPERTY(QString hoverPreviewTime READ hoverPreviewTime NOTIFY hoverPreviewChanged)
    Q_PROPERTY(QString hoverPreviewLabel READ hoverPreviewLabel NOTIFY hoverPreviewChanged)

    Q_PROPERTY(QColor accentColor READ accentColor NOTIFY themeChanged)
    Q_PROPERTY(int cornerRadius READ cornerRadius NOTIFY themeChanged)
    Q_PROPERTY(bool darkTheme READ darkTheme NOTIFY themeChanged)
    Q_PROPERTY(bool notificationVisible READ notificationVisible NOTIFY notificationChanged)
    Q_PROPERTY(QString notificationText READ notificationText NOTIFY notificationChanged)
    Q_PROPERTY(bool notificationFailure READ notificationFailure NOTIFY notificationChanged)

    explicit PlayerController(const QString& caption, const QString& url,
                              QObject* parent = nullptr);
    PlayerController(const QString& caption, const QString& url,
                     const QList<shell::QualityEntry>& qualities, int initialHeight,
                     shell::PlayerHost* host, QObject* parent = nullptr);
    ~PlayerController() override;

    /// Steps episode in place: same window/session; clears pending resume and
    /// skip segments and opens at 0.
    void applyEpisode(const QString& label, const QString& embed, int index,
                      const QList<shell::QualityEntry>& qualities,
                      const QVector<QVariantMap>& variants);
    /// Keeps the position across a dub/sub swap.
    void applyVariant(const QList<shell::QualityEntry>& qualities, const QString& label,
                      const QString& embed = QString());
    void applyRefreshedStreams(const QList<shell::QualityEntry>& qualities, int height = 0);
    void setPlaylist(const QVector<QVariantMap>& playlist, int index = 0);
    /// Stored offset for the next episode; raises at most one offer, after the
    /// length is known. 0 means nothing to offer.
    void setEpisodeResumePoint(double seconds);
    void setVariants(const QVector<QVariantMap>& variants, const QString& currentEmbed);
    /// `type` is the provider id; forward intervals only; the clock decides
    /// which are offered.
    void setSkipSegments(const QVector<QVariantMap>& segments);
    void stepEpisode(int delta);
    int currentEpisodeIndex() const { return episodeIndex_; }
    void setTrackQuality(const QString& embed, int quality);
    void showError(const QString& message);
    /// Reports an auxiliary action failure without invoking playback recovery.
    void reportActionFailure(const QString& message);
    /// Informational wait, not a failure: no retry, no re-extraction.
    void showRuntimePendingStatus();
    void setBackgroundActivity(const QString& message);
    void applyAnimeJanaiRequest(const QString& id);
    void applyAnimeJanaiBackend(const QString& backend);
    void onAnimeJanaiSourceContextChanged();
    quint64 animeJanaiPlanGeneration() const { return animeJanaiPlanGeneration_; }
    /// Persists first, derives the effective value from the filter snapshot;
    /// never touches URL/quality cache/Anime4K chain.
    void applyNativeRequest(native::menu::Action action, const QString& value);
    void reportNativeProcessingFailure(const QString& reason);
    void reportNativeProcessingFailure(const QString& reasonKey, const QString& reason);
    void submitNativeAdmission(const native::PreparedAdmissionRequest& request);
    void pushNativeInputs();
    void refreshNativeInputs();
    void refreshStoredSettings();
    void startPlayer();

    bool isPipActive() const { return pipActive_; }
    bool hasActiveSource() const;
    bool pauseIfPlaying();
    void resumePlayback();
    std::optional<native::NativePreparationContext> nativePreparationContext() const;
    void armNativeProbe(native::Backend backend = native::Backend::TensorRT);
    /// `frameIndex` 1 is a real adjacent source frame, restored afterwards.
    void requestNativeReferenceFrame(int frameIndex = 0);
    int currentHeight() const { return currentHeight_; }
    QString currentEmbed() const { return currentEmbed_; }

    Q_INVOKABLE void toggleSeries();
    Q_INVOKABLE void setSeriesExpanded(bool expanded);
    Q_INVOKABLE void openAudioMenu();
    Q_INVOKABLE void openSubtitlesMenu();
    /// Changes presentation only; playback keeps the same window and session.
    Q_INVOKABLE void togglePip();

    /// Holds the session paused for real engine work; explicit play/pause is
    /// retained without advancing the video. The visible status lives in the
    /// separate nonactivating PreparationWindow (never an in-scene sheet, and
    /// the player window is never made topmost for a build).
    void beginEnginePreparation(const QString& title);
    void updateEnginePreparation(int stageIndex, int stageCount, const QString& phase,
                                 const QString& detail);
    void endEnginePreparation(bool ok, const QString& summary);
    void retitleEnginePreparation(const QString& title);
    void updateEnginePreparationSummary(const QString& summary);
    bool enginePreparationVisible() const { return enginePreparationVisible_; }
    void syncAnimeJanaiEngineBuildState(VideoSession* owner = nullptr);

    Q_INVOKABLE void showWindow();
    Q_INVOKABLE void hideWindow();
    Q_INVOKABLE void minimizeWindow();
    /// Acknowledged close; false means refused and the window stays up.
    Q_INVOKABLE bool requestClose();
    PlayerWindow* window() const { return window_.get(); }
    bool isWindowVisible() const;

    Q_INVOKABLE void togglePlayPause();
    Q_INVOKABLE void seekTo(double seconds);
    Q_INVOKABLE void seekBy(double seconds);
    Q_INVOKABLE void skipForward();
    Q_INVOKABLE void setVolume(double volume);
    Q_INVOKABLE void adjustVolume(double delta);
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void setRate(double rate);
    Q_INVOKABLE void setVideoScaleMode(int mode);
    Q_INVOKABLE void makeDefaultVideoPlayer();
    Q_INVOKABLE QString formatTime(double seconds) const;
    Q_INVOKABLE QString formatRemaining(double seconds) const;
    Q_INVOKABLE void retryPlayback();

    Q_INVOKABLE PlayerMenuModel* menuModel(const QString& menuId) const;
    /// Resolves the level a submenu row opens and reuses its cached model for
    /// the session lifetime (no per-hover allocation). Rebuilds mark their own
    /// Row::selected; hover never changes a model's selection. Null for a
    /// non-submenu row.
    Q_INVOKABLE PlayerMenuModel* submenuModel(const QString& menuId, int row);
    /// Dispatches the row's typed action; heading, disabled and submenu rows
    /// are inert.
    Q_INVOKABLE void activateMenuRow(const QString& menuId, int row);
    Q_INVOKABLE void closeMenus();
    /// Escape priority: resume sheet, search sheet, menu stack (as last
    /// reported), series drawer. False when nothing was dismissed.
    Q_INVOKABLE bool dismissOverlay();

    /// Aggregate interaction hold from the QML root: menus, modal sheets,
    /// focused controls, hover and scrubbing. While held the idle timer never
    /// fires; releasing it restarts the dwell.
    Q_INVOKABLE void setInteractionActive(bool active);
    /// The cascading stack is open/closed, so dismissOverlay() can treat Escape
    /// as consumed even though QML owns the visual stack.
    Q_INVOKABLE void setMenuStackVisible(bool visible);
    Q_INVOKABLE void noteUserActivity();

    Q_INVOKABLE void selectEpisode(int playlistIndex);
    Q_INVOKABLE void goToNextEpisode();
    Q_INVOKABLE void goToPreviousEpisode();
    Q_INVOKABLE void selectQuality(int height);
    Q_INVOKABLE void requestExternalTrack(bool subtitle);
    Q_INVOKABLE void adjustSubtitleDelay(double delta);
    Q_INVOKABLE void resetSubtitleDelay();

    Q_INVOKABLE void toggleFullscreen();
    Q_INVOKABLE void leavePip();
    Q_INVOKABLE void beginWindowMove();
    Q_INVOKABLE void beginWindowResize(int edges);

    Q_INVOKABLE void copyCurrentFrame();
    Q_INVOKABLE void openCompare();
    Q_INVOKABLE void copyMediaLink();
    Q_INVOKABLE void openContainingFolder();
    Q_INVOKABLE void openMediaInformation();
    Q_INVOKABLE void openClipDialog();
    Q_INVOKABLE void applyAnime4KPreset(const QString& presetId);
    Q_INVOKABLE void openEnhancementSettings();
    Q_INVOKABLE void activateSkipOffer(int row);

    /// Stale generation/token is a no-op; there is no nested exec().
    Q_INVOKABLE void answerResume(quint64 generation, ResumeChoice choice);
    Q_INVOKABLE void openSubtitleSearch();
    Q_INVOKABLE void setSubtitleSearchQuery(const QString& query);
    /// The OpenSubtitles provider's own language code (the same choices the
    /// Widgets search offered from the system locale). An unknown code is
    /// refused; search uses the last accepted one.
    Q_INVOKABLE void setSubtitleSearchLanguage(const QString& code);
    Q_INVOKABLE void searchSubtitles();
    Q_INVOKABLE void downloadSubtitle(int row);
    Q_INVOKABLE void closeSubtitleSearch();
    Q_INVOKABLE void showHoverPreview(double seconds);
    Q_INVOKABLE void hideHoverPreview();
    Q_INVOKABLE void cancelEnginePreparation();
    Q_INVOKABLE void handleDroppedUrls(const QList<QUrl>& urls);
    Q_INVOKABLE bool handleKey(int key, int modifiers);

    VideoSession* session() const { return session_; }
    QString caption() const { return caption_; }
    QString windowTitle() const;
    QString mediaPath() const { return url_; }
    QString mediaOrigin() const { return mediaOrigin_; }
    bool hasMedia() const;
    bool isDirectMedia() const { return directMedia_; }
    bool localMedia() const;
    bool hasVideo() const;
    int sourceWidth() const { return sourceWidth_; }
    int sourceHeight() const { return sourceHeight_; }
    double aspectRatio() const { return aspectRatio_; }
    QStringList videoBadges() const { return videoBadges_; }
    QString episodeLabel() const { return episodeLabel_; }
    bool hasPreviousEpisode() const { return hasPreviousEpisode_; }
    bool hasNextEpisode() const { return hasNextEpisode_; }
    bool seriesExpanded() const { return seriesExpanded_; }

    double duration() const { return duration_; }
    double position() const { return position_; }
    bool playing() const { return playing_; }
    bool enginePauseHold() const { return enginePauseHold_; }
    QString pauseHoldText() const;
    double volume() const { return volume_; }
    bool muted() const { return muted_; }
    double rate() const { return speed_; }
    QVariantList speedPresets() const;
    int videoScaleMode() const { return videoScaleMode_; }
    QVariantList timelineMarks() const { return timelineMarks_; }
    PlayerMenuModel* skipOffers() const { return skipOffers_; }
    double networkBytesPerSecond() const { return networkBytesPerSecond_; }
    double streamFramesPerSecond() const { return streamFramesPerSecond_; }

    QString qualityLabel() const { return qualityLabel_; }
    bool audioTracksAvailable() const { return audioTrackCount_ > 0; }
    bool subtitleTracksAvailable() const { return subtitleTrackCount_ > 0; }
    double subtitleDelay() const { return subtitleDelay_; }

    bool fullscreen() const { return fullscreen_; }
    ChromeState chromeState() const { return chromeState_; }
    QString stateName() const;
    bool autoHidden() const { return chromeState_ == ChromeState::FullscreenAutoHidden; }

    PlayerMenuModel* pictureMenu() const { return pictureMenu_; }
    PlayerMenuModel* pictureSubtitlesMenu() const { return pictureSubtitlesMenu_; }
    PlayerMenuModel* audioMenu() const { return audioMenu_; }
    PlayerMenuModel* subtitlesMenu() const { return subtitlesMenu_; }
    PlayerMenuModel* enhancementMenu() const { return enhancementMenu_; }
    PlayerMenuModel* qualityMenu() const { return qualityMenu_; }
    PlayerMenuModel* settingsMenu() const { return settingsMenu_; }
    PlayerMenuModel* seriesMenu() const { return seriesMenu_; }
    PlayerMenuModel* keyHintsMenu() const { return keyHintsMenu_; }
    PlayerMenuModel* remedyMenu() const { return remedyMenu_; }
    PlayerMenuModel* subtitleResults() const { return subtitleResults_; }

    int anime4kMode() const { return static_cast<int>(anime4kMode_); }
    QString anime4kChainId() const { return anime4kChainId_; }
    QString animeJanaiPresetId() const { return animeJanaiPresetId_; }
    QString animeJanaiStatus() const { return animeJanaiStatus_; }
    QString animeJanaiNote() const { return animeJanaiNote_; }
    QString animeJanaiBackendName() const;
    QString animeJanaiChipText() const;
    QString animeJanaiChipTooltip() const;
    QString enhancementChipText() const;
    QString enhancementChipTooltip() const;
    bool nativeProcessingRequested() const { return nativeRequestEnabled(); }
    bool nativeHudVisible() const { return nativeStatusLines_.visible; }
    bool nativeHudActive() const { return nativeStatusLines_.active; }
    QString nativeHudHeadline() const { return nativeStatusLines_.headline; }
    QString nativeHudState() const { return nativeStatusLines_.state; }
    QString nativeHudProvider() const { return nativeStatusLines_.provider; }
    QString nativeHudModel() const { return nativeStatusLines_.model; }
    QString nativeHudGeometry() const { return nativeStatusLines_.geometry; }
    QString nativeHudFps() const { return nativeStatusLines_.fps; }
    QString nativeHudInterpolation() const { return nativeStatusLines_.interpolation; }
    QString nativeHudDropped() const { return nativeStatusLines_.dropped; }
    QString nativeHudReason() const { return nativeStatusLines_.reason; }

    bool processingActive() const { return processingActive_; }
    QString processingReasonKey() const { return lastProcessingReasonKey_; }
    QString processingReason() const { return lastProcessingReason_; }
    QString processingMessage() const { return processingMessage_; }
    bool retryVisible() const { return retryOffered_; }
    QString statusText() const { return statusText_; }
    StatusMode statusMode() const { return statusMode_; }
    bool errorActive() const { return errorActive_; }
    bool runtimePending() const { return runtimePendingVisible_; }
    bool openingLoading() const { return openingLoading_; }
    QString openingHeadline() const { return openingHeadline_; }
    QString openingDetail() const { return openingDetail_; }
    bool compareLoading() const { return compareLoadingActive_; }

    QString preparationTitle() const { return preparationTitle_; }
    int preparationStageIndex() const { return preparationStageIndex_; }
    int preparationStageCount() const { return preparationStageCount_; }
    QString preparationPhase() const { return preparationPhase_; }
    QString preparationDetail() const { return preparationDetail_; }
    QString preparationSummary() const { return preparationSummary_; }
    bool preparationCancelPending() const { return enginePreparationCancelPending_; }

    bool resumeSheetVisible() const { return resumeSheetVisible_; }
    QString resumeSheetLabel() const { return resumeSheetLabel_; }
    double resumeSheetSeconds() const { return resumeSheetSeconds_; }
    quint64 resumeSheetGeneration() const { return resumeSheetGeneration_; }

    bool subtitleSearchVisible() const { return subtitleSearchVisible_; }
    QString subtitleSearchQuery() const { return subtitleSearchQuery_; }
    QString subtitleSearchLanguage() const { return subtitleSearchLanguage_; }
    QString subtitleSearchStatus() const { return subtitleSearchStatus_; }
    bool subtitleSearchBusy() const { return subtitleSearchBusy_; }
    bool subtitleDownloadBusy() const { return subtitleDownloadBusy_; }

    bool hoverPreviewVisible() const { return hoverPreviewVisible_; }
    QUrl hoverPreviewSource() const { return hoverPreviewSource_; }
    QString hoverPreviewTime() const { return hoverPreviewTime_; }
    QString hoverPreviewLabel() const { return hoverPreviewLabel_; }

    QColor accentColor() const { return accentColor_; }
    int cornerRadius() const { return cornerRadius_; }
    bool darkTheme() const { return darkTheme_; }
    bool notificationVisible() const { return notificationVisible_; }
    QString notificationText() const { return notificationText_; }
    bool notificationFailure() const { return notificationFailure_; }

    bool interactionActive() const { return interactionActive_; }
    bool menuStackVisible() const { return menuStackVisible_; }

signals:
    void episodePlaybackStarted(int playlistIndex);
    void episodePlaybackFinished(int playlistIndex);
    void episodeProgress(int playlistIndex, double position, double duration);
    void episodeResumeDeclined(int playlistIndex);
    void enginePreparationCancelRequested();
    void runtimeRequested();
    void nativeAdmissionRequested(const native::SelectionState& selection,
                                  const native::NativePreparationContext& context);
    void nativeAdmissionCancelled();
    void nativeReferenceFrameReady(const native::FrameFixture& frame, quint64 sourceEpoch,
                                   double framePosition, int frameIndex,
                                   std::optional<native::Rational> frameFps);
    void nativeReferenceFrameFailed(const QString& reasonKey, const QString& reason,
                                    int frameIndex);
    void processingSettingsRequested(shell::ProcessingSettingsIntent intent);
    void componentSetupRequested();
    void upscalersRequested();

    void menuOpenRequested(const QString& menuId);
    void menusCloseRequested();
    void shortcutsSheetRequested();
    void pictureFocusRequested();
    void alertRequested(const QString& message);
    void resumeSheetAttentionRequested(bool visible);
    void chromeWakeRequested();

    void mediaChanged();
    void episodeChanged();
    void episodeLabelChanged();
    void episodeSteppingChanged();
    void playlistChanged();
    void durationChanged();
    void positionChanged();
    void playingChanged();
    void enginePauseHoldChanged();
    void volumeChanged();
    void mutedChanged();
    void rateChanged();
    void videoScaleModeChanged();
    void qualityChanged();
    void variantsChanged();
    void trackAvailabilityChanged();
    void subtitleDelayChanged();
    void aspectRatioChanged();
    void sourceSizeChanged();
    void streamStatsChanged();
    void videoBadgesChanged();
    void skipSegmentsChanged();
    void chromeStateChanged();
    void fullscreenChanged();
    void pipChanged();
    void activeSourceChanged();
    void openingChanged();
    void compareLoadingChanged();
    void anime4kChanged();
    void animeJanaiChanged();
    void nativeHudChanged();
    void processingChanged();
    void statusChanged();
    void preparationChanged();
    void resumeSheetChanged();
    void subtitleSearchChanged();
    void hoverPreviewChanged();
    void themeChanged();
    void notificationChanged();
    void menusChanged();
    void seriesExpandedChanged();
    void interactionChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct VariantInfo {
        QString embed;
        QString label;
        QString display;
        QString track;
        QString kind;
        QString kindLabel;
        int quality = 0;
    };

    void buildModels();
    void connectSession();
    void connectWindow();
    void connectTheme();
    void installPresetShortcuts();
    void installWindowEventFilter();

    void flashStatus(const QString& message, bool failure = false);
    void showErrorStatus(const QString& message);
    void ensureOpenFailureVisible();
    void showProcessingStatus(const QString& reason);
    void showProcessingFailure(const QString& reasonKey, const QString& reason);
    bool hasProcessingFailure() const { return processingActive_; }
    void retireProcessingRow();
    void publishStatus();

    void showOpeningOverlay();
    void syncOpeningOverlay();
    void hideOpeningOverlay();
    QString runtimeVisibleDetail() const;
    void finishCompareLoading();
    void scheduleCompareStart(quint64 generation, int delayMs);
    void startCompareWindow(quint64 generation);
    void checkCompareLoading(quint64 generation);

    void onProcessingPrepareRequested();
    void onProcessingProvidersRequested();
    void onProcessingOriginalRequested();
    bool nativeEffectiveActive() const;
    QString processingFailureMessage(const QString& reason) const;

    void rebuildPictureMenu();
    void rebuildSubtitleMenus();
    void rebuildAudioMenu();
    void rebuildSubtitlesMenu();
    void rebuildEnhancementMenu();
    void rebuildQualityMenu();
    void rebuildSettingsMenu();
    void rebuildSeriesMenu();
    void rebuildKeyHintsMenu();
    void rebuildRemedyMenu();
    void rebuildSkipOffers();
    void rebuildTimelineMarks();
    void publishTrackAvailability();
    void refreshTrackMenus();
    QVector<PlayerMenuModel::Row> mediaTrackRows(const QString& kind) const;
    QVector<PlayerMenuModel::Row> trackRows(const QString& kind) const;
    void appendExternalTrackRow(QVector<PlayerMenuModel::Row>& rows, bool subtitle);
    void appendSubtitleDelayRows(QVector<PlayerMenuModel::Row>& rows);
    QVector<PlayerMenuModel::Row> capturePictureRows() const;
    QVector<PlayerMenuModel::Row> enhancementRootRows() const;
    QVector<PlayerMenuModel::Row> enhancementLevelRows(const QString& levelId) const;
    QVector<PlayerMenuModel::Row> videoScaleModeRows() const;
    QVector<PlayerMenuModel::Row> keyHintRows() const;
    QVector<PlayerMenuModel::Row> remedyRows() const;
    PlayerMenuModel* ensureSubmenuModel(const QString& menuId);
    /// Drops only levels the current catalogue/source no longer names. Called
    /// on catalogue/source/track/locale/settings changes, never on menu close.
    void pruneObsoleteSubmenuModels();
    void publishMenuAction(PlayerMenuModel::Action action, const QString& value,
                           const QString& rowId);
    static QVector<PlayerMenuModel::Row> makeRows(
        const QVector<native::menu::Row>& nativeRows);

    void probeUnknownTracks();
    void setVariantsVisibility();
    void watchMediaTracks();
    void onQualityPicked(int height);
    void onTrackPicked(const QString& kind, const QString& rowId, const QString& value);
    static QString trackRowText(const VariantInfo& variant);
    static QList<shell::QualityEntry> usableQualities(
        const QList<shell::QualityEntry>& qualities);
    static VariantInfo variantFromMap(const QVariantMap& map);

    void onMediaDropped(const QString& path);
    void copyStreamUrl();
    void openMediaFolder();
    void openMediaInformationWindow();
    void closeMediaInformation();
    void openCompareWindow();
    void openClipWindow();

    void openSubtitlesSearchDialog();
    void applySubtitleSearchReply(const QVariantMap& reply, const QString& apiKey);
    void applySubtitleDownloadReply(const QByteArray& bytes, const QString& error);
    QString subtitleSearchSeed() const;
    void publishSubtitleSearch();

    void applyHoverPreviewFrame(quint64 token, double requestedSeconds, double decodedSeconds,
                                const QImage& image);
    void clearHoverPreview();

    void onMediaLoaded(quint64 mediaToken);
    void onMediaEnded(quint64 mediaToken);
    void onEngineError(const QString& message);
    void onEngineDuration(double seconds);
    void offerResume(double seconds);
    void raiseResumeSheet(double seconds);
    void closeResumeSheet();
    void onEnginePosition(double seconds);
    void onEnginePlaying(bool playing);
    bool openStream(const QString& url, int height, bool resume = true);
    void restoreResumePoint();
    void applyPendingPause();
    void goToEpisode(int position, bool closeList = false);
    void setQualities(const QList<shell::QualityEntry>& qualities);
    int pickTargetHeight(int requested) const;
    /// Stable opaque media identity for the current playlist entry; 0 is
    /// reserved for direct media and for no current playlist entry.
    quint64 episodeMediaToken() const;
    /// Resolves only tokens belonging to the current playlist. Stale tokens
    /// from a replaced playlist and token 0 return -1.
    int episodeIndexFromToken(quint64 mediaToken) const;
    void applyEpisodeInternal(const QString& label, const QString& embed, int index,
                              const QList<shell::QualityEntry>& qualities,
                              const QVector<QVariantMap>& variants, bool preservePosition);

    void setChromeState(ChromeState state);
    void publishChromeState();
    void onWindowFullscreenChanged(bool fullscreen);
    void syncWindowMode();
    void applyPlaybackGeometry();
    bool onShellKey(int key, int modifiers);
    void enterPip();
    void leavePipInternal(bool closing = false);
    void publishPipState();
    /// Arms the single idle timer only when auto-hide is meaningful (fullscreen,
    /// no interaction hold, no open sheet/menu).
    void armIdleTimer();
    void restartIdleTimer();
    void suspendIdleTimer();
    bool idleHoldActive() const;
    void onIdleTimeout();

    void onAnimeJanaiStatusChanged(const QString& status);
    void onAnimeJanaiFailure(const QString& reason);
    void onAnimeJanaiEngineBuildChanged(bool active, bool succeeded, const QString& detail);
    void handleAnimeJanaiEngineBuildChanged(VideoSession* owner, bool active, bool succeeded,
                                            const QString& detail);
    void restateAnimeJanaiBuildLanguage();
    QString animeJanaiBuildDetailText() const;
    void publishAnimeJanai();
    void publishAnime4K();
    static QString animeJanaiBackendName(native::Backend backend);

    void onNativeStatusChanged(const native::NativeStatusView& status);
    void onNativePlaybackStatusChanged(const native::NativePlaybackController::Status& status);
    void onNativeProcessingFailure(const QString& reasonKey, const QString& reason);
    void onNativeReferenceFrameReady(const native::FrameFixture& frame, quint64 sourceEpoch,
                                     double framePosition, int frameIndex,
                                     std::optional<native::Rational> frameFps);
    void onNativeReferenceFrameFailed(const QString& reasonKey, const QString& reason,
                                      int frameIndex);
    void cancelPendingReferenceCapture(const QString& reasonKey, const QString& reason);
    void syncNativeStatusPolling(bool requested);
    bool nativeRequestEnabled() const;
    void announceNativeRequest();
    void publishNativeHud();
    void onNativeActionRequested(native::menu::Action action, const QString& value);

    void restatePreparation();
    void publishPreparation();

    void publishNotification(const QString& text, bool failure);
    void clearNotification();
    void publishTheme();

    void attachAuxiliaryTransient(QWidget* window) const;
    void closeAuxiliaryWindows();

    static constexpr int kStatusMs = 1900;
    static constexpr int kAutohideMs = 2500;

    std::unique_ptr<PlayerWindow> window_;
    /// Native Quick status window for engine preparation only; owns its own
    /// topmost enforcement and destroys it once hidden.
    std::unique_ptr<PreparationWindow> preparationWindow_;
    VideoSession* session_ = nullptr;
    shell::PlayerHost* host_ = nullptr;
    bool directMedia_ = false;

    QPointer<QWidget> compareWindow_;
    QPointer<QWidget> clipWindow_;
    QPointer<QWidget> mediaInformationWindow_;

    PlayerMenuModel* pictureMenu_ = nullptr;
    PlayerMenuModel* pictureSubtitlesMenu_ = nullptr;
    PlayerMenuModel* audioMenu_ = nullptr;
    PlayerMenuModel* subtitlesMenu_ = nullptr;
    PlayerMenuModel* enhancementMenu_ = nullptr;
    PlayerMenuModel* qualityMenu_ = nullptr;
    PlayerMenuModel* settingsMenu_ = nullptr;
    PlayerMenuModel* scaleMenu_ = nullptr;
    PlayerMenuModel* seriesMenu_ = nullptr;
    PlayerMenuModel* keyHintsMenu_ = nullptr;
    PlayerMenuModel* remedyMenu_ = nullptr;
    PlayerMenuModel* subtitleResults_ = nullptr;
    PlayerMenuModel* skipOffers_ = nullptr;
    QHash<QString, PlayerMenuModel*> submenuModels_;
    QVariantList timelineMarks_;

    QTimer* statusTimer_ = nullptr;
    QTimer* notificationTimer_ = nullptr;
    QTimer* idleTimer_ = nullptr;
    bool interactionActive_ = false;
    bool menuStackVisible_ = false;
    QString statusText_;
    StatusMode statusMode_ = StatusMode::None;
    QString backgroundActivity_;
    bool errorActive_ = false;
    bool runtimePendingVisible_ = false;
    bool retryOffered_ = false;
    bool processingActive_ = false;
    QString lastProcessingReasonKey_;
    QString lastProcessingReason_;
    QString processingMessage_;
    native::menu::StatusLines nativeStatusLines_;
    bool componentSetupRequestedOnce_ = false;
    bool refreshAttempted_ = false;

    bool openingLoading_ = false;
    QString openingHeadline_;
    QString openingDetail_;
    bool compareLoadingActive_ = false;
    quint64 compareLoadingGeneration_ = 0;
    uint64_t compareLoadingSourceEpoch_ = 0;
    bool compareLoadingStartScheduled_ = false;
    QTimer* compareStartTimer_ = nullptr;

    bool enginePreparationVisible_ = false;
    QString preparationTitle_;
    QString preparationPhase_;
    QString preparationDetail_;
    QString preparationSummary_;
    int preparationStageIndex_ = -1;
    int preparationStageCount_ = 0;
    quint64 enginePreparationGeneration_ = 0;
    bool enginePreparationCancelPending_ = false;
    uint64_t enginePreparationSourceEpoch_ = 0;
    bool enginePreparationActive_ = false;
    bool enginePreparationHeldIntent_ = false;

    bool resumeSheetVisible_ = false;
    double resumeSheetSeconds_ = 0.0;
    quint64 resumeSheetGeneration_ = 0;
    QString resumeSheetLabel_;
    bool resumeSheetWasPlaying_ = false;
    quint64 resumeSheetToken_ = 0;
    double episodeResumeSeconds_ = 0.0;
    double episodeSeekPending_ = 0.0;
    /// Media token whose resume box was already offered; direct media is 0.
    quint64 resumeOfferedToken_ = static_cast<quint64>(-1);
    bool resumePending_ = false;
    double resumePosition_ = 0.0;
    bool resumePaused_ = false;
    bool pauseAfterLoad_ = false;
    quint64 resumePauseGeneration_ = 0;
    double lastProgressSeconds_ = -1.0;
    int lastProgressEpisode_ = -1;

    QString caption_;
    QString url_;
    QString mediaOrigin_;
    QList<shell::QualityEntry> qualities_;
    QVector<VariantInfo> variants_;
    QVector<QVariantMap> playlist_;
    /// Opaque token at each current playlist index; regenerated only when the
    /// playlist is replaced, not when a source/quality for an entry is reopened.
    QVector<quint64> playlistMediaTokens_;
    quint64 nextEpisodeMediaToken_ = 1;
    QString currentEmbed_;
    int currentHeight_ = 0;
    int episodeIndex_ = 0;
    int lastFinishedEpisodeIndex_ = -1;
    bool hasPreviousEpisode_ = false;
    bool hasNextEpisode_ = false;
    bool seriesExpanded_ = false;
    bool engineReady_ = false;
    bool hasActiveSource_ = false;

    double duration_ = 0.0;
    double position_ = 0.0;
    bool playing_ = false;
    double volume_ = 0.7;
    bool muted_ = false;
    double speed_ = 1.0;
    double aspectRatio_ = 0.0;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    double networkBytesPerSecond_ = 0.0;
    double streamFramesPerSecond_ = 0.0;
    bool enginePauseHold_ = false;
    QString episodeLabel_;
    QString qualityLabel_;
    QStringList videoBadges_;
    int audioTrackCount_ = 0;
    int subtitleTrackCount_ = 0;
    double subtitleDelay_ = 0.0;
    std::unique_ptr<QTemporaryDir> subtitleTempDir_;
    int videoScaleMode_ = 0;

    anime4k::Mode anime4kMode_ = anime4k::Mode::Off;
    QString anime4kChainId_ = QStringLiteral("off");
    QString animeJanaiPresetId_;
    QString animeJanaiStatus_;
    QString animeJanaiNote_;
    native::Backend animeJanaiBackend_ = native::Backend::TensorRT;
    native::AnimeJanaiPlan animeJanaiPlan_;
    quint64 animeJanaiSequence_ = 0;
    quint64 animeJanaiPlanGeneration_ = 0;
    std::shared_ptr<std::atomic_bool> animeJanaiCancellation_;
    bool animeJanaiWaitingForSourceContext_ = false;
    uint64_t animeJanaiPreparationSourceEpoch_ = 0;
    uint64_t animeJanaiPendingPreparationSourceEpoch_ = 0;
    native::AnimeJanaiSourceContext animeJanaiLastPreparedSource_;
    quint64 animeJanaiBuildGeneration_ = 0;
    QString animeJanaiBuildDetailRaw_;
    QString animeJanaiBuildPresetId_;
    QString animeJanaiBuildPresetLabel_;
    int animeJanaiBuildWidth_ = 0;
    int animeJanaiBuildHeight_ = 0;
    int animeJanaiBuildStageIndex_ = -1;
    int animeJanaiBuildStageCount_ = 0;

    struct WorkerSlot {
        QPointer<PlayerController> controller;
    };
    std::shared_ptr<WorkerSlot> workerSlot_;

    native::menu::Inputs nativeInputs_;
    bool nativePlaybackRequested_ = false;
    bool nativeRequestAnnounced_ = false;
    native::SelectionState nativeRequestSelection_;
    std::optional<native::NativePreparationContext> nativeRequestContext_;
    QPointer<VideoSession> nativeReferenceSession_;
    quint64 nativeReferenceSourceEpoch_ = 0;
    int nativeReferenceFrameIndex_ = -1;
    bool nativeReferencePending_ = false;

    bool fullscreen_ = true;
    ChromeState chromeState_ = ChromeState::DefaultFit;
    bool pipActive_ = false;
    bool pipWasFullscreen_ = false;

    shell::FramePreviewService* previewService_ = nullptr;
    bool hoverPreviewVisible_ = false;
    QUrl hoverPreviewSource_;
    double hoverPreviewSeconds_ = -1.0;
    QString hoverPreviewTime_;
    QString hoverPreviewLabel_;
    quint64 hoverPreviewToken_ = 0;
    quint64 hoverPreviewPrewarmEpoch_ = 0;

    bool subtitleSearchVisible_ = false;
    QString subtitleSearchQuery_;
    QString subtitleSearchLanguage_;
    QString subtitleSearchStatus_;
    bool subtitleSearchBusy_ = false;
    bool subtitleDownloadBusy_ = false;
    QVector<shell::OpenSubtitlesHit> subtitleHits_;
    quint64 subtitleSearchToken_ = 0;

    QColor accentColor_;
    int cornerRadius_ = 0;
    bool darkTheme_ = false;
    bool notificationVisible_ = false;
    QString notificationText_;
    bool notificationFailure_ = false;

    bool closed_ = false;
    bool shuttingDown_ = false;
    bool shutdownComplete_ = false;
    bool forwardingKey_ = false;
};

}  // namespace quick
}  // namespace mplayer
