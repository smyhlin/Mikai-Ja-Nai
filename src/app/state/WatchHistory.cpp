#include "state/WatchHistory.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QThread>
#include <QTimer>

#include <cmath>
#include <mutex>

namespace mikai::app {
namespace {

QString defaultKey() { return QString::fromUtf8(kWatchHistoryKey); }

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
        bool shouldWriteImmediately = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            latest_.insert(key, payload);
            bool updated = false;
            for (Item& item : pending_) {
                if (item.key == key) {
                    item.payload = std::move(payload);
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                pending_.append(Item{std::move(key), std::move(payload)});
            }
            QCoreApplication* app = QCoreApplication::instance();
            if (app != nullptr && QThread::currentThread() == app->thread()) {
                ensureTimerLocked(app);
                if (flushTimer_ != nullptr && !flushTimer_->isActive()) {
                    flushTimer_->start();
                }
            } else {
                shouldWriteImmediately = true;
            }
        }
        if (shouldWriteImmediately) {
            flush();
        }
    }

    void flush() {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (flushTimer_ != nullptr) {
            flushTimer_->stop();
        }
        if (pending_.isEmpty()) {
            if (!writeFailed_) {
                latest_.clear();
            }
            return;
        }
        if (QCoreApplication::instance() == nullptr) {
            return;
        }
        QList<Item> items;
        items.swap(pending_);

        QSettings settings;
        for (const Item& item : items) {
            if (item.payload.isEmpty()) {
                settings.remove(item.key);
            } else {
                settings.setValue(item.key, item.payload);
            }
        }
        settings.sync();
        const bool saved = settings.status() == QSettings::NoError;
        if (!saved) {
            qWarning() << "mikai: could not save episode history, settings status:"
                       << settings.status();
            writeFailed_ = true;
        } else {
            writeFailed_ = false;
            latest_.clear();
        }
    }

    ~HistoryWriter() = default;

private:
    struct Item {
        QString key;
        QString payload;
    };

    void ensureTimerLocked(QCoreApplication* app) {
        if (flushTimer_ != nullptr) return;
        auto* timer = new QTimer(app);
        timer->setSingleShot(true);
        timer->setInterval(500);
        QObject::connect(timer, &QTimer::timeout, app, [this] { flush(); });
        QObject::connect(app, &QCoreApplication::aboutToQuit, app, [this] { flush(); });
        flushTimer_ = timer;
    }

    std::mutex mutex_;
    QPointer<QTimer> flushTimer_;
    QHash<QString, QString> latest_;
    QList<Item> pending_;
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
    const std::lock_guard<std::mutex> lock(titlesMutex_);
    loadLocked();
}

void WatchHistory::loadLocked() {
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
        const QJsonObject durations = record.value(QStringLiteral("durations")).toObject();
        for (auto length = durations.constBegin(); length != durations.constEnd(); ++length) {
            const double seconds = length.value().toDouble(-1.0);
            if (!length.key().isEmpty() && seconds > 0.0) {
                progress.durations.insert(length.key(), seconds);
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
    const std::lock_guard<std::mutex> lock(titlesMutex_);
    saveLocked();
}

void WatchHistory::saveLocked() {
    QJsonObject root;
    for (auto it = titles_.constBegin(); it != titles_.constEnd(); ++it) {
        QJsonObject record;
        if (!it->current.isEmpty()) {
            record.insert(QStringLiteral("current"), it->current);
        }
        if (!it->watched.isEmpty()) {
            record.insert(QStringLiteral("watched"), QJsonArray::fromStringList(it->watched));
        }
        if (!it->durations.isEmpty()) {
            QJsonObject durations;
            for (auto length = it->durations.constBegin(); length != it->durations.constEnd();
                 ++length) {
                durations.insert(length.key(), length.value());
            }
            record.insert(QStringLiteral("durations"), durations);
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
    const std::lock_guard<std::mutex> lock(titlesMutex_);
    // Built under the lock, returned after it is released.
    const TitleWatchProgress copy = titles_.value(ref);
    return copy;
}

QString WatchHistory::current(const QString& ref) const {
    const std::lock_guard<std::mutex> lock(titlesMutex_);
    const auto it = titles_.constFind(ref);
    return it != titles_.constEnd() ? it->current : QString();
}

bool WatchHistory::isWatched(const QString& ref, const QString& episode) const {
    if (episode.isEmpty()) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(titlesMutex_);
    const auto it = titles_.constFind(ref);
    return it != titles_.constEnd() && it->watched.contains(episode);
}

void WatchHistory::setCurrent(const QString& ref, const QString& episode) {
    if (ref.isEmpty() || episode.isEmpty()) {
        return;
    }
    // One critical section: load, mutate and save under the same lock, so a
    // second writer cannot slip in between and lose this write.
    const std::lock_guard<std::mutex> lock(titlesMutex_);
    // Several title pages may outlive each other; merge the latest records before writing.
    loadLocked();
    auto it = titles_.find(ref);
    if (it != titles_.end() && it->current == episode) {
        return;
    }
    if (it == titles_.end()) {
        it = titles_.insert(ref, TitleWatchProgress{});
    }
    it->current = episode;
    saveLocked();
}

void WatchHistory::setWatched(const QString& ref, const QString& episode, bool watched) {
    if (ref.isEmpty() || episode.isEmpty()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(titlesMutex_);
    loadLocked();
    if (setWatchedLocked(ref, episode, watched)) {
        saveLocked();
    }
}

bool WatchHistory::setWatchedLocked(const QString& ref, const QString& episode, bool watched) {
    auto it = titles_.find(ref);
    const bool known = it != titles_.end() && it->watched.contains(episode);
    if (watched == known) {
        return false;
    }
    if (it == titles_.end()) {
        it = titles_.insert(ref, TitleWatchProgress{});
    }
    if (watched) {
        it->watched.append(episode);
        // A finished episode restarts from the beginning if it is replayed.
        it->offsets.remove(episode);
        it->durations.remove(episode);
    } else {
        it->watched.removeAll(episode);
    }
    if (it->current.isEmpty() && it->watched.isEmpty() && it->offsets.isEmpty() &&
        it->durations.isEmpty()) {
        titles_.erase(it);
    }
    return true;
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

    const std::lock_guard<std::mutex> lock(titlesMutex_);
    loadLocked();
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
    saveLocked();
}

void WatchHistory::clear() {
    const std::lock_guard<std::mutex> lock(titlesMutex_);
    titles_.clear();
    saveLocked();
}


double WatchHistory::offset(const QString& ref, const QString& episode) const {
    if (ref.isEmpty() || episode.isEmpty()) {
        return 0.0;
    }
    const std::lock_guard<std::mutex> lock(titlesMutex_);
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

double WatchHistory::duration(const QString& ref, const QString& episode) const {
    if (ref.isEmpty() || episode.isEmpty()) {
        return 0.0;
    }
    const std::lock_guard<std::mutex> lock(titlesMutex_);
    const auto it = titles_.constFind(ref);
    if (it == titles_.constEnd() || it->watched.contains(episode)) {
        return 0.0;
    }
    return it->durations.value(episode, 0.0);
}

void WatchHistory::clearOffset(const QString& ref, const QString& episode) {
    if (ref.isEmpty() || episode.isEmpty()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(titlesMutex_);
    loadLocked();
    auto it = titles_.find(ref);
    if (it == titles_.end()) {
        return;
    }
    const bool hadOffset = it->offsets.remove(episode) > 0;
    it->durations.remove(episode);
    if (hadOffset) {
        saveLocked();
    }
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
    const std::lock_guard<std::mutex> lock(titlesMutex_);
    if (duration > 0.0 && position >= duration * 0.9) {
        loadLocked();
        if (setWatchedLocked(ref, episode, true)) {
            saveLocked();
        }
        return;
    }
    loadLocked();
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
    if (duration > 0.0) {
        it->durations.insert(episode, duration);
    }
    saveLocked();
}

}  // namespace mikai::app
