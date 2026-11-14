#include "VideoSession.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QMetaObject>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "gui/IdleInhibitor.hpp"
#include "gui/UiProfile.hpp"

#include "VideoRenderBridge.hpp"
#include "backend/Diagnostics.hpp"
#include "native/bundle/NativeBundle.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "native/status/NativeFilterCommand.hpp"

namespace mplayer {

namespace {

constexpr qint64 kDiagnosticsPollIntervalMs = 500;
// A lost reply must not leave a property unread for the rest of the source.
constexpr qint64 kDiagnosticsRequestTimeoutMs = 5000;
// A probe read is answered in microseconds or not at all; the bound only exists
// so a manifest can never hang the GUI thread on a core that stopped answering.
constexpr int kProbeReadTimeoutMs = 250;

// A recreated context gets 50 ms x 12 - two thirds of a second - of forced
// paints to prove the live core still delivers output before the source is
// reloaded.
constexpr int kContextReattachTicks = 12;

// One waited frame is 2,500 ms, the same bound the old polling wait had - now
// as a QTimer, not a loop.
constexpr int kUnfilteredFrameWaitMs = 2500;
// A presented frame counts as the seek's frame only when the observed playhead
// is near the seek target: a repaint of an older frame is not proof the paused
// core decoded anything new.
constexpr double kUnfilteredFramePositionToleranceSec = 0.5;

// Process-wide options are adopted by each live session on its state pump.
VideoSession::HdrToneMappingPolicy g_hdrToneMappingPolicy =
    VideoSession::HdrToneMappingPolicy::Auto;
int g_hdrToneMappingPolicyRevision = 1;
bool g_animeJanaiHdrOverride = false;
int g_animeJanaiHdrOverrideRevision = 1;

// Display scaling is a viewing preference, not a per-window state: one policy
// per process, adopted by every live session on its own state pump.
VideoScalePolicy g_videoScalePolicy;
int g_videoScalePolicyRevision = 1;

int hdrToneMappingPolicyRevision() { return g_hdrToneMappingPolicyRevision; }

int animeJanaiHdrOverrideRevision() { return g_animeJanaiHdrOverrideRevision; }

bool hdrFixesEnabled() {
    static const bool disabled = qEnvironmentVariable("MIKAI_HDR_FIXES") == QStringLiteral("0");
    return !disabled;
}

auto hdrPolicyOptionList(VideoSession::HdrToneMappingPolicy policy) {
    const bool stable = policy == VideoSession::HdrToneMappingPolicy::Stable;
    const bool vivid = policy == VideoSession::HdrToneMappingPolicy::Vivid;
    using Option = std::pair<const char*, const char*>;
    std::vector<Option> options{
        Option{"dither-depth", "auto"},
        Option{"tone-mapping", vivid ? "mobius" : "bt.2390"},
    };
    // tone-mapping-param is a float option whose reset token exists only as an
    // internal default: setting the literal "default" is refused at runtime,
    // and bt.2390 ignores the parameter anyway.
    if (vivid) options.emplace_back("tone-mapping-param", "0.3");
    options.emplace_back("hdr-compute-peak", stable ? "no" : "yes");
    options.emplace_back("hdr-peak-percentile", "99.995");
    options.emplace_back("gamut-mapping-mode", stable ? "clip" : "auto");
    return options;
}

/// The mpv format a probe property is read as. A manifest names the shape it
/// asserts on, and the wrong shape is the manifest's mistake to see.
mpv_format probePropertyFormat(ProbePropertyKind want) {
    switch (want) {
        case ProbePropertyKind::Flag:
            return MPV_FORMAT_FLAG;
        case ProbePropertyKind::Text:
            return MPV_FORMAT_STRING;
        case ProbePropertyKind::Number:
            break;
    }
    return MPV_FORMAT_DOUBLE;
}

/// Turns one reply into the shape the caller asked for, or refuses it. Both
/// numeric formats are accepted; a string or flag of another shape is not
/// coerced.
bool probeValueFromReply(const AsyncReply& reply, ProbePropertyKind want,
                         ProbePropertyValue& value) {
    if (!reply.ok()) return false;
    switch (want) {
        case ProbePropertyKind::Flag:
            if (reply.format != MPV_FORMAT_FLAG) return false;
            value.kind = ProbePropertyValue::Kind::Flag;
            value.flag = reply.flag;
            return true;
        case ProbePropertyKind::Text:
            if (reply.format != MPV_FORMAT_STRING) return false;
            value.kind = ProbePropertyValue::Kind::Text;
            value.text = QString::fromStdString(reply.text);
            return true;
        case ProbePropertyKind::Number:
            break;
    }
    if (reply.format != MPV_FORMAT_DOUBLE && reply.format != MPV_FORMAT_INT64) return false;
    value.kind = ProbePropertyValue::Kind::Double;
    value.number = reply.number;
    return true;
}

constexpr const char* kDecoderDropProperty = "decoder-frame-drop-count";
// Current mpv: "frame-drop-count" is the VO drop counter (the old
// vo-drop-frame-count is gone), so it is never read as the decoder's.
constexpr const char* kVoDropProperty = "frame-drop-count";
constexpr const char* kVoDelayedProperty = "vo-delayed-frame-count";
constexpr const char* kVfProperty = "vf";

/// The `lib=` option the native filter needs to open its inference shim: the
/// producer library of the validated native bundle this process actually
/// loaded, proved against that bundle's own inventory digest by resolution.
QVector<native::NativeFilterOption> provedNativeAttachOptions() {
    const native::RuntimeSelection& selection = native::currentSelection();
    if (!selection.loaded || selection.kind != native::BundleKind::Native ||
        !selection.resolvedBundle.has_value()) {
        return {};
    }
    const native::Bundle& bundle = *selection.resolvedBundle;
    if (!native::isBundleValidationCurrent(bundle)) return {};
    const QString producer = native::bundlePreparationProducerLibrary();
    for (const native::BundleFile& file : bundle.files) {
        if (QFileInfo(file.path).fileName().compare(producer, Qt::CaseInsensitive) != 0) continue;
        const QString absolute =
            QFileInfo(QDir::cleanPath(QDir(bundle.root).filePath(file.path))).canonicalFilePath();
        if (absolute.isEmpty()) continue;
        return {native::NativeFilterOption{QStringLiteral("lib"), absolute}};
    }
    return {};
}

std::optional<int64_t> playlistEntryIdFromCommand(const mpv_event_command& command) {
    const mpv_node& result = command.result;
    if (result.format != MPV_FORMAT_NODE_MAP || result.u.list == nullptr) return std::nullopt;
    for (int index = 0; index < result.u.list->num; ++index) {
        const char* key = result.u.list->keys[index];
        const mpv_node& value = result.u.list->values[index];
        if (key != nullptr && std::strcmp(key, "playlist_entry_id") == 0 &&
            value.format == MPV_FORMAT_INT64) {
            return value.u.int64;
        }
    }
    return std::nullopt;
}

}  // namespace

int clampedVideoZoomPercent(int zoomPercent) {
    return std::clamp(zoomPercent, -50, 100);
}

QString videoZoomArgument(int zoomPercent) {
    // The percentage names a scale factor; mpv's video-zoom is its log2. Fixed
    // six decimals with Qt's own formatting: std::to_string would emit a comma
    // under uk_UA and the core would refuse the option.
    const double factor = 1.0 + clampedVideoZoomPercent(zoomPercent) / 100.0;
    return QString::number(std::log2(factor), 'f', 6);
}

int effectiveVideoZoomPercent(VideoScaleMode mode, int zoomPercent) {
    if (mode == VideoScaleMode::Stretch || mode == VideoScaleMode::Original) return 0;
    return clampedVideoZoomPercent(zoomPercent);
}

std::vector<VideoScaleOption> videoScaleOptions(const VideoScalePolicy& policy,
                                                const std::string& downscaleDefault) {
    // Every mode carries all seven: a mode that only wrote its own option would
    // leave the previous mode's crop or aspect override in place.
    const bool stretch = policy.mode == VideoScaleMode::Stretch;
    const bool original = policy.mode == VideoScaleMode::Original;
    const char* aspect = "no";
    if (policy.mode == VideoScaleMode::Aspect16x9) {
        aspect = "16:9";
    } else if (policy.mode == VideoScaleMode::Aspect4x3) {
        aspect = "4:3";
    }
    std::vector<VideoScaleOption> options;
    options.push_back({"keepaspect", stretch ? "no" : "yes"});
    options.push_back(
        {"panscan", policy.mode == VideoScaleMode::Fill ? "1.000000" : "0.000000"});
    options.push_back({"video-aspect-override", aspect});
    options.push_back({"video-unscaled", original ? "yes" : "no"});
    options.push_back(
        {"video-zoom",
         videoZoomArgument(effectiveVideoZoomPercent(policy.mode, policy.zoomPercent))
             .toStdString()});
    options.push_back(
        {"scale", policy.upscale.isEmpty() ? std::string("lanczos")
                                           : policy.upscale.toStdString()});
    if (!policy.downscale.isEmpty()) {
        options.push_back({"dscale", policy.downscale.toStdString()});
    } else if (!downscaleDefault.empty()) {
        options.push_back({"dscale", downscaleDefault});
    }
    return options;
}

QString videoScaleModeLabel(VideoScaleMode mode) {
    switch (mode) {
        case VideoScaleMode::Fit:
            return QCoreApplication::translate("mplayer::videoscale", "Native (fit to window)");
        case VideoScaleMode::Fill:
            return QCoreApplication::translate("mplayer::videoscale", "Fill (crop edges)");
        case VideoScaleMode::Stretch:
            return QCoreApplication::translate("mplayer::videoscale", "Stretch");
        case VideoScaleMode::Aspect16x9:
            return QCoreApplication::translate("mplayer::videoscale", "16:9");
        case VideoScaleMode::Aspect4x3:
            return QCoreApplication::translate("mplayer::videoscale", "4:3");
        case VideoScaleMode::Original:
            return QCoreApplication::translate("mplayer::videoscale", "Original size (1:1)");
    }
    return {};
}

VideoScalePolicy videoScalePolicyFromSettings(const QSettings& settings) {
    // The stored preference, read from the caller's own store handle so this
    // adds no QSettings/INI allocation per call.
    VideoScalePolicy policy;
    const int storedMode = settings.value(
        QString::fromLatin1(VideoSession::kVideoScaleModeSettingKey),
        static_cast<int>(VideoScaleMode::Fit)).toInt();
    if (storedMode >= static_cast<int>(VideoScaleMode::Fit) &&
        storedMode <= static_cast<int>(VideoScaleMode::Original)) {
        policy.mode = static_cast<VideoScaleMode>(storedMode);
    }
    policy.zoomPercent =
        settings.value(QString::fromLatin1(VideoSession::kVideoZoomSettingKey), 0).toInt();
    policy.upscale = settings.value(
        QString::fromLatin1(VideoSession::kVideoUpscaleSettingKey),
        QStringLiteral("lanczos")).toString();
    policy.downscale = settings.value(
        QString::fromLatin1(VideoSession::kVideoDownscaleSettingKey), QString()).toString();
    return policy;
}

void VideoScaleFitLock::release() {
    if (!held_) return;
    held_ = false;
    auto* session = static_cast<VideoSession*>(session_.data());
    if (session == nullptr) return;
    session->compareFitActive_ = false;
    session->applyVideoScaleToEngine();
}

VideoSession::VideoSession(QObject* parent) : QObject(parent) {
    // Owned by the session, not by a GL context: a context is destroyed and
    // recreated when the view moves, and a second pump started there would run
    // this one's steps twice per core.
    statePump_.setInterval(50);
    connect(&statePump_, &QTimer::timeout, this, &VideoSession::pumpEvents);
    statePump_.start();
    // A recreated context asks the live core whether it still delivers output.
    // The budget is time, not a paint count.
    reattachTimer_.setSingleShot(false);
    reattachTimer_.setInterval(50);
    connect(&reattachTimer_, &QTimer::timeout, this, [this] {
        if (!reattachPending_) return;
        if (--reattachTicksLeft_ <= 0) {
            finishContextReattach();
            return;
        }
        if (presenter_ != nullptr && presenter_->canPresentNow()) presenter_->requestRepaint();
    });
    // Keep a replayable snapshot ahead of external listeners: a dialog that
    // reconnects after missing the original signal can synchronize from typed
    // getters without a second event source.
    connect(this, &VideoSession::animeJanaiEngineBuildChanged, this,
            [this](bool active, bool succeeded, const QString& detail) {
                Q_UNUSED(succeeded);
                animeJanaiEngineBuildActive_ = active;
                animeJanaiEngineBuildDetail_ = detail;
                publishEnginePauseHold();
            });
    nativeStatusClock_.start();
    nativePlaybackClock_.start();
    diagnosticsClock_.start();
    // The controller never reaches the core itself: it hands this session one
    // typed command at a time, and the async reply comes back through the same
    // drain as every other event.
    nativePlayback_.setCommandSink(
        [this](const native::NativePlaybackCommand& command) { deliverNativeCommand(command); });
    // Installed once, before any core exists. The callback only forwards the
    // published view: nothing here can take the core lock or reach into mpv.
    nativeStatus_.setChangeCallback([this](const native::NativeStatusView& view) {
        if (view.snapshot.has_value()) nativeSnapshotSeen_ = true;
        if (view.availability.value == native::Availability::Unavailable &&
            native::statusRefusalHaltsPolling(view.availability.reasonKey)) {
            // A filter this session just attached may not have published its tag
            // map yet, so a missing or unreadable property is retried until the
            // first snapshot arrives. Every other refusal repeats identically.
            const bool transient =
                nativeFilterBound_ && !nativeSnapshotSeen_ &&
                (view.availability.reasonKey == QStringLiteral("native.status.missing") ||
                 view.availability.reasonKey == QStringLiteral("native.status.request"));
            nativeStatusHalted_ = !transient;
        } else if (view.snapshot.has_value()) {
            // The first response named a state no poll can advance (Off,
            // Suspended, Failed, ...): stop after it and wait for a new request.
            nativeStatusHalted_ = !nativeSnapshotKeepsPolling(view.snapshot->state);
        }
        if (mpv_) {
            applyNativeDecision(nativePlayback_.observe(view, mpv_->sourceEpoch(),
                                                       nativePlaybackNowMs()));
            if (view.snapshot.has_value() && view.snapshot->probe.has_value() &&
                view.snapshot->probe->source.has_value()) {
                nativePlayback_.onSourceGeometry(*view.snapshot->probe->source);
                publishNativePlaybackStatus();
            }
        }
        emit nativeStatusChanged(view);
    });
}

VideoSession::~VideoSession() {
    // A closed player must not leave the session inhibited.
    mikai::gui::idleInhibitor().release(this);
    cancelMediaInformation();
    if (unfilteredCapture_ != nullptr) {
        // Abandoned by a destroyed session: the caller is never completed and no
        // late reply may reach a state owned by this object.
        unfilteredCapture_->finished = true;
        unfilteredCapture_->completion = nullptr;
        if (unfilteredCapture_->timer != nullptr) {
            unfilteredCapture_->timer->stop();
            unfilteredCapture_->timer = nullptr;
        }
        unfilteredCapture_.reset();
    }
    cancelAnimeJanaiOperation();
    // Hooks first: nothing may reach this session while the presenters below
    // are still able to report. The presenter keeps the core lease and frees its
    // own render context, so the core outlives every render-context free.
    setPresenter(nullptr);
    if (mpv_) {
        for (const uint64_t id : observers_) mpv_->unobserve(id);
        observers_.clear();
    }
    // The native-status reader borrows the same core: release the binding and
    // cancel any in-flight request before the core is reset.
    resetNativeStatusReader(false);
    resetDiagnostics();
    // The core's terminate-destroy synchronously frees the filter graph and its
    // native jobs. The controller's claims are held against exactly that work,
    // so they may only be released after it.
    mpv_.reset();
    nativePlayback_.endSource();
}

// ── presentation host ─────────────────────────────────────────────────────

void VideoSession::setPresenter(VideoPresenter* presenter) {
    if (presenter_ == presenter) return;
    if (presenter_ != nullptr) {
        // The hooks are outputs of the presenter: clear them before the pointer
        // is replaced, so no callback can reach a presenter that is gone.
        presenter_->framePresented = nullptr;
        presenter_->reloadPositionCaptured = nullptr;
        presenter_->renderContextReady = nullptr;
        presenter_->renderContextLost = nullptr;
        presenter_->targetStateChanged = nullptr;
        presenter_->renderError = nullptr;
    }
    presenter_ = presenter;
    if (presenter_ == nullptr) return;
    attachPresenterHooks();
    lastTargetFacts_ = presenter_->targetFacts();
    // A hold taken while no presenter existed is still owed to the new one.
    if (engineFrameHoldRequested_) presenter_->acquireRetainedPicture();
    if (pendingOpen_ && isContextReady()) {
        const bool restorePaused = requestedPause() || enginePauseHeld();
        if (startPendingUrl() && restorePaused) reassertPaused();
    }
}

void VideoSession::attachPresenterHooks() {
    presenter_->framePresented = [this](const PresentationReport& report) {
        handleFramePresented(report);
    };
    presenter_->reloadPositionCaptured = [this](double position) {
        handleReloadPositionCaptured(position);
    };
    presenter_->renderContextReady = [this](quint64 generation) {
        handleRenderContextReady(generation);
    };
    presenter_->renderContextLost = [this] { handleRenderContextLost(); };
    presenter_->targetStateChanged = [this](const RenderTargetFacts& facts) {
        handleTargetStateChanged(facts);
    };
    presenter_->renderError = [this](const QString& message) { handleRenderError(message); };
}

RenderSnapshot VideoSession::renderSnapshot() const {
    RenderSnapshot snapshot;
    snapshot.target = presenter_ != nullptr ? presenter_->targetFacts() : lastTargetFacts_;
    snapshot.sourceEpoch = sourceEpoch();
    snapshot.mediaToken = mediaToken_;
    snapshot.videoAvailable = hasVideo_;
    snapshot.opening = opening_;
    snapshot.paused = playbackPauseIntent();
    snapshot.engineFrameHoldRequested = engineFrameHoldRequested_;
    // The presenter reads this to capture a reload offset before the render
    // context is freed, so it must describe the load that is really playing:
    // a timestamp left over from a replaced source, or one read before the
    // file landed, would resume the replacement where the viewer never was.
    // A loaded paused source at 0 is a real offset, so zero is valid here.
    const bool sourceValid = mpv_ != nullptr && !url_.isEmpty() && !observedIdle_ &&
                             entryLoadReported_ && entrySourceEpoch_ == mpv_->sourceEpoch();
    const double playhead = std::isfinite(observedPosition_) ? observedPosition_ : position_;
    if (sourceValid && std::isfinite(playhead) && playhead >= 0.0) {
        snapshot.position = playhead;
        snapshot.positionValid = true;
    }
    return snapshot;
}

void VideoSession::handleFramePresented(const PresentationReport& report) {
    // The frame this report presents is already counted, so the serial is not
    // needed beyond this line: a report only says what reached the target.
    Q_UNUSED(report.serial);
    // A report for a replaced source says nothing about what is loaded now, and
    // letting it clear the new load's opening/landing state would announce a
    // frame the viewer is not looking at.
    if (report.sourceEpoch != 0 && report.sourceEpoch != sourceEpoch()) return;
    ++presentedFrames_;
    if (report.freshOutputFrame) ++diagnostics_.renderedOutputFrames;
    // A recreated renderer that has demonstrably delivered output needs no
    // reload. The pass serial is NOT proof on its own: with no video output mpv
    // still renders successfully (a VideoFrame pass with no update), which is a
    // black pass, so the bridge's own flag is what distinguishes a real frame -
    // including a REDRAW, which never sets `freshOutputFrame` but does update
    // the output. The held picture and re-blits do not set it either.
    if (reattachPending_ && presenter_ != nullptr && presenter_->hasVisibleOutputFrame()) {
        reattachPending_ = false;
        reattachTimer_.stop();
        reattachTicksLeft_ = 0;
        // Nothing will resume from it, and keeping it would block the next loss
        // from capturing its own offset.
        pendingResumePosition_ = 0.0;
        pendingResumePositionValid_ = false;
    }
    if (report.freshOutputFrame) {
        setOpening(false);
        // A frame of the loaded source reached the screen: the reattach window
        // and the reload offset it carried have both been delivered.
        reattachPending_ = false;
        reattachTimer_.stop();
        reattachTicksLeft_ = 0;
        pendingResumePosition_ = 0.0;
        pendingResumePositionValid_ = false;
        // A working renderer ends a failing streak.
        renderErrorReported_ = false;
    }
    if (unfilteredCapture_ != nullptr) checkUnfilteredFramePresented(unfilteredCapture_);
}

void VideoSession::handleReloadPositionCaptured(double position) {
    // Captured by the presenter BEFORE the dying render context reports 0 for a
    // file it no longer decodes; a capture already stored belongs to an earlier
    // loss whose reload has painted nothing yet, so it is kept.
    if (pendingResumePositionValid_) return;
    if (!std::isfinite(position) || position < 0.0) return;
    pendingResumePosition_ = position;
    pendingResumePositionValid_ = true;
}

void VideoSession::handleRenderContextReady(quint64 generation) {
    Q_UNUSED(generation);
    lastTargetFacts_.renderContextReady = true;
    if (pendingOpen_) {
        // A loadfile resets the core's pause, so the intent is saved around it.
        const bool restorePaused = requestedPause() || enginePauseHeld();
        if (startPendingUrl() && restorePaused) reassertPaused();
        return;
    }
    if (!url_.isEmpty() && !reattachPending_) beginContextReattach();
}

void VideoSession::handleRenderContextLost() {
    // Releasing the render context kills the old video output; an error that
    // follows is marked as the renderer's doing rather than the source's.
    rendererTeardownErrorPending_ = true;
    reattachPending_ = false;
    reattachTimer_.stop();
    reattachTicksLeft_ = 0;
    lastTargetFacts_.renderContextReady = false;
}

void VideoSession::handleTargetStateChanged(const RenderTargetFacts& facts) {
    lastTargetFacts_ = facts;
    if (!pendingOpen_ || !facts.renderContextReady || !facts.presentable) return;
    const bool restorePaused = requestedPause() || enginePauseHeld();
    if (startPendingUrl() && restorePaused) reassertPaused();
}

void VideoSession::handleRenderError(const QString& message) {
    // One report per streak: a renderer that fails on every pass must not
    // repeat itself for every frame it was asked for.
    if (renderErrorReported_) return;
    renderErrorReported_ = true;
    emit errorOccurred(message);
}

RenderTargetColorBits VideoSession::renderTargetColorBits() const {
    return presenter_ != nullptr ? presenter_->targetFacts().colorBits
                                 : lastTargetFacts_.colorBits;
}

int VideoSession::renderTargetInternalFormat() const {
    return presenter_ != nullptr ? presenter_->targetFacts().colorInternalFormat
                                 : lastTargetFacts_.colorInternalFormat;
}

int VideoSession::renderTargetDepthBitsPerChannel() const {
    return presenter_ != nullptr ? presenter_->targetFacts().depthBitsPerChannel
                                 : lastTargetFacts_.depthBitsPerChannel;
}

bool VideoSession::renderTargetFormatProbed() const {
    return renderTargetInternalFormat() != 0;
}

bool VideoSession::isContextReady() const {
    if (presenter_ == nullptr) return lastTargetFacts_.renderContextReady;
    return lastTargetFacts_.renderContextReady || presenter_->targetFacts().renderContextReady;
}

bool VideoSession::waitForContext(int timeoutMs) {
    if (presenter_ == nullptr) return false;
    return presenter_->waitForContext(timeoutMs);
}

quint64 VideoSession::renderContextGeneration() const {
    return presenter_ != nullptr ? presenter_->renderContextGeneration() : 0;
}

bool VideoSession::hasVisibleOutputFrame() const {
    return presenter_ != nullptr && presenter_->hasVisibleOutputFrame();
}

bool VideoSession::hasRetainedPicture() const {
    return presenter_ != nullptr && presenter_->hasRetainedPicture();
}

qint64 VideoSession::retainedPictureBytes() const {
    return presenter_ != nullptr ? presenter_->retainedPictureBytes() : 0;
}

UiProfileCounters VideoSession::uiProfileCounters() const {
    return presenter_ != nullptr ? presenter_->uiProfileCounters() : UiProfileCounters{};
}

void VideoSession::refreshRenderState() {
    if (presenter_ != nullptr) presenter_->refreshRenderState();
}

void VideoSession::holdPictureAcrossLoad() {
    if (presenter_ == nullptr) return;
    presenter_->retainAcrossLoad();
    presenter_->requestRepaint();
}

// ── engine / source ───────────────────────────────────────────────────────

void VideoSession::ensureEngine() {
    if (mpv_) return;
    // Entry ids belong to one core's playlist; a core created here has never
    // reported one, so nothing a previous core named can be matched against it.
    pendingLoadTokens_.clear();
    entryTokensById_.clear();
    std::vector<std::pair<std::string, std::string>> options{
        // Routes decoded frames into the presenter's render context.
        {"vo", "libmpv"},
        // MoonAnime's CDN answers 400 to a playlist that arrives without an
        // Accept-Language header. The value is never inspected, only its
        // presence, and mpv splits this option on commas so it stays comma-free.
        {"user-agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
         "Chrome/142.0.0.0 Safari/537.36"},
        {"http-header-fields", "Accept-Language: uk-UA"},
        {"hwdec", "auto-safe"},
        {"volume", mpvNumber(requestedVolume_)},
        {"mute", requestedMuted_ ? "yes" : "no"},
        // The keyboard belongs to the shell: mpv's own bindings would consume
        // Space and the arrow keys before Qt ever sees them.
        {"input-default-bindings", "no"},
        {"input-vo-keyboard", "no"},
        {"input-cursor", "no"},
        {"osc", "no"},
        // Reference captures must use decoded frames without shaders or OSD.
        {"screenshot-sw", "yes"},
        // Qt owns presentation, so render calls stay nonblocking.
        {"video-timing-offset", "0"},
        // Prevent SDR video from incorrectly triggering HDR on the display:
        // hint colorspace only from source metadata.
        {"target-colorspace-hint-mode", "source"},
        {"target-colorspace-hint", "no"},
    };
    // The display mode is applied before initialize so the first frame is
    // already in it. An Auto downscale is left out here on purpose: dscale's
    // real default can only be read from an initialized core.
    for (const VideoScaleOption& option : videoScaleOptions(videoScalePolicy(), {})) {
        options.emplace_back(option.name, option.value);
    }
    // The default belongs to the core that answered it, and this is a new one.
    downscaleDefault_.reset();
    downscaleDefaultError_.reset();
    downscaleWasExplicit_ = false;
    mpv_ = std::make_shared<MpvHandle>(options);
    mpvApiVersion_ = MpvHandle::apiVersion();
    applyHdrPolicyToEngine();
    applyVideoScaleToEngine(/*queryCoreDefault=*/true);
    // A plan may have been selected before the core existed. Request its backend
    // reports before any source or official filter command reaches this core.
    mpv_->setBackendLogObservationEnabled(animeJanaiBackendLogsNeeded());
    // The filter opens its inference shim only for an explicitly named producer
    // library, so the binding an admission is later loaded into has to name the
    // one this core's own bundle ships.
    nativePlayback_.setAttachOptions(provedNativeAttachOptions());
    // The reader borrows this core, and is (re)bound here because this is the
    // one place a core is created.
    nativeStatus_.attach(mpv_.get());
    // A fresh core, so any raw number a previous one left reads as unknown.
    resetDiagnostics();
    mpv_->setEventSink([this](mpv_event* event) {
        if (event->event_id == MPV_EVENT_LOG_MESSAGE && event->data != nullptr) {
            const auto* message = static_cast<const mpv_event_log_message*>(event->data);
            const QString line =
                QString::fromUtf8(message->text == nullptr ? "" : message->text).trimmed();
            const QString prefix =
                QString::fromUtf8(message->prefix == nullptr ? "" : message->prefix);
            // libmpv echoes the URLs it was handed and the filter commands it
            // was asked to run; both can carry a signed token, and Qt logging is
            // persisted. The redaction happens at this boundary. The observation
            // and failure paths below keep the exact mpv text: matching a status
            // marker is not a persistence boundary.
            const QString safePrefix = mikai::backend::redactDiagnostic(prefix);
            const QString safeLine = mikai::backend::redactDiagnostic(line);
            if (message->log_level <= MPV_LOG_LEVEL_WARN) {
                qWarning().noquote() << "[mpv]" << safePrefix << safeLine;
            } else {
                qInfo().noquote() << "[mpv]" << safePrefix << safeLine;
            }
            const QString lower = (prefix + QLatin1Char(' ') + line).toLower();
            observeAnimeJanaiBackendLog(prefix, line);
            const bool ajiMessage = lower.contains(QStringLiteral("aji")) ||
                                    lower.contains(QStringLiteral("animejanai")) ||
                                    lower.contains(QStringLiteral("inference shim")) ||
                                    (animeJanaiFilterBound_ &&
                                     (lower.contains(QStringLiteral("inference")) ||
                                      lower.contains(QStringLiteral("model")) ||
                                      lower.contains(QStringLiteral("backend")) ||
                                      lower.contains(QStringLiteral("engine"))));
            if (ajiMessage &&
                (lower.contains(QStringLiteral("error")) ||
                 lower.contains(QStringLiteral("failed")) ||
                 lower.contains(QStringLiteral("failure")))) {
                reportAnimeJanaiBackendError(line);
            }
            return;
        }
        if (event->event_id == MPV_EVENT_COMMAND_REPLY) {
            // Keep the raw reply: superseded callbacks are dropped, but this
            // result still identifies the entry created by this loadfile.
            const auto pending = pendingLoadTokens_.find(event->reply_userdata);
            if (pending != pendingLoadTokens_.end()) {
                const LoadIdentity load = pending->second;
                pendingLoadTokens_.erase(pending);
                if (event->error < 0) {
                    // A refused load never reports FILE_LOADED, so the gate it
                    // armed is dropped here - only for the load still current.
                    if (load.epoch == mpv_->sourceEpoch() && presenter_ != nullptr) {
                        presenter_->noteLoadRefused(load.epoch);
                    }
                } else if (event->data != nullptr) {
                    if (const auto entryId = playlistEntryIdFromCommand(
                            *static_cast<const mpv_event_command*>(event->data))) {
                        // Replacements retire older entries; playlist ids only increase.
                        if (entryTokensById_.size() >= 64) {
                            entryTokensById_.erase(entryTokensById_.begin());
                        }
                        if (havePlaylistEntryId_ && currentPlaylistEntryId_ == *entryId) {
                            entryMediaToken_ = load.token;
                            entrySourceEpoch_ = load.epoch;
                            // A named START_FILE and an accepted command are not a
                            // loaded file, so this latches only when FILE_LOADED
                            // has already reported for this entry and the epoch is
                            // current. An episode token is reused across quality
                            // swaps, so it cannot tell two loads apart.
                            if (entryLoadReported_ && load.epoch == mpv_->sourceEpoch()) {
                                if (presenter_ != nullptr) presenter_->noteLoadLanded(load.epoch);
                                mediaTracksLoaded_ = true;
                                mediaTracksDirty_ = true;
                                // FILE_LOADED may precede the command reply that
                                // finally identifies this entry. Publish only now,
                                // rather than emitting an unnamed token.
                                emit mediaLoaded(load.token);
                            }
                        } else {
                            entryTokensById_[*entryId] = load;
                        }
                    }
                }
            }
            return;
        }
        if (event->event_id == MPV_EVENT_START_FILE) {
            // Errors after START_FILE belong to this entry, not renderer teardown.
            rendererTeardownErrorPending_ = false;
            if (event->data != nullptr) {
                currentPlaylistEntryId_ =
                    static_cast<const mpv_event_start_file*>(event->data)->playlist_entry_id;
                havePlaylistEntryId_ = true;
            }
            entryMediaToken_ = 0;
            entrySourceEpoch_ = 0;
            if (havePlaylistEntryId_) {
                const auto known = entryTokensById_.find(currentPlaylistEntryId_);
                if (known != entryTokensById_.end()) {
                    entryMediaToken_ = known->second.token;
                    entrySourceEpoch_ = known->second.epoch;
                    entryTokensById_.erase(known);
                }
            }
            entryLoadReported_ = false;
            return;
        }
        if (event->event_id == MPV_EVENT_FILE_LOADED) {
            // A command or START_FILE is only an attempt; report once when mpv
            // says the file is loaded.
            if (!entryLoadReported_) {
                entryLoadReported_ = true;
                if (entrySourceEpoch_ != 0 && entrySourceEpoch_ == mpv_->sourceEpoch()) {
                    mediaTracksLoaded_ = true;
                    mediaTracksDirty_ = true;
                    if (presenter_ != nullptr) presenter_->noteLoadLanded(entrySourceEpoch_);
                    emit mediaLoaded(entryMediaToken_);
                }
            }
            return;
        }
        if (event->event_id != MPV_EVENT_END_FILE || event->data == nullptr) return;
        const auto* ended = static_cast<const mpv_event_end_file*>(event->data);
        if (ended->reason == MPV_END_FILE_REASON_EOF) {
            // EOF completes only the loaded entry; a stale end must not alter
            // the tracking for the current one.
            if (havePlaylistEntryId_ && ended->playlist_entry_id != currentPlaylistEntryId_) {
                return;
            }
            if (entrySourceEpoch_ != mpv_->sourceEpoch()) return;
            url_.clear();
            setOpening(false);
            animeJanaiSourceRequested_ = false;
            diagnosticsActive_ = false;
            if (entryLoadReported_) emit mediaEnded(entryMediaToken_);
            entryMediaToken_ = 0;
            entryLoadReported_ = false;
            return;
        }
        if (ended->reason != MPV_END_FILE_REASON_ERROR) return;
        // With vo=libmpv the video output exists only under the presenter's
        // render context, so freeing that context is what uninitializes the VO
        // and makes mpv report this error. That event can be drained BEFORE the
        // presenter's queued loss hook arrives, so the cause is read from the
        // presenter's own readiness, which turns false the moment a teardown
        // starts - not from a flag that only lands with the loss report. The
        // source identity and the captured offset are deliberately kept: the
        // reattach path is what rebuilds the VO, and a source cleared here could
        // never be reloaded. A VO failure under a READY context is a genuine
        // failure and still takes the error branch below.
        const bool rendererNotReady =
            presenter_ != nullptr && !presenter_->targetFacts().renderContextReady;
        if ((rendererTeardownErrorPending_ || rendererNotReady) &&
            ended->error == MPV_ERROR_VO_INIT_FAILED) {
            rendererTeardownErrorPending_ = false;
            setOpening(false);
            qWarning("VideoSession: retired the video output error caused by releasing the "
                     "render context (mpv: %d)",
                     ended->error);
            return;
        }
        if (havePlaylistEntryId_ && ended->playlist_entry_id != currentPlaylistEntryId_) {
            qWarning("VideoSession: retired a playback error for playlist entry %lld (playing "
                     "%lld, mpv: %d)",
                     static_cast<long long>(ended->playlist_entry_id),
                     static_cast<long long>(currentPlaylistEntryId_), ended->error);
            return;
        }
        if (entrySourceEpoch_ != 0 && entrySourceEpoch_ != mpv_->sourceEpoch()) {
            // The replacement load has been submitted but has not reached
            // START_FILE yet. The last started entry is already retired.
            return;
        }
        url_.clear();
        pendingOpen_ = false;
        // A load that failed may never report FILE_LOADED, so the gate it armed
        // is dropped here: the held picture stays with the error overlay instead
        // of being stranded behind a latch nothing can satisfy.
        if (presenter_ != nullptr) presenter_->noteLoadRefused(entrySourceEpoch_);
        setOpening(false);
        animeJanaiSourceRequested_ = false;
        diagnosticsActive_ = false;
        emit errorOccurred(tr("Could not play the video (mpv: %1)").arg(ended->error));
    });
    observePlaybackState();
    // The applied id, not the mode: a chain with no Mode is only recorded until
    // the engine exists, so reading the mode here would drop it before the
    // first frame.
    const QString chainId =
        anime4kChainId_.isEmpty() ? anime4k::modeId(anime4kMode_) : anime4kChainId_;
    QString reason;
    const QString shaders = resolvedChainForCurrentSource(chainId, &reason);
    if (!reason.isEmpty() && chainId != QLatin1String("off")) {
        emit anime4kGated(reason);
    } else {
        if (chainId != QLatin1String("off") && hdrFixesEnabled() &&
            sourceColorProfile_.requiresSdrGate()) {
            emit anime4kGated(tr("Anime4K skipped: source transfer metadata is unknown or HDR"));
        }
        if (!shaders.isEmpty()) applyShaderChainRequest(shaders);
    }
}

bool VideoSession::open(const QString& url, quint64 mediaToken) {
    // A new source replaces what is playing, and the viewer keeps looking at the
    // outgoing picture while the new one loads. The epoch below still retires
    // every reply and every claim the outgoing source owned: the hold is a
    // picture, not state. An empty URL is the intentional clear.
    reattachPending_ = false;
    reattachTimer_.stop();
    if (url.isEmpty()) {
        engineFrameHoldRequested_ = false;
        if (presenter_ != nullptr) presenter_->invalidateRetainedPictures();
    } else {
        holdPictureAcrossLoad();
    }
    pendingMediaTrackSelection_ = {};
    resetMediaTracks();
    discardEnginePause();
    finishAnimeJanaiEngineBuild(false, tr("The source changed during engine preparation."));
    animeJanaiSourceRequested_ = !url.isEmpty();
    pendingOpen_ = false;
    // The identity belongs to THIS request: a deferred load picks it up when it
    // runs, and a loadfile refused below never leaves it on the queue.
    mediaToken_ = mediaToken;
    cancelAnimeJanaiOperation();
    animeJanaiEpoch_ = 0;
    if (animeJanaiPlan_.enabled) {
        setAnimeJanaiStatus(QStringLiteral("Configured; waiting for a loaded source"));
    }
    url_ = url;
    // An explicit admission replaces any offset a loss captured.
    pendingResumePosition_ = 0.0;
    pendingResumePositionValid_ = false;
    // The viewer-facing wait starts here, not at the load command.
    setOpening(!url_.isEmpty());
    networkBytesPerSecond_ = 0.0;
    streamFramesPerSecond_ = 0.0;
    streamFpsSource_ = StreamFpsSource::Unknown;
    statsClock_.restart();
    observedContainerFps_ = 0.0;
    observedVideoWidth_ = 0.0;
    observedVideoHeight_ = 0.0;
    observedOutputWidth_ = 0.0;
    observedOutputHeight_ = 0.0;
    observedOutputRotation_ = 0.0;
    publishDisplayAspectRatio(0.0);
    emit streamStatsChanged(0.0, 0.0, StreamFpsSource::Unknown);
    try {
        if (mpv_ != nullptr) {
            mpv_->setBackendLogObservationEnabled(animeJanaiBackendLogsNeeded());
        }
        ensureEngine();
    } catch (const MpvError& e) {
        // A failed engine cannot drain a deferred load.
        url_.clear();
        animeJanaiSourceRequested_ = false;
        pendingOpen_ = false;
        setOpening(false);
        diagnosticsActive_ = false;
        if (mpv_ != nullptr) {
            try {
                mpv_->setBackendLogObservationEnabled(false);
            } catch (const MpvError&) {
            }
        }
        emit errorOccurred(QString::fromUtf8(e.what()));
        return false;
    }
    // A new source: the previous filter's snapshot says nothing about this one,
    // so the reader is rebound (which clears the published view) and a halt
    // recorded for the old source does not outlive it.
    resetNativeStatusReader(true);
    nativeStatusHalted_ = false;
    // A source is open, so the controller binds the core's epoch. The load below
    // advances it and the next pump rebinds; the previous session's claims are
    // deliberately kept, because a source change is not a stop.
    nativeSourceActive_ = true;
    syncNativeSource();
    // A new source: the old raw numbers say nothing about this one.
    resetDiagnostics();
    diagnosticsActive_ = true;
    // With vo=libmpv the video output attaches to the render context, and mpv
    // will not start a VO that has nowhere to draw: loading first leaves the
    // core with no video. Callers legitimately open before the view is shown.
    if (!isContextReady()) {
        // Latched BEFORE the presenter is asked, because a presenter that can
        // create its context now reports it synchronously: the context-ready
        // hook drains this deferred load, and a latch set afterwards would
        // strand it behind a load nobody starts.
        pendingOpen_ = true;
        try {
            mpv_->setBackendLogObservationEnabled(false);
        } catch (const MpvError& e) {
            emit errorOccurred(QString::fromUtf8(e.what()));
        }
        if (presenter_ != nullptr) {
            QString reason;
            if (!presenter_->ensureRenderContext(coreLease(), &reason) && !reason.isEmpty()) {
                emit errorOccurred(reason);
            }
        }
        // Already drained by the context-ready hook.
        if (!pendingOpen_) return true;
        if (!isContextReady()) return true;
    }
    pendingOpen_ = false;
    return startPendingUrl();
}

bool VideoSession::startPendingUrl(double resumeAt) {
    if (mikai::gui::uiprofile::enabled()) {
        // Host only: a stream URL carries a signed query that must never reach a
        // log file, and a local path is identified by its scheme alone.
        const QString host = url_.contains(QLatin1String("://"))
                                 ? QUrl(url_).host()
                                 : QStringLiteral("local");
        mikai::gui::uiprofile::log(
            "session.startUrl",
            QStringLiteral("position=%1 host=%2")
                .arg(QString::number(resumeAt, 'f', 3), host));
    }
    pendingOpen_ = false;
    if (url_.isEmpty() || mpv_ == nullptr) {
        // There is nothing left to wait for, and no new source will replace the
        // picture a hold would be standing in for.
        setOpening(false);
        engineFrameHoldRequested_ = false;
        if (presenter_ != nullptr) presenter_->invalidateRetainedPictures();
        animeJanaiSourceRequested_ = false;
        return false;
    }
    try {
        mpv_->setBackendLogObservationEnabled(animeJanaiBackendLogsNeeded());
    } catch (const MpvError& e) {
        animeJanaiSourceRequested_ = false;
        emit errorOccurred(QString::fromUtf8(e.what()));
        return false;
    }
    holdPictureAcrossLoad();
    // The source ends HERE, before the load is issued: the epoch is what retires
    // a reply for the source being replaced, and a command queued behind this
    // one must already be reading the new epoch when its reply lands.
    mpv_->invalidateAsyncEpoch();
    const uint64_t loadEpoch = mpv_->sourceEpoch();
    resetMediaTracks();
    // Even equal-length replacements owe a new duration notification so a
    // deferred resume/seek cannot wait forever on the previous file's value.
    observedDuration_ = 0.0;
    observedPosition_ = 0.0;
    observedVideoWidth_ = observedVideoHeight_ = 0.0;
    // Same boundary for the filtered output: the loaded file's geometry cannot
    // shape the replacement, and a reset published aspect lets an identical
    // value from the new file be announced rather than suppressed as unchanged.
    observedOutputWidth_ = observedOutputHeight_ = observedOutputRotation_ = 0.0;
    publishDisplayAspectRatio(0.0);
    hasVideo_ = false;
    observedColorProfile_ = {};
    sourceColorProfile_ = {};
    if (hdrRenderingFixesEnabled()) {
        issueCoreCommand({"set", "target-colorspace-hint", "no"},
                         "target-colorspace-hint", false);
    }
    const QString activeChainId =
        anime4kChainId_.isEmpty() ? anime4k::modeId(anime4kMode_) : anime4kChainId_;
    if (!anime4k::shaderStringForCandidate(activeChainId).isEmpty() && hdrFixesEnabled())
        applyShaderChainRequest(QString());
    emit sourceColorProfileChanged(sourceColorProfile_);
    videoWidth_ = videoHeight_ = 0;
    position_ = 0.0;
    duration_ = 0.0;
    emit durationChanged(0.0);
    emit positionChanged(0.0);
    const MediaTrackSelection selection = nextMediaTrackSelection_.value_or(MediaTrackSelection{});
    nextMediaTrackSelection_.reset();
    const auto option = [](std::optional<int64_t> id) {
        if (!id.has_value()) return std::string("auto");
        return *id == 0 ? std::string("no") : std::to_string(*id);
    };
    std::string options = "aid=" + option(selection.audio) + ",sid=" + option(selection.subtitle) +
                          ",sub-auto=exact,audio-file-auto=exact";
    if (resumeAt > 0.0) options += ",start=" + mpvNumber(resumeAt);
    std::vector<std::string> load{"loadfile", url_.toStdString(), "replace"};
    if ((mpvApiVersion_ >> 16) > 2 ||
        ((mpvApiVersion_ >> 16) == 2 && (mpvApiVersion_ & 0xffffu) >= 3)) {
        load.emplace_back("-1");
    }
    load.push_back(std::move(options));
    // Armed BEFORE the submission: a transport refusal reports synchronously
    // inside issueCoreCommand and drops the gate there, so arming after the call
    // would overwrite that with a gate nothing can satisfy.
    if (presenter_ != nullptr) presenter_->armLoadLanding(loadEpoch);
    issueCoreCommand(std::move(load), "loadfile", /*reportRefusal=*/true, mediaToken_);
    // mpv also comes up paused when a render context is its only consumer: it
    // has no window of its own to drive playback. Without this the core decodes
    // one frame, hands it over and stops, which reads as a black surface and a
    // frozen clock rather than as an obvious failure.
    if (enginePreparationPaused_) {
        enginePauseEpoch_ = mpv_->sourceEpoch();
        setPlaying(false);
        requestPausedWrite(true);
    } else {
        // The host may intend to open paused and unpause itself, so the state is
        // announced locally and the core is only told, never waited for.
        requestPausedWrite(false);
    }

    // The load advanced the core's source epoch. Binding it here means the epoch
    // a caller reads is the one the admission is requested under.
    syncNativeSource();
    animeJanaiEpoch_ = mpv_->sourceEpoch();
    emit sourceEpochChanged(animeJanaiEpoch_);
    if (animeJanaiPlan_.enabled) processAnimeJanaiPlan();
    if (presenter_ != nullptr) presenter_->requestRepaint();
    return true;
}

void VideoSession::close() {
    // A closed source is not reattached to: the attempt belongs to the picture
    // this close is taking away.
    reattachPending_ = false;
    reattachTimer_.stop();
    // A captured offset belongs to the source this close is taking away; the
    // next open brings its own intent.
    pendingResumePosition_ = 0.0;
    pendingResumePositionValid_ = false;
    url_.clear();
    setOpening(false);
    engineFrameHoldRequested_ = false;
    if (presenter_ != nullptr) presenter_->invalidateRetainedPictures();
    observedColorProfile_ = {};
    sourceColorProfile_ = {};
    emit sourceColorProfileChanged(sourceColorProfile_);
    resetMediaTracks();
    nextMediaTrackSelection_.reset();
    pendingMediaTrackSelection_ = {};
    discardEnginePause();
    finishAnimeJanaiEngineBuild(false, tr("Playback closed during engine preparation."));
    animeJanaiSourceRequested_ = false;
    pendingOpen_ = false;
    cancelAnimeJanaiOperation();
    animeJanaiEpoch_ = 0;
    // Nothing this session already asked to play can start now: the stop below
    // retires those requests, so their identities must not pair with an entry a
    // later load starts.
    pendingLoadTokens_.clear();
    entryTokensById_.clear();
    entryMediaToken_ = 0;
    entryLoadReported_ = false;
    mediaToken_ = 0;
    setAnimeJanaiStatus(animeJanaiPlan_.enabled
                            ? QStringLiteral("Configured; waiting for a loaded source")
                            : QStringLiteral("Off"));
    if (!mpv_) return;
    try {
        mpv_->setBackendLogObservationEnabled(false);
    } catch (const MpvError& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
    }
    // Nothing is loaded after this: drop the reader before the source ends so
    // no request in flight can describe a file that is gone.
    resetNativeStatusReader(false);
    nativeStatusHalted_ = false;
    resetDiagnostics();
    diagnosticsActive_ = false;
    // Same retirement as the load: what was outstanding belonged to the source
    // that is ending, so it is dropped before the stop is issued.
    mpv_->invalidateAsyncEpoch();
    pendingPauseWrite_.reset();
    animeJanaiEpoch_ = mpv_->sourceEpoch();
    observedVideoWidth_ = 0.0;
    observedVideoHeight_ = 0.0;
    observedOutputWidth_ = 0.0;
    observedOutputHeight_ = 0.0;
    observedOutputRotation_ = 0.0;
    publishDisplayAspectRatio(0.0);
    observedContainerFps_ = 0.0;
    emit sourceEpochChanged(mpv_->sourceEpoch());
    // A stop on an already-dead core is not worth reporting. Keep the keyed
    // cache lease until mpv acknowledges that the old pipeline has stopped.
    const auto stoppingLease = animeJanaiStageBuildLease_;
    const AsyncRequestId stopRequest = mpv_->commandAsync(
        {"stop"}, [this, stoppingLease](const AsyncReply& reply) {
            if (!reply.ok()) return;
            if (stoppingLease) stoppingLease->release();
            if (animeJanaiStageBuildLease_ == stoppingLease) animeJanaiStageBuildLease_.reset();
        });
    Q_UNUSED(stopRequest);
    animeJanaiEpoch_ = 0;
    // Emitted before the reset so the chrome falls back to its play glyph while
    // the session still reports a loaded file.
    if (playing_) {
        playing_ = false;
        publishPlayingState(false);
    }
    hasVideo_ = false;
    position_ = 0.0;
    duration_ = 0.0;
    videoWidth_ = videoHeight_ = 0;
    networkBytesPerSecond_ = 0.0;
    streamFramesPerSecond_ = 0.0;
    streamFpsSource_ = StreamFpsSource::Unknown;
    // The core keeps reporting while nothing is loaded, and the next open must
    // not start from the previous file's numbers.
    observedDuration_ = 0.0;
    observedPosition_ = 0.0;
    observedVideoWidth_ = 0.0;
    observedVideoHeight_ = 0.0;
    observedCacheSpeed_ = 0.0;
    observedContainerFps_ = 0.0;
    emit streamStatsChanged(0.0, 0.0, StreamFpsSource::Unknown);
    handleState();
    if (presenter_ != nullptr) presenter_->requestRepaint();
}

// ── transport ─────────────────────────────────────────────────────────────

void VideoSession::togglePause() {
    // The requested state, not the observed one: a toggle during a write that is
    // still in flight has to invert what the viewer last asked for.
    setPaused(enginePauseHeld() ? !enginePauseIntent_ : !requestedPause());
}

void VideoSession::setPaused(bool paused) {
    ++pauseRequestGeneration_;
    const bool held = enginePauseHeld();
    if ((held || enginePreparationPaused_) &&
        pauseRequestGeneration_ > enginePauseRequestGeneration_) {
        enginePauseIntent_ = paused;
    }
    if (!mpv_) return;
    // The viewer's intent is announced at once, through playing_, and told to
    // the core without waiting for it.
    setPlaying(!held && !paused);
    requestPausedWrite(held || paused);
}

void VideoSession::reassertPaused() {
    if (!mpv_) return;
    setPlaying(false);
    requestPausedWrite(true);
}

void VideoSession::requestPausedWrite(bool paused) {
    if (mpv_ == nullptr) return;
    const uint64_t epoch = mpv_->sourceEpoch();
    const quint64 generation = ++pauseWriteGeneration_;
    // With no earlier write awaiting observation, `observedPaused_` is what the
    // core holds: a request for that same value changes nothing and owes no
    // event, so it is the only request the reply may retire.
    const bool alreadyHeld = !pendingPauseWrite_.has_value() && observedPaused_ == paused;
    pendingPauseWrite_ = PauseWrite{generation, epoch, paused};
    const AsyncRequestId request = mpv_->commandAsync(
        {"set", "pause", paused ? "yes" : "no"},
        [this, epoch, generation, alreadyHeld](const AsyncReply& reply) {
            if (mpv_ == nullptr) return;
            // A superseded request, or one for a source that has since been
            // replaced, says nothing about what is playing now.
            if (!pendingPauseWrite_ || pendingPauseWrite_->generation != generation) return;
            if (epoch != mpv_->sourceEpoch()) {
                pendingPauseWrite_.reset();
                return;
            }
            if (!reply.ok()) {
                // Refused: nothing will be observed to hold the request, so the
                // property is authoritative again and the refusal is named.
                pendingPauseWrite_.reset();
                emit errorOccurred(
                    tr("pause: %1")
                        .arg(QString::fromStdString(mpv_->describeError(reply.error))));
                return;
            }
            if (alreadyHeld) pendingPauseWrite_.reset();
        });
    if (!request.valid()) {
        // No reply will ever arrive for a command the transport would not take.
        pendingPauseWrite_.reset();
        qWarning("VideoSession: the mpv transport did not accept set pause");
        emit errorOccurred(tr("pause: the mpv transport did not accept the command"));
    }
}

void VideoSession::seek(double seconds) {
    // A viewer seek supersedes a captured animejanai reconfigure position: the
    // picture they moved to is where the rebuild must land, not where it began.
    discardAnimeJanaiReconfigurePosition();
    if (!mpv_) return;
    // A seek before a file is loaded is normal during startup, so a refusal is
    // tolerated here.
    issueCoreCommand({"seek", mpvNumber(seconds), "absolute"}, "seek",
                     /*reportRefusal=*/false);
    emit seekRequested();
}

void VideoSession::seekExact(double seconds) {
    discardAnimeJanaiReconfigurePosition();
    if (!mpv_) return;
    // A position put back after the engine moved it must be the position, not
    // the keyframe the core would otherwise re-decode from.
    issueCoreCommand({"seek", mpvNumber(seconds), "absolute+exact"}, "seek",
                     /*reportRefusal=*/false);
    emit seekRequested();
}

void VideoSession::seekRelative(double delta) {
    discardAnimeJanaiReconfigurePosition();
    if (!mpv_) return;
    // `relative` lets the core resolve the step against the position it is at
    // right now, and `exact` keeps the landed frame deterministic.
    issueCoreCommand({"seek", mpvNumber(delta), "relative+exact"}, "seek",
                     /*reportRefusal=*/false);
    emit seekRequested();
}

void VideoSession::setVolume(double volume) {
    requestedVolume_ = volume;
    if (!mpv_) return;
    issueCoreCommand({"set", "volume", mpvNumber(volume)}, "volume", /*reportRefusal=*/true);
}

void VideoSession::setMuted(bool muted) {
    requestedMuted_ = muted;
    if (!mpv_) return;
    issueCoreCommand({"set", "mute", muted ? "yes" : "no"}, "mute", /*reportRefusal=*/true);
}

void VideoSession::setSpeed(double speed) {
    if (!mpv_) return;
    issueCoreCommand({"set", "speed", mpvNumber(speed)}, "speed", /*reportRefusal=*/true);
}

void VideoSession::issueCoreCommand(std::vector<std::string> args, const char* what,
                                    bool reportRefusal, std::optional<quint64> mediaToken) {
    if (mpv_ == nullptr || args.empty()) return;
    // The reply is only ever an error report, so a request against a superseded
    // source - whose refusal says nothing about what is playing now - is dropped
    // by the epoch captured here.
    const uint64_t epoch = mpv_->sourceEpoch();
    const std::string label = what;
    const std::string command = args.front();
    const AsyncRequestId request = mpv_->commandAsync(
        std::move(args), [this, epoch, label, reportRefusal](const AsyncReply& reply) {
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() || reply.ok()) return;
            const QString reason = QString::fromStdString(mpv_->describeError(reply.error));
            if (reportRefusal) {
                emit errorOccurred(tr("%1: %2").arg(QString::fromStdString(label), reason));
                return;
            }
            // Tolerated, still not silent: naming the command is the only
            // diagnostic a refusal of a seek before load or a stop has.
            qWarning("VideoSession: %s refused (%s)", label.c_str(), qUtf8Printable(reason));
        });
    if (!request.valid() && mediaToken.has_value() && epoch == mpv_->sourceEpoch() &&
        presenter_ != nullptr) {
        // A command the transport refused will never report FILE_LOADED, so the
        // gate the submission armed is dropped - for the current load only.
        presenter_->noteLoadRefused(epoch);
    }
    if (request.valid() && mediaToken.has_value()) {
        // Keyed by the reply's own userdata, so the sink can read the entry this
        // exact command created even when the transport drops the callback
        // above: a source superseded before the reply lands still names its
        // entry.
        pendingLoadTokens_.emplace(request.value, LoadIdentity{*mediaToken, epoch});
    }
    if (!request.valid()) {
        // No reply will ever arrive for a command mpv would not take, so the
        // refusal is named here instead of waited for.
        qWarning("VideoSession: the mpv transport did not accept %s", command.c_str());
        if (reportRefusal) {
            emit errorOccurred(tr("%1: the mpv transport did not accept the command")
                                   .arg(QString::fromStdString(label)));
        }
    }
}

bool VideoSession::isPlaying() const { return playing_; }

void VideoSession::setPlaying(bool playing) {
    // Announced, not just recorded: the state pump compares the core against
    // this very flag, so writing it silently would leave the play control on
    // its old glyph.
    if (playing == playing_) return;
    playing_ = playing;
    publishPlayingState(playing_);
}

void VideoSession::setOpening(bool opening) {
    if (opening == opening_) return;
    opening_ = opening;
    if (presenter_ != nullptr) presenter_->requestRepaint();
    emit openingChanged(opening_);
}

void VideoSession::publishPlayingState(bool playing) {
    // The session must not blank the screen or idle-suspend under a playing
    // video. Each session owns one reason, so a paused main player does not
    // release the lock a picture-in-picture window still needs.
    mikai::gui::idleInhibitor().setActiveFor(this, playing);
    emit playingChanged(playing);
}

// ── enhancement chains ────────────────────────────────────────────────────

void VideoSession::applyShaderChainRequest(const QString& shaders) {
    if (mpv_ == nullptr) return;
    // Only the write is submitted here. What the session reports as the chain is
    // mpv's own observed value, from the glsl-shaders observer.
    issueCoreCommand({"set", "glsl-shaders", shaders.toStdString()}, "glsl-shaders",
                     /*reportRefusal=*/true);
}

void VideoSession::setAnime4KChain(const QString& candidateId) {
    if (candidateId == anime4kChainId_) return;
    QString reason;
    const QString shaders = resolvedChainForCurrentSource(candidateId, &reason);
    if (!reason.isEmpty() && candidateId != QLatin1String("off")) {
        emit anime4kGated(reason);
        return;
    }
    ++anime4KRequestGeneration_;
    anime4kChainId_ = candidateId;
    // A named chain supersedes the enum, but the enum is not cleared: a later
    // setAnime4KMode rewrites both.
    if (!mpv_) return;
    if (candidateId != QLatin1String("off") && hdrFixesEnabled() &&
        sourceColorProfile_.requiresSdrGate()) {
        emit anime4kGated(tr("Anime4K skipped: source transfer metadata is unknown or HDR"));
    }
    applyShaderChainRequest(shaders);
    // vo=libmpv rebuilds the filter chain on the next rendered frame, and a
    // paused video produces none.
    refreshRenderState();
}

void VideoSession::setAnime4KMode(anime4k::Mode mode) {
    // The id is compared too: an override the compare window applied must be
    // taken back when the same mode is picked again.
    const QString id = anime4k::modeId(mode);
    if (mode == anime4kMode_ && id == anime4kChainId_) return;
    QString reason;
    const QString shaders = resolvedChainForCurrentSource(id, &reason);
    if (!reason.isEmpty() && id != QLatin1String("off")) {
        emit anime4kGated(reason);
        return;
    }
    ++anime4KRequestGeneration_;
    anime4kMode_ = mode;
    // The id is the persistence key, so it is recorded here too.
    anime4kChainId_ = id;
    if (!mpv_) return;
    if (id != QLatin1String("off") && hdrFixesEnabled() &&
        sourceColorProfile_.requiresSdrGate()) {
        emit anime4kGated(tr("Anime4K skipped: source transfer metadata is unknown or HDR"));
    }
    applyShaderChainRequest(shaders);
    refreshRenderState();
}

QString VideoSession::effectiveShaderChain() const {
    if (!mpv_) return QString();
    // mpv only echoes this back - it does not validate a shader path list - so
    // no answer separates a working chain from a preset naming a deleted file.
    return shaderChain_;
}

QString VideoSession::resolvedChainForCurrentSource(const QString& candidateId,
                                                    QString* reason) const {
    const QString shaders = anime4k::shaderStringForCandidate(candidateId, QString(), reason);
    if (shaders.isEmpty()) return shaders;  // Off, unknown, or an unavailable required stage
    if (hdrFixesEnabled() && sourceColorProfile_.requiresSdrGate()) return QString();
    return shaders;
}

void VideoSession::handleSourceColorProfileChanged() {
    if (observedColorProfile_.pixelformat == sourceColorProfile_.pixelformat &&
        observedColorProfile_.primaries == sourceColorProfile_.primaries &&
        observedColorProfile_.gamma == sourceColorProfile_.gamma) {
        return;
    }
    const bool wasGateRequired = sourceColorProfile_.requiresSdrGate();
    const bool wasHdr = sourceColorProfile_.isHdr();
    sourceColorProfile_ = observedColorProfile_;
    const bool requiresSdrGate = sourceColorProfile_.requiresSdrGate();
    const bool hdr = sourceColorProfile_.isHdr();
    if (requiresSdrGate != wasGateRequired || hdr != wasHdr) {
        const QString chainId =
            anime4kChainId_.isEmpty() ? anime4k::modeId(anime4kMode_) : anime4kChainId_;
        QString resolveReason;
        const QString requested =
            anime4k::shaderStringForCandidate(chainId, QString(), &resolveReason);
        if (!resolveReason.isEmpty() && chainId != QLatin1String("off")) {
            emit anime4kGated(resolveReason);
        } else if (requiresSdrGate != wasGateRequired && !requested.isEmpty() &&
                   mpv_ != nullptr) {
            QString reason;
            const QString allowed = resolvedChainForCurrentSource(chainId, &reason);
            if (!reason.isEmpty()) {
                emit anime4kGated(reason);
            } else {
                if (allowed.isEmpty()) {
                    emit anime4kGated(
                        tr("Anime4K skipped: source transfer metadata is unknown or HDR"));
                }
                applyShaderChainRequest(allowed);
            }
            // A paused core produces no frame to rebuild the chain on.
            refreshRenderState();
        }
        if (requiresSdrGate && !(hdr && animeJanaiHdrOverride())) {
            enforceAnimeJanaiHdrGate();
        } else if (animeJanaiSourceGated_) {
            animeJanaiSourceGated_ = false;
            setAnimeJanaiStatus(QStringLiteral("Configured; waiting for a loaded source"));
            if (mpv_ != nullptr && !url_.isEmpty() && !pendingOpen_) processAnimeJanaiPlan();
        }
    }
    emit sourceColorProfileChanged(sourceColorProfile_);
    videoMetadata_.isHdr = sourceColorProfile_.isHdr();
    videoMetadata_.isDolbyVision = sourceColorProfile_.primaries.contains(QStringLiteral("dolby")) ||
                                   sourceColorProfile_.gamma.contains(QStringLiteral("dolby"));
    if (videoMetadata_.isHdr) {
        if (videoMetadata_.isDolbyVision) {
            videoMetadata_.hdrFormat = QStringLiteral("Dolby Vision");
        } else if (sourceColorProfile_.gamma == QStringLiteral("smpte2084")) {
            videoMetadata_.hdrFormat = QStringLiteral("HDR10");
        } else if (sourceColorProfile_.gamma == QStringLiteral("hlg")) {
            videoMetadata_.hdrFormat = QStringLiteral("HLG");
        } else {
            videoMetadata_.hdrFormat = QStringLiteral("HDR");
        }
    } else {
        videoMetadata_.hdrFormat.clear();
    }
    videoMetadata_.hasDolbyAtmos = videoMetadata_.audioCodec.contains(QStringLiteral("eac3")) ||
                                   videoMetadata_.audioCodec.contains(QStringLiteral("truehd"));
    emit videoMetadataChanged(videoMetadata_);
    if (mpv_ != nullptr && hdrRenderingFixesEnabled()) {
        issueCoreCommand(
            {"set", "target-colorspace-hint", sourceColorProfile_.isHdr() ? "auto" : "no"},
            "target-colorspace-hint", false);
    }
}

void VideoSession::updateVideoMetadata() {
    if (videoMetadata_.videoCodec.contains(QStringLiteral("dvhe")) ||
        videoMetadata_.videoCodec.contains(QStringLiteral("dovi"))) {
        videoMetadata_.isDolbyVision = true;
        videoMetadata_.hdrFormat = QStringLiteral("Dolby Vision");
    }
    videoMetadata_.hasDolbyAtmos = videoMetadata_.audioCodec.contains(QStringLiteral("eac3")) ||
                                   videoMetadata_.audioCodec.contains(QStringLiteral("truehd"));
    emit videoMetadataChanged(videoMetadata_);
}

void VideoSession::enforceAnimeJanaiHdrGate() {
    if (!hdrFixesEnabled() || mpv_ == nullptr ||
        !sourceColorProfile_.requiresSdrGate() ||
        (sourceColorProfile_.isHdr() && animeJanaiHdrOverride()) || !animeJanaiPlan_.enabled) {
        return;
    }
    animeJanaiSourceGated_ = true;
    const QString notice = sourceColorProfile_.isHdr()
                               ? tr("Skipped on HDR: native stages force 8-bit NV12")
                               : tr("Skipped: source transfer metadata is unknown");
    if (!animeJanaiFilterBound_ && !animeJanaiEngineBuildActive_ && !animeJanaiBusy_) {
        setAnimeJanaiStatus(notice);
        return;
    }
    const uint64_t epoch = mpv_->sourceEpoch();
    cancelAnimeJanaiOperation();
    const quint64 generation = animeJanaiGeneration_;
    animeJanaiBusy_ = true;
    removeAnimeJanaiFilters(epoch, generation, [this, epoch, generation, notice] {
        animeJanaiBusy_ = false;
        if (animeJanaiStageBuildLease_) animeJanaiStageBuildLease_->release();
        animeJanaiStageBuildLease_.reset();
        finishAnimeJanaiEngineBuild(false, tr("Enhancement engine build stopped for this source."));
        setEngineFrameHold(false);
        restoreAnimeJanaiHwdec(epoch, generation);
        setAnimeJanaiStatus(notice);
    });
}

void VideoSession::setAnimeJanaiHdrOverride(bool allowed) {
    if (g_animeJanaiHdrOverride == allowed) return;
    g_animeJanaiHdrOverride = allowed;
    ++g_animeJanaiHdrOverrideRevision;
}

bool VideoSession::animeJanaiHdrOverride() { return g_animeJanaiHdrOverride; }

bool VideoSession::hdrRenderingFixesEnabled() { return hdrFixesEnabled(); }

void VideoSession::applyHdrPolicyToEngine() {
    appliedHdrPolicyRevision_ = hdrToneMappingPolicyRevision();
    if (mpv_ == nullptr) return;
    if (!hdrFixesEnabled()) return;
    for (const auto& option : hdrPolicyOptionList(hdrToneMappingPolicy())) {
        // Tolerated: a runtime older than an option refuses that one entry, is
        // named in the warning log, and playback continues unaffected.
        issueCoreCommand({"set", option.first, option.second}, option.first,
                         /*reportRefusal=*/false);
    }
    issueCoreCommand({"set", "target-colorspace-hint-mode", "source"},
                     "target-colorspace-hint-mode", false);
    issueCoreCommand({"set", "target-colorspace-hint", sourceColorProfile_.isHdr() ? "auto" : "no"},
                     "target-colorspace-hint", false);
}

void VideoSession::setEngineFrameHold(bool active) {
    engineFrameHoldRequested_ = active;
    if (presenter_ != nullptr) {
        if (active) {
            // Capture before the filter changes; ordinary playback must not copy
            // every frame.
            presenter_->acquireRetainedPicture();
        } else {
            // The held picture is replaceable only by the next real output frame.
            presenter_->releaseRetainedPicture();
        }
        presenter_->requestRepaint();
    }
}

// ── unfiltered capture ────────────────────────────────────────────────────

QImage VideoSession::grabUnfilteredFrame(QString* reason) {
    if (reason != nullptr) reason->clear();
    if (QThread::currentThread() != thread()) {
        if (reason != nullptr) *reason = QStringLiteral("not the GUI thread");
        return {};
    }
    if (!mpv_ || !hasVisibleOutputFrame()) {
        if (reason != nullptr) *reason = QStringLiteral("no decoded frame to capture");
        return {};
    }
    QString filter;
    QString shaders;
    try {
        filter = QString::fromStdString(mpv_->getString("vf"));
        shaders = QString::fromStdString(mpv_->getString("glsl-shaders"));
    } catch (const MpvError& error) {
        if (reason != nullptr) *reason = QString::fromUtf8(error.what());
        return {};
    }
    const double target = position();
    // Restore is unconditional: a capture that fails must not leave playback
    // without its processing chain.
    struct Restore {
        VideoSession* session;
        const QString filter;
        const QString shaders;
        ~Restore() {
            // The core can be gone between the capture and this restore; the new
            // core has no filter to restore anyway.
            if (session->mpv_) {
                try {
                    session->mpv_->setString("vf", filter.toStdString());
                    session->mpv_->setString("glsl-shaders", shaders.toStdString());
                } catch (const MpvError&) {
                }
            }
            session->refreshRenderState();
        }
    } restore{this, filter, shaders};

    try {
        // Toggling a filter that is not there is pure risk: a core with nothing
        // bound needs no teardown.
        if (!filter.isEmpty() || !filter.trimmed().isEmpty()) mpv_->setString("vf", "");
        if (!shaders.isEmpty()) mpv_->setString("glsl-shaders", "");
    } catch (const MpvError& error) {
        if (reason != nullptr) *reason = QString::fromUtf8(error.what());
        return {};
    }
    // A paused core does not decode on its own: a small seek back and forward is
    // what makes mpv produce a frame through the now-empty filter chain.
    const auto waitForFrame = [this](double seconds) {
        seek(seconds);
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 2500) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            if (hasVisibleOutputFrame()) return true;
            QThread::msleep(10);
        }
        return hasVisibleOutputFrame();
    };
    if (!waitForFrame(target > 0.1 ? target - 0.1 : 0.0) || !waitForFrame(target)) {
        if (reason != nullptr) *reason = QStringLiteral("the decoder produced no frame");
        return {};
    }
    const QImage image = grabVideoFramebuffer();
    if (image.isNull() && reason != nullptr) *reason = QStringLiteral("the framebuffer was empty");
    return image;
}

void VideoSession::grabUnfilteredFrameAsync(std::function<void(QImage, QString)> completion) {
    if (!completion) return;
    if (QThread::currentThread() != thread()) {
        completion({}, QStringLiteral("not the GUI thread"));
        return;
    }
    if (!mpv_ || !hasVisibleOutputFrame()) {
        completion({}, QStringLiteral("no decoded frame to capture"));
        return;
    }
    // One capture at a time: a second call completes at once instead of queuing
    // behind the first, whose filter restore it would otherwise race.
    if (unfilteredCapture_ != nullptr) {
        completion({}, QStringLiteral("an unfiltered capture is already running"));
        return;
    }
    auto state = std::make_shared<UnfilteredCapture>();
    state->session = QPointer<VideoSession>(this);
    state->completion = std::move(completion);
    state->epoch = mpv_->sourceEpoch();
    state->target = position();
    unfilteredCapture_ = state;

    // Filter properties travel the async transport: a synchronous read takes
    // the core lock, which native work may be holding. Both reads must land
    // before anything is cleared, so the restore knows what to put back.
    state->readsPending = 2;
    const auto noteReadIssued = [this, state](AsyncRequestId request) {
        if (request.valid()) return;
        state->readFailed = true;
        if (--state->readsPending == 0) onUnfilteredFiltersRead(state);
    };
    noteReadIssued(mpv_->getPropertyAsync(
        "vf", MPV_FORMAT_STRING, [this, state](const AsyncReply& reply) {
            if (!unfilteredCaptureActive(state)) return;
            if (!reply.ok() || reply.format != MPV_FORMAT_STRING) {
                state->readFailed = true;
            } else {
                state->filter = QString::fromStdString(reply.text);
            }
            if (--state->readsPending == 0) onUnfilteredFiltersRead(state);
        }));
    noteReadIssued(mpv_->getPropertyAsync(
        "glsl-shaders", MPV_FORMAT_STRING, [this, state](const AsyncReply& reply) {
            if (!unfilteredCaptureActive(state)) return;
            if (!reply.ok() || reply.format != MPV_FORMAT_STRING) {
                state->readFailed = true;
            } else {
                state->shaders = QString::fromStdString(reply.text);
            }
            if (--state->readsPending == 0) onUnfilteredFiltersRead(state);
        }));
}

bool VideoSession::unfilteredCaptureActive(
    const std::shared_ptr<UnfilteredCapture>& state) const {
    return state != nullptr && !state->finished && state->session != nullptr &&
           unfilteredCapture_.get() == state.get() && mpv_ != nullptr &&
           state->epoch == mpv_->sourceEpoch();
}

void VideoSession::onUnfilteredFiltersRead(const std::shared_ptr<UnfilteredCapture>& state) {
    if (!unfilteredCaptureActive(state)) return;
    if (state->readFailed) {
        finishUnfilteredCapture(state, {}, tr("the video filter chain could not be read"));
        return;
    }
    // Toggling a filter that is not there is pure risk.
    const bool clearFilter = !state->filter.isEmpty() || !state->filter.trimmed().isEmpty();
    const bool clearShaders = !state->shaders.isEmpty();
    state->setsPending = (clearFilter ? 1 : 0) + (clearShaders ? 1 : 0);
    if (state->setsPending == 0) {
        onUnfilteredFiltersCleared(state);
        return;
    }
    const auto onFilterCleared = [this, state](const AsyncReply& reply) {
        if (!unfilteredCaptureActive(state)) return;
        if (!reply.ok()) state->setFailed = true;
        if (--state->setsPending == 0) onUnfilteredFiltersCleared(state);
    };
    const auto noteSetIssued = [this, state](AsyncRequestId request) {
        if (request.valid()) return;
        state->setFailed = true;
        if (--state->setsPending == 0) onUnfilteredFiltersCleared(state);
    };
    if (clearFilter) {
        state->clearedFilter = true;
        noteSetIssued(mpv_->commandAsync({"set", "vf", ""}, onFilterCleared));
    }
    if (clearShaders) {
        state->clearedShaders = true;
        noteSetIssued(mpv_->commandAsync({"set", "glsl-shaders", ""}, onFilterCleared));
    }
}

void VideoSession::onUnfilteredFiltersCleared(
    const std::shared_ptr<UnfilteredCapture>& state) {
    if (!unfilteredCaptureActive(state)) return;
    if (state->setFailed) {
        finishUnfilteredCapture(state, {}, tr("the video filter chain could not be removed"));
        return;
    }
    // A paused core does not decode on its own: a small seek back and forward is
    // what makes mpv produce a frame through the now-empty filter chain. Both
    // seeks are preserved, in order.
    waitUnfilteredFrame(state, state->target > 0.1 ? state->target - 0.1 : 0.0);
}

void VideoSession::waitUnfilteredFrame(const std::shared_ptr<UnfilteredCapture>& state,
                                       double seekPosition) {
    if (!unfilteredCaptureActive(state)) return;
    state->expectedPosition = seekPosition;
    state->framesBeforeSeek = presentedFrames_;
    state->waitPhase += 1;
    if (state->timer == nullptr) {
        state->timer = new QTimer(this);
        state->timer->setSingleShot(true);
        const std::weak_ptr<UnfilteredCapture> weakState = state;
        connect(state->timer.data(), &QTimer::timeout, this, [this, weakState] {
            if (auto held = weakState.lock()) {
                if (held->session != nullptr) {
                    finishUnfilteredCapture(held, {}, tr("the decoder produced no frame"));
                }
            }
        });
    }
    state->timer->start(kUnfilteredFrameWaitMs);
    // The presentation report and the playhead observation can arrive in either
    // order, so both handlers re-check this state.
    seek(seekPosition);
}

void VideoSession::checkUnfilteredFramePresented(
    const std::shared_ptr<UnfilteredCapture>& state) {
    if (state == nullptr || state->finished) return;
    if (mpv_ == nullptr || state->epoch != mpv_->sourceEpoch()) {
        finishUnfilteredCapture(state, {}, tr("the source changed during capture"));
        return;
    }
    if (!unfilteredCaptureActive(state)) return;
    // A new frame must have been presented since the seek, and the observed
    // playhead must be near the seek target: an arbitrary repaint of an older
    // frame is not proof the paused core decoded anything new.
    if (presentedFrames_ <= state->framesBeforeSeek) return;
    if (std::abs(position() - state->expectedPosition) > kUnfilteredFramePositionToleranceSec) {
        return;
    }
    if (state->timer != nullptr) state->timer->stop();
    if (state->waitPhase == 1) {
        waitUnfilteredFrame(state, state->target);
        return;
    }
    captureUnfilteredFrame(state);
}

void VideoSession::captureUnfilteredFrame(const std::shared_ptr<UnfilteredCapture>& state) {
    if (!unfilteredCaptureActive(state)) return;
    // The pixels arrive through the async screenshot reply: the same
    // `screenshot-raw` mechanism FramePreviewService and CompareRenderer use on
    // their vo=libmpv cores - no second screenshot path. The reply is parsed
    // here rather than through previewFrameFromRaw, which refuses frames wider
    // than 1024 px: a player frame is full-size.
    const AsyncRequestId request = mpv_->commandAsync(
        {"screenshot-raw", "video", "rgba"},
        [this, state](const AsyncReply& reply) {
            if (!unfilteredCaptureActive(state)) return;
            if (!reply.ok() || reply.node.format != MPV_FORMAT_NODE_MAP) {
                finishUnfilteredCapture(state, {}, tr("the frame could not be captured"));
                return;
            }
            const NodeValue* width = nullptr;
            const NodeValue* height = nullptr;
            const NodeValue* stride = nullptr;
            const NodeValue* format = nullptr;
            const NodeValue* data = nullptr;
            for (const auto& entry : reply.node.map) {
                if (entry.first == "w") width = &entry.second;
                else if (entry.first == "h") height = &entry.second;
                else if (entry.first == "stride") stride = &entry.second;
                else if (entry.first == "format") format = &entry.second;
                else if (entry.first == "data") data = &entry.second;
            }
            if (width == nullptr || height == nullptr || stride == nullptr ||
                format == nullptr || data == nullptr) {
                finishUnfilteredCapture(state, {},
                                        tr("the frame capture did not report its pixels"));
                return;
            }
            // qint64, not qsizetype: `stride * height` overflows a 32-bit
            // qsizetype before the comparison could reject it.
            const qint64 declaredWidth = width->integer;
            const qint64 declaredHeight = height->integer;
            const qint64 declaredStride = stride->integer;
            if (declaredWidth <= 0 || declaredHeight <= 0 ||
                declaredStride < declaredWidth * 4 || format->text != "rgba" ||
                static_cast<qint64>(data->bytes.size()) < declaredStride * declaredHeight) {
                finishUnfilteredCapture(state, {},
                                        tr("the captured frame could not be read"));
                return;
            }
            // copy(): the reply's storage dies with the next drain.
            const QImage frame =
                QImage(reinterpret_cast<const uchar*>(data->bytes.data()),
                       static_cast<int>(declaredWidth), static_cast<int>(declaredHeight),
                       static_cast<qsizetype>(declaredStride), QImage::Format_RGBA8888)
                    .copy();
            if (frame.isNull()) {
                finishUnfilteredCapture(state, {}, tr("the captured frame could not be read"));
                return;
            }
            finishUnfilteredCapture(state, frame, {});
        });
    if (!request.valid()) {
        finishUnfilteredCapture(state, {}, tr("the player would not take the capture request"));
    }
}

void VideoSession::finishUnfilteredCapture(const std::shared_ptr<UnfilteredCapture>& state,
                                           const QImage& image, const QString& reason) {
    if (state == nullptr || state->finished) return;
    state->finished = true;
    if (state->timer != nullptr) {
        state->timer->stop();
        state->timer->deleteLater();
        state->timer = nullptr;
    }
    if (unfilteredCapture_.get() == state.get()) unfilteredCapture_.reset();
    restoreUnfilteredFilters(state);
    // The captured content is not what is on screen now, so a normal paint is
    // asked for rather than leaving the view showing the capture.
    if (presenter_ != nullptr) presenter_->requestRepaint();
    if (state->completion) state->completion(image, reason);
}

void VideoSession::restoreUnfilteredFilters(
    const std::shared_ptr<UnfilteredCapture>& state) {
    auto* session = state->session.data();
    if (session == nullptr || session->mpv_ == nullptr ||
        state->epoch != session->mpv_->sourceEpoch()) {
        return;
    }
    // Only restore what this capture cleared, and only while the chain is still
    // the cleared one: a filter change made mid-capture belongs to its owner.
    const auto restoreIfCleared = [state](const char* property, const QString& saved) {
        auto* target = state->session.data();
        if (target == nullptr || target->mpv_ == nullptr) return;
        target->mpv_->getPropertyAsync(
            property, MPV_FORMAT_STRING,
            [state, property, saved](const AsyncReply& reply) {
                auto* live = state->session.data();
                if (live == nullptr || live->mpv_ == nullptr ||
                    state->epoch != live->mpv_->sourceEpoch() || !reply.ok() ||
                    reply.format != MPV_FORMAT_STRING || !reply.text.empty()) {
                    return;
                }
                live->mpv_->commandAsync(
                    {"set", property, saved.toStdString()},
                    [state, property](const AsyncReply& answer) {
                        if (answer.ok()) return;
                        auto* restoreTarget = state->session.data();
                        if (restoreTarget == nullptr || restoreTarget->mpv_ == nullptr) return;
                        qWarning("VideoSession: the unfiltered capture could not restore "
                                 "%s (%s)",
                                 property,
                                 qUtf8Printable(QString::fromStdString(
                                     restoreTarget->mpv_->describeError(answer.error))));
                    });
            });
    };
    if (state->clearedFilter) restoreIfCleared("vf", state->filter);
    if (state->clearedShaders) restoreIfCleared("glsl-shaders", state->shaders);
}

QImage VideoSession::grabVideoFramebuffer() {
    if (QThread::currentThread() != thread()) {
        qWarning("VideoSession: grabVideoFramebuffer() is GUI thread only");
        return QImage();
    }
    if (presenter_ == nullptr) return QImage();
    QString reason;
    // The presenter's readback is video-only by contract: chrome never reaches a
    // capture consumer through this path.
    const QImage image = presenter_->readVideoFrame(&reason);
    if (!reason.isEmpty()) qWarning("VideoSession: %s", qUtf8Printable(reason));
    // The captured content is not what is on screen now, so a normal paint is
    // asked for rather than leaving the view showing the capture.
    presenter_->requestRepaint();
    return image;
}

// ── probes / tracks ───────────────────────────────────────────────────────

std::optional<ProbePropertyValue> VideoSession::probeMpvProperty(const QString& name,
                                                                 ProbePropertyKind want) {
    if (mpv_ == nullptr) return std::nullopt;
    // Never a synchronous property read: that takes the core lock, which native
    // work can be holding. The read travels the asynchronous transport and is
    // waited for here with a bound, pumping Qt so render callbacks are not
    // starved. A nested probe is refused rather than answered from a queue this
    // call is already draining.
    if (probeReadActive_) return std::nullopt;
    const mpv_format format = probePropertyFormat(want);
    // Shared: a read abandoned at the deadline is answered later, on the pump's
    // drain, and that answer must land somewhere alive.
    struct Answer {
        bool done = false;
        AsyncReply reply;
    };
    auto answer = std::make_shared<Answer>();
    // Pumping Qt events can run anything, including this session's teardown, so
    // the session is guarded rather than assumed to outlive the wait.
    QPointer<VideoSession> self(this);
    const uint64_t epoch = mpv_->sourceEpoch();
    probeReadActive_ = true;
    const AsyncRequestId request =
        mpv_->getPropertyAsync(name.toStdString(), format,
                               [answer](const AsyncReply& reply) {
                                   answer->reply = reply;
                                   answer->done = true;
                               });
    QElapsedTimer clock;
    clock.start();
    while (request.valid() && !answer->done && clock.elapsed() < kProbeReadTimeoutMs) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 5);
        if (self.isNull()) return std::nullopt;
        mpv_->drainEvents();
    }
    if (self.isNull()) return std::nullopt;
    probeReadActive_ = false;
    // Retired, so a late reply runs no callback and the probe reports "no answer"
    // rather than an answer that belongs to a source already replaced.
    if (!answer->done || epoch != mpv_->sourceEpoch()) {
        mpv_->cancelAsync(request);
        return std::nullopt;
    }
    ProbePropertyValue value;
    if (!probeValueFromReply(answer->reply, want, value)) return std::nullopt;
    return value;
}

AsyncRequestId VideoSession::requestPlaybackTimestamp(
    std::function<void(uint64_t, std::optional<double>)> callback) {
    if (mpv_ == nullptr || !callback) return {};
    const uint64_t epoch = mpv_->sourceEpoch();
    return mpv_->getPropertyAsync(
        "time-pos", MPV_FORMAT_DOUBLE,
        [this, epoch, callback = std::move(callback)](const AsyncReply& reply) mutable {
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch() || !reply.ok() ||
                reply.format != MPV_FORMAT_DOUBLE) {
                callback(epoch, std::nullopt);
                return;
            }
            callback(epoch, reply.number);
        });
}

bool VideoSession::probeCycleMpvTrack(const QString& property) {
    if (mpv_ == nullptr ||
        (property != QStringLiteral("aid") && property != QStringLiteral("sid"))) {
        return false;
    }
    return mpv_->commandAsync({"cycle", property.toStdString()},
                              [](const AsyncReply&) {}).valid();
}

void VideoSession::setSubtitleDelay(double seconds) {
    if (mpv_ == nullptr) return;
    try {
        mpv_->setNumber("sub-delay", seconds);
    } catch (const MpvError&) {
        // A core that cannot take the property is a core with no subtitles.
    }
}

bool VideoSession::addExternalTrack(const QString& file, bool subtitle, QString* reason) {
    if (reason != nullptr) reason->clear();
    const auto fail = [reason](const QString& text) {
        if (reason != nullptr) *reason = text;
        return false;
    };
    if (mpv_ == nullptr) return fail(QStringLiteral("no playback core"));
    if (file.isEmpty() || !QFileInfo::exists(file)) return fail(QStringLiteral("file not found"));
    try {
        const AsyncRequestId request = mpv_->commandAsync(
            {subtitle ? "sub-add" : "audio-add", file.toStdString(), "select"},
            [](const AsyncReply&) {});
        if (!request.valid()) return fail(QStringLiteral("the core rejected the request"));
    } catch (const MpvError& error) {
        return fail(QString::fromUtf8(error.what()));
    }
    return true;
}

void VideoSession::setNextMediaTrackSelection(MediaTrackSelection selection) {
    nextMediaTrackSelection_ = std::move(selection);
}

void VideoSession::resetMediaTracks() {
    cancelMediaInformation();
    emit mediaInformationInvalidated();
    if (mpv_ != nullptr && mediaTracksRequest_.valid()) mpv_->cancelAsync(mediaTracksRequest_);
    mediaTracksRequest_ = {};
    mediaTracksLoaded_ = false;
    mediaTracksDirty_ = false;
    const MediaTrackSnapshot cleared{mpv_ != nullptr ? mpv_->coreIdentity() : 0,
                                     mpv_ != nullptr ? mpv_->sourceEpoch() : 0, false, {}};
    if (mediaTracks_ == cleared) return;
    mediaTracks_ = cleared;
    emit mediaTracksChanged();
}

void VideoSession::refreshMediaTracks() {
    if (mpv_ == nullptr || !mediaTracksLoaded_ || !mediaTracksDirty_ ||
        mediaTracksRequest_.valid() || entrySourceEpoch_ != mpv_->sourceEpoch()) return;
    mediaTracksDirty_ = false;
    const uint64_t identity = mpv_->coreIdentity();
    const uint64_t epoch = mpv_->sourceEpoch();
    mediaTracksRequest_ = mpv_->getPropertyAsync(
        "track-list", MPV_FORMAT_NODE, [this, identity, epoch](const AsyncReply& reply) {
            mediaTracksRequest_ = {};
            if (mpv_ == nullptr || identity != mpv_->coreIdentity() ||
                epoch != mpv_->sourceEpoch() || !mediaTracksLoaded_ ||
                entrySourceEpoch_ != epoch) return;
            QString reason;
            auto tracks = reply.ok() ? parseMediaTracks(reply.node, &reason) : std::nullopt;
            if (!tracks.has_value()) {
                const MediaTrackSnapshot unavailable{identity, epoch, false, {}};
                if (mediaTracks_ != unavailable) {
                    mediaTracks_ = unavailable;
                    emit mediaTracksChanged();
                }
                if (!reply.ok()) reason = QString::fromStdString(mpv_->describeError(reply.error));
                emit mediaTracksError(tr("Cannot read audio/subtitle tracks: %1").arg(reason));
                return;
            }
            MediaTrackSnapshot snapshot{identity, epoch, true, std::move(*tracks)};
            const MediaTrackSelection observed = selectedMediaTracks(snapshot);
            if (observed.audio == pendingMediaTrackSelection_.audio)
                pendingMediaTrackSelection_.audio.reset();
            if (observed.subtitle == pendingMediaTrackSelection_.subtitle)
                pendingMediaTrackSelection_.subtitle.reset();
            if (snapshot == mediaTracks_) return;
            mediaTracks_ = std::move(snapshot);
            emit mediaTracksChanged();
        });
    if (!mediaTracksRequest_.valid()) {
        emit mediaTracksError(tr("The playback core did not accept the track discovery request."));
    }
}

bool VideoSession::selectMediaTrack(MediaTrackType type, int64_t id,
                                    uint64_t expectedCoreIdentity,
                                    uint64_t expectedSourceEpoch) {
    if (mpv_ == nullptr || !mediaTracks_.ready || expectedCoreIdentity != mpv_->coreIdentity() ||
        expectedSourceEpoch != mpv_->sourceEpoch() ||
        mediaTracks_.coreIdentity != expectedCoreIdentity ||
        mediaTracks_.sourceEpoch != expectedSourceEpoch) {
        emit mediaTracksError(tr("The media changed. Reopen the track menu."));
        return false;
    }
    const bool available =
        id == 0 || std::any_of(mediaTracks_.tracks.cbegin(), mediaTracks_.tracks.cend(),
                               [type, id](const MediaTrack& track) {
                                   return track.type == type && track.id == id;
                               });
    if (!available) {
        emit mediaTracksError(tr("The selected track is no longer available."));
        return false;
    }
    const char* property = type == MediaTrackType::Audio ? "aid" : "sid";
    const AsyncRequestId request = mpv_->commandAsync(
        {"set", property, id == 0 ? std::string("no") : std::to_string(id)},
        [this, type, id](const AsyncReply& reply) {
            if (!reply.ok()) {
                auto& pending = type == MediaTrackType::Audio
                                    ? pendingMediaTrackSelection_.audio
                                    : pendingMediaTrackSelection_.subtitle;
                if (pending == id) pending.reset();
                emit mediaTracksError(
                    tr("Cannot select %1: %2")
                        .arg(type == MediaTrackType::Audio ? tr("audio track")
                                                           : tr("subtitle track"),
                             QString::fromStdString(mpv_->describeError(reply.error))));
                return;
            }
            mediaTracksDirty_ = true;
        });
    if (request.valid()) {
        auto& pending = type == MediaTrackType::Audio ? pendingMediaTrackSelection_.audio
                                                      : pendingMediaTrackSelection_.subtitle;
        pending = id;
        return true;
    }
    emit mediaTracksError(tr("The playback core did not accept the track selection request."));
    return false;
}

// ── display scaling policy ────────────────────────────────────────────────

void VideoSession::setVideoScalePolicy(const VideoScalePolicy& policy) {
    VideoScalePolicy normalized = policy;
    normalized.zoomPercent = clampedVideoZoomPercent(policy.zoomPercent);
    if (normalized == g_videoScalePolicy) return;
    g_videoScalePolicy = normalized;
    ++g_videoScalePolicyRevision;
    // Every live session adopts the new revision on its own pump tick, so this
    // setter never has to know how many sessions exist.
}

VideoScalePolicy VideoSession::videoScalePolicy() { return g_videoScalePolicy; }

void VideoSession::setVideoScaleMode(VideoScaleMode mode) {
    VideoScalePolicy policy = g_videoScalePolicy;
    policy.mode = mode;
    setVideoScalePolicy(policy);
}

int VideoSession::videoScalePolicyRevision() { return g_videoScalePolicyRevision; }

std::optional<std::string> VideoSession::downscaleDefaultForCore() {
    if (downscaleDefault_.has_value()) return downscaleDefault_;
    // A failure is remembered: a second read cannot succeed where the first
    // failed, and the pump must never take the core lock to find that out.
    if (downscaleDefaultError_.has_value()) return std::nullopt;
    if (mpv_ == nullptr) {
        downscaleDefaultError_ = std::string("the core is not initialized");
        return std::nullopt;
    }
    try {
        downscaleDefault_ = mpv_->getString("option-info/dscale/default-value");
    } catch (const MpvError& error) {
        downscaleDefaultError_ = error.what();
        return std::nullopt;
    }
    return downscaleDefault_;
}

void VideoSession::applyVideoScaleToEngine(bool queryCoreDefault) {
    appliedVideoScaleRevision_ = videoScalePolicyRevision();
    if (mpv_ == nullptr) return;
    VideoScalePolicy policy = g_videoScalePolicy;
    if (compareFitActive_) {
        // Compare compares the picture, not the viewer's window: Fit with no
        // zoom keeps both panes the same shape whatever the saved mode is.
        policy.mode = VideoScaleMode::Fit;
        policy.zoomPercent = 0;
    }
    // The core's own default is read once, at creation and whatever the saved
    // policy says, so a later switch back to Auto can restore a real id. Only
    // creation may take the core lock for that read; the pump uses the cache.
    std::string downscaleDefault;
    if (queryCoreDefault) {
        if (const auto resolved = downscaleDefaultForCore(); resolved.has_value()) {
            downscaleDefault = *resolved;
        }
    } else if (downscaleDefault_.has_value()) {
        downscaleDefault = *downscaleDefault_;
    }
    if (policy.downscale.isEmpty() && downscaleDefault.empty() && downscaleWasExplicit_) {
        // The engine still holds the explicit id this session wrote and its real
        // default is unreadable: no id may be guessed and the literal "auto" is
        // refused, so the failure is named instead of leaving the substitute in
        // place unspoken.
        emit displayScalingRefused(
            tr("dscale: the core's default downscaler could not be read (%1). The previous "
               "downscaler stays in effect; choose one explicitly or reopen the player.")
                .arg(QString::fromStdString(
                    downscaleDefaultError_.value_or("no default value reported"))));
    }
    const std::vector<VideoScaleOption> writes = videoScaleOptions(policy, downscaleDefault);
    // Redraw after all accepted writes reply; an older apply cannot complete a
    // newer batch or leave a paused view displaying its old snapshot.
    const quint64 batch = ++videoScaleBatch_;
    videoScaleBatchPending_ = 0;
    for (const VideoScaleOption& option : writes) {
        // Issued here rather than through issueCoreCommand: that helper reports a
        // refusal through errorOccurred, which the player treats as a failed
        // source. A scaler id the runtime does not know must name itself without
        // claiming the stream failed.
        const std::string name = option.name;
        const AsyncRequestId request = mpv_->commandAsync(
            {"set", name, option.value},
            [this, name, batch](const AsyncReply& reply) {
                if (mpv_ == nullptr) return;
                QString refusal;
                if (!reply.ok()) {
                    const QString reason =
                        QString::fromStdString(mpv_->describeError(reply.error));
                    qWarning("VideoSession: %s refused (%s)", name.c_str(),
                             qUtf8Printable(reason));
                    refusal = tr("%1: %2").arg(QString::fromStdString(name), reason);
                }
                if (batch == videoScaleBatch_ && --videoScaleBatchPending_ == 0) {
                    refreshRenderState();
                }
                // A receiver can destroy this session while reporting.
                if (!refusal.isEmpty()) emit displayScalingRefused(refusal);
            });
        // commandAsync drops rejected submissions without invoking their callback.
        if (request.valid()) ++videoScaleBatchPending_;
    }
    // What the engine now holds, not what was asked for.
    if (!policy.downscale.isEmpty()) {
        downscaleWasExplicit_ = true;
    } else if (!downscaleDefault.empty()) {
        downscaleWasExplicit_ = false;
    }
    emit videoScaleStateChanged();
}

std::optional<VideoScaleFitLock> VideoSession::acquireVideoScaleFitLock() {
    if (compareFitActive_) return std::nullopt;
    compareFitActive_ = true;
    applyVideoScaleToEngine();
    return VideoScaleFitLock(this, true);
}

void VideoSession::setHdrToneMappingPolicy(HdrToneMappingPolicy policy) {
    if (policy == g_hdrToneMappingPolicy) return;
    g_hdrToneMappingPolicy = policy;
    ++g_hdrToneMappingPolicyRevision;
    // The pump of every live session adopts the new revision on its own tick.
}

VideoSession::HdrToneMappingPolicy VideoSession::hdrToneMappingPolicy() {
    return g_hdrToneMappingPolicy;
}

void VideoSession::setFrameTimingLogPath(const QString& path) {
    // One process-wide writer lives in the render bridge; the session only
    // forwards, so both adapters and every session share the same storage.
    mplayer::setRenderFrameTimingLogPath(path);
    // The frame-timing log prints these counters, so they must be collected for
    // that launch even when MIKAI_UI_PROFILE was not set.
    mikai::gui::uiprofile::forceCounters(!path.isEmpty());
}

QString VideoSession::frameTimingLogPath() { return mplayer::renderFrameTimingLogPath(); }

void VideoSession::logFrameTiming(const QString& line) {
    mplayer::appendRenderFrameTimingLog(line);
}

// ── reattach ──────────────────────────────────────────────────────────────

void VideoSession::beginContextReattach() {
    // mpv says it has no file loaded: there is no picture to wait for, so the
    // load is the only path back and it is taken once, right here. Its forced
    // render happens after the submission, in finishContextReattach.
    if (mpv_ == nullptr || observedIdle_) {
        finishContextReattach();
        return;
    }
    reattachPending_ = true;
    reattachTicksLeft_ = kContextReattachTicks;
    reattachTimer_.start();
    // The window only proves a live core by painting: a recreated renderer has
    // no video output until a pass consumes, so one is forced here and each
    // tick asks for another pass.
    refreshRenderState();
    if (presenter_ != nullptr) presenter_->requestRepaint();
}

void VideoSession::finishContextReattach() {
    reattachTimer_.stop();
    reattachPending_ = false;
    reattachTicksLeft_ = 0;
    if (mpv_ == nullptr || url_.isEmpty() || pendingOpen_) return;
    if (presenter_ != nullptr && presenter_->hasVisibleOutputFrame()) {
        // The recreated context really delivered output (the bridge sets this
        // only for a rendered frame), so the live core is fine: reloading here
        // would reset the playhead under a seek already in flight.
        pendingResumePosition_ = 0.0;
        pendingResumePositionValid_ = false;
        return;
    }
    // The old video output died with its render context, so the filter it carried
    // is no longer in the core: the reload is what rebuilds it.
    animeJanaiFilterBound_ = false;
    nativeFilterBound_ = false;
    MediaTrackSelection selection = mediaTrackSelection();
    if (pendingMediaTrackSelection_.audio.has_value())
        selection.audio = pendingMediaTrackSelection_.audio;
    if (pendingMediaTrackSelection_.subtitle.has_value())
        selection.subtitle = pendingMediaTrackSelection_.subtitle;
    setNextMediaTrackSelection(selection);
    // The position the viewer left and the pause intent they set both survive.
    const bool restorePaused = requestedPause() || enginePauseHeld();
    // Only the captured offset can be trusted here: this reload is issued up to
    // two thirds of a second after the context went, and by then the dead output
    // has already reported 0 through the property pump.
    startPendingUrl(resumePositionForReload());
    if (restorePaused) reassertPaused();
}

// ── state pump ────────────────────────────────────────────────────────────

void VideoSession::observePlaybackState() {
    if (!mpv_) return;
    if (!observers_.empty()) return;
    // The registered format is what mpv delivers, so each observer reads exactly
    // what it wrote.
    const auto watch = [this](const char* name, mpv_format format, auto&& apply) {
        // A property this core does not have costs the HUD that one number, not
        // playback, so a refusal is reported and stepped over.
        try {
            const uint64_t id = mpv_->observe(
                name, format, [apply = std::forward<decltype(apply)>(apply)](const Property& p) {
                    apply(p);
                });
            if (id != 0) observers_.push_back(id);
        } catch (const MpvError& error) {
            qWarning("mpv: cannot observe %s (%s)", name, error.what());
        }
    };
    watch("idle-active", MPV_FORMAT_FLAG, [this](const Property& p) {
        const bool idle = !p.isFlag() || p.flag;
        if (idle == observedIdle_) return;
        observedIdle_ = idle;
    });
    watch("duration", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedDuration_ = p.isNumber() ? p.number : 0.0; });
    watch("time-pos", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedPosition_ = p.isNumber() ? p.number : 0.0; });
    // The event already carries the flag as mpv holds it. While a write is in
    // flight the only value accepted is the one that was asked for: anything else
    // was produced before the request reached the core.
    watch("pause", MPV_FORMAT_FLAG, [this](const Property& p) {
        const bool paused = p.isFlag() && p.flag;
        if (mpv_ != nullptr && pendingPauseWrite_.has_value()) {
            if (pendingPauseWrite_->epoch != mpv_->sourceEpoch()) {
                // Issued against a source that has since been replaced: nothing
                // confirms it and nothing may be blocked by it.
                pendingPauseWrite_.reset();
            } else if (pendingPauseWrite_->target != paused) {
                // Produced before the request reached the core.
                return;
            } else {
                pendingPauseWrite_.reset();
            }
        }
        if (observedPaused_ != paused) {
            observedPaused_ = paused;
        }
    });
    watch("video-params/w", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedVideoWidth_ = p.isNumber() ? p.number : 0.0; });
    watch("video-params/h", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedVideoHeight_ = p.isNumber() ? p.number : 0.0; });
    // What the picture actually looks like after mpv's filters: dw/dh already
    // carry the sample aspect ratio, while rotate is the intended clockwise turn
    // that has NOT been applied to that output.
    watch("video-out-params/dw", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedOutputWidth_ = p.isNumber() ? p.number : 0.0; });
    watch("video-out-params/dh", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedOutputHeight_ = p.isNumber() ? p.number : 0.0; });
    watch("video-out-params/rotate", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedOutputRotation_ = p.isNumber() ? p.number : 0.0; });
    watch("cache-speed", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedCacheSpeed_ = p.isNumber() ? p.number : 0.0; });
    watch("estimated-vf-fps", MPV_FORMAT_DOUBLE, [this](const Property& p) {
        if (diagnosticsActive_ && p.isNumber()) {
            diagnostics_.estimatedVfFps = p.number;
        } else {
            diagnostics_.estimatedVfFps.reset();
        }
    });
    watch("container-fps", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedContainerFps_ = p.isNumber() ? p.number : 0.0; });
    watch("glsl-shaders", MPV_FORMAT_STRING,
          [this](const Property& p) { shaderChain_ = QString::fromStdString(p.text); });
    // T-102: the source's own color facts, staged (observedColorProfile_) and
    // published through one diffing handler. They arrive as ordinary property
    // events in the same drain as everything else.
    watch("video-params/gamma", MPV_FORMAT_STRING, [this](const Property& p) {
        const QString gamma = QString::fromStdString(p.text).toLower();
        if (gamma == observedColorProfile_.gamma) return;
        observedColorProfile_.gamma = gamma;
        handleSourceColorProfileChanged();
    });
    watch("video-params/primaries", MPV_FORMAT_STRING, [this](const Property& p) {
        const QString primaries = QString::fromStdString(p.text).toLower();
        if (primaries == observedColorProfile_.primaries) return;
        observedColorProfile_.primaries = primaries;
        handleSourceColorProfileChanged();
    });
    watch("video-params/pixelformat", MPV_FORMAT_STRING, [this](const Property& p) {
        const QString pixelformat = QString::fromStdString(p.text).toLower();
        if (pixelformat == observedColorProfile_.pixelformat) return;
        observedColorProfile_.pixelformat = pixelformat;
        handleSourceColorProfileChanged();
    });
    watch("video-codec", MPV_FORMAT_STRING, [this](const Property& p) {
        const QString codec = QString::fromStdString(p.text).toLower();
        if (codec == videoMetadata_.videoCodec) return;
        videoMetadata_.videoCodec = codec;
        updateVideoMetadata();
    });
    watch("audio-codec", MPV_FORMAT_STRING, [this](const Property& p) {
        const QString codec = QString::fromStdString(p.text).toLower();
        if (codec == videoMetadata_.audioCodec) return;
        videoMetadata_.audioCodec = codec;
        updateVideoMetadata();
    });
    for (const char* property : {"track-list", "aid", "sid"}) {
        watch(property, MPV_FORMAT_NONE, [this](const Property&) { mediaTracksDirty_ = true; });
    }
}

void VideoSession::pumpEvents() {
    if (!mpv_) return;
    if (appliedHdrPolicyRevision_ != hdrToneMappingPolicyRevision()) {
        applyHdrPolicyToEngine();
    }
    if (appliedVideoScaleRevision_ != videoScalePolicyRevision()) {
        applyVideoScaleToEngine();
    }
    if (appliedAnimeJanaiOverrideRevision_ != animeJanaiHdrOverrideRevision()) {
        appliedAnimeJanaiOverrideRevision_ = animeJanaiHdrOverrideRevision();
        const bool overrideAllowsSource =
            sourceColorProfile_.isHdr() && animeJanaiHdrOverride();
        if (sourceColorProfile_.requiresSdrGate() && !overrideAllowsSource) {
            enforceAnimeJanaiHdrGate();
        } else if (animeJanaiSourceGated_) {
            animeJanaiSourceGated_ = false;
            setAnimeJanaiStatus(QStringLiteral("Configured; waiting for a loaded source"));
            if (!url_.isEmpty() && !pendingOpen_) processAnimeJanaiPlan();
        }
    }
    // Before the drain, so an observation dispatched from it is read under the
    // epoch the controller is currently bound to.
    syncNativeSource();
    if (animeJanaiEpoch_ != 0 && animeJanaiEpoch_ != mpv_->sourceEpoch()) {
        engineFrameHoldRequested_ = false;
        if (presenter_ != nullptr) presenter_->invalidateRetainedPictures();
        discardEnginePause();
        finishAnimeJanaiEngineBuild(false, tr("The source changed during engine preparation."));
        cancelAnimeJanaiOperation();
        animeJanaiEpoch_ = mpv_->sourceEpoch();
        if (animeJanaiPlan_.enabled) processAnimeJanaiPlan();
    }
    try {
        mpv_->drainEvents();
    } catch (const MpvError&) {
        return;
    }
    publishObservedState();
    handleState();
    updateStreamStats();
    // The last things the pump does: a request issued here is answered by the
    // next pump's drainEvents, so a reply is never delivered re-entrantly from
    // inside the tick that issued it.
    pollNativeStatus();
    pollDiagnostics();
    pollAnimeJanaiEngineBuild();
    refreshMediaTracks();
    // Bounded deadline for the native transition in flight: a command the core
    // accepted but the filter never acknowledged ends in a named non-Active
    // verdict rather than hanging the transition forever.
    applyNativeDecision(nativePlayback_.tick(nativePlaybackNowMs()));
}

void VideoSession::publishObservedState() {
    const bool currentLoaded = entryLoadReported_ && entrySourceEpoch_ == mpv_->sourceEpoch();
    if (currentLoaded && observedDuration_ > 0 && observedDuration_ != duration_) {
        duration_ = observedDuration_;
        emit durationChanged(duration_);
    }
    // 10 ms is finer than any display, so this cannot look stepped.
    if (currentLoaded && std::abs(observedPosition_ - position_) > 0.01) {
        position_ = observedPosition_;
        emit positionChanged(position_);
        if (unfilteredCapture_ != nullptr) checkUnfilteredFramePresented(unfilteredCapture_);
    }
    // A write still in flight is the viewer's intent, so an observation that has
    // not caught up with it cannot flip the control back.
    const bool playing =
        !url_.isEmpty() && !observedIdle_ && !requestedPause() && !enginePauseHeld();
    if (playing != playing_) {
        playing_ = playing;
        publishPlayingState(playing_);
    }
}

void VideoSession::updateStreamStats() {
    if (!statsClock_.isValid()) {
        statsClock_.start();
        return;
    }
    if (statsClock_.elapsed() < 500) return;
    statsClock_.restart();

    // Observed, not read: these two belong to a HUD label and must not wait on
    // the core lock.
    const double bytesPerSecond = std::max(0.0, observedCacheSpeed_);
    const ResolvedStreamFps resolvedFps =
        resolveStreamFps(diagnostics_.estimatedVfFps, observedContainerFps_,
                         diagnosticsActive_);
    const double framesPerSecond = resolvedFps.framesPerSecond;
    const StreamFpsSource fpsSource = resolvedFps.source;
    if (std::abs(bytesPerSecond - networkBytesPerSecond_) < 1024.0 &&
        std::abs(framesPerSecond - streamFramesPerSecond_) < 0.01 &&
        fpsSource == streamFpsSource_) {
        return;
    }
    networkBytesPerSecond_ = bytesPerSecond;
    streamFramesPerSecond_ = framesPerSecond;
    streamFpsSource_ = fpsSource;
    emit streamStatsChanged(networkBytesPerSecond_, streamFramesPerSecond_, streamFpsSource_);
}

void VideoSession::handleState() {
    if (!mpv_) return;
    // One value, two answers. The width decides both whether there is a video at
    // all and how wide it is, and it was observed rather than read.
    const int w = static_cast<int>(observedVideoWidth_);
    const bool vid = !url_.isEmpty() && w > 0;
    if (vid != hasVideo_) hasVideo_ = vid;
    if (hasVideo_) {
        const int h = static_cast<int>(observedVideoHeight_);
        if (w != videoWidth_ || h != videoHeight_) {
            videoWidth_ = w;
            videoHeight_ = h;
            emit videoSizeChanged(w, h);
        }
    }
    if (!hasVideo_) {
        publishDisplayAspectRatio(0.0);
        return;
    }
    // Display geometry includes sample aspect and rotation; processing retains
    // coded size.
    double aspectWidth = 0.0;
    double aspectHeight = 0.0;
    if (std::isfinite(observedOutputWidth_) && observedOutputWidth_ > 0.0 &&
        std::isfinite(observedOutputHeight_) && observedOutputHeight_ > 0.0) {
        aspectWidth = observedOutputWidth_;
        aspectHeight = observedOutputHeight_;
        // vo_libmpv handles quarter turns without swapping its reported dw/dh.
        const double reportedRotation =
            std::isfinite(observedOutputRotation_) ? observedOutputRotation_ : 0.0;
        long rotation = std::lround(reportedRotation);
        rotation %= 360;
        if (rotation < 0) rotation += 360;
        if (rotation % 180 == 90) std::swap(aspectWidth, aspectHeight);
    } else if (videoWidth_ > 0 && videoHeight_ > 0) {
        aspectWidth = videoWidth_;
        aspectHeight = videoHeight_;
    }
    publishDisplayAspectRatio(aspectWidth > 0.0 && aspectHeight > 0.0
                                  ? aspectWidth / aspectHeight
                                  : 0.0);
}

void VideoSession::publishDisplayAspectRatio(double ratio) {
    if (!std::isfinite(ratio) || ratio <= 0.0) ratio = 0.0;
    if (ratio == displayAspectRatio_) return;
    displayAspectRatio_ = ratio;
    emit displayAspectRatioChanged(ratio);
}

// ── getters ───────────────────────────────────────────────────────────────

double VideoSession::position() const { return position_; }

double VideoSession::duration() const { return duration_; }

int VideoSession::videoWidth() const { return videoWidth_; }

int VideoSession::videoHeight() const { return videoHeight_; }

bool VideoSession::hasVideo() const { return hasVideo_; }

uint64_t VideoSession::sourceEpoch() const {
    return mpv_ != nullptr ? mpv_->sourceEpoch() : 0;
}

native::AnimeJanaiSourceContext VideoSession::animeJanaiSourceContext() const {
    native::AnimeJanaiSourceContext source;
    source.width = observedVideoWidth_ > 0.0 ? static_cast<int>(observedVideoWidth_) : 0;
    source.height = observedVideoHeight_ > 0.0 ? static_cast<int>(observedVideoHeight_) : 0;
    source.fps = std::isfinite(observedContainerFps_) && observedContainerFps_ > 0.0
                     ? observedContainerFps_
                     : 0.0;
    return source;
}

PlaybackDiagnostics VideoSession::playbackDiagnostics() const { return diagnostics_; }

void VideoSession::setDiagnosticsPolling(bool enabled) {
    // T-205: the raw-counter poll costs an mpv property read twice a second
    // while no in-app consumer of these numbers exists at all.
    if (diagnosticsPolling_ == enabled) return;
    diagnosticsPolling_ = enabled;
    if (!enabled) {
        // A disabled poll owns no in-flight reads and answers no stale values.
        resetDiagnostics();
    }
}

void VideoSession::setPumpSuspended(bool suspended) {
    // T-207: the timer's interval is set once in the constructor and
    // start()/stop() preserve it; a resume does not need to know it.
    if (suspended) {
        statePump_.stop();
    } else {
        statePump_.start();
    }
}

// ── raw diagnostics ───────────────────────────────────────────────────────

void VideoSession::resetDiagnostics() {
    for (DiagnosticRequest& request : diagnosticRequests_) {
        if (request.pending.valid() && mpv_) mpv_->cancelAsync(request.pending);
        request = DiagnosticRequest{};
    }
    // Unknown, not zero: the previous source's numbers say nothing about this one.
    diagnostics_ = PlaybackDiagnostics{};
    diagnosticsLastPollMs_ = diagnosticsClock_.elapsed();
}

void VideoSession::pollDiagnostics() {
    // T-205: the app - which has no consumer of the raw counters - switches the
    // poll off on every session it owns. Probes keep the constructor default
    // (on) because their verdicts assert on these counters. A frame-timing log
    // carries these counters, so that diagnostic launch keeps the poll alive.
    if (!mpv_ || !diagnosticsActive_ ||
        (!diagnosticsPolling_ && mplayer::renderFrameTimingLogPath().isEmpty())) {
        return;
    }
    const qint64 nowMs = diagnosticsClock_.elapsed();
    if (nowMs - diagnosticsLastPollMs_ < kDiagnosticsPollIntervalMs) return;
    diagnosticsLastPollMs_ = nowMs;
    // Bumped by load/stop: a reply from an old epoch is dropped, not published.
    const uint64_t epoch = mpv_->sourceEpoch();
    issueDiagnostic(kDecoderDropSlot, kDecoderDropProperty, epoch);
    issueDiagnostic(kVoDropSlot, kVoDropProperty, epoch);
    issueDiagnostic(kVoDelayedSlot, kVoDelayedProperty, epoch);
    issueDiagnostic(kVfSlot, kVfProperty, epoch);
}

void VideoSession::issueDiagnostic(DiagnosticSlot slot, const char* property, uint64_t epoch) {
    DiagnosticRequest& request = diagnosticRequests_[slot];
    const qint64 nowMs = diagnosticsClock_.elapsed();
    if (request.pending.valid()) {
        // An answer that never came: abandon it so the property is not stuck
        // unread, and stop presenting the value it was meant to refresh.
        if (nowMs - request.lastRequestMs < kDiagnosticsRequestTimeoutMs) return;
        mpv_->cancelAsync(request.pending);
        request.pending = AsyncRequestId{};
        setDiagnosticUnknown(slot);
    }
    request.lastRequestMs = nowMs;
    request.pending = mpv_->getPropertyAsync(
        property, MPV_FORMAT_NODE, [this, slot, epoch](const AsyncReply& reply) {
            DiagnosticRequest& slotRequest = diagnosticRequests_[slot];
            if (!slotRequest.pending.valid()) return;
            slotRequest.pending = AsyncRequestId{};
            // A reply for a superseded source says nothing about the current one.
            if (mpv_ == nullptr || epoch != mpv_->sourceEpoch()) {
                setDiagnosticUnknown(slot);
                return;
            }
            applyDiagnosticReply(slot, reply);
        });
    if (!request.pending.valid()) setDiagnosticUnknown(slot);
}

void VideoSession::applyDiagnosticReply(DiagnosticSlot slot, const AsyncReply& reply) {
    switch (slot) {
        case kDecoderDropSlot:
            diagnostics_.decoderDropCount = exactInteger(reply);
            break;
        case kVoDropSlot:
            diagnostics_.voDropCount = exactInteger(reply);
            break;
        case kVoDelayedSlot:
            diagnostics_.voDelayedFrameCount = exactInteger(reply);
            break;
        case kVfSlot: {
            bool present = false;
            int count = 0;
            if (summarizeVfChain(reply, &present, &count)) {
                diagnostics_.nativeFilterPresent = present;
                diagnostics_.vfCount = count;
            } else {
                diagnostics_.nativeFilterPresent.reset();
                diagnostics_.vfCount.reset();
            }
            break;
        }
    }
}

void VideoSession::setDiagnosticUnknown(DiagnosticSlot slot) {
    switch (slot) {
        case kDecoderDropSlot:
            diagnostics_.decoderDropCount.reset();
            break;
        case kVoDropSlot:
            diagnostics_.voDropCount.reset();
            break;
        case kVoDelayedSlot:
            diagnostics_.voDelayedFrameCount.reset();
            break;
        case kVfSlot:
            diagnostics_.nativeFilterPresent.reset();
            diagnostics_.vfCount.reset();
            break;
    }
}

}  // namespace mplayer
