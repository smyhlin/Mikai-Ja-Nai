#pragma once

#include <QDir>
#include <QString>
#include <QStringList>

// mpv runtime discovery. It finds modules shipped beside the executable;
// callers may provide an explicit hint for probes and developer use.
namespace mikai::backend {

QDir cacheDir();
QStringList libraryNames();

// Every candidate module the search walks, in the order it walks them: an
// explicit hint first, then the bundled locations beside the executable. A
// caller validates each one and skips a broken module instead of being stopped
// by whichever name it hit first. Paths are absolute and name a file,
// deduplicated case-insensitively.
QStringList runtimeCandidates(const QString& hint = {});

}  // namespace mikai::backend
