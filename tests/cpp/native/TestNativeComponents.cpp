#include "TestNativeComponents.hpp"

#include "dialogs/ComponentStoreTab.hpp"
#include "workers/Runner.hpp"

#include "native/components/NativeComponentCatalog.hpp"
#include "native/components/NativeComponentStore.hpp"
#include "native/components/NativeComponentProvisioning.hpp"
#include "native/components/NativeComponentToolchain.hpp"
#include "native/models/NativeSpatialAssets.hpp"

#include "native/bundle/NativePaths.hpp"

#include <iterator>

#include <QtCore/QCryptographicHash>
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QElapsedTimer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QLocale>
#include <QtCore/QTimer>
#include <QtCore/qscopeguard.h>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>

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

void TestNativeComponents::componentProgressSeparatesCardsAndAggregatesSpatialGraphs() {
    mikai::app::Runner runner;
    mikai::app::NativeComponentProvisioner provisioner(runner);
    QWidget reparentedHost;
    mikai::app::ComponentStoreTab tab;
    tab.setComponentProvisioner(&provisioner);

    auto* runtimeBar =
        tab.findChild<QProgressBar*>(QStringLiteral("tensorRtRuntimeCardProgress"));
    auto* runtimeStatus =
        tab.findChild<QLabel*>(QStringLiteral("tensorRtRuntimeCardProgressStatus"));
    auto* kernelsBar =
        tab.findChild<QProgressBar*>(QStringLiteral("tensorRtKernelsCardProgress"));
    auto* kernelsStatus =
        tab.findChild<QLabel*>(QStringLiteral("tensorRtKernelsCardProgressStatus"));
    auto* spatialBar =
        tab.findChild<QProgressBar*>(QStringLiteral("animeJanaiModelsCardProgress"));
    auto* spatialStatus =
        tab.findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardProgressStatus"));
    auto* rifeBar =
        tab.findChild<QProgressBar*>(QStringLiteral("rifeModelsCardProgress"));
    auto* rifeStatus =
        tab.findChild<QLabel*>(QStringLiteral("rifeModelsCardProgressStatus"));
    QVERIFY(runtimeBar != nullptr);
    QVERIFY(runtimeStatus != nullptr);
    QVERIFY(kernelsBar != nullptr);
    QVERIFY(kernelsStatus != nullptr);
    QVERIFY(spatialBar != nullptr);
    QVERIFY(spatialStatus != nullptr);
    QVERIFY(rifeBar != nullptr);
    QVERIFY(rifeStatus != nullptr);
    QVERIFY(tab.findChild<QProgressBar*>(QStringLiteral("nativeComponentTabProgress")) == nullptr);

    const auto publish = [&provisioner](const QString& pack, qint64 received, qint64 total,
                                        ComponentProvisionStage stage) {
        emit provisioner.progress(ComponentProvisionProgress{pack, received, total, stage});
    };
    publish(QStringLiteral("trt-runtime"), 25, 100, ComponentProvisionStage::Downloading);
    publish(QStringLiteral("trt-sm120"), 80, 100, ComponentProvisionStage::Verifying);
    QCOMPARE(runtimeBar->value(), 25);
    QCOMPARE(kernelsBar->value(), 80);
    QVERIFY(runtimeStatus->text().contains(QStringLiteral("trt-runtime")));
    QVERIFY(kernelsStatus->text().contains(QStringLiteral("Verifying")));
    QVERIFY(kernelsStatus->text().contains(QStringLiteral("trt-sm120")));

    publish(QStringLiteral("trt-sm120"), 100, 100, ComponentProvisionStage::Installed);
    QCOMPARE(kernelsBar->value(), 100);
    QVERIFY(kernelsStatus->text().contains(QStringLiteral("Installed")));
    QCOMPARE(runtimeBar->value(), 25);
    QVERIFY(runtimeStatus->text().contains(QStringLiteral("Downloading")));

    publish(QStringLiteral("trt-runtime"), 100, 100, ComponentProvisionStage::Downloading);
    QCOMPARE(runtimeBar->value(), 100);
    QVERIFY(runtimeStatus->text().contains(QStringLiteral("Downloading")));
    QVERIFY(!runtimeStatus->text().contains(QStringLiteral("Installed")));
    publish(QStringLiteral("trt-runtime"), 0, 0, ComponentProvisionStage::Verifying);
    QCOMPARE(runtimeBar->value(), 100);
    QVERIFY(runtimeStatus->text().contains(QStringLiteral("Verifying")));
    QVERIFY(runtimeStatus->text().contains(QStringLiteral("100")));
    QVERIFY(!runtimeStatus->text().contains(QStringLiteral("Installed")));
    publish(QStringLiteral("trt-runtime"), 100, 100, ComponentProvisionStage::Installed);
    QVERIFY(runtimeStatus->text().contains(QStringLiteral("Installed")));

    const RifeArchivePack& rifePack = rifeArchivePack();
    publish(rifePack.name, 40, 100, ComponentProvisionStage::Downloading);
    QCOMPARE(rifeBar->value(), 40);
    QCOMPARE(runtimeBar->value(), 100);
    QCOMPARE(kernelsBar->value(), 100);
    QVERIFY(rifeStatus->text().contains(rifePack.asset));
    publish(rifePack.name, 40, 100, ComponentProvisionStage::Extracting);
    QVERIFY(rifeStatus->text().contains(QStringLiteral("Extracting")));
    publish(rifePack.name, 100, 100, ComponentProvisionStage::Installed);
    QCOMPARE(rifeBar->value(), 100);
    QVERIFY(rifeStatus->text().contains(QStringLiteral("Installed")));

    const QVector<SpatialAsset> assets = spatialAssetCatalog();
    QCOMPARE(assets.size(), 5);
    qint64 aggregateReceived = 0;
    qint64 aggregateTotal = 0;
    for (int index = 0; index < assets.size(); ++index) {
        const SpatialAsset& asset = assets.at(index);
        const qint64 received = asset.bytes * (index + 1) / 10;
        aggregateReceived += received;
        aggregateTotal += asset.bytes;
        publish(asset.id, received, asset.bytes, ComponentProvisionStage::Downloading);
    }
    QCOMPARE(spatialBar->value(),
             static_cast<int>(aggregateReceived * 100 / aggregateTotal));
    for (const SpatialAsset& asset : assets)
        QVERIFY(spatialStatus->text().contains(asset.name));

    tab.setParent(&reparentedHost);
    QApplication::processEvents();
    QCOMPARE(runtimeBar->value(), 100);
    QCOMPARE(kernelsBar->value(), 100);
    QCOMPARE(spatialBar->value(),
             static_cast<int>(aggregateReceived * 100 / aggregateTotal));

    const SpatialAsset& completedAsset = assets.first();
    publish(completedAsset.id, completedAsset.bytes, completedAsset.bytes,
            ComponentProvisionStage::Installed);
    const qint64 afterOneInstalled =
        aggregateReceived - completedAsset.bytes / 10 + completedAsset.bytes;
    QCOMPARE(spatialBar->value(),
             static_cast<int>(afterOneInstalled * 100 / aggregateTotal));
    QVERIFY(spatialStatus->text().contains(
        QStringLiteral("Installed: ") + completedAsset.name));

    tab.cancelAcquisition();
    QVERIFY(spatialStatus->text().contains(QStringLiteral("Cancelled")));
    QVERIFY(spatialStatus->text().contains(
        QStringLiteral("Installed: ") + completedAsset.name));
    QVERIFY(runtimeStatus->text().contains(QStringLiteral("Installed")));
    QVERIFY(kernelsStatus->text().contains(QStringLiteral("Installed")));

    emit provisioner.finished(false, true, QString());
    publish(QStringLiteral("trt-runtime"), 100, 100, ComponentProvisionStage::Downloading);
    publish(QStringLiteral("trt-sm120"), 80, 100, ComponentProvisionStage::Installing);
    publish(QStringLiteral("trt-runtime"), 100, 100, ComponentProvisionStage::Installed);
    emit provisioner.finished(false, false, QStringLiteral("kernel digest failed"));
    QVERIFY(runtimeStatus->text().contains(QStringLiteral("Installed")));
    QVERIFY(kernelsStatus->text().contains(QStringLiteral("Failed")));
    QVERIFY(kernelsStatus->text().contains(QStringLiteral("kernel digest failed")));
    QVERIFY(rifeStatus->text().contains(QStringLiteral("Installed")));
    QVERIFY(spatialStatus->text().contains(QStringLiteral("Cancelled")));
    QVERIFY(spatialStatus->text().contains(
        QStringLiteral("Installed: ") + completedAsset.name));
}

void TestNativeComponents::upscalingPackReviewsAndQueuesMissingGraphs_data() {
    QTest::addColumn<bool>("corruptRow");
    QTest::newRow("all-five-absent") << false;
    QTest::newRow("one-corrupt-four-absent") << true;
}

void TestNativeComponents::upscalingPackReviewsAndQueuesMissingGraphs() {
    QFETCH(bool, corruptRow);
    const QVector<SpatialAsset> catalog = spatialAssetCatalog();
    QCOMPARE(catalog.size(), 5);
    // The parent launcher supplies a physically isolated application directory.
    // Never replace or delete an existing graph to arrange this consumer test.
    QVERIFY(storedSpatialAssets().isEmpty());
    const QString root = spatialAssetRoot();
    const bool rootExisted = QFileInfo::exists(root);
    const QString corruptDirectory =
        QDir(root).filePath(SpatialAssetStore::directoryName(catalog.first().id));
    QVERIFY(!QFileInfo::exists(corruptDirectory));
    const auto cleanup = qScopeGuard([&] {
        if (corruptRow) QDir(corruptDirectory).removeRecursively();
        if (!rootExisted) QDir().rmdir(root);
    });
    if (corruptRow) {
        QVERIFY(writeBytes(QDir(corruptDirectory).filePath(
                               SpatialAssetStore::manifestFileName()),
                           QByteArrayLiteral("{}")));
        QVERIFY(writeBytes(QDir(corruptDirectory).filePath(
                               SpatialAssetStore::graphFileName()),
                           QByteArrayLiteral("not a pinned graph")));
        const QVector<StoredSpatialAsset> stored = storedSpatialAssets();
        QCOMPARE(stored.size(), 1);
        QVERIFY(!stored.first().verified);
    }

    mikai::app::Runner runner;
    mikai::app::ComponentStoreTab tab;
    tab.setRunner(&runner);
    auto* pack = tab.findChild<QCheckBox*>(QStringLiteral("animeJanaiModelsCardCheck"));
    auto* status = tab.findChild<QLabel*>(QStringLiteral("nativeComponentStatus"));
    auto* apply = tab.findChild<QPushButton*>(QStringLiteral("primary"));
    auto* cancel = tab.findChild<QPushButton*>(QStringLiteral("nativeComponentTabCancelButton"));
    QVERIFY(pack != nullptr);
    QVERIFY(status != nullptr);
    // The inventory now lands on a worker; the card state and the recommended
    // selections are published only when it does. Constructions must read that
    // state, so the first assertions wait for the snapshot with a bounded
    // deadline rather than reading whatever the constructor rendered.
    QElapsedTimer inventoryDeadline;
    inventoryDeadline.start();
    while (!tab.inventoryReady() && inventoryDeadline.elapsed() < 5000) {
        QApplication::processEvents();
        QTest::qWait(10);
    }
    QVERIFY(tab.inventoryReady());
    auto* packState = tab.findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardState"));
    auto* packSize = tab.findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardSize"));
    QVERIFY(packState != nullptr);
    QVERIFY(packSize != nullptr);
    QVERIFY(packState->text().contains(
        QStringLiteral("%1/%2").arg(corruptRow ? 1 : 0).arg(catalog.size())));
    QVERIFY(packState->text().contains(QStringLiteral("0/%1").arg(catalog.size())));
    QVERIFY(apply != nullptr);
    QVERIFY(cancel != nullptr);
    QVERIFY(pack->isChecked());
    QVERIFY(!pack->accessibleName().isEmpty());
    QVERIFY(pack->focusPolicy() != Qt::NoFocus);
    QVERIFY(!pack->testAttribute(Qt::WA_TransparentForMouseEvents));
    QVERIFY(status->text().isEmpty());  // Construction never initiates acquisition.
    for (QCheckBox* box : tab.findChildren<QCheckBox*>()) {
        if (box->isChecked()) box->click();
    }
    QVERIFY(!pack->isChecked());
    pack->click();
    QVERIFY(pack->isChecked());
    for (QCheckBox* box : tab.findChildren<QCheckBox*>()) {
        if (box != pack) QVERIFY(!box->isChecked());
    }

    qint64 expectedBytes = 0;
    for (const SpatialAsset& asset : catalog) expectedBytes += asset.bytes;
    QCOMPARE(packSize->text(), QLocale().formattedDataSize(expectedBytes));
    bool reviewTimedOut = false;
    auto review = [&](QMessageBox::StandardButton answer) {
        QString prompt;
        reviewTimedOut = false;
        QElapsedTimer elapsed;
        elapsed.start();
        QTimer modalResponse;
        connect(&modalResponse, &QTimer::timeout, &tab, [&] {
            // Ownership identifies this real review even when Windows has not
            // registered it as the application's active modal widget yet.
            auto* dialog = tab.findChild<QMessageBox*>();
            if (elapsed.hasExpired(5000)) {
                reviewTimedOut = true;
                if (dialog != nullptr) {
                    modalResponse.stop();
                    dialog->reject();
                }
                return;
            }
            if (dialog == nullptr || !dialog->isVisible() || !prompt.isEmpty()) return;
            QAbstractButton* button = dialog->button(answer);
            if (button == nullptr) return;
            prompt = dialog->text();
            // Keep the deadline armed until the synchronous review returns:
            // a click that fails to close the dialog must fail, not hang.
            button->click();
        });
        modalResponse.start(10);
        tab.downloadMissing();
        QElapsedTimer checkDeadline;
        checkDeadline.start();
        while (prompt.isEmpty() && !reviewTimedOut && checkDeadline.elapsed() < 5000) {
            QApplication::processEvents();
            QTest::qWait(10);
        }
        return prompt;
    };

    for (QMessageBox::StandardButton answer : {QMessageBox::Cancel, QMessageBox::Yes}) {
        const QString prompt = review(answer);
        QVERIFY2(!reviewTimedOut, "The Components installation review could not be answered within 5 seconds.");
        QVERIFY(!prompt.isEmpty());
        for (const SpatialAsset& asset : catalog) {
            QVERIFY2(prompt.contains(asset.name), qPrintable(prompt));
        }
        QVERIFY2(prompt.contains(QLocale().formattedDataSize(expectedBytes)), qPrintable(prompt));
        if (answer == QMessageBox::Cancel) {
            QVERIFY(tab.statusKey().isEmpty());
            // The transient "Checking selected components..." label must be
            // gone after the user declines the review.
            QVERIFY(status->text().isEmpty());
            QVERIFY(pack->isChecked());
        }
    }
    QElapsedTimer idleDeadline;
    idleDeadline.start();
    while (tab.acquisitionRunning() && idleDeadline.elapsed() < 5000) {
        QApplication::processEvents();
        QTest::qWait(10);
    }
    QVERIFY(!tab.acquisitionRunning());
    runner.waitForDone(5000);
    // A null client stops before any network request, but only after the real
    // consumer has reviewed and queued the selected pinned models.
    QCOMPARE(tab.statusKey(), QStringLiteral("native.components.noClient"));
    QVERIFY(!status->text().isEmpty());
    QCOMPARE(status->text(), tab.statusReason());
    QCOMPARE(status->accessibleDescription(), QStringLiteral("failure"));
    QVERIFY(apply->isEnabled());
    QVERIFY(pack->isEnabled());
    QVERIFY(!cancel->isEnabled());

    apply->click();
    QCOMPARE(tab.statusKey(), QStringLiteral("native.components.noClient"));
    QCOMPARE(status->accessibleDescription(), QStringLiteral("failure"));
    QVERIFY(pack->isChecked());
}

void TestNativeComponents::concurrentUnitsSettleOnceEachAndCancelArmsEveryUnit() {
    // The acquisition units now run side by side instead of as one chain, so
    // the run's lifecycle has its own public shape: one Apply starts the
    // selected work, the run settles exactly once through its own finalizer,
    // and while a run is up the Cancel action is armed. A null client makes
    // the spatial unit refuse before any network, which is what this test
    // watches without touching a mirror.
    mikai::app::Runner runner;
    mikai::app::ComponentStoreTab tab;
    tab.setRunner(&runner);
    auto* pack = tab.findChild<QCheckBox*>(QStringLiteral("animeJanaiModelsCardCheck"));
    auto* status = tab.findChild<QLabel*>(QStringLiteral("nativeComponentStatus"));
    auto* cancel = tab.findChild<QPushButton*>(QStringLiteral("nativeComponentTabCancelButton"));
    QVERIFY(pack != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(cancel != nullptr);
    QElapsedTimer inventoryDeadline;
    inventoryDeadline.start();
    while (!tab.inventoryReady() && inventoryDeadline.elapsed() < 5000) {
        QApplication::processEvents();
        QTest::qWait(10);
    }
    QVERIFY(tab.inventoryReady());

    // Only the spatial unit refuses hermetically; the backend and RIFE units
    // would hit the network, so the coordination contract is exercised with
    // the spatial unit and the null client.
    for (QCheckBox* box : tab.findChildren<QCheckBox*>()) {
        if (box->isChecked()) box->click();
    }
    pack->setChecked(true);
    bool answered = false;
    QTimer modalResponse;
    connect(&modalResponse, &QTimer::timeout, &tab, [&] {
        auto* dialog = tab.findChild<QMessageBox*>();
        if (dialog == nullptr || !dialog->isVisible()) return;
        QAbstractButton* button = dialog->button(QMessageBox::Yes);
        if (button == nullptr) return;
        answered = true;
        button->click();
    });
    modalResponse.start(10);
    tab.downloadMissing();
    QElapsedTimer settleDeadline;
    settleDeadline.start();
    while (tab.acquisitionRunning() && settleDeadline.elapsed() < 5000) {
        QApplication::processEvents();
        QTest::qWait(10);
    }
    QVERIFY(answered);
    QVERIFY(!tab.acquisitionRunning());
    runner.waitForDone(5000);
    QVERIFY(!cancel->isEnabled());
    QCOMPARE(tab.statusKey(), QStringLiteral("native.components.noClient"));
    QVERIFY(!status->text().isEmpty());

    // A second Apply must be accepted after the run settled: the finalizer
    // cleared every busy latch, so the surface is not permanently stuck.
    tab.downloadMissing();
    QElapsedTimer refill;
    refill.start();
    while (!tab.acquisitionRunning() && refill.elapsed() < 5000) {
        QApplication::processEvents();
        QTest::qWait(10);
    }
    QVERIFY(tab.acquisitionRunning());
    QVERIFY(cancel->isEnabled());
    cancel->click();
    QElapsedTimer idleDeadline;
    idleDeadline.start();
    while (tab.acquisitionRunning() && idleDeadline.elapsed() < 5000) {
        QApplication::processEvents();
        QTest::qWait(10);
    }
    QVERIFY(!tab.acquisitionRunning());
    QVERIFY(!cancel->isEnabled());
    runner.waitForDone(5000);
}

void TestNativeComponents::upscalingSelectionSurvivesRefreshAndMissingGraphsFailVerification() {
    QVERIFY(storedSpatialAssets().isEmpty());
    mikai::app::Runner inventoryRunner;
    mikai::app::ComponentStoreTab tab;
    tab.setRunner(&inventoryRunner);
    auto* pack = tab.findChild<QCheckBox*>(QStringLiteral("animeJanaiModelsCardCheck"));
    auto* status = tab.findChild<QLabel*>(QStringLiteral("nativeComponentStatus"));
    QVERIFY(pack != nullptr);
    QVERIFY(status != nullptr);
    QElapsedTimer inventoryDeadline;
    inventoryDeadline.start();
    while (!tab.inventoryReady() && inventoryDeadline.elapsed() < 5000) {
        QApplication::processEvents();
        QTest::qWait(10);
    }
    QVERIFY(tab.inventoryReady());
    QVERIFY(pack->isChecked());
    pack->click();
    QVERIFY(!pack->isChecked());
    tab.refresh();
    QVERIFY(!pack->isChecked());
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&tab, &languageChange);
    QVERIFY(!pack->isChecked());
    QVERIFY(!pack->accessibleName().isEmpty());

    // Verify is off-GUI: the inventories are hashed on a worker and the verdict
    // is published on the GUI thread. The refresh above starts its own
    // inventory scan, which disables the Verify action until it lands, so the
    // click below waits for the surface to be idle again. Every assertion then
    // waits with a bounded deadline for the publication, so a run that never
    // completes fails the test instead of hanging it.
    QElapsedTimer idleDeadline;
    idleDeadline.start();
    while (tab.acquisitionRunning() && idleDeadline.elapsed() < 5000) {
        QApplication::processEvents();
        QTest::qWait(10);
    }
    QVERIFY(!tab.acquisitionRunning());
    mikai::app::Runner verifyRunner;
    tab.setRunner(&verifyRunner);
    auto* verifyButton = tab.findChild<QPushButton*>(QStringLiteral("nativeComponentVerifyButton"));
    QVERIFY(verifyButton != nullptr);
    QVERIFY(verifyButton->isEnabled());
    verifyButton->click();
    QElapsedTimer verifyDeadline;
    verifyDeadline.start();
    while (tab.statusKey().isEmpty() && verifyDeadline.elapsed() < 5000) {
        QApplication::processEvents();
        QTest::qWait(10);
    }
    QApplication::processEvents();
    QCOMPARE(tab.statusKey(), QStringLiteral("native.components.digest"));
    QCOMPARE(status->accessibleDescription(), QStringLiteral("failure"));
    QVERIFY(!status->text().isEmpty());
    QVERIFY(!pack->isChecked());
    QVERIFY(pack->isEnabled());
    QVERIFY(verifyButton->isEnabled());
    verifyRunner.waitForDone(5000);

    // A separate, explicit setup confirmation re-evaluates recommendations;
    // refresh, retranslation and the normal Apply action never do so.
    tab.installRecommended();
    QVERIFY(pack->isChecked());
    QCOMPARE(status->accessibleDescription(), QStringLiteral("failure"));
    QVERIFY(!status->text().isEmpty());
}

void TestNativeComponents::cancelledTensorRtProvisioningDoesNotTouchItsFixtures() {
    QTemporaryDir storeRoot;
    QTemporaryDir scratch;
    QVERIFY(storeRoot.isValid());
    QVERIFY(scratch.isValid());
    const QStringList scratchBefore = QDir(scratch.path()).entryList(QDir::NoDotAndDotDot);
    const QVector<ComponentPack> packs{*tensorRtRuntimePack()};
    const auto result = provisionTensorRtComponents(
        ComponentStore{storeRoot.path()}, packs, scratch.path(), {},
        [] { return true; });
    QVERIFY(result.cancelled);
    QVERIFY(!result.ok);
    QCOMPARE(result.reasonKey, QStringLiteral("native.components.cancelled"));
    QCOMPARE(QDir(storeRoot.path()).entryList(QDir::NoDotAndDotDot), QStringList{});
    QCOMPARE(QDir(scratch.path()).entryList(QDir::NoDotAndDotDot), scratchBefore);
}

void TestNativeComponents::cancelledRifeProvisioningDoesNotTouchItsFixtures() {
    QTemporaryDir storeRoot;
    QVERIFY(storeRoot.isValid());
    const auto result = provisionRifeArchive(
        RifeArchiveStore{storeRoot.path()}, rifeModelPins(), {}, [] { return true; });
    QVERIFY(result.cancelled);
    QVERIFY(!result.ok);
    QCOMPARE(result.reasonKey, QStringLiteral("native.components.cancelled"));
    QCOMPARE(QDir(storeRoot.path()).entryList(QDir::NoDotAndDotDot), QStringList{});
}
void TestNativeComponents::selectionUsesTheExactPack() {
    struct Generation {
        int smMajor;
        int smMinor;
        const char* pack;
    };
    const Generation generations[] = {{12, 0, "trt-sm120"}, {7, 5, "trt-sm75"}, {8, 6, "trt-sm86"},
                                     {9, 0, "trt-sm90"},  {10, 0, "trt-sm100"}};
#ifdef Q_OS_WIN
    const QString pattern = QStringLiteral("nvinfer_builder_resource_sm%1%2_11.dll");
#else
    const QString pattern = QStringLiteral("libnvinfer_builder_resource_sm%1%2_11.so");
#endif
    for (const Generation& generation : generations) {
        const ComponentPack* selected =
            tensorRtResourcePackFor(generation.smMajor, generation.smMinor);
        QVERIFY2(selected != nullptr, generation.pack);
        QCOMPARE(selected->name, QString::fromLatin1(generation.pack));
        QCOMPARE(resourceNameOf(selected),
                 pattern.arg(generation.smMajor).arg(generation.smMinor));
    }
    QCOMPARE(tensorRtWindowsComponentCatalog().size(), 9);
    QCOMPARE(tensorRtLinuxComponentCatalog().size(), 9);
    QCOMPARE(rifeWindowsArchivePack().asset, QStringLiteral("component-rife.7z"));
    QCOMPARE(rifeLinuxArchivePack().asset, QStringLiteral("component-rife-linux-x64.7z"));
}

void TestNativeComponents::selectionFallsBackToPtxWhenTheReleasePublishesNoPack() {
    struct Shape {
        int smMajor;
        int smMinor;
    };
    const Shape shapes[] = {{6, 1}, {10, 1}};
#ifdef Q_OS_WIN
    const QString expectedPtx = QStringLiteral("nvinfer_builder_resource_ptx_11.dll");
#else
    const QString expectedPtx = QStringLiteral("libnvinfer_builder_resource_ptx_11.so");
#endif
    for (const Shape& shape : shapes) {
        const ComponentPack* selected = tensorRtResourcePackFor(shape.smMajor, shape.smMinor);
        QVERIFY(selected != nullptr);
        QCOMPARE(selected->name, QStringLiteral("trt-ptx"));
        QCOMPARE(selected->resourceFile, expectedPtx);
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
    const ComponentPack* sm120 = packNamed(QStringLiteral("trt-sm120"));
    QVERIFY(sm120 != nullptr);
    const ComponentBinDirs dirs =
        componentBinDirs(bundle.path(), trtDir, QStringLiteral("cuda"),
                         {trtDir + QLatin1Char('/') + sm120->resourceFile},
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

    QString removeKey;
    QString removeReason;
    QVERIFY2(removePack(store, *resource, &removeKey, &removeReason),
             qPrintable(removeKey + QLatin1String(": ") + removeReason));
    QCOMPARE(installedPackNames(store), QStringList({runtime->name}));
    QVERIFY(!storedPack(store, *resource).installed);
    QVERIFY(storedPack(store, *runtime).installed);
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