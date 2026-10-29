#include "TestRuntimeSelection.hpp"

#include "MpvApi.hpp"
#include "native/bundle/NativeBundle.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"

#include <backend/Runtime.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QObject>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QVariant>

#include <optional>

using mplayer::native::BundleKind;
using mplayer::native::BundleLayout;
using mplayer::native::RuntimeChoice;
using mplayer::native::RuntimeSelection;

namespace {

bool writeFileBytes(const QString& path, const QByteArray& data);

/// The directory holding the first packaged candidate the selection would
/// walk, or nothing when this machine has none. The suite never downloads.
std::optional<QString> packagedRuntimeDir() {
    const QStringList candidates = mikai::backend::runtimeCandidates();
    if (candidates.isEmpty()) {
        return std::nullopt;
    }
    return QFileInfo(candidates.first()).absolutePath();
}

/// A DLL placed in the former per-user cache must not become a runtime
/// candidate when the packaged module is absent.
bool stockCacheIsExcluded() {
    QTemporaryDir temp;
    if (!temp.isValid()) return false;
    const QByteArray previous = qgetenv("LOCALAPPDATA");
    qputenv("LOCALAPPDATA", temp.path().toUtf8());

    const QString stockCache =
        QDir(temp.path()).filePath(QStringLiteral("MPVBackend/runtime"));
    const QString stockModule = QDir(stockCache).filePath(QStringLiteral("mpv-2.dll"));
    const bool written = writeFileBytes(stockModule, QByteArrayLiteral("stock runtime"));
    const QStringList candidates = mikai::backend::runtimeCandidates();

    if (previous.isNull()) {
        qunsetenv("LOCALAPPDATA");
    } else {
        qputenv("LOCALAPPDATA", previous);
    }
    if (!written) return false;
    const QString normalizedCache = QDir::cleanPath(stockCache) + QDir::separator();
    for (const QString& candidate : candidates) {
        if (QDir::cleanPath(candidate).startsWith(normalizedCache, Qt::CaseInsensitive)) {
            return false;
        }
    }
    return true;
}

bool nativeBundleInstalled() {
    QStringList rejections;
    for (const mplayer::native::Bundle& bundle : mplayer::native::discoverBundles({}, &rejections)) {
        if (bundle.kind == BundleKind::Native) {
            return true;
        }
    }
    return false;
}

/// "Not one byte changed" over a runtime tree: every file's relative path, size
/// and modification time, in a stable order.
QString treeFingerprint(const QString& root) {
    QStringList rows;
    const QDir base(root);
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo info(it.next());
        rows.append(QStringLiteral("%1|%2|%3")
                        .arg(base.relativeFilePath(info.absoluteFilePath()))
                        .arg(info.size())
                        .arg(info.lastModified().toMSecsSinceEpoch()));
    }
    rows.sort();
    return rows.join(QLatin1Char('\n'));
}

// A hermetic Native bundle fixture, so the resolve/commit handoff can be
// exercised without a machine that has a native runtime installed. The bytes
// are synthetic x64 PE images for the parser, not loadable libraries; this
// mirrors the fixture TestNativeBundle uses for the same reason.
constexpr quint64 kFixtureMpvApi = 131077;  // MPV_MAKE_VERSION(2, 5)
constexpr int kFixtureProtocol = 1;
constexpr int kFixtureEngineSchema = 1;

QByteArray syntheticPe(const QStringList& imports) {
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
    put16(0x84, 0x8664);         // x64
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
    put32(kOptional + 112 + 1 * 8, kImportRva);
    put32(kOptional + 112 + 13 * 8, 0);  // no delay imports

    qsizetype nameAt = 0x400 + (0x1100 - kImportRva);
    for (qsizetype i = 0; i < imports.size(); ++i) {
        const QByteArray latin = imports.at(i).toLatin1();
        for (qsizetype j = 0; j < latin.size(); ++j) image[nameAt + j] = latin.at(j);
        image[nameAt + latin.size()] = '\0';
        const quint32 rva = kImportRva + quint32(nameAt - 0x400);
        nameAt += latin.size() + 1;
        put32(0x400 + i * 20 + 12, rva);
    }
    put32(kOptional + 112 + 1 * 8 + 4, quint32((imports.size() + 1) * 20));
    return image;
}

QString digestOf(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool writeFileBytes(const QString& path, const QByteArray& data) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(data) == data.size();
}

/// A Native bundle whose closure spans two directories: the core imports a DLL
/// that lives under `trt/`, and the manifest declares it as a dependency.
bool writeSyntheticNativeBundle(const QString& root) {
    const QByteArray module =
        syntheticPe({QStringLiteral("kernel32.dll"), QStringLiteral("nvinfer_10.dll")});
    const QByteArray dependency = syntheticPe({QStringLiteral("kernel32.dll")});
    if (!writeFileBytes(QDir(root).filePath(QStringLiteral("libmpv-2.dll")), module)) return false;
    if (!writeFileBytes(QDir(root).filePath(QStringLiteral("trt/nvinfer_10.dll")), dependency)) {
        return false;
    }
    QJsonArray files;
    const auto record = [&files](const QString& path, const QByteArray& data) {
        files.append(QJsonObject{{QStringLiteral("path"), path},
                                 {QStringLiteral("bytes"), double(data.size())},
                                 {QStringLiteral("sha256"), digestOf(data)}});
    };
    record(QStringLiteral("libmpv-2.dll"), module);
    record(QStringLiteral("trt/nvinfer_10.dll"), dependency);
    const QJsonObject manifest{
        {QStringLiteral("schema"), QStringLiteral("mikai.native.bundle/1")},
        {QStringLiteral("kind"), QStringLiteral("native")},
        {QStringLiteral("version"), QStringLiteral("9.9.9")},
        {QStringLiteral("module"), QStringLiteral("libmpv-2.dll")},
        {QStringLiteral("mpvApiVersion"), double(kFixtureMpvApi)},
        {QStringLiteral("protocolVersion"), double(kFixtureProtocol)},
        {QStringLiteral("engineSchemaVersion"), double(kFixtureEngineSchema)},
        {QStringLiteral("deps"), QJsonArray::fromStringList(QStringList{QStringLiteral("trt")})},
        {QStringLiteral("files"), files}};
    return writeFileBytes(QDir(root).filePath(QStringLiteral("bundle.json")),
                          QJsonDocument(manifest).toJson(QJsonDocument::Indented));
}

/// The handoff a resolver is supposed to produce: the display fields plus the
/// whole validated bundle. `describe` is private to the implementation, so a
/// hermetic test states the selection the same way.
RuntimeSelection handoffFor(const mplayer::native::Bundle& bundle) {
    RuntimeSelection selection;
    selection.requested = RuntimeChoice::Native;
    selection.kind = bundle.kind;
    selection.root = bundle.root;
    selection.modulePath = bundle.modulePath;
    selection.descriptorId = bundle.descriptorId;
    selection.version = bundle.version;
    selection.resolvedBundle = bundle;
    return selection;
}

}  // namespace

void TestRuntimeSelection::anAbsentOrCorruptStoredChoiceReadsAsStandard() {
    QSettings settings;
    settings.beginGroup(mplayer::native::runtimeChoiceSettingsGroup());
    settings.remove(QString());
    settings.endGroup();
    settings.sync();
    QCOMPARE(mplayer::native::storedRuntimeChoice(), RuntimeChoice::Standard);

    // Only the exact word selects native, in any case and with surrounding
    // slack. Everything else -- standard, a near miss, an empty value -- is
    // Standard: the fallback is the answer when the preference is not.
    QCOMPARE(mplayer::native::runtimeChoiceFromString(QStringLiteral("native")),
             RuntimeChoice::Native);
    QCOMPARE(mplayer::native::runtimeChoiceFromString(QStringLiteral("Native")),
             RuntimeChoice::Native);
    QCOMPARE(mplayer::native::runtimeChoiceFromString(QStringLiteral("  NATIVE \t")),
             RuntimeChoice::Native);
    QCOMPARE(mplayer::native::runtimeChoiceFromString(QStringLiteral("standard")),
             RuntimeChoice::Standard);
    QCOMPARE(mplayer::native::runtimeChoiceFromString(QStringLiteral("natives")),
             RuntimeChoice::Standard);
    QCOMPARE(mplayer::native::runtimeChoiceFromString(QStringLiteral("native1")),
             RuntimeChoice::Standard);
    QCOMPARE(mplayer::native::runtimeChoiceFromString(QString()), RuntimeChoice::Standard);
    QCOMPARE(mplayer::native::runtimeChoiceFromString(QStringLiteral("nonsense")),
             RuntimeChoice::Standard);

    // The same through the settings file: a truncated, a mistyped and a
    // non-text value are all read back as Standard, never as a broken native.
    for (const QVariant& corrupt :
         {QVariant(QStringLiteral("nativ")), QVariant(QStringLiteral("true")), QVariant(1)}) {
        QSettings store;
        store.beginGroup(mplayer::native::runtimeChoiceSettingsGroup());
        store.setValue(mplayer::native::runtimeChoiceSettingsKey(), corrupt);
        store.endGroup();
        store.sync();
        QCOMPARE(mplayer::native::storedRuntimeChoice(), RuntimeChoice::Standard);
    }

    QCOMPARE(mplayer::native::runtimeChoiceToString(RuntimeChoice::Native),
             QStringLiteral("native"));
    QCOMPARE(mplayer::native::runtimeChoiceToString(RuntimeChoice::Standard),
             QStringLiteral("standard"));
}

void TestRuntimeSelection::theStoredChoiceRoundTripsThroughTheIsolatedSettings() {
    QString reason;
#ifndef Q_OS_LINUX
    QVERIFY2(mplayer::native::storeRuntimeChoice(RuntimeChoice::Native, &reason),
             qPrintable(reason));
    QCOMPARE(mplayer::native::storedRuntimeChoice(), RuntimeChoice::Native);
#endif
    QVERIFY2(mplayer::native::storeRuntimeChoice(RuntimeChoice::Standard, &reason),
             qPrintable(reason));
    QCOMPARE(mplayer::native::storedRuntimeChoice(), RuntimeChoice::Standard);

    // The choice lives in exactly one additive key under player/native, which
    // is the scope the native preference surfaces share.
    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("player/native/runtimeChoice")).toString(),
             QStringLiteral("standard"));
}

void TestRuntimeSelection::linuxRejectsPrivateNativeWithoutBreakingStandard() {
#ifdef Q_OS_LINUX
    QSettings settings;
    settings.beginGroup(mplayer::native::runtimeChoiceSettingsGroup());
    settings.setValue(mplayer::native::runtimeChoiceSettingsKey(), QStringLiteral("native"));
    settings.endGroup();
    settings.sync();

    QString reason;
    QCOMPARE(mplayer::native::storedRuntimeChoice(&reason), RuntimeChoice::Standard);
    QVERIFY2(reason.contains(QStringLiteral("Linux")), qPrintable(reason));
    QCOMPARE(settings.value(QStringLiteral("player/native/runtimeChoice")).toString(),
             QStringLiteral("native"));
    QVERIFY(!mplayer::native::storeRuntimeChoice(RuntimeChoice::Native, &reason));
    QVERIFY(reason.contains(QStringLiteral("Linux")));
    QCOMPARE(settings.value(QStringLiteral("player/native/runtimeChoice")).toString(),
             QStringLiteral("native"));

    const RuntimeSelection directStandard = mplayer::native::resolveRuntime(RuntimeChoice::Standard);
    const RuntimeSelection standard = mplayer::native::resolveStoredRuntime();
    QCOMPARE(standard.requested, RuntimeChoice::Standard);
    QCOMPARE(standard.kind, directStandard.kind);
    QCOMPARE(standard.modulePath, directStandard.modulePath);
    QVERIFY(standard.nativeNotApplied);
    QVERIFY(standard.runtimeUnavailable);
    QVERIFY(standard.reason.contains(reason));
    if (!directStandard.reason.isEmpty()) {
        QVERIFY(standard.reason.contains(directStandard.reason));
    }

    const RuntimeSelection explicitNative =
        mplayer::native::resolveRuntime(RuntimeChoice::Native);
    QVERIFY(!explicitNative.loaded);
    QVERIFY(!explicitNative.resolvedBundle.has_value());
    QVERIFY(explicitNative.runtimeUnavailable);
    QVERIFY(explicitNative.nativeNotApplied);
    QVERIFY(explicitNative.reason.contains(QStringLiteral("Linux")));

    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QVERIFY(writeSyntheticNativeBundle(temp.path()));
    const RuntimeSelection named = mplayer::native::resolveNamedBundle(temp.path());
    QVERIFY(!named.resolvedBundle.has_value());
    QVERIFY(named.nativeNotApplied);
    QVERIFY(named.reason.contains(QStringLiteral("Linux")));

    QString bundleReason;
    const auto bundle = mplayer::native::readBundle(temp.path(), &bundleReason);
    QVERIFY2(bundle.has_value(), qPrintable(bundleReason));
    const RuntimeSelection committed = mplayer::native::commitRuntime(handoffFor(*bundle));
    QVERIFY(!committed.loaded);
    QVERIFY(committed.nativeNotApplied);
    QVERIFY(committed.reason.contains(QStringLiteral("Linux")));
    const RuntimeSelection selected = mplayer::native::selectRuntime(RuntimeChoice::Native);
    QVERIFY(!selected.loaded);
    QVERIFY(selected.nativeNotApplied);
    QVERIFY(selected.reason.contains(QStringLiteral("Linux")));
    QVERIFY(!mplayer::native::runtimeSelected());

    QVERIFY(mplayer::native::storeRuntimeChoice(RuntimeChoice::Standard, &reason));
    QVERIFY(reason.isEmpty());
#else
    QSKIP("Linux platform policy");
#endif
}

void TestRuntimeSelection::recoveryIsOneAttemptAndNeverReloads() {
    if (mplayer::native::runtimeSelected()) {
        QSKIP("a module was already pinned by an earlier case");
    }
    QVERIFY(!mplayer::native::recoveryInFlight());
    QVERIFY(mplayer::native::beginRecovery());
    QVERIFY(mplayer::native::recoveryInFlight());
    // A second player asking at the same moment joins the one attempt instead
    // of starting another provisioning run.
    QVERIFY(!mplayer::native::beginRecovery());
    QVERIFY(mplayer::native::recoveryInFlight());
    mplayer::native::endRecovery();
    QVERIFY(!mplayer::native::recoveryInFlight());
    // And the latch is not one-shot: a failed attempt can be retried.
    QVERIFY(mplayer::native::beginRecovery());
    mplayer::native::endRecovery();
    QVERIFY(!mplayer::native::recoveryInFlight());
}

void TestRuntimeSelection::aNativeDescriptorIsNeverLoadedFromItsLabelAlone() {
    if (mplayer::native::runtimeSelected()) {
        QSKIP("a module was already pinned by an earlier case");
    }
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    // A selection that carries display fields but no validated bundle: the
    // label says Native, the file it names is not there, and nothing was
    // checked against bytes on disk.
    RuntimeSelection resolved;
    resolved.requested = RuntimeChoice::Native;
    resolved.kind = BundleKind::Native;
    resolved.root = QFileInfo(temp.path()).canonicalFilePath();
    resolved.modulePath = QDir(resolved.root).filePath(QStringLiteral("libmpv-2.dll"));
    resolved.descriptorId = QString(64, QLatin1Char('a'));
    resolved.version = QStringLiteral("9.9.9");

    const RuntimeSelection committed = mplayer::native::commitRuntime(resolved);
    // The label is not evidence. Nothing is loaded, the failure is named, and
    // no capability or API version is claimed for the module that never loaded.
    QCOMPARE(committed.kind, BundleKind::Native);
    QVERIFY(!committed.loaded);
    QVERIFY(committed.runtimeUnavailable);
    QVERIFY(!committed.reason.isEmpty());
    QCOMPARE(committed.apiVersion, quint64(0));
    QVERIFY(!mplayer::native::runtimeSelected());
}

void TestRuntimeSelection::aRefusedNativeRequestFallsBackToStandardAndNamesWhy() {
#ifdef Q_OS_LINUX
    QSKIP("Linux rejects explicit Native without a Standard fallback");
#endif
    QVERIFY2(stockCacheIsExcluded(),
             "a DLL in the per-user cache must not be a runtime candidate");
    if (mplayer::native::runtimeSelected()) {
        QSKIP("a module was already pinned by an earlier case");
    }
    if (nativeBundleInstalled()) {
        QSKIP("a native bundle is installed, so the refusal path is not the one under test");
    }
    if (!packagedRuntimeDir().has_value()) {
        QSKIP("no packaged mpv runtime is present");
    }

    const RuntimeSelection resolved = mplayer::native::resolveRuntime(RuntimeChoice::Native);
    // Resolving is not loading: the module is pinned only by commitRuntime, on
    // the GUI thread.
    QVERIFY(!resolved.loaded);
    QCOMPARE(resolved.requested, RuntimeChoice::Native);
    QVERIFY2(resolved.nativeNotApplied, qPrintable(resolved.reason));
    QVERIFY(resolved.runtimeUnavailable);
    QVERIFY(!resolved.reason.isEmpty());
    // The fallback is a real packaged Standard module, not a placeholder.
    QCOMPARE(resolved.kind, BundleKind::Standard);
    QVERIFY(!resolved.modulePath.isEmpty());
    QVERIFY(QFileInfo::exists(resolved.modulePath));
    QVERIFY(!mplayer::native::runtimeSelected());
}

void TestRuntimeSelection::aResolvedNativeHandoffCarriesItsDependencyClosureAndProtocol() {
    if (mplayer::native::runtimeSelected()) {
        QSKIP("a module was already pinned by an earlier case");
    }
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QVERIFY2(writeSyntheticNativeBundle(temp.path()), "the synthetic native bundle must write");

    QString reason;
    const std::optional<mplayer::native::Bundle> bundle =
        mplayer::native::readBundle(temp.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    const RuntimeSelection resolved = handoffFor(*bundle);

    // The handoff is not the lossy display view: the validated bundle travels
    // with it, dependency directory and protocol included.
    QVERIFY(resolved.resolvedBundle.has_value());
    QCOMPARE(resolved.resolvedBundle->deps, QStringList{QStringLiteral("trt")});
    QCOMPARE(resolved.resolvedBundle->protocolVersion, kFixtureProtocol);
    QCOMPARE(resolved.resolvedBundle->engineSchemaVersion, kFixtureEngineSchema);

    // The descriptor the loader would be handed carries both: the dependency
    // directory is absolute and real, and the protocol is unchanged. Rebuilding
    // a descriptor from the display fields alone would lose this.
    const mplayer::MpvBundle descriptor = resolved.resolvedBundle->toMpvBundleDescriptor();
    QCOMPARE(descriptor.protocolVersion, kFixtureProtocol);
    QCOMPARE(descriptor.deps.size(), 1);
    QCOMPARE(descriptor.deps.first(),
             QDir::cleanPath(QDir(bundle->root).filePath(QStringLiteral("trt"))));
    QVERIFY(QFileInfo(descriptor.deps.first()).isDir());
    QCOMPARE(descriptor.root, resolved.root);
    QCOMPARE(descriptor.modulePath, resolved.modulePath);

    // Resolution carries the bundle, it does not load: commit alone pins.
    QVERIFY(!resolved.loaded);
    QVERIFY(!mplayer::native::runtimeSelected());
}

void TestRuntimeSelection::aStaleOrForgedBundleIsRefusedBeforeTheLoader() {
    if (mplayer::native::runtimeSelected()) {
        QSKIP("a module was already pinned by an earlier case");
    }
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QVERIFY2(writeSyntheticNativeBundle(temp.path()), "the synthetic native bundle must write");

    QString reason;
    const std::optional<mplayer::native::Bundle> bundle =
        mplayer::native::readBundle(temp.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    const RuntimeSelection resolved = handoffFor(*bundle);

    // The bytes changed since resolve. Revalidation must refuse it by name
    // before the loader is asked, rather than loading a stale module; if commit
    // trusted only the display fields it would never notice.
    QVERIFY(writeFileBytes(QDir(temp.path()).filePath(QStringLiteral("libmpv-2.dll")),
                           QByteArrayLiteral("changed since the resolver read it")));
    const RuntimeSelection stale = mplayer::native::commitRuntime(resolved);
    QVERIFY(!stale.loaded);
    QVERIFY(stale.runtimeUnavailable);
    QVERIFY(!mplayer::native::runtimeSelected());

    // A file the resolver carried but that has since disappeared is refused
    // too: the closure is not dropped on the way to the loader.
    QVERIFY(writeSyntheticNativeBundle(temp.path()));
    QVERIFY(QDir(QDir(temp.path()).filePath(QStringLiteral("trt"))).removeRecursively());
    const RuntimeSelection missing = mplayer::native::commitRuntime(resolved);
    QVERIFY(!missing.loaded);
    QVERIFY(missing.runtimeUnavailable);
    QVERIFY(!mplayer::native::runtimeSelected());

    // A selection whose display path was diverted from its descriptor is
    // refused as forged, so a different module cannot be smuggled past it.
    RuntimeSelection forged = handoffFor(*bundle);
    forged.modulePath = QDir(temp.path()).filePath(QStringLiteral("elsewhere.dll"));
    const RuntimeSelection refused = mplayer::native::commitRuntime(forged);
    QVERIFY(!refused.loaded);
    QVERIFY(refused.runtimeUnavailable);
    QVERIFY(!mplayer::native::runtimeSelected());
}

void TestRuntimeSelection::forkIdentityRequiresTheValidatedModuleAndActualApi() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString modulePath = QDir(temp.path()).filePath(QStringLiteral("mpv-2.dll"));
    QVERIFY(writeFileBytes(modulePath, QByteArrayLiteral("module")));

    mplayer::native::Bundle bundle;
    bundle.kind = BundleKind::Standard;
    bundle.root = QDir(temp.path()).canonicalPath();
    bundle.modulePath = QFileInfo(modulePath).canonicalFilePath();
    bundle.files.append(mplayer::native::BundleFile{
        QStringLiteral("mpv-2.dll"), 122146816,
        QStringLiteral("90de8f89fa1421eaec510e5cfe11de75846b445489864220ec811fd0f08b4649")});

    RuntimeSelection selection;
    selection.loaded = true;
    selection.kind = BundleKind::Standard;
    selection.modulePath = bundle.modulePath;
    selection.apiVersion = 131077;
    selection.version = QStringLiteral("stock-this-display-label-is-arbitrary");
    selection.resolvedBundle = bundle;
    QVERIFY(mplayer::native::isAnimeJanaiForkRuntime(selection));

    selection.version = QStringLiteral("d6d93599d5-not-the-fork");
    QVERIFY(mplayer::native::isAnimeJanaiForkRuntime(selection));

    selection.resolvedBundle->files[0].sha256 = QString(64, QLatin1Char('0'));
    QVERIFY(!mplayer::native::isAnimeJanaiForkRuntime(selection));
    selection.resolvedBundle = bundle;
    selection.resolvedBundle->files[0].bytes -= 1;
    QVERIFY(!mplayer::native::isAnimeJanaiForkRuntime(selection));
    selection.resolvedBundle = bundle;
    selection.apiVersion = 131076;
    QVERIFY(!mplayer::native::isAnimeJanaiForkRuntime(selection));
    selection.apiVersion = 131077;
    selection.kind = BundleKind::Native;
    QVERIFY(!mplayer::native::isAnimeJanaiForkRuntime(selection));
    selection.kind = BundleKind::Standard;
    selection.resolvedBundle->kind = BundleKind::Native;
    QVERIFY(!mplayer::native::isAnimeJanaiForkRuntime(selection));
    selection.resolvedBundle = bundle;
    selection.modulePath = QStringLiteral("different-module.dll");
    QVERIFY(!mplayer::native::isAnimeJanaiForkRuntime(selection));
    selection.modulePath = bundle.modulePath;
    selection.resolvedBundle->files.clear();
    QVERIFY(!mplayer::native::isAnimeJanaiForkRuntime(selection));
    selection.resolvedBundle = bundle;
    selection.loaded = false;
    QVERIFY(!mplayer::native::isAnimeJanaiForkRuntime(selection));
    selection.loaded = true;
    selection.resolvedBundle.reset();
    QVERIFY(!mplayer::native::isAnimeJanaiForkRuntime(selection));
}

void TestRuntimeSelection::forkIdentityAcceptsTheBundledLinuxForkByItsFilter() {
#if !defined(Q_OS_UNIX)
    QSKIP("The Linux fork is identified by its filter name, not a Windows digest");
#else
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString modulePath = QDir(temp.path()).filePath(QStringLiteral("libmpv.so.2"));
    // A packaged Linux module is rewritten by patchelf, so its digest differs
    // between build hosts; the fork's own filter name is what identifies it.
    QVERIFY(writeFileBytes(modulePath, QByteArrayLiteral("ELF\0animejanai filter table\0")));

    mplayer::native::Bundle bundle;
    bundle.kind = BundleKind::Standard;
    bundle.root = QDir(temp.path()).canonicalPath();
    bundle.modulePath = QFileInfo(modulePath).canonicalFilePath();
    bundle.files.append(mplayer::native::BundleFile{
        QStringLiteral("libmpv.so.2"), 33, QString(64, QLatin1Char('a'))});

    RuntimeSelection selection;
    selection.loaded = true;
    selection.kind = BundleKind::Standard;
    selection.modulePath = bundle.modulePath;
    selection.apiVersion = 131077;
    selection.resolvedBundle = bundle;
    QVERIFY(mplayer::native::isAnimeJanaiForkRuntime(selection));

    QVERIFY(writeFileBytes(modulePath, QByteArrayLiteral("ELF\0stock mpv without the filter\0")));
    QVERIFY(!mplayer::native::isAnimeJanaiForkRuntime(selection));

    selection.apiVersion = 131076;
    QVERIFY(!mplayer::native::isAnimeJanaiForkRuntime(selection));
#endif
}

void TestRuntimeSelection::publishingAStockRuntimeNeverTouchesTheSourceOrTheReferenceInstall() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString source = QDir(temp.path()).filePath(QStringLiteral("stock"));
    const QString runtimeRoot = QDir(temp.path()).filePath(QStringLiteral("runtime"));
    // A flat stock runtime: one real x64 PE whose only import is a Windows
    // system module, so its closure is satisfied by the module's own directory.
    QVERIFY(writeFileBytes(QDir(source).filePath(QStringLiteral("libmpv-2.dll")),
                           syntheticPe({QStringLiteral("kernel32.dll")})));
    const QString sourceModule = QDir(source).filePath(QStringLiteral("libmpv-2.dll"));
    const QString sourceBefore = treeFingerprint(source);
    QVERIFY(!sourceBefore.isEmpty());

    QString reason;
    const std::optional<mplayer::native::Bundle> described =
        mplayer::native::describeStandardRuntime(sourceModule, &reason);
    QVERIFY2(described.has_value(), qPrintable(reason));
    QCOMPARE(described->kind, BundleKind::Standard);
    // A stock core's client API version is learned from the loaded module, so
    // the descriptor carries the declared "unknown until load" value.
    QCOMPARE(described->mpvApiVersion, quint64(0));

    // Describing is read-only. A stock runtime has no manifest, and looking at
    // it must never be what creates one.
    QVERIFY(!QFileInfo::exists(QDir(source).filePath(QStringLiteral("bundle.json"))));

    QString publishedRoot;
    QString publishReason;
    QVERIFY2(mplayer::native::installAtomic(*described, runtimeRoot, &publishReason, &publishedRoot),
             qPrintable(publishReason));
    QVERIFY(!publishedRoot.isEmpty());
    QVERIFY(QFileInfo(publishedRoot).isDir());
    QVERIFY(QDir::cleanPath(publishedRoot).startsWith(QDir::cleanPath(runtimeRoot),
                                                     Qt::CaseInsensitive));
    // The manifest is materialised from the inventory that was verified against
    // disk, and only inside what this call published.
    QVERIFY(QFileInfo::exists(QDir(publishedRoot).filePath(QStringLiteral("bundle.json"))));
    QVERIFY(!QFileInfo::exists(QDir(source).filePath(QStringLiteral("bundle.json"))));
    QCOMPARE(treeFingerprint(source), sourceBefore);

    // The published bundle is re-read from disk rather than trusted because it
    // was copied from a valid source, and the copy is what will be loaded.
    QString readReason;
    const std::optional<mplayer::native::Bundle> published =
        mplayer::native::readBundle(publishedRoot, &readReason);
    QVERIFY2(published.has_value(), qPrintable(readReason));
    QCOMPARE(QFileInfo(publishedRoot).fileName(),
             mplayer::native::bundleDirectoryName(BundleKind::Standard, published->version));
    QString validateReason;
    QVERIFY2(mplayer::native::validateBundle(*published, &validateReason), qPrintable(validateReason));
    QCOMPARE(published->modulePath, QDir(publishedRoot).filePath(QStringLiteral("libmpv-2.dll")));

    // The pointer is the one selected-bundle fact, and it names what was just
    // published.
    QFile pointer(QDir(runtimeRoot).filePath(
        mplayer::native::bundlePointerFileName(BundleKind::Standard)));
    QVERIFY(pointer.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(pointer.readAll()).trimmed(), QFileInfo(publishedRoot).fileName());
    pointer.close();

    // A published bundle is immutable, so a retry cannot rewrite what a load
    // may already be using, and it leaves no staging directory behind.
    QString secondReason;
    QVERIFY2(!mplayer::native::installAtomic(*described, runtimeRoot, &secondReason),
             "republishing the same inventory must be refused");
    QVERIFY(!QFileInfo::exists(
        QDir(runtimeRoot).filePath(QStringLiteral(".staging-") + QFileInfo(publishedRoot).fileName())));
    QCOMPARE(treeFingerprint(source), sourceBefore);

    // The published bundle is discoverable as what it is: a Standard bundle
    // under an app-owned root, with its module inside it.
    QStringList rejections;
    const QList<mplayer::native::Bundle> found = mplayer::native::discoverBundles({runtimeRoot},
                                                                                 &rejections);
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().kind, BundleKind::Standard);
    QCOMPARE(found.first().modulePath, published->modulePath);

    // The stock reference install is refused outright, so recovery can never
    // publish into it even when asked to.
    const QString stockRoot = QDir(temp.path()).filePath(QStringLiteral("MPVBackend/runtime"));
    QString stockReason;
    QVERIFY2(!mplayer::native::installAtomic(*described, stockRoot, &stockReason, nullptr),
             "installing into the stock tree must be refused");
    QVERIFY2(stockReason.contains(QStringLiteral("stock"), Qt::CaseInsensitive),
             qPrintable(stockReason));
    QVERIFY(!QFileInfo::exists(stockRoot));
    QCOMPARE(treeFingerprint(source), sourceBefore);
}

void TestRuntimeSelection::namedBundleResolutionIsExactAndHasNoFallback() {
    const RuntimeChoice storedBefore = mplayer::native::storedRuntimeChoice();
    const RuntimeSelection packaged = mplayer::native::resolveRuntime(RuntimeChoice::Standard);
    QVERIFY2(packaged.resolvedBundle.has_value(), qPrintable(packaged.reason));
    // A distribution libmpv is a host module, not a self-contained closure:
    // publishing it would claim a private copy the bundle does not carry. Only
    // a packaged directory layout is what this case is about.
    if (packaged.resolvedBundle->layout == BundleLayout::SystemModule) {
        QSKIP("the selected runtime is a host system module, which is not installable as a "
              "bundle");
    }

    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString runtimeRoot = QDir(temp.path()).filePath(QStringLiteral("runtime"));
    QString publishedRoot;
    QString reason;
    QVERIFY2(mplayer::native::installAtomic(*packaged.resolvedBundle, runtimeRoot, &reason,
                                           &publishedRoot),
             qPrintable(reason));
    QVERIFY(!publishedRoot.isEmpty());

    const RuntimeSelection named = mplayer::native::resolveNamedBundle(publishedRoot);
    QVERIFY2(named.resolvedBundle.has_value(), qPrintable(named.reason));
    QVERIFY(!named.loaded);
    QCOMPARE(named.kind, BundleKind::Standard);
    QCOMPARE(QDir::cleanPath(named.root), QDir::cleanPath(publishedRoot));
    QCOMPARE(QFileInfo(named.modulePath).canonicalFilePath(),
             QFileInfo(QDir(publishedRoot).filePath(
                           QFileInfo(packaged.resolvedBundle->modulePath).fileName()))
                 .canonicalFilePath());
    QVERIFY(!mplayer::native::runtimeSelected());
    QCOMPARE(mplayer::native::storedRuntimeChoice(), storedBefore);

    const QString emptyRoot = QDir(temp.path()).filePath(QStringLiteral("empty"));
    QVERIFY(QDir().mkpath(emptyRoot));
    const RuntimeSelection refused = mplayer::native::resolveNamedBundle(emptyRoot);
    QVERIFY(!refused.resolvedBundle.has_value());
    QVERIFY(refused.runtimeUnavailable);
    QVERIFY(!refused.reason.isEmpty());
    QVERIFY(!mplayer::native::runtimeSelected());
    QCOMPARE(mplayer::native::storedRuntimeChoice(), storedBefore);
}

void TestRuntimeSelection::selectingThePackagedForkLoadsWithoutMutation() {
    const QStringList candidates = mikai::backend::runtimeCandidates();
    const QString packagedRoot =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("mpv"));
    QStringList packagedCandidates;
    for (const QString& candidate : candidates) {
        if (QDir::cleanPath(candidate).startsWith(
                QDir::cleanPath(packagedRoot) + QDir::separator(), Qt::CaseInsensitive)) {
            packagedCandidates.append(candidate);
        }
    }
    if (!packagedCandidates.isEmpty()) {
        QCOMPARE(candidates.first(), packagedCandidates.first());
    }

    const std::optional<QString> packaged = packagedRuntimeDir();
    if (!packaged.has_value()) {
        QSKIP("no packaged mpv runtime is present");
    }
    const QString root = packaged.value();
    const QString treeBefore = treeFingerprint(root);
    QVERIFY(!treeBefore.isEmpty());
    const QByteArray pathBefore = qgetenv("PATH");
    const QStringList searchPathsBefore = QDir::searchPaths(QStringLiteral("mpv"));

    const RuntimeSelection selection = mplayer::native::selectStandardRuntime();
    QVERIFY2(selection.loaded, qPrintable(selection.reason));
    QCOMPARE(selection.kind, BundleKind::Standard);
    QCOMPARE(selection.requested, RuntimeChoice::Standard);
    QVERIFY(!selection.nativeNotApplied);
    QVERIFY(selection.apiVersion > 0);
    QVERIFY(QFileInfo::exists(selection.modulePath));
    QVERIFY(selection.modulePath.startsWith(selection.root, Qt::CaseInsensitive));
    QVERIFY(mplayer::native::runtimeSelected());
    QCOMPARE(mplayer::native::currentSelection().modulePath, selection.modulePath);

    // Selecting loads through the explicit loader: no PATH edit, no QDir
    // search-path pretence, and not one byte of the packaged tree written.
    QCOMPARE(qgetenv("PATH"), pathBefore);
    QCOMPARE(QDir::searchPaths(QStringLiteral("mpv")), searchPathsBefore);
    QCOMPARE(treeFingerprint(root), treeBefore);

    // Recovery must refuse once a module is pinned: that is what keeps a retry
    // from becoming a second load.
    QVERIFY(!mplayer::native::beginRecovery());
}

void TestRuntimeSelection::aDifferentChoiceNeverHotSwapsThePinnedModule() {
    if (!mplayer::native::runtimeSelected()) {
        QSKIP("no module was pinned, so there is nothing to swap away from");
    }
    const RuntimeSelection pinned = mplayer::native::currentSelection();
    QCOMPARE(pinned.kind, BundleKind::Standard);

    // The same choice is not a restart, and the module is not loaded a second
    // time.
    const RuntimeSelection again = mplayer::native::selectRuntime(RuntimeChoice::Standard);
    QVERIFY(again.loaded);
    QCOMPARE(again.kind, BundleKind::Standard);
    QVERIFY(!again.restartRequired);
    QCOMPARE(again.modulePath, pinned.modulePath);

    // A different one is, and it says so instead of pretending to swap.
    const RuntimeSelection native = mplayer::native::selectRuntime(RuntimeChoice::Native);
#ifdef Q_OS_LINUX
    QVERIFY(!native.restartRequired);
    QVERIFY(native.runtimeUnavailable);
    QVERIFY(native.nativeNotApplied);
    QVERIFY(native.reason.contains(QStringLiteral("Linux")));
#else
    QVERIFY(native.restartRequired);
#endif
    QVERIFY(!native.reason.isEmpty());
    QCOMPARE(native.requested, RuntimeChoice::Native);
    QVERIFY(native.loaded);
    QCOMPARE(native.kind, BundleKind::Standard);
    QCOMPARE(mplayer::native::currentSelection().modulePath, pinned.modulePath);

    // Even an explicit commit of a different descriptor reports the pinned
    // module back rather than replacing it under the live handles.
    RuntimeSelection alien;
    alien.kind = BundleKind::Native;
    alien.root = pinned.root;
    alien.modulePath = pinned.modulePath;
    alien.descriptorId = QStringLiteral("other");
    const RuntimeSelection refused = mplayer::native::commitRuntime(alien);
    QCOMPARE(refused.kind, BundleKind::Standard);
    QCOMPARE(refused.modulePath, pinned.modulePath);
#ifdef Q_OS_LINUX
    QVERIFY(!refused.restartRequired);
    QVERIFY(refused.runtimeUnavailable);
    QVERIFY(refused.nativeNotApplied);
    QVERIFY(refused.reason.contains(QStringLiteral("Linux")));
#else
    QVERIFY(refused.restartRequired);
#endif
    QCOMPARE(mplayer::native::currentSelection().kind, BundleKind::Standard);
    QCOMPARE(mplayer::native::currentSelection().modulePath, pinned.modulePath);
#ifndef Q_OS_LINUX
    QVERIFY(mplayer::native::currentSelection().restartRequired);
#endif
    const QString refusedStatus = mplayer::native::runtimeStatusText();
    QVERIFY2(refusedStatus.contains(refused.reason), qPrintable(refusedStatus));

    // A later request already satisfied by the pinned module clears request-only
    // state without changing the actual loaded identity.
    const RuntimeSelection satisfied = mplayer::native::selectRuntime(RuntimeChoice::Standard);
    QVERIFY(satisfied.loaded);
    QVERIFY(!satisfied.restartRequired);
    QVERIFY(!satisfied.nativeNotApplied);
    QVERIFY(!satisfied.runtimeUnavailable);
    QVERIFY(satisfied.reason.isEmpty());
    QCOMPARE(satisfied.modulePath, pinned.modulePath);
    QCOMPARE(mplayer::native::currentSelection().modulePath, pinned.modulePath);
    QCOMPARE(mplayer::native::currentSelection().requested, RuntimeChoice::Standard);
    QVERIFY(!mplayer::native::currentSelection().restartRequired);
    QVERIFY(!mplayer::native::currentSelection().nativeNotApplied);
    QVERIFY(!mplayer::native::currentSelection().runtimeUnavailable);
    QVERIFY(!mplayer::native::runtimeStatusText().contains(refused.reason));
}

void TestRuntimeSelection::theLoaderPinsExactlyTheValidatedClosure() {
    // M07's central claim: the loader receives the exact descriptor that was
    // validated, so nothing it checked is reconstructed or lost on the way in.
    // This is a synthetic PE fixture, so the module cannot actually load; what
    // is verified here is that the descriptor the loader would be handed is the
    // one the bundle's own validation produced, byte-for-byte in every field.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QVERIFY2(writeSyntheticNativeBundle(temp.path()), "the synthetic native bundle must write");

    QString reason;
    const std::optional<mplayer::native::Bundle> bundle =
        mplayer::native::readBundle(temp.path(), &reason);
    QVERIFY2(bundle.has_value(), qPrintable(reason));
    QVERIFY2(mplayer::native::validateBundle(*bundle, &reason), qPrintable(reason));

    const mplayer::MpvBundle descriptor = bundle->toMpvBundleDescriptor();
    // Every field is derived from the validated bundle, not from the display
    // projection: a caller rebuilding the descriptor from root/modulePath would
    // lose the protocol and the dependency directories.
    QCOMPARE(descriptor.root, bundle->root);
    QCOMPARE(descriptor.modulePath, bundle->modulePath);
    QCOMPARE(descriptor.descriptorId, bundle->descriptorId);
    QCOMPARE(descriptor.protocolVersion, kFixtureProtocol);
    // Dependency directories are absolute, canonical and inside the root, so
    // restricted search has a real directory to register for each one.
    QCOMPARE(descriptor.deps.size(), bundle->deps.size());
    for (int i = 0; i < descriptor.deps.size(); ++i) {
        const QString expected = QDir::cleanPath(QDir(bundle->root).filePath(bundle->deps.at(i)));
        QCOMPARE(descriptor.deps.at(i), expected);
        QVERIFY(QDir::isAbsolutePath(descriptor.deps.at(i)));
        QVERIFY(QFileInfo(descriptor.deps.at(i)).isDir());
    }
    // The descriptor identity is a stable digest of the manifest bytes, so two
    // reads of the same bundle produce the same descriptor.
    QVERIFY(descriptor.descriptorId.size() == 64);
    const std::optional<mplayer::native::Bundle> again =
        mplayer::native::readBundle(temp.path(), &reason);
    QVERIFY2(again.has_value(), qPrintable(reason));
    QCOMPARE(again->descriptorId, bundle->descriptorId);
    QCOMPARE(again->toMpvBundleDescriptor().descriptorId, descriptor.descriptorId);
}

void TestRuntimeSelection::anUnresolvedSelectionNeverReachesTheLoaderWithACapabilityClaim() {
    if (mplayer::native::runtimeSelected()) {
        QSKIP("a module was already pinned by an earlier case");
    }
    // A selection that names a native bundle whose module is absent: the loader
    // must refuse it, and the refusal must not carry any API version or
    // capability claim for a module that never loaded.
    RuntimeSelection resolved;
    resolved.requested = RuntimeChoice::Native;
    resolved.kind = BundleKind::Native;
    resolved.resolvedBundle.reset();
    const RuntimeSelection refused = mplayer::native::commitRuntime(resolved);
    QVERIFY(!refused.loaded);
    QVERIFY(refused.runtimeUnavailable);
    QVERIFY(!refused.reason.isEmpty());
    QCOMPARE(refused.apiVersion, quint64(0));
    QVERIFY(!mplayer::native::runtimeSelected());
}

/// The startup state a player sees while the cold hash is still running. It is
/// a truthful wait, never the missing-runtime failure, and the wait cannot be
/// reported as unavailable one turn before the module loads.
void TestRuntimeSelection::aPendingResolutionNamesTheWaitRatherThanAMissingRuntime() {
    QVERIFY(!mplayer::native::runtimeSelected());
    QVERIFY(!mplayer::native::runtimeResolutionPending());

    const mplayer::native::ResolutionLease lease = mplayer::native::beginResolution();
    QVERIFY2(lease, "no module is pinned and no resolution is running, so one may be taken");
    QVERIFY(mplayer::native::runtimeResolutionPending());

    // One resolution per process: a second owner joins nothing and starts nothing.
    QVERIFY(!mplayer::native::beginResolution());
    QVERIFY(mplayer::native::runtimeResolutionPending());

    // The pending line is not a refusal: it names the work in flight, and both
    // surfaces that would otherwise say "unavailable" say "pending" instead.
    const QString pending = mplayer::native::runtimePendingPlaybackText();
    QVERIFY2(!pending.isEmpty(), "a pending resolution names its wait");
    QVERIFY(!pending.contains(QStringLiteral("unavailable"), Qt::CaseInsensitive));
    const QString status = mplayer::native::runtimeStatusText();
    QVERIFY2(status == pending, qPrintable(status));

    const QString played = mplayer::native::runtimeUnavailablePlaybackText(QString());
    QVERIFY2(played.contains(pending), qPrintable(played));
    QVERIFY(!played.contains(QStringLiteral("unavailable"), Qt::CaseInsensitive));

    // A named reason still does not get reported as unavailable while the
    // resolution that would produce it has not published yet.
    const QString named = mplayer::native::runtimeUnavailablePlaybackText(QStringLiteral("x"));
    QVERIFY2(!named.contains(QStringLiteral("x")), qPrintable(named));

    mplayer::native::endResolution(lease);
    QVERIFY(!mplayer::native::runtimeResolutionPending());
    // With nothing in flight and nothing failed, the surface is the plain
    // unloaded state rather than a lie in either direction.
    QVERIFY(!mplayer::native::runtimeSelected());
    QVERIFY(!mplayer::native::runtimeUnavailablePlaybackText(QString())
                 .contains(QStringLiteral("pending"), Qt::CaseInsensitive));
}

/// A waiter is published exactly once, and only after the terminal state is on
/// record. A callback registered after everything settled is still queued, so
/// the caller that just asked is never re-entered by its own question.
void TestRuntimeSelection::aResolutionWaiterIsPublishedOnceAfterTheTerminalState() {
    int calls = 0;
    {
        // A destroyed context discards its waiter instead of delivering it.
        auto* context = new QObject();
        mplayer::native::whenRuntimeResolutionFinishes(context, [&calls] { ++calls; });
        delete context;
    }

    // A waiter whose context is alive. Registration is synchronous bookkeeping
    // only: the counter does not move until the event loop runs the delivery.
    auto* context = new QObject();
    auto* other = new QObject();
    int otherCalls = 0;
    bool settled = false;

    const mplayer::native::ResolutionLease lease = mplayer::native::beginResolution();
    QVERIFY(lease);
    mplayer::native::whenRuntimeResolutionFinishes(context, [&] {
        ++calls;
        // The waiter sees the terminal state, not the in-flight one.
        settled = !mplayer::native::runtimeResolutionPending();
    });
    mplayer::native::whenRuntimeResolutionFinishes(other, [&otherCalls] { ++otherCalls; });

    // Clearing the lease publishes the waiters, but they are still only queued.
    mplayer::native::endResolution(lease);
    QCOMPARE(calls, 0);
    QCOMPARE(otherCalls, 0);

    // Run the GUI event loop until both deliveries have landed.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCOMPARE(calls, 1);
    QCOMPARE(otherCalls, 1);
    QVERIFY2(settled, "a waiter sees the settled state, never the in-flight one");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCOMPARE(calls, 1);

    // A registration made after everything settled is queued once, not called
    // synchronously: the queue runs and the callback arrives exactly once.
    mplayer::native::whenRuntimeResolutionFinishes(context, [&calls] { ++calls; });
    QCOMPARE(calls, 1);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCOMPARE(calls, 2);

    // A destroyed context is discarded rather than delivered to a dead widget.
    mplayer::native::whenRuntimeResolutionFinishes(other, [&otherCalls] { ++otherCalls; });
    delete other;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCOMPARE(otherCalls, 1);

    delete context;
}

/// The shape a playback request actually takes while startup is still hashing:
/// its own resolution is refused, so it joins the one already running instead of
/// starting a second one, and it answers only once the terminal state is on
/// record. It releases nothing itself, which is what keeps a request that is
/// cancelled mid-wait from ending someone else's resolution.
void TestRuntimeSelection::aRefusedRequestJoinsTheRunningResolutionAndAnswersLate() {
    QVERIFY(!mplayer::native::runtimeSelected());

    auto* context = new QObject();
    bool answered = false;
    bool sawPending = false;

    // Startup owns the one resolution; a request arriving during it must be
    // refused, not given its own.
    const mplayer::native::ResolutionLease startup = mplayer::native::beginResolution();
    QVERIFY(startup);
    QVERIFY(!mplayer::native::beginResolution());

    // What the request does with that refusal: it waits on the running one.
    // Nothing may be pinned here, so the answer it sees is the settled absence
    // rather than a module; the point is when it arrives, not what it carries.
    mplayer::native::whenRuntimeResolutionFinishes(context, [&] {
        answered = true;
        sawPending = mplayer::native::runtimeResolutionPending();
    });

    // Still running, so the request has heard nothing and the process is not
    // holding a second resolution.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QVERIFY(!answered);
    QVERIFY(mplayer::native::runtimeResolutionPending());

    // The running resolution finishes; only then does the request answer, and
    // it sees the settled state rather than the in-flight one.
    mplayer::native::endResolution(startup);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QVERIFY2(answered, "a joined request answers after the running resolution settles");
    QVERIFY2(!sawPending, "the answer never observes the in-flight state");
    QVERIFY(!mplayer::native::runtimeResolutionPending());

    // The request released nothing of its own, so the next request can take the
    // one resolution rather than finding it latched forever.
    const mplayer::native::ResolutionLease next = mplayer::native::beginResolution();
    QVERIFY(next);
    mplayer::native::endResolution(next);
    QVERIFY(!mplayer::native::runtimeResolutionPending());

    delete context;
}

void TestRuntimeSelection::aResolutionWaiterSeesThePublishedRuntimeRefusalReason() {
    QVERIFY(!mplayer::native::runtimeSelected());
    const mplayer::native::ResolutionLease lease = mplayer::native::beginResolution();
    QVERIFY(lease);

    auto* context = new QObject();
    bool delivered = false;
    QString waiterReason;
    QString waiterStatus;
    mplayer::native::whenRuntimeResolutionFinishes(context, [&] {
        delivered = true;
        const RuntimeSelection terminal = mplayer::native::currentSelection();
        waiterReason = terminal.reason;
        waiterStatus = mplayer::native::runtimeStatusText();
    });

    RuntimeSelection unresolved;
    unresolved.requested = RuntimeChoice::Standard;
    unresolved.kind = BundleKind::Standard;
    unresolved.reason = QStringLiteral("probe refusal: packaged runtime unavailable");
    const RuntimeSelection refused = mplayer::native::commitRuntime(unresolved);
    QVERIFY(!refused.loaded);
    QVERIFY(refused.runtimeUnavailable);
    QCOMPARE(refused.reason, unresolved.reason);
    mplayer::native::endResolution(lease);

    QTRY_VERIFY(delivered);
    QCOMPARE(waiterReason, unresolved.reason);
    QVERIFY2(waiterStatus.contains(unresolved.reason), qPrintable(waiterStatus));
    const RuntimeSelection published = mplayer::native::currentSelection();
    QVERIFY(!published.loaded);
    QVERIFY(published.runtimeUnavailable);
    QCOMPARE(published.reason, unresolved.reason);

    delete context;
}

/// A lease from a superseded owner must not release the resolution that
/// replaced it. That is what keeps a cancelled recovery - one whose delivery
/// Runner suppressed, or whose owner was destroyed - from either locking every
/// later caller out or cutting short the resolution in its place.
void TestRuntimeSelection::aStaleLeaseNeverReleasesTheResolutionThatReplacedIt() {
    const mplayer::native::ResolutionLease first = mplayer::native::beginResolution();
    QVERIFY(first);
    // A later owner cannot take the one resolution while the first holds it.
    QVERIFY(!mplayer::native::beginResolution());

    // Ending the first lease publishes its waiters; the stale lease below is
    // then a no-op, and the resolution that replaced it is untouched.
    mplayer::native::endResolution(first);
    QVERIFY(!mplayer::native::runtimeResolutionPending());

    const mplayer::native::ResolutionLease second = mplayer::native::beginResolution();
    QVERIFY(second);
    mplayer::native::endResolution(first);
    QVERIFY2(mplayer::native::runtimeResolutionPending(),
             "a stale lease must not release the resolution that replaced it");
    mplayer::native::endResolution(second);
    QVERIFY(!mplayer::native::runtimeResolutionPending());

    // A module already pinned refuses a resolution outright, which is what
    // keeps a retry from becoming a second load.
    if (mplayer::native::runtimeSelected()) {
        QVERIFY(!mplayer::native::beginResolution());
    }

    // A lease whose owner went away can still be used to release: the lease is
    // the token, not the widget, so a suppressed recovery never latches the
    // one resolution forever.
    const mplayer::native::ResolutionLease orphan = mplayer::native::beginResolution();
    QVERIFY(orphan);
    mplayer::native::endResolution(orphan);
    QVERIFY(!mplayer::native::runtimeResolutionPending());
}

/// The loader preflight reads the module's own PE import table. A module that
/// declares no system-driver dependency is never asked about one, and a
/// synthetic module that declares the Vulkan loader is asked about exactly
/// that. No DLL is loaded and no dependency is copied.
void TestRuntimeSelection::aModuleSystemDriverImportIsReadFromItsOwnImage() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    // A module with only Windows system imports needs no supporting loader.
    const QString plain = QDir(temp.path()).filePath(QStringLiteral("plain/libmpv-2.dll"));
    QVERIFY(writeFileBytes(plain, syntheticPe({QStringLiteral("kernel32.dll")})));
    QVERIFY(mplayer::native::moduleSystemDriverImports(plain).isEmpty());
    QVERIFY(mplayer::native::runtimeLoaderDependencyRefusal(plain).isEmpty());

    // A module that statically imports the Vulkan loader is asked about it:
    // either it is refused by name, or the loader this machine supplies is
    // recorded. Never a silent load and never a copied DLL.
    const QString needsLoader = QDir(temp.path()).filePath(QStringLiteral("vulkan/libmpv-2.dll"));
    QVERIFY(writeFileBytes(needsLoader, syntheticPe({QStringLiteral("kernel32.dll"),
                                                    QStringLiteral("vulkan-1.dll")})));
    const QStringList required = mplayer::native::moduleSystemDriverImports(needsLoader);
    QVERIFY2(required.contains(QStringLiteral("vulkan-1.dll")),
             qPrintable(required.join(QLatin1Char(','))));
    const QString refusal = mplayer::native::runtimeLoaderDependencyRefusal(needsLoader);
    if (!refusal.isEmpty()) {
        // Refused: the reason names the dependency and the module, and it is a
        // refusal rather than a fallback.
        QVERIFY2(refusal.contains(QStringLiteral("vulkan"), Qt::CaseInsensitive),
                 qPrintable(refusal));
        QVERIFY2(refusal.contains(QStringLiteral("libmpv-2.dll")), qPrintable(refusal));
    }

    // A module that cannot be read at all is never guessed about: the loader's
    // own refusal speaks for that case, so this answers nothing.
    QVERIFY(mplayer::native::moduleSystemDriverImports(
                QDir(temp.path()).filePath(QStringLiteral("absent/libmpv-2.dll")))
                .isEmpty());
    QVERIFY(!mplayer::native::runtimeSelected());
}

void TestRuntimeSelection::theUnavailablePlaybackLineNamesTheSameReasonAsTheStatusLine() {
    if (mplayer::native::runtimeSelected()) {
        QSKIP("a module was already pinned by an earlier case");
    }
    // M08's user-facing contract: a runtime that could not be found produces
    // one clear line naming why, never a silent black player. The title page and
    // the standalone player share this one text, so the same failure reads the
    // same way wherever it surfaces.
    //
    // Deliberately hermetic: this checks the two text surfaces against each
    // other without resolving or loading a runtime.
    QVERIFY(!mplayer::native::runtimeSelected());
    QVERIFY(!mplayer::native::currentSelection().loaded);

    const QString status = mplayer::native::runtimeStatusText();
    QVERIFY2(!status.isEmpty(), "with nothing loaded the status line must still name a state");

    // An empty reason is not an excuse to say nothing: it falls back to the
    // status line, so the surface is never blank.
    const QString fromStatus = mplayer::native::runtimeUnavailablePlaybackText(QString());
    QVERIFY(!fromStatus.isEmpty());
    QVERIFY2(fromStatus.contains(status), qPrintable(fromStatus));

    // A named reason is carried verbatim, so the line a user reads contains the
    // same detail the log recorded rather than a generic apology.
    const QString named = QStringLiteral("no usable native runtime bundle: native-2.1.0");
    const QString fromReason = mplayer::native::runtimeUnavailablePlaybackText(named);
    QVERIFY2(fromReason.contains(named), qPrintable(fromReason));
    QVERIFY(fromReason != fromStatus);

    // The two surfaces stay consistent: given the status line as the reason, the
    // playback line names it, which is what makes one shared reason sufficient.
    const QString echoed = mplayer::native::runtimeUnavailablePlaybackText(status);
    QVERIFY2(echoed.contains(status), qPrintable(echoed));
}
