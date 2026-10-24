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

#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include "MpvApi.hpp"

namespace mplayer {
namespace native {

/// Persisted as the lowercase strings "standard" / "native", never as ordinals.
enum class BundleKind { Standard, Native };

/// Owned runtimes prove their complete directory tree. A host libmpv proves
/// only its canonical module; its dependencies remain the host loader's
/// responsibility and it must never be published as a self-contained bundle.
enum class BundleLayout { OwnedDirectory, SystemModule };

/// One declared file: root-relative, forward slashes, exact size, lower-case
/// SHA-256.
struct BundleFile {
    QString path;
    qint64 bytes = 0;
    QString sha256;
};

/// The three preparation executables a bundle ships. Root-relative, `.exe`,
/// each a member of `Bundle::files`; the layout is a declaration, not a claim
/// that a build will succeed.
struct BundlePreparationTools {
    QString inspect;
    QString prep;
    QString verifier;
};

/// The provider runtime directories the tools resolve their own DLLs against.
/// Root-relative single directory names; `trt` and `cuda` may be the same
/// directory, and a directory carrying DLLs must also be a declared dependency.
struct BundlePreparationRuntime {
    QString ort;
    QString trt;
    QString cuda;
};

/// The optional preparation block: which provider backends the toolchain can
/// prepare for, the source-ABI provenance of the inference core the bundle was
/// built from, the tools themselves, and their provider runtime directories.
///
/// The block is a validated statement about the bundle's own bytes, pinned by
/// the module and file hashes the inventory already proves. It is not an
/// independent measurement: `coreSha`/`sourceAbi` are not re-derived from the
/// binaries, and no CUDA/TensorRT version is asserted here - the tools' own
/// device query supplies those. Its presence says nothing about engine or
/// effective readiness.
struct BundlePreparation {
    QStringList types;
    QString coreSha;
    int sourceAbi = 0;
    BundlePreparationTools tools;
    BundlePreparationRuntime runtime;
};

/// `root` and `modulePath` are absolute and canonical. `deps` are dependency
/// directory names as the manifest states them, relative to `root`;
/// `toMpvBundleDescriptor()` is what turns them into the absolute directories
/// restricted DLL search needs. `files` is the complete declared inventory for
/// OwnedDirectory, or just the canonical libmpv for SystemModule. The latter
/// deliberately makes no claim about the host's dependency closure.
struct Bundle {
    BundleKind kind = BundleKind::Standard;
    BundleLayout layout = BundleLayout::OwnedDirectory;
    QString root;
    QString modulePath;
    QString descriptorId;
    QString version;
    quint64 mpvApiVersion = 0;
    int protocolVersion = 0;
    int engineSchemaVersion = 0;
    QStringList deps;
    QVector<BundleFile> files;
    /// Present exactly when the manifest declares a complete, valid
    /// `preparation` block. A bundle without one is a playback-only bundle:
    /// preparation is then named unavailable rather than completed.
    std::optional<BundlePreparation> preparation;

    /// Loader-facing projection. No IO, no module handle, no capability claim.
    MpvBundle toMpvBundleDescriptor() const;
};

/// The one manifest schema this build understands; any other value is refused
/// rather than interpreted.
QString bundleSchemaId();

/// The preparation backend types this build can prepare for, in a stable order.
/// A manifest naming any other type is refused rather than read as the one it
/// does.
QStringList bundlePreparationTypes();

/// The backend library a preparation type names inside the bundle:
/// "tensorrt" -> "aji_trt.dll", "directml" -> "aji_dml.dll"; empty for a type
/// this build does not know.
QString bundlePreparationBackendLibrary(const QString& type);

/// The library a native bundle ships that owns the source ABI: the dispatcher
/// the filter loads ("aji.dll"). Every declared backend is loaded through it, so
/// a preparation block is incoherent without it.
QString bundlePreparationProducerLibrary();

/// Parses a present `preparation` member. Shape only: a reserved placeholder
/// (forty zeroes) or a zero `sourceAbi` parses here and is refused by
/// `readBundle`/`validateBundle`, so a layout example stays readable while a
/// real bundle cannot carry an unmeasured value.
std::optional<BundlePreparation> parsePreparation(const QJsonObject& manifest,
                                                  QString* reason = nullptr);

QString bundleKindName(BundleKind kind);

/// `<applicationDirPath>/native`. Never created on read.
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
/// undeclared in an owned root. SystemModule instead proves exactly one
/// canonical host libmpv, not the directory around it. A statement about files,
/// never about a loaded module.
bool validateBundle(const Bundle& bundle, QString* reason = nullptr);

/// `validateBundle` with a trust-on-unchanged fast path.
///
/// A full validation hashes every declared file, which on a native bundle is
/// most of a gigabyte; doing that on every start is what made a launch take half
/// a minute. This computes a stat-only fingerprint of the tree first - every
/// file's relative path, size and modification time - and when it matches a
/// previously validated fingerprint, the earlier verdict is trusted.
///
/// Only a *valid* verdict is remembered: a failure is re-probed on the next call
/// so a repaired bundle or a reconnected disk is never remembered as broken.
/// `invalidateBundleValidation` drops a remembered verdict, which is what the
/// load-failure path does so a bundle that stops working is re-hashed.
bool validateBundleCached(const Bundle& bundle, QString* reason = nullptr);

/// Stat-only check of an exact descriptor validated in this process. Never
/// hashes payload bytes or repairs a missing/stale verdict: resolution must run
/// validateBundleCached off-thread first. Used at GUI-thread load/attach
/// boundaries so a cold cache cannot turn playback startup into a blocking hash.
bool isBundleValidationCurrent(const Bundle& bundle, QString* reason = nullptr);

/// Drops the remembered verdict for `root`, so the next check hashes again.
void invalidateBundleValidation(const QString& root);

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

/// `describeStandardRuntime` with the same trust-on-unchanged cache: describing a
/// stock runtime hashes every file beside the module and reads each DLL's
/// imports, so the descriptor is remembered against the tree's stat-only
/// fingerprint and rebuilt from the cache when nothing moved.
std::optional<Bundle> describeStandardRuntimeCached(const QString& modulePath,
                                                    QString* reason = nullptr);

/// Linux host-only selection: resolves the SONAME alias to one regular
/// canonical libmpv.so.2[.version], hashes that module, and records a stat-only
/// proof for GUI-thread commit. Does not inventory /usr/lib, follow unrelated
/// symlinks, claim private dependency ownership, or alter loader search paths.
/// Host dependency availability and the actual API are learned only at load.
std::optional<Bundle> describeSystemRuntimeCached(const QString& modulePath,
                                                 QString* reason = nullptr);

/// Stages into a same-volume `.staging-*` directory, copies only the verified
/// inventory plus the manifest, re-validates the staged payload from disk,
/// renames it into an immutable `<kind>-<version>` directory, and only then
/// writes the `current-<kind>` pointer through `QSaveFile`. Any failure removes
/// only the staging directory this call created and leaves every previously
/// published bundle and the pointer untouched. The stock
/// `%LOCALAPPDATA%/MPVBackend/runtime` tree is refused outright.
///
/// A stock `describeStandardRuntime` descriptor has no `bundle.json` beside it
/// and must not be written to. For exactly that case - kind Standard, a zero
/// load-time API version, and no manifest in the source root - the manifest is
/// materialised into the staging directory from the already-verified inventory
/// and version. The source is read, never written. A Native descriptor without
/// a manifest is refused rather than invented. On success `publishedRoot`, when
/// given, receives the absolute path of the immutable published directory.
///
/// Standard and Native are never mixed inside one bundle, so a descriptor whose
/// `kind` or `version` disagrees with the `bundle.json` beside it is refused by
/// name before anything is staged: publishing it would create a
/// `<kind>-<version>` directory whose name contradicts its own manifest, which
/// discovery can only refuse again.
bool installAtomic(const Bundle& source, const QString& runtimeRoot, QString* reason = nullptr,
                   QString* publishedRoot = nullptr);

/// `<kind>-<version>`, the immutable published directory name.
QString bundleDirectoryName(BundleKind kind, const QString& version);

/// `current-<kind>`, the per-kind pointer file name inside a runtime root.
QString bundlePointerFileName(BundleKind kind);

}  // namespace native
}  // namespace mplayer