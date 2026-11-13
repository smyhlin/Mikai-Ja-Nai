#include "AnimeJanaiRuntimeStore.hpp"

#include "backend/Cancellation.hpp"
#include "backend/Diagnostics.hpp"
#include "backend/Downloads.hpp"
#include "native/bundle/NativePaths.hpp"
#include "NativeGpuProbe.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirListing>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QVarLengthArray>
#include <QProcess>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>

namespace mplayer::native {
namespace {

class StoreText {
    Q_DECLARE_TR_FUNCTIONS(AnimeJanaiRuntimeStore)
};

constexpr auto kToolVersion = "v0.2.1";
#ifdef Q_OS_WIN
constexpr auto kPlatform = "windows-x64";
constexpr auto kToolName = "aji-runtime.exe";
constexpr auto kToolArchive = "aji-runtime-windows-x64.zip";
constexpr qint64 kToolArchiveBytes = 1282779;
constexpr auto kToolArchiveSha = "f51a6c00134ee74e818a759ec4bee14594fd8ccbf7a9e31de5eeb6919c2c4d0b";
constexpr qint64 kToolBytes = 2676224;
constexpr auto kToolSha = "9944dffde3d60ef74ae8978b2575a0992b139bfc14f01512da4a0fa626a4a9d8";
constexpr qint64 kToolLicenseBytes = 35823;
constexpr auto kToolLicenseSha = "230184f60bae2feaf244f10a8bac053c8ff33a183bcc365b4d8b876d2b7f4809";
constexpr qint64 kIndexBytes = 15242;
constexpr auto kIndexSha = "5b6f0450ba4eed34d6e25dde072ce1298f083d91d142fd858495483e7711aea1";
#else
constexpr auto kPlatform = "linux-x64";
constexpr auto kToolName = "aji-runtime";
constexpr auto kToolArchive = "aji-runtime-linux-x64.tar.gz";
constexpr qint64 kToolArchiveBytes = 1477673;
constexpr auto kToolArchiveSha = "d596c211dfecdb2574a165834ee25f4b8d7424b9003740f98e19850a619479d7";
constexpr qint64 kToolBytes = 3190512;
constexpr auto kToolSha = "48f82a07d0782387547b7d2375dacdc9434d61c9e1ac8387268a70774fb4eeb0";
constexpr qint64 kToolLicenseBytes = 35149;
constexpr auto kToolLicenseSha = "3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986";
constexpr qint64 kIndexBytes = 13028;
constexpr auto kIndexSha = "d441210f7747b255f17483f7c8b921817ad3f997abab5a1ba5d348b51511ab61";
#endif
QMutex g_toolMutex;
std::atomic<bool> g_relocating{false};

bool workCancelled(const std::function<bool()>& cancelled) {
    return (cancelled && cancelled()) ||
           mikai::backend::WorkCancellationScope::cancellationRequested();
}

std::unique_lock<QMutex> acquireToolLock(const std::function<bool()>& cancelled) {
    if (workCancelled(cancelled)) return {};
    while (!g_toolMutex.tryLock(200))
        if (workCancelled(cancelled)) return {};
    return std::unique_lock<QMutex>(g_toolMutex, std::adopt_lock);
}

QString normalizedPath(const QString& path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

bool samePath(const QString& left, const QString& right) {
    return normalizedPath(left).compare(normalizedPath(right), pathCaseSensitivity()) == 0;
}

QString sharedPhysicalRoot(const QString& root) {
    const QFileInfo info(root);
    // Qt's canonicalFilePath() does not resolve NTFS junctions.
    const QString target = info.isJunction() ? info.junctionTarget() :
                           info.isSymbolicLink() ? info.symLinkTarget() : info.absoluteFilePath();
    const NativePathTrust trust = nativePathTrust(target, true);
    if (!trust.trusted || trust.form != NativePathForm::Local) return {};
    const QString canonical = QFileInfo(target).canonicalFilePath();
    return normalizedPath(canonical.isEmpty() ? target : canonical);
}

bool sharedPathIsSafe(const QString& root, const QString& path) {
    if (root.isEmpty() || !pathWithinRoot(root, path)) return false;
    const QString physicalRoot = sharedPhysicalRoot(root);
    if (physicalRoot.isEmpty() || !QFileInfo(physicalRoot).isDir()) return false;
    // Only the store root may be the upstream-managed relocation link.
    const QString physicalPath = QDir(physicalRoot).filePath(QDir(root).relativeFilePath(path));
    return pathHasNoReparsePoints(QFileInfo(physicalRoot).absolutePath(), physicalPath);
}

std::optional<QJsonObject> readObject(const QString& path, QString* reason) {
    QByteArray bytes;
    QString error;
    if (readSharedMetadataFile(path, 256 * 1024, &bytes, &error) != SharedMetadataReadStatus::Ok) {
        if (reason) *reason = StoreText::tr("Cannot read runtime metadata %1: %2").arg(path, error);
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (!document.isObject()) {
        if (reason) *reason = StoreText::tr("Invalid runtime metadata %1: %2")
                                  .arg(path, parseError.errorString());
        return std::nullopt;
    }
    return document.object();
}

QVarLengthArray<const ComponentPack*, 2> selectedPacks(Backend backend, QString* reason) {
    if (backend == Backend::TensorRTRTX) {
#ifdef Q_OS_WIN
        return {tensorRtRtxRuntimePack()};
#else
        if (reason) *reason = StoreText::tr("TensorRT for RTX has no published Linux runtime.");
        return {};
#endif
    }
    if (backend != Backend::TensorRT) {
        if (reason) *reason = StoreText::tr("Shared runtime storage is used by TensorRT backends only.");
        return {};
    }
    const auto& gpu = observeComputeCapability();
    const ComponentPack* resource = tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
    if (!gpu.nvidia || resource == nullptr) {
        if (reason) *reason = StoreText::tr("TensorRT requires an NVIDIA GPU with compute capability 7.5 or newer.");
        return {};
    }
    return {tensorRtRuntimePack(), resource};
}

QString packDirectory(const QString& root, const ComponentPack& pack) {
    return QDir(root).filePath(QStringLiteral("packs/%1/%2").arg(pack.name, pack.version));
}

QString applicationReferencePath() {
#ifdef Q_OS_LINUX
    const QString appDir = qEnvironmentVariable("APPDIR");
    const QString image = qEnvironmentVariable("APPIMAGE");
    const NativePathTrust imageTrust = nativePathTrust(image, true);
    // AppImage mount paths vanish; garbage collection must retain the persistent image.
    if (!appDir.isEmpty() && imageTrust.trusted && imageTrust.form == NativePathForm::Local &&
        pathWithinRoot(appDir, QCoreApplication::applicationFilePath()) && QFileInfo(image).isFile())
        return normalizedPath(QFileInfo(image).canonicalFilePath());
#endif
    return normalizedPath(QCoreApplication::applicationFilePath());
}

QString appId(Backend backend) {
    static const QString identity = [] {
        QString path = applicationReferencePath();
        if (pathCaseSensitivity() == Qt::CaseInsensitive) path = path.toCaseFolded();
        return QString::fromLatin1(QCryptographicHash::hash(
            path.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    }();
    return QStringLiteral("mikai-%1-%2").arg(identity, backend == Backend::TensorRTRTX
                                                       ? QStringLiteral("trtrtx")
                                                       : QStringLiteral("trt"));
}

QString toolDirectory() {
    const QString tools = nativeSubdirectory(QStringLiteral("tools"));
    return nativeRoot().isEmpty() ? QString() :
        QDir(tools).filePath(QStringLiteral("aji-runtime/%1").arg(QString::fromLatin1(kToolVersion)));
}

QString toolPath() {
    const QString directory = toolDirectory();
    return directory.isEmpty() ? QString() : QDir(directory).filePath(QString::fromLatin1(kToolName));
}

QString indexPath() {
    const QString directory = toolDirectory();
    return directory.isEmpty() ? QString() :
        QDir(directory).filePath(QStringLiteral("runtime-2-%1.json").arg(QString::fromLatin1(kPlatform)));
}

bool verifiedOwnedFile(const QString& path, qint64 bytes, const char* digest,
                       const std::function<bool()>& cancelled) {
    const QFileInfo info(path);
    return !path.isEmpty() && info.isFile() && info.size() == bytes &&
           pathHasNoReparsePoints(QFileInfo(nativeRoot()).absolutePath(), path) &&
           hashFile(path, nullptr, cancelled) == QLatin1String(digest);
}

bool ensureTool(const std::function<void(const ComponentProvisionProgress&)>& progress,
                const std::function<bool()>& cancelled, QString* reason) {
    auto lock = acquireToolLock(cancelled);
    if (!lock) return false;
    if (workCancelled(cancelled)) return false;
    const QString directory = toolDirectory();
    if (directory.isEmpty()) {
        *reason = StoreText::tr("The app-owned runtime tools directory is unavailable.");
        return false;
    }
    if (verifiedOwnedFile(toolPath(), kToolBytes, kToolSha, cancelled) &&
        verifiedOwnedFile(QDir(directory).filePath(QStringLiteral("LICENSE")),
                          kToolLicenseBytes, kToolLicenseSha, cancelled)) return true;
    const QString parent = QFileInfo(directory).absolutePath();
    const QString scratchRoot = nativeSubdirectory(QStringLiteral("scratch"));
    if (!ensureOwnedNativeDirectory(parent, reason) || !ensureOwnedNativeDirectory(scratchRoot, reason)) return false;
    QTemporaryDir scratch(QDir(scratchRoot).filePath(QStringLiteral("aji-runtime-bootstrap-XXXXXX")));
    if (!scratch.isValid()) {
        *reason = scratch.errorString();
        return false;
    }
    const QString archive = scratch.filePath(QString::fromLatin1(kToolArchive));
    const QUrl url(QStringLiteral("https://github.com/the-database/aji-runtime/releases/download/%1/%2")
                       .arg(QString::fromLatin1(kToolVersion), QString::fromLatin1(kToolArchive)));
    if (!mikai::backend::downloadToFile(url, archive, kToolArchiveBytes,
            QString::fromLatin1(kToolArchiveSha), [&](qint64 got, qint64 total) {
                if (progress) progress({QStringLiteral("aji-runtime-v0.2.1"), got, total,
                                        ComponentProvisionStage::Downloading});
            }, cancelled, reason)) return false;
    QString sevenZip;
    try {
        sevenZip = mikai::backend::ensureSevenZip(nativeToolsDirectory());
    } catch (const std::exception& error) {
        *reason = QString::fromLocal8Bit(error.what());
        return false;
    }
    if (sevenZip.isEmpty()) {
        *reason = StoreText::tr("7-Zip is unavailable to unpack the runtime manager.");
        return false;
    }
    const QString extracted = scratch.filePath(QStringLiteral("unpacked"));
    if (!QDir().mkpath(extracted) ||
        !mikai::backend::extractArchive(sevenZip, archive, extracted, reason)) return false;
    const QString executable = QDir(extracted).filePath(QString::fromLatin1(kToolName));
    if (!verifiedOwnedFile(executable, kToolBytes, kToolSha, cancelled) ||
        !verifiedOwnedFile(QDir(extracted).filePath(QStringLiteral("LICENSE")),
                           kToolLicenseBytes, kToolLicenseSha, cancelled)) {
        *reason = StoreText::tr("The runtime manager does not match its verified executable and license inventory.");
        return false;
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(executable, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
            QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther)) {
        *reason = StoreText::tr("Cannot make the verified runtime manager executable.");
        return false;
    }
#endif
    if (workCancelled(cancelled)) return false;
    const QString previous = directory + QStringLiteral(".previous");
    if (QFileInfo::exists(previous)) {
        *reason = StoreText::tr("An unfinished runtime-manager replacement must be resolved first: %1").arg(previous);
        return false;
    }
    const bool replacing = QFileInfo::exists(directory);
    if (replacing && (!pathHasNoReparsePoints(QFileInfo(nativeRoot()).absolutePath(), directory) ||
                      !QDir().rename(directory, previous))) {
        *reason = StoreText::tr("Cannot preserve the previous runtime manager: %1").arg(directory);
        return false;
    }
    if (!QDir().rename(extracted, directory)) {
        if (replacing && !QDir().rename(previous, directory))
            qWarning().noquote() << StoreText::tr("Cannot restore the runtime manager from %1").arg(previous);
        *reason = StoreText::tr("Cannot publish the verified runtime manager: %1").arg(directory);
        return false;
    }
    if (replacing && !QDir(previous).removeRecursively())
        qWarning().noquote() << StoreText::tr("Cannot remove the previous runtime manager: %1").arg(previous);
    return true;
}

bool ensureIndex(const std::function<void(const ComponentProvisionProgress&)>& progress,
                 const std::function<bool()>& cancelled, QString* reason) {
    auto lock = acquireToolLock(cancelled);
    if (!lock) return false;
    const QString destination = indexPath();
    if (verifiedOwnedFile(destination, kIndexBytes, kIndexSha, cancelled)) return true;
    if (workCancelled(cancelled)) return false;
    const QString scratchRoot = nativeSubdirectory(QStringLiteral("scratch"));
    if (!ensureOwnedNativeDirectory(scratchRoot, reason)) return false;
    QTemporaryDir scratch(QDir(scratchRoot).filePath(QStringLiteral("aji-runtime-index-XXXXXX")));
    if (!scratch.isValid()) {
        *reason = scratch.errorString();
        return false;
    }
    const QString temporary = scratch.filePath(QStringLiteral("index.json"));
    const QUrl url(QStringLiteral("https://github.com/the-database/animejanai-inference/releases/download/runtime-2/packs-%1.json")
                       .arg(QString::fromLatin1(kPlatform)));
    if (!mikai::backend::downloadToFile(url, temporary, kIndexBytes, QString::fromLatin1(kIndexSha),
            [&](qint64 got, qint64 total) {
                if (progress) progress({QStringLiteral("aji-runtime-index"), got, total,
                                        ComponentProvisionStage::Downloading});
            }, cancelled, reason)) return false;
    const QString trust = QFileInfo(nativeRoot()).absolutePath();
    if (!pathHasNoReparsePoints(trust, QFileInfo(destination).absolutePath()) ||
        (QFileInfo::exists(destination) && !pathHasNoReparsePoints(trust, destination))) {
        *reason = StoreText::tr("The runtime index cache is redirected: %1").arg(destination);
        return false;
    }
    QFile source(temporary);
    QSaveFile output(destination);
    if (!source.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        *reason = StoreText::tr("Cannot publish the verified runtime index: %1").arg(destination);
        return false;
    }
    const QByteArray bytes = source.readAll();
    if (source.error() != QFile::NoError || output.write(bytes) != bytes.size() ||
        !commitMetadataFile(output, reason)) {
        if (reason->isEmpty()) *reason = output.errorString();
        return false;
    }
    return true;
}

struct ToolResult {
    bool ok = false;
    bool cancelled = false;
    QString output;
};

ToolResult runTool(QStringList arguments, const QString& root,
                   const std::function<void(const ComponentProvisionProgress&)>& progress,
                   const std::function<bool()>& cancelled) {
    const bool moving = arguments.first() == QLatin1String("move");
    arguments.append({QStringLiteral("--store"), root, QStringLiteral("--progress"), QStringLiteral("lines")});
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(toolPath(), arguments, QIODevice::ReadOnly);
    const bool started = moving ? process.waitForStarted(5000) :
        mikai::backend::waitForProcessStarted(process, 5000) ==
            mikai::backend::ProcessWaitResult::Finished;
    if (!started) return {false, !moving && workCancelled(cancelled), process.errorString()};
    QByteArray tail;
    QByteArray pending;
    bool done = false;
    const auto number = [](QByteArrayView value, qint64* result) {
        if (value.isEmpty()) return false;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), *result);
        return parsed.ec == std::errc() && parsed.ptr == value.data() + value.size();
    };
    const auto consume = [&] {
        const QByteArray chunk = process.readAllStandardOutput();
        tail.append(chunk);
        if (tail.size() > 64 * 1024) tail.remove(0, tail.size() - 64 * 1024);
        pending.append(chunk);
        qsizetype end;
        while ((end = pending.indexOf('\n')) >= 0) {
            const QByteArrayView line = QByteArrayView(pending).first(end).trimmed();
            std::array<QByteArrayView, 4> fields{};
            qsizetype start = 0;
            for (auto& field : fields) {
                const qsizetype space = line.indexOf(' ', start);
                field = line.sliced(start, space < 0 ? line.size() - start : space - start);
                if (space < 0) break;
                start = space + 1;
            }
            if (line == "AJI_DONE") done = true;
            if (progress) {
                qint64 got = 0, total = 0;
                const auto slash = fields[1].indexOf('/');
                const QByteArrayView pack = slash < 0 ? fields[1] : fields[1].first(slash);
                if (fields[0] == "AJI_DOWNLOAD" && number(fields[2], &got) && number(fields[3], &total))
                    progress({QString::fromUtf8(pack), got, total, ComponentProvisionStage::Downloading});
                else if (fields[0] == "AJI_EXTRACT")
                    progress({QString::fromUtf8(pack), 0, 0, ComponentProvisionStage::Extracting});
                else if (fields[0] == "AJI_PLAN" && fields[3] == "installed" && number(fields[2], &total))
                    progress({QString::fromUtf8(pack), total, total, ComponentProvisionStage::Installed});
                else if (fields[0] == "AJI_MOVE" && number(fields[1], &got) && number(fields[2], &total))
                    progress({QStringLiteral("aji-runtime-storage"), got, total, ComponentProvisionStage::Installing});
            }
            pending.remove(0, end + 1);
        }
        if (pending.size() > 64 * 1024) pending.remove(0, pending.size() - 64 * 1024);
    };
    QElapsedTimer deadline;
    deadline.start();
    while (process.state() != QProcess::NotRunning) {
        // Upstream move has no cancellation handshake around its directory relink.
        if (!moving && (workCancelled(cancelled) || deadline.hasExpired(60 * 60 * 1000))) {
            const bool wasCancelled = workCancelled(cancelled);
            process.kill();
            process.waitForFinished(5000);
            consume();
            return {false, wasCancelled, wasCancelled ? StoreText::tr("Runtime storage operation was cancelled.")
                                                       : StoreText::tr("The runtime manager exceeded its operation deadline.")};
        }
        process.waitForReadyRead(200);
        consume();
    }
    consume();
    const bool ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 && done;
    return {ok, false, ok ? QString() :
        mikai::backend::redactDiagnostic(QString::fromUtf8(tail).trimmed())};
}

ComponentProvisionResult failed(QString reason, bool cancelled = false) {
    return {false, cancelled, {}, cancelled ? QStringLiteral("animejanai.shared.cancelled")
                                          : QStringLiteral("animejanai.shared.failed"), std::move(reason)};
}

}  // namespace

QString animeJanaiSharedRuntimeStoreRoot(QString* reason) {
    QString root = qEnvironmentVariable("AJI_RUNTIME_STORE");
    if (root.isEmpty()) {
#ifdef Q_OS_WIN
        const QString base = qEnvironmentVariable("LOCALAPPDATA");
#else
        QString base = qEnvironmentVariable("XDG_DATA_HOME");
        if (base.isEmpty()) {
            const QString home = qEnvironmentVariable("HOME");
            if (!home.isEmpty()) base = QDir(home).filePath(QStringLiteral(".local/share"));
        }
#endif
        if (!base.isEmpty()) root = QDir(base).filePath(QStringLiteral("the-database/aji"));
    }
    if (!root.isEmpty()) root = QDir(root).absolutePath();
    const NativePathTrust trust = nativePathTrust(root, true);
    if (!trust.trusted || trust.form != NativePathForm::Local || QDir(root).isRoot()) {
        if (reason) *reason = StoreText::tr("Shared runtime storage requires an absolute local folder: %1")
                                  .arg(trust.reason);
        return {};
    }
    return normalizedPath(root);
}

bool animeJanaiRuntimeRelocationInProgress() {
    return g_relocating.load(std::memory_order_acquire);
}

AnimeJanaiRuntimeStorage describeAnimeJanaiRuntimeStorage() {
    AnimeJanaiRuntimeStorage result;
    result.root = animeJanaiSharedRuntimeStoreRoot(&result.reason);
    if (result.root.isEmpty()) return result;
    const QFileInfo info(result.root);
    result.moved = info.isSymbolicLink() || info.isJunction();
    result.location = sharedPhysicalRoot(result.root);
    if (result.location.isEmpty()) result.location = result.root;
    const QString marker = QDir(result.root).filePath(QStringLiteral("store.json"));
    if (!sharedPathIsSafe(result.root, marker)) {
        result.reason = StoreText::tr("Shared TensorRT runtime storage is not installed or its drive is unavailable.");
        return result;
    }
    const auto object = readObject(marker, &result.reason);
    result.available = object && object->value(QStringLiteral("format")).toDouble(-1) == 1;
    if (!result.available && result.reason.isEmpty())
        result.reason = StoreText::tr("The shared runtime store format is not supported.");
    return result;
}

StoredPack describeSharedAnimeJanaiRuntimePack(const QString& root, const ComponentPack& pack,
                                               bool rehash, const std::function<bool()>& cancelled) {
    StoredPack result;
    result.pack = pack.name;
    result.directory = packDirectory(root, pack);
    const auto reject = [&](QString reason) {
        result.reasonKey = QStringLiteral("animejanai.shared.packInvalid");
        result.reason = std::move(reason);
    };
    const QString metadata = QDir(result.directory).filePath(QStringLiteral(".pack.json"));
    if (root.isEmpty() || pack.filePins.isEmpty() || pack.filePins.size() > 64 ||
        !sharedPathIsSafe(root, metadata)) {
        reject(StoreText::tr("Shared runtime pack is missing or redirected: %1").arg(pack.name));
        return result;
    }
    const auto object = readObject(metadata, &result.reason);
    const QString kind = pack.name == QLatin1String("trt-runtime") ? QStringLiteral("trt") :
                         pack.name == QLatin1String("trtrtx-runtime") ? QStringLiteral("trtrtx") :
                                                                    QStringLiteral("trt-builder");
    if (!object || object->value(QStringLiteral("format")).toDouble(-1) != 1 ||
        object->value(QStringLiteral("name")).toString() != pack.name ||
        object->value(QStringLiteral("version")).toString() != pack.version ||
        object->value(QStringLiteral("kind")).toString() != kind ||
        object->value(QStringLiteral("platform")).toString() != QLatin1String(kPlatform) ||
        !object->value(QStringLiteral("files")).isArray()) {
        reject(StoreText::tr("Shared runtime pack metadata does not match the pinned index: %1").arg(pack.name));
        return result;
    }
    const QJsonArray files = object->value(QStringLiteral("files")).toArray();
    qsizetype entryCount = 0;
    for ([[maybe_unused]] const auto& entry :
         QDirListing(result.directory, QDirListing::IteratorFlag::IncludeHidden)) {
        if (++entryCount > pack.filePins.size() + 1) break;
    }
    if (files.size() != pack.filePins.size() || entryCount != pack.filePins.size() + 1) {
        reject(StoreText::tr("Shared runtime pack has an unexpected file inventory: %1").arg(pack.name));
        return result;
    }
    quint64 seen = 0;
    for (const QJsonValue& value : files) {
        const QJsonObject recorded = value.toObject();
        const QString path = recorded.value(QStringLiteral("path")).toString();
        qsizetype index = 0;
        while (index < pack.filePins.size() && pack.filePins[index].path != path) ++index;
        if (index == pack.filePins.size() || (seen & (quint64(1) << index))) {
            reject(StoreText::tr("Shared runtime pack has an unknown or repeated file: %1").arg(path));
            return result;
        }
        seen |= quint64(1) << index;
        const ComponentFilePin& pin = pack.filePins[index];
        const QString file = QDir(result.directory).filePath(pin.path);
        const QFileInfo info(file);
        if (recorded.value(QStringLiteral("bytes")).toDouble(-1) != double(pin.bytes) ||
            recorded.value(QStringLiteral("sha256")).toString() != pin.sha256 ||
            !info.isFile() || info.size() != pin.bytes || !sharedPathIsSafe(root, file)) {
            reject(StoreText::tr("Shared runtime file does not match the pinned index: %1").arg(file));
            return result;
        }
        if (workCancelled(cancelled)) {
            reject(StoreText::tr("Runtime verification was cancelled."));
            return result;
        }
#ifndef Q_OS_WIN
        if (pin.path == QLatin1String("trtexec") && !info.isExecutable()) {
            reject(StoreText::tr("The shared TensorRT trtexec is not executable: %1").arg(file));
            return result;
        }
#endif
        if (rehash) {
            qint64 bytes = 0;
            QString error;
            const QString digest = hashFile(file, &bytes, [&] { return workCancelled(cancelled); }, &error);
            if (digest != pin.sha256 || bytes != pin.bytes) {
                reject(StoreText::tr("Shared runtime file failed SHA-256 verification: %1. %2").arg(file, error));
                return result;
            }
        }
        result.bytes += pin.bytes;
    }
    result.installed = true;
    return result;
}

bool animeJanaiSharedRuntimeRefCurrent(const QString& root, Backend backend, QString* reason) {
    const auto packs = selectedPacks(backend, reason);
    if (root.isEmpty() || packs.isEmpty()) return false;
    const QString file = QDir(root).filePath(QStringLiteral("refs/%1.json").arg(appId(backend)));
    if (!sharedPathIsSafe(root, file)) {
        if (reason) *reason = StoreText::tr("The shared runtime has no live reference for this application.");
        return false;
    }
    const auto object = readObject(file, reason);
    if (!object || object->value(QStringLiteral("app_id")).toString() != appId(backend) ||
        !samePath(object->value(QStringLiteral("app_path")).toString(), applicationReferencePath()) ||
        !object->value(QStringLiteral("packs")).isArray()) {
        if (reason) *reason = StoreText::tr("The shared runtime reference is missing or out of date for this application.");
        return false;
    }
    QStringList expected, actual;
    for (const ComponentPack* pack : packs) expected.append(pack->name + QLatin1Char('/') + pack->version);
    for (const QJsonValue& value : object->value(QStringLiteral("packs")).toArray()) actual.append(value.toString());
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    if (actual != expected) {
        if (reason) *reason = StoreText::tr("The shared runtime reference does not retain the required backend packs.");
        return false;
    }
    return true;
}

AnimeJanaiSharedRuntime describeAnimeJanaiSharedRuntime(Backend backend, const QString& ajiDirectory,
                                                      bool rehash,
                                                      const std::function<bool()>& cancelled) {
    AnimeJanaiSharedRuntime result;
    result.root = animeJanaiSharedRuntimeStoreRoot(&result.reason);
    const auto packs = selectedPacks(backend, &result.reason);
    if (result.root.isEmpty() || packs.isEmpty() ||
        !animeJanaiSharedRuntimeRefCurrent(result.root, backend, &result.reason)) return result;
    const QString mappingPath = QDir(ajiDirectory).filePath(QStringLiteral("aji-runtime.json"));
    const QString overridePath = qEnvironmentVariable("AJI_RUNTIME_JSON");
    if (!overridePath.isEmpty()) {
        result.reason = StoreText::tr("AJI_RUNTIME_JSON overrides the managed backend mapping. Unset it before using managed TensorRT.");
        return result;
    }
    if (!pathHasNoReparsePoints(QFileInfo(nativeRoot()).absolutePath(), mappingPath)) {
        result.reason = StoreText::tr("The app-owned shared runtime mapping is missing or redirected.");
        return result;
    }
    const auto mapping = readObject(mappingPath, &result.reason);
    const bool rtx = backend == Backend::TensorRTRTX;
    if (!mapping || mapping->value(QStringLiteral("format")).toDouble(-1) != 1 ||
        mapping->size() != (rtx ? 2 : 3) ||
        !samePath(mapping->value(rtx ? QStringLiteral("trtrtx") : QStringLiteral("trt")).toString(),
                  packDirectory(result.root, *packs.first())) ||
        (!rtx && (!mapping->value(QStringLiteral("trt_builder")).isArray() ||
                  mapping->value(QStringLiteral("trt_builder")).toArray().size() != 1 ||
                  !samePath(mapping->value(QStringLiteral("trt_builder")).toArray().first().toString(),
                            packDirectory(result.root, *packs.last()))))) {
        result.reason = StoreText::tr("The shared runtime mapping does not match the pinned backend packs.");
        return result;
    }
    for (const ComponentPack* pack : packs) {
        const StoredPack stored = describeSharedAnimeJanaiRuntimePack(result.root, *pack, rehash, cancelled);
        if (!stored.installed) {
            result.reason = stored.reason;
            return result;
        }
        result.libraryDirs.append(stored.directory);
    }
    if (!rtx) result.trtexecPath = QDir(result.libraryDirs.first()).filePath(
#ifdef Q_OS_WIN
        QStringLiteral("trtexec.exe"));
#else
        QStringLiteral("trtexec"));
#endif
    result.installed = true;
    return result;
}

QJsonArray animeJanaiSharedRuntimeAcquisitions(Backend backend) {
    QJsonArray result;
    const auto packs = selectedPacks(backend, nullptr);
    if (packs.isEmpty()) return result;
    const auto source = [](const QString& name, const QString& url, qint64 bytes,
                           const QString& digest) {
        return QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("url"), url},
                           {QStringLiteral("bytes"), bytes}, {QStringLiteral("sha256"), digest}};
    };
    result.append(source(QStringLiteral("aji-runtime-v0.2.1"),
        QStringLiteral("https://github.com/the-database/aji-runtime/releases/download/%1/%2")
            .arg(QString::fromLatin1(kToolVersion), QString::fromLatin1(kToolArchive)),
        kToolArchiveBytes, QString::fromLatin1(kToolArchiveSha)));
    result.append(source(QStringLiteral("aji-runtime-index"),
        QStringLiteral("https://github.com/the-database/animejanai-inference/releases/download/runtime-2/packs-%1.json")
            .arg(QString::fromLatin1(kPlatform)),
        kIndexBytes, QString::fromLatin1(kIndexSha)));
    for (const ComponentPack* pack : packs)
        result.append(source(pack->name, pack->url, pack->assetBytes, pack->assetSha256));
    return result;
}

QVector<AnimeJanaiMissingDownload> animeJanaiSharedRuntimeMissingDownloads(Backend backend) {
    QVector<AnimeJanaiMissingDownload> result;
    const auto packs = selectedPacks(backend, nullptr);
    if (packs.isEmpty()) return result;
    result.reserve(packs.size() + 2);
    const QString root = animeJanaiSharedRuntimeStoreRoot();
    if (QFileInfo(toolPath()).size() != kToolBytes ||
        QFileInfo(QDir(toolDirectory()).filePath(QStringLiteral("LICENSE"))).size() != kToolLicenseBytes)
        result.append({QStringLiteral("aji-runtime-v0.2.1"), kToolArchiveBytes});
    if (QFileInfo(indexPath()).size() != kIndexBytes)
        result.append({QStringLiteral("aji-runtime-index"), kIndexBytes});
    for (const ComponentPack* pack : packs)
        if (!describeSharedAnimeJanaiRuntimePack(root, *pack).installed)
            result.append({pack->name, pack->assetBytes});
    return result;
}

ComponentProvisionResult provisionAnimeJanaiSharedRuntime(
    Backend backend, const QString& ajiDirectory,
    const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled) {
    QString error;
    const QString root = animeJanaiSharedRuntimeStoreRoot(&error);
    const auto packs = selectedPacks(backend, &error);
    if (root.isEmpty() || packs.isEmpty()) return failed(error);
    const auto stopped = [&] { return workCancelled(cancelled); };
    if (stopped()) return failed(StoreText::tr("Runtime storage operation was cancelled."), true);
    if (!ensureTool(progress, stopped, &error) || !ensureIndex(progress, stopped, &error))
        return failed(error.isEmpty() ? StoreText::tr("Runtime storage operation was cancelled.") : error, stopped());
    QStringList arguments{QStringLiteral("ensure"), QStringLiteral("--app-id"), appId(backend),
        QStringLiteral("--app-path"), applicationReferencePath(),
        QStringLiteral("--app-version"), QCoreApplication::applicationVersion(),
        QStringLiteral("--index"), indexPath(), QStringLiteral("--index-sha256"), QString::fromLatin1(kIndexSha),
        QStringLiteral("--want"), backend == Backend::TensorRTRTX ? QStringLiteral("trtrtx") : QStringLiteral("trt"),
        QStringLiteral("--gpus")};
    const auto& gpu = observeComputeCapability();
    arguments.append(QStringLiteral("sm%1%2").arg(gpu.smMajor).arg(gpu.smMinor));
    if (!ajiDirectory.isEmpty()) arguments.append({QStringLiteral("--aji-dir"), ajiDirectory});
    const ToolResult tool = runTool(arguments, root, progress, stopped);
    if (!tool.ok) return failed(StoreText::tr("The runtime manager could not retain the backend packs: %1").arg(tool.output), tool.cancelled);
    if (!animeJanaiSharedRuntimeRefCurrent(root, backend, &error)) return failed(error);
    ComponentProvisionResult result;
    for (const ComponentPack* pack : packs) {
        const StoredPack stored = describeSharedAnimeJanaiRuntimePack(root, *pack);
        if (!stored.installed) return failed(stored.reason);
        result.installedPacks.append(pack->name);
        if (progress) progress({pack->name, pack->assetBytes, pack->assetBytes, ComponentProvisionStage::Installed});
    }
    if (!ajiDirectory.isEmpty() && !describeAnimeJanaiSharedRuntime(backend, ajiDirectory).installed)
        return failed(StoreText::tr("The runtime manager did not publish a valid backend mapping."));
    result.ok = true;
    return result;
}

ComponentProvisionResult relocateAnimeJanaiSharedRuntime(
    const QString& destination, bool restoreDefault,
    const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled) {
    bool expected = false;
    if (!g_relocating.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return failed(StoreText::tr("A shared runtime relocation is already running."));
    const auto release = qScopeGuard([] { g_relocating.store(false, std::memory_order_release); });
    QString error;
    const auto location = describeAnimeJanaiRuntimeStorage();
    if (!location.available) return failed(location.reason);
    if (!restoreDefault) {
        const NativePathTrust trust = nativePathTrust(destination, true);
        if (!trust.trusted || trust.form != NativePathForm::Local || QDir(destination).isRoot())
            return failed(StoreText::tr("Choose an absolute local folder for shared runtime storage: %1").arg(trust.reason));
    }
    const auto stopped = [&] { return workCancelled(cancelled); };
    if (!ensureTool(progress, stopped, &error))
        return failed(error.isEmpty() ? StoreText::tr("Runtime storage operation was cancelled.") : error, stopped());
    if (stopped()) return failed(StoreText::tr("Runtime storage operation was cancelled."), true);
    const ToolResult tool = runTool({QStringLiteral("move"), restoreDefault ? QStringLiteral("--default")
                                                                         : normalizedPath(destination)},
                                    location.root, progress, stopped);
    if (!tool.ok) return failed(StoreText::tr("The runtime store could not be moved: %1").arg(tool.output), tool.cancelled);
    const auto moved = describeAnimeJanaiRuntimeStorage();
    if (!moved.available || (restoreDefault && moved.moved))
        return failed(StoreText::tr("The runtime manager finished without a usable storage location.") +
                      (moved.reason.isEmpty() ? QString{} : QLatin1Char(' ') + moved.reason));
    ComponentProvisionResult result;
    result.ok = true;
    return result;
}

}  // namespace mplayer::native
