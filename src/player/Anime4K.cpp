#include "Anime4K.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace mplayer {
namespace anime4k {

namespace {

QStringList shaderFilesForMode(Mode mode) {
    switch (mode) {
        case Mode::Off:
            return {};
        case Mode::FastA:
            return {
                QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
                QStringLiteral("Anime4K_Restore_CNN_M.glsl"),
                QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
                QStringLiteral("Anime4K_Upscale_CNN_x2_S.glsl")
            };
        case Mode::FastB:
            return {
                QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
                QStringLiteral("Anime4K_Restore_CNN_Soft_M.glsl"),
                QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
                QStringLiteral("Anime4K_Upscale_CNN_x2_S.glsl")
            };
        case Mode::FastC:
            return {
                QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
                QStringLiteral("Anime4K_Upscale_Denoise_CNN_x2_M.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
                QStringLiteral("Anime4K_Upscale_CNN_x2_S.glsl")
            };
        case Mode::HqA:
            return {
                QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
                QStringLiteral("Anime4K_Restore_CNN_VL.glsl"),
                QStringLiteral("Anime4K_Upscale_CNN_x2_VL.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
                QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")
            };
        case Mode::HqB:
            return {
                QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
                QStringLiteral("Anime4K_Restore_CNN_Soft_VL.glsl"),
                QStringLiteral("Anime4K_Upscale_CNN_x2_VL.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
                QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")
            };
        case Mode::HqC:
            return {
                QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
                QStringLiteral("Anime4K_Upscale_Denoise_CNN_x2_VL.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
                QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")
            };
        case Mode::HqAA:
            return {
                QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
                QStringLiteral("Anime4K_Restore_CNN_VL.glsl"),
                QStringLiteral("Anime4K_Upscale_CNN_x2_VL.glsl"),
                QStringLiteral("Anime4K_Restore_CNN_M.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
                QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")
            };
        case Mode::HqBB:
            return {
                QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
                QStringLiteral("Anime4K_Restore_CNN_Soft_VL.glsl"),
                QStringLiteral("Anime4K_Upscale_CNN_x2_VL.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
                QStringLiteral("Anime4K_Restore_CNN_Soft_M.glsl"),
                QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")
            };
        case Mode::HqCA:
            return {
                QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
                QStringLiteral("Anime4K_Upscale_Denoise_CNN_x2_VL.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
                QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
                QStringLiteral("Anime4K_Restore_CNN_M.glsl"),
                QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")
            };
    }
    return {};
}

}  // namespace

QVector<ModeInfo> availableModes() {
    return {
        {Mode::Off, QStringLiteral("off"), QStringLiteral("Вимкнено"), QStringLiteral("Оригінальне зображення")},
        {Mode::FastA, QStringLiteral("fast_a"), QStringLiteral("A (Швидкий)"), QStringLiteral("Відновлення ліній + апскейл (1080p/SD)")},
        {Mode::FastB, QStringLiteral("fast_b"), QStringLiteral("B (Швидкий)"), QStringLiteral("Згладжування артефактів + апскейл (720p)")},
        {Mode::FastC, QStringLiteral("fast_c"), QStringLiteral("C (Швидкий)"), QStringLiteral("Denoise + апскейл для якісного відео")},
        {Mode::HqA, QStringLiteral("hq_a"), QStringLiteral("A (Якісний)"), QStringLiteral("Глибоке відновлення ліній (CNN VL)")},
        {Mode::HqB, QStringLiteral("hq_b"), QStringLiteral("B (Якісний)"), QStringLiteral("М'яке відновлення та згладжування")},
        {Mode::HqC, QStringLiteral("hq_c"), QStringLiteral("C (Якісний)"), QStringLiteral("Високий PSNR + очищення шумів")},
        {Mode::HqAA, QStringLiteral("hq_aa"), QStringLiteral("A+A (Максимум)"), QStringLiteral("Подвійне відновлення для розмитого відео")},
        {Mode::HqBB, QStringLiteral("hq_bb"), QStringLiteral("B+B (Максимум)"), QStringLiteral("Подвійне м'яке відновлення")},
        {Mode::HqCA, QStringLiteral("hq_ca"), QStringLiteral("C+A (Максимум)"), QStringLiteral("Denoise + максимальне відновлення деталей")}
    };
}

QString findShadersDir() {
    QStringList candidates;
    if (QCoreApplication::instance()) {
        candidates << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("shaders"));
    }
    candidates << QDir::current().filePath(QStringLiteral("shaders"));
    candidates << QDir::current().filePath(QStringLiteral("build-cmake/shaders"));

    for (const QString& path : candidates) {
        QDir dir(path);
        if (dir.exists() && dir.exists(QStringLiteral("Anime4K_Clamp_Highlights.glsl"))) {
            return dir.canonicalPath();
        }
    }
    return QString();
}

QString shaderStringForMode(Mode mode, const QString& shadersDir) {
    if (mode == Mode::Off) {
        return QString();
    }
    const QString dir = shadersDir.isEmpty() ? findShadersDir() : shadersDir;
    if (dir.isEmpty()) {
        return QString();
    }
    const QStringList files = shaderFilesForMode(mode);
    QStringList paths;
    paths.reserve(files.size());
    for (const QString& file : files) {
        paths << QDir(dir).filePath(file);
    }
#ifdef Q_OS_WIN
    return paths.join(QLatin1Char(';'));
#else
    return paths.join(QLatin1Char(':'));
#endif
}

}  // namespace anime4k
}  // namespace mplayer
