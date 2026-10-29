// AnimeJaNai filter plan binding, log observation, engine build polling, and
// hardware decoder restoration for VideoSurface.

#include "VideoSurface.hpp"

#include "MpvHandle.hpp"
#include "native/components/AnimeJanaiBuildLog.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>

#include <string>
#include <utility>

#ifndef Q_OS_WIN
#include <dlfcn.h>
#endif

namespace {

/// The fork's TensorRT backend resolves libnvinfer by bare soname, so the
/// closure folder must already be on the process loader path when the filter
/// initialises; a plain dlopen failure surfaces as the opaque "Failed to
/// create inference context". Preloading the closure RTLD_GLOBAL makes those
/// sonames resolvable. Idempotent: already-loaded sonames are skipped.
void preloadAnimeJanaiBackendLibs(const QStringList& dirs) {
#ifndef Q_OS_WIN
    for (const QString& dirPath : dirs) {
        if (dirPath.isEmpty()) continue;
        const QDir dir(dirPath);
        const QFileInfoList entries = dir.entryInfoList(
            {QStringLiteral("*.so"), QStringLiteral("*.so.*")}, QDir::Files, QDir::Name);
        for (const QFileInfo& entry : entries) {
            const QByteArray path = entry.absoluteFilePath().toUtf8();
            if (::dlopen(path.constData(), RTLD_NOW | RTLD_GLOBAL) != nullptr)
                continue;
            qWarning().noquote() << "[animejanai] backend preload failed for"
                                 << entry.fileName() << ":" << ::dlerror();
        }
    }
#else
    Q_UNUSED(dirs);
#endif
}

}  // namespace

namespace mplayer {

namespace {

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

void VideoSurface::removeAnimeJanaiFilters(uint64_t epoch, quint64 generation,
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

void VideoSurface::setAnimeJanaiPlan(const native::AnimeJanaiPlan& plan) {
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
    if (processingConfigurationChanged) setEngineFrameHold(true);
    cancelAnimeJanaiOperation();
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
            if (animeJanaiStageBuildLease_) animeJanaiStageBuildLease_->release();
            animeJanaiStageBuildLease_.reset();
            setEngineFrameHold(false);
            restoreAnimeJanaiHwdec(epoch, generation);
            return;
        }
        animeJanaiBusy_ = true;
        removeAnimeJanaiFilters(epoch, generation, [this, epoch, generation] {
            animeJanaiBusy_ = false;
            if (animeJanaiStageBuildLease_) animeJanaiStageBuildLease_->release();
            animeJanaiStageBuildLease_.reset();
            finishAnimeJanaiEngineBuild(false, tr("TensorRT engine build cancelled."));
            setEngineFrameHold(false);
            restoreAnimeJanaiHwdec(epoch, generation);
        });
        return;
    }
    if (!plan.available) {
        animeJanaiFail(plan.reasonKey.isEmpty() ? QStringLiteral("animejanai.plan.unavailable")
                                                : plan.reasonKey,
                       plan.reason.isEmpty() ? QStringLiteral("AnimeJaNai plan is unavailable")
                                             : plan.reason);
        return;
    }
    if (plan.filterSpec.isEmpty() || plan.stages.isEmpty()) {
        animeJanaiFail(QStringLiteral("animejanai.plan.filterSpecInvalid"),
                       QStringLiteral("available AnimeJaNai plan has no executable filter stages"));
        return;
    }
    const bool sourceAllowed =
        !sourceColorProfile_.requiresSdrGate() ||
        (sourceColorProfile_.isHdr() && animeJanaiHdrOverride());
    if (hdrRenderingFixesEnabled() && plan.enabled && !sourceAllowed) {
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
    if (plan.requiresAji &&
        std::none_of(plan.stages.cbegin(), plan.stages.cend(),
                     [](const native::AnimeJanaiPlanStage& stage) {
                         return stage.kind == native::AnimeJanaiPlanStage::Kind::Aji;
                     })) {
        animeJanaiFail(QStringLiteral("animejanai.plan.filterSpecInvalid"),
                       QStringLiteral("AJI plan has no AnimeJaNai stage"));
        return;
    }
    if (plan.requiresAji && plan.slot == 0) {
        animeJanaiFail(QStringLiteral("animejanai.slot.bypass"),
                       QStringLiteral("AnimeJaNai slot 0 is passthrough and cannot be reported as processing"));
        return;
    }
    if (configurationChange) {
        const uint64_t epoch = mpv_->sourceEpoch();
        const quint64 generation = animeJanaiGeneration_;
        animeJanaiBusy_ = true;
        setAnimeJanaiStatus(QStringLiteral("Switching AnimeJaNai configuration"));
        removeAnimeJanaiFilters(epoch, generation, [this, epoch, generation] {
            animeJanaiBusy_ = false;
            if (animeJanaiStageBuildLease_) animeJanaiStageBuildLease_->release();
            animeJanaiStageBuildLease_.reset();
            animeJanaiReconfigureAfterRestore_ = true;
            finishAnimeJanaiEngineBuild(false, tr("Previous TensorRT engine build stopped."));
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
    if (plan.requiresAji && (nativeFilterBound_ || !noPrivateFilterOrTransition)) {
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
    if (!plan.requiresAji) {
        const uint64_t epoch = mpv_->sourceEpoch();
        const quint64 generation = animeJanaiGeneration_;
        const std::string spec(plan.filterSpec.constData(),
                               static_cast<size_t>(plan.filterSpec.size()));
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
                refreshRenderState();
                setEngineFrameHold(false);
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
    if (plan.requiresAji && !native::isAnimeJanaiForkRuntime(selection)) {
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
                if (!animeJanaiEngineBuildActive_) setEngineFrameHold(false);
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
           animeJanaiPlan_.requiresAji &&
           native::isAnimeJanaiForkRuntime(native::currentSelection());
}

void VideoSurface::observeAnimeJanaiBackendLog(const QString& prefix, const QString& line) {
    if (!animeJanaiBackendLogsNeeded() ||
        (!animeJanaiFilterBound_ && !animeJanaiReportPending_) ||
        (animeJanaiBusy_ && !animeJanaiReportPending_) || animeJanaiEpoch_ == 0 ||
        animeJanaiEpoch_ != mpv_->sourceEpoch()) return;

    const QString lower = (prefix + QLatin1Char(' ') + line).toLower();
    const int buildLogSlot = animeJanaiPlan_.selectedChainNumber > 0
                                 ? 1 : animeJanaiPlan_.slot;
    const native::AnimeJanaiBuildLogEvent buildLogEvent =
        animeJanaiPlan_.backend == native::Backend::TensorRT
            ? animeJanaiAggregateBuildGate_.observeLog(prefix, line, animeJanaiPlan_.backend,
                                                       buildLogSlot)
            : native::classifyAnimeJanaiBuildLog(prefix, line, animeJanaiPlan_.backend,
                                                 buildLogSlot);
    if (buildLogEvent == native::AnimeJanaiBuildLogEvent::TensorRtBuildFailed) {
        const QString enginePath = native::animeJanaiBuildFailureEnginePath(prefix, line);
        QStringList failureDetails;
        if (!enginePath.isEmpty()) {
            failureDetails = native::readAnimeJanaiBuildLogTail(
                enginePath + QStringLiteral(".build.log"), native::nativeRoot());
        }
        if (failureDetails.isEmpty()) failureDetails << line.trimmed();
        animeJanaiFail(QStringLiteral("animejanai.build.failed"),
                       failureDetails.join(QLatin1Char('\n')));
        return;
    }
    const QString trimmedLine = line.trimmed();
    if (animeJanaiEngineBuildActive_ &&
        trimmedLine.compare(
            QLatin1String("background engine build finished; reconfiguring"),
            Qt::CaseInsensitive) == 0) {
        animeJanaiBackgroundBuildFinished_ = true;
    }
    if (buildLogEvent == native::AnimeJanaiBuildLogEvent::TensorRtBuildStarted) {
        ++animeJanaiPendingAjiBuildCount_;
        if (!animeJanaiEngineBuildActive_) {
            animeJanaiBuildPollingRefused_ = false;
            animeJanaiBuildNextPollMs_ = 0;
            animeJanaiBackgroundBuildFinished_ = false;
            beginEnginePause();
        }
        const QString detail = tr("Compiling the selected model for this video resolution.");
        setEngineFrameHold(true);
        setAnimeJanaiStatus(tr("Building TensorRT engine…"));
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
                inW = m.captured(1).toInt();
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
    finishAnimeJanaiEngineBuild(true, tr("TensorRT engine ready; backend configuration observed."));
    if (animeJanaiStageBuildLease_) animeJanaiStageBuildLease_->release();
    animeJanaiStageBuildLease_.reset();
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
    QString display = status;
    if (animeJanaiPlan_.enabled && animeJanaiPlan_.available &&
        !animeJanaiSourceGated_ && !animeJanaiPlan_.note.isEmpty() &&
        !status.startsWith(QStringLiteral("Processing failed"))) {
        display += QStringLiteral(" · ") + animeJanaiPlan_.note;
    }
    if (animeJanaiStatus_ == display) return;
    animeJanaiStatus_ = display;
    emit animeJanaiStatusChanged(display);
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

void VideoSurface::finishAnimeJanaiEngineBuild(bool succeeded, const QString& detail) {
    if (!animeJanaiEngineBuildActive_) return;
    setEngineFrameHold(false);
    emit animeJanaiEngineBuildChanged(false, succeeded, detail);
    endEnginePause(true);
}

void VideoSurface::animeJanaiFail(const QString& reasonKey, const QString& reason) {
    animeJanaiReportPending_ = false;
    if (animeJanaiFilterBound_ && !animeJanaiFailureCleanupActive_ && mpv_ != nullptr) {
        animeJanaiFailureCleanupActive_ = true;
        const uint64_t epoch = mpv_->sourceEpoch();
        const quint64 generation = animeJanaiGeneration_;
        removeAnimeJanaiFilters(epoch, generation, [this] {
            animeJanaiFailureCleanupActive_ = false;
            if (animeJanaiStageBuildLease_) animeJanaiStageBuildLease_->release();
            animeJanaiStageBuildLease_.reset();
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

void VideoSurface::cancelAnimeJanaiOperation() {
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
}

void VideoSurface::processAnimeJanaiPlan() {
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
            const QString required = animeJanaiPlan_.backend == native::Backend::TensorRT
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
                            preloadAnimeJanaiBackendLibs(animeJanaiPlan_.backendLibDirs);
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
                                    refreshRenderState();
                                    if (!animeJanaiEngineBuildActive_) setEngineFrameHold(false);
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
            setEngineFrameHold(true);
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
                setEngineFrameHold(true);
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
