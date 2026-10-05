// The two actual import entry points: the only part of the model store that runs
// the inspector. `importFile` reads a spatial graph through the ONNX Runtime C
// API and publishes it; `importTemporalFile` additionally runs the caller's raw
// reference tensor to prove a temporal boundary. Everything the store itself
// promises - ownership, strict manifest reading, publication, integrity and the
// digest helpers - lives in NativeModelInventory.cpp and builds without ONNX
// Runtime, so a build without the inspector can still read, verify and publish
// owned rows.
//
// This translation unit is compiled into `mikai_model_inspection` because it is
// the only one that names `inspectGraph`, `inspectDeclaredBoundaries` and
// `runReferenceTensor`.

#include "native/models/NativeModelInventory.hpp"

#include <limits>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include "native/models/NativeModelInspect.hpp"
#include "native/bundle/NativePaths.hpp"

namespace mplayer {
namespace native {

ModelImportOutcome ModelInventory::importFile(const QString& sourcePath,
                                              const InspectionRuntime& runtime,
                                              const QSize& stageGeometry,
                                              const BackendAvailability& backends,
                                              const QString& termsNote,
                                              ImportCancellation* cancellation) {
  ModelImportOutcome outcome;
  const auto refuse = [&outcome](const QString& key, const QString& text) {
    outcome.ok = false;
    outcome.published = false;
    outcome.reasonKey = key;
    outcome.reason = text;
    return outcome;
  };

  // The path is judged before anything is opened: an import of a stored or
  // hand-picked path must not be able to name a truncation, a device namespace or
  // a redirector share, and the reason it is refused has to name the boundary
  // rather than the bytes.
  const NativePathTrust trust = nativePathTrust(sourcePath, true);
  if (!trust.trusted) {
    const bool representable =
        trust.representable && trust.form == NativePathForm::Local;
    return refuse(representable ? QStringLiteral("model.source.path")
                                : QStringLiteral("model.source.pathTrust"),
                  trust.reason);
  }

  const QFileInfo sourceInfo(QDir::cleanPath(sourcePath));
  if (!sourceInfo.isAbsolute()) {
    return refuse(QStringLiteral("model.source.absolute"),
                  QStringLiteral("the model path '%1' is not an absolute path").arg(sourcePath));
  }
  if (!sourceInfo.isFile()) {
    return refuse(QStringLiteral("model.source.missing"),
                  QStringLiteral("%1 does not exist or is not a regular file").arg(sourcePath));
  }
  const QString canonicalSource = sourceInfo.canonicalFilePath();
  if (canonicalSource.isEmpty()) {
    return refuse(QStringLiteral("model.source.missing"),
                  QStringLiteral("%1 could not be resolved").arg(sourcePath));
  }
  // The resolved path is a second, independent obligation: the canonical form is
  // what is actually opened, so it is the one that has to be representable too.
  const NativePathTrust canonicalTrust = nativePathTrust(canonicalSource, true);
  if (!canonicalTrust.trusted) {
    return refuse(QStringLiteral("model.source.pathTrust"), canonicalTrust.reason);
  }

  QFile file(canonicalSource);
  if (!file.open(QIODevice::ReadOnly)) {
    return refuse(QStringLiteral("model.source.unreadable"),
                  QStringLiteral("%1 cannot be read: %2")
                      .arg(canonicalSource, file.errorString()));
  }
  const QByteArray bytes = file.readAll();
  if (file.error() != QFile::NoError) {
    return refuse(QStringLiteral("model.source.unreadable"),
                  QStringLiteral("%1 could not be read: %2")
                      .arg(canonicalSource, file.errorString()));
  }
  file.close();

  if (cancellation != nullptr && cancellation->isCancelled()) {
    // A cancel that arrived before the graph was inspected: the CPU reference is
    // never run and the store is never touched.
    return refuse(QStringLiteral("native.import.cancelled"),
                  QStringLiteral("the import was cancelled before inspection began"));
  }

  const InspectionResult inspection = inspectGraph(runtime, bytes, stageGeometry);
  if (!inspection.ok) {
    // A refusal publishes nothing: a truncated graph, two inputs, a rank-5
    // boundary or a non-uniform declared scale leaves the store untouched.
    return refuse(inspection.reasonKey, inspection.reason);
  }

  // The publication gate: the one atomic step that commits the import. If a
  // cancel won the race while the graph was being inspected, nothing is written
  // and the outcome says so; if this call passes, the copy is committed and a
  // later cancel is refused rather than re-labelling a committed row.
  if (cancellation != nullptr && !cancellation->beginPublication()) {
    return refuse(QStringLiteral("native.import.cancelled"),
                  QStringLiteral("the import was cancelled before anything was published"));
  }

  const QString displayName = sourceInfo.completeBaseName();
  return publish(bytes, inspection, canonicalSource, displayName, backends, termsNote);
}

ModelImportOutcome ModelInventory::importTemporalFile(const QString& sourcePath,
                                                      const InspectionRuntime& runtime,
                                                      const QVector<RawReferenceInput>& rawInputs,
                                                      const BackendAvailability& backends,
                                                      const QString& termsNote,
                                                      ImportCancellation* cancellation) {
  ModelImportOutcome outcome;
  const auto refuse = [&outcome](const QString& key, const QString& text) {
    outcome.ok = false;
    outcome.published = false;
    outcome.reasonKey = key;
    outcome.reason = text;
    return outcome;
  };
  // Same boundary as `importFile`, and for the same reason: this path is opened
  // before anything about the graph is known, so what it spells has to be
  // proved first. The refusal text does not quote the path, so a crafted name
  // never reaches a log through it.
  const NativePathTrust trust = nativePathTrust(sourcePath, true);
  if (!trust.trusted) {
    const bool representable =
        trust.representable && trust.form == NativePathForm::Local;
    return refuse(representable ? QStringLiteral("model.source.path")
                                : QStringLiteral("model.source.pathTrust"),
                  trust.reason);
  }
  const QFileInfo sourceInfo(QDir::cleanPath(sourcePath));
  if (!sourceInfo.isAbsolute()) {
    return refuse(QStringLiteral("model.source.absolute"),
                  QStringLiteral("the model path is not an absolute path"));
  }
  if (!sourceInfo.isFile()) {
    return refuse(QStringLiteral("model.source.missing"),
                  QStringLiteral("the model path does not exist or is not a regular file"));
  }
  const QString canonicalSource = sourceInfo.canonicalFilePath();
  if (canonicalSource.isEmpty()) {
    return refuse(QStringLiteral("model.source.missing"),
                  QStringLiteral("the model path could not be resolved"));
  }
  const NativePathTrust canonicalTrust = nativePathTrust(canonicalSource, true);
  if (!canonicalTrust.trusted) {
    return refuse(QStringLiteral("model.source.pathTrust"), canonicalTrust.reason);
  }
  QFile file(canonicalSource);
  if (!file.open(QIODevice::ReadOnly)) {
    return refuse(QStringLiteral("model.source.unreadable"),
                  QStringLiteral("the model file cannot be read"));
  }
  const QByteArray bytes = file.readAll();
  if (file.error() != QFile::NoError) {
    return refuse(QStringLiteral("model.source.unreadable"),
                  QStringLiteral("the model file could not be read"));
  }
  file.close();
  if (cancellation != nullptr && cancellation->isCancelled()) {
    return refuse(QStringLiteral("native.import.cancelled"),
                  QStringLiteral("the import was cancelled before inspection began"));
  }
  if (rawInputs.size() != 1) {
    return refuse(QStringLiteral("model.temporal.inputs"),
                  QStringLiteral("a temporal import needs one raw fp16 reference tensor"));
  }
  DeclaredBoundaries boundaries;
  QString declaredKey;
  QString declaredReason;
  if (!inspectDeclaredBoundaries(runtime, bytes, &boundaries, &declaredKey, &declaredReason)) {
    return refuse(declaredKey, declaredReason);
  }
  if (boundaries.inputs.size() != 1 || boundaries.outputs.size() != 1) {
    return refuse(QStringLiteral("model.profile.ineligible"),
                  QStringLiteral("the graph declares %1 input(s) and %2 output(s); exactly one input and one output are required").arg(boundaries.inputs.size()).arg(boundaries.outputs.size()));
  }
  if (rawInputs.first().name != boundaries.inputs.first().name) {
    return refuse(QStringLiteral("model.temporal.inputs"),
                  QStringLiteral("the temporal reference names input %1, but the graph declares input %2").arg(rawInputs.first().name, boundaries.inputs.first().name));
  }
  ModelProfile declared;
  declared.input = boundaries.inputs.first().profile;
  declared.output = boundaries.outputs.first().profile;
  const bool declaredFp16 = isEligibleIoType(declared.input.dtype) &&
                            isEligibleIoType(declared.output.dtype);
  if (!declaredFp16) {
    ModelProfile fp16Sample = declared;
    fp16Sample.input.dtype = QString::fromLatin1(kEligibleIoType);
    fp16Sample.output.dtype = QString::fromLatin1(kEligibleIoType);
    if (!temporalCandidateRefusal(fp16Sample).isEmpty()) {
      return refuse(QStringLiteral("model.profile.ineligible"), temporalCandidateRefusal(declared));
    }
    InspectionResult unsupported;
    unsupported.ok = true;
    unsupported.declared = declared;
    if (cancellation != nullptr && !cancellation->beginPublication()) {
      return refuse(QStringLiteral("native.import.cancelled"), QStringLiteral("cancelled"));
    }
    const QString displayNameU = sourceInfo.completeBaseName();
    return publish(bytes, unsupported, canonicalSource, displayNameU, backends, termsNote, ModelRole::Temporal);
  }
  const RawReferenceInput& raw = rawInputs.first();
  if (raw.dtype != QString::fromLatin1(kEligibleIoType) ||
      raw.shape.size() != 4 || raw.shape.at(0) != 1 || raw.shape.at(1) != 11 ||
      raw.shape.at(2) <= 0 || raw.shape.at(3) <= 0 ||
      raw.shape.at(2) > std::numeric_limits<int>::max() ||
      raw.shape.at(3) > std::numeric_limits<int>::max() ||
      (declared.input.height.has_value() && raw.shape.at(2) != *declared.input.height) ||
      (declared.input.width.has_value() && raw.shape.at(3) != *declared.input.width)) {
    return refuse(QStringLiteral("model.temporal.inputs"),
                  QStringLiteral("the temporal reference names the wrong tensor, dtype, or shape"));
  }
  const QString temporalRefusal = temporalCandidateRefusal(declared);
  if (!temporalRefusal.isEmpty()) {
    return refuse(QStringLiteral("model.profile.ineligible"), temporalRefusal);
  }

  QFile rawFile(raw.filePath);
  if (raw.filePath.isEmpty() || !rawFile.open(QIODevice::ReadOnly)) {
    return refuse(QStringLiteral("model.temporal.input.unreadable"),
                  QStringLiteral("the raw temporal reference tensor cannot be read"));
  }
  const QByteArray rawBytes = rawFile.readAll();
  if (rawFile.error() != QFile::NoError) {
    return refuse(QStringLiteral("model.temporal.input.unreadable"),
                  QStringLiteral("the raw temporal reference tensor could not be read"));
  }
  rawFile.close();
  const qint64 bytesPerRow = raw.shape.at(3) * 11 * 2;
  if (raw.shape.at(2) > std::numeric_limits<int>::max() / bytesPerRow ||
      rawBytes.size() != raw.shape.at(2) * bytesPerRow) {
    return refuse(QStringLiteral("model.temporal.inputs"),
                  QStringLiteral("the raw temporal reference tensor byte count does not match its fp16 shape"));
  }

  ReferenceInput referenceInput;
  referenceInput.name = raw.name;
  referenceInput.dtype = raw.dtype;
  referenceInput.shape = raw.shape;
  referenceInput.data = rawBytes;
  const ReferenceRun run = runReferenceTensor(runtime, bytes, {referenceInput});
  InspectionResult inspection;
  inspection.ok = true;
  inspection.inputName = raw.name;
  inspection.outputName = boundaries.outputs.first().name;
  inspection.declared = declared;
  if (!run.ok) {
    inspection.referenceKey = run.reasonKey;
    inspection.referenceReason = run.reason;
  } else if (run.outputs.size() != 1 ||
             run.outputs.first().name != boundaries.outputs.first().name) {
    return refuse(QStringLiteral("model.temporal.outputs"),
                  QStringLiteral("the temporal reference returned an unexpected output count or name"));
  } else {
    const ReferenceTensor& output = run.outputs.first();
    ModelProfile proof;
    proof.input.dtype = raw.dtype;
    proof.input.rank = raw.shape.size();
    proof.input.batch = raw.shape.at(0);
    proof.input.channels = raw.shape.at(1);
    proof.input.height = raw.shape.at(2);
    proof.input.width = raw.shape.at(3);
    proof.output.dtype = output.dtype;
    proof.output.rank = output.shape.size();
    if (output.shape.size() == 4) {
      proof.output.batch = output.shape.at(0);
      proof.output.channels = output.shape.at(1);
      proof.output.height = output.shape.at(2);
      proof.output.width = output.shape.at(3);
    }
    const QString proofRefusal = temporalReferenceRefusal(declared, proof);
    if (!proofRefusal.isEmpty()) {
      return refuse(QStringLiteral("model.temporal.boundary"), proofRefusal);
    }
    inspection.referenceRan = true;
    inspection.runGeometry = QSize(static_cast<int>(raw.shape.at(3)),
                                   static_cast<int>(raw.shape.at(2)));
    inspection.resolvedGeometry =
        QSize(static_cast<int>(*proof.output.width), static_cast<int>(*proof.output.height));
    inspection.resolvedScale = proof.derivedUniformScale();
    inspection.verifiedReferenceProfile = proof;
  }

  if (cancellation != nullptr && !cancellation->beginPublication()) {
    return refuse(QStringLiteral("native.import.cancelled"),
                  QStringLiteral("the import was cancelled before anything was published"));
  }
  const QString displayName = sourceInfo.completeBaseName();
  return publish(bytes, inspection, canonicalSource, displayName, backends, termsNote,
                 ModelRole::Temporal);
}

}  // namespace native
}  // namespace mplayer
