#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QUrl>

#include <functional>

namespace mikai::backend {

QByteArray fetchUrlBytes(const QUrl& url, const QMap<QString, QString>& headers, int timeoutMs);

// Streams `url` to `destination` and only reports success once the file on disk
// carries exactly `expectedBytes` bytes whose SHA-256 matches `expectedSha256`.
//
// `destination` is created exclusively: a path that already exists is refused
// with a reason naming it and is left byte-for-byte untouched, and a file that
// appears between the existence check and the open is refused rather than
// truncated. Only this call's own partial is removed, so a cancelled, failed or
// refused download never destroys a neighbour's file.
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

QString ensureSevenZip();

bool extractArchive(const QString& sevenZip, const QString& archive, const QString& destination,
                    QString* reason);

}  // namespace mikai::backend
