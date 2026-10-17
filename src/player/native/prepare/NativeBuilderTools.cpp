#include "NativeBuilderInternal.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>

#include <optional>

#include <algorithm>

namespace mplayer {
namespace native {

namespace builder_detail {

BuilderResult refuse(const QString& key, const QString& text, int stage,
                     const QString& phase, const QString& toolError) {
  BuilderResult result;
  result.reasonKey = key;
  result.reason = redactNativeDiagnosticText(text);
  result.stageIndex = stage;
  result.phase = phase;
  result.toolError = redactNativeDiagnosticText(toolError);
  return result;
}

BuilderResult cancelledResult(const QString& phase, int stage) {
  BuilderResult result;
  result.cancelled = true;
  result.reasonKey = QStringLiteral("build.cancelled");
  result.reason = QStringLiteral("the build was cancelled before it could commit anything");
  result.stageIndex = stage;
  result.phase = phase;
  return result;
}

void emitProgress(const BuilderProgressCallback& callback, int stage, const QString& model,
                  const QString& phase, const QString& detail, const QString& log) {
  if (!callback) {
    return;
  }
  BuilderProgress progress;
  progress.stageIndex = stage;
  progress.modelId = model;
  progress.phase = phase;
  progress.detail = redactNativeDiagnosticText(detail);
  progress.log = redactNativeDiagnosticText(log);
  callback(progress);
}

bool isCancelled(NativeBuildCancellation* cancellation) {
  return cancellation != nullptr && cancellation->isCancelled();
}

QString tailText(const QByteArray& bytes, int maxBytes) {
  return QString::fromUtf8(bytes.right(maxBytes));
}

void appendBounded(QByteArray* kept, const QByteArray& chunk) {
  if (chunk.isEmpty()) {
    return;
  }
  kept->append(chunk);
  if (kept->size() > kMaxToolBytes) {
    kept->remove(0, static_cast<int>(kept->size() - kMaxToolBytes));
  }
}

bool parseObject(const QByteArray& bytes, QJsonObject* object, QString* error) {
  QJsonParseError parseError{};
  const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    if (error != nullptr) {
      *error = parseError.errorString();
    }
    return false;
  }
  *object = document.object();
  return true;
}

bool shapeDims(const QString& text, qint64 (&dims)[4]) {
  const QStringList parts = text.split(QLatin1Char('x'));
  if (parts.size() != 4) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    bool ok = false;
    const qint64 value = parts.at(i).toLongLong(&ok);
    if (!ok || value <= 0 || value > kMaxExtent) {
      return false;
    }
    dims[i] = value;
  }
  return true;
}

QString dimsText(const QJsonArray& array) {
  QStringList parts;
  for (const QJsonValue& value : array) {
    parts.append(QString::number(static_cast<qint64>(value.toDouble())));
  }
  return parts.join(QLatin1Char('x'));
}

int elementBytes(const QString& dtype) {
  if (dtype == QLatin1String("fp16")) {
    return 2;
  }
  if (dtype == QLatin1String("fp32")) {
    return 4;
  }
  return 0;
}

bool plainPathNoReparse(const QString& canonical) {
  if (canonical.isEmpty()) {
    return false;
  }
  const QString cleaned = QDir::cleanPath(canonical);
  const QStringList parts = cleaned.split(QLatin1Char('/'), Qt::SkipEmptyParts);
  if (cleaned.size() >= 2 && cleaned.at(1) == QLatin1Char(':')) {
    if (parts.isEmpty()) {
      return false;
    }
    QString current = parts.first() + QLatin1Char('/');
    if (!pathHasNoReparsePoints(current, current)) {
      return false;
    }
    for (int i = 1; i < parts.size(); ++i) {
      current += parts.at(i);
      if (!pathHasNoReparsePoints(current, current)) {
        return false;
      }
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
  if (!current.isEmpty() && !pathHasNoReparsePoints(current, current)) {
    return false;
  }
  for (const QString& part : parts) {
    if (current.endsWith(QLatin1Char('/'))) {
      current += part;
    } else {
      current += QLatin1Char('/') + part;
    }
    if (!pathHasNoReparsePoints(current, current)) {
      return false;
    }
  }
  return true;
}

bool canonicalDir(const QString& path, QString* canonicalOut) {
  if (path.isEmpty() || !QDir::isAbsolutePath(path)) {
    return false;
  }
  if (QFileInfo(path).isSymLink()) {
    return false;
  }
  const QString canonical = canonicalPath(path);
  if (canonical.isEmpty() || !QFileInfo(canonical).isDir()) {
    return false;
  }
  if (QFileInfo(canonical).isSymLink()) {
    return false;
  }
  if (!plainPathNoReparse(canonical)) {
    return false;
  }
  if (canonicalOut != nullptr) {
    *canonicalOut = canonical;
  }
  return true;
}

bool canonicalExe(const QString& path, QString* canonicalOut) {
  if (path.isEmpty() || !QDir::isAbsolutePath(path)) {
    return false;
  }
  if (QFileInfo(path).isSymLink()) {
    return false;
  }
  const QString canonical = canonicalPath(path);
  if (canonical.isEmpty() || !QFileInfo(canonical).isFile()) {
    return false;
  }
  if (QFileInfo(canonical).isSymLink()) {
    return false;
  }
  if (!plainPathNoReparse(canonical)) {
    return false;
  }
  if (canonicalOut != nullptr) {
    *canonicalOut = canonical;
  }
  return true;
}

bool canonicalFileInside(const QString& rootCanonical, const QString& path,
                         QString* canonicalOut) {
  if (path.isEmpty() || !QDir::isAbsolutePath(path)) {
    return false;
  }
  if (QFileInfo(path).isSymLink()) {
    return false;
  }
  const QString canonical = canonicalPath(path);
  if (canonical.isEmpty() || !QFileInfo(canonical).isFile()) {
    return false;
  }
  if (QFileInfo(canonical).isSymLink()) {
    return false;
  }
  if (!pathWithinRoot(rootCanonical, canonical)) {
    return false;
  }
  if (!pathHasNoReparsePoints(rootCanonical, canonical)) {
    return false;
  }
  if (canonicalOut != nullptr) {
    *canonicalOut = canonical;
  }
  return true;
}


QString toolExitCause(const ToolResult& run) {
  const QString status = run.exitStatus == QProcess::NormalExit
                             ? QStringLiteral("normal exit")
                             : QStringLiteral("abnormal termination");
  QString code = QString::number(run.exitCode);
  if (run.exitStatus != QProcess::NormalExit && run.exitCode != 0) {
    code = QStringLiteral("%1 (0x%2)")
               .arg(run.exitCode)
               .arg(static_cast<quint32>(run.exitCode), 8, 16, QLatin1Char('0'));
  }
  QString text = QStringLiteral("the tool ended with %1, code %2").arg(status, code);
  if (!run.processError.isEmpty() &&
      run.processError != QLatin1String("Unknown error")) {
    text += QStringLiteral(" (%1)").arg(run.processError);
  }
  return text;
}

QString toolPathList(const ToolPaths& tools) {
  QStringList parts;
  if (!tools.trtBin.isEmpty()) {
    parts.append(QDir::toNativeSeparators(tools.trtBin));
  }
  if (!tools.cudaBin.isEmpty()) {
    parts.append(QDir::toNativeSeparators(tools.cudaBin));
  }
  const QString systemRoot =
      QProcessEnvironment::systemEnvironment().value(QStringLiteral("SystemRoot"),
                                                     QStringLiteral("C:\\Windows"));
  parts.append(QDir::toNativeSeparators(systemRoot + QStringLiteral("\\System32")));
  parts.append(QDir::toNativeSeparators(systemRoot));
  return parts.join(QLatin1Char(';'));
}

QProcessEnvironment childEnvironment(const ToolPaths& tools, bool forInspect) {
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("PATH"), toolPathList(tools));
  environment.remove(QStringLiteral("LD_LIBRARY_PATH"));
  environment.remove(QStringLiteral("DYLD_LIBRARY_PATH"));
  if (forInspect) {
    const QString exeDir = QFileInfo(QCoreApplication::applicationFilePath()).absolutePath();
    if (!exeDir.isEmpty()) {
      environment.insert(QStringLiteral("PATH"),
                         QDir::toNativeSeparators(exeDir) + QLatin1Char(';') +
                             environment.value(QStringLiteral("PATH")));
    }
  }
  return environment;
}

bool drainOnce(QProcess* process, ToolResult* result, const BuilderProgressCallback& progress,
               int stage, const QString& model, const QString& phase) {
  const QByteArray outChunk = process->readAllStandardOutput();
  const QByteArray errChunk = process->readAllStandardError();
  if (outChunk.isEmpty() && errChunk.isEmpty()) {
    return false;
  }
  if (result->output.size() + outChunk.size() > kMaxToolBytes) {
    result->sawOutputLimit = true;
  }
  if (result->error.size() + errChunk.size() > kMaxToolBytes) {
    result->sawOutputLimit = true;
  }
  appendBounded(&result->output, outChunk);
  appendBounded(&result->error, errChunk);
  emitProgress(progress, stage, model, phase, QStringLiteral("running"),
               tailText(errChunk, 500));
  return true;
}

bool waitStart(QProcess* process, ToolResult* result,
               NativeBuildCancellation* cancellation) {
  for (int i = 0; i < kStartSlices; ++i) {
    if (process->waitForStarted(kSliceMs)) {
      return true;
    }
    if (isCancelled(cancellation)) {
      return false;
    }
    if (process->state() == QProcess::NotRunning &&
        process->error() != QProcess::Timedout) {
      result->launchError = process->errorString();
      return false;
    }
  }
  result->launchError = QStringLiteral("the tool did not start within the start budget");
  return false;
}

bool awaitStop(QProcess* process, ToolResult* result) {
  if (process->state() == QProcess::NotRunning) {
    result->stopped = true;
    return true;
  }
  process->kill();
  for (int i = 0; i < kStopSlices; ++i) {
    if (process->waitForFinished(kSliceMs)) {
      result->stopped = true;
      return true;
    }
  }
  return false;
}

bool runBlocking(const QString& program, const QStringList& arguments,
                 const QProcessEnvironment& environment,
                 NativeBuildCancellation* cancellation,
                 const BuilderProgressCallback& progress, int stage, const QString& model,
                 const QString& phase, ToolResult* result) {
  QProcess process;
  process.setProgram(program);
  process.setArguments(arguments);
  process.setProcessEnvironment(environment);
  process.start();
  if (!waitStart(&process, result, cancellation)) {
    if (process.state() != QProcess::NotRunning) {
      awaitStop(&process, result);
    }
    appendBounded(&result->output, process.readAllStandardOutput());
    appendBounded(&result->error, process.readAllStandardError());
    result->exitCode = process.exitCode();
    result->exitStatus = process.exitStatus();
    result->processError = process.errorString();
    return false;
  }
  for (;;) {
    if (process.waitForFinished(kSliceMs)) {
      break;
    }
    drainOnce(&process, result, progress, stage, model, phase);
    if (isCancelled(cancellation)) {
      appendBounded(&result->output, process.readAllStandardOutput());
      appendBounded(&result->error, process.readAllStandardError());
      if (!awaitStop(&process, result)) {
        result->launchError = QStringLiteral("the helper did not stop after kill");
        appendBounded(&result->output, process.readAllStandardOutput());
        appendBounded(&result->error, process.readAllStandardError());
        result->exitCode = process.exitCode();
        result->exitStatus = process.exitStatus();
        result->processError = process.errorString();
        return false;
      }
      appendBounded(&result->output, process.readAllStandardOutput());
      appendBounded(&result->error, process.readAllStandardError());
      result->exitCode = process.exitCode();
      result->exitStatus = process.exitStatus();
      result->processError = process.errorString();
      return false;
    }
  }
  appendBounded(&result->output, process.readAllStandardOutput());
  appendBounded(&result->error, process.readAllStandardError());
  if (result->output.size() >= kMaxToolBytes || result->error.size() >= kMaxToolBytes) {
    result->sawOutputLimit = true;
  }
  result->exitCode = process.exitCode();
  result->exitStatus = process.exitStatus();
  result->processError = process.errorString();
  return true;
}


bool tensorBytes(const QString& dtype, const qint64 (&dims)[4], qint64* bytesOut) {
  const int elem = elementBytes(dtype);
  if (elem == 0) {
    return false;
  }
  qint64 elements = 1;
  for (int i = 0; i < 4; ++i) {
    if (dims[i] <= 0 || elements > kMaxTensorBytes / dims[i]) {
      return false;
    }
    elements *= dims[i];
  }
  if (elements > kMaxTensorBytes / elem) {
    return false;
  }
  *bytesOut = elements * elem;
  return true;
}

bool refTensors(const QJsonObject& manifest, const QString& refDir, const QString& key,
                QVector<RefTensor>* out, QString* failKey, QString* failReason) {
  const QJsonValue listValue = manifest.value(key);
  if (!listValue.isArray()) {
    if (failKey != nullptr) {
      *failKey = QStringLiteral("reference.manifest.inputs");
    }
    if (failReason != nullptr) {
      *failReason = QStringLiteral("reference manifest has no ") + key;
    }
    return false;
  }
  const QJsonArray list = listValue.toArray();
  for (const QJsonValue& entry : list) {
    if (!entry.isObject()) {
      if (failKey != nullptr) {
        *failKey = QStringLiteral("reference.manifest.inputs");
      }
      if (failReason != nullptr) {
        *failReason = QStringLiteral("reference entry is not an object");
      }
      return false;
    }
    const QJsonObject object = entry.toObject();
    RefTensor tensor;
    tensor.name = object.value(QStringLiteral("name")).toString();
    tensor.dtype = object.value(QStringLiteral("dtype")).toString();
    tensor.shape = dimsText(object.value(QStringLiteral("shape")).toArray());
    tensor.file = object.value(QStringLiteral("file")).toString();
    tensor.sha256 = object.value(QStringLiteral("sha256")).toString();
    tensor.bytes = static_cast<qint64>(object.value(QStringLiteral("bytes")).toDouble(-1));
    if (tensor.name.isEmpty() || tensor.shape.isEmpty() || tensor.file.isEmpty()) {
      if (failKey != nullptr) {
        *failKey = QStringLiteral("reference.manifest.inputs");
      }
      if (failReason != nullptr) {
        *failReason = QStringLiteral("reference entry misses a field");
      }
      return false;
    }
    if (elementBytes(tensor.dtype) == 0) {
      if (failKey != nullptr) {
        *failKey = QStringLiteral("reference.inputs.dtype");
      }
      if (failReason != nullptr) {
        *failReason = QStringLiteral("reference tensor is not fp16/fp32");
      }
      return false;
    }
    if (!isSha256Hex(tensor.sha256)) {
      if (failKey != nullptr) {
        *failKey = QStringLiteral("reference.manifest.inputs");
      }
      if (failReason != nullptr) {
        *failReason = QStringLiteral("reference entry has no sha256");
      }
      return false;
    }
    qint64 dims[4] = {0, 0, 0, 0};
    if (!shapeDims(tensor.shape, dims)) {
      if (failKey != nullptr) {
        *failKey = QStringLiteral("reference.inputs.shape");
      }
      if (failReason != nullptr) {
        *failReason = QStringLiteral("reference tensor shape is not NxCxHxW");
      }
      return false;
    }
    qint64 want = 0;
    if (!tensorBytes(tensor.dtype, dims, &want) || want != tensor.bytes) {
      if (failKey != nullptr) {
        *failKey = QStringLiteral("reference.inputs.bytes");
      }
      if (failReason != nullptr) {
        *failReason = QStringLiteral("reference entry bytes do not match dtype and shape");
      }
      return false;
    }
    QString nameReason;
    const QString safe = safeRelativePath(tensor.file, &nameReason);
    if (safe.isEmpty() || safe.contains(QLatin1Char('/'))) {
      if (failKey != nullptr) {
        *failKey = QStringLiteral("reference.manifest.inputs");
      }
      if (failReason != nullptr) {
        *failReason = QStringLiteral("reference entry file is not a flat relative name");
      }
      return false;
    }
    const QString joined = QDir(refDir).filePath(safe);
    QString canonical;
    if (!canonicalFileInside(QDir::cleanPath(QFileInfo(refDir).absoluteFilePath()), joined,
                             &canonical)) {
      if (failKey != nullptr) {
        *failKey = QStringLiteral("reference.manifest.inputs");
      }
      if (failReason != nullptr) {
        *failReason = QStringLiteral("reference file escapes its directory");
      }
      return false;
    }
    QString measureReason;
    const std::optional<FileDigest> measured = measureFile(canonical, &measureReason);
    if (!measured.has_value()) {
      if (failKey != nullptr) {
        *failKey = QStringLiteral("reference.inputs.unreadable");
      }
      if (failReason != nullptr) {
        *failReason = measureReason;
      }
      return false;
    }
    if (measured->sha256.toLower() != tensor.sha256.toLower() ||
        measured->bytes != tensor.bytes) {
      if (failKey != nullptr) {
        *failKey = QStringLiteral("reference.inputs.bytes");
      }
      if (failReason != nullptr) {
        *failReason = QStringLiteral("reference file is not the recorded bytes");
      }
      return false;
    }
    tensor.file = canonical;
    out->append(tensor);
  }
  if (out->isEmpty()) {
    if (failKey != nullptr) {
      *failKey = QStringLiteral("reference.manifest.inputs");
    }
    if (failReason != nullptr) {
      *failReason = QStringLiteral("reference lists no tensors");
    }
    return false;
  }
  return true;
}


bool resolveTools(const BuilderToolPaths& raw, ResolvedTools* out, QString* key,
                  QString* reason) {
  ToolPaths resolved;
  if (!canonicalExe(raw.inspectExecutable, &resolved.inspect)) {
    if (key != nullptr) {
      *key = QStringLiteral("tool.missing");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("native_inspect path is not a plain file");
    }
    return false;
  }
  if (!canonicalExe(raw.prepExecutable, &resolved.prep)) {
    if (key != nullptr) {
      *key = QStringLiteral("tool.missing");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("prep helper path is not a plain file");
    }
    return false;
  }
  if (!canonicalExe(raw.verifierExecutable, &resolved.verify)) {
    if (key != nullptr) {
      *key = QStringLiteral("tool.missing");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("verifier path is not a plain file");
    }
    return false;
  }
  if (!canonicalDir(raw.ortRuntimeDir, &resolved.ortDir)) {
    if (key != nullptr) {
      *key = QStringLiteral("tool.runtime.missing");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("ORT runtime directory is not a plain directory");
    }
    return false;
  }
  QString ortModule;
  const QString ortJoined = QDir(resolved.ortDir).filePath(QString::fromLatin1(kOrtModule));
  if (!canonicalFileInside(resolved.ortDir, ortJoined, &ortModule)) {
    if (key != nullptr) {
      *key = QStringLiteral("tool.runtime.missing");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("ORT runtime directory holds no plain onnxruntime.dll");
    }
    return false;
  }
  if (!raw.trtBinDir.isEmpty() && !canonicalDir(raw.trtBinDir, &resolved.trtBin)) {
    if (key != nullptr) {
      *key = QStringLiteral("tool.runtime.missing");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("TensorRT bin directory is not a plain directory");
    }
    return false;
  }
  if (!raw.cudaBinDir.isEmpty() && !canonicalDir(raw.cudaBinDir, &resolved.cudaBin)) {
    if (key != nullptr) {
      *key = QStringLiteral("tool.runtime.missing");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("CUDA bin directory is not a plain directory");
    }
    return false;
  }
  out->paths = resolved;
  out->prepVerifyEnv = childEnvironment(resolved, false);
  out->inspectEnv = childEnvironment(resolved, true);
  return true;
}

bool proveOwnedModel(const QString& modelRootCanonical, const StoredModel& model,
                     QString* storedCanonicalOut, FileDigest* digestOut, QString* key,
                     QString* reason) {
  if (model.storedPath.isEmpty()) {
    if (key != nullptr) {
      *key = QStringLiteral("model.owned.missing");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("the stage names no owned copy");
    }
    return false;
  }
  if (!isSha256Hex(model.record.onnxSha256)) {
    if (key != nullptr) {
      *key = QStringLiteral("model.identity.missing");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("the row has no digest to compare against");
    }
    return false;
  }
  QString canonical;
  if (!canonicalFileInside(modelRootCanonical, model.storedPath, &canonical)) {
    if (key != nullptr) {
      *key = QStringLiteral("model.owned.missing");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("the owned copy is not a plain file inside the owned root");
    }
    return false;
  }
  QString measureReason;
  const std::optional<FileDigest> measured = measureFile(canonical, &measureReason);
  if (!measured.has_value()) {
    if (key != nullptr) {
      *key = QStringLiteral("model.owned.changed");
    }
    if (reason != nullptr) {
      *reason = measureReason;
    }
    return false;
  }
  if (measured->sha256.toLower() != model.record.onnxSha256.toLower() ||
      measured->bytes != model.record.onnxBytes) {
    if (key != nullptr) {
      *key = QStringLiteral("model.owned.changed");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("the owned copy no longer matches its recorded bytes");
    }
    return false;
  }
  const QString derivedId = userModelIdFor(
      computeContentId(model.record.role, model.record.onnxSha256, model.record.actualProfile));
  if (derivedId != model.record.id) {
    if (key != nullptr) {
      *key = QStringLiteral("model.owned.changed");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("the row id no longer derives from its digest and profile");
    }
    return false;
  }
  if (storedCanonicalOut != nullptr) {
    *storedCanonicalOut = canonical;
  }
  if (digestOut != nullptr) {
    *digestOut = *measured;
  }
  return true;
}

bool reqString(const QJsonObject& object, const char* key, QString* out) {
  const QJsonValue value = object.value(QString::fromLatin1(key));
  if (!value.isString() || value.toString().isEmpty()) {
    return false;
  }
  *out = value.toString();
  return true;
}

bool reqInt(const QJsonObject& object, const char* key, qint64* out) {
  const QJsonValue value = object.value(QString::fromLatin1(key));
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

bool reqUint(const QJsonObject& object, const char* key, quint64* out) {
  qint64 integer = 0;
  if (!reqInt(object, key, &integer) || integer < 0) {
    return false;
  }
  *out = static_cast<quint64>(integer);
  return true;
}

bool declaredDimsFromArray(const QJsonArray& array, qint64 (&dims)[4], bool allowDynamic) {
  if (array.size() != 4) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    if (!array.at(i).isDouble()) {
      return false;
    }
    const double number = array.at(i).toDouble();
    const qint64 integer = static_cast<qint64>(number);
    if (static_cast<double>(integer) != number) {
      return false;
    }
    if (integer == -1 && allowDynamic) {
      dims[i] = -1;
      continue;
    }
    if (integer <= 0 || integer > kMaxExtent) {
      return false;
    }
    dims[i] = integer;
  }
  return true;
}

/// A stride is an offset inside the tensor, not an extent: the outermost stride
/// of a contiguous 1x3x360x640 fp16 tensor is 691200 elements, far past the
/// per-axis extent bound a shape is held to. A stride is still bounded by the
/// tensor's own outermost contiguous offset, which it can never exceed.
bool stridesFromArray(const QJsonArray& array, qint64 (&strides)[4], qint64 maxStride) {
  if (array.size() != 4) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    if (!array.at(i).isDouble()) {
      return false;
    }
    const double number = array.at(i).toDouble();
    const qint64 integer = static_cast<qint64>(number);
    if (static_cast<double>(integer) != number) {
      return false;
    }
    if (integer <= 0 || integer > maxStride) {
      return false;
    }
    strides[i] = integer;
  }
  return true;
}


bool parseDeviceEvent(const QJsonObject& event, MeasuredPrep* measured, QString* key,
                      QString* reason) {
  auto fail = [&](const char* field) {
    if (key != nullptr) {
      *key = QStringLiteral("build.event.device");
    }
    if (reason != nullptr) {
      *reason = QString(QStringLiteral("device-selected has no usable ")) +
                QString::fromLatin1(field);
    }
    return false;
  };
  qint64 ordinal = 0;
  qint64 memory = 0;
  qint64 processors = 0;
  qint64 cudaRuntime = 0;
  qint64 cudaDriverApi = 0;
  qint64 trtLib = 0;
  if (!reqInt(event, "deviceOrdinal", &ordinal)) {
    return fail("deviceOrdinal");
  }
  if (!reqString(event, "deviceUuid", &measured->deviceUuid)) {
    return fail("deviceUuid");
  }
  if (!reqString(event, "deviceName", &measured->deviceName)) {
    return fail("deviceName");
  }
  if (!reqString(event, "computeCapability", &measured->computeCapability)) {
    return fail("computeCapability");
  }
  if (!reqInt(event, "totalGlobalMemoryBytes", &memory) || memory <= 0) {
    return fail("totalGlobalMemoryBytes");
  }
  if (!reqInt(event, "multiProcessorCount", &processors) || processors <= 0) {
    return fail("multiProcessorCount");
  }
  if (!reqInt(event, "cudaRuntimeVersion", &cudaRuntime) || cudaRuntime <= 0) {
    return fail("cudaRuntimeVersion");
  }
  measured->cudaRuntimeVersion = static_cast<int>(cudaRuntime);
  if (!reqInt(event, "cudaDriverApiVersion", &cudaDriverApi) || cudaDriverApi <= 0) {
    return fail("cudaDriverApiVersion");
  }
  measured->cudaDriverApiVersion = static_cast<int>(cudaDriverApi);
  if (!reqString(event, "cudaDriverFileVersion", &measured->cudaDriverFileVersion)) {
    return fail("cudaDriverFileVersion");
  }
  if (!reqInt(event, "tensorRtLibVersion", &trtLib) || trtLib <= 0) {
    return fail("tensorRtLibVersion");
  }
  if (!reqUint(event, "workspaceBytesRequested", &measured->workspaceRequested)) {
    return fail("workspaceBytesRequested");
  }
  if (!reqString(event, "workspaceBytesSource", &measured->workspaceSource)) {
    return fail("workspaceBytesSource");
  }
  measured->deviceOrdinal = static_cast<int>(ordinal);
  measured->totalGlobalMemory = static_cast<quint64>(memory);
  measured->multiProcessorCount = static_cast<int>(processors);
  measured->tensorRtLibVersion = static_cast<int>(trtLib);
  return true;
}

bool parseGraphEvent(const QJsonObject& event, MeasuredPrep* measured, QString* key,
                      QString* reason) {
  auto fail = [&](const char* field) {
    if (key != nullptr) {
      *key = QStringLiteral("build.event.parsed");
    }
    if (reason != nullptr) {
      *reason = QString(QStringLiteral("graph-parsed has no usable ")) +
                QString::fromLatin1(field);
    }
    return false;
  };
  if (!reqString(event, "onnxSha256", &measured->onnxSha)) {
    return fail("onnxSha256");
  }
  if (!isSha256Hex(measured->onnxSha)) {
    return fail("onnxSha256");
  }
  qint64 bytes = 0;
  if (!reqInt(event, "onnxBytes", &bytes) || bytes <= 0) {
    return fail("onnxBytes");
  }
  measured->onnxBytes = bytes;
  const QJsonValue bindingsValue = event.value(QStringLiteral("bindings"));
  if (!bindingsValue.isArray()) {
    return fail("bindings");
  }
  const QJsonArray bindings = bindingsValue.toArray();
  bool sawInput = false;
  bool sawOutput = false;
  for (const QJsonValue& entry : bindings) {
    if (!entry.isObject()) {
      return fail("bindings");
    }
    const QJsonObject binding = entry.toObject();
    QString name;
    QString role;
    QString type;
    QString format;
    if (!reqString(binding, "name", &name)) {
      return fail("bindings.name");
    }
    if (!reqString(binding, "role", &role)) {
      return fail("bindings.role");
    }
    if (!reqString(binding, "type", &type)) {
      return fail("bindings.type");
    }
    if (!reqString(binding, "format", &format)) {
      return fail("bindings.format");
    }
    if (format != QLatin1String("linear")) {
      return fail("bindings.format");
    }
    qint64 dims[4] = {0, 0, 0, 0};
    if (!declaredDimsFromArray(binding.value(QStringLiteral("dims")).toArray(), dims, true)) {
      return fail("bindings.dims");
    }
    if (role == QLatin1String("input")) {
      if (sawInput) {
        return fail("bindings");
      }
      sawInput = true;
      measured->inName = name;
      measured->inType = type;
      for (int i = 0; i < 4; ++i) {
        measured->inDims[i] = dims[i];
      }
    } else if (role == QLatin1String("output")) {
      if (sawOutput) {
        return fail("bindings");
      }
      sawOutput = true;
      measured->outName = name;
      measured->outType = type;
      for (int i = 0; i < 4; ++i) {
        measured->outDims[i] = dims[i];
      }
    } else {
      return fail("bindings.role");
    }
  }
  if (!sawInput || !sawOutput) {
    return fail("bindings");
  }
  return true;
}

bool parseStartedEvent(const QJsonObject& event, MeasuredPrep* measured, QString* key,
                       QString* reason) {
  auto fail = [&](const char* field) {
    if (key != nullptr) {
      *key = QStringLiteral("build.event.started");
    }
    if (reason != nullptr) {
      *reason = QString(QStringLiteral("build-started has no usable ")) +
                QString::fromLatin1(field);
    }
    return false;
  };
  BuilderPrepIdentity flags;
  quint64 number = 0;
  qint64 level = 0;
  if (!reqUint(event, "builderFlags", &number)) {
    return fail("builderFlags");
  }
  flags.builderFlags = number;
  flags.builderFlagsPresent = true;
  if (!event.contains(QStringLiteral("tf32Allowed")) ||
      !event.value(QStringLiteral("tf32Allowed")).isBool()) {
    return fail("tf32Allowed");
  }
  flags.tf32Allowed = event.value(QStringLiteral("tf32Allowed")).toBool();
  flags.tf32AllowedPresent = true;
  if (!reqString(event, "engineCapability", &flags.engineCapability)) {
    return fail("engineCapability");
  }
  flags.engineCapabilityPresent = true;
  if (!reqInt(event, "builderOptimizationLevel", &level) || level < 0) {
    return fail("builderOptimizationLevel");
  }
  flags.builderOptimizationLevel = static_cast<int>(level);
  flags.builderOptimizationLevelPresent = true;
  if (!reqUint(event, "tacticSources", &number)) {
    return fail("tacticSources");
  }
  flags.tacticSources = number;
  flags.tacticSourcesPresent = true;
  if (!reqUint(event, "networkFlags", &number)) {
    return fail("networkFlags");
  }
  flags.networkFlags = number;
  flags.networkFlagsPresent = true;
  if (!event.contains(QStringLiteral("stronglyTyped")) ||
      !event.value(QStringLiteral("stronglyTyped")).isBool()) {
    return fail("stronglyTyped");
  }
  flags.stronglyTyped = event.value(QStringLiteral("stronglyTyped")).toBool();
  flags.stronglyTypedPresent = true;
  qint64 workspace = 0;
  if (!reqInt(event, "workspaceBytes", &workspace) || workspace <= 0) {
    return fail("workspaceBytes");
  }
  measured->flags = flags;
  measured->workspaceBytes = static_cast<quint64>(workspace);
  return true;
}

bool parseRawEvent(const QJsonObject& event, MeasuredPrep* measured, QString* key,
                   QString* reason) {
  auto fail = [&](const char* field) {
    if (key != nullptr) {
      *key = QStringLiteral("build.event.raw");
    }
    if (reason != nullptr) {
      *reason = QString(QStringLiteral("raw-produced has no usable ")) +
                QString::fromLatin1(field);
    }
    return false;
  };
  if (!reqString(event, "engineSha256", &measured->rawEngineSha)) {
    return fail("engineSha256");
  }
  if (!isSha256Hex(measured->rawEngineSha)) {
    return fail("engineSha256");
  }
  qint64 engineBytes = 0;
  qint64 onnxBytes = 0;
  quint64 workspace = 0;
  quint64 requested = 0;
  qint64 trtLib = 0;
  if (!reqInt(event, "engineBytes", &engineBytes) || engineBytes <= 0) {
    return fail("engineBytes");
  }
  if (!reqString(event, "onnxSha256", &measured->rawOnnxSha)) {
    return fail("onnxSha256");
  }
  if (!reqInt(event, "onnxBytes", &onnxBytes) || onnxBytes <= 0) {
    return fail("onnxBytes");
  }
  if (!reqString(event, "deviceUuid", &measured->rawDeviceUuid)) {
    return fail("deviceUuid");
  }
  if (!reqString(event, "computeCapability", &measured->rawComputeCapability)) {
    return fail("computeCapability");
  }
  if (!reqString(event, "cudaDriverFileVersion", &measured->rawDriverFileVersion)) {
    return fail("cudaDriverFileVersion");
  }
  if (!reqInt(event, "tensorRtLibVersion", &trtLib) || trtLib <= 0) {
    return fail("tensorRtLibVersion");
  }
  if (!reqUint(event, "workspaceBytesRequested", &requested)) {
    return fail("workspaceBytesRequested");
  }
  if (!reqString(event, "workspaceBytesSource", &measured->rawWorkspaceSource)) {
    return fail("workspaceBytesSource");
  }
  if (!reqUint(event, "workspaceBytes", &workspace) || workspace == 0) {
    return fail("workspaceBytes");
  }
  qint64 runtime = 0;
  if (!reqInt(event, "cudaRuntimeVersion", &runtime) || runtime <= 0) {
    return fail("cudaRuntimeVersion");
  }
  measured->rawCudaRuntimeVersion = static_cast<int>(runtime);
  qint64 api = 0;
  if (!reqInt(event, "cudaDriverApiVersion", &api) || api <= 0) {
    return fail("cudaDriverApiVersion");
  }
  measured->rawCudaDriverApiVersion = static_cast<int>(api);
  BuilderPrepIdentity rawFlags;
  quint64 number = 0;
  qint64 level = 0;
  if (!reqUint(event, "builderFlags", &number)) {
    return fail("builderFlags");
  }
  rawFlags.builderFlags = number;
  rawFlags.builderFlagsPresent = true;
  if (!event.contains(QStringLiteral("tf32Allowed")) ||
      !event.value(QStringLiteral("tf32Allowed")).isBool()) {
    return fail("tf32Allowed");
  }
  rawFlags.tf32Allowed = event.value(QStringLiteral("tf32Allowed")).toBool();
  rawFlags.tf32AllowedPresent = true;
  if (!reqString(event, "engineCapability", &rawFlags.engineCapability)) {
    return fail("engineCapability");
  }
  rawFlags.engineCapabilityPresent = true;
  if (!reqInt(event, "builderOptimizationLevel", &level) || level < 0) {
    return fail("builderOptimizationLevel");
  }
  rawFlags.builderOptimizationLevel = static_cast<int>(level);
  rawFlags.builderOptimizationLevelPresent = true;
  if (!reqUint(event, "tacticSources", &number)) {
    return fail("tacticSources");
  }
  rawFlags.tacticSources = number;
  rawFlags.tacticSourcesPresent = true;
  if (!reqUint(event, "networkFlags", &number)) {
    return fail("networkFlags");
  }
  rawFlags.networkFlags = number;
  rawFlags.networkFlagsPresent = true;
  if (!event.contains(QStringLiteral("stronglyTyped")) ||
      !event.value(QStringLiteral("stronglyTyped")).isBool()) {
    return fail("stronglyTyped");
  }
  rawFlags.stronglyTyped = event.value(QStringLiteral("stronglyTyped")).toBool();
  rawFlags.stronglyTypedPresent = true;
  if (!reqString(event, "shapeBinding", &measured->rawShapeBinding)) {
    return fail("shapeBinding");
  }
  if (!reqString(event, "ioFormat", &measured->rawIoFormat)) {
    return fail("ioFormat");
  }
  measured->rawEngineBytes = engineBytes;
  measured->rawOnnxBytes = onnxBytes;
  measured->rawTensorRtLibVersion = static_cast<int>(trtLib);
  measured->rawWorkspaceRequested = requested;
  measured->rawWorkspaceBytes = workspace;
  measured->rawFlags = rawFlags;
  return true;
}

QVector<BuildOption> expectedOptions(const BuilderPrepIdentity& flags) {
  QVector<BuildOption> options;
  BuildOption builderFlags;
  builderFlags.name = QStringLiteral("builderFlags");
  builderFlags.value = QString::number(flags.builderFlags);
  builderFlags.hasValue = true;
  options.append(builderFlags);
  BuildOption tactics;
  tactics.name = QStringLiteral("tacticSources");
  tactics.value = QString::number(flags.tacticSources);
  tactics.hasValue = true;
  options.append(tactics);
  BuildOption netFlags;
  netFlags.name = QStringLiteral("networkFlags");
  netFlags.value = QString::number(flags.networkFlags);
  netFlags.hasValue = true;
  options.append(netFlags);
  BuildOption level;
  level.name = QStringLiteral("builderOptimizationLevel");
  level.value = QString::number(flags.builderOptimizationLevel);
  level.hasValue = true;
  options.append(level);
  BuildOption capability;
  capability.name = QStringLiteral("engineCapability");
  capability.value = flags.engineCapability;
  capability.hasValue = true;
  options.append(capability);
  return options;
}

/// The declared builder optimization level, read from the build identity's
/// options. Exactly one `builderOptimizationLevel` must be present and its value
/// an integer in 0..5. It is forwarded to the prep helper verbatim as
/// `--builder-optimization-level`, so the declared level is the level that is
/// built; a missing, repeated or out-of-range level refuses rather than silently
/// running the helper's own default.
bool declaredOptimizationLevel(const EngineBuildIdentity& build, int* levelOut,
                               QString* key, QString* reason) {
  int found = 0;
  int level = -1;
  for (const BuildOption& option : build.options) {
    if (option.name != QLatin1String("builderOptimizationLevel")) {
      continue;
    }
    ++found;
    bool ok = false;
    const int value = option.hasValue ? option.value.toInt(&ok) : -1;
    if (!option.hasValue || !ok || value < 0 || value > 5) {
      if (key != nullptr) {
        *key = QStringLiteral("build.options.optimizationLevel");
      }
      if (reason != nullptr) {
        *reason = QStringLiteral(
            "the declared builderOptimizationLevel is not an integer in 0..5");
      }
      return false;
    }
    level = value;
  }
  if (found != 1) {
    if (key != nullptr) {
      *key = QStringLiteral("build.options.optimizationLevel");
    }
    if (reason != nullptr) {
      *reason =
          found == 0
              ? QStringLiteral("the build options declare no builderOptimizationLevel")
              : QStringLiteral(
                    "the build options declare builderOptimizationLevel more than once");
    }
    return false;
  }
  if (levelOut != nullptr) {
    *levelOut = level;
  }
  return true;
}

bool sameFlags(const BuilderPrepIdentity& a, const BuilderPrepIdentity& b) {
  return a.builderFlags == b.builderFlags && a.tf32Allowed == b.tf32Allowed &&
         a.engineCapability == b.engineCapability &&
         a.builderOptimizationLevel == b.builderOptimizationLevel &&
         a.tacticSources == b.tacticSources && a.networkFlags == b.networkFlags &&
         a.stronglyTyped == b.stronglyTyped;
}

bool trtLibFromRuntime(const QString& runtime, int* libOut) {
  const QStringList parts = runtime.split(QLatin1Char('.'));
  if (parts.size() < 3) {
    return false;
  }
  bool ok = false;
  const int major = parts.at(0).toInt(&ok);
  if (!ok || major <= 0 || major > 99) {
    return false;
  }
  const int minor = parts.at(1).toInt(&ok);
  if (!ok || minor < 0 || minor > 99) {
    return false;
  }
  const int patch = parts.at(2).toInt(&ok);
  if (!ok || patch < 0 || patch > 99) {
    return false;
  }
  *libOut = major * 10000 + minor * 100 + patch;
  return true;
}

bool cudaRuntimeFromCuda(const QString& cuda, int* runtimeOut) {
  const QStringList parts = cuda.split(QLatin1Char('.'));
  if (parts.size() < 2) {
    return false;
  }
  bool ok = false;
  const int major = parts.at(0).toInt(&ok);
  if (!ok || major <= 0 || major > 99) {
    return false;
  }
  const int minor = parts.at(1).toInt(&ok);
  if (!ok || minor < 0 || minor > 99) {
    return false;
  }
  *runtimeOut = major * 1000 + minor * 10;
  return true;
}

bool checkMeasuredComplete(const MeasuredPrep& measured, QString* key, QString* reason) {
  auto fail = [&](const char* field) {
    if (key != nullptr) {
      *key = QStringLiteral("build.event.incomplete");
    }
    if (reason != nullptr) {
      *reason = QString(QStringLiteral("the helper omitted ")) +
                QString::fromLatin1(field);
    }
    return false;
  };
  if (measured.deviceUuid.isEmpty() || measured.computeCapability.isEmpty()) {
    return fail("device identity");
  }
  if (measured.tensorRtLibVersion <= 0 || measured.cudaRuntimeVersion <= 0) {
    return fail("versions");
  }
  if (measured.cudaDriverFileVersion.isEmpty() || measured.onnxSha.isEmpty()) {
    return fail("driver/graph identity");
  }
  if (measured.inName.isEmpty() || measured.outName.isEmpty()) {
    return fail("bindings");
  }
  if (!measured.flags.builderFlagsPresent || !measured.flags.tf32AllowedPresent ||
      !measured.flags.engineCapabilityPresent ||
      !measured.flags.builderOptimizationLevelPresent ||
      !measured.flags.tacticSourcesPresent || !measured.flags.networkFlagsPresent ||
      !measured.flags.stronglyTypedPresent) {
    return fail("builder flags");
  }
  if (measured.rawEngineSha.isEmpty() || measured.rawOnnxSha.isEmpty() ||
      measured.rawDeviceUuid.isEmpty() || measured.rawComputeCapability.isEmpty() ||
      measured.rawDriverFileVersion.isEmpty() ||
      measured.rawTensorRtLibVersion <= 0 || measured.rawWorkspaceSource.isEmpty() ||
      measured.rawShapeBinding.isEmpty() || measured.rawIoFormat.isEmpty()) {
    return fail("raw product identity");
  }
  if (!measured.rawFlags.builderFlagsPresent || !measured.rawFlags.tf32AllowedPresent ||
      !measured.rawFlags.engineCapabilityPresent ||
      !measured.rawFlags.builderOptimizationLevelPresent ||
      !measured.rawFlags.tacticSourcesPresent || !measured.rawFlags.networkFlagsPresent ||
      !measured.rawFlags.stronglyTypedPresent) {
    return fail("raw builder flags");
  }
  if (!sameFlags(measured.flags, measured.rawFlags)) {
    if (key != nullptr) {
      *key = QStringLiteral("build.identity.mismatch");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("build-started and raw-produced disagree on builder flags");
    }
    return false;
  }
  if (!isSameDeviceUuid(measured.deviceUuid, measured.rawDeviceUuid) ||
      measured.computeCapability != measured.rawComputeCapability ||
      measured.tensorRtLibVersion != measured.rawTensorRtLibVersion ||
      measured.cudaRuntimeVersion != measured.rawCudaRuntimeVersion ||
      measured.cudaDriverApiVersion != measured.rawCudaDriverApiVersion ||
      measured.cudaDriverFileVersion != measured.rawDriverFileVersion ||
      measured.workspaceRequested != measured.rawWorkspaceRequested ||
      measured.workspaceBytes != measured.rawWorkspaceBytes ||
      measured.workspaceSource != measured.rawWorkspaceSource ||
      measured.onnxSha.toLower() != measured.rawOnnxSha.toLower() ||
      measured.onnxBytes != measured.rawOnnxBytes) {
    if (key != nullptr) {
      *key = QStringLiteral("build.identity.mismatch");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("build-started and raw-produced disagree on device or product");
    }
    return false;
  }
  return true;
}

bool checkMeasuredAgainstBuild(const MeasuredPrep& measured, const EngineBuildIdentity& build,
                               quint64 workspaceRequest, QString* key, QString* reason) {
  auto fail = [&](const QString& k, const QString& t) {
    if (key != nullptr) {
      *key = k;
    }
    if (reason != nullptr) {
      *reason = t;
    }
    return false;
  };
  if (measured.flags.tf32Allowed) {
    return fail(QStringLiteral("build.tf32.mismatch"),
                QStringLiteral("the helper measured tf32Allowed=true"));
  }
  if (build.tf32) {
    return fail(QStringLiteral("build.tf32.mismatch"),
                QStringLiteral("the request claims tf32 with measured false"));
  }
  if (!measured.flags.stronglyTyped) {
    return fail(QStringLiteral("build.stronglyTyped.mismatch"),
                QStringLiteral("the helper did not measure stronglyTyped=true"));
  }
  if (!build.stronglyTyped) {
    return fail(QStringLiteral("build.stronglyTyped.mismatch"),
                QStringLiteral("the request claims untyped with measured true"));
  }
  if (!isSameDeviceUuid(measured.deviceUuid, build.deviceUuid)) {
    return fail(QStringLiteral("build.device.mismatch"),
                QStringLiteral("the helper selected another device"));
  }
  if (measured.computeCapability != build.smText()) {
    return fail(QStringLiteral("build.device.mismatch"),
                QStringLiteral("the helper SM is not the observed SM"));
  }
  int wantTrt = 0;
  if (!trtLibFromRuntime(build.runtimeVersion, &wantTrt) ||
      wantTrt != measured.tensorRtLibVersion) {
    return fail(QStringLiteral("build.runtime.mismatch"),
                QStringLiteral("the helper TensorRT library is not the observed runtime"));
  }
  int wantCuda = 0;
  if (!cudaRuntimeFromCuda(build.cudaVersion, &wantCuda) ||
      wantCuda != measured.cudaRuntimeVersion) {
    return fail(QStringLiteral("build.cuda.mismatch"),
                QStringLiteral("the helper CUDA runtime is not the observed CUDA"));
  }
  if (build.driverVersion != measured.cudaDriverFileVersion) {
    return fail(QStringLiteral("build.driver.mismatch"),
                QStringLiteral("the helper driver file is not the observed driver"));
  }
  if (workspaceRequest == 0) {
    if (measured.workspaceSource != QLatin1String("default")) {
      return fail(QStringLiteral("build.workspace.mismatch"),
                  QStringLiteral("the helper did not record its own default workspace"));
    }
  } else if (measured.workspaceBytes != workspaceRequest ||
             measured.workspaceRequested != workspaceRequest ||
             measured.workspaceSource != QLatin1String("flag")) {
    return fail(QStringLiteral("build.workspace.mismatch"),
                QStringLiteral("the helper workspace is not the requested bound"));
  }
  const QString wantOptions = canonicalizeBuildOptions(expectedOptions(measured.flags));
  if (build.canonicalOptions() != wantOptions) {
    return fail(QStringLiteral("build.options.mismatch"),
                QStringLiteral("the observed options do not encode the measured builder facts"));
  }
  return true;
}

bool canonicalAnyFile(const QString& path, QString* canonicalOut) {
  if (path.isEmpty() || !QDir::isAbsolutePath(path)) {
    return false;
  }
  if (QFileInfo(path).isSymLink()) {
    return false;
  }
  const QString canonical = canonicalPath(path);
  if (canonical.isEmpty() || !QFileInfo(canonical).isFile()) {
    return false;
  }
  if (QFileInfo(canonical).isSymLink()) {
    return false;
  }
  if (!plainPathNoReparse(canonical)) {
    return false;
  }
  if (canonicalOut != nullptr) {
    *canonicalOut = canonical;
  }
  return true;
}


bool preflight(const BuilderRequest& request, Preflight* out, QString* key,
               QString* reason) {
  auto fail = [&](const char* k, const QString& t) {
    if (key != nullptr) {
      *key = QString::fromLatin1(k);
    }
    if (reason != nullptr) {
      *reason = t;
    }
    return false;
  };
  const PreparationPlan& plan = request.plan;
  if (!plan.canPrepare()) {
    if (!plan.blockers.isEmpty()) {
      if (key != nullptr) {
        *key = plan.blockers.first().reasonKey;
      }
      if (reason != nullptr) {
        *reason = plan.blockers.first().reason;
      }
      return false;
    }
    if (key != nullptr) {
      *key = QStringLiteral("plan.preparing");
    }
    if (reason != nullptr) {
      *reason = plan.unknowns.isEmpty() ? QStringLiteral("the plan is not resolved")
                                         : plan.unknowns.first();
    }
    return false;
  }
  const int count = plan.stages.size();
  if (count <= 0) {
    return fail("plan.stages.empty", QStringLiteral("the plan resolved to no stages"));
  }
  if (request.models.size() != count || request.references.size() != count) {
    return fail("build.inputs.count",
                QStringLiteral("models and references need one entry per stage"));
  }
  const QString missing = request.build.firstMissingRequiredField();
  if (!missing.isEmpty()) {
    return fail("request.incomplete",
                QStringLiteral("the observed build identity records no ") + missing);
  }
  int optimizationLevel = 0;
  if (!declaredOptimizationLevel(request.build, &optimizationLevel, key, reason)) {
    return false;
  }
  if (request.build.provider != plan.backend) {
    return fail("build.provider.mismatch",
                QStringLiteral("the observed provider is not the planned backend"));
  }
  if (!plan.device.isKnown()) {
    return fail("plan.device.unknown",
                QStringLiteral("the plan names no observed device"));
  }
  if (!isSameDeviceUuid(request.build.deviceUuid, plan.device.computeDeviceUuid) ||
      request.build.smMajor != plan.device.smMajor ||
      request.build.smMinor != plan.device.smMinor) {
    return fail("build.device.mismatch",
                QStringLiteral("the observed device is not the planned device"));
  }
  if (!canonicalDir(request.modelRoot, &out->modelRootCanonical)) {
    return fail("model.root.unowned",
                QStringLiteral("the models root is not a plain owned directory"));
  }
  QString storeWhy;
  if (!request.store.isOwned(&storeWhy)) {
    if (key != nullptr) {
      *key = QStringLiteral("store.unowned");
    }
    if (reason != nullptr) {
      *reason = storeWhy.isEmpty() ? QStringLiteral("the artifact store is not app-owned")
                                   : storeWhy;
    }
    return false;
  }
  if (!resolveTools(request.tools, &out->tools, key, reason)) {
    return false;
  }
  return true;
}

bool validateCallerRef(const BuilderStageReference& ref, const ResolvedStage& stage,
                       QString* key, QString* reason) {
  const bool wantFixture = !ref.fixturePath.isEmpty();
  const bool wantRaw = !ref.rawInputs.isEmpty();
  if (wantFixture == wantRaw) {
    if (key != nullptr) {
      *key = QStringLiteral("reference.inputs.mode");
    }
    if (reason != nullptr) {
      *reason = QStringLiteral("one reference source per stage");
    }
    return false;
  }
  if (wantFixture) {
    if (stage.role != ModelRole::Spatial || stage.inputChannels != 3) {
      if (key != nullptr) {
        *key = QStringLiteral("reference.inputs.mode");
      }
      if (reason != nullptr) {
        *reason = QStringLiteral("a fixture supplies 1x3xHxW only");
      }
      return false;
    }
    QString canonical;
    if (!canonicalAnyFile(ref.fixturePath, &canonical)) {
      if (key != nullptr) {
        *key = QStringLiteral("fixture.unreadable");
      }
      if (reason != nullptr) {
        *reason = QStringLiteral("the fixture is not a plain file");
      }
      return false;
    }
    return true;
  }
  for (const BuilderRawInput& input : ref.rawInputs) {
    if (input.name.isEmpty()) {
      if (key != nullptr) {
        *key = QStringLiteral("reference.inputs.name");
      }
      if (reason != nullptr) {
        *reason = QStringLiteral("a raw input has no tensor name");
      }
      return false;
    }
    if (elementBytes(input.dtype) == 0) {
      if (key != nullptr) {
        *key = QStringLiteral("reference.inputs.dtype");
      }
      if (reason != nullptr) {
        *reason = QStringLiteral("a raw input is not fp16/fp32");
      }
      return false;
    }
    qint64 dims[4] = {0, 0, 0, 0};
    if (!shapeDims(input.shape, dims)) {
      if (key != nullptr) {
        *key = QStringLiteral("reference.inputs.shape");
      }
      if (reason != nullptr) {
        *reason = QStringLiteral("a raw input shape is not NxCxHxW");
      }
      return false;
    }
    if (dims[0] != 1 || dims[1] != stage.inputChannels ||
        dims[2] != stage.inputGeometry.height() || dims[3] != stage.inputGeometry.width()) {
      if (key != nullptr) {
        *key = QStringLiteral("reference.geometry.mismatch");
      }
      if (reason != nullptr) {
        *reason = QStringLiteral("a raw input is not the stage input geometry");
      }
      return false;
    }
    QString canonical;
    if (!canonicalAnyFile(input.filePath, &canonical)) {
      if (key != nullptr) {
        *key = QStringLiteral("reference.inputs.unreadable");
      }
      if (reason != nullptr) {
        *reason = QStringLiteral("a raw input file is not a plain file");
      }
      return false;
    }
    qint64 want = 0;
    if (!tensorBytes(input.dtype, dims, &want)) {
      if (key != nullptr) {
        *key = QStringLiteral("reference.inputs.bytes");
      }
      if (reason != nullptr) {
        *reason = QStringLiteral("a raw input exceeds the byte bound");
      }
      return false;
    }
    QString measureReason;
    const std::optional<FileDigest> measured = measureFile(canonical, &measureReason);
    if (!measured.has_value()) {
      if (key != nullptr) {
        *key = QStringLiteral("reference.inputs.unreadable");
      }
      if (reason != nullptr) {
        *reason = measureReason;
      }
      return false;
    }
    if (measured->bytes != want) {
      if (key != nullptr) {
        *key = QStringLiteral("reference.inputs.bytes");
      }
      if (reason != nullptr) {
        *reason = QStringLiteral("a raw input file is not the declared tensor bytes");
      }
      return false;
    }
  }
  return true;
}

bool exportReference(const ResolvedTools& tools, const QString& storedCanonical,
                     const ResolvedStage& stage, const BuilderStageReference& ref,
                     const QString& refDir, int index, const QString& modelId,
                     NativeBuildCancellation* cancellation,
                     const BuilderProgressCallback& progress, ExportedRef* out,
                     BuilderResult* bad) {
  QString key;
  QString reason;
  if (!validateCallerRef(ref, stage, &key, &reason)) {
    *bad = refuse(key, reason, index, QStringLiteral("reference"));
    return false;
  }
  // The exporter always writes a fresh directory and refuses one that already
  // exists, so the directory is proved absent here and created by the tool.
  if (QFileInfo::exists(refDir)) {
    *bad = refuse(QStringLiteral("reference.directory.exists"),
                  QStringLiteral("the reference directory already exists"), index,
                  QStringLiteral("reference"));
    return false;
  }
  QStringList args;
  args.append(QStringLiteral("--runtime"));
  args.append(tools.paths.ortDir);
  args.append(QStringLiteral("--reference-dir"));
  args.append(refDir);
  if (!ref.fixturePath.isEmpty()) {
    args.append(QStringLiteral("--fixture"));
    args.append(ref.fixturePath);
    args.append(QStringLiteral("--resolve"));
    args.append(QString(QStringLiteral("%1x%2"))
                    .arg(stage.inputGeometry.width())
                    .arg(stage.inputGeometry.height()));
  } else {
    for (const BuilderRawInput& input : ref.rawInputs) {
      args.append(QStringLiteral("--reference-input"));
      args.append(input.name);
      args.append(input.dtype);
      args.append(input.shape);
      args.append(input.filePath);
    }
  }
  args.append(QStringLiteral("--json"));
  args.append(storedCanonical);
  ToolResult run;
  const bool exited = runBlocking(tools.paths.inspect, args, tools.inspectEnv, cancellation,
                                  progress, index, modelId, QStringLiteral("reference"),
                                  &run);
  if (!exited) {
    if (isCancelled(cancellation) && run.launchError.isEmpty()) {
      *bad = cancelledResult(QStringLiteral("reference"), index);
      return false;
    }
    if (!run.stopped && isCancelled(cancellation)) {
      *bad = refuse(QStringLiteral("reference.cancel.stop"),
                    QStringLiteral("the helper did not stop after kill: ") +
                        run.launchError,
                    index, QStringLiteral("reference"), tailText(run.error));
      return false;
    }
    const QString detail =
        run.launchError.isEmpty() ? tailText(run.error) : run.launchError;
    *bad = refuse(QStringLiteral("tool.launch"), detail, index,
                  QStringLiteral("reference"), tailText(run.error));
    return false;
  }
  if (run.sawOutputLimit) {
    *bad = refuse(QStringLiteral("reference.output.truncated"),
                  QStringLiteral("the inspect output exceeded its bound"), index,
                  QStringLiteral("reference"), tailText(run.error));
    return false;
  }
  if (run.exitStatus != QProcess::NormalExit || run.exitCode != 0) {
    const QString detail =
        tailText(run.error).isEmpty()
            ? QStringLiteral("the CPU reference export failed: ") + toolExitCause(run)
            : tailText(run.error);
    *bad = refuse(QStringLiteral("reference.export.failed"), detail, index,
                  QStringLiteral("reference"), detail);
    return false;
  }
  QFile manifestFile(QDir(refDir).filePath(QString::fromLatin1(kReferenceFileName)));
  if (!manifestFile.open(QIODevice::ReadOnly)) {
    *bad = refuse(QStringLiteral("reference.manifest.missing"),
                  QStringLiteral("the reference manifest was not written"), index,
                  QStringLiteral("reference"));
    return false;
  }
  QJsonObject manifest;
  if (!parseObject(manifestFile.readAll(), &manifest)) {
    *bad = refuse(QStringLiteral("reference.manifest.json"),
                  QStringLiteral("the reference manifest is not JSON"), index,
                  QStringLiteral("reference"));
    return false;
  }
  QString measureReason;
  const std::optional<FileDigest> onnxMeasured =
      measureFile(storedCanonical, &measureReason);
  if (!onnxMeasured.has_value()) {
    *bad = refuse(QStringLiteral("model.owned.changed"), measureReason, index,
                  QStringLiteral("reference"));
    return false;
  }
  if (manifest.value(QStringLiteral("graphSha256")).toString().toLower() !=
      onnxMeasured->sha256.toLower()) {
    *bad = refuse(QStringLiteral("reference.graph.mismatch"),
                  QStringLiteral("the reference came from other bytes"), index,
                  QStringLiteral("reference"));
    return false;
  }
  QVector<RefTensor> inputs;
  QVector<RefTensor> outputs;
  QString failKey;
  QString failReason;
  if (!refTensors(manifest, refDir, QStringLiteral("inputs"), &inputs, &failKey,
                  &failReason) ||
      !refTensors(manifest, refDir, QStringLiteral("outputs"), &outputs, &failKey,
                  &failReason)) {
    *bad = refuse(failKey, failReason, index, QStringLiteral("reference"));
    return false;
  }
  if (inputs.size() != 1 || outputs.size() != 1) {
    *bad = refuse(QStringLiteral("reference.inputs.count"),
                  QStringLiteral("the worker binds one stage input and one output"), index,
                  QStringLiteral("reference"));
    return false;
  }
  if (inputs.first().dtype != QLatin1String("fp16") ||
      outputs.first().dtype != QLatin1String("fp16")) {
    *bad = refuse(QStringLiteral("reference.inputs.dtype"),
                  QStringLiteral("the stage binding is fp16 only"), index,
                  QStringLiteral("reference"));
    return false;
  }
  out->manifest = manifest;
  out->dir = refDir;
  out->inputs = inputs;
  out->outputs = outputs;
  return true;
}


}  // namespace builder_detail
}  // namespace native
}  // namespace mplayer
