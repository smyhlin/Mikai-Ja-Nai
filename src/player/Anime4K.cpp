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
        {Mode::Off, QStringLiteral("off"), QCoreApplication::translate("Anime4K", "Disabled"),
         QCoreApplication::translate("Anime4K", "Original image"), QStringLiteral("anime4k"), {}},
        {Mode::FastA, QStringLiteral("fast_a"),
         QCoreApplication::translate("Anime4K", "A (Fast)"),
         QCoreApplication::translate("Anime4K", "Line restoration + upscale (1080p/SD)"),
         QStringLiteral("anime4k/speed"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_M.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_S.glsl")}},
        {Mode::FastB, QStringLiteral("fast_b"),
         QCoreApplication::translate("Anime4K", "B (Fast)"),
         QCoreApplication::translate("Anime4K", "Artifact smoothing + upscale (720p)"),
         QStringLiteral("anime4k/speed"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_Soft_M.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_S.glsl")}},
        {Mode::FastC, QStringLiteral("fast_c"),
         QCoreApplication::translate("Anime4K", "C (Fast)"),
         QCoreApplication::translate("Anime4K", "Denoise + upscale for high-quality video"),
         QStringLiteral("anime4k/speed"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Upscale_Denoise_CNN_x2_M.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_S.glsl")}},
        {Mode::HqA, QStringLiteral("hq_a"),
         QCoreApplication::translate("Anime4K", "A (High Quality)"),
         QCoreApplication::translate("Anime4K", "Deep line restoration (CNN VL)"),
         QStringLiteral("anime4k/quality"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_VL.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_VL.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")}},
        {Mode::HqB, QStringLiteral("hq_b"),
         QCoreApplication::translate("Anime4K", "B (High Quality)"),
         QCoreApplication::translate("Anime4K", "Soft restoration and smoothing"),
         QStringLiteral("anime4k/quality"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_Soft_VL.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_VL.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")}},
        {Mode::HqC, QStringLiteral("hq_c"),
         QCoreApplication::translate("Anime4K", "C (High Quality)"),
         QCoreApplication::translate("Anime4K", "High PSNR + noise cleanup"),
         QStringLiteral("anime4k/quality"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Upscale_Denoise_CNN_x2_VL.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")}},
        {Mode::HqAA, QStringLiteral("hq_aa"),
         QCoreApplication::translate("Anime4K", "A+A (Maximum)"),
         QCoreApplication::translate("Anime4K", "Double restoration for blurry video"),
         QStringLiteral("anime4k/maximum"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_VL.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_VL.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_M.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")}},
        {Mode::HqBB, QStringLiteral("hq_bb"),
         QCoreApplication::translate("Anime4K", "B+B (Maximum)"),
         QCoreApplication::translate("Anime4K", "Double soft restoration"),
         QStringLiteral("anime4k/maximum"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_Soft_VL.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_VL.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x2.glsl"),
          QStringLiteral("Anime4K_AutoDownscalePre_x4.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_Soft_M.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")}},
        {Mode::HqCA, QStringLiteral("hq_ca"),
         QCoreApplication::translate("Anime4K", "C+A (Maximum)"),
         QCoreApplication::translate("Anime4K", "Denoise + maximum detail restoration"),
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
         QCoreApplication::translate("Anime4K", "3D Graphics"),
         QCoreApplication::translate("Anime4K", "Anti-aliasing for 3D graphics - experimental"),
         QStringLiteral("anime4k/experimental"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_3DGraphics_AA_Upscale_x2_US.glsl")}},
        {Mode::ExpOldFilm, QStringLiteral("exp_old_film"),
         QCoreApplication::translate("Anime4K", "Old Film Restore"),
         QCoreApplication::translate("Anime4K", "Old film restoration - experimental"),
         QStringLiteral("anime4k/experimental"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Deblur_Original.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_M.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")}},
        {Mode::ExpDeblurStrong, QStringLiteral("exp_deblur_strong"),
         QCoreApplication::translate("Anime4K", "Deblur"),
         QCoreApplication::translate("Anime4K", "Strong deblurring (Strong Deblur) - experimental"),
         QStringLiteral("anime4k/experimental"),
         {QStringLiteral("Anime4K_Clamp_Highlights.glsl"),
          QStringLiteral("Anime4K_Deblur_DoG.glsl"),
          QStringLiteral("Anime4K_Restore_CNN_M.glsl"),
          QStringLiteral("Anime4K_Upscale_CNN_x2_M.glsl")}},
        {Mode::ExpNoiseBilateralMean, QStringLiteral("exp_noise_bilateral_mean"),
         QCoreApplication::translate("Anime4K", "Bilateral Mean"),
         QCoreApplication::translate("Anime4K", "Complete denoising preset - experimental"),
         QStringLiteral("anime4k/experimental/noise"),
         {QStringLiteral("Anime4K_Denoise_Bilateral_Mean.glsl")}},
        {Mode::ExpNoiseBilateralMedian, QStringLiteral("exp_noise_bilateral_median"),
         QCoreApplication::translate("Anime4K", "Bilateral Median"),
         QCoreApplication::translate("Anime4K", "Complete denoising preset - experimental"),
         QStringLiteral("anime4k/experimental/noise"),
         {QStringLiteral("Anime4K_Denoise_Bilateral_Median.glsl")}},
        {Mode::ExpNoiseBilateralMode, QStringLiteral("exp_noise_bilateral_mode"),
         QCoreApplication::translate("Anime4K", "Bilateral Mode"),
         QCoreApplication::translate("Anime4K", "Complete denoising preset - experimental"),
         QStringLiteral("anime4k/experimental/noise"),
         {QStringLiteral("Anime4K_Denoise_Bilateral_Mode.glsl")}},
        {Mode::ExpThinVeryFast, QStringLiteral("exp_thin_veryfast"),
         QCoreApplication::translate("Anime4K", "VeryFast"),
         QCoreApplication::translate("Anime4K", "Complete preset for thin lines - experimental"),
         QStringLiteral("anime4k/experimental/thin"),
         {QStringLiteral("Anime4K_Thin_VeryFast.glsl")}},
        {Mode::ExpThinFast, QStringLiteral("exp_thin_fast"),
         QCoreApplication::translate("Anime4K", "Fast"),
         QCoreApplication::translate("Anime4K", "Complete preset for thin lines - experimental"),
         QStringLiteral("anime4k/experimental/thin"),
         {QStringLiteral("Anime4K_Thin_Fast.glsl")}},
        {Mode::ExpThinHq, QStringLiteral("exp_thin_hq"),
         QCoreApplication::translate("Anime4K", "HQ"),
         QCoreApplication::translate("Anime4K", "Complete preset for thin lines - experimental"),
         QStringLiteral("anime4k/experimental/thin"),
         {QStringLiteral("Anime4K_Thin_HQ.glsl")}},
        {Mode::ExpTextureDtdX2, QStringLiteral("exp_texture_dtd_x2"),
         QCoreApplication::translate("Anime4K", "DTD x2"),
         QCoreApplication::translate("Anime4K", "Complete preset for texture - experimental"),
         QStringLiteral("anime4k/experimental/texture"),
         {QStringLiteral("Anime4K_Upscale_DTD_x2.glsl")}},
        {Mode::ExpTextureDogX2, QStringLiteral("exp_texture_dog_x2"),
         QCoreApplication::translate("Anime4K", "DoG x2"),
         QCoreApplication::translate("Anime4K", "Complete preset for texture - experimental"),
         QStringLiteral("anime4k/experimental/texture"),
         {QStringLiteral("Anime4K_Upscale_DoG_x2.glsl")}},
        {Mode::ExpDarkenVeryFast, QStringLiteral("exp_darken_veryfast"),
         QCoreApplication::translate("Anime4K", "VeryFast"),
         QCoreApplication::translate("Anime4K",
                                     "Complete preset for line darkening - experimental"),
         QStringLiteral("anime4k/experimental/darken"),
         {QStringLiteral("Anime4K_Darken_VeryFast.glsl")}},
        {Mode::ExpDarkenFast, QStringLiteral("exp_darken_fast"),
         QCoreApplication::translate("Anime4K", "Fast"),
         QCoreApplication::translate("Anime4K",
                                     "Complete preset for line darkening - experimental"),
         QStringLiteral("anime4k/experimental/darken"),
         {QStringLiteral("Anime4K_Darken_Fast.glsl")}},
        {Mode::ExpDarkenHq, QStringLiteral("exp_darken_hq"),
         QCoreApplication::translate("Anime4K", "HQ"),
         QCoreApplication::translate("Anime4K",
                                     "Complete preset for line darkening - experimental"),
         QStringLiteral("anime4k/experimental/darken"),
         {QStringLiteral("Anime4K_Darken_HQ.glsl")}}
    };
    return kModes;
}

/// Only implemented Anime4K categories are navigable; unsupported model
/// families do not get inert placeholder pages.
const QVector<CategoryInfo>& categories() {
    static const QVector<CategoryInfo> kCategories = {
        {QStringLiteral("anime4k"), QString(), QStringLiteral("Anime4K"),
         QCoreApplication::translate("Anime4K", "Anime4K presets: upscaling and image restoration")},
        {QStringLiteral("anime4k/speed"), QStringLiteral("anime4k"),
         QCoreApplication::translate("Anime4K", "Speed"),
         QCoreApplication::translate("Anime4K", "Low GPU load, real time")},
        {QStringLiteral("anime4k/quality"), QStringLiteral("anime4k"),
         QCoreApplication::translate("Anime4K", "Quality"),
         QCoreApplication::translate("Anime4K", "Balanced high quality")},
        {QStringLiteral("anime4k/maximum"), QStringLiteral("anime4k"),
         QCoreApplication::translate("Anime4K", "Maximum"),
         QCoreApplication::translate("Anime4K", "Multi-stage enhancement, higher GPU cost")},
        {QStringLiteral("anime4k/experimental"), QStringLiteral("anime4k"),
         QCoreApplication::translate("Anime4K", "Experimental"),
         QCoreApplication::translate("Anime4K", "Special cases, not typical settings")},
        {QStringLiteral("anime4k/experimental/noise"),
         QStringLiteral("anime4k/experimental"),
         QCoreApplication::translate("Anime4K", "Noise Reduction"),
         QCoreApplication::translate("Anime4K",
                                     "Noise reduction: one shader - a complete preset. "
                                     "Experimental, not a typical setting")},
        {QStringLiteral("anime4k/experimental/thin"), QStringLiteral("anime4k/experimental"),
         QCoreApplication::translate("Anime4K", "Thin Lines"),
         QCoreApplication::translate("Anime4K", "Thin lines: one shader - a complete preset. "
                                                 "Experimental, not a typical setting")},
        {QStringLiteral("anime4k/experimental/texture"), QStringLiteral("anime4k/experimental"),
         QCoreApplication::translate("Anime4K", "Texture Enhancement"),
         QCoreApplication::translate("Anime4K", "Textures: one shader - a complete preset. "
                                                 "Experimental, not a typical setting")},
        {QStringLiteral("anime4k/experimental/darken"), QStringLiteral("anime4k/experimental"),
         QCoreApplication::translate("Anime4K", "Darken"),
         QCoreApplication::translate("Anime4K", "Line darkening: one shader - a complete preset. "
                                                 "Experimental, not a typical setting")}
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
    // The size suffix is the part a viewer cannot read off the family name, so
    // it is worth spelling out where there is one. Each shape the hint can take
    // is one message rather than a run of glued pieces, so a translation is
    // free to put the scale and the size in whatever order reads in its
    // language.
    QString size;
    for (const QString& candidate : {QStringLiteral("VL"), QStringLiteral("UUL"),
                                     QStringLiteral("UL"), QStringLiteral("L"),
                                     QStringLiteral("M"), QStringLiteral("S")}) {
        if (label.endsWith(QLatin1Char(' ') + candidate) || label.endsWith(candidate)) {
            size = candidate;
            break;
        }
    }
    const QString family = kindLabel(kind);
    if (scale > 0 && !size.isEmpty()) {
        return QCoreApplication::translate("Anime4K", "%1 ×%2 · size %3")
            .arg(family, QString::number(scale), size);
    }
    if (scale > 0) {
        return QCoreApplication::translate("Anime4K", "%1 ×%2").arg(family, QString::number(scale));
    }
    if (!size.isEmpty()) {
        return QCoreApplication::translate("Anime4K", "%1 · size %2").arg(family, size);
    }
    return family;
}

}  // namespace

// ── the candidate catalogue ──────────────────────────────────────────────

QString kindLabel(Kind kind) {
    switch (kind) {
        case Kind::Preset: return QCoreApplication::translate("Anime4K", "Presets");
        case Kind::Restore: return QCoreApplication::translate("Anime4K", "Restoration");
        case Kind::Denoise: return QCoreApplication::translate("Anime4K", "Noise reduction");
        case Kind::Deblur: return QCoreApplication::translate("Anime4K", "Blur");
        case Kind::Thin: return QCoreApplication::translate("Anime4K", "Thin lines");
        case Kind::Darken: return QCoreApplication::translate("Anime4K", "Darkening");
        case Kind::Upscale: return QCoreApplication::translate("Anime4K", "Upscaling");
        case Kind::Downscale: return QCoreApplication::translate("Anime4K", "Downscaling");
        case Kind::Clamp: return QCoreApplication::translate("Anime4K", "Clamping");
        case Kind::Graphics: return QCoreApplication::translate("Anime4K", "3D graphics");
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
    // group already says what it is ("A (Fast)", "3D Graphics"), so the four
    // main categories add nothing and are left off.
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
    // this level: "Disabled" leads the Anime4K menu, and the three direct
    // experiments lead "Experimental", exactly as the menu is meant to read.
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
        row.role = MenuRow::Role::Category;
        row.id = category.id;
        row.label = category.label;
        row.tooltip = category.tooltip;
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
        const QDir appDir(QCoreApplication::applicationDirPath());
        candidates << appDir.filePath(QStringLiteral("shaders"));
        candidates << appDir.filePath(QStringLiteral("../shaders"));
        candidates << appDir.filePath(QStringLiteral("../../shaders"));
    }
    candidates << QDir::current().filePath(QStringLiteral("shaders"));
    candidates << QDir::current().filePath(QStringLiteral("build/release/shaders"));
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
