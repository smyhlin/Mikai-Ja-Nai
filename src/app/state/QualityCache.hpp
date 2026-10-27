#pragma once

#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>

namespace mikai::app {

/// Stream URLs are signed and expire, so a cached entry lives for this long
/// only; the title page re-extracts a stream that fails to play, and an entry
/// that aged out simply extracts again.
extern const qint64 kQualityCacheTtlMs;
/// Cap, so a long browsing session cannot grow the settings blob without end.
extern const int kQualityCacheMaxEntries;

/// Cross-session store for extracted HLS quality URLs, keyed by embed URL.
///
/// A provider with no video for an episode is a page-lifetime negative result,
/// not a cache entry: it is remembered in memory only, so a provider that gains
/// the episode later is not blocked for the rest of the session by a file.
class QualityCache : public QObject {
    Q_OBJECT

public:
    explicit QualityCache(QObject* parent = nullptr);
    explicit QualityCache(const QString& settingsKey, QObject* parent = nullptr);
    ~QualityCache() override;

    /// Read the persisted entries, dropping anything past the TTL.
    void load();
    /// Persist changed entries after dropping expired URLs.
    void flushNow();
    /// Drop everything, in memory and on disk.
    void clear();

    /// The cached heights for `embed`, empty when absent or expired.
    QMap<int, QString> qualities(const QString& embed) const;

    /// Remember `map` for `embed`. A `nowMsecsSinceEpoch` of 0 means "now";
    /// an explicit stamp lets a test prove the TTL without waiting for it.
    void remember(const QString& embed, const QMap<int, QString>& map,
                  qint64 nowMsecsSinceEpoch = 0);

    bool isNoSource(const QString& embed) const;
    void markNoSource(const QString& embed);

    /// Drop one embed's entry, in memory and on disk. Used when a stream failed
    /// to play: leaving the dead signed URL in the store would replay it on the
    /// next click instead of extracting again.
    void forget(const QString& embed);

    /// Live entry count, expired ones included until the next prune.
    int size() const;

    void setSettingsKey(const QString& key);
    QString settingsKey() const { return key_; }

private:
    void prune();
    bool trimToLimit();

    struct Entry {
        qint64 fetchedAt = 0;
        QMap<int, QString> qualities;
    };

    QString key_;
    QMap<QString, Entry> entries_;
    QSet<QString> noSource_;
    // Remains set if QSettings reports a failed write, allowing a later retry.
    bool dirty_ = false;
};

}  // namespace mikai::app
