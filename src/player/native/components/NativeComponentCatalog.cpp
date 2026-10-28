// The pinned AnimeJaNai `3.7.0` component catalogue (Windows x64 and Linux x64).
// See NativeComponentCatalog.hpp for the rules; this file is their one
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
    bool exactAssetBytes;
};

/// The Windows x64 assets of the `3.7.0` release.
constexpr PackRow kWindowsRows[] = {
    // trtexec.exe is not a pack member: the private prepared-helper path builds
    // plans in process and never invokes it, while the official v0.9.0
    // config/first-build path still requires and receives it from this archive.
    {"trt-runtime", "component-trt-runtime.7z", 186822775,
     "282cb9b178db241f61f7aedf1ef214af56c2a342428253d04a8d8916d6729373",
     {"cudart64_13.dll", "CUDA_LICENSE.txt", "DirectML_LICENSE.txt", "nvinfer_11.dll",
      "nvinfer_plugin_11.dll", "nvonnxparser_11.dll"},
     "", true},
    {"trt-sm75", "component-trt-sm75.7z", 90359224,
     "4bc46f2fd2e9aad1b90666e634334197a28348f24f9a1bdb0d07fadc59b3bc4f",
     {"nvinfer_builder_resource_sm75_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm75_11.dll", true},
    {"trt-sm80", "component-trt-sm80.7z", 160207044,
     "5bfe7a9288e866baa8111c851634ec077572130527ebe8844a74109e6a0e5393",
     {"nvinfer_builder_resource_sm80_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm80_11.dll", true},
    {"trt-sm86", "component-trt-sm86.7z", 149799926,
     "bdbdc7b92cc3143f76045263a08e506af4594e717763d118ac65404b7ba0b23a",
     {"nvinfer_builder_resource_sm86_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm86_11.dll", true},
    {"trt-sm89", "component-trt-sm89.7z", 158554965,
     "34bd51be1cb1581db3e494d1b30732c926cd9ac1a340efabdf0f9148911fdc87",
     {"nvinfer_builder_resource_sm89_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm89_11.dll", true},
    {"trt-sm90", "component-trt-sm90.7z", 420452077,
     "b884e983b8b0d61ecebf9990fbed6a3fa13023b5cf9b4c2920933777e5f5ceb5",
     {"nvinfer_builder_resource_sm90_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm90_11.dll", true},
    {"trt-sm100", "component-trt-sm100.7z", 251528952,
     "98a4ab1a906123ba61db70cc470c13595b60cf94acde6625c53ad5556a58f447",
     {"nvinfer_builder_resource_sm100_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm100_11.dll", true},
    {"trt-sm120", "component-trt-sm120.7z", 229417183,
     "954821ef65824b0c766998db7d2ae49c68041dac7f78328152ec41f674378c15",
     {"nvinfer_builder_resource_sm120_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_sm120_11.dll", true},
    {"trt-ptx", "component-trt-ptx.7z", 208391718,
     "7bd93d7f610ec31124b4ed677395e86337a0fbe9279d6315d7005d64f6dd398e",
     {"nvinfer_builder_resource_ptx_11.dll", nullptr, nullptr, nullptr, nullptr, nullptr},
     "nvinfer_builder_resource_ptx_11.dll", true},
};

/// The Linux x64 (`-linux-x64`) assets of the `3.7.0` release:
/// https://github.com/the-database/mpv-AnimeJaNai/releases/tag/3.7.0
constexpr PackRow kLinuxRows[] = {
    // The archive also ships the fully versioned files (libnvinfer.so.11.3.0 and
    // friends); installing the SONAME links covers the closure without storing
    // a second copy of ~400 MB of libraries.
    // trtexec is handled separately by AnimeJanaiInstallation.cpp (explicit copy after
    // archive extraction or store fallback), so it is not listed here to avoid a
    // duplicate copy that would fail with QIODevice::NewOnly.
    {"trt-runtime", "component-trt-runtime-linux-x64.7z", 263192576,
     "2d3a5a20008d752d3cd287fedd5f774baa6853068921bbd9b2add96742a7406b",
     {"CUDA_LICENSE.txt", "libcudart.so.13", "libnvinfer.so.11", "libnvinfer_plugin.so.11",
      "libnvonnxparser.so.11"},
     "", false},
    {"trt-sm75", "component-trt-sm75-linux-x64.7z", 91121152,
     "ccaf69de0d99940d56553b8f31de4e1aeda6b9ce3f55d5509b56d4a2e07f4cdd",
     {"libnvinfer_builder_resource_sm75.so.11.3.0", nullptr, nullptr, nullptr, nullptr, nullptr},
     "libnvinfer_builder_resource_sm75.so.11.3.0", false},
    {"trt-sm80", "component-trt-sm80-linux-x64.7z", 161480704,
     "9ecdc3ed796f0509dbdc2b8420440bf7961e1a5b82b6aa51d64ddbd12e554258",
     {"libnvinfer_builder_resource_sm80.so.11.3.0", nullptr, nullptr, nullptr, nullptr, nullptr},
     "libnvinfer_builder_resource_sm80.so.11.3.0", false},
    {"trt-sm86", "component-trt-sm86-linux-x64.7z", 150994944,
     "a4d8b40e4974dd504d8b3a37ece656f6d29dfdda86b76065b70fa74f0aa33bf2",
     {"libnvinfer_builder_resource_sm86.so.11.3.0", nullptr, nullptr, nullptr, nullptr, nullptr},
     "libnvinfer_builder_resource_sm86.so.11.3.0", false},
    {"trt-sm89", "component-trt-sm89-linux-x64.7z", 159383552,
     "cd683e8afd41cf315fe6dc488a1c97dbcb5b5b7e5795893ece4079129d677871",
     {"libnvinfer_builder_resource_sm89.so.11.3.0", nullptr, nullptr, nullptr, nullptr, nullptr},
     "libnvinfer_builder_resource_sm89.so.11.3.0", false},
    {"trt-sm90", "component-trt-sm90-linux-x64.7z", 425721856,
     "3054e26228be0b2413af0d6d99aa7cc621178c4f902aed8a3bfc6160c57f7b9d",
     {"libnvinfer_builder_resource_sm90.so.11.3.0", nullptr, nullptr, nullptr, nullptr, nullptr},
     "libnvinfer_builder_resource_sm90.so.11.3.0", false},
    {"trt-sm100", "component-trt-sm100-linux-x64.7z", 252706816,
     "71beeb07e3c1e15d7b8c7a27455a931f1c92a453d0d2fe6d6eb5052168270e9f",
     {"libnvinfer_builder_resource_sm100.so.11.3.0", nullptr, nullptr, nullptr, nullptr, nullptr},
     "libnvinfer_builder_resource_sm100.so.11.3.0", false},
    {"trt-sm120", "component-trt-sm120-linux-x64.7z", 230686720,
     "f3f52f355cbe9d7768a16ec2e974681ce56847aaed2f988150fd23f9c8074d2e",
     {"libnvinfer_builder_resource_sm120.so.11.3.0", nullptr, nullptr, nullptr, nullptr, nullptr},
     "libnvinfer_builder_resource_sm120.so.11.3.0", false},
    {"trt-ptx", "component-trt-ptx-linux-x64.7z", 208666624,
     "c1061e7b11cb865c4c1f7d1eea4bce7357f45e073b74bbf10916b3a72faba0f3",
     {"libnvinfer_builder_resource_ptx.so.11.3.0", nullptr, nullptr, nullptr, nullptr, nullptr},
     "libnvinfer_builder_resource_ptx.so.11.3.0", false},
};

QString assetUrl(const QString& asset) {
    return QStringLiteral("https://github.com/%1/releases/download/%2/%3")
        .arg(QString::fromLatin1(kRepository), componentReleaseTag(), asset);
}

template <std::size_t N>
QVector<ComponentPack> buildCatalogFromRows(const PackRow (&rows)[N]) {
    QVector<ComponentPack> packs;
    packs.reserve(static_cast<qsizetype>(N));
    for (const PackRow& row : rows) {
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
        pack.exactAssetBytes = row.exactAssetBytes;
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

const QVector<ComponentPack>& tensorRtWindowsComponentCatalog() {
    static const QVector<ComponentPack> kCatalog = buildCatalogFromRows(kWindowsRows);
    return kCatalog;
}

const QVector<ComponentPack>& tensorRtLinuxComponentCatalog() {
    static const QVector<ComponentPack> kCatalog = buildCatalogFromRows(kLinuxRows);
    return kCatalog;
}

const QVector<ComponentPack>& tensorRtComponentCatalog() {
#ifdef Q_OS_WIN
    return tensorRtWindowsComponentCatalog();
#else
    return tensorRtLinuxComponentCatalog();
#endif
}

const RifeArchivePack& rifeWindowsArchivePack() {
    static const RifeArchivePack pack{
        QStringLiteral("rife-3.7.0"),
        QStringLiteral("component-rife.7z"),
        QStringLiteral("https://github.com/the-database/mpv-AnimeJaNai/releases/download/3.7.0/component-rife.7z"),
        462444035,
        QStringLiteral("d41de2b56972cb3c446d9830bd5c5e7257e6e099ef0e7a2d5770ec01a9bf7251"),
        true};
    return pack;
}

const RifeArchivePack& rifeLinuxArchivePack() {
    static const RifeArchivePack pack{
        QStringLiteral("rife-3.7.0"),
        QStringLiteral("component-rife-linux-x64.7z"),
        QStringLiteral("https://github.com/the-database/mpv-AnimeJaNai/releases/download/3.7.0/component-rife-linux-x64.7z"),
        462444035,
        QStringLiteral("265797d56fb1376434658e6adf70ea5ee85b8ac4c51231fdd9e744d35462e136"),
        false};
    return pack;
}

const RifeArchivePack& rifeArchivePack() {
#ifdef Q_OS_WIN
    return rifeWindowsArchivePack();
#else
    return rifeLinuxArchivePack();
#endif
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
