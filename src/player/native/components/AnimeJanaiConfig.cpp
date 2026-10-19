#include "AnimeJanaiConfig.hpp"

#include "backend/Cancellation.hpp"
#include "native/bundle/NativePaths.hpp"
#include "native/models/NativeRife.hpp"
#include "native/models/NativeSpatialAssets.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QMap>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>

#include <algorithm>
#include <cmath>
#include <utility>

namespace mplayer::native {
namespace {

class ConfigText {
    Q_DECLARE_TR_FUNCTIONS(AnimeJanaiConfig)
};

constexpr auto kDefaultTrtSettings =
    "--builderOptimizationLevel=5 --optShapes=input:%video_resolution% --skipInference";
constexpr auto kLegacyTrtSettings =
    "--stronglyTyped --optShapes=input:%video_resolution% --inputIOFormats=fp16:chw "
    "--outputIOFormats=fp16:chw --builderOptimizationLevel=5 "
    "--tacticSources=-CUDNN,-CUBLAS,-CUBLAS_LT --skipInference";

constexpr qint64 kMinOnnxFileBytes = 64;
constexpr qint64 kMaxOnnxFileBytes = 2LL * 1024LL * 1024LL * 1024LL;  // 2 GB
constexpr qint64 kMaxModelMetadataBytes = 4LL * 1024LL * 1024LL;
constexpr qint64 kMaxModelSidecarBytes = 64LL * 1024LL;

QString animeJanaiRootDir() {
    return QDir(nativeRoot()).filePath(QStringLiteral("animejanai"));
}

QString stagedSpatialModelsDir() {
    return QDir(animeJanaiRootDir()).filePath(QStringLiteral("models/spatial"));
}

QString externalModelsRegistryPath() {
    return QDir(animeJanaiRootDir()).filePath(QStringLiteral("models/external-models.json"));
}

QString sanitizeModelStem(const QString& rawStem) {
    QString cleaned;
    cleaned.reserve(rawStem.size());
    for (const QChar ch : rawStem.trimmed()) {
        if (ch.isLetterOrNumber() || ch == u'_' || ch == u'-' || ch == u'.' || ch == u'+') {
            cleaned.append(ch);
        } else if (ch.isSpace()) {
            cleaned.append(u'_');
        }
    }
    while (cleaned.startsWith(u'.')) cleaned.remove(0, 1);
    while (cleaned.endsWith(u'.')) cleaned.chop(1);
    return cleaned;
}

int inferScaleFactor(const QString& stem) {
    static const QRegularExpression scalePrefixRx(
        QStringLiteral("(?:^|[_\\-\\s])([12348])[xX](?:[_\\-\\s]|$)"));
    const QRegularExpressionMatch match = scalePrefixRx.match(stem);
    if (match.hasMatch()) {
        bool ok = false;
        const int factor = match.captured(1).toInt(&ok);
        if (ok && factor >= 1 && factor <= 8) return factor;
    }
    return 2;
}

QString inferArchitectureHint(const QString& stem) {
    const QString lower = stem.toLower();
    if (lower.contains(QStringLiteral("compact")) || lower.contains(QStringLiteral("srvgg"))) {
        return QStringLiteral("RealESRGAN Compact (SRVGGNet)");
    }
    if (lower.contains(QStringLiteral("realesrgan")) || lower.contains(QStringLiteral("esrgan")) ||
        lower.contains(QStringLiteral("rrdb"))) {
        return QStringLiteral("RealESRGAN");
    }
    if (lower.contains(QStringLiteral("span"))) {
        return QStringLiteral("SPAN");
    }
    if (lower.contains(QStringLiteral("dat"))) {
        return QStringLiteral("DAT");
    }
    if (lower.contains(QStringLiteral("swinir")) || lower.contains(QStringLiteral("hat"))) {
        return QStringLiteral("SwinIR / HAT");
    }
    if (lower.contains(QStringLiteral("cugan"))) {
        return QStringLiteral("Real-CUGAN");
    }
    return QStringLiteral("RealESRGAN / ONNX");
}

struct ExternalModelPin {
    QString id;
    QString filePath;
    QString displayName;
    qint64 bytes = 0;
    QString sha256;
};

QVector<ExternalModelPin> readExternalModelPins(QString* error = nullptr) {
    QVector<ExternalModelPin> pins;
    const QString path = externalModelsRegistryPath();
    if (!QFileInfo::exists(path)) return pins;
    if (!nativePathTrust(path, true).trusted ||
        !pathWithinRoot(nativeRoot(), path) ||
        !pathHasNoReparsePoints(nativeRoot(), path)) {
        if (error) *error = ConfigText::tr("The external model registry path is untrusted.");
        return pins;
    }
    QByteArray bytes;
    QString readError;
    const SharedMetadataReadStatus readStatus =
        readSharedMetadataFile(path, kMaxModelMetadataBytes, &bytes, &readError);
    if (readStatus == SharedMetadataReadStatus::TooLarge) {
        if (error) *error = ConfigText::tr("The external model registry exceeds the supported metadata size.");
        return pins;
    }
    if (readStatus != SharedMetadataReadStatus::Ok) {
        if (error) *error = readError;
        return pins;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    const QJsonObject root = document.object();
    if (parseError.error != QJsonParseError::NoError || !root.contains(QStringLiteral("models")) ||
        root.value(QStringLiteral("schema")).toString() != QStringLiteral("animejanai.external-models/1") ||
        !root.value(QStringLiteral("models")).isArray()) {
        if (error) *error = ConfigText::tr("The external model registry is malformed.");
        return {};
    }
    static const QRegularExpression digestRx(QStringLiteral("^[0-9a-f]{64}$"));
    for (const QJsonValue& value : root.value(QStringLiteral("models")).toArray()) {
        const QJsonObject object = value.toObject();
        ExternalModelPin pin;
        pin.id = object.value(QStringLiteral("id")).toString();
        pin.filePath = object.value(QStringLiteral("path")).toString();
        pin.displayName = object.value(QStringLiteral("displayName")).toString();
        pin.bytes = object.value(QStringLiteral("bytes")).toVariant().toLongLong();
        pin.sha256 = object.value(QStringLiteral("sha256")).toString().toLower();
        if (pin.id.isEmpty() || !QFileInfo(pin.filePath).isAbsolute() ||
            pin.bytes < kMinOnnxFileBytes || pin.bytes > kMaxOnnxFileBytes ||
            !digestRx.match(pin.sha256).hasMatch()) {
            if (error) *error = ConfigText::tr("The external model registry contains an invalid pin.");
            return {};
        }
        pins.push_back(std::move(pin));
    }
    if (error) error->clear();
    return pins;
}

bool writeExternalModelPins(const QVector<ExternalModelPin>& pins, QString* error) {
    const QString path = externalModelsRegistryPath();
    const QString parent = QFileInfo(path).absolutePath();
    if (!nativePathTrust(parent, true).trusted ||
        !pathWithinRoot(nativeRoot(), parent) ||
        !pathHasNoReparsePoints(nativeRoot(), parent) ||
        !QDir().mkpath(parent) ||
        !nativePathTrust(path, true).trusted ||
        !pathWithinRoot(nativeRoot(), path) ||
        !pathHasNoReparsePoints(nativeRoot(), path)) {
        if (error) *error = ConfigText::tr("The app-owned external model registry directory is unavailable or redirected.");
        return false;
    }
    QJsonArray models;
    for (const ExternalModelPin& pin : pins) {
        models.append(QJsonObject{{QStringLiteral("id"), pin.id},
                                  {QStringLiteral("path"), pin.filePath},
                                  {QStringLiteral("displayName"), pin.displayName},
                                  {QStringLiteral("bytes"), pin.bytes},
                                  {QStringLiteral("sha256"), pin.sha256}});
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("animejanai.external-models/1")},
        {QStringLiteral("models"), models}}).toJson(QJsonDocument::Compact);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        if (error) *error = file.errorString();
        return false;
    }
    if (!commitMetadataFile(file, error)) return false;
    if (error) error->clear();
    return true;
}

QString hashFileSha256(const QString& path, qint64* bytesOut,
                       const std::function<bool()>& cancelled) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 bytes = 0;
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    while (!file.atEnd()) {
        if (mikai::backend::WorkCancellationScope::cancellationRequested() ||
            (cancelled && cancelled())) return {};
        const qint64 count = file.read(buffer.data(), buffer.size());
        if (count <= 0) return {};
        hash.addData(QByteArrayView(buffer.constData(), count));
        bytes += count;
    }
    if (bytesOut) *bytesOut = bytes;
    return QString::fromLatin1(hash.result().toHex());
}

struct OwnedOnnxEntry {
    QString path;           ///< Absolute graph path as the directory spells it.
    QString canonicalPath;  ///< Canonical identity used for trust comparisons.
    QString fileName;       ///< Actual on-disk name, preserving spaces and case.
    QString rawStem;
    QString id;             ///< Sanitized identifier; may collide with another stem.
    qint64 bytes = 0;
};

/// Enumerates regular `.onnx` files directly inside the app-owned custom model
/// directory. Metadata only: no graph is opened and no model bytes are read.
QVector<OwnedOnnxEntry> ownedOnnxEntries() {
    QVector<OwnedOnnxEntry> entries;
    const QString dirPath = customAnimeJanaiModelsDir();
    if (!nativePathTrust(dirPath, true).trusted ||
        !pathWithinRoot(nativeRoot(), dirPath) ||
        !pathHasNoReparsePoints(nativeRoot(), dirPath)) {
        return entries;
    }
    const QDir dir(dirPath);
    if (!dir.exists()) return entries;
    const QFileInfoList candidates =
        dir.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo& candidate : candidates) {
        if (candidate.isSymLink() ||
            candidate.suffix().compare(QStringLiteral("onnx"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString absolute = candidate.absoluteFilePath();
        const QString canonical = candidate.canonicalFilePath();
        if (canonical.isEmpty() || !nativePathTrust(absolute, true).trusted ||
            !nativePathTrust(canonical, true).trusted ||
            !pathWithinRoot(dirPath, canonical) ||
            !pathWithinRoot(dirPath, absolute) ||
            !pathHasNoReparsePoints(nativeRoot(), absolute) ||
            !pathHasNoReparsePoints(nativeRoot(), canonical)) {
            continue;
        }
        OwnedOnnxEntry entry;
        entry.path = absolute;
        entry.canonicalPath = canonical;
        entry.fileName = candidate.fileName();
        entry.rawStem = candidate.completeBaseName();
        entry.id = sanitizeModelStem(entry.rawStem);
        entry.bytes = candidate.size();
        entries.push_back(std::move(entry));
    }
    return entries;
}

enum class OwnedSidecarState { Absent, Invalid, Valid };

struct OwnedSidecar {
    OwnedSidecarState state = OwnedSidecarState::Absent;
    QString reason;
    QString sha256;
    qint64 bytes = 0;
};

/// Classifies the integrity record beside an owned graph. Metadata only.
/// `requireGraphSizeMatch` is false only for removal, where a graph that changed
/// since registration must still be deletable while an unregistered paste is not.
OwnedSidecar readOwnedSidecar(const QString& graphPath, qint64 graphBytes,
                              bool requireGraphSizeMatch = true) {
    OwnedSidecar record;
    const QString sidecarPath = graphPath + QStringLiteral(".source.json");
    const QFileInfo sidecarInfo(sidecarPath);
    if (!sidecarInfo.exists() && !sidecarInfo.isSymLink()) {
        record.state = OwnedSidecarState::Absent;
        record.reason = ConfigText::tr(
            "This model is not registered; register it explicitly before using it.");
        return record;
    }
    const QString dirPath = customAnimeJanaiModelsDir();
    if (!sidecarInfo.isFile() || sidecarInfo.isSymLink() ||
        !nativePathTrust(sidecarPath, true).trusted ||
        !pathWithinRoot(dirPath, sidecarPath) ||
        !pathHasNoReparsePoints(nativeRoot(), sidecarPath)) {
        record.state = OwnedSidecarState::Invalid;
        record.reason = ConfigText::tr(
            "The model integrity record is not an app-owned regular file.");
        return record;
    }
    QByteArray sidecarBytes;
    const SharedMetadataReadStatus sidecarStatus =
        readSharedMetadataFile(sidecarPath, kMaxModelSidecarBytes, &sidecarBytes);
    if (sidecarStatus != SharedMetadataReadStatus::Ok) {
        record.state = OwnedSidecarState::Invalid;
        record.reason =
            sidecarStatus == SharedMetadataReadStatus::TooLarge
                ? ConfigText::tr(
                      "The model integrity record exceeds the supported metadata size.")
                : ConfigText::tr(
                      "The model integrity record is unreadable or exceeds the supported metadata size.");
        return record;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(sidecarBytes, &parseError);
    const QJsonObject provenance = document.object();
    const QString digest = provenance.value(QStringLiteral("sha256")).toString().toLower();
    const qint64 pinnedBytes =
        provenance.value(QStringLiteral("bytes")).toVariant().toLongLong();
    static const QRegularExpression digestRx(QStringLiteral("^[0-9a-f]{64}$"));
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        !digestRx.match(digest).hasMatch() || pinnedBytes < 0) {
        record.state = OwnedSidecarState::Invalid;
        record.reason = ConfigText::tr("The model integrity record is malformed.");
        return record;
    }
    if (requireGraphSizeMatch && pinnedBytes != graphBytes) {
        record.state = OwnedSidecarState::Invalid;
        record.reason = ConfigText::tr(
            "The model changed since it was registered; remove its integrity record, then register it again.");
        return record;
    }
    record.state = OwnedSidecarState::Valid;
    record.sha256 = digest;
    record.bytes = pinnedBytes;
    return record;
}

bool readProtoVarint(QFile& file, qint64 fileSize, qint64* scanned, quint64* value) {
    *value = 0;
    for (int index = 0; index < 10; ++index) {
        if (*scanned >= 1024 * 1024 || file.pos() >= fileSize) return false;
        char byteData = 0;
        if (!file.getChar(&byteData)) return false;
        ++*scanned;
        const unsigned char byte = static_cast<unsigned char>(byteData);
        if (index == 9 && (byte & 0xfe) != 0) return false;
        *value |= static_cast<quint64>(byte & 0x7f) << (index * 7);
        if ((byte & 0x80) == 0) return true;
    }
    return false;
}

struct ParsedIni {
    QMap<QString, QMap<QString, QString>> sections;
};

ParsedIni parseIniText(QStringView text) {
    ParsedIni ini;
    QString section;
    qsizetype start = 0;
    while (start <= text.size()) {
        qsizetype end = text.indexOf(u'\n', start);
        if (end < 0) end = text.size();
        QString line = text.mid(start, end - start).toString().trimmed();
        if (line.endsWith(u'\r')) line.chop(1);
        if (!line.isEmpty() && !line.startsWith(u';') && !line.startsWith(u'#')) {
            if (line.startsWith(u'[') && line.endsWith(u']')) {
                section = line.mid(1, line.size() - 2).trimmed().toLower();
            } else if (!section.isEmpty()) {
                const qsizetype equals = line.indexOf(u'=');
                if (equals > 0) {
                    const QString key = line.left(equals).trimmed().toLower();
                    const QString val = line.mid(equals + 1).trimmed();
                    ini.sections[section].insert(key, val);
                }
            }
        }
        if (end == text.size()) break;
        start = end + 1;
    }
    return ini;
}

QString iniValue(const ParsedIni& ini, const QString& section, const QString& key,
                 const QString& fallback = {}) {
    const auto secIt = ini.sections.constFind(section.toLower());
    if (secIt == ini.sections.cend()) return fallback;
    const auto it = secIt->constFind(key.toLower());
    return it == secIt->cend() ? fallback : *it;
}

bool isYes(QStringView input) {
    return input.trimmed().compare(u"yes", Qt::CaseInsensitive) == 0;
}

int toInt(const QString& input, int fallback = 0) {
    bool ok = false;
    const int result = input.trimmed().toInt(&ok, 10);
    return ok ? result : fallback;
}

double toDouble(const QString& input, double fallback = 0.0) {
    bool ok = false;
    const double result = QLocale::c().toDouble(input.trimmed(), &ok);
    return ok && std::isfinite(result) ? result : fallback;
}

int captureNumber(const QString& text, const QRegularExpression& expression, int group = 1) {
    const QRegularExpressionMatch match = expression.match(text);
    return match.hasMatch() ? toInt(match.captured(group), -1) : -1;
}

QVector<int> sortedIndices(const QMap<QString, QString>& keys,
                           const QRegularExpression& expression) {
    QSet<int> unique;
    for (auto it = keys.cbegin(); it != keys.cend(); ++it) {
        const int number = captureNumber(it.key(), expression);
        if (number > 0) unique.insert(number);
    }
    QVector<int> result(unique.cbegin(), unique.cend());
    std::sort(result.begin(), result.end());
    return result;
}

QVector<AnimeJanaiSlot> parseSlotsFromIni(const ParsedIni& ini, bool profileOnly,
                                               QString* error = nullptr) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
    };
    QVector<AnimeJanaiSlot> parsedSlots;
    static const QRegularExpression slotRx(QStringLiteral("^slot_(\\d+)$"));
    static const QRegularExpression chainRx(QStringLiteral("^chain_(\\d+)_"));
    static const QRegularExpression modelRx(QStringLiteral("^chain_(\\d+)_model_(\\d+)_"));

    for (auto secIt = ini.sections.cbegin(); secIt != ini.sections.cend(); ++secIt) {
        const QString sectionName = secIt.key();
        if (profileOnly ? (sectionName != QStringLiteral("slot") && !slotRx.match(sectionName).hasMatch())
                        : !slotRx.match(sectionName).hasMatch()) {
            continue;
        }
        const auto& keys = secIt.value();
        const QString profileExtensionVersion = keys.value(QStringLiteral("mikai_chain_steps_version"));
        if (!profileExtensionVersion.isEmpty()) {
            bool versionOk = false;
            const int parsedVersion = profileExtensionVersion.toInt(&versionOk);
            if (!versionOk || parsedVersion != 1) {
                fail(ConfigText::tr("The AnimeJaNai mixed-chain extension version is unsupported or malformed."));
                return {};
            }
        }
        AnimeJanaiSlot slot;
        slot.number = profileOnly ? 1 : captureNumber(sectionName, slotRx);
        if (slot.number <= 0) continue;
        if (!profileOnly && (slot.number < 1 || slot.number > 10)) continue;

        slot.profileName =
            keys.value(QStringLiteral("profile_name"), QStringLiteral("Custom Profile"));
        slot.descriptionText = keys.value(QStringLiteral("description"));
        const QVector<int> chainNumbers = sortedIndices(keys, chainRx);
        for (const int chainNumber : chainNumbers) {
            const QString prefix = QStringLiteral("chain_%1_").arg(chainNumber);
            AnimeJanaiChain chain;
            chain.number = chainNumber;
            chain.minResolution =
                keys.value(prefix + QStringLiteral("min_resolution"), QStringLiteral("0x0"));
            chain.maxResolution =
                keys.value(prefix + QStringLiteral("max_resolution"), QStringLiteral("0x0"));
            chain.minFps = toInt(keys.value(prefix + QStringLiteral("min_fps")), 0);
            chain.maxFps = toInt(keys.value(prefix + QStringLiteral("max_fps")), 0);
            chain.rifeEnabled = isYes(keys.value(prefix + QStringLiteral("rife")));
            chain.rifeEnsemble = isYes(keys.value(prefix + QStringLiteral("rife_ensemble")));
            chain.rifeBeforeUpscale =
                !keys.contains(prefix + QStringLiteral("rife_before_upscale")) ||
                isYes(keys.value(prefix + QStringLiteral("rife_before_upscale")));
            chain.rifeFactorNumerator =
                std::max(1, toInt(keys.value(prefix + QStringLiteral("rife_factor_numerator")), 2));
            chain.rifeFactorDenominator = std::max(
                1, toInt(keys.value(prefix + QStringLiteral("rife_factor_denominator")), 1));
            chain.rifeSceneDetectThreshold = std::clamp(
                toDouble(keys.value(prefix + QStringLiteral("rife_scene_detect_threshold")), 0.150),
                0.0, 1.0);
            if (keys.contains(prefix + QStringLiteral("rife_model"))) {
                chain.rifeModel = AnimeJanaiConfigStore::rifeValueToLabel(
                    keys.value(prefix + QStringLiteral("rife_model")));
            }

            const QString extensionVersion = keys.value(
                QStringLiteral("mikai_chain_steps_version"),
                iniValue(ini, QStringLiteral("global"), QStringLiteral("mikai_chain_steps_version")));
            const QRegularExpression anyStepRx(
                QStringLiteral("^chain_%1_step_\\d+_").arg(chainNumber));
            bool containsMixedStepKeys = false;
            for (auto keyIt = keys.cbegin(); keyIt != keys.cend(); ++keyIt) {
                if (anyStepRx.match(keyIt.key()).hasMatch()) {
                    containsMixedStepKeys = true;
                    break;
                }
            }
            if (extensionVersion.isEmpty() && containsMixedStepKeys) {
                fail(ConfigText::tr("Mixed AnimeJaNai steps are missing their extension version."));
                return {};
            }
            int parsedExtensionVersion = 0;
            if (!extensionVersion.isEmpty()) {
                bool versionOk = false;
                parsedExtensionVersion = extensionVersion.toInt(&versionOk);
                if (!versionOk || parsedExtensionVersion != 1) {
                    fail(ConfigText::tr("The AnimeJaNai mixed-chain extension version is unsupported or malformed."));
                    return {};
                }
            }
            QSet<int> modelNumbers;
            for (auto keyIt = keys.cbegin(); keyIt != keys.cend(); ++keyIt) {
                const QRegularExpressionMatch match = modelRx.match(keyIt.key());
                if (match.hasMatch() && toInt(match.captured(1), -1) == chainNumber) {
                    const int modelNum = toInt(match.captured(2), -1);
                    if (modelNum > 0) modelNumbers.insert(modelNum);
                }
            }
            QVector<int> orderedModels(modelNumbers.cbegin(), modelNumbers.cend());
            std::sort(orderedModels.begin(), orderedModels.end());
            for (const int modelNumber : orderedModels) {
                const QString modelPrefix =
                    prefix + QStringLiteral("model_%1_").arg(modelNumber);
                AnimeJanaiChainStep step;
                step.number = static_cast<int>(chain.steps.size()) + 1;
                step.kind = AnimeJanaiStepKind::OnnxModel;
                step.id = keys.value(modelPrefix + QStringLiteral("name"));
                step.resizeHeightBeforeUpscale = std::clamp(
                    toInt(keys.value(modelPrefix + QStringLiteral("resize_height_before_upscale")), 0),
                    0, 4320);
                step.resizeFactorBeforeUpscale = std::clamp(
                    toInt(keys.value(modelPrefix + QStringLiteral("resize_factor_before_upscale")), 100),
                    1, 400);
                chain.steps.push_back(std::move(step));
            }
            if (parsedExtensionVersion == 1 && containsMixedStepKeys) {
                static const QRegularExpression stepRx(
                    QStringLiteral("^chain_(\\d+)_step_(\\d+)_"));
                QSet<int> stepNumbers;
                for (auto keyIt = keys.cbegin(); keyIt != keys.cend(); ++keyIt) {
                    const auto match = stepRx.match(keyIt.key());
                    if (match.hasMatch() && toInt(match.captured(1), -1) == chainNumber) {
                        const int stepNumber = toInt(match.captured(2), -1);
                        if (stepNumber > 0) stepNumbers.insert(stepNumber);
                    }
                }
                QVector<int> orderedSteps(stepNumbers.cbegin(), stepNumbers.cend());
                std::sort(orderedSteps.begin(), orderedSteps.end());
                QVector<AnimeJanaiChainStep> mixedSteps;
                for (const int stepNumber : orderedSteps) {
                    const QString stepPrefix = prefix + QStringLiteral("step_%1_").arg(stepNumber);
                    if (!keys.contains(stepPrefix + QStringLiteral("kind")) ||
                        !keys.contains(stepPrefix + QStringLiteral("id")) ||
                        !keys.contains(stepPrefix + QStringLiteral("scale_factor")) ||
                        !keys.contains(stepPrefix + QStringLiteral("resize_height_before_upscale")) ||
                        !keys.contains(stepPrefix + QStringLiteral("resize_factor_before_upscale"))) {
                        fail(ConfigText::tr("The AnimeJaNai mixed-chain step is incomplete."));
                        return {};
                    }
                    AnimeJanaiChainStep step;
                    step.number = stepNumber;
                    const QString kind = keys.value(stepPrefix + QStringLiteral("kind"));
                    if (kind == QStringLiteral("onnx")) step.kind = AnimeJanaiStepKind::OnnxModel;
                    else if (kind == QStringLiteral("anime4k")) step.kind = AnimeJanaiStepKind::Anime4K;
                    else {
                        fail(ConfigText::tr("The AnimeJaNai mixed-chain step kind is unsupported."));
                        return {};
                    }
                    step.id = keys.value(stepPrefix + QStringLiteral("id"));
                    bool scaleOk = false;
                    step.scaleFactor = keys.value(stepPrefix + QStringLiteral("scale_factor")).toInt(&scaleOk);
                    if (!scaleOk || (step.scaleFactor != 1 && step.scaleFactor != 2 &&
                                     step.scaleFactor != 4)) {
                        fail(ConfigText::tr("The AnimeJaNai mixed-chain scale factor is invalid."));
                        return {};
                    }
                    bool heightOk = false;
                    bool resizeFactorOk = false;
                    step.resizeHeightBeforeUpscale = keys.value(
                        stepPrefix + QStringLiteral("resize_height_before_upscale")).toInt(&heightOk);
                    step.resizeFactorBeforeUpscale = keys.value(
                        stepPrefix + QStringLiteral("resize_factor_before_upscale")).toInt(&resizeFactorOk);
                    if (!heightOk || !resizeFactorOk || step.resizeHeightBeforeUpscale < 0 ||
                        step.resizeHeightBeforeUpscale > 4320 ||
                        step.resizeFactorBeforeUpscale < 1 || step.resizeFactorBeforeUpscale > 400) {
                        fail(ConfigText::tr("The AnimeJaNai mixed-chain resize settings are invalid."));
                        return {};
                    }
                    mixedSteps.push_back(std::move(step));
                }
                if (mixedSteps.isEmpty()) {
                    fail(ConfigText::tr("The AnimeJaNai mixed-chain extension contains no steps."));
                    return {};
                }
                chain.steps = std::move(mixedSteps);
                QString stepError;
                if (!AnimeJanaiConfigStore::validateAnimeJanaiChainSteps(chain, &stepError)) {
                    fail(stepError);
                    return {};
                }
            }
            slot.chains.push_back(std::move(chain));
        }
        parsedSlots.push_back(std::move(slot));
    }
    std::sort(parsedSlots.begin(), parsedSlots.end(),
              [](const AnimeJanaiSlot& a, const AnimeJanaiSlot& b) { return a.number < b.number; });
    return parsedSlots;
}

void putLine(QString& out, const QString& key, const QString& value) {
    QString safeValue = value;
    safeValue.remove(QChar::Null);
    safeValue.replace(u'\r', u' ');
    safeValue.replace(u'\n', u' ');
    out += key + u'=' + safeValue + u'\n';
}

QString shapeResolution(const QString& shape, const QString& fallback) {
    static const QRegularExpression rx(QStringLiteral("input:1x3x(\\d+)x(\\d+)"));
    const QRegularExpressionMatch match = rx.match(shape);
    return match.hasMatch() ? match.captured(2) + u'x' + match.captured(1) : fallback;
}

QString resolutionShape(QStringView resolution) {
    static const QRegularExpression rx(QStringLiteral("(\\d+)x(\\d+)"));
    const QRegularExpressionMatch match = rx.match(resolution.toString());
    return match.hasMatch()
               ? QStringLiteral("1x3x%1x%2").arg(match.captured(2), match.captured(1))
               : QStringLiteral("1x3x0x0");
}

QString replaceOrInsertSetting(QString settings, const QString& pattern,
                               const QString& replacement, const QString& insert) {
    const QRegularExpression rx(pattern);
    if (rx.match(settings).hasMatch()) return settings.replace(rx, replacement);
    const qsizetype tactics = settings.indexOf(QStringLiteral("--tacticSources"));
    if (tactics >= 0) {
        settings.insert(tactics, insert + u' ');
    } else {
        settings += (settings.isEmpty() ? QString() : QStringLiteral(" ")) + insert;
    }
    return settings;
}

}  // namespace

QString customAnimeJanaiModelsDir() {
    return QDir(animeJanaiRootDir()).filePath(QStringLiteral("models/custom"));
}

bool validateCustomOnnxFile(const QString& filePath, qint64* bytesOut, QString* reasonOut) {
    const auto fail = [reasonOut](const QString& message) {
        if (reasonOut != nullptr) *reasonOut = message;
        return false;
    };
    const QFileInfo info(filePath);
    if (!info.exists() || !info.isFile() || info.isSymLink()) {
        return fail(ConfigText::tr("The selected ONNX model file does not exist or is a symbolic link."));
    }
    if (info.suffix().compare(QStringLiteral("onnx"), Qt::CaseInsensitive) != 0) {
        return fail(ConfigText::tr("Only .onnx model files are supported by AnimeJaNai."));
    }
    const qint64 size = info.size();
    if (size < kMinOnnxFileBytes || size > kMaxOnnxFileBytes) {
        return fail(ConfigText::tr("The ONNX file size (%1 bytes) is outside the valid bounds.")
                        .arg(size));
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(file.errorString());
    }
    // Validate bounded ModelProto framing only; tensor/operator compatibility is runtime-specific.
    bool hasIrVersion = false;
    bool hasGraph = false;
    qint64 scanned = 0;
    while (file.pos() < size && scanned < 1024 * 1024) {
        if (mikai::backend::WorkCancellationScope::cancellationRequested()) {
            return fail(ConfigText::tr("ONNX model validation was cancelled."));
        }
        quint64 tag = 0;
        if (!readProtoVarint(file, size, &scanned, &tag) || tag == 0) {
            return fail(ConfigText::tr("The ONNX ModelProto has a malformed protobuf field tag."));
        }
        const quint64 field = tag >> 3;
        const quint64 wire = tag & 7;
        if (field == 0 || field > 0x1fffffff) {
            return fail(ConfigText::tr("The ONNX ModelProto has an invalid protobuf field number."));
        }
        if (field == 1 && wire != 0) {
            return fail(ConfigText::tr("The ONNX ModelProto ir_version field has an invalid wire type."));
        }
        if (field == 7 && wire != 2) {
            return fail(ConfigText::tr("The ONNX ModelProto graph field has an invalid wire type."));
        }
        if (wire == 0) {
            quint64 value = 0;
            if (!readProtoVarint(file, size, &scanned, &value)) {
                return fail(ConfigText::tr("The ONNX ModelProto has a malformed protobuf varint."));
            }
            if (field == 1) {
                if (value == 0 || value > 100) {
                    return fail(ConfigText::tr("The ONNX ModelProto ir_version field is implausible."));
                }
                hasIrVersion = true;
            }
        } else if (wire == 1 || wire == 5) {
            const qint64 skip = wire == 1 ? 8 : 4;
            if (file.pos() > size - skip || !file.seek(file.pos() + skip)) {
                return fail(ConfigText::tr("The ONNX ModelProto contains a truncated protobuf field."));
            }
        } else if (wire == 2) {
            quint64 length = 0;
            if (!readProtoVarint(file, size, &scanned, &length) ||
                length > static_cast<quint64>(size - file.pos())) {
                return fail(ConfigText::tr("The ONNX ModelProto contains an invalid protobuf field length."));
            }
            if (field == 7) {
                if (length == 0) {
                    return fail(ConfigText::tr("The ONNX ModelProto has an empty graph field."));
                }
                hasGraph = true;
            }
            if (!file.seek(file.pos() + static_cast<qint64>(length))) {
                return fail(ConfigText::tr("The ONNX ModelProto graph field could not be skipped safely."));
            }
        } else {
            return fail(ConfigText::tr("The ONNX ModelProto uses an unsupported protobuf wire type."));
        }
    }
    if (!hasIrVersion || !hasGraph) {
        return fail(ConfigText::tr("The file does not contain plausible ONNX ModelProto ir_version and graph fields."));
    }
    if (bytesOut != nullptr) *bytesOut = size;
    if (reasonOut != nullptr) reasonOut->clear();
    return true;
}

QVector<CustomAnimeJanaiModel> listCustomAnimeJanaiModels() {
    QVector<CustomAnimeJanaiModel> models;
    QSet<QString> seenIds;
    for (const OwnedOnnxEntry& entry : ownedOnnxEntries()) {
        if (entry.id.isEmpty() || seenIds.contains(entry.id.toLower()) ||
            entry.bytes < kMinOnnxFileBytes || entry.bytes > kMaxOnnxFileBytes) {
            continue;
        }
        const OwnedSidecar sidecar = readOwnedSidecar(entry.path, entry.bytes);
        if (sidecar.state != OwnedSidecarState::Valid) continue;

        CustomAnimeJanaiModel model;
        model.id = entry.id;
        model.fileName = entry.fileName;
        model.displayName = entry.rawStem;
        model.filePath = entry.path;
        model.bytes = entry.bytes;
        model.sha256 = sidecar.sha256;
        model.scaleFactor = inferScaleFactor(entry.rawStem);
        model.architectureHint = inferArchitectureHint(entry.rawStem);
        model.valid = true;
        models.push_back(std::move(model));
        seenIds.insert(entry.id.toLower());
    }

    const QVector<ExternalModelPin> pins = readExternalModelPins();
    for (const ExternalModelPin& pin : pins) {
        const QFileInfo entry(pin.filePath);
        if (!entry.isFile() || entry.isSymLink() || entry.size() != pin.bytes ||
            entry.canonicalFilePath().compare(pin.filePath, Qt::CaseInsensitive) != 0 ||
            seenIds.contains(pin.id.toLower())) {
            continue;
        }
        CustomAnimeJanaiModel model;
        model.id = pin.id;
        model.fileName = entry.fileName();
        model.displayName = pin.displayName;
        model.filePath = pin.filePath;
        model.bytes = pin.bytes;
        model.sha256 = pin.sha256;
        model.scaleFactor = inferScaleFactor(pin.displayName);
        model.architectureHint = inferArchitectureHint(pin.displayName);
        model.valid = true;
        models.push_back(std::move(model));
        seenIds.insert(pin.id.toLower());
    }
    std::sort(models.begin(), models.end(),
              [](const CustomAnimeJanaiModel& a, const CustomAnimeJanaiModel& b) {
                  return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
              });
    return models;
}

QVector<CustomAnimeJanaiModel> listUnregisteredOwnedAnimeJanaiModels() {
    QVector<CustomAnimeJanaiModel> candidates;
    const auto pathKey = [](const QString& path) {
        const QString clean = QDir::cleanPath(path);
        return pathCaseSensitivity() == Qt::CaseInsensitive ? clean.toLower() : clean;
    };
    QSet<QString> registeredPaths;
    for (const ExternalModelPin& pin : readExternalModelPins()) {
        registeredPaths.insert(pathKey(pin.filePath));
    }
    for (const OwnedOnnxEntry& entry : ownedOnnxEntries()) {
        if (entry.id.isEmpty() || registeredPaths.contains(pathKey(entry.path))) {
            continue;
        }
        const OwnedSidecar sidecar = readOwnedSidecar(entry.path, entry.bytes);
        const bool withinSizeBounds =
            entry.bytes >= kMinOnnxFileBytes && entry.bytes <= kMaxOnnxFileBytes;
        // A valid record within bounds is already executable through
        // listCustomAnimeJanaiModels(); anything else is still a candidate.
        if (sidecar.state == OwnedSidecarState::Valid && withinSizeBounds) continue;

        CustomAnimeJanaiModel candidate;
        candidate.id = entry.id;
        candidate.fileName = entry.fileName;
        candidate.displayName = entry.rawStem;
        candidate.filePath = entry.path;
        candidate.bytes = entry.bytes;
        candidate.scaleFactor = inferScaleFactor(entry.rawStem);
        candidate.architectureHint = inferArchitectureHint(entry.rawStem);
        candidate.valid = false;
        if (entry.bytes < kMinOnnxFileBytes) {
            candidate.reason = ConfigText::tr(
                "This model file is smaller than a complete ONNX graph; replace it with a finished model before registering it.");
        } else if (entry.bytes > kMaxOnnxFileBytes) {
            candidate.reason = ConfigText::tr(
                "This model file exceeds the supported ONNX size bound.");
        } else {
            candidate.reason = sidecar.reason;
        }
        candidates.push_back(std::move(candidate));
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const CustomAnimeJanaiModel& a, const CustomAnimeJanaiModel& b) {
                  return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
              });
    return candidates;
}

QVector<CustomAnimeJanaiModel> listExternalCustomAnimeJanaiModelCandidates(QString* errorOut) {
    return listExternalCustomAnimeJanaiModelCandidates(
        AnimeJanaiConfigStore::load().externalModelsDir, errorOut);
}

QVector<CustomAnimeJanaiModel> listExternalCustomAnimeJanaiModelCandidates(
    const QString& externalDirectory, QString* errorOut) {
    QVector<CustomAnimeJanaiModel> candidates;
    const QString configuredRoot = externalDirectory.trimmed();
    if (errorOut) errorOut->clear();
    if (configuredRoot.isEmpty()) return candidates;
    const QString externalRoot = QFileInfo(configuredRoot).canonicalFilePath();
    if (externalRoot.isEmpty() || !QFileInfo(externalRoot).isDir()) {
        if (errorOut) *errorOut = ConfigText::tr("The external model directory is not configured or unavailable.");
        return candidates;
    }
    QString registryError;
    const QVector<ExternalModelPin> pins = readExternalModelPins(&registryError);
    QMap<QString, ExternalModelPin> registeredPaths;
    for (const ExternalModelPin& pin : pins) {
        registeredPaths.insert(QDir::cleanPath(pin.filePath), pin);
    }
    const QDir dir(externalRoot);
    const QFileInfoList entries = dir.entryInfoList(
        {QStringLiteral("*.onnx")}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo& entry : entries) {
        const QString path = entry.canonicalFilePath();
        if (path.isEmpty()) continue;
        const auto pin = registeredPaths.constFind(QDir::cleanPath(path));
        if (pin != registeredPaths.cend() && entry.size() == pin->bytes) continue;
        const QString stem = entry.completeBaseName();
        const QString id = pin != registeredPaths.cend() ? pin->id : sanitizeModelStem(stem);
        if (id.isEmpty()) continue;
        CustomAnimeJanaiModel candidate;
        candidate.id = id;
        candidate.fileName = entry.fileName();
        candidate.displayName = stem;
        candidate.filePath = path;
        candidate.bytes = entry.size();
        candidate.scaleFactor = inferScaleFactor(stem);
        candidate.architectureHint = inferArchitectureHint(stem);
        candidate.valid = false;
        candidate.reason = !registryError.isEmpty()
                               ? registryError
                               : (pin != registeredPaths.cend()
                                      ? ConfigText::tr("The external model size changed since it was pinned; explicitly refresh its pin.")
                                      : ConfigText::tr("Pin this external model explicitly before using it."));
        candidates.push_back(std::move(candidate));
    }
    if (errorOut) *errorOut = registryError;
    return candidates;
}

std::optional<CustomAnimeJanaiModel> findCustomAnimeJanaiModel(const QString& idOrStem) {
    QString query = idOrStem.trimmed();
    if (query.startsWith(QStringLiteral("spatial:custom:"), Qt::CaseInsensitive)) {
        query = query.mid(QStringLiteral("spatial:custom:").size());
    } else if (query.startsWith(QStringLiteral("custom:"), Qt::CaseInsensitive)) {
        query = query.mid(QStringLiteral("custom:").size());
    }
    if (query.endsWith(QStringLiteral(".onnx"), Qt::CaseInsensitive)) {
        query.chop(5);
    }
    const QString sanitized = sanitizeModelStem(query);
    for (const CustomAnimeJanaiModel& model : listCustomAnimeJanaiModels()) {
        if (model.id.compare(query, Qt::CaseInsensitive) == 0 ||
            model.id.compare(sanitized, Qt::CaseInsensitive) == 0 ||
            model.displayName.compare(query, Qt::CaseInsensitive) == 0 ||
            model.sha256.compare(query, Qt::CaseInsensitive) == 0) {
            return model;
        }
    }
    return std::nullopt;
}

bool registerExternalCustomAnimeJanaiModel(const QString& sourceFilePath,
                                            CustomAnimeJanaiModel* registeredOut,
                                            QString* errorOut,
                                            const std::function<bool()>& cancelled,
                                            const QString& externalDirectory) {
    const auto fail = [errorOut](const QString& message) {
        if (errorOut) *errorOut = message;
        return false;
    };
    const auto isCancelled = [&] {
        return (cancelled && cancelled()) ||
               mikai::backend::WorkCancellationScope::cancellationRequested();
    };
    if (isCancelled()) {
        return fail(ConfigText::tr("External model registration was cancelled."));
    }
    const QString selectedDirectory = externalDirectory.trimmed().isEmpty()
                                          ? AnimeJanaiConfigStore::load().externalModelsDir.trimmed()
                                          : externalDirectory.trimmed();
    const QString externalRoot = QFileInfo(selectedDirectory).canonicalFilePath();
    if (externalRoot.isEmpty()) {
        return fail(ConfigText::tr("Choose an available external model directory before registering a model."));
    }
    const QFileInfo sourceInfo(sourceFilePath);
    const QString canonicalPath = sourceInfo.canonicalFilePath();
    if (!sourceInfo.isFile() || sourceInfo.isSymLink() || canonicalPath.isEmpty() ||
        !pathWithinRoot(externalRoot, canonicalPath)) {
        return fail(ConfigText::tr("The selected model is not a regular file inside the configured external model directory."));
    }
    qint64 bytes = 0;
    QString validationError;
    if (!validateCustomOnnxFile(canonicalPath, &bytes, &validationError)) {
        return fail(validationError);
    }
    const QString stem = sourceInfo.completeBaseName();
    const QString id = sanitizeModelStem(stem);
    if (id.isEmpty()) {
        return fail(ConfigText::tr("The external ONNX filename cannot be converted into a valid model identifier."));
    }
    for (const CustomAnimeJanaiModel& existing : listCustomAnimeJanaiModels()) {
        if (existing.id.compare(id, Qt::CaseInsensitive) == 0 &&
            existing.filePath.compare(canonicalPath, Qt::CaseInsensitive) != 0) {
            return fail(ConfigText::tr("Another custom model already uses the identifier %1.").arg(id));
        }
    }
    QString registryError;
    QVector<ExternalModelPin> pins = readExternalModelPins(&registryError);
    if (!registryError.isEmpty()) return fail(registryError);
    const auto existing = std::find_if(pins.begin(), pins.end(), [&canonicalPath](const ExternalModelPin& pin) {
        return pin.filePath.compare(canonicalPath, Qt::CaseInsensitive) == 0;
    });
    for (const ExternalModelPin& pin : pins) {
        if (pin.id.compare(id, Qt::CaseInsensitive) == 0 &&
            pin.filePath.compare(canonicalPath, Qt::CaseInsensitive) != 0) {
            return fail(ConfigText::tr("Another external model already uses the identifier %1.").arg(id));
        }
    }
    qint64 hashedBytes = 0;
    const QString digest = hashFileSha256(canonicalPath, &hashedBytes, cancelled);
    if (isCancelled()) {
        return fail(ConfigText::tr("External model registration was cancelled."));
    }
    if (digest.isEmpty() || hashedBytes != bytes || QFileInfo(canonicalPath).size() != bytes) {
        return fail(ConfigText::tr("The external model changed while its integrity pin was being calculated."));
    }
    ExternalModelPin pin{id, canonicalPath, stem, bytes, digest};
    if (isCancelled()) {
        return fail(ConfigText::tr("External model registration was cancelled."));
    }
    if (existing == pins.end()) pins.push_back(pin);
    else *existing = pin;
    QString saveError;
    if (!writeExternalModelPins(pins, &saveError)) return fail(saveError);

    if (registeredOut) {
        registeredOut->id = id;
        registeredOut->fileName = sourceInfo.fileName();
        registeredOut->displayName = stem;
        registeredOut->filePath = canonicalPath;
        registeredOut->bytes = bytes;
        registeredOut->sha256 = digest;
        registeredOut->scaleFactor = inferScaleFactor(stem);
        registeredOut->architectureHint = inferArchitectureHint(stem);
        registeredOut->valid = true;
        registeredOut->reason.clear();
    }
    if (errorOut) errorOut->clear();
    return true;
}

bool registerOwnedCustomAnimeJanaiModel(const QString& sourceFilePath,
                                        CustomAnimeJanaiModel* registeredOut,
                                        QString* errorOut,
                                        const std::function<bool()>& cancelled) {
    const auto fail = [errorOut](const QString& message) {
        if (errorOut) *errorOut = message;
        return false;
    };
    const auto isCancelled = [&] {
        return (cancelled && cancelled()) ||
               mikai::backend::WorkCancellationScope::cancellationRequested();
    };
    if (isCancelled()) {
        return fail(ConfigText::tr("Custom model registration was cancelled."));
    }

    const QString ownedRoot = customAnimeJanaiModelsDir();
    if (!nativePathTrust(ownedRoot, true).trusted ||
        !pathWithinRoot(nativeRoot(), ownedRoot) ||
        !pathHasNoReparsePoints(nativeRoot(), ownedRoot)) {
        return fail(ConfigText::tr("The custom AnimeJaNai models directory is untrusted or redirected."));
    }
    const QString canonicalRoot = QFileInfo(ownedRoot).canonicalFilePath();
    if (canonicalRoot.isEmpty()) {
        return fail(ConfigText::tr("The custom AnimeJaNai models directory is unavailable."));
    }

    const QFileInfo sourceInfo(sourceFilePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile() || sourceInfo.isSymLink()) {
        return fail(ConfigText::tr("The selected ONNX model does not exist or is not an app-owned regular file."));
    }
    const QString canonicalSource = sourceInfo.canonicalFilePath();
    if (canonicalSource.isEmpty() ||
        !nativePathTrust(canonicalSource, true).trusted ||
        QDir::cleanPath(QFileInfo(canonicalSource).absolutePath())
                .compare(QDir::cleanPath(canonicalRoot), pathCaseSensitivity()) != 0 ||
        !pathWithinRoot(canonicalRoot, canonicalSource) ||
        !pathHasNoReparsePoints(nativeRoot(), canonicalSource)) {
        return fail(ConfigText::tr(
            "Only models already inside the app-owned custom model folder can be registered; borrowed external models are pinned in their own store."));
    }

    const QString sidecarPath = canonicalSource + QStringLiteral(".source.json");
    const auto sidecarPresent = [&sidecarPath] {
        const QFileInfo info(sidecarPath);
        return info.exists() || info.isSymLink();
    };
    if (sidecarPresent()) {
        return fail(ConfigText::tr(
            "This model already has an integrity record; remove the registered model before registering it again."));
    }

    const QString cleanSource = QDir::cleanPath(canonicalSource);
    std::optional<OwnedOnnxEntry> selected;
    for (const OwnedOnnxEntry& entry : ownedOnnxEntries()) {
        if (QDir::cleanPath(entry.canonicalPath)
                .compare(cleanSource, pathCaseSensitivity()) == 0) {
            selected = entry;
        }
    }
    if (!selected.has_value()) {
        return fail(ConfigText::tr(
            "The selected model is no longer present in the app-owned custom model folder."));
    }
    if (selected->id.isEmpty()) {
        return fail(ConfigText::tr(
            "The ONNX filename cannot be converted into a valid model identifier."));
    }
    for (const OwnedOnnxEntry& entry : ownedOnnxEntries()) {
        if (!entry.id.isEmpty() &&
            entry.id.compare(selected->id, Qt::CaseInsensitive) == 0 &&
            QDir::cleanPath(entry.canonicalPath)
                    .compare(cleanSource, pathCaseSensitivity()) != 0) {
            return fail(ConfigText::tr(
                "Another model file in the custom folder already uses the identifier %1.")
                            .arg(selected->id));
        }
    }
    QString registryError;
    const QVector<ExternalModelPin> pins = readExternalModelPins(&registryError);
    if (!registryError.isEmpty()) return fail(registryError);
    for (const ExternalModelPin& pin : pins) {
        if (QDir::cleanPath(pin.filePath).compare(cleanSource, pathCaseSensitivity()) == 0) {
            return fail(ConfigText::tr(
                "This model is already registered as an external model pin; remove that pin before registering it in place."));
        }
        if (pin.id.compare(selected->id, Qt::CaseInsensitive) == 0) {
            return fail(ConfigText::tr(
                "Another registered model already uses the identifier %1.")
                            .arg(selected->id));
        }
    }

    qint64 bytes = 0;
    QString validationError;
    if (!validateCustomOnnxFile(canonicalSource, &bytes, &validationError)) {
        return fail(validationError);
    }
    if (isCancelled()) {
        return fail(ConfigText::tr("Custom model registration was cancelled."));
    }

    const QFileInfo beforeHash(canonicalSource);
    const QDateTime modifiedBeforeHash = beforeHash.lastModified();
    qint64 hashedBytes = 0;
    const QString digest = hashFileSha256(canonicalSource, &hashedBytes, cancelled);
    if (isCancelled()) {
        return fail(ConfigText::tr("Custom model registration was cancelled."));
    }
    if (digest.isEmpty()) {
        return fail(ConfigText::tr(
            "The custom model could not be read while its integrity digest was being calculated."));
    }
    const QFileInfo afterHash(canonicalSource);
    if (hashedBytes != bytes ||
        !afterHash.isFile() || afterHash.isSymLink() ||
        afterHash.size() != bytes ||
        afterHash.lastModified() != modifiedBeforeHash ||
        afterHash.canonicalFilePath().compare(canonicalSource, pathCaseSensitivity()) != 0 ||
        !nativePathTrust(canonicalSource, true).trusted ||
        !pathWithinRoot(canonicalRoot, canonicalSource) ||
        !pathHasNoReparsePoints(nativeRoot(), canonicalSource)) {
        return fail(ConfigText::tr(
            "The custom model changed while its integrity record was being prepared."));
    }
    if (sidecarPresent()) {
        return fail(ConfigText::tr(
            "This model already has an integrity record; remove the registered model before registering it again."));
    }

    const QJsonObject provenance{
        {QStringLiteral("id"), selected->id},
        {QStringLiteral("originalFileName"), selected->fileName},
        {QStringLiteral("bytes"), bytes},
        {QStringLiteral("sha256"), digest},
        {QStringLiteral("registeredAt"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    if (isCancelled()) {
        return fail(ConfigText::tr("Custom model registration was cancelled."));
    }
    QSaveFile sidecar(sidecarPath);
    sidecar.setDirectWriteFallback(false);
    const QByteArray provenanceBytes =
        QJsonDocument(provenance).toJson(QJsonDocument::Compact);
    if (!sidecar.open(QIODevice::WriteOnly) ||
        sidecar.write(provenanceBytes) != provenanceBytes.size()) {
        return fail(ConfigText::tr("Could not write the model integrity record: %1")
                        .arg(sidecar.errorString()));
    }
    QString commitError;
    if (!commitMetadataFile(sidecar, &commitError)) {
        return fail(ConfigText::tr("Could not write the model integrity record: %1")
                        .arg(commitError));
    }
    if (isCancelled()) {
        QFile::remove(sidecarPath);
        return fail(ConfigText::tr("Custom model registration was cancelled."));
    }

    if (registeredOut != nullptr) {
        registeredOut->id = selected->id;
        registeredOut->fileName = selected->fileName;
        registeredOut->displayName = selected->rawStem;
        registeredOut->filePath = selected->path;
        registeredOut->bytes = bytes;
        registeredOut->sha256 = digest;
        registeredOut->scaleFactor = inferScaleFactor(selected->rawStem);
        registeredOut->architectureHint = inferArchitectureHint(selected->rawStem);
        registeredOut->valid = true;
        registeredOut->reason.clear();
    }
    if (errorOut != nullptr) errorOut->clear();
    return true;
}

bool importCustomAnimeJanaiModel(const QString& sourceFilePath,
                                 CustomAnimeJanaiModel* importedOut,
                                 QString* errorOut,
                                 const std::function<bool()>& cancelled) {
    const auto fail = [errorOut](const QString& message) {
        if (errorOut != nullptr) *errorOut = message;
        return false;
    };
    const auto isCancelled = [&] {
        return (cancelled && cancelled()) ||
               mikai::backend::WorkCancellationScope::cancellationRequested();
    };
    if (isCancelled()) {
        return fail(ConfigText::tr("Custom model import was cancelled."));
    }

    qint64 bytes = 0;
    QString validationError;
    if (!validateCustomOnnxFile(sourceFilePath, &bytes, &validationError)) {
        return fail(validationError);
    }

    const QFileInfo sourceInfo(sourceFilePath);
    const QString id = sanitizeModelStem(sourceInfo.completeBaseName());
    if (id.isEmpty()) {
        return fail(ConfigText::tr("The selected ONNX filename cannot be converted into a valid model identifier."));
    }

    const QString targetDir = customAnimeJanaiModelsDir();
    const auto targetDirTrusted = [&] {
        return nativePathTrust(targetDir, true).trusted &&
               pathWithinRoot(nativeRoot(), targetDir) &&
               pathHasNoReparsePoints(nativeRoot(), targetDir);
    };
    if (!targetDirTrusted()) {
        return fail(ConfigText::tr("The custom AnimeJaNai models directory is untrusted or redirected."));
    }
    if (!QDir().mkpath(targetDir)) {
        return fail(ConfigText::tr("Could not create the custom AnimeJaNai models directory."));
    }
    if (!targetDirTrusted()) {
        return fail(ConfigText::tr("The custom AnimeJaNai models directory is untrusted or redirected."));
    }

    const QString targetPath = QDir(targetDir).filePath(id + QStringLiteral(".onnx"));
    const QString sidecarPath = targetPath + QStringLiteral(".source.json");
    if (QFileInfo::exists(targetPath) || QFileInfo::exists(sidecarPath)) {
        return fail(ConfigText::tr("A custom model with this identifier already exists; remove it before importing a replacement."));
    }
    for (const CustomAnimeJanaiModel& existing : listCustomAnimeJanaiModels()) {
        if (existing.id.compare(id, Qt::CaseInsensitive) == 0) {
            return fail(ConfigText::tr("Another registered custom model already uses the identifier %1.").arg(id));
        }
    }
    QFile input(sourceFilePath);
    if (!input.open(QIODevice::ReadOnly)) {
        return fail(input.errorString());
    }
    QSaveFile output(targetPath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        return fail(output.errorString());
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 copiedBytes = 0;
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    while (!input.atEnd()) {
        if (isCancelled()) {
            output.cancelWriting();
            return fail(ConfigText::tr("Custom model import was cancelled."));
        }
        const qint64 count = input.read(buffer.data(), buffer.size());
        if (count <= 0 || copiedBytes > bytes - count ||
            output.write(buffer.constData(), count) != count) {
            output.cancelWriting();
            return fail(input.error() != QFile::NoError ? input.errorString() : output.errorString());
        }
        copiedBytes += count;
        hash.addData(QByteArrayView(buffer.constData(), count));
    }
    if (isCancelled()) {
        output.cancelWriting();
        return fail(ConfigText::tr("Custom model import was cancelled."));
    }
    if (copiedBytes != bytes || !output.commit()) {
        output.cancelWriting();
        return fail(input.error() != QFile::NoError ? input.errorString() :
                    ConfigText::tr("The source ONNX model changed while being imported."));
    }

    const QString digest = QString::fromLatin1(hash.result().toHex());
    QString copiedValidationError;
    if (isCancelled()) {
        QFile::remove(targetPath);
        return fail(ConfigText::tr("Custom model import was cancelled."));
    }
    if (!validateCustomOnnxFile(targetPath, nullptr, &copiedValidationError) ||
        QFileInfo(targetPath).size() != bytes) {
        QFile::remove(targetPath);
        return fail(copiedValidationError.isEmpty()
                        ? ConfigText::tr("The imported ONNX model size changed during publication.")
                        : copiedValidationError);
    }
    const QJsonObject provenance{
        {QStringLiteral("id"), id},
        {QStringLiteral("originalFileName"), sourceInfo.fileName()},
        {QStringLiteral("bytes"), bytes},
        {QStringLiteral("sha256"), digest},
        {QStringLiteral("importedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};

    if (isCancelled()) {
        QFile::remove(targetPath);
        return fail(ConfigText::tr("Custom model import was cancelled."));
    }
    QSaveFile sidecar(sidecarPath);
    sidecar.setDirectWriteFallback(false);
    const QByteArray provenanceBytes = QJsonDocument(provenance).toJson(QJsonDocument::Compact);
    if (!sidecar.open(QIODevice::WriteOnly) ||
        sidecar.write(provenanceBytes) != provenanceBytes.size()) {
        const QString sidecarError = sidecar.errorString();
        QFile::remove(targetPath);
        return fail(ConfigText::tr("Could not persist the imported model integrity record: %1").arg(sidecarError));
    }
    QString commitError;
    if (!commitMetadataFile(sidecar, &commitError)) {
        QFile::remove(targetPath);
        return fail(ConfigText::tr("Could not persist the imported model integrity record: %1").arg(commitError));
    }

    if (isCancelled()) {
        QFile::remove(targetPath);
        QFile::remove(sidecarPath);
        return fail(ConfigText::tr("Custom model import was cancelled."));
    }
    if (importedOut != nullptr) {
        importedOut->id = id;
        importedOut->fileName = id + QStringLiteral(".onnx");
        importedOut->displayName = id;
        importedOut->filePath = targetPath;
        importedOut->bytes = bytes;
        importedOut->sha256 = digest;
        importedOut->scaleFactor = inferScaleFactor(id);
        importedOut->architectureHint = inferArchitectureHint(id);
        importedOut->valid = true;
    }
    if (errorOut != nullptr) errorOut->clear();
    return true;
}

bool removeCustomAnimeJanaiModel(const QString& id, QString* errorOut) {
    QString query = id.trimmed();
    if (query.startsWith(QStringLiteral("spatial:custom:"), Qt::CaseInsensitive)) {
        query = query.mid(QStringLiteral("spatial:custom:").size());
    } else if (query.startsWith(QStringLiteral("custom:"), Qt::CaseInsensitive)) {
        query = query.mid(QStringLiteral("custom:").size());
    }
    if (query.endsWith(QStringLiteral(".onnx"), Qt::CaseInsensitive)) query.chop(5);
    query = query.trimmed();
    const QString sanitized = sanitizeModelStem(query);
    if (query.isEmpty() || sanitized.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = ConfigText::tr("No custom model identifier was specified.");
        }
        return false;
    }

    QString digest;
    QVector<OwnedOnnxEntry> ownedMatches;
    for (const OwnedOnnxEntry& entry : ownedOnnxEntries()) {
        if (entry.id.isEmpty()) continue;
        if (entry.fileName.compare(query, pathCaseSensitivity()) == 0 ||
            entry.rawStem.compare(query, pathCaseSensitivity()) == 0 ||
            entry.id.compare(query, pathCaseSensitivity()) == 0 ||
            entry.id.compare(sanitized, pathCaseSensitivity()) == 0) {
            ownedMatches.push_back(entry);
        }
    }
    if (ownedMatches.size() > 1) {
        // Prefer an exact actual-filename/stem match, then the single registered
        // graph; only a genuine tie is reported as ambiguous.
        QVector<OwnedOnnxEntry> exactMatches;
        for (const OwnedOnnxEntry& entry : ownedMatches) {
            if (entry.fileName.compare(query, pathCaseSensitivity()) == 0 ||
                entry.rawStem.compare(query, pathCaseSensitivity()) == 0) {
                exactMatches.push_back(entry);
            }
        }
        if (exactMatches.size() == 1) {
            ownedMatches = exactMatches;
        } else {
            QVector<OwnedOnnxEntry> registeredMatches;
            for (const OwnedOnnxEntry& entry : ownedMatches) {
                if (readOwnedSidecar(entry.path, entry.bytes, false).state ==
                    OwnedSidecarState::Valid) {
                    registeredMatches.push_back(entry);
                }
            }
            if (!registeredMatches.isEmpty()) ownedMatches = registeredMatches;
        }
    }
    if (ownedMatches.size() > 1) {
        QStringList names;
        names.reserve(ownedMatches.size());
        for (const OwnedOnnxEntry& entry : ownedMatches) names.append(entry.fileName);
        if (errorOut != nullptr) {
            *errorOut = ConfigText::tr(
                "The custom model identifier %1 matches more than one file (%2); remove it by exact filename.")
                            .arg(query, names.join(QStringLiteral(", ")));
        }
        return false;
    }
    if (ownedMatches.size() == 1) {
        // Resolve the actual on-disk graph, so a pasted filename with spaces or
        // mixed case is addressed exactly rather than as `<sanitized-id>.onnx`.
        const OwnedOnnxEntry& target = ownedMatches.first();
        const OwnedSidecar sidecar = readOwnedSidecar(target.path, target.bytes, false);
        if (sidecar.state != OwnedSidecarState::Valid) {
            if (errorOut != nullptr) {
                *errorOut = ConfigText::tr(
                    "The custom model %1 is not registered; only registered models can be removed.")
                                .arg(target.fileName);
            }
            return false;
        }
        if (!QFileInfo(target.path).isFile() || QFileInfo(target.path).isSymLink() ||
            !nativePathTrust(target.path, true).trusted ||
            !pathWithinRoot(customAnimeJanaiModelsDir(), target.path) ||
            !pathHasNoReparsePoints(nativeRoot(), target.path)) {
            if (errorOut != nullptr) {
                *errorOut = ConfigText::tr("The custom model path is not an app-owned regular file.");
            }
            return false;
        }
        if (!QFile::remove(target.path)) {
            if (errorOut != nullptr) {
                *errorOut = ConfigText::tr("Could not remove %1.").arg(target.path);
            }
            return false;
        }
        QFile::remove(target.path + QStringLiteral(".source.json"));
        digest = sidecar.sha256;
    } else {
        QString registryError;
        QVector<ExternalModelPin> pins = readExternalModelPins(&registryError);
        if (!registryError.isEmpty()) {
            if (errorOut) *errorOut = registryError;
            return false;
        }
        const auto external = std::find_if(pins.begin(), pins.end(), [&sanitized](const ExternalModelPin& pin) {
            return pin.id.compare(sanitized, Qt::CaseInsensitive) == 0;
        });
        if (external == pins.end()) {
            if (errorOut != nullptr) {
                *errorOut = ConfigText::tr("The custom model %1 does not exist in the registered stores.")
                                .arg(sanitized);
            }
            return false;
        }
        digest = external->sha256;
        pins.erase(external);
        // Borrowed external graphs are never deleted; only their registration is.
        if (!writeExternalModelPins(pins, errorOut)) return false;
    }
    const QString stagedDirPath = stagedSpatialModelsDir();
    if (nativePathTrust(stagedDirPath, true).trusted &&
        pathWithinRoot(nativeRoot(), stagedDirPath) &&
        pathHasNoReparsePoints(nativeRoot(), stagedDirPath)) {
        const QDir stagedDir(stagedDirPath);
        for (const QFileInfo& entry : stagedDir.entryInfoList(QDir::Files | QDir::NoSymLinks)) {
            if (!entry.isFile() || entry.isSymLink() ||
                !entry.fileName().startsWith(digest) ||
                !nativePathTrust(entry.absoluteFilePath(), true).trusted ||
                !pathWithinRoot(stagedDirPath, entry.absoluteFilePath()) ||
                !pathHasNoReparsePoints(nativeRoot(), entry.absoluteFilePath())) {
                continue;
            }
            QFile::remove(entry.absoluteFilePath());
        }
    }

    if (errorOut != nullptr) errorOut->clear();
    return true;
}

QString AnimeJanaiConfigStore::configFilePath() {
    return QDir(animeJanaiRootDir()).filePath(QStringLiteral("animejanai_manager.conf"));
}

AnimeJanaiConfig AnimeJanaiConfigStore::load(QString* error) {
    if (error != nullptr) error->clear();
    const QString path = configFilePath();
    if (!QFileInfo::exists(path)) return {};
    return loadFromFile(path, error);
}

AnimeJanaiConfig AnimeJanaiConfigStore::loadFromFile(const QString& path, QString* error) {
    if (error != nullptr) error->clear();
    const auto fail = [error](const QString& reason) {
        if (error != nullptr) {
            *error = reason;
        } else {
            qWarning().noquote() << "[AnimeJanaiConfig]" << reason;
        }
        return AnimeJanaiConfig{};
    };
    QByteArray bytes;
    QString readError;
    const SharedMetadataReadStatus readStatus =
        readSharedMetadataFile(path, kMaxModelMetadataBytes, &bytes, &readError);
    if (readStatus == SharedMetadataReadStatus::Unreadable) {
        return fail(ConfigText::tr("Could not read AnimeJaNai configuration '%1': %2")
                        .arg(path, readError));
    }
    if (readStatus == SharedMetadataReadStatus::TooLarge) {
        return fail(ConfigText::tr("The AnimeJaNai configuration exceeds the supported size."));
    }
    const QString text = QString::fromUtf8(bytes);
    const ParsedIni ini = parseIniText(text);
    bool recognized = ini.sections.contains(QStringLiteral("global"));
    for (auto section = ini.sections.cbegin(); !recognized && section != ini.sections.cend();
         ++section) {
        if (!section.key().startsWith(QStringLiteral("slot_"))) continue;
        bool valid = false;
        const int number = section.key().mid(5).toInt(&valid);
        recognized = valid && number >= 1 && number <= 10;
    }
    if (!recognized) {
        return fail(ConfigText::tr("The file contains no AnimeJaNai global settings or custom profile slots."));
    }
    return parse(text, error);
}

AnimeJanaiConfig AnimeJanaiConfigStore::parse(QStringView text, QString* error) {
    const ParsedIni ini = parseIniText(text);
    AnimeJanaiConfig config;
    config.enableLogging =
        isYes(iniValue(ini, QStringLiteral("global"), QStringLiteral("logging"), QStringLiteral("yes")));
    config.externalModelsDir =
        iniValue(ini, QStringLiteral("global"), QStringLiteral("external_models_dir"));

    const int version =
        toInt(iniValue(ini, QStringLiteral("global"), QStringLiteral("config_version"), QStringLiteral("1")), 1);
    const QString extensionVersion =
        iniValue(ini, QStringLiteral("global"), QStringLiteral("mikai_chain_steps_version"));
    if (!extensionVersion.isEmpty()) {
        bool extensionOk = false;
        const int parsed = extensionVersion.toInt(&extensionOk);
        if (!extensionOk || parsed != 1) {
            if (error) *error = ConfigText::tr("The AnimeJaNai mixed-chain extension version is unsupported or malformed.");
            return {};
        }
    }
    QString trt = iniValue(ini, QStringLiteral("global"), QStringLiteral("trt_engine_settings"));
    if (version < 2 &&
        (trt == QString::fromLatin1(kDefaultTrtSettings) ||
         trt == QString::fromLatin1(kLegacyTrtSettings))) {
        trt.clear();
    }
    static const QRegularExpression precisionRx(QStringLiteral("--(fp16|bf16)\\b"));
    if (version < 3 && precisionRx.match(trt).hasMatch()) {
        trt.replace(precisionRx, QStringLiteral("--stronglyTyped"));
        trt.replace(QRegularExpression(QStringLiteral("--stronglyTyped(\\s+--stronglyTyped)+")),
                    QStringLiteral("--stronglyTyped"));
    }
    if (!trt.trimmed().isEmpty()) {
        config.trtEngineSettings = trt.trimmed();
    }

    const QString legacyPreset =
        iniValue(ini, QStringLiteral("global"), QStringLiteral("default_preset"), QStringLiteral("standard"));
    const auto isSharp = [&](const QString& key) {
        return iniValue(ini, QStringLiteral("global"), key, legacyPreset)
                   .compare(QStringLiteral("sharp"), Qt::CaseInsensitive) == 0;
    };
    config.qualitySharp = isSharp(QStringLiteral("quality_preset"));
    config.balancedSharp = isSharp(QStringLiteral("balanced_preset"));
    config.performanceSharp = isSharp(QStringLiteral("performance_preset"));
    config.customSlots = parseSlotsFromIni(ini, false, error);
    if (error && !error->isEmpty()) return {};
    if (error) error->clear();
    return config;
}

QString AnimeJanaiConfigStore::serializeSlotSection(const AnimeJanaiSlot& slot,
                                                    int overrideSlotNumber) {
    QString output;
    if (overrideSlotNumber == 0) {
        output += QStringLiteral("[slot]\n");
    } else {
        const int num = overrideSlotNumber > 0 ? overrideSlotNumber : slot.number;
        output += QStringLiteral("[slot_%1]\n").arg(num);
    }
    putLine(output, QStringLiteral("profile_name"), slot.profileName);
    if (!slot.descriptionText.isEmpty()) {
        putLine(output, QStringLiteral("description"), slot.descriptionText);
    }
    const bool hasMixedSteps = std::any_of(
        slot.chains.cbegin(), slot.chains.cend(), [](const AnimeJanaiChain& chain) {
            return std::any_of(chain.steps.cbegin(), chain.steps.cend(),
                               [](const AnimeJanaiChainStep& step) {
                                   return step.kind == AnimeJanaiStepKind::Anime4K;
                               });
        });
    if (hasMixedSteps)
        putLine(output, QStringLiteral("mikai_chain_steps_version"), QStringLiteral("1"));

    QVector<const AnimeJanaiChain*> chains;
    chains.reserve(slot.chains.size());
    for (const AnimeJanaiChain& chain : slot.chains) chains.push_back(&chain);
    std::sort(chains.begin(), chains.end(),
              [](const AnimeJanaiChain* a, const AnimeJanaiChain* b) { return a->number < b->number; });

    for (const AnimeJanaiChain* chainPtr : chains) {
        const AnimeJanaiChain& chain = *chainPtr;
        const QString p = QStringLiteral("chain_%1_").arg(chain.number);
        putLine(output, p + QStringLiteral("min_resolution"),
                chain.minResolution.isEmpty() ? QStringLiteral("0x0") : chain.minResolution);
        putLine(output, p + QStringLiteral("max_resolution"),
                chain.maxResolution.isEmpty() ? QStringLiteral("0x0") : chain.maxResolution);
        putLine(output, p + QStringLiteral("min_fps"), QString::number(chain.minFps));
        putLine(output, p + QStringLiteral("max_fps"), QString::number(chain.maxFps));

        const bool mixed = std::any_of(chain.steps.cbegin(), chain.steps.cend(),
                                       [](const AnimeJanaiChainStep& step) {
                                           return step.kind == AnimeJanaiStepKind::Anime4K;
                                       });
        QVector<const AnimeJanaiChainStep*> steps;
        steps.reserve(chain.steps.size());
        for (const AnimeJanaiChainStep& step : chain.steps) steps.push_back(&step);
        std::sort(steps.begin(), steps.end(),
                  [](const AnimeJanaiChainStep* a, const AnimeJanaiChainStep* b) {
                      return a->number < b->number;
                  });
        int legacyModelNumber = 0;
        for (const AnimeJanaiChainStep* stepPtr : steps) {
            const AnimeJanaiChainStep& step = *stepPtr;
            if (step.kind == AnimeJanaiStepKind::OnnxModel) {
                const QString m = p + QStringLiteral("model_%1_").arg(++legacyModelNumber);
                putLine(output, m + QStringLiteral("resize_height_before_upscale"),
                        QString::number(step.resizeHeightBeforeUpscale));
                putLine(output, m + QStringLiteral("resize_factor_before_upscale"),
                        QString::number(step.resizeFactorBeforeUpscale));
                putLine(output, m + QStringLiteral("name"), step.id);
            }
            if (mixed) {
                const QString s = p + QStringLiteral("step_%1_").arg(step.number);
                putLine(output, s + QStringLiteral("kind"),
                        step.kind == AnimeJanaiStepKind::OnnxModel ? QStringLiteral("onnx") :
                        step.kind == AnimeJanaiStepKind::Anime4K ? QStringLiteral("anime4k") :
                        QStringLiteral("unsupported"));
                putLine(output, s + QStringLiteral("id"), step.id);
                putLine(output, s + QStringLiteral("scale_factor"), QString::number(step.scaleFactor));
                putLine(output, s + QStringLiteral("resize_height_before_upscale"),
                        QString::number(step.resizeHeightBeforeUpscale));
                putLine(output, s + QStringLiteral("resize_factor_before_upscale"),
                        QString::number(step.resizeFactorBeforeUpscale));
            }
        }
        putLine(output, p + QStringLiteral("rife"),
                chain.rifeEnabled ? QStringLiteral("yes") : QStringLiteral("no"));
        putLine(output, p + QStringLiteral("rife_model"), rifeLabelToValue(chain.rifeModel));
        putLine(output, p + QStringLiteral("rife_factor_numerator"),
                QString::number(std::max(1, chain.rifeFactorNumerator)));
        putLine(output, p + QStringLiteral("rife_factor_denominator"),
                QString::number(std::max(1, chain.rifeFactorDenominator)));
        putLine(output, p + QStringLiteral("rife_scene_detect_threshold"),
                QLocale::c().toString(chain.rifeSceneDetectThreshold, 'g', 6));
        putLine(output, p + QStringLiteral("rife_ensemble"),
                chain.rifeEnsemble ? QStringLiteral("yes") : QStringLiteral("no"));
        putLine(output, p + QStringLiteral("rife_before_upscale"),
                chain.rifeBeforeUpscale ? QStringLiteral("yes") : QStringLiteral("no"));
    }
    return output;
}

QString AnimeJanaiConfigStore::serialize(const AnimeJanaiConfig& config) {
    QString output = QStringLiteral("[global]\n");
    putLine(output, QStringLiteral("config_version"), QStringLiteral("3"));
    const bool hasMixedSteps = std::any_of(
        config.customSlots.cbegin(), config.customSlots.cend(), [](const AnimeJanaiSlot& slot) {
            return std::any_of(slot.chains.cbegin(), slot.chains.cend(),
                               [](const AnimeJanaiChain& chain) {
                                   return std::any_of(chain.steps.cbegin(), chain.steps.cend(),
                                                      [](const AnimeJanaiChainStep& step) {
                                                          return step.kind == AnimeJanaiStepKind::Anime4K;
                                                      });
                               });
        });
    if (hasMixedSteps)
        putLine(output, QStringLiteral("mikai_chain_steps_version"), QStringLiteral("1"));
    putLine(output, QStringLiteral("logging"),
            config.enableLogging ? QStringLiteral("yes") : QStringLiteral("no"));
    if (!config.externalModelsDir.trimmed().isEmpty()) {
        putLine(output, QStringLiteral("external_models_dir"), config.externalModelsDir.trimmed());
    }
    if (!config.trtEngineSettings.trimmed().isEmpty() &&
        config.trtEngineSettings.trimmed() != QString::fromLatin1(kDefaultTrtSettings)) {
        putLine(output, QStringLiteral("trt_engine_settings"), config.trtEngineSettings.trimmed());
    }
    if (config.qualitySharp) putLine(output, QStringLiteral("quality_preset"), QStringLiteral("sharp"));
    if (config.balancedSharp) putLine(output, QStringLiteral("balanced_preset"), QStringLiteral("sharp"));
    if (config.performanceSharp) putLine(output, QStringLiteral("performance_preset"), QStringLiteral("sharp"));

    QVector<const AnimeJanaiSlot*> orderedSlots;
    orderedSlots.reserve(config.customSlots.size());
    for (const AnimeJanaiSlot& slot : config.customSlots) {
        if (slot.number >= 1 && slot.number <= 10) orderedSlots.push_back(&slot);
    }
    std::sort(orderedSlots.begin(), orderedSlots.end(),
              [](const AnimeJanaiSlot* a, const AnimeJanaiSlot* b) { return a->number < b->number; });
    for (const AnimeJanaiSlot* slot : orderedSlots) {
        output += u'\n';
        output += serializeSlotSection(*slot, slot->number);
    }
    return output;
}

bool AnimeJanaiConfigStore::save(const AnimeJanaiConfig& config, QString* error) {
    if (!validateAnimeJanaiConfig(config, error)) return false;
    const QString root = animeJanaiRootDir();
    const auto rootTrusted = [&] {
        return nativePathTrust(root, true).trusted &&
               pathWithinRoot(nativeRoot(), root) &&
               pathHasNoReparsePoints(nativeRoot(), root);
    };
    if (!rootTrusted()) {
        if (error != nullptr) *error = ConfigText::tr("The AnimeJaNai config directory is untrusted or redirected.");
        return false;
    }
    if (!QDir().mkpath(root) || !rootTrusted()) {
        if (error != nullptr) *error = ConfigText::tr("The AnimeJaNai config directory could not be safely created.");
        return false;
    }
    return saveToFile(configFilePath(), config, error);
}

bool AnimeJanaiConfigStore::saveToFile(const QString& path, const AnimeJanaiConfig& config,
                                       QString* error) {
    if (!validateAnimeJanaiConfig(config, error)) return false;
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error != nullptr) {
            *error = ConfigText::tr("Could not create directory for %1.").arg(path);
        }
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    const QByteArray bytes = serialize(config).toUtf8();
    if (file.write(bytes) != bytes.size()) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    if (!commitMetadataFile(file, error)) return false;
    if (error != nullptr) error->clear();
    return true;
}

AnimeJanaiSlot AnimeJanaiConfigStore::loadProfile(const QString& path, bool* ok, QString* error) {
    QFile file(path);
    if (!file.exists()) {
        if (ok != nullptr) *ok = false;
        if (error != nullptr) *error = file.errorString();
        return {};
    }
    QByteArray bytes;
    QString readError;
    const SharedMetadataReadStatus readStatus =
        readSharedMetadataFile(path, kMaxModelMetadataBytes, &bytes, &readError);
    if (readStatus == SharedMetadataReadStatus::Unreadable) {
        if (ok != nullptr) *ok = false;
        if (error != nullptr) *error = readError;
        return {};
    }
    if (readStatus == SharedMetadataReadStatus::TooLarge) {
        if (ok != nullptr) *ok = false;
        if (error != nullptr) *error = ConfigText::tr("The profile configuration exceeds the supported metadata size.");
        return {};
    }
    return parseProfile(QString::fromUtf8(bytes), ok, error);
}

AnimeJanaiSlot AnimeJanaiConfigStore::parseProfile(QStringView text, bool* ok, QString* error) {
    const QVector<AnimeJanaiSlot> slotsList = parseSlotsFromIni(parseIniText(text), true, error);
    if (error && !error->isEmpty()) {
        if (ok) *ok = false;
        return {};
    }
    if (slotsList.isEmpty()) {
        if (ok != nullptr) *ok = false;
        if (error != nullptr) {
            *error = ConfigText::tr("The profile configuration does not contain a [slot] section.");
        }
        return {};
    }
    if (ok != nullptr) *ok = true;
    if (error != nullptr) error->clear();
    return slotsList.front();
}

bool AnimeJanaiConfigStore::saveProfile(const QString& path, const AnimeJanaiSlot& slot,
                                        QString* error) {
    for (const AnimeJanaiChain& chain : slot.chains) {
        if (!validateAnimeJanaiChainSteps(chain, error)) return false;
    }
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error != nullptr) *error = ConfigText::tr("Could not create directory for %1.").arg(path);
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    const QByteArray bytes = serializeProfile(slot).toUtf8();
    if (file.write(bytes) != bytes.size()) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    if (!commitMetadataFile(file, error)) return false;
    if (error != nullptr) error->clear();
    return true;
}

QString AnimeJanaiConfigStore::serializeProfile(const AnimeJanaiSlot& slot) {
    return serializeSlotSection(slot, 0);
}

QVector<AnimeJanaiSlot> AnimeJanaiConfigStore::defaultProfiles(const AnimeJanaiConfig& config) {
    static const QString sd = QStringLiteral(
        "2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo");
    const auto makeModel = [](int number, const QString& id) {
        AnimeJanaiChainStep step;
        step.number = number;
        step.id = id;
        return step;
    };
    const auto makeChain = [](int number, const QString& minRes, const QString& maxRes, int maxFps,
                              const QVector<AnimeJanaiChainStep>& steps) {
        AnimeJanaiChain c;
        c.number = number;
        c.minResolution = minRes;
        c.maxResolution = maxRes;
        c.maxFps = maxFps;
        c.steps = steps;
        return c;
    };
    const auto hdBalanced = [](bool sharp) {
        return sharp
                   ? QStringLiteral("2x_AnimeJaNai_HD_V3.1Sharp1_Balanced_SPANF3_b8f64_unshuffle_fp16")
                   : QStringLiteral("2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16");
    };
    const auto hdPerf = [](bool sharp) {
        return sharp
                   ? QStringLiteral("2x_AnimeJaNai_HD_V3.1Sharp1_Performance_SPANF3_b5f48_unshuffle_fp16")
                   : QStringLiteral("2x_AnimeJaNai_HD_V3.1_Performance_SPANF3_b5f48_unshuffle_fp16");
    };

    QVector<AnimeJanaiSlot> profiles;
    {
        AnimeJanaiSlot quality;
        quality.number = 1001;
        quality.profileName = QStringLiteral("Quality");
        quality.descriptionText = QStringLiteral("Adaptive HD/SD 2x upscaling up to 60 fps");
        quality.chains = {
            makeChain(1, QStringLiteral("1280x720"), QStringLiteral("1920x1080"), 31,
                      {makeModel(1, hdBalanced(config.qualitySharp))}),
            makeChain(2, QStringLiteral("0x0"), QStringLiteral("1280x720"), 31,
                      {makeModel(1, sd)}),
            makeChain(3, QStringLiteral("0x0"), QStringLiteral("1920x1080"), 61,
                      {makeModel(1, hdBalanced(config.qualitySharp))})};
        profiles.push_back(std::move(quality));
    }
    {
        AnimeJanaiSlot balanced;
        balanced.number = 1002;
        balanced.profileName = QStringLiteral("Balanced");
        balanced.descriptionText = QStringLiteral("Adaptive HD Balanced + SD Compact 2x upscaling");
        balanced.chains = {
            makeChain(1, QStringLiteral("1280x720"), QStringLiteral("1920x1080"), 31,
                      {makeModel(1, hdBalanced(config.balancedSharp))}),
            makeChain(2, QStringLiteral("0x0"), QStringLiteral("1280x720"), 31,
                      {makeModel(1, sd)})};
        profiles.push_back(std::move(balanced));
    }
    {
        AnimeJanaiSlot perf;
        perf.number = 1003;
        perf.profileName = QStringLiteral("Performance");
        perf.descriptionText = QStringLiteral("Lightweight HD Performance + SD Compact 2x upscaling");
        perf.chains = {
            makeChain(1, QStringLiteral("1280x720"), QStringLiteral("1920x1080"), 31,
                      {makeModel(1, hdPerf(config.performanceSharp))}),
            makeChain(2, QStringLiteral("0x0"), QStringLiteral("1280x720"), 31,
                      {makeModel(1, sd)})};
        profiles.push_back(std::move(perf));
    }
    return profiles;
}

QStringList AnimeJanaiConfigStore::availableUpscaleModelNames() {
    QStringList names;
    QSet<QString> seen;
    for (const SpatialAsset& asset : spatialAssetCatalog()) {
        QString stem = asset.name;
        if (stem.endsWith(QStringLiteral(".onnx"), Qt::CaseInsensitive)) stem.chop(5);
        if (!seen.contains(stem.toLower())) {
            names.append(stem);
            seen.insert(stem.toLower());
        }
    }
    for (const CustomAnimeJanaiModel& custom : listCustomAnimeJanaiModels()) {
        if (!seen.contains(custom.id.toLower())) {
            names.append(custom.id);
            seen.insert(custom.id.toLower());
        }
    }
    return names;
}

QStringList AnimeJanaiConfigStore::availableRifeModelLabels() {
    QStringList labels;
    QSet<QString> seen;
    for (const RifeModelPin& pin : rifeModelPins()) {
        if (pin.ensemble) continue;
        const int code = rifeModelCode(pin.version, pin.variant);
        const QString label = rifeValueToLabel(QString::number(code));
        if (!label.isEmpty() && !seen.contains(label)) {
            labels.append(label);
            seen.insert(label);
        }
    }
    return labels;
}

QString AnimeJanaiConfigStore::rifeLabelToValue(QStringView label) {
    const QString text = label.toString().trimmed();
    if (text.isEmpty()) return QStringLiteral("426");
    if (text.compare(QStringLiteral("RIFE 4.26 Heavy"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("4262");
    }
    static const QRegularExpression rx(
        QStringLiteral("^RIFE\\s+v?(\\d+)\\.(\\d+)(?:\\s+(Lite|Heavy))?$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = rx.match(text);
    if (!match.hasMatch()) return text;
    const QString suffix = match.captured(3).toLower();
    return match.captured(1) + match.captured(2) +
           (suffix == QStringLiteral("lite")
                ? QStringLiteral("1")
                : (suffix == QStringLiteral("heavy") ? QStringLiteral("2") : QString()));
}

bool AnimeJanaiConfigStore::validateAnimeJanaiChainSteps(const AnimeJanaiChain& chain,
                                                          QString* error) {
    for (qsizetype index = 0; index < chain.steps.size(); ++index) {
        const AnimeJanaiChainStep& step = chain.steps[index];
        if (step.number != index + 1) {
            if (error) *error = ConfigText::tr("AnimeJaNai chain step numbers must be contiguous and ordered.");
            return false;
        }
        if (step.kind == AnimeJanaiStepKind::Unsupported) {
            if (error) *error = ConfigText::tr("The AnimeJaNai chain contains an unsupported step kind.");
            return false;
        }
        if (step.kind != AnimeJanaiStepKind::OnnxModel &&
            step.kind != AnimeJanaiStepKind::Anime4K) {
            if (error) *error = ConfigText::tr("The AnimeJaNai chain contains an unsupported step kind.");
            return false;
        }
        if (step.id.trimmed().isEmpty()) {
            if (error) *error = ConfigText::tr("The AnimeJaNai chain contains a step without an identifier.");
            return false;
        }
        if (step.scaleFactor != 1 && step.scaleFactor != 2 && step.scaleFactor != 4) {
            if (error) *error = ConfigText::tr("The AnimeJaNai chain scale factor must be 1, 2, or 4.");
            return false;
        }
    }
    if (error) error->clear();
    return true;
}

bool AnimeJanaiConfigStore::validateAnimeJanaiConfig(const AnimeJanaiConfig& config,
                                                      QString* error) {
    for (const AnimeJanaiSlot& slot : config.customSlots) {
        for (const AnimeJanaiChain& chain : slot.chains) {
            if (!validateAnimeJanaiChainSteps(chain, error)) return false;
        }
    }
    if (error) error->clear();
    return true;
}

QString AnimeJanaiConfigStore::rifeValueToLabel(QStringView value) {
    const QString v = value.toString().trimmed();
    if (v.isEmpty()) return QStringLiteral("RIFE 4.26");
    if (v.startsWith(QStringLiteral("RIFE "), Qt::CaseInsensitive)) return v;
    if (v == QStringLiteral("4262")) return QStringLiteral("RIFE 4.26 Heavy");
    if (v.size() < 2) return v;
    const QString decimal = v.size() == 2 ? v.mid(1, 1) : v.mid(1, 2);
    QString label = QStringLiteral("RIFE %1.%2").arg(v.front()).arg(decimal);
    if (v.size() >= 4 && v.endsWith(u'1')) label += QStringLiteral(" Lite");
    return label;
}

bool AnimeJanaiConfigStore::trtDynamicSelected(QStringView settings) {
    static const QRegularExpression rx(
        QStringLiteral("--minShapes=\\S+\\s+--optShapes=\\S+\\s+--maxShapes=\\S+"));
    return rx.match(settings.toString()).hasMatch();
}

bool AnimeJanaiConfigStore::trtStaticSelected(QStringView settings) {
    return settings.toString().contains(QStringLiteral("--optShapes=input:%video_resolution%"));
}

bool AnimeJanaiConfigStore::trtStaticOnnxSelected(QStringView settings) {
    const QString s = settings.toString();
    return !s.contains(QStringLiteral("--minShapes")) &&
           !s.contains(QStringLiteral("--optShapes")) &&
           !s.contains(QStringLiteral("--maxShapes"));
}

QString AnimeJanaiConfigStore::trtDynamicMinResolution(QStringView settings) {
    static const QRegularExpression rx(QStringLiteral("--minShapes=(\\S+)"));
    const QRegularExpressionMatch match = rx.match(settings.toString());
    return match.hasMatch() ? shapeResolution(match.captured(1), QStringLiteral("0x0"))
                            : QStringLiteral("0x0");
}

QString AnimeJanaiConfigStore::trtDynamicOptResolution(QStringView settings) {
    static const QRegularExpression rx(QStringLiteral("--optShapes=(\\S+)"));
    const QRegularExpressionMatch match = rx.match(settings.toString());
    return match.hasMatch() ? shapeResolution(match.captured(1), QStringLiteral("1920x1080"))
                            : QStringLiteral("1920x1080");
}

QString AnimeJanaiConfigStore::trtDynamicMaxResolution(QStringView settings) {
    static const QRegularExpression rx(QStringLiteral("--maxShapes=(\\S+)"));
    const QRegularExpressionMatch match = rx.match(settings.toString());
    return match.hasMatch() ? shapeResolution(match.captured(1), QStringLiteral("1920x1080"))
                            : QStringLiteral("1920x1080");
}

QString AnimeJanaiConfigStore::setTrtDynamicMinResolution(QString settings,
                                                          QStringView resolution) {
    const QString shape = QStringLiteral("--minShapes=input:%1").arg(resolutionShape(resolution));
    return replaceOrInsertSetting(std::move(settings),
                                  QStringLiteral("--minShapes=input:\\d+x\\d+x\\d+x\\d+"), shape,
                                  shape);
}

QString AnimeJanaiConfigStore::setTrtDynamicOptResolution(QString settings,
                                                          QStringView resolution) {
    const QString shape = QStringLiteral("--optShapes=input:%1").arg(resolutionShape(resolution));
    return replaceOrInsertSetting(std::move(settings), QStringLiteral("--optShapes=\\S+"), shape,
                                  shape);
}

QString AnimeJanaiConfigStore::setTrtDynamicMaxResolution(QString settings,
                                                          QStringView resolution) {
    const QString shape = QStringLiteral("--maxShapes=input:%1").arg(resolutionShape(resolution));
    return replaceOrInsertSetting(std::move(settings),
                                  QStringLiteral("--maxShapes=input:\\d+x\\d+x\\d+x\\d+"), shape,
                                  shape);
}

QString AnimeJanaiConfigStore::trtBuilderOptimizationLevel(QStringView settings) {
    static const QRegularExpression rx(QStringLiteral("--builderOptimizationLevel=(\\S+)"));
    const QRegularExpressionMatch match = rx.match(settings.toString());
    return match.hasMatch() ? match.captured(1) : QStringLiteral("5");
}

QString AnimeJanaiConfigStore::setTrtBuilderOptimizationLevel(QString settings,
                                                              QStringView level) {
    const QString flag = QStringLiteral("--builderOptimizationLevel=%1").arg(level.toString());
    return replaceOrInsertSetting(std::move(settings),
                                  QStringLiteral("--builderOptimizationLevel=\\S+"), flag, flag);
}

QString AnimeJanaiConfigStore::setTrtStaticOnnx(QString settings) {
    return settings
        .replace(QRegularExpression(
                     QStringLiteral("\\s*--(min|opt|max)Shapes=input:(\\d+x\\d+x\\d+x\\d+|%video_resolution%)")),
                 QString())
        .trimmed();
}

QString AnimeJanaiConfigStore::setTrtStatic(QString settings) {
    settings = setTrtStaticOnnx(std::move(settings));
    return replaceOrInsertSetting(std::move(settings), QStringLiteral("--optShapes=\\S+"),
                                  QStringLiteral("--optShapes=input:%video_resolution%"),
                                  QStringLiteral("--optShapes=input:%video_resolution%"));
}

QString AnimeJanaiConfigStore::setTrtDynamic(QString settings) {
    settings = setTrtStaticOnnx(std::move(settings));
    const QString shapes =
        QStringLiteral("--minShapes=input:1x3x16x16 --optShapes=input:1x3x1080x1920 --maxShapes=input:1x3x1080x1920");
    return replaceOrInsertSetting(std::move(settings), QStringLiteral("--optShapes=\\S+"), shapes,
                                  shapes);
}

}  // namespace mplayer::native
