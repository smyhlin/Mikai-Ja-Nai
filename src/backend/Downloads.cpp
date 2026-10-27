#include "Downloads.hpp"

#include "Cancellation.hpp"
#include "Diagnostics.hpp"
#include "HttpTransport.hpp"
#include "Runtime.hpp"

#include <QByteArrayView>
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
#include <QTemporaryFile>
#include <QTimer>
#include <QWaitCondition>

#include <functional>
#include <stdexcept>

namespace mikai::backend {
namespace {

/// Silence limit for a streaming response.
constexpr int kStallTimeoutMs = 60000;

/// Cancellation polling interval.
constexpr int kCancelPollMs = 200;

/// Minimum interval between progress updates.
constexpr int kProgressIntervalMs = 100;
constexpr qsizetype kDownloadChunkBytes = 64 * 1024;

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
    // Prefer the bundled, verified executable over PATH.
    const QString bundled = QDir(toolsDirectory).filePath(QStringLiteral("7z.exe"));
    const QString library = QDir(toolsDirectory).filePath(QStringLiteral("7z.dll"));
    if (QFileInfo(bundled).isFile() && QFileInfo(library).isFile()) {
        return bundled;
    }
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
    return {};
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

void throwIfBootstrapCancelled() {
    if (WorkCancellationScope::cancellationRequested()) {
        throw std::runtime_error("the request was cancelled");
    }
}

void stopBootstrapProcess(QProcess& process) {
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(1000);
    }
}

bool waitForBootstrapStart(QProcess& process, int timeoutMs) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (process.state() == QProcess::Starting) {
        if (WorkCancellationScope::cancellationRequested()) {
            stopBootstrapProcess(process);
            throw std::runtime_error("the request was cancelled");
        }
        const int remaining = timeoutMs - static_cast<int>(elapsed.elapsed());
        if (remaining <= 0) {
            stopBootstrapProcess(process);
            return false;
        }
        process.waitForStarted(qMin(kCancelPollMs, remaining));
    }
    if (WorkCancellationScope::cancellationRequested()) {
        stopBootstrapProcess(process);
        throw std::runtime_error("the request was cancelled");
    }
    return process.state() == QProcess::Running;
}

bool waitForBootstrapFinish(QProcess& process, int timeoutMs) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (process.state() != QProcess::NotRunning) {
        if (WorkCancellationScope::cancellationRequested()) {
            stopBootstrapProcess(process);
            throw std::runtime_error("the request was cancelled");
        }
        const int remaining = timeoutMs - static_cast<int>(elapsed.elapsed());
        if (remaining <= 0) {
            stopBootstrapProcess(process);
            return false;
        }
        process.waitForFinished(qMin(kCancelPollMs, remaining));
    }
    throwIfBootstrapCancelled();
    return true;
}

}  // namespace

QByteArray fetchUrlBytes(const QUrl& url, const QMap<QString, QString>& headers, int timeoutMs) {
    // Cancellation takes precedence over transport and status errors.
    QNetworkRequest request(url);
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }
    const HttpFetchResult result =
        runBlockingFetch(transportManagerForThread(), request, HttpFetchOptions{timeoutMs, false, {}});
    if (result.cancelled || WorkCancellationScope::cancellationRequested()) {
        throw std::runtime_error("the request was cancelled");
    }
    if (result.timedOut) {
        const QString message = redactDiagnostic(
            QStringLiteral("GET %1 timed out after %2 ms").arg(url.toString()).arg(timeoutMs));
        throw std::runtime_error(message.toStdString());
    }
    if (result.status != 0 && (result.status < 200 || result.status >= 300)) {
        const QString message = redactDiagnostic(
            QStringLiteral("GET %1 returned HTTP status %2").arg(url.toString()).arg(result.status));
        throw std::runtime_error(message.toStdString());
    }
    if (!result.networkError.isEmpty()) {
        const QString message = redactDiagnostic(
            QStringLiteral("GET %1 failed: %2").arg(url.toString(), result.networkError));
        throw std::runtime_error(message.toStdString());
    }
    return result.body;
}

bool downloadToFile(const QUrl& url, const QString& destination, qint64 expectedBytes,
                    const QString& expectedSha256,
                    const std::function<void(qint64 received, qint64 total)>& progress,
                    const std::function<bool()>& cancelled, QString* reason) {
    // Only this call's private partial is removed on failure.
    auto refuse = [&](const QString& text) {
        if (reason) {
            *reason = text;
        }
        return false;
    };
    // The private partial is a sibling of the destination and is the only path
    // failure cleanup removes.
    QString ownedPartial;
    auto fail = [&](const QString& text) {
        if (!ownedPartial.isEmpty()) {
            QFile::remove(ownedPartial);
            ownedPartial.clear();
        }
        if (reason) {
            *reason = text;
        }
        return false;
    };
    // Redact paths and signed URLs before composing caller-visible errors.
    const QString namedDestination = redactDiagnostic(destination);
    const QString namedUrl = redactDiagnostic(url.toString());
    if (QFileInfo::exists(destination)) {
        return refuse(QStringLiteral("refusing to overwrite %1").arg(namedDestination));
    }
    // Publish only a fully written, size- and digest-verified sibling partial.
    const QFileInfo destinationInfo(destination);
    QTemporaryFile file(QDir(destinationInfo.absolutePath())
                            .filePath(destinationInfo.fileName() +
                                      QStringLiteral(".part-XXXXXX")));
    if (!file.open()) {
        return refuse(QStringLiteral("could not write %1").arg(namedDestination));
    }
    ownedPartial = file.fileName();
    // Refuse a destination that appeared while the partial was being written.
    auto publish = [&]() {
        if (QFileInfo::exists(destination)) {
            return fail(QStringLiteral("refusing to overwrite %1").arg(namedDestination));
        }
        if (!file.rename(destination)) {
            return fail(QStringLiteral("could not finalize %1: %2")
                            .arg(namedDestination, file.errorString()));
        }
        file.setAutoRemove(false);
        ownedPartial.clear();
        return true;
    };

    // Local sources use the same verification and cancellation path.
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
        QByteArray buffer(kDownloadChunkBytes, Qt::Uninitialized);
        QElapsedTimer localProgressClock;
        localProgressClock.start();
        const auto reportLocalProgress = [&](bool terminal) {
            if (progress && (terminal || localProgressClock.elapsed() >= kProgressIntervalMs)) {
                localProgressClock.restart();
                progress(localReceived, localTotal);
            }
        };
        while (!localIn.atEnd()) {
            if ((cancelled && cancelled()) || WorkCancellationScope::cancellationRequested()) {
                file.close();
                reportLocalProgress(true);
                return fail(QStringLiteral("cancelled while copying %1").arg(namedDestination));
            }
            const qint64 n = localIn.read(buffer.data(), buffer.size());
            if (n < 0 || file.write(buffer.constData(), n) != n) {
                file.close();
                reportLocalProgress(true);
                return fail(QStringLiteral("could not write all of %1 to disk").arg(namedDestination));
            }
            localHash.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(n)));
            localReceived += n;
            reportLocalProgress(false);
        }
        const bool flushed = file.flush();
        file.close();
        reportLocalProgress(true);
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
        return publish();
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "MPVBackend");
    QNetworkReply* reply = manager.get(request);
    QByteArray buffer(kDownloadChunkBytes, Qt::Uninitialized);
    reply->setReadBufferSize(buffer.size());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 received = 0;
    qint64 total = 0;
    bool writeError = false;
    QElapsedTimer progressClock;
    progressClock.start();
    const auto maybeReport = [&] {
        if (!progress || progressClock.elapsed() < kProgressIntervalMs) {
            return;
        }
        progressClock.restart();
        progress(received, total);
    };
    const auto consume = [&] {
        while (!writeError && reply->bytesAvailable() > 0) {
            const qint64 count = reply->read(buffer.data(), buffer.size());
            if (count <= 0) {
                break;
            }
            const qint64 written = file.write(buffer.constData(), count);
            if (written == count) {
                hash.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(count)));
                received += count;
            } else {
                writeError = true;
            }
            if (total == 0) {
                total = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
            }
            maybeReport();
        }
        if (writeError) {
            reply->abort();
        }
    };
    QEventLoop loop;
    // Bound silence rather than total download time; restart on each data event.
    QTimer stall;
    stall.setSingleShot(true);
    bool stalled = false;
    QObject::connect(&stall, &QTimer::timeout, &loop, [&] {
        stalled = true;
        reply->abort();
    });
    stall.start(kStallTimeoutMs);

    // Poll independently of data so a quiet reply remains cancellable.
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
    // Check deferred storage errors before publishing the file.
    const bool flushed = file.flush();
    file.close();
    const bool storageFinished =
        flushed && !writeError && file.error() == QFileDevice::NoError;
    if (progress) {
        progress(received, total);
    }
    const bool wasCancelled = cancellationPending();
    const QString error =
        reply->error() == QNetworkReply::NoError ? QString{} : reply->errorString();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    if (wasCancelled) {
        return fail(QStringLiteral("cancelled while downloading %1").arg(namedUrl));
    }
    if (stalled) {
        return fail(QStringLiteral("%1 sent nothing for %2 seconds")
                        .arg(namedUrl)
                        .arg(kStallTimeoutMs / 1000));
    }
    if (status != 0 && (status < 200 || status >= 300)) {
        return fail(QStringLiteral("GET %1 returned HTTP status %2").arg(namedUrl).arg(status));
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
    return publish();
}

QString ensureSevenZip(const QString& toolsDirectory) {
    throwIfBootstrapCancelled();
    if (toolsDirectory.isEmpty()) return {};
    // Return an existing install before waiting for another bootstrap.
    const QString present = findSevenZip(toolsDirectory);
    if (!present.isEmpty()) {
        throwIfBootstrapCancelled();
        return present;
    }

    const QString tools = toolsDirectory;
    if (!QDir().mkpath(tools)) {
        throw std::runtime_error("could not create " + tools.toStdString());
    }

    // Reserve a single bootstrap; network and extraction work stay outside the lock.
    static QMutex bootstrapMutex;
    static QWaitCondition bootstrapSettled;
    static bool bootstrapInFlight = false;
    QMutexLocker locker(&bootstrapMutex);
    while (true) {
        throwIfBootstrapCancelled();
        const QString installed = findSevenZip(tools);
        if (!installed.isEmpty()) {
            return installed;
        }
        if (bootstrapInFlight) {
            bootstrapSettled.wait(&bootstrapMutex, kCancelPollMs);
            throwIfBootstrapCancelled();
            continue;
        }
        throwIfBootstrapCancelled();
        bootstrapInFlight = true;
        locker.unlock();

        QTemporaryDir staging(QDir(tools).filePath(QStringLiteral(".7zip-bootstrap-XXXXXX")));
        QString failure;
        if (!staging.isValid()) {
            failure = QStringLiteral("could not create a temporary 7-Zip bootstrap directory");
        } else {
            try {
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
                const QString installer =
                    QDir(staging.path()).filePath(QStringLiteral("7z2603-x64.exe"));
                // Extract into staging; publish only after verification.
                const QString staged =
                    QDir(staging.path()).filePath(QStringLiteral("staged"));

                writeVerified(bootstrap, sevenZipUrl(), bootstrapSha);
                writeVerified(installer, sevenZipInstallerUrl(), installerSha);
                throwIfBootstrapCancelled();
                QProcess extract;
                extract.start(bootstrap, {QStringLiteral("x"), QStringLiteral("-o%1").arg(staged),
                                          QStringLiteral("-y"), installer});
                if (!waitForBootstrapStart(extract, 5000) ||
                    !waitForBootstrapFinish(extract, 60000) ||
                    extract.exitStatus() != QProcess::NormalExit || extract.exitCode() != 0) {
                    const QString details =
                        QString::fromLocal8Bit(extract.readAllStandardError()).right(500);
                    throw std::runtime_error(
                        QStringLiteral("could not extract the verified x64 7-Zip package: %1")
                            .arg(details).toStdString());
                }
#else
                constexpr auto linuxTarSha = "dc99eff5008f1ab79bd7084c68513701547a808a89502bf4133683535ab3c695";
                const QString archive =
                    QDir(staging.path()).filePath(QStringLiteral("7z2603-linux-x64.tar.xz"));
                const QString staged =
                    QDir(staging.path()).filePath(QStringLiteral("staged"));
                if (!QDir().mkpath(staged)) {
                    throw std::runtime_error(
                        "could not create a staging directory for the 7-Zip bootstrap");
                }
                writeVerified(archive, sevenZipLinuxUrl(), linuxTarSha);
                throwIfBootstrapCancelled();
                QProcess extract;
                extract.start(QStringLiteral("tar"),
                              {QStringLiteral("-xJf"), archive, QStringLiteral("-C"), staged});
                if (!waitForBootstrapStart(extract, 5000) ||
                    !waitForBootstrapFinish(extract, 60000) ||
                    extract.exitStatus() != QProcess::NormalExit || extract.exitCode() != 0) {
                    const QString details =
                        QString::fromLocal8Bit(extract.readAllStandardError()).right(500);
                    throw std::runtime_error(
                        QStringLiteral("could not extract the verified Linux x64 7-Zip package: %1")
                            .arg(details).toStdString());
                }
#endif
            } catch (const std::exception& error) {
                failure = QString::fromUtf8(error.what());
            }
        }

        locker.relock();
        bootstrapInFlight = false;
        bootstrapSettled.wakeAll();
        if (failure.isEmpty() && WorkCancellationScope::cancellationRequested()) {
            failure = QStringLiteral("the request was cancelled");
        }
        if (!failure.isEmpty()) {
            throw std::runtime_error(failure.toStdString());
        }

#ifdef Q_OS_WIN
        // Publish the verified pair while holding the bootstrap reservation.
        const QString staged = QDir(staging.path()).filePath(QStringLiteral("staged"));
        const QString tool = QDir(tools).filePath(QStringLiteral("7z.exe"));
        const QString library = QDir(tools).filePath(QStringLiteral("7z.dll"));
        if (!QFile::rename(QDir(staged).filePath(QStringLiteral("7z.exe")), tool) ||
            !QFile::rename(QDir(staged).filePath(QStringLiteral("7z.dll")), library)) {
            QFile::remove(tool);
            QFile::remove(library);
            throw std::runtime_error("could not publish the verified x64 7-Zip package");
        }
        if (!QFileInfo(tool).isFile() || !QFileInfo(library).isFile()) {
            throw std::runtime_error("verified x64 7-Zip package did not contain 7z.exe and 7z.dll");
        }
        QFile::remove(QDir(staging.path()).filePath(QStringLiteral("7zr.exe")));
        QFile::remove(QDir(staging.path()).filePath(QStringLiteral("7z2603-x64.exe")));
        staging.remove();
        return tool;
#else
        const QString staged = QDir(staging.path()).filePath(QStringLiteral("staged"));
        for (const QString& candidateName : {QStringLiteral("7zz"), QStringLiteral("7zzs")}) {
            const QString stagedTool = QDir(staged).filePath(candidateName);
            if (!QFileInfo::exists(stagedTool)) {
                continue;
            }
            // The executable bit is granted before the rename so a reader can
            // never observe a bundled binary findSevenZip would refuse.
            QFile::setPermissions(stagedTool, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                                  QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                                  QFileDevice::ExeOther);
            if (!QFileInfo(stagedTool).isFile() || !QFileInfo(stagedTool).isExecutable()) {
                throw std::runtime_error("verified Linux x64 7-Zip package did not contain 7zz or 7zzs");
            }
            const QString tool = QDir(tools).filePath(candidateName);
            if (!QFile::rename(stagedTool, tool)) {
                QFile::remove(tool);
                throw std::runtime_error("could not publish the verified Linux x64 7-Zip package");
            }
            QFile::remove(QDir(staging.path()).filePath(
                QStringLiteral("7z2603-linux-x64.tar.xz")));
            staging.remove();
            return tool;
        }
        throw std::runtime_error("verified Linux x64 7-Zip package did not contain 7zz or 7zzs");
#endif
    }
}

bool extractArchive(const QString& sevenZip, const QString& archive, const QString& destination,
                    QString* reason) {
    enum class ProcessResult { Finished, StartFailed, TimedOut, Cancelled };
    auto cancelled = [&] {
        if (!WorkCancellationScope::cancellationRequested()) return false;
        if (reason) *reason = QStringLiteral("archive extraction was cancelled");
        return true;
    };
    // Both archive tools are owned by this call. Never fall back to another
    // tool after cancellation, and reap the current one before returning.
    auto waitForExtraction = [&](QProcess& process, int startTimeoutMs) {
        QElapsedTimer elapsed;
        elapsed.start();
        bool started = false;
        while (process.state() == QProcess::Starting) {
            if (cancelled()) {
                stopBootstrapProcess(process);
                return ProcessResult::Cancelled;
            }
            const int remaining = startTimeoutMs - static_cast<int>(elapsed.elapsed());
            if (remaining <= 0) {
                stopBootstrapProcess(process);
                return ProcessResult::StartFailed;
            }
            if (process.waitForStarted(qMin(kCancelPollMs, remaining))) {
                started = true;
                break;
            }
        }
        if (cancelled()) {
            stopBootstrapProcess(process);
            return ProcessResult::Cancelled;
        }
        if (!started && process.state() != QProcess::Running) return ProcessResult::StartFailed;

        elapsed.restart();
        while (process.state() != QProcess::NotRunning) {
            if (cancelled()) {
                stopBootstrapProcess(process);
                return ProcessResult::Cancelled;
            }
            const int remaining = 300000 - static_cast<int>(elapsed.elapsed());
            if (remaining <= 0) {
                process.kill();
                process.waitForFinished(2000);
                return ProcessResult::TimedOut;
            }
            process.waitForFinished(qMin(kCancelPollMs, remaining));
        }
        return cancelled() ? ProcessResult::Cancelled : ProcessResult::Finished;
    };
    if (cancelled()) return false;
#ifndef Q_OS_WIN
    if (archive.endsWith(QLatin1String(".tar.zst"), Qt::CaseInsensitive)) {
        QProcess tarProcess;
        if (cancelled()) return false;
        tarProcess.start(QStringLiteral("tar"),
                         {QStringLiteral("--zstd"), QStringLiteral("-xf"), archive,
                          QStringLiteral("-C"), QDir(destination).absolutePath()});
        const ProcessResult result = waitForExtraction(tarProcess, 3000);
        if (result == ProcessResult::Cancelled) return false;
        if (result == ProcessResult::Finished &&
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
        if (cancelled()) return false;
        process.start(sevenZip, args);
        const ProcessResult result = waitForExtraction(process, 5000);
        if (result == ProcessResult::Cancelled) return false;
        if (result == ProcessResult::StartFailed) {
            if (reason) {
                *reason = QStringLiteral("7-Zip failed to start: %1").arg(process.errorString());
            }
            return false;
        }
        if (result == ProcessResult::TimedOut) {
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
    return !cancelled();
}

}  // namespace mikai::backend
