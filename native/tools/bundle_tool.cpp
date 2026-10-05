
#include "native/NativeBundle.hpp"
#include "native/NativePaths.hpp"
#include "native/NativePeImage.hpp"

// The real native producer ABI, taken from its own header so a signature drift
// is a compile error here rather than a reinterpret_cast that faults at the
// first call. Resolved through the relative path so the tool needs no extra
// include directory.
#include "../include/aji_native.h"

#include <type_traits>

#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtCore/QSet>
#include <QtCore/QTextStream>

#include <windows.h>

namespace {

using namespace mplayer;

/// The placeholder digest the checked-in layout examples carry. It is not a
/// SHA-256 of anything - no file hashes to sixty-four zeroes - so an example
/// can never be mistaken for a bundle whose bytes were measured, and `verify`
/// reports those records as unrecorded rather than as matching.
const QString kUnrecordedDigest(64, QLatin1Char('0'));

QString win32ErrorText(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString text;
    if (buffer != nullptr && length != 0)
        text = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
    if (buffer != nullptr) LocalFree(buffer);
    if (text.isEmpty()) text = QStringLiteral("unknown Windows error");
    return QStringLiteral("%1 (Win32 %2)").arg(text).arg(code);
}

/// The dependency directories of one bundle, registered for exactly as long as
/// this object lives. Identical flags to the loader, so a bundle that verifies
/// here is loadable by the same search the application will perform, and the
/// process PATH is never read or written.
class RestrictedSearch {
public:
    RestrictedSearch(const native::Bundle& bundle, QString* reason) {
        const QString rootPrefix = bundle.root + QLatin1Char('/');
        for (const QString& dep : bundle.deps) {
            const QString canonical =
                native::canonicalPath(QDir(bundle.root).filePath(dep));
            if (canonical.isEmpty() || !QDir(canonical).exists() ||
                !canonical.startsWith(rootPrefix, native::pathCaseSensitivity())) {
                if (reason != nullptr)
                    *reason = QStringLiteral("dependency directory '%1' is not a real directory "
                                             "inside the bundle root")
                                  .arg(dep);
                return;
            }
            const HANDLE cookie = AddDllDirectory(reinterpret_cast<LPCWSTR>(canonical.utf16()));
            if (cookie == nullptr) {
                if (reason != nullptr)
                    *reason = QStringLiteral("dependency directory '%1' could not be registered "
                                             "for the restricted search: %2")
                                  .arg(dep, win32ErrorText(GetLastError()));
                return;
            }
            cookies_.append(cookie);
        }
    }

    ~RestrictedSearch() {
        for (HANDLE cookie : cookies_) RemoveDllDirectory(cookie);
    }

    RestrictedSearch(const RestrictedSearch&) = delete;
    RestrictedSearch& operator=(const RestrictedSearch&) = delete;

private:
    QList<HANDLE> cookies_;
};

/// The module's own directory, the bundle's dependency directories and System32
/// - the one legitimate dependency outside the root. Nothing else, so a bundle
/// that needs an ambient Conda, torch or Python directory cannot pass here.
constexpr DWORD kRestrictedSearch = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                    LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                    LOAD_LIBRARY_SEARCH_USER_DIRS;

HMODULE loadRestricted(const QString& absolutePath, QString* reason) {
    const HMODULE module =
        LoadLibraryExW(reinterpret_cast<LPCWSTR>(absolutePath.utf16()), nullptr, kRestrictedSearch);
    if (module == nullptr) {
        if (reason != nullptr)
            *reason = QStringLiteral("%1 does not load under the restricted search: %2")
                          .arg(QFileInfo(absolutePath).fileName(), win32ErrorText(GetLastError()));
        return nullptr;
    }
    // DLL initializers can install process-global handlers that survive FreeLibrary.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(module), &pinned)) {
        if (reason != nullptr)
            *reason = QStringLiteral("could not pin %1: %2")
                          .arg(QFileInfo(absolutePath).fileName(), win32ErrorText(GetLastError()));
        return nullptr;
    }
    return module;
}

QStringList declaredDlls(const native::Bundle& bundle) {
    QStringList paths;
    for (const native::BundleFile& file : bundle.files) {
        if (file.path.endsWith(QLatin1String(".dll"), Qt::CaseInsensitive))
            paths.append(file.path);
    }
    paths.sort(Qt::CaseInsensitive);
    return paths;
}

// GGML keeps a process-global terminate handler; loading and unloading every helper is unsafe.
bool classifyInventoryDlls(const native::Bundle& bundle, QStringList* mpvModules, QString* reason) {
    mpvModules->clear();
    for (const QString& relative : declaredDlls(bundle)) {
        const QString absolute = native::canonicalPath(QDir(bundle.root).filePath(relative));
        QFile file(absolute);
        if (!file.open(QIODevice::ReadOnly)) {
            if (reason != nullptr)
                *reason = QStringLiteral("'%1' is declared but cannot be read: %2")
                              .arg(relative, file.errorString());
            return false;
        }
        uchar* mapping = file.map(0, file.size());
        if (mapping == nullptr) {
            if (reason != nullptr)
                *reason = QStringLiteral("'%1' cannot be mapped: %2").arg(relative, file.errorString());
            return false;
        }
        const auto unmap = qScopeGuard([&file, mapping] { file.unmap(mapping); });
        const QByteArray bytes = QByteArray::fromRawData(
            reinterpret_cast<const char*>(mapping), file.size());
        const native::PeExportInfo exp = native::inspectPeExports(bytes);
        if (!exp.ok) {
            if (reason != nullptr)
                *reason = QStringLiteral("'%1' has malformed export table: %2")
                              .arg(relative, exp.reason);
            return false;
        }
        if (exp.exportsMpvClientApiVersion) {
            mpvModules->append(relative);
        }
    }
    return true;
}

int loadDependencyOnly(const QString& root, const QString& relative, QTextStream& err) {
    QString reason;
    const auto bundle = native::readBundle(root, &reason);
    if (!bundle) {
        err << reason << '\n';
        return 2;
    }
    const native::BundleFile* declared = nullptr;
    for (const auto& file : bundle->files) {
        if (file.path == relative) {
            declared = &file;
            break;
        }
    }
    if (declared == nullptr || !relative.endsWith(QLatin1String(".dll"), Qt::CaseInsensitive)) {
        err << "not a declared DLL: " << relative << '\n';
        return 2;
    }
    QFile file(QDir(bundle->root).filePath(relative));
    if (!file.open(QIODevice::ReadOnly) || file.size() != declared->bytes) {
        err << "declared DLL cannot be read at its recorded size: " << relative << '\n';
        return 2;
    }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    if (!digest.addData(&file) ||
        QString::fromLatin1(digest.result().toHex()) != declared->sha256) {
        err << "declared DLL digest changed: " << relative << '\n';
        return 2;
    }
    const RestrictedSearch search(*bundle, &reason);
    if (!reason.isEmpty()) {
        err << reason << '\n';
        return 2;
    }
    const HMODULE module = loadRestricted(QFileInfo(file).absoluteFilePath(), &reason);
    if (module == nullptr) {
        err << reason << '\n';
        return 2;
    }
    FreeLibrary(module);
    return 0;
}

bool checkDependenciesInChildren(const native::Bundle& bundle, const QString& core,
                                QString* reason) {
    for (const QString& relative : declaredDlls(bundle)) {
        if (relative == core) continue;
        QProcess child;
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({QStringLiteral("__load-dll"), bundle.root, relative});
        child.start();
        if (!child.waitForStarted(10000)) {
            *reason = QStringLiteral("cannot start the isolated DLL check for '%1': %2")
                          .arg(relative, child.errorString());
            return false;
        }
        if (!child.waitForFinished(60000)) {
            child.kill();
            child.waitForFinished();
            *reason = QStringLiteral("the isolated DLL check for '%1' did not finish: %2")
                          .arg(relative, child.errorString());
            return false;
        }
        if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0) {
            *reason = QStringLiteral("the isolated DLL check for '%1' failed (exit %2): %3")
                          .arg(relative).arg(child.exitCode())
                          .arg(QString::fromLocal8Bit(child.readAllStandardError()).trimmed());
            return false;
        }
    }
    return true;
}

/// decltype'd from the mpv headers, so a signature drift is a compile error here
/// rather than a reinterpret_cast that faults at the first call.
struct ModuleProbe {
    decltype(&::mpv_create) create = nullptr;
    decltype(&::mpv_set_option_string) setOptionString = nullptr;
    decltype(&::mpv_terminate_destroy) terminateDestroy = nullptr;
    decltype(&::mpv_error_string) errorString = nullptr;
    decltype(&::mpv_client_api_version) clientApiVersion = nullptr;
};

bool bindModule(HMODULE module, ModuleProbe* probe, QString* reason) {
    const auto bind = [&](const char* name, auto& slot) {
        using Fn = std::remove_reference_t<decltype(slot)>;
        static_assert(std::is_pointer_v<Fn>);
        slot = reinterpret_cast<Fn>(::GetProcAddress(module, name));
    };
    bind("mpv_create", probe->create);
    bind("mpv_set_option_string", probe->setOptionString);
    bind("mpv_terminate_destroy", probe->terminateDestroy);
    bind("mpv_error_string", probe->errorString);
    bind("mpv_client_api_version", probe->clientApiVersion);
    const char* missing = nullptr;
    if (probe->create == nullptr) missing = "mpv_create";
    else if (probe->setOptionString == nullptr) missing = "mpv_set_option_string";
    else if (probe->terminateDestroy == nullptr) missing = "mpv_terminate_destroy";
    else if (probe->errorString == nullptr) missing = "mpv_error_string";
    else if (probe->clientApiVersion == nullptr) missing = "mpv_client_api_version";
    if (missing == nullptr) return true;
    if (reason != nullptr)
        *reason = QStringLiteral("the mpv module does not export %1")
                      .arg(QString::fromLatin1(missing));
    return false;
}

/// Loads the declared native producer - the dispatcher the filter loads - and
/// asks it for its real ABI: the same no-context v2 snapshot the ABI probe
/// reads. A producer that does not export the snapshot, speaks another protocol,
/// or reports an ABI other than the block's declared one is refused by name.
/// This is the one place `sourceAbi` is checked against the bytes rather than
/// trusted as metadata.
bool checkPreparationAbi(const native::Bundle& bundle, QString* reason) {
    const QString library = native::bundlePreparationProducerLibrary();
    const native::BundleFile* declared = nullptr;
    for (const native::BundleFile& file : bundle.files) {
        if (QFileInfo(file.path).fileName().compare(library, native::pathCaseSensitivity()) == 0) {
            declared = &file;
            break;
        }
    }
    if (declared == nullptr) {
        if (reason != nullptr)
            *reason = QStringLiteral("the preparation block names no producer: the bundle ships no "
                                     "'%1'")
                          .arg(library);
        return false;
    }
    const QString absolute = native::canonicalPath(QDir(bundle.root).filePath(declared->path));
    QString loadReason;
    const HMODULE module = loadRestricted(absolute, &loadReason);
    if (module == nullptr) {
        if (reason != nullptr) *reason = loadReason;
        return false;
    }
    const auto getSnapshot = reinterpret_cast<decltype(&::aji_native_get_snapshot)>(
        ::GetProcAddress(module, "aji_native_get_snapshot"));
    if (getSnapshot == nullptr) {
        if (reason != nullptr)
            *reason = QStringLiteral("the declared native producer '%1' does not export "
                                     "aji_native_get_snapshot, so its ABI cannot be checked")
                          .arg(library);
        return false;
    }
    aji_native_snapshot_v2 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    const int rc = getSnapshot(nullptr, AJI_NATIVE_PROTOCOL_VERSION, &snapshot);
    if (snapshot.struct_size != sizeof(snapshot) ||
        snapshot.protocol_version != AJI_NATIVE_PROTOCOL_VERSION ||
        snapshot.schema_version != AJI_NATIVE_SCHEMA_VERSION || rc != AJI_NATIVE_NO_CONTEXT) {
        if (reason != nullptr)
            *reason = QStringLiteral("the declared native producer '%1' does not answer the v2 "
                                     "no-context snapshot")
                          .arg(library);
        return false;
    }
    if (snapshot.aji_abi_version != quint32(bundle.preparation->sourceAbi)) {
        if (reason != nullptr)
            *reason = QStringLiteral("the declared native producer '%1' reports aji ABI %2, the "
                                     "preparation block declares %3")
                          .arg(library)
                          .arg(snapshot.aji_abi_version)
                          .arg(bundle.preparation->sourceAbi);
        return false;
    }
    return true;
}

/// A native bundle has to be able to register mikai_native, and the cheapest
/// proof of that asks the core and nothing else: ask it to take the filter by
/// name. A core without the filter refuses the option, which is exactly the
/// incoherent case - the manifest says `native` and the core would play on.
/// The handle is created, asked one question and destroyed uninitialized, so no
/// window, no video and no audio ever exist.
bool coreRegistersNativeFilter(const ModuleProbe& probe, QString* reason) {
    mpv_handle* handle = probe.create();
    if (handle == nullptr) {
        if (reason != nullptr)
            *reason = QStringLiteral("the module would not create an mpv handle");
        return false;
    }
    // config=no keeps a developer's own mpv.conf out of the answer.
    probe.setOptionString(handle, "config", "no");
    const int rc = probe.setOptionString(handle, "vf", "@mikai_native:animejanai");
    QString failure;
    if (rc < 0) {
        const char* text = probe.errorString(rc);
        failure = text != nullptr ? QString::fromUtf8(text).trimmed()
                                  : QStringLiteral("unknown error");
    }
    probe.terminateDestroy(handle);
    if (failure.isEmpty()) return true;
    if (reason != nullptr)
        *reason = QStringLiteral("the core refuses @mikai_native (%1): a bundle labelled native "
                                 "whose core cannot register the filter is not coherent")
                      .arg(failure);
    return false;
}

void describeBundle(QTextStream& out, const native::Bundle& bundle) {
    out << "  kind             " << native::bundleKindName(bundle.kind) << '\n'
        << "  version          " << bundle.version << '\n'
        << "  descriptorId     " << bundle.descriptorId << '\n'
        << "  root             " << bundle.root << '\n'
        << "  module           " << bundle.modulePath << '\n'
        << "  files            " << bundle.files.size() << '\n'
        << "  mpv client API   "
        << (bundle.mpvApiVersion == 0 ? QStringLiteral("learned at load")
                                      : QString::number(bundle.mpvApiVersion))
        << '\n'
        << "  protocolVersion  " << bundle.protocolVersion << '\n'
        << "  engineSchema     " << bundle.engineSchemaVersion << '\n';
    for (const QString& dep : bundle.deps) out << "  dep              " << dep << '\n';
    if (!bundle.preparation.has_value()) {
        out << "  preparation      none (playback-only; preparation unavailable)\n";
        return;
    }
    const native::BundlePreparation& preparation = *bundle.preparation;
    out << "  preparation      " << preparation.types.join(QStringLiteral(", ")) << '\n'
        << "  source coreSha   " << preparation.coreSha << '\n'
        << "  source ABI       " << preparation.sourceAbi << '\n'
        << "  tool inspect     " << preparation.tools.inspect << '\n'
        << "  tool prep        " << preparation.tools.prep << '\n'
        << "  tool verifier    " << preparation.tools.verifier << '\n'
        << "  runtime ort      " << preparation.runtime.ort << '\n'
        << "  runtime trt      " << preparation.runtime.trt << '\n'
        << "  runtime cuda     " << preparation.runtime.cuda << '\n';
}

int verify(const QString& path, QTextStream& out, QTextStream& err) {
    const auto reject = [&err, &path](const QString& reason) {
        err << "rejected: " << path << ": " << reason << '\n';
        return 2;
    };

    QString reason;
    const std::optional<native::Bundle> bundle = native::readBundle(path, &reason);
    if (!bundle.has_value()) return reject(reason);
    if (!native::validateBundle(*bundle, &reason)) return reject(reason);

    // Byte-level coherence is established; from here the bundle is loaded the
    // way the application will load it.
    QString searchReason;
    const RestrictedSearch search(*bundle, &searchReason);
    if (!searchReason.isEmpty()) return reject(searchReason);

    QStringList mpvModules;
    if (!classifyInventoryDlls(*bundle, &mpvModules, &reason)) return reject(reason);
    if (mpvModules.size() != 1)
        return reject(QStringLiteral("a bundle carries exactly one mpv module; this one carries %1")
                          .arg(mpvModules.isEmpty() ? QStringLiteral("none")
                                                   : mpvModules.join(QStringLiteral(", "))));
    if (mpvModules.first().compare(QDir(bundle->root).relativeFilePath(bundle->modulePath),
                                   native::pathCaseSensitivity()) != 0)
        return reject(QStringLiteral("the manifest names '%1' but the mpv module in this bundle is "
                                     "'%2'")
                          .arg(QDir(bundle->root).relativeFilePath(bundle->modulePath), mpvModules.first()));

    if (!checkDependenciesInChildren(*bundle, mpvModules.first(), &reason)) return reject(reason);
    const QString selectedModuleAbs = bundle->modulePath;
    QString moduleReason;
    HMODULE module = loadRestricted(selectedModuleAbs, &moduleReason);
    if (module == nullptr) return reject(moduleReason);
    ModuleProbe probe;
    const bool bound = bindModule(module, &probe, &reason);
    const quint64 reported = bound ? static_cast<quint64>(probe.clientApiVersion()) : 0;
    bool coherent = bound;
    // A declared version this build can drive is the manifest's claim; a
    // mismatch means the bytes are not the bundle the manifest describes.
    if (bound && bundle->mpvApiVersion != 0 && reported != bundle->mpvApiVersion) {
        reason = QStringLiteral("the module reports mpv client API %1, the manifest declares %2")
                     .arg(reported)
                     .arg(bundle->mpvApiVersion);
        coherent = false;
    }
    if (coherent && bundle->kind == native::BundleKind::Native) {
        coherent = coreRegistersNativeFilter(probe, &reason);
        // The block's sourceAbi is checked against the actual producer, not
        // trusted as metadata: the dispatcher is loaded and asked for its real
        // ABI.
        if (coherent && bundle->preparation.has_value())
            coherent = checkPreparationAbi(*bundle, &reason);
    }
    FreeLibrary(module);
    if (!coherent) return reject(reason);

    out << "accepted: " << path << '\n';
    describeBundle(out, *bundle);
    out << "  inventory        " << declaredDlls(*bundle).size()
        << " DLL(s), verified hashes and x64 eager/delay import closure; static export classification\n"
        << "  dependency loads " << declaredDlls(*bundle).size() - 1
        << " DLL(s), each checked in a fresh restricted-search child\n"
        << "  mpv module       1 (" << mpvModules.first() << "), client API " << reported << '\n'
        << "  native filter    "
        << (bundle->kind == native::BundleKind::Native
                ? QStringLiteral("core accepts @mikai_native")
                : QStringLiteral("not required for a standard bundle"))
        << '\n'
        << "  preparation ABI  "
        << (bundle->preparation.has_value()
                ? QStringLiteral("native producer reports aji ABI %1 for %2")
                      .arg(bundle->preparation->sourceAbi)
                      .arg(bundle->preparation->types.join(QStringLiteral(", ")))
                : QStringLiteral("none declared; playback-only bundle"))
        << '\n';
    return 0;
}

/// The layout a manifest declares, without the payload it names. The checked-in
/// examples sit next to no vendored binary by design - nothing is redistributed
/// here - so this is how their layout is read, and it is deliberately not a
/// weaker `verify`: every check that needs bytes needs bytes.
int manifest(const QString& manifestPath, QTextStream& out, QTextStream& err) {
    const auto reject = [&err, &manifestPath](const QString& reason) {
        err << "rejected: " << manifestPath << ": " << reason << '\n';
        return 2;
    };

    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly))
        return reject(QStringLiteral("cannot be read: %1").arg(file.errorString()));
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return reject(QStringLiteral("not a valid JSON object: %1").arg(parseError.errorString()));
    const QJsonObject object = document.object();

    QString schema;
    if (!object.value(QStringLiteral("schema")).isString())
        return reject(QStringLiteral("no string 'schema'"));
    schema = object.value(QStringLiteral("schema")).toString();
    if (schema != native::bundleSchemaId())
        return reject(QStringLiteral("manifest schema '%1' is not supported; this build reads '%2'")
                          .arg(schema, native::bundleSchemaId()));

    const QString kind = object.value(QStringLiteral("kind")).toString();
    if (kind != QLatin1String("standard") && kind != QLatin1String("native"))
        return reject(QStringLiteral("kind '%1' is neither 'standard' nor 'native'").arg(kind));
    const QString version = object.value(QStringLiteral("version")).toString();
    static const QRegularExpression versionPattern(
        QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z][0-9A-Za-z.-]*)?$"));
    if (!versionPattern.match(version).hasMatch())
        return reject(QStringLiteral("version '%1' is not MAJOR.MINOR.PATCH[-label]").arg(version));
    const QString module = object.value(QStringLiteral("module")).toString();
    QString pathReason;
    if (native::safeRelativePath(module, &pathReason).isEmpty())
        return reject(QStringLiteral("module %1").arg(pathReason));

    out << "layout: " << manifestPath << '\n'
        << "  schema           " << schema << '\n'
        << "  kind             " << kind << '\n'
        << "  version          " << version << '\n'
        << "  module           " << module << '\n'
        << "  mpvApiVersion    " << object.value(QStringLiteral("mpvApiVersion")).toInt() << '\n'
        << "  protocolVersion  " << object.value(QStringLiteral("protocolVersion")).toInt() << '\n'
        << "  engineSchema     " << object.value(QStringLiteral("engineSchemaVersion")).toInt() << '\n';

    QStringList deps;
    for (const QJsonValue& value : object.value(QStringLiteral("deps")).toArray())
        deps.append(value.toString());
    for (const QString& dep : deps) {
        QString depReason;
        const QString normalized = native::safeRelativePath(dep, &depReason);
        if (normalized.isEmpty()) return reject(QStringLiteral("dependency %1").arg(depReason));
        if (normalized.contains(QLatin1Char('/')))
            return reject(QStringLiteral("dependency '%1' must be one directory inside the bundle")
                              .arg(dep));
        out << "  dep              " << normalized << '\n';
    }

    // A dependency directory with no declared file behind it is a search-path
    // entry with no closure, so the layout is incoherent on its own terms.
    const QJsonArray files = object.value(QStringLiteral("files")).toArray();
    if (files.isEmpty()) return reject(QStringLiteral("'files' is empty; a bundle ships files"));
    QSet<QString> declared;
    qsizetype unrecorded = 0;
    bool moduleDeclared = false;
    for (const QJsonValue& value : files) {
        if (!value.isObject()) return reject(QStringLiteral("'files' holds a non-object entry"));
        const QJsonObject record = value.toObject();
        const QString relative = record.value(QStringLiteral("path")).toString();
        QString fileReason;
        const QString normalized = native::safeRelativePath(relative, &fileReason);
        if (normalized.isEmpty()) return reject(QStringLiteral("file %1").arg(fileReason));
        if (declared.contains(normalized.toLower()))
            return reject(QStringLiteral("file '%1' is declared twice").arg(relative));
        declared.insert(normalized.toLower());
        if (normalized.compare(module, Qt::CaseInsensitive) == 0) moduleDeclared = true;
        const QString digest = record.value(QStringLiteral("sha256")).toString().toLower();
        if (digest == kUnrecordedDigest) ++unrecorded;
        out << "  file             " << normalized.leftJustified(36) << " "
            << QString::number(static_cast<qint64>(record.value(QStringLiteral("bytes")).toDouble()))
                   .leftJustified(12)
            << digest << '\n';
    }
    if (!moduleDeclared)
        return reject(QStringLiteral("module '%1' is not in the file inventory").arg(module));
    for (const QString& dep : deps) {
        bool holds = false;
        for (const QString& file : declared) {
            if (file.startsWith(dep.toLower() + QLatin1Char('/'))) {
                holds = true;
                break;
            }
        }
        if (!holds)
            return reject(QStringLiteral("dependency directory '%1' holds no declared file")
                              .arg(dep));
    }

    // The optional preparation block, validated as a layout: every tool path and
    // backend library must be declared, and a runtime directory must hold a
    // declared file and be a declared dependency when it carries DLLs. Only the
    // declared layout is checked here; the bytes are `verify`'s job.
    bool preparationUnrecorded = false;
    if (object.contains(QStringLiteral("preparation"))) {
        if (kind != QLatin1String("native"))
            return reject(QStringLiteral("a standard bundle declares no preparation block"));
        QString preparationReason;
        const std::optional<native::BundlePreparation> preparation =
            native::parsePreparation(object, &preparationReason);
        if (!preparation.has_value())
            return reject(QStringLiteral("preparation %1").arg(preparationReason));

        for (const QString& tool : {preparation->tools.inspect, preparation->tools.prep,
                                    preparation->tools.verifier}) {
            if (!declared.contains(tool.toLower()))
                return reject(QStringLiteral("preparation tool '%1' is not a declared file").arg(tool));
        }
        const auto declaresLibrary = [&declared](const QString& library) {
            for (const QString& file : declared) {
                if (QFileInfo(file).fileName().compare(library, Qt::CaseInsensitive) == 0)
                    return true;
            }
            return false;
        };
        if (!declaresLibrary(native::bundlePreparationProducerLibrary()))
            return reject(QStringLiteral("preparation block names no producer: '%1' is not declared")
                              .arg(native::bundlePreparationProducerLibrary()));
        for (const QString& type : preparation->types) {
            const QString library = native::bundlePreparationBackendLibrary(type);
            if (library.isEmpty())
                return reject(QStringLiteral("preparation type '%1' is not one this build can "
                                             "prepare")
                                  .arg(type));
            if (!declaresLibrary(library))
                return reject(QStringLiteral("preparation type '%1' needs '%2', which is not declared")
                                  .arg(type, library));
        }
        QSet<QString> checkedDirectories;
        for (const QString& directory : {preparation->runtime.ort, preparation->runtime.trt,
                                         preparation->runtime.cuda}) {
            const QString key = directory.toLower();
            if (checkedDirectories.contains(key)) continue;
            checkedDirectories.insert(key);
            bool holds = false;
            bool holdsDll = false;
            for (const QString& file : declared) {
                if (!file.startsWith(key + QLatin1Char('/'))) continue;
                holds = true;
                if (file.endsWith(QLatin1String(".dll"))) holdsDll = true;
            }
            if (!holds)
                return reject(QStringLiteral("preparation runtime directory '%1' holds no declared "
                                             "file")
                                  .arg(directory));
            if (holdsDll) {
                bool declaredDependency = false;
                for (const QString& dep : deps) {
                    if (dep.compare(directory, Qt::CaseInsensitive) == 0) {
                        declaredDependency = true;
                        break;
                    }
                }
                if (!declaredDependency)
                    return reject(QStringLiteral("preparation runtime directory '%1' carries DLLs "
                                                 "but is not a declared dependency")
                                      .arg(directory));
            }
        }
        if (preparation->coreSha == QString(40, QLatin1Char('0')) || preparation->sourceAbi == 0)
            preparationUnrecorded = true;

        out << "  preparation      " << preparation->types.join(QStringLiteral(", ")) << '\n'
            << "  source coreSha   " << preparation->coreSha << '\n'
            << "  source ABI       " << preparation->sourceAbi << '\n'
            << "  tool inspect     " << preparation->tools.inspect << '\n'
            << "  tool prep        " << preparation->tools.prep << '\n'
            << "  tool verifier    " << preparation->tools.verifier << '\n'
            << "  runtime ort      " << preparation->runtime.ort << '\n'
            << "  runtime trt      " << preparation->runtime.trt << '\n'
            << "  runtime cuda     " << preparation->runtime.cuda << '\n';
    }

    out << "  declared files   " << declared.size() << '\n';
    if (preparationUnrecorded)
        out << "  unrecorded       the preparation source is a placeholder: this is a layout "
               "example, not a measured bundle, and `verify` will refuse it\n";
    if (unrecorded > 0)
        out << "  unrecorded       " << unrecorded
            << " record(s) carry the placeholder digest: this is a layout example, not a measured "
               "bundle, and `verify` will refuse it until the bytes exist\n";
    return 0;
}

int describe(const QString& modulePath, QTextStream& out, QTextStream& err) {
    QString reason;
    const std::optional<native::Bundle> bundle = native::describeStandardRuntime(modulePath, &reason);
    if (!bundle.has_value()) {
        err << "rejected: " << modulePath << ": " << reason << '\n';
        return 2;
    }
    out << "described: " << modulePath << '\n';
    describeBundle(out, *bundle);
    return 0;
}

int list(const QStringList& roots, QTextStream& out, QTextStream& err) {
    QStringList rejections;
    const QList<native::Bundle> bundles = native::discoverBundles(roots, &rejections);
    for (const native::Bundle& bundle : bundles) {
        out << "found: " << bundle.root << '\n';
        describeBundle(out, bundle);
    }
    for (const QString& rejection : rejections) err << "rejected: " << rejection << '\n';
    if (bundles.isEmpty() && rejections.isEmpty()) out << "no bundle found under the given roots\n";
    return 0;
}

int install(const QString& source, const QString& runtimeRoot, QTextStream& out, QTextStream& err) {
    QString reason;
    const std::optional<native::Bundle> bundle = native::readBundle(source, &reason);
    if (!bundle.has_value()) {
        err << "rejected: " << source << ": " << reason << '\n';
        return 2;
    }
    if (!native::installAtomic(*bundle, runtimeRoot, &reason)) {
        err << "install failed: " << reason << '\n';
        return 3;
    }
    out << "installed: " << bundle->root << " into " << runtimeRoot << '\n';
    out << "  published as " << native::bundleDirectoryName(bundle->kind, bundle->version) << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mikai_bundle_tool"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1"));
    const QStringList rawArguments = application.arguments();
    if (rawArguments.size() == 4 && rawArguments.at(1) == QLatin1String("__load-dll")) {
        QTextStream err(stderr);
        return loadDependencyOnly(rawArguments.at(2), rawArguments.at(3), err);
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Validate, describe, list and install mikai runtime bundles. Only 'verify' "
                       "loads a module; 'describe', 'list' and 'manifest' read files only."));
    parser.addHelpOption();
    parser.addPositionalArgument(
        QStringLiteral("command"),
        QStringLiteral("verify | describe | manifest | list | install"));
    parser.addPositionalArgument(QStringLiteral("path"),
                                 QStringLiteral("bundle root, module path for 'describe', or "
                                                "manifest file for 'manifest'"));
    parser.addOption({QStringLiteral("root"), QStringLiteral("Runtime root for 'install', or a "
                                                            "search root for 'list' (repeatable)"),
                       QStringLiteral("directory")});
    parser.process(application);

    QTextStream out(stdout);
    QTextStream err(stderr);
    const QStringList arguments = parser.positionalArguments();
    if (arguments.isEmpty()) {
        err << "a command is required\n";
        return 2;
    }
    const QString command = arguments.first();
    const QString path = arguments.size() > 1 ? arguments.at(1) : QString{};
    const QStringList roots = parser.values(QStringLiteral("root"));
    const QString runtimeRoot =
        roots.isEmpty() ? native::runtimeStorageRoot() : roots.first();

    int rc = 2;
    bool loadedAModule = false;
    if (command == QLatin1String("verify")) {
        if (path.isEmpty()) {
            err << "verify needs a bundle root\n";
        } else if (!QFileInfo(path).isDir()) {
            // The layout examples are named native.json / stock.json precisely so
            // they can never be an installed bundle's bundle.json. Describing one
            // is `manifest`'s job; verifying needs the assembled root beside it.
            err << "verify needs a bundle root directory, not " << path
                << "\nuse 'manifest " << path << "' to read the layout it declares\n";
        } else {
            loadedAModule = true;
            rc = verify(QDir(path).absolutePath(), out, err);
        }
    } else if (command == QLatin1String("describe")) {
        if (path.isEmpty()) {
            err << "describe needs a module path\n";
        } else {
            rc = describe(QFileInfo(path).absoluteFilePath(), out, err);
        }
    } else if (command == QLatin1String("manifest")) {
        if (path.isEmpty()) {
            err << "manifest needs a manifest file\n";
        } else {
            rc = manifest(QFileInfo(path).absoluteFilePath(), out, err);
        }
    } else if (command == QLatin1String("list")) {
        rc = list(roots, out, err);
    } else if (command == QLatin1String("install")) {
        if (path.isEmpty()) {
            err << "install needs a bundle root\n";
        } else {
            rc = install(QDir(path).absolutePath(), runtimeRoot, out, err);
        }
    } else {
        err << "unknown command '" << command << "'\n";
    }

    out.flush();
    err.flush();
    if (rc == 0) {
        if (loadedAModule)
            out << "Verified against the bytes and the loader: files present, hash-matching and "
                   "loadable under a restricted search inside the bundle root, one mpv module, "
                   "and - for a native bundle - a core that accepts @mikai_native.\n"
                   "Not verified: playback, a prepared engine, or any real frame.\n";
        else
            out << "This is file-level validation only: not module loadability, not native filter "
                   "registration.\n";
    }
    return rc;
}