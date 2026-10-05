// mikai_native_verify - run one raw serialized engine on one explicitly named
// CUDA device and measure its real outputs against CPU references. Evidence
// only: it publishes nothing, and a run here makes no plan prepared or active.
//
// Two invariants the shape handling depends on:
//   * a declared -1 is not a failure - the HD graph has dynamic H/W and a
//     symbolic output batch, so only the shape a real execution context
//     resolves is a fact about this plan;
//   * the digest is taken over the same buffer handed to deserializeCudaEngine,
//     so "verified bytes" and "deserialized bytes" cannot drift apart.
//
// Protocol: exactly one JSON object on stdout, always - ok:true with the full
// report, or ok:false with phase, error{code,message}, bindings already known,
// and an outputs array carrying every measurement actually made before the
// refusal. Both envelopes name the engine bytes, the device and the TensorRT
// library the run was made against, for every part of that identity that was
// established before the refusal: a rejection that did not name its engine
// could not be attributed to any file. A measured output also says where its
// deviation sits - the worst element's coordinate, how many elements are
// outside the absolute bound, and how many of those are on a spatial edge -
// because a precision divergence and a staging or padding error are told apart
// by where the error is, not by how large its maximum is. TensorRT and CUDA
// messages go to stderr.

#include <windows.h>

#include <bcrypt.h>

#include <cuda.h>

#include <NvInferRuntime.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kName[] = L"mikai_native_verify";

enum ExitCode {
    kUsage = 2,
    kPreflight = 3,
    kDigest = 4,
    kDevice = 5,
    kDeserialize = 6,
    kBinding = 7,
    kExecution = 8,
    kNumerics = 9,
    kInternal = 10,
};

constexpr double kFp16MaxAbsoluteError = 0.02;
constexpr double kFp16MaxRmse = 0.005;
constexpr double kFp32MaxAbsoluteError = 0.0002;
constexpr double kFp32MaxRmse = 0.00005;

bool equalsNoCase(const std::wstring_view a, const std::wstring_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i]))
            return false;
    }
    return true;
}

std::string utf8FromWide(const std::wstring_view text) {
    if (text.empty())
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed, nullptr,
                        nullptr);
    return out;
}

std::wstring wideFromUtf8(const std::string_view text) {
    if (text.empty())
        return {};
    const int needed =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

// TensorRT returns tensor names as narrow UTF-8; diagnostics are wide. Every
// message converts here, so no name is ever concatenated across the two.
std::wstring quoted(const std::string& name) {
    return L"\"" + wideFromUtf8(name) + L"\"";
}

// -- element types

enum class ElementType { Fp16, Fp32 };

const wchar_t* elementTypeName(const ElementType type) {
    return type == ElementType::Fp16 ? L"fp16" : L"fp32";
}

size_t elementTypeSize(const ElementType type) {
    return type == ElementType::Fp16 ? 2u : 4u;
}

bool parseElementType(const std::wstring_view text, ElementType& type, std::wstring& reason) {
    if (text == L"fp16") {
        type = ElementType::Fp16;
        return true;
    }
    if (text == L"fp32") {
        type = ElementType::Fp32;
        return true;
    }
    reason = L"element type \"" + std::wstring(text) + L"\" is not fp16 or fp32";
    return false;
}

// Subnormals and the infinities survive: folding them away would hide a real
// failure rather than report it.
float halfToFloat(const uint16_t bits) {
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

template <typename T>
T loadAs(const void* source, const size_t index) {
    T value;
    std::memcpy(&value, static_cast<const unsigned char*>(source) + index * sizeof(T), sizeof(T));
    return value;
}

float elementAsFloat(const void* source, const size_t index, const ElementType type) {
    return type == ElementType::Fp16 ? halfToFloat(loadAs<uint16_t>(source, index))
                                     : loadAs<float>(source, index);
}

// -- SHA-256 and files

bool sha256Hex(const unsigned char* bytes, const size_t count, std::wstring& hex, std::wstring& reason) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    // Flags 0, not BCRYPT_ALG_HANDLE_HMAC_FLAG: that flag selects the keyed
    // HMAC provider, whose digest is keyed with an empty secret and so is NOT
    // the SHA-256 every manifest, the prep helper and this tool's --engine-sha256
    // mean. A plain provider is what makes the digest comparable.
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        reason = L"BCrypt has no SHA-256 provider on this machine, so no digest can be measured";
        return false;
    }
    struct AlgorithmCloser {
        BCRYPT_ALG_HANDLE handle;
        ~AlgorithmCloser() { BCryptCloseAlgorithmProvider(handle, 0); }
    } closer{algorithm};

    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD resultBytes = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                          sizeof(objectBytes), &resultBytes, 0) != 0) {
        reason = L"BCrypt did not report a SHA-256 object length";
        return false;
    }
    std::vector<unsigned char> object(objectBytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) != 0) {
        reason = L"BCrypt could not create a SHA-256 hash object";
        return false;
    }
    struct HashCloser {
        BCRYPT_HASH_HANDLE handle;
        ~HashCloser() { BCryptDestroyHash(handle); }
    } hashCloser{hash};

    // ULONG-sized chunks: BCryptHashData takes a ULONG, so a bigger plan would
    // otherwise be hashed truncated to its low 32 bits.
    for (size_t offset = 0; offset < count;) {
        const size_t remaining = count - offset;
        const ULONG chunk =
            static_cast<ULONG>(remaining > MAXDWORD ? static_cast<size_t>(MAXDWORD) : remaining);
        if (BCryptHashData(hash, const_cast<PUCHAR>(bytes + offset), chunk, 0) != 0) {
            reason = L"BCrypt could not hash the bytes it was given";
            return false;
        }
        offset += chunk;
    }
    unsigned char digest[32];
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) {
        reason = L"BCrypt could not finish the digest";
        return false;
    }

    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    hex.assign(sizeof(digest) * 2, L'0');
    for (size_t i = 0; i < sizeof(digest); ++i) {
        hex[i * 2] = kHex[digest[i] >> 4];
        hex[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    return true;
}

bool parseExpectedDigest(const std::wstring_view text, std::wstring& lower, std::wstring& reason) {
    lower.assign(text);
    for (wchar_t& c : lower) {
        if (c >= L'A' && c <= L'F')
            c = static_cast<wchar_t>(c - L'A' + L'a');
        else if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) {
            reason = L"--engine-sha256 \"" + std::wstring(text) + L"\" is not hexadecimal";
            return false;
        }
    }
    if (lower.size() != 64) {
        reason = L"--engine-sha256 \"" + std::wstring(text) + L"\" is " + std::to_wstring(lower.size()) +
                 L" characters, not 64";
        return false;
    }
    return true;
}

bool readWholeFile(const std::wstring_view path, std::vector<unsigned char>& bytes, std::wstring& reason) {
    const std::wstring owned(path);
    const HANDLE file = CreateFileW(owned.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        reason = L"cannot open " + owned + L" (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    struct FileCloser {
        HANDLE handle;
        ~FileCloser() { CloseHandle(handle); }
    } closer{file};

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        reason = owned + L" is empty or its size could not be read";
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t filled = 0;
    while (filled < bytes.size()) {
        const size_t remaining = bytes.size() - filled;
        const DWORD chunk = static_cast<DWORD>(remaining > 0x10000000u ? 0x10000000u : remaining);
        DWORD read = 0;
        if (!ReadFile(file, bytes.data() + filled, chunk, &read, nullptr) || read == 0) {
            reason = L"cannot read " + owned + L" (error " + std::to_wstring(GetLastError()) +
                     L"); the file is not the size it reported";
            return false;
        }
        filled += read;
    }
    return true;
}

// -- command line

struct TensorRequest {
    std::string name;
    std::wstring nameText;
    ElementType type = ElementType::Fp16;
    int64_t dims[4] = {1, 1, 1, 1};
    uint64_t elements = 1;
    std::wstring path;
    /// The digest of the bytes actually read from `path`, so a report names the
    /// engine arg bytes it fed and the reference bytes it compared against
    /// instead of only a file name that may have moved on since.
    std::wstring fileSha256;
    std::vector<unsigned char> host;  // input bytes, or the CPU reference bytes
};

struct Options {
    std::wstring engine;
    std::wstring engineDigest;
    std::wstring deviceUuid;
    std::vector<TensorRequest> inputs;
    std::vector<TensorRequest> outputs;
    bool absoluteErrorOverride = false;
    bool rmseOverride = false;
    double maxAbsoluteError = 0.0;
    double maxRmse = 0.0;
};

bool isFlag(const std::wstring_view text) {
    return text.size() > 2 && text[0] == L'-' && text[1] == L'-';
}

bool takeValue(const int argc, wchar_t** argv, int& index, const std::wstring_view flag,
               std::wstring& value, std::wstring& reason) {
    if (index + 1 >= argc) {
        reason = L"argument " + std::wstring(flag) + L" has no value";
        return false;
    }
    const std::wstring_view candidate = argv[index + 1];
    if (candidate.empty() || isFlag(candidate)) {
        reason = L"argument " + std::wstring(flag) + L" has no usable value";
        return false;
    }
    value = candidate;
    ++index;
    return true;
}

// Engine IO names are arbitrary strings; only empty or control-bearing ones
// cannot be compared or printed unambiguously.
bool validTensorName(const std::wstring_view name, std::wstring& reason) {
    if (name.empty()) {
        reason = L"a tensor name is empty";
        return false;
    }
    for (const wchar_t c : name) {
        if (c < 0x20 || c == 0x7F) {
            reason = L"tensor name \"" + std::wstring(name) + L"\" carries a control character";
            return false;
        }
    }
    return true;
}

bool parseShape(const std::wstring_view text, int64_t (&dims)[4], uint64_t& elements, std::wstring& reason) {
    int32_t rank = 0;
    size_t at = 0;
    for (;;) {
        const size_t cross = text.find(L'x', at);
        const std::wstring_view digits = text.substr(at, cross - at);
        if (digits.empty()) {
            reason = L"shape \"" + std::wstring(text) + L"\" has an empty extent";
            return false;
        }
        uint64_t extent = 0;
        for (const wchar_t c : digits) {
            if (c < L'0' || c > L'9') {
                reason = L"shape \"" + std::wstring(text) + L"\" has a non-numeric extent";
                return false;
            }
            const uint64_t digit = static_cast<uint64_t>(c - L'0');
            if (extent > (UINT64_MAX - digit) / 10) {
                reason = L"shape \"" + std::wstring(text) + L"\" has an extent that does not fit";
                return false;
            }
            extent = extent * 10 + digit;
        }
        if (extent == 0) {
            reason = L"shape \"" + std::wstring(text) + L"\" has a zero extent";
            return false;
        }
        if (++rank > 4) {
            reason = L"shape \"" + std::wstring(text) + L"\" is not rank 4 (NxCxHxW)";
            return false;
        }
        dims[rank - 1] = static_cast<int64_t>(extent);
        if (cross == std::wstring_view::npos)
            break;
        at = cross + 1;
    }
    if (rank != 4) {
        reason = L"shape \"" + std::wstring(text) + L"\" is not rank 4 (NxCxHxW)";
        return false;
    }
    elements = static_cast<uint64_t>(dims[0]) * static_cast<uint64_t>(dims[1]) *
               static_cast<uint64_t>(dims[2]) * static_cast<uint64_t>(dims[3]);
    if (elements > (1ull << 32)) {
        reason = L"shape \"" + std::wstring(text) + L"\" names " + std::to_wstring(elements) +
                 L" elements, which this verifier refuses to bind";
        return false;
    }
    return true;
}

bool parseTolerance(const std::wstring_view text, const std::wstring_view flag, double& value,
                    std::wstring& reason) {
    const std::wstring owned(text);
    wchar_t* tail = nullptr;
    const double parsed = std::wcstod(owned.c_str(), &tail);
    if (tail == owned.c_str() || *tail != L'\0' || !std::isfinite(parsed) || parsed <= 0.0) {
        reason = std::wstring(flag) + L" must be a finite positive number, not \"" + owned + L"\"";
        return false;
    }
    value = parsed;
    return true;
}

bool parseTensorRequest(const int argc, wchar_t** argv, int& index, const std::wstring_view flag,
                        TensorRequest& request, std::wstring& reason) {
    if (index + 4 >= argc) {
        reason = std::wstring(flag) + L" needs <name> <fp16|fp32> <NxCxHxW> <file>";
        return false;
    }
    for (int offset = 1; offset <= 4; ++offset) {
        if (isFlag(argv[index + offset])) {
            reason = std::wstring(flag) + L" needs <name> <fp16|fp32> <NxCxHxW> <file>, but found " +
                     std::wstring(argv[index + offset]);
            return false;
        }
    }
    request.nameText = argv[index + 1];
    request.name = utf8FromWide(request.nameText);
    if (request.name.empty() || !validTensorName(request.nameText, reason))
        return false;
    if (!parseElementType(argv[index + 2], request.type, reason))
        return false;
    if (!parseShape(argv[index + 3], request.dims, request.elements, reason))
        return false;
    request.path = argv[index + 4];
    index += 4;
    return true;
}

bool seenName(const std::vector<TensorRequest>& requests, const std::string& name) {
    for (const TensorRequest& request : requests) {
        if (request.name == name)
            return true;
    }
    return false;
}

bool parseArguments(const int argc, wchar_t** argv, Options& options, std::wstring& reason) {
    bool engineSeen = false;
    bool digestSeen = false;
    bool uuidSeen = false;
    bool absoluteSeen = false;
    bool rmseSeen = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring_view flag = argv[i];
        std::wstring value;
        if (flag == L"--engine") {
            if (engineSeen) {
                reason = L"--engine is given twice";
                return false;
            }
            if (!takeValue(argc, argv, i, flag, options.engine, reason))
                return false;
            engineSeen = true;
        } else if (flag == L"--engine-sha256") {
            if (digestSeen) {
                reason = L"--engine-sha256 is given twice";
                return false;
            }
            if (!takeValue(argc, argv, i, flag, options.engineDigest, reason))
                return false;
            digestSeen = true;
        } else if (flag == L"--device-uuid") {
            if (uuidSeen) {
                reason = L"--device-uuid is given twice";
                return false;
            }
            if (!takeValue(argc, argv, i, flag, options.deviceUuid, reason))
                return false;
            uuidSeen = true;
        } else if (flag == L"--input" || flag == L"--output") {
            TensorRequest request;
            if (!parseTensorRequest(argc, argv, i, flag, request, reason))
                return false;
            std::vector<TensorRequest>& list = flag == L"--input" ? options.inputs : options.outputs;
            if (seenName(list, request.name)) {
                reason = std::wstring(flag) + L" names \"" + request.nameText + L"\" twice";
                return false;
            }
            list.push_back(std::move(request));
        } else if (flag == L"--max-absolute-error") {
            if (absoluteSeen) {
                reason = L"--max-absolute-error is given twice";
                return false;
            }
            if (!takeValue(argc, argv, i, flag, value, reason))
                return false;
            if (!parseTolerance(value, flag, options.maxAbsoluteError, reason))
                return false;
            options.absoluteErrorOverride = true;
            absoluteSeen = true;
        } else if (flag == L"--max-rmse") {
            if (rmseSeen) {
                reason = L"--max-rmse is given twice";
                return false;
            }
            if (!takeValue(argc, argv, i, flag, value, reason))
                return false;
            if (!parseTolerance(value, flag, options.maxRmse, reason))
                return false;
            options.rmseOverride = true;
            rmseSeen = true;
        } else {
            reason = L"unknown argument " + std::wstring(flag) +
                     L"; usage: --engine <raw.engine> --engine-sha256 <64 hex> --device-uuid <uuid> "
                     L"[--input <name> <fp16|fp32> <NxCxHxW> <raw>]... "
                     L"[--output <name> <fp16|fp32> <NxCxHxW> <cpu-reference.raw>]... "
                     L"[--max-absolute-error <v>] [--max-rmse <v>]";
            return false;
        }
    }

    if (!engineSeen) {
        reason = L"--engine is required";
        return false;
    }
    if (!digestSeen) {
        reason = L"--engine-sha256 is required: without the expected digest this tool would measure an "
                 L"unknown file";
        return false;
    }
    if (!uuidSeen) {
        reason = L"--device-uuid is required: this tool will not choose a device on its own";
        return false;
    }
    if (options.inputs.empty()) {
        reason = L"at least one --input is required";
        return false;
    }
    if (options.outputs.empty()) {
        reason = L"at least one --output is required";
        return false;
    }
    return true;
}

// -- CUDA devices and error text

std::wstring uuidText(const CUuuid& uuid) {
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring text = L"GPU-";
    text.reserve(40);
    for (size_t i = 0; i < sizeof(uuid.bytes); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            text.push_back(L'-');
        const unsigned char byte = static_cast<unsigned char>(uuid.bytes[i]);
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

bool parseUuid(const std::wstring_view text, CUuuid& uuid, std::wstring& reason) {
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

std::wstring cudaErrorText(const CUresult result) {
    const char* name = nullptr;
    const char* text = nullptr;
    cuGetErrorName(result, &name);
    cuGetErrorString(result, &text);
    std::wstring out = name != nullptr ? wideFromUtf8(name) : L"CUDA error " + std::to_wstring(result);
    if (text != nullptr && *text != '\0') {
        out += L" (";
        out += wideFromUtf8(text);
        out += L")";
    }
    return out;
}

bool checkCuda(const CUresult result, const std::wstring& what, std::wstring& reason) {
    if (result == CUDA_SUCCESS)
        return true;
    reason = what + L" failed: " + cudaErrorText(result);
    return false;
}

// -- RAII

class PrimaryContext {
public:
    PrimaryContext() = default;
    PrimaryContext(const PrimaryContext&) = delete;
    PrimaryContext& operator=(const PrimaryContext&) = delete;
    ~PrimaryContext() {
        if (previous_ != nullptr)
            cuCtxSetCurrent(previous_);
        if (device_ != 0)
            cuDevicePrimaryCtxRelease(device_);
    }

    bool retain(const CUdevice device, std::wstring& reason) {
        CUcontext context = nullptr;
        if (!checkCuda(cuDevicePrimaryCtxRetain(&context, device),
                       L"cuDevicePrimaryCtxRetain on the selected device", reason))
            return false;
        CUcontext previous = nullptr;
        if (!checkCuda(cuCtxGetCurrent(&previous), L"cuCtxGetCurrent", reason)) {
            cuDevicePrimaryCtxRelease(device);
            return false;
        }
        if (!checkCuda(cuCtxSetCurrent(context), L"cuCtxSetCurrent on the selected device", reason)) {
            cuDevicePrimaryCtxRelease(device);
            return false;
        }
        device_ = device;
        previous_ = previous;
        return true;
    }

private:
    CUdevice device_ = 0;
    CUcontext previous_ = nullptr;
};

class Stream {
public:
    Stream() = default;
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    ~Stream() {
        if (stream_ != nullptr)
            cuStreamDestroy(stream_);
    }

    bool create(std::wstring& reason) {
        return checkCuda(cuStreamCreate(&stream_, CU_STREAM_NON_BLOCKING),
                         L"cuStreamCreate on the selected device", reason);
    }

    CUstream get() const { return stream_; }

private:
    CUstream stream_ = nullptr;
};

class DeviceBuffer {
public:
    DeviceBuffer() = default;
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    // reserve() on a vector of these must be able to relocate an element, so
    // the resource moves with it and the emptied source stays silent.
    DeviceBuffer(DeviceBuffer&& other) noexcept : pointer_(std::exchange(other.pointer_, 0)) {}
    ~DeviceBuffer() {
        if (pointer_ != 0)
            cuMemFree(pointer_);
    }

    bool allocate(const size_t bytes, const std::wstring& what, std::wstring& reason) {
        CUdeviceptr pointer = 0;
        if (!checkCuda(cuMemAlloc(&pointer, bytes), what + L" (device allocation)", reason))
            return false;
        pointer_ = pointer;
        return true;
    }

    CUdeviceptr get() const { return pointer_; }

private:
    CUdeviceptr pointer_ = 0;
};

// Pinned, so the copies really ride the run stream instead of being staged
// behind the driver's back.
class PinnedBuffer {
public:
    PinnedBuffer() = default;
    PinnedBuffer(const PinnedBuffer&) = delete;
    PinnedBuffer& operator=(const PinnedBuffer&) = delete;
    PinnedBuffer(PinnedBuffer&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}
    ~PinnedBuffer() {
        if (pointer_ != nullptr)
            cuMemFreeHost(pointer_);
    }

    bool allocate(const size_t bytes, const std::wstring& what, std::wstring& reason) {
        void* pointer = nullptr;
        if (!checkCuda(cuMemAllocHost(&pointer, bytes), what + L" (pinned host allocation)", reason))
            return false;
        pointer_ = pointer;
        return true;
    }

    void* get() const { return pointer_; }

private:
    void* pointer_ = nullptr;
};

// The caller's raw input bytes are page-locked in place rather than copied into
// a second buffer: the file already holds exactly what the copy needs.
class RegisteredHost {
public:
    RegisteredHost() = default;
    RegisteredHost(const RegisteredHost&) = delete;
    RegisteredHost& operator=(const RegisteredHost&) = delete;
    RegisteredHost(RegisteredHost&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}
    ~RegisteredHost() {
        if (pointer_ != nullptr)
            cuMemHostUnregister(pointer_);
    }

    bool registerRange(void* pointer, const size_t bytes, const std::wstring& what,
                       std::wstring& reason) {
        const CUresult result = cuMemHostRegister(pointer, bytes, CU_MEMHOSTREGISTER_PORTABLE);
        if (result != CUDA_SUCCESS) {
            // No staging copy and no pageable fallback: a pageable upload would
            // not be the transfer this report describes.
            reason = what + L" could not be page-locked in place: " + cudaErrorText(result) +
                     L". Nothing was uploaded and nothing was staged; a driver that cannot register "
                     L"caller-owned bytes cannot produce this measurement";
            return false;
        }
        pointer_ = pointer;
        return true;
    }

private:
    void* pointer_ = nullptr;
};

template <typename T>
struct Destroyer {
    void operator()(T* object) const noexcept {
        // TensorRT 11 dropped destroy(): IRuntime, ICudaEngine and
        // IExecutionContext each declare a public virtual destructor, and the
        // pinned headers define them inline. Nothing else in this file holds a
        // reference, so each owner deletes its own object - and every one of
        // these is destroyed while the primary context is still current.
        delete object;
    }
};

using RuntimePtr = std::unique_ptr<nvinfer1::IRuntime, Destroyer<nvinfer1::IRuntime>>;
using EnginePtr = std::unique_ptr<nvinfer1::ICudaEngine, Destroyer<nvinfer1::ICudaEngine>>;
using ContextPtr = std::unique_ptr<nvinfer1::IExecutionContext, Destroyer<nvinfer1::IExecutionContext>>;

// TensorRT's own words are the only authoritative cause for a refusal.
class RecordingLogger : public nvinfer1::ILogger {
public:
    void log(const Severity severity, const nvinfer1::AsciiChar* message) noexcept override {
        if (message == nullptr)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (severity == Severity::kERROR || severity == Severity::kINTERNAL_ERROR)
            lastError_ = message;
        std::fprintf(stderr, "mikai_native_verify: TensorRT: %s\n", message);
    }

    std::wstring lastError() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return wideFromUtf8(lastError_);
    }

    void clearLastError() {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::string lastError_;
};

// -- bindings

struct Binding {
    std::string name;
    /// Set once a declared tensor file has been matched to this binding: the
    /// file and the digest of the bytes this run binds for it (an input's fed
    /// bytes, or an output's CPU reference bytes).
    std::wstring file;
    std::wstring fileSha256;
    nvinfer1::TensorIOMode mode = nvinfer1::TensorIOMode::kNONE;
    ElementType type = ElementType::Fp16;
    nvinfer1::TensorFormat format = nvinfer1::TensorFormat::kLINEAR;
    int32_t vectorizedDim = -1;
    int32_t reportedBytesPerComponent = -1;  // -1 when not vectorized; element size comes from dtype
    int32_t bytesPerElement = 0;
    int64_t declared[4] = {-1, -1, -1, -1};  // as the engine reports them
    int64_t dims[4] = {0, 0, 0, 0};          // concrete, after the context resolves
    int64_t strides[4] = {0, 0, 0, 0};
    bool concrete = false;
    uint64_t elements = 0;
    size_t bytes = 0;
    CUdeviceptr device = 0;
    unsigned char* measured = nullptr;
    const unsigned char* reference = nullptr;
};

// One record per output the run actually measured. A refusal carries the
// records it got and nothing else: an output that was never measured is
// reported as not measured rather than as zero, because a zero would read as
// a measurement.
struct OutputRecord {
    std::string name;
    ElementType type = ElementType::Fp16;
    int64_t declared[4] = {-1, -1, -1, -1};
    int64_t dims[4] = {0, 0, 0, 0};
    uint64_t elements = 0;
    bool finite = false;
    bool measured = false;
    double maxAbsoluteError = 0.0;
    double rmse = 0.0;
    double maxReferenceMagnitude = 0.0;
    double absoluteTolerance = 0.0;
    double rmseTolerance = 0.0;
    bool withinTolerance = false;
    // Where the deviation is, not just how large its maximum is. The spatial
    // axes are the last two of the concrete NCHW shape: an image edge is where
    // a wrong crop, padding or byte order shows up, while a precision
    // divergence between two engines is spread over the whole plane.
    int64_t worstElementIndex = -1;
    uint64_t elementsOutsideTolerance = 0;
    uint64_t borderElementsOutsideTolerance = 0;
};

const wchar_t* ioModeName(const nvinfer1::TensorIOMode mode) {
    switch (mode) {
        case nvinfer1::TensorIOMode::kINPUT:
            return L"input";
        case nvinfer1::TensorIOMode::kOUTPUT:
            return L"output";
        default:
            return L"none";
    }
}

bool dataTypeMatches(const nvinfer1::DataType type, const ElementType element) {
    return element == ElementType::Fp16 ? type == nvinfer1::DataType::kHALF
                                       : type == nvinfer1::DataType::kFLOAT;
}

const wchar_t* dataTypeName(const nvinfer1::DataType type) {
    switch (type) {
        case nvinfer1::DataType::kFLOAT:
            return L"fp32";
        case nvinfer1::DataType::kHALF:
            return L"fp16";
        case nvinfer1::DataType::kINT8:
            return L"int8";
        case nvinfer1::DataType::kINT32:
            return L"int32";
        case nvinfer1::DataType::kBOOL:
            return L"bool";
        case nvinfer1::DataType::kUINT8:
            return L"uint8";
        case nvinfer1::DataType::kFP8:
            return L"fp8";
        case nvinfer1::DataType::kBF16:
            return L"bf16";
        case nvinfer1::DataType::kINT64:
            return L"int64";
        case nvinfer1::DataType::kINT4:
            return L"int4";
        case nvinfer1::DataType::kFP4:
            return L"fp4";
        case nvinfer1::DataType::kE8M0:
            return L"e8m0";
        default:
            return L"unknown";
    }
}

// Only the enumerators a refusal actually needs to name; every value is
// reported with its number too, so an unknown one is still unambiguous.
const wchar_t* tensorFormatName(const nvinfer1::TensorFormat format) {
    switch (format) {
        case nvinfer1::TensorFormat::kLINEAR:
            return L"kLINEAR";
        case nvinfer1::TensorFormat::kCHW2:
            return L"kCHW2";
        case nvinfer1::TensorFormat::kHWC8:
            return L"kHWC8";
        case nvinfer1::TensorFormat::kCHW4:
            return L"kCHW4";
        case nvinfer1::TensorFormat::kCHW16:
            return L"kCHW16";
        case nvinfer1::TensorFormat::kCHW32:
            return L"kCHW32";
        case nvinfer1::TensorFormat::kDHWC8:
            return L"kDHWC8";
        case nvinfer1::TensorFormat::kCDHW32:
            return L"kCDHW32";
        case nvinfer1::TensorFormat::kHWC:
            return L"kHWC";
        case nvinfer1::TensorFormat::kHWC16:
            return L"kHWC16";
        case nvinfer1::TensorFormat::kDHWC:
            return L"kDHWC";
        default:
            return L"an unnamed format";
    }
}

bool isInput(const Binding& binding) {
    return binding.mode == nvinfer1::TensorIOMode::kINPUT;
}

// -- JSON

void appendJsonString(std::string& out, const std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(kHex[c >> 4]);
                    out.push_back(kHex[c & 0x0F]);
                } else {
                    out.push_back(raw);
                }
        }
    }
    out.push_back('"');
}

void appendJsonWide(std::string& out, const std::wstring_view text) {
    appendJsonString(out, utf8FromWide(text));
}

void appendJsonNumber(std::string& out, const double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.9g", value);
    out += text;
}

std::string jsonShape(const int64_t (&dims)[4]) {
    std::string out = "[";
    for (int i = 0; i < 4; ++i) {
        if (i != 0)
            out.push_back(',');
        out += std::to_string(dims[i]);
    }
    out.push_back(']');
    return out;
}

// The identity of the one run this process makes: the engine bytes whose digest
// it checked, the device it measured on, and the TensorRT library it
// deserialized with. Every envelope carries whatever part of that identity was
// established before the envelope was written, so a rejection is evidence about
// a named file on a named device instead of an unattributable measurement. A
// field that was never established is left out, never defaulted.
struct RunIdentity {
    bool engineKnown = false;
    std::wstring enginePath;
    size_t engineBytes = 0;
    std::wstring engineSha256;
    std::wstring expectedSha256;
    bool engineSha256Matched = false;
    bool deviceKnown = false;
    std::wstring deviceUuid;
    int32_t deviceOrdinal = 0;
    std::wstring deviceName;
    int32_t smMajor = 0;
    int32_t smMinor = 0;
    int32_t driverApiVersion = 0;
    int32_t runtimeApiVersion = 0;
    bool tensorRtKnown = false;
    int32_t tensorRtVersion = 0;
};

RunIdentity g_identity;

// The success and the failure envelope are written from this one emitter: two
// spellings of the same fields would drift apart, and a consumer reads one
// schema.
void appendIdentity(std::string& out, const RunIdentity& identity) {
    if (identity.engineKnown) {
        out += "  \"engine\": {\"path\": ";
        appendJsonWide(out, identity.enginePath);
        out += ", \"bytes\": " + std::to_string(identity.engineBytes);
        if (!identity.engineSha256.empty()) {
            out += ", \"sha256\": ";
            appendJsonWide(out, identity.engineSha256);
        }
        if (!identity.expectedSha256.empty()) {
            out += ", \"expectedSha256\": ";
            appendJsonWide(out, identity.expectedSha256);
        }
        if (!identity.engineSha256.empty() && !identity.expectedSha256.empty()) {
            out += ", \"sha256Matched\": ";
            out += identity.engineSha256Matched ? "true" : "false";
        }
        out += "},\n";
    }
    if (identity.deviceKnown) {
        out += "  \"device\": {\"uuid\": ";
        appendJsonWide(out, identity.deviceUuid);
        out += ", \"ordinal\": " + std::to_string(identity.deviceOrdinal);
        out += ", \"name\": ";
        appendJsonWide(out, identity.deviceName);
        out += ", \"smMajor\": " + std::to_string(identity.smMajor);
        out += ", \"smMinor\": " + std::to_string(identity.smMinor);
        out += ", \"driverApiVersion\": " + std::to_string(identity.driverApiVersion);
        out += ", \"runtimeApiVersion\": " + std::to_string(identity.runtimeApiVersion);
        out += "},\n";
    }
    if (identity.tensorRtKnown) {
        out += "  \"tensorrt\": {\"libraryVersion\": " + std::to_string(identity.tensorRtVersion);
        out += ", \"major\": " + std::to_string(identity.tensorRtVersion / 10000);
        out += ", \"minor\": " + std::to_string((identity.tensorRtVersion / 100) % 100);
        out += ", \"patch\": " + std::to_string(identity.tensorRtVersion % 100) + "},\n";
    }
}

// Pointer, not a reference to [4]: TensorRT's Dims carries eight extents.
std::wstring shapeText(const int64_t* dims) {
    return std::to_wstring(dims[0]) + L"x" + std::to_wstring(dims[1]) + L"x" + std::to_wstring(dims[2]) +
           L"x" + std::to_wstring(dims[3]);
}

// The same record shape is used in success and failure envelopes so a consumer
// reads one schema; in a failure the measured fields may be zero for an output
// that was never reached, and "measured": false says so explicitly.
void appendOutputArray(std::string& out, const std::vector<OutputRecord>* outputs) {
    if (outputs == nullptr || outputs->empty()) {
        out += "[]";
        return;
    }
    out += "[";
    for (size_t index = 0; index < outputs->size(); ++index) {
        const OutputRecord& record = (*outputs)[index];
        out += index == 0 ? "\n" : ",\n";
        out += "    {\"name\": ";
        appendJsonString(out, record.name);
        out += ", \"dtype\": ";
        appendJsonWide(out, elementTypeName(record.type));
        out += ", \"declaredShape\": " + jsonShape(record.declared);
        out += ", \"shape\": " + jsonShape(record.dims);
        out += ", \"elements\": " + std::to_string(record.elements);
        out += ", \"finite\": ";
        out += record.finite ? "true" : "false";
        out += ", \"measured\": ";
        out += record.measured ? "true" : "false";
        if (record.measured) {
            out += ", \"maxAbsoluteError\": ";
            appendJsonNumber(out, record.maxAbsoluteError);
            out += ", \"rmse\": ";
            appendJsonNumber(out, record.rmse);
            out += ", \"maxReferenceMagnitude\": ";
            appendJsonNumber(out, record.maxReferenceMagnitude);
            out += ", \"worstElementIndex\": " + std::to_string(record.worstElementIndex);
            if (record.worstElementIndex >= 0 && record.dims[2] > 0 && record.dims[3] > 0) {
                const int64_t plane = record.dims[2] * record.dims[3];
                const int64_t channel = record.worstElementIndex / plane;
                const int64_t inPlane = record.worstElementIndex % plane;
                out += ", \"worstElementCoordinate\": [" + std::to_string(channel) + ", " +
                       std::to_string(inPlane / record.dims[3]) + ", " +
                       std::to_string(inPlane % record.dims[3]) + "]";
            }
            out += ", \"elementsOutsideTolerance\": " +
                   std::to_string(record.elementsOutsideTolerance);
            out += ", \"borderElementsOutsideTolerance\": " +
                   std::to_string(record.borderElementsOutsideTolerance);
        }
        out += ", \"maxAbsoluteErrorTolerance\": ";
        appendJsonNumber(out, record.absoluteTolerance);
        out += ", \"rmseTolerance\": ";
        appendJsonNumber(out, record.rmseTolerance);
        out += ", \"withinTolerance\": ";
        out += record.withinTolerance ? "true" : "false";
        out += "}";
    }
    out += "\n  ]";
}

// declaredShape is reported next to the concrete shape because the two differ:
// a plan from the HD graph keeps -1 for dynamic H/W and the symbolic batch.
void appendBindingArray(std::string& out, const std::vector<Binding>* bindings) {
    if (bindings == nullptr || bindings->empty()) {
        out += "[]";
        return;
    }
    out += "[";
    for (size_t index = 0; index < bindings->size(); ++index) {
        const Binding& binding = (*bindings)[index];
        out += index == 0 ? "\n" : ",\n";
        out += "    {\"name\": ";
        appendJsonString(out, binding.name);
        out += ", \"mode\": ";
        appendJsonWide(out, ioModeName(binding.mode));
        out += ", \"dtype\": ";
        appendJsonWide(out, elementTypeName(binding.type));
        out += ", \"format\": ";
        appendJsonWide(out, tensorFormatName(binding.format));
        out += ", \"vectorizedDim\": " + std::to_string(binding.vectorizedDim);
        out += ", \"reportedBytesPerComponent\": " + std::to_string(binding.reportedBytesPerComponent);
        out += ", \"bytesPerElement\": " + std::to_string(binding.bytesPerElement);
        out += ", \"declaredShape\": " + jsonShape(binding.declared);
        if (binding.concrete) {
            out += ", \"shape\": " + jsonShape(binding.dims);
            out += ", \"strides\": " + jsonShape(binding.strides);
        }
        out += ", \"elements\": " + std::to_string(binding.elements);
        out += ", \"bytes\": " + std::to_string(binding.bytes);
        if (!binding.file.empty()) {
            out += ", \"file\": ";
            appendJsonWide(out, binding.file);
        }
        if (!binding.fileSha256.empty()) {
            out += ", \"fileSha256\": ";
            appendJsonWide(out, binding.fileSha256);
        }
        out += "}";
    }
    out += "\n  ]";
}

int report(const ExitCode code, const char* phase, const char* errorCode, const std::wstring& reason,
           const std::vector<Binding>* bindings = nullptr,
           const std::vector<OutputRecord>* outputs = nullptr) {
    std::string json;
    json += "{\n  \"tool\": \"mikai_native_verify\",\n  \"schemaVersion\": 1,\n";
    json += "  \"ok\": false,\n  \"result\": \"failed\",\n  \"phase\": ";
    appendJsonString(json, phase);
    json += ",\n  \"error\": {\"code\": ";
    appendJsonString(json, errorCode);
    json += ", \"message\": ";
    appendJsonWide(json, reason);
    json += "},\n";
    appendIdentity(json, g_identity);
    json += "  \"bindingsKnown\": ";
    json += bindings != nullptr ? "true" : "false";
    json += ",\n  \"bindings\": ";
    appendBindingArray(json, bindings);
    json += ",\n  \"outputs\": ";
    appendOutputArray(json, outputs);
    json += "\n}\n";

    std::fwrite(json.data(), 1, json.size(), stdout);
    std::fflush(stdout);
    std::fwprintf(stderr, L"%ls: %hs/%hs: %ls\n", kName, phase, errorCode, reason.c_str());
    return code;
}

// -- device selection and tensor files

struct DeviceFacts {
    std::wstring uuid;
    std::wstring name;
    int ordinal = 0;
    int smMajor = 0;
    int smMinor = 0;
    int driverApiVersion = 0;
    int runtimeApiVersion = 0;
};

// The UUID is the only accepted handle: an ordinal fallback would measure the
// plan on hardware the caller never named.
bool selectDevice(const CUuuid& requested, DeviceFacts& facts, const char*& code, std::wstring& reason) {
    code = "cuda-driver";
    int count = 0;
    if (!checkCuda(cuDeviceGetCount(&count), L"cuDeviceGetCount", reason))
        return false;
    if (count < 1) {
        reason = L"no CUDA device is present; nothing was executed";
        return false;
    }
    std::wstring present;
    for (int index = 0; index < count; ++index) {
        const CUdevice device = static_cast<CUdevice>(index);
        CUuuid uuid{};
        if (cuDeviceGetUuid(&uuid, device) != CUDA_SUCCESS)
            continue;
        if (std::memcmp(&uuid, &requested, sizeof(CUuuid)) != 0) {
            present += L"\n  " + uuidText(uuid);
            continue;
        }
        char name[256] = {};
        cuDeviceGetName(name, static_cast<int>(sizeof(name)), device);
        facts.uuid = uuidText(uuid);
        facts.name = wideFromUtf8(name);
        facts.ordinal = index;
        if (!checkCuda(cuDeviceGetAttribute(&facts.smMajor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,
                                            device),
                       L"cuDeviceGetAttribute(COMPUTE_CAPABILITY_MAJOR)", reason) ||
            !checkCuda(cuDeviceGetAttribute(&facts.smMinor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,
                                            device),
                       L"cuDeviceGetAttribute(COMPUTE_CAPABILITY_MINOR)", reason)) {
            return false;
        }
        return true;
    }
    code = "device-not-found";
    reason = L"--device-uuid " + uuidText(requested) + L" is not one of this machine's CUDA devices (" +
             std::to_wstring(count) + L" present):" + present;
    return false;
}

bool loadRequests(std::vector<TensorRequest>& requests, const wchar_t* flag, const char*& code,
                  std::wstring& reason) {
    for (TensorRequest& request : requests) {
        code = "tensor-unreadable";
        if (!readWholeFile(request.path, request.host, reason))
            return false;
        code = "tensor-size-mismatch";
        const size_t expected = static_cast<size_t>(request.elements) * elementTypeSize(request.type);
        if (request.host.size() != expected) {
            reason = std::wstring(flag) + L" \"" + request.nameText + L"\": " + request.path + L" is " +
                     std::to_wstring(request.host.size()) + L" bytes but shape " +
                     shapeText(request.dims) + L" " + elementTypeName(request.type) + L" is exactly " +
                     std::to_wstring(expected) +
                     L"; a raw tensor file carries no header, so this file is not that tensor";
            return false;
        }
        // Over the bytes that are bound below, not over a second read of the
        // path: this digest and the tensor the engine sees are the same bytes.
        code = "digest-unavailable";
        if (!sha256Hex(request.host.data(), request.host.size(), request.fileSha256, reason))
            return false;
    }
    return true;
}

TensorRequest* findRequest(std::vector<TensorRequest>& requests, const std::string& name) {
    for (TensorRequest& request : requests) {
        if (request.name == name)
            return &request;
    }
    return nullptr;
}

// -- binding validation

// What the plan says it has, before any context exists. -1 is legal here; zero
// and a rank outside 4 are not.
bool collectBindings(nvinfer1::ICudaEngine& engine, std::vector<Binding>& bindings, const char*& code,
                     std::wstring& reason) {
    code = "engine-io";
    const int32_t tensorCount = engine.getNbIOTensors();
    if (tensorCount <= 0) {
        reason = L"the deserialized engine reports no input or output tensors";
        return false;
    }
    const int32_t profiles = engine.getNbOptimizationProfiles();
    if (profiles != 1) {
        reason = L"the deserialized engine carries " + std::to_wstring(profiles) +
                 L" optimization profile(s); this verifier binds one fixed profile, and a second profile "
                 L"would let the run choose its own shape";
        return false;
    }
    bindings.reserve(static_cast<size_t>(tensorCount));
    for (int32_t index = 0; index < tensorCount; ++index) {
        const char* name = engine.getIOTensorName(index);
        if (name == nullptr || *name == '\0') {
            reason = L"engine tensor " + std::to_wstring(index) + L" has no usable name";
            return false;
        }
        code = "unsupported-binding";
        Binding binding;
        binding.name = name;
        binding.mode = engine.getTensorIOMode(name);
        if (binding.mode != nvinfer1::TensorIOMode::kINPUT &&
            binding.mode != nvinfer1::TensorIOMode::kOUTPUT) {
            reason = L"engine tensor " + quoted(binding.name) + L" is neither an input nor an output (mode " +
                     std::to_wstring(static_cast<int32_t>(binding.mode)) + L")";
            return false;
        }
        const nvinfer1::DataType engineType = engine.getTensorDataType(name);
        if (!dataTypeMatches(engineType, ElementType::Fp16) &&
            !dataTypeMatches(engineType, ElementType::Fp32)) {
            reason = L"engine tensor " + quoted(binding.name) + L" has dtype " + dataTypeName(engineType) +
                     L"; this verifier measures fp16 and fp32 tensors only";
            return false;
        }
        binding.type = dataTypeMatches(engineType, ElementType::Fp16) ? ElementType::Fp16
                                                                     : ElementType::Fp32;

        // The raw tensors are contiguous NCHW byte blocks, so anything the
        // engine stores in another order would be compared against the wrong
        // elements and could be read past its own bytes. Every layout query
        // below names profile 0 explicitly: this engine carries exactly one
        // profile, and the SDK documents that the single-argument overloads can
        // only answer for profile 0 anyway (NvInferRuntime.h:3315, :3400).
        binding.format = engine.getTensorFormat(name, 0);
        if (binding.format != nvinfer1::TensorFormat::kLINEAR) {
            reason = L"engine tensor " + quoted(binding.name) + L" is stored as " +
                     tensorFormatName(binding.format) + L" (format " +
                     std::to_wstring(static_cast<int32_t>(binding.format)) +
                     L"); this verifier compares contiguous NCHW bytes, so only kLINEAR is measurable "
                     L"here";
            return false;
        }
        binding.vectorizedDim = engine.getTensorVectorizedDim(name, 0);
        if (binding.vectorizedDim != -1) {
            reason = L"engine tensor " + quoted(binding.name) + L" is vectorized along axis " +
                     std::to_wstring(binding.vectorizedDim) +
                     L"; a vectorized tensor's bytes are not a plain element sequence";
            return false;
        }
        // getTensorBytesPerComponent returns -1 for exactly this case - a
        // tensor whose scalars per vector is 1 - and returns the vector
        // component size otherwise (NvInferRuntime.h:3306). The bytes per
        // ELEMENT are the dtype's, and that is the number the buffers use.
        binding.reportedBytesPerComponent = engine.getTensorBytesPerComponent(name, 0);
        if (binding.reportedBytesPerComponent != -1) {
            reason = L"engine tensor " + quoted(binding.name) + L" reports " +
                     std::to_wstring(binding.reportedBytesPerComponent) +
                     L" bytes per component although no dimension is vectorized, which the SDK "
                     L"contract makes impossible; this engine's layout is not one this verifier can "
                     L"measure";
            return false;
        }
        binding.bytesPerElement = static_cast<int32_t>(elementTypeSize(binding.type));
        if (engine.getTensorLocation(name) != nvinfer1::TensorLocation::kDEVICE) {
            reason = L"engine tensor " + quoted(binding.name) +
                     L" is a host tensor; this verifier binds device memory only";
            return false;
        }
        const nvinfer1::Dims dims = engine.getTensorShape(name);
        if (dims.nbDims != 4) {
            reason = L"engine tensor " + quoted(binding.name) + L" has rank " + std::to_wstring(dims.nbDims) +
                     L"; this verifier binds rank-4 NCHW tensors only";
            return false;
        }
        for (int32_t axis = 0; axis < dims.nbDims; ++axis) {
            if (dims.d[axis] == 0 || dims.d[axis] < -1) {
                reason = L"engine tensor " + quoted(binding.name) + L" declares extent " +
                         std::to_wstring(dims.d[axis]) + L" on axis " + std::to_wstring(axis) +
                         L"; only -1 (dynamic) and positive extents are meaningful";
                return false;
            }
            binding.declared[axis] = dims.d[axis];
        }
        bindings.push_back(std::move(binding));
    }
    code = "engine-io";
    return true;
}

// min == opt == max is the fixed prepared engine this project expects; a wider
// profile would let the run pick its own shape.
bool matchDeclared(nvinfer1::ICudaEngine& engine, std::vector<Binding>& bindings, Options& options,
                   const char*& code, std::wstring& reason) {
    code = "binding-mismatch";
    size_t realInputs = 0;
    size_t realOutputs = 0;
    for (const Binding& binding : bindings) {
        (isInput(binding) ? realInputs : realOutputs)++;
    }
    if (realInputs != options.inputs.size() || realOutputs != options.outputs.size()) {
        reason = L"the engine exposes " + std::to_wstring(realInputs) + L" input(s) and " +
                 std::to_wstring(realOutputs) + L" output(s), but " +
                 std::to_wstring(options.inputs.size()) + L" --input and " +
                 std::to_wstring(options.outputs.size()) +
                 L" --output were given; every engine tensor must be declared and every declaration must "
                 L"be an engine tensor";
        return false;
    }

    for (Binding& binding : bindings) {
        const bool input = isInput(binding);
        std::vector<TensorRequest>& list = input ? options.inputs : options.outputs;
        TensorRequest* request = findRequest(list, binding.name);
        if (request == nullptr) {
            reason = L"engine " + std::wstring(ioModeName(binding.mode)) + L" " + quoted(binding.name) +
                     L" was not declared by " + (input ? L"--input" : L"--output");
            return false;
        }
        if (binding.type != request->type) {
            reason = L"engine " + std::wstring(ioModeName(binding.mode)) + L" " + quoted(binding.name) +
                     L" is " + elementTypeName(binding.type) + L" but " +
                     (input ? L"--input" : L"--output") + L" declared " + elementTypeName(request->type);
            return false;
        }
        const size_t expected = static_cast<size_t>(request->elements) * elementTypeSize(request->type);
        if (request->host.size() != expected) {
            reason = L"engine " + std::wstring(ioModeName(binding.mode)) + L" " + quoted(binding.name) +
                     L" holds " + std::to_wstring(expected) + L" bytes but the file named for it holds " +
                     std::to_wstring(request->host.size());
            return false;
        }
        // Inputs are already fixed by the profile check below; outputs are
        // resized from the shape TensorRT resolves.
        binding.reference = request->host.data();
        binding.elements = request->elements;
        binding.bytes = expected;
        binding.file = request->path;
        binding.fileSha256 = request->fileSha256;

        if (!input)
            continue;

        code = "profile-mismatch";
        static constexpr nvinfer1::OptProfileSelector kSelectors[] = {
            nvinfer1::OptProfileSelector::kMIN, nvinfer1::OptProfileSelector::kOPT,
            nvinfer1::OptProfileSelector::kMAX};
        static constexpr const wchar_t* kSelectorNames[] = {L"min", L"opt", L"max"};
        for (int selector = 0; selector < 3; ++selector) {
            const nvinfer1::Dims profile =
                engine.getProfileShape(binding.name.c_str(), 0, kSelectors[selector]);
            if (profile.nbDims != 4) {
                reason = L"optimization profile 0 " + std::wstring(kSelectorNames[selector]) + L" for " +
                         quoted(binding.name) + L" is rank " + std::to_wstring(profile.nbDims) +
                         L", not the rank-4 shape " + (input ? L"--input" : L"--output") + L" declared";
                return false;
            }
            for (int32_t axis = 0; axis < 4; ++axis) {
                if (profile.d[axis] != request->dims[axis]) {
                    reason = L"optimization profile 0 " + std::wstring(kSelectorNames[selector]) +
                             L" for input " + quoted(binding.name) + L" is " + shapeText(profile.d) +
                             L" but --input declared " + shapeText(request->dims) +
                             L"; this verifier accepts only a fixed profile";
                    return false;
                }
            }
        }
    }

    for (const TensorRequest& request : options.inputs) {
        bool found = false;
        for (const Binding& binding : bindings) {
            if (isInput(binding) && binding.name == request.name)
                found = true;
        }
        if (!found) {
            reason = L"--input \"" + request.nameText + L"\" is not an input of the deserialized engine";
            return false;
        }
    }
    for (const TensorRequest& request : options.outputs) {
        bool found = false;
        for (const Binding& binding : bindings) {
            if (!isInput(binding) && binding.name == request.name)
                found = true;
        }
        if (!found) {
            reason = L"--output \"" + request.nameText + L"\" is not an output of the deserialized engine";
            return false;
        }
    }
    code = "binding-mismatch";
    return true;
}

// Before setTensorAddress, as TensorRT documents.
bool specifyInputs(nvinfer1::IExecutionContext& execution, std::vector<Binding>& bindings, Options& options,
                   std::wstring& reason) {
    for (const Binding& binding : bindings) {
        if (!isInput(binding))
            continue;
        const TensorRequest& request = *findRequest(options.inputs, binding.name);
        nvinfer1::Dims dims;
        dims.nbDims = 4;
        for (int32_t axis = 0; axis < 4; ++axis)
            dims.d[axis] = request.dims[axis];
        if (!execution.setInputShape(binding.name.c_str(), dims)) {
            reason = L"setInputShape(" + quoted(binding.name) + L", " + shapeText(request.dims) +
                     L") was refused; the input is not consistent with the engine's own profile";
            return false;
        }
    }
    return true;
}

// Runs after the input addresses are set, because TensorRT wants an address for
// any shape-inference IO tensor. Output buffers are sized from the result, so
// nothing is bound or enqueued until every shape here has been validated.
bool resolveShapes(nvinfer1::IExecutionContext& execution, std::vector<Binding>& bindings, Options& options,
                   const char*& code, std::wstring& reason) {
    code = "shape-inference-failed";
    char const* insufficient[16];
    const int32_t pending = execution.inferShapes(16, insufficient);
    if (pending != 0) {
        reason = pending < 0
                     ? L"TensorRT could not infer shapes for this plan"
                     : L"TensorRT still considers " + std::to_wstring(pending) +
                           L" input(s) insufficiently specified:";
        for (int32_t i = 0; i < (pending < 0 ? 0 : (pending < 16 ? pending : 16)); ++i)
            reason += L" \"" + wideFromUtf8(insufficient[i]) + L"\"";
        return false;
    }

    for (Binding& binding : bindings) {
        code = "shape-unresolved";
        const TensorRequest& request =
            *(isInput(binding) ? findRequest(options.inputs, binding.name)
                              : findRequest(options.outputs, binding.name));
        const nvinfer1::Dims resolved = execution.getTensorShape(binding.name.c_str());
        if (resolved.nbDims != 4) {
            reason = L"tensor " + quoted(binding.name) + L" resolved to rank " +
                     std::to_wstring(resolved.nbDims) +
                     L" after the input shapes were set; this verifier binds rank-4 NCHW tensors only";
            return false;
        }
        for (int32_t axis = 0; axis < 4; ++axis) {
            if (resolved.d[axis] < 1) {
                reason = L"tensor " + quoted(binding.name) + L" resolved to extent " +
                         std::to_wstring(resolved.d[axis]) + L" on axis " + std::to_wstring(axis) +
                         L"; the plan did not resolve one concrete shape";
                return false;
            }
        }
        for (int32_t axis = 0; axis < 4; ++axis) {
            if (resolved.d[axis] != request.dims[axis]) {
                reason = L"tensor " + quoted(binding.name) + L" resolved to " + shapeText(resolved.d) +
                         L" but " + (isInput(binding) ? L"--input" : L"--output") + L" " +
                         request.nameText + L" declared " + shapeText(request.dims);
                return false;
            }
        }
        for (int32_t axis = 0; axis < 4; ++axis)
            binding.dims[axis] = resolved.d[axis];
        binding.concrete = true;
        binding.elements = static_cast<uint64_t>(resolved.d[0]) * static_cast<uint64_t>(resolved.d[1]) *
                           static_cast<uint64_t>(resolved.d[2]) * static_cast<uint64_t>(resolved.d[3]);
        binding.bytes = static_cast<size_t>(binding.elements) * elementTypeSize(binding.type);

        // The raw file is one contiguous NCHW block, so the engine's own
        // strides must be the row-major ones or the byte order does not match.
        code = "stride-mismatch";
        const nvinfer1::Dims strides = execution.getTensorStrides(binding.name.c_str());
        const int64_t expected[4] = {resolved.d[1] * resolved.d[2] * resolved.d[3],
                                     resolved.d[2] * resolved.d[3], resolved.d[3], 1};
        if (strides.nbDims != 4) {
            reason = L"tensor " + quoted(binding.name) + L" reported " + std::to_wstring(strides.nbDims) +
                     L" stride(s) instead of the four a rank-4 tensor has";
            return false;
        }
        for (int32_t axis = 0; axis < 4; ++axis) {
            if (strides.d[axis] != expected[axis]) {
                reason = L"tensor " + quoted(binding.name) + L" resolves to " + shapeText(resolved.d) +
                         L" with stride " + std::to_wstring(strides.d[axis]) + L" on axis " +
                         std::to_wstring(axis) + L", but contiguous NCHW needs " +
                         std::to_wstring(expected[axis]) +
                         L"; the engine's memory order is not the byte order of the raw tensor";
                return false;
            }
        }
        for (int32_t axis = 0; axis < 4; ++axis)
            binding.strides[axis] = strides.d[axis];
    }
    code = "shape-unresolved";
    return true;
}

bool measure(const Binding& binding, OutputRecord& record, std::wstring& reason) {
    const size_t count = static_cast<size_t>(binding.elements);
    // The concrete NCHW shape is already resolved here; the last two axes are
    // the plane whose edge an error is localized against.
    const int64_t height = binding.dims[2];
    const int64_t width = binding.dims[3];
    const int64_t plane = height * width;
    double squared = 0.0;
    for (size_t index = 0; index < count; ++index) {
        const float produced = elementAsFloat(binding.measured, index, binding.type);
        const float reference = elementAsFloat(binding.reference, index, binding.type);
        if (!std::isfinite(produced)) {
            reason = L"output " + quoted(binding.name) + L" element " + std::to_wstring(index) +
                     L" is not finite on the selected device";
            return false;
        }
        if (!std::isfinite(reference)) {
            reason = L"the CPU reference for output " + quoted(binding.name) + L" element " +
                     std::to_wstring(index) + L" is not finite";
            return false;
        }
        const double magnitude = std::fabs(static_cast<double>(reference));
        if (magnitude > record.maxReferenceMagnitude)
            record.maxReferenceMagnitude = magnitude;
        const double difference =
            std::fabs(static_cast<double>(produced) - static_cast<double>(reference));
        if (difference > record.maxAbsoluteError) {
            record.maxAbsoluteError = difference;
            record.worstElementIndex = static_cast<int64_t>(index);
        }
        // Counted against the same absolute bound the verdict uses, so an RMSE
        // refusal with no element here is visibly a broad, not a spiky, drift.
        if (plane > 0 && width > 0 && difference > record.absoluteTolerance) {
            ++record.elementsOutsideTolerance;
            const int64_t inPlane = static_cast<int64_t>(index) % plane;
            const int64_t y = inPlane / width;
            const int64_t x = inPlane % width;
            if (y == 0 || x == 0 || y == height - 1 || x == width - 1)
                ++record.borderElementsOutsideTolerance;
        }
        squared += difference * difference;
    }
    record.rmse = std::sqrt(squared / static_cast<double>(count));
    record.finite = true;
    record.measured = true;
    return true;
}

int verify(int argc, wchar_t** argv) {
    Options options;
    std::wstring reason;
    if (!parseArguments(argc, argv, options, reason))
        return report(kUsage, "arguments", "usage", reason);
    CUuuid requestedUuid{};
    if (!parseUuid(options.deviceUuid, requestedUuid, reason))
        return report(kUsage, "arguments", "usage", reason);
    std::wstring expectedDigest;
    if (!parseExpectedDigest(options.engineDigest, expectedDigest, reason))
        return report(kUsage, "arguments", "usage", reason);

    // Refused before CUDA or TensorRT is touched at all.
    std::vector<unsigned char> engineBytes;
    if (!readWholeFile(options.engine, engineBytes, reason))
        return report(kPreflight, "integrity", "file-unreadable", reason);
    // The path and byte count are facts from the moment the file was read, and
    // the digests from the moment they were computed: a digest refusal names
    // the bytes it refused, not just the expectation they missed.
    g_identity.engineKnown = true;
    g_identity.enginePath = options.engine;
    g_identity.engineBytes = engineBytes.size();
    g_identity.expectedSha256 = expectedDigest;
    std::wstring actualDigest;
    if (!sha256Hex(engineBytes.data(), engineBytes.size(), actualDigest, reason))
        return report(kInternal, "integrity", "digest-unavailable", reason);
    g_identity.engineSha256 = actualDigest;
    g_identity.engineSha256Matched = equalsNoCase(actualDigest, expectedDigest);
    if (!equalsNoCase(actualDigest, expectedDigest))
        return report(kDigest, "integrity", "digest-mismatch",
                      L"--engine " + options.engine + L" hashes to " + actualDigest + L" but " +
                          expectedDigest +
                          L" was expected; these are not the expected bytes, so nothing was deserialized");

    const char* code = "tensor-unreadable";
    if (!loadRequests(options.inputs, L"--input", code, reason))
        return report(kPreflight, "input", code, reason);
    if (!loadRequests(options.outputs, L"--output", code, reason))
        return report(kPreflight, "input", code, reason);

    if (!checkCuda(cuInit(0), L"cuInit", reason))
        return report(kDevice, "device", "cuda-driver", reason);
    DeviceFacts device;
    if (!selectDevice(requestedUuid, device, code, reason))
        return report(kDevice, "device", code, reason);
    if (!checkCuda(cuDriverGetVersion(&device.driverApiVersion), L"cuDriverGetVersion", reason))
        return report(kDevice, "device", "cuda-driver", reason);
    const cudaError_t runtimeVersionResult = cudaRuntimeGetVersion(&device.runtimeApiVersion);
    if (runtimeVersionResult != cudaSuccess)
        return report(kDevice, "device", "cuda-driver",
                      L"cudaRuntimeGetVersion failed: " +
                          wideFromUtf8(cudaGetErrorString(runtimeVersionResult)));
    // The device and both API versions are established here; every later
    // envelope names the hardware the measurement belongs to.
    g_identity.deviceKnown = true;
    g_identity.deviceUuid = device.uuid;
    g_identity.deviceOrdinal = device.ordinal;
    g_identity.deviceName = device.name;
    g_identity.smMajor = device.smMajor;
    g_identity.smMinor = device.smMinor;
    g_identity.driverApiVersion = device.driverApiVersion;
    g_identity.runtimeApiVersion = device.runtimeApiVersion;

    PrimaryContext context;
    if (!context.retain(static_cast<CUdevice>(device.ordinal), reason))
        return report(kDevice, "device", "device-context", reason);

    RecordingLogger logger;
    RuntimePtr runtime(nvinfer1::createInferRuntime(logger));
    if (!runtime)
        return report(kDeserialize, "deserialize", "runtime-unavailable",
                      L"createInferRuntime returned nothing" +
                          (logger.lastError().empty() ? std::wstring() : L": " + logger.lastError()));
    const int32_t libraryVersion = getInferLibVersion();
    g_identity.tensorRtKnown = true;
    g_identity.tensorRtVersion = libraryVersion;

    logger.clearLastError();
    EnginePtr engine(runtime->deserializeCudaEngine(engineBytes.data(), engineBytes.size()));
    if (!engine)
        return report(kDeserialize, "deserialize", "deserialize-refused",
                      L"--engine " + options.engine + L" was refused by TensorRT runtime " +
                          std::to_wstring(libraryVersion) + L" on " + device.uuid + L": " +
                          (logger.lastError().empty() ? std::wstring(L"deserializeCudaEngine returned nullptr")
                                                      : logger.lastError()));

    std::vector<Binding> bindings;
    if (!collectBindings(*engine, bindings, code, reason))
        return report(kBinding, "bindings", code, reason, &bindings);
    if (!matchDeclared(*engine, bindings, options, code, reason))
        return report(kBinding, "bindings", code, reason, &bindings);

    logger.clearLastError();
    ContextPtr execution(engine->createExecutionContext());
    if (!execution)
        return report(kExecution, "execution", "context-failed",
                      L"createExecutionContext returned nothing" +
                          (logger.lastError().empty() ? std::wstring() : L": " + logger.lastError()));

    if (!specifyInputs(*execution, bindings, options, reason))
        return report(kBinding, "bindings", "shape-unresolved", reason, &bindings);

    Stream stream;
    if (!stream.create(reason))
        return report(kExecution, "execution", "stream-failed", reason, &bindings);

    std::vector<PinnedBuffer> hosts;        // outputs only: measured bytes cannot alias the reference
    std::vector<DeviceBuffer> devices;
    std::vector<RegisteredHost> registrations;  // inputs only, page-locked in place
    hosts.reserve(bindings.size());
    devices.reserve(bindings.size());
    registrations.reserve(bindings.size());
    for (size_t index = 0; index < bindings.size(); ++index) {
        hosts.emplace_back();
        devices.emplace_back();
        registrations.emplace_back();
    }

    // Input sizes are already fixed: the caller supplied those bytes and the
    // profile check confirmed them.
    for (size_t index = 0; index < bindings.size(); ++index) {
        Binding& binding = bindings[index];
        if (!isInput(binding))
            continue;
        if (!devices[index].allocate(binding.bytes, L"tensor " + quoted(binding.name), reason))
            return report(kExecution, "execution", "allocation-failed", reason, &bindings);
        if (!registrations[index].registerRange(
                const_cast<unsigned char*>(binding.reference), binding.bytes,
                L"input " + quoted(binding.name) + L" (" + std::to_wstring(binding.bytes) + L" bytes)",
                reason))
            return report(kExecution, "execution", "host-registration-failed", reason, &bindings);
        binding.device = devices[index].get();
        if (!execution->setTensorAddress(binding.name.c_str(), reinterpret_cast<void*>(binding.device)))
            return report(kExecution, "execution", "bind-failed",
                          L"setTensorAddress(" + quoted(binding.name) + L") was refused" +
                              (logger.lastError().empty() ? std::wstring() : L": " + logger.lastError()),
                          &bindings);
    }

    if (!resolveShapes(*execution, bindings, options, code, reason))
        return report(kBinding, "bindings", code, reason, &bindings);

    for (size_t index = 0; index < bindings.size(); ++index) {
        Binding& binding = bindings[index];
        if (isInput(binding))
            continue;
        if (!hosts[index].allocate(binding.bytes, L"tensor " + quoted(binding.name) + L" host buffer",
                                   reason) ||
            !devices[index].allocate(binding.bytes, L"tensor " + quoted(binding.name), reason))
            return report(kExecution, "execution", "allocation-failed", reason, &bindings);
        binding.measured = static_cast<unsigned char*>(hosts[index].get());
        binding.device = devices[index].get();
        if (!execution->setTensorAddress(binding.name.c_str(), reinterpret_cast<void*>(binding.device)))
            return report(kExecution, "execution", "bind-failed",
                          L"setTensorAddress(" + quoted(binding.name) + L") was refused" +
                              (logger.lastError().empty() ? std::wstring() : L": " + logger.lastError()),
                          &bindings);
    }

    for (const Binding& binding : bindings) {
        if (!isInput(binding))
            continue;
        // binding.reference is the caller's own raw bytes, page-locked in
        // place above; binding.measured is null for inputs.
        if (!checkCuda(cuMemcpyHtoDAsync(binding.device, const_cast<unsigned char*>(binding.reference),
                                         binding.bytes, stream.get()),
                       L"host-to-device copy for input " + quoted(binding.name), reason))
            return report(kExecution, "execution", "copy-failed", reason, &bindings);
    }

    logger.clearLastError();
    if (!execution->enqueueV3(stream.get()))
        return report(kExecution, "execution", "enqueue-failed",
                      L"enqueueV3 was refused" +
                          (logger.lastError().empty() ? std::wstring() : L": " + logger.lastError()),
                      &bindings);
    if (!checkCuda(cuStreamSynchronize(stream.get()), L"cuStreamSynchronize after enqueueV3", reason))
        return report(kExecution, "execution", "sync-failed", reason, &bindings);

    // The only device-to-host copies in the tool; they carry a measurement,
    // never a frame.
    for (const Binding& binding : bindings) {
        if (isInput(binding))
            continue;
        if (!checkCuda(cuMemcpyDtoHAsync(binding.measured, binding.device, binding.bytes, stream.get()),
                       L"device-to-host copy for output " + quoted(binding.name), reason))
            return report(kExecution, "execution", "copy-failed", reason, &bindings);
    }
    if (!checkCuda(cuStreamSynchronize(stream.get()), L"cuStreamSynchronize after the output copies", reason))
        return report(kExecution, "execution", "sync-failed", reason, &bindings);

    std::vector<OutputRecord> records;
    for (const Binding& binding : bindings) {
        if (isInput(binding))
            continue;
        OutputRecord record;
        record.name = binding.name;
        record.type = binding.type;
        for (int axis = 0; axis < 4; ++axis)
            record.dims[axis] = binding.dims[axis];
        for (int axis = 0; axis < 4; ++axis)
            record.declared[axis] = binding.declared[axis];
        record.elements = binding.elements;
        const bool half = binding.type == ElementType::Fp16;
        record.absoluteTolerance = options.absoluteErrorOverride
                                       ? options.maxAbsoluteError
                                       : (half ? kFp16MaxAbsoluteError : kFp32MaxAbsoluteError);
        record.rmseTolerance =
            options.rmseOverride ? options.maxRmse : (half ? kFp16MaxRmse : kFp32MaxRmse);
        if (!measure(binding, record, reason)) {
            records.push_back(record);
            return report(kNumerics, "numerics", "non-finite", reason, &bindings, &records);
        }
        record.withinTolerance =
            record.maxAbsoluteError <= record.absoluteTolerance && record.rmse <= record.rmseTolerance;
        if (!record.withinTolerance) {
            std::fprintf(stderr,
                         "mikai_native_verify: output \"%s\" measured maxAbsoluteError %.9g "
                         "(tolerance %.9g) and RMSE %.9g (tolerance %.9g) over %llu elements, "
                         "reference magnitude %.9g, worst element %lld, %llu element(s) outside the "
                         "absolute bound of which %llu are on a spatial edge\n",
                         binding.name.c_str(), record.maxAbsoluteError, record.absoluteTolerance,
                         record.rmse, record.rmseTolerance,
                         static_cast<unsigned long long>(binding.elements),
                         record.maxReferenceMagnitude,
                         static_cast<long long>(record.worstElementIndex),
                         static_cast<unsigned long long>(record.elementsOutsideTolerance),
                         static_cast<unsigned long long>(record.borderElementsOutsideTolerance));
            records.push_back(record);
            return report(kNumerics, "numerics", "out-of-tolerance",
                          L"output " + quoted(binding.name) +
                              L" is outside the requested tolerance; the measured values are in the "
                              L"outputs array of this JSON report",
                          &bindings, &records);
        }
        records.push_back(record);
    }

    std::string json;
    json += "{\n  \"tool\": \"mikai_native_verify\",\n  \"schemaVersion\": 1,\n";
    json += "  \"ok\": true,\n  \"result\": \"verified\",\n";
    json += "  \"phase\": \"complete\",\n";
    json += "  \"scope\": \"engine bytes, real bindings and numerics on one named CUDA device\",\n";
    json += "  \"manifestVerified\": false,\n  \"prepared\": false,\n";
    json += "  \"claim\": \"A measured run of one raw serialized engine. This is not manifest validation, "
            "not publication, and not Prepared or Active state.\",\n";

    appendIdentity(json, g_identity);

    json += "  \"tolerances\": {\"policy\": \"conservative verification bounds, not a neural output "
            "quality claim\", \"maxAbsoluteErrorOverride\": ";
    json += options.absoluteErrorOverride ? "true" : "false";
    json += ", \"maxRmseOverride\": ";
    json += options.rmseOverride ? "true" : "false";
    json += "},\n";

    json += "  \"bindings\": ";
    appendBindingArray(json, &bindings);

    json += ",\n  \"outputs\": ";
    appendOutputArray(json, &records);
    json += "\n}\n";

    std::fwrite(json.data(), 1, json.size(), stdout);
    std::fflush(stdout);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        return verify(argc, argv);
    } catch (const std::exception& error) {
        // A raw tensor file is whatever size the caller points at.
        return report(kInternal, "internal", "allocation-failed",
                      std::wstring(L"the verifier could not allocate what the inputs describe: ") +
                          wideFromUtf8(error.what()));
    }
}
