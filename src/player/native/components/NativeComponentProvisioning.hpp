#pragma once

#include <QString>
#include <QStringList>

#include "native/components/NativeComponentStore.hpp"
#include "native/models/NativeRife.hpp"

#include <functional>

namespace mplayer::native {

enum class ComponentProvisionStage {
    Downloading,
    Extracting,
    Installing,
    Verifying,
    Installed
};

struct ComponentProvisionProgress {
    QString pack;
    qint64 received = 0;
    qint64 total = 0;
    ComponentProvisionStage stage = ComponentProvisionStage::Downloading;
};

struct ComponentProvisionResult {
    bool ok = false;
    bool cancelled = false;
    QStringList installedPacks;
    QString reasonKey;
    QString reason;
};

/// A pack archive the caller already downloaded and digest-verified against the
/// same pin. When the pack being provisioned is `packName`, its bytes are
/// installed from `path` instead of being fetched a second time, and the file is
/// left in place for the caller, which keeps using it afterwards. The caller
/// owns the path's lifetime.
struct VerifiedComponentArchive {
    QString packName;
    QString path;
};

ComponentProvisionResult provisionTensorRtComponents(
    const ComponentStore& store, const QVector<ComponentPack>& packs, const QString& scratchDirectory,
    const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled,
    const VerifiedComponentArchive* reusedArchive = nullptr);

ComponentProvisionResult provisionRifeArchive(
    const RifeArchiveStore& store, const QVector<RifeModelPin>& pins,
    const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled);

}  // namespace mplayer::native
