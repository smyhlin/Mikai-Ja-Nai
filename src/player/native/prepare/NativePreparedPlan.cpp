#include "NativePreparedPlan.hpp"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QStringList>

#include "native/bundle/NativePaths.hpp"

namespace mplayer {
namespace native {

namespace {

constexpr const char* kAdmissionSchema = "mikai.native.admission/1";

void setReason(QString* key, QString* reason, const QString& keyText, const QString& text) {
  if (key != nullptr) *key = keyText;
  if (reason != nullptr) *reason = text;
}

void clearReason(QString* key, QString* reason) {
  if (key != nullptr) key->clear();
  if (reason != nullptr) reason->clear();
}

/// `QDir::cleanPath` splitting, so a drive-letter root is not read as a relative
/// component. Mirrors the builder's own containment walk; no shared public helper
/// exists for it.
bool plainPathNoReparse(const QString& canonical) {
  const QString cleaned = QDir::cleanPath(canonical);
  const QStringList parts = cleaned.split(QLatin1Char('/'), Qt::SkipEmptyParts);
  if (cleaned.size() >= 2 && cleaned.at(1) == QLatin1Char(':')) {
    if (parts.isEmpty()) return false;
    QString current = parts.first() + QLatin1Char('/');
    if (!pathHasNoReparsePoints(current, current)) return false;
    for (int i = 1; i < parts.size(); ++i) {
      current += parts.at(i);
      if (!pathHasNoReparsePoints(current, current)) return false;
      current += QLatin1Char('/');
    }
    return true;
  }
  QString current;
  if (cleaned.startsWith(QLatin1String("//"))) {
    current = QStringLiteral("//");
  } else if (cleaned.startsWith(QLatin1Char('/'))) {
    current = QStringLiteral("/");
  }
  if (!current.isEmpty() && !pathHasNoReparsePoints(current, current)) return false;
  for (const QString& part : parts) {
    current += current.endsWith(QLatin1Char('/')) ? part : QLatin1Char('/') + part;
    if (!pathHasNoReparsePoints(current, current)) return false;
  }
  return true;
}

/// An existing plain directory with no reparse point on the way to it. Empty or
/// non-absolute, a symlink, a file, or a path whose existing part walks through a
/// junction is refused.
bool canonicalOwnedDir(const QString& path, QString* canonicalOut) {
  if (path.isEmpty() || !QDir::isAbsolutePath(path)) return false;
  if (QFileInfo(path).isSymLink()) return false;
  const QString canonical = canonicalPath(path);
  if (canonical.isEmpty() || !QFileInfo(canonical).isDir()) return false;
  if (QFileInfo(canonical).isSymLink()) return false;
  if (!plainPathNoReparse(canonical)) return false;
  if (canonicalOut != nullptr) *canonicalOut = canonical;
  return true;
}

/// An absolute plain file inside `rootCanonical`, reached without a reparse
/// point. Mirrors the builder's owned-copy proof.
bool ownedFileInside(const QString& rootCanonical, const QString& path, QString* canonicalOut) {
  if (path.isEmpty() || !QDir::isAbsolutePath(path)) return false;
  if (QFileInfo(path).isSymLink()) return false;
  const QString canonical = canonicalPath(path);
  if (canonical.isEmpty() || !QFileInfo(canonical).isFile()) return false;
  if (QFileInfo(canonical).isSymLink()) return false;
  if (!pathWithinRoot(rootCanonical, canonical)) return false;
  if (!pathHasNoReparsePoints(rootCanonical, canonical)) return false;
  if (canonicalOut != nullptr) *canonicalOut = canonical;
  return true;
}

QJsonValue jsonInt(qint64 value) { return QJsonValue(value); }

bool sameSource(const SourceGeometry& left, const SourceGeometry& right) {
  return left.codedSize == right.codedSize && left.crop == right.crop &&
         left.rotationDegrees == right.rotationDegrees && left.fps == right.fps &&
         left.csp == right.csp && left.matrix == right.matrix && left.range == right.range &&
         left.siting == right.siting && left.bitDepth == right.bitDepth;
}

bool sameDevice(const DeviceIdentity& left, const DeviceIdentity& right) {
  return isSameDeviceUuid(left.decoderDeviceUuid, right.decoderDeviceUuid) &&
         isSameDeviceUuid(left.computeDeviceUuid, right.computeDeviceUuid) &&
         left.adapterLuid == right.adapterLuid && left.smMajor == right.smMajor &&
         left.smMinor == right.smMinor;
}

bool sameStage(const ManifestStage& recorded, const StageBuildRequest& request) {
  return recorded.role == request.role && recorded.inputShape == request.inputShape &&
         recorded.outputShape == request.outputShape && recorded.ioFormat == request.ioFormat &&
         recorded.padLeft == request.padLeft && recorded.padTop == request.padTop;
}

QString orderWireName(TemporalOrder order) {
  return order == TemporalOrder::BeforeSpatial ? QStringLiteral("before_spatial")
                                               : QStringLiteral("after_spatial");
}

/// The whole plan and machine half of the request: identity facts, the observed
/// device/build agreement, owned roots and the one-entry-per-stage shape.
bool validateRequest(const PreparedAdmissionRequest& request, QString* key, QString* reason) {
  if (request.requestId.isEmpty()) {
    setReason(key, reason, QStringLiteral("admission.requestId.missing"),
              QStringLiteral("the admission request names no request id, so a reply could not be "
                             "matched to it"));
    return false;
  }
  if (!request.plan.canPrepare() || request.plan.id.isEmpty()) {
    setReason(key, reason, QStringLiteral("plan.unresolved"),
              QStringLiteral("the preparation plan is not a resolved, identified plan"));
    return false;
  }
  if (request.plan.stages.isEmpty()) {
    setReason(key, reason, QStringLiteral("plan.stages.empty"),
              QStringLiteral("the plan resolved to no stages, so there is nothing to admit"));
    return false;
  }
  if (request.recipeId.isEmpty() || request.plan.recipeId != request.recipeId) {
    setReason(key, reason, QStringLiteral("plan.recipe.mismatch"),
              QStringLiteral("the plan was resolved for recipe '%1', not the requested '%2'")
                  .arg(request.plan.recipeId, request.recipeId));
    return false;
  }
  if (request.plan.backend != request.backend) {
    setReason(key, reason, QStringLiteral("plan.backend.mismatch"),
              QStringLiteral("the plan was resolved for the %1 backend, not the requested %2")
                  .arg(stableName(request.plan.backend), stableName(request.backend)));
    return false;
  }
  if (!sameSource(request.plan.source, request.source)) {
    setReason(key, reason, QStringLiteral("plan.source.mismatch"),
              QStringLiteral("the observed source is not the source the plan was resolved "
                             "against"));
    return false;
  }
  if (!sameDevice(request.plan.device, request.device)) {
    setReason(key, reason, QStringLiteral("plan.device.mismatch"),
              QStringLiteral("the observed device is not the device the plan was resolved "
                             "against"));
    return false;
  }
  if (!request.device.isKnown()) {
    setReason(key, reason, QStringLiteral("plan.device.unknown"),
              QStringLiteral("the plan names no fully observed decoder/compute device"));
    return false;
  }
  const QString missing = request.build.firstMissingRequiredField();
  if (!missing.isEmpty()) {
    setReason(key, reason, QStringLiteral("request.incomplete"),
              QStringLiteral("the observed build identity records no %1").arg(missing));
    return false;
  }
  if (request.build.provider != request.plan.backend) {
    setReason(key, reason, QStringLiteral("build.provider.mismatch"),
              QStringLiteral("the observed provider %1 is not the planned backend %2")
                  .arg(stableName(request.build.provider), stableName(request.plan.backend)));
    return false;
  }
  if (!isSameDeviceUuid(request.build.deviceUuid, request.device.computeDeviceUuid) ||
      request.build.smMajor != request.device.smMajor ||
      request.build.smMinor != request.device.smMinor) {
    setReason(key, reason, QStringLiteral("build.device.mismatch"),
              QStringLiteral("the observed build device is not the planned compute device"));
    return false;
  }
  const QString sensitive = request.build.firstSensitiveField();
  if (!sensitive.isEmpty()) {
    setReason(key, reason, QStringLiteral("build.sensitiveData"),
              QStringLiteral("the observed build identity records %1, which is not a runtime, "
                             "plugin or device value")
                  .arg(sensitive));
    return false;
  }
  QString storeWhy;
  if (!request.store.isOwned(&storeWhy)) {
    setReason(key, reason, QStringLiteral("store.unowned"),
              storeWhy.isEmpty() ? QStringLiteral("the artifact store is not app-owned") : storeWhy);
    return false;
  }
  if (!canonicalOwnedDir(request.modelRoot, nullptr)) {
    setReason(key, reason, QStringLiteral("model.root.unowned"),
              QStringLiteral("the models root is not a plain owned directory"));
    return false;
  }
  if (request.models.size() != request.plan.stages.size()) {
    setReason(key, reason, QStringLiteral("model.count.mismatch"),
              QStringLiteral("the plan has %1 stages but %2 owned model rows")
                  .arg(request.plan.stages.size())
                  .arg(request.models.size()));
    return false;
  }
  return true;
}

/// Replays the plan's own ordering rules over the resolved stages: one spatial
/// stage per recipe index in order, at most one temporal stage placed exactly
/// where its order says, pre-resizes applied where they say, unpadded content
/// geometry threaded stage to stage, temporal tensors padded to the alignment
/// multiple, and cadence carried through unchanged except at interpolation.
bool validatePlanOrder(const PreparedAdmissionRequest& request, QString* key, QString* reason) {
  const std::optional<QSize> display = request.source.displaySize();
  if (!display.has_value()) {
    setReason(key, reason, QStringLiteral("source.geometry.unknown"),
              QStringLiteral("the observed source has no display geometry"));
    return false;
  }
  if (!request.source.fps.has_value() || !request.source.fps->isValid()) {
    setReason(key, reason, QStringLiteral("source.cadence.unknown"),
              QStringLiteral("the observed source has no cadence"));
    return false;
  }

  const int last = request.plan.stages.size() - 1;
  QSize current = *display;
  Rational cadence = *request.source.fps;
  int spatialRecipe = 0;
  int temporalStages = 0;

  for (int i = 0; i <= last; ++i) {
    const ResolvedStage& stage = request.plan.stages.at(i);

    // A pre-resize the plan records applies before the stage it names; the one
    // hoisted resize applies before the first stage.
    for (const PreResizeStep& resize : request.plan.resizes) {
      if (resize.stageIndex == -1 && i == 0 && resize.beforeTemporal) {
        current = resize.to;
      } else if (resize.stageIndex == i) {
        current = resize.to;
      }
    }

    if (stage.kind == StageKind::Spatial) {
      if (stage.role != ModelRole::Spatial) {
        setReason(key, reason, QStringLiteral("plan.role.mismatch"),
                  QStringLiteral("spatial stage %1 carries the %2 role")
                      .arg(stage.modelId, stableName(stage.role)));
        return false;
      }
      if (stage.recipeIndex != spatialRecipe) {
        setReason(key, reason, QStringLiteral("plan.order.mismatch"),
                  QStringLiteral("spatial stage %1 carries recipe index %2 where %3 was expected")
                      .arg(stage.modelId)
                      .arg(stage.recipeIndex)
                      .arg(spatialRecipe));
        return false;
      }
      ++spatialRecipe;
      if (stage.inputGeometry != current) {
        setReason(key, reason, QStringLiteral("plan.geometry.mismatch"),
                  QStringLiteral("spatial stage %1 runs at %2x%3 but the chain reaches it at "
                                 "%4x%5")
                      .arg(stage.modelId)
                      .arg(stage.inputGeometry.width())
                      .arg(stage.inputGeometry.height())
                      .arg(current.width())
                      .arg(current.height()));
        return false;
      }
      if (stage.contentGeometry != stage.outputGeometry) {
        setReason(key, reason, QStringLiteral("plan.geometry.mismatch"),
                  QStringLiteral("spatial stage %1 does not leave its output geometry as its "
                                 "content geometry")
                      .arg(stage.modelId));
        return false;
      }
      if (stage.padL != 0 || stage.padT != 0) {
        setReason(key, reason, QStringLiteral("plan.padding.mismatch"),
                  QStringLiteral("spatial stage %1 is padded, but a spatial stage is built for "
                                 "its exact work shape")
                      .arg(stage.modelId));
        return false;
      }
      if (stage.inputFps != cadence || stage.outputFps != cadence) {
        setReason(key, reason, QStringLiteral("plan.cadence.mismatch"),
                  QStringLiteral("spatial stage %1 does not carry the chain cadence through")
                      .arg(stage.modelId));
        return false;
      }
      const std::optional<int> scale = uniformIntegerScale(stage.inputGeometry, stage.outputGeometry);
      if (!scale.has_value() || !stage.scale.has_value() || *scale != *stage.scale) {
        setReason(key, reason, QStringLiteral("plan.scale.mismatch"),
                  QStringLiteral("spatial stage %1 does not resolve to the uniform scale it "
                                 "records")
                      .arg(stage.modelId));
        return false;
      }
    } else {
      if (stage.role != ModelRole::Temporal) {
        setReason(key, reason, QStringLiteral("plan.role.mismatch"),
                  QStringLiteral("temporal stage %1 carries the %2 role")
                      .arg(stage.modelId, stableName(stage.role)));
        return false;
      }
      if (stage.recipeIndex != -1) {
        setReason(key, reason, QStringLiteral("plan.order.mismatch"),
                  QStringLiteral("temporal stage %1 carries a recipe index").arg(stage.modelId));
        return false;
      }
      if ((stage.order == TemporalOrder::BeforeSpatial && i != 0) ||
          (stage.order == TemporalOrder::AfterSpatial && i != last)) {
        setReason(key, reason, QStringLiteral("plan.order.mismatch"),
                  QStringLiteral("temporal stage %1 is not placed where its order says")
                      .arg(stage.modelId));
        return false;
      }
      if (++temporalStages > 1) {
        setReason(key, reason, QStringLiteral("plan.order.mismatch"),
                  QStringLiteral("the plan carries more than one temporal stage"));
        return false;
      }
      if (!stage.factor.isValid()) {
        setReason(key, reason, QStringLiteral("plan.factor.invalid"),
                  QStringLiteral("temporal stage %1 records a non-positive factor")
                      .arg(stage.modelId));
        return false;
      }
      if (stage.sceneThreshold < 0.0 || stage.sceneThreshold > 1.0) {
        setReason(key, reason, QStringLiteral("plan.threshold.invalid"),
                  QStringLiteral("temporal stage %1 records a scene threshold outside [0, 1]")
                      .arg(stage.modelId));
        return false;
      }
      if (stage.contentGeometry != current) {
        setReason(key, reason, QStringLiteral("plan.geometry.mismatch"),
                  QStringLiteral("temporal stage %1 interpolates %2x%3 but the chain reaches it "
                                 "at %4x%5")
                      .arg(stage.modelId)
                      .arg(stage.contentGeometry.width())
                      .arg(stage.contentGeometry.height())
                      .arg(current.width())
                      .arg(current.height()));
        return false;
      }
      int padL = -1;
      int padT = -1;
      const QSize padded = padToTensorAlignment(current, &padL, &padT);
      if (stage.inputGeometry != padded || stage.outputGeometry != padded ||
          stage.padL != padL || stage.padT != padT) {
        setReason(key, reason, QStringLiteral("plan.padding.mismatch"),
                  QStringLiteral("temporal stage %1 is not padded to the alignment multiple at "
                                 "%2x%3")
                      .arg(stage.modelId)
                      .arg(current.width())
                      .arg(current.height()));
        return false;
      }
      if (stage.inputFps != cadence) {
        setReason(key, reason, QStringLiteral("plan.cadence.mismatch"),
                  QStringLiteral("temporal stage %1 does not interpolate the chain cadence")
                      .arg(stage.modelId));
        return false;
      }
      const std::optional<Rational> out = scaleCadence(cadence, stage.factor.num, stage.factor.den);
      if (!out.has_value() || stage.outputFps != *out) {
        setReason(key, reason, QStringLiteral("plan.cadence.mismatch"),
                  QStringLiteral("temporal stage %1's output cadence is not its input cadence "
                                 "scaled by its factor")
                      .arg(stage.modelId));
        return false;
      }
    }

    if (stage.inputChannels <= 0 || stage.outputChannels <= 0) {
      setReason(key, reason, QStringLiteral("plan.channels.invalid"),
                QStringLiteral("stage %1 declares a non-positive channel count").arg(stage.modelId));
      return false;
    }
    current = stage.contentGeometry;
    cadence = stage.outputFps;
  }

  const std::optional<QSize> output = request.plan.outputGeometry();
  if (!output.has_value() || *output != current) {
    setReason(key, reason, QStringLiteral("plan.geometry.mismatch"),
              QStringLiteral("the plan's output geometry is not the chain's end geometry"));
    return false;
  }
  const std::optional<Rational> outputCadence = request.plan.outputCadence();
  if (!outputCadence.has_value() || *outputCadence != cadence) {
    setReason(key, reason, QStringLiteral("plan.cadence.mismatch"),
              QStringLiteral("the plan's output cadence is not the chain's end cadence"));
    return false;
  }
  return true;
}

/// The owned copy behind a stage: inside the owned root, a plain file, still
/// hashing to the record's digest and size, and still deriving the record's id.
bool proveOwnedModel(const QString& rootCanonical, const ModelRecord& record,
                     QString* canonicalOut, QString* key, QString* reason) {
  if (record.onnxPath.isEmpty()) {
    setReason(key, reason, QStringLiteral("model.owned.missing"),
              QStringLiteral("model %1 names no owned copy").arg(record.id));
    return false;
  }
  if (!isSha256Hex(record.onnxSha256)) {
    setReason(key, reason, QStringLiteral("model.identity.missing"),
              QStringLiteral("model %1 has no digest to compare against").arg(record.id));
    return false;
  }
  QString canonical;
  if (!ownedFileInside(rootCanonical, record.onnxPath, &canonical)) {
    setReason(key, reason, QStringLiteral("model.owned.missing"),
              QStringLiteral("model %1's copy is not a plain file inside the owned models root")
                  .arg(record.id));
    return false;
  }
  QString measureReason;
  const std::optional<FileDigest> measured = measureFile(canonical, &measureReason);
  if (!measured.has_value()) {
    setReason(key, reason, QStringLiteral("model.owned.changed"), measureReason);
    return false;
  }
  if (measured->sha256.toLower() != record.onnxSha256.toLower() ||
      measured->bytes != record.onnxBytes) {
    setReason(key, reason, QStringLiteral("model.owned.changed"),
              QStringLiteral("model %1's owned copy no longer matches its recorded bytes")
                  .arg(record.id));
    return false;
  }
  const QString derivedId = userModelIdFor(
      computeContentId(record.role, record.onnxSha256, record.actualProfile));
  if (derivedId != record.id) {
    setReason(key, reason, QStringLiteral("model.identity.changed"),
              QStringLiteral("model %1's id no longer derives from its digest and declared "
                             "profile")
                  .arg(record.id));
    return false;
  }
  if (canonicalOut != nullptr) *canonicalOut = canonical;
  return true;
}

/// The canonical pre-build request for one resolved stage, built only from the
/// existing identities: the row's graph content identity, the stage's padded
/// shapes and the observed build identity. No alternate key scheme is applied.
bool buildRequestForStage(const ResolvedStage& stage, const ModelRecord& record,
                          const EngineBuildIdentity& build, BuildRequest* out, QString* key,
                          QString* reason) {
  if (record.role != stage.role) {
    setReason(key, reason, QStringLiteral("model.role.mismatch"),
              QStringLiteral("stage %1 is a %2 stage but its owned row is a %3 model")
                  .arg(stage.modelId, stableName(stage.role), stableName(record.role)));
    return false;
  }
  if (record.contentId() != stage.modelContentId ||
      graphIdentityOf(record).contentId() != stage.modelContentId) {
    setReason(key, reason, QStringLiteral("model.content.mismatch"),
              QStringLiteral("the owned row for stage %1 is not the planned graph")
                  .arg(stage.modelId));
    return false;
  }
  if (record.actualProfile.input != stage.declaredInput ||
      record.actualProfile.output != stage.declaredOutput) {
    setReason(key, reason, QStringLiteral("model.profile.mismatch"),
              QStringLiteral("the owned row for stage %1 declares a different boundary than the "
                             "planned stage")
                  .arg(stage.modelId));
    return false;
  }

  StageBuildRequest stageRequest;
  stageRequest.role = stage.role;
  stageRequest.inputShape = stage.inputShape();
  stageRequest.outputShape = stage.outputShape();
  stageRequest.ioFormat = stage.ioFormats();
  stageRequest.padLeft = stage.padL;
  stageRequest.padTop = stage.padT;
  if (stageRequest.inputShape.isEmpty() || stageRequest.outputShape.isEmpty() ||
      !stageRequest.isComplete()) {
    setReason(key, reason, QStringLiteral("stage.geometry.invalid"),
              QStringLiteral("stage %1's shape cannot be bound").arg(stage.modelId));
    return false;
  }

  BuildRequest built;
  built.graph = graphIdentityOf(record);
  built.stage = stageRequest;
  built.build = build;
  const QString missing = built.firstMissingRequiredField();
  if (!missing.isEmpty()) {
    setReason(key, reason, QStringLiteral("request.incomplete"),
              QStringLiteral("the build request for stage %1 records no %2")
                  .arg(stage.modelId, missing));
    return false;
  }
  *out = built;
  return true;
}

QJsonObject jsonSource(const SourceGeometry& source) {
  QJsonObject object;
  if (source.codedSize.has_value()) {
    object.insert(QStringLiteral("codedWidth"), jsonInt(source.codedSize->width()));
    object.insert(QStringLiteral("codedHeight"), jsonInt(source.codedSize->height()));
  }
  if (source.crop.has_value()) {
    QJsonObject crop;
    crop.insert(QStringLiteral("x"), jsonInt(source.crop->x()));
    crop.insert(QStringLiteral("y"), jsonInt(source.crop->y()));
    crop.insert(QStringLiteral("width"), jsonInt(source.crop->width()));
    crop.insert(QStringLiteral("height"), jsonInt(source.crop->height()));
    object.insert(QStringLiteral("crop"), crop);
  }
  if (source.rotationDegrees.has_value()) {
    object.insert(QStringLiteral("rotation"), jsonInt(*source.rotationDegrees));
  }
  if (source.fps.has_value()) {
    QJsonObject fps;
    fps.insert(QStringLiteral("num"), jsonInt(source.fps->num));
    fps.insert(QStringLiteral("den"), jsonInt(source.fps->den));
    object.insert(QStringLiteral("fps"), fps);
  }
  object.insert(QStringLiteral("csp"), source.csp);
  object.insert(QStringLiteral("matrix"), source.matrix);
  object.insert(QStringLiteral("range"), source.range);
  object.insert(QStringLiteral("siting"), source.siting);
  object.insert(QStringLiteral("bitDepth"), jsonInt(source.bitDepth));
  return object;
}

QJsonObject jsonDevice(const DeviceIdentity& device) {
  QJsonObject object;
  object.insert(QStringLiteral("decoderDeviceUuid"), device.decoderDeviceUuid);
  object.insert(QStringLiteral("computeDeviceUuid"), device.computeDeviceUuid);
  object.insert(QStringLiteral("adapterLuid"), device.adapterLuid);
  object.insert(QStringLiteral("smMajor"), jsonInt(device.smMajor));
  object.insert(QStringLiteral("smMinor"), jsonInt(device.smMinor));
  return object;
}

QJsonObject jsonBuild(const EngineBuildIdentity& build) {
  QJsonObject object;
  object.insert(QStringLiteral("provider"), stableName(build.provider));
  object.insert(QStringLiteral("runtimeVersion"), build.runtimeVersion);
  object.insert(QStringLiteral("pluginVersion"), build.pluginVersion);
  object.insert(QStringLiteral("coreSha"), build.coreSha);
  object.insert(QStringLiteral("sourceAbi"), jsonInt(build.sourceAbi));
  object.insert(QStringLiteral("precision"), stableName(build.precision));
  object.insert(QStringLiteral("tf32"), build.tf32);
  object.insert(QStringLiteral("stronglyTyped"), build.stronglyTyped);
  object.insert(QStringLiteral("skipInference"), build.skipInference);
  object.insert(QStringLiteral("deviceUuid"), build.deviceUuid);
  object.insert(QStringLiteral("adapterLuid"), build.adapterLuid);
  object.insert(QStringLiteral("smMajor"), jsonInt(build.smMajor));
  object.insert(QStringLiteral("smMinor"), jsonInt(build.smMinor));
  object.insert(QStringLiteral("cudaVersion"), build.cudaVersion);
  object.insert(QStringLiteral("driverVersion"), build.driverVersion);
  object.insert(QStringLiteral("canonicalOptions"), build.canonicalOptions());
  return object;
}

QJsonArray jsonShape(int channels, const QSize& size) {
  QJsonArray array;
  array.append(jsonInt(1));
  array.append(jsonInt(channels));
  array.append(jsonInt(size.height()));
  array.append(jsonInt(size.width()));
  return array;
}

QJsonObject buildAdmissionCommand(const PreparedAdmissionRequest& request,
                                  const QVector<BuildRequest>& builds,
                                  const QVector<ArtifactRecord>& artifacts,
                                  const QVector<QString>& modelPaths,
                                  const QVector<QString>& dtypes) {
  QJsonObject command;
  command.insert(QStringLiteral("schema"), preparedAdmissionSchema());
  command.insert(QStringLiteral("protocol"), jsonInt(kPreparedAdmissionProtocol));
  command.insert(QStringLiteral("requestId"), request.requestId);
  command.insert(QStringLiteral("revision"), QJsonValue(static_cast<qint64>(request.revision)));
  command.insert(QStringLiteral("epoch"), QJsonValue(static_cast<qint64>(request.epoch)));

  QJsonObject requested;
  requested.insert(QStringLiteral("recipe"), request.recipeId);
  requested.insert(QStringLiteral("backend"), stableName(request.backend));
  command.insert(QStringLiteral("requested"), requested);

  command.insert(QStringLiteral("source"), jsonSource(request.source));
  command.insert(QStringLiteral("device"), jsonDevice(request.device));
  command.insert(QStringLiteral("build"), jsonBuild(request.build));

  QJsonArray stages;
  for (int i = 0; i < request.plan.stages.size(); ++i) {
    const ResolvedStage& stage = request.plan.stages.at(i);
    const ArtifactRecord& artifact = artifacts.at(i);
    QJsonObject entry;
    entry.insert(QStringLiteral("ordinal"), jsonInt(i));
    entry.insert(QStringLiteral("role"), stableName(stage.role));
    if (stage.kind == StageKind::Temporal) {
      entry.insert(QStringLiteral("order"), orderWireName(stage.order));
    }
    entry.insert(QStringLiteral("modelId"), stage.modelId);
    entry.insert(QStringLiteral("modelContentId"), stage.modelContentId);
    entry.insert(QStringLiteral("modelSha256"), artifact.manifest.source.onnxSha256);
    entry.insert(QStringLiteral("engineSha256"), artifact.manifest.engineSha256);
    entry.insert(QStringLiteral("buildKey"), builds.at(i).buildKey());
    entry.insert(QStringLiteral("modelPath"), modelPaths.at(i));
    entry.insert(QStringLiteral("enginePath"), artifact.enginePath);
    // The owned store persists no tensor names. The producer contract defines an
    // empty binding name as "discover from the validated engine", so nothing is
    // invented here; a caller that knows the names supplies them nowhere the
    // store records.
    entry.insert(QStringLiteral("inputTensor"), QString());
    entry.insert(QStringLiteral("outputTensor"), QString());
    entry.insert(QStringLiteral("inputDtype"), dtypes.at(i));
    entry.insert(QStringLiteral("outputDtype"), dtypes.at(i));
    entry.insert(QStringLiteral("layout"), QStringLiteral("chw"));
    entry.insert(QStringLiteral("inputShape"), jsonShape(stage.inputChannels, stage.inputGeometry));
    entry.insert(QStringLiteral("outputShape"),
                 jsonShape(stage.outputChannels, stage.outputGeometry));
    QJsonObject content;
    content.insert(QStringLiteral("w"), jsonInt(stage.contentGeometry.width()));
    content.insert(QStringLiteral("h"), jsonInt(stage.contentGeometry.height()));
    entry.insert(QStringLiteral("contentSize"), content);
    entry.insert(QStringLiteral("padLeft"), jsonInt(stage.padL));
    entry.insert(QStringLiteral("padTop"), jsonInt(stage.padT));
    if (stage.kind == StageKind::Temporal) {
      QJsonObject factor;
      factor.insert(QStringLiteral("num"), jsonInt(stage.factor.num));
      factor.insert(QStringLiteral("den"), jsonInt(stage.factor.den));
      entry.insert(QStringLiteral("factor"), factor);
      entry.insert(QStringLiteral("sceneThreshold"), stage.sceneThreshold);
    }
    stages.append(entry);
  }
  command.insert(QStringLiteral("stages"), stages);
  return command;
}

}  // namespace

QString preparedAdmissionSchema() { return QString::fromLatin1(kAdmissionSchema); }

std::optional<PreparedAdmission> resolvePreparedAdmission(const PreparedAdmissionRequest& request,
                                                          QString* reasonKey, QString* reason) {
  clearReason(reasonKey, reason);

  if (!validateRequest(request, reasonKey, reason)) return std::nullopt;
  if (!validatePlanOrder(request, reasonKey, reason)) return std::nullopt;

  QString rootCanonical;
  if (!canonicalOwnedDir(request.modelRoot, &rootCanonical)) {
    setReason(reasonKey, reason, QStringLiteral("model.root.unowned"),
              QStringLiteral("the models root is not a plain owned directory"));
    return std::nullopt;
  }

  const int count = request.plan.stages.size();
  QVector<BuildRequest> builds;
  QVector<ArtifactRecord> artifacts;
  QVector<QString> modelPaths;
  QVector<QString> dtypes;
  // Held locally until the whole plan resolves: any refusal below destroys these
  // and releases every claim taken so far.
  QVector<ArtifactLease> leases;
  builds.reserve(count);
  artifacts.reserve(count);
  modelPaths.reserve(count);
  dtypes.reserve(count);
  leases.reserve(count);

  for (int i = 0; i < count; ++i) {
    const ResolvedStage& stage = request.plan.stages.at(i);
    const ModelRecord& record = request.models.at(i);

    if (record.id != stage.modelId) {
      setReason(reasonKey, reason, QStringLiteral("model.mismatch"),
                QStringLiteral("the owned row at stage %1 is not the planned model").arg(i));
      return std::nullopt;
    }

    QString modelPath;
    if (!proveOwnedModel(rootCanonical, record, &modelPath, reasonKey, reason)) {
      return std::nullopt;
    }

    BuildRequest built;
    if (!buildRequestForStage(stage, record, request.build, &built, reasonKey, reason)) {
      return std::nullopt;
    }

    if (stage.declaredInput.dtype.isEmpty() || stage.declaredOutput.dtype.isEmpty()) {
      setReason(reasonKey, reason, QStringLiteral("stage.dtype.unknown"),
                QStringLiteral("stage %1 declares no tensor dtype").arg(stage.modelId));
      return std::nullopt;
    }
    if (stage.declaredInput.dtype != QLatin1String(kEligibleIoType) ||
        stage.declaredOutput.dtype != QLatin1String(kEligibleIoType)) {
      setReason(reasonKey, reason, QStringLiteral("stage.dtype.unsupported"),
                QStringLiteral("stage %1 declares %2/%3, but this bridge binds %4 only")
                    .arg(stage.modelId, stage.declaredInput.dtype, stage.declaredOutput.dtype,
                         QString::fromLatin1(kEligibleIoType)));
      return std::nullopt;
    }

    const std::optional<ArtifactRecord> found =
        findArtifactForRequest(request.store, built, reasonKey, reason);
    if (!found.has_value()) return std::nullopt;
    const ArtifactRecord& artifact = *found;

    // The manifest must describe exactly the stage and graph this request names,
    // not merely share a build key with them.
    if (artifact.manifest.buildKey != built.buildKey()) {
      setReason(reasonKey, reason, QStringLiteral("artifact.declared.mismatch"),
                QStringLiteral("the artifact for stage %1 records another build key")
                    .arg(stage.modelId));
      return std::nullopt;
    }
    if (!(artifact.manifest.graphIdentity() == built.graph)) {
      setReason(reasonKey, reason, QStringLiteral("artifact.graph.mismatch"),
                QStringLiteral("the artifact for stage %1 records another graph identity")
                    .arg(stage.modelId));
      return std::nullopt;
    }
    if (!sameStage(artifact.manifest.stage, built.stage)) {
      setReason(reasonKey, reason, QStringLiteral("artifact.stage.mismatch"),
                QStringLiteral("the artifact for stage %1 records another prepared stage")
                    .arg(stage.modelId));
      return std::nullopt;
    }
    if (artifact.manifest.source.modelContentId != stage.modelContentId) {
      setReason(reasonKey, reason, QStringLiteral("artifact.content.mismatch"),
                QStringLiteral("the artifact for stage %1 names another model content id")
                    .arg(stage.modelId));
      return std::nullopt;
    }
    if (artifact.manifest.source.onnxSha256.toLower() != record.onnxSha256.toLower()) {
      setReason(reasonKey, reason, QStringLiteral("artifact.source.mismatch"),
                QStringLiteral("the artifact for stage %1 was built from another graph digest")
                    .arg(stage.modelId));
      return std::nullopt;
    }
    if (!isSha256Hex(artifact.manifest.engineSha256) ||
        artifact.manifest.engineSha256 != artifact.manifest.engine.sha256) {
      setReason(reasonKey, reason, QStringLiteral("artifact.engineDigest.missing"),
                QStringLiteral("the artifact for stage %1 records no usable engine digest")
                    .arg(stage.modelId));
      return std::nullopt;
    }

    // The observed machine now must still be the machine the engine was built
    // for, then the source bytes, then the record itself re-proved on disk.
    if (!verifyArtifactRuntime(artifact, request.build, reasonKey, reason)) return std::nullopt;
    if (!verifyArtifactSource(artifact, modelPath, reasonKey, reason)) return std::nullopt;
    if (!verifyArtifact(request.store, artifact, reasonKey, reason)) return std::nullopt;

    const std::optional<ArtifactLease> lease =
        acquireArtifactLease(request.store, built.buildKey(), reasonKey, reason);
    if (!lease.has_value()) return std::nullopt;

    builds.append(built);
    artifacts.append(artifact);
    modelPaths.append(modelPath);
    dtypes.append(stage.declaredInput.dtype);
    leases.append(*lease);
  }

  PreparedAdmission admission;
  admission.artifacts = artifacts;
  admission.leases = leases;
  admission.command = buildAdmissionCommand(request, builds, artifacts, modelPaths, dtypes);
  clearReason(reasonKey, reason);
  return admission;
}

}  // namespace native
}  // namespace mplayer
