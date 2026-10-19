// Filesystem observation for the AnimeJaNai model catalogue.
//
// The watcher publishes only "something the catalogue can see changed"; it
// never imports, registers, hashes or inspects a graph. Consumers re-enumerate
// the existing catalogue APIs on their own thread. Observation is metadata
// only (existence, byte count, modification time, metadata-change time and
// directory entry names).
//
// Coverage is complete over what the public inventory can list: every relevant
// file a catalogue directory holds (ONNX graphs, plus owned sidecars) and every
// registered pin path inside the registry JSON, however many there are. Nothing
// relevant is truncated by entry count or model count, so a paste beyond any
// arbitrary ordinal still signals; the only size bound is the bytes read from
// the registry JSON. Unrelated directory traffic (engines, stats, temp files)
// never enters the observed signature, so a noisy folder cannot turn every
// unrelated write into a UI rebuild.

#pragma once

#include <QtCore/QFileSystemWatcher>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVector>

namespace mplayer::native {

class AnimeJanaiCatalogWatcher final : public QObject {
    Q_OBJECT

public:
    explicit AnimeJanaiCatalogWatcher(QObject* parent = nullptr);

    /// Uses an unsaved manager folder instead of the saved external models
    /// directory. An empty string explicitly disables the external folder.
    void setExternalModelsDirectory(const QString& directory);

    /// Re-enumerates and re-arms every watch, then adopts the current metadata
    /// as the baseline. Does not emit by itself.
    void refresh();

signals:
    /// Emitted after filesystem activity settles when a relevant file event
    /// arrived, or when the observed metadata signature changed.
    void catalogChanged();

private:
    struct ObservedEntry {
        QString path;
        qint64 bytes = -1;
        qint64 modifiedMs = -1;
        /// Metadata-change (inode/MFT change) time. An in-place write or an
        /// atomic replacement updates it even when size and modification time
        /// are preserved, so a restored mtime cannot hide a graph edit. Qt
        /// aliases this time to the modification time on Windows, so the
        /// native NTFS change time is queried there instead.
        qint64 metadataChangedMs = -1;
        bool exists = false;
        bool isDir = false;

        bool operator==(const ObservedEntry& other) const {
            return path == other.path && bytes == other.bytes &&
                   modifiedMs == other.modifiedMs &&
                   metadataChangedMs == other.metadataChangedMs &&
                   exists == other.exists && isDir == other.isDir;
        }
    };

    struct Observation {
        QVector<ObservedEntry> entries;
        QStringList watchPaths;
    };

    Observation observe() const;
    void reconcileWatches(const QStringList& desired);
    void scheduleEvaluation();
    void evaluate();

    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QVector<ObservedEntry> m_snapshot;
    QString m_externalOverride;
    bool m_externalOverrideSet = false;
    bool m_relevantFileEvent = false;
    bool m_hasSnapshot = false;
};

}  // namespace mplayer::native
