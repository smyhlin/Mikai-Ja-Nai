#include "MediaInformationPresentation.hpp"

#include <QCoreApplication>
#include <QLocale>
#include <QStringList>

#include <cmath>
#include <limits>
#include <utility>

namespace mplayer::shell {
namespace {

QString availableText(const QString& value) {
    return value.isEmpty()
        ? QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Unavailable")
        : value;
}

QString flagText(bool value, bool known) {
    if (!known)
        return QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Unavailable");
    return value
        ? QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Yes")
        : QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "No");
}

QString durationText(const std::optional<double>& seconds, const QLocale& locale) {
    if (!seconds || !std::isfinite(*seconds) || *seconds < 0)
        return availableText({});
    const long double milliseconds = std::round(static_cast<long double>(*seconds) * 1000.0L);
    // Keep out-of-range durations as seconds to avoid an overflowing integer cast.
    if (milliseconds >= static_cast<long double>(std::numeric_limits<quint64>::max()))
        return QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "%1 s")
            .arg(locale.toString(*seconds, 'g', QLocale::FloatingPointShortest));
    const quint64 total = static_cast<quint64>(milliseconds);
    QLocale clockLocale(locale);
    clockLocale.setNumberOptions(clockLocale.numberOptions() | QLocale::OmitGroupSeparator);
    const auto padded = [&clockLocale](quint64 number, int digits) {
        QString text = clockLocale.toString(number);
        const QString zero = clockLocale.toString(0);
        while (text.size() < digits) text.prepend(zero);
        return text;
    };
    QString clock = QStringLiteral("%1:%2:%3")
        .arg(padded(total / 3600000, 2), padded(total / 60000 % 60, 2),
             padded(total / 1000 % 60, 2));
    if (total % 1000 != 0)
        clock += clockLocale.decimalPoint() + padded(total % 1000, 3);
    return clock;
}

QString sizeText(const std::optional<int64_t>& size, const QLocale& locale) {
    if (!size || *size < 0) return availableText({});
    const QString exact = QCoreApplication::translate(
        "mplayer::shell::MediaInformationDialog", "%1 bytes")
        .arg(locale.toString(static_cast<qlonglong>(*size)));
    if (*size < 1024) return exact;
    return QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "%1 (%2)")
        .arg(locale.formattedDataSize(*size, 2, QLocale::DataSizeIecFormat), exact);
}

QString bitrateText(const std::optional<double>& bitrate, const QLocale& locale) {
    if (!bitrate || !std::isfinite(*bitrate) || *bitrate < 0) return availableText({});
    const QString exact = QCoreApplication::translate(
        "mplayer::shell::MediaInformationDialog", "%1 bit/s")
        .arg(locale.toString(*bitrate, 'g', QLocale::FloatingPointShortest));
    QString human;
    if (*bitrate >= 1000000000.0)
        human = QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "%1 Gbit/s")
            .arg(locale.toString(*bitrate / 1000000000.0, 'f', 2));
    else if (*bitrate >= 1000000.0)
        human = QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "%1 Mbit/s")
            .arg(locale.toString(*bitrate / 1000000.0, 'f', 2));
    else if (*bitrate >= 1000.0)
        human = QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "%1 kbit/s")
            .arg(locale.toString(*bitrate / 1000.0, 'f', 2));
    else
        return exact;
    return QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "%1 (%2)")
        .arg(human, exact);
}

MediaInformationNode trackNode(const MediaTrack& track, const QString& prefix,
                               const QLocale& locale) {
    const QString id = prefix + QLatin1Char('.') + QString::number(track.id);
    QStringList summary;
    if (!track.title.isEmpty()) summary.append(track.title);
    if (!track.language.isEmpty()) summary.append(track.language);
    if (!track.codec.isEmpty()) summary.append(track.codec);
    summary.append(QCoreApplication::translate(
        "mplayer::shell::MediaInformationDialog", "Selected: %1")
        .arg(flagText(track.selected, track.selectionKnown)));
    MediaInformationNode node{
        id,
        QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Track %1")
            .arg(locale.toString(static_cast<qlonglong>(track.id))),
        summary.join(QStringLiteral(" · ")), {}};
    const auto add = [&node, &id](const char* suffix, const QString& label, const QString& value) {
        node.children.append({id + QLatin1Char('.') + QLatin1String(suffix), label, value, {}});
    };
    add("id", QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "ID"),
        locale.toString(static_cast<qlonglong>(track.id)));
    add("title", QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Title"),
        availableText(track.title));
    add("language", QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Language"),
        availableText(track.language));
    add("codec", QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Codec"),
        availableText(track.codec));
    if (track.type == MediaTrackType::Audio)
        add("channels", QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Channels"),
            track.audioChannels > 0 ? locale.toString(track.audioChannels) : availableText({}));
    add("default", QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Default"),
        flagText(track.isDefault, track.defaultKnown));
    add("forced", QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Forced"),
        flagText(track.forced, track.forcedKnown));
    add("external", QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "External"),
        flagText(track.external, track.externalKnown));
    add("external-file", QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "External file"),
        availableText(track.externalFilename));
    add("selected", QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Selected"),
        flagText(track.selected, track.selectionKnown));
    return node;
}

void appendPlainText(QString& output, const MediaInformationNode& node, int depth) {
    const QString indentation(depth * 2, QLatin1Char(' '));
    output += indentation + node.label;
    if (!node.value.isEmpty()) {
        output += QStringLiteral(": ");
        // Indent literal continuation lines to keep them attached to their property.
        QString value = node.value;
        value.replace(QStringLiteral("\n"), QStringLiteral("\n") + indentation + QStringLiteral("  "));
        output += value;
    }
    output += QLatin1Char('\n');
    for (const MediaInformationNode& child : node.children)
        appendPlainText(output, child, depth + 1);
}

}

// Out-of-line equality avoids recursive Qt container trait instantiation.
bool operator==(const MediaInformationNode& left, const MediaInformationNode& right) {
    return left.id == right.id && left.label == right.label && left.value == right.value &&
           left.children == right.children;
}

MediaInformationDocument buildMediaInformationDocument(
    const MediaInformation& information, const MediaTrackSnapshot& tracks,
    bool timedOut, const QString& trackError, const QString& playbackError) {
    const QLocale locale;
    MediaInformationDocument document;
    document.reserve(6);
    MediaInformationNode status{QStringLiteral("status"),
        QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Status"), {}, {}};
    if (information.loading)
        status.children.append({QStringLiteral("status.loading"),
            QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Status"),
            QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Reading media information…"), {}});
    if (timedOut)
        status.children.append({QStringLiteral("status.timeout"),
            QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Status"),
            QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "The playback core did not return all media information in time."), {}});
    if (!status.children.isEmpty()) document.append(std::move(status));

    MediaInformationNode file{QStringLiteral("file"),
        QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "File"), {}, {}};
    file.children = {
        {QStringLiteral("file.filename"), QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Filename"), availableText(information.filename), {}},
        {QStringLiteral("file.path"), QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Path"), availableText(information.path), {}},
        {QStringLiteral("file.container"), QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Container"), availableText(information.container), {}},
        {QStringLiteral("file.duration"), QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Duration"), durationText(information.duration, locale), {}},
        {QStringLiteral("file.size"), QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Size"), sizeText(information.size, locale), {}}
    };
    document.append(std::move(file));

    QString dimensions = availableText({});
    const auto dimensionText = [&locale](const std::optional<int64_t>& dimension) {
        return dimension && *dimension > 0
            ? locale.toString(static_cast<qlonglong>(*dimension)) : availableText({});
    };
    if ((information.width && *information.width > 0) || (information.height && *information.height > 0))
        dimensions = QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "%1 × %2")
            .arg(dimensionText(information.width), dimensionText(information.height));
    QString frameRate = availableText({});
    if (information.framesPerSecond && std::isfinite(*information.framesPerSecond) && *information.framesPerSecond >= 0)
        frameRate = QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "%1 fps")
            .arg(locale.toString(*information.framesPerSecond, 'f', 3));
    MediaInformationNode video{QStringLiteral("video"),
        QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Video"), {}, {}};
    video.children = {
        {QStringLiteral("video.codec"), QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Codec"), availableText(information.videoCodec), {}},
        {QStringLiteral("video.dimensions"), QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Dimensions"), dimensions, {}},
        {QStringLiteral("video.frame-rate"), QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Frame rate"), frameRate, {}},
        {QStringLiteral("video.video-bitrate"), QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Video bitrate"), bitrateText(information.videoBitrate, locale), {}},
        {QStringLiteral("video.audio-bitrate"), QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Audio bitrate"), bitrateText(information.audioBitrate, locale), {}}
    };
    document.append(std::move(video));

    for (const MediaTrackType type : {MediaTrackType::Audio, MediaTrackType::Subtitle}) {
        const QString prefix = type == MediaTrackType::Audio ? QStringLiteral("audio") : QStringLiteral("subtitles");
        MediaInformationNode group{prefix,
            type == MediaTrackType::Audio
                ? QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Audio tracks")
                : QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Subtitle tracks"), {}, {}};
        if (tracks.ready) {
            for (const MediaTrack& track : tracks.tracks)
                if (track.type == type) group.children.append(trackNode(track, prefix, locale));
            group.value = group.children.isEmpty()
                ? QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "None")
                : QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Tracks: %1")
                    .arg(locale.toString(static_cast<qlonglong>(group.children.size())));
        } else {
            group.value = availableText({});
        }
        document.append(std::move(group));
    }

    MediaInformationNode errors{QStringLiteral("errors"),
        QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Unavailable properties"), {}, {}};
    if (!playbackError.isEmpty())
        errors.children.append({QStringLiteral("errors.playback"),
            QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Playback error"), playbackError, {}});
    if (!trackError.isEmpty())
        errors.children.append({QStringLiteral("errors.track"),
            QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Track information error"), trackError, {}});
    for (qsizetype index = 0; index < information.errors.size(); ++index)
        errors.children.append({QStringLiteral("errors.property.%1").arg(index),
            QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Unavailable properties"),
            availableText(information.errors.at(index)), {}});
    if (!errors.children.isEmpty()) {
        if (!playbackError.isEmpty() || !trackError.isEmpty())
            errors.label = QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Errors");
        document.append(std::move(errors));
    }
    return document;
}

QString mediaInformationPlainText(const MediaInformationDocument& document) {
    QString output;
    for (const MediaInformationNode& node : document) {
        if (!output.isEmpty()) output += QLatin1Char('\n');
        appendPlainText(output, node, 0);
    }
    return output;
}

}
