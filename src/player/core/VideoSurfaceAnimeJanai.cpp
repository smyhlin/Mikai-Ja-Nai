// AnimeJaNai filter plan binding, log observation, engine build polling, and
// hardware decoder restoration for VideoSession.

#include "VideoSession.hpp"

#include "MpvHandle.hpp"
#include "native/components/AnimeJanaiBuildLog.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>

#include <cmath>
#include <string>
#include <utility>

namespace mplayer {

namespace {

/// How long a restored playhead may take to be confirmed before the frame hold
/// is released anyway, the spacing between observation reads while it is owed,
/// and a hard bound on how many of them. Both are bounds on the wait and never
/// a second seek; the spacing is what gives a decoder a real chance to land the
/// sought frame instead of burning the reads before the seek is decoded.
constexpr qint64 kReconfigureSettleTimeoutMs = 3000;
constexpr int kReconfigureSettleConfirmIntervalMs = 60;
constexpr int kReconfigureSettleMaxReads = 48;

QStringList animeJanaiFilterLabels(const native::AnimeJanaiPlan& plan) {
    QStringList labels;
    static const QRegularExpression labelPattern(QStringLiteral("@([A-Za-z0-9_]+):"));
    for (const native::AnimeJanaiPlanStage& stage : plan.stages) {
        // T-315: the labels are collected per stage, because every stage is its
        // own vf entry and the fallback must name exactly the stages whose spec
        // carried no @label. The old form stopped at the first stage and
        // silently dropped the fallback names of every later one.
        QStringList stageLabels;
        QRegularExpressionMatchIterator matches = labelPattern.globalMatch(
            QString::fromUtf8(stage.filterSpec));
        while (matches.hasNext()) {
            stageLabels.append(matches.next().captured(1));
        }
        if (stageLabels.isEmpty() && !stage.filterLabel.isEmpty())
            stageLabels.append(stage.filterLabel);
        for (const QString& label : stageLabels) {
            if (!labels.contains(label)) labels.append(label);
        }
    }
    if (labels.isEmpty() && plan.requiresAji)
        labels.append(QString::fromLatin1(native::kAnimeJanaiFilterLabel));
    return labels;
}

}  // namespace

void VideoSession::removeAnimeJanaiFilters(uint64_t epoch, quint64 generation,
                                           std::function<void()> completion) {
    if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
        generation != animeJanaiGeneration_) {
        // T-315: the removal is moot, but the caller's continuation is not - it
        // releases the frame hold, the busy flag and the build lease, and the
        // old early-out left all three to the open() self-heal, which never
        // runs for a source that keeps playing.
        if (completion) completion();
        return;
    }
    if (animeJanaiBoundFilterLabels_.isEmpty()) {
        animeJanaiFilterBound_ = false;
        if (completion) completion();
        return;
    }
    const std::string label = "@" + animeJanaiBoundFilterLabels_.first().toStdString();
    animeJanaiRequest_ = mpv_->commandAsync(
        {"vf", "remove", label},
        [this, epoch, generation, completion = std::move(completion)](
            const AsyncReply& reply) mutable {
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                generation != animeJanaiGeneration_) {
                // T-315: same rule as the entry guard - the superseded removal
                // still owes its caller the cleanup continuation.
                if (completion) completion();
                return;
            }
            animeJanaiRequest_ = {};
            if (!reply.ok()) {
                animeJanaiBusy_ = false;
                animeJanaiFail(QStringLiteral("animejanai.filter.removeRefused"),
                               QString::fromStdString(mpv_->describeError(reply.error)));
                return;
            }
            animeJanaiBoundFilterLabels_.removeFirst();
            removeAnimeJanaiFilters(epoch, generation, std::move(completion));
        });
    if (!animeJanaiRequest_.valid()) {
        animeJanaiBusy_ = false;
        animeJanaiFail(QStringLiteral("animejanai.filter.removeNotSubmitted"),
                       QStringLiteral("mpv did not accept removing an owned filter"));
    }
}

void VideoSession::setAnimeJanaiPlan(const native::AnimeJanaiPlan& plan) {
    const uint64_t currentSourceEpoch = mpv_ != nullptr ? mpv_->sourceEpoch() : 0;
    bool samePlan =
        animeJanaiPlan_.enabled == plan.enabled &&
        animeJanaiPlan_.available == plan.available &&
        animeJanaiPlan_.presetId == plan.presetId &&
        animeJanaiPlan_.reasonKey == plan.reasonKey &&
        animeJanaiPlan_.reason == plan.reason &&
        animeJanaiPlan_.note == plan.note &&
        animeJanaiPlan_.backend == plan.backend &&
        animeJanaiPlan_.slot == plan.slot &&
        animeJanaiPlan_.libraryPath == plan.libraryPath &&
        animeJanaiPlan_.configPath == plan.configPath &&
        animeJanaiPlan_.modelDir == plan.modelDir &&
        animeJanaiPlan_.rifeModelDir == plan.rifeModelDir &&
        animeJanaiPlan_.trtexecPath == plan.trtexecPath &&
        animeJanaiPlan_.trtexecLibDir == plan.trtexecLibDir &&
        animeJanaiPlan_.statsPath == plan.statsPath &&
        animeJanaiPlan_.backendLibDirs == plan.backendLibDirs &&
        animeJanaiPlan_.filterSpec == plan.filterSpec &&
        animeJanaiPlan_.selectedChainNumber == plan.selectedChainNumber &&
        animeJanaiPlan_.requiresAji == plan.requiresAji &&
        animeJanaiPlan_.stages.size() == plan.stages.size();
    if (samePlan) {
        for (int i = 0; i < plan.stages.size(); ++i) {
            const native::AnimeJanaiPlanStage& oldStage = animeJanaiPlan_.stages.at(i);
            const native::AnimeJanaiPlanStage& newStage = plan.stages.at(i);
            if (oldStage.kind != newStage.kind || oldStage.filterLabel != newStage.filterLabel ||
                oldStage.filterSpec != newStage.filterSpec ||
                oldStage.configPath != newStage.configPath ||
                oldStage.statsPath != newStage.statsPath ||
                oldStage.firstStepNumber != newStage.firstStepNumber ||
                oldStage.lastStepNumber != newStage.lastStepNumber ||
                oldStage.presetId != newStage.presetId ||
                oldStage.scaleFactor != newStage.scaleFactor) {
                samePlan = false;
                break;
            }
        }
    }
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
        animeJanaiPlan_.trtexecLibDir == plan.trtexecLibDir &&
        animeJanaiPlan_.backendLibDirs == plan.backendLibDirs;
    const bool slotOnlyChange = animeJanaiFilterBound_ && plan.enabled && plan.requiresAji &&
                                plan.stages.size() <= 1 && sameConfiguration &&
                                animeJanaiPlan_.stages.size() <= 1 &&
                                animeJanaiPlan_.slot != plan.slot;
    const bool configurationChange = animeJanaiFilterBound_ && plan.enabled &&
                                     (!sameConfiguration ||
                                      animeJanaiPlan_.filterSpec != plan.filterSpec);
    const bool planWillReconfigureOutput =
        (plan.enabled && plan.available &&
         (!animeJanaiFilterBound_ || slotOnlyChange || configurationChange)) ||
        (!plan.enabled && (animeJanaiFilterBound_ || animeJanaiEngineBuildActive_));
    const bool processingConfigurationChanged =
        planWillReconfigureOutput && mpv_ != nullptr && currentSourceEpoch != 0 &&
        animeJanaiSourceRequested_ && !pendingOpen_ && !url_.isEmpty();
    if (processingConfigurationChanged) {
        // A new user request re-bases the position: an abandoned attempt's
        // capture would otherwise seek the viewer back to where it started.
        discardAnimeJanaiReconfigurePosition();
        setEngineFrameHold(true);
    }
    const native::Backend previousBackend = animeJanaiPlan_.backend;
    cancelAnimeJanaiOperation();
    // Every plan request is a new operation: a viewer seek that cancelled the
    // previous restore must not suppress this one's capture.
    animeJanaiReconfigureRestoreSuppressed_ = false;
    animeJanaiPlan_ = plan;
    if (!samePlan) {
        animeJanaiAjiStageCount_ = 0;
        for (const native::AnimeJanaiPlanStage& stage : plan.stages) {
            if (stage.kind == native::AnimeJanaiPlanStage::Kind::Aji)
                ++animeJanaiAjiStageCount_;
        }
        if (plan.requiresAji && animeJanaiAjiStageCount_ == 0)
            animeJanaiAjiStageCount_ = 1;
        animeJanaiConfiguredAjiStageCount_ = 0;
        animeJanaiAggregateBuildGate_ = {};
        animeJanaiPendingAjiBuildCount_ = 0;
        animeJanaiBuildPollStageIndex_ = 0;
    }
    if (preserveProcessingObservation) {
        animeJanaiObserved_ = true;
        animeJanaiObservedEpoch_ = currentSourceEpoch;
        animeJanaiObservedGeneration_ = animeJanaiGeneration_;
        animeJanaiObservedStatus_ = preservedObservedStatus;
    }
    if (!mpv_) {
        applyAnimeJanaiPlanChange(previousBackend, configurationChange, slotOnlyChange);
        return;
    }
    if (processingConfigurationChanged) {
        // Every hwdec/filter write below re-inits the decoder and lets mpv land
        // the playhead on a keyframe, so the position to restore is read from
        // the core now, before the first of them is issued. The booleans were
        // computed against the pre-replacement plan, so they travel with the
        // continuation rather than being recomputed from the new state.
        beginAnimeJanaiReconfigureHold(
            [this, previousBackend, configurationChange, slotOnlyChange] {
                applyAnimeJanaiPlanChange(previousBackend, configurationChange, slotOnlyChange);
            });
        return;
    }
    applyAnimeJanaiPlanChange(previousBackend, configurationChange, slotOnlyChange);
}

void VideoSession::applyAnimeJanaiPlanChange(native::Backend previousBackend,
                                             bool configurationChange,
                                             bool slotOnlyChange) {
    if (!mpv_) {
        if (!animeJanaiPlan_.enabled) {
            setAnimeJanaiStatus(QStringLiteral("Off"));
            return;
        }
        if (!animeJanaiPlan_.available) {
            animeJanaiFail(animeJanaiPlan_.reasonKey.isEmpty()
                               ? QStringLiteral("animejanai.plan.unavailable")
                               : animeJanaiPlan_.reasonKey,
                           animeJanaiPlan_.reason.isEmpty()
                               ? QStringLiteral("AnimeJaNai plan is unavailable")
                               : animeJanaiPlan_.reason);
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
    if (!animeJanaiPlan_.enabled) {
        const quint64 generation = animeJanaiGeneration_;
        const uint64_t epoch = animeJanaiEpoch_;
        if (!animeJanaiFilterBound_ && !animeJanaiEngineBuildActive_) {
            if (animeJanaiStageBuildLease_) animeJanaiStageBuildLease_->release();
            animeJanaiStageBuildLease_.reset();
            // The restore below writes hwdec and moves the playhead again, so
            // the frame hold and the captured position belong to its terminal
            // settle, not to this earlier one.
            restoreAnimeJanaiHwdec(epoch, generation);
            return;
        }
        animeJanaiBusy_ = true;
        removeAnimeJanaiFilters(epoch, generation, [this, epoch, generation] {
            animeJanaiBusy_ = false;
            if (animeJanaiStageBuildLease_) animeJanaiStageBuildLease_->release();
            animeJanaiStageBuildLease_.reset();
            finishAnimeJanaiEngineBuild(
                false,
                tr("%1 engine build cancelled.")
                    .arg(animeJanaiPlan_.backend == native::Backend::TensorRTRTX
                             ? VideoSession::tr("TensorRT for RTX")
                             : VideoSession::tr("TensorRT")));
            restoreAnimeJanaiHwdec(epoch, generation);
        });
        return;
    }
    if (!animeJanaiPlan_.available) {
        animeJanaiFail(animeJanaiPlan_.reasonKey.isEmpty()
                           ? QStringLiteral("animejanai.plan.unavailable")
                           : animeJanaiPlan_.reasonKey,
                       animeJanaiPlan_.reason.isEmpty()
                           ? QStringLiteral("AnimeJaNai plan is unavailable")
                           : animeJanaiPlan_.reason);
        return;
    }
    if (animeJanaiPlan_.filterSpec.isEmpty() || animeJanaiPlan_.stages.isEmpty()) {
        animeJanaiFail(QStringLiteral("animejanai.plan.filterSpecInvalid"),
                       QStringLiteral("available AnimeJaNai plan has no executable filter stages"));
        return;
    }
    const bool sourceAllowed =
        !sourceColorProfile_.requiresSdrGate() ||
        (sourceColorProfile_.isHdr() && animeJanaiHdrOverride());
    if (hdrRenderingFixesEnabled() && animeJanaiPlan_.enabled && !sourceAllowed) {
        if (animeJanaiFilterBound_ || animeJanaiEngineBuildActive_ || animeJanaiBusy_) {
            enforceAnimeJanaiHdrGate();
        } else {
            animeJanaiSourceGated_ = true;
            setAnimeJanaiStatus(sourceColorProfile_.isHdr()
                                    ? tr("Skipped on HDR: native stages force 8-bit NV12")
                                    : tr("Skipped: source transfer metadata is unknown"));
        }
        return;
    }
    if (sourceAllowed) animeJanaiSourceGated_ = false;
    if (animeJanaiPlan_.requiresAji &&
        std::none_of(animeJanaiPlan_.stages.cbegin(), animeJanaiPlan_.stages.cend(),
                     [](const native::AnimeJanaiPlanStage& stage) {
                         return stage.kind == native::AnimeJanaiPlanStage::Kind::Aji;
                     })) {
        animeJanaiFail(QStringLiteral("animejanai.plan.filterSpecInvalid"),
                       QStringLiteral("AJI plan has no AnimeJaNai stage"));
        return;
    }
    if (animeJanaiPlan_.requiresAji && animeJanaiPlan_.slot == 0) {
        animeJanaiFail(QStringLiteral("animejanai.slot.bypass"),
                       QStringLiteral("AnimeJaNai slot 0 is passthrough and cannot be reported as processing"));
        return;
    }
    if (configurationChange) {
        const uint64_t epoch = mpv_->sourceEpoch();
        const quint64 generation = animeJanaiGeneration_;
        animeJanaiBusy_ = true;
        setAnimeJanaiStatus(QStringLiteral("Switching AnimeJaNai configuration"));
        removeAnimeJanaiFilters(epoch, generation, [this, epoch, generation, previousBackend] {
            animeJanaiBusy_ = false;
            if (animeJanaiStageBuildLease_) animeJanaiStageBuildLease_->release();
            animeJanaiStageBuildLease_.reset();
            animeJanaiReconfigureAfterRestore_ = true;
            finishAnimeJanaiEngineBuild(
                false,
                tr("Previous %1 engine build stopped.")
                    .arg(previousBackend == native::Backend::TensorRTRTX
                             ? VideoSession::tr("TensorRT for RTX")
                             : VideoSession::tr("TensorRT")));
            restoreAnimeJanaiHwdec(epoch, generation);
        });
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
    if (animeJanaiPlan_.requiresAji && (nativeFilterBound_ || !noPrivateFilterOrTransition)) {
        const QString details =
            QStringLiteral("private mikai_native state=%1 bound=%2 reason=%3: %4; "
                           "AnimeJaNai cannot run concurrently")
                .arg(QString::fromLatin1(VideoSession::nativePlaybackStateName(nativeState)))
                .arg(nativeFilterBound_ ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(nativeStatus.reasonKey.isEmpty() ? QStringLiteral("none") : nativeStatus.reasonKey,
                     nativeStatus.reason.isEmpty() ? QStringLiteral("no additional detail")
                                                   : nativeStatus.reason);
        animeJanaiFail(QStringLiteral("animejanai.privateNativeConflict"), details);
        return;
    }
    if (!animeJanaiPlan_.requiresAji) {
        const uint64_t epoch = mpv_->sourceEpoch();
        const quint64 generation = animeJanaiGeneration_;
        const std::string spec(animeJanaiPlan_.filterSpec.constData(),
                               static_cast<size_t>(animeJanaiPlan_.filterSpec.size()));
        animeJanaiBusy_ = true;
        animeJanaiReportPending_ = true;
        animeJanaiRequest_ = mpv_->commandAsync(
            {"vf", "add", spec},
            [this, epoch, generation](const AsyncReply& attached) {
                if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                    generation != animeJanaiGeneration_) return;
                animeJanaiRequest_ = {};
                animeJanaiBusy_ = false;
                animeJanaiReportPending_ = false;
                if (!attached.ok()) {
                    animeJanaiFail(QStringLiteral("animejanai.filter.attachRefused"),
                                   QString::fromStdString(mpv_->describeError(attached.error)));
                    return;
                }
                animeJanaiFilterBound_ = true;
                animeJanaiBoundFilterLabels_ = animeJanaiFilterLabels(animeJanaiPlan_);
                setAnimeJanaiStatus(QStringLiteral("Configured; waiting for Anime4K output"));
                if (presenter_ != nullptr) presenter_->refreshRenderState();
                settleAnimeJanaiReconfigurePosition([this] { setEngineFrameHold(false); });
            });
        if (!animeJanaiRequest_.valid()) {
            animeJanaiReportPending_ = false;
            animeJanaiBusy_ = false;
            animeJanaiFail(QStringLiteral("animejanai.filter.attachNotSubmitted"),
                           QStringLiteral("mpv did not accept the Anime4K filter request"));
        }
        return;
    }
    const native::RuntimeSelection& selection = native::currentSelection();
    if (animeJanaiPlan_.requiresAji && !native::isAnimeJanaiForkRuntime(selection)) {
        animeJanaiFail(QStringLiteral("animejanai.core.unsupported"),
                       QStringLiteral("AnimeJaNai requires its own mpv fork; the loaded "
                                      "libmpv core is not that fork"));
        return;
    }
    if (slotOnlyChange) {
        const uint64_t epoch = mpv_->sourceEpoch();
        setAnimeJanaiStatus(QStringLiteral("Switching AnimeJaNai slot"));
        const quint64 generation = animeJanaiGeneration_;
        animeJanaiBusy_ = true;
        animeJanaiReportPending_ = true;
        animeJanaiRequest_ = mpv_->commandAsync(
            {"vf-command", native::kAnimeJanaiFilterLabel, "slot",
             std::to_string(animeJanaiPlan_.slot)},
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
                settleAnimeJanaiReconfigurePosition([this] {
                    if (!animeJanaiEngineBuildActive_) setEngineFrameHold(false);
                });
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

bool VideoSession::animeJanaiProcessingObserved() const {
    return animeJanaiObserved_ && animeJanaiFilterBound_ && animeJanaiEpoch_ != 0 &&
           mpv_ != nullptr && animeJanaiObservedEpoch_ == mpv_->sourceEpoch() &&
           animeJanaiObservedGeneration_ == animeJanaiGeneration_;
}

void VideoSession::resetAnimeJanaiObservation() {
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
bool VideoSession::animeJanaiBackendLogsNeeded() const {
            return mpv_ != nullptr && animeJanaiSourceRequested_ && !pendingOpen_ &&
           animeJanaiPlan_.enabled && animeJanaiPlan_.available &&
           animeJanaiPlan_.requiresAji &&
           native::isAnimeJanaiForkRuntime(native::currentSelection());
}

void VideoSession::observeAnimeJanaiBackendLog(const QString& prefix, const QString& line) {
    if (!animeJanaiBackendLogsNeeded() ||
        (!animeJanaiFilterBound_ && !animeJanaiReportPending_) ||
        (animeJanaiBusy_ && !animeJanaiReportPending_) || animeJanaiEpoch_ == 0 ||
        animeJanaiEpoch_ != mpv_->sourceEpoch()) return;

    const QString lower = (prefix + QLatin1Char(' ') + line).toLower();
    const QString product =
        animeJanaiPlan_.backend == native::Backend::TensorRTRTX
            ? VideoSession::tr("TensorRT for RTX")
            : VideoSession::tr("TensorRT");
    const int buildLogSlot = animeJanaiPlan_.selectedChainNumber > 0
                                 ? 1 : animeJanaiPlan_.slot;
    const native::AnimeJanaiBuildLogEvent buildLogEvent =
        native::isTensorRtFamily(animeJanaiPlan_.backend)
            ? animeJanaiAggregateBuildGate_.observeLog(prefix, line, animeJanaiPlan_.backend,
                                                       buildLogSlot)
            : native::classifyAnimeJanaiBuildLog(prefix, line, animeJanaiPlan_.backend,
                                                 buildLogSlot);
    if (buildLogEvent == native::AnimeJanaiBuildLogEvent::BackendSubstituted) {
        // The dispatcher kept playing on the classic backend after the
        // requested RTX one failed. Reporting the filter as processing would
        // attribute that run to a backend that never started.
        animeJanaiFail(
            QStringLiteral("animejanai.backend.substituted"),
            VideoSession::tr("TensorRT for RTX failed and the core fell back to TensorRT; "
                             "the requested backend is not running.") +
                QLatin1Char('\n') + line.trimmed());
        return;
    }
    if (buildLogEvent == native::AnimeJanaiBuildLogEvent::TensorRtBuildFailed) {
        QString failureLogPath = native::animeJanaiBuildFailureLogPath(prefix, line);
        if (failureLogPath.isEmpty())
            failureLogPath = native::animeJanaiEngineFailedLogPath(line);
        QStringList failureDetails;
        if (!failureLogPath.isEmpty()) {
            failureDetails = native::readAnimeJanaiBuildLogTail(failureLogPath,
                                                                native::nativeRoot());
        }
        if (failureDetails.isEmpty()) failureDetails << line.trimmed();
        animeJanaiFail(QStringLiteral("animejanai.build.failed"),
                       failureDetails.join(QLatin1Char('\n')));
        return;
    }
    if (buildLogEvent == native::AnimeJanaiBuildLogEvent::TensorRtEngineWarmupFailed) {
        // The fork retries the cached engine on the caller thread and reports
        // any real error there, so this stays a warning with its evidence
        // instead of tearing the filter down before that load runs.
        const QStringList warmDetails = native::readAnimeJanaiBuildLogTail(
            animeJanaiAggregateBuildGate_.warmupLogPath(), native::nativeRoot());
        qWarning().noquote() << "[animejanai] engine warm-up failed:"
                             << (warmDetails.isEmpty() ? line.trimmed()
                                                       : warmDetails.join(QLatin1Char('\n')));
        setAnimeJanaiStatus(
            tr("%1 engine warm-up failed; loading the cached engine directly.").arg(product));
        return;
    }
    const QString trimmedLine = line.trimmed();
    if (animeJanaiEngineBuildActive_ &&
        trimmedLine.compare(
            QLatin1String("background engine build finished; reconfiguring"),
            Qt::CaseInsensitive) == 0) {
        animeJanaiBackgroundBuildFinished_ = true;
    }
    if (native::animeJanaiEnginePrepEvent(buildLogEvent)) {
        resetAnimeJanaiObservation();
        ++animeJanaiPendingAjiBuildCount_;
        if (!animeJanaiEngineBuildActive_) {
            animeJanaiBuildPollingRefused_ = false;
            animeJanaiBuildNextPollMs_ = 0;
            animeJanaiBackgroundBuildFinished_ = false;
            beginEnginePause();
        }
        const QString detail =
            buildLogEvent == native::AnimeJanaiBuildLogEvent::TensorRtEnginePrepStarted
                ? tr("Preparing the selected model for this session.")
                : tr("Compiling the selected model for this video resolution.");
        setEngineFrameHold(true);
        setAnimeJanaiStatus(
            buildLogEvent == native::AnimeJanaiBuildLogEvent::TensorRtEnginePrepStarted
                ? tr("Preparing the selected model for this session.")
                : tr("Building %1 engine…").arg(product));
        emit animeJanaiEngineBuildChanged(true, false, detail);
    }
    if (buildLogEvent == native::AnimeJanaiBuildLogEvent::SlotConfigured) {
        animeJanaiConfiguredAjiStageCount_ =
            qMin(animeJanaiAjiStageCount_, animeJanaiConfiguredAjiStageCount_ + 1);
        if (animeJanaiPendingAjiBuildCount_ > 0) --animeJanaiPendingAjiBuildCount_;
        if (animeJanaiConfiguredAjiStageCount_ < animeJanaiAjiStageCount_) {
            setAnimeJanaiStatus(
                tr("Configured AnimeJaNai stage %1 of %2; waiting for remaining stages.")
                    .arg(animeJanaiConfiguredAjiStageCount_)
                    .arg(animeJanaiAjiStageCount_));
            return;
        }
        if (animeJanaiPendingAjiBuildCount_ > 0) {
            setAnimeJanaiStatus(tr("Waiting for all AnimeJaNai engine builds to finish."));
            return;
        }
    }
    const QString backendShort =
        animeJanaiPlan_.backend == native::Backend::TensorRTRTX
            ? QStringLiteral("TRT-RTX")
            : animeJanaiPlan_.backend == native::Backend::TensorRT
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
        lower.contains(QStringLiteral("interpolation")) &&
        (!animeJanaiEngineBuildActive_ || animeJanaiBackgroundBuildFinished_)) {
        double factor = 2.0;
        static const QRegularExpression kRifeFactorRe(
            QStringLiteral("rife\\s+active:\\s*(\\d+)\\s*/\\s*(\\d+)"),
            QRegularExpression::CaseInsensitiveOption);
        if (const QRegularExpressionMatch m = kRifeFactorRe.match(line); m.hasMatch()) {
            const double num = m.captured(1).toDouble();
            const double den = m.captured(2).toDouble();
            if (num > 0.0 && den > 0.0) factor = num / den;
        }
        const double baseFps = observedContainerFps_;
        const double targetFps = baseFps * factor;
        const int srcH = videoHeight_ > 0
                             ? videoHeight_
                             : static_cast<int>(std::lround(observedVideoHeight_));
        if (srcH > 0 && baseFps > 0.0) {
            observed = QStringLiteral("%1 · %2 · %3p · %4 → %5 FPS")
                           .arg(backendShort, presetLabel)
                           .arg(srcH)
                           .arg(baseFps, 0, 'f', 2)
                           .arg(targetFps, 0, 'f', 2);
        } else if (baseFps > 0.0) {
            observed = QStringLiteral("%1 · %2 · %3 → %4 FPS")
                           .arg(backendShort, presetLabel)
                           .arg(baseFps, 0, 'f', 2)
                           .arg(targetFps, 0, 'f', 2);
        } else {
            observed = QStringLiteral("%1 · %2 · interpolation active")
                           .arg(backendShort, presetLabel);
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
                inH = m.captured(2).toInt();
                outW = m.captured(3).toInt();
                outH = m.captured(4).toInt();
            }
            if (inH > 0 && outH > 0) {
                observed = QStringLiteral("Configured · %1 · %2 · %3p → %4p (%5×%6)")
                               .arg(backendShort, presetLabel)
                               .arg(inH)
                               .arg(outH)
                               .arg(outW)
                               .arg(outH);
            } else {
                observed = QStringLiteral("Configured · %1 · %2").arg(backendShort, presetLabel);
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

void VideoSession::publishAnimeJanaiObservation(const QString& status, uint64_t epoch,
                                                quint64 generation) {
    if (!animeJanaiBackendLogsNeeded() || !animeJanaiFilterBound_ ||
        epoch == 0 || epoch != mpv_->sourceEpoch() || epoch != animeJanaiEpoch_ ||
        generation != animeJanaiGeneration_) return;
    animeJanaiObserved_ = true;
    animeJanaiObservedEpoch_ = epoch;
    animeJanaiObservedGeneration_ = generation;
    animeJanaiObservedStatus_ = status;
    setAnimeJanaiStatus(status);
    finishAnimeJanaiEngineBuild(
        true,
        tr("%1 engine ready; backend configuration observed.")
            .arg(animeJanaiPlan_.backend == native::Backend::TensorRTRTX
                     ? VideoSession::tr("TensorRT for RTX")
                     : VideoSession::tr("TensorRT")));
    if (animeJanaiStageBuildLease_) animeJanaiStageBuildLease_->release();
    animeJanaiStageBuildLease_.reset();
}

void VideoSession::publishPendingAnimeJanaiObservation(uint64_t epoch, quint64 generation) {
    const QString reported = std::move(animeJanaiPendingObservedStatus_);
    const uint64_t reportEpoch = animeJanaiPendingObservedEpoch_;
    const quint64 reportGeneration = animeJanaiPendingObservedGeneration_;
    animeJanaiPendingObservedEpoch_ = 0;
    animeJanaiPendingObservedGeneration_ = 0;
    if (!reported.isEmpty() && reportEpoch == epoch && reportGeneration == generation) {
        publishAnimeJanaiObservation(reported, reportEpoch, reportGeneration);
    }
}

QString VideoSession::animeJanaiPlanNote() const {
    if (!animeJanaiPlan_.enabled || !animeJanaiPlan_.available || animeJanaiSourceGated_) {
        return QString();
    }
    return animeJanaiPlan_.note;
}

void VideoSession::setAnimeJanaiStatus(const QString& status) {
    if (animeJanaiObserved_ && animeJanaiObservedEpoch_ == (mpv_ ? mpv_->sourceEpoch() : 0) &&
        animeJanaiObservedGeneration_ == animeJanaiGeneration_ &&
        status != animeJanaiObservedStatus_) return;
    // The plan's own remarks are not state: they describe the backend for as
    // long as it is selected, so they travel beside the status (and land in the
    // chip's tooltip) instead of being appended to a line that is read at a
    // glance over the picture.
    if (animeJanaiStatus_ == status) return;
    animeJanaiStatus_ = status;
    emit animeJanaiStatusChanged(status);
}

bool VideoSession::enginePauseHeld() const {
    return enginePauseEpoch_ != 0 && mpv_ != nullptr &&
           enginePauseEpoch_ == mpv_->sourceEpoch() &&
           (enginePreparationPaused_ || animeJanaiEngineBuildActive_);
}

void VideoSession::publishEnginePauseHold() {
    const bool held = enginePauseHeld();
    if (held == enginePauseHoldPublished_) return;
    enginePauseHoldPublished_ = held;
    emit enginePauseHoldChanged(held);
}

bool VideoSession::playbackPauseIntent() const {
    return enginePauseHeld() ? enginePauseIntent_ : !isPlaying();
}

void VideoSession::beginEnginePause() {
    if (enginePauseHeld()) return;
    enginePauseIntent_ = !isPlaying();
    enginePauseRequestGeneration_ = pauseRequestGeneration_;
    if (mpv_ == nullptr || mpv_->sourceEpoch() == 0) return;
    enginePauseEpoch_ = mpv_->sourceEpoch();
    reassertPaused();
    setPlaying(false);
    publishEnginePauseHold();
}

void VideoSession::endEnginePause(bool restoreIntent) {
    if (enginePreparationPaused_ || animeJanaiEngineBuildActive_) return;
    const bool sameSource = mpv_ != nullptr && enginePauseEpoch_ != 0 &&
                            enginePauseEpoch_ == mpv_->sourceEpoch();
    const bool paused = enginePauseIntent_;
    enginePauseEpoch_ = 0;
    // Announced before the early returns: the hold is over on every one of them,
    // and a chip left standing would outlive the pause it described.
    publishEnginePauseHold();
    if (!restoreIntent || !sameSource) return;
    setPlaying(!paused);
    requestPausedWrite(paused);
}

void VideoSession::discardEnginePause() {
    enginePauseEpoch_ = 0;
    enginePreparationPaused_ = false;
    publishEnginePauseHold();
}

void VideoSession::setEnginePreparationPaused(bool active, bool restoreIntent) {
    if (active == enginePreparationPaused_) return;
    if (active) beginEnginePause();
    enginePreparationPaused_ = active;
    publishEnginePauseHold();
    if (!active) endEnginePause(restoreIntent);
}

void VideoSession::pollAnimeJanaiEngineBuild() {
    if (!animeJanaiEngineBuildActive_ || !animeJanaiFilterBound_ || animeJanaiBusy_ ||
        animeJanaiBuildPollingRefused_ || mpv_ == nullptr ||
        animeJanaiBuildPollRequest_.valid() ||
        nativeStatusClock_.elapsed() < animeJanaiBuildNextPollMs_) return;
    animeJanaiBuildNextPollMs_ = nativeStatusClock_.elapsed() + 500;
    const uint64_t epoch = mpv_->sourceEpoch();
    const quint64 generation = animeJanaiGeneration_;
    QStringList ajiLabels;
    for (const native::AnimeJanaiPlanStage& stage : animeJanaiPlan_.stages) {
        if (stage.kind == native::AnimeJanaiPlanStage::Kind::Aji &&
            !stage.filterLabel.isEmpty())
            ajiLabels.append(stage.filterLabel);
    }
    if (ajiLabels.isEmpty())
        ajiLabels.append(QString::fromLatin1(native::kAnimeJanaiFilterLabel));
    const int pollIndex = animeJanaiBuildPollStageIndex_ % ajiLabels.size();
    const std::string pollLabel = ajiLabels.at(pollIndex).toStdString();
    // Each AJI context owns its poll command; rotate across all build-capable stages.
    animeJanaiBuildPollRequest_ = mpv_->commandAsync(
        {"vf-command", pollLabel, "poll", ""},
        [this, epoch, generation, pollIndex, stageCount = ajiLabels.size()](const AsyncReply& reply) {
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                generation != animeJanaiGeneration_) return;
            animeJanaiBuildPollRequest_ = {};
            if (reply.ok() && stageCount > 1)
                animeJanaiBuildPollStageIndex_ = (pollIndex + 1) % stageCount;
            if (!reply.ok() && animeJanaiEngineBuildActive_) {
                animeJanaiBuildPollingRefused_ = true;
                animeJanaiFail(QStringLiteral("animejanai.build.pollRefused"),
                               QString::fromStdString(mpv_->describeError(reply.error)));
            }
        });
    if (!animeJanaiBuildPollRequest_.valid()) {
        animeJanaiBuildPollingRefused_ = true;
        animeJanaiFail(QStringLiteral("animejanai.build.pollNotSubmitted"),
                       QStringLiteral("mpv did not accept the engine build status poll"));
    }
}

void VideoSession::finishAnimeJanaiEngineBuild(bool succeeded, const QString& detail) {
    if (!animeJanaiEngineBuildActive_) return;
    if (animeJanaiReconfigurePositionValid_ && animeJanaiReconfigureRequest_.valid()) {
        // A settle is still confirming the restored playhead. Its continuation
        // owns the release, so the held picture is not dropped before the seek
        // that puts the position back has been issued.
        auto previous = std::move(animeJanaiReconfigureSettleDone_);
        animeJanaiReconfigureSettleDone_ = [this, previous = std::move(previous)]() mutable {
            if (previous) previous();
            setEngineFrameHold(false);
        };
    } else {
        setEngineFrameHold(false);
    }
    emit animeJanaiEngineBuildChanged(false, succeeded, detail);
    endEnginePause(true);
}

// The fork reports a failed engine build as an execution-context error, which
// names neither the build nor its cause. When the newest build log beside the
// model shows the GPU ran out of memory, that log is the failure to report.
void VideoSession::reportAnimeJanaiBackendError(const QString& line) {
    if (animeJanaiPlan_.enabled && animeJanaiPlan_.available &&
        native::isTensorRtFamily(animeJanaiPlan_.backend)) {
        const QString evidence = native::animeJanaiOutOfMemoryEngineBuildEvidence(
            animeJanaiPlan_.modelDir, native::nativeRoot());
        if (!evidence.isEmpty()) {
            animeJanaiFail(
                QStringLiteral("animejanai.build.outOfMemory"),
                tr("Not enough free GPU memory to build the %1 engine; "
                   "free GPU memory and try again.")
                        .arg(animeJanaiPlan_.backend == native::Backend::TensorRTRTX
                                 ? VideoSession::tr("TensorRT for RTX")
                                 : VideoSession::tr("TensorRT")) +
                    QLatin1Char('\n') + evidence);
            return;
        }
    }
    animeJanaiFail(QStringLiteral("animejanai.backend.failed"), line);
}

void VideoSession::animeJanaiFail(const QString& reasonKey, const QString& reason) {
    animeJanaiReportPending_ = false;
    if (!animeJanaiReconfigurePositionValid_ && mpv_ != nullptr &&
        animeJanaiReconfigureRequest_.valid()) {
        // A capture still in flight must not go on to apply a plan this failure
        // has already abandoned.
        mpv_->cancelAsync(animeJanaiReconfigureRequest_);
        animeJanaiReconfigureRequest_ = AsyncRequestId{};
    }
    if (!animeJanaiFilterBound_ && animeJanaiPreviousHwdec_.isEmpty()) {
        // No decoder or filter write has run, so the playhead never moved and a
        // kept target would later seek back to a position the viewer has left.
        discardAnimeJanaiReconfigurePosition();
    }
    if (animeJanaiFilterBound_ && !animeJanaiFailureCleanupActive_ && mpv_ != nullptr) {
        animeJanaiFailureCleanupActive_ = true;
        const uint64_t epoch = mpv_->sourceEpoch();
        const quint64 generation = animeJanaiGeneration_;
        removeAnimeJanaiFilters(epoch, generation, [this, epoch, generation] {
            animeJanaiFailureCleanupActive_ = false;
            if (animeJanaiStageBuildLease_) animeJanaiStageBuildLease_->release();
            animeJanaiStageBuildLease_.reset();
            if (mpv_ != nullptr && epoch == mpv_->sourceEpoch() &&
                generation == animeJanaiGeneration_) {
                restoreAnimeJanaiHwdec(epoch, generation);
            }
        });
    } else if (!animeJanaiFilterBound_) {
        if (animeJanaiStageBuildLease_) animeJanaiStageBuildLease_->release();
        animeJanaiStageBuildLease_.reset();
    }
    animeJanaiBackgroundBuildFinished_ = false;
    resetAnimeJanaiObservation();
    if (mpv_ != nullptr) {
        try {
            mpv_->setBackendLogObservationEnabled(false);
        } catch (const MpvError& error) {
            emit errorOccurred(QString::fromUtf8(error.what()));
        }
    }
    const QString detail = reasonKey + QStringLiteral(": ") + reason;
    qWarning().noquote() << "[animejanai]" << detail;
    finishAnimeJanaiEngineBuild(false, detail);
    setEngineFrameHold(false);
    setAnimeJanaiStatus(QStringLiteral("Processing failed — ") + detail);
    emit animeJanaiFailure(detail);
}

void VideoSession::cancelAnimeJanaiOperation() {
    animeJanaiBackgroundBuildFinished_ = false;
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
    // The reconfigure target/read is NOT cancelled here: a superseding plan that
    // really changes the output discards it explicitly before this cancel, and a
    // no-op plan must not drop the settle that is still putting the playhead
    // back. Its own guards keep it from acting on a replaced source.
}

void VideoSession::discardAnimeJanaiReconfigurePosition() {
    ++animeJanaiReconfigureDiscardSerial_;
    const bool settling = animeJanaiReconfigurePositionValid_;
    animeJanaiReconfigurePosition_ = 0.0;
    animeJanaiReconfigurePositionValid_ = false;
    animeJanaiReconfigureEpoch_ = 0;
    animeJanaiReconfigureGeneration_ = 0;
    if (!settling) {
        // An in-flight capture belongs to the plan that is about to apply, so it
        // is left running; it sees the serial move and re-reads the position the
        // viewer just chose instead of storing the pre-seek one. Cancelling it
        // would strand that plan with no continuation.
        return;
    }
    if (mpv_ != nullptr && animeJanaiReconfigureRequest_.valid()) {
        mpv_->cancelAsync(animeJanaiReconfigureRequest_);
    }
    animeJanaiReconfigureRequest_ = AsyncRequestId{};
    auto settleDone = std::move(animeJanaiReconfigureSettleDone_);
    animeJanaiReconfigureSettleDone_ = {};
    if (settleDone) settleDone();
}

void VideoSession::beginAnimeJanaiReconfigureHold(std::function<void()> onCaptured) {
    // Keep the current picture up while the decoder rebuilds: capturing later,
    // after the hwdec write, captures the gap instead of the frame.
    setEngineFrameHold(true);
    if (animeJanaiReconfigureRestoreSuppressed_) {
        // This operation's restore was cancelled by a viewer seek: the chain is
        // still reconfigured, but nothing captured now may become a seek target.
        if (onCaptured) onCaptured();
        return;
    }
    // A target captured for this very source and operation already describes
    // where the viewer was before the first write; recapturing now would record
    // a position an earlier stage of the same reconfigure has already moved.
    if (animeJanaiReconfigurePositionValid_ && mpv_ != nullptr &&
        animeJanaiReconfigureEpoch_ == mpv_->sourceEpoch() &&
        animeJanaiReconfigureGeneration_ == animeJanaiGeneration_) {
        if (onCaptured) onCaptured();
        return;
    }
    animeJanaiReconfigurePosition_ = 0.0;
    animeJanaiReconfigurePositionValid_ = false;
    if (animeJanaiReconfigureRequest_.valid()) {
        // The read in flight belongs to a superseded stage: cancel it (a settle's
        // own continuation is dropped with it) so this stage's confirmed position
        // is the one that lands.
        if (mpv_ != nullptr) mpv_->cancelAsync(animeJanaiReconfigureRequest_);
        animeJanaiReconfigureRequest_ = AsyncRequestId{};
        animeJanaiReconfigureSettleDone_ = {};
    }
    if (mpv_ == nullptr || url_.isEmpty()) {
        if (onCaptured) onCaptured();
        return;
    }
    const uint64_t epoch = mpv_->sourceEpoch();
    const quint64 generation = animeJanaiGeneration_;
    const quint64 discardSerial = animeJanaiReconfigureDiscardSerial_;
    // The core's own clock is read BEFORE anything writes hwdec or the filter
    // chain: both writes re-init the decoder, and mpv then lands the playhead on
    // a keyframe. A cached UI position would either lag that clock or record the
    // post-write jump as the place to restore.
    animeJanaiReconfigureRequest_ = mpv_->getPropertyAsync(
        "time-pos", MPV_FORMAT_DOUBLE,
        [this, epoch, generation, discardSerial, onCaptured = std::move(onCaptured)](
            const AsyncReply& reply) mutable {
            animeJanaiReconfigureRequest_ = AsyncRequestId{};
            // A reply from a source or operation that is gone describes a
            // position this session no longer owns.
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                generation != animeJanaiGeneration_) {
                return;
            }
            if (discardSerial != animeJanaiReconfigureDiscardSerial_) {
                // A viewer seek superseded this operation's automatic restore. The
                // seek may still be decoding, so no position read here could name
                // where it will land; the chain is applied with NO target for the
                // rest of this operation instead of seeking backwards later.
                animeJanaiReconfigureRestoreSuppressed_ = true;
                animeJanaiReconfigurePosition_ = 0.0;
                animeJanaiReconfigurePositionValid_ = false;
                if (onCaptured) onCaptured();
                return;
            }
            if (!reply.ok() || !std::isfinite(reply.number) || reply.number < 0.0) {
                animeJanaiFail(QStringLiteral("animejanai.position.unavailable"),
                               QStringLiteral("the core did not report a usable position before "
                                              "the chain was reconfigured"));
                return;
            }
            // Timestamp zero is a real position (a source parked at its start),
            // so only an unavailable or negative reading is refused.
            animeJanaiReconfigurePosition_ = reply.number;
            animeJanaiReconfigurePositionValid_ = true;
            animeJanaiReconfigureEpoch_ = epoch;
            animeJanaiReconfigureGeneration_ = generation;
            if (onCaptured) onCaptured();
        });
    if (!animeJanaiReconfigureRequest_.valid()) {
        animeJanaiFail(QStringLiteral("animejanai.position.request"),
                       QStringLiteral("mpv did not accept the position read that guards the "
                                      "reconfigure"));
    }
}

void VideoSession::settleAnimeJanaiReconfigurePosition(std::function<void()> onSettled) {
    auto finish = [onSettled = std::move(onSettled)]() mutable {
        if (onSettled) onSettled();
    };
    if (!animeJanaiReconfigurePositionValid_ || mpv_ == nullptr || url_.isEmpty()) {
        animeJanaiReconfigurePosition_ = 0.0;
        animeJanaiReconfigurePositionValid_ = false;
        finish();
        return;
    }
    if (animeJanaiReconfigureRequest_.valid() || animeJanaiReconfigureSettleDone_) {
        // A probe or a spaced confirmation is already running for this target
        // (possibly between timer ticks, with no read in flight). Its
        // continuation owns the hold release, and no second seek may be issued,
        // so this caller rides on it instead of starting another sequence.
        auto previous = std::move(animeJanaiReconfigureSettleDone_);
        animeJanaiReconfigureSettleDone_ = [previous = std::move(previous),
                                            finish = std::move(finish)]() mutable {
            if (previous) previous();
            finish();
        };
        return;
    }
    const double target = animeJanaiReconfigurePosition_;
    const uint64_t epoch = animeJanaiReconfigureEpoch_;
    animeJanaiReconfigureSettleDone_ = finish;
    // The restore decision is made from the core's confirmed clock, read after
    // the write whose own re-seek moves it - never from the 50 ms UI cache.
    animeJanaiReconfigureRequest_ = mpv_->getPropertyAsync(
        "time-pos", MPV_FORMAT_DOUBLE,
        [this, target, epoch](const AsyncReply& reply) {
            animeJanaiReconfigureRequest_ = AsyncRequestId{};
            // Still owed only while the same source owns this target and it was
            // not discarded: a viewer seek clears it, and a superseding
            // reconfigure that really changes the chain cancels this read.
            const bool stillOwed =
                mpv_ != nullptr && epoch == mpv_->sourceEpoch() &&
                animeJanaiReconfigurePositionValid_ &&
                animeJanaiReconfigurePosition_ == target;
            // mpv's own keyframe landing is allowed this much slack; anything
            // larger is the jump the viewer would see.
            constexpr double kRestoreTolerance = 0.35;
            const bool landed = reply.ok() && std::isfinite(reply.number) &&
                                std::abs(reply.number - target) <= kRestoreTolerance;
            if (!stillOwed) {
                // A viewer seek or a replaced source owns the playhead; this is
                // an expected cancellation and no restore is owed.
                animeJanaiReconfigurePosition_ = 0.0;
                animeJanaiReconfigurePositionValid_ = false;
                auto settleDone = std::move(animeJanaiReconfigureSettleDone_);
                animeJanaiReconfigureSettleDone_ = {};
                if (settleDone) settleDone();
                return;
            }
            if (!landed && reply.ok() && std::isfinite(reply.number)) {
                // Exactly one exact seek, then the target stays owed until the
                // core itself reports the playhead back at it: submitting the
                // command is not settlement, and the hold is released by that
                // confirmation. The public seekExact is deliberately not used:
                // it carries the viewer-seek semantics that cancel this restore.
                // Submitting directly keeps the confirming reads queued AFTER it.
                (void)mpv_->commandAsync({"seek", mpvNumber(target), "absolute+exact"},
                                         [](const AsyncReply&) {});
                emit seekRequested();
                confirmAnimeJanaiReconfigureSettled(
                    target, epoch, nativePlaybackNowMs() + kReconfigureSettleTimeoutMs, 0);
                return;
            }
            if (!landed) {
                // The playhead cannot even be read, so no seek can be justified
                // and no restore is claimed: this is a visible processing error,
                // not a silent release.
                QString reason;
                if (mpv_ != nullptr)
                    reason = QString::fromStdString(mpv_->describeError(reply.error));
                const QString detail =
                    tr("AnimeJaNai is running, but the playhead could not be read to "
                       "restore it (%1). Playback continues from where the core is.")
                        .arg(reason.isEmpty() ? tr("no usable position") : reason);
                qWarning().noquote() << "[animejanai]" << detail;
                emit animeJanaiFailure(detail);
            }
            // Already at the target, or reported above: no further seek.
            animeJanaiReconfigurePosition_ = 0.0;
            animeJanaiReconfigurePositionValid_ = false;
            auto settleDone = std::move(animeJanaiReconfigureSettleDone_);
            animeJanaiReconfigureSettleDone_ = {};
            if (settleDone) settleDone();
        });
    if (!animeJanaiReconfigureRequest_.valid()) {
        animeJanaiReconfigurePosition_ = 0.0;
        animeJanaiReconfigurePositionValid_ = false;
        auto settleDone = std::move(animeJanaiReconfigureSettleDone_);
        animeJanaiReconfigureSettleDone_ = {};
        if (settleDone) settleDone();
    }
}

void VideoSession::confirmAnimeJanaiReconfigureSettled(double target, uint64_t epoch,
                                                       qint64 deadlineMs, int reads) {
    const auto cancelSilently = [this]() {
        // A viewer seek (discard) or a replaced source owns the playhead now:
        // that is an expected cancellation, not a processing failure, and it is
        // never reported as a settled restore.
        animeJanaiReconfigurePosition_ = 0.0;
        animeJanaiReconfigurePositionValid_ = false;
        animeJanaiReconfigureRequest_ = AsyncRequestId{};
        auto settleDone = std::move(animeJanaiReconfigureSettleDone_);
        animeJanaiReconfigureSettleDone_ = {};
        if (settleDone) settleDone();
    };
    const auto reportUnconfirmed = [this, target, reads](const QString& why) {
        // The chain itself was applied; what failed is this session's ability to
        // confirm the playhead. That is a visible processing error with context,
        // not a silent diagnostic and not a "restore succeeded".
        const QString detail =
            tr("AnimeJaNai is running, but the playhead could not be confirmed back "
               "at %1 s (%2). Playback continues from where the core landed.")
                .arg(target, 0, 'f', 3)
                .arg(why);
        qWarning().noquote() << "[animejanai]" << detail;
        if (reads > 0)
            logFrameTiming(QStringLiteral("animejanai reconfigure settle unconfirmed "
                                          "target=%1s checks=%2")
                               .arg(target, 0, 'f', 6)
                               .arg(reads));
        emit animeJanaiFailure(detail);
    };
    // A viewer seek (discard) clears the target, and a source change owns
    // another operation: either ends this restore without seeking again.
    if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
        !animeJanaiReconfigurePositionValid_ ||
        animeJanaiReconfigurePosition_ != target) {
        cancelSilently();
        return;
    }
    if (reads >= kReconfigureSettleMaxReads || nativePlaybackNowMs() > deadlineMs) {
        reportUnconfirmed(tr("%1 position checks over %2 ms")
                              .arg(reads)
                              .arg(kReconfigureSettleTimeoutMs));
        cancelSilently();
        return;
    }
    animeJanaiReconfigureRequest_ = mpv_->getPropertyAsync(
        "time-pos", MPV_FORMAT_DOUBLE,
        [this, target, epoch, deadlineMs, reads,
         cancelSilently, reportUnconfirmed](const AsyncReply& reply) {
            animeJanaiReconfigureRequest_ = AsyncRequestId{};
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() ||
                !animeJanaiReconfigurePositionValid_ ||
                animeJanaiReconfigurePosition_ != target) {
                cancelSilently();
                return;
            }
            constexpr double kRestoreTolerance = 0.35;
            const bool landed = reply.ok() && std::isfinite(reply.number) &&
                                std::abs(reply.number - target) <= kRestoreTolerance;
            if (landed) {
                // This observation is the settlement: the target is discharged
                // and the hold release runs now, not on the seek submission.
                animeJanaiReconfigurePosition_ = 0.0;
                animeJanaiReconfigurePositionValid_ = false;
                auto settleDone = std::move(animeJanaiReconfigureSettleDone_);
                animeJanaiReconfigureSettleDone_ = {};
                if (settleDone) settleDone();
                return;
            }
            if (!reply.ok()) {
                // A refused read cannot confirm anything; release by name rather
                // than waiting out the bound on a core that will not answer.
                QString reason;
                if (mpv_ != nullptr)
                    reason = QString::fromStdString(mpv_->describeError(reply.error));
                reportUnconfirmed(tr("the position read was refused: %1").arg(reason));
                cancelSilently();
                return;
            }
            if (reads + 1 >= kReconfigureSettleMaxReads ||
                nativePlaybackNowMs() + kReconfigureSettleConfirmIntervalMs > deadlineMs) {
                reportUnconfirmed(tr("%1 position checks over %2 ms")
                                      .arg(reads + 1)
                                      .arg(kReconfigureSettleTimeoutMs));
                cancelSilently();
                return;
            }
            // Still owed. The next observation is spaced by the session's own
            // timer so the decoder can actually decode the sought frame; re-reads
            // are not chained back-to-back and nothing blocks the GUI thread.
            QTimer::singleShot(kReconfigureSettleConfirmIntervalMs, this,
                               [this, target, epoch, deadlineMs, reads] {
                                   confirmAnimeJanaiReconfigureSettled(
                                       target, epoch, deadlineMs, reads + 1);
                               });
        });
    if (!animeJanaiReconfigureRequest_.valid()) {
        reportUnconfirmed(tr("the core would not take the position read"));
        cancelSilently();
    }
}

void VideoSession::processAnimeJanaiPlan() {
    if (!mpv_ || !animeJanaiPlan_.enabled || !animeJanaiPlan_.available) return;
    // Unknown transfer metadata is not proof of SDR; the explicit override
    // applies only to a source positively identified as HDR.
    if (hdrRenderingFixesEnabled() && sourceColorProfile_.requiresSdrGate() &&
        !(sourceColorProfile_.isHdr() && animeJanaiHdrOverride())) {
        animeJanaiSourceGated_ = true;
        setAnimeJanaiStatus(sourceColorProfile_.isHdr()
                                ? tr("Skipped on HDR: native stages force 8-bit NV12")
                                : tr("Skipped: source transfer metadata is unknown"));
        return;
    }
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
    // The confirmed playhead is captured before the hwdec and vf writes below:
    // both re-init the decoder, and the restore must compare against the position
    // the viewer actually had.
    beginAnimeJanaiReconfigureHold([this] { applyAnimeJanaiFilterPlan(); });
}

void VideoSession::applyAnimeJanaiFilterPlan() {
    if (!mpv_ || !animeJanaiPlan_.enabled || !animeJanaiPlan_.available ||
        animeJanaiBusy_ || !animeJanaiSourceRequested_ || url_.isEmpty() || pendingOpen_ ||
        animeJanaiFilterBound_) {
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
            // T-413: `d3d11`, the canonical mpv spelling, everywhere a core is
            // aligned for DirectML - the probe path (backendHwdec) spells it the
            // same way, so the write/readback comparison cannot drift on the
            // legacy `d3d11va` alias.
            const QString required = native::isTensorRtFamily(animeJanaiPlan_.backend)
                                         ? QStringLiteral("nvdec")
                                         : QStringLiteral("d3d11");
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
                            if (!animeJanaiStageBuildLease_) {
                                if (animeJanaiPlan_.cacheKeys.isEmpty()) {
                                    animeJanaiBusy_ = false;
                                    animeJanaiFail(QStringLiteral("animejanai.cache.keysMissing"),
                                                   QStringLiteral("The AJI plan has no owned engine-cache keys."));
                                    restoreAnimeJanaiHwdec(epoch, generation);
                                    return;
                                }
                                QString busyKey;
                                QString busyOwner;
                                animeJanaiStageBuildLease_ = animeJanaiPlan_.cacheLease;
                                if (!animeJanaiStageBuildLease_ ||
                                    !animeJanaiStageBuildLease_->held())
                                    animeJanaiStageBuildLease_ =
                                    native::AnimeJanaiStageBuildLease::tryAcquire(
                                        animeJanaiPlan_.cacheKeys,
                                        QStringLiteral("%1:%2:%3")
                                            .arg(animeJanaiPlan_.presetId)
                                            .arg(epoch)
                                            .arg(generation),
                                        &busyKey, &busyOwner);
                                if (!animeJanaiStageBuildLease_) {
                                    animeJanaiBusy_ = false;
                                    animeJanaiFail(
                                        QStringLiteral("animejanai.cache.busy"),
                                        tr("The engine cache is in use by %1 (%2).")
                                            .arg(busyOwner, busyKey));
                                    restoreAnimeJanaiHwdec(epoch, generation);
                                    return;
                                }
                            }
                            native::preloadAnimeJanaiBackendLibs(animeJanaiPlan_.backendLibDirs);
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
                                    animeJanaiBoundFilterLabels_ =
                                        animeJanaiFilterLabels(animeJanaiPlan_);
                                    animeJanaiReportPending_ = false;
                                    setAnimeJanaiStatus(QStringLiteral(
                                        "Configured; filter accepted (inference not observed)"));
                                    publishPendingAnimeJanaiObservation(epoch, generation);
                                    // The chain changed under a running core, and
                                    // a paused one produces no frame to rebuild
                                    // it on, so the render state is refreshed.
                                    if (presenter_ != nullptr) presenter_->refreshRenderState();
                                    settleAnimeJanaiReconfigurePosition([this] {
                                        if (!animeJanaiEngineBuildActive_) {
                                            setEngineFrameHold(false);
                                            if (animeJanaiStageBuildLease_) {
                                                animeJanaiStageBuildLease_->release();
                                                animeJanaiStageBuildLease_.reset();
                                            }
                                        }
                                    });
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

void VideoSession::restoreAnimeJanaiHwdec(uint64_t epoch, quint64 generation) {
    if (!mpv_ || animeJanaiPreviousHwdec_.isNull() || animeJanaiPreviousHwdec_.isEmpty()) {
        if (animeJanaiReconfigureAfterRestore_) {
            setEngineFrameHold(true);
            animeJanaiReconfigureAfterRestore_ = false;
            processAnimeJanaiPlan();
        } else {
            settleAnimeJanaiReconfigurePosition([this] {
                setEngineFrameHold(false);
                if (!animeJanaiPlan_.enabled) setAnimeJanaiStatus(QStringLiteral("Off"));
            });
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
                setEngineFrameHold(true);
                animeJanaiReconfigureAfterRestore_ = false;
                processAnimeJanaiPlan();
            } else {
                settleAnimeJanaiReconfigurePosition([this] {
                    setEngineFrameHold(false);
                    if (!animeJanaiPlan_.enabled) setAnimeJanaiStatus(QStringLiteral("Off"));
                });
            }
        });
    if (!animeJanaiRequest_.valid()) {
        animeJanaiReconfigureAfterRestore_ = false;
        animeJanaiFail(QStringLiteral("animejanai.hwdec.restoreNotSubmitted"),
                       QStringLiteral("mpv did not accept restoring the previous hwdec setting"));
    }
}


}  // namespace mplayer
