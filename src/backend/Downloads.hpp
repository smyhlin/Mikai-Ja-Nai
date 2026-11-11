#pragma once

#include <QByteArray>
#include <QMap>
#include <QProcess>
#include <QString>
#include <QUrl>

#include <functional>

namespace mikai::backend {

/// Outcome of a bounded, cancellation-aware wait on a child process.
/// (P2-01: the single wait shape; the bootstrap/extraction/describe waits
/// below are all built on these two functions.)
enum class ProcessWaitResult {
    Finished,     ///< The waited-for transition happened.
    StartFailed,  ///< The process never reached Running (start wait only).
    TimedOut,     ///< The finish deadline passed; the process was killed.
    Cancelled,    ///< WorkCancellationScope was requested; the process was killed.
};

/// Wait until `process` leaves Starting, polling WorkCancellationScope every
/// 200 ms. The process is killed and reaped on timeout or cancellation.
/// Returns Finished only when the process is Running.
ProcessWaitResult waitForProcessStarted(QProcess& process, int startTimeoutMs);

/// Wait until `process` reaches NotRunning, polling WorkCancellationScope
/// every 200 ms. The process is killed and reaped on timeout or
/// cancellation (`reapTimeoutMs` bounds the reap after the kill).
ProcessWaitResult waitForProcessFinished(QProcess& process, int finishTimeoutMs,
                                         int reapTimeoutMs = 1000);

QByteArray fetchUrlBytes(const QUrl& url, const QMap<QString, QString>& headers, int timeoutMs);

// Streams `url` to `destination` and only reports success once the file on disk
// carries exactly `expectedBytes` bytes whose SHA-256 matches `expectedSha256`.
//
// The bytes go to a unique sibling partial (`<name>.part-XXXXXX`) and only a
// fully written, verified partial is renamed over `destination`, so a hard
// crash mid-transfer can never leave a partial at the final path. A path that
// already exists is refused with a reason naming it and is left byte-for-byte
// untouched, and a file that appears while this call was writing is refused at
// the rename rather than clobbered. Only this call's own partial is removed,
// so a cancelled, failed or refused download never destroys a neighbour's
// file.
//
// Every chunk's write result, the final flush and the close error are checked;
// a short write or a storage error aborts the transfer and reports failure. The
// transfer is dropped once storage fails, once a 60 s silent stretch elapses, or
// once cancellation is seen - through `cancelled` or the shared thread-local
// work-cancellation state - whichever comes first. `progress` reports at most
// every 100 ms and always once more at the terminal state.
//
// Every user-visible/loggable URL-bearing string this function produces is run
// through backend::redactDiagnostic, so signed credentials never reach a log or
// an exported report.
bool downloadToFile(const QUrl& url, const QString& destination, qint64 expectedBytes,
                    const QString& expectedSha256,
                    const std::function<void(qint64 received, qint64 total)>& progress,
                    const std::function<bool()>& cancelled, QString* reason);

QString ensureSevenZip(const QString& toolsDirectory);

// Cancellation stops and reaps the extraction tool and reports failure. The
// caller owns destination and must discard its potentially partial contents.
bool extractArchive(const QString& sevenZip, const QString& archive, const QString& destination,
                    QString* reason);

}  // namespace mikai::backend
