#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace mplayer {
namespace anime4k {

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
    HqCA         // Mode C+A (HQ) - Highest fidelity
};

struct ModeInfo {
    Mode mode;
    QString id;
    QString label;
    QString hint;
};

/// What kind of work a candidate does, which is how the compare window groups
/// the 49 individual shaders. A preset belongs to the group of the first step
/// that does the real work, so a reader scanning the list finds the restore
/// models together rather than scattered by file name.
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

/// One selectable thing: either a named preset (the nine above) or a single
/// .glsl file.
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

/// Every selectable candidate: the nine presets plus each .glsl found in the
/// shaders folder, so a chain can name a shader that has no preset at all.
///
/// Returns empty when there is no shaders folder, which is the only thing that
/// makes the comparison impossible - a missing file inside a folder that does
/// exist is reported per candidate by `resolve`.
QVector<Candidate> allCandidates(const QString& shadersDir = QString());

/// The id of a mode, which is its persistence key. Empty for Off: a disabled
/// chain has no preset, and writing an ordinal instead is what would let a
/// preset added later silently repoint an existing selection.
QString modeId(Mode mode);

/// The presets alone, in menu order.
QVector<ModeInfo> availableModes();

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

/// Resolves shader file names for the given mode into an mpv-compatible
/// `glsl-shaders` string with absolute paths using the shaders folder beside
/// the executable or in the current working directory.
QString shaderStringForMode(Mode mode, const QString& shadersDir = QString());

/// Finds the directory containing Anime4K .glsl files.
QString findShadersDir();

}  // namespace anime4k
}  // namespace mplayer
