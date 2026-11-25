#pragma once

#include <backend/Models.hpp>

#include <QSet>
#include <QString>
#include <QVector>

#include <optional>

namespace mikai::app::selection {

enum class SelectionReason {
    None,
    Explicit,
    SharedLink,
    FavouriteTeam,
    PreferredProvider,
    FirstAvailable,
};

struct SelectionInput {
    QVector<backend::Release> releases;

    // Watch history already loaded by caller.
    QString currentEpisode;
    QSet<QString> watchedEpisodes;

    // Preferences already loaded by caller.
    QString favouriteProvider;
    QString favouriteTeamSlug;
    QString defaultProvider;

    // Optional parsed shared deep link. All three (provider, team, episode)
    // must be non-empty for the shared playback target to apply.
    QString sharedProvider;
    QString sharedTeamSlug;
    QString sharedEpisode;
    std::optional<int> sharedTimeSec;

    // Optional explicit user override (e.g. switching release/provider/team).
    std::optional<int> requestedReleaseIndex;
    QString requestedProvider;
    QString requestedTeamSlug;
};

struct SelectionResult {
    bool valid = false;
    int releaseIndex = -1;
    QString provider;       // Normalised provider key (labels::providerKey)
    QString teamSlug;       // Release-local team slug (empty if release has no team)
    QString teamName;       // Display name only
    QString episodeNumber;  // String form of Episode::number ("3", "12.5", "OVA")
    int episodeIndex = -1;
    int sourceIndex = -1;
    std::optional<int> startTimeSec;
    SelectionReason reason = SelectionReason::None;
};

/// Numeric-aware comparator for string episode identifiers ("2" < "10",
/// "12" < "12.5" < "13", numeric before non-numeric like "OVA").
int compareEpisodeNumbers(const QString& a, const QString& b);

/// Pure deterministic playback selection:
/// Explicit > SharedLink > FavouriteTeam > PreferredProvider > FirstAvailable.
/// Every step validates that the target release/provider/episode actually exists.
SelectionResult resolve(const SelectionInput& input);

}  // namespace mikai::app::selection
