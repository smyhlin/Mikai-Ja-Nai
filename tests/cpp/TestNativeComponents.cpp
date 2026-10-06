#include "TestNativeComponents.hpp"

#include "native/components/NativeComponentCatalog.hpp"
#include "native/components/NativeComponentStore.hpp"
#include "native/components/NativeComponentToolchain.hpp"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>

using namespace mplayer::native;

namespace {

constexpr auto kRecordFile = "component.json";
constexpr auto kRecordSchema = "mikai.native.components/1";

bool isSha256Hex(const QString& value) {
    if (value.size() != 64) return false;
    for (const QChar& c : value) {
        const bool digit = c >= QLatin1Char('0') && c <= QLatin1Char('9');
        const bool lowerHex = c >= QLatin1Char('a') && c <= QLatin1Char('f');
        if (!digit && !lowerHex) return false;
    }
    return true;
}

QByteArray filePayload(const QString& seed) {
    return QByteArrayLiteral("component fixture bytes for ") + seed.toUtf8() + "\n";
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(bytes) == bytes.size();
}

QString sha256Of(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QJsonObject packObject(const ComponentPack& pack) {
    QJsonObject object;
    object.insert(QStringLiteral("name"), pack.name);
    object.insert(QStringLiteral("asset"), pack.asset);
    object.insert(QStringLiteral("assetBytes"), pack.assetBytes);
    object.insert(QStringLiteral("assetSha256"), pack.assetSha256);
    object.insert(QStringLiteral("url"), pack.url);
    return object;
}

QJsonObject recordObject(const QVector<ComponentPack>& packs, const QJsonArray& files) {
    QJsonArray packArray;
    for (const ComponentPack& pack : packs) packArray.append(packObject(pack));
    QJsonObject root;
    root.insert(QStringLiteral("schema"), QString::fromLatin1(kRecordSchema));
    root.insert(QStringLiteral("release"), componentReleaseTag());
    root.insert(QStringLiteral("packs"), packArray);
    root.insert(QStringLiteral("files"), files);
    return root;
}

bool writeRecord(const ComponentStore& store, const QJsonObject& root) {
    const QString directory = componentReleaseDirectory(store);
    if (!QDir().mkpath(directory)) return false;
    return writeBytes(QDir(directory).filePath(QString::fromLatin1(kRecordFile)),
                      QJsonDocument(root).toJson(QJsonDocument::Compact));
}

/// Publishes the given packs into the store the way the installer would: every
/// declared file exists on disk at the size the record states, under the release
/// directory as a flat basename.
bool populateStore(const ComponentStore& store, const QVector<ComponentPack>& packs) {
    const QString directory = componentReleaseDirectory(store);
    if (!QDir().mkpath(directory)) return false;
    QJsonArray files;
    for (const ComponentPack& pack : packs) {
        for (const QString& name : pack.files) {
            const QByteArray payload = filePayload(name);
            if (!writeBytes(QDir(directory).filePath(name), payload)) return false;
            QJsonObject file;
            file.insert(QStringLiteral("path"), name);
            file.insert(QStringLiteral("bytes"), static_cast<qint64>(payload.size()));
            file.insert(QStringLiteral("sha256"), sha256Of(payload));
            files.append(file);
        }
    }
    return writeRecord(store, recordObject(packs, files));
}

const ComponentPack* packNamed(const QString& name) {
    const QVector<ComponentPack>& catalog = tensorRtComponentCatalog();
    for (const ComponentPack& pack : catalog) {
        if (pack.name == name) return &pack;
    }
    return nullptr;
}

QString resourceNameOf(const ComponentPack* pack) {
    return pack == nullptr ? QString() : builderResourceFileName(*pack);
}

}  // namespace

void TestNativeComponents::selectionUsesTheExactPack() {
    struct Generation {
        int smMajor;
        int smMinor;
        const char* pack;
    };
    const Generation generations[] = {{12, 0, "trt-sm120"}, {7, 5, "trt-sm75"}, {8, 6, "trt-sm86"},
                                     {9, 0, "trt-sm90"},  {10, 0, "trt-sm100"}};
    for (const Generation& generation : generations) {
        const ComponentPack* selected =
            tensorRtResourcePackFor(generation.smMajor, generation.smMinor);
        QVERIFY2(selected != nullptr, generation.pack);
        QCOMPARE(selected->name, QString::fromLatin1(generation.pack));
        QCOMPARE(resourceNameOf(selected),
                 QStringLiteral("nvinfer_builder_resource_sm%1%2_11.dll")
                     .arg(generation.smMajor)
                     .arg(generation.smMinor));
    }
}

void TestNativeComponents::selectionFallsBackToPtxWhenTheReleasePublishesNoPack() {
    struct Shape {
        int smMajor;
        int smMinor;
    };
    const Shape shapes[] = {{6, 1}, {10, 1}};
    for (const Shape& shape : shapes) {
        const ComponentPack* selected = tensorRtResourcePackFor(shape.smMajor, shape.smMinor);
        QVERIFY(selected != nullptr);
        QCOMPARE(selected->name, QStringLiteral("trt-ptx"));
        QCOMPARE(selected->resourceFile, QStringLiteral("nvinfer_builder_resource_ptx_11.dll"));
    }
}

void TestNativeComponents::selectionRefusesADoubleDigitMinor() {
    // `trt-sm1210` is a name no release publishes, and approximating it onto
    // trt-sm120 would hand another generation's builder resource to this GPU.
    QVERIFY(tensorRtResourcePackFor(12, 10) == nullptr);
    QVERIFY(tensorRtResourcePackFor(0, 0) == nullptr);
}

static QStringList packNameOf(const QVector<const ComponentPack*>& packs) {
    QStringList names;
    for (const ComponentPack* pack : packs) {
        if (pack != nullptr) names.append(pack->name);
    }
    return names;
}

void TestNativeComponents::compatiblePacksNameRuntimeAndThisGpuOnly() {
    QCOMPARE(packNameOf(compatiblePacksFor(12, 0)),
             QStringList({QStringLiteral("trt-runtime"), QStringLiteral("trt-sm120")}));
    QCOMPARE(packNameOf(compatiblePacksFor(10, 1)),
             QStringList({QStringLiteral("trt-runtime"), QStringLiteral("trt-ptx")}));
}

void TestNativeComponents::compatiblePacksAreEmptyWhenTheShapeIsUnusable() {
    QVERIFY(compatiblePacksFor(0, 0).isEmpty());
    QVERIFY(compatiblePacksFor(12, 10).isEmpty());
}

void TestNativeComponents::catalogueDigestsAndSizesAreWellFormed() {
    const QVector<ComponentPack>& catalog = tensorRtComponentCatalog();
    QVERIFY(!catalog.isEmpty());
    QVERIFY(packNamed(QStringLiteral("trt-runtime")) != nullptr);
    for (const ComponentPack& pack : catalog) {
        QVERIFY2(!pack.name.isEmpty(), qPrintable(pack.asset));
        QVERIFY2(isSha256Hex(pack.assetSha256), qPrintable(pack.name));
        QVERIFY2(pack.assetBytes > 0, qPrintable(pack.name));
        QVERIFY2(pack.url.contains(QLatin1String("github.com")), qPrintable(pack.url));
        QVERIFY2(pack.url.contains(QStringLiteral("/%1/").arg(componentReleaseTag())),
                 qPrintable(pack.url));
        QVERIFY2(!pack.files.isEmpty(), qPrintable(pack.name));
        for (const QString& file : pack.files) {
            QVERIFY2(!file.contains(QLatin1Char('/')), qPrintable(file));
        }
        if (!pack.resourceFile.isEmpty()) {
            QVERIFY2(pack.files.contains(pack.resourceFile), qPrintable(pack.resourceFile));
        }
        if (pack.name == QLatin1String("trt-runtime")) {
            // Nothing in this build invokes trtexec.exe, and the bundle
            // inventory cannot describe an executable the manifest does not list.
            QVERIFY(pack.resourceFile.isEmpty());
            QVERIFY(!pack.files.contains(QStringLiteral("trtexec.exe")));
        }
    }

    // The exact lists, not merely non-empty ones. A walk over the row's file
    // slots that ran past their end used to append whatever pointers sat beside
    // the table, and the runtime row is the one that fills every slot, so the
    // count is the assertion that catches it.
    QCOMPARE(packNamed(QStringLiteral("trt-runtime"))->files,
             QStringList({QStringLiteral("cudart64_13.dll"), QStringLiteral("CUDA_LICENSE.txt"),
                          QStringLiteral("DirectML_LICENSE.txt"), QStringLiteral("nvinfer_11.dll"),
                          QStringLiteral("nvinfer_plugin_11.dll"),
                          QStringLiteral("nvonnxparser_11.dll")}));
    for (const ComponentPack& pack : catalog) {
        if (pack.resourceFile.isEmpty()) continue;
        QCOMPARE(pack.files, QStringList({pack.resourceFile}));
    }
}

void TestNativeComponents::storeRecordRoundTrips() {
    const ComponentPack* pack = packNamed(QStringLiteral("trt-sm120"));
    QVERIFY(pack != nullptr);

    QTemporaryDir extraction;
    QVERIFY(extraction.isValid());
    for (const QString& name : pack->files) {
        QVERIFY(writeBytes(QDir(extraction.path()).filePath(
                               QStringLiteral("animejanai/inference/") + name),
                           filePayload(name)));
    }

    QTemporaryDir store;
    QVERIFY(store.isValid());
    const ComponentStore target{store.path()};

    QString reasonKey;
    QString reason;
    QVERIFY2(installPack(target, *pack, extraction.path(), &reasonKey, &reason),
             qPrintable(reasonKey + QLatin1String(": ") + reason));

    const StoredPack installed = storedPack(target, *pack);
    QVERIFY2(installed.installed, qPrintable(installed.reason));
    QCOMPARE(installed.pack, pack->name);
    QCOMPARE(installed.directory, componentReleaseDirectory(target));
    QVERIFY(installed.bytes > 0);
    QVERIFY(installed.reasonKey.isEmpty());

    // One byte-level absence under the record: the same record now describes a
    // directory that cannot serve the pack.
    const QString victim = QDir(componentReleaseDirectory(target)).filePath(pack->files.first());
    QVERIFY(QFile::remove(victim));
    const StoredPack damaged = storedPack(target, *pack);
    QVERIFY(!damaged.installed);
    QVERIFY(!damaged.reason.isEmpty());
}

void TestNativeComponents::storeRefusesAnUnknownRecordKey() {
    QTemporaryDir store;
    QVERIFY(store.isValid());
    const ComponentStore target{store.path()};

    QJsonObject root = recordObject(QVector<ComponentPack>(), QJsonArray());
    root.insert(QStringLiteral("installedBy"), QStringLiteral("something-else"));
    QVERIFY(writeRecord(target, root));

    const ComponentPack* pack = packNamed(QStringLiteral("trt-sm120"));
    QVERIFY(pack != nullptr);
    const StoredPack result = storedPack(target, *pack);
    QVERIFY(!result.installed);
    QCOMPARE(result.reasonKey, QStringLiteral("native.components.record"));
}

void TestNativeComponents::storeRefusesAnUnsafeRecordPath() {
    QTemporaryDir store;
    QVERIFY(store.isValid());
    const ComponentStore target{store.path()};

    const QByteArray payload = filePayload(QStringLiteral("evil.dll"));
    QJsonObject file;
    file.insert(QStringLiteral("path"), QStringLiteral("../evil.dll"));
    file.insert(QStringLiteral("bytes"), static_cast<qint64>(payload.size()));
    file.insert(QStringLiteral("sha256"), sha256Of(payload));
    QJsonArray files;
    files.append(file);
    QVERIFY(writeRecord(target, recordObject(QVector<ComponentPack>(), files)));

    const ComponentPack* pack = packNamed(QStringLiteral("trt-sm120"));
    QVERIFY(pack != nullptr);
    const StoredPack result = storedPack(target, *pack);
    QVERIFY(!result.installed);
    QCOMPARE(result.reasonKey, QStringLiteral("native.components.record"));
}

void TestNativeComponents::toolchainPrefersTheBundleResource() {
    QTemporaryDir bundle;
    QVERIFY(bundle.isValid());
    QTemporaryDir store;
    QVERIFY(store.isValid());

    GpuComputeCapability gpu;
    gpu.nvidia = true;
    gpu.smMajor = 12;
    gpu.smMinor = 0;
    const QString trtDir = QStringLiteral("trt");
    const ComponentBinDirs dirs =
        componentBinDirs(bundle.path(), trtDir, QStringLiteral("cuda"),
                         {trtDir + QLatin1Char('/') + QStringLiteral("nvinfer_builder_resource_sm120_11.dll")},
                         gpu, ComponentStore{store.path()});
    QCOMPARE(dirs.trtBinDir, QDir(bundle.path()).filePath(trtDir));
    QVERIFY(!dirs.fromStore);
    QVERIFY(dirs.packName.isEmpty());
    QVERIFY(dirs.reasonKey.isEmpty());
}

void TestNativeComponents::toolchainUsesTheStoreWhenTheBundleCarriesNoResourceForThisGpu() {
    QTemporaryDir bundle;
    QVERIFY(bundle.isValid());
    QTemporaryDir store;
    QVERIFY(store.isValid());
    const ComponentStore target{store.path()};

    const ComponentPack* runtime = tensorRtRuntimePack();
    const ComponentPack* resource = packNamed(QStringLiteral("trt-sm120"));
    QVERIFY(runtime != nullptr);
    QVERIFY(resource != nullptr);
    QVERIFY2(populateStore(target, {*runtime, *resource}), qPrintable(target.root));

    GpuComputeCapability gpu;
    gpu.nvidia = true;
    gpu.smMajor = 12;
    gpu.smMinor = 0;
    const ComponentBinDirs dirs =
        componentBinDirs(bundle.path(), QStringLiteral("trt"), QStringLiteral("cuda"),
                         {QStringLiteral("trt/nvinfer_11.dll")}, gpu, target);
    QVERIFY2(dirs.fromStore, qPrintable(dirs.reason));
    QCOMPARE(dirs.trtBinDir, componentReleaseDirectory(target));
    QCOMPARE(dirs.cudaBinDir, componentReleaseDirectory(target));
    QCOMPARE(dirs.packName, QStringLiteral("trt-sm120"));
    QVERIFY(dirs.reasonKey.isEmpty());
}

void TestNativeComponents::toolchainRefusesWhenNeitherSourceHasTheResource() {
    QTemporaryDir bundle;
    QVERIFY(bundle.isValid());
    QTemporaryDir store;
    QVERIFY(store.isValid());

    GpuComputeCapability gpu;
    gpu.nvidia = true;
    gpu.smMajor = 12;
    gpu.smMinor = 0;
    const ComponentBinDirs dirs =
        componentBinDirs(bundle.path(), QStringLiteral("trt"), QStringLiteral("cuda"),
                         {QStringLiteral("trt/nvinfer_11.dll")}, gpu, ComponentStore{store.path()});
    QVERIFY(!dirs.fromStore);
    QVERIFY(dirs.trtBinDir.isEmpty());
    QVERIFY(dirs.cudaBinDir.isEmpty());
    QCOMPARE(dirs.reasonKey, QStringLiteral("native.components.missing"));
    QVERIFY(!dirs.reason.isEmpty());
}

void TestNativeComponents::verifyPackAcceptsTheRecordedBytes() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const ComponentStore store{root.path()};
    const ComponentPack* pack = packNamed(QStringLiteral("trt-sm120"));
    QVERIFY(pack != nullptr);
    QVERIFY(populateStore(store, {*pack}));

    const StoredPack verified = verifyPack(store, *pack);
    QVERIFY(verified.installed);
    QVERIFY(verified.reasonKey.isEmpty());
    QVERIFY(verified.bytes > 0);
}

void TestNativeComponents::verifyPackCatchesReplacedBytes() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const ComponentStore store{root.path()};
    const ComponentPack* pack = packNamed(QStringLiteral("trt-sm120"));
    QVERIFY(pack != nullptr);
    QVERIFY(populateStore(store, {*pack}));

    // Same length, different bytes: the size check a status read performs still
    // passes, so only re-hashing can catch this.
    const QString name = pack->files.first();
    const QByteArray original = filePayload(name);
    QByteArray replacement = original;
    replacement[0] = static_cast<char>(replacement.at(0) ^ 0x5A);
    const QString path = QDir(componentReleaseDirectory(store)).filePath(name);
    QVERIFY(writeBytes(path, replacement));
    QCOMPARE(QFileInfo(path).size(), static_cast<qint64>(original.size()));
    QVERIFY(storedPack(store, *pack).installed);

    const StoredPack verified = verifyPack(store, *pack);
    QVERIFY(!verified.installed);
    QCOMPARE(verified.reasonKey, QStringLiteral("native.components.digest"));
}

void TestNativeComponents::installedPackNamesFollowTheRecord() {
    QTemporaryDir empty;
    QVERIFY(empty.isValid());
    QVERIFY(installedPackNames(ComponentStore{empty.path()}).isEmpty());

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const ComponentStore store{root.path()};
    const ComponentPack* runtime = tensorRtRuntimePack();
    const ComponentPack* resource = packNamed(QStringLiteral("trt-sm120"));
    QVERIFY(runtime != nullptr);
    QVERIFY(resource != nullptr);
    QVERIFY(populateStore(store, {*runtime, *resource}));
    QCOMPARE(installedPackNames(store),
             QStringList({runtime->name, resource->name}));
}

void TestNativeComponents::toolchainKeepsTheBundleDirsWithoutAnNvidiaDevice() {
    QTemporaryDir bundle;
    QVERIFY(bundle.isValid());

    GpuComputeCapability gpu;
    const QString trtDir = QStringLiteral("trt");
    const ComponentBinDirs dirs =
        componentBinDirs(bundle.path(), trtDir, QStringLiteral("cuda"), {}, gpu,
                         ComponentStore{QString()});
    QCOMPARE(dirs.trtBinDir, QDir(bundle.path()).filePath(trtDir));
    QCOMPARE(dirs.cudaBinDir, QDir(bundle.path()).filePath(QStringLiteral("cuda")));
    QVERIFY(!dirs.fromStore);
    QVERIFY(dirs.reasonKey.isEmpty());
}