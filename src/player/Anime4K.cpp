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

// ── the candidate catalogue ──────────────────────────────────────────────

QString kindLabel(Kind kind) {
    switch (kind) {
        case Kind::Preset: return QStringLiteral("Пресети");
        case Kind::Restore: return QStringLiteral("Відновлення");
        case Kind::Denoise: return QStringLiteral("Шумоподавлення");
        case Kind::Deblur: return QStringLiteral("Розмиття");
        case Kind::Thin: return QStringLiteral("Тонкі лінії");
        case Kind::Darken: return QStringLiteral("Затемнення");
        case Kind::Upscale: return QStringLiteral("Апскейл");
        case Kind::Downscale: return QStringLiteral("Даунскейл");
        case Kind::Clamp: return QStringLiteral("Обмеження");
        case Kind::Graphics: return QStringLiteral("3D-графіка");
    }
    return QString();
}

namespace {

/// The x2/x3/x4 a shader name states, 0 when it does not scale. Read from the
/// file name rather than a table so a shader added later needs no entry here.
int scaleFromName(const QString& file) {
    if (file.contains(QLatin1String("_x4_"))) {
        return 4;
    }
    if (file.contains(QLatin1String("_x3_"))) {
        return 3;
    }
    if (file.contains(QLatin1String("_x2_"))) {
        return 2;
    }
    return 0;
}

Kind kindFromName(const QString& file) {
    // Order matters only in that Restore must not be caught by a looser test;
    // every prefix Anime4K uses is distinct once the family is named.
    if (file.contains(QLatin1String("Restore"))) {
        return Kind::Restore;
    }
    if (file.contains(QLatin1String("Denoise"))) {
        return Kind::Denoise;
    }
    if (file.contains(QLatin1String("Deblur"))) {
        return Kind::Deblur;
    }
    if (file.contains(QLatin1String("Thin"))) {
        return Kind::Thin;
    }
    if (file.contains(QLatin1String("Darken"))) {
        return Kind::Darken;
    }
    if (file.contains(QLatin1String("AutoDownscalePre"))) {
        return Kind::Downscale;
    }
    if (file.contains(QLatin1String("Upscale"))) {
        return Kind::Upscale;
    }
    if (file.contains(QLatin1String("Clamp"))) {
        return Kind::Clamp;
    }
    if (file.contains(QLatin1String("3DGraphics"))) {
        return Kind::Graphics;
    }
    return Kind::Preset;
}

/// A readable name for one shader file: the Anime4K_ prefix and the .glsl
/// suffix carry nothing a viewer needs.
QString labelFromFile(const QString& file) {
    QString name = file;
    if (name.startsWith(QLatin1String("Anime4K_"))) {
        name = name.mid(8);
    }
    if (name.endsWith(QLatin1String(".glsl"))) {
        name.chop(5);
    }
    if (name.endsWith(QLatin1String("_US"))) {
        name.chop(3);
    }
    return name.replace(QLatin1Char('_'), QLatin1Char(' ')).trimmed();
}

/// A one-line description of what the shader does, from its family and size.
QString hintFromFile(const QString& file, Kind kind) {
    const QString label = labelFromFile(file);
    const int scale = scaleFromName(file);
    QString hint = kindLabel(kind);
    if (scale > 0) {
        hint += QStringLiteral(" ×%1").arg(scale);
    }
    // The size suffix is the part a viewer cannot read off the family name,
    // so it is worth spelling out where there is one.
    for (const QString& size : {QStringLiteral("VL"), QStringLiteral("UUL"),
                                 QStringLiteral("UL"), QStringLiteral("L"),
                                 QStringLiteral("M"), QStringLiteral("S")}) {
        if (label.endsWith(QLatin1Char(' ') + size) || label.endsWith(size)) {
            hint += QStringLiteral(" · розмір %1").arg(size);
            break;
        }
    }
    return hint;
}

}  // namespace
QString modeId(Mode mode) {
    for (const ModeInfo& info : availableModes()) {
        if (info.mode == mode) {
            return info.id;
        }
    }
    return QString();
}
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

QVector<Candidate> allCandidates(const QString& shadersDir) {
    QVector<Candidate> candidates;
    // Presets first: they are the chains most viewers actually want, and the
    // compare window lists them above the individual files without a heading.
    for (const ModeInfo& info : availableModes()) {
        Candidate candidate;
        candidate.id = info.id;
        candidate.label = info.label;
        candidate.hint = info.hint;
        candidate.kind = Kind::Preset;
        candidate.mode = info.mode;
        candidate.files = shaderFilesForMode(info.mode);
        int scale = 0;
        for (const QString& file : candidate.files) {
            scale = std::max(scale, scaleFromName(file));
        }
        candidate.scale = scale;
        candidates.append(candidate);
    }
    const QString dir = shadersDir.isEmpty() ? findShadersDir() : shadersDir;
    if (dir.isEmpty()) {
        return candidates;
    }
    QStringList files = QDir(dir).entryList({QStringLiteral("*.glsl")}, QDir::Files, QDir::Name);
    // The presets are already listed as chains. Re-listing Clamp_Highlights on
    // its own is still useful - it is a filter a viewer may want alone - but the
    // AutoDownscale steps are plumbing inside every preset and are noise as
    // standalone entries.
    for (const QString& file : std::as_const(files)) {
        if (file.contains(QLatin1String("AutoDownscalePre"))) {
            continue;
        }
        const Kind kind = kindFromName(file);
        Candidate candidate;
        candidate.id = file;
        candidate.label = labelFromFile(file);
        candidate.hint = hintFromFile(file, kind);
        candidate.kind = kind;
        candidate.files = {file};
        candidate.scale = scaleFromName(file);
        candidates.append(candidate);
    }
    return candidates;
}

QStringList filesForId(const QString& id, const QString& shadersDir) {
    if (id.isEmpty()) {
        return {};
    }
    if (id == QLatin1String("off")) {
        return {};
    }
    for (const Candidate& candidate : allCandidates(shadersDir)) {
        if (candidate.id == id) {
            return candidate.files;
        }
    }
    return {};
}

Candidate candidateForId(const QString& id, const QString& shadersDir) {
    for (const Candidate& candidate : allCandidates(shadersDir)) {
        if (candidate.id == id) {
            return candidate;
        }
    }
    return Candidate();
}

bool chainIsComplete(const Candidate& candidate, const QString& shadersDir) {
    if (candidate.files.isEmpty()) {
        return candidate.id == QLatin1String("off");
    }
    const QString dir = shadersDir.isEmpty() ? findShadersDir() : shadersDir;
    if (dir.isEmpty()) {
        return false;
    }
    for (const QString& file : candidate.files) {
        if (!QFileInfo::exists(QDir(dir).filePath(file))) {
            return false;
        }
    }
    return true;
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

QString shaderStringForCandidate(const QString& id, const QString& shadersDir) {
    const QString dir = shadersDir.isEmpty() ? findShadersDir() : shadersDir;
    if (dir.isEmpty()) {
        return QString();
    }
    const QStringList files = filesForId(id, dir);
    if (files.isEmpty()) {
        return QString();
    }
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
