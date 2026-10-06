#pragma once

#include <QString>

#include "native/components/NativeComponentCatalog.hpp"

namespace mplayer::native {

struct ComponentStore {
    QString root;
};

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

}  // namespace mplayer::native
