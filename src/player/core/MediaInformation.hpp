#pragma once

#include "MediaTracks.hpp"

#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

namespace mplayer {

// Owned source metadata read asynchronously from the playback core. Empty
// optionals mean unavailable; stream bitrates are not guessed from file size.
struct MediaInformation {
    uint64_t coreIdentity = 0;
    uint64_t sourceEpoch = 0;
    QString filename;
    QString path;
    QString container;
    QString videoCodec;
    std::optional<double> duration;
    std::optional<int64_t> size;
    std::optional<double> videoBitrate;
    std::optional<double> audioBitrate;
    std::optional<int64_t> width;
    std::optional<int64_t> height;
    std::optional<double> framesPerSecond;
    bool loading = true;
    QStringList errors;
};

} // namespace mplayer
