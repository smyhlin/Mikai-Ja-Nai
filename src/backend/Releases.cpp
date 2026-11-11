#include "Releases.hpp"

#include <algorithm>
#include <utility>

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
    // P2-04: decorate-sort-undecorate. The comparator used to recompute
    // playableEpisodeCount() (a scan over episodes and sources) and teamKey()
    // (a join + toLower) on every comparison; each is now computed once per
    // release. Ordering is unchanged: same keys, same stable sort.
    struct Scored {
        Release release;
        int kind = 0;
        int playable = 0;
        QString team;
    };
    QVector<Scored> scored;
    scored.reserve(releases.size());
    for (Release& release : releases) {
        Scored entry;
        entry.kind = kindOrder(release.kind);
        entry.playable = playableEpisodeCount(release);
        entry.team = teamKey(release);
        entry.release = std::move(release);
        scored.append(std::move(entry));
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& left, const Scored& right) {
        if (left.kind != right.kind) {
            return left.kind < right.kind;
        }
        if (left.playable != right.playable) {
            return left.playable > right.playable;
        }
        return left.team < right.team;
    });
    QVector<Release> sorted;
    sorted.reserve(scored.size());
    for (Scored& entry : scored) {
        sorted.append(std::move(entry.release));
    }
    return sorted;
}

}  // namespace mikai::backend
