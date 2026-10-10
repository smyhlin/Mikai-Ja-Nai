#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <functional>

#include "native/components/NativeComponentProvisioning.hpp"
#include "native/status/NativeTypes.hpp"

namespace mplayer::native {

inline constexpr char kAnimeJanaiFilterLabel[] = "mikai_aji";

struct AnimeJanaiPreset {
    QString id;
    QString label;
    int slot = 0;
    bool rife = false;
    QString modelFile;
    bool supported = true;
    QString reason;
};

QVector<AnimeJanaiPreset> animeJanaiPresets();

struct AnimeJanaiInstallation {
    bool installed = false;
    QString root;
    QString reasonKey;
    QString reason;
    bool heavyRifeSupported = false;
};

AnimeJanaiInstallation describeAnimeJanaiInstallation(Backend backend, bool rehash = false);
ComponentProvisionResult provisionAnimeJanaiRuntime(
    Backend backend, const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled);
ComponentProvisionResult importAnimeJanaiRuntime(
    const QString& sourceDir, Backend backend,
    const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled);

struct AnimeJanaiPlan {
    bool enabled = false;
    bool available = false;
    QString presetId;
    QString reasonKey;
    QString reason;
    Backend backend = Backend::TensorRT;
    int slot = 0;
    QString libraryPath;
    QString configPath;
    QString modelDir;
    QString rifeModelDir;
    QString trtexecPath;
    QString trtexecLibDir;
    QString statsPath;
    QByteArray filterSpec;
};

AnimeJanaiPlan prepareAnimeJanaiPlan(const QString& presetId, Backend backend,
                                     const std::function<bool()>& cancelled = {});

}  // namespace mplayer::native
