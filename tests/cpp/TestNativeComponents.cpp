#include "TestNativeComponents.hpp"

#include "native/components/NativeComponentCatalog.hpp"
#include "native/components/NativeComponentStore.hpp"
#include "native/components/NativeComponentToolchain.hpp"

#include "native/bundle/NativePaths.hpp"

#include <iterator>

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
QJsonObject rifeInstallRecord() {
    const RifeArchivePack& pack = rifeArchivePack();
    QJsonObject archive;
    archive.insert(QStringLiteral("name"), pack.name);
    archive.insert(QStringLiteral("asset"), pack.asset);
    archive.insert(QStringLiteral("url"), pack.url);
    archive.insert(QStringLiteral("assetBytes"), pack.assetBytes);
    archive.insert(QStringLiteral("assetSha256"), pack.assetSha256);
    QJsonArray files;
    for (const RifeModelPin& pin : rifeModelPins()) {
        QJsonObject file;
        file.insert(QStringLiteral("path"), pin.fileName);
        file.insert(QStringLiteral("bytes"), pin.bytes);
        file.insert(QStringLiteral("sha256"), pin.graphSha256);
        files.append(file);
    }
    QJsonObject record;
    record.insert(QStringLiteral("schema"), QStringLiteral("mikai.native.rife/1"));
    record.insert(QStringLiteral("release"), componentReleaseTag());
    record.insert(QStringLiteral("archive"), archive);
    record.insert(QStringLiteral("files"), files);
    return record;
}

bool writeRifeFixture(const QString& directory, bool record) {
    if (!QDir().mkpath(directory)) return false;
    for (const RifeModelPin& pin : rifeModelPins()) {
        QFile file(QDir(directory).filePath(pin.fileName));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            !file.resize(pin.bytes)) {
            return false;
        }
    }
    if (!record) return true;
    return writeBytes(QDir(directory).filePath(QStringLiteral("install.json")),
                      QJsonDocument(rifeInstallRecord()).toJson(QJsonDocument::Compact));
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

void TestNativeComponents::rifeGraphWithoutInstallRecordIsMissing() {
    QDir().mkpath(nativeRoot());
    QTemporaryDir root(QDir(nativeRoot()).filePath(QStringLiteral("rife-store-test-XXXXXX")));
    QVERIFY(root.isValid());
    QVERIFY(writeRifeFixture(root.path(), false));

    const StoredRifeGraph stored = storedRifeGraph(
        RifeArchiveStore{root.path()}, rifeModelPins().first());
    QVERIFY(!stored.installed);
    QVERIFY(!stored.reason.isEmpty());
}

void TestNativeComponents::rifeGraphRecordTrustsVerifiedIdentityUntilManualVerify() {
    QDir().mkpath(nativeRoot());
    QTemporaryDir root(QDir(nativeRoot()).filePath(QStringLiteral("rife-store-test-XXXXXX")));
    QVERIFY(root.isValid());
    QVERIFY(writeRifeFixture(root.path(), true));

    const RifeModelPin& pin = rifeModelPins().first();
    const QString path = QDir(root.path()).filePath(pin.fileName);
    QFile graph(path);
    QVERIFY(graph.open(QIODevice::ReadWrite));
    QVERIFY(graph.seek(0));
    const char corrupt = 'X';
    QCOMPARE(graph.write(&corrupt, 1), qint64(1));
    graph.close();

    const StoredRifeGraph stored = storedRifeGraph(RifeArchiveStore{root.path()}, pin);
    QVERIFY(stored.installed);
    QCOMPARE(stored.bytes, pin.bytes);

    const StoredRifeGraph verified = verifyRifeGraph(RifeArchiveStore{root.path()}, pin);
    QVERIFY(!verified.installed);
    QVERIFY(verified.reason.contains(QStringLiteral("SHA-256")));
}

void TestNativeComponents::rifeGraphRecordRequiresTheExactInventory() {
    QDir().mkpath(nativeRoot());
    QTemporaryDir root(QDir(nativeRoot()).filePath(QStringLiteral("rife-store-test-XXXXXX")));
    QVERIFY(root.isValid());
    QVERIFY(writeRifeFixture(root.path(), true));
    QVERIFY(writeBytes(QDir(root.path()).filePath(QStringLiteral("unexpected.onnx")),
                       QByteArrayLiteral("extra")));

    const StoredRifeGraph stored = storedRifeGraph(
        RifeArchiveStore{root.path()}, rifeModelPins().first());
    QVERIFY(!stored.installed);
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