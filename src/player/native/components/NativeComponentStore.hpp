#pragma once

#include <QString>
#include <QVector>

#include <functional>

#include "native/components/NativeComponentCatalog.hpp"
#include "native/models/NativeRife.hpp"

namespace mplayer::native {

struct ComponentStore {
    QString root;
};
struct RifeArchiveStore {
    QString root;
};

struct StoredRifeGraph {
    bool installed = false;
    QString fileName;
    QString path;
    qint64 bytes = 0;
    QString reason;
};

RifeArchiveStore rifeArchiveStore();
StoredRifeGraph storedRifeGraph(const RifeArchiveStore& store, const RifeModelPin& pin);
StoredRifeGraph verifyRifeGraph(const RifeArchiveStore& store, const RifeModelPin& pin);

/// Publishes the complete allowlisted RIFE graph inventory from an already
/// validated extraction directory, rolling back if replacement cannot be published.
bool installRifeGraphs(const RifeArchiveStore& store, const QVector<RifeModelPin>& pins,
                       const QString& extractionRoot, const std::function<bool()>& cancelled,
                       QString* reason);

ComponentStore componentStore();

struct StoredPack {
    bool installed = false;
    QString pack;
    QString directory;
    qint64 bytes = 0;
    QString reasonKey;
    QString reason;
};

QString componentReleaseDirectory(const ComponentStore& store);

StoredPack storedPack(const ComponentStore& store, const ComponentPack& pack);

/// Re-hashes every file the record attributes to `pack` and compares it with the
/// recorded digest and size. This is the manual check behind the tab's Verify
/// button: `storedPack` trusts the recorded size, this proves the bytes.
StoredPack verifyPack(const ComponentStore& store, const ComponentPack& pack);

/// Every pack name the record lists, in record order.
QStringList installedPackNames(const ComponentStore& store);

bool installPack(const ComponentStore& store, const ComponentPack& pack,
                 const QString& extractionRoot, QString* reasonKey, QString* reason);

/// Removes `pack` from the store record and deletes any of its files that are
/// not shared by another still-installed pack (matching AnimeJaNaiUpdater --remove).
bool removePack(const ComponentStore& store, const ComponentPack& pack,
                QString* reasonKey, QString* reason);

}  // namespace mplayer::native
