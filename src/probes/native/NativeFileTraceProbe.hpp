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
// Logging is WriteFile on a pre-opened handle. Failures to format or write a
// line are counted and invalidate every absence claim from then on; the log
// never silently loses a record.
//
// The own-handle table is advisory: insertion happens after the real open, and
// the rename count is sampled after the real rename, so the printed value is an
// observation, never a causal proof that no handle existed at failure time.
// Drops, path truncation, basename-only comparisons, in-flight opens/closes and
// log loss are all reported as UNKNOWN/lower-bound, never as zero.
//
// Coverage is explicit per API and module. Missing coverage, a failed image
// bound check or a failed protection restore makes install report failure and
// marks absence claims UNKNOWN.
//
// Limit: a SetFileAttributesW issued inside ucrtbase (for example by _wchmod,
// which QtCore uses for QSaveFile::setPermissions) is not reached because only
// this executable's and Qt6Core.dll's import tables are patched. That call
// targets the temporary file, not the destination.

#pragma once

#include <QtCore/qglobal.h>

#include <atomic>
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

constexpr int kMaxTrackedHandles = 256;
constexpr int kMaxTrackedPath = 4096;
constexpr int kMaxImportName = 256;
constexpr wchar_t kLevelFile[] = L"external-models.json";
constexpr wchar_t kLevelTempPrefix[] = L"external-models.json.";
constexpr wchar_t kRenameSeparator[] = L"========================================";

struct TrackedHandle {
    HANDLE handle = nullptr;
    unsigned long long generation = 0;
    wchar_t path[kMaxTrackedPath] = {};
    DWORD access = 0;
    DWORD share = 0;
    bool open = false;
};

struct ApiCoverage {
    int createFileW = 0;
    int closeHandle = 0;
    int setFileInformation = 0;
    int setFileAttributes = 0;
};

struct ModulePatchResult {
    int patched = 0;
    ApiCoverage coverage;
    bool failed = false;
};

inline CRITICAL_SECTION g_stateLock;
inline CRITICAL_SECTION g_logLock;
inline bool g_locksInitialized = false;
inline TrackedHandle g_tracked[kMaxTrackedHandles];
inline unsigned long long g_nextGeneration = 0;  // under g_stateLock
inline std::atomic<int> g_droppedHandles{0};
inline std::atomic<int> g_truncatedRecords{0};
inline std::atomic<int> g_compareFallback{0};
inline std::atomic<int> g_inFlightCreates{0};
inline std::atomic<int> g_inFlightCloses{0};
inline std::atomic<int> g_logLost{0};
inline HANDLE g_log = INVALID_HANDLE_VALUE;
inline thread_local int g_hookDepth = 0;
inline wchar_t g_installError[768] = {};

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

inline void setInstallError(const wchar_t* text) {
    wcsncpy(g_installError, text != nullptr ? text : L"", 767);
    g_installError[767] = 0;
}

inline const wchar_t* lastError() { return g_installError; }

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

inline bool endsWithNoCase(const wchar_t* text, const wchar_t* suffix) {
    if (text == nullptr || suffix == nullptr) return false;
    const size_t textLength = wcslen(text);
    const size_t suffixLength = wcslen(suffix);
    return textLength >= suffixLength &&
           _wcsicmp(text + textLength - suffixLength, suffix) == 0;
}

inline bool isTraceTarget(const wchar_t* path) {
    const wchar_t* base = fileNamePart(path);
    return _wcsicmp(base, kLevelFile) == 0 || _wcsnicmp(base, kLevelTempPrefix, 21) == 0;
}

inline bool isAbsoluteLocalPath(const wchar_t* path) {
    if (path == nullptr) return false;
    const size_t length = wcslen(path);
    if (length < 3) return false;
    if (path[0] == L'\\' || path[1] != L':') return false;  // no UNC/device forms
    if (!iswalpha(path[0]) || (path[2] != L'\\' && path[2] != L'/')) return false;
    return true;
}

/// Refuses anything that is not a fresh local .log file under an existing,
/// non-reparse directory outside the app-owned native/animejanai stores. The
/// normalized spelling is returned for CreateFileW, so the bytes opened are the
/// bytes validated: dot/dot-dot components, ADS, duplicate separators, entities
/// ending in space or dot, and long-path truncation all fail closed. The caller
/// opens with CREATE_NEW, so an existing file (including a hardlink) is never
/// overwritten.
inline bool tracePathAllowed(const wchar_t* path, wchar_t* normalized, size_t capacity) {
    if (!isAbsoluteLocalPath(path) || normalized == nullptr || capacity < 8) {
        setInstallError(L"output must be an absolute local drive path");
        return false;
    }
    const size_t length = wcslen(path);
    if (length >= capacity - 1) {
        setInstallError(L"output path is longer than the trace supports");
        return false;
    }
    size_t used = 0;
    for (size_t index = 0; index < length; ++index) {
        wchar_t character = path[index];
        if (index >= 2 && character == L':') {
            setInstallError(L"output may not use an alternate data stream");
            return false;
        }
        if (character == L'/') character = L'\\';
        if (character == L'\\' && index > 0 && path[index - 1] == L'\\') {
            setInstallError(L"output path has an empty component");
            return false;
        }
        normalized[used++] = character;
    }
    if (used == 0 || normalized[used - 1] == L'\\') {
        setInstallError(L"output path must name a file");
        return false;
    }
    normalized[used] = 0;
    const wchar_t* filePart = fileNamePart(normalized);
    if (!endsWithNoCase(filePart, L".log")) {
        setInstallError(L"output file name must end with .log");
        return false;
    }
    if (isTraceTarget(normalized)) {
        setInstallError(L"output may not name the registry or its temp sibling");
        return false;
    }
    if (::GetFileAttributesW(normalized) != INVALID_FILE_ATTRIBUTES) {
        setInstallError(L"output must not already exist (fresh trace required)");
        return false;
    }
    wchar_t prefix[kMaxTrackedPath];
    wcsncpy(prefix, normalized, 3);  // "X:\"
    prefix[3] = 0;
    for (size_t at = 3; normalized[at] != 0;) {
        size_t end = at;
        while (normalized[end] != 0 && normalized[end] != L'\\') ++end;
        if (end == at) {
            setInstallError(L"output path has an empty component");
            return false;
        }
        wchar_t component[kMaxTrackedPath];
        const size_t count = end - at < kMaxTrackedPath - 1 ? end - at : kMaxTrackedPath - 1;
        wcsncpy(component, normalized + at, count);
        component[count] = 0;
        if (_wcsicmp(component, L".") == 0 || _wcsicmp(component, L"..") == 0 ||
            component[count - 1] == L' ' || component[count - 1] == L'.') {
            setInstallError(L"output path is not in canonical Win32 spelling");
            return false;
        }
        if (_wcsicmp(component, L"native") == 0 ||
            _wcsicmp(component, L"animejanai") == 0) {
            setInstallError(L"output may not be inside the native or animejanai store");
            return false;
        }
        const bool last = normalized[end] == 0;
        if (!last) {
            wcsncat(prefix, component, kMaxTrackedPath - wcslen(prefix) - 1);
            wcsncat(prefix, L"\\", kMaxTrackedPath - wcslen(prefix) - 1);
            const DWORD attributes = ::GetFileAttributesW(prefix);
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                setInstallError(L"output directory must already exist");
                return false;
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                setInstallError(L"output directory is a reparse alias");
                return false;
            }
        }
        at = last ? end : end + 1;
    }
    return true;
}

/// Removes a stored long-path prefix and unifies separators. Returns false when
/// the input cannot fit, which marks the record and every later count UNKNOWN.
inline bool normalizePath(const wchar_t* input, wchar_t* output, size_t capacity,
                          bool* absolute) {
    if (input == nullptr || output == nullptr || capacity < 4) return false;
    const wchar_t* text = input;
    const size_t inputLength = wcslen(input);
    if (inputLength >= 4 && text[0] == L'\\' && text[1] == L'\\' &&
        text[2] == L'?' && text[3] == L'\\') {
        text += 4;
    }
    const size_t remaining = wcslen(text);
    *absolute = remaining >= 3 && iswalpha(text[0]) && text[1] == L':' &&
                (text[2] == L'\\' || text[2] == L'/');
    size_t used = 0;
    for (; *text != 0; ++text) {
        if (used + 1 >= capacity) return false;
        output[used++] = *text == L'/' ? L'\\' : *text;
    }
    output[used] = 0;
    return used > 0;
}

/// Full normalized path identity. A relative spelling can only be compared by
/// basename, which is reported as a weaker comparison and never as complete.
inline bool sameFileIdentity(const wchar_t* left, const wchar_t* right, bool* fallback,
                             bool* truncated) {
    wchar_t leftPath[kMaxTrackedPath];
    wchar_t rightPath[kMaxTrackedPath];
    bool leftAbsolute = false;
    bool rightAbsolute = false;
    if (!normalizePath(left, leftPath, kMaxTrackedPath, &leftAbsolute) ||
        !normalizePath(right, rightPath, kMaxTrackedPath, &rightAbsolute)) {
        *truncated = true;
        return false;
    }
    if (leftAbsolute && rightAbsolute) {
        return _wcsicmp(leftPath, rightPath) == 0;
    }
    *fallback = true;
    return _wcsicmp(fileNamePart(leftPath), fileNamePart(rightPath)) == 0;
}

inline void writeLogText(const wchar_t* text, int length) {
    if (text == nullptr || length <= 0) return;
    if (g_log == INVALID_HANDLE_VALUE) {
        g_logLost.fetch_add(1);
        return;
    }
    char utf8[32768];
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text, length, utf8,
                                            static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    if (bytes <= 0) {
        g_logLost.fetch_add(1);
        return;
    }
    ::EnterCriticalSection(&g_logLock);
    DWORD written = 0;
    const BOOL ok = ::WriteFile(g_log, utf8, static_cast<DWORD>(bytes), &written, nullptr);
    ::LeaveCriticalSection(&g_logLock);
    if (ok == FALSE || written != static_cast<DWORD>(bytes)) g_logLost.fetch_add(1);
}

inline void logLine(const wchar_t* format, ...) {
    wchar_t payload[8192];
    wchar_t line[8192];
    va_list args;
    va_start(args, format);
    const int payloadLength = std::vswprintf(payload, 8192, format, args);
    va_end(args);
    // Timestamp is taken when the line is written, after the real call; it is
    // an ordering aid, never an event timestamp.
    const int length = payloadLength > 0
                           ? std::swprintf(line, 8192, L"logMs=%llu %ls",
                                           static_cast<unsigned long long>(::GetTickCount64()),
                                           payload)
                           : payloadLength;
    if (length < 0) {
        g_logLost.fetch_add(1);
        line[0] = L'[';
        line[1] = L'L';
        line[2] = L'O';
        line[3] = L'S';
        line[4] = L'T';
        line[5] = L']';
        line[6] = 0;
        writeLogText(line, 6);
        return;
    }
    int bounded = length < 8160 ? length : 8160;
    if (length >= 8160) {
        g_logLost.fetch_add(1);
        static const wchar_t marker[] = L" [TRUNCATED]";
        const int markerLength = static_cast<int>(sizeof(marker) / sizeof(wchar_t)) - 1;
        wcsncpy(line + bounded, marker, markerLength);
        bounded += markerLength;
    }
    line[bounded++] = L'\r';
    line[bounded++] = L'\n';
    line[bounded] = 0;
    writeLogText(line, bounded);
}

inline void trackOpenLocked(HANDLE handle, const wchar_t* path, DWORD access, DWORD share) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return;
    for (TrackedHandle& entry : g_tracked) {
        if (!entry.open) {
            entry.handle = handle;
            entry.generation = ++g_nextGeneration;
            entry.access = access;
            entry.share = share;
            entry.open = true;
            const size_t length = path != nullptr ? wcslen(path) : 0;
            if (length >= kMaxTrackedPath - 1) g_truncatedRecords.fetch_add(1);
            const size_t copy = length < kMaxTrackedPath - 1 ? length : kMaxTrackedPath - 1;
            if (copy > 0) wcsncpy(entry.path, path, copy);
            entry.path[copy] = 0;
            return;
        }
    }
    g_droppedHandles.fetch_add(1);
}

inline bool findOpenHandleLocked(HANDLE handle, TrackedHandle* out) {
    for (const TrackedHandle& entry : g_tracked) {
        if (entry.open && entry.handle == handle) {
            if (out != nullptr) *out = entry;
            return true;
        }
    }
    return false;
}

inline bool findOpenHandle(HANDLE handle, TrackedHandle* out) {
    ::EnterCriticalSection(&g_stateLock);
    const bool found = findOpenHandleLocked(handle, out);
    ::LeaveCriticalSection(&g_stateLock);
    return found;
}

/// Removes exactly the record generation observed before the real close. A
/// failed close keeps it open; a reused numeric handle never matches its old
/// generation, so a late bookkeeping step cannot clear a live new record.
inline void markClosedGeneration(HANDLE handle, unsigned long long generation) {
    ::EnterCriticalSection(&g_stateLock);
    for (TrackedHandle& entry : g_tracked) {
        if (entry.open && entry.handle == handle && entry.generation == generation) {
            entry.open = false;
            break;
        }
    }
    ::LeaveCriticalSection(&g_stateLock);
}

/// Caller holds g_stateLock. The count is an observation of records that were
/// still marked open at the sample, never proof about the moment of failure.
inline int countOpenOnTargetLocked(const wchar_t* destination, wchar_t* detail,
                                   size_t detailCapacity) {
    int count = 0;
    if (detail != nullptr && detailCapacity > 0) detail[0] = 0;
    for (const TrackedHandle& entry : g_tracked) {
        if (!entry.open) continue;
        if (!isTraceTarget(entry.path)) continue;
        bool fallback = false;
        bool truncated = false;
        if (!sameFileIdentity(entry.path, destination, &fallback, &truncated)) {
            if (fallback) g_compareFallback.fetch_add(1);
            if (truncated) g_truncatedRecords.fetch_add(1);
            continue;
        }
        if (fallback) g_compareFallback.fetch_add(1);
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
    return count;
}

inline void logAttributes(const wchar_t* label, const wchar_t* path) {
    const DWORD attributes = ::GetFileAttributesW(path);
    logLine(L"%ls attrsObservedAfter=1 attrs=0x%08lX readOnly=%d exists=%d",
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
    if (fileName == nullptr || !isTraceTarget(fileName)) {
        return g_realCreateFileW(fileName, desiredAccess, shareMode, securityAttributes,
                                 creationDisposition, flagsAndAttributes, templateFile);
    }
    g_inFlightCreates.fetch_add(1);
    const HANDLE result = g_realCreateFileW(fileName, desiredAccess, shareMode,
                                            securityAttributes, creationDisposition,
                                            flagsAndAttributes, templateFile);
    const DWORD error = ::GetLastError();
    ::EnterCriticalSection(&g_stateLock);
    if (result != INVALID_HANDLE_VALUE) {
        trackOpenLocked(result, fileName, desiredAccess, shareMode);
    }
    ::LeaveCriticalSection(&g_stateLock);
    g_inFlightCreates.fetch_sub(1);
    logLine(L"CreateFileW path=\"%ls\" access=0x%08lX share=0x%08lX disposition=%lu "
            L"flags=0x%08lX handle=%p result=%ls error=%lu",
            fileName, static_cast<unsigned long>(desiredAccess),
            static_cast<unsigned long>(shareMode),
            static_cast<unsigned long>(creationDisposition),
            static_cast<unsigned long>(flagsAndAttributes), result,
            result != INVALID_HANDLE_VALUE ? L"ok" : L"failed",
            static_cast<unsigned long>(error));
    logAttributes(L"CreateFileW target", fileName);
    ::SetLastError(error);
    return result;
}

inline BOOL WINAPI hookCloseHandle(HANDLE handle) {
    const HookScope scope;
    if (!scope.active || g_realCloseHandle == nullptr) {
        return g_realCloseHandle != nullptr ? g_realCloseHandle(handle) : FALSE;
    }
    TrackedHandle tracked;
    const bool wasTracked = findOpenHandle(handle, &tracked);
    if (!wasTracked) {
        const BOOL result = g_realCloseHandle(handle);
        const DWORD error = ::GetLastError();
        ::SetLastError(error);
        return result;
    }
    g_inFlightCloses.fetch_add(1);
    const BOOL result = g_realCloseHandle(handle);
    const DWORD error = ::GetLastError();
    if (result) markClosedGeneration(handle, tracked.generation);
    g_inFlightCloses.fetch_sub(1);
    logLine(L"CloseHandle handle=%p path=\"%ls\" generation=%llu trackOpenBefore=1 "
            L"result=%ls error=%lu",
            handle, tracked.path, tracked.generation, result ? L"ok" : L"failed",
            static_cast<unsigned long>(error));
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
    bool targetTruncated = false;
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
        if (units > kMaxTrackedPath - 1) {
            units = kMaxTrackedPath - 1;
            targetTruncated = true;
            g_truncatedRecords.fetch_add(1);
        }
        if (rename->FileName != nullptr) {
            wcsncpy(renameTarget, rename->FileName, units);
            renameTarget[units] = 0;
        }
    }
    TrackedHandle tracked;
    const bool trackedHandle = isRename && findOpenHandle(handle, &tracked);
    const BOOL result = g_realSetFileInformationByHandle(handle, infoClass, info, infoSize);
    const DWORD error = ::GetLastError();
    if (isRename) {
        wchar_t ownOpen[768];
        int openCount = 0;
        ::EnterCriticalSection(&g_stateLock);
        openCount = countOpenOnTargetLocked(renameTarget, ownOpen, 768);
        ::LeaveCriticalSection(&g_stateLock);
        const int dropped = g_droppedHandles.load();
        const int truncated = g_truncatedRecords.load();
        const int fallback = g_compareFallback.load();
        const int inFlightCreates = g_inFlightCreates.load();
        const int inFlightCloses = g_inFlightCloses.load();
        const int logLost = g_logLost.load();
        const bool setComplete = dropped == 0 && truncated == 0 && fallback == 0 &&
                                 inFlightCreates == 0 && inFlightCloses == 0 &&
                                 logLost == 0;
        const int bookkeepingOverlap =
            (inFlightCreates + inFlightCloses) > 0 ? 1 : 0;
        wchar_t ownSummary[1024];
        if (setComplete) {
            std::swprintf(ownSummary, 1024,
                          L"%d observedAfterRename (record set complete; sample after the "
                          L"real rename, not causal)",
                          openCount);
        } else {
            std::swprintf(ownSummary, 1024,
                          L"UNKNOWN lowerBound=%d dropped=%d truncated=%d pathFallback=%d "
                          L"inFlightCreates=%d inFlightCloses=%d logLoss=%d (not causal)",
                          openCount, dropped, truncated, fallback, inFlightCreates,
                          inFlightCloses, logLost);
        }
        if (isTraceTarget(renameTarget) || trackedHandle || targetTruncated) {
            logLine(L"SetFileInformationByHandle RENAME handle=%p source=\"%ls\" "
                    L"target=\"%ls\" targetTruncated=%d replaceExisting=%d size=%lu "
                    L"result=%ls error=%lu bookkeepingOverlap=%d recordSet=%ls "
                    L"ownOpenObservedAfter=%ls %ls "
                    L"zeroMeaning=no-own-handle-observed-after-rename",
                    handle, trackedHandle ? tracked.path : L"(untracked)", renameTarget,
                    targetTruncated ? 1 : 0, replaceExisting ? 1 : 0,
                    static_cast<unsigned long>(infoSize), result ? L"ok" : L"failed",
                    static_cast<unsigned long>(error), bookkeepingOverlap,
                    setComplete ? L"COMPLETE" : L"UNKNOWN", ownSummary, ownOpen);
            logAttributes(L"RENAME target", renameTarget);
        }
    } else if (trackedHandle) {
        logLine(L"SetFileInformationByHandle handle=%p class=%d path=\"%ls\" result=%ls "
                L"error=%lu",
                handle, static_cast<int>(infoClass), tracked.path,
                result ? L"ok" : L"failed", static_cast<unsigned long>(error));
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

struct ImageBounds {
    BYTE* base = nullptr;
    size_t size = 0;
};

inline bool imageBounds(HMODULE module, ImageBounds* out) {
    if (module == nullptr || out == nullptr) return false;
    MEMORY_BASIC_INFORMATION first{};
    if (::VirtualQuery(module, &first, sizeof(first)) != sizeof(first)) return false;
    auto* allocationBase = reinterpret_cast<BYTE*>(first.AllocationBase);
    if (allocationBase == nullptr) return false;
    // Walk every contiguous committed MEM_IMAGE region of the allocation, not
    // just the first region VirtualQuery reports: headers, .rdata and .idata
    // commonly have different protections and sizes.
    size_t imageSize = 0;
    BYTE* cursor = allocationBase;
    while (true) {
        MEMORY_BASIC_INFORMATION region{};
        if (::VirtualQuery(cursor, &region, sizeof(region)) != sizeof(region)) return false;
        if (reinterpret_cast<BYTE*>(region.AllocationBase) != allocationBase) break;
        if (region.State != MEM_COMMIT || region.Type != MEM_IMAGE) break;
        if ((region.Protect & PAGE_NOACCESS) != 0 || (region.Protect & PAGE_GUARD) != 0)
            return false;
        if (reinterpret_cast<BYTE*>(region.BaseAddress) != cursor) return false;
        imageSize += static_cast<size_t>(region.RegionSize);
        cursor += region.RegionSize;
        if (imageSize > static_cast<size_t>(512) * 1024 * 1024) return false;
    }
    if (imageSize < sizeof(IMAGE_DOS_HEADER)) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(allocationBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew < 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS) > imageSize) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(allocationBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }
    const size_t declared = static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
    if (declared < static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS)) return false;
    out->base = allocationBase;
    out->size = declared < imageSize ? declared : imageSize;
    return true;
}

inline bool contains(const ImageBounds& bounds, size_t offset, size_t length) {
    return offset <= bounds.size && length <= bounds.size - offset;
}

inline bool readImportName(const ImageBounds& bounds, size_t rva, char* out, size_t capacity) {
    if (out == nullptr || capacity < 2 || !contains(bounds, rva, 1)) return false;
    const char* text = reinterpret_cast<const char*>(bounds.base + rva);
    const size_t available = bounds.size - rva;
    const size_t limit = available < capacity - 1 ? available : capacity - 1;
    for (size_t index = 0; index < limit; ++index) {
        out[index] = text[index];
        if (text[index] == '\0') return index > 0;
    }
    return false;
}

inline ModulePatchResult patchModule(HMODULE module, const wchar_t* moduleLabel) {
    ModulePatchResult result;
    ImageBounds bounds;
    if (!imageBounds(module, &bounds)) {
        result.failed = true;
        logLine(L"PATCH module=%ls result=FAILED reason=unreadable-image", moduleLabel);
        return result;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bounds.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(bounds.base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size == 0) {
        logLine(L"PATCH module=%ls result=no-import-directory coverage=UNKNOWN", moduleLabel);
        return result;
    }
    const size_t descriptorCount = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    if (descriptorCount == 0 ||
        !contains(bounds, directory.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
        result.failed = true;
        logLine(L"PATCH module=%ls result=FAILED reason=import-directory-out-of-image",
                moduleLabel);
        return result;
    }
    bool stop = false;
    for (size_t descriptorIndex = 0; descriptorIndex < descriptorCount && !stop;
         ++descriptorIndex) {
        const size_t descriptorOffset =
            directory.VirtualAddress + descriptorIndex * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        if (!contains(bounds, descriptorOffset, sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
            result.failed = true;
            stop = true;
            logLine(L"PATCH module=%ls result=FAILED reason=descriptor-out-of-image",
                    moduleLabel);
            break;
        }
        const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
            bounds.base + descriptorOffset);
        if (descriptor->Name == 0) break;
        if (!contains(bounds, descriptor->Name, 1)) {
            result.failed = true;
            stop = true;
            logLine(L"PATCH module=%ls result=FAILED reason=dll-name-out-of-image",
                    moduleLabel);
            break;
        }
        char dllName[kMaxImportName];
        if (!readImportName(bounds, descriptor->Name, dllName, sizeof(dllName))) {
            result.failed = true;
            stop = true;
            logLine(L"PATCH module=%ls result=FAILED reason=dll-name-unbounded",
                    moduleLabel);
            break;
        }
        if (descriptor->OriginalFirstThunk == 0) continue;
        if (!contains(bounds, descriptor->FirstThunk, sizeof(IMAGE_THUNK_DATA)) ||
            !contains(bounds, descriptor->OriginalFirstThunk, sizeof(IMAGE_THUNK_DATA))) {
            result.failed = true;
            stop = true;
            logLine(L"PATCH module=%ls result=FAILED reason=thunk-array-out-of-image",
                    moduleLabel);
            break;
        }
        for (size_t thunkIndex = 0;; ++thunkIndex) {
            const size_t thunkOffset =
                descriptor->FirstThunk + thunkIndex * sizeof(IMAGE_THUNK_DATA);
            const size_t nameOffset =
                descriptor->OriginalFirstThunk + thunkIndex * sizeof(IMAGE_THUNK_DATA);
            if (!contains(bounds, thunkOffset, sizeof(IMAGE_THUNK_DATA)) ||
                !contains(bounds, nameOffset, sizeof(IMAGE_THUNK_DATA))) {
                result.failed = true;
                stop = true;
                logLine(L"PATCH module=%ls result=FAILED reason=thunk-entry-out-of-image",
                        moduleLabel);
                break;
            }
            auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(bounds.base + thunkOffset);
            auto* nameThunk =
                reinterpret_cast<IMAGE_THUNK_DATA*>(bounds.base + nameOffset);
            if (nameThunk->u1.AddressOfData == 0) break;
            if ((nameThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) != 0) continue;
            // AddressOfData names IMAGE_IMPORT_BY_NAME, whose first member is
            // the 2-byte Hint; the API name starts after it.
            const size_t importByNameRva = static_cast<size_t>(nameThunk->u1.AddressOfData);
            const size_t hintOffset = offsetof(IMAGE_IMPORT_BY_NAME, Name);
            if (!contains(bounds, importByNameRva, hintOffset + 1)) {
                result.failed = true;
                stop = true;
                logLine(L"PATCH module=%ls result=FAILED reason=import-name-out-of-image",
                        moduleLabel);
                break;
            }
            char functionName[kMaxImportName];
            if (!readImportName(bounds, importByNameRva + hintOffset, functionName,
                                sizeof(functionName))) {
                result.failed = true;
                stop = true;
                logLine(L"PATCH module=%ls result=FAILED reason=import-name-unbounded",
                        moduleLabel);
                break;
            }
            FARPROC replacement = nullptr;
            int* coverageSlot = nullptr;
            if (std::strcmp(functionName, "CreateFileW") == 0) {
                replacement = reinterpret_cast<FARPROC>(&hookCreateFileW);
                coverageSlot = &result.coverage.createFileW;
            } else if (std::strcmp(functionName, "CloseHandle") == 0) {
                replacement = reinterpret_cast<FARPROC>(&hookCloseHandle);
                coverageSlot = &result.coverage.closeHandle;
            } else if (std::strcmp(functionName, "SetFileInformationByHandle") == 0) {
                replacement = reinterpret_cast<FARPROC>(&hookSetFileInformationByHandle);
                coverageSlot = &result.coverage.setFileInformation;
            } else if (std::strcmp(functionName, "SetFileAttributesW") == 0) {
                replacement = reinterpret_cast<FARPROC>(&hookSetFileAttributesW);
                coverageSlot = &result.coverage.setFileAttributes;
            }
            if (replacement == nullptr || coverageSlot == nullptr) continue;
            ++*coverageSlot;
            auto* slot = reinterpret_cast<FARPROC*>(&thunk->u1.Function);
            if (*slot == replacement) continue;
            DWORD previousProtection = 0;
            if (::VirtualProtect(slot, sizeof(FARPROC), PAGE_EXECUTE_READWRITE,
                                 &previousProtection) == FALSE) {
                result.failed = true;
                stop = true;
                logLine(L"PATCH module=%ls function=%hs result=FAILED reason=protect-failed",
                        moduleLabel, functionName);
                break;
            }
            *slot = replacement;
            DWORD restoredProtection = 0;
            if (::VirtualProtect(slot, sizeof(FARPROC), previousProtection,
                                 &restoredProtection) == FALSE) {
                result.failed = true;
                stop = true;
                logLine(L"PATCH module=%ls function=%hs result=FAILED "
                        L"reason=restore-protection-failed previous=0x%08lX",
                        moduleLabel, functionName,
                        static_cast<unsigned long>(previousProtection));
                break;
            }
            ++result.patched;
        }
    }
    logLine(L"PATCH module=%ls patched=%d createFileW=%d closeHandle=%d "
            L"setFileInformation=%d setFileAttributes=%d result=%ls",
            moduleLabel, result.patched, result.coverage.createFileW,
            result.coverage.closeHandle, result.coverage.setFileInformation,
            result.coverage.setFileAttributes, result.failed ? L"failed" : L"ok");
    return result;
}

}  // namespace detail

/// Installs the trace. The output path is policy-checked before anything is
/// opened (local absolute `.log`, existing non-reparse parent, outside the
/// native/animejanai stores, never the registry). The output handle is opened
/// before any import is patched. Returns false with lastError() set when the
/// path is refused, the log cannot be opened, a bound or protection restoration
/// fails, data was lost while writing the trace, or required per-module import
/// coverage is incomplete.
inline bool install(const wchar_t* outputPath) {
    using namespace detail;
    g_installError[0] = 0;
    wchar_t normalizedOutput[kMaxTrackedPath];
    if (!tracePathAllowed(outputPath, normalizedOutput, kMaxTrackedPath)) return false;
    const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == nullptr) {
        setInstallError(L"kernel32.dll is not loaded");
        return false;
    }
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
        setInstallError(L"one or more kernel32 hooks could not be resolved");
        return false;
    }
    if (!g_locksInitialized) {
        ::InitializeCriticalSection(&g_stateLock);
        ::InitializeCriticalSection(&g_logLock);
        g_locksInitialized = true;
    }
    g_log = g_realCreateFileW(normalizedOutput, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log == INVALID_HANDLE_VALUE) {
        setInstallError(L"the trace log could not be created at the given path");
        return false;
    }
    logLine(L"# native file trace pid=%lu target=%ls",
            static_cast<unsigned long>(::GetCurrentProcessId()), kLevelFile);
    logLine(L"# filters: %ls and %ls<random>", kLevelFile, kLevelTempPrefix);

    const ModulePatchResult own =
        patchModule(::GetModuleHandleW(nullptr), L"executable");
    const HMODULE qtCore = ::GetModuleHandleW(L"Qt6Core.dll");
    const HMODULE qtCoreDebug = ::GetModuleHandleW(L"Qt6Cored.dll");
    const HMODULE qtModule = qtCore != nullptr ? qtCore : qtCoreDebug;
    const ModulePatchResult qt =
        qtModule != nullptr ? patchModule(qtModule, L"Qt6Core.dll")
                            : ModulePatchResult{0, {}, true};
    if (qtModule == nullptr) {
        logLine(L"PATCH module=Qt6Core.dll result=FAILED reason=module-not-loaded");
    }

    const bool coverageComplete = own.coverage.createFileW > 0 &&
                                  own.coverage.closeHandle > 0 &&
                                  qt.coverage.createFileW > 0 &&
                                  qt.coverage.closeHandle > 0 &&
                                  qt.coverage.setFileInformation > 0;
    logLine(L"# coverage createFileW own=%d patched=%d qt=%d qtPatched=%d "
            L"sfiQt=%d closeOwn=%d closeQt=%d dropped=%d truncated=%d logLoss=%d "
            L"status=%ls",
            own.coverage.createFileW, own.patched, qt.coverage.createFileW, qt.patched,
            qt.coverage.setFileInformation, own.coverage.closeHandle,
            qt.coverage.closeHandle, g_droppedHandles.load(),
            g_truncatedRecords.load(), g_logLost.load(),
            coverageComplete ? L"COMPLETE" : L"INCOMPLETE");
    if (g_logLost.load() > 0) {
        setInstallError(L"the trace log lost a line; absence claims are UNKNOWN");
        return false;
    }
    if (!coverageComplete) {
        setInstallError(L"required import coverage is incomplete; own-handle absence "
                        L"claims from this trace are UNKNOWN");
        return false;
    }
    if (own.failed || qt.failed) {
        setInstallError(L"an import patch bound/restore check failed; absence claims "
                        L"are UNKNOWN");
        return false;
    }
    logLine(L"%ls", kRenameSeparator);
    ::SetLastError(0);
    return true;
}

#else  // !Q_OS_WIN

inline bool install(const wchar_t*) { return false; }
inline const wchar_t* lastError() { return L"native trace is Windows-only"; }

#endif  // Q_OS_WIN

}  // namespace mikai::probes::native_trace
