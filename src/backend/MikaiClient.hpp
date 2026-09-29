#pragma once

#include <QByteArray>
#include <QMap>
#include <QMutex>
#include <QString>
#include <QVariant>
#include <QWaitCondition>

#include <optional>

#include "Errors.hpp"
#include "Models.hpp"

class QNetworkAccessManager;

namespace mikai::backend {

// The API sits behind a bot filter that challenges requests without a
// User-Agent. Qt sends none by default, so every request has to carry one;
// the Python client got this from requests' own default.
inline const QString kApiUserAgent = QStringLiteral(
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/142.0.0.0 Safari/537.36");

struct RateLimitState {
    int limit = 0;
    std::optional<int> remaining;
    std::optional<int> used;
    int resetIn = 0;
    int queued = 0;
    int inFlight = 0;
};

// RateLimiter mirrors the Python client's condition-variable gate: it parks
// callers when the server advertises no remaining quota, when the advertised
// reset window has not elapsed, or after a 429 with a Retry-After.
class RateLimiter {
public:
    RateLimiter() = default;

    int acquireSlot();
    void finishCall(const QMap<QString, QString>& headers, int generation, int statusCode = 0);
    RateLimitState state(bool keyed) const;

    // Called after the key changes so the next caller starts a fresh window.
    void reset();

private:
    mutable QMutex mutex_;
    QWaitCondition condition_;
    std::optional<int> limit_;
    std::optional<int> remaining_;
    double resetAt_ = 0.0;
    double retryAt_ = 0.0;
    int inFlight_ = 0;
    int waiters_ = 0;
    int generation_ = 0;
};

// Every call is blocking and throws MikaiApiError on a failure envelope or a
// transport error. The client is safe to share across threads: each worker
// gets its own QNetworkAccessManager, and the limiter is mutex-guarded.
class MikaiClient {
public:
    explicit MikaiClient(const QString& apiKey = {});
    ~MikaiClient();
    MikaiClient(const MikaiClient&) = delete;
    MikaiClient& operator=(const MikaiClient&) = delete;

    SearchResult search(const QString& query, int page = 1, int limit = 20) const;
    SearchResult browseOngoing(int page = 1, int limit = 24) const;
    SearchResult browsePopular(int page = 1, int limit = 18) const;
    UpdatesResult getUpdates(int page = 1, int limit = 12) const;
    ScheduleResult getSchedule() const;
    AnimeDetail getAnime(const QString& ref) const;
    PlayerResult getPlayer(const QString& ref) const;

    // Artwork download: bypasses the rate limiter, 20s timeout.
    QByteArray fetchBytes(const QString& url) const;

    QString apiKey() const;
    void setApiKey(const QString& apiKey);

    // Returns {valid, authenticated, rate_limit, name, error}.
    QMap<QString, QVariant> validateApiKey(const QString& apiKey) const;

    RateLimitState rateLimitState() const;

private:
    QNetworkAccessManager* managerForThread() const;
    QJsonObject get(const QString& path, const QMap<QString, QString>& params = {}) const;
    QJsonObject apiGet(const QString& url, const QMap<QString, QString>& params = {},
                       const QString& overrideKey = {}, int timeoutMs = 15000) const;

    mutable QString apiKey_;
    mutable RateLimiter limiter_;
};

}  // namespace mikai::backend
