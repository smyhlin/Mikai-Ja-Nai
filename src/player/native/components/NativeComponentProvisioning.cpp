#include "native/components/NativeComponentProvisioning.hpp"

#include "backend/Downloads.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QUrl>

namespace mplayer::native {
namespace {

QString translated(const char* message) {
    return QCoreApplication::translate("NativeComponentProvisioning", message);
}

// One download at a time across the whole process: two concurrent runs would
// race on the same scratch and on the store record.
QMutex g_runMutex;
bool g_running = false;

}  // namespace

ComponentProvisionResult provisionTensorRtComponents(
    const ComponentStore& store, const QVector<ComponentPack>& packs, const QString& scratchDirectory,
    const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled) {
    ComponentProvisionResult result;
    auto fail = [&result](const QString& key, const QString& text) {
        result.ok = false;
        result.reasonKey = key;
        result.reason = text;
        return result;
    };

    {
        QMutexLocker lock(&g_runMutex);
        if (g_running) {
            result.reasonKey = QStringLiteral("native.components.busy");
            result.reason = translated("A component download is already running.");
            return result;
        }
        g_running = true;
    }
    const auto releaseRun = qScopeGuard([] {
        QMutexLocker lock(&g_runMutex);
        g_running = false;
    });

    if (store.root.isEmpty()) {
        return fail(QStringLiteral("native.components.install"),
                    QStringLiteral("the component store root is not available"));
    }
    if (packs.isEmpty()) {
        return fail(QStringLiteral("native.components.install"),
                    QStringLiteral("no component packs were named"));
    }

    const QString sevenZip = mikai::backend::ensureSevenZip();
    if (sevenZip.isEmpty()) {
        return fail(QStringLiteral("native.components.extract"),
                    translated("7-Zip is not available to unpack the component archive."));
    }

    const QDir scratch(scratchDirectory);
    if (!QDir().mkpath(scratch.absolutePath())) {
        return fail(QStringLiteral("native.components.install"),
                    QStringLiteral("%1 could not be created").arg(scratch.absolutePath()));
    }

    for (const ComponentPack& pack : packs) {
        const QString archive = scratch.filePath(pack.asset);
        QString downloadReason;
        const bool downloaded = mikai::backend::downloadToFile(
            QUrl(pack.url), archive, pack.assetBytes, pack.assetSha256,
            [&](qint64 received, qint64 total) {
                if (progress) progress({pack.name, received, total});
            },
            cancelled, &downloadReason);
        if (!downloaded) {
            const bool wasCancelled = cancelled && cancelled();
            return fail(wasCancelled ? QStringLiteral("native.components.cancelled")
                                     : QStringLiteral("native.components.fetch"),
                        downloadReason);
        }

        const QString extractDir = scratch.filePath(QStringLiteral("extract"));
        QDir(extractDir).removeRecursively();
        QDir().mkpath(extractDir);
        QString extractReason;
        if (!mikai::backend::extractArchive(sevenZip, archive, extractDir, &extractReason)) {
            return fail(QStringLiteral("native.components.extract"), extractReason);
        }

        QString installKey;
        QString installReason;
        if (!installPack(store, pack, extractDir, &installKey, &installReason)) {
            return fail(installKey, installReason);
        }
        result.installedPacks.append(pack.name);

        QFile::remove(archive);
        QDir(extractDir).removeRecursively();
    }

    result.ok = true;
    return result;
}

}  // namespace mplayer::native
