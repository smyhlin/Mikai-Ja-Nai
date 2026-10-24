#include "MikaiClient.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMetaObject>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QHash>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace mikai::backend {
namespace {

constexpr int kAnonymousLimit = 60;
constexpr int kKeyedLimit = 300;
constexpr int kApiTimeoutMs = 15000;
constexpr int kArtworkTimeoutMs = 20000;
constexpr int kValidateTimeoutMs = 10000;
constexpr auto kApiHeader = "X-API-Key";
// An abort is not synchronous: a reply parked in DNS or TLS finishes a moment
// later. The drain after it needs a bound, because the second exec() used to
// run with no timer at all and lost the worker thread for good.
constexpr int kAbortDrainMs = 2000;
// A 429 is retried, because the limiter parks the caller for Retry-After and
// the same request may then succeed. It used to retry forever, so an endpoint
// that always answered 429 pinned its worker thread for the whole session.
constexpr int kMaxRateLimitRetries = 3;
// How long a parked caller sleeps before it looks at the cancellation scope
// again. The scope is thread-local and a cancel is a plain atomic store, so it
// cannot wake the condition variable on its own; a slice this short bounds the
// delay a Cancel press feels without turning the wait into a poll loop - an
// exhausted limiter window may be minutes long, and this costs one wakeup per
// second at most.
constexpr int kCancelSliceMs = 200;
constexpr qsizetype kMaxCacheEntries = 128;
constexpr qsizetype kMaxCacheBytes = 4 * 1024 * 1024;

struct CachedJson {
    QJsonObject body;
    QByteArray etag;
    double freshUntil = 0.0;
    qsizetype bytes = 0;
    quint64 touched = 0;
};

struct JsonCache {
    QMutex mutex;
    QHash<QByteArray, CachedJson> entries;
    qsizetype bytes = 0;
    quint64 clock = 0;
};

JsonCache& jsonCache() {
    static JsonCache cache;
    return cache;
}

QByteArray cacheKey(const QUrl& url, const CredentialSlot& slot) {
    QByteArray identity = url.toEncoded(QUrl::FullyEncoded);
    identity += '\n';
    identity += QByteArray::number(slot.generation);
    identity += '\n';
    identity += QCryptographicHash::hash(slot.key.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QCryptographicHash::hash(identity, QCryptographicHash::Sha256);
}

bool cacheableEndpoint(const QUrl& url, const QString& overrideKey) {
    if (!overrideKey.isEmpty()) return false;
    const QString path = url.path();
    return !path.endsWith(QStringLiteral("/health")) && !path.endsWith(QStringLiteral("/me")) &&
           !path.endsWith(QStringLiteral("/anime/random"));
}

int cacheMaxAge(const QString& control) {
    if (control.contains(QStringLiteral("no-store"), Qt::CaseInsensitive) ||
        control.contains(QStringLiteral("no-cache"), Qt::CaseInsensitive)) return 0;
    const QStringList directives = control.split(QLatin1Char(','));
    for (const QString& directive : directives) {
        const QString item = directive.trimmed();
        const qsizetype equals = item.indexOf(QLatin1Char('='));
        if (equals >= 0 && item.left(equals).trimmed().compare(QStringLiteral("max-age"), Qt::CaseInsensitive) == 0) {
            bool ok = false;
            const int seconds = item.mid(equals + 1).trimmed().remove(QLatin1Char('"')).toInt(&ok);
            return ok ? std::max(0, seconds) : 0;
        }
    }
    return 0;
}

void pruneCache(JsonCache& cache) {
    while (cache.entries.size() > kMaxCacheEntries || cache.bytes > kMaxCacheBytes) {
        auto oldest = cache.entries.end();
        for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
            if (oldest == cache.entries.end() || it->touched < oldest->touched) oldest = it;
        }
        if (oldest == cache.entries.end()) break;
        cache.bytes -= oldest->bytes;
        cache.entries.erase(oldest);
    }
}

double monotonicSeconds() {
    static const QElapsedTimer timer = [] {
        QElapsedTimer started;
        started.start();
        return started;
    }();
    return static_cast<double>(timer.nsecsElapsed()) / 1e9;
}

// Cloudflare lower-cases every header name, so a case-sensitive lookup
// silently loses the quota snapshot. A reply carries ~20 headers and the
// limiter reads 4, so names are classified once per header here rather than
// rescanning per lookup.
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

enum class QuotaHeader { kNone, kLimit, kRemaining, kReset, kRetryAfter };

QuotaHeader classifyHeader(QByteArrayView name) {
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

std::optional<int> quotaInt(const QString& value) {
    bool ok = false;
    const int parsed = value.toInt(&ok);
    return ok ? std::optional<int>(std::max(0, parsed)) : std::nullopt;
}

// An isolated caller - a test, a probe - installs its own .env location here so
// the search below can never reach a file that belongs to the repository or to
// an installed build. An empty override restores the search.
QString& envOverride() {
    static QString path;
    return path;
}

// A QNetworkAccessManager belongs to the thread that created it, and the
// artwork pool fires a dozen concurrent GETs, so give every worker its own
// instead of serialising them behind one connection pool.
QNetworkAccessManager* threadManager() {
    // One manager per thread, so the hot path is a thread-local read rather
    // than a process-global mutex taken on every request. The old registry was
    // keyed by QThread*, which also handed a replacement pool thread the
    // recycled address of a dead one, along with its manager.
    static thread_local QNetworkAccessManager* manager = [] {
        auto* created = new QNetworkAccessManager;
        // A pool thread that expires must not leave its manager behind: the
        // thread is gone, and nothing can ever use or safely destroy it again.
        QObject::connect(QThread::currentThread(), &QThread::finished, created,
                         &QObject::deleteLater);
        return created;
    }();
    return manager;
}

// The four quota headers, read in one pass. Everything else on the reply is of
// no interest and is not copied into a QString map at all.
struct QuotaHeaders {
    std::optional<int> limit;
    std::optional<int> remaining;
    std::optional<int> reset;
    std::optional<int> retryAfter;
};

struct HttpResult {
    int status = 0;
    QByteArray body;
    QuotaHeaders quota;
    QString error;
    QString cacheControl;
    QByteArray etag;
    bool cancelled = false;
};

HttpResult runBlocking(QNetworkAccessManager* manager, const QNetworkRequest& request, int timeoutMs,
                      const std::function<void(qint64, qint64)>& progress = {}) {
    HttpResult result;
    if (WorkCancellationScope::cancellationRequested()) {
        result.cancelled = true;
        return result;
    }
    QNetworkReply* reply = manager->get(request);
    QEventLoop loop;
    QTimer timer;
    QTimer abortDrainTimer;
    QMetaObject::Connection progressConnection;
    if (progress) {
        progressConnection = QObject::connect(reply, &QNetworkReply::downloadProgress, &loop,
            [&progress](qint64 received, qint64 total) { progress(received, total); });
    }
    timer.setSingleShot(true);
    abortDrainTimer.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        timedOut = reply->isRunning();
        loop.quit();
    });
    QObject::connect(&abortDrainTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    // Cancellation is observed on its own clock rather than only when the reply
    // finishes: a worker being shut down while a request is parked in DNS or
    // TLS must not hold its thread until the deadline. Aborting the reply is
    // what releases the event loop, and the drain below bounds the moment it
    // takes to unwind.
    QTimer cancelWatch;
    cancelWatch.setSingleShot(false);
    cancelWatch.setInterval(kCancelSliceMs);
    bool cancelSeen = false;
    QObject::connect(&cancelWatch, &QTimer::timeout, &loop, [&] {
        if (WorkCancellationScope::cancellationRequested()) {
            cancelSeen = true;
            loop.quit();
        }
    });
    cancelWatch.start();
    const auto stopWatchers = [&] {
        cancelWatch.stop();
    };
    timer.start(timeoutMs);
    loop.exec();
    timer.stop();
    if (reply->isRunning()) {
        // The deadline fired first: abort so the reply cannot outlive us. The
        // drain is on the same timer rather than an unbounded wait, because an
        // abort can complete late and this thread belongs to a pool.
        reply->abort();
        if (reply->isRunning()) {
            abortDrainTimer.start(kAbortDrainMs);
            loop.exec();
        }
    }
    stopWatchers();
    timer.stop();
    abortDrainTimer.stop();
    QObject::disconnect(progressConnection);
    // Only a cancel that actually stopped this reply counts: a caller whose
    // scope happens to be installed while an ordinary reply completes has not
    // cancelled anything, and reporting it as cancelled would discard a result
    // the server did send.
    result.cancelled = cancelSeen;

    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto pairs = reply->rawHeaderPairs();
    for (const auto& pair : pairs) {
        if (pair.first.compare("Cache-Control", Qt::CaseInsensitive) == 0) {
            result.cacheControl = QString::fromLatin1(pair.second);
        } else if (pair.first.compare("ETag", Qt::CaseInsensitive) == 0) {
            result.etag = pair.second;
        }
        switch (classifyHeader(pair.first)) {
            case QuotaHeader::kLimit:
                result.quota.limit = quotaInt(QString::fromUtf8(pair.second));
                break;
            case QuotaHeader::kRemaining:
                result.quota.remaining = quotaInt(QString::fromUtf8(pair.second));
                break;
            case QuotaHeader::kReset:
                result.quota.reset = quotaInt(QString::fromUtf8(pair.second));
                break;
            case QuotaHeader::kRetryAfter:
                result.quota.retryAfter = quotaInt(QString::fromUtf8(pair.second));
                break;
            case QuotaHeader::kNone:
                break;
        }
    }
    if (timedOut && !result.cancelled) {
        result.error = QStringLiteral("the request timed out after %1 ms").arg(timeoutMs);
    } else if (reply->error() != QNetworkReply::NoError) {
        result.error = reply->errorString();
    }
    result.body = reply->readAll();
    reply->deleteLater();
    return result;
}

QString reasonFor(int status) {
    switch (status) {
        case 400: return QStringLiteral("Bad Request");
        case 401: return QStringLiteral("Unauthorized");
        case 403: return QStringLiteral("Forbidden");
        case 404: return QStringLiteral("Not Found");
        case 429: return QStringLiteral("Too Many Requests");
        case 500: return QStringLiteral("Internal Server Error");
        case 503: return QStringLiteral("Service Unavailable");
        default: return QStringLiteral("request failed");
    }
}

SearchResult searchFromPayload(const QJsonObject& payload, int fallbackPage) {
    SearchResult result;
    const QJsonValue total = payload.value(QStringLiteral("total"));
    if (total.isDouble()) {
        result.total = static_cast<int>(total.toDouble());
    }
    const QJsonValue page = payload.value(QStringLiteral("page"));
    result.page = page.isDouble() ? std::optional<int>(static_cast<int>(page.toDouble()))
                                  : std::optional<int>(fallbackPage);
    const QJsonValue pages = payload.value(QStringLiteral("pages"));
    if (pages.isDouble()) {
        result.pages = static_cast<int>(pages.toDouble());
    }
    const QJsonArray items = payload.value(QStringLiteral("result")).toArray();
    result.items.reserve(items.size());
    for (const QJsonValue& value : items) {
        result.items.append(parseSummary(value.toObject()));
    }
    return result;
}

TeamProfile parseTeamProfile(const QJsonObject& data) {
    TeamProfile profile;
    profile.team = parseTeam(data);
    profile.description = data.value(QStringLiteral("description")).toString();
    profile.birthday = data.value(QStringLiteral("birthday")).toString();
    profile.avatar = parsePosterSet(data.value(QStringLiteral("avatar")).toObject());
    const QJsonObject links = data.value(QStringLiteral("links")).toObject();
    for (auto it = links.begin(); it != links.end(); ++it) {
        if (it.value().isString()) profile.links.insert(it.key(), it.value().toString());
    }
    profile.has_voice = data.value(QStringLiteral("hasVoice")).toBool();
    profile.has_subs = data.value(QStringLiteral("hasSubs")).toBool();
    profile.anime_count = data.value(QStringLiteral("animeCount")).toInt();
    const QJsonObject views = data.value(QStringLiteral("views")).toObject();
    profile.views_total = static_cast<qint64>(views.value(QStringLiteral("total")).toDouble());
    profile.views_last_30_days =
        static_cast<qint64>(views.value(QStringLiteral("last30Days")).toDouble());
    return profile;
}

const QStringList& weekdayOrder() {
    static const QStringList days{QStringLiteral("monday"),   QStringLiteral("tuesday"),
                                  QStringLiteral("wednesday"), QStringLiteral("thursday"),
                                  QStringLiteral("friday"),   QStringLiteral("saturday"),
                                  QStringLiteral("sunday")};
    return days;
}

}  // namespace

QString envFilePath() {
    const QString override = envOverride();
    if (!override.isEmpty()) {
        return override;
    }
    // The .env the Python client wrote next to the repo root. The C++ build has
    // no __file__, so walk up from the executable until a .env shows up and fall
    // back to the current working directory.
    QDir dir = QCoreApplication::applicationDirPath();
    for (int depth = 0; depth < 4; ++depth) {
        const QString candidate = dir.filePath(QStringLiteral(".env"));
        if (QFile::exists(candidate)) {
            return candidate;
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return QDir::current().filePath(QStringLiteral(".env"));
}

void setEnvFilePathOverride(const QString& path) { envOverride() = path; }

QString apiUserAgent() {
    QString name = QCoreApplication::applicationName().simplified();
    if (name.isEmpty()) name = kApiApplicationName;
    name.replace(QLatin1Char(' '), QLatin1Char('-'));
    const QString declaredVersion = QCoreApplication::applicationVersion();
    const QString version = declaredVersion.isEmpty() ? kApiApplicationVersion : declaredVersion;
    return QStringLiteral("%1/%2").arg(name, version);
}

namespace {

QString readApiKeyFromEnv() {
    QFile file(envFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const int equals = line.indexOf(QLatin1Char('='));
        const int separator = equals >= 0 ? equals : line.indexOf(QLatin1Char(':'));
        if (separator < 0) {
            continue;
        }
        const QString name = line.left(separator).trimmed().toLower();
        if (name == QLatin1String("x-api-key") || name == QLatin1String("api_key") ||
            name == QLatin1String("apikey")) {
            const QString value = line.mid(separator + 1).trimmed();
            return value.isEmpty() ? QString{} : value;
        }
    }
    return {};
}

// The payload the file holds, written through a QSaveFile so a partial write
// never lands. Clearing the key writes an empty file rather than removing it:
// the Python client reads the location, not the entry.
bool writeEnvFile(const QString& path, const QString& apiKey, QString* reason) {
    QSaveFile file(path);
    // No direct-write fallback: renaming a temporary onto the target is what
    // makes a failed replace leave the previous key exactly as it was.
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (reason != nullptr) {
            *reason = QStringLiteral("could not write %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    const QByteArray payload =
        apiKey.isEmpty() ? QByteArray{} : QStringLiteral("X-API-Key: %1\n").arg(apiKey).toUtf8();
    if (!payload.isEmpty() && file.write(payload) != payload.size()) {
        if (reason != nullptr) {
            *reason = QStringLiteral("could not write %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    if (!file.commit()) {
        if (reason != nullptr) {
            *reason = QStringLiteral("could not save %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

// A failure envelope is decoded before anything else is decided about an HTTP
// answer, so a documented 403/404/422 keeps its code and message. A body that
// is not an envelope is an infrastructure answer - a proxy page, a gateway
// error - which keeps the HTTP status that names it.
MikaiApiError apiErrorFromHttp(const HttpResult& result) {
    ApiErrorEnvelope envelope;
    if (parseApiErrorEnvelope(result.body, result.status, &envelope)) {
        return MikaiApiError(envelope.code, envelope.message, envelope.status);
    }
    if (!result.error.isEmpty() && !isHttpStatusError(result.status)) {
        return MikaiApiError(QStringLiteral("network_error"), result.error, 0);
    }
    return MikaiApiError(QStringLiteral("http_error"), reasonFor(result.status), result.status);
}

}  // namespace

int RateLimiter::acquireSlot() {
    bool waiting = false;
    QMutexLocker locker(&mutex_);
    for (;;) {
        if (WorkCancellationScope::cancellationRequested()) {
            // No slot was taken, so no quota update is owed. Throwing is the
            // only honest answer: the caller asked for a slot that a shutdown
            // is no longer willing to wait for.
            if (waiting) {
                --waiters_;
            }
            throw MikaiApiError(QStringLiteral("cancelled"),
                                QStringLiteral("the request was cancelled before it was sent"), 0);
        }
        const double now = monotonicSeconds();
        if (resetAt_ != 0.0 && now >= resetAt_) {
            remaining_.reset();
            resetAt_ = 0.0;
        }
        const double resetWait = std::max(0.0, resetAt_ - now);
        const double retryWait = std::max(0.0, retryAt_ - now);
        const double quotaWait = (remaining_.has_value() && *remaining_ <= inFlight_) ? resetWait : 0.0;
        const double delay = std::max(retryWait, quotaWait);
        if (delay <= 0.0) {
            if (waiting) {
                --waiters_;
            }
            ++inFlight_;
            return generation_;
        }
        if (!waiting) {
            ++waiters_;
            waiting = true;
        }
        // The wait is sliced so a cancellation is observed within a few hundred
        // milliseconds even though the advertised window can be minutes long.
        // The mutex is held across the store, which is what makes the scope
        // observable exactly once rather than intermittently.
        const double windowMs = (delay + 0.05) * 1000.0;
        const int sliceMs = std::isfinite(windowMs)
                                ? static_cast<int>(std::clamp(
                                      windowMs, 1.0, static_cast<double>(kCancelSliceMs)))
                                : kCancelSliceMs;
        condition_.wait(&mutex_, QDeadlineTimer(sliceMs));
    }
}

void RateLimiter::finishCall(const QMap<QString, QString>& headers, int generation, int statusCode) {
    // One pass over the map, four values out. Cloudflare lower-cases every
    // header name on the way through, so a case-sensitive lookup would silently
    // lose the whole quota snapshot.
    std::optional<int> limit;
    std::optional<int> remaining;
    std::optional<int> reset;
    std::optional<int> retryAfter;
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        switch (classifyHeader(it.key().toLatin1())) {
            case QuotaHeader::kLimit:
                limit = quotaInt(it.value());
                break;
            case QuotaHeader::kRemaining:
                remaining = quotaInt(it.value());
                break;
            case QuotaHeader::kReset:
                reset = quotaInt(it.value());
                break;
            case QuotaHeader::kRetryAfter:
                retryAfter = quotaInt(it.value());
                break;
            case QuotaHeader::kNone:
                break;
        }
    }
    applyQuota(std::move(limit), std::move(remaining), std::move(reset), std::move(retryAfter),
               generation, statusCode);
}

void RateLimiter::applyQuota(std::optional<int> limit, std::optional<int> remaining,
                             std::optional<int> reset, std::optional<int> retryAfter, int generation,
                             int statusCode) {
    QMutexLocker locker(&mutex_);
    inFlight_ = std::max(0, inFlight_ - 1);
    if (generation != generation_) {
        // The key changed while this call was in flight: its headers describe
        // a window that no longer applies. The key change itself already woke
        // everyone through reset().
        condition_.wakeOne();
        return;
    }
    if (limit.has_value()) {
        limit_ = limit;
    }
    if (remaining.has_value()) {
        // Concurrent replies can arrive out of order, so keep the
        // conservative value until the advertised reset.
        if (resetAt_ != 0.0 && monotonicSeconds() < resetAt_) {
            remaining_ = remaining_.has_value() ? std::min(*remaining_, *remaining) : remaining;
        } else {
            remaining_ = remaining;
        }
    }
    if (reset.has_value()) {
        resetAt_ = monotonicSeconds() + *reset;
    }
    if (statusCode == 429) {
        retryAt_ = std::max(retryAt_, monotonicSeconds() + retryAfter.value_or(1));
    }
    // Wake only as many waiters as the quota can admit. Waking one is too few
    // (a reply can open the window for several callers, and the rest would sit
    // on their deadline for the whole window); waking everyone is too many
    // (every response stirred the whole queue and most re-parked). A waiter
    // re-polls on its own deadline regardless, so this is latency, not
    // correctness: an idle limiter wakes everyone, an exhausted one wakes
    // nobody, and a key change still broadcasts through reset().
    int admissible = waiters_;
    if (retryAt_ > monotonicSeconds()) {
        admissible = 0;
    } else if (remaining_.has_value()) {
        admissible = std::min(admissible, *remaining_ - inFlight_);
    }
    for (int i = 0; i < admissible; ++i) {
        condition_.wakeOne();
    }
}

RateLimitState RateLimiter::state(bool keyed) const {
    QMutexLocker locker(&mutex_);
    const double now = monotonicSeconds();
    if (resetAt_ != 0.0 && now >= resetAt_) {
        // The window rolled over and the server has not reported a new count.
        const_cast<RateLimiter*>(this)->remaining_.reset();
        const_cast<RateLimiter*>(this)->resetAt_ = 0.0;
    }
    RateLimitState snapshot;
    snapshot.limit = limit_.value_or(keyed ? kKeyedLimit : kAnonymousLimit);
    snapshot.remaining = remaining_;
    if (remaining_.has_value()) {
        snapshot.used = std::max(0, snapshot.limit - *remaining_);
    }
    snapshot.resetIn =
        resetAt_ != 0.0 ? static_cast<int>(std::max(0.0, std::floor(resetAt_ - now + 0.999))) : 0;
    snapshot.queued = waiters_;
    snapshot.inFlight = inFlight_;
    return snapshot;
}

int RateLimiter::generation() const {
    QMutexLocker locker(&mutex_);
    return generation_;
}

void RateLimiter::reset() {
    QMutexLocker locker(&mutex_);
    ++generation_;
    limit_.reset();
    remaining_.reset();
    resetAt_ = 0.0;
    retryAt_ = 0.0;
    // Every parked caller is released, so the next one out of the queue takes
    // the credential snapshot of the new epoch rather than continuing with the
    // header it captured before the key changed.
    condition_.wakeAll();
}

MikaiClient::MikaiClient(const QString& apiKey, const QString& baseUrl)
    : baseUrl_(baseUrl.isEmpty() ? kBaseUrl : baseUrl) {
    const QString explicitKey = apiKey.trimmed();
    if (!explicitKey.isEmpty()) {
        apiKey_ = explicitKey;
        return;
    }
    const QString fromEnv = qEnvironmentVariable("MIKAI_API_KEY").trimmed();
    apiKey_ = fromEnv.isEmpty() ? readApiKeyFromEnv() : fromEnv;
}

MikaiClient::~MikaiClient() = default;

QNetworkAccessManager* MikaiClient::managerForThread() const { return threadManager(); }

CredentialSlot MikaiClient::credentialSlot() const {
    // One lock for both fields: the key and the epoch it was captured for leave
    // this function together or not at all, so a caller that parks and resumes
    // can never send the old header under the new generation.
    QMutexLocker locker(&keyMutex_);
    CredentialSlot slot;
    slot.key = apiKey_;
    slot.generation = limiter_.generation();
    return slot;
}

QJsonObject MikaiClient::apiGet(const QString& url, const QMap<QString, QString>& params,
                                const QString& overrideKey, int timeoutMs) const {
    QUrl target(url);
    if (!params.isEmpty()) {
        QUrlQuery query(target.query());
        for (auto it = params.cbegin(); it != params.cend(); ++it) {
            query.addQueryItem(it.key(), it.value());
        }
        target.setQuery(query);
    }

    if (WorkCancellationScope::cancellationRequested()) {
        throw MikaiApiError(QStringLiteral("cancelled"),
                            QStringLiteral("the request was cancelled"), 0);
    }
    const bool cacheable = cacheableEndpoint(target, overrideKey);
    QByteArray key;
    QByteArray validator;
    QJsonObject revalidationBody;
    if (cacheable) {
        const CredentialSlot slot = credentialSlot();
        key = cacheKey(target, slot);
        JsonCache& cache = jsonCache();
        QMutexLocker lock(&cache.mutex);
        auto it = cache.entries.find(key);
        if (it != cache.entries.end()) {
            it->touched = ++cache.clock;
            if (it->freshUntil > monotonicSeconds()) return it->body;
            validator = it->etag;
            if (!validator.isEmpty()) revalidationBody = it->body;
        }
    }

    for (int attempt = 0;; ++attempt) {
        // The limiter grants the slot first and the credentials are read after
        // it: a caller released by a key change resumes with the key that is
        // current now, not with a header from the epoch it parked under.
        limiter_.acquireSlot();
        QNetworkRequest request(target);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::SameOriginRedirectPolicy);
        request.setRawHeader("Accept", "application/json");
        request.setRawHeader("User-Agent", apiUserAgent().toUtf8());
        // The credential snapshot carries the generation it was read at, and
        // this is the generation the reply is charged to. A caller released by a
        // key change reads the new key and the new epoch together here; a reply
        // from a superseded epoch takes no quota from the window it does not
        // describe. An explicit override key is a caller-supplied credential with
        // no epoch of its own, so it stays with the epoch the limiter granted.
        const CredentialSlot slot =
            overrideKey.isEmpty()
                ? credentialSlot()
                : CredentialSlot{overrideKey, limiter_.generation()};
        if (cacheable) {
            const QByteArray actualKey = cacheKey(target, slot);
            if (actualKey != key) {
                key = actualKey;
                validator.clear();
                revalidationBody = {};
            }
        }
        if (!validator.isEmpty()) request.setRawHeader("If-None-Match", validator);
        if (!slot.key.isEmpty()) {
            request.setRawHeader(kApiHeader, slot.key.toUtf8());
        }

        const HttpResult result = runBlocking(managerForThread(), request, timeoutMs);
        const int status = result.status;
        limiter_.applyQuota(result.quota.limit, result.quota.remaining, result.quota.reset,
                            result.quota.retryAfter, slot.generation, status);
        if (result.cancelled) {
            throw MikaiApiError(QStringLiteral("cancelled"),
                                QStringLiteral("the request was cancelled"), 0);
        }
        if (status == 429 && attempt < kMaxRateLimitRetries) {
            continue;  // the limiter parked us; the same request may now succeed
        }
        if (status == 304 && cacheable && !key.isEmpty() && !validator.isEmpty()) {
            JsonCache& cache = jsonCache();
            QMutexLocker lock(&cache.mutex);
            auto it = cache.entries.find(key);
            // A different request can evict or replace this entry while the
            // validator is in flight. A 304 describes the body that supplied
            // that validator, never a later entry found under the same key.
            if (it != cache.entries.end() && it->etag == validator) {
                if (result.cacheControl.contains(QStringLiteral("no-store"), Qt::CaseInsensitive)) {
                    cache.bytes -= it->bytes;
                    cache.entries.erase(it);
                } else {
                    if (!result.etag.isEmpty()) it->etag = result.etag;
                    it->freshUntil = monotonicSeconds() + cacheMaxAge(result.cacheControl);
                    it->touched = ++cache.clock;
                }
            }
            return revalidationBody;
        }
        if (!result.error.isEmpty() && !isHttpStatusError(status)) {
            throw MikaiApiError(QStringLiteral("network_error"), result.error, 0);
        }
        if (isHttpStatusError(status)) {
            // The envelope is decoded before the transport classification: a
            // documented API failure keeps its code, message and status.
            throw apiErrorFromHttp(result);
        }
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(result.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            throw MikaiApiError(QStringLiteral("http_error"), reasonFor(status), status);
        }
        const QJsonObject object = document.object();
        if (cacheable && !key.isEmpty()) {
            JsonCache& cache = jsonCache();
            QMutexLocker lock(&cache.mutex);
            const QString control = result.cacheControl;
            auto old = cache.entries.find(key);
            const bool noStore = control.contains(QStringLiteral("no-store"), Qt::CaseInsensitive);
            const bool success = object.value(QStringLiteral("ok")).toBool();
            if (success && noStore) {
                if (old != cache.entries.end()) {
                    cache.bytes -= old->bytes;
                    cache.entries.erase(old);
                }
            } else if (success) {
                const qsizetype bytes = document.toJson(QJsonDocument::Compact).size();
                if (bytes <= kMaxCacheBytes) {
                    if (old != cache.entries.end()) cache.bytes -= old->bytes;
                    CachedJson entry;
                    entry.body = object;
                    entry.etag = result.etag;
                    entry.bytes = bytes;
                    entry.freshUntil = monotonicSeconds() + cacheMaxAge(control);
                    entry.touched = ++cache.clock;
                    cache.entries.insert(key, entry);
                    cache.bytes += bytes;
                    pruneCache(cache);
                }
            }
        }
        return object;
    }
}

QJsonObject MikaiClient::get(const QString& path, const QMap<QString, QString>& params) const {
    const QJsonObject payload = apiGet(baseUrl_ + path, params);
    if (!payload.value(QStringLiteral("ok")).toBool()) {
        const QJsonObject error = payload.value(QStringLiteral("error")).toObject();
        const QJsonValue code = error.value(QStringLiteral("code"));
        const QJsonValue message = error.value(QStringLiteral("message"));
        throw MikaiApiError(code.isString() ? code.toString() : QStringLiteral("unknown"),
                            message.isString() ? message.toString() : QStringLiteral("request failed"),
                            payload.value(QStringLiteral("error")).toObject()
                                .value(QStringLiteral("errorCode"))
                                .toInt(0));
    }
    return payload;
}

SearchResult MikaiClient::search(const AnimeQuery& query) const {
    QMap<QString, QString> params;
    params.insert(QStringLiteral("page"), QString::number(query.page));
    params.insert(QStringLiteral("limit"), QString::number(query.limit));
    const auto csv = [](const auto& values) {
        QStringList items;
        items.reserve(values.size());
        for (const auto& value : values) items.append(QString::number(value));
        return items.join(QLatin1Char(','));
    };
    const auto addList = [&params](const QString& name, const QStringList& values) {
        if (!values.isEmpty()) params.insert(name, values.join(QLatin1Char(',')));
    };
    if (!query.search.trimmed().isEmpty()) params.insert(QStringLiteral("search"), query.search.trimmed());
    addList(QStringLiteral("genres"), query.genres);
    addList(QStringLiteral("genresExclude"), query.genres_exclude);
    addList(QStringLiteral("tags"), query.tags);
    addList(QStringLiteral("tagsExclude"), query.tags_exclude);
    addList(QStringLiteral("seasons"), query.seasons);
    addList(QStringLiteral("formats"), query.formats);
    addList(QStringLiteral("statuses"), query.statuses);
    addList(QStringLiteral("studios"), query.studios);
    if (!query.years.isEmpty()) params.insert(QStringLiteral("years"), csv(query.years));
    if (query.year_from) params.insert(QStringLiteral("yearFrom"), QString::number(*query.year_from));
    if (query.year_to) params.insert(QStringLiteral("yearTo"), QString::number(*query.year_to));
    if (!query.teams.isEmpty()) params.insert(QStringLiteral("teams"), csv(query.teams));
    params.insert(QStringLiteral("genresMatch"), query.genres_match);
    params.insert(QStringLiteral("tagsMatch"), query.tags_match);
    params.insert(QStringLiteral("sort"), query.sort);
    if (!query.order.isEmpty()) params.insert(QStringLiteral("order"), query.order);
    return searchFromPayload(get(QStringLiteral("/anime"), params), query.page);
}

ApiMeta MikaiClient::getMeta() const {
    const QJsonObject payload = get(QStringLiteral("/meta"));
    const QJsonObject result = payload.value(QStringLiteral("result")).toObject();
    const auto strings = [](const QJsonValue& value) {
        QStringList values;
        if (value.isArray()) {
            for (const QJsonValue& item : value.toArray()) {
                if (item.isString()) values.append(item.toString());
            }
        }
        return values;
    };
    ApiMeta meta;
    meta.version = result.value(QStringLiteral("version")).toString();
    meta.formats = strings(result.value(QStringLiteral("formats")));
    meta.statuses = strings(result.value(QStringLiteral("statuses")));
    meta.seasons = strings(result.value(QStringLiteral("seasons")));
    meta.anime_sort_fields = strings(result.value(QStringLiteral("animeSortFields")));
    meta.sort_orders = strings(result.value(QStringLiteral("sortOrders")));
    meta.match_modes = strings(result.value(QStringLiteral("matchModes")));
    return meta;
}

QVector<GenreTag> MikaiClient::getGenres() const {
    QVector<GenreTag> values;
    const QJsonArray array = get(QStringLiteral("/genres")).value(QStringLiteral("result")).toArray();
    values.reserve(array.size());
    for (const QJsonValue& raw : array) {
        const QJsonObject object = raw.toObject();
        values.append(GenreTag{object.value(QStringLiteral("name")).toString(),
                               object.value(QStringLiteral("ua")).toString()});
    }
    return values;
}

QVector<GenreTag> MikaiClient::getTags() const {
    QVector<GenreTag> values;
    const QJsonArray array = get(QStringLiteral("/tags")).value(QStringLiteral("result")).toArray();
    values.reserve(array.size());
    for (const QJsonValue& raw : array) {
        const QJsonObject object = raw.toObject();
        values.append(GenreTag{object.value(QStringLiteral("name")).toString(),
                               object.value(QStringLiteral("ua")).toString()});
    }
    return values;
}

QStringList MikaiClient::getStudios() const {
    QStringList values;
    const QJsonArray array = get(QStringLiteral("/studios")).value(QStringLiteral("result")).toArray();
    values.reserve(array.size());
    for (const QJsonValue& raw : array) {
        if (raw.isString()) values.append(raw.toString());
        else if (raw.isObject()) values.append(raw.toObject().value(QStringLiteral("name")).toString());
    }
    return values;
}

TeamsResult MikaiClient::getTeams(const QString& search, int page, int limit) const {
    QMap<QString, QString> params{{QStringLiteral("page"), QString::number(page)},
                                  {QStringLiteral("limit"), QString::number(limit)}};
    if (!search.trimmed().isEmpty()) params.insert(QStringLiteral("search"), search.trimmed());
    const QJsonObject payload = get(QStringLiteral("/teams"), params);
    TeamsResult result;
    if (payload.value(QStringLiteral("total")).isDouble()) result.total = payload.value(QStringLiteral("total")).toInt();
    if (payload.value(QStringLiteral("page")).isDouble()) result.page = payload.value(QStringLiteral("page")).toInt();
    if (payload.value(QStringLiteral("pages")).isDouble()) result.pages = payload.value(QStringLiteral("pages")).toInt();
    const QJsonArray items = payload.value(QStringLiteral("result")).toArray();
    result.items.reserve(items.size());
    for (const QJsonValue& raw : items) result.items.append(parseTeamProfile(raw.toObject()));
    return result;
}

TeamProfile MikaiClient::getTeam(const QString& ref) const {
    return parseTeamProfile(get(QStringLiteral("/teams/") + ref)
                                .value(QStringLiteral("result")).toObject());
}

SearchResult MikaiClient::getTeamAnime(const QString& ref, int page, int limit) const {
    QMap<QString, QString> params{{QStringLiteral("page"), QString::number(page)},
                                  {QStringLiteral("limit"), QString::number(limit)}};
    return searchFromPayload(get(QStringLiteral("/teams/") + ref + QStringLiteral("/anime"), params), page);
}

SearchResult MikaiClient::browseOngoing(int page, int limit) const {
    AnimeQuery query;
    query.statuses = {QStringLiteral("ongoing")};
    query.sort = QStringLiteral("updated");
    query.order = QStringLiteral("desc");
    query.page = page;
    query.limit = limit;
    return search(query);
}

SearchResult MikaiClient::browsePopular(int page, int limit) const {
    AnimeQuery query;
    query.sort = QStringLiteral("views");
    query.order = QStringLiteral("desc");
    query.page = page;
    query.limit = limit;
    return search(query);
}

UpdatesResult MikaiClient::getUpdates(int page, int limit, const QString& since) const {
    QMap<QString, QString> params;
    params.insert(QStringLiteral("page"), QString::number(page));
    params.insert(QStringLiteral("limit"), QString::number(limit));
    if (!since.isEmpty()) params.insert(QStringLiteral("since"), since);
    const QJsonObject payload = get(QStringLiteral("/updates"), params);
    UpdatesResult result;
    const QJsonValue total = payload.value(QStringLiteral("total"));
    if (total.isDouble()) {
        result.total = static_cast<int>(total.toDouble());
    }
    if (payload.value(QStringLiteral("page")).isDouble()) result.page = payload.value(QStringLiteral("page")).toInt();
    if (payload.value(QStringLiteral("pages")).isDouble()) result.pages = payload.value(QStringLiteral("pages")).toInt();
    const QJsonArray items = payload.value(QStringLiteral("result")).toArray();
    result.items.reserve(items.size());
    for (const QJsonValue& value : items) {
        result.items.append(parseUpdate(value.toObject()));
    }
    return result;
}

ScheduleResult MikaiClient::getSchedule() const {
    const QJsonObject payload =
        get(QStringLiteral("/schedule")).value(QStringLiteral("result")).toObject();
    ScheduleResult schedule;
    for (const QString& day : weekdayOrder()) {
        const QJsonArray entries = payload.value(day).toArray();
        QVector<AiringEntry> parsed;
        parsed.reserve(entries.size());
        for (const QJsonValue& value : entries) {
            parsed.append(parseAiringEntry(value.toObject()));
        }
        schedule.days.insert(day, parsed);
    }
    return schedule;
}

AnimeDetail MikaiClient::getAnime(const QString& ref) const {
    return parseDetail(get(QStringLiteral("/anime/") + ref).value(QStringLiteral("result")).toObject());
}

PlayerResult MikaiClient::getPlayer(const QString& ref) const {
    return parsePlayer(get(QStringLiteral("/anime/") + ref + QStringLiteral("/player"))
                           .value(QStringLiteral("result"))
                           .toObject());
}

QByteArray MikaiClient::fetchBytes(
    const QString& url, int timeoutMs,
    const std::function<void(qint64, qint64)>& progress) const {
    QNetworkRequest request{QUrl(url)};
    request.setRawHeader("User-Agent", apiUserAgent().toUtf8());
    const int timeout = timeoutMs > 0 ? timeoutMs : kArtworkTimeoutMs;
    const HttpResult result = runBlocking(managerForThread(), request, timeout, progress);
    if (result.cancelled) {
        throw MikaiApiError(QStringLiteral("cancelled"), QStringLiteral("the request was cancelled"),
                            0);
    }
    if (!result.error.isEmpty() && !isHttpStatusError(result.status)) {
        throw MikaiApiError(QStringLiteral("network_error"), result.error, 0);
    }
    if (isHttpStatusError(result.status)) {
        // Artwork mirrors answer with a bare status, but an API-shaped envelope
        // would still carry the documented code and message, so it is decoded
        // here rather than discarded.
        throw apiErrorFromHttp(result);
    }
    return result.body;
}

QString MikaiClient::apiKey() const {
    QMutexLocker locker(&keyMutex_);
    return apiKey_;
}

ApiKeySaveResult MikaiClient::setApiKey(const QString& apiKey) {
    const QString candidate = apiKey.trimmed();
    // The file is written and committed before anything visible changes: a read
    // only directory, a full disk or an unavailable parent must leave the
    // previous key on disk and the active key in memory exactly as they were.
    QString reason;
    if (!writeEnvFile(envFilePath(), candidate, &reason)) {
        return ApiKeySaveResult{false, reason};
    }
    {
        // Both halves happen under one lock, in this order, so no caller can
        // ever observe the new key paired with the old generation or the new
        // generation paired with the old key. Releasing the parked callers is
        // part of the same step: they wake up, take the snapshot that is
        // current now, and send with the key this save just published.
        QMutexLocker locker(&keyMutex_);
        apiKey_ = candidate;
        limiter_.reset();
    }
    return ApiKeySaveResult{true, {}};
}

QMap<QString, QVariant> MikaiClient::validateApiKey(const QString& candidateKey) const {
    const QString key = candidateKey.trimmed();
    QNetworkRequest request{QUrl(baseUrl_ + QStringLiteral("/me"))};
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", apiUserAgent().toUtf8());
    if (!key.isEmpty()) {
        request.setRawHeader(kApiHeader, key.toUtf8());
    }

    HttpResult result;
    try {
        const CredentialSlot saved = credentialSlot();
        if (!key.isEmpty() && key == saved.key) {
            // Validation tests the fixed candidate, not whichever key becomes
            // current while it parks. Its quota must therefore stay with the
            // candidate's original epoch even if Save releases this waiter.
            limiter_.acquireSlot();
            result = runBlocking(managerForThread(), request, kValidateTimeoutMs);
            const int status = result.status;
            limiter_.applyQuota(result.quota.limit, result.quota.remaining, result.quota.reset,
                                result.quota.retryAfter, saved.generation, status);
            if (result.cancelled) {
                throw MikaiApiError(QStringLiteral("cancelled"),
                                    QStringLiteral("the request was cancelled"), 0);
            }
        } else {
            // A candidate key, anonymous access included, has its own quota:
            // keep its response out of the saved key's counter.
            result = runBlocking(managerForThread(), request, kValidateTimeoutMs);
            if (result.cancelled) {
                throw MikaiApiError(QStringLiteral("cancelled"),
                                    QStringLiteral("the request was cancelled"), 0);
            }
        }
        if (!result.error.isEmpty() && !isHttpStatusError(result.status)) {
            throw MikaiApiError(QStringLiteral("network_error"), result.error, 0);
        }
        if (isHttpStatusError(result.status)) {
            // A disabled or invalid key is an HTTP answer first, and its
            // envelope is the only thing that says which of the two it is.
            const MikaiApiError http = apiErrorFromHttp(result);
            QMap<QString, QVariant> refused;
            refused.insert(QStringLiteral("valid"), false);
            refused.insert(QStringLiteral("error_code"), http.code);
            refused.insert(QStringLiteral("error"), http.message);
            return refused;
        }
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(result.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            throw MikaiApiError(QStringLiteral("http_error"), QStringLiteral("non-JSON response"),
                                result.status);
        }

        const QJsonObject data = document.object();
        QMap<QString, QVariant> out;
        if (data.value(QStringLiteral("ok")).toBool()) {
            const QJsonObject payload = data.value(QStringLiteral("result")).toObject();
            out.insert(QStringLiteral("valid"), true);
            out.insert(QStringLiteral("authenticated"),
                       payload.value(QStringLiteral("authenticated")).toBool());
            out.insert(QStringLiteral("rate_limit"),
                       payload.value(QStringLiteral("rateLimitPerMin")).toInt(kAnonymousLimit));
            out.insert(QStringLiteral("name"), payload.value(QStringLiteral("name")).toString());
        } else {
            const QJsonObject error = data.value(QStringLiteral("error")).toObject();
            out.insert(QStringLiteral("valid"), false);
            out.insert(QStringLiteral("error_code"),
                       error.value(QStringLiteral("code")).toString(QStringLiteral("unknown")));
            out.insert(QStringLiteral("error"), error.value(QStringLiteral("message"))
                                                    .toString(QStringLiteral("invalid key")));
        }
        return out;
    } catch (const MikaiApiError& error) {
        QMap<QString, QVariant> out;
        out.insert(QStringLiteral("valid"), false);
        out.insert(QStringLiteral("error_code"), error.code);
        out.insert(QStringLiteral("error"), error.message);
        return out;
    }
}

RateLimitState MikaiClient::rateLimitState() const {
    QMutexLocker locker(&keyMutex_);
    return limiter_.state(!apiKey_.isEmpty());
}

}  // namespace mikai::backend
