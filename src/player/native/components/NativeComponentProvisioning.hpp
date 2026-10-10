#pragma once

#include <QString>
#include <QStringList>

#include "native/components/NativeComponentStore.hpp"
#include "native/models/NativeRife.hpp"

#include <functional>

namespace mplayer::native {

struct ComponentProvisionProgress {
    QString pack;
    qint64 received = 0;
    qint64 total = 0;
};

struct ComponentProvisionResult {
    bool ok = false;
    bool cancelled = false;
    QStringList installedPacks;
    QString reasonKey;
    QString reason;
};

ComponentProvisionResult provisionTensorRtComponents(
    const ComponentStore& store, const QVector<ComponentPack>& packs, const QString& scratchDirectory,
    const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled);
ComponentProvisionResult provisionRifeArchive(
    const RifeArchiveStore& store, const QVector<RifeModelPin>& pins,
    const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled);

}  // namespace mplayer::native
