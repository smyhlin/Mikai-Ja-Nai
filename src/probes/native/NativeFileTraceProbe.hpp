// TEMPORARY DIAGNOSTIC SCAFFOLD - DELETE AFTER ATTRIBUTION.
//
// Probe-only, user-mode Win32 file trace for the external model registry. It
// exists to attribute the "Access is denied." failure of the atomic metadata
// commit without elevation, kernel tooling, Procmon or an SDK patch. It hooks
// only this executable's and Qt6Core.dll's import tables, and only for
// CreateFileW / CloseHandle / SetFileInformationByHandle / SetFileAttributesW.
// Every other file, process and API is left untouched. It never retries,
// bypasses, ignores or suppresses an application result: the real call's return
// value and GetLastError() are preserved for the caller.
//
// Logging uses a pre-opened Win32 write handle and WriteFile only. No Qt I/O,
// no heap use and no locks that an application thread could hold are taken
// outside the small internal log/table critical sections.
//
// Every hooked call immediately captures the real return value and
// GetLastError(), logs after that capture, and then restores that same value
// with SetLastError() before returning, so the application observes exactly
// what the real API produced. Target strings are logged losslessly as UTF-16
// converted to UTF-8.
//
// Limit: a SetFileAttributesW issued inside ucrtbase (for example by _wchmod,
// which QtCore uses for QSaveFile::setPermissions) is not reached because only
// this executable's and Qt6Core.dll's import tables are patched. That call
// targets the temporary file, not the destination, so it cannot hide a
// destination attribute change made through QtCore's own direct calls.

#pragma once

#include <QtCore/qglobal.h>

#include <cstdarg>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <cwctype>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace mikai::probes::native_trace {

#ifdef Q_OS_WIN

namespace detail {

constexpr int kMaxTrackedHandles = 128;
constexpr int kMaxTrackedPath = 1024;
constexpr wchar_t kLevelFile[] = L"external-models.json";
constexpr wchar_t kLevelTempPrefix[] = L"external-models.json.";
constexpr wchar_t kRenameSeparator[] = L"========================================\n";

struct TrackedHandle {
    HANDLE handle = nullptr;
    wchar_t path[kMaxTrackedPath] = {};
    DWORD access = 0;
    DWORD share = 0;
    bool open = false;
};

inline CRITICAL_SECTION g_stateLock;
inline CRITICAL_SECTION g_logLock;
inline TrackedHandle g_tracked[kMaxTrackedHandles];
inline HANDLE g_log = INVALID_HANDLE_VALUE;
inline thread_local int g_hookDepth = 0;

using FnCreateFileW = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD,
                                      DWORD, HANDLE);
using FnCloseHandle = BOOL(WINAPI*)(HANDLE);
using FnSetFileInformationByHandle = BOOL(WINAPI*)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID,
                                                   DWORD);
using FnSetFileAttributesW = BOOL(WINAPI*)(LPCWSTR, DWORD);

inline FnCreateFileW g_realCreateFileW = nullptr;
inline FnCloseHandle g_realCloseHandle = nullptr;
inline FnSetFileInformationByHandle g_realSetFileInformationByHandle = nullptr;
inline FnSetFileAttributesW g_realSetFileAttributesW = nullptr;

struct HookScope {
    const bool active;
    HookScope() : active(g_hookDepth == 0) {
        if (active) g_hookDepth = 1;
    }
    ~HookScope() {
        if (active) g_hookDepth = 0;
    }
};

inline const wchar_t* fileNamePart(const wchar_t* path) {
    if (path == nullptr) return L"";
    const wchar_t* base = path;
    for (const wchar_t* at = path; *at != 0; ++at) {
        if (*at == L'\\' || *at == L'/') base = at + 1;
    }
    return base;
}

/// True for the registry file itself and for the QSaveFile temp siblings named
/// `<registry>.<random>`, which is the only path set this trace authorizes.
inline bool isTraceTarget(const wchar_t* path) {
    const wchar_t* base = fileNamePart(path);
    return _wcsicmp(base, kLevelFile) == 0 || _wcsnicmp(base, kLevelTempPrefix, 21) == 0;
}

inline bool isAbsolutePath(const wchar_t* path) {
    if (path == nullptr) return false;
    const size_t length = wcslen(path);
    if (length < 3) return false;
    if (path[0] == L'\\' && path[1] == L'\\') return true;
    return iswalpha(path[0]) && path[1] == L':' &&
           (path[2] == L'\\' || path[2] == L'/');
}

inline void writeLogText(const wchar_t* text, int length) {
    if (g_log == INVALID_HANDLE_VALUE || text == nullptr || length <= 0) return;
    char utf8[16384];
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text, length, utf8,
                                            static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    if (bytes <= 0) return;
    ::EnterCriticalSection(&g_logLock);
    DWORD written = 0;
    ::WriteFile(g_log, utf8, static_cast<DWORD>(bytes), &written, nullptr);
    ::LeaveCriticalSection(&g_logLock);
}

inline void logLine(const wchar_t* format, ...) {
    wchar_t line[4096];
    va_list args;
    va_start(args, format);
    const int length = std::vswprintf(line, 4096, format, args);
    va_end(args);
    if (length <= 0) return;
    int bounded = length < 4093 ? length : 4093;
    line[bounded++] = L'\r';
    line[bounded++] = L'\n';
    line[bounded] = 0;
    writeLogText(line, bounded);
}

inline void trackOpen(HANDLE handle, const wchar_t* path, DWORD access, DWORD share) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return;
    ::EnterCriticalSection(&g_stateLock);
    for (TrackedHandle& entry : g_tracked) {
        if (!entry.open) {
            entry.handle = handle;
            entry.access = access;
            entry.share = share;
            entry.open = true;
            const wchar_t* source = path != nullptr ? path : L"";
            wcsncpy(entry.path, source, kMaxTrackedPath - 1);
            entry.path[kMaxTrackedPath - 1] = 0;
            break;
        }
    }
    ::LeaveCriticalSection(&g_stateLock);
}

/// Finds a tracked open handle and copies its record out under the table lock.
inline bool findOpenHandle(HANDLE handle, TrackedHandle* out) {
    bool found = false;
    ::EnterCriticalSection(&g_stateLock);
    for (const TrackedHandle& entry : g_tracked) {
        if (entry.open && entry.handle == handle) {
            if (out != nullptr) *out = entry;
            found = true;
            break;
        }
    }
    ::LeaveCriticalSection(&g_stateLock);
    return found;
}

inline void markClosed(HANDLE handle) {
    ::EnterCriticalSection(&g_stateLock);
    for (TrackedHandle& entry : g_tracked) {
        if (entry.open && entry.handle == handle) {
            entry.open = false;
            break;
        }
    }
    ::LeaveCriticalSection(&g_stateLock);
}

/// How many of this process's own traced handles are still open on destination.
/// The destination spelling is compared case-insensitively after separator
/// normalization; the temp sibling has a different name and never counts.
inline int countOpenOnTarget(const wchar_t* destination, wchar_t* detail,
                             size_t detailCapacity) {
    int count = 0;
    if (detail != nullptr && detailCapacity > 0) detail[0] = 0;
    ::EnterCriticalSection(&g_stateLock);
    for (const TrackedHandle& entry : g_tracked) {
        if (!entry.open) continue;
        if (isTraceTarget(entry.path) &&
            _wcsicmp(fileNamePart(entry.path), fileNamePart(destination)) == 0) {
            if (detail != nullptr && detailCapacity > 0) {
                wchar_t piece[96];
                std::swprintf(piece, 96, L"[share=0x%08lX access=0x%08lX]",
                         static_cast<unsigned long>(entry.share),
                         static_cast<unsigned long>(entry.access));
                const size_t used = wcslen(detail);
                if (used + 1 < detailCapacity) {
                    wcsncat(detail, piece, detailCapacity - used - 1);
                }
            }
            ++count;
        }
    }
    ::LeaveCriticalSection(&g_stateLock);
    return count;
}

inline void logAttributes(const wchar_t* label, const wchar_t* path) {
    const DWORD attributes = ::GetFileAttributesW(path);
    logLine(L"%ls attrs=0x%08lX readOnly=%d exists=%d",
            label, static_cast<unsigned long>(attributes),
            attributes != INVALID_FILE_ATTRIBUTES &&
                    (attributes & FILE_ATTRIBUTE_READONLY) != 0
                ? 1
                : 0,
            attributes != INVALID_FILE_ATTRIBUTES ? 1 : 0);
}

inline HANDLE WINAPI hookCreateFileW(LPCWSTR fileName, DWORD desiredAccess, DWORD shareMode,
                                     LPSECURITY_ATTRIBUTES securityAttributes,
                                     DWORD creationDisposition, DWORD flagsAndAttributes,
                                     HANDLE templateFile) {
    const HookScope scope;
    if (!scope.active || g_realCreateFileW == nullptr) {
        return g_realCreateFileW != nullptr
                   ? g_realCreateFileW(fileName, desiredAccess, shareMode, securityAttributes,
                                       creationDisposition, flagsAndAttributes, templateFile)
                   : INVALID_HANDLE_VALUE;
    }
    const HANDLE result = g_realCreateFileW(fileName, desiredAccess, shareMode,
                                            securityAttributes, creationDisposition,
                                            flagsAndAttributes, templateFile);
    const DWORD error = ::GetLastError();
    if (fileName != nullptr && isTraceTarget(fileName)) {
        logLine(L"CreateFileW path=\"%ls\" access=0x%08lX share=0x%08lX disposition=%lu "
                L"flags=0x%08lX handle=%p result=%ls error=%lu",
                fileName, static_cast<unsigned long>(desiredAccess),
                static_cast<unsigned long>(shareMode),
                static_cast<unsigned long>(creationDisposition),
                static_cast<unsigned long>(flagsAndAttributes), result,
                result != INVALID_HANDLE_VALUE ? L"ok" : L"failed",
                static_cast<unsigned long>(error));
        logAttributes(L"CreateFileW target", fileName);
        if (result != INVALID_HANDLE_VALUE) {
            trackOpen(result, fileName, desiredAccess, shareMode);
            logLine(L"TRACK open handle=%p path=\"%ls\"", result, fileName);
        }
    }
    ::SetLastError(error);
    return result;
}

inline BOOL WINAPI hookCloseHandle(HANDLE handle) {
    const HookScope scope;
    TrackedHandle tracked;
    const bool wasTracked = scope.active && findOpenHandle(handle, &tracked);
    const BOOL result = g_realCloseHandle != nullptr ? g_realCloseHandle(handle) : FALSE;
    const DWORD error = ::GetLastError();
    if (wasTracked) {
        logLine(L"CloseHandle handle=%p path=\"%ls\" trackOpenBefore=%d result=%ls error=%lu",
                handle, tracked.path, 1, result ? L"ok" : L"failed",
                static_cast<unsigned long>(error));
        if (result) markClosed(handle);
    }
    ::SetLastError(error);
    return result;
}

inline BOOL WINAPI hookSetFileInformationByHandle(HANDLE handle,
                                                  FILE_INFO_BY_HANDLE_CLASS infoClass,
                                                  LPVOID info, DWORD infoSize) {
    const HookScope scope;
    if (!scope.active || g_realSetFileInformationByHandle == nullptr) {
        return g_realSetFileInformationByHandle != nullptr
                   ? g_realSetFileInformationByHandle(handle, infoClass, info, infoSize)
                   : FALSE;
    }
    wchar_t renameTarget[kMaxTrackedPath] = {};
    bool isRename = false;
    BOOL replaceExisting = FALSE;
    if (infoClass == FileRenameInfo && info != nullptr &&
        infoSize >= offsetof(FILE_RENAME_INFO, FileName)) {
        const auto* rename = static_cast<const FILE_RENAME_INFO*>(info);
        isRename = true;
        replaceExisting = *reinterpret_cast<const BYTE*>(rename);
        const size_t nameOffset = offsetof(FILE_RENAME_INFO, FileName);
        const size_t available =
            infoSize > nameOffset ? (infoSize - nameOffset) / sizeof(wchar_t) : 0;
        size_t units = rename->FileNameLength / sizeof(wchar_t);
        if (units > available) units = available;
        if (units > kMaxTrackedPath - 1) units = kMaxTrackedPath - 1;
        if (rename->FileName != nullptr) {
            wcsncpy(renameTarget, rename->FileName, units);
            renameTarget[units] = 0;
        }
    }
    const BOOL result = g_realSetFileInformationByHandle(handle, infoClass, info, infoSize);
    const DWORD error = ::GetLastError();
    if (scope.active) {
        TrackedHandle tracked;
        const bool trackedHandle = findOpenHandle(handle, &tracked);
        if (isRename && (isTraceTarget(renameTarget) || trackedHandle)) {
            wchar_t ownOpen[768];
            const int openCount = countOpenOnTarget(renameTarget, ownOpen, 768);
            logLine(L"SetFileInformationByHandle RENAME handle=%p source=\"%ls\" "
                    L"target=\"%ls\" replaceExisting=%d size=%lu result=%ls error=%lu "
                    L"ownOpenOnTarget=%d %ls",
                    handle, trackedHandle ? tracked.path : L"(untracked)", renameTarget,
                    replaceExisting ? 1 : 0, static_cast<unsigned long>(infoSize),
                    result ? L"ok" : L"failed", static_cast<unsigned long>(error),
                    openCount, ownOpen);
            logAttributes(L"RENAME target", renameTarget);
        } else if (trackedHandle) {
            logLine(L"SetFileInformationByHandle handle=%p class=%d path=\"%ls\" result=%ls "
                    L"error=%lu",
                    handle, static_cast<int>(infoClass), tracked.path,
                    result ? L"ok" : L"failed", static_cast<unsigned long>(error));
        }
    }
    ::SetLastError(error);
    return result;
}

inline BOOL WINAPI hookSetFileAttributesW(LPCWSTR fileName, DWORD attributes) {
    const HookScope scope;
    if (!scope.active || g_realSetFileAttributesW == nullptr) {
        return g_realSetFileAttributesW != nullptr
                   ? g_realSetFileAttributesW(fileName, attributes)
                   : FALSE;
    }
    const BOOL result = g_realSetFileAttributesW(fileName, attributes);
    const DWORD error = ::GetLastError();
    if (fileName != nullptr && isTraceTarget(fileName)) {
        logLine(L"SetFileAttributesW path=\"%ls\" attributes=0x%08lX readOnly=%d result=%ls "
                L"error=%lu",
                fileName, static_cast<unsigned long>(attributes),
                (attributes & FILE_ATTRIBUTE_READONLY) != 0 ? 1 : 0,
                result ? L"ok" : L"failed", static_cast<unsigned long>(error));
        logAttributes(L"SetFileAttributesW after", fileName);
    }
    ::SetLastError(error);
    return result;
}

inline int patchImports(HMODULE module) {
    if (module == nullptr) return 0;
    auto* base = reinterpret_cast<BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size == 0) return 0;
    int patched = 0;
    auto* descriptor =
        reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        // Without an original thunk array the IAT holds resolved addresses, not
        // import-by-name records; there is no safe name to match.
        if (descriptor->OriginalFirstThunk == 0) continue;
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + descriptor->OriginalFirstThunk);
        for (; nameThunk->u1.AddressOfData != 0; ++thunk, ++nameThunk) {
            if ((nameThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) != 0) continue;
            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + nameThunk->u1.AddressOfData);
            const char* name = reinterpret_cast<const char*>(import->Name);
            FARPROC replacement = nullptr;
            if (std::strcmp(name, "CreateFileW") == 0) {
                replacement = reinterpret_cast<FARPROC>(&hookCreateFileW);
            } else if (std::strcmp(name, "CloseHandle") == 0) {
                replacement = reinterpret_cast<FARPROC>(&hookCloseHandle);
            } else if (std::strcmp(name, "SetFileInformationByHandle") == 0) {
                replacement = reinterpret_cast<FARPROC>(&hookSetFileInformationByHandle);
            } else if (std::strcmp(name, "SetFileAttributesW") == 0) {
                replacement = reinterpret_cast<FARPROC>(&hookSetFileAttributesW);
            }
            if (replacement == nullptr) continue;
            auto* slot = reinterpret_cast<FARPROC*>(&thunk->u1.Function);
            if (*slot == replacement) continue;
            DWORD previousProtection = 0;
            if (::VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE,
                                 &previousProtection) == FALSE) {
                continue;
            }
            *slot = replacement;
            DWORD ignored = 0;
            ::VirtualProtect(slot, sizeof(void*), previousProtection, &ignored);
            ++patched;
        }
    }
    return patched;
}

}  // namespace detail

/// Installs the trace. The output handle is opened before any import is patched,
/// so logging can never re-enter a hook. Returns false when the path is not
/// absolute, the log cannot be opened, or no matching import slot was found in
/// this executable or Qt6Core.dll.
inline bool install(const wchar_t* outputPath) {
    using namespace detail;
    if (!isAbsolutePath(outputPath)) return false;
    const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == nullptr) return false;
    g_realCreateFileW =
        reinterpret_cast<FnCreateFileW>(::GetProcAddress(kernel32, "CreateFileW"));
    g_realCloseHandle =
        reinterpret_cast<FnCloseHandle>(::GetProcAddress(kernel32, "CloseHandle"));
    g_realSetFileInformationByHandle = reinterpret_cast<FnSetFileInformationByHandle>(
        ::GetProcAddress(kernel32, "SetFileInformationByHandle"));
    g_realSetFileAttributesW = reinterpret_cast<FnSetFileAttributesW>(
        ::GetProcAddress(kernel32, "SetFileAttributesW"));
    if (g_realCreateFileW == nullptr || g_realCloseHandle == nullptr ||
        g_realSetFileInformationByHandle == nullptr ||
        g_realSetFileAttributesW == nullptr) {
        return false;
    }
    ::InitializeCriticalSection(&g_stateLock);
    ::InitializeCriticalSection(&g_logLock);
    g_log = g_realCreateFileW(outputPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log == INVALID_HANDLE_VALUE) return false;
    logLine(L"# native file trace pid=%lu target=%ls",
            static_cast<unsigned long>(::GetCurrentProcessId()), kLevelFile);
    logLine(L"# filters: %ls and %ls<random>", kLevelFile, kLevelTempPrefix);
    int patched = patchImports(::GetModuleHandleW(nullptr));
    logLine(L"# own executable import slots patched=%d", patched);
    const HMODULE qtCore = ::GetModuleHandleW(L"Qt6Core.dll");
    const HMODULE qtCoreDebug = ::GetModuleHandleW(L"Qt6Cored.dll");
    const int qtPatched =
        qtCore != nullptr ? patchImports(qtCore)
                          : (qtCoreDebug != nullptr ? patchImports(qtCoreDebug) : -1);
    logLine(L"# Qt6Core.dll import slots patched=%d%ls", qtPatched,
            qtCore == nullptr && qtCoreDebug == nullptr ? L" (module not loaded)" : L"");
    logLine(L"%ls", kRenameSeparator);
    ::SetLastError(0);
    return patched > 0 || qtPatched > 0;
}

#else  // !Q_OS_WIN

inline bool install(const wchar_t*) { return false; }

#endif  // Q_OS_WIN

}  // namespace mikai::probes::native_trace
