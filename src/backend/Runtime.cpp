#include "Runtime.hpp"

#include <QCoreApplication>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>
#include <QVector>

#ifndef Q_OS_WIN
#error "The mpv runtime discovery in this port targets Windows."
#endif

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
    return candidates;
}

}  // namespace

QDir cacheDir() {
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) {
        base = QDir::homePath();
    }
    return QDir(QDir(base).filePath(QStringLiteral("MPVBackend/runtime")));
}

QStringList libraryNames() {
    return {QStringLiteral("mpv-2.dll"), QStringLiteral("libmpv-2.dll"), QStringLiteral("mpv.dll")};
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
