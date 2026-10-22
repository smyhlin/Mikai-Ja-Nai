#pragma once

#include "Models.hpp"

#include <QSet>
#include <QString>
#include <QVector>

namespace mikai::backend {

class MikaiClient;

struct UpdateFeedState {
    QString watermark;
    QVector<AnimeUpdate> items;
    QSet<QString> boundary_keys;
};

UpdateFeedState refreshUpdateFeed(const MikaiClient& client, const UpdateFeedState& previous,
                                  int retainedLimit = 12);

}  // namespace mikai::backend
