#include "TestRuntimeSelection.hpp"

#include "MpvApi.hpp"
#include "native/NativeBundle.hpp"
#include "native/NativeRuntimeSelection.hpp"

#include <backend/Runtime.hpp>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QVariant>

#include <optional>

using mplayer::native::BundleKind;
using mplayer::native::RuntimeChoice;
using mplayer::native::RuntimeSelection;

namespace {

/// The directory holding the stock runtime the selection would provision, or
/// nothing when this machine has none. The suite never downloads: a missing
/// runtime skips the live cases instead of starting a network fetch. The stock
/// discovery reports the entry it matched, which may be the module itself.
std::optional<QString> stockRuntimeDir() {
    const std::optional<QDir> found = mikai::backend::findRuntime();
    if (!found.has_value()) {
        return std::nullopt;
    }
    const QFileInfo entry(found->path());
    return QDir::cleanPath(entry.isFile() ? entry.absolutePath() : entry.absoluteFilePath());
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
    QVERIFY2(mplayer::native::storeRuntimeChoice(RuntimeChoice::Native, &reason),
             qPrintable(reason));
    QCOMPARE(mplayer::native::storedRuntimeChoice(), RuntimeChoice::Native);
    QVERIFY2(mplayer::native::storeRuntimeChoice(RuntimeChoice::Standard, &reason),
             qPrintable(reason));
    QCOMPARE(mplayer::native::storedRuntimeChoice(), RuntimeChoice::Standard);

    // The choice lives in exactly one additive key under player/native, which
    // is the scope the native preference surfaces share.
    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("player/native/runtimeChoice")).toString(),
             QStringLiteral("standard"));
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
    if (mplayer::native::runtimeSelected()) {
        QSKIP("a module was already pinned by an earlier case");
    }
    if (nativeBundleInstalled()) {
        QSKIP("a native bundle is installed, so the refusal path is not the one under test");
    }
    if (!stockRuntimeDir().has_value()) {
        QSKIP("no mpv runtime is present, and a test must not download one");
    }

    const RuntimeSelection resolved = mplayer::native::resolveRuntime(RuntimeChoice::Native);
    // Resolving is not loading: the module is pinned only by commitRuntime, on
    // the GUI thread.
    QVERIFY(!resolved.loaded);
    QCOMPARE(resolved.requested, RuntimeChoice::Native);
    QVERIFY2(resolved.nativeNotApplied, qPrintable(resolved.reason));
    QVERIFY(resolved.runtimeUnavailable);
    QVERIFY(!resolved.reason.isEmpty());
    // The fallback is the real stock runtime, not a placeholder descriptor.
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
    QVERIFY2(stale.reason.contains(QStringLiteral("libmpv-2.dll")), qPrintable(stale.reason));
    QVERIFY(!mplayer::native::runtimeSelected());

    // A file the resolver carried but that has since disappeared is refused
    // too: the closure is not dropped on the way to the loader.
    QVERIFY(writeSyntheticNativeBundle(temp.path()));
    QVERIFY(QDir(QDir(temp.path()).filePath(QStringLiteral("trt"))).removeRecursively());
    const RuntimeSelection missing = mplayer::native::commitRuntime(resolved);
    QVERIFY(!missing.loaded);
    QVERIFY2(missing.reason.contains(QStringLiteral("trt")), qPrintable(missing.reason));
    QVERIFY(!mplayer::native::runtimeSelected());

    // A selection whose display path was diverted from its descriptor is
    // refused as forged, so a different module cannot be smuggled past it.
    RuntimeSelection forged = handoffFor(*bundle);
    forged.modulePath = QDir(temp.path()).filePath(QStringLiteral("elsewhere.dll"));
    const RuntimeSelection refused = mplayer::native::commitRuntime(forged);
    QVERIFY(!refused.loaded);
    QVERIFY2(refused.reason.contains(QStringLiteral("does not match")), qPrintable(refused.reason));
    QVERIFY(!mplayer::native::runtimeSelected());
}

void TestRuntimeSelection::selectingStandardLoadsTheStockRuntimeWithoutTouchingIt() {
    const std::optional<QString> stock = stockRuntimeDir();
    if (!stock.has_value()) {
        QSKIP("no mpv runtime is present, and a test must not download one");
    }
    const QString root = stock.value();
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
    QVERIFY(!mplayer::native::runtimeStatusText().isEmpty());

    // The module the loader actually received is the resolved closure: the same
    // root, module and dependency directories, and the same protocol that was
    // validated, carried through resolve and commit without being rebuilt.
    QVERIFY(selection.resolvedBundle.has_value());
    const mplayer::MpvBundle loaded = mplayer::MpvApi::instance().bundle();
    QCOMPARE(loaded.root, selection.resolvedBundle->root);
    QCOMPARE(loaded.modulePath, selection.resolvedBundle->modulePath);
    QCOMPARE(loaded.descriptorId, selection.resolvedBundle->descriptorId);
    QCOMPARE(loaded.protocolVersion, selection.resolvedBundle->protocolVersion);
    QCOMPARE(loaded.deps, selection.resolvedBundle->toMpvBundleDescriptor().deps);

    // Selecting loads through the explicit loader: no PATH edit, no QDir
    // search-path pretence, and not one byte of the stock tree written.
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
    QVERIFY(native.restartRequired);
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
    QVERIFY(!refused.restartRequired);
}
