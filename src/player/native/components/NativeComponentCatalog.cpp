// The pinned AnimeJaNai `3.7.0` component catalogue (Windows x64 and Linux x64).
// See NativeComponentCatalog.hpp for the rules; this file is their one
// implementation.

#include "NativeComponentCatalog.hpp"

#include <iterator>

namespace mplayer {
namespace native {

namespace {

constexpr char kReleaseTag[] = "3.11.0";
constexpr char kRepository[] = "the-database/mpv-AnimeJaNai";

constexpr int kMaxRowFiles = 7;

struct PackFileRow {
    const char* path;
    qint64 bytes;
    const char* sha256;
};

struct PackRow {
    const char* name;
    const char* asset;
    qint64 assetBytes;
    const char* assetSha256;
    const char* version;
    PackFileRow files[kMaxRowFiles];
    const char* resourceFile;
};

constexpr PackRow kWindowsRows[] = {
    {"trt-runtime", "trt-runtime-windows-x64.7z", 167328448,
     "e90ecfce92ff35af48a595cb16003e4df7a3f77e812737d8e9d14a4a41722d95", "11.4.0.106-cuda13.4",
     {{"CUDA_LICENSE.txt", 68070, "088381bc2d891e719a2a9398645b00bb45f3b24231473a8283ac7e3e66b8a028"},
      {"TensorRT_Acknowledgements.txt", 327505, "175f0eff1102f77a9c84973e5e0cd2b5284b63abd4db379108b4d303189e9844"},
      {"cudart64_13.dll", 551024, "05bfafcb97bd53b0089568a96e1e5bd6921637bd0853ff7f5342bd31a6a16890"},
      {"nvinfer_11.dll", 394814576, "24c40791e950e17e44fc777e81dd22fc9da0db856d5f45fc379211d34d384044"},
      {"nvinfer_plugin_11.dll", 45633136, "5b9e4e143d2c86978e775f6d08dc2fe0ad4c71ca824ab6427a4e895e338049b1"},
      {"nvonnxparser_11.dll", 2241648, "4a426d73970ce3af7fc1a1f1f29f8957933a28ce7e0fee18b7fb0227f71d685e"},
      {"trtexec.exe", 981104, "f57cce2baf75f457b547586daed4e250bb1d64716a76f2f95933901019f47803"}},
     ""},
    {"trt-sm75", "trt-sm75-windows-x64.7z", 84624118,
     "4ee3d28dd285c7cd477a3938ded3de8e6391c89f24e7c6e0f6a1cf59ad11abd4", "11.4.0.106",
     {{"nvinfer_builder_resource_sm75_11.dll", 118533232, "7b47a94f3b05f83c3af4f4446a78bc811efad7985ce9909b1a45ed02fa73c267"}},
     "nvinfer_builder_resource_sm75_11.dll"},
    {"trt-sm80", "trt-sm80-windows-x64.7z", 153292989,
     "25931eb2ae3c1780aae73f3dbb10773ec57e21457a749d0b8bee8b034ce53458", "11.4.0.106",
     {{"nvinfer_builder_resource_sm80_11.dll", 190718576, "3aa4e7cf330b5d0b9466d0017dcc01a5777f98aa0c6df10a3f4fccc0df1a4365"}},
     "nvinfer_builder_resource_sm80_11.dll"},
    {"trt-sm86", "trt-sm86-windows-x64.7z", 142865976,
     "72ab75e4a6edbdc28381c897821e0b01d4b05d027b70e52d62193220274288bc", "11.4.0.106",
     {{"nvinfer_builder_resource_sm86_11.dll", 180030576, "351cf7eb5fe7b605fa48b1eb811a65b7e6b4746fd2b911c6ca8a768b0434edfd"}},
     "nvinfer_builder_resource_sm86_11.dll"},
    {"trt-sm89", "trt-sm89-windows-x64.7z", 151608988,
     "0983741bb6fc64a7e991fb4a53c73c051bf127b5a13f4faf0f1ec6b572752833", "11.4.0.106",
     {{"nvinfer_builder_resource_sm89_11.dll", 189072496, "7aced8ca0208605eda8cdd77840ddab2eb5a91b3e3a568571be12c0549c61ac2"}},
     "nvinfer_builder_resource_sm89_11.dll"},
    {"trt-sm90", "trt-sm90-windows-x64.7z", 398376462,
     "0704ac813447716a19afcea567840e08b13e24bf4d1491d61bcc0948f07251b3", "11.4.0.106",
     {{"nvinfer_builder_resource_sm90_11.dll", 462014576, "6fd7167a7a7ac76b60a0d32b4d722f734bb303bba6d00f98c0ba3f17cebfd028"}},
     "nvinfer_builder_resource_sm90_11.dll"},
    {"trt-sm100", "trt-sm100-windows-x64.7z", 229671324,
     "fb3ed7d0c69838e82ddf40409d54d598c6ed2304d1a32b5db5b339f73427445a", "11.4.0.106",
     {{"nvinfer_builder_resource_sm100_11.dll", 290771568, "60645872ef25ae16cb893048d88069f46774f75a5ae5547b21cb8e9bdb4ad844"}},
     "nvinfer_builder_resource_sm100_11.dll"},
    {"trt-sm120", "trt-sm120-windows-x64.7z", 207817866,
     "f36b2ab932e0ce212a2b612ca14c34981f5f11b37d55b00bccd3565e2ec3d292", "11.4.0.106",
     {{"nvinfer_builder_resource_sm120_11.dll", 269390448, "cb69e038787ddf4f1a15f18517ec158ba9e5d581a4248831db6c35a9d806bec0"}},
     "nvinfer_builder_resource_sm120_11.dll"},
    {"trt-ptx", "trt-ptx-windows-x64.7z", 196936312,
     "c4638bc45abe061e0e340676828db4334e4aba57c9e2f8e644d0463f70af0752", "11.4.0.106",
     {{"nvinfer_builder_resource_ptx_11.dll", 242597488, "de9c41c6584721101fd57aaa271009e68a59c58a5ccbe860d0b853a9d1169b36"}},
     "nvinfer_builder_resource_ptx_11.dll"}
};

constexpr PackRow kLinuxRows[] = {
    {"trt-runtime", "trt-runtime-linux-x64.7z", 245433445,
     "6bdc8b6241b198a6d0b04ab570b88637ddb92517febf696b05cbb3f904217697", "11.4.0.106-cuda13.4",
     {{"CUDA_LICENSE.txt", 68070, "088381bc2d891e719a2a9398645b00bb45f3b24231473a8283ac7e3e66b8a028"},
      {"TensorRT_Acknowledgements.txt", 660, "de4735dbd461804c69879c23c1d27f587ed04fa5433988e0c65efe7c0a219073"},
      {"libcudart.so.13", 798496, "a77eeb711d350a7791a45a07f37cd3b2ae61c71da4db1bf50cc8bd45d33b5b72"},
      {"libnvinfer.so.11", 715142744, "fe5b772875d69e2138a00d9198a72a0d3b00712801475ec20f7c4e0e1e521e59"},
      {"libnvinfer_plugin.so.11", 47333576, "2e979f102b5acd3856ef1a57169e7032f22f5f85c9bcc4c30e9a32647662fb7d"},
      {"libnvonnxparser.so.11", 3999648, "f7e18f33774d4398de854c630c442ec24bc430f9555a0129804db3c4e2cf5cd7"},
      {"trtexec", 2479320, "28a6a92f7adfd761e253cf12bf708942ec91f4bf5b75d41e981a303facbc3b3d"}},
     ""},
    {"trt-sm75", "trt-sm75-linux-x64.7z", 84507111,
     "8da3d44a76689e09d77c31349fd029ef59c068d5d5059b50da7d78e88195d524", "11.4.0.106",
     {{"libnvinfer_builder_resource_sm75.so.11.4.0", 118384608, "f1a746c94fce6ed6aa9ea08ab67b7b194537346d71284880624b387e9c495420"}},
     "libnvinfer_builder_resource_sm75.so.11.4.0"},
    {"trt-sm80", "trt-sm80-linux-x64.7z", 153399541,
     "6a195fa335095a7b6e8d0d55914d2e9bbc26588254aa7fb4786acc82ed1e0b4c", "11.4.0.106",
     {{"libnvinfer_builder_resource_sm80.so.11.4.0", 190569872, "ecf7483204cd58c21668aaa3c758def03e3755277a5528a95e7ebf5ca831a3fe"}},
     "libnvinfer_builder_resource_sm80.so.11.4.0"},
    {"trt-sm86", "trt-sm86-linux-x64.7z", 142868591,
     "0b182a114638461feb50f154df69c7bd7d1d5d648cec66edf107c9fe8dc33993", "11.4.0.106",
     {{"libnvinfer_builder_resource_sm86.so.11.4.0", 179881560, "4a2df7d9e8e15ae6fc09c351ecc3935893a19ad271d52785e0816c6254ee818f"}},
     "libnvinfer_builder_resource_sm86.so.11.4.0"},
    {"trt-sm89", "trt-sm89-linux-x64.7z", 151732112,
     "827141254d37207cda953ff077e819daa178ec1120ce08c5ad20f6ce1c4850ba", "11.4.0.106",
     {{"libnvinfer_builder_resource_sm89.so.11.4.0", 188923672, "04b2dc16c38122588982e69f9f558ca1235d257c4f09102a1d75cb49115faf28"}},
     "libnvinfer_builder_resource_sm89.so.11.4.0"},
    {"trt-sm90", "trt-sm90-linux-x64.7z", 405994273,
     "dbefda3720fd270eb2b7727e00144a5b224c744c6a72b7f3881868f25ca1a0b7", "11.4.0.106",
     {{"libnvinfer_builder_resource_sm90.so.11.4.0", 461874352, "a10ff0060be1e03771324ed850da12c5b44b2986e6657fceffae227ab5f0f13d"}},
     "libnvinfer_builder_resource_sm90.so.11.4.0"},
    {"trt-sm100", "trt-sm100-linux-x64.7z", 234004050,
     "9179ddc964f6683b6cbbca1e5bd2974c418451ec872f040dbab5cc1238ca7797", "11.4.0.106",
     {{"libnvinfer_builder_resource_sm100.so.11.4.0", 290620824, "d1782042639b3ba81a065383a08fccabeb5d5860865b404505fe7d7162ad47bf"}},
     "libnvinfer_builder_resource_sm100.so.11.4.0"},
    {"trt-sm120", "trt-sm120-linux-x64.7z", 212475726,
     "8f22d6706d4d12190524ee2116b2f6abdae7fcc8845e071ed296a4e0306d568c", "11.4.0.106",
     {{"libnvinfer_builder_resource_sm120.so.11.4.0", 269247512, "71f73cd1564519ae759b2e1d0cacd3b298f22c96d8901b3679d45836a27f07dd"}},
     "libnvinfer_builder_resource_sm120.so.11.4.0"},
    {"trt-ptx", "trt-ptx-linux-x64.7z", 196515294,
     "d6ddaff4405847369f72df462147a2d39723531d3b4e1d362d0e0464d17d9a2e", "11.4.0.106",
     {{"libnvinfer_builder_resource_ptx.so.11.4.0", 241285472, "32e667753cd716d6c68f576a0cf0638687ec8472bfbe8dc33eb419e87b27c0a5"}},
     "libnvinfer_builder_resource_ptx.so.11.4.0"}
};

constexpr PackRow kWindowsRtxRows[] = {
    {"trtrtx-runtime", "trtrtx-runtime-windows-x64.7z", 64238886,
     "b2bcc091c399b0e3f0779e298f4a33c721d52d493da707e4356bd2b9a99d66bd", "1.7.1.116-cuda13.4",
     {{"CUDA_LICENSE.txt", 68070, "088381bc2d891e719a2a9398645b00bb45f3b24231473a8283ac7e3e66b8a028"},
      {"TensorRT-RTX-Acknowledgements.txt", 107164, "611660ec0ae19c7b8df4a6442009c8bc85ba9e56d7d5ea042564841e72032d25"},
      {"TensorRT-RTX-NOTICE.txt", 868, "da7aa4b4bba64232bb9e868c5899e14bd74e983c6f5df017febfe24f72e2b5e7"},
      {"cudart64_13.dll", 551024, "05bfafcb97bd53b0089568a96e1e5bd6921637bd0853ff7f5342bd31a6a16890"},
      {"tensorrt_onnxparser_rtx_1_7.dll", 2329200, "b38d00d107dae534f1fc855df5bc1a04fc0ae25135b5b10d17f9b30cc4c9c411"},
      {"tensorrt_rtx_1_7.dll", 229312112, "7b8a06d4b66dfe7da7f1ff48a994ad9dcf107cd28b4822d6f25453528c0db6dd"}},
     ""}
};

QString assetUrl(const QString& asset) {
    return QStringLiteral("https://github.com/the-database/animejanai-inference/releases/download/runtime-2/%1")
        .arg(asset);
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
        pack.version = QString::fromLatin1(row.version);
        for (const PackFileRow& file : row.files) {
            if (file.path == nullptr) continue;
            const QString name = QString::fromLatin1(file.path);
            pack.filePins.append({name, file.bytes, QString::fromLatin1(file.sha256)});
            if (name != QLatin1String("trtexec") && name != QLatin1String("trtexec.exe"))
                pack.files.append(name);
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

const ComponentPack* tensorRtRtxRuntimePack() {
    static const QVector<ComponentPack> catalog = buildCatalogFromRows(kWindowsRtxRows);
    return &catalog.first();
}

const RifeArchivePack& rifeWindowsArchivePack() {
    static const RifeArchivePack pack{
        QStringLiteral("rife-3.11.0"),
        QStringLiteral("component-rife.7z"),
        QStringLiteral("https://github.com/the-database/mpv-AnimeJaNai/releases/download/3.11.0/component-rife.7z"),
        462444571,
        QStringLiteral("d8d4ba94b56059de085622e46b3730934dd6996583cd5eaee38000518baff0d5"),
        true};
    return pack;
}

const RifeArchivePack& rifeLinuxArchivePack() {
    static const RifeArchivePack pack{
        QStringLiteral("rife-3.11.0"),
        QStringLiteral("component-rife-linux-x64.7z"),
        QStringLiteral("https://github.com/the-database/mpv-AnimeJaNai/releases/download/3.11.0/component-rife-linux-x64.7z"),
        462444582,
        QStringLiteral("6fd21cdde1bcc5f36107e147c9d024c4eebf300f57952375fca89ccb90a93146"),
        true};
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
    if (smMajor < 7 || smMinor < 0 || smMinor > 9 || (smMajor == 7 && smMinor < 5))
        return nullptr;

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
