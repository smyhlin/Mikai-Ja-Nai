// OpenSubtitles.com REST v1, the two calls a subtitle picker needs.
//
// The service requires an API key on every request; this module never stores
// one, it takes the caller's. Both calls block the calling thread and are meant
// to run on a worker.

#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace mplayer {
namespace shell {

struct OpenSubtitlesHit {
    int fileId = 0;
    QString language;
    QString releaseName;
    QString hearingImpaired;
    int downloads = 0;
};

struct OpenSubtitlesSearch {
    QVector<OpenSubtitlesHit> hits;
    QString error;
};

/// GET /subtitles with a free-text query and a language code ("uk", "en", …).
OpenSubtitlesSearch searchOpenSubtitles(const QString& apiKey, const QString& query,
                                        const QString& languageCode, int timeoutMs = 20000);

/// Fetches the subtitle file for a hit: POST /download for a link, then the
/// bytes behind it. The format is whatever the publisher uploaded (usually SRT).
QByteArray downloadOpenSubtitles(const QString& apiKey, int fileId, QString* error,
                                 int timeoutMs = 60000);

}  // namespace shell
}  // namespace mplayer
