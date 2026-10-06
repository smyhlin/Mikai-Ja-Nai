#include "MikaiClient.hpp"

#include <QCoreApplication>
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
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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

// The .env the Python client wrote next to the repo root. The C++ build has
// no __file__, so walk up from the executable until a .env shows up and fall
// back to the current working directory.
QString envFilePath() {
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
};

HttpResult runBlocking(QNetworkAccessManager* manager, const QNetworkRequest& request, int timeoutMs) {
    HttpResult result;
    QNetworkReply* reply = manager->get(request);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    if (reply->isRunning()) {
        // The deadline fired first: abort so the reply cannot outlive us. The
        // drain is on the same timer rather than an unbounded wait, because an
        // abort can complete late and this thread belongs to a pool.
        reply->abort();
        timer.start(kAbortDrainMs);
        loop.exec();
    }
    timer.stop();

    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto pairs = reply->rawHeaderPairs();
    for (const auto& pair : pairs) {
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
    if (reply->error() != QNetworkReply::NoError) {
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

const QStringList& weekdayOrder() {
    static const QStringList days{QStringLiteral("monday"),   QStringLiteral("tuesday"),
                                  QStringLiteral("wednesday"), QStringLiteral("thursday"),
                                  QStringLiteral("friday"),   QStringLiteral("saturday"),
                                  QStringLiteral("sunday")};
    return days;
}

}  // namespace

int RateLimiter::acquireSlot() {
    bool waiting = false;
    QMutexLocker locker(&mutex_);
    for (;;) {
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
        condition_.wait(&mutex_, QDeadlineTimer(static_cast<int>((delay + 0.05) * 1000.0)));
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

void RateLimiter::reset() {
    QMutexLocker locker(&mutex_);
    ++generation_;
    limit_.reset();
    remaining_.reset();
    resetAt_ = 0.0;
    retryAt_ = 0.0;
    condition_.wakeAll();
}

MikaiClient::MikaiClient(const QString& apiKey) {
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

    QNetworkRequest request(target);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", kApiUserAgent.toUtf8());
    const QString key = overrideKey.isEmpty() ? apiKey_ : overrideKey;
    if (!key.isEmpty()) {
        request.setRawHeader(kApiHeader, key.toUtf8());
    }

    for (int attempt = 0;; ++attempt) {
        const int generation = limiter_.acquireSlot();
        const HttpResult result = runBlocking(managerForThread(), request, timeoutMs);
        const int status = result.status;
        limiter_.applyQuota(result.quota.limit, result.quota.remaining, result.quota.reset,
                            result.quota.retryAfter, generation, status);
        if (status == 429 && attempt < kMaxRateLimitRetries) {
            continue;  // the limiter parked us; the same request may now succeed
        }
        if (!result.error.isEmpty()) {
            throw MikaiApiError(QStringLiteral("network_error"), result.error, 0);
        }
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(result.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            throw MikaiApiError(QStringLiteral("http_error"), reasonFor(status), status);
        }
        return document.object();
    }
}

QJsonObject MikaiClient::get(const QString& path, const QMap<QString, QString>& params) const {
    const QJsonObject payload = apiGet(kBaseUrl + path, params);
    if (!payload.value(QStringLiteral("ok")).toBool()) {
        const QJsonObject error = payload.value(QStringLiteral("error")).toObject();
        const QJsonValue code = error.value(QStringLiteral("code"));
        const QJsonValue message = error.value(QStringLiteral("message"));
        throw MikaiApiError(code.isString() ? code.toString() : QStringLiteral("unknown"),
                            message.isString() ? message.toString() : QStringLiteral("request failed"),
                            0);
    }
    return payload;
}

SearchResult MikaiClient::search(const QString& query, int page, int limit) const {
    QMap<QString, QString> params;
    params.insert(QStringLiteral("page"), QString::number(page));
    params.insert(QStringLiteral("limit"), QString::number(limit));
    const QString trimmed = query.trimmed();
    if (!trimmed.isEmpty()) {
        params.insert(QStringLiteral("search"), trimmed);
    }
    return searchFromPayload(get(QStringLiteral("/anime"), params), page);
}

SearchResult MikaiClient::browseOngoing(int page, int limit) const {
    QMap<QString, QString> params;
    params.insert(QStringLiteral("statuses"), QStringLiteral("ongoing"));
    params.insert(QStringLiteral("sort"), QStringLiteral("updated"));
    params.insert(QStringLiteral("order"), QStringLiteral("desc"));
    params.insert(QStringLiteral("page"), QString::number(page));
    params.insert(QStringLiteral("limit"), QString::number(limit));
    return searchFromPayload(get(QStringLiteral("/anime"), params), page);
}

SearchResult MikaiClient::browsePopular(int page, int limit) const {
    QMap<QString, QString> params;
    params.insert(QStringLiteral("sort"), QStringLiteral("views"));
    params.insert(QStringLiteral("order"), QStringLiteral("desc"));
    params.insert(QStringLiteral("page"), QString::number(page));
    params.insert(QStringLiteral("limit"), QString::number(limit));
    return searchFromPayload(get(QStringLiteral("/anime"), params), page);
}

UpdatesResult MikaiClient::getUpdates(int page, int limit) const {
    QMap<QString, QString> params;
    params.insert(QStringLiteral("page"), QString::number(page));
    params.insert(QStringLiteral("limit"), QString::number(limit));
    const QJsonObject payload = get(QStringLiteral("/updates"), params);
    UpdatesResult result;
    const QJsonValue total = payload.value(QStringLiteral("total"));
    if (total.isDouble()) {
        result.total = static_cast<int>(total.toDouble());
    }
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

QByteArray MikaiClient::fetchBytes(const QString& url, int timeoutMs) const {
    QNetworkRequest request{QUrl(url)};
    request.setRawHeader("User-Agent", kApiUserAgent.toUtf8());
    const int timeout = timeoutMs > 0 ? timeoutMs : kArtworkTimeoutMs;
    const HttpResult result = runBlocking(managerForThread(), request, timeout);
    if (!result.error.isEmpty() || result.status >= 400) {
        throw MikaiApiError(QStringLiteral("network_error"),
                            result.error.isEmpty() ? reasonFor(result.status) : result.error,
                            result.status);
    }
    return result.body;
}

QString MikaiClient::apiKey() const { return apiKey_; }

void MikaiClient::setApiKey(const QString& apiKey) {
    apiKey_ = apiKey.trimmed();
    QFile file(envFilePath());
    if (!apiKey_.isEmpty()) {
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            file.write(QStringLiteral("X-API-Key: %1\n").arg(apiKey_).toUtf8());
        }
    } else if (file.exists()) {
        file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        file.close();
    }
    limiter_.reset();
}

QMap<QString, QVariant> MikaiClient::validateApiKey(const QString& apiKey) const {
    const QString key = apiKey.trimmed();
    QNetworkRequest request{QUrl(kBaseUrl + QStringLiteral("/me"))};
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", kApiUserAgent.toUtf8());
    if (!key.isEmpty()) {
        request.setRawHeader(kApiHeader, key.toUtf8());
    }

    HttpResult result;
    try {
        if (!key.isEmpty() && key == apiKey_) {
            // The saved key: route it through the limiter so its own quota
            // stays honest.
            const int generation = limiter_.acquireSlot();
            result = runBlocking(managerForThread(), request, kValidateTimeoutMs);
            const int status = result.status;
            limiter_.applyQuota(result.quota.limit, result.quota.remaining, result.quota.reset,
                                result.quota.retryAfter, generation, status);
        } else {
            // A candidate key, anonymous access included, has its own quota:
            // keep its response out of the saved key's counter.
            result = runBlocking(managerForThread(), request, kValidateTimeoutMs);
        }
        if (!result.error.isEmpty()) {
            throw MikaiApiError(QStringLiteral("network_error"), result.error, result.status);
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

RateLimitState MikaiClient::rateLimitState() const { return limiter_.state(!apiKey_.isEmpty()); }

}  // namespace mikai::backend
