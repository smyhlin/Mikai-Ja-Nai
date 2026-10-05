#include "MpvApi.hpp"

#include "native/bundle/NativePaths.hpp"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QObject>
#include <QStringList>

#include <algorithm>
#include <type_traits>

namespace mplayer {
namespace {

QString win32Error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString text;
    if (buffer != nullptr && length != 0) {
        text = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
    }
    if (buffer != nullptr) LocalFree(buffer);
    if (text.isEmpty()) text = QObject::tr("unknown Windows error");
    return QStringLiteral("%1 (Win32 %2)").arg(text).arg(code);
}

// A retry after a transient provisioning hiccup re-presents the same module,
// so the resolved path decides identity as much as the descriptor id does. When
// neither carries a descriptor id the closure decides it too: the same module
// reached through a different dependency set is a different selection, because
// the search that resolved it would be a different one.
bool sameSelection(const MpvBundle& a, const MpvBundle& b) {
    if (!a.descriptorId.isEmpty() && !b.descriptorId.isEmpty() &&
        a.descriptorId != b.descriptorId) {
        return false;
    }
    if (QDir::cleanPath(a.modulePath).compare(QDir::cleanPath(b.modulePath),
                                              Qt::CaseInsensitive) != 0) {
        return false;
    }
    const auto canonicalDeps = [](const MpvBundle& bundle) {
        QStringList result;
        result.reserve(bundle.deps.size());
        for (const QString& dep : bundle.deps) result.append(QDir::cleanPath(dep));
        std::sort(result.begin(), result.end());
        return result;
    };
    return canonicalDeps(a) == canonicalDeps(b);
}

}  // namespace

MpvApi& MpvApi::instance() {
    static MpvApi api;
    return api;
}

quint64 MpvApi::headerApiVersion() {
    return static_cast<quint64>(MPV_CLIENT_API_VERSION);
}

bool MpvApi::load(const MpvBundle& bundle, QString* reason) {
    QMutexLocker lock(&mutex_);
    auto fail = [&](const QString& message) {
        state_.error = message;
        if (reason != nullptr) *reason = message;
        return false;
    };
    auto done = [&]() {
        state_.error.clear();
        if (reason != nullptr) reason->clear();
        return true;
    };

    if (state_.loaded) {
        // A re-selection is a no-op, not a reload: unloading would pull the
        // code out from under every live core handle and cached table pointer.
        if (sameSelection(state_.bundle, bundle)) return done();
        return fail(QObject::tr("Refusing to replace the selected mpv module (%1) with a "
                                "different one (%2) while it is in use")
                        .arg(state_.bundle.descriptorId.isEmpty()
                                 ? state_.bundle.modulePath
                                 : state_.bundle.descriptorId,
                             bundle.descriptorId.isEmpty() ? bundle.modulePath
                                                          : bundle.descriptorId));
    }

    if (bundle.root.isEmpty() || bundle.modulePath.isEmpty()) {
        return fail(QObject::tr("mpv runtime unavailable: the selection descriptor names no "
                                "root or no module path"));
    }

    // This is the one boundary where a module actually becomes code in the
    // process, so the descriptor's own spelling is proved representable before
    // the loader is asked anything: a truncation, a device namespace or a
    // redirector share is refused here by name rather than becoming a load.
    // The wide `LoadLibraryExW` below is only safe because this ran first.
    const native::NativePathTrust rootTrust = native::nativePathTrust(bundle.root, true);
    if (!rootTrust.trusted) {
        return fail(QObject::tr("mpv runtime unavailable: the selected root is not a usable "
                                "local path: %1")
                        .arg(rootTrust.reason));
    }
    const native::NativePathTrust moduleTrust = native::nativePathTrust(bundle.modulePath, true);
    if (!moduleTrust.trusted) {
        return fail(QObject::tr("mpv runtime unavailable: the selected module path is not a "
                                "usable local path: %1")
                        .arg(moduleTrust.reason));
    }

    const QFileInfo rootInfo(QDir::cleanPath(bundle.root));
    const QFileInfo moduleInfo(QDir::cleanPath(bundle.modulePath));
    if (!rootInfo.isAbsolute() || !moduleInfo.isAbsolute()) {
        return fail(QObject::tr("mpv runtime unavailable: the selection descriptor must name "
                                "absolute paths (root=%1, module=%2)")
                        .arg(bundle.root, bundle.modulePath));
    }

    // Canonicalising first resolves junctions and 8.3 names, so containment is
    // checked against the file the loader will actually open.
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    const QString canonicalModule = moduleInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty() || canonicalModule.isEmpty()) {
        return fail(QObject::tr("mpv runtime unavailable: %1 does not exist")
                        .arg(canonicalRoot.isEmpty() ? bundle.root : bundle.modulePath));
    }

    // The canonical spellings are what reach the loader and the search, so they
    // carry the same obligation as the ones the caller supplied.
    const native::NativePathTrust canonicalRootTrust = native::nativePathTrust(canonicalRoot, true);
    if (!canonicalRootTrust.trusted) {
        return fail(QObject::tr("mpv runtime unavailable: the root does not resolve to a usable "
                                "local path: %1")
                        .arg(canonicalRootTrust.reason));
    }
    const native::NativePathTrust canonicalModuleTrust = native::nativePathTrust(canonicalModule, true);
    if (!canonicalModuleTrust.trusted) {
        return fail(QObject::tr("mpv runtime unavailable: the module does not resolve to a usable "
                                "local path: %1")
                        .arg(canonicalModuleTrust.reason));
    }

    const QString rootPrefix = canonicalRoot.endsWith(QLatin1Char('/'))
                                   ? canonicalRoot
                                   : canonicalRoot + QLatin1Char('/');
    if (!canonicalModule.startsWith(rootPrefix, Qt::CaseInsensitive)) {
        return fail(QObject::tr("mpv runtime unavailable: the module %1 is outside the owned "
                                "runtime root %2")
                        .arg(canonicalModule, canonicalRoot));
    }

    QList<HANDLE> cookies;
    // What was actually registered, canonicalised: the descriptor the loader
    // records has to be the one the search resolves through, not the spelling
    // the caller handed over.
    QStringList registeredDeps;
    const auto unregisterAll = [&cookies] {
        for (HANDLE cookie : cookies) RemoveDllDirectory(cookie);
        cookies.clear();
    };
    for (const QString& dep : bundle.deps) {
        // A dependency directory is registered with the loader for the life of
        // the process, so it is checked by the same rule as the module: what the
        // descriptor spells has to be a local path the platform can carry.
        const native::NativePathTrust depTrust = native::nativePathTrust(dep, true);
        if (!depTrust.trusted) {
            unregisterAll();
            return fail(QObject::tr("mpv runtime unavailable: dependency directory %1 is not a "
                                    "usable local path: %2")
                            .arg(dep, depTrust.reason));
        }
        const QFileInfo depInfo(QDir::cleanPath(dep));
        if (!depInfo.isAbsolute() || !depInfo.isDir()) {
            unregisterAll();
            return fail(QObject::tr("mpv runtime unavailable: dependency %1 is not an existing "
                                    "directory")
                            .arg(dep));
        }
        const QString canonicalDep = depInfo.canonicalFilePath();
        if (!canonicalDep.startsWith(rootPrefix, Qt::CaseInsensitive)) {
            unregisterAll();
            return fail(QObject::tr("mpv runtime unavailable: dependency directory %1 is outside "
                                    "the owned runtime root %2")
                            .arg(canonicalDep, canonicalRoot));
        }
        const native::NativePathTrust canonicalDepTrust = native::nativePathTrust(canonicalDep, true);
        if (!canonicalDepTrust.trusted) {
            unregisterAll();
            return fail(QObject::tr("mpv runtime unavailable: dependency directory %1 does not "
                                    "resolve to a usable local path: %2")
                            .arg(canonicalDep, canonicalDepTrust.reason));
        }
        if (registeredDeps.contains(canonicalDep, Qt::CaseInsensitive)) continue;
        // The module's own imports resolve through these. The process PATH is
        // never read and never written.
        const HANDLE cookie =
            AddDllDirectory(reinterpret_cast<LPCWSTR>(canonicalDep.utf16()));
        if (cookie == nullptr) {
            // Silently dropping it would leave the restricted search answering
            // for this module's dependencies from somewhere else, or fail later
            // with an error that names the module instead of the directory. A
            // fork bundle carries the deepest closure this app has, so a
            // dependency it declares has to be honoured or refused by name.
            const DWORD code = GetLastError();
            unregisterAll();
            return fail(QObject::tr("mpv runtime unavailable: dependency directory %1 cannot be "
                                    "registered: %2")
                            .arg(canonicalDep, win32Error(code)));
        }
        cookies.append(cookie);
        registeredDeps.append(canonicalDep);
    }

    // Own directory, the registered dependency dirs and System32 - the one
    // legitimate dependency outside the root. No PATH, no current directory,
    // no default Windows folders.
    constexpr DWORD kSearch = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 |
                              LOAD_LIBRARY_SEARCH_USER_DIRS;
    const HMODULE module =
        LoadLibraryExW(reinterpret_cast<LPCWSTR>(canonicalModule.utf16()), nullptr, kSearch);
    if (module == nullptr) {
        const DWORD code = GetLastError();
        unregisterAll();
        return fail(QObject::tr("mpv runtime unavailable: cannot load %1: %2")
                        .arg(canonicalModule, win32Error(code)));
    }

    // Every entry point the player uses has to resolve now: a table with a hole
    // in it becomes a null call on whichever code path happens to be first.
    MpvApiInstance table;
    const char* missing = nullptr;
    const auto bind = [&](const char* name, auto& slot) {
        using Fn = std::remove_reference_t<decltype(slot)>;
        static_assert(std::is_pointer_v<Fn>);
        const FARPROC address = ::GetProcAddress(module, name);
        if (address == nullptr) {
            if (missing == nullptr) missing = name;
            return;
        }
        slot = reinterpret_cast<Fn>(address);
    };
    bind("mpv_create", table.create);
    bind("mpv_set_option_string", table.setOptionString);
    bind("mpv_initialize", table.initialize);
    bind("mpv_request_log_messages", table.requestLogMessages);
    bind("mpv_terminate_destroy", table.terminateDestroy);
    bind("mpv_command", table.command);
    bind("mpv_command_async", table.commandAsync);
    bind("mpv_command_node_async", table.commandNodeAsync);
    bind("mpv_get_property", table.getProperty);
    bind("mpv_get_property_async", table.getPropertyAsync);
    bind("mpv_get_property_string", table.getPropertyString);
    bind("mpv_free", table.free);
    bind("mpv_free_node_contents", table.freeNodeContents);
    bind("mpv_set_property", table.setProperty);
    bind("mpv_set_property_string", table.setPropertyString);
    bind("mpv_observe_property", table.observeProperty);
    bind("mpv_unobserve_property", table.unobserveProperty);
    bind("mpv_wait_event", table.waitEvent);
    bind("mpv_client_api_version", table.clientApiVersion);
    bind("mpv_error_string", table.errorString);
    bind("mpv_render_context_create", table.renderContextCreate);
    bind("mpv_render_context_set_update_callback", table.renderContextSetUpdateCallback);
    bind("mpv_render_context_update", table.renderContextUpdate);
    bind("mpv_render_context_report_swap", table.renderContextReportSwap);
    bind("mpv_render_context_render", table.renderContextRender);
    bind("mpv_render_context_free", table.renderContextFree);
    if (missing != nullptr) {
        unregisterAll();
        FreeLibrary(module);
        return fail(QObject::tr("mpv runtime unavailable: %1 does not export %2")
                        .arg(canonicalModule, QString::fromLatin1(missing)));
    }

    // A differing major client API version means a different event/struct
    // contract, so every call through this table would misread its arguments.
    const quint64 runtimeVersion = static_cast<quint64>(table.clientApiVersion());
    const quint64 headerVersion = headerApiVersion();
    if ((runtimeVersion >> 16) != (headerVersion >> 16)) {
        unregisterAll();
        FreeLibrary(module);
        return fail(QObject::tr("mpv runtime unavailable: %1 reports client API %2, which is "
                                "incompatible with the %3 it was selected against")
                        .arg(canonicalModule)
                        .arg(runtimeVersion)
                        .arg(headerVersion));
    }

    state_.module = module;
    state_.funcs = table;
    state_.bundle = bundle;
    state_.bundle.root = canonicalRoot;
    state_.bundle.modulePath = canonicalModule;
    // The recorded closure is the one that is registered, so a later caller that
    // compares the loader's descriptor against the selection that produced it is
    // comparing the directories the search actually uses.
    state_.bundle.deps = registeredDeps;
    state_.apiVersion = runtimeVersion;
    state_.loaded = true;
    // Held for the life of the process, never removed: the module stays mapped
    // past every core handle, so its imports must keep resolving the same way.
    for (HANDLE cookie : cookies) state_.userDirs.append(cookie);
    return done();
}

bool MpvApi::loaded() const {
    QMutexLocker lock(&mutex_);
    return state_.loaded;
}

const MpvBundle& MpvApi::bundle() const {
    QMutexLocker lock(&mutex_);
    return state_.bundle;
}

const MpvApiInstance& MpvApi::functions() const {
    QMutexLocker lock(&mutex_);
    return state_.funcs;
}

HMODULE MpvApi::module() const noexcept {
    QMutexLocker lock(&mutex_);
    return state_.module;
}

quint64 MpvApi::apiVersion() const {
    QMutexLocker lock(&mutex_);
    return state_.apiVersion;
}

QString MpvApi::error() const {
    QMutexLocker lock(&mutex_);
    return state_.error;
}

}  // namespace mplayer