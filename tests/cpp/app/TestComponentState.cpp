#include "TestComponentState.hpp"

#include "dialogs/ComponentCardState.hpp"
#include "dialogs/ComponentStoreTab.hpp"
#include "dialogs/SettingsDialog.hpp"
#include "state/NativeComponentProvisioner.hpp"
#include "workers/Runner.hpp"

#include "backend/MikaiClient.hpp"

#include "native/components/NativeComponentCatalog.hpp"
#include "native/components/NativeComponentProvisioning.hpp"
#include "native/components/NativeComponentStore.hpp"
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
    const auto cleanup = qScopeGuard([&] {
        QDir(releaseDirectory).removeRecursively();
        QDir().rmdir(tensorRtDirectory);
    });

    // The runtime pack is hardware-independent; a GPU-matched kernels pack is
    // not available on every test machine, so the scenario drives the card
    // whose inventory exists everywhere.
    const ComponentPack* runtime = tensorRtRuntimePack();
    QVERIFY(runtime != nullptr);
    QVERIFY(populateStore(store, {*runtime}));

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
    tab.refresh();
    waitUntil([&] { return !tab.acquisitionRunning(); });

    // Disk is the authority: the stale claim is cleared, not repainted.
    QVERIFY(runtimeBar->isHidden());
    QVERIFY(runtimeStatus->isHidden());
    QVERIFY(runtimeStatus->text().isEmpty());
    QVERIFY(spatialStatus->isHidden());
    QVERIFY(!spatialStatus->text().contains(QStringLiteral("Installed: ")));
    // A 0/5 card never begins with "installed" in English either.
    QVERIFY(spatialState->text().startsWith(QStringLiteral("not installed (0/")));
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
    QVERIFY(cardState != nullptr && cardBar != nullptr && progressStatus != nullptr);
    QVERIFY2(cardState->text().startsWith(QStringLiteral("5/5 installed")),
             qPrintable(cardState->text()));

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
    QVERIFY2(cardState->text().startsWith(QStringLiteral("4/5 installed")),
             qPrintable(cardState->text()));
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
    QVERIFY2(cardState->text().startsWith(QStringLiteral("1/5 installed")),
             qPrintable(cardState->text()));
    QVERIFY2(cardState->text().contains(QStringLiteral("0/5 verified")),
             qPrintable(cardState->text()));
    runner.waitForDone(5000);
}

void TestComponentState::verificationTurnsAConfirmedBarIndeterminate() {
    const ComponentStore store = componentStore();
    QVERIFY(installedPackNames(store).isEmpty());
    const QString releaseDirectory = componentReleaseDirectory(store);
    const QString tensorRtDirectory = QFileInfo(releaseDirectory).absolutePath();
    const auto cleanup = qScopeGuard([&] {
        QDir(releaseDirectory).removeRecursively();
        QDir().rmdir(tensorRtDirectory);
    });
    const ComponentPack* runtime = tensorRtRuntimePack();
    QVERIFY(runtime != nullptr);
    QVERIFY(populateStore(store, {*runtime}));

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

    auto* apply = tab.findChild<QPushButton*>(QStringLiteral("primary"));
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
