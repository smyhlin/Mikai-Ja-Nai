#pragma once

#include <QString>
#include <QVector>

#include "Models.hpp"

namespace mikai::backend {

// Episodes a viewer can actually press play on: those carrying at least one
// source with an embed url. Releases that list no episodes at all fall back
// to the advertised episode count.
int playableEpisodeCount(const Release& release);

// Voice before subtitles, then the release with the most playable episodes,
// then team names alphabetically -- the order the title page shows tabs in.
QVector<Release> sortReleases(QVector<Release> releases);

}  // namespace mikai::backend
