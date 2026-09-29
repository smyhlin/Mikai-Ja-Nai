#include "Releases.hpp"

#include <algorithm>

namespace mikai::backend {
namespace {

int kindOrder(const QString& kind) {
    if (kind == QLatin1String("voice")) {
        return 0;
    }
    if (kind == QLatin1String("sub")) {
        return 1;
    }
    return 2;
}

QString teamKey(const Release& release) {
    QStringList names;
    names.reserve(release.teams.size());
    for (const Team& team : release.teams) {
        names.append(team.name);
    }
    return names.join(QStringLiteral(", ")).toLower();
}

}  // namespace

int playableEpisodeCount(const Release& release) {
    if (release.episodes.isEmpty()) {
        return release.episodes_count.value_or(0);
    }
    int playable = 0;
    for (const Episode& episode : release.episodes) {
        const bool hasSource = std::any_of(
            episode.sources.cbegin(), episode.sources.cend(),
            [](const Source& source) { return !source.embed_url.isEmpty(); });
        if (hasSource) {
            ++playable;
        }
    }
    return playable;
}

QVector<Release> sortReleases(QVector<Release> releases) {
    std::stable_sort(releases.begin(), releases.end(), [](const Release& left, const Release& right) {
        const int leftKind = kindOrder(left.kind);
        const int rightKind = kindOrder(right.kind);
        if (leftKind != rightKind) {
            return leftKind < rightKind;
        }
        const int leftPlayable = playableEpisodeCount(left);
        const int rightPlayable = playableEpisodeCount(right);
        if (leftPlayable != rightPlayable) {
            return leftPlayable > rightPlayable;
        }
        return teamKey(left) < teamKey(right);
    });
    return releases;
}

}  // namespace mikai::backend
