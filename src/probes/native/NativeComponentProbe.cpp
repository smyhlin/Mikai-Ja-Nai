// Hand-run probe for the GPU-adaptive TensorRT component path. It reports the
// observed device, the pack the catalogue selects for it, and what the app-owned
// component store holds; `--fetch` pre-populates the store; `--from-sdk` proves
// the selection decides whether a real engine build succeeds. Nothing here is
// part of the app: it is the falsifiable check for the pack choice.

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>
#include <optional>

#include "native/bundle/NativeBundle.hpp"
#include "native/components/NativeComponentCatalog.hpp"
#include "native/components/NativeGpuProbe.hpp"
#include "native/components/NativeComponentProvisioning.hpp"
#include "native/components/NativeComponentStore.hpp"
#include "native/components/NativeComponentToolchain.hpp"

using namespace mplayer::native;

namespace {

void report(bool ok, const char* detail) {
    std::printf("RESULT: %s (%s)\n", ok ? "PASS" : "FAIL", detail);
}

QString hashFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty()) break;
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}

const ComponentPack* packByName(const QString& name) {
    for (const ComponentPack& pack : tensorRtComponentCatalog()) {
        if (pack.name == name) return &pack;
    }
    return nullptr;
}

void printPack(const ComponentPack* pack) {
    if (pack == nullptr) {
        std::printf("  pack: none\n");
        return;
    }
    std::printf("  pack: %s  %s  %lld bytes\n", qPrintable(pack->name), qPrintable(pack->asset),
                static_cast<long long>(pack->assetBytes));
    std::printf("  url:  %s\n", qPrintable(pack->url));
}

QString bundleRootFor(const QString& overrideRoot) {
    if (!overrideRoot.isEmpty()) return overrideRoot;
    for (const Bundle& bundle : discoverBundles()) {
        if (bundle.kind == BundleKind::Native) return bundle.root;
    }
    return {};
}

int runPrint(const QString& bundleOverride) {
    const GpuComputeCapability& gpu = observeComputeCapability();
    std::printf("gpu: %s\n", qPrintable(computeCapabilityText(gpu)));
    const ComponentPack* resource = tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
    printPack(resource);

    const ComponentStore store = componentStore();
    std::printf("store: %s\n", qPrintable(store.root));
    for (const ComponentPack* pack : {tensorRtRuntimePack(), resource}) {
        if (pack == nullptr) continue;
        const StoredPack stored = storedPack(store, *pack);
        std::printf("  %s installed=%d bytes=%lld %s\n", qPrintable(pack->name),
                    stored.installed ? 1 : 0, static_cast<long long>(stored.bytes),
                    qPrintable(stored.reason));
    }

    const QString bundleRoot = bundleRootFor(bundleOverride);
    QStringList declaredTrtFiles;
    QString trtDirName;
    QString cudaDirName;
    // An explicit root is read as a bundle in its own right, so the fast path
    // this probe exists to check is reachable without installing anything under
    // the probe's own application identity. Discovery is the fallback.
    std::optional<Bundle> described;
    if (!bundleOverride.isEmpty()) {
        QString readReason;
        described = readBundle(bundleRoot, &readReason);
        if (!described.has_value()) {
            std::printf("bundle unreadable: %s\n", qPrintable(readReason));
        }
    }
    if (!described.has_value()) {
        for (const Bundle& bundle : discoverBundles()) {
            if (bundle.kind != BundleKind::Native || bundle.root != bundleRoot) continue;
            described = bundle;
            break;
        }
    }
    if (described.has_value() && described->preparation.has_value()) {
        trtDirName = described->preparation->runtime.trt;
        cudaDirName = described->preparation->runtime.cuda;
        const QString prefix = trtDirName + QLatin1Char('/');
        for (const BundleFile& file : described->files) {
            if (file.path.startsWith(prefix)) declaredTrtFiles.append(file.path);
        }
    }
    std::printf("declared trt files: %lld\n", static_cast<long long>(declaredTrtFiles.size()));
    const ComponentBinDirs dirs = componentBinDirs(bundleRoot, trtDirName, cudaDirName,
                                                   declaredTrtFiles, gpu, store);
    std::printf("bundle: %s\n", qPrintable(bundleRoot));
    std::printf("trtBinDir: %s\n", qPrintable(dirs.trtBinDir));
    std::printf("cudaBinDir: %s\n", qPrintable(dirs.cudaBinDir));
    std::printf("fromStore=%d pack=%s\n", dirs.fromStore ? 1 : 0, qPrintable(dirs.packName));
    if (!dirs.reasonKey.isEmpty()) std::printf("refusal: %s: %s\n", qPrintable(dirs.reasonKey),
                                               qPrintable(dirs.reason));
    return 0;
}

int runFetch(const QString& names) {
    QVector<ComponentPack> packs;
    for (const QString& name : names.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const ComponentPack* pack = packByName(name.trimmed());
        if (pack == nullptr) {
            std::printf("unknown pack '%s'\n", qPrintable(name));
            report(false, "unknown pack");
            return 1;
        }
        packs.append(*pack);
    }
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        report(false, "no scratch directory");
        return 1;
    }
    const ComponentStore store = componentStore();
    const ComponentProvisionResult result = provisionTensorRtComponents(
        store, packs, scratch.path(), nullptr, nullptr);
    if (!result.ok) {
        std::printf("provisioning refused: %s: %s\n", qPrintable(result.reasonKey),
                    qPrintable(result.reason));
        report(false, "provisioning failed");
        return 1;
    }
    const QString directory = componentReleaseDirectory(store);
    for (const ComponentPack& pack : packs) {
        for (const QString& name : pack.files) {
            const QString path = QDir(directory).filePath(name);
            const QFileInfo info(path);
            std::printf("%s  %lld bytes  sha256 %s\n", qPrintable(name),
                        static_cast<long long>(info.size()), qPrintable(hashFile(path)));
        }
    }
    report(true, "packs installed");
    return 0;
}

int runFromSdk(const QString& sdkDir, const QString& packName, const QString& helper,
               const QString& graph, const QString& out, const QString& shapes,
               const QString& uuid, bool describeOnly) {
    const ComponentPack* pack = packName.isEmpty()
                                    ? tensorRtResourcePackFor(observeComputeCapability().smMajor,
                                                              observeComputeCapability().smMinor)
                                    : packByName(packName);
    if (pack == nullptr) {
        report(false, "no pack selected");
        return 1;
    }
    QTemporaryDir binDir;
    if (!binDir.isValid()) {
        report(false, "no scratch bin directory");
        return 1;
    }
    // The helper links the runtime at load time, so the scratch bin directory
    // stages the runtime set first and the measured resource on top: the only
    // pairing the acquisition record measured as working. A load that names a
    // different DLL here means the naming is wrong, not that the pack is. The
    // TensorRT-only SDK carries no CUDA runtime of its own, so a file the SDK
    // directory lacks falls back to the app-owned store - which holds the same
    // pinned pack set after `--fetch` - rather than failing the run for a
    // transport detail the choice under test does not depend on.
    QStringList staged = tensorRtRuntimePack()->files + pack->files;
    staged.removeDuplicates();
    const QString storeDir = componentReleaseDirectory(componentStore());
    for (const QString& name : staged) {
        QString source = QDir(sdkDir).filePath(name);
        QString origin = QStringLiteral("sdk");
        if (!QFileInfo::exists(source)) {
            source = QDir(storeDir).filePath(name);
            origin = QStringLiteral("store");
        }
        if (!QFileInfo::exists(source)) {
            std::printf("missing in SDK and store: %s\n", qPrintable(name));
            report(false, "SDK bin directory is incomplete");
            return 1;
        }
        std::printf("staging %s (from %s)\n", qPrintable(name), qPrintable(origin));
        if (!QFile::copy(source, QDir(binDir.path()).filePath(name))) {
            report(false, "could not stage the pack file");
            return 1;
        }
    }

    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString systemRoot = environment.value(QStringLiteral("SystemRoot"));
    environment.insert(QStringLiteral("PATH"),
                       QStringLiteral("%1;%2/System32;%2").arg(binDir.path(), systemRoot));
    process.setProcessEnvironment(environment);
    process.setProgram(helper);
    // The builder refuses without the device it is building for; an empty uuid
    // leaves the choice to the helper, which is how the describe path is used.
    QStringList helperArgs = describeOnly ? QStringList{QStringLiteral("--describe-device"),
                                                        QStringLiteral("--json")}
                                          : QStringList{QStringLiteral("--onnx"), graph,
                                                        QStringLiteral("--out"), out,
                                                        QStringLiteral("--shapes"), shapes};
    if (!uuid.isEmpty()) {
        helperArgs << QStringLiteral("--device-uuid") << uuid;
    }
    process.setArguments(helperArgs);
    process.start();
    if (!process.waitForStarted(30000)) {
        report(false, "helper did not start");
        return 1;
    }
    process.waitForFinished(600000);
    const QByteArray combined = process.readAllStandardOutput() + process.readAllStandardError();
    std::printf("%s\n", combined.right(2000).constData());
    const int exitCode = process.exitCode();
    std::printf("helper exit: %d\n", exitCode);
    report(exitCode == 0, exitCode == 0 ? "helper succeeded" : "helper failed");
    return exitCode == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    QStringList arguments;
    for (int index = 1; index < argc; ++index) arguments.append(QString::fromLocal8Bit(argv[index]));
    const auto valueOf = [&arguments](const QString& flag) -> QString {
        const int index = arguments.indexOf(flag);
        return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1) : QString();
    };

    if (arguments.contains(QStringLiteral("--fetch"))) {
        return runFetch(valueOf(QStringLiteral("--fetch")));
    }
    if (arguments.contains(QStringLiteral("--from-sdk"))) {
        return runFromSdk(valueOf(QStringLiteral("--from-sdk")),
                          valueOf(QStringLiteral("--pack")), valueOf(QStringLiteral("--helper")),
                          valueOf(QStringLiteral("--graph")), valueOf(QStringLiteral("--out")),
                          valueOf(QStringLiteral("--shapes")), valueOf(QStringLiteral("--uuid")),
                          arguments.contains(QStringLiteral("--describe-only")));
    }
    return runPrint(valueOf(QStringLiteral("--bundle")));
}
