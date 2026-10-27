#include "UpdateFeed.hpp"

#include "Cancellation.hpp"
#include "Errors.hpp"
#include "MikaiClient.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaType>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace mikai::backend {
namespace {

QDateTime parsedTime(const QString& value) {
    QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!parsed.isValid()) parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed;
}

QString canonicalTime(const QString& value) {
    const QDateTime time = parsedTime(value);
    return time.isValid() ? time.toUTC().toString(Qt::ISODateWithMs) : QString();
}

bool isNumeric(const QVariant& value) {
    switch (value.metaType().id()) {
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
        case QMetaType::Double:
        case QMetaType::Float:
        case QMetaType::Short:
        case QMetaType::UShort:
        case QMetaType::Char:
        case QMetaType::UChar:
        case QMetaType::Long:
        case QMetaType::ULong:
        case QMetaType::SChar:
            return true;
        default:
            return false;
    }
}

QString episodeKey(const QVariant& episode) {
    if (isNumeric(episode)) {
        double number = episode.toDouble();
        if (number == 0.0) number = 0.0;
        return QStringLiteral("number:%1").arg(QString::number(number, 'g', 17));
    }
    return QStringLiteral("value:%1:%2")
        .arg(QString::fromLatin1(episode.metaType().name()), episode.toString());
}

QString eventKey(const AnimeUpdate& update) {
    const QString publishedAt = canonicalTime(update.published_at);
    QJsonObject key;
    key.insert(QStringLiteral("anime"), update.anime.ref);
    key.insert(QStringLiteral("published_at"), publishedAt);
    key.insert(QStringLiteral("kind"), update.kind);
    key.insert(QStringLiteral("provider"), update.provider);
    key.insert(QStringLiteral("episode"), episodeKey(update.episode));

    QStringList teams;
    teams.reserve(update.teams.size());
    for (const Team& team : update.teams) {
        if (team.id != 0) {
            teams.append(QStringLiteral("id:%1").arg(team.id));
        } else if (!team.slug.isEmpty()) {
            teams.append(QStringLiteral("slug:%1").arg(team.slug));
        } else {
            teams.append(QStringLiteral("name:%1").arg(team.name));
        }
    }
    std::sort(teams.begin(), teams.end());
    if (teams.isEmpty() && !update.team_name.isEmpty()) {
        teams.append(QStringLiteral("name:%1").arg(update.team_name));
    }
    QJsonArray teamKeys;
    for (const QString& team : teams) teamKeys.append(team);
    key.insert(QStringLiteral("teams"), teamKeys);
    return publishedAt + QLatin1Char('|') +
           QString::fromUtf8(QJsonDocument(key).toJson(QJsonDocument::Compact));
}

QDateTime eventKeyTime(const QString& key) {
    const qsizetype separator = key.indexOf(QLatin1Char('|'));
    return separator < 0 ? QDateTime() : parsedTime(key.first(separator));
}

bool newerFirst(const AnimeUpdate& left, const AnimeUpdate& right) {
    const QDateTime leftTime = parsedTime(left.published_at);
    const QDateTime rightTime = parsedTime(right.published_at);
    if (!leftTime.isValid()) return false;
    if (!rightTime.isValid()) return true;
    return leftTime > rightTime;
}

void checkCancelled() {
    if (WorkCancellationScope::cancellationRequested()) {
        throw MikaiApiError{QStringLiteral("cancelled"), QStringLiteral("Operation cancelled"), 0};
    }
}

int pageCount(const UpdatesResult& result, int limit) {
    if (result.pages.has_value()) return std::max(1, *result.pages);
    if (result.total.has_value() && limit > 0) {
        return std::max(1, (*result.total + limit - 1) / limit);
    }
    return 1;
}

}  // namespace

UpdateFeedState refreshUpdateFeed(const MikaiClient& client, const UpdateFeedState& previous,
                                  int retainedLimit) {
    // T-311: the API family caps a page at 50 rows whatever `limit` asks for -
    // the same documented cap the search pager already strides at. Asking for
    // 100 only made the refresh count pages the server disagrees with, and it
    // burned quota recounting them on every incremental refresh.
    constexpr int apiPageSize = 50;
    constexpr int incrementalPageSize = apiPageSize;
    constexpr int maximumPageSize = apiPageSize;
    const bool bootstrap = previous.watermark.isEmpty() && previous.items.isEmpty();
    const QDateTime previousWatermark = parsedTime(previous.watermark);
    if (!bootstrap && !previousWatermark.isValid()) {
        throw MikaiApiError{QStringLiteral("invalid_update_timestamp"),
                            QStringLiteral("The update feed watermark is invalid"), 0};
    }
    for (const AnimeUpdate& item : previous.items) {
        if (!parsedTime(item.published_at).isValid()) {
            throw MikaiApiError{QStringLiteral("invalid_update_timestamp"),
                                QStringLiteral("An update has an invalid publishedAt timestamp"), 0};
        }
    }

    const QString since = bootstrap
        ? QString()
        : previousWatermark.addSecs(-1).toUTC().toString(Qt::ISODateWithMs);
    const int requestLimit = bootstrap
        ? std::clamp(retainedLimit, 1, maximumPageSize)
        : incrementalPageSize;
    const int keep = std::max(0, retainedLimit);

    const auto validateUpdates = [](const UpdatesResult& updates) {
        for (const AnimeUpdate& item : updates.items) {
            if (!parsedTime(item.published_at).isValid()) {
                throw MikaiApiError{QStringLiteral("invalid_update_timestamp"),
                                    QStringLiteral("An update has an invalid publishedAt timestamp"), 0};
            }
        }
    };
    const auto addRetained = [keep](QVector<AnimeUpdate>& items, const AnimeUpdate& update) {
        if (keep == 0) return;
        const auto position = std::upper_bound(
            items.cbegin(), items.cend(), update,
            [](const AnimeUpdate& value, const AnimeUpdate& existing) {
                return newerFirst(value, existing);
            });
        items.insert(position, update);
        if (items.size() > keep) items.removeLast();
    };

    checkCancelled();
    const UpdatesResult first = client.getUpdates(1, requestLimit, since);
    validateUpdates(first);
    QString upperWatermark = previous.watermark;
    if (!first.items.isEmpty()) {
        QDateTime newest;
        for (const AnimeUpdate& item : first.items) {
            const QDateTime time = parsedTime(item.published_at);
            if (!newest.isValid() || time > newest) newest = time;
        }
        if (!previousWatermark.isValid() || newest > previousWatermark) {
            upperWatermark = newest.toUTC().toString(Qt::ISODateWithMs);
        }
    }

    UpdateFeedState result;
    result.watermark = upperWatermark;
    result.items = previous.items;
    if (result.items.size() > keep) result.items.resize(keep);

    QSet<QString> seen = previous.boundary_keys;
    for (const AnimeUpdate& item : previous.items) seen.insert(eventKey(item));
    const QDateTime watermarkTime = parsedTime(result.watermark);
    const QDateTime overlapStart = watermarkTime.isValid() ? watermarkTime.addSecs(-1) : QDateTime();
    for (const QString& key : previous.boundary_keys) {
        if (eventKeyTime(key) >= overlapStart) result.boundary_keys.insert(key);
    }
    for (const AnimeUpdate& item : previous.items) {
        if (parsedTime(item.published_at) >= overlapStart) result.boundary_keys.insert(eventKey(item));
    }

    const auto consume = [&](const UpdatesResult& updates) {
        validateUpdates(updates);
        for (const AnimeUpdate& item : updates.items) {
            const QString key = eventKey(item);
            const bool overlaps = parsedTime(item.published_at) >= overlapStart;
            if (overlaps) result.boundary_keys.insert(key);
            if (seen.contains(key)) {
                for (AnimeUpdate& retained : result.items) {
                    if (eventKey(retained) == key) {
                        retained = item;
                        break;
                    }
                }
                continue;
            }
            seen.insert(key);
            addRetained(result.items, item);
        }
    };
    consume(first);

    if (!bootstrap) {
        int pages = pageCount(first, requestLimit);
        for (int page = 2; page <= pages; ++page) {
            checkCancelled();
            const UpdatesResult next = client.getUpdates(page, requestLimit, since);
            consume(next);
            pages = std::max(pages, pageCount(next, requestLimit));
        }
    }
    checkCancelled();
    return result;
}

}  // namespace mikai::backend
