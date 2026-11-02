#pragma once

#include <QString>
#include <QStringList>

#include "native/bundle/NativeBundle.hpp"
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

/// One answer to "is the TensorRT builder closure usable here": which of the
/// two store packs are actually on disk, whether the installed bundle itself
/// declares this GPU's builder resource, and what is missing by name.
///
/// This is the single predicate the app and the player share. The badge, the
/// failure line, the Apply decision and the model's readiness all read it (or
/// the store truth inside it) instead of deriving a second opinion, so a card
/// can never claim "installed" while another surface names the missing pack.
struct TensorRtClosureStatus {
    bool runtimeStored = false;      ///< storedPack(trt-runtime).installed
    bool kernelsStored = false;      ///< storedPack(trt-smXX).installed
    bool bundleProvidesDirs = false; ///< bundle declares this GPU's builder resource
    /// Both store packs are present, or the bundle already carries the resource.
    bool dirsAvailable = false;
    /// Stable pack names, e.g. {"trt-runtime"} or {"trt-sm75"}. Empty exactly
    /// when `dirsAvailable` is true.
    QStringList missing;
};

TensorRtClosureStatus tensorRtClosureStatus(const ComponentStore& store, const Bundle* bundle,
                                            int smMajor, int smMinor);

/// Store truth only: the pack is installed when the store record and its
/// declared files say so. A pack that happens to fill a gap the bundle leaves
/// open is still downloaded-but-unusable until the closure agrees, and callers
/// that need that answer ask `tensorRtClosureStatus`.
bool gpuResourceInstalledForThisDevice(const ComponentPack* pack, const StoredPack& stored);

}  // namespace mplayer::native
