#include "state/QualityCache.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSettings>

#include <algorithm>

namespace mikai::app {

const qint64 kQualityCacheTtlMs = 1800000;
const int kQualityCacheMaxEntries = 240;
const qint64 kQualityCacheFlushIntervalMs = 5000;

namespace {

QString defaultKey() { return QStringLiteral("quality/urls"); }

}  // namespace

QualityCache::QualityCache(QObject* parent) : QualityCache(defaultKey(), parent) {}

QualityCache::QualityCache(const QString& settingsKey, QObject* parent)
    : QObject(parent), key_(settingsKey) {}

QualityCache::~QualityCache() = default;

void QualityCache::setSettingsKey(const QString& key) { key_ = key; }

void QualityCache::prune() {
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - kQualityCacheTtlMs;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->fetchedAt < cutoff || it->qualities.isEmpty()) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

void QualityCache::load() {
    entries_.clear();
    // What is in memory now is what is on disk, so there is nothing to write
    // until something changes.
    dirty_ = false;
    lastFlushAtMs_ = 0;
    QSettings settings;
    const QString raw = settings.value(key_).toString();
    if (raw.isEmpty()) {
        return;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return;
    }
    const QJsonObject entries = document.object().value(QStringLiteral("entries")).toObject();
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - kQualityCacheTtlMs;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        const QJsonObject record = it.value().toObject();
        const qint64 fetchedAt =
            static_cast<qint64>(record.value(QStringLiteral("at")).toDouble() * 1000.0);
        if (fetchedAt < cutoff) {
            continue;
        }
        const QJsonObject heights = record.value(QStringLiteral("heights")).toObject();
        QMap<int, QString> map;
        for (auto h = heights.constBegin(); h != heights.constEnd(); ++h) {
            const QString url = h.value().toString();
            if (!url.isEmpty()) {
                map.insert(h.key().toInt(), url);
            }
        }
        if (!map.isEmpty()) {
            entries_.insert(it.key(), Entry{fetchedAt, map});
        }
    }
}

void QualityCache::flush() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastFlushAtMs_ != 0 && now - lastFlushAtMs_ < kQualityCacheFlushIntervalMs) {
        return;
    }
    flushNow();
}

void QualityCache::flushNow() {
    if (!dirty_) {
        // Memory already matches the file, so writing it again would produce a
        // byte-identical blob. Skipping here is what lets the teardown paths
        // call this unconditionally.
        return;
    }
    prune();
    QJsonObject entries;
    for (auto it = entries_.constBegin(); it != entries_.constEnd(); ++it) {
        QJsonObject heights;
        for (auto h = it->qualities.constBegin(); h != it->qualities.constEnd(); ++h) {
            heights.insert(QString::number(h.key()), h.value());
        }
        QJsonObject record;
        record.insert(QStringLiteral("at"), it->fetchedAt / 1000.0);
        record.insert(QStringLiteral("heights"), heights);
        entries.insert(it.key(), record);
    }
    QJsonObject root;
    root.insert(QStringLiteral("entries"), entries);
    QSettings settings;
    settings.setValue(key_, QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    dirty_ = false;
    lastFlushAtMs_ = QDateTime::currentMSecsSinceEpoch();
}

void QualityCache::clear() {
    entries_.clear();
    noSource_.clear();
    QSettings settings;
    settings.remove(key_);
    // The key is gone from disk, so there is nothing left for a flush to write.
    dirty_ = false;
}

QMap<int, QString> QualityCache::qualities(const QString& embed) const {
    const auto it = entries_.constFind(embed);
    if (it == entries_.constEnd()) {
        return {};
    }
    if (it->fetchedAt < QDateTime::currentMSecsSinceEpoch() - kQualityCacheTtlMs) {
        return {};
    }
    return it->qualities;
}

void QualityCache::remember(const QString& embed, const QMap<int, QString>& map,
                            qint64 nowMsecsSinceEpoch) {
    if (embed.isEmpty() || map.isEmpty()) {
        return;
    }
    const qint64 stamp =
        nowMsecsSinceEpoch != 0 ? nowMsecsSinceEpoch : QDateTime::currentMSecsSinceEpoch();
    entries_.insert(embed, Entry{stamp, map});
    dirty_ = true;
    // Evict the oldest until the cap holds: a signed URL that is nearly expired
    // is worth less than a fresh one, so recency is the right order to drop in.
    while (entries_.size() > kQualityCacheMaxEntries) {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->fetchedAt < oldest->fetchedAt) {
                oldest = it;
            }
        }
        entries_.erase(oldest);
    }
}

void QualityCache::forget(const QString& embed) {
    entries_.remove(embed);
    // The no-source mark goes too: the provider may well serve the episode now,
    // and a page-lifetime veto is not the place to keep a recovered one.
    noSource_.remove(embed);
    dirty_ = true;
}

bool QualityCache::isNoSource(const QString& embed) const { return noSource_.contains(embed); }

void QualityCache::markNoSource(const QString& embed) { noSource_.insert(embed); }

int QualityCache::size() const { return static_cast<int>(entries_.size()); }

}  // namespace mikai::app
