#include "NativePaths.hpp"

#include "native/bundle/NativeDiagnostics.hpp"

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtCore/QReadWriteLock>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>

#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace mplayer {
namespace native {
namespace {

/// CON, PRN, AUX, NUL, COM1-9, LPT1-9. A name that is one of these never
/// reaches the bytes it describes: every handle opened with it lands on a
/// device, and a hash of that device is not the file the record claimed.
bool isReservedDeviceName(const QString& segment) {
    QString base = segment;
    const qsizetype dot = base.indexOf(QLatin1Char('.'));
    if (dot >= 0) base = base.left(dot);
    base = base.toUpper();
    if (base == QLatin1String("CON") || base == QLatin1String("PRN") ||
        base == QLatin1String("AUX") || base == QLatin1String("NUL"))
        return true;
    if (base.size() != 4) return false;
    const QChar digit = base.at(3);
    return (base.left(3) == QLatin1String("COM") || base.left(3) == QLatin1String("LPT")) &&
           digit.isDigit() && digit != QLatin1Char('0');
}

bool hasForbiddenCharacter(const QString& segment) {
    for (const QChar c : segment) {
        // Control characters plus < > : " | ? *: a wildcard would make the
        // name a pattern rather than a file, and ':' a drive letter or an
        // alternate data stream.
        if (c.unicode() < 0x20) return true;
        switch (c.unicode()) {
            case '<': case '>': case ':': case '"': case '|': case '?': case '*':
                return true;
            default:
                break;
        }
    }
    return false;
}

bool isReparsePoint(const QString& path) {
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<const wchar_t*>(path.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return QFileInfo(path).isSymLink();
#endif
}

/// True for a code unit that cannot stand alone in a UTF-8 encoding. A high
/// surrogate is well-formed only when the next unit is its low partner, and a
/// low surrogate never begins a pair. Qt stores a lone one as itself and would
/// otherwise replace it with U+FFFD, which is a *different* path than the caller
/// named - and one that may well exist.
bool isUnpairedSurrogate(QChar unit, const QString& path, qsizetype at) {
    const char16_t value = unit.unicode();
    if (value >= 0xDC00 && value <= 0xDFFF) {
        // A low surrogate closes a pair; anything before it must already have
        // been consumed by the high-surrogate branch below.
        return at == 0 || !(path.at(at - 1).unicode() >= 0xD800 && path.at(at - 1).unicode() <= 0xDBFF);
    }
    if (value < 0xD800 || value > 0xDBFF) return false;
    return at + 1 >= path.size() ||
           !(path.at(at + 1).unicode() >= 0xDC00 && path.at(at + 1).unicode() <= 0xDFFF);
}

/// The number of UTF-16 code units a path occupies, which is what every wide
/// Win32 API bounds itself by. Counting units rather than code points keeps a
/// supplementary character - one code point, two units, four UTF-8 bytes - from
/// being compared against a limit it does not belong to.
int wideUnitLength(const QString& path) {
    int units = 0;
    for (qsizetype i = 0; i < path.size(); ++i) {
        const char16_t value = path.at(i).unicode();
        if (value >= 0xD800 && value <= 0xDBFF && i + 1 < path.size()) ++i;
        ++units;
    }
    return units;
}

/// The two-character prefixes Win32 gives a meaning of their own, and which the
/// usual path normalization does not strip: `\\?\` turns off all parsing, and
/// `\\.\` names a device. Both spell `\\`, so they are decided from the third
/// unit rather than by splitting on separators.
bool isDevicePrefix(const QString& path) {
    return path.size() >= 4 && path.at(0) == QLatin1Char('\\') && path.at(1) == QLatin1Char('\\') &&
           (path.at(2) == QLatin1Char('?') || path.at(2) == QLatin1Char('.')) &&
           path.at(3) == QLatin1Char('\\');
}

#ifdef Q_OS_WIN
/// The spelling a Win32 file API needs so that neither MAX_PATH nor the normal
/// parser rewrites the name. An already-extended path is left alone, a share
/// keeps its root under the UNC form, and a local path gains the plain prefix.
QString extendedWindowsPath(const QString& path) {
    const QString nativePath = QDir::toNativeSeparators(path);
    if (nativePath.startsWith(QLatin1String("\\\\?\\"))) return nativePath;
    if (nativePath.startsWith(QLatin1String("\\\\"))) {
        return QStringLiteral("\\\\?\\UNC\\") + nativePath.mid(2);
    }
    // The extended form only qualifies a fully rooted name; a drive-relative or
    // relative path must keep the form CreateFile already resolves.
    const bool driveRooted =
        nativePath.size() >= 3 && nativePath.at(0).isLetter() &&
        nativePath.at(1) == QLatin1Char(':') && nativePath.at(2) == QLatin1Char('\\');
    if (!driveRooted) return nativePath;
    return QStringLiteral("\\\\?\\") + nativePath;
}

/// Serializes in-process metadata publication with every in-process reader of
/// the same files. Qt 6.12's Windows QSaveFile::commit() replaces the
/// destination with a non-POSIX FileRenameInfo that refuses any open handle.
QReadWriteLock& metadataPublicationLock() {
    static QReadWriteLock lock;
    return lock;
}

#endif

}  // namespace

NativePathTrust nativePathTrust(const QString& path, bool requireAbsolute) {
    NativePathTrust trust;
    // Several of these refusals quote the offending path, and a path can carry a
    // URL-shaped run or a token. Every refusal leaves through here so the
    // diagnostic boundary is crossed once, at the one place this check produces
    // text a caller might log.
    const auto refuse = [&trust](const QString& text) {
        trust.trusted = false;
        trust.reason = redactNativeDiagnosticText(text);
        return trust;
    };

    if (path.isEmpty()) return refuse(QStringLiteral("the path is empty"));

    // The whole point of this check: a NUL survives a length-delimited buffer
    // but not the C string the loader, the file API or the UTF-8 conversion reads.
    if (path.contains(QChar(0))) {
        trust.representable = false;
        return refuse(QStringLiteral("the path carries an embedded NUL, which would truncate "
                                     "the path the platform receives"));
    }
    for (qsizetype i = 0; i < path.size(); ++i) {
        if (!isUnpairedSurrogate(path.at(i), path, i)) continue;
        trust.representable = false;
        return refuse(QStringLiteral("the path carries an unpaired UTF-16 surrogate at offset "
                                     "%1, which has no UTF-8 encoding and would name a "
                                     "different file than the caller asked for")
                          .arg(i));
    }

    // Qt's conversion is what the rest of this program hands to Win32, so it is
    // the conversion whose product is measured, and an invalid byte sequence
    // (which a well-formed QString does not produce, but a hand-built one could)
    // is caught rather than counted.
    const QByteArray utf8 = path.toUtf8();
    if (QString::fromUtf8(utf8) != path) {
        trust.representable = false;
        return refuse(QStringLiteral("the path does not survive a UTF-8 round trip, so no Win32 "
                                     "wide API could receive the same name"));
    }
    trust.byteLength = static_cast<int>(utf8.size());

    if (isDevicePrefix(path)) {
        trust.form = NativePathForm::Device;
        return refuse(QStringLiteral("the path names the Win32 device or extended-syntax namespace "
                                     "(%1), which resolves without path parsing and is never a "
                                     "user's model or artifact directory")
                          .arg(QString(path.left(6))));
    }
    if (path.startsWith(QLatin1String("\\\\"))) {
        trust.form = NativePathForm::Network;
        return refuse(QStringLiteral("the path names a network share (%1), not a local "
                                     "directory; a redirector root is not something this "
                                     "application owns")
                          .arg(QString(path.left(std::min<qsizetype>(path.size(), 8)))));
    }
    trust.form = NativePathForm::Local;

    const bool driveRooted =
        path.size() >= 3 && path.at(1) == QLatin1Char(':') && path.at(0).isLetter() &&
        (path.at(2) == QLatin1Char('/') || path.at(2) == QLatin1Char('\\'));
    const bool rooted = driveRooted || path.startsWith(QLatin1Char('/'));
    if (requireAbsolute && !rooted) {
        return refuse(QStringLiteral("the path %1 is not absolute, so it would resolve against "
                                     "the process working directory")
                          .arg(path));
    }

    trust.wideLength = wideUnitLength(path);
    // The wide APIs carry a terminator inside this budget; a path of exactly the
    // limit therefore still needs one more unit than a call can pass.
    if (trust.wideLength + 1 > kNativeWidePathLimit) {
        return refuse(QStringLiteral("the path needs %1 UTF-16 code units, which exceeds the %2 a "
                                     "Win32 wide path API accepts")
                          .arg(trust.wideLength)
                          .arg(kNativeWidePathLimit));
    }

    // The per-component limit is separate from the total: a path can be short
    // overall and still name a component no file system can hold.
    qsizetype componentStart = driveRooted ? 3 : (rooted ? 1 : 0);
    for (qsizetype i = componentStart; i <= path.size(); ++i) {
        const bool atEnd = i == path.size();
        if (!atEnd && path.at(i) != QLatin1Char('/') && path.at(i) != QLatin1Char('\\')) continue;
        const int componentLength = static_cast<int>(i - componentStart);
        if (componentLength > kNativeComponentLimit) {
            return refuse(QStringLiteral("path component %1 is %2 UTF-16 code units long, which "
                                         "exceeds the %3 a Win32 path component accepts")
                              .arg(QString(path.mid(componentStart, componentLength)))
                              .arg(componentLength)
                              .arg(kNativeComponentLimit));
        }
        componentStart = i + 1;
    }

    trust.trusted = true;
    return trust;
}

QString safeRelativePath(const QString& raw, QString* reason) {
    const auto refuse = [reason](const QString& text) {
        if (reason) *reason = text;
        return QString{};
    };
    if (reason) reason->clear();
    if (raw.isEmpty()) return refuse(QStringLiteral("path is empty"));
    // The representability check runs first: it is the one that decides whether a
    // NUL or a lone surrogate would reach the platform at all, and the spelling
    // rules below only judge the path that survives it. A stored name that spells
    // the device or extended-syntax namespace is refused here with the reason the
    // check gave, because no per-segment rule would ever see those characters.
    const NativePathTrust trust = nativePathTrust(raw, false);
    if (!trust.representable || trust.form == NativePathForm::Device) return refuse(trust.reason);
    if (raw.contains(QLatin1Char('\\')))
        return refuse(QStringLiteral("path %1 uses a backslash; stored paths use '/'").arg(raw));
    if (raw.startsWith(QLatin1Char('/'))) return refuse(QStringLiteral("path %1 is absolute").arg(raw));
    if (raw.size() >= 2 && raw.at(1) == QLatin1Char(':'))
        return refuse(QStringLiteral("path %1 names a drive").arg(raw));

    for (const QString& segment : raw.split(QLatin1Char('/'))) {
        if (segment.isEmpty()) return refuse(QStringLiteral("path %1 has an empty segment").arg(raw));
        if (segment == QLatin1String("."))
            return refuse(QStringLiteral("path %1 has a '.' segment").arg(raw));
        if (segment == QLatin1String(".."))
            return refuse(QStringLiteral("path %1 has a '..' segment").arg(raw));
        if (hasForbiddenCharacter(segment))
            return refuse(QStringLiteral("path %1 has a character Windows does not allow").arg(raw));
        if (segment.endsWith(QLatin1Char(' ')) || segment.endsWith(QLatin1Char('.')))
            return refuse(QStringLiteral("path segment %1 ends with a space or dot").arg(segment));
        if (isReservedDeviceName(segment))
            return refuse(QStringLiteral("path segment %1 is a reserved device name").arg(segment));
    }
    return raw;
}

Qt::CaseSensitivity pathCaseSensitivity() {
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString canonicalPath(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists()) return {};
    return info.canonicalFilePath();
}
QString nearestExistingAncestor(const QString& path) {
    if (path.isEmpty()) return {};
    QString current = QDir::cleanPath(path);
    for (;;) {
        if (QFileInfo::exists(current)) return current;
        const QString parent = QFileInfo(current).absolutePath();
        // A component that does not exist cannot contain the app-owned tree, and
        // the filesystem root has no parent to step up to.
        if (parent.isEmpty() || parent == current) return {};
        current = parent;
    }
}

bool pathWithinRoot(const QString& root, const QString& candidate) {
    if (root.isEmpty() || candidate.isEmpty()) return false;
    const QString cleanRoot = QDir::cleanPath(root);
    const QString cleanCandidate = QDir::cleanPath(candidate);
    if (cleanCandidate.compare(cleanRoot, pathCaseSensitivity()) == 0) return true;
    return cleanCandidate.startsWith(cleanRoot, pathCaseSensitivity()) &&
           (cleanRoot.endsWith(QLatin1Char('/')) ||
            (cleanCandidate.size() > cleanRoot.size() &&
             cleanCandidate.at(cleanRoot.size()) == QLatin1Char('/')));
}

QStringList ancestorChain(const QString& root, const QString& path) {
    QStringList chain;
    const QString cleanRoot = QDir::cleanPath(root);
    QString current = QDir::cleanPath(path);
    if (cleanRoot.isEmpty() || current.isEmpty()) return chain;
    if (!pathWithinRoot(cleanRoot, current)) return chain;

    chain.append(current);
    while (current.compare(cleanRoot, pathCaseSensitivity()) != 0) {
        const qsizetype slash = current.lastIndexOf(QLatin1Char('/'));
        if (slash < 0) break;
        current.truncate(std::max(slash, cleanRoot.size()));
        chain.append(current);
    }
    return chain;
}

bool pathHasNoReparsePoints(const QString& root, const QString& path) {
    const QStringList chain = ancestorChain(root, path);
    if (chain.isEmpty()) return false;  // outside the root, or no such root
    for (const QString& component : chain) {
        if (isReparsePoint(component)) return false;
    }
    return true;
}

namespace {

/// True when a directory accepts new entries. A permission bit is not enough:
/// an AppImage mount and a read-only bind mount both report a writable mode and
/// still refuse every create, so the probe has to try.
bool directoryAcceptsWrites(const QString& dir) {
    if (dir.isEmpty() || !QFileInfo(dir).isDir() || !QFileInfo(dir).isWritable()) return false;
    QTemporaryDir probe(QDir(dir).filePath(QStringLiteral("mikai-write-probe-XXXXXX")));
    return probe.isValid();
}

/// The store lives beside the executable for a portable tree and for every
/// developer build. A packaged tree is read-only: an AppImage is a squashfs
/// mount (/tmp/.mount_*) and a package installs under /usr, so nothing beside
/// that executable can be created and the per-user data location takes over.
QString chooseNativeRoot() {
    const QString override = qEnvironmentVariable("MIKAI_NATIVE_ROOT").trimmed();
    if (!override.isEmpty()) return QDir::cleanPath(override);
    const QString exeDir = QCoreApplication::applicationDirPath();
    if (!exeDir.isEmpty() && directoryAcceptsWrites(exeDir)) {
        return QDir(exeDir).filePath(QStringLiteral("native"));
    }
    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!dataDir.isEmpty()) return QDir(dataDir).filePath(QStringLiteral("native"));
    return exeDir.isEmpty() ? QString() : QDir(exeDir).filePath(QStringLiteral("native"));
}

}  // namespace

QString nativeRoot() {
    static const QString root = chooseNativeRoot();
    return root;
}

QStringList nativeSubdirectoryNames() {
    return {QStringLiteral("runtime"), QStringLiteral("models"),  QStringLiteral("assets"),
            QStringLiteral("artifacts"), QStringLiteral("recipes"), QStringLiteral("scratch"),
            QStringLiteral("logs"), QStringLiteral("components"), QStringLiteral("tools")};
}

QString nativeSubdirectory(const QString& name, QString* reason) {
    if (reason) reason->clear();
    if (!nativeSubdirectoryNames().contains(name)) {
        if (reason)
            *reason = QStringLiteral("'%1' is not an app-owned native directory (%2)")
                          .arg(name, nativeSubdirectoryNames().join(QLatin1String(", ")));
        return {};
    }
    return QDir(nativeRoot()).filePath(name);
}

QString nativeToolsDirectory() {
    const QString root = nativeRoot();
    return root.isEmpty() ? QString{} : QDir(root).filePath(QStringLiteral("tools/7zip-26.03"));
}

QStringList pruneNativeStore(const QString& rootOverride) {
    const QString root = rootOverride.isEmpty() ? nativeRoot() : QDir::cleanPath(rootOverride);
    if (root.isEmpty() || !QFileInfo(root).isDir()) return {};
    // A writer holds a stage for the length of one archive extraction and a
    // partial for one file copy; an hour is far past either, and a backup is
    // only kept until its replacement finished, which is seconds. Anything
    // younger than its own window may belong to another window of this app, so
    // it is left alone rather than raced.
    constexpr qint64 kScratchSeconds = 24 * 60 * 60;
    constexpr qint64 kStageSeconds = 60 * 60;
    constexpr qint64 kBackupSeconds = 24 * 60 * 60;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QStringList removed;

    const auto olderThan = [&](const QFileInfo& info, qint64 seconds) {
        return info.lastModified().isValid() && info.lastModified().toUTC().secsTo(now) > seconds;
    };
    const auto drop = [&](const QString& path) {
        const QFileInfo info(path);
        const bool ok = info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
        if (ok) removed.append(path);
    };

    const QString scratch = QDir(root).filePath(QStringLiteral("scratch"));
    if (QFileInfo(scratch).isDir()) {
        const QFileInfoList entries = QDir(scratch).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo& entry : entries) {
            if (entry.isSymLink()) continue;
            if (olderThan(entry, kScratchSeconds)) drop(entry.absoluteFilePath());
        }
    }

    // Staging trees and interrupted copies live inside the component tree, at a
    // depth the installer writes them at: components/<kind>/<version>/.
    const QString components = QDir(root).filePath(QStringLiteral("components"));
    if (QFileInfo(components).isDir()) {
        for (const QFileInfo& kind : QDir(components).entryInfoList(
                 QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
            for (const QFileInfo& release : QDir(kind.absoluteFilePath()).entryInfoList(
                     QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
                for (const QFileInfo& entry : QDir(release.absoluteFilePath()).entryInfoList(
                         QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
                    if (entry.isSymLink()) continue;
                    const QString name = entry.fileName();
                    if (name.startsWith(QStringLiteral("aji-stage-"))) {
                        if (olderThan(entry, kStageSeconds)) drop(entry.absoluteFilePath());
                    } else if (name.endsWith(QStringLiteral(".partial"))) {
                        if (olderThan(entry, kStageSeconds)) drop(entry.absoluteFilePath());
                    } else if (name.endsWith(QStringLiteral(".previous"))) {
                        if (olderThan(entry, kBackupSeconds)) drop(entry.absoluteFilePath());
                    }
                }
            }
        }
    }
    return removed;
}

qint64 metadataChangeTimeMs(const QString& path, const QFileInfo& info) {
#ifdef Q_OS_WIN
    {
        const QReadLocker locker(&metadataPublicationLock());
        const QString nativePath = extendedWindowsPath(path);
        const HANDLE handle = ::CreateFileW(
            reinterpret_cast<LPCWSTR>(nativePath.utf16()), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            FILE_BASIC_INFO basicInfo{};
            const BOOL queried = ::GetFileInformationByHandleEx(
                handle, FileBasicInfo, &basicInfo, sizeof(basicInfo));
            ::CloseHandle(handle);
            const qint64 ticks = basicInfo.ChangeTime.QuadPart;
            if (queried && ticks > 0) {
                constexpr qint64 kTicksPerMillisecond = 10000;               // FILETIME is 100 ns
                constexpr qint64 kEpochOffsetMilliseconds = 11644473600000;  // 1601 -> 1970
                return ticks / kTicksPerMillisecond - kEpochOffsetMilliseconds;
            }
        }
    }
#else
    Q_UNUSED(path);
#endif
    return info.metadataChangeTime().toMSecsSinceEpoch();
}

SharedMetadataReadStatus readSharedMetadataFile(const QString& path, qint64 maxBytes,
                                                QByteArray* bytes, QString* error) {
    if (bytes != nullptr) bytes->clear();
    if (error != nullptr) error->clear();
    if (bytes == nullptr) {
        if (error != nullptr) *error = QStringLiteral("The metadata byte sink is unavailable.");
        return SharedMetadataReadStatus::Unreadable;
    }
    // `maxBytes + 1` is how the read proves the bound, so the bound cannot be
    // the largest qint64 without overflowing that expression.
    if (maxBytes < 0 || maxBytes == std::numeric_limits<qint64>::max()) {
        if (error != nullptr) *error = QStringLiteral("The metadata byte bound is invalid.");
        return SharedMetadataReadStatus::Unreadable;
    }
    QFile file(path);
    QByteArray data;
    bool readFailed = false;
    QString readError;
    {
#ifdef Q_OS_WIN
        const QReadLocker locker(&metadataPublicationLock());
#endif
        if (!file.open(QIODevice::ReadOnly)) {
            if (error != nullptr) *error = file.errorString();
            return SharedMetadataReadStatus::Unreadable;
        }
        // One byte past the bound distinguishes an at-bound file from an
        // over-bound one, and also catches a file that grows after any probe.
        data = file.read(maxBytes + 1);
        readFailed = file.error() != QFile::NoError;
        if (readFailed) readError = file.errorString();
        file.close();
    }
    if (readFailed) {
        if (error != nullptr) *error = readError;
        return SharedMetadataReadStatus::Unreadable;
    }
    if (data.size() > maxBytes) return SharedMetadataReadStatus::TooLarge;
    *bytes = data;
    return SharedMetadataReadStatus::Ok;
}

bool commitMetadataFile(QSaveFile& file, QString* error) {
    if (error != nullptr) error->clear();
#ifdef Q_OS_WIN
    const QWriteLocker locker(&metadataPublicationLock());
#endif
    if (file.commit()) return true;
    if (error != nullptr) *error = file.errorString();
    return false;
}

}  // namespace native
}  // namespace mplayer