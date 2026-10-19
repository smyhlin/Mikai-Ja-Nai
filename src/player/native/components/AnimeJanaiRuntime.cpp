#include "AnimeJanaiRuntime.hpp"

#include "native/bundle/NativePaths.hpp"
#include "native/components/AnimeJanaiConfig.hpp"
#include "native/models/NativeRife.hpp"
#include "native/models/NativeSpatialAssets.hpp"
#include "native/status/NativeFilterCommand.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QHash>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <QtCore/QMap>

namespace mplayer::native {
namespace {

QHash<QString, QString>& activeAnimeJanaiStageCaches() {
    static QHash<QString, QString> owners;
    return owners;
}
std::mutex& animeJanaiStageCacheMutex() {
    static std::mutex mutex;
    return mutex;
}

QString appNativeRoot() {
    return nativeRoot();
}
QString configRoot() { return QDir(appNativeRoot()).filePath(QStringLiteral("animejanai")); }
class RuntimeText {
    Q_DECLARE_TR_FUNCTIONS(AnimeJanaiRuntime)
};

void fail(AnimeJanaiPlan& plan, const QString& key, const QString& message) {
    plan.available = false;
    plan.reasonKey = key;
    plan.reason = message;
    plan.filterSpec.clear();
    plan.stages.clear();
    plan.cacheKeys.clear();
    plan.cacheLease.reset();
}

QString sha256(const QString& path, const std::function<bool()>& cancelled = {}) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    while (!file.atEnd()) {
        if (cancelled && cancelled()) return {};
        const qint64 count = file.read(buffer.data(), buffer.size());
        if (count <= 0) return {};
        hash.addData(QByteArrayView(buffer.constData(), count));
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool regularFile(const QString& path) {
    const QFileInfo info(path);
    return info.isFile() && !info.isSymLink();
}

bool hashMatches(const QString& path, qint64 size, const QString& digest,
                 const std::function<bool()>& cancelled = {}) {
    const QFileInfo info(path);
    return regularFile(path) && info.size() == size && sha256(path, cancelled) == digest;
}

bool pathInside(const QString& root, const QString& path) {
    return nativePathTrust(path, true).trusted &&
           pathWithinRoot(nativeRoot(), root) && pathWithinRoot(root, path) &&
           pathHasNoReparsePoints(nativeRoot(), path);
}

bool copyVerified(const QString& source, const QString& target, qint64 size, const QString& digest,
                  const QString& ownedRoot, const std::function<bool()>& cancelled, QString* error) {
    if (cancelled && cancelled()) { *error = RuntimeText::tr("AnimeJaNai preparation was cancelled."); return false; }
    const QFileInfo sourceInfo(source);
    if (!regularFile(source) || sourceInfo.size() != size) {
        *error = RuntimeText::tr("The selected model graph is missing or has an unexpected size.");
        return false;
    }
    if (!pathInside(ownedRoot, target) ||
        !QDir().mkpath(QFileInfo(target).absolutePath()) || !pathInside(ownedRoot, target)) {
        *error = RuntimeText::tr("The app-owned AnimeJaNai model directory is unavailable or redirected.");
        return false;
    }
    if (QFileInfo::exists(target)) {
        if (!hashMatches(source, size, digest, cancelled)) {
            *error = cancelled && cancelled()
                         ? RuntimeText::tr("AnimeJaNai preparation was cancelled.")
                         : RuntimeText::tr("The selected model graph does not match its pinned content digest.");
            return false;
        }
        if (hashMatches(target, size, digest, cancelled)) return true;
        *error = cancelled && cancelled()
                     ? RuntimeText::tr("AnimeJaNai preparation was cancelled.")
                     : RuntimeText::tr("An existing staged AnimeJaNai graph is corrupt and will not be replaced.");
        return false;
    }
    const QString temporary = target + QStringLiteral(".tmp-") +
                              QUuid::createUuid().toString(QUuid::Id128);
    QFile input(source);
    QSaveFile output(temporary);
    output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        *error = !input.isOpen() ? input.errorString() : output.errorString();
        return false;
    }
    QCryptographicHash copiedHash(QCryptographicHash::Sha256);
    qint64 copiedBytes = 0;
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    while (!input.atEnd()) {
        if (cancelled && cancelled()) {
            output.cancelWriting();
            *error = RuntimeText::tr("AnimeJaNai preparation was cancelled.");
            return false;
        }
        const qint64 count = input.read(buffer.data(), buffer.size());
        if (count <= 0 || copiedBytes > size - count ||
            output.write(buffer.constData(), count) != count) {
            *error = input.error() != QFile::NoError ? input.errorString() : output.errorString();
            output.cancelWriting();
            return false;
        }
        copiedBytes += count;
        copiedHash.addData(QByteArrayView(buffer.constData(), count));
    }
    if (cancelled && cancelled()) {
        output.cancelWriting();
        *error = RuntimeText::tr("AnimeJaNai preparation was cancelled.");
        return false;
    }
    if (copiedBytes != size ||
        QString::fromLatin1(copiedHash.result().toHex()) != digest) {
        output.cancelWriting();
        *error = RuntimeText::tr("The selected model graph does not match its pinned content digest.");
        return false;
    }
    if (!output.commit()) {
        QFile::remove(temporary);
        *error = RuntimeText::tr("Staged AnimeJaNai graph verification or atomic publication failed.");
        return false;
    }
    if (cancelled && cancelled()) {
        QFile::remove(temporary);
        *error = RuntimeText::tr("AnimeJaNai preparation was cancelled.");
        return false;
    }
    if (!QFile::rename(temporary, target)) {
        QFile::remove(temporary);
        if (hashMatches(target, size, digest, cancelled)) return true;
        *error = RuntimeText::tr("Staged AnimeJaNai graph verification or atomic publication failed.");
        return false;
    }
    return true;
}

const AnimeJanaiPreset* findPreset(const QString& id, const QVector<AnimeJanaiPreset>& presets) {
    for (const AnimeJanaiPreset& preset : presets) if (preset.id == id) return &preset;
    return nullptr;
}

QString safeConfigName(const QString& presetId) {
    if (presetId.startsWith(QStringLiteral("builtin_slot:"))) {
        bool ok = false;
        const int slotNum = presetId.mid(QStringLiteral("builtin_slot:").size()).toInt(&ok);
        if (ok && slotNum > 0) {
            return QStringLiteral("custom_slot_%1").arg(slotNum);
        }
    }
    if (presetId.startsWith(QStringLiteral("builtin"))) return QStringLiteral("builtins");
    return QString::fromLatin1(QCryptographicHash::hash(presetId.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QByteArray modelLine(int chain, const QByteArray& name) {
    return QByteArrayLiteral("chain_") + QByteArray::number(chain) + QByteArrayLiteral("_model_1_name=") + name + '\n';
}

// Retired SD opset-23 model cleanup (adopted from AnimeJaNaiUpdater MigrateAnimeJaNaiConf).
// Gated on the opset-21 SD model being staged so the last copy is never removed.
void pruneLegacySdModelOrphans(const QString& modelDir) {
    const QString op21Hash = QStringLiteral("ef6be5b2535da762dc7e9e4c5cbdb57780f18ab21417d63ab575f05004538411");
    const QString op21Name = QStringLiteral("2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo");
    const QString op23Name = QStringLiteral("2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op23_dynamo");
    const QDir dir(modelDir);
    if (!dir.exists()) return;
    if (!QFileInfo::exists(dir.filePath(op21Hash + QStringLiteral(".onnx"))) &&
        !QFileInfo::exists(dir.filePath(op21Name + QStringLiteral(".onnx")))) {
        return;
    }
    for (const QFileInfo& entry : dir.entryInfoList(QDir::Files | QDir::NoSymLinks)) {
        const QString fileName = entry.fileName();
        if (fileName.startsWith(op23Name + QLatin1Char('.'))) {
            QFile::remove(entry.absoluteFilePath());
        }
    }
}

QByteArray globalIniHeader(const AnimeJanaiConfig& managerConf) {
    const QString defaultTrtSettings = QStringLiteral(
        "--builderOptimizationLevel=5 --optShapes=input:%video_resolution% --skipInference");
    QByteArray ini = QByteArrayLiteral("[global]\nconfig_version=3\n");
    ini += QByteArrayLiteral("backend=TensorRT\n");
    ini += managerConf.enableLogging ? QByteArrayLiteral("logging=yes\n")
                                     : QByteArrayLiteral("logging=no\n");
    if (!managerConf.trtEngineSettings.trimmed().isEmpty() &&
        managerConf.trtEngineSettings.trimmed() != defaultTrtSettings) {
        QString settings = managerConf.trtEngineSettings.trimmed();
        settings.remove(QChar::Null);
        settings.replace(u'\r', u' ');
        settings.replace(u'\n', u' ');
        ini += QByteArrayLiteral("trt_engine_settings=") + settings.toUtf8() + '\n';
    }
    if (managerConf.qualitySharp) {
        ini += QByteArrayLiteral("quality_preset=sharp\n");
    }
    if (managerConf.balancedSharp) {
        ini += QByteArrayLiteral("balanced_preset=sharp\n");
    }
    if (managerConf.performanceSharp) {
        ini += QByteArrayLiteral("performance_preset=sharp\n");
    }
    return ini;
}

QByteArray builtinIni(const AnimeJanaiConfig& managerConf) {
    const QByteArray hdBalanced =
        QByteArrayLiteral("2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16");
    const QByteArray hdBalancedSharp =
        QByteArrayLiteral("2x_AnimeJaNai_HD_V3.1Sharp1_Balanced_SPANF3_b8f64_unshuffle_fp16");
    const QByteArray hdPerformance =
        QByteArrayLiteral("2x_AnimeJaNai_HD_V3.1_Performance_SPANF3_b5f48_unshuffle_fp16");
    const QByteArray hdPerformanceSharp =
        QByteArrayLiteral("2x_AnimeJaNai_HD_V3.1Sharp1_Performance_SPANF3_b5f48_unshuffle_fp16");
    const QByteArray sd =
        QByteArrayLiteral("2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo");

    const QByteArray qualityHd = managerConf.qualitySharp ? hdBalancedSharp : hdBalanced;
    const QByteArray balancedHd = managerConf.balancedSharp ? hdBalancedSharp : hdBalanced;
    const QByteArray performanceHd =
        managerConf.performanceSharp ? hdPerformanceSharp : hdPerformance;

    QByteArray ini = globalIniHeader(managerConf);
    ini += QByteArrayLiteral("[slot_1001]\nprofile_name=Quality\n");
    ini += QByteArrayLiteral("chain_1_min_resolution=1280x720\nchain_1_max_resolution=1920x1080\nchain_1_max_fps=31\n") + modelLine(1, qualityHd);
    ini += QByteArrayLiteral("chain_2_min_resolution=0x0\nchain_2_max_resolution=1280x720\nchain_2_max_fps=31\n") + modelLine(2, sd);
    ini += QByteArrayLiteral("chain_3_min_resolution=0x0\nchain_3_max_resolution=1920x1080\nchain_3_max_fps=61\n") + modelLine(3, qualityHd);
    ini += QByteArrayLiteral("[slot_1002]\nprofile_name=Balanced\n");
    ini += QByteArrayLiteral("chain_1_min_resolution=1280x720\nchain_1_max_resolution=1920x1080\nchain_1_max_fps=31\n") + modelLine(1, balancedHd);
    ini += QByteArrayLiteral("chain_2_min_resolution=0x0\nchain_2_max_resolution=1280x720\nchain_2_max_fps=31\n") + modelLine(2, sd);
    ini += QByteArrayLiteral("[slot_1003]\nprofile_name=Performance\n");
    ini += QByteArrayLiteral("chain_1_min_resolution=1280x720\nchain_1_max_resolution=1920x1080\nchain_1_max_fps=31\n") + modelLine(1, performanceHd);
    ini += QByteArrayLiteral("chain_2_min_resolution=0x0\nchain_2_max_resolution=1280x720\nchain_2_max_fps=31\n") + modelLine(2, sd);
    return ini;
}

bool writeIni(const QByteArray& ini, const QString& path, const QString& root, QString* error) {
    if (!pathInside(root, path) ||
        !QDir().mkpath(QFileInfo(path).absolutePath()) || !pathInside(root, path)) {
        *error = RuntimeText::tr("The app-owned AnimeJaNai configuration directory is unavailable or redirected.");
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(ini) != ini.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}

bool retainSpatialLicense(const SpatialAsset& asset, const QString& graphPath,
                          const QString& root, QString* error) {
    const SpatialAssetLicense license = spatialAssetLicense();
    const QJsonObject record{
        {QStringLiteral("source"), asset.url},
        {QStringLiteral("commit"), asset.commit},
        {QStringLiteral("upstreamFile"), asset.name},
        {QStringLiteral("sha256"), asset.sha256},
        {QStringLiteral("license"), license.spdx},
        {QStringLiteral("licenseUrl"), license.url},
        {QStringLiteral("legalCodeUrl"), license.legalCodeUrl},
        {QStringLiteral("notice"), license.notice},
        {QStringLiteral("useScope"), license.useScope}};
    return writeIni(QJsonDocument(record).toJson(), graphPath + QStringLiteral(".source.json"), root, error);
}

bool retainCustomModelProvenance(const CustomAnimeJanaiModel& model, const QString& graphPath,
                                 const QString& root, QString* error) {
    const QJsonObject record{
        {QStringLiteral("source"), QStringLiteral("user-imported")},
        {QStringLiteral("upstreamFile"), model.fileName},
        {QStringLiteral("sha256"), model.sha256},
        {QStringLiteral("scaleFactor"), model.scaleFactor},
        {QStringLiteral("architectureHint"), model.architectureHint}};
    return writeIni(QJsonDocument(record).toJson(), graphPath + QStringLiteral(".source.json"), root, error);
}

QByteArray officialFilterSpec(const QVector<NativeFilterOption>& options, QString* error) {
    QByteArray result = QByteArrayLiteral("@mikai_aji:animejanai");
    char separator = '=';
    for (const NativeFilterOption& option : options) {
        if (option.name.isEmpty() || option.value.contains(QChar::Null)) {
            *error = RuntimeText::tr("An AnimeJaNai filter option cannot be represented safely.");
            return {};
        }
        const QByteArray value = option.value.toUtf8();
        result += separator + option.name.toLatin1() + "=%" + QByteArray::number(value.size()) + '%' + value;
        separator = ':';
    }
    return result;
}

const RifeModelPin* rifeModelPinForFileName(const QString& fileName) {
    for (const RifeModelPin& pin : rifeModelPins()) {
        if (pin.fileName == fileName) return &pin;
    }
    return nullptr;
}

const RifeModelPin* rifeModelPinForCode(int code, bool ensemble) {
    for (const RifeModelPin& pin : rifeModelPins()) {
        if (rifeModelCode(pin.version, pin.variant) == code && pin.ensemble == ensemble) {
            return &pin;
        }
    }
    return nullptr;
}

const SpatialAsset* findSpatialAssetByStemOrId(const QString& candidate,
                                               const QVector<SpatialAsset>& catalog) {
    const QString trimmed = candidate.trimmed();
    if (trimmed.isEmpty()) return nullptr;
    for (const SpatialAsset& asset : catalog) {
        const QString stem = asset.name.endsWith(QStringLiteral(".onnx"), Qt::CaseInsensitive)
                                 ? asset.name.chopped(5)
                                 : asset.name;
        if (asset.id.compare(trimmed, Qt::CaseInsensitive) == 0 ||
            asset.name.compare(trimmed, Qt::CaseInsensitive) == 0 ||
            stem.compare(trimmed, Qt::CaseInsensitive) == 0) {
            return &asset;
        }
    }
    return nullptr;
}

} // namespace

AnimeJanaiStageBuildLease::AnimeJanaiStageBuildLease(QStringList cacheKeys, QString ownerContext)
    : cacheKeys_(std::move(cacheKeys)), ownerContext_(std::move(ownerContext)) {}

AnimeJanaiStageBuildLease::~AnimeJanaiStageBuildLease() {
    release();
}

std::shared_ptr<AnimeJanaiStageBuildLease> AnimeJanaiStageBuildLease::tryAcquire(
    const QStringList& cacheKeys, const QString& ownerContext,
    QString* busyKey, QString* busyOwner) {
    if (busyKey) busyKey->clear();
    if (busyOwner) busyOwner->clear();
    QStringList orderedKeys;
    orderedKeys.reserve(cacheKeys.size());
    for (const QString& key : cacheKeys) {
        if (key.isEmpty()) return {};
        orderedKeys.append(QDir::cleanPath(key));
    }
    std::sort(orderedKeys.begin(), orderedKeys.end());
    orderedKeys.erase(std::unique(orderedKeys.begin(), orderedKeys.end()), orderedKeys.end());
    if (orderedKeys.isEmpty()) return {};

    std::lock_guard<std::mutex> lock(animeJanaiStageCacheMutex());
    QHash<QString, QString>& active = activeAnimeJanaiStageCaches();
    for (const QString& key : orderedKeys) {
        const auto owner = active.constFind(key);
        if (owner != active.cend()) {
            if (busyKey) *busyKey = key;
            if (busyOwner) *busyOwner = owner.value();
            return {};
        }
    }
    const QString context = ownerContext.trimmed().isEmpty()
                                ? QStringLiteral("another AnimeJaNai pipeline")
                                : ownerContext.trimmed();
    for (const QString& key : orderedKeys) active.insert(key, context);
    return std::shared_ptr<AnimeJanaiStageBuildLease>(
        new AnimeJanaiStageBuildLease(std::move(orderedKeys), context));
}

bool AnimeJanaiStageBuildLease::held() const {
    std::lock_guard<std::mutex> lock(animeJanaiStageCacheMutex());
    return held_;
}

void AnimeJanaiStageBuildLease::release() {
    std::lock_guard<std::mutex> lock(animeJanaiStageCacheMutex());
    if (!held_) return;
    QHash<QString, QString>& active = activeAnimeJanaiStageCaches();
    for (const QString& key : cacheKeys_) {
        const auto owner = active.find(key);
        if (owner != active.end() && owner.value() == ownerContext_) active.erase(owner);
    }
    held_ = false;
}

QVector<AnimeJanaiPreset> animeJanaiPresets() {
    QVector<AnimeJanaiPreset> presets;
    const QVector<SpatialAsset> spatial = spatialAssetCatalog();
    const QVector<CustomAnimeJanaiModel> customSpatial = listCustomAnimeJanaiModels();
    const AnimeJanaiConfig managerConf = AnimeJanaiConfigStore::load();
    const QVector<RifeModelPin>& rife = rifeModelPins();

    presets.reserve(3 + managerConf.customSlots.size() + spatial.size() + customSpatial.size() +
                    rife.size());
    presets.append({QStringLiteral("builtin1001"), RuntimeText::tr("Quality"), 1001, false, {}, true, {}});
    presets.append({QStringLiteral("builtin1002"), RuntimeText::tr("Balanced"), 1002, false, {}, true, {}});
    presets.append({QStringLiteral("builtin1003"), RuntimeText::tr("Performance"), 1003, false, {}, true, {}});

    for (const AnimeJanaiSlot& slot : managerConf.customSlots) {
        if (slot.number <= 0 || slot.chains.isEmpty()) continue;
        const QString profileTitle =
            slot.profileName.trimmed().isEmpty()
                ? RuntimeText::tr("Slot %1").arg(slot.number)
                : slot.profileName.trimmed();
        presets.append({QStringLiteral("builtin_slot:%1").arg(slot.number),
                        profileTitle,
                        slot.number,
                        false,
                        {},
                        true,
                        slot.descriptionText});
    }

    for (qsizetype i = 0; i < spatial.size(); ++i) {
        const SpatialAsset& asset = spatial.at(i);
        presets.append({QStringLiteral("spatial:") + asset.id, asset.name, 2001 + static_cast<int>(i), false,
                        asset.name, true, {}});
    }
    for (qsizetype i = 0; i < customSpatial.size(); ++i) {
        const CustomAnimeJanaiModel& model = customSpatial.at(i);
        presets.append({QStringLiteral("spatial:custom:") + model.id,
                        model.displayName,
                        2501 + static_cast<int>(i),
                        false,
                        model.fileName,
                        model.valid,
                        model.reason});
    }
    for (qsizetype i = 0; i < rife.size(); ++i) {
        const RifeModelPin& pin = rife.at(i);
        presets.append({QStringLiteral("rife:") + pin.fileName, pin.fileName, 2101 + static_cast<int>(i), true,
                        pin.fileName, true, {}});
    }
    return presets;
}

namespace {
AnimeJanaiPlan prepareMixedProfile(const QString& presetId, Backend backend,
                                   const std::function<bool()>& cancelled,
                                   const AnimeJanaiSourceContext& source,
                                   const Anime4KShaderProgramResolver& shaderResolver,
                                   const AnimeJanaiConfig& managerConf,
                                   const AnimeJanaiSlot& profile) {
    AnimeJanaiPlan plan;
    plan.enabled = true;
    plan.presetId = presetId;
    plan.backend = backend;
    plan.slot = profile.number;
    plan.requiresAji = false;
    QString validationError;
    if (!AnimeJanaiConfigStore::validateAnimeJanaiConfig(managerConf, &validationError)) {
        fail(plan, QStringLiteral("animejanai.config.invalid"), validationError);
        return plan;
    }
    QVector<AnimeJanaiChain> chains = profile.chains;
    std::sort(chains.begin(), chains.end(), [](const AnimeJanaiChain& a, const AnimeJanaiChain& b) {
        return a.number < b.number;
    });
    if (source.width <= 0 || source.height <= 0 || !std::isfinite(source.fps) || source.fps <= 0.0) {
        plan.needsSourceContext = true;
        plan.reasonKey = QStringLiteral("animejanai.source.required");
        plan.reason = RuntimeText::tr("AnimeJaNai mixed chains require the original source dimensions and frame rate.");
        return plan;
    }
    const auto parseBound = [](const QString& text, qint64* area, bool* unbounded) {
        const QString value = text.trimmed().toLower();
        if (value == QStringLiteral("infxinf")) { *unbounded = true; *area = 0; return true; }
        const qsizetype x = value.indexOf(QLatin1Char('x'));
        if (x <= 0) return false;
        bool widthOk = false;
        bool heightOk = false;
        const qint64 width = value.left(x).toLongLong(&widthOk);
        const qint64 height = value.mid(x + 1).toLongLong(&heightOk);
        if (!widthOk || !heightOk || width < 0 || height < 0 ||
            (height != 0 && width > std::numeric_limits<qint64>::max() / height)) return false;
        *unbounded = width == 0 || height == 0;
        *area = *unbounded ? 0 : width * height;
        return true;
    };
    const qint64 sourceArea = qint64(source.width) * source.height;
    const AnimeJanaiChain* selected = nullptr;
    for (const AnimeJanaiChain& chain : chains) {
        qint64 minimumArea = 0;
        qint64 maximumArea = 0;
        bool minimumUnbounded = false;
        bool maximumUnbounded = false;
        if (!parseBound(chain.minResolution, &minimumArea, &minimumUnbounded) ||
            !parseBound(chain.maxResolution, &maximumArea, &maximumUnbounded)) {
            fail(plan, QStringLiteral("animejanai.profile.bounds"), RuntimeText::tr("A mixed AnimeJaNai chain has invalid source bounds."));
            return plan;
        }
        Q_UNUSED(minimumUnbounded);
        const bool areaMatches = sourceArea >= minimumArea &&
                                 (maximumUnbounded || sourceArea <= maximumArea);
        const bool fpsMatches = source.fps >= chain.minFps &&
                               (chain.maxFps <= 0 || source.fps <= chain.maxFps);
        if (areaMatches && fpsMatches) { selected = &chain; break; }
    }
    if (!selected) {
        fail(plan, QStringLiteral("animejanai.profile.noChain"),
             RuntimeText::tr("No AnimeJaNai chain matches the original source dimensions and frame rate."));
        return plan;
    }
    plan.selectedChainNumber = selected->number;
    const QString ownedRoot = configRoot();
    AnimeJanaiSlot selectedProfile;
    selectedProfile.number = profile.number;
    selectedProfile.profileName = profile.profileName;
    selectedProfile.descriptionText = profile.descriptionText;
    selectedProfile.chains.append(*selected);
    const QByteArray serializedSelection =
        AnimeJanaiConfigStore::serializeSlotSection(selectedProfile, 1).toUtf8();
    const QString profileKey = QString::fromLatin1(
        QCryptographicHash::hash(presetId.toUtf8() + QByteArrayLiteral("\n") + serializedSelection,
                                 QCryptographicHash::Sha256).toHex().left(24));

    const bool hasAji = selected->rifeEnabled ||
                        std::any_of(selected->steps.cbegin(), selected->steps.cend(),
                                    [](const AnimeJanaiChainStep& step) {
                                        return step.kind == AnimeJanaiStepKind::OnnxModel;
                                    });
    plan.requiresAji = hasAji;
    if (hasAji) {
        const QString cacheKey = QDir(ownedRoot).filePath(
            QStringLiteral("models/mixed_%1_cache").arg(profileKey));
        plan.cacheKeys.append(QDir::cleanPath(QFileInfo(cacheKey).absoluteFilePath()));
        QString busyKey;
        QString busyOwner;
        plan.cacheLease = AnimeJanaiStageBuildLease::tryAcquire(
            plan.cacheKeys, QStringLiteral("AnimeJaNai %1 chain %2")
                                .arg(profile.profileName).arg(selected->number),
            &busyKey, &busyOwner);
        if (!plan.cacheLease) {
            fail(plan, QStringLiteral("animejanai.cache.busy"),
                 RuntimeText::tr("The AnimeJaNai engine cache is currently in use by %1.")
                     .arg(busyOwner.isEmpty() ? RuntimeText::tr("another operation") : busyOwner));
            return plan;
        }
    }
    AnimeJanaiInstallation installation;
    if (hasAji) {
        installation = describeAnimeJanaiInstallation(backend, true);
        if (!installation.installed) {
            fail(plan, installation.reasonKey, installation.reason);
            return plan;
        }
    }

    const RifeModelPin* rifePin = nullptr;
    if (selected->rifeEnabled) {
        const int code = AnimeJanaiConfigStore::rifeLabelToValue(selected->rifeModel).toInt();
        rifePin = rifeModelPinForCode(code, selected->rifeEnsemble);
        if (!rifePin) {
            fail(plan, QStringLiteral("animejanai.model.pinMissing"),
                 RuntimeText::tr("The selected RIFE ensemble has no compiled content pin."));
            return plan;
        }
        if (rifePin->variant == RifeVariant::Heavy && !installation.heavyRifeSupported) {
            fail(plan, QStringLiteral("animejanai.preset.rifeHeavyUnsupported"),
                 RuntimeText::tr("This installed AnimeJaNai runtime does not report support for the selected heavy RIFE model."));
            return plan;
        }
    }
    if (backend == Backend::TensorRT && hasAji) {
        const QString winTrtexec = QDir(installation.root).filePath(QStringLiteral("trtexec.exe"));
        const QString linuxTrtexec = QDir(installation.root).filePath(QStringLiteral("trtexec"));
        plan.trtexecPath = regularFile(winTrtexec) ? winTrtexec : linuxTrtexec;
        if (!regularFile(plan.trtexecPath)) {
            fail(plan, QStringLiteral("animejanai.runtime.trtexecMissing"),
                 RuntimeText::tr("The installed TensorRT closure does not include its verified upstream trtexec executable."));
            return plan;
        }
    }
    if (hasAji) {
        const QString linuxLibPath = QDir(installation.root).filePath(QStringLiteral("libaji.so"));
        plan.libraryPath = regularFile(linuxLibPath) ? linuxLibPath
                                                    : QDir(installation.root).filePath(QStringLiteral("aji.dll"));
        plan.trtexecLibDir = installation.root;
    }

    struct SpatialItem { AnimeJanaiChainStep step; QString graph; QString digest; qint64 bytes; const SpatialAsset* pinned; std::optional<CustomAnimeJanaiModel> custom; };
    QVector<SpatialItem> spatialItems;
    QMap<int, AnimeJanaiPlanStage> shaderStages;
    const QVector<SpatialAsset> catalog = spatialAssetCatalog();
    for (const AnimeJanaiChainStep& step : selected->steps) {
        if (cancelled && cancelled()) {
            fail(plan, QStringLiteral("animejanai.prepare.cancelled"),
                 RuntimeText::tr("AnimeJaNai preparation was cancelled."));
            return plan;
        }
        if (step.kind == AnimeJanaiStepKind::Unsupported) {
            fail(plan, QStringLiteral("animejanai.profile.stepUnsupported"),
                 RuntimeText::tr("A mixed AnimeJaNai chain contains an unsupported step type."));
            return plan;
        }
        if (step.kind == AnimeJanaiStepKind::Anime4K) {
            if (step.scaleFactor != 1 && step.scaleFactor != 2 && step.scaleFactor != 4) {
                fail(plan, QStringLiteral("animejanai.profile.scaleInvalid"),
                     RuntimeText::tr("Anime4K output scale must be 1, 2, or 4."));
                return plan;
            }
            if (!shaderResolver) {
                fail(plan, QStringLiteral("animejanai.shader.resolverMissing"),
                     RuntimeText::tr("No Anime4K shader resolver is available."));
                return plan;
            }
            QString reason;
            const QByteArray program = shaderResolver(step.id, &reason);
            if (cancelled && cancelled()) {
                fail(plan, QStringLiteral("animejanai.prepare.cancelled"),
                     RuntimeText::tr("AnimeJaNai preparation was cancelled."));
                return plan;
            }
            if (program.isEmpty()) {
                fail(plan, QStringLiteral("animejanai.shader.missing"),
                     reason.isEmpty() ? RuntimeText::tr("The selected Anime4K shader program is missing or invalid.") : reason);
                return plan;
            }
            const QString shaderRoot = QDir(ownedRoot).filePath(QStringLiteral("shaders/") + profileKey);
            const QString shaderPath = QDir(shaderRoot).filePath(QStringLiteral("stage_%1.glsl").arg(step.number));
            if (!pathInside(ownedRoot, shaderPath) ||
                !QDir().mkpath(QFileInfo(shaderPath).absolutePath()) || !pathInside(ownedRoot, shaderPath)) {
                fail(plan, QStringLiteral("animejanai.shader.directory"),
                     RuntimeText::tr("The app-owned AnimeJaNai shader directory is unavailable or redirected."));
                return plan;
            }
            if (QFileInfo::exists(shaderPath)) {
                QFile old(shaderPath);
                if (!regularFile(shaderPath) || !old.open(QIODevice::ReadOnly) || old.readAll() != program) {
                    fail(plan, QStringLiteral("animejanai.shader.ownership"),
                         RuntimeText::tr("An existing staged Anime4K shader is not the expected immutable app-owned program."));
                    return plan;
                }
            } else {
                QSaveFile output(shaderPath);
                output.setDirectWriteFallback(false);
                if (!output.open(QIODevice::WriteOnly) || output.write(program) != program.size() || !output.commit()) {
                    fail(plan, QStringLiteral("animejanai.shader.write"), output.errorString());
                    return plan;
                }
            }
            const QString escapedPath = shaderPath;
            QByteArray spec = QByteArrayLiteral("@mikai_a4k_download_") +
                              QByteArray::number(step.number) +
                              QByteArrayLiteral(":format=fmt=nv12,");
            if (step.resizeHeightBeforeUpscale > 0 || step.resizeFactorBeforeUpscale != 100) {
                QByteArray resize = QByteArrayLiteral("@mikai_a4k_resize_") + QByteArray::number(step.number) +
                                    QByteArrayLiteral(":libplacebo=");
                if (step.resizeHeightBeforeUpscale > 0) {
                    const QByteArray height =
                        QByteArray::number(step.resizeHeightBeforeUpscale) + '*' +
                        QByteArray::number(step.resizeFactorBeforeUpscale) +
                        QByteArrayLiteral("/100");
                    resize += QByteArrayLiteral("h=") + height +
                              QByteArrayLiteral(":w=iw*(") + height + QByteArrayLiteral(")/ih");
                } else {
                    resize += QByteArrayLiteral("w=iw*") + QByteArray::number(step.resizeFactorBeforeUpscale) +
                              QByteArrayLiteral("/100:h=ih*") + QByteArray::number(step.resizeFactorBeforeUpscale) +
                              QByteArrayLiteral("/100");
                }
                resize += QByteArrayLiteral(":format=nv12");
                spec += resize + ',';
            }
            spec += QByteArrayLiteral("@mikai_a4k_") + QByteArray::number(step.number) +
                    QByteArrayLiteral(":libplacebo=custom_shader_path=%") +
                    QByteArray::number(escapedPath.toUtf8().size()) + '%' + escapedPath.toUtf8() +
                    QByteArrayLiteral(":w=iw*") + QByteArray::number(step.scaleFactor) +
                    QByteArrayLiteral(":h=ih*") + QByteArray::number(step.scaleFactor) +
                    QByteArrayLiteral(":format=nv12");
            AnimeJanaiPlanStage stage;
            stage.kind = AnimeJanaiPlanStage::Kind::Anime4K;
            stage.filterLabel = QStringLiteral("mikai_a4k_%1").arg(step.number);
            stage.filterSpec = spec;
            stage.firstStepNumber = step.number;
            stage.lastStepNumber = step.number;
            stage.presetId = step.id;
            stage.scaleFactor = step.scaleFactor;
            stage.cacheLease = plan.cacheLease;
            shaderStages.insert(step.number, stage);
            continue;
        }
        if (step.kind != AnimeJanaiStepKind::OnnxModel || step.id.trimmed().isEmpty()) {
            fail(plan, QStringLiteral("animejanai.profile.modelEmpty"),
                 RuntimeText::tr("A mixed AnimeJaNai ONNX step has no model reference."));
            return plan;
        }
        const SpatialAsset* pinned = findSpatialAssetByStemOrId(step.id, catalog);
        if (pinned) {
            const auto stored = storedSpatialAssets();
            const auto item = std::find_if(stored.cbegin(), stored.cend(),
                                            [pinned](const StoredSpatialAsset& row) { return row.asset.id == pinned->id; });
            if (item == stored.cend() || !item->verified) {
                fail(plan, QStringLiteral("animejanai.model.missing"),
                     item == stored.cend() ? RuntimeText::tr("A pinned spatial model referenced by the mixed chain is not installed.") : item->reason);
                return plan;
            }
            spatialItems.append({step, item->graphPath, pinned->sha256, pinned->bytes, pinned, std::nullopt});
        } else {
            const auto custom = findCustomAnimeJanaiModel(step.id);
            if (!custom || !custom->valid) {
                fail(plan, QStringLiteral("animejanai.model.missing"),
                     RuntimeText::tr("The mixed chain references ONNX model '%1', which is missing or invalid.").arg(step.id));
                return plan;
            }
            spatialItems.append({step, custom->filePath, custom->sha256, custom->bytes, nullptr, custom});
        }
    }

    const auto appendAji = [&](const QVector<SpatialItem>& items, bool includeRife, int stageNumber,
                               int firstStep, int lastStep, AnimeJanaiPlan* target) -> bool {
        if (items.isEmpty() && !includeRife) return true;
        QByteArray cacheIdentity;
        for (const SpatialItem& item : items) {
            cacheIdentity += item.digest.toUtf8() + ':' +
                             QByteArray::number(item.step.number) + ':' +
                             QByteArray::number(item.step.resizeHeightBeforeUpscale) + ':' +
                             QByteArray::number(item.step.resizeFactorBeforeUpscale) +
                             QByteArrayLiteral("\n");
        }
        if (includeRife && rifePin) {
            cacheIdentity += rifePin->graphSha256.toUtf8() + ':' +
                             QByteArray::number(selected->rifeFactorNumerator) + ':' +
                             QByteArray::number(selected->rifeFactorDenominator) + ':' +
                             QByteArray::number(selected->rifeEnsemble) + ':' +
                             QByteArray::number(selected->rifeSceneDetectThreshold, 'g', 17);
        }
        const QString stageKey = QStringLiteral("mixed_%1_s%2_%3")
                                     .arg(profileKey)
                                     .arg(stageNumber)
                                     .arg(QString::fromLatin1(QCryptographicHash::hash(
                                         cacheIdentity, QCryptographicHash::Sha256).toHex().left(16)));
        const QString modelDir = QDir(ownedRoot).filePath(QStringLiteral("models/") + stageKey);
        const QString rifeModelDir = QDir(ownedRoot).filePath(QStringLiteral("models/") + stageKey + QStringLiteral("_rife"));
        const QString configPath = QDir(ownedRoot).filePath(QStringLiteral("config/") + stageKey + QStringLiteral(".ini"));
        const QString statsPath = QDir(ownedRoot).filePath(QStringLiteral("stats/") + stageKey + QStringLiteral(".txt"));
        if (!pathInside(ownedRoot, modelDir) ||
            !QDir().mkpath(modelDir) || !pathInside(ownedRoot, modelDir)) {
            fail(*target, QStringLiteral("animejanai.model.directory"),
                 RuntimeText::tr("The app-owned per-stage AnimeJaNai model directory is unavailable or redirected."));
            return false;
        }
        QStringList models;
        int modelNumber = 0;
        for (const SpatialItem& item : items) {
            const QString targetPath = QDir(modelDir).filePath(item.digest + QStringLiteral(".onnx"));
            QString copyError;
            if (!copyVerified(item.graph, targetPath, item.bytes, item.digest, ownedRoot, cancelled, &copyError)) {
                fail(*target, QStringLiteral("animejanai.model.stage"), copyError);
                return false;
            }
            if (item.pinned && !retainSpatialLicense(*item.pinned, targetPath, ownedRoot, &copyError)) {
                fail(*target, QStringLiteral("animejanai.model.provenance"), copyError);
                return false;
            }
            if (item.custom && !retainCustomModelProvenance(*item.custom, targetPath, ownedRoot, &copyError)) {
                fail(*target, QStringLiteral("animejanai.model.provenance"), copyError);
                return false;
            }
            ++modelNumber;
            models.append(QStringLiteral("chain_1_model_%1_name=%2\n").arg(modelNumber).arg(item.digest));
            if (item.step.resizeHeightBeforeUpscale > 0)
                models.append(QStringLiteral("chain_1_model_%1_resize_height_before_upscale=%2\n").arg(modelNumber).arg(item.step.resizeHeightBeforeUpscale));
            if (item.step.resizeFactorBeforeUpscale != 100)
                models.append(QStringLiteral("chain_1_model_%1_resize_factor_before_upscale=%2\n").arg(modelNumber).arg(item.step.resizeFactorBeforeUpscale));
        }
        QString rifeConfig;
        if (includeRife) {
            const StoredRifeGraph stored = storedRifeGraph(rifeArchiveStore(), *rifePin);
            if (!stored.installed) {
                fail(*target, QStringLiteral("animejanai.model.missing"),
                     stored.reason.isEmpty() ? RuntimeText::tr("The selected pinned RIFE graph is missing or corrupt.") : stored.reason);
                return false;
            }
            QString copyError;
            if (!pathInside(ownedRoot, rifeModelDir) ||
                !QDir().mkpath(rifeModelDir) || !pathInside(ownedRoot, rifeModelDir)) {
                fail(*target, QStringLiteral("animejanai.model.directory"),
                     RuntimeText::tr("The app-owned per-stage RIFE model directory is unavailable or redirected."));
                return false;
            }
            const QString targetPath = QDir(rifeModelDir).filePath(rifePin->fileName);
            if (!copyVerified(stored.path, targetPath, rifePin->bytes, rifePin->graphSha256,
                              ownedRoot, cancelled, &copyError)) {
                fail(*target, QStringLiteral("animejanai.model.stage"), copyError);
                return false;
            }
            const QJsonObject provenance{
                {QStringLiteral("source"), QStringLiteral("https://github.com/the-database/mpv-AnimeJaNai/releases/tag/3.7.0")},
                {QStringLiteral("upstreamFile"), rifePin->fileName},
                {QStringLiteral("sha256"), rifePin->graphSha256},
                {QStringLiteral("terms"), QStringLiteral("upstream terms not verified")}};
            if (!writeIni(QJsonDocument(provenance).toJson(),
                          targetPath + QStringLiteral(".source.json"), ownedRoot, &copyError)) {
                fail(*target, QStringLiteral("animejanai.model.provenance"), copyError);
                return false;
            }
            const int code = rifeModelCode(rifePin->version, rifePin->variant);
            rifeConfig = QStringLiteral("chain_1_rife=yes\nchain_1_rife_factor_numerator=%1\nchain_1_rife_factor_denominator=%2\nchain_1_rife_model=%3\nchain_1_rife_ensemble=%4\nchain_1_rife_scene_detect_threshold=%5\nchain_1_rife_before_upscale=%6\n")
                             .arg(selected->rifeFactorNumerator).arg(selected->rifeFactorDenominator)
                             .arg(code).arg(selected->rifeEnsemble ? QStringLiteral("yes") : QStringLiteral("no"))
                             .arg(QLocale::c().toString(selected->rifeSceneDetectThreshold, 'g', 6))
                             .arg(selected->rifeBeforeUpscale ? QStringLiteral("yes") : QStringLiteral("no"));
        }
        const int generatedSlot = 1;
        QByteArray ini = globalIniHeader(managerConf);
        ini += QByteArrayLiteral("[slot_1]\nprofile_name=Mixed chain segment\n");
        ini += QByteArrayLiteral("chain_1_min_resolution=0x0\nchain_1_max_resolution=0x0\nchain_1_min_fps=0\nchain_1_max_fps=0\n");
        ini += models.join(QString()).toUtf8();
        ini += rifeConfig.toUtf8();
        if (backend == Backend::DirectML)
            ini.replace(QByteArrayLiteral("backend=TensorRT"), QByteArrayLiteral("backend=DirectML"));
        QString error;
        if (!writeIni(ini, configPath, ownedRoot, &error) ||
            !pathInside(ownedRoot, statsPath) ||
            !QDir().mkpath(QFileInfo(statsPath).absolutePath()) || !pathInside(ownedRoot, statsPath)) {
            fail(*target, QStringLiteral("animejanai.config.write"),
                 error.isEmpty() ? RuntimeText::tr("The app-owned AnimeJaNai stats directory is unavailable or redirected.") : error);
            return false;
        }
        QVector<NativeFilterOption> options{
            {QStringLiteral("lib"), plan.libraryPath},
            {QStringLiteral("conf"), configPath},
            {QStringLiteral("model-dir"), modelDir},
            {QStringLiteral("rife-model-dir"), rifeModelDir},
            {QStringLiteral("trtexec"), plan.trtexecPath},
            {QStringLiteral("trtexec-libdir"), plan.trtexecLibDir},
            {QStringLiteral("slot"), QString::number(generatedSlot)},
            {QStringLiteral("stats"), statsPath},
            {QStringLiteral("output-444"), QStringLiteral("no")},
            {QStringLiteral("passthrough"), QStringLiteral("no")}};
        QString filterError;
        const QByteArray filter = officialFilterSpec(options, &filterError);
        if (filter.isEmpty()) {
            fail(*target, QStringLiteral("animejanai.filter.spec"), filterError);
            return false;
        }
        AnimeJanaiPlanStage stage;
        stage.kind = AnimeJanaiPlanStage::Kind::Aji;
        stage.filterLabel = target->stages.isEmpty()
                                ? QString::fromLatin1(kAnimeJanaiFilterLabel)
                                : QStringLiteral("mikai_aji_%1").arg(stageNumber);
        stage.filterSpec = '@' + stage.filterLabel.toUtf8() + filter.mid(filter.indexOf(':'));
        stage.configPath = configPath;
        stage.statsPath = statsPath;
        stage.firstStepNumber = firstStep;
        stage.lastStepNumber = lastStep;
        stage.presetId = presetId;
        stage.cacheLease = target->cacheLease;
        target->stages.append(stage);
        return true;
    };

    if (!hasAji && selected->steps.isEmpty()) {
        fail(plan, QStringLiteral("animejanai.profile.chainEmpty"),
             RuntimeText::tr("The selected AnimeJaNai chain has no processing steps."));
        return plan;
    }
    int stageNumber = 0;
    const bool rifeBefore = selected->rifeEnabled && selected->rifeBeforeUpscale;
    bool rifePlaced = false;
    QVector<SpatialItem> run;
    int firstStep = 0;
    int lastStep = 0;
    const auto flushRun = [&](bool attachRife) -> bool {
        if (run.isEmpty() && !attachRife) return true;
        ++stageNumber;
        if (!appendAji(run, attachRife, stageNumber, firstStep, lastStep, &plan)) return false;
        run.clear();
        firstStep = 0;
        lastStep = 0;
        if (attachRife) rifePlaced = true;
        return true;
    };
    if (rifeBefore && !selected->steps.isEmpty() &&
        selected->steps.first().kind == AnimeJanaiStepKind::Anime4K) {
        if (!flushRun(true)) return plan;
    }
    for (const AnimeJanaiChainStep& step : selected->steps) {
        if (step.kind == AnimeJanaiStepKind::OnnxModel) {
            const auto item = std::find_if(spatialItems.cbegin(), spatialItems.cend(),
                                           [&step](const SpatialItem& entry) {
                                               return entry.step.number == step.number;
                                           });
            if (item == spatialItems.cend()) {
                fail(plan, QStringLiteral("animejanai.model.missing"),
                     RuntimeText::tr("A mixed chain ONNX step could not be staged."));
                return plan;
            }
            if (run.isEmpty()) firstStep = step.number;
            lastStep = step.number;
            run.append(*item);
            continue;
        }
        if (step.kind == AnimeJanaiStepKind::Anime4K) {
            if (!flushRun(rifeBefore && !rifePlaced)) return plan;
            const auto shader = shaderStages.constFind(step.number);
            if (shader == shaderStages.cend()) {
                fail(plan, QStringLiteral("animejanai.shader.missing"),
                     RuntimeText::tr("The selected Anime4K shader program was not staged."));
                return plan;
            }
            plan.stages.append(shader.value());
        }
    }
    const bool attachAfter = selected->rifeEnabled && !selected->rifeBeforeUpscale &&
                             !selected->steps.isEmpty() &&
                             selected->steps.last().kind == AnimeJanaiStepKind::OnnxModel;
    if (!flushRun(attachAfter || (rifeBefore && !rifePlaced))) return plan;
    if (selected->rifeEnabled && !rifePlaced) {
        if (!flushRun(true)) return plan;
    }
    if (plan.stages.isEmpty()) {
        fail(plan, QStringLiteral("animejanai.profile.chainEmpty"),
             RuntimeText::tr("The selected AnimeJaNai chain has no processing steps."));
        return plan;
    }
    QByteArray pipeline;
    for (const AnimeJanaiPlanStage& stage : plan.stages) {
        if (!pipeline.isEmpty()) pipeline += ',';
        pipeline += stage.filterSpec;
    }
    plan.filterSpec = pipeline;
    const auto firstAji = std::find_if(plan.stages.cbegin(), plan.stages.cend(),
                                       [](const AnimeJanaiPlanStage& stage) {
                                           return stage.kind == AnimeJanaiPlanStage::Kind::Aji;
                                       });
    if (firstAji != plan.stages.cend()) {
        plan.slot = 1;
        plan.configPath = firstAji->configPath;
        plan.statsPath = firstAji->statsPath;
        const QString stageBase = QFileInfo(firstAji->configPath).completeBaseName();
        plan.modelDir = QDir(ownedRoot).filePath(QStringLiteral("models/") + stageBase);
        plan.rifeModelDir = QDir(ownedRoot).filePath(QStringLiteral("models/") + stageBase +
                                                   QStringLiteral("_rife"));
    }
    plan.available = true;
    return plan;
}
}  // namespace

AnimeJanaiPlan prepareAnimeJanaiPlan(const QString& presetId, Backend backend,
                                    const std::function<bool()>& cancelled,
                                    const AnimeJanaiSourceContext& source,
                                    const Anime4KShaderProgramResolver& shaderResolver) {
    AnimeJanaiPlan plan;
    plan.backend = backend;
    if (presetId.isEmpty()) return plan;
    plan.enabled = true;
    plan.presetId = presetId;
    const auto stop = [&] { return cancelled && cancelled(); };
    if (stop()) { fail(plan, QStringLiteral("animejanai.prepare.cancelled"), RuntimeText::tr("AnimeJaNai preparation was cancelled.")); return plan; }
    if (presetId.startsWith(QStringLiteral("builtin_slot:"))) {
        QString managerError;
        const AnimeJanaiConfig earlyConfig = AnimeJanaiConfigStore::load(&managerError);
        if (!managerError.isEmpty()) {
            fail(plan, QStringLiteral("animejanai.config.invalid"), managerError);
            return plan;
        }
        bool slotOk = false;
        const int slotNumber = presetId.mid(QStringLiteral("builtin_slot:").size()).toInt(&slotOk);
        const auto slot = std::find_if(earlyConfig.customSlots.cbegin(), earlyConfig.customSlots.cend(),
                                       [slotNumber](const AnimeJanaiSlot& candidate) {
                                           return candidate.number == slotNumber;
                                       });
        if (slotOk && slot != earlyConfig.customSlots.cend()) {
            const bool hasAnime4K = std::any_of(slot->chains.cbegin(), slot->chains.cend(),
                                                [](const AnimeJanaiChain& chain) {
                                                    return std::any_of(chain.steps.cbegin(), chain.steps.cend(),
                                                                       [](const AnimeJanaiChainStep& step) {
                                                                           return step.kind == AnimeJanaiStepKind::Anime4K;
                                                                       });
                                                });
            if (hasAnime4K)
                return prepareMixedProfile(presetId, backend, cancelled, source,
                                           shaderResolver, earlyConfig, *slot);
        }
    }
    const QVector<AnimeJanaiPreset> presets = animeJanaiPresets();
    const AnimeJanaiPreset* preset = findPreset(presetId, presets);
    if (!preset) { fail(plan, QStringLiteral("animejanai.preset.invalid"), RuntimeText::tr("The selected AnimeJaNai preset is not recognised.")); return plan; }

    const AnimeJanaiInstallation installation = describeAnimeJanaiInstallation(backend, true);
    if (!installation.installed) { fail(plan, installation.reasonKey, installation.reason); return plan; }
    const RifeModelPin* selectedRife = preset->rife ? rifeModelPinForFileName(preset->modelFile) : nullptr;
    if (preset->rife && !selectedRife) {
        fail(plan, QStringLiteral("animejanai.model.pinMissing"), RuntimeText::tr("The selected RIFE graph has no compiled content pin."));
        return plan;
    }
    if (selectedRife && selectedRife->variant == RifeVariant::Heavy && !installation.heavyRifeSupported) {
        fail(plan, QStringLiteral("animejanai.preset.rifeHeavyUnsupported"),
             RuntimeText::tr("This installed AnimeJaNai runtime does not report support for the selected heavy RIFE model."));
        return plan;
    }

    const QString ownedRoot = configRoot();
    const QString linuxLibPath = QDir(installation.root).filePath(QStringLiteral("libaji.so"));
    plan.libraryPath = regularFile(linuxLibPath)
                           ? linuxLibPath
                           : QDir(installation.root).filePath(QStringLiteral("aji.dll"));
    plan.configPath = QDir(ownedRoot).filePath(QStringLiteral("config/") + safeConfigName(presetId) + QStringLiteral(".ini"));
    plan.modelDir = QDir(ownedRoot).filePath(QStringLiteral("models/spatial"));
    plan.rifeModelDir = QDir(ownedRoot).filePath(QStringLiteral("models/rife"));
    plan.statsPath = QDir(ownedRoot).filePath(QStringLiteral("stats/") + safeConfigName(presetId) + QStringLiteral(".txt"));
    plan.slot = preset->slot;
    plan.trtexecLibDir = installation.root;
    plan.cacheKeys = {
        QDir::cleanPath(QFileInfo(plan.modelDir).absoluteFilePath()),
        QDir::cleanPath(QFileInfo(plan.rifeModelDir).absoluteFilePath())};
    QString busyKey;
    QString busyOwner;
    plan.cacheLease = AnimeJanaiStageBuildLease::tryAcquire(
        plan.cacheKeys, QStringLiteral("AnimeJaNai %1").arg(preset->label),
        &busyKey, &busyOwner);
    if (!plan.cacheLease) {
        fail(plan, QStringLiteral("animejanai.cache.busy"),
             RuntimeText::tr("The AnimeJaNai engine cache is currently in use by %1.")
                 .arg(busyOwner.isEmpty() ? RuntimeText::tr("another operation") : busyOwner));
        return plan;
    }
    if (backend == Backend::TensorRT) {
        const QString winTrtexec = QDir(installation.root).filePath(QStringLiteral("trtexec.exe"));
        const QString linuxTrtexec = QDir(installation.root).filePath(QStringLiteral("trtexec"));
        plan.trtexecPath = regularFile(winTrtexec) ? winTrtexec : linuxTrtexec;
        if (!regularFile(plan.trtexecPath)) {
            fail(plan, QStringLiteral("animejanai.runtime.trtexecMissing"),
                 RuntimeText::tr("The installed TensorRT closure does not include its verified upstream trtexec executable."));
            return plan;
        }
    }

    QString managerError;
    const AnimeJanaiConfig managerConf = AnimeJanaiConfigStore::load(&managerError);
    if (!managerError.isEmpty()) {
        fail(plan, QStringLiteral("animejanai.config.invalid"), managerError);
        return plan;
    }
    QString ini;
    if (presetId.startsWith(QStringLiteral("builtin_slot:"))) {
        bool slotOk = false;
        const int slotNum = presetId.mid(QStringLiteral("builtin_slot:").size()).toInt(&slotOk);
        const auto slotIt = std::find_if(
            managerConf.customSlots.cbegin(), managerConf.customSlots.cend(),
            [slotNum](const AnimeJanaiSlot& candidate) { return candidate.number == slotNum; });
        if (!slotOk || slotIt == managerConf.customSlots.cend() || slotIt->chains.isEmpty()) {
            fail(plan, QStringLiteral("animejanai.preset.invalid"),
                 RuntimeText::tr("The selected custom AnimeJaNai profile slot is empty or invalid."));
            return plan;
        }

        AnimeJanaiSlot stagedSlot = *slotIt;
        const auto pinnedCatalog = spatialAssetCatalog();
        const auto storedPinned = storedSpatialAssets();
        QSet<QString> stagedSpatialDigests;
        QSet<QString> stagedRifeFiles;

        for (AnimeJanaiChain& chain : stagedSlot.chains) {
            if (chain.steps.isEmpty() && !chain.rifeEnabled) {
                fail(plan, QStringLiteral("animejanai.profile.chainEmpty"),
                     RuntimeText::tr("Chain %1 in custom profile '%2' has no model or RIFE step.")
                         .arg(chain.number).arg(stagedSlot.profileName));
                return plan;
            }
            for (AnimeJanaiChainStep& modelStep : chain.steps) {
                if (modelStep.kind != AnimeJanaiStepKind::OnnxModel) {
                    fail(plan, QStringLiteral("animejanai.profile.stepUnsupported"),
                         RuntimeText::tr("A model-only AnimeJaNai profile contains an unsupported step type."));
                    return plan;
                }
                const QString rawName = modelStep.id.trimmed();
                if (rawName.isEmpty()) {
                    fail(plan, QStringLiteral("animejanai.profile.modelEmpty"),
                         RuntimeText::tr("Model step %1 in chain %2 of custom profile '%3' has no model.")
                             .arg(modelStep.number).arg(chain.number).arg(stagedSlot.profileName));
                    return plan;
                }

                if (const SpatialAsset* pinned = findSpatialAssetByStemOrId(rawName, pinnedCatalog)) {
                    const auto item = std::find_if(
                        storedPinned.cbegin(), storedPinned.cend(),
                        [pinned](const StoredSpatialAsset& row) { return row.asset.id == pinned->id; });
                    if (item == storedPinned.cend() || !item->verified) {
                        fail(plan, QStringLiteral("animejanai.model.missing"),
                             item == storedPinned.cend()
                                 ? RuntimeText::tr("A pinned spatial model required by custom profile '%1' is not installed.")
                                       .arg(stagedSlot.profileName)
                                 : item->reason);
                        return plan;
                    }
                    if (!stagedSpatialDigests.contains(pinned->sha256)) {
                        const QString target =
                            QDir(plan.modelDir).filePath(pinned->sha256 + QStringLiteral(".onnx"));
                        QString copyError;
                        if (!copyVerified(item->graphPath, target, pinned->bytes, pinned->sha256,
                                          ownedRoot, cancelled, &copyError) ||
                            !retainSpatialLicense(*pinned, target, ownedRoot, &copyError)) {
                            fail(plan, QStringLiteral("animejanai.model.stage"), copyError);
                            return plan;
                        }
                        stagedSpatialDigests.insert(pinned->sha256);
                    }
                    modelStep.id = pinned->sha256;
                    continue;
                }

                const std::optional<CustomAnimeJanaiModel> customModel =
                    findCustomAnimeJanaiModel(rawName);
                if (!customModel.has_value() || !customModel->valid) {
                    fail(plan, QStringLiteral("animejanai.model.missing"),
                         RuntimeText::tr("Custom profile '%1' references ONNX model '%2', which is missing or invalid.")
                             .arg(stagedSlot.profileName, rawName));
                    return plan;
                }
                if (!stagedSpatialDigests.contains(customModel->sha256)) {
                    const QString target =
                        QDir(plan.modelDir).filePath(customModel->sha256 + QStringLiteral(".onnx"));
                    QString copyError;
                    if (!copyVerified(customModel->filePath, target, customModel->bytes,
                                      customModel->sha256, ownedRoot, cancelled, &copyError) ||
                        !retainCustomModelProvenance(*customModel, target, ownedRoot, &copyError)) {
                        fail(plan, QStringLiteral("animejanai.model.stage"), copyError);
                        return plan;
                    }
                    stagedSpatialDigests.insert(customModel->sha256);
                }
                modelStep.id = customModel->sha256;
            }

            if (chain.rifeEnabled) {
                const int code =
                    AnimeJanaiConfigStore::rifeLabelToValue(chain.rifeModel).toInt();
                const RifeModelPin* pin = rifeModelPinForCode(code, chain.rifeEnsemble);
                if (pin == nullptr) {
                    fail(plan, QStringLiteral("animejanai.model.pinMissing"),
                         RuntimeText::tr("The RIFE model '%1' in custom profile '%2' has no compiled content pin.")
                             .arg(chain.rifeModel, stagedSlot.profileName));
                    return plan;
                }
                if (pin->variant == RifeVariant::Heavy && !installation.heavyRifeSupported) {
                    fail(plan, QStringLiteral("animejanai.preset.rifeHeavyUnsupported"),
                         RuntimeText::tr("This installed AnimeJaNai runtime does not report support for the selected heavy RIFE model."));
                    return plan;
                }
                const StoredRifeGraph stored = storedRifeGraph(rifeArchiveStore(), *pin);
                if (!stored.installed) {
                    fail(plan, QStringLiteral("animejanai.model.missing"),
                         stored.reason.isEmpty()
                             ? RuntimeText::tr("The selected pinned RIFE graph is missing or corrupt.")
                             : stored.reason);
                    return plan;
                }
                if (!stagedRifeFiles.contains(pin->fileName)) {
                    QString copyError;
                    if (!copyVerified(stored.path, QDir(plan.rifeModelDir).filePath(pin->fileName),
                                      pin->bytes, pin->graphSha256, ownedRoot, cancelled, &copyError)) {
                        fail(plan, QStringLiteral("animejanai.model.stage"), copyError);
                        return plan;
                    }
                    const QJsonObject provenance{
                        {QStringLiteral("source"),
                         QStringLiteral("https://github.com/the-database/mpv-AnimeJaNai/releases/tag/3.7.0")},
                        {QStringLiteral("upstreamFile"), pin->fileName},
                        {QStringLiteral("sha256"), pin->graphSha256},
                        {QStringLiteral("terms"), QStringLiteral("upstream terms not verified")}};
                    if (!writeIni(QJsonDocument(provenance).toJson(),
                                  QDir(plan.rifeModelDir)
                                      .filePath(pin->fileName + QStringLiteral(".source.json")),
                                  ownedRoot, &copyError)) {
                        fail(plan, QStringLiteral("animejanai.model.provenance"), copyError);
                        return plan;
                    }
                    stagedRifeFiles.insert(pin->fileName);
                }
            }
        }

        ini = QString::fromUtf8(globalIniHeader(managerConf)) +
              AnimeJanaiConfigStore::serializeSlotSection(stagedSlot, plan.slot);
    } else if (presetId.startsWith(QStringLiteral("builtin"))) {
        const auto assets = spatialAssetCatalog();
        QStringList requiredNames = {
            QStringLiteral("2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx"),
            QStringLiteral("2x_AnimeJaNai_HD_V3.1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx"),
            QStringLiteral("2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo.onnx")};
        if (managerConf.qualitySharp || managerConf.balancedSharp) {
            requiredNames.append(QStringLiteral(
                "2x_AnimeJaNai_HD_V3.1Sharp1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx"));
        }
        if (managerConf.performanceSharp) {
            requiredNames.append(QStringLiteral(
                "2x_AnimeJaNai_HD_V3.1Sharp1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx"));
        }
        const auto stored = storedSpatialAssets();
        for (const QString& required : requiredNames) {
            const auto assetIt = std::find_if(assets.cbegin(), assets.cend(), [&required](const SpatialAsset& row) { return row.name == required; });
            if (assetIt == assets.cend()) { fail(plan, QStringLiteral("animejanai.model.catalogueMissing"), RuntimeText::tr("A required built-in model is absent from the pinned catalogue.")); return plan; }
            const auto item = std::find_if(stored.cbegin(), stored.cend(), [&assetIt](const StoredSpatialAsset& row) { return row.asset.id == assetIt->id; });
            if (item == stored.cend() || !item->verified) {
                fail(plan, QStringLiteral("animejanai.model.missing"), item == stored.cend() ? RuntimeText::tr("A model required by the built-in AnimeJaNai profiles is not installed.") : item->reason);
                return plan;
            }
            const QString target = QDir(plan.modelDir).filePath(assetIt->sha256 + QStringLiteral(".onnx"));
            QString copyError;
            if (!copyVerified(item->graphPath, target, assetIt->bytes, assetIt->sha256, ownedRoot, cancelled, &copyError) ||
                !retainSpatialLicense(*assetIt, target, ownedRoot, &copyError)) {
                fail(plan, QStringLiteral("animejanai.model.stage"), copyError); return plan;
            }
        }
        pruneLegacySdModelOrphans(plan.modelDir);
        ini = QString::fromUtf8(builtinIni(managerConf));
        for (const SpatialAsset& asset : assets)
            ini.replace(asset.name.chopped(5), asset.sha256);
    } else if (preset->rife) {
        const RifeModelPin* pin = selectedRife;
        const StoredRifeGraph stored = storedRifeGraph(rifeArchiveStore(), *pin);
        if (!stored.installed) {
            fail(plan, QStringLiteral("animejanai.model.missing"), stored.reason.isEmpty() ? RuntimeText::tr("The selected pinned RIFE graph is missing or corrupt.") : stored.reason);
            return plan;
        }
        // ABI 8 resolves RIFE filenames from its numeric model code.
        const QString safeName = pin->fileName;
        QString copyError;
        if (!copyVerified(stored.path, QDir(plan.rifeModelDir).filePath(safeName), pin->bytes, pin->graphSha256,
                          ownedRoot, cancelled, &copyError)) {
            fail(plan, QStringLiteral("animejanai.model.stage"), copyError); return plan;
        }
        const QJsonObject provenance{
            {QStringLiteral("source"), QStringLiteral("https://github.com/the-database/mpv-AnimeJaNai/releases/tag/3.7.0")},
            {QStringLiteral("upstreamFile"), pin->fileName},
            {QStringLiteral("sha256"), pin->graphSha256},
            {QStringLiteral("terms"), QStringLiteral("upstream terms not verified")}};
        if (!writeIni(QJsonDocument(provenance).toJson(),
                      QDir(plan.rifeModelDir).filePath(safeName + QStringLiteral(".source.json")),
                      ownedRoot, &copyError)) {
            fail(plan, QStringLiteral("animejanai.model.provenance"), copyError); return plan;
        }
        const int code = rifeModelCode(pin->version, pin->variant);
        ini = QString::fromUtf8(globalIniHeader(managerConf)) +
              QStringLiteral("[slot_%1]\nprofile_name=RIFE 2x\nchain_1_min_resolution=0x0\nchain_1_max_resolution=infxinf\nchain_1_rife=yes\nchain_1_rife_factor_numerator=2\nchain_1_rife_factor_denominator=1\nchain_1_rife_model=%2\nchain_1_rife_ensemble=%3\n")
                  .arg(plan.slot)
                  .arg(code)
                  .arg(pin->ensemble ? QStringLiteral("yes") : QStringLiteral("no"));
    } else if (presetId.startsWith(QStringLiteral("spatial:custom:"))) {
        const QString customId = presetId.mid(QStringLiteral("spatial:custom:").size());
        const std::optional<CustomAnimeJanaiModel> customModel = findCustomAnimeJanaiModel(customId);
        if (!customModel.has_value()) {
            fail(plan, QStringLiteral("animejanai.model.invalid"),
                 RuntimeText::tr("The selected custom ONNX model is not registered."));
            return plan;
        }
        if (!customModel->valid) {
            fail(plan, QStringLiteral("animejanai.model.missing"),
                 customModel->reason.isEmpty()
                     ? RuntimeText::tr("The selected custom ONNX model is missing or failed integrity verification.")
                     : customModel->reason);
            return plan;
        }
        const QString safeName = customModel->sha256;
        QString copyError;
        const QString target = QDir(plan.modelDir).filePath(safeName + QStringLiteral(".onnx"));
        if (!copyVerified(customModel->filePath, target, customModel->bytes, customModel->sha256,
                          ownedRoot, cancelled, &copyError) ||
            !retainCustomModelProvenance(*customModel, target, ownedRoot, &copyError)) {
            fail(plan, QStringLiteral("animejanai.model.stage"), copyError);
            return plan;
        }
        ini = QString::fromUtf8(globalIniHeader(managerConf)) +
              QStringLiteral("[slot_%1]\nprofile_name=%2\nchain_1_min_resolution=0x0\nchain_1_max_resolution=infxinf\nchain_1_model_1_name=%3\n")
                  .arg(plan.slot)
                  .arg(customModel->id, safeName);
    } else {
        const SpatialAsset* asset = findSpatialAsset(presetId.mid(QStringLiteral("spatial:").size()));
        if (!asset) { fail(plan, QStringLiteral("animejanai.model.invalid"), RuntimeText::tr("The selected spatial model is not in the pinned catalogue.")); return plan; }
        const auto stored = storedSpatialAssets();
        const auto item = std::find_if(stored.cbegin(), stored.cend(), [asset](const StoredSpatialAsset& row) { return row.asset.id == asset->id; });
        if (item == stored.cend() || !item->verified) {
            fail(plan, QStringLiteral("animejanai.model.missing"), item == stored.cend() ? RuntimeText::tr("The selected spatial model is not installed.") : item->reason);
            return plan;
        }
        const QString safeName = asset->sha256;
        QString copyError;
        const QString target = QDir(plan.modelDir).filePath(safeName + QStringLiteral(".onnx"));
        if (!copyVerified(item->graphPath, target, asset->bytes, asset->sha256, ownedRoot, cancelled, &copyError) ||
            !retainSpatialLicense(*asset, target, ownedRoot, &copyError)) {
            fail(plan, QStringLiteral("animejanai.model.stage"), copyError); return plan;
        }
        ini = QString::fromUtf8(globalIniHeader(managerConf)) +
              QStringLiteral("[slot_%1]\nprofile_name=Spatial model\nchain_1_min_resolution=0x0\nchain_1_max_resolution=infxinf\nchain_1_model_1_name=%2\n")
                  .arg(plan.slot)
                  .arg(safeName);
    }
    if (stop()) { fail(plan, QStringLiteral("animejanai.prepare.cancelled"), RuntimeText::tr("AnimeJaNai preparation was cancelled.")); return plan; }
    if (backend == Backend::DirectML)
        ini.replace(QStringLiteral("backend=TensorRT"), QStringLiteral("backend=DirectML"));
    QString configError;
    if (!writeIni(ini.toUtf8(), plan.configPath, ownedRoot, &configError)) {
        fail(plan, QStringLiteral("animejanai.config.write"), configError); return plan;
    }
    if (!pathInside(ownedRoot, plan.statsPath) ||
        !QDir().mkpath(QFileInfo(plan.statsPath).absolutePath()) || !pathInside(ownedRoot, plan.statsPath)) {
        fail(plan, QStringLiteral("animejanai.stats.directory"), RuntimeText::tr("The app-owned AnimeJaNai stats directory is unavailable or redirected.")); return plan;
    }

    QVector<NativeFilterOption> options{
        {QStringLiteral("lib"), plan.libraryPath},
        {QStringLiteral("conf"), plan.configPath},
        {QStringLiteral("model-dir"), plan.modelDir},
        {QStringLiteral("rife-model-dir"), plan.rifeModelDir},
        {QStringLiteral("trtexec"), plan.trtexecPath},
        {QStringLiteral("trtexec-libdir"), plan.trtexecLibDir},
        {QStringLiteral("slot"), QString::number(plan.slot)},
        {QStringLiteral("stats"), plan.statsPath},
        {QStringLiteral("output-444"), QStringLiteral("no")},
        {QStringLiteral("passthrough"), QStringLiteral("no")}};
    QString filterError;
    plan.filterSpec = officialFilterSpec(options, &filterError);
    if (plan.filterSpec.isEmpty()) { fail(plan, QStringLiteral("animejanai.filter.spec"), filterError); return plan; }
    plan.available = true;
    plan.requiresAji = true;
    AnimeJanaiPlanStage stage;
    stage.kind = AnimeJanaiPlanStage::Kind::Aji;
    stage.filterLabel = QString::fromLatin1(kAnimeJanaiFilterLabel);
    stage.filterSpec = plan.filterSpec;
    stage.configPath = plan.configPath;
    stage.statsPath = plan.statsPath;
    stage.firstStepNumber = 1;
    stage.lastStepNumber = 1;
    stage.presetId = presetId;
    stage.cacheLease = plan.cacheLease;
    plan.stages.append(stage);
    plan.cacheKeys.append(QDir::cleanPath(QFileInfo(plan.modelDir).absoluteFilePath()));
    plan.cacheKeys.append(QDir::cleanPath(QFileInfo(plan.rifeModelDir).absoluteFilePath()));
    return plan;
}

} // namespace mplayer::native
