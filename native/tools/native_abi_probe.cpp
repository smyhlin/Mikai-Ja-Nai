/*
 * native_abi_probe — hand-run ABI check for the produced aji.dll.
 *
 * Default mode (one absolute DLL path) loads the dispatcher and exercises the
 * native v2 snapshot/control refusal contract: no-context, protocol mismatch,
 * unknown action, and the malformed-size canary. It resolves every symbol at
 * run time and links no CUDA, TensorRT or Qt library, so it runs on a machine
 * without the GPU SDK.
 *
 * Optional real-context mode (--real-context) additionally drives the public
 * ABI-8 lifecycle on a GPU the caller names by UUID:
 *   - nvcuda.dll is still resolved at run time (no link dependency); the
 *     device is selected by a real cuDeviceGetUuid query, never device 0;
 *   - that device's primary context is retained with the driver API and handed
 *     to aji_create through aji_create_params.cuda_context;
 *   - a resize-only chain (a resize step and no model, hence no engine) is
 *     configured from a probe-owned configuration file, one real NV12 frame is
 *     completed through aji_infer/aji_flush/aji_wait/aji_done, and the v2
 *     snapshot must still report Unavailable with no effective, prepared or
 *     active claim even though the legacy context is active and a real ticket
 *     completed;
 *   - drain, suspend and resume are exercised through the real context and must
 *     answer truthfully (acknowledged only when the backend said so), with no
 *     ready intermediate ever reported.
 * The caller names the backend library and the TensorRT runtime directory
 * explicitly; the runtime directory is registered with the loader exactly as
 * the player does, so the backend's imports resolve from it (the process PATH
 * is never consulted). Missing prerequisites are refused by name, never
 * guessed or defaulted.
 * This is a status/ABI probe only: it makes no claim about inference fidelity.
 */

#include "aji.h"
#include "aji_native.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace {
using SnapshotFn = int (*)(aji_ctx *, uint32_t, aji_native_snapshot_v2 *);
using ControlFn = int (*)(aji_ctx *, uint32_t, uint32_t, uint64_t, uint32_t,
                          aji_native_snapshot_v2 *);

struct GuardedSnapshot {
    uint64_t before;
    aji_native_snapshot_v2 value;
    uint64_t after;
};

bool unavailable(const aji_native_snapshot_v2 &snapshot, uint32_t result,
                 uint32_t requestedProtocol) {
    if (snapshot.struct_size != sizeof(snapshot) || snapshot.result != result ||
        snapshot.requested_protocol_version != requestedProtocol ||
        snapshot.protocol_version != AJI_NATIVE_PROTOCOL_VERSION ||
        snapshot.schema_version != AJI_NATIVE_SCHEMA_VERSION ||
        snapshot.aji_abi_version != 8u || snapshot.state != AJI_NATIVE_STATE_UNAVAILABLE ||
        snapshot.capability_bits != 0 || snapshot.effective_slot != 0 ||
        snapshot.stage_count != 0)
        return false;
    for (uint32_t i = 0; i < AJI_NATIVE_MAX_STAGES; ++i) {
        if (snapshot.engine_sha256[i][0] || snapshot.model_sha256[i][0] ||
            snapshot.prepared_build_keys[i][0])
            return false;
    }
    return true;
}

bool malformedSizesAreUntouched(SnapshotFn getSnapshot, ControlFn control) {
    GuardedSnapshot guarded{};
    std::memset(&guarded, 0xA5, sizeof(guarded));
    guarded.value.struct_size = sizeof(guarded.value) - 1u;
    unsigned char before[sizeof(guarded)];
    std::memcpy(before, &guarded, sizeof(guarded));
    if (getSnapshot(nullptr, AJI_NATIVE_PROTOCOL_VERSION, &guarded.value) !=
            AJI_NATIVE_INVALID_ARGUMENT ||
        std::memcmp(&guarded, before, sizeof(guarded)) != 0)
        return false;

    guarded.value.struct_size = sizeof(guarded.value) - 1u;
    unsigned char beforeControl[sizeof(guarded)];
    std::memcpy(beforeControl, &guarded, sizeof(guarded));
    if (control(nullptr, AJI_NATIVE_PROTOCOL_VERSION, AJI_NATIVE_CONTROL_DRAIN, 0, 0,
                &guarded.value) != AJI_NATIVE_INVALID_ARGUMENT ||
        std::memcmp(&guarded, beforeControl, sizeof(guarded)) != 0)
        return false;

    guarded.value.struct_size = sizeof(guarded.value) + 1u;
    unsigned char beforeOversize[sizeof(guarded)];
    std::memcpy(beforeOversize, &guarded, sizeof(guarded));
    if (getSnapshot(nullptr, AJI_NATIVE_PROTOCOL_VERSION, &guarded.value) !=
            AJI_NATIVE_INVALID_ARGUMENT ||
        std::memcmp(&guarded, beforeOversize, sizeof(guarded)) != 0)
        return false;
    guarded.value.struct_size = sizeof(guarded.value) + 1u;
    std::memcpy(beforeOversize, &guarded, sizeof(guarded));
    if (control(nullptr, AJI_NATIVE_PROTOCOL_VERSION, AJI_NATIVE_CONTROL_DRAIN, 0, 0,
                &guarded.value) != AJI_NATIVE_INVALID_ARGUMENT ||
        std::memcmp(&guarded, beforeOversize, sizeof(guarded)) != 0)
        return false;
    return true;
}

#ifdef _WIN32
const wchar_t *winErrorName(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND: return L"ERROR_FILE_NOT_FOUND";
    case ERROR_PATH_NOT_FOUND: return L"ERROR_PATH_NOT_FOUND";
    case ERROR_ACCESS_DENIED: return L"ERROR_ACCESS_DENIED";
    case ERROR_BAD_EXE_FORMAT: return L"ERROR_BAD_EXE_FORMAT";
    case ERROR_MOD_NOT_FOUND: return L"ERROR_MOD_NOT_FOUND";
    case ERROR_INVALID_PARAMETER: return L"ERROR_INVALID_PARAMETER";
    default: return L"WIN32_ERROR";
    }
}

void reportWinError(const wchar_t *operation, const std::filesystem::path &path,
                    DWORD error) {
    wchar_t message[512] = {};
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, error, 0, message,
                                      static_cast<DWORD>(sizeof(message) / sizeof(message[0])),
                                      nullptr);
    std::fwprintf(stderr, L"native_abi_probe: %ls failed for %ls: %ls (%lu): %ls\n",
                  operation, path.c_str(), winErrorName(error), error,
                  size ? message : L"no system message");
}

// Walks every component of an absolute path, refusing reparse points anywhere
// and requiring the final component to be a file (or a directory) as asked.
// Nothing is followed, so a junction cannot redirect the probe at a foreign
// binary or directory.
bool walkPlainAbsolute(const std::filesystem::path &input, bool wantDirectory,
                       std::filesystem::path *normalized) {
    if (!input.is_absolute()) {
        std::fwprintf(stderr, L"native_abi_probe: path must be absolute: %ls\n",
                      input.c_str());
        return false;
    }
    *normalized = input.lexically_normal();
    std::filesystem::path current = normalized->root_path();
    const DWORD rootAttributes = GetFileAttributesW(current.c_str());
    if (rootAttributes == INVALID_FILE_ATTRIBUTES) {
        reportWinError(L"GetFileAttributesW", current, GetLastError());
        return false;
    }
    if ((rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        std::fwprintf(stderr, L"native_abi_probe: reparse root refused: %ls\n", current.c_str());
        return false;
    }
    const auto relative = normalized->relative_path();
    for (auto it = relative.begin(); it != relative.end(); ++it) {
        current /= *it;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            reportWinError(L"GetFileAttributesW", current, GetLastError());
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            std::fwprintf(stderr, L"native_abi_probe: reparse path component refused: %ls\n",
                          current.c_str());
            return false;
        }
        const bool last = std::next(it) == relative.end();
        if (!last && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            std::fwprintf(stderr, L"native_abi_probe: non-directory path component: %ls\n",
                          current.c_str());
            return false;
        }
        if (last) {
            const bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!wantDirectory && isDirectory) {
                std::fwprintf(stderr, L"native_abi_probe: path is a directory: %ls\n",
                              current.c_str());
                return false;
            }
            if (wantDirectory && !isDirectory) {
                std::fwprintf(stderr, L"native_abi_probe: path is not a directory: %ls\n",
                              current.c_str());
                return false;
            }
        }
    }
    return true;
}

bool plainAbsoluteFile(const std::filesystem::path &input,
                       std::filesystem::path *normalized) {
    return walkPlainAbsolute(input, false, normalized);
}

bool plainAbsoluteDirectory(const std::filesystem::path &input,
                            std::filesystem::path *normalized) {
    return walkPlainAbsolute(input, true, normalized);
}

void *openModule(const std::filesystem::path &path) {
    return static_cast<void *>(LoadLibraryExW(
        path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32));
}
void *symbol(void *module, const char *name) {
    return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(module), name));
}
void closeModule(void *module) {
    if (module) FreeLibrary(static_cast<HMODULE>(module));
}
#else
void *openModule(const char *path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
void *symbol(void *module, const char *name) { return dlsym(module, name); }
void closeModule(void *module) { if (module) dlclose(module); }
#endif

int runProbe(void *module, const char *displayPath) {
    const auto getSnapshot = reinterpret_cast<SnapshotFn>(
        symbol(module, "aji_native_get_snapshot"));
    const auto control = reinterpret_cast<ControlFn>(
        symbol(module, "aji_native_control"));
    if (!getSnapshot || !control) {
        std::fprintf(stderr,
                     "native_abi_probe: %s lacks aji_native_get_snapshot or aji_native_control\n",
                     displayPath);
        return 1;
    }
    if (!malformedSizesAreUntouched(getSnapshot, control)) {
        std::fprintf(stderr, "native_abi_probe: malformed-size refusal modified caller memory\n");
        return 1;
    }

    aji_native_snapshot_v2 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    const int noContextRc = getSnapshot(nullptr, AJI_NATIVE_PROTOCOL_VERSION, &snapshot);
    if (noContextRc != AJI_NATIVE_NO_CONTEXT ||
        !unavailable(snapshot, AJI_NATIVE_NO_CONTEXT, AJI_NATIVE_PROTOCOL_VERSION)) {
        std::fprintf(stderr,
                     "native_abi_probe: no-context snapshot violated ABI v2 contract (rc=%d result=%u state=%u capabilities=%u effective=%u)\n",
                     noContextRc, snapshot.result, snapshot.state, snapshot.capability_bits,
                     snapshot.effective_slot);
        return 1;
    }

    snapshot = {};
    snapshot.struct_size = sizeof(snapshot);
    const uint32_t incompatible = AJI_NATIVE_PROTOCOL_VERSION + 1u;
    const int mismatchRc = getSnapshot(nullptr, incompatible, &snapshot);
    if (mismatchRc != AJI_NATIVE_PROTOCOL_MISMATCH ||
        !unavailable(snapshot, AJI_NATIVE_PROTOCOL_MISMATCH, incompatible)) {
        std::fprintf(stderr,
                     "native_abi_probe: protocol negotiation violated v2 contract (rc=%d result=%u requested=%u protocol=%u schema=%u)\n",
                     mismatchRc, snapshot.result, snapshot.requested_protocol_version,
                     snapshot.protocol_version, snapshot.schema_version);
        return 1;
    }

    snapshot = {};
    snapshot.struct_size = sizeof(snapshot);
    const int noContextControl = control(nullptr, AJI_NATIVE_PROTOCOL_VERSION,
                                         AJI_NATIVE_CONTROL_DRAIN, 1, 0, &snapshot);
    if (noContextControl != AJI_NATIVE_NO_CONTEXT ||
        !unavailable(snapshot, AJI_NATIVE_NO_CONTEXT, AJI_NATIVE_PROTOCOL_VERSION)) {
        std::fprintf(stderr,
                     "native_abi_probe: no-context control violated v2 contract (rc=%d result=%u state=%u capabilities=%u effective=%u)\n",
                     noContextControl, snapshot.result, snapshot.state,
                     snapshot.capability_bits, snapshot.effective_slot);
        return 1;
    }

    snapshot = {};
    snapshot.struct_size = sizeof(snapshot);
    const int invalidAction = control(nullptr, AJI_NATIVE_PROTOCOL_VERSION, UINT32_MAX,
                                      2, 0, &snapshot);
    if (invalidAction != AJI_NATIVE_INVALID_ARGUMENT ||
        !unavailable(snapshot, AJI_NATIVE_INVALID_ARGUMENT, AJI_NATIVE_PROTOCOL_VERSION)) {
        std::fprintf(stderr,
                     "native_abi_probe: invalid control action was not refused safely (rc=%d result=%u state=%u capabilities=%u effective=%u)\n",
                     invalidAction, snapshot.result, snapshot.state,
                     snapshot.capability_bits, snapshot.effective_slot);
        return 1;
    }

    std::printf("native ABI-8 snapshot v2: present; protocol/schema %u/%u; refusal snapshots are Unavailable; malformed sizes are untouched\n",
                AJI_NATIVE_PROTOCOL_VERSION, AJI_NATIVE_SCHEMA_VERSION);
    return 0;
}

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Optional real-context scenario
// ---------------------------------------------------------------------------

using CreateFn = aji_ctx *(*)(const aji_create_params *);
using ConfigureFn = int (*)(aji_ctx *, int, int, double, int *, int *);
using InferFn = int (*)(aji_ctx *, const aji_frame *, const aji_frame *, void *);
using FlushFn = uint64_t (*)(aji_ctx *, void *);
using DoneFn = int (*)(aji_ctx *, uint64_t);
using WaitFn = int (*)(aji_ctx *, uint64_t);
using LastErrorFn = const char *(*)(aji_ctx *);
using DestroyFn = void (*)(aji_ctx **);

constexpr int kProbeWidth = 64;
constexpr int kProbeHeight = 64;
constexpr double kProbeFps = 24.0;
constexpr uint32_t kControlTimeoutMs = 2000u;
constexpr uint64_t kGuardBefore = 0x5A5A5A5A5A5A5A5Aull;
constexpr uint64_t kGuardAfter = 0xA5A5A5A5A5A5A5A5ull;

// A chain with a resize step and no model: aji_configure activates it and
// allocates its resample plan, but no engine exists, so the context is a real,
// legacy-active configuration whose plan identity cannot be verified.
constexpr char kProbeConf[] =
    "; Generated by native_abi_probe for the optional real-context scenario.\n"
    "; chain_1_model_1 has no name (no engine) and a 150% resize, so it is an\n"
    "; unverified resize-only configuration that must still snapshot Unavailable.\n"
    "[global]\n"
    "backend=TensorRT\n"
    "logging=yes\n"
    "\n"
    "[slot_1]\n"
    "profile_name=Probe Resize Only\n"
    "chain_1_min_resolution=0x0\n"
    "chain_1_max_resolution=infxinf\n"
    "chain_1_min_fps=0\n"
    "chain_1_max_fps=inf\n"
    "chain_1_model_1_name=\n"
    "chain_1_model_1_resize_factor_before_upscale=150\n";

std::wstring widen(const char *utf8) {
    if (!utf8 || !*utf8) return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (length <= 1) return std::wstring();
    std::wstring out(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), length);
    return out;
}

std::string narrowUtf8(const std::wstring &wide) {
    if (wide.empty()) return std::string();
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                           static_cast<int>(wide.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) return std::string();
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        out.data(), length, nullptr, nullptr);
    return out;
}

size_t boundedLength(const char *text, size_t capacity) {
    size_t length = 0;
    while (length < capacity && text[length] != '\0') ++length;
    return length;
}

template <size_t N>
std::wstring textOf(const char (&text)[N]) {
    return widen(std::string(text, boundedLength(text, N)).c_str());
}

bool equalsNoCase(const std::wstring &a, const std::wstring &b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool containsNoCase(const char *haystack, const char *needle) {
    const size_t length = std::strlen(needle);
    if (length == 0) return true;
    for (const char *start = haystack; *start; ++start) {
        size_t index = 0;
        while (index < length && start[index] != '\0' &&
               std::tolower(static_cast<unsigned char>(start[index])) ==
                   std::tolower(static_cast<unsigned char>(needle[index])))
            ++index;
        if (index == length) return true;
    }
    return false;
}

int refuse(const std::wstring &message, int code) {
    std::fwprintf(stderr, L"native_abi_probe: %ls\n", message.c_str());
    return code;
}

void probeLog(void *, int level, const char *message) {
    std::fprintf(stderr, "native_abi_probe: [backend %d] %s\n", level,
                 message ? message : "");
}

// Minimal CUDA driver ABI, declared here so the probe never needs cuda.h or a
// link dependency on the toolkit. Every entry point is resolved from nvcuda.dll
// at run time; the versioned spellings are tried first.
using CUresult = int;
using CUdevice = int;
using CUdeviceptr = unsigned long long;
struct CUuuid { char bytes[16]; };
struct CUctx_st;
using CUcontext = CUctx_st *;

constexpr CUresult kCudaSuccess = 0;

void *driverSymbol(HMODULE module, const char *preferred, const char *fallback) {
    void *resolved = reinterpret_cast<void *>(GetProcAddress(module, preferred));
    if (!resolved && fallback)
        resolved = reinterpret_cast<void *>(GetProcAddress(module, fallback));
    return resolved;
}

struct CudaDriver {
    HMODULE module = nullptr;
    CUresult(__stdcall *init)(unsigned int) = nullptr;
    CUresult(__stdcall *deviceGetCount)(int *) = nullptr;
    CUresult(__stdcall *deviceGet)(CUdevice *, int) = nullptr;
    CUresult(__stdcall *deviceGetUuid)(CUuuid *, CUdevice) = nullptr;
    CUresult(__stdcall *deviceGetName)(char *, int, CUdevice) = nullptr;
    CUresult(__stdcall *primaryCtxRetain)(CUcontext *, CUdevice) = nullptr;
    CUresult(__stdcall *primaryCtxRelease)(CUdevice) = nullptr;
    CUresult(__stdcall *ctxGetCurrent)(CUcontext *) = nullptr;
    CUresult(__stdcall *ctxSetCurrent)(CUcontext) = nullptr;
    CUresult(__stdcall *memAlloc)(CUdeviceptr *, size_t) = nullptr;
    CUresult(__stdcall *memFree)(CUdeviceptr) = nullptr;
    CUresult(__stdcall *memcpyHtoD)(CUdeviceptr, const void *, size_t) = nullptr;
    CUresult(__stdcall *driverGetVersion)(int *) = nullptr;
    CUresult(__stdcall *getErrorName)(CUresult, const char **) = nullptr;
    CUresult(__stdcall *getErrorString)(CUresult, const char **) = nullptr;

    CudaDriver() = default;
    CudaDriver(const CudaDriver &) = delete;
    CudaDriver &operator=(const CudaDriver &) = delete;
    ~CudaDriver() {
        if (module) FreeLibrary(module);
    }

    bool load(std::wstring *why) {
        module = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            *why = L"nvcuda.dll is not loadable from System32 (error " +
                   std::to_wstring(GetLastError()) + L")";
            return false;
        }
        init = reinterpret_cast<decltype(init)>(driverSymbol(module, "cuInit", nullptr));
        deviceGetCount = reinterpret_cast<decltype(deviceGetCount)>(
            driverSymbol(module, "cuDeviceGetCount", nullptr));
        deviceGet = reinterpret_cast<decltype(deviceGet)>(
            driverSymbol(module, "cuDeviceGet", nullptr));
        deviceGetUuid = reinterpret_cast<decltype(deviceGetUuid)>(
            driverSymbol(module, "cuDeviceGetUuid_v2", "cuDeviceGetUuid"));
        deviceGetName = reinterpret_cast<decltype(deviceGetName)>(
            driverSymbol(module, "cuDeviceGetName", nullptr));
        primaryCtxRetain = reinterpret_cast<decltype(primaryCtxRetain)>(
            driverSymbol(module, "cuDevicePrimaryCtxRetain", nullptr));
        primaryCtxRelease = reinterpret_cast<decltype(primaryCtxRelease)>(
            driverSymbol(module, "cuDevicePrimaryCtxRelease_v2", "cuDevicePrimaryCtxRelease"));
        ctxGetCurrent = reinterpret_cast<decltype(ctxGetCurrent)>(
            driverSymbol(module, "cuCtxGetCurrent", nullptr));
        ctxSetCurrent = reinterpret_cast<decltype(ctxSetCurrent)>(
            driverSymbol(module, "cuCtxSetCurrent", nullptr));
        memAlloc = reinterpret_cast<decltype(memAlloc)>(
            driverSymbol(module, "cuMemAlloc_v2", "cuMemAlloc"));
        memFree = reinterpret_cast<decltype(memFree)>(
            driverSymbol(module, "cuMemFree_v2", "cuMemFree"));
        memcpyHtoD = reinterpret_cast<decltype(memcpyHtoD)>(
            driverSymbol(module, "cuMemcpyHtoD_v2", "cuMemcpyHtoD"));
        driverGetVersion = reinterpret_cast<decltype(driverGetVersion)>(
            driverSymbol(module, "cuDriverGetVersion", nullptr));
        getErrorName = reinterpret_cast<decltype(getErrorName)>(
            driverSymbol(module, "cuGetErrorName", nullptr));
        getErrorString = reinterpret_cast<decltype(getErrorString)>(
            driverSymbol(module, "cuGetErrorString", nullptr));
        if (!init || !deviceGetCount || !deviceGet || !deviceGetUuid ||
            !primaryCtxRetain || !primaryCtxRelease || !ctxGetCurrent ||
            !ctxSetCurrent || !memAlloc || !memFree || !memcpyHtoD) {
            *why = L"nvcuda.dll is missing a required driver entry point";
            return false;
        }
        return true;
    }

    std::wstring text(CUresult result) const {
        std::wstring out = L"CUDA error " + std::to_wstring(result);
        const char *name = nullptr;
        if (getErrorName && getErrorName(result, &name) == kCudaSuccess && name && *name)
            out = widen(name);
        const char *detail = nullptr;
        if (getErrorString && getErrorString(result, &detail) == kCudaSuccess && detail && *detail) {
            out += L" (";
            out += widen(detail);
            out += L")";
        }
        return out;
    }

    bool copyToDevice(CUdeviceptr destination, const void *source, size_t bytes,
                      std::wstring *why) {
        const CUresult result = memcpyHtoD(destination, source, bytes);
        if (result != kCudaSuccess) {
            *why = L"cuMemcpyHtoD failed: " + text(result);
            return false;
        }
        return true;
    }
};

std::wstring uuidHex(const CUuuid &uuid) {
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring text;
    text.reserve(32);
    for (const char raw : uuid.bytes) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 0x0F]);
    }
    return text;
}

std::wstring uuidDashed(const CUuuid &uuid) {
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring text = L"GPU-";
    text.reserve(40);
    for (size_t i = 0; i < sizeof(uuid.bytes); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) text.push_back(L'-');
        const unsigned char byte = static_cast<unsigned char>(uuid.bytes[i]);
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 0x0F]);
    }
    return text;
}

bool parseUuid(const std::wstring &input, CUuuid *uuid, std::wstring *why) {
    const std::wstring invalid =
        L"--uuid \"" + input + L"\" is not a 16-byte CUDA UUID";
    std::wstring digits = input;
    if (digits.rfind(L"GPU-", 0) == 0) digits.erase(0, 4);
    std::memset(uuid, 0, sizeof(*uuid));
    int nibbles = 0;
    for (const wchar_t character : digits) {
        if (character == L'-' || character == L'{' || character == L'}') continue;
        int value = -1;
        if (character >= L'0' && character <= L'9') value = character - L'0';
        else if (character >= L'a' && character <= L'f') value = character - L'a' + 10;
        else if (character >= L'A' && character <= L'F') value = character - L'A' + 10;
        if (value < 0 || nibbles >= 32) {
            *why = invalid;
            return false;
        }
        unsigned char &byte = reinterpret_cast<unsigned char *>(uuid->bytes)[nibbles / 2];
        if (nibbles % 2 == 0) byte = static_cast<unsigned char>(value << 4);
        else byte = static_cast<unsigned char>(byte | value);
        ++nibbles;
    }
    if (nibbles != 32) {
        *why = invalid;
        return false;
    }
    return true;
}

bool selectDeviceByUuid(CudaDriver &driver, const CUuuid &requested, int *ordinal,
                        std::wstring *actual, std::wstring *why) {
    int count = 0;
    const CUresult counted = driver.deviceGetCount(&count);
    if (counted != kCudaSuccess) {
        *why = L"cuDeviceGetCount failed: " + driver.text(counted);
        return false;
    }
    if (count < 1) {
        *why = L"no CUDA device is present on this machine";
        return false;
    }
    std::wstring present;
    for (int index = 0; index < count; ++index) {
        CUdevice device = 0;
        if (driver.deviceGet(&device, index) != kCudaSuccess) continue;
        CUuuid uuid{};
        if (driver.deviceGetUuid(&uuid, device) != kCudaSuccess) continue;
        if (std::memcmp(&uuid, &requested, sizeof(CUuuid)) != 0) {
            present += L"\n  " + uuidDashed(uuid);
            continue;
        }
        *ordinal = index;
        *actual = uuidDashed(uuid);
        return true;
    }
    *why = L"--uuid " + uuidDashed(requested) +
           L" is not one of this machine's CUDA devices (" + std::to_wstring(count) +
           L" present):" + present;
    return false;
}

class PrimaryContext {
public:
    PrimaryContext() = default;
    PrimaryContext(const PrimaryContext &) = delete;
    PrimaryContext &operator=(const PrimaryContext &) = delete;
    ~PrimaryContext() {
        if (!driver_) return;
        if (active_) driver_->ctxSetCurrent(previous_);
        if (device_ >= 0) driver_->primaryCtxRelease(device_);
    }

    bool retain(CudaDriver &driver, int ordinal, std::wstring *why) {
        CUcontext context = nullptr;
        CUresult result = driver.primaryCtxRetain(&context, ordinal);
        if (result != kCudaSuccess) {
            *why = L"cuDevicePrimaryCtxRetain failed: " + driver.text(result);
            return false;
        }
        CUcontext previous = nullptr;
        result = driver.ctxGetCurrent(&previous);
        if (result != kCudaSuccess) {
            driver.primaryCtxRelease(ordinal);
            *why = L"cuCtxGetCurrent failed: " + driver.text(result);
            return false;
        }
        result = driver.ctxSetCurrent(context);
        if (result != kCudaSuccess) {
            driver.primaryCtxRelease(ordinal);
            *why = L"cuCtxSetCurrent failed: " + driver.text(result);
            return false;
        }
        driver_ = &driver;
        device_ = ordinal;
        context_ = context;
        previous_ = previous;
        active_ = true;
        return true;
    }

    void *context() const { return reinterpret_cast<void *>(context_); }

private:
    CudaDriver *driver_ = nullptr;
    int device_ = -1;
    CUcontext context_ = nullptr;
    CUcontext previous_ = nullptr;
    bool active_ = false;
};

class DeviceBuffer {
public:
    DeviceBuffer() = default;
    DeviceBuffer(const DeviceBuffer &) = delete;
    DeviceBuffer &operator=(const DeviceBuffer &) = delete;
    ~DeviceBuffer() {
        if (driver_ && pointer_) driver_->memFree(pointer_);
    }

    bool allocate(CudaDriver &driver, size_t bytes, const wchar_t *what,
                  std::wstring *why) {
        CUdeviceptr pointer = 0;
        const CUresult result = driver.memAlloc(&pointer, bytes);
        if (result != kCudaSuccess) {
            *why = std::wstring(what) + L" allocation failed: " + driver.text(result);
            return false;
        }
        driver_ = &driver;
        pointer_ = pointer;
        return true;
    }

    CUdeviceptr get() const { return pointer_; }
    void *asVoid() const {
        return reinterpret_cast<void *>(static_cast<uintptr_t>(pointer_));
    }

private:
    CudaDriver *driver_ = nullptr;
    CUdeviceptr pointer_ = 0;
};

// The probe's own scratch directory: created empty under %TEMP% with a unique
// name and removed on destruction. No caller or product path is written.
class ScopedTempDirectory {
public:
    ScopedTempDirectory() = default;
    ScopedTempDirectory(const ScopedTempDirectory &) = delete;
    ScopedTempDirectory &operator=(const ScopedTempDirectory &) = delete;
    ~ScopedTempDirectory() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    bool create(std::wstring *why) {
        std::error_code error;
        const std::filesystem::path base = std::filesystem::temp_directory_path(error);
        if (error) {
            *why = L"cannot resolve the temporary directory: " + widen(error.message().c_str());
            return false;
        }
        const std::wstring unique = std::to_wstring(GetCurrentProcessId()) + L"-" +
                                    std::to_wstring(GetTickCount64());
        for (int attempt = 0; attempt < 32; ++attempt) {
            const std::filesystem::path candidate =
                base / (L"mikai-native-abi-probe-" + unique + L"-" +
                        std::to_wstring(attempt));
            std::error_code created_error;
            if (std::filesystem::create_directory(candidate, created_error)) {
                path_ = candidate;
                return true;
            }
            if (created_error) {
                *why = L"cannot create the probe directory " + candidate.wstring() + L": " +
                       widen(created_error.message().c_str());
                return false;
            }
        }
        *why = L"cannot create a unique probe directory under " + base.wstring();
        return false;
    }

    const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
};

// Registers a caller-named runtime directory with the loader for the lifetime
// of the probe. The player resolves the backend's TensorRT imports the same
// way (AddDllDirectory, never the process PATH); the dispatcher's own backend
// load then finds them through LOAD_LIBRARY_SEARCH_DEFAULT_DIRS.
class RegisteredDllDirectory {
public:
    RegisteredDllDirectory() = default;
    RegisteredDllDirectory(const RegisteredDllDirectory &) = delete;
    RegisteredDllDirectory &operator=(const RegisteredDllDirectory &) = delete;
    ~RegisteredDllDirectory() {
        if (cookie_) RemoveDllDirectory(cookie_);
    }

    bool add(const std::filesystem::path &directory, std::wstring *why) {
        cookie_ = AddDllDirectory(directory.c_str());
        if (!cookie_) {
            *why = L"--runtime-dir cannot be registered with the loader (error " +
                   std::to_wstring(GetLastError()) + L"): " + directory.wstring();
            return false;
        }
        return true;
    }

private:
    HANDLE cookie_ = nullptr;
};

class AjiContextGuard {
public:
    AjiContextGuard(aji_ctx *context, DestroyFn destroy)
        : context_(context), destroy_(destroy) {}
    AjiContextGuard(const AjiContextGuard &) = delete;
    AjiContextGuard &operator=(const AjiContextGuard &) = delete;
    ~AjiContextGuard() {
        if (context_ && destroy_) destroy_(&context_);
    }
    aji_ctx *get() const { return context_; }

private:
    aji_ctx *context_ = nullptr;
    DestroyFn destroy_ = nullptr;
};

struct GuardedRealSnapshot {
    uint64_t before = kGuardBefore;
    aji_native_snapshot_v2 value{};
    uint64_t after = kGuardAfter;
    bool intact() const { return before == kGuardBefore && after == kGuardAfter; }
};

// The contract: an observation may only claim Unavailable, and must leave every
// effective/prepared field empty and carry a nonempty reason. Nothing about the
// legacy active flag or a completed ticket changes that.
bool truthfullyUnavailable(const aji_native_snapshot_v2 &snapshot,
                           uint32_t expectedResult, std::wstring *why) {
    const auto fail = [&](const wchar_t *detail) {
        *why = detail;
        return false;
    };
    if (snapshot.struct_size != sizeof(snapshot))
        return fail(L"struct_size does not match the v2 layout");
    if (snapshot.requested_protocol_version != AJI_NATIVE_PROTOCOL_VERSION)
        return fail(L"requested_protocol_version does not echo the request");
    if (snapshot.protocol_version != AJI_NATIVE_PROTOCOL_VERSION)
        return fail(L"protocol_version is not v2");
    if (snapshot.schema_version != AJI_NATIVE_SCHEMA_VERSION)
        return fail(L"schema_version is not v2");
    if (snapshot.aji_abi_version != 8u)
        return fail(L"aji_abi_version is not 8");
    if (snapshot.result != expectedResult)
        return fail(L"result does not match the call's return code");
    if (snapshot.state != AJI_NATIVE_STATE_UNAVAILABLE)
        return fail(L"state is not Unavailable");
    if (snapshot.effective_slot != 0)
        return fail(L"effective_slot is claimed");
    if (snapshot.output_width != 0 || snapshot.output_height != 0)
        return fail(L"effective output dimensions are claimed");
    if (snapshot.effective_plan[0] != '\0')
        return fail(L"effective_plan is claimed");
    for (uint32_t stage = 0; stage < AJI_NATIVE_MAX_STAGES; ++stage) {
        if (snapshot.prepared_build_keys[stage][0] != '\0')
            return fail(L"prepared_build_keys are claimed");
    }
    if (snapshot.error[0] == '\0')
        return fail(L"the reason text is empty");
    return true;
}

std::wstring lastErrorText(LastErrorFn lastError, aji_ctx *context) {
    if (!lastError || !context) return L"(no context)";
    const char *message = lastError(context);
    return message ? widen(message) : L"(no message)";
}

bool controlIsTruthful(ControlFn control, aji_ctx *context, uint32_t action,
                       uint64_t epoch, const wchar_t *phase, uint32_t *returned,
                       aji_native_snapshot_v2 *observed, std::wstring *failure) {
    GuardedRealSnapshot guarded;
    guarded.value.struct_size = sizeof(guarded.value);
    const int rc = control(context, AJI_NATIVE_PROTOCOL_VERSION, action, epoch,
                           kControlTimeoutMs, &guarded.value);
    *observed = guarded.value;
    *returned = static_cast<uint32_t>(rc);
    if (!guarded.intact()) {
        *failure = std::wstring(phase) + L": control wrote past the caller's buffer";
        return false;
    }
    if (rc != AJI_NATIVE_OK && rc != AJI_NATIVE_OPERATION_FAILED) {
        *failure = std::wstring(phase) + L": control returned an unexpected code " +
                   std::to_wstring(rc);
        return false;
    }
    std::wstring reason;
    if (!truthfullyUnavailable(guarded.value, static_cast<uint32_t>(rc), &reason)) {
        *failure = std::wstring(phase) + L": " + reason;
        return false;
    }
    return true;
}

struct RealContextOptions {
    std::filesystem::path backend;
    std::filesystem::path runtime_dir;
    std::wstring uuid;
};

int runRealContextScenario(void *module, const char *displayPath,
                           const RealContextOptions &options) {
    const auto getSnapshot = reinterpret_cast<SnapshotFn>(
        symbol(module, "aji_native_get_snapshot"));
    const auto control = reinterpret_cast<ControlFn>(
        symbol(module, "aji_native_control"));
    const auto create = reinterpret_cast<CreateFn>(symbol(module, "aji_create"));
    const auto configure = reinterpret_cast<ConfigureFn>(symbol(module, "aji_configure"));
    const auto infer = reinterpret_cast<InferFn>(symbol(module, "aji_infer"));
    const auto flush = reinterpret_cast<FlushFn>(symbol(module, "aji_flush"));
    const auto done = reinterpret_cast<DoneFn>(symbol(module, "aji_done"));
    const auto wait = reinterpret_cast<WaitFn>(symbol(module, "aji_wait"));
    const auto lastError = reinterpret_cast<LastErrorFn>(symbol(module, "aji_last_error"));
    const auto destroy = reinterpret_cast<DestroyFn>(symbol(module, "aji_destroy"));
    if (!getSnapshot || !control || !create || !configure || !infer || !flush ||
        !done || !wait || !destroy) {
        return refuse(L"real-context scenario: " + widen(displayPath) +
                          L" does not export the ABI-8 lifecycle and v2 snapshot/control symbols",
                      2);
    }

    CUuuid requested{};
    std::wstring why;
    if (!parseUuid(options.uuid, &requested, &why)) return refuse(why, 2);

    CudaDriver driver;
    if (!driver.load(&why)) return refuse(why, 2);
    CUresult status = driver.init(0);
    if (status != kCudaSuccess)
        return refuse(L"cuInit failed: " + driver.text(status), 2);

    int ordinal = -1;
    std::wstring deviceText;
    if (!selectDeviceByUuid(driver, requested, &ordinal, &deviceText, &why))
        return refuse(why, 2);

    PrimaryContext primary;
    if (!primary.retain(driver, ordinal, &why)) return refuse(why, 2);

    ScopedTempDirectory scratch;
    if (!scratch.create(&why)) return refuse(why, 2);
    const std::filesystem::path conf_path = scratch.path() / L"probe-resize-only.conf";
    {
        std::ofstream conf(conf_path, std::ios::binary | std::ios::trunc);
        conf << kProbeConf;
        conf.close();
        if (!conf)
            return refuse(L"cannot write the probe configuration " + conf_path.wstring(), 2);
    }
    const std::string conf_utf8 = narrowUtf8(conf_path.wstring());
    const std::string model_utf8 = narrowUtf8(scratch.path().wstring());

    aji_create_params params{};
    params.api_version = AJI_API_VERSION;
    params.cuda_context = primary.context();
    params.conf_path = conf_utf8.c_str();
    params.model_dir = model_utf8.c_str();
    params.slot = 1;
    params.log = &probeLog;
    params.log_opaque = nullptr;

    aji_ctx *created = create(&params);
    if (!created)
        return refuse(L"aji_create returned null for the resize-only configuration", 2);
    AjiContextGuard context(created, destroy);

    int out_w = 0;
    int out_h = 0;
    const int configured =
        configure(context.get(), kProbeWidth, kProbeHeight, kProbeFps, &out_w, &out_h);
    if (configured != 1)
        return refuse(L"aji_configure did not activate the resize-only chain (rc=" +
                          std::to_wstring(configured) + L"): " +
                          lastErrorText(lastError, context.get()),
                      1);
    if (out_w <= kProbeWidth || out_h <= kProbeHeight || (out_w & 1) || (out_h & 1))
        return refuse(L"the resize-only chain reported implausible output " +
                          std::to_wstring(out_w) + L"x" + std::to_wstring(out_h),
                      1);

    const size_t in_y_bytes = static_cast<size_t>(kProbeWidth) * kProbeHeight;
    const size_t in_uv_bytes = in_y_bytes / 2;
    const size_t out_y_bytes = static_cast<size_t>(out_w) * out_h;
    const size_t out_uv_bytes = out_y_bytes / 2;

    DeviceBuffer in_y_buffer;
    DeviceBuffer in_uv_buffer;
    DeviceBuffer out_y_buffer;
    DeviceBuffer out_uv_buffer;
    if (!in_y_buffer.allocate(driver, in_y_bytes, L"input luma", &why) ||
        !in_uv_buffer.allocate(driver, in_uv_bytes, L"input chroma", &why) ||
        !out_y_buffer.allocate(driver, out_y_bytes, L"output luma", &why) ||
        !out_uv_buffer.allocate(driver, out_uv_bytes, L"output chroma", &why))
        return refuse(why, 1);

    std::vector<unsigned char> host_y(in_y_bytes);
    std::vector<unsigned char> host_uv(in_uv_bytes);
    for (size_t i = 0; i < host_y.size(); ++i)
        host_y[i] = static_cast<unsigned char>((i * 31u + 7u) & 0xFFu);
    for (size_t i = 0; i < host_uv.size(); ++i)
        host_uv[i] = static_cast<unsigned char>(128u + ((i * 17u) & 0x3Fu));
    std::vector<unsigned char> zero(out_y_bytes + out_uv_bytes, 0);
    if (!driver.copyToDevice(in_y_buffer.get(), host_y.data(), in_y_bytes, &why) ||
        !driver.copyToDevice(in_uv_buffer.get(), host_uv.data(), in_uv_bytes, &why) ||
        !driver.copyToDevice(out_y_buffer.get(), zero.data(), out_y_bytes, &why) ||
        !driver.copyToDevice(out_uv_buffer.get(), zero.data(), out_uv_bytes, &why))
        return refuse(why, 1);

    aji_frame source{};
    source.width = kProbeWidth;
    source.height = kProbeHeight;
    source.format = AJI_FMT_NV12;
    source.matrix = AJI_MATRIX_BT709;
    source.range = AJI_RANGE_LIMITED;
    source.siting = AJI_SITING_LEFT;
    source.plane[0] = in_y_buffer.asVoid();
    source.plane[1] = in_uv_buffer.asVoid();
    source.stride[0] = static_cast<ptrdiff_t>(kProbeWidth);
    source.stride[1] = static_cast<ptrdiff_t>(kProbeWidth);

    aji_frame destination{};
    destination.width = out_w;
    destination.height = out_h;
    destination.format = AJI_FMT_NV12;
    destination.matrix = AJI_MATRIX_BT709;
    destination.range = AJI_RANGE_LIMITED;
    destination.siting = AJI_SITING_LEFT;
    destination.plane[0] = out_y_buffer.asVoid();
    destination.plane[1] = out_uv_buffer.asVoid();
    destination.stride[0] = static_cast<ptrdiff_t>(out_w);
    destination.stride[1] = static_cast<ptrdiff_t>(out_w);

    const auto submitAndComplete = [&](uint64_t *ticket, std::wstring *failure) {
        const int inferRc = infer(context.get(), &source, &destination, nullptr);
        if (inferRc != AJI_OK) {
            *failure = L"aji_infer failed (rc=" + std::to_wstring(inferRc) + L"): " +
                       lastErrorText(lastError, context.get());
            return false;
        }
        const uint64_t placed = flush(context.get(), nullptr);
        if (placed == 0) {
            *failure = L"aji_flush returned 0 (no ticket was placed): " +
                       lastErrorText(lastError, context.get());
            return false;
        }
        const int waitRc = wait(context.get(), placed);
        if (waitRc != AJI_OK) {
            *failure = L"aji_wait failed for ticket " + std::to_wstring(placed) +
                       L" (rc=" + std::to_wstring(waitRc) + L"): " +
                       lastErrorText(lastError, context.get());
            return false;
        }
        const int doneRc = done(context.get(), placed);
        if (doneRc != 1) {
            *failure = L"aji_done reported " + std::to_wstring(doneRc) +
                       L" for completed ticket " + std::to_wstring(placed);
            return false;
        }
        *ticket = placed;
        return true;
    };

    const auto readSnapshot = [&](const wchar_t *phase, GuardedRealSnapshot *out,
                                  std::wstring *failure) {
        GuardedRealSnapshot guarded;
        guarded.value.struct_size = sizeof(guarded.value);
        const int rc =
            getSnapshot(context.get(), AJI_NATIVE_PROTOCOL_VERSION, &guarded.value);
        if (rc != AJI_NATIVE_OK) {
            *failure = std::wstring(phase) + L": snapshot returned " + std::to_wstring(rc);
            return false;
        }
        if (!guarded.intact()) {
            *failure = std::wstring(phase) + L": snapshot wrote past the caller's buffer";
            return false;
        }
        std::wstring reason;
        if (!truthfullyUnavailable(guarded.value, AJI_NATIVE_OK, &reason)) {
            *failure = std::wstring(phase) + L": " + reason;
            return false;
        }
        *out = guarded;
        return true;
    };

    uint64_t ticket = 0;
    if (!submitAndComplete(&ticket, &why)) return refuse(why, 1);

    GuardedRealSnapshot active_snapshot{};
    if (!readSnapshot(L"after real completion", &active_snapshot, &why))
        return refuse(why, 1);
    if (!containsNoCase(active_snapshot.value.provider, "tensorrt"))
        return refuse(L"the snapshot provider is not the TensorRT backend: " +
                          textOf(active_snapshot.value.provider),
                      1);
    if ((active_snapshot.value.capability_bits & AJI_NATIVE_CAP_CU_DEVICE_UUID) == 0)
        return refuse(L"the snapshot did not report the CU device UUID capability", 1);
    const std::wstring expected_identity = L"CUDA-UUID:" + uuidHex(requested);
    const std::wstring reported_identity = textOf(active_snapshot.value.device_identity);
    if (reported_identity != expected_identity)
        return refuse(L"the snapshot device identity " + reported_identity +
                          L" does not match the caller-observed " + expected_identity,
                      1);
    if (active_snapshot.value.last_submitted_ticket != ticket ||
        active_snapshot.value.completed_ticket != ticket)
        return refuse(L"the snapshot ticket state (" +
                          std::to_wstring(active_snapshot.value.last_submitted_ticket) +
                          L"/" +
                          std::to_wstring(active_snapshot.value.completed_ticket) +
                          L") does not match the completed ticket " +
                          std::to_wstring(ticket),
                      1);

    uint32_t drain_rc = 0;
    uint32_t suspend_rc = 0;
    uint32_t resume_rc = 0;
    aji_native_snapshot_v2 ignored{};
    if (!controlIsTruthful(control, context.get(), AJI_NATIVE_CONTROL_DRAIN, 7, L"drain",
                           &drain_rc, &ignored, &why))
        return refuse(why, 1);
    if (!controlIsTruthful(control, context.get(), AJI_NATIVE_CONTROL_SUSPEND, 8,
                           L"suspend", &suspend_rc, &ignored, &why))
        return refuse(why, 1);
    if (!controlIsTruthful(control, context.get(), AJI_NATIVE_CONTROL_RESUME, 9, L"resume",
                           &resume_rc, &ignored, &why))
        return refuse(why, 1);

    if (resume_rc == AJI_NATIVE_OK) {
        uint64_t resumed_ticket = 0;
        if (!submitAndComplete(&resumed_ticket, &why))
            return refuse(L"resume was acknowledged but the restored chain could not "
                          L"complete a frame: " +
                              why,
                          1);
        GuardedRealSnapshot resumed_snapshot{};
        if (!readSnapshot(L"after resume", &resumed_snapshot, &why)) return refuse(why, 1);
    }

    std::printf("native ABI-8 real-context probe: %s (ordinal %d); resize-only %dx%d -> %dx%d; "
                "ticket %llu completed; snapshot Unavailable; drain=%u suspend=%u resume=%u\n",
                narrowUtf8(deviceText).c_str(), ordinal, kProbeWidth, kProbeHeight, out_w,
                out_h, static_cast<unsigned long long>(ticket), drain_rc, suspend_rc,
                resume_rc);
    return 0;
}

struct ProbeArguments {
    std::filesystem::path dll;
    bool real_context = false;
    std::filesystem::path backend;
    std::filesystem::path runtime_dir;
    std::wstring uuid;
};

enum class ArgumentOutcome { Ok, Usage, Refused };

ArgumentOutcome parseArguments(int argc, wchar_t **argv, ProbeArguments *out,
                               std::wstring *why) {
    if (argc < 2) {
        *why = L"missing DLL path";
        return ArgumentOutcome::Usage;
    }
    out->dll = argv[1];
    bool saw_backend = false;
    bool saw_runtime = false;
    bool saw_uuid = false;
    for (int index = 2; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--real-context") {
            out->real_context = true;
            continue;
        }
        if (argument == L"--backend" || argument == L"--runtime-dir" ||
            argument == L"--uuid") {
            if (index + 1 >= argc) {
                *why = argument + L" needs a value";
                return ArgumentOutcome::Usage;
            }
            const std::wstring value = argv[++index];
            if (argument == L"--backend") {
                out->backend = value;
                saw_backend = true;
            } else if (argument == L"--runtime-dir") {
                out->runtime_dir = value;
                saw_runtime = true;
            } else {
                out->uuid = value;
                saw_uuid = true;
            }
            continue;
        }
        *why = L"unknown argument " + argument;
        return ArgumentOutcome::Usage;
    }
    if (out->real_context) {
        if (!saw_uuid || !saw_backend || !saw_runtime) {
            *why = L"--real-context requires --uuid, --backend and --runtime-dir";
            return ArgumentOutcome::Refused;
        }
    } else if (saw_uuid || saw_backend || saw_runtime) {
        *why = L"--uuid, --backend and --runtime-dir require --real-context";
        return ArgumentOutcome::Refused;
    }
    return ArgumentOutcome::Ok;
}

void printUsage() {
    std::fwprintf(stderr,
                  L"usage: native_abi_probe <absolute-path-to-aji.dll> [--real-context "
                  L"--uuid <GPU-uuid> --backend <absolute-path-to-aji_trt.dll> "
                  L"--runtime-dir <absolute-TensorRT-runtime-directory>]\n");
}
#endif // _WIN32

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    ProbeArguments arguments;
    std::wstring why;
    switch (parseArguments(argc, argv, &arguments, &why)) {
    case ArgumentOutcome::Usage:
        printUsage();
        return 2;
    case ArgumentOutcome::Refused:
        return refuse(why, 2);
    case ArgumentOutcome::Ok:
        break;
    }

    std::filesystem::path dll;
    if (!plainAbsoluteFile(arguments.dll, &dll)) return 2;

    RealContextOptions real;
    RegisteredDllDirectory runtime_registration;
    if (arguments.real_context) {
        std::filesystem::path backend;
        std::filesystem::path runtime;
        if (!plainAbsoluteFile(arguments.backend, &backend)) return 2;
        if (!plainAbsoluteDirectory(arguments.runtime_dir, &runtime)) return 2;
        if (!equalsNoCase(backend.filename().wstring(), L"aji_trt.dll"))
            return refuse(L"--backend must name aji_trt.dll, the TensorRT backend the "
                          L"probe configuration selects: " +
                              backend.wstring(),
                          2);
        std::error_code error;
        if (!std::filesystem::equivalent(backend.parent_path(), dll.parent_path(), error) ||
            error)
            return refuse(L"--backend must sit beside the dispatcher DLL; the dispatcher "
                          L"only loads a backend from its own directory (" +
                              dll.parent_path().wstring() + L")",
                          2);
        if (!runtime_registration.add(runtime, &why)) return refuse(why, 2);
        real.backend = backend;
        real.runtime_dir = runtime;
        real.uuid = arguments.uuid;
    }

    void *module = openModule(dll);
    if (!module) {
        reportWinError(L"LoadLibraryExW", dll, GetLastError());
        return 2;
    }
    int result = runProbe(module, "aji.dll");
    if (result == 0 && arguments.real_context)
        result = runRealContextScenario(module, "aji.dll", real);
    closeModule(module);
    return result;
}
#else
int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr,
                     "native_abi_probe: the real-context scenario requires Windows; "
                     "usage: native_abi_probe <path-to-aji.dll>\n");
        return 2;
    }
    void *module = openModule(argv[1]);
    if (!module) {
        std::fprintf(stderr, "native_abi_probe: cannot load %s: %s\n", argv[1], dlerror());
        return 2;
    }
    const int result = runProbe(module, argv[1]);
    closeModule(module);
    return result;
}
#endif
