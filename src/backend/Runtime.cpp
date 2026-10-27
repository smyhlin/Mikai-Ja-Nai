#include "Runtime.hpp"

#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QVector>

namespace mikai::backend {
namespace {

// Every library file under `folder`, one entry per match, in the name order
// libraryNames() declares and then lexicographically, so a caller enumerating
// candidates gets a stable order rather than the filesystem's.
QStringList librariesIn(const QDir& folder) {
    QStringList found;
    if (!folder.exists()) {
        return found;
    }
    for (const QString& name : libraryNames()) {
        QStringList matches;
        QDirIterator it(folder.absolutePath(), {name}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            matches.append(QDir::cleanPath(it.next()));
        }
        matches.sort(Qt::CaseInsensitive);
        found.append(matches);
    }
    return found;
}

// With no hint, only the packaged mpv directory is eligible. An explicit hint
// adds a developer/probe location before that package.
QVector<QDir> candidateDirs(const QString& hint) {
    QVector<QDir> candidates;
    if (!hint.isEmpty()) {
        const QFileInfo info(hint);
        candidates.append(info.isDir() ? QDir(hint) : QDir(info.absolutePath()));
    }
    candidates.append(
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("mpv")));
#ifndef Q_OS_WIN
    if (hint.isEmpty()) {
        for (const QString& sysDir : {QStringLiteral("/usr/lib/x86_64-linux-gnu"),
                                      QStringLiteral("/usr/lib64"),
                                      QStringLiteral("/usr/lib"),
                                      QStringLiteral("/usr/local/lib")}) {
            if (QDir(sysDir).exists()) {
                candidates.append(QDir(sysDir));
            }
        }
    }
#endif
    return candidates;
}

}  // namespace

QDir cacheDir() {
#ifdef Q_OS_WIN
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) {
        base = QDir::homePath();
    }
    return QDir(QDir(base).filePath(QStringLiteral("MPVBackend/runtime")));
#else
    // T-414: the non-Windows cache lives under the platform's cache location
    // instead of squatting directly in $HOME; the trailing MPVBackend/runtime
    // subfolder keeps the on-disk layout predictable for anything that
    // recognises that tree. A legacy ~/MPVBackend/runtime is migrated into
    // place once, when the new path does not exist yet - best-effort, because
    // a cache that refuses to move is only a stale copy, never a lost one.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString target = QDir(base).filePath(QStringLiteral("MPVBackend/runtime"));
    const QString legacy = QDir(QDir::homePath()).filePath(QStringLiteral("MPVBackend/runtime"));
    if (QFileInfo::exists(legacy) && !QFileInfo::exists(target)) {
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile::rename(legacy, target);
    }
    return QDir(target);
#endif
}

QStringList libraryNames() {
#ifdef Q_OS_WIN
    return {QStringLiteral("mpv-2.dll"), QStringLiteral("libmpv-2.dll"), QStringLiteral("mpv.dll")};
#else
    return {QStringLiteral("libmpv.so.2"), QStringLiteral("libmpv.so")};
#endif
}

QStringList runtimeCandidates(const QString& hint) {
    QStringList candidates;
    QSet<QString> seen;
    for (const QDir& dir : candidateDirs(hint)) {
        for (const QString& module : librariesIn(dir)) {
            const QString key = QDir::cleanPath(module).toLower();
            if (seen.contains(key)) {
                continue;
            }
            seen.insert(key);
            candidates.append(module);
        }
    }
    return candidates;
}

}  // namespace mikai::backend
