// The one path policy the native track uses, so a second convention cannot
// grow: manifests, artifact caches, model directories and scratch space all
// resolve through these helpers.
//
// A stored name is relative and normalized before it is joined to a root
// (absolute paths, drive letters, backslashes, `.`/`..`, wildcards, reserved
// device names and trailing space/dot are refused); containment is decided on
// canonical paths with the platform's case rules; and no component from the
// root down to the target may be a reparse point, because a junction inside an
// owned directory can redirect a resolved path outside it.
//
// Storage layout lives here too: `AppLocalDataLocation/native` with runtime,
// models, assets, artifacts, recipes, scratch and logs beneath it. A read never
// creates one.

#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace mplayer {
namespace native {

/// Normalizes a stored relative path, or returns an empty string with `reason`
/// set. The result uses forward slashes and is safe to join to an owned root.
QString safeRelativePath(const QString& raw, QString* reason = nullptr);

/// The case sensitivity Windows uses for path comparison, so a caller building
/// its own containment check does not re-derive it.
Qt::CaseSensitivity pathCaseSensitivity();

/// True when `candidate` is `root` or lies beneath it, after cleaning both and
/// comparing with the platform's case rules.
bool pathWithinRoot(const QString& root, const QString& candidate);

/// Canonical (symlink-resolved) form of an existing path, or empty when the
/// path does not exist.
QString canonicalPath(const QString& path);
/// The deepest ancestor of `path` that exists - `path` itself when it does - or
/// an empty string when no component of it does. Cleaned, never created, and not
/// canonicalized: a caller that has to judge a path before it is created decides
/// containment with this and then proves the resolved ancestor itself.
QString nearestExistingAncestor(const QString& path);

/// `root` down to `path`, nearest first. Empty when `path` is not inside `root`.
QStringList ancestorChain(const QString& root, const QString& path);

/// True when both exist, `path` is inside `root`, and no component on the way is
/// a reparse point.
bool pathHasNoReparsePoints(const QString& root, const QString& path);

/// `AppLocalDataLocation()/native`. Never created by this call.
QString nativeRoot();

/// The app-owned subdirectory names, in the order §9.2 lists them, with the
/// source-asset store M35 adds (`assets`) named alongside the rest.
QStringList nativeSubdirectoryNames();

/// `nativeRoot()/<name>` for a name from `nativeSubdirectoryNames()`, or empty
/// with `reason` set for anything else. Never created by this call.
QString nativeSubdirectory(const QString& name, QString* reason = nullptr);

}  // namespace native
}  // namespace mplayer