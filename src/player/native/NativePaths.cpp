#include "NativePaths.hpp"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>

#ifdef Q_OS_WIN
#  include <Windows.h>
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

}  // namespace

QString safeRelativePath(const QString& raw, QString* reason) {
    const auto refuse = [reason](const QString& text) {
        if (reason) *reason = text;
        return QString{};
    };
    if (reason) reason->clear();
    if (raw.isEmpty()) return refuse(QStringLiteral("path is empty"));
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
    return cleanCandidate.startsWith(cleanRoot + QLatin1Char('/'), pathCaseSensitivity());
}

QStringList ancestorChain(const QString& root, const QString& path) {
    QStringList chain;
    const QString cleanRoot = QDir::cleanPath(root);
    QString current = QDir::cleanPath(path);
    if (cleanRoot.isEmpty() || current.isEmpty()) return chain;
    if (!pathWithinRoot(cleanRoot, current)) return chain;

    chain.append(current);
    const QString rootWithSlash = cleanRoot + QLatin1Char('/');
    while (current.compare(cleanRoot, pathCaseSensitivity()) != 0) {
        const qsizetype slash = current.lastIndexOf(QLatin1Char('/'));
        if (slash < 0 || slash + 1 <= cleanRoot.size()) break;
        if (!current.startsWith(rootWithSlash, pathCaseSensitivity())) break;
        current = current.left(slash);
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

QString nativeRoot() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("native"));
}

QStringList nativeSubdirectoryNames() {
    return {QStringLiteral("runtime"), QStringLiteral("models"),  QStringLiteral("assets"),
            QStringLiteral("artifacts"), QStringLiteral("recipes"), QStringLiteral("scratch"),
            QStringLiteral("logs")};
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

}  // namespace native
}  // namespace mplayer