#include <utility>
#include "AnimeJanaiRuntime.hpp"
#include "AnimeJanaiRuntimeStore.hpp"

#include "backend/Cancellation.hpp"
#include "backend/Downloads.hpp"
#include "native/bundle/NativePaths.hpp"
#include "native/components/NativeComponentCatalog.hpp"
#include "native/components/NativeComponentStore.hpp"
#include "native/components/NativeComponentProvisioning.hpp"
#include "native/components/NativeGpuProbe.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
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
#else
#  include <dlfcn.h>
#endif

#include <algorithm>
#include <exception>

namespace mplayer::native {

namespace {
constexpr auto kLinuxAjiDispatcherSha = "6dd2119af3c4b8469399fcf50b955ddda350bbf1758d7b36aa797b608491755b";
constexpr auto kLinuxAjiTrtSha = "9f55b88491a0bf3bb44faa82342b468d8513b5ca44f023ec82fe60bb6c5818be";
}

bool detail::matchesPinnedLinuxAjiPair(const QString& dispatcherSha256, const QString& backendSha256) {
    return dispatcherSha256 == QLatin1String(kLinuxAjiDispatcherSha) &&
           backendSha256 == QLatin1String(kLinuxAjiTrtSha);
}

bool detail::validateAnimeJanaiArchiveListing(const QString& listing, bool allowInternalSymlinks,
                                               bool tarMembers, QString* error,
                                               const QSet<QString>& expectedFiles) {
    QSet<QString> seen;
    QSet<QString> files;
    QSet<QString> links;
    QString path;
    bool active = false, entry = false, hasPath = false, folder = false, link = false;
    auto inspect = [&]() {
        if (!active || !entry) return true;
        // Recognize only the original relative root spellings. In particular,
        // stripping a trailing slash must never turn an absolute root into one.
        if (hasPath && folder && !link &&
            (path.isEmpty() || path == QLatin1String(".") ||
             (tarMembers && path == QLatin1String("./")))) return true;
        QString normalizedPath = path;
        normalizedPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (tarMembers && normalizedPath.startsWith(QLatin1String("./")))
            normalizedPath.remove(0, 2);
        if (folder && normalizedPath.endsWith(QLatin1Char('/'))) normalizedPath.chop(1);
        QString safeReason;
        const QString normalized = safeRelativePath(normalizedPath, &safeReason);
        if (!hasPath || normalized.isEmpty() || normalized != normalizedPath) {
            *error = QStringLiteral("The pinned package archive contains an unsafe path: %1 (%2)")
                         .arg(path, safeReason.isEmpty() ? QStringLiteral("path is not canonical") : safeReason);
            return false;
        }
        const QString key = normalizedPath.toCaseFolded();
        if (seen.contains(key)) {
            *error = QStringLiteral("The pinned package archive contains a duplicate path: %1").arg(path);
            return false;
        }
        seen.insert(key);
        if (folder && !expectedFiles.isEmpty()) {
            *error = QStringLiteral("The verified runtime archive contains an unexpected directory: %1").arg(path);
            return false;
        }
        if (!folder) files.insert(normalizedPath);
        if (link) links.insert(key);
        return true;
    };
    auto reset = [&] { path.clear(); entry = false; hasPath = false; folder = false; link = false; };
    for (QString line : listing.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.startsWith(QLatin1String("----------"))) {
            if (!inspect()) return false;
            reset(); active = true;
        } else if (!active) {
            continue;
        } else if (line.isEmpty()) {
            if (!inspect()) return false;
            reset();
        } else if (line == QLatin1String("Path =") || line.startsWith(QLatin1String("Path = "))) {
            path = line.mid(7); hasPath = true; entry = true;
        } else if (line == QLatin1String("Folder = +")) {
            folder = true; entry = true;
        } else if (line.startsWith(QLatin1String("Symbolic Link =")) ||
                   line.startsWith(QLatin1String("Hard Link ="))) {
            // 7-Zip prints both fields for every entry and leaves the value empty
            // for an ordinary file. Treating the field itself as a link rejected
            // every real archive: aji-linux-x64.tar.zst has no links at all.
            const QString target = line.mid(line.indexOf(QLatin1Char('=')) + 1).trimmed();
            if (!target.isEmpty()) {
                if (line.startsWith(QLatin1String("Hard Link ="))) {
                    *error = QStringLiteral("The pinned package archive contains a link entry.");
                    return false;
                }
                if (!allowInternalSymlinks) {
                    *error = QStringLiteral("The pinned package archive contains a link entry.");
                    return false;
                }
                QString targetReason;
                if (safeRelativePath(target, &targetReason) != target) {
                    *error = QStringLiteral("The pinned package archive contains an unsafe symbolic link target: %1")
                                 .arg(target);
                    return false;
                }
                link = true;
            }
            entry = true;
        } else {
            entry = true;
        }
    }
    if (!inspect()) return false;
    for (const QString& member : seen) {
        qsizetype slash = member.indexOf(QLatin1Char('/'));
        while (slash >= 0) {
            if (links.contains(member.left(slash))) {
                *error = QStringLiteral("The pinned package archive contains a symbolic link ancestor: %1").arg(member);
                return false;
            }
            slash = member.indexOf(QLatin1Char('/'), slash + 1);
        }
    }
    if (seen.isEmpty()) {
        *error = QStringLiteral("The pinned package archive contains no files or directories.");
        return false;
    }
    if (!expectedFiles.isEmpty() && files != expectedFiles) {
        *error = QStringLiteral("The verified runtime archive does not contain the exact expected file inventory.");
        return false;
    }
    return true;
}

namespace {

#ifdef Q_OS_WIN
constexpr auto kAjiBytes = qint64(953548);
constexpr auto kAjiSha = "1ae2a0d16be2a660d8197fe4f64e8be7cf7637d7842b19994636e42309ab17b2";
constexpr auto kAjiUrl = "https://github.com/the-database/animejanai-inference/releases/download/v0.27.0/aji-windows-x64.zip";
constexpr auto kAjiArchiveName = "aji-windows-x64.zip";
#else
constexpr auto kAjiBytes = qint64(449466);
constexpr auto kAjiSha = "044a9cc652fa7660218597c1faa01b7c0abdd97f10a644b0250a0a42321ac0f5";
constexpr auto kAjiUrl = "https://github.com/the-database/animejanai-inference/releases/download/v0.27.0/aji-linux-x64.tar.zst";
constexpr auto kAjiArchiveName = "aji-linux-x64.tar.zst";
#endif
constexpr qint64 kOrtBytes = 12458649;
constexpr qint64 kDirectMlBytes = 202292617;
constexpr auto kAjiSchema = "mikai.native.animejanai/3";
constexpr auto kAjiVersion = "v0.27.0";
// Acquisition identity and installation path must share the archive's version pin.
QString ajiDispatcherLabel() {
    static const QString label = QStringLiteral("aji-") + QString::fromLatin1(kAjiVersion);
    return label;
}
constexpr qint64 kRtxBackendBytes = 440346;
constexpr auto kRtxBackendSha = "13417fd2a061bc4539ea2509c1ced5023b947d561431a9b527c718c89a00ca35";
constexpr auto kRtxBackendUrl = "https://github.com/the-database/animejanai-inference/releases/download/v0.27.0/aji-trtrtx-backend-windows-x64.zip";
constexpr auto kRtxBackendArchiveName = "aji-trtrtx-backend-windows-x64.zip";
constexpr auto kRtxBackendLabel = "aji-trtrtx-v0.27.0";
QMutex g_installMutex;
bool g_installRunning = false;

QString backendName(Backend backend);

QString installationRoot(Backend backend) {
    const QString components = nativeSubdirectory(QStringLiteral("components"));
    return components.isEmpty() ? QString{} :
        QDir(components).filePath(
            QStringLiteral("aji/%1/").arg(QString::fromLatin1(kAjiVersion)) + backendName(backend));
}
QString translated(const char* text) {
    return QCoreApplication::translate("AnimeJanaiInstallation", text);
}
QString backendName(Backend backend) {
    if (backend == Backend::TensorRT) return QStringLiteral("trt");
    if (backend == Backend::TensorRTRTX) return QStringLiteral("trtrtx");
    return QStringLiteral("dml");
}
QString backendDll(Backend backend) {
    if (backend == Backend::TensorRT) return QStringLiteral("aji_trt.dll");
    if (backend == Backend::TensorRTRTX) return QStringLiteral("aji_trtrtx.dll");
    return QStringLiteral("aji_dml.dll");
}
const ComponentPack* selectedTensorRtResource() {
    const GpuComputeCapability& capability = observeComputeCapability();
    if (!capability.nvidia) return nullptr;
    return tensorRtResourcePackFor(capability.smMajor, capability.smMinor);
}

const ComponentPack* resourceInDirectory(const QString& directory, QString* error) {
    const ComponentPack* found = nullptr;
    auto scanCatalog = [&](const QVector<ComponentPack>& catalog) -> bool {
        for (const ComponentPack& pack : catalog) {
            if (pack.resourceFile.isEmpty()) continue;
            const QFileInfo candidate(QDir(directory).filePath(pack.resourceFile));
            if (!candidate.exists()) continue;
            if (found != nullptr && found->resourceFile != pack.resourceFile) {
                *error = QStringLiteral("The TensorRT source contains more than one builder resource.");
                return false;
            }
            found = &pack;
        }
        return true;
    };
    if (!scanCatalog(tensorRtComponentCatalog())) return nullptr;
    if (found == nullptr) *error = QStringLiteral("The TensorRT source is missing its selected builder resource.");
    return found;
}

QStringList windowsClosureFiles(Backend backend, const ComponentPack* resource = nullptr) {
    if (backend == Backend::TensorRTRTX) {
        // The released RTX backend imports tensorrt_rtx_1_7.dll and
        // tensorrt_onnxparser_rtx_1_7.dll and nothing else: no cudart, no classic
        // nvinfer/plugin/parser pair, no builder resource and no trtexec. Both
        // NVIDIA notices are mandatory members of the selected closure.
        return {QStringLiteral("aji.dll"), QStringLiteral("aji_trtrtx.dll"),
                QStringLiteral("tensorrt_rtx_1_7.dll"),
                QStringLiteral("tensorrt_onnxparser_rtx_1_7.dll"),
                QStringLiteral("TensorRT-RTX-NOTICE.txt"),
                QStringLiteral("TensorRT-RTX-Acknowledgements.txt")};
    }
    QStringList files{QStringLiteral("aji.dll"), backendDll(backend)};
    if (backend == Backend::TensorRT) {
        files << QStringLiteral("cudart64_13.dll") << QStringLiteral("nvinfer_11.dll")
              << QStringLiteral("nvinfer_plugin_11.dll") << QStringLiteral("nvonnxparser_11.dll");
        if (resource != nullptr) files << resource->resourceFile;
        files << QStringLiteral("trtexec.exe") << QStringLiteral("CUDA_LICENSE.txt")
              << QStringLiteral("TensorRT_Acknowledgements.txt");
    } else {
        files << QStringLiteral("onnxruntime.dll") << QStringLiteral("DirectML.dll")
              << QStringLiteral("DirectML_LICENSE.txt");
    }
    return files;
}

QStringList linuxClosureFiles(Backend backend, const ComponentPack* resource = nullptr) {
    if (backend != Backend::TensorRT) {
        // TensorRT for RTX publishes no Linux asset. Every entry point refuses it
        // by name before reaching a closure, and the fallthrough keeps returning
        // the RTX Windows list rather than pretending the DirectML closure fits.
        return windowsClosureFiles(backend, resource);
    }
    QStringList files{QStringLiteral("libaji.so"), QStringLiteral("libaji_trt.so"),
                      QStringLiteral("libcudart.so.13"), QStringLiteral("libnvinfer.so.11"),
                      QStringLiteral("libnvinfer_plugin.so.11"), QStringLiteral("libnvonnxparser.so.11")};
    if (resource != nullptr) files << resource->resourceFile;
    files << QStringLiteral("trtexec") << QStringLiteral("CUDA_LICENSE.txt")
          << QStringLiteral("TensorRT_Acknowledgements.txt");
    return files;
}

QStringList closureFiles(Backend backend, const ComponentPack* resource = nullptr,
                         bool linuxClosure = false) {
    return linuxClosure ? linuxClosureFiles(backend, resource)
                        : windowsClosureFiles(backend, resource);
}

QStringList optionalNoticeFiles(Backend backend) {
    if (backend == Backend::TensorRT)
        return {QStringLiteral("CUDA_LICENSE.txt"), QStringLiteral("DirectML_LICENSE.txt"),
                QStringLiteral("THIRD_PARTY_NOTICES.txt"), QStringLiteral("TensorRT_Acknowledgements.txt")};
    // Both RTX notices are required closure members, so nothing here is optional.
    if (backend == Backend::TensorRTRTX) return {};
    return {QStringLiteral("ORT_LICENSE.txt"), QStringLiteral("ORT_ThirdPartyNotices.txt"),
            QStringLiteral("DirectML_LICENSE_CODE.txt"), QStringLiteral("DirectML_ThirdPartyNotices.txt"),
            QStringLiteral("THIRD_PARTY_NOTICES.txt")};
}

bool directoryUsesLinuxClosure(const QString& directory) {
    return QFileInfo::exists(QDir(directory).filePath(QStringLiteral("libaji.so")));
}

QStringList sharedClosureFiles(Backend backend, bool linuxClosure) {
    return {linuxClosure ? QStringLiteral("libaji.so") : QStringLiteral("aji.dll"),
            linuxClosure ? QStringLiteral("libaji_trt.so") : backendDll(backend),
            QStringLiteral("aji-runtime.json")};
}

bool hasSharedMapping(const QString& directory) {
    return QFileInfo::exists(QDir(directory).filePath(QStringLiteral("aji-runtime.json")));
}

QStringList recordedClosureFiles(const QString& directory, Backend backend, const ComponentPack* resource) {
    const bool linuxClosure = directoryUsesLinuxClosure(directory) ||
        (resource != nullptr && resource->resourceFile.endsWith(QLatin1String(".so")));
    if (isTensorRtFamily(backend) && hasSharedMapping(directory))
        return sharedClosureFiles(backend, linuxClosure);
    QStringList files = closureFiles(backend, resource, linuxClosure);
    for (const QString& notice : optionalNoticeFiles(backend)) {
        if (files.contains(notice)) continue;
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
bool validPeX64(const QString& path) {
    constexpr quint16 kImageFileMachineAmd64 = 0x8664;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray dos = f.read(64);
    if (dos.size() != 64 || dos.left(2) != QByteArrayLiteral("MZ")) return false;
    const quint32 offset = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(dos.constData() + 0x3c));
    if (offset > 16 * 1024 * 1024 || !f.seek(offset)) return false;
    const QByteArray header = f.read(6);
    if (header.size() != 6 || header.left(4) != QByteArray("PE\0\0", 4) ||
        qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(header.constData() + 4)) != kImageFileMachineAmd64 ||
        !f.seek(static_cast<qint64>(offset) + 24)) return false;
    const QByteArray optionalMagic = f.read(2);
    return optionalMagic.size() == 2 &&
           qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(optionalMagic.constData())) == 0x20b;
}
bool validElfX64(const QString& path) {
    constexpr quint16 kElfMachineX86_64 = 0x3e;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray hdr = f.read(20);
    if (hdr.size() < 20) return false;
    if (hdr.left(4) != QByteArray("\x7f" "ELF", 4)) return false;
    if (static_cast<uchar>(hdr.at(4)) != 2 || static_cast<uchar>(hdr.at(5)) != 1) return false;
    const quint16 machine =
        qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(hdr.constData() + 18));
    return machine == kElfMachineX86_64;
}
bool isBinaryFileName(const QString& name) {
    return name.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive) ||
           name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive) ||
           name.endsWith(QStringLiteral(".so"), Qt::CaseInsensitive) ||
           name.contains(QStringLiteral(".so."), Qt::CaseInsensitive) ||
           name == QLatin1String("trtexec");
}
bool validBinaryImageX64(const QString& name, const QString& path) {
    if (name.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive) ||
        name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        return validPeX64(path);
    }
    return validElfX64(path);
}
bool noReparse(const QString& root, const QString& path) {
    const QFileInfo info(path);
    return info.exists() && !info.isSymLink() && pathWithinRoot(root, info.absoluteFilePath()) &&
           pathHasNoReparsePoints(root, info.absoluteFilePath());
}
QString pinnedOfficialAjiDigest(const QString& name) {
    if (name == QLatin1String("libaji.so"))
        return QString::fromLatin1(kLinuxAjiDispatcherSha);
    if (name == QLatin1String("libaji_trt.so"))
        return QString::fromLatin1(kLinuxAjiTrtSha);
    if (name == QLatin1String("aji.dll"))
        return QStringLiteral("095d18018b2ae7f40d9b7e814a9891867f6028ea94d3e06048d2d40276e63b45");
    if (name == QLatin1String("aji_trt.dll"))
        return QStringLiteral("a681d572073dc0c0229a8f93a563aa8210dd283403d83ea175217ac23fde8a92");
    if (name == QLatin1String("aji_dml.dll"))
        return QStringLiteral("57ca026d5c1acebd541c7080efbcfd7c7b46f03d8d9411f667fed049f2236642");
    // The RTX dispatcher has no aji_api_version export either, so these per-file
    // pins are the only identity gate for an official RTX acquisition.
    if (name == QLatin1String("aji_trtrtx.dll"))
        return QStringLiteral("405c3e0e2bf2fce24e0e3f16a1de04bc52cf4fdb012e3bfb94d3a68cdf33f9c4");
    if (name == QLatin1String("tensorrt_rtx_1_7.dll"))
        return QStringLiteral("7b8a06d4b66dfe7da7f1ff48a994ad9dcf107cd28b4822d6f25453528c0db6dd");
    if (name == QLatin1String("tensorrt_onnxparser_rtx_1_7.dll"))
        return QStringLiteral("b38d00d107dae534f1fc855df5bc1a04fc0ae25135b5b10d17f9b30cc4c9c411");
    if (name == QLatin1String("TensorRT-RTX-NOTICE.txt"))
        return QStringLiteral("da7aa4b4bba64232bb9e868c5899e14bd74e983c6f5df017febfe24f72e2b5e7");
    if (name == QLatin1String("TensorRT-RTX-Acknowledgements.txt"))
        return QStringLiteral("611660ec0ae19c7b8df4a6442009c8bc85ba9e56d7d5ea042564841e72032d25");
    return {};
}
bool matchesPinnedOfficialAbi(const QString& directory, Backend backend) {
#ifndef Q_OS_WIN
    if (backend != Backend::TensorRT) return false;
    const QString dispatcherPath = QDir(directory).filePath(QStringLiteral("libaji.so"));
    const QString backendPath = QDir(directory).filePath(QStringLiteral("libaji_trt.so"));
    return validElfX64(dispatcherPath) && validElfX64(backendPath) &&
           detail::matchesPinnedLinuxAjiPair(hashFile(dispatcherPath), hashFile(backendPath));
#else
    const QString dispatcher = hashFile(QDir(directory).filePath(QStringLiteral("aji.dll")));
    const QString selected = hashFile(QDir(directory).filePath(backendDll(backend)));
    return dispatcher == pinnedOfficialAjiDigest(QStringLiteral("aji.dll")) &&
           selected == pinnedOfficialAjiDigest(backendDll(backend));
#endif
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
    const QByteArray encoded = QFile::encodeName(path);
    void* module = dlopen(encoded.constData(), RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) return false;
    using VersionFn = int (*)();
    using HeavyFn = int (*)();
    auto version = reinterpret_cast<VersionFn>(dlsym(module, "aji_api_version"));
    auto heavyFn = reinterpret_cast<HeavyFn>(dlsym(module, "aji_rife_heavy_supported"));
    const bool ok = version != nullptr;
    if (ok) { *abi = version(); *heavy = heavyFn != nullptr && heavyFn() != 0; }
    dlclose(module);
    return ok;
#endif
}

bool matchesCompatibleBackend(const QString& directory, Backend backend) {
#ifdef Q_OS_WIN
    const QString name = backendDll(backend);
#else
    if (backend != Backend::TensorRT) return false;
    const QString name = QStringLiteral("libaji_trt.so");
#endif
    const QString path = QDir(directory).filePath(name);
    if (hashFile(path) == pinnedOfficialAjiDigest(name)) return true;
    int abi = 0;
    bool ignoredHeavy = false;
    return runtimeExports(path, &abi, &ignoredHeavy) && abi == 9;
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
    record.insert(QStringLiteral("abi"), 9);
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

bool validateSafeArchive(const QString& sevenZip, const QString& archive, QString* error,
                         bool allowInternalSymlinks = false,
                         const std::function<bool()>& cancelled = {},
                         const QSet<QString>& expectedFiles = {}) {
    const auto finish = [&](QProcess& process) {
        if (!process.waitForStarted()) return false;
        while (!process.waitForFinished(100)) {
            if (cancelled && cancelled()) {
                process.kill();
                process.waitForFinished();
                *error = QStringLiteral("AnimeJaNai installation was cancelled.");
                return false;
            }
            if (process.state() == QProcess::NotRunning) break;
        }
        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    };
    // A .tar.zst listing describes the compressed stream, not the tar members.
    // Decompress only to a private temporary tar and inspect that before either
    // tar or 7-Zip is allowed to extract its contents.
    const bool tarMembers = archive.endsWith(QLatin1String(".tar.zst"), Qt::CaseInsensitive);
    QTemporaryDir temporary;
    QString listingArchive = archive;
    if (tarMembers) {
        if (!temporary.isValid()) {
            *error = QStringLiteral("Could not create a temporary directory for archive validation.");
            return false;
        }
        listingArchive = QDir(temporary.path()).filePath(QStringLiteral("members.tar"));
        QProcess decompress;
        decompress.setStandardOutputFile(listingArchive);
        decompress.start(sevenZip, {QStringLiteral("x"), QStringLiteral("-so"), archive});
        if (!finish(decompress)) {
            if (error->isEmpty())
                *error = QStringLiteral("The pinned package archive could not be decompressed for validation (%1).")
                             .arg(processFailureDetails(decompress));
            return false;
        }
    }
    QProcess process;
    process.start(sevenZip, {QStringLiteral("l"), QStringLiteral("-slt"), listingArchive});
    if (!finish(process)) {
        if (error->isEmpty())
            *error = QStringLiteral("The pinned package archive listing could not be read (%1).")
                         .arg(processFailureDetails(process));
        return false;
    }
    return detail::validateAnimeJanaiArchiveListing(
        QString::fromLocal8Bit(process.readAllStandardOutput()), allowInternalSymlinks, tarMembers, error, expectedFiles);
}

#ifdef Q_OS_WIN
bool validateArchiveListing(const QString& sevenZip, const QString& archive, const QString& prefix,
                            const QSet<QString>& expectedFiles, int expectedFileCount,
                            QString* error) {
    QProcess process;
    process.start(sevenZip, {QStringLiteral("l"), QStringLiteral("-slt"), archive});
    if (!process.waitForStarted()) {
        *error = QStringLiteral("The verified runtime archive listing could not be read (%1).")
                     .arg(processFailureDetails(process));
        return false;
    }
    // X-H6: was waitForFinished(-1) — unbounded, and a hung archive tool
    // blocked a transferPool_ thread forever. 100 ms slices, 60 s cap,
    // honouring the surrounding cancellation scope; the child is killed
    // on cancel or timeout.
    {
        QElapsedTimer deadline;
        deadline.start();
        bool cancelled = false;
        bool timedOut = false;
        while (!process.waitForFinished(100)) {
            if (mikai::backend::WorkCancellationScope::cancellationRequested()) {
                cancelled = true;
                break;
            }
            if (deadline.elapsed() >= 60000) {
                timedOut = true;
                break;
            }
            if (process.state() == QProcess::NotRunning) break;
        }
        if (cancelled || timedOut) {
            process.kill();
            process.waitForFinished();
            *error = cancelled
                         ? QStringLiteral("The verified runtime archive listing was cancelled.")
                         : QStringLiteral(
                               "The verified runtime archive listing timed out after 60 seconds.");
            return false;
        }
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
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

#endif

QJsonObject acquisition(const QString& name, const QString& url, qint64 bytes, const QString& sha) {
    QJsonObject item;
    item.insert(QStringLiteral("name"), name);
    item.insert(QStringLiteral("url"), url);
    item.insert(QStringLiteral("bytes"), bytes);
    item.insert(QStringLiteral("sha256"), sha);
    return item;
}

QJsonArray officialAcquisitions(Backend backend) {
    QJsonArray result;
    result.append(acquisition(ajiDispatcherLabel(), QString::fromLatin1(kAjiUrl),
                              kAjiBytes, QString::fromLatin1(kAjiSha)));
    if (isTensorRtFamily(backend)) {
        if (backend == Backend::TensorRTRTX)
            result.append(acquisition(QString::fromLatin1(kRtxBackendLabel), QString::fromLatin1(kRtxBackendUrl),
                                      kRtxBackendBytes, QString::fromLatin1(kRtxBackendSha)));
        for (const QJsonValue& source : animeJanaiSharedRuntimeAcquisitions(backend))
            result.append(source);
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
    const bool ok = out.flush();
    out.close();
#ifndef Q_OS_WIN
    if (ok && isBinaryFileName(QFileInfo(to).fileName()) &&
        !QFile::setPermissions(to,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                              QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                              QFileDevice::ReadOther | QFileDevice::ExeOther)) {
        *error = QStringLiteral("%1 could not be made executable").arg(to);
        return false;
    }
#endif
    return ok;
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
        const bool binary = isBinaryFileName(name);
        if (binary && !validBinaryImageX64(name, path)) { *error = QStringLiteral("AnimeJaNai runtime file is not a valid x64 binary image: %1").arg(name); return false; }
        qint64 bytes = 0; const QString digest = hashFile(path, &bytes);
        if (digest.isEmpty() || bytes != info.size()) { *error = QStringLiteral("AnimeJaNai runtime file could not be measured: %1").arg(name); return false; }
        const QString officialDigest = pinnedOfficialAjiDigest(name);
        if (provenance == QLatin1String("official-download") &&
            !officialDigest.isEmpty() && digest != officialDigest) {
            *error = translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The downloaded Aji binary does not match its pinned v0.27.0 identity: %1")).arg(name);
            return false;
        }
        QJsonObject e; e.insert(QStringLiteral("path"), name); e.insert(QStringLiteral("bytes"), bytes); e.insert(QStringLiteral("sha256"), digest); entries.append(e);
    }
    int abi = 0; bool heavy = false;
    // The official v0.27.0 dispatcher has no version-query export. Its pinned
    // dispatcher/backend pair establishes ABI 9; custom dispatchers must report it.
    // Both upstream backend model-name mappings turn code 4262 into rife_v4.26,
    // not rife_v4.26_heavy, so the official pair cannot select the heavy graph.
    if (requireAjiExports && provenance == QLatin1String("local")) {
        const QString dispPath = QFileInfo::exists(QDir(stage).filePath(QStringLiteral("libaji.so")))
                                     ? QDir(stage).filePath(QStringLiteral("libaji.so"))
                                     : QDir(stage).filePath(QStringLiteral("aji.dll"));
        const bool hasVersionExport = runtimeExports(dispPath, &abi, &heavy);
        if ((hasVersionExport && abi != 9) ||
            (!hasVersionExport && !matchesPinnedOfficialAbi(stage, backend))) {
            *error = translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The imported AnimeJaNai dispatcher does not report ABI 9 and is not the pinned official v0.27.0 x64 backend pair."));
            return false;
        }
        if (!matchesCompatibleBackend(stage, backend)) {
            *error = translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The imported AnimeJaNai backend does not report ABI 9 and is not the pinned official v0.27.0 backend."));
            return false;
        }
        if (!hasVersionExport) heavy = false;
    }
    if (heavyOut) *heavyOut = heavy;
    return writeInstallRecord(stage, backend, entries, provenance, heavy, acquisitions, error);
}

ComponentProvisionResult provisionSharedRuntime(
    const QString& scratchDir, const QString& sevenZip, const QString& ajiRoot, Backend backend,
    [[maybe_unused]] bool reuseExistingShims, const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled) {
    const auto isCancelled = [&] {
        return (cancelled && cancelled()) ||
               mikai::backend::WorkCancellationScope::cancellationRequested();
    };
    QString error;
    QString backendRoot = ajiRoot;
#ifdef Q_OS_WIN
    if (backend == Backend::TensorRTRTX && !reuseExistingShims) {
        const QString archive = QDir(scratchDir).filePath(QString::fromLatin1(kRtxBackendArchiveName));
        if (!mikai::backend::downloadToFile(QUrl(QString::fromLatin1(kRtxBackendUrl)), archive,
                kRtxBackendBytes, QString::fromLatin1(kRtxBackendSha),
                [&](qint64 got, qint64 total) {
                    if (progress) progress({QString::fromLatin1(kRtxBackendLabel), got, total,
                                            ComponentProvisionStage::Downloading});
                }, isCancelled, &error))
            return failed(QStringLiteral("animejanai.installation.download"), error, isCancelled());
        const QSet<QString> members{QStringLiteral("aji_trtrtx.dll")};
        if (!validateArchiveListing(sevenZip, archive, QString(), members, 1, &error))
            return failed(QStringLiteral("animejanai.installation.archive"), error);
        backendRoot = QDir(scratchDir).filePath(QStringLiteral("rtx-backend-extract"));
        if (!QDir().mkpath(backendRoot) ||
            !mikai::backend::extractArchive(sevenZip, archive, backendRoot, &error))
            return failed(QStringLiteral("animejanai.installation.extract"), error, isCancelled());
    }
#endif
    const QString parent = QFileInfo(installationRoot(backend)).absolutePath();
    QTemporaryDir stage(QDir(parent).filePath(QStringLiteral("aji-stage-XXXXXX")));
    if (!stage.isValid())
        return failed(QStringLiteral("animejanai.installation.stage"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "An AnimeJaNai staging directory could not be created.")));
#ifdef Q_OS_WIN
    const QString dispatcher = QStringLiteral("aji.dll");
    const QString selected = backendDll(backend);
#else
    const QString dispatcher = QStringLiteral("libaji.so");
    const QString selected = QStringLiteral("libaji_trt.so");
#endif
    for (const QString& name : {dispatcher, selected}) {
        const QString originRoot = name == dispatcher ? ajiRoot : backendRoot;
        const QString origin = QDir(originRoot).filePath(name);
        if (!noReparse(originRoot, origin) ||
            !copyFile(origin, stage.filePath(name), &error, isCancelled))
            return failed(QStringLiteral("animejanai.installation.dependency"), error, isCancelled());
    }
    auto result = provisionAnimeJanaiSharedRuntime(backend, stage.path(), progress, isCancelled);
    if (!result.ok) return result;
    bool heavy = false;
    if (progress) progress({ajiDispatcherLabel(), 0, 0, ComponentProvisionStage::Verifying});
    if (!recordAndVerify(stage.path(), backend, recordedClosureFiles(stage.path(), backend, nullptr),
                         QStringLiteral("official-download"), true, officialAcquisitions(backend),
                         &error, &heavy))
        return failed(QStringLiteral("animejanai.installation.invalid"), error, isCancelled());
    const auto dependencies = describeAnimeJanaiSharedRuntime(backend, stage.path(), true, isCancelled);
    if (!dependencies.installed)
        return failed(QStringLiteral("animejanai.installation.dependency"), dependencies.reason, isCancelled());
    if (isCancelled())
        return failed(QStringLiteral("animejanai.installation.cancelled"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "AnimeJaNai installation was cancelled.")), true);
    if (!publishStage(stage.path(), backend, &error))
        return failed(QStringLiteral("animejanai.installation.publish"), error);
    result.installedPacks.prepend(ajiDispatcherLabel());
    if (backend == Backend::TensorRTRTX)
        result.installedPacks.append(QString::fromLatin1(kRtxBackendLabel));
    if (progress) progress({ajiDispatcherLabel(), 0, 0, ComponentProvisionStage::Installed});
    return result;
}

} // namespace

AnimeJanaiInstallation describeAnimeJanaiInstallation(Backend backend, bool rehash) {
    AnimeJanaiInstallation result;
    result.root = installationRoot(backend);
    auto refuse = [&](const QString& key, const QString& text) { result.reasonKey = key; result.reason = text; return result; };
#ifndef Q_OS_WIN
    if (backend == Backend::DirectML)
        return refuse(QStringLiteral("animejanai.provision.platformUnsupported"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "DirectML is only available on Windows; select TensorRT on Linux.")));
    if (backend == Backend::TensorRTRTX)
        return refuse(QStringLiteral("animejanai.provision.platformUnsupported"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "TensorRT for RTX is available only on Windows.")));
#endif
    if (result.root.isEmpty()) return refuse(QStringLiteral("animejanai.installation.pathInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The app-owned AnimeJaNai runtime path is unavailable.")));
    const QFileInfo rootInfo(result.root);
    if (rootInfo.exists() && (!rootInfo.isDir() || rootInfo.isSymLink() ||
        !pathWithinRoot(nativeRoot(), result.root) ||
        !pathHasNoReparsePoints(nativeRoot(), result.root)))
        return refuse(QStringLiteral("animejanai.installation.pathInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The AnimeJaNai installation directory is not a safe app-owned path.")));
    const QString recordPath = QDir(result.root).filePath(QStringLiteral("install.json"));
    if (rootInfo.exists() && !noReparse(result.root, recordPath))
        return refuse(QStringLiteral("animejanai.installation.pathInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The AnimeJaNai installation record is a reparse point or unsafe path.")));
    QFile f(recordPath);
    if (!f.open(QIODevice::ReadOnly)) return refuse(QStringLiteral("animejanai.installation.recordMissing"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The app-owned AnimeJaNai installation record is missing.")));
    QJsonParseError parse{}; const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The AnimeJaNai installation record is corrupt.")));
    const QJsonObject rec = doc.object();
    const QStringList recordKeys{QStringLiteral("schema"), QStringLiteral("version"), QStringLiteral("backend"),
                                 QStringLiteral("provenance"), QStringLiteral("archiveBytes"), QStringLiteral("archiveSha256"),
                                 QStringLiteral("abi"), QStringLiteral("heavyRifeSupported"), QStringLiteral("files"),
                                 QStringLiteral("acquisitions")};
    for (const QString& key : recordKeys) {
        if (!rec.contains(key)) return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The AnimeJaNai installation record is missing a required field.")));
    }
    for (auto it = rec.constBegin(); it != rec.constEnd(); ++it) {
        if (!recordKeys.contains(it.key())) return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The AnimeJaNai installation record contains an unknown field.")));
    }
    if (rec.value(QStringLiteral("schema")).toString() != QLatin1String(kAjiSchema) ||
        rec.value(QStringLiteral("version")).toString() != QLatin1String(kAjiVersion) ||
        rec.value(QStringLiteral("backend")).toString() != backendName(backend) ||
        rec.value(QStringLiteral("abi")).toInt() != 9 ||
        !rec.value(QStringLiteral("heavyRifeSupported")).isBool() ||
        !rec.value(QStringLiteral("files")).isArray() ||
        !rec.value(QStringLiteral("acquisitions")).isArray()) {
        return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The AnimeJaNai installation record has an incompatible schema, backend, or ABI.")));
    }
    const QJsonArray files = rec.value(QStringLiteral("files")).toArray();
    const ComponentPack* resource = nullptr;
    bool linuxRecordClosure = false;
    for (const QJsonValue& item : files) {
        if (!item.isObject()) continue;
        if (item.toObject().value(QStringLiteral("path")).toString() == QLatin1String("libaji.so")) {
            linuxRecordClosure = true;
            break;
        }
    }
#ifdef Q_OS_WIN
    if (linuxRecordClosure)
        return refuse(QStringLiteral("animejanai.installation.platformMismatch"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The installed AnimeJaNai runtime is not a Windows x64 closure.")));
#else
    if (!linuxRecordClosure)
        return refuse(QStringLiteral("animejanai.installation.platformMismatch"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The installed AnimeJaNai runtime is not a Linux x64 closure.")));
#endif
    if (backend == Backend::TensorRTRTX) {
        // Upstream's dispatcher falls back to the classic TensorRT backend when
        // aji_trtrtx fails and logs "using TensorRT instead". A managed RTX root
        // must never contain that backend, or the requested backend would be
        // silently substituted after a re-import that copied extra files.
        if (QFileInfo::exists(QDir(result.root).filePath(QStringLiteral("aji_trt.dll"))))
            return refuse(QStringLiteral("animejanai.installation.fallbackBackend"),
                          translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The installed TensorRT for RTX runtime contains the classic TensorRT backend, which the dispatcher would silently fall back to.")));
    }
    const bool shared = isTensorRtFamily(backend) && hasSharedMapping(result.root);
    if (backend == Backend::TensorRT && !shared) {
        auto matchCatalog = [&](const QVector<ComponentPack>& catalog) -> bool {
            for (const QJsonValue& item : files) {
                if (!item.isObject()) continue;
                const QString path = item.toObject().value(QStringLiteral("path")).toString();
                for (const ComponentPack& pack : catalog) {
                    if (!pack.resourceFile.isEmpty() && pack.resourceFile == path) {
                        if (resource != nullptr && resource->resourceFile != pack.resourceFile)
                            return false;
                        resource = &pack;
                    }
                }
            }
            return true;
        };
        if (!matchCatalog(tensorRtComponentCatalog())) {
            return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The TensorRT closure records multiple builder resources.")));
        }
        if (resource == nullptr) return refuse(QStringLiteral("animejanai.installation.incomplete"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The TensorRT closure does not record a compatible builder resource.")));
        const GpuComputeCapability& capability = observeComputeCapability();
        const ComponentPack* selected = selectedTensorRtResource();
        if (capability.nvidia && (selected == nullptr || resource->name != selected->name))
            return refuse(QStringLiteral("animejanai.installation.resourceMismatch"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The installed TensorRT builder resource does not match the observed GPU.")));
    }
    if (shared && backend == Backend::TensorRT) {
        resource = selectedTensorRtResource();
        if (resource == nullptr)
            return refuse(QStringLiteral("animejanai.installation.gpuUnsupported"),
                          translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The observed GPU has no compatible TensorRT builder pack.")));
    }
    const QStringList required = shared ? sharedClosureFiles(backend, linuxRecordClosure)
                                       : closureFiles(backend, resource, linuxRecordClosure);
    const QStringList optional = optionalNoticeFiles(backend);
    const QString provenance = rec.value(QStringLiteral("provenance")).toString();
    const QJsonArray acquisitionRecords = rec.value(QStringLiteral("acquisitions")).toArray();
    if (provenance == QLatin1String("official-download") && isTensorRtFamily(backend) && !shared)
        return refuse(QStringLiteral("animejanai.installation.recordInvalid"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The managed TensorRT runtime is missing its shared-pack mapping.")));
    if ((provenance == QStringLiteral("local") &&
         (!acquisitionRecords.isEmpty() || rec.value(QStringLiteral("archiveBytes")).toVariant().toLongLong() != 0 ||
          !rec.value(QStringLiteral("archiveSha256")).toString().isEmpty())) ||
        (provenance == QStringLiteral("official-download") &&
         (rec.value(QStringLiteral("archiveBytes")).toVariant().toLongLong() != kAjiBytes ||
          rec.value(QStringLiteral("archiveSha256")).toString() != QLatin1String(kAjiSha) ||
          acquisitionRecords != officialAcquisitions(backend))) ||
        (provenance != QStringLiteral("local") && provenance != QStringLiteral("official-download")))
        return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The AnimeJaNai installation acquisition record does not match its pinned release sources.")));
    QMap<QString, QJsonObject> records;
    for (const QJsonValue& value : files) {
        if (!value.isObject()) return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The AnimeJaNai installation record contains an invalid file entry.")));
        const QJsonObject item = value.toObject();
        const QStringList fileKeys{QStringLiteral("path"), QStringLiteral("bytes"), QStringLiteral("sha256")};
        for (auto it = item.constBegin(); it != item.constEnd(); ++it) {
            if (!fileKeys.contains(it.key())) return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The AnimeJaNai installation record contains an unknown file field.")));
        }
        const QString name = item.value(QStringLiteral("path")).toString();
        QString why;
        const QString digest = item.value(QStringLiteral("sha256")).toString();
        if (safeRelativePath(name, &why).isEmpty() || name.contains(QLatin1Char('/')) || records.contains(name) ||
            item.value(QStringLiteral("bytes")).toVariant().toLongLong() <= 0 || digest.size() != 64 ||
            std::any_of(digest.cbegin(), digest.cend(), [](QChar c) { return !(c >= QLatin1Char('0') && c <= QLatin1Char('9')) && !(c >= QLatin1Char('a') && c <= QLatin1Char('f')); }))
            return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The AnimeJaNai installation record contains an unsafe or incomplete file entry.")));
        records.insert(name, item);
    }
    for (auto it = records.cbegin(); it != records.cend(); ++it) {
        if (!required.contains(it.key()) && !optional.contains(it.key()))
            return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The AnimeJaNai installation record includes a file outside the selected backend closure.")));
    }
    for (const QString& name : required) {
        if (!records.contains(name)) return refuse(QStringLiteral("animejanai.installation.incomplete"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The selected AnimeJaNai backend closure is incomplete: %1 is missing.")).arg(name));
    }
    QStringList verifyNames = required;
    for (const QString& name : optional) if (records.contains(name)) verifyNames.append(name);
    for (const QString& name : verifyNames) {
        const QString path = QDir(result.root).filePath(name); const QFileInfo info(path); const QJsonObject entry = records.value(name);
        if (!noReparse(result.root, path) || info.size() != entry.value(QStringLiteral("bytes")).toVariant().toLongLong()) return refuse(QStringLiteral("animejanai.installation.fileInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "An AnimeJaNai runtime file is missing, unsafe, or has changed size: %1")).arg(name));
        if (rehash && hashFile(path) != entry.value(QStringLiteral("sha256")).toString()) return refuse(QStringLiteral("animejanai.installation.fileInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "An AnimeJaNai runtime file failed its recorded SHA-256 check: %1")).arg(name));
        const QString officialDigest = pinnedOfficialAjiDigest(name);
        if (provenance == QLatin1String("official-download") &&
            !officialDigest.isEmpty() && entry.value(QStringLiteral("sha256")).toString() != officialDigest)
            return refuse(QStringLiteral("animejanai.installation.fileInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "An official Aji library does not match the pinned v0.27.0 release.")));
        if (isBinaryFileName(name) && !validBinaryImageX64(name, path)) return refuse(QStringLiteral("animejanai.installation.peInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "An AnimeJaNai runtime dependency is not a valid x64 binary image: %1")).arg(name));
#ifndef Q_OS_WIN
        if (name == QLatin1String("trtexec") && !info.isExecutable())
            return refuse(QStringLiteral("animejanai.installation.fileInvalid"),
                          translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The installed TensorRT trtexec is not executable.")));
#endif
    }
    if (records.size() < required.size() || records.size() > required.size() + optional.size())
        return refuse(QStringLiteral("animejanai.installation.recordInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The AnimeJaNai installation record does not describe the exact selected backend closure.")));
    result.heavyRifeSupported = rec.value(QStringLiteral("heavyRifeSupported")).toBool();
    if (provenance == QLatin1String("official-download") && result.heavyRifeSupported)
        return refuse(QStringLiteral("animejanai.installation.capabilityInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "Official Aji v0.27.0 does not map the heavy RIFE model.")));
    if (rehash && provenance == QLatin1String("local")) {
        int abi = 0; bool heavy = false;
        const QString dispPath = linuxRecordClosure
                                     ? QDir(result.root).filePath(QStringLiteral("libaji.so"))
                                     : QDir(result.root).filePath(QStringLiteral("aji.dll"));
        const bool hasVersionExport = runtimeExports(dispPath, &abi, &heavy);
        if ((hasVersionExport && abi != 9) ||
            (!hasVersionExport && !matchesPinnedOfficialAbi(result.root, backend)))
            return refuse(QStringLiteral("animejanai.installation.abiInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The installed AnimeJaNai dispatcher does not report ABI 9 and does not match the pinned official v0.27.0 x64 backend pair.")));
        if (!matchesCompatibleBackend(result.root, backend))
            return refuse(QStringLiteral("animejanai.installation.abiInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The installed AnimeJaNai backend does not report ABI 9 and does not match the pinned official v0.27.0 backend.")));
        if (!hasVersionExport) heavy = false;
        if (result.heavyRifeSupported != heavy)
            return refuse(QStringLiteral("animejanai.installation.capabilityInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The recorded AnimeJaNai capability does not match the dispatcher export.")));
    }
    result.localFilesPresent = true;
    if (shared) {
        const AnimeJanaiSharedRuntime runtime = describeAnimeJanaiSharedRuntime(backend, result.root, rehash);
        if (!runtime.installed)
            return refuse(QStringLiteral("animejanai.installation.sharedRuntime"), runtime.reason);
        result.sharedRuntime = true;
        result.backendLibDirs = runtime.libraryDirs;
        result.trtexecPath = runtime.trtexecPath;
    } else {
        result.backendLibDirs.append(result.root);
        if (backend == Backend::TensorRT) {
            result.trtexecPath = QDir(result.root).filePath(
#ifdef Q_OS_WIN
                QStringLiteral("trtexec.exe"));
#else
                QStringLiteral("trtexec"));
#endif
        }
    }
    result.installed = true;
    return result;
}

QVector<AnimeJanaiMissingDownload> animeJanaiMissingDownloads(Backend backend) {
#ifndef Q_OS_WIN
    if (backend == Backend::DirectML || backend == Backend::TensorRTRTX) return {};
#endif
    const AnimeJanaiInstallation installation = describeAnimeJanaiInstallation(backend, false);
    if (installation.installed) return {};
    QVector<AnimeJanaiMissingDownload> missing;
    if (!installation.localFilesPresent) missing.append({ajiDispatcherLabel(), kAjiBytes});
    if (isTensorRtFamily(backend)) {
        if (backend == Backend::TensorRTRTX && !installation.localFilesPresent)
            missing.append({QString::fromLatin1(kRtxBackendLabel), kRtxBackendBytes});
        missing.append(animeJanaiSharedRuntimeMissingDownloads(backend));
    } else {
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
    { QMutexLocker lock(&g_installMutex); if (g_installRunning) return failed(QStringLiteral("animejanai.installation.busy"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "An AnimeJaNai runtime installation is already running."))); g_installRunning = true; }
    const auto release = qScopeGuard([] { QMutexLocker lock(&g_installMutex); g_installRunning = false; });
    const auto isCancelled = [&] { return cancelled && cancelled(); };
    if (isCancelled()) return failed(QStringLiteral("animejanai.installation.cancelled"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "AnimeJaNai installation was cancelled.")), true);
#ifndef Q_OS_WIN
    if (backend == Backend::DirectML)
        return failed(QStringLiteral("animejanai.provision.platformUnsupported"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "DirectML is only available on Windows; select TensorRT on Linux.")));
    if (backend == Backend::TensorRTRTX)
        return failed(QStringLiteral("animejanai.provision.platformUnsupported"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "TensorRT for RTX is available only on Windows.")));
#endif
    const QFileInfo srcInfo(sourceDir); const QString sourceRoot = srcInfo.canonicalFilePath();
    const NativePathTrust sourceTrust = nativePathTrust(sourceRoot, true);
    if (sourceRoot.isEmpty() || !srcInfo.isDir() || !sourceTrust.trusted || sourceTrust.form != NativePathForm::Local) return failed(QStringLiteral("animejanai.installation.sourceInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The selected AnimeJaNai runtime directory is not a trusted local directory.")));
    const QString localRuntimeId = QStringLiteral("animejanai-local-runtime");
    const QString source = sourceRuntimeDirectory(sourceRoot);
    if (!pathHasNoReparsePoints(sourceRoot, source))
        return failed(QStringLiteral("animejanai.installation.sourceInvalid"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The selected AnimeJaNai inference directory is redirected or contains a reparse point.")));
#ifdef Q_OS_WIN
    if (directoryUsesLinuxClosure(source))
        return failed(QStringLiteral("animejanai.installation.platformMismatch"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The selected AnimeJaNai runtime is not a Windows x64 closure.")));
#else
    if (!directoryUsesLinuxClosure(source))
        return failed(QStringLiteral("animejanai.installation.platformMismatch"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The selected AnimeJaNai runtime is not a Linux x64 closure.")));
#endif
    const ComponentPack* resource = nullptr;
    if (backend == Backend::TensorRT) {
        QString resourceError;
        resource = resourceInDirectory(source, &resourceError);
        if (resource == nullptr) return failed(QStringLiteral("animejanai.installation.incomplete"), resourceError);
        const ComponentPack* selected = selectedTensorRtResource();
        const GpuComputeCapability& capability = observeComputeCapability();
        if (capability.nvidia && (selected == nullptr || resource->name != selected->name))
            return failed(QStringLiteral("animejanai.installation.resourceMismatch"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The imported TensorRT builder resource does not match the observed GPU.")));
    }
    const QString stageParent = QFileInfo(installationRoot(backend)).absolutePath();
    QString directoryError;
    if (!ensureOwnedNativeDirectory(stageParent, &directoryError))
        return failed(QStringLiteral("animejanai.installation.stage"), directoryError);
    QTemporaryDir temp(QDir(stageParent).filePath(QStringLiteral("aji-import-XXXXXX")));
    if (!temp.isValid()) return failed(QStringLiteral("animejanai.installation.stage"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "An AnimeJaNai staging directory could not be created.")));
    QStringList imported = recordedClosureFiles(source, backend, resource);
    qint64 totalBytes = 0;
    for (const QString& name : imported) totalBytes += QFileInfo(QDir(source).filePath(name)).size();
    qint64 copiedBytes = 0;
    if (progress) progress({localRuntimeId, 0, totalBytes, ComponentProvisionStage::Installing});
    for (const QString& name : imported) {
        if (cancelled && cancelled()) return failed(QStringLiteral("animejanai.installation.cancelled"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "AnimeJaNai installation was cancelled.")), true);
        const QString input = QDir(source).filePath(name);
        if (!noReparse(source, input)) return failed(QStringLiteral("animejanai.installation.incomplete"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The selected runtime directory is missing a required safe file: %1")).arg(name));
        QString error;
        if (!copyFile(input, temp.filePath(name), &error, cancelled, [&](qint64 count) {
                copiedBytes += count;
                if (progress) progress({QStringLiteral("animejanai-local-runtime"), copiedBytes, totalBytes,
                                        ComponentProvisionStage::Installing});
            })) {
            const bool wasCancelled = isCancelled();
            return failed(wasCancelled ? QStringLiteral("animejanai.installation.cancelled") : QStringLiteral("animejanai.installation.copy"),
                          wasCancelled ? translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "AnimeJaNai installation was cancelled.")) : error, wasCancelled);
        }
    }
    bool heavy = false; QString error;
    if (progress) progress({localRuntimeId, copiedBytes, totalBytes, ComponentProvisionStage::Verifying});
    if (!recordAndVerify(temp.path(), backend, imported, QStringLiteral("local"), true, {}, &error, &heavy)) return failed(QStringLiteral("animejanai.installation.invalid"), error);
    if (cancelled && cancelled()) return failed(QStringLiteral("animejanai.installation.cancelled"), translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "AnimeJaNai installation was cancelled.")), true);
    if (!publishStage(temp.path(), backend, &error)) return failed(QStringLiteral("animejanai.installation.publish"), error);
    if (progress) progress({localRuntimeId, copiedBytes, totalBytes, ComponentProvisionStage::Installed});
    ComponentProvisionResult result; result.ok = true; result.installedPacks << QStringLiteral("aji-v0.27.0");
    result.installedPacks << (backend == Backend::TensorRTRTX ? QString::fromLatin1(kRtxBackendLabel)
                                                              : backendName(backend));
    return result;
}

ComponentProvisionResult provisionAnimeJanaiRuntime(Backend backend,
    const std::function<void(const ComponentProvisionProgress&)>& progress,
    const std::function<bool()>& cancelled) {
    {
        QMutexLocker lock(&g_installMutex);
        if (g_installRunning)
            return failed(QStringLiteral("animejanai.installation.busy"),
                          translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "An AnimeJaNai runtime installation is already running.")));
        g_installRunning = true;
    }
    const auto releaseRun = qScopeGuard([] {
        QMutexLocker lock(&g_installMutex);
        g_installRunning = false;
    });
#ifndef Q_OS_WIN
    if (backend == Backend::DirectML)
        return failed(QStringLiteral("animejanai.provision.platformUnsupported"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "DirectML is only available on Windows; select TensorRT on Linux.")));
    if (backend == Backend::TensorRTRTX)
        return failed(QStringLiteral("animejanai.provision.platformUnsupported"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "TensorRT for RTX is available only on Windows.")));
#endif
    const auto isCancelled = [&] {
        return (cancelled && cancelled()) ||
               mikai::backend::WorkCancellationScope::cancellationRequested();
    };
    if (isCancelled())
        return failed(QStringLiteral("animejanai.installation.cancelled"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "AnimeJaNai installation was cancelled.")), true);
    const AnimeJanaiInstallation current = describeAnimeJanaiInstallation(backend, true);
    if (current.installed) {
        ComponentProvisionResult result;
        result.ok = true;
        result.installedPacks.append(ajiDispatcherLabel());
        if (backend == Backend::TensorRTRTX)
            result.installedPacks.append(QString::fromLatin1(kRtxBackendLabel));
        return result;
    }
    if (backend == Backend::TensorRT && selectedTensorRtResource() == nullptr)
        return failed(QStringLiteral("animejanai.provision.gpuUnsupported"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "The observed GPU has no compatible TensorRT builder pack.")));

    const QString scratchRoot = nativeSubdirectory(QStringLiteral("scratch"));
    QString directoryError;
    if (!ensureOwnedNativeDirectory(scratchRoot, &directoryError))
        return failed(QStringLiteral("animejanai.installation.scratch"), directoryError);
    QTemporaryDir scratch(QDir(scratchRoot).filePath(QStringLiteral("aji-runtime-XXXXXX")));
    if (!scratch.isValid())
        return failed(QStringLiteral("animejanai.installation.scratch"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "A temporary AnimeJaNai acquisition directory could not be created.")));
    QString sevenZip;
    try {
        sevenZip = mikai::backend::ensureSevenZip(nativeToolsDirectory());
    } catch (const std::exception& error) {
        return failed(QStringLiteral("animejanai.installation.extractor"),
                      QString::fromLocal8Bit(error.what()), isCancelled());
    }
    if (sevenZip.isEmpty())
        return failed(QStringLiteral("animejanai.installation.extractor"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "7-Zip is not available to unpack the AnimeJaNai runtime.")));

#ifdef Q_OS_WIN
    const QStringList shimNames{QStringLiteral("aji.dll"), backendDll(backend)};
#else
    const QStringList shimNames{QStringLiteral("libaji.so"), QStringLiteral("libaji_trt.so")};
#endif
    bool reuseExistingShims = !current.root.isEmpty();
    for (const QString& name : shimNames)
        reuseExistingShims = reuseExistingShims && noReparse(current.root, QDir(current.root).filePath(name));
    reuseExistingShims = reuseExistingShims && matchesPinnedOfficialAbi(current.root, backend);
    QString ajiRoot = current.root;
    QString error;
    if (!reuseExistingShims) {
        const QString archive = scratch.filePath(QString::fromLatin1(kAjiArchiveName));
        if (!mikai::backend::downloadToFile(QUrl(QString::fromLatin1(kAjiUrl)), archive,
                kAjiBytes, QString::fromLatin1(kAjiSha),
                [&](qint64 got, qint64 total) {
                    if (progress) progress({ajiDispatcherLabel(), got, total, ComponentProvisionStage::Downloading});
                }, isCancelled, &error))
            return failed(QStringLiteral("animejanai.installation.download"), error, isCancelled());
        if (progress) progress({ajiDispatcherLabel(), 0, 0, ComponentProvisionStage::Extracting});
#ifdef Q_OS_WIN
        const QSet<QString> members{QStringLiteral("aji.dll"), QStringLiteral("aji_trt.dll"),
            QStringLiteral("aji_dml.dll"), QStringLiteral("aji_encode.exe"),
            QStringLiteral("aji_harness_dml.exe"), QStringLiteral("aji_harness.exe"),
            QStringLiteral("aji_kernel_test.exe")};
        if (!validateArchiveListing(sevenZip, archive, QString(), members, 7, &error))
            return failed(QStringLiteral("animejanai.installation.archive"), error);
#else
        const QSet<QString> members{QStringLiteral("libaji.so"), QStringLiteral("libaji_trt.so"),
            QStringLiteral("aji_harness"), QStringLiteral("aji_kernel_test")};
        if (!validateSafeArchive(sevenZip, archive, &error, false, isCancelled, members))
            return failed(QStringLiteral("animejanai.installation.archive"), error, isCancelled());
#endif
        const QString extracted = scratch.filePath(QStringLiteral("aji-extract"));
        if (!QDir().mkpath(extracted) ||
            !mikai::backend::extractArchive(sevenZip, archive, extracted, &error))
            return failed(QStringLiteral("animejanai.installation.extract"), error, isCancelled());
        ajiRoot = sourceRuntimeDirectory(extracted);
    }
    if (isCancelled())
        return failed(QStringLiteral("animejanai.installation.cancelled"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "AnimeJaNai installation was cancelled.")), true);
    const QString parent = QFileInfo(installationRoot(backend)).absolutePath();
    if (!ensureOwnedNativeDirectory(parent, &error))
        return failed(QStringLiteral("animejanai.installation.stage"), error);
    if (isTensorRtFamily(backend))
        return provisionSharedRuntime(scratch.path(), sevenZip, ajiRoot, backend,
                                      reuseExistingShims, progress, isCancelled);

    constexpr auto kOrtSha = "57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681";
    constexpr auto kDirectMlSha = "4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9";
    const QString ortUrl = QStringLiteral("https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg");
    const QString directMlUrl = QStringLiteral("https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg");
    const QString ortArchive = scratch.filePath(QStringLiteral("onnxruntime-directml.nupkg"));
    const QString directMlArchive = scratch.filePath(QStringLiteral("directml.nupkg"));
    for (const auto& asset : {std::make_pair(ortUrl, std::make_pair(kOrtBytes, QString::fromLatin1(kOrtSha))),
                             std::make_pair(directMlUrl, std::make_pair(kDirectMlBytes, QString::fromLatin1(kDirectMlSha)))}) {
        const QString path = asset.first == ortUrl ? ortArchive : directMlArchive;
        const QString name = asset.first == ortUrl ? QStringLiteral("onnxruntime-directml-1.24.4")
                                                   : QStringLiteral("directml-1.15.4");
        if (!mikai::backend::downloadToFile(QUrl(asset.first), path, asset.second.first, asset.second.second,
                [&](qint64 got, qint64 total) {
                    if (progress) progress({name, got, total, ComponentProvisionStage::Downloading});
                }, isCancelled, &error))
            return failed(QStringLiteral("animejanai.installation.download"), error, isCancelled());
        if (!validateSafeArchive(sevenZip, path, &error, false, isCancelled))
            return failed(QStringLiteral("animejanai.installation.archive"), error, isCancelled());
    }
    const QString ortExtract = scratch.filePath(QStringLiteral("ort-extract"));
    const QString directMlExtract = scratch.filePath(QStringLiteral("directml-extract"));
    if (!mikai::backend::extractArchive(sevenZip, ortArchive, ortExtract, &error) ||
        !mikai::backend::extractArchive(sevenZip, directMlArchive, directMlExtract, &error))
        return failed(QStringLiteral("animejanai.installation.extract"), error, isCancelled());
    QTemporaryDir stage(QDir(parent).filePath(QStringLiteral("aji-stage-XXXXXX")));
    if (!stage.isValid())
        return failed(QStringLiteral("animejanai.installation.stage"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "An AnimeJaNai staging directory could not be created.")));
    for (const QString& name : shimNames) {
        const QString source = QDir(ajiRoot).filePath(name);
        if (!noReparse(ajiRoot, source) || !copyFile(source, stage.filePath(name), &error, isCancelled))
            return failed(QStringLiteral("animejanai.installation.dependency"), error, isCancelled());
    }
    const QStringList staged{QStringLiteral("onnxruntime.dll"), QStringLiteral("DirectML.dll"),
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
    for (qsizetype index = 0; index < staged.size(); ++index) {
        const bool optional = index >= 3;
        if (optional && !QFileInfo::exists(origins[index])) continue;
        const QString& originRoot = index == 0 || index == 3 || index == 4 ? ortExtract : directMlExtract;
        if (!noReparse(originRoot, origins[index]) ||
            !copyFile(origins[index], stage.filePath(staged[index]), &error, isCancelled))
            return failed(QStringLiteral("animejanai.installation.dependency"), error, isCancelled());
    }
    bool heavy = false;
    if (!recordAndVerify(stage.path(), backend, recordedClosureFiles(stage.path(), backend, nullptr),
                         QStringLiteral("official-download"), true, officialAcquisitions(backend),
                         &error, &heavy))
        return failed(QStringLiteral("animejanai.installation.invalid"), error, isCancelled());
    if (isCancelled())
        return failed(QStringLiteral("animejanai.installation.cancelled"),
                      translated(QT_TRANSLATE_NOOP("AnimeJanaiInstallation", "AnimeJaNai installation was cancelled.")), true);
    if (!publishStage(stage.path(), backend, &error))
        return failed(QStringLiteral("animejanai.installation.publish"), error);
    ComponentProvisionResult result;
    result.ok = true;
    result.installedPacks = {ajiDispatcherLabel(), QStringLiteral("onnxruntime-directml-1.24.4"),
                             QStringLiteral("directml-1.15.4")};
    if (progress)
        for (const QString& name : result.installedPacks)
            progress({name, 0, 0, ComponentProvisionStage::Installed});
    return result;
}

QString detail::officialAjiDigest(const QString& fileName) { return pinnedOfficialAjiDigest(fileName); }

} // namespace mplayer::native
