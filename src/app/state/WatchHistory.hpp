#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

namespace mikai::app {

inline constexpr char kWatchHistoryKey[] = "title/watchHistory";

struct TitleWatchProgress {
    QString current;
    QStringList watched;
    /// Playback offset in seconds per episode, so a half-watched episode can be
    /// resumed instead of restarted. Only meaningful for episodes that are not
    /// watched yet: finishing one clears its offset.
    QMap<QString, double> offsets;
};

class WatchHistory {
public:
    explicit WatchHistory(const QString& settingsKey = {});

    TitleWatchProgress progress(const QString& ref) const;
    QString current(const QString& ref) const;
    bool isWatched(const QString& ref, const QString& episode) const;
    void setCurrent(const QString& ref, const QString& episode);
    void setWatched(const QString& ref, const QString& episode, bool watched);
    void setEpisodesWatched(const QString& ref, const QStringList& episodes, bool watched);

    /// Offset in seconds to resume this episode from, or 0 when it should start
    /// at the beginning (never started, already watched, or abandoned at the
    /// very start).
    double offset(const QString& ref, const QString& episode) const;
    /// Records how far an episode has been watched. `duration` may be 0 while a
    /// live stream has no known length; then only the offset is stored.
    void setOffset(const QString& ref, const QString& episode, double position,
                   double duration);
    /// Clear this store and persist the empty history.
    void clear();

    /// Wait until queued history writes are committed. Safe to call again if
    /// the application continues mutating history afterward.
    static void flushHistoryWrites();

private:
    void load();
    void save();

    QString key_;
    QMap<QString, TitleWatchProgress> titles_;
};

}  // namespace mikai::app
