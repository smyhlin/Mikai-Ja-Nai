#include "TestAnimeJanaiRuntime.hpp"

#include "PlayerDialog.hpp"
#include "VideoSurface.hpp"
#include "native/bundle/NativePaths.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLibraryInfo>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QScopeGuard>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QProcess>

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

bool writeRecord(Backend backend, const QString& recordedBackend = {}) {
    const QString root = runtimePath(backend);
    QJsonArray entries;
    for (const QString& name : required(backend)) {
        const QByteArray bytes = minimalX64Pe();
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
    record.insert(QStringLiteral("heavyRifeSupported"), false);
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
    // main() already refused any pre-existing nonempty native tree. Claim only
    // this suite's version directory, never an installation left by a caller.
    QVERIFY2(!QFileInfo::exists(fixtureRoot()), "AnimeJaNai fixture root is already occupied");
    QVERIFY(QDir().mkpath(fixtureRoot()));
    ownsFixture_ = true;
}

void TestAnimeJanaiRuntime::init() {
    QVERIFY(ownsFixture_);
    QVERIFY(QDir(runtimePath(Backend::DirectML)).removeRecursively());
    QVERIFY(QDir(runtimePath(Backend::TensorRT)).removeRecursively());
}

void TestAnimeJanaiRuntime::cleanup() {
    if (!ownsFixture_) return;
    QVERIFY(QDir(runtimePath(Backend::DirectML)).removeRecursively());
    QVERIFY(QDir(runtimePath(Backend::TensorRT)).removeRecursively());
}

void TestAnimeJanaiRuntime::cleanupTestCase() {
    if (!ownsFixture_) return;
    QVERIFY(QDir(fixtureRoot()).removeRecursively());
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
