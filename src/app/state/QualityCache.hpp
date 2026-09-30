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
/// Shortest gap between two writes of the settings blob. Long enough to
/// collapse a burst of title opens into one write, short enough that a session
/// which ends abruptly loses almost nothing.
extern const qint64 kQualityCacheFlushIntervalMs;

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
    /// Write the live entries back, dropping anything past the TTL first, but
    /// only when something changed and the previous write is at least
    /// `kQualityCacheFlushIntervalMs` old. Browsing re-extracts constantly, and
    /// rewriting the whole blob on every title open is a registry write of a
    /// few hundred kilobytes each time for entries that are all still valid.
    /// A session that ends without reaching the interval still persists through
    /// flushNow(); a hard kill costs only the entries from the last interval,
    /// which simply extract again.
    void flush();
    /// Write the live entries back regardless of the debounce. This is the
    /// teardown path, where leaving entries unwritten would lose them.
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

    struct Entry {
        qint64 fetchedAt = 0;
        QMap<int, QString> qualities;
    };

    QString key_;
    QMap<QString, Entry> entries_;
    QSet<QString> noSource_;
    // Set by anything that changes what a write would produce, so a flush with
    // nothing to say costs nothing.
    bool dirty_ = false;
    qint64 lastFlushAtMs_ = 0;
};

}  // namespace mikai::app
