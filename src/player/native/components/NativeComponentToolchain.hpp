#pragma once

#include <QString>
#include <QStringList>

#include "native/components/NativeComponentStore.hpp"
#include "native/components/NativeGpuProbe.hpp"

namespace mplayer::native {

struct ComponentBinDirs {
    QString trtBinDir;
    QString cudaBinDir;
    QString packName;
    bool fromStore = false;
    QString reasonKey;
    QString reason;
};

ComponentBinDirs componentBinDirs(const QString& bundleRoot, const QString& trtDirName,
                                  const QString& cudaDirName, const QStringList& declaredTrtFiles,
                                  const GpuComputeCapability& gpu, const ComponentStore& store);
bool componentBinDirsAvailable(const ComponentBinDirs& dirs);
bool gpuResourceInstalledForThisDevice(const ComponentPack* pack, const StoredPack& stored,
                                       bool componentDirsAvailable);

}  // namespace mplayer::native
