// Out-of-band engine preparation: a separate process, never the frame loop.
//
// This helper builds a plan IN-PROCESS through the TensorRT builder API: it
// selects one named CUDA device, parses one actual ONNX graph with NvOnnxParser,
// binds one fixed shape set per real network input, and moves the serialized
// bytes to --out. It launches no other executable and falls back to none.
//
// Those bytes are RAW: nothing here deserialises, verifies or publishes them.
// Only the native verification step and the artifact store may publish a plan.
//
// Exit 0 only when --out received a non-empty plan. Codes 2..6 all leave --out
// exactly as it was found and name their cause.

#include <windows.h>

#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <bcrypt.h>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kName[] = L"mikai_native_prep";

// The bounded TensorRT workspace when the caller does not ask for another one.
// It is a tactic-selection bound, not an inference-buffer allowance.
constexpr uint64_t kDefaultWorkspaceBytes = 1073741824ULL;

// TensorRT's own default builder optimization level, so an omitted
// --builder-optimization-level produces exactly the plan this helper produced
// before the option existed. It bounds how long the builder may search for
// tactics; it selects no dtype, no layout and no tolerance.
constexpr int32_t kDefaultBuilderOptimizationLevel = 3;

enum ExitCode { kUsage = 2, kPreflight = 3, kSetup = 4, kBuild = 5, kPublish = 6 };

int report(const ExitCode code, const std::wstring& reason) {
    std::fflush(stdout);
    std::fwprintf(stderr, L"%ls: %ls\n", kName, reason.c_str());
    std::fflush(stderr);
    return code;
}

// One JSON object per line on stdout, written only after the transition it
// names has actually happened. No event claims a plan is verified, prepared or
// active; the raw record says so explicitly.
std::string utf8(const std::wstring& text) {
    if (text.empty())
        return std::string();
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (bytes <= 0)
        return std::string();
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), bytes, nullptr,
                        nullptr);
    return out;
}

std::wstring wide(const std::string& utf8Text) {
    if (utf8Text.empty())
        return std::wstring();
    const int characters = MultiByteToWideChar(CP_UTF8, 0, utf8Text.c_str(), -1, nullptr, 0);
    if (characters <= 1)
        return std::wstring();
    std::wstring out(static_cast<size_t>(characters - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Text.c_str(), -1, out.data(), characters);
    return out;
}

void appendJsonString(std::string& out, const std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    char escape[8];
                    std::snprintf(escape, sizeof escape, "\\u%04x", byte);
                    out += escape;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void emit(std::string json) {
    json.push_back('\n');
    std::fwrite(json.data(), 1, json.size(), stdout);
    std::fflush(stdout);
}

std::string unsignedNumber(const uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof text, "%llu", static_cast<unsigned long long>(value));
    return text;
}

std::string signedNumber(const int64_t value) {
    char text[24];
    std::snprintf(text, sizeof text, "%lld", static_cast<long long>(value));
    return text;
}

std::string quoted(const std::wstring& text) {
    std::string out;
    appendJsonString(out, utf8(text));
    return out;
}

void appendField(std::string& json, const char* const name, const std::string& value) {
    if (json.back() != '{')
        json.push_back(',');
    appendJsonString(json, name);
    json.push_back(':');
    json += value;
}

void appendNumber(std::string& json, const char* const name, const uint64_t value) {
    appendField(json, name, unsignedNumber(value));
}

// SHA-256, so graph and engine identity are measured rather than asserted.
bool sha256Hex(const void* data, const size_t bytes, std::string& hex) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD written = 0;
    DWORD digestSize = 0;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                          sizeof objectSize, &written, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    std::vector<unsigned char> object(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    const unsigned char* const input = static_cast<const unsigned char*>(data);
    size_t hashed = 0;
    while (hashed < bytes) {
        const size_t chunk = std::min<size_t>(bytes - hashed, 1024u * 1024u);
        if (BCryptHashData(hash, const_cast<PUCHAR>(input + hashed), static_cast<ULONG>(chunk), 0) < 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return false;
        }
        hashed += chunk;
    }
    std::vector<unsigned char> digest(32);  // SHA-256 is 32 bytes by definition
    const NTSTATUS finished =
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (finished < 0)
        return false;

    static constexpr char kHex[] = "0123456789abcdef";
    hex.clear();
    hex.reserve(digest.size() * 2);
    for (const unsigned char byte : digest) {
        hex.push_back(kHex[byte >> 4]);
        hex.push_back(kHex[byte & 0x0F]);
    }
    return true;
}

// The real TensorRT parser/builder log, on stderr.
class StderrLogger : public nvinfer1::ILogger {
public:
    void log(const Severity severity, const char* const message) noexcept override {
        static std::mutex serial;
        static const char* const kNames[] = {"INTERNAL ERROR", "ERROR", "WARNING", "INFO", "VERBOSE"};
        const int level = static_cast<int>(severity);
        const char* const label =
            (level >= 0 && level < static_cast<int>(sizeof kNames / sizeof kNames[0])) ? kNames[level] : "LOG";
        const std::lock_guard<std::mutex> lock(serial);
        std::fprintf(stderr, "%ls: %s: %s\n", kName, label, message != nullptr ? message : "");
        std::fflush(stderr);
    }
};

const char* typeName(const nvinfer1::DataType type) {
    switch (type) {
        case nvinfer1::DataType::kFLOAT: return "float32";
        case nvinfer1::DataType::kHALF: return "float16";
        case nvinfer1::DataType::kBF16: return "bfloat16";
        case nvinfer1::DataType::kINT8: return "int8";
        case nvinfer1::DataType::kUINT8: return "uint8";
        case nvinfer1::DataType::kINT32: return "int32";
        case nvinfer1::DataType::kINT64: return "int64";
        case nvinfer1::DataType::kBOOL: return "bool";
        case nvinfer1::DataType::kFP8: return "float8";
        case nvinfer1::DataType::kINT4: return "int4";
        case nvinfer1::DataType::kFP4: return "float4";
        case nvinfer1::DataType::kE8M0: return "e8m0";
    }
    return "unknown";
}

// A CUDA name field is not guaranteed to carry its terminator, so it is copied
// only up to the field and no further.
std::string boundedText(const char* const field, const size_t capacity) {
    size_t length = 0;
    while (length < capacity && field[length] != '\0')
        ++length;
    return std::string(field, length);
}

// The helper binds floating-point network inputs. A graph whose declared input
// is an integer, boolean or sub-byte type is refused by name rather than built
// against a contract it does not meet.
bool supportedInputType(const nvinfer1::DataType type) {
    return type == nvinfer1::DataType::kFLOAT || type == nvinfer1::DataType::kHALF ||
           type == nvinfer1::DataType::kBF16;
}

// Every network input and output is bound to plain contiguous linear layout.
// The native pipeline's buffers are contiguous NCHW, so a vectorised IO format
// would describe the same logical shape while requiring a different number of
// allocated bytes; the engine must never choose one behind the caller's back.
// This changes no dtype and no extent - only the permitted memory layout.
constexpr nvinfer1::TensorFormats kLinearOnly =
    1U << static_cast<uint32_t>(nvinfer1::TensorFormat::kLINEAR);

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

struct Options {
    std::wstring onnx;
    std::wstring out;
    std::wstring shapes;
    std::wstring deviceUuid;
    uint64_t workspaceBytes = kDefaultWorkspaceBytes;
    bool workspaceGiven = false;
    int32_t builderOptimizationLevel = kDefaultBuilderOptimizationLevel;
    bool builderOptimizationLevelGiven = false;
    // --describe-device: report the real device, runtime and builder policy
    // without reading a graph, deserializing, building, inferring or writing.
    bool describeDevice = false;
    bool jsonRequested = false;
};

struct FlagBinding {
    const wchar_t* flag;
    std::wstring Options::*field;
};

constexpr FlagBinding kRequiredFlags[] = {
    {L"--onnx", &Options::onnx},
    {L"--out", &Options::out},
    {L"--shapes", &Options::shapes},
    {L"--device-uuid", &Options::deviceUuid},
};

bool parseArguments(const int argc, wchar_t** argv, Options& options, std::wstring& reason) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view flag = argv[i];

        if (flag == L"--workspace-bytes") {
            if (options.workspaceGiven) {
                reason = L"repeated argument --workspace-bytes";
                return false;
            }
            if (i + 1 >= argc) {
                reason = L"argument --workspace-bytes has no value";
                return false;
            }
            const std::wstring_view text = argv[++i];
            if (text.empty() || text.rfind(L"--", 0) == 0) {
                reason = L"argument --workspace-bytes has no usable value";
                return false;
            }
            uint64_t value = 0;
            for (const wchar_t c : text) {
                if (c < L'0' || c > L'9') {
                    reason = L"--workspace-bytes \"" + std::wstring(text) + L"\" is not a byte count";
                    return false;
                }
                const uint64_t digit = static_cast<uint64_t>(c - L'0');
                if (value > (UINT64_MAX - digit) / 10) {
                    reason = L"--workspace-bytes \"" + std::wstring(text) + L"\" does not fit in 64 bits";
                    return false;
                }
                value = value * 10 + digit;
            }
            if (value == 0) {
                reason = L"--workspace-bytes must be greater than zero";
                return false;
            }
            options.workspaceBytes = value;
            options.workspaceGiven = true;
            continue;
        }

        const FlagBinding* binding = nullptr;
        for (const FlagBinding& candidate : kRequiredFlags) {
            if (flag == candidate.flag) {
                binding = &candidate;
                break;
            }
        }
        if (flag == L"--describe-device") {
            if (options.describeDevice) {
                reason = L"repeated argument --describe-device";
                return false;
            }
            options.describeDevice = true;
            continue;
        }

        if (flag == L"--json") {
            if (options.jsonRequested) {
                reason = L"repeated argument --json";
                return false;
            }
            options.jsonRequested = true;
            continue;
        }

        if (flag == L"--builder-optimization-level") {
            if (options.builderOptimizationLevelGiven) {
                reason = L"repeated argument --builder-optimization-level";
                return false;
            }
            if (i + 1 >= argc) {
                reason = L"argument --builder-optimization-level has no value";
                return false;
            }
            const std::wstring_view text = argv[++i];
            if (text.size() != 1 || text[0] < L'0' || text[0] > L'5') {
                reason = L"--builder-optimization-level \"" + std::wstring(text) +
                         L"\" is not one of 0..5; the builder's documented range is 0 (fastest build, "
                         L"fewest tactics) to 5 (longest build, most tactics)";
                return false;
            }
            options.builderOptimizationLevel = static_cast<int32_t>(text[0] - L'0');
            options.builderOptimizationLevelGiven = true;
            continue;
        }

        if (binding == nullptr) {
            reason = L"unknown argument " + std::wstring(flag) +
                     L"; usage: --onnx <graph> --out <file> --shapes <name:d0xd1xd2xd3[,name:...]> "
                     L"--device-uuid <uuid> [--workspace-bytes <uint64>] "
                     L"[--builder-optimization-level <0..5>] | "
                     L"--describe-device [--json] [--device-uuid <uuid>] "
                     L"[--workspace-bytes <uint64>] [--builder-optimization-level <0..5>]";
            return false;
        }
        if (!(options.*(binding->field)).empty()) {
            reason = L"repeated argument " + std::wstring(flag);
            return false;
        }
        if (i + 1 >= argc) {
            reason = L"argument " + std::wstring(flag) + L" has no value";
            return false;
        }
        const std::wstring_view value = argv[++i];
        if (value.empty() || value.rfind(L"--", 0) == 0) {
            reason = L"argument " + std::wstring(flag) + L" has no usable value";
            return false;
        }
        options.*(binding->field) = value;
    }

    if (options.describeDevice)
        return true;

    for (const FlagBinding& candidate : kRequiredFlags) {
        if ((options.*(candidate.field)).empty()) {
            reason = std::wstring(L"missing ") + candidate.flag;
            return false;
        }
    }
    return true;
}

// ONNX input names are arbitrary strings, so nothing is whitelisted here. Only
// this helper's own separators, which would make the specification ambiguous
// rather than unusual, and control characters, which cannot survive a command
// line, are refused.
bool validName(const std::wstring_view name) {
    if (name.empty())
        return false;
    for (const wchar_t c : name) {
        if (c == L':' || c == L',' || c < 0x20 || c == 0x7F)
            return false;
    }
    return true;
}

bool validExtent(const std::wstring_view digits, const std::wstring_view& entry, std::wstring& reason) {
    if (digits.empty()) {
        reason = L"--shapes entry \"" + std::wstring(entry) + L"\" has an empty extent";
        return false;
    }
    uint64_t value = 0;
    for (const wchar_t c : digits) {
        if (c < L'0' || c > L'9') {
            reason = L"--shapes entry \"" + std::wstring(entry) + L"\" has a non-numeric extent";
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(c - L'0');
        if (value > (UINT64_MAX - digit) / 10) {
            reason = L"--shapes entry \"" + std::wstring(entry) + L"\" has an extent that does not fit";
            return false;
        }
        value = value * 10 + digit;
    }
    if (value == 0) {
        reason = L"--shapes entry \"" + std::wstring(entry) + L"\" has a zero extent";
        return false;
    }
    return true;
}

bool parseExtents(const std::wstring_view dims, const std::wstring_view& entry, int64_t (&extent)[4],
                  std::wstring& reason) {
    int rank = 0;
    size_t at = 0;
    for (;;) {
        const size_t x = dims.find(L'x', at);
        const std::wstring_view digits = dims.substr(at, x - at);
        if (!validExtent(digits, entry, reason))
            return false;
        if (++rank > 4) {
            reason = L"--shapes entry \"" + std::wstring(entry) + L"\" has more than 4 extents";
            return false;
        }
        uint64_t value = 0;
        for (const wchar_t c : digits)
            value = value * 10 + static_cast<uint64_t>(c - L'0');
        if (value > static_cast<uint64_t>(INT64_MAX)) {
            reason = L"--shapes entry \"" + std::wstring(entry) + L"\" has an extent TensorRT cannot bind";
            return false;
        }
        extent[rank - 1] = static_cast<int64_t>(value);
        if (x == std::wstring_view::npos)
            break;
        at = x + 1;
    }
    if (rank != 4) {
        reason = L"--shapes entry \"" + std::wstring(entry) + L"\" is not rank-4";
        return false;
    }
    return true;
}

struct ShapeSpec {
    std::string name;
    int64_t extent[4]{};
};

bool parseShapes(const std::wstring_view spec, std::vector<ShapeSpec>& shapes, std::wstring& reason) {
    shapes.clear();
    size_t at = 0;
    for (;;) {
        const size_t comma = spec.find(L',', at);
        const std::wstring_view entry = spec.substr(at, comma - at);
        const size_t colon = entry.find(L':');
        if (colon == std::wstring_view::npos || colon == 0 || colon + 1 == entry.size()) {
            reason = L"--shapes entry \"" + std::wstring(entry) + L"\" is not name:d0xd1xd2xd3";
            return false;
        }
        const std::wstring_view name = entry.substr(0, colon);
        if (!validName(name)) {
            reason = L"--shapes entry \"" + std::wstring(entry) +
                     L"\" has an input name this helper cannot carry";
            return false;
        }
        ShapeSpec shape;
        shape.name = utf8(std::wstring(name));
        if (shape.name.empty()) {
            reason = L"--shapes entry \"" + std::wstring(entry) + L"\" has an unusable input name";
            return false;
        }
        for (const ShapeSpec& seen : shapes) {
            if (seen.name == shape.name) {
                reason = L"--shapes names input \"" + std::wstring(name) + L"\" twice";
                return false;
            }
        }
        if (!parseExtents(entry.substr(colon + 1), entry, shape.extent, reason))
            return false;
        shapes.push_back(std::move(shape));

        if (comma == std::wstring_view::npos)
            return true;
        at = comma + 1;
    }
}

std::wstring uuidText(const cudaUUID_t& uuid) {
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring text = L"GPU-";
    text.reserve(40);
    for (size_t i = 0; i < sizeof(uuid.bytes); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            text.push_back(L'-');
        const uint8_t byte = static_cast<uint8_t>(uuid.bytes[i]);
        text.push_back(kHex[byte >> 4]);
        text.push_back(kHex[byte & 0x0F]);
    }
    return text;
}

constexpr uint8_t kNotHex = 0xFF;

uint8_t hexNibble(const wchar_t c) {
    if (c >= L'0' && c <= L'9')
        return static_cast<uint8_t>(c - L'0');
    if (c >= L'a' && c <= L'f')
        return static_cast<uint8_t>(c - L'a' + 10);
    if (c >= L'A' && c <= L'F')
        return static_cast<uint8_t>(c - L'A' + 10);
    return kNotHex;
}

bool parseUuid(const std::wstring_view text, cudaUUID_t& uuid, std::wstring& reason) {
    std::wstring_view digits = text;
    if (digits.substr(0, 4) == L"GPU-")
        digits.remove_prefix(4);

    int nibbles = 0;
    for (const wchar_t c : digits) {
        if (c == L'-' || c == L'{' || c == L'}')
            continue;
        const uint8_t value = hexNibble(c);
        if (value == kNotHex || nibbles >= 32) {
            reason = L"--device-uuid \"" + std::wstring(text) + L"\" is not a 16-byte CUDA UUID";
            return false;
        }
        if (nibbles % 2 == 0) {
            uuid.bytes[nibbles / 2] = static_cast<char>(value << 4);
        } else {
            uuid.bytes[nibbles / 2] =
                static_cast<char>(static_cast<uint8_t>(uuid.bytes[nibbles / 2]) | value);
        }
        ++nibbles;
    }
    if (nibbles != 32) {
        reason = L"--device-uuid \"" + std::wstring(text) + L"\" is not a 16-byte CUDA UUID";
        return false;
    }
    return true;
}

std::wstring cudaFailure(const std::wstring& what, const cudaError_t status) {
    const char* const text = cudaGetErrorString(status);
    return what + L" failed (" + std::to_wstring(static_cast<int>(status)) + L": " +
           wide(text != nullptr ? text : "no description") + L")";
}

// The version of the nvcuda.dll the process actually has loaded, read from
// that file's own version resource. cudaDriverGetVersion returns the CUDA
// driver API level, which does not change between driver releases and so cannot
// identify the driver that produced an engine.
std::wstring loadedDriverFileVersion() {
    const HMODULE module = GetModuleHandleW(L"nvcuda.dll");
    if (module == nullptr)
        return std::wstring();
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return std::wstring();

    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (size == 0)
        return std::wstring();
    std::vector<unsigned char> block(size);
    if (!GetFileVersionInfoW(path, handle, size, block.data()))
        return std::wstring();

    // VerQueryValueW hands back a void** out-parameter pointing into the block,
    // so the result is taken as an address and only then read as the struct.
    void* queried = nullptr;
    UINT words = 0;
    if (!VerQueryValueW(block.data(), L"\\", &queried, &words) || queried == nullptr ||
        words < sizeof(VS_FIXEDFILEINFO))
        return std::wstring();
    const VS_FIXEDFILEINFO* const fixed = static_cast<const VS_FIXEDFILEINFO*>(queried);
    if (fixed->dwSignature != 0xFEEF04BD)
        return std::wstring();
    return std::to_wstring(HIWORD(fixed->dwFileVersionMS)) + L"." +
           std::to_wstring(LOWORD(fixed->dwFileVersionMS)) + L"." +
           std::to_wstring(HIWORD(fixed->dwFileVersionLS)) + L"." +
           std::to_wstring(LOWORD(fixed->dwFileVersionLS));
}

struct DeviceFacts {
    int ordinal = 0;
    std::string name;
    int major = 0;
    int minor = 0;
    uint64_t totalGlobalMemory = 0;
    int multiProcessorCount = 0;
    int runtimeVersion = 0;
    int driverApiVersion = 0;
    std::wstring driverFileVersion;
};

// The requested device becomes the CUDA runtime's current device before
// anything is parsed or built, so the builder has no choice about which
// adapter it runs on. There is no fallback ordinal: a UUID that is not present
// is a refusal, not a build against whatever happened to be device 0.
bool selectDevice(const cudaUUID_t& requested, DeviceFacts& facts, std::wstring& reason) {
    int count = 0;
    const cudaError_t counted = cudaGetDeviceCount(&count);
    if (counted != cudaSuccess) {
        reason = cudaFailure(L"cudaGetDeviceCount", counted);
        return false;
    }
    if (count < 1) {
        reason = L"no CUDA device is present; nothing was built";
        return false;
    }

    std::wstring present;
    int ordinal = -1;
    cudaDeviceProp properties{};
    for (int index = 0; index < count; ++index) {
        cudaDeviceProp candidate{};
        if (cudaGetDeviceProperties(&candidate, index) != cudaSuccess)
            continue;
        if (std::memcmp(candidate.uuid.bytes, requested.bytes, sizeof requested.bytes) == 0) {
            ordinal = index;
            properties = candidate;
            break;
        }
        present += L"\n  " + uuidText(candidate.uuid);
    }
    if (ordinal < 0) {
        reason = L"--device-uuid " + uuidText(requested) + L" is not one of this machine's CUDA devices (" +
                 std::to_wstring(count) + L" present):" + present;
        return false;
    }

    const cudaError_t selected = cudaSetDevice(ordinal);
    if (selected != cudaSuccess) {
        reason = cudaFailure(L"cudaSetDevice " + std::to_wstring(ordinal), selected);
        return false;
    }
    int current = -1;
    const cudaError_t read = cudaGetDevice(&current);
    if (read != cudaSuccess || current != ordinal) {
        reason = L"the CUDA runtime did not keep the selected device";
        return false;
    }

    int runtimeVersion = 0;
    if (cudaRuntimeGetVersion(&runtimeVersion) != cudaSuccess)
        runtimeVersion = 0;
    int driverApiVersion = 0;
    if (cudaDriverGetVersion(&driverApiVersion) != cudaSuccess)
        driverApiVersion = 0;

    facts.ordinal = ordinal;
    facts.name = boundedText(properties.name, sizeof properties.name);
    facts.major = properties.major;
    facts.minor = properties.minor;
    facts.totalGlobalMemory = properties.totalGlobalMem;
    facts.multiProcessorCount = properties.multiProcessorCount;
    facts.runtimeVersion = runtimeVersion;
    facts.driverApiVersion = driverApiVersion;
    facts.driverFileVersion = loadedDriverFileVersion();
    return true;
}

// The least priority the device reports, so a build does not compete with
// playback for the same adapter. A scheduler hint, not a timing loop.
bool createProfileStream(cudaStream_t& stream, int& priority, std::wstring& reason) {
    int least = 0;
    int greatest = 0;
    const cudaError_t range = cudaDeviceGetStreamPriorityRange(&least, &greatest);
    if (range != cudaSuccess) {
        reason = cudaFailure(L"cudaDeviceGetStreamPriorityRange", range);
        return false;
    }
    const cudaError_t created =
        cudaStreamCreateWithPriority(&stream, cudaStreamNonBlocking, least);
    if (created != cudaSuccess) {
        reason = cudaFailure(L"cudaStreamCreateWithPriority", created);
        return false;
    }
    priority = least;
    return true;
}

std::wstring parentDirectory(const std::wstring_view path) {
    const size_t cut = path.find_last_of(L"\\/");
    if (cut == std::wstring_view::npos)
        return L".";
    if (cut == 0)
        return std::wstring(path.substr(0, 1));
    return std::wstring(path.substr(0, cut));
}

bool isExistingFile(const std::wstring_view path, uint64_t* bytes) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(std::wstring(path).c_str(), GetFileExInfoStandard, &info))
        return false;
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;
    if (bytes != nullptr)
        *bytes = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    return true;
}

bool pathExists(const std::wstring_view path) {
    return GetFileAttributesW(std::wstring(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// CREATE_NEW, so a name already in use is skipped rather than removed: only a
// file this helper created is ever deleted. The plan is written here and moved
// onto --out only after a complete serialization.
bool reserveScratchFile(const std::wstring& directory, const std::wstring_view outPath,
                        std::wstring& scratch, std::wstring& reason) {
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    const size_t cut = outPath.find_last_of(L"\\/");
    const std::wstring_view stem = cut == std::wstring_view::npos ? outPath : outPath.substr(cut + 1);

    ULONGLONG seed = GetTickCount64() ^ (static_cast<ULONGLONG>(GetCurrentProcessId()) << 32);
    for (int attempt = 0; attempt < 64; ++attempt, ++seed) {
        std::wstring name = std::wstring(stem) + L".mikai-prep-";
        for (int nibble = 0; nibble < 12; ++nibble)
            name.push_back(kHex[(seed >> (nibble * 4)) & 0x0F]);
        name += L".tmp";

        const std::wstring candidate = directory + L"\\" + name;
        const HANDLE reserved = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reserved != INVALID_HANDLE_VALUE) {
            CloseHandle(reserved);
            scratch = candidate;
            return true;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            reason = L"no scratch file could be reserved beside " + std::wstring(outPath) + L" (error " +
                     std::to_wstring(error) + L"); the build was not started";
            return false;
        }
    }
    reason = L"no unique scratch file could be reserved beside " + std::wstring(outPath);
    return false;
}

// The scratch file belongs to this helper alone. A deletion that fails is
// reported rather than swallowed: it would otherwise leave engine-shaped bytes
// next to --out that nothing accounts for. release() runs only after the bytes
// have been moved onto --out.
class ScratchFile {
public:
    ScratchFile() = default;
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
    ~ScratchFile() {
        if (path_.empty())
            return;
        if (!DeleteFileW(path_.c_str())) {
            std::fwprintf(stderr, L"%ls: could not remove the scratch file %ls (error %lu)\n", kName,
                          path_.c_str(), GetLastError());
            std::fflush(stderr);
        }
    }

    void adopt(std::wstring path) { path_ = std::move(path); }
    void release() { path_.clear(); }
    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
};

// Every TensorRT and CUDA object this helper creates is owned here and torn
// down in reverse order, so an unexpected exception unwinding through wmain
// still releases the GPU context and never leaves a scratch file published.
struct Build {
    StderrLogger logger;
    nvinfer1::IBuilder* builder = nullptr;
    nvinfer1::INetworkDefinition* network = nullptr;
    nvonnxparser::IParser* parser = nullptr;
    nvinfer1::IBuilderConfig* config = nullptr;
    cudaStream_t profileStream = nullptr;

    ~Build() {
        if (profileStream != nullptr)
            cudaStreamDestroy(profileStream);
        delete config;
        delete parser;
        delete network;
        delete builder;
    }

    Build() = default;
    Build(const Build&) = delete;
    Build& operator=(const Build&) = delete;
};

bool writeBytes(const std::wstring& path, const void* data, const size_t bytes, std::wstring& reason) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        reason = L"the reserved scratch file could not be opened for writing (error " +
                 std::to_wstring(GetLastError()) + L")";
        return false;
    }
    const auto* cursor = static_cast<const unsigned char*>(data);
    size_t remaining = bytes;
    bool written = true;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1024u * 1024u));
        DWORD done = 0;
        if (!WriteFile(file, cursor, chunk, &done, nullptr) || done == 0) {
            reason = L"the serialized plan could not be written (error " + std::to_wstring(GetLastError()) +
                     L"); --out is unchanged";
            written = false;
            break;
        }
        cursor += done;
        remaining -= done;
    }
    if (written && !FlushFileBuffers(file)) {
        reason = L"the serialized plan could not be flushed to disk (error " +
                 std::to_wstring(GetLastError()) + L"); --out is unchanged";
        written = false;
    }
    CloseHandle(file);
    return written;
}

bool readWholeFile(const std::wstring& path, std::vector<char>& bytes, std::wstring& reason) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        reason = L"--onnx could not be opened (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        reason = L"--onnx has no usable length (error " + std::to_wstring(error) + L")";
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    bool read = true;
    while (done < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 1024u * 1024u));
        DWORD got = 0;
        if (!ReadFile(file, bytes.data() + done, chunk, &got, nullptr) || got == 0) {
            reason = L"--onnx could not be read (error " + std::to_wstring(GetLastError()) + L")";
            read = false;
            break;
        }
        done += got;
    }
    CloseHandle(file);
    return read;
}

// ---------------------------------------------------------------------------
// Graph parsing and shape binding
// ---------------------------------------------------------------------------

std::string dimsJson(const nvinfer1::Dims& dims) {
    std::string out = "[";
    for (int32_t index = 0; index < dims.nbDims; ++index) {
        if (index != 0)
            out.push_back(',');
        out += signedNumber(dims.d[index]);
    }
    out.push_back(']');
    return out;
}

// A tensor name is bounded, NUL-terminated UTF-8, so the comparison against a
// --shapes name is a comparison of the same text.
bool sameName(const nvinfer1::ITensor& tensor, const std::string& requested) {
    return boundedText(tensor.getName(), 4096) == requested;
}

// Every input and output the parsed graph really declares, with its declared
// type, extents and permitted IO format. A dynamic extent is what TensorRT
// reports (-1) and stays -1 here, so the event states what the graph said, not
// what was asked for. The format is set to linear on the way through, so the
// engine is never free to choose a vectorised layout the pipeline cannot hold.
bool describeBindings(nvinfer1::INetworkDefinition& network, std::string& json,
                      std::wstring& reason) {
    const int32_t inputs = network.getNbInputs();
    const int32_t outputs = network.getNbOutputs();
    if (inputs < 1) {
        reason = L"the parsed graph declares no network input, so there is nothing to bind";
        return false;
    }

    std::string bindings;
    bool first = true;
    for (int32_t index = 0; index < inputs; ++index) {
        nvinfer1::ITensor* const tensor = network.getInput(index);
        if (tensor == nullptr) {
            reason = L"network input " + std::to_wstring(index) + L" is absent from the parsed graph";
            return false;
        }
        const std::string name = boundedText(tensor->getName(), 4096);
        if (name.empty()) {
            reason = L"network input " + std::to_wstring(index) + L" has no usable name";
            return false;
        }
        if (tensor->isShapeTensor()) {
            reason = L"network input \"" + wide(name) +
                     L"\" is a shape tensor, whose element values this helper does not set";
            return false;
        }
        if (!supportedInputType(tensor->getType())) {
            reason = L"network input \"" + wide(name) + L"\" is " + wide(typeName(tensor->getType())) +
                     L"; only float32, float16 and bfloat16 network inputs are supported";
            return false;
        }
        tensor->setAllowedFormats(kLinearOnly);
        if (!first)
            bindings.push_back(',');
        first = false;
        bindings += "{\"name\":";
        appendJsonString(bindings, name);
        bindings += ",\"role\":\"input\",\"type\":";
        appendJsonString(bindings, typeName(tensor->getType()));
        bindings += ",\"dims\":";
        bindings += dimsJson(tensor->getDimensions());
        bindings += ",\"format\":\"linear\"";
        bindings.push_back('}');
    }
    for (int32_t index = 0; index < outputs; ++index) {
        nvinfer1::ITensor* const tensor = network.getOutput(index);
        if (tensor == nullptr) {
            reason = L"network output " + std::to_wstring(index) + L" is absent from the parsed graph";
            return false;
        }
        const std::string name = boundedText(tensor->getName(), 4096);
        if (name.empty()) {
            reason = L"network output " + std::to_wstring(index) + L" has no usable name";
            return false;
        }
        tensor->setAllowedFormats(kLinearOnly);
        if (!first)
            bindings.push_back(',');
        first = false;
        bindings += "{\"name\":";
        appendJsonString(bindings, name);
        bindings += ",\"role\":\"output\",\"type\":";
        appendJsonString(bindings, typeName(tensor->getType()));
        bindings += ",\"dims\":";
        bindings += dimsJson(tensor->getDimensions());
        bindings += ",\"format\":\"linear\"";
        bindings.push_back('}');
    }

    json = std::move(bindings);
    return true;
}

// One fixed shape set per real network input: min = opt = max, so the plan binds
// shapes instead of becoming a dynamic-shape plan. A declared static extent that
// disagrees with the requested one is a refusal, not something to bind over.
bool bindShapes(nvinfer1::IBuilder& builder, nvinfer1::INetworkDefinition& network,
                const std::vector<ShapeSpec>& shapes, nvinfer1::IBuilderConfig& config,
                std::wstring& reason) {
    const int32_t count = network.getNbInputs();
    if (static_cast<size_t>(count) != shapes.size()) {
        reason = L"--shapes names " + std::to_wstring(shapes.size()) + L" input(s) but the graph declares " +
                 std::to_wstring(count);
        return false;
    }

    std::vector<const nvinfer1::ITensor*> requested;
    requested.reserve(shapes.size());
    bool dynamic = false;
    for (const ShapeSpec& shape : shapes) {
        const nvinfer1::ITensor* match = nullptr;
        for (int32_t index = 0; index < count && match == nullptr; ++index) {
            const nvinfer1::ITensor* const candidate = network.getInput(index);
            if (candidate != nullptr && sameName(*candidate, shape.name))
                match = candidate;
        }
        if (match == nullptr) {
            reason = L"--shapes names input \"" + wide(shape.name) +
                     L"\", which the graph does not declare as a network input";
            return false;
        }
        requested.push_back(match);

        const nvinfer1::Dims declared = match->getDimensions();
        if (declared.nbDims != 4) {
            reason = L"network input \"" + wide(shape.name) + L"\" is rank-" +
                     std::to_wstring(declared.nbDims) + L", not rank-4";
            return false;
        }
        for (int32_t axis = 0; axis < 4; ++axis) {
            if (declared.d[axis] < 0) {
                dynamic = true;
                continue;
            }
            if (declared.d[axis] != shape.extent[axis]) {
                reason = L"--shapes gives input \"" + wide(shape.name) + L"\" extent " +
                         std::to_wstring(shape.extent[axis]) + L" on axis " + std::to_wstring(axis) +
                         L", but the graph declares " + std::to_wstring(declared.d[axis]);
                return false;
            }
        }
    }

    if (!dynamic)
        return true;

    nvinfer1::IOptimizationProfile* const profile = builder.createOptimizationProfile();
    if (profile == nullptr) {
        reason = L"TensorRT would not create an optimization profile for a graph with dynamic inputs";
        return false;
    }
    for (size_t index = 0; index < requested.size(); ++index) {
        nvinfer1::Dims dims{};
        dims.nbDims = 4;
        for (int32_t axis = 0; axis < 4; ++axis)
            dims.d[axis] = shapes[index].extent[axis];
        const char* const name = requested[index]->getName();
        if (!profile->setDimensions(name, nvinfer1::OptProfileSelector::kMIN, dims) ||
            !profile->setDimensions(name, nvinfer1::OptProfileSelector::kOPT, dims) ||
            !profile->setDimensions(name, nvinfer1::OptProfileSelector::kMAX, dims)) {
            reason = L"TensorRT refused the fixed shape set for input \"" + wide(shapes[index].name) +
                     L"\" (rank-4, min = opt = max)";
            return false;
        }
    }
    if (config.addOptimizationProfile(profile) < 0) {
        reason = L"TensorRT refused the optimization profile carrying the requested shape sets";
        return false;
    }
    return true;
}

}  // namespace

namespace {

// The device record is stated once and reused by both progress events, so the
// two can never disagree about which adapter answered.
void appendDeviceFacts(std::string& json, const DeviceFacts& facts, const cudaUUID_t& requested,
                       const uint64_t requestedWorkspace, const bool workspaceFromFlag) {
    appendNumber(json, "deviceOrdinal", static_cast<uint64_t>(facts.ordinal));
    appendField(json, "deviceUuid", quoted(uuidText(requested)));
    appendField(json, "deviceName", quoted(wide(facts.name)));
    appendField(json, "computeCapability",
                quoted(std::to_wstring(facts.major) + L"." + std::to_wstring(facts.minor)));
    appendNumber(json, "totalGlobalMemoryBytes", facts.totalGlobalMemory);
    appendNumber(json, "multiProcessorCount", static_cast<uint64_t>(facts.multiProcessorCount));
    if (facts.runtimeVersion != 0)
        appendNumber(json, "cudaRuntimeVersion", static_cast<uint64_t>(facts.runtimeVersion));
    if (facts.driverApiVersion != 0)
        appendNumber(json, "cudaDriverApiVersion", static_cast<uint64_t>(facts.driverApiVersion));
    appendField(json, "cudaDriverFileVersion",
                facts.driverFileVersion.empty() ? "\"unknown\"" : quoted(facts.driverFileVersion));
    appendNumber(json, "tensorRtLibVersion", static_cast<uint64_t>(getInferLibVersion()));
    appendNumber(json, "workspaceBytesRequested", requestedWorkspace);
    appendField(json, "workspaceBytesSource", workspaceFromFlag ? "\"flag\"" : "\"default\"");
}

const char* capabilityName(const nvinfer1::EngineCapability capability) {
    switch (capability) {
        case nvinfer1::EngineCapability::kSTANDARD: return "STANDARD";
        case nvinfer1::EngineCapability::kSAFETY: return "SAFETY";
        case nvinfer1::EngineCapability::kDLA_STANDALONE: return "DLA_STANDALONE";
    }
    return "unknown";
}

void appendBuildFacts(std::string& json, nvinfer1::IBuilderConfig& config,
                      const nvinfer1::INetworkDefinition& network) {
    appendNumber(json, "builderFlags", static_cast<uint64_t>(config.getFlags()));
    appendField(json, "tf32Allowed", config.getFlag(nvinfer1::BuilderFlag::kTF32) ? "true" : "false");
    std::string capability = "\"";
    capability += capabilityName(config.getEngineCapability());
    capability += "\"";
    appendField(json, "engineCapability", capability);
    appendField(json, "builderOptimizationLevel",
                 signedNumber(static_cast<int64_t>(config.getBuilderOptimizationLevel())));
    appendNumber(json, "tacticSources", static_cast<uint64_t>(config.getTacticSources()));
    appendNumber(json, "networkFlags", static_cast<uint64_t>(network.getFlags()));
    // TensorRT 11 removed weak typing: a network created with createNetworkV2(0)
    // is always strongly typed, and kSTRONGLY_TYPED is deprecated and ignored.
    // That is an API contract, not a value this process can read back from the
    // config, so the field is paired with its basis and must never be read as a
    // measurement.
    appendField(json, "stronglyTyped", "true");
    appendField(json, "stronglyTypedBasis",
                "\"trt11-api-contract: createNetworkV2(0) is strongly typed; kSTRONGLY_TYPED is "
                "deprecated and has no readback\"");
}

// A refusal in the --describe-device mode is one JSON object too, so a caller
// parses one schema: the same schema id and protocol as the record it asked for,
// described:false, and a named error code instead of a bare exit status.
int reportDescribeFailure(const ExitCode code, const char* phase, const char* errorCode,
                          const std::wstring& reason) {
    std::string json = "{\"schema\":\"mikai.native.preparation.environment/1\"";
    appendNumber(json, "protocol", 1);
    appendField(json, "tool", "\"mikai_native_prep\"");
    appendField(json, "described", "false");
    appendField(json, "ok", "false");
    json += ",\"phase\":";
    appendJsonString(json, phase);
    json += ",\"error\":{\"code\":";
    appendJsonString(json, errorCode);
    json += ",\"message\":";
    appendJsonString(json, utf8(reason));
    json += "}}";
    emit(std::move(json));
    std::fwprintf(stderr, L"%ls: %hs/%hs: %ls\n", kName, phase, errorCode, reason.c_str());
    std::fflush(stderr);
    return code;
}

// The describe mode may run without a named adapter, because it reports the
// machine rather than building for a plan. The adapter it then reports is the
// first one this machine has, read from CUDA here and passed through the same
// selection rule the build path uses - never an ordinal recorded without asking
// the runtime what that ordinal is.
bool firstDeviceUuid(cudaUUID_t* uuid, std::wstring& reason) {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count < 1) {
        reason = L"no CUDA device is present on this machine, so there is no device to describe";
        return false;
    }
    cudaDeviceProp properties{};
    if (cudaGetDeviceProperties(&properties, 0) != cudaSuccess) {
        reason = L"the CUDA runtime would not describe device 0";
        return false;
    }
    std::memcpy(uuid->bytes, properties.uuid.bytes, sizeof uuid->bytes);
    return true;
}

// --describe-device: the real device, runtime and builder policy, with no ONNX,
// no deserialization, no build, no inference and no write. Nothing here is
// copied from a decoder or from an earlier event: the device is selected by the
// same UUID rule the build uses, its compute capability is queried from CUDA,
// and the builder policy is read back from a real IBuilderConfig created with
// the same flags the build path sets.
int describeDevice(const Options& options) {
    if (!options.onnx.empty() || !options.out.empty() || !options.shapes.empty())
        return reportDescribeFailure(kUsage, "arguments", "describe.arguments",
                                     L"--describe-device reads no graph and writes no plan, so --onnx, "
                                     L"--out and --shapes cannot be given with it");
    std::wstring reason;
    cudaUUID_t requested{};
    const bool uuidRequested = !options.deviceUuid.empty();
    if (uuidRequested) {
        if (!parseUuid(options.deviceUuid, requested, reason))
            return reportDescribeFailure(kUsage, "arguments", "describe.deviceUuid", reason);
    } else if (!firstDeviceUuid(&requested, reason)) {
        return reportDescribeFailure(kPreflight, "device", "describe.device.notFound", reason);
    }

    // Whichever way the adapter was named, it goes through the same selection
    // rule the build path uses, and the record says whether the caller named it.
    DeviceFacts facts;
    if (!selectDevice(requested, facts, reason))
        return reportDescribeFailure(kPreflight, "device", "describe.device.notFound", reason);

    Build build;
    build.builder = nvinfer1::createInferBuilder(build.logger);
    if (build.builder == nullptr)
        return reportDescribeFailure(kSetup, "runtime", "describe.runtime.builderUnavailable",
                                     L"TensorRT would not create a builder on this machine, so this "
                                     L"helper's builder policy cannot be read back");
    build.network = build.builder->createNetworkV2(0);
    if (build.network == nullptr)
        return reportDescribeFailure(kSetup, "runtime", "describe.runtime.networkUnavailable",
                                     L"TensorRT would not create a network definition, so the recorded "
                                     L"network flags cannot be read back");
    build.config = build.builder->createBuilderConfig();
    if (build.config == nullptr)
        return reportDescribeFailure(kSetup, "runtime", "describe.runtime.configUnavailable",
                                     L"TensorRT would not create a builder configuration, so the "
                                     L"recorded builder policy cannot be read back");

    build.config->clearFlag(nvinfer1::BuilderFlag::kTF32);
    if (build.config->getFlag(nvinfer1::BuilderFlag::kTF32))
        return reportDescribeFailure(kSetup, "policy", "describe.policy.tf32Allowed",
                                     L"TensorRT still reports TF32 allowed after clearing "
                                     L"BuilderFlag::kTF32; the policy this helper would build with "
                                     L"cannot be recorded");

    build.config->setBuilderOptimizationLevel(options.builderOptimizationLevel);
    const int32_t effectiveLevel = build.config->getBuilderOptimizationLevel();
    if (effectiveLevel != options.builderOptimizationLevel)
        return reportDescribeFailure(kSetup, "policy", "describe.policy.optimizationLevel",
                                     L"TensorRT reports builder optimization level " +
                                         std::to_wstring(effectiveLevel) + L" against the requested " +
                                         std::to_wstring(options.builderOptimizationLevel));

    build.config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE,
                                     static_cast<size_t>(options.workspaceBytes));
    const size_t effectiveWorkspace =
        build.config->getMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE);
    if (effectiveWorkspace != static_cast<size_t>(options.workspaceBytes))
        return reportDescribeFailure(kSetup, "policy", "describe.policy.workspaceBound",
                                     L"TensorRT reports a workspace limit of " +
                                         std::to_wstring(effectiveWorkspace) +
                                         L" bytes against the requested " +
                                         std::to_wstring(options.workspaceBytes));

    // One flat document: the measured device and build facts keep the exact key
    // names and types the build events already record, so a describe record and
    // a build log compare key by key. The policy block is named as policy
    // because precision and skipInference are statements about what this helper
    // prepares, not readbacks from the device.
    std::string json = "{\"schema\":\"mikai.native.preparation.environment/1\"";
    appendNumber(json, "protocol", 1);
    appendField(json, "tool", "\"mikai_native_prep\"");
    appendField(json, "described", "true");
    appendField(json, "ok", "true");
    // Proves the mode's promise in the record itself: nothing was serialized.
    appendNumber(json, "builtEngineBytes", 0);
    appendField(json, "uuidRequested", uuidRequested ? "true" : "false");
    appendDeviceFacts(json, facts, requested, options.workspaceBytes, options.workspaceGiven);
    appendBuildFacts(json, *build.config, *build.network);
    if (options.builderOptimizationLevelGiven)
        appendField(json, "builderOptimizationLevelRequested",
                    signedNumber(static_cast<int64_t>(options.builderOptimizationLevel)));
    appendNumber(json, "workspaceBytes", static_cast<uint64_t>(effectiveWorkspace));
    appendField(json, "workspacePool", "\"WORKSPACE\"");
    appendField(json, "optimizationProfile", "\"fixed-per-input\"");
    appendField(json, "policy",
                "{\"precision\":\"fp16\",\"skipInference\":true}");
    // No plugin version is queryable from this mode: TensorRT exposes only
    // per-creator plugin API versions (IPluginCreator::getPluginVersion) and the
    // NVIDIA binaries carry no Win32 version resource, so the TensorRT release
    // is not the plugin's version either. Unknown stays unknown; the plugin
    // revision is the caller's, taken from the validated bundle's backend
    // content revision, and this record never states a value it did not read.
    appendField(json, "pluginVersion", "\"unknown\"");
    json.push_back('}');
    emit(std::move(json));
    return 0;
}

int run(const int argc, wchar_t** argv) {
    Options options;
    std::wstring reason;
    if (!parseArguments(argc, argv, options, reason)) {
        if (options.describeDevice)
            return reportDescribeFailure(kUsage, "arguments", "describe.arguments", reason);
        return report(kUsage, reason);
    }
    if (options.jsonRequested && !options.describeDevice)
        return report(kUsage, L"--json is the machine-readable form of --describe-device and is only "
                               L"meaningful with it");
    if (options.describeDevice)
        return describeDevice(options);

    std::vector<ShapeSpec> shapes;
    if (!parseShapes(options.shapes, shapes, reason))
        return report(kUsage, reason);

    cudaUUID_t requested{};
    if (!parseUuid(options.deviceUuid, requested, reason))
        return report(kUsage, reason);

    uint64_t onnxBytes = 0;
    if (!isExistingFile(options.onnx, &onnxBytes) || onnxBytes == 0)
        return report(kPreflight, L"--onnx is missing, empty or not a file: " + options.onnx);
    if (pathExists(options.out))
        return report(kPreflight, L"--out already exists and is never replaced: " + options.out);

    DeviceFacts facts;
    if (!selectDevice(requested, facts, reason))
        return report(kPreflight, reason);

    const bool workspaceFromFlag = options.workspaceGiven;
    std::string deviceEvent = "{\"event\":\"device-selected\"";
    appendDeviceFacts(deviceEvent, facts, requested, options.workspaceBytes, workspaceFromFlag);
    deviceEvent.push_back('}');
    emit(std::move(deviceEvent));

    std::vector<char> graph;
    if (!readWholeFile(options.onnx, graph, reason))
        return report(kSetup, reason);

    std::string graphSha256;
    if (!sha256Hex(graph.data(), graph.size(), graphSha256))
        graphSha256.clear();

    Build build;
    build.builder = nvinfer1::createInferBuilder(build.logger);
    if (build.builder == nullptr)
        return report(kSetup, L"TensorRT would not create a builder for this machine");

    build.network = build.builder->createNetworkV2(0);
    // NvInfer.h: the network is always strongly typed and kSTRONGLY_TYPED is
    // deprecated and ignored, so flags 0 already carries the contract's
    // stronglyTyped=true with nothing further to set.
    if (build.network == nullptr)
        return report(kSetup, L"TensorRT would not create a network definition");

    build.parser = nvonnxparser::createParser(*build.network, build.logger);
    if (build.parser == nullptr)
        return report(kSetup, L"the ONNX parser could not be created");

    ScratchFile scratch;
    {
        std::wstring reserved;
        if (!reserveScratchFile(parentDirectory(options.out), options.out, reserved, reason))
            return report(kPreflight, reason);
        scratch.adopt(std::move(reserved));
    }

    // Parsed from the bytes read here, never by path, for two reasons at once:
    // the graph that is bound is the same snapshot that was hashed (no TOCTOU),
    // and a Unicode path cannot be mistranslated by any code page. No model path
    // is handed back, so the parser cannot pull in external weight files that no
    // recorded hash covers; a graph that needs them fails here, by name.
    if (!build.parser->parse(graph.data(), graph.size(), nullptr)) {
        const int errors = build.parser->getNbErrors();
        std::fflush(stdout);
        if (errors <= 0) {
            std::fwprintf(stderr, L"%ls: the ONNX parser reported failure without naming a cause\n", kName);
        } else {
            for (int index = 0; index < errors; ++index) {
                const nvonnxparser::IParserError* const error = build.parser->getError(index);
                if (error == nullptr)
                    continue;
                std::fprintf(stderr,
                             "%ls: parser error %d of %d: %s\n  at %s:%d in %s\n  node %d \"%s\" op \"%s\"\n",
                             kName, index + 1, errors, error->desc() != nullptr ? error->desc() : "",
                             error->file() != nullptr ? error->file() : "", error->line(),
                             error->func() != nullptr ? error->func() : "", error->node(),
                             error->nodeName() != nullptr ? error->nodeName() : "",
                             error->nodeOperator() != nullptr ? error->nodeOperator() : "");
            }
        }
        std::fflush(stderr);
        return report(kBuild, errors <= 0
                                 ? L"the ONNX graph could not be parsed; the parser named no cause"
                                 : L"the ONNX graph could not be parsed; the parser errors above name the "
                                   L"cause. --out is unchanged");
    }

    std::string bindings;
    if (!describeBindings(*build.network, bindings, reason))
        return report(kBuild, reason);

    std::string parsedEvent = "{\"event\":\"graph-parsed\"";
    appendField(parsedEvent, "onnx", quoted(options.onnx));
    appendNumber(parsedEvent, "onnxBytes", onnxBytes);
    if (!graphSha256.empty())
        appendField(parsedEvent, "onnxSha256", "\"" + graphSha256 + "\"");
    appendNumber(parsedEvent, "inputCount", static_cast<uint64_t>(build.network->getNbInputs()));
    appendNumber(parsedEvent, "outputCount", static_cast<uint64_t>(build.network->getNbOutputs()));
    appendField(parsedEvent, "ioFormat", "\"linear-only\"");
    appendField(parsedEvent, "bindings", "[" + bindings + "]");
    parsedEvent.push_back('}');
    emit(std::move(parsedEvent));

    build.config = build.builder->createBuilderConfig();
    if (build.config == nullptr)
        return report(kSetup, L"TensorRT would not create a builder configuration");

    // TensorRT enables TF32 by default; artifact identity requires it disabled.
    build.config->clearFlag(nvinfer1::BuilderFlag::kTF32);
    if (build.config->getFlag(nvinfer1::BuilderFlag::kTF32)) {
        return report(kSetup, L"TensorRT still reports TF32 allowed after clearing BuilderFlag::kTF32; "
                               L"the no-TF32 contract cannot be met, so nothing was built");
    }

    // The builder optimization level is the one knob the author's own recipe
    // sets (their trtexec settings carry --builderOptimizationLevel=5) and the
    // only lever this helper was missing to reproduce that plan. It bounds the
    // tactic search only: no dtype, no layout, no shape and no tolerance moves.
    // It is read back before the build, because a level the builder silently
    // ignored could not then be recorded next to the plan it produced.
    build.config->setBuilderOptimizationLevel(options.builderOptimizationLevel);
    const int32_t effectiveLevel = build.config->getBuilderOptimizationLevel();
    if (effectiveLevel != options.builderOptimizationLevel) {
        return report(kSetup, L"TensorRT reports builder optimization level " +
                                  std::to_wstring(effectiveLevel) + L" against the requested " +
                                  std::to_wstring(options.builderOptimizationLevel) +
                                  L"; a level that was not honoured cannot be recorded as measured, so "
                                  L"nothing was built");
    }

    if (!bindShapes(*build.builder, *build.network, shapes, *build.config, reason))
        return report(kBuild, reason);

    build.config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE,
                                     static_cast<size_t>(options.workspaceBytes));
    const size_t effectiveWorkspace =
        build.config->getMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE);
    if (effectiveWorkspace != static_cast<size_t>(options.workspaceBytes)) {
        return report(kSetup, L"TensorRT did not apply the requested workspace limit; it reports " +
                                  std::to_wstring(effectiveWorkspace) + L" bytes against the " +
                                  std::to_wstring(options.workspaceBytes) +
                                  L" requested, and the builder errors above say why. A bound that was "
                                  L"silently ignored cannot be recorded as honoured");
    }

    int profilePriority = 0;
    if (!createProfileStream(build.profileStream, profilePriority, reason))
        return report(kSetup, reason);
    build.config->setProfileStream(build.profileStream);

    std::string startedEvent = "{\"event\":\"build-started\"";
    appendNumber(startedEvent, "workspaceBytes", static_cast<uint64_t>(effectiveWorkspace));
    appendNumber(startedEvent, "workspaceBytesRequested", options.workspaceBytes);
    appendField(startedEvent, "workspacePool", "\"WORKSPACE\"");
    appendField(startedEvent, "optimizationProfile", "\"fixed-per-input\"");
    appendField(startedEvent, "profileStreamPriority", signedNumber(profilePriority));
    appendField(startedEvent, "profileStreamNonBlocking", "true");
    appendBuildFacts(startedEvent, *build.config, *build.network);
    if (options.builderOptimizationLevelGiven)
        appendField(startedEvent, "builderOptimizationLevelRequested",
                    signedNumber(static_cast<int64_t>(options.builderOptimizationLevel)));
    startedEvent.push_back('}');
    emit(std::move(startedEvent));

    std::unique_ptr<nvinfer1::IHostMemory> blob(
        build.builder->buildSerializedNetwork(*build.network, *build.config));
    const size_t engineBytes = blob ? blob->size() : 0;
    if (!blob || engineBytes == 0 || blob->data() == nullptr)
        return report(kBuild, L"TensorRT built no plan for this graph, shape set and device; the builder "
                               L"log above names the cause. --out is unchanged");

    std::string engineSha256;
    const bool hashed = sha256Hex(blob->data(), engineBytes, engineSha256);

    std::string serializedEvent = "{\"event\":\"serialized\"";
    appendNumber(serializedEvent, "engineBytes", static_cast<uint64_t>(engineBytes));
    serializedEvent.push_back('}');
    emit(std::move(serializedEvent));

    // Streamed straight out of TensorRT's own buffer into the reserved scratch
    // file, so no second copy of the plan exists in this process.
    std::wstring writeFailure;
    const bool written = writeBytes(scratch.path(), blob->data(), engineBytes, writeFailure);
    blob.reset();
    if (!written)
        return report(kBuild, writeFailure);

    // Without MOVEFILE_REPLACE_EXISTING, a path that appeared during the build
    // is a refusal rather than something to overwrite.
    if (!MoveFileExW(scratch.path().c_str(), options.out.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        return report(kPublish, L"the built plan could not be moved to " + options.out + L" (error " +
                                    std::to_wstring(error) +
                                    L"); the scratch plan was discarded and --out is unchanged");
    }
    scratch.release();

    std::string raw = "{\"event\":\"raw-produced\"";
    appendField(raw, "out", quoted(options.out));
    appendNumber(raw, "engineBytes", static_cast<uint64_t>(engineBytes));
    if (hashed)
        appendField(raw, "engineSha256", "\"" + engineSha256 + "\"");
    appendField(raw, "onnx", quoted(options.onnx));
    appendNumber(raw, "onnxBytes", onnxBytes);
    if (!graphSha256.empty())
        appendField(raw, "onnxSha256", "\"" + graphSha256 + "\"");
    appendDeviceFacts(raw, facts, requested, options.workspaceBytes, workspaceFromFlag);
    appendField(raw, "shapeBinding", "\"fixed-per-input\"");
    appendField(raw, "ioFormat", "\"linear-only\"");
    appendNumber(raw, "workspaceBytes", static_cast<uint64_t>(effectiveWorkspace));
    appendNumber(raw, "workspaceBytesRequested", options.workspaceBytes);
    appendBuildFacts(raw, *build.config, *build.network);
    if (options.builderOptimizationLevelGiven)
        appendField(raw, "builderOptimizationLevelRequested",
                    signedNumber(static_cast<int64_t>(options.builderOptimizationLevel)));
    appendField(raw, "rawVerified", "false");
    raw.push_back('}');
    emit(std::move(raw));

    std::fwprintf(stderr,
                  L"%ls: wrote %ls (%zu bytes) of RAW TensorRT builder output, unverified here. Only the "
                  L"native verification step and the artifact store may publish it.\n",
                  kName, options.out.c_str(), engineBytes);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& failure) {
        return report(kBuild, L"an unexpected failure interrupted the build: " + wide(failure.what()));
    } catch (...) {
        return report(kBuild, L"an unexpected failure of unknown kind interrupted the build; --out is "
                               L"unchanged");
    }
}
