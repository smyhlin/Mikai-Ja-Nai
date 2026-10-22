#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

namespace mikai::app {

inline constexpr char kWatchHistoryKey[] = "title/watchHistory";

struct TitleWatchProgress {
    QString current;
    QStringList watched;
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

private:
    void load();
    void save();

    QString key_;
    QMap<QString, TitleWatchProgress> titles_;
};

}  // namespace mikai::app
