#include "TestComponentState.hpp"

#include "dialogs/ComponentCardState.hpp"
#include "dialogs/ComponentStoreTab.hpp"
#include "dialogs/SettingsDialog.hpp"
#include "state/NativeComponentProvisioner.hpp"
#include "workers/Runner.hpp"

#include "backend/MikaiClient.hpp"

#include "native/components/AnimeJanaiRuntime.hpp"
#include "native/components/NativeComponentCatalog.hpp"
#include "native/components/NativeComponentProvisioning.hpp"
#include "native/components/NativeComponentStore.hpp"
#include "native/components/NativeGpuProbe.hpp"
#include "native/models/NativeSpatialAssets.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/qscopeguard.h>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>

#include <functional>

using namespace mplayer::native;

namespace {

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

/// Publishes the packs the way the installer would: every declared file exists
/// at the recorded size under the release directory, and the record lists them.
bool populateStore(const ComponentStore& store, const QVector<ComponentPack>& packs) {
    const QString directory = componentReleaseDirectory(store);
    if (!QDir().mkpath(directory)) return false;
    QJsonArray packArray;
    QJsonArray files;
    for (const ComponentPack& pack : packs) {
        packArray.append(packObject(pack));
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
    QJsonObject root;
    root.insert(QStringLiteral("schema"), QStringLiteral("mikai.native.components/1"));
    root.insert(QStringLiteral("release"), componentReleaseTag());
    root.insert(QStringLiteral("packs"), packArray);
    root.insert(QStringLiteral("files"), files);
    return writeBytes(QDir(directory).filePath(QStringLiteral("component.json")),
                      QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void waitUntil(const std::function<bool()>& condition, int timeoutMs = 5000) {
    QElapsedTimer deadline;
    deadline.start();
    while (!condition() && deadline.elapsed() < timeoutMs) {
        QApplication::processEvents();
        QTest::qWait(10);
    }
    QApplication::processEvents();
}

/// The installation root the production predicate reads. describe() sets it
/// even when no record exists yet, so the fixture never duplicates the
/// "aji/v0.24.0/trt" layout.
QString closureRoot() {
    return describeAnimeJanaiInstallation(Backend::TensorRT).root;
}

/// A minimal x64 binary image: header only, enough for validBinaryImageX64.
/// Mirrors TestAnimeJanaiRuntime::fixtureImage().
QByteArray minimalBinaryImage() {
#ifdef Q_OS_WIN
    QByteArray image(90, '\0');
    image[0] = 'M'; image[1] = 'Z';
    image[0x3c] = 64;
    image[64] = 'P'; image[65] = 'E';
    image[68] = char(0x64); image[69] = char(0x86);
    image[88] = char(0x0b); image[89] = char(0x02);
    return image;
#else
    QByteArray image(64, '\0');
    image[0] = '\x7f'; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 2; image[5] = 1; image[18] = 0x3e;
    return image;
#endif
}

/// The closure members describeAnimeJanaiInstallation requires for the
/// selected backend: the dispatcher, the backend library, the CUDA/TensorRT
/// libraries, the GPU-matched builder resource and trtexec (plus the two
/// license notices, which are required members of the Windows closure).
/// Mirrors TestAnimeJanaiRuntime::required().
QStringList closureFileNames(const ComponentPack* resource) {
#ifdef Q_OS_WIN
    QStringList files{QStringLiteral("aji.dll"), QStringLiteral("aji_trt.dll"),
                      QStringLiteral("cudart64_13.dll"), QStringLiteral("nvinfer_11.dll"),
                      QStringLiteral("nvinfer_plugin_11.dll"),
                      QStringLiteral("nvonnxparser_11.dll")};
    if (resource != nullptr) files << resource->resourceFile;
    files << QStringLiteral("trtexec.exe") << QStringLiteral("CUDA_LICENSE.txt")
          << QStringLiteral("DirectML_LICENSE.txt");
    return files;
#else
    QStringList files{QStringLiteral("libaji.so"), QStringLiteral("libaji_trt.so"),
                      QStringLiteral("libcudart.so.13"), QStringLiteral("libnvinfer.so.11"),
                      QStringLiteral("libnvinfer_plugin.so.11"),
                      QStringLiteral("libnvonnxparser.so.11")};
    if (resource != nullptr) files << resource->resourceFile;
    files << QStringLiteral("trtexec");
    return files;
#endif
}

/// Creates the backend closure record (install.json + closure files) the way
/// the installer would, so the runtime card exercises the real installed
/// path: the inventory counts the card installed only when
/// describeAnimeJanaiInstallation reports the closure installed, which
/// needs the record's schema/version/backend/abi, the Linux/Windows closure
/// files above (libaji.so/aji.dll present, the GPU-matched builder resource,
/// trtexec executable on Linux) and matching sizes and hashes. The
/// dispatcher and backend libraries are the real test dispatcher when the
/// build provides it, so the record also survives the verify re-hash: a
/// header-only image has no aji_api_version export and no pinned digest, so
/// a re-hash would refuse it. Returns false when a record already exists,
/// so a stale closure is never silently reused.
bool populateClosure() {
    const QString root = closureRoot();
    if (root.isEmpty() || QFileInfo::exists(root)) return false;
    const GpuComputeCapability& capability = observeComputeCapability();
    const ComponentPack* resource = capability.nvidia
        ? tensorRtResourcePackFor(capability.smMajor, capability.smMinor)
        : tensorRtResourcePackFor(12, 0);
    if (resource == nullptr) return false;
    const QStringList names = closureFileNames(resource);
#ifdef Q_OS_WIN
    const QString dispatcherFile = QStringLiteral("aji.dll");
    const QString backendFile = QStringLiteral("aji_trt.dll");
#else
    const QString dispatcherFile = QStringLiteral("libaji.so");
    const QString backendFile = QStringLiteral("libaji_trt.so");
#endif
    QByteArray dispatcherBytes;
    bool abiExporting = false;
#ifdef MIKAI_TEST_AJI_DISPATCHER
    QFile dispatcher(QString::fromUtf8(MIKAI_TEST_AJI_DISPATCHER));
    if (dispatcher.open(QIODevice::ReadOnly)) {
        dispatcherBytes = dispatcher.readAll();
        abiExporting = !dispatcherBytes.isEmpty();
    }
#endif
    QJsonArray entries;
    for (const QString& name : names) {
        const QByteArray bytes =
            (name == dispatcherFile || name == backendFile) && abiExporting
                ? dispatcherBytes
                : minimalBinaryImage();
        if (!writeBytes(QDir(root).filePath(name), bytes)) return false;
#ifndef Q_OS_WIN
        if (name == QLatin1String("trtexec") &&
            !QFile::setPermissions(QDir(root).filePath(name),
                                   QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                       QFileDevice::ExeOwner))
            return false;
#endif
        QJsonObject entry;
        entry.insert(QStringLiteral("path"), name);
        entry.insert(QStringLiteral("bytes"), static_cast<qint64>(bytes.size()));
        entry.insert(QStringLiteral("sha256"), sha256Of(bytes));
        entries.append(entry);
    }
    QJsonObject record;
    record.insert(QStringLiteral("schema"), QStringLiteral("mikai.native.animejanai/3"));
    record.insert(QStringLiteral("version"), QStringLiteral("v0.24.0"));
    record.insert(QStringLiteral("backend"), QStringLiteral("trt"));
    record.insert(QStringLiteral("abi"), 9);
    record.insert(QStringLiteral("provenance"), QStringLiteral("local"));
    record.insert(QStringLiteral("archiveBytes"), 0);
    record.insert(QStringLiteral("archiveSha256"), QString());
    record.insert(QStringLiteral("heavyRifeSupported"), abiExporting);
    record.insert(QStringLiteral("files"), entries);
    record.insert(QStringLiteral("acquisitions"), QJsonArray{});
    return writeBytes(QDir(root).filePath(QStringLiteral("install.json")),
                      QJsonDocument(record).toJson(QJsonDocument::Compact));
}

}  // namespace

void TestComponentState::progressDecisionNeverFabricatesCompletion() {
    using namespace mikai::app::components;

    // Nothing happened, and the disk holds nothing: no meter at all.
    CardState empty;
    empty.inventory.known = true;
    empty.inventory.total = 5;
    QVERIFY(!componentProgressBar(empty).visible);
    QVERIFY(!componentProgressPercent(empty).has_value());
    QCOMPARE(static_cast<int>(cardReadiness(empty)), static_cast<int>(Readiness::Missing));

    // A real transfer reports its own bytes.
    CardState active;
    active.transfer.active = 2;
    active.transfer.received = 50;
    active.transfer.total = 200;
    QCOMPARE(componentProgressPercent(active).value(), 25);
    QVERIFY(componentProgressBar(active).visible);
    QVERIFY(!componentProgressBar(active).indeterminate);

    // An active transfer with no accounting is indeterminate, not complete.
    CardState unmetered;
    unmetered.transfer.active = 1;
    unmetered.transfer.unmetered = 1;
    QVERIFY(!componentProgressPercent(unmetered).has_value());
    QVERIFY(componentProgressBar(unmetered).indeterminate);

    // Terminal items plus a confirming inventory may read 100 %.
    CardState confirmed;
    confirmed.transfer.installed = 5;
    confirmed.inventory.known = true;
    confirmed.inventory.installed = 5;
    confirmed.inventory.total = 5;
    QCOMPARE(componentProgressPercent(confirmed).value(), 100);

    // Terminal items with a 0/5 disk inventory are not 100 %.
    CardState stale;
    stale.transfer.installed = 5;
    stale.inventory.known = true;
    stale.inventory.installed = 0;
    stale.inventory.total = 5;
    QCOMPARE(componentProgressPercent(stale).value(), 0);
    QCOMPARE(componentProgressBar(stale).percent, 0);

    // Full bytes while the transfer is still active: never 100 before the
    // publish is confirmed by the disk.
    CardState activeFull;
    activeFull.transfer.active = 1;
    activeFull.transfer.received = 100;
    activeFull.transfer.total = 100;
    activeFull.inventory.known = true;
    activeFull.inventory.total = 5;
    QCOMPARE(componentProgressPercent(activeFull).value(), 99);
    QCOMPARE(componentProgressBar(activeFull).percent, 99);
    QVERIFY(!componentProgressBar(activeFull).indeterminate);

    // One success beside one failure: failure wins, even with a confirming
    // inventory, and the pure function agrees with cardReadiness.
    CardState mixed;
    mixed.transfer.installed = 1;
    mixed.transfer.failed = 1;
    mixed.inventory.known = true;
    mixed.inventory.installed = 1;
    mixed.inventory.total = 5;
    QCOMPARE(componentProgressPercent(mixed).value(), 0);
    QVERIFY(componentProgressBar(mixed).failed);
    QCOMPARE(static_cast<int>(cardReadiness(mixed)), static_cast<int>(Readiness::Failed));

    // All failed after a full byte transfer: the axis no longer carries those
    // bytes, and the bar is failure-coloured, never complete.
    CardState failed;
    failed.transfer.failed = 5;
    failed.transfer.received = 500;
    failed.transfer.total = 500;
    QCOMPARE(componentProgressPercent(failed).value(), 0);
    const ProgressBarDecision failedBar = componentProgressBar(failed);
    QVERIFY(failedBar.visible);
    QVERIFY(failedBar.failed);
    QCOMPARE(failedBar.percent, 0);
    QVERIFY(!failedBar.indeterminate);

    // Cancelled transfers contribute nothing either.
    CardState cancelled;
    cancelled.transfer.cancelled = 2;
    cancelled.transfer.received = 200;
    cancelled.transfer.total = 200;
    QCOMPARE(componentProgressPercent(cancelled).value(), 0);

    // Verifying is indeterminate even with bytes on record.
    CardState verifying;
    verifying.verificationRunning = true;
    verifying.transfer.active = 1;
    verifying.transfer.received = 100;
    verifying.transfer.total = 100;
    QVERIFY(!componentProgressPercent(verifying).has_value());
    QVERIFY(componentProgressBar(verifying).indeterminate);
    QCOMPARE(static_cast<int>(cardReadiness(verifying)), static_cast<int>(Readiness::Verifying));
}

void TestComponentState::failedFullTransferNeverShowsAFullBar() {
    mikai::app::Runner runner;
    mikai::app::NativeComponentProvisioner provisioner(runner);
    mikai::app::ComponentStoreTab tab;
    tab.setComponentProvisioner(&provisioner);

    auto* runtimeBar =
        tab.findChild<QProgressBar*>(QStringLiteral("tensorRtRuntimeCardProgress"));
    auto* runtimeStatus =
        tab.findChild<QLabel*>(QStringLiteral("tensorRtRuntimeCardProgressStatus"));
    QVERIFY(runtimeBar != nullptr);
    QVERIFY(runtimeStatus != nullptr);

    emit provisioner.progress(ComponentProvisionProgress{
        QStringLiteral("trt-runtime"), 100, 100, ComponentProvisionStage::Downloading});
    QCOMPARE(runtimeBar->value(), 99);
    QVERIFY(runtimeStatus->text().contains(QStringLiteral("Downloading")));

    // The transfer reached every byte, then failed: the bar must not remain at
    // 100 % and the line must carry the reason.
    emit provisioner.finished(false, false, QStringLiteral("digest mismatch"));
    QCOMPARE(runtimeBar->value(), 0);
    QCOMPARE(runtimeBar->maximum(), 100);
    QVERIFY(runtimeStatus->text().contains(QStringLiteral("Failed")));
    QVERIFY(runtimeStatus->text().contains(QStringLiteral("digest mismatch")));
}

void TestComponentState::deletedFilesClearTheStaleBarOnRefresh() {
    const ComponentStore store = componentStore();
    QVERIFY(installedPackNames(store).isEmpty());
    const QString releaseDirectory = componentReleaseDirectory(store);
    const QString tensorRtDirectory = QFileInfo(releaseDirectory).absolutePath();
    const QString closureDirectory = closureRoot();
    const auto cleanup = qScopeGuard([&] {
        QDir(closureDirectory).removeRecursively();
        QDir(releaseDirectory).removeRecursively();
        QDir().rmdir(tensorRtDirectory);
    });

    // The runtime pack is hardware-independent; a GPU-matched kernels pack is
    // not available on every test machine, so the scenario drives the card
    // whose inventory exists everywhere.
    const ComponentPack* runtime = tensorRtRuntimePack();
    QVERIFY(runtime != nullptr);
    QVERIFY(populateStore(store, {*runtime}));
    // The runtime card counts installed only through the backend's own
    // closure record: a stored pack without it is a download, not readiness,
    // and the bar is withdrawn. The fixture creates the real closure, so the
    // 100 below is the installed path, not a stale row.
    QVERIFY(populateClosure());
    const AnimeJanaiInstallation closure =
        describeAnimeJanaiInstallation(Backend::TensorRT);
    QVERIFY2(closure.installed, qPrintable(closure.reason));

    mikai::app::Runner runner;
    mikai::app::NativeComponentProvisioner provisioner(runner);
    mikai::app::ComponentStoreTab tab;
    tab.setComponentProvisioner(&provisioner);
    tab.setRunner(&runner);
    waitUntil([&] { return tab.inventoryReady() && !tab.acquisitionRunning(); });
    QVERIFY(tab.inventoryReady());

    auto* runtimeBar =
        tab.findChild<QProgressBar*>(QStringLiteral("tensorRtRuntimeCardProgress"));
    auto* runtimeStatus =
        tab.findChild<QLabel*>(QStringLiteral("tensorRtRuntimeCardProgressStatus"));
    auto* spatialState = tab.findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardState"));
    auto* spatialStatus =
        tab.findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardProgressStatus"));
    QVERIFY(runtimeBar != nullptr && runtimeStatus != nullptr);
    QVERIFY(spatialState != nullptr);
    QVERIFY(spatialStatus != nullptr);

    emit provisioner.progress(ComponentProvisionProgress{
        runtime->name, 100, 100, ComponentProvisionStage::Installed});
    // The inventory confirms the terminal item, so this is a truthful 100 %.
    QCOMPARE(runtimeBar->value(), 100);

    QVERIFY(QDir(releaseDirectory).removeRecursively());
    // Removing the runtime from disk removes its closure record too: while
    // the record survives, the inventory still confirms the card and the
    // stale bar legitimately survives with it.
    QVERIFY(QDir(closureDirectory).removeRecursively());
    tab.refresh();
    waitUntil([&] { return !tab.acquisitionRunning(); });

    // Disk is the authority: the stale claim is cleared, not repainted.
    QVERIFY(runtimeBar->isHidden());
    QVERIFY(runtimeStatus->isHidden());
    QVERIFY(runtimeStatus->text().isEmpty());
    QVERIFY(spatialStatus->isHidden());
    QVERIFY(!spatialStatus->text().contains(QStringLiteral("Installed: ")));
    // The counts are the truth the old badge prose carried.
    auto* spatialCounts = tab.findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardCount"));
    QVERIFY(spatialCounts != nullptr);
    QVERIFY2(spatialCounts->text().startsWith(QStringLiteral("0/")),
             qPrintable(spatialCounts->text()));
    runner.waitForDone(5000);
}

void TestComponentState::staleModelItemIsClearedWhenUnrelatedModelsRemain() {
    QVERIFY(storedSpatialAssets().isEmpty());
    const QVector<SpatialAsset> catalog = spatialAssetCatalog();
    QCOMPARE(catalog.size(), 5);
    const QString root = spatialAssetRoot();
    const bool rootExisted = QFileInfo::exists(root);
    QStringList createdDirectories;
    const auto cleanup = qScopeGuard([&] {
        for (const QString& directory : createdDirectories)
            QDir(directory).removeRecursively();
        if (!rootExisted) QDir().rmdir(root);
    });
    // Presence only: named directories with a graph file. The bytes do not
    // match the pins, but the inventory counts four unrelated present models,
    // which is exactly the aggregate count that used to mask a deleted item.
    for (const SpatialAsset& asset : catalog) {
        const QString directory =
            QDir(root).filePath(SpatialAssetStore::directoryName(asset.id));
        QVERIFY(QDir().mkpath(directory));
        createdDirectories << directory;
        QVERIFY(writeBytes(QDir(directory).filePath(SpatialAssetStore::graphFileName()),
                           QByteArrayLiteral("placeholder")));
    }

    mikai::app::Runner runner;
    mikai::app::NativeComponentProvisioner provisioner(runner);
    mikai::app::ComponentStoreTab tab;
    tab.setComponentProvisioner(&provisioner);
    tab.setRunner(&runner);
    waitUntil([&] { return tab.inventoryReady() && !tab.acquisitionRunning(); });
    QVERIFY(tab.inventoryReady());

    auto* cardState = tab.findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardState"));
    auto* cardBar =
        tab.findChild<QProgressBar*>(QStringLiteral("animeJanaiModelsCardProgress"));
    auto* progressStatus =
        tab.findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardProgressStatus"));
    auto* cardCounts = tab.findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardCount"));
    QVERIFY(cardState != nullptr && cardBar != nullptr && progressStatus != nullptr);
    QVERIFY(cardCounts != nullptr);
    QVERIFY2(cardCounts->text().startsWith(QStringLiteral("5/5 installed")),
             qPrintable(cardCounts->text()));

    const SpatialAsset& installed = catalog.last();
    emit provisioner.progress(ComponentProvisionProgress{
        installed.id, 100, 100, ComponentProvisionStage::Installed});
    QCOMPARE(cardBar->value(), 100);
    QVERIFY(progressStatus->text().contains(QStringLiteral("Installed: ")));

    // Delete only the item this session installed; four unrelated model files
    // remain. Identity reconciliation must drop the stale item, not keep it
    // because a catalog-wide count is still non-zero.
    QVERIFY(QFile::remove(QDir(createdDirectories.last())
                              .filePath(SpatialAssetStore::graphFileName())));
    tab.refresh();
    waitUntil([&] { return !tab.acquisitionRunning(); });

    QVERIFY(progressStatus->isHidden());
    QVERIFY(!progressStatus->text().contains(QStringLiteral("Installed: ")));
    QVERIFY(cardBar->isHidden());
    QVERIFY(cardCounts != nullptr);
    QVERIFY2(cardCounts->text().startsWith(QStringLiteral("4/5 installed")),
             qPrintable(cardCounts->text()));
    runner.waitForDone(5000);
}

void TestComponentState::initialInventorySurvivesClientInjection() {
    QVERIFY(storedSpatialAssets().isEmpty());
    const QVector<SpatialAsset> catalog = spatialAssetCatalog();
    QCOMPARE(catalog.size(), 5);
    const QString root = spatialAssetRoot();
    const bool rootExisted = QFileInfo::exists(root);
    const QString directory =
        QDir(root).filePath(SpatialAssetStore::directoryName(catalog.first().id));
    const auto cleanup = qScopeGuard([&] {
        QDir(directory).removeRecursively();
        if (!rootExisted) QDir().rmdir(root);
    });
    // Presence only: one named directory with a graph file. The bytes do not
    // match the pins, so this proves visibility and count, never verification.
    QVERIFY(QDir().mkpath(directory));
    QVERIFY(writeBytes(QDir(directory).filePath(SpatialAssetStore::graphFileName()),
                       QByteArrayLiteral("placeholder")));

    mikai::app::Runner runner;
    mikai::backend::MikaiClient client(QString(), QStringLiteral("http://127.0.0.1:1"));
    // The application's own initialization order: the dialog builds the
    // Components tab and binds the runner before the client. Binding the client
    // bumps the run generation, and the initial inventory scan the runner
    // started must not be stranded by it - no Refresh, no click, no probe help.
    mikai::app::SettingsDialog dialog(client, runner);
    auto* tab = dialog.findChild<mikai::app::ComponentStoreTab*>();
    QVERIFY(tab != nullptr);
    QVERIFY(dialog.showPage(QStringLiteral("components")));

    auto* cardState = tab->findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardState"));
    QVERIFY(cardState != nullptr);

    waitUntil([&] { return tab->inventoryReady() && !tab->acquisitionRunning(); });
    QVERIFY(tab->inventoryReady());
    auto* cardCounts = tab->findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardCount"));
    QVERIFY(cardCounts != nullptr);
    QVERIFY2(cardCounts->text().startsWith(QStringLiteral("1/5 installed")),
             qPrintable(cardCounts->text()));
    QVERIFY2(cardCounts->text().contains(QStringLiteral("0/5 verified")),
             qPrintable(cardCounts->text()));
    runner.waitForDone(5000);
}

void TestComponentState::verificationTurnsAConfirmedBarIndeterminate() {
    const ComponentStore store = componentStore();
    QVERIFY(installedPackNames(store).isEmpty());
    const QString releaseDirectory = componentReleaseDirectory(store);
    const QString tensorRtDirectory = QFileInfo(releaseDirectory).absolutePath();
    const QString closureDirectory = closureRoot();
    const auto cleanup = qScopeGuard([&] {
        QDir(closureDirectory).removeRecursively();
        QDir(releaseDirectory).removeRecursively();
        QDir().rmdir(tensorRtDirectory);
    });
    const ComponentPack* runtime = tensorRtRuntimePack();
    QVERIFY(runtime != nullptr);
    QVERIFY(populateStore(store, {*runtime}));
    // Same real closure as deletedFilesClearTheStaleBarOnRefresh, proved
    // here through the verify re-hash as well: the proof run re-hashes the
    // backend record, so only a closure the production predicate accepts
    // with rehash can read 100 again afterwards.
    QVERIFY(populateClosure());
    const AnimeJanaiInstallation closure =
        describeAnimeJanaiInstallation(Backend::TensorRT, true);
    QVERIFY2(closure.installed, qPrintable(closure.reason));

    mikai::app::Runner runner;
    mikai::app::NativeComponentProvisioner provisioner(runner);
    mikai::app::ComponentStoreTab tab;
    tab.setComponentProvisioner(&provisioner);
    tab.setRunner(&runner);
    waitUntil([&] { return tab.inventoryReady() && !tab.acquisitionRunning(); });
    QVERIFY(tab.inventoryReady());

    auto* runtimeBar =
        tab.findChild<QProgressBar*>(QStringLiteral("tensorRtRuntimeCardProgress"));
    auto* verifyButton =
        tab.findChild<QPushButton*>(QStringLiteral("nativeComponentVerifyButton"));
    QVERIFY(runtimeBar != nullptr && verifyButton != nullptr);
    emit provisioner.progress(ComponentProvisionProgress{
        runtime->name, 100, 100, ComponentProvisionStage::Installed});
    QCOMPARE(runtimeBar->value(), 100);

    verifyButton->click();
    QVERIFY(tab.verifyActive());
    QCOMPARE(runtimeBar->maximum(), 0);
    waitUntil([&] { return !tab.verifyActive(); });
    // The proof is over; the confirmed terminal item may read 100 again.
    QCOMPARE(runtimeBar->maximum(), 100);
    QCOMPARE(runtimeBar->value(), 100);
    runner.waitForDone(5000);
}

void TestComponentState::verificationIsNotADownload() {
    mikai::app::Runner runner;
    mikai::app::ComponentStoreTab tab;
    tab.setRunner(&runner);
    waitUntil([&] { return tab.inventoryReady() && !tab.acquisitionRunning(); });
    QVERIFY(tab.inventoryReady());

    auto* verifyButton =
        tab.findChild<QPushButton*>(QStringLiteral("nativeComponentVerifyButton"));
    auto* status = tab.findChild<QLabel*>(QStringLiteral("nativeComponentStatus"));
    QVERIFY(verifyButton != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(verifyButton->isEnabled());

    verifyButton->click();
    // The proof run is its own activity: the transfer predicate stays false and
    // the status line is the verification wording, not a download.
    QVERIFY(tab.verifyActive());
    QVERIFY(!tab.downloadActive());
    QVERIFY(status->text().contains(QStringLiteral("Verifying")));
    QCOMPARE(status->accessibleDescription(), QStringLiteral("busy"));

    waitUntil([&] { return !tab.verifyActive(); });
    QVERIFY(!tab.verifyActive());
    QVERIFY(!tab.downloadActive());
    QVERIFY(verifyButton->isEnabled());
    runner.waitForDone(5000);
}

void TestComponentState::generationBumpMidCheckReleasesEveryLatch() {
    mikai::app::Runner runner;
    mikai::app::ComponentStoreTab tab;
    tab.setRunner(&runner);
    waitUntil([&] { return tab.inventoryReady() && !tab.acquisitionRunning(); });
    QVERIFY(tab.inventoryReady());

    auto* apply = tab.findChild<QPushButton*>(QStringLiteral("componentDownloadButton"));
    auto* cancel =
        tab.findChild<QPushButton*>(QStringLiteral("nativeComponentTabCancelButton"));
    auto* backendCombo =
        tab.findChild<QComboBox*>(QStringLiteral("animeJanaiRuntimeBackend"));
    auto* status = tab.findChild<QLabel*>(QStringLiteral("nativeComponentStatus"));
    QVERIFY(apply != nullptr && cancel != nullptr);
    QVERIFY(backendCombo != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(apply->isEnabled());

    // Start the off-GUI check, then invalidate its generation the way a backend
    // change does. The queued delivery is discarded, so the in-flight latches
    // and the muted busy line must be released, not left stuck.
    tab.downloadMissing();
    QVERIFY(tab.acquisitionRunning());
    QVERIFY(!apply->isEnabled());
    if (backendCombo->count() >= 2) {
        backendCombo->setCurrentIndex((backendCombo->currentIndex() + 1) %
                                      backendCombo->count());
    } else {
        tab.setRunner(nullptr);
    }
    // The backend change re-reads the inventory, so the surface settles only
    // after that scan lands - the point is that the abandoned check's latches
    // are gone, not that the scan itself is instantaneous.
    waitUntil([&] { return !tab.acquisitionRunning(); });
    QVERIFY(!tab.acquisitionRunning());
    QVERIFY(apply->isEnabled());
    QVERIFY(!cancel->isEnabled());
    QVERIFY(!tab.downloadActive());
    QVERIFY(!tab.verifyActive());
    QVERIFY(tab.statusKey().isEmpty());
    QVERIFY(status->text().isEmpty());
    runner.waitForDone(5000);
}

void TestComponentState::secondProvisionerSurvivesTheFirstBeingDestroyed() {
    // P1-06: two provisioners (Settings dialog + startup prompt) share one
    // run slot, now a QPointer guarded by a QMutex. Destroying the first
    // must not strand the second: the slot auto-nulls instead of dangling,
    // and the second provisioner stays usable with no stuck running_ flag.
    // (A live two-run interleaving needs a GPU + Native runtime, which the
    // hermetic suite has not got; the slot discipline itself — claim only if
    // empty, clear only if still this — is what this case pins.)
    mikai::app::Runner runner;
    auto* first = new mikai::app::NativeComponentProvisioner(runner);
    mikai::app::NativeComponentProvisioner second(runner);
    QVERIFY(!first->running());
    QVERIFY(!second.running());
    delete first;

    QVERIFY(!second.running());
    // The side-effect-free check still answers instead of hanging or
    // crashing; it never starts a download.
    QString reason;
    const bool needed = second.needsDownload(&reason);
    QVERIFY(!needed || !reason.isEmpty());
    QVERIFY(!second.running());
    second.cancel();
    QVERIFY(!second.running());
    runner.waitForDone(5000);
}
