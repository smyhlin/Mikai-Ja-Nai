#include <utility>
#include "Models.hpp"

#include <QDebug>
#include <QJsonArray>
#include <QJsonValue>
#include <QUrl>

#include <cmath>
#include <limits>

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
        // P1-07: a double outside int range (or NaN/inf) makes the cast
        // below undefined behaviour, so check first and fall back.
        const double d = value.toDouble();
        if (!std::isfinite(d) || d < static_cast<double>(std::numeric_limits<int>::min()) ||
            d >= static_cast<double>(std::numeric_limits<int>::max()) + 1.0) {
            qWarning("Models: optInt: JSON number out of int range; using default");
            return std::nullopt;
        }
        return static_cast<int>(d);
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

std::optional<qint64> optInt64(const QJsonValue& value) {
    if (value.isDouble()) {
        // P1-07: same range check as optInt (see above); a double >= 2^63
        // or < -2^63 cannot be a qint64.
        const double d = value.toDouble();
        if (!std::isfinite(d) || d < static_cast<double>(std::numeric_limits<qint64>::min()) ||
            d >= static_cast<double>(std::numeric_limits<qint64>::max())) {
            qWarning("Models: optInt64: JSON number out of qint64 range; using default");
            return std::nullopt;
        }
        return static_cast<qint64>(d);
    }
    if (value.isString()) {
        bool ok = false;
        const qint64 parsed = value.toString().toLongLong(&ok);
        if (ok) return parsed;
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

// P1-07: an external-link id is a single path segment. Anything containing
// a separator, a query/fragment marker or a parent traversal is rejected
// outright instead of being encoded into a link.
bool externalLinkIdIsSafe(const QString& id) {
    return !id.isEmpty() && !id.contains(QLatin1Char('/')) &&
           !id.contains(QLatin1Char('?')) && !id.contains(QLatin1Char('#')) &&
           !id.contains(QStringLiteral(".."));
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

const UaPoster* coverPoster(const AnimeDetail& detail) {
    // The selected poster wins, but only when it carries an image that can
    // actually be shown: a selected poster with no usable URL is not on
    // screen, and crediting its author would name someone for a picture
    // nobody sees.
    const auto usable = [](const UaPoster& poster) {
        return !imageUrl(poster.images.big).isEmpty() || !imageUrl(poster.images.medium).isEmpty();
    };
    for (const UaPoster& poster : detail.ua_posters) {
        if (poster.is_selected && usable(poster)) {
            return &poster;
        }
    }
    for (const UaPoster& poster : detail.ua_posters) {
        if (usable(poster)) {
            return &poster;
        }
    }
    // The original poster is on screen instead: no UA poster is credited.
    return nullptr;
}

QString coverUrl(const AnimeDetail& detail) {
    if (const UaPoster* poster = coverPoster(detail)) {
        for (const ImageVariant& size : {poster->images.big, poster->images.medium}) {
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

QString episodeKindFor(const AnimeDetail& detail, const Episode& episode) {
    if (!episode.kind.isEmpty()) {
        return episode.kind;
    }
    const int type = episode.number.metaType().id();
    if (type != QMetaType::Double && type != QMetaType::Float && type != QMetaType::Int &&
        type != QMetaType::UInt && type != QMetaType::LongLong &&
        type != QMetaType::ULongLong && type != QMetaType::Short &&
        type != QMetaType::UShort && type != QMetaType::QString) {
        return {};
    }
    bool ok = false;
    const double number = episode.number.toDouble(&ok);
    if (!ok || !std::isfinite(number) || number <= 0.0) {
        return {};
    }
    const double canonical = std::floor(number);
    for (auto it = detail.episode_types.cbegin(); it != detail.episode_types.cend(); ++it) {
        for (const EpisodeRange& range : it.value()) {
            if (canonical >= range.from && canonical <= range.to) {
                return it.key();
            }
        }
    }
    return {};
}

QString teamLinkServiceName(const QString& key) {
    static const QMap<QString, QString> names{
        {QStringLiteral("telegram"), QStringLiteral("Telegram")},
        {QStringLiteral("youtube"), QStringLiteral("YouTube")},
        {QStringLiteral("tiktok"), QStringLiteral("TikTok")},
        {QStringLiteral("instagram"), QStringLiteral("Instagram")},
        {QStringLiteral("discord"), QStringLiteral("Discord")},
        {QStringLiteral("patreon"), QStringLiteral("Patreon")},
        {QStringLiteral("donatello"), QStringLiteral("Donatello")},
        {QStringLiteral("monobase"), QStringLiteral("Monobase")},
        {QStringLiteral("monojar"), QStringLiteral("Monobank")},
        {QStringLiteral("monobank"), QStringLiteral("Monobank")},
        {QStringLiteral("facebook"), QStringLiteral("Facebook")},
        {QStringLiteral("twitter"), QStringLiteral("Twitter")},
        {QStringLiteral("x"), QStringLiteral("X")},
        {QStringLiteral("vk"), QStringLiteral("VK")},
        {QStringLiteral("vimeo"), QStringLiteral("Vimeo")},
        {QStringLiteral("twitch"), QStringLiteral("Twitch")},
    };
    const QString named = names.value(key);
    return named.isEmpty() ? key : named;
}

QVector<std::pair<QString, QString>> externalLinks(const AnimeDetail& detail) {
    QVector<std::pair<QString, QString>> links;
    const auto id = [&detail](const QString& key) { return detail.ids.value(key).toString(); };
    // P1-07: build each URL with QUrl instead of concatenating the raw id,
    // so a crafted id can never walk the path off-site or smuggle a query.
    // Base URLs and link order are unchanged; IMDb keeps its trailing slash.
    const auto appendLink = [&links](const QString& name, const QString& base,
                                     const QString& linkId, bool trailingSlash) {
        if (linkId.isEmpty()) {
            return;
        }
        if (!externalLinkIdIsSafe(linkId)) {
            // Never log the id itself: it is attacker-controlled input.
            qWarning("Models: externalLinks: rejecting unsafe id for '%s'",
                     qPrintable(name));
            return;
        }
        QUrl url(base);
        QString path = url.path() + QLatin1Char('/') +
                       QString::fromUtf8(QUrl::toPercentEncoding(linkId));
        if (trailingSlash) {
            path += QLatin1Char('/');
        }
        url.setPath(path);
        links.append({name, url.toString()});
    };
    appendLink(QStringLiteral("MyAnimeList"), QStringLiteral("https://myanimelist.net/anime"),
               id(QStringLiteral("mal")), false);
    appendLink(QStringLiteral("AniList"), QStringLiteral("https://anilist.co/anime"),
               id(QStringLiteral("al")), false);
    appendLink(QStringLiteral("Shikimori"), QStringLiteral("https://shikimori.one/animes"),
               id(QStringLiteral("hikka")), false);
    appendLink(QStringLiteral("IMDb"), QStringLiteral("https://www.imdb.com/title"),
               id(QStringLiteral("imdb")), true);
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
    team.id = optInt64(data.value(QStringLiteral("id"))).value_or(0);
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
    // P1-07: checked read — a missing or wrong-typed field keeps the old
    // default (false) and logs once instead of silently collapsing.
    const QJsonValue isCollabValue = entry.value(QStringLiteral("isCollab"));
    if (isCollabValue.isBool()) {
        update.is_collab = isCollabValue.toBool();
    } else {
        qWarning("Models: parseUpdate: 'isCollab' missing or wrong type; using default");
    }
    update.kind = text(entry.value(QStringLiteral("kind")));
    update.provider = text(entry.value(QStringLiteral("provider")));
    update.episode = optionalVariant(entry.value(QStringLiteral("episode")));
    update.published_at = text(entry.value(QStringLiteral("publishedAt")));
    update.embed_url = text(entry.value(QStringLiteral("embedUrl")));
    return update;
}

AiringEntry parseAiringEntry(const QJsonObject& entry) {
    AiringEntry airing;
    airing.anime = parseSummary(objectOrEmpty(entry.value(QStringLiteral("anime"))));
    airing.episode = optionalVariant(entry.value(QStringLiteral("episode")));
    airing.airing = text(entry.value(QStringLiteral("airing")));
    const QJsonValue finishedValue = entry.value(QStringLiteral("finished"));
    if (finishedValue.isBool()) {
        airing.finished = finishedValue.toBool();
    } else {
        qWarning("Models: parseAiringEntry: 'finished' missing or wrong type; using default");
    }
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
        const QJsonObject raw = value.toObject();
        UaPoster poster;
        poster.id = optInt64(raw.value(QStringLiteral("id"))).value_or(0);
        const QJsonValue isSelectedValue = raw.value(QStringLiteral("isSelected"));
        if (isSelectedValue.isBool()) {
            poster.is_selected = isSelectedValue.toBool();
        } else {
            qWarning("Models: parseDetail: 'isSelected' missing or wrong type; using default");
        }
        poster.author = text(raw.value(QStringLiteral("author")));
        if (raw.value(QStringLiteral("team")).isObject()) {
            poster.team = parseTeam(raw.value(QStringLiteral("team")).toObject());
        }
        poster.images = parsePosterSet(objectOrEmpty(raw.value(QStringLiteral("images"))));
        detail.ua_posters.append(poster);
    }

    const QJsonObject episodeTypes = objectOrEmpty(data.value(QStringLiteral("episodeTypes")));
    for (auto it = episodeTypes.begin(); it != episodeTypes.end(); ++it) {
        QVector<EpisodeRange> ranges;
        const QJsonArray values = arrayOrEmpty(it.value());
        ranges.reserve(values.size());
        for (const QJsonValue& value : values) {
            if (value.isObject()) {
                const QJsonObject range = value.toObject();
                const auto from = optDouble(range.value(QStringLiteral("from")));
                const auto to = optDouble(range.value(QStringLiteral("to")));
                if (from && to) ranges.append(EpisodeRange{*from, *to});
                continue;
            }
            const QJsonArray pair = value.toArray();
            if (pair.size() >= 2) {
                const auto from = optDouble(pair.at(0));
                const auto to = optDouble(pair.at(1));
                if (from && to) ranges.append(EpisodeRange{*from, *to});
            }
        }
        detail.episode_types.insert(it.key(), std::move(ranges));
    }
    const QJsonValue nextEpisode = data.value(QStringLiteral("nextEpisode"));
    if (nextEpisode.isObject()) {
        const QJsonObject raw = nextEpisode.toObject();
        detail.next_episode = NextEpisode{
            optionalVariant(raw.value(QStringLiteral("episode"))),
            text(raw.value(QStringLiteral("airing")))};
    }
    const QJsonValue licensedValue = data.value(QStringLiteral("licensed"));
    if (licensedValue.isBool()) {
        detail.licensed = licensedValue.toBool();
    } else {
        qWarning("Models: parseDetail: 'licensed' missing or wrong type; using default");
    }
    detail.license = objectOrEmpty(data.value(QStringLiteral("license")));

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
    const QJsonValue licensedValue = data.value(QStringLiteral("licensed"));
    if (licensedValue.isBool()) {
        player.licensed = licensedValue.toBool();
    } else {
        qWarning("Models: parsePlayer: 'licensed' missing or wrong type; using default");
    }
    player.license = objectOrEmpty(data.value(QStringLiteral("license")));

    const QJsonArray releases = arrayOrEmpty(data.value(QStringLiteral("releases")));
    player.releases.reserve(releases.size());
    for (const QJsonValue& releaseValue : releases) {
        const QJsonObject raw = releaseValue.toObject();
        Release release;
        release.id = text(raw.value(QStringLiteral("id")));
        release.kind = text(raw.value(QStringLiteral("kind")));
        const QJsonValue isCollabValue = raw.value(QStringLiteral("isCollab"));
        if (isCollabValue.isBool()) {
            release.is_collab = isCollabValue.toBool();
        } else {
            qWarning("Models: parsePlayer: 'isCollab' missing or wrong type; using default");
        }

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
            episode.kind = text(rawEpisode.value(QStringLiteral("kind")));

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
