// The pinned AnimeJaNai `3.6.3` component catalogue. See
// NativeComponentCatalog.hpp for the rules; this file is their one
// implementation.

#include "NativeComponentCatalog.hpp"

#include <iterator>

namespace mplayer {
namespace native {

namespace {

constexpr char kReleaseTag[] = "3.6.3";
constexpr char kRepository[] = "the-database/mpv-AnimeJaNai";

/// A row's file list is a fixed-size slot array padded with nulls, and the walk
/// over it is bounded by the array itself rather than by a sentinel: `trt-runtime`
/// fills every slot, so a sentinel-terminated walk would step past the end of the
/// row and read the next row's pointers as file names.
constexpr int kMaxRowFiles = 6;

struct PackRow {
    const char* name;
    const char* asset;
    qint64 assetBytes;
    const char* assetSha256;
    const char* files[kMaxRowFiles];
    const char* resourceFile;
};

/// The Windows assets of the `3.6.3` release. The release also publishes
/// `-linux-x64` variants of the same packs; those are not this build's
/// platform and are not part of the catalogue.
constexpr PackRow kRows[] = {
    // trtexec.exe is deliberately excluded: nothing in this build invokes it.
    {"trt-runtime", "component-trt-runtime.7z", 186822775,
     "f00ec45133e86965e0b5c91e3c7727c7264bb6029505bdc044b2146983b57a0b",
     {"cudart64_13.dll", "CUDA_LICENSE.txt", "DirectML_LICENSE.txt", "nvinfer_11.dll",
      "nvinfer_plugin_11.dll", "nvonnxparser_11.dll"},
     ""},
    {"trt-sm75", "component-trt-sm75.7z", 90359224,
     "9aa6ac39c328e197eaf99265ed04a94e8e9988358d9c7f9a047c59fe12ac4476",
     {"nvinfer_builder_resource_sm75_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm75_11.dll"},
    {"trt-sm80", "component-trt-sm80.7z", 160207044,
     "5f4806ea00044774da927471d0f44d45f54567dfeeb4d0eaef52524a8b89a2b7",
     {"nvinfer_builder_resource_sm80_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm80_11.dll"},
    {"trt-sm86", "component-trt-sm86.7z", 149799926,
     "be567cd6b51dedf6fa964c069e4590ae20b2dec9febea4a7857f7e719f5f2b95",
     {"nvinfer_builder_resource_sm86_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm86_11.dll"},
    {"trt-sm89", "component-trt-sm89.7z", 158554965,
     "7de69ef11a1e2a033cec5837a0ead9c85d1cd6f13bfebd42d3374006ffec2c1d",
     {"nvinfer_builder_resource_sm89_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm89_11.dll"},
    {"trt-sm90", "component-trt-sm90.7z", 420452077,
     "d560e829c32cb809b1faf87fe71e17ff82fa343f834c6a6d75416beb8525da6a",
     {"nvinfer_builder_resource_sm90_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm90_11.dll"},
    {"trt-sm100", "component-trt-sm100.7z", 251528952,
     "aeafa38abb96335b76286f31c29f76181f75339a6ac727a4d42bfee9467920a4",
     {"nvinfer_builder_resource_sm100_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm100_11.dll"},
    {"trt-sm120", "component-trt-sm120.7z", 229417183,
     "db69ffdf1b658f290db63546433295ea640339840087a5c460d334521dd588a6",
     {"nvinfer_builder_resource_sm120_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm120_11.dll"},
    {"trt-ptx", "component-trt-ptx.7z", 208391718,
     "149fdc181da552ebf7b55bbe8f8544e1beee365d14564924d84b94646165a03f",
     {"nvinfer_builder_resource_ptx_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_ptx_11.dll"},
};

QString assetUrl(const QString& asset) {
    return QStringLiteral("https://github.com/%1/releases/download/%2/%3")
        .arg(QString::fromLatin1(kRepository), componentReleaseTag(), asset);
}

QVector<ComponentPack> buildCatalog() {
    QVector<ComponentPack> packs;
    packs.reserve(static_cast<qsizetype>(sizeof(kRows) / sizeof(kRows[0])));
    for (const PackRow& row : kRows) {
        ComponentPack pack;
        pack.name = QString::fromLatin1(row.name);
        pack.asset = QString::fromLatin1(row.asset);
        pack.url = assetUrl(pack.asset);
        pack.assetBytes = row.assetBytes;
        pack.assetSha256 = QString::fromLatin1(row.assetSha256);
        for (const char* const* file = std::begin(row.files); file != std::end(row.files); ++file) {
            if (*file == nullptr) continue;
            pack.files.append(QString::fromLatin1(*file));
        }
        pack.resourceFile = QString::fromLatin1(row.resourceFile);
        packs.append(pack);
    }
    return packs;
}

}  // namespace

QString componentReleaseTag() { return QString::fromLatin1(kReleaseTag); }

QString componentReleaseUrl() {
    return QStringLiteral("https://github.com/%1/releases/tag/%2")
        .arg(QString::fromLatin1(kRepository), componentReleaseTag());
}

const QVector<ComponentPack>& tensorRtComponentCatalog() {
    static const QVector<ComponentPack> kCatalog = buildCatalog();
    return kCatalog;
}

const ComponentPack* tensorRtRuntimePack() {
    const QVector<ComponentPack>& catalog = tensorRtComponentCatalog();
    for (const ComponentPack& pack : catalog) {
        if (pack.name == QLatin1String("trt-runtime")) return &pack;
    }
    return nullptr;
}

const ComponentPack* tensorRtResourcePackFor(int smMajor, int smMinor) {
    // A minor outside 0..9 has no `trt-sm{MA}{MI}` spelling, and no reported
    // major means no device was observed at all: neither is approximable.
    if (smMajor <= 0 || smMinor < 0 || smMinor > 9) return nullptr;

    const QVector<ComponentPack>& catalog = tensorRtComponentCatalog();
    const QString exact = QStringLiteral("trt-sm%1%2").arg(smMajor).arg(smMinor);
    for (const ComponentPack& pack : catalog) {
        if (pack.name == exact) return &pack;
    }
    for (const ComponentPack& pack : catalog) {
        if (pack.name == QLatin1String("trt-ptx")) return &pack;
    }
    return nullptr;
}

QString builderResourceFileName(const ComponentPack& pack) { return pack.resourceFile; }

QVector<const ComponentPack*> compatiblePacksFor(int smMajor, int smMinor) {
    QVector<const ComponentPack*> packs;
    // A shape the release cannot name has no compatible set: the runtime is not
    // useful on its own when no builder resource can accompany it.
    const ComponentPack* resource = tensorRtResourcePackFor(smMajor, smMinor);
    if (resource == nullptr) {
        return packs;
    }
    const QVector<ComponentPack>& catalog = tensorRtComponentCatalog();
    for (const ComponentPack& pack : catalog) {
        if (pack.name == QLatin1String("trt-runtime")) {
            packs.append(&pack);
            break;
        }
    }
    if (resource->name != QLatin1String("trt-runtime")) {
        packs.append(resource);
    }
    return packs;
}

}  // namespace native
}  // namespace mplayer