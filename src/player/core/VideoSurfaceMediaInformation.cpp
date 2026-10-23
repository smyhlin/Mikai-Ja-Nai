#include "VideoSurface.hpp"

#include <array>
#include <cmath>
#include <memory>
#include <utility>

namespace mplayer {

void VideoSurface::cancelMediaInformation() {
    ++mediaInformationGeneration_;
    if (mpv_ != nullptr) {
        for (const AsyncRequestId id : mediaInformationRequests_) mpv_->cancelAsync(id);
    }
    mediaInformationRequests_.clear();
}

bool VideoSurface::requestMediaInformation(
    std::function<void(const MediaInformation&)> callback) {
    cancelMediaInformation();
    if (mpv_ == nullptr || !mediaTracksLoaded_ ||
        entrySourceEpoch_ != mpv_->sourceEpoch() || !callback) return false;

    struct Field {
        const char* name;
        mpv_format format;
    };
    static constexpr std::array fields{
        Field{"filename", MPV_FORMAT_STRING}, Field{"path", MPV_FORMAT_STRING},
        Field{"file-format", MPV_FORMAT_STRING}, Field{"duration", MPV_FORMAT_DOUBLE},
        Field{"file-size", MPV_FORMAT_INT64}, Field{"video-bitrate", MPV_FORMAT_DOUBLE},
        Field{"audio-bitrate", MPV_FORMAT_DOUBLE}, Field{"video-params", MPV_FORMAT_NODE},
        Field{"container-fps", MPV_FORMAT_DOUBLE}, Field{"video-codec", MPV_FORMAT_STRING}};
    struct Pending {
        MediaInformation information;
        size_t remaining = fields.size();
        std::function<void(const MediaInformation&)> publish;
    };
    auto pending = std::make_shared<Pending>();
    pending->information.coreIdentity = mpv_->coreIdentity();
    pending->information.sourceEpoch = mpv_->sourceEpoch();
    pending->publish = std::move(callback);
    const uint64_t generation = mediaInformationGeneration_;
    mediaInformationRequests_.reserve(fields.size());
    for (size_t index = 0; index < fields.size(); ++index) {
        const Field field = fields[index];
        const auto receive = [this, pending, generation, index, field](const AsyncReply& reply) {
            if (mpv_ == nullptr || generation != mediaInformationGeneration_ ||
                pending->information.coreIdentity != mpv_->coreIdentity() ||
                pending->information.sourceEpoch != mpv_->sourceEpoch() ||
                !mediaTracksLoaded_ || entrySourceEpoch_ != mpv_->sourceEpoch()) return;
            MediaInformation& info = pending->information;
            if (!reply.ok()) {
                info.errors.append(QStringLiteral("%1: %2")
                    .arg(QString::fromLatin1(field.name),
                         QString::fromStdString(mpv_->describeError(reply.error))));
            } else if (reply.format == field.format) {
                switch (index) {
                case 0: info.filename = QString::fromStdString(reply.text); break;
                case 1: info.path = QString::fromStdString(reply.text); break;
                case 2: info.container = QString::fromStdString(reply.text); break;
                case 3:
                    if (std::isfinite(reply.number) && reply.number >= 0) info.duration = reply.number;
                    break;
                case 4:
                    if (reply.integer >= 0) info.size = reply.integer;
                    break;
                case 5:
                    if (std::isfinite(reply.number) && reply.number > 0) info.videoBitrate = reply.number;
                    break;
                case 6:
                    if (std::isfinite(reply.number) && reply.number > 0) info.audioBitrate = reply.number;
                    break;
                case 7:
                    if (reply.node.format == MPV_FORMAT_NODE_MAP) {
                        for (const auto& [key, value] : reply.node.map) {
                            if (value.format != MPV_FORMAT_INT64 || value.integer <= 0) continue;
                            if (key == "w") info.width = value.integer;
                            else if (key == "h") info.height = value.integer;
                        }
                    }
                    break;
                case 8:
                    if (std::isfinite(reply.number) && reply.number > 0)
                        info.framesPerSecond = reply.number;
                    break;
                case 9: info.videoCodec = QString::fromStdString(reply.text); break;
                }
            }
            info.loading = --pending->remaining != 0;
            pending->publish(info);
        };
        const AsyncRequestId request = mpv_->getPropertyAsync(field.name, field.format, receive);
        if (request.valid()) {
            mediaInformationRequests_.push_back(request);
        } else {
            AsyncReply refused;
            refused.error = MPV_ERROR_GENERIC;
            receive(refused);
        }
    }
    return true;
}

} // namespace mplayer
