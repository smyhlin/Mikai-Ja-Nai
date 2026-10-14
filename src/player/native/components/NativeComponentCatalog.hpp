// The pinned AnimeJaNai component catalogue: the TensorRT runtime pack, one
// builder-resource pack per published compute generation, the PTX pack, and the
// separate RIFE archive transport pin. RIFE graph identity lives in NativeRife.
//
// Three decisions are load-bearing here.
//
//  * The catalogue is data, not a lookup against the network. Every asset name,
//    byte count and SHA-256 of the `3.7.0` release is pinned in this table, so a
//    re-uploaded asset is refused by digest at the transport rather than
//    installed, and no run-time decision depends on GitHub being reachable.
//    Bumping the release means editing this table and `native/manifest/
//    animejanai-release.json` together.
//
//  * Selection mirrors the upstream updater (`RecommendedPacks`): the exact
//    generation pack when the release publishes one, otherwise the PTX pack.
//    The pack name is built as `trt-sm{MA}{MI}`, so the minor must be 0..9;
//    anything else is a shape the release cannot name and is refused with a null
//    rather than silently approximated. The same selection must be reproducible
//    in both directions, so it is computed from the numbers, never from an index.
//
//  * `trtexec.exe` / `trtexec` is deliberately absent from the runtime pack's
//    store file list: the private prepared-helper path builds plans in process
//    and never invokes it, while the official v0.9.0 config/first-build path
//    extracts it directly into its runtime closure.
//
// The runtime and the builder resource are separate packs because the release
// publishes them separately, but they are always selected and installed as one
// same-release pair: cross-paired closures are measured-incompatible (see
// `native/manifest/videojanai-release.json`), so the pack set is carried whole.

#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace mplayer {
namespace native {

/// One published component pack. `files` are basenames under
/// `animejanai/inference/` in the archive, in install order; `resourceFile` is
/// the single builder resource the pack carries and is empty for a pack that
/// carries none.
struct ComponentPack {
    QString name;
    QString asset;
    QString url;
    qint64 assetBytes = 0;
    QString assetSha256;
    QStringList files;
    QString resourceFile;
    bool exactAssetBytes = true;
};

/// The pinned release tag, "3.7.0".
QString componentReleaseTag();

/// The release page the assets above come from.
QString componentReleaseUrl();

/// Every pinned pack for the active host platform, runtime first, then the
/// resource packs in generation order and the PTX pack last.
const QVector<ComponentPack>& tensorRtComponentCatalog();

/// Explicit platform-specific catalogues for Windows x64 and Linux x64.
const QVector<ComponentPack>& tensorRtWindowsComponentCatalog();
const QVector<ComponentPack>& tensorRtLinuxComponentCatalog();

/// The independently downloadable RIFE 3.7.0 archive for the active host
/// platform (`component-rife.7z` on Windows, `component-rife-linux-x64.7z` on
/// Linux). Its graph inventory comes from rifeModelPins(); this pack describes
/// only the archive transport.
struct RifeArchivePack {
    QString name;
    QString asset;
    QString url;
    qint64 assetBytes = 0;
    QString assetSha256;
    bool exactAssetBytes = true;
};

const RifeArchivePack& rifeArchivePack();
const RifeArchivePack& rifeWindowsArchivePack();
const RifeArchivePack& rifeLinuxArchivePack();

/// The pack named "trt-runtime"; never null while the table is compiled in.
const ComponentPack* tensorRtRuntimePack();

/// The pack carrying the builder resource this GPU's compute capability needs:
/// the exact `trt-sm{MA}{MI}` when the release publishes it, otherwise the PTX
/// pack. Null when the shape itself is unusable - no major version reported, or
/// a minor version outside 0..9, which the pack name cannot express.
const ComponentPack* tensorRtResourcePackFor(int smMajor, int smMinor);

/// The single builder-resource basename the pack carries, empty for a pack that
/// carries none.
QString builderResourceFileName(const ComponentPack& pack);

/// What one GPU may actually use: the runtime every TensorRT build needs, plus
/// the single builder resource its compute capability resolves to. The other
/// generations belong to other cards and listing them offers downloads that
/// could never run here, so this is also what every surface that renders the
/// catalogue shows. Empty when the shape is unusable.
QVector<const ComponentPack*> compatiblePacksFor(int smMajor, int smMinor);

}  // namespace native
}  // namespace mplayer
