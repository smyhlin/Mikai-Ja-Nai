#include "NativeBuilderInternal.hpp"

#include <QtCore/QTemporaryDir>

#include <optional>

#include <algorithm>

namespace mplayer {
namespace native {

using namespace builder_detail;

namespace {

struct PrepProduct {
  MeasuredPrep measured;
  QString engineSha;
  qint64 engineBytes = 0;
};

bool parsePrepStream(const QByteArray& output, MeasuredPrep* measured, bool* sawDevice,
                     bool* sawParsed, bool* sawStarted, bool* sawSerialized,
                     bool* sawRaw, QString* key, QString* reason) {
  *sawDevice = false;
  *sawParsed = false;
  *sawStarted = false;
  *sawSerialized = false;
  *sawRaw = false;
  const QList<QByteArray> lines = output.split('\n');
  for (const QByteArray& rawLine : lines) {
    const QByteArray line = rawLine.trimmed();
    if (line.isEmpty()) {
      continue;
    }
    QJsonObject event;
    if (!parseObject(line, &event)) {
      if (key != nullptr) {
        *key = QStringLiteral("build.event.json");
      }
      if (reason != nullptr) {
        *reason = QStringLiteral("a prep event is not JSON");
      }
      return false;
    }
    const QString name = event.value(QStringLiteral("event")).toString();
    if (name == QLatin1String("device-selected")) {
      if (*sawDevice) {
        if (key != nullptr) {
          *key = QStringLiteral("build.event.duplicate");
        }
        if (reason != nullptr) {
          *reason = QStringLiteral("device-selected appears twice");
        }
        return false;
      }
      *sawDevice = true;
      if (!parseDeviceEvent(event, measured, key, reason)) {
        return false;
      }
    } else if (name == QLatin1String("graph-parsed")) {
      if (*sawParsed) {
        if (key != nullptr) {
          *key = QStringLiteral("build.event.duplicate");
        }
        if (reason != nullptr) {
          *reason = QStringLiteral("graph-parsed appears twice");
        }
        return false;
      }
      *sawParsed = true;
      if (!parseGraphEvent(event, measured, key, reason)) {
        return false;
      }
    } else if (name == QLatin1String("build-started")) {
      if (*sawStarted) {
        if (key != nullptr) {
          *key = QStringLiteral("build.event.duplicate");
        }
        if (reason != nullptr) {
          *reason = QStringLiteral("build-started appears twice");
        }
        return false;
      }
      *sawStarted = true;
      if (!parseStartedEvent(event, measured, key, reason)) {
        return false;
      }
    } else if (name == QLatin1String("serialized")) {
      *sawSerialized = true;
    } else if (name == QLatin1String("raw-produced")) {
      if (*sawRaw) {
        if (key != nullptr) {
          *key = QStringLiteral("build.event.duplicate");
        }
        if (reason != nullptr) {
          *reason = QStringLiteral("raw-produced appears twice");
        }
        return false;
      }
      *sawRaw = true;
      if (!parseRawEvent(event, measured, key, reason)) {
        return false;
      }
    } else {
      if (key != nullptr) {
        *key = QStringLiteral("build.event.unknown");
      }
      if (reason != nullptr) {
        *reason = QString(QStringLiteral("unknown prep event ")) + name;
      }
      return false;
    }
  }
  return true;
}

bool prepTypeIsFp16(const QString& type) {
  return type == QLatin1String("float16") || type == QLatin1String("fp16");
}

bool checkParsedBindings(const MeasuredPrep& measured, const StageBuildRequest& stage,
                         const QString& exportInName, const QString& exportInShape,
                         const QString& exportOutShape, QString* key, QString* reason) {
  auto fail = [&](const char* k, const QString& t) {
    if (key != nullptr) {
      *key = QString::fromLatin1(k);
    }
    if (reason != nullptr) {
      *reason = t;
    }
    return false;
  };
  if (measured.inName != exportInName) {
    return fail("build.binding.mismatch",
                QStringLiteral("the parsed input is not the reference input"));
  }
  if (!prepTypeIsFp16(measured.inType) || !prepTypeIsFp16(measured.outType)) {
    return fail("build.binding.dtype",
                QStringLiteral("the parsed graph is not float16 on both tensors"));
  }
  qint64 wantIn[4] = {0, 0, 0, 0};
  qint64 wantOut[4] = {0, 0, 0, 0};
  if (!shapeDims(stage.inputShape, wantIn) || !shapeDims(stage.outputShape, wantOut)) {
    return fail("stage.geometry.invalid",
                QStringLiteral("the stage shape is not NxCxHxW"));
  }
  const qint64* declaredIn[2] = {measured.inDims, measured.outDims};
  const qint64* want[2] = {wantIn, wantOut};
  for (int t = 0; t < 2; ++t) {
    if (declaredIn[t][0] != 1) {
      return fail("build.binding.batch",
                  QStringLiteral("the parsed graph batch is not 1"));
    }
    const int wantChannels = (t == 0) ? want[0][1] : want[1][1];
    if (declaredIn[t][1] != wantChannels) {
      return fail("build.binding.channels",
                  QStringLiteral("the parsed channels are not the stage channels"));
    }
    for (int a = 2; a < 4; ++a) {
      if (declaredIn[t][a] != -1 && declaredIn[t][a] != want[t][a]) {
        return fail("build.binding.shape",
                    QStringLiteral("a static parsed extent is not the stage extent"));
      }
    }
  }
  qint64 exportIn[4] = {0, 0, 0, 0};
  qint64 exportOut[4] = {0, 0, 0, 0};
  if (!shapeDims(exportInShape, exportIn) || !shapeDims(exportOutShape, exportOut)) {
    return fail("reference.inputs.shape",
                QStringLiteral("a reference shape is not NxCxHxW"));
  }
  for (int a = 0; a < 4; ++a) {
    if (exportIn[a] != wantIn[a] || exportOut[a] != wantOut[a]) {
      return fail("reference.geometry.mismatch",
                  QStringLiteral("a reference tensor is not the stage shape"));
    }
  }
  return true;
}

bool runPrep(const ResolvedTools& tools, const QString& storedCanonical,
             const StageBuildRequest& stage, const EngineBuildIdentity& build,
             const QString& inName, const QString& exportInShape,
             const QString& exportOutShape, const QString& engineOut, quint64 workspaceRequest,
             int index, const QString& modelId, NativeBuildCancellation* cancellation,
             const BuilderProgressCallback& progress, PrepProduct* out, BuilderResult* bad) {
  QFile::remove(engineOut);
  const QString shapesArg = inName + QStringLiteral(":") + stage.inputShape;
  QStringList args;
  args.append(QStringLiteral("--onnx"));
  args.append(storedCanonical);
  args.append(QStringLiteral("--out"));
  args.append(engineOut);
  args.append(QStringLiteral("--shapes"));
  args.append(shapesArg);
  args.append(QStringLiteral("--device-uuid"));
  args.append(build.deviceUuid);
  int optimizationLevel = 0;
  QString levelKey;
  QString levelReason;
  if (!declaredOptimizationLevel(build, &optimizationLevel, &levelKey, &levelReason)) {
    *bad = refuse(levelKey, levelReason, index, QStringLiteral("build"));
    return false;
  }
  args.append(QStringLiteral("--builder-optimization-level"));
  args.append(QString::number(optimizationLevel));
  if (workspaceRequest != 0) {
    args.append(QStringLiteral("--workspace-bytes"));
    args.append(QString::number(workspaceRequest));
  }
  ToolResult run;
  const bool exited =
      runBlocking(tools.paths.prep, args, tools.prepVerifyEnv, cancellation, progress,
                  index, modelId, QStringLiteral("build"), &run);
  if (!exited) {
    if (isCancelled(cancellation) && run.launchError.isEmpty()) {
      *bad = cancelledResult(QStringLiteral("build"), index);
      return false;
    }
    if (!run.stopped && isCancelled(cancellation)) {
      *bad = refuse(QStringLiteral("build.cancel.stop"),
                    QStringLiteral("the helper did not stop after kill: ") +
                        run.launchError,
                    index, QStringLiteral("build"), tailText(run.error));
      return false;
    }
    const QString detail =
        run.launchError.isEmpty() ? tailText(run.error) : run.launchError;
    *bad = refuse(QStringLiteral("tool.launch"), detail, index,
                  QStringLiteral("build"), tailText(run.error));
    return false;
  }
  if (run.sawOutputLimit) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("build.output.truncated"),
                  QStringLiteral("the prep output exceeded its bound"), index,
                  QStringLiteral("build"), tailText(run.error));
    return false;
  }
  if (run.exitStatus != QProcess::NormalExit || run.exitCode != 0 ||
      !QFileInfo(engineOut).isFile()) {
    QFile::remove(engineOut);
    const QString detail =
        tailText(run.error).isEmpty()
            ? QStringLiteral("the prep helper failed: ") + toolExitCause(run)
            : tailText(run.error);
    *bad = refuse(QStringLiteral("build.failed"), detail, index, QStringLiteral("build"),
                  detail);
    return false;
  }
  MeasuredPrep measured;
  bool sawDevice = false;
  bool sawParsed = false;
  bool sawStarted = false;
  bool sawSerialized = false;
  bool sawRaw = false;
  QString key;
  QString reason;
  if (!parsePrepStream(run.output, &measured, &sawDevice, &sawParsed, &sawStarted,
                       &sawSerialized, &sawRaw, &key, &reason)) {
    QFile::remove(engineOut);
    *bad = refuse(key, reason, index, QStringLiteral("build"), tailText(run.error));
    return false;
  }
  if (!sawDevice || !sawParsed || !sawStarted || !sawSerialized || !sawRaw) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("build.event.missing"),
                  QStringLiteral("the helper omitted an identity event"), index,
                  QStringLiteral("build"), tailText(run.error));
    return false;
  }
  if (!checkMeasuredComplete(measured, &key, &reason)) {
    QFile::remove(engineOut);
    *bad = refuse(key, reason, index, QStringLiteral("build"), tailText(run.error));
    return false;
  }
  QString onnxReason;
  const std::optional<FileDigest> onnxMeasured =
      measureFile(storedCanonical, &onnxReason);
  if (!onnxMeasured.has_value()) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("model.owned.changed"), onnxReason, index,
                  QStringLiteral("build"));
    return false;
  }
  if (measured.onnxSha.toLower() != onnxMeasured->sha256.toLower() ||
      measured.onnxBytes != onnxMeasured->bytes ||
      measured.rawOnnxSha.toLower() != onnxMeasured->sha256.toLower() ||
      measured.rawOnnxBytes != onnxMeasured->bytes) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("build.graph.mismatch"),
                  QStringLiteral("the helper parsed other bytes"), index,
                  QStringLiteral("build"), tailText(run.error));
    return false;
  }
  if (measured.rawShapeBinding != QLatin1String("fixed-per-input") ||
      measured.rawIoFormat != QLatin1String("linear-only")) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("build.binding.mismatch"),
                  QStringLiteral("the helper did not bind fixed linear I/O"), index,
                  QStringLiteral("build"), tailText(run.error));
    return false;
  }
  if (!checkParsedBindings(measured, stage, inName, exportInShape, exportOutShape, &key,
                           &reason)) {
    QFile::remove(engineOut);
    *bad = refuse(key, reason, index, QStringLiteral("build"), tailText(run.error));
    return false;
  }
  if (!checkMeasuredAgainstBuild(measured, build, workspaceRequest, &key, &reason)) {
    QFile::remove(engineOut);
    *bad = refuse(key, reason, index, QStringLiteral("build"), tailText(run.error));
    return false;
  }
  QString engineReason;
  const std::optional<FileDigest> engineMeasured =
      measureFile(engineOut, &engineReason);
  if (!engineMeasured.has_value() || engineMeasured->bytes <= 0) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("build.engine.missing"),
                  engineReason.isEmpty()
                      ? QStringLiteral("the helper wrote no engine bytes")
                      : engineReason,
                  index, QStringLiteral("build"), tailText(run.error));
    return false;
  }
  if (engineMeasured->sha256.toLower() != measured.rawEngineSha.toLower() ||
      engineMeasured->bytes != measured.rawEngineBytes) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("build.digest.mismatch"),
                  QStringLiteral("the engine file is not the reported product"), index,
                  QStringLiteral("build"), tailText(run.error));
    return false;
  }
  out->measured = measured;
  out->engineSha = engineMeasured->sha256.toLower();
  out->engineBytes = engineMeasured->bytes;
  return true;
}

bool checkBindingEntry(const QJsonObject& binding, const RefTensor& declared, bool isInput,
                         QString* key, QString* reason) {
  auto fail = [&](const char* k, const QString& t) {
    if (key != nullptr) {
      *key = QString::fromLatin1(k);
    }
    if (reason != nullptr) {
      *reason = t;
    }
    return false;
  };
  if (binding.value(QStringLiteral("name")).toString() != declared.name) {
    return fail("verify.binding.name",
                QStringLiteral("a reported binding is not a declared tensor"));
  }
  const QString wantMode = isInput ? QStringLiteral("input") : QStringLiteral("output");
  if (binding.value(QStringLiteral("mode")).toString() != wantMode) {
    return fail("verify.binding.mode",
                QStringLiteral("a reported binding mode is not its declared mode"));
  }
  if (binding.value(QStringLiteral("dtype")).toString() != declared.dtype) {
    return fail("verify.binding.dtype",
                QStringLiteral("a reported binding dtype is not its declared dtype"));
  }
  if (binding.value(QStringLiteral("format")).toString() != QLatin1String("kLINEAR")) {
    return fail("verify.binding.format",
                QStringLiteral("a reported binding is not kLINEAR"));
  }
  if (binding.value(QStringLiteral("vectorizedDim")).toInt(-2) != -1 ||
      binding.value(QStringLiteral("reportedBytesPerComponent")).toInt(0) != -1) {
    return fail("verify.binding.vectorized",
                QStringLiteral("a reported binding is vectorized"));
  }
  qint64 want[4] = {0, 0, 0, 0};
  if (!shapeDims(declared.shape, want)) {
    return fail("verify.binding.shape",
                QStringLiteral("a declared shape is not NxCxHxW"));
  }
  const int wantElem = elementBytes(declared.dtype);
  if (binding.value(QStringLiteral("bytesPerElement")).toInt(-1) != wantElem) {
    return fail("verify.binding.element",
                QStringLiteral("a reported element size is not its dtype size"));
  }
  qint64 reported[4] = {0, 0, 0, 0};
  qint64 declaredShape[4] = {0, 0, 0, 0};
  if (!declaredDimsFromArray(binding.value(QStringLiteral("shape")).toArray(), reported,
                             false) ||
      !declaredDimsFromArray(binding.value(QStringLiteral("declaredShape")).toArray(),
                             declaredShape, true)) {
    return fail("verify.binding.shape",
                QStringLiteral("a reported binding shape is not rank-4"));
  }
  for (int a = 0; a < 4; ++a) {
    if (reported[a] != want[a]) {
      return fail("verify.binding.shape",
                  QStringLiteral("a reported shape is not the declared shape"));
    }
  }
  if (declaredShape[0] != 1 || declaredShape[1] != want[1]) {
    return fail("verify.binding.declared",
                QStringLiteral("a declared engine batch or channels moved"));
  }
  for (int a = 2; a < 4; ++a) {
    if (declaredShape[a] != -1 && declaredShape[a] != want[a]) {
      return fail("verify.binding.declared",
                  QStringLiteral("a static declared extent is not the stage extent"));
    }
  }
  qint64 elements = 0;
  if (!tensorBytes(declared.dtype, want, &elements) ||
      binding.value(QStringLiteral("elements")).toDouble(-1) !=
          static_cast<double>(elements / wantElem) ||
      binding.value(QStringLiteral("bytes")).toDouble(-1) != static_cast<double>(elements)) {
    return fail("verify.binding.bytes",
                QStringLiteral("a reported byte count is not its tensor bytes"));
  }
  if (binding.contains(QStringLiteral("strides")) &&
      !tensorStridesAreContiguous(binding.value(QStringLiteral("strides")).toArray(),
                                  binding.value(QStringLiteral("shape")).toArray())) {
    return fail("verify.binding.strides",
                QStringLiteral("a reported stride is not rank-4, is past this tensor, or is not "
                               "contiguous NCHW"));
  }
  return true;
}

bool runVerify(const ResolvedTools& tools, const EngineBuildIdentity& build,
               const ExportedRef& ref, const StageBuildRequest& stage,
               const QString& engineOut, const QString& engineSha, int index,
               const QString& modelId, NativeBuildCancellation* cancellation,
               const BuilderProgressCallback& progress, BuilderResult* bad) {
  QStringList args;
  args.append(QStringLiteral("--engine"));
  args.append(engineOut);
  args.append(QStringLiteral("--engine-sha256"));
  args.append(engineSha);
  args.append(QStringLiteral("--device-uuid"));
  args.append(build.deviceUuid);
  for (const RefTensor& tensor : ref.inputs) {
    args.append(QStringLiteral("--input"));
    args.append(tensor.name);
    args.append(tensor.dtype);
    args.append(tensor.shape);
    args.append(tensor.file);
  }
  for (const RefTensor& tensor : ref.outputs) {
    args.append(QStringLiteral("--output"));
    args.append(tensor.name);
    args.append(tensor.dtype);
    args.append(tensor.shape);
    args.append(tensor.file);
  }
  ToolResult run;
  const bool exited =
      runBlocking(tools.paths.verify, args, tools.prepVerifyEnv, cancellation, progress,
                  index, modelId, QStringLiteral("verify"), &run);
  if (!exited) {
    if (isCancelled(cancellation) && run.launchError.isEmpty()) {
      *bad = cancelledResult(QStringLiteral("verify"), index);
      return false;
    }
    if (!run.stopped && isCancelled(cancellation)) {
      QFile::remove(engineOut);
      *bad = refuse(QStringLiteral("verify.cancel.stop"),
                    QStringLiteral("the verifier did not stop after kill: ") +
                        run.launchError,
                    index, QStringLiteral("verify"), tailText(run.error));
      return false;
    }
    const QString detail =
        run.launchError.isEmpty() ? tailText(run.error) : run.launchError;
    *bad = refuse(QStringLiteral("tool.launch"), detail, index,
                  QStringLiteral("verify"), tailText(run.error));
    return false;
  }
  if (run.sawOutputLimit) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("verify.output.truncated"),
                  QStringLiteral("the verifier output exceeded its bound"), index,
                  QStringLiteral("verify"), tailText(run.error));
    return false;
  }
  QJsonObject report;
  if (!parseObject(run.output.trimmed(), &report)) {
    QFile::remove(engineOut);
    QString detail = tailText(run.error) + tailText(run.output);
    if (detail.isEmpty()) {
      detail = QStringLiteral("the verifier emitted no JSON report: ") + toolExitCause(run);
    }
    *bad = refuse(QStringLiteral("verify.json"),
                  QStringLiteral("the verifier emitted no JSON report"), index,
                  QStringLiteral("verify"), detail);
    return false;
  }
  if (run.exitStatus != QProcess::NormalExit || run.exitCode != 0 ||
      !report.value(QStringLiteral("ok")).toBool(false)) {
    const QString message = report.value(QStringLiteral("error"))
                                .toObject()
                                .value(QStringLiteral("message"))
                                .toString(tailText(run.error));
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("verify.") +
                       report.value(QStringLiteral("phase"))
                           .toString(QStringLiteral("failed")),
                   message.isEmpty()
                       ? QStringLiteral("the engine failed bindings or numerics")
                       : message,
                   index, QStringLiteral("verify"), message);
    return false;
  }
  QString failKey;
  QString failReason;
  if (report.value(QStringLiteral("tool")).toString() !=
          QLatin1String("mikai_native_verify") ||
      report.value(QStringLiteral("schemaVersion")).toInt(-1) != 1 ||
      report.value(QStringLiteral("result")).toString() !=
          QLatin1String("verified") ||
      report.value(QStringLiteral("phase")).toString() != QLatin1String("complete")) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("verify.unknown.success"),
                  QStringLiteral("the verifier report is not a measured success"), index,
                  QStringLiteral("verify"));
    return false;
  }
  if (report.value(QStringLiteral("manifestVerified")).toBool(true) ||
      report.value(QStringLiteral("prepared")).toBool(true)) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("verify.unknown.success"),
                  QStringLiteral("the verifier claimed more than a measured run"), index,
                  QStringLiteral("verify"));
    return false;
  }
  const QJsonObject engine = report.value(QStringLiteral("engine")).toObject();
  if (engine.value(QStringLiteral("sha256")).toString().toLower() !=
          engineSha.toLower() ||
      !engine.value(QStringLiteral("sha256Matched")).toBool(false) ||
      engine.value(QStringLiteral("expectedSha256")).toString().toLower() !=
          engineSha.toLower()) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("verify.identity.mismatch"),
                  QStringLiteral("the report names another engine"), index,
                  QStringLiteral("verify"));
    return false;
  }
  const QJsonObject device = report.value(QStringLiteral("device")).toObject();
  if (!isSameDeviceUuid(device.value(QStringLiteral("uuid")).toString(), build.deviceUuid)) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("verify.identity.mismatch"),
                  QStringLiteral("the report names another device"), index,
                  QStringLiteral("verify"));
    return false;
  }
  if (device.value(QStringLiteral("smMajor")).toInt(-1) != build.smMajor ||
      device.value(QStringLiteral("smMinor")).toInt(-2) != build.smMinor) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("verify.device.mismatch"),
                  QStringLiteral("the report SM is not the observed SM"), index,
                  QStringLiteral("verify"));
    return false;
  }
  const QJsonObject tensorRt = report.value(QStringLiteral("tensorrt")).toObject();
  int wantTrt = 0;
  if (!trtLibFromRuntime(build.runtimeVersion, &wantTrt) ||
      tensorRt.value(QStringLiteral("libraryVersion")).toInt(-1) != wantTrt) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("verify.runtime.mismatch"),
                  QStringLiteral("the verifier runtime is not the observed runtime"),
                  index, QStringLiteral("verify"));
    return false;
  }
  if (report.value(QStringLiteral("tolerances"))
          .toObject()
          .value(QStringLiteral("maxAbsoluteErrorOverride"))
          .toBool(true) ||
      report.value(QStringLiteral("tolerances"))
          .toObject()
          .value(QStringLiteral("maxRmseOverride"))
          .toBool(true)) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("verify.tolerance.override"),
                  QStringLiteral("the run overrode its tolerances"), index,
                  QStringLiteral("verify"));
    return false;
  }
  const QJsonArray bindings = report.value(QStringLiteral("bindings")).toArray();
  const QJsonArray outputs = report.value(QStringLiteral("outputs")).toArray();
  if (bindings.size() != 2 || outputs.size() != 1) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("verify.bindings.count"),
                  QStringLiteral("the report is not one input and one output"), index,
                  QStringLiteral("verify"));
    return false;
  }
  QMap<QString, QJsonObject> byName;
  for (const QJsonValue& entry : bindings) {
    if (!entry.isObject()) {
      QFile::remove(engineOut);
      *bad = refuse(QStringLiteral("verify.bindings.json"),
                    QStringLiteral("a binding entry is not an object"), index,
                    QStringLiteral("verify"));
      return false;
    }
    const QJsonObject binding = entry.toObject();
    const QString name = binding.value(QStringLiteral("name")).toString();
    if (name.isEmpty() || byName.contains(name)) {
      QFile::remove(engineOut);
      *bad = refuse(QStringLiteral("verify.bindings.name"),
                    QStringLiteral("a binding name is empty or repeated"), index,
                    QStringLiteral("verify"));
      return false;
    }
    byName.insert(name, binding);
  }
  if (!byName.contains(ref.inputs.first().name) ||
      !byName.contains(ref.outputs.first().name)) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("verify.binding.mismatch"),
                  QStringLiteral("the report omits a declared tensor"), index,
                  QStringLiteral("verify"));
    return false;
  }
  if (!checkBindingEntry(byName.value(ref.inputs.first().name), ref.inputs.first(), true,
                         &failKey, &failReason) ||
      !checkBindingEntry(byName.value(ref.outputs.first().name), ref.outputs.first(),
                         false, &failKey, &failReason)) {
    QFile::remove(engineOut);
    *bad = refuse(failKey, failReason, index, QStringLiteral("verify"));
    return false;
  }
  if (ref.outputs.first().shape != stage.outputShape) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("verify.output.shape"),
                  QStringLiteral("the reference output is not the stage output"), index,
                  QStringLiteral("verify"));
    return false;
  }
  const QJsonObject measured = outputs.first().toObject();
  if (measured.value(QStringLiteral("name")).toString() != ref.outputs.first().name ||
      measured.value(QStringLiteral("dtype")).toString() != ref.outputs.first().dtype) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("verify.output.identity"),
                  QStringLiteral("the measured output is not the declared output"), index,
                  QStringLiteral("verify"));
    return false;
  }
  qint64 wantOut[4] = {0, 0, 0, 0};
  qint64 gotOut[4] = {0, 0, 0, 0};
  if (!shapeDims(ref.outputs.first().shape, wantOut) ||
      !declaredDimsFromArray(measured.value(QStringLiteral("shape")).toArray(), gotOut,
                             false)) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("verify.output.shape"),
                  QStringLiteral("the measured output shape is not rank-4"), index,
                  QStringLiteral("verify"));
    return false;
  }
  for (int a = 0; a < 4; ++a) {
    if (wantOut[a] != gotOut[a]) {
      QFile::remove(engineOut);
      *bad = refuse(QStringLiteral("verify.output.shape"),
                    QStringLiteral("the measured output is not the declared shape"),
                    index, QStringLiteral("verify"));
      return false;
    }
  }
  if (!measured.value(QStringLiteral("measured")).toBool(false) ||
      !measured.value(QStringLiteral("finite")).toBool(false) ||
      !measured.value(QStringLiteral("withinTolerance")).toBool(false)) {
    QFile::remove(engineOut);
    *bad = refuse(QStringLiteral("verify.numerics"),
                  QStringLiteral("an output was not measured within tolerance"), index,
                  QStringLiteral("verify"));
    return false;
  }
  return true;
}

struct MainFlow {
  const BuilderRequest* request = nullptr;
  const Preflight* checked = nullptr;
  NativeBuildCancellation* cancellation = nullptr;
  BuilderProgressCallback progress;
  QTemporaryDir* scratch = nullptr;
  QVector<BuilderStageOutcome> full;
  QVector<Pending> pending;
  QStringList engineFiles;
  QStringList warnings;
};

void noteWarning(MainFlow* flow, const QString& text) {
  flow->warnings.append(redactNativeDiagnosticText(text));
}

bool removeOneFile(const QString& path, QString* warningOut) {
  if (path.isEmpty()) {
    return true;
  }
  if (!QFile::exists(path)) {
    return true;
  }
  if (!QFile::remove(path)) {
    if (warningOut != nullptr) {
      *warningOut = QString(QStringLiteral("cleanup left ")) + path;
    }
    return false;
  }
  return true;
}

void dropEngines(MainFlow* flow) {
  for (const QString& path : flow->engineFiles) {
    QString warning;
    if (!removeOneFile(path, &warning)) {
      noteWarning(flow, warning);
    }
  }
  flow->engineFiles.clear();
}

BuilderResult stageRequestFor(const QString& modelRootCanonical, const BuilderRequest& request, const QString& storedCanonical,
                              int index, BuildRequest* out, QString* key, QString* reason) {
  const ResolvedStage& stage = request.plan.stages.at(index);
  const StoredModel& model = request.models.at(index);
  if (model.record.id != stage.modelId) {
    if (key != nullptr) {
      *key = QStringLiteral("model.mismatch");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("the stage model is not the supplied owned copy");
    }
    return refuse(*key, *reason, index, QStringLiteral("cache"));
  }
  FileDigest digest;
  QString canonical;
  QString proveKey;
  QString proveReason;
  if (!proveOwnedModel(modelRootCanonical, model, &canonical, &digest, &proveKey, &proveReason)) {
    return refuse(proveKey, proveReason, index, QStringLiteral("cache"));
  }
  if (canonical != storedCanonical) {
    return refuse(QStringLiteral("model.owned.changed"),
                  QStringLiteral("the owned copy moved during the build"), index,
                  QStringLiteral("cache"));
  }
  if (model.record.contentId() != stage.modelContentId ||
      graphIdentityOf(model.record).contentId() != stage.modelContentId) {
    return refuse(QStringLiteral("model.owned.identity"),
                  QStringLiteral("the owned row is not the planned graph"), index,
                  QStringLiteral("cache"));
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
    return refuse(QStringLiteral("stage.geometry.invalid"),
                  QStringLiteral("the stage shape cannot be bound"), index,
                  QStringLiteral("cache"));
  }
  BuildRequest built;
  built.graph = graphIdentityOf(model.record);
  built.stage = stageRequest;
  built.build = request.build;
  const QString missing = built.firstMissingRequiredField();
  if (!missing.isEmpty()) {
    return refuse(QStringLiteral("request.incomplete"),
                  QStringLiteral("the build request records no ") + missing, index,
                  QStringLiteral("cache"));
  }
  *out = built;
  BuilderResult empty;
  return empty;
}

BuilderResult buildOneStage(MainFlow* flow, int index) {
  const BuilderRequest& request = *flow->request;
  const Preflight& checked = *flow->checked;
  const ResolvedStage& stage = request.plan.stages.at(index);
  const StoredModel& model = request.models.at(index);
  emitProgress(flow->progress, index, stage.modelId, QStringLiteral("cache"),
               stage.modelContentId);
  QString storedCanonical;
  FileDigest digest;
  QString proveKey;
  QString proveReason;
  if (!proveOwnedModel(checked.modelRootCanonical, model, &storedCanonical, &digest,
                       &proveKey, &proveReason)) {
    BuilderResult bad = refuse(proveKey, proveReason, index, QStringLiteral("cache"));
    bad.warnings = flow->warnings;
    return bad;
  }
  BuildRequest built;
  QString buildKey;
  QString buildReason;
  const BuilderResult requestBad = stageRequestFor(
      checked.modelRootCanonical, request, storedCanonical, index, &built, &buildKey,
      &buildReason);
  if (!buildKey.isEmpty() || !buildReason.isEmpty()) {
    BuilderResult bad = requestBad;
    bad.warnings = flow->warnings;
    return bad;
  }
  QString hitKey;
  QString hitReason;
  const std::optional<ArtifactRecord> hit =
      findArtifactForRequest(request.store, built, &hitKey, &hitReason);
  if (hit.has_value()) {
    QString runtimeKey;
    QString runtimeReason;
    QString sourceKey;
    QString sourceReason;
    const bool runtimeOk =
        verifyArtifactRuntime(*hit, request.build, &runtimeKey, &runtimeReason);
    const bool sourceOk =
        verifyArtifactSource(*hit, storedCanonical, &sourceKey, &sourceReason);
    if (runtimeOk && sourceOk) {
      BuilderStageOutcome outcome;
      outcome.request = built;
      outcome.record = *hit;
      outcome.cacheHit = true;
      flow->full[index] = outcome;
      emitProgress(flow->progress, index, stage.modelId, QStringLiteral("cache"),
                   built.buildKey());
      BuilderResult empty;
      empty.warnings = flow->warnings;
      return empty;
    }
    if (!runtimeOk) {
      emitProgress(flow->progress, index, stage.modelId, QStringLiteral("cache"),
                   runtimeKey + QStringLiteral(":") + runtimeReason);
    } else {
      emitProgress(flow->progress, index, stage.modelId, QStringLiteral("cache"),
                   sourceKey + QStringLiteral(":") + sourceReason);
    }
  } else if (hitKey != QLatin1String("artifact.missing")) {
    emitProgress(flow->progress, index, stage.modelId, QStringLiteral("cache"),
                 hitKey + QStringLiteral(":") + hitReason);
  }
  const QString refDir = QDir(flow->scratch->path())
                             .filePath(QStringLiteral("ref-") + QString::number(index));
  emitProgress(flow->progress, index, stage.modelId, QStringLiteral("reference"), refDir);
  ExportedRef exported;
  BuilderResult refBad;
  if (!exportReference(checked.tools, storedCanonical, stage, request.references.at(index),
                       refDir, index, stage.modelId, flow->cancellation, flow->progress,
                       &exported, &refBad)) {
    refBad.warnings = flow->warnings;
    return refBad;
  }
  if (exported.inputs.first().shape != built.stage.inputShape ||
      exported.outputs.first().shape != built.stage.outputShape) {
    BuilderResult bad = refuse(QStringLiteral("reference.geometry.mismatch"),
                              QStringLiteral("a reference tensor is not the stage shape"),
                              index, QStringLiteral("reference"));
    bad.warnings = flow->warnings;
    return bad;
  }
  const QString engineOut =
      QDir(flow->scratch->path())
          .filePath(QStringLiteral("stage-") + QString::number(index) +
                    QStringLiteral(".engine"));
  emitProgress(flow->progress, index, stage.modelId, QStringLiteral("build"),
               exported.inputs.first().name + QStringLiteral(":") + built.stage.inputShape);
  PrepProduct product;
  BuilderResult prepBad;
  if (!runPrep(checked.tools, storedCanonical, built.stage, request.build,
               exported.inputs.first().name, exported.inputs.first().shape,
               exported.outputs.first().shape, engineOut, request.workspaceBytes, index,
               stage.modelId, flow->cancellation, flow->progress, &product, &prepBad)) {
    prepBad.warnings = flow->warnings;
    return prepBad;
  }
  emitProgress(flow->progress, index, stage.modelId, QStringLiteral("build"),
               QStringLiteral("builderFlags=%1 tacticSources=%2 networkFlags=%3")
                   .arg(product.measured.flags.builderFlags)
                   .arg(product.measured.flags.tacticSources)
                   .arg(product.measured.flags.networkFlags));
  emitProgress(flow->progress, index, stage.modelId, QStringLiteral("verify"),
               product.engineSha.left(16));
  BuilderResult verifyBad;
  if (!runVerify(checked.tools, request.build, exported, built.stage, engineOut,
                 product.engineSha, index, stage.modelId, flow->cancellation,
                 flow->progress, &verifyBad)) {
    verifyBad.warnings = flow->warnings;
    return verifyBad;
  }
  emitProgress(flow->progress, index, stage.modelId, QStringLiteral("verify"),
               QStringLiteral("verified"));
  flow->engineFiles.append(engineOut);
  Pending stored;
  stored.request = built;
  stored.enginePath = engineOut;
  stored.engineSha = product.engineSha;
  stored.engineBytes = product.engineBytes;
  stored.prep = product.measured.flags;
  stored.prep.tensorRtLibVersion = product.measured.tensorRtLibVersion;
  stored.prep.tensorRtLibVersionPresent = true;
  stored.prep.cudaRuntimeVersion = product.measured.cudaRuntimeVersion;
  stored.prep.cudaRuntimeVersionPresent = true;
  stored.prep.cudaDriverApiVersion = product.measured.cudaDriverApiVersion;
  stored.prep.cudaDriverApiVersionPresent = true;
  stored.prep.cudaDriverFileVersion = product.measured.cudaDriverFileVersion;
  stored.prep.workspaceBytes = product.measured.workspaceBytes;
  stored.prep.workspaceBytesPresent = true;
  stored.prep.workspaceSource = product.measured.workspaceSource;
  stored.onnxSha = digest.sha256.toLower();
  stored.onnxBytes = digest.bytes;
  stored.declared = model.record.actualProfile;
  stored.storedCanonical = storedCanonical;
  stored.index = index;
  flow->pending.append(stored);
  BuilderStageOutcome hole;
  hole.request = built;
  hole.cacheHit = false;
  hole.prepIdentity = stored.prep;
  flow->full[index] = hole;
  BuilderResult empty;
  empty.warnings = flow->warnings;
  return empty;
}

bool rollbackNew(MainFlow* flow, const QStringList& fresh) {
  bool clean = true;
  for (const QString& key : fresh) {
    QString removeKey;
    QString removeReason;
    if (!removeArtifact(flow->request->store, key, &removeKey, &removeReason)) {
      clean = false;
      noteWarning(flow, QString(QStringLiteral("rollback left ")) + key +
                             QStringLiteral(": ") + removeKey +
                             QStringLiteral(": ") + removeReason);
    }
  }
  return clean;
}

BuilderResult publishAll(MainFlow* flow) {
  const BuilderRequest& request = *flow->request;
  QStringList fresh;
  for (const Pending& item : flow->pending) {
    ArtifactManifest manifest;
    manifest.schema = artifactManifestSchema();
    manifest.mikaiFormat = 1;
    manifest.buildKey = item.request.buildKey();
    manifest.engineSha256 = item.engineSha;
    manifest.source.modelContentId = item.request.graph.contentId();
    manifest.source.onnxSha256 = item.onnxSha;
    manifest.source.onnxBytes = item.onnxBytes;
    manifest.source.declaredProfile = item.declared;
    manifest.stage.role = item.request.stage.role;
    manifest.stage.inputShape = item.request.stage.inputShape;
    manifest.stage.outputShape = item.request.stage.outputShape;
    manifest.stage.ioFormat = item.request.stage.ioFormat;
    manifest.stage.padLeft = item.request.stage.padLeft;
    manifest.stage.padTop = item.request.stage.padTop;
    manifest.build.provider = item.request.build.provider;
    manifest.build.runtime = item.request.build.runtimeVersion;
    manifest.build.plugin = item.request.build.pluginVersion;
    manifest.build.coreSha = item.request.build.coreSha;
    manifest.build.sourceAbi = item.request.build.sourceAbi;
    manifest.build.precision = item.request.build.precision;
    manifest.build.tf32 = item.request.build.tf32;
    manifest.build.stronglyTyped = item.request.build.stronglyTyped;
    manifest.build.skipInference = item.request.build.skipInference;
    manifest.build.canonicalOptions = item.request.build.canonicalOptions();
    manifest.device.uuid = item.request.build.deviceUuid;
    manifest.device.sm = item.request.build.smText();
    manifest.device.driver = item.request.build.driverVersion;
    manifest.device.cuda = item.request.build.cudaVersion;
    manifest.device.luid = item.request.build.adapterLuid;
    manifest.engine.file = QString::fromLatin1(kEngineFileName);
    manifest.engine.bytes = item.engineBytes;
    manifest.engine.sha256 = item.engineSha;
    manifest.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QString manifestKey;
    QString manifestReason;
    if (!manifest.isSelfConsistent(&manifestKey, &manifestReason)) {
      rollbackNew(flow, fresh);
      BuilderResult bad = refuse(manifestKey.isEmpty() ? QStringLiteral("manifest.inconsistent")
                                                       : manifestKey,
                               manifestReason, item.index, QStringLiteral("publish"));
      bad.warnings = flow->warnings;
      return bad;
    }
    emitProgress(flow->progress, item.index,
                 request.plan.stages.at(item.index).modelId,
                 QStringLiteral("publish"), manifest.buildKey);
    const bool preExists =
        readArtifact(request.store, manifest.buildKey, nullptr, nullptr).has_value();
    QString publishKey;
    QString publishReason;
    const std::optional<ArtifactRecord> record =
        publishArtifact(request.store, manifest, item.enginePath, &publishKey,
                        &publishReason);
    if (!record.has_value()) {
      rollbackNew(flow, fresh);
      BuilderResult bad = refuse(publishKey.isEmpty() ? QStringLiteral("publish.failed")
                                                      : publishKey,
                               publishReason, item.index, QStringLiteral("publish"),
                               publishReason);
      bad.warnings = flow->warnings;
      return bad;
    }
    if (!preExists) {
      fresh.append(manifest.buildKey);
    }
    QString proofKey;
    QString proofReason;
    const bool runtimeOk =
        verifyArtifactRuntime(*record, request.build, &proofKey, &proofReason);
    QString sourceKey;
    QString sourceReason;
    const bool sourceOk =
        verifyArtifactSource(*record, item.storedCanonical, &sourceKey, &sourceReason);
    if (!runtimeOk || !sourceOk) {
      rollbackNew(flow, fresh);
      BuilderResult bad =
          refuse(!runtimeOk ? proofKey : sourceKey,
                 !runtimeOk ? proofReason : sourceReason, item.index,
                 QStringLiteral("publish"));
      bad.warnings = flow->warnings;
      return bad;
    }
    BuilderStageOutcome outcome;
    outcome.request = item.request;
    outcome.record = *record;
    outcome.cacheHit = preExists;
    outcome.prepIdentity = item.prep;
    flow->full[item.index] = outcome;
  }
  BuilderResult done;
  done.ok = true;
  done.stages = flow->full;
  done.publishedBuildKeys = fresh;
  done.phase = QStringLiteral("done");
  done.warnings = flow->warnings;
  return done;
}

}  // namespace

BuilderResult runNativeBuild(const BuilderRequest& request,
                             NativeBuildCancellation* cancellation,
                             const BuilderProgressCallback& progress) {
  QString key;
  QString reason;
  Preflight checked;
  if (!preflight(request, &checked, &key, &reason)) {
    return refuse(key, reason, -1, QStringLiteral("plan"));
  }
  if (isCancelled(cancellation)) {
    return cancelledResult(QStringLiteral("plan"));
  }
  emitProgress(progress, -1, {}, QStringLiteral("lease"), request.plan.id);
  std::optional<NativeWorkLease> lease =
      NativeWorkLease::acquire(NativeWorkKind::Preparation);
  if (!lease.has_value()) {
    return refuse(QStringLiteral("build.busy"),
                  QStringLiteral("another preparation or inference holds the device"), -1,
                  QStringLiteral("lease"));
  }
  QString scratchReason;
  QString scratchBase = nativeSubdirectory(QStringLiteral("scratch"), &scratchReason);
  std::optional<QTemporaryDir> owned;
  std::optional<QTemporaryDir> probed;
  QTemporaryDir* scratch = nullptr;
  if (!scratchBase.isEmpty()) {
    QDir().mkpath(scratchBase);
    owned.emplace(QDir(scratchBase).filePath(QStringLiteral("builder-XXXXXX")));
    owned->setAutoRemove(false);
    if (!owned->isValid()) {
      return refuse(QStringLiteral("scratch.create"),
                    QStringLiteral("the owned scratch template cannot be created"), -1,
                    QStringLiteral("lease"));
    }
    scratch = &(*owned);
  } else {
    if (!request.store.root().isEmpty() &&
        pathWithinRoot(QDir::tempPath(), request.store.root())) {
      probed.emplace(QDir(request.store.root()).filePath(QStringLiteral(".tmp-XXXXXX")));
      probed->setAutoRemove(true);
      if (!probed->isValid()) {
        return refuse(QStringLiteral("scratch.create"),
                      QStringLiteral("the probe scratch template cannot be created"), -1,
                      QStringLiteral("lease"));
      }
      scratch = &(*probed);
    } else {
      return refuse(QStringLiteral("scratch.unowned"),
                    scratchReason.isEmpty()
                        ? QStringLiteral("no owned scratch hierarchy")
                        : scratchReason,
                    -1, QStringLiteral("lease"));
    }
  }
  const QString scratchDir = scratch->path();
  if (scratchDir.isEmpty() || !QFileInfo(scratchDir).isDir()) {
    return refuse(QStringLiteral("scratch.create"),
                  QStringLiteral("the exclusive scratch directory is missing"), -1,
                  QStringLiteral("lease"));
  }
  QString storeReason;
  if (!request.store.ensureOwned(&storeReason)) {
    if (scratch == &(*owned)) {
      QDir dir(scratchDir);
      dir.removeRecursively();
    }
    return refuse(QStringLiteral("store.unowned"),
                  storeReason.isEmpty()
                      ? QStringLiteral("the artifact store cannot be created")
                      : storeReason,
                  -1, QStringLiteral("lease"));
  }
  MainFlow flow;
  flow.request = &request;
  flow.checked = &checked;
  flow.cancellation = cancellation;
  flow.progress = progress;
  flow.scratch = scratch;
  flow.full = QVector<BuilderStageOutcome>(request.plan.stages.size());
  const int count = request.plan.stages.size();
  for (int i = 0; i < count; ++i) {
    if (isCancelled(cancellation)) {
      BuilderResult bad = cancelledResult(QStringLiteral("cache"), i);
      bad.warnings = flow.warnings;
      if (scratch == &(*owned)) {
        QDir dir(scratchDir);
        if (!dir.removeRecursively()) {
          bad.warnings.append(QStringLiteral("cleanup left the scratch directory"));
        }
      }
      dropEngines(&flow);
      return bad;
    }
    const BuilderResult stageBad = buildOneStage(&flow, i);
    if (!stageBad.ok && (!stageBad.reasonKey.isEmpty() || !stageBad.reason.isEmpty() ||
                         stageBad.cancelled)) {
      BuilderResult bad = stageBad;
      if (scratch == &(*owned)) {
        QDir dir(scratchDir);
        if (!dir.removeRecursively()) {
          bad.warnings.append(QStringLiteral("cleanup left the scratch directory"));
        }
      }
      dropEngines(&flow);
      return bad;
    }
  }
  if (isCancelled(cancellation)) {
    BuilderResult bad = cancelledResult(QStringLiteral("verify"));
    bad.warnings = flow.warnings;
    if (scratch == &(*owned)) {
      QDir dir(scratchDir);
      if (!dir.removeRecursively()) {
        bad.warnings.append(QStringLiteral("cleanup left the scratch directory"));
      }
    }
    dropEngines(&flow);
    return bad;
  }
  const bool hasNew = !flow.pending.isEmpty();
  if (hasNew && cancellation != nullptr && !cancellation->beginPublication()) {
    BuilderResult bad = cancelledResult(QStringLiteral("publish"));
    bad.warnings = flow.warnings;
    if (scratch == &(*owned)) {
      QDir dir(scratchDir);
      if (!dir.removeRecursively()) {
        bad.warnings.append(QStringLiteral("cleanup left the scratch directory"));
      }
    }
    dropEngines(&flow);
    return bad;
  }
  BuilderResult published = publishAll(&flow);
  dropEngines(&flow);
  if (scratch == &(*owned)) {
    QDir dir(scratchDir);
    if (!dir.removeRecursively()) {
      published.warnings.append(QStringLiteral("cleanup left the scratch directory"));
    }
  }
  if (published.ok) {
    emitProgress(progress, -1, {}, QStringLiteral("done"), request.plan.id);
  }
  return published;
}

bool tensorStridesAreContiguous(const QJsonArray& strides, const QJsonArray& shape) {
  qint64 dims[4] = {0, 0, 0, 0};
  if (!declaredDimsFromArray(shape, dims, false)) {
    return false;
  }
  const qint64 wanted[4] = {dims[1] * dims[2] * dims[3], dims[2] * dims[3], dims[3], 1};
  qint64 got[4] = {0, 0, 0, 0};
  if (!stridesFromArray(strides, got, wanted[0])) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    if (got[i] != wanted[i]) {
      return false;
    }
  }
  return true;
}

namespace {

// Strict probe-document members. A member the document omits and a member it
// spells wrongly are different refusals, so each reader answers false and the
// caller says which member and why.
bool memberOnly(const QJsonObject& object, const QStringList& allowed, QString* offender) {
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (!allowed.contains(it.key())) {
      if (offender != nullptr) {
        *offender = it.key();
      }
      return false;
    }
  }
  return true;
}

bool memberString(const QJsonObject& object, const char* field, QString* out) {
  const QJsonValue value = object.value(QString::fromLatin1(field));
  if (!value.isString() || value.toString().isEmpty()) {
    return false;
  }
  *out = value.toString();
  return true;
}

bool memberBool(const QJsonObject& object, const char* field, bool* out) {
  const QJsonValue value = object.value(QString::fromLatin1(field));
  if (!value.isBool()) {
    return false;
  }
  *out = value.toBool();
  return true;
}

bool memberInt(const QJsonObject& object, const char* field, qint64* out) {
  const QJsonValue value = object.value(QString::fromLatin1(field));
  if (!value.isDouble()) {
    return false;
  }
  const double number = value.toDouble();
  const qint64 integer = static_cast<qint64>(number);
  if (static_cast<double>(integer) != number) {
    return false;
  }
  *out = integer;
  return true;
}

bool memberUint(const QJsonObject& object, const char* field, quint64* out) {
  qint64 integer = 0;
  if (!memberInt(object, field, &integer) || integer < 0) {
    return false;
  }
  *out = static_cast<quint64>(integer);
  return true;
}

bool memberPath(const QJsonObject& object, const char* field, QString* out) {
  QString value;
  if (!memberString(object, field, &value) || !QDir::isAbsolutePath(value)) {
    return false;
  }
  *out = value;
  return true;
}

}  // namespace

bool parseBuilderProbeRequest(const QJsonObject& object, BuilderProbeRequest* out,
                              QString* reasonKey, QString* reason) {
  const auto fail = [reasonKey, reason](const char* key, const QString& text) {
    if (reasonKey != nullptr) {
      *reasonKey = QString::fromLatin1(key);
    }
    if (reason != nullptr) {
      *reason = text;
    }
    return false;
  };
  const auto unknown = [&](const QString& offender) {
    return fail("probe.unknown",
                QStringLiteral("'%1' is not a member of this request").arg(offender));
  };

  {
    QString offender;
    const QStringList top{QStringLiteral("recipe"),     QStringLiteral("modelRoot"),
                          QStringLiteral("store"),      QStringLiteral("workspaceBytes"),
                          QStringLiteral("source"),     QStringLiteral("device"),
                          QStringLiteral("backends"),   QStringLiteral("build"),
                          QStringLiteral("tools"),      QStringLiteral("references")};
    if (!memberOnly(object, top, &offender)) {
      return unknown(offender);
    }
  }

  BuilderProbeRequest parsed;
  if (!memberPath(object, "recipe", &parsed.recipePath)) {
    return fail("probe.path", QStringLiteral("recipe must be a non-empty absolute path"));
  }
  if (!memberPath(object, "modelRoot", &parsed.request.modelRoot)) {
    return fail("probe.path", QStringLiteral("modelRoot must be a non-empty absolute path"));
  }
  QString storeRoot;
  if (!memberPath(object, "store", &storeRoot)) {
    return fail("probe.path", QStringLiteral("store must be a non-empty absolute path"));
  }
  parsed.request.store = ArtifactStore(storeRoot);
  if (!memberUint(object, "workspaceBytes", &parsed.request.workspaceBytes)) {
    return fail("probe.member",
                QStringLiteral("workspaceBytes must be an explicit non-negative integer"));
  }

  // ------------------------------------------------------------------ source
  {
    const QJsonValue value = object.value(QStringLiteral("source"));
    if (!value.isObject()) {
      return fail("probe.member", QStringLiteral("source is missing or not an object"));
    }
    const QJsonObject source = value.toObject();
    QString offender;
    const QStringList keys{QStringLiteral("width"),   QStringLiteral("height"),
                           QStringLiteral("fps"),     QStringLiteral("csp"),
                           QStringLiteral("matrix"),  QStringLiteral("range"),
                           QStringLiteral("siting"),  QStringLiteral("bitDepth"),
                           QStringLiteral("crop"),    QStringLiteral("rotation")};
    if (!memberOnly(source, keys, &offender)) {
      return unknown(QStringLiteral("source.") + offender);
    }
    qint64 width = 0;
    qint64 height = 0;
    if (!memberInt(source, "width", &width) || width <= 0 || width > kMaxExtent ||
        !memberInt(source, "height", &height) || height <= 0 || height > kMaxExtent) {
      return fail("probe.member",
                  QStringLiteral("source width and height must be positive integers"));
    }
    parsed.source.codedSize = QSize(static_cast<int>(width), static_cast<int>(height));
    const QJsonValue fpsValue = source.value(QStringLiteral("fps"));
    if (!fpsValue.isObject()) {
      return fail("probe.member", QStringLiteral("source.fps is missing or not an object"));
    }
    const QJsonObject fps = fpsValue.toObject();
    if (!memberOnly(fps, QStringList{QStringLiteral("num"), QStringLiteral("den")}, &offender)) {
      return unknown(QStringLiteral("source.fps.") + offender);
    }
    qint64 num = 0;
    qint64 den = 0;
    if (!memberInt(fps, "num", &num) || !memberInt(fps, "den", &den)) {
      return fail("probe.member", QStringLiteral("source.fps needs integer num and den"));
    }
    parsed.source.fps = Rational{static_cast<int>(num), static_cast<int>(den)};
    if (!parsed.source.fps->isValid()) {
      return fail("probe.member", QStringLiteral("source.fps is not a positive ratio"));
    }
    const auto optionalText = [&](const char* field, QString* target) -> bool {
      if (!source.contains(QString::fromLatin1(field))) {
        return true;
      }
      const QJsonValue text = source.value(QString::fromLatin1(field));
      if (!text.isString()) {
        return fail("probe.type",
                    QStringLiteral("source.%1 is not a string")
                        .arg(QString::fromLatin1(field)));
      }
      *target = text.toString();
      return true;
    };
    if (!optionalText("csp", &parsed.source.csp) ||
        !optionalText("matrix", &parsed.source.matrix) ||
        !optionalText("range", &parsed.source.range) ||
        !optionalText("siting", &parsed.source.siting)) {
      return false;
    }
    if (source.contains(QStringLiteral("bitDepth"))) {
      qint64 bits = 0;
      if (!memberInt(source, "bitDepth", &bits) || bits < 0 || bits > kMaxExtent) {
        return fail("probe.member", QStringLiteral("source.bitDepth is not a bit count"));
      }
      parsed.source.bitDepth = static_cast<int>(bits);
    }
    if (source.contains(QStringLiteral("crop"))) {
      const QJsonValue cropValue = source.value(QStringLiteral("crop"));
      if (!cropValue.isObject()) {
        return fail("probe.type", QStringLiteral("source.crop is not an object"));
      }
      const QJsonObject crop = cropValue.toObject();
      if (!memberOnly(
              crop,
              QStringList{QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"),
                          QStringLiteral("height")},
              &offender)) {
        return unknown(QStringLiteral("source.crop.") + offender);
      }
      qint64 x = 0;
      qint64 y = 0;
      qint64 cropWidth = 0;
      qint64 cropHeight = 0;
      if (!memberInt(crop, "x", &x) || !memberInt(crop, "y", &y) ||
          !memberInt(crop, "width", &cropWidth) || !memberInt(crop, "height", &cropHeight) ||
          cropWidth <= 0 || cropHeight <= 0) {
        return fail("probe.member", QStringLiteral("source.crop needs a positive rectangle"));
      }
      parsed.source.crop = QRect(static_cast<int>(x), static_cast<int>(y),
                                 static_cast<int>(cropWidth), static_cast<int>(cropHeight));
    }
    if (source.contains(QStringLiteral("rotation"))) {
      qint64 rotation = 0;
      if (!memberInt(source, "rotation", &rotation)) {
        return fail("probe.type", QStringLiteral("source.rotation is not an integer"));
      }
      parsed.source.rotationDegrees = static_cast<int>(rotation);
    }
  }

  // ------------------------------------------------------------------ device
  {
    const QJsonValue value = object.value(QStringLiteral("device"));
    if (!value.isObject()) {
      return fail("probe.member", QStringLiteral("device is missing or not an object"));
    }
    const QJsonObject device = value.toObject();
    QString offender;
    const QStringList keys{QStringLiteral("decoderUuid"), QStringLiteral("computeUuid"),
                           QStringLiteral("smMajor"),     QStringLiteral("smMinor"),
                           QStringLiteral("adapterLuid")};
    if (!memberOnly(device, keys, &offender)) {
      return unknown(QStringLiteral("device.") + offender);
    }
    if (!memberString(device, "decoderUuid", &parsed.device.decoderDeviceUuid) ||
        !memberString(device, "computeUuid", &parsed.device.computeDeviceUuid)) {
      return fail("probe.member",
                  QStringLiteral("device decoderUuid and computeUuid must be observed"));
    }
    qint64 smMajor = 0;
    qint64 smMinor = 0;
    if (!memberInt(device, "smMajor", &smMajor) || smMajor <= 0 || smMajor > kMaxExtent ||
        !memberInt(device, "smMinor", &smMinor) || smMinor < 0 || smMinor > kMaxExtent) {
      return fail("probe.member",
                  QStringLiteral("device smMajor and smMinor must be observed"));
    }
    parsed.device.smMajor = static_cast<int>(smMajor);
    parsed.device.smMinor = static_cast<int>(smMinor);
    if (device.contains(QStringLiteral("adapterLuid")) &&
        !memberString(device, "adapterLuid", &parsed.device.adapterLuid)) {
      return fail("probe.type", QStringLiteral("device.adapterLuid is not a string"));
    }
  }

  // ---------------------------------------------------------------- backends
  {
    const QJsonValue value = object.value(QStringLiteral("backends"));
    if (!value.isArray()) {
      return fail("probe.member", QStringLiteral("backends is missing or not an array"));
    }
    const QJsonArray array = value.toArray();
    if (array.isEmpty()) {
      return fail("probe.member", QStringLiteral("backends names no runtime capability"));
    }
    for (const QJsonValue& entry : array) {
      if (!entry.isString()) {
        return fail("probe.type", QStringLiteral("a backend name is not a string"));
      }
      const std::optional<Backend> backend = backendFromName(entry.toString());
      if (!backend.has_value()) {
        return fail("probe.member",
                    QStringLiteral("'%1' is not a backend this build carries")
                        .arg(entry.toString()));
      }
      if (!parsed.backends.available.contains(*backend)) {
        parsed.backends.available.append(*backend);
      }
    }
  }

  // ------------------------------------------------------------------- build
  {
    const QJsonValue value = object.value(QStringLiteral("build"));
    if (!value.isObject()) {
      return fail("probe.member", QStringLiteral("build is missing or not an object"));
    }
    const QJsonObject buildObject = value.toObject();
    QString offender;
    const QStringList keys{QStringLiteral("provider"),        QStringLiteral("runtimeVersion"),
                           QStringLiteral("pluginVersion"),   QStringLiteral("coreSha"),
                           QStringLiteral("sourceAbi"),       QStringLiteral("precision"),
                           QStringLiteral("tf32"),            QStringLiteral("stronglyTyped"),
                           QStringLiteral("skipInference"),   QStringLiteral("deviceUuid"),
                           QStringLiteral("smMajor"),         QStringLiteral("smMinor"),
                           QStringLiteral("cudaVersion"),     QStringLiteral("driverVersion"),
                           QStringLiteral("adapterLuid"),     QStringLiteral("options")};
    if (!memberOnly(buildObject, keys, &offender)) {
      return unknown(QStringLiteral("build.") + offender);
    }
    EngineBuildIdentity build;
    QString providerName;
    if (!memberString(buildObject, "provider", &providerName)) {
      return fail("probe.member", QStringLiteral("build.provider is missing"));
    }
    const std::optional<Backend> provider = backendFromName(providerName);
    if (!provider.has_value()) {
      return fail("probe.member",
                  QStringLiteral("build.provider '%1' is not a backend this build carries")
                      .arg(providerName));
    }
    build.provider = *provider;
    if (!memberString(buildObject, "runtimeVersion", &build.runtimeVersion) ||
        !memberString(buildObject, "pluginVersion", &build.pluginVersion) ||
        !memberString(buildObject, "coreSha", &build.coreSha) ||
        !memberString(buildObject, "cudaVersion", &build.cudaVersion) ||
        !memberString(buildObject, "driverVersion", &build.driverVersion) ||
        !memberString(buildObject, "deviceUuid", &build.deviceUuid)) {
      return fail("probe.member",
                  QStringLiteral("every observed build text field must be present"));
    }
    qint64 sourceAbi = 0;
    if (!memberInt(buildObject, "sourceAbi", &sourceAbi) || sourceAbi <= 0 ||
        sourceAbi > kMaxExtent) {
      return fail("probe.member", QStringLiteral("build.sourceAbi must be observed"));
    }
    build.sourceAbi = static_cast<int>(sourceAbi);
    QString precisionName;
    if (!memberString(buildObject, "precision", &precisionName)) {
      return fail("probe.member", QStringLiteral("build.precision is missing"));
    }
    const std::optional<EnginePrecision> precision = enginePrecisionFromName(precisionName);
    if (!precision.has_value()) {
      return fail("probe.member",
                  QStringLiteral("build.precision '%1' is not a precision this build carries")
                      .arg(precisionName));
    }
    build.precision = *precision;
    if (!memberBool(buildObject, "tf32", &build.tf32) ||
        !memberBool(buildObject, "stronglyTyped", &build.stronglyTyped) ||
        !memberBool(buildObject, "skipInference", &build.skipInference)) {
      return fail("probe.member",
                  QStringLiteral("build tf32, stronglyTyped and skipInference must be explicit"));
    }
    qint64 buildSmMajor = 0;
    qint64 buildSmMinor = 0;
    if (!memberInt(buildObject, "smMajor", &buildSmMajor) || buildSmMajor <= 0 ||
        buildSmMajor > kMaxExtent || !memberInt(buildObject, "smMinor", &buildSmMinor) ||
        buildSmMinor < 0 || buildSmMinor > kMaxExtent) {
      return fail("probe.member", QStringLiteral("build smMajor and smMinor must be observed"));
    }
    build.smMajor = static_cast<int>(buildSmMajor);
    build.smMinor = static_cast<int>(buildSmMinor);
    if (buildObject.contains(QStringLiteral("adapterLuid")) &&
        !memberString(buildObject, "adapterLuid", &build.adapterLuid)) {
      return fail("probe.type", QStringLiteral("build.adapterLuid is not a string"));
    }
    const QJsonValue optionsValue = buildObject.value(QStringLiteral("options"));
    if (!optionsValue.isArray()) {
      return fail("probe.member", QStringLiteral("build.options is missing or not an array"));
    }
    const QJsonArray options = optionsValue.toArray();
    if (options.isEmpty()) {
      return fail("probe.member", QStringLiteral("build.options names no builder options"));
    }
    QStringList optionTokens;
    for (const QJsonValue& entry : options) {
      if (!entry.isString() || entry.toString().isEmpty()) {
        return fail("probe.type", QStringLiteral("a builder option is not a non-empty string"));
      }
      optionTokens.append(entry.toString());
    }
    QString optionsReason;
    const std::optional<QVector<BuildOption>> parsedOptions =
        parseBuildOptions(optionTokens.join(QLatin1Char(' ')), &optionsReason);
    if (!parsedOptions.has_value()) {
      return fail("probe.member",
                  QStringLiteral("build.options are not builder options: %1").arg(optionsReason));
    }
    build.options = *parsedOptions;
    int declaredLevel = 0;
    QString levelKey;
    QString levelReason;
    if (!declaredOptimizationLevel(build, &declaredLevel, &levelKey, &levelReason)) {
      return fail("build.options.optimizationLevel", levelReason);
    }
    const QString missing = build.firstMissingRequiredField();
    if (!missing.isEmpty()) {
      return fail("probe.build",
                  QStringLiteral("the observed build identity records no %1").arg(missing));
    }
    parsed.request.build = build;
  }

  // ------------------------------------------------------------------- tools
  {
    const QJsonValue value = object.value(QStringLiteral("tools"));
    if (!value.isObject()) {
      return fail("probe.member", QStringLiteral("tools is missing or not an object"));
    }
    const QJsonObject tools = value.toObject();
    QString offender;
    const QStringList keys{QStringLiteral("inspect"),        QStringLiteral("prep"),
                           QStringLiteral("verifier"),       QStringLiteral("ortRuntimeDir"),
                           QStringLiteral("trtBinDir"),      QStringLiteral("cudaBinDir")};
    if (!memberOnly(tools, keys, &offender)) {
      return unknown(QStringLiteral("tools.") + offender);
    }
    if (!memberPath(tools, "inspect", &parsed.request.tools.inspectExecutable) ||
        !memberPath(tools, "prep", &parsed.request.tools.prepExecutable) ||
        !memberPath(tools, "verifier", &parsed.request.tools.verifierExecutable) ||
        !memberPath(tools, "ortRuntimeDir", &parsed.request.tools.ortRuntimeDir)) {
      return fail("probe.path",
                  QStringLiteral("inspect, prep, verifier and ortRuntimeDir must be absolute"));
    }
    const auto optionalDir = [&](const char* field, QString* target) -> bool {
      if (!tools.contains(QString::fromLatin1(field))) {
        return true;
      }
      if (!memberPath(tools, field, target)) {
        return fail("probe.path",
                    QStringLiteral("tools.%1 must be an absolute path")
                        .arg(QString::fromLatin1(field)));
      }
      return true;
    };
    if (!optionalDir("trtBinDir", &parsed.request.tools.trtBinDir) ||
        !optionalDir("cudaBinDir", &parsed.request.tools.cudaBinDir)) {
      return false;
    }
  }

  // -------------------------------------------------------------- references
  {
    const QJsonValue value = object.value(QStringLiteral("references"));
    if (!value.isArray()) {
      return fail("probe.member", QStringLiteral("references is missing or not an array"));
    }
    const QJsonArray array = value.toArray();
    if (array.isEmpty()) {
      return fail("probe.member", QStringLiteral("references names no stage input"));
    }
    for (const QJsonValue& entry : array) {
      if (!entry.isObject()) {
        return fail("probe.type", QStringLiteral("a stage reference is not an object"));
      }
      const QJsonObject reference = entry.toObject();
      QString offender;
      if (!memberOnly(reference,
                      QStringList{QStringLiteral("fixture"), QStringLiteral("raw")},
                      &offender)) {
        return unknown(QStringLiteral("references.") + offender);
      }
      BuilderStageReference stageReference;
      const bool hasFixture = reference.contains(QStringLiteral("fixture"));
      const bool hasRaw = reference.contains(QStringLiteral("raw"));
      if (hasFixture == hasRaw) {
        return fail("probe.reference.mode",
                    QStringLiteral("a stage reference is one fixture or one raw-input list"));
      }
      if (hasFixture) {
        if (!memberPath(reference, "fixture", &stageReference.fixturePath)) {
          return fail("probe.path", QStringLiteral("a fixture must be an absolute path"));
        }
      } else {
        const QJsonValue rawValue = reference.value(QStringLiteral("raw"));
        if (!rawValue.isArray() || rawValue.toArray().isEmpty()) {
          return fail("probe.member", QStringLiteral("raw is missing or names no tensor"));
        }
        for (const QJsonValue& rawEntry : rawValue.toArray()) {
          if (!rawEntry.isObject()) {
            return fail("probe.type", QStringLiteral("a raw input is not an object"));
          }
          const QJsonObject raw = rawEntry.toObject();
          if (!memberOnly(raw,
                          QStringList{QStringLiteral("name"), QStringLiteral("dtype"),
                                      QStringLiteral("shape"), QStringLiteral("path")},
                          &offender)) {
            return unknown(QStringLiteral("references.raw.") + offender);
          }
          BuilderRawInput input;
          if (!memberString(raw, "name", &input.name) ||
              !memberString(raw, "dtype", &input.dtype) ||
              !memberString(raw, "shape", &input.shape) ||
              !memberPath(raw, "path", &input.filePath)) {
            return fail("probe.member",
                        QStringLiteral("a raw input needs a name, dtype, shape and absolute path"));
          }
          stageReference.rawInputs.append(input);
        }
      }
      parsed.request.references.append(stageReference);
    }
  }

  *out = parsed;
  if (reasonKey != nullptr) {
    reasonKey->clear();
  }
  if (reason != nullptr) {
    reason->clear();
  }
  return true;
}

}  // namespace native
}  // namespace mplayer
