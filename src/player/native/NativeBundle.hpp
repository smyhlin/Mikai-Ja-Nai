// A runtime bundle: one coherent mpv module plus its self-contained dependency
// closure, described by a manifest whose every claim is checked against the
// bytes on disk. Nothing here loads a module, so a passing `validateBundle`
// deliberately says nothing about whether a native filter registers.
//
// Standard and Native share the machinery and are never mixed inside one
// bundle. Installed bundles live as immutable `<kind>-<version>` directories
// under `native/runtime` plus a `current-<kind>` pointer; the sole read-only
// exception is `describeStandardRuntime`, which describes the stock runtime
// where it already is and writes nothing.
//
// Every entry point returns an explicit result or a `reason` naming the
// offending file, so a caller can tell "none found" from "found and rejected".

#pragma once

#include <optional>

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include "MpvApi.hpp"

namespace mplayer {
namespace native {

/// Persisted as the lowercase strings "standard" / "native", never as ordinals.
enum class BundleKind { Standard, Native };

/// One declared file: root-relative, forward slashes, exact size, lower-case
/// SHA-256.
struct BundleFile {
    QString path;
    qint64 bytes = 0;
    QString sha256;
};

/// `root` and `modulePath` are absolute and canonical. `deps` are dependency
/// directory names as the manifest states them, relative to `root`;
/// `toMpvBundleDescriptor()` is what turns them into the absolute directories
/// restricted DLL search needs. `files` is the complete declared inventory, so
/// a caller need not re-read the manifest to enumerate the closure.
struct Bundle {
    BundleKind kind = BundleKind::Standard;
    QString root;
    QString modulePath;
    QString descriptorId;
    QString version;
    quint64 mpvApiVersion = 0;
    int protocolVersion = 0;
    int engineSchemaVersion = 0;
    QStringList deps;
    QVector<BundleFile> files;

    /// Loader-facing projection. No IO, no module handle, no capability claim.
    MpvBundle toMpvBundleDescriptor() const;
};

/// The one manifest schema this build understands; any other value is refused
/// rather than interpreted.
QString bundleSchemaId();

QString bundleKindName(BundleKind kind);

/// `AppLocalDataLocation()/native`. Never created on read.
QString nativeStorageRoot();

/// `nativeStorageRoot()/runtime`.
QString runtimeStorageRoot();

/// Strict parse of `<root>/bundle.json`: exact schema id, every required member
/// present, no unknown member, known kind, explicit version, and a module and
/// inventory that survive `safeRelativePath`. Values this build cannot honour -
/// a future protocol or engine schema, an mpv API version outside the supported
/// window - are refused rather than reinterpreted.
std::optional<Bundle> readBundle(const QString& root, QString* reason = nullptr);

/// Checks the bundle against the bytes on disk: the inventory exists, matches
/// its recorded size and digest, stays inside the root, crosses no reparse
/// point, names a real x64 PE for every DLL, leaves no imported module
/// unresolved except the explicit Windows system exceptions, and leaves nothing
/// undeclared in the root. A statement about files, never about a loaded module.
bool validateBundle(const Bundle& bundle, QString* reason = nullptr);

/// Valid bundles under `roots`, best first. Defaults to the app-owned runtime
/// storage plus `native/runtime` beside the executable; within each root the
/// `current-<kind>` pointer outranks versioned directories, newest first.
/// Every failing candidate is named in `rejections` and does not stop the
/// search, so a corrupt bundle cannot hide a valid one of the same kind.
/// Staging directories are never candidates.
QList<Bundle> discoverBundles(const QStringList& roots = {}, QStringList* rejections = nullptr);

/// Describes the already-present stock runtime without copying or modifying
/// anything. The inventory is computed from the actual bytes in the module's
/// directory tree, so it reports what is really there; a module whose imports
/// cannot be satisfied inside that tree plus the system exceptions is refused
/// by name, because that is the shape of a runtime that fails at load for
/// reasons the user can act on.
std::optional<Bundle> describeStandardRuntime(const QString& modulePath, QString* reason = nullptr);

/// Stages into a same-volume `.staging-*` directory, copies only the verified
/// inventory plus the manifest, re-validates the staged payload from disk,
/// renames it into an immutable `<kind>-<version>` directory, and only then
/// writes the `current-<kind>` pointer through `QSaveFile`. Any failure removes
/// only the staging directory this call created and leaves every previously
/// published bundle and the pointer untouched. The stock
/// `%LOCALAPPDATA%/MPVBackend/runtime` tree is refused outright.
bool installAtomic(const Bundle& source, const QString& runtimeRoot, QString* reason = nullptr);

/// `<kind>-<version>`, the immutable published directory name.
QString bundleDirectoryName(BundleKind kind, const QString& version);

/// `current-<kind>`, the per-kind pointer file name inside a runtime root.
QString bundlePointerFileName(BundleKind kind);

}  // namespace native
}  // namespace mplayer