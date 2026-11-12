#include "PlayerController.hpp"

#include "PlayerMenuModel.hpp"
#include "PlayerWindow.hpp"

#include "../compare/CompareWindow.hpp"
#include "../core/VideoSession.hpp"
#include "../shell/ClipDialog.hpp"
#include "../shell/FramePreviewService.hpp"
#include "../shell/MediaInformationDialog.hpp"
#include "../shell/OpenSubtitlesClient.hpp"
#include "../shell/ShellTheme.hpp"
#include "../shaders/Anime4K.hpp"
#include "gui/Clipboard.hpp"
#include "gui/Win32Support.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLocale>
#include <QMetaObject>
#include <QRunnable>
#include <QSettings>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

namespace mplayer {
namespace quick {

namespace {

bool isLocalPath(const QString& url) {
    if (url.isEmpty()) return false;
    if (QDir::isAbsolutePath(url)) return true;
    return QUrl::fromUserInput(url).isLocalFile();
}

QStringList subtitleLanguageCodes() {
    QStringList codes;
    const QString system = QLocale::system().name().left(2).toLower();
    if (system.size() == 2) codes.append(system);
    for (const QString& code :
         {QStringLiteral("uk"), QStringLiteral("en"), QStringLiteral("ru"),
          QStringLiteral("pl"), QStringLiteral("de"), QStringLiteral("es")}) {
        if (!codes.contains(code)) codes.append(code);
    }
    return codes;
}

bool isSubtitleFile(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("srt") || suffix == QLatin1String("ass") ||
           suffix == QLatin1String("ssa") || suffix == QLatin1String("vtt") ||
           suffix == QLatin1String("sub");
}

}  // namespace

// ── variants / qualities ─────────────────────────────────────────────────

QList<shell::QualityEntry> PlayerController::usableQualities(
    const QList<shell::QualityEntry>& qualities) {
    QList<shell::QualityEntry> usable;
    for (const shell::QualityEntry& entry : qualities) {
        if (!entry.url.trimmed().isEmpty()) usable.append(entry);
    }
    return usable;
}

PlayerController::VariantInfo PlayerController::variantFromMap(const QVariantMap& map) {
    VariantInfo info;
    info.embed = map.value(QStringLiteral("embed")).toString();
    info.label = map.value(QStringLiteral("label")).toString();
    info.display = map.value(QStringLiteral("display")).toString();
    info.track = map.value(QStringLiteral("track")).toString();
    info.kind = map.value(QStringLiteral("kind")).toString();
    info.kindLabel = map.value(QStringLiteral("kind_label")).toString();
    info.quality = map.value(QStringLiteral("quality")).toInt();
    return info;
}

void PlayerController::startPlayer() {
    if (closed_ || url_.isEmpty()) return;
    episodeSeekPending_ = episodeResumeSeconds_;
    episodeResumeSeconds_ = 0.0;
    if (!openStream(url_, 0, /*resume=*/false)) {
        episodeSeekPending_ = 0.0;
        ensureOpenFailureVisible();
    }
}

void PlayerController::setQualities(const QList<shell::QualityEntry>& qualities) {
    if (directMedia_) return;
    qualities_ = usableQualities(qualities);
    if (!qualityLabel_.isEmpty() && currentHeight_ > 0) {
        bool offered = false;
        for (const shell::QualityEntry& entry : qualities_) {
            if (entry.height == currentHeight_) {
                offered = true;
                break;
            }
        }
        if (!offered) {
            currentHeight_ = pickTargetHeight(0);
            const QString label =
                currentHeight_ > 0 ? QStringLiteral("%1p").arg(currentHeight_) : QString();
            if (qualityLabel_ != label) {
                qualityLabel_ = label;
                emit qualityChanged();
            }
        }
    }
    rebuildQualityMenu();
    emit variantsChanged();
}

int PlayerController::pickTargetHeight(int requested) const {
    int best = 0;
    for (const shell::QualityEntry& entry : qualities_) best = std::max(best, entry.height);
    if (requested <= 0) return best;
    for (const shell::QualityEntry& entry : qualities_) {
        if (entry.height == requested) return requested;
    }
    return best;
}

// ── episode / source ─────────────────────────────────────────────────────

quint64 PlayerController::episodeMediaToken() const {
    if (directMedia_ || episodeIndex_ < 0 || episodeIndex_ >= playlistMediaTokens_.size()) {
        return 0;
    }
    return playlistMediaTokens_.at(episodeIndex_);
}

int PlayerController::episodeIndexFromToken(quint64 mediaToken) const {
    if (mediaToken == 0) return -1;
    return playlistMediaTokens_.indexOf(mediaToken);
}

void PlayerController::setEpisodeResumePoint(double seconds) {
    episodeResumeSeconds_ = (std::isfinite(seconds) && seconds > 0.0) ? seconds : 0.0;
}

void PlayerController::setPlaylist(const QVector<QVariantMap>& playlist, int index) {
    if (directMedia_ && !playlist.isEmpty()) return;
    const bool playlistReplaced = playlist_ != playlist;
    playlist_ = playlist;
    if (playlistReplaced) {
        playlistMediaTokens_.clear();
        playlistMediaTokens_.reserve(playlist_.size());
        for (qsizetype i = 0; i < playlist_.size(); ++i) {
            quint64 token = nextEpisodeMediaToken_++;
            if (token == 0) token = nextEpisodeMediaToken_++;
            playlistMediaTokens_.append(token);
        }
    }
    if (playlist_.isEmpty()) seriesExpanded_ = false;
    const int previousIndex = episodeIndex_;
    episodeIndex_ = (index >= 0 && index < playlist_.size()) ? index : 0;
    hasPreviousEpisode_ = episodeIndex_ > 0;
    hasNextEpisode_ = episodeIndex_ + 1 < playlist_.size();
    if (episodeIndex_ >= 0 && episodeIndex_ < playlist_.size()) {
        const QString label = playlist_.at(episodeIndex_).value(QStringLiteral("label")).toString();
        if (!label.isEmpty() && episodeLabel_ != label) {
            episodeLabel_ = label;
            emit episodeLabelChanged();
        }
    }
    if (previousIndex != episodeIndex_) emit episodeChanged();
    emit playlistChanged();
    emit episodeSteppingChanged();
    if (!playlist_.isEmpty()) noteUserActivity();
    rebuildSeriesMenu();
}

void PlayerController::stepEpisode(int delta) {
    goToEpisode(episodeIndex_ + delta);
}

void PlayerController::goToEpisode(int position, bool closeList) {
    if (closed_ || directMedia_ || position < 0 || position >= playlist_.size()) return;
    if (closeList) setSeriesExpanded(false);
    if (host_ != nullptr) host_->requestEpisode(position);
}

void PlayerController::selectEpisode(int playlistIndex) {
    goToEpisode(playlistIndex, true);
}

void PlayerController::goToNextEpisode() {
    stepEpisode(1);
}

void PlayerController::goToPreviousEpisode() {
    stepEpisode(-1);
}

void PlayerController::applyEpisodeInternal(const QString& label, const QString& embed, int index,
                                            const QList<shell::QualityEntry>& qualities,
                                            const QVector<QVariantMap>& variants,
                                            bool preservePosition) {
    episodeIndex_ = index;
    QVector<VariantInfo> parsed;
    parsed.reserve(variants.size());
    for (const QVariantMap& map : variants) parsed.append(variantFromMap(map));
    variants_ = parsed;
    setVariantsVisibility();
    currentEmbed_ = embed;
    setQualities(qualities);
    rebuildAudioMenu();
    rebuildSubtitlesMenu();
    const int target = autoQuality_ ? pickTargetHeight(0) : pickTargetHeight(currentHeight_);
    const QString nextCaption = target > 0
        ? QStringLiteral("%1 · %2p").arg(label).arg(target)
        : label;
    if (caption_ != nextCaption) {
        caption_ = nextCaption;
        emit mediaChanged();
    }
    if (episodeLabel_ != label) {
        episodeLabel_ = label;
        emit episodeLabelChanged();
    }
    rebuildSeriesMenu();
    rebuildPictureMenu();
    setPlaylist(playlist_, index);
    lastProgressEpisode_ = -1;
    lastProgressSeconds_ = -1.0;
    for (const shell::QualityEntry& entry : qualities_) {
        if (entry.height != target) continue;
        currentHeight_ = target;
        qualityLabel_ = target > 0 ? QStringLiteral("%1p").arg(target) : QString();
        openStream(entry.url, target, preservePosition);
        emit qualityChanged();
        return;
    }
}

void PlayerController::applyEpisode(const QString& label, const QString& embed, int index,
                                    const QList<shell::QualityEntry>& qualities,
                                    const QVector<QVariantMap>& variants) {
    episodeResumeSeconds_ = 0.0;
    if (closed_ || directMedia_ || qualities.isEmpty()) return;
    setSkipSegments({});
    applyEpisodeInternal(label, embed, index, qualities, variants, /*preservePosition=*/false);
    if (errorActive_) return;
    flashStatus(tr("Episode: %1").arg(label));
}

void PlayerController::applyVariant(const QList<shell::QualityEntry>& qualities,
                                    const QString& label, const QString& embed) {
    if (closed_ || directMedia_ || qualities.isEmpty()) return;
    if (!embed.isEmpty() && embed != currentEmbed_) {
        currentEmbed_ = embed;
        emit variantsChanged();
        rebuildAudioMenu();
        rebuildSubtitlesMenu();
    }
    setQualities(qualities);
    const int target = pickTargetHeight(0);
    for (const shell::QualityEntry& entry : qualities_) {
        if (entry.height != target) continue;
        currentHeight_ = target;
        qualityLabel_ = target > 0 ? QStringLiteral("%1p").arg(target) : QString();
        const QString nextCaption = target > 0
            ? QStringLiteral("%1 · %2p").arg(label).arg(target)
            : label;
        if (caption_ != nextCaption) {
            caption_ = nextCaption;
            emit mediaChanged();
        }
        if (episodeLabel_ != label) {
            episodeLabel_ = label;
            emit episodeLabelChanged();
        }
        emit qualityChanged();
        if (!openStream(entry.url, target, /*resume=*/true)) {
            ensureOpenFailureVisible();
            return;
        }
        flashStatus(tr("Track: %1").arg(label));
        return;
    }
}

void PlayerController::applyRefreshedStreams(const QList<shell::QualityEntry>& qualities,
                                             int height) {
    if (closed_ || directMedia_ || qualities.isEmpty()) return;
    setQualities(qualities);
    const int target = pickTargetHeight(height);
    for (const shell::QualityEntry& entry : qualities_) {
        if (entry.height != target) continue;
        currentHeight_ = target;
        qualityLabel_ = target > 0 ? QStringLiteral("%1p").arg(target) : QString();
        emit qualityChanged();
        if (!openStream(entry.url, target, /*resume=*/true)) {
            ensureOpenFailureVisible();
            return;
        }
        flashStatus(tr("Stream link has expired — refreshing"));
        return;
    }
}

void PlayerController::selectQuality(int height) {
    if (closed_ || directMedia_) return;
    if (height == 0) {
        autoQuality_ = true;
        const int target = pickTargetHeight(0);
        if (target > 0 && target != currentHeight_) {
            for (const shell::QualityEntry& entry : qualities_) {
                if (entry.height != target) continue;
                currentHeight_ = target;
                qualityLabel_ = QStringLiteral("%1p").arg(target);
                emit qualityChanged();
                if (!openStream(entry.url, target, /*resume=*/true)) {
                    ensureOpenFailureVisible();
                    return;
                }
                flashStatus(tr("Switched to Auto (%1p)").arg(target));
                rebuildQualityMenu();
                return;
            }
        }
        rebuildQualityMenu();
        flashStatus(tr("Quality set to Auto"));
        return;
    }
    autoQuality_ = false;
    for (const shell::QualityEntry& entry : qualities_) {
        if (entry.height != height) continue;
        currentHeight_ = height;
        qualityLabel_ = QStringLiteral("%1p").arg(height);
        emit qualityChanged();
        if (!openStream(entry.url, height, /*resume=*/true)) {
            ensureOpenFailureVisible();
            return;
        }
        flashStatus(tr("Switching to %1p…").arg(height));
        return;
    }
    flashStatus(tr("%1p is not available for this release").arg(height), true);
}

bool PlayerController::openStream(const QString& url, int height, bool resume) {
    if (closed_ || url.isEmpty()) return false;
    showOpeningOverlay();
    ++animeJanaiSequence_;
    if (animeJanaiCancellation_) animeJanaiCancellation_->store(true);
    if (errorActive_) {
        errorActive_ = false;
        retryOffered_ = false;
        if (statusTimer_ != nullptr) statusTimer_->stop();
        publishStatus();
    }
    retireProcessingRow();

    const bool rearmPreparationAfterOpen = enginePreparationActive_;
    const bool carriedPausedIntent = session_->playbackPauseIntent();
    resumePending_ = resume && session_->hasVideo();
    if (resumePending_) {
        resumePosition_ = session_->position();
        resumePaused_ = carriedPausedIntent;
    } else {
        pauseAfterLoad_ = session_->hasVideo() && carriedPausedIntent;
    }
    if (height > 0) {
        currentHeight_ = height;
        const QString label = QStringLiteral("%1p").arg(height);
        if (qualityLabel_ != label) {
            qualityLabel_ = label;
            emit qualityChanged();
        }
    }
    url_ = url;
    mediaOrigin_ = directMedia_ ? caption_ : mediaOrigin_;
    engineReady_ = session_->open(url, episodeMediaToken());
    if (rearmPreparationAfterOpen && engineReady_) {
        session_->setEnginePreparationPaused(true, true);
    }
    if (engineReady_) {
        runtimePendingVisible_ = false;
        resumePauseGeneration_ = session_->pauseRequestGeneration();
        if (previewService_ != nullptr) {
            previewService_->setSource(url, session_->sourceEpoch());
        }
    }
    rebuildQualityMenu();
    emit mediaChanged();
    if (!engineReady_) return false;
    refreshNativeInputs();
    if (!animeJanaiPresetId_.isEmpty()) applyAnimeJanaiRequest(animeJanaiPresetId_);
    return true;
}

void PlayerController::restoreResumePoint() {
    resumePending_ = false;
    const double limit = session_->duration();
    const double position = std::clamp(resumePosition_, 0.0, limit > 0.0 ? limit : resumePosition_);
    session_->seek(position);
    position_ = position;
    emit positionChanged();
    if (resumePauseGeneration_ != session_->pauseRequestGeneration()) return;
    if (resumePaused_) session_->setPaused(true);
}

void PlayerController::applyPendingPause() {
    if (!pauseAfterLoad_) return;
    pauseAfterLoad_ = false;
    if (resumePauseGeneration_ != session_->pauseRequestGeneration()) return;
    session_->setPaused(true);
}

// ── engine → controller ──────────────────────────────────────────────────

void PlayerController::onMediaLoaded(quint64 mediaToken) {
    if (closed_ || session_ == nullptr) return;
    if (mediaToken != episodeMediaToken()) return;
    if (errorActive_) {
        errorActive_ = false;
        retryOffered_ = false;
        publishStatus();
    }
    const int index = episodeIndexFromToken(mediaToken);
    if (index < 0) return;
    lastFinishedEpisodeIndex_ = -1;
    emit episodePlaybackStarted(index);
}

void PlayerController::onMediaEnded(quint64 mediaToken) {
    if (closed_ || session_ == nullptr) return;
    const int index = episodeIndexFromToken(mediaToken);
    if (index < 0 || index == lastFinishedEpisodeIndex_) return;
    lastFinishedEpisodeIndex_ = index;
    emit episodePlaybackFinished(index);
}

void PlayerController::onEngineError(const QString& message) {
    if (closed_) return;
    if (!engineReady_ && native::runtimeResolutionPending()) {
        showRuntimePendingStatus();
        emit runtimeRequested();
        return;
    }
    showErrorStatus(message.isEmpty() ? tr("Could not play the stream") : message);
    if (!directMedia_ && !refreshAttempted_) {
        refreshAttempted_ = true;
        if (host_ != nullptr) host_->playbackFailed();
    }
}

void PlayerController::showError(const QString& message) {
    onEngineError(message);
}

void PlayerController::onEngineDuration(double seconds) {
    if (closed_) return;
    const bool changed = !qFuzzyCompare(duration_ + 1.0, seconds + 1.0);
    duration_ = seconds;
    if (changed) emit durationChanged();
    if (resumePending_ && seconds > 0.0) restoreResumePoint();
    if (episodeSeekPending_ > 0.0 && seconds > 0.0) {
        const double target = std::min(episodeSeekPending_, seconds - 5.0);
        episodeSeekPending_ = 0.0;
        if (target > 0.0) {
            const quint64 token = episodeMediaToken();
            QMetaObject::invokeMethod(
                this,
                [this, target, token] {
                    if (closed_ || token != episodeMediaToken()) return;
                    if (resumeOfferedToken_ == token) return;
                    resumeOfferedToken_ = token;
                    offerResume(target);
                },
                Qt::QueuedConnection);
        }
    }
    if (changed && position_ > duration_ && duration_ > 0.0) {
        position_ = duration_;
        emit positionChanged();
    }
}

void PlayerController::onEnginePosition(double seconds) {
    if (closed_) return;
    if (resumePending_ || session_->duration() <= 0.0) return;
    const bool changed = !qFuzzyCompare(position_ + 1.0, seconds + 1.0);
    position_ = seconds;
    if (changed) emit positionChanged();
    if (!timelineMarks_.isEmpty()) rebuildSkipOffers();
    const double total = session_->duration();
    if (total <= 0.0 || episodeIndex_ < 0) return;
    if (episodeIndex_ != lastProgressEpisode_) {
        lastProgressEpisode_ = episodeIndex_;
        lastProgressSeconds_ = -1.0;
    }
    if (lastProgressSeconds_ < 0.0 || std::abs(seconds - lastProgressSeconds_) >= 5.0) {
        lastProgressSeconds_ = seconds;
        emit episodeProgress(episodeIndex_, seconds, total);
    }
}

void PlayerController::onEnginePlaying(bool playing) {
    if (closed_) return;
    if (resumePending_ && session_->duration() > 0.0) restoreResumePoint();
    if (playing_ != playing) {
        playing_ = playing;
        emit playingChanged();
    }
    applyPendingPause();
}

// ── resume sheet ─────────────────────────────────────────────────────────

void PlayerController::offerResume(double seconds) {
    raiseResumeSheet(seconds);
}

void PlayerController::raiseResumeSheet(double seconds) {
    if (closed_ || resumeSheetVisible_) return;
    if (!std::isfinite(seconds) || seconds <= 0.0) return;
    resumeSheetToken_ = episodeMediaToken();
    resumeSheetSeconds_ = seconds;
    resumeSheetLabel_ = formatTime(seconds);
    resumeSheetWasPlaying_ = session_ != nullptr && !session_->playbackPauseIntent();
    if (resumeSheetWasPlaying_) session_->setPaused(true);
    resumeSheetVisible_ = true;
    ++resumeSheetGeneration_;
    emit resumeSheetChanged();
    emit resumeSheetAttentionRequested(true);
    suspendIdleTimer();
}

void PlayerController::closeResumeSheet() {
    if (!resumeSheetVisible_) return;
    const bool resume = resumeSheetWasPlaying_;
    const quint64 token = resumeSheetToken_;
    resumeSheetVisible_ = false;
    resumeSheetWasPlaying_ = false;
    resumeSheetToken_ = 0;
    emit resumeSheetChanged();
    emit resumeSheetAttentionRequested(false);
    if (resume && session_ != nullptr && !closed_ && token == episodeMediaToken()) {
        session_->setPaused(false);
    }
    armIdleTimer();
}

void PlayerController::answerResume(quint64 generation, ResumeChoice choice) {
    if (!resumeSheetVisible_ || generation != resumeSheetGeneration_) return;
    const quint64 token = resumeSheetToken_;
    const bool wasPlaying = resumeSheetWasPlaying_;
    const double seconds = resumeSheetSeconds_;
    const QString label = resumeSheetLabel_;
    resumeSheetVisible_ = false;
    resumeSheetWasPlaying_ = false;
    resumeSheetToken_ = 0;
    emit resumeSheetChanged();
    emit resumeSheetAttentionRequested(false);
    if (!closed_ && token == episodeMediaToken()) {
        if (choice == ResumeChoice::Continue) {
            session_->seekExact(seconds);
            position_ = seconds;
            emit positionChanged();
            flashStatus(tr("Continued from %1").arg(label));
        } else if (choice == ResumeChoice::StartOver) {
            emit episodeResumeDeclined(episodeIndex_);
            flashStatus(tr("Starting from the beginning"));
        }
        if (wasPlaying) session_->setPaused(false);
    }
    armIdleTimer();
}

// ── playback owner ───────────────────────────────────────────────────────

bool PlayerController::hasActiveSource() const {
    return session_ != nullptr && session_->hasVideo();
}

bool PlayerController::pauseIfPlaying() {
    if (session_ == nullptr || !session_->isPlaying()) return false;
    session_->setPaused(true);
    return true;
}

void PlayerController::resumePlayback() {
    if (session_ == nullptr || session_->isPlaying()) return;
    session_->setPaused(false);
}

bool PlayerController::hasMedia() const {
    return !url_.isEmpty();
}

bool PlayerController::localMedia() const {
    return isLocalPath(url_);
}

bool PlayerController::hasVideo() const {
    return session_ != nullptr && session_->hasVideo();
}

// ── media commands ───────────────────────────────────────────────────────

void PlayerController::onMediaDropped(const QString& path) {
    if (closed_ || path.isEmpty()) return;
    directMedia_ = true;
    leavePip();
    qualities_.clear();
    currentHeight_ = 0;
    qualityLabel_.clear();
    currentEmbed_.clear();
    variants_.clear();
    setSeriesExpanded(false);
    setPlaylist({});
    setVariants({}, {});
    setSkipSegments({});
    rebuildAudioMenu();
    rebuildSubtitlesMenu();
    rebuildQualityMenu();
    emit qualityChanged();
    emit variantsChanged();
    emit trackAvailabilityChanged();
    caption_ = QFileInfo(path).fileName();
    mediaOrigin_ = caption_;
    episodeLabel_ = caption_;
    emit episodeLabelChanged();
    emit mediaChanged();
    if (!openStream(path, 0, /*resume=*/false)) {
        ensureOpenFailureVisible();
        return;
    }
    flashStatus(tr("Playing local file"));
}

void PlayerController::handleDroppedUrls(const QList<QUrl>& urls) {
    if (closed_ || urls.isEmpty()) return;
    QString mediaPath;
    QStringList subtitles;
    for (const QUrl& url : urls) {
        const QString local = url.isLocalFile() ? url.toLocalFile() : url.toString();
        if (local.isEmpty()) continue;
        if (isSubtitleFile(local)) {
            subtitles.append(local);
        } else if (mediaPath.isEmpty()) {
            mediaPath = local;
        }
    }
    if (mediaPath.isEmpty() && subtitles.isEmpty()) return;
    if (!mediaPath.isEmpty()) onMediaDropped(mediaPath);
    for (const QString& subtitle : subtitles) {
        if (session_ == nullptr || !session_->hasVideo()) {
            flashStatus(tr("The subtitle could not be loaded."), true);
            continue;
        }
        QString reason;
        if (!session_->addExternalTrack(subtitle, true, &reason)) {
            flashStatus(reason.isEmpty() ? tr("The subtitle could not be loaded.") : reason, true);
        } else {
            flashStatus(tr("Subtitle attached"));
        }
    }
    if (!subtitles.isEmpty()) refreshTrackMenus();
}

void PlayerController::copyStreamUrl() {
    if (closed_) return;
    if (!directMedia_) {
        if (host_ != nullptr) host_->requestCopyStreamUrl();
        return;
    }
    if (!mikai::gui::copyText(url_)) {
        flashStatus(tr("Failed to copy link"), true);
        return;
    }
    flashStatus(tr("Video link copied"));
}

void PlayerController::copyMediaLink() {
    copyStreamUrl();
}

void PlayerController::copyCurrentFrame() {
    if (closed_ || session_ == nullptr || !session_->hasVideo() || window_ == nullptr) {
        flashStatus(tr("No frame to copy"), true);
        return;
    }
    QString reason;
    const QImage frame = window_->grabVideoFrame(&reason);
    if (frame.isNull()) {
        flashStatus(reason.isEmpty() ? tr("Could not read the current frame") : reason, true);
        return;
    }
    if (!mikai::gui::copyImage(frame)) {
        flashStatus(tr("Failed to copy frame"), true);
        return;
    }
    flashStatus(tr("Frame copied (%1×%2)").arg(frame.width()).arg(frame.height()));
}

void PlayerController::openMediaFolder() {
    if (closed_) return;
    QString local = url_;
    if (!QDir::isAbsolutePath(local)) {
        const QUrl source = QUrl::fromUserInput(local);
        if (!source.isLocalFile()) {
            flashStatus(tr("This source is a stream — it has no folder"), true);
            return;
        }
        local = source.toLocalFile();
    }
    const QFileInfo media(local);
    if (!media.isFile()) {
        flashStatus(tr("The video file is no longer there"), true);
        return;
    }
    const QString folder = media.absolutePath();
    if (!QFileInfo(folder).isDir() || !QDesktopServices::openUrl(QUrl::fromLocalFile(folder))) {
        flashStatus(tr("Could not open the containing folder"), true);
        return;
    }
    flashStatus(tr("Containing folder opened"));
}

void PlayerController::openContainingFolder() {
    openMediaFolder();
}

void PlayerController::openMediaInformation() {
    openMediaInformationWindow();
}

void PlayerController::openMediaInformationWindow() {
    if (closed_ || !directMedia_ || session_ == nullptr) return;
    const QString local = session_->sourceUrl();
    if (local.isEmpty() ||
        (!QDir::isAbsolutePath(local) && !QUrl::fromUserInput(local).isLocalFile())) {
        return;
    }
    if (mediaInformationWindow_ == nullptr) {
        auto* dialog = new shell::MediaInformationDialog(session_);
        dialog->setAttribute(Qt::WA_DeleteOnClose, true);
        connect(dialog, &QDialog::finished, this, [this, dialog] {
            if (mediaInformationWindow_ == dialog) mediaInformationWindow_.clear();
        });
        attachAuxiliaryTransient(dialog);
        mediaInformationWindow_ = dialog;
    }
    mediaInformationWindow_->show();
    mediaInformationWindow_->raise();
    mediaInformationWindow_->activateWindow();
}

void PlayerController::closeMediaInformation() {
    if (mediaInformationWindow_ != nullptr) mediaInformationWindow_->close();
}

// ── Compare ──────────────────────────────────────────────────────────────

void PlayerController::openCompare() {
    openCompareWindow();
}

void PlayerController::openCompareWindow() {
    if (closed_ || session_ == nullptr || !session_->hasVideo()) {
        finishCompareLoading();
        if (!closed_) flashStatus(tr("No video to compare"), true);
        return;
    }
    if (window_ != nullptr && window_->compactPip()) {
        finishCompareLoading();
        flashStatus(tr("No video to compare"), true);
        return;
    }
    if (anime4k::findShadersDir().isEmpty()) {
        finishCompareLoading();
        flashStatus(tr("Shaders folder not found — comparison is unavailable"), true);
        return;
    }
    if (anime4k::availableModes().size() < 2) {
        finishCompareLoading();
        flashStatus(tr("Not enough variants to compare"), true);
        return;
    }
    if (compareLoadingActive_) return;

    auto* compare = qobject_cast<shell::CompareWindow*>(compareWindow_.data());
    if (compare != nullptr && compare->isVisible()) {
        if (compare->comparisonPending()) {
            compare->raise();
            compare->activateWindow();
            return;
        }
    }
    compareLoadingActive_ = true;
    compareLoadingSourceEpoch_ = session_->sourceEpoch();
    compareLoadingStartScheduled_ = false;
    const quint64 generation = ++compareLoadingGeneration_;
    emit compareLoadingChanged();
    armIdleTimer();
    scheduleCompareStart(generation, 80);
    QTimer::singleShot(250, this, [this, generation] { scheduleCompareStart(generation, 0); });
    if (window_ != nullptr) window_->requestRepaint();
}

void PlayerController::scheduleCompareStart(quint64 generation, int delayMs) {
    if (!compareLoadingActive_ || generation != compareLoadingGeneration_ ||
        compareLoadingStartScheduled_) {
        return;
    }
    compareLoadingStartScheduled_ = true;
    QTimer::singleShot(delayMs, this, [this, generation] { startCompareWindow(generation); });
}

void PlayerController::startCompareWindow(quint64 generation) {
    if (!compareLoadingActive_ || generation != compareLoadingGeneration_) return;
    if (closed_ || session_ == nullptr || !session_->hasVideo() ||
        session_->sourceEpoch() != compareLoadingSourceEpoch_) {
        finishCompareLoading();
        return;
    }
    auto* compare = qobject_cast<shell::CompareWindow*>(compareWindow_.data());
    if (compare == nullptr) {
        compare = new shell::CompareWindow(session_, anime4kChainId_, nullptr, animeJanaiBackend_);
        compare->setAttribute(Qt::WA_DeleteOnClose, true);
        connect(compare, &QDialog::finished, this, [this, compare] {
            if (compareWindow_ == compare) compareWindow_.clear();
        });
        attachAuxiliaryTransient(compare);
        compareWindow_ = compare;
    } else {
        compare->setCurrentCandidateId(anime4kChainId_);
    }
    compare->setAjiBackend(animeJanaiBackend_);
    if (!compare->isVisible()) {
        compare->show();
    } else {
        compare->captureNow();
    }
    if (!compareLoadingActive_ || generation != compareLoadingGeneration_ || closed_) return;
    if (!compare->isVisible() || session_ == nullptr || !session_->hasVideo() ||
        session_->sourceEpoch() != compareLoadingSourceEpoch_) {
        finishCompareLoading();
        return;
    }
    compare->raise();
    compare->activateWindow();
    checkCompareLoading(generation);
}

void PlayerController::checkCompareLoading(quint64 generation) {
    if (!compareLoadingActive_ || generation != compareLoadingGeneration_) return;
    auto* compare = qobject_cast<shell::CompareWindow*>(compareWindow_.data());
    if (closed_ || session_ == nullptr || !session_->hasVideo() ||
        session_->sourceEpoch() != compareLoadingSourceEpoch_ || compare == nullptr ||
        !compare->isVisible() || !compare->comparisonPending()) {
        finishCompareLoading();
        return;
    }
    QTimer::singleShot(50, this, [this, generation] { checkCompareLoading(generation); });
}

void PlayerController::finishCompareLoading() {
    if (!compareLoadingActive_) return;
    compareLoadingActive_ = false;
    compareLoadingStartScheduled_ = false;
    ++compareLoadingGeneration_;
    emit compareLoadingChanged();
    armIdleTimer();
}

// ── Clip ─────────────────────────────────────────────────────────────────

void PlayerController::openClipDialog() {
    openClipWindow();
}

void PlayerController::openClipWindow() {
    if (closed_ || session_ == nullptr || !session_->hasVideo() ||
        !std::isfinite(session_->duration()) || session_->duration() <= 0.0) {
        flashStatus(tr("A seekable video timeline is not available"), true);
        return;
    }
    if (clipWindow_ != nullptr) {
        clipWindow_->show();
        clipWindow_->raise();
        clipWindow_->activateWindow();
        return;
    }
    auto* dialog = new shell::ClipDialog(session_, caption_);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (clipWindow_ == dialog) clipWindow_.clear();
    });
    attachAuxiliaryTransient(dialog);
    clipWindow_ = dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

// ── tracks ───────────────────────────────────────────────────────────────

void PlayerController::setVariantsVisibility() {
    int audio = 0;
    int subtitles = 0;
    if (directMedia_) {
        if (session_ != nullptr && session_->mediaTracks().ready) {
            for (const MediaTrack& track : session_->mediaTracks().tracks) {
                if (track.type == MediaTrackType::Audio) {
                    ++audio;
                } else {
                    ++subtitles;
                }
            }
        }
    } else {
        for (const VariantInfo& variant : variants_) {
            if (variant.embed.isEmpty()) continue;
            if (variant.kind == QStringLiteral("sub")) {
                ++subtitles;
            } else {
                ++audio;
            }
        }
    }
    audioTrackCount_ = audio;
    subtitleTrackCount_ = subtitles;
    publishTrackAvailability();
}

void PlayerController::watchMediaTracks() {
    setVariantsVisibility();
    if (directMedia_) refreshTrackMenus();
}

void PlayerController::setVariants(const QVector<QVariantMap>& variants,
                                   const QString& currentEmbed) {
    if (directMedia_ && !variants.isEmpty()) return;
    QVector<VariantInfo> parsed;
    parsed.reserve(variants.size());
    for (const QVariantMap& map : variants) parsed.append(variantFromMap(map));
    variants_ = parsed;
    currentEmbed_ = currentEmbed;
    setVariantsVisibility();
    rebuildAudioMenu();
    rebuildSubtitlesMenu();
    rebuildPictureMenu();
    emit variantsChanged();
}

void PlayerController::setTrackQuality(const QString& embed, int quality) {
    if (closed_ || embed.isEmpty() || quality <= 0) return;
    bool changed = false;
    for (VariantInfo& variant : variants_) {
        if (variant.embed == embed && variant.quality != quality) {
            variant.quality = quality;
            changed = true;
        }
    }
    if (!changed) return;
    rebuildAudioMenu();
    rebuildSubtitlesMenu();
}

void PlayerController::probeUnknownTracks() {
    if (directMedia_ || host_ == nullptr) return;
    for (const VariantInfo& variant : variants_) {
        if (variant.embed.isEmpty() || variant.embed == currentEmbed_) continue;
        if (variant.quality <= 0) host_->requestTrackProbe(variant.embed);
    }
}

void PlayerController::openAudioMenu() {
    if (closed_) return;
    emit menuOpenRequested(QStringLiteral("audio"));
    noteUserActivity();
    if (audioTrackCount_ <= 0) flashStatus(tr("No other audio tracks available"));
    probeUnknownTracks();
}

void PlayerController::openSubtitlesMenu() {
    if (closed_) return;
    emit menuOpenRequested(QStringLiteral("subtitles"));
    noteUserActivity();
    probeUnknownTracks();
}

void PlayerController::requestExternalTrack(bool subtitle) {
    if (closed_ || session_ == nullptr) return;
    if (!subtitle && !localMedia()) {
        flashStatus(tr("External audio needs a local video"), true);
        return;
    }
    const QString filter = subtitle
        ? tr("Subtitles (*.srt *.ass *.ssa *.vtt *.sub);;All files (*)")
        : tr("Audio (*.mka *.aac *.ac3 *.flac *.mp3 *.opus *.ogg *.wav);;All files (*)");
    const uint64_t sourceEpoch = session_->sourceEpoch();
    const QString path = QFileDialog::getOpenFileName(
        nullptr, subtitle ? tr("Select subtitles from disk…") : tr("Select audio from disk…"),
        QString(), filter);
    if (path.isEmpty()) return;
    if (closed_ || session_ == nullptr || session_->sourceEpoch() != sourceEpoch ||
        !session_->hasVideo()) {
        return;
    }
    QString reason;
    if (!session_->addExternalTrack(path, subtitle, &reason)) {
        flashStatus(reason.isEmpty() ? tr("The track could not be loaded.") : reason, true);
        return;
    }
    flashStatus(subtitle ? tr("Subtitle attached") : tr("Audio track attached"));
    refreshTrackMenus();
}

void PlayerController::adjustSubtitleDelay(double delta) {
    if (!std::isfinite(delta)) return;
    const double next = std::clamp(subtitleDelay_ + delta, -30.0, 30.0);
    if (qFuzzyCompare(next + 1.0, subtitleDelay_ + 1.0)) return;
    subtitleDelay_ = next;
    if (session_ != nullptr) session_->setSubtitleDelay(subtitleDelay_);
    emit subtitleDelayChanged();
    rebuildSubtitlesMenu();
    flashStatus(tr("Subtitle delay %1 s").arg(subtitleDelay_, 0, 'f', 1));
}

void PlayerController::resetSubtitleDelay() {
    if (qFuzzyIsNull(subtitleDelay_)) return;
    subtitleDelay_ = 0.0;
    if (session_ != nullptr) session_->setSubtitleDelay(0.0);
    emit subtitleDelayChanged();
    rebuildSubtitlesMenu();
    flashStatus(tr("Subtitle delay 0.0 s"));
}

// ── skip segments ────────────────────────────────────────────────────────

void PlayerController::setSkipSegments(const QVector<QVariantMap>& segments) {
    if (closed_) return;
    QVariantList marks;
    marks.reserve(segments.size());
    for (const QVariantMap& map : segments) {
        const double start = map.value(QStringLiteral("start")).toDouble();
        const double end = map.value(QStringLiteral("end")).toDouble();
        if (!std::isfinite(start) || !std::isfinite(end) || end <= start) continue;
        QVariantMap mark;
        mark.insert(QStringLiteral("start"), start);
        mark.insert(QStringLiteral("end"), end);
        mark.insert(QStringLiteral("label"), map.value(QStringLiteral("label")).toString());
        mark.insert(QStringLiteral("type"), map.value(QStringLiteral("type")).toString());
        marks.append(mark);
    }
    timelineMarks_ = marks;
    rebuildTimelineMarks();
    rebuildSkipOffers();
    emit skipSegmentsChanged();
}

void PlayerController::activateSkipOffer(int row) {
    if (closed_ || skipOffers_ == nullptr) return;
    const PlayerMenuModel::Row entry = skipOffers_->row(row);
    if (!entry.enabled || entry.heading) return;
    bool ok = false;
    const int index = entry.value.toInt(&ok);
    if (!ok || index < 0 || index >= timelineMarks_.size()) return;
    const QVariantMap mark = timelineMarks_.at(index).toMap();
    const double end = mark.value(QStringLiteral("end")).toDouble();
    if (!std::isfinite(end) || end <= 0.0) return;
    const double limit = duration_ > 0.0 ? duration_ : session_ != nullptr ? session_->duration() : 0.0;
    const double target = limit > 0.0 ? std::min(end, limit) : end;
    if (session_ != nullptr) {
        session_->seek(target);
        position_ = target;
        emit positionChanged();
    }
    noteUserActivity();
}

// ── series ───────────────────────────────────────────────────────────────

void PlayerController::toggleSeries() {
    setSeriesExpanded(!seriesExpanded_);
}

void PlayerController::setSeriesExpanded(bool expanded) {
    const bool next = expanded && !directMedia_ && !playlist_.isEmpty();
    if (seriesExpanded_ == next) return;
    if (next) {
        emit menusCloseRequested();
        noteUserActivity();
    }
    seriesExpanded_ = next;
    emit seriesExpandedChanged();
    rebuildSeriesMenu();
    armIdleTimer();
}

// ── subtitle search ──────────────────────────────────────────────────────

QString PlayerController::subtitleSearchSeed() const {
    QString seed = caption_;
    const int tail = seed.lastIndexOf(QStringLiteral(" · "));
    if (tail > 0 && seed.endsWith(QLatin1Char('p'), Qt::CaseInsensitive)) seed.truncate(tail);
    seed = seed.trimmed();
    if (!seed.isEmpty()) return seed;
    const QString local = isLocalPath(url_) ? (QDir::isAbsolutePath(url_)
                                                   ? url_
                                                   : QUrl::fromUserInput(url_).toLocalFile())
                                            : QString();
    return QFileInfo(local).completeBaseName();
}

void PlayerController::openSubtitlesSearchDialog() {
    openSubtitleSearch();
}

void PlayerController::openEnhancementSettings() {
    if (closed_) return;
    emit processingSettingsRequested(shell::ProcessingSettingsIntent::Providers);
}

void PlayerController::openSubtitleSearch() {
    if (closed_) return;
    subtitleSearchVisible_ = true;
    subtitleSearchQuery_ = subtitleSearchSeed();
    const QString system = QLocale::system().name().left(2).toLower();
    subtitleSearchLanguage_ =
        subtitleLanguageCodes().contains(system) ? system : QStringLiteral("en");
    subtitleSearchStatus_.clear();
    subtitleHits_.clear();
    subtitleSearchBusy_ = false;
    subtitleDownloadBusy_ = false;
    ++subtitleSearchToken_;
    publishSubtitleSearch();
    suspendIdleTimer();
}

void PlayerController::setSubtitleSearchQuery(const QString& query) {
    if (subtitleSearchQuery_ == query) return;
    subtitleSearchQuery_ = query;
    emit subtitleSearchChanged();
}

void PlayerController::searchSubtitles() {
    if (closed_ || subtitleSearchBusy_) return;
    const QString query = subtitleSearchQuery_.trimmed();
    if (query.isEmpty()) {
        subtitleSearchStatus_ = tr("Enter a title or release name.");
        publishSubtitleSearch();
        return;
    }
    const QString apiKey = mikai::gui::loadOpenSubtitlesApiKey();
    if (apiKey.isEmpty()) {
        subtitleSearchStatus_ = tr("Add your OpenSubtitles API key in Settings › Access.");
        publishSubtitleSearch();
        return;
    }
    const quint64 token = ++subtitleSearchToken_;
    subtitleSearchBusy_ = true;
    subtitleDownloadBusy_ = false;
    subtitleSearchStatus_ = tr("Searching…");
    publishSubtitleSearch();

    const QString language = subtitleSearchLanguage_;
    const uint64_t sourceEpoch = session_ != nullptr ? session_->sourceEpoch() : 0;
    const std::shared_ptr<WorkerSlot> slot = workerSlot_;
    QThreadPool::globalInstance()->start(QRunnable::create([slot, token, apiKey, query, language,
                                                             sourceEpoch] {
        const shell::OpenSubtitlesSearch found = shell::searchOpenSubtitles(apiKey, query, language);
        QVariantMap reply;
        if (!found.error.isEmpty()) {
            reply.insert(QStringLiteral("error"), found.error);
        } else {
            QVariantList hits;
            for (const shell::OpenSubtitlesHit& hit : found.hits) {
                QVariantMap row;
                row.insert(QStringLiteral("fileId"), hit.fileId);
                row.insert(QStringLiteral("language"), hit.language);
                row.insert(QStringLiteral("release"), hit.releaseName);
                row.insert(QStringLiteral("downloads"), hit.downloads);
                hits.append(row);
            }
            reply.insert(QStringLiteral("hits"), hits);
        }
        QCoreApplication* app = QCoreApplication::instance();
        if (app == nullptr) return;
        QMetaObject::invokeMethod(
            app,
            [slot, token, sourceEpoch, reply, apiKey] {
                if (!slot || slot->controller == nullptr) return;
                PlayerController* controller = slot->controller.data();
                if (controller->closed_ || token != controller->subtitleSearchToken_ ||
                    controller->session_ == nullptr ||
                    controller->session_->sourceEpoch() != sourceEpoch) {
                    return;
                }
                controller->applySubtitleSearchReply(reply, apiKey);
            },
            Qt::QueuedConnection);
    }));
}

void PlayerController::applySubtitleSearchReply(const QVariantMap& reply, const QString& apiKey) {
    if (closed_) return;
    subtitleSearchBusy_ = false;
    subtitleDownloadBusy_ = false;
    if (reply.contains(QStringLiteral("error"))) {
        subtitleHits_.clear();
        subtitleSearchStatus_ = reply.value(QStringLiteral("error")).toString();
        publishSubtitleSearch();
        return;
    }
    if (!apiKey.isEmpty()) {
        QString reason;
        if (!mikai::gui::saveOpenSubtitlesApiKey(apiKey, &reason)) {
            qWarning().noquote() << "mikai: could not save the OpenSubtitles API key:" << reason;
        }
    }
    subtitleHits_.clear();
    const QVariantList hits = reply.value(QStringLiteral("hits")).toList();
    for (const QVariant& entry : hits) {
        const QVariantMap row = entry.toMap();
        shell::OpenSubtitlesHit hit;
        hit.fileId = row.value(QStringLiteral("fileId")).toInt();
        hit.language = row.value(QStringLiteral("language")).toString();
        hit.releaseName = row.value(QStringLiteral("release")).toString();
        hit.downloads = row.value(QStringLiteral("downloads")).toInt();
        if (hit.fileId > 0) subtitleHits_.append(hit);
    }
    subtitleSearchStatus_ = subtitleHits_.isEmpty()
        ? tr("Nothing found for that title.")
        : tr("%n subtitle(s) found", nullptr, int(subtitleHits_.size()));
    publishSubtitleSearch();
}

void PlayerController::downloadSubtitle(int row) {
    if (closed_ || subtitleDownloadBusy_ || row < 0 || row >= subtitleHits_.size()) return;
    const QString apiKey = mikai::gui::loadOpenSubtitlesApiKey();
    if (apiKey.isEmpty()) {
        subtitleSearchStatus_ = tr("Add your OpenSubtitles API key in Settings › Access.");
        publishSubtitleSearch();
        return;
    }
    const shell::OpenSubtitlesHit hit = subtitleHits_.at(row);
    if (hit.fileId <= 0) return;
    const quint64 token = subtitleSearchToken_;
    const uint64_t sourceEpoch = session_ != nullptr ? session_->sourceEpoch() : 0;
    const std::shared_ptr<WorkerSlot> slot = workerSlot_;
    subtitleDownloadBusy_ = true;
    subtitleSearchStatus_ = tr("Downloading…");
    publishSubtitleSearch();

    QThreadPool::globalInstance()->start(QRunnable::create([slot, token, sourceEpoch, apiKey, hit] {
        QString error;
        const QByteArray bytes = shell::downloadOpenSubtitles(apiKey, hit.fileId, &error);
        QCoreApplication* app = QCoreApplication::instance();
        if (app == nullptr) return;
        QMetaObject::invokeMethod(
            app,
            [slot, token, sourceEpoch, bytes, error] {
                if (!slot || slot->controller == nullptr) return;
                PlayerController* controller = slot->controller.data();
                if (controller->closed_ || token != controller->subtitleSearchToken_ ||
                    controller->session_ == nullptr ||
                    controller->session_->sourceEpoch() != sourceEpoch) {
                    return;
                }
                controller->applySubtitleDownloadReply(bytes, error);
            },
            Qt::QueuedConnection);
    }));
}

void PlayerController::applySubtitleDownloadReply(const QByteArray& bytes, const QString& error) {
    if (closed_) return;
    subtitleDownloadBusy_ = false;
    if (!error.isEmpty()) {
        subtitleSearchStatus_ = error;
        publishSubtitleSearch();
        return;
    }
    subtitleTempDir_ = std::make_unique<QTemporaryDir>();
    if (!subtitleTempDir_->isValid()) {
        subtitleSearchStatus_ = tr("No temporary folder for the subtitle.");
        publishSubtitleSearch();
        return;
    }
    const QString path = subtitleTempDir_->filePath(QStringLiteral("opensubtitles.srt"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || bytes.isEmpty() ||
        file.write(bytes) != bytes.size()) {
        subtitleSearchStatus_ = tr("The subtitle could not be written.");
        publishSubtitleSearch();
        return;
    }
    file.close();
    QString reason;
    if (session_ == nullptr || !session_->addExternalTrack(path, true, &reason)) {
        subtitleSearchStatus_ =
            reason.isEmpty() ? tr("The subtitle could not be loaded.") : reason;
        publishSubtitleSearch();
        return;
    }
    subtitleSearchStatus_ = tr("Attached.");
    publishSubtitleSearch();
    refreshTrackMenus();
    flashStatus(tr("Subtitle attached"));
}

void PlayerController::closeSubtitleSearch() {
    if (!subtitleSearchVisible_) return;
    subtitleSearchVisible_ = false;
    ++subtitleSearchToken_;
    subtitleSearchBusy_ = false;
    subtitleDownloadBusy_ = false;
    subtitleSearchStatus_.clear();
    publishSubtitleSearch();
    armIdleTimer();
}

void PlayerController::publishSubtitleSearch() {
    if (subtitleResults_ != nullptr) {
        QVector<PlayerMenuModel::Row> rows;
        rows.reserve(subtitleHits_.size());
        for (int i = 0; i < subtitleHits_.size(); ++i) {
            const shell::OpenSubtitlesHit& hit = subtitleHits_.at(i);
            PlayerMenuModel::Row row;
            row.id = QStringLiteral("hit/%1").arg(hit.fileId);
            row.label = tr("%1 · %2 · %3 downloads")
                            .arg(hit.releaseName, hit.language)
                            .arg(hit.downloads);
            row.tooltip = row.label;
            row.action = PlayerMenuModel::Action::DownloadSubtitle;
            row.value = QString::number(i);
            row.enabled = !subtitleDownloadBusy_;
            rows.append(row);
        }
        subtitleResults_->setRows(rows);
    }
    emit subtitleSearchChanged();
}

}  // namespace quick
}  // namespace mplayer
