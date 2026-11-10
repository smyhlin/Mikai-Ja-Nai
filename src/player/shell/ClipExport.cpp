#include "ClipExport.hpp"

#include "MpvApi.hpp"
#include "MpvHandle.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QRunnable>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>


namespace mplayer::shell {
namespace {

QString errorText(const char* where, int code, mplayer::MpvApi& api) {
    const char* detail = api.functions().errorString(code);
    return QStringLiteral("%1: %2").arg(QString::fromLatin1(where),
        detail ? QString::fromUtf8(detail) : QString::number(code));
}

bool outputSize(const ClipExportRequest& request, QSize& size, QString& error) {
    if (request.maximumHeight != 480 && request.maximumHeight != 720 && request.maximumHeight != 1080) {
        error = QObject::tr("The selected output quality is not supported.");
        return false;
    }
    if (request.sourceSize.width() < 2 || request.sourceSize.height() < 2) {
        error = QObject::tr("The source video dimensions are unavailable.");
        return false;
    }
    const int sourceWidth = request.sourceSize.width();
    const int sourceHeight = request.sourceSize.height();
    const int boundedHeight = std::min(sourceHeight, request.maximumHeight);
    const long double scale = static_cast<long double>(boundedHeight) / sourceHeight;
    const long double desiredWidth = std::min(
        static_cast<long double>(sourceWidth), static_cast<long double>(sourceWidth) * scale);
    const auto nearestEven = [](long double value, int maximum) {
        const int evenMaximum = maximum & ~1;
        const long double rounded = std::min(
            std::floor(value / 2.0L + 0.5L) * 2.0L, static_cast<long double>(evenMaximum));
        return std::clamp(static_cast<int>(rounded), 2, evenMaximum);
    };
    const int width = nearestEven(desiredWidth, sourceWidth);
    const int height = nearestEven(boundedHeight, boundedHeight);
    if (width < 2 || height < 2) {
        error = QObject::tr("The source video is too small to encode as MP4.");
        return false;
    }
    size = QSize(width, height);
    return true;
}

QString sanitizedError(QString message, const ClipExportRequest& request) {
    if (!request.source.isEmpty()) message.replace(request.source, QObject::tr("the media source"));
    if (!request.externalAudioPath.isEmpty())
        message.replace(request.externalAudioPath, QObject::tr("[redacted]"));
    for (const auto& [key, value] : request.sourceOptions) {
        Q_UNUSED(key);
        if (!value.empty()) message.replace(QString::fromStdString(value), QObject::tr("[redacted]"));
    }
    return message;
}

void publish(QPointer<ClipExport> target, double progress, bool final,
             bool succeeded, const QString& path, const QString& error) {
    QCoreApplication* dispatcher = QCoreApplication::instance();
    if (dispatcher == nullptr) return;
    QMetaObject::invokeMethod(dispatcher, [target, progress, final, succeeded, path, error] {
        if (!target) return;
        if (final) emit target->finished(succeeded, path, error);
        else emit target->progressChanged(progress);
    }, Qt::QueuedConnection);
}

bool readU32(QFile& file, qint64 offset, quint32& value) {
    if (!file.seek(offset)) return false;
    char bytes[4];
    if (file.read(bytes, 4) != 4) return false;
    value = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(bytes));
    return true;
}

bool inspectMp4Boxes(QFile& file, qint64 begin, qint64 end, int depth,
                     const std::string& codec, bool& video, bool& audio, bool& mediaData) {
    if (depth > 8) return false;
    qint64 offset = begin;
    while (offset + 8 <= end) {
        quint32 size32 = 0;
        if (!readU32(file, offset, size32)) return false;
        if (!file.seek(offset + 4)) return false;
        char typeBytes[4];
        if (file.read(typeBytes, 4) != 4) return false;
        const QByteArray type(typeBytes, 4);
        qint64 header = 8;
        quint64 boxSize = size32;
        if (size32 == 1) {
            char bytes[8];
            if (!file.seek(offset + 8) || file.read(bytes, 8) != 8) return false;
            boxSize = qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(bytes));
            header = 16;
        } else if (size32 == 0) {
            boxSize = quint64(end - offset);
        }
        if (boxSize < quint64(header) || boxSize > quint64(end - offset)) return false;
        const qint64 boxEnd = offset + qint64(boxSize);
        const qint64 payload = offset + header;
        if (type == "mdat") {
            mediaData = true;
        } else if (type == "stsd") {
            quint32 entries = 0;
            if (!readU32(file, payload + 4, entries)) return false;
            qint64 entry = payload + 8;
            for (quint32 i = 0; i < entries; ++i) {
                quint32 entrySize = 0;
                if (!readU32(file, entry, entrySize) || entrySize < 8 ||
                    quint64(entrySize) > quint64(boxEnd - entry)) return false;
                if (!file.seek(entry + 4)) return false;
                char entryTypeBytes[4];
                if (file.read(entryTypeBytes, 4) != 4) return false;
                const QByteArray entryType(entryTypeBytes, 4);
                if ((entryType == "avc1" && codec == "libx264") ||
                    (entryType == "mp4v" && codec == "mpeg4")) video = true;
                if (entryType == "mp4a") audio = true;
                entry += entrySize;
            }
        } else if (type == "moov" || type == "trak" || type == "mdia" ||
                   type == "minf" || type == "stbl" || type == "edts" ||
                   type == "dinf" || type == "udta") {
            if (!inspectMp4Boxes(file, payload, boxEnd, depth + 1, codec, video, audio, mediaData)) return false;
        }
        offset = boxEnd;
    }
    return offset == end;
}

bool mp4ContainsStreams(const QString& path, const std::string& codec, bool requireAudio) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 16) return false;
    quint32 ftypSize = 0;
    if (!readU32(file, 0, ftypSize) || ftypSize < 8 ||
        !file.seek(4) || file.read(4) != QByteArrayLiteral("ftyp")) return false;
    bool video = false;
    bool audio = false;
    bool mediaData = false;
    if (!inspectMp4Boxes(file, 0, file.size(), 0, codec, video, audio, mediaData)) return false;
    return video && mediaData && (!requireAudio || audio);
}

void publishCodec(QPointer<ClipExport> target, const QString& codec) {
    QCoreApplication* dispatcher = QCoreApplication::instance();
    if (dispatcher == nullptr) return;
    QMetaObject::invokeMethod(dispatcher, [target, codec] {
        if (target) emit target->encodingStarted(codec);
    }, Qt::QueuedConnection);
}

bool hasSelectedAudioTrack(mpv_handle* handle, mplayer::MpvApi& api,
                          qint64 requestedTrack, const QString& expectedExternalPath) {
    mpv_node node{};
    if (api.functions().getProperty(handle, "track-list", MPV_FORMAT_NODE, &node) < 0) return false;
    bool selected = false;
    if (node.format == MPV_FORMAT_NODE_ARRAY && node.u.list != nullptr) {
        for (int i = 0; i < node.u.list->num && !selected; ++i) {
            const mpv_node& item = node.u.list->values[i];
            if (item.format != MPV_FORMAT_NODE_MAP || item.u.list == nullptr) continue;
            const char* type = nullptr;
            bool active = false;
            bool external = false;
            const char* externalFilename = nullptr;
            qint64 id = -1;
            for (int field = 0; field < item.u.list->num; ++field) {
                const char* key = item.u.list->keys[field];
                const mpv_node& value = item.u.list->values[field];
                if (key == nullptr) continue;
                if (std::strcmp(key, "type") == 0 && value.format == MPV_FORMAT_STRING)
                    type = value.u.string;
                else if (std::strcmp(key, "selected") == 0 && value.format == MPV_FORMAT_FLAG)
                    active = value.u.flag != 0;
                else if (std::strcmp(key, "external") == 0 && value.format == MPV_FORMAT_FLAG)
                    external = value.u.flag != 0;
                else if (std::strcmp(key, "external-filename") == 0 &&
                         value.format == MPV_FORMAT_STRING)
                    externalFilename = value.u.string;
                else if (std::strcmp(key, "id") == 0 && value.format == MPV_FORMAT_INT64)
                    id = value.u.int64;
            }
            bool matchingExternal = false;
            if (external && externalFilename != nullptr && !expectedExternalPath.isEmpty()) {
                const QString selectedPath =
                    QFileInfo(QString::fromUtf8(externalFilename)).canonicalFilePath();
                const QString requestedPath = QFileInfo(expectedExternalPath).canonicalFilePath();
                matchingExternal = !selectedPath.isEmpty() && selectedPath == requestedPath;
            }
            selected = type != nullptr && std::strcmp(type, "audio") == 0 && active &&
                       (requestedTrack == 0 || id == requestedTrack) &&
                       (expectedExternalPath.isEmpty() || matchingExternal);
        }
    }
    api.functions().freeNodeContents(&node);
    return selected;
}

bool encoderAvailable(mplayer::MpvHandle& handle, const char* name) {
    mplayer::MpvApi& api = mplayer::MpvApi::instance();
    mpv_node node{};
    const int rc = api.functions().getProperty(handle.raw(), "encoder-list", MPV_FORMAT_NODE, &node);
    if (rc < 0) return false;
    bool found = false;
    if (node.format == MPV_FORMAT_NODE_ARRAY && node.u.list != nullptr) {
        for (int i = 0; i < node.u.list->num && !found; ++i) {
            const mpv_node& item = node.u.list->values[i];
            if (item.format != MPV_FORMAT_NODE_MAP || item.u.list == nullptr) continue;
            for (int field = 0; field < item.u.list->num; ++field) {
                const char* key = item.u.list->keys[field];
                const mpv_node& value = item.u.list->values[field];
                if (key != nullptr && std::strcmp(key, "driver") == 0 &&
                    value.format == MPV_FORMAT_STRING && value.u.string != nullptr &&
                    std::strcmp(value.u.string, name) == 0) {
                    found = true;
                    break;
                }
            }
        }
    }
    api.functions().freeNodeContents(&node);
    return found;
}

}  // namespace

struct ClipExport::State {
    std::atomic_bool cancelled{false};
    std::atomic_bool running{false};
};

ClipExport::ClipExport(QObject* parent) : QObject(parent), state_(std::make_shared<State>()) {}

ClipExport::~ClipExport() { cancel(); }

void ClipExport::cancel() {
    state_->cancelled.store(true, std::memory_order_relaxed);
}

void ClipExport::start(const ClipExportRequest& request) {
    if (state_->running.exchange(true, std::memory_order_acq_rel)) {
        emit finished(false, request.outputPath, tr("An export is already in progress."));
        return;
    }
    state_->cancelled.store(false, std::memory_order_relaxed);
    const auto state = state_;
    const QPointer<ClipExport> target(this);
    QThreadPool::globalInstance()->start(QRunnable::create([state, target, request] {
        QString failure;
        QString outputPath = request.outputPath;
        QString tempPath;
        bool succeeded = false;
        auto done = [&] {
            state->running.store(false, std::memory_order_release);
            if (succeeded) publish(target, 1.0, false, false, outputPath, {});
            publish(target, 0.0, true, succeeded, outputPath, sanitizedError(failure, request));
        };
        const double length = request.endSeconds - request.startSeconds;
        if (request.source.trimmed().isEmpty() || request.outputPath.trimmed().isEmpty() ||
            !std::isfinite(request.startSeconds) || !std::isfinite(request.endSeconds) ||
            request.startSeconds < 0.0 || request.endSeconds <= request.startSeconds ||
            length > kMaximumClipDuration) {
            failure = QObject::tr("The selected clip range is invalid (clips may be up to 60 seconds).");
            done();
            return;
        }
        if (state->cancelled.load(std::memory_order_relaxed)) {
            failure = QObject::tr("Export cancelled."); done(); return;
        }
        const QFileInfo destination(request.outputPath);
        const bool destinationEntryExists = destination.exists() || destination.isSymLink();
        if (destinationEntryExists && destination.isSymLink()) {
            failure = QObject::tr("The destination is a symbolic link and cannot be replaced.");
            done(); return;
        }
        if (destinationEntryExists && !request.replaceExisting) {
            failure = QObject::tr("The destination already exists."); done(); return;
        }
        if (destinationEntryExists && !destination.isFile()) {
            failure = QObject::tr("Only an existing regular file may be replaced.");
            done(); return;
        }
        if (!destination.dir().exists()) {
            failure = QObject::tr("The destination folder does not exist."); done(); return;
        }
        QSize size;
        if (!outputSize(request, size, failure)) { done(); return; }

        QTemporaryFile temporary(destination.dir().filePath(QStringLiteral(".clip-export-XXXXXX.mp4")));
        if (!temporary.open()) {
            failure = QObject::tr("Could not create a temporary MP4 beside the destination: %1").arg(temporary.errorString());
            if (!temporary.remove() && QFileInfo::exists(temporary.fileName()))
                qWarning() << "Could not remove clip export temporary file after"
                           << failure << ":" << temporary.errorString();
            done(); return;
        }
        tempPath = temporary.fileName();
        temporary.close();
        try {
            std::string codec = "libx264";
            {
                mplayer::MpvHandle probe({{"config", "no"}, {"vo", "null"}, {"audio", "no"}});
                if (!encoderAvailable(probe, "libx264")) {
                    if (!encoderAvailable(probe, "mpeg4")) {
                        throw std::runtime_error("The selected mpv runtime provides neither libx264 nor the MPEG-4 Part 2 encoder.");
                    }
                    codec = "mpeg4";
                }
            }

            const bool audioOff = request.externalAudioPath.isEmpty() &&
                                  request.audioTrackId && *request.audioTrackId == 0;
            std::vector<std::pair<std::string, std::string>> options = request.sourceOptions;
            options.emplace_back("config", "no");
            options.emplace_back("terminal", "no");
            options.emplace_back("input-default-bindings", "no");
            options.emplace_back("hr-seek", "yes");
            options.emplace_back("o", tempPath.toStdString());
            options.emplace_back("of", "mp4");
            options.emplace_back("ovc", codec);
            // libavcodec global_quality uses FF_QP2LAMBDA units: quantizer 4 × 118.
            options.emplace_back("ovcopts", codec == "libx264"
                ? "preset=veryfast,crf=23,profile=high"
                : "flags=+qscale,global_quality=472");
            options.emplace_back("oac", audioOff ? "no" : "aac");
            options.emplace_back("ofopts", "movflags=+faststart");
            options.emplace_back("vf", "scale=" + std::to_string(size.width()) + ":" +
                std::to_string(size.height()) + ",format=fmt=yuv420p");
            if (audioOff) options.emplace_back("audio", "no");
            if (!request.externalAudioPath.isEmpty())
                options.emplace_back("aid", "auto");
            else if (request.audioTrackId && *request.audioTrackId > 0)
                options.emplace_back("aid", std::to_string(*request.audioTrackId));
            auto handle = std::make_unique<mplayer::MpvHandle>(options);
            mplayer::MpvApi& api = mplayer::MpvApi::instance();
            const int logResult = api.functions().requestLogMessages(handle->raw(), "error");
            if (logResult < 0)
                throw std::runtime_error(errorText("mpv_request_log_messages", logResult, api).toStdString());
            if (!request.externalAudioPath.isEmpty()) {
                const QByteArray externalAudioPath = request.externalAudioPath.toUtf8();
                mpv_node pathNode{};
                pathNode.format = MPV_FORMAT_STRING;
                pathNode.u.string = const_cast<char*>(externalAudioPath.constData());
                mpv_node_list pathList{};
                pathList.num = 1;
                pathList.values = &pathNode;
                mpv_node audioFilesNode{};
                audioFilesNode.format = MPV_FORMAT_NODE_ARRAY;
                audioFilesNode.u.list = &pathList;
                const int audioFilesResult = api.functions().setProperty(
                    handle->raw(), "audio-files", MPV_FORMAT_NODE, &audioFilesNode);
                if (audioFilesResult < 0)
                    throw std::runtime_error(errorText(
                        "mpv_set_property audio-files", audioFilesResult, api).toStdString());
            }
            const QString startValue = QString::fromStdString(mpvNumber(request.startSeconds));
            const QString lengthValue = QString::fromStdString(mpvNumber(length));
            const std::string source = request.source.toStdString();
            std::string clipOptions = ("start=" + startValue + ",length=" + lengthValue).toStdString();
            if (request.externalAudioPath.isEmpty() && request.audioTrackId &&
                *request.audioTrackId > 0)
                clipOptions += ",aid=" + std::to_string(*request.audioTrackId);
            publishCodec(target, codec == "libx264" ? QObject::tr("H.264") : QObject::tr("MPEG-4 Part 2"));
            const char* command[] = {"loadfile", source.c_str(), "replace", "-1", clipOptions.c_str(), nullptr};
            int rc = api.functions().command(handle->raw(), command);
            if (rc < 0) throw std::runtime_error(errorText("mpv loadfile", rc, api).toStdString());
            publish(target, 0.0, false, false, outputPath, {});
            bool ended = false;
            bool requireAudio = false;
            while (!ended) {
                if (state->cancelled.load(std::memory_order_relaxed)) {
                    failure = QObject::tr("Export cancelled.");
                    const char* stop[] = {"stop", nullptr};
                    api.functions().command(handle->raw(), stop);
                    break;
                }
                mpv_event* event = api.functions().waitEvent(handle->raw(), 0.1);
                if (event == nullptr || event->event_id == MPV_EVENT_NONE) {
                    double currentTime = 0.0;
                    if (api.functions().getProperty(handle->raw(), "time-pos", MPV_FORMAT_DOUBLE, &currentTime) >= 0 &&
                        std::isfinite(currentTime)) {
                        const double fraction = std::clamp((currentTime - request.startSeconds) / length, 0.0, 0.99);
                        publish(target, fraction, false, false, outputPath, {});
                    }
                    continue;
                }
                if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
                    const auto* message = static_cast<const mpv_event_log_message*>(event->data);
                    if (message != nullptr && message->level != nullptr &&
                        (std::strcmp(message->level, "error") == 0 ||
                         std::strcmp(message->level, "fatal") == 0) && message->text != nullptr) {
                        failure = QString::fromUtf8(message->text).trimmed();
                    }
                } else if (event->event_id == MPV_EVENT_END_FILE) {
                    const auto* end = static_cast<const mpv_event_end_file*>(event->data);
                    ended = true;
                    if (end == nullptr || end->reason != MPV_END_FILE_REASON_EOF || end->error < 0) {
                        if (end != nullptr && end->error < 0)
                            failure = errorText("mpv encoding failed", end->error, api);
                        else
                            failure = QObject::tr("mpv ended the encode before normal end-of-file (reason %1).")
                                .arg(end ? int(end->reason) : -1);
                    }
                } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
                    const bool externalAudioRequested = !request.externalAudioPath.isEmpty();
                    const qint64 requestedAudio = externalAudioRequested
                        ? 0 : request.audioTrackId.value_or(0);
                    requireAudio = !audioOff && hasSelectedAudioTrack(
                        handle->raw(), api, requestedAudio,
                        externalAudioRequested ? request.externalAudioPath : QString());
                    const bool audioExplicit = !request.externalAudioPath.isEmpty() ||
                        (request.audioTrackId && *request.audioTrackId > 0);
                    if (audioExplicit && !requireAudio) {
                        failure = QObject::tr("The selected audio track could not be loaded.");
                        const char* stop[] = {"stop", nullptr};
                        api.functions().command(handle->raw(), stop);
                    }
                    publish(target, 0.1, false, false, outputPath, {});
                }
            }
            handle.reset();
            if (state->cancelled.load(std::memory_order_relaxed)) {
                if (failure.isEmpty()) failure = QObject::tr("Export cancelled.");
            } else if (failure.isEmpty()) {
                QFile encoded(tempPath);
                if (!encoded.open(QIODevice::ReadOnly) || encoded.size() <= 0 ||
                    !mp4ContainsStreams(tempPath, codec, requireAudio)) {
                    failure = QObject::tr("The encoder completed without producing the requested MP4 video and audio streams.");
                } else {
                    encoded.close();
                    if (state->cancelled.load(std::memory_order_relaxed)) {
                        failure = QObject::tr("Export cancelled.");
                    } else if (request.replaceExisting) {
                        if (temporary.renameOverwrite(request.outputPath)) {
                            temporary.setAutoRemove(false);
                            succeeded = true;
                        } else {
                            failure = QObject::tr("Could not atomically publish the exported clip: %1")
                                .arg(temporary.errorString());
                        }
                    } else if (temporary.rename(request.outputPath)) {
                        temporary.setAutoRemove(false);
                        succeeded = true;
                    } else {
                        failure = QObject::tr("Could not atomically publish the exported clip: %1")
                            .arg(temporary.errorString());
                    }
                }
            }
        } catch (const std::exception& error) {
            failure = QString::fromUtf8(error.what());
        }
        if (!succeeded && temporary.isOpen()) temporary.close();
        if (!succeeded && !temporary.remove())
            qWarning() << "Could not remove clip export temporary file after"
                       << failure << ":" << temporary.errorString();
        done();
    }));
}

}  // namespace mplayer::shell
