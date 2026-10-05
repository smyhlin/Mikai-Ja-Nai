#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTextStream>

#include <cmath>
#include <optional>
#include <utility>

namespace {
struct ProcessResult {
  int exitCode = -1;
  QProcess::ExitStatus exitStatus = QProcess::CrashExit;
  QByteArray output;
  QByteArray errors;
  QString launchError;
};

ProcessResult run(const QString& executable, const QStringList& arguments) {
  QProcess process;
  process.start(executable, arguments);
  ProcessResult result;
  if (!process.waitForStarted(10000)) {
    result.launchError = process.errorString();
    return result;
  }
  if (!process.waitForFinished(180000)) {
    result.launchError = process.errorString();
    process.kill();
    process.waitForFinished(10000);
  }
  result.exitCode = process.exitCode();
  result.exitStatus = process.exitStatus();
  result.output = process.readAllStandardOutput();
  result.errors = process.readAllStandardError();
  return result;
}

std::optional<QJsonObject> object(const QByteArray& bytes) {
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) return std::nullopt;
  return document.object();
}

std::optional<QJsonObject> readObject(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
  return object(file.readAll());
}

QByteArray digest(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) return {};
  return hash.result().toHex();
}

QString shape(const QJsonArray& dimensions) {
  QStringList parts;
  for (const auto dimension : dimensions) parts.push_back(QString::number(dimension.toInteger()));
  return parts.join(QLatin1Char('x'));
}

QString tensorPath(const QString& root, const QJsonObject& tensor) {
  const QString relative = tensor.value(QStringLiteral("file")).toString();
  if (relative.isEmpty() || QFileInfo(relative).isAbsolute()) return {};
  const QString resolved = QFileInfo(QDir(root).filePath(relative)).canonicalFilePath();
  const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
  if (!resolved.startsWith(canonicalRoot + QLatin1Char('/'), Qt::CaseInsensitive)) return {};
  return resolved;
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size();
}

class Checks {
 public:
  void check(bool passed, const QString& behavior) {
    QTextStream(stdout) << (passed ? "PASS: " : "FAIL: ") << behavior << '\n';
    ++count_;
    if (!passed) ++failed_;
  }

  int finish() const {
    QTextStream(stdout) << "RESULT: " << (failed_ == 0 ? "PASS" : "FAIL") << " ("
                        << count_ - failed_ << '/' << count_ << ")\n";
    return failed_ == 0 ? 0 : 1;
  }

 private:
  int count_ = 0;
  int failed_ = 0;
};

void diagnostics(const ProcessResult& result) {
  QTextStream(stderr) << result.launchError << '\n' << result.errors << '\n' << result.output << '\n';
}

bool refused(const ProcessResult& result, const QString& phase, const QString& code) {
  const auto response = object(result.output);
  return result.exitStatus == QProcess::NormalExit && result.exitCode != 0 && response
      && !response->value(QStringLiteral("ok")).toBool(true)
      && response->value(QStringLiteral("phase")).toString() == phase
      && response->value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString() == code;
}
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  const QStringList arguments = app.arguments();
  if (arguments.size() != 8) {
    QTextStream(stderr) << "Usage: native_verification_probe <native_inspect> <mikai_native_verify>"
                           " <engine> <onnx> <64x64-frame.ppm> <GPU-uuid> <ORT-runtime-dir>\n";
    return 2;
  }
  const QString inspector = arguments.at(1);
  const QString verifier = arguments.at(2);
  const QString engine = arguments.at(3);
  const QString graph = arguments.at(4);
  const QString frame = arguments.at(5);
  const QString deviceUuid = arguments.at(6);
  const QString ortRuntime = arguments.at(7);
  const QByteArray engineHash = digest(engine);
  const QByteArray graphHash = digest(graph);
  const QByteArray frameHash = digest(frame);
  if (engineHash.isEmpty() || graphHash.isEmpty() || frameHash.isEmpty()) {
    QTextStream(stderr) << "Cannot read the real engine/graph/frame fixture.\n";
    return 2;
  }
  QTemporaryDir scratch(QDir::tempPath() + QStringLiteral("/mikai verification Ж-XXXXXX"));
  if (!scratch.isValid()) return 2;
  const QString referenceDir = scratch.filePath(QStringLiteral("actual CPU reference"));
  auto exportArguments = [&](const QString& directory, const QString& geometry) {
    return QStringList{graph, QStringLiteral("--runtime"), ortRuntime,
                       QStringLiteral("--reference-dir"), directory,
                       QStringLiteral("--fixture"), frame,
                       QStringLiteral("--resolve"), geometry, QStringLiteral("--json")};
  };
  Checks checks;
  const auto exported = run(inspector, exportArguments(referenceDir, QStringLiteral("64x64")));
  const auto reference = readObject(QDir(referenceDir).filePath(QStringLiteral("reference.json")));
  const bool exportPassed = exported.exitStatus == QProcess::NormalExit && exported.exitCode == 0 && reference;
  checks.check(exportPassed, QStringLiteral("A real decoded frame produces a complete CPU reference"));
  if (!exportPassed) {
    diagnostics(exported);
    return checks.finish();
  }

  const auto inputs = reference->value(QStringLiteral("inputs")).toArray();
  const auto outputs = reference->value(QStringLiteral("outputs")).toArray();
  if (inputs.isEmpty() || outputs.isEmpty()) {
    QTextStream(stderr) << "The graph reference has no actual input/output tensors.\n";
    return 2;
  }
  QStringList verifyArguments{QStringLiteral("--engine"), engine,
                              QStringLiteral("--engine-sha256"), QString::fromLatin1(engineHash),
                              QStringLiteral("--device-uuid"), deviceUuid};
  for (const auto& group : {std::pair{QStringLiteral("--input"), inputs},
                           std::pair{QStringLiteral("--output"), outputs}}) {
    for (const auto value : group.second) {
      const auto tensor = value.toObject();
      const QString path = tensorPath(referenceDir, tensor);
      if (path.isEmpty()) return 2;
      verifyArguments << group.first << tensor.value(QStringLiteral("name")).toString()
                      << tensor.value(QStringLiteral("dtype")).toString()
                      << shape(tensor.value(QStringLiteral("shape")).toArray()) << path;
    }
  }
  const auto verified = run(verifier, verifyArguments);
  const auto verification = object(verified.output);
  const bool gpuPassed = verified.exitStatus == QProcess::NormalExit && verified.exitCode == 0
      && verification && verification->value(QStringLiteral("ok")).toBool();
  checks.check(gpuPassed, QStringLiteral("The selected GPU engine matches the real CPU frame reference"));
  if (!gpuPassed) diagnostics(verified);
  if (verification) QTextStream(stdout) << QJsonDocument(*verification).toJson(QJsonDocument::Compact) << '\n';

  QStringList wrongInputArguments = verifyArguments;
  const qsizetype firstInput = wrongInputArguments.indexOf(QStringLiteral("--input"));
  wrongInputArguments[firstInput + 1] = QStringLiteral("not-the-model-input");
  const auto wrongInput = run(verifier, wrongInputArguments);
  const auto wrongInputReport = object(wrongInput.output);
  bool actualInputNamed = false;
  if (wrongInputReport) {
    const QString actualName = inputs.first().toObject().value(QStringLiteral("name")).toString();
    for (const auto binding : wrongInputReport->value(QStringLiteral("bindings")).toArray()) {
      const auto actual = binding.toObject();
      actualInputNamed |= actual.value(QStringLiteral("name")).toString() == actualName
          && actual.value(QStringLiteral("mode")).toString() == QStringLiteral("input");
    }
  }
  const bool bindingPassed = refused(wrongInput, QStringLiteral("bindings"), QStringLiteral("binding-mismatch"))
      && actualInputNamed;
  checks.check(bindingPassed, QStringLiteral("A wrong input binding is rejected while the output remains correct"));
  if (!bindingPassed) diagnostics(wrongInput);

  const QString corruptPath = scratch.filePath(QStringLiteral("corrupt.engine"));
  if (!QFile::copy(engine, corruptPath)) return 2;
  QFile corrupt(corruptPath);
  if (!corrupt.open(QIODevice::ReadWrite) || corrupt.size() == 0 || !corrupt.seek(corrupt.size() - 1)) return 2;
  QByteArray last = corrupt.read(1);
  if (last.size() != 1 || !corrupt.seek(corrupt.size() - 1)) return 2;
  last[0] = static_cast<char>(static_cast<unsigned char>(last.at(0)) ^ 0x5a);
  if (corrupt.write(last) != 1) return 2;
  corrupt.close();
  QStringList corruptArguments = verifyArguments;
  corruptArguments[corruptArguments.indexOf(QStringLiteral("--engine")) + 1] = corruptPath;
  const auto corrupted = run(verifier, corruptArguments);
  const auto corruptedReport = object(corrupted.output);
  const bool integrityPassed = refused(corrupted, QStringLiteral("integrity"), QStringLiteral("digest-mismatch"))
      && corruptedReport && !corruptedReport->value(QStringLiteral("bindingsKnown")).toBool(true);
  checks.check(integrityPassed, QStringLiteral("Corrupted engine bytes fail integrity before deserialization"));
  if (!integrityPassed) diagnostics(corrupted);

  const auto firstOutput = outputs.first().toObject();
  const qint64 outputBytes = firstOutput.value(QStringLiteral("bytes")).toInteger();
  if (outputBytes <= 0 || outputBytes > 64 * 1024 * 1024) return 2;
  const QString blackReference = scratch.filePath(QStringLiteral("wrong finite CPU reference.bin"));
  if (!writeBytes(blackReference, QByteArray(outputBytes, '\0'))) return 2;
  QStringList blackArguments = verifyArguments;
  blackArguments[blackArguments.indexOf(QStringLiteral("--output")) + 4] = blackReference;
  const auto black = run(verifier, blackArguments);
  const auto blackReport = object(black.output);
  const auto measuredOutputs = blackReport
      ? blackReport->value(QStringLiteral("outputs")).toArray() : QJsonArray{};
  const auto measurement = measuredOutputs.isEmpty() ? QJsonObject{} : measuredOutputs.first().toObject();
  const auto absoluteError = measurement.value(QStringLiteral("maxAbsoluteError"));
  const auto rmse = measurement.value(QStringLiteral("rmse"));
  const auto absoluteLimit = measurement.value(QStringLiteral("maxAbsoluteErrorTolerance"));
  const auto rmseLimit = measurement.value(QStringLiteral("rmseTolerance"));
  const bool metricsPresent = absoluteError.isDouble() && rmse.isDouble()
      && absoluteLimit.isDouble() && rmseLimit.isDouble()
      && std::isfinite(absoluteError.toDouble()) && std::isfinite(rmse.toDouble())
      && absoluteLimit.toDouble() > 0 && rmseLimit.toDouble() > 0;
  const bool numericalPassed = refused(black, QStringLiteral("numerics"), QStringLiteral("out-of-tolerance"))
      && measurement.value(QStringLiteral("name")) == firstOutput.value(QStringLiteral("name"))
      && measurement.value(QStringLiteral("shape")) == firstOutput.value(QStringLiteral("shape"))
      && measurement.value(QStringLiteral("measured")).toBool()
      && measurement.value(QStringLiteral("finite")).toBool()
      && !measurement.value(QStringLiteral("withinTolerance")).toBool(true)
      && metricsPresent
      && (absoluteError.toDouble() > absoluteLimit.toDouble() || rmse.toDouble() > rmseLimit.toDouble());
  checks.check(numericalPassed, QStringLiteral("Correct bindings cannot disguise numerically wrong output"));
  if (!numericalPassed) diagnostics(black);

  const QString occupied = scratch.filePath(QStringLiteral("occupied reference"));
  if (!QDir().mkdir(occupied)) return 2;
  const QString sentinel = QDir(occupied).filePath(QStringLiteral("keep.bin"));
  const QByteArray sentinelBytes("previous reference must survive");
  if (!writeBytes(sentinel, sentinelBytes)) return 2;
  const QByteArray sentinelHash = digest(sentinel);
  const auto occupiedExport = run(inspector, exportArguments(occupied, QStringLiteral("64x64")));
  checks.check(occupiedExport.exitStatus == QProcess::NormalExit && occupiedExport.exitCode != 0
                   && digest(sentinel) == sentinelHash,
               QStringLiteral("Export refuses an existing reference directory without altering it"));

  const QString wrongGeometry = scratch.filePath(QStringLiteral("wrong geometry"));
  const auto geometryExport = run(inspector, exportArguments(wrongGeometry, QStringLiteral("32x32")));
  checks.check(geometryExport.exitStatus == QProcess::NormalExit && geometryExport.exitCode != 0
                   && !QFileInfo::exists(wrongGeometry),
               QStringLiteral("Reference geometry is never silently stretched into a different engine request"));
  checks.check(digest(engine) == engineHash && digest(graph) == graphHash && digest(frame) == frameHash,
               QStringLiteral("Verification leaves the source engine, graph and decoded frame unchanged"));
  return checks.finish();
}
