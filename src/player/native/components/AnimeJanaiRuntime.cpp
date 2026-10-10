#include "AnimeJanaiRuntime.hpp"

#include "native/bundle/NativePaths.hpp"
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
#include <QtCore/QSaveFile>

#include <algorithm>

namespace mplayer::native {
namespace {

QString appNativeRoot() {
    return nativeRoot();
}
QString configRoot() { return QDir(appNativeRoot()).filePath(QStringLiteral("animejanai")); }
QString translated(const char* text) { return QCoreApplication::translate("AnimeJanaiRuntime", text); }

void fail(AnimeJanaiPlan& plan, const QString& key, const QString& message) {
    plan.available = false;
    plan.reasonKey = key;
    plan.reason = message;
    plan.filterSpec.clear();
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
    if (cancelled && cancelled()) { *error = translated("AnimeJaNai preparation was cancelled."); return false; }
    if (!hashMatches(source, size, digest, cancelled)) {
        *error = translated("The selected model graph is missing or does not match its compiled content pin.");
        return false;
    }
    if (!pathInside(ownedRoot, target) ||
        !QDir().mkpath(QFileInfo(target).absolutePath()) || !pathInside(ownedRoot, target)) {
        *error = translated("The app-owned AnimeJaNai model directory is unavailable or redirected.");
        return false;
    }
    if (QFileInfo::exists(target)) {
        if (hashMatches(target, size, digest, cancelled)) return true;
        *error = translated("An existing staged AnimeJaNai graph is corrupt and will not be replaced.");
        return false;
    }
    const QString temporary = target + QStringLiteral(".tmp-") + QString::number(QCoreApplication::applicationPid());
    QFile input(source);
    QSaveFile output(temporary);
    output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        *error = !input.isOpen() ? input.errorString() : output.errorString();
        return false;
    }
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    while (!input.atEnd()) {
        if (cancelled && cancelled()) {
            output.cancelWriting();
            *error = translated("AnimeJaNai preparation was cancelled.");
            return false;
        }
        const qint64 count = input.read(buffer.data(), buffer.size());
        if (count <= 0 || output.write(buffer.constData(), count) != count) {
            *error = input.error() != QFile::NoError ? input.errorString() : output.errorString();
            output.cancelWriting();
            return false;
        }
    }
    if (!output.commit() || !hashMatches(temporary, size, digest, cancelled) || !QFile::rename(temporary, target)) {
        QFile::remove(temporary);
        *error = translated("Staged AnimeJaNai graph verification or atomic publication failed.");
        return false;
    }
    return true;
}

const AnimeJanaiPreset* findPreset(const QString& id, const QVector<AnimeJanaiPreset>& presets) {
    for (const AnimeJanaiPreset& preset : presets) if (preset.id == id) return &preset;
    return nullptr;
}

QString safeConfigName(const QString& presetId) {
    if (presetId.startsWith(QStringLiteral("builtin"))) return QStringLiteral("builtins");
    return QString::fromLatin1(QCryptographicHash::hash(presetId.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QByteArray modelLine(int chain, const QString& name) {
    return QByteArrayLiteral("chain_") + QByteArray::number(chain) + QByteArrayLiteral("_model_1_name=") + name.toUtf8() + '\n';
}

QByteArray builtinIni() {
    const QByteArray hdBalanced = QByteArrayLiteral("2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16");
    const QByteArray hdPerformance = QByteArrayLiteral("2x_AnimeJaNai_HD_V3.1_Performance_SPANF3_b5f48_unshuffle_fp16");
    const QByteArray sd = QByteArrayLiteral("2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo");
    QByteArray ini = QByteArrayLiteral("[global]\nconfig_version=2\nbackend=TensorRT\nlogging=yes\n");
    ini += QByteArrayLiteral("[slot_1001]\nprofile_name=Quality\n");
    ini += QByteArrayLiteral("chain_1_min_resolution=1280x720\nchain_1_max_resolution=1920x1080\nchain_1_max_fps=31\n") + modelLine(1, hdBalanced);
    ini += QByteArrayLiteral("chain_2_min_resolution=0x0\nchain_2_max_resolution=1280x720\nchain_2_max_fps=31\n") + modelLine(2, sd);
    ini += QByteArrayLiteral("chain_3_min_resolution=0x0\nchain_3_max_resolution=1920x1080\nchain_3_max_fps=61\n") + modelLine(3, hdBalanced);
    ini += QByteArrayLiteral("[slot_1002]\nprofile_name=Balanced\n");
    ini += QByteArrayLiteral("chain_1_min_resolution=1280x720\nchain_1_max_resolution=1920x1080\nchain_1_max_fps=31\n") + modelLine(1, hdBalanced);
    ini += QByteArrayLiteral("chain_2_min_resolution=0x0\nchain_2_max_resolution=1280x720\nchain_2_max_fps=31\n") + modelLine(2, sd);
    ini += QByteArrayLiteral("[slot_1003]\nprofile_name=Performance\n");
    ini += QByteArrayLiteral("chain_1_min_resolution=1280x720\nchain_1_max_resolution=1920x1080\nchain_1_max_fps=31\n") + modelLine(1, hdPerformance);
    ini += QByteArrayLiteral("chain_2_min_resolution=0x0\nchain_2_max_resolution=1280x720\nchain_2_max_fps=31\n") + modelLine(2, sd);
    return ini;
}

bool writeIni(const QByteArray& ini, const QString& path, const QString& root, QString* error) {
    if (!pathInside(root, path) ||
        !QDir().mkpath(QFileInfo(path).absolutePath()) || !pathInside(root, path)) {
        *error = translated("The app-owned AnimeJaNai configuration directory is unavailable or redirected.");
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

QByteArray officialFilterSpec(const QVector<NativeFilterOption>& options, QString* error) {
    QByteArray result = QByteArrayLiteral("@mikai_aji:animejanai");
    char separator = '=';
    for (const NativeFilterOption& option : options) {
        if (option.name.isEmpty() || option.value.contains(QChar::Null)) {
            *error = translated("An AnimeJaNai filter option cannot be represented safely.");
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

} // namespace

QVector<AnimeJanaiPreset> animeJanaiPresets() {
    QVector<AnimeJanaiPreset> presets;
    const QVector<SpatialAsset> spatial = spatialAssetCatalog();
    const QVector<RifeModelPin>& rife = rifeModelPins();
    presets.reserve(3 + spatial.size() + rife.size());
    presets.append({QStringLiteral("builtin1001"), translated("Quality"), 1001, false, {}, true, {}});
    presets.append({QStringLiteral("builtin1002"), translated("Balanced"), 1002, false, {}, true, {}});
    presets.append({QStringLiteral("builtin1003"), translated("Performance"), 1003, false, {}, true, {}});
    for (qsizetype i = 0; i < spatial.size(); ++i) {
        const SpatialAsset& asset = spatial.at(i);
        presets.append({QStringLiteral("spatial:") + asset.id, asset.name, 2001 + static_cast<int>(i), false,
                        asset.name, true, {}});
    }
    for (qsizetype i = 0; i < rife.size(); ++i) {
        const RifeModelPin& pin = rife.at(i);
        presets.append({QStringLiteral("rife:") + pin.fileName, pin.fileName, 2101 + static_cast<int>(i), true,
                        pin.fileName, true, {}});
    }
    return presets;
}

AnimeJanaiPlan prepareAnimeJanaiPlan(const QString& presetId, Backend backend,
                                    const std::function<bool()>& cancelled) {
    AnimeJanaiPlan plan;
    plan.backend = backend;
    if (presetId.isEmpty()) return plan;
    plan.enabled = true;
    plan.presetId = presetId;
    const auto stop = [&] { return cancelled && cancelled(); };
    if (stop()) { fail(plan, QStringLiteral("animejanai.prepare.cancelled"), translated("AnimeJaNai preparation was cancelled.")); return plan; }
    const QVector<AnimeJanaiPreset> presets = animeJanaiPresets();
    const AnimeJanaiPreset* preset = findPreset(presetId, presets);
    if (!preset) { fail(plan, QStringLiteral("animejanai.preset.invalid"), translated("The selected AnimeJaNai preset is not recognised.")); return plan; }

    const AnimeJanaiInstallation installation = describeAnimeJanaiInstallation(backend, true);
    if (!installation.installed) { fail(plan, installation.reasonKey, installation.reason); return plan; }
    const RifeModelPin* selectedRife = preset->rife ? rifeModelPinForFileName(preset->modelFile) : nullptr;
    if (preset->rife && !selectedRife) {
        fail(plan, QStringLiteral("animejanai.model.pinMissing"), translated("The selected RIFE graph has no compiled content pin."));
        return plan;
    }
    if (selectedRife && selectedRife->variant == RifeVariant::Heavy && !installation.heavyRifeSupported) {
        fail(plan, QStringLiteral("animejanai.preset.rifeHeavyUnsupported"),
             translated("This installed AnimeJaNai runtime does not report support for the selected heavy RIFE model."));
        return plan;
    }

    const QString ownedRoot = configRoot();
    plan.libraryPath = QDir(installation.root).filePath(QStringLiteral("aji.dll"));
    plan.configPath = QDir(ownedRoot).filePath(QStringLiteral("config/") + safeConfigName(presetId) + QStringLiteral(".ini"));
    plan.modelDir = QDir(ownedRoot).filePath(QStringLiteral("models/spatial"));
    plan.rifeModelDir = QDir(ownedRoot).filePath(QStringLiteral("models/rife"));
    plan.statsPath = QDir(ownedRoot).filePath(QStringLiteral("stats/") + safeConfigName(presetId) + QStringLiteral(".txt"));
    plan.slot = preset->slot;
    plan.trtexecLibDir = installation.root;
    if (backend == Backend::TensorRT) {
        plan.trtexecPath = QDir(installation.root).filePath(QStringLiteral("trtexec.exe"));
        if (!regularFile(plan.trtexecPath)) {
            fail(plan, QStringLiteral("animejanai.runtime.trtexecMissing"),
                 translated("The installed TensorRT closure does not include its verified upstream trtexec executable."));
            return plan;
        }
    }

    QString ini;
    if (presetId.startsWith(QStringLiteral("builtin"))) {
        const auto assets = spatialAssetCatalog();
        const QStringList requiredNames = {
            QStringLiteral("2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx"),
            QStringLiteral("2x_AnimeJaNai_HD_V3.1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx"),
            QStringLiteral("2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo.onnx")};
        for (const QString& required : requiredNames) {
            const auto assetIt = std::find_if(assets.cbegin(), assets.cend(), [&required](const SpatialAsset& row) { return row.name == required; });
            if (assetIt == assets.cend()) { fail(plan, QStringLiteral("animejanai.model.catalogueMissing"), translated("A required built-in model is absent from the pinned catalogue.")); return plan; }
            const auto stored = storedSpatialAssets();
            const auto item = std::find_if(stored.cbegin(), stored.cend(), [&assetIt](const StoredSpatialAsset& row) { return row.asset.id == assetIt->id; });
            if (item == stored.cend() || !item->verified || !hashMatches(item->graphPath, assetIt->bytes, assetIt->sha256, cancelled)) {
                fail(plan, QStringLiteral("animejanai.model.missing"), item == stored.cend() ? translated("A model required by the built-in AnimeJaNai profiles is not installed.") : item->reason);
                return plan;
            }
            const QString target = QDir(plan.modelDir).filePath(assetIt->sha256 + QStringLiteral(".onnx"));
            QString copyError;
            if (!copyVerified(item->graphPath, target, assetIt->bytes, assetIt->sha256, ownedRoot, cancelled, &copyError) ||
                !retainSpatialLicense(*assetIt, target, ownedRoot, &copyError)) {
                fail(plan, QStringLiteral("animejanai.model.stage"), copyError); return plan;
            }
        }
        ini = QString::fromLatin1(builtinIni());
        for (const SpatialAsset& asset : assets)
            ini.replace(asset.name.chopped(5), asset.sha256);
    } else if (preset->rife) {
        const RifeModelPin* pin = selectedRife;
        const StoredRifeGraph stored = storedRifeGraph(rifeArchiveStore(), *pin);
        if (!stored.installed || !hashMatches(stored.path, pin->bytes, pin->graphSha256, cancelled)) {
            fail(plan, QStringLiteral("animejanai.model.missing"), stored.reason.isEmpty() ? translated("The selected pinned RIFE graph is missing or corrupt.") : stored.reason);
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
        ini = QStringLiteral("[global]\nconfig_version=2\nbackend=TensorRT\nlogging=yes\n[slot_%1]\nprofile_name=RIFE 2x\nchain_1_min_resolution=0x0\nchain_1_max_resolution=infxinf\nchain_1_rife=yes\nchain_1_rife_factor_numerator=2\nchain_1_rife_factor_denominator=1\nchain_1_rife_model=%2\nchain_1_rife_ensemble=%3\n")
            .arg(plan.slot).arg(code).arg(pin->ensemble ? QStringLiteral("yes") : QStringLiteral("no"));
    } else {
        const SpatialAsset* asset = findSpatialAsset(presetId.mid(QStringLiteral("spatial:").size()));
        if (!asset) { fail(plan, QStringLiteral("animejanai.model.invalid"), translated("The selected spatial model is not in the pinned catalogue.")); return plan; }
        const auto stored = storedSpatialAssets();
        const auto item = std::find_if(stored.cbegin(), stored.cend(), [asset](const StoredSpatialAsset& row) { return row.asset.id == asset->id; });
        if (item == stored.cend() || !item->verified || !hashMatches(item->graphPath, asset->bytes, asset->sha256, cancelled)) {
            fail(plan, QStringLiteral("animejanai.model.missing"), item == stored.cend() ? translated("The selected spatial model is not installed.") : item->reason);
            return plan;
        }
        const QString safeName = asset->sha256;
        QString copyError;
        const QString target = QDir(plan.modelDir).filePath(safeName + QStringLiteral(".onnx"));
        if (!copyVerified(item->graphPath, target, asset->bytes, asset->sha256, ownedRoot, cancelled, &copyError) ||
            !retainSpatialLicense(*asset, target, ownedRoot, &copyError)) {
            fail(plan, QStringLiteral("animejanai.model.stage"), copyError); return plan;
        }
        ini = QStringLiteral("[global]\nconfig_version=2\nbackend=TensorRT\nlogging=yes\n[slot_%1]\nprofile_name=Spatial model\nchain_1_min_resolution=0x0\nchain_1_max_resolution=infxinf\nchain_1_model_1_name=%2\n")
            .arg(plan.slot).arg(safeName);
    }
    if (stop()) { fail(plan, QStringLiteral("animejanai.prepare.cancelled"), translated("AnimeJaNai preparation was cancelled.")); return plan; }
    if (backend == Backend::DirectML)
        ini.replace(QStringLiteral("backend=TensorRT"), QStringLiteral("backend=DirectML"));
    QString configError;
    if (!writeIni(ini.toUtf8(), plan.configPath, ownedRoot, &configError)) {
        fail(plan, QStringLiteral("animejanai.config.write"), configError); return plan;
    }
    if (!pathInside(ownedRoot, plan.statsPath) ||
        !QDir().mkpath(QFileInfo(plan.statsPath).absolutePath()) || !pathInside(ownedRoot, plan.statsPath)) {
        fail(plan, QStringLiteral("animejanai.stats.directory"), translated("The app-owned AnimeJaNai stats directory is unavailable or redirected.")); return plan;
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
    return plan;
}

} // namespace mplayer::native
