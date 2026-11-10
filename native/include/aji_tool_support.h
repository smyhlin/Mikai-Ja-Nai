// aji_tool_support.h -- shared helpers for the MSVC-native tools
// (native_abi_probe, mikai_native_prep, mikai_native_verify,
// native_prepared_runtime_probe). X-D20: the four tools carried their own
// copies of these, and the copies had drifted (widen took const char* in one
// tool and const std::string& in another, equalsNoCase was _wcsicmp in one,
// a towlower loop in another and ASCII-only in the third, halfToFloat had
// two loop spellings, CudaDriver had two member sets).
//
// Qt-free; Win32 only (MultiByteToWideChar / WideCharToMultiByte,
// LoadLibraryExW / GetProcAddress / FreeLibrary). No CUDA, TensorRT or JSON
// dependency: the driver is resolved from nvcuda.dll at run time, so no tool
// needs cuda.h or a link dependency on the toolkit. bcrypt is not needed
// here (nothing in this header hashes).
//
// The string / half helpers live in the global namespace so the existing
// call sites stay unchanged. The CUDA-driver part lives in namespace
// aji_tool: mikai_native_verify.cpp includes the real <cuda.h>, whose
// ::CUresult / ::CUdevice / ::CUuuid / ::CUcontext / ::CUstream would
// collide with global aliases. The two tools that use the driver add
// `using namespace aji_tool;`; the other two never name those types.
#ifndef AJI_TOOL_SUPPORT_H
#define AJI_TOOL_SUPPORT_H

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>
#include <string_view>

// UTF-8 <-> UTF-16, size-based (embedded NULs survive, unlike the old -1
// spellings that stopped at the first NUL). Empty input converts to empty
// output; widen(nullptr) is empty rather than undefined.
inline std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), length);
    return out;
}

inline std::wstring widen(const char *utf8) {
    return utf8 ? widen(std::string_view(utf8)) : std::wstring();
}

inline std::string narrowUtf8(std::wstring_view wide) {
    if (wide.empty()) return std::string();
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) return std::string();
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), length, nullptr, nullptr);
    return out;
}

// Case-insensitive equality. The wide overload folds with towlower; the
// narrow overload folds ASCII only, which is what the runtime probe's old
// lowerAscii comparison did — its inputs are ASCII ids ("bt601", "full").
// Overloads resolve by argument type, so no call site changes spelling.
inline bool equalsNoCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

inline bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const unsigned char left = static_cast<unsigned char>(a[i]);
        const unsigned char right = static_cast<unsigned char>(b[i]);
        const unsigned char foldedLeft =
            (left >= 'A' && left <= 'Z') ? static_cast<unsigned char>(left - 'A' + 'a') : left;
        const unsigned char foldedRight =
            (right >= 'A' && right <= 'Z') ? static_cast<unsigned char>(right - 'A' + 'a') : right;
        if (foldedLeft != foldedRight) return false;
    }
    return true;
}

// Binary16 -> binary32. Subnormals and the infinities survive: folding them
// away would hide a real failure rather than report it. The two old copies
// (a while-loop and a do-while spelling) computed the same bits; this is the
// while-loop spelling.
inline float halfToFloat(const uint16_t bits) {
    const uint32_t sign = static_cast<uint32_t>(bits >> 15) << 31;
    const uint32_t exponent = (bits >> 10) & 0x1Fu;
    const uint32_t mantissa = bits & 0x3FFu;
    uint32_t result;
    if (exponent == 0) {
        if (mantissa == 0) {
            result = sign;
        } else {
            uint32_t normalised = mantissa;
            int32_t shift = -1;
            while ((normalised & 0x400u) == 0) {
                normalised <<= 1;
                ++shift;
            }
            normalised &= 0x3FFu;
            result = sign | (static_cast<uint32_t>(127 - 15 - shift) << 23) | (normalised << 13);
        }
    } else if (exponent == 31) {
        result = sign | 0x7F800000u | (mantissa << 13);
    } else {
        result = sign | ((exponent + (127u - 15u)) << 23) | (mantissa << 13);
    }
    float value;
    std::memcpy(&value, &result, sizeof(value));
    return value;
}

namespace aji_tool {

// Minimal CUDA driver ABI, declared here so no tool needs cuda.h or a link
// dependency on the toolkit. Every entry point is resolved from nvcuda.dll
// at run time; the versioned spellings are tried first.
using CUresult = int;
using CUdevice = int;
using CUdeviceptr = unsigned long long;
struct CUuuid {
    char bytes[16];
};
struct CUctx_st;
using CUcontext = CUctx_st *;
struct CUstream_st;
using CUstream = CUstream_st *;

constexpr CUresult kCudaSuccess = 0;

inline void *driverSymbol(HMODULE module, const char *preferred, const char *fallback) {
    void *resolved = reinterpret_cast<void *>(GetProcAddress(module, preferred));
    if (!resolved && fallback)
        resolved = reinterpret_cast<void *>(GetProcAddress(module, fallback));
    return resolved;
}

// The union of the two old structs: the runtime probe also needs
// deviceGetAttribute, ctxGetDevice, memcpyDtoH and the stream entry points,
// and load() requires the full set. All of them exist in every nvcuda.dll
// since CUDA 2.0 (streamCreate since 1.0), so requiring them does not narrow
// the drivers the ABI probe accepts in practice. deviceGetName is resolved
// but, as before, is not required: both old load()s omitted it from the
// completeness check.
struct CudaDriver {
    HMODULE module = nullptr;
    CUresult(__stdcall *init)(unsigned int) = nullptr;
    CUresult(__stdcall *deviceGetCount)(int *) = nullptr;
    CUresult(__stdcall *deviceGet)(CUdevice *, int) = nullptr;
    CUresult(__stdcall *deviceGetUuid)(CUuuid *, CUdevice) = nullptr;
    CUresult(__stdcall *deviceGetName)(char *, int, CUdevice) = nullptr;
    CUresult(__stdcall *deviceGetAttribute)(int *, int, CUdevice) = nullptr;
    CUresult(__stdcall *primaryCtxRetain)(CUcontext *, CUdevice) = nullptr;
    CUresult(__stdcall *primaryCtxRelease)(CUdevice) = nullptr;
    CUresult(__stdcall *ctxGetCurrent)(CUcontext *) = nullptr;
    CUresult(__stdcall *ctxSetCurrent)(CUcontext) = nullptr;
    CUresult(__stdcall *ctxGetDevice)(CUdevice *) = nullptr;
    CUresult(__stdcall *memAlloc)(CUdeviceptr *, size_t) = nullptr;
    CUresult(__stdcall *memFree)(CUdeviceptr) = nullptr;
    CUresult(__stdcall *memcpyHtoD)(CUdeviceptr, const void *, size_t) = nullptr;
    CUresult(__stdcall *memcpyDtoH)(void *, CUdeviceptr, size_t) = nullptr;
    CUresult(__stdcall *streamCreate)(CUstream *, unsigned int) = nullptr;
    CUresult(__stdcall *streamDestroy)(CUstream) = nullptr;
    CUresult(__stdcall *streamSynchronize)(CUstream) = nullptr;
    CUresult(__stdcall *driverGetVersion)(int *) = nullptr;
    CUresult(__stdcall *getErrorName)(CUresult, const char **) = nullptr;
    CUresult(__stdcall *getErrorString)(CUresult, const char **) = nullptr;

    CudaDriver() = default;
    CudaDriver(const CudaDriver &) = delete;
    CudaDriver &operator=(const CudaDriver &) = delete;
    ~CudaDriver() {
        if (module) FreeLibrary(module);
    }

    bool load(std::string *why) {
        module = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            *why = "nvcuda.dll is not loadable from System32 (error " +
                   std::to_string(GetLastError()) + ")";
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
        deviceGetAttribute = reinterpret_cast<decltype(deviceGetAttribute)>(
            driverSymbol(module, "cuDeviceGetAttribute", nullptr));
        primaryCtxRetain = reinterpret_cast<decltype(primaryCtxRetain)>(
            driverSymbol(module, "cuDevicePrimaryCtxRetain", nullptr));
        primaryCtxRelease = reinterpret_cast<decltype(primaryCtxRelease)>(
            driverSymbol(module, "cuDevicePrimaryCtxRelease_v2", "cuDevicePrimaryCtxRelease"));
        ctxGetCurrent = reinterpret_cast<decltype(ctxGetCurrent)>(
            driverSymbol(module, "cuCtxGetCurrent", nullptr));
        ctxSetCurrent = reinterpret_cast<decltype(ctxSetCurrent)>(
            driverSymbol(module, "cuCtxSetCurrent", nullptr));
        ctxGetDevice = reinterpret_cast<decltype(ctxGetDevice)>(
            driverSymbol(module, "cuCtxGetDevice", nullptr));
        memAlloc = reinterpret_cast<decltype(memAlloc)>(
            driverSymbol(module, "cuMemAlloc_v2", "cuMemAlloc"));
        memFree = reinterpret_cast<decltype(memFree)>(
            driverSymbol(module, "cuMemFree_v2", "cuMemFree"));
        memcpyHtoD = reinterpret_cast<decltype(memcpyHtoD)>(
            driverSymbol(module, "cuMemcpyHtoD_v2", "cuMemcpyHtoD"));
        memcpyDtoH = reinterpret_cast<decltype(memcpyDtoH)>(
            driverSymbol(module, "cuMemcpyDtoH_v2", "cuMemcpyDtoH"));
        streamCreate = reinterpret_cast<decltype(streamCreate)>(
            driverSymbol(module, "cuStreamCreate", nullptr));
        streamDestroy = reinterpret_cast<decltype(streamDestroy)>(
            driverSymbol(module, "cuStreamDestroy_v2", "cuStreamDestroy"));
        streamSynchronize = reinterpret_cast<decltype(streamSynchronize)>(
            driverSymbol(module, "cuStreamSynchronize", nullptr));
        driverGetVersion = reinterpret_cast<decltype(driverGetVersion)>(
            driverSymbol(module, "cuDriverGetVersion", nullptr));
        getErrorName = reinterpret_cast<decltype(getErrorName)>(
            driverSymbol(module, "cuGetErrorName", nullptr));
        getErrorString = reinterpret_cast<decltype(getErrorString)>(
            driverSymbol(module, "cuGetErrorString", nullptr));
        if (!init || !deviceGetCount || !deviceGet || !deviceGetUuid || !deviceGetAttribute ||
            !primaryCtxRetain || !primaryCtxRelease || !ctxGetCurrent || !ctxSetCurrent ||
            !ctxGetDevice || !memAlloc || !memFree || !memcpyHtoD || !memcpyDtoH ||
            !streamCreate || !streamDestroy || !streamSynchronize) {
            *why = "nvcuda.dll is missing a required driver entry point";
            return false;
        }
        return true;
    }

    // Narrow diagnostic: the ABI probe widens it at its call sites, where
    // the diagnostics are wide.
    std::string text(CUresult result) const {
        std::string out = "CUDA error " + std::to_string(result);
        const char *name = nullptr;
        if (getErrorName && getErrorName(result, &name) == kCudaSuccess && name && *name)
            out = name;
        const char *detail = nullptr;
        if (getErrorString && getErrorString(result, &detail) == kCudaSuccess && detail && *detail)
            out += std::string(" (") + detail + ")";
        return out;
    }

    bool copyToDevice(CUdeviceptr destination, const void *source, size_t bytes,
                      std::string *why) {
        const CUresult result = memcpyHtoD(destination, source, bytes);
        if (result != kCudaSuccess) {
            *why = "cuMemcpyHtoD failed: " + text(result);
            return false;
        }
        return true;
    }
};

}  // namespace aji_tool

#endif  // AJI_TOOL_SUPPORT_H
