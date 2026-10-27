#include "HttpTransport.hpp"

#include "Cancellation.hpp"

#include <QEventLoop>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstddef>

namespace mikai::backend {
namespace {

// Bound post-abort draining and quiet-request cancellation polling.
constexpr int kAbortDrainMs = 2000;
constexpr int kCancelSliceMs = 200;

// Quota response headers are matched case-insensitively without allocating.
template <std::size_t N>
bool equalsLoweredAscii(QByteArrayView name, const char (&lowered)[N]) {
    if (name.size() != static_cast<qsizetype>(N - 1)) {
        return false;
    }
    for (qsizetype i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != lowered[i]) {
            return false;
        }
    }
    return true;
}

bool isPublicRedirectHeader(QByteArrayView name) {
    return equalsLoweredAscii(name, "accept") || equalsLoweredAscii(name, "accept-encoding") ||
           equalsLoweredAscii(name, "accept-language") || equalsLoweredAscii(name, "cache-control") ||
           equalsLoweredAscii(name, "content-type") || equalsLoweredAscii(name, "if-modified-since") ||
           equalsLoweredAscii(name, "if-none-match") || equalsLoweredAscii(name, "origin") ||
           equalsLoweredAscii(name, "range") || equalsLoweredAscii(name, "referer") ||
           equalsLoweredAscii(name, "user-agent");
}

bool hasCredentialHeader(const QNetworkRequest& request) {
    if (!request.url().userInfo().isEmpty() ||
        request.header(QNetworkRequest::CookieHeader).isValid()) {
        return true;
    }
    for (const QByteArray& name : request.rawHeaderList()) {
        if (!isPublicRedirectHeader(name)) {
            return true;
        }
    }
    return false;
}

}  // namespace

QuotaHeader classifyQuotaHeader(QByteArrayView name) {
    if (equalsLoweredAscii(name, "x-ratelimit-limit")) {
        return QuotaHeader::kLimit;
    }
    if (equalsLoweredAscii(name, "x-ratelimit-remaining")) {
        return QuotaHeader::kRemaining;
    }
    if (equalsLoweredAscii(name, "x-ratelimit-reset")) {
        return QuotaHeader::kReset;
    }
    if (equalsLoweredAscii(name, "retry-after")) {
        return QuotaHeader::kRetryAfter;
    }
    return QuotaHeader::kNone;
}

std::optional<int> quotaHeaderValue(const QString& value) {
    bool ok = false;
    const int parsed = value.toInt(&ok);
    return ok ? std::optional<int>(std::max(0, parsed)) : std::nullopt;
}

QNetworkAccessManager* transportManagerForThread() {
    // Keep the manager on its worker thread and retire it when that thread exits.
    static thread_local QNetworkAccessManager* manager = [] {
        auto* created = new QNetworkAccessManager;
        if (QThread* thread = QThread::currentThread()) {
            QObject::connect(thread, &QThread::finished, created, &QObject::deleteLater,
                             Qt::DirectConnection);
        }
        return created;
    }();
    return manager;
}

HttpFetchResult runBlockingFetch(QNetworkAccessManager* manager, const QNetworkRequest& request,
                                 const HttpFetchOptions& options) {
    HttpFetchResult result;
    if (WorkCancellationScope::cancellationRequested()) {
        result.cancelled = true;
        return result;
    }
    // Credential-bearing requests may redirect only within their origin.
    const bool hasCredential = hasCredentialHeader(request);
    QNetworkReply* reply = nullptr;
    if (hasCredential) {
        QNetworkRequest restricted = request;
        restricted.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                QNetworkRequest::SameOriginRedirectPolicy);
        reply = manager->get(restricted);
    } else {
        reply = manager->get(request);
    }
    QEventLoop loop;
    QTimer timer;
    QTimer abortDrainTimer;
    QMetaObject::Connection progressConnection;
    if (options.progress) {
        progressConnection = QObject::connect(reply, &QNetworkReply::downloadProgress, &loop,
            [&options](qint64 received, qint64 total) { options.progress(received, total); });
    }
    timer.setSingleShot(true);
    abortDrainTimer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        result.timedOut = reply->isRunning();
        loop.quit();
    });
    QObject::connect(&abortDrainTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    // Quiet requests must remain cancellable.
    QTimer cancelWatch;
    cancelWatch.setInterval(kCancelSliceMs);
    QObject::connect(&cancelWatch, &QTimer::timeout, &loop, [&] {
        if (reply->isRunning() && WorkCancellationScope::cancellationRequested()) {
            result.cancelled = true;
            loop.quit();
        }
    });
    timer.start(options.timeoutMs);
    cancelWatch.start();
    loop.exec();
    timer.stop();
    if (reply->isRunning()) {
        // Bound the abort drain so a pool thread cannot wait indefinitely.
        reply->abort();
        if (reply->isRunning()) {
            abortDrainTimer.start(kAbortDrainMs);
            loop.exec();
        }
    }
    cancelWatch.stop();
    abortDrainTimer.stop();
    QObject::disconnect(progressConnection);

    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto pairs = reply->rawHeaderPairs();
    for (const auto& pair : pairs) {
        switch (classifyQuotaHeader(pair.first)) {
            case QuotaHeader::kLimit:
                result.quota.limit = quotaHeaderValue(QString::fromUtf8(pair.second));
                break;
            case QuotaHeader::kRemaining:
                result.quota.remaining = quotaHeaderValue(QString::fromUtf8(pair.second));
                break;
            case QuotaHeader::kReset:
                result.quota.reset = quotaHeaderValue(QString::fromUtf8(pair.second));
                break;
            case QuotaHeader::kRetryAfter:
                result.quota.retryAfter = quotaHeaderValue(QString::fromUtf8(pair.second));
                break;
            case QuotaHeader::kNone:
                break;
        }
        if (pair.first.compare("Cache-Control", Qt::CaseInsensitive) == 0) {
            result.cacheControl = QString::fromLatin1(pair.second);
        } else if (pair.first.compare("ETag", Qt::CaseInsensitive) == 0) {
            result.etag = pair.second;
        }
        if (options.collectHeaders) {
            result.headers.insert(QString::fromUtf8(pair.first), QString::fromUtf8(pair.second));
        }
    }
    if (reply->error() != QNetworkReply::NoError) {
        result.networkError = reply->errorString();
    }
    result.body = reply->readAll();
    reply->deleteLater();
    return result;
}

}  // namespace mikai::backend
