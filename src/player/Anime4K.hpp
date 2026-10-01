#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace mplayer {
namespace anime4k {

/// Every selectable preset: the ten built-ins, then the experiments as
/// single-purpose chains of their own.
///
/// The enum is a menu shorthand, never the persisted value: the id in
/// `ModeInfo` is what `player/anime4k` stores.
enum class Mode {
    Off,
    FastA,       // Mode A (Fast) - Restore_CNN_M + Upscale_CNN_x2_M + Upscale_CNN_x2_S
    FastB,       // Mode B (Fast) - Restore_CNN_Soft_M + Upscale_CNN_x2_M + Upscale_CNN_x2_S
    FastC,       // Mode C (Fast) - Upscale_Denoise_CNN_x2_M + Upscale_CNN_x2_S
    HqA,         // Mode A (HQ) - Restore_CNN_VL + Upscale_CNN_x2_VL + Upscale_CNN_x2_M
    HqB,         // Mode B (HQ) - Restore_CNN_Soft_VL + Upscale_CNN_x2_VL + Upscale_CNN_x2_M
    HqC,         // Mode C (HQ) - Upscale_Denoise_CNN_x2_VL + Upscale_CNN_x2_M
    HqAA,        // Mode A+A (HQ) - Perceptual quality restoration
    HqBB,        // Mode B+B (HQ) - Denoise + Deblur
    HqCA,        // Mode C+A (HQ) - Highest fidelity
    // Experiments: one fixed chain each, offered under Експериментальні.
    Exp3dGraphics,
    ExpOldFilm,
    ExpDeblurStrong,
    ExpNoiseBilateralMean,
    ExpNoiseBilateralMedian,
    ExpNoiseBilateralMode,
    ExpThinVeryFast,
    ExpThinFast,
    ExpThinHq,
    ExpTextureDtdX2,
    ExpTextureDogX2,
    ExpDarkenVeryFast,
    ExpDarkenFast,
    ExpDarkenHq
};

/// One preset: its persisted id, its menu label and hint, the category it hangs
/// under, and the ordered chain it applies. Metadata and chain live in this one
/// table, so a preset is added in one place.
struct ModeInfo {
    Mode mode;
    QString id;
    QString label;
    QString hint;
    /// The menu category this preset is a leaf of, by `CategoryInfo::id`.
    QString category;
    /// Shader file names in chain order, without the folder. Empty is Off.
    QStringList files;
};

/// One node of the enhancement menu above the presets. A `placeholder` node is
/// informational and can never be selected.
struct CategoryInfo {
    QString id;
    /// The category this one hangs under; empty for a top-level row.
    QString parent;
    QString label;
    QString tooltip;
    bool placeholder = false;
};

/// What kind of work a loose shader candidate does, used for its inventory
/// label and hint. Presets use `Kind::Preset`.
enum class Kind {
    Preset,
    Restore,
    Denoise,
    Deblur,
    Thin,
    Darken,
    Upscale,
    Downscale,
    Clamp,
    Graphics
};

QString kindLabel(Kind kind);

/// One catalog candidate: either a named player preset or a single .glsl file.
///
/// `id` is the persistence key. It is the file name for a single shader and the
/// preset's own id otherwise, never the enum ordinal: a preset added to the
/// table later must not be able to silently repoint a selection the user
/// already made.
struct Candidate {
    QString id;
    QString label;
    QString hint;
    Kind kind = Kind::Preset;
    /// Set only for a preset; a single shader resolves to itself.
    Mode mode = Mode::Off;
    /// Shader file names, in chain order, without the folder.
    QStringList files;
    /// The x2/x3/x4 factor the chain applies, 0 when it does not scale.
    int scale = 0;
};

/// The catalog inventory: presets from the code table plus each .glsl found in
/// the shaders folder. The player menu and Compare chooser use presets only;
/// with no folder, presets are still listed but their files cannot be resolved,
/// which `chainIsComplete` reports per candidate.
QVector<Candidate> allCandidates(const QString& shadersDir = QString());

/// The id of a mode, which is its persistence key. Empty for a mode the table
/// does not carry: a disabled chain has no preset, and writing an ordinal
/// instead is what would let a preset added later silently repoint an existing
/// selection.
QString modeId(Mode mode);

/// The mode a persisted id names, `Mode::Off` for anything the table does not
/// carry - which is how a stale setting is detected rather than silently
/// applied.
Mode modeForId(const QString& id);

/// The presets alone, in menu order.
QVector<ModeInfo> availableModes();

/// The Ukrainian label of a category, empty when the id names none.
QString categoryLabel(const QString& categoryId);

/// A chip label that stands on its own: a nested experiment is qualified with
/// its family ("Thin Lines · HQ"), a top-level preset is not.
QString displayLabel(Mode mode);

/// Whether `presetId` hangs anywhere under `categoryId`, so a menu can mark the
/// closed groups the current selection is hidden inside.
bool categoryContainsPreset(const QString& categoryId, const QString& presetId);

/// One row of the enhancement menu, which is what the shell paints.
struct MenuRow {
    enum class Role { Preset, Category, Placeholder };
    Role role = Role::Category;
    /// The preset id, or the category id for a Category/Placeholder row.
    QString id;
    QString label;
    QString tooltip;
    /// True when a Category row really has something under it, which is what a
    /// menu uses to decide whether to offer the drill-down at all.
    bool hasChildren = false;
};

/// The rows of menu level `parent`: its preset leaves first, then the
/// categories under it. Empty `parent` is the Покращення root. A category row
/// only drills down and a placeholder row is disabled, so neither selects.
QVector<MenuRow> menuRows(const QString& parent);

/// The chain behind a candidate id, as file names. Empty for an id nothing
/// resolves to, which is also how an unknown persisted selection is detected
/// rather than silently applied.
QStringList filesForId(const QString& id, const QString& shadersDir = QString());

/// A candidate by id, or a default-constructed one when nothing matches.
Candidate candidateForId(const QString& id, const QString& shadersDir = QString());

/// True when every file in a candidate is actually on disk. mpv's
/// `glsl-shaders` echoes the string back without validating it, so a chain
/// naming a deleted shader still reads back complete: this is the only check
/// that can tell a working chain from a plausible one.
bool chainIsComplete(const Candidate& candidate, const QString& shadersDir = QString());

/// Resolves a candidate's file names into an mpv-compatible `glsl-shaders`
/// string of absolute paths. Empty for Off, an unknown id, or a missing folder.
QString shaderStringForCandidate(const QString& id, const QString& shadersDir = QString());

/// Finds the directory containing Anime4K .glsl files.
QString findShadersDir();

}  // namespace anime4k
}  // namespace mplayer
