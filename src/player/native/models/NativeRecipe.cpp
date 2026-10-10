#include "NativeRecipe.hpp"

#include <cmath>
#include <limits>

#include "native/models/NativeRife.hpp"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QStringList>
#include <QtCore/QStringView>

namespace mplayer {
namespace native {

namespace {

constexpr const char* kSchema = "mikai.native.recipe/1";

// The temporal scene threshold is a normalized difference sum: a value outside
// [0,1] would either never detect a cut or always take one, and NaN would compare
// false against everything and silently disable detection.
constexpr double kSceneThresholdMin = 0.0;
constexpr double kSceneThresholdMax = 1.0;

bool jsonIsInteger(const QJsonValue& value) {
  if (!value.isDouble()) return false;
  const double d = value.toDouble();
  if (!std::isfinite(d)) return false;
  if (std::floor(d) != d) return false;
  return d >= static_cast<double>(std::numeric_limits<int>::min()) &&
         d <= static_cast<double>(std::numeric_limits<int>::max());
}

bool jsonInt(const QJsonObject& object, const QString& key, int* out) {
  const QJsonValue value = object.value(key);
  if (!jsonIsInteger(value)) return false;
  *out = static_cast<int>(value.toDouble());
  return true;
}

QString jsonString(const QJsonObject& object, const QString& key) {
  return object.value(key).toString();
}

/// Rejects a member name this schema version does not define. A silently ignored
/// typo ("preRsize", "sceneThreshhold") leaves a stage configured as something
/// the document never said, which is worse than a refused document.
bool noUnknownKeys(const QJsonObject& object, const QStringList& known) {
  for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
    if (!known.contains(it.key())) return false;
  }
  return true;
}

QStringList spatialKeys() {
  return {QStringLiteral("model"), QStringLiteral("preResize")};
}

QStringList geometryKeys() {
  return {QStringLiteral("w"), QStringLiteral("h")};
}

QStringList temporalKeys() {
  return {QStringLiteral("model"),     QStringLiteral("version"), QStringLiteral("variant"),
          QStringLiteral("ensemble"),  QStringLiteral("factor"),  QStringLiteral("order"),
          QStringLiteral("sceneThreshold")};
}

QStringList factorKeys() {
  return {QStringLiteral("num"), QStringLiteral("den")};
}

QStringList recipeKeys() {
  return {QStringLiteral("schema"), QStringLiteral("id"),    QStringLiteral("label"),
          QStringLiteral("backend"), QStringLiteral("spatial"), QStringLiteral("temporal")};
}

}  // namespace

// ------------------------------------------------------------------- shape

RecipeShape Recipe::shape() const {
  if (!spatial.isEmpty() && temporal.has_value()) return RecipeShape::Combined;
  if (!spatial.isEmpty()) return RecipeShape::SpatialOnly;
  if (temporal.has_value()) return RecipeShape::RifeOnly;
  return RecipeShape::Empty;
}

QString Recipe::shapeName() const {
  switch (shape()) {
    case RecipeShape::SpatialOnly:
      return QStringLiteral("spatial-only");
    case RecipeShape::RifeOnly:
      return QStringLiteral("rife-only");
    case RecipeShape::Combined:
      return QStringLiteral("combined");
    case RecipeShape::Empty:
      return QStringLiteral("empty");
  }
  return QStringLiteral("empty");
}

// --------------------------------------------------------------- validation

RecipeVerdict RecipeVerdict::rejected(const QString& reasonKey, const QString& reason) {
  RecipeVerdict verdict;
  verdict.ok = false;
  verdict.reasonKey = reasonKey;
  verdict.reason = reason;
  return verdict;
}

RecipeVerdict validateSpatialStage(const SpatialStage& stage) {
  if (stage.modelId.isEmpty()) {
    return RecipeVerdict::rejected(QStringLiteral("stage.model.missing"),
                                   QStringLiteral("a spatial stage names no model"));
  }
  if (!isValidModelId(stage.modelId)) {
    return RecipeVerdict::rejected(
        QStringLiteral("stage.model.id.invalid"),
        QStringLiteral("spatial stage model id %1 is not a usable identifier")
            .arg(stage.modelId));
  }
  if (stage.preResizeEnabled) {
    if (stage.preResize.width() <= 0 || stage.preResize.height() <= 0) {
      return RecipeVerdict::rejected(
          QStringLiteral("stage.preresize.invalid"),
          QStringLiteral("spatial stage %1 requests a pre-resize of %2x%3; both sides must be "
                         "positive")
              .arg(stage.modelId)
              .arg(stage.preResize.width())
              .arg(stage.preResize.height()));
    }
  }
  return RecipeVerdict::accepted();
}

RecipeVerdict validateTemporalStage(const TemporalStage& stage) {
  if (!stage.factor.isValid()) {
    return RecipeVerdict::rejected(
        QStringLiteral("stage.temporal.factor.invalid"),
        QStringLiteral("temporal stage %1 requests a factor of %2/%3; both must be positive")
            .arg(stage.modelId())
            .arg(stage.factor.num)
            .arg(stage.factor.den));
  }
  if (!std::isfinite(stage.sceneThreshold) || stage.sceneThreshold < kSceneThresholdMin ||
      stage.sceneThreshold > kSceneThresholdMax) {
    return RecipeVerdict::rejected(
        QStringLiteral("stage.temporal.threshold.invalid"),
        QStringLiteral("temporal stage %1 requests a scene threshold of %2; it must be a finite "
                       "value in [0, 1]")
            .arg(stage.modelId())
            .arg(stage.sceneThreshold));
  }
  return RecipeVerdict::accepted();
}

RecipeVerdict validateRecipe(const Recipe& recipe) {
  if (recipe.id.isEmpty()) {
    return RecipeVerdict::rejected(QStringLiteral("recipe.id.missing"),
                                   QStringLiteral("the recipe has no id"));
  }
  if (!isValidModelId(recipe.id)) {
    return RecipeVerdict::rejected(
        QStringLiteral("recipe.id.invalid"),
        QStringLiteral("recipe id %1 is not a usable identifier").arg(recipe.id));
  }

  switch (recipe.shape()) {
    case RecipeShape::SpatialOnly:
    case RecipeShape::RifeOnly:
    case RecipeShape::Combined:
      break;
    case RecipeShape::Empty:
      return RecipeVerdict::rejected(
          QStringLiteral("recipe.empty"),
          QStringLiteral("recipe %1 resolves to zero spatial stages and no temporal stage, which "
                         "is not the same thing as a RIFE-only recipe and would reach the core as "
                         "a configure with nothing to do")
              .arg(recipe.id));
  }

  for (int i = 0; i < recipe.spatial.size(); ++i) {
    const RecipeVerdict verdict = validateSpatialStage(recipe.spatial.at(i));
    if (!verdict.ok) {
      return RecipeVerdict::rejected(verdict.reasonKey,
                                     QStringLiteral("spatial stage %1: %2")
                                         .arg(i)
                                         .arg(verdict.reason));
    }
  }

  if (recipe.temporal.has_value()) {
    RecipeVerdict verdict = validateTemporalStage(*recipe.temporal);
    if (!verdict.ok) return verdict;
  }

  return RecipeVerdict::accepted();
}

// -------------------------------------------------------------- eligibility

namespace {

/// One model reference resolved against the supplied catalogue. An absent row is
/// a refusal, never "unknown but probably fine". The backend is settled by the
/// caller before this runs.
bool resolveModel(const Recipe& recipe, const ModelRole requiredRole, const QString& modelId,
                  const CatalogView& catalog, QString* reasonKey, QString* reason) {
  const auto refuse = [reasonKey, reason](const QString& key, const QString& text) {
    *reasonKey = key;
    *reason = text;
    return false;
  };

  const ModelRecord* record = catalog.find(modelId);
  if (record == nullptr) {
    return refuse(QStringLiteral("model.missing"),
                  QStringLiteral("recipe %1 names model %2, which is not in the supplied catalogue")
                      .arg(recipe.id, modelId));
  }

  QString offerKey;
  QString offerReason;
  record->refusal(&offerKey, &offerReason);
  if (!offerKey.isEmpty()) {
    return refuse(offerKey, QStringLiteral("recipe %1: %2").arg(recipe.id, offerReason));
  }

  // A row that is well formed and eligible is still not a licence to run: the
  // availability its provisioner supplied decides, and a row that is not usable
  // now is refused with the fact it states. `refusal` above already reports a
  // non-Ready row that states no reason as `model.reason.missing`, so this only
  // adds the row that is honest about being Preparing or Unavailable.
  if (!record->availability.isUsable()) {
    return refuse(record->availability.reasonKey,
                  QStringLiteral("recipe %1: model %2 is %3: %4")
                      .arg(recipe.id, modelId, stableName(record->availability.value),
                           record->availability.reason));
  }

  if (record->role != requiredRole) {
    return refuse(QStringLiteral("model.role.mismatch"),
                  QStringLiteral("recipe %1 names model %2 as a %3 model, but it is a %4 model")
                      .arg(recipe.id, modelId, stableName(requiredRole), stableName(record->role)));
  }

  if (!record->supportsBackend(recipe.backend)) {
    return refuse(QStringLiteral("model.backend.unsupported"),
                  QStringLiteral("recipe %1 asks for model %2 on the %3 backend, which that model "
                                 "does not support")
                      .arg(recipe.id, modelId, stableName(recipe.backend)));
  }
  return true;
}

}  // namespace

RecipeEligibility evaluateEligibility(const Recipe& recipe, const CatalogView& catalog,
                                      const BackendAvailability& backends) {
  RecipeEligibility result;
  result.requested = SelectionState{recipe.id, recipe.backend};

  const auto refuse = [&result](const QString& reasonKey, const QString& reason) {
    result.availability = unavailableAvailability(reasonKey, reason);
    // Effective stays the untouched Original pipeline: a refused recipe is never
    // partially applied and is never swapped for a different one.
    result.effective = SelectionState{};
  };

  const RecipeVerdict shape = validateRecipe(recipe);
  if (!shape.ok) {
    refuse(shape.reasonKey, shape.reason);
    return result;
  }

  if (!backends.has(recipe.backend)) {
    refuse(QStringLiteral("backend.unavailable"),
           QStringLiteral("recipe %1 asks for the %2 backend, which this runtime does not carry; "
                          "no other backend is substituted")
               .arg(recipe.id, stableName(recipe.backend)));
    return result;
  }

  for (int i = 0; i < recipe.spatial.size(); ++i) {
    QString reasonKey;
    QString reason;
    if (!resolveModel(recipe, ModelRole::Spatial, recipe.spatial.at(i).modelId, catalog,
                      &reasonKey, &reason)) {
      result.stageReasons.append(reason);
      refuse(reasonKey, reason);
      return result;
    }
  }

  if (recipe.temporal.has_value()) {
    // A temporal stage names a curated variant, not a catalogue id. The exact
    // pinned owned graph is what is evaluated; a curated-name lookup would match
    // no owned row and a row that merely claims the name is never accepted.
    const RifeModelResolution rife = resolveRifeModel(*recipe.temporal, catalog);
    if (!rife.resolved()) {
      const QString reason =
          QStringLiteral("recipe %1: %2").arg(recipe.id, rife.reason);
      result.stageReasons.append(reason);
      refuse(rife.reasonKey, reason);
      return result;
    }
    if (!rife.record->supportsBackend(recipe.backend)) {
      const QString reasonKey = QStringLiteral("model.backend.unsupported");
      const QString reason =
          QStringLiteral("recipe %1 asks for model %2 on the %3 backend, which that model does "
                         "not support")
              .arg(recipe.id, recipe.temporal->modelId(), stableName(recipe.backend));
      result.stageReasons.append(reason);
      refuse(reasonKey, reason);
      return result;
    }
  }

  result.availability = readyAvailability();
  result.effective = result.requested;
  return result;
}

// -------------------------------------------------------------- enum wire names

QString stableName(ModelRole role) {
  switch (role) {
    case ModelRole::Spatial:
      return QStringLiteral("spatial");
    case ModelRole::Temporal:
      return QStringLiteral("temporal");
  }
  return QString();
}

QString stableName(Backend backend) {
  switch (backend) {
    case Backend::TensorRT:
      return QStringLiteral("tensorrt");
    case Backend::DirectML:
      return QStringLiteral("directml");
  }
  return QString();
}

QString stableName(Availability availability) {
  switch (availability) {
    case Availability::Ready:
      return QStringLiteral("ready");
    case Availability::Preparing:
      return QStringLiteral("preparing");
    case Availability::Unavailable:
      return QStringLiteral("unavailable");
  }
  return QString();
}

QString stableName(SpatialRelation relation) {
  switch (relation) {
    case SpatialRelation::Identity:
      return QStringLiteral("identity");
    case SpatialRelation::Upscale:
      return QStringLiteral("upscale");
    case SpatialRelation::Downscale:
      return QStringLiteral("downscale");
    case SpatialRelation::Reframe:
      return QStringLiteral("reframe");
  }
  return QString();
}

QString stableName(TemporalOrder order) {
  switch (order) {
    case TemporalOrder::BeforeSpatial:
      return QStringLiteral("beforeSpatial");
    case TemporalOrder::AfterSpatial:
      return QStringLiteral("afterSpatial");
  }
  return QString();
}

QString stableName(RifeVersion version) {
  switch (version) {
    case RifeVersion::V4_7:
      return QStringLiteral("4.7");
    case RifeVersion::V4_8:
      return QStringLiteral("4.8");
    case RifeVersion::V4_9:
      return QStringLiteral("4.9");
    case RifeVersion::V4_10:
      return QStringLiteral("4.10");
    case RifeVersion::V4_11:
      return QStringLiteral("4.11");
    case RifeVersion::V4_12:
      return QStringLiteral("4.12");
    case RifeVersion::V4_13:
      return QStringLiteral("4.13");
    case RifeVersion::V4_14:
      return QStringLiteral("4.14");
    case RifeVersion::V4_15:
      return QStringLiteral("4.15");
    case RifeVersion::V4_16:
      return QStringLiteral("4.16");
    case RifeVersion::V4_17:
      return QStringLiteral("4.17");
    case RifeVersion::V4_18:
      return QStringLiteral("4.18");
    case RifeVersion::V4_19:
      return QStringLiteral("4.19");
    case RifeVersion::V4_20:
      return QStringLiteral("4.20");
    case RifeVersion::V4_21:
      return QStringLiteral("4.21");
    case RifeVersion::V4_22:
      return QStringLiteral("4.22");
    case RifeVersion::V4_23:
      return QStringLiteral("4.23");
    case RifeVersion::V4_24:
      return QStringLiteral("4.24");
    case RifeVersion::V4_25:
      return QStringLiteral("4.25");
    case RifeVersion::V4_26:
      return QStringLiteral("4.26");
  }
  return QString();
}

QString stableName(RifeVariant variant) {
  switch (variant) {
    case RifeVariant::Full:
      return QStringLiteral("full");
    case RifeVariant::Lite:
      return QStringLiteral("lite");
    case RifeVariant::Heavy:
      return QStringLiteral("heavy");
  }
  return QString();
}

std::optional<ModelRole> modelRoleFromName(const QString& name) {
  if (name == QLatin1String("spatial")) return ModelRole::Spatial;
  if (name == QLatin1String("temporal")) return ModelRole::Temporal;
  return std::nullopt;
}

std::optional<Backend> backendFromName(const QString& name) {
  if (name == QLatin1String("tensorrt")) return Backend::TensorRT;
  if (name == QLatin1String("directml")) return Backend::DirectML;
  return std::nullopt;
}

std::optional<Availability> availabilityFromName(const QString& name) {
  if (name == QLatin1String("ready")) return Availability::Ready;
  if (name == QLatin1String("preparing")) return Availability::Preparing;
  if (name == QLatin1String("unavailable")) return Availability::Unavailable;
  return std::nullopt;
}

std::optional<SpatialRelation> spatialRelationFromName(const QString& name) {
  if (name == QLatin1String("identity")) return SpatialRelation::Identity;
  if (name == QLatin1String("upscale")) return SpatialRelation::Upscale;
  if (name == QLatin1String("downscale")) return SpatialRelation::Downscale;
  if (name == QLatin1String("reframe")) return SpatialRelation::Reframe;
  return std::nullopt;
}

std::optional<TemporalOrder> temporalOrderFromName(const QString& name) {
  if (name == QLatin1String("beforeSpatial")) return TemporalOrder::BeforeSpatial;
  if (name == QLatin1String("afterSpatial")) return TemporalOrder::AfterSpatial;
  return std::nullopt;
}

std::optional<RifeVersion> rifeVersionFromName(const QString& name) {
  if (name == QLatin1String("4.7")) return RifeVersion::V4_7;
  if (name == QLatin1String("4.8")) return RifeVersion::V4_8;
  if (name == QLatin1String("4.9")) return RifeVersion::V4_9;
  if (name == QLatin1String("4.10")) return RifeVersion::V4_10;
  if (name == QLatin1String("4.11")) return RifeVersion::V4_11;
  if (name == QLatin1String("4.12")) return RifeVersion::V4_12;
  if (name == QLatin1String("4.13")) return RifeVersion::V4_13;
  if (name == QLatin1String("4.14")) return RifeVersion::V4_14;
  if (name == QLatin1String("4.15")) return RifeVersion::V4_15;
  if (name == QLatin1String("4.16")) return RifeVersion::V4_16;
  if (name == QLatin1String("4.17")) return RifeVersion::V4_17;
  if (name == QLatin1String("4.18")) return RifeVersion::V4_18;
  if (name == QLatin1String("4.19")) return RifeVersion::V4_19;
  if (name == QLatin1String("4.20")) return RifeVersion::V4_20;
  if (name == QLatin1String("4.21")) return RifeVersion::V4_21;
  if (name == QLatin1String("4.22")) return RifeVersion::V4_22;
  if (name == QLatin1String("4.23")) return RifeVersion::V4_23;
  if (name == QLatin1String("4.24")) return RifeVersion::V4_24;
  if (name == QLatin1String("4.25")) return RifeVersion::V4_25;
  if (name == QLatin1String("4.26")) return RifeVersion::V4_26;
  return std::nullopt;
}

std::optional<RifeVariant> rifeVariantFromName(const QString& name) {
  if (name == QLatin1String("full")) return RifeVariant::Full;
  if (name == QLatin1String("lite")) return RifeVariant::Lite;
  if (name == QLatin1String("heavy")) return RifeVariant::Heavy;
  return std::nullopt;
}

QVector<Backend> allBackends() {
  return {Backend::TensorRT, Backend::DirectML};
}

QString rifeModelName(RifeVersion version, RifeVariant variant, bool ensemble) {
  const QString variantName =
      variant == RifeVariant::Lite ? QStringLiteral("_lite")
      : variant == RifeVariant::Heavy ? QStringLiteral("_heavy")
                                      : QString();
  return QStringLiteral("rife_v%1%2%3")
      .arg(stableName(version), variantName,
           ensemble ? QStringLiteral("_ensemble") : QString());
}

int rifeModelCode(RifeVersion version, RifeVariant variant) {
  int baseCode = 0;
  switch (version) {
    case RifeVersion::V4_7:
      baseCode = 47;
      break;
    case RifeVersion::V4_8:
      baseCode = 48;
      break;
    case RifeVersion::V4_9:
      baseCode = 49;
      break;
    case RifeVersion::V4_10:
      baseCode = 410;
      break;
    case RifeVersion::V4_11:
      baseCode = 411;
      break;
    case RifeVersion::V4_12:
      baseCode = 412;
      break;
    case RifeVersion::V4_13:
      baseCode = 413;
      break;
    case RifeVersion::V4_14:
      baseCode = 414;
      break;
    case RifeVersion::V4_15:
      baseCode = 415;
      break;
    case RifeVersion::V4_16:
      baseCode = 416;
      break;
    case RifeVersion::V4_17:
      baseCode = 417;
      break;
    case RifeVersion::V4_18:
      baseCode = 418;
      break;
    case RifeVersion::V4_19:
      baseCode = 419;
      break;
    case RifeVersion::V4_20:
      baseCode = 420;
      break;
    case RifeVersion::V4_21:
      baseCode = 421;
      break;
    case RifeVersion::V4_22:
      baseCode = 422;
      break;
    case RifeVersion::V4_23:
      baseCode = 423;
      break;
    case RifeVersion::V4_24:
      baseCode = 424;
      break;
    case RifeVersion::V4_25:
      baseCode = 425;
      break;
    case RifeVersion::V4_26:
      baseCode = 426;
      break;
    default:
      return 0;
  }

  switch (variant) {
    case RifeVariant::Full:
      return baseCode;
    case RifeVariant::Lite:
      return baseCode * 10 + 1;
    case RifeVariant::Heavy:
      return baseCode * 10 + 2;
  }
  return 0;
}

bool isEligibleIoType(const QString& ioType) {
  return ioType == QLatin1String("fp16");
}

Rational Rational::reduced() const {
  if (!isValid()) return *this;
  int a = num;
  int b = den;
  while (b != 0) {
    const int t = a % b;
    a = b;
    b = t;
  }
  if (a <= 0) return *this;
  return Rational{num / a, den / a};
}

bool Rational::isCanonical() const {
  return isValid() && reduced() == *this;
}

QString Rational::toString() const {
  return QStringLiteral("%1/%2").arg(num).arg(den);
}

bool AvailabilityState::hasMandatoryReason() const {
  if (value == Availability::Ready) return reasonKey.isEmpty() && reason.isEmpty();
  return !reasonKey.isEmpty() && !reason.isEmpty();
}

AvailabilityState readyAvailability() {
  return AvailabilityState{Availability::Ready, QString(), QString()};
}

AvailabilityState preparingAvailability(const QString& reasonKey, const QString& reason) {
  return AvailabilityState{Availability::Preparing, reasonKey, reason};
}

AvailabilityState unavailableAvailability(const QString& reasonKey, const QString& reason) {
  return AvailabilityState{Availability::Unavailable, reasonKey, reason};
}

bool isSameDeviceUuid(const QString& left, const QString& right) {
  // CUDA tools add GPU- to the same UUID the decoder reports without it.
  static constexpr QStringView kGpuPrefix = u"GPU-";
  const auto payload = [](QStringView value) {
    return value.size() > kGpuPrefix.size() &&
                   value.left(kGpuPrefix.size()).compare(kGpuPrefix, Qt::CaseInsensitive) == 0
               ? value.mid(kGpuPrefix.size())
               : value;
  };

  const QStringView a = payload(left);
  const QStringView b = payload(right);
  qsizetype ia = 0;
  qsizetype ib = 0;
  while (true) {
    while (ia < a.size() && !a.at(ia).isLetterOrNumber()) ++ia;
    while (ib < b.size() && !b.at(ib).isLetterOrNumber()) ++ib;
    if (ia == a.size() || ib == b.size()) {
      return ia == a.size() && ib == b.size();
    }
    if (a.at(ia).toLower() != b.at(ib).toLower()) return false;
    ++ia;
    ++ib;
  }
}

bool isValidModelId(const QString& id) {
  if (id.isEmpty() || id.size() > 200) return false;
  if (id != id.trimmed()) return false;
  for (const QChar c : id) {
    if (c.isSpace()) return false;
    if (c.unicode() < 0x20 || c.unicode() > 0x7e) return false;
  }
  return true;
}

bool isSha256Hex(const QString& digest) {
  if (digest.size() != 64) return false;
  for (const QChar c : digest) {
    const char16_t u = c.unicode();
    if (!((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f'))) return false;
  }
  return true;
}

bool isBuiltinModelId(const QString& id) {
  return isValidModelId(id) && !id.startsWith(QLatin1String(kUserModelIdPrefix));
}

bool isUserModelId(const QString& id) {
  if (!id.startsWith(QLatin1String(kUserModelIdPrefix))) return false;
  return isSha256Hex(id.mid(QLatin1String(kUserModelIdPrefix).size()));
}

// ------------------------------------------------------ model profile helpers

namespace {

/// The uniform positive integer scale the fixed declared geometry implies, or
/// nullopt when either side is dynamic or the shapes are not a uniform multiple.
/// Identity is 1. The factor is derived from the actual declared shapes; a
/// caller-supplied scale is never trusted over them.
std::optional<int> deriveScale(const TensorProfile& input, const TensorProfile& output) {
  if (input.isDynamicGeometry() || output.isDynamicGeometry()) return std::nullopt;
  const qint64 inH = *input.height;
  const qint64 inW = *input.width;
  const qint64 outH = *output.height;
  const qint64 outW = *output.width;
  if (inH <= 0 || inW <= 0 || outH <= 0 || outW <= 0) return std::nullopt;
  if (outH % inH != 0 || outW % inW != 0) return std::nullopt;
  const qint64 scaleH = outH / inH;
  const qint64 scaleW = outW / inW;
  if (scaleH != scaleW || scaleH <= 0) return std::nullopt;
  return static_cast<int>(scaleH);
}

}  // namespace

namespace {

/// The NCHW boundary rules both spatial checks apply. `allowSymbolicOutputBatch`
/// is the whole difference between the strict eligibility check and the candidate
/// check: it lets the declared *output* batch be the symbolic -1 an exporter
/// writes for a dynamic batch, while the input batch is still required to be
/// exactly 1. Nothing is rewritten so that a profile fits these rules - a caller
/// that needs a concrete boundary gets it from a run, never from a copy with a
/// forced batch.
QString spatialBoundsRefusal(const ModelProfile& profile, const bool allowSymbolicOutputBatch) {
  const TensorProfile& in = profile.input;
  const TensorProfile& out = profile.output;

  if (!isEligibleIoType(in.dtype) || !isEligibleIoType(out.dtype)) {
    return QStringLiteral("declared input %1 (%2) and output %3 (%4); only %5 I/O is eligible on "
                          "both tensors, and no converter is applied")
        .arg(in.describe(), in.dtype.isEmpty() ? QStringLiteral("no dtype") : in.dtype,
             out.describe(), out.dtype.isEmpty() ? QStringLiteral("no dtype") : out.dtype,
             QLatin1String(kEligibleIoType));
  }
  if (in.rank != 4 || out.rank != 4) {
    return QStringLiteral("declared input rank %1 and output rank %2; rank 4 required on both")
        .arg(in.rank)
        .arg(out.rank);
  }
  const bool outputBatchOk = out.batch == 1 || (allowSymbolicOutputBatch && out.batch == -1);
  if (in.batch != 1 || !outputBatchOk) {
    return QStringLiteral("declared input %1x%2xHxW and output %3x%4xHxW; N=1 required on both")
        .arg(in.batch)
        .arg(in.channels)
        .arg(out.batch)
        .arg(out.channels);
  }
  if (in.channels != 3 || out.channels != 3) {
    return QStringLiteral("declared input %1x%2xHxW and output %3x%4xHxW; C=3 required on both")
        .arg(in.batch)
        .arg(in.channels)
        .arg(out.batch)
        .arg(out.channels);
  }
  if (in.height && *in.height <= 0) {
    return QStringLiteral("declared input height %1; must be positive").arg(*in.height);
  }
  if (in.width && *in.width <= 0) {
    return QStringLiteral("declared input width %1; must be positive").arg(*in.width);
  }
  if (out.height && *out.height <= 0) {
    return QStringLiteral("declared output height %1; must be positive").arg(*out.height);
  }
  if (out.width && *out.width <= 0) {
    return QStringLiteral("declared output width %1; must be positive").arg(*out.width);
  }
  // A dynamic graph resolves its geometry at the stage it actually runs at, so
  // its scale is simply unknown here - not a non-uniform one. Only two fixed,
  // declared shapes can fail the uniform-multiple rule.
  if (!in.isDynamicGeometry() && !out.isDynamicGeometry() && !deriveScale(in, out).has_value()) {
    return QStringLiteral("declared output %1 is not a uniform positive integer multiple of input "
                          "%2")
        .arg(out.describe(), in.describe());
  }
  return QString();
}

}  // namespace

QString spatialEligibilityRefusal(const ModelProfile& profile) {
  return spatialBoundsRefusal(profile, false);
}

QString spatialCandidateRefusal(const ModelProfile& profile) {
  return spatialBoundsRefusal(profile, true);
}

QString spatialReferenceRefusal(const ModelProfile& declared, const ModelProfile& reference) {
  const QString candidateRefusal = spatialCandidateRefusal(declared);
  if (!candidateRefusal.isEmpty()) return candidateRefusal;

  // A proof is what a run returned, so it has to be eligible on its own terms and
  // fully concrete: no dimension stays symbolic through a run, and a proof whose
  // geometry is unknown cannot be compared against anything.
  const QString referenceRefusal = spatialEligibilityRefusal(reference);
  if (!referenceRefusal.isEmpty()) return referenceRefusal;
  if (reference.isDynamicGeometry()) {
    return QStringLiteral("the reference %1 -> %2 has no concrete geometry; a run returns shapes, "
                          "so a declaration is not a proof")
        .arg(reference.input.describe(), reference.output.describe());
  }

  // The reference is evidence about this exact declaration: every dtype, rank,
  // channel count and dimension the declaration actually states must be the one
  // the run produced. A dimension the declaration leaves dynamic is not
  // contradicted by the run's concrete one - that is what the run answers - and
  // the symbolic batch is never compared, because settling it is the run's whole
  // purpose. Nothing is forced onto `declared` here.
  const auto statedByRun = [](const TensorProfile& declaredTensor,
                              const TensorProfile& referenceTensor) {
    if (declaredTensor.dtype != referenceTensor.dtype) return false;
    if (declaredTensor.rank != referenceTensor.rank) return false;
    if (declaredTensor.channels != referenceTensor.channels) return false;
    if (declaredTensor.batch != -1 && declaredTensor.batch != referenceTensor.batch) return false;
    if (declaredTensor.height.has_value() && declaredTensor.height != referenceTensor.height) {
      return false;
    }
    if (declaredTensor.width.has_value() && declaredTensor.width != referenceTensor.width) {
      return false;
    }
    return true;
  };
  if (!statedByRun(declared.input, reference.input) ||
      !statedByRun(declared.output, reference.output)) {
    return QStringLiteral("the reference %1 -> %2 does not prove declared %3 -> %4")
        .arg(reference.input.describe(), reference.output.describe(), declared.input.describe(),
             declared.output.describe());
  }
  return QString();
}

std::optional<int> ModelProfile::derivedUniformScale() const {
  return deriveScale(input, output);
}

std::optional<QSize> ModelProfile::sourceGeometry() const {
  if (!input.hasGeometry()) return std::nullopt;
  return QSize(static_cast<int>(*input.width), static_cast<int>(*input.height));
}

std::optional<QSize> ModelProfile::outputGeometry() const {
  if (!output.hasGeometry()) return std::nullopt;
  return QSize(static_cast<int>(*output.width), static_cast<int>(*output.height));
}

QString ModelRecord::contentId() const {
  return computeContentId(role, onnxSha256, actualProfile);
}

bool ModelRecord::isOffered() const {
  QString key;
  QString reason;
  refusal(&key, &reason);
  return key.isEmpty();
}

void ModelRecord::refusal(QString* reasonKey, QString* reason) const {
  const auto fail = [reasonKey, reason](const QString& key, const QString& text) {
    if (reasonKey) *reasonKey = key;
    if (reason) *reason = text;
  };

  if (!isValidModelId(id)) {
    fail(QStringLiteral("model.id.invalid"),
         QStringLiteral("model id is missing or not a usable identifier"));
    return;
  }
  if (!isSha256Hex(onnxSha256)) {
    fail(QStringLiteral("model.digest.missing"),
         QStringLiteral("model %1 has no 64-character ONNX sha256, so its bytes cannot be "
                        "identified; a file name alone is not identity")
                 .arg(id));
    return;
  }
  const bool builtin = isBuiltinModelId(id);
  if (builtin && origin == ModelOrigin::UserImported) {
    fail(QStringLiteral("model.id.builtin.not.user"),
         QStringLiteral("model id %1 is a built-in name, but the row claims a user import").arg(id));
    return;
  }
  if (!builtin && origin == ModelOrigin::Builtin) {
    fail(QStringLiteral("model.id.user.not.builtin"),
         QStringLiteral("model id %1 is content-addressed, but the row claims to be built-in")
             .arg(id));
    return;
  }
  // The NCHW spatial boundary rules apply to spatial graphs only. A temporal row
  // declares a different boundary and its eligibility comes from the temporal
  // inspection that provisions it, so no spatial refusal is invented for it here.
  if (role == ModelRole::Spatial) {
    const QString profileRefusal = spatialEligibilityRefusal(actualProfile);
    if (!profileRefusal.isEmpty()) {
      const QString plainRefusal =
          QStringLiteral("model %1 is refused: %2").arg(id, profileRefusal);
      // One declared relaxation exists, and metadata cannot settle it: -1 is an
      // exporter's way of saying "batch to be learned", so such a row is a
      // candidate, not an eligible boundary. It is honoured only when a verified
      // reference run has already proved the concrete boundary for this very
      // declaration - never by rewriting the declared batch to 1, and never from
      // the declaration alone.
      if (!spatialCandidateRefusal(actualProfile).isEmpty()) {
        fail(QStringLiteral("model.profile.ineligible"), plainRefusal);
        return;
      }
      if (!verifiedReferenceProfile.has_value()) {
        fail(QStringLiteral("model.profile.ineligible"),
             QStringLiteral("%1; the declared output batch is symbolic (-1) and no verified "
                            "reference run has proved it")
                 .arg(plainRefusal));
        return;
      }
      const QString proofRefusal =
          spatialReferenceRefusal(actualProfile, *verifiedReferenceProfile);
      if (!proofRefusal.isEmpty()) {
        fail(QStringLiteral("model.profile.ineligible"),
             QStringLiteral("%1; %2").arg(plainRefusal, proofRefusal));
        return;
      }
    }
  }
  if (!availability.hasMandatoryReason()) {
    fail(QStringLiteral("model.reason.missing"),
         QStringLiteral("model %1 is %2 without saying why").arg(id, stableName(availability.value)));
    return;
  }
  fail(QString(), QString());
}

const ModelRecord* CatalogView::find(const QString& id) const {
  for (const ModelRecord& record : models) {
    if (record.id == id) return &record;
  }
  return nullptr;
}

bool CatalogView::contains(const QString& id) const {
  return find(id) != nullptr;
}

QString contentIdentityPayload(const ModelRole role, const QString& onnxSha256,
                               const ModelProfile& profile) {
  QStringList parts;
  parts << stableName(role) << onnxSha256.toLower();
  for (const TensorProfile* tensor : {&profile.input, &profile.output}) {
    const QString prefix = (tensor == &profile.input) ? QStringLiteral("in") : QStringLiteral("out");
    parts << QStringLiteral("%1.rank=%2").arg(prefix).arg(tensor->rank);
    parts << QStringLiteral("%1.dtype=%2").arg(prefix).arg(tensor->dtype);
    parts << QStringLiteral("%1.n=%3").arg(prefix).arg(tensor->batch);
    parts << QStringLiteral("%1.c=%4").arg(prefix).arg(tensor->channels);
    parts << QStringLiteral("%1.h=%5")
                 .arg(prefix)
                 .arg(tensor->height ? QString::number(*tensor->height) : QStringLiteral("?"));
    parts << QStringLiteral("%1.w=%6")
                 .arg(prefix)
                 .arg(tensor->width ? QString::number(*tensor->width) : QStringLiteral("?"));
  }
  return parts.join(QLatin1Char('\n'));
}

QString computeContentId(const ModelRole role, const QString& onnxSha256,
                         const ModelProfile& profile) {
  const QByteArray digest = QCryptographicHash::hash(
      contentIdentityPayload(role, onnxSha256, profile).toUtf8(), QCryptographicHash::Sha256);
  return QStringLiteral("sha256:") + QString::fromLatin1(digest.toHex());
}

QString userModelIdFor(const QString& contentId) {
  static const QString prefix = QString::fromLatin1(kUserModelIdPrefix);
  QString digest = contentId;
  if (digest.startsWith(QLatin1String("sha256:"))) digest = digest.mid(7);
  return prefix + digest;
}

// ------------------------------------------------------------ serialization

QString recipeSchema() {
  return QString::fromLatin1(kSchema);
}

QJsonObject recipeToJson(const Recipe& recipe) {
  QJsonObject document;
  QJsonArray stages;
  for (const SpatialStage& stage : recipe.spatial) {
    QJsonObject entry;
    entry.insert(QStringLiteral("model"), stage.modelId);
    if (stage.preResizeEnabled && stage.preResize.width() > 0 && stage.preResize.height() > 0) {
      QJsonObject geometry;
      geometry.insert(QStringLiteral("w"), stage.preResize.width());
      geometry.insert(QStringLiteral("h"), stage.preResize.height());
      entry.insert(QStringLiteral("preResize"), geometry);
    }
    stages.append(entry);
  }
  document.insert(QStringLiteral("schema"), recipeSchema());
  document.insert(QStringLiteral("id"), recipe.id);
  document.insert(QStringLiteral("label"), recipe.label);
  document.insert(QStringLiteral("backend"), stableName(recipe.backend));
  document.insert(QStringLiteral("spatial"), stages);

  if (recipe.temporal.has_value()) {
    const TemporalStage& stage = *recipe.temporal;
    QJsonObject factor;
    factor.insert(QStringLiteral("num"), stage.factor.num);
    factor.insert(QStringLiteral("den"), stage.factor.den);

    QJsonObject temporal;
    temporal.insert(QStringLiteral("model"), stage.modelId());
    temporal.insert(QStringLiteral("version"), stableName(stage.version));
    temporal.insert(QStringLiteral("variant"), stableName(stage.variant));
    temporal.insert(QStringLiteral("ensemble"), stage.ensemble);
    temporal.insert(QStringLiteral("factor"), factor);
    temporal.insert(QStringLiteral("order"), stableName(stage.order));
    temporal.insert(QStringLiteral("sceneThreshold"), stage.sceneThreshold);
    document.insert(QStringLiteral("temporal"), temporal);
  }

  return document;
}

namespace {

/// Reads the temporal member. Every field is required: `order` in particular,
/// because defaulting it would pick an execution order the document never stated.
bool readTemporal(const QJsonValue& value, TemporalStage* stage, QString* errorKey,
                  QString* error) {
  const auto reject = [errorKey, error](const QString& key, const QString& text) {
    *errorKey = key;
    *error = text;
    return false;
  };

  if (!value.isObject()) {
    return reject(QStringLiteral("recipe.temporal.not.an.object"),
                  QStringLiteral("the temporal member is not an object"));
  }
  const QJsonObject object = value.toObject();
  if (!noUnknownKeys(object, temporalKeys())) {
    return reject(QStringLiteral("recipe.temporal.unknown.member"),
                  QStringLiteral("the temporal member carries a key this schema does not define"));
  }

  const QString model = jsonString(object, QStringLiteral("model"));
  if (model.isEmpty()) {
    return reject(QStringLiteral("stage.temporal.model.missing"),
                  QStringLiteral("the temporal stage names no model"));
  }

  const std::optional<RifeVersion> version =
      rifeVersionFromName(jsonString(object, QStringLiteral("version")));
  if (!version.has_value()) {
    return reject(QStringLiteral("stage.temporal.version.unknown"),
                  QStringLiteral("temporal version %1 is not a revision this build speaks")
                      .arg(jsonString(object, QStringLiteral("version"))));
  }
  const std::optional<RifeVariant> variant =
      rifeVariantFromName(jsonString(object, QStringLiteral("variant")));
  if (!variant.has_value()) {
    return reject(QStringLiteral("stage.temporal.variant.unknown"),
                  QStringLiteral("temporal variant %1 is not supported by this build")
                      .arg(jsonString(object, QStringLiteral("variant"))));
  }
  if (!object.value(QStringLiteral("ensemble")).isBool()) {
    return reject(QStringLiteral("stage.temporal.ensemble.malformed"),
                  QStringLiteral("the temporal ensemble flag is not a boolean"));
  }
  const bool ensemble = object.value(QStringLiteral("ensemble")).toBool();

  if (model != rifeModelName(*version, *variant, ensemble)) {
    return reject(QStringLiteral("stage.temporal.model.mismatch"),
                  QStringLiteral("temporal model %1 contradicts version %2 / variant %3 / ensemble "
                                 "%4")
                      .arg(model, jsonString(object, QStringLiteral("version")),
                           jsonString(object, QStringLiteral("variant")),
                           ensemble ? QStringLiteral("true") : QStringLiteral("false")));
  }

  const QJsonValue factorValue = object.value(QStringLiteral("factor"));
  if (!factorValue.isObject()) {
    return reject(QStringLiteral("stage.temporal.factor.malformed"),
                  QStringLiteral("the temporal factor is not an object"));
  }
  const QJsonObject factor = factorValue.toObject();
  if (!noUnknownKeys(factor, factorKeys())) {
    return reject(QStringLiteral("stage.temporal.factor.unknown.member"),
                  QStringLiteral("the temporal factor carries a key this schema does not define"));
  }
  Rational factorValueParsed;
  if (!jsonInt(factor, QStringLiteral("num"), &factorValueParsed.num) ||
      !jsonInt(factor, QStringLiteral("den"), &factorValueParsed.den)) {
    return reject(QStringLiteral("stage.temporal.factor.malformed"),
                  QStringLiteral("the temporal factor needs integer num and den"));
  }

  const QJsonValue orderValue = object.value(QStringLiteral("order"));
  if (!orderValue.isString()) {
    return reject(QStringLiteral("stage.temporal.order.missing"),
                  QStringLiteral("the temporal stage states no order; an unstated order is "
                                 "ambiguity, not something to default"));
  }
  const std::optional<TemporalOrder> order = temporalOrderFromName(orderValue.toString());
  if (!order.has_value()) {
    return reject(QStringLiteral("stage.temporal.order.unknown"),
                  QStringLiteral("temporal order %1 is neither beforeSpatial nor afterSpatial")
                      .arg(orderValue.toString()));
  }

  const QJsonValue thresholdValue = object.value(QStringLiteral("sceneThreshold"));
  if (!thresholdValue.isDouble()) {
    return reject(QStringLiteral("stage.temporal.threshold.malformed"),
                  QStringLiteral("the temporal scene threshold is not a number"));
  }
  const double threshold = thresholdValue.toDouble();

  TemporalStage parsed;
  parsed.version = *version;
  parsed.variant = *variant;
  parsed.ensemble = ensemble;
  // The pair is kept exactly as written. 4/2 and 2/1 mean the same frame rate,
  // and reducing it here would rewrite the request without being asked to.
  parsed.factor = factorValueParsed;
  parsed.order = *order;
  parsed.sceneThreshold = threshold;
  *stage = parsed;
  return true;
}

}  // namespace

bool parseRecipe(const QJsonObject& object, Recipe* recipe, QString* errorKey, QString* error) {
  const auto reject = [recipe, errorKey, error](const QString& key, const QString& text) {
    *recipe = Recipe{};
    if (errorKey) *errorKey = key;
    if (error) *error = text;
    return false;
  };

  if (!noUnknownKeys(object, recipeKeys())) {
    return reject(QStringLiteral("recipe.unknown.member"),
                  QStringLiteral("the recipe carries a key this schema does not define"));
  }

  const QString schema = jsonString(object, QStringLiteral("schema"));
  if (schema != recipeSchema()) {
    return reject(QStringLiteral("recipe.schema.unknown"),
                  QStringLiteral("recipe schema %1 is not %2").arg(schema, recipeSchema()));
  }

  const QString id = jsonString(object, QStringLiteral("id"));
  if (!isValidModelId(id)) {
    return reject(QStringLiteral("recipe.id.invalid"),
                  QStringLiteral("the recipe has no usable id"));
  }

  const std::optional<Backend> backend = backendFromName(jsonString(object, QStringLiteral("backend")));
  if (!backend.has_value()) {
    return reject(QStringLiteral("recipe.backend.unknown"),
                  QStringLiteral("recipe backend %1 is neither tensorrt nor directml")
                      .arg(jsonString(object, QStringLiteral("backend"))));
  }

  const QJsonValue spatialValue = object.value(QStringLiteral("spatial"));
  if (!spatialValue.isArray()) {
    return reject(QStringLiteral("recipe.spatial.malformed"),
                  QStringLiteral("the spatial member is not an array"));
  }

  Recipe parsed;
  parsed.id = id;
  parsed.label = jsonString(object, QStringLiteral("label"));
  parsed.backend = *backend;

  const QJsonArray stages = spatialValue.toArray();
  for (int i = 0; i < stages.size(); ++i) {
    const QJsonValue stageValue = stages.at(i);
    if (!stageValue.isObject()) {
      return reject(QStringLiteral("stage.malformed"),
                    QStringLiteral("spatial stage %1 is not an object").arg(i));
    }
    const QJsonObject stageObject = stageValue.toObject();
    if (!noUnknownKeys(stageObject, spatialKeys())) {
      return reject(QStringLiteral("stage.unknown.member"),
                    QStringLiteral("spatial stage %1 carries a key this schema does not define")
                        .arg(i));
    }
    SpatialStage stage;
    stage.modelId = jsonString(stageObject, QStringLiteral("model"));
    const QJsonValue preResize = stageObject.value(QStringLiteral("preResize"));
    if (!preResize.isUndefined() && !preResize.isNull()) {
      if (!preResize.isObject()) {
        return reject(QStringLiteral("stage.preresize.malformed"),
                      QStringLiteral("spatial stage %1 has a preResize that is not an object")
                          .arg(i));
      }
      const QJsonObject geometry = preResize.toObject();
      if (!noUnknownKeys(geometry, geometryKeys())) {
        return reject(QStringLiteral("stage.preresize.unknown.member"),
                      QStringLiteral("spatial stage %1 has a preResize with a key this schema does "
                                     "not define")
                          .arg(i));
      }
      int w = 0;
      int h = 0;
      if (!jsonInt(geometry, QStringLiteral("w"), &w) ||
          !jsonInt(geometry, QStringLiteral("h"), &h)) {
        return reject(QStringLiteral("stage.preresize.malformed"),
                      QStringLiteral("spatial stage %1 has a preResize without integer w and h")
                          .arg(i));
      }
      if (w <= 0 || h <= 0) {
        return reject(QStringLiteral("stage.preresize.invalid"),
                      QStringLiteral("spatial stage %1 has a preResize of %2x%3; both sides must be "
                                     "positive")
                          .arg(i)
                          .arg(w)
                          .arg(h));
      }
      stage.preResize = QSize(w, h);
      stage.preResizeEnabled = true;
    }
    parsed.spatial.append(stage);
  }

  const QJsonValue temporalValue = object.value(QStringLiteral("temporal"));
  if (!temporalValue.isUndefined() && !temporalValue.isNull()) {
    TemporalStage temporal;
    QString key;
    QString text;
    if (!readTemporal(temporalValue, &temporal, &key, &text)) {
      return reject(key, text);
    }
    parsed.temporal = temporal;
  }

  const RecipeVerdict verdict = validateRecipe(parsed);
  if (!verdict.ok) return reject(verdict.reasonKey, verdict.reason);

  *recipe = parsed;
  if (errorKey) *errorKey = QString();
  if (error) *error = QString();
  return true;
}

bool parseRecipe(const QByteArray& payload, Recipe* recipe, QString* errorKey, QString* error) {
  QJsonParseError parseError{};
  const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    *recipe = Recipe{};
    if (errorKey) *errorKey = QStringLiteral("recipe.json.malformed");
    if (error) {
      *error = QStringLiteral("the recipe is not a JSON object document: %1")
                   .arg(parseError.errorString());
    }
    return false;
  }
  return parseRecipe(document.object(), recipe, errorKey, error);
}

QByteArray serializeRecipe(const Recipe& recipe) {
  return QJsonDocument(recipeToJson(recipe)).toJson(QJsonDocument::Compact);
}

}  // namespace native
}  // namespace mplayer