// AnimeJaNai filter plan binding, log observation, engine build polling, and
// hardware decoder restoration for VideoSurface.

#include "VideoSurface.hpp"

#include "MpvHandle.hpp"
#include "native/components/AnimeJanaiBuildLog.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"

#include <QDir>
#include <QFileInfo>
#include <QTimer>

#include <string>
#include <utility>

namespace mplayer {

void VideoSurface::setAnimeJanaiPlan(const native::AnimeJanaiPlan& plan) {
    const uint64_t currentSourceEpoch = mpv_ != nullptr ? mpv_->sourceEpoch() : 0;
    const bool samePlan =
        animeJanaiPlan_.enabled == plan.enabled &&
        animeJanaiPlan_.available == plan.available &&
        animeJanaiPlan_.presetId == plan.presetId &&
        animeJanaiPlan_.reasonKey == plan.reasonKey &&
        animeJanaiPlan_.reason == plan.reason &&
        animeJanaiPlan_.backend == plan.backend &&
        animeJanaiPlan_.slot == plan.slot &&
        animeJanaiPlan_.libraryPath == plan.libraryPath &&
        animeJanaiPlan_.configPath == plan.configPath &&
        animeJanaiPlan_.modelDir == plan.modelDir &&
        animeJanaiPlan_.rifeModelDir == plan.rifeModelDir &&
        animeJanaiPlan_.trtexecPath == plan.trtexecPath &&
        animeJanaiPlan_.trtexecLibDir == plan.trtexecLibDir &&
        animeJanaiPlan_.statsPath == plan.statsPath &&
        animeJanaiPlan_.filterSpec == plan.filterSpec;
    const bool preserveProcessingObservation =
        mpv_ != nullptr && samePlan && animeJanaiPlan_.enabled &&
        animeJanaiPlan_.available && animeJanaiFilterBound_ && !animeJanaiBusy_ &&
        !animeJanaiReportPending_ && animeJanaiObserved_ &&
        animeJanaiObservedEpoch_ == currentSourceEpoch &&
        animeJanaiEpoch_ == currentSourceEpoch &&
        animeJanaiObservedGeneration_ == animeJanaiGeneration_;
    const QString preservedObservedStatus = animeJanaiObservedStatus_;
    const bool sameConfiguration =
        animeJanaiPlan_.backend == plan.backend &&
        animeJanaiPlan_.libraryPath == plan.libraryPath &&
        animeJanaiPlan_.configPath == plan.configPath &&
        animeJanaiPlan_.modelDir == plan.modelDir &&
        animeJanaiPlan_.rifeModelDir == plan.rifeModelDir &&
        animeJanaiPlan_.trtexecPath == plan.trtexecPath &&
        animeJanaiPlan_.trtexecLibDir == plan.trtexecLibDir;
    const bool slotOnlyChange = animeJanaiFilterBound_ && plan.enabled &&
                                sameConfiguration && animeJanaiPlan_.slot != plan.slot;
    const bool configurationChange = animeJanaiFilterBound_ && plan.enabled &&
                                     !sameConfiguration;
    cancelAnimeJanaiOperation();
    animeJanaiPlan_ = plan;
    if (preserveProcessingObservation) {
        animeJanaiObserved_ = true;
        animeJanaiObservedEpoch_ = currentSourceEpoch;
        animeJanaiObservedGeneration_ = animeJanaiGeneration_;
        animeJanaiObservedStatus_ = preservedObservedStatus;
    }
    if (!mpv_) {
        if (!plan.enabled) {
            setAnimeJanaiStatus(QStringLiteral("Off"));
            return;
        }
        if (!plan.available) {
            animeJanaiFail(plan.reasonKey.isEmpty() ? QStringLiteral("animejanai.plan.unavailable")
                                                    : plan.reasonKey,
                           plan.reason.isEmpty() ? QStringLiteral("AnimeJaNai plan is unavailable")
                                                 : plan.reason);
            return;
        }
        setAnimeJanaiStatus(QStringLiteral("Configured; waiting for a source"));
        return;
    }
    animeJanaiEpoch_ = mpv_->sourceEpoch();
    try {
        mpv_->setBackendLogObservationEnabled(animeJanaiBackendLogsNeeded());
    } catch (const MpvError& error) {
        animeJanaiFail(QStringLiteral("animejanai.backend.failed"),
                       QString::fromUtf8(error.what()));
        return;
    }
    if (!plan.enabled) {
        const quint64 generation = animeJanaiGeneration_;
        const uint64_t epoch = animeJanaiEpoch_;
        if (!animeJanaiFilterBound_ && !animeJanaiEngineBuildActive_) {
            restoreAnimeJanaiHwdec(epoch, generation);
            return;
        }
        animeJanaiBusy_ = true;
        animeJanaiRequest_ = mpv_->commandAsync(
            {"vf", "remove", std::string("@") + native::kAnimeJanaiFilterLabel},
            [this, epoch, generation](const AsyncReply& reply) {
                if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                    generation != animeJanaiGeneration_) return;
                animeJanaiRequest_ = {};
                animeJanaiBusy_ = false;
                if (!reply.ok()) {
                    animeJanaiFail(QStringLiteral("animejanai.filter.removeRefused"),
                                   QString::fromStdString(mpv_->describeError(reply.error)));
                    return;
                }
                animeJanaiFilterBound_ = false;
                finishAnimeJanaiEngineBuild(false, tr("TensorRT engine build cancelled."));
                restoreAnimeJanaiHwdec(epoch, generation);
            });
        if (!animeJanaiRequest_.valid()) {
            animeJanaiBusy_ = false;
            animeJanaiFail(QStringLiteral("animejanai.filter.removeNotSubmitted"),
                           QStringLiteral("mpv did not accept the AnimeJaNai filter removal request"));
        }
        return;
    }
    if (!plan.available) {
        animeJanaiFail(plan.reasonKey.isEmpty() ? QStringLiteral("animejanai.plan.unavailable")
                                                : plan.reasonKey,
                       plan.reason.isEmpty() ? QStringLiteral("AnimeJaNai plan is unavailable")
                                             : plan.reason);
        return;
    }
    const QByteArray expectedFilterPrefix =
        QByteArray("@") + native::kAnimeJanaiFilterLabel + QByteArray(":");
    if (plan.filterSpec.isEmpty() || !plan.filterSpec.startsWith(expectedFilterPrefix)) {
        animeJanaiFail(QStringLiteral("animejanai.plan.filterSpecInvalid"),
                       QStringLiteral("available AnimeJaNai plan has no mikai_aji filter specification"));
        return;
    }
    if (plan.slot == 0) {
        animeJanaiFail(QStringLiteral("animejanai.slot.bypass"),
                       QStringLiteral("AnimeJaNai slot 0 is passthrough and cannot be reported as processing"));
        return;
    }
    if (configurationChange) {
        const uint64_t epoch = mpv_->sourceEpoch();
        const quint64 generation = animeJanaiGeneration_;
        animeJanaiBusy_ = true;
        setAnimeJanaiStatus(QStringLiteral("Switching AnimeJaNai configuration"));
        animeJanaiRequest_ = mpv_->commandAsync(
            {"vf", "remove", std::string("@") + native::kAnimeJanaiFilterLabel},
            [this, epoch, generation](const AsyncReply& reply) {
                if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                    generation != animeJanaiGeneration_) return;
                animeJanaiRequest_ = {};
                animeJanaiBusy_ = false;
                if (!reply.ok()) {
                    animeJanaiFail(QStringLiteral("animejanai.backend.removeRefused"),
                                   QString::fromStdString(mpv_->describeError(reply.error)));
                    return;
                }
                animeJanaiFilterBound_ = false;
                finishAnimeJanaiEngineBuild(false, tr("Previous TensorRT engine build stopped."));
                animeJanaiReconfigureAfterRestore_ = true;
                restoreAnimeJanaiHwdec(epoch, generation);
            });
        if (!animeJanaiRequest_.valid()) {
            animeJanaiBusy_ = false;
            animeJanaiFail(QStringLiteral("animejanai.backend.removeNotSubmitted"),
                           QStringLiteral("mpv did not accept removing the old backend filter"));
        }
        return;
    }
    const native::NativePlaybackController::Status nativeStatus = nativePlayback_.status();
    const native::NativePlaybackController::State nativeState = nativeStatus.state;
    // Controller states are not, by themselves, proof that the filter is in the
    // core: Idle is the normal unrequested state, and Attached can remain after
    // a refused add. The surface's bound bit includes an add/remove in flight
    // and is cleared only by a refused add or an observed removal.
    const bool stateCanBeUnbound =
        nativeState == native::NativePlaybackController::State::Idle ||
        nativeState == native::NativePlaybackController::State::Attached ||
        nativeState == native::NativePlaybackController::State::Off ||
        nativeState == native::NativePlaybackController::State::Failed;
    const bool noPrivateFilterOrTransition = !nativeFilterBound_ && stateCanBeUnbound;
    if (nativeFilterBound_ || !noPrivateFilterOrTransition) {
        const QString details =
            QStringLiteral("private mikai_native state=%1 bound=%2 reason=%3: %4; "
                           "AnimeJaNai cannot run concurrently")
                .arg(QString::fromLatin1(VideoSurface::nativePlaybackStateName(nativeState)))
                .arg(nativeFilterBound_ ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(nativeStatus.reasonKey.isEmpty() ? QStringLiteral("none") : nativeStatus.reasonKey,
                     nativeStatus.reason.isEmpty() ? QStringLiteral("no additional detail")
                                                   : nativeStatus.reason);
        animeJanaiFail(QStringLiteral("animejanai.privateNativeConflict"), details);
        return;
    }
    const native::RuntimeSelection& selection = native::currentSelection();
    if (!native::isAnimeJanaiForkRuntime(selection)) {
        animeJanaiFail(QStringLiteral("animejanai.core.unsupported"),
                       QStringLiteral("AnimeJaNai requires the official Standard fork d6d93599d5; "
                                      "the loaded libmpv core is not that supported fork"));
        return;
    }
    if (slotOnlyChange) {
        const uint64_t epoch = mpv_->sourceEpoch();
        setAnimeJanaiStatus(QStringLiteral("Switching AnimeJaNai slot"));
        const quint64 generation = animeJanaiGeneration_;
        animeJanaiBusy_ = true;
        animeJanaiReportPending_ = true;
        animeJanaiRequest_ = mpv_->commandAsync(
            {"vf-command", native::kAnimeJanaiFilterLabel, "slot", std::to_string(plan.slot)},
            [this, epoch, generation](const AsyncReply& reply) {
                if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                    generation != animeJanaiGeneration_) return;
                animeJanaiRequest_ = {};
                animeJanaiBusy_ = false;
                if (!reply.ok()) {
                    animeJanaiFail(QStringLiteral("animejanai.slot.refused"),
                                   QString::fromStdString(mpv_->describeError(reply.error)));
                    return;
                }
                animeJanaiReportPending_ = false;
                setAnimeJanaiStatus(QStringLiteral(
                    "Configured; filter accepted (inference not observed)"));
                publishPendingAnimeJanaiObservation(epoch, generation);
            });
        if (!animeJanaiRequest_.valid()) {
            animeJanaiReportPending_ = false;
            animeJanaiBusy_ = false;
            animeJanaiFail(QStringLiteral("animejanai.slot.notSubmitted"),
                           QStringLiteral("mpv did not accept the AnimeJaNai slot request"));
        }
        return;
    }
    processAnimeJanaiPlan();
}

bool VideoSurface::animeJanaiProcessingObserved() const {
    return animeJanaiObserved_ && animeJanaiFilterBound_ && animeJanaiEpoch_ != 0 &&
           mpv_ != nullptr && animeJanaiObservedEpoch_ == mpv_->sourceEpoch() &&
           animeJanaiObservedGeneration_ == animeJanaiGeneration_;
}

void VideoSurface::resetAnimeJanaiObservation() {
    animeJanaiObserved_ = false;
    animeJanaiObservedEpoch_ = 0;
    animeJanaiObservedGeneration_ = 0;
    animeJanaiObservedStatus_.clear();
    animeJanaiPendingObservedStatus_.clear();
    animeJanaiPendingObservedEpoch_ = 0;
    animeJanaiPendingObservedGeneration_ = 0;
}

// libmpv has one client-level log threshold, so verbose delivery is scoped to
// the active official plan that consumes these backend reports.
bool VideoSurface::animeJanaiBackendLogsNeeded() const {
    return mpv_ != nullptr && animeJanaiSourceRequested_ && !pendingOpen_ &&
           animeJanaiPlan_.enabled && animeJanaiPlan_.available &&
           native::isAnimeJanaiForkRuntime(native::currentSelection());
}

void VideoSurface::observeAnimeJanaiBackendLog(const QString& prefix, const QString& line) {
    if (!animeJanaiBackendLogsNeeded() ||
        (!animeJanaiFilterBound_ && !animeJanaiReportPending_) ||
        (animeJanaiBusy_ && !animeJanaiReportPending_) || animeJanaiEpoch_ == 0 ||
        animeJanaiEpoch_ != mpv_->sourceEpoch()) return;

    const QString lower = (prefix + QLatin1Char(' ') + line).toLower();
    const native::AnimeJanaiBuildLogEvent buildLogEvent =
        native::classifyAnimeJanaiBuildLog(prefix, line, animeJanaiPlan_.backend,
                                           animeJanaiPlan_.slot);
    if (buildLogEvent == native::AnimeJanaiBuildLogEvent::TensorRtBuildStarted) {
        if (!animeJanaiEngineBuildActive_) {
            animeJanaiBuildPollingRefused_ = false;
            animeJanaiBuildNextPollMs_ = 0;
            beginEnginePause();
        }
        animeJanaiEngineBuildActive_ = true;
        const QString detail = tr("Compiling the selected model for this video resolution.");
        setAnimeJanaiStatus(tr("Building TensorRT engine…"));
        emit animeJanaiEngineBuildChanged(true, false, detail);
    }
    const QString backendShort =
        animeJanaiPlan_.backend == native::Backend::TensorRT
            ? QStringLiteral("TRT")
            : QStringLiteral("DML");
    QString presetLabel = animeJanaiPlan_.presetId;
    for (const native::AnimeJanaiPreset& preset : native::animeJanaiPresets()) {
        if (preset.id.compare(animeJanaiPlan_.presetId, Qt::CaseInsensitive) == 0) {
            presetLabel = preset.label;
            break;
        }
    }
    QString observed;
    if (animeJanaiPlan_.presetId.startsWith(QStringLiteral("rife:"), Qt::CaseInsensitive) &&
        lower.contains(QStringLiteral("rife active:")) &&
        lower.contains(QStringLiteral("interpolation"))) {
        double factor = 2.0;
        static const QRegularExpression kRifeFactorRe(
            QStringLiteral("rife\\s+active:\\s*(\\d+)\\s*/\\s*(\\d+)"),
            QRegularExpression::CaseInsensitiveOption);
        if (const QRegularExpressionMatch m = kRifeFactorRe.match(line); m.hasMatch()) {
            const double num = m.captured(1).toDouble();
            const double den = m.captured(2).toDouble();
            if (num > 0.0 && den > 0.0) factor = num / den;
        }
        const double baseFps =
            observedContainerFps_ > 0.0
                ? observedContainerFps_
                : (streamFramesPerSecond_ > 0.0 ? streamFramesPerSecond_ / factor : 23.98);
        const double targetFps = baseFps * factor;
        const int srcH = videoHeight_ > 0
                             ? videoHeight_
                             : static_cast<int>(std::lround(observedVideoHeight_));
        if (srcH > 0) {
            observed = QStringLiteral("%1 · %2 · %3p · %4 → %5 FPS")
                           .arg(backendShort, presetLabel)
                           .arg(srcH)
                           .arg(baseFps, 0, 'f', 2)
                           .arg(targetFps, 0, 'f', 2);
        } else {
            observed = QStringLiteral("%1 · %2 · %3 → %4 FPS")
                           .arg(backendShort, presetLabel)
                           .arg(baseFps, 0, 'f', 2)
                           .arg(targetFps, 0, 'f', 2);
        }
    } else {
        if (buildLogEvent == native::AnimeJanaiBuildLogEvent::SlotConfigured) {
            int inW = videoWidth_ > 0 ? videoWidth_ : static_cast<int>(std::lround(observedVideoWidth_));
            int inH = videoHeight_ > 0 ? videoHeight_ : static_cast<int>(std::lround(observedVideoHeight_));
            int outW = inW * 2;
            int outH = inH * 2;
            static const QRegularExpression kSlotDimRe(
                QStringLiteral("(\\d+)x(\\d+)\\s*->\\s*(\\d+)x(\\d+)"),
                QRegularExpression::CaseInsensitiveOption);
            if (const QRegularExpressionMatch m = kSlotDimRe.match(line); m.hasMatch()) {
                inW = m.captured(1).toInt();
                inH = m.captured(2).toInt();
                outW = m.captured(3).toInt();
                outH = m.captured(4).toInt();
            }
            if (inH > 0 && outH > 0) {
                observed = QStringLiteral("%1 · %2 · %3p → %4p (%5×%6)")
                               .arg(backendShort, presetLabel)
                               .arg(inH)
                               .arg(outH)
                               .arg(outW)
                               .arg(outH);
            } else {
                observed = QStringLiteral("%1 · %2").arg(backendShort, presetLabel);
            }
        }
    }
    if (observed.isEmpty()) return;

    if (animeJanaiFilterBound_ && !animeJanaiReportPending_) {
        publishAnimeJanaiObservation(observed, animeJanaiEpoch_, animeJanaiGeneration_);
    } else {
        animeJanaiPendingObservedStatus_ = std::move(observed);
        animeJanaiPendingObservedEpoch_ = animeJanaiEpoch_;
        animeJanaiPendingObservedGeneration_ = animeJanaiGeneration_;
    }
}

void VideoSurface::publishAnimeJanaiObservation(const QString& status, uint64_t epoch,
                                                quint64 generation) {
    if (!animeJanaiBackendLogsNeeded() || !animeJanaiFilterBound_ ||
        epoch == 0 || epoch != mpv_->sourceEpoch() || epoch != animeJanaiEpoch_ ||
        generation != animeJanaiGeneration_) return;
    animeJanaiObserved_ = true;
    animeJanaiObservedEpoch_ = epoch;
    animeJanaiObservedGeneration_ = generation;
    animeJanaiObservedStatus_ = status;
    setAnimeJanaiStatus(status);
    finishAnimeJanaiEngineBuild(true, tr("TensorRT engine ready; processing started."));
}

void VideoSurface::publishPendingAnimeJanaiObservation(uint64_t epoch, quint64 generation) {
    const QString reported = std::move(animeJanaiPendingObservedStatus_);
    const uint64_t reportEpoch = animeJanaiPendingObservedEpoch_;
    const quint64 reportGeneration = animeJanaiPendingObservedGeneration_;
    animeJanaiPendingObservedEpoch_ = 0;
    animeJanaiPendingObservedGeneration_ = 0;
    if (!reported.isEmpty() && reportEpoch == epoch && reportGeneration == generation) {
        publishAnimeJanaiObservation(reported, reportEpoch, reportGeneration);
    }
}

void VideoSurface::setAnimeJanaiStatus(const QString& status) {
    if (animeJanaiObserved_ && animeJanaiObservedEpoch_ == (mpv_ ? mpv_->sourceEpoch() : 0) &&
        animeJanaiObservedGeneration_ == animeJanaiGeneration_ &&
        status != animeJanaiObservedStatus_) return;
    if (animeJanaiStatus_ == status) return;
    animeJanaiStatus_ = status;
    emit animeJanaiStatusChanged(status);
}

bool VideoSurface::enginePauseHeld() const {
    return enginePauseEpoch_ != 0 && mpv_ != nullptr &&
           enginePauseEpoch_ == mpv_->sourceEpoch() &&
           (enginePreparationPaused_ || animeJanaiEngineBuildActive_);
}

bool VideoSurface::playbackPauseIntent() const {
    return enginePauseHeld() ? enginePauseIntent_ : !isPlaying();
}

void VideoSurface::beginEnginePause() {
    if (enginePauseHeld()) return;
    enginePauseIntent_ = !isPlaying();
    enginePauseRequestGeneration_ = pauseRequestGeneration_;
    if (mpv_ == nullptr || mpv_->sourceEpoch() == 0) return;
    enginePauseEpoch_ = mpv_->sourceEpoch();
    reassertPaused();
    setPlaying(false);
}

void VideoSurface::endEnginePause(bool restoreIntent) {
    if (enginePreparationPaused_ || animeJanaiEngineBuildActive_) return;
    const bool sameSource = mpv_ != nullptr && enginePauseEpoch_ != 0 &&
                            enginePauseEpoch_ == mpv_->sourceEpoch();
    const bool paused = enginePauseIntent_;
    enginePauseEpoch_ = 0;
    if (!restoreIntent || !sameSource) return;
    setPlaying(!paused);
    requestPausedWrite(paused);
}

void VideoSurface::discardEnginePause() {
    enginePauseEpoch_ = 0;
    enginePreparationPaused_ = false;
}

void VideoSurface::setEnginePreparationPaused(bool active, bool restoreIntent) {
    if (active == enginePreparationPaused_) return;
    if (active) beginEnginePause();
    enginePreparationPaused_ = active;
    if (!active) endEnginePause(restoreIntent);
}

void VideoSurface::pollAnimeJanaiEngineBuild() {
    if (!animeJanaiEngineBuildActive_ || !animeJanaiFilterBound_ || animeJanaiBusy_ ||
        animeJanaiBuildPollingRefused_ || mpv_ == nullptr ||
        animeJanaiBuildPollRequest_.valid() ||
        nativeStatusClock_.elapsed() < animeJanaiBuildNextPollMs_) return;
    animeJanaiBuildNextPollMs_ = nativeStatusClock_.elapsed() + 500;
    const uint64_t epoch = mpv_->sourceEpoch();
    const quint64 generation = animeJanaiGeneration_;
    // The official filter's poll command wakes completion without advancing playback.
    // The payload is required by the transport and ignored by the filter.
    animeJanaiBuildPollRequest_ = mpv_->commandAsync(
        {"vf-command", native::kAnimeJanaiFilterLabel, "poll", ""},
        [this, epoch, generation](const AsyncReply& reply) {
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                generation != animeJanaiGeneration_) return;
            animeJanaiBuildPollRequest_ = {};
            if (!reply.ok() && animeJanaiEngineBuildActive_) {
                animeJanaiBuildPollingRefused_ = true;
                emit animeJanaiFailure(QStringLiteral("animejanai.build.pollRefused: ") +
                                      QString::fromStdString(mpv_->describeError(reply.error)));
            }
        });
    if (!animeJanaiBuildPollRequest_.valid()) {
        animeJanaiBuildPollingRefused_ = true;
        emit animeJanaiFailure(QStringLiteral("animejanai.build.pollNotSubmitted"));
    }
}

void VideoSurface::finishAnimeJanaiEngineBuild(bool succeeded, const QString& detail) {
    if (!animeJanaiEngineBuildActive_) return;
    animeJanaiEngineBuildActive_ = false;
    endEnginePause(true);
    emit animeJanaiEngineBuildChanged(false, succeeded, detail);
}

void VideoSurface::animeJanaiFail(const QString& reasonKey, const QString& reason) {
    animeJanaiReportPending_ = false;
    resetAnimeJanaiObservation();
    if (mpv_ != nullptr) {
        try {
            mpv_->setBackendLogObservationEnabled(false);
        } catch (const MpvError& error) {
            emit errorOccurred(QString::fromUtf8(error.what()));
        }
    }
    const QString detail = reasonKey + QStringLiteral(": ") + reason;
    finishAnimeJanaiEngineBuild(false, detail);
    setAnimeJanaiStatus(QStringLiteral("Processing failed — ") + detail);
    emit animeJanaiFailure(detail);
}

void VideoSurface::cancelAnimeJanaiOperation() {
    if (mpv_ != nullptr && animeJanaiBuildPollRequest_.valid()) {
        mpv_->cancelAsync(animeJanaiBuildPollRequest_);
    }
    animeJanaiBuildPollRequest_ = {};
    ++animeJanaiGeneration_;
    animeJanaiReportPending_ = false;
    resetAnimeJanaiObservation();
    if (mpv_ != nullptr && animeJanaiRequest_.valid()) {
        mpv_->cancelAsync(animeJanaiRequest_);
    }
    animeJanaiRequest_ = {};
    animeJanaiBusy_ = false;
    animeJanaiReconfigureAfterRestore_ = false;
}

void VideoSurface::processAnimeJanaiPlan() {
    if (!mpv_ || !animeJanaiPlan_.enabled || !animeJanaiPlan_.available) return;
    try {
        mpv_->setBackendLogObservationEnabled(animeJanaiBackendLogsNeeded());
    } catch (const MpvError& error) {
        animeJanaiFail(QStringLiteral("animejanai.backend.failed"),
                       QString::fromUtf8(error.what()));
        return;
    }
    if (animeJanaiBusy_) return;
    if (!animeJanaiSourceRequested_ || url_.isEmpty() || pendingOpen_) {
        setAnimeJanaiStatus(QStringLiteral("Configured; waiting for a loaded source"));
        return;
    }
    if (animeJanaiFilterBound_) {
        setAnimeJanaiStatus(QStringLiteral("Configured; filter retained (inference not observed)"));
        return;
    }
    const uint64_t epoch = mpv_->sourceEpoch();
    const quint64 generation = animeJanaiGeneration_;
    animeJanaiBusy_ = true;
    setAnimeJanaiStatus(QStringLiteral("Configuring"));
    // Read and retain the user's decoder choice before temporarily aligning it.
    animeJanaiRequest_ = mpv_->getPropertyAsync(
        "hwdec", MPV_FORMAT_STRING,
        [this, epoch, generation](const AsyncReply& prior) {
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                generation != animeJanaiGeneration_) return;
            animeJanaiRequest_ = {};
            if (!prior.ok() || prior.format != MPV_FORMAT_STRING) {
                animeJanaiBusy_ = false;
                animeJanaiFail(QStringLiteral("animejanai.hwdec.previousUnavailable"),
                               QStringLiteral("could not read the current hwdec setting"));
                return;
            }
            if (animeJanaiPreviousHwdec_.isEmpty()) {
                animeJanaiPreviousHwdec_ = QString::fromStdString(prior.text);
            }
            const QString required = animeJanaiPlan_.backend == native::Backend::TensorRT
                                         ? QStringLiteral("nvdec")
                                         : QStringLiteral("d3d11va");
            const auto write = mpv_->commandAsync(
                {"set", "hwdec", required.toStdString()},
                [this, epoch, generation, required](const AsyncReply& written) {
                    if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                        generation != animeJanaiGeneration_) return;
                    animeJanaiRequest_ = {};
                    if (!written.ok()) {
                        animeJanaiBusy_ = false;
                        animeJanaiFail(QStringLiteral("animejanai.hwdec.writeRefused"),
                                       QString::fromStdString(mpv_->describeError(written.error)));
                        return;
                    }
                    animeJanaiRequest_ = mpv_->getPropertyAsync(
                        "hwdec", MPV_FORMAT_STRING,
                        [this, epoch, generation, required](const AsyncReply& observed) {
                            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                                generation != animeJanaiGeneration_) return;
                            animeJanaiRequest_ = {};
                            if (!observed.ok() || observed.format != MPV_FORMAT_STRING) {
                                animeJanaiBusy_ = false;
                                animeJanaiFail(QStringLiteral("animejanai.hwdec.readbackFailed"),
                                               QStringLiteral("could not read the decoder setting back"));
                                restoreAnimeJanaiHwdec(epoch, generation);
                                return;
                            }
                            const QString decoder = QString::fromStdString(observed.text);
                            if (decoder.compare(required, Qt::CaseInsensitive) != 0) {
                                animeJanaiBusy_ = false;
                                animeJanaiFail(QStringLiteral("animejanai.hwdec.incompatible"),
                                               QStringLiteral("the core reports hwdec='%1'; required '%2'")
                                                   .arg(decoder, required));
                                restoreAnimeJanaiHwdec(epoch, generation);
                                return;
                            }
                            const std::string spec(animeJanaiPlan_.filterSpec.constData(),
                                                   static_cast<size_t>(animeJanaiPlan_.filterSpec.size()));
                            animeJanaiReportPending_ = true;
                            animeJanaiRequest_ = mpv_->commandAsync(
                                {"vf", "add", spec},
                                [this, epoch, generation](const AsyncReply& attached) {
                                    if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                                        generation != animeJanaiGeneration_) return;
                                    animeJanaiRequest_ = {};
                                    animeJanaiBusy_ = false;
                                    if (!attached.ok()) {
                                        animeJanaiFail(QStringLiteral("animejanai.filter.attachRefused"),
                                                       QString::fromStdString(
                                                           mpv_->describeError(attached.error)));
                                        restoreAnimeJanaiHwdec(epoch, generation);
                                        return;
                                    }
                                    animeJanaiFilterBound_ = true;
                                    animeJanaiReportPending_ = false;
                                    setAnimeJanaiStatus(QStringLiteral(
                                        "Configured; filter accepted (inference not observed)"));
                                    publishPendingAnimeJanaiObservation(epoch, generation);
                                    // The chain changed under a running core, and
                                    // a paused one produces no frame to rebuild
                                    // it on, so the render state is refreshed.
                                    refreshRenderState();
                                });
                            if (!animeJanaiRequest_.valid()) {
                                animeJanaiReportPending_ = false;
                                animeJanaiBusy_ = false;
                                animeJanaiFail(QStringLiteral("animejanai.filter.attachNotSubmitted"),
                                               QStringLiteral("mpv did not accept the AnimeJaNai "
                                                              "filter request"));
                                restoreAnimeJanaiHwdec(epoch, generation);
                            }
                        });
                    if (!animeJanaiRequest_.valid()) {
                        animeJanaiBusy_ = false;
                        animeJanaiFail(QStringLiteral("animejanai.hwdec.readbackNotSubmitted"),
                                       QStringLiteral("mpv did not accept the hwdec readback request"));
                        restoreAnimeJanaiHwdec(epoch, generation);
                    }
                });
            if (!write.valid()) {
                animeJanaiBusy_ = false;
                animeJanaiFail(QStringLiteral("animejanai.hwdec.writeNotSubmitted"),
                               QStringLiteral("mpv did not accept the hwdec write request"));
            }
        });
    if (!animeJanaiRequest_.valid()) {
        animeJanaiBusy_ = false;
        animeJanaiFail(QStringLiteral("animejanai.hwdec.readNotSubmitted"),
                       QStringLiteral("mpv did not accept the previous hwdec read request"));
    }
}

void VideoSurface::restoreAnimeJanaiHwdec(uint64_t epoch, quint64 generation) {
    if (!mpv_ || animeJanaiPreviousHwdec_.isNull() || animeJanaiPreviousHwdec_.isEmpty()) {
        if (animeJanaiReconfigureAfterRestore_) {
            animeJanaiReconfigureAfterRestore_ = false;
            processAnimeJanaiPlan();
        } else if (!animeJanaiPlan_.enabled) {
            setAnimeJanaiStatus(QStringLiteral("Off"));
        }
        return;
    }
    const QString previous = animeJanaiPreviousHwdec_;
    animeJanaiPreviousHwdec_.clear();
    animeJanaiRequest_ = mpv_->commandAsync(
        {"set", "hwdec", previous.toStdString()},
        [this, epoch, generation](const AsyncReply& reply) {
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                generation != animeJanaiGeneration_) return;
            animeJanaiRequest_ = {};
            if (!reply.ok()) {
                animeJanaiReconfigureAfterRestore_ = false;
                animeJanaiFail(QStringLiteral("animejanai.hwdec.restoreRefused"),
                               QString::fromStdString(mpv_->describeError(reply.error)));
                return;
            }
            if (animeJanaiReconfigureAfterRestore_) {
                animeJanaiReconfigureAfterRestore_ = false;
                processAnimeJanaiPlan();
            } else if (!animeJanaiPlan_.enabled) {
                setAnimeJanaiStatus(QStringLiteral("Off"));
            }
        });
    if (!animeJanaiRequest_.valid()) {
        animeJanaiReconfigureAfterRestore_ = false;
        animeJanaiFail(QStringLiteral("animejanai.hwdec.restoreNotSubmitted"),
                       QStringLiteral("mpv did not accept restoring the previous hwdec setting"));
    }
}


}  // namespace mplayer
