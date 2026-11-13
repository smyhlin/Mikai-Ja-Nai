#pragma once

#include "AnimeJanaiRuntime.hpp"

#include <QJsonArray>

namespace mplayer::native {

struct AnimeJanaiRuntimeStorage {
    QString root;
    QString location;
    QString reason;
    bool available = false;
    bool moved = false;
};

struct AnimeJanaiSharedRuntime {
    bool installed = false;
    QString root;
    QString trtexecPath;
    QStringList libraryDirs;
    QString reason;
};

QString animeJanaiSharedRuntimeStoreRoot(QString* reason = nullptr);
AnimeJanaiRuntimeStorage describeAnimeJanaiRuntimeStorage();
bool animeJanaiRuntimeRelocationInProgress();

StoredPack describeSharedAnimeJanaiRuntimePack(
    const QString& root, const ComponentPack& pack, bool rehash = false,
    const std::function<bool()>& cancelled = {});
bool animeJanaiSharedRuntimeRefCurrent(const QString& root, Backend backend,
                                      QString* reason = nullptr);
AnimeJanaiSharedRuntime describeAnimeJanaiSharedRuntime(
    Backend backend, const QString& ajiDirectory, bool rehash = false,
    const std::function<bool()>& cancelled = {});
QVector<AnimeJanaiMissingDownload> animeJanaiSharedRuntimeMissingDownloads(Backend backend);
QJsonArray animeJanaiSharedRuntimeAcquisitions(Backend backend);

// An empty ajiDirectory retains the packs for a prepared Native consumer without a dispatcher.
ComponentProvisionResult provisionAnimeJanaiSharedRuntime(
    Backend backend, const QString& ajiDirectory,
    const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled);
ComponentProvisionResult relocateAnimeJanaiSharedRuntime(
    const QString& destination, bool restoreDefault,
    const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled);

}  // namespace mplayer::native
