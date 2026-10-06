#include "TestNativeBundle.hpp"

#include "native/prepare/NativeArtifactStore.hpp"
#include "native/bundle/NativeBundle.hpp"
#include "native/bundle/NativePaths.hpp"
#include "native/bundle/NativePeImage.hpp"

#include <algorithm>

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QProcess>
#include <QtCore/QSaveFile>
#include <QtCore/QScopeGuard>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>

using namespace mplayer;
using namespace mplayer::native;

namespace {

constexpr quint64 kMpvApi = 131077;  // MPV_MAKE_VERSION(2, 5), the stock core's version
constexpr quint64 kTooOldMpvApi = 65537;  // 1.1
constexpr quint64 kTooNewMpvApi = 196866;  // 3.2
constexpr int kNativeProtocol = 1;
constexpr int kNativeEngineSchema = 1;

// A minimal but genuinely well-formed x64 PE image: DOS header, PE header,
// PE32+ optional header with sixteen data directories, and one section whose
// contents are an import table and the names it points at. This is a byte
// fixture for the parser, not a loadable library, and nothing here claims it
// would load.
QByteArray syntheticPe(const QStringList& imports, const QStringList& delayImports = {},
                       quint16 machine = 0x8664) {
    QByteArray image(0x800, '\0');
    const auto put8 = [&image](qsizetype at, quint8 value) {
        image[at] = char(value);
    };
    const auto put16 = [&put8](qsizetype at, quint16 value) {
        put8(at, quint8(value & 0xFF));
        put8(at + 1, quint8(value >> 8));
    };
    const auto put32 = [&put16](qsizetype at, quint32 value) {
        put16(at, quint16(value & 0xFFFF));
        put16(at + 2, quint16(value >> 16));
    };
    const auto put64 = [&put32](qsizetype at, quint64 value) {
        put32(at, quint32(value & 0xFFFFFFFFu));
        put32(at + 4, quint32(value >> 32));
    };

    put16(0, 0x5A4D);            // MZ
    put32(0x3C, 0x80);           // e_lfanew
    put32(0x80, 0x00004550);     // PE\0\0
    put16(0x84, machine);
    put16(0x86, 1);              // NumberOfSections
    put16(0x94, 0xF0);           // SizeOfOptionalHeader
    put16(0x96, 0x2022);         // Characteristics: executable, large address aware
    constexpr qsizetype kOptional = 0x98;
    put16(kOptional, 0x20B);     // PE32+
    put32(kOptional + 16, 0x1000);  // AddressOfEntryPoint
    put32(kOptional + 20, 0x1000);  // BaseOfCode
    put64(kOptional + 24, 0x140000000ull);  // ImageBase
    put32(kOptional + 32, 0x1000);  // SectionAlignment
    put32(kOptional + 36, 0x200);   // FileAlignment
    put32(kOptional + 56, 0x3000);  // SizeOfImage
    put32(kOptional + 60, 0x400);   // SizeOfHeaders
    put32(kOptional + 108, 16);     // NumberOfRvaAndSizes
    constexpr qsizetype kSection = kOptional + 0xF0;
    put32(kSection + 8, 0x400);    // VirtualSize
    put32(kSection + 12, 0x1000);  // VirtualAddress
    put32(kSection + 16, 0x400);   // SizeOfRawData
    put32(kSection + 20, 0x400);   // PointerToRawData
    put32(kSection + 36, 0x60000020);  // code, execute, read

    constexpr quint32 kImportRva = 0x1000;
    constexpr quint32 kDelayRva = 0x1080;
    constexpr quint32 kNamesRva = 0x1100;
    put32(kOptional + 112 + 1 * 8, kImportRva);
    put32(kOptional + 112 + 13 * 8, kDelayRva);

    qsizetype nameAt = 0x400 + (kNamesRva - kImportRva);
    const auto writeName = [&](const QString& name) {
        const QByteArray latin = name.toLatin1();
        for (qsizetype i = 0; i < latin.size(); ++i) image[nameAt + i] = latin.at(i);
        image[nameAt + latin.size()] = '\0';
        const quint32 rva = kImportRva + quint32(nameAt - 0x400);
        nameAt += latin.size() + 1;
        return rva;
    };

    for (qsizetype i = 0; i < imports.size(); ++i) {
        const qsizetype descriptor = 0x400 + i * 20;
        put32(descriptor + 12, writeName(imports.at(i)));
    }
    put32(kOptional + 112 + 1 * 8 + 4, quint32((imports.size() + 1) * 20));

    for (qsizetype i = 0; i < delayImports.size(); ++i) {
        const qsizetype descriptor = 0x400 + (kDelayRva - kImportRva) + i * 32;
        put32(descriptor, 1);  // attributes: names are RVAs
        put32(descriptor + 4, writeName(delayImports.at(i)));
    }
    put32(kOptional + 112 + 13 * 8 + 4, quint32((delayImports.size() + 1) * 32));

    return image;
}

void putPe32(QByteArray& image, qsizetype at, quint32 value) {
    for (int i = 0; i < 4; ++i) image[at + i] = char(value >> (8 * i));
}

QByteArray exportedPe(const QByteArray& name) {
    QByteArray image = syntheticPe({});
    putPe32(image, 0x98 + 112, 0x1200);
    putPe32(image, 0x98 + 116, 0x100);
    putPe32(image, 0x600 + 20, 1);
    putPe32(image, 0x600 + 24, 1);
    putPe32(image, 0x600 + 28, 0x1250);
    putPe32(image, 0x600 + 32, 0x1254);
    putPe32(image, 0x600 + 36, 0x1258);
    putPe32(image, 0x650, 0x1300);
    putPe32(image, 0x654, 0x1270);
    std::copy(name.cbegin(), name.cend(), image.begin() + 0x670);
    image[0x670 + name.size()] = '\0';
    image[0x700] = char(0xc3);
    return image;
}

QByteArray junk() { return QByteArrayLiteral("not a portable executable at all"); }

QString digestOf(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool writeFile(const QString& path, const QByteArray& data) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(data) == data.size();
}

bool readFile(const QString& path, QByteArray* out) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    *out = file.readAll();
    return true;
}

/// Everything a fixture needs; the defaults describe a two-DLL Standard bundle
/// whose only imports are Windows system modules.
struct Fixture {
    QString kind = QStringLiteral("standard");
    QString version = QStringLiteral("1.0.0");
    QString module = QStringLiteral("libmpv-2.dll");
    QStringList deps;
    quint64 mpvApiVersion = kMpvApi;
    int protocolVersion = 0;
    int engineSchemaVersion = 0;
    QMap<QString, QByteArray> files;
};

QJsonObject manifestFor(const Fixture& fixture) {
    QJsonArray files;
    QStringList paths = fixture.files.keys();
    std::sort(paths.begin(), paths.end());
    for (const QString& path : paths) {
        const QByteArray data = fixture.files.value(path);
        files.append(QJsonObject{{QStringLiteral("path"), path},
                                 {QStringLiteral("bytes"), double(data.size())},
                                 {QStringLiteral("sha256"), digestOf(data)}});
    }
    return QJsonObject{{QStringLiteral("schema"), QStringLiteral("mikai.native.bundle/1")},
                       {QStringLiteral("kind"), fixture.kind},
                       {QStringLiteral("version"), fixture.version},
                       {QStringLiteral("module"), fixture.module},
                       {QStringLiteral("mpvApiVersion"), double(fixture.mpvApiVersion)},
                       {QStringLiteral("protocolVersion"), double(fixture.protocolVersion)},
                       {QStringLiteral("engineSchemaVersion"), double(fixture.engineSchemaVersion)},
                       {QStringLiteral("deps"), QJsonArray::fromStringList(fixture.deps)},
                       {QStringLiteral("files"), files}};
}

bool writeManifest(const QString& root, const QJsonObject& manifest) {
    return writeFile(QDir(root).filePath(QStringLiteral("bundle.json")),
                     QJsonDocument(manifest).toJson(QJsonDocument::Indented));
}

bool writeFixture(const QString& root, const Fixture& fixture) {
    if (!QDir().mkpath(root)) return false;
    for (auto it = fixture.files.cbegin(); it != fixture.files.cend(); ++it) {
        if (!writeFile(QDir(root).filePath(it.key()), it.value())) return false;
    }
    return writeManifest(root, manifestFor(fixture));
}

/// The default Standard bundle: one x64 core plus a helper DLL beside it, both
/// importing only Windows system modules.
Fixture standardFixture(const QString& version = QStringLiteral("1.0.0")) {
    Fixture fixture;
    fixture.version = version;
    fixture.files.insert(QStringLiteral("libmpv-2.dll"),
                         syntheticPe({QStringLiteral("kernel32.dll"), QStringLiteral("user32.dll")}));
    fixture.files.insert(QStringLiteral("helper.dll"), syntheticPe({QStringLiteral("kernel32.dll")}));
    return fixture;
}

/// A Native bundle: the core plus a dependency directory holding a DLL that the
/// core imports, so the closure genuinely spans two directories.
Fixture nativeFixture(const QString& version = QStringLiteral("2.1.0")) {
    Fixture fixture;
    fixture.kind = QStringLiteral("native");
    fixture.version = version;
    fixture.deps = {QStringLiteral("trt")};
    fixture.protocolVersion = kNativeProtocol;
    fixture.engineSchemaVersion = kNativeEngineSchema;
    fixture.files.insert(QStringLiteral("libmpv-2.dll"),
                         syntheticPe({QStringLiteral("kernel32.dll"),
                                      QStringLiteral("nvinfer_10.dll")}));
    fixture.files.insert(QStringLiteral("trt/nvinfer_10.dll"),
                         syntheticPe({QStringLiteral("kernel32.dll")}));
    return fixture;
}

/// The complete, coherent preparation block a real prepared bundle carries: one
/// known backend type, an observed (non-placeholder) source pin and ABI, the
/// three tool executables, and the three provider runtime directories. The paths
/// are the ones `preparedNativeFixture` actually declares and writes.
QJsonObject completePreparationBlock() {
    return QJsonObject{
        {QStringLiteral("types"), QJsonArray{bundlePreparationTypes().value(0)}},
        {QStringLiteral("source"),
         QJsonObject{{QStringLiteral("coreSha"), QString(40, QLatin1Char('a'))},
                     {QStringLiteral("sourceAbi"), 3}}},
        {QStringLiteral("tools"),
         QJsonObject{{QStringLiteral("inspect"), QStringLiteral("tools/inspect.exe")},
                     {QStringLiteral("prep"), QStringLiteral("tools/prep.exe")},
                     {QStringLiteral("verifier"), QStringLiteral("tools/verifier.exe")}}},
        {QStringLiteral("runtime"),
         QJsonObject{{QStringLiteral("ort"), QStringLiteral("ort")},
                     {QStringLiteral("trt"), QStringLiteral("trt")},
                     {QStringLiteral("cuda"), QStringLiteral("cuda")}}}};
}

/// A Native bundle whose inventory holds everything a preparation block names:
/// the core, the dispatcher that owns the source ABI, the one backend library a
/// declared type resolves to, three tool executables, and three provider runtime
/// directories each carrying a declared DLL and therefore declared as a
/// dependency. Nothing here claims the toolchain runs; it is the layout the
/// reader accepts.
Fixture preparedNativeFixture(const QString& version = QStringLiteral("3.0.0")) {
    Fixture fixture;
    fixture.kind = QStringLiteral("native");
    fixture.version = version;
    fixture.deps = {QStringLiteral("ort"), QStringLiteral("trt"), QStringLiteral("cuda")};
    fixture.protocolVersion = kNativeProtocol;
    fixture.engineSchemaVersion = kNativeEngineSchema;
    const QString producer = bundlePreparationProducerLibrary();
    const QString backend = bundlePreparationBackendLibrary(bundlePreparationTypes().value(0));
    fixture.files.insert(QStringLiteral("libmpv-2.dll"),
                         syntheticPe({QStringLiteral("kernel32.dll"), producer}));
    fixture.files.insert(producer, syntheticPe({QStringLiteral("kernel32.dll")}));
    fixture.files.insert(backend, syntheticPe({QStringLiteral("kernel32.dll")}));
    fixture.files.insert(QStringLiteral("tools/inspect.exe"), QByteArrayLiteral("inspect helper"));
    fixture.files.insert(QStringLiteral("tools/prep.exe"), QByteArrayLiteral("prep helper"));
    fixture.files.insert(QStringLiteral("tools/verifier.exe"), QByteArrayLiteral("verify helper"));
    fixture.files.insert(QStringLiteral("ort/onnxruntime.dll"),
                         syntheticPe({QStringLiteral("kernel32.dll")}));
    fixture.files.insert(QStringLiteral("trt/nvinfer_10.dll"),
                         syntheticPe({QStringLiteral("kernel32.dll")}));
    fixture.files.insert(QStringLiteral("cuda/cudart64_12.dll"),
                         syntheticPe({QStringLiteral("kernel32.dll")}));
    return fixture;
}

/// Writes a fixture's files and a manifest carrying `preparation` verbatim.
/// `preparation` may be any JSON shape, including a partial or malformed one, so
/// the refusal tests exercise the reader rather than the fixture builder.
bool writePreparedFixture(const QString& root, const Fixture& fixture,
                          const QJsonValue& preparation = completePreparationBlock()) {
    if (!QDir().mkpath(root)) return false;
    for (auto it = fixture.files.cbegin(); it != fixture.files.cend(); ++it) {
        if (!writeFile(QDir(root).filePath(it.key()), it.value())) return false;
    }
    QJsonObject manifest = manifestFor(fixture);
    if (!preparation.isUndefined()) manifest.insert(QStringLiteral("preparation"), preparation);
    return writeManifest(root, manifest);
}

/// Entry names directly under `root`, sorted, so a read that must write nothing
/// can be shown to have changed nothing.
QStringList entryNames(const QString& root) {
    QStringList names;
    const QFileInfoList entries = QDir(root).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& entry : entries) names.append(entry.fileName());
    names.sort();
    return names;
}

QString tempDir() {
    static int counter = 0;
    const QString path = QDir(QDir::tempPath())
                             .filePath(QStringLiteral("mikai-bundle-test-%1-%2")
                                           .arg(QCoreApplication::applicationPid())
                                           .arg(++counter));
    QDir().mkpath(path);
    return path;
}

/// The real stock runtime, when this machine has one. The suite stays hermetic
/// without it: only the two stock-bridge tests consult this.
QString stockModulePath() {
    QString base = qEnvironmentVariable("MIKAI_STOCK_RUNTIME");
    if (base.isEmpty()) base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) return {};
    const QString candidate =
        QDir(base).filePath(QStringLiteral("MPVBackend/runtime/libmpv-2.dll"));
    return QFileInfo::exists(candidate) ? QFileInfo(candidate).absoluteFilePath() : QString{};
}

/// True when a read-only pointer file really does stop an atomic flip on this
/// filesystem and leaves the previous bytes readable. Windows refuses to
/// replace a read-only file; where some filesystem does not, the caller skips
/// rather than asserting a guarantee this platform does not give.
bool pointerFlipIsBlockedByReadOnly() {
    QTemporaryDir scratch;
    if (!scratch.isValid()) return false;
    const QString pointer = QDir(scratch.path()).filePath(QStringLiteral("current-standard"));
    if (!writeFile(pointer, QByteArrayLiteral("standard-1.0.0\n"))) return false;
    const QFile::Permissions writable = QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser |
                                        QFile::WriteUser;
    if (!QFile::setPermissions(pointer, QFile::ReadOwner | QFile::ReadUser)) return false;

    bool blocked = false;
    QSaveFile attempt(pointer);
    attempt.setDirectWriteFallback(false);
    if (attempt.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QByteArray payload = QByteArrayLiteral("standard-9.9.9\n");
        if (attempt.write(payload) != payload.size()) {
            attempt.cancelWriting();
            blocked = true;
        } else {
            blocked = !attempt.commit();
        }
    } else {
        blocked = true;
    }

    QFile::setPermissions(pointer, writable);
    QFile::remove(pointer);
    return blocked;
}

/// Creates a directory junction, which needs no elevation. Returns false when
/// the environment will not make one, so the junction test skips instead of
/// asserting something it could not set up.
bool makeJunction(const QString& link, const QString& target) {
    QProcess process;
    process.start(QStringLiteral("cmd.exe"),
                  {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"), link,
                   target});
    process.waitForFinished(20000);
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 &&
           QFileInfo(link).isDir();
}

}  // namespace

void TestNativeBundle::staticExportsDistinguishTheCoreFromAHelper() {
    const auto core = inspectPeExports(exportedPe("mpv_client_api_version"));
    QVERIFY2(core.ok, qPrintable(core.reason));
    QVERIFY(core.exportsMpvClientApiVersion);
    const auto helper = inspectPeExports(exportedPe("aji_create"));
    QVERIFY2(helper.ok, qPrintable(helper.reason));
    QVERIFY(!helper.exportsMpvClientApiVersion);
    const auto resource = inspectPeExports(syntheticPe({}));
    QVERIFY2(resource.ok, qPrintable(resource.reason));
    QVERIFY(!resource.exportsMpvClientApiVersion);
}

void TestNativeBundle::malformedExportsNeverClassifyAnImageAsTheCore_data() {
    QTest::addColumn<QByteArray>("image");
    QByteArray image = exportedPe("mpv_client_api_version");
    image.truncate(0x680);
    QTest::newRow("truncated-name") << image;
    image = exportedPe("mpv_client_api_version");
    putPe32(image, 0x600 + 24, 0xffffffffu);
    QTest::newRow("oversized-name-table") << image;
    image = exportedPe("mpv_client_api_version");
    image[0x658] = 1;
    QTest::newRow("ordinal-outside-functions") << image;
    image = exportedPe("mpv_client_api_version");
    putPe32(image, 0x650, 0);
    QTest::newRow("null-function") << image;
    image = exportedPe("mpv_client_api_version");
    putPe32(image, 0x98 + 112, 0xfffffff0u);
    QTest::newRow("overflowing-directory-range") << image;
    image = exportedPe("mpv_client_api_version");
    putPe32(image, 0x600 + 24, 2);
    putPe32(image, 0x654 + 4, 0x1400);
    putPe32(image, 0x600 + 36, 0x1260);
    QTest::newRow("bad-name-after-valid-core-name") << image;
}

void TestNativeBundle::malformedExportsNeverClassifyAnImageAsTheCore() {
    QFETCH(QByteArray, image);
    const auto result = inspectPeExports(image);
    QVERIFY(!result.ok);
    QVERIFY(!result.exportsMpvClientApiVersion);
}

void TestNativeBundle::aForwardedClientApiIsRefusedWithoutResolvingIt() {
    QByteArray image = exportedPe("mpv_client_api_version");
    putPe32(image, 0x650, 0x1290);
    const QByteArray forwarder("outside.mpv_client_api_version");
    std::copy(forwarder.cbegin(), forwarder.cend(), image.begin() + 0x690);
    const auto result = inspectPeExports(image);
    QVERIFY(!result.ok);
    QVERIFY(!result.exportsMpvClientApiVersion);
}

void TestNativeBundle::aValidStandardBundleIsReadAndValidated() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QVERIFY(writeFixture(temp.path(), standardFixture()));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QCOMPARE(bundle->kind, BundleKind::Standard);
    QCOMPARE(bundle->version, QStringLiteral("1.0.0"));
    QCOMPARE(bundle->mpvApiVersion, kMpvApi);
    QCOMPARE(bundle->protocolVersion, 0);
    QCOMPARE(bundle->files.size(), 2);
    QVERIFY2(validateBundle(*bundle, &reason), qPrintable(reason));

    // The descriptor is a projection of what was verified: absolute canonical
    // paths, and dependency directories the loader can hand to restricted
    // search. It says nothing about a loaded module.
    const MpvBundle descriptor = bundle->toMpvBundleDescriptor();
    QCOMPARE(descriptor.root, QFileInfo(temp.path()).canonicalFilePath());
    QCOMPARE(descriptor.modulePath,
             QFileInfo(QDir(temp.path()).filePath(QStringLiteral("libmpv-2.dll")))
                 .canonicalFilePath());
    QVERIFY(descriptor.descriptorId.size() == 64);
    QVERIFY(QDir::isAbsolutePath(descriptor.modulePath));
    QVERIFY(descriptor.deps.isEmpty());
}

void TestNativeBundle::aValidationVerdictIsRememberedUntilTheTreeChanges() {
    QTemporaryDir temp;
    QTemporaryDir cache;
    QVERIFY(temp.isValid());
    QVERIFY(cache.isValid());
    qputenv("MIKAI_NATIVE_CACHE", cache.filePath(QStringLiteral("verified.json")).toUtf8());
    QVERIFY(writeFixture(temp.path(), standardFixture()));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QVERIFY2(validateBundleCached(*bundle, &reason), qPrintable(reason));
    QVERIFY(QFileInfo::exists(cache.filePath(QStringLiteral("verified.json"))));
    // The second call is answered from the cache: nothing about the tree moved.
    QVERIFY2(validateBundleCached(*bundle, &reason), qPrintable(reason));

    // A module replaced with different bytes moves both its size and its
    // modification time, so the fingerprint no longer matches and the bytes are
    // proved again rather than trusted.
    const QString modulePath = QDir(temp.path()).filePath(QStringLiteral("libmpv-2.dll"));
    QVERIFY(writeFile(modulePath, QByteArray(64, 'z')));
    QVERIFY(!validateBundleCached(*bundle, &reason));
    QVERIFY(!reason.isEmpty());

    qunsetenv("MIKAI_NATIVE_CACHE");
}

void TestNativeBundle::aFailedValidationIsNeverRemembered() {
    QTemporaryDir temp;
    QTemporaryDir cache;
    QVERIFY(temp.isValid());
    QVERIFY(cache.isValid());
    qputenv("MIKAI_NATIVE_CACHE", cache.filePath(QStringLiteral("verified.json")).toUtf8());
    QVERIFY(writeFixture(temp.path(), standardFixture()));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));

    // Corrupt it, refuse it, then repair it: the refusal must not have been
    // written down, or a repaired bundle would stay refused until the cache was
    // deleted by hand.
    const QString modulePath = QDir(temp.path()).filePath(QStringLiteral("libmpv-2.dll"));
    QByteArray original;
    QVERIFY(readFile(modulePath, &original));
    QVERIFY(writeFile(modulePath, QByteArray(64, 'z')));
    QVERIFY(!validateBundleCached(*bundle, &reason));

    QVERIFY(writeFile(modulePath, original));
    QVERIFY2(validateBundleCached(*bundle, &reason), qPrintable(reason));

    // Dropping the verdict explicitly has the same effect as a first run.
    invalidateBundleValidation(bundle->root);
    QVERIFY2(validateBundleCached(*bundle, &reason), qPrintable(reason));

    qunsetenv("MIKAI_NATIVE_CACHE");
}

void TestNativeBundle::invalidatingARootAlsoDropsItsCachedDescriptor() {
    const QString module = stockModulePath();
    if (module.isEmpty()) QSKIP("this machine has no stock runtime to describe");
    QTemporaryDir cache;
    QVERIFY(cache.isValid());
    qputenv("MIKAI_NATIVE_CACHE", cache.filePath(QStringLiteral("verified.json")).toUtf8());

    QString reason;
    const std::optional<Bundle> first = describeStandardRuntimeCached(module, &reason);
    QVERIFY2(first.has_value(), qPrintable(reason));
    QVERIFY(QFileInfo::exists(cache.filePath(QStringLiteral("verified.json"))));

    // Answered from the cache while the tree is unchanged, and identical to what
    // the bytes described.
    const std::optional<Bundle> second = describeStandardRuntimeCached(module, &reason);
    QVERIFY(second.has_value());
    QCOMPARE(second->descriptorId, first->descriptorId);

    // A stock runtime that stops loading is invalidated by its root. The
    // descriptor has to go with the verdict: keeping it would answer the next
    // start with the description the loader just refused.
    invalidateBundleValidation(first->root);

    QFile file(cache.filePath(QStringLiteral("verified.json")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QVERIFY(object.value(QStringLiteral("stock")).toArray().isEmpty());
    QVERIFY(object.value(QStringLiteral("validations")).toArray().isEmpty());

    qunsetenv("MIKAI_NATIVE_CACHE");
}

void TestNativeBundle::anUnknownSchemaIsRefusedNotGuessed() {
    QTemporaryDir temp;
    Fixture fixture = standardFixture();
    QJsonObject manifest = manifestFor(fixture);
    manifest.insert(QStringLiteral("schema"), QStringLiteral("mikai.native.bundle/2"));
    QVERIFY(writeFixture(temp.path(), fixture));
    QVERIFY(writeManifest(temp.path(), manifest));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY(!bundle.has_value());
    QVERIFY(reason.contains(QStringLiteral("mikai.native.bundle/2")));
}

void TestNativeBundle::anUnknownMemberIsRefusedNotIgnored() {
    QTemporaryDir temp;
    Fixture fixture = standardFixture();
    QVERIFY(writeFixture(temp.path(), fixture));
    QJsonObject manifest = manifestFor(fixture);
    manifest.insert(QStringLiteral("modulePath"), QStringLiteral("libmpv-2.dll"));
    QVERIFY(writeManifest(temp.path(), manifest));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY(!bundle.has_value());
    QVERIFY(reason.contains(QStringLiteral("modulePath")));
}

void TestNativeBundle::aMissingRequiredMemberIsRefused() {
    QTemporaryDir temp;
    Fixture fixture = standardFixture();
    QVERIFY(writeFixture(temp.path(), fixture));
    QJsonObject manifest = manifestFor(fixture);
    manifest.remove(QStringLiteral("deps"));
    QVERIFY(writeManifest(temp.path(), manifest));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY(!bundle.has_value());
    QVERIFY(reason.contains(QStringLiteral("deps")));
}

void TestNativeBundle::anUnknownKindIsRefusedNotDefaulted() {
    QTemporaryDir temp;
    Fixture fixture = standardFixture();
    QVERIFY(writeFixture(temp.path(), fixture));
    QJsonObject manifest = manifestFor(fixture);
    manifest.insert(QStringLiteral("kind"), QStringLiteral("forked"));
    QVERIFY(writeManifest(temp.path(), manifest));

    QString reason;
    QVERIFY(!readBundle(temp.path(), &reason).has_value());
    QVERIFY(reason.contains(QStringLiteral("forked")));
}

void TestNativeBundle::aMalformedVersionIsRefused() {
    QTemporaryDir temp;
    Fixture fixture = standardFixture();
    fixture.version = QStringLiteral("1.0");
    QVERIFY(writeFixture(temp.path(), fixture));

    QString reason;
    QVERIFY(!readBundle(temp.path(), &reason).has_value());
    QVERIFY(reason.contains(QStringLiteral("1.0")));
}

void TestNativeBundle::aTraversingFilePathIsRefused() {
    QTemporaryDir temp;
    const QString outside = tempDir();
    // Imports no other file in this manifest uses: with helper.dll's own bytes
    // the record would also duplicate a digest, and the traversing path must be
    // the only defect the reader can find.
    const QByteArray evil = syntheticPe({QStringLiteral("kernel32.dll"),
                                         QStringLiteral("advapi32.dll")});
    QVERIFY(writeFile(QDir(outside).filePath(QStringLiteral("evil.dll")), evil));
    Fixture fixture = standardFixture();
    fixture.files.insert(QStringLiteral("../%1/evil.dll").arg(QFileInfo(outside).fileName()),
                         evil);
    QVERIFY(writeFixture(temp.path(), fixture));

    QString reason;
    QVERIFY(!readBundle(temp.path(), &reason).has_value());
    QVERIFY(reason.contains(QStringLiteral("..")));
}

void TestNativeBundle::anAbsoluteOrBackslashedPathIsRefused() {
    const QStringList unsafePaths = {
        QDir::temp().filePath(QStringLiteral("elsewhere.dll")),
        QStringLiteral("trt\\nvinfer.dll"),
        QStringLiteral("trt/NUL.dll"),
    };
    for (const QString& path : unsafePaths) {
        QTemporaryDir temp;
        const Fixture fixture = standardFixture();
        QVERIFY(writeFixture(temp.path(), fixture));
        QJsonObject manifest = manifestFor(fixture);
        QJsonArray files = manifest.value(QStringLiteral("files")).toArray();
        QJsonObject file = files.first().toObject();
        file.insert(QStringLiteral("path"), path);
        files.replace(0, file);
        manifest.insert(QStringLiteral("files"), files);
        QVERIFY(writeManifest(temp.path(), manifest));
        QString reason;
        QVERIFY(!readBundle(temp.path(), &reason).has_value());
    }
}

void TestNativeBundle::theManifestMayNotListItself() {
    QTemporaryDir temp;
    Fixture fixture = standardFixture();
    QVERIFY(writeFixture(temp.path(), fixture));
    QJsonObject manifest = manifestFor(fixture);
    QJsonArray files = manifest.value(QStringLiteral("files")).toArray();
    files.append(QJsonObject{{QStringLiteral("path"), QStringLiteral("bundle.json")},
                             {QStringLiteral("bytes"), 10},
                             {QStringLiteral("sha256"), QString(64, QLatin1Char('0'))}});
    manifest.insert(QStringLiteral("files"), files);
    QVERIFY(writeManifest(temp.path(), manifest));

    QString reason;
    QVERIFY(!readBundle(temp.path(), &reason).has_value());
    QVERIFY(reason.contains(QStringLiteral("bundle.json")));
}

void TestNativeBundle::aModuleOutsideTheInventoryIsRefused() {
    QTemporaryDir temp;
    Fixture fixture = standardFixture();
    QVERIFY(writeFixture(temp.path(), fixture));
    QJsonObject manifest = manifestFor(fixture);
    manifest.insert(QStringLiteral("module"), QStringLiteral("other/libmpv-2.dll"));
    QVERIFY(writeManifest(temp.path(), manifest));

    QString reason;
    QVERIFY(!readBundle(temp.path(), &reason).has_value());
    QVERIFY(reason.contains(QStringLiteral("other/libmpv-2.dll")));
}

void TestNativeBundle::aTruncatedModuleIsRefusedBySize() {
    QTemporaryDir temp;
    Fixture fixture = standardFixture();
    QVERIFY(writeFixture(temp.path(), fixture));
    // Same length in the manifest, fewer bytes on disk: the size check must
    // catch this before the digest, because "truncated" is the more useful name.
    QByteArray truncated = syntheticPe({QStringLiteral("kernel32.dll"),
                                         QStringLiteral("user32.dll")});
    truncated.chop(1);
    QVERIFY(writeFile(QDir(temp.path()).filePath(QStringLiteral("libmpv-2.dll")), truncated));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY(bundle.has_value());
    QVERIFY2(!validateBundle(*bundle, &reason), qPrintable(reason));
    QVERIFY(reason.contains(QStringLiteral("libmpv-2.dll")));
    QVERIFY(reason.contains(QStringLiteral("bytes")));
}

void TestNativeBundle::aTamperedModuleIsRefusedByDigest() {
    QTemporaryDir temp;
    Fixture fixture = standardFixture();
    QVERIFY(writeFixture(temp.path(), fixture));
    // Same size, different bytes: only the digest can catch this.
    QByteArray tampered =
        syntheticPe({QStringLiteral("kernel32.dll"), QStringLiteral("user32.dll")});
    tampered[tampered.size() / 2] = char(tampered.at(tampered.size() / 2) ^ 0xFF);
    QVERIFY(writeFile(QDir(temp.path()).filePath(QStringLiteral("libmpv-2.dll")), tampered));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY(bundle.has_value());
    QVERIFY2(!validateBundle(*bundle, &reason), qPrintable(reason));
    QVERIFY(reason.contains(QStringLiteral("hashes to")));
}

void TestNativeBundle::anUndeclaredFileInTheRootIsRefused() {
    QTemporaryDir temp;
    QVERIFY(writeFixture(temp.path(), standardFixture()));
    QVERIFY(writeFile(QDir(temp.path()).filePath(QStringLiteral("notes.txt")),
                      QByteArrayLiteral("a file nothing verified")));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY(bundle.has_value());
    QVERIFY2(!validateBundle(*bundle, &reason), qPrintable(reason));
    QVERIFY(reason.contains(QStringLiteral("notes.txt")));
}

void TestNativeBundle::aNonPeModuleIsRefused() {
    QTemporaryDir temp;
    Fixture fixture = standardFixture();
    fixture.files.insert(QStringLiteral("libmpv-2.dll"), junk());
    QVERIFY(writeFixture(temp.path(), fixture));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY(bundle.has_value());
    QVERIFY2(!validateBundle(*bundle, &reason), qPrintable(reason));
    QVERIFY(reason.contains(QStringLiteral("libmpv-2.dll")));
    QVERIFY(reason.contains(QStringLiteral("PE")));
}

void TestNativeBundle::anX86ModuleIsRefused() {
    QTemporaryDir temp;
    Fixture fixture = standardFixture();
    fixture.files.insert(QStringLiteral("libmpv-2.dll"),
                         syntheticPe({QStringLiteral("kernel32.dll")}, {}, 0x014C));
    QVERIFY(writeFixture(temp.path(), fixture));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY(bundle.has_value());
    QVERIFY2(!validateBundle(*bundle, &reason), qPrintable(reason));
    QVERIFY(reason.contains(QStringLiteral("x64")));
}

void TestNativeBundle::anImportOutsideTheBundleIsRefused() {
    QTemporaryDir temp;
    Fixture fixture = standardFixture();
    fixture.files.insert(
        QStringLiteral("libmpv-2.dll"),
        syntheticPe({QStringLiteral("kernel32.dll"), QStringLiteral("libavcodec-61.dll")}));
    QVERIFY(writeFixture(temp.path(), fixture));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY(bundle.has_value());
    QVERIFY2(!validateBundle(*bundle, &reason), qPrintable(reason));
    QVERIFY(reason.contains(QStringLiteral("libavcodec-61.dll")));
}

void TestNativeBundle::aDelayImportOutsideTheBundleIsRefused() {
    QTemporaryDir temp;
    Fixture fixture = standardFixture();
    // Eager imports are all system modules; the closure is broken only by the
    // delayed one, so passing means the delay table was really walked.
    fixture.files.insert(QStringLiteral("libmpv-2.dll"),
                         syntheticPe({QStringLiteral("kernel32.dll")},
                                     {QStringLiteral("cudnn64_9.dll")}));
    QVERIFY(writeFixture(temp.path(), fixture));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY(bundle.has_value());
    QVERIFY2(!validateBundle(*bundle, &reason), qPrintable(reason));
    QVERIFY(reason.contains(QStringLiteral("cudnn64_9.dll")));
}

void TestNativeBundle::aSystemImportIsAccepted() {
    QTemporaryDir temp;
    Fixture fixture = nativeFixture();
    fixture.files.insert(QStringLiteral("libmpv-2.dll"),
                         syntheticPe({QStringLiteral("kernel32.dll"),
                                      QStringLiteral("nvinfer_10.dll"),
                                      QStringLiteral("GDIPLUS.dll")},
                                     {QStringLiteral("WSOCK32.dll")}));
    QVERIFY(writeFixture(temp.path(), fixture));
    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QVERIFY2(validateBundle(*bundle, &reason), qPrintable(reason));
}

void TestNativeBundle::aDependencyDirectoryHoldingNoDeclaredFileIsRefused() {
    QTemporaryDir temp;
    Fixture fixture = nativeFixture();
    fixture.deps = {QStringLiteral("empty")};
    QVERIFY(writeFixture(temp.path(), fixture));
    QDir().mkpath(QDir(temp.path()).filePath(QStringLiteral("empty")));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY(bundle.has_value());
    QVERIFY2(!validateBundle(*bundle, &reason), qPrintable(reason));
    QVERIFY(reason.contains(QStringLiteral("empty")));
}

void TestNativeBundle::anUnsupportedNativeProtocolVersionIsRefused() {
    QTemporaryDir temp;
    Fixture fixture = nativeFixture();
    fixture.protocolVersion = kNativeProtocol + 1;
    QVERIFY(writeFixture(temp.path(), fixture));

    QString reason;
    QVERIFY(!readBundle(temp.path(), &reason).has_value());
    QVERIFY(reason.contains(QStringLiteral("protocol version")));
}

void TestNativeBundle::aStandardBundleDeclaringANativeProtocolIsRefused() {
    QTemporaryDir temp;
    Fixture fixture = standardFixture();
    fixture.protocolVersion = kNativeProtocol;
    QVERIFY(writeFixture(temp.path(), fixture));

    QString reason;
    QVERIFY(!readBundle(temp.path(), &reason).has_value());
    QVERIFY(reason.contains(QStringLiteral("standard bundle")));
}

void TestNativeBundle::anMpvApiVersionOutsideTheSupportedWindowIsRefused() {
    for (const quint64 version : {kTooOldMpvApi, kTooNewMpvApi}) {
        QTemporaryDir temp;
        Fixture fixture = standardFixture();
        fixture.mpvApiVersion = version;
        QVERIFY(writeFixture(temp.path(), fixture));
        QString reason;
        QVERIFY(!readBundle(temp.path(), &reason).has_value());
        QVERIFY(reason.contains(QStringLiteral("supported window")));
    }
}

void TestNativeBundle::aNativeBundleWithoutAnMpvApiVersionIsRefused() {
    QTemporaryDir temp;
    Fixture fixture = nativeFixture();
    fixture.mpvApiVersion = 0;
    QVERIFY(writeFixture(temp.path(), fixture));

    QString reason;
    QVERIFY(!readBundle(temp.path(), &reason).has_value());
    QVERIFY(reason.contains(QStringLiteral("mpv client API version")));
}

void TestNativeBundle::theDescriptorCarriesTheValidatedRootModuleAndDeps() {
    QTemporaryDir temp;
    QVERIFY(writeFixture(temp.path(), nativeFixture()));
    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));

    const MpvBundle descriptor = bundle->toMpvBundleDescriptor();
    QCOMPARE(descriptor.root, QFileInfo(temp.path()).canonicalFilePath());
    QCOMPARE(descriptor.protocolVersion, kNativeProtocol);
    QCOMPARE(descriptor.deps.size(), 1);
    // The manifest says "trt"; the loader needs the directory itself.
    QCOMPARE(descriptor.deps.first(),
             QDir::cleanPath(QDir(descriptor.root).filePath(QStringLiteral("trt"))));
    QVERIFY(QDir::isAbsolutePath(descriptor.deps.first()));
    // Identity is a digest of the manifest bytes, so it is stable and content
    // addressed rather than derived from the path.
    QCOMPARE(descriptor.descriptorId, bundle->descriptorId);
}

void TestNativeBundle::discoveryReportsNothingWhenThereIsNothing() {
    QTemporaryDir temp;
    QStringList rejections;
    const QList<Bundle> found = discoverBundles({temp.path()}, &rejections);
    QVERIFY(found.isEmpty());
    QVERIFY(rejections.isEmpty());
}

void TestNativeBundle::discoveryFindsAValidBundleAndNamesNoRejection() {
    QTemporaryDir temp;
    const QString root = QDir(temp.path()).filePath(QStringLiteral("standard-1.0.0"));
    QVERIFY(writeFixture(root, standardFixture()));

    QStringList rejections;
    const QList<Bundle> found = discoverBundles({temp.path()}, &rejections);
    QCOMPARE(found.size(), 1);
    QVERIFY(rejections.isEmpty());
    QCOMPARE(found.first().version, QStringLiteral("1.0.0"));
}

void TestNativeBundle::theCurrentPointerOutranksAnOlderVersion() {
    QTemporaryDir temp;
    QVERIFY(writeFixture(QDir(temp.path()).filePath(QStringLiteral("standard-1.0.0")),
                        standardFixture(QStringLiteral("1.0.0"))));
    QVERIFY(writeFixture(QDir(temp.path()).filePath(QStringLiteral("standard-1.9.0")),
                        standardFixture(QStringLiteral("1.9.0"))));
    QVERIFY(writeFile(QDir(temp.path()).filePath(bundlePointerFileName(BundleKind::Standard)),
                      QByteArrayLiteral("standard-1.0.0\n")));

    QStringList rejections;
    const QList<Bundle> found = discoverBundles({temp.path()}, &rejections);
    // The pointer decides which one is selected; the others are still valid and
    // reported, so a caller can see what else is installed.
    QCOMPARE(found.first().version, QStringLiteral("1.0.0"));
    QCOMPARE(found.size(), 2);
    QVERIFY(rejections.isEmpty());
}

void TestNativeBundle::anInvalidCandidateDoesNotHideAValidOneOfTheSameKind() {
    QTemporaryDir temp;
    // Newest first, and the newer one is the broken one: a bad candidate must
    // not end the search.
    QVERIFY(writeFixture(QDir(temp.path()).filePath(QStringLiteral("standard-2.0.0")),
                        standardFixture(QStringLiteral("2.0.0"))));
    QVERIFY(writeFile(QDir(temp.path()).filePath(QStringLiteral("standard-2.0.0/libmpv-2.dll")),
                      junk()));
    QVERIFY(writeFixture(QDir(temp.path()).filePath(QStringLiteral("standard-1.0.0")),
                        standardFixture(QStringLiteral("1.0.0"))));

    QStringList rejections;
    const QList<Bundle> found = discoverBundles({temp.path()}, &rejections);
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().version, QStringLiteral("1.0.0"));
    QCOMPARE(rejections.size(), 1);
    QVERIFY(rejections.first().contains(QStringLiteral("standard-2.0.0")));
}

void TestNativeBundle::aCorruptCandidateIsRejectedByName() {
    QTemporaryDir temp;
    const QString broken = QDir(temp.path()).filePath(QStringLiteral("standard-1.0.0"));
    QVERIFY(writeFixture(broken, standardFixture()));
    QVERIFY(writeFile(QDir(broken).filePath(QStringLiteral("libmpv-2.dll")), junk()));

    QStringList rejections;
    QVERIFY(discoverBundles({temp.path()}, &rejections).isEmpty());
    QCOMPARE(rejections.size(), 1);
    QVERIFY(rejections.first().contains(QStringLiteral("libmpv-2.dll")));
}

void TestNativeBundle::aStagingDirectoryIsNeverACandidate() {
    QTemporaryDir temp;
    const QString staging =
        QDir(temp.path()).filePath(QStringLiteral(".staging-standard-1.0.0"));
    QVERIFY(writeFixture(staging, standardFixture()));

    QStringList rejections;
    QVERIFY(discoverBundles({temp.path()}, &rejections).isEmpty());
    QVERIFY(rejections.isEmpty());
}

void TestNativeBundle::aCorruptPointerDoesNotHideVersionedBundles() {
    QTemporaryDir temp;
    QVERIFY(writeFixture(QDir(temp.path()).filePath(QStringLiteral("standard-1.0.0")),
                        standardFixture()));
    QVERIFY(writeFile(QDir(temp.path()).filePath(bundlePointerFileName(BundleKind::Standard)),
                      QByteArrayLiteral("../../escape\n")));

    QStringList rejections;
    const QList<Bundle> found = discoverBundles({temp.path()}, &rejections);
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().version, QStringLiteral("1.0.0"));
    QCOMPARE(rejections.size(), 1);
    QVERIFY(rejections.first().contains(bundlePointerFileName(BundleKind::Standard)));
}

void TestNativeBundle::aCorruptBundleBehindTheCurrentPointerStillSelectsThePreviousOne() {
    QTemporaryDir temp;
    // The pointer names the newest bundle and that one is broken: the selection
    // a previous install decided must not be the last word.
    QVERIFY(writeFixture(QDir(temp.path()).filePath(QStringLiteral("standard-1.0.0")),
                        standardFixture(QStringLiteral("1.0.0"))));
    const QString corrupt = QDir(temp.path()).filePath(QStringLiteral("standard-2.0.0"));
    QVERIFY(writeFixture(corrupt, standardFixture(QStringLiteral("2.0.0"))));
    QVERIFY(writeFile(QDir(corrupt).filePath(QStringLiteral("libmpv-2.dll")), junk()));
    QVERIFY(writeFile(QDir(temp.path()).filePath(bundlePointerFileName(BundleKind::Standard)),
                      QByteArrayLiteral("standard-2.0.0\n")));

    QStringList rejections;
    const QList<Bundle> found = discoverBundles({temp.path()}, &rejections);

    // The valid sibling is what a reader gets, and the broken one is named so a
    // user can act on it rather than silently running the older bundle.
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().version, QStringLiteral("1.0.0"));
    QVERIFY(!rejections.isEmpty());
    for (const QString& rejection : rejections) {
        QVERIFY2(rejection.contains(QStringLiteral("standard-2.0.0")), qPrintable(rejection));
        QVERIFY(!rejection.contains(QStringLiteral("standard-1.0.0")));
    }
}

void TestNativeBundle::discoveryOverTheDefaultRootsFindsAnInstalledBundleAndSkipsTheStockTree() {
    // No roots argument: the sweep must cover the app-owned runtime storage
    // and native/runtime beside the executable, and nothing else.
    const QString storage = runtimeStorageRoot();
    QCOMPARE(QDir::cleanPath(storage),
             QDir::cleanPath(QDir(nativeStorageRoot()).filePath(QStringLiteral("runtime"))));
    const QString beside = QDir(QCoreApplication::applicationDirPath())
                               .filePath(QStringLiteral("native/runtime"));
    QVERIFY(QDir::isAbsolutePath(beside));
    QVERIFY2(storage.compare(beside, Qt::CaseInsensitive) != 0, qPrintable(beside));

    // A uniquely versioned bundle, so a real installation beside it in the
    // user's AppData is neither read nor disturbed. The probe publishes into the
    // real app-owned runtime root and restores it afterwards whatever happens.
    const QString version = QStringLiteral("0.0.0-bundleprobe");
    QTemporaryDir source;
    QVERIFY(writeFixture(source.path(), standardFixture(version)));
    QString reason;
    const std::optional<Bundle> bundle = readBundle(source.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));

    const QString pointerPath =
        QDir(storage).filePath(bundlePointerFileName(BundleKind::Standard));
    const QString expectedRoot =
        QDir(storage).filePath(bundleDirectoryName(BundleKind::Standard, version));
    QByteArray pointerBefore;
    const bool hadPointer = readFile(pointerPath, &pointerBefore);
    // A published directory is immutable, so a probe left by an interrupted
    // run has to go before this one can claim the name.
    QDir(expectedRoot).removeRecursively();
    const auto restore = qScopeGuard([&] {
        QDir(expectedRoot).removeRecursively();
        if (hadPointer) {
            writeFile(pointerPath, pointerBefore);
        } else {
            QFile::remove(pointerPath);
        }
    });

    QVERIFY2(installAtomic(*bundle, storage, &reason), qPrintable(reason));
    QStringList rejections;
    const QList<Bundle> found = discoverBundles({}, &rejections);

    bool sawProbe = false;
    for (const Bundle& candidate : found) {
        if (candidate.root.compare(QFileInfo(expectedRoot).canonicalFilePath(),
                                   Qt::CaseInsensitive) == 0)
            sawProbe = true;
    }
    QVERIFY2(sawProbe, "the bundle installed under the default runtime root was not discovered");

    // The stock MPVBackend/runtime tree is not one of the default roots, and
    // naming it explicitly is skipped outright rather than searched: a bundle
    // sitting there is neither reported nor rejected.
    QTemporaryDir stockTree;
    QVERIFY(QDir().mkpath(QDir(stockTree.path()).filePath(QStringLiteral("MPVBackend/runtime"))));
    const QString stockRoot =
        QDir(stockTree.path()).filePath(QStringLiteral("MPVBackend/runtime"));
    QVERIFY(writeFixture(
        QDir(stockRoot).filePath(bundleDirectoryName(BundleKind::Standard, version)),
        standardFixture(version)));
    QStringList stockRejections;
    QVERIFY(discoverBundles({stockRoot}, &stockRejections).isEmpty());
    QVERIFY(stockRejections.isEmpty());
}

void TestNativeBundle::aDirectoryNamedForOneKindHoldingTheOtherIsRefusedByName() {
    QTemporaryDir temp;
    // A directory named for one kind holding the other kind's manifest: the
    // name says where it belongs, the manifest says what it is, and the two
    // disagreeing is never resolved silently.
    const QString root = QDir(temp.path()).filePath(QStringLiteral("standard-2.0.0"));
    QVERIFY(writeFixture(root, nativeFixture(QStringLiteral("2.0.0"))));

    QStringList rejections;
    QVERIFY(discoverBundles({temp.path()}, &rejections).isEmpty());
    QCOMPARE(rejections.size(), 1);
    QVERIFY2(rejections.first().contains(QStringLiteral("standard-2.0.0")),
             qPrintable(rejections.first()));
    QVERIFY2(rejections.first().contains(QStringLiteral("native")), qPrintable(rejections.first()));
    QVERIFY2(rejections.first().contains(QStringLiteral("standard")), qPrintable(rejections.first()));

    // The same bundle under a directory named for what it holds is found, so
    // the refusal is about the disagreement rather than about native bundles.
    QTemporaryDir honest;
    QVERIFY(writeFixture(QDir(honest.path()).filePath(QStringLiteral("native-2.0.0")),
                        nativeFixture(QStringLiteral("2.0.0"))));
    QStringList honestRejections;
    const QList<Bundle> found = discoverBundles({honest.path()}, &honestRejections);
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().kind, BundleKind::Native);
    QVERIFY(honestRejections.isEmpty());
}

void TestNativeBundle::aPublishedBundleDirectoryIsNotDescribedAsAStockRuntime() {
    // findRuntime() walks the application directory recursively, so a Native
    // bundle published beside the executable is reachable through the stock
    // path. Describing it as Standard would report the wrong kind for a core
    // whose own manifest says otherwise.
    QTemporaryDir appDir;
    QTemporaryDir scratch;
    const QString stock = QDir(appDir.path()).filePath(QStringLiteral("native/runtime"));
    const QString published =
        QDir(stock).filePath(bundleDirectoryName(BundleKind::Native, QStringLiteral("2.1.0")));
    QVERIFY(writeFixture(published, nativeFixture()));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(published, &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QCOMPARE(bundle->kind, BundleKind::Native);

    // The published root is refused by name, before anything is inventoried.
    const QString module = QDir(published).filePath(QStringLiteral("libmpv-2.dll"));
    QVERIFY(QFileInfo::exists(module));
    reason.clear();
    const std::optional<Bundle> described = describeStandardRuntime(module, &reason);
    QVERIFY(!described.has_value());
    QVERIFY2(reason.contains(QStringLiteral("bundle.json")), qPrintable(reason));
    QVERIFY2(reason.contains(bundleDirectoryName(BundleKind::Native, QStringLiteral("2.1.0"))),
             qPrintable(reason));

    // The refusal is about the manifest beside the module, not the module: the
    // very same bytes in a directory with no bundle.json describe as Standard.
    const QString bare = QDir(scratch.path()).filePath(QStringLiteral("bare"));
    QVERIFY(QDir().mkpath(QDir(bare).filePath(QStringLiteral("trt"))));
    for (const BundleFile& entry : bundle->files) {
        QByteArray data;
        QVERIFY(readFile(QDir(published).filePath(entry.path), &data));
        QVERIFY(writeFile(QDir(bare).filePath(entry.path), data));
    }
    reason.clear();
    const std::optional<Bundle> stockBundle =
        describeStandardRuntime(QDir(bare).filePath(QStringLiteral("libmpv-2.dll")), &reason);
    QVERIFY2(stockBundle.has_value(), qPrintable(reason));
    QCOMPARE(stockBundle->kind, BundleKind::Standard);
    QVERIFY(stockBundle->version.startsWith(QStringLiteral("stock-")));
}

void TestNativeBundle::installPublishesAnImmutableVersionedDirectoryAndSelectsIt() {
    QTemporaryDir source;
    QTemporaryDir runtime;
    QVERIFY(writeFixture(source.path(), standardFixture(QStringLiteral("1.2.3"))));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(source.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QVERIFY2(installAtomic(*bundle, runtime.path(), &reason), qPrintable(reason));

    const QString published =
        QDir(runtime.path()).filePath(QStringLiteral("standard-1.2.3"));
    QVERIFY(QFileInfo(published).isDir());
    QVERIFY(QFileInfo(QDir(published).filePath(QStringLiteral("libmpv-2.dll"))).isFile());
    QVERIFY(QFileInfo(QDir(published).filePath(QStringLiteral("bundle.json"))).isFile());

    QByteArray pointer;
    QVERIFY(readFile(QDir(runtime.path()).filePath(bundlePointerFileName(BundleKind::Standard)),
                      &pointer));
    QCOMPARE(QString::fromUtf8(pointer).trimmed(), QStringLiteral("standard-1.2.3"));

    QStringList rejections;
    const QList<Bundle> found = discoverBundles({runtime.path()}, &rejections);
    QCOMPARE(found.size(), 1);
    QVERIFY(rejections.isEmpty());

    // A published directory is immutable: the same version cannot be published
    // twice, and the first one is not rewritten.
    QVERIFY(!installAtomic(*bundle, runtime.path(), &reason));
    QVERIFY(reason.contains(QStringLiteral("immutable")));
}

void TestNativeBundle::theInstalledBundleIsSelfContainedAfterTheSourceIsGone() {
    QTemporaryDir source;
    QTemporaryDir runtime;
    QVERIFY(writeFixture(source.path(), nativeFixture()));
    QString reason;
    const std::optional<Bundle> bundle = readBundle(source.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QVERIFY2(installAtomic(*bundle, runtime.path(), &reason), qPrintable(reason));
    QVERIFY(QDir(source.path()).removeRecursively());

    QStringList rejections;
    const QList<Bundle> found = discoverBundles({runtime.path()}, &rejections);
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().kind, BundleKind::Native);
    QVERIFY2(validateBundle(found.first(), &reason), qPrintable(reason));
    // The dependency directory travelled with the bundle, so restricted search
    // has somewhere to look.
    QCOMPARE(found.first().toMpvBundleDescriptor().deps.size(), 1);
}

void TestNativeBundle::anInterruptedInstallLeavesNoVisibleBundleAndKeepsTheSelection() {
    QTemporaryDir source;
    QTemporaryDir runtime;
    QVERIFY(writeFixture(source.path(), standardFixture(QStringLiteral("1.0.0"))));
    QString reason;
    const std::optional<Bundle> first = readBundle(source.path(), &reason);
    QVERIFY2(first.has_value(), qPrintable(reason));
    QVERIFY2(installAtomic(*first, runtime.path(), &reason), qPrintable(reason));

    // An interrupted second install: a staging directory holding a half-copied
    // bundle, and no pointer change.
    const QString staging =
        QDir(runtime.path()).filePath(QStringLiteral(".staging-standard-2.0.0"));
    QVERIFY(writeFixture(staging, standardFixture(QStringLiteral("2.0.0"))));
    QVERIFY(QFile::remove(QDir(staging).filePath(QStringLiteral("libmpv-2.dll"))));

    QStringList rejections;
    QList<Bundle> found = discoverBundles({runtime.path()}, &rejections);
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().version, QStringLiteral("1.0.0"));
    QVERIFY(rejections.isEmpty());

    // Retrying clears the stale scratch and succeeds; the previous bundle is
    // still there either way.
    QVERIFY(writeFixture(source.path(), standardFixture(QStringLiteral("2.0.0"))));
    const std::optional<Bundle> second = readBundle(source.path(), &reason);
    QVERIFY2(second.has_value(), qPrintable(reason));
    QVERIFY2(installAtomic(*second, runtime.path(), &reason), qPrintable(reason));
    QVERIFY(!QFileInfo::exists(staging));
    QVERIFY(QFileInfo(QDir(runtime.path()).filePath(QStringLiteral("standard-1.0.0"))).isDir());
    QVERIFY(QFileInfo(QDir(runtime.path()).filePath(QStringLiteral("standard-2.0.0"))).isDir());
    found = discoverBundles({runtime.path()}, &rejections);
    QCOMPARE(found.first().version, QStringLiteral("2.0.0"));
}

void TestNativeBundle::aFailingInstallKeepsThePreviousSelectionUsable() {
    QTemporaryDir source;
    QTemporaryDir runtime;
    QVERIFY(writeFixture(source.path(), standardFixture(QStringLiteral("1.0.0"))));
    QString reason;
    const std::optional<Bundle> first = readBundle(source.path(), &reason);
    QVERIFY2(first.has_value(), qPrintable(reason));
    QVERIFY2(installAtomic(*first, runtime.path(), &reason), qPrintable(reason));

    // The second source is broken after it was parsed: the inventory no longer
    // matches the bytes.
    QVERIFY(writeFixture(source.path(), standardFixture(QStringLiteral("2.0.0"))));
    const std::optional<Bundle> second = readBundle(source.path(), &reason);
    QVERIFY2(second.has_value(), qPrintable(reason));
    QVERIFY(QFile::remove(QDir(source.path()).filePath(QStringLiteral("helper.dll"))));

    QVERIFY2(!installAtomic(*second, runtime.path(), &reason), qPrintable(reason));
    QVERIFY(reason.contains(QStringLiteral("helper.dll")));

    // Nothing half-installed is visible and the previous selection still works.
    QVERIFY(!QFileInfo::exists(QDir(runtime.path()).filePath(QStringLiteral("standard-2.0.0"))));
    const QFileInfoList leftovers =
        QDir(runtime.path()).entryInfoList(QDir::Dirs | QDir::Files | QDir::Hidden | QDir::System |
                                           QDir::NoDotAndDotDot);
    for (const QFileInfo& entry : leftovers)
        QVERIFY2(!entry.fileName().startsWith(QStringLiteral(".staging")), qPrintable(entry.fileName()));

    QByteArray pointer;
    QVERIFY(readFile(QDir(runtime.path()).filePath(bundlePointerFileName(BundleKind::Standard)),
                      &pointer));
    QCOMPARE(QString::fromUtf8(pointer).trimmed(), QStringLiteral("standard-1.0.0"));
    QStringList rejections;
    const QList<Bundle> found = discoverBundles({runtime.path()}, &rejections);
    QCOMPARE(found.size(), 1);
    QVERIFY2(validateBundle(found.first(), &reason), qPrintable(reason));
}

void TestNativeBundle::republishingAnExistingVersionIsRefused() {
    QTemporaryDir source;
    QTemporaryDir runtime;
    QVERIFY(writeFixture(source.path(), standardFixture(QStringLiteral("1.0.0"))));
    QString reason;
    const std::optional<Bundle> bundle = readBundle(source.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QVERIFY2(installAtomic(*bundle, runtime.path(), &reason), qPrintable(reason));

    const QString published =
        QDir(runtime.path()).filePath(QStringLiteral("standard-1.0.0"));
    const qint64 before = QFileInfo(QDir(published).filePath(QStringLiteral("libmpv-2.dll"))).size();
    QVERIFY(!installAtomic(*bundle, runtime.path(), &reason));
    QCOMPARE(QFileInfo(QDir(published).filePath(QStringLiteral("libmpv-2.dll"))).size(), before);
}

void TestNativeBundle::installingIntoTheStockRuntimeTreeIsRefused() {
    QTemporaryDir temp;
    const QString stock =
        QDir(temp.path()).filePath(QStringLiteral("MPVBackend/runtime"));
    QVERIFY(QDir().mkpath(stock));

    Fixture fixture = standardFixture();
    QVERIFY(writeFixture(temp.path(), fixture));
    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));

    QVERIFY(!installAtomic(*bundle, stock, &reason));
    QVERIFY(reason.contains(QStringLiteral("MPVBackend")));
    // Refused before anything was written, so the stock tree is untouched.
    QVERIFY(QDir(stock).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
}

void TestNativeBundle::aNativeBundleInstallsAndIsDiscoveredAsNative() {
    QTemporaryDir source;
    QTemporaryDir runtime;
    QVERIFY(writeFixture(source.path(), nativeFixture()));
    QString reason;
    const std::optional<Bundle> bundle = readBundle(source.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QVERIFY2(installAtomic(*bundle, runtime.path(), &reason), qPrintable(reason));

    QByteArray pointer;
    QVERIFY(readFile(QDir(runtime.path()).filePath(bundlePointerFileName(BundleKind::Native)),
                      &pointer));
    QCOMPARE(QString::fromUtf8(pointer).trimmed(), QStringLiteral("native-2.1.0"));
    QVERIFY(!QFileInfo::exists(
        QDir(runtime.path()).filePath(bundlePointerFileName(BundleKind::Standard))));

    QStringList rejections;
    const QList<Bundle> found = discoverBundles({runtime.path()}, &rejections);
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().kind, BundleKind::Native);
    QCOMPARE(found.first().toMpvBundleDescriptor().protocolVersion, kNativeProtocol);
}

void TestNativeBundle::aBundlePublishedWithoutAPointerFlipLeavesThePreviousSelection() {
    if (!pointerFlipIsBlockedByReadOnly())
        QSKIP("this filesystem replaces a read-only pointer, so the flip cannot be blocked");

    QTemporaryDir source;
    QTemporaryDir runtime;
    QVERIFY(writeFixture(source.path(), standardFixture(QStringLiteral("1.0.0"))));
    QString reason;
    const std::optional<Bundle> first = readBundle(source.path(), &reason);
    QVERIFY2(first.has_value(), qPrintable(reason));
    QVERIFY2(installAtomic(*first, runtime.path(), &reason), qPrintable(reason));

    // The pointer now decides the selection, and it is made unwritable, so the
    // flip at the end of the next install cannot land.
    const QString pointerPath =
        QDir(runtime.path()).filePath(bundlePointerFileName(BundleKind::Standard));
    const QFile::Permissions writable = QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser |
                                        QFile::WriteUser;
    QVERIFY(QFile::setPermissions(pointerPath, QFile::ReadOwner | QFile::ReadUser));

    // The second bundle is published, but not selected.
    QVERIFY(writeFixture(source.path(), standardFixture(QStringLiteral("2.0.0"))));
    const std::optional<Bundle> second = readBundle(source.path(), &reason);
    QVERIFY2(second.has_value(), qPrintable(reason));
    QVERIFY2(!installAtomic(*second, runtime.path(), &reason), qPrintable(reason));
    QVERIFY2(reason.contains(QStringLiteral("not selected")), qPrintable(reason));
    QVERIFY(QFileInfo::exists(QDir(runtime.path()).filePath(QStringLiteral("standard-2.0.0"))));

    // The selection a reader gets is still the previous one, and it still works.
    QVERIFY(QFile::setPermissions(pointerPath, writable));
    QByteArray pointer;
    QVERIFY(readFile(pointerPath, &pointer));
    QCOMPARE(QString::fromUtf8(pointer).trimmed(), QStringLiteral("standard-1.0.0"));
    QStringList rejections;
    const QList<Bundle> found = discoverBundles({runtime.path()}, &rejections);
    QCOMPARE(found.size(), 2);
    QCOMPARE(found.first().version, QStringLiteral("1.0.0"));
    QVERIFY2(validateBundle(found.first(), &reason), qPrintable(reason));
    QVERIFY(rejections.isEmpty());
}

void TestNativeBundle::aTruncatingBundleRootIsRefusedByName() {
    // A bundle root arrives from a stored pointer or a caller, and every
    // filesystem API below the reader takes a NUL-terminated C string. A name
    // carrying its own NUL would be truncated to a *different* directory that
    // may well exist and hold a bundle - so it has to be refused before the
    // reader asks the filesystem anything.
    QTemporaryDir temp;
    QVERIFY(QDir().mkpath(temp.path()));
    QVERIFY(writeFixture(temp.path(), standardFixture()));

    QString reason;
    const std::optional<Bundle> read =
        readBundle(temp.path() + QChar(0) + QStringLiteral("elsewhere"), &reason);
    QVERIFY2(!read.has_value(), "a truncating root must not resolve to a bundle");
    QVERIFY2(reason.contains(QStringLiteral("NUL")), qPrintable(reason));

    // The untruncated name still reads, so the refusal is about the name and not
    // about this fixture.
    QVERIFY2(readBundle(temp.path(), &reason).has_value(), qPrintable(reason));
}

void TestNativeBundle::aDeviceOrShareBundleRootIsRefusedByName() {
    QTemporaryDir temp;
    QString reason;
    // Both spell `\\`, and neither is a directory this application owns: one
    // resolves through the Win32 device/extended namespace with path parsing
    // switched off, the other through a network redirector. Both would make
    // "inside the bundle root" a claim about somewhere else entirely.
    for (const QString& root :
         {QStringLiteral("\\\\?\\C:\\Mikai\\runtime"), QStringLiteral("\\\\.\\PhysicalDrive0"),
          QStringLiteral("\\\\server\\share\\runtime")}) {
        const std::optional<Bundle> read = readBundle(root, &reason);
        QVERIFY2(!read.has_value(), qPrintable(root));
        QVERIFY2(!reason.isEmpty(), qPrintable(root));
    }
    // The device and share refusals each name which namespace was refused, so a
    // caller can tell a deliberate refusal from a missing directory.
    QVERIFY(!readBundle(QStringLiteral("\\\\server\\share\\runtime"), &reason).has_value());
    QVERIFY2(reason.contains(QStringLiteral("network")), qPrintable(reason));
    QVERIFY(!readBundle(QStringLiteral("\\\\?\\C:\\Mikai"), &reason).has_value());
    QVERIFY2(reason.contains(QStringLiteral("device")), qPrintable(reason));
}

void TestNativeBundle::aNonAsciiLocalBundleRootIsAccepted() {
    // The refusal set is not "non-ASCII". A local directory with a non-ASCII
    // name is an ordinary place to keep a runtime, and it has to read through
    // exactly as an ASCII one does.
    QTemporaryDir temp;
    const QString root = QDir(temp.path()).filePath(QString::fromUtf8("Нативный Рантайм"));
    QVERIFY(writeFixture(root, standardFixture(QStringLiteral("1.0.0-unicode"))));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(root, &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QCOMPARE(bundle->version, QStringLiteral("1.0.0-unicode"));
    QVERIFY2(validateBundle(*bundle, &reason), qPrintable(reason));
}

void TestNativeBundle::aColonCommaPercentLocalPathIsAccepted() {
    // These four are named together because they are exactly the characters that
    // a filter command, a log line or a naive quoting scheme mangles. None of
    // them is special to a Win32 *local path* - the colon only means something
    // as a drive separator or an alternate data stream, which the per-segment
    // rules in safeRelativePath already refuse.
    QTemporaryDir temp;
    const QString leaf = QString::fromUtf8("Пакет, 100% (v2)");
    const QString root = QDir(temp.path()).filePath(leaf);
    QVERIFY(writeFixture(root, standardFixture(QStringLiteral("1.0.0-punct"))));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(root, &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QVERIFY2(validateBundle(*bundle, &reason), qPrintable(reason));

    // The same characters inside a stored relative name, which the manifest
    // carries, resolve and validate rather than being refused.
    Fixture fixture = standardFixture(QStringLiteral("1.0.0-punct"));
    fixture.files.insert(leaf + QStringLiteral("/helper.dll"),
                         syntheticPe({QStringLiteral("kernel32.dll")}));
    fixture.deps = {leaf};
    const QString punct = QDir(temp.path()).filePath(QStringLiteral("punct"));
    QVERIFY(writeFixture(punct, fixture));
    const std::optional<Bundle> withPunct = readBundle(punct, &reason);
    QVERIFY2(withPunct.has_value(), qPrintable(reason));
    QVERIFY2(validateBundle(*withPunct, &reason), qPrintable(reason));
}

void TestNativeBundle::aMalformedUnicodePathIsRefusedNotReplaced() {
    // A lone surrogate has no UTF-8 encoding. Converting it would substitute
    // U+FFFD, and the result names a *different* file - one that may exist and
    // that the caller never asked about. Refusing is the only honest answer.
    QTemporaryDir temp;
    const QString orphanHigh(1, QChar(0xD800));
    const QString orphanLow(1, QChar(0xDC00));

    QString reason;
    for (const QString& bad : {temp.path() + QLatin1Char('/') + orphanHigh,
                               temp.path() + QLatin1Char('/') + orphanLow,
                               orphanHigh + temp.path()}) {
        const std::optional<Bundle> bundle = readBundle(bad, &reason);
        QVERIFY2(!bundle.has_value(), qPrintable(bad.toUtf8().toHex()));
        QVERIFY2(reason.contains(QStringLiteral("surrogate")), qPrintable(reason));
    }

    // A correctly paired surrogate is an ordinary character and must survive: it
    // is the pairing, not the code point, that the check is about.
    QString paired(1, QChar(0xD83D));  // U+1F600, as a surrogate pair
    paired.append(QChar(0xDE00));
    const QString root = QDir(temp.path()).filePath(paired);
    QVERIFY(writeFixture(root, standardFixture(QStringLiteral("1.0.0-emoji"))));
    QVERIFY2(readBundle(root, &reason).has_value(), qPrintable(reason));
}

void TestNativeBundle::anOverlongPathComponentIsRefusedAtItsExactLimit() {
    // The per-component limit is Win32's, and the boundary is exact: a component
    // of exactly the limit is a name the filesystem accepts, and one more is
    // not. Asserting both sides is what makes this a limit test rather than a
    // "long paths are refused" test.
    QString reason;
    const QString base = QStringLiteral("C:/models");
    const QString longest =
        base + QLatin1Char('/') + QString(kNativeComponentLimit, QLatin1Char('x'));
    const QString tooLong =
        base + QLatin1Char('/') + QString(kNativeComponentLimit + 1, QLatin1Char('x'));

    const NativePathTrust atLimit = nativePathTrust(longest, true);
    QVERIFY2(atLimit.trusted, qPrintable(atLimit.reason));

    const NativePathTrust over = nativePathTrust(tooLong, true);
    QVERIFY2(!over.trusted, "a component one unit over the limit must be refused");
    QVERIFY2(over.reason.contains(QString::number(kNativeComponentLimit)), qPrintable(over.reason));

    // The same total length split across two components is fine: the limit is
    // per component, so a path must not be refused for its length while every
    // part of it is individually nameable.
    const QString split = base + QLatin1Char('/') +
                          QString(kNativeComponentLimit, QLatin1Char('a')) + QLatin1Char('/') +
                          QString(kNativeComponentLimit, QLatin1Char('b'));
    const NativePathTrust parts = nativePathTrust(split, true);
    QVERIFY2(parts.trusted, qPrintable(parts.reason));
    QVERIFY2(parts.wideLength > kNativeComponentLimit,
             "the split path is longer than one component, which is the point");
}

void TestNativeBundle::aPathAtTheWideLimitIsAcceptedAndOneBeyondIsRefused() {
    // The wide Win32 APIs bound a path at 32767 UTF-16 code units, not at the 260
    // the narrow ones impose. Refusing anything past 260 would refuse a path the
    // wide loader opens without complaint, which is the blanket-length rule this
    // boundary exists to replace.
    //
    const int units = kNativeWidePathLimit - 1;  // Reserve the terminating NUL.
    const QString component = QString(253, QLatin1Char('x')) + QLatin1Char('/');
    QString atLimit = QStringLiteral("C:/");
    atLimit.reserve(units + 1);
    while (atLimit.size() + component.size() <= units) {
        atLimit += component;
    }
    atLimit += QString(units - atLimit.size(), QLatin1Char('y'));
    const NativePathTrust ok = nativePathTrust(atLimit, true);
    QVERIFY2(ok.trusted, qPrintable(ok.reason));

    const NativePathTrust over = nativePathTrust(atLimit + QLatin1Char('z'), true);
    QVERIFY2(!over.trusted, "a path one unit past the wide limit must be refused");
}

void TestNativeBundle::anUnrepresentableStoreRootIsRefusedAsUnowned() {
    // The artifact store is allowed to create files in and delete files from its
    // root. A root that names a truncation or a device namespace must therefore
    // never become a writable store, whatever the containment argument would say
    // about it - and that decision is observable through isOwned().
    ArtifactStore truncating(QDir(QDir::tempPath()).filePath(QStringLiteral("store")) + QChar(0) +
                             QStringLiteral("other"));
    QString reason;
    QVERIFY2(!truncating.isOwned(&reason), "a truncating store root must not be owned");
    QVERIFY2(reason.contains(QStringLiteral("NUL")), qPrintable(reason));

    for (const QString& root : {QStringLiteral("\\\\.\\PhysicalDrive0"),
                                QStringLiteral("\\\\server\\share\\artifacts")}) {
        ArtifactStore store(root);
        QVERIFY2(!store.isOwned(&reason), qPrintable(root));
        QVERIFY2(!reason.isEmpty(), qPrintable(root));
    }

    // A real temporary directory is still owned, so the refusal above is about
    // the name and not about every explicit root.
    QTemporaryDir temp;
    QVERIFY2(ArtifactStore(temp.path()).isOwned(&reason), qPrintable(reason));
}

void TestNativeBundle::aStoredRelativeNameWithMalformedUnicodeIsRefused() {
    // The manifest-declared file names take the same check as an absolute path:
    // a name that cannot be encoded is refused where it is read, not later where
    // it would be opened.
    QString reason;
    const QString orphan(1, QChar(0xD800));
    for (const QString& bad :
         {orphan + QStringLiteral(".onnx"), QStringLiteral("pack/") + orphan + QStringLiteral("/m.onnx"),
          QStringLiteral("pack") + QChar(0)}) {
        const QString safe = safeRelativePath(bad, &reason);
        QVERIFY2(safe.isEmpty(), qPrintable(bad.toUtf8().toHex()));
        QVERIFY2(!reason.isEmpty(), qPrintable(bad.toUtf8().toHex()));
    }

    // The same non-ASCII, comma and percent characters in a well-formed
    // relative name are ordinary and must be kept verbatim.
    const QString kept = QString::fromUtf8("Пакет, 100%/model.onnx");
    QCOMPARE(safeRelativePath(kept, &reason), kept);
}

void TestNativeBundle::theNativeLayoutResolvesUnderOneAppLocalHierarchy() {
    // One hierarchy, one base: every app-owned native directory is a name
    // beneath AppLocalDataLocation()/native, so nothing is written to a second
    // root that a user's roaming profile or a portable install would miss.
    const QString appLocal =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QVERIFY(!appLocal.isEmpty());
    const QString storage = nativeStorageRoot();
    QCOMPARE(storage, QDir::cleanPath(QDir(appLocal).filePath(QStringLiteral("native"))));
    QCOMPARE(runtimeStorageRoot(), QDir::cleanPath(QDir(storage).filePath(QStringLiteral("runtime"))));

    const QStringList names = nativeSubdirectoryNames();
    QCOMPARE(names.size(), 8);
    for (const QString& name : names) {
        const QString resolved = nativeSubdirectory(name);
        QCOMPARE(resolved, QDir::cleanPath(QDir(storage).filePath(name)));
        QVERIFY2(resolved.startsWith(storage + QLatin1Char('/')), qPrintable(resolved));
    }

    // A name outside the published list resolves to nothing, with a reason
    // naming the list it must come from, rather than to a directory the layout
    // never declared.
    QString reason;
    QVERIFY(nativeSubdirectory(QStringLiteral("bundles"), &reason).isEmpty());
    QVERIFY2(reason.contains(QStringLiteral("bundles")), qPrintable(reason));
    QVERIFY2(reason.contains(QStringLiteral("runtime")), qPrintable(reason));
}

void TestNativeBundle::aUnicodeAndMixedCaseRootInstallsAndDiscovers() {
    QTemporaryDir temp;
    const QString root = QDir(temp.path()).filePath(QStringLiteral("Нативний Bundle Runtime"));
    const QString source = QDir(temp.path()).filePath(QStringLiteral("Джерело З_bundle"));
    QVERIFY(QDir().mkpath(root));

    Fixture fixture = standardFixture(QStringLiteral("1.0.0-utf8"));
    fixture.deps = {QStringLiteral("trt")};
    // Distinct paths may contain identical bytes; this DLL also exercises Unicode inventory.
    fixture.files.insert(QStringLiteral("trt/Інференс_DLL.dll"),
                         syntheticPe({QStringLiteral("kernel32.dll")}));
    QVERIFY(writeFixture(source, fixture));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(source, &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QVERIFY2(validateBundle(*bundle, &reason), qPrintable(reason));
    QVERIFY2(installAtomic(*bundle, root, &reason), qPrintable(reason));

    QVERIFY(QFileInfo(QDir(root).filePath(QStringLiteral("standard-1.0.0-utf8"))).isDir());
    QStringList rejections;
    const QList<Bundle> found = discoverBundles({root}, &rejections);
    QCOMPARE(found.size(), 1);
    QVERIFY2(validateBundle(found.first(), &reason), qPrintable(reason));
    QCOMPARE(found.first().version, QStringLiteral("1.0.0-utf8"));
}

void TestNativeBundle::aDifferentlyCasedFileNameResolvesOnWindows() {
    QTemporaryDir temp;
    Fixture fixture = standardFixture();
    fixture.module = QStringLiteral("LIBMPV-2.DLL");
    fixture.files.insert(QStringLiteral("libmpv-2.dll"),
                         syntheticPe({QStringLiteral("kernel32.dll"),
                                      QStringLiteral("user32.dll")}));
    QVERIFY(writeFixture(temp.path(), fixture));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
#ifdef Q_OS_WIN
    // Windows compares paths case-insensitively, so the declared spelling
    // resolves and the bundle is valid.
    QVERIFY2(validateBundle(*bundle, &reason), qPrintable(reason));
#else
    QVERIFY(!validateBundle(*bundle, &reason));
#endif
}

void TestNativeBundle::aJunctionInsideTheBundleIsRefused() {
    QTemporaryDir temp;
    const QString outside = tempDir();
    QVERIFY(writeFile(QDir(outside).filePath(QStringLiteral("planted.dll")),
                      syntheticPe({QStringLiteral("kernel32.dll")})));
    const QString link = QDir(outside).filePath(QStringLiteral("linked"));
    if (!makeJunction(link, temp.path())) QSKIP("this environment will not create a junction");

    // A bundle root that is itself a junction is refused by name: reading it
    // would otherwise report a root the caller never asked for.
    Fixture fixture = standardFixture();
    QVERIFY(writeFixture(temp.path(), fixture));
    QString reason;
    const std::optional<Bundle> viaJunction = readBundle(link, &reason);
    QVERIFY(!viaJunction.has_value());
    QVERIFY2(reason.contains(QStringLiteral("reparse")), qPrintable(reason));

    // And a junction *inside* a bundle is refused too, so a manifest entry can
    // never be redirected outside the bundle it was verified against.
    Fixture planted = standardFixture(QStringLiteral("1.0.1"));
    // Distinct imports again: helper.dll's bytes would make readBundle refuse
    // the manifest on the shared digest before the junction could be the reason.
    planted.files.insert(QStringLiteral("trt/redirect.dll"),
                         syntheticPe({QStringLiteral("kernel32.dll"),
                                      QStringLiteral("advapi32.dll")}));
    QVERIFY(writeFixture(temp.path(), planted));
    const QString inner = QDir(temp.path()).filePath(QStringLiteral("trt/link"));
    QVERIFY(makeJunction(inner, outside));
    reason.clear();
    const std::optional<Bundle> redirected = readBundle(temp.path(), &reason);
    QVERIFY(redirected.has_value());
    QVERIFY2(!validateBundle(*redirected, &reason), qPrintable(reason));
    QVERIFY(reason.contains(QStringLiteral("reparse")));
}

void TestNativeBundle::theStockRuntimeIsDescribedFromItsActualBytesWithoutWriting() {
    const QString module = stockModulePath();
    if (module.isEmpty()) QSKIP("this machine has no stock runtime to describe");

    const QString root = QFileInfo(module).absolutePath();
    const QFileInfoList before =
        QDir(root).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    QByteArray moduleBefore;
    QVERIFY(readFile(module, &moduleBefore));

    QString reason;
    const std::optional<Bundle> bundle = describeStandardRuntime(module, &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));

    // Describing is reading: not one byte of the stock runtime changes.
    const QFileInfoList after =
        QDir(root).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    QCOMPARE(after.size(), before.size());
    QByteArray moduleAfter;
    QVERIFY(readFile(module, &moduleAfter));
    QCOMPARE(moduleAfter, moduleBefore);
    for (int i = 0; i < before.size(); ++i) QCOMPARE(after.at(i).fileName(), before.at(i).fileName());

    QCOMPARE(bundle->kind, BundleKind::Standard);
    QCOMPARE(bundle->root, QFileInfo(root).canonicalFilePath());
    QCOMPARE(bundle->modulePath, QFileInfo(module).canonicalFilePath());
    QCOMPARE(bundle->protocolVersion, 0);
    QVERIFY(bundle->version.startsWith(QStringLiteral("stock-")));
    QVERIFY(!bundle->files.isEmpty());

    // The inventory is measured, not declared: the core's own size and digest.
    bool sawModule = false;
    for (const BundleFile& file : bundle->files) {
        if (file.path.compare(QFileInfo(module).fileName(), Qt::CaseInsensitive) != 0) continue;
        sawModule = true;
        QCOMPARE(file.bytes, qint64(moduleBefore.size()));
        QCOMPARE(file.sha256, digestOf(moduleBefore));
    }
    QVERIFY(sawModule);

    // Identity is derived from the actual bytes, so it is stable across calls
    // and free of the path it happens to sit at.
    const std::optional<Bundle> again = describeStandardRuntime(module, &reason);
    QVERIFY2(again.has_value(), qPrintable(reason));
    QCOMPARE(again->descriptorId, bundle->descriptorId);
    QCOMPARE(again->version, bundle->version);
    QCOMPARE(again->files.size(), bundle->files.size());
}

void TestNativeBundle::aModuleImportingSomethingAbsentIsRefusedByName() {
    QTemporaryDir temp;
    // A core that imports something neither beside it nor part of Windows: the
    // shape of a runtime that would fail at load for a reason a user can read.
    const QString module = QDir(temp.path()).filePath(QStringLiteral("libmpv-2.dll"));
    QVERIFY(writeFile(module, syntheticPe({QStringLiteral("kernel32.dll"),
                                           QStringLiteral("torch.dll")})));

    QString reason;
    const std::optional<Bundle> bundle = describeStandardRuntime(module, &reason);
    QVERIFY(!bundle.has_value());
    QVERIFY2(reason.contains(QStringLiteral("torch.dll")), qPrintable(reason));

    // A DLL that is not one at all is refused with its own name too.
    const QString fake = QDir(temp.path()).filePath(QStringLiteral("other.dll"));
    QVERIFY(writeFile(fake, junk()));
    reason.clear();
    const std::optional<Bundle> fakeBundle = describeStandardRuntime(fake, &reason);
    QVERIFY(!fakeBundle.has_value());
    QVERIFY(reason.contains(QStringLiteral("other.dll")));
}

void TestNativeBundle::aPlaybackOnlyNativeBundleRemainsValidWithoutPreparation() {
    QTemporaryDir temp;
    QVERIFY(writeFixture(temp.path(), nativeFixture()));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    // No preparation block: the bundle is playback-only, and the absence is a
    // real absence rather than a defaulted one.
    QVERIFY(!bundle->preparation.has_value());
    QVERIFY2(validateBundle(*bundle, &reason), qPrintable(reason));
    QVERIFY(!bundle->preparation.has_value());
}

void TestNativeBundle::aCompletePreparationBlockNamesOwnedToolsAndVerifiedBackend() {
    QTemporaryDir temp;
    QVERIFY(writePreparedFixture(temp.path(), preparedNativeFixture()));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QVERIFY2(validateBundle(*bundle, &reason), qPrintable(reason));
    QVERIFY(bundle->preparation.has_value());

    const BundlePreparation& preparation = *bundle->preparation;
    const auto declared = [&bundle](const QString& path) {
        return std::any_of(bundle->files.cbegin(), bundle->files.cend(),
                           [&path](const BundleFile& file) {
                               return file.path.compare(path, Qt::CaseInsensitive) == 0;
                           });
    };
    const QString canonicalRoot = QFileInfo(bundle->root).canonicalFilePath();

    // Each tool is a declared inventory file that really is on disk inside the
    // bundle root, so the consumer gets an owned executable path and not a bare
    // string.
    const QStringList tools{preparation.tools.inspect, preparation.tools.prep,
                            preparation.tools.verifier};
    for (const QString& tool : tools) {
        QVERIFY2(!tool.isEmpty(), "a complete block names all three tools");
        QVERIFY2(declared(tool), qPrintable(tool));
        const QFileInfo info(QDir(bundle->root).filePath(tool));
        QVERIFY2(info.isFile(), qPrintable(tool));
        QVERIFY2(info.canonicalFilePath().startsWith(canonicalRoot + QLatin1Char('/')),
                 qPrintable(tool));
    }

    // The provenance is observed, not a placeholder: a full lower-case hex pin
    // and a positive ABI.
    QVERIFY(preparation.sourceAbi > 0);
    const bool fullHexPin =
        preparation.coreSha.size() == 40 &&
        std::all_of(preparation.coreSha.cbegin(), preparation.coreSha.cend(), [](QChar c) {
            return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                   (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
        });
    QVERIFY(fullHexPin);
    QVERIFY(preparation.coreSha != QString(40, QLatin1Char('0')));

    // Each declared backend type resolves to a library this bundle actually
    // ships: present in the verified inventory and on disk with the recorded
    // digest, so the path a consumer receives is a verified one.
    QVERIFY(!preparation.types.isEmpty());
    for (const QString& type : preparation.types) {
        const QString library = bundlePreparationBackendLibrary(type);
        QVERIFY2(!library.isEmpty(), qPrintable(type));
        const QFileInfo info(QDir(bundle->root).filePath(library));
        QVERIFY2(info.isFile(), qPrintable(library));
        QByteArray bytes;
        QVERIFY(readFile(info.absoluteFilePath(), &bytes));
        const QString digest = digestOf(bytes);
        bool recorded = false;
        for (const BundleFile& file : bundle->files) {
            if (file.path.compare(library, Qt::CaseInsensitive) == 0 && file.sha256 == digest)
                recorded = true;
        }
        QVERIFY2(recorded, qPrintable(library));
    }

    // The dispatcher that owns the source ABI travels in the inventory too.
    QVERIFY(declared(bundlePreparationProducerLibrary()));

    // The runtime directories are real directories inside the root, each holding
    // a declared file and declared as a dependency because it carries DLLs.
    const QStringList runtimeDirs{preparation.runtime.ort, preparation.runtime.trt,
                                  preparation.runtime.cuda};
    for (const QString& directory : runtimeDirs) {
        QVERIFY(QFileInfo(QDir(bundle->root).filePath(directory)).isDir());
        QVERIFY(bundle->deps.contains(directory, Qt::CaseInsensitive));
    }
}

void TestNativeBundle::aPreparationBlockMissingAnySubMemberIsRefused() {
    const QJsonObject complete = completePreparationBlock();
    const QStringList topLevel{QStringLiteral("types"), QStringLiteral("source"),
                               QStringLiteral("tools"), QStringLiteral("runtime")};
    const QStringList toolKeys{QStringLiteral("inspect"), QStringLiteral("prep"),
                               QStringLiteral("verifier")};
    const QStringList sourceKeys{QStringLiteral("coreSha"), QStringLiteral("sourceAbi")};
    const QStringList runtimeKeys{QStringLiteral("ort"), QStringLiteral("trt"),
                                  QStringLiteral("cuda")};

    // Start from a bundle that reads cleanly, so a refusal after a mutation can
    // only be the missing fact and not an earlier defect.
    QTemporaryDir probe;
    QVERIFY(writePreparedFixture(probe.path(), preparedNativeFixture()));
    QString probeReason;
    QVERIFY2(readBundle(probe.path(), &probeReason).has_value(), qPrintable(probeReason));

    const auto expectRefused = [](const QJsonObject& preparation, const QString& needle) {
        QTemporaryDir temp;
        if (!writePreparedFixture(temp.path(), preparedNativeFixture(), preparation)) return false;
        const QStringList before = entryNames(temp.path());
        QString reason;
        const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
        // A refused read writes nothing: the source directory is unchanged.
        if (entryNames(temp.path()) != before) return false;
        return !bundle.has_value() && reason.contains(needle);
    };

    for (const QString& member : topLevel) {
        QJsonObject preparation = complete;
        preparation.remove(member);
        QVERIFY2(expectRefused(preparation, member), qPrintable(member));
    }
    for (const QString& tool : toolKeys) {
        QJsonObject tools = complete.value(QStringLiteral("tools")).toObject();
        tools.remove(tool);
        QJsonObject preparation = complete;
        preparation.insert(QStringLiteral("tools"), tools);
        QVERIFY2(expectRefused(preparation, tool), qPrintable(tool));
    }
    for (const QString& key : sourceKeys) {
        QJsonObject source = complete.value(QStringLiteral("source")).toObject();
        source.remove(key);
        QJsonObject preparation = complete;
        preparation.insert(QStringLiteral("source"), source);
        QVERIFY2(expectRefused(preparation, key), qPrintable(key));
    }
    for (const QString& key : runtimeKeys) {
        QJsonObject runtime = complete.value(QStringLiteral("runtime")).toObject();
        runtime.remove(key);
        QJsonObject preparation = complete;
        preparation.insert(QStringLiteral("runtime"), runtime);
        QVERIFY2(expectRefused(preparation, key), qPrintable(key));
    }
}

void TestNativeBundle::aPlaceholderOrZeroProvenanceIsRefused() {
    const QJsonObject complete = completePreparationBlock();
    {
        QJsonObject source = complete.value(QStringLiteral("source")).toObject();
        source.insert(QStringLiteral("coreSha"), QString(40, QLatin1Char('0')));
        QJsonObject preparation = complete;
        preparation.insert(QStringLiteral("source"), source);
        QTemporaryDir temp;
        QVERIFY(writePreparedFixture(temp.path(), preparedNativeFixture(), preparation));
        QString reason;
        QVERIFY(!readBundle(temp.path(), &reason).has_value());
        QVERIFY2(reason.contains(QStringLiteral("placeholder")), qPrintable(reason));
    }
    {
        QJsonObject source = complete.value(QStringLiteral("source")).toObject();
        source.insert(QStringLiteral("sourceAbi"), 0);
        QJsonObject preparation = complete;
        preparation.insert(QStringLiteral("source"), source);
        QTemporaryDir temp;
        QVERIFY(writePreparedFixture(temp.path(), preparedNativeFixture(), preparation));
        QString reason;
        QVERIFY(!readBundle(temp.path(), &reason).has_value());
    }

    // The validator refuses an unmeasured provenance too, even on an otherwise
    // coherent bundle, so a value that bypassed the reader cannot be observed.
    QTemporaryDir temp;
    QVERIFY(writePreparedFixture(temp.path(), preparedNativeFixture()));
    QString reason;
    std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    bundle->preparation->coreSha = QString(40, QLatin1Char('0'));
    QVERIFY2(!validateBundle(*bundle, &reason), qPrintable(reason));
    bundle->preparation->coreSha = QString(40, QLatin1Char('a'));
    bundle->preparation->sourceAbi = 0;
    QVERIFY2(!validateBundle(*bundle, &reason), qPrintable(reason));
}

void TestNativeBundle::aStandardBundleCarryingPreparationIsRefused() {
    // A Standard bundle names no native toolchain, so a preparation block on it
    // is refused by the reader before it could describe a toolchain the bundle
    // does not own.
    QTemporaryDir temp;
    QVERIFY(writePreparedFixture(temp.path(), standardFixture(), completePreparationBlock()));
    QString reason;
    QVERIFY(!readBundle(temp.path(), &reason).has_value());

    // And the validator refuses the same contradiction on an already-parsed
    // bundle: kind is a checked fact, not a label the block can override.
    QTemporaryDir native;
    QVERIFY(writePreparedFixture(native.path(), preparedNativeFixture()));
    std::optional<Bundle> bundle = readBundle(native.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QVERIFY2(validateBundle(*bundle, &reason), qPrintable(reason));
    bundle->kind = BundleKind::Standard;
    QVERIFY2(!validateBundle(*bundle, &reason), qPrintable(reason));
}

void TestNativeBundle::anUnknownDuplicateOrEmptyBackendListIsRefused() {
    struct TypeCase {
        QJsonArray types;
        const char* label;
    };
    const QList<TypeCase> cases{
        {QJsonArray{QStringLiteral("cuda")}, "unknown-type"},
        {QJsonArray{bundlePreparationTypes().value(0), bundlePreparationTypes().value(0)},
         "duplicate-type"},
        {QJsonArray{}, "empty-list"},
        {QJsonArray{3}, "non-string-type"},
    };
    for (const TypeCase& typeCase : cases) {
        QJsonObject preparation = completePreparationBlock();
        preparation.insert(QStringLiteral("types"), typeCase.types);
        QTemporaryDir temp;
        QVERIFY(writePreparedFixture(temp.path(), preparedNativeFixture(), preparation));
        QString reason;
        QVERIFY2(!readBundle(temp.path(), &reason).has_value(), typeCase.label);
    }
}

void TestNativeBundle::anUnsafeToolPathOrOneOutsideTheInventoryIsRefused() {
    const QStringList cases{
        QStringLiteral("../evil.exe"),
        QDir::temp().filePath(QStringLiteral("evil.exe")),
        QStringLiteral("tools/elsewhere.exe"),
        QStringLiteral("tools/inspect.dll"),
    };
    for (const QString& toolPath : cases) {
        QJsonObject tools = completePreparationBlock().value(QStringLiteral("tools")).toObject();
        tools.insert(QStringLiteral("inspect"), toolPath);
        QJsonObject preparation = completePreparationBlock();
        preparation.insert(QStringLiteral("tools"), tools);
        QTemporaryDir temp;
        QVERIFY(writePreparedFixture(temp.path(), preparedNativeFixture(), preparation));
        QString reason;
        const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
        QVERIFY2(!bundle.has_value(), qPrintable(toolPath));
        // The refusal names either the unsafe path or the missing inventory
        // entry, never a silently accepted path.
        QVERIFY2(!reason.trimmed().isEmpty(), qPrintable(toolPath));
    }
}

void TestNativeBundle::aRuntimeDirectoryOutsideTheBundleIsRefused() {
    const QStringList keys{QStringLiteral("ort"), QStringLiteral("trt"), QStringLiteral("cuda")};
    const QStringList escaping{QStringLiteral("../escape"), QStringLiteral("nested/dir"),
                               QDir::temp().filePath(QStringLiteral("ort"))};
    for (const QString& key : keys) {
        for (const QString& value : escaping) {
            QJsonObject runtime =
                completePreparationBlock().value(QStringLiteral("runtime")).toObject();
            runtime.insert(key, value);
            QJsonObject preparation = completePreparationBlock();
            preparation.insert(QStringLiteral("runtime"), runtime);
            QTemporaryDir temp;
            QVERIFY(writePreparedFixture(temp.path(), preparedNativeFixture(), preparation));
            QString reason;
            QVERIFY2(!readBundle(temp.path(), &reason).has_value(), qPrintable(key + value));
            QVERIFY2(!reason.trimmed().isEmpty(), qPrintable(key + value));
        }
    }
}

void TestNativeBundle::aRuntimeDirectoryWithNoOwnedFileIsRefused() {
    // The directory itself exists and parses, so the refusal has to come from the
    // closure check: a provider runtime with no inventoried file behind it is not
    // somewhere the tools could resolve their own DLLs.
    const QStringList keys{QStringLiteral("ort"), QStringLiteral("trt"), QStringLiteral("cuda")};
    for (const QString& key : keys) {
        const QString emptyDir = QStringLiteral("blank-") + key;
        QTemporaryDir temp;
        QVERIFY(writePreparedFixture(temp.path(), preparedNativeFixture()));
        QVERIFY(QDir().mkpath(QDir(temp.path()).filePath(emptyDir)));
        QJsonObject runtime = completePreparationBlock().value(QStringLiteral("runtime")).toObject();
        runtime.insert(key, emptyDir);
        QJsonObject preparation = completePreparationBlock();
        preparation.insert(QStringLiteral("runtime"), runtime);
        QJsonObject manifest = manifestFor(preparedNativeFixture());
        manifest.insert(QStringLiteral("preparation"), preparation);
        QVERIFY(writeManifest(temp.path(), manifest));

        QString reason;
        const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
        QVERIFY2(bundle.has_value(), qPrintable(reason));
        QVERIFY2(!validateBundle(*bundle, &reason), qPrintable(reason));
        QVERIFY2(reason.contains(emptyDir), qPrintable(reason));
    }
}

void TestNativeBundle::aProducerOrBackendLibraryAbsentOrTamperedIsRefused() {
    const QString producer = bundlePreparationProducerLibrary();
    const QString backend = bundlePreparationBackendLibrary(bundlePreparationTypes().value(0));

    // Absent from the inventory: a block naming a producer or backend the bundle
    // does not declare is incoherent, whatever its own files say.
    {
        Fixture fixture = preparedNativeFixture();
        fixture.files.remove(producer);
        QTemporaryDir temp;
        QVERIFY(writePreparedFixture(temp.path(), fixture));
        QString reason;
        QVERIFY2(!readBundle(temp.path(), &reason).has_value(), qPrintable(producer));
        QVERIFY2(reason.contains(producer), qPrintable(reason));
    }
    {
        Fixture fixture = preparedNativeFixture();
        fixture.files.remove(backend);
        QTemporaryDir temp;
        QVERIFY(writePreparedFixture(temp.path(), fixture));
        QString reason;
        QVERIFY2(!readBundle(temp.path(), &reason).has_value(), qPrintable(backend));
        QVERIFY2(reason.contains(backend), qPrintable(reason));
    }

    // Present in the inventory but tampered on disk: only the digest can catch a
    // same-size change to the producer or the backend the block pins.
    for (const QString& library : {producer, backend}) {
        QTemporaryDir temp;
        QVERIFY(writePreparedFixture(temp.path(), preparedNativeFixture()));
        QByteArray tampered = syntheticPe({QStringLiteral("kernel32.dll")});
        tampered[tampered.size() / 2] = char(tampered.at(tampered.size() / 2) ^ 0xFF);
        QVERIFY(writeFile(QDir(temp.path()).filePath(library), tampered));

        QString reason;
        const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
        QVERIFY2(bundle.has_value(), qPrintable(reason));
        QVERIFY2(!validateBundle(*bundle, &reason), qPrintable(reason));
        QVERIFY2(reason.contains(library), qPrintable(reason));
    }
}

void TestNativeBundle::anAllowedPreparationSurvivesInstallIntoANewOwnedRoot() {
    QTemporaryDir source;
    QTemporaryDir runtime;
    QVERIFY(writePreparedFixture(source.path(), preparedNativeFixture()));

    QString reason;
    const std::optional<Bundle> bundle = readBundle(source.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QVERIFY2(validateBundle(*bundle, &reason), qPrintable(reason));

    QString publishedRoot;
    QVERIFY2(installAtomic(*bundle, runtime.path(), &reason, &publishedRoot), qPrintable(reason));
    QVERIFY(!publishedRoot.isEmpty());
    QVERIFY(publishedRoot.startsWith(QDir::cleanPath(runtime.path())));

    // The installed copy is a different root and the preparation block travelled
    // with it: the tools and runtime directories now resolve inside the owned
    // published directory, not back at the source.
    QStringList rejections;
    const QList<Bundle> found = discoverBundles({runtime.path()}, &rejections);
    QCOMPARE(found.size(), 1);
    QVERIFY(rejections.isEmpty());
    const Bundle& published = found.first();
    QCOMPARE(published.kind, BundleKind::Native);
    QVERIFY2(validateBundle(published, &reason), qPrintable(reason));
    QVERIFY(published.preparation.has_value());
    QVERIFY(QFileInfo(published.root).canonicalFilePath() !=
            QFileInfo(bundle->root).canonicalFilePath());

    const BundlePreparation& preparation = *published.preparation;
    const QStringList tools{preparation.tools.inspect, preparation.tools.prep,
                            preparation.tools.verifier};
    for (const QString& tool : tools)
        QVERIFY2(QFileInfo(QDir(published.root).filePath(tool)).isFile(), qPrintable(tool));
    const QStringList runtimeDirs{preparation.runtime.ort, preparation.runtime.trt,
                                  preparation.runtime.cuda};
    for (const QString& directory : runtimeDirs) {
        QVERIFY2(QFileInfo(QDir(published.root).filePath(directory)).isDir(),
                 qPrintable(directory));
        QVERIFY(published.deps.contains(directory, Qt::CaseInsensitive));
    }
    const QString backend = bundlePreparationBackendLibrary(preparation.types.value(0));
    QVERIFY2(QFileInfo(QDir(published.root).filePath(backend)).isFile(), qPrintable(backend));
}

void TestNativeBundle::noUnverifiedPreparationIsEverPresentedAsAvailable() {
    // The UI toolchain is observed only from a validated bundle's preparation
    // block, so any partial or false block must be refused by the reader and
    // never reach that consumer.
    QList<QJsonObject> falseBlocks;
    {
        QJsonObject block = completePreparationBlock();
        block.remove(QStringLiteral("tools"));
        falseBlocks.append(block);
    }
    {
        QJsonObject block = completePreparationBlock();
        block.insert(QStringLiteral("types"), QJsonArray{QStringLiteral("cuda")});
        falseBlocks.append(block);
    }
    {
        QJsonObject source = completePreparationBlock().value(QStringLiteral("source")).toObject();
        source.insert(QStringLiteral("coreSha"), QString(40, QLatin1Char('0')));
        QJsonObject block = completePreparationBlock();
        block.insert(QStringLiteral("source"), source);
        falseBlocks.append(block);
    }
    {
        QJsonObject source = completePreparationBlock().value(QStringLiteral("source")).toObject();
        source.insert(QStringLiteral("sourceAbi"), 0);
        QJsonObject block = completePreparationBlock();
        block.insert(QStringLiteral("source"), source);
        falseBlocks.append(block);
    }
    for (const QJsonObject& block : falseBlocks) {
        QTemporaryDir temp;
        QVERIFY(writePreparedFixture(temp.path(), preparedNativeFixture(), block));
        QString reason;
        QVERIFY2(!readBundle(temp.path(), &reason).has_value(),
                 qPrintable(QString(QJsonDocument(block).toJson(QJsonDocument::Compact))));
    }

    // A Standard bundle is not a consumer of the block at all: carrying one is
    // refused rather than read as a native toolchain.
    {
        QTemporaryDir temp;
        QVERIFY(writePreparedFixture(temp.path(), standardFixture(), completePreparationBlock()));
        QString reason;
        QVERIFY(!readBundle(temp.path(), &reason).has_value());
    }

    // The one block this build accepts is fully resolved, so a consumer that
    // trusts `preparation.has_value()` receives owned paths rather than strings.
    QTemporaryDir temp;
    QVERIFY(writePreparedFixture(temp.path(), preparedNativeFixture()));
    QString reason;
    const std::optional<Bundle> bundle = readBundle(temp.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QVERIFY(bundle->preparation.has_value());
    QVERIFY2(QFileInfo(QDir(bundle->root).filePath(bundle->preparation->tools.inspect)).isFile(),
             qPrintable(bundle->preparation->tools.inspect));
}
