#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

namespace mikai::backend {

enum class UpdateCheckStatus { UpdateAvailable, UpToDate, Failed };

struct UpdateCheckResult {
    UpdateCheckStatus status = UpdateCheckStatus::Failed;
    QString version;
    QUrl releaseUrl;
};

// Blocking network access: call only from a worker thread.
UpdateCheckResult checkForUpdates(const QString& currentVersion);

// Deterministic parser used by tests and the network implementation.
UpdateCheckResult parseLatestReleaseResponse(const QByteArray& response,
                                            const QString& currentVersion);

}  // namespace mikai::backend
