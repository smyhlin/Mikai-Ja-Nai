// What a Windows binary declares about itself, read from its bytes alone.
// Nothing here loads a module or resolves an export: a passing result means
// "well-formed 64-bit PE whose imports are these names", never that the image
// will load or that a native filter registered.
//
// The delay-import table is walked too - satisfying eager imports while
// ignoring delayed ones has not closed a closure - including its VA-based
// descriptor form, which is why the image base is read here.

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

/// Parses an in-memory PE image. Never throws, never loads.
PeImageInfo inspectPeImage(const QByteArray& bytes);

/// The only external dependency exception in the design: modules the operating
/// system owns. API sets match by prefix, since the system decides which
/// contract a given one forwards to. Adding a redistributable library here
/// would silently weaken the closure check.
QStringList systemImportExceptions();

bool isSystemImportException(const QString& dllName);

}  // namespace native
}  // namespace mplayer