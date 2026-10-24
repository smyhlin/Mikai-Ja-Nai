#include "Downloads.hpp"

#include "Cancellation.hpp"
#include "Diagnostics.hpp"
#include "Runtime.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <functional>
#include <stdexcept>

namespace mikai::backend {
namespace {

/// How long a streaming download may go without a single byte before it is
/// treated as dead. Generous, because the assets are hundreds of megabytes and a
/// slow link can pause between chunks, but finite, because the alternative is a
/// thread that never comes back.
constexpr int kStallTimeoutMs = 60000;

/// How often an in-flight download asks whether it has been cancelled. Short
/// enough that a Cancel press feels immediate, long enough to be free.
constexpr int kCancelPollMs = 200;

/// How often the progress callback is allowed to fire. Per-chunk reporting
/// posts a GUI update for every 4 KB of a 400 MB asset, and the two surfaces
/// consuming it re-format and repaint on each one.
constexpr int kProgressIntervalMs = 100;

#ifdef Q_OS_WIN
const QUrl& sevenZipUrl() {
    static const QUrl url(QStringLiteral("https://github.com/ip7z/7zip/releases/download/26.03/7zr.exe"));
    return url;
}

const QUrl& sevenZipInstallerUrl() {
    static const QUrl url(QStringLiteral("https://github.com/ip7z/7zip/releases/download/26.03/7z2603-x64.exe"));
    return url;
}
#else
const QUrl& sevenZipLinuxUrl() {
    static const QUrl url(QStringLiteral("https://github.com/ip7z/7zip/releases/download/26.03/7z2603-linux-x64.tar.xz"));
    return url;
}
#endif

QString sha256(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString findSevenZip(const QString& toolsDirectory) {
    // An empty application tree yields a CWD-relative "7z.exe" probe; that
    // would let a planted binary in the working directory pass as trusted.
    if (toolsDirectory.isEmpty()) return {};
#ifdef Q_OS_WIN
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
    const QString bundled = QDir(toolsDirectory).filePath(QStringLiteral("7z.exe"));
    const QString library = QDir(toolsDirectory).filePath(QStringLiteral("7z.dll"));
    return QFileInfo(bundled).isFile() && QFileInfo(library).isFile() ? bundled : QString{};
#else
    for (const QString& name : {QStringLiteral("7zz"), QStringLiteral("7zzs"), QStringLiteral("7z")}) {
        const QString bundled = QDir(toolsDirectory).filePath(name);
        const QFileInfo info(bundled);
        if (info.isFile() && info.isExecutable()) {
            return bundled;
        }
    }
    for (const QString& name : {QStringLiteral("7zz"), QStringLiteral("7z"), QStringLiteral("7za")}) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty()) {
            return found;
        }
    }
    for (const QString& candidate : {QStringLiteral("/usr/bin/7zz"),
                                     QStringLiteral("/usr/bin/7z"),
                                     QStringLiteral("/usr/local/bin/7zz"),
                                     QStringLiteral("/usr/local/bin/7z")}) {
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable()) {
            return candidate;
        }
    }
    return {};
#endif
}

QNetworkAccessManager& managerForThread() {
    static thread_local QNetworkAccessManager* manager = [] {
        auto* created = new QNetworkAccessManager;
        if (QThread* thread = QThread::currentThread()) {
            QObject::connect(thread, &QThread::finished, created, &QObject::deleteLater);
        }
        return created;
    }();
    return *manager;
}

}  // namespace

QByteArray fetchUrlBytes(const QUrl& url, const QMap<QString, QString>& headers, int timeoutMs) {
    if (WorkCancellationScope::cancellationRequested()) {
        throw std::runtime_error("the request was cancelled");
    }
    QNetworkRequest request(url);
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }
    QNetworkReply* reply = managerForThread().get(request);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QTimer cancelTimer;
    cancelTimer.setInterval(kCancelPollMs);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&cancelTimer, &QTimer::timeout, &loop, [&]() {
        if (WorkCancellationScope::cancellationRequested()) {
            loop.quit();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    cancelTimer.start();
    loop.exec();
    cancelTimer.stop();
    if (reply->isRunning()) {
        reply->abort();
        // abort() can emit finished synchronously. Never enter a second loop
        // after that signal has already been delivered; a delayed abort still
        // has a finite drain deadline.
        if (reply->isRunning()) {
            timer.start(2000);
            loop.exec();
        }
    }
    timer.stop();
    const QByteArray body = reply->readAll();
    const QString error =
        reply->error() == QNetworkReply::NoError ? QString{} : reply->errorString();
    reply->deleteLater();
    if (WorkCancellationScope::cancellationRequested()) {
        throw std::runtime_error("the request was cancelled");
    }
    if (!error.isEmpty()) {
        // `url` may carry a signed path or query; it never reaches the message
        // as the transport built it.
        const QString message = redactDiagnostic(
            QStringLiteral("GET %1 failed: %2").arg(url.toString(), error));
        throw std::runtime_error(message.toStdString());
    }
    return body;
}

bool downloadToFile(const QUrl& url, const QString& destination, qint64 expectedBytes,
                    const QString& expectedSha256,
                    const std::function<void(qint64 received, qint64 total)>& progress,
                    const std::function<bool()>& cancelled, QString* reason) {
    bool created = false;
    // A refusal removes nothing. This invocation owns a file only once it has
    // opened it with exclusive creation, so that and nothing else is ever
    // deleted: a destination that already existed - or one that raced into place
    // between the existence check and the open - belongs to whoever put it
    // there and is reported untouched.
    auto refuse = [&](const QString& text) {
        if (reason) {
            *reason = text;
        }
        return false;
    };
    // `created` tracks this invocation's own partial. It is the only path this
    // call will ever remove, so a cancellation, a verification failure or a
    // storage error cannot destroy a neighbour's file.
    auto fail = [&](const QString& text) {
        if (created) {
            created = false;
            QFile::remove(destination);
        }
        if (reason) {
            *reason = text;
        }
        return false;
    };
    // Both quoted names are user-visible and loggable text, so both go through
    // the diagnostic scrubber: a signed asset URL carries its credentials in the
    // path or query, and quoting it whole would copy a token into a log or a
    // fault report.
    const QString namedDestination = redactDiagnostic(destination);
    const QString namedUrl = redactDiagnostic(url.toString());
    if (QFileInfo::exists(destination)) {
        return refuse(QStringLiteral("refusing to overwrite %1").arg(namedDestination));
    }
    // NewOnly is the exclusive-create flag: the open fails outright when the path
    // appeared after the check above, instead of truncating the file that just
    // arrived. WriteOnly | Truncate cannot make that promise, which is what let a
    // concurrent writer's data be destroyed by a caller that never touched it.
    QFile file(destination);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return refuse(QStringLiteral("could not write %1").arg(namedDestination));
    }
    created = true;

    // Support upstream ANIMEJANAI_PACKS_DIR local directory override and file:// URLs
    // while enforcing the exact same size, SHA-256, and cancellation contracts.
    QString localSourcePath;
    if (url.isLocalFile()) {
        localSourcePath = url.toLocalFile();
    } else {
        const QString packsDir = qEnvironmentVariable("ANIMEJANAI_PACKS_DIR").trimmed();
        if (!packsDir.isEmpty()) {
            const QString assetName = QFileInfo(url.path()).fileName();
            const QString candidate = QDir(packsDir).filePath(assetName);
            if (!assetName.isEmpty() && QFileInfo(candidate).isFile()) {
                localSourcePath = candidate;
            }
        }
    }
    if (!localSourcePath.isEmpty()) {
        QFile localIn(localSourcePath);
        if (!localIn.open(QIODevice::ReadOnly)) {
            file.close();
            return fail(QStringLiteral("could not read local pack %1").arg(redactDiagnostic(localSourcePath)));
        }
        QCryptographicHash localHash(QCryptographicHash::Sha256);
        const qint64 localTotal = localIn.size();
        qint64 localReceived = 0;
        QByteArray buffer(64 * 1024, Qt::Uninitialized);
        while (!localIn.atEnd()) {
            if ((cancelled && cancelled()) || WorkCancellationScope::cancellationRequested()) {
                file.close();
                return fail(QStringLiteral("cancelled while copying %1").arg(namedDestination));
            }
            const qint64 n = localIn.read(buffer.data(), buffer.size());
            if (n < 0 || file.write(buffer.constData(), n) != n) {
                file.close();
                return fail(QStringLiteral("could not write all of %1 to disk").arg(namedDestination));
            }
            localHash.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(n)));
            localReceived += n;
            if (progress) progress(localReceived, localTotal);
        }
        const bool flushed = file.flush();
        file.close();
        // A final progress publication can itself cancel the copy. There may
        // be no next chunk on which to observe that request.
        if ((cancelled && cancelled()) || WorkCancellationScope::cancellationRequested()) {
            return fail(QStringLiteral("cancelled while copying %1").arg(namedDestination));
        }
        if (!flushed || file.error() != QFileDevice::NoError) {
            return fail(QStringLiteral("could not write all of %1 to disk").arg(namedDestination));
        }
        if (expectedBytes > 0 && localReceived != expectedBytes) {
            return fail(QStringLiteral("size mismatch for %1: got %2, expected %3")
                            .arg(namedDestination).arg(localReceived).arg(expectedBytes));
        }
        if (expectedBytes <= 0 && localReceived <= 0) {
            return fail(QStringLiteral("size mismatch for %1: got %2, expected %3")
                            .arg(namedDestination).arg(localReceived).arg(localTotal));
        }
        if (QString::fromLatin1(localHash.result().toHex()).compare(expectedSha256, Qt::CaseInsensitive) != 0) {
            return fail(QStringLiteral("sha256 mismatch for %1").arg(namedDestination));
        }
        return true;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "MPVBackend");
    QNetworkReply* reply = manager.get(request);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 received = 0;
    qint64 total = 0;
    bool writeError = false;
    QElapsedTimer progressClock;
    progressClock.start();
    const auto maybeReport = [&] {
        if (!progress) {
            return;
        }
        // The terminal chunk always reports, so the final state lands even
        // when the interval has not elapsed yet.
        if (received != expectedBytes && progressClock.elapsed() < kProgressIntervalMs) {
            return;
        }
        progressClock.restart();
        progress(received, total);
    };
    const auto consume = [&] {
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty()) {
            return;
        }
        // Bytes count only when the write returned every one of them. A short
        // write and a storage failure look identical to the network layer, and
        // the digest below is computed over the chunk rather than over what the
        // file accepted - so crediting the chunk before it reached disk would
        // certify a file that is not there.
        const qint64 written = file.write(chunk);
        if (written == chunk.size()) {
            hash.addData(chunk);
            received += chunk.size();
        } else {
            writeError = true;
        }
        if (total == 0) {
            total = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        }
        maybeReport();
        if (writeError) {
            // Storage is gone; the stall watchdog has nothing left to guard
            // against. Drop the connection now rather than keep draining bytes
            // into a file that cannot take them.
            reply->abort();
        }
    };
    QEventLoop loop;
    // A stall watchdog, because a connection that is accepted and then goes quiet
    // produces neither readyRead nor finished: without this the loop below never
    // returns, the pool thread is pinned for the life of the process, and the
    // cancellation callback - which is only consulted when data arrives - can
    // never be observed either. Restarted on every chunk, so it bounds silence
    // rather than total duration, which is what a 200 MB asset needs.
    QTimer stall;
    stall.setSingleShot(true);
    bool stalled = false;
    QObject::connect(&stall, &QTimer::timeout, &loop, [&] {
        stalled = true;
        reply->abort();
    });
    stall.start(kStallTimeoutMs);

    // Cancellation is polled on its own clock rather than only when a chunk
    // arrives: a download that has gone quiet is exactly when a viewer presses
    // Cancel, and waiting for the stall watchdog would leave the button dead for
    // a minute. The shared work-cancellation state is consulted as well, so a
    // cancel that arrived through the Runner's pools - a shutdown, a page that
    // tore itself down - is answered here without the caller having to rewire its
    // predicate to see it.
    QTimer cancelPoll;
    cancelPoll.setInterval(kCancelPollMs);
    const auto cancellationPending = [&] {
        return (cancelled && cancelled()) || WorkCancellationScope::cancellationRequested();
    };
    QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&] {
        if (cancellationPending()) {
            reply->abort();
        }
    });
    cancelPoll.start();

    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        stall.start(kStallTimeoutMs);
        consume();
        if (cancellationPending()) {
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, [&] {
        consume();
        loop.quit();
    });
    loop.exec();
    stall.stop();
    cancelPoll.stop();
    // The loop has ended but the file must be settled too: flush pushes the last
    // buffer out and close reports any deferred failure. Neither was inspected
    // before, so a full disk could produce a completed download that no later
    // reader could open.
    const bool flushed = file.flush();
    file.close();
    const bool storageFinished =
        flushed && !writeError && file.error() == QFileDevice::NoError;
    const bool wasCancelled = cancellationPending();
    const QString error =
        reply->error() == QNetworkReply::NoError ? QString{} : reply->errorString();
    reply->deleteLater();
    if (stalled) {
        return fail(QStringLiteral("%1 sent nothing for %2 seconds")
                        .arg(namedUrl)
                        .arg(kStallTimeoutMs / 1000));
    }
    if (wasCancelled) {
        return fail(QStringLiteral("cancelled while downloading %1").arg(namedUrl));
    }
    if (!error.isEmpty()) {
        return fail(QStringLiteral("GET %1 failed: %2").arg(namedUrl, error));
    }
    if (!storageFinished) {
        return fail(QStringLiteral("could not write all of %1 to disk").arg(namedDestination));
    }
    if (expectedBytes > 0 && received != expectedBytes) {
        return fail(QStringLiteral("size mismatch for %1: got %2, expected %3")
                        .arg(namedDestination)
                        .arg(received)
                        .arg(expectedBytes));
    }
    if (expectedBytes <= 0 && (received <= 0 || (total > 0 && received != total))) {
        return fail(QStringLiteral("size mismatch for %1: got %2, expected %3")
                        .arg(namedDestination)
                        .arg(received)
                        .arg(total));
    }
    if (QString::fromLatin1(hash.result().toHex()).compare(expectedSha256, Qt::CaseInsensitive) !=
        0) {
        return fail(QStringLiteral("sha256 mismatch for %1").arg(namedDestination));
    }
    if (progress) {
        progress(received, total);
    }
    return true;
}

QString ensureSevenZip(const QString& toolsDirectory) {
    if (toolsDirectory.isEmpty()) return {};
    static QMutex bootstrapMutex;
    const QMutexLocker locker(&bootstrapMutex);
    const QString found = findSevenZip(toolsDirectory);
    if (!found.isEmpty()) {
        return found;
    }

    const QString tools = toolsDirectory;
    if (!QDir().mkpath(tools)) {
        throw std::runtime_error("could not create " + tools.toStdString());
    }
    QTemporaryDir staging(QDir(tools).filePath(QStringLiteral(".7zip-bootstrap-XXXXXX")));
    if (!staging.isValid()) {
        throw std::runtime_error("could not create a temporary 7-Zip bootstrap directory");
    }

    auto writeVerified = [](const QString& path, const QUrl& url, const char* digest) {
        const QByteArray bytes =
            fetchUrlBytes(url, {{QStringLiteral("User-Agent"), QStringLiteral("MPVBackend")}},
                          60000);
        if (sha256(bytes) != QLatin1String(digest)) {
            throw std::runtime_error("official 7-Zip download SHA-256 mismatch");
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            file.write(bytes) != bytes.size() || !file.flush()) {
            throw std::runtime_error("could not write " + path.toStdString());
        }
    };

#ifdef Q_OS_WIN
    // 7zr can unpack the official self-extracting installer, but cannot handle
    // ZIP/NuGet archives used by runtime provisioning. Verify both official
    // artifacts before executing/extracting anything from them.
    constexpr auto bootstrapSha = "ad4c82fadcbdf93c03b4fc440f300509c7d60c5c2f4d183e35d9d70d6957037d";
    constexpr auto installerSha = "0859c524b8a63551848f0c246abddcb1d0b7b656b0fbfe879f8d85e61a9e6edd";
    const QString bootstrap = QDir(staging.path()).filePath(QStringLiteral("7zr.exe"));
    const QString installer = QDir(staging.path()).filePath(QStringLiteral("7z2603-x64.exe"));

    writeVerified(bootstrap, sevenZipUrl(), bootstrapSha);
    writeVerified(installer, sevenZipInstallerUrl(), installerSha);
    QProcess extract;
    extract.start(bootstrap, {QStringLiteral("x"), QStringLiteral("-o%1").arg(tools),
                              QStringLiteral("-y"), installer});
    if (!extract.waitForStarted() || !extract.waitForFinished(60000) ||
        extract.exitStatus() != QProcess::NormalExit || extract.exitCode() != 0) {
        const QString details = QString::fromLocal8Bit(extract.readAllStandardError()).right(500);
        throw std::runtime_error(
            QStringLiteral("could not extract the verified x64 7-Zip package: %1")
                .arg(details).toStdString());
    }
    const QString tool = QDir(tools).filePath(QStringLiteral("7z.exe"));
    if (!QFileInfo(tool).isFile() ||
        !QFileInfo(QDir(tools).filePath(QStringLiteral("7z.dll"))).isFile()) {
        throw std::runtime_error("verified x64 7-Zip package did not contain 7z.exe and 7z.dll");
    }
    QFile::remove(bootstrap);
    QFile::remove(installer);
    staging.remove();
    return tool;
#else
    constexpr auto linuxTarSha = "dc99eff5008f1ab79bd7084c68513701547a808a89502bf4133683535ab3c695";
    const QString archive = QDir(staging.path()).filePath(QStringLiteral("7z2603-linux-x64.tar.xz"));
    writeVerified(archive, sevenZipLinuxUrl(), linuxTarSha);
    QProcess extract;
    extract.start(QStringLiteral("tar"),
                  {QStringLiteral("-xJf"), archive, QStringLiteral("-C"), tools});
    if (!extract.waitForStarted(5000) || !extract.waitForFinished(60000) ||
        extract.exitStatus() != QProcess::NormalExit || extract.exitCode() != 0) {
        const QString details = QString::fromLocal8Bit(extract.readAllStandardError()).right(500);
        throw std::runtime_error(
            QStringLiteral("could not extract the verified Linux x64 7-Zip package: %1")
                .arg(details).toStdString());
    }
    for (const QString& candidateName : {QStringLiteral("7zz"), QStringLiteral("7zzs")}) {
        const QString tool = QDir(tools).filePath(candidateName);
        if (QFileInfo::exists(tool)) {
            QFile::setPermissions(tool, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                        QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                        QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                        QFileDevice::ExeOther);
            if (QFileInfo(tool).isFile() && QFileInfo(tool).isExecutable()) {
                QFile::remove(archive);
                staging.remove();
                return tool;
            }
        }
    }
    throw std::runtime_error("verified Linux x64 7-Zip package did not contain 7zz or 7zzs");
#endif
}

bool extractArchive(const QString& sevenZip, const QString& archive, const QString& destination,
                    QString* reason) {
#ifndef Q_OS_WIN
    if (archive.endsWith(QLatin1String(".tar.zst"), Qt::CaseInsensitive)) {
        QProcess tarProcess;
        tarProcess.start(QStringLiteral("tar"),
                         {QStringLiteral("--zstd"), QStringLiteral("-xf"), archive,
                          QStringLiteral("-C"), QDir(destination).absolutePath()});
        if (tarProcess.waitForStarted(3000) && tarProcess.waitForFinished(300000) &&
            tarProcess.exitStatus() == QProcess::NormalExit && tarProcess.exitCode() == 0) {
            return true;
        }
    }
#endif
    auto runSevenZip = [&](const QString& inputArchive, const QString& outDir) -> bool {
        QProcess process;
        QStringList args{QStringLiteral("x"),
                         QStringLiteral("-o%1").arg(QDir(outDir).absolutePath())};
#ifndef Q_OS_WIN
        // Restore symlinks as links on POSIX (matches AnimeJaNaiUpdater Extract -snl).
        args << QStringLiteral("-snl");
#endif
        args << QStringLiteral("-y") << inputArchive;
        process.start(sevenZip, args);
        if (!process.waitForStarted(5000)) {
            if (reason) {
                *reason = QStringLiteral("7-Zip failed to start: %1").arg(process.errorString());
            }
            return false;
        }
        if (!process.waitForFinished(300000)) {
            process.kill();
            process.waitForFinished(2000);
            if (reason) {
                *reason = QStringLiteral("7-Zip extraction timed out for %1").arg(inputArchive);
            }
            return false;
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            const QString tail = QString::fromUtf8(process.readAllStandardError()).right(500);
            if (reason) {
                *reason = QStringLiteral("7-Zip extraction failed (exit %1): %2")
                              .arg(process.exitCode())
                              .arg(tail);
            }
            return false;
        }
        return true;
    };

    if (!runSevenZip(archive, destination)) {
        return false;
    }

    // Compound tarballs (e.g. aji-linux-x64.tar.zst) unpack to an intermediate
    // .tar archive on the first 7-Zip pass; unpack the inner .tar automatically.
    const QString lower = archive.toLower();
    if (lower.endsWith(QLatin1String(".tar.zst")) ||
        lower.endsWith(QLatin1String(".tar.xz")) ||
        lower.endsWith(QLatin1String(".tar.gz")) ||
        lower.endsWith(QLatin1String(".tgz"))) {
        const QStringList entries =
            QDir(destination).entryList(QStringList{QStringLiteral("*.tar")},
                                        QDir::Files | QDir::NoSymLinks);
        if (entries.size() == 1) {
            const QString innerTar = QDir(destination).filePath(entries.first());
            if (!runSevenZip(innerTar, destination)) {
                QFile::remove(innerTar);
                return false;
            }
            QFile::remove(innerTar);
        }
    }
    return true;
}

}  // namespace mikai::backend
