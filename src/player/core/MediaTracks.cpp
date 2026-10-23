#include "MediaTracks.hpp"

#include "MpvHandle.hpp"

#include <QSet>

#include <limits>
#include <utility>

namespace mplayer {

std::optional<QVector<MediaTrack>> parseMediaTracks(const NodeValue& value, QString* error) {
    if (error != nullptr) error->clear();
    const auto refuse = [error](const QString& reason) -> std::optional<QVector<MediaTrack>> {
        if (error != nullptr) *error = reason;
        return std::nullopt;
    };
    if (value.format != MPV_FORMAT_NODE_ARRAY) {
        return refuse(QStringLiteral("track-list is not an array"));
    }
    QVector<MediaTrack> tracks;
    QSet<int64_t> audioIds;
    QSet<int64_t> subtitleIds;
    for (const NodeValue& row : value.list) {
        if (row.format != MPV_FORMAT_NODE_MAP) {
            return refuse(QStringLiteral("track-list contains a non-object entry"));
        }
        const NodeValue* type = nullptr;
        for (const auto& [key, field] : row.map) {
            if (key == "type") {
                type = &field;
                break;
            }
        }
        if (type == nullptr || type->format != MPV_FORMAT_STRING ||
            (type->text != "audio" && type->text != "sub")) continue;
        MediaTrack track;
        track.type = type->text == "audio" ? MediaTrackType::Audio : MediaTrackType::Subtitle;
        for (const auto& [key, field] : row.map) {
            if (key == "id" && field.format == MPV_FORMAT_INT64) track.id = field.integer;
            else if (key == "title" && field.format == MPV_FORMAT_STRING)
                track.title = QString::fromStdString(field.text);
            else if (key == "lang" && field.format == MPV_FORMAT_STRING)
                track.language = QString::fromStdString(field.text);
            else if (key == "codec" && field.format == MPV_FORMAT_STRING)
                track.codec = QString::fromStdString(field.text);
            else if (key == "external-filename" && field.format == MPV_FORMAT_STRING)
                track.externalFilename = QString::fromStdString(field.text);
            else if (key == "selected" && field.format == MPV_FORMAT_FLAG)
                track.selected = field.flag;
            else if (key == "external" && field.format == MPV_FORMAT_FLAG)
                track.external = field.flag;
            else if (key == "default" && field.format == MPV_FORMAT_FLAG)
                track.isDefault = field.flag;
            else if (key == "forced" && field.format == MPV_FORMAT_FLAG)
                track.forced = field.flag;
            else if (key == "demux-channel-count" && field.format == MPV_FORMAT_INT64 &&
                     field.integer > 0 && field.integer <= std::numeric_limits<int>::max())
                track.audioChannels = static_cast<int>(field.integer);
        }
        if (track.id <= 0) return refuse(QStringLiteral("audio/subtitle track has no positive integer ID"));
        QSet<int64_t>& ids = track.type == MediaTrackType::Audio ? audioIds : subtitleIds;
        if (ids.contains(track.id)) {
            return refuse(QStringLiteral("duplicate audio/subtitle track ID"));
        }
        ids.insert(track.id);
        tracks.append(std::move(track));
    }
    return tracks;
}

MediaTrackSelection selectedMediaTracks(const MediaTrackSnapshot& snapshot) {
    if (!snapshot.ready) return {};
    MediaTrackSelection result{0, 0};
    for (const MediaTrack& track : snapshot.tracks) {
        if (!track.selected) continue;
        if (track.type == MediaTrackType::Audio) result.audio = track.id;
        else result.subtitle = track.id;
    }
    return result;
}

} // namespace mplayer
