#include "Anime4K.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace mplayer {
namespace anime4k {

namespace {

/// The catalogue. Order is menu order, and it is also the order the compare
/// window lists the presets in, so a new preset goes where it belongs in the
/// menu rather than at the end.
///
/// Every chain below is written out in the order mpv has to run it. Do not
/// merge, reorder or "tidy" one: a chain is an image pipeline, and the order is
/// the filter graph. `files` is the only copy of a chain - nothing else in the
/// tree holds a second one.
const QVector<ModeInfo>& catalogue() {
    static const QVector<ModeInfo> kModes = {
        {Mode::Off, QStringLiteral("off"), QStringLiteral("Вимкнено"),
         QStringLiteral("Оригінальне зображення"), QStringLiteral("anime4k"), {}},
        {Mode::FastA, QStringLiteral("fast_a"), QStringLiteral("A (Швидкий)"),
         QStringLiteral("Відновлення ліній + апскейл (1080p/SD)"),
         QStringLiteral("anime4k/speed"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_M.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_S.glsl")}},
        {Mode::FastB, QStringLiteral("fast_b"), QStringLiteral("B (Швидкий)"),
         QStringLiteral("Згладжування артефактів + апскейл (720p)"),
         QStringLiteral("anime4k/speed"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_Soft_M.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_S.glsl")}},
        {Mode::FastC, QStringLiteral("fast_c"), QStringLiteral("C (Швидкий)"),
         QStringLiteral("Denoise + апскейл для якісного відео"), QStringLiteral("anime4k/speed"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Upscale_Denoise_CNN_x2_M.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_S.glsl")}},
        {Mode::HqA, QStringLiteral("hq_a"), QStringLiteral("A (Якісний)"),
         QStringLiteral("Глибоке відновлення ліній (CNN VL)"), QStringLiteral("anime4k/quality"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_VL.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_VL.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")}},
        {Mode::HqB, QStringLiteral("hq_b"), QStringLiteral("B (Якісний)"),
         QStringLiteral("М'яке відновлення та згладжування"), QStringLiteral("anime4k/quality"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_Soft_VL.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_VL.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")}},
        {Mode::HqC, QStringLiteral("hq_c"), QStringLiteral("C (Якісний)"),
         QStringLiteral("Високий PSNR + очищення шумів"), QStringLiteral("anime4k/quality"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Upscale_Denoise_CNN_x2_VL.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")}},
        {Mode::HqAA, QStringLiteral("hq_aa"), QStringLiteral("A+A (Максимум)"),
         QStringLiteral("Подвійне відновлення для розмитого відео"),
         QStringLiteral("anime4k/maximum"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_VL.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_VL.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_M.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")}},
        {Mode::HqBB, QStringLiteral("hq_bb"), QStringLiteral("B+B (Максимум)"),
         QStringLiteral("Подвійне м'яке відновлення"), QStringLiteral("anime4k/maximum"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_Soft_VL.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_VL.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_Soft_M.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")}},
        {Mode::HqCA, QStringLiteral("hq_ca"), QStringLiteral("C+A (Максимум)"),
         QStringLiteral("Denoise + максимальне відновлення деталей"),
         QStringLiteral("anime4k/maximum"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Upscale_Denoise_CNN_x2_VL.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_M.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")}},
        // ── experiments ────────────────────────────────────────────────
        // Fixed chains. The single-file entries are complete presets on their
        // own, not toggleable effects: composing them is what the viewer would
        // have to do by hand, and guessing extra stages would be inventing a
        // pipeline Anime4K does not document.
        {Mode::Exp3dGraphics, QStringLiteral("exp_3d_graphics"),
         QStringLiteral("3D Graphics"),
         QStringLiteral("Антиаліасинг для 3D-графіки — експериментально"),
         QStringLiteral("anime4k/experimental"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_3DGraphics_AA_Upscale_x2_US.glsl")}},
        {Mode::ExpOldFilm, QStringLiteral("exp_old_film"),
         QStringLiteral("Old Film Restore"),
         QStringLiteral("Відновлення старого фільму — експериментально"),
         QStringLiteral("anime4k/experimental"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Deblur_Original.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_M.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")}},
        {Mode::ExpDeblurStrong, QStringLiteral("exp_deblur_strong"),
         QStringLiteral("Deblur"),
         QStringLiteral("Сильне знеблюрення (Strong Deblur) — експериментально"),
         QStringLiteral("anime4k/experimental"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Deblur_DoG.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_M.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")}},
        {Mode::ExpNoiseBilateralMean, QStringLiteral("exp_noise_bilateral_mean"),
         QStringLiteral("Bilateral Mean"),
         QStringLiteral("Повний пресет шумоподавлення — експериментально"),
         QStringLiteral("anime4k/experimental/noise"),
         {QStringLiteral("Anime4K_Denoise_Bilateral_Mean.glsl")}},
        {Mode::ExpNoiseBilateralMedian, QStringLiteral("exp_noise_bilateral_median"),
         QStringLiteral("Bilateral Median"),
         QStringLiteral("Повний пресет шумоподавлення — експериментально"),
         QStringLiteral("anime4k/experimental/noise"),
         {QStringLiteral("Anime4K_Denoise_Bilateral_Median.glsl")}},
        {Mode::ExpNoiseBilateralMode, QStringLiteral("exp_noise_bilateral_mode"),
         QStringLiteral("Bilateral Mode"),
         QStringLiteral("Повний пресет шумоподавлення — експериментально"),
         QStringLiteral("anime4k/experimental/noise"),
         {QStringLiteral("Anime4K_Denoise_Bilateral_Mode.glsl")}},
        {Mode::ExpThinVeryFast, QStringLiteral("exp_thin_veryfast"), QStringLiteral("VeryFast"),
         QStringLiteral("Повний пресет для тонких ліній — експериментально"),
         QStringLiteral("anime4k/experimental/thin"),
         {QStringLiteral("Anime4K_Thin_VeryFast.glsl")}},
        {Mode::ExpThinFast, QStringLiteral("exp_thin_fast"), QStringLiteral("Fast"),
         QStringLiteral("Повний пресет для тонких ліній — експериментально"),
         QStringLiteral("anime4k/experimental/thin"),
         {QStringLiteral("Anime4K_Thin_Fast.glsl")}},
        {Mode::ExpThinHq, QStringLiteral("exp_thin_hq"), QStringLiteral("HQ"),
         QStringLiteral("Повний пресет для тонких ліній — експериментально"),
         QStringLiteral("anime4k/experimental/thin"),
         {QStringLiteral("Anime4K_Thin_HQ.glsl")}},
        {Mode::ExpTextureDtdX2, QStringLiteral("exp_texture_dtd_x2"), QStringLiteral("DTD x2"),
         QStringLiteral("Повний пресет для текстури — експериментально"),
         QStringLiteral("anime4k/experimental/texture"),
         {QStringLiteral("Anime4K_Upscale_DTD_x2.glsl")}},
        {Mode::ExpTextureDogX2, QStringLiteral("exp_texture_dog_x2"), QStringLiteral("DoG x2"),
         QStringLiteral("Повний пресет для текстури — експериментально"),
         QStringLiteral("anime4k/experimental/texture"),
         {QStringLiteral("Anime4K_Upscale_DoG_x2.glsl")}},
        {Mode::ExpDarkenVeryFast, QStringLiteral("exp_darken_veryfast"),
         QStringLiteral("VeryFast"),
         QStringLiteral("Повний пресет затемнення ліній — експериментально"),
         QStringLiteral("anime4k/experimental/darken"),
         {QStringLiteral("Anime4K_Darken_VeryFast.glsl")}},
        {Mode::ExpDarkenFast, QStringLiteral("exp_darken_fast"), QStringLiteral("Fast"),
         QStringLiteral("Повний пресет затемнення ліній — експериментально"),
         QStringLiteral("anime4k/experimental/darken"),
         {QStringLiteral("Anime4K_Darken_Fast.glsl")}},
        {Mode::ExpDarkenHq, QStringLiteral("exp_darken_hq"), QStringLiteral("HQ"),
         QStringLiteral("Повний пресет затемнення ліній — експериментально"),
         QStringLiteral("anime4k/experimental/darken"),
         {QStringLiteral("Anime4K_Darken_HQ.glsl")}}
    };
    return kModes;
}

/// The menu above the presets, parents before children. The placeholder entries
/// are the models this player does not run: they are here so the menu says so
/// instead of staying silent about them.
const QVector<CategoryInfo>& categories() {
    static const QVector<CategoryInfo> kCategories = {
        {QStringLiteral("anime4k"), QString(), QStringLiteral("Anime4K"),
         QStringLiteral("Пресети Anime4K: апскейл і відновлення зображення"), false},
        {QStringLiteral("anime4k/speed"), QStringLiteral("anime4k"), QStringLiteral("Швидкість"),
         QStringLiteral("Низьке навантаження на GPU, реальний час"), false},
        {QStringLiteral("anime4k/quality"), QStringLiteral("anime4k"), QStringLiteral("Якість"),
         QStringLiteral("Збалансована висока якість"), false},
        {QStringLiteral("anime4k/maximum"), QStringLiteral("anime4k"), QStringLiteral("Максимум"),
         QStringLiteral("Багатоетапне покращення, вища ціна GPU"), false},
        {QStringLiteral("anime4k/experimental"), QStringLiteral("anime4k"),
         QStringLiteral("Експериментальні"),
         QStringLiteral("Спеціальні випадки, не типові налаштування"), false},
        // The four experiment families are named the way Anime4K names them, so
        // a viewer looking for a documented preset finds it under the same name;
        // the warning that they are not defaults is in the tooltip.
        {QStringLiteral("anime4k/experimental/noise"),
         QStringLiteral("anime4k/experimental"), QStringLiteral("Noise Reduction"),
         QStringLiteral("Шумоподавлення: один шейдер — повний пресет. "
                        "Експериментально, не типове налаштування"), false},
        {QStringLiteral("anime4k/experimental/thin"), QStringLiteral("anime4k/experimental"),
         QStringLiteral("Thin Lines"),
         QStringLiteral("Тонкі лінії: один шейдер — повний пресет. "
                        "Експериментально, не типове налаштування"), false},
        {QStringLiteral("anime4k/experimental/texture"), QStringLiteral("anime4k/experimental"),
         QStringLiteral("Texture Enhancement"),
         QStringLiteral("Текстури: один шейдер — повний пресет. "
                        "Експериментально, не типове налаштування"), false},
        {QStringLiteral("anime4k/experimental/darken"), QStringLiteral("anime4k/experimental"),
         QStringLiteral("Darken"),
         QStringLiteral("Затемнення ліній: один шейдер — повний пресет. "
                        "Експериментально, не типове налаштування"), false},
        // The models this player does not run each get their own page rather
        // than a dead row, so the menu can say where they will live: the page
        // holds a single disabled В планах leaf and the usual way back.
        {QStringLiteral("rife"), QString(), QStringLiteral("RIFE"),
         QStringLiteral("Інтерполяція кадрів: двигун не реалізовано"), false},
        {QStringLiteral("realesrgan"), QString(), QStringLiteral("RealESRGAN"),
         QStringLiteral("Нейромережевий апскейл: двигун не реалізовано"), false},
        {QStringLiteral("other-models"), QString(), QStringLiteral("Інші моделі"),
         QStringLiteral("Інші моделі покращення: двигун не реалізовано"), false},
        {QStringLiteral("rife/planned"), QStringLiteral("rife"), QStringLiteral("В планах"),
         QStringLiteral("У планах: RIFE ще не реалізовано"), true},
        {QStringLiteral("realesrgan/planned"), QStringLiteral("realesrgan"),
         QStringLiteral("В планах"), QStringLiteral("У планах: RealESRGAN ще не реалізовано"),
         true},
        {QStringLiteral("other-models/planned"), QStringLiteral("other-models"),
         QStringLiteral("В планах"), QStringLiteral("У планах: ці моделі ще не реалізовано"),
         true}
    };
    return kCategories;
}

const ModeInfo* infoForMode(Mode mode) {
    for (const ModeInfo& info : catalogue()) {
        if (info.mode == mode) {
            return &info;
        }
    }
    return nullptr;
}

const CategoryInfo* categoryForId(const QString& id) {
    for (const CategoryInfo& category : categories()) {
        if (category.id == id) {
            return &category;
        }
    }
    return nullptr;
}

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
    // Order matters where a family token appears inside another name: the
    // 3D-graphics shaders carry "Upscale" too (Anime4K_3DGraphics_AA_Upscale_*),
    // and Restore must not be caught by a looser test. Otherwise every prefix
    // Anime4K uses is distinct once the family is named.
    if (file.contains(QLatin1String("3DGraphics"))) {
        return Kind::Graphics;
    }
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

QString modeId(Mode mode) {
    const ModeInfo* info = infoForMode(mode);
    return info != nullptr ? info->id : QString();
}

Mode modeForId(const QString& id) {
    for (const ModeInfo& info : catalogue()) {
        if (info.id == id) {
            return info.mode;
        }
    }
    return Mode::Off;
}

QVector<ModeInfo> availableModes() {
    // Returned by value, as before: callers range over a temporary. The copy is
    // a refcount bump on the same QVector, not a rebuild of the table.
    return catalogue();
}

QString categoryLabel(const QString& categoryId) {
    const CategoryInfo* category = categoryForId(categoryId);
    return category != nullptr ? category->label : QString();
}

QString displayLabel(Mode mode) {
    const ModeInfo* info = infoForMode(mode);
    if (info == nullptr) {
        return QString();
    }
    // A leaf deeper than one category reads as a bare "HQ" or "Fast" on its
    // own, so it is qualified with the group it sits in. A leaf of a top-level
    // group already says what it is ("A (Швидкий)", "3D Graphics"), so the
    // four main categories add nothing and are left off.
    const CategoryInfo* parent = categoryForId(info->category);
    const bool nested = parent != nullptr && !parent->parent.isEmpty() &&
                        parent->parent != QLatin1String("anime4k");
    return nested ? parent->label + QStringLiteral(" · ") + info->label : info->label;
}

bool categoryContainsPreset(const QString& categoryId, const QString& presetId) {
    const ModeInfo* info = nullptr;
    for (const ModeInfo& candidate : catalogue()) {
        if (candidate.id == presetId) {
            info = &candidate;
            break;
        }
    }
    if (info == nullptr) {
        return false;
    }
    // A preset sits in its own category or in one nested below it, which is how
    // a closed group knows it holds the current selection.
    return info->category == categoryId ||
           info->category.startsWith(categoryId + QLatin1Char('/'));
}

QVector<MenuRow> menuRows(const QString& parent) {
    QVector<MenuRow> rows;
    // Presets first, in catalogue order, then the categories that hang under
    // this level: Вимкнено leads the Anime4K menu, and the three direct
    // experiments lead Експериментальні, exactly as the menu is meant to read.
    for (const ModeInfo& info : catalogue()) {
        if (info.category != parent) {
            continue;
        }
        MenuRow row;
        row.role = MenuRow::Role::Preset;
        row.id = info.id;
        row.label = info.label;
        row.tooltip = info.hint;
        rows.append(row);
    }
    for (const CategoryInfo& category : categories()) {
        if (category.parent != parent) {
            continue;
        }
        MenuRow row;
        row.role = category.placeholder ? MenuRow::Role::Placeholder : MenuRow::Role::Category;
        row.id = category.id;
        row.label = category.label;
        row.tooltip = category.tooltip;
        if (!category.placeholder) {
            for (const ModeInfo& info : catalogue()) {
                if (info.category == category.id) {
                    row.hasChildren = true;
                    break;
                }
            }
            if (!row.hasChildren) {
                for (const CategoryInfo& nested : categories()) {
                    if (nested.parent == category.id) {
                        row.hasChildren = true;
                        break;
                    }
                }
            }
        }
        rows.append(row);
    }
    return rows;
}

QVector<Candidate> allCandidates(const QString& shadersDir) {
    QVector<Candidate> candidates;
    // Presets first: they are the chains most viewers actually want, and the
    // compare window lists them above the individual files without a heading.
    for (const ModeInfo& info : catalogue()) {
        Candidate candidate;
        candidate.id = info.id;
        candidate.label = info.label;
        candidate.hint = info.hint;
        candidate.kind = Kind::Preset;
        candidate.mode = info.mode;
        candidate.files = info.files;
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
    // Every file has to exist before the chain is handed over. mpv echoes
    // glsl-shaders back without validating it, so a chain naming a file that
    // is not there reads back complete and fails silently at the next frame;
    // refusing to build it here is the only place that can catch it.
    const QStringList files = filesForId(id, dir);
    if (files.isEmpty()) {
        return QString();
    }
    for (const QString& file : files) {
        if (!QFileInfo::exists(QDir(dir).filePath(file))) {
            return QString();
        }
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

}  // namespace anime4k
}  // namespace mplayer
