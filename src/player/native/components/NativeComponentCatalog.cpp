// The pinned AnimeJaNai `3.7.0` component catalogue. See
// NativeComponentCatalog.hpp for the rules; this file is their one
// implementation.

#include "NativeComponentCatalog.hpp"

#include <iterator>

namespace mplayer {
namespace native {

namespace {

constexpr char kReleaseTag[] = "3.7.0";
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

/// The Windows assets of the `3.7.0` release. The release also publishes
/// `-linux-x64` variants of the same packs; those are not this build's
/// platform and are not part of the catalogue.
constexpr PackRow kRows[] = {
    // trtexec.exe is deliberately excluded: nothing in this build invokes it.
    {"trt-runtime", "component-trt-runtime.7z", 186822775,
     "282cb9b178db241f61f7aedf1ef214af56c2a342428253d04a8d8916d6729373",
     {"cudart64_13.dll", "CUDA_LICENSE.txt", "DirectML_LICENSE.txt", "nvinfer_11.dll",
      "nvinfer_plugin_11.dll", "nvonnxparser_11.dll"},
     ""},
    {"trt-sm75", "component-trt-sm75.7z", 90359224,
     "4bc46f2fd2e9aad1b90666e634334197a28348f24f9a1bdb0d07fadc59b3bc4f",
     {"nvinfer_builder_resource_sm75_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm75_11.dll"},
    {"trt-sm80", "component-trt-sm80.7z", 160207044,
     "5bfe7a9288e866baa8111c851634ec077572130527ebe8844a74109e6a0e5393",
     {"nvinfer_builder_resource_sm80_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm80_11.dll"},
    {"trt-sm86", "component-trt-sm86.7z", 149799926,
     "bdbdc7b92cc3143f76045263a08e506af4594e717763d118ac65404b7ba0b23a",
     {"nvinfer_builder_resource_sm86_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm86_11.dll"},
    {"trt-sm89", "component-trt-sm89.7z", 158554965,
     "34bd51be1cb1581db3e494d1b30732c926cd9ac1a340efabdf0f9148911fdc87",
     {"nvinfer_builder_resource_sm89_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm89_11.dll"},
    {"trt-sm90", "component-trt-sm90.7z", 420452077,
     "b884e983b8b0d61ecebf9990fbed6a3fa13023b5cf9b4c2920933777e5f5ceb5",
     {"nvinfer_builder_resource_sm90_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm90_11.dll"},
    {"trt-sm100", "component-trt-sm100.7z", 251528952,
     "98a4ab1a906123ba61db70cc470c13595b60cf94acde6625c53ad5556a58f447",
     {"nvinfer_builder_resource_sm100_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm100_11.dll"},
    {"trt-sm120", "component-trt-sm120.7z", 229417183,
     "954821ef65824b0c766998db7d2ae49c68041dac7f78328152ec41f674378c15",
     {"nvinfer_builder_resource_sm120_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm120_11.dll"},
    {"trt-ptx", "component-trt-ptx.7z", 208391718,
     "7bd93d7f610ec31124b4ed677395e86337a0fbe9279d6315d7005d64f6dd398e",
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
const RifeArchivePack& rifeArchivePack() {
    static const RifeArchivePack pack{
        QStringLiteral("rife-3.7.0"),
        QStringLiteral("component-rife.7z"),
        QStringLiteral("https://github.com/the-database/mpv-AnimeJaNai/releases/download/3.7.0/component-rife.7z"),
        462444035,
        QStringLiteral("d41de2b56972cb3c446d9830bd5c5e7257e6e099ef0e7a2d5770ec01a9bf7251")};
    return pack;
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