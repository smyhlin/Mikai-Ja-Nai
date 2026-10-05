
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace mplayer {
namespace native {

/// Lower-cased, deduplicated, sorted so two runs over the same bytes compare
/// equal regardless of descriptor order.
struct PeImageInfo {
    bool ok = false;
    QString reason;
    quint16 machine = 0;
    QStringList imports;
    QStringList delayImports;
};

struct PeExportInfo {
    bool ok = false;
    QString reason;
    bool exportsMpvClientApiVersion = false;
};

/// Parses an in-memory PE image. Never throws, never loads.
PeImageInfo inspectPeImage(const QByteArray& bytes);

PeExportInfo inspectPeExports(const QByteArray& bytes);

/// The only external dependency exception in the design: modules the operating
/// system owns. API sets match by prefix, since the system decides which
/// contract a given one forwards to. Adding a redistributable library here
/// would silently weaken the closure check.
QStringList systemImportExceptions();

bool isSystemImportException(const QString& dllName);

}  // namespace native
}  // namespace mplayer