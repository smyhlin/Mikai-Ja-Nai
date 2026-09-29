#include "Runtime.hpp"

#include <QCoreApplication>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

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

const QUrl& sevenZipUrl() {
    static const QUrl url(QStringLiteral("https://www.7-zip.org/a/7zr.exe"));
    return url;
}

QByteArray download(const QUrl& url, const QMap<QString, QString>& headers, int timeoutMs) {
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }
    QNetworkReply* reply = manager.get(request);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    if (reply->isRunning()) {
        reply->abort();
        loop.exec();
    }
    timer.stop();
    const QByteArray body = reply->readAll();
    const QString error =
        reply->error() == QNetworkReply::NoError ? QString{} : reply->errorString();
    reply->deleteLater();
    if (!error.isEmpty()) {
        throw std::runtime_error(
            QStringLiteral("GET %1 failed: %2").arg(url.toString(), error).toStdString());
    }
    return body;
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

QString findSevenZip() {
    const QString found = QStandardPaths::findExecutable(QStringLiteral("7z.exe"));
    if (!found.isEmpty()) {
        return found;
    }
    for (const QString& candidate : {QStringLiteral("C:/Program Files/7-Zip/7z.exe"),
                                     QStringLiteral("C:/Program Files (x86)/7-Zip/7z.exe")}) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    const QString cached = QDir::cleanPath(cacheDir().filePath(QStringLiteral("../tools/7zr.exe")));
    return QFileInfo::exists(cached) ? cached : QString{};
}

QString ensureSevenZip() {
    const QString found = findSevenZip();
    if (!found.isEmpty()) {
        return found;
    }
    const QString tool = QDir::cleanPath(cacheDir().filePath(QStringLiteral("../tools/7zr.exe")));
    QDir().mkpath(QFileInfo(tool).absolutePath());
    const QByteArray binary =
        download(sevenZipUrl(), {{QStringLiteral("User-Agent"), QStringLiteral("MPVBackend")}}, 60000);
    QFile file(tool);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error("could not write " + tool.toStdString());
    }
    file.write(binary);
    file.close();
    return tool;
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

std::optional<QDir> findRuntime(const QString& hint) {
    QVector<QDir> candidates;
    if (!hint.isEmpty()) {
        const QFileInfo info(hint);
        candidates.append(info.isDir() ? QDir(hint) : QDir(info.absolutePath()));
    }

    // Common bundled locations next to the executable.
    const QDir appRoot(QCoreApplication::applicationDirPath());
    candidates.append(QDir(appRoot.filePath(QStringLiteral("mpv"))));
    candidates.append(QDir(appRoot.filePath(QStringLiteral("lib"))));
    candidates.append(appRoot);

    // The runtime downloaded by an earlier run.
    candidates.append(cacheDir());

    for (const QDir& candidate : candidates) {
        if (const std::optional<QDir> found = folderWithLibrary(candidate)) {
            return found;
        }
    }
    return std::nullopt;
}

QDir ensureRuntime(const QString& hint, bool autoDownload) {
    if (const std::optional<QDir> found = findRuntime(hint)) {
        return *found;
    }
    if (!autoDownload) {
        throw std::runtime_error("libmpv was not found. Pass a hint path or enable auto download.");
    }
    if (QSysInfo::currentCpuArchitecture() != QLatin1String("x86_64")) {
        throw std::runtime_error(
            QStringLiteral("This automatic downloader supports Windows x64; detected %1.")
                .arg(QSysInfo::currentCpuArchitecture())
                .toStdString());
    }

    const QByteArray releases = download(
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
        file.write(download(QUrl(downloadUrl),
                            {{QStringLiteral("User-Agent"), QStringLiteral("MPVBackend")}}, 120000));
    }

    // The mpv dev archive uses the BCJ2 filter, so extraction needs the
    // official 7-Zip console rather than a plain archive reader.
    const QString sevenZip = ensureSevenZip();
    const QDir extracted = QDir(temp.filePath(QStringLiteral("extracted")));
    extracted.mkpath(QStringLiteral("."));
    QProcess process;
    process.start(sevenZip, {QStringLiteral("x"), QStringLiteral("-o%1").arg(extracted.absolutePath()),
                             QStringLiteral("-y"), archive});
    if (!process.waitForFinished(300000) || process.exitCode() != 0) {
        const QString tail = QString::fromUtf8(process.readAllStandardError()).right(500);
        throw std::runtime_error(QStringLiteral("7-Zip extraction failed (exit %1): %2")
                                     .arg(process.exitCode())
                                     .arg(tail)
                                     .toStdString());
    }

    if (!folderWithLibrary(extracted).has_value()) {
        throw std::runtime_error("The downloaded mpv archive did not contain a supported libmpv DLL.");
    }

    // Keep the full tree: libmpv resolves FFmpeg and friends next to it.
    const QDir target = cacheDir();
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
