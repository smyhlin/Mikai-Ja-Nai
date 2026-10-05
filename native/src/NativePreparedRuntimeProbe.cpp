/*
 * NativePreparedRuntimeProbe - hand-run console probe for the managed
 * prepared-plan v1 runtime.
 *
 * It loads the produced dispatcher (aji.dll) dynamically, resolves the
 * original ABI-8 lifecycle exports plus the additive prepared-plan exports,
 * queries the actual CUDA device/context through nvcuda.dll (no CUDA or
 * TensorRT link dependency), and drives the real managed runtime:
 *
 *   - builds an aji_native_prepared_plan_v1 from explicit CLI facts or from
 *     an existing mikai.native.admission/1 document, with the source/device
 *     and build facts the parent supplies and the device facts the probe
 *     independently measures;
 *   - aji_native_create_prepared / aji_native_apply_prepared, observing that
 *     Prepared is only the actually loaded plan and Active only a completed
 *     output under the current epoch;
 *   - aji_configure + aji_infer + aji_flush + aji_done/aji_wait on the
 *     probe's own CUDA stream;
 *   - suspend (resources released), resume (revalidated, no build), ordered
 *     drain on the real stream, epoch advance without reload, and a
 *     transactional refused replacement that retains the prior plan;
 *   - digest-before-deserialize refusals and invalid dtype/shape/device/
 *     runtime/model/engine/build descriptors.
 *
 * The completed spatial output is measured against an existing
 * mikai.frameReference manifest: the manifest's own graph/tensor digests and
 * shapes are re-hashed and enforced, the static input tensor is fed through
 * the documented 4:4:4 matrix path (honestly labelled a static numerical
 * input, NOT a decoded NVDEC frame), and the completed output is projected
 * back through the same documented matrix and compared to the manifest's
 * CPU output with the unchanged fp16 bounds. The bounds are never loosened.
 *
 * No Qt, CUDA, TensorRT or JSON library is linked: nvcuda.dll and aji.dll are
 * resolved at run time, the small JSON reader is local, and only bcrypt is
 * linked for SHA-256. Every refusal is named; missing required input is
 * refused by name rather than guessed.
 */

// <windows.h> defines min/max as macros unless NOMINMAX is set; this probe uses
// std::min/std::max, so the macros must not be defined.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "aji.h"
#include "aji_native.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr const char *kProbeName = "NativePreparedRuntimeProbe";

enum ExitCode {
    kOk = 0,
    kUsage = 2,
    kPreflight = 3,
    kDevice = 4,
    kSymbols = 5,
    kAdmission = 6,
    kLifecycle = 7,
    kNumerics = 8,
    kInternal = 9,
};

// Same fp16 bounds the existing engine verifier uses; never weakened here.
constexpr double kFp16MaxAbsoluteError = 0.02;
constexpr double kFp16MaxRmse = 0.005;
constexpr uint32_t kControlTimeoutMs = 5000u;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

std::wstring widen(const std::string &utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        out.data(), length);
    return out;
}

std::string narrowUtf8(const std::wstring &wide) {
    if (wide.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                           static_cast<int>(wide.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        out.data(), length, nullptr, nullptr);
    return out;
}

std::string toUtf8(const std::filesystem::path &path) {
    const std::string u8 = path.u8string();
    return u8;
}

std::string lowerAscii(std::string text) {
    for (char &ch : text)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return text;
}

bool equalsNoCase(const std::string &a, const std::string &b) {
    return lowerAscii(a) == lowerAscii(b);
}

bool containsNoCase(const std::string &haystack, const std::string &needle) {
    if (needle.empty()) return true;
    return lowerAscii(haystack).find(lowerAscii(needle)) != std::string::npos;
}

bool isLowerHex(const std::string &text, size_t length) {
    if (text.size() != length) return false;
    for (char ch : text)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    return true;
}

std::string jsonQuote(const std::string &text) {
    std::string out = "\"";
    for (unsigned char ch : text) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                out += buffer;
            } else {
                out += static_cast<char>(ch);
            }
        }
    }
    out += "\"";
    return out;
}

std::string jsonNumber(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", value);
    return buffer;
}

// ---------------------------------------------------------------------------
// SHA-256 (bcrypt; the only linked crypto dependency)
// ---------------------------------------------------------------------------

bool sha256Bytes(const void *data, size_t length, std::string *hex,
                 std::string *why) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;
    bool ok = false;
    auto finish = [&]() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        if (why) *why = "BCryptOpenAlgorithmProvider(SHA256) failed";
        return false;
    }
    DWORD objectLength = 0, digestLength = 0, produced = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                          &produced, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&digestLength), sizeof(digestLength),
                          &produced, 0) != 0) {
        if (why) *why = "BCryptGetProperty failed";
        finish();
        return false;
    }
    object.resize(objectLength);
    digest.resize(digestLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) != 0) {
        if (why) *why = "BCryptCreateHash failed";
        finish();
        return false;
    }
    const unsigned char *cursor = static_cast<const unsigned char *>(data);
    size_t remaining = length;
    while (remaining > 0) {
        const ULONG chunk = static_cast<ULONG>(std::min<size_t>(remaining, 1u << 30));
        if (BCryptHashData(hash, const_cast<PUCHAR>(cursor), chunk, 0) != 0) {
            if (why) *why = "BCryptHashData failed";
            finish();
            return false;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    if (BCryptFinishHash(hash, digest.data(), digestLength, 0) != 0) {
        if (why) *why = "BCryptFinishHash failed";
        finish();
        return false;
    }
    static const char digits[] = "0123456789abcdef";
    hex->clear();
    hex->reserve(digest.size() * 2);
    for (unsigned char byte : digest) {
        hex->push_back(digits[byte >> 4]);
        hex->push_back(digits[byte & 0x0f]);
    }
    ok = true;
    finish();
    return ok;
}

bool readFileBytes(const std::filesystem::path &path, std::vector<unsigned char> *out,
                   std::string *why) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (why) *why = "cannot open " + toUtf8(path);
        return false;
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        if (why) *why = "cannot size " + toUtf8(path);
        return false;
    }
    stream.seekg(0, std::ios::beg);
    out->assign(static_cast<size_t>(size), 0);
    if (size > 0) stream.read(reinterpret_cast<char *>(out->data()), size);
    if (!stream) {
        if (why) *why = "cannot read " + toUtf8(path);
        return false;
    }
    return true;
}

bool sha256File(const std::filesystem::path &path, std::string *hex, std::string *why) {
    std::vector<unsigned char> bytes;
    if (!readFileBytes(path, &bytes, why)) return false;
    return sha256Bytes(bytes.data(), bytes.size(), hex, why);
}

// ---------------------------------------------------------------------------
// minimal JSON reader (no dependency; used for the reference manifest and the
// optional admission document)
// ---------------------------------------------------------------------------

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    std::string number;
    std::string text;
    std::vector<Json> items;
    // Parallel vectors (not pair<string,Json>) so the element type stays a
    // plain std::vector<Json>, whose incomplete-type support C++17 guarantees.
    std::vector<std::string> memberKeys;
    std::vector<Json> memberValues;

    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    bool isString() const { return type == Type::String; }
    bool isNumber() const { return type == Type::Number; }

    const Json *member(const char *key) const {
        if (type != Type::Object) return nullptr;
        for (size_t i = 0; i < memberKeys.size(); ++i)
            if (memberKeys[i] == key) return &memberValues[i];
        return nullptr;
    }
    const Json *item(size_t index) const {
        if (type != Type::Array || index >= items.size()) return nullptr;
        return &items[index];
    }
    size_t count() const { return type == Type::Array ? items.size() : 0; }
    std::string str() const { return type == Type::String ? text : std::string(); }
    bool booleanValue(bool fallback) const { return type == Type::Bool ? boolean : fallback; }
    int integer(int fallback) const {
        return type == Type::Number ? static_cast<int>(std::strtol(number.c_str(), nullptr, 10))
                                    : fallback;
    }
    unsigned long long u64(unsigned long long fallback) const {
        return type == Type::Number
                   ? std::strtoull(number.c_str(), nullptr, 10)
                   : fallback;
    }
};

struct JsonParser {
    const std::string &source;
    size_t index = 0;
    std::string *why;

    bool fail(const std::string &message) {
        if (why) *why = message + " at byte " + std::to_string(index);
        return false;
    }
    void skipWhitespace() {
        while (index < source.size() &&
               (source[index] == ' ' || source[index] == '\t' || source[index] == '\n' ||
                source[index] == '\r'))
            ++index;
    }
    bool parse(Json *out) {
        skipWhitespace();
        if (!value(out)) return false;
        skipWhitespace();
        if (index != source.size()) return fail("trailing bytes after JSON value");
        return true;
    }
    bool value(Json *out) {
        skipWhitespace();
        if (index >= source.size()) return fail("unexpected end of JSON");
        const char ch = source[index];
        if (ch == '{') return object(out);
        if (ch == '[') return array(out);
        if (ch == '"') {
            out->type = Json::Type::String;
            return string(&out->text);
        }
        if (ch == 't') {
            if (source.compare(index, 4, "true") != 0) return fail("bad literal");
            index += 4;
            out->type = Json::Type::Bool;
            out->boolean = true;
            return true;
        }
        if (ch == 'f') {
            if (source.compare(index, 5, "false") != 0) return fail("bad literal");
            index += 5;
            out->type = Json::Type::Bool;
            out->boolean = false;
            return true;
        }
        if (ch == 'n') {
            if (source.compare(index, 4, "null") != 0) return fail("bad literal");
            index += 4;
            out->type = Json::Type::Null;
            return true;
        }
        if (ch == '-' || (ch >= '0' && ch <= '9')) return number(out);
        return fail("unexpected character");
    }
    bool object(Json *out) {
        out->type = Json::Type::Object;
        ++index;
        skipWhitespace();
        if (index < source.size() && source[index] == '}') {
            ++index;
            return true;
        }
        for (;;) {
            skipWhitespace();
            if (index >= source.size() || source[index] != '"')
                return fail("expected an object member name");
            std::string key;
            if (!string(&key)) return false;
            skipWhitespace();
            if (index >= source.size() || source[index] != ':')
                return fail("expected ':'");
            ++index;
            Json member;
            if (!value(&member)) return false;
            out->memberKeys.push_back(std::move(key));
            out->memberValues.push_back(std::move(member));
            skipWhitespace();
            if (index >= source.size()) return fail("unterminated object");
            if (source[index] == ',') {
                ++index;
                continue;
            }
            if (source[index] == '}') {
                ++index;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }
    bool array(Json *out) {
        out->type = Json::Type::Array;
        ++index;
        skipWhitespace();
        if (index < source.size() && source[index] == ']') {
            ++index;
            return true;
        }
        for (;;) {
            Json item;
            if (!value(&item)) return false;
            out->items.push_back(std::move(item));
            skipWhitespace();
            if (index >= source.size()) return fail("unterminated array");
            if (source[index] == ',') {
                ++index;
                continue;
            }
            if (source[index] == ']') {
                ++index;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }
    static int hexValue(char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    }
    static void encodeUtf8(unsigned codePoint, std::string *out) {
        if (codePoint < 0x80) {
            out->push_back(static_cast<char>(codePoint));
        } else if (codePoint < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            out->push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            out->push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            out->push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }
    bool string(std::string *out) {
        ++index;
        out->clear();
        while (index < source.size()) {
            const unsigned char ch = static_cast<unsigned char>(source[index++]);
            if (ch == '"') return true;
            if (ch != '\\') {
                out->push_back(static_cast<char>(ch));
                continue;
            }
            if (index >= source.size()) return fail("bad escape");
            const char escape = source[index++];
            switch (escape) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                if (index + 4 > source.size()) return fail("bad \\u escape");
                unsigned codePoint = 0;
                for (int i = 0; i < 4; ++i) {
                    const int digit = hexValue(source[index++]);
                    if (digit < 0) return fail("bad \\u escape");
                    codePoint = codePoint * 16 + static_cast<unsigned>(digit);
                }
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF && index + 6 <= source.size() &&
                    source[index] == '\\' && source[index + 1] == 'u') {
                    index += 2;
                    unsigned low = 0;
                    for (int i = 0; i < 4; ++i) {
                        const int digit = hexValue(source[index++]);
                        if (digit < 0) return fail("bad surrogate escape");
                        low = low * 16 + static_cast<unsigned>(digit);
                    }
                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                }
                encodeUtf8(codePoint, out);
                break;
            }
            default: return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }
    bool number(Json *out) {
        const size_t start = index;
        out->type = Json::Type::Number;
        if (source[index] == '-') ++index;
        while (index < source.size() && source[index] >= '0' && source[index] <= '9') ++index;
        if (index < source.size() && source[index] == '.') {
            ++index;
            while (index < source.size() && source[index] >= '0' && source[index] <= '9') ++index;
        }
        if (index < source.size() && (source[index] == 'e' || source[index] == 'E')) {
            ++index;
            if (index < source.size() && (source[index] == '+' || source[index] == '-')) ++index;
            while (index < source.size() && source[index] >= '0' && source[index] <= '9') ++index;
        }
        if (index == start) return fail("bad number");
        out->number = source.substr(start, index - start);
        return true;
    }
};

bool parseJson(const std::string &text, Json *out, std::string *why) {
    JsonParser parser{text, 0, why};
    return parser.parse(out);
}

// ---------------------------------------------------------------------------
// options
// ---------------------------------------------------------------------------

struct Options {
    std::filesystem::path modulePath;
    std::filesystem::path enginePath;
    std::filesystem::path modelPath;
    std::string engineSha;
    std::string modelSha;
    std::filesystem::path referencePath;
    std::filesystem::path admissionPath;

    std::string deviceUuid;
    std::string decoderUuid;
    std::string adapterLuid;

    std::string buildRuntime;
    std::string buildPlugin;
    std::string buildCoreSha;
    std::string buildOptions;
    std::string buildPrecision = "fp16";
    int buildSourceAbi = 8;
    int buildTf32 = 0;
    int buildStronglyTyped = 1;
    int buildSkipInference = 0;

    std::string sourceCsp = "yuv444p16";
    std::string sourceMatrix = "bt709";
    std::string sourceRange = "limited";
    std::string sourceSiting = "left";
    int sourceBitDepth = 8;
    unsigned long long sourceFpsNum = 25;
    unsigned long long sourceFpsDen = 1;

    std::string requestedPlan;
    std::string recipeId;
    std::string buildKey;
    std::string modelId;
    std::string modelContentId;
    std::string inputTensor;
    std::string outputTensor;
    unsigned long long revision = 1;
    unsigned long long epoch = 1;

    std::filesystem::path workDir;
    std::vector<std::filesystem::path> dependencyDirs;
    bool skipRefusals = false;
    bool measure = true;
    bool queryFactory = false;
    bool help = false;
};

void usage() {
    std::fprintf(stderr,
        "%s - drive the managed prepared-plan v1 runtime on real hardware.\n"
        "\n"
        "usage:\n"
        "  %s --module <aji.dll> --engine <path> --engine-sha256 <64hex>\n"
        "     --model <path> --model-sha256 <64hex> --reference <reference.json>\n"
        "     --device-uuid <uuid> --decoder-uuid <uuid>\n"
        "     --build-runtime <major.minor.patch> --build-plugin <text>\n"
        "     --build-core-sha <40|64hex> --build-key <text>\n"
        "     [--admission <admission.json>] [--requested-plan <text>] [--recipe-id <text>]\n"
        "     [--build-options \"--name[=value] ...\"] [--build-precision fp16|fp32]\n"
        "     [--source-matrix bt709|bt601|bt2020] [--source-range limited|full]\n"
        "     [--source-fps <num/den>] [--revision N] [--epoch N] [--work-dir <dir>]\n"
        "     [--skip-refusals] [--no-measure]\n"
        "\n"
        "  --query-factory drives only aji_native_query_factory on this probe's own\n"
        "  already-adopted compute context (requires --module and --device-uuid). It\n"
        "  is a DIRECT-COMPUTE factory proof: a real context, device and runtime\n"
        "  factory, NOT a decoded source, an N1 chain or an artifact/readiness claim.\n"
        "  The prepared-plan and measurement inputs are not used in that mode.\n"
        "\n"
        "  --dependency-dir <absolute dir> registers one NativeBundle dependency\n"
        "  directory (cuda / mpv-deps / ort / trt) with AddDllDirectory for this\n"
        "  process, so the loaded module and the backend it loads resolve their\n"
        "  dependencies there. Repeatable. Each argument must be an existing\n"
        "  absolute directory with no '..'; a missing, relative or traversing path\n"
        "  is refused rather than searched in the current directory. Without it,\n"
        "  only the module's own directory and the system directories are searched.\n"
        "\n"
        "  --admission replaces the explicit stage/source/build facts with an existing\n"
        "  mikai.native.admission/1 document; the digest and device checks are unchanged.\n"
        "  --reference is always required: it supplies the static input and the spatial\n"
        "  stage geometry. --no-measure skips only the numerical output comparison.\n",
        kProbeName, kProbeName);
}

bool needValue(int argc, char **argv, int *index, std::string *out, std::string *why) {
    if (*index + 1 >= argc) {
        *why = std::string("missing value for ") + argv[*index];
        return false;
    }
    *out = argv[++(*index)];
    return true;
}

bool parseOptions(int argc, char **argv, Options *opt, std::string *why) {
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        std::string value;
        auto take = [&](std::string *out) { return needValue(argc, argv, &i, out, why); };
        if (flag == "--help" || flag == "-h") {
            opt->help = true;
            return true;
        } else if (flag == "--module") {
            if (!take(&value)) return false;
            opt->modulePath = std::filesystem::path(widen(value));
        } else if (flag == "--engine") {
            if (!take(&value)) return false;
            opt->enginePath = std::filesystem::path(widen(value));
        } else if (flag == "--engine-sha256") {
            if (!take(&opt->engineSha)) return false;
        } else if (flag == "--model") {
            if (!take(&value)) return false;
            opt->modelPath = std::filesystem::path(widen(value));
        } else if (flag == "--model-sha256") {
            if (!take(&opt->modelSha)) return false;
        } else if (flag == "--reference") {
            if (!take(&value)) return false;
            opt->referencePath = std::filesystem::path(widen(value));
        } else if (flag == "--admission") {
            if (!take(&value)) return false;
            opt->admissionPath = std::filesystem::path(widen(value));
        } else if (flag == "--device-uuid") {
            if (!take(&opt->deviceUuid)) return false;
        } else if (flag == "--decoder-uuid") {
            if (!take(&opt->decoderUuid)) return false;
        } else if (flag == "--adapter-luid") {
            if (!take(&opt->adapterLuid)) return false;
        } else if (flag == "--build-runtime") {
            if (!take(&opt->buildRuntime)) return false;
        } else if (flag == "--build-plugin") {
            if (!take(&opt->buildPlugin)) return false;
        } else if (flag == "--build-core-sha") {
            if (!take(&opt->buildCoreSha)) return false;
        } else if (flag == "--build-options") {
            if (!take(&opt->buildOptions)) return false;
        } else if (flag == "--build-precision") {
            if (!take(&opt->buildPrecision)) return false;
        } else if (flag == "--build-source-abi") {
            if (!take(&value)) return false;
            opt->buildSourceAbi = std::atoi(value.c_str());
        } else if (flag == "--build-tf32") {
            if (!take(&value)) return false;
            opt->buildTf32 = std::atoi(value.c_str());
        } else if (flag == "--build-strongly-typed") {
            if (!take(&value)) return false;
            opt->buildStronglyTyped = std::atoi(value.c_str());
        } else if (flag == "--build-skip-inference") {
            if (!take(&value)) return false;
            opt->buildSkipInference = std::atoi(value.c_str());
        } else if (flag == "--source-csp") {
            if (!take(&opt->sourceCsp)) return false;
        } else if (flag == "--source-matrix") {
            if (!take(&opt->sourceMatrix)) return false;
        } else if (flag == "--source-range") {
            if (!take(&opt->sourceRange)) return false;
        } else if (flag == "--source-siting") {
            if (!take(&opt->sourceSiting)) return false;
        } else if (flag == "--source-bit-depth") {
            if (!take(&value)) return false;
            opt->sourceBitDepth = std::atoi(value.c_str());
        } else if (flag == "--source-fps") {
            if (!take(&value)) return false;
            const size_t slash = value.find('/');
            if (slash == std::string::npos) {
                *why = "--source-fps must be <num>/<den>";
                return false;
            }
            opt->sourceFpsNum = std::strtoull(value.substr(0, slash).c_str(), nullptr, 10);
            opt->sourceFpsDen = std::strtoull(value.substr(slash + 1).c_str(), nullptr, 10);
        } else if (flag == "--requested-plan") {
            if (!take(&opt->requestedPlan)) return false;
        } else if (flag == "--recipe-id") {
            if (!take(&opt->recipeId)) return false;
        } else if (flag == "--build-key") {
            if (!take(&opt->buildKey)) return false;
        } else if (flag == "--model-id") {
            if (!take(&opt->modelId)) return false;
        } else if (flag == "--model-content-id") {
            if (!take(&opt->modelContentId)) return false;
        } else if (flag == "--input-tensor") {
            if (!take(&opt->inputTensor)) return false;
        } else if (flag == "--output-tensor") {
            if (!take(&opt->outputTensor)) return false;
        } else if (flag == "--revision") {
            if (!take(&value)) return false;
            opt->revision = std::strtoull(value.c_str(), nullptr, 10);
        } else if (flag == "--epoch") {
            if (!take(&value)) return false;
            opt->epoch = std::strtoull(value.c_str(), nullptr, 10);
        } else if (flag == "--work-dir") {
            if (!take(&value)) return false;
            opt->workDir = std::filesystem::path(widen(value));
        } else if (flag == "--skip-refusals") {
            opt->skipRefusals = true;
        } else if (flag == "--no-measure") {
            opt->measure = false;
        } else if (flag == "--dependency-dir") {
            if (!take(&value)) return false;
            opt->dependencyDirs.push_back(std::filesystem::path(widen(value)));
        } else if (flag == "--query-factory") {
            opt->queryFactory = true;
        } else {
            *why = "unknown argument " + flag;
            return false;
        }
    }
    if (opt->modulePath.empty()) { *why = "missing required input --module"; return false; }
    if (opt->deviceUuid.empty()) { *why = "missing required input --device-uuid"; return false; }
    {
        std::vector<std::filesystem::path> canonical;
        for (const std::filesystem::path &raw : opt->dependencyDirs) {
            if (raw.empty()) { *why = "--dependency-dir must not be empty"; return false; }
            if (!raw.is_absolute()) {
                *why = "--dependency-dir must be an absolute directory: " + toUtf8(raw);
                return false;
            }
            for (const std::filesystem::path &part : raw) {
                if (part == std::filesystem::path(L"..")) {
                    *why = "--dependency-dir must not contain '..': " + toUtf8(raw);
                    return false;
                }
            }
            std::error_code canonicalError;
            const std::filesystem::path full =
                std::filesystem::weakly_canonical(raw, canonicalError);
            if (canonicalError) {
                *why = "cannot resolve --dependency-dir " + toUtf8(raw) + ": " +
                       canonicalError.message();
                return false;
            }
            std::error_code dirError;
            if (!std::filesystem::is_directory(full, dirError)) {
                *why = "--dependency-dir is not an existing directory: " + toUtf8(raw);
                return false;
            }
            canonical.push_back(full);
        }
        opt->dependencyDirs = std::move(canonical);
    }
    if (opt->workDir.empty()) {
        std::error_code error;
        const std::filesystem::path base = std::filesystem::temp_directory_path(error);
        opt->workDir = (error ? std::filesystem::path(L".") : base) /
                       std::filesystem::path(L"mikai_prepared_probe_" +
                                             std::to_wstring(GetCurrentProcessId()));
    }
    if (opt->queryFactory)
        return true;
    if (opt->enginePath.empty()) { *why = "missing required input --engine"; return false; }
    if (opt->modelPath.empty()) { *why = "missing required input --model"; return false; }
    if (!isLowerHex(opt->engineSha, 64)) { *why = "--engine-sha256 must be 64 lower-case hex"; return false; }
    if (!isLowerHex(opt->modelSha, 64)) { *why = "--model-sha256 must be 64 lower-case hex"; return false; }
    if (opt->referencePath.empty()) { *why = "missing required input --reference"; return false; }
    if (opt->admissionPath.empty()) {
        if (opt->decoderUuid.empty()) { *why = "missing required input --decoder-uuid"; return false; }
        if (opt->buildRuntime.empty()) { *why = "missing required input --build-runtime"; return false; }
        if (opt->buildPlugin.empty()) { *why = "missing required input --build-plugin"; return false; }
        if (!isLowerHex(opt->buildCoreSha, 40) && !isLowerHex(opt->buildCoreSha, 64)) {
            *why = "--build-core-sha must be 40 or 64 lower-case hex";
            return false;
        }
        if (opt->buildKey.empty()) { *why = "missing required input --build-key"; return false; }
    }
    return true;
}

// ---------------------------------------------------------------------------
// reference manifest (mikai.frameReference)
// ---------------------------------------------------------------------------

struct RefTensor {
    std::string name;
    std::string dtype;
    std::string file;
    std::string sha;
    unsigned long long bytes = 0;
    std::vector<long long> shape;
};

struct Reference {
    std::filesystem::path manifestPath;
    std::filesystem::path dir;
    std::string graphPath;
    std::string graphSha;
    RefTensor input;
    RefTensor output;
};

bool tensorFromJson(const Json &object, RefTensor *tensor, std::string *why) {
    if (!object.isObject()) { *why = "reference tensor entry is not an object"; return false; }
    const Json *name = object.member("name");
    const Json *dtype = object.member("dtype");
    const Json *file = object.member("file");
    const Json *sha = object.member("sha256");
    const Json *shape = object.member("shape");
    if (!name || !dtype || !file || !sha || !shape || !shape->isArray()) {
        *why = "reference tensor entry misses a required field";
        return false;
    }
    tensor->name = name->str();
    tensor->dtype = dtype->str();
    tensor->file = file->str();
    tensor->sha = sha->str();
    if (tensor->name.empty() || tensor->file.empty() || !isLowerHex(tensor->sha, 64)) {
        *why = "reference tensor has an empty name/file or a non-sha256 digest";
        return false;
    }
    if (tensor->dtype != "fp16") {
        *why = "reference tensor dtype is not fp16";
        return false;
    }
    unsigned long long elements = 1;
    for (size_t i = 0; i < shape->count(); ++i) {
        const Json *dim = shape->item(i);
        if (!dim || !dim->isNumber()) { *why = "reference shape has a non-number dimension"; return false; }
        const long long value = std::strtoll(dim->number.c_str(), nullptr, 10);
        if (value <= 0) { *why = "reference shape has a non-positive dimension"; return false; }
        tensor->shape.push_back(value);
        elements *= static_cast<unsigned long long>(value);
    }
    if (tensor->shape.size() != 4) { *why = "reference tensor is not rank 4 (NxCxHxW)"; return false; }
    tensor->bytes = elements * 2;
    return true;
}

bool loadReference(const std::filesystem::path &manifestPath, Reference *reference,
                   std::string *why) {
    std::vector<unsigned char> bytes;
    if (!readFileBytes(manifestPath, &bytes, why)) return false;
    Json root;
    if (!parseJson(std::string(bytes.begin(), bytes.end()), &root, why)) {
        *why = "reference manifest is not valid JSON: " + *why;
        return false;
    }
    if (!root.isObject()) { *why = "reference manifest is not a JSON object"; return false; }
    const Json *schema = root.member("schema");
    if (!schema || schema->str() != "mikai.frameReference") {
        *why = "reference manifest schema is not mikai.frameReference";
        return false;
    }
    reference->manifestPath = manifestPath;
    reference->dir = manifestPath.parent_path();
    const Json *graphPath = root.member("graphPath");
    const Json *graphSha = root.member("graphSha256");
    if (!graphSha || !isLowerHex(graphSha->str(), 64)) {
        *why = "reference manifest has no graphSha256";
        return false;
    }
    reference->graphSha = graphSha->str();
    reference->graphPath = graphPath ? graphPath->str() : std::string();
    const Json *inputs = root.member("inputs");
    const Json *outputs = root.member("outputs");
    if (!inputs || !inputs->isArray() || inputs->count() != 1 ||
        !outputs || !outputs->isArray() || outputs->count() != 1) {
        *why = "reference manifest must carry exactly one input and one output tensor";
        return false;
    }
    if (!tensorFromJson(*inputs->item(0), &reference->input, why)) return false;
    if (!tensorFromJson(*outputs->item(0), &reference->output, why)) return false;
    // Resolve and re-hash every file this measurement depends on.
    auto resolve = [&](const RefTensor &tensor, std::filesystem::path *path) {
        const std::filesystem::path relative = std::filesystem::path(widen(tensor.file));
        if (relative.is_absolute()) {
            *why = "reference tensor file is absolute: " + tensor.file;
            return false;
        }
        for (const auto &component : relative)
            if (component == "..") { *why = "reference tensor file escapes its directory"; return false; }
        *path = reference->dir / relative;
        return true;
    };
    std::filesystem::path inputPath, outputPath;
    if (!resolve(reference->input, &inputPath)) return false;
    if (!resolve(reference->output, &outputPath)) return false;
    for (const auto &pair : {std::make_pair(&reference->input, &inputPath),
                             std::make_pair(&reference->output, &outputPath)}) {
        const RefTensor &tensor = *pair.first;
        std::string actual;
        if (!sha256File(*pair.second, &actual, why)) return false;
        if (actual != tensor.sha) {
            *why = "reference tensor " + tensor.name + " digest mismatch (manifest " +
                   tensor.sha + ", actual " + actual + ")";
            return false;
        }
    }
    if (!reference->graphPath.empty()) {
        std::string actual;
        std::string graphWhy;
        if (!sha256File(std::filesystem::path(widen(reference->graphPath)), &actual, &graphWhy)) {
            *why = "reference graphPath is not readable: " + graphWhy;
            return false;
        }
        if (actual != reference->graphSha) {
            *why = "reference graph digest mismatch (manifest " + reference->graphSha +
                   ", actual " + actual + ")";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// CUDA driver (resolved from nvcuda.dll; no link dependency)
// ---------------------------------------------------------------------------

using CUresult = int;
using CUdevice = int;
using CUdeviceptr = unsigned long long;
struct CUuuid { char bytes[16]; };
struct CUctx_st;
using CUcontext = CUctx_st *;
struct CUstream_st;
using CUstream = CUstream_st *;

constexpr CUresult kCudaSuccess = 0;
constexpr int kAttributeComputeCapabilityMajor = 75;
constexpr int kAttributeComputeCapabilityMinor = 76;

void *driverSymbol(HMODULE module, const char *preferred, const char *fallback) {
    void *resolved = reinterpret_cast<void *>(GetProcAddress(module, preferred));
    if (!resolved && fallback) resolved = reinterpret_cast<void *>(GetProcAddress(module, fallback));
    return resolved;
}

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
        deviceGetCount = reinterpret_cast<decltype(deviceGetCount)>(driverSymbol(module, "cuDeviceGetCount", nullptr));
        deviceGet = reinterpret_cast<decltype(deviceGet)>(driverSymbol(module, "cuDeviceGet", nullptr));
        deviceGetUuid = reinterpret_cast<decltype(deviceGetUuid)>(driverSymbol(module, "cuDeviceGetUuid_v2", "cuDeviceGetUuid"));
        deviceGetName = reinterpret_cast<decltype(deviceGetName)>(driverSymbol(module, "cuDeviceGetName", nullptr));
        deviceGetAttribute = reinterpret_cast<decltype(deviceGetAttribute)>(driverSymbol(module, "cuDeviceGetAttribute", nullptr));
        primaryCtxRetain = reinterpret_cast<decltype(primaryCtxRetain)>(driverSymbol(module, "cuDevicePrimaryCtxRetain", nullptr));
        primaryCtxRelease = reinterpret_cast<decltype(primaryCtxRelease)>(driverSymbol(module, "cuDevicePrimaryCtxRelease_v2", "cuDevicePrimaryCtxRelease"));
        ctxGetCurrent = reinterpret_cast<decltype(ctxGetCurrent)>(driverSymbol(module, "cuCtxGetCurrent", nullptr));
        ctxSetCurrent = reinterpret_cast<decltype(ctxSetCurrent)>(driverSymbol(module, "cuCtxSetCurrent", nullptr));
        ctxGetDevice = reinterpret_cast<decltype(ctxGetDevice)>(driverSymbol(module, "cuCtxGetDevice", nullptr));
        memAlloc = reinterpret_cast<decltype(memAlloc)>(driverSymbol(module, "cuMemAlloc_v2", "cuMemAlloc"));
        memFree = reinterpret_cast<decltype(memFree)>(driverSymbol(module, "cuMemFree_v2", "cuMemFree"));
        memcpyHtoD = reinterpret_cast<decltype(memcpyHtoD)>(driverSymbol(module, "cuMemcpyHtoD_v2", "cuMemcpyHtoD"));
        memcpyDtoH = reinterpret_cast<decltype(memcpyDtoH)>(driverSymbol(module, "cuMemcpyDtoH_v2", "cuMemcpyDtoH"));
        streamCreate = reinterpret_cast<decltype(streamCreate)>(driverSymbol(module, "cuStreamCreate", nullptr));
        streamDestroy = reinterpret_cast<decltype(streamDestroy)>(driverSymbol(module, "cuStreamDestroy_v2", "cuStreamDestroy"));
        streamSynchronize = reinterpret_cast<decltype(streamSynchronize)>(driverSymbol(module, "cuStreamSynchronize", nullptr));
        driverGetVersion = reinterpret_cast<decltype(driverGetVersion)>(driverSymbol(module, "cuDriverGetVersion", nullptr));
        getErrorName = reinterpret_cast<decltype(getErrorName)>(driverSymbol(module, "cuGetErrorName", nullptr));
        getErrorString = reinterpret_cast<decltype(getErrorString)>(driverSymbol(module, "cuGetErrorString", nullptr));
        if (!init || !deviceGetCount || !deviceGet || !deviceGetUuid || !deviceGetAttribute ||
            !primaryCtxRetain || !primaryCtxRelease || !ctxGetCurrent || !ctxSetCurrent ||
            !ctxGetDevice || !memAlloc || !memFree || !memcpyHtoD || !memcpyDtoH ||
            !streamCreate || !streamDestroy || !streamSynchronize) {
            *why = "nvcuda.dll is missing a required driver entry point";
            return false;
        }
        return true;
    }

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
};

std::string uuidHex(const CUuuid &uuid) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (char raw : uuid.bytes) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0f]);
    }
    return out;
}

bool parseUuid(const std::string &input, CUuuid *uuid, std::string *why) {
    std::string digits = input;
    if (digits.rfind("GPU-", 0) == 0) digits.erase(0, 4);
    std::memset(uuid, 0, sizeof(*uuid));
    int nibbles = 0;
    for (char ch : digits) {
        if (ch == '-' || ch == '{' || ch == '}') continue;
        int value = -1;
        if (ch >= '0' && ch <= '9') value = ch - '0';
        else if (ch >= 'a' && ch <= 'f') value = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') value = ch - 'A' + 10;
        if (value < 0 || nibbles >= 32) {
            *why = "--device-uuid \"" + input + "\" is not a 16-byte CUDA UUID";
            return false;
        }
        unsigned char &byte = reinterpret_cast<unsigned char *>(uuid->bytes)[nibbles / 2];
        if (nibbles % 2 == 0) byte = static_cast<unsigned char>(value << 4);
        else byte = static_cast<unsigned char>(byte | value);
        ++nibbles;
    }
    if (nibbles != 32) {
        *why = "--device-uuid \"" + input + "\" is not a 16-byte CUDA UUID";
        return false;
    }
    return true;
}

struct DeviceBuffer {
    CudaDriver *driver = nullptr;
    CUdeviceptr pointer = 0;
    DeviceBuffer() = default;
    DeviceBuffer(const DeviceBuffer &) = delete;
    DeviceBuffer &operator=(const DeviceBuffer &) = delete;
    ~DeviceBuffer() {
        if (driver && pointer) driver->memFree(pointer);
    }
    bool allocate(CudaDriver &cuda, size_t bytes, const char *what, std::string *why) {
        CUdeviceptr allocated = 0;
        const CUresult result = cuda.memAlloc(&allocated, bytes);
        if (result != kCudaSuccess) {
            *why = std::string(what) + " allocation failed: " + cuda.text(result);
            return false;
        }
        driver = &cuda;
        pointer = allocated;
        return true;
    }
    void *asVoid() const { return reinterpret_cast<void *>(static_cast<uintptr_t>(pointer)); }
};

// ---------------------------------------------------------------------------
// fp16 + the documented 4:4:4 matrix (mirrors kernels.cu k_pre444/k_post444)
// ---------------------------------------------------------------------------

float halfToFloat(uint16_t half) {
    const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
    uint32_t exponent = (half >> 10) & 0x1Fu;
    uint32_t mantissa = half & 0x3FFu;
    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            int shift = -1;
            uint32_t value = mantissa;
            do {
                ++shift;
                value <<= 1;
            } while ((value & 0x400u) == 0);
            bits = sign | (static_cast<uint32_t>(127 - 15 - shift) << 23) |
                   ((value & 0x3FFu) << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

struct Csp {
    double kr = 0.2126;
    double kb = 0.0722;
    double yoff = 0.0;
    double yscale = 65535.0;
    double coff = 32768.0;
    double cscale = 65535.0;
};

Csp makeCsp444(int matrix, int range) {
    Csp csp;
    switch (matrix) {
    case AJI_MATRIX_BT601: csp.kr = 0.299; csp.kb = 0.114; break;
    case AJI_MATRIX_BT2020: csp.kr = 0.2627; csp.kb = 0.0593; break;
    default: csp.kr = 0.2126; csp.kb = 0.0722; break;
    }
    const double scale = 256.0;
    if (range == AJI_RANGE_FULL) {
        csp.yoff = 0.0;
        csp.yscale = 65535.0;
        csp.coff = 32768.0;
        csp.cscale = 65535.0;
    } else {
        csp.yoff = 16.0 * scale;
        csp.yscale = 219.0 * scale;
        csp.coff = 128.0 * scale;
        csp.cscale = 224.0 * scale;
    }
    return csp;
}

int matrixFromName(const std::string &name) {
    if (equalsNoCase(name, "bt601")) return AJI_MATRIX_BT601;
    if (equalsNoCase(name, "bt2020")) return AJI_MATRIX_BT2020;
    return AJI_MATRIX_BT709;
}

int rangeFromName(const std::string &name) {
    return equalsNoCase(name, "full") ? AJI_RANGE_FULL : AJI_RANGE_LIMITED;
}

uint16_t quantize16(double value) {
    return static_cast<uint16_t>(std::min(65535.0, std::max(0.0, std::rint(value))));
}

void rgbToYuv444(double r, double g, double b, const Csp &csp, double *y, double *u, double *v) {
    *y = csp.kr * r + (1.0 - csp.kr - csp.kb) * g + csp.kb * b;
    *u = (b - *y) / (2.0 * (1.0 - csp.kb));
    *v = (r - *y) / (2.0 * (1.0 - csp.kr));
}

void yuv444ToRgb(double y, double u, double v, const Csp &csp, double *r, double *g, double *b) {
    const double kg = 1.0 - csp.kr - csp.kb;
    *r = y + 2.0 * (1.0 - csp.kr) * v;
    *b = y + 2.0 * (1.0 - csp.kb) * u;
    *g = y - (2.0 * csp.kb * (1.0 - csp.kb) * u + 2.0 * csp.kr * (1.0 - csp.kr) * v) / kg;
}

// ---------------------------------------------------------------------------
// plan construction
// ---------------------------------------------------------------------------

struct StageSpec {
    uint32_t role = AJI_NATIVE_ROLE_SPATIAL;
    uint32_t order = AJI_NATIVE_ORDER_NONE;
    uint32_t ordinal = 0;
    uint32_t inWidth = 0, inHeight = 0, outWidth = 0, outHeight = 0;
    uint32_t contentWidth = 0, contentHeight = 0, padLeft = 0, padTop = 0;
    uint32_t inChannels = 3, outChannels = 3;
    uint32_t inDtype = AJI_NATIVE_DTYPE_FP16, outDtype = AJI_NATIVE_DTYPE_FP16;
    uint32_t factorNum = 0, factorDen = 0;
    double sceneThreshold = 0.0;
    std::string modelId, modelContentId, modelSha, engineSha, buildKey;
    std::string modelPath, enginePath, inputTensor, outputTensor;
};

bool copyText(char *destination, size_t capacity, const std::string &text,
              const char *field, std::string *why) {
    if (text.size() + 1 > capacity) {
        *why = std::string(field) + " is longer than " + std::to_string(capacity - 1) + " bytes";
        return false;
    }
    std::memcpy(destination, text.c_str(), text.size() + 1);
    return true;
}

struct PlanFacts {
    std::string requestedPlan = "probe:managed-prepared-v1";
    std::string recipeId = "probe.recipe";
    std::string backend = "TensorRT";
    std::string deviceIdentity;
    std::string decoderUuid;
    std::string computeUuid;
    std::string adapterLuid;
    uint32_t smMajor = 0, smMinor = 0;
    uint32_t codedWidth = 0, codedHeight = 0;
    uint32_t cropX = 0, cropY = 0, cropWidth = 0, cropHeight = 0;
    uint32_t rotation = 0;
    uint32_t fpsNum = 25, fpsDen = 1;
    uint32_t bitDepth = 8;
    std::string csp = "yuv444p16";
    std::string matrix = "bt709";
    std::string range = "limited";
    std::string siting = "left";
    std::string buildProvider = "TensorRT";
    std::string buildRuntime;
    std::string buildPlugin;
    std::string buildCoreSha;
    uint32_t buildSourceAbi = 8;
    std::string buildPrecision = "fp16";
    uint32_t buildTf32 = 0, buildStronglyTyped = 1, buildSkipInference = 0;
    std::string buildOptions;
    unsigned long long revision = 1;
    unsigned long long epoch = 1;
};

bool buildPlan(const PlanFacts &facts, const std::vector<StageSpec> &stages,
               aji_native_prepared_plan_v1 *plan, std::string *why) {
    if (stages.empty() || stages.size() > AJI_NATIVE_MAX_STAGES) {
        *why = "a plan needs 1..AJI_NATIVE_MAX_STAGES stages";
        return false;
    }
    std::memset(plan, 0, sizeof(*plan));
    plan->struct_size = sizeof(*plan);
    plan->requested_protocol = AJI_NATIVE_PREPARED_PROTOCOL_VERSION;
    plan->stage_count = static_cast<uint32_t>(stages.size());
    plan->flags = AJI_NATIVE_PLAN_FLAG_PREPARED_ONLY;
    plan->revision = facts.revision;
    plan->epoch = facts.epoch;
    if (!copyText(plan->requested_plan, sizeof(plan->requested_plan), facts.requestedPlan, "requested_plan", why) ||
        !copyText(plan->recipe_id, sizeof(plan->recipe_id), facts.recipeId, "recipe_id", why) ||
        !copyText(plan->backend, sizeof(plan->backend), facts.backend, "backend", why) ||
        !copyText(plan->device_identity, sizeof(plan->device_identity), facts.deviceIdentity, "device_identity", why) ||
        !copyText(plan->decoder_device_uuid, sizeof(plan->decoder_device_uuid), facts.decoderUuid, "decoder_device_uuid", why) ||
        !copyText(plan->compute_device_uuid, sizeof(plan->compute_device_uuid), facts.computeUuid, "compute_device_uuid", why) ||
        !copyText(plan->adapter_luid, sizeof(plan->adapter_luid), facts.adapterLuid, "adapter_luid", why) ||
        !copyText(plan->source_csp, sizeof(plan->source_csp), facts.csp, "source_csp", why) ||
        !copyText(plan->source_matrix, sizeof(plan->source_matrix), facts.matrix, "source_matrix", why) ||
        !copyText(plan->source_range, sizeof(plan->source_range), facts.range, "source_range", why) ||
        !copyText(plan->source_siting, sizeof(plan->source_siting), facts.siting, "source_siting", why) ||
        !copyText(plan->build_provider, sizeof(plan->build_provider), facts.buildProvider, "build_provider", why) ||
        !copyText(plan->build_runtime, sizeof(plan->build_runtime), facts.buildRuntime, "build_runtime", why) ||
        !copyText(plan->build_plugin, sizeof(plan->build_plugin), facts.buildPlugin, "build_plugin", why) ||
        !copyText(plan->build_core_sha, sizeof(plan->build_core_sha), facts.buildCoreSha, "build_core_sha", why) ||
        !copyText(plan->build_precision, sizeof(plan->build_precision), facts.buildPrecision, "build_precision", why) ||
        !copyText(plan->build_options, sizeof(plan->build_options), facts.buildOptions, "build_options", why))
        return false;
    plan->source_coded_width = facts.codedWidth;
    plan->source_coded_height = facts.codedHeight;
    plan->source_crop_x = facts.cropX;
    plan->source_crop_y = facts.cropY;
    plan->source_crop_width = facts.cropWidth;
    plan->source_crop_height = facts.cropHeight;
    plan->source_rotation = facts.rotation;
    plan->source_fps_num = facts.fpsNum;
    plan->source_fps_den = facts.fpsDen;
    plan->source_bit_depth = facts.bitDepth;
    plan->device_sm_major = facts.smMajor;
    plan->device_sm_minor = facts.smMinor;
    plan->build_source_abi = facts.buildSourceAbi;
    plan->build_tf32 = facts.buildTf32;
    plan->build_strongly_typed = facts.buildStronglyTyped;
    plan->build_skip_inference = facts.buildSkipInference;

    for (size_t i = 0; i < stages.size(); ++i) {
        const StageSpec &spec = stages[i];
        aji_native_prepared_stage_v1 &stage = plan->stages[i];
        const std::string at = "stage " + std::to_string(i);
        stage.struct_size = sizeof(stage);
        stage.role = spec.role;
        stage.order = spec.order;
        stage.ordinal = spec.ordinal;
        stage.input_width = spec.inWidth;
        stage.input_height = spec.inHeight;
        stage.output_width = spec.outWidth;
        stage.output_height = spec.outHeight;
        stage.content_width = spec.contentWidth;
        stage.content_height = spec.contentHeight;
        stage.pad_left = spec.padLeft;
        stage.pad_top = spec.padTop;
        stage.input_channels = spec.inChannels;
        stage.output_channels = spec.outChannels;
        stage.input_dtype = spec.inDtype;
        stage.output_dtype = spec.outDtype;
        stage.input_layout = AJI_NATIVE_LAYOUT_CHW;
        stage.output_layout = AJI_NATIVE_LAYOUT_CHW;
        stage.factor_num = spec.factorNum;
        stage.factor_den = spec.factorDen;
        stage.scene_threshold = spec.sceneThreshold;
        if (!copyText(stage.model_id, sizeof(stage.model_id), spec.modelId, (at + " model_id").c_str(), why) ||
            !copyText(stage.model_content_id, sizeof(stage.model_content_id), spec.modelContentId, (at + " model_content_id").c_str(), why) ||
            !copyText(stage.model_sha256, sizeof(stage.model_sha256), spec.modelSha, (at + " model_sha256").c_str(), why) ||
            !copyText(stage.engine_sha256, sizeof(stage.engine_sha256), spec.engineSha, (at + " engine_sha256").c_str(), why) ||
            !copyText(stage.build_key, sizeof(stage.build_key), spec.buildKey, (at + " build_key").c_str(), why) ||
            !copyText(stage.model_path, sizeof(stage.model_path), spec.modelPath, (at + " model_path").c_str(), why) ||
            !copyText(stage.engine_path, sizeof(stage.engine_path), spec.enginePath, (at + " engine_path").c_str(), why) ||
            !copyText(stage.input_tensor, sizeof(stage.input_tensor), spec.inputTensor, (at + " input_tensor").c_str(), why) ||
            !copyText(stage.output_tensor, sizeof(stage.output_tensor), spec.outputTensor, (at + " output_tensor").c_str(), why))
            return false;
    }
    return true;
}

// Build the explicit-facts plan: one spatial stage whose geometry comes from
// the reference manifest (so the plan is measured against that manifest).
bool buildExplicitPlan(const Options &opt, const Reference &reference, const PlanFacts &base,
                       aji_native_prepared_plan_v1 *plan, std::string *why) {
    StageSpec stage;
    stage.role = AJI_NATIVE_ROLE_SPATIAL;
    stage.order = AJI_NATIVE_ORDER_NONE;
    stage.ordinal = 0;
    stage.inChannels = static_cast<uint32_t>(reference.input.shape[1]);
    stage.inHeight = static_cast<uint32_t>(reference.input.shape[2]);
    stage.inWidth = static_cast<uint32_t>(reference.input.shape[3]);
    stage.outChannels = static_cast<uint32_t>(reference.output.shape[1]);
    stage.outHeight = static_cast<uint32_t>(reference.output.shape[2]);
    stage.outWidth = static_cast<uint32_t>(reference.output.shape[3]);
    stage.contentWidth = stage.inWidth;
    stage.contentHeight = stage.inHeight;
    stage.modelId = opt.modelId.empty() ? ("sha256:" + opt.modelSha.substr(0, 16)) : opt.modelId;
    stage.modelContentId = opt.modelContentId.empty() ? opt.modelSha : opt.modelContentId;
    stage.modelSha = opt.modelSha;
    stage.engineSha = opt.engineSha;
    stage.buildKey = opt.buildKey;
    stage.modelPath = toUtf8(opt.modelPath);
    stage.enginePath = toUtf8(opt.enginePath);
    stage.inputTensor = opt.inputTensor;
    stage.outputTensor = opt.outputTensor;

    PlanFacts facts = base;
    facts.codedWidth = stage.inWidth;
    facts.codedHeight = stage.inHeight;
    return buildPlan(facts, {stage}, plan, why);
}

// ---------------------------------------------------------------------------
// admission document -> plan (mikai.native.admission/1, the existing wire)
// ---------------------------------------------------------------------------

std::string jsonString(const Json *object, const char *key) {
    if (!object) return {};
    const Json *value = object->member(key);
    return value ? value->str() : std::string();
}

int jsonInt(const Json *object, const char *key, int fallback) {
    if (!object) return fallback;
    const Json *value = object->member(key);
    return value ? value->integer(fallback) : fallback;
}

bool admissionToPlan(const std::filesystem::path &path, const std::string &actualUuid,
                     aji_native_prepared_plan_v1 *plan, std::string *why) {
    std::vector<unsigned char> bytes;
    if (!readFileBytes(path, &bytes, why)) return false;
    Json root;
    if (!parseJson(std::string(bytes.begin(), bytes.end()), &root, why)) {
        *why = "admission document is not valid JSON: " + *why;
        return false;
    }
    if (!root.isObject()) { *why = "admission document is not an object"; return false; }
    if (jsonString(&root, "schema") != "mikai.native.admission/1") {
        *why = "admission schema is not mikai.native.admission/1";
        return false;
    }
    if (jsonInt(&root, "protocol", 0) != 1) {
        *why = "admission protocol is not 1";
        return false;
    }
    const Json *requested = root.member("requested");
    const Json *source = root.member("source");
    const Json *device = root.member("device");
    const Json *build = root.member("build");
    const Json *stages = root.member("stages");
    if (!source || !device || !build || !stages || !stages->isArray() || stages->count() == 0) {
        *why = "admission document misses source/device/build/stages";
        return false;
    }
    PlanFacts facts;
    facts.requestedPlan = jsonString(&root, "requestId");
    if (facts.requestedPlan.empty()) { *why = "admission requestId is empty"; return false; }
    facts.recipeId = jsonString(requested, "recipe");
    facts.backend = jsonString(requested, "backend");
    if (facts.recipeId.empty()) { *why = "admission requested.recipe is empty"; return false; }
    if (facts.backend.empty()) { *why = "admission requested.backend is empty"; return false; }
    facts.revision = root.member("revision") ? root.member("revision")->u64(0) : 0;
    facts.epoch = root.member("epoch") ? root.member("epoch")->u64(0) : 0;
    facts.codedWidth = static_cast<uint32_t>(jsonInt(source, "codedWidth", 0));
    facts.codedHeight = static_cast<uint32_t>(jsonInt(source, "codedHeight", 0));
    if (const Json *crop = source->member("crop")) {
        facts.cropX = static_cast<uint32_t>(jsonInt(crop, "x", 0));
        facts.cropY = static_cast<uint32_t>(jsonInt(crop, "y", 0));
        facts.cropWidth = static_cast<uint32_t>(jsonInt(crop, "width", 0));
        facts.cropHeight = static_cast<uint32_t>(jsonInt(crop, "height", 0));
    }
    facts.rotation = static_cast<uint32_t>(jsonInt(source, "rotation", 0));
    if (const Json *fps = source->member("fps")) {
        facts.fpsNum = static_cast<uint32_t>(jsonInt(fps, "num", 25));
        facts.fpsDen = static_cast<uint32_t>(jsonInt(fps, "den", 1));
    }
    facts.bitDepth = static_cast<uint32_t>(jsonInt(source, "bitDepth", 0));
    facts.csp = jsonString(source, "csp");
    facts.matrix = jsonString(source, "matrix");
    facts.range = jsonString(source, "range");
    facts.siting = jsonString(source, "siting");
    facts.decoderUuid = jsonString(device, "decoderDeviceUuid");
    facts.computeUuid = jsonString(device, "computeDeviceUuid");
    facts.adapterLuid = jsonString(device, "adapterLuid");
    facts.smMajor = static_cast<uint32_t>(jsonInt(device, "smMajor", 0));
    facts.smMinor = static_cast<uint32_t>(jsonInt(device, "smMinor", 0));
    facts.deviceIdentity = facts.computeUuid;
    facts.buildProvider = jsonString(build, "provider");
    facts.buildRuntime = jsonString(build, "runtimeVersion");
    facts.buildPlugin = jsonString(build, "pluginVersion");
    facts.buildCoreSha = jsonString(build, "coreSha");
    facts.buildSourceAbi = static_cast<uint32_t>(jsonInt(build, "sourceAbi", 0));
    facts.buildPrecision = jsonString(build, "precision");
    facts.buildTf32 = static_cast<uint32_t>(jsonInt(build, "tf32", 0));
    facts.buildStronglyTyped = static_cast<uint32_t>(jsonInt(build, "stronglyTyped", 0));
    facts.buildSkipInference = static_cast<uint32_t>(jsonInt(build, "skipInference", 0));
    facts.buildOptions = jsonString(build, "canonicalOptions");
    // The actual device identity is measured by the probe, never copied: if the
    // admission does not name this device the identity check will refuse it.
    (void)actualUuid;

    std::vector<StageSpec> specs;
    for (size_t i = 0; i < stages->count(); ++i) {
        const Json *entry = stages->item(i);
        if (!entry || !entry->isObject()) { *why = "admission stage is not an object"; return false; }
        StageSpec spec;
        spec.ordinal = static_cast<uint32_t>(jsonInt(entry, "ordinal", static_cast<int>(i)));
        const std::string role = jsonString(entry, "role");
        if (equalsNoCase(role, "spatial")) spec.role = AJI_NATIVE_ROLE_SPATIAL;
        else if (equalsNoCase(role, "temporal")) spec.role = AJI_NATIVE_ROLE_TEMPORAL;
        else { *why = "admission stage role is neither spatial nor temporal"; return false; }
        const std::string order = jsonString(entry, "order");
        if (equalsNoCase(order, "before_spatial")) spec.order = AJI_NATIVE_ORDER_BEFORE_SPATIAL;
        else if (equalsNoCase(order, "after_spatial")) spec.order = AJI_NATIVE_ORDER_AFTER_SPATIAL;
        else spec.order = AJI_NATIVE_ORDER_NONE;
        const Json *inputShape = entry->member("inputShape");
        const Json *outputShape = entry->member("outputShape");
        const Json *content = entry->member("contentSize");
        if (!inputShape || !inputShape->isArray() || inputShape->count() != 4 ||
            !outputShape || !outputShape->isArray() || outputShape->count() != 4) {
            *why = "admission stage shape is not [1,C,H,W]";
            return false;
        }
        spec.inChannels = static_cast<uint32_t>(inputShape->item(1)->integer(0));
        spec.inHeight = static_cast<uint32_t>(inputShape->item(2)->integer(0));
        spec.inWidth = static_cast<uint32_t>(inputShape->item(3)->integer(0));
        spec.outChannels = static_cast<uint32_t>(outputShape->item(1)->integer(0));
        spec.outHeight = static_cast<uint32_t>(outputShape->item(2)->integer(0));
        spec.outWidth = static_cast<uint32_t>(outputShape->item(3)->integer(0));
        spec.contentWidth = static_cast<uint32_t>(jsonInt(content, "w", spec.inWidth));
        spec.contentHeight = static_cast<uint32_t>(jsonInt(content, "h", spec.inHeight));
        spec.padLeft = static_cast<uint32_t>(jsonInt(entry, "padLeft", 0));
        spec.padTop = static_cast<uint32_t>(jsonInt(entry, "padTop", 0));
        const std::string dtype = jsonString(entry, "inputDtype");
        spec.inDtype = equalsNoCase(dtype, "fp32") ? AJI_NATIVE_DTYPE_FP32 : AJI_NATIVE_DTYPE_FP16;
        spec.outDtype = equalsNoCase(jsonString(entry, "outputDtype"), "fp32") ? AJI_NATIVE_DTYPE_FP32 : AJI_NATIVE_DTYPE_FP16;
        spec.modelId = jsonString(entry, "modelId");
        spec.modelContentId = jsonString(entry, "modelContentId");
        spec.modelSha = jsonString(entry, "modelSha256");
        spec.engineSha = jsonString(entry, "engineSha256");
        spec.buildKey = jsonString(entry, "buildKey");
        spec.modelPath = jsonString(entry, "modelPath");
        spec.enginePath = jsonString(entry, "enginePath");
        spec.inputTensor = jsonString(entry, "inputTensor");
        spec.outputTensor = jsonString(entry, "outputTensor");
        if (const Json *factor = entry->member("factor")) {
            spec.factorNum = static_cast<uint32_t>(jsonInt(factor, "num", 0));
            spec.factorDen = static_cast<uint32_t>(jsonInt(factor, "den", 0));
        }
        if (const Json *scene = entry->member("sceneThreshold"))
            spec.sceneThreshold = scene->isNumber() ? std::strtod(scene->number.c_str(), nullptr) : 0.0;
        specs.push_back(spec);
    }
    return buildPlan(facts, specs, plan, why);
}

// ---------------------------------------------------------------------------
// probe state
// ---------------------------------------------------------------------------

using CreatePreparedFn = aji_ctx *(*)(const aji_create_params *,
                                      const aji_native_prepared_plan_v1 *,
                                      aji_native_snapshot_v2 *);
using ApplyPreparedFn = int (*)(aji_ctx *, const aji_native_prepared_plan_v1 *,
                                aji_native_snapshot_v2 *);
using ControlOnStreamFn = int (*)(aji_ctx *, uint32_t, uint32_t, uint64_t, uint32_t,
                                  void *, aji_native_snapshot_v2 *);
using GetSnapshotFn = int (*)(aji_ctx *, uint32_t, aji_native_snapshot_v2 *);
using ConfigureFn = int (*)(aji_ctx *, int, int, double, int *, int *);
using InferFn = int (*)(aji_ctx *, const aji_frame *, const aji_frame *, void *);
using FlushFn = uint64_t (*)(aji_ctx *, void *);
using DoneFn = int (*)(aji_ctx *, uint64_t);
using WaitFn = int (*)(aji_ctx *, uint64_t);
using LastErrorFn = const char *(*)(aji_ctx *);
using DestroyFn = void (*)(aji_ctx **);
using CurrentLogFn = const char *(*)(aji_ctx *);
using QueryFactoryFn = int (*)(const aji_create_params *,
                               aji_native_factory_capabilities_v1 *);

struct Symbols {
    CreatePreparedFn createPrepared = nullptr;
    ApplyPreparedFn applyPrepared = nullptr;
    ControlOnStreamFn controlOnStream = nullptr;
    GetSnapshotFn getSnapshot = nullptr;
    ConfigureFn configure = nullptr;
    InferFn infer = nullptr;
    FlushFn flush = nullptr;
    DoneFn done = nullptr;
    WaitFn wait = nullptr;
    LastErrorFn lastError = nullptr;
    DestroyFn destroy = nullptr;
    CurrentLogFn currentLog = nullptr;
    QueryFactoryFn queryFactory = nullptr;
};

struct Measurement {
    bool measured = false;
    double maxAbsoluteError = 0.0;
    double rmse = 0.0;
    unsigned long long elements = 0;
    unsigned long long elementsOutsideTolerance = 0;
    unsigned long long borderElementsOutsideTolerance = 0;
    unsigned long long clippedReferenceElements = 0;
    unsigned long long worstIndex = 0;
    int worstChannel = 0, worstY = 0, worstX = 0;
    double unclippedMaxAbsoluteError = 0.0;
    double unclippedRmse = 0.0;
    unsigned long long unclippedElements = 0;
    bool withinTolerance = false;
    bool unclippedWithinTolerance = false;
};

// Process-wide dependency search directories registered for the loaded module
// and any backend it loads. The AddDllDirectory cookies stay alive until the
// module and its contexts are gone; the destructor removes them afterwards.
struct DependencySearchDirs {
    std::vector<DLL_DIRECTORY_COOKIE> cookies;
    ~DependencySearchDirs() {
        for (DLL_DIRECTORY_COOKIE cookie : cookies)
            if (cookie)
                RemoveDllDirectory(cookie);
    }
    bool add(const std::filesystem::path &dir, std::string *why) {
        const DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(dir.c_str());
        if (!cookie) {
            *why = "AddDllDirectory failed for " + toUtf8(dir) + " (error " +
                   std::to_string(GetLastError()) + ")";
            return false;
        }
        cookies.push_back(cookie);
        return true;
    }
};

// ---------------------------------------------------------------------------
// the probe
// ---------------------------------------------------------------------------

struct Probe {
    Options opt;
    CudaDriver cuda;
    Reference reference;
    aji_native_prepared_plan_v1 plan{};
    aji_native_prepared_plan_v1 copyPlan{};
    Symbols api;
    void *module = nullptr;
    DependencySearchDirs dllDirs;
    CUuuid uuid{};
    CUdevice device = 0;
    std::string uuidHexText;
    std::string deviceName;
    int smMajor = 0, smMinor = 0;
    int driverVersion = 0;
    CUcontext context = nullptr;
    int retainedDevice = -1;
    CUcontext previousContext = nullptr;
    CUstream stream = nullptr;
    aji_ctx *ctx = nullptr;
    int matrix = AJI_MATRIX_BT709;
    int range = AJI_RANGE_LIMITED;
    Csp csp;
    DeviceBuffer inY, inU, inV, outY, outU, outV;
    std::vector<uint16_t> hostInY, hostInU, hostInV;
    std::vector<float> hostRefRgb;
    std::vector<uint16_t> hostRefOut;
    std::vector<uint16_t> hostOutY, hostOutU, hostOutV;
    Measurement measurement;
    std::vector<std::string> scenarios;
    std::vector<std::string> failures;
    std::ostringstream report;

    bool check(bool ok, const std::string &name, const std::string &detail) {
        scenarios.push_back("{\"name\":" + jsonQuote(name) + ",\"ok\":" +
                            (ok ? "true" : "false") + ",\"detail\":" + jsonQuote(detail) + "}");
        if (!ok) failures.push_back(name + ": " + detail);
        std::fprintf(stderr, "%s: %s - %s\n", ok ? "ok" : "FAIL", name.c_str(), detail.c_str());
        return ok;
    }

    bool takeSnapshot(aji_native_snapshot_v2 *snapshot, std::string *why) {
        std::memset(snapshot, 0, sizeof(*snapshot));
        snapshot->struct_size = sizeof(*snapshot);
        const int rc = api.getSnapshot(ctx, AJI_NATIVE_PROTOCOL_VERSION, snapshot);
        if (rc != AJI_NATIVE_OK) {
            *why = "aji_native_get_snapshot returned " + std::to_string(rc);
            return false;
        }
        return true;
    }

    std::string lastError() const {
        const char *message = ctx && api.lastError ? api.lastError(ctx) : nullptr;
        return message ? message : std::string();
    }

    bool inferOnce(std::string *why) {
        const int configured =
            api.configure(ctx, static_cast<int>(plan.stages[0].input_width),
                          static_cast<int>(plan.stages[0].input_height),
                          static_cast<double>(plan.source_fps_num) /
                              static_cast<double>(plan.source_fps_den),
                          nullptr, nullptr);
        if (configured != 1) {
            *why = "aji_configure returned " + std::to_string(configured) + ": " + lastError();
            return false;
        }
        aji_frame in{}, out{};
        in.width = static_cast<int>(plan.stages[0].input_width);
        in.height = static_cast<int>(plan.stages[0].input_height);
        in.format = AJI_FMT_YUV444P16;
        in.matrix = matrix;
        in.range = range;
        in.siting = AJI_SITING_LEFT;
        in.plane[0] = inY.asVoid();
        in.plane[1] = inU.asVoid();
        in.plane[2] = inV.asVoid();
        in.stride[0] = in.stride[1] = in.stride[2] =
            static_cast<ptrdiff_t>(plan.stages[0].input_width) * 2;
        out.width = static_cast<int>(plan.stages[0].output_width);
        out.height = static_cast<int>(plan.stages[0].output_height);
        out.format = AJI_FMT_YUV444P16;
        out.matrix = matrix;
        out.range = range;
        out.siting = AJI_SITING_LEFT;
        out.plane[0] = outY.asVoid();
        out.plane[1] = outU.asVoid();
        out.plane[2] = outV.asVoid();
        out.stride[0] = out.stride[1] = out.stride[2] =
            static_cast<ptrdiff_t>(plan.stages[0].output_width) * 2;
        const int rc = api.infer(ctx, &in, &out, stream);
        if (rc != AJI_OK) {
            *why = "aji_infer returned " + std::to_string(rc) + ": " + lastError();
            return false;
        }
        const uint64_t ticket = api.flush(ctx, stream);
        if (ticket == 0) {
            *why = "aji_flush returned no ticket: " + lastError();
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kControlTimeoutMs);
        for (;;) {
            const int done = api.done(ctx, ticket);
            if (done == 1) break;
            if (done < 0) {
                *why = "aji_done reported an error: " + lastError();
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                if (api.wait(ctx, ticket) != AJI_OK) {
                    *why = "aji_wait failed: " + lastError();
                    return false;
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    bool observeActive(const std::string &name, std::string *why) {
        aji_native_snapshot_v2 snapshot{};
        if (!takeSnapshot(&snapshot, why)) return false;
        if (snapshot.state != AJI_NATIVE_STATE_ACTIVE) {
            *why = "state is " + std::to_string(snapshot.state) + ", not Active";
            return false;
        }
        if (snapshot.completed_ticket != snapshot.last_submitted_ticket) {
            *why = "completed ticket does not equal the last submitted ticket";
            return false;
        }
        check(snapshot.error[0] == '\0', name + ".error", "Active snapshot carries an empty reason");
        return true;
    }

    bool control(uint32_t action, unsigned long long epoch, std::string *why) {
        aji_native_snapshot_v2 snapshot{};
        std::memset(&snapshot, 0, sizeof(snapshot));
        snapshot.struct_size = sizeof(snapshot);
        const int rc = api.controlOnStream(ctx, AJI_NATIVE_PROTOCOL_VERSION, action, epoch,
                                           kControlTimeoutMs, stream, &snapshot);
        if (rc != AJI_NATIVE_OK) {
            *why = "control " + std::to_string(action) + " returned " + std::to_string(rc) +
                   ": " + std::string(snapshot.error);
            return false;
        }
        return true;
    }

    bool apply(const aji_native_prepared_plan_v1 &candidate, aji_native_snapshot_v2 *snapshot,
               std::string *why) {
        std::memset(snapshot, 0, sizeof(*snapshot));
        snapshot->struct_size = sizeof(*snapshot);
        const int rc = api.applyPrepared(ctx, &candidate, snapshot);
        if (rc != AJI_NATIVE_OK) {
            *why = "aji_native_apply_prepared returned " + std::to_string(rc) + ": " +
                   std::string(snapshot->error);
            return false;
        }
        return true;
    }

    bool expectRefusal(const std::string &name, const aji_native_prepared_plan_v1 &candidate,
                       const std::string &expected, std::string *why) {
        aji_native_snapshot_v2 snapshot{};
        std::string detail;
        if (apply(candidate, &snapshot, &detail)) {
            check(false, name, "a refused plan was accepted");
            return false;
        }
        if (snapshot.state != AJI_NATIVE_STATE_UNAVAILABLE) {
            check(false, name, "refusal did not report Unavailable");
            return false;
        }
        if (!containsNoCase(snapshot.error, expected)) {
            check(false, name, "refusal reason '" + std::string(snapshot.error) +
                                   "' does not name '" + expected + "'");
            return false;
        }
        aji_native_snapshot_v2 retained{};
        if (!takeSnapshot(&retained, why)) return false;
        if (retained.state != AJI_NATIVE_STATE_ACTIVE ||
            std::string(retained.effective_plan) != plan.requested_plan) {
            check(false, name, "the refused replacement did not retain the prior active plan");
            return false;
        }
        check(true, name, "refused by name: " + std::string(snapshot.error));
        return true;
    }
};

// ---------------------------------------------------------------------------
// measurement
// ---------------------------------------------------------------------------

bool loadReferenceTensors(Probe &probe, std::string *why) {
    auto readTensor = [&](const RefTensor &tensor, std::vector<uint16_t> *out) {
        std::vector<unsigned char> bytes;
        const std::filesystem::path path = probe.reference.dir / std::filesystem::path(widen(tensor.file));
        if (!readFileBytes(path, &bytes, why)) return false;
        if (bytes.size() != tensor.bytes) {
            *why = "reference tensor " + tensor.name + " has " +
                   std::to_string(bytes.size()) + " bytes, expected " +
                   std::to_string(tensor.bytes);
            return false;
        }
        out->resize(bytes.size() / 2);
        std::memcpy(out->data(), bytes.data(), bytes.size());
        return true;
    };
    std::vector<uint16_t> input;
    if (!readTensor(probe.reference.input, &input)) return false;
    if (!readTensor(probe.reference.output, &probe.hostRefOut)) return false;
    const size_t channels = static_cast<size_t>(probe.reference.input.shape[1]);
    const size_t height = static_cast<size_t>(probe.reference.input.shape[2]);
    const size_t width = static_cast<size_t>(probe.reference.input.shape[3]);
    if (channels != 3) {
        *why = "the reference input is not a 3-channel image; the 4:4:4 measurement needs RGB";
        return false;
    }
    probe.hostRefRgb.assign(channels * height * width, 0.0f);
    for (size_t i = 0; i < probe.hostRefRgb.size(); ++i)
        probe.hostRefRgb[i] = halfToFloat(input[i]);
    return true;
}

bool encodeInputFrame(Probe &probe, std::string *why) {
    const size_t width = static_cast<size_t>(probe.reference.input.shape[3]);
    const size_t height = static_cast<size_t>(probe.reference.input.shape[2]);
    const size_t plane = width * height;
    probe.hostInY.resize(plane);
    probe.hostInU.resize(plane);
    probe.hostInV.resize(plane);
    unsigned long long clipped = 0;
    for (size_t i = 0; i < plane; ++i) {
        const double r = probe.hostRefRgb[i];
        const double g = probe.hostRefRgb[plane + i];
        const double b = probe.hostRefRgb[2 * plane + i];
        double y = 0.0, u = 0.0, v = 0.0;
        rgbToYuv444(r, g, b, probe.csp, &y, &u, &v);
        const double rawY = y * probe.csp.yscale + probe.csp.yoff;
        const double rawU = u * probe.csp.cscale + probe.csp.coff;
        const double rawV = v * probe.csp.cscale + probe.csp.coff;
        if (rawY < 0.0 || rawY > 65535.0 || rawU < 0.0 || rawU > 65535.0 ||
            rawV < 0.0 || rawV > 65535.0)
            ++clipped;
        probe.hostInY[i] = quantize16(rawY);
        probe.hostInU[i] = quantize16(rawU);
        probe.hostInV[i] = quantize16(rawV);
    }
    probe.check(clipped == 0, "measurement.inputContainer",
                "the reference input fits the 4:4:4 container (" + std::to_string(clipped) +
                    " clipped)");
    const size_t bytes = plane * 2;
    if (probe.cuda.memcpyHtoD(probe.inY.pointer, probe.hostInY.data(), bytes) != kCudaSuccess ||
        probe.cuda.memcpyHtoD(probe.inU.pointer, probe.hostInU.data(), bytes) != kCudaSuccess ||
        probe.cuda.memcpyHtoD(probe.inV.pointer, probe.hostInV.data(), bytes) != kCudaSuccess) {
        *why = "uploading the static input frame failed";
        return false;
    }
    return true;
}

bool measureOutput(Probe &probe, std::string *why) {
    const size_t width = static_cast<size_t>(probe.reference.output.shape[3]);
    const size_t height = static_cast<size_t>(probe.reference.output.shape[2]);
    const size_t plane = width * height;
    const size_t bytes = plane * 2;
    probe.hostOutY.resize(plane);
    probe.hostOutU.resize(plane);
    probe.hostOutV.resize(plane);
    if (probe.cuda.streamSynchronize(probe.stream) != kCudaSuccess) {
        *why = "stream synchronize before the output readback failed";
        return false;
    }
    if (probe.cuda.memcpyDtoH(probe.hostOutY.data(), probe.outY.pointer, bytes) != kCudaSuccess ||
        probe.cuda.memcpyDtoH(probe.hostOutU.data(), probe.outU.pointer, bytes) != kCudaSuccess ||
        probe.cuda.memcpyDtoH(probe.hostOutV.data(), probe.outV.pointer, bytes) != kCudaSuccess) {
        *why = "reading the completed output frame failed";
        return false;
    }

    // got: the completed frame projected back through the same 4:4:4 matrix.
    std::vector<float> got(3 * plane);
    for (size_t i = 0; i < plane; ++i) {
        const double y = (static_cast<double>(probe.hostOutY[i]) - probe.csp.yoff) / probe.csp.yscale;
        const double u = (static_cast<double>(probe.hostOutU[i]) - probe.csp.coff) / probe.csp.cscale;
        const double v = (static_cast<double>(probe.hostOutV[i]) - probe.csp.coff) / probe.csp.cscale;
        double r = 0.0, g = 0.0, b = 0.0;
        yuv444ToRgb(y, u, v, probe.csp, &r, &g, &b);
        got[i] = static_cast<float>(r);
        got[plane + i] = static_cast<float>(g);
        got[2 * plane + i] = static_cast<float>(b);
    }
    // expected: the manifest's CPU output projected through the same documented
    // 4:4:4 matrix, so both sides pass through the identical container.
    std::vector<float> expected(3 * plane);
    std::vector<unsigned char> clipped(plane, 0);
    for (size_t i = 0; i < plane; ++i) {
        const double r = halfToFloat(probe.hostRefOut[i]);
        const double g = halfToFloat(probe.hostRefOut[plane + i]);
        const double b = halfToFloat(probe.hostRefOut[2 * plane + i]);
        double y = 0.0, u = 0.0, v = 0.0;
        rgbToYuv444(r, g, b, probe.csp, &y, &u, &v);
        const double rawY = y * probe.csp.yscale + probe.csp.yoff;
        const double rawU = u * probe.csp.cscale + probe.csp.coff;
        const double rawV = v * probe.csp.cscale + probe.csp.coff;
        if (rawY < 0.0 || rawY > 65535.0 || rawU < 0.0 || rawU > 65535.0 ||
            rawV < 0.0 || rawV > 65535.0)
            clipped[i] = 1;
        const double dy = (static_cast<double>(quantize16(rawY)) - probe.csp.yoff) / probe.csp.yscale;
        const double du = (static_cast<double>(quantize16(rawU)) - probe.csp.coff) / probe.csp.cscale;
        const double dv = (static_cast<double>(quantize16(rawV)) - probe.csp.coff) / probe.csp.cscale;
        double pr = 0.0, pg = 0.0, pb = 0.0;
        yuv444ToRgb(dy, du, dv, probe.csp, &pr, &pg, &pb);
        expected[i] = static_cast<float>(pr);
        expected[plane + i] = static_cast<float>(pg);
        expected[2 * plane + i] = static_cast<float>(pb);
    }

    Measurement &m = probe.measurement;
    m.measured = true;
    m.elements = 3 * plane;
    double squared = 0.0, unclippedSquared = 0.0;
    for (size_t i = 0; i < m.elements; ++i) {
        const size_t pixel = i % plane;
        const size_t channel = i / plane;
        const double gotValue = got[i];
        const double expectedValue = expected[i];
        const double difference = std::fabs(gotValue - expectedValue);
        squared += difference * difference;
        if (difference > m.maxAbsoluteError) {
            m.maxAbsoluteError = difference;
            m.worstIndex = i;
            m.worstChannel = static_cast<int>(channel);
            m.worstY = static_cast<int>(pixel / width);
            m.worstX = static_cast<int>(pixel % width);
        }
        if (difference > kFp16MaxAbsoluteError) {
            ++m.elementsOutsideTolerance;
            const int x = static_cast<int>(pixel % width);
            const int y = static_cast<int>(pixel / width);
            if (x == 0 || y == 0 || x == static_cast<int>(width) - 1 ||
                y == static_cast<int>(height) - 1)
                ++m.borderElementsOutsideTolerance;
        }
        if (!clipped[pixel]) {
            ++m.unclippedElements;
            const double raw = std::fabs(gotValue - halfToFloat(probe.hostRefOut[i]));
            unclippedSquared += raw * raw;
            if (raw > m.unclippedMaxAbsoluteError) m.unclippedMaxAbsoluteError = raw;
        }
    }
    for (size_t i = 0; i < plane; ++i)
        if (clipped[i]) ++m.clippedReferenceElements;
    m.rmse = std::sqrt(squared / static_cast<double>(m.elements));
    m.unclippedRmse = m.unclippedElements
                          ? std::sqrt(unclippedSquared / static_cast<double>(m.unclippedElements))
                          : 0.0;
    m.withinTolerance = m.maxAbsoluteError <= kFp16MaxAbsoluteError && m.rmse <= kFp16MaxRmse;
    m.unclippedWithinTolerance =
        m.unclippedElements == 0 ||
        (m.unclippedMaxAbsoluteError <= kFp16MaxAbsoluteError && m.unclippedRmse <= kFp16MaxRmse);
    return true;
}

// ---------------------------------------------------------------------------
// driver / symbol preflight
// ---------------------------------------------------------------------------

bool openDevice(Probe &probe, std::string *why) {
    if (!probe.cuda.load(why)) return false;
    if (probe.cuda.init(0) != kCudaSuccess) {
        *why = "cuInit failed";
        return false;
    }
    CUuuid requested{};
    if (!parseUuid(probe.opt.deviceUuid, &requested, why)) return false;
    int count = 0;
    if (probe.cuda.deviceGetCount(&count) != kCudaSuccess || count < 1) {
        *why = "no CUDA device is present";
        return false;
    }
    std::string present;
    for (int index = 0; index < count; ++index) {
        CUdevice device = 0;
        if (probe.cuda.deviceGet(&device, index) != kCudaSuccess) continue;
        CUuuid uuid{};
        if (probe.cuda.deviceGetUuid(&uuid, device) != kCudaSuccess) continue;
        if (std::memcmp(&uuid, &requested, sizeof(CUuuid)) != 0) {
            present += "\n  " + uuidHex(uuid);
            continue;
        }
        probe.device = device;
        probe.uuid = uuid;
        probe.uuidHexText = uuidHex(uuid);
        char name[256] = {};
        if (probe.cuda.deviceGetName) probe.cuda.deviceGetName(name, sizeof(name), device);
        probe.deviceName = name;
        probe.cuda.deviceGetAttribute(&probe.smMajor, kAttributeComputeCapabilityMajor, device);
        probe.cuda.deviceGetAttribute(&probe.smMinor, kAttributeComputeCapabilityMinor, device);
        break;
    }
    if (probe.uuidHexText.empty()) {
        *why = "--device-uuid " + probe.opt.deviceUuid + " is not one of this machine's devices:" + present;
        return false;
    }
    if (probe.cuda.primaryCtxRetain(&probe.context, probe.device) != kCudaSuccess) {
        *why = "cuDevicePrimaryCtxRetain failed";
        return false;
    }
    probe.retainedDevice = probe.device;
    if (probe.cuda.ctxGetCurrent(&probe.previousContext) != kCudaSuccess ||
        probe.cuda.ctxSetCurrent(probe.context) != kCudaSuccess) {
        *why = "making the retained primary context current failed";
        return false;
    }
    if (probe.cuda.streamCreate(&probe.stream, 0) != kCudaSuccess) {
        *why = "cuStreamCreate failed";
        return false;
    }
    if (probe.cuda.driverGetVersion) probe.cuda.driverGetVersion(&probe.driverVersion);
    // The device facts the plan carries are the ones this probe just measured.
    return true;
}

bool registerDependencyDirs(Probe &probe, std::string *why) {
    for (const std::filesystem::path &dir : probe.opt.dependencyDirs)
        if (!probe.dllDirs.add(dir, why))
            return false;
    return true;
}

bool openModule(Probe &probe, std::string *why) {
    probe.module = LoadLibraryExW(probe.opt.modulePath.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                      LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                      LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!probe.module) {
        *why = "cannot load " + toUtf8(probe.opt.modulePath) + " (error " +
               std::to_string(GetLastError()) + ")";
        return false;
    }
    auto resolve = [&](const char *name) {
        return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(probe.module), name));
    };
    probe.api.createPrepared = reinterpret_cast<CreatePreparedFn>(resolve("aji_native_create_prepared"));
    probe.api.applyPrepared = reinterpret_cast<ApplyPreparedFn>(resolve("aji_native_apply_prepared"));
    probe.api.controlOnStream = reinterpret_cast<ControlOnStreamFn>(resolve("aji_native_control_on_stream"));
    probe.api.getSnapshot = reinterpret_cast<GetSnapshotFn>(resolve("aji_native_get_snapshot"));
    probe.api.configure = reinterpret_cast<ConfigureFn>(resolve("aji_configure"));
    probe.api.infer = reinterpret_cast<InferFn>(resolve("aji_infer"));
    probe.api.flush = reinterpret_cast<FlushFn>(resolve("aji_flush"));
    probe.api.done = reinterpret_cast<DoneFn>(resolve("aji_done"));
    probe.api.wait = reinterpret_cast<WaitFn>(resolve("aji_wait"));
    probe.api.lastError = reinterpret_cast<LastErrorFn>(resolve("aji_last_error"));
    probe.api.destroy = reinterpret_cast<DestroyFn>(resolve("aji_destroy"));
    probe.api.currentLog = reinterpret_cast<CurrentLogFn>(resolve("aji_current_log"));
    // Optional: only the bounded --query-factory mode requires it.
    probe.api.queryFactory =
        reinterpret_cast<QueryFactoryFn>(resolve("aji_native_query_factory"));
    const char *missing = nullptr;
    if (!probe.api.createPrepared) missing = "aji_native_create_prepared";
    else if (!probe.api.applyPrepared) missing = "aji_native_apply_prepared";
    else if (!probe.api.controlOnStream) missing = "aji_native_control_on_stream";
    else if (!probe.api.getSnapshot) missing = "aji_native_get_snapshot";
    else if (!probe.api.configure) missing = "aji_configure";
    else if (!probe.api.infer) missing = "aji_infer";
    else if (!probe.api.flush) missing = "aji_flush";
    else if (!probe.api.done) missing = "aji_done";
    else if (!probe.api.wait) missing = "aji_wait";
    else if (!probe.api.destroy) missing = "aji_destroy";
    if (missing) {
        *why = std::string("the dispatcher does not export ") + missing;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// factory capability query mode
// ---------------------------------------------------------------------------

// Bounded mode: it drives only aji_native_query_factory against the probe's
// own already-adopted compute context. This is a DIRECT-COMPUTE factory proof
// (a real context, real device, real runtime factory); it is NOT a decoded
// source, an N1 chain, a prepared plan or an artifact/readiness attestation.
int runFactoryQuery(Probe &probe) {
    const QueryFactoryFn query = probe.api.queryFactory;
    if (!query) {
        const std::string why =
            "the dispatcher does not export aji_native_query_factory";
        std::fprintf(stderr, "%s: symbols: %s\n", kProbeName, why.c_str());
        std::printf("{\"probe\":%s,\"ok\":false,\"mode\":\"query-factory\","
                    "\"phase\":\"symbols\",\"error\":%s}\n",
                    jsonQuote(kProbeName).c_str(), jsonQuote(why).c_str());
        return kSymbols;
    }

    aji_create_params params{};
    params.api_version = AJI_API_VERSION;
    params.cuda_context = probe.context;

    aji_native_factory_capabilities_v1 caps{};

    // 1. a mismatched declared size is refused before any backend is touched.
    caps = aji_native_factory_capabilities_v1{};
    caps.struct_size = sizeof(caps) - 1;
    caps.requested_protocol = AJI_NATIVE_FACTORY_QUERY_PROTOCOL_VERSION;
    const int wrongSize = query(&params, &caps);
    probe.check(wrongSize == AJI_NATIVE_INVALID_ARGUMENT, "query.wrong_size",
                "a mismatched struct_size is refused as an invalid argument");

    // 2. a mismatched query protocol is refused by name.
    caps = aji_native_factory_capabilities_v1{};
    caps.struct_size = sizeof(caps);
    caps.requested_protocol = AJI_NATIVE_FACTORY_QUERY_PROTOCOL_VERSION + 1u;
    const int wrongProtocol = query(&params, &caps);
    probe.check(wrongProtocol == AJI_NATIVE_PROTOCOL_MISMATCH, "query.wrong_protocol",
                "a mismatched query protocol is refused as a protocol mismatch");

    // 3. absent create parameters are refused.
    caps = aji_native_factory_capabilities_v1{};
    caps.struct_size = sizeof(caps);
    caps.requested_protocol = AJI_NATIVE_FACTORY_QUERY_PROTOCOL_VERSION;
    const int noParams = query(nullptr, &caps);
    probe.check(noParams == AJI_NATIVE_INVALID_ARGUMENT, "query.null_params",
                "an absent create-parameter pointer is refused");

    // 4. a null context is refused: this query never creates a context.
    caps = aji_native_factory_capabilities_v1{};
    caps.struct_size = sizeof(caps);
    caps.requested_protocol = AJI_NATIVE_FACTORY_QUERY_PROTOCOL_VERSION;
    aji_create_params nullParams{};
    nullParams.api_version = AJI_API_VERSION;
    nullParams.cuda_context = nullptr;
    const int noContext = query(&nullParams, &caps);
    probe.check(noContext == AJI_NATIVE_NO_CONTEXT, "query.null_context",
                "a null adopted context is refused with a named reason");
    probe.check(caps.role_bits == 0 && caps.backend == AJI_NATIVE_FACTORY_BACKEND_NONE,
                "query.null_context.no_claims",
                "a refused query claims neither a backend nor roles");
    probe.check(caps.reason_key[0] != '\0' && caps.reason[0] != '\0',
                "query.null_context.reason", "the refusal is named");

    // 5. the real query on the actual adopted context.
    caps = aji_native_factory_capabilities_v1{};
    caps.struct_size = sizeof(caps);
    caps.requested_protocol = AJI_NATIVE_FACTORY_QUERY_PROTOCOL_VERSION;
    const int rc = query(&params, &caps);
    probe.check(rc == AJI_NATIVE_OK && caps.result == AJI_NATIVE_OK, "query.ok",
                "the adopted-context factory query succeeded");
    probe.check(caps.struct_size == sizeof(caps), "query.struct_size",
                "the reply's struct_size matches this producer's layout");
    probe.check(caps.protocol_version == AJI_NATIVE_FACTORY_QUERY_PROTOCOL_VERSION &&
                    caps.requested_protocol == AJI_NATIVE_FACTORY_QUERY_PROTOCOL_VERSION,
                "query.protocol", "the query announces its own protocol version");
    probe.check(caps.native_protocol_version == AJI_NATIVE_PROTOCOL_VERSION &&
                    caps.native_schema_version == AJI_NATIVE_SCHEMA_VERSION,
                "query.native_versions", "native protocol/schema are v2");
    probe.check(caps.source_abi == AJI_API_VERSION, "query.source_abi",
                "source ABI is the loaded " + std::to_string(AJI_API_VERSION));
    probe.check(caps.backend == AJI_NATIVE_FACTORY_BACKEND_TENSORRT, "query.backend",
                "the observed backend is TensorRT");
    probe.check((caps.role_bits & AJI_NATIVE_FACTORY_ROLE_SPATIAL) != 0 &&
                    (caps.role_bits & AJI_NATIVE_FACTORY_ROLE_RIFE) != 0,
                "query.roles", "spatial fp16 and 11-channel RIFE roles are reported");
    probe.check(caps.runtime_version[0] != '\0', "query.runtime",
                "runtime version is the actually loaded token: " +
                    std::string(caps.runtime_version));
    probe.check(std::strstr(caps.device_identity, probe.uuidHexText.c_str()) != nullptr,
                "query.device_identity", "device identity is the actually adopted CUDA UUID");
    probe.check(static_cast<int>(caps.sm_major) == probe.smMajor &&
                    static_cast<int>(caps.sm_minor) == probe.smMinor,
                "query.compute_capability", "the reported SM matches the measured device");
    probe.check(std::string(caps.provider) == "TensorRT", "query.provider",
                "provider names the actually loaded runtime factory");
    probe.check(rc == AJI_NATIVE_OK && caps.role_bits != 0 && caps.provider[0] != '\0' &&
                    caps.runtime_version[0] != '\0' && caps.device_identity[0] != '\0',
                "query.no_fake_readiness",
                "no all-zero provenance: claims hold only with a real observation");

    std::ostringstream out;
    out << "{\"probe\":" << jsonQuote(kProbeName)
        << ",\"ok\":" << (probe.failures.empty() ? "true" : "false")
        << ",\"mode\":\"query-factory\""
        << ",\"proof\":\"DIRECTCOMPUTE-factory-only-not-decoded-source-N1\""
        << ",\"device\":{\"uuid\":" << jsonQuote(probe.uuidHexText)
        << ",\"name\":" << jsonQuote(probe.deviceName)
        << ",\"smMajor\":" << probe.smMajor
        << ",\"smMinor\":" << probe.smMinor
        << ",\"driverVersion\":" << probe.driverVersion << "}"
        << ",\"factory\":{\"queryProtocol\":" << caps.protocol_version
        << ",\"nativeProtocol\":" << caps.native_protocol_version
        << ",\"nativeSchema\":" << caps.native_schema_version
        << ",\"sourceAbi\":" << caps.source_abi
        << ",\"structSize\":" << caps.struct_size
        << ",\"observed\":" << (rc == AJI_NATIVE_OK ? "true" : "false")
        << ",\"backend\":" << caps.backend
        << ",\"provider\":" << jsonQuote(caps.provider)
        << ",\"spatial\":"
        << ((caps.role_bits & AJI_NATIVE_FACTORY_ROLE_SPATIAL) ? "true" : "false")
        << ",\"rife\":"
        << ((caps.role_bits & AJI_NATIVE_FACTORY_ROLE_RIFE) ? "true" : "false")
        << ",\"runtimeVersion\":" << jsonQuote(caps.runtime_version)
        << ",\"deviceIdentity\":" << jsonQuote(caps.device_identity)
        << ",\"smMajor\":" << caps.sm_major
        << ",\"smMinor\":" << caps.sm_minor
        << ",\"reasonKey\":" << jsonQuote(caps.reason_key)
        << ",\"reason\":" << jsonQuote(caps.reason) << "}"
        << ",\"scenarios\":[";
    for (size_t i = 0; i < probe.scenarios.size(); ++i) {
        if (i) out << ",";
        out << probe.scenarios[i];
    }
    out << "],\"failures\":[";
    for (size_t i = 0; i < probe.failures.size(); ++i) {
        if (i) out << ",";
        out << jsonQuote(probe.failures[i]);
    }
    out << "]}";

    if (probe.stream) probe.cuda.streamDestroy(probe.stream);
    if (probe.context) {
        probe.cuda.ctxSetCurrent(probe.previousContext);
        probe.cuda.primaryCtxRelease(probe.retainedDevice);
    }
    if (probe.module) FreeLibrary(static_cast<HMODULE>(probe.module));

    std::printf("%s\n", out.str().c_str());
    std::fflush(stdout);
    if (!probe.failures.empty()) {
        std::fprintf(stderr, "%s: %zu scenario(s) failed\n", kProbeName,
                     probe.failures.size());
        return kLifecycle;
    }
    return kOk;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int runProbe(int argc, char **argv) {
    Probe probe;
    std::string why;
    if (!parseOptions(argc, argv, &probe.opt, &why)) {
        std::fprintf(stderr, "%s: %s\n", kProbeName, why.c_str());
        std::printf("{\"probe\":%s,\"ok\":false,\"phase\":\"usage\",\"error\":%s}\n",
                    jsonQuote(kProbeName).c_str(), jsonQuote(why).c_str());
        return kUsage;
    }
    if (probe.opt.help) {
        usage();
        return kOk;
    }
    auto fatal = [&](const char *phase, const std::string &message, int code) {
        std::fprintf(stderr, "%s: %s: %s\n", kProbeName, phase, message.c_str());
        std::printf("{\"probe\":%s,\"ok\":false,\"phase\":%s,\"error\":%s}\n",
                    jsonQuote(kProbeName).c_str(), jsonQuote(phase).c_str(),
                    jsonQuote(message).c_str());
        return code;
    };

    {
        std::error_code error;
        std::filesystem::create_directories(probe.opt.workDir, error);
        if (error) return fatal("workdir", "cannot create " + toUtf8(probe.opt.workDir) +
                                                ": " + error.message(), kPreflight);
    }

    if (!openDevice(probe, &why)) return fatal("device", why, kDevice);
    if (!registerDependencyDirs(probe, &why)) return fatal("dependencies", why, kPreflight);
    if (!openModule(probe, &why)) return fatal("symbols", why, kSymbols);

    if (probe.opt.queryFactory)
        return runFactoryQuery(probe);

    // Preflight: re-hash the owned artifacts and enforce the caller's digests
    // before anything is deserialized.
    std::string engineActual, modelActual;
    if (!sha256File(probe.opt.enginePath, &engineActual, &why))
        return fatal("digest", why, kPreflight);
    if (!sha256File(probe.opt.modelPath, &modelActual, &why))
        return fatal("digest", why, kPreflight);
    if (engineActual != probe.opt.engineSha)
        return fatal("digest", "engine digest mismatch: expected " + probe.opt.engineSha +
                                   ", actual " + engineActual, kPreflight);
    if (modelActual != probe.opt.modelSha)
        return fatal("digest", "model digest mismatch: expected " + probe.opt.modelSha +
                                   ", actual " + modelActual, kPreflight);

    // The reference manifest supplies the static input and the spatial stage
    // geometry, and its own graph digest must be the plan's model digest.
    if (!loadReference(probe.opt.referencePath, &probe.reference, &why))
        return fatal("reference", why, kPreflight);
    if (probe.reference.graphSha != probe.opt.modelSha)
        return fatal("reference",
                     "the reference graphSha256 does not equal the plan model digest: " +
                         probe.reference.graphSha + " vs " + probe.opt.modelSha,
                     kPreflight);

    if (!probe.opt.admissionPath.empty()) {
        if (!admissionToPlan(probe.opt.admissionPath, probe.uuidHexText, &probe.plan, &why))
            return fatal("admission", why, kAdmission);
        // The plan's own digests must equal the files we will feed the runtime.
        if (std::string(probe.plan.stages[0].engine_sha256) != engineActual)
            return fatal("admission", "the admission engine digest does not match the engine file",
                         kAdmission);
        if (std::string(probe.plan.stages[0].model_sha256) != modelActual)
            return fatal("admission", "the admission model digest does not match the model file",
                         kAdmission);
    } else {
        PlanFacts facts;
        facts.requestedPlan = probe.opt.requestedPlan.empty() ? "probe:managed-prepared-v1"
                                                              : probe.opt.requestedPlan;
        facts.recipeId = probe.opt.recipeId.empty() ? "probe.recipe" : probe.opt.recipeId;
        facts.deviceIdentity = "CUDA-UUID:" + probe.uuidHexText;
        facts.decoderUuid = probe.opt.decoderUuid;
        facts.computeUuid = "CUDA-UUID:" + probe.uuidHexText;
        facts.adapterLuid = probe.opt.adapterLuid;
        facts.smMajor = static_cast<uint32_t>(probe.smMajor);
        facts.smMinor = static_cast<uint32_t>(probe.smMinor);
        facts.fpsNum = static_cast<uint32_t>(probe.opt.sourceFpsNum);
        facts.fpsDen = static_cast<uint32_t>(probe.opt.sourceFpsDen);
        facts.bitDepth = static_cast<uint32_t>(probe.opt.sourceBitDepth);
        facts.csp = probe.opt.sourceCsp;
        facts.matrix = probe.opt.sourceMatrix;
        facts.range = probe.opt.sourceRange;
        facts.siting = probe.opt.sourceSiting;
        facts.buildRuntime = probe.opt.buildRuntime;
        facts.buildPlugin = probe.opt.buildPlugin;
        facts.buildCoreSha = probe.opt.buildCoreSha;
        facts.buildSourceAbi = static_cast<uint32_t>(probe.opt.buildSourceAbi);
        facts.buildPrecision = probe.opt.buildPrecision;
        facts.buildTf32 = static_cast<uint32_t>(probe.opt.buildTf32);
        facts.buildStronglyTyped = static_cast<uint32_t>(probe.opt.buildStronglyTyped);
        facts.buildSkipInference = static_cast<uint32_t>(probe.opt.buildSkipInference);
        facts.buildOptions = probe.opt.buildOptions;
        facts.revision = probe.opt.revision;
        facts.epoch = probe.opt.epoch;
        if (!buildExplicitPlan(probe.opt, probe.reference, facts, &probe.plan, &why))
            return fatal("plan", why, kPreflight);
    }

    if (probe.plan.stage_count != 1 || probe.plan.stages[0].role != AJI_NATIVE_ROLE_SPATIAL) {
        return fatal("plan",
                     "this direct probe drives exactly one spatial stage; the admitted plan has " +
                         std::to_string(probe.plan.stage_count) + " stage(s)",
                     kPreflight);
    }
    // The static input frame and the output readback are sized from the plan,
    // so the admitted stage geometry must be exactly the reference geometry.
    if (static_cast<long long>(probe.plan.stages[0].input_channels) != probe.reference.input.shape[1] ||
        static_cast<long long>(probe.plan.stages[0].input_height) != probe.reference.input.shape[2] ||
        static_cast<long long>(probe.plan.stages[0].input_width) != probe.reference.input.shape[3] ||
        static_cast<long long>(probe.plan.stages[0].output_channels) != probe.reference.output.shape[1] ||
        static_cast<long long>(probe.plan.stages[0].output_height) != probe.reference.output.shape[2] ||
        static_cast<long long>(probe.plan.stages[0].output_width) != probe.reference.output.shape[3])
        return fatal("reference",
                     "the admitted stage geometry does not match the reference manifest", kPreflight);

    probe.matrix = matrixFromName(probe.plan.source_matrix);
    probe.range = rangeFromName(probe.plan.source_range);
    probe.csp = makeCsp444(probe.matrix, probe.range);

    // Frame buffers (the spatial stage geometry drives their sizes).
    const size_t inPlane = static_cast<size_t>(probe.plan.stages[0].input_width) *
                           static_cast<size_t>(probe.plan.stages[0].input_height);
    const size_t outPlane = static_cast<size_t>(probe.plan.stages[0].output_width) *
                            static_cast<size_t>(probe.plan.stages[0].output_height);
    if (!probe.inY.allocate(probe.cuda, inPlane * 2, "input Y", &why) ||
        !probe.inU.allocate(probe.cuda, inPlane * 2, "input Cb", &why) ||
        !probe.inV.allocate(probe.cuda, inPlane * 2, "input Cr", &why) ||
        !probe.outY.allocate(probe.cuda, outPlane * 2, "output Y", &why) ||
        !probe.outU.allocate(probe.cuda, outPlane * 2, "output Cb", &why) ||
        !probe.outV.allocate(probe.cuda, outPlane * 2, "output Cr", &why))
        return fatal("device", why, kDevice);

    if (!loadReferenceTensors(probe, &why)) return fatal("reference", why, kPreflight);
    if (!encodeInputFrame(probe, &why)) return fatal("measurement", why, kPreflight);

    // ---- create_prepared: Prepared is the actually loaded plan --------------
    aji_create_params params{};
    params.api_version = AJI_API_VERSION;
    params.cuda_context = probe.context;
    aji_native_snapshot_v2 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    probe.ctx = probe.api.createPrepared(&params, &probe.plan, &snapshot);
    if (!probe.ctx) {
        return fatal("create_prepared",
                     "refused: " + std::string(snapshot.error), kAdmission);
    }
    probe.check(snapshot.result == AJI_NATIVE_OK, "create_prepared.result", "result is OK");
    probe.check(snapshot.state == AJI_NATIVE_STATE_PREPARED, "create_prepared.state",
                "state is Prepared before any output");
    probe.check(std::string(snapshot.effective_plan) == probe.plan.requested_plan,
                "create_prepared.effective_plan", "effective plan echoes the requested plan");
    probe.check(snapshot.output_width == probe.plan.stages[0].output_width &&
                    snapshot.output_height == probe.plan.stages[0].output_height,
                "create_prepared.output", "effective output geometry is the loaded plan's");
    probe.check(snapshot.stage_count == probe.plan.stage_count, "create_prepared.stages",
                "stage count is the loaded plan's");
    probe.check((snapshot.capability_bits & AJI_NATIVE_CAP_PREPARED_ONLY) != 0,
                "create_prepared.capability", "PREPARED_ONLY capability is set");
    probe.check(std::string(snapshot.engine_sha256[0]) == probe.plan.stages[0].engine_sha256 &&
                    std::string(snapshot.model_sha256[0]) == probe.plan.stages[0].model_sha256,
                "create_prepared.digests", "effective engine/model digests match the plan");
    probe.check(std::string(snapshot.prepared_build_keys[0]) == probe.plan.stages[0].build_key,
                "create_prepared.build_key", "effective build key matches the plan");
    probe.check(snapshot.error[0] == '\0', "create_prepared.error", "no refusal reason");

    // ---- configure + one completed frame -> Active --------------------------
    {
        std::string inferWhy;
        if (!probe.inferOnce(&inferWhy))
            return fatal("inference", inferWhy, kLifecycle);
        std::string activeWhy;
        if (!probe.observeActive("active", &activeWhy))
            return fatal("active", activeWhy, kLifecycle);
        probe.check(true, "active.state", "state is Active only after a completed output");

        if (probe.opt.measure) {
            if (!measureOutput(probe, &why)) return fatal("measurement", why, kNumerics);
            const Measurement &m = probe.measurement;
            probe.check(m.withinTolerance, "measurement.bounds",
                        "completed output within fp16 bounds (maxAbs " +
                            jsonNumber(m.maxAbsoluteError) + ", rmse " + jsonNumber(m.rmse) + ")");
            probe.check(m.unclippedWithinTolerance, "measurement.unclipped",
                        "unclipped elements within fp16 bounds (maxAbs " +
                            jsonNumber(m.unclippedMaxAbsoluteError) + ", rmse " +
                            jsonNumber(m.unclippedRmse) + ")");
        }
    }

    // ---- lifecycle scenarios ------------------------------------------------
    if (!probe.opt.skipRefusals) {
        // ordered drain on the real inference stream, with an advanced caller
        // epoch that must not reload or re-admit the plan.
        {
            aji_native_snapshot_v2 before{};
            if (!probe.takeSnapshot(&before, &why)) return fatal("drain", why, kLifecycle);
            std::string drainWhy;
            const bool drained = probe.control(AJI_NATIVE_CONTROL_DRAIN, probe.plan.epoch + 1, &drainWhy);
            aji_native_snapshot_v2 after{};
            if (!probe.takeSnapshot(&after, &why)) return fatal("drain", why, kLifecycle);
            probe.check(drained, "drain.ok", drainWhy.empty() ? "ordered drain acknowledged" : drainWhy);
            probe.check(after.state == AJI_NATIVE_STATE_ACTIVE, "drain.state",
                        "drain keeps the plan Active without a reload");
            probe.check(after.epoch == probe.plan.epoch, "drain.epoch",
                        "a drain epoch argument does not overwrite the effective epoch");
            probe.check(after.revision == before.revision, "drain.revision",
                        "drain does not re-admit or reload the plan");
        }

        // epoch advance without a build: re-admission must revalidate and must
        // not inherit the previous epoch's completion.
        {
            aji_native_prepared_plan_v1 advanced = probe.plan;
            advanced.epoch = probe.plan.epoch + 1;
            aji_native_snapshot_v2 applied{};
            std::string applyWhy;
            const bool ok = probe.apply(advanced, &applied, &applyWhy);
            probe.check(ok, "epoch.apply", ok ? "same plan re-admitted at a new epoch" : applyWhy);
            if (ok) {
                probe.check(applied.state == AJI_NATIVE_STATE_PREPARED, "epoch.state",
                            "a new epoch does not inherit the previous completion");
                std::string inferWhy;
                const bool inferred = probe.inferOnce(&inferWhy);
                probe.check(inferred, "epoch.reinfer", inferred ? "completed under the new epoch" : inferWhy);
                if (inferred) {
                    std::string activeWhy;
                    probe.check(probe.observeActive("epoch.active", &activeWhy), "epoch.active",
                                activeWhy.empty() ? "Active under the new epoch" : activeWhy);
                }
            }
        }

        // transactional refused replacement: every mutation is refused by name
        // and the prior active plan is retained.
        struct Negative {
            const char *name;
            const char *expected;
            void (*mutate)(aji_native_prepared_plan_v1 *);
        };
        const Negative negatives[] = {
            {"descriptor.struct_size", "struct_size",
             [](aji_native_prepared_plan_v1 *p) { p->struct_size -= 1; }},
            {"descriptor.protocol", "requested_protocol",
             [](aji_native_prepared_plan_v1 *p) { p->requested_protocol = 2; }},
            {"descriptor.flags", "flags must be exactly",
             [](aji_native_prepared_plan_v1 *p) { p->flags = 0; }},
            {"descriptor.ordinal", "ordinal",
             [](aji_native_prepared_plan_v1 *p) { p->stages[0].ordinal = 1; }},
            {"dtype.descriptor", "dtype disagrees",
             [](aji_native_prepared_plan_v1 *p) { p->stages[0].input_dtype = AJI_NATIVE_DTYPE_FP32; }},
            {"dtype.engine", "input dtype does not match",
             [](aji_native_prepared_plan_v1 *p) {
                 p->build_precision[0] = 0;
                 std::snprintf(p->build_precision, sizeof(p->build_precision), "%s", "fp32");
                 p->stages[0].input_dtype = AJI_NATIVE_DTYPE_FP32;
                 p->stages[0].output_dtype = AJI_NATIVE_DTYPE_FP32;
             }},
            {"engine.digest", "engine digest mismatch",
             [](aji_native_prepared_plan_v1 *p) {
                 std::memset(p->stages[0].engine_sha256, 'f', 64);
                 p->stages[0].engine_sha256[64] = 0;
             }},
            {"model.digest", "model digest mismatch",
             [](aji_native_prepared_plan_v1 *p) {
                 std::memset(p->stages[0].model_sha256, '0', 64);
                 p->stages[0].model_sha256[64] = 0;
             }},
            {"device.identity", "device_identity does not name",
             [](aji_native_prepared_plan_v1 *p) {
                 std::snprintf(p->device_identity, sizeof(p->device_identity), "CUDA-UUID:%s",
                               "00000000000000000000000000000000");
             }},
            {"runtime.version", "build_runtime",
             [](aji_native_prepared_plan_v1 *p) {
                 std::snprintf(p->build_runtime, sizeof(p->build_runtime), "%s", "0.0.0");
             }},
            {"build.provider", "is not TensorRT",
             [](aji_native_prepared_plan_v1 *p) {
                 std::snprintf(p->build_provider, sizeof(p->build_provider), "%s", "DirectML");
             }},
            {"build.options", "not --name[=value]",
             [](aji_native_prepared_plan_v1 *p) {
                 std::snprintf(p->build_options, sizeof(p->build_options), "%s", "nope");
             }},
            {"shape.engine", "shape",
             [](aji_native_prepared_plan_v1 *p) {
                 p->stages[0].output_width -= 2;
             }},
        };
        for (const Negative &negative : negatives) {
            aji_native_prepared_plan_v1 candidate = probe.plan;
            negative.mutate(&candidate);
            std::string refusalWhy;
            probe.expectRefusal(negative.name, candidate, negative.expected, &refusalWhy);
        }
        // A refused admission never deletes or rewrites the owned artifact.
        {
            std::string rehashed;
            std::string hashWhy;
            const bool intact = sha256File(probe.opt.enginePath, &rehashed, &hashWhy) &&
                                rehashed == engineActual;
            probe.check(intact, "refusal.artifact_untouched",
                        intact ? "the engine file is byte-identical after the refusals" : hashWhy);
        }

        // suspend releases; resume revalidates and does not build.
        {
            std::string controlWhy;
            const bool suspended = probe.control(AJI_NATIVE_CONTROL_SUSPEND, probe.plan.epoch, &controlWhy);
            probe.check(suspended, "suspend.ok", suspended ? "suspend acknowledged" : controlWhy);
            aji_native_snapshot_v2 snapshot{};
            if (probe.takeSnapshot(&snapshot, &why)) {
                probe.check(snapshot.state == AJI_NATIVE_STATE_SUSPENDED, "suspend.state",
                            "state is Suspended");
                probe.check(snapshot.output_width == 0 && snapshot.output_height == 0,
                            "suspend.released", "device resources are released (no effective output)");
            }
            aji_frame in{}, out{};
            const int inferRc = probe.api.infer(probe.ctx, &in, &out, probe.stream);
            probe.check(inferRc != AJI_OK, "suspend.infer", "aji_infer is refused while suspended");

            const bool resumed = probe.control(AJI_NATIVE_CONTROL_RESUME, probe.plan.epoch + 2, &controlWhy);
            probe.check(resumed, "resume.ok", resumed ? "resume revalidated and reloaded the plan" : controlWhy);
            aji_native_snapshot_v2 resumedSnapshot{};
            if (probe.takeSnapshot(&resumedSnapshot, &why)) {
                probe.check(resumedSnapshot.state == AJI_NATIVE_STATE_PREPARED, "resume.state",
                            "resume leaves Prepared until an output completes");
                probe.check(std::string(resumedSnapshot.engine_sha256[0]) ==
                                probe.plan.stages[0].engine_sha256,
                            "resume.digests", "resume revalidated the same engine digest");
            }
            std::string inferWhy;
            if (probe.inferOnce(&inferWhy)) {
                std::string activeWhy;
                probe.check(probe.observeActive("resume.active", &activeWhy), "resume.active",
                            activeWhy.empty() ? "Active after a completed resume output" : activeWhy);
            } else {
                probe.check(false, "resume.active", inferWhy);
            }
        }

        // resume refusal with a missing artifact, on probe-owned copies so the
        // owned original is never touched.
        {
            std::filesystem::path copyEngine = probe.opt.workDir / "engine_copy.engine";
            std::filesystem::path copyModel = probe.opt.workDir / "model_copy.onnx";
            std::error_code error;
            std::filesystem::copy_file(probe.opt.enginePath, copyEngine,
                                       std::filesystem::copy_options::overwrite_existing, error);
            std::filesystem::copy_file(probe.opt.modelPath, copyModel,
                                       std::filesystem::copy_options::overwrite_existing, error);
            probe.copyPlan = probe.plan;
            std::snprintf(probe.copyPlan.stages[0].engine_path,
                          sizeof(probe.copyPlan.stages[0].engine_path), "%s",
                          toUtf8(copyEngine).c_str());
            std::snprintf(probe.copyPlan.stages[0].model_path,
                          sizeof(probe.copyPlan.stages[0].model_path), "%s",
                          toUtf8(copyModel).c_str());
            aji_native_snapshot_v2 applied{};
            std::string applyWhy;
            if (probe.apply(probe.copyPlan, &applied, &applyWhy)) {
                probe.check(true, "artifact.copy", "the byte-identical copy plan was admitted");
                std::string controlWhy;
                const bool copySuspended =
                    probe.control(AJI_NATIVE_CONTROL_SUSPEND, probe.copyPlan.epoch, &controlWhy);
                probe.check(copySuspended, "artifact.suspend",
                            copySuspended ? "copy plan suspended" : controlWhy);
                { std::ofstream truncate(copyEngine, std::ios::binary | std::ios::trunc); }
                const bool resumed = probe.control(AJI_NATIVE_CONTROL_RESUME,
                                                   probe.copyPlan.epoch + 1, &controlWhy);
                probe.check(!resumed && containsNoCase(controlWhy, "digest"),
                            "artifact.resume_refusal",
                            resumed ? "resume accepted a missing artifact"
                                    : "resume refused the missing artifact: " + controlWhy);
                aji_native_snapshot_v2 retained{};
                if (probe.takeSnapshot(&retained, &why))
                    probe.check(retained.state == AJI_NATIVE_STATE_SUSPENDED, "artifact.retained",
                                "the refused resume keeps the retained plan suspended");
                std::filesystem::copy_file(probe.opt.enginePath, copyEngine,
                                           std::filesystem::copy_options::overwrite_existing, error);
                const bool restored = probe.control(AJI_NATIVE_CONTROL_RESUME,
                                                    probe.copyPlan.epoch + 2, &controlWhy);
                probe.check(restored, "artifact.resume_ok",
                            restored ? "resume revalidated the restored artifact" : controlWhy);
            } else {
                probe.check(false, "artifact.copy", applyWhy);
            }
        }
    }

    // ---- report -------------------------------------------------------------
    std::string activeLog;
    if (probe.ctx && probe.api.currentLog) activeLog = probe.api.currentLog(probe.ctx);
    std::ostringstream out;
    out << "{\"probe\":" << jsonQuote(kProbeName)
        << ",\"ok\":" << (probe.failures.empty() ? "true" : "false")
        << ",\"module\":" << jsonQuote(toUtf8(probe.opt.modulePath))
        << ",\"backend\":" << jsonQuote(probe.plan.backend)
        << ",\"device\":{\"uuid\":" << jsonQuote(probe.uuidHexText)
        << ",\"name\":" << jsonQuote(probe.deviceName)
        << ",\"smMajor\":" << probe.smMajor
        << ",\"smMinor\":" << probe.smMinor
        << ",\"driverVersion\":" << probe.driverVersion
        << ",\"buildRuntime\":" << jsonQuote(probe.plan.build_runtime) << "}";
    out << ",\"artifacts\":{\"engine\":{\"path\":" << jsonQuote(toUtf8(probe.opt.enginePath))
        << ",\"sha256\":" << jsonQuote(engineActual) << "}"
        << ",\"model\":{\"path\":" << jsonQuote(toUtf8(probe.opt.modelPath))
        << ",\"sha256\":" << jsonQuote(modelActual) << "}}";
    if (!probe.opt.referencePath.empty()) {
        out << ",\"reference\":{\"manifest\":" << jsonQuote(toUtf8(probe.opt.referencePath))
            << ",\"graphSha256\":" << jsonQuote(probe.reference.graphSha)
            << ",\"inputSha256\":" << jsonQuote(probe.reference.input.sha)
            << ",\"outputSha256\":" << jsonQuote(probe.reference.output.sha) << "}";
    }
    out << ",\"plan\":{\"requestedPlan\":" << jsonQuote(probe.plan.requested_plan)
        << ",\"recipeId\":" << jsonQuote(probe.plan.recipe_id)
        << ",\"revision\":" << probe.plan.revision
        << ",\"epoch\":" << probe.plan.epoch
        << ",\"stageCount\":" << probe.plan.stage_count
        << ",\"currentLog\":" << jsonQuote(activeLog) << "}";
    const Measurement &m = probe.measurement;
    out << ",\"measurement\":{\"measured\":" << (m.measured ? "true" : "false")
        << ",\"maxAbsoluteError\":" << jsonNumber(m.maxAbsoluteError)
        << ",\"rmse\":" << jsonNumber(m.rmse)
        << ",\"elements\":" << m.elements
        << ",\"elementsOutsideTolerance\":" << m.elementsOutsideTolerance
        << ",\"borderElementsOutsideTolerance\":" << m.borderElementsOutsideTolerance
        << ",\"clippedReferenceElements\":" << m.clippedReferenceElements
        << ",\"worstElementIndex\":" << m.worstIndex
        << ",\"worstElementCoordinate\":[" << m.worstChannel << "," << m.worstY << ","
        << m.worstX << "]"
        << ",\"unclippedMaxAbsoluteError\":" << jsonNumber(m.unclippedMaxAbsoluteError)
        << ",\"unclippedRmse\":" << jsonNumber(m.unclippedRmse)
        << ",\"unclippedElements\":" << m.unclippedElements
        << ",\"withinTolerance\":" << (m.withinTolerance ? "true" : "false")
        << ",\"unclippedWithinTolerance\":" << (m.unclippedWithinTolerance ? "true" : "false")
        << "}";
    out << ",\"scenarios\":[";
    for (size_t i = 0; i < probe.scenarios.size(); ++i) {
        if (i) out << ",";
        out << probe.scenarios[i];
    }
    out << "]";
    out << ",\"failures\":[";
    for (size_t i = 0; i < probe.failures.size(); ++i) {
        if (i) out << ",";
        out << jsonQuote(probe.failures[i]);
    }
    out << "]}";

    if (probe.ctx) {
        aji_ctx *handle = probe.ctx;
        probe.api.destroy(&handle);
        probe.ctx = nullptr;
    }
    if (probe.stream) probe.cuda.streamDestroy(probe.stream);
    if (probe.context) {
        probe.cuda.ctxSetCurrent(probe.previousContext);
        probe.cuda.primaryCtxRelease(probe.retainedDevice);
    }
    if (probe.module) FreeLibrary(static_cast<HMODULE>(probe.module));

    std::printf("%s\n", out.str().c_str());
    std::fflush(stdout);

    if (!probe.failures.empty()) {
        std::fprintf(stderr, "%s: %zu scenario(s) failed\n", kProbeName, probe.failures.size());
        if (m.measured && !m.withinTolerance) return kNumerics;
        return kLifecycle;
    }
    return kOk;
}

} // namespace

int main(int argc, char **argv) {
    return runProbe(argc, argv);
}
