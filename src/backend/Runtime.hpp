#pragma once

#include <QDir>
#include <QString>
#include <QStringList>

#include <optional>

// Stock mpv runtime discovery. It finds folders holding a libmpv: the one a
// previous run provisioned, or one shipped next to the executable. On Windows
// x64 it can also fetch the latest mpv development archive.
namespace mikai::backend {

QDir cacheDir();
QStringList libraryNames();

// Every candidate module the stock search walks, in the order it walks them:
// the hint first, then the bundled locations beside the executable, then the
// runtime a previous run downloaded. A caller validates each one and skips a
// broken module instead of being stopped by whichever name it hit first. Paths
// are absolute and name a file, deduplicated case-insensitively.
QStringList runtimeCandidates(const QString& hint = {});

// Downloads the stock mpv development archive and installs its full closure
// into `destination`, which is created when missing. The destination is the
// caller's: the stock cache is only touched when the caller names it, so an
// app-owned scratch directory can be provisioned without disturbing a
// reference install. Returns the directory holding the library.
QDir provisionRuntime(const QString& destination);

}  // namespace mikai::backend
