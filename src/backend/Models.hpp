#pragma once

#include <QJsonObject>
#include <QMap>
#include <QMetaType>
#include <utility>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include <optional>

// Models mirror mikai_backend/models.py field for field, so the members keep
// the Python names. Nullable numbers are std::optional, nullable strings are
// an empty QString, the Python lists are QVector and the id dicts QMap.
namespace mikai::backend {

inline const QString kMikaiPageUrl = QStringLiteral("https://mikai.me/anime/");
inline const QString kBaseUrl = QStringLiteral("https://api.mikai.me/public/v1");

struct Titles {
    QString ua;
    QString english;
    QString original;
};

struct ImageVariant {
    QString webp;
    QString jpg;
};

struct PosterSet {
    ImageVariant big;
    ImageVariant medium;
    ImageVariant small;
};

struct Team {
    qint64 id = 0;
    QString slug;
    QString name;
};

struct Source {
    QString provider = QStringLiteral("?");
    QString embed_url;
};

struct Episode {
    QVariant number;
    QString label;
    QString kind;
    QVector<Source> sources;
};

struct EpisodeRange {
    double from = 0.0;
    double to = 0.0;
};

struct NextEpisode {
    QVariant episode;
    QString airing;
};

struct Release {
    QString id;
    QString kind;
    bool is_collab = false;
    QVector<Team> teams;
    std::optional<int> episodes_count;
    QVector<Episode> episodes;
};

struct UaPoster {
    qint64 id = 0;
    bool is_selected = false;
    QString author;
    std::optional<Team> team;
    PosterSet images;
};

struct GenreTag {
    QString name;
    QString ua;
};

struct EpisodeStats {
    std::optional<int> total;
    int aired = 0;
    int localized = 0;
    std::optional<int> duration_min;
};

struct Scores {
    std::optional<double> mikai;
    int mikai_count = 0;
    std::optional<double> mal;
    int mal_count = 0;
};

struct AnimeSummary {
    QString ref;
    Titles titles;
    std::optional<int> year;
    QString format;
    QString status;
    std::optional<int> episodes;
    PosterSet poster;
};

struct SearchResult {
    std::optional<int> total;
    std::optional<int> page;
    std::optional<int> pages;
    QVector<AnimeSummary> items;
};

// The feed entries let an episode arrive as a number or as a free-form string
// ("OVA", "12.5"), so they keep the QVariant the JSON carried.
struct AnimeUpdate {
    AnimeSummary anime;
    QVector<Team> teams;
    QString team_name;
    bool is_collab = false;
    QString kind;
    QString provider;
    QString embed_url;
    QVariant episode;
    QString published_at;
};

struct UpdatesResult {
    std::optional<int> total;
    std::optional<int> page;
    std::optional<int> pages;
    QVector<AnimeUpdate> items;
};

struct AiringEntry {
    AnimeSummary anime;
    QVariant episode;
    QString airing;
    bool finished = false;
};

struct ScheduleResult {
    QMap<QString, QVector<AiringEntry>> days;
};

struct AnimeRelation {
    QString kind;
    AnimeSummary anime;
};

struct AnimeDetail {
    QString ref;
    QMap<QString, QVariant> ids;
    Titles titles;
    QString description;
    QString format;
    QString status;
    QString start_date;
    QString source;
    QString studio;
    QString country;
    QString age_rating;
    EpisodeStats episodes;
    Scores scores;
    QVector<GenreTag> genres;
    QVector<GenreTag> tags;
    PosterSet poster;
    QVector<UaPoster> ua_posters;
    QMap<QString, QVector<EpisodeRange>> episode_types;
    std::optional<NextEpisode> next_episode;
    bool licensed = false;
    QJsonObject license;
    QVector<AnimeRelation> relations;
    QVector<AnimeSummary> similar;
};

struct PlayerResult {
    bool licensed = false;
    QJsonObject license;
    QVector<Release> releases;
};

struct AnimeQuery {
    QString search;
    QStringList genres;
    QStringList genres_exclude;
    QString genres_match = QStringLiteral("all");
    QStringList tags;
    QStringList tags_exclude;
    QString tags_match = QStringLiteral("all");
    QVector<int> years;
    std::optional<int> year_from;
    std::optional<int> year_to;
    QStringList seasons;
    QStringList formats;
    QStringList statuses;
    QStringList studios;
    QVector<qint64> teams;
    QString sort = QStringLiteral("name");
    QString order;
    int page = 1;
    int limit = 20;

    bool operator==(const AnimeQuery&) const = default;
};

struct ApiMeta {
    QString version;
    QStringList formats;
    QStringList statuses;
    QStringList seasons;
    QStringList anime_sort_fields;
    QStringList sort_orders;
    QStringList match_modes;
};

struct TeamProfile {
    Team team;
    QString description;
    QString birthday;
    PosterSet avatar;
    QMap<QString, QString> links;
    bool has_voice = false;
    bool has_subs = false;
    int anime_count = 0;
    qint64 views_total = 0;
    qint64 views_last_30_days = 0;
};

struct TeamsResult {
    std::optional<int> total;
    std::optional<int> page;
    std::optional<int> pages;
    QVector<TeamProfile> items;
};

// Free helpers mirroring the Python properties and module-level helpers.
QString displayName(const Titles& titles);
QString genreDisplay(const GenreTag& tag);
QString animePageUrl(const QString& ref);
/// The public page for a title. `slug` is the human-readable tail the site
/// actually serves - /anime/7584 answers 404 while /anime/7584-slozy-tiary is
/// the page - so it is preferred whenever the payload carried one.
QString animePageUrl(const QString& ref, const QString& slug);
QString thumbUrl(const AnimeSummary& anime);
QString posterUrl(const AnimeSummary& anime);
/// The UA poster the cover actually shows: the selected one when it carries
/// a usable image, otherwise the first UA poster that does. Null when the
/// display falls back to the original poster, so a caller can credit exactly
/// the picture on screen - and name no author when the picture is not a UA
/// poster at all.
const UaPoster* coverPoster(const AnimeDetail& detail);
QString coverUrl(const AnimeDetail& detail);
QString episodeKindFor(const AnimeDetail& detail, const Episode& episode);
QVector<std::pair<QString, QString>> externalLinks(const AnimeDetail& detail);
QString teamLinkServiceName(const QString& key);

// JSON parsers mirroring the Python _parse_* functions.
Titles parseTitles(const QJsonObject& data);
ImageVariant parseImageVariant(const QJsonObject& data);
PosterSet parsePosterSet(const QJsonObject& data);
Team parseTeam(const QJsonObject& data);
AnimeSummary parseSummary(const QJsonObject& entry);
AnimeUpdate parseUpdate(const QJsonObject& entry);
AiringEntry parseAiringEntry(const QJsonObject& entry);
AnimeDetail parseDetail(const QJsonObject& data);
PlayerResult parsePlayer(const QJsonObject& data);

}  // namespace mikai::backend

Q_DECLARE_METATYPE(mikai::backend::Team)
Q_DECLARE_METATYPE(mikai::backend::GenreTag)
Q_DECLARE_METATYPE(mikai::backend::AnimeQuery)
Q_DECLARE_METATYPE(mikai::backend::ApiMeta)
Q_DECLARE_METATYPE(mikai::backend::TeamProfile)
Q_DECLARE_METATYPE(mikai::backend::TeamsResult)
Q_DECLARE_METATYPE(mikai::backend::AnimeUpdate)
Q_DECLARE_METATYPE(mikai::backend::UpdatesResult)
