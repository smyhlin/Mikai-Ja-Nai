#include "VideoSurface.hpp"

#include <QCoreApplication>
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
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "gui/IdleInhibitor.hpp"
#include "gui/UiProfile.hpp"

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

// A recreated context gets 50 ms x 12 - two thirds of a second - of forced
// paints to prove the live core still delivers output before the source is
// reloaded. Beating 60 Hz, this is long enough for a frame to arrive and short
// enough not to read as a stall.
constexpr int kContextReattachTicks = 12;

constexpr int kUnpaintableGraceTicks = 2;
// Bounded re-check of an unpaintable widget: about one 60 Hz frame, so a grace
// that became paintable again is not left waiting for a callback mpv may never
// raise for it.
constexpr int kUnpaintableRetryMs = 16;
constexpr double kMinRenderLeadMs = 0.25;
constexpr double kMaxRenderLeadMs = 8.0;

// Integer GL tokens this file needs that the GL 1.1 headers do not define. They
// are values, not an extension check: each readback below is only used for the
// format the target was probed at, which the driver already supports because it
// created that attachment.
constexpr GLenum kGlRgba = 0x1908;
constexpr GLenum kGlUnsignedByte = 0x1401;
constexpr GLenum kGlUnsignedShort = 0x1403;
constexpr GLenum kGlHalfFloat = 0x140B;                    // GL 3.0
constexpr GLenum kGlPackAlignment = 0x0D05;
constexpr GLenum kGlReadFramebuffer = 0x8CA8;
constexpr GLenum kGlColorAttachmentObjectType = 0x8CD0;
constexpr GLenum kGlColorAttachmentObjectName = 0x8CD1;
constexpr GLenum kGlTextureObject = 0x1702;
constexpr GLenum kGlRenderbufferObject = 0x8D41;
constexpr GLenum kGlTextureInternalFormat = 0x1003;
constexpr GLenum kGlRenderbufferInternalFormat = 0x8D44;

// A defined backdrop for a surface with nothing to show. Deliberately the same
// tone the opening placeholder uses, because both say "this is a stage, not a
// frame" rather than the bare black a failed clear leaves behind.
const QColor& videoBackdrop() {
    static const QColor colour(9, 10, 14);
    return colour;
}

/// Antialiased alpha coverage of one rounded corner, rasterised at `size` device
/// pixels: opaque inside the quarter disc, transparent outside it. The patch's
/// own top-left pixel is the widget's corner and the straight edges past the arc
/// stay at full coverage, so a patch can be blitted without a seam.
QImage cornerCoveragePatch(int size, qreal radiusDevice) {
    QImage patch(size, size, QImage::Format_ARGB32_Premultiplied);
    if (patch.isNull()) return patch;
    patch.fill(Qt::transparent);
    QPainter painter(&patch);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    // The quarter disc of a rounded rect whose corner radius spans the whole
    // arc; everything past it - the part this patch extends into - is filled.
    QPainterPath shape;
    shape.addRoundedRect(QRectF(0.0, 0.0, radiusDevice * 2.0, radiusDevice * 2.0),
                         radiusDevice, radiusDevice);
    painter.drawPath(shape);
    return patch;
}

/// Monotonic nanoseconds for local duration measurements only. MPV target_time
/// comparisons use mpv_get_time_us(), because it is a distinct clock domain.
qint64 steadyClockNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Process-wide options are adopted by each live surface on its state pump.
VideoSurface::HdrToneMappingPolicy g_hdrToneMappingPolicy =
    VideoSurface::HdrToneMappingPolicy::Auto;
int g_hdrToneMappingPolicyRevision = 1;
bool g_animeJanaiHdrOverride = false;
int g_animeJanaiHdrOverrideRevision = 1;
QString g_frameTimingLogPath;

// Display scaling is a viewing preference, not a per-window state: one policy
// per process, adopted by every live surface on its own state pump, exactly
// like the HDR policy above.
VideoScalePolicy g_videoScalePolicy;
int g_videoScalePolicyRevision = 1;

int hdrToneMappingPolicyRevision() { return g_hdrToneMappingPolicyRevision; }

int animeJanaiHdrOverrideRevision() { return g_animeJanaiHdrOverrideRevision; }
bool hdrFixesEnabled() {
    static const bool disabled = qEnvironmentVariable("MIKAI_HDR_FIXES") == QStringLiteral("0");
    return !disabled;
}

auto hdrPolicyOptionList(VideoSurface::HdrToneMappingPolicy policy) {
    const bool stable = policy == VideoSurface::HdrToneMappingPolicy::Stable;
    const bool vivid = policy == VideoSurface::HdrToneMappingPolicy::Vivid;
    using Option = std::pair<const char*, const char*>;
    std::vector<Option> options{
        Option{"dither-depth", "auto"},
        Option{"tone-mapping", vivid ? "mobius" : "bt.2390"},
    };
    // tone-mapping-param is a float option whose reset token exists only as an
    // internal default: setting the literal "default" is refused at runtime,
    // and bt.2390 ignores the parameter anyway. Only the policy that defines a
    // value sets it.
    if (vivid) options.emplace_back("tone-mapping-param", "0.3");
    options.emplace_back("hdr-compute-peak", stable ? "no" : "yes");
    options.emplace_back("hdr-peak-percentile", "99.995");
    options.emplace_back("gamut-mapping-mode", stable ? "clip" : "auto");
    return options;
}

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
// Frames the VO took out of their named presentation slot. This is the number
// that separates "the compositor could not keep up" from "the decoder did".
constexpr const char* kVoDelayedProperty = "vo-delayed-frame-count";
constexpr const char* kVfProperty = "vf";


/// The `lib=` option the native filter needs to open its inference shim: the
/// producer library of the validated native bundle this process actually
/// loaded, proved against that bundle's own inventory digest by resolution.
/// Empty when the loaded runtime is not a native bundle or the worker's exact
/// descriptor/stat-tree proof is stale - the binding is then probe-only, and
/// an admission against it is refused by the core rather than loaded from a
/// guessed path.
QVector<native::NativeFilterOption> provedNativeAttachOptions() {
    const native::RuntimeSelection& selection = native::currentSelection();
    if (!selection.loaded || selection.kind != native::BundleKind::Native ||
        !selection.resolvedBundle.has_value()) {
        return {};
    }
    const native::Bundle& bundle = *selection.resolvedBundle;
    // Resolution already hashed the inventory on its worker. An unchanged proof
    // is checked here without re-reading a potentially large DLL on the GUI.
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
    // The percentage names a scale factor; mpv's video-zoom is its log2, so
    // -50 % is -1.000000 and +100 % is 1.000000. Fixed six decimals with Qt's
    // own formatting: std::to_string would emit a comma under uk_UA and the
    // core would refuse the option.
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
            return QCoreApplication::translate("mplayer::videoscale", "Fit to window");
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
    // adds no QSettings/INI allocation per call. Installing the static stays the
    // caller's step: startup and a store-refresh reach the value at different
    // moments.
    VideoScalePolicy policy;
    const int storedMode = settings.value(
        QString::fromLatin1(VideoSurface::kVideoScaleModeSettingKey),
        static_cast<int>(VideoScaleMode::Fit)).toInt();
    if (storedMode >= static_cast<int>(VideoScaleMode::Fit) &&
        storedMode <= static_cast<int>(VideoScaleMode::Original)) {
        policy.mode = static_cast<VideoScaleMode>(storedMode);
    }
    policy.zoomPercent =
        settings.value(QString::fromLatin1(VideoSurface::kVideoZoomSettingKey), 0).toInt();
    policy.upscale = settings.value(
        QString::fromLatin1(VideoSurface::kVideoUpscaleSettingKey),
        QStringLiteral("lanczos")).toString();
    policy.downscale = settings.value(
        QString::fromLatin1(VideoSurface::kVideoDownscaleSettingKey), QString()).toString();
    return policy;
}

bool VideoSurface::hdrRenderingFixesEnabled() {
    return hdrFixesEnabled();
}

void VideoSurface::ensureEngine() {
    if (mpv_) return;
    // Entry ids belong to one core's playlist; a core created here has never
    // reported one, so nothing a previous core named can be matched against it.
    pendingLoadTokens_.clear();
    entryTokensById_.clear();
    // These are the options the Python player passed through python-mpv, which
    // spelled them with underscores; mpv itself takes dashes.
    std::vector<std::pair<std::string, std::string>> options{
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
        // Qt owns presentation, so render calls stay nonblocking. mpv documents
        // video-timing-offset=0 as an alternative to its render-time wait; this
        // client also schedules against target_time using mpv's own clock.
        {"video-timing-offset", "0"},
    };
    // The display mode is applied before initialize so the first frame is
    // already in it. An Auto downscale is left out here on purpose: dscale's
    // real default can only be read from an initialized core, and the pinned
    // runtime refuses the literal "auto". applyVideoScaleToEngine() below
    // queries it and sets the whole set before any file is loaded.
    for (const VideoScaleOption& option : videoScaleOptions(videoScalePolicy(), {})) {
        options.emplace_back(option.name, option.value);
    }
    // The default belongs to the core that answered it, and this is a new one.
    downscaleDefault_.reset();
    downscaleDefaultError_.reset();
    downscaleWasExplicit_ = false;
    mpv_ = std::make_unique<MpvHandle>(options);
    mpvApiVersion_ = MpvHandle::apiVersion();
    applyHdrPolicyToEngine();
    applyVideoScaleToEngine(/*queryCoreDefault=*/true);
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
                    // armed is dropped here - but only for the load still current:
                    // a refusal for a superseded submission says nothing about the
                    // one that replaced it.
                    if (load.epoch == mpv_->sourceEpoch()) {
                        engineFrameHoldNeedsLoadLanding_ = false;
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
                            // loaded file, so this latches only when FILE_LOADED has
                            // already reported for this entry (the same condition
                            // the event's own branch uses) and the epoch is current.
                            // An episode token is reused across quality and variant
                            // swaps, so it cannot tell two loads apart.
                            if (engineFrameHoldNeedsLoadLanding_ && entryLoadReported_ &&
                                load.epoch == mpv_->sourceEpoch()) {
                                engineFrameHoldLandingSeen_ = true;
                            }
                            if (entryLoadReported_ && load.epoch == mpv_->sourceEpoch()) {
                                mediaTracksLoaded_ = true;
                                mediaTracksDirty_ = true;
                                // FILE_LOADED may precede the command reply that
                                // finally identifies this entry. Publish only
                                // now, rather than emitting an unnamed token.
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
                // Only an entry the reply has identified, on the epoch that is
                // current now, proves this surface's load landed. An unnamed
                // event is not evidence: the stale-event guard below exists
                // precisely because a FILE_LOADED for a superseded source can
                // arrive after a replacement was submitted.
                if (entrySourceEpoch_ != 0 && entrySourceEpoch_ == mpv_->sourceEpoch()) {
                    mediaTracksLoaded_ = true;
                    mediaTracksDirty_ = true;
                    if (engineFrameHoldNeedsLoadLanding_) engineFrameHoldLandingSeen_ = true;
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
            // An ended file is not an open source to replay on GL recreation.
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
        engineFrameHoldNeedsLoadLanding_ = false;
        // The wait ended in a failure; an overlay that kept claiming to open
        // something would be the only thing left on screen.
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
    const QString requested = anime4k::shaderStringForCandidate(chainId);
    if (!requested.isEmpty()) {
        const QString shaders = resolvedChainForCurrentSource(chainId);
        if (shaders.isEmpty()) {
            emit anime4kGated(tr("Anime4K skipped: source transfer metadata is unknown or HDR"));
        } else {
            applyShaderChainRequest(shaders);
        }
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
    const QString requested = anime4k::shaderStringForCandidate(candidateId);
    const QString shaders = resolvedChainForCurrentSource(candidateId);
    if (!requested.isEmpty() && shaders.isEmpty()) {
        emit anime4kGated(tr("Anime4K skipped: source transfer metadata is unknown or HDR"));
    }
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
    const QString requested = anime4k::shaderStringForCandidate(anime4kChainId_);
    // Unknown or HDR transfer metadata gates the chain; the requested id remains
    // remembered so it can be restored when a known SDR source arrives.
    const QString shaders = resolvedChainForCurrentSource(anime4kChainId_);
    if (!requested.isEmpty() && shaders.isEmpty()) {
        emit anime4kGated(tr("Anime4K skipped: source transfer metadata is unknown or HDR"));
    }
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
    // One nonblocking timer defers pending frames to mpv's presentation slot.
    pacingTimer_.setSingleShot(true);
    pacingTimer_.setTimerType(Qt::PreciseTimer);
    connect(&pacingTimer_, &QTimer::timeout, this, [this] { onPacingTick(); });
    // A placeholder that never moves is a frozen frame to a viewer. It only runs
    // while the surface is opening and has no engine to paint with, so it costs
    // nothing during playback or on an idle surface.
    placeholderTimer_.setInterval(90);
    connect(&placeholderTimer_, &QTimer::timeout, this, [this] {
        ++placeholderPhase_;
        update();
    });
    // A recreated context asks the live core whether it still delivers output.
    // The budget is time, not a paint count: Qt may coalesce these updates into
    // one paint, and what the attempt needs is a bounded window in which a frame
    // could have arrived.
    reattachTimer_.setSingleShot(false);
    reattachTimer_.setInterval(50);
    connect(&reattachTimer_, &QTimer::timeout, this, [this] {
        if (!reattachPending_) return;
        if (--reattachTicksLeft_ <= 0) {
            finishContextReattach();
            return;
        }
        if (paintCanHappen()) update();
    });
    // Keep a replayable snapshot ahead of external listeners: a dialog that
    // reconnects after missing the original signal can synchronize from these
    // typed getters without dynamic properties or a second event source.
    connect(this, &VideoSurface::animeJanaiEngineBuildChanged, this,
            [this](bool active, bool succeeded, const QString& detail) {
                Q_UNUSED(succeeded);
                animeJanaiEngineBuildActive_ = active;
                animeJanaiEngineBuildDetail_ = detail;
                publishEnginePauseHold();
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
        if (!engineFrameHoldFbo_) {
            // A filter change needs no load to land: any real frame may release
            // this snapshot, so it must not inherit a pending landing gate.
            engineFrameHoldNeedsLoadLanding_ = false;
            engineFrameHoldLandingSeen_ = false;
            captureEngineFrameHold();
        }
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

void VideoSurface::holdPictureAcrossLoad() {
    if (engineFrameHoldActive_) return;              // a freeze transition owns it
    if (engineFrameHoldFbo_ && engineFrameHoldAwaitingOutput_) {
        // The snapshot is already the right one, but the gate belongs to the
        // load about to be issued, not to the last one that landed.
        engineFrameHoldNeedsLoadLanding_ = true;
        engineFrameHoldLandingSeen_ = false;
        update();
        return;
    }
    if (hasVisibleOutputFrame_ && captureEngineFrameHold()) {
        // Awaiting, not active: the held picture stands in only until the new
        // source delivers, and then releases the GPU storage. `active` would
        // keep it up over a picture that has arrived.
        engineFrameHoldAwaitingOutput_ = true;
        // The outgoing source can still hand out one more real frame after this
        // capture, and mpv bumps the command epoch at SUBMISSION, so "a frame
        // with a newer epoch" is true for the outgoing source too. What proves
        // the replacement is mpv reporting FILE_LOADED for this surface's own
        // load request, which is why the release waits for that landing.
        engineFrameHoldNeedsLoadLanding_ = true;
        engineFrameHoldLandingSeen_ = false;
        update();
        return;
    }
    // No picture to keep, but the one read out of a dying context is still the
    // frame the viewer was looking at: this reload window is exactly what it is
    // for, so it is not cleared here.
    if (teardownPicture_.isNull()) {
        invalidateEngineFrameHold();
    } else {
        clearEngineFrameHold();
    }
}

void VideoSurface::clearEngineFrameHold() {
    engineFrameHoldActive_ = false;
    engineFrameHoldAwaitingOutput_ = false;
    engineFrameHoldNeedsLoadLanding_ = false;
    engineFrameHoldLandingSeen_ = false;
    hasVisibleOutputFrame_ = false;
    if (!engineFrameHoldFbo_) return;
    QOpenGLContext* previous = nullptr;
    const bool current = makeRenderContextCurrent(&previous);
    engineFrameHoldFbo_.reset();
    if (current) releaseRenderContextCurrent(previous);
}

void VideoSurface::invalidateEngineFrameHold() {
    clearEngineFrameHold();
    // A source boundary retires the presented copy for the same reason: the
    // picture it holds belongs to the source that is going away.
    invalidatePresentedPicture();
    // The teardown picture belongs to the same boundary: an intentional clear,
    // a close or a new source retires it with the hold. A load that has one to
    // show uses clearEngineFrameHold() instead.
    teardownPicture_ = QImage();
    teardownPictureFormat_ = 0;
}

qint64 VideoSurface::retainedPictureBytes() const {
    qint64 bytes = teardownPicture_.sizeInBytes();
    if (presentedPictureFbo_ && presentedPictureFbo_->isValid()) {
        const int perPixel = presentedPictureFormat_ == kGlInternalFormatRgba16F ||
                                     presentedPictureFormat_ == kGlInternalFormatRgba16
                                 ? 8
                                 : 4;
        bytes += qint64(presentedPictureFbo_->width()) * presentedPictureFbo_->height() * perPixel;
    }
    if (engineFrameHoldFbo_ && engineFrameHoldFbo_->isValid()) {
        // The snapshot is stored at the target's own precision, so its per-pixel
        // cost follows the probed format, not a fixed RGBA8 assumption.
        const int perPixel =
            renderInternalFormat_ == kGlInternalFormatRgba16F ||
                    renderInternalFormat_ == kGlInternalFormatRgba16
                ? 8
                : 4;
        bytes += qint64(engineFrameHoldFbo_->width()) * engineFrameHoldFbo_->height() * perPixel;
    }
    return bytes;
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
    // The snapshot is a copy of the widget's own target, so it takes that
    // target's probed format and per-channel depth: an 8-bit copy of a 10-bit or
    // float target would quantize the picture the hold exists to preserve. RGBA8
    // is only the fallback for a target whose format could not be read.
    const bool probed = renderFormatProbed_ && renderInternalFormat_ != 0;
    const int snapshotFormat = probed ? renderInternalFormat_ : kGlInternalFormatRgba8;
    const int snapshotDepth = probed ? renderDepthBitsPerChannel_ : 0;
    QOpenGLFramebufferObjectFormat snapshotFboFormat;
    snapshotFboFormat.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    snapshotFboFormat.setInternalTextureFormat(snapshotFormat);
    auto snapshot = std::make_unique<QOpenGLFramebufferObject>(size, snapshotFboFormat);
    QOpenGLFunctions* gl = context_->functions();
    bool frameConsumed = false;
    bool captured = false;
    {
        // Isolate capture from Qt's overlay state; end this painter before changing contexts.
        QOpenGLPaintDevice device(size);
        device.setDevicePixelRatio(1.0);
        QPainter painter(&device);
        painter.beginNativePainting();
        if (snapshot->isValid() && snapshot->bind()) {
            gl->glViewport(0, 0, size.width(), size.height());
            gl->glDisable(GL_BLEND);
            gl->glDisable(GL_SCISSOR_TEST);
            // New attachments have undefined pixels outside the video rectangle.
            gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            gl->glClear(GL_COLOR_BUFFER_BIT);
            // Copying the widget FBO would retain HUD pixels.
            const uint64_t updateFlags = render_->update();
            frameConsumed = framePending_ || (updateFlags & MPV_RENDER_UPDATE_FRAME) != 0;
            // The same format and depth the widget target gets, so mpv dithers
            // for the precision this snapshot really stores.
            const int result =
                render_->render(snapshot->handle(), size.width(), size.height(),
                                snapshotFormat, snapshotDepth);
            captured = result >= 0;
        }
        if (QOpenGLExtraFunctions* extra = context_->extraFunctions()) {
            extra->glBindVertexArray(0);
            extra->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            extra->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        }
        gl->glBindFramebuffer(GL_FRAMEBUFFER,
                              static_cast<GLuint>(defaultFramebufferObject()));
        painter.endNativePainting();
        painter.end();
    }
    if (!captured) {
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
    // A filtered blit between a fixed-point and a floating-point buffer is not
    // defined by GL, so the snapshot and the target are compared instead of
    // assuming they match: they only differ on a target whose format is unknown.
    const GLenum filter =
        static_cast<int>(engineFrameHoldFbo_->format().internalTextureFormat()) ==
                renderInternalFormat_
            ? GL_LINEAR
            : GL_NEAREST;
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, engineFrameHoldFbo_->handle());
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    gl->glBlitFramebuffer(0, 0, engineFrameHoldFbo_->width(), engineFrameHoldFbo_->height(),
                          0, 0, width, height, GL_COLOR_BUFFER_BIT, filter);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
    return true;
}

void VideoSurface::invalidatePresentedPicture() {
    presentedPictureFbo_.reset();
    presentedPictureFormat_ = 0;
}

bool VideoSurface::drawPresentedPicture(unsigned int target, int width, int height) {
    if (!presentedPictureFbo_ || !presentedPictureFbo_->isValid() || width <= 0 || height <= 0) {
        return false;
    }
    if (presentedPictureFbo_->size() != QSize(width, height)) {
        // A new target size makes the copy describe a shape that is gone.
        invalidatePresentedPicture();
        return false;
    }
    QOpenGLContext* glContext = QOpenGLContext::currentContext();
    if (glContext == nullptr) return false;
    QOpenGLExtraFunctions* gl = glContext->extraFunctions();
    if (gl == nullptr) return false;
    // A filtered blit between a fixed-point and a floating-point buffer is not
    // defined by GL, so the two formats are compared rather than assumed equal.
    // The caller already bound `target`. Querying the previous binding here is a
    // pipeline stall on every chrome repaint, and restoring to `target` is the
    // state that caller still owns.
    const GLenum filter =
        presentedPictureFormat_ == renderInternalFormat_ ? GL_LINEAR : GL_NEAREST;
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, presentedPictureFbo_->handle());
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    gl->glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, filter);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, target);
    return true;
}

void VideoSurface::capturePresentedPicture(unsigned int target, int width, int height) {
    if (width <= 0 || height <= 0 || context_ == nullptr) {
        invalidatePresentedPicture();
        return;
    }
    if (presentedPictureFbo_ && presentedPictureFbo_->size() != QSize(width, height)) {
        invalidatePresentedPicture();
    }
    if (!presentedPictureFbo_) {
        // The copy takes the widget target's own probed format, for the same
        // reason the transition hold does: an 8-bit copy of a 10-bit or float
        // target would quantize the picture it exists to preserve.
        const bool probed = renderFormatProbed_ && renderInternalFormat_ != 0;
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
        format.setInternalTextureFormat(probed ? renderInternalFormat_ : kGlInternalFormatRgba8);
        auto snapshot = std::make_unique<QOpenGLFramebufferObject>(QSize(width, height), format);
        if (!snapshot->isValid()) {
            invalidatePresentedPicture();
            return;
        }
        presentedPictureFormat_ = probed ? renderInternalFormat_ : kGlInternalFormatRgba8;
        presentedPictureFbo_ = std::move(snapshot);
    }
    QOpenGLContext* glContext = QOpenGLContext::currentContext();
    QOpenGLExtraFunctions* gl = glContext != nullptr ? glContext->extraFunctions() : nullptr;
    if (gl == nullptr) {
        invalidatePresentedPicture();
        return;
    }
    GLint previousRead = 0;
    GLint previousDraw = 0;
    gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, presentedPictureFbo_->handle());
    // The copy is one-to-one, so no filtering is asked for at all.
    gl->glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
}

bool VideoSurface::captureTeardownPicture() {
    if (destroying_ || context_ == nullptr) return false;
    QOpenGLExtraFunctions* gl = context_->extraFunctions();
    if (gl == nullptr) return false;
    // Only a private snapshot is read. Qt destroys the widget's framebuffers
    // before it deletes the context, so at aboutToBeDestroyed
    // defaultFramebufferObject() is 0 and names the offscreen surface behind the
    // widget, not the picture; the caller therefore renders into this hold first
    // when there is none.
    if (!engineFrameHoldFbo_ || !engineFrameHoldFbo_->isValid()) return false;
    const GLuint source = engineFrameHoldFbo_->handle();
    const QSize size = engineFrameHoldFbo_->size();
    if (size.isEmpty()) return false;

    GLint previousRead = 0;
    GLint previousDraw = 0;
    gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
    gl->glBindFramebuffer(kGlReadFramebuffer, source);
    // Read the attachment's own format instead of assuming one: a float or
    // 10-bit target read as 8-bit bytes would be the quantization this exists to
    // avoid.
    GLint objectType = 0;
    GLint objectName = 0;
    GLint internalFormat = 0;
    gl->glGetFramebufferAttachmentParameteriv(kGlReadFramebuffer, GL_COLOR_ATTACHMENT0,
                                              kGlColorAttachmentObjectType, &objectType);
    gl->glGetFramebufferAttachmentParameteriv(kGlReadFramebuffer, GL_COLOR_ATTACHMENT0,
                                              kGlColorAttachmentObjectName, &objectName);
    if (objectType == static_cast<GLint>(kGlTextureObject)) {
        GLint previousTexture = 0;
        gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(objectName));
        gl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, kGlTextureInternalFormat, &internalFormat);
        gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    } else if (objectType == static_cast<GLint>(kGlRenderbufferObject)) {
        GLint previousRenderbuffer = 0;
        gl->glGetIntegerv(0x8CA7, &previousRenderbuffer);
        gl->glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(objectName));
        gl->glGetRenderbufferParameteriv(GL_RENDERBUFFER, kGlRenderbufferInternalFormat,
                                         &internalFormat);
        gl->glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previousRenderbuffer));
    }

    GLenum readType = kGlUnsignedByte;
    QImage::Format imageFormat = QImage::Format_RGBA8888;
    switch (internalFormat) {
        case kGlInternalFormatRgba16F:
            // Half floats are copied as half floats; the buffer QImage allocates
            // is the same four-channel half layout glReadPixels writes.
            readType = kGlHalfFloat;
            imageFormat = QImage::Format_RGBA16FPx4;
            break;
        case kGlInternalFormatRgba16:
            readType = kGlUnsignedShort;
            imageFormat = QImage::Format_RGBA64;
            break;
        case kGlInternalFormatRgb10A2:
            // Widened to 16 bits per channel rather than copied packed: the
            // channel order of a packed 10:10:10:2 word is a Qt image-layout
            // question this code would otherwise be guessing at, and widening is
            // exact - no 8-bit step, no invented colour.
            readType = kGlUnsignedShort;
            imageFormat = QImage::Format_RGBA64;
            break;
        default:
            // Only a target whose attachment format could not be named at all
            // falls here, and that is said in the log rather than assumed.
            break;
    }

    QImage picture(size, imageFormat);
    if (picture.isNull()) {
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
        return false;
    }
    GLint previousPack = 4;
    gl->glGetIntegerv(kGlPackAlignment, &previousPack);
    gl->glPixelStorei(kGlPackAlignment, 1);
    gl->glReadPixels(0, 0, size.width(), size.height(), kGlRgba, readType, picture.bits());
    gl->glPixelStorei(kGlPackAlignment, static_cast<GLint>(previousPack));
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));

    // GL hands back rows bottom-first; a QImage is stored top-first. Flipped in
    // place - the copying form would double a 66 MB 4K half-float buffer, which
    // is the allocation this one-copy budget exists to avoid.
    picture.flip(Qt::Vertical);
    teardownPicture_ = std::move(picture);
    teardownPictureFormat_ = internalFormat;
    mikai::gui::uiprofile::noteTeardownPictureCaptured();
    if (mikai::gui::uiprofile::enabled()) {
        mikai::gui::uiprofile::log(
            "surface.teardownPicture",
            QStringLiteral("size=%1x%2 internal_format=%3 bytes=%4")
                .arg(size.width())
                .arg(size.height())
                .arg(teardownPictureFormat_, 0, 16)
                .arg(teardownPicture_.sizeInBytes()));
    }
    return true;
}

bool VideoSurface::drawRetainedPicture(QPainter& painter, unsigned int target, int width,
                                       int height) {
    // Raw GL inside a section of its own: the caller may have handed over an
    // ordinary painter, and a blit needs Qt's paint engine out of the way.
    if (engineFrameHoldFbo_ && engineFrameHoldFbo_->isValid()) {
        painter.beginNativePainting();
        const bool blitted = drawEngineFrameHold(target, width, height);
        painter.endNativePainting();
        if (blitted) return true;
    }
    if (contextReady_ && context_ != nullptr) {
        // The last presented frame is a copy with no chrome on it, exactly what a
        // pass that cannot render has to show.
        painter.beginNativePainting();
        const bool blitted = drawPresentedPicture(target, width, height);
        painter.endNativePainting();
        if (blitted) return true;
    }
    return paintTeardownPicture(painter);
}

bool VideoSurface::paintTeardownPicture(QPainter& painter) {
    if (teardownPicture_.isNull()) return false;
    // The readback is the finished frame, letterbox bars included, so it is
    // fitted by its own aspect rather than stretched over the whole widget.
    const QRectF area(rect());
    const double scale = std::min(area.width() / teardownPicture_.width(),
                                  area.height() / teardownPicture_.height());
    if (!(scale > 0.0)) return false;
    const QSizeF fitted(teardownPicture_.width() * scale, teardownPicture_.height() * scale);
    const QRectF box(area.center().x() - fitted.width() / 2.0,
                     area.center().y() - fitted.height() / 2.0, fitted.width(), fitted.height());
    // The readback is already device-sized, so fitting it to the widget's own
    // logical rect maps it 1:1 through the widget's ratio - no resampling, and no
    // hint needed. Only a letterbox that really shrinks it is resampled, and
    // there nearest-neighbour would alias the whole picture.
    const qreal ratio = devicePixelRatioF();
    const bool resampled =
        std::abs(box.width() * ratio - teardownPicture_.width()) > 1.0 ||
        std::abs(box.height() * ratio - teardownPicture_.height()) > 1.0;
    if (!resampled) {
        painter.drawImage(box, teardownPicture_);
        return true;
    }
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(box, teardownPicture_);
    painter.restore();
    return true;
}

VideoSurface::~VideoSurface() {
    // A closed player must not leave the session inhibited.
    mikai::gui::idleInhibitor().release(this);
    cancelMediaInformation();
    if (frameTimingLog_.isOpen() && !timingLogBuffer_.isEmpty()) {
        const qint64 written = frameTimingLog_.write(timingLogBuffer_);
        if (written != timingLogBuffer_.size()) {
            timingLogBroken_ = true;
            qWarning("VideoSurface timing log: final flush failed for '%s'",
                     qUtf8Printable(frameTimingLog_.fileName()));
        }
        timingLogBuffer_.resize(0);
    }
    cancelAnimeJanaiOperation();
    // The render context holds a pointer to the mpv core, so it has to go first,
    // and with THIS widget's context current: freeing it releases GL objects.
    // QOpenGLContext::aboutToBeDestroyed normally does this, and this is the
    // fallback for a widget destroyed without its context ever being torn down.
    // A widget that is going away retains nothing, so the teardown readback -
    // a device-sized copy nobody could ever see - is skipped by this flag.
    destroying_ = true;
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
    // A new source replaces what is playing, and the viewer keeps looking at the
    // outgoing picture while the new one loads (T-E1). The epoch below still
    // retires every reply and every claim the outgoing source owned: the hold is
    // a picture, not state. An empty URL is the intentional clear, so nothing is
    // kept for it.
    // A load asked for now owns the picture; a reattach attempt for the outgoing
    // one would race it with a second loadfile.
    reattachPending_ = false;
    reattachTimer_.stop();
    if (url.isEmpty()) {
        invalidateEngineFrameHold();
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
    // An explicit admission replaces any offset a loss captured: the caller names
    // the source and its own resume intent, and a reload of the previous one must
    // never replay that offset into this file.
    pendingResumePosition_ = 0.0;
    pendingResumePositionValid_ = false;
    // The viewer-facing wait starts here, not at the load command: a window with
    // a URL and no picture is opening, whether or not a core has taken it yet.
    setOpening(!url_.isEmpty());
    networkBytesPerSecond_ = 0.0;
    streamFramesPerSecond_ = 0.0;
    streamFpsSource_ = StreamFpsSource::Unknown;
    statsClock_.restart();
    observedContainerFps_ = 0.0;
    observedVideoWidth_ = 0.0;
    observedVideoHeight_ = 0.0;
    // Nothing of the previous source describes the one being opened: its output
    // geometry and the shape published from it are gone until this source
    // reports its own.
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
        // Qt does not repeat initializeGL when a pending runtime becomes available.
        if (!contextReady_ && context_ != nullptr && context_ == context() && isValid()) {
            QOpenGLContext* previous = QOpenGLContext::currentContext();
            QOpenGLWidget::makeCurrent();
            try {
                if (QOpenGLContext::currentContext() != context_) {
                    throw MpvError(tr("Could not activate the video OpenGL context.").toStdString());
                }
                createRenderContext();
                forceRender_ = true;
            } catch (...) {
                releaseRenderContextCurrent(previous);
                throw;
            }
            releaseRenderContextCurrent(previous);
            update();
        }
    } catch (const MpvError& e) {
        // A failed renderer cannot drain a deferred load.
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
    if (mikai::gui::uiprofile::enabled()) {
        // Host only: a stream URL carries a signed query that must never reach a
        // log file, and a local path is identified by its scheme alone.
        const QString host = url_.contains(QLatin1String("://"))
                                 ? QUrl(url_).host()
                                 : QStringLiteral("local");
        mikai::gui::uiprofile::log(
            "surface.startUrl",
            QStringLiteral("position=%1 host=%2")
                .arg(QString::number(resumeAt, 'f', 3), host));
    }
    pendingOpen_ = false;
    pacingTimer_.stop();
    clearPacingProfile();
    pacingTimerArmed_ = false;
    pacingTargetTime_ = 0;
    pacingRetryOnly_ = false;
    pacingMeasurePending_ = false;
    framePending_ = false;
    if (url_.isEmpty() || mpv_ == nullptr) {
        // There is nothing left to wait for, and no new source will replace the
        // picture a hold would be standing in for.
        setOpening(false);
        invalidateEngineFrameHold();
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
    // would overwrite that with a gate nothing can satisfy. Every submission
    // re-arms whatever picture is held - freeze included, because a freeze also
    // owns the load that follows - and a gate already satisfied by an earlier
    // landing would otherwise let the outgoing source's last frame clear this one.
    if (engineFrameHoldFbo_) {
        engineFrameHoldNeedsLoadLanding_ = true;
        engineFrameHoldLandingSeen_ = false;
    }
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
    if (frameTimingLog_.isOpen() && !timingLogBuffer_.isEmpty()) {
        const qint64 written = frameTimingLog_.write(timingLogBuffer_);
        if (written != timingLogBuffer_.size()) {
            timingLogBroken_ = true;
            qWarning("VideoSurface timing log: close flush failed for '%s'",
                     qUtf8Printable(frameTimingLog_.fileName()));
        }
        timingLogBuffer_.resize(0);
    }
    // A closed source is not reattached to: the attempt belongs to the picture
    // this close is taking away.
    reattachPending_ = false;
    reattachTimer_.stop();
    // A captured offset belongs to the source this close is taking away; the next
    // open brings its own intent.
    pendingResumePosition_ = 0.0;
    pendingResumePositionValid_ = false;
    // A later GL recreation may restore only an open source, never a stopped
    pacingTimer_.stop();
    clearPacingProfile();
    pacingTimerArmed_ = false;
    pacingTargetTime_ = 0;
    pacingRetryOnly_ = false;
    pacingMeasurePending_ = false;
    framePending_ = false;
    // pooled surface's last URL.
    url_.clear();
    setOpening(false);
    invalidateEngineFrameHold();
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
            if (animeJanaiStageBuildLease_ == stoppingLease)
                animeJanaiStageBuildLease_.reset();
        });
    Q_UNUSED(stopRequest);
    animeJanaiEpoch_ = 0;
    // Emitted before the reset so the chrome falls back to its play glyph
    // while the surface still reports a loaded file.
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


QImage VideoSurface::grabUnfilteredFrame(QString* reason) {
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
        VideoSurface* surface;
        const QString filter;
        const QString shaders;
        ~Restore() {
            // The core can be gone (a source change destroyed it) between the
            // capture and this restore; restoring into a null handle would be
            // undefined, and the new core has no filter to restore anyway.
            if (surface->mpv_) {
                try {
                    surface->mpv_->setString("vf", filter.toStdString());
                    surface->mpv_->setString("glsl-shaders", shaders.toStdString());
                } catch (const MpvError&) {
                }
            }
            surface->update();
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
    // A paused core does not decode on its own: a small seek back and forward
    // is what makes mpv produce a frame through the now-empty filter chain.
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
    emit seekRequested();
}

void VideoSurface::seekRelative(double delta) {
    if (!mpv_) return;
    // `relative` lets the core resolve the step against the position it is at
    // right now instead of one the caller sampled a pump interval ago, and
    // `exact` keeps the landed frame deterministic rather than keyframe-snapped.
    issueCoreCommand({"seek", mpvNumber(delta), "relative+exact"}, "seek",
                     /*reportRefusal=*/false);
    emit seekRequested();
}

void VideoSurface::setVolume(double volume) {
    requestedVolume_ = volume;
    if (!mpv_) return;
    issueCoreCommand({"set", "volume", mpvNumber(volume)}, "volume", /*reportRefusal=*/true);
}

void VideoSurface::setMuted(bool muted) {
    requestedMuted_ = muted;
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
    if (!request.valid() && mediaToken.has_value() && epoch == mpv_->sourceEpoch()) {
        // A command the transport refused will never report FILE_LOADED, so the
        // gate the submission armed is dropped - for the current load only.
        engineFrameHoldNeedsLoadLanding_ = false;
    }
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

void VideoSurface::setSubtitleDelay(double seconds) {
    if (mpv_ == nullptr) return;
    try {
        mpv_->setNumber("sub-delay", seconds);
    } catch (const MpvError&) {
        // A core that cannot take the property is a core with no subtitles.
    }
}

bool VideoSurface::addExternalTrack(const QString& file, bool subtitle, QString* reason) {
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
    publishPlayingState(playing_);
}

void VideoSurface::setOpening(bool opening) {
    if (opening == opening_) return;
    opening_ = opening;
    updatePlaceholderAnimation();
    emit openingChanged(opening_);
}

void VideoSurface::updatePlaceholderAnimation() {
    // The placeholder is only ever painted by the no-engine branch, so the
    // animation exists exactly while that branch is what the viewer sees.
    const bool needed = opening_ && (!contextReady_ || render_ == nullptr);
    if (needed == placeholderTimer_.isActive()) return;
    if (needed) {
        placeholderPhase_ = 0;
        placeholderTimer_.start();
    } else {
        placeholderTimer_.stop();
    }
    update();
}

void VideoSurface::paintOpeningPlaceholder(QPainter& painter) {
    // A backdrop, not a frame. Defined enough that the window cannot be mistaken
    // for a failed black rectangle, neutral enough that the dialog's own words
    // stay the thing being read.
    painter.fillRect(rect(), videoBackdrop());
    const QPointF centre(width() / 2.0, height() / 2.0);
    const double radius = std::max(10.0, std::min(width(), height()) * 0.035);
    paintOpeningRing(painter, centre, radius, std::max(2.0, radius * 0.18));
}

void VideoSurface::paintOpeningRing(QPainter& painter, const QPointF& centre, double radius,
                                    double penWidth) {
    // This target is single-sampled: QPainter::Antialiasing only reaches it when
    // the framebuffer carries samples, so a track and an arc drawn straight into
    // it have hard stair-steps. The ring is rasterised instead - the raster
    // paint engine computes real coverage - and blitted 1:1, into one reused
    // buffer rather than a multisampled target or a per-tick allocation.
    const qreal ratio = devicePixelRatioF();
    if (!(ratio > 0.0)) return;
    // The stroke's half-width reaches past the ring, and a cap reaches further.
    const int extent =
        std::max(2, static_cast<int>(std::ceil((radius + penWidth) * 2.0 * ratio)));
    if (openingRing_.size() != QSize(extent, extent)) {
        openingRing_ = QImage(extent, extent, QImage::Format_ARGB32_Premultiplied);
        // A new buffer holds no ring yet, so the phase must not claim otherwise.
        openingRingPhase_ = -1;
    }
    if (openingRing_.isNull()) return;
    // The buffer is placed in the target's own device pixels: the widget's paint
    // device is scaled by the ratio, and a fractional logical origin would
    // resample the coverage that was just rasterised.
    const int originX =
        static_cast<int>(std::lround(centre.x() * ratio)) - extent / 2;
    const int originY =
        static_cast<int>(std::lround(centre.y() * ratio)) - extent / 2;
    if (openingRingPhase_ != placeholderPhase_ || openingRingRadius_ != radius ||
        openingRingRatio_ != ratio) {
        openingRing_.fill(Qt::transparent);
        QPainter ring(&openingRing_);
        ring.setRenderHint(QPainter::Antialiasing, true);
        // The ring sits at the buffer's own centre, so the same pixels serve
        // every position the buffer is drawn at.
        const QRectF deviceRing(extent / 2.0 - radius * ratio, extent / 2.0 - radius * ratio,
                                radius * 2.0 * ratio, radius * 2.0 * ratio);
        QPen pen(QColor(255, 255, 255, 45));
        pen.setWidthF(penWidth * ratio);
        ring.setPen(pen);
        ring.setBrush(Qt::NoBrush);
        ring.drawEllipse(deviceRing);
        // One sweeping arc, advanced by the timer: an indeterminate wait says
        // "working", never a percentage this surface does not know.
        QPen sweep(QColor(255, 255, 255, 190));
        sweep.setWidthF(penWidth * ratio);
        sweep.setCapStyle(Qt::RoundCap);
        ring.setPen(sweep);
        const int startAngle = (placeholderPhase_ * 24) % 360;
        ring.drawArc(deviceRing, startAngle * 16, 100 * 16);
        openingRingPhase_ = placeholderPhase_;
        openingRingRadius_ = radius;
        openingRingRatio_ = ratio;
    }
    painter.save();
    painter.setWorldTransform(QTransform::fromScale(1.0 / ratio, 1.0 / ratio));
    painter.drawImage(QPoint(originX, originY), openingRing_);
    painter.restore();
}

void VideoSurface::setDisplayCornerRadius(int radius) {
    const int requested = std::max(0, radius);
    if (requested == displayCornerRadius_) return;
    displayCornerRadius_ = requested;
    // The patches describe the previous radius; the next displayed pass rebuilds
    // them. The repaint is what makes the new corners appear without waiting for
    // a frame - a paused surface may get none.
    displayCornerTopLeft_ = QImage();
    displayCornerTopRight_ = QImage();
    displayCornerBottomLeft_ = QImage();
    displayCornerBottomRight_ = QImage();
    displayCornerRatio_ = 0.0;
    // A rounded picture carries alpha the window has to composite; the opaque
    // flag claims it never has any, so it stands only while the corners are
    // square. Nothing else about the widget changes: same window flags, same
    // children, same target.
    setAttribute(Qt::WA_OpaquePaintEvent, displayCornerRadius_ <= 0);
    update();
}

void VideoSurface::paintDisplayCornerCoverage(QPainter& painter, int targetWidth,
                                              int targetHeight) {
    // A suppressed overlay is a capture, not a display: the raw pixels must reach
    // grabVideoFramebuffer() uncut, exactly as they do with square corners.
    if (displayCornerRadius_ <= 0 || overlaySuppressed_) return;
    const qreal ratio = devicePixelRatioF();
    if (!(ratio > 0.0) || targetWidth <= 0 || targetHeight <= 0) return;
    // The ratio is part of the coverage: the arc is rasterised in device pixels,
    // so a window that moved to another screen must not reuse the old arc.
    if (displayCornerTopLeft_.isNull() || displayCornerRatio_ != ratio) {
        const qreal radiusDevice = displayCornerRadius_ * ratio;
        // One whole device pixel past the arc keeps the straight edges sealed.
        const int patch = std::max(2, static_cast<int>(std::ceil(radiusDevice)) + 1);
        // Two patches must never meet: past that the radius does not describe a
        // corner and no coverage is invented for it.
        if (patch * 2 > targetWidth || patch * 2 > targetHeight) return;
        displayCornerTopLeft_ = cornerCoveragePatch(patch, radiusDevice);
        if (displayCornerTopLeft_.isNull()) return;
        displayCornerTopRight_ = displayCornerTopLeft_.flipped(Qt::Horizontal);
        displayCornerBottomLeft_ = displayCornerTopLeft_.flipped(Qt::Vertical);
        displayCornerBottomRight_ = displayCornerTopLeft_.flipped(Qt::Horizontal | Qt::Vertical);
        displayCornerRatio_ = ratio;
    }
    const int patch = displayCornerTopLeft_.width();
    painter.save();
    // Alpha coverage, not paint: the picture and the chrome over it keep their
    // colours, and only what falls outside the rounded shape loses its alpha.
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    // Device pixels, on the target's own grid. Cancelling the widget's ratio
    // transform puts each patch exactly on the buffer's pixels, so a fractional
    // ratio cannot resample the coverage into a soft or uneven corner.
    painter.setWorldTransform(QTransform::fromScale(1.0 / ratio, 1.0 / ratio));
    painter.drawImage(QPoint(0, 0), displayCornerTopLeft_);
    painter.drawImage(QPoint(targetWidth - patch, 0), displayCornerTopRight_);
    painter.drawImage(QPoint(0, targetHeight - patch), displayCornerBottomLeft_);
    painter.drawImage(QPoint(targetWidth - patch, targetHeight - patch),
                      displayCornerBottomRight_);
    painter.restore();
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
    const bool profileContext = mikai::gui::uiprofile::countersEnabled();
    const qint64 profileStartNs = profileContext ? mikai::gui::uiprofile::nowNs() : 0;
    if (profileContext) ++profileInitializeGlCalls_;
    QOpenGLWidget::initializeGL();
    context_ = QOpenGLContext::currentContext();
    // Failed runtime initialization still leaves a Qt context that needs tracking.
    contextDestroyedConnection_ =
        connect(context_, &QOpenGLContext::aboutToBeDestroyed, this,
                &VideoSurface::releaseRenderContext, Qt::DirectConnection);
    const bool recreated = renderGeneration_ > 0;
    // Reloading the VO issues an ordinary loadfile, which resets its pause
    // write. Save the pre-reload intent, including a deferred pre-show pause.
    const bool restorePaused = requestedPause() || enginePauseHeld();
    try {
        ensureEngine();
        createRenderContext();
        // The first paint after the context exists has to render whatever mpv
        // is holding, even if no frame has been decoded yet: the framebuffer
        // has never been drawn into.
        forceRender_ = true;
        if (recreated && !pendingOpen_ && !url_.isEmpty()) {
            // A new render context attaches as a new video output while mpv keeps
            // the file loaded, so the core is asked first: force a render and let
            // a bounded window of paints say whether it still delivers output.
            // Only a core that reports nothing, or says it has nothing loaded, is
            // handed the file again - see finishContextReattach().
            beginContextReattach();
        }
        if (pendingOpen_) startPendingUrl();
        // The load must precede the pause reassertion; reversing the order lets
        // startPendingUrl's normal unpause overwrite a paused hand-off.
        if (restorePaused) reassertPaused();
    } catch (const MpvError& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
        contextReady_ = false;
    }
    if (mikai::gui::uiprofile::enabled()) {
        const qint64 elapsedUs =
            profileContext ? (mikai::gui::uiprofile::nowNs() - profileStartNs) / 1000 : -1;
        mikai::gui::uiprofile::log(
            "surface.initializeGL",
            QStringLiteral("generation=%1 recreated=%2 ready=%3 context=%4 elapsed_us=%5")
                .arg(static_cast<qulonglong>(renderGeneration_))
                .arg(static_cast<int>(recreated))
                .arg(static_cast<int>(contextReady_))
                .arg(QStringLiteral("0x%1").arg(
                    static_cast<qulonglong>(reinterpret_cast<quintptr>(context_)), 0, 16))
                .arg(elapsedUs));
    }
}

void VideoSurface::beginContextReattach() {
    // mpv says it has no file loaded: there is no picture to wait for, so the
    // load is the only path back and it is taken once, right here.
    if (mpv_ == nullptr || observedIdle_) {
        finishContextReattach();
        return;
    }
    reattachPending_ = true;
    reattachTicksLeft_ = kContextReattachTicks;
    reattachTimer_.start();
    forceRender_ = true;
    update();
}

void VideoSurface::finishContextReattach() {
    reattachTimer_.stop();
    reattachPending_ = false;
    reattachTicksLeft_ = 0;
    if (mpv_ == nullptr || url_.isEmpty() || pendingOpen_) return;
    // The old video output died with its render context, so the filter it carried
    // is no longer in the core: the reload is what rebuilds it, and the native
    // binding must not claim a chain that went with that output.
    animeJanaiFilterBound_ = false;
    nativeFilterBound_ = false;
    MediaTrackSelection selection = mediaTrackSelection();
    if (pendingMediaTrackSelection_.audio.has_value())
        selection.audio = pendingMediaTrackSelection_.audio;
    if (pendingMediaTrackSelection_.subtitle.has_value())
        selection.subtitle = pendingMediaTrackSelection_.subtitle;
    setNextMediaTrackSelection(selection);
    // The position the viewer left and the pause intent they set both survive.
    // initializeGL reasserts the pause after its own reload, and this deferred
    // path has to do the same: a loadfile resets the core's pause, and a viewer
    // who left a paused source must not find it playing.
    const bool restorePaused = requestedPause() || enginePauseHeld();
    // Only the captured offset can be trusted here: this reload is issued up to
    // two thirds of a second after the context went, and by then the dead output
    // has already reported 0 through the property pump.
    startPendingUrl(resumePositionForReload());
    if (restorePaused) reassertPaused();
}

void VideoSurface::createRenderContext() {
    if (mpv_ == nullptr || context_ == nullptr) return;
    // A new context owns a new widget framebuffer whose attachment format says
    // nothing about the one the previous context probed.
    resetRenderTargetFormat();
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
    contextReady_ = true;
    // The engine path paints from now on; the placeholder's timer has nothing
    // left to animate even if the first frame has not arrived yet.
    updatePlaceholderAnimation();
}

void VideoSurface::resizeGL(int width, int height) {
    QOpenGLWidget::resizeGL(width, height);
    // A resize gets a freshly allocated framebuffer, so whatever the last paint
    // drew into the old one is gone and the next paint has to render even though
    // no frame has been decoded since.
    //
    // The attachment FORMAT is deliberately not re-probed here: it is a property
    // of the context and the surface format, not of the size, and Qt reallocates
    // a new color attachment with that same format. Probing again would put a
    // synchronous format query in the resize path for an answer already known -
    // resetRenderTargetFormat() is for context creation and release only.
    forceRender_ = true;
    // The copy describes the target that no longer exists.
    invalidatePresentedPicture();
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

VideoSurface::PaintScheduleDecision VideoSurface::decidePaintSchedule(bool pacedRelease, bool forced,
                                                                    bool armedSlot,
                                                                    bool frameOwed,
                                                                    bool holdOwesOutput) {
    PaintScheduleDecision decision;
    // A hold taken across a source change is released by the frame it is waiting
    // for, and that release is a presentation of its own.
    decision.consumesFrame = pacedRelease || forced || (frameOwed && (!armedSlot || holdOwesOutput));
    decision.keepsArmedSlot = armedSlot && !decision.consumesFrame;
    return decision;
}

void VideoSurface::paintGL() {
    // Profile observations are recorded before any decision is taken: the
    // counters describe what this paint found, not what it left behind.
    const bool profilePaint = mikai::gui::uiprofile::countersEnabled();
    const bool pacingActiveAtEntry = profilePaint && pacingTimer_.isActive();
    const bool pacingFired = profilePaint && profilePacingFired_;
    const qint64 profilePaintStartNs =
        profilePaint ? mikai::gui::uiprofile::nowNs() : 0;
    if (profilePaint) {
        ++profilePaintCalls_;
        profileLastPaintEntryNs_ = profilePaintStartNs;
        if ((pacingActiveAtEntry || pacingFired) && profilePacingArmedAtNs_ != 0) {
            profileLastPaintDelayMs_ =
                mikai::gui::uiprofile::elapsedMs(profilePacingArmedAtNs_);
        } else {
            profileLastPaintDelayMs_ = -1;
        }
    }
    QOpenGLWidget::paintGL();
    if (!contextReady_ || !render_) {
        // No renderer, so nothing this surface armed can ever be presented:
        // whatever slot it was holding goes with the engine.
        pacingTimer_.stop();
        pacingTimerArmed_ = false;
        pacingTargetTime_ = 0;
        pacingRetryOnly_ = false;
        pacedPaintReleased_ = false;
        if (profilePaint) {
            profilePacingAwaitingPaint_ = false;
            profilePacingArmedAtNs_ = 0;
            profilePacingFired_ = false;
            if (pacingActiveAtEntry && !framePending_) ++profilePacingDisarms_;
        }
        // No engine, so there is no frame to render - but NoPartialUpdate leaves
        // whatever is in this buffer visible, so this pass always defines every
        // pixel. The order is the retainer, then the opening placeholder, then
        // the surface's own backdrop: a picture this surface already holds says
        // more than a wait indicator, and neither is ever replaced by bare black.
        // The overlay belongs to the widget rather than to the core and is
        // painted over all of them.
        QPainter unrendered(this);
        const QSize retainedSize = QSize(width(), height()) * devicePixelRatioF();
        const bool retained =
            drawRetainedPicture(unrendered,
                                static_cast<unsigned int>(defaultFramebufferObject()),
                                std::max(1, retainedSize.width()),
                                std::max(1, retainedSize.height()));
        if (!retained) {
            if (opening_) {
                paintOpeningPlaceholder(unrendered);
            } else {
                unrendered.fillRect(rect(), videoBackdrop());
            }
        }
        if (!overlaySuppressed_) emit paintOverlay(&unrendered);
        // Last, and over the overlay: the coverage is what the window composites.
        paintDisplayCornerCoverage(unrendered, std::max(1, retainedSize.width()),
                                   std::max(1, retainedSize.height()));
        if (profilePaint) {
            mikai::gui::uiprofile::recordPaintPass(
                mikai::gui::uiprofile::kPaintUnrendered,
                (mikai::gui::uiprofile::nowNs() - profilePaintStartNs) / 1000);
        }
        if (mikai::gui::uiprofile::enabled()) {
            const qint64 elapsedUs = profilePaint
                                         ? (mikai::gui::uiprofile::nowNs() - profilePaintStartNs) /
                                               1000
                                         : -1;
            mikai::gui::uiprofile::log(
                "surface.paintGL",
                QStringLiteral("result=unrendered haveFrame=0 elapsed_us=%1").arg(elapsedUs));
        }
        return;
    }
    // Which paint this is decides what it may take. A paint the pacing timer
    // released, a forced redraw and a frame that arrived with no slot to wait for
    // are presentations; every other pass is Qt asking for a repaint over a
    // picture that is already on screen. Presenting the frame a scheduled slot is
    // still holding would be the burst this classification exists to stop, and
    // stopping the timer here would leave the frame with nothing to present it.
    const bool pacedRelease = pacedPaintReleased_;
    pacedPaintReleased_ = false;
    const bool armedSlot = pacingTimerArmed_ && !pacingRetryOnly_;
    PaintScheduleDecision schedule = decidePaintSchedule(
        pacedRelease, forceRender_, armedSlot, framePending_, engineFrameHoldAwaitingOutput_);
    // MPV_RENDER_UPDATE_FRAME marks a new output frame. A pass that keeps an armed
    // slot does not read mpv's queue at all: that flag is the slot's frame, and
    // consuming it here would leave the paint it is waiting for with nothing to
    // present. Every other pass may read it, and an arrival with no slot behind
    // it is presented at once.
    bool frameArrived = false;
    if (schedule.consumesFrame || !schedule.keepsArmedSlot) {
        // A paused chrome repaint already has the picture it will blit. Asking
        // mpv whether a frame arrived is a context sync, and nothing is coming.
        const QSize deviceSize = QSize(width(), height()) * devicePixelRatioF();
        const bool snapshotCovers =
            presentedPictureFbo_ && presentedPictureFbo_->isValid() &&
            presentedPictureFbo_->size() == deviceSize;
        // A reattach is the opposite of quiet: the NEW video output is exactly
        // what has to be asked whether it still delivers, and a paused source
        // has no other frame coming to read. Suppressing update() here spent the
        // two-thirds-of-a-second reattach window without ever reading the queue,
        // so every paused recreation fell through to a full reload (which resets
        // the playhead to 0) instead of reattaching to the core that still holds
        // the file.
        const bool pausedQuiet = !playing_ && !framePending_ && !pacingTimerArmed_ &&
                                 !engineFrameHoldAwaitingOutput_ && !reattachPending_ &&
                                 !schedule.consumesFrame && snapshotCovers;
        if (!pausedQuiet) {
            const uint64_t updateFlags = render_->update();
            frameArrived = (updateFlags & MPV_RENDER_UPDATE_FRAME) != 0;
            if (!schedule.consumesFrame) {
                schedule = decidePaintSchedule(false, false, false, frameArrived, false);
            }
        }
    }
    const bool frameOwed = framePending_ || frameArrived;
    const bool consumesFrame = schedule.consumesFrame;
    const bool newOutputFrame = consumesFrame && frameOwed;
    if (schedule.keepsArmedSlot) {
        // The slot is the frame's: keep it armed, and keep the measurement window
        // of the paint it is waiting for.
        if (profilePaint) {
            profileLastPaintDelayMs_ = -1;
        }
    } else {
        if (profilePaint) {
            // Only an ACTIVE timer this pass really stopped is a disarm.
            if (pacingActiveAtEntry && !frameOwed) ++profilePacingDisarms_;
            profilePacingAwaitingPaint_ = false;
            profilePacingArmedAtNs_ = 0;
            profilePacingFired_ = false;
        }
        pacingTimer_.stop();
        pacingTimerArmed_ = false;
        pacingTargetTime_ = 0;
        pacingRetryOnly_ = false;
    }
    bool freshOutputFrame = false;
    if (engineFrameHoldAwaitingOutput_) {
        mpv_render_frame_info frameInfo{};
        const bool realFrame = render_->nextFrameInfo(frameInfo) && isRealNewFrame(frameInfo);
        // A hold taken across a source change is released only by a frame of the
        // source that is actually loading. mpv bumps the command epoch when the
        // load is SUBMITTED, so an epoch comparison is already true for the
        // outgoing source's last frame; the landing event is what proves the
        // replacement took.
        const bool replacementLanded =
            !engineFrameHoldNeedsLoadLanding_ || engineFrameHoldLandingSeen_;
        freshOutputFrame = realFrame && replacementLanded;
    }
    // forceRender_ covers the framebuffer Qt hands back empty: a resize or a
    // re-show wipes the picture, and with a paused core no new frame is coming.
    const bool haveFrame = newOutputFrame || forceRender_;

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
    // The boundary around mpv's raw GL, which maps logical widget coordinates
    // onto this framebuffer. Everything raw stays inside it: Qt owns the paint
    // engine's target and state outside this window, and mpv leaves the
    // vertex-array binding and unpack row length behind, so both are put back
    // before the window closes.
    QPainter painter(this);
    painter.beginNativePainting();
    // Clear reused Qt buffers before rendering, including pixels outside the video rectangle.
    gl->glBindFramebuffer(GL_FRAMEBUFFER, target);
    gl->glViewport(0, 0, targetWidth, targetHeight);
    // Qt enables blending before paintGL; libmpv requires its default disabled state.
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_SCISSOR_TEST);
    // A full-size blit overwrites every pixel, including the letterbox. Clearing
    // first is a second write of the same buffer on every chrome repaint.
    const bool holdCovers =
        retainedPictureVisible && engineFrameHoldFbo_ && engineFrameHoldFbo_->isValid() &&
        engineFrameHoldFbo_->size() == QSize(targetWidth, targetHeight);
    const bool snapshotCovers =
        !consumesFrame && presentedPictureFbo_ && presentedPictureFbo_->isValid() &&
        presentedPictureFbo_->size() == QSize(targetWidth, targetHeight);
    bool cleared = false;
    if (!holdCovers && !snapshotCovers) {
        gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        gl->glClear(GL_COLOR_BUFFER_BIT);
        cleared = true;
    }

    // Query the widget color attachment once per context/target, with that
    // target actually bound, so the format handed to mpv is the one it draws
    // into rather than a previous framebuffer's.
    if (!renderFormatProbed_) probeFboColorDepth();

    int renderResult = 0;
    // A pass is three costs, and a class p95 that cannot name them blames the
    // wrong one: mpv's render, the retained/teardown picture, and the chrome.
    const bool phaseTimed = mikai::gui::uiprofile::countersEnabled();
    qint64 retentionMicros = 0;
    qint64 engineMicros = 0;
    qint64 phaseMark = phaseTimed ? mikai::gui::uiprofile::nowNs() : 0;
    // Named so the pass classification below can say whether the picture on
    // screen came from the held snapshot or from mpv's current output.
    const bool heldPictureDrawn =
        retainedPictureVisible && drawEngineFrameHold(target, targetWidth, targetHeight);
    if (phaseTimed) {
        retentionMicros += (mikai::gui::uiprofile::nowNs() - phaseMark) / 1000;
        phaseMark = mikai::gui::uiprofile::nowNs();
    }
    bool presentedPictureDrawn = false;
    if (heldPictureDrawn) {
        if (newOutputFrame) {
            renderResult = render_->skipRendering();
            if (renderResult == 0) render_->reportSwap();
        }
    } else if (!consumesFrame &&
               (presentedPictureDrawn = drawPresentedPicture(target, targetWidth, targetHeight))) {
        // A repaint presents the picture that is already on screen. Re-running
        // mpv's render here would show the frame its VO is still holding for the
        // slot this pass is not allowed to take.
    } else {
        if (!cleared) {
            gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            gl->glClear(GL_COLOR_BUFFER_BIT);
            cleared = true;
        }
        renderResult = render_->render(static_cast<int>(target), targetWidth, targetHeight,
                                       renderInternalFormat_, renderDepthBitsPerChannel_);
        presentedFrame = haveFrame;
        if (renderResult >= 0) {
            if (presentedFrame) {
                // The picture moved on: the snapshot describes the frame before it.
                invalidatePresentedPicture();
            } else {
                capturePresentedPicture(target, targetWidth, targetHeight);
            }
        }
    }
    if (phaseTimed) {
        engineMicros += (mikai::gui::uiprofile::nowNs() - phaseMark) / 1000;
    }
    if (QOpenGLExtraFunctions* extra = glContext->extraFunctions()) {
        extra->glBindVertexArray(0);
        extra->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        extra->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, target);
    painter.endNativePainting();
    // The render lead is measured at the swap, not here: composition and the
    // vsync wait happen after this function returns, and a lead that excludes
    // them is the very lateness the timer is supposed to subtract.
    if (!g_frameTimingLogPath.isEmpty()) {
        const auto counterValue = [](const std::optional<int64_t>& value) {
            return value.has_value() ? QString::number(*value) : QStringLiteral("unknown");
        };
        logFrameTiming(
            QStringLiteral("paint result=%1 newframe=%2 pending=%3 forced=%4 lead_ms=%5 "
                           "paint_delay_ms=%6 skip_reason=%7 drop=%8 vo_delayed=%9 "
                           "pause_held=%10 disarm=%11 chrome_dirty=%12")
                .arg(renderResult)
                .arg(static_cast<int>(newOutputFrame))
                .arg(static_cast<int>(framePending_))
                .arg(static_cast<int>(forceRender_))
                .arg(QString::number(renderLeadMs_, 'f', 2))
                .arg(profileLastPaintDelayMs_)
                .arg(QString::fromLatin1(profilePacingReason_))
                .arg(counterValue(diagnostics_.voDropCount))
                .arg(counterValue(diagnostics_.voDelayedFrameCount))
                .arg(static_cast<int>(enginePauseHeld()))
                .arg(static_cast<qulonglong>(profilePacingDisarms_))
                .arg(static_cast<qulonglong>(
                    mikai::gui::uiprofile::g_chromeDirtyRequests.load())));
    }
    lastRenderedFboSize_ = deviceSize;
    hasRenderedFrame_ = hasRenderedFrame_ || renderResult == 0;
    if (renderResult < 0) {
        // A failed render leaves the target holding something this surface did
        // not put there, so the copy no longer describes what is on screen.
        invalidatePresentedPicture();
        // The retained picture stands in for the frame the renderer could not
        // produce, so a reported failure is never also a black clear.
        drawRetainedPicture(painter, target, targetWidth, targetHeight);
    }
    // A context that has not received anything from the core yet was just
    // cleared to black. The picture read back from the lost context stands in
    // there, exactly as the hold does for a load, until the core delivers - and
    // NoPartialUpdate means it has to be drawn in full on every such pass, so its
    // cost belongs to the same retention segment as the held blit. Painting it
    // once per target would leave the buffer holding whatever the next pass
    // cleared it to.
    if (renderResult == 0 && !retainedPictureVisible && !presentedPictureDrawn &&
        !newOutputFrame && !framePending_) {
        const qint64 fallbackStart = phaseTimed ? mikai::gui::uiprofile::nowNs() : 0;
        paintTeardownPicture(painter);
        if (phaseTimed) {
            retentionMicros += (mikai::gui::uiprofile::nowNs() - fallbackStart) / 1000;
        }
    }
    if (reattachPending_ && renderResult == 0 && newOutputFrame) {
        // The live core delivered output on its own after the context came back:
        // the source never had to be reloaded.
        reattachPending_ = false;
        reattachTimer_.stop();
        // Nothing will resume from it, and holding it over a live source would let
        // it age behind the viewer's seeks. Only a frame this pass presented retires
        // it: no frame is owed to a load here, because a load ends the reattach.
        if (presentedFrame) pendingResumePositionValid_ = false;
    }
    if (renderResult == 0 && newOutputFrame && presentedFrame) {
        // A frame the current source really rendered is on the buffer now: the CPU
        // fallback has nothing left to stand in for, so it is retired at once
        // instead of being re-blitted for the rest of the session.
        teardownPicture_ = QImage();
        teardownPictureFormat_ = 0;
    }
    if (renderResult == 0 && freshOutputFrame && !retainedPictureVisible) {
        hasVisibleOutputFrame_ = true;
        // A frame of the loaded source reached the screen: the opening wait is
        // over, and the placeholder must give way to the picture.
        setOpening(false);
        engineFrameHoldAwaitingOutput_ = false;
        engineFrameHoldNeedsLoadLanding_ = false;
        engineFrameHoldLandingSeen_ = false;
        // The fresh output has replaced the transition snapshot. Release its
        // GPU storage now; normal playback retains no extra video framebuffer.
        engineFrameHoldFbo_.reset();
        // A frame of the source the reload asked for is on screen - the landing
        // gate above proved it - so the offset it carried has been delivered and
        // the next loss captures its own.
        pendingResumePositionValid_ = false;
        // The teardown readback has done its job for the same reason.
        teardownPicture_ = QImage();
        teardownPictureFormat_ = 0;
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
        setOpening(false);
        ++diagnostics_.renderedOutputFrames;
        // A real frame is on screen again, so the picture read back for the lost
        // context has done its job and its device-sized copy goes.
        teardownPicture_ = QImage();
        teardownPictureFormat_ = 0;
    }
    if (renderResult < 0) {
        // The buffer holds the retained picture rather than that frame, so a
        // report for it would name a presentation that is no longer on screen.
        videoFramePainted_ = false;
        // One report per streak: a renderer that fails on every paint would
        // otherwise repeat the same message for every frame it was asked for.
        if (!renderFailureStreak_ && mpv_ != nullptr) {
            renderFailureStreak_ = true;
            const QString message =
                tr("Video render failed (mpv: %1)")
                    .arg(QString::fromStdString(mpv_->describeError(renderResult)));
            QMetaObject::invokeMethod(this, [this, message] { emit errorOccurred(message); },
                                      Qt::QueuedConnection);
        }
    } else {
        renderFailureStreak_ = false;
        if (presentedFrame) {
            // Only ever set: a UI-only repaint must not throw away the report
            // the frame before it is still owed.
            videoFramePainted_ = true;
        }
    }
    if (consumesFrame) {
        // The frame this pass was allowed to present has been presented; one it
        // was not is still the slot's, and that tick is what presents it.
        framePending_ = false;
    }
    forceRender_ = false;

    // No self-update here: mpv's own update callback is what asks for the next
    // paint, so scheduling one from paintGL would only spin on a paused core.
    qint64 overlayMicros = 0;
    phaseMark = phaseTimed ? mikai::gui::uiprofile::nowNs() : 0;
    if (!overlaySuppressed_) emit paintOverlay(&painter);
    if (phaseTimed) overlayMicros = (mikai::gui::uiprofile::nowNs() - phaseMark) / 1000;
    // After the chrome, because a control that stands in a corner is part of the
    // rounded shape - and outside the overlay segment, because this is the
    // picture's own presentation cost, not the chrome's.
    paintDisplayCornerCoverage(painter, targetWidth, targetHeight);
    if (profilePaint) {
        // What this pass cost, classified by what it actually put on screen: the
        // chrome-repaint p95 GOAL-F needs is the pattern below, not a mean.
        const int pass = heldPictureDrawn
                             ? mikai::gui::uiprofile::kPaintRetained
                             : (renderResult == 0 && presentedFrame && newOutputFrame)
                                   ? mikai::gui::uiprofile::kPaintVideoFrame
                                   : mikai::gui::uiprofile::kPaintChromeOnly;
        const qint64 totalMicros =
            (mikai::gui::uiprofile::nowNs() - profilePaintStartNs) / 1000;
        mikai::gui::uiprofile::recordPaintPass(pass, totalMicros);
        // The class the pass was just given is what the phase split is kept
        // against: a paused chrome-only repaint's overlay cost must not be read
        // as an active frame's engine cost, and the global sums cannot say.
        mikai::gui::uiprofile::notePaintPhase(
            pass, 0, engineMicros,
            totalMicros - engineMicros - retentionMicros - overlayMicros);
        mikai::gui::uiprofile::notePaintPhase(pass, 1, retentionMicros, 0);
        mikai::gui::uiprofile::notePaintPhase(pass, 2, overlayMicros, 0);
    }
    if (mikai::gui::uiprofile::enabled()) {
        const qint64 elapsedUs =
            profilePaint ? (mikai::gui::uiprofile::nowNs() - profilePaintStartNs) / 1000 : -1;
        mikai::gui::uiprofile::log(
            "surface.paintGL",
            QStringLiteral("result=%1 haveFrame=%2 newframe=%3 presented=%4 elapsed_us=%5 "
                           "paint_delay_ms=%6 skip_reason=%7")
                .arg(renderResult)
                .arg(static_cast<int>(haveFrame))
                .arg(static_cast<int>(newOutputFrame))
                .arg(static_cast<int>(presentedFrame))
                .arg(elapsedUs)
                .arg(profileLastPaintDelayMs_)
                .arg(QString::fromLatin1(profilePacingReason_)));
    }
    if (profilePaint) profilePacingReason_ = "none";
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
        // T-204: the return from skip mode forces one redraw, because the
        // buffer mpv last handed over may have been consumed by a skip and no
        // frame would otherwise ever repaint the widget.
        if (skippingFrames_) {
            skippingFrames_ = false;
            forceRender_ = true;
            if (!g_frameTimingLogPath.isEmpty())
                logFrameTiming(QStringLiteral("skip-mode left; forced redraw"));
        }
        unpaintableStreak_ = 0;
        framePending_ = framePending_ || frame;
        if (framePending_ || forceRender_) {
            // T-202: the paint lands in the compositor slot mpv named for the
            // frame (or at once, when nothing is pending or pacing is off).
            schedulePacedPaint();
        }
    } else {
        // T-204: a single transient non-paintable round (a popup, an occlusion,
        // a DWM operation) must not discard a frame - the streak has to persist
        // first, so the frame is held pending and one skip-mode pass answers
        // mpv only after the widget really stayed unpaintable.
        ++unpaintableStreak_;
        if (unpaintableStreak_ < kUnpaintableGraceTicks) {
            framePending_ = framePending_ || frame;
            // A one-shot redraw may be the last callback; retry before mpv's VO timeout.
            if (framePending_ || videoFramePainted_) {
                const int remainingMs =
                    pacingTimer_.isActive() ? pacingTimer_.remainingTime() : -1;
                if (!pacingTimer_.isActive() || remainingMs < 0 ||
                    remainingMs > kUnpaintableRetryMs) {
                    clearPacingProfile();
                    pacingTimerArmed_ = true;
                    // Retry timers must not bias presentation-lead estimates.
                    pacingTargetTime_ = 0;
                    pacingRetryOnly_ = true;
                    pacingTimer_.start(kUnpaintableRetryMs);
                }
            }
            if (!g_frameTimingLogPath.isEmpty())
                logFrameTiming(QStringLiteral("unpaintable streak=%1 (grace; frame held)")
                                   .arg(unpaintableStreak_));
            releaseRenderContextCurrent(previous);
            return;
        }
        if (!skippingFrames_) {
            skippingFrames_ = true;
            if (!g_frameTimingLogPath.isEmpty())
                logFrameTiming(QStringLiteral("skip-mode entered (streak=%1)")
                                   .arg(unpaintableStreak_));
        }
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
    // The lead is measured here because this is the moment the compositor has the
    // frame: FBO composition and the vsync wait both happen between paintGL
    // returning and this signal, and a lead that left them out would keep the
    // timer scheduling frames later than the screen can take them.
    if (pacingMeasurePending_) {
        pacingMeasurePending_ = false;
        const double tookMs = static_cast<double>(steadyClockNowNs() - pacingFiredAtNs_) / 1.0e6;
        if (tookMs > 0.0 && tookMs < 1000.0) {
            renderLeadMs_ = std::clamp(0.8 * renderLeadMs_ + 0.2 * tookMs, kMinRenderLeadMs,
                                       kMaxRenderLeadMs);
        }
    }
    if (!videoFramePainted_) return;
    videoFramePainted_ = false;
    if (!contextReady_ || render_ == nullptr) return;
    QOpenGLContext* previous = nullptr;
    if (!makeRenderContextCurrent(&previous)) return;
    // T-201: the swap interval mpv's VO timing feeds on. A re-time burst - the
    // signature of the periodic frame-order glitch - shows here as a
    // discontinuity long before it shows anywhere else.
    if (!g_frameTimingLogPath.isEmpty()) {
        const qint64 nowNs = steadyClockNowNs();
        const qint64 deltaMs = lastSwapNs_ != 0 ? (nowNs - lastSwapNs_) / 1000000 : -1;
        logFrameTiming(QStringLiteral("swap now=%1 delta_ms=%2")
                           .arg(QString::number(nowNs), QString::number(deltaMs)));
        lastSwapNs_ = nowNs;
    }
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

bool VideoSurface::clientFramePacingEnabled() {
    // The kill-switch is read once per process: it exists for a rollback (see
    // plan.md Phase 2a) and for field diagnostics, both of which relaunch. An
    // unset variable keeps the fix on; only an explicit "0" disables it.
    static const bool disabled =
        qEnvironmentVariable("MIKAI_CLIENT_FRAME_PACING") == QStringLiteral("0");
    return !disabled;
}

void VideoSurface::schedulePacedPaint() {
    const bool profilePacing = mikai::gui::uiprofile::countersEnabled();
    // A forced repaint (resize, re-show, an engine hold) or pacing disabled
    // paints at once: the deferred path exists only to place a decoded frame
    // into the compositor slot mpv named for it, and nothing else may wait.
    if (forceRender_ || !framePending_ || !clientFramePacingEnabled()) {
        pacingTimer_.stop();
        clearPacingProfile();
        pacingTimerArmed_ = false;
        pacingTargetTime_ = 0;
        pacingRetryOnly_ = false;
        if (profilePacing) profilePacingReason_ = "forced";
        update();
        return;
    }
    mpv_render_frame_info info{};
    bool haveTarget = false;
    bool repeatedOrRedraw = false;
    qint64 targetTime = 0;
    if (render_->nextFrameInfo(info)) {
        repeatedOrRedraw =
            (info.flags & (MPV_RENDER_FRAME_INFO_REDRAW | MPV_RENDER_FRAME_INFO_REPEAT)) != 0;
        haveTarget = isRealNewFrame(info);
        if (haveTarget) targetTime = info.target_time;
    }
    const qint64 nowUs = render_->timeUs();
    const uint64_t apiVersion = mpvApiVersion_;
    const bool targetTimeInNanoseconds =
        (apiVersion >> 16) > 2 || ((apiVersion >> 16) == 2 && (apiVersion & 0xffffu) >= 2);
    const std::optional<int> delay =
        haveTarget ? clientPacingDelayMilliseconds(targetTime, nowUs, renderLeadMs_, apiVersion)
                   : std::nullopt;
    if (!delay.has_value()) {
        // No frame info, a slot that already passed, or a target so far out it
        // belongs to a paused core: paint at once, exactly as before.
        pacingTimer_.stop();
        clearPacingProfile();
        pacingTimerArmed_ = false;
        pacingTargetTime_ = 0;
        pacingRetryOnly_ = false;
        const char* reason = repeatedOrRedraw
                                 ? ((info.flags & MPV_RENDER_FRAME_INFO_REPEAT) != 0
                                        ? "repeat" : "redraw")
                                 : (haveTarget ? "elapsed" : "no-info");
        if (profilePacing) profilePacingReason_ = reason;
        if (!g_frameTimingLogPath.isEmpty()) {
            logFrameTiming(QStringLiteral("pace immediate target_time=%1 unit=%2 now_us=%3 reason=%4")
                               .arg(QString::number(targetTime),
                                    QString::fromLatin1(targetTimeInNanoseconds ? "ns" : "us"),
                                    QString::number(nowUs), QString::fromLatin1(reason)));
        }
        update();
        return;
    }
    if (pacingTimerArmed_ && !pacingRetryOnly_ && pacingTargetTime_ <= targetTime) {
        // Preserve earlier presentation slots; retries have no presentation target.
        return;
    }
    pacingTimerArmed_ = true;
    pacingTargetTime_ = targetTime;
    pacingRetryOnly_ = false;
    // Round up so the timer never fires before the lead-adjusted slot.
    const int delayMs = *delay;
    pacingTimer_.start(delayMs);
    if (profilePacing) {
        profilePacingArmedAtNs_ = mikai::gui::uiprofile::nowNs();
        profilePacingAwaitingPaint_ = true;
        profilePacingFired_ = false;
        profilePacingReason_ = "paced";
    }
    if (!g_frameTimingLogPath.isEmpty()) {
        logFrameTiming(QStringLiteral("pace deferred target_time=%1 unit=%2 now_us=%3 delay_ms=%4 lead_ms=%5")
                           .arg(QString::number(targetTime),
                                QString::fromLatin1(targetTimeInNanoseconds ? "ns" : "us"),
                                QString::number(nowUs))
                           .arg(delayMs)
                           .arg(QString::number(renderLeadMs_, 'f', 2)));
    }
}

void VideoSurface::onPacingTick() {
    const bool retry = pacingRetryOnly_;
    pacingTimerArmed_ = false;
    pacingTargetTime_ = 0;
    pacingRetryOnly_ = false;
    if (!contextReady_ || render_ == nullptr) return;
    if (!paintCanHappen()) {
        // Qt would discard update(); advance to skip/ack rather than strand the frame.
        advanceRenderState();
        return;
    }
    unpaintableStreak_ = 0;
    // An unpresented painted frame still needs composition and one swap report.
    if (framePending_ || forceRender_ || videoFramePainted_) {
        if (!retry && mikai::gui::uiprofile::countersEnabled() &&
            profilePacingAwaitingPaint_) {
            profilePacingFired_ = true;
        }
        pacingFiredAtNs_ = retry ? 0 : steadyClockNowNs();
        pacingMeasurePending_ = !retry;
        // The paint this update() asks for is the frame's presentation, not a
        // repaint: it may consume the frame and report its swap.
        pacedPaintReleased_ = !retry;
        update();
    }
}

void VideoSurface::resetRenderTargetFormat() {
    // Never called per paint or per resize: the queries below cost a round trip,
    // and their answer describes one target - so a new context or a released one
    // makes it unknown again rather than merely stale.
    mikai::gui::uiprofile::noteRenderTargetFormatReset();
    renderFormatProbed_ = false;
    renderInternalFormat_ = 0;
    renderDepthBitsPerChannel_ = 0;
    renderTargetColorBits_ = {};
}

void VideoSurface::probeFboColorDepth() {
    if (context_ == nullptr) return;
    QOpenGLExtraFunctions* gl = context_->extraFunctions();
    if (gl == nullptr) return;
    // Probe the framebuffer mpv will actually draw into, not whichever one a
    // caller happened to leave bound.
    context_->functions()->glBindFramebuffer(
        GL_FRAMEBUFFER, static_cast<GLuint>(defaultFramebufferObject()));
    renderFormatProbed_ = true;
    mikai::gui::uiprofile::noteRenderTargetProbe();
    // Discover the attached object's actual sized format. Component bit counts
    // are not enough to distinguish e.g. packed normalized and float formats.
    constexpr GLenum kAttachmentObjectType = 0x8CD0;
    constexpr GLenum kAttachmentObjectName = 0x8CD1;
    constexpr GLenum kTextureObject = 0x1702;
    constexpr GLenum kRenderbufferObject = 0x8D41;
    constexpr GLenum kTextureInternalFormat = 0x1003;
    constexpr GLenum kRenderbufferInternalFormat = 0x8D44;
    GLint objectType = 0;
    GLint objectName = 0;
    GLint internalFormat = 0;
    gl->glGetFramebufferAttachmentParameteriv(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, kAttachmentObjectType, &objectType);
    gl->glGetFramebufferAttachmentParameteriv(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, kAttachmentObjectName, &objectName);
    gl->glGetFramebufferAttachmentParameteriv(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0x8212, &renderTargetColorBits_.red);
    gl->glGetFramebufferAttachmentParameteriv(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0x8213, &renderTargetColorBits_.green);
    gl->glGetFramebufferAttachmentParameteriv(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0x8214, &renderTargetColorBits_.blue);
    if (objectType == static_cast<GLint>(kTextureObject)) {
        GLint previousTexture = 0;
        gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(objectName));
        gl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, kTextureInternalFormat, &internalFormat);
        gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    } else if (objectType == static_cast<GLint>(kRenderbufferObject)) {
        GLint previousRenderbuffer = 0;
        gl->glGetIntegerv(0x8CA7, &previousRenderbuffer);
        gl->glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(objectName));
        gl->glGetRenderbufferParameteriv(GL_RENDERBUFFER, kRenderbufferInternalFormat,
                                         &internalFormat);
        gl->glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previousRenderbuffer));
    }
    if (renderTargetColorBits_.red > 0 &&
        renderTargetColorBits_.red == renderTargetColorBits_.green &&
        renderTargetColorBits_.red == renderTargetColorBits_.blue) {
        renderDepthBitsPerChannel_ = renderTargetColorBits_.red;
    } else {
        renderDepthBitsPerChannel_ = 0;
    }
    switch (internalFormat) {
        case kGlInternalFormatRgba8:
        case kGlInternalFormatRgb10A2:
        case kGlInternalFormatRgba16:
        case kGlInternalFormatRgba16F:
            renderInternalFormat_ = internalFormat;
            break;
        default:
            renderInternalFormat_ = 0;
            break;
    }
    if (!g_frameTimingLogPath.isEmpty())
        logFrameTiming(QStringLiteral("fbo red_bits=%1 green_bits=%2 blue_bits=%3 depth_bits=%4 "
                                      "internal_format=%5")
                           .arg(renderTargetColorBits_.red)
                           .arg(renderTargetColorBits_.green)
                           .arg(renderTargetColorBits_.blue)
                           .arg(renderDepthBitsPerChannel_)
                           .arg(internalFormat, 0, 16));
}

void VideoSurface::logFrameTiming(const QString& line) {
    if (timingLogBroken_) return;
    const QString path = frameTimingLogPath();
    if (path.isEmpty()) return;
    if (frameTimingLog_.isOpen() && frameTimingLog_.fileName() != path) {
        if (!timingLogBuffer_.isEmpty() &&
            frameTimingLog_.write(timingLogBuffer_) != timingLogBuffer_.size()) {
            timingLogBroken_ = true;
            qWarning("VideoSurface timing log: path-change flush failed for '%s'",
                     qUtf8Printable(frameTimingLog_.fileName()));
            timingLogBuffer_.resize(0);
            frameTimingLog_.close();
            return;
        }
        timingLogBuffer_.resize(0);
        frameTimingLog_.close();
    }
    if (!frameTimingLog_.isOpen()) {
        frameTimingLog_.setFileName(path);
        if (!frameTimingLog_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            timingLogBroken_ = true;
            qWarning("VideoSurface timing log: cannot open '%s': %s",
                     qUtf8Printable(path), qUtf8Printable(frameTimingLog_.errorString()));
            frameTimingLog_.setFileName(QString());
            return;
        }
        timingLogFlushClock_.start();
    }
    timingLogBuffer_.append((line + QLatin1Char('\n')).toUtf8());
    if (timingLogBuffer_.size() < 32768 && timingLogFlushClock_.elapsed() < 1000) return;
    const qint64 written = frameTimingLog_.write(timingLogBuffer_);
    if (written != timingLogBuffer_.size()) {
        timingLogBroken_ = true;
        qWarning("VideoSurface timing log: write failed for '%s'",
                 qUtf8Printable(frameTimingLog_.fileName()));
        timingLogBuffer_.resize(0);
        frameTimingLog_.close();
        frameTimingLog_.setFileName(QString());
        return;
    }
    timingLogBuffer_.resize(0);
    timingLogFlushClock_.restart();
}

QString VideoSurface::resolvedChainForCurrentSource(const QString& candidateId) const {
    const QString shaders = anime4k::shaderStringForCandidate(candidateId);
    if (shaders.isEmpty()) return shaders;  // Off or unknown: not the gate's business
    if (hdrFixesEnabled() && sourceColorProfile_.requiresSdrGate()) return QString();
    return shaders;
}

void VideoSurface::handleSourceColorProfileChanged() {
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
        const QString requested = anime4k::shaderStringForCandidate(chainId);
        if (requiresSdrGate != wasGateRequired && !requested.isEmpty() && mpv_ != nullptr) {
            const QString allowed = resolvedChainForCurrentSource(chainId);
            if (allowed.isEmpty()) {
                emit anime4kGated(tr("Anime4K skipped: source transfer metadata is unknown or HDR"));
            }
            applyShaderChainRequest(allowed);
            // A paused core produces no frame to rebuild the chain on, so the
            // render state is refreshed the same way a user request would be.
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
    // Update HDR/Dolby metadata from color profile
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
    // Check for Dolby Atmos in audio codec
    videoMetadata_.hasDolbyAtmos = videoMetadata_.audioCodec.contains(QStringLiteral("eac3")) ||
                                   videoMetadata_.audioCodec.contains(QStringLiteral("truehd"));
    emit videoMetadataChanged(videoMetadata_);
}

void VideoSurface::updateVideoMetadata() {
    // Check for Dolby Vision in video codec
    if (videoMetadata_.videoCodec.contains(QStringLiteral("dvhe")) ||
        videoMetadata_.videoCodec.contains(QStringLiteral("dovi"))) {
        videoMetadata_.isDolbyVision = true;
        videoMetadata_.hdrFormat = QStringLiteral("Dolby Vision");
    }
    // Check for Dolby Atmos in audio codec
    videoMetadata_.hasDolbyAtmos = videoMetadata_.audioCodec.contains(QStringLiteral("eac3")) ||
                                   videoMetadata_.audioCodec.contains(QStringLiteral("truehd"));
    emit videoMetadataChanged(videoMetadata_);
}

void VideoSurface::enforceAnimeJanaiHdrGate() {
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

void VideoSurface::setAnimeJanaiHdrOverride(bool allowed) {
    if (g_animeJanaiHdrOverride == allowed) return;
    g_animeJanaiHdrOverride = allowed;
    ++g_animeJanaiHdrOverrideRevision;
}

bool VideoSurface::animeJanaiHdrOverride() {
    return g_animeJanaiHdrOverride;
}

void VideoSurface::applyHdrPolicyToEngine() {
    appliedHdrPolicyRevision_ = hdrToneMappingPolicyRevision();
    if (mpv_ == nullptr) return;
    if (!hdrFixesEnabled()) return;
    for (const auto& option : hdrPolicyOptionList(hdrToneMappingPolicy())) {
        // Tolerated: a runtime older than an option refuses that one entry, is
        // named in the warning log, and playback continues unaffected.
        issueCoreCommand({"set", option.first, option.second}, option.first,
                         /*reportRefusal=*/false);
    }
}

void VideoSurface::setPumpSuspended(bool suspended) {
    // T-207: see the header. The timer's interval is set once in the
    // constructor and start()/stop() preserve it; a resume does not need to
    // know it.
    if (suspended) {
        statePump_.stop();
    } else {
        statePump_.start();
    }
}

void VideoSurface::setDiagnosticsPolling(bool enabled) {
    // T-205: the raw-counter poll costs an mpv property read twice a second
    // (the vf entry as a full node tree, the heaviest recurring read in normal
    // playback) while no in-app consumer of these numbers exists at all.
    if (diagnosticsPolling_ == enabled) return;
    diagnosticsPolling_ = enabled;
    if (!enabled) {
        // A disabled poll owns no in-flight reads and answers no stale values;
        // the next source re-arms the clock either way.
        resetDiagnostics();
    }
}

void VideoSurface::setVideoScalePolicy(const VideoScalePolicy& policy) {
    VideoScalePolicy normalized = policy;
    normalized.zoomPercent = clampedVideoZoomPercent(policy.zoomPercent);
    if (normalized == g_videoScalePolicy) return;
    g_videoScalePolicy = normalized;
    ++g_videoScalePolicyRevision;
    // Every live surface adopts the new revision on its own pump tick, so this
    // setter never has to know how many surfaces exist.
}

VideoScalePolicy VideoSurface::videoScalePolicy() {
    return g_videoScalePolicy;
}

void VideoSurface::setVideoScaleMode(VideoScaleMode mode) {
    VideoScalePolicy policy = g_videoScalePolicy;
    policy.mode = mode;
    setVideoScalePolicy(policy);
}

int VideoSurface::videoScalePolicyRevision() { return g_videoScalePolicyRevision; }

std::optional<std::string> VideoSurface::downscaleDefaultForCore() {
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

void VideoSurface::applyVideoScaleToEngine(bool queryCoreDefault) {
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
        // The engine still holds the explicit id this surface wrote and its real
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
    // newer batch or leave a paused surface displaying its old snapshot.
    const quint64 batch = ++videoScaleBatch_;
    videoScaleBatchPending_ = 0;
    for (const VideoScaleOption& option : writes) {
        // Issued here rather than through issueCoreCommand: that helper reports a
        // refusal through errorOccurred, which the player treats as a failed
        // source and answers with a re-extraction. A scaler id the runtime does
        // not know must name itself without claiming the stream failed.
        const std::string name = option.name;
        const AsyncRequestId request =
            mpv_->commandAsync({"set", name, option.value},
                               [this, name, batch](const AsyncReply& reply) {
                                   if (mpv_ == nullptr) return;
                                   QString refusal;
                                   if (!reply.ok()) {
                                       const QString reason = QString::fromStdString(
                                           mpv_->describeError(reply.error));
                                       qWarning("VideoSurface: %s refused (%s)", name.c_str(),
                                                qUtf8Printable(reason));
                                       refusal = tr("%1: %2").arg(QString::fromStdString(name), reason);
                                   }
                                   if (batch == videoScaleBatch_ && --videoScaleBatchPending_ == 0) {
                                       refreshRenderState();
                                   }
                                   // A receiver can destroy this surface while reporting.
                                   if (!refusal.isEmpty()) emit displayScalingRefused(refusal);
                               });
        // commandAsync drops rejected submissions without invoking their callback.
        if (request.valid()) ++videoScaleBatchPending_;
    }
    // What the engine now holds, not what was asked for: an explicit id written
    // here, a restored default, or - when Auto could not be produced - whatever
    // was there before, which stays named as explicit.
    if (!policy.downscale.isEmpty()) {
        downscaleWasExplicit_ = true;
    } else if (!downscaleDefault.empty()) {
        downscaleWasExplicit_ = false;
    }
    emit videoScaleStateChanged();
}

std::optional<VideoScaleFitLock> VideoSurface::acquireVideoScaleFitLock() {
    if (compareFitActive_) return std::nullopt;
    compareFitActive_ = true;
    applyVideoScaleToEngine();
    return VideoScaleFitLock(this, true);
}

void VideoScaleFitLock::release() {
    if (!held_) return;
    held_ = false;
    auto* surface = static_cast<VideoSurface*>(surface_.data());
    if (surface == nullptr) return;
    surface->compareFitActive_ = false;
    surface->applyVideoScaleToEngine();
}

void VideoSurface::setHdrToneMappingPolicy(HdrToneMappingPolicy policy) {
    if (policy == g_hdrToneMappingPolicy) return;
    g_hdrToneMappingPolicy = policy;
    ++g_hdrToneMappingPolicyRevision;
    // The pump of every live surface adopts the new revision on its own tick,
    // so this setter never has to know how many surfaces exist.
}

VideoSurface::HdrToneMappingPolicy VideoSurface::hdrToneMappingPolicy() {
    return g_hdrToneMappingPolicy;
}

void VideoSurface::setFrameTimingLogPath(const QString& path) {
    g_frameTimingLogPath = path;
    // The frame-timing log prints these counters, so they must be collected for
    // that launch even when MIKAI_UI_PROFILE was not set.
    mikai::gui::uiprofile::forceCounters(!path.isEmpty());
}

QString VideoSurface::frameTimingLogPath() {
    return g_frameTimingLogPath;
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
    // Captured before the render context goes: freeing it kills the old video
    // output, which then reports 0 for a file it no longer decodes. `position_`
    // is what the viewer and the context probe saw, so it is what a reload
    // resumes at. A snapshot still standing belongs to a reload that has painted
    // nothing yet, so a second loss keeps it rather than resuming a core that was
    // just handed that same offset.
    if (!pendingResumePositionValid_ && std::isfinite(position_) && position_ >= 0.0) {
        // 0.0 is a real offset: a loss at the very start must resume at the start.
        pendingResumePosition_ = position_;
        pendingResumePositionValid_ = true;
    }
    clearPacingProfile();
    pacingTimer_.stop();
    pacingTimerArmed_ = false;
    pacingTargetTime_ = 0;
    pacingRetryOnly_ = false;
    pacingMeasurePending_ = false;
    pacedPaintReleased_ = false;
    // Its pixels belonged to a context that is going away.
    invalidatePresentedPicture();
    if (render_ == nullptr) {
        if (contextDestroyedConnection_) {
            disconnect(contextDestroyedConnection_);
            contextDestroyedConnection_ = QMetaObject::Connection();
        }
        contextReady_ = false;
        context_ = nullptr;
        return;
    }
    QOpenGLContext* previous = nullptr;
    const bool current = makeRenderContextCurrent(&previous);
    // One last picture out of the context before it goes: the current frame is
    // rendered into a private snapshot first, because Qt has already destroyed
    // the widget's framebuffers by the time this slot runs. Read back at the
    // target's own precision, one copy per context, never per paint.
    if (current && !destroying_ && !engineFrameHoldFbo_ && hasVisibleOutputFrame_) {
        captureEngineFrameHold();
    }
    if (current) captureTeardownPicture();
    if (current) engineFrameHoldFbo_.reset();
    // The probe answered for a framebuffer this context owned, so it is reset
    // only after the readback above has used it.
    resetRenderTargetFormat();
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
    watch("idle-active", MPV_FORMAT_FLAG,
          [this](const Property& p) { observedIdle_ = !p.isFlag() || p.flag; });
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
    // What the picture actually looks like after mpv's filters: dw/dh already
    // carry the sample aspect ratio, while rotate is the intended clockwise turn
    // that has NOT been applied to that output. handleState composes the two, so
    // an anamorphic or portrait source is not measured by its coded size.
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
    // events in the same drain as everything else, so no poll and no core lock
    // is ever involved in knowing the verdict that gates the chains.
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
    // Video codec and audio codec watchers for metadata display
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

void VideoSurface::setNextMediaTrackSelection(MediaTrackSelection selection) {
    nextMediaTrackSelection_ = std::move(selection);
}

void VideoSurface::resetMediaTracks() {
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
    const bool currentLoaded =
        entryLoadReported_ && entrySourceEpoch_ == mpv_->sourceEpoch();
    if (currentLoaded && observedDuration_ > 0 && observedDuration_ != duration_) {
        duration_ = observedDuration_;
        emit durationChanged(duration_);
    }
    // 10 ms is finer than any display, so this cannot look stepped.
    if (currentLoaded && std::abs(observedPosition_ - position_) > 0.01) {
        position_ = observedPosition_;
        emit positionChanged(position_);
    }
    // Compare like with like: the property is "pause", the cached state is
    // "playing". Guarding on `paused != playing_` means a playing video
    // (false vs false) never updates, so isPlaying() stays false forever. A write
    // still in flight is the viewer's intent, so an observation that has not
    // caught up with it cannot flip the control back.
    const bool playing =
        !url_.isEmpty() && !observedIdle_ && !requestedPause() && !enginePauseHeld();
    if (playing != playing_) {
        playing_ = playing;
        publishPlayingState(playing_);
    }
}

void VideoSurface::publishPlayingState(bool playing) {
    // The session must not blank the screen or idle-suspend under a playing
    // video. Each surface owns one reason, so a paused main player does not
    // release the lock a picture-in-picture window still needs.
    mikai::gui::idleInhibitor().setActiveFor(this, playing);
    emit playingChanged(playing);
}

void VideoSurface::pumpEvents() {
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
    // Display geometry includes sample aspect and rotation; processing retains coded size.
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

void VideoSurface::publishDisplayAspectRatio(double ratio) {
    if (!std::isfinite(ratio) || ratio <= 0.0) ratio = 0.0;
    if (ratio == displayAspectRatio_) return;
    displayAspectRatio_ = ratio;
    emit displayAspectRatioChanged(ratio);
}

PlaybackDiagnostics VideoSurface::playbackDiagnostics() const {
    return diagnostics_;
}

void VideoSurface::clearPacingProfile() {
    profilePacingAwaitingPaint_ = false;
    profilePacingArmedAtNs_ = 0;
    profilePacingFired_ = false;
}

VideoSurface::UiProfileCounters VideoSurface::uiProfileCounters() const {
    UiProfileCounters counters;
    counters.initializeGlCalls = profileInitializeGlCalls_;
    counters.contextGeneration = renderGeneration_;
    counters.paintCalls = profilePaintCalls_;
    counters.pacingDisarms = profilePacingDisarms_;
    counters.lastPaintDelayMs = profileLastPaintDelayMs_;
    counters.lastPaintEntryNs = profileLastPaintEntryNs_;
    return counters;
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
    // T-205: the app - which has no consumer of the raw counters - switches the
    // poll off on every surface it owns, so normal playback performs no mpv
    // reads at all here. Probes keep the constructor default (on) because their
    // verdicts assert on these counters, and they alone read them.
    // A frame-timing log carries these counters, so that diagnostic launch keeps
    // the poll alive even though the app normally switches it off (T-205).
    if (!mpv_ || !diagnosticsActive_ ||
        (!diagnosticsPolling_ && g_frameTimingLogPath.isEmpty()))
        return;
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

void VideoSurface::setDiagnosticUnknown(DiagnosticSlot slot) {
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
