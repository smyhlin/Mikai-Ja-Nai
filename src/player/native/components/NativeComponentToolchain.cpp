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
bool gpuResourceInstalledForThisDevice(const ComponentPack* pack, const StoredPack& stored) {
    return pack != nullptr && stored.installed;
}

TensorRtClosureStatus tensorRtClosureStatus(const ComponentStore& store, const Bundle* bundle,
                                            int smMajor, int smMinor) {
    TensorRtClosureStatus status;
    const ComponentPack* runtimePack = tensorRtRuntimePack();
    const ComponentPack* kernelsPack = tensorRtResourcePackFor(smMajor, smMinor);
    if (runtimePack != nullptr) status.runtimeStored = storedPack(store, *runtimePack).installed;
    if (kernelsPack != nullptr) status.kernelsStored = storedPack(store, *kernelsPack).installed;

    // The bundle's preparation block is a statement about its own bytes: it
    // provides the builder resource for this GPU only when its declared file
    // inventory contains that exact runtime-relative file.
    if (bundle != nullptr && bundle->preparation.has_value() && kernelsPack != nullptr) {
        const BundlePreparation& preparation = *bundle->preparation;
        const QString declared = preparation.runtime.trt + QLatin1Char('/') +
                                 builderResourceFileName(*kernelsPack);
        if (!preparation.runtime.trt.isEmpty() && !preparation.runtime.cuda.isEmpty()) {
            for (const BundleFile& file : bundle->files) {
                if (file.path == declared) {
                    status.bundleProvidesDirs = true;
                    break;
                }
            }
        }
    }

    status.dirsAvailable =
        status.bundleProvidesDirs || (status.runtimeStored && status.kernelsStored);
    if (!status.dirsAvailable) {
        if (!status.runtimeStored) {
            status.missing.append(runtimePack != nullptr
                                      ? runtimePack->name
                                      : QStringLiteral("trt-runtime"));
        }
        if (!status.kernelsStored) {
            status.missing.append(kernelsPack != nullptr
                                      ? kernelsPack->name
                                      : QStringLiteral("trt-sm%1%2").arg(smMajor).arg(smMinor));
        }
    }
    return status;
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
