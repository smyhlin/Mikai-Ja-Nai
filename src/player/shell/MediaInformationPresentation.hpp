#pragma once

#include "../core/MediaInformation.hpp"
#include "../core/MediaTracks.hpp"

#include <QString>
#include <QVector>

namespace mplayer::shell {

struct MediaInformationNode {
    QString id;
    QString label;
    QString value;
    QVector<MediaInformationNode> children;
    friend bool operator==(const MediaInformationNode&, const MediaInformationNode&);
};

using MediaInformationDocument = QVector<MediaInformationNode>;

MediaInformationDocument buildMediaInformationDocument(
    const MediaInformation& information, const MediaTrackSnapshot& tracks,
    bool timedOut, const QString& trackError, const QString& playbackError);
QString mediaInformationPlainText(const MediaInformationDocument& document);

}
