#include "VideoSurface.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QThread>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "MpvApi.hpp"
#include "MpvHandle.hpp"
#include "MpvRenderContext.hpp"
#include "backend/Diagnostics.hpp"
#include "native/bundle/NativeBundle.hpp"
#include "native/components/AnimeJanaiBuildLog.hpp"
#include "native/status/NativeFilterCommand.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"

namespace mplayer {

namespace {

constexpr qint64 kDiagnosticsPollIntervalMs = 500;
// A lost reply must not leave a property unread for the rest of the source.
constexpr qint64 kDiagnosticsRequestTimeoutMs = 5000;
constexpr qint64 kRestoreSettleTimeoutMs = 5000;
constexpr int kRestoreSettleMaxAttempts = 5;
// A probe read is answered in microseconds or not at all; the bound only exists
// so a manifest can never hang the GUI thread on a core that stopped answering.
constexpr int kProbeReadTimeoutMs = 250;

/// Numeric argument for an mpv command. mpv parses these itself, so a
/// locale-formatted value ("1,5") would be a parse error: the number is rendered
/// with Qt's locale-independent formatting instead of std::to_string.
std::string mpvNumber(double value) {
    return QString::number(value, 'f', 6).toStdString();
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

/// Turns one reply into the shape the caller asked for, or refuses it. An
/// integer property that was asked for as a number answers with an integer, and
/// that is the number the manifest asserted on - so both numeric formats are
/// accepted, while a string or flag reply of another shape is not coerced into
/// one.
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
constexpr const char* kVfProperty = "vf";


/// The `lib=` option the native filter needs to open its inference shim: the
/// producer library of the validated native bundle this process actually
/// loaded, proved against that bundle's own inventory digest. Empty when the
/// loaded runtime is not a native bundle or the declared dispatcher is gone or
/// no longer matches its recorded bytes - the binding is then probe-only, and
/// an admission against it is refused by the core rather than loaded from a
/// guessed path.
QVector<native::NativeFilterOption> provedNativeAttachOptions() {
    const native::RuntimeSelection& selection = native::currentSelection();
    if (!selection.loaded || selection.kind != native::BundleKind::Native ||
        !selection.resolvedBundle.has_value()) {
        return {};
    }
    const native::Bundle& bundle = *selection.resolvedBundle;
    const QString producer = native::bundlePreparationProducerLibrary();
    for (const native::BundleFile& file : bundle.files) {
        if (QFileInfo(file.path).fileName().compare(producer, Qt::CaseInsensitive) != 0) continue;
        const QString absolute =
            QFileInfo(QDir::cleanPath(QDir(bundle.root).filePath(file.path))).canonicalFilePath();
        if (absolute.isEmpty()) continue;
        QFile handle(absolute);
        if (!handle.open(QIODevice::ReadOnly)) continue;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&handle)) continue;
        if (hash.result().toHex().compare(file.sha256.toLatin1(), Qt::CaseInsensitive) != 0) {
            continue;
        }
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

void VideoSurface::ensureEngine() {
    if (mpv_) return;
    // Entry ids belong to one core's playlist; a core created here has never
    // reported one, so nothing a previous core named can be matched against it.
    pendingLoadTokens_.clear();
    entryTokensById_.clear();
    // These are the options the Python player passed through python-mpv, which
    // spelled them with underscores; mpv itself takes dashes.
    const std::vector<std::pair<std::string, std::string>> options{
        // Routes decoded frames into OUR render context. Without it mpv opens a
        // video output of its own, the render context never receives a frame
        // (update() stays at generation 0) and render() still returns success
        // while painting nothing.
        {"vo", "libmpv"},
        // Not cosmetic: MoonAnime's CDN answers 400 to a playlist that arrives
        // without an Accept-Language header, so mpv would sit on a stream that
        // extraction read perfectly well. The value is never inspected, only its
        // presence, and mpv splits http-header-fields on commas so it must stay
        // comma-free.
        {"user-agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
         "Chrome/142.0.0.0 Safari/537.36"},
        {"http-header-fields", "Accept-Language: uk-UA"},
        {"hwdec", "auto-safe"},
        {"keepaspect", "yes"},
        {"volume", "100"},
        // The keyboard belongs to the shell: mpv's own bindings would consume
        // Space and the arrow keys before Qt ever sees them.
        {"input-default-bindings", "no"},
        {"input-vo-keyboard", "no"},
        {"input-cursor", "no"},
        {"osc", "no"},
        // Reference captures must be the decoded source frame, not what the VO
        // rendered: with this set, screenshot-raw reads the current video frame
        // from the software path (no GLSL shader chain, no OSD), which is the
        // only capture this app's oracle may use. Nothing else in the player
        // reads mpv screenshots.
        {"screenshot-sw", "yes"},
        // Rendering here is driven by Qt's compositor, so mpv cannot block for a
        // display time it does not own: every render call passes
        // BLOCK_FOR_TARGET_TIME=0 and mpv keeps video timed to audio through this
        // option instead. Leave it out and A/V sync drifts by the headroom mpv
        // would otherwise have waited out.
        {"video-timing-offset", "0"},
    };
    mpv_ = std::make_unique<MpvHandle>(options);
    // A plan may have been selected before the core existed. Request its backend
    // reports before any source or official filter command reaches this core.
    mpv_->setBackendLogObservationEnabled(animeJanaiBackendLogsNeeded());
    // The filter opens its inference shim only for an explicitly named producer
    // library, so the binding an admission is later loaded into has to name the
    // one this core's own bundle ships; without it every admission is refused
    // by the core before its payload is parsed.
    nativePlayback_.setAttachOptions(provedNativeAttachOptions());
    // The reader borrows this core, and is (re)bound here because this is the
    // one place a core is created. It publishes an Unavailable (unread) view
    // until the first poll answers; polling itself stays off until the parent
    // asks for it.
    nativeStatus_.attach(mpv_.get());
    // A fresh core, so any raw number a previous one left reads as unknown.
    resetDiagnostics();
    mpv_->setEventSink([this](mpv_event* event) {
        if (event->event_id == MPV_EVENT_LOG_MESSAGE && event->data != nullptr) {
            const auto* message = static_cast<const mpv_event_log_message*>(event->data);
            const QString line = QString::fromUtf8(message->text == nullptr ? "" : message->text).trimmed();
            const QString prefix = QString::fromUtf8(message->prefix == nullptr ? "" : message->prefix);
            // libmpv echoes the URLs it was handed and the filter commands it
            // was asked to run. Both can carry a signed token, and everything
            // Qt logging emits is persisted by the session log and carried into
            // crash reports, so the redaction happens at this boundary rather
            // than at every reader of a log line. The observation and failure
            // paths below keep the exact mpv text: matching a status marker or
            // naming a failure is not a persistence boundary, and redacting
            // there would change what the player reports to the user.
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
                animeJanaiFail(QStringLiteral("animejanai.backend.failed"), line);
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
                if (event->error >= 0 && event->data != nullptr) {
                    if (const auto entryId = playlistEntryIdFromCommand(
                            *static_cast<const mpv_event_command*>(event->data))) {
                        // Replacements retire older entries; playlist ids only increase.
                        if (entryTokensById_.size() >= 64) {
                            entryTokensById_.erase(entryTokensById_.begin());
                        }
                        if (havePlaylistEntryId_ && currentPlaylistEntryId_ == *entryId) {
                            entryMediaToken_ = load.token;
                            entrySourceEpoch_ = load.epoch;
                            if (entryLoadReported_ && load.epoch == mpv_->sourceEpoch()) {
                                mediaTracksLoaded_ = true;
                                mediaTracksDirty_ = true;
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
            // Unknown entries stay unnamed; never pair by command issue order.
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
                if (entrySourceEpoch_ == mpv_->sourceEpoch()) {
                    mediaTracksLoaded_ = true;
                    mediaTracksDirty_ = true;
                }
                emit mediaLoaded(entryMediaToken_);
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
            if (entryLoadReported_) emit mediaEnded(entryMediaToken_);
            entryMediaToken_ = 0;
            entryLoadReported_ = false;
            return;
        }
        if (ended->reason != MPV_END_FILE_REASON_ERROR) return;
        if (rendererTeardownErrorPending_ && ended->error == MPV_ERROR_VO_INIT_FAILED) {
            // Releasing the render context is what killed the old video output -
            // vo_libmpv uninitializes the VO and errors its track - so this END_FILE
            // describes the renderer being replaced, not a source that failed.
            // Named for developers instead of reported as a playback failure; a
            // video output that really cannot initialize belongs to the entry a
            // START_FILE has since named, which clears the marker above.
            rendererTeardownErrorPending_ = false;
            qWarning("VideoSurface: retired the video output error caused by releasing the "
                     "render context (mpv: %d)",
                     ended->error);
            return;
        }
        if (havePlaylistEntryId_ && ended->playlist_entry_id != currentPlaylistEntryId_) {
            // The entry this error names is not the one playing: the result
            // belongs to a source that has already been replaced.
            qWarning("VideoSurface: retired a playback error for playlist entry %lld (playing "
                     "%lld, mpv: %d)",
                     static_cast<long long>(ended->playlist_entry_id),
                     static_cast<long long>(currentPlaylistEntryId_), ended->error);
            return;
        }
        emit errorOccurred(tr("Could not play the video (mpv: %1)").arg(ended->error));
    });
    observePlaybackState();
    // The applied id, not the mode: a chain with no Mode is only recorded until
    // the engine exists, so reading the mode here would drop it before the
    // first frame.
    const QString chainId =
        anime4kChainId_.isEmpty() ? anime4k::modeId(anime4kMode_) : anime4kChainId_;
    const QString shaders = anime4k::shaderStringForCandidate(chainId);
    if (!shaders.isEmpty()) {
        applyShaderChainRequest(shaders);
    }
}

void VideoSurface::applyShaderChainRequest(const QString& shaders) {
    if (mpv_ == nullptr) return;
    // Only the write is submitted here. What the surface reports as the chain is
    // mpv's own observed value, from the glsl-shaders observer, so a submission
    // is never reported as though it were in effect.
    issueCoreCommand({"set", "glsl-shaders", shaders.toStdString()}, "glsl-shaders",
                     /*reportRefusal=*/true);
}

void VideoSurface::setAnime4KChain(const QString& candidateId) {
    ++anime4KRequestGeneration_;
    if (candidateId == anime4kChainId_) {
        return;
    }
    anime4kChainId_ = candidateId;
    // A named chain supersedes the enum, but the enum is not cleared: a later
    // setAnime4KMode rewrites both, and leaving it stale would let the two
    // disagree about what is on screen.
    if (!mpv_) {
        return;
    }
    const QString shaders = anime4k::shaderStringForCandidate(candidateId);
    applyShaderChainRequest(shaders);
    // vo=libmpv rebuilds the filter chain on the next rendered frame, and a
    // paused video produces none, so without this the change would appear to
    // do nothing until playback resumed.
    refreshRenderState();
}
void VideoSurface::setAnime4KMode(anime4k::Mode mode) {
    ++anime4KRequestGeneration_;
    // The id is compared too: an override the compare window applied must be
    // taken back when the same mode is picked again.
    const QString id = anime4k::modeId(mode);
    if (mode == anime4kMode_ && id == anime4kChainId_) return;
    anime4kMode_ = mode;
    // The id is the persistence key, so it is recorded here too: a mode chosen
    // from the menu and a chain chosen in the compare window are the same
    // setting, and whichever came last must be what a restart restores.
    anime4kChainId_ = id;
    if (!mpv_) return;
    // The id is resolved, so a mode and a chain go through one path - and only
    // one of the two can name a chain that is not on disk.
    const QString shaders = anime4k::shaderStringForCandidate(anime4kChainId_);
    applyShaderChainRequest(shaders);
    // vo=libmpv only rebuilds the filter chain when a new frame is rendered,
    // and a paused video produces none, so the mode would appear to have no
    // effect until playback resumed.
    refreshRenderState();
}

QString VideoSurface::effectiveShaderChain() const {
    if (!mpv_) return QString();
    // mpv only echoes this back - it does not validate a shader path list - so
    // no answer separates a working chain from a preset naming a deleted file.
    return shaderChain_;
}


bool VideoSurface::waitForContext(int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!contextReady_ && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(20);
    }
    return contextReady_;
}

VideoSurface::VideoSurface(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumSize(160, 90);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    // Without this the widget is composited as a separate texture and mpv's
    // frame would be blended against a stale backing store.
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    // Both connections belong to the widget, not to a GL context: a context is
    // destroyed and recreated when the widget moves to another window, and a
    // second pump started there would run this one's steps twice per core.
    statePump_.setInterval(50);
    connect(&statePump_, &QTimer::timeout, this, &VideoSurface::pumpEvents);
    statePump_.start();
    // The swap report mpv asks for names the moment its frame was presented, and
    // that is what Qt reports here, after the composition it belongs to.
    connect(this, &QOpenGLWidget::frameSwapped, this, &VideoSurface::onFrameSwapped);
    // Keep a replayable snapshot ahead of external listeners: a dialog that
    // reconnects after missing the original signal can synchronize from these
    // typed getters without dynamic properties or a second event source.
    connect(this, &VideoSurface::animeJanaiEngineBuildChanged, this,
            [this](bool active, bool succeeded, const QString& detail) {
                Q_UNUSED(succeeded);
                animeJanaiEngineBuildActive_ = active;
                animeJanaiEngineBuildDetail_ = detail;
            });
    nativeStatusClock_.start();
    nativePlaybackClock_.start();
    diagnosticsClock_.start();
    // The controller never reaches the core itself: it hands this surface one
    // typed command at a time, and the async reply comes back through the same
    // drain as every other event.
    nativePlayback_.setCommandSink(
        [this](const native::NativePlaybackCommand& command) { deliverNativeCommand(command); });
    // Installed once, before any core exists. The reader calls this from
    // drainEvents on this thread, and the callback only forwards the published
    // view: nothing here can take the core lock or reach into mpv, so no
    // repaint or teardown can run it against a borrowed handle.
    nativeStatus_.setChangeCallback([this](const native::NativeStatusView& view) {
        // A refusal the reader will keep giving back - no filter attached, a
        // read the core would not take, or a filter document that has drifted
        // from this build's schema/protocol/tags - repeats identically for the
        // life of the source. Repeating the request every 500 ms would only
        // repeat the refusal, so the poll stops until the request state changes
        // or another source is opened. The Unavailable view is still forwarded:
        // the surface never invents an Active status.
        if (view.snapshot.has_value()) nativeSnapshotSeen_ = true;
        if (view.availability.value == native::Availability::Unavailable &&
            native::statusRefusalHaltsPolling(view.availability.reasonKey)) {
            // A filter this surface just attached may not have published its tag
            // map yet, so a missing or unreadable property is retried until the
            // first snapshot arrives. Every other refusal - a drifted schema, a
            // tag this build does not know - repeats identically, so it halts.
            const bool transient =
                nativeFilterBound_ && !nativeSnapshotSeen_ &&
                (view.availability.reasonKey == QStringLiteral("native.status.missing") ||
                 view.availability.reasonKey == QStringLiteral("native.status.request"));
            nativeStatusHalted_ = !transient;
        } else if (view.snapshot.has_value()) {
            // The first response named a state no poll can advance (Off,
            // Suspended, Failed, ...): stop after it and wait for a new
            // request, rather than reading the same terminal state forever. A
            // Probe snapshot is NOT one of those - the producer only fills in
            // its source and device facts when it adopts a decoded frame - so
            // the policy lives in one predicate beside the FPS resolver rather
            // than as a special case here.
            nativeStatusHalted_ = !nativeSnapshotKeepsPolling(view.snapshot->state);
        }
        // The reader published a genuine change, so it is the one observation
        // the controller may act on. It is read under the epoch the request was
        // issued at, so a reply for a superseded source cannot be acknowledged.
        if (mpv_) {
            applyNativeDecision(nativePlayback_.observe(view, mpv_->sourceEpoch(),
                                                       nativePlaybackNowMs()));
            // The producer's own source report: a moved display geometry or
            // cadence drops an admission still in flight. Nothing is loaded or
            // rebuilt here - the reset is the whole effect.
            if (view.snapshot.has_value() && view.snapshot->probe.has_value() &&
                view.snapshot->probe->source.has_value()) {
                nativePlayback_.onSourceGeometry(*view.snapshot->probe->source);
                publishNativePlaybackStatus();
            }
        }
        emit nativeStatusChanged(view);
    });
}

void VideoSurface::setEngineFrameHold(bool active) {
    if (active) {
        if (engineFrameHoldActive_) return;
        engineFrameHoldActive_ = true;
        engineFrameHoldAwaitingOutput_ = false;
        // Capture before the filter changes; ordinary playback must not copy every frame.
        if (!engineFrameHoldFbo_) captureEngineFrameHold();
        update();
        return;
    }
    if (!engineFrameHoldActive_ && !engineFrameHoldAwaitingOutput_) return;
    engineFrameHoldActive_ = false;
    engineFrameHoldAwaitingOutput_ = engineFrameHoldFbo_ != nullptr;
    // If there was no retained picture, the next paint can show the current
    // renderer state directly; otherwise the first successful new output frame
    // is the only event that replaces the held image.
    update();
}

void VideoSurface::invalidateEngineFrameHold() {
    engineFrameHoldActive_ = false;
    engineFrameHoldAwaitingOutput_ = false;
    hasVisibleOutputFrame_ = false;
    if (!engineFrameHoldFbo_) return;
    QOpenGLContext* previous = nullptr;
    const bool current = makeRenderContextCurrent(&previous);
    engineFrameHoldFbo_.reset();
    if (current) releaseRenderContextCurrent(previous);
}

bool VideoSurface::captureEngineFrameHold() {
    if (!contextReady_ || context_ == nullptr || render_ == nullptr ||
        !hasVisibleOutputFrame_) return false;
    QOpenGLContext* previous = nullptr;
    if (!makeRenderContextCurrent(&previous)) return false;
    const QSize size = lastRenderedFboSize_;
    if (size.isEmpty()) {
        releaseRenderContextCurrent(previous);
        return false;
    }
    auto snapshot = std::make_unique<QOpenGLFramebufferObject>(
        size, QOpenGLFramebufferObject::NoAttachment);
    if (!snapshot->isValid()) {
        snapshot.reset();
        releaseRenderContextCurrent(previous);
        return false;
    }

    // Re-render mpv's cached image into a video-only target. The Qt widget FBO
    // may already contain paintOverlay() from its previous composition, so
    // blitting it would bake HUD pixels into the transition image.
    const uint64_t updateFlags = render_->update();
    const bool frameConsumed = framePending_ || (updateFlags & MPV_RENDER_UPDATE_FRAME) != 0;
    const int result = render_->render(snapshot->handle(), size.width(), size.height(), 0);
    if (QOpenGLExtraFunctions* extra = context_->extraFunctions()) {
        extra->glBindVertexArray(0);
        extra->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        extra->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
    context_->functions()->glBindFramebuffer(
        GL_FRAMEBUFFER, static_cast<GLuint>(defaultFramebufferObject()));
    if (result < 0) {
        snapshot.reset();
        releaseRenderContextCurrent(previous);
        return false;
    }

    engineFrameHoldFbo_ = std::move(snapshot);
    framePending_ = false;
    // Capturing can consume an update, but it is not an on-screen presentation.
    if (frameConsumed || videoFramePainted_) {
        videoFramePainted_ = false;
        render_->reportSwap();
    }
    releaseRenderContextCurrent(previous);
    return true;
}

bool VideoSurface::drawEngineFrameHold(unsigned int target, int width, int height) {
    if (!engineFrameHoldFbo_ || !engineFrameHoldFbo_->isValid() || width <= 0 || height <= 0)
        return false;
    QOpenGLContext* glContext = QOpenGLContext::currentContext();
    if (glContext == nullptr) return false;
    QOpenGLExtraFunctions* gl = glContext->extraFunctions();
    if (gl == nullptr) return false;
    GLint previousRead = 0;
    GLint previousDraw = 0;
    gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, engineFrameHoldFbo_->handle());
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    gl->glBlitFramebuffer(0, 0, engineFrameHoldFbo_->width(), engineFrameHoldFbo_->height(),
                          0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
    return true;
}

VideoSurface::~VideoSurface() {
    cancelAnimeJanaiOperation();
    // The render context holds a pointer to the mpv core, so it has to go first,
    // and with THIS widget's context current: freeing it releases GL objects.
    // QOpenGLContext::aboutToBeDestroyed normally does this, and this is the
    // fallback for a widget destroyed without its context ever being torn down.
    releaseRenderContext();
    if (mpv_) {
        // Unobserve for real before the core goes, so mpv is not still
        // formatting changes for a widget that is being destroyed.
        for (const uint64_t id : observers_) mpv_->unobserve(id);
        observers_.clear();
    }
    // The native-status reader borrows the same core: release the binding and
    // cancel any in-flight request before the core is reset, so no callback can
    // run against a widget that is going away.
    resetNativeStatusReader(false);
    resetDiagnostics();
    // The core's terminate-destroy synchronously frees the filter graph and its
    // native jobs. The controller's claims are held against exactly that work, so
    // they may only be released after it - never while the core, and therefore a
    // job that may still be reading an artifact, is alive.
    mpv_.reset();
    nativePlayback_.endSource();
}

bool VideoSurface::open(const QString& url, quint64 mediaToken) {
    invalidateEngineFrameHold();
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
    networkBytesPerSecond_ = 0.0;
    streamFramesPerSecond_ = 0.0;
    streamFpsSource_ = StreamFpsSource::Unknown;
    statsClock_.restart();
    observedContainerFps_ = 0.0;
    observedVideoWidth_ = 0.0;
    observedVideoHeight_ = 0.0;
    emit streamStatsChanged(0.0, 0.0, StreamFpsSource::Unknown);
    try {
        if (mpv_ != nullptr) {
            mpv_->setBackendLogObservationEnabled(animeJanaiBackendLogsNeeded());
        }
        ensureEngine();
    } catch (const MpvError& e) {
        // No core could be created - the selected runtime refused, or none was
        // selected. That is an unavailability to report, not an exception for a
        // caller that only asked whether the open worked: this function is the
        // first thing that touches the loader on the GUI thread.
        url_.clear();
        animeJanaiSourceRequested_ = false;
        pendingOpen_ = false;
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
    // The load waits for the GL context. With vo=libmpv the video output
    // attaches to the render context, and mpv will not start a VO that has
    // nowhere to draw: loading first leaves the core with no video and a black
    // surface. Callers legitimately open before the widget is ever shown.
    if (!contextReady_) {
        pendingOpen_ = true;
        try {
            mpv_->setBackendLogObservationEnabled(false);
        } catch (const MpvError& e) {
            emit errorOccurred(QString::fromUtf8(e.what()));
        }
        return true;
    }
    return startPendingUrl();
}

bool VideoSurface::startPendingUrl(double resumeAt) {
    pendingOpen_ = false;
    if (url_.isEmpty() || mpv_ == nullptr) {
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
    invalidateEngineFrameHold();
    // The source ends HERE, before the load is issued: the epoch is what retires
    // a reply for the source being replaced, and a command queued behind this
    // one must already be reading the new epoch when its reply lands.
    mpv_->invalidateAsyncEpoch();
    resetMediaTracks();
    const MediaTrackSelection selection = nextMediaTrackSelection_.value_or(MediaTrackSelection{});
    nextMediaTrackSelection_.reset();
    const auto option = [](std::optional<int64_t> id) {
        if (!id.has_value()) return std::string("auto");
        return *id == 0 ? std::string("no") : std::to_string(*id);
    };
    std::string options = "aid=" + option(selection.audio) + ",sid=" + option(selection.subtitle) +
                          ",sub-auto=exact,audio-file-auto=exact";
    if (resumeAt > 0.0) options += ",start=" + mpvNumber(resumeAt);
    std::vector<std::string> load{"loadfile", url_.toStdString(), "replace", "-1", std::move(options)};
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
    // a caller reads is the one the admission is requested under, instead of the
    // two racing a rebind on the next pump.
    syncNativeSource();
    animeJanaiEpoch_ = mpv_->sourceEpoch();
    emit sourceEpochChanged(animeJanaiEpoch_);
    if (animeJanaiPlan_.enabled) processAnimeJanaiPlan();
    return true;
}

void VideoSurface::close() {
    invalidateEngineFrameHold();
    resetMediaTracks();
    nextMediaTrackSelection_.reset();
    pendingMediaTrackSelection_ = {};
    discardEnginePause();
    finishAnimeJanaiEngineBuild(false, tr("Playback closed during engine preparation."));
    animeJanaiSourceRequested_ = false;
    pendingOpen_ = false;
    cancelAnimeJanaiOperation();
    animeJanaiEpoch_ = 0;
    // Nothing this surface already asked to play can start now: the stop below
    // retires those requests, so their identities must not pair with an entry a
    // later load starts. A reply already drained cannot pair either.
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
    // no request in flight can describe a file that is gone, and clear the halt
    // that belonged to the old source.
    resetNativeStatusReader(false);
    nativeStatusHalted_ = false;
    resetDiagnostics();
    diagnosticsActive_ = false;
    // Same retirement as the load: what was outstanding belonged to the source
    // that is ending, so it is dropped before the stop is issued. An accepted
    // pause write awaiting its observation belongs to this source too and will
    // never be confirmed against the next one.
    mpv_->invalidateAsyncEpoch();
    pendingPauseWrite_.reset();
    animeJanaiEpoch_ = mpv_->sourceEpoch();
    observedVideoWidth_ = 0.0;
    observedVideoHeight_ = 0.0;
    observedContainerFps_ = 0.0;
    emit sourceEpochChanged(mpv_->sourceEpoch());
    // A stop on an already-dead core is not worth reporting. Keep the keyed
    // cache lease until mpv acknowledges that the old pipeline has stopped.
    const auto stoppingLease = animeJanaiStageBuildLease_;
    const AsyncRequestId stopRequest = mpv_->commandAsync(
        {"stop"}, [this, stoppingLease](const AsyncReply& reply) {
            if (!reply.ok()) return;
            if (stoppingLease) stoppingLease->release();
            if (animeJanaiStageBuildLease_ == stoppingLease)
                animeJanaiStageBuildLease_.reset();
        });
    Q_UNUSED(stopRequest);
    animeJanaiEpoch_ = 0;
    // Emitted before the reset so the chrome falls back to its play glyph
    // while the surface still reports a loaded file.
    if (playing_) {
        playing_ = false;
        emit playingChanged(false);
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
}

void VideoSurface::togglePause() {
    // The requested state, not the observed one: a toggle during a write that is
    // still in flight has to invert what the viewer last asked for, or a rapid
    // double toggle would request the same value twice.
    setPaused(enginePauseHeld() ? !enginePauseIntent_ : !requestedPause());
}

void VideoSurface::setPaused(bool paused) {
    ++pauseRequestGeneration_;
    const bool held = enginePauseHeld();
    if ((held || enginePreparationPaused_) &&
        pauseRequestGeneration_ > enginePauseRequestGeneration_) {
        enginePauseIntent_ = paused;
    }
    if (!mpv_) return;
    // The viewer's intent is announced at once, through playing_, and told to the
    // core without waiting for it: the core may be holding frames for a native
    // filter, and a control that blocks behind that is a frozen window. What the
    // core holds is read back from its own property event, never assumed.
    setPlaying(!held && !paused);
    requestPausedWrite(held || paused);
}

void VideoSurface::reassertPaused() {
    if (!mpv_) return;
    setPlaying(false);
    requestPausedWrite(true);
}

void VideoSurface::requestPausedWrite(bool paused) {
    if (mpv_ == nullptr) return;
    const uint64_t epoch = mpv_->sourceEpoch();
    const quint64 generation = ++pauseWriteGeneration_;
    // With no earlier write awaiting observation, `observedPaused_` is what the
    // core holds: a request for that same value changes nothing and owes no
    // event, so it is the only request the reply may retire. One that differs
    // from the observation stays owed until the core is seen to hold it, because
    // the command reply can be drained before the property event it causes, and
    // the event's stale predecessor must not overwrite the viewer's intent in
    // between.
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
        qWarning("VideoSurface: the mpv transport did not accept set pause");
        emit errorOccurred(tr("pause: the mpv transport did not accept the command"));
    }
}

QImage VideoSurface::grabVideoFramebuffer() {
    if (QThread::currentThread() != thread()) {
        qWarning("VideoSurface: grabVideoFramebuffer() is GUI thread only");
        return QImage();
    }
    QImage image;
    {
        // Scoped, and rolled back even on an early exit: the paint Qt makes for
        // the grab leaves the overlay out, and every later paint puts it back.
        QScopedValueRollback<bool> suppressed(overlaySuppressed_, true);
        image = QOpenGLWidget::grabFramebuffer();
    }
    // The captured content is not what is on screen now, so the widget is asked
    // for a normal paint rather than being left showing the suppressed one.
    update();
    return image;
}

void VideoSurface::seek(double seconds) {
    if (!mpv_) return;
    // A seek before a file is loaded is normal during startup, so a refusal is
    // tolerated here - the same tolerance the synchronous call had.
    issueCoreCommand({"seek", mpvNumber(seconds), "absolute"}, "seek",
                     /*reportRefusal=*/false);
    emit seekRequested(seconds);
}

void VideoSurface::setVolume(double volume) {
    if (!mpv_) return;
    issueCoreCommand({"set", "volume", mpvNumber(volume)}, "volume", /*reportRefusal=*/true);
}

void VideoSurface::setMuted(bool muted) {
    if (!mpv_) return;
    issueCoreCommand({"set", "mute", muted ? "yes" : "no"}, "mute", /*reportRefusal=*/true);
}

void VideoSurface::setSpeed(double speed) {
    if (!mpv_) return;
    issueCoreCommand({"set", "speed", mpvNumber(speed)}, "speed", /*reportRefusal=*/true);
}

void VideoSurface::issueCoreCommand(std::vector<std::string> args, const char* what,
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
            qWarning("VideoSurface: %s refused (%s)", label.c_str(), qUtf8Printable(reason));
        });
    if (request.valid() && mediaToken.has_value()) {
        // Keyed by the reply's own userdata, so the sink can read the entry this
        // exact command created even when the transport drops the callback
        // above: a source superseded before the reply lands still names its
        // entry, and that entry's start is real if mpv gets to it.
        pendingLoadTokens_.emplace(request.value, LoadIdentity{*mediaToken, epoch});
    }
    if (!request.valid()) {
        // No reply will ever arrive for a command mpv would not take, so the
        // refusal is named here instead of waited for. The transport reports only
        // that it did not accept the command, and no mpv error code is invented
        // for it.
        qWarning("VideoSurface: the mpv transport did not accept %s", command.c_str());
        if (reportRefusal) {
            emit errorOccurred(tr("%1: the mpv transport did not accept the command")
                                   .arg(QString::fromStdString(label)));
        }
    }
}

bool VideoSurface::isPlaying() const { return playing_; }

std::optional<ProbePropertyValue> VideoSurface::probeMpvProperty(const QString& name,
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
    // Pumping Qt events can run anything, including this widget's teardown, so
    // the surface is guarded rather than assumed to outlive the wait.
    QPointer<VideoSurface> self(this);
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

AsyncRequestId VideoSurface::requestPlaybackTimestamp(
    std::function<void(uint64_t, std::optional<double>)> callback) {
    if (mpv_ == nullptr || !callback) {
        return {};
    }
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

bool VideoSurface::probeCycleMpvTrack(const QString& property) {
    if (mpv_ == nullptr ||
        (property != QStringLiteral("aid") && property != QStringLiteral("sid"))) {
        return false;
    }
    return mpv_->commandAsync({"cycle", property.toStdString()},
                              [](const AsyncReply&) {}).valid();
}

void VideoSurface::setPlaying(bool playing) {
    // Announced, not just recorded. The state pump cannot notice a change this
    // class makes itself: it compares the core against this very flag, so
    // writing it here and staying silent left the pump nothing to report and
    // the play control frozen on its old glyph - the video paused behind a
    // "play" button, or running behind a "pause" one.
    if (playing == playing_) {
        return;
    }
    playing_ = playing;
    emit playingChanged(playing_);
}
double VideoSurface::position() const { return position_; }

double VideoSurface::duration() const { return duration_; }
int VideoSurface::videoWidth() const { return videoWidth_; }
int VideoSurface::videoHeight() const { return videoHeight_; }
native::AnimeJanaiSourceContext VideoSurface::animeJanaiSourceContext() const {
    native::AnimeJanaiSourceContext source;
    source.width = observedVideoWidth_ > 0.0 ? static_cast<int>(observedVideoWidth_) : 0;
    source.height = observedVideoHeight_ > 0.0 ? static_cast<int>(observedVideoHeight_) : 0;
    source.fps = std::isfinite(observedContainerFps_) && observedContainerFps_ > 0.0
                     ? observedContainerFps_ : 0.0;
    return source;
}
bool VideoSurface::hasVideo() const { return hasVideo_; }
uint64_t VideoSurface::sourceEpoch() const {
    return mpv_ != nullptr ? mpv_->sourceEpoch() : 0;
}

// ── GL lifecycle ──────────────────────────────────────────────────────────

void VideoSurface::initializeGL() {
    QOpenGLWidget::initializeGL();
    context_ = QOpenGLContext::currentContext();
    const bool recreated = renderGeneration_ > 0;
    try {
        ensureEngine();
        createRenderContext();
        // The first paint after the context exists has to render whatever mpv
        // is holding, even if no frame has been decoded yet: the framebuffer
        // has never been drawn into.
        forceRender_ = true;
        if (recreated && !pendingOpen_) {
            // Freeing the old render context disabled the core's video track
            // (vo_libmpv kills the VO and errors the track), so a source that was
            // already loaded has to be handed back to it, at the position the
            // viewer left. The old chain died with that video, so the native
            // binding is dropped with it rather than claiming a filter that is
            // no longer in the core.
            animeJanaiFilterBound_ = false;
            nativeFilterBound_ = false;
            MediaTrackSelection selection = mediaTrackSelection();
            if (pendingMediaTrackSelection_.audio.has_value())
                selection.audio = pendingMediaTrackSelection_.audio;
            if (pendingMediaTrackSelection_.subtitle.has_value())
                selection.subtitle = pendingMediaTrackSelection_.subtitle;
            setNextMediaTrackSelection(selection);
            startPendingUrl(observedPosition_);
        }
        if (requestedPause() || enginePauseHeld()) reassertPaused();
        if (pendingOpen_) startPendingUrl();
    } catch (const MpvError& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
        contextReady_ = false;
    }
}

void VideoSurface::createRenderContext() {
    if (mpv_ == nullptr || context_ == nullptr) return;
    const GlApi gl = loadGlApi();
    ++renderGeneration_;
    const quint64 generation = renderGeneration_;
    // Passed at construction: mpv raises the callback from its own threads and
    // once during registration itself, where a target installed afterwards would
    // be reached while still empty. The hop coalesces (mpv produces frames faster
    // than the GUI thread paints) and carries the generation, so one queued by a
    // released context does nothing.
    render_ = std::make_unique<MpvRenderContext>(
        *mpv_, gl,
        [this, generation] {
            if (queuedRenderCallback_.exchange(generation) == generation) return;
            QMetaObject::invokeMethod(this, [this, generation] { onRenderUpdate(generation); },
                                      Qt::QueuedConnection);
        },
        /*advancedControl=*/true);
    // mpv draws with the origin at the bottom-left; Qt composites the
    // QOpenGLWidget's framebuffer from the top-left. Without this the picture
    // is upside down - which a non-black pixel check happily passes.
    render_->setFlipY(true);
    // Direct, and emitted while the context can still be made current - which is
    // exactly what freeing the renderer needs.
    contextDestroyedConnection_ =
        connect(context_, &QOpenGLContext::aboutToBeDestroyed, this,
                &VideoSurface::releaseRenderContext, Qt::DirectConnection);
    contextReady_ = true;
}

void VideoSurface::resizeGL(int width, int height) {
    QOpenGLWidget::resizeGL(width, height);
    // Qt hands QOpenGLWidget a freshly allocated framebuffer on a resize, so
    // whatever the last paint drew into the old one is gone. The next paint
    // has to render even though no frame has been decoded since.
    forceRender_ = true;
    if (contextReady_) render_->update();
    // Ask for that paint rather than hoping Qt repaints on its own: the fresh
    // buffer has no defined content, and with a paused core nothing else will
    // schedule one, so the widget would composite undefined pixels.
    update();
}

void VideoSurface::showEvent(QShowEvent* event) {
    QOpenGLWidget::showEvent(event);
    // Same reasoning as resizeGL: a surface that was hidden (the pooled PiP
    // float) cannot assume its framebuffer still holds the last frame, and
    // with a paused core there may never be another one.
    forceRender_ = true;
    // A plain event, not a paint: the context is NOT current here, and every
    // render call needs it to be.
    refreshRenderState();
}

void VideoSurface::hideEvent(QHideEvent* event) {
    QOpenGLWidget::hideEvent(event);
    // Composition stops with the widget, so the frame it painted will never be
    // presented: the owed report is sent as the drop it is rather than left for
    // mpv's VO to time out on.
    acknowledgeDroppedFrame();
}

void VideoSurface::paintGL() {
    QOpenGLWidget::paintGL();
    if (!contextReady_ || !render_) {
        // No engine, so there is no frame to render - but NoPartialUpdate leaves
        // whatever is in this buffer visible, so it still gets a defined black
        // and the overlay, which belongs to the widget rather than to the core.
        QPainter unrendered(this);
        unrendered.fillRect(rect(), Qt::black);
        if (!overlaySuppressed_) emit paintOverlay(&unrendered);
        return;
    }
    // MPV_RENDER_UPDATE_FRAME marks a new output frame, independent of UI paints.
    // The callback hop's update() may have consumed that flag already, so a frame
    // it carried counts the same as one this call reports.
    const uint64_t updateFlags = render_->update();
    const bool newOutputFrame = (updateFlags & MPV_RENDER_UPDATE_FRAME) != 0 || framePending_;
    bool freshOutputFrame = false;
    if (engineFrameHoldAwaitingOutput_) {
        mpv_render_frame_info frameInfo{};
        freshOutputFrame = render_->nextFrameInfo(frameInfo) && isRealNewFrame(frameInfo);
    }
    // forceRender_ covers the framebuffer Qt hands back empty: a resize or a
    // re-show wipes the picture, and with a paused core no new frame is coming.
    const bool haveFrame = framePending_ || newOutputFrame || forceRender_;

    const bool retainedPictureVisible =
        engineFrameHoldFbo_ &&
        (engineFrameHoldActive_ || (engineFrameHoldAwaitingOutput_ && !freshOutputFrame));
    bool presentedFrame = false;

    // Qt renders QOpenGLWidget into its own framebuffer object, so fbo=0 would
    // make mpv bind the window's default framebuffer and paint nothing. That
    // object is sized in device pixels from the same rounded product Qt uses, so
    // a fractional DPR must not truncate here or mpv would fill a smaller
    // rectangle than the buffer it was handed.
    const GLuint target = static_cast<GLuint>(defaultFramebufferObject());
    const QSize deviceSize = QSize(width(), height()) * devicePixelRatioF();
    const int targetWidth = std::max(1, deviceSize.width());
    const int targetHeight = std::max(1, deviceSize.height());
    // QOpenGLWidget guarantees its own context is current here, so the render
    // context and this FBO are the ones these entry points act on.
    QOpenGLContext* glContext = QOpenGLContext::currentContext();
    if (glContext == nullptr) {
        // There is no context to render into or to report from, and guessing at
        // one would be worse than leaving the picture out.
        qWarning("VideoSurface: paintGL without a current OpenGL context");
        return;
    }
    QOpenGLFunctions* gl = glContext->functions();
    // Clear reused Qt buffers before rendering, including pixels outside the video rectangle.
    gl->glBindFramebuffer(GL_FRAMEBUFFER, target);
    gl->glViewport(0, 0, targetWidth, targetHeight);
    // Qt enables blending before paintGL; libmpv requires its default disabled state.
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);

    // The boundary around mpv's raw GL, which maps logical widget coordinates
    // onto this framebuffer. mpv leaves the vertex-array binding and unpack row
    // length behind and Qt only re-establishes its own state on the next paint,
    // so both are put back inside the native window.
    QPainter painter(this);
    painter.beginNativePainting();
    int renderResult = 0;
    if (retainedPictureVisible && drawEngineFrameHold(target, targetWidth, targetHeight)) {
        if (newOutputFrame) {
            renderResult = render_->skipRendering();
            if (renderResult == 0) render_->reportSwap();
        }
    } else {
        renderResult = render_->render(static_cast<int>(target), targetWidth, targetHeight, 0);
        presentedFrame = haveFrame;
    }
    if (QOpenGLExtraFunctions* extra = glContext->extraFunctions()) {
        extra->glBindVertexArray(0);
        extra->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        extra->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, target);
    painter.endNativePainting();
    lastRenderedFboSize_ = deviceSize;
    hasRenderedFrame_ = hasRenderedFrame_ || renderResult == 0;
    if (renderResult < 0 && engineFrameHoldFbo_ &&
        (engineFrameHoldActive_ || engineFrameHoldAwaitingOutput_)) {
        painter.beginNativePainting();
        drawEngineFrameHold(target, targetWidth, targetHeight);
        painter.endNativePainting();
    }
    if (renderResult == 0 && freshOutputFrame && !retainedPictureVisible) {
        hasVisibleOutputFrame_ = true;
        engineFrameHoldAwaitingOutput_ = false;
        // The fresh output has replaced the transition snapshot. Release its
        // GPU storage now; normal playback retains no extra video framebuffer.
        engineFrameHoldFbo_.reset();
    }
    if (renderResult == 0 && freshOutputFrame && !retainedPictureVisible &&
        animeJanaiPlan_.enabled && animeJanaiPlan_.available &&
        !animeJanaiPlan_.requiresAji && animeJanaiFilterBound_) {
        setAnimeJanaiStatus(QStringLiteral("Anime4K output observed"));
    }

    // Monotonic: paintCanHappen() reads it as "this widget has painted at least
    // once", which a failed render does not undo.
    if (newOutputFrame && renderResult == 0 && presentedFrame) {
        hasVisibleOutputFrame_ = true;
        ++diagnostics_.renderedOutputFrames;
    }
    if (renderResult < 0) {
        // The buffer holds the defined black rather than that frame, so a report
        // for it would name a presentation that is no longer on screen.
        videoFramePainted_ = false;
        if (mpv_ != nullptr) {
            const QString message =
                tr("Video render failed (mpv: %1)")
                    .arg(QString::fromStdString(mpv_->describeError(renderResult)));
            QMetaObject::invokeMethod(this, [this, message] { emit errorOccurred(message); },
                                      Qt::QueuedConnection);
        }
    } else if (presentedFrame) {
        // Only ever set: a UI-only repaint must not throw away the report the
        // frame before it is still owed.
        videoFramePainted_ = true;
    }
    framePending_ = false;
    forceRender_ = false;

    // No self-update here: mpv's own update callback is what asks for the next
    // paint, so scheduling one from paintGL would only spin on a paused core.
    if (!overlaySuppressed_) emit paintOverlay(&painter);
}

void VideoSurface::onRenderUpdate(quint64 generation) {
    // A hop queued by a render context that has since been released: the widget
    // it would touch no longer owns a renderer.
    if (generation != renderGeneration_) return;
    queuedRenderCallback_.store(0);
    advanceRenderState();
}

void VideoSurface::advanceRenderState() {
    if (!contextReady_ || render_ == nullptr) return;
    QOpenGLContext* previous = nullptr;
    if (!makeRenderContextCurrent(&previous)) return;
    // update() runs on every callback, painted or not: in advanced mode it is
    // what releases the core's queued work and allocates its decoder textures,
    // and Qt skips paintGL for an unexposed widget.
    const uint64_t flags = render_->update();
    const bool frame = (flags & MPV_RENDER_UPDATE_FRAME) != 0;
    if (paintCanHappen()) {
        framePending_ = framePending_ || frame;
        if (framePending_ || forceRender_) update();
    } else {
        // Nothing can be composited now, so the work mpv waits on is answered
        // here with ONE report: its new frame is consumed by a skip (which the
        // render API counts as rendered), or the frame this surface painted but
        // the widget will never present is acked as the intentional drop it is.
        const bool owed = videoFramePainted_ || frame;
        const bool consumed = !frame || render_->skipRendering() == 0;
        framePending_ = false;
        if (consumed) {
            videoFramePainted_ = false;
            if (owed) render_->reportSwap();
        }
    }
    releaseRenderContextCurrent(previous);
}

bool VideoSurface::paintCanHappen() const {
    if (!isVisible()) return false;
    // An opaque sibling can suppress paints without hiding the widget or window.
    if (visibleRegion().isEmpty()) return false;
    const QWindow* handle = window() != nullptr ? window()->windowHandle() : nullptr;
    if (handle == nullptr) return false;
    if ((handle->windowState() & Qt::WindowMinimized) != 0) return false;
    // Once this surface has painted, an unexposed window is one Qt is not
    // compositing, so a frame kept for it would never be presented nor acked.
    // Before the first paint there is nothing to lose: the render context exists
    // because Qt was already painting this widget.
    return !hasRenderedFrame_ || handle->isExposed();
}

void VideoSurface::onFrameSwapped() {
    if (!videoFramePainted_) return;
    videoFramePainted_ = false;
    if (!contextReady_ || render_ == nullptr) return;
    QOpenGLContext* previous = nullptr;
    if (!makeRenderContextCurrent(&previous)) return;
    // mpv reads the moment this arrives as the moment the frame reached the
    // screen, which is exactly what the signal it is sent from reports.
    render_->reportSwap();
    releaseRenderContextCurrent(previous);
}

void VideoSurface::acknowledgeDroppedFrame() {
    if (!videoFramePainted_) return;
    videoFramePainted_ = false;
    if (!contextReady_ || render_ == nullptr) return;
    QOpenGLContext* previous = nullptr;
    if (!makeRenderContextCurrent(&previous)) return;
    render_->reportSwap();
    releaseRenderContextCurrent(previous);
}

void VideoSurface::refreshRenderState() {
    // The chain changed under a possibly paused core, so the next paint has to
    // redraw even when mpv announces no new frame.
    forceRender_ = true;
    advanceRenderState();
}

bool VideoSurface::makeRenderContextCurrent(QOpenGLContext** previous) {
    if (previous != nullptr) *previous = nullptr;
    if (context_ == nullptr || !contextReady_) return false;
    QOpenGLContext* before = QOpenGLContext::currentContext();
    if (previous != nullptr) *previous = before;
    if (before == context_) return true;
    QOpenGLWidget::makeCurrent();
    if (QOpenGLContext::currentContext() != context_) {
        qWarning("VideoSurface: could not make the render context current");
        return false;
    }
    return true;
}

void VideoSurface::releaseRenderContextCurrent(QOpenGLContext* previous) {
    // One that WAS current before is put back with its own surface: frameSwapped
    // can run while a different compositor context is current, and Qt's
    // composition goes on using that one after this returns.
    if (previous == context_) return;
    if (previous != nullptr) {
        if (QSurface* surface = previous->surface()) {
            previous->makeCurrent(surface);
            return;
        }
    }
    // Nothing to hand back, so the context this call made current is released
    // instead of being left behind.
    QOpenGLWidget::doneCurrent();
}

void VideoSurface::releaseRenderContext() {
    if (render_ == nullptr) return;
    QOpenGLContext* previous = nullptr;
    const bool current = makeRenderContextCurrent(&previous);
    if (current) engineFrameHoldFbo_.reset();
    if (current) {
        // The render context's destructor detaches mpv's update callback before
        // it frees the context: a callback in flight cannot reach a freed one.
        // Freeing also makes mpv kill the old video output and error its track,
        // so the playback error that follows is marked as this surface's doing.
        rendererTeardownErrorPending_ = true;
        render_.reset();
        releaseRenderContextCurrent(previous);
    }
    if (contextDestroyedConnection_) {
        disconnect(contextDestroyedConnection_);
        contextDestroyedConnection_ = QMetaObject::Connection();
    }
    contextReady_ = false;
    context_ = nullptr;
    engineFrameHoldFbo_.reset();
    engineFrameHoldActive_ = false;
    engineFrameHoldAwaitingOutput_ = false;
    hasVisibleOutputFrame_ = false;
    videoFramePainted_ = false;
    framePending_ = false;
    forceRender_ = false;
    // Any hop the released context's callback queued is now stale.
    ++renderGeneration_;
    if (!current) {
        // mpv_render_context_free() releases GL objects and drains core requests
        // queued for the renderer, so it needs THIS widget's context current;
        // with none - or with somebody else's - it faults inside the driver.
        // The invariant is that the context dies before the widget, so there is
        // no recoverable state here to fake: stop rather than free, leak or
        // corrupt.
        qFatal("VideoSurface: the mpv render context cannot be released without its OpenGL "
               "context current (the widget outlived its context)");
    }
}

// ── state pump ────────────────────────────────────────────────────────────

void VideoSurface::observePlaybackState() {
    if (!mpv_) return;
    if (!observers_.empty()) return;
    // The registered format is what mpv delivers, so each observer reads
    // exactly what it wrote.
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
        observedPaused_ = paused;
    });
    watch("video-params/w", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedVideoWidth_ = p.isNumber() ? p.number : 0.0; });
    watch("video-params/h", MPV_FORMAT_DOUBLE,
          [this](const Property& p) { observedVideoHeight_ = p.isNumber() ? p.number : 0.0; });
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
    for (const char* property : {"track-list", "aid", "sid"}) {
        watch(property, MPV_FORMAT_NONE, [this](const Property&) { mediaTracksDirty_ = true; });
    }
}

void VideoSurface::setNextMediaTrackSelection(MediaTrackSelection selection) {
    nextMediaTrackSelection_ = std::move(selection);
}

void VideoSurface::resetMediaTracks() {
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

void VideoSurface::refreshMediaTracks() {
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

bool VideoSurface::selectMediaTrack(MediaTrackType type, int64_t id,
                                     uint64_t expectedCoreIdentity, uint64_t expectedSourceEpoch) {
    if (mpv_ == nullptr || !mediaTracks_.ready || expectedCoreIdentity != mpv_->coreIdentity() ||
        expectedSourceEpoch != mpv_->sourceEpoch() || mediaTracks_.coreIdentity != expectedCoreIdentity ||
        mediaTracks_.sourceEpoch != expectedSourceEpoch) {
        emit mediaTracksError(tr("The media changed. Reopen the track menu."));
        return false;
    }
    const bool available = id == 0 || std::any_of(
        mediaTracks_.tracks.cbegin(), mediaTracks_.tracks.cend(),
        [type, id](const MediaTrack& track) { return track.type == type && track.id == id; });
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
                emit mediaTracksError(tr("Cannot select %1: %2")
                    .arg(type == MediaTrackType::Audio ? tr("audio track") : tr("subtitle track"),
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

void VideoSurface::publishObservedState() {
    if (observedDuration_ > 0 && observedDuration_ != duration_) {
        duration_ = observedDuration_;
        emit durationChanged(duration_);
    }
    // 10 ms is finer than any display, so this cannot look stepped.
    if (std::abs(observedPosition_ - position_) > 0.01) {
        position_ = observedPosition_;
        emit positionChanged(position_);
    }
    // Compare like with like: the property is "pause", the cached state is
    // "playing". Guarding on `paused != playing_` means a playing video
    // (false vs false) never updates, so isPlaying() stays false forever. A write
    // still in flight is the viewer's intent, so an observation that has not
    // caught up with it cannot flip the control back.
    const bool playing = !requestedPause() && !enginePauseHeld();
    if (playing != playing_) {
        playing_ = playing;
        emit playingChanged(playing_);
    }
}

void VideoSurface::pumpEvents() {
    if (!mpv_) return;
    // Before the drain, so an observation dispatched from it is read under the
    // epoch the controller is currently bound to.
    syncNativeSource();
    if (animeJanaiEpoch_ != 0 && animeJanaiEpoch_ != mpv_->sourceEpoch()) {
        invalidateEngineFrameHold();
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
    // The last thing the pump does: a request issued here is answered by the
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

void VideoSurface::updateStreamStats() {
    if (!statsClock_.isValid()) {
        statsClock_.start();
        return;
    }
    if (statsClock_.elapsed() < 500) {
        return;
    }
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
    emit streamStatsChanged(networkBytesPerSecond_, streamFramesPerSecond_,
                            streamFpsSource_);
}

void VideoSurface::handleState() {
    if (!mpv_) return;
    // One value, two answers. The width decides both whether there is a video at
    // all and how wide it is, and it was observed rather than read, so this costs
    // no trip through the core lock against mpv's own render thread.
    const int w = static_cast<int>(observedVideoWidth_);
    const bool vid = w > 0;
    if (vid != hasVideo_) hasVideo_ = vid;
    if (hasVideo_) {
        const int h = static_cast<int>(observedVideoHeight_);
        if (w != videoWidth_ || h != videoHeight_) {
            videoWidth_ = w;
            videoHeight_ = h;
            emit videoSizeChanged(w, h);
        }
    }
}


PlaybackDiagnostics VideoSurface::playbackDiagnostics() const {
    return diagnostics_;
}

void VideoSurface::resetDiagnostics() {
    for (DiagnosticRequest& request : diagnosticRequests_) {
        if (request.pending.valid() && mpv_) mpv_->cancelAsync(request.pending);
        request = DiagnosticRequest{};
    }
    // Unknown, not zero: the previous source's numbers say nothing about this one.
    diagnostics_ = PlaybackDiagnostics{};
    diagnosticsLastPollMs_ = diagnosticsClock_.elapsed();
}

void VideoSurface::pollDiagnostics() {
    if (!mpv_ || !diagnosticsActive_) return;
    const qint64 nowMs = diagnosticsClock_.elapsed();
    if (nowMs - diagnosticsLastPollMs_ < kDiagnosticsPollIntervalMs) return;
    diagnosticsLastPollMs_ = nowMs;
    // Bumped by load/stop: a reply from an old epoch is dropped, not published.
    const uint64_t epoch = mpv_->sourceEpoch();
    issueDiagnostic(kDecoderDropSlot, kDecoderDropProperty, epoch);
    issueDiagnostic(kVoDropSlot, kVoDropProperty, epoch);
    issueDiagnostic(kVfSlot, kVfProperty, epoch);
}

void VideoSurface::issueDiagnostic(DiagnosticSlot slot, const char* property, uint64_t epoch) {
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

void VideoSurface::applyDiagnosticReply(DiagnosticSlot slot, const AsyncReply& reply) {
    switch (slot) {
        case kDecoderDropSlot:
            diagnostics_.decoderDropCount = exactInteger(reply);
            break;
        case kVoDropSlot:
            diagnostics_.voDropCount = exactInteger(reply);
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

void VideoSurface::setDiagnosticUnknown(DiagnosticSlot slot) {
    switch (slot) {
        case kDecoderDropSlot:
            diagnostics_.decoderDropCount.reset();
            break;
        case kVoDropSlot:
            diagnostics_.voDropCount.reset();
            break;
        case kVfSlot:
            diagnostics_.nativeFilterPresent.reset();
            diagnostics_.vfCount.reset();
            break;
    }
}

}  // namespace mplayer
