// Processing consumers of the native Quick player controller: the Anime4K and
// AnimeJaNai chains, the native admission/status/observation assembly, reference
// frame capture, processing failures and remedies, and the separate engine
// preparation status window. Generic status publication stays in the controller
// host unit; this unit mutates the processing fields and asks it to publish.

#include "PlayerController.hpp"

#include "PreparationWindow.hpp"
#include "PlayerWindow.hpp"

#include "../core/VideoSession.hpp"
#include "../native/bundle/NativePrefs.hpp"
#include "../native/bundle/NativeRuntimeSelection.hpp"
#include "../native/models/NativeUpscalers.hpp"
#include "../native/status/NativeMenu.hpp"
#include "../native/status/NativeStatus.hpp"
#include "../native/status/NativeTypes.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <QSettings>
#include <QThreadPool>
#include <QUrl>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

namespace mplayer {
namespace quick {

namespace {

constexpr auto kAnime4KSettingKey = "player/anime4k";
constexpr auto kAnimeJanaiPresetSettingKey = "player/animejanai/preset";
constexpr auto kAnimeJanaiBackendSettingKey = "player/animejanai/backend";

/// The catalogue's name for a preset identity in the language installed now, or
/// `fallback` when the catalogue no longer offers that identity.
QString animeJanaiPresetLabel(const QString& presetId, const QString& fallback) {
    if (!presetId.isEmpty()) {
        for (const native::AnimeJanaiPreset& preset : native::animeJanaiPresets()) {
            if (preset.id.compare(presetId, Qt::CaseInsensitive) == 0) return preset.label;
        }
    }
    return fallback;
}

/// The factory observation the session that owns playback published for its
/// current source, or nullptr when it published none. The pointer addresses the
/// session's own view, so a replaced source cannot keep the old one eligible.
const native::NativeFactoryObservation* observedFactoryOf(const VideoSession* owner) {
    if (owner == nullptr) return nullptr;
    const native::NativeStatusView& view = owner->nativeStatus();
    if (!view.snapshot.has_value() || !view.snapshot->factory.has_value()) return nullptr;
    return &*view.snapshot->factory;
}

/// A refused transition that only says "there was nothing to do". It arrives
/// through the same failure signal as a real one, so it is named here and
/// swallowed rather than shown as a failure the viewer cannot act on.
bool nativeFailureIsBenign(const QString& reasonKey) {
    return reasonKey == QLatin1String("native.playback.nothingToRemove") ||
           reasonKey == QLatin1String("native.playback.nothingToDrain") ||
           reasonKey == QLatin1String("native.playback.nothingToSuspend") ||
           reasonKey == QLatin1String("native.playback.notSuspended") ||
           reasonKey == QLatin1String("native.playback.noSource") ||
           reasonKey == QLatin1String("native.playback.transitionInFlight");
}

}  // namespace

// ── native assembly ─────────────────────────────────────────────────────────

void PlayerController::pushNativeInputs() {
    if (closed_) return;
    // The four inputs the status HUD and the native menu read, replaced as one:
    // the request, the producer availability, the runtime actually selected and
    // the filter's own status. None of them is invented here.
    native::menu::Inputs inputs;
    inputs.preferences = native::loadNativePreferences();
    inputs.preferences.enabled = nativePlaybackRequested_ && inputs.preferences.enabled;
    const native::NativeFactoryObservation* factory = observedFactoryOf(session_);
    inputs.availability = native::currentNativeAvailability(factory);
    inputs.runtime = native::currentSelection();
    if (session_ != nullptr) inputs.status = session_->nativeStatus();
    nativeInputs_ = inputs;
    publishNativeHud();
    syncNativeStatusPolling(nativeInputs_.preferences.enabled);
}

void PlayerController::refreshNativeInputs() {
    pushNativeInputs();
    // A settings change writes the store and then asks for this refresh, so the
    // selection it may have changed is announced from here too. The
    // announcement dedups and returns early while the request is off.
    announceNativeRequest();
}

void PlayerController::refreshStoredSettings() {
    if (closed_) return;
    installPresetShortcuts();
    QSettings settings;
    // Display scaling is process-wide; reading the store keeps a player
    // refreshed from a store edited elsewhere in step. The setter applies only a
    // real change.
    VideoSession::setVideoScalePolicy(videoScalePolicyFromSettings(settings));

    const QString anime4kId = settings.value(kAnime4KSettingKey, QStringLiteral("off")).toString();
    bool anime4kKnown = false;
    for (const anime4k::ModeInfo& mode : anime4k::availableModes()) {
        if (mode.id == anime4kId) {
            anime4kKnown = true;
            break;
        }
    }
    if (anime4kKnown) {
        const anime4k::Mode mode = anime4k::modeForId(anime4kId);
        const QString chainId = anime4k::modeId(mode);
        if (anime4kMode_ != mode || anime4kChainId_ != chainId) {
            anime4kMode_ = mode;
            anime4kChainId_ = chainId;
            if (session_ != nullptr) session_->setAnime4KChain(chainId);
            publishAnime4K();
        }
    }

    const QString previousPreset = animeJanaiPresetId_;
    const native::Backend previousBackend = animeJanaiBackend_;
    const QString preset = settings.value(kAnimeJanaiPresetSettingKey).toString();
    const auto backend = native::backendFromName(
        settings.value(kAnimeJanaiBackendSettingKey).toString());
    const bool changed = preset != previousPreset ||
                         (backend.has_value() && *backend != previousBackend);
    if (backend.has_value()) animeJanaiBackend_ = *backend;
    animeJanaiPresetId_ = preset;
    publishAnimeJanai();
    rebuildEnhancementMenu();

    const bool nativeWasRequested = nativePlaybackRequested_;
    nativePlaybackRequested_ = native::loadNativePreferences().enabled;
    refreshNativeInputs();
    if (nativeWasRequested && !nativePlaybackRequested_) {
        nativeRequestAnnounced_ = false;
        nativeRequestContext_.reset();
        emit nativeAdmissionCancelled();
        if (session_ != nullptr) session_->turnNativeOff();
    }
    // Components can install missing dependencies without changing the request.
    const bool retryUnavailable = animeJanaiPlan_.enabled && !animeJanaiPlan_.available &&
                                  animeJanaiPlanGeneration_ == animeJanaiSequence_;
    if ((changed || retryUnavailable) &&
        (!previousPreset.isEmpty() || !animeJanaiPresetId_.isEmpty())) {
        applyAnimeJanaiRequest(animeJanaiPresetId_);
    }
}

void PlayerController::onNativeActionRequested(native::menu::Action action, const QString& value) {
    applyNativeRequest(action, value);
}

void PlayerController::onNativeStatusChanged(const native::NativeStatusView& status) {
    if (closed_ || qobject_cast<VideoSession*>(sender()) != session_ || session_ == nullptr) return;
    native::menu::Inputs inputs = nativeInputs_;
    inputs.status = status;
    inputs.availability = native::currentNativeAvailability(observedFactoryOf(session_));
    nativeInputs_ = inputs;
    publishNativeHud();
    // The observation may be the first one to carry this source and device. The
    // announcement dedups, so a repeated pump costs nothing.
    announceNativeRequest();
}

void PlayerController::syncNativeStatusPolling(bool requested) {
    if (session_ == nullptr) return;
    // The private filter belongs only to a pinned Native runtime. In
    // particular, the Standard AnimeJaNai fork has its own filter ABI and must
    // never be queried for the private status property.
    const native::RuntimeSelection& runtime = native::currentSelection();
    const bool nativeModule = runtime.loaded && runtime.kind == native::BundleKind::Native;
    session_->setNativeStatusPolling(requested && engineReady_ && nativeModule);
}

void PlayerController::applyNativeRequest(native::menu::Action action, const QString& value) {
    if (action == native::menu::Action::None) return;

    native::NativePreferencePatch patch;
    switch (action) {
        case native::menu::Action::TurnOff:
            patch.enabled = false;
            break;
        case native::menu::Action::TurnOn:
            patch.enabled = true;
            break;
        case native::menu::Action::SetBackend: {
            const std::optional<native::Backend> backend = native::backendFromName(value);
            if (!backend.has_value()) return;
            patch.enabled = true;
            patch.backend = *backend;
            break;
        }
        case native::menu::Action::SetRecipe:
            if (value.isEmpty()) return;
            patch.enabled = true;
            patch.recipeId = value;
            break;
        case native::menu::Action::None:
            return;
    }

    QString reason;
    if (!native::saveNativePreferences(patch, &reason)) {
        // A refused save is reported as itself; nothing was changed.
        flashStatus(tr("Could not save the native preference: %1").arg(reason));
        return;
    }
    nativePlaybackRequested_ = action != native::menu::Action::TurnOff;
    refreshNativeInputs();
    rebuildEnhancementMenu();

    if (action == native::menu::Action::TurnOff) {
        // Off never touches the Anime4K chain, the stream or the media. It does
        // stop native processing on the session and retires any assembly the
        // previous request had started.
        nativeRequestAnnounced_ = false;
        nativeRequestContext_.reset();
        emit nativeAdmissionCancelled();
        if (session_ != nullptr) session_->turnNativeOff();
        flashStatus(tr("Native processing off · Original picture"));
        return;
    }

    // A requested change is announced to the assembly layer, which owns the
    // model store, the measured build identity and the runtime backends. The
    // announcement carries only facts this session actually observed.
    announceNativeRequest();
}

bool PlayerController::nativeRequestEnabled() const {
    return nativePlaybackRequested_ && native::loadNativePreferences().enabled;
}

std::optional<native::NativePreparationContext> PlayerController::nativePreparationContext() const {
    if (session_ == nullptr) return std::nullopt;
    return session_->nativePreparationContext();
}

void PlayerController::armNativeProbe(native::Backend backend) {
    if (session_ != nullptr) session_->armNativeProbe(backend);
}

void PlayerController::announceNativeRequest() {
    if (closed_ || !nativePlaybackRequested_ || session_ == nullptr) return;
    const native::RuntimeSelection& runtime = native::currentSelection();
    if (!runtime.loaded || runtime.kind != native::BundleKind::Native) return;
    const native::NativePreferences preferences = native::loadNativePreferences();
    if (!preferences.enabled || preferences.recipeId.isEmpty()) return;
    // The probe binding is what makes an observation possible at all, and it is
    // armed only here - on an explicit request that names a recipe, never at
    // startup. It is idempotent and carries no admission.
    session_->armNativeProbe(preferences.backend);
    const std::optional<native::NativePreparationContext> context =
        session_->nativePreparationContext();
    if (!context.has_value()) {
        // Nothing was observed on this session yet; the request waits for the
        // next observation rather than announcing a source and device nobody read.
        return;
    }
    native::SelectionState selection;
    selection.recipeId = preferences.recipeId;
    selection.backend = preferences.backend;
    // Only a NEW selection or a NEW observation reissues the request; the pump
    // republishes the same facts and must not spin the assembly layer.
    if (nativeRequestAnnounced_ && nativeRequestSelection_ == selection &&
        nativeRequestContext_.has_value() &&
        native::sourceGeometryEqual(nativeRequestContext_->source, context->source) &&
        native::deviceIdentityEqual(nativeRequestContext_->device, context->device) &&
        nativeRequestContext_->modulePath == context->modulePath) {
        return;
    }
    if (nativeRequestAnnounced_) {
        // The selection or the owning session moved: the generation that was in
        // flight describes a request that is no longer the one wanted.
        emit nativeAdmissionCancelled();
    }
    nativeRequestAnnounced_ = true;
    nativeRequestSelection_ = selection;
    nativeRequestContext_ = *context;
    emit nativeAdmissionRequested(selection, *context);
}

void PlayerController::submitNativeAdmission(const native::PreparedAdmissionRequest& request) {
    if (session_ == nullptr) {
        reportNativeProcessingFailure(QStringLiteral("native.core.absent"),
                                      tr("No playback core can take a native admission."));
        return;
    }
    // The epoch is the session's own observed source epoch, which the assembly
    // layer cannot know. A request stamped for another source is refused by the
    // controller rather than applied here.
    native::PreparedAdmissionRequest stamped = request;
    stamped.epoch = session_->nativePlaybackStatus().sourceEpoch;
    // The session has to read its own filter back for the admission to be
    // acknowledged: a submitted command is not an observation.
    session_->setNativeStatusPolling(true);
    session_->requestNativeAdmission(stamped);
}

void PlayerController::reportNativeProcessingFailure(const QString& reason) {
    reportNativeProcessingFailure(QString(), reason);
}

void PlayerController::reportNativeProcessingFailure(const QString& reasonKey,
                                                     const QString& reason) {
    showProcessingFailure(reasonKey, reason);
}

bool PlayerController::nativeEffectiveActive() const {
    if (session_ == nullptr) return false;
    // A confirmed admission or an effective plan. A refused REPLACEMENT leaves
    // the previous plan in force, so "the Original picture" would be a lie here.
    const native::NativePlaybackController::Status& status = session_->nativePlaybackStatus();
    return status.confirmed || status.effective.has_value();
}

QString PlayerController::processingFailureMessage(const QString& reason) const {
    const QString base = reason.trimmed().isEmpty() ? tr("Native processing failed.")
                                                    : reason.trimmed();
    return nativeEffectiveActive() ? tr("%1 Keeping the current picture.").arg(base)
                                   : tr("%1 Staying on the Original picture.").arg(base);
}

void PlayerController::onNativePlaybackStatusChanged(
    const native::NativePlaybackController::Status& status) {
    Q_UNUSED(status);
    if (closed_ || qobject_cast<VideoSession*>(sender()) != session_ || session_ == nullptr) return;
    // This controller keeps no second core and no PiP hand-off, so a playback
    // status change only re-words an open remedy row and refreshes the HUD.
    if (processingActive_) {
        processingMessage_ = processingFailureMessage(lastProcessingReason_);
        statusText_ = processingMessage_;
        publishStatus();
    }
    publishNativeHud();
}

void PlayerController::onNativeProcessingFailure(const QString& reasonKey,
                                                 const QString& reason) {
    if (closed_ || qobject_cast<VideoSession*>(sender()) != session_ || session_ == nullptr) return;
    if (nativeFailureIsBenign(reasonKey)) return;
    reportNativeProcessingFailure(reasonKey, reason);
}

void PlayerController::requestNativeReferenceFrame(int frameIndex) {
    if (closed_) return;
    if (nativeReferencePending_ && nativeReferenceSession_.isNull()) {
        // The session that was asked is gone; its reply can never arrive.
        nativeReferencePending_ = false;
    }
    if (nativeReferencePending_) {
        emit nativeReferenceFrameFailed(
            QStringLiteral("native.reference.inFlight"),
            tr("A reference frame capture is already in flight."), frameIndex);
        return;
    }
    if (session_ == nullptr) {
        emit nativeReferenceFrameFailed(QStringLiteral("native.reference.noCore"),
                                        tr("No playback core can capture a reference frame."),
                                        frameIndex);
        return;
    }
    // The epoch and the index guard a reply that arrives after the source moved
    // or after this controller abandoned the request.
    nativeReferenceSession_ = session_;
    nativeReferenceSourceEpoch_ = session_->sourceEpoch();
    nativeReferenceFrameIndex_ = frameIndex;
    nativeReferencePending_ = true;
    session_->requestNativeReferenceFrame(frameIndex);
}

void PlayerController::onNativeReferenceFrameReady(const native::FrameFixture& frame,
                                                   quint64 sourceEpoch, double framePosition,
                                                   int frameIndex,
                                                   std::optional<native::Rational> frameFps) {
    if (closed_ || !nativeReferencePending_ ||
        sender() != nativeReferenceSession_.data()) {
        return;
    }
    if (frameIndex != nativeReferenceFrameIndex_) {
        // A frame for a request this controller has moved past.
        return;
    }
    if (sourceEpoch != 0 && nativeReferenceSourceEpoch_ != 0 &&
        sourceEpoch != nativeReferenceSourceEpoch_) {
        // If this pending request still belongs to the session's current source,
        // this is a delayed reply from an older request and must not retire it.
        // Otherwise the reply confirms that the pending request became stale.
        if (nativeReferenceSession_ != nullptr &&
            nativeReferenceSession_->sourceEpoch() != nativeReferenceSourceEpoch_) {
            nativeReferencePending_ = false;
            emit nativeReferenceFrameFailed(
                QStringLiteral("native.reference.staleSource"),
                tr("The source changed while the capture was in flight."), frameIndex);
        }
        return;
    }
    nativeReferencePending_ = false;
    emit nativeReferenceFrameReady(frame, sourceEpoch, framePosition, frameIndex, frameFps);
}

void PlayerController::onNativeReferenceFrameFailed(const QString& reasonKey,
                                                    const QString& reason, int frameIndex) {
    if (closed_ || !nativeReferencePending_ ||
        sender() != nativeReferenceSession_.data()) {
        return;
    }
    if (frameIndex != nativeReferenceFrameIndex_) return;
    nativeReferencePending_ = false;
    emit nativeReferenceFrameFailed(reasonKey, reason, frameIndex);
}

void PlayerController::cancelPendingReferenceCapture(const QString& reasonKey,
                                                     const QString& reason) {
    if (!nativeReferencePending_ || nativeReferenceSession_ == nullptr ||
        nativeReferenceSession_->sourceEpoch() == nativeReferenceSourceEpoch_) {
        return;
    }
    nativeReferencePending_ = false;
    emit nativeReferenceFrameFailed(reasonKey, reason, nativeReferenceFrameIndex_);
}

void PlayerController::publishNativeHud() {
    nativeStatusLines_ = native::menu::statusLines(nativeInputs_);
    emit nativeHudChanged();
}

// ── processing status / remedies ────────────────────────────────────────────

void PlayerController::showProcessingStatus(const QString& reason) {
    if (closed_) return;
    // The generic native-status line carries no remedies, so a status that is
    // not a failure can never be mistaken for one.
    if (processingActive_ || errorActive_) {
        statusText_ = reason;
        publishStatus();
        return;
    }
    flashStatus(reason);
}

void PlayerController::showProcessingFailure(const QString& reasonKey, const QString& reason) {
    if (closed_) return;
    lastProcessingReasonKey_ = reasonKey;
    lastProcessingReason_ = reason.trimmed().isEmpty() ? reasonKey.trimmed() : reason.trimmed();
    processingActive_ = true;
    processingMessage_ = processingFailureMessage(lastProcessingReason_);
    statusText_ = processingMessage_;
    statusMode_ = StatusMode::Processing;
    rebuildRemedyMenu();
    publishStatus();
    if (reasonKey == QLatin1String("animejanai.installation.recordMissing")) {
        if (!componentSetupRequestedOnce_) {
            componentSetupRequestedOnce_ = true;
            emit componentSetupRequested();
        }
    }
}

void PlayerController::retireProcessingRow() {
    processingActive_ = false;
    lastProcessingReasonKey_.clear();
    lastProcessingReason_.clear();
    processingMessage_.clear();
    // An outstanding media error keeps the panel up and keeps its own chip.
    if (!errorActive_) {
        statusText_.clear();
        statusMode_ = StatusMode::None;
    }
    rebuildRemedyMenu();
    publishStatus();
}

void PlayerController::onProcessingPrepareRequested() {
    if (closed_) return;
    // Nothing happens here beyond asking the host to open its existing Processing
    // tab: no URL, no quality probe, no stream state, and no preparation is
    // started by opening anything.
    emit processingSettingsRequested(shell::ProcessingSettingsIntent::Prepare);
}

void PlayerController::onProcessingProvidersRequested() {
    if (closed_) return;
    // Only brings the existing validated provider choice into view; Save
    // persists it exactly as it does today.
    emit processingSettingsRequested(shell::ProcessingSettingsIntent::Providers);
}

void PlayerController::onProcessingOriginalRequested() {
    if (closed_) return;
    // The existing native TurnOff path: it saves the request, stops native work
    // on the session, retires the assembly and leaves the media and the Anime4K
    // chain untouched. Only this row is retired; a media error keeps its chip.
    const bool animeJanaiFailure = lastProcessingReasonKey_.startsWith(QLatin1String("animejanai."));
    retireProcessingRow();
    if (animeJanaiFailure) {
        applyAnimeJanaiRequest(QString());
        return;
    }
    applyNativeRequest(native::menu::Action::TurnOff, QString());
}

// ── Anime4K ─────────────────────────────────────────────────────────────────

void PlayerController::applyAnime4KPreset(const QString& presetId) {
    if (closed_) return;
    // Guarded, because this entry point is fed by stored bindings: a key can
    // outlive the preset it was bound to. An unknown id is refused rather than
    // mapped onto Off.
    bool known = false;
    for (const anime4k::ModeInfo& info : anime4k::availableModes()) {
        if (info.id == presetId) {
            known = true;
            break;
        }
    }
    if (!known) return;
    const anime4k::Mode mode = anime4k::modeForId(presetId);
    anime4kMode_ = mode;
    anime4kChainId_ = anime4k::modeId(mode);
    if (session_ != nullptr) session_->setAnime4KChain(anime4kChainId_);
    // The shader chain is a rendering choice, not per-episode state: it is the
    // one player setting that outlives a session.
    QSettings().setValue(QStringLiteral("player/anime4k"), anime4kChainId_);
    QString label = anime4k::displayLabel(mode);
    if (label.isEmpty()) label = anime4k::modeId(mode);
    flashStatus(tr("Anime4K: %1").arg(label));
    publishAnime4K();
    rebuildEnhancementMenu();
}

QString PlayerController::enhancementChipText() const {
    if (anime4kMode_ == anime4k::Mode::Off) return QString();
    QString label = anime4k::displayLabel(anime4kMode_);
    if (label.isEmpty()) label = anime4k::modeId(anime4kMode_);
    if (sourceWidth_ > 0 && sourceHeight_ > 0) {
        return tr("Anime4K · %1 · %2×%3").arg(label).arg(sourceWidth_).arg(sourceHeight_);
    }
    return tr("Anime4K · %1").arg(label);
}

QString PlayerController::enhancementChipTooltip() const {
    if (anime4kMode_ == anime4k::Mode::Off) return QString();
    QString label = anime4k::displayLabel(anime4kMode_);
    if (label.isEmpty()) label = anime4k::modeId(anime4kMode_);
    return label;
}

void PlayerController::publishAnime4K() {
    emit anime4kChanged();
}

// ── AnimeJaNai ──────────────────────────────────────────────────────────────

void PlayerController::applyAnimeJanaiRequest(const QString& id) {
    if (closed_) return;
    const QString presetId = id;
    animeJanaiWaitingForSourceContext_ = false;
    animeJanaiPresetId_ = id;
    QSettings settings;
    settings.setValue(kAnimeJanaiPresetSettingKey, id);
    settings.setValue(kAnimeJanaiBackendSettingKey, animeJanaiBackendName(animeJanaiBackend_));
    ++animeJanaiSequence_;
    if (animeJanaiCancellation_) animeJanaiCancellation_->store(true);
    const quint64 sequence = animeJanaiSequence_;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    animeJanaiCancellation_ = cancelled;
    // The slot is created and assigned on the GUI thread; the worker captures
    // only the shared slot, never a raw controller address.
    if (!workerSlot_) workerSlot_ = std::make_shared<WorkerSlot>();
    workerSlot_->controller = this;
    animeJanaiStatus_ = id.isEmpty() ? tr("Turning off…") : tr("Preparing…");
    animeJanaiNote_.clear();
    publishAnimeJanai();
    const QString requestedPreset = presetId;
    const native::Backend backend = animeJanaiBackend_;
    const native::AnimeJanaiSourceContext source =
        session_ != nullptr ? session_->animeJanaiSourceContext()
                            : native::AnimeJanaiSourceContext{};
    const uint64_t sourceEpoch = session_ != nullptr ? session_->sourceEpoch() : 0;
    animeJanaiPendingPreparationSourceEpoch_ = sourceEpoch;
    const std::shared_ptr<WorkerSlot> slot = workerSlot_;
    if (slot == nullptr) return;
    QThreadPool::globalInstance()->start(QRunnable::create(
        [slot, cancelled, sequence, requestedPreset, backend, source, sourceEpoch] {
            native::AnimeJanaiPlan plan = native::prepareAnimeJanaiPlan(
                requestedPreset, backend, [cancelled] { return cancelled->load(); }, source,
                [](const QString& candidate, QString* reason) {
                    return anime4k::filterShaderProgramForCandidate(candidate, QString(), reason);
                });
            if (cancelled->load()) return;
            QObject* appRoot = QCoreApplication::instance();
            if (appRoot == nullptr) return;
            QMetaObject::invokeMethod(
                appRoot,
                [slot, cancelled, sequence, sourceEpoch, source, plan = std::move(plan)]() mutable {
                    if (cancelled->load()) return;
                    PlayerController* controller =
                        slot != nullptr ? slot->controller.data() : nullptr;
                    if (controller == nullptr || controller->closed_ ||
                        controller->animeJanaiSequence_ != sequence) {
                        return;
                    }
                    if (controller->session_ != nullptr &&
                        controller->session_->sourceEpoch() != sourceEpoch) {
                        return;
                    }
                    controller->animeJanaiPreparationSourceEpoch_ = sourceEpoch;
                    controller->animeJanaiLastPreparedSource_ = source;
                    controller->animeJanaiPendingPreparationSourceEpoch_ = 0;
                    controller->animeJanaiWaitingForSourceContext_ = plan.needsSourceContext;
                    if (plan.needsSourceContext) {
                        controller->animeJanaiPlan_ = plan;
                        controller->animeJanaiPlanGeneration_ = sequence;
                        controller->animeJanaiStatus_ = PlayerController::tr(
                            "Waiting for source metadata…");
                        controller->publishAnimeJanai();
                        return;
                    }
                    if (controller->lastProcessingReasonKey_.startsWith(
                            QLatin1String("animejanai.")) &&
                        (!plan.enabled || plan.available)) {
                        controller->retireProcessingRow();
                    }
                    if (cancelled->load()) return;
                    controller->animeJanaiPlan_ = plan;
                    controller->animeJanaiPlanGeneration_ = sequence;
                    if (controller->session_ != nullptr) {
                        controller->session_->setAnimeJanaiPlan(plan);
                    }
                    controller->animeJanaiStatus_ =
                        !plan.available
                            ? PlayerController::tr("Unavailable · %1").arg(plan.reason)
                            : (controller->session_ != nullptr
                                   ? controller->session_->animeJanaiStatus()
                                   : PlayerController::tr("Off"));
                    controller->animeJanaiNote_ =
                        controller->session_ != nullptr
                            ? controller->session_->animeJanaiPlanNote()
                            : QString();
                    controller->publishAnimeJanai();
                    controller->rebuildEnhancementMenu();
                    if (cancelled->load()) return;
                    if (plan.enabled && !plan.available) {
                        controller->showProcessingFailure(plan.reasonKey, plan.reason);
                    }
                },
                Qt::QueuedConnection);
        }));
}

void PlayerController::onAnimeJanaiSourceContextChanged() {
    if (closed_ || animeJanaiPresetId_.isEmpty() || session_ == nullptr) return;
    const uint64_t epoch = session_->sourceEpoch();
    if (epoch == 0) return;
    const native::AnimeJanaiSourceContext source = session_->animeJanaiSourceContext();
    const bool hasDimensions = source.width > 0 && source.height > 0;
    if (!hasDimensions) return;
    const bool sourceChanged = animeJanaiPreparationSourceEpoch_ != 0 &&
                               animeJanaiPreparationSourceEpoch_ != epoch;
    const bool contextChanged = source.width != animeJanaiLastPreparedSource_.width ||
                                source.height != animeJanaiLastPreparedSource_.height ||
                                source.fps != animeJanaiLastPreparedSource_.fps;
    if ((sourceChanged || (animeJanaiWaitingForSourceContext_ && contextChanged)) &&
        animeJanaiPendingPreparationSourceEpoch_ != epoch) {
        applyAnimeJanaiRequest(animeJanaiPresetId_);
    }
}

void PlayerController::applyAnimeJanaiBackend(const QString& backendName) {
    if (closed_) return;
    const auto backend = native::backendFromName(backendName);
    if (!backend) return;
    if (!native::describeAnimeJanaiInstallation(*backend, false).installed) {
        flashStatus(tr("The %1 backend is not installed; install it in Settings › Components.")
                        .arg(backendName));
        return;
    }
    animeJanaiBackend_ = *backend;
    QSettings().setValue(kAnimeJanaiBackendSettingKey, animeJanaiBackendName(*backend));
    publishAnimeJanai();
    rebuildEnhancementMenu();
    if (!animeJanaiPresetId_.isEmpty()) applyAnimeJanaiRequest(animeJanaiPresetId_);
}

void PlayerController::onAnimeJanaiStatusChanged(const QString& status) {
    auto* owner = qobject_cast<VideoSession*>(sender());
    if (closed_ || owner == nullptr || owner != session_) return;
    syncAnimeJanaiEngineBuildState(owner);
    animeJanaiStatus_ = status;
    animeJanaiNote_ = owner->animeJanaiPlanNote();
    publishAnimeJanai();
}

void PlayerController::onAnimeJanaiFailure(const QString& reason) {
    if (closed_ || qobject_cast<VideoSession*>(sender()) != session_ || session_ == nullptr) return;
    showProcessingFailure(QStringLiteral("animejanai.failure"), reason);
}

void PlayerController::onAnimeJanaiEngineBuildChanged(bool active, bool succeeded,
                                                      const QString& detail) {
    handleAnimeJanaiEngineBuildChanged(qobject_cast<VideoSession*>(sender()), active, succeeded,
                                       detail);
}

void PlayerController::syncAnimeJanaiEngineBuildState(VideoSession* owner) {
    if (closed_) return;
    if (owner == nullptr) owner = session_;
    if (owner == nullptr || owner != session_) return;
    if (owner->animeJanaiEngineBuildActive()) {
        handleAnimeJanaiEngineBuildChanged(owner, true, false,
                                           owner->animeJanaiEngineBuildDetail());
    }
}

QString PlayerController::animeJanaiBuildDetailText() const {
    // Only the wrapper around the filter's words is this controller's to
    // translate. The raw detail is re-emitted exactly as the build reported it,
    // and the preset name is the catalogue's rendering of this operation's
    // retained id, already re-derived in the installed language by a restatement.
    QString text = animeJanaiBuildDetailRaw_;
    QStringList facts;
    if (!animeJanaiBuildPresetLabel_.isEmpty()) {
        facts << tr("Preset: %1").arg(animeJanaiBuildPresetLabel_);
    }
    if (animeJanaiBuildWidth_ > 0 && animeJanaiBuildHeight_ > 0) {
        facts << tr("Source: %1 × %2").arg(animeJanaiBuildWidth_).arg(animeJanaiBuildHeight_);
    }
    if (!facts.isEmpty()) {
        if (!text.isEmpty()) text += QStringLiteral(" · ");
        text += facts.join(QStringLiteral(" · "));
    }
    return text;
}

void PlayerController::restateAnimeJanaiBuildLanguage() {
    if (closed_ || !enginePreparationActive_) return;
    // A newer operation owns the window: its own caller words it, and restating
    // this build's words would overwrite text that is no longer theirs to say.
    if (animeJanaiBuildGeneration_ != enginePreparationGeneration_) return;
    animeJanaiBuildPresetLabel_ =
        animeJanaiPresetLabel(animeJanaiBuildPresetId_, animeJanaiBuildPresetLabel_);
    // Text only: the setters never begin or end a run, move the frame hold,
    // issue a command, bump a generation, restart the elapsed timer or cancel
    // anything, and the stage keeps the index and count the build supplied.
    const bool rtx = animeJanaiPlan_.backend == native::Backend::TensorRTRTX;
    retitleEnginePreparation(rtx ? tr("Building TensorRT for RTX engine")
                                 : tr("Building TensorRT engine"));
    updateEnginePreparation(animeJanaiBuildStageIndex_, animeJanaiBuildStageCount_,
                            rtx ? tr("TensorRT for RTX compilation")
                                : tr("TensorRT compilation"),
                            animeJanaiBuildDetailText());
}

void PlayerController::handleAnimeJanaiEngineBuildChanged(VideoSession* owner, bool active,
                                                          bool succeeded, const QString& detail) {
    if (closed_ || owner == nullptr || owner != session_) return;
    if (active) {
        owner->setEngineFrameHold(true);
        if (enginePreparationGeneration_ == 0 ||
            animeJanaiBuildGeneration_ != enginePreparationGeneration_) {
            const native::AnimeJanaiPlan plan = owner->animeJanaiPlan();
            beginEnginePreparation(plan.backend == native::Backend::TensorRTRTX
                                       ? tr("Building TensorRT for RTX engine")
                                       : tr("Building TensorRT engine"));
            animeJanaiBuildGeneration_ = enginePreparationGeneration_;
            // The operation's preset is captured as its catalogue identity,
            // once, when the operation starts. The label recorded beside it is
            // only that identity's rendering under the current language and the
            // fallback for a catalogue that no longer lists it.
            animeJanaiBuildPresetId_ = plan.presetId;
            animeJanaiBuildPresetLabel_ = animeJanaiPresetLabel(plan.presetId, plan.presetId);
        }
        // The build's words are retained as typed state, not as the text the
        // window happens to show: a language switch restates the app-owned
        // wrapper from these facts and re-emits the filter's detail verbatim.
        animeJanaiBuildDetailRaw_ = detail;
        animeJanaiBuildWidth_ = owner->videoWidth();
        animeJanaiBuildHeight_ = owner->videoHeight();
        animeJanaiBuildStageIndex_ = -1;
        animeJanaiBuildStageCount_ = 0;
        // Re-assert the pause hold on the session that is actually building.
        if (!enginePreparationActive_) beginEnginePreparation(preparationTitle_);
        updateEnginePreparation(
            animeJanaiBuildStageIndex_, animeJanaiBuildStageCount_,
            animeJanaiPlan_.backend == native::Backend::TensorRTRTX
                ? tr("TensorRT for RTX compilation")
                : tr("TensorRT compilation"),
            animeJanaiBuildDetailText());
        return;
    }
    if (!owner->animeJanaiEngineBuildActive()) owner->setEngineFrameHold(false);
    if (animeJanaiBuildGeneration_ == 0) {
        // A build this controller never tracked: report the fact, own no window.
        if (!detail.isEmpty()) flashStatus(detail, !succeeded);
        return;
    }
    animeJanaiBuildDetailRaw_.clear();
    animeJanaiBuildPresetId_.clear();
    animeJanaiBuildPresetLabel_.clear();
    animeJanaiBuildWidth_ = 0;
    animeJanaiBuildHeight_ = 0;
    animeJanaiBuildStageIndex_ = -1;
    animeJanaiBuildStageCount_ = 0;
    // A superseded build reports itself but cannot end the operation owning the
    // window now: whatever superseded it already released the hold it held.
    if (animeJanaiBuildGeneration_ != enginePreparationGeneration_) {
        animeJanaiBuildGeneration_ = 0;
        if (!detail.isEmpty()) flashStatus(detail, !succeeded);
        return;
    }
    animeJanaiBuildGeneration_ = 0;
    if (enginePreparationActive_) {
        endEnginePreparation(succeeded, detail);
    } else if (!detail.isEmpty()) {
        flashStatus(detail, !succeeded);
    }
}

QString PlayerController::animeJanaiBackendName(native::Backend backend) {
    return native::stableName(backend);
}

QString PlayerController::animeJanaiBackendName() const {
    return animeJanaiBackendName(animeJanaiBackend_);
}

QString PlayerController::animeJanaiChipText() const {
    const QString& id = animeJanaiPresetId_;
    if (id.isEmpty()) return QString();
    // Three real backend values, so the chip names the one actually requested
    // instead of folding RTX into the classic TRT label.
    const std::optional<native::Backend> backendValue = native::backendFromName(
        animeJanaiBackendName(animeJanaiBackend_));
    const QString backendShort =
        backendValue == native::Backend::DirectML
            ? QStringLiteral("DML")
            : (backendValue == native::Backend::TensorRTRTX ? QStringLiteral("TRT-RTX")
                                                            : QStringLiteral("TRT"));
    QString presetLabel = id;
    bool presetInCatalogue = false;
    for (const native::AnimeJanaiPreset& preset : native::animeJanaiPresets()) {
        if (preset.id.compare(id, Qt::CaseInsensitive) == 0) {
            presetLabel = preset.label;
            presetInCatalogue = true;
            break;
        }
    }
    // A downloaded upscaler is a selection of its own and is named by its own
    // catalogue row, not reported as an unknown preset.
    if (!presetInCatalogue && id.startsWith(QLatin1String("upscaler:"))) {
        if (const std::optional<native::UpscalerModel> model =
                native::upscalerForId(id.mid(9));
            model.has_value()) {
            presetLabel = QStringLiteral("%1 · %2×").arg(model->displayName).arg(model->scale);
            presetInCatalogue = true;
        }
    }
    if (!presetInCatalogue) {
        // The retained selection outlived its graph. It is still the selection,
        // so the chip says the catalogue cannot resolve it instead of falling
        // back to Off or to a model the viewer never chose.
        return QStringLiteral("%1 · %2").arg(backendShort, tr("Unavailable · %1").arg(id));
    }
    QString visibleStatus = animeJanaiStatus_;
    if (visibleStatus.startsWith(QLatin1String("Configured;"))) {
        if (id.startsWith(QLatin1String("rife:"), Qt::CaseInsensitive)) {
            const double srcFps = streamFramesPerSecond_ > 0.0 ? streamFramesPerSecond_ : 23.98;
            const double outFps = srcFps * 2.0;
            if (sourceHeight_ > 0) {
                visibleStatus = QStringLiteral("%1 · %2 · %3p · %4 → %5 FPS")
                                    .arg(backendShort, presetLabel)
                                    .arg(sourceHeight_)
                                    .arg(srcFps, 0, 'f', 2)
                                    .arg(outFps, 0, 'f', 2);
            } else {
                visibleStatus =
                    QStringLiteral("%1 · %2 · %3").arg(backendShort, presetLabel, tr("Preparing…"));
            }
        } else if (sourceHeight_ > 0) {
            const int outH = sourceHeight_ * 2;
            const int outW = (sourceWidth_ > 0 ? sourceWidth_ : (sourceHeight_ * 16 / 9)) * 2;
            visibleStatus = QStringLiteral("%1 · %2 · %3p → %4p (%5×%4)")
                                .arg(backendShort, presetLabel)
                                .arg(sourceHeight_)
                                .arg(outH)
                                .arg(outW);
        } else {
            visibleStatus =
                QStringLiteral("%1 · %2 · %3").arg(backendShort, presetLabel, tr("Preparing…"));
        }
    }
    const bool alreadyPrefixed =
        visibleStatus.startsWith(QLatin1String("TRT-RTX ")) ||
        visibleStatus.startsWith(QLatin1String("TRT ")) ||
        visibleStatus.startsWith(QLatin1String("RTX ")) ||
        visibleStatus.startsWith(QLatin1String("DML ")) ||
        visibleStatus.startsWith(QLatin1String("AnimeJaNai"), Qt::CaseInsensitive);
    return alreadyPrefixed ? visibleStatus
                           : QStringLiteral("%1 · %2").arg(backendShort, visibleStatus);
}

QString PlayerController::animeJanaiChipTooltip() const {
    if (animeJanaiPresetId_.isEmpty()) return QString();
    QStringList parts;
    if (!animeJanaiNote_.isEmpty()) parts << animeJanaiNote_;
    if (!animeJanaiStatus_.isEmpty() && animeJanaiStatus_ != animeJanaiNote_) {
        parts << animeJanaiStatus_;
    }
    return parts.join(QStringLiteral("\n\n"));
}

void PlayerController::publishAnimeJanai() {
    emit animeJanaiChanged();
}

// ── engine preparation ──────────────────────────────────────────────────────

void PlayerController::beginEnginePreparation(const QString& title) {
    if (closed_) return;
    // Supersedes any earlier operation, including one never completed.
    ++enginePreparationGeneration_;
    enginePreparationActive_ = true;
    enginePreparationHeldIntent_ = session_ != nullptr ? session_->playbackPauseIntent() : false;
    enginePreparationSourceEpoch_ = session_ != nullptr ? session_->sourceEpoch() : 0;
    if (session_ != nullptr) session_->setEnginePreparationPaused(true);
    preparationTitle_ = title;
    preparationPhase_.clear();
    preparationDetail_.clear();
    preparationSummary_.clear();
    preparationStageIndex_ = -1;
    preparationStageCount_ = 0;
    enginePreparationCancelPending_ = false;
    enginePreparationVisible_ = true;
    // The window is created once by the controller host next to the player
    // window; this path only presents it.
    if (preparationWindow_) preparationWindow_->present();
    publishPreparation();
}

void PlayerController::updateEnginePreparation(int stageIndex, int stageCount,
                                                const QString& phase, const QString& detail) {
    if (closed_ || !enginePreparationActive_) return;
    preparationStageIndex_ = stageIndex;
    preparationStageCount_ = stageCount;
    preparationPhase_ = phase;
    preparationDetail_ = detail;
    publishPreparation();
}

void PlayerController::endEnginePreparation(bool ok, const QString& summary) {
    if (closed_ || !enginePreparationActive_) return;
    enginePreparationActive_ = false;
    preparationSummary_ = summary;
    enginePreparationVisible_ = false;
    enginePreparationCancelPending_ = false;
    enginePreparationSourceEpoch_ = 0;
    enginePreparationHeldIntent_ = false;
    if (session_ != nullptr) {
        // The terminal applies the latest viewer intent to the session that
        // still plays.
        session_->setEnginePreparationPaused(false, true);
    }
    publishPreparation();
    if (preparationWindow_) preparationWindow_->finish(ok);
    if (!summary.isEmpty()) flashStatus(summary, !ok);
}

void PlayerController::retitleEnginePreparation(const QString& title) {
    if (closed_ || !enginePreparationActive_) return;
    preparationTitle_ = title;
    publishPreparation();
}

void PlayerController::updateEnginePreparationSummary(const QString& summary) {
    if (closed_ || !enginePreparationActive_) return;
    preparationSummary_ = summary;
    publishPreparation();
}

void PlayerController::cancelEnginePreparation() {
    if (closed_ || !enginePreparationActive_) return;
    enginePreparationCancelPending_ = true;
    publishPreparation();
    if (session_ != nullptr && session_->animeJanaiEngineBuildActive()) {
        applyAnimeJanaiRequest(QString());
        return;
    }
    emit enginePreparationCancelRequested();
}

void PlayerController::restatePreparation() {
    if (closed_) return;
    publishPreparation();
}

void PlayerController::publishPreparation() {
    emit preparationChanged();
}

QString PlayerController::pauseHoldText() const {
    if (!enginePauseHold_) return QString();
    return tr("Preparing enhancement — playback paused");
}

}  // namespace quick
}  // namespace mplayer
