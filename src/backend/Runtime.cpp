#include "Runtime.hpp"

#include "Downloads.hpp"

#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QUrl>
#include <QVector>

#include <stdexcept>

#ifndef Q_OS_WIN
#error "The mpv runtime discovery in this port targets Windows."
#endif

namespace mikai::backend {
namespace {

// The latest x64 mpv development archive from the Windows build project's
// GitHub Releases API. The release is resolved at runtime, not hard-coded.
const QUrl& releasesApi() {
    static const QUrl url(
        QStringLiteral("https://api.github.com/repos/shinchiro/mpv-winbuild-cmake/releases/latest"));
    return url;
}

// libmpv ships with FFmpeg and other DLLs beside it, so the extracted tree
// has to stay intact; only the directory holding the library is reported.
std::optional<QDir> folderWithLibrary(const QDir& folder) {
    if (!folder.exists()) {
        return std::nullopt;
    }
    for (const QString& name : libraryNames()) {
        QDirIterator it(folder.absolutePath(), {name}, QDir::Files,
                        QDirIterator::Subdirectories);
        if (it.hasNext()) {
            return QDir(it.next());
        }
    }
    return std::nullopt;
}

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

// The directories the stock search walks, in its order: the caller's hint
// first, then the bundled locations beside the executable, then the runtime a
// previous run downloaded.
QVector<QDir> candidateDirs(const QString& hint) {
    QVector<QDir> candidates;
    if (!hint.isEmpty()) {
        const QFileInfo info(hint);
        candidates.append(info.isDir() ? QDir(hint) : QDir(info.absolutePath()));
    }
    const QDir appRoot(QCoreApplication::applicationDirPath());
    candidates.append(QDir(appRoot.filePath(QStringLiteral("mpv"))));
    candidates.append(QDir(appRoot.filePath(QStringLiteral("lib"))));
    candidates.append(appRoot);
    candidates.append(cacheDir());
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

QDir provisionRuntime(const QString& destination) {
    if (destination.trimmed().isEmpty()) {
        throw std::runtime_error("no destination was given for the mpv runtime");
    }
    if (QSysInfo::currentCpuArchitecture() != QLatin1String("x86_64")) {
        throw std::runtime_error(
            QStringLiteral("This automatic downloader supports Windows x64; detected %1.")
                .arg(QSysInfo::currentCpuArchitecture())
                .toStdString());
    }

    const QByteArray releases = fetchUrlBytes(
        releasesApi(),
        {{QStringLiteral("Accept"), QStringLiteral("application/vnd.github+json")},
         {QStringLiteral("User-Agent"), QStringLiteral("MPVBackend")}},
        30000);
    const QJsonArray assets =
        QJsonDocument::fromJson(releases).object().value(QStringLiteral("assets")).toArray();

    QString assetName;
    QString downloadUrl;
    for (const QJsonValue& value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        if (name.startsWith(QStringLiteral("mpv-dev-x86_64-")) &&
            name.endsWith(QStringLiteral(".7z"))) {
            assetName = name;
            downloadUrl = asset.value(QStringLiteral("browser_download_url")).toString();
            break;
        }
    }
    if (assetName.isEmpty()) {
        throw std::runtime_error(
            "Could not find an x64 mpv development archive in the latest release.");
    }

    const QDir temp = QDir(QDir(QDir::tempPath())
                               .filePath(QStringLiteral("mpv-download-%1")
                                             .arg(QCoreApplication::applicationPid())));
    QDir().mkpath(temp.absolutePath());
    const QString archive = temp.filePath(assetName);
    {
        QFile file(archive);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            throw std::runtime_error("could not write " + archive.toStdString());
        }
        file.write(fetchUrlBytes(QUrl(downloadUrl),
                                 {{QStringLiteral("User-Agent"), QStringLiteral("MPVBackend")}},
                                 120000));
    }

    // The mpv dev archive uses the BCJ2 filter, so extraction needs the
    // official 7-Zip console rather than a plain archive reader.
    const QString sevenZip = ensureSevenZip();
    const QDir extracted = QDir(temp.filePath(QStringLiteral("extracted")));
    extracted.mkpath(QStringLiteral("."));
    QString extractReason;
    if (!extractArchive(sevenZip, archive, extracted.absolutePath(), &extractReason)) {
        throw std::runtime_error(extractReason.toStdString());
    }

    if (!folderWithLibrary(extracted).has_value()) {
        throw std::runtime_error("The downloaded mpv archive did not contain a supported libmpv DLL.");
    }

    // Keep the full tree: libmpv resolves FFmpeg and friends next to it. The
    // destination is replaced atomically through a sibling staging directory,
    // so a failure never leaves a half-installed tree behind the caller.
    const QDir target(destination);
    QDir().mkpath(target.absolutePath());
    const QDir staging = QDir(target.absolutePath() + QStringLiteral(".new"));
    QDir(staging.absolutePath()).removeRecursively();
    if (!QDir().mkpath(staging.absolutePath())) {
        throw std::runtime_error("could not stage " + staging.absolutePath().toStdString());
    }
    QDirIterator files(extracted.absolutePath(), QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        const QString relative = QDir(extracted.absolutePath()).relativeFilePath(path);
        if (QFileInfo(path).isDir()) {
            QDir().mkpath(staging.filePath(relative));
            continue;
        }
        QDir().mkpath(QFileInfo(staging.filePath(relative)).absolutePath());
        QFile::remove(staging.filePath(relative));
        if (!QFile::copy(path, staging.filePath(relative))) {
            throw std::runtime_error("could not copy " + path.toStdString());
        }
    }

    QDir(target.absolutePath()).removeRecursively();
    if (!QDir().rename(staging.absolutePath(), target.absolutePath())) {
        throw std::runtime_error(
            "could not install the mpv runtime into " + target.absolutePath().toStdString());
    }
    QDir(temp.absolutePath()).removeRecursively();

    if (const std::optional<QDir> installed = folderWithLibrary(target)) {
        return *installed;
    }
    throw std::runtime_error("Extracted mpv runtime is missing its libmpv DLL.");
}

}  // namespace mikai::backend
