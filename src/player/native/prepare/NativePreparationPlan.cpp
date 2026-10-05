#include "NativePreparationPlan.hpp"

#include <numeric>

#include <QtCore/QCryptographicHash>

#include "native/models/NativeRife.hpp"

namespace mplayer {
namespace native {

namespace {

constexpr const char* kPlanSchema = "mikai.native.prepplan/1";

/// Mirrors the reference's `i & ~1`: the nearest even value at or below, so an
/// odd border never splits 4:2:0 chroma.
int floorEven(int value) { return value & ~1; }

int roundUpTo(int value, int multiple) {
  return ((value + multiple - 1) / multiple) * multiple;
}

/// `num/den` reduced, refusing anything that would not fit in an int rather than
/// wrapping it. A wrapped cadence is a wrong PTS schedule, not a rounding error.
std::optional<Rational> reducedChecked(qint64 num, qint64 den) {
  if (num <= 0 || den <= 0) return std::nullopt;
  const qint64 divisor = std::gcd(num, den);
  if (divisor > 1) {
    num /= divisor;
    den /= divisor;
  }
  constexpr qint64 kLimit = 2147483647LL;
  if (num > kLimit || den > kLimit) return std::nullopt;
  return Rational{static_cast<int>(num), static_cast<int>(den)};
}

}  // namespace

// ---------------------------------------------------------------- geometry

bool isRefusableGeometry(const QSize& size) {
  return size.width() <= 0 || size.height() <= 0 || size.width() > kMaxDimension ||
         size.height() > kMaxDimension;
}

std::optional<int> uniformIntegerScale(const QSize& input, const QSize& output) {
  if (isRefusableGeometry(input) || isRefusableGeometry(output)) return std::nullopt;
  if (output.width() % input.width() != 0) return std::nullopt;
  if (output.height() % input.height() != 0) return std::nullopt;
  const int byWidth = output.width() / input.width();
  const int byHeight = output.height() / input.height();
  if (byWidth != byHeight || byWidth <= 0) return std::nullopt;
  return byWidth;
}

QSize padToTensorAlignment(const QSize& size, int* padL, int* padT) {
  const int paddedWidth = roundUpTo(size.width(), kTensorAlign);
  const int paddedHeight = roundUpTo(size.height(), kTensorAlign);
  if (padL != nullptr) *padL = floorEven((paddedWidth - size.width()) / 2);
  if (padT != nullptr) *padT = floorEven((paddedHeight - size.height()) / 2);
  return QSize(paddedWidth, paddedHeight);
}

QSize paddingExtent(const QSize& size) {
  // The total added on each axis. How it is split is what padL/padT answer.
  const QSize padded = padToTensorAlignment(size, nullptr, nullptr);
  return QSize(padded.width() - size.width(), padded.height() - size.height());
}

std::optional<Rational> scaleCadence(const Rational& fps, int num, int den) {
  if (!fps.isValid() || num <= 0 || den <= 0) return std::nullopt;
  return reducedChecked(static_cast<qint64>(fps.num) * num,
                        static_cast<qint64>(fps.den) * den);
}

QString stageShapeString(int channels, const QSize& size) {
  if (channels <= 0 || isRefusableGeometry(size)) return QString();
  return QStringLiteral("1x%1x%2x%3").arg(channels).arg(size.height()).arg(size.width());
}

std::optional<QSize> displayGeometry(const std::optional<QSize>& codedSize,
                                     const std::optional<QRect>& crop,
                                     const std::optional<int>& rotationDegrees) {
  if (!codedSize.has_value()) return std::nullopt;
  QSize size = *codedSize;
  if (crop.has_value()) {
    // A crop that leaves nothing, or reaches outside the coded frame, is a
    // property this build cannot honour rather than one to silently clamp.
    if (crop->isEmpty() || crop->x() < 0 || crop->y() < 0 ||
        crop->x() + crop->width() > size.width() || crop->y() + crop->height() > size.height()) {
      return std::nullopt;
    }
    size = crop->size();
  }
  if (rotationDegrees.has_value()) {
    const int degrees = *rotationDegrees;
    if (degrees == 90 || degrees == 270) {
      size = QSize(size.height(), size.width());
    } else if (degrees != 0 && degrees != 180) {
      return std::nullopt;
    }
  }
  return size;
}

std::optional<QSize> SourceGeometry::displaySize() const {
  return displayGeometry(codedSize, crop, rotationDegrees);
}

bool SourceGeometry::hasModelledRotation() const {
  if (!rotationDegrees.has_value()) return false;
  const int degrees = *rotationDegrees;
  return degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270;
}

std::optional<qint64> SourceGeometry::pixelCount() const {
  const std::optional<QSize> display = displaySize();
  if (!display.has_value()) return std::nullopt;
  return static_cast<qint64>(display->width()) * static_cast<qint64>(display->height());
}

bool ChainWindow::admits(qint64 pixels, const Rational& fpsValue) const {
  return refusal(pixels, fpsValue, nullptr, nullptr).isEmpty();
}

QString ChainWindow::refusal(qint64 pixels, const Rational& fpsValue, QString* reasonKey,
                             QString* reason) const {
  const auto refuse = [reasonKey, reason](const QString& key, const QString& text) {
    if (reasonKey != nullptr) *reasonKey = key;
    if (reason != nullptr) *reason = text;
    return text;
  };
  if (minPixels > 0 && pixels < minPixels) {
    return refuse(QStringLiteral("source.window.pixels"),
                  QStringLiteral("source has %1 pixels, below this chain's minimum of %2")
                      .arg(pixels)
                      .arg(minPixels));
  }
  if (maxPixels > 0 && pixels > maxPixels) {
    return refuse(QStringLiteral("source.window.pixels"),
                  QStringLiteral("source has %1 pixels, above this chain's maximum of %2")
                      .arg(pixels)
                      .arg(maxPixels));
  }
  if (!fpsValue.isValid()) {
    return refuse(QStringLiteral("source.window.fps"),
                  QStringLiteral("source cadence is unknown, so it cannot be matched against this "
                                 "chain's fps window"));
  }
  const qint64 sourceFps = static_cast<qint64>(fpsValue.num);
  if (minFps.has_value() &&
      sourceFps * static_cast<qint64>(minFps->den) <
          static_cast<qint64>(minFps->num) * static_cast<qint64>(fpsValue.den)) {
    return refuse(QStringLiteral("source.window.fps"),
                  QStringLiteral("source cadence %1 is below this chain's minimum of %2")
                      .arg(fpsValue.toString(), minFps->toString()));
  }
  if (maxFps.has_value() &&
      sourceFps * static_cast<qint64>(maxFps->den) >
          static_cast<qint64>(maxFps->num) * static_cast<qint64>(fpsValue.den)) {
    return refuse(QStringLiteral("source.window.fps"),
                  QStringLiteral("source cadence %1 is above this chain's maximum of %2")
                      .arg(fpsValue.toString(), maxFps->toString()));
  }
  if (reasonKey != nullptr) *reasonKey = QString();
  if (reason != nullptr) reason->clear();
  return QString();
}

// ------------------------------------------------------------ stage shapes

QString ResolvedStage::inputShape() const {
  return stageShapeString(inputChannels, inputGeometry);
}

QString ResolvedStage::outputShape() const {
  return stageShapeString(outputChannels, outputGeometry);
}

QString ResolvedStage::ioFormats() const { return QStringLiteral("fp16:chw"); }

QString ResolvedStage::buildRequestPayload() const {
  QStringList lines;
  lines << QStringLiteral("schema=%1").arg(QString::fromLatin1(kPlanSchema));
  lines << QStringLiteral("role=%1").arg(stableName(role));
  lines << QStringLiteral("model=%1").arg(modelContentId);
  lines << QStringLiteral("input=%1").arg(inputShape());
  lines << QStringLiteral("output=%1").arg(outputShape());
  lines << QStringLiteral("io=%1").arg(ioFormats());
  lines << QStringLiteral("padL=%1").arg(padL);
  lines << QStringLiteral("padT=%1").arg(padT);
  return lines.join(QLatin1Char('\n'));
}

// -------------------------------------------------------------------- plan

std::optional<QSize> PreparationPlan::outputGeometry() const {
  if (!isResolved() || stages.isEmpty()) return std::nullopt;
  return stages.last().contentGeometry;
}

std::optional<Rational> PreparationPlan::outputCadence() const {
  if (!isResolved() || stages.isEmpty()) return std::nullopt;
  return stages.last().outputFps;
}

QString PreparationPlan::canonicalBody() const {
  QStringList lines;
  lines << QStringLiteral("schema=%1").arg(QString::fromLatin1(kPlanSchema));
  lines << QStringLiteral("recipe=%1").arg(recipeId);
  lines << QStringLiteral("backend=%1").arg(stableName(backend));
  const std::optional<QSize> display = source.displaySize();
  lines << QStringLiteral("source=%1x%2")
               .arg(display.has_value() ? display->width() : 0)
               .arg(display.has_value() ? display->height() : 0);
  lines << QStringLiteral("fps=%1").arg(source.fps.has_value() ? source.fps->toString()
                                                               : QStringLiteral("?"));
  lines << QStringLiteral("csp=%1").arg(source.csp);
  lines << QStringLiteral("matrix=%1").arg(source.matrix);
  lines << QStringLiteral("range=%1").arg(source.range);
  lines << QStringLiteral("siting=%1").arg(source.siting);
  lines << QStringLiteral("bits=%1").arg(source.bitDepth);
  for (const ResolvedStage& stage : stages) {
    lines << QStringLiteral("stage=%1").arg(stage.buildRequestPayload());
    // Cadence and interpolation are part of the plan's identity but not of the
    // engine it needs: the same engine serves 2x and 3x. They enter the body
    // here rather than in buildRequestPayload() so two plans that differ only in
    // their output cadence cannot collide on one id.
    lines << QStringLiteral("stagefps=%1>%2")
                 .arg(stage.inputFps.toString(), stage.outputFps.toString());
    if (stage.kind == StageKind::Temporal) {
      lines << QStringLiteral("stageinterp=order=%1 factor=%2 threshold=%3")
                   .arg(stableName(stage.order), stage.factor.toString())
                   .arg(stage.sceneThreshold);
    }
  }
  for (const PreResizeStep& resize : resizes) {
    lines << QStringLiteral("resize=%1 %2x%3->%4x%5 temporal=%6")
                 .arg(resize.modelId)
                 .arg(resize.from.width())
                 .arg(resize.from.height())
                 .arg(resize.to.width())
                 .arg(resize.to.height())
                 .arg(resize.beforeTemporal ? 1 : 0);
  }
  return lines.join(QLatin1Char('\n'));
}

namespace {

/// Accumulates a plan being resolved: unknowns while facts are missing, blockers
/// for facts that refuse it. Once anything is blocked nothing else is recorded,
/// so a refused plan does not also claim it was waiting on something.
class PlanBuilder {
 public:
  void noteUnknown(const QString& fact) {
    if (blockers.isEmpty() && !unknowns.contains(fact)) unknowns.append(fact);
  }

  void block(const QString& reasonKey, const QString& reason, int stageIndex = -1) {
    if (!blockers.isEmpty()) return;
    blockers.append(PlanBlocker{reasonKey, reason, stageIndex});
    unknowns.clear();
  }

  void addStage(const ResolvedStage& stage) { stages.append(stage); }
  void addResize(const PreResizeStep& resize) { resizes.append(resize); }

  // Public: the finish paths are lambdas, and a lambda does not inherit the
  // access of the function that created it.
  QVector<ResolvedStage> stages;
  QVector<PreResizeStep> resizes;
  QVector<QString> unknowns;
  QVector<PlanBlocker> blockers;
};

/// The pre-resize a recipe stage asks for, as a target size. The reference
/// computes this from a percentage or a 16:9 height; a recipe states the size
/// directly, and the same rule applies to both: even on both axes so 4:2:0 chroma
/// stays aligned, and refused outright when the percentage would leave a frame of
/// nothing rather than clamped to something smaller than asked for.
bool resolvePreResize(const SpatialStage& stage, QSize* target, QString* reasonKey,
                      QString* reason) {
  if (!stage.preResizeEnabled) {
    *reasonKey = QStringLiteral("stage.preresize.absent");
    return false;
  }
  const QSize requested = stage.preResize;
  if (isRefusableGeometry(requested)) {
    *reasonKey = QStringLiteral("stage.preresize.invalid");
    *reason = QStringLiteral("spatial stage %1 requests a pre-resize of %2x%3, which is not a "
                             "usable frame size")
                  .arg(stage.modelId)
                  .arg(requested.width())
                  .arg(requested.height());
    return false;
  }
  if ((requested.width() & 1) != 0 || (requested.height() & 1) != 0) {
    *reasonKey = QStringLiteral("stage.preresize.odd");
    *reason = QStringLiteral("spatial stage %1 requests a pre-resize of %2x%3; both sides must be "
                             "even so 4:2:0 chroma stays aligned")
                  .arg(stage.modelId)
                  .arg(requested.width())
                  .arg(requested.height());
    return false;
  }
  *target = requested;
  return true;
}

/// One spatial stage's geometry, from its own declaration or from the caller's
/// resolver. `resolvedAt` is the size the stage will actually run at.
bool resolveSpatialGeometry(const ModelRecord& record, const QSize& resolvedAt,
                            const DynamicProfileResolver& resolver, ResolvedStage* stage,
                            QString* reasonKey, QString* reason) {
  const ModelProfile& declared = record.actualProfile;

  QSize input = resolvedAt;
  QSize output;
  bool fromResolver = false;

  if (declared.isDynamicGeometry()) {
    if (!resolver) {
      *reasonKey = QStringLiteral("stage.dynamic.unresolved");
      *reason = QStringLiteral("spatial stage %1 leaves H and W dynamic and no resolver was "
                               "supplied, so its geometry at %2x%3 is unknown; a scale is not "
                               "assumed from the file name")
                    .arg(record.id)
                    .arg(resolvedAt.width())
                    .arg(resolvedAt.height());
      return false;
    }
    const std::optional<ResolvedProfile> resolved = resolver(record, resolvedAt);
    if (!resolved.has_value()) {
      *reasonKey = QStringLiteral("stage.dynamic.unresolved");
      *reason = QStringLiteral("spatial stage %1 leaves H and W dynamic and reported no geometry "
                               "at %2x%3; a scale is not assumed from the file name")
                    .arg(record.id)
                    .arg(resolvedAt.width())
                    .arg(resolvedAt.height());
      return false;
    }
    if (resolved->input != resolvedAt) {
      *reasonKey = QStringLiteral("stage.dynamic.mismatch");
      *reason = QStringLiteral("spatial stage %1 resolved geometry for a %2x%3 input, but this "
                               "stage runs at %4x%5")
                    .arg(record.id)
                    .arg(resolved->input.width())
                    .arg(resolved->input.height())
                    .arg(resolvedAt.width())
                    .arg(resolvedAt.height());
      return false;
    }
    if (isRefusableGeometry(resolved->output) || resolved->inputChannels <= 0 ||
        resolved->outputChannels <= 0) {
      *reasonKey = QStringLiteral("stage.dynamic.invalid");
      *reason = QStringLiteral("spatial stage %1 resolved to %2x%3 at a %4x%5 input, which is not "
                               "a usable tensor shape")
                    .arg(record.id)
                    .arg(resolved->output.width())
                    .arg(resolved->output.height())
                    .arg(resolved->input.width())
                    .arg(resolved->input.height());
      return false;
    }
    input = resolved->input;
    output = resolved->output;
    fromResolver = true;
    stage->inputChannels = resolved->inputChannels;
    stage->outputChannels = resolved->outputChannels;
  } else {
    const std::optional<QSize> declaredInput = declared.sourceGeometry();
    const std::optional<QSize> declaredOutput = declared.outputGeometry();
    if (!declaredInput.has_value() || !declaredOutput.has_value()) {
      *reasonKey = QStringLiteral("stage.geometry.partial");
      *reason = QStringLiteral("spatial stage %1 declares input %2 and output %3, which is not a "
                               "fully declared geometry")
                    .arg(record.id, declared.input.describe(), declared.output.describe());
      return false;
    }
    if (*declaredInput != resolvedAt) {
      // A fixed graph cannot be fed another size: its engine binds one shape. The
      // reference builds per resolution and would refuse too.
      *reasonKey = QStringLiteral("stage.geometry.mismatch");
      *reason = QStringLiteral("spatial stage %1 declares a fixed input of %2x%3, but this stage "
                               "runs at %4x%5; a fixed graph is built for the size it declares")
                    .arg(record.id)
                    .arg(declaredInput->width())
                    .arg(declaredInput->height())
                    .arg(resolvedAt.width())
                    .arg(resolvedAt.height());
      return false;
    }
    input = *declaredInput;
    output = *declaredOutput;
    stage->inputChannels = static_cast<int>(declared.input.channels);
    stage->outputChannels = static_cast<int>(declared.output.channels);
  }

  const std::optional<int> scale = uniformIntegerScale(input, output);
  if (!scale.has_value()) {
    *reasonKey = QStringLiteral("stage.scale.invalid");
    *reason = QStringLiteral("spatial stage %1 produces %2x%3 from a %4x%5 input, which is not a "
                             "uniform positive integer multiple")
                  .arg(record.id)
                  .arg(output.width())
                  .arg(output.height())
                  .arg(input.width())
                  .arg(input.height());
    return false;
  }

  stage->inputGeometry = input;
  stage->outputGeometry = output;
  stage->contentGeometry = output;
  stage->scale = scale;
  stage->geometryFromResolver = fromResolver;
  return true;
}

}  // namespace

PreparationPlan resolvePreparationPlan(const PlanRequest& request) {
  PlanBuilder builder;
  PreparationPlan plan;
  plan.recipeId = request.recipe.id;
  plan.backend = request.recipe.backend;
  plan.source = request.source;
  plan.device = request.device;

  const auto finishUnavailable = [&plan, &builder]() {
    // Nothing runs while the plan is not resolved, so no stage or resize is
    // exposed: a refused plan cannot be half-executed.
    plan.unknowns = builder.unknowns;
    plan.blockers = builder.blockers;
    const PlanBlocker& first = builder.blockers.first();
    plan.availability = unavailableAvailability(first.reasonKey, first.reason);
    plan.id.clear();
  };

  const auto finishUnknown = [&plan, &builder]() {
    plan.unknowns = builder.unknowns;
    plan.blockers = builder.blockers;
    const QString reason = builder.unknowns.isEmpty()
                               ? QStringLiteral("the preparation plan has not been resolved yet")
                               : QStringLiteral("waiting for %1 to be observed")
                                     .arg(builder.unknowns.join(QStringLiteral(", ")));
    plan.availability =
        preparingAvailability(QStringLiteral("plan.preparing"), reason);
    // No identity is claimed over a body that is not settled: two Preparing plans
    // for the same source are the same plan, but nothing should look them up yet.
    plan.id.clear();
  };

  const auto finishResolved = [&plan, &builder]() {
    plan.stages = builder.stages;
    plan.resizes = builder.resizes;
    plan.unknowns = builder.unknowns;
    plan.blockers = builder.blockers;
    const QByteArray digest = QCryptographicHash::hash(plan.canonicalBody().toUtf8(),
                                                       QCryptographicHash::Sha256);
    plan.id = QStringLiteral("sha256:") + QString::fromLatin1(digest.toHex());
    plan.availability = readyAvailability();
  };

  // ---- the recipe itself ---------------------------------------------------
  const RecipeVerdict shape = validateRecipe(request.recipe);
  if (!shape.ok) {
    builder.block(shape.reasonKey, shape.reason);
    finishUnavailable();
    return plan;
  }
  if (!request.backends.has(request.recipe.backend)) {
    builder.block(QStringLiteral("backend.unavailable"),
                  QStringLiteral("recipe %1 asks for the %2 backend, which this runtime does not "
                                 "carry; no other backend is substituted")
                      .arg(request.recipe.id, stableName(request.recipe.backend)));
    finishUnavailable();
    return plan;
  }

  // ---- what has to be known before anything can be resolved ----------------
  const std::optional<QSize> display = request.source.displaySize();
  if (!display.has_value()) {
    builder.noteUnknown(request.source.codedSize.has_value()
                            ? QStringLiteral("a display geometry this build can honour")
                            : QStringLiteral("the decoded source geometry"));
    finishUnknown();
    return plan;
  }
  if (isRefusableGeometry(*display)) {
    builder.block(QStringLiteral("source.geometry.invalid"),
                  QStringLiteral("the decoded source is %1x%2, which is not a usable frame size")
                      .arg(display->width())
                      .arg(display->height()));
    finishUnavailable();
    return plan;
  }
  if (!request.source.fps.has_value() || !request.source.fps->isValid()) {
    builder.noteUnknown(QStringLiteral("the source cadence"));
    finishUnknown();
    return plan;
  }

  const std::optional<qint64> pixels = request.source.pixelCount();
  QString windowKey;
  QString windowReason;
  if (pixels.has_value()) {
    const QString refusal = request.window.refusal(*pixels, *request.source.fps, &windowKey,
                                                   &windowReason);
    if (!refusal.isEmpty()) {
      builder.block(windowKey, windowReason);
      finishUnavailable();
      return plan;
    }
  }

  // Geometry resolves without a device, because geometry is what a device-independent
  // stage identity is built from. The device is still required before any engine can
  // be built, so it is an unknown rather than a blocker here.
  if (!request.device.isKnown()) {
    builder.noteUnknown(QStringLiteral("the decoder and compute device identity"));
  }

  // ---- resolve every named model, all or nothing ---------------------------
  QVector<const ModelRecord*> spatialRecords;
  spatialRecords.reserve(request.recipe.spatial.size());
  for (const SpatialStage& stage : request.recipe.spatial) {
    const ModelRecord* record = request.catalog.find(stage.modelId);
    if (record == nullptr) {
      builder.block(QStringLiteral("model.missing"),
                    QStringLiteral("recipe %1 names spatial model %2, which is not in the "
                                   "supplied catalogue; the stage is refused, not skipped")
                        .arg(request.recipe.id, stage.modelId),
                    static_cast<int>(spatialRecords.size()));
      finishUnavailable();
      return plan;
    }
    QString offerKey;
    QString offerReason;
    record->refusal(&offerKey, &offerReason);
    if (!offerKey.isEmpty()) {
      builder.block(offerKey, QStringLiteral("recipe %1: %2").arg(request.recipe.id, offerReason),
                    static_cast<int>(spatialRecords.size()));
      finishUnavailable();
      return plan;
    }
    if (record->role != ModelRole::Spatial) {
      builder.block(QStringLiteral("model.role.mismatch"),
                    QStringLiteral("recipe %1 names model %2 as a spatial model, but it is a %3 "
                                   "model")
                        .arg(request.recipe.id, stage.modelId, stableName(record->role)),
                    static_cast<int>(spatialRecords.size()));
      finishUnavailable();
      return plan;
    }
    if (!record->supportsBackend(request.recipe.backend)) {
      builder.block(QStringLiteral("model.backend.unsupported"),
                    QStringLiteral("recipe %1 asks for model %2 on the %3 backend, which that "
                                   "model does not support")
                        .arg(request.recipe.id, stage.modelId, stableName(request.recipe.backend)),
                    static_cast<int>(spatialRecords.size()));
      finishUnavailable();
      return plan;
    }
    spatialRecords.append(record);
  }

  // The temporal stage names a curated variant, never a catalogue id: the
  // variant is joined to the exact pinned owned graph here, and the plan then
  // carries that graph's own content-addressed id. A missing, corrupt, wrongly
  // pinned or numerically unproved row keeps its own named reason.
  const ModelRecord* temporalRecord = nullptr;
  if (request.recipe.temporal.has_value()) {
    const TemporalStage& temporal = *request.recipe.temporal;
    const RifeModelResolution rife = resolveRifeModel(temporal, request.catalog);
    if (!rife.resolved()) {
      builder.block(rife.reasonKey,
                    QStringLiteral("recipe %1: %2").arg(request.recipe.id, rife.reason),
                    static_cast<int>(spatialRecords.size()));
      finishUnavailable();
      return plan;
    }
    temporalRecord = rife.record;
    if (!temporalRecord->supportsBackend(request.recipe.backend)) {
      builder.block(QStringLiteral("model.backend.unsupported"),
                    QStringLiteral("recipe %1 asks for model %2 on the %3 backend, which that "
                                   "model does not support")
                        .arg(request.recipe.id, temporal.modelId(),
                             stableName(request.recipe.backend)),
                    static_cast<int>(spatialRecords.size()));
      finishUnavailable();
      return plan;
    }
  }

  // ---- the chain, in execution order --------------------------------------
  const TemporalStage* temporal = request.recipe.temporal.has_value()
                                      ? &*request.recipe.temporal
                                      : nullptr;
  const bool temporalFirst = temporal != nullptr && temporal->order == TemporalOrder::BeforeSpatial;

  QSize current = *display;
  Rational cadence = *request.source.fps;

  // When interpolation runs first, the first model's work size is hoisted ahead of
  // it, so RIFE runs at the work geometry and every interpolated frame is upscaled
  // from there. The reference does exactly this and suppresses the model loop's own
  // copy of that resize.
  bool firstPreResizeHoisted = false;
  if (temporalFirst && !spatialRecords.isEmpty()) {
    QSize target;
    QString reasonKey;
    QString reason;
    if (resolvePreResize(request.recipe.spatial.first(), &target, &reasonKey, &reason)) {
      PreResizeStep step;
      step.stageIndex = -1;
      step.modelId = request.recipe.spatial.first().modelId;
      step.from = current;
      step.to = target;
      step.beforeTemporal = true;
      builder.addResize(step);
      current = target;
      firstPreResizeHoisted = true;
    } else if (reasonKey != QLatin1String("stage.preresize.absent")) {
      builder.block(reasonKey, reason);
      finishUnavailable();
      return plan;
    }
  }

  const auto appendTemporalStage = [&](const QSize& at, const Rational& atFps) -> bool {
    // The reference's temporal boundary: an 11-channel input and a 3-channel
    // output, both at the padded geometry. This is declared here rather than read
    // from a profile, because a temporal row's declared I/O is the vsmnet boundary
    // and not the padding the bridge actually binds.
    constexpr int kRifeInputChannels = 11;
    constexpr int kRifeOutputChannels = 3;

    ResolvedStage stage;
    stage.kind = StageKind::Temporal;
    stage.role = ModelRole::Temporal;
    // The resolved owned graph's own id, exactly as for a spatial stage: the
    // curated variant name above is a request, and the plan carries the identity
    // planning, selection and admission all join on downstream.
    stage.modelId = temporalRecord->id;
    stage.recipeIndex = -1;
    stage.inputChannels = kRifeInputChannels;
    stage.outputChannels = kRifeOutputChannels;
    stage.contentGeometry = at;
    stage.inputGeometry = padToTensorAlignment(at, &stage.padL, &stage.padT);
    stage.outputGeometry = stage.inputGeometry;
    stage.inputFps = atFps;
    stage.order = temporal->order;
    stage.factor = temporal->factor;
    stage.sceneThreshold = temporal->sceneThreshold;

    const std::optional<Rational> outFps = scaleCadence(atFps, temporal->factor.num,
                                                        temporal->factor.den);
    if (!outFps.has_value()) {
      builder.block(QStringLiteral("stage.temporal.cadence.overflow"),
                    QStringLiteral("temporal stage %1 at cadence %2 with a factor of %3/%4 does not "
                                   "produce a cadence this build can express")
                        .arg(temporal->modelId(), atFps.toString())
                        .arg(temporal->factor.num)
                        .arg(temporal->factor.den));
      return false;
    }
    stage.outputFps = *outFps;

    stage.declaredInput = temporalRecord->actualProfile.input;
    stage.declaredOutput = temporalRecord->actualProfile.output;
    stage.modelContentId = temporalRecord->contentId();

    if (stage.inputShape().isEmpty() || stage.outputShape().isEmpty()) {
      builder.block(QStringLiteral("stage.temporal.geometry.invalid"),
                    QStringLiteral("temporal stage %1 would pad %2x%3 to a shape this build cannot "
                                   "bind")
                        .arg(temporal->modelId())
                        .arg(at.width())
                        .arg(at.height()));
      return false;
    }
    builder.addStage(stage);
    return true;
  };

  if (temporalFirst) {
    if (!appendTemporalStage(current, cadence)) {
      finishUnavailable();
      return plan;
    }
    // Everything after interpolation runs on the interpolated cadence.
    cadence = builder.stages.last().outputFps;
  }

  for (int i = 0; i < spatialRecords.size(); ++i) {
    const ModelRecord* record = spatialRecords.at(i);
    const SpatialStage& recipeStage = request.recipe.spatial.at(i);

    if (!(i == 0 && firstPreResizeHoisted)) {
      QSize target;
      QString reasonKey;
      QString reason;
      if (resolvePreResize(recipeStage, &target, &reasonKey, &reason)) {
        PreResizeStep step;
        step.stageIndex = builder.stages.size();
        step.modelId = recipeStage.modelId;
        step.from = current;
        step.to = target;
        // Only the hoisted pre-resize runs ahead of interpolation.
        step.beforeTemporal = false;
        builder.addResize(step);
        current = target;
      } else if (reasonKey != QLatin1String("stage.preresize.absent")) {
        builder.block(reasonKey, reason, builder.stages.size());
        finishUnavailable();
        return plan;
      }
    }

    ResolvedStage stage;
    stage.kind = StageKind::Spatial;
    stage.role = ModelRole::Spatial;
    stage.modelId = record->id;
    stage.recipeIndex = i;
    stage.declaredInput = record->actualProfile.input;
    stage.declaredOutput = record->actualProfile.output;
    stage.modelContentId = record->contentId();
    stage.inputFps = cadence;
    stage.outputFps = cadence;

    QString reasonKey;
    QString reason;
    if (!resolveSpatialGeometry(*record, current, request.dynamicResolver, &stage, &reasonKey,
                                &reason)) {
      builder.block(reasonKey, reason, builder.stages.size());
      finishUnavailable();
      return plan;
    }
    if (stage.inputShape().isEmpty() || stage.outputShape().isEmpty()) {
      builder.block(QStringLiteral("stage.geometry.invalid"),
                    QStringLiteral("spatial stage %1 resolves to a shape this build cannot bind")
                        .arg(record->id),
                    builder.stages.size());
      finishUnavailable();
      return plan;
    }
    builder.addStage(stage);
    current = stage.contentGeometry;
  }

  if (temporal != nullptr && !temporalFirst) {
    if (!appendTemporalStage(current, cadence)) {
      finishUnavailable();
      return plan;
    }
  }

  if (builder.stages.isEmpty()) {
    builder.block(QStringLiteral("plan.stages.empty"),
                  QStringLiteral("recipe %1 resolved to no stages, which would reach the core as a "
                                 "configure with nothing to do")
                      .arg(request.recipe.id));
    finishUnavailable();
    return plan;
  }

  if (!builder.blockers.isEmpty()) {
    finishUnavailable();
    return plan;
  }
  if (!builder.unknowns.isEmpty()) {
    finishUnknown();
    return plan;
  }
  finishResolved();
  return plan;
}

}  // namespace native
}  // namespace mplayer
