#include "TestAnimeJanaiRuntime.hpp"

#include "PlayerDialog.hpp"
#include "VideoSurface.hpp"
#include "../../fixtures/IdentityOnnxFixture.hpp"
#include "backend/Cancellation.hpp"
#include "native/bundle/NativePaths.hpp"
#include "native/components/AnimeJanaiCatalogWatcher.hpp"
#include "native/components/AnimeJanaiConfig.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLibraryInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QScopeGuard>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QProcess>
#include <QtTest/QSignalSpy>

#include <algorithm>
#include <atomic>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

using namespace mplayer::native;

namespace {

QString fixtureRoot() {
    return QDir(nativeSubdirectory(QStringLiteral("components")))
        .filePath(QStringLiteral("aji/v0.9.0"));
}

QString runtimePath(Backend backend = Backend::TensorRT) {
    const QString name = backend == Backend::TensorRT ? QStringLiteral("trt") : QStringLiteral("dml");
    return QDir(fixtureRoot()).filePath(name);
}

/// A valid identity ModelProto whose metadata tag changes the file size, so a
/// replacement is observable from public inventory without graph inspection.
QByteArray taggedIdentityGraph(const QByteArray& tag) {
    QByteArray graph = mikai::tests::fixtures::makeTestIdentityOnnxGraph();
    QByteArray metadata;
    metadata.append(char(0x0a));
    metadata.append(char(1));
    metadata.append('k');
    metadata.append(char(0x12));
    metadata.append(static_cast<char>(tag.size()));
    metadata.append(tag);
    graph.append(char(0x72));  // ModelProto.metadata_props
    graph.append(static_cast<char>(metadata.size()));
    graph.append(metadata);
    return graph;
}

bool listsFileNamed(const QVector<CustomAnimeJanaiModel>& models, const QString& fileName) {
    return std::any_of(models.begin(), models.end(),
                       [&fileName](const CustomAnimeJanaiModel& model) {
                           return model.fileName == fileName;
                       });
}

const CustomAnimeJanaiModel* modelNamed(const QVector<CustomAnimeJanaiModel>& models,
                                        const QString& fileName) {
    for (const CustomAnimeJanaiModel& model : models) {
        if (model.fileName == fileName) return &model;
    }
    return nullptr;
}

/// Rewrites the final byte in place, keeping size and leaving the graph valid.
bool rewriteLastByteSameSize(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) return false;
    const qint64 size = file.size();
    if (size <= 0 || !file.seek(size - 1)) return false;
    const char replacement = 0x78;
    return file.write(&replacement, 1) == 1 && file.flush();
}

bool writeFile(const QString& path, const QByteArray& data) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

QString sha(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray minimalX64Pe() {
    QByteArray image(90, '\0');
    image[0] = 'M'; image[1] = 'Z';
    image[0x3c] = 64;
    image[64] = 'P'; image[65] = 'E';
    image[68] = char(0x64); image[69] = char(0x86);
    image[88] = char(0x0b); image[89] = char(0x02);
    return image;
}

QStringList required(Backend backend) {
    QStringList files{QStringLiteral("aji.dll"), backend == Backend::TensorRT ? QStringLiteral("aji_trt.dll") : QStringLiteral("aji_dml.dll")};
    if (backend == Backend::TensorRT) {
        files << QStringLiteral("cudart64_13.dll") << QStringLiteral("nvinfer_11.dll")
              << QStringLiteral("nvinfer_plugin_11.dll") << QStringLiteral("nvonnxparser_11.dll")
              << QStringLiteral("nvinfer_builder_resource_sm120_11.dll") << QStringLiteral("trtexec.exe")
              << QStringLiteral("CUDA_LICENSE.txt") << QStringLiteral("DirectML_LICENSE.txt");
    } else {
        files << QStringLiteral("onnxruntime.dll") << QStringLiteral("DirectML.dll")
              << QStringLiteral("DirectML_LICENSE.txt");
    }
    return files;
}

bool writeRecord(Backend backend, const QString& recordedBackend = {},
                 bool abiExportingDispatcher = false) {
    const QString root = runtimePath(backend);
    QJsonArray entries;
    for (const QString& name : required(backend)) {
        QByteArray bytes = minimalX64Pe();
#ifdef MIKAI_TEST_AJI_DISPATCHER
        if (abiExportingDispatcher && name == QLatin1String("aji.dll")) {
            QFile dispatcher(QString::fromUtf8(MIKAI_TEST_AJI_DISPATCHER));
            if (!dispatcher.open(QIODevice::ReadOnly)) return false;
            bytes = dispatcher.readAll();
        }
#endif
        if (!writeFile(QDir(root).filePath(name), bytes)) return false;
        QJsonObject e; e.insert(QStringLiteral("path"), name); e.insert(QStringLiteral("bytes"), bytes.size()); e.insert(QStringLiteral("sha256"), sha(bytes)); entries.append(e);
    }
    QJsonObject record;
    record.insert(QStringLiteral("schema"), QStringLiteral("mikai.native.animejanai/2"));
    record.insert(QStringLiteral("version"), QStringLiteral("v0.9.0"));
    record.insert(QStringLiteral("backend"), recordedBackend.isEmpty() ? (backend == Backend::TensorRT ? QStringLiteral("trt") : QStringLiteral("dml")) : recordedBackend);
    record.insert(QStringLiteral("abi"), 8);
    record.insert(QStringLiteral("provenance"), QStringLiteral("local"));
    record.insert(QStringLiteral("archiveBytes"), 0);
    record.insert(QStringLiteral("archiveSha256"), QString());
    record.insert(QStringLiteral("heavyRifeSupported"), abiExportingDispatcher);
    record.insert(QStringLiteral("files"), entries);
    record.insert(QStringLiteral("acquisitions"), QJsonArray{});
    return writeFile(QDir(root).filePath(QStringLiteral("install.json")), QJsonDocument(record).toJson(QJsonDocument::Compact));
}

bool updateRecordFile(const QString& root, const QString& name, const QByteArray& bytes) {
    QFile record(QDir(root).filePath(QStringLiteral("install.json")));
    if (!record.open(QIODevice::ReadOnly)) return false;
    QJsonObject object = QJsonDocument::fromJson(record.readAll()).object();
    record.close();
    QJsonArray files = object.value(QStringLiteral("files")).toArray();
    for (qsizetype i = 0; i < files.size(); ++i) {
        QJsonObject entry = files.at(i).toObject();
        if (entry.value(QStringLiteral("path")).toString() != name) continue;
        entry.insert(QStringLiteral("bytes"), bytes.size());
        entry.insert(QStringLiteral("sha256"), sha(bytes));
        files.replace(i, entry);
        object.insert(QStringLiteral("files"), files);
        return writeFile(record.fileName(), QJsonDocument(object).toJson(QJsonDocument::Compact));
    }
    return false;
}

} // namespace

void TestAnimeJanaiRuntime::initTestCase() {
    QVERIFY2(!QFileInfo::exists(fixtureRoot()), "AnimeJaNai fixture root is already occupied");
    const QString animeJanaiRoot = QDir(nativeRoot()).filePath(QStringLiteral("animejanai"));
    QVERIFY2(!QFileInfo::exists(animeJanaiRoot), "AnimeJaNai model/config data already exists");
    QVERIFY(QDir().mkpath(fixtureRoot()));
    ownsFixture_ = true;
    ownsAnimeJanai_ = true;
}

void TestAnimeJanaiRuntime::init() {
    QVERIFY(ownsFixture_);
    QVERIFY(QDir(runtimePath(Backend::DirectML)).removeRecursively());
    QVERIFY(QDir(runtimePath(Backend::TensorRT)).removeRecursively());
    if (ownsAnimeJanai_) QDir(QDir(nativeRoot()).filePath(QStringLiteral("animejanai"))).removeRecursively();
}

void TestAnimeJanaiRuntime::cleanup() {
    if (!ownsFixture_) return;
    QVERIFY(QDir(runtimePath(Backend::DirectML)).removeRecursively());
    QVERIFY(QDir(runtimePath(Backend::TensorRT)).removeRecursively());
    if (ownsAnimeJanai_) QDir(QDir(nativeRoot()).filePath(QStringLiteral("animejanai"))).removeRecursively();
}

void TestAnimeJanaiRuntime::cleanupTestCase() {
    if (!ownsFixture_) return;
    QVERIFY(QDir(fixtureRoot()).removeRecursively());
    if (ownsAnimeJanai_) QDir(QDir(nativeRoot()).filePath(QStringLiteral("animejanai"))).removeRecursively();
    ownsAnimeJanai_ = false;
    ownsFixture_ = false;
}

void TestAnimeJanaiRuntime::directRunRefusesExistingNativeTreeWithoutMutation() {
    QTemporaryDir installed;
    QVERIFY(installed.isValid());
    const QString executable = QDir(installed.path())
        .filePath(QFileInfo(QCoreApplication::applicationFilePath()).fileName());
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), executable));
    const QString recordPath = QDir(installed.path())
        .filePath(QStringLiteral("native/components/aji/v0.9.0/trt/install.json"));
    const QString dependencyPath = QDir(installed.path())
        .filePath(QStringLiteral("native/components/aji/v0.9.0/trt/aji.dll"));
    const QByteArray recordBefore = QByteArrayLiteral("owned installed record: must not be overwritten");
    const QByteArray dependencyBefore = QByteArrayLiteral("owned installed dependency");
    QVERIFY(writeFile(recordPath, recordBefore));
    QVERIFY(writeFile(dependencyPath, dependencyBefore));

    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PATH"),
        QLibraryInfo::path(QLibraryInfo::BinariesPath) + QDir::listSeparator() +
        environment.value(QStringLiteral("PATH")));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_PLUGIN_PATH"),
        QLibraryInfo::path(QLibraryInfo::PluginsPath));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"),
        QDir(QLibraryInfo::path(QLibraryInfo::PluginsPath)).filePath(QStringLiteral("platforms")));
    child.setProcessEnvironment(environment);
    child.setWorkingDirectory(installed.path());
    // This real test function writes the exact install.json that triggered the
    // incident if the entry guard regresses. Do not recursively run this test.
    child.start(executable, {QStringLiteral("TestAnimeJanaiRuntime"),
                            QStringLiteral("cancelledImportDoesNotPublish")});
    QVERIFY2(child.waitForStarted(), qPrintable(child.errorString()));
    if (!child.waitForFinished(30000)) {
        child.kill();
        child.waitForFinished();
        QFAIL("direct test invocation did not finish");
    }
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 2);
    QVERIFY(child.readAllStandardError().contains("refusing native fixture mutation"));
    QFile record(recordPath);
    QVERIFY(record.open(QIODevice::ReadOnly));
    QCOMPARE(record.readAll(), recordBefore);
    QFile dependency(dependencyPath);
    QVERIFY(dependency.open(QIODevice::ReadOnly));
    QCOMPARE(dependency.readAll(), dependencyBefore);
    QCOMPARE(QDir(installed.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot),
             QStringList{QStringLiteral("native")});
}

void TestAnimeJanaiRuntime::missingInstallIsNotReady() {
    const auto result = describeAnimeJanaiInstallation(Backend::DirectML);
    QVERIFY(!result.installed);
    QCOMPARE(result.reasonKey, QStringLiteral("animejanai.installation.recordMissing"));
}

void TestAnimeJanaiRuntime::missingRequiredBackendDependencyIsNotReady() {
    QVERIFY(writeRecord(Backend::DirectML));
    QVERIFY(QFile::remove(QDir(runtimePath(Backend::DirectML)).filePath(QStringLiteral("DirectML.dll"))));
    const auto result = describeAnimeJanaiInstallation(Backend::DirectML);
    QVERIFY(!result.installed);
    QCOMPARE(result.reasonKey, QStringLiteral("animejanai.installation.fileInvalid"));
    QVERIFY(result.reason.contains(QStringLiteral("DirectML.dll")));
}

void TestAnimeJanaiRuntime::storedFailedRequestReprobesAfterDependencyInstall() {
    QVERIFY(writeRecord(Backend::DirectML));
    const QString dependency = QDir(runtimePath(Backend::DirectML))
        .filePath(QStringLiteral("DirectML.dll"));
    QVERIFY(QFile::remove(dependency));

    QSettings settings;
    const QString presetKey = QStringLiteral("player/animejanai/preset");
    const QString backendKey = QStringLiteral("player/animejanai/backend");
    const QVariant priorPreset = settings.value(presetKey);
    const QVariant priorBackend = settings.value(backendKey);
    const auto restoreSettings = qScopeGuard([&] {
        if (priorPreset.isValid()) settings.setValue(presetKey, priorPreset);
        else settings.remove(presetKey);
        if (priorBackend.isValid()) settings.setValue(backendKey, priorBackend);
        else settings.remove(backendKey);
    });
    settings.setValue(presetKey, QStringLiteral("builtin1002"));
    settings.setValue(backendKey, QStringLiteral("directml"));

#ifdef Q_OS_WIN
    // The worker deliberately loads a fake PE; keep its expected loader refusal
    // out of Windows hard-error UI for this isolated test process.
    const UINT priorErrorMode = SetErrorMode(
        GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    const auto restoreErrorMode = qScopeGuard([priorErrorMode] {
        SetErrorMode(priorErrorMode);
    });
#endif

    mplayer::shell::PlayerDialog dialog(QStringLiteral("Dependency retry"), QString());
    auto* surface = dialog.findChild<mplayer::VideoSurface*>();
    QVERIFY(surface != nullptr);
    QTRY_COMPARE(surface->animeJanaiPlan().reasonKey,
                 QStringLiteral("animejanai.installation.fileInvalid"));
    QVERIFY(surface->animeJanaiPlan().enabled);
    QVERIFY(!surface->animeJanaiPlan().available);
    QVERIFY(surface->animeJanaiPlan().reason.contains(QStringLiteral("DirectML.dll")));

    QVERIFY(writeFile(dependency, minimalX64Pe()));
    dialog.refreshStoredSettings();

    // The fake dispatcher cannot load, but restoring its dependency must expose
    // that next consumer failure instead of retaining the missing-file result.
    QTRY_COMPARE(surface->animeJanaiPlan().reasonKey,
                 QStringLiteral("animejanai.installation.abiInvalid"));
    QVERIFY(surface->animeJanaiPlan().enabled);
    QVERIFY(!surface->animeJanaiPlan().available);
    QCOMPARE(surface->animeJanaiPlan().presetId, QStringLiteral("builtin1002"));
    QCOMPARE(settings.value(presetKey).toString(), QStringLiteral("builtin1002"));
    QCOMPARE(settings.value(backendKey).toString(), QStringLiteral("directml"));
}

void TestAnimeJanaiRuntime::backendMismatchIsNotReady() {
    QVERIFY(writeRecord(Backend::TensorRT, QStringLiteral("dml")));
    const auto result = describeAnimeJanaiInstallation(Backend::TensorRT);
    QVERIFY(!result.installed);
    QCOMPARE(result.reasonKey, QStringLiteral("animejanai.installation.recordInvalid"));
}

void TestAnimeJanaiRuntime::sizeAndDigestCorruptionAreRejected() {
    QVERIFY(writeRecord(Backend::DirectML));
    const QString dispatcher = QDir(runtimePath(Backend::DirectML)).filePath(QStringLiteral("aji.dll"));
    const QByteArray original = minimalX64Pe();
    QVERIFY(writeFile(dispatcher, original + 'x'));
    auto sizeChanged = describeAnimeJanaiInstallation(Backend::DirectML);
    QVERIFY(!sizeChanged.installed);
    QCOMPARE(sizeChanged.reasonKey, QStringLiteral("animejanai.installation.fileInvalid"));
    QVERIFY(writeFile(dispatcher, QByteArrayLiteral("corrupt PE fixture:aji.dll")));
    auto digestChanged = describeAnimeJanaiInstallation(Backend::DirectML, true);
    QVERIFY(!digestChanged.installed);
    QCOMPARE(digestChanged.reasonKey, QStringLiteral("animejanai.installation.fileInvalid"));
}

void TestAnimeJanaiRuntime::badPeAndUnsafeRecordPathAreRejected() {
    QVERIFY(writeRecord(Backend::DirectML));
    const QString root = runtimePath(Backend::DirectML);
    const QByteArray invalidPe = QByteArrayLiteral("not a PE image");
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("aji.dll")), invalidPe));
    QVERIFY(updateRecordFile(root, QStringLiteral("aji.dll"), invalidPe));
    auto badPe = describeAnimeJanaiInstallation(Backend::DirectML, true);
    QVERIFY(!badPe.installed);
    QCOMPARE(badPe.reasonKey, QStringLiteral("animejanai.installation.peInvalid"));

    QFile record(QDir(root).filePath(QStringLiteral("install.json")));
    QVERIFY(record.open(QIODevice::ReadOnly));
    QJsonObject object = QJsonDocument::fromJson(record.readAll()).object();
    record.close();
    QJsonArray files = object.value(QStringLiteral("files")).toArray();
    QJsonObject first = files.first().toObject();
    first.insert(QStringLiteral("path"), QStringLiteral("../aji.dll"));
    files.replace(0, first);
    object.insert(QStringLiteral("files"), files);
    QVERIFY(writeFile(record.fileName(), QJsonDocument(object).toJson(QJsonDocument::Compact)));
    const auto unsafe = describeAnimeJanaiInstallation(Backend::DirectML);
    QVERIFY(!unsafe.installed);
    QCOMPARE(unsafe.reasonKey, QStringLiteral("animejanai.installation.recordInvalid"));
}

void TestAnimeJanaiRuntime::wrongArchitectureIsRejected() {
    QVERIFY(writeRecord(Backend::DirectML));
    QByteArray image(70, '\0');
    image[0] = 'M'; image[1] = 'Z';
    image[0x3c] = 64;
    image[64] = 'P'; image[65] = 'E';
    image[68] = 0x4c; image[69] = 0x01;
    const QString root = runtimePath(Backend::DirectML);
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("aji.dll")), image));
    QFile record(QDir(root).filePath(QStringLiteral("install.json")));
    QVERIFY(record.open(QIODevice::ReadOnly));
    QJsonObject object = QJsonDocument::fromJson(record.readAll()).object();
    record.close();
    QJsonArray files = object.value(QStringLiteral("files")).toArray();
    QJsonObject dispatcher = files.first().toObject();
    dispatcher.insert(QStringLiteral("bytes"), image.size());
    dispatcher.insert(QStringLiteral("sha256"), sha(image));
    files.replace(0, dispatcher);
    object.insert(QStringLiteral("files"), files);
    QVERIFY(writeFile(record.fileName(), QJsonDocument(object).toJson(QJsonDocument::Compact)));
    const auto result = describeAnimeJanaiInstallation(Backend::DirectML, true);
    QVERIFY(!result.installed);
    QCOMPARE(result.reasonKey, QStringLiteral("animejanai.installation.peInvalid"));
}

void TestAnimeJanaiRuntime::reparseDependencyIsRejected() {
#ifdef Q_OS_WIN
    QVERIFY(writeRecord(Backend::DirectML));
    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    QVERIFY(writeFile(QDir(outside.path()).filePath(QStringLiteral("payload")), QByteArrayLiteral("outside")));
    const QString junction = QDir(runtimePath(Backend::DirectML)).filePath(QStringLiteral("DirectML.dll"));
    QVERIFY(QFile::remove(junction));
    QProcess create;
    create.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("mklink"),
        QStringLiteral("/J"), junction, outside.path()});
    if (!create.waitForStarted() || !create.waitForFinished() || create.exitCode() != 0)
        QSKIP("Windows did not permit creating a temporary directory junction.");
    const auto removeJunction = qScopeGuard([&] {
        QProcess remove;
        remove.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("rmdir"), junction});
        remove.waitForFinished();
    });
    const auto result = describeAnimeJanaiInstallation(Backend::DirectML);
    QVERIFY(!result.installed);
    QVERIFY(result.reasonKey == QStringLiteral("animejanai.installation.fileInvalid") ||
            result.reasonKey == QStringLiteral("animejanai.installation.pathInvalid"));
#else
    QSKIP("The AnimeJaNai runtime is Windows-only.");
#endif
}

void TestAnimeJanaiRuntime::cancelledImportDoesNotPublish() {
    const QString root = runtimePath(Backend::TensorRT);
    QVERIFY(QDir().mkpath(root));
    const QByteArray sentinel = QByteArrayLiteral("prior installation remains untouched");
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("install.json")), sentinel));
    QTemporaryDir source;
    QVERIFY(source.isValid());
    const auto result = importAnimeJanaiRuntime(source.path(), Backend::TensorRT, {}, [] { return true; });
    QVERIFY(!result.ok);
    QVERIFY(result.cancelled);
    QFile record(QDir(root).filePath(QStringLiteral("install.json")));
    QVERIFY(record.open(QIODevice::ReadOnly));
    QCOMPARE(record.readAll(), sentinel);
}
void TestAnimeJanaiRuntime::cancelledReplacementPreservesPriorInstall() {
    const QString root = runtimePath(Backend::DirectML);
    QVERIFY(QDir().mkpath(root));
    const QByteArray sentinel = QByteArrayLiteral("previous verified runtime record");
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("install.json")), sentinel));
    QTemporaryDir source;
    QVERIFY(source.isValid());
    for (const QString& name : required(Backend::DirectML))
        QVERIFY(writeFile(QDir(source.path()).filePath(name), QByteArrayLiteral("replacement candidate")));

    int cancellationChecks = 0;
    const auto result = importAnimeJanaiRuntime(source.path(), Backend::DirectML, {},
        [&cancellationChecks] { return ++cancellationChecks >= 4; });
    QVERIFY(!result.ok);
    QVERIFY(result.cancelled);
    QFile record(QDir(root).filePath(QStringLiteral("install.json")));
    QVERIFY(record.open(QIODevice::ReadOnly));
    QCOMPARE(record.readAll(), sentinel);
}

void TestAnimeJanaiRuntime::unsupportedDispatcherIsNotReady() {
    QVERIFY(writeRecord(Backend::DirectML));
    QFile record(QDir(runtimePath(Backend::DirectML)).filePath(QStringLiteral("install.json")));
    QVERIFY(record.open(QIODevice::ReadOnly));
    QJsonObject object = QJsonDocument::fromJson(record.readAll()).object();
    record.close();
    object.insert(QStringLiteral("schema"), QStringLiteral("unknown"));
    QVERIFY(writeFile(record.fileName(), QJsonDocument(object).toJson(QJsonDocument::Compact)));
    const auto result = describeAnimeJanaiInstallation(Backend::DirectML, true);
    QVERIFY(!result.installed);
    QCOMPARE(result.reasonKey, QStringLiteral("animejanai.installation.recordInvalid"));
}

void TestAnimeJanaiRuntime::unknownModelAndDefaultPlanRemainPure() {
    const AnimeJanaiPlan off = prepareAnimeJanaiPlan(QString(), Backend::TensorRT);
    QVERIFY(!off.enabled);
    QVERIFY(!off.available);
    const AnimeJanaiPlan unknown = prepareAnimeJanaiPlan(QStringLiteral("rife:not-a-pinned-file"), Backend::TensorRT);
    QVERIFY(unknown.enabled);
    QVERIFY(!unknown.available);
    QCOMPARE(unknown.reasonKey, QStringLiteral("animejanai.preset.invalid"));
}

void TestAnimeJanaiRuntime::customOnnxImportIsAtomicAndRemovable() {

    QTemporaryDir sourceDir;
    QVERIFY(sourceDir.isValid());
    const QString source =
        QDir(sourceDir.path()).filePath(QStringLiteral("custom_identity.onnx"));
    QVERIFY(writeFile(source, mikai::tests::fixtures::makeTestIdentityOnnxGraph()));

    CustomAnimeJanaiModel imported;
    QString error;
    QVERIFY2(importCustomAnimeJanaiModel(source, &imported, &error), qPrintable(error));
    QVERIFY(imported.valid);
    QVERIFY(!imported.sha256.isEmpty());
    const QString managedPath =
        QDir(customAnimeJanaiModelsDir()).filePath(imported.fileName);
    QVERIFY(QFileInfo::exists(managedPath));

    QString duplicateError;
    QVERIFY(!importCustomAnimeJanaiModel(source, nullptr, &duplicateError));
    QVERIFY(!duplicateError.isEmpty());
    const auto registered = findCustomAnimeJanaiModel(imported.id);
    QVERIFY(registered.has_value());
    QCOMPARE(registered->sha256, imported.sha256);

    QFile priorModel(managedPath);
    QVERIFY(priorModel.open(QIODevice::ReadOnly));
    const QByteArray priorBytes = priorModel.readAll();
    priorModel.close();
    const QString cancelledSource =
        QDir(sourceDir.path()).filePath(QStringLiteral("cancelled_identity.onnx"));
    QVERIFY(writeFile(cancelledSource, mikai::tests::fixtures::makeTestIdentityOnnxGraph()));
    int cancellationChecks = 0;
    QString cancellationError;
    QVERIFY(!importCustomAnimeJanaiModel(
        cancelledSource, nullptr, &cancellationError,
        [&cancellationChecks] { return ++cancellationChecks >= 2; }));
    QVERIFY(cancellationError.contains(QStringLiteral("cancelled")));
    const QString cancelledTarget = QDir(customAnimeJanaiModelsDir())
                                        .filePath(QStringLiteral("cancelled_identity.onnx"));
    QVERIFY(!QFileInfo::exists(cancelledTarget));
    QVERIFY(!QFileInfo::exists(cancelledTarget + QStringLiteral(".source.json")));
    QFile priorModelAfter(managedPath);
    QVERIFY(priorModelAfter.open(QIODevice::ReadOnly));
    QCOMPARE(priorModelAfter.readAll(), priorBytes);
    priorModelAfter.close();

    std::atomic_bool runnerCancelled{true};
    const mikai::backend::WorkCancellationScope cancellationScope(&runnerCancelled);
    QString runnerCancellationError;
    QVERIFY(!importCustomAnimeJanaiModel(cancelledSource, nullptr, &runnerCancellationError));
    QVERIFY(!runnerCancellationError.isEmpty());

    QVERIFY(removeCustomAnimeJanaiModel(imported.id, &error));
    QVERIFY(!QFileInfo::exists(managedPath));

    QVERIFY(!findCustomAnimeJanaiModel(imported.id).has_value());
}

void TestAnimeJanaiRuntime::customModelsPrepareAndStagePinnedContent() {
#ifdef Q_OS_WIN
    QVERIFY(writeRecord(Backend::DirectML, {}, true));
    QTemporaryDir sourceDir;
    QVERIFY(sourceDir.isValid());
    const QByteArray graph = mikai::tests::fixtures::makeTestIdentityOnnxGraph();
    QString importError;
    const auto importNamed = [&](const QString& name, CustomAnimeJanaiModel* model) {
        const QString path = QDir(sourceDir.path()).filePath(name + QStringLiteral(".onnx"));
        QByteArray uniqueGraph = graph;
        const QByteArray nameBytes = name.toUtf8();
        QByteArray metadata;
        metadata.append(char(0x0a));
        metadata.append(char(1));
        metadata.append('k');
        metadata.append(char(0x12));
        metadata.append(static_cast<char>(nameBytes.size()));
        metadata.append(nameBytes);
        uniqueGraph.append(char(0x72));  // ModelProto.metadata_props
        uniqueGraph.append(static_cast<char>(metadata.size()));
        uniqueGraph.append(metadata);
        if (!writeFile(path, uniqueGraph)) return false;
        return importCustomAnimeJanaiModel(path, model, &importError) && model->valid;
    };

    CustomAnimeJanaiModel first;
    CustomAnimeJanaiModel second;
    CustomAnimeJanaiModel third;
    QVERIFY2(importNamed(QStringLiteral("4x_Custom_First"), &first), qPrintable(importError));
    QVERIFY2(importNamed(QStringLiteral("2x_Custom_Second"), &second), qPrintable(importError));
    QVERIFY2(importNamed(QStringLiteral("3x_Custom_Third"), &third), qPrintable(importError));
    QString duplicateError;
    QVERIFY(!importCustomAnimeJanaiModel(
        QDir(sourceDir.path()).filePath(QStringLiteral("4x_Custom_First.onnx")),
        nullptr, &duplicateError));
    QVERIFY(!duplicateError.isEmpty());
    const auto originalPin = findCustomAnimeJanaiModel(first.id);
    QVERIFY(originalPin.has_value());
    QCOMPARE(originalPin->sha256, first.sha256);
    const QString presetId = QStringLiteral("spatial:custom:") + first.id;
    const AnimeJanaiPlan directPlan = prepareAnimeJanaiPlan(presetId, Backend::DirectML);
    QVERIFY2(directPlan.available, qPrintable(directPlan.reason));
    const QString stagedFirst =
        QDir(directPlan.modelDir).filePath(first.sha256 + QStringLiteral(".onnx"));
    QVERIFY(QFileInfo::exists(stagedFirst));
    QVERIFY(directPlan.cacheLease != nullptr);
    directPlan.cacheLease->release();

    QFile priorConfig(directPlan.configPath);
    QVERIFY(priorConfig.open(QIODevice::ReadOnly));
    const QByteArray priorConfigBytes = priorConfig.readAll();
    priorConfig.close();

    QFile registeredGraph(first.filePath);
    QVERIFY(registeredGraph.open(QIODevice::ReadOnly));
    const QByteArray registeredBytes = registeredGraph.readAll();
    registeredGraph.close();
    QByteArray mutated = registeredBytes;
    mutated[mutated.size() - 1] ^= 1;
    QVERIFY(writeFile(first.filePath, mutated));
    const AnimeJanaiPlan changedPlan = prepareAnimeJanaiPlan(presetId, Backend::DirectML);
    QVERIFY(!changedPlan.available);
    QCOMPARE(changedPlan.reasonKey, QStringLiteral("animejanai.model.stage"));
    QVERIFY(changedPlan.filterSpec.isEmpty());
    QVERIFY(priorConfig.open(QIODevice::ReadOnly));
    QCOMPARE(priorConfig.readAll(), priorConfigBytes);
    priorConfig.close();

    QVERIFY(writeFile(first.filePath, registeredBytes));
    int cancellationChecks = 0;
    const AnimeJanaiPlan cancelledPlan = prepareAnimeJanaiPlan(
        presetId, Backend::DirectML, [&cancellationChecks] { return ++cancellationChecks >= 2; });
    QVERIFY(!cancelledPlan.available);
    QVERIFY(cancelledPlan.filterSpec.isEmpty());
    QVERIFY(priorConfig.open(QIODevice::ReadOnly));
    QCOMPARE(priorConfig.readAll(), priorConfigBytes);
    priorConfig.close();

    AnimeJanaiConfig config;
    AnimeJanaiSlot slot;
    slot.number = 1;
    slot.profileName = QStringLiteral("Three custom models");
    AnimeJanaiChain firstChain;
    firstChain.number = 1;
    firstChain.steps.append({1, AnimeJanaiStepKind::OnnxModel, first.fileName, 0, 100, 2});
    firstChain.steps.append({2, AnimeJanaiStepKind::OnnxModel, second.fileName, 0, 100, 2});
    AnimeJanaiChain secondChain;
    secondChain.number = 2;
    secondChain.maxFps = 24;
    secondChain.steps.append({1, AnimeJanaiStepKind::OnnxModel, third.fileName, 0, 100, 2});
    slot.chains = {firstChain, secondChain};
    config.customSlots.append(slot);
    QString saveError;
    QVERIFY2(AnimeJanaiConfigStore::save(config, &saveError), qPrintable(saveError));

    const AnimeJanaiPlan slotPlan =
        prepareAnimeJanaiPlan(QStringLiteral("builtin_slot:1"), Backend::DirectML);
    QVERIFY2(slotPlan.available, qPrintable(slotPlan.reason));
    for (const CustomAnimeJanaiModel* model : {&first, &second, &third}) {
        QVERIFY(QFileInfo::exists(QDir(slotPlan.modelDir)
                                      .filePath(model->sha256 + QStringLiteral(".onnx"))));
    }
    QFile slotConfig(slotPlan.configPath);
    QVERIFY(slotConfig.open(QIODevice::ReadOnly));
    const QByteArray slotBytes = slotConfig.readAll();
    QVERIFY(slotBytes.contains(QByteArrayLiteral("config_version=3")));
    QVERIFY(slotBytes.contains(QByteArrayLiteral("backend=DirectML")));
    for (const CustomAnimeJanaiModel* model : {&first, &second}) {
        QVERIFY(slotBytes.contains(QByteArrayLiteral("chain_1_model_") +
                                   (model == &first ? QByteArrayLiteral("1") : QByteArrayLiteral("2")) +
                                   QByteArrayLiteral("_name=") + model->sha256.toUtf8()));
    }
    QVERIFY(slotBytes.contains(QByteArrayLiteral("chain_2_model_1_name=") +
                               third.sha256.toUtf8()));

    QVERIFY(slotPlan.cacheLease != nullptr);
    slotPlan.cacheLease->release();
    slot.chains[1].rifeEnabled = true;
    slot.chains[1].rifeModel = QStringLiteral("RIFE 4.26 Heavy");
    slot.chains[1].rifeEnsemble = true;
    config.customSlots[0] = slot;
    QVERIFY2(AnimeJanaiConfigStore::save(config, &saveError), qPrintable(saveError));
    const AnimeJanaiPlan missingEnsemble =
        prepareAnimeJanaiPlan(QStringLiteral("builtin_slot:1"), Backend::DirectML);
    QVERIFY(!missingEnsemble.available);
    QCOMPARE(missingEnsemble.reasonKey, QStringLiteral("animejanai.model.pinMissing"));

    QVERIFY(removeCustomAnimeJanaiModel(second.id));
    QVERIFY(!findCustomAnimeJanaiModel(second.id).has_value());
#else
    QSKIP("The AnimeJaNai runtime is Windows-only.");
#endif
}

void TestAnimeJanaiRuntime::externalModelsRequireExplicitPinAndRepin() {
    QTemporaryDir externalDir;
    QVERIFY(externalDir.isValid());
    const QString externalPath =
        QDir(externalDir.path()).filePath(QStringLiteral("External_2x.onnx"));
    const QByteArray graph = mikai::tests::fixtures::makeTestIdentityOnnxGraph();
    QVERIFY(writeFile(externalPath, graph));

    AnimeJanaiConfig config;
    config.externalModelsDir = externalDir.path();
    QString saveError;
    QTemporaryDir outsideDir;
    QVERIFY(outsideDir.isValid());
    const QString outsidePath =
        QDir(outsideDir.path()).filePath(QStringLiteral("Outside_2x.onnx"));
    QVERIFY(writeFile(outsidePath, graph));
    QString outsideError;
    QVERIFY(!registerExternalCustomAnimeJanaiModel(
        outsidePath, nullptr, &outsideError, {}, externalDir.path()));
    QVERIFY(outsideError.contains(QStringLiteral("inside")));
    QVERIFY(QFileInfo::exists(outsidePath));

    QString scanError;
    const QVector<CustomAnimeJanaiModel> candidates =
        listExternalCustomAnimeJanaiModelCandidates(externalDir.path(), &scanError);
    QVERIFY(scanError.isEmpty());
    QCOMPARE(candidates.size(), 1);
    QVERIFY(!candidates.first().valid);
    QVERIFY(candidates.first().sha256.isEmpty());
    QVERIFY(!candidates.first().reason.isEmpty());

    CustomAnimeJanaiModel pinned;
    QString pinError;
    QVERIFY2(registerExternalCustomAnimeJanaiModel(
                 externalPath, &pinned, &pinError, {}, externalDir.path()),
             qPrintable(pinError));
    QVERIFY(pinned.valid);
    const auto unsavedDirectoryPin = findCustomAnimeJanaiModel(pinned.id);
    QVERIFY(unsavedDirectoryPin.has_value());
    QCOMPARE(unsavedDirectoryPin->sha256, pinned.sha256);
    QVERIFY2(AnimeJanaiConfigStore::save(config, &saveError), qPrintable(saveError));
    QVERIFY(QFileInfo::exists(QDir(nativeRoot()).filePath(
        QStringLiteral("animejanai/models/external-models.json"))));
    QVERIFY(!QFileInfo::exists(externalPath + QStringLiteral(".source.json")));
    const auto listed = findCustomAnimeJanaiModel(pinned.id);
    QVERIFY(listed.has_value());
    QCOMPARE(listed->sha256, pinned.sha256);

    QByteArray changed = graph;
    changed[changed.size() - 1] ^= 1;
    QVERIFY(writeFile(externalPath, changed));
    const QString changedDigest = sha(changed);
    QVERIFY(changedDigest != pinned.sha256);
    const auto unchangedPin = findCustomAnimeJanaiModel(pinned.id);
    QVERIFY(unchangedPin.has_value());
    QCOMPARE(unchangedPin->sha256, pinned.sha256);

    const QString previousDigest = listed->sha256;
    const QString cancelledExternal =
        QDir(externalDir.path()).filePath(QStringLiteral("Cancelled_2x.onnx"));
    QVERIFY(writeFile(cancelledExternal, graph));
    int registrationChecks = 0;
    QString registrationError;
    QVERIFY(!registerExternalCustomAnimeJanaiModel(
        cancelledExternal, nullptr, &registrationError,
        [&registrationChecks] { return ++registrationChecks >= 2; }));
    QVERIFY(!registrationError.isEmpty());
    QVERIFY(QFileInfo::exists(cancelledExternal));
    QVERIFY(!findCustomAnimeJanaiModel(QStringLiteral("Cancelled_2x")).has_value());

    QVERIFY2(registerExternalCustomAnimeJanaiModel(externalPath, &pinned, &pinError),
             qPrintable(pinError));
    QVERIFY(pinned.sha256 != previousDigest);
    QVERIFY(removeCustomAnimeJanaiModel(pinned.id, &pinError));
    QVERIFY(QFileInfo::exists(externalPath));
    QVERIFY(!findCustomAnimeJanaiModel(pinned.id).has_value());
}

void TestAnimeJanaiRuntime::malformedOnnxModelIsRejected() {
    QTemporaryDir sourceDir;
    QVERIFY(sourceDir.isValid());
    const QString malformedPath =
        QDir(sourceDir.path()).filePath(QStringLiteral("not-a-graph.onnx"));
    QVERIFY(writeFile(malformedPath, QByteArray(128, 'x')));
    qint64 bytes = 0;
    QString reason;
    QVERIFY(!validateCustomOnnxFile(malformedPath, &bytes, &reason));
    QVERIFY(!reason.isEmpty());
    QCOMPARE(bytes, qint64(0));
    QString importError;
    QVERIFY(!importCustomAnimeJanaiModel(malformedPath, nullptr, &importError));
    QVERIFY(!importError.isEmpty());

    const QString externalModel =
        QDir(sourceDir.path()).filePath(QStringLiteral("external.onnx"));
    QVERIFY(writeFile(externalModel, mikai::tests::fixtures::makeTestIdentityOnnxGraph()));
    QString removalError;
    QVERIFY(!removeCustomAnimeJanaiModel(externalModel, &removalError));
    QVERIFY(!removalError.isEmpty());
    QVERIFY(QFileInfo::exists(externalModel));

    AnimeJanaiSlot unsafeSlot;
    unsafeSlot.profileName = QStringLiteral("Profile\n[slot_2]");
    AnimeJanaiChain unsafeChain;
    unsafeChain.steps.append({1, AnimeJanaiStepKind::OnnxModel,
                              QStringLiteral("../../outside\n[slot_2]"), 0, 100, 2});
    unsafeSlot.chains.append(unsafeChain);
    const auto roundTrip = AnimeJanaiConfigStore::parse(
        AnimeJanaiConfigStore::serializeSlotSection(unsafeSlot));
    QCOMPARE(roundTrip.customSlots.size(), 1);
    QCOMPARE(roundTrip.customSlots.first().number, 1);
}

void TestAnimeJanaiRuntime::invalidConfigFilesReportErrorsWithoutReplacingStoredSettings() {
    AnimeJanaiConfig config;
    config.qualitySharp = true;
    QString error;
    QVERIFY2(AnimeJanaiConfigStore::save(config, &error), qPrintable(error));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString missing = QDir(directory.path()).filePath(QStringLiteral("missing.conf"));
    AnimeJanaiConfigStore::loadFromFile(missing, &error);
    QVERIFY(!error.isEmpty());
    QVERIFY(AnimeJanaiConfigStore::load().qualitySharp);

    const QString malformed = QDir(directory.path()).filePath(QStringLiteral("malformed.conf"));
    QVERIFY(writeFile(malformed, QByteArrayLiteral("<html>Not a configuration</html>")));
    AnimeJanaiConfigStore::loadFromFile(malformed, &error);
    QVERIFY(!error.isEmpty());
    QVERIFY(AnimeJanaiConfigStore::load().qualitySharp);

    const QString defaults = QDir(directory.path()).filePath(QStringLiteral("defaults.conf"));
    QVERIFY(writeFile(defaults, QByteArrayLiteral("[global]\nconfig_version=3\n")));
    const auto loaded = AnimeJanaiConfigStore::loadFromFile(defaults, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(!loaded.qualitySharp);
    QVERIFY(AnimeJanaiConfigStore::load().qualitySharp);
}

void TestAnimeJanaiRuntime::managerConfigParsesAndSerializesMultiChainSlotsAndTrtSettings() {
    AnimeJanaiConfig config;
    config.enableLogging = false;
    config.qualitySharp = true;
    config.balancedSharp = true;
    config.performanceSharp = true;
    config.trtEngineSettings = AnimeJanaiConfigStore::setTrtDynamic(config.trtEngineSettings);
    config.trtEngineSettings =
        AnimeJanaiConfigStore::setTrtDynamicMinResolution(config.trtEngineSettings, QStringLiteral("480x480"));
    config.trtEngineSettings =
        AnimeJanaiConfigStore::setTrtBuilderOptimizationLevel(config.trtEngineSettings, QStringLiteral("4"));

    QVERIFY(AnimeJanaiConfigStore::trtDynamicSelected(config.trtEngineSettings));
    QCOMPARE(AnimeJanaiConfigStore::trtDynamicMinResolution(config.trtEngineSettings),
             QStringLiteral("480x480"));
    QCOMPARE(AnimeJanaiConfigStore::trtBuilderOptimizationLevel(config.trtEngineSettings),
             QStringLiteral("4"));

    AnimeJanaiSlot slot;
    slot.number = 1;
    slot.profileName = QStringLiteral("Custom 2-Step RealESRGAN");
    slot.descriptionText = QStringLiteral("Two model steps plus frame interpolation");
    AnimeJanaiChain chain;
    chain.number = 1;
    chain.minResolution = QStringLiteral("1280x720");
    chain.maxResolution = QStringLiteral("1920x1080");
    chain.minFps = 0;
    chain.maxFps = 31;
    chain.steps.append({1, AnimeJanaiStepKind::OnnxModel,
                        QStringLiteral("2x_Custom_Compact"), 720, 75, 2});
    chain.steps.append({2, AnimeJanaiStepKind::OnnxModel,
                        QStringLiteral("4x_Custom_Sharp"), 1080, 80, 2});
    chain.rifeEnabled = true;
    chain.rifeModel = QStringLiteral("RIFE 4.26");
    chain.rifeFactorNumerator = 2;
    chain.rifeFactorDenominator = 1;
    chain.rifeSceneDetectThreshold = 0.150;
    chain.rifeEnsemble = true;
    chain.rifeBeforeUpscale = false;
    slot.chains.append(chain);
    AnimeJanaiChain secondChain;
    secondChain.number = 2;
    secondChain.minResolution = QStringLiteral("0x0");
    secondChain.maxResolution = QStringLiteral("1280x720");
    secondChain.maxFps = 24;
    secondChain.steps.append({1, AnimeJanaiStepKind::OnnxModel,
                              QStringLiteral("2x_Other_Model"), 0, 100, 2});
    slot.chains.append(secondChain);
    config.customSlots.append(slot);

    const QString serialized = AnimeJanaiConfigStore::serialize(config);
    QVERIFY(serialized.contains(QStringLiteral("config_version=3")));
    const AnimeJanaiConfig roundTrip = AnimeJanaiConfigStore::parse(serialized);
    QCOMPARE(roundTrip.enableLogging, config.enableLogging);
    QCOMPARE(roundTrip.qualitySharp, config.qualitySharp);
    QCOMPARE(roundTrip.balancedSharp, config.balancedSharp);
    QCOMPARE(roundTrip.performanceSharp, config.performanceSharp);
    QCOMPARE(roundTrip.trtEngineSettings, config.trtEngineSettings);
    QCOMPARE(roundTrip.customSlots.size(), 1);
    QCOMPARE(roundTrip.customSlots.first().profileName, slot.profileName);
    QCOMPARE(roundTrip.customSlots.first().descriptionText, slot.descriptionText);
    QCOMPARE(roundTrip.customSlots.first().chains.size(), 2);
    QCOMPARE(roundTrip.customSlots.first().chains.first().steps.size(), 2);
    QCOMPARE(roundTrip.customSlots.first().chains.first().steps.first().resizeHeightBeforeUpscale, 720);
    QCOMPARE(roundTrip.customSlots.first().chains.first().steps.first().resizeFactorBeforeUpscale, 75);
    QCOMPARE(roundTrip.customSlots.first().chains.first().minResolution, QStringLiteral("1280x720"));
    QCOMPARE(roundTrip.customSlots.first().chains.first().maxResolution, QStringLiteral("1920x1080"));
    QCOMPARE(roundTrip.customSlots.first().chains.first().maxFps, 31);
    QCOMPARE(roundTrip.customSlots.first().chains.first().rifeModel, QStringLiteral("RIFE 4.26"));
    QCOMPARE(roundTrip.customSlots.first().chains.first().steps.at(1).id,
             QStringLiteral("4x_Custom_Sharp"));
    QVERIFY(roundTrip.customSlots.first().chains.first().rifeEnabled);
    QCOMPARE(roundTrip.customSlots.first().chains.first().rifeFactorNumerator, 2);
    QCOMPARE(roundTrip.customSlots.first().chains.first().rifeFactorDenominator, 1);
    QCOMPARE(roundTrip.customSlots.first().chains.first().rifeSceneDetectThreshold, 0.150);
    QVERIFY(roundTrip.customSlots.first().chains.first().rifeEnsemble);
    QVERIFY(!roundTrip.customSlots.first().chains.first().rifeBeforeUpscale);
    QCOMPARE(roundTrip.customSlots.first().chains.at(1).steps.first().id,
             QStringLiteral("2x_Other_Model"));
    const AnimeJanaiConfig unsafeSlots =
        AnimeJanaiConfigStore::parse(QStringLiteral("[slot_11]\nprofile_name=Unsafe\nchain_1_model_1_name=../../outside\n"));
    QVERIFY(unsafeSlots.customSlots.isEmpty());

    QTemporaryDir profileDirectory;
    QVERIFY(profileDirectory.isValid());
    const QString profilePath =
        QDir(profileDirectory.path()).filePath(QStringLiteral("custom.pconf"));
    QString profileError;
    QVERIFY2(AnimeJanaiConfigStore::saveProfile(profilePath, slot, &profileError),
             qPrintable(profileError));
    bool profileOk = false;
    const AnimeJanaiSlot profile =
        AnimeJanaiConfigStore::loadProfile(profilePath, &profileOk, &profileError);
    QVERIFY2(profileOk, qPrintable(profileError));
    QCOMPARE(profile, slot);
}

void TestAnimeJanaiRuntime::mixedAnime4KPlanUsesSourceSelectionWithoutAji() {
#ifdef Q_OS_WIN
    AnimeJanaiConfig config;
    AnimeJanaiSlot slot;
    slot.number = 1;
    AnimeJanaiChain first;
    first.number = 1;
    first.minResolution = QStringLiteral("1920x1080");
    first.maxResolution = QStringLiteral("1920x1080");
    first.minFps = 23;
    first.maxFps = 24;
    first.steps.append({1, AnimeJanaiStepKind::Anime4K, QStringLiteral("fast_a"), 720, 75, 1});
    AnimeJanaiChain second;
    second.number = 2;
    second.minResolution = QStringLiteral("3000x2000");
    second.maxResolution = QStringLiteral("4000x3000");
    second.steps.append({1, AnimeJanaiStepKind::Anime4K, QStringLiteral("hq_b"), 0, 100, 4});
    slot.chains = {first, second};
    config.customSlots.append(slot);
    QString error;
    QVERIFY2(AnimeJanaiConfigStore::save(config, &error), qPrintable(error));

    auto resolver = [](const QString& id, QString* reason) {
        if (reason) reason->clear();
        return QByteArrayLiteral("shader:") + id.toUtf8();
    };
    AnimeJanaiPlan deferred = prepareAnimeJanaiPlan(
        QStringLiteral("builtin_slot:1"), Backend::DirectML, {}, {}, resolver);
    QVERIFY(deferred.enabled);
    QVERIFY(!deferred.available);
    QVERIFY(deferred.needsSourceContext);
    QVERIFY(!deferred.requiresAji);

    const AnimeJanaiSourceContext source{1920, 1080, 24.0};
    const AnimeJanaiPlan plan = prepareAnimeJanaiPlan(
        QStringLiteral("builtin_slot:1"), Backend::DirectML, {}, source, resolver);
    QVERIFY2(plan.available, qPrintable(plan.reason));
    QVERIFY(!plan.requiresAji);
    QCOMPARE(plan.selectedChainNumber, 1);
    QCOMPARE(plan.stages.size(), 1);
    QCOMPARE(plan.stages.first().kind, AnimeJanaiPlanStage::Kind::Anime4K);
    QCOMPARE(plan.stages.first().scaleFactor, 1);
    QVERIFY(plan.filterSpec.contains(QByteArrayLiteral("w=iw*1")));
    QVERIFY(plan.filterSpec.contains(QByteArrayLiteral("h=ih*1")));
    QVERIFY(plan.filterSpec.contains(QByteArrayLiteral("mikai_a4k_resize_1")));
    for (const int scale : {2, 4}) {
        config.customSlots[0].chains[0].steps[0].scaleFactor = scale;
        QVERIFY2(AnimeJanaiConfigStore::save(config, &error), qPrintable(error));
        const AnimeJanaiPlan scaled = prepareAnimeJanaiPlan(
            QStringLiteral("builtin_slot:1"), Backend::DirectML, {}, source, resolver);
        QVERIFY2(scaled.available, qPrintable(scaled.reason));
        QCOMPARE(scaled.stages.first().scaleFactor, scale);
        QVERIFY(scaled.filterSpec.contains(QByteArrayLiteral("w=iw*") + QByteArray::number(scale)));
        QVERIFY(scaled.filterSpec.contains(QByteArrayLiteral("h=ih*") + QByteArray::number(scale)));
    }
    const AnimeJanaiPlan noMatch = prepareAnimeJanaiPlan(
        QStringLiteral("builtin_slot:1"), Backend::DirectML, {}, {1280, 720, 24.0}, resolver);
    QVERIFY(!noMatch.available);
    QCOMPARE(noMatch.reasonKey, QStringLiteral("animejanai.profile.noChain"));
#else
    QSKIP("The AnimeJaNai runtime is Windows-only.");
#endif
}

void TestAnimeJanaiRuntime::stageCacheLeasesRefuseOverlapAndReleaseAtTerminal() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString firstKey = QDir(directory.path()).filePath(QStringLiteral("first"));
    const QString secondKey = QDir(directory.path()).filePath(QStringLiteral("second"));
    const QString thirdKey = QDir(directory.path()).filePath(QStringLiteral("third"));
    auto first = AnimeJanaiStageBuildLease::tryAcquire(
        {firstKey, secondKey}, QStringLiteral("Player"));
    QVERIFY(first != nullptr);
    const auto retainedPlanCopy = first;
    QString busyKey;
    QString busyOwner;
    QVERIFY(!AnimeJanaiStageBuildLease::tryAcquire(
        {secondKey, thirdKey}, QStringLiteral("Compare"), &busyKey, &busyOwner));
    QCOMPARE(busyKey, secondKey);
    QCOMPARE(busyOwner, QStringLiteral("Player"));
    auto unrelated = AnimeJanaiStageBuildLease::tryAcquire(
        {thirdKey}, QStringLiteral("Benchmark"));
    QVERIFY(unrelated != nullptr);
    first->release();
    QVERIFY(!retainedPlanCopy->held());
    auto replacement = AnimeJanaiStageBuildLease::tryAcquire(
        {firstKey, secondKey}, QStringLiteral("Player"));
    QVERIFY(replacement != nullptr);
    retainedPlanCopy->release();
    QVERIFY(!AnimeJanaiStageBuildLease::tryAcquire(
        {secondKey}, QStringLiteral("Compare")));
    replacement.reset();
    QVERIFY(AnimeJanaiStageBuildLease::tryAcquire(
        {firstKey, secondKey}, QStringLiteral("Compare")) != nullptr);
}

void TestAnimeJanaiRuntime::mixedConfigStepsRoundTripAndRejectInvalidData() {
    QString legacyError;
    const AnimeJanaiConfig legacy = AnimeJanaiConfigStore::parse(
        QStringLiteral("[slot_1]\nprofile_name=Legacy\n"
                       "chain_1_model_2_name=second\n"
                       "chain_1_model_2_resize_height_before_upscale=540\n"
                       "chain_1_model_2_resize_factor_before_upscale=75\n"
                       "chain_1_model_1_name=first\n"), &legacyError);
    QVERIFY2(legacyError.isEmpty(), qPrintable(legacyError));
    QCOMPARE(legacy.customSlots.size(), 1);
    QCOMPARE(legacy.customSlots.first().chains.size(), 1);
    QCOMPARE(legacy.customSlots.first().chains.first().steps.size(), 2);
    QCOMPARE(legacy.customSlots.first().chains.first().steps.at(0).id, QStringLiteral("first"));
    QCOMPARE(legacy.customSlots.first().chains.first().steps.at(1).id, QStringLiteral("second"));
    QCOMPARE(legacy.customSlots.first().chains.first().steps.at(1).resizeHeightBeforeUpscale, 540);
    QCOMPARE(legacy.customSlots.first().chains.first().steps.at(1).resizeFactorBeforeUpscale, 75);

    AnimeJanaiSlot slot;
    slot.profileName = QStringLiteral("Mixed");
    AnimeJanaiChain chain;
    chain.steps = {
        {1, AnimeJanaiStepKind::OnnxModel, QStringLiteral("2x_Custom"), 720, 75, 2},
        {2, AnimeJanaiStepKind::Anime4K, QStringLiteral("fast_b"), 480, 80, 4},
        {3, AnimeJanaiStepKind::OnnxModel, QStringLiteral("custom_sha"), 0, 100, 2},
    };
    slot.chains.append(chain);
    AnimeJanaiChain secondChain;
    secondChain.number = 2;
    secondChain.steps = {
        {1, AnimeJanaiStepKind::Anime4K, QStringLiteral("hq_ca"), 0, 100, 1}};
    slot.chains.append(secondChain);
    const QString exported = AnimeJanaiConfigStore::serializeProfile(slot);
    QVERIFY(exported.contains(QStringLiteral("mikai_chain_steps_version=1")));
    QVERIFY(exported.contains(QStringLiteral("chain_1_step_2_kind=anime4k")));
    bool ok = false;
    QString error;
    const AnimeJanaiSlot imported = AnimeJanaiConfigStore::parseProfile(exported, &ok, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(imported, slot);

    (void)AnimeJanaiConfigStore::parse(
        QStringLiteral("[global]\nconfig_version=3\nmikai_chain_steps_version=7\n"), &error);
    QVERIFY(!error.isEmpty());

    for (const QString& invalidStep : {
             QStringLiteral("chain_1_step_1_kind=unknown\nchain_1_step_1_id=x\nchain_1_step_1_scale_factor=2\n"),
             QStringLiteral("chain_1_step_1_kind=anime4k\nchain_1_step_1_id=fast_a\nchain_1_step_1_scale_factor=3\n")}) {
        const QString input = QStringLiteral(
            "[slot]\nmikai_chain_steps_version=1\nchain_1_min_resolution=0x0\n"
            "chain_1_step_1_resize_height_before_upscale=0\n"
            "chain_1_step_1_resize_factor_before_upscale=100\n") +
            invalidStep;
        (void)AnimeJanaiConfigStore::parseProfile(input, &ok, &error);
        QVERIFY(!ok);
        QVERIFY(!error.isEmpty());
    }
    AnimeJanaiConfig invalidConfig;
    slot.chains[0].steps[1].scaleFactor = 3;
    invalidConfig.customSlots.append(slot);
    QVERIFY(!AnimeJanaiConfigStore::validateAnimeJanaiConfig(invalidConfig, &error));
    QVERIFY(!error.isEmpty());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString protectedPath = QDir(directory.path()).filePath(QStringLiteral("saved.conf"));
    const QByteArray original = QByteArrayLiteral("preserve existing data");
    QVERIFY(writeFile(protectedPath, original));
    QVERIFY(!AnimeJanaiConfigStore::saveToFile(protectedPath, invalidConfig, &error));
    QFile protectedFile(protectedPath);
    QVERIFY(protectedFile.open(QIODevice::ReadOnly));
    QCOMPARE(protectedFile.readAll(), original);
}

void TestAnimeJanaiRuntime::catalogWatcherReportsPastedRenamedAndRemovedModels() {
    QTemporaryDir externalDir;
    QVERIFY(externalDir.isValid());
    AnimeJanaiCatalogWatcher watcher;
    QSignalSpy changed(&watcher, &AnimeJanaiCatalogWatcher::catalogChanged);
    watcher.setExternalModelsDirectory(externalDir.path());
    watcher.refresh();

    const QString pastedName = QStringLiteral("External_Paste_2x.onnx");
    const QString pastedPath = QDir(externalDir.path()).filePath(pastedName);
    QVERIFY(writeFile(pastedPath, taggedIdentityGraph(QByteArrayLiteral("pasted"))));
    QVERIFY(changed.wait(5000));
    const QVector<CustomAnimeJanaiModel> pastedCandidates =
        listExternalCustomAnimeJanaiModelCandidates(externalDir.path());
    const CustomAnimeJanaiModel* pasted = modelNamed(pastedCandidates, pastedName);
    QVERIFY(pasted != nullptr);
    QVERIFY(!pasted->valid);
    QVERIFY(!pasted->reason.isEmpty());

    const QString renamedName = QStringLiteral("External_Renamed_2x.onnx");
    const QString renamedPath = QDir(externalDir.path()).filePath(renamedName);
    changed.clear();
    QVERIFY(QFile::rename(pastedPath, renamedPath));
    QVERIFY(changed.wait(5000));
    QTRY_VERIFY_WITH_TIMEOUT(
        listsFileNamed(listExternalCustomAnimeJanaiModelCandidates(externalDir.path()), renamedName),
        5000);
    QVERIFY(!listsFileNamed(listExternalCustomAnimeJanaiModelCandidates(externalDir.path()),
                            pastedName));

    changed.clear();
    QVERIFY(QFile::remove(renamedPath));
    QVERIFY(changed.wait(5000));
    QTRY_VERIFY_WITH_TIMEOUT(
        !listsFileNamed(listExternalCustomAnimeJanaiModelCandidates(externalDir.path()), renamedName),
        5000);
}

void TestAnimeJanaiRuntime::catalogWatcherRearmsAfterAtomicReplacementAndInPlaceEdit() {
    QTemporaryDir externalDir;
    QVERIFY(externalDir.isValid());
    AnimeJanaiCatalogWatcher watcher;
    QSignalSpy changed(&watcher, &AnimeJanaiCatalogWatcher::catalogChanged);
    watcher.setExternalModelsDirectory(externalDir.path());
    watcher.refresh();

    const QString name = QStringLiteral("External_Replace_2x.onnx");
    const QString path = QDir(externalDir.path()).filePath(name);
    QVERIFY(writeFile(path, taggedIdentityGraph(QByteArrayLiteral("first"))));
    QVERIFY(changed.wait(5000));
    const qint64 initialBytes = QFileInfo(path).size();
    const QVector<CustomAnimeJanaiModel> initialCandidates =
        listExternalCustomAnimeJanaiModelCandidates(externalDir.path());
    const CustomAnimeJanaiModel* initialModel = modelNamed(initialCandidates, name);
    QVERIFY(initialModel != nullptr);
    QCOMPARE(initialModel->bytes, initialBytes);

    changed.clear();
    {
        QSaveFile replacement(path);
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        const QByteArray replacementBytes = taggedIdentityGraph(QByteArrayLiteral("atomic-replacement"));
        QVERIFY(replacement.write(replacementBytes) == replacementBytes.size());
        QVERIFY(replacement.commit());
    }
    QVERIFY(changed.wait(5000));
    const qint64 replacedBytes = QFileInfo(path).size();
    QVERIFY(replacedBytes != initialBytes);
    const QVector<CustomAnimeJanaiModel> replacedCandidates =
        listExternalCustomAnimeJanaiModelCandidates(externalDir.path());
    const CustomAnimeJanaiModel* replacedModel = modelNamed(replacedCandidates, name);
    QVERIFY(replacedModel != nullptr);
    QCOMPARE(replacedModel->bytes, replacedBytes);

    changed.clear();
    QVERIFY(rewriteLastByteSameSize(path));
    QVERIFY(changed.wait(5000));
    QCOMPARE(QFileInfo(path).size(), replacedBytes);
    const QVector<CustomAnimeJanaiModel> editedCandidates =
        listExternalCustomAnimeJanaiModelCandidates(externalDir.path());
    const CustomAnimeJanaiModel* editedModel = modelNamed(editedCandidates, name);
    QVERIFY(editedModel != nullptr);
    QCOMPARE(editedModel->bytes, replacedBytes);
}

void TestAnimeJanaiRuntime::catalogWatcherTracksMissingCreatedAndRecreatedDirectory() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString watched = QDir(root.path()).filePath(QStringLiteral("catalog"));
    QVERIFY(!QFileInfo::exists(watched));
    AnimeJanaiCatalogWatcher watcher;
    QSignalSpy changed(&watcher, &AnimeJanaiCatalogWatcher::catalogChanged);
    watcher.setExternalModelsDirectory(watched);
    watcher.refresh();
    QVERIFY(listExternalCustomAnimeJanaiModelCandidates(watched).isEmpty());

    QVERIFY(QDir().mkpath(watched));
    QVERIFY(writeFile(QDir(watched).filePath(QStringLiteral("Created_2x.onnx")),
                      taggedIdentityGraph(QByteArrayLiteral("created"))));
    QVERIFY(changed.wait(5000));
    QTRY_VERIFY_WITH_TIMEOUT(
        listsFileNamed(listExternalCustomAnimeJanaiModelCandidates(watched),
                       QStringLiteral("Created_2x.onnx")),
        5000);

    changed.clear();
    QVERIFY(QDir(watched).removeRecursively());
    QVERIFY(changed.wait(5000));
    QVERIFY(listExternalCustomAnimeJanaiModelCandidates(watched).isEmpty());

    changed.clear();
    QVERIFY(QDir().mkpath(watched));
    QVERIFY(writeFile(QDir(watched).filePath(QStringLiteral("Recreated_2x.onnx")),
                      taggedIdentityGraph(QByteArrayLiteral("recreated"))));
    QVERIFY(changed.wait(5000));
    QTRY_VERIFY_WITH_TIMEOUT(
        listsFileNamed(listExternalCustomAnimeJanaiModelCandidates(watched),
                       QStringLiteral("Recreated_2x.onnx")),
        5000);
}

void TestAnimeJanaiRuntime::catalogWatcherSwitchesOverrideWithoutWatchingOldDirectory() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString first = QDir(root.path()).filePath(QStringLiteral("first"));
    const QString second = QDir(root.path()).filePath(QStringLiteral("second"));
    QVERIFY(QDir().mkpath(first));
    QVERIFY(QDir().mkpath(second));
    AnimeJanaiCatalogWatcher watcher;
    QSignalSpy changed(&watcher, &AnimeJanaiCatalogWatcher::catalogChanged);
    watcher.setExternalModelsDirectory(first);
    watcher.refresh();
    QVERIFY(writeFile(QDir(first).filePath(QStringLiteral("First_2x.onnx")),
                      taggedIdentityGraph(QByteArrayLiteral("first"))));
    QVERIFY(changed.wait(5000));
    QCOMPARE(listExternalCustomAnimeJanaiModelCandidates(first).size(), 1);

    watcher.setExternalModelsDirectory(second);
    watcher.refresh();
    changed.clear();
    QVERIFY(writeFile(QDir(first).filePath(QStringLiteral("Late_2x.onnx")),
                      taggedIdentityGraph(QByteArrayLiteral("late"))));
    QVERIFY(!changed.wait(1500));
    QVERIFY(listsFileNamed(listExternalCustomAnimeJanaiModelCandidates(first),
                           QStringLiteral("Late_2x.onnx")));
    QVERIFY(!listsFileNamed(listExternalCustomAnimeJanaiModelCandidates(second),
                            QStringLiteral("Late_2x.onnx")));

    QVERIFY(writeFile(QDir(second).filePath(QStringLiteral("Second_2x.onnx")),
                      taggedIdentityGraph(QByteArrayLiteral("second"))));
    QVERIFY(changed.wait(5000));
    QVERIFY(listsFileNamed(listExternalCustomAnimeJanaiModelCandidates(second),
                           QStringLiteral("Second_2x.onnx")));
}

void TestAnimeJanaiRuntime::catalogWatcherKeepsExternalPinsExplicit() {
    QTemporaryDir externalDir;
    QVERIFY(externalDir.isValid());
    const QString name = QStringLiteral("External_Pin_2x.onnx");
    const QString path = QDir(externalDir.path()).filePath(name);
    AnimeJanaiCatalogWatcher watcher;
    QSignalSpy changed(&watcher, &AnimeJanaiCatalogWatcher::catalogChanged);
    watcher.setExternalModelsDirectory(externalDir.path());
    watcher.refresh();
    QVERIFY(writeFile(path, taggedIdentityGraph(QByteArrayLiteral("pin"))));
    QVERIFY(changed.wait(5000));

    const QVector<CustomAnimeJanaiModel> candidates =
        listExternalCustomAnimeJanaiModelCandidates(externalDir.path());
    const CustomAnimeJanaiModel* candidate = modelNamed(candidates, name);
    QVERIFY(candidate != nullptr);
    QVERIFY(!candidate->valid);
    QVERIFY(listCustomAnimeJanaiModels().isEmpty());

    changed.clear();
    CustomAnimeJanaiModel pinned;
    QString error;
    QVERIFY2(registerExternalCustomAnimeJanaiModel(path, &pinned, &error, {}, externalDir.path()),
             qPrintable(error));
    QVERIFY(changed.wait(5000));
    const auto listed = findCustomAnimeJanaiModel(pinned.id);
    QVERIFY(listed.has_value());
    QCOMPARE(listed->sha256, pinned.sha256);
    QVERIFY(!listsFileNamed(listExternalCustomAnimeJanaiModelCandidates(externalDir.path()), name));

    changed.clear();
    QVERIFY(rewriteLastByteSameSize(path));
    QVERIFY(changed.wait(5000));
    const auto retained = findCustomAnimeJanaiModel(pinned.id);
    QVERIFY(retained.has_value());
    QCOMPARE(retained->sha256, pinned.sha256);

    QVERIFY2(removeCustomAnimeJanaiModel(pinned.id, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(!findCustomAnimeJanaiModel(pinned.id).has_value());
    QVERIFY(listsFileNamed(listExternalCustomAnimeJanaiModelCandidates(externalDir.path()), name));
}

void TestAnimeJanaiRuntime::catalogWatcherObservesCompleteCatalogueBeyondLegacyLimits() {
    QTemporaryDir externalDir;
    QVERIFY(externalDir.isValid());
    const QDir directory(externalDir.path());

    // The legacy observation signature adopted at most the first 512 matching
    // metadata rows after examining at most 4096 directory entries. With noise
    // entries ahead of the catalogue, every model fell outside that window, so
    // the watcher could not report changes the public inventory already lists.
    const QByteArray modelBytes = taggedIdentityGraph(QByteArrayLiteral("overflow"));
    for (int index = 0; index < 4100; ++index) {
        const QString name =
            QStringLiteral("000_noise_%1.tmp").arg(index, 4, 10, QLatin1Char('0'));
        QVERIFY(writeFile(directory.filePath(name), QByteArrayLiteral("n")));
    }
    QStringList modelNames;
    modelNames.reserve(530);
    for (int index = 0; index < 530; ++index) {
        const QString name =
            QStringLiteral("Anime_2x_%1.onnx").arg(index, 4, 10, QLatin1Char('0'));
        QVERIFY(writeFile(directory.filePath(name), modelBytes));
        modelNames.push_back(name);
    }

    AnimeJanaiCatalogWatcher watcher;
    QSignalSpy changed(&watcher, &AnimeJanaiCatalogWatcher::catalogChanged);
    watcher.setExternalModelsDirectory(externalDir.path());
    watcher.refresh();

    // Inventory agreement: the first and last legacy scan-order names must
    // already be consumer-visible before the watcher is asked to follow them.
    const QVector<CustomAnimeJanaiModel> baseline =
        listExternalCustomAnimeJanaiModelCandidates(externalDir.path());
    QVERIFY(listsFileNamed(baseline, modelNames.constFirst()));
    QVERIFY(listsFileNamed(baseline, modelNames.constLast()));

    // A lexicographically late paste must notify even though the legacy bounded
    // signature never contained it.
    const QString lateName = QStringLiteral("Zzz_Late_Paste_2x.onnx");
    const QString latePath = directory.filePath(lateName);
    QVERIFY(writeFile(latePath, taggedIdentityGraph(QByteArrayLiteral("late"))));
    QVERIFY(changed.wait(5000));
    QTRY_VERIFY_WITH_TIMEOUT(
        listsFileNamed(listExternalCustomAnimeJanaiModelCandidates(externalDir.path()), lateName),
        5000);
    const qint64 pastedBytes = QFileInfo(latePath).size();

    // In-place replacement keeping byte count and modification time must still
    // notify: the observed file itself is the evidence, not the metadata diff.
    QTest::qWait(400);
    changed.clear();
    const QDateTime pastedModified = QFileInfo(latePath).lastModified();
    QVERIFY(rewriteLastByteSameSize(latePath));
    {
        QFile restored(latePath);
        QVERIFY(restored.open(QIODevice::ReadWrite));
        // The premise is that the write really is hidden from metadata: fail
        // here rather than silently degrading into an ordinary mtime change.
        QVERIFY(restored.setFileTime(pastedModified, QFileDevice::FileModificationTime));
    }
    QVERIFY(changed.wait(5000));
    const QVector<CustomAnimeJanaiModel> edited =
        listExternalCustomAnimeJanaiModelCandidates(externalDir.path());
    const CustomAnimeJanaiModel* editedModel = modelNamed(edited, lateName);
    QVERIFY(editedModel != nullptr);
    QCOMPARE(editedModel->bytes, pastedBytes);

    QTest::qWait(400);
    changed.clear();
    QVERIFY(QFile::remove(latePath));
    QVERIFY(changed.wait(5000));
    QTRY_VERIFY_WITH_TIMEOUT(
        !listsFileNamed(listExternalCustomAnimeJanaiModelCandidates(externalDir.path()), lateName),
        5000);

    // Non-model churn shares the watched directory but is not catalogue state.
    QTest::qWait(400);
    changed.clear();
    QVERIFY(writeFile(directory.filePath(QStringLiteral("000_noise_0000.tmp")),
                      QByteArrayLiteral("m")));
    QVERIFY(writeFile(directory.filePath(QStringLiteral("000_noise_added.tmp")),
                      QByteArrayLiteral("n")));
    QVERIFY(!changed.wait(1500));
    const QVector<CustomAnimeJanaiModel> afterNoise =
        listExternalCustomAnimeJanaiModelCandidates(externalDir.path());
    QVERIFY(listsFileNamed(afterNoise, modelNames.constLast()));
    QVERIFY(!listsFileNamed(afterNoise, QStringLiteral("000_noise_added.tmp")));
}

void TestAnimeJanaiRuntime::pastedOwnedModelRequiresExplicitRegistration() {
    AnimeJanaiCatalogWatcher watcher;
    QSignalSpy changed(&watcher, &AnimeJanaiCatalogWatcher::catalogChanged);
    watcher.refresh();

    const QString fileName = QStringLiteral("4x Pasted Model (v2).onnx");
    const QString pastedPath = QDir(customAnimeJanaiModelsDir()).filePath(fileName);
    const QByteArray graph = taggedIdentityGraph(QByteArrayLiteral("owned-paste"));
    QVERIFY(writeFile(pastedPath, graph));
    QVERIFY(changed.wait(5000));

    const QVector<CustomAnimeJanaiModel> candidates = listUnregisteredOwnedAnimeJanaiModels();
    QCOMPARE(candidates.size(), 1);
    const CustomAnimeJanaiModel candidate = candidates.first();
    QCOMPARE(candidate.filePath, pastedPath);
    QCOMPARE(candidate.fileName, fileName);
    QVERIFY(!candidate.valid);
    QVERIFY(!candidate.reason.isEmpty());
    QVERIFY(candidate.sha256.isEmpty());
    QCOMPARE(candidate.bytes, qint64(graph.size()));
    QVERIFY(listCustomAnimeJanaiModels().isEmpty());
    QVERIFY(!findCustomAnimeJanaiModel(candidate.id).has_value());

    {
        std::atomic_bool runnerCancelled{true};
        const mikai::backend::WorkCancellationScope cancellationScope(&runnerCancelled);
        QString cancellationError;
        QVERIFY(!registerOwnedCustomAnimeJanaiModel(pastedPath, nullptr, &cancellationError));
        QVERIFY(!cancellationError.isEmpty());
    }
    QVERIFY(!QFileInfo::exists(pastedPath + QStringLiteral(".source.json")));
    QVERIFY(QFileInfo::exists(pastedPath));
    QVERIFY(listCustomAnimeJanaiModels().isEmpty());
    QCOMPARE(listUnregisteredOwnedAnimeJanaiModels().size(), 1);

    changed.clear();
    CustomAnimeJanaiModel registered;
    QString error;
    QVERIFY2(registerOwnedCustomAnimeJanaiModel(pastedPath, &registered, &error), qPrintable(error));
    QVERIFY(registered.valid);
    QCOMPARE(registered.sha256, sha(graph));
    QCOMPARE(registered.bytes, qint64(graph.size()));
    QCOMPARE(registered.fileName, fileName);
    QVERIFY(!registered.id.isEmpty());
    QVERIFY(QFileInfo::exists(pastedPath + QStringLiteral(".source.json")));
    QVERIFY(changed.wait(5000));
    const auto runtimeVisible = findCustomAnimeJanaiModel(registered.id);
    QVERIFY(runtimeVisible.has_value());
    QCOMPARE(runtimeVisible->sha256, registered.sha256);
    QVERIFY(listUnregisteredOwnedAnimeJanaiModels().isEmpty());

    QVERIFY2(removeCustomAnimeJanaiModel(registered.fileName, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(pastedPath));
    QVERIFY(!QFileInfo::exists(pastedPath + QStringLiteral(".source.json")));
    QVERIFY(!findCustomAnimeJanaiModel(registered.id).has_value());
    QVERIFY(listUnregisteredOwnedAnimeJanaiModels().isEmpty());
}
