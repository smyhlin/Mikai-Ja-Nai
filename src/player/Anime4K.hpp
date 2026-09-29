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

QVector<ModeInfo> availableModes();

/// Resolves shader file names for the given mode into an mpv-compatible
/// `glsl-shaders` string with absolute paths using the shaders folder beside
/// the executable or in the current working directory.
QString shaderStringForMode(Mode mode, const QString& shadersDir = QString());

/// Finds the directory containing Anime4K .glsl files.
QString findShadersDir();

}  // namespace anime4k
}  // namespace mplayer
