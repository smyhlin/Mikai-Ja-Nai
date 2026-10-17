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

}  // namespace mikai::app::component_store_detail
