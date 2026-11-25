#include "app/support/PlaybackSelection.hpp"

#include "app/support/Labels.hpp"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace mikai::app::selection {
namespace {

struct ParsedNumber {
    bool numeric = false;
    double value = 0.0;
    QString suffix;
};

ParsedNumber parseEpisodeNumber(const QString& raw) {
    const QString trimmed = raw.trimmed();
    static const QRegularExpression rx(QStringLiteral("^([0-9]+(?:\\.[0-9]+)?)(.*)$"));
    const QRegularExpressionMatch match = rx.match(trimmed);
    if (!match.hasMatch()) {
        return {false, 0.0, trimmed};
    }
    bool ok = false;
    const double val = match.captured(1).toDouble(&ok);
    if (!ok || !std::isfinite(val)) {
        return {false, 0.0, trimmed};
    }
    return {true, val, match.captured(2).trimmed()};
}

QString episodeKey(const backend::Episode& ep) {
    return labels::variant(ep.number).trimmed();
}

bool releaseMatchesTeam(const backend::Release& release, const QString& teamSlug) {
    const QString want = teamSlug.trimmed();
    if (want.isEmpty()) return release.teams.isEmpty();
    for (const backend::Team& team : release.teams) {
        if (team.slug.trimmed().compare(want, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString primaryTeamSlug(const backend::Release& release) {
    for (const backend::Team& team : release.teams) {
        if (!team.slug.trimmed().isEmpty()) return team.slug.trimmed();
    }
    return {};
}

QString primaryTeamName(const backend::Release& release, const QString& preferredSlug) {
    const QString want = preferredSlug.trimmed();
    if (!want.isEmpty()) {
        for (const backend::Team& team : release.teams) {
            if (team.slug.trimmed().compare(want, Qt::CaseInsensitive) == 0) {
                return team.name.trimmed();
            }
        }
    }
    for (const backend::Team& team : release.teams) {
        if (!team.name.trimmed().isEmpty()) return team.name.trimmed();
    }
    return {};
}

int findSourceForProvider(const backend::Episode& ep, const QString& normalizedProvider) {
    for (int i = 0; i < ep.sources.size(); ++i) {
        if (ep.sources[i].embed_url.trimmed().isEmpty()) continue;
        if (normalizedProvider.isEmpty() ||
            labels::providerKey(labels::providerFor(ep.sources[i])) == normalizedProvider) {
            return i;
        }
    }
    return -1;
}

bool releaseHasProvider(const backend::Release& release, const QString& normalizedProvider) {
    for (const backend::Episode& ep : release.episodes) {
        if (findSourceForProvider(ep, normalizedProvider) >= 0) return true;
    }
    return false;
}

QStringList providersInRelease(const backend::Release& release) {
    QStringList out;
    for (const backend::Episode& ep : release.episodes) {
        for (const backend::Source& src : ep.sources) {
            if (src.embed_url.trimmed().isEmpty()) continue;
            const QString key = labels::providerKey(labels::providerFor(src));
            if (!key.isEmpty() && !out.contains(key)) out.append(key);
        }
    }
    return out;
}

SelectionResult buildForReleaseProvider(const QVector<backend::Release>& releases,
                                        int releaseIndex,
                                        const QString& normalizedProvider,
                                        const QString& preferredTeamSlug,
                                        const QString& exactEpisode,
                                        const QString& currentEpisode,
                                        const QSet<QString>& watchedEpisodes,
                                        std::optional<int> startTimeSec,
                                        SelectionReason reason) {
    if (releaseIndex < 0 || releaseIndex >= releases.size()) return {};
    const backend::Release& release = releases[releaseIndex];

    struct CandidateEp {
        int epIdx;
        int srcIdx;
        QString key;
    };
    QVector<CandidateEp> available;
    for (int i = 0; i < release.episodes.size(); ++i) {
        const int srcIdx = findSourceForProvider(release.episodes[i], normalizedProvider);
        if (srcIdx >= 0) {
            available.push_back({i, srcIdx, episodeKey(release.episodes[i])});
        }
    }
    if (available.isEmpty()) return {};

    // Sort indices by numeric-aware episode order while keeping original index for lookup.
    std::stable_sort(available.begin(), available.end(),
                     [](const CandidateEp& a, const CandidateEp& b) {
                         return compareEpisodeNumbers(a.key, b.key) < 0;
                     });

    int chosenPos = 0;
    bool matchedExact = false;

    // 1. Exact episode (e.g. from shared link).
    if (!exactEpisode.trimmed().isEmpty()) {
        const QString want = exactEpisode.trimmed();
        for (int i = 0; i < available.size(); ++i) {
            if (available[i].key.compare(want, Qt::CaseInsensitive) == 0) {
                chosenPos = i;
                matchedExact = true;
                break;
            }
        }
        if (!matchedExact) return {};
    } else {
        // 2. Resume/next episode from watch history:
        //    - If currentEpisode is present and not watched, land on it.
        //    - If currentEpisode is watched (or absent), land on the first unwatched
        //      episode strictly after currentEpisode / highest watched episode.
        const QString cur = currentEpisode.trimmed();
        int anchorPos = -1;
        if (!cur.isEmpty()) {
            for (int i = 0; i < available.size(); ++i) {
                if (available[i].key.compare(cur, Qt::CaseInsensitive) == 0) {
                    anchorPos = i;
                    break;
                }
            }
        }
        if (anchorPos >= 0) {
            if (!watchedEpisodes.contains(available[anchorPos].key)) {
                chosenPos = anchorPos;
            } else if (anchorPos + 1 < available.size()) {
                chosenPos = anchorPos + 1;
            } else {
                chosenPos = anchorPos;
            }
        } else if (!watchedEpisodes.isEmpty()) {
            int lastWatched = -1;
            for (int i = 0; i < available.size(); ++i) {
                if (watchedEpisodes.contains(available[i].key)) lastWatched = i;
            }
            if (lastWatched >= 0 && lastWatched + 1 < available.size()) {
                chosenPos = lastWatched + 1;
            } else if (lastWatched >= 0) {
                chosenPos = lastWatched;
            }
        }
    }

    const CandidateEp& picked = available[chosenPos];
    SelectionResult result;
    result.valid = true;
    result.releaseIndex = releaseIndex;
    result.provider = labels::providerKey(
        labels::providerFor(release.episodes[picked.epIdx].sources[picked.srcIdx]));
    result.teamSlug = !preferredTeamSlug.trimmed().isEmpty() &&
                              releaseMatchesTeam(release, preferredTeamSlug)
                          ? preferredTeamSlug.trimmed()
                          : primaryTeamSlug(release);
    result.teamName = primaryTeamName(release, result.teamSlug);
    result.episodeNumber = picked.key;
    result.episodeIndex = picked.epIdx;
    result.sourceIndex = picked.srcIdx;
    result.startTimeSec = matchedExact ? startTimeSec : std::nullopt;
    result.reason = reason;
    return result;
}

}  // namespace

int compareEpisodeNumbers(const QString& a, const QString& b) {
    const ParsedNumber pa = parseEpisodeNumber(a);
    const ParsedNumber pb = parseEpisodeNumber(b);
    if (pa.numeric && pb.numeric) {
        if (pa.value < pb.value) return -1;
        if (pa.value > pb.value) return 1;
        return pa.suffix.compare(pb.suffix, Qt::CaseInsensitive);
    }
    if (pa.numeric != pb.numeric) return pa.numeric ? -1 : 1;
    return a.trimmed().compare(b.trimmed(), Qt::CaseInsensitive);
}

SelectionResult resolve(const SelectionInput& input) {
    if (input.releases.isEmpty()) return {};

    const QString defaultProv = labels::providerKey(input.defaultProvider);

    // 0. Explicit override (e.g. user clicked a release/provider/team).
    if (input.requestedReleaseIndex.has_value()) {
        const int idx = *input.requestedReleaseIndex;
        if (idx >= 0 && idx < input.releases.size()) {
            const QString reqProv = labels::providerKey(input.requestedProvider);
            QString provToUse = reqProv;
            if (provToUse.isEmpty() || !releaseHasProvider(input.releases[idx], provToUse)) {
                const QStringList ordered =
                    labels::orderedProviders(providersInRelease(input.releases[idx]), defaultProv);
                provToUse = ordered.isEmpty() ? QString() : ordered.first();
            }
            if (!provToUse.isEmpty()) {
                SelectionResult res = buildForReleaseProvider(
                    input.releases, idx, provToUse, input.requestedTeamSlug, QString(),
                    input.currentEpisode, input.watchedEpisodes, std::nullopt,
                    SelectionReason::Explicit);
                if (res.valid) return res;
            }
        }
    }

    // 1. Shared deep link (provider and episode are required; when teamSlug is
    //    provided it must match the release, and when omitted we prefer any
    //    favourite-team release carrying sharedProv+sharedEp before falling back
    //    to the first release carrying sharedProv+sharedEp).
    const QString sharedProv = labels::providerKey(input.sharedProvider);
    const QString sharedTeam = input.sharedTeamSlug.trimmed();
    const QString sharedEp = input.sharedEpisode.trimmed();
    const QString favTeam = input.favouriteTeamSlug.trimmed();
    if (!sharedProv.isEmpty() && !sharedEp.isEmpty()) {
        if (!sharedTeam.isEmpty()) {
            for (int i = 0; i < input.releases.size(); ++i) {
                if (!releaseMatchesTeam(input.releases[i], sharedTeam)) continue;
                if (!releaseHasProvider(input.releases[i], sharedProv)) continue;
                SelectionResult res = buildForReleaseProvider(
                    input.releases, i, sharedProv, sharedTeam, sharedEp,
                    input.currentEpisode, input.watchedEpisodes, input.sharedTimeSec,
                    SelectionReason::SharedLink);
                if (res.valid) return res;
            }
        } else {
            if (!favTeam.isEmpty()) {
                for (int i = 0; i < input.releases.size(); ++i) {
                    if (!releaseMatchesTeam(input.releases[i], favTeam)) continue;
                    if (!releaseHasProvider(input.releases[i], sharedProv)) continue;
                    SelectionResult res = buildForReleaseProvider(
                        input.releases, i, sharedProv, favTeam, sharedEp,
                        input.currentEpisode, input.watchedEpisodes, input.sharedTimeSec,
                        SelectionReason::SharedLink);
                    if (res.valid) return res;
                }
            }
            for (int i = 0; i < input.releases.size(); ++i) {
                if (!releaseHasProvider(input.releases[i], sharedProv)) continue;
                SelectionResult res = buildForReleaseProvider(
                    input.releases, i, sharedProv, QString(), sharedEp,
                    input.currentEpisode, input.watchedEpisodes, input.sharedTimeSec,
                    SelectionReason::SharedLink);
                if (res.valid) return res;
            }
        }
    }

    // 2. Per-title favourite team (must verify provider & team actually exist!).
    const QString favProv = labels::providerKey(input.favouriteProvider);
    if (!favProv.isEmpty() && !favTeam.isEmpty()) {
        for (int i = 0; i < input.releases.size(); ++i) {
            if (!releaseMatchesTeam(input.releases[i], favTeam)) continue;
            if (!releaseHasProvider(input.releases[i], favProv)) continue;
            SelectionResult res = buildForReleaseProvider(
                input.releases, i, favProv, favTeam, QString(), input.currentEpisode,
                input.watchedEpisodes, std::nullopt, SelectionReason::FavouriteTeam);
            if (res.valid) return res;
        }
    }
    // If favourite team exists on another provider (stored provider disappeared),
    // fall back to the same team with preferred/first provider before dropping team.
    if (!favTeam.isEmpty()) {
        for (int i = 0; i < input.releases.size(); ++i) {
            if (!releaseMatchesTeam(input.releases[i], favTeam)) continue;
            const QStringList ordered =
                labels::orderedProviders(providersInRelease(input.releases[i]), defaultProv);
            for (const QString& prov : ordered) {
                SelectionResult res = buildForReleaseProvider(
                    input.releases, i, prov, favTeam, QString(), input.currentEpisode,
                    input.watchedEpisodes, std::nullopt, SelectionReason::FavouriteTeam);
                if (res.valid) return res;
            }
        }
    }

    // 3. Global default provider.
    if (!defaultProv.isEmpty()) {
        for (int i = 0; i < input.releases.size(); ++i) {
            if (!releaseHasProvider(input.releases[i], defaultProv)) continue;
            SelectionResult res = buildForReleaseProvider(
                input.releases, i, defaultProv, QString(), QString(), input.currentEpisode,
                input.watchedEpisodes, std::nullopt, SelectionReason::PreferredProvider);
            if (res.valid) return res;
        }
    }

    // 4. First available release/provider.
    for (int i = 0; i < input.releases.size(); ++i) {
        const QStringList ordered =
            labels::orderedProviders(providersInRelease(input.releases[i]), defaultProv);
        for (const QString& prov : ordered) {
            SelectionResult res = buildForReleaseProvider(
                input.releases, i, prov, QString(), QString(), input.currentEpisode,
                input.watchedEpisodes, std::nullopt, SelectionReason::FirstAvailable);
            if (res.valid) return res;
        }
    }

    return {};
}

}  // namespace mikai::app::selection
