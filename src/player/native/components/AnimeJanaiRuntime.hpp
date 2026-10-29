#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QSet>
#include <QtCore/QVector>

#include <functional>
#include <memory>

#include "native/components/AnimeJanaiConfig.hpp"
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

/// One still-missing acquisition item, priced at its pinned transfer size.
struct AnimeJanaiMissingDownload {
    QString label;
    qint64 bytes = 0;
};

/// The exact queue `provisionAnimeJanaiRuntime()` would fetch right now: it
/// answers from install records and stored-pack state only, so it neither
/// rehashes nor downloads.
QVector<AnimeJanaiMissingDownload> animeJanaiMissingDownloads(Backend backend);
qint64 animeJanaiMissingDownloadBytes(Backend backend);

namespace detail {
/// Validate the actual members of a 7-Zip technical listing before extraction.
bool validateAnimeJanaiArchiveListing(const QString& listing, bool allowInternalSymlinks,
                                      bool tarMembers, QString* error,
                                      const QSet<QString>& expectedFiles = {});
bool matchesPinnedLinuxAjiPair(const QString& dispatcherSha256, const QString& backendSha256);
enum class MixedRifeOrder { Before, After, Unknown };
/// Resolve upstream's chain-wide RIFE order from all ONNX temporal windows.
MixedRifeOrder mixedRifeOrder(bool requestedBefore, const QVector<std::optional<int>>& windows);
}

AnimeJanaiInstallation describeAnimeJanaiInstallation(Backend backend, bool rehash = false);
ComponentProvisionResult provisionAnimeJanaiRuntime(
    Backend backend, const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled);
ComponentProvisionResult importAnimeJanaiRuntime(
    const QString& sourceDir, Backend backend,
    const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled);

struct AnimeJanaiSourceContext {
    int width = 0;
    int height = 0;
    double fps = 0.0;
};

using Anime4KShaderProgramResolver = std::function<QByteArray(const QString&, QString*)>;

/// Process-local, all-or-nothing ownership for app-managed AJI engine-cache paths.
/// Planning acquires the complete cache-key set before staging artifacts; consumers
/// retain the shared token until the entire pipeline reaches a terminal state.
class AnimeJanaiStageBuildLease final {
public:
    AnimeJanaiStageBuildLease(const AnimeJanaiStageBuildLease&) = delete;
    AnimeJanaiStageBuildLease& operator=(const AnimeJanaiStageBuildLease&) = delete;
    ~AnimeJanaiStageBuildLease();

    static std::shared_ptr<AnimeJanaiStageBuildLease> tryAcquire(
        const QStringList& cacheKeys, const QString& ownerContext,
        QString* busyKey = nullptr, QString* busyOwner = nullptr);
    void release();
    bool held() const;

private:
    AnimeJanaiStageBuildLease(QStringList cacheKeys, QString ownerContext);
    QStringList cacheKeys_;
    QString ownerContext_;
    bool held_ = true;
};

struct AnimeJanaiPlanStage {
    enum class Kind { Aji, Anime4K };
    Kind kind = Kind::Aji;
    QString filterLabel;
    QByteArray filterSpec;
    QString configPath;
    QString statsPath;
    int firstStepNumber = 0;
    int lastStepNumber = 0;
    QString presetId;
    int scaleFactor = 1;
    std::shared_ptr<AnimeJanaiStageBuildLease> cacheLease;
};

struct AnimeJanaiPlan {
    bool enabled = false;
    bool available = false;
    QString presetId;
    QString reasonKey;
    QString reason;
    QString note; ///< Non-error description of an effective plan adjustment.
    Backend backend = Backend::TensorRT;
    int slot = 0;
    QString libraryPath;
    QString configPath;
    QString modelDir;
    QString rifeModelDir;
    QString trtexecPath;
    QString trtexecLibDir;
    QString statsPath;
    /// Folders whose shared libraries must already be resolvable when the
    /// fork dlopens its backend (TensorRT by bare soname). Unix-only: the
    /// surface preloads them RTLD_GLOBAL before attaching the filter.
    QStringList backendLibDirs;
    QByteArray filterSpec;
    QVector<AnimeJanaiPlanStage> stages;
    int selectedChainNumber = 0;
    bool needsSourceContext = false;
    bool requiresAji = false;
    QStringList cacheKeys;
    std::shared_ptr<AnimeJanaiStageBuildLease> cacheLease;
};

AnimeJanaiPlan prepareAnimeJanaiPlan(
    const QString& presetId, Backend backend,
    const std::function<bool()>& cancelled = {},
    const AnimeJanaiSourceContext& source = {},
    const Anime4KShaderProgramResolver& shaderResolver = {});

}  // namespace mplayer::native
