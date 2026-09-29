#include "Models.hpp"

#include <QJsonArray>
#include <QJsonValue>

namespace mikai::backend {
namespace {

// Python's `data.get("x") or ""` collapses missing keys, nulls and wrong
// types into one empty fallback, which is what an empty QString carries here.
QString text(const QJsonValue& value) {
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble(), 'g', 17);
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    return {};
}

// Every poster field ships a webp and a jpg sibling, and webp is the smaller
// one, so it wins. That only works because the imageformats plugin carries
// qwebp.dll: a base Qt install has gif/ico/jpeg/svg only, and QPixmap answers a
// webp payload with a null image and no error at all. TestModels::webpIsDecodable
// is the guard that makes a missing plugin a loud failure rather than a screen
// of empty placeholders.
QString imageUrl(const ImageVariant& variant) {
    return variant.webp.isEmpty() ? variant.jpg : variant.webp;
}

std::optional<int> optInt(const QJsonValue& value) {
    if (value.isDouble()) {
        return static_cast<int>(value.toDouble());
    }
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().toInt(&ok);
        if (ok) {
            return parsed;
        }
    }
    return std::nullopt;
}

std::optional<double> optDouble(const QJsonValue& value) {
    if (value.isDouble()) {
        return value.toDouble();
    }
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        if (ok) {
            return parsed;
        }
    }
    return std::nullopt;
}

QVariant optionalVariant(const QJsonValue& value) {
    if (value.isUndefined() || value.isNull()) {
        return {};
    }
    return value.toVariant();
}

QJsonObject objectOrEmpty(const QJsonValue& value) {
    return value.isObject() ? value.toObject() : QJsonObject{};
}

QJsonArray arrayOrEmpty(const QJsonValue& value) {
    return value.isArray() ? value.toArray() : QJsonArray{};
}

}  // namespace

QString displayName(const Titles& titles) {
    if (!titles.ua.isEmpty()) {
        return titles.ua;
    }
    if (!titles.english.isEmpty()) {
        return titles.english;
    }
    if (!titles.original.isEmpty()) {
        return titles.original;
    }
    return QStringLiteral("?");
}

QString genreDisplay(const GenreTag& tag) { return tag.ua.isEmpty() ? tag.name : tag.ua; }

QString animePageUrl(const QString& ref) { return kMikaiPageUrl + ref; }

QString animePageUrl(const QString& ref, const QString& slug) {
    // The bare id is not a page: mikai.me serves the title at /anime/<id>-<slug>
    // and 404s on /anime/<id>, so a copied link that only the API understands
    // is a dead link. The slug is optional because a list payload has none, and
    // an id with a trailing dash would be worse than a 404.
    if (!slug.isEmpty() && !ref.isEmpty()) {
        return kMikaiPageUrl + ref + QLatin1Char('-') + slug;
    }
    return animePageUrl(ref);
}

QString thumbUrl(const AnimeSummary& anime) {
    return imageUrl(anime.poster.small);
}

QString posterUrl(const AnimeSummary& anime) {
    return imageUrl(anime.poster.big);
}

QString coverUrl(const AnimeDetail& detail) {
    for (const UaPoster& poster : detail.ua_posters) {
        for (const ImageVariant& size : {poster.images.big, poster.images.medium}) {
            const QString url = imageUrl(size);
            if (!url.isEmpty()) {
                return url;
            }
        }
    }
    for (const ImageVariant& size : {detail.poster.big, detail.poster.medium}) {
        const QString url = imageUrl(size);
        if (!url.isEmpty()) {
            return url;
        }
    }
    return {};
}

QVector<QPair<QString, QString>> externalLinks(const AnimeDetail& detail) {
    QVector<QPair<QString, QString>> links;
    const auto id = [&detail](const QString& key) { return detail.ids.value(key).toString(); };

    const QString mal = id(QStringLiteral("mal"));
    if (!mal.isEmpty()) {
        links.append(
            {QStringLiteral("MyAnimeList"), QStringLiteral("https://myanimelist.net/anime/") + mal});
    }
    const QString al = id(QStringLiteral("al"));
    if (!al.isEmpty()) {
        links.append({QStringLiteral("AniList"), QStringLiteral("https://anilist.co/anime/") + al});
    }
    const QString hikka = id(QStringLiteral("hikka"));
    if (!hikka.isEmpty()) {
        links.append(
            {QStringLiteral("Shikimori"), QStringLiteral("https://shikimori.one/animes/") + hikka});
    }
    const QString imdb = id(QStringLiteral("imdb"));
    if (!imdb.isEmpty()) {
        links.append({QStringLiteral("IMDb"),
                      QStringLiteral("https://www.imdb.com/title/") + imdb + QStringLiteral("/")});
    }
    return links;
}

Titles parseTitles(const QJsonObject& data) {
    Titles titles;
    titles.ua = text(data.value(QStringLiteral("ua")));
    titles.english = text(data.value(QStringLiteral("english")));
    titles.original = text(data.value(QStringLiteral("original")));
    return titles;
}

ImageVariant parseImageVariant(const QJsonObject& data) {
    ImageVariant variant;
    variant.webp = text(data.value(QStringLiteral("webp")));
    variant.jpg = text(data.value(QStringLiteral("jpg")));
    return variant;
}

PosterSet parsePosterSet(const QJsonObject& data) {
    PosterSet posters;
    posters.big = parseImageVariant(objectOrEmpty(data.value(QStringLiteral("big"))));
    posters.medium = parseImageVariant(objectOrEmpty(data.value(QStringLiteral("medium"))));
    posters.small = parseImageVariant(objectOrEmpty(data.value(QStringLiteral("small"))));
    return posters;
}

Team parseTeam(const QJsonObject& data) {
    Team team;
    team.id = optInt(data.value(QStringLiteral("id"))).value_or(0);
    team.slug = text(data.value(QStringLiteral("slug")));
    team.name = text(data.value(QStringLiteral("name")));
    return team;
}

AnimeSummary parseSummary(const QJsonObject& entry) {
    const QJsonObject ids = objectOrEmpty(entry.value(QStringLiteral("ids")));
    QString ref = text(ids.value(QStringLiteral("mikai")));
    if (ref.isEmpty()) {
        for (const QString& key : {QStringLiteral("mal"), QStringLiteral("al"), QStringLiteral("hikka"),
                                   QStringLiteral("slug")}) {
            const QString value = text(ids.value(key));
            if (!value.isEmpty()) {
                ref = key + QLatin1Char(':') + value;
                break;
            }
        }
    }

    AnimeSummary anime;
    anime.ref = ref;
    anime.titles = parseTitles(objectOrEmpty(entry.value(QStringLiteral("titles"))));
    anime.year = optInt(entry.value(QStringLiteral("year")));
    anime.format = text(entry.value(QStringLiteral("format")));
    anime.status = text(entry.value(QStringLiteral("status")));
    anime.episodes = optInt(entry.value(QStringLiteral("episodes")));
    anime.poster = parsePosterSet(
        objectOrEmpty(objectOrEmpty(entry.value(QStringLiteral("images"))).value(QStringLiteral("poster"))));
    return anime;
}

AnimeUpdate parseUpdate(const QJsonObject& entry) {
    AnimeUpdate update;
    update.anime = parseSummary(objectOrEmpty(entry.value(QStringLiteral("anime"))));
    const QJsonArray teams = arrayOrEmpty(entry.value(QStringLiteral("teams")));
    update.teams.reserve(teams.size());
    for (const QJsonValue& value : teams) {
        update.teams.append(parseTeam(value.toObject()));
    }
    update.team_name = text(entry.value(QStringLiteral("teamName")));
    update.is_collab = entry.value(QStringLiteral("isCollab")).toBool();
    update.kind = text(entry.value(QStringLiteral("kind")));
    update.provider = text(entry.value(QStringLiteral("provider")));
    update.episode = optionalVariant(entry.value(QStringLiteral("episode")));
    update.published_at = text(entry.value(QStringLiteral("publishedAt")));
    return update;
}

AiringEntry parseAiringEntry(const QJsonObject& entry) {
    AiringEntry airing;
    airing.anime = parseSummary(objectOrEmpty(entry.value(QStringLiteral("anime"))));
    airing.episode = optionalVariant(entry.value(QStringLiteral("episode")));
    airing.airing = text(entry.value(QStringLiteral("airing")));
    airing.finished = entry.value(QStringLiteral("finished")).toBool();
    return airing;
}

AnimeDetail parseDetail(const QJsonObject& data) {
    const QJsonObject ids = objectOrEmpty(data.value(QStringLiteral("ids")));
    const QJsonObject episodes = objectOrEmpty(data.value(QStringLiteral("episodes")));
    const QJsonObject scores = objectOrEmpty(data.value(QStringLiteral("scores")));

    AnimeDetail detail;
    detail.ref = text(ids.value(QStringLiteral("mikai")));
    for (auto it = ids.begin(); it != ids.end(); ++it) {
        detail.ids.insert(it.key(), it.value().toVariant());
    }
    detail.titles = parseTitles(objectOrEmpty(data.value(QStringLiteral("titles"))));
    detail.description = text(data.value(QStringLiteral("description")));
    detail.format = text(data.value(QStringLiteral("format")));
    detail.status = text(data.value(QStringLiteral("status")));
    detail.start_date = text(data.value(QStringLiteral("startDate")));
    detail.source = text(data.value(QStringLiteral("source")));
    detail.studio = text(data.value(QStringLiteral("studio")));
    detail.country = text(data.value(QStringLiteral("country")));
    detail.age_rating = text(data.value(QStringLiteral("ageRating")));

    detail.episodes.total = optInt(episodes.value(QStringLiteral("total")));
    detail.episodes.aired = optInt(episodes.value(QStringLiteral("aired"))).value_or(0);
    detail.episodes.localized = optInt(episodes.value(QStringLiteral("localized"))).value_or(0);
    detail.episodes.duration_min = optInt(episodes.value(QStringLiteral("durationMin")));

    detail.scores.mikai = optDouble(scores.value(QStringLiteral("mikai")));
    detail.scores.mikai_count = optInt(scores.value(QStringLiteral("mikaiCount"))).value_or(0);
    detail.scores.mal = optDouble(scores.value(QStringLiteral("mal")));
    detail.scores.mal_count = optInt(scores.value(QStringLiteral("malCount"))).value_or(0);

    const auto readTags = [](const QJsonArray& source, QVector<GenreTag>& target) {
        target.reserve(source.size());
        for (const QJsonValue& value : source) {
            const QJsonObject raw = value.toObject();
            target.append(GenreTag{text(raw.value(QStringLiteral("name"))),
                                   text(raw.value(QStringLiteral("ua")))});
        }
    };
    readTags(arrayOrEmpty(data.value(QStringLiteral("genres"))), detail.genres);
    readTags(arrayOrEmpty(data.value(QStringLiteral("tags"))), detail.tags);

    detail.poster = parsePosterSet(
        objectOrEmpty(objectOrEmpty(data.value(QStringLiteral("images"))).value(QStringLiteral("poster"))));

    const QJsonArray uaPosters = arrayOrEmpty(data.value(QStringLiteral("uaPosters")));
    detail.ua_posters.reserve(uaPosters.size());
    for (const QJsonValue& value : uaPosters) {
        UaPoster poster;
        poster.images =
            parsePosterSet(objectOrEmpty(value.toObject().value(QStringLiteral("images"))));
        detail.ua_posters.append(poster);
    }

    const QJsonArray relations = arrayOrEmpty(data.value(QStringLiteral("relations")));
    detail.relations.reserve(relations.size());
    for (const QJsonValue& value : relations) {
        const QJsonObject relation = value.toObject();
        AnimeRelation entry;
        entry.kind = text(relation.value(QStringLiteral("kind")));
        entry.anime = parseSummary(objectOrEmpty(relation.value(QStringLiteral("anime"))));
        detail.relations.append(entry);
    }

    const QJsonArray similar = arrayOrEmpty(data.value(QStringLiteral("similar")));
    detail.similar.reserve(similar.size());
    for (const QJsonValue& value : similar) {
        detail.similar.append(parseSummary(value.toObject()));
    }

    return detail;
}

PlayerResult parsePlayer(const QJsonObject& data) {
    PlayerResult player;
    player.licensed = data.value(QStringLiteral("licensed")).toBool();

    const QJsonArray releases = arrayOrEmpty(data.value(QStringLiteral("releases")));
    player.releases.reserve(releases.size());
    for (const QJsonValue& releaseValue : releases) {
        const QJsonObject raw = releaseValue.toObject();
        Release release;
        release.id = text(raw.value(QStringLiteral("id")));
        release.kind = text(raw.value(QStringLiteral("kind")));
        release.is_collab = raw.value(QStringLiteral("isCollab")).toBool();

        const QJsonArray teams = arrayOrEmpty(raw.value(QStringLiteral("teams")));
        release.teams.reserve(teams.size());
        for (const QJsonValue& value : teams) {
            release.teams.append(parseTeam(value.toObject()));
        }
        release.episodes_count = optInt(raw.value(QStringLiteral("episodesCount")));

        const QJsonArray episodes = arrayOrEmpty(raw.value(QStringLiteral("episodes")));
        release.episodes.reserve(episodes.size());
        for (const QJsonValue& episodeValue : episodes) {
            const QJsonObject rawEpisode = episodeValue.toObject();
            Episode episode;
            episode.number = optionalVariant(rawEpisode.value(QStringLiteral("number")));
            episode.label = text(rawEpisode.value(QStringLiteral("label")));

            const QJsonArray sources = arrayOrEmpty(rawEpisode.value(QStringLiteral("sources")));
            episode.sources.reserve(sources.size());
            for (const QJsonValue& sourceValue : sources) {
                const QJsonObject rawSource = sourceValue.toObject();
                Source source;
                source.provider = text(rawSource.value(QStringLiteral("provider")));
                if (source.provider.isEmpty()) {
                    source.provider = QStringLiteral("?");
                }
                source.embed_url = text(rawSource.value(QStringLiteral("embedUrl")));
                episode.sources.append(source);
            }
            release.episodes.append(episode);
        }
        player.releases.append(release);
    }
    return player;
}

}  // namespace mikai::backend
