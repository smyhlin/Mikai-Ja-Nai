#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QMap>
#include <QString>

#include <functional>
#include <optional>

class QNetworkAccessManager;
class QNetworkRequest;

namespace mikai::backend {

// Shared blocking transport used by API, stream and download callers.

// Quota headers are classified case-insensitively.
enum class QuotaHeader { kNone, kLimit, kRemaining, kReset, kRetryAfter };

QuotaHeader classifyQuotaHeader(QByteArrayView name);
std::optional<int> quotaHeaderValue(const QString& value);

// The quota snapshot a reply's four headers describe.
struct QuotaSnapshot {
    std::optional<int> limit;
    std::optional<int> remaining;
    std::optional<int> reset;
    std::optional<int> retryAfter;
};

// Transport outcome; callers retain their own error wording.
struct HttpFetchResult {
    int status = 0;
    QByteArray body;
    bool cancelled = false;
    bool timedOut = false;
    QString networkError;
    QuotaSnapshot quota;
    QString cacheControl;
    QByteArray etag;
    // Present only when `HttpFetchOptions::collectHeaders` asked for it.
    QMap<QString, QString> headers;
};

struct HttpFetchOptions {
    int timeoutMs = 20000;
    // Collect arbitrary headers only for callers that need them.
    bool collectHeaders = false;
    std::function<void(qint64 received, qint64 total)> progress;
};

// Runs one bounded GET on the calling thread; retry policy remains with callers.
HttpFetchResult runBlockingFetch(QNetworkAccessManager* manager, const QNetworkRequest& request,
                                 const HttpFetchOptions& options);

// Lazily creates a manager on the calling thread and retires it with that thread.
QNetworkAccessManager* transportManagerForThread();

}  // namespace mikai::backend
