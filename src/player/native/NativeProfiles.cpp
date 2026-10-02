#include "NativeProfiles.hpp"

#include <QtCore/QCoreApplication>

namespace mplayer {
namespace native {
namespace {

/// The one lookup with this file's context. Every literal it is handed must be
/// wrapped in QT_TRANSLATE_NOOP: that macro is what lupdate reads, and at run
/// time it is the literal itself.
QString translated(const char* message) {
  return QCoreApplication::translate("NativeProfiles", message);
}

// Byte-verified acquisitions, from `native/spatial-acquisition.md`. A window
// names its model by digest; the catalogue's own id is used in the resolved
// recipe, so no built-in alias is invented here.
constexpr const char* kHdBalancedSha =
    "b308e93c3dda3c4f9f968295b2b7b8d1c0583a711bca53a08d51134d9d1cbd5c";
constexpr const char* kHdPerformanceSha =
    "7b804d87320f37b7269d6e27796046fbd975e95e2aa1f65d5557f52a36282716";
constexpr const char* kSdCompactSha =
    "ded46591ddfaeb22b3daaf84ad7b36838335aa3efca66acfa36177ce4ab21eb5";

constexpr const char* kHdBalancedName = "2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16";
constexpr const char* kHdPerformanceName =
    "2x_AnimeJaNai_HD_V3.1_Performance_SPANF3_b5f48_unshuffle_fp16";
constexpr const char* kSdCompactName =
    "2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo";

// The two pixel quantities the reference windows are stated in. The comparison
// is the product of the source's positive dimensions, never pixels times frames.
constexpr qint64 kSdPixels = 1280LL * 720LL;
constexpr qint64 kHdPixels = 1920LL * 1080LL;

ProfileWindow window(qint64 minPixels, qint64 maxPixels, int minFps, int maxFps,
                     const char* sha, const char* name) {
  ProfileWindow result;
  result.minPixels = minPixels;
  result.maxPixels = maxPixels;
  result.minFps = minFps;
  result.maxFps = maxFps;
  result.modelSha256 = QString::fromLatin1(sha);
  result.modelName = QString::fromLatin1(name);
  return result;
}

/// Exact rational comparison by cross-multiplication of positive integers.
/// `num/den` in [minFps, maxFps] iff `minFps*den <= num <= maxFps*den`. Both
/// sides are widened to 64 bits, and the widest product here (61 * INT_MAX) is
/// far inside that, so a boundary is decided exactly and never by rounding.
bool rateInWindow(const Rational& rate, int minFps, int maxFps) {
  const qint64 num = rate.num;
  const qint64 den = rate.den;
  return num >= static_cast<qint64>(minFps) * den && num <= static_cast<qint64>(maxFps) * den;
}

/// The row `catalog` vouches for that carries this exact spatial model. Matching
/// is by digest and role, which is the identity the acquisition pinned; the
/// record's own id - content-addressed by the catalogue - is what a recipe names.
const ModelRecord* findSpatialModel(const CatalogView& catalog, const QString& sha256) {
  for (const ModelRecord& record : catalog.models) {
    if (record.role != ModelRole::Spatial) continue;
    if (record.onnxSha256.compare(sha256, Qt::CaseInsensitive) != 0) continue;
    return &record;
  }
  return nullptr;
}

}  // namespace

QVector<ProfileTemplate> profileTemplates() {
  ProfileTemplate quality;
  quality.id = QStringLiteral("janai.profile.quality.v1");
  quality.label = translated(QT_TRANSLATE_NOOP("NativeProfiles", "Quality"));
  quality.windows = {
      window(kSdPixels, kHdPixels, 0, 31, kHdBalancedSha, kHdBalancedName),
      window(0, kSdPixels, 0, 31, kSdCompactSha, kSdCompactName),
      window(0, kHdPixels, 0, 61, kHdBalancedSha, kHdBalancedName),
  };

  ProfileTemplate balanced;
  balanced.id = QStringLiteral("janai.profile.balanced.v1");
  balanced.label = translated(QT_TRANSLATE_NOOP("NativeProfiles", "Balanced"));
  balanced.windows = {
      window(kSdPixels, kHdPixels, 0, 31, kHdBalancedSha, kHdBalancedName),
      window(0, kSdPixels, 0, 31, kSdCompactSha, kSdCompactName),
  };

  ProfileTemplate performance;
  performance.id = QStringLiteral("janai.profile.performance.v1");
  performance.label = translated(QT_TRANSLATE_NOOP("NativeProfiles", "Performance"));
  performance.windows = {
      window(kSdPixels, kHdPixels, 0, 31, kHdPerformanceSha, kHdPerformanceName),
      window(0, kSdPixels, 0, 31, kSdCompactSha, kSdCompactName),
  };

  return {quality, balanced, performance};
}

QVector<NativeRecipeOption> builtinProfileOptions(const QString& reasonKey,
                                                  const QString& reason) {
  // Always Unavailable. A profile cannot be Ready without an engine, and the
  // availability is the caller's observed gap, not a claim made here.
  const AvailabilityState state = unavailableAvailability(reasonKey, reason);
  QVector<NativeRecipeOption> options;
  for (const ProfileTemplate& profile : profileTemplates()) {
    NativeRecipeOption option;
    option.id = profile.id;
    option.label = profile.label;
    option.availability = state;
    options.append(option);
  }
  return options;
}

std::optional<Recipe> resolveProfile(const QString& id, const QSize& sourceSize,
                                     const Rational& sourceRate, Backend backend,
                                     const CatalogView& catalog, QString* reasonKey,
                                     QString* reason) {
  if (reasonKey != nullptr) reasonKey->clear();
  if (reason != nullptr) reason->clear();

  const auto fail = [reasonKey, reason](const QString& key,
                                        const QString& text) -> std::optional<Recipe> {
    if (reasonKey != nullptr) *reasonKey = key;
    if (reason != nullptr) *reason = text;
    return std::nullopt;
  };

  const QVector<ProfileTemplate> profiles = profileTemplates();
  const ProfileTemplate* profile = nullptr;
  for (const ProfileTemplate& candidate : profiles) {
    if (candidate.id == id) {
      profile = &candidate;
      break;
    }
  }
  if (profile == nullptr) {
    return fail(QStringLiteral("native.profile.unknown"),
                translated(QT_TRANSLATE_NOOP(
                    "NativeProfiles",
                    "The requested profile “%1” is not one of this build's curated profiles."))
                    .arg(id));
  }

  // An unknown source is named as needing a source. Zero or negative geometry is
  // not knowledge of a small source, and an invalid rate is not zero fps.
  if (sourceSize.width() <= 0 || sourceSize.height() <= 0 || !sourceRate.isValid()) {
    return fail(QStringLiteral("native.profile.needsSource"),
                translated(QT_TRANSLATE_NOOP(
                    "NativeProfiles",
                    "The source's geometry and frame rate must both be known before the profile "
                    "“%1” can be resolved."))
                    .arg(profile->id));
  }

  const qint64 pixels = static_cast<qint64>(sourceSize.width()) *
                        static_cast<qint64>(sourceSize.height());

  const ProfileWindow* matched = nullptr;
  for (const ProfileWindow& candidate : profile->windows) {
    if (pixels < candidate.minPixels || pixels > candidate.maxPixels) continue;
    if (!rateInWindow(sourceRate, candidate.minFps, candidate.maxFps)) continue;
    matched = &candidate;
    break;
  }
  if (matched == nullptr) {
    return fail(QStringLiteral("native.profile.sourceUnsupported"),
                translated(QT_TRANSLATE_NOOP(
                    "NativeProfiles",
                    "No “%1” window covers a %2x%3 source at %4 fps."))
                    .arg(profile->id)
                    .arg(sourceSize.width())
                    .arg(sourceSize.height())
                    .arg(sourceRate.toString()));
  }

  // The window is already chosen. A model this build does not have is refused by
  // name rather than replaced with a later window's model.
  const ModelRecord* record = findSpatialModel(catalog, matched->modelSha256);
  if (record == nullptr) {
    return fail(
        QStringLiteral("native.profile.modelMissing"),
        translated(QT_TRANSLATE_NOOP(
            "NativeProfiles",
            "The profile “%1” needs the %2 model, which the catalogue does not have; no other "
            "window's model is substituted."))
            .arg(profile->id, matched->modelName));
  }
  // The row must be identified by its own content. A content-addressed row whose
  // id does not derive from its digest and declared profile is an alias for
  // something it is not, and is refused rather than adopted.
  if (isUserModelId(record->id) && record->id != userModelIdFor(record->contentId())) {
    return fail(QStringLiteral("native.profile.identityMismatch"),
                translated(QT_TRANSLATE_NOOP(
                    "NativeProfiles",
                    "The catalogue row %1 for the %2 model does not derive its id from its own "
                    "content, so it is refused."))
                    .arg(record->id, matched->modelName));
  }
  if (!record->isOffered()) {
    QString modelKey;
    QString modelReason;
    record->refusal(&modelKey, &modelReason);
    return fail(QStringLiteral("native.profile.modelUnavailable"),
                translated(QT_TRANSLATE_NOOP(
                    "NativeProfiles",
                    "The profile “%1” needs the %2 model, which cannot be used: %3"))
                    .arg(profile->id, record->id, modelReason));
  }
  // A row can be well formed and still not usable now: the availability its
  // provisioner supplied decides. `isOffered` above covers the structural
  // refusals; this covers the honest Preparing/Unavailable row, which is refused
  // with its own stated reason and never replaced by another window's model.
  if (!record->availability.isUsable()) {
    return fail(QStringLiteral("native.profile.modelUnavailable"),
                translated(QT_TRANSLATE_NOOP(
                    "NativeProfiles",
                    "The profile “%1” needs the %2 model, which cannot be used: %3"))
                    .arg(profile->id, record->id, record->availability.reason));
  }
  if (!record->supportsBackend(backend)) {
    return fail(QStringLiteral("native.profile.backendUnsupported"),
                translated(QT_TRANSLATE_NOOP(
                    "NativeProfiles",
                    "The %1 model does not offer the %2 backend."))
                    .arg(record->id, stableName(backend)));
  }

  Recipe recipe;
  recipe.id = profile->id;
  recipe.label = profile->label;
  SpatialStage stage;
  stage.modelId = record->id;
  recipe.spatial = {stage};
  recipe.backend = backend;

  const RecipeVerdict verdict = validateRecipe(recipe);
  if (!verdict.ok) {
    return fail(verdict.reasonKey, verdict.reason);
  }
  return recipe;
}

}  // namespace native
}  // namespace mplayer
