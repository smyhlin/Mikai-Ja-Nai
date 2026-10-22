// Native and AnimeJaNai processing orchestration, reference frame capture,
// engine-preparation overlay management, and Picture-in-Picture core handoff
// for PlayerDialog.

#include "PlayerDialog.hpp"

#include "PipFloat.hpp"
#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
#include "ShellWidgets.hpp"
#include "VideoSurface.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"

#include <QCoreApplication>
#include <QCursor>
#include <QDebug>
#include <QGuiApplication>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QScreen>
#include <QSettings>
#include <QThreadPool>
#include <QTimer>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

namespace mplayer {
namespace shell {

namespace {

constexpr int kPipDefaultWidth = 480;
constexpr int kPipDefaultHeight = 270;
constexpr int kStatusMs = 1900;

constexpr auto kAnimeJanaiPresetSettingKey = "player/animejanai/preset";
constexpr auto kAnimeJanaiBackendSettingKey = "player/animejanai/backend";

QString animeJanaiBackendName(native::Backend backend) {
    return backend == native::Backend::DirectML ? QStringLiteral("directml")
                                                 : QStringLiteral("tensorrt");
}
/// The catalogue's name for a preset identity in the language installed now, or
/// `fallback` when the catalogue no longer offers that identity. Only the
/// catalogue knows which names are app-owned words and which are user or model
/// text: deriving from the stable id re-renders the former and returns the
/// latter unchanged, instead of guessing from a previously rendered label.
QString animeJanaiPresetLabel(const QString& presetId, const QString& fallback) {
    if (!presetId.isEmpty()) {
        for (const native::AnimeJanaiPreset& preset : native::animeJanaiPresets()) {
            if (preset.id.compare(presetId, Qt::CaseInsensitive) == 0) return preset.label;
        }
    }
    return fallback;
}

}  // namespace

namespace {

/// The factory observation the core that owns playback currently holds, or
/// nullptr when that core has published no accepted status document for the
/// source it is playing. The pointer addresses the owning surface's own
/// published view - a closed, replaced or rebound source has already cleared
/// that view - so an old source's observation cannot be offered as the current
/// one's eligibility. It lives as long as the call that read it.
const native::NativeFactoryObservation* observedFactoryOf(const VideoSurface* owner) {
    if (owner == nullptr) return nullptr;
    const native::NativeStatusView& view = owner->nativeStatus();
    if (!view.snapshot.has_value() || !view.snapshot->factory.has_value()) return nullptr;
    return &*view.snapshot->factory;
}

}  // namespace

void PlayerDialog::pushNativeInputs() {
    if (shell_ == nullptr) {
        return;
    }
    // The four inputs the shell and its status HUD read, replaced as one: the
    // request, the producer availability, the runtime that was actually selected
    // and the filter's own status. None of them is invented here.
    native::menu::Inputs inputs;
    inputs.preferences = native::loadNativePreferences();
    // The availability is a pure projection of the factory observation the core
    // that OWNS PLAYBACK has actually published for the source it is playing -
    // the float's own observation while PiP is up, never the parked main core's.
    // A core with no accepted document for its current source passes nullptr, so
    // a closed or replaced source cannot keep the roles eligible. This is
    // eligibility to REQUEST, never a claim that a plan is prepared or running:
    // announceNativeRequest()'s observed-context gate is what authorizes a
    // request, and it remains separate from this field.
    VideoSurface* owner = playbackSurface();
    inputs.availability = native::currentNativeAvailability(observedFactoryOf(owner));
    inputs.runtime = native::currentSelection();
    if (owner != nullptr) {
        inputs.status = owner->nativeStatus();
    }
    shell_->setNativeMenuInputs(inputs);
    syncNativeStatusPolling(inputs.preferences.enabled);
}

void PlayerDialog::refreshStoredSettings() {
    loadPresetShortcuts();
    QSettings animeJanaiSettings;
    const QString preset = animeJanaiSettings.value(kAnimeJanaiPresetSettingKey).toString();
    const QString backendName = animeJanaiSettings.value(kAnimeJanaiBackendSettingKey).toString();
    const auto backend = native::backendFromName(backendName);
    const bool changed = preset != animeJanaiPresetId_ ||
                         (backend.has_value() && *backend != animeJanaiBackend_);
    animeJanaiPresetId_ = preset;
    if (backend.has_value()) animeJanaiBackend_ = *backend;
    shell_->setAnimeJanaiBackend(animeJanaiBackendName(animeJanaiBackend_));
    refreshNativeInputs();
    // Components can install missing dependencies without changing the request.
    const bool retryUnavailable = animeJanaiPlan_.enabled && !animeJanaiPlan_.available &&
                                  animeJanaiPlanGeneration_ == animeJanaiSequence_;
    if (changed || retryUnavailable) applyAnimeJanaiRequest(animeJanaiPresetId_);
}

void PlayerDialog::refreshNativeInputs() {
    pushNativeInputs();
    // A settings change writes the store and then asks for this refresh, so the
    // selection it may have changed is announced from here too. The announcement
    // dedups, and it returns early while the request is off. An ownership change
    // pushes the inputs only: announcing there would race the claim hand-off,
    // whose own settle step is what re-announces.
    announceNativeRequest();
}

void PlayerDialog::onNativeActionRequested(native::menu::Action action, const QString& value) {
    applyNativeRequest(action, value);
}

void PlayerDialog::onNativeStatusChanged(const native::NativeStatusView& status) {
    if (closed_ || shell_ == nullptr) {
        return;
    }
    // Only the core that owns playback speaks for the current source. The parked
    // core keeps polling its own filter while the float plays, and letting its
    // document reach the HUD (or the availability) would report the wrong core's
    // roles for the source the viewer is actually watching.
    VideoSurface* owner = playbackSurface();
    if (owner != nullptr && sender() != owner) {
        return;
    }
    // The reader published a genuine change: amend the inputs already held
    // rather than reading the store again, and let the shell re-render the HUD
    // from them. Polling is deliberately not re-armed here - the surface is the
    // one that stops on a terminal state or an absent filter, and a call here
    // would clear that halt.
    native::menu::Inputs inputs = shell_->nativeInputs();
    inputs.status = status;
    // A genuine probe observation is what moves the availability: eligibility
    // named from the factory this document carries, and named unavailable when
    // it carries none (unobserved, refused, or a source that just changed).
    inputs.availability = native::currentNativeAvailability(observedFactoryOf(owner));
    shell_->setNativeMenuInputs(inputs);
    // The observation may be the first one to carry this core's source and
    // device. The announcement dedups, so a repeated pump costs nothing.
    announceNativeRequest();
}

void PlayerDialog::syncNativeStatusPolling(bool requested) {
    if (surface_ == nullptr) {
        return;
    }
    // The private filter belongs only to a pinned Native runtime. In
    // particular, the Standard AnimeJaNai fork has its own filter ABI and must
    // never be queried for the private status property.
    const native::RuntimeSelection& runtime = native::currentSelection();
    const bool nativeModule = runtime.loaded && runtime.kind == native::BundleKind::Native;
    surface_->setNativeStatusPolling(requested && engineReady_ && nativeModule);
}

void PlayerDialog::applyNativeRequest(native::menu::Action action, const QString& value) {
    if (action == native::menu::Action::None) {
        return;
    }

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
            if (!backend.has_value()) {
                return;
            }
            patch.enabled = true;
            patch.backend = *backend;
            break;
        }
        case native::menu::Action::SetRecipe:
            if (value.isEmpty()) {
                return;
            }
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
    refreshNativeInputs();

    if (action == native::menu::Action::TurnOff) {
        // Off never touches the Anime4K chain, the stream or the media. It does
        // stop native processing on whichever core owns playback now, and it
        // retires any assembly the previous request had started.
        nativeHandoff_ = NativeHandoff::None;
        nativeHandoffFloat_.clear();
        nativeRequestAnnounced_ = false;
        nativeRequestContext_.reset();
        emit nativeAdmissionCancelled();
        if (VideoSurface* owner = playbackSurface()) {
            owner->turnNativeOff();
        }
        flashStatus(tr("Native processing off · Original picture"));
        return;
    }

    // A requested change is announced to the assembly layer, which owns the
    // model store, the measured build identity and the runtime backends. The
    // announcement carries only facts this core actually observed; it never
    // claims the chain is running, and nothing here touches the URL, the stream
    // or the media.
    announceNativeRequest();
}

void PlayerDialog::applyAnimeJanaiRequest(const QString& id) {
    const QString presetId = id;
    animeJanaiWaitingForSourceContext_ = false;
    animeJanaiPresetId_ = id;
    QSettings settings;
    settings.setValue(kAnimeJanaiPresetSettingKey, id);
    settings.setValue(kAnimeJanaiBackendSettingKey,
                      animeJanaiBackendName(animeJanaiBackend_));
    ++animeJanaiSequence_;
    if (animeJanaiCancellation_) animeJanaiCancellation_->store(true);
    const quint64 sequence = animeJanaiSequence_;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    animeJanaiCancellation_ = cancelled;
    shell_->setAnimeJanaiSelection(animeJanaiPresetId_,
                                   id.isEmpty() ? tr("Turning off…") : tr("Preparing…"));
    const QString requestedPreset = presetId;
    const native::Backend backend = animeJanaiBackend_;
    VideoSurface* preparationSurface = playbackSurface();
    if (preparationSurface == nullptr) preparationSurface = surface_;
    const native::AnimeJanaiSourceContext source =
        preparationSurface != nullptr ? preparationSurface->animeJanaiSourceContext()
                                      : native::AnimeJanaiSourceContext{};
    const uint64_t sourceEpoch = preparationSurface != nullptr
                                     ? preparationSurface->sourceEpoch() : 0;
    animeJanaiPendingPreparationSourceEpoch_ = sourceEpoch;
    // Raw owner: copying a QPointer on the worker would race dialog
    // destruction (QPointer is GUI-thread only). A plain address copies safely;
    // the queued body runs on the dialog's own thread, where it checks the
    // shared cancellation BEFORE touching the address. The destructor always
    // sets this generation's flag, so a dead owner always reads cancelled and
    // is never dereferenced; same-thread serialisation keeps a live owner
    // alive for the rest of the body.
    PlayerDialog* ownerDialog = this;
    QThreadPool::globalInstance()->start(QRunnable::create(
        [ownerDialog, cancelled, sequence, requestedPreset, backend, source, sourceEpoch] {
            native::AnimeJanaiPlan plan = native::prepareAnimeJanaiPlan(
                requestedPreset, backend, [cancelled] { return cancelled->load(); }, source,
                [](const QString& id, QString* reason) {
                    return anime4k::filterShaderProgramForCandidate(id, QString(), reason);
                });
            if (cancelled->load()) return;
            QObject* appRoot = QCoreApplication::instance();
            if (appRoot == nullptr) return;
            QMetaObject::invokeMethod(appRoot,
                [ownerDialog, cancelled, sequence, sourceEpoch, source,
                 plan = std::move(plan)]() mutable {
                    if (cancelled->load()) return;
                    // Local guard on the GUI thread: safe here (never copied
                    // off-thread). Every call below can reenter via nested
                    // event loops or direct signals that close/delete the
                    // dialog, so the guard is re-checked after each one.
                    QPointer<PlayerDialog> guard(ownerDialog);
                    if (guard.isNull() || cancelled->load() ||
                        guard->closed_ || guard->animeJanaiSequence_ != sequence) return;
                    VideoSurface* currentOwner = guard->playbackSurface();
                    if (currentOwner == nullptr) currentOwner = guard->surface_;
                    if (currentOwner == nullptr || currentOwner->sourceEpoch() != sourceEpoch) return;
                    guard->animeJanaiPreparationSourceEpoch_ = sourceEpoch;
                    guard->animeJanaiLastPreparedSource_ = source;
                    guard->animeJanaiPendingPreparationSourceEpoch_ = 0;
                    guard->animeJanaiWaitingForSourceContext_ = plan.needsSourceContext;
                    if (plan.needsSourceContext) {
                        guard->animeJanaiPlan_ = plan;
                        guard->animeJanaiPlanGeneration_ = sequence;
                        guard->shell_->setAnimeJanaiSelection(
                            plan.presetId, PlayerDialog::tr("Waiting for source metadata…"));
                        return;
                    }
                    if (guard->lastProcessingReasonKey_.startsWith(
                            QLatin1String("animejanai.")) &&
                        (!plan.enabled || plan.available)) {
                        guard->retireProcessingRow();
                    }
                    if (guard.isNull() || cancelled->load()) return;
                    guard->animeJanaiPlan_ = plan;
                    guard->animeJanaiPlanGeneration_ = sequence;
                    VideoSurface* owner = guard->playbackSurface();
                    if (owner == nullptr) owner = guard->surface_;
                    if (owner != nullptr) owner->setAnimeJanaiPlan(plan);
                    if (guard.isNull() || cancelled->load()) return;
                    const QString status = !plan.available
                        ? PlayerDialog::tr("Unavailable · %1").arg(plan.reason)
                        : (owner != nullptr ? owner->animeJanaiStatus() : PlayerDialog::tr("Off"));
                    guard->shell_->setAnimeJanaiSelection(plan.presetId, status);
                    if (guard.isNull() || cancelled->load()) return;
                    if (plan.enabled && !plan.available) {
                        guard->showProcessingFailure(plan.reasonKey, plan.reason);
                    }
                }, Qt::QueuedConnection);
        }));
}

void PlayerDialog::onAnimeJanaiSourceContextChanged() {
    if (closed_ || animeJanaiPresetId_.isEmpty()) return;
    VideoSurface* owner = playbackSurface();
    if (owner == nullptr) owner = surface_;
    if (owner == nullptr) return;
    const uint64_t epoch = owner->sourceEpoch();
    if (epoch == 0) return;
    const native::AnimeJanaiSourceContext source = owner->animeJanaiSourceContext();
    const bool hasDimensions = source.width > 0 && source.height > 0;
    if (!hasDimensions) return;
    const bool sourceChanged = animeJanaiPreparationSourceEpoch_ != 0 &&
                               animeJanaiPreparationSourceEpoch_ != epoch;
    const bool contextChanged = source.width != animeJanaiLastPreparedSource_.width ||
                                source.height != animeJanaiLastPreparedSource_.height ||
                                source.fps != animeJanaiLastPreparedSource_.fps;
    if ((sourceChanged || (animeJanaiWaitingForSourceContext_ && contextChanged)) &&
        animeJanaiPendingPreparationSourceEpoch_ != epoch)
        applyAnimeJanaiRequest(animeJanaiPresetId_);
}

void PlayerDialog::applyAnimeJanaiBackend(const QString& backendName) {
    const auto backend = native::backendFromName(backendName);
    if (!backend) return;
    animeJanaiBackend_ = *backend;
    shell_->setAnimeJanaiBackend(animeJanaiBackendName(*backend));
    QSettings().setValue(kAnimeJanaiBackendSettingKey, animeJanaiBackendName(*backend));
    if (!animeJanaiPresetId_.isEmpty()) applyAnimeJanaiRequest(animeJanaiPresetId_);
}

void PlayerDialog::onAnimeJanaiStatusChanged(const QString& status) {
    auto* owner = qobject_cast<VideoSurface*>(sender());
    if (closed_ || owner == nullptr || owner != playbackSurface()) return;
    syncAnimeJanaiEngineBuildState(owner);
    shell_->setAnimeJanaiSelection(animeJanaiPresetId_, status);
}

void PlayerDialog::onAnimeJanaiFailure(const QString& reason) {
    if (closed_ || sender() != playbackSurface()) return;
    showProcessingFailure(QStringLiteral("animejanai.failure"), reason);
}

void PlayerDialog::applyAnimeJanaiPlanToFloat(VideoSurface* target) {
    if (closed_ || target == nullptr) return;
    if (animeJanaiBuildingSurface_ != nullptr && animeJanaiBuildingSurface_ != target) {
        native::AnimeJanaiPlan off;
        target->setAnimeJanaiPlan(off);
        animeJanaiPlanWaitSurface_ = target;
        return;
    }
    target->setAnimeJanaiPlan(animeJanaiPlan_);
    if (animeJanaiPlanWaitSurface_ == target) animeJanaiPlanWaitSurface_.clear();
}

void PlayerDialog::settleAnimeJanaiPlanOnSurface(VideoSurface* completed, bool succeeded) {
    if (completed == nullptr) return;
    if (!succeeded) {
        if (animeJanaiPlanWaitSurface_ == playbackSurface()) {
            animeJanaiPlanWaitSurface_.clear();
        }
        return;
    }
    if (animeJanaiPlanWaitSurface_ == playbackSurface()) {
        VideoSurface* waiting = animeJanaiPlanWaitSurface_;
        animeJanaiPlanWaitSurface_.clear();
        applyAnimeJanaiPlanToFloat(waiting);
    }
}

void PlayerDialog::onAnimeJanaiEngineBuildChanged(bool active, bool succeeded,
                                                 const QString& detail) {
    handleAnimeJanaiEngineBuildChanged(qobject_cast<VideoSurface*>(sender()), active, succeeded,
                                       detail);
}

void PlayerDialog::syncAnimeJanaiEngineBuildState(VideoSurface* owner) {
    if (closed_) return;
    if (owner == nullptr) owner = playbackSurface();
    if (owner == nullptr || owner != playbackSurface()) return;
    if (owner->animeJanaiEngineBuildActive()) {
        handleAnimeJanaiEngineBuildChanged(owner, true, false,
                                           owner->animeJanaiEngineBuildDetail());
    }
}

QString PlayerDialog::animeJanaiBuildDetailText() const {
    // Only the wrapper around the filter's words is this dialog's to translate.
    // The raw detail is re-emitted exactly as the build reported it, and the
    // preset name is the catalogue's rendering of this operation's retained id,
    // already re-derived in the installed language by a restatement.
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

void PlayerDialog::restateAnimeJanaiBuildLanguage() {
    if (closed_ || animeJanaiBuildingSurface_ == nullptr || processingOverlay_ == nullptr) {
        return;
    }
    // A newer operation owns the window: its own caller words it, and restating
    // this build's words would overwrite text that is no longer theirs to say.
    if (animeJanaiBuildGeneration_ != enginePreparationGeneration_) return;
    // The preset name is the catalogue's rendering of the identity this
    // operation started with, so derive it again in the language now installed.
    // App-owned builtin names follow it; user and model names are the
    // catalogue's own stored text and come back unchanged. An identity the
    // catalogue no longer lists keeps the name the operation began with.
    animeJanaiBuildPresetLabel_ =
        animeJanaiPresetLabel(animeJanaiBuildPresetId_, animeJanaiBuildPresetLabel_);
    // Text only. The setters never begin or end a run, move the frame hold,
    // issue a command, bump a generation, restart the elapsed timer or cancel
    // anything, and the stage keeps the index and count the build supplied.
    retitleEnginePreparation(tr("Building TensorRT engine"));
    updateEnginePreparation(animeJanaiBuildStageIndex_, animeJanaiBuildStageCount_,
                            tr("TensorRT compilation"), animeJanaiBuildDetailText());
}

void PlayerDialog::handleAnimeJanaiEngineBuildChanged(VideoSurface* owner, bool active,
                                                       bool succeeded, const QString& detail) {
    if (closed_ || owner == nullptr) return;
    if (active) {
        if (owner != playbackSurface()) return;
        owner->setEngineFrameHold(true);
        if (animeJanaiBuildingSurface_ != owner) {
            beginEnginePreparation(tr("Building TensorRT engine"));
            animeJanaiBuildGeneration_ = enginePreparationGeneration_;
            animeJanaiBuildingSurface_ = owner;
            // The operation's preset is captured as its catalogue identity,
            // once, when the operation starts. The label recorded beside it is
            // only that identity's rendering under the current language and the
            // fallback for a catalogue that no longer lists it; a restatement
            // re-derives it from the id instead of trusting these words.
            const native::AnimeJanaiPlan plan = owner->animeJanaiPlan();
            animeJanaiBuildPresetId_ = plan.presetId;
            animeJanaiBuildPresetLabel_ =
                animeJanaiPresetLabel(plan.presetId, plan.presetId);
        }
        // The build's words are retained as typed state, not as the text the
        // overlay happens to show: a language switch restates the app-owned
        // wrapper from these facts and re-emits the filter's detail verbatim.
        animeJanaiBuildDetailRaw_ = detail;
        animeJanaiBuildWidth_ = owner->videoWidth();
        animeJanaiBuildHeight_ = owner->videoHeight();
        animeJanaiBuildStageIndex_ = -1;
        animeJanaiBuildStageCount_ = 0;
        updateEnginePreparation(animeJanaiBuildStageIndex_, animeJanaiBuildStageCount_,
                                tr("TensorRT compilation"), animeJanaiBuildDetailText());
        return;
    }
    if (!owner->animeJanaiEngineBuildActive()) owner->setEngineFrameHold(false);
    if (animeJanaiBuildingSurface_ != owner) return;
    animeJanaiBuildingSurface_.clear();
    animeJanaiBuildDetailRaw_.clear();
    animeJanaiBuildPresetId_.clear();
    animeJanaiBuildPresetLabel_.clear();
    animeJanaiBuildWidth_ = 0;
    animeJanaiBuildHeight_ = 0;
    animeJanaiBuildStageIndex_ = -1;
    animeJanaiBuildStageCount_ = 0;
    settleAnimeJanaiPlanOnSurface(owner, succeeded);
    // A superseded build reports itself but cannot end the operation owning the
    // window now: whatever superseded it already released the hold it held.
    if (animeJanaiBuildGeneration_ != enginePreparationGeneration_) {
        flashStatus(detail, !succeeded);
        return;
    }
    endEnginePreparation(succeeded, detail);
    flashStatus(detail, !succeeded);
}

void PlayerDialog::reportNativeProcessingFailure(const QString& reason) {
    reportNativeProcessingFailure(QString(), reason);
}

void PlayerDialog::reportNativeProcessingFailure(const QString& reasonKey, const QString& reason) {
    showProcessingFailure(reasonKey, reason);
}

bool PlayerDialog::nativeEffectiveActive() const {
    VideoSurface* owner = playbackSurface();
    if (owner == nullptr) return false;
    // A confirmed admission or an effective plan. A refused REPLACEMENT leaves the
    // previous plan in force, so "the Original picture" would be a lie here.
    const native::NativePlaybackController::Status& status = owner->nativePlaybackStatus();
    return status.confirmed || status.effective.has_value();
}

QString PlayerDialog::processingFailureMessage(const QString& reason) const {
    const QString base = reason.trimmed().isEmpty() ? tr("Native processing failed.")
                                                    : reason.trimmed();
    return nativeEffectiveActive() ? tr("%1 Keeping the current picture.").arg(base)
                                   : tr("%1 Staying on the Original picture.").arg(base);
}

namespace {

/// True while the controller has real native work to release: a confirmed plan,
/// an effective one, or a transition that is still holding the inference claim.
/// A merely attached probe holds nothing, so a suspend against it would be
/// refused and reported as a failure nobody caused.
bool nativeCoreEngaged(const native::NativePlaybackController::Status& status) {
    using State = native::NativePlaybackController::State;
    return status.confirmed || status.effective.has_value() ||
           status.state == State::Admitting || status.state == State::Active ||
           status.state == State::Draining;
}

/// A refused transition that says only "there was nothing to do". It arrives
/// through the same failure signal as a real one - a refused transition must
/// never be silent - so it is named here and swallowed rather than shown as a
/// processing failure the viewer cannot act on.
bool nativeFailureIsBenign(const QString& reasonKey) {
    return reasonKey == QLatin1String("native.playback.nothingToRemove") ||
           reasonKey == QLatin1String("native.playback.nothingToDrain") ||
           reasonKey == QLatin1String("native.playback.nothingToSuspend") ||
           reasonKey == QLatin1String("native.playback.notSuspended") ||
           reasonKey == QLatin1String("native.playback.noSource") ||
           reasonKey == QLatin1String("native.playback.transitionInFlight");
}

}  // namespace

void PlayerDialog::submitNativeAdmission(const native::PreparedAdmissionRequest& request) {
    VideoSurface* owner = playbackSurface();
    if (owner == nullptr) {
        reportNativeProcessingFailure(QStringLiteral("native.core.absent"),
                                      tr("No playback core can take a native admission."));
        return;
    }
    // The epoch is the owning core's own observed source epoch, which the
    // assembly layer cannot know. A request stamped for another source is
    // refused by the controller rather than applied here.
    native::PreparedAdmissionRequest stamped = request;
    stamped.epoch = owner->nativePlaybackStatus().sourceEpoch;
    // The core has to read its own filter back for the admission to be
    // acknowledged: a submitted command is not an observation.
    owner->setNativeStatusPolling(true);
    owner->requestNativeAdmission(stamped);
}

bool PlayerDialog::hasActiveSource() const {
    VideoSurface* owner = playbackSurface();
    return owner != nullptr && owner->hasVideo();
}

bool PlayerDialog::pauseIfPlaying() {
    VideoSurface* owner = playbackSurface();
    if (owner == nullptr || !owner->isPlaying()) return false;
    // setPaused announces playingChanged, which is what paints the shell and
    // the PiP control. A second write here would fight that signal.
    owner->setPaused(true);
    return true;
}

void PlayerDialog::resumePlayback() {
    VideoSurface* owner = playbackSurface();
    if (owner == nullptr || owner->isPlaying()) return;
    owner->setPaused(false);
}

VideoSurface* PlayerDialog::playbackSurface() const {
    // The float owns playback while it is up; the main core is parked. Native
    // work follows the core that is actually decoding, not the hidden one.
    if (pip_ != nullptr && pip_->videoSurface() != nullptr) {
        return pip_->videoSurface();
    }
    return surface_;
}

bool PlayerDialog::nativeRequestEnabled() const {
    return native::loadNativePreferences().enabled;
}

std::optional<native::NativePreparationContext> PlayerDialog::nativePreparationContext() const {
    VideoSurface* owner = playbackSurface();
    if (owner == nullptr) {
        return std::nullopt;
    }
    return owner->nativePreparationContext();
}

void PlayerDialog::armNativeProbe(native::Backend backend) {
    if (VideoSurface* owner = playbackSurface()) {
        owner->armNativeProbe(backend);
    }
}

void PlayerDialog::announceNativeRequest() {
    if (closed_) {
        return;
    }
    const native::RuntimeSelection& runtime = native::currentSelection();
    if (!runtime.loaded || runtime.kind != native::BundleKind::Native) {
        return;
    }
    if (nativeHandoff_ != NativeHandoff::None) {
        // Mid hand-off the single inference claim still belongs to the outgoing
        // core, and a request for the incoming one can only be refused on the
        // lease it has not released yet. That refusal would be read as the
        // hand-off settling, so the announcement waits for the settling path,
        // which announces the incoming core once the claim is actually free.
        return;
    }
    const native::NativePreferences preferences = native::loadNativePreferences();
    if (!preferences.enabled || preferences.recipeId.isEmpty()) {
        return;
    }
    VideoSurface* owner = playbackSurface();
    if (owner == nullptr) {
        return;
    }
    // The probe binding is what makes an observation possible at all, and it is
    // armed only here - on an explicit request that names a recipe, never at
    // startup. It is idempotent and carries no admission.
    // The backend the request names, which is the stored one here: this path is
    // the saved request being announced, not an unsaved settings choice.
    owner->armNativeProbe(preferences.backend);
    const std::optional<native::NativePreparationContext> context =
        owner->nativePreparationContext();
    if (!context.has_value()) {
        // Nothing was observed on this core yet. The request waits for the next
        // observation rather than announcing a source and device nobody read.
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
        // The selection or the owning core moved: the generation that was in
        // flight describes a request that is no longer the one wanted.
        emit nativeAdmissionCancelled();
    }
    nativeRequestAnnounced_ = true;
    nativeRequestSelection_ = selection;
    nativeRequestContext_ = *context;
    emit nativeAdmissionRequested(selection, *context);
}

void PlayerDialog::requestNativeReferenceFrame(int frameIndex) {
    if (closed_) {
        return;
    }
    if (nativeReferencePending_ && nativeReferenceSurface_.isNull()) {
        // The core that was asked is gone; its reply can never arrive.
        nativeReferencePending_ = false;
    }
    if (nativeReferencePending_) {
        emit nativeReferenceFrameFailed(
            QStringLiteral("native.reference.inFlight"),
            tr("A reference frame capture is already in flight."), frameIndex);
        return;
    }
    VideoSurface* owner = playbackSurface();
    if (owner == nullptr) {
        emit nativeReferenceFrameFailed(QStringLiteral("native.reference.noCore"),
                                        tr("No playback core can capture a reference frame."),
                                        frameIndex);
        return;
    }
    // The epoch and the index are recorded so a reply that arrives after the
    // source moved - or that belongs to a request this dialog has already
    // abandoned - is dropped instead of handed over as the frame that was asked
    // for.
    nativeReferenceSurface_ = owner;
    nativeReferenceSourceEpoch_ = owner->nativePlaybackStatus().sourceEpoch;
    nativeReferenceFrameIndex_ = frameIndex;
    nativeReferencePending_ = true;
    owner->requestNativeReferenceFrame(frameIndex);
}

void PlayerDialog::onNativeReferenceFrameReady(const native::FrameFixture& frame,
                                               quint64 sourceEpoch, double framePosition,
                                               int frameIndex,
                                               std::optional<native::Rational> frameFps) {
    if (closed_ || !nativeReferencePending_ || sender() != nativeReferenceSurface_.data()) {
        return;
    }
    if (frameIndex != nativeReferenceFrameIndex_) {
        // A frame for a request this dialog has moved past. The surface is the
        // authority on its own captures, but this index is the dialog's own guard:
        // forwarding it would hand a caller a frame it never asked for.
        return;
    }
    if (sourceEpoch != 0 && nativeReferenceSourceEpoch_ != 0 &&
        sourceEpoch != nativeReferenceSourceEpoch_) {
        // The source moved under the capture: this frame is not the one asked
        // for, and the surface's own stale-source refusal is the authority.
        return;
    }
    nativeReferencePending_ = false;
    emit nativeReferenceFrameReady(frame, sourceEpoch, framePosition, frameIndex, frameFps);
}

void PlayerDialog::onNativeReferenceFrameFailed(const QString& reasonKey, const QString& reason,
                                                int frameIndex) {
    if (closed_ || !nativeReferencePending_ || sender() != nativeReferenceSurface_.data()) {
        return;
    }
    if (frameIndex != nativeReferenceFrameIndex_) {
        return;
    }
    nativeReferencePending_ = false;
    emit nativeReferenceFrameFailed(reasonKey, reason, frameIndex);
}

void PlayerDialog::cancelPendingReferenceCapture(const QString& reasonKey, const QString& reason) {
    if (!nativeReferencePending_) {
        return;
    }
    nativeReferencePending_ = false;
    emit nativeReferenceFrameFailed(reasonKey, reason, nativeReferenceFrameIndex_);
}

void PlayerDialog::activateFloatNative() {
    if (closed_ || pip_ == nullptr) {
        return;
    }
    nativeHandoffFloat_.clear();
    // The float owns playback now: its own core, its own observed source and
    // device are what the request must be assembled for, and its own reader must
    // be running for the probe observation and the acknowledgement to arrive.
    pip_->videoSurface()->setNativeStatusPolling(true);
    announceNativeRequest();
}

void PlayerDialog::finishNativeHandoff() {
    nativeHandoffFloat_.clear();
    if (closed_ || surface_ == nullptr) {
        return;
    }
    // The float's generation is spent; the main core takes the single inference
    // claim back. Resume revalidates the artifacts and device the plan named, so
    // no fresh assembly is needed for a plan the controller already holds. A
    // main core that was never suspended has nothing to resume, and asking would
    // be refused as a failure nobody caused.
    emit nativeAdmissionCancelled();
    if (surface_->nativePlaybackStatus().state == native::NativePlaybackController::State::Suspended) {
        surface_->resumeNativeProcessing();
    }
}

void PlayerDialog::onNativePlaybackStatusChanged(
    const native::NativePlaybackController::Status& status) {
    if (closed_ || nativeHandoff_ == NativeHandoff::None) {
        return;
    }
    // A settled state, not a request: only the controller's own observation
    // releases the single inference claim.
    const bool settled = status.state == native::NativePlaybackController::State::Suspended ||
                         status.state == native::NativePlaybackController::State::Off ||
                         status.state == native::NativePlaybackController::State::Failed;
    if (!settled) {
        return;
    }
    VideoSurface* floatSurface =
        nativeHandoffFloat_ != nullptr ? nativeHandoffFloat_->videoSurface() : nullptr;
    if (nativeHandoff_ == NativeHandoff::SuspendingMain) {
        if (sender() == floatSurface) {
            return;
        }
        nativeHandoff_ = NativeHandoff::None;
        activateFloatNative();
        return;
    }
    if (sender() == floatSurface) {
        nativeHandoff_ = NativeHandoff::None;
        finishNativeHandoff();
    }
}

void PlayerDialog::onNativeProcessingFailure(const QString& reasonKey, const QString& reason) {
    if (closed_) {
        return;
    }
    if (nativeHandoff_ != NativeHandoff::None) {
        const NativeHandoff phase = nativeHandoff_;
        nativeHandoff_ = NativeHandoff::None;
        // A hand-off that could not suspend still has to move on, or the
        // incoming core would never get its turn. The refusal is the hand-off's
        // own bookkeeping, not a viewer-facing processing failure; a real one
        // surfaces when the incoming core's own admission is refused.
        if (phase == NativeHandoff::SuspendingMain) {
            activateFloatNative();
        } else {
            finishNativeHandoff();
        }
        return;
    }
    if (nativeFailureIsBenign(reasonKey)) {
        return;
    }
    reportNativeProcessingFailure(reasonKey, reason);
}


void PlayerDialog::showRuntimePendingStatus() {
    if (closed_) {
        return;
    }
    // The runtime layer's own sentence about the work in flight. Nothing here
    // invents a status or guesses how long it takes: a launch that is still
    // hashing the bundle is told exactly that, never "runtime missing".
    const QString text = mplayer::native::runtimePendingPlaybackText();
    if (runtimePendingVisible_ && statusLabel_->text() == text) {
        return;
    }
    runtimePendingVisible_ = true;
    statusLabel_->setText(text);
    statusLabel_->setToolTip(text);
    // The wait can outlast any status timeout, so it is held without one: an
    // informational line must not vanish while the resolution is still running
    // and the viewer is still waiting for the reason nothing plays yet.
    statusTimer_->stop();
    // The retry chip is a media failure's affordance. While the runtime is still
    // being resolved the media never had a chance to fail, so retrying now can
    // only repeat the same open: the chip stands down, and the failure text it
    // carries - if any - is left alone for the terminal answer.
    retryButton_->hide();
    if (!processingActive_) {
        processingPrepareButton_->hide();
        processingProvidersButton_->hide();
        processingOriginalButton_->hide();
    }
    statusPanel_->show();
    statusPanel_->raise();
    layoutPanels();
}

void PlayerDialog::showProcessingStatus(const QString& reason) {
    if (closed_) {
        return;
    }
    // The GENERIC native-status line: progress and an acknowledged Off use this.
    // It carries no transient timer of its own and no remedies, so a status that
    // is not a failure can never be mistaken for one, and it leaves an actionable
    // row already up untouched.
    if (processingActive_ || errorActive_) {
        statusLabel_->setText(reason);
        statusLabel_->setToolTip(reason);
        statusPanel_->show();
        statusPanel_->raise();
        layoutPanels();
        return;
    }
    statusLabel_->setText(reason);
    statusLabel_->setToolTip(reason);
    statusTimer_->start(kStatusMs);
    statusPanel_->show();
    statusPanel_->raise();
}

void PlayerDialog::showProcessingFailure(const QString& reasonKey, const QString& reason) {
    if (closed_) {
        return;
    }
    lastProcessingReasonKey_ = reasonKey;
    lastProcessingReason_ = reason.trimmed().isEmpty() ? reasonKey.trimmed() : reason.trimmed();
    showProcessingStatus(processingFailureMessage(lastProcessingReason_));
    // The row is shown only after the label, and it has no timer: the remedies
    // stay up until one is chosen, a new source supersedes them, or the dialog
    // closes. The Original action is scoped to the failing processor.
    processingActive_ = true;
    const bool animeJanaiFailure = reasonKey.startsWith(QLatin1String("animejanai."));
    processingPrepareButton_->setVisible(!animeJanaiFailure);
    processingProvidersButton_->setText(animeJanaiFailure ? tr("Settings") : tr("Providers"));
    processingProvidersButton_->setToolTip(
        animeJanaiFailure ? tr("Open processing and component settings")
                          : tr("Choose the processing provider"));
    processingOriginalButton_->setText(
        animeJanaiFailure ? tr("Turn off AnimeJaNai") : tr("Original"));
    processingProvidersButton_->show();
    processingOriginalButton_->show();
    statusLabel_->setToolTip(statusLabel_->text());
    statusPanel_->show();
    statusPanel_->raise();
    layoutPanels();
    if (reasonKey == QLatin1String("animejanai.installation.recordMissing")) {
        if (!componentSetupRequestedOnce_) {
            componentSetupRequestedOnce_ = true;
            emit componentSetupRequested();
        }
    }
}

void PlayerDialog::retireProcessingRow() {
    processingActive_ = false;
    lastProcessingReasonKey_.clear();
    lastProcessingReason_.clear();
    processingPrepareButton_->hide();
    processingProvidersButton_->hide();
    processingOriginalButton_->hide();
    // An outstanding media error keeps the panel up and keeps its own chip.
    if (!errorActive_) {
        statusPanel_->hide();
    }
}

void PlayerDialog::onProcessingPrepareRequested() {
    if (closed_) {
        return;
    }
    // Nothing happens here beyond asking the host to open its existing Processing
    // tab: no URL, no quality probe, no stream state, and no preparation is
    // started by opening anything.
    emit processingSettingsRequested(ProcessingSettingsIntent::Prepare);
}

void PlayerDialog::onProcessingProvidersRequested() {
    if (closed_) {
        return;
    }
    // Only brings the existing validated provider choice into view; Save
    // persists it exactly as it does today.
    emit processingSettingsRequested(ProcessingSettingsIntent::Providers);
}

void PlayerDialog::onProcessingOriginalRequested() {
    if (closed_) {
        return;
    }
    // The existing native TurnOff path: it saves the request, stops native work on
    // whichever core owns playback, retires the assembly and leaves the media and
    // the Anime4K chain untouched. Only this row is retired; a media error keeps
    // its chip and its recovery loop.
    if (lastProcessingReasonKey_.startsWith(QLatin1String("animejanai."))) {
        applyAnimeJanaiRequest(QString());
        return;
    }
    retireProcessingRow();
    applyNativeRequest(native::menu::Action::TurnOff, QString());
}


// ═══════════════════════════════════════════════════════════════════════════
//  picture in picture
// ═══════════════════════════════════════════════════════════════════════════

bool PlayerDialog::isPipActive() const { return pip_ != nullptr; }

void PlayerDialog::moveEnginePreparationPause(VideoSurface* owner,
                                              std::optional<bool> carriedIntent) {
    if (!enginePreparationActive_) return;
    // Read the intent while the hold is still in place: releasing it first
    // collapses playbackPauseIntent() onto the physical pause the hold forced.
    const bool savedIntent = carriedIntent.has_value()
        ? *carriedIntent
        : (enginePreparationSurface_ != nullptr
               ? enginePreparationSurface_->playbackPauseIntent()
               : owner != nullptr ? owner->playbackPauseIntent() : true);
    if (enginePreparationSurface_ != nullptr && enginePreparationSurface_ != owner) {
        // A hold left behind is released outright: it named a core that no
        // longer plays, and restoring its intent there would resume or un-pause
        // media this operation no longer owns.
        enginePreparationSurface_->setEnginePreparationPaused(false, false);
    }
    enginePreparationSurface_ = owner;
    enginePreparationSourceEpoch_ = owner != nullptr ? owner->sourceEpoch() : 0;
    if (owner != nullptr) {
        owner->setEnginePreparationPaused(true);
        if (owner->playbackPauseIntent() != savedIntent) {
            owner->setPaused(savedIntent);
        }
    }
}

void PlayerDialog::beginEnginePreparation(const QString& title) {
    if (closed_) return;
    // Supersedes any earlier operation, including one never completed.
    ++enginePreparationGeneration_;
    enginePreparationActive_ = true;
    moveEnginePreparationPause(playbackSurface());
    if (processingOverlay_ == nullptr) {
        processingOverlay_ = new ProcessingOverlay(this);
        connect(processingOverlay_, &ProcessingOverlay::cancelRequested, this, [this] {
            if (animeJanaiBuildingSurface_ != nullptr) {
                applyAnimeJanaiRequest(QString());
            } else {
                emit enginePreparationCancelRequested();
            }
        });
    }
    processingOverlay_->anchorTo(this);
    processingOverlay_->begin(title);
}

void PlayerDialog::updateEnginePreparation(int stageIndex, int stageCount, const QString& phase,
                                           const QString& detail) {
    if (closed_ || processingOverlay_ == nullptr) return;
    processingOverlay_->setStage(stageIndex, stageCount, phase, detail);
}

void PlayerDialog::retitleEnginePreparation(const QString& title) {
    if (!closed_ && processingOverlay_ != nullptr) processingOverlay_->setOperationTitle(title);
}

void PlayerDialog::updateEnginePreparationSummary(const QString& summary) {
    if (!closed_ && processingOverlay_ != nullptr) processingOverlay_->setSummary(summary);
}
void PlayerDialog::endEnginePreparation(bool ok, const QString& summary) {
    if (closed_ || animeJanaiBuildingSurface_ != nullptr) return;
    enginePreparationActive_ = false;
    if (enginePreparationSurface_ != nullptr) {
        // The terminal applies the LATEST viewer intent, but only to the core
        // that still plays.
        const bool restore = enginePreparationSurface_ == playbackSurface();
        enginePreparationSurface_->setEnginePreparationPaused(false, restore);
    }
    enginePreparationSurface_.clear();
    enginePreparationSourceEpoch_ = 0;
    if (processingOverlay_ != nullptr) processingOverlay_->finish(ok, summary);
}

bool PlayerDialog::enginePreparationVisible() const {
    return processingOverlay_ != nullptr && processingOverlay_->isVisible();
}

void PlayerDialog::togglePip() {
    // Float the video in a borderless always-on-top window. The float decodes
    // the same url in its own surface: mpv gives one core per surface, and
    // sharing one would mean reworking VideoSurface's handle ownership.
    if (closed_) {
        return;
    }
    if (pip_ != nullptr) {
        leavePip();
    } else {
        enterPip();
    }
}

void PlayerDialog::enterPip() {
    if (url_.isEmpty()) {
        return;
    }
    pipWasFullscreen_ = fullscreen_;
    if (fullscreen_) {
        fullscreen_ = false;
        transitioning_ = true;
        if (isFullScreen()) {
            showNormal();
        }
        transitioning_ = false;
        shell_->leaveFullscreen();
    }
    // Parentless on purpose: a child window is clipped to its parent's z-order,
    // so a float parented to the player slides under whatever app the user
    // switches to.
    // Reused across entries. Building or destroying a second QOpenGLWidget
    // while the player's is on screen faults the display driver in
    // DrvPresentBuffers, so the float outlives a single PiP session.
    const bool fresh = pipPool_ == nullptr;
    auto* window = fresh ? new PipFloat : pipPool_;
    pipPool_ = window;
    // Connected once. The float outlives a single PiP session, and connecting
    // again on every entry would stack a second copy of each handler, so one
    // restore click would run leavePip repeatedly.
    if (fresh) {
        // No play/seek signals: the float drives its own core, so routing them
        // to the main surface would move a video the viewer cannot see.
        connect(window, &PipFloat::muteChanged, this, [this](bool muted) {
            muted_ = muted;
            surface_->setMuted(muted_);
            shell_->setMuted(muted_);
        });
        // Two buttons, two meanings: the expand icon returns to the full
        // player, the X closes it for good. Wiring both to leavePip made the X
        // put the full window back, which is the opposite of what it promises.
        connect(window, &PipFloat::restoreRequested, this, [this] { leavePip(); });
        // close() alone is enough: the dialog's close event is what parks and
        // pauses the float, and it reads pip_, not the local `window`, so a
        // pooled float is still the one that gets put away.
        connect(window, &PipFloat::closeRequested, this, &PlayerDialog::close);
        // The float is its own core, so the episode lifecycle while it plays -
        // its load and its natural end - comes from its own surface too. A
        // stopped or replaced parked core reports neither.
        connect(window->videoSurface(), &VideoSurface::mediaLoaded, this,
                [this, source = window->videoSurface()](quint64 mediaToken) {
                    onMediaLoaded(source, mediaToken);
                });
        connect(window->videoSurface(), &VideoSurface::mediaEnded, this,
                [this, source = window->videoSurface()](quint64 mediaToken) {
                    onMediaEnded(source, mediaToken);
                });
        // The float is its own core, so its native transaction observations and
        // failures come from its own surface, not the main one.
        connect(window->videoSurface(), &VideoSurface::nativePlaybackStatusChanged, this,
                &PlayerDialog::onNativePlaybackStatusChanged);
        connect(window->videoSurface(), &VideoSurface::nativeProcessingFailure, this,
                &PlayerDialog::onNativeProcessingFailure);
        connect(window->videoSurface(), &VideoSurface::animeJanaiStatusChanged, this,
                &PlayerDialog::onAnimeJanaiStatusChanged);
        connect(window->videoSurface(), &VideoSurface::animeJanaiFailure, this,
                &PlayerDialog::onAnimeJanaiFailure);
        connect(window->videoSurface(), &VideoSurface::animeJanaiEngineBuildChanged, this,
                &PlayerDialog::onAnimeJanaiEngineBuildChanged);
        connect(window->videoSurface(), &VideoSurface::animeJanaiStatusChanged, this,
                [this](const QString&) {
                    VideoSurface* current = playbackSurface();
                    if (current == animeJanaiPlanWaitSurface_) {
                        animeJanaiPlanWaitSurface_.clear();
                        applyAnimeJanaiPlanToFloat(current);
                    }
                });
        // The float's own probe observation is what makes its source/device
        // ready, and its own factory observation is what names the current
        // source's availability while it owns playback. The slot drops the
        // parked main core's documents and dedups the announcement against the
        // pump.
        connect(window->videoSurface(), &VideoSurface::nativeStatusChanged, this,
                &PlayerDialog::onNativeStatusChanged);
        // The float's own core can be the one a reference capture is asked of.
        connect(window->videoSurface(), &VideoSurface::nativeReferenceFrameReady, this,
                &PlayerDialog::onNativeReferenceFrameReady);
        connect(window->videoSurface(), &VideoSurface::nativeReferenceFrameFailed, this,
                &PlayerDialog::onNativeReferenceFrameFailed);
    }
    applyAnimeJanaiPlanToFloat(window->videoSurface());
    // The float owns a second core, so it needs the chain separately: the mode
    // alone would drop a chain the compare window applied. It is pooled, so the
    // recorded id is compared rather than assumed - "off" clears whatever a
    // previous session left on it.
    window->videoSurface()->setAnime4KMode(anime4kMode_);
    const QString floatChain =
        anime4kChainId_.isEmpty() ? anime4k::modeId(anime4kMode_) : anime4kChainId_;
    if (!floatChain.isEmpty() && window->videoSurface()->anime4KChainId() != floatChain) {
        window->videoSurface()->setAnime4KChain(floatChain);
    }

    // The float opens from the main core's viewer-level pause intent - a hold
    // means the intent, not the physical state.
    const bool enteringPauseIntent = surface_->playbackPauseIntent();
    const bool enteringPaused = enginePreparationActive_ || enteringPauseIntent;
    window->setPlaying(!enteringPaused);
    window->setTime(surface_->position(), surface_->duration());
    window->setMuted(muted_);
    window->openStream(url_, surface_->position(), enteringPaused, episodeMediaToken());
    // Volume lives on the core and the float has its own. Without this it
    // opens at mpv's default 100% while the player sits wherever the viewer
    // set it, so the volume jumps on entering PiP.
    window->videoSurface()->setVolume(volume_ * 100.0);
    // The float has its own mpv core, so the main one keeps decoding behind a
    // hidden window unless it is stopped. That doubled the bandwidth, burned
    // the rate budget, and meant coming back to two videos at different
    // positions. Only the float plays while PiP is up.
    pipResumePosition_ = surface_->position();
    pipResumePlaying_ = !enteringPaused;
    surface_->setPaused(true);

    // Pooled reentry preserves the viewer's position and size.
    if (fresh) {
        window->resize(kPipDefaultWidth, kPipDefaultHeight);
        // Placed against the screen under the pointer, not against the player.
        // The whole point of a float is to move it to another monitor, and
        // opening it in the corner of the fullscreen player put it exactly where
        // the viewer was already looking.
        if (QScreen* current = QGuiApplication::screenAt(QCursor::pos())) {
            window->moveToCorner(current);
        } else if (QScreen* fallback = screen()) {
            window->moveToCorner(fallback);
        }
    }
    pip_ = window;
    moveEnginePreparationPause(window->videoSurface(), enteringPauseIntent);
    syncAnimeJanaiEngineBuildState(window->videoSurface());
    // The owning core changes here, so a capture asked of the main core can no
    // longer describe what the viewer is watching.
    cancelPendingReferenceCapture(QStringLiteral("native.reference.coreChanged"),
                                  tr("The reference capture was asked of the other core."));
    window->show();
    window->raise();
    syncPipBar();
    // Native hand-off: there is one process-wide inference claim, so the float
    // may only admit once the main core's claim is released by an acknowledged
    // suspend. The hand-off gate reads the stored request, never a claim that
    // native is already running.
    if (nativeRequestEnabled() && surface_ != nullptr) {
        // The float's own reader must be running for it to observe its source
        // and device and to acknowledge its own admission later.
        window->videoSurface()->setNativeStatusPolling(true);
        if (nativeCoreEngaged(surface_->nativePlaybackStatus())) {
            nativeHandoff_ = NativeHandoff::SuspendingMain;
            nativeHandoffFloat_ = window;
            surface_->suspendNativeProcessing();
        } else {
            // Nothing to release: the float may take the claim straight away.
            activateFloatNative();
        }
    }
    // The float owns playback from here, so the HUD, the menu and the
    // availability follow ITS observation, never the parked main core's: until
    // the float publishes a document of its own the roles are named unavailable
    // rather than inherited from the core that is no longer decoding. The
    // announcement is not made here: the float's own settle step makes it once
    // the claim is free.
    pushNativeInputs();
            flashStatus(tr("Picture in picture"));
    // The float owns the picture now, so what is left behind would be an empty
    // black frame.
    hide();
}

bool PlayerDialog::settleParkedNative() {
    // leavePip() clears pip_ but keeps the pooled widget, so both are checked.
    PipFloat* parked = pip_ != nullptr ? pip_ : pipPool_;
    if (parked == nullptr || parked->videoSurface() == nullptr) {
        return true;
    }
    return parked->videoSurface()->settleNativeForTeardown();
}

void PlayerDialog::releasePipPool() {
    nativeHandoff_ = NativeHandoff::None;
    nativeHandoffFloat_.clear();
    nativeReferencePending_ = false;
    nativeReferenceSurface_.clear();
    PipFloat* pool = pipPool_;
    pipPool_ = nullptr;
    pip_ = nullptr;
    if (pool == nullptr) {
        return;
    }
    if (pool->videoSurface() != nullptr &&
        !pool->videoSurface()->nativePlaybackStatus().pendingRequestId.isEmpty()) {
        qWarning().noquote() << "mikai: parked native transition in flight at destruction ("
                             << pool->videoSurface()->nativePlaybackStatus().pendingRequestId
                             << "); forced teardown, not an acknowledgement";
    }
    // NVIDIA faults if the pooled GL widget dies while the main surface is visible.
    hide();
    pool->hide();
    pool->videoSurface()->setPaused(true);
    pool->closeStream();
    delete pool;
}

void PlayerDialog::leavePip(bool closing) {
    PipFloat* window = pip_;
    // The float owns play/pause while PiP is up: the main core returns to the
    // float's live pause intent, not to the entry record. A viewer who paused
    // the float comes back paused; one who left it running comes back
    // running. The entry flag only covers a missing window.
    const bool returningPaused = window != nullptr
        ? window->videoSurface()->playbackPauseIntent()
        : !pipResumePlaying_;
    surface_->setAnimeJanaiPlan(animeJanaiPlan_);
    pip_ = nullptr;
    // The hold moves BEFORE the explicit request is applied: with the hold in
    syncAnimeJanaiEngineBuildState(surface_);
    // place the request only records the intent.
    moveEnginePreparationPause(closing || closed_ ? nullptr : surface_);
    if (!closing && !closed_) surface_->setPaused(returningPaused);
    shell_->setAnimeJanaiSelection(animeJanaiPresetId_, surface_->animeJanaiStatus());
    // The owning core returns to the main player, so a capture asked of the
    // float can no longer describe what the viewer is watching.
    cancelPendingReferenceCapture(QStringLiteral("native.reference.coreChanged"),
                                  tr("The reference capture was asked of the other core."));
    // Everything the viewer did in PiP happened in the float's core, so that
    // is where the main player resumes - not the position it was parked at on
    // entry. Read before anything else, and NOT clamped against the main
    // surface's duration: the two cores can disagree, and clamping a good
    // position to zero would silently skip the hand-back.
    const double resumeAt = window != nullptr ? std::max(0.0, window->position()) : 0.0;
    if (window != nullptr) {
        // Hidden, not destroyed. Tearing down its QOpenGLWidget while the
        // player's is coming back crashed the display driver; keeping the float
        // alive costs one idle surface and makes the next entry instant. It is
        // released with the dialog.
        window->hide();
        // Paused, though: a hidden float still has a live mpv core, and a
        // playing one decodes the stream a second time behind the player.
        window->videoSurface()->setPaused(true);
    }
    if (closed_) {
        nativeHandoff_ = NativeHandoff::None;
        nativeHandoffFloat_.clear();
        return;
    }
    const bool wasFullscreen = pipWasFullscreen_;
    pipWasFullscreen_ = false;
    if (closing) {
        nativeHandoff_ = NativeHandoff::None;
        nativeHandoffFloat_.clear();
        return;
    }
    // Native hand-back, the mirror of the entry: release the float's claim,
    // then let the main core take the single inference claim back.
    if (nativeRequestEnabled() && window != nullptr && window->videoSurface() != nullptr) {
        if (nativeCoreEngaged(window->videoSurface()->nativePlaybackStatus())) {
            nativeHandoff_ = NativeHandoff::SuspendingFloat;
            nativeHandoffFloat_ = window;
            window->videoSurface()->suspendNativeProcessing();
        } else {
            finishNativeHandoff();
        }
    } else if (surface_ != nullptr &&
               surface_->nativePlaybackStatus().state ==
                   native::NativePlaybackController::State::Suspended) {
        surface_->resumeNativeProcessing();
    }
    if (animeJanaiPlanWaitSurface_ == window->videoSurface()) {
        animeJanaiPlanWaitSurface_.clear();
    }
    // The main core owns playback again from here, so the HUD, the menu and the
    // availability go back to its own observation instead of the float's. The
    // announcement is left to the resume that follows, exactly as before.
    pushNativeInputs();
    // Deferred one turn: the hold, if any, has already moved onto the main core.
    // The pause generation drops a request a newer viewer intent overtook.
    const uint64_t returnEpoch = surface_->sourceEpoch();
    const quint64 returnPauseGeneration = surface_->pauseRequestGeneration();
    QTimer::singleShot(0, this, [this, resumeAt, wasFullscreen, returnEpoch,
                                returnPauseGeneration, returningPaused] {
        if (closed_ || pip_ != nullptr || surface_->sourceEpoch() != returnEpoch) {
            return;
        }
        if (!isVisible()) {
            show();
        }
        if (wasFullscreen && !fullscreen_) {
            fullscreen_ = true;
            transitioning_ = true;
            showFullScreen();
            transitioning_ = false;
            shell_->enterFullscreen();
        }
        if (surface_->hasVideo() && pip_ == nullptr &&
            surface_->sourceEpoch() == returnEpoch) {
            if (resumeAt > 0.0) {
                surface_->seek(resumeAt);
                shell_->setPosition(resumeAt);
            }
            if (surface_->sourceEpoch() == returnEpoch &&
                surface_->pauseRequestGeneration() == returnPauseGeneration) {
                surface_->setPaused(returningPaused);
            }
        }
    });
            flashStatus(tr("Returned to window"));
}

void PlayerDialog::syncPipBar() {
    if (pip_ == nullptr) {
        return;
    }

    // Volume and mute carry across: the viewer expects them to, and the float
    // is a second core that knows nothing about them. Position and play state
    // are deliberately not pushed - the main core is stopped while PiP is up,
    // and feeding its frozen state back is what pinned the counter to the
    // moment PiP opened.
    pip_->setMuted(muted_);
    pip_->setVolume(volume_);
}

}  // namespace shell
}  // namespace mplayer
