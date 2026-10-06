#include "Downloads.hpp"

#include "Runtime.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

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

const QUrl& sevenZipUrl() {
    static const QUrl url(QStringLiteral("https://www.7-zip.org/a/7zr.exe"));
    return url;
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

}  // namespace

QByteArray fetchUrlBytes(const QUrl& url, const QMap<QString, QString>& headers, int timeoutMs) {
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

bool downloadToFile(const QUrl& url, const QString& destination, qint64 expectedBytes,
                    const QString& expectedSha256,
                    const std::function<void(qint64 received, qint64 total)>& progress,
                    const std::function<bool()>& cancelled, QString* reason) {
    auto fail = [&](const QString& text) {
        QFile::remove(destination);
        if (reason) {
            *reason = text;
        }
        return false;
    };
    if (QFileInfo::exists(destination)) {
        return fail(QStringLiteral("refusing to overwrite %1").arg(destination));
    }
    QFile file(destination);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(QStringLiteral("could not write %1").arg(destination));
    }
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "MPVBackend");
    QNetworkReply* reply = manager.get(request);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 received = 0;
    qint64 total = 0;
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
        file.write(chunk);
        hash.addData(chunk);
        received += chunk.size();
        if (total == 0) {
            total = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        }
        maybeReport();
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
    // a minute.
    QTimer cancelPoll;
    cancelPoll.setInterval(kCancelPollMs);
    QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&] {
        if (cancelled && cancelled()) {
            reply->abort();
        }
    });
    if (cancelled) {
        cancelPoll.start();
    }

    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        stall.start(kStallTimeoutMs);
        consume();
        if (cancelled && cancelled()) {
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
    file.close();
    const bool wasCancelled = cancelled && cancelled();
    const QString error =
        reply->error() == QNetworkReply::NoError ? QString{} : reply->errorString();
    reply->deleteLater();
    if (stalled) {
        return fail(QStringLiteral("%1 sent nothing for %2 seconds")
                        .arg(url.toString())
                        .arg(kStallTimeoutMs / 1000));
    }
    if (wasCancelled) {
        return fail(QStringLiteral("cancelled while downloading %1").arg(url.toString()));
    }
    if (!error.isEmpty()) {
        return fail(QStringLiteral("GET %1 failed: %2").arg(url.toString(), error));
    }
    if (received != expectedBytes) {
        return fail(QStringLiteral("size mismatch for %1: got %2, expected %3")
                        .arg(destination)
                        .arg(received)
                        .arg(expectedBytes));
    }
    if (QString::fromLatin1(hash.result().toHex()).compare(expectedSha256, Qt::CaseInsensitive) !=
        0) {
        return fail(QStringLiteral("sha256 mismatch for %1").arg(destination));
    }
    if (progress) {
        progress(received, total);
    }
    return true;
}

QString ensureSevenZip() {
    const QString found = findSevenZip();
    if (!found.isEmpty()) {
        return found;
    }
    const QString tool = QDir::cleanPath(cacheDir().filePath(QStringLiteral("../tools/7zr.exe")));
    QDir().mkpath(QFileInfo(tool).absolutePath());
    const QByteArray binary =
        fetchUrlBytes(sevenZipUrl(), {{QStringLiteral("User-Agent"), QStringLiteral("MPVBackend")}},
                      60000);
    QFile file(tool);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error("could not write " + tool.toStdString());
    }
    file.write(binary);
    file.close();
    return tool;
}

bool extractArchive(const QString& sevenZip, const QString& archive, const QString& destination,
                    QString* reason) {
    QProcess process;
    process.start(sevenZip, {QStringLiteral("x"),
                             QStringLiteral("-o%1").arg(QDir(destination).absolutePath()),
                             QStringLiteral("-y"), archive});
    if (!process.waitForFinished(300000) || process.exitCode() != 0) {
        const QString tail = QString::fromUtf8(process.readAllStandardError()).right(500);
        if (reason) {
            *reason = QStringLiteral("7-Zip extraction failed (exit %1): %2")
                          .arg(process.exitCode())
                          .arg(tail);
        }
        return false;
    }
    return true;
}

}  // namespace mikai::backend
