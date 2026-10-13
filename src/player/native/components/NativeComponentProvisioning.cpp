#include "native/components/NativeComponentProvisioning.hpp"

#include "native/models/NativeRife.hpp"

#include "backend/Downloads.hpp"
#include "native/bundle/NativePaths.hpp"
#include "native/components/NativeComponentCatalog.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QUrl>
#include <QProcess>

namespace mplayer::native {
namespace {

QString translated(const char* message) {
    return QCoreApplication::translate("NativeComponentProvisioning", message);
}

// One download at a time across the whole process: two concurrent runs would
// race on the same scratch and on the store record.
QMutex g_runMutex;
bool g_running = false;
QMutex g_rifeRunMutex;
bool g_rifeRunning = false;

bool validateRifeArchiveListing(const QString& sevenZip, const QString& archive,
                                const QVector<RifeModelPin>& pins, QString* reason) {
    QProcess process;
    process.start(sevenZip, {QStringLiteral("l"), QStringLiteral("-slt"), archive});
    if (!process.waitForStarted() || !process.waitForFinished(-1) ||
        process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *reason = QStringLiteral("the RIFE archive listing could not be read");
        return false;
    }
    QSet<QString> expected;
    for (const RifeModelPin& pin : pins) expected.insert(pin.fileName);
    QSet<QString> found;
    QSet<QString> seenPaths;
    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    bool itemsStarted = false;
    bool hasEntryProperties = false;
    QString currentPath;
    bool currentFolder = false;
    auto inspect = [&]() -> bool {
        if (currentPath.isEmpty() && hasEntryProperties) {
            *reason = QStringLiteral("the RIFE archive listing contains an entry without a path");
            return false;
        }
        if (currentPath.isEmpty()) return true;
        QString path = currentPath;
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (path.endsWith(QLatin1Char('/'))) path.chop(1);
        const bool directory = currentFolder || path == QStringLiteral("animejanai") ||
                               path == QStringLiteral("animejanai/rife");
        QString pathReason;
        if (safeRelativePath(path, &pathReason).isEmpty() || seenPaths.contains(path)) {
            *reason = QStringLiteral("the RIFE archive contains an unsafe or duplicate path: %1")
                          .arg(currentPath);
            return false;
        }
        seenPaths.insert(path);
        if (directory) {
            if (path != QStringLiteral("animejanai") &&
                path != QStringLiteral("animejanai/rife")) {
                *reason = QStringLiteral("unexpected RIFE archive directory: %1").arg(path);
                return false;
            }
            return true;
        }
        const QString prefix = QStringLiteral("animejanai/rife/");
        if (!path.startsWith(prefix)) {
            *reason = QStringLiteral("unexpected RIFE archive file: %1").arg(path);
            return false;
        }
        const QString name = path.mid(prefix.size());
        if (!expected.contains(name) || found.contains(name)) {
            *reason = QStringLiteral("unexpected or duplicate RIFE archive graph: %1").arg(path);
            return false;
        }
        found.insert(name);
        return true;
    };

    const QStringList lines = output.split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.startsWith(QStringLiteral("----------"))) {
            if (!inspect()) return false;
            currentPath.clear();
            currentFolder = false;
            hasEntryProperties = false;
            itemsStarted = true;
            continue;
        }
        if (!itemsStarted) continue;
        if (line.isEmpty()) {
            if (!inspect()) return false;
            currentPath.clear();
            currentFolder = false;
            hasEntryProperties = false;
            continue;
        }
        if (line.startsWith(QStringLiteral("Path = "))) {
            if (!inspect()) return false;
            currentPath = line.mid(7);
            currentFolder = false;
        } else {
            hasEntryProperties = true;
            if (line.startsWith(QStringLiteral("Symbolic Link = ")) ||
                line.startsWith(QStringLiteral("Hard Link = "))) {
                *reason = QStringLiteral("the RIFE archive contains a link entry");
                return false;
            }
            if (line == QStringLiteral("Folder = +")) currentFolder = true;
        }
    }
    if (!inspect()) return false;
    if (found != expected) {
        *reason = QStringLiteral("the RIFE archive does not contain the exact pinned graph inventory");
        return false;
    }
    return true;
}

}  // namespace

ComponentProvisionResult provisionTensorRtComponents(
    const ComponentStore& store, const QVector<ComponentPack>& packs, const QString& scratchDirectory,
    const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled,
    const VerifiedComponentArchive* reusedArchive) {
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

    const QString sevenZip = mikai::backend::ensureSevenZip(nativeToolsDirectory());
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
        const bool reuseThisPack = reusedArchive != nullptr &&
            !reusedArchive->packName.isEmpty() && reusedArchive->packName == pack.name;
        const QString archive = reuseThisPack ? reusedArchive->path : scratch.filePath(pack.asset);
        if (reuseThisPack) {
            const QFileInfo reused(archive);
            if (!reused.isFile() || !reused.isReadable()) {
                return fail(QStringLiteral("native.components.fetch"),
                            QStringLiteral("the verified component archive is not readable: %1")
                                .arg(archive));
            }
        } else {
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

        if (!reuseThisPack) QFile::remove(archive);
        QDir(extractDir).removeRecursively();
    }

    result.ok = true;
    return result;
}

ComponentProvisionResult provisionRifeArchive(
    const RifeArchiveStore& store, const QVector<RifeModelPin>& pins,
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
        QMutexLocker lock(&g_rifeRunMutex);
        if (g_rifeRunning) {
            result.reasonKey = QStringLiteral("native.components.busy");
            result.reason = translated("A RIFE archive download is already running.");
            return result;
        }
        g_rifeRunning = true;
    }
    const auto releaseRun = qScopeGuard([] {
        QMutexLocker lock(&g_rifeRunMutex);
        g_rifeRunning = false;
    });

    if (store.root.isEmpty() || pins.size() != 47) {
        return fail(QStringLiteral("native.components.install"),
                    translated("The pinned RIFE archive inventory is unavailable."));
    }
    const QString sevenZip = mikai::backend::ensureSevenZip(nativeToolsDirectory());
    if (sevenZip.isEmpty()) {
        return fail(QStringLiteral("native.components.extract"),
                    translated("7-Zip is not available to unpack the RIFE archive."));
    }
    const QString scratchRoot = nativeSubdirectory(QStringLiteral("scratch"));
    if (scratchRoot.isEmpty() || !QDir().mkpath(scratchRoot)) {
        return fail(QStringLiteral("native.components.install"),
                    translated("The RIFE archive scratch directory is not available."));
    }
    QTemporaryDir scratch(QDir(scratchRoot).filePath(QStringLiteral("rife-archive-XXXXXX")));
    if (!scratch.isValid()) {
        return fail(QStringLiteral("native.components.install"),
                    translated("A temporary RIFE archive directory could not be created."));
    }

    const RifeArchivePack& pack = rifeArchivePack();
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
    if (cancelled && cancelled()) {
        return fail(QStringLiteral("native.components.cancelled"),
                    translated("The RIFE archive download was cancelled."));
    }

    QString listingReason;
    if (!validateRifeArchiveListing(sevenZip, archive, pins, &listingReason)) {
        return fail(QStringLiteral("native.components.extract"), listingReason);
    }

    const QString extraction = scratch.filePath(QStringLiteral("extract"));
    if (!QDir().mkpath(extraction)) {
        return fail(QStringLiteral("native.components.extract"),
                    translated("The RIFE archive extraction directory could not be created."));
    }
    QString extractReason;
    if (!mikai::backend::extractArchive(sevenZip, archive, extraction, &extractReason)) {
        return fail(QStringLiteral("native.components.extract"), extractReason);
    }
    if (cancelled && cancelled()) {
        return fail(QStringLiteral("native.components.cancelled"),
                    translated("The RIFE archive installation was cancelled."));
    }

    QString installReason;
    if (!installRifeGraphs(store, pins, extraction, cancelled, &installReason)) {
        const bool wasCancelled = cancelled && cancelled();
        return fail(wasCancelled ? QStringLiteral("native.components.cancelled")
                                 : QStringLiteral("native.components.install"),
                    installReason);
    }
    result.ok = true;
    result.installedPacks.append(pack.name);
    return result;
}

}  // namespace mplayer::native
