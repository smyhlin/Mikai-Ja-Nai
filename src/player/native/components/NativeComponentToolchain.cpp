#include "native/components/NativeComponentToolchain.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QLocale>

namespace mplayer::native {
namespace {

QString translated(const char* message) {
    return QCoreApplication::translate("NativeComponentToolchain", message);
}

}  // namespace

bool componentBinDirsAvailable(const ComponentBinDirs& dirs) {
    return dirs.reasonKey.isEmpty() && !dirs.trtBinDir.isEmpty() && !dirs.cudaBinDir.isEmpty();
}
bool gpuResourceInstalledForThisDevice(const ComponentPack* pack, const StoredPack& stored,
                                       bool componentDirsAvailable) {
    return pack != nullptr && (stored.installed || componentDirsAvailable);
}

ComponentBinDirs componentBinDirs(const QString& bundleRoot, const QString& trtDirName,
                                  const QString& cudaDirName, const QStringList& declaredTrtFiles,
                                  const GpuComputeCapability& gpu, const ComponentStore& store) {
    ComponentBinDirs result;
    result.trtBinDir = QDir(bundleRoot).filePath(trtDirName);
    result.cudaBinDir = QDir(bundleRoot).filePath(cudaDirName);

    // No NVIDIA device observed: the bundle's own directories are the only
    // statement of where its runtimes are, and a resource that happens to fit
    // still works.
    if (gpu.smMajor <= 0) {
        return result;
    }

    const ComponentPack* resourcePack = tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
    if (resourcePack == nullptr) {
        result.reasonKey = QStringLiteral("native.components.missing");
        result.reason = translated("The release publishes no builder resource for SM %1.%2.")
                            .arg(gpu.smMajor)
                            .arg(gpu.smMinor);
        return result;
    }

    const QString declaredResource =
        trtDirName + QLatin1Char('/') + builderResourceFileName(*resourcePack);
    if (declaredTrtFiles.contains(declaredResource)) {
        return result;
    }

    const ComponentPack* runtimePack = tensorRtRuntimePack();
    if (runtimePack != nullptr) {
        const StoredPack runtime = storedPack(store, *runtimePack);
        const StoredPack resource = storedPack(store, *resourcePack);
        if (runtime.installed && resource.installed) {
            result.trtBinDir = resource.directory;
            result.cudaBinDir = resource.directory;
            result.packName = resourcePack->name;
            result.fromStore = true;
            return result;
        }
    }

    result.trtBinDir.clear();
    result.cudaBinDir.clear();
    result.reasonKey = QStringLiteral("native.components.missing");
    result.reason = translated("The TensorRT builder resource for this GPU (%1, %2) is not "
                               "installed; download it before preparing an engine.")
                        .arg(resourcePack->name,
                             QLocale().formattedDataSize(resourcePack->assetBytes));
    return result;
}

}  // namespace mplayer::native
