#include "state/WatchHistory.hpp"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSettings>
#include <QSet>

namespace mikai::app {
namespace {

QString defaultKey() { return QString::fromLatin1(kWatchHistoryKey); }

}  // namespace

WatchHistory::WatchHistory(const QString& settingsKey)
    : key_(settingsKey.isEmpty() ? defaultKey() : settingsKey) {
    load();
}

void WatchHistory::load() {
    titles_.clear();
    QSettings settings;
    const QByteArray raw = settings.value(key_).toString().toUtf8();
    if (raw.isEmpty()) {
        return;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning().noquote() << "mikai: could not read episode history:"
                             << (error.error != QJsonParseError::NoError
                                     ? error.errorString()
                                     : QStringLiteral("expected an object"));
        return;
    }
    const QJsonObject root = document.object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        if (!it.value().isObject()) {
            continue;
        }
        const QJsonObject record = it.value().toObject();
        TitleWatchProgress progress;
        progress.current = record.value(QStringLiteral("current")).toString();
        const QJsonArray watched = record.value(QStringLiteral("watched")).toArray();
        for (const QJsonValue& value : watched) {
            const QString episode = value.toString();
            if (!episode.isEmpty() && !progress.watched.contains(episode)) {
                progress.watched.append(episode);
            }
        }
        if (progress.current.isEmpty() && progress.watched.isEmpty()) {
            continue;
        }
        titles_.insert(it.key(), progress);
    }
}

void WatchHistory::save() {
    QJsonObject root;
    for (auto it = titles_.constBegin(); it != titles_.constEnd(); ++it) {
        QJsonObject record;
        if (!it->current.isEmpty()) {
            record.insert(QStringLiteral("current"), it->current);
        }
        if (!it->watched.isEmpty()) {
            record.insert(QStringLiteral("watched"), QJsonArray::fromStringList(it->watched));
        }
        if (record.isEmpty()) {
            continue;
        }
        root.insert(it.key(), record);
    }
    QSettings settings;
    if (root.isEmpty()) {
        settings.remove(key_);
    } else {
        settings.setValue(key_,
                          QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        qWarning() << "mikai: could not save episode history, settings status:"
                   << settings.status();
    }
}

TitleWatchProgress WatchHistory::progress(const QString& ref) const {
    return titles_.value(ref);
}

QString WatchHistory::current(const QString& ref) const {
    const auto it = titles_.constFind(ref);
    return it != titles_.constEnd() ? it->current : QString();
}

bool WatchHistory::isWatched(const QString& ref, const QString& episode) const {
    if (episode.isEmpty()) {
        return false;
    }
    const auto it = titles_.constFind(ref);
    return it != titles_.constEnd() && it->watched.contains(episode);
}

void WatchHistory::setCurrent(const QString& ref, const QString& episode) {
    if (ref.isEmpty() || episode.isEmpty()) {
        return;
    }
    // Several title pages may outlive each other; merge the latest records before writing.
    load();
    auto it = titles_.find(ref);
    if (it != titles_.end() && it->current == episode) {
        return;
    }
    if (it == titles_.end()) {
        it = titles_.insert(ref, TitleWatchProgress{});
    }
    it->current = episode;
    save();
}

void WatchHistory::setWatched(const QString& ref, const QString& episode, bool watched) {
    if (ref.isEmpty() || episode.isEmpty()) {
        return;
    }
    load();
    auto it = titles_.find(ref);
    const bool known = it != titles_.end() && it->watched.contains(episode);
    if (watched == known) {
        return;
    }
    if (it == titles_.end()) {
        it = titles_.insert(ref, TitleWatchProgress{});
    }
    if (watched) {
        it->watched.append(episode);
    } else {
        it->watched.removeAll(episode);
    }
    if (it->current.isEmpty() && it->watched.isEmpty()) {
        titles_.erase(it);
    }
    save();
}

void WatchHistory::setEpisodesWatched(const QString& ref, const QStringList& episodes,
                                      bool watched) {
    if (ref.isEmpty() || episodes.isEmpty()) {
        return;
    }
    QSet<QString> requested;
    requested.reserve(episodes.size());
    for (const QString& episode : episodes) {
        if (!episode.isEmpty()) {
            requested.insert(episode);
        }
    }
    if (requested.isEmpty()) {
        return;
    }

    load();
    auto it = titles_.find(ref);
    if (watched) {
        if (it != titles_.end()) {
            for (const QString& episode : it->watched) {
                requested.remove(episode);
            }
        }
        if (requested.isEmpty()) {
            return;
        }
        if (it == titles_.end()) {
            it = titles_.insert(ref, TitleWatchProgress{});
        }
        it->watched.reserve(it->watched.size() + requested.size());
        for (const QString& episode : episodes) {
            if (requested.remove(episode)) {
                it->watched.append(episode);
            }
        }
    } else {
        if (it == titles_.end()) {
            return;
        }
        const qsizetype removed = it->watched.removeIf(
            [&requested](const QString& episode) { return requested.contains(episode); });
        if (removed == 0) {
            return;
        }
        if (it->current.isEmpty() && it->watched.isEmpty()) {
            titles_.erase(it);
        }
    }
    save();
}

}  // namespace mikai::app
