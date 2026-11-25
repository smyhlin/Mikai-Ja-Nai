#include "app/support/PlaybackLink.hpp"

#include "app/support/Labels.hpp"

#include <QUrl>
#include <QUrlQuery>

#include <limits>

namespace mikai::app {
namespace {

constexpr int kMaxTimeSeconds = 24 * 60 * 60;  // 24h upper bound

bool isSafeToken(const QString& value) {
    if (value.isEmpty() || value.size() > 256) return false;
    for (const QChar ch : value) {
        if (ch.isSpace() || ch.unicode() < 0x20 || ch.unicode() == 0x7f) return false;
    }
    return true;
}

}  // namespace

bool isPlaybackLinkUrl(const QString& raw) {
    return raw.trimmed().startsWith(QStringLiteral("mikai-janai://"), Qt::CaseInsensitive);
}

QString formatPlaybackLink(const PlaybackLink& link) {
    const QString ref = link.ref.trimmed();
    if (!isSafeToken(ref)) return {};

    QUrl url;
    url.setScheme(QStringLiteral("mikai-janai"));
    url.setHost(QStringLiteral("play"));

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ref"), ref);

    const QString provider = labels::providerKey(link.provider);
    const QString team = link.teamSlug.trimmed();
    const QString episode = link.episode.trimmed();
    if (!provider.isEmpty() && !episode.isEmpty()) {
        query.addQueryItem(QStringLiteral("provider"), provider);
        if (!team.isEmpty()) {
            query.addQueryItem(QStringLiteral("team"), team);
        }
        query.addQueryItem(QStringLiteral("episode"), episode);
        if (link.timeSec.has_value() && *link.timeSec >= 0 && *link.timeSec <= kMaxTimeSeconds) {
            query.addQueryItem(QStringLiteral("t"), QString::number(*link.timeSec));
        }
    }
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

std::optional<PlaybackLink> parsePlaybackLink(const QString& raw, QString* error) {
    const auto fail = [error](const QString& message) -> std::optional<PlaybackLink> {
        if (error) *error = message;
        return std::nullopt;
    };

    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty()) return fail(QStringLiteral("Playback link is empty."));

    const QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid()) return fail(QStringLiteral("Playback link is not a valid URL."));
    if (url.scheme().compare(QStringLiteral("mikai-janai"), Qt::CaseInsensitive) != 0) {
        return fail(QStringLiteral("Unsupported URL scheme (expected mikai-janai://)."));
    }
    if (url.host().compare(QStringLiteral("play"), Qt::CaseInsensitive) != 0) {
        return fail(QStringLiteral("Unsupported mikai-janai action (expected play)."));
    }

    const QUrlQuery query(url);
    const QString ref = query.queryItemValue(QStringLiteral("ref"), QUrl::FullyDecoded).trimmed();
    if (!isSafeToken(ref)) {
        return fail(QStringLiteral("Playback link is missing a valid title reference (ref)."));
    }

    PlaybackLink parsed;
    parsed.ref = ref;

    const QString rawProv =
        query.queryItemValue(QStringLiteral("provider"), QUrl::FullyDecoded).trimmed();
    const QString rawTeam =
        query.queryItemValue(QStringLiteral("team"), QUrl::FullyDecoded).trimmed();
    const QString rawEp =
        query.queryItemValue(QStringLiteral("episode"), QUrl::FullyDecoded).trimmed();
    const QString rawTime =
        query.queryItemValue(QStringLiteral("t"), QUrl::FullyDecoded).trimmed();

    const QString normProv = labels::providerKey(rawProv);
    // Provider + episode are required together for a playback target; partial
    // parameters fall back cleanly to opening the title without failing.
    if (!normProv.isEmpty() && !rawEp.isEmpty()) {
        if (!isSafeToken(rawEp) || (!rawTeam.isEmpty() && !isSafeToken(rawTeam))) {
            return fail(QStringLiteral("Playback link contains unsafe team or episode parameter."));
        }
        parsed.provider = normProv;
        parsed.teamSlug = rawTeam;
        parsed.episode = rawEp;

        if (!rawTime.isEmpty()) {
            bool ok = false;
            const qint64 secs = rawTime.toLongLong(&ok);
            if (!ok || secs < 0 || secs > kMaxTimeSeconds) {
                return fail(QStringLiteral("Playback link timecode (t) must be a non-negative integer in seconds."));
            }
            parsed.timeSec = static_cast<int>(secs);
        }
    }

    if (error) error->clear();
    return parsed;
}

}  // namespace mikai::app
