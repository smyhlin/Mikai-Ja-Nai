#pragma once

#include <QString>
#include <QVector>

#include <cstdint>
#include <optional>

namespace mplayer {

struct NodeValue;

enum class MediaTrackType { Audio, Subtitle };

struct MediaTrack {
    int64_t id = 0;
    MediaTrackType type = MediaTrackType::Audio;
    QString title;
    QString language;
    QString codec;
    QString externalFilename;
    int audioChannels = 0;
    bool selected = false;
    bool external = false;
    bool isDefault = false;
    bool forced = false;
    bool selectionKnown = false;
    bool externalKnown = false;
    bool defaultKnown = false;
    bool forcedKnown = false;
    friend bool operator==(const MediaTrack&, const MediaTrack&) = default;
};

struct MediaTrackSelection {
    std::optional<int64_t> audio;
    std::optional<int64_t> subtitle;
    friend bool operator==(const MediaTrackSelection&, const MediaTrackSelection&) = default;
};

struct MediaTrackSnapshot {
    uint64_t coreIdentity = 0;
    uint64_t sourceEpoch = 0;
    bool ready = false;
    QVector<MediaTrack> tracks;
    friend bool operator==(const MediaTrackSnapshot&, const MediaTrackSnapshot&) = default;
};

std::optional<QVector<MediaTrack>> parseMediaTracks(const NodeValue& value,
                                                  QString* error = nullptr);
MediaTrackSelection selectedMediaTracks(const MediaTrackSnapshot& snapshot);

} // namespace mplayer
