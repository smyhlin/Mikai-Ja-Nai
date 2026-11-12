// Menu model contents: every row builder, rebuild, lookup, cached submenu level
// and typed row activation of the native Quick player controller. The models
// themselves are allocated by the controller's constructor path; this unit owns
// what they carry.

#include "PlayerController.hpp"

#include "PlayerMenuModel.hpp"

#include "../core/MediaTracks.hpp"
#include "../core/VideoSession.hpp"
#include "../native/models/NativeUpscalers.hpp"
#include "../native/status/NativeMenu.hpp"

#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace mplayer {
namespace quick {

namespace {

using Row = PlayerMenuModel::Row;
using Action = PlayerMenuModel::Action;

// Enhancement level ids. The catalogue's own structural levels plus the
// AnimeJaNai levels this controller routes to.
constexpr auto kUpscalersRoot = "processing:upscale";
constexpr auto kUpscaleProfiles = "processing:upscale:profiles";
constexpr auto kUpscaleSpatial = "processing:upscale:spatial";
constexpr auto kUpscaleSpatialCustom = "processing:upscale:spatial:custom";
constexpr auto kUpscaleOptional = "processing:upscale:optional";
constexpr auto kRestoreRoot = "processing:restore";
constexpr auto kInterpolationRoot = "processing:interpolation";
constexpr auto kAnimeJanaiRife = "animejanai:rife";
constexpr auto kAnimeJanaiBackend = "animejanai:backend";

constexpr int kModelsPerPage = 8;
constexpr auto kAnime4KSettingKey = "player/anime4k";

QString rifeVersion(const native::AnimeJanaiPreset& preset) {
    // A standard preset's model file carries its RIFE version; the catalogue
    // already resolved that identity when the preset was listed.
    QString text = preset.modelFile;
    if (text.isEmpty()) text = preset.id;
    const int at = text.indexOf(QStringLiteral("v4."), 0, Qt::CaseInsensitive);
    if (at < 0) return QString();
    int end = at + 3;
    while (end < text.size() &&
           (text.at(end).isDigit() || text.at(end) == QLatin1Char('.'))) {
        ++end;
    }
    return text.mid(at, end - at);
}

int rifeMinor(const QString& version) {
    bool ok = false;
    const int minor = version.section(QLatin1Char('.'), 1, 1).toInt(&ok);
    return ok ? minor : -1;
}

int rifeRange(const QString& version) {
    const int minor = rifeMinor(version);
    return minor >= 7 ? (minor - 7) / 5 : -1;
}

QString rifeRangeLabel(int range) {
    const int first = 7 + range * 5;
    return PlayerController::tr("RIFE v4.%1–v4.%2").arg(first).arg(first + 4);
}

/// A catalogue-derived RIFE level exists only while a preset hangs in it. The
/// structural levels always stand.
bool isCatalogueRifeLevel(const QString& levelId) {
    const QString rife = QString::fromLatin1(kAnimeJanaiRife);
    return levelId.startsWith(rife + QStringLiteral(":range:")) ||
           levelId.startsWith(rife + QStringLiteral(":version:"));
}

bool rifeLevelHasPreset(const QString& levelId,
                        const QVector<native::AnimeJanaiPreset>& presets) {
    const QString rife = QString::fromLatin1(kAnimeJanaiRife);
    if (levelId.startsWith(rife + QStringLiteral(":range:"))) {
        const int range = levelId.section(QLatin1Char(':'), -1).toInt();
        for (const native::AnimeJanaiPreset& preset : presets) {
            if (preset.rife && rifeRange(rifeVersion(preset)) == range) return true;
        }
        return false;
    }
    if (levelId.startsWith(rife + QStringLiteral(":version:"))) {
        const QString version = levelId.section(QLatin1Char(':'), -1);
        for (const native::AnimeJanaiPreset& preset : presets) {
            if (preset.rife && rifeVersion(preset) == version) return true;
        }
        return false;
    }
    return true;
}

bool privateNativeRuntimeActive() {
    const native::RuntimeSelection& runtime = native::currentSelection();
    return runtime.loaded && runtime.kind == native::BundleKind::Native;
}

void appendAnime4KPresetRow(QVector<Row>& rows, const anime4k::MenuRow& source,
                            const QString& currentId) {
    Row row;
    row.id = QStringLiteral("anime4k.") + source.id;
    row.label = source.label;
    row.tooltip = source.tooltip;
    switch (source.role) {
        case anime4k::MenuRow::Role::Preset:
            row.action = Action::SetEnhancement;
            row.value = QStringLiteral("anime4k|") + source.id;
            row.selected = source.id == currentId;
            break;
        case anime4k::MenuRow::Role::Category:
            row.action = Action::OpenMenu;
            row.value = source.id;
            row.submenu = source.hasChildren;
            row.enabled = source.hasChildren;
            row.selected = currentId != QLatin1String("off") &&
                           anime4k::categoryContainsPreset(source.id, currentId);
            break;
    }
    rows.append(row);
}

}  // namespace

// ── row builders ────────────────────────────────────────────────────────────

QVector<PlayerMenuModel::Row> PlayerController::makeRows(
    const QVector<native::menu::Row>& nativeRows) {
    QVector<Row> rows;
    rows.reserve(nativeRows.size());
    for (const native::menu::Row& source : nativeRows) {
        Row row;
        row.id = source.id;
        row.label = source.label;
        row.tooltip = source.tooltip;
        row.enabled = !source.disabled;
        row.submenu = source.submenu;
        row.selected = source.current;
        row.action = source.submenu ? Action::OpenMenu : Action::None;
        row.value = source.value;
        rows.append(row);
    }
    return rows;
}

QVector<PlayerMenuModel::Row> PlayerController::capturePictureRows() const {
    QVector<Row> rows;
    Row frame;
    frame.id = QStringLiteral("picture.frame");
    frame.label = tr("Copy frame");
    frame.icon = QStringLiteral("camera");
    frame.action = Action::CaptureFrame;
    frame.value = QString::number(static_cast<qulonglong>(
        session_ != nullptr ? session_->sourceEpoch() : 0));
    rows.append(frame);

    Row compare;
    compare.id = QStringLiteral("picture.compare");
    compare.label = tr("Compare quality");
    compare.icon = QStringLiteral("compare");
    compare.action = Action::OpenCompare;
    compare.value = frame.value;
    rows.append(compare);

    Row link;
    link.id = QStringLiteral("picture.copyLink");
    link.label = tr("Copy link");
    link.icon = QStringLiteral("link");
    link.action = Action::CopyMediaLink;
    rows.append(link);

    if (localMedia()) {
        Row folder;
        folder.id = QStringLiteral("picture.folder");
        folder.label = tr("Open containing folder");
        folder.icon = QStringLiteral("folder");
        folder.tooltip = QFileInfo(QUrl::fromUserInput(url_).toLocalFile()).absolutePath();
        folder.action = Action::OpenContainingFolder;
        rows.append(folder);

        Row information;
        information.id = QStringLiteral("picture.information");
        information.label = tr("Media information");
        information.icon = QStringLiteral("information");
        information.action = Action::OpenMediaInformation;
        rows.append(information);
    }

    Row clip;
    clip.id = QStringLiteral("picture.clip");
    clip.label = tr("Cut moment…");
    clip.icon = QStringLiteral("clapperboard");
    clip.action = Action::OpenClipDialog;
    rows.append(clip);

    Row scale;
    scale.id = QStringLiteral("picture.scale");
    scale.label = tr("Video scaling");
    scale.icon = QStringLiteral("expand");
    scale.submenu = true;
    scale.action = Action::OpenMenu;
    scale.value = QStringLiteral("picture.scale");
    rows.append(scale);

    Row subtitles;
    subtitles.id = QStringLiteral("picture.subtitles");
    subtitles.label = tr("Subtitles");
    subtitles.tooltip = tr("Attach subtitles from disk or search OpenSubtitles");
    subtitles.icon = QStringLiteral("captions");
    subtitles.submenu = true;
    subtitles.action = Action::OpenMenu;
    subtitles.value = QStringLiteral("picture.subtitles");
    rows.append(subtitles);

    Row keys;
    keys.id = QStringLiteral("picture.keys");
    keys.label = tr("Keyboard shortcuts  (?)");
    keys.icon = QStringLiteral("keyboard");
    keys.action = Action::OpenShortcuts;
    rows.append(keys);

    Row exit;
    exit.id = QStringLiteral("picture.exit");
    exit.label = tr("Exit  (Esc)");
    exit.icon = QStringLiteral("exit");
    exit.action = Action::ExitPlayer;
    rows.append(exit);
    return rows;
}

QVector<PlayerMenuModel::Row> PlayerController::videoScaleModeRows() const {
    QVector<Row> rows;
    const VideoScaleMode modes[] = {VideoScaleMode::Fit, VideoScaleMode::Fill,
                                    VideoScaleMode::Stretch, VideoScaleMode::Aspect16x9,
                                    VideoScaleMode::Aspect4x3};
    for (const VideoScaleMode mode : modes) {
        Row row;
        const int ordinal = static_cast<int>(mode);
        row.id = QStringLiteral("scale.") + QString::number(ordinal);
        row.label = videoScaleModeLabel(mode);
        row.icon = QStringLiteral("expand");
        row.selected = videoScaleMode_ == ordinal;
        row.action = Action::SetVideoScaleMode;
        row.value = QString::number(ordinal);
        rows.append(row);
    }
    return rows;
}

QVector<PlayerMenuModel::Row> PlayerController::keyHintRows() const {
    struct Hint {
        const char* key;
        const char* description;
    };
    static const Hint hints[] = {
        {"Space", QT_TR_NOOP("Pause / play")},
        {"F / F11", QT_TR_NOOP("Fullscreen / windowed")},
        {"M", QT_TR_NOOP("Mute / unmute audio")},
        {"S", QT_TR_NOOP("Episode list")},
        {"T", QT_TR_NOOP("Choose audio track")},
        {"C", QT_TR_NOOP("Choose subtitle track")},
        {"Q", QT_TR_NOOP("Quality, speed and scaling")},
        {"←  →", QT_TR_NOOP("Seek ∓5 s")},
        {"↑  ↓", QT_TR_NOOP("Volume ±10 %")},
        {"Esc", QT_TR_NOOP("Close panel / window")},
        {"? / /", QT_TR_NOOP("Show keyboard shortcuts in player")},
        {"Enter", QT_TR_NOOP("Search from the search field")},
        {"Shift + scroll", QT_TR_NOOP("Jump through home feed rows")},
        {"Alt + scroll", QT_TR_NOOP("Scroll title rows or change provider")},
        {"Scroll", QT_TR_NOOP("Adjust volume over the player volume control")},
        {"Double-click", QT_TR_NOOP("Toggle player fullscreen")},
        {"F / S / R / P", QT_TR_NOOP("Compare: fit / swap / recapture / pin")},
        {"Space", QT_TR_NOOP("Compare: hold to peek at side A")},
        {"+ / − / 0", QT_TR_NOOP("Cover viewer: zoom in / out / reset")},
    };
    QVector<Row> rows;
    int index = 0;
    for (const Hint& hint : hints) {
        Row row;
        row.id = QStringLiteral("key.") + QString::number(index++);
        row.label = QString::fromLatin1(hint.key);
        row.detail = tr(hint.description);
        row.enabled = false;
        rows.append(row);
    }
    return rows;
}

QVector<PlayerMenuModel::Row> PlayerController::remedyRows() const {
    QVector<Row> rows;
    const bool animeJanaiFailure = lastProcessingReasonKey_.startsWith(QLatin1String("animejanai."));
    if (!lastProcessingReason_.isEmpty()) {
        Row reason;
        reason.id = QStringLiteral("remedy.reason");
        reason.label = lastProcessingReasonKey_;
        reason.detail = lastProcessingReason_;
        reason.heading = true;
        rows.append(reason);
    }
    if (!animeJanaiFailure) {
        Row prepare;
        prepare.id = QStringLiteral("remedy.prepare");
        prepare.label = tr("Prepare");
        prepare.tooltip = tr("Open processing and prepare this graph");
        prepare.action = Action::PrepareProcessing;
        rows.append(prepare);
    }
    Row providers;
    providers.id = QStringLiteral("remedy.providers");
    providers.label = animeJanaiFailure ? tr("Settings") : tr("Providers");
    providers.tooltip = animeJanaiFailure ? tr("Open processing and component settings")
                                          : tr("Choose the processing provider");
    providers.action = Action::OpenProcessingProviders;
    rows.append(providers);

    Row original;
    original.id = QStringLiteral("remedy.original");
    original.label = animeJanaiFailure ? tr("Turn off AnimeJaNai") : tr("Original");
    original.tooltip = animeJanaiFailure ? tr("Stop AnimeJaNai processing")
                                         : tr("Stay on the Original picture");
    original.action = Action::TurnOffProcessing;
    rows.append(original);
    return rows;
}

QString PlayerController::trackRowText(const VariantInfo& variant) {
    QString base;
    if (!variant.display.isEmpty() && !variant.track.isEmpty()) {
        base = QStringLiteral("%1 · %2").arg(variant.display, variant.track);
    } else {
        base = variant.label;
    }
    return variant.quality > 0 ? QStringLiteral("%1 · %2p").arg(base).arg(variant.quality) : base;
}

QVector<PlayerMenuModel::Row> PlayerController::mediaTrackRows(const QString& kind) const {
    QVector<Row> rows;
    if (session_ == nullptr) return rows;
    const MediaTrackSnapshot& snapshot = session_->mediaTracks();
    if (!snapshot.ready) return rows;
    const MediaTrackType type =
        kind == QLatin1String("sub") ? MediaTrackType::Subtitle : MediaTrackType::Audio;
    bool any = false;
    for (const MediaTrack& track : snapshot.tracks) {
        if (track.type == type) {
            any = true;
            break;
        }
    }
    if (!any) return rows;
    const auto token = [&snapshot, type](int64_t id) {
        return QStringLiteral("media|%1|%2|%3|%4")
            .arg(type == MediaTrackType::Audio ? QStringLiteral("audio") : QStringLiteral("sub"))
            .arg(id)
            .arg(static_cast<qulonglong>(snapshot.coreIdentity))
            .arg(static_cast<qulonglong>(snapshot.sourceEpoch));
    };
    const MediaTrackSelection selection = session_->mediaTrackSelection();
    if (type == MediaTrackType::Subtitle) {
        Row off;
        off.id = QStringLiteral("track.off");
        off.label = tr("Off");
        off.selected = selection.subtitle == 0;
        off.action = Action::SelectTrack;
        off.value = token(0);
        rows.append(off);
    }
    for (const MediaTrack& track : snapshot.tracks) {
        if (track.type != type) continue;
        QStringList details;
        const QString fallback = type == MediaTrackType::Audio ? tr("Audio track %1").arg(track.id)
                                                               : tr("Subtitle track %1").arg(track.id);
        details.append(track.title.isEmpty() ? fallback : track.title);
        if (!track.language.isEmpty()) details.append(track.language);
        if (!track.codec.isEmpty()) details.append(track.codec.toUpper());
        if (track.audioChannels > 0) details.append(tr("%1 ch").arg(track.audioChannels));
        if (track.external) details.append(tr("External"));
        if (track.forced) details.append(tr("Forced"));
        if (track.isDefault) details.append(tr("Default"));
        Row row;
        row.id = QStringLiteral("track.") + QString::number(track.id);
        row.label = details.join(QStringLiteral(" · "));
        row.tooltip = track.externalFilename.isEmpty()
                          ? row.label
                          : row.label + QLatin1Char('\n') + track.externalFilename;
        row.selected = track.selected;
        row.action = Action::SelectTrack;
        row.value = token(track.id);
        rows.append(row);
    }
    return rows;
}

QVector<PlayerMenuModel::Row> PlayerController::trackRows(const QString& kind) const {
    if (directMedia_) return mediaTrackRows(kind);
    QVector<Row> rows;
    QString heading;
    for (const VariantInfo& variant : variants_) {
        if (variant.embed.isEmpty()) continue;
        if (!kind.isEmpty() && variant.kind != kind) continue;
        if (variant.kindLabel != heading) {
            heading = variant.kindLabel;
            if (!heading.isEmpty()) {
                Row section;
                section.id = QStringLiteral("track.heading.") + heading;
                section.label = heading;
                section.heading = true;
                rows.append(section);
            }
        }
        const QString text = trackRowText(variant);
        Row row;
        row.id = QStringLiteral("track.") + variant.embed;
        row.label = text;
        row.detail = variant.label.isEmpty() || variant.label == text ? QString() : variant.label;
        row.selected = variant.embed == currentEmbed_;
        row.action = Action::SelectTrack;
        row.value = variant.embed;
        rows.append(row);
    }
    return rows;
}

void PlayerController::appendExternalTrackRow(QVector<PlayerMenuModel::Row>& rows,
                                              bool subtitle) {
    if (!subtitle && !localMedia()) return;
    Row row;
    row.id = subtitle ? QStringLiteral("track.external.sub") : QStringLiteral("track.external.audio");
    row.label = subtitle ? tr("Select subtitles from disk…") : tr("Select audio from disk…");
    row.icon = subtitle ? QStringLiteral("captions") : QStringLiteral("microphone");
    row.action = Action::LoadExternalTrack;
    row.value = subtitle ? QStringLiteral("sub") : QStringLiteral("audio");
    rows.append(row);
}

void PlayerController::appendSubtitleDelayRows(QVector<PlayerMenuModel::Row>& rows) {
    Row heading;
    heading.id = QStringLiteral("delay.heading");
    heading.label = tr("Subtitle delay · %1 s").arg(subtitleDelay_, 0, 'f', 1);
    heading.heading = true;
    rows.append(heading);

    const auto step = [this, &rows](double delta, const QString& text) {
        Row row;
        row.id = QStringLiteral("delay.") + QString::number(delta, 'f', 1);
        row.label = text;
        row.action = Action::SetSubtitleDelay;
        row.value = QString::number(subtitleDelay_ + delta, 'f', 3);
        rows.append(row);
    };
    step(-0.1, tr("0.1 s earlier  (−0.1)"));
    step(0.1, tr("0.1 s later  (+0.1)"));

    Row reset;
    reset.id = QStringLiteral("delay.reset");
    reset.label = tr("Reset delay  (0.0 s)");
    reset.enabled = !qFuzzyIsNull(subtitleDelay_);
    reset.action = Action::ResetSubtitleDelay;
    rows.append(reset);
}

QVector<PlayerMenuModel::Row> PlayerController::enhancementRootRows() const {
    QVector<Row> rows;
    const QString anime4kId = anime4kChainId_.isEmpty() ? anime4k::modeId(anime4kMode_)
                                                        : anime4kChainId_;
    for (const anime4k::MenuRow& source : anime4k::menuRows(QString())) {
        appendAnime4KPresetRow(rows, source, anime4kId);
    }
    const auto addCategory = [&rows](const QString& label, const QString& tooltip,
                                     const QString& levelId) {
        Row row;
        row.id = QStringLiteral("level.") + levelId;
        row.label = label;
        row.tooltip = tooltip;
        row.submenu = true;
        row.action = Action::OpenMenu;
        row.value = levelId;
        rows.append(row);
    };
    addCategory(tr("Restore (Anime4K)"), tr("Anime4K restoration shaders"),
                QString::fromLatin1(kRestoreRoot));
    addCategory(tr("Upscale"), tr("AnimeJaNai models, spatial upscalers, and optional graphs"),
                QString::fromLatin1(kUpscalersRoot));
    addCategory(tr("Interpolation (RIFE)"), tr("Temporal frame interpolation models"),
                QString::fromLatin1(kInterpolationRoot));
    addCategory(tr("Backend"), tr("Standard AnimeJaNai execution backend"),
                QString::fromLatin1(kAnimeJanaiBackend));
    if (privateNativeRuntimeActive()) {
        for (const Row& row : makeRows(native::menu::rootRows(nativeInputs_))) {
            rows.append(row);
        }
    }
    return rows;
}

QVector<PlayerMenuModel::Row> PlayerController::enhancementLevelRows(const QString& levelId) const {
    QVector<Row> rows;
    if (levelId.isEmpty()) return enhancementRootRows();

    const QString anime4kId = anime4kChainId_.isEmpty() ? anime4k::modeId(anime4kMode_)
                                                        : anime4kChainId_;
    const QString ajiId = animeJanaiPresetId_;
    const QVector<native::AnimeJanaiPreset> presets = native::animeJanaiPresets();
    const QString backendNote = animeJanaiNote_;

    // AnimeJaNai tree: interpolation, RIFE ranges/versions, backend.
    const auto addSubmenu = [&rows](const QString& label, const QString& id) {
        Row row;
        row.id = QStringLiteral("level.") + id;
        row.label = label;
        row.submenu = true;
        row.action = Action::OpenMenu;
        row.value = id;
        rows.append(row);
    };
    const auto appendAjRow = [](QVector<Row>& target, const Row& row) { target.append(row); };

    if (levelId == QLatin1String(kInterpolationRoot)) {
        bool currentIsRife = ajiId.startsWith(QLatin1String("rife:"));
        bool currentRifeFound = false;
        for (const native::AnimeJanaiPreset& preset : presets) {
            const bool selected = preset.rife &&
                                  preset.id.compare(ajiId, Qt::CaseInsensitive) == 0;
            currentIsRife |= selected;
            currentRifeFound |= selected;
        }
        Row off;
        off.id = QStringLiteral("aji.off");
        off.label = tr("Off");
        off.tooltip = tr("Disable the Standard AnimeJaNai preset; Anime4K is unchanged.");
        off.action = Action::SetEnhancement;
        off.value = QStringLiteral("animejanai|");
        off.selected = !currentIsRife;
        rows.append(off);
        if (currentIsRife && !currentRifeFound) {
            Row missing;
            missing.id = QStringLiteral("aji.missing");
            missing.label = tr("Unavailable · %1").arg(ajiId);
            missing.tooltip = tr("This model is no longer in the catalogue. The selection is kept "
                                 "until you choose another.");
            missing.action = Action::SetEnhancement;
            missing.value = QStringLiteral("animejanai|") + ajiId;
            missing.selected = true;
            missing.enabled = false;
            rows.append(missing);
        }
        // The RIFE ranges hang off this level.
        QMap<int, bool> ranges;
        for (const native::AnimeJanaiPreset& preset : presets) {
            if (preset.rife) ranges.insert(rifeRange(rifeVersion(preset)), true);
        }
        for (int range : ranges.keys()) {
            if (range >= 0) {
                addSubmenu(rifeRangeLabel(range), QString::fromLatin1(kAnimeJanaiRife) +
                                                      QStringLiteral(":range:") +
                                                      QString::number(range));
            }
        }
        return rows;
    }

    if (levelId.startsWith(QString::fromLatin1(kAnimeJanaiRife) + QStringLiteral(":range:"))) {
        const int range = levelId.section(QLatin1Char(':'), -1).toInt();
        QMap<int, QString> versions;
        for (const native::AnimeJanaiPreset& preset : presets) {
            const QString version = rifeVersion(preset);
            if (preset.rife && rifeRange(version) == range) {
                versions.insert(rifeMinor(version), version);
            }
        }
        for (const QString& version : versions.values()) {
            addSubmenu(tr("RIFE v%1").arg(version), QString::fromLatin1(kAnimeJanaiRife) +
                                                        QStringLiteral(":version:") + version);
        }
        return rows;
    }

    if (levelId.startsWith(QString::fromLatin1(kAnimeJanaiRife) + QStringLiteral(":version:"))) {
        const QString version = levelId.section(QLatin1Char(':'), -1);
        for (const native::AnimeJanaiPreset& preset : presets) {
            if (!preset.rife || rifeVersion(preset) != version) continue;
            Row row;
            row.id = QStringLiteral("aji.") + preset.id;
            row.label = preset.label;
            row.tooltip = preset.supported ? preset.reason
                                           : tr("Unavailable: %1").arg(preset.reason);
            row.action = Action::SetEnhancement;
            row.value = QStringLiteral("animejanai|") + preset.id;
            row.selected = preset.id == ajiId;
            row.enabled = preset.supported;
            appendAjRow(rows, row);
        }
        return rows;
    }

    if (levelId == QLatin1String(kAnimeJanaiBackend)) {
        struct BackendRow {
            native::Backend backend;
            QString label;
        };
        const BackendRow entries[] = {
            {native::Backend::TensorRT, tr("TensorRT")},
            {native::Backend::TensorRTRTX, tr("TensorRT for RTX")},
            {native::Backend::DirectML, tr("DirectML")},
        };
        const QString current = animeJanaiBackendName(animeJanaiBackend_);
        for (const BackendRow& entry : entries) {
            Row row;
            row.id = QStringLiteral("backend.") + native::stableName(entry.backend);
            row.label = entry.label;
            const QString backend = native::stableName(entry.backend);
            row.action = Action::SetEnhancement;
            row.value = QStringLiteral("ajibackend|") + backend;
            row.selected = backend == current;
            const bool installed =
                native::describeAnimeJanaiInstallation(entry.backend, false).installed;
#ifdef Q_OS_WIN
            if (!installed) {
                row.enabled = false;
                row.tooltip = tr("Not installed. Install it in Settings › Components.");
            } else if (entry.backend == native::Backend::TensorRTRTX) {
                row.tooltip = tr("In-process TensorRT-RTX 1.7 engine builds; CUDA GPUs, "
                                 "Windows only.");
                if (!backendNote.isEmpty()) row.tooltip += QStringLiteral("\n\n") + backendNote;
            }
#else
            if (entry.backend == native::Backend::TensorRTRTX) {
                row.enabled = false;
                row.tooltip = tr("TensorRT for RTX has no published runtime on this platform. "
                                 "The saved RTX request is kept but cannot run.");
            } else if (!installed) {
                row.enabled = false;
                row.tooltip = tr("Not installed. Install it in Settings › Components.");
            }
#endif
            rows.append(row);
        }
        return rows;
    }

    if (levelId == QLatin1String(kUpscalersRoot)) {
        bool currentIsRife = ajiId.startsWith(QLatin1String("rife:"));
        bool selectedPresetFound = ajiId.isEmpty();
        for (const native::AnimeJanaiPreset& preset : presets) {
            currentIsRife |= preset.rife &&
                             preset.id.compare(ajiId, Qt::CaseInsensitive) == 0;
            selectedPresetFound |= !preset.rife &&
                                   preset.id.compare(ajiId, Qt::CaseInsensitive) == 0;
        }
        Row off;
        off.id = QStringLiteral("aji.off");
        off.label = tr("Off");
        off.tooltip = tr("Disable the Standard AnimeJaNai preset; Anime4K is unchanged.");
        off.action = Action::SetEnhancement;
        off.value = QStringLiteral("animejanai|");
        off.selected = ajiId.isEmpty() || currentIsRife;
        rows.append(off);
        addSubmenu(tr("Profiles"), QString::fromLatin1(kUpscaleProfiles));
        addSubmenu(tr("Spatial models"), QString::fromLatin1(kUpscaleSpatial));
        addSubmenu(tr("Optional upscalers"), QString::fromLatin1(kUpscaleOptional));
        if (!selectedPresetFound && !ajiId.isEmpty() && !currentIsRife &&
            !ajiId.startsWith(QLatin1String("upscaler:"))) {
            Row missing;
            missing.id = QStringLiteral("aji.missing");
            missing.label = tr("Unavailable · %1").arg(ajiId);
            missing.tooltip = tr("This model is no longer in the catalogue. The selection is kept "
                                 "until you choose another.");
            missing.action = Action::SetEnhancement;
            missing.value = QStringLiteral("animejanai|") + ajiId;
            missing.selected = true;
            missing.enabled = false;
            rows.append(missing);
        }
        return rows;
    }

    const auto appendPaged = [&rows](const QVector<native::AnimeJanaiPreset>& presetsToShow,
                                     const QString& baseLevel, const QString& currentLevel,
                                     const QString& currentId) {
        int page = -1;
        if (currentLevel.startsWith(baseLevel + QStringLiteral(":page:"))) {
            bool ok = false;
            page = currentLevel.section(QLatin1Char(':'), -1).toInt(&ok);
            if (!ok || page < 0) page = -1;
        }
        const int pageCount = (presetsToShow.size() + kModelsPerPage - 1) / kModelsPerPage;
        if (page < 0 && pageCount > 1) {
            for (int index = 0; index < pageCount; ++index) {
                Row row;
                row.id = QStringLiteral("level.") + baseLevel + QStringLiteral(".page.") +
                         QString::number(index);
                row.label = PlayerController::tr("Models %1–%2")
                                .arg(index * kModelsPerPage + 1)
                                .arg(qMin((index + 1) * kModelsPerPage, presetsToShow.size()));
                row.submenu = true;
                row.action = Action::OpenMenu;
                row.value = baseLevel + QStringLiteral(":page:") + QString::number(index);
                rows.append(row);
            }
            return;
        }
        if (pageCount == 0) return;
        if (page < 0) page = 0;
        page = qMin(page, pageCount - 1);
        const int first = page * kModelsPerPage;
        const int last = qMin(first + kModelsPerPage, presetsToShow.size());
        for (int index = first; index < last; ++index) {
            const native::AnimeJanaiPreset& preset = presetsToShow.at(index);
            Row row;
            row.id = QStringLiteral("aji.") + preset.id;
            row.label = preset.label;
            row.tooltip = preset.supported ? preset.reason
                                           : PlayerController::tr("Unavailable: %1").arg(preset.reason);
            row.action = Action::SetEnhancement;
            row.value = QStringLiteral("animejanai|") + preset.id;
            row.selected = preset.id.compare(currentId, Qt::CaseInsensitive) == 0;
            row.enabled = preset.supported;
            rows.append(row);
        }
    };

    if (levelId == QLatin1String(kUpscaleProfiles) ||
        levelId.startsWith(QLatin1String(kUpscaleProfiles) + QStringLiteral(":page:"))) {
        QVector<native::AnimeJanaiPreset> profiles;
        for (const native::AnimeJanaiPreset& preset : presets) {
            if (!preset.rife && !preset.id.startsWith(QLatin1String("spatial:"))) {
                profiles.append(preset);
            }
        }
        appendPaged(profiles, QString::fromLatin1(kUpscaleProfiles), levelId, ajiId);
        return rows;
    }

    if (levelId == QLatin1String(kUpscaleSpatial)) {
        QVector<native::AnimeJanaiPreset> pinned;
        QVector<native::AnimeJanaiPreset> custom;
        QStringList stems;
        for (const native::UpscalerModel& model : native::upscalerCatalog()) {
            stems << model.fileName.toLower();
            stems << QFileInfo(model.fileName).completeBaseName().toLower();
        }
        for (const native::AnimeJanaiPreset& preset : presets) {
            if (!preset.id.startsWith(QLatin1String("spatial:")) || preset.rife) continue;
            if (preset.id.startsWith(QLatin1String("spatial:custom:"))) {
                const QString stem =
                    preset.id.mid(QStringLiteral("spatial:custom:").size()).toLower();
                if (!stems.contains(stem)) custom.append(preset);
            } else {
                pinned.append(preset);
            }
        }
        appendPaged(pinned, QString::fromLatin1(kUpscaleSpatial), levelId, ajiId);
        if (!custom.isEmpty()) {
            addSubmenu(tr("Custom models"), QString::fromLatin1(kUpscaleSpatialCustom));
        }
        return rows;
    }

    if (levelId == QLatin1String(kUpscaleSpatialCustom) ||
        levelId.startsWith(QLatin1String(kUpscaleSpatialCustom) + QStringLiteral(":page:"))) {
        QVector<native::AnimeJanaiPreset> custom;
        QStringList stems;
        for (const native::UpscalerModel& model : native::upscalerCatalog()) {
            stems << model.fileName.toLower();
            stems << QFileInfo(model.fileName).completeBaseName().toLower();
        }
        for (const native::AnimeJanaiPreset& preset : presets) {
            if (preset.id.startsWith(QLatin1String("spatial:custom:")) && !preset.rife) {
                const QString stem =
                    preset.id.mid(QStringLiteral("spatial:custom:").size()).toLower();
                if (!stems.contains(stem)) custom.append(preset);
            }
        }
        appendPaged(custom, QString::fromLatin1(kUpscaleSpatialCustom), levelId, ajiId);
        return rows;
    }

    if (levelId == QLatin1String(kUpscaleOptional)) {
        for (const native::UpscalerModel& model : native::upscalerCatalog()) {
            const bool installed = !native::installedUpscalerPath(model).isEmpty();
            Row row;
            row.id = QStringLiteral("upscaler.") + model.fileName;
            row.label = installed ? tr("%1  · installed").arg(model.displayName)
                                  : tr("%1  · download…").arg(model.displayName);
            row.tooltip = installed ? tr("Stored under the app-owned model folder")
                                    : tr("Opens Settings › Upscalers to download it");
            row.enabled = true;
            row.action = Action::SetEnhancement;
            row.value = installed ? (QStringLiteral("upscaler|") + model.fileName)
                                  : (QStringLiteral("upscalersettings|") + model.fileName);
            row.selected = installed && ajiId == QStringLiteral("upscaler:") + model.fileName;
            rows.append(row);
        }
        Row settings;
        settings.id = QStringLiteral("upscaler.settings");
        settings.label = tr("Settings · manage upscalers");
        settings.action = Action::SetEnhancement;
        settings.value = QStringLiteral("upscalersettings|");
        rows.append(settings);
        return rows;
    }

    if (privateNativeRuntimeActive() && native::menu::ownsLevel(levelId)) {
        for (const native::menu::Row& source : native::menu::levelRows(levelId, nativeInputs_)) {
            Row row;
            row.id = source.id;
            row.label = source.label;
            row.tooltip = source.tooltip;
            row.enabled = !source.disabled;
            row.submenu = source.submenu;
            row.selected = source.current;
            if (source.action != native::menu::Action::None) {
                row.action = Action::SetEnhancement;
                row.value = QStringLiteral("native|") +
                            native::menu::actionName(source.action) + QLatin1Char('|') +
                            source.value;
            }
            rows.append(row);
        }
        return rows;
    }

    const QString anime4kLevel = levelId == QLatin1String(kRestoreRoot) ? QString() : levelId;
    for (const anime4k::MenuRow& source : anime4k::menuRows(anime4kLevel)) {
        appendAnime4KPresetRow(rows, source, anime4kId);
    }
    return rows;
}

// ── rebuilds ────────────────────────────────────────────────────────────────

void PlayerController::rebuildPictureMenu() {
    if (pictureMenu_ != nullptr) pictureMenu_->setRows(capturePictureRows());
    if (scaleMenu_ != nullptr) scaleMenu_->setRows(videoScaleModeRows());
}

void PlayerController::rebuildSubtitleMenus() {
    if (pictureSubtitlesMenu_ != nullptr) {
        QVector<Row> rows;
        Row disk;
        disk.id = QStringLiteral("subtitles.disk");
        disk.label = tr("From disk…");
        disk.icon = QStringLiteral("folder");
        disk.action = Action::LoadExternalTrack;
        disk.value = QStringLiteral("sub");
        rows.append(disk);
        Row online;
        online.id = QStringLiteral("subtitles.online");
        online.label = tr("OpenSubtitles…");
        online.tooltip = tr("Search subtitles by title and language");
        online.icon = QStringLiteral("captions");
        online.action = Action::OpenSubtitleSearch;
        rows.append(online);
        pictureSubtitlesMenu_->setRows(rows);
    }
    if (subtitlesMenu_ != nullptr) {
        QVector<Row> rows = trackRows(QStringLiteral("sub"));
        appendSubtitleDelayRows(rows);
        appendExternalTrackRow(rows, true);
        subtitlesMenu_->setRows(rows);
    }
}

void PlayerController::rebuildAudioMenu() {
    if (audioMenu_ == nullptr) return;
    QVector<Row> rows = trackRows(QStringLiteral("voice"));
    appendExternalTrackRow(rows, false);
    audioMenu_->setRows(rows);
}

void PlayerController::rebuildSubtitlesMenu() {
    if (subtitlesMenu_ == nullptr) return;
    QVector<Row> rows = trackRows(QStringLiteral("sub"));
    appendSubtitleDelayRows(rows);
    appendExternalTrackRow(rows, true);
    subtitlesMenu_->setRows(rows);
}

void PlayerController::rebuildEnhancementMenu() {
    if (enhancementMenu_ == nullptr) return;
    enhancementMenu_->setRows(enhancementRootRows());

    // Resolve the complete current catalogue on a real state change. Hover-time
    // submenu lookup below is then an O(1) cache read: it never scans models or
    // probes the filesystem while the pointer moves across a menu.
    QStringList pending;
    QSet<QString> visited;
    for (const Row& row : enhancementMenu_->rows()) {
        if (row.action == Action::OpenMenu && !row.value.isEmpty()) pending.append(row.value);
    }
    while (!pending.isEmpty()) {
        const QString level = pending.takeLast();
        if (visited.contains(level)) continue;
        visited.insert(level);
        PlayerMenuModel* model = ensureSubmenuModel(level);
        if (model == nullptr) continue;
        model->setRows(enhancementLevelRows(level));
        for (const Row& row : model->rows()) {
            if (row.action == Action::OpenMenu && !row.value.isEmpty()) pending.append(row.value);
        }
    }
    pruneObsoleteSubmenuModels();
}

void PlayerController::rebuildQualityMenu() {
    if (qualityMenu_ == nullptr) return;
    QVector<int> heights;
    heights.reserve(qualities_.size());
    for (const shell::QualityEntry& entry : qualities_) heights.append(entry.height);
    std::sort(heights.begin(), heights.end(), std::greater<int>());
    heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
    QVector<Row> rows;
    if (heights.size() > 1) {
        Row autoRow;
        autoRow.id = QStringLiteral("quality.auto");
        autoRow.label = tr("Auto");
        if (currentHeight_ > 0) {
            autoRow.detail = QStringLiteral("%1p").arg(currentHeight_);
        }
        autoRow.selected = autoQuality_;
        autoRow.action = Action::SelectQuality;
        autoRow.value = QStringLiteral("0");
        rows.append(autoRow);
    }
    for (int height : heights) {
        Row row;
        row.id = QStringLiteral("quality.") + QString::number(height);
        row.label = QStringLiteral("%1p").arg(height);
        row.selected = !autoQuality_ && (height == currentHeight_);
        row.action = Action::SelectQuality;
        row.value = QString::number(height);
        rows.append(row);
    }
    qualityMenu_->setRows(rows);
    rebuildSettingsMenu();
}

void PlayerController::rebuildSettingsMenu() {
    if (settingsMenu_ == nullptr) return;
    QVector<Row> rows;
    if (qualityMenu_ != nullptr) {
        for (const Row& row : qualityMenu_->rows()) rows.append(row);
    }

    Row scaleHeading;
    scaleHeading.id = QStringLiteral("settings.scale.heading");
    scaleHeading.label = tr("SCALING");
    scaleHeading.heading = true;
    rows.append(scaleHeading);
    for (const Row& row : videoScaleModeRows()) rows.append(row);

    Row defaultPlayer;
    defaultPlayer.id = QStringLiteral("settings.defaultPlayer");
    defaultPlayer.label = tr("Make mikai the default video player");
    defaultPlayer.action = Action::MakeDefaultVideoPlayer;
    rows.append(defaultPlayer);
    settingsMenu_->setRows(rows);
}

void PlayerController::rebuildSeriesMenu() {
    if (seriesMenu_ == nullptr) return;
    QVector<Row> rows;
    if (!directMedia_) {
        for (int index = 0; index < playlist_.size(); ++index) {
            const QVariantMap& item = playlist_.at(index);
            QString label = item.value(QStringLiteral("label")).toString().trimmed();
            const QString number = item.value(QStringLiteral("number")).toString().trimmed();
            if (label.isEmpty()) {
                label = number.isEmpty() ? tr("Episode %1").arg(index + 1)
                                         : tr("Episode %1").arg(number);
            }
            Row row;
            row.id = QStringLiteral("episode.") + QString::number(index);
            row.label = label;
            row.detail = number.isEmpty() || label == number ? QString() : number;
            row.selected = index == episodeIndex_;
            row.action = Action::SelectEpisode;
            row.value = QString::number(index);
            rows.append(row);
        }
    }
    seriesMenu_->setRows(rows);
}

void PlayerController::rebuildKeyHintsMenu() {
    if (keyHintsMenu_ == nullptr) return;
    keyHintsMenu_->setRows(keyHintRows());
}

void PlayerController::rebuildRemedyMenu() {
    if (remedyMenu_ == nullptr) return;
    remedyMenu_->setRows(remedyRows());
}

void PlayerController::rebuildSkipOffers() {
    if (skipOffers_ == nullptr) return;
    QVector<Row> rows;
    for (int index = 0; index < timelineMarks_.size(); ++index) {
        const QVariantMap mark = timelineMarks_.at(index).toMap();
        const double start = mark.value(QStringLiteral("start")).toDouble();
        const double end = mark.value(QStringLiteral("end")).toDouble();
        if (!(end > start) || position_ < start || position_ >= end) continue;
        const QString type = mark.value(QStringLiteral("type")).toString();
        const QString label = mark.value(QStringLiteral("label")).toString();
        Row row;
        row.id = QStringLiteral("skip.") + QString::number(index);
        row.label = label.isEmpty() ? tr("Skip segment") : tr("Skip %1").arg(label);
        row.detail = tr("Jump to %1").arg(formatTime(end));
        row.action = Action::SkipSegment;
        row.value = QString::number(index);
        rows.append(row);
    }
    // An unchanged offer set must not reset delegates under the pointer.
    const QVector<Row>& current = skipOffers_->rows();
    if (current.size() == rows.size()) {
        bool same = true;
        for (int i = 0; i < rows.size(); ++i) {
            if (current.at(i).id != rows.at(i).id || current.at(i).label != rows.at(i).label ||
                current.at(i).value != rows.at(i).value) {
                same = false;
                break;
            }
        }
        if (same) return;
    }
    skipOffers_->setRows(rows);
}

void PlayerController::rebuildTimelineMarks() {
    emit skipSegmentsChanged();
}

void PlayerController::publishTrackAvailability() {
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
    if (audioTrackCount_ == audio && subtitleTrackCount_ == subtitles) return;
    audioTrackCount_ = audio;
    subtitleTrackCount_ = subtitles;
    emit trackAvailabilityChanged();
}

void PlayerController::refreshTrackMenus() {
    rebuildAudioMenu();
    rebuildSubtitlesMenu();
}

PlayerMenuModel* PlayerController::ensureSubmenuModel(const QString& levelId) {
    if (levelId.isEmpty()) return nullptr;
    if (PlayerMenuModel* cached = submenuModels_.value(levelId, nullptr)) return cached;
    auto* model = new PlayerMenuModel(this);
    submenuModels_.insert(levelId, model);
    return model;
}

void PlayerController::pruneObsoleteSubmenuModels() {
    const QVector<native::AnimeJanaiPreset> presets = native::animeJanaiPresets();
    QVector<native::AnimeJanaiPreset> profiles;
    QVector<native::AnimeJanaiPreset> pinned;
    QVector<native::AnimeJanaiPreset> custom;
    QStringList stems;
    for (const native::UpscalerModel& model : native::upscalerCatalog()) {
        stems << model.fileName.toLower();
        stems << QFileInfo(model.fileName).completeBaseName().toLower();
    }
    for (const native::AnimeJanaiPreset& preset : presets) {
        if (!preset.id.startsWith(QLatin1String("spatial:")) || preset.rife) {
            if (!preset.rife) profiles.append(preset);
            continue;
        }
        if (preset.id.startsWith(QLatin1String("spatial:custom:"))) {
            const QString stem = preset.id.mid(QStringLiteral("spatial:custom:").size()).toLower();
            if (!stems.contains(stem)) custom.append(preset);
        } else {
            pinned.append(preset);
        }
    }
    const auto pageCount = [](qsizetype size) {
        return (size + kModelsPerPage - 1) / kModelsPerPage;
    };
    const auto pageIndex = [](const QString& levelId, const QString& base, int* index) {
        const QString prefix = base + QStringLiteral(":page:");
        if (!levelId.startsWith(prefix)) return false;
        bool ok = false;
        *index = levelId.mid(prefix.size()).toInt(&ok);
        return ok && *index >= 0;
    };
    QStringList stale;
    for (auto it = submenuModels_.begin(); it != submenuModels_.end(); ++it) {
        const QString& level = it.key();
        bool keep = true;
        if (isCatalogueRifeLevel(level)) {
            keep = rifeLevelHasPreset(level, presets);
        } else if (level == QLatin1String(kUpscaleProfiles)) {
            keep = !profiles.isEmpty();
        } else if (level.startsWith(QLatin1String(kUpscaleProfiles) + QStringLiteral(":page:"))) {
            int index = -1;
            keep = pageIndex(level, QString::fromLatin1(kUpscaleProfiles), &index) &&
                   index < pageCount(profiles.size()) && index >= 0;
        } else if (level == QLatin1String(kUpscaleSpatial)) {
            keep = !pinned.isEmpty();
        } else if (level.startsWith(QLatin1String(kUpscaleSpatial) + QStringLiteral(":page:"))) {
            int index = -1;
            keep = pageIndex(level, QString::fromLatin1(kUpscaleSpatial), &index) &&
                   index < pageCount(pinned.size()) && index >= 0;
        } else if (level == QLatin1String(kUpscaleSpatialCustom)) {
            keep = !custom.isEmpty();
        } else if (level.startsWith(QLatin1String(kUpscaleSpatialCustom) +
                                    QStringLiteral(":page:"))) {
            int index = -1;
            keep = pageIndex(level, QString::fromLatin1(kUpscaleSpatialCustom), &index) &&
                   index < pageCount(custom.size()) && index >= 0;
        } else if (level == QLatin1String(kUpscaleOptional)) {
            keep = !native::upscalerCatalog().isEmpty();
        }
        if (!keep) stale.append(level);
    }
    for (const QString& level : stale) {
        PlayerMenuModel* model = submenuModels_.take(level);
        delete model;
    }
}

// ── lookups and activation ──────────────────────────────────────────────────

PlayerMenuModel* PlayerController::menuModel(const QString& menuId) const {
    if (menuId == QLatin1String("picture")) return pictureMenu_;
    if (menuId == QLatin1String("picture.subtitles")) return pictureSubtitlesMenu_;
    if (menuId == QLatin1String("picture.scale")) {
        if (scaleMenu_ != nullptr) scaleMenu_->setRows(videoScaleModeRows());
        return scaleMenu_;
    }
    if (menuId == QLatin1String("audio")) return audioMenu_;
    if (menuId == QLatin1String("subtitles")) return subtitlesMenu_;
    if (menuId == QLatin1String("enhancements")) return enhancementMenu_;
    if (menuId == QLatin1String("quality")) return qualityMenu_;
    if (menuId == QLatin1String("settings")) return settingsMenu_;
    if (menuId == QLatin1String("series")) return seriesMenu_;
    if (menuId == QLatin1String("keys")) return keyHintsMenu_;
    if (menuId == QLatin1String("remedies")) return remedyMenu_;
    if (menuId == QLatin1String("subtitleResults")) return subtitleResults_;
    if (menuId == QLatin1String("skip")) return skipOffers_;
    return submenuModels_.value(menuId, nullptr);
}

PlayerMenuModel* PlayerController::submenuModel(const QString& menuId, int row) {
    PlayerMenuModel* parent = menuModel(menuId);
    if (parent == nullptr || row < 0 || row >= parent->rowCount()) return nullptr;
    const Row source = parent->row(row);
    if (source.action != Action::OpenMenu || source.value.isEmpty()) return nullptr;
    return menuModel(source.value);
}

void PlayerController::publishMenuAction(PlayerMenuModel::Action action, const QString& value,
                                         const QString& rowId) {
    Q_UNUSED(rowId);
    switch (action) {
        case Action::None:
        case Action::OpenMenu:
        case Action::CloseMenus:
        case Action::OpenShortcuts:
        case Action::OpenEnhancementSettings:
            return;
        case Action::TogglePlayback:
            togglePlayPause();
            return;
        case Action::SeekTo:
            seekTo(value.toDouble());
            return;
        case Action::SeekBy:
            seekBy(value.toDouble());
            return;
        case Action::SetVolume:
            setVolume(value.toDouble() / 100.0);
            return;
        case Action::ToggleMute:
            toggleMute();
            return;
        case Action::SetSpeed:
            setRate(value.toDouble());
            return;
        case Action::SetVideoScaleMode:
            setVideoScaleMode(value.toInt());
            return;
        case Action::SelectTrack:
            return;  // dispatched with its menu kind by activateMenuRow()
        case Action::LoadExternalTrack:
            requestExternalTrack(value == QLatin1String("sub"));
            return;
        case Action::SetSubtitleDelay:
            {
                const double target = value.toDouble();
                adjustSubtitleDelay(target - subtitleDelay_);
            }
            return;
        case Action::ResetSubtitleDelay:
            resetSubtitleDelay();
            return;
        case Action::SelectQuality:
            selectQuality(value.toInt());
            return;
        case Action::SelectEpisode:
            selectEpisode(value.toInt());
            return;
        case Action::PreviousEpisode:
            goToPreviousEpisode();
            return;
        case Action::NextEpisode:
            goToNextEpisode();
            return;
        case Action::SkipSegment:
            activateSkipOffer(value.toInt());
            return;
        case Action::CaptureFrame:
            copyCurrentFrame();
            return;
        case Action::OpenCompare:
            openCompare();
            return;
        case Action::CopyMediaLink:
            copyMediaLink();
            return;
        case Action::OpenContainingFolder:
            openContainingFolder();
            return;
        case Action::OpenMediaInformation:
            openMediaInformation();
            return;
        case Action::OpenClipDialog:
            openClipDialog();
            return;
        case Action::OpenSubtitleSearch:
            openSubtitleSearch();
            return;
        case Action::DownloadSubtitle:
            downloadSubtitle(value.toInt());
            return;
        case Action::TogglePiP:
            togglePip();
            return;
        case Action::ToggleFullscreen:
            toggleFullscreen();
            return;
        case Action::MakeDefaultVideoPlayer:
            makeDefaultVideoPlayer();
            return;
        case Action::PrepareProcessing:
            onProcessingPrepareRequested();
            return;
        case Action::OpenProcessingProviders:
            onProcessingProvidersRequested();
            return;
        case Action::TurnOffProcessing:
            onProcessingOriginalRequested();
            return;
        case Action::ExitPlayer:
            requestClose();
            return;
        case Action::SetEnhancement: {
            const int separator = value.indexOf(QLatin1Char('|'));
            const QString kind = separator < 0 ? value.left(0) : value.left(separator);
            const QString payload = separator < 0 ? QString() : value.mid(separator + 1);
            if (kind == QLatin1String("anime4k")) {
                applyAnime4KPreset(payload);
            } else if (kind == QLatin1String("animejanai")) {
                applyAnimeJanaiRequest(payload);
            } else if (kind == QLatin1String("ajibackend")) {
                applyAnimeJanaiBackend(payload);
            } else if (kind == QLatin1String("upscaler")) {
                applyAnimeJanaiRequest(QStringLiteral("upscaler:") + payload);
            } else if (kind == QLatin1String("upscalersettings")) {
                emit upscalersRequested();
            } else if (kind == QLatin1String("native")) {
                const native::RuntimeSelection& runtime = native::currentSelection();
                if (!runtime.loaded || runtime.kind != native::BundleKind::Native) return;
                const int second = payload.indexOf(QLatin1Char('|'));
                const native::menu::Action nativeAction = native::menu::actionFromName(
                    second < 0 ? payload : payload.left(second));
                if (nativeAction == native::menu::Action::None) return;
                onNativeActionRequested(nativeAction,
                                        second < 0 ? QString() : payload.mid(second + 1));
            }
            return;
        }
    }
}

void PlayerController::activateMenuRow(const QString& menuId, int row) {
    PlayerMenuModel* model = menuModel(menuId);
    if (model == nullptr) return;
    if (row < 0 || row >= model->rowCount()) return;
    const Row source = model->row(row);
    if (source.heading || !source.enabled) return;
    if (source.action == Action::OpenMenu) {
        if (!source.value.isEmpty()) emit menuOpenRequested(source.value);
        return;
    }
    if (source.action == Action::CloseMenus) {
        closeMenus();
        return;
    }
    if (source.action == Action::OpenShortcuts) {
        emit shortcutsSheetRequested();
        return;
    }
    if (source.action == Action::SelectTrack) {
        const QString kind = menuId == QLatin1String("audio") ? QStringLiteral("audio")
                                                              : QStringLiteral("sub");
        onTrackPicked(kind, source.id, source.value);
        return;
    }
    if (source.action == Action::OpenEnhancementSettings) {
        openEnhancementSettings();
        return;
    }
    if (source.action == Action::CaptureFrame || source.action == Action::OpenCompare) {
        if (session_ == nullptr || !session_->hasVideo()) {
            return;
        }
    }
    publishMenuAction(source.action, source.value, source.id);
}

void PlayerController::closeMenus() {
    emit menusCloseRequested();
}

// ── selection changes ───────────────────────────────────────────────────────

void PlayerController::onQualityPicked(int height) {
    if (closed_) return;
    for (const shell::QualityEntry& entry : qualities_) {
        if (entry.height != height) continue;
        const bool ok = openStream(entry.url, height);
        if (!ok) {
            ensureOpenFailureVisible();
            return;
        }
        flashStatus(tr("Switching to %1p…").arg(height));
        return;
    }
    flashStatus(tr("%1p is not available for this release").arg(height));
}

void PlayerController::onTrackPicked(const QString& kind, const QString& rowId,
                                     const QString& value) {
    if (closed_) return;
    if (value.startsWith(QLatin1String("media|"))) {
        const QStringList parts = value.split(QLatin1Char('|'));
        if (parts.size() != 5 || session_ == nullptr) return;
        const QString type = parts.at(1);
        bool idOk = false;
        bool coreOk = false;
        bool epochOk = false;
        const int64_t id = parts.at(2).toLongLong(&idOk);
        const uint64_t core = parts.at(3).toULongLong(&coreOk);
        const uint64_t epoch = parts.at(4).toULongLong(&epochOk);
        if (!idOk || !coreOk || !epochOk) return;
        session_->selectMediaTrack(
            type == QLatin1String("audio") ? MediaTrackType::Audio : MediaTrackType::Subtitle,
            id, core, epoch);
        return;
    }
    Q_UNUSED(kind);
    const QString embed = value.isEmpty() ? rowId.mid(QStringLiteral("track.").size()) : value;
    if (embed.isEmpty() || embed == currentEmbed_) return;
    QString label = embed;
    for (const VariantInfo& variant : variants_) {
        if (variant.embed != embed) continue;
        label = variant.label.isEmpty() ? trackRowText(variant) : variant.label;
        break;
    }
    if (host_ != nullptr) host_->requestVariant(embed, label);
}

}  // namespace quick
}  // namespace mplayer
