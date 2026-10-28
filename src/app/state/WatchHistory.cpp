#include "state/WatchHistory.hpp"

#include <QHash>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSettings>
#include <QSet>

#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace mikai::app {
namespace {

QString defaultKey() { return QString::fromLatin1(kWatchHistoryKey); }

class HistoryWriter {
public:
    static HistoryWriter& instance() {
        static HistoryWriter writer;
        return writer;
    }

    QString snapshot(const QString& key) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto cached = latest_.constFind(key);
        if (cached != latest_.constEnd()) {
            return cached.value();
        }

        QSettings settings;
        const QString payload = settings.value(key).toString();
        latest_.insert(key, payload);
        return payload;
    }

    void submit(QString key, QString payload) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            latest_.insert(key, payload);
            for (Item& item : pending_) {
                if (item.key == key) {
                    item.payload = std::move(payload);
                    return;
                }
            }
            pending_.append(Item{std::move(key), std::move(payload)});
        }
        condition_.notify_one();
    }

    void flush() {
        std::unique_lock<std::mutex> lock(mutex_);
        drained_.wait(lock, [this] { return pending_.isEmpty() && !writing_; });
        if (!writeFailed_) {
            latest_.clear();
        }
    }

    ~HistoryWriter() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    struct Item {
        QString key;
        QString payload;
    };

    HistoryWriter()
        : thread_([this] {
              for (;;) {
                  Item item;
                  {
                      std::unique_lock<std::mutex> lock(mutex_);
                      condition_.wait(lock, [this] { return stopping_ || !pending_.isEmpty(); });
                      if (pending_.isEmpty()) {
                          if (stopping_) {
                              return;
                          }
                          continue;
                      }
                      item = pending_.takeFirst();
                      writing_ = true;
                  }

                  QSettings settings;
                  if (item.payload.isEmpty()) {
                      settings.remove(item.key);
                  } else {
                      settings.setValue(item.key, item.payload);
                  }
                  settings.sync();
                  const bool saved = settings.status() == QSettings::NoError;
                  if (!saved) {
                      qWarning() << "mikai: could not save episode history, settings status:"
                                 << settings.status();
                  }

                  {
                      const std::lock_guard<std::mutex> lock(mutex_);
                      writeFailed_ = writeFailed_ || !saved;
                      writing_ = false;
                      if (pending_.isEmpty()) {
                          drained_.notify_all();
                      }
                  }
              }
          }) {}

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable drained_;
    QHash<QString, QString> latest_;
    QList<Item> pending_;
    bool writing_ = false;
    bool stopping_ = false;
    bool writeFailed_ = false;
};

}  // namespace

void WatchHistory::flushHistoryWrites() {
    HistoryWriter::instance().flush();
}

WatchHistory::WatchHistory(const QString& settingsKey)
    : key_(settingsKey.isEmpty() ? defaultKey() : settingsKey) {
    load();
}

void WatchHistory::load() {
    titles_.clear();
    const QByteArray raw = HistoryWriter::instance().snapshot(key_).toUtf8();
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
        const QJsonObject offsets = record.value(QStringLiteral("offsets")).toObject();
        for (auto offset = offsets.constBegin(); offset != offsets.constEnd(); ++offset) {
            const double seconds = offset.value().toDouble(-1.0);
            if (!offset.key().isEmpty() && seconds > 0.0) {
                progress.offsets.insert(offset.key(), seconds);
            }
        }
        if (progress.current.isEmpty() && progress.watched.isEmpty() && progress.offsets.isEmpty()) {
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
        if (!it->offsets.isEmpty()) {
            QJsonObject offsets;
            for (auto offset = it->offsets.constBegin(); offset != it->offsets.constEnd();
                 ++offset) {
                offsets.insert(offset.key(), offset.value());
            }
            record.insert(QStringLiteral("offsets"), offsets);
        }
        if (record.isEmpty()) {
            continue;
        }
        root.insert(it.key(), record);
    }
    HistoryWriter::instance().submit(
        key_, root.isEmpty()
                 ? QString()
                 : QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
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
        // A finished episode restarts from the beginning if it is replayed.
        it->offsets.remove(episode);
    } else {
        it->watched.removeAll(episode);
    }
    if (it->current.isEmpty() && it->watched.isEmpty() && it->offsets.isEmpty()) {
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

void WatchHistory::clear() {
    titles_.clear();
    save();
}


double WatchHistory::offset(const QString& ref, const QString& episode) const {
    if (ref.isEmpty() || episode.isEmpty()) {
        return 0.0;
    }
    const auto it = titles_.constFind(ref);
    if (it == titles_.constEnd()) {
        return 0.0;
    }
    // A watched episode is not resumed, even if an offset survived an older
    // build that did not clear it.
    if (it->watched.contains(episode)) {
        return 0.0;
    }
    return it->offsets.value(episode, 0.0);
}

void WatchHistory::setOffset(const QString& ref, const QString& episode, double position,
                             double duration) {
    if (ref.isEmpty() || episode.isEmpty()) {
        return;
    }
    if (!std::isfinite(position) || !std::isfinite(duration) || position < 0.0) {
        return;
    }
    // Under half a minute there is nothing worth resuming; the beginning is
    // where the episode would start anyway.
    constexpr double kResumeFloorSeconds = 30.0;
    if (position < kResumeFloorSeconds) {
        return;
    }
    // Finishing a stream without an end-of-file event (a live HLS edge, a
    // dropped connection at the credits) has to count as watched too, which is
    // why the threshold lives here and not only in the ended handler.
    if (duration > 0.0 && position >= duration * 0.9) {
        setWatched(ref, episode, true);
        return;
    }
    load();
    auto it = titles_.find(ref);
    if (it != titles_.end() && it->watched.contains(episode)) {
        return;
    }
    if (it == titles_.end()) {
        it = titles_.insert(ref, TitleWatchProgress{});
    }
    if (qFuzzyCompare(it->offsets.value(episode, -1.0), position)) {
        return;
    }
    it->offsets.insert(episode, position);
    save();
}

}  // namespace mikai::app
