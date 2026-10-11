#include "AnimeJanaiRuntime.hpp"

#include "backend/Downloads.hpp"
#include "native/bundle/NativePaths.hpp"
#include "native/components/NativeComponentCatalog.hpp"
#include "native/components/NativeComponentStore.hpp"
#include "native/components/NativeComponentProvisioning.hpp"
#include "native/components/NativeGpuProbe.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QProcess>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QScopeGuard>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrl>
#include <QtCore/QMap>
#include <QtCore/QtEndian>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <algorithm>
#include <exception>

namespace mplayer::native {
namespace {

constexpr auto kAjiBytes = qint64(657798);
constexpr qint64 kOrtBytes = 12458649;
constexpr qint64 kDirectMlBytes = 202292617;
constexpr auto kAjiSha = "22828c41439ac2b165a18294ba3e15257e8df617ab1d7ab6fb0f96e3915c5586";
constexpr auto kAjiUrl = "https://github.com/the-database/animejanai-inference/releases/download/v0.9.0/aji-windows-x64.zip";
constexpr auto kAjiSchema = "mikai.native.animejanai/2";
constexpr auto kAjiVersion = "v0.9.0";
QMutex g_installMutex;
bool g_installRunning = false;

QString backendName(Backend backend);

QString installationRoot(Backend backend) {
    const QString components = nativeSubdirectory(QStringLiteral("components"));
    return components.isEmpty() ? QString{} :
        QDir(components).filePath(QStringLiteral("aji/v0.9.0/") + backendName(backend));
}
QString translated(const char* text) {
    return QCoreApplication::translate("AnimeJanaiInstallation", text);
}
QString backendName(Backend backend) {
    return backend == Backend::TensorRT ? QStringLiteral("trt") : QStringLiteral("dml");
}
QString backendDll(Backend backend) {
    return backend == Backend::TensorRT ? QStringLiteral("aji_trt.dll") : QStringLiteral("aji_dml.dll");
}
const ComponentPack* selectedTensorRtResource() {
    const GpuComputeCapability& capability = observeComputeCapability();
    if (!capability.nvidia) return nullptr;
    return tensorRtResourcePackFor(capability.smMajor, capability.smMinor);
}

const ComponentPack* resourceInDirectory(const QString& directory, QString* error) {
    const QVector<ComponentPack>& catalog = tensorRtComponentCatalog();
    const ComponentPack* found = nullptr;
    for (const ComponentPack& pack : catalog) {
        if (pack.resourceFile.isEmpty()) continue;
        const QFileInfo candidate(QDir(directory).filePath(pack.resourceFile));
        if (!candidate.exists()) continue;
        if (found != nullptr) {
            *error = QStringLiteral("The TensorRT source contains more than one builder resource.");
            return nullptr;
        }
        found = &pack;
    }
    if (found == nullptr) *error = QStringLiteral("The TensorRT source is missing its selected builder resource.");
    return found;
}

QStringList closureFiles(Backend backend, const ComponentPack* resource = nullptr) {
    QStringList files{QStringLiteral("aji.dll"), backendDll(backend)};
    if (backend == Backend::TensorRT) {
        files << QStringLiteral("cudart64_13.dll") << QStringLiteral("nvinfer_11.dll")
              << QStringLiteral("nvinfer_plugin_11.dll") << QStringLiteral("nvonnxparser_11.dll");
        if (resource != nullptr) files << resource->resourceFile;
        files << QStringLiteral("trtexec.exe") << QStringLiteral("CUDA_LICENSE.txt")
              << QStringLiteral("DirectML_LICENSE.txt");
    } else {
        files << QStringLiteral("onnxruntime.dll") << QStringLiteral("DirectML.dll")
              << QStringLiteral("DirectML_LICENSE.txt");
    }
    return files;
}

QStringList optionalNoticeFiles(Backend backend) {
    if (backend == Backend::TensorRT)
        return {QStringLiteral("THIRD_PARTY_NOTICES.txt"), QStringLiteral("TensorRT_Acknowledgements.txt")};
    return {QStringLiteral("ORT_LICENSE.txt"), QStringLiteral("ORT_ThirdPartyNotices.txt"),
            QStringLiteral("DirectML_LICENSE_CODE.txt"), QStringLiteral("DirectML_ThirdPartyNotices.txt"),
            QStringLiteral("THIRD_PARTY_NOTICES.txt")};
}

QStringList recordedClosureFiles(const QString& directory, Backend backend, const ComponentPack* resource) {
    QStringList files = closureFiles(backend, resource);
    for (const QString& notice : optionalNoticeFiles(backend)) {
        const QFileInfo info(QDir(directory).filePath(notice));
        if (info.isFile() && !info.isSymLink()) files.append(notice);
    }
    return files;
}

QString sourceRuntimeDirectory(const QString& source) {
    const QStringList candidates{
        QDir(source).filePath(QStringLiteral("animejanai/inference")),
        QDir(source).filePath(QStringLiteral("inference"))};
    for (const QString& candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isDir() && !info.isSymLink()) return candidate;
    }
    return source;
}
QString hashFile(const QString& path, qint64* bytes = nullptr) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total = 0;
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    while (!f.atEnd()) {
        const qint64 n = f.read(buffer.data(), buffer.size());
        if (n <= 0) return {};
        hash.addData(QByteArrayView(buffer.constData(), n));
        total += n;
    }
    if (bytes) *bytes = total;
    return QString::fromLatin1(hash.result().toHex());
}
bool validPeX64(const QString& path) {
#ifdef Q_OS_WIN
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray dos = f.read(64);
    if (dos.size() != 64 || dos.left(2) != QByteArrayLiteral("MZ")) return false;
    const quint32 offset = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(dos.constData() + 0x3c));
    if (offset > 16 * 1024 * 1024 || !f.seek(offset)) return false;
    const QByteArray header = f.read(6);
    if (header.size() != 6 || header.left(4) != QByteArray("PE\0\0", 4) ||
        qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(header.constData() + 4)) != IMAGE_FILE_MACHINE_AMD64 ||
        !f.seek(static_cast<qint64>(offset) + 24)) return false;
    const QByteArray optionalMagic = f.read(2);
    return optionalMagic.size() == 2 &&
           qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(optionalMagic.constData())) == 0x20b;
#else
    Q_UNUSED(path);
    return false;
#endif
}
bool noReparse(const QString& root, const QString& path) {
    const QFileInfo info(path);
    return info.exists() && !info.isSymLink() && pathWithinRoot(root, info.absoluteFilePath()) &&
           pathHasNoReparsePoints(root, info.absoluteFilePath());
}
QString officialAjiDigest(const QString& name) {
    if (name == QLatin1String("aji.dll"))
        return QStringLiteral("5184e162f61b262c9c50a43ecec233d6797e7a2e0434e1d41c83e8d5065f2164");
    if (name == QLatin1String("aji_trt.dll"))
        return QStringLiteral("0340fa0b5b83b98ada8c351beea20f5ef68fcce31b5b263a3b1bdb8e404f27ea");
    if (name == QLatin1String("aji_dml.dll"))
        return QStringLiteral("ece31c721cf92fbdd571fcc8d89b9378de362e99e18f8146cbf403863b919ed2");
    return {};
}
bool matchesPinnedOfficialAbi(const QString& directory, Backend backend) {
    const QString dispatcher = hashFile(QDir(directory).filePath(QStringLiteral("aji.dll")));
    const QString selected = hashFile(QDir(directory).filePath(backendDll(backend)));
    return dispatcher == QLatin1String("5184e162f61b262c9c50a43ecec233d6797e7a2e0434e1d41c83e8d5065f2164") &&
           selected == (backend == Backend::TensorRT
               ? QLatin1String("0340fa0b5b83b98ada8c351beea20f5ef68fcce31b5b263a3b1bdb8e404f27ea")
               : QLatin1String("ece31c721cf92fbdd571fcc8d89b9378de362e99e18f8146cbf403863b919ed2"));
}

bool runtimeExports(const QString& path, int* abi, bool* heavy) {
#ifdef Q_OS_WIN
    const QString parent = QFileInfo(path).absolutePath();
    DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(reinterpret_cast<LPCWSTR>(parent.utf16()));
    HMODULE module = LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS |
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) { if (cookie) RemoveDllDirectory(cookie); return false; }
    using VersionFn = int (*)();
    using HeavyFn = int (*)();
    auto version = reinterpret_cast<VersionFn>(GetProcAddress(module, "aji_api_version"));
    auto heavyFn = reinterpret_cast<HeavyFn>(GetProcAddress(module, "aji_rife_heavy_supported"));
    const bool ok = version != nullptr;
    if (ok) { *abi = version(); *heavy = heavyFn != nullptr && heavyFn() != 0; }
    FreeLibrary(module);
    if (cookie) RemoveDllDirectory(cookie);
    return ok;
#else
    Q_UNUSED(path); Q_UNUSED(abi); Q_UNUSED(heavy);
    return false;
#endif
}

bool writeInstallRecord(const QString& directory, Backend backend, const QJsonArray& files,
                        const QString& provenance, bool heavy, const QJsonArray& acquisitions,
                        QString* error) {
    QJsonObject record;
    record.insert(QStringLiteral("schema"), QString::fromLatin1(kAjiSchema));
    record.insert(QStringLiteral("version"), QString::fromLatin1(kAjiVersion));
    record.insert(QStringLiteral("backend"), backendName(backend));
    record.insert(QStringLiteral("provenance"), provenance);
    record.insert(QStringLiteral("archiveBytes"), provenance == QStringLiteral("official-download") ? static_cast<qint64>(kAjiBytes) : qint64(0));
    record.insert(QStringLiteral("archiveSha256"), provenance == QStringLiteral("official-download") ? QString::fromLatin1(kAjiSha) : QString{});
    record.insert(QStringLiteral("abi"), 8);
    record.insert(QStringLiteral("heavyRifeSupported"), heavy);
    record.insert(QStringLiteral("files"), files);
    record.insert(QStringLiteral("acquisitions"), acquisitions);
    QSaveFile out(QDir(directory).filePath(QStringLiteral("install.json")));
    const QByteArray payload = QJsonDocument(record).toJson(QJsonDocument::Compact);
    if (!out.open(QIODevice::WriteOnly) || out.write(payload) != payload.size() || !out.commit()) {
        *error = out.errorString(); return false;
    }
    return true;
}

bool publishStage(const QString& stage, Backend backend, QString* error) {
    const QString target = installationRoot(backend);
    const QString parent = QFileInfo(target).absolutePath();
    if (!pathWithinRoot(nativeRoot(), parent) || !QDir().mkpath(parent) ||
        !pathHasNoReparsePoints(nativeRoot(), parent)) {
        *error = QStringLiteral("the app-owned runtime directory could not be created safely"); return false;
    }
    if (QFileInfo::exists(target) && (!QFileInfo(target).isDir() ||
        !pathHasNoReparsePoints(nativeRoot(), target))) {
        *error = QStringLiteral("the previous runtime directory is not a safe app-owned directory"); return false;
    }
    const QString backup = target + QStringLiteral(".previous");
    if (QFileInfo::exists(backup) && (!QFileInfo(backup).isDir() ||
        !pathHasNoReparsePoints(nativeRoot(), backup))) {
        *error = QStringLiteral("the previous runtime backup is not a safe app-owned directory"); return false;
    }
    QDir(backup).removeRecursively();
    const bool hadTarget = QFileInfo::exists(target);
    if (hadTarget && !QDir().rename(target, backup)) { *error = QStringLiteral("the previous runtime could not be preserved"); return false; }
    if (!QDir().rename(stage, target)) {
        if (hadTarget) QDir().rename(backup, target);
        *error = QStringLiteral("the verified runtime could not be atomically published"); return false;
    }
    if (hadTarget) QDir(backup).removeRecursively();
    return true;
}

ComponentProvisionResult failed(const QString& key, const QString& reason, bool cancelled = false) {
    ComponentProvisionResult r; r.reasonKey = key; r.reason = reason; r.cancelled = cancelled; return r;
}

QString processFailureDetails(QProcess& process) {
    return QStringLiteral("process error: %1; exit code: %2; stderr: %3")
        .arg(process.errorString())
        .arg(process.exitCode())
        .arg(QString::fromLocal8Bit(process.readAllStandardError()).trimmed().right(500));
}

bool validateSafeArchive(const QString& sevenZip, const QString& archive, QString* error) {
    QProcess process;
    process.start(sevenZip, {QStringLiteral("l"), QStringLiteral("-slt"), archive});
    if (!process.waitForStarted() || !process.waitForFinished(-1) ||
        process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *error = QStringLiteral("The pinned package archive listing could not be read (%1).")
                     .arg(processFailureDetails(process));
        return false;
    }
    QSet<QString> seen;
    QString path;
    bool active = false;
    bool entry = false;
    bool folder = false;
    auto inspect = [&] {
        if (!active || !entry) return true;
        QString normalizedPath = path;
        normalizedPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (folder && normalizedPath.endsWith(QLatin1Char('/'))) normalizedPath.chop(1);
        QString safeReason;
        const QString normalized = safeRelativePath(normalizedPath, &safeReason);
        if (normalized.isEmpty() || normalized != normalizedPath) {
            *error = QStringLiteral("The pinned package archive contains an unsafe path: %1 (%2)")
                         .arg(path, safeReason.isEmpty() ? QStringLiteral("path is not canonical") : safeReason);
            return false;
        }
        const QString duplicateKey = normalizedPath.toCaseFolded();
        if (seen.contains(duplicateKey)) {
            *error = QStringLiteral("The pinned package archive contains a duplicate path: %1").arg(path);
            return false;
        }
        seen.insert(duplicateKey);
        return true;
    };
    for (QString line : QString::fromLocal8Bit(process.readAllStandardOutput()).split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.startsWith(QStringLiteral("----------"))) {
            if (!inspect()) return false;
            active = true; entry = false; folder = false; path.clear();
            continue;
        }
        if (!active) continue;
        if (line.isEmpty()) { if (!inspect()) return false; entry = false; folder = false; path.clear(); continue; }
        if (line.startsWith(QStringLiteral("Path = "))) { path = line.mid(7); entry = true; }
        else if (line == QStringLiteral("Folder = +")) folder = true;
        else if (line.startsWith(QStringLiteral("Symbolic Link = ")) ||
                 line.startsWith(QStringLiteral("Hard Link = "))) {
            *error = QStringLiteral("The pinned package archive contains a link entry.");
            return false;
        } else entry = true;
    }
    if (!inspect()) return false;
    return !seen.isEmpty();
}

bool validateArchiveListing(const QString& sevenZip, const QString& archive, const QString& prefix,
                            const QSet<QString>& expectedFiles, int expectedFileCount,
                            QString* error) {
    QProcess process;
    process.start(sevenZip, {QStringLiteral("l"), QStringLiteral("-slt"), archive});
    if (!process.waitForStarted() || !process.waitForFinished(-1) ||
        process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *error = QStringLiteral("The verified runtime archive listing could not be read (%1).")
                     .arg(processFailureDetails(process));
        return false;
    }
    QSet<QString> seen;
    QSet<QString> foundFiles;
    QString path;
    bool folder = false;
    bool hasProps = false;
    bool active = false;
    auto inspect = [&] {
        if (!active) return true;
        if (path.isEmpty()) return !hasProps;
        QString entryPath = path;
        entryPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (folder && entryPath.endsWith(QLatin1Char('/'))) entryPath.chop(1);
        QString safeReason;
        const QString normalized = safeRelativePath(entryPath, &safeReason);
        if (normalized.isEmpty() || normalized != entryPath) {
            *error = QStringLiteral("The verified runtime archive contains an unsafe path: %1 (%2)")
                         .arg(path, safeReason.isEmpty() ? QStringLiteral("path is not canonical") : safeReason);
            return false;
        }
        const QString duplicateKey = entryPath.toCaseFolded();
        if (seen.contains(duplicateKey)) {
            *error = QStringLiteral("The verified runtime archive contains a duplicate path: %1").arg(path);
            return false;
        }
        seen.insert(duplicateKey);
        if (folder) {
            QString parent = prefix;
            if (parent.endsWith(QLatin1Char('/'))) parent.chop(1);
            const int slash = parent.lastIndexOf(QLatin1Char('/'));
            const QString grandparent = slash < 0 ? QString{} : parent.left(slash);
            if (entryPath != parent && entryPath != grandparent) {
                *error = QStringLiteral("The verified runtime archive contains an unexpected directory: %1").arg(path);
                return false;
            }
            return true;
        }
        if (!entryPath.startsWith(prefix)) {
            *error = QStringLiteral("The verified runtime archive contains an unexpected file path: %1").arg(path);
            return false;
        }
        const QString name = entryPath.mid(prefix.size());
        if (name.contains(QLatin1Char('/')) || name.isEmpty()) {
            *error = QStringLiteral("The verified runtime archive contains a nested file path: %1").arg(path);
            return false;
        }
        if (expectedFiles.contains(name)) foundFiles.insert(name);
        else if (expectedFileCount < 0) foundFiles.insert(name);
        else {
            *error = QStringLiteral("The verified runtime archive contains an unexpected file: %1").arg(path);
            return false;
        }
        return true;
    };
    for (QString line : QString::fromLocal8Bit(process.readAllStandardOutput()).split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.startsWith(QStringLiteral("----------"))) {
            if (!inspect()) return false;
            active = true; path.clear(); folder = false; hasProps = false;
            continue;
        }
        if (!active) continue;
        if (line.isEmpty()) { if (!inspect()) return false; path.clear(); folder = false; hasProps = false; continue; }
        if (line.startsWith(QStringLiteral("Path = "))) { path = line.mid(7); hasProps = true; }
        else if (line.startsWith(QStringLiteral("Folder = +"))) { folder = true; hasProps = true; }
        else if (line.startsWith(QStringLiteral("Symbolic Link = ")) ||
                 line.startsWith(QStringLiteral("Hard Link = "))) {
            *error = QStringLiteral("The verified runtime archive contains a link entry.");
            return false;
        } else hasProps = true;
    }
    if (!inspect()) return false;
    if (expectedFileCount >= 0 && foundFiles.size() != expectedFileCount) {
        *error = QStringLiteral("The verified runtime archive does not contain the exact expected file inventory.");
        return false;
    }
    for (const QString& name : expectedFiles) {
        if (!foundFiles.contains(name)) {
            *error = QStringLiteral("The verified runtime archive is missing %1.").arg(name);
            return false;
        }
    }
    return true;
}

QJsonObject acquisition(const QString& name, const QString& url, qint64 bytes, const QString& sha) {
    QJsonObject item;
    item.insert(QStringLiteral("name"), name);
    item.insert(QStringLiteral("url"), url);
    item.insert(QStringLiteral("bytes"), bytes);
    item.insert(QStringLiteral("sha256"), sha);
    return item;
}

QJsonArray officialAcquisitions(Backend backend, const ComponentPack* resource) {
    QJsonArray result;
    result.append(acquisition(QStringLiteral("aji-v0.9.0"), QString::fromLatin1(kAjiUrl),
                              kAjiBytes, QString::fromLatin1(kAjiSha)));
    if (backend == Backend::TensorRT) {
        const ComponentPack* runtime = tensorRtRuntimePack();
        if (runtime != nullptr) result.append(acquisition(runtime->name, runtime->url, runtime->assetBytes, runtime->assetSha256));
        if (resource != nullptr) result.append(acquisition(resource->name, resource->url, resource->assetBytes, resource->assetSha256));
    } else {
        result.append(acquisition(QStringLiteral("Microsoft.ML.OnnxRuntime.DirectML/1.24.4"),
            QStringLiteral("https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg"),
            12458649, QStringLiteral("57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681")));
        result.append(acquisition(QStringLiteral("Microsoft.AI.DirectML/1.15.4"),
            QStringLiteral("https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg"),
            202292617, QStringLiteral("4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9")));
    }
    return result;
}

bool copyFile(const QString& from, const QString& to, QString* error,
              const std::function<bool()>& cancelled = {},
              const std::function<void(qint64)>& copiedProgress = {}) {
    QFile in(from);
    if (!in.open(QIODevice::ReadOnly)) { *error = QStringLiteral("%1 could not be read").arg(from); return false; }
    QFile out(to);
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { *error = QStringLiteral("%1 could not be created").arg(to); return false; }
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    for (;;) {
        if (cancelled && cancelled()) { *error = QStringLiteral("cancelled"); return false; }
        const qint64 n = in.read(buffer.data(), buffer.size());
        if (n < 0) { *error = in.errorString(); return false; }
        if (n == 0) break;
        if (out.write(buffer.constData(), n) != n) { *error = out.errorString(); return false; }
        if (copiedProgress) copiedProgress(n);
    }
    return out.flush();
}

bool recordAndVerify(const QString& stage, Backend backend, const QStringList& files,
                     const QString& provenance, bool requireAjiExports, const QJsonArray& acquisitions,
                     QString* error, bool* heavyOut) {
    QJsonArray entries;
    for (const QString& name : files) {
        const QString path = QDir(stage).filePath(name);
        QFileInfo info(path);
        if (!info.isFile() || info.isSymLink() || info.size() <= 0 || !noReparse(stage, path)) {
            *error = QStringLiteral("AnimeJaNai runtime file is missing, unsafe, or empty: %1").arg(name); return false;
        }
        const bool binary = name.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive) || name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive);
        if (binary && !validPeX64(path)) { *error = QStringLiteral("AnimeJaNai runtime file is not a valid x64 PE image: %1").arg(name); return false; }
        qint64 bytes = 0; const QString digest = hashFile(path, &bytes);
        if (digest.isEmpty() || bytes != info.size()) { *error = QStringLiteral("AnimeJaNai runtime file could not be measured: %1").arg(name); return false; }
        const QString officialDigest = officialAjiDigest(name);
        if (provenance == QLatin1String("official-download") &&
            !officialDigest.isEmpty() && digest != officialDigest) {
            *error = QStringLiteral("The downloaded Aji DLL does not match its pinned v0.9.0 identity: %1").arg(name);
            return false;
        }
        QJsonObject e; e.insert(QStringLiteral("path"), name); e.insert(QStringLiteral("bytes"), bytes); e.insert(QStringLiteral("sha256"), digest); entries.append(e);
    }
    int abi = 0; bool heavy = false;
    // Upstream v0.9.0 has no version-query export; its exact archive/DLL pins establish ABI 8.
    if (requireAjiExports && provenance == QLatin1String("local")) {
        const bool hasVersionExport = runtimeExports(QDir(stage).filePath(QStringLiteral("aji.dll")), &abi, &heavy);
        if ((hasVersionExport && abi != 8) ||
            (!hasVersionExport && !matchesPinnedOfficialAbi(stage, backend))) {
            *error = QStringLiteral("The imported AnimeJaNai dispatcher does not report ABI 8 and is not the pinned official v0.9.0 x64 backend pair.");
            return false;
        }
        if (!hasVersionExport) heavy = false;
    }
    if (heavyOut) *heavyOut = heavy;
    return writeInstallRecord(stage, backend, entries, provenance, heavy, acquisitions, error);
}

} // namespace

AnimeJanaiInstallation describeAnimeJanaiInstallation(Backend backend, bool rehash) {
    AnimeJanaiInstallation result;
    result.root = installationRoot(backend);
    auto refuse = [&](const QString& key, const QString& text) { result.reasonKey = key; result.reason = text; return result; };
    if (result.root.isEmpty()) return refuse(QStringLiteral("animejanai.installation.pathInvalid"), translated("The app-owned AnimeJaNai runtime path is unavailable."));
    const QFileInfo rootInfo(result.root);
    if (rootInfo.exists() && (!rootInfo.isDir() || rootInfo.isSymLink() ||
        !pathWithinRoot(nativeRoot(), result.root) ||
        !pathHasNoReparsePoints(nativeRoot(), result.root)))
        return refuse(QStringLiteral("animejanai.installation.pathInvalid"), translated("The AnimeJaNai installation directory is not a safe app-owned path."));
    const QString recordPath = QDir(result.root).filePath(QStringLiteral("install.json"));
    if (rootInfo.exists() && !noReparse(result.root, recordPath))
        return refuse(QStringLiteral("animejanai.installation.pathInvalid"), translated("The AnimeJaNai installation record is a reparse point or unsafe path."));
    QFile f(recordPath);
    if (!f.open(QIODevice::ReadOnly)) return refuse(QStringLiteral("animejanai.installation.recordMissing"), translated("The app-owned AnimeJaNai installation record is missing."));
    QJsonParseError parse{}; const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated("The AnimeJaNai installation record is corrupt."));
    const QJsonObject rec = doc.object();
    const QStringList recordKeys{QStringLiteral("schema"), QStringLiteral("version"), QStringLiteral("backend"),
                                 QStringLiteral("provenance"), QStringLiteral("archiveBytes"), QStringLiteral("archiveSha256"),
                                 QStringLiteral("abi"), QStringLiteral("heavyRifeSupported"), QStringLiteral("files"),
                                 QStringLiteral("acquisitions")};
    for (const QString& key : recordKeys) {
        if (!rec.contains(key)) return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated("The AnimeJaNai installation record is missing a required field."));
    }
    for (auto it = rec.constBegin(); it != rec.constEnd(); ++it) {
        if (!recordKeys.contains(it.key())) return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated("The AnimeJaNai installation record contains an unknown field."));
    }
    if (rec.value(QStringLiteral("schema")).toString() != QLatin1String(kAjiSchema) ||
        rec.value(QStringLiteral("version")).toString() != QLatin1String(kAjiVersion) ||
        rec.value(QStringLiteral("backend")).toString() != backendName(backend) ||
        rec.value(QStringLiteral("abi")).toInt() != 8 ||
        !rec.value(QStringLiteral("heavyRifeSupported")).isBool() ||
        !rec.value(QStringLiteral("files")).isArray() ||
        !rec.value(QStringLiteral("acquisitions")).isArray()) {
        return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated("The AnimeJaNai installation record has an incompatible schema, backend, or ABI."));
    }
    const QJsonArray files = rec.value(QStringLiteral("files")).toArray();
    const ComponentPack* resource = nullptr;
    if (backend == Backend::TensorRT) {
        for (const QJsonValue& item : files) {
            if (!item.isObject()) continue;
            const QString path = item.toObject().value(QStringLiteral("path")).toString();
            for (const ComponentPack& pack : tensorRtComponentCatalog()) {
                if (!pack.resourceFile.isEmpty() && pack.resourceFile == path) {
                    if (resource != nullptr) return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated("The TensorRT closure records multiple builder resources."));
                    resource = &pack;
                }
            }
        }
        if (resource == nullptr) return refuse(QStringLiteral("animejanai.installation.incomplete"), translated("The TensorRT closure does not record a compatible builder resource."));
        const GpuComputeCapability& capability = observeComputeCapability();
        const ComponentPack* selected = selectedTensorRtResource();
        if (capability.nvidia && (selected == nullptr || resource->resourceFile != selected->resourceFile))
            return refuse(QStringLiteral("animejanai.installation.resourceMismatch"), translated("The installed TensorRT builder resource does not match the observed GPU."));
    }
    const QStringList required = closureFiles(backend, resource);
    const QStringList optional = optionalNoticeFiles(backend);
    const QString provenance = rec.value(QStringLiteral("provenance")).toString();
    const QJsonArray acquisitionRecords = rec.value(QStringLiteral("acquisitions")).toArray();
    if ((provenance == QStringLiteral("local") &&
         (!acquisitionRecords.isEmpty() || rec.value(QStringLiteral("archiveBytes")).toVariant().toLongLong() != 0 ||
          !rec.value(QStringLiteral("archiveSha256")).toString().isEmpty())) ||
        (provenance == QStringLiteral("official-download") &&
         (rec.value(QStringLiteral("archiveBytes")).toVariant().toLongLong() != kAjiBytes ||
          rec.value(QStringLiteral("archiveSha256")).toString() != QLatin1String(kAjiSha) ||
          acquisitionRecords != officialAcquisitions(backend, resource))) ||
        (provenance != QStringLiteral("local") && provenance != QStringLiteral("official-download")))
        return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated("The AnimeJaNai installation acquisition record does not match its pinned release sources."));
    QMap<QString, QJsonObject> records;
    for (const QJsonValue& value : files) {
        if (!value.isObject()) return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated("The AnimeJaNai installation record contains an invalid file entry."));
        const QJsonObject item = value.toObject();
        const QStringList fileKeys{QStringLiteral("path"), QStringLiteral("bytes"), QStringLiteral("sha256")};
        for (auto it = item.constBegin(); it != item.constEnd(); ++it) {
            if (!fileKeys.contains(it.key())) return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated("The AnimeJaNai installation record contains an unknown file field."));
        }
        const QString name = item.value(QStringLiteral("path")).toString();
        QString why;
        const QString digest = item.value(QStringLiteral("sha256")).toString();
        if (safeRelativePath(name, &why).isEmpty() || name.contains(QLatin1Char('/')) || records.contains(name) ||
            item.value(QStringLiteral("bytes")).toVariant().toLongLong() <= 0 || digest.size() != 64 ||
            std::any_of(digest.cbegin(), digest.cend(), [](QChar c) { return !(c >= QLatin1Char('0') && c <= QLatin1Char('9')) && !(c >= QLatin1Char('a') && c <= QLatin1Char('f')); }))
            return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated("The AnimeJaNai installation record contains an unsafe or incomplete file entry."));
        records.insert(name, item);
    }
    for (auto it = records.cbegin(); it != records.cend(); ++it) {
        if (!required.contains(it.key()) && !optional.contains(it.key()))
            return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated("The AnimeJaNai installation record includes a file outside the selected backend closure."));
    }
    for (const QString& name : required) {
        if (!records.contains(name)) return refuse(QStringLiteral("animejanai.installation.incomplete"), translated("The selected AnimeJaNai backend closure is incomplete: %1 is missing.").arg(name));
    }
    QStringList verifyNames = required;
    for (const QString& name : optional) if (records.contains(name)) verifyNames.append(name);
    for (const QString& name : verifyNames) {
        const QString path = QDir(result.root).filePath(name); const QFileInfo info(path); const QJsonObject entry = records.value(name);
        if (!noReparse(result.root, path) || info.size() != entry.value(QStringLiteral("bytes")).toVariant().toLongLong()) return refuse(QStringLiteral("animejanai.installation.fileInvalid"), translated("An AnimeJaNai runtime file is missing, unsafe, or has changed size: %1").arg(name));
        if (rehash && hashFile(path) != entry.value(QStringLiteral("sha256")).toString()) return refuse(QStringLiteral("animejanai.installation.fileInvalid"), translated("An AnimeJaNai runtime file failed its recorded SHA-256 check: %1").arg(name));
        const QString officialDigest = officialAjiDigest(name);
        if (provenance == QLatin1String("official-download") &&
            !officialDigest.isEmpty() && entry.value(QStringLiteral("sha256")).toString() != officialDigest)
            return refuse(QStringLiteral("animejanai.installation.fileInvalid"), translated("An official Aji DLL does not match the pinned v0.9.0 release."));
        if ((name.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive) || name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) && !validPeX64(path)) return refuse(QStringLiteral("animejanai.installation.peInvalid"), translated("An AnimeJaNai runtime dependency is not a valid x64 Windows PE image: %1").arg(name));
    }
    if (records.size() < required.size() || records.size() > required.size() + optional.size())
        return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated("The AnimeJaNai installation record does not describe the exact selected backend closure."));
    result.heavyRifeSupported = rec.value(QStringLiteral("heavyRifeSupported")).toBool();
    if (provenance == QLatin1String("official-download") && result.heavyRifeSupported)
        return refuse(QStringLiteral("animejanai.installation.capabilityInvalid"), translated("Official Aji v0.9.0 does not support heavy RIFE."));
    if (rehash && provenance == QLatin1String("local")) {
        int abi = 0; bool heavy = false;
        const bool hasVersionExport = runtimeExports(QDir(result.root).filePath(QStringLiteral("aji.dll")), &abi, &heavy);
        if ((hasVersionExport && abi != 8) ||
            (!hasVersionExport && !matchesPinnedOfficialAbi(result.root, backend)))
            return refuse(QStringLiteral("animejanai.installation.abiInvalid"), translated("The installed AnimeJaNai dispatcher does not report ABI 8 and does not match the pinned official v0.9.0 x64 backend pair."));
        if (!hasVersionExport) heavy = false;
        if (result.heavyRifeSupported != heavy)
            return refuse(QStringLiteral("animejanai.installation.capabilityInvalid"), translated("The recorded AnimeJaNai capability does not match the dispatcher export."));
    }
    result.installed = true;
    return result;
}

QVector<AnimeJanaiMissingDownload> animeJanaiMissingDownloads(Backend backend) {
    QVector<AnimeJanaiMissingDownload> missing;
    const AnimeJanaiInstallation installation = describeAnimeJanaiInstallation(backend, false);
    if (!installation.installed) missing.append({QStringLiteral("aji-v0.9.0"), kAjiBytes});
    if (backend == Backend::TensorRT) {
        const ComponentStore store = componentStore();
        const ComponentPack* runtime = tensorRtRuntimePack();
        if (runtime != nullptr && !storedPack(store, *runtime).installed)
            missing.append({runtime->name, runtime->assetBytes});
        const ComponentPack* resource = selectedTensorRtResource();
        if (resource != nullptr && !storedPack(store, *resource).installed)
            missing.append({resource->name, resource->assetBytes});
    } else if (!installation.installed) {
        missing.append({QStringLiteral("onnxruntime-directml-1.24.4"), kOrtBytes});
        missing.append({QStringLiteral("directml-1.15.4"), kDirectMlBytes});
    }
    return missing;
}

qint64 animeJanaiMissingDownloadBytes(Backend backend) {
    qint64 total = 0;
    for (const AnimeJanaiMissingDownload& item : animeJanaiMissingDownloads(backend)) total += item.bytes;
    return total;
}

ComponentProvisionResult importAnimeJanaiRuntime(const QString& sourceDir, Backend backend,
    const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled) {
    { QMutexLocker lock(&g_installMutex); if (g_installRunning) return failed(QStringLiteral("animejanai.installation.busy"), translated("An AnimeJaNai runtime installation is already running.")); g_installRunning = true; }
    const auto release = qScopeGuard([] { QMutexLocker lock(&g_installMutex); g_installRunning = false; });
    const auto isCancelled = [&] { return cancelled && cancelled(); };
    if (isCancelled()) return failed(QStringLiteral("animejanai.installation.cancelled"), translated("AnimeJaNai installation was cancelled."), true);
    const QFileInfo srcInfo(sourceDir); const QString sourceRoot = srcInfo.canonicalFilePath();
    const NativePathTrust sourceTrust = nativePathTrust(sourceRoot, true);
    if (sourceRoot.isEmpty() || !srcInfo.isDir() || !sourceTrust.trusted || sourceTrust.form != NativePathForm::Local) return failed(QStringLiteral("animejanai.installation.sourceInvalid"), translated("The selected AnimeJaNai runtime directory is not a trusted local directory."));
    const QString source = sourceRuntimeDirectory(sourceRoot);
    if (!pathHasNoReparsePoints(sourceRoot, source))
        return failed(QStringLiteral("animejanai.installation.sourceInvalid"), translated("The selected AnimeJaNai inference directory is redirected or contains a reparse point."));
    const ComponentPack* resource = nullptr;
    if (backend == Backend::TensorRT) {
        QString resourceError;
        resource = resourceInDirectory(source, &resourceError);
        if (resource == nullptr) return failed(QStringLiteral("animejanai.installation.incomplete"), resourceError);
        const ComponentPack* selected = selectedTensorRtResource();
        const GpuComputeCapability& capability = observeComputeCapability();
        if (capability.nvidia && (selected == nullptr || resource->resourceFile != selected->resourceFile))
            return failed(QStringLiteral("animejanai.installation.resourceMismatch"), translated("The imported TensorRT builder resource does not match the observed GPU."));
    }
    const QString stageParent = QFileInfo(installationRoot(backend)).absolutePath();
    if (!pathWithinRoot(nativeRoot(), stageParent) || !QDir().mkpath(stageParent) ||
        !pathHasNoReparsePoints(nativeRoot(), stageParent))
        return failed(QStringLiteral("animejanai.installation.stage"), translated("The app-owned runtime directory is not a safe staging location."));
    QTemporaryDir temp(QDir(stageParent).filePath(QStringLiteral("aji-import-XXXXXX")));
    if (!temp.isValid()) return failed(QStringLiteral("animejanai.installation.stage"), translated("An AnimeJaNai staging directory could not be created."));
    QStringList imported = recordedClosureFiles(source, backend, resource);
    qint64 totalBytes = 0;
    for (const QString& name : imported) totalBytes += QFileInfo(QDir(source).filePath(name)).size();
    qint64 copiedBytes = 0;
    for (const QString& name : imported) {
        if (cancelled && cancelled()) return failed(QStringLiteral("animejanai.installation.cancelled"), translated("AnimeJaNai installation was cancelled."), true);
        const QString input = QDir(source).filePath(name);
        if (!noReparse(source, input)) return failed(QStringLiteral("animejanai.installation.incomplete"), translated("The selected runtime directory is missing a required safe file: %1").arg(name));
        QString error;
        if (!copyFile(input, temp.filePath(name), &error, cancelled, [&](qint64 count) {
                copiedBytes += count;
                if (progress) progress({QStringLiteral("animejanai-local-runtime"), copiedBytes, totalBytes});
            })) {
            const bool wasCancelled = isCancelled();
            return failed(wasCancelled ? QStringLiteral("animejanai.installation.cancelled") : QStringLiteral("animejanai.installation.copy"),
                          wasCancelled ? translated("AnimeJaNai installation was cancelled.") : error, wasCancelled);
        }
    }
    bool heavy = false; QString error;
    if (!recordAndVerify(temp.path(), backend, imported, QStringLiteral("local"), true, {}, &error, &heavy)) return failed(QStringLiteral("animejanai.installation.invalid"), error);
    if (cancelled && cancelled()) return failed(QStringLiteral("animejanai.installation.cancelled"), translated("AnimeJaNai installation was cancelled."), true);
    if (!publishStage(temp.path(), backend, &error)) return failed(QStringLiteral("animejanai.installation.publish"), error);
    ComponentProvisionResult result; result.ok = true; result.installedPacks << QStringLiteral("aji-v0.9.0") << backendName(backend); return result;
}

ComponentProvisionResult provisionAnimeJanaiRuntime(Backend backend,
    const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled) {
    {
        QMutexLocker lock(&g_installMutex);
        if (g_installRunning) return failed(QStringLiteral("animejanai.installation.busy"), translated("An AnimeJaNai runtime installation is already running."));
        g_installRunning = true;
    }
    const auto releaseRun = qScopeGuard([] { QMutexLocker lock(&g_installMutex); g_installRunning = false; });
    const AnimeJanaiInstallation current = describeAnimeJanaiInstallation(backend, true);
    if (current.installed) { ComponentProvisionResult r; r.ok = true; r.installedPacks << QStringLiteral("aji-v0.9.0"); return r; }
    const auto isCancelled = [&] { return cancelled && cancelled(); };
    if (isCancelled()) return failed(QStringLiteral("animejanai.installation.cancelled"), translated("AnimeJaNai installation was cancelled."), true);

    const QString scratchRoot = nativeSubdirectory(QStringLiteral("scratch"));
    if (scratchRoot.isEmpty() || !QDir().mkpath(scratchRoot))
        return failed(QStringLiteral("animejanai.installation.scratch"), translated("The AnimeJaNai scratch directory is unavailable."));
    QTemporaryDir scratch(QDir(scratchRoot).filePath(QStringLiteral("aji-runtime-XXXXXX")));
    if (!scratch.isValid()) return failed(QStringLiteral("animejanai.installation.scratch"), translated("A temporary AnimeJaNai acquisition directory could not be created."));

    QString sevenZip;
    try {
        sevenZip = mikai::backend::ensureSevenZip();
    } catch (const std::exception& error) {
        return failed(QStringLiteral("animejanai.installation.extractor"), QString::fromLocal8Bit(error.what()));
    }
    if (sevenZip.isEmpty()) return failed(QStringLiteral("animejanai.installation.extractor"), translated("7-Zip is not available to unpack the AnimeJaNai runtime."));

    const ComponentPack* trtResource = nullptr;
    QVector<const ComponentPack*> trtPacks;
    if (backend == Backend::TensorRT) {
        const GpuComputeCapability& capability = observeComputeCapability();
        if (!capability.nvidia) return failed(QStringLiteral("animejanai.provision.gpuUnavailable"),
            capability.reason.isEmpty() ? translated("TensorRT requires an observed NVIDIA GPU capability.") : capability.reason);
        trtPacks = compatiblePacksFor(capability.smMajor, capability.smMinor);
        trtResource = tensorRtResourcePackFor(capability.smMajor, capability.smMinor);
        if (tensorRtRuntimePack() == nullptr || trtResource == nullptr || trtPacks.size() != 2)
            return failed(QStringLiteral("animejanai.provision.gpuUnsupported"), translated("The observed NVIDIA GPU has no pinned TensorRT runtime and builder-resource closure."));
    }

    QString error;
    const QString ajiArchive = scratch.filePath(QStringLiteral("aji-windows-x64.zip"));
    const bool gotAji = mikai::backend::downloadToFile(QUrl(QString::fromLatin1(kAjiUrl)), ajiArchive,
        kAjiBytes, QString::fromLatin1(kAjiSha),
        [&](qint64 received, qint64 total) { if (progress) progress({QStringLiteral("aji-v0.9.0"), received, total}); },
        cancelled, &error);
    if (!gotAji) {
        const bool wasCancelled = isCancelled();
        return failed(wasCancelled ? QStringLiteral("animejanai.installation.cancelled") : QStringLiteral("animejanai.installation.download"),
                      error, wasCancelled);
    }
    const QSet<QString> ajiMembers{QStringLiteral("aji.dll"), QStringLiteral("aji_trt.dll"), QStringLiteral("aji_dml.dll"),
        QStringLiteral("aji_encode.exe"), QStringLiteral("aji_harness_dml.exe"),
        QStringLiteral("aji_harness.exe"), QStringLiteral("aji_kernel_test.exe")};
    if (!validateArchiveListing(sevenZip, ajiArchive, QString(), ajiMembers, 7, &error))
        return failed(QStringLiteral("animejanai.installation.archive"), error);
    const QString ajiExtract = scratch.filePath(QStringLiteral("aji-extract"));
    if (!QDir().mkpath(ajiExtract) || !mikai::backend::extractArchive(sevenZip, ajiArchive, ajiExtract, &error))
        return failed(QStringLiteral("animejanai.installation.extract"), error);
    const QString ajiRoot = ajiExtract;
    if (isCancelled()) return failed(QStringLiteral("animejanai.installation.cancelled"), translated("AnimeJaNai installation was cancelled."), true);

    const QString stageParent = QFileInfo(installationRoot(backend)).absolutePath();
    if (!pathWithinRoot(nativeRoot(), stageParent) || !QDir().mkpath(stageParent) ||
        !pathHasNoReparsePoints(nativeRoot(), stageParent))
        return failed(QStringLiteral("animejanai.installation.stage"), translated("The app-owned runtime directory is not a safe staging location."));

    const ComponentStore store = componentStore();
    QJsonArray acquisitions;
    acquisitions.append(acquisition(QStringLiteral("aji-v0.9.0"), QString::fromLatin1(kAjiUrl),
                                    kAjiBytes, QString::fromLatin1(kAjiSha)));
    const ComponentPack* resource = nullptr;
    const QString packageExtract = scratch.filePath(QStringLiteral("package-extract"));
    if (backend == Backend::TensorRT) {
        const QVector<const ComponentPack*>& selected = trtPacks;
        const ComponentPack* runtime = tensorRtRuntimePack();
        resource = trtResource;
        QVector<ComponentPack> missingPacks;
        QSet<QString> verifiedPacks;
        for (const ComponentPack* pack : selected) {
            if (pack == nullptr) return failed(QStringLiteral("animejanai.provision.gpuUnsupported"), translated("The observed NVIDIA GPU has no pinned TensorRT runtime and builder-resource closure."));
            const StoredPack present = storedPack(store, *pack);
            if (!present.installed) {
                missingPacks.append(*pack);
                continue;
            }
            if (verifyPack(store, *pack).installed) verifiedPacks.insert(pack->name);
            else missingPacks.append(*pack);
        }
        // One archive serves two consumers - the pack install and the trtexec
        // extraction - so a missing runtime pack is fetched once here and handed
        // to provisioning, which then leaves the file in place for extraction.
        const QString runtimeArchive = scratch.filePath(runtime->asset);
        bool runtimeArchiveReady = false;
        for (const ComponentPack& missing : missingPacks) {
            if (missing.name != runtime->name) continue;
            if (!mikai::backend::downloadToFile(QUrl(runtime->url), runtimeArchive, runtime->assetBytes,
                runtime->assetSha256, [&](qint64 received, qint64 total) {
                    if (progress) progress({QStringLiteral("trt-runtime"), received, total});
                }, cancelled, &error)) {
                const bool wasCancelled = isCancelled();
                return failed(wasCancelled ? QStringLiteral("animejanai.installation.cancelled") : QStringLiteral("animejanai.installation.download"),
                              error, wasCancelled);
            }
            runtimeArchiveReady = true;
            break;
        }
        if (!missingPacks.isEmpty()) {
            const VerifiedComponentArchive reuse{runtime->name, runtimeArchive};
            ComponentProvisionResult dependencies = provisionTensorRtComponents(
                store, missingPacks, scratch.path(), progress, cancelled,
                runtimeArchiveReady ? &reuse : nullptr);
            if (!dependencies.ok) return dependencies;
        }
        for (const ComponentPack* pack : selected) {
            if (!verifiedPacks.contains(pack->name) && !verifyPack(store, *pack).installed)
                return failed(QStringLiteral("animejanai.provision.dependencyIntegrity"), translated("A pinned TensorRT dependency failed store verification."));
        }
        const StoredPack verifiedRuntime = storedPack(store, *runtime);
        const StoredPack verifiedResource = storedPack(store, *resource);
        if (!verifiedRuntime.installed || !verifiedResource.installed)
            return failed(QStringLiteral("animejanai.provision.dependencyIntegrity"),
                          !verifiedRuntime.installed ? verifiedRuntime.reason : verifiedResource.reason);
        for (const ComponentPack* pack : selected)
            acquisitions.append(acquisition(pack->name, pack->url, pack->assetBytes, pack->assetSha256));

        if (!runtimeArchiveReady &&
            !mikai::backend::downloadToFile(QUrl(runtime->url), runtimeArchive, runtime->assetBytes,
                runtime->assetSha256, [&](qint64 received, qint64 total) {
                    if (progress) progress({QStringLiteral("trt-runtime-trtexec"), received, total});
                }, cancelled, &error)) {
            const bool wasCancelled = isCancelled();
            return failed(wasCancelled ? QStringLiteral("animejanai.installation.cancelled") : QStringLiteral("animejanai.installation.download"),
                          error, wasCancelled);
        }
        QSet<QString> runtimeMembers;
        for (const QString& file : runtime->files) runtimeMembers.insert(file);
        runtimeMembers.insert(QStringLiteral("trtexec.exe"));
        if (!validateArchiveListing(sevenZip, runtimeArchive, QStringLiteral("animejanai/inference/"),
                                    runtimeMembers, runtimeMembers.size(), &error))
            return failed(QStringLiteral("animejanai.installation.archive"), error);
        if (!mikai::backend::extractArchive(sevenZip, runtimeArchive, packageExtract, &error))
            return failed(QStringLiteral("animejanai.installation.extract"), error);
    } else {
        constexpr auto kOrtSha = "57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681";
        constexpr auto kDirectMlSha = "4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9";
        const QString ortUrl = QStringLiteral("https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg");
        const QString directMlUrl = QStringLiteral("https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg");
        const QString ortArchive = scratch.filePath(QStringLiteral("onnxruntime-directml.nupkg"));
        const QString directMlArchive = scratch.filePath(QStringLiteral("directml.nupkg"));
        for (const auto& asset : {qMakePair(ortUrl, qMakePair(kOrtBytes, QString::fromLatin1(kOrtSha))),
                                  qMakePair(directMlUrl, qMakePair(kDirectMlBytes, QString::fromLatin1(kDirectMlSha)))}) {
            const QString path = asset.first == ortUrl ? ortArchive : directMlArchive;
            const QString name = asset.first == ortUrl ? QStringLiteral("onnxruntime-directml-1.24.4") : QStringLiteral("directml-1.15.4");
            if (!mikai::backend::downloadToFile(QUrl(asset.first), path, asset.second.first, asset.second.second,
                [&](qint64 received, qint64 total) { if (progress) progress({name, received, total}); },
                cancelled, &error)) {
                const bool wasCancelled = isCancelled();
                return failed(wasCancelled ? QStringLiteral("animejanai.installation.cancelled") : QStringLiteral("animejanai.installation.download"),
                              error, wasCancelled);
            }
            if (!validateSafeArchive(sevenZip, path, &error))
                return failed(QStringLiteral("animejanai.installation.archive"), error);
        }
        const QString ortExtract = scratch.filePath(QStringLiteral("ort-extract"));
        const QString directMlExtract = scratch.filePath(QStringLiteral("directml-extract"));
        if (!mikai::backend::extractArchive(sevenZip, ortArchive, ortExtract, &error) ||
            !mikai::backend::extractArchive(sevenZip, directMlArchive, directMlExtract, &error))
            return failed(QStringLiteral("animejanai.installation.extract"), error);
        acquisitions.append(acquisition(QStringLiteral("Microsoft.ML.OnnxRuntime.DirectML/1.24.4"), ortUrl,
                                        kOrtBytes, QString::fromLatin1(kOrtSha)));
        acquisitions.append(acquisition(QStringLiteral("Microsoft.AI.DirectML/1.15.4"), directMlUrl,
                                        kDirectMlBytes, QString::fromLatin1(kDirectMlSha)));
        QTemporaryDir stage(QDir(QFileInfo(installationRoot(backend)).absolutePath()).filePath(QStringLiteral("aji-stage-XXXXXX")));
        if (!stage.isValid()) return failed(QStringLiteral("animejanai.installation.stage"), translated("An AnimeJaNai staging directory could not be created."));
        for (const QString& name : {QStringLiteral("aji.dll"), backendDll(backend)}) {
            const QString source = QDir(ajiRoot).filePath(name);
            if (!noReparse(ajiRoot, source) || !copyFile(source, stage.filePath(name), &error, cancelled)) {
                const bool wasCancelled = isCancelled();
                return failed(wasCancelled ? QStringLiteral("animejanai.installation.cancelled") : QStringLiteral("animejanai.installation.dependency"),
                              wasCancelled ? translated("AnimeJaNai installation was cancelled.") :
                                  (error.isEmpty() ? translated("The verified Aji archive is missing a required dispatcher DLL.") : error), wasCancelled);
            }
        }
        const QStringList staged{
            QStringLiteral("onnxruntime.dll"), QStringLiteral("DirectML.dll"),
            QStringLiteral("DirectML_LICENSE.txt"), QStringLiteral("ORT_LICENSE.txt"),
            QStringLiteral("ORT_ThirdPartyNotices.txt"), QStringLiteral("DirectML_LICENSE_CODE.txt"),
            QStringLiteral("DirectML_ThirdPartyNotices.txt")};
        const QStringList origins{
            QDir(ortExtract).filePath(QStringLiteral("runtimes/win-x64/native/onnxruntime.dll")),
            QDir(directMlExtract).filePath(QStringLiteral("bin/x64-win/DirectML.dll")),
            QDir(directMlExtract).filePath(QStringLiteral("LICENSE.txt")),
            QDir(ortExtract).filePath(QStringLiteral("LICENSE")),
            QDir(ortExtract).filePath(QStringLiteral("ThirdPartyNotices.txt")),
            QDir(directMlExtract).filePath(QStringLiteral("LICENSE-CODE.txt")),
            QDir(directMlExtract).filePath(QStringLiteral("ThirdPartyNotices.txt"))};
        for (qsizetype i = 0; i < staged.size(); ++i) {
            const QString& originRoot = i == 0 || i == 3 || i == 4 ? ortExtract : directMlExtract;
            const bool optionalNotice = i >= 3;
            if (optionalNotice && !QFileInfo::exists(origins.at(i))) continue;
            if (!noReparse(originRoot, origins.at(i)) ||
                !copyFile(origins.at(i), stage.filePath(staged.at(i)), &error, cancelled)) {
                const bool wasCancelled = isCancelled();
                return failed(wasCancelled ? QStringLiteral("animejanai.installation.cancelled") : QStringLiteral("animejanai.installation.dependency"),
                              wasCancelled ? translated("AnimeJaNai installation was cancelled.") :
                                  (error.isEmpty() ? translated("A pinned DirectML package member is missing.") : error), wasCancelled);
            }
        }
        bool heavy = false;
        if (!recordAndVerify(stage.path(), backend, recordedClosureFiles(stage.path(), backend, nullptr), QStringLiteral("official-download"), true,
                             acquisitions, &error, &heavy))
            return failed(QStringLiteral("animejanai.installation.invalid"), error);
        if (heavy) return failed(QStringLiteral("animejanai.installation.capabilityInvalid"), translated("The official Aji v0.9.0 asset unexpectedly reports heavy RIFE support."));
        if (isCancelled()) return failed(QStringLiteral("animejanai.installation.cancelled"), translated("AnimeJaNai installation was cancelled."), true);
        if (!publishStage(stage.path(), backend, &error)) return failed(QStringLiteral("animejanai.installation.publish"), error);
        ComponentProvisionResult result; result.ok = true; result.installedPacks << QStringLiteral("aji-v0.9.0") << QStringLiteral("onnxruntime-directml-1.24.4") << QStringLiteral("directml-1.15.4"); return result;
    }

    QTemporaryDir stage(QDir(QFileInfo(installationRoot(backend)).absolutePath()).filePath(QStringLiteral("aji-stage-XXXXXX")));
    if (!stage.isValid()) return failed(QStringLiteral("animejanai.installation.stage"), translated("An AnimeJaNai staging directory could not be created."));
    for (const QString& name : {QStringLiteral("aji.dll"), backendDll(backend)}) {
        const QString source = QDir(ajiRoot).filePath(name);
        if (!noReparse(ajiRoot, source) || !copyFile(source, stage.filePath(name), &error))
            return failed(QStringLiteral("animejanai.installation.dependency"), error.isEmpty() ? translated("The verified Aji archive is missing a required dispatcher DLL.") : error);
    }
    const ComponentPack* runtime = tensorRtRuntimePack();
    const QString storeDir = componentReleaseDirectory(store);
    for (const QString& name : {QStringLiteral("cudart64_13.dll"), QStringLiteral("nvinfer_11.dll"),
                                QStringLiteral("nvinfer_plugin_11.dll"), QStringLiteral("nvonnxparser_11.dll"),
                                resource->resourceFile, QStringLiteral("CUDA_LICENSE.txt"), QStringLiteral("DirectML_LICENSE.txt")}) {
        const QString source = QDir(storeDir).filePath(name);
        if (!noReparse(storeDir, source) || !copyFile(source, stage.filePath(name), &error, cancelled)) {
            const bool wasCancelled = isCancelled();
            return failed(wasCancelled ? QStringLiteral("animejanai.installation.cancelled") : QStringLiteral("animejanai.installation.dependency"),
                          wasCancelled ? translated("AnimeJaNai installation was cancelled.") :
                              (error.isEmpty() ? translated("The verified TensorRT store is missing a selected dependency.") : error), wasCancelled);
        }
    }
    for (const QString& notice : optionalNoticeFiles(backend)) {
        const QString source = QDir(packageExtract).filePath(QStringLiteral("animejanai/inference/") + notice);
        if (!QFileInfo::exists(source)) continue;
        if (!noReparse(packageExtract, source) || !copyFile(source, stage.filePath(notice), &error, cancelled)) {
            const bool wasCancelled = isCancelled();
            return failed(wasCancelled ? QStringLiteral("animejanai.installation.cancelled") : QStringLiteral("animejanai.installation.dependency"),
                          wasCancelled ? translated("AnimeJaNai installation was cancelled.") :
                              (error.isEmpty() ? translated("An upstream runtime notice could not be copied.") : error), wasCancelled);
        }
    }
    const QString trtexec = QDir(packageExtract).filePath(QStringLiteral("animejanai/inference/trtexec.exe"));
    if (!noReparse(packageExtract, trtexec) || !copyFile(trtexec, stage.filePath(QStringLiteral("trtexec.exe")), &error, cancelled)) {
        const bool wasCancelled = isCancelled();
        return failed(wasCancelled ? QStringLiteral("animejanai.installation.cancelled") : QStringLiteral("animejanai.installation.dependency"),
                      wasCancelled ? translated("AnimeJaNai installation was cancelled.") :
                          (error.isEmpty() ? translated("The verified upstream TensorRT archive is missing trtexec.exe.") : error), wasCancelled);
    }
    bool heavy = false;
    if (!recordAndVerify(stage.path(), backend, recordedClosureFiles(stage.path(), backend, resource), QStringLiteral("official-download"), true,
                         acquisitions, &error, &heavy))
        return failed(QStringLiteral("animejanai.installation.invalid"), error);
    if (heavy) return failed(QStringLiteral("animejanai.installation.capabilityInvalid"), translated("The official Aji v0.9.0 asset unexpectedly reports heavy RIFE support."));
    if (isCancelled()) return failed(QStringLiteral("animejanai.installation.cancelled"), translated("AnimeJaNai installation was cancelled."), true);
    if (!publishStage(stage.path(), backend, &error)) return failed(QStringLiteral("animejanai.installation.publish"), error);
    ComponentProvisionResult result; result.ok = true; result.installedPacks << QStringLiteral("aji-v0.9.0") << runtime->name << resource->name; return result;
}

} // namespace mplayer::native
