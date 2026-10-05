// A reference export is not a player: it reads the files a caller names, runs
// one CPU inference, and exits.

#pragma once

#include <optional>

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include "native/prepare/NativeFrameFixture.hpp"
#include "native/models/NativeModelInspect.hpp"

namespace mplayer {
namespace native {

/// Reads the P6 fixture this exporter feeds. The frame type and the canonical
/// packer it produces live in NativeFrameFixture.hpp (unconditional); this
/// reader stays with the exporter because it hashes the payload with the
/// model-side digest helper, so it is only built together with the inspector.
bool readPpmFixture(const QString& path, FrameFixture* fixture, QString* reasonKey = nullptr,
                    QString* reason = nullptr);

struct RawReferenceInput {
  QString name;
  /// `fp16` or `fp32`. No other dtype is read from a file, and no tensor is ever
  /// converted into a dtype the caller did not name.
  QString dtype;
  /// Every dimension strictly positive; a symbolic dimension cannot be fed.
  QVector<qint64> shape;
  QString filePath;
};

struct ReferenceExportRequest {
  /// Must not exist: an export never overwrites, and a directory it did not
  /// create is never removed - not even to clean up a later failure.
  QString directory;
  /// Mutually exclusive with `rawInputs`.
  QString fixturePath;
  /// Must equal the fixture's own geometry; a frame is never stretched to fit.
  QSize resolveGeometry;
  /// Empty selects the fixture path.
  QVector<RawReferenceInput> rawInputs;
};

struct ReferenceExport {
  QString directory;
  QJsonObject manifest;
  QStringList inputPaths;
  QStringList outputPaths;
  QString manifestPath;
};

/// `graphSha256` is the digest of the single in-memory snapshot the session is
/// created from, so it cannot describe bytes other than the ones that ran.
///
/// Nothing is written unless the run succeeded, and `reference.json` is
/// committed last: a directory containing it is a complete export.
bool exportCpuFrameReference(const InspectionRuntime& runtime, const QByteArray& graphBytes,
                             const QString& graphPath, const ReferenceExportRequest& request,
                             ReferenceExport* result, QString* reasonKey = nullptr,
                             QString* reason = nullptr);

}  // namespace native
}  // namespace mplayer
