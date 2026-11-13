#include "Downloads.hpp"

#include "Cancellation.hpp"
#include "Diagnostics.hpp"
#include "HttpTransport.hpp"
#include "Runtime.hpp"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDebug>
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
#include <QDebug>
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

/// Archive-extraction wall clock (P2-06): one named constant, not an inline
/// literal at the wait call.
constexpr int kExtractionTimeoutMs = 300000;

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
    if (toolsDirectory.isEmpty()) {
        qWarning() << "mikai: findSevenZip: empty tools directory, refusing CWD-relative probe";
        return {};
    }
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
    qWarning() << "mikai: findSevenZip: no 7z executable found";
    return {};
#else
    for (const QString& name : {QStringLiteral("7zz"), QStringLiteral("7zzs"), QStringLiteral("7z")}) {
        const QString bundled = QDir(toolsDirectory).filePath(name);
        const QFileInfo info(bundled);
        if (info.isFile() && info.isExecutable()) {
            return bundled;
        }
    }
    // Distribution builds can lack Zstd; the pinned extractor supports every runtime archive.
    qWarning() << "mikai: findSevenZip: no bundled 7z executable found";
    return {};
#endif
}

void throwIfBootstrapCancelled() {
    if (WorkCancellationScope::cancellationRequested()) {
        throw std::runtime_error("the request was cancelled");
    }
}

bool waitForBootstrapStart(QProcess& process, int timeoutMs) {
    // (P2-01: thin wrapper over the one shared wait; the throw-on-cancel
    // contract is the bootstrap callers' own. Finished only counts when the
    // process is still Running: a bootstrap tool that already exited is a
    // failed start for its callers, exactly as before.)
    switch (waitForProcessStarted(process, timeoutMs)) {
        case ProcessWaitResult::Cancelled: throw std::runtime_error("the request was cancelled");
        case ProcessWaitResult::Finished: return process.state() == QProcess::Running;
        case ProcessWaitResult::StartFailed:
        case ProcessWaitResult::TimedOut: return false;
    }
    return false;
}

bool waitForBootstrapFinish(QProcess& process, int timeoutMs) {
    // (P2-01: thin wrapper over the one shared wait; the throw-on-cancel
    // contract is the bootstrap callers' own.)
    switch (waitForProcessFinished(process, timeoutMs)) {
        case ProcessWaitResult::Cancelled: throw std::runtime_error("the request was cancelled");
        case ProcessWaitResult::Finished: return true;
        case ProcessWaitResult::TimedOut:
        case ProcessWaitResult::StartFailed: return false;
    }
    return false;
}

}  // namespace

// ── P2-01: the one bounded, cancellation-aware QProcess wait ────────────────
// Both waits poll WorkCancellationScope every kCancelPollMs and kill + reap
// the child on timeout or cancellation, so no caller blocks on a child that
// never answers.

ProcessWaitResult waitForProcessStarted(QProcess& process, int startTimeoutMs) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (process.state() == QProcess::Starting) {
        if (WorkCancellationScope::cancellationRequested()) {
            if (process.state() != QProcess::NotRunning) {
                process.kill();
                process.waitForFinished(1000);
            }
            return ProcessWaitResult::Cancelled;
        }
        const int remaining = startTimeoutMs - static_cast<int>(elapsed.elapsed());
        if (remaining <= 0) {
            if (process.state() != QProcess::NotRunning) {
                process.kill();
                process.waitForFinished(1000);
            }
            return ProcessWaitResult::StartFailed;
        }
        process.waitForStarted(qMin(kCancelPollMs, remaining));
    }
    // Left Starting: Running, or started and already finished (a fast
    // helper's output is ready, not a launch failure). Only a process
    // that never started at all reports StartFailed.
    return process.error() == QProcess::FailedToStart ? ProcessWaitResult::StartFailed
                                                      : ProcessWaitResult::Finished;
}

ProcessWaitResult waitForProcessFinished(QProcess& process, int finishTimeoutMs,
                                         int reapTimeoutMs) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (process.state() != QProcess::NotRunning) {
        if (WorkCancellationScope::cancellationRequested()) {
            process.kill();
            process.waitForFinished(reapTimeoutMs);
            return ProcessWaitResult::Cancelled;
        }
        const int remaining = finishTimeoutMs - static_cast<int>(elapsed.elapsed());
        if (remaining <= 0) {
            process.kill();
            process.waitForFinished(reapTimeoutMs);
            return ProcessWaitResult::TimedOut;
        }
        process.waitForFinished(qMin(kCancelPollMs, remaining));
    }
    return WorkCancellationScope::cancellationRequested() ? ProcessWaitResult::Cancelled
                                                          : ProcessWaitResult::Finished;
}

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
    if (toolsDirectory.isEmpty()) {
        qWarning() << "mikai: ensureSevenZip: empty tools directory";
        return {};
    }
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

namespace {

// P0-01 (rung 3 — list entries first, reject bad ones; do not write a custom
// extractor). A crafted pack must never write outside its destination, so an
// archive is listed before anything is extracted and extraction never starts
// on an unlisted archive. The in-code check is mandatory regardless of the
// extractor version: the bundled 7-Zip is 26.03 (>= 25.01, which fixed
// CVE-2025-55188; 25.00 fixed CVE-2025-11001 / CVE-2025-11002), but a system
// 7-Zip from PATH may be older and no call passes -snld20.
bool archiveEntriesAreSafe(const QStringList& entries, QString* reason) {
    for (const QString& raw : entries) {
        const QString entry = QDir::cleanPath(raw);
        // After cleanPath, ".." can only be leading; the contains() check is
        // the plan's belt-and-braces form of the same rule. The drive-letter
        // check covers "C:..." on platforms where QDir::isAbsolutePath does
        // not recognise it.
        if (entry.startsWith(QLatin1String("..")) || QDir::isAbsolutePath(entry) ||
            entry.contains(QLatin1String("..")) ||
            (entry.size() >= 2 && entry.at(1) == QLatin1Char(':'))) {
            if (reason) {
                *reason =
                    QStringLiteral("archive entry escapes destination: %1").arg(raw);
            }
            return false;
        }
    }
    return true;
}

struct ArchiveListing {
    QStringList entries;
    bool hasSymlink = false;
};

// One bounded listing run: start the tool, reap it, capture stdout. A listing
// that never starts, is cancelled, or does not finish in time refuses the
// archive instead of letting extraction proceed unlisted.
bool runArchiveListing(const QString& program, const QStringList& args, QByteArray* output,
                       QString* reason) {
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(5000)) {
        if (reason) {
            *reason = QStringLiteral("could not list archive: %1 failed to start").arg(program);
        }
        return false;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    while (process.state() != QProcess::NotRunning) {
        if (WorkCancellationScope::cancellationRequested()) {
            process.kill();
            process.waitForFinished(2000);
            if (reason) *reason = QStringLiteral("archive extraction was cancelled");
            return false;
        }
        const int remaining = 30000 - static_cast<int>(elapsed.elapsed());
        if (remaining <= 0) {
            process.kill();
            process.waitForFinished(2000);
            if (reason) *reason = QStringLiteral("could not list archive: listing timed out");
            return false;
        }
        process.waitForFinished(qMin(200, remaining));
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (reason) {
            *reason =
                QStringLiteral("could not list archive (exit %1)").arg(process.exitCode());
        }
        return false;
    }
    if (output) *output = process.readAllStandardOutput();
    return true;
}

// `tar --zstd -tf` names the entries; `tar --zstd -tvf` is the symlink
// oracle. A verbose line starts with the entry mode, so any line starting
// with 'l' is a link entry. The names come from -tf alone because -tvf
// column layout differs between GNU and BSD tar. cleanPath() is lexical and
// keeps symlinks (REV5 V-34), so the name check alone cannot catch a link
// escape — the oracle is mandatory, not advisory.
bool listTarEntries(const QString& archive, ArchiveListing* listing, QString* reason) {
    QByteArray names;
    if (!runArchiveListing(QStringLiteral("tar"),
                           {QStringLiteral("--zstd"), QStringLiteral("-tf"), archive}, &names,
                           reason)) {
        return false;
    }
    QByteArray verbose;
    if (!runArchiveListing(QStringLiteral("tar"),
                           {QStringLiteral("--zstd"), QStringLiteral("-tvf"), archive}, &verbose,
                           reason)) {
        return false;
    }
    listing->entries = QString::fromUtf8(names).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QByteArray& line : verbose.split('\n')) {
        if (!line.isEmpty() && line.at(0) == 'l') {
            listing->hasSymlink = true;
            break;
        }
    }
    return true;
}

// `7z l -slt` prints one blank-line-separated block per entry, each with
// "Path = ..." and "Attributes = ...". The first block describes the archive
// itself — it carries a "Type = ..." line, which no entry block has — and is
// skipped. (Skipping by block order would be wrong: blank lines in the header
// precede the first block.) A link entry carries an Attributes value starting
// with 'l' (the Unix mode string) or a "Symbolic Link = " line.
bool listSevenZipEntries(const QString& sevenZip, const QString& archive,
                         ArchiveListing* listing, QString* reason) {
    QByteArray output;
    if (!runArchiveListing(sevenZip,
                           {QStringLiteral("l"), QStringLiteral("-slt"), archive}, &output,
                           reason)) {
        return false;
    }
    const QString text = QString::fromUtf8(output);
    QString pendingPath;
    bool pendingIsArchive = false;
    bool pendingSymlink = false;
    const auto flushBlock = [&] {
        if (!pendingIsArchive && !pendingPath.isEmpty()) {
            if (pendingSymlink) listing->hasSymlink = true;
            listing->entries.append(pendingPath);
        }
        pendingPath.clear();
        pendingIsArchive = false;
        pendingSymlink = false;
    };
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            flushBlock();
            continue;
        }
        if (trimmed.startsWith(QLatin1String("Path = "))) {
            pendingPath = trimmed.mid(7);
        } else if (trimmed.startsWith(QLatin1String("Type = "))) {
            pendingIsArchive = true;
        } else if (trimmed.startsWith(QLatin1String("Attributes = "))) {
            if (trimmed.mid(13).trimmed().startsWith(QLatin1Char('l'))) {
                pendingSymlink = true;
            }
        } else if (trimmed.startsWith(QLatin1String("Symbolic Link = "))) {
            pendingSymlink = true;
        }
    }
    flushBlock();
    return true;
}

// Lists the archive and rejects anything that would escape the destination
// or smuggle a symlink through it. Returns false with *reason set; nothing
// has been extracted when this returns false.
bool archiveListingIsSafe(const QString& sevenZip, const QString& archive, bool useTar,
                          QString* reason) {
    ArchiveListing listing;
    const bool listed = useTar ? listTarEntries(archive, &listing, reason)
                               : listSevenZipEntries(sevenZip, archive, &listing, reason);
    if (!listed) {
        if (reason != nullptr) {
            qWarning().noquote() << "mikai: refusing unlisted archive:"
                                 << redactDiagnostic(*reason);
        }
        return false;
    }
    if (listing.hasSymlink) {
        if (reason != nullptr) {
            *reason = QStringLiteral("archive contains a symbolic link entry; "
                                     "symlinks are not extracted");
        }
        qWarning("mikai: refusing archive with a symbolic link entry");
        return false;
    }
    if (!archiveEntriesAreSafe(listing.entries, reason)) {
        if (reason != nullptr) {
            qWarning().noquote() << "mikai: refusing archive entry:"
                                 << redactDiagnostic(*reason);
        }
        return false;
    }
    return true;
}

}  // namespace

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
    // (P2-01: the waits are the one shared helper; this lambda only maps
    // its outcome onto ProcessResult and records the reason.)
    auto waitForExtraction = [&](QProcess& process, int startTimeoutMs) {
        const ProcessWaitResult started = waitForProcessStarted(process, startTimeoutMs);
        if (started == ProcessWaitResult::Cancelled) {
            cancelled();
            return ProcessResult::Cancelled;
        }
        if (started != ProcessWaitResult::Finished) return ProcessResult::StartFailed;
        const ProcessWaitResult finished = waitForProcessFinished(process, kExtractionTimeoutMs, 2000);
        if (finished == ProcessWaitResult::Cancelled) {
            cancelled();
            return ProcessResult::Cancelled;
        }
        if (finished == ProcessWaitResult::TimedOut) return ProcessResult::TimedOut;
        return ProcessResult::Finished;
    };
    if (cancelled()) return false;
#ifndef Q_OS_WIN
    if (archive.endsWith(QLatin1String(".tar.zst"), Qt::CaseInsensitive)) {
        // P0-01: list first, extract second — a rejected listing extracts nothing.
        if (!archiveListingIsSafe(sevenZip, archive, true, reason)) {
            return false;
        }
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

    // P0-01: list first, extract second — a rejected listing extracts nothing.
    // The -snl flag is gone: symlink entries are rejected by the listing, and
    // no caller opts in to extracting them (7-Zip >= 25.01 is pinned for the
    // bundled tool; never pass -snld20).
    if (!archiveListingIsSafe(sevenZip, archive, false, reason)) {
        return false;
    }

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
            // P0-01: the inner .tar is its own archive — list it before the
            // second pass, or it would be extracted unlisted.
            if (!archiveListingIsSafe(sevenZip, innerTar, false, reason)) {
                QFile::remove(innerTar);
                return false;
            }
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
