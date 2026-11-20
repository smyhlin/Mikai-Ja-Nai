#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVector>

#include <functional>
#include <optional>

namespace mikai::backend {

enum class UpdateCheckStatus { UpdateAvailable, UpToDate, Failed };

enum class InstallKind {
    WindowsInstaller,
    WindowsPortable,
    LinuxAppImage,
    LinuxTarball,
    SystemPackage,
    Unknown,
};

struct UpdateAsset {
    QString name;
    QUrl downloadUrl;
    qint64 size = 0;
};

struct UpdateCheckResult {
    UpdateCheckStatus status = UpdateCheckStatus::Failed;
    QString version;
    QUrl releaseUrl;
    QString tagName;
    QVector<UpdateAsset> assets;
};

struct StagedUpdate {
    QString stagedPath;
    QString stagingDir;
    QString assetName;
    QString version;
    InstallKind installKind = InstallKind::Unknown;
};

// Blocking network access: call only from a worker thread.
UpdateCheckResult checkForUpdates(const QString& currentVersion);

// Deterministic parser used by tests and the network implementation.
UpdateCheckResult parseLatestReleaseResponse(const QByteArray& response,
                                            const QString& currentVersion);

// Detect how the current binary was installed so we only self-update writable
// installs and refuse to overwrite system-managed packages (/usr, /opt, etc.).
InstallKind detectInstallKind(const QString& applicationFilePath = {},
                              const QString& appImageEnv = {});
bool isSelfUpdatable(InstallKind kind);

// Deterministic release asset & SHA256SUMS helpers.
std::optional<UpdateAsset> selectUpdateAsset(const QVector<UpdateAsset>& assets, InstallKind kind);
std::optional<UpdateAsset> findChecksumsAsset(const QVector<UpdateAsset>& assets);
QString parseSha256Sums(const QByteArray& content, const QString& assetName);

// Download SHA256SUMS + platform release asset into a staging folder and verify
// exact byte length and SHA-256 digest before returning true. Blocking; run on
// a worker thread.
bool downloadAndVerifyUpdate(const UpdateCheckResult& release, InstallKind kind,
                             const std::function<void(qint64 received, qint64 total)>& progress,
                             StagedUpdate* out, QString* error);

// Launch a detached platform updater helper that waits for `currentPid`, backs
// up the previous binary/tree for rollback, applies `staged`, and relaunches.
bool launchUpdateInstaller(const StagedUpdate& staged, qint64 currentPid, QString* error);

}  // namespace mikai::backend
