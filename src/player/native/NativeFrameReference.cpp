#include "NativeFrameReference.hpp"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QtEndian>

#include "NativeCatalog.hpp"
#include "NativeModelInventory.hpp"

namespace mplayer {
namespace native {

namespace {

constexpr qint64 kMaxFixtureSide = 32768;
constexpr qint64 kMaxFixtureBytes = 512ll * 1024 * 1024;

constexpr auto kManifestFileName = "reference.json";

constexpr int kReferenceSchemaVersion = 1;

void refuseWith(QString* key, QString* reason, const QString& k, const QString& r) {
  if (key != nullptr) *key = k;
  if (reason != nullptr) *reason = r;
}

bool fp16AllFinite(const QByteArray& data, qsizetype* firstBad) {
  const auto* bytes = reinterpret_cast<const uchar*>(data.constData());
  const qsizetype count = data.size() / 2;
  for (qsizetype i = 0; i < count; ++i) {
    const quint16 bits = qFromLittleEndian<quint16>(bytes + i * 2);
    if ((bits & 0x7C00u) == 0x7C00u) {
      if (firstBad != nullptr) *firstBad = i;
      return false;
    }
  }
  return true;
}

bool fp32AllFinite(const QByteArray& data, qsizetype* firstBad) {
  const auto* bytes = reinterpret_cast<const uchar*>(data.constData());
  const qsizetype count = data.size() / 4;
  for (qsizetype i = 0; i < count; ++i) {
    const quint32 bits = qFromLittleEndian<quint32>(bytes + i * 4);
    if ((bits & 0x7F800000u) == 0x7F800000u) {
      if (firstBad != nullptr) *firstBad = i;
      return false;
    }
  }
  return true;
}

bool tensorAllFinite(const QString& dtype, const QByteArray& data, qsizetype* firstBad) {
  return dtype == QLatin1String("fp16") ? fp16AllFinite(data, firstBad)
                                        : fp32AllFinite(data, firstBad);
}

qint64 elementSizeOf(const QString& dtype) {
  if (dtype == QLatin1String("fp16")) return 2;
  if (dtype == QLatin1String("fp32")) return 4;
  return 0;
}

QString elementCountText(const QVector<qint64>& shape) {
  qint64 elements = 1;
  for (const qint64 dim : shape) elements *= dim;
  return QString::number(elements);
}

QJsonArray dimsArray(const QVector<qint64>& shape) {
  QJsonArray dims;
  for (const qint64 dim : shape) dims.append(static_cast<double>(dim));
  return dims;
}

QJsonObject declaredTensorJson(const NamedTensorProfile& tensor) {
  QJsonObject object;
  object.insert(QStringLiteral("name"), tensor.name);
  object.insert(QStringLiteral("dtype"), tensor.profile.dtype);
  object.insert(QStringLiteral("rank"), tensor.profile.rank);
  object.insert(QStringLiteral("declaredShape"), dimsArray(tensor.dims));
  if (tensor.profile.rank == 4) {
    object.insert(QStringLiteral("n"), static_cast<double>(tensor.profile.batch));
    object.insert(QStringLiteral("c"), static_cast<double>(tensor.profile.channels));
    if (tensor.profile.height.has_value()) {
      object.insert(QStringLiteral("h"), static_cast<double>(*tensor.profile.height));
    }
    if (tensor.profile.width.has_value()) {
      object.insert(QStringLiteral("w"), static_cast<double>(*tensor.profile.width));
    }
  }
  return object;
}

QJsonObject exportedTensorJson(const ReferenceTensor& tensor, const QString& file) {
  QJsonObject object;
  object.insert(QStringLiteral("name"), tensor.name);
  object.insert(QStringLiteral("dtype"), tensor.dtype);
  object.insert(QStringLiteral("rank"), static_cast<double>(tensor.shape.size()));
  object.insert(QStringLiteral("shape"), dimsArray(tensor.shape));
  object.insert(QStringLiteral("elements"), static_cast<double>(tensor.elementCount));
  object.insert(QStringLiteral("bytes"), static_cast<double>(tensor.data.size()));
  object.insert(QStringLiteral("sha256"), sha256HexOf(tensor.data));
  object.insert(QStringLiteral("file"), file);
  return object;
}

bool skipPpmWhitespace(const QByteArray& data, qsizetype* pos, QString* reason) {
  while (*pos < data.size()) {
    const char c = data.at(*pos);
    if (c == '#') {
      while (*pos < data.size() && data.at(*pos) != '\n') ++(*pos);
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++(*pos);
      continue;
    }
    return true;
  }
  *reason = QStringLiteral("the PPM header ends before the pixel payload begins");
  return false;
}

bool readPpmUint(const QByteArray& data, qsizetype* pos, quint32* value, QString* reason) {
  if (!skipPpmWhitespace(data, pos, reason)) return false;
  const char first = data.at(*pos);
  if (first < '0' || first > '9') {
    *reason = QStringLiteral("expected a decimal number in the PPM header at byte %1").arg(*pos);
    return false;
  }
  quint64 result = 0;
  while (*pos < data.size() && data.at(*pos) >= '0' && data.at(*pos) <= '9') {
    result = result * 10 + static_cast<quint64>(data.at(*pos) - '0');
    if (result > 0xFFFFFFFFull) {
      *reason = QStringLiteral("a PPM header number at byte %1 exceeds 32 bits").arg(*pos);
      return false;
    }
    ++(*pos);
  }
  *value = static_cast<quint32>(result);
  return true;
}

}  // namespace

bool readPpmFixture(const QString& path, FrameFixture* fixture, QString* reasonKey,
                    QString* reason) {
  const QFileInfo info(QDir::cleanPath(path));
  QFile file(info.absoluteFilePath());
  if (!info.isAbsolute() || !info.isFile() || !file.open(QIODevice::ReadOnly)) {
    refuseWith(reasonKey, reason, QStringLiteral("fixture.unreadable"),
               QStringLiteral("'%1' is not a readable absolute file").arg(path));
    return false;
  }
  const QByteArray data = file.readAll();
  file.close();

  if (data.size() < 2 || data.at(0) != 'P' || data.at(1) != '6') {
    refuseWith(reasonKey, reason, QStringLiteral("fixture.encoding"),
               QStringLiteral("'%1' is not a binary P6 PPM; this reader accepts P6 only")
                   .arg(path));
    return false;
  }

  qsizetype pos = 2;
  QString error;
  quint32 width = 0;
  quint32 height = 0;
  quint32 maxval = 0;
  if (!readPpmUint(data, &pos, &width, &error) || !readPpmUint(data, &pos, &height, &error) ||
      !readPpmUint(data, &pos, &maxval, &error)) {
    refuseWith(reasonKey, reason, QStringLiteral("fixture.header"), error);
    return false;
  }
  if (width == 0 || height == 0 || width > static_cast<quint32>(kMaxFixtureSide) ||
      height > static_cast<quint32>(kMaxFixtureSide)) {
    refuseWith(reasonKey, reason, QStringLiteral("fixture.geometry"),
               QStringLiteral("the PPM declares %1x%2, which is not positive and bounded")
                   .arg(width)
                   .arg(height));
    return false;
  }
  if (maxval != 255) {
    refuseWith(reasonKey, reason, QStringLiteral("fixture.maxval"),
               QStringLiteral("the PPM declares maxval %1; only 255 (8-bit samples) is supported")
                   .arg(maxval));
    return false;
  }
  if (pos >= data.size() ||
      (data.at(pos) != ' ' && data.at(pos) != '\t' && data.at(pos) != '\r' &&
       data.at(pos) != '\n')) {
    refuseWith(reasonKey, reason, QStringLiteral("fixture.header"),
               QStringLiteral("the PPM header is not followed by a single whitespace byte at %1")
                   .arg(pos));
    return false;
  }
  ++pos;

  const qint64 payloadBytes = static_cast<qint64>(width) * height * 3;
  if (payloadBytes <= 0 || payloadBytes > kMaxFixtureBytes) {
    refuseWith(reasonKey, reason, QStringLiteral("fixture.geometry"),
               QStringLiteral("the PPM payload would be %1 bytes, beyond this reader's bound")
                   .arg(payloadBytes));
    return false;
  }
  const qint64 available = data.size() - pos;
  if (available != payloadBytes) {
    refuseWith(reasonKey, reason, QStringLiteral("fixture.payload"),
               QStringLiteral("the PPM holds %1 payload bytes but %2x%3x3 needs %4")
                   .arg(available)
                   .arg(width)
                   .arg(height)
                   .arg(payloadBytes));
    return false;
  }

  fixture->size = QSize(static_cast<int>(width), static_cast<int>(height));
  fixture->rgb = data.mid(pos, static_cast<qsizetype>(payloadBytes));
  fixture->path = info.absoluteFilePath();
  fixture->fileBytes = data.size();
  fixture->fileSha256 = sha256HexOf(data);
  if (reasonKey != nullptr) reasonKey->clear();
  if (reason != nullptr) reason->clear();
  return true;
}

bool exportCpuFrameReference(const InspectionRuntime& runtime, const QByteArray& graphBytes,
                             const QString& graphPath, const ReferenceExportRequest& request,
                             ReferenceExport* result, QString* reasonKey, QString* reason) {
  const auto refuse = [reasonKey, reason](const QString& key, const QString& text) {
    if (reasonKey != nullptr) *reasonKey = key;
    if (reason != nullptr) *reason = text;
    return false;
  };

  const QFileInfo dirInfo(QDir::cleanPath(request.directory));
  if (dirInfo.exists()) {
    return refuse(QStringLiteral("reference.directory.exists"),
                  QStringLiteral("'%1' already exists; an export always writes a fresh directory")
                      .arg(request.directory));
  }
  if (request.fixturePath.isEmpty() == request.rawInputs.isEmpty()) {
    return refuse(QStringLiteral("reference.inputs.mode"),
                  QStringLiteral("exactly one input source is required: a --fixture frame or one "
                                 "or more --reference-input tensors"));
  }

  DeclaredBoundaries declared;
  QString declaredKey;
  QString declaredReason;
  if (!inspectDeclaredBoundaries(runtime, graphBytes, &declared, &declaredKey, &declaredReason)) {
    return refuse(declaredKey, declaredReason);
  }

  QVector<ReferenceInput> bound;
  QString kind;
  FrameFixture fixture;

  if (!request.rawInputs.isEmpty()) {
    kind = QStringLiteral("raw");
    for (const RawReferenceInput& raw : request.rawInputs) {
      if (raw.name.isEmpty()) {
        return refuse(QStringLiteral("reference.inputs.name"),
                      QStringLiteral("a --reference-input was given without a tensor name"));
      }
      if (elementSizeOf(raw.dtype) == 0) {
        return refuse(QStringLiteral("reference.inputs.dtype"),
                      QStringLiteral("--reference-input '%1' is named %2; only fp16 and fp32 are "
                                     "read from a file")
                          .arg(raw.name, raw.dtype));
      }
      if (raw.shape.isEmpty()) {
        return refuse(QStringLiteral("reference.inputs.shape"),
                      QStringLiteral("--reference-input '%1' has no shape").arg(raw.name));
      }
      qint64 elements = 1;
      for (const qint64 dim : raw.shape) {
        if (dim <= 0) {
          return refuse(QStringLiteral("reference.inputs.shape"),
                        QStringLiteral("--reference-input '%1' has the non-positive dimension %2; "
                                       "a symbolic dimension cannot be fed")
                            .arg(raw.name)
                            .arg(dim));
        }
        if (elements > kMaxFixtureBytes / dim) {
          return refuse(QStringLiteral("reference.inputs.shape"),
                        QStringLiteral("--reference-input '%1' is beyond the exporter's allocation "
                                       "bound")
                            .arg(raw.name));
        }
        elements *= dim;
      }

      const QFileInfo fileInfo(QDir::cleanPath(raw.filePath));
      QFile file(fileInfo.absoluteFilePath());
      if (!fileInfo.isAbsolute() || !fileInfo.isFile() || !file.open(QIODevice::ReadOnly)) {
        return refuse(QStringLiteral("reference.inputs.unreadable"),
                      QStringLiteral("--reference-input '%1' names the file '%2', which is not a "
                                     "readable absolute file")
                          .arg(raw.name, raw.filePath));
      }
      const QByteArray data = file.readAll();
      file.close();

      const qint64 needed = elements * elementSizeOf(raw.dtype);
      if (data.size() != needed) {
        return refuse(QStringLiteral("reference.inputs.bytes"),
                      QStringLiteral("--reference-input '%1' reads %2 bytes from '%3', but %4 as %5 "
                                     "needs exactly %6")
                          .arg(raw.name)
                          .arg(data.size())
                          .arg(fileInfo.absoluteFilePath(), elementCountText(raw.shape))
                          .arg(raw.dtype)
                          .arg(needed));
      }

      ReferenceInput input;
      input.name = raw.name;
      input.dtype = raw.dtype;
      input.shape = raw.shape;
      input.data = data;
      bound.append(input);
    }
  } else {
    kind = QStringLiteral("ppm");
    if (declared.inputs.size() != 1) {
      return refuse(QStringLiteral("reference.graph.boundary"),
                    QStringLiteral("a --fixture frame supplies one spatial input, but this graph "
                                   "declares %1 input(s); supply the real tensors with "
                                   "--reference-input instead")
                        .arg(declared.inputs.size()));
    }
    const TensorProfile& declaredInput = declared.inputs.first().profile;
    if (declaredInput.rank != 4 || declaredInput.batch != 1 || declaredInput.channels != 3) {
      return refuse(QStringLiteral("reference.graph.boundary"),
                    QStringLiteral("a --fixture frame supplies 1x3xHxW fp16, but this graph "
                                   "declares input %1x%2 at rank %3; supply the real tensors with "
                                   "--reference-input instead")
                        .arg(declaredInput.batch)
                        .arg(declaredInput.channels)
                        .arg(declaredInput.rank));
    }
    if (!isEligibleIoType(declaredInput.dtype)) {
      return refuse(QStringLiteral("reference.graph.dtype"),
                    QStringLiteral("a --fixture frame supplies %1 bytes, but this graph declares a "
                                   "%2 input; supply real tensors with --reference-input instead")
                        .arg(QLatin1String(kEligibleIoType), declaredInput.dtype));
    }

    if (!readPpmFixture(request.fixturePath, &fixture, &declaredKey, &declaredReason)) {
      return refuse(declaredKey, declaredReason);
    }
    if (request.resolveGeometry.isValid() && fixture.size != request.resolveGeometry) {
      return refuse(QStringLiteral("reference.geometry.mismatch"),
                    QStringLiteral("the fixture is %1x%2 but --resolve names %3x%4; a frame is "
                                   "never stretched to fit a requested geometry")
                        .arg(fixture.size.width())
                        .arg(fixture.size.height())
                        .arg(request.resolveGeometry.width())
                        .arg(request.resolveGeometry.height()));
    }
    if (declaredInput.hasGeometry()) {
      const QSize fixed(static_cast<int>(*declaredInput.width),
                        static_cast<int>(*declaredInput.height));
      if (fixed != fixture.size) {
        return refuse(QStringLiteral("reference.geometry.declared"),
                      QStringLiteral("the graph declares a fixed %1x%2 input but the fixture is "
                                     "%3x%4")
                          .arg(fixed.width())
                          .arg(fixed.height())
                          .arg(fixture.size.width())
                          .arg(fixture.size.height()));
      }
    }

    ReferenceInput input;
    input.name = declared.inputs.first().name;
    input.dtype = QLatin1String(kEligibleIoType);
    input.shape = {1, 3, fixture.size.height(), fixture.size.width()};
    input.data = frameToFp16Tensor(fixture);
    bound.append(input);
  }

  const ReferenceRun run = runReferenceTensor(runtime, graphBytes, bound);
  if (!run.ok) return refuse(run.reasonKey, run.reason);

  for (const ReferenceTensor& tensor : run.outputs) {
    qsizetype firstBad = -1;
    if (!tensorAllFinite(tensor.dtype, tensor.data, &firstBad)) {
      return refuse(QStringLiteral("reference.output.nonFinite"),
                    QStringLiteral("the CPU run returned a non-finite %1 at element %2 of "
                                   "output '%3'; a reference with a NaN or an infinity in it "
                                   "cannot be compared")
                        .arg(tensor.dtype)
                        .arg(firstBad)
                        .arg(tensor.name));
    }
  }

  QStringList inputFiles;
  for (qsizetype i = 0; i < bound.size(); ++i) {
    inputFiles.append(QStringLiteral("input-%1-%2.bin").arg(i).arg(bound.at(i).dtype));
  }
  QStringList outputFiles;
  for (qsizetype i = 0; i < run.outputs.size(); ++i) {
    outputFiles.append(QStringLiteral("output-%1-%2.bin").arg(i).arg(run.outputs.at(i).dtype));
  }

  QJsonArray declaredInputs;
  for (const NamedTensorProfile& tensor : declared.inputs) {
    declaredInputs.append(declaredTensorJson(tensor));
  }
  QJsonArray declaredOutputs;
  for (const NamedTensorProfile& tensor : declared.outputs) {
    declaredOutputs.append(declaredTensorJson(tensor));
  }
  QJsonArray exportedInputs;
  for (qsizetype i = 0; i < bound.size(); ++i) {
    ReferenceTensor tensor;
    tensor.name = bound.at(i).name;
    tensor.dtype = bound.at(i).dtype;
    tensor.shape = bound.at(i).shape;
    tensor.data = bound.at(i).data;
    tensor.elementCount = tensor.data.size() / elementSizeOf(bound.at(i).dtype);
    exportedInputs.append(exportedTensorJson(tensor, inputFiles.at(i)));
  }
  QJsonArray exportedOutputs;
  for (qsizetype i = 0; i < run.outputs.size(); ++i) {
    exportedOutputs.append(exportedTensorJson(run.outputs.at(i), outputFiles.at(i)));
  }

  QJsonObject manifest;
  manifest.insert(QStringLiteral("schema"), QStringLiteral("mikai.frameReference"));
  manifest.insert(QStringLiteral("version"), kReferenceSchemaVersion);
  manifest.insert(QStringLiteral("kind"), kind);
  manifest.insert(QStringLiteral("graphPath"), graphPath);
  manifest.insert(QStringLiteral("graphBytes"), static_cast<double>(graphBytes.size()));
  manifest.insert(QStringLiteral("graphSha256"), sha256HexOf(graphBytes));
  if (kind == QLatin1String("ppm")) {
    manifest.insert(QStringLiteral("framePath"), fixture.path);
    manifest.insert(QStringLiteral("frameSha256"), fixture.fileSha256);
    manifest.insert(QStringLiteral("frameBytes"), static_cast<double>(fixture.fileBytes));
    manifest.insert(QStringLiteral("frameEncoding"), QStringLiteral("P6"));
    manifest.insert(QStringLiteral("frameWidth"), static_cast<double>(fixture.size.width()));
    manifest.insert(QStringLiteral("frameHeight"), static_cast<double>(fixture.size.height()));
  }
  manifest.insert(QStringLiteral("declaredInputs"), declaredInputs);
  manifest.insert(QStringLiteral("declaredOutputs"), declaredOutputs);
  manifest.insert(QStringLiteral("inputs"), exportedInputs);
  manifest.insert(QStringLiteral("outputs"), exportedOutputs);
  manifest.insert(QStringLiteral("runtime"), QJsonObject{
      {QStringLiteral("module"), runtime.modulePath},
      {QStringLiteral("version"), inspectionRuntimeLoadedVersion()}});

  const QString dirPath = dirInfo.absoluteFilePath();
  if (!QDir().mkpath(dirPath)) {
    return refuse(QStringLiteral("reference.directory.create"),
                  QStringLiteral("'%1' could not be created").arg(dirPath));
  }

  const auto writeFile = [dirPath](const QString& fileName, const QByteArray& bytes,
                                   QString* fileReason) {
    QSaveFile file(QDir(dirPath).filePath(fileName));
    if (!file.open(QIODevice::WriteOnly)) {
      *fileReason = QStringLiteral("'%1' could not be opened for writing: %2")
                        .arg(fileName, file.errorString());
      return false;
    }
    if (file.write(bytes) != bytes.size()) {
      *fileReason = QStringLiteral("'%1' could not be written completely: %2")
                        .arg(fileName, file.errorString());
      return false;
    }
    if (!file.commit()) {
      *fileReason =
          QStringLiteral("'%1' could not be committed: %2").arg(fileName, file.errorString());
      return false;
    }
    return true;
  };

  const auto cleanup = [dirPath, inputFiles, outputFiles]() {
    QDir dir(dirPath);
    for (const QString& fileName : inputFiles) dir.remove(fileName);
    for (const QString& fileName : outputFiles) dir.remove(fileName);
    dir.remove(QLatin1String(kManifestFileName));
    QDir().rmdir(dirPath);
  };

  QString fileReason;
  for (qsizetype i = 0; i < bound.size(); ++i) {
    if (!writeFile(inputFiles.at(i), bound.at(i).data, &fileReason)) {
      cleanup();
      return refuse(QStringLiteral("reference.write"), fileReason);
    }
  }
  for (qsizetype i = 0; i < run.outputs.size(); ++i) {
    if (!writeFile(outputFiles.at(i), run.outputs.at(i).data, &fileReason)) {
      cleanup();
      return refuse(QStringLiteral("reference.write"), fileReason);
    }
  }
  if (!writeFile(QLatin1String(kManifestFileName),
                 QJsonDocument(manifest).toJson(QJsonDocument::Indented), &fileReason)) {
    cleanup();
    return refuse(QStringLiteral("reference.write"), fileReason);
  }

  result->directory = dirPath;
  result->manifest = manifest;
  result->inputPaths.clear();
  for (const QString& fileName : inputFiles) {
    result->inputPaths.append(QDir(dirPath).filePath(fileName));
  }
  result->outputPaths.clear();
  for (const QString& fileName : outputFiles) {
    result->outputPaths.append(QDir(dirPath).filePath(fileName));
  }
  result->manifestPath = QDir(dirPath).filePath(QLatin1String(kManifestFileName));
  if (reasonKey != nullptr) reasonKey->clear();
  if (reason != nullptr) reason->clear();
  return true;
}

}  // namespace native
}  // namespace mplayer
