#pragma once

#include <QByteArray>
#include <QMap>
#include <QMutex>
#include <QString>
#include <QVariant>
#include <QWaitCondition>

#include <optional>
#include <functional>

#include "Cancellation.hpp"
#include "Errors.hpp"
#include "Models.hpp"

class QNetworkAccessManager;

namespace mikai::backend {

// One association of a credential snapshot with the rate-limit generation it
// was captured for. `apiGet()` takes this after the limiter grants a slot and
// not before: a key change while the caller was parked must not let the request
// keep a header from a superseded epoch, and must not let a reply from a
// superseded epoch describe the new key's window either.
struct CredentialSlot {
    QString key;
    int generation = 0;
};

// The .env the Python client wrote next to the repo root. The C++ build has no
// __file__, so the search walks up from the executable until a .env shows up
// and falls back to the current working directory. A caller that must never
// touch whatever file that search would find - a test, a probe - installs an
// override instead; an empty override restores the search.
QString envFilePath();
void setEnvFilePathOverride(const QString& path);

// The outcome of persisting a key. The .env is replaced through a QSaveFile,
// which writes a sibling temporary and renames it into place, so a failed save
// leaves the previous file exactly as it was and the active key untouched.
struct ApiKeySaveResult {
    bool ok = false;
    QString reason;
};

inline const QString kApiApplicationName = QStringLiteral("Mikai Wrapper");
inline const QString kApiApplicationVersion = QStringLiteral("0.1.0");

QString apiUserAgent();

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

    // Grants a slot and returns the generation it belongs to. Throws
    // MikaiApiError("cancelled") when the calling thread's cancellation scope
    // is set, so a shutdown does not wait out an advertised window.
    int acquireSlot();
    // Takes the whole header map and pulls the four quota values out of it in a
    // single pass.
    void finishCall(const QMap<QString, QString>& headers, int generation, int statusCode = 0);
    RateLimitState state(bool keyed) const;

    // The current epoch. Every acquired slot carries it, and every quota update
    // arriving with a different one is discarded: it describes a window that no
    // longer belongs to this key.
    int generation() const;

    // Called after the key changes so the next caller starts a fresh window.
    // This also releases every parked caller, which re-reads its credentials
    // rather than continuing with a header from the superseded epoch.
    void reset();

    // The request path already has the four values in hand, so it applies them
    // without re-walking a header map nobody built for it. MikaiClient uses this
    // on the request path; finishCall is the same thing spelled for a caller
    // that only has the raw header map.
    void applyQuota(std::optional<int> limit, std::optional<int> remaining,
                    std::optional<int> reset, std::optional<int> retryAfter, int generation,
                    int statusCode);

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
    explicit MikaiClient(const QString& apiKey = {}, const QString& baseUrl = {});
    ~MikaiClient();
    MikaiClient(const MikaiClient&) = delete;
    MikaiClient& operator=(const MikaiClient&) = delete;

    SearchResult search(const AnimeQuery& query) const;
    ApiMeta getMeta() const;
    QVector<GenreTag> getGenres() const;
    QVector<GenreTag> getTags() const;
    QStringList getStudios() const;
    TeamsResult getTeams(const QString& search = {}, int page = 1, int limit = 50) const;
    TeamProfile getTeam(const QString& ref) const;
    SearchResult getTeamAnime(const QString& ref, int page = 1, int limit = 20) const;
    SearchResult browseOngoing(int page = 1, int limit = 24) const;
    SearchResult browsePopular(int page = 1, int limit = 18) const;
    UpdatesResult getUpdates(int page = 1, int limit = 12, const QString& since = {}) const;
    ScheduleResult getSchedule() const;
    AnimeDetail getAnime(const QString& ref) const;
    PlayerResult getPlayer(const QString& ref) const;

    // Artwork download: bypasses the rate limiter, 20s timeout by default. This
    // is the one transport callers use for large non-artwork payloads (a model
    // graph, for instance, needs far more than the artwork budget); a negative
    // timeout keeps the artwork default.
    QByteArray fetchBytes(
        const QString& url, int timeoutMs = -1,
        const std::function<void(qint64 received, qint64 total)>& progress = {}) const;

    QString baseUrl() const { return baseUrl_; }

    // The credential snapshot, safe to read from any thread.
    QString apiKey() const;

    // Persists the key, then publishes it. The .env is replaced through a
    // QSaveFile and committed before anything visible changes: a write that
    // fails leaves the previous file intact and the active key as it was, and
    // reports why rather than pretending the save happened.
    ApiKeySaveResult setApiKey(const QString& apiKey);

    // Returns {valid, authenticated, rate_limit, name, error}.
    QMap<QString, QVariant> validateApiKey(const QString& apiKey) const;

    RateLimitState rateLimitState() const;

private:
    QNetworkAccessManager* managerForThread() const;
    QJsonObject get(const QString& path, const QMap<QString, QString>& params = {}) const;
    QJsonObject apiGet(const QString& url, const QMap<QString, QString>& params = {},
                       const QString& overrideKey = {}, int timeoutMs = 15000) const;

    // The credential snapshot and the limiter generation it was captured for,
    // read as one unit so a key change cannot hand a request the new epoch with
    // the old header.
    CredentialSlot credentialSlot() const;

    const QString baseUrl_;
    mutable QMutex keyMutex_;
    mutable QString apiKey_;
    mutable RateLimiter limiter_;
};

}  // namespace mikai::backend
