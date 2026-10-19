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
// Storage layout lives here too: `<applicationDirPath>/native` with runtime,
// models, assets, artifacts, recipes, scratch and logs beneath it. A read never
// creates one.
//
// `nativePathTrust` is the one representability check every other boundary
// reuses before it hands a path to the platform: it answers what this path is,
// not where it points, so it composes with `safeRelativePath` and the
// containment helpers instead of replacing them. It exists because "the string
// is not empty" is not a safety property on Win32 - a NUL truncates the C
// string the loader actually sees, an unpaired surrogate has no UTF-8 byte
// encoding at all, and `\\?\`, `\\.\` and `\\server\share` reach namespaces that
// are not the user's directory.

#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

class QByteArray;
class QFileInfo;
class QSaveFile;

namespace mplayer {
namespace native {

/// Which namespace a path spells, so a caller that genuinely accepts one of
/// them can accept it deliberately instead of accepting it by accident.
enum class NativePathForm {
    /// A plain local path: `C:\models\pack`, or a bare relative name.
    Local,
    /// `\\server\share\...` - a network redirector root, not a local directory.
    Network,
    /// `\\?\...` or `\\.\...` - the extended-syntax and device namespaces,
    /// which resolve without the usual path parsing at all.
    Device,
};

/// The outcome of the one path check every native boundary runs before it opens,
/// loads or deserializes anything. It answers whether the platform can represent
/// the path at all; it does not answer where the path points. Containment stays
/// with `pathWithinRoot`/`pathHasNoReparsePoints` on the canonical form.
struct NativePathTrust {
    bool trusted = false;
    NativePathForm form = NativePathForm::Local;
    /// UTF-16 code units the path needs, terminator excluded. This is the count
    /// the wide Win32 APIs are bounded by; `AJI_NATIVE_PATH_CAPACITY` on the
    /// prepared POD is a UTF-8 *byte* capacity and is a separate limit.
    int wideLength = 0;
    /// UTF-8 bytes the path needs, terminator excluded.
    int byteLength = 0;
    /// False when the path cannot be spelled as well-formed UTF-8 at all: an
    /// embedded NUL, or an unpaired surrogate with no code point.
    bool representable = true;
    QString reason;
};

/// The longest path a Win32 wide API accepts, in UTF-16 code units. This is the
/// limit of the wide file APIs, which is not the 260 the non-wide ones impose, so
/// a long local path is never refused for length here - only where a caller
/// actually hands it to a narrow API.
constexpr int kNativeWidePathLimit = 32767;

/// The longest single component of a Win32 path may be, in UTF-16 code units.
/// The limit is per component, not on the path as a whole.
constexpr int kNativeComponentLimit = 255;

/// Classifies a path and proves Win32 can represent it, naming the refusal.
/// `requireAbsolute` additionally demands a drive-qualified or UNC root, which
/// is what a caller resolving a stored or manifest-supplied path needs before it
/// joins anything to it.
///
/// A drive colon, a comma, a percent sign and non-ASCII characters are all
/// ordinary local-path characters here and are accepted; what is refused is a
/// path that would change meaning: a truncation, a different namespace, an
/// absent root, or a length no Win32 call could carry.
NativePathTrust nativePathTrust(const QString& path, bool requireAbsolute);

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

/// `<applicationDirPath>/native`. Never created by this call.
QString nativeRoot();

/// The app-owned subdirectory names, in the order §9.2 lists them, with the
/// source-asset store M35 adds (`assets`) named alongside the rest.
QStringList nativeSubdirectoryNames();

/// `nativeRoot()/<name>` for a name from `nativeSubdirectoryNames()`, or empty
/// with `reason` set for anything else. Never created by this call.
QString nativeSubdirectory(const QString& name, QString* reason = nullptr);
QString nativeToolsDirectory();

/// The outcome of `readSharedMetadataFile`. `Unreadable` means the file could
/// not be opened or read; `TooLarge` means it held more than the caller's byte
/// bound; `Ok` means `bytes` holds the file's snapshot.
enum class SharedMetadataReadStatus {
    Ok,
    Unreadable,
    TooLarge,
};

/// Reads at most `maxBytes` from `path` and stores the snapshot in `bytes`.
/// `bytes` is cleared on every non-Ok outcome. `error`, when given, carries
/// the platform's open/read reason for `Unreadable`.
///
/// On Windows the bounded open/read/close runs under the metadata read lock,
/// so an in-process `commitMetadataFile` cannot replace the file mid-read;
/// parsing stays outside the lock. The read is bounded to `maxBytes + 1`
/// bytes, so a file that grows after any size probe is refused as `TooLarge`
/// rather than parsed truncated.
SharedMetadataReadStatus readSharedMetadataFile(const QString& path, qint64 maxBytes,
                                                QByteArray* bytes, QString* error = nullptr);

/// NTFS's system change time in milliseconds, or `info.metadataChangeTime()`
/// when the platform exposes no separate change time. On Windows the attribute
/// handle is opened under the metadata read lock, so an in-process commit
/// cannot run between the query and its handle close.
qint64 metadataChangeTimeMs(const QString& path, const QFileInfo& info);

/// Commits `file` to its destination and reports QSaveFile's original failure
/// through `error`, which is cleared on success. On Windows the commit runs
/// under a write lock because Qt 6.12's QSaveFile::commit() uses a non-POSIX
/// replace-rename that refuses to replace a destination with any open handle;
/// the lock therefore excludes every in-process metadata reader. Elsewhere it
/// is an ordinary commit. Callers keep writing, hashing and preparing before
/// the call, outside the lock.
bool commitMetadataFile(QSaveFile& file, QString* error = nullptr);

}  // namespace native
}  // namespace mplayer