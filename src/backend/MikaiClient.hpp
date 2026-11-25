#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QString>
#include <QVariant>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>

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
// __file__, so the search checks the executable's directory and its immediate
// parent; anything further above an installed binary is deliberately not
// adopted. The current working directory is deliberately NOT searched (P0-03):
// it depends on how the app was launched and a planted .env there would supply
// the API key. A caller that must never touch whatever file that search would
// find - a test, a probe - installs an override instead; an empty override
// restores the search.
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
inline const QString kApiApplicationVersion = QStringLiteral("0.2.4.2");
inline constexpr int kApiAnonymousRequestsPerMinute = 300;
inline constexpr int kApiKeyedRequestsPerMinute = 600;

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
    // T-415: forgets an advertised window that has rolled over. A mutator
    // concern: the const state() snapshot presents the same view without
    // writing.
    void forgetElapsedWindow();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
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
    TeamsResult getTeams(const QString& search = {}, int page = 1, int limit = 50,
                         std::optional<bool> haveVoice = std::nullopt,
                         std::optional<bool> haveSubs = std::nullopt,
                         const QString& sort = {}, const QString& order = {}) const;
    TeamProfile getTeam(const QString& ref) const;
    SearchResult getTeamAnime(const QString& ref, int page = 1, int limit = 20) const;
    SearchResult browseOngoing(int page = 1, int limit = 24) const;
    SearchResult browsePopular(int page = 1, int limit = 18) const;
    UpdatesResult getUpdates(int page = 1, int limit = 12, const QString& since = {},
                             const QString& order = {}) const;
    ScheduleResult getSchedule() const;
    AnimeDetail getAnime(const QString& ref, const QStringList& include = {}) const;
    PlayerResult getPlayer(const QString& ref) const;

    // API v1.5 Tier 1 read-only endpoints.
    QJsonObject getHealth() const;
    AnimeDetail getRandomAnime(const QStringList& genres = {},
                               const QStringList& formats = {}) const;
    QVector<AnimeDetail> getAnimeBatch(const QStringList& refs,
                                       const QStringList& include = {}) const;
    QJsonObject resolveAnime(const QStringList& refs) const;
    QJsonObject getLicenses(int page = 1, int limit = 20, const QString& since = {},
                            const QString& holder = {}, const QString& platform = {}) const;
    QJsonArray getLicenseHolders() const;
    QVector<UaPoster> getAnimePosters(const QString& ref) const;
    QJsonObject getUaPosters(int page = 1, int limit = 20, const QString& author = {},
                             std::optional<qint64> teamId = std::nullopt) const;

    // API v1.5 Tier 2 account (/me/*) & device-link authentication (/auth/device*).
    QJsonObject startDeviceAuth(const QString& clientName = {}) const;
    QJsonObject pollDeviceAuthToken(const QString& deviceCode) const;
    QJsonObject cancelDeviceAuth(const QString& deviceCode) const;
    QJsonObject revokeMyApiKey() const;

    QJsonObject getMyList(const QString& status = {}, int page = 1, int limit = 20,
                          const QString& sort = {}, const QString& order = {}) const;
    QJsonObject getMyListEntry(const QString& ref) const;
    QJsonObject putMyListEntry(const QString& ref, const QJsonObject& entry) const;
    QJsonObject deleteMyListEntry(const QString& ref) const;
    QJsonObject addMyListRewatch(const QString& ref, const QJsonObject& payload = {}) const;
    QJsonObject deleteMyListRewatch(const QString& ref) const;

    QJsonObject getMyHistory(int page = 1, int limit = 20) const;
    QJsonObject getMySubscriptions(int page = 1, int limit = 20) const;
    QJsonObject getMySubscription(const QString& ref) const;
    QJsonObject putMySubscription(const QString& ref, const QJsonObject& payload = {}) const;
    QJsonObject deleteMySubscription(const QString& ref) const;
    QJsonObject getMyNotifications(int page = 1, int limit = 20, bool unreadOnly = false) const;
    QJsonObject markMyNotificationsRead(const QJsonObject& payload = {}) const;

    QJsonObject getMyCollections(int page = 1, int limit = 20) const;
    QJsonObject createMyCollection(const QJsonObject& payload) const;
    QJsonObject getMySavedCollections(int page = 1, int limit = 20) const;
    QJsonObject getMyCollection(const QString& id) const;
    QJsonObject updateMyCollection(const QString& id, const QJsonObject& payload) const;
    QJsonObject deleteMyCollection(const QString& id) const;
    QJsonObject getMyCollectionAnime(const QString& id, int page = 1, int limit = 20) const;
    QJsonObject addAnimeToMyCollection(const QString& id, const QString& ref,
                                       const QJsonObject& payload = {}) const;
    QJsonObject removeAnimeFromMyCollection(const QString& id, const QString& ref) const;
    QJsonObject reorderMyCollection(const QString& id, const QJsonObject& payload) const;

    // API v1.5 Tier 3 read-only endpoints (characters, people, collections, comments, blog, translit).
    QJsonObject getCharacters(const QString& search = {}, int page = 1, int limit = 20) const;
    QJsonObject getCharacter(const QString& ref) const;
    QJsonObject getCharacterAnime(const QString& ref, int page = 1, int limit = 20) const;
    QJsonObject getCharacterVoices(const QString& ref, int page = 1, int limit = 20) const;
    QJsonObject getPeople(const QString& search = {}, int page = 1, int limit = 20) const;
    QJsonObject getPerson(const QString& ref) const;
    QJsonObject getPersonAnime(const QString& ref, int page = 1, int limit = 20) const;
    QJsonObject getPersonVoices(const QString& ref, int page = 1, int limit = 20) const;
    QJsonObject getPersonStaff(const QString& ref, int page = 1, int limit = 20) const;
    QJsonObject getAnimeCharacters(const QString& ref, int page = 1, int limit = 20) const;
    QJsonObject getAnimeStaff(const QString& ref, int page = 1, int limit = 20) const;
    QJsonObject getCollections(const QString& search = {}, int page = 1, int limit = 20,
                               const QString& sort = {}, const QString& order = {}) const;
    QJsonObject getCollection(const QString& ref) const;
    QJsonObject getCollectionAnime(const QString& ref, int page = 1, int limit = 20) const;
    QJsonObject getCollectionComments(const QString& ref, int page = 1, int limit = 20) const;
    QJsonObject getUserCollections(const QString& username, int page = 1, int limit = 20) const;
    QJsonObject getComments(const QString& targetType, const QString& targetRef,
                            int page = 1, int limit = 20) const;
    QJsonObject getAnimeComments(const QString& ref, int page = 1, int limit = 20) const;
    QJsonObject getBlogPostComments(const QString& slug, int page = 1, int limit = 20) const;
    QJsonObject getCommentReplies(const QString& id, int page = 1, int limit = 20) const;
    QJsonObject getBlogPosts(int page = 1, int limit = 20, const QString& tag = {}) const;
    QJsonObject getBlogPost(const QString& slug) const;
    QJsonArray getBlogTags() const;
    QJsonObject transliterate(const QString& text, const QString& system = {}) const;
    QJsonArray getTranslitSystems() const;

    // Artwork download: bypasses the rate limiter, 20s timeout by default. This
    // is the one transport callers use for large non-artwork payloads (a model
    // graph, for instance, needs far more than the artwork budget); a negative
    // timeout keeps the artwork default.
    /// How a payload URL may redirect. Artwork stays on its own origin; a
    /// pinned download follows a redirect only to another https host, because a
    /// mirror such as HuggingFace answers `resolve/` with a CDN location and the
    /// bytes are proved by digest on arrival anyway.
    enum class Redirects { SameOrigin, FollowSafe };
    QByteArray fetchBytes(
        const QString& url, int timeoutMs = -1,
        const std::function<void(qint64 received, qint64 total)>& progress = {},
        Redirects redirects = Redirects::SameOrigin) const;

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
    QJsonObject post(const QString& path, const QJsonObject& body = {}) const;
    QJsonObject put(const QString& path, const QJsonObject& body = {}) const;
    QJsonObject patch(const QString& path, const QJsonObject& body = {}) const;
    QJsonObject del(const QString& path, const QJsonObject& body = {}) const;
    QJsonObject apiGet(const QString& url, const QMap<QString, QString>& params = {},
                       const QString& overrideKey = {}, int timeoutMs = 15000) const;
    QJsonObject apiPost(const QString& url, const QJsonObject& body,
                        int timeoutMs = 15000) const;
    QJsonObject apiMutate(int methodEnum, const QString& url, const QJsonObject& body,
                          int timeoutMs = 15000) const;

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
