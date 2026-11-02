#pragma once

#include "dialogs/ComponentStoreTab.hpp"

#include <native/bundle/NativeBundle.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>
#include <native/components/AnimeJanaiRuntime.hpp>
#include <native/components/NativeComponentCatalog.hpp>
#include <native/components/NativeComponentStore.hpp>
#include <native/components/NativeComponentToolchain.hpp>
#include <native/components/NativeGpuProbe.hpp>
#include <native/models/NativeRife.hpp>
#include <native/models/NativeSpatialAssets.hpp>

#include <QString>
#include <QVector>
#include <QVariantMap>
#include <atomic>

namespace mikai::app::component_store_detail {

mplayer::native::ComponentBinDirs installedComponentBinDirs(
    const mplayer::native::GpuComputeCapability& gpu,
    const mplayer::native::ComponentStore& store);

const mplayer::native::StoredSpatialAsset* spatialInventoryRow(
    const QVector<mplayer::native::StoredSpatialAsset>& stored, const QString& id);

QVariantMap inventorySnapshot(const mplayer::native::GpuComputeCapability& gpu,
                              bool verifyRife, bool rehashBackend,
                              const std::atomic<bool>* cancelled);

/// The named failure lines a selected TensorRT closure owes the user: the
/// missing store pack is reported by name, and a present-but-unusable closure
/// names the prerequisite separately. Empty exactly when the selection is
/// satisfied. Wording stays in the tab's translation context.
QStringList tensorRtClosureFailureLines(
    const mplayer::native::TensorRtClosureStatus& closure, bool runtimeSelected,
    bool kernelsSelected, const QString& kernelsName);

}  // namespace mikai::app::component_store_detail
