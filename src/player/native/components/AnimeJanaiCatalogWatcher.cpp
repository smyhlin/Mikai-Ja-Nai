#include "native/components/AnimeJanaiCatalogWatcher.hpp"

#include "native/bundle/NativePaths.hpp"
#include "native/components/AnimeJanaiConfig.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>

#include <algorithm>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace mplayer::native {
namespace {

constexpr int kCoalesceMilliseconds = 250;
// Observation is bounded by bytes read, never by catalogue cardinality. The
// public inventory lists every relevant entry a directory holds and every
// registered pin inside the registry, so any per-file, per-pin or per-watch cap
// here would silently stop reporting a late paste, rename or replacement.
// Unrelated names (engines, stats, temp files) are filtered by name below and
// never enter the signature, which is what keeps a noisy folder quiet.
constexpr qint64 kMaxRegistryBytes = 4LL * 1024LL * 1024LL;

#ifdef Q_OS_WIN
// QFileInfo::metadataChangeTime() cannot see an in-place write on Windows: Qt
// stores the last-write time in its change-time slot (see
// QFileSystemMetaData::fillFromFindInfo), so a write that restores the
// modification time leaves the observed metadata identical. NTFS maintains a
// separate, system-managed change time that every write advances and that
// QFile::setFileTime cannot reset; it is only reachable through a file handle.
qint64 nativeChangeTimeMs(const QString& path) {
    QString nativePath = QDir::toNativeSeparators(path);
    // Qt itself stats such paths; use the extended form so a catalogue deeper
    // than MAX_PATH keeps the same coverage instead of silently dropping it.
    if (!nativePath.startsWith(QStringLiteral("\\\\?\\"))) {
        nativePath = nativePath.startsWith(QStringLiteral("\\\\"))
                         ? QStringLiteral("\\\\?\\UNC\\") + nativePath.mid(2)
                         : QStringLiteral("\\\\?\\") + nativePath;
    }
    const HANDLE handle =
        ::CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()), FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    FILE_BASIC_INFO basicInfo{};
    const BOOL queried =
        ::GetFileInformationByHandleEx(handle, FileBasicInfo, &basicInfo, sizeof(basicInfo));
    ::CloseHandle(handle);
    if (!queried) return -1;
    const qint64 ticks = basicInfo.ChangeTime.QuadPart;
    if (ticks <= 0) return -1;
    constexpr qint64 kTicksPerMillisecond = 10000;                // FILETIME is 100 ns
    constexpr qint64 kEpochOffsetMilliseconds = 11644473600000;   // 1601 -> 1970
    return ticks / kTicksPerMillisecond - kEpochOffsetMilliseconds;
}
#endif

qint64 metadataChangeMs(const QString& path, const QFileInfo& info) {
#ifdef Q_OS_WIN
    const qint64 nativeChangeMs = nativeChangeTimeMs(path);
    if (nativeChangeMs > 0) return nativeChangeMs;
#else
    Q_UNUSED(path);
#endif
    // Non-Windows platforms expose a real inode change time. The Windows
    // fallback only matters when the handle query failed or the file system
    // keeps no change time, where it degrades to the modification time.
    return info.metadataChangeTime().toMSecsSinceEpoch();
}

QString externalRegistryPath() {
    // The public API exposes the custom models directory but not the registry
    // file beside it; keep the one private filename here in sync with
    // AnimeJanaiConfig.cpp.
    return QDir(QFileInfo(customAnimeJanaiModelsDir()).absolutePath())
        .filePath(QStringLiteral("external-models.json"));
}

QString pathKey(QString path) {
    path = QDir::cleanPath(path);
    if (pathCaseSensitivity() == Qt::CaseInsensitive) path = path.toLower();
    return path;
}

bool trustedAppTarget(const QString& path) {
    if (path.isEmpty() || !nativePathTrust(path, true).trusted) return false;
    const QString root = nativeRoot();
    if (root.isEmpty() || !pathWithinRoot(root, path)) return false;
    if (!QFileInfo::exists(path)) return true;
    return pathHasNoReparsePoints(root, path);
}

bool trustedAppAncestor(const QString& path) {
    // Before the app-owned root exists the nearest existing ancestor can sit
    // above it; anchor that walk at the application directory.
    if (path.isEmpty() || !nativePathTrust(path, true).trusted) return false;
    const QString appDir = QCoreApplication::applicationDirPath();
    if (appDir.isEmpty() || !pathWithinRoot(appDir, path)) return false;
    return pathHasNoReparsePoints(appDir, path);
}

bool trustedExternalPath(const QString& path) {
    if (path.isEmpty()) return false;
    const NativePathTrust trust = nativePathTrust(path, true);
    return trust.trusted && trust.form != NativePathForm::Device;
}

QString resolvedExternalPath(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

bool trustedTarget(const QString& path, bool appOwned) {
    return appOwned ? trustedAppTarget(path) : trustedExternalPath(path);
}

bool trustedAncestor(const QString& path, bool appOwned) {
    return appOwned ? trustedAppAncestor(path) : trustedExternalPath(path);
}

struct WatchCollector {
    QStringList paths;
    QSet<QString> keys;

    void add(const QString& path) {
        if (path.isEmpty()) return;
        const QString cleaned = QDir::cleanPath(path);
        const QString key = pathKey(cleaned);
        if (key.isEmpty() || keys.contains(key)) return;
        keys.insert(key);
        paths.push_back(cleaned);
    }

    void addExistingOrAncestor(const QString& path, bool appOwned) {
        if (path.isEmpty()) return;
        if (QFileInfo::exists(path)) {
            if (trustedTarget(path, appOwned)) add(path);
            return;
        }
        const QString ancestor = nearestExistingAncestor(path);
        if (!ancestor.isEmpty() && trustedAncestor(ancestor, appOwned)) add(ancestor);
    }

    void addFileAndParent(const QString& path, bool appOwned) {
        if (QFileInfo::exists(path) && trustedTarget(path, appOwned)) add(path);
        const QString parent = QFileInfo(path).absolutePath();
        if (!parent.isEmpty()) addExistingOrAncestor(parent, appOwned);
    }

    void addDirectoryAndParent(const QString& path, bool appOwned) {
        if (QFileInfo(path).isDir()) {
            if (trustedTarget(path, appOwned)) add(path);
        } else {
            addExistingOrAncestor(path, appOwned);
        }
        const QString parent = QFileInfo(path).absolutePath();
        if (!parent.isEmpty()) addExistingOrAncestor(parent, appOwned);
    }
};

QStringList registeredGraphPaths(const QString& registryPath) {
    QStringList paths;
    QFile file(registryPath);
    if (!file.open(QIODevice::ReadOnly)) return paths;
    if (file.size() < 0 || file.size() > kMaxRegistryBytes) return paths;
    const QByteArray bytes = file.read(kMaxRegistryBytes + 1);
    if (bytes.size() > kMaxRegistryBytes) return paths;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return paths;
    const QJsonValue models = document.object().value(QStringLiteral("models"));
    if (!models.isArray()) return paths;

    // Pins may be temporarily invalid or the graph may be missing; watching
    // every representable pinned path lets recreation notify the catalogue.
    QSet<QString> seen;
    for (const QJsonValue& value : models.toArray()) {
        const QString raw = value.toObject().value(QStringLiteral("path")).toString();
        if (raw.isEmpty() || !QFileInfo(raw).isAbsolute() || !trustedExternalPath(raw)) continue;
        const QString cleaned = QDir::cleanPath(raw);
        const QString key = pathKey(cleaned);
        if (key.isEmpty() || seen.contains(key)) continue;
        seen.insert(key);
        paths.push_back(cleaned);
    }
    return paths;
}

}  // namespace

AnimeJanaiCatalogWatcher::AnimeJanaiCatalogWatcher(QObject* parent)
    : QObject(parent), m_watcher(this), m_debounce(this) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kCoalesceMilliseconds);
    connect(&m_debounce, &QTimer::timeout, this, &AnimeJanaiCatalogWatcher::evaluate);

    // A file watch is only installed for a catalogue-relevant file (config,
    // registry, model or sidecar). Publish it even when size and timestamps are
    // unchanged, because an atomic or in-place replacement can preserve both.
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] {
        m_relevantFileEvent = true;
        scheduleEvaluation();
    });
    // Directory events are ambiguous (engine/stat/temp writes land here), so
    // those are filtered by the observed metadata signature instead.
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
            [this] { scheduleEvaluation(); });

    refresh();
}

void AnimeJanaiCatalogWatcher::setExternalModelsDirectory(const QString& directory) {
    const QString trimmed = directory.trimmed();
    if (m_externalOverrideSet && m_externalOverride == trimmed) return;
    m_externalOverrideSet = true;
    m_externalOverride = trimmed;
    scheduleEvaluation();
}

void AnimeJanaiCatalogWatcher::refresh() {
    m_debounce.stop();
    m_relevantFileEvent = false;
    const Observation observation = observe();
    reconcileWatches(observation.watchPaths);
    m_snapshot = observation.entries;
    m_hasSnapshot = true;
}

AnimeJanaiCatalogWatcher::Observation AnimeJanaiCatalogWatcher::observe() const {
    Observation observation;
    WatchCollector watch;

    const auto appendFile = [&observation](const QString& filePath) {
        const QFileInfo info(filePath);
        ObservedEntry entry;
        entry.path = QDir::cleanPath(filePath);
        entry.exists = info.exists();
        entry.isDir = info.isDir();
        if (info.isFile()) {
            entry.bytes = info.size();
            entry.modifiedMs = info.lastModified().toMSecsSinceEpoch();
            entry.metadataChangedMs = metadataChangeMs(entry.path, info);
        }
        observation.entries.push_back(entry);
    };

    const auto observeDirectory = [&observation, &watch](const QString& directory,
                                                          bool appOwned, bool includeSidecars) {
        const QFileInfo directoryInfo(directory);
        ObservedEntry directoryEntry;
        directoryEntry.path = QDir::cleanPath(directory);
        directoryEntry.exists = directoryInfo.exists();
        directoryEntry.isDir = directoryInfo.isDir();
        observation.entries.push_back(directoryEntry);
        if (!directoryInfo.isDir()) return;

        // Complete enumeration of the directory: the public inventory lists
        // every relevant entry it holds, so stopping at a fixed count would
        // silently drop lexicographically late models from observation.
        QDirIterator iterator(directory, QDir::Files | QDir::NoSymLinks,
                              QDirIterator::NoIteratorFlags);
        while (iterator.hasNext()) {
            iterator.next();
            const QString name = iterator.fileName();
            const bool model = name.endsWith(QStringLiteral(".onnx"), Qt::CaseInsensitive);
            const bool sidecar =
                includeSidecars &&
                name.endsWith(QStringLiteral(".source.json"), Qt::CaseInsensitive);
            if (!model && !sidecar) continue;

            const QFileInfo info = iterator.fileInfo();
            if (!info.isFile()) continue;
            const QString filePath = QDir::cleanPath(info.absoluteFilePath());
            if (!trustedTarget(filePath, appOwned)) continue;

            ObservedEntry entry;
            entry.path = filePath;
            entry.exists = true;
            entry.bytes = info.size();
            entry.modifiedMs = info.lastModified().toMSecsSinceEpoch();
            entry.metadataChangedMs = metadataChangeMs(entry.path, info);
            observation.entries.push_back(entry);
            watch.add(filePath);
        }
    };

    const QString configPath = AnimeJanaiConfigStore::configFilePath();
    AnimeJanaiConfig config;
    if (trustedAppTarget(configPath)) {
        QString configError;
        config = AnimeJanaiConfigStore::load(&configError);
        appendFile(configPath);
        watch.addFileAndParent(configPath, true);
    }

    const QString customDirectory = customAnimeJanaiModelsDir();
    if (trustedAppTarget(customDirectory)) {
        observeDirectory(customDirectory, true, true);
        watch.addDirectoryAndParent(customDirectory, true);
    }

    const QString configuredExternal =
        (m_externalOverrideSet ? m_externalOverride : config.externalModelsDir).trimmed();
    if (!configuredExternal.isEmpty()) {
        const QString externalDirectory = resolvedExternalPath(configuredExternal);
        if (trustedExternalPath(externalDirectory)) {
            observeDirectory(externalDirectory, false, false);
            watch.addDirectoryAndParent(externalDirectory, false);
        }
    }

    const QString registryPath = externalRegistryPath();
    if (trustedAppTarget(registryPath)) {
        appendFile(registryPath);
        watch.addFileAndParent(registryPath, true);
        const QStringList registeredGraphs = registeredGraphPaths(registryPath);
        for (const QString& graphPath : registeredGraphs) {
            if (!trustedExternalPath(graphPath)) continue;
            appendFile(graphPath);
            watch.addFileAndParent(graphPath, false);
        }
    }

    std::sort(observation.entries.begin(), observation.entries.end(),
              [](const ObservedEntry& left, const ObservedEntry& right) {
                  const int order = left.path.compare(right.path, pathCaseSensitivity());
                  return order != 0 ? order < 0 : left.path < right.path;
              });
    observation.watchPaths = watch.paths;
    return observation;
}

void AnimeJanaiCatalogWatcher::reconcileWatches(const QStringList& desired) {
    QSet<QString> wanted;
    QStringList orderedPaths;
    orderedPaths.reserve(desired.size());
    for (const QString& path : desired) {
        const QString key = pathKey(path);
        if (key.isEmpty() || wanted.contains(key)) continue;
        wanted.insert(key);
        orderedPaths.push_back(path);
    }

    QStringList actualPaths = m_watcher.files();
    actualPaths += m_watcher.directories();
    QSet<QString> present;
    QStringList stale;
    for (const QString& path : actualPaths) {
        const QString key = pathKey(path);
        if (key.isEmpty() || present.contains(key)) continue;
        if (!wanted.contains(key)) {
            stale.push_back(path);
            continue;
        }
        present.insert(key);
    }
    if (!stale.isEmpty()) m_watcher.removePaths(stale);

    QStringList missing;
    for (const QString& path : orderedPaths) {
        if (!present.contains(pathKey(path))) missing.push_back(path);
    }
    if (!missing.isEmpty()) m_watcher.addPaths(missing);
}

void AnimeJanaiCatalogWatcher::scheduleEvaluation() {
    // Coalesce bursts, but do not let a continuously growing file postpone the
    // notification forever: the window starts on the first event and is not
    // restarted while evaluation is already pending.
    if (!m_debounce.isActive()) m_debounce.start();
}

void AnimeJanaiCatalogWatcher::evaluate() {
    const Observation observation = observe();
    reconcileWatches(observation.watchPaths);

    const bool signatureChanged = !m_hasSnapshot || observation.entries != m_snapshot;
    const bool fileEvent = m_relevantFileEvent;
    m_snapshot = observation.entries;
    m_hasSnapshot = true;
    m_relevantFileEvent = false;

    if (signatureChanged || fileEvent) emit catalogChanged();
}

}  // namespace mplayer::native
