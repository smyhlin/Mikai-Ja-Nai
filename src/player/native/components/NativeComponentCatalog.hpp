#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace mplayer::native {

struct ComponentFilePin {
    QString path;
    qint64 bytes = 0;
    QString sha256;
};

struct ComponentPack {
    QString name;
    QString asset;
    QString url;
    qint64 assetBytes = 0;
    QString assetSha256;
    // trtexec stays out of the prepared-helper store; the AJI closure owns its copy.
    QStringList files;
    QString resourceFile;
    bool exactAssetBytes = true;
    QString version{};
    QVector<ComponentFilePin> filePins{};
};

QString componentReleaseTag();
QString componentReleaseUrl();
const QVector<ComponentPack>& tensorRtComponentCatalog();
const QVector<ComponentPack>& tensorRtWindowsComponentCatalog();
const QVector<ComponentPack>& tensorRtLinuxComponentCatalog();
const ComponentPack* tensorRtRtxRuntimePack();

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
const ComponentPack* tensorRtRuntimePack();
// TensorRT 11 requires SM 7.5+; PTX is not a fallback for an older GPU.
const ComponentPack* tensorRtResourcePackFor(int smMajor, int smMinor);
QString builderResourceFileName(const ComponentPack& pack);
QVector<const ComponentPack*> compatiblePacksFor(int smMajor, int smMinor);

}  // namespace mplayer::native
