#include "PlayerController.hpp"

#include "PlayerMenuModel.hpp"
#include "PlayerWindow.hpp"
#include "PreparationWindow.hpp"

#include "../core/VideoSession.hpp"
#include "../shell/FramePreviewService.hpp"
#include "../shell/ShellTheme.hpp"
#include "../shaders/Anime4K.hpp"
#include "../shaders/PresetShortcuts.hpp"
#include "gui/Clipboard.hpp"
#include "gui/SystemDefaultPlayer.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"
#include "native/components/NativeGpuProbe.hpp"

#include <QAccessible>
#include <QAccessibleEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QEvent>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLocale>
#include <QPalette>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QTimer>
#include <QTemporaryDir>
#include <QUrl>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace mplayer {
namespace quick {

namespace {

constexpr auto kAnime4KSettingKey = "player/anime4k";
constexpr auto kAnimeJanaiBackendSettingKey = "player/animejanai/backend";
constexpr auto kPresetShortcutsProperty = "mikaiPresetShortcuts";

constexpr double kShellRateMin = 0.25;
constexpr double kShellRateMax = 2.0;

QString episodeFromCaption(const QString& caption) {
    if (caption.contains(QStringLiteral(" · "))) {
        return caption.split(QStringLiteral(" · ")).first().trimmed();
    }
    return caption.trimmed();
}

}  // namespace

PlayerController::PlayerController(const QString& caption, const QString& url, QObject* parent)
    : PlayerController(caption, url, {}, 0, nullptr, parent) {}

PlayerController::PlayerController(const QString& caption, const QString& url,
                                   const QList<shell::QualityEntry>& qualities, int initialHeight,
                                   shell::PlayerHost* host, QObject* parent)
    : QObject(parent),
      host_(host),
      directMedia_(host == nullptr),
      caption_(caption),
      url_(url),
      mediaOrigin_(caption),
      qualities_(usableQualities(qualities)),
      currentHeight_(initialHeight) {
    const QString initialEpisode = episodeFromCaption(caption_);
    if (!initialEpisode.isEmpty()) episodeLabel_ = initialEpisode;

    session_ = new VideoSession(this);
    session_->setVolume(volume_ * 100.0);
    session_->setDiagnosticsPolling(false);

    QSettings animeJanaiSettings;
    const QString storedBackend = animeJanaiSettings.value(kAnimeJanaiBackendSettingKey).toString();
    const auto& gpu = native::observeComputeCapability();
    auto backend = native::backendFromName(storedBackend);
    if (!backend.has_value() ||
        !native::describeAnimeJanaiInstallation(*backend, false).installed) {
        if (native::isNvidiaRtx(gpu) &&
            native::describeAnimeJanaiInstallation(native::Backend::TensorRTRTX, false).installed) {
            backend = native::Backend::TensorRTRTX;
        } else if (native::describeAnimeJanaiInstallation(native::Backend::TensorRT, false)
                       .installed) {
            backend = native::Backend::TensorRT;
        } else if (native::describeAnimeJanaiInstallation(native::Backend::DirectML, false)
                       .installed) {
            backend = native::Backend::DirectML;
        }
        if (backend.has_value()) {
            animeJanaiSettings.setValue(kAnimeJanaiBackendSettingKey,
                                        animeJanaiBackendName(*backend));
        }
    }
    animeJanaiBackend_ =
        backend.has_value() ? *backend : native::defaultAnimeJanaiBackend(gpu);
#ifdef Q_OS_LINUX
    if (animeJanaiBackend_ == native::Backend::DirectML) {
        animeJanaiBackend_ = native::Backend::TensorRT;
    }
#endif
    session_->setAnime4KMode(anime4kMode_);

    statusTimer_ = new QTimer(this);
    statusTimer_->setSingleShot(true);
    connect(statusTimer_, &QTimer::timeout, this, [this] {
        if (errorActive_ || processingActive_) return;
        statusMode_ = StatusMode::None;
        statusText_.clear();
        publishStatus();
    });

    notificationTimer_ = new QTimer(this);
    notificationTimer_->setSingleShot(true);
    notificationTimer_->setInterval(3000);
    connect(notificationTimer_, &QTimer::timeout, this, &PlayerController::clearNotification);

    idleTimer_ = new QTimer(this);
    idleTimer_->setSingleShot(true);
    connect(idleTimer_, &QTimer::timeout, this, &PlayerController::onIdleTimeout);

    previewService_ = new shell::FramePreviewService(this);
    connect(previewService_, &shell::FramePreviewService::frameReady, this,
            [this](quint64 token, double requestedSeconds, double decodedSeconds,
                   const QImage& frame) {
                applyHoverPreviewFrame(token, requestedSeconds, decodedSeconds, frame);
            });
    connect(previewService_, &shell::FramePreviewService::frameUnavailable, this,
            [this](quint64 token, double, const QString&) {
                if (token == hoverPreviewToken_) clearHoverPreview();
            });

    buildModels();
    connectSession();
    connectTheme();
    installWindowEventFilter();
    installPresetShortcuts();

    QWindow* transientParent = qobject_cast<QWindow*>(parent);
    if (transientParent == nullptr) {
        if (auto* widget = qobject_cast<QWidget*>(parent)) {
            if (QWidget* top = widget->window()) transientParent = top->windowHandle();
        }
    }
    window_ = std::make_unique<PlayerWindow>(this, transientParent);
    connectWindow();
    preparationWindow_ = std::make_unique<PreparationWindow>(this, window_.get());
    connect(preparationWindow_.get(), &PreparationWindow::dismissed, this, [this] {
        if (!enginePreparationVisible_ && !enginePreparationCancelPending_) return;
        enginePreparationVisible_ = false;
        enginePreparationCancelPending_ = false;
        emit preparationChanged();
    });

    if (currentHeight_ <= 0) currentHeight_ = pickTargetHeight(0);
    qualityLabel_ = currentHeight_ > 0 ? QStringLiteral("%1p").arg(currentHeight_) : QString();

    publishTheme();
    window_->loadPlayerScene();
}

PlayerController::~PlayerController() {
    closed_ = true;
    if (statusTimer_) statusTimer_->stop();
    if (notificationTimer_) notificationTimer_->stop();
    if (idleTimer_) idleTimer_->stop();
    if (window_) window_->hideWindow();
    closeAuxiliaryWindows();
    finishCompareLoading();
    if (previewService_) previewService_->shutdown();
    if (preparationWindow_) {
        disconnect(preparationWindow_.get(), nullptr, this, nullptr);
        preparationWindow_->dismiss();
        preparationWindow_.reset();
    }
    ++animeJanaiSequence_;
    if (animeJanaiCancellation_) animeJanaiCancellation_->store(true);
    if (workerSlot_) workerSlot_->controller = nullptr;
    enginePreparationActive_ = false;
    enginePreparationVisible_ = false;
    if (session_ != nullptr) {
        disconnect(session_, nullptr, this, nullptr);
        session_->close();
    }
    window_.reset();
}

// ── assembly ─────────────────────────────────────────────────────────────

void PlayerController::buildModels() {
    pictureMenu_ = new PlayerMenuModel(this);
    pictureSubtitlesMenu_ = new PlayerMenuModel(this);
    audioMenu_ = new PlayerMenuModel(this);
    subtitlesMenu_ = new PlayerMenuModel(this);
    enhancementMenu_ = new PlayerMenuModel(this);
    qualityMenu_ = new PlayerMenuModel(this);
    settingsMenu_ = new PlayerMenuModel(this);
    scaleMenu_ = new PlayerMenuModel(this);
    seriesMenu_ = new PlayerMenuModel(this);
    keyHintsMenu_ = new PlayerMenuModel(this);
    remedyMenu_ = new PlayerMenuModel(this);
    subtitleResults_ = new PlayerMenuModel(this);
    skipOffers_ = new PlayerMenuModel(this);

    workerSlot_ = std::make_shared<WorkerSlot>();
    workerSlot_->controller = this;

    rebuildPictureMenu();
    rebuildSubtitleMenus();
    rebuildAudioMenu();
    rebuildSubtitlesMenu();
    rebuildEnhancementMenu();
    rebuildQualityMenu();
    rebuildSettingsMenu();
    rebuildSeriesMenu();
    rebuildKeyHintsMenu();
    rebuildRemedyMenu();
    rebuildSkipOffers();
    rebuildTimelineMarks();
    emit menusChanged();
}

void PlayerController::connectSession() {
    connect(session_, &VideoSession::errorOccurred, this, &PlayerController::onEngineError);
    connect(session_, &VideoSession::mediaLoaded, this, &PlayerController::onMediaLoaded);
    connect(session_, &VideoSession::mediaEnded, this, &PlayerController::onMediaEnded);
    connect(session_, &VideoSession::durationChanged, this, &PlayerController::onEngineDuration);
    connect(session_, &VideoSession::positionChanged, this, &PlayerController::onEnginePosition);
    connect(session_, &VideoSession::playingChanged, this, &PlayerController::onEnginePlaying);

    connect(session_, &VideoSession::videoSizeChanged, this, [this](int width, int height) {
        if (sourceWidth_ == width && sourceHeight_ == height) return;
        sourceWidth_ = width;
        sourceHeight_ = height;
        if (width > 0 && height > 0) aspectRatio_ = double(width) / double(height);
        emit sourceSizeChanged();
        emit mediaChanged();
        emit aspectRatioChanged();
    });
    connect(session_, &VideoSession::displayAspectRatioChanged, this, [this](double ratio) {
        if (qFuzzyCompare(aspectRatio_ + 1.0, ratio + 1.0)) return;
        aspectRatio_ = ratio;
        emit aspectRatioChanged();
    });
    connect(session_, &VideoSession::streamStatsChanged, this,
            [this](double bytesPerSecond, double framesPerSecond, StreamFpsSource) {
                networkBytesPerSecond_ = bytesPerSecond;
                streamFramesPerSecond_ = framesPerSecond;
                emit streamStatsChanged();
            });
    connect(session_, &VideoSession::enginePauseHoldChanged, this, [this](bool held) {
        if (enginePauseHold_ == held) return;
        enginePauseHold_ = held;
        emit enginePauseHoldChanged();
    });
    connect(session_, &VideoSession::openingChanged, this, [this](bool opening) {
        if (opening) {
            syncOpeningOverlay();
        } else {
            hideOpeningOverlay();
        }
    });
    connect(session_, &VideoSession::sourceEpochChanged, this, [this](uint64_t epoch) {
        cancelPendingReferenceCapture(
            QStringLiteral("native.reference.sourceChanged"),
            tr("The media source changed before the reference frame could be captured."));
        if (subtitleSearchBusy_ || subtitleDownloadBusy_) {
            ++subtitleSearchToken_;
            subtitleSearchBusy_ = false;
            subtitleDownloadBusy_ = false;
            subtitleSearchStatus_.clear();
            emit subtitleSearchChanged();
        }
        hideHoverPreview();
        if (previewService_ != nullptr && session_ != nullptr) {
            previewService_->setSource(session_->sourceUrl(), epoch);
        }
        hoverPreviewPrewarmEpoch_ = 0;
        if (compareLoadingActive_) finishCompareLoading();
        if (clipWindow_ != nullptr) {
            clipWindow_->close();
            clipWindow_->deleteLater();
            clipWindow_.clear();
        }
        if (mediaInformationWindow_ != nullptr) mediaInformationWindow_->close();
        if (session_ != nullptr) {
            sourceWidth_ = session_->videoWidth();
            sourceHeight_ = session_->videoHeight();
            emit sourceSizeChanged();
        }
    });
    connect(session_, &VideoSession::videoMetadataChanged, this,
            [this](const VideoSession::VideoMetadata& meta) {
        QStringList badges;
        if (meta.isHdr && !meta.isDolbyVision) {
            badges.append(meta.hdrFormat.isEmpty() ? QStringLiteral("HDR") : meta.hdrFormat);
        }
        if (meta.isDolbyVision) badges.append(QStringLiteral("Dolby Vision"));
        if (meta.hasDolbyAtmos) badges.append(QStringLiteral("Dolby Atmos"));
        QString codec;
        const QString raw = meta.videoCodec;
        if (raw.contains(QStringLiteral("hevc")) || raw.contains(QStringLiteral("hvc"))) {
            codec = QStringLiteral("HEVC");
        } else if (raw.contains(QStringLiteral("avc")) || raw.contains(QStringLiteral("h264"))) {
            codec = QStringLiteral("H.264");
        } else if (raw.contains(QStringLiteral("av01")) || raw.contains(QStringLiteral("av1"))) {
            codec = QStringLiteral("AV1");
        } else if (raw.contains(QStringLiteral("vp9"))) {
            codec = QStringLiteral("VP9");
        } else if (raw.contains(QStringLiteral("vp8"))) {
            codec = QStringLiteral("VP8");
        } else if (!raw.isEmpty()) {
            codec = raw.toUpper();
        }
        if (!codec.isEmpty()) badges.append(codec);
        if (videoBadges_ != badges) {
            videoBadges_ = badges;
            emit videoBadgesChanged();
        }
    });
    connect(session_, &VideoSession::mediaTracksChanged, this, [this] {
        setVariantsVisibility();
        if (directMedia_) refreshTrackMenus();
    });
    connect(session_, &VideoSession::mediaTracksError, this, [this](const QString& reason) {
        if (!closed_ && directMedia_) flashStatus(reason);
    });
    connect(session_, &VideoSession::seekRequested, this, [this] {
        if (previewService_ != nullptr) previewService_->noteActivity();
    });
    connect(session_, &VideoSession::anime4kGated, this, [this](const QString& notice) {
        flashStatus(notice);
    });
    connect(session_, &VideoSession::displayScalingRefused, this, [this](const QString& reason) {
        flashStatus(reason);
    });

    // Processing-owned reactions.
    connect(session_, &VideoSession::animeJanaiStatusChanged, this,
            &PlayerController::onAnimeJanaiStatusChanged);
    connect(session_, &VideoSession::animeJanaiFailure, this,
            &PlayerController::onAnimeJanaiFailure);
    connect(session_, &VideoSession::animeJanaiEngineBuildChanged, this,
            &PlayerController::onAnimeJanaiEngineBuildChanged);
    connect(session_, &VideoSession::nativeStatusChanged, this,
            &PlayerController::onNativeStatusChanged);
    connect(session_, &VideoSession::nativePlaybackStatusChanged, this,
            &PlayerController::onNativePlaybackStatusChanged);
    connect(session_, &VideoSession::nativeProcessingFailure, this,
            &PlayerController::onNativeProcessingFailure);
    connect(session_, &VideoSession::nativeReferenceFrameReady, this,
            &PlayerController::onNativeReferenceFrameReady);
    connect(session_, &VideoSession::nativeReferenceFrameFailed, this,
            &PlayerController::onNativeReferenceFrameFailed);

    if (previewService_ != nullptr) previewService_->setSource(QString(), 0);
    syncAnimeJanaiEngineBuildState(session_);
    refreshNativeInputs();
}

void PlayerController::connectWindow() {
    if (window_ == nullptr) return;
    connect(window_.get(), &PlayerWindow::closeRequested, this, &PlayerController::requestClose);
    connect(window_.get(), &PlayerWindow::fullscreenChanged, this,
            &PlayerController::onWindowFullscreenChanged);
    connect(window_.get(), &PlayerWindow::compactPipChanged, this,
            [this](bool) { publishPipState(); });
    connect(window_.get(), &PlayerWindow::sceneLoadFailed, this, [this](const QString& error) {
        showErrorStatus(tr("Could not open the player window: %1").arg(error));
    });
    connect(window_.get(), &PlayerWindow::framePresented, this, [this] {
        hideOpeningOverlay();
        if (previewService_ != nullptr) previewService_->noteActivity();
        if (compareLoadingActive_) scheduleCompareStart(compareLoadingGeneration_, 80);
    });
    connect(window_.get(), &PlayerWindow::sceneReady, this, [this] {
        if (window_ != nullptr && window_->isVisible()) window_->raiseWindow();
    });
}

void PlayerController::connectTheme() {
    connect(&shell::accentNotifier(), &shell::AccentNotifier::changed, this,
            [this](const QColor&) { publishTheme(); });
}

void PlayerController::publishTheme() {
    const QColor accent = shell::accent();
    const int radius = int(std::lround(shell::cornerRadius()));
    const QPalette palette = QGuiApplication::palette();
    const bool dark = palette.color(QPalette::Window).lightness() < 128;
    if (accentColor_ == accent && cornerRadius_ == radius && darkTheme_ == dark) return;
    accentColor_ = accent;
    cornerRadius_ = radius;
    darkTheme_ = dark;
    emit themeChanged();
}

void PlayerController::installPresetShortcuts() {
    QSettings settings;
    settings.beginGroup(QLatin1String(shell::kPresetShortcutSettingsGroup));
    const QStringList ids = settings.childKeys();
    settings.endGroup();

    QVariantMap bindings;
    QMap<QString, shell::ShortcutClaim> claims;
    for (const QString& id : ids) {
        const QString stored = settings.value(QLatin1String(shell::kPresetShortcutSettingsGroup) +
                                              QLatin1Char('/') + id)
                                   .toString();
        if (stored.isEmpty()) continue;
        const QKeySequence sequence = QKeySequence::fromString(stored, QKeySequence::PortableText);
        if (sequence.isEmpty()) continue;
        const shell::ShortcutConflict conflict = shell::findShortcutConflict(sequence, claims);
        if (conflict.blocked) continue;
        shell::ShortcutClaim claim;
        claim.ownerId = id;
        claim.ownerLabel = id;
        claim.kind = shell::ShortcutOwnerKind::Preset;
        claims.insert(shell::shortcutClaimKey(sequence), claim);
        bindings.insert(shell::shortcutClaimKey(sequence), id);
    }
    setProperty(kPresetShortcutsProperty, bindings);
}

void PlayerController::installWindowEventFilter() {
    if (window_ != nullptr) window_->installEventFilter(this);
    QObject::installEventFilter(this);
}

// ── window ───────────────────────────────────────────────────────────────

void PlayerController::showWindow() {
    if (closed_ || window_ == nullptr) return;
    window_->setCompactPip(pipActive_);
    window_->setFullscreenIntent(fullscreen_);
    window_->applyWindowMode();
    if (fullscreen_ && !pipActive_) {
        window_->showFullscreen();
    } else {
        window_->showWindowed();
    }
    window_->raiseWindow();
    window_->installEventFilter(this);
    setChromeState(fullscreen_ && !pipActive_ ? ChromeState::FullscreenActive
                                              : ChromeState::DefaultFit);
    noteUserActivity();
}

void PlayerController::hideWindow() {
    if (window_ != nullptr) window_->hideWindow();
}

void PlayerController::minimizeWindow() {
    if (window_ != nullptr) window_->showMinimized();
}

bool PlayerController::requestClose() {
    if (shutdownComplete_ || closed_) return true;
    if (shuttingDown_) return shutdownComplete_;
    if (window_ == nullptr) return true;

    if (session_->hasVideo() && session_->duration() > 0.0 && episodeIndex_ >= 0) {
        emit episodeProgress(episodeIndex_, session_->position(), session_->duration());
    }

    const bool pauseIntentAtClose = session_->playbackPauseIntent();
    const quint64 pauseGenerationAtClose = session_->pauseRequestGeneration();
    const bool runtimePendingWasVisible = runtimePendingVisible_;
    const bool enginePreparationWasActive = enginePreparationActive_;
    const bool statusTimerWasActive = statusTimer_ != nullptr && statusTimer_->isActive();

    shuttingDown_ = true;
    closed_ = true;
    hideOpeningOverlay();
    finishCompareLoading();
    closeAuxiliaryWindows();
    if (idleTimer_) idleTimer_->stop();
    if (statusTimer_) statusTimer_->stop();

    enginePreparationActive_ = false;
    ++animeJanaiSequence_;
    if (animeJanaiCancellation_) animeJanaiCancellation_->store(true);
    if (workerSlot_) workerSlot_->controller = nullptr;
    if (previewService_ != nullptr) previewService_->cancel();

    session_->setPaused(true);
    const bool settled = session_->settleNativeForTeardown();
    if (!settled && !session_->nativeTeardownSettleAbandoned()) {
        qWarning().noquote()
            << "mikai: native stop unsettled (native.playback.teardownUnsettled); close refused";
        closed_ = false;
        shuttingDown_ = false;
        if (workerSlot_) workerSlot_->controller = this;
        if (enginePreparationWasActive) enginePreparationActive_ = true;
        if (session_->pauseRequestGeneration() == pauseGenerationAtClose) {
            session_->setPaused(pauseIntentAtClose);
        }
        if (runtimePendingWasVisible && !engineReady_ &&
            native::runtimeResolutionPending()) {
            showRuntimePendingStatus();
        } else if (statusTimerWasActive && statusTimer_ != nullptr) {
            statusTimer_->start();
        }
        flashStatus(tr("Native processing did not acknowledge its stop; the player stays open so "
                       "it can be retried."));
        return false;
    }
    if (!settled) {
        qWarning().noquote()
            << "mikai: native stop unacknowledged (native.playback.teardownUnsettled); closing "
               "after the bounded settle";
    }

    closeResumeSheet();
    if (subtitleSearchVisible_) {
        subtitleSearchVisible_ = false;
        ++subtitleSearchToken_;
        subtitleSearchBusy_ = false;
        subtitleDownloadBusy_ = false;
        emit subtitleSearchChanged();
    }
    leavePipInternal(/*closing=*/true);
    if (preparationWindow_) {
        preparationWindow_->dismiss();
        preparationWindow_.reset();
    }
    session_->close();
    window_->acceptClose();
    shutdownComplete_ = true;
    if (host_ != nullptr) host_->playerClosed();
    return true;
}

bool PlayerController::isWindowVisible() const {
    return window_ != nullptr && window_->isVisible();
}

// ── transport ────────────────────────────────────────────────────────────

void PlayerController::togglePlayPause() {
    if (closed_ || session_ == nullptr) return;
    if (!session_->hasVideo() || url_.isEmpty()) {
        flashStatus(tr("No media loaded — drop a file to begin"));
        return;
    }
    noteUserActivity();
    session_->togglePause();
}

void PlayerController::seekTo(double seconds) {
    if (closed_ || session_ == nullptr || !session_->hasVideo()) return;
    const double limit = duration_ > 0.0 ? duration_ : session_->duration();
    const double target = std::clamp(std::isfinite(seconds) ? seconds : 0.0, 0.0, std::max(0.0, limit));
    session_->seek(target);
    if (std::abs(position_ - target) > 0.001) {
        position_ = target;
        emit positionChanged();
    }
    noteUserActivity();
}

void PlayerController::seekBy(double seconds) {
    if (closed_ || session_ == nullptr || !session_->hasVideo()) return;
    if (!std::isfinite(seconds) || qFuzzyIsNull(seconds)) return;
    session_->seekRelative(seconds);
    noteUserActivity();
}

void PlayerController::skipForward() {
    if (session_ == nullptr || !session_->hasVideo()) {
        flashStatus(tr("No media loaded — drop a file to begin"));
        return;
    }
    seekTo(position_ + 85.0);
}

void PlayerController::setVolume(double volume) {
    const double value = std::clamp(std::isfinite(volume) ? volume : volume_, 0.0, 1.0);
    if (qFuzzyCompare(value + 1.0, volume_ + 1.0)) return;
    volume_ = value;
    if (session_ != nullptr) session_->setVolume(volume_ * 100.0);
    emit volumeChanged();
    flashStatus(tr("Volume %1%").arg(int(std::lround(volume_ * 100.0))));
}

void PlayerController::adjustVolume(double delta) {
    setVolume(volume_ + delta);
}

void PlayerController::toggleMute() {
    muted_ = !muted_;
    if (session_ != nullptr) session_->setMuted(muted_);
    emit mutedChanged();
    flashStatus(muted_ ? tr("Muted") : tr("Unmuted"));
}

void PlayerController::setRate(double rate) {
    if (!std::isfinite(rate)) return;
    const double value = std::clamp(rate, kShellRateMin, kShellRateMax);
    if (qFuzzyCompare(value + 1.0, speed_ + 1.0)) return;
    speed_ = value;
    if (session_ != nullptr) session_->setSpeed(speed_);
    emit rateChanged();
    flashStatus(tr("Speed %1×").arg(speed_, 0, 'f', 2));
}

void PlayerController::setVideoScaleMode(int mode) {
    if (mode < int(VideoScaleMode::Fit) || mode > int(VideoScaleMode::Original)) return;
    if (videoScaleMode_ == mode) return;
    videoScaleMode_ = mode;
    VideoSession::setVideoScaleMode(static_cast<VideoScaleMode>(mode));
    emit videoScaleModeChanged();
}

void PlayerController::makeDefaultVideoPlayer() {
    QString error;
    if (!mikai::gui::requestSystemDefaultPlayer(&error)) {
        flashStatus(error, true);
        return;
    }
#ifdef Q_OS_LINUX
    flashStatus(tr("In desktop settings, choose Mikai Ja Nai under Default Applications. Your "
                   "defaults have not been changed."));
#else
    flashStatus(tr("Choose Mikai Ja Nai in Windows Default Apps. Your defaults have not been "
                   "changed."));
#endif
}

QString PlayerController::formatTime(double seconds) const {
    const double value = std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
    const int total = int(value);
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(secs, 2, 10, QLatin1Char('0'));
}

QString PlayerController::formatRemaining(double seconds) const {
    const double total = duration_ > 0.0 ? duration_ : (session_ != nullptr ? session_->duration() : 0.0);
    const double value = std::isfinite(seconds) ? seconds : 0.0;
    return QStringLiteral("-%1").arg(formatTime(std::max(0.0, total - value)));
}

void PlayerController::retryPlayback() {
    if (closed_ || url_.isEmpty()) return;
    if (!engineReady_) {
        if (native::runtimeResolutionPending()) {
            showRuntimePendingStatus();
        } else if (!errorActive_) {
            showErrorStatus(tr("Could not play the stream"));
        }
        emit runtimeRequested();
        return;
    }
    if (!openStream(url_, currentHeight_, /*resume=*/false)) ensureOpenFailureVisible();
}

// ── presentation state / idle ────────────────────────────────────────────

void PlayerController::setInteractionActive(bool active) {
    if (interactionActive_ == active) return;
    interactionActive_ = active;
    emit interactionChanged();
    if (active) {
        suspendIdleTimer();
        if (previewService_ != nullptr) previewService_->noteActivity();
    } else {
        restartIdleTimer();
    }
}

void PlayerController::setMenuStackVisible(bool visible) {
    if (menuStackVisible_ == visible) return;
    menuStackVisible_ = visible;
    emit interactionChanged();
    if (visible) {
        suspendIdleTimer();
    } else {
        restartIdleTimer();
    }
}

void PlayerController::noteUserActivity() {
    if (closed_) return;
    if (previewService_ != nullptr) previewService_->noteActivity();
    if (chromeState_ == ChromeState::FullscreenAutoHidden) {
        setChromeState(ChromeState::FullscreenActive);
        emit chromeWakeRequested();
    }
    restartIdleTimer();
}

void PlayerController::setChromeState(ChromeState state) {
    if (chromeState_ == state) {
        armIdleTimer();
        return;
    }
    chromeState_ = state;
    publishChromeState();
    armIdleTimer();
}

void PlayerController::publishChromeState() {
    emit chromeStateChanged();
}

void PlayerController::syncWindowMode() {
    if (window_ == nullptr) return;
    window_->setCompactPip(pipActive_);
    window_->setFullscreenIntent(fullscreen_);
    window_->applyWindowMode();
}

void PlayerController::applyPlaybackGeometry() {
    syncWindowMode();
}

bool PlayerController::idleHoldActive() const {
    return interactionActive_ || menuStackVisible_ || resumeSheetVisible_ ||
           subtitleSearchVisible_ || compareLoadingActive_ || openingLoading_ ||
           enginePreparationVisible_;
}

void PlayerController::armIdleTimer() {
    if (idleTimer_ == nullptr) return;
    if (fullscreen_ && !pipActive_ && chromeState_ == ChromeState::FullscreenActive &&
        !idleHoldActive()) {
        idleTimer_->start(kAutohideMs);
    } else {
        idleTimer_->stop();
    }
}

void PlayerController::restartIdleTimer() {
    if (idleTimer_ == nullptr) return;
    if (fullscreen_ && !pipActive_ && chromeState_ == ChromeState::FullscreenActive &&
        !idleHoldActive()) {
        idleTimer_->start(kAutohideMs);
    }
}

void PlayerController::suspendIdleTimer() {
    if (idleTimer_ != nullptr) idleTimer_->stop();
}

void PlayerController::onIdleTimeout() {
    if (closed_) return;
    if (idleHoldActive()) return;
    if (!fullscreen_ || pipActive_ || window_ == nullptr ||
        !window_->windowStates().testFlag(Qt::WindowFullScreen)) {
        if (!fullscreen_ || pipActive_) {
            setChromeState(ChromeState::DefaultFit);
        }
        return;
    }
    setChromeState(ChromeState::FullscreenAutoHidden);
    emit menusCloseRequested();
}

void PlayerController::onWindowFullscreenChanged(bool fullscreen) {
    if (closed_ || pipActive_) return;
    if (fullscreen_ == fullscreen) return;
    fullscreen_ = fullscreen;
    emit fullscreenChanged();
    setChromeState(fullscreen ? ChromeState::FullscreenActive : ChromeState::DefaultFit);
}

void PlayerController::toggleFullscreen() {
    if (closed_ || window_ == nullptr) return;
    if (pipActive_) {
        leavePipInternal(false);
        return;
    }
    fullscreen_ = !fullscreen_;
    window_->setFullscreenIntent(fullscreen_);
    window_->applyWindowMode();
    if (fullscreen_) {
        window_->showFullscreen();
    } else {
        window_->showWindowed();
    }
    emit fullscreenChanged();
    setChromeState(fullscreen_ ? ChromeState::FullscreenActive : ChromeState::DefaultFit);
}

void PlayerController::beginWindowMove() {
    if (window_ != nullptr && !closed_) window_->beginSystemMove();
}

void PlayerController::beginWindowResize(int edges) {
    if (window_ == nullptr || closed_) return;
    window_->beginSystemResize(Qt::Edges(edges));
}

// ── PiP ──────────────────────────────────────────────────────────────────

void PlayerController::togglePip() {
    if (closed_) return;
    if (pipActive_) {
        leavePipInternal(false);
    } else {
        enterPip();
    }
}

void PlayerController::leavePip() {
    leavePipInternal(false);
}

void PlayerController::enterPip() {
    if (closed_ || pipActive_ || window_ == nullptr) return;
    pipWasFullscreen_ = fullscreen_;
    emit menusCloseRequested();
    window_->setCompactPip(true);
    window_->showWindowed();
    window_->raiseWindow();
    pipActive_ = true;
    publishPipState();
    setChromeState(ChromeState::DefaultFit);
    flashStatus(tr("Picture in picture"));
}

void PlayerController::leavePipInternal(bool closing) {
    if (!pipActive_) return;
    pipActive_ = false;
    if (window_ != nullptr) {
        if (!closing) {
            fullscreen_ = pipWasFullscreen_;
            window_->setFullscreenIntent(fullscreen_);
        }
        window_->setCompactPip(false);
        if (!closing) {
            if (fullscreen_) {
                window_->showFullscreen();
            } else {
                window_->showWindowed();
            }
        }
    }
    publishPipState();
    if (!closing) {
        setChromeState(fullscreen_ ? ChromeState::FullscreenActive : ChromeState::DefaultFit);
    }
}

void PlayerController::publishPipState() {
    emit pipChanged();
}

// ── status ───────────────────────────────────────────────────────────────

void PlayerController::publishStatus() {
    retryOffered_ = errorActive_;
    emit statusChanged();
    emit processingChanged();
}

void PlayerController::reportActionFailure(const QString& message) {
    flashStatus(message, /*failure=*/true);
    publishNotification(message, /*failure=*/true);
}

void PlayerController::flashStatus(const QString& message, bool failure) {
    if (closed_ || message.isEmpty()) return;
    runtimePendingVisible_ = false;
    statusText_ = message;
    if (errorActive_) {
        statusMode_ = StatusMode::Error;
    } else if (processingActive_) {
        statusMode_ = StatusMode::Processing;
    } else {
        statusMode_ = StatusMode::Transient;
        if (statusTimer_ != nullptr) statusTimer_->start(kStatusMs);
    }
    publishStatus();
    if (failure) {
        emit alertRequested(message);
        if (QAccessible::isActive() && window_ != nullptr) {
            QAccessibleAnnouncementEvent event(window_.get(), message);
            QAccessible::updateAccessibility(&event);
        }
    }
}

void PlayerController::showErrorStatus(const QString& message) {
    if (closed_) return;
    errorActive_ = true;
    runtimePendingVisible_ = false;
    if (statusTimer_ != nullptr) statusTimer_->stop();
    hideOpeningOverlay();
    statusText_ = message;
    statusMode_ = StatusMode::Error;
    retryOffered_ = true;
    publishStatus();
    emit alertRequested(message);
    if (QAccessible::isActive() && window_ != nullptr) {
        QAccessibleAnnouncementEvent event(window_.get(), message);
        QAccessible::updateAccessibility(&event);
    }
}

void PlayerController::ensureOpenFailureVisible() {
    if (closed_) return;
    if (!engineReady_ && native::runtimeResolutionPending()) {
        showRuntimePendingStatus();
        emit runtimeRequested();
        return;
    }
    if (!errorActive_) showErrorStatus(tr("Could not play the stream"));
    if (!directMedia_ && !refreshAttempted_) {
        refreshAttempted_ = true;
        if (host_ != nullptr) host_->playbackFailed();
    }
    emit runtimeRequested();
}

void PlayerController::showRuntimePendingStatus() {
    if (closed_) return;
    const QString text = native::runtimePendingPlaybackText();
    if (runtimePendingVisible_ && statusText_ == text) return;
    runtimePendingVisible_ = true;
    statusText_ = text;
    statusMode_ = StatusMode::RuntimePending;
    retryOffered_ = false;
    if (statusTimer_ != nullptr) statusTimer_->stop();
    syncOpeningOverlay();
    publishStatus();
}

void PlayerController::setBackgroundActivity(const QString& message) {
    if (closed_) return;
    backgroundActivity_ = message;
    if (message.isEmpty()) {
        if (statusTimer_ != nullptr && statusTimer_->isActive()) return;
        if (!errorActive_ && !processingActive_ && statusText_.isEmpty()) {
            statusMode_ = StatusMode::None;
            publishStatus();
        }
        return;
    }
    statusText_ = message;
    statusMode_ = StatusMode::Background;
    if (statusTimer_ != nullptr) statusTimer_->stop();
    publishStatus();
}

void PlayerController::publishNotification(const QString& text, bool failure) {
    if (closed_ || text.isEmpty()) return;
    notificationText_ = text;
    notificationFailure_ = failure;
    notificationVisible_ = true;
    if (notificationTimer_ != nullptr) notificationTimer_->start();
    emit notificationChanged();
}

void PlayerController::clearNotification() {
    if (!notificationVisible_) return;
    notificationVisible_ = false;
    notificationText_.clear();
    notificationFailure_ = false;
    emit notificationChanged();
}

// ── opening overlay ──────────────────────────────────────────────────────

void PlayerController::showOpeningOverlay() {
    if (closed_) return;
    const bool wasLoading = openingLoading_;
    openingLoading_ = true;
    openingHeadline_ = tr("Opening stream…");
    const QString detail = runtimeVisibleDetail();
    openingDetail_ = detail.isEmpty() ? caption_ : detail;
    if (!wasLoading) emit openingChanged();
    syncOpeningOverlay();
    armIdleTimer();
}

void PlayerController::syncOpeningOverlay() {
    if (closed_ || !openingLoading_) return;
    const QString detail = runtimeVisibleDetail();
    const QString next = detail.isEmpty() ? caption_ : detail;
    if (openingDetail_ == next) return;
    openingDetail_ = next;
    emit openingChanged();
}

void PlayerController::hideOpeningOverlay() {
    if (!openingLoading_) return;
    openingLoading_ = false;
    emit openingChanged();
    armIdleTimer();
}

QString PlayerController::runtimeVisibleDetail() const {
    return runtimePendingVisible_ ? statusText_ : QString();
}

// ── sheets ───────────────────────────────────────────────────────────────

void PlayerController::showHoverPreview(double seconds) {
    if (closed_ || !std::isfinite(seconds) || previewService_ == nullptr) return;
    hoverPreviewSeconds_ = seconds;
    hoverPreviewTime_ = formatTime(seconds);
    hoverPreviewLabel_ = episodeLabel_.isEmpty() ? caption_ : episodeLabel_;
    const quint64 token = ++hoverPreviewToken_;
    hoverPreviewVisible_ = true;
    emit hoverPreviewChanged();
    previewService_->requestFrame(token, seconds);
}

void PlayerController::hideHoverPreview() {
    if (hoverPreviewToken_ != 0) ++hoverPreviewToken_;
    if (previewService_ != nullptr) previewService_->cancel();
    if (!hoverPreviewVisible_ && hoverPreviewSource_.isEmpty()) return;
    hoverPreviewVisible_ = false;
    hoverPreviewSource_.clear();
    hoverPreviewSeconds_ = -1.0;
    hoverPreviewTime_.clear();
    hoverPreviewLabel_.clear();
    if (window_ != nullptr) window_->clearHoverPreviewFrame();
    emit hoverPreviewChanged();
}

void PlayerController::applyHoverPreviewFrame(quint64 token, double requestedSeconds,
                                               double decodedSeconds, const QImage& image) {
    if (closed_ || window_ == nullptr || token != hoverPreviewToken_ || image.isNull()) return;
    if (std::abs(requestedSeconds - hoverPreviewSeconds_) > 0.75) return;
    window_->setHoverPreviewFrame(token, image);
    const QUrl source = window_->hoverPreviewSource();
    hoverPreviewSource_ = source;
    hoverPreviewVisible_ = !source.isEmpty();
    hoverPreviewTime_ = formatTime(decodedSeconds >= 0.0 ? decodedSeconds : requestedSeconds);
    emit hoverPreviewChanged();
}

void PlayerController::clearHoverPreview() {
    if (!hoverPreviewVisible_ && hoverPreviewSource_.isEmpty()) return;
    hoverPreviewVisible_ = false;
    hoverPreviewSource_.clear();
    if (window_ != nullptr) window_->clearHoverPreviewFrame();
    emit hoverPreviewChanged();
}

// ── key input ────────────────────────────────────────────────────────────

bool PlayerController::handleKey(int key, int modifiers) {
    if (closed_) return false;
    if (forwardingKey_) return false;
    forwardingKey_ = true;
    const bool consumed = onShellKey(key, modifiers);
    forwardingKey_ = false;
    return consumed;
}

bool PlayerController::onShellKey(int key, int modifiers) {
    const bool shift = (modifiers & Qt::ShiftModifier) != 0;
    const bool control = (modifiers & Qt::ControlModifier) != 0;
    const bool alt = (modifiers & Qt::AltModifier) != 0;

    if (key == Qt::Key_Escape) {
        return dismissOverlay();
    }
    if (control || alt) {
        const QVariantMap bindings = property(kPresetShortcutsProperty).toMap();
        if (!bindings.isEmpty() && !shift) {
            const QKeySequence sequence(modifiers | key);
            const QString id = bindings.value(shell::shortcutClaimKey(sequence)).toString();
            if (!id.isEmpty()) {
                applyAnime4KPreset(id);
                noteUserActivity();
                return true;
            }
        }
        return false;
    }

    switch (key) {
        case Qt::Key_Space:
        case Qt::Key_K:
            noteUserActivity();
            togglePlayPause();
            return true;
        case Qt::Key_Left:
            seekBy(shift ? -10.0 : -5.0);
            return true;
        case Qt::Key_Right:
            seekBy(shift ? 10.0 : 5.0);
            return true;
        case Qt::Key_Up:
            adjustVolume(0.1);
            return true;
        case Qt::Key_Down:
            adjustVolume(-0.1);
            return true;
        case Qt::Key_F:
            noteUserActivity();
            toggleFullscreen();
            return true;
        case Qt::Key_M:
            toggleMute();
            return true;
        case Qt::Key_P:
            noteUserActivity();
            togglePip();
            return true;
        case Qt::Key_S:
            if (!playlist_.isEmpty()) {
                toggleSeries();
                return true;
            }
            break;
        case Qt::Key_T:
            openAudioMenu();
            return true;
        case Qt::Key_C:
            openSubtitlesMenu();
            return true;
        case Qt::Key_Q:
            emit menuOpenRequested(QStringLiteral("settings"));
            return true;
        case Qt::Key_Question:
        case Qt::Key_Slash:
            emit shortcutsSheetRequested();
            return true;
        default:
            break;
    }

    const QVariantMap bindings = property(kPresetShortcutsProperty).toMap();
    if (!bindings.isEmpty()) {
        const QKeySequence sequence(modifiers | key);
        const QString id = bindings.value(shell::shortcutClaimKey(sequence)).toString();
        if (!id.isEmpty()) {
            applyAnime4KPreset(id);
            noteUserActivity();
            return true;
        }
    }
    return false;
}

bool PlayerController::eventFilter(QObject* watched, QEvent* event) {
    if (closed_) return false;
    switch (event->type()) {
        case QEvent::LanguageChange:
            if (watched != window_.get()) break;
            rebuildPictureMenu();
            rebuildSubtitleMenus();
            rebuildAudioMenu();
            rebuildSubtitlesMenu();
            rebuildEnhancementMenu();
            rebuildQualityMenu();
            rebuildSettingsMenu();
            rebuildSeriesMenu();
            rebuildKeyHintsMenu();
            rebuildRemedyMenu();
            rebuildSkipOffers();
            publishSubtitleSearch();
            emit mediaChanged();
            emit enginePauseHoldChanged();
            publishAnime4K();
            publishAnimeJanai();
            publishNativeHud();
            publishStatus();
            publishPreparation();
            break;
        case QEvent::WindowActivate:
            if (watched == window_.get()) noteUserActivity();
            break;
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::Wheel:
            noteUserActivity();
            break;
        default:
            break;
    }
    return QObject::eventFilter(watched, event);
}

// ── getters ──────────────────────────────────────────────────────────────

QString PlayerController::windowTitle() const {
    return directMedia_ ? tr("Player — %1").arg(caption_) : caption_;
}

QVariantList PlayerController::speedPresets() const {
    QVariantList presets;
    for (double speed : {0.5, 1.0, 1.2, 1.5, 1.7, 2.0}) presets.append(speed);
    return presets;
}

bool PlayerController::dismissOverlay() {
    if (closed_) return false;
    if (resumeSheetVisible_) {
        closeResumeSheet();
        return true;
    }
    if (subtitleSearchVisible_) {
        subtitleSearchVisible_ = false;
        subtitleSearchBusy_ = false;
        subtitleDownloadBusy_ = false;
        subtitleSearchStatus_.clear();
        if (subtitleSearchToken_ != 0) ++subtitleSearchToken_;
        emit subtitleSearchChanged();
        armIdleTimer();
        return true;
    }
    if (menuStackVisible_) {
        emit menusCloseRequested();
        return true;
    }
    if (seriesExpanded_) {
        setSeriesExpanded(false);
        return true;
    }
    return false;
}

QString PlayerController::stateName() const {
    switch (chromeState_) {
        case ChromeState::FullscreenActive:
            return QStringLiteral("FULLSCREEN_ACTIVE");
        case ChromeState::FullscreenAutoHidden:
            return QStringLiteral("FULLSCREEN_AUTO_HIDDEN");
        case ChromeState::DefaultFit:
            break;
    }
    return QStringLiteral("DEFAULT_FIT");
}

// ── sheets: subtitle search language ─────────────────────────────────────

void PlayerController::setSubtitleSearchLanguage(const QString& code) {
    if (code.size() != 2) return;
    const QString normalized = code.toLower();
    static const QStringList kFixedCodes = {QStringLiteral("uk"), QStringLiteral("en"),
                                            QStringLiteral("ru"), QStringLiteral("pl"),
                                            QStringLiteral("de"), QStringLiteral("es")};
    const QString systemCode = QLocale::system().name().left(2).toLower();
    if (normalized != systemCode && !kFixedCodes.contains(normalized)) return;
    if (subtitleSearchLanguage_ == normalized) return;
    subtitleSearchLanguage_ = normalized;
    emit subtitleSearchChanged();
}

// ── auxiliaries ──────────────────────────────────────────────────────────

void PlayerController::attachAuxiliaryTransient(QWidget* window) const {
    if (window == nullptr || window_ == nullptr) return;
    window->winId();
    if (QWindow* handle = window->windowHandle()) handle->setTransientParent(window_.get());
}

void PlayerController::closeAuxiliaryWindows() {
    // Compare owns renderer and session locks; release them before native teardown.
    QWidget* compare = compareWindow_.data();
    compareWindow_.clear();
    if (compare != nullptr) {
        compare->close();
        delete compare;
    }
    if (clipWindow_ != nullptr) {
        clipWindow_->close();
        clipWindow_->deleteLater();
        clipWindow_.clear();
    }
    if (mediaInformationWindow_ != nullptr) {
        mediaInformationWindow_->close();
        mediaInformationWindow_->deleteLater();
        mediaInformationWindow_.clear();
    }
}

}  // namespace quick
}  // namespace mplayer
