#include "NativeBundle.hpp"

#include "native/bundle/NativePaths.hpp"
#include "native/bundle/NativePeImage.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>

namespace mplayer {
namespace native {
namespace {

constexpr const char* kSchemaId = "mikai.native.bundle/1";
constexpr const char* kManifestFileName = "bundle.json";
constexpr const char* kStagingPrefix = ".staging-";

// The mpv client API this build can drive. A core built against an older API is
// missing entry points this code may call, and a newer one may have changed
// them; both are refused rather than attempted.
constexpr quint64 kMinSupportedMpvApiMajor = 2;
constexpr quint64 kMinSupportedMpvApiMinor = 1;
constexpr quint64 kMaxSupportedMpvApiMinor = 5;

// A Standard bundle carries neither (0/0); a Native bundle must carry exactly
// these, so a bundle built for a later protocol is a named refusal rather than a
// load-and-guess.
constexpr int kNativeProtocolVersion = 1;
constexpr int kNativeEngineSchemaVersion = 1;

const QRegularExpression& versionPattern() {
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z][0-9A-Za-z.-]*)?$"));
    return pattern;
}

const QRegularExpression& sha256Pattern() {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern;
}

const QRegularExpression& sha1Pattern() {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{40}$"));
    return pattern;
}

/// The placeholder an unsubstituted layout example carries: forty zeroes is a
/// well-formed commit shape no real commit uses, so it can never be mistaken for
/// a measured source pin.
const QString& reservedCoreSha() {
    static const QString value(40, QLatin1Char('0'));
    return value;
}

/// A single path segment safe to use as a directory or file name of ours: the
/// published directory name, the staging name and the pointer content all go
/// through this, so a crafted manifest can never steer a write.
const QRegularExpression& tokenPattern() {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9._-]{1,96}$"));
    return pattern;
}

void setReason(QString* reason, const QString& text) {
    if (reason) *reason = text;
}

QString mpvApiVersionText(quint64 version) {
    return QStringLiteral("%1.%2")
        .arg(version >> 16)
        .arg(version & 0xFFFFu, 4, 10, QLatin1Char('0'));
}

bool jsonInteger(const QJsonValue& value, qint64* out) {
    if (!value.isDouble()) return false;
    const double d = value.toDouble();
    if (!std::isfinite(d)) return false;
    if (std::floor(d) != d) return false;
    if (d < -9.2e18 || d > 9.2e18) return false;
    *out = static_cast<qint64>(d);
    return true;
}

bool jsonString(const QJsonObject& object, const QString& key, QString* out) {
    const QJsonValue value = object.value(key);
    if (!value.isString()) return false;
    *out = value.toString();
    return true;
}

/// Refuses a member this schema version does not define. A silently ignored
/// typo ("modulePath", "sha2560") produces a bundle whose manifest says
/// something other than what was verified.
bool noUnknownKeys(const QJsonObject& object, const QStringList& known, QString* offender) {
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (known.contains(it.key())) continue;
        if (offender) *offender = it.key();
        return false;
    }
    return true;
}

QStringList manifestKeys() {
    return {QStringLiteral("schema"),   QStringLiteral("kind"),     QStringLiteral("version"),
            QStringLiteral("module"),   QStringLiteral("mpvApiVersion"),
            QStringLiteral("protocolVersion"), QStringLiteral("engineSchemaVersion"),
            QStringLiteral("deps"),      QStringLiteral("files"),
            QStringLiteral("preparation")};
}

QStringList preparationKeys() {
    return {QStringLiteral("types"), QStringLiteral("source"), QStringLiteral("tools"),
            QStringLiteral("runtime")};
}

QStringList preparationSourceKeys() {
    return {QStringLiteral("coreSha"), QStringLiteral("sourceAbi")};
}

QStringList preparationToolKeys() {
    return {QStringLiteral("inspect"), QStringLiteral("prep"), QStringLiteral("verifier")};
}

QStringList preparationRuntimeKeys() {
    return {QStringLiteral("ort"), QStringLiteral("trt"), QStringLiteral("cuda")};
}

/// The preparation block's tools, producer and declared backends must all be
/// files this bundle's inventory declares. Pure manifest reasoning: no payload
/// byte is read, so this holds for a parsed bundle before validation touches the
/// filesystem.
bool preparationInventoryCoherent(const Bundle& bundle, QString* reason) {
    const BundlePreparation& preparation = *bundle.preparation;
    const auto declared = [&bundle](const QString& path) {
        return std::any_of(bundle.files.cbegin(), bundle.files.cend(),
                           [&path](const BundleFile& file) {
                               return file.path.compare(path, pathCaseSensitivity()) == 0;
                           });
    };
    const auto declaresLibrary = [&bundle](const QString& library) {
        return std::any_of(bundle.files.cbegin(), bundle.files.cend(),
                           [&library](const BundleFile& file) {
                               return QFileInfo(file.path).fileName().compare(
                                          library, pathCaseSensitivity()) == 0;
                           });
    };
    const QStringList tools{preparation.tools.inspect, preparation.tools.prep,
                            preparation.tools.verifier};
    for (const QString& tool : tools) {
        if (!declared(tool)) {
            setReason(reason, QStringLiteral("preparation tool '%1' is not in the file inventory")
                                  .arg(tool));
            return false;
        }
    }
    // The dispatcher owns the source ABI every backend is reached through; a
    // block without it names no producer the ABI could be checked against.
    if (!declaresLibrary(bundlePreparationProducerLibrary())) {
        setReason(reason, QStringLiteral("preparation block needs '%1' in the bundle, which declares "
                                         "none")
                              .arg(bundlePreparationProducerLibrary()));
        return false;
    }
    // A declared backend type without its library would let a caller build for a
    // provider the bundle cannot run.
    for (const QString& type : preparation.types) {
        const QString library = bundlePreparationBackendLibrary(type);
        if (library.isEmpty()) {
            setReason(reason, QStringLiteral("preparation type '%1' is not one this build can prepare")
                                  .arg(type));
            return false;
        }
        if (!declaresLibrary(library)) {
            setReason(reason, QStringLiteral("preparation type '%1' needs '%2' in the bundle, which "
                                             "declares none")
                                  .arg(type, library));
            return false;
        }
    }
    return true;
}

QStringList fileKeys() {
    return {QStringLiteral("path"), QStringLiteral("bytes"), QStringLiteral("sha256")};
}

QString sha256OfFile(const QString& path, qint64* bytes, QString* reason) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setReason(reason, QStringLiteral("%1 cannot be read: %2").arg(path, file.errorString()));
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total = 0;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1 << 16);
        if (chunk.isEmpty()) break;
        total += chunk.size();
        hash.addData(chunk);
    }
    if (file.error() != QFile::NoError) {
        setReason(reason, QStringLiteral("%1 failed while reading: %2").arg(path, file.errorString()));
        return {};
    }
    if (bytes) *bytes = total;
    return QString::fromLatin1(hash.result().toHex());
}

QString sha256OfBytes(const QByteArray& data) {
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

/// Every regular file under `root`, relative and slash-separated. Returns false
/// with a reason if any directory on the way is a reparse point, because a
/// junction inside the tree makes "the inventory" unanswerable.
bool listFilesRelative(const QString& root, QStringList* files, QString* reason) {
    files->clear();
    const QString cleanRoot = QDir::cleanPath(root);
    QStringList queue{cleanRoot};
    while (!queue.isEmpty()) {
        const QString current = queue.takeFirst();
        const QFileInfoList entries =
            QDir(current).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden |
                                        QDir::System);
        for (const QFileInfo& entry : entries) {
            const QString absolute = QDir::cleanPath(entry.absoluteFilePath());
            if (!pathHasNoReparsePoints(root, absolute)) {
                setReason(reason,
                          QStringLiteral("%1 is a reparse point or escapes the bundle root")
                              .arg(absolute));
                return false;
            }
            if (entry.isDir()) {
                queue.append(absolute);
                continue;
            }
            if (!entry.isFile()) {
                setReason(reason, QStringLiteral("%1 is neither a file nor a directory").arg(absolute));
                return false;
            }
            files->append(absolute.mid(cleanRoot.size() + 1));
        }
    }
    std::sort(files->begin(), files->end());
    return true;
}

/// Removes a directory this process created inside a runtime root. Refuses
/// anything whose name does not carry the staging prefix or that sits outside
/// `runtimeRoot`, so a bug here cannot delete a published bundle or a user
/// directory.
bool removeStaging(const QString& runtimeRoot, const QString& stagingPath, QString* reason) {
    const QString cleanRoot = QDir::cleanPath(runtimeRoot);
    const QString cleanStaging = QDir::cleanPath(stagingPath);
    const QString name = QFileInfo(cleanStaging).fileName();
    if (!name.startsWith(QLatin1String(kStagingPrefix))) {
        setReason(reason, QStringLiteral("refusing to remove %1: not a staging directory")
                               .arg(cleanStaging));
        return false;
    }
    if (!pathWithinRoot(cleanRoot, cleanStaging) || cleanStaging == cleanRoot) {
        setReason(reason, QStringLiteral("refusing to remove %1: outside the runtime root")
                               .arg(cleanStaging));
        return false;
    }
    if (!QFileInfo::exists(cleanStaging)) return true;
    if (!QDir(cleanStaging).removeRecursively()) {
        setReason(reason, QStringLiteral("could not remove staging directory %1").arg(cleanStaging));
        return false;
    }
    return true;
}

/// True when `path` is, or lives under, the stock `%LOCALAPPDATA%/MPVBackend/runtime`
/// tree the existing downloader owns. Installing there is refused outright: that
/// tree is stock discovery's concern and this work must not disturb it.
bool isStockRuntimeTree(const QString& path) {
    QStringList segments = QDir::cleanPath(path).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype i = 0; i + 1 < segments.size(); ++i) {
        if (segments.at(i).compare(QLatin1String("MPVBackend"), Qt::CaseInsensitive) == 0 &&
            segments.at(i + 1).compare(QLatin1String("runtime"), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

bool compareVersionsNewestFirst(const QString& left, const QString& right) {
    const QStringList a = left.split(QLatin1Char('.'));
    const QStringList b = right.split(QLatin1Char('.'));
    const qsizetype count = std::min(a.size(), b.size());
    for (qsizetype i = 0; i < count; ++i) {
        bool aOk = false;
        bool bOk = false;
        const qint64 av = a.at(i).toLongLong(&aOk);
        const qint64 bv = b.at(i).toLongLong(&bOk);
        if (aOk && bOk && av != bv) return av > bv;
        if (a.at(i) != b.at(i)) return a.at(i) > b.at(i);
    }
    return a.size() != b.size() && a.size() > b.size();
}

QString versionOfDirectoryName(const QString& name, BundleKind* kind) {
    const QString prefix = bundleKindName(*kind) + QLatin1Char('-');
    if (!name.startsWith(prefix)) return {};
    const QString version = name.mid(prefix.size());
    return versionPattern().match(version).hasMatch() ? version : QString{};
}

}  // namespace

MpvBundle Bundle::toMpvBundleDescriptor() const {
    MpvBundle descriptor;
    descriptor.root = root;
    descriptor.modulePath = modulePath;
    descriptor.descriptorId = descriptorId;
    descriptor.protocolVersion = protocolVersion;
    // Manifest dependencies are relative directory names; the loader needs the
    // absolute directories themselves. root is already canonical and the names
    // are already normalized, so this is a projection, not a filesystem read.
    descriptor.deps.clear();
    for (const QString& dep : deps)
        descriptor.deps.append(QDir::cleanPath(QDir(root).filePath(dep)));
    return descriptor;
}

// -------------------------------------------------------------- identifiers

QString bundleSchemaId() { return QString::fromLatin1(kSchemaId); }

QStringList bundlePreparationTypes() {
    return {QStringLiteral("tensorrt"), QStringLiteral("directml")};
}

QString bundlePreparationBackendLibrary(const QString& type) {
    if (type == QLatin1String("tensorrt")) return QStringLiteral("aji_trt.dll");
    if (type == QLatin1String("directml")) return QStringLiteral("aji_dml.dll");
    return {};
}

QString bundlePreparationProducerLibrary() { return QStringLiteral("aji.dll"); }

std::optional<BundlePreparation> parsePreparation(const QJsonObject& manifest, QString* reason) {
    setReason(reason, QString{});
    const auto refuse = [reason](const QString& text) -> std::optional<BundlePreparation> {
        setReason(reason, text);
        return std::nullopt;
    };

    const QJsonValue value = manifest.value(QStringLiteral("preparation"));
    if (!value.isObject()) return refuse(QStringLiteral("'preparation' is not an object"));
    const QJsonObject block = value.toObject();
    QString offender;
    if (!noUnknownKeys(block, preparationKeys(), &offender))
        return refuse(QStringLiteral("'preparation' has an unknown member '%1'").arg(offender));

    if (!block.value(QStringLiteral("types")).isArray())
        return refuse(QStringLiteral("'preparation.types' is not an array"));
    const QJsonArray types = block.value(QStringLiteral("types")).toArray();
    if (types.isEmpty()) return refuse(QStringLiteral("'preparation.types' is empty"));
    const QStringList knownTypes = bundlePreparationTypes();
    QStringList parsedTypes;
    for (const QJsonValue& entry : types) {
        if (!entry.isString())
            return refuse(QStringLiteral("'preparation.types' holds a non-string entry"));
        const QString type = entry.toString();
        if (!knownTypes.contains(type))
            return refuse(
                QStringLiteral("preparation type '%1' is not one this build can prepare").arg(type));
        if (parsedTypes.contains(type))
            return refuse(QStringLiteral("preparation type '%1' is listed twice").arg(type));
        parsedTypes.append(type);
    }

    const QJsonValue sourceValue = block.value(QStringLiteral("source"));
    if (!sourceValue.isObject())
        return refuse(QStringLiteral("'preparation.source' is not an object"));
    const QJsonObject source = sourceValue.toObject();
    if (!noUnknownKeys(source, preparationSourceKeys(), &offender))
        return refuse(QStringLiteral("'preparation.source' has an unknown member '%1'").arg(offender));
    QString coreSha;
    if (!jsonString(source, QStringLiteral("coreSha"), &coreSha))
        return refuse(QStringLiteral("'preparation.source.coreSha' is not a string"));
    coreSha = coreSha.toLower();
    if (!sha1Pattern().match(coreSha).hasMatch())
        return refuse(QStringLiteral("'preparation.source.coreSha' is not a full 40-hex commit"));
    qint64 sourceAbi = 0;
    if (!jsonInteger(source.value(QStringLiteral("sourceAbi")), &sourceAbi) || sourceAbi < 0)
        return refuse(QStringLiteral("'preparation.source.sourceAbi' is not a non-negative integer"));

    const QJsonValue toolsValue = block.value(QStringLiteral("tools"));
    if (!toolsValue.isObject())
        return refuse(QStringLiteral("'preparation.tools' is not an object"));
    const QJsonObject tools = toolsValue.toObject();
    if (!noUnknownKeys(tools, preparationToolKeys(), &offender))
        return refuse(QStringLiteral("'preparation.tools' has an unknown member '%1'").arg(offender));
    BundlePreparationTools parsedTools;
    const QStringList toolKeys = preparationToolKeys();
    QString* toolSlots[] = {&parsedTools.inspect, &parsedTools.prep, &parsedTools.verifier};
    for (int i = 0; i < toolKeys.size(); ++i) {
        QString raw;
        if (!jsonString(tools, toolKeys.at(i), &raw))
            return refuse(
                QStringLiteral("'preparation.tools.%1' is not a string").arg(toolKeys.at(i)));
        QString pathReason;
        const QString normalized = safeRelativePath(raw, &pathReason);
        if (normalized.isEmpty())
            return refuse(QStringLiteral("preparation tool %1").arg(pathReason));
        if (!normalized.endsWith(QLatin1String(".exe"), pathCaseSensitivity()))
            return refuse(QStringLiteral("preparation tool '%1' is not a .exe path").arg(raw));
        *toolSlots[i] = normalized;
    }

    const QJsonValue runtimeValue = block.value(QStringLiteral("runtime"));
    if (!runtimeValue.isObject())
        return refuse(QStringLiteral("'preparation.runtime' is not an object"));
    const QJsonObject runtime = runtimeValue.toObject();
    if (!noUnknownKeys(runtime, preparationRuntimeKeys(), &offender))
        return refuse(
            QStringLiteral("'preparation.runtime' has an unknown member '%1'").arg(offender));
    BundlePreparationRuntime parsedRuntime;
    const QStringList runtimeKeys = preparationRuntimeKeys();
    QString* runtimeSlots[] = {&parsedRuntime.ort, &parsedRuntime.trt, &parsedRuntime.cuda};
    for (int i = 0; i < runtimeKeys.size(); ++i) {
        QString raw;
        if (!jsonString(runtime, runtimeKeys.at(i), &raw))
            return refuse(
                QStringLiteral("'preparation.runtime.%1' is not a string").arg(runtimeKeys.at(i)));
        QString pathReason;
        const QString normalized = safeRelativePath(raw, &pathReason);
        if (normalized.isEmpty())
            return refuse(QStringLiteral("preparation runtime directory %1").arg(pathReason));
        if (normalized.contains(QLatin1Char('/')))
            return refuse(QStringLiteral("'preparation.runtime.%1' must be one directory inside the "
                                         "bundle")
                              .arg(runtimeKeys.at(i)));
        *runtimeSlots[i] = normalized;
    }

    BundlePreparation preparation;
    preparation.types = parsedTypes;
    preparation.coreSha = coreSha;
    preparation.sourceAbi = int(sourceAbi);
    preparation.tools = parsedTools;
    preparation.runtime = parsedRuntime;
    return preparation;
}

QString bundleKindName(BundleKind kind) {
    return kind == BundleKind::Native ? QStringLiteral("native") : QStringLiteral("standard");
}

QString nativeStorageRoot() {
    return nativeRoot();
}

QString runtimeStorageRoot() {
    return nativeSubdirectory(QStringLiteral("runtime"));
}

QString bundleDirectoryName(BundleKind kind, const QString& version) {
    return bundleKindName(kind) + QLatin1Char('-') + version;
}

QString bundlePointerFileName(BundleKind kind) {
    return QStringLiteral("current-") + bundleKindName(kind);
}

// ------------------------------------------------------------------- read

std::optional<Bundle> readBundle(const QString& root, QString* reason) {
    setReason(reason, QString{});
    const auto refuse = [reason](const QString& text) -> std::optional<Bundle> {
        setReason(reason, text);
        return std::nullopt;
    };

    if (root.trimmed().isEmpty()) return refuse(QStringLiteral("no bundle root was given"));
    // The root is spelled by whoever pointed at the bundle, so what it says is
    // checked before anything is asked of the filesystem: a truncation, a device
    // namespace or a redirector share must be refused by name, not resolved and
    // then explained by whatever it happened to be.
    const NativePathTrust rootTrust = nativePathTrust(root, true);
    if (!rootTrust.trusted) {
        return refuse(QStringLiteral("bundle root is not a usable local path: %1")
                          .arg(rootTrust.reason));
    }
    const QFileInfo rootInfo(root);
    if (!rootInfo.exists() || !rootInfo.isDir())
        return refuse(QStringLiteral("%1 is not an existing directory").arg(root));
    // Checked before canonicalizing: a root that is itself a junction describes
    // a directory the caller did not name, so "inside the bundle" would be a
    // claim about somewhere else.
    const QString absoluteRoot = QDir::cleanPath(rootInfo.absoluteFilePath());
    if (!pathHasNoReparsePoints(absoluteRoot, absoluteRoot))
        return refuse(QStringLiteral("%1 is a reparse point; a bundle root must be a real directory")
                          .arg(absoluteRoot));
    const QString canonical = rootInfo.canonicalFilePath();
    if (canonical.isEmpty())
        return refuse(QStringLiteral("%1 cannot be resolved to a canonical path").arg(root));
    const NativePathTrust canonicalTrust = nativePathTrust(canonical, true);
    if (!canonicalTrust.trusted) {
        return refuse(QStringLiteral("bundle root does not resolve to a usable local path: %1")
                          .arg(canonicalTrust.reason));
    }

    const QString manifestPath = QDir(canonical).filePath(QLatin1String(kManifestFileName));
    const QString canonicalManifest = canonicalPath(manifestPath);
    if (canonicalManifest.isEmpty())
        return refuse(QStringLiteral("%1 has no readable bundle.json").arg(canonical));
    if (!pathWithinRoot(canonical, canonicalManifest) ||
        !pathHasNoReparsePoints(canonical, canonicalManifest))
        return refuse(QStringLiteral("%1 escapes the bundle root or crosses a reparse point")
                          .arg(canonicalManifest));
    QFile manifestFile(canonicalManifest);
    if (!manifestFile.open(QIODevice::ReadOnly))
        return refuse(QStringLiteral("bundle.json cannot be read: %1")
                          .arg(manifestFile.errorString()));
    const QByteArray manifestBytes = manifestFile.readAll();
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(manifestBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return refuse(QStringLiteral("bundle.json is not a valid JSON object: %1")
                          .arg(parseError.errorString()));
    const QJsonObject manifest = document.object();

    QString offender;
    if (!noUnknownKeys(manifest, manifestKeys(), &offender))
        return refuse(QStringLiteral("bundle.json has an unknown member '%1'").arg(offender));

    QString schema;
    if (!jsonString(manifest, QStringLiteral("schema"), &schema))
        return refuse(QStringLiteral("bundle.json has no string 'schema'"));
    if (schema != bundleSchemaId())
        return refuse(QStringLiteral("manifest schema '%1' is not supported; this build reads '%2'")
                          .arg(schema, bundleSchemaId()));

    QString kindText;
    if (!jsonString(manifest, QStringLiteral("kind"), &kindText))
        return refuse(QStringLiteral("bundle.json has no string 'kind'"));
    BundleKind kind{};
    if (kindText == QLatin1String("standard")) {
        kind = BundleKind::Standard;
    } else if (kindText == QLatin1String("native")) {
        kind = BundleKind::Native;
    } else {
        return refuse(QStringLiteral("kind '%1' is neither 'standard' nor 'native'").arg(kindText));
    }

    QString version;
    if (!jsonString(manifest, QStringLiteral("version"), &version))
        return refuse(QStringLiteral("bundle.json has no string 'version'"));
    if (!versionPattern().match(version).hasMatch())
        return refuse(QStringLiteral("version '%1' is not MAJOR.MINOR.PATCH[-label]").arg(version));

    QString moduleRelative;
    if (!jsonString(manifest, QStringLiteral("module"), &moduleRelative))
        return refuse(QStringLiteral("bundle.json has no string 'module'"));
    QString normalizeReason;
    const QString modulePath = safeRelativePath(moduleRelative, &normalizeReason);
    if (modulePath.isEmpty())
        return refuse(QStringLiteral("module %1").arg(normalizeReason));

    qint64 apiVersion = 0;
    if (!jsonInteger(manifest.value(QStringLiteral("mpvApiVersion")), &apiVersion) || apiVersion < 0)
        return refuse(QStringLiteral("mpvApiVersion is not a non-negative integer"));
    qint64 protocolVersion = 0;
    if (!jsonInteger(manifest.value(QStringLiteral("protocolVersion")), &protocolVersion) ||
        protocolVersion < 0)
        return refuse(QStringLiteral("protocolVersion is not a non-negative integer"));
    qint64 engineSchemaVersion = 0;
    if (!jsonInteger(manifest.value(QStringLiteral("engineSchemaVersion")), &engineSchemaVersion) ||
        engineSchemaVersion < 0)
        return refuse(QStringLiteral("engineSchemaVersion is not a non-negative integer"));

    // Version support is decided here, before any file is touched, so an
    // unsupported bundle costs one read of bundle.json and nothing else.
    if (apiVersion == 0) {
        if (kind != BundleKind::Standard)
            return refuse(QStringLiteral("a native bundle must declare its mpv client API version"));
    } else {
        const quint64 major = quint64(apiVersion) >> 16;
        const quint64 minor = quint64(apiVersion) & 0xFFFFu;
        if (major != kMinSupportedMpvApiMajor || minor < kMinSupportedMpvApiMinor ||
            minor > kMaxSupportedMpvApiMinor)
            return refuse(QStringLiteral("mpv client API %1 is outside the supported window %2.%3-%4.%5")
                              .arg(mpvApiVersionText(quint64(apiVersion)))
                              .arg(kMinSupportedMpvApiMajor)
                              .arg(kMinSupportedMpvApiMinor)
                              .arg(kMinSupportedMpvApiMajor)
                              .arg(kMaxSupportedMpvApiMinor));
    }
    if (kind == BundleKind::Native) {
        if (protocolVersion != kNativeProtocolVersion)
            return refuse(QStringLiteral("native protocol version %1 is not the supported %2")
                              .arg(protocolVersion)
                              .arg(kNativeProtocolVersion));
        if (engineSchemaVersion != kNativeEngineSchemaVersion)
            return refuse(QStringLiteral("native engine schema version %1 is not the supported %2")
                              .arg(engineSchemaVersion)
                              .arg(kNativeEngineSchemaVersion));
    } else {
        if (protocolVersion != 0 || engineSchemaVersion != 0)
            return refuse(QStringLiteral(
                "a standard bundle declares no native protocol or engine schema (got %1/%2)")
                .arg(protocolVersion)
                .arg(engineSchemaVersion));
    }

    Bundle bundle;
    bundle.kind = kind;
    bundle.root = canonical;
    bundle.modulePath = QDir(canonical).filePath(modulePath);
    bundle.version = version;
    bundle.mpvApiVersion = quint64(apiVersion);
    bundle.protocolVersion = int(protocolVersion);
    bundle.engineSchemaVersion = int(engineSchemaVersion);

    // The descriptor identity is a digest of the exact manifest bytes, so a
    // bundle whose manifest changed is a different descriptor even if every
    // payload file still matches.
    bundle.descriptorId = sha256OfBytes(manifestBytes);

    if (!manifest.value(QStringLiteral("deps")).isArray())
        return refuse(QStringLiteral("'deps' is not an array"));
    const QJsonArray deps = manifest.value(QStringLiteral("deps")).toArray();
    QSet<QString> seenDeps;
    for (const QJsonValue& value : deps) {
        if (!value.isString())
            return refuse(QStringLiteral("'deps' holds a non-string entry"));
        QString depReason;
        const QString dep = safeRelativePath(value.toString(), &depReason);
        if (dep.isEmpty()) return refuse(QStringLiteral("dependency %1").arg(depReason));
        if (dep.contains(QLatin1Char('/')))
            return refuse(QStringLiteral("dependency '%1' must be a directory inside the bundle")
                              .arg(value.toString()));
        if (!seenDeps.contains(dep.toLower())) {
            seenDeps.insert(dep.toLower());
            bundle.deps.append(dep);
        }
    }

    if (!manifest.value(QStringLiteral("files")).isArray())
        return refuse(QStringLiteral("'files' is not an array"));
    const QJsonArray files = manifest.value(QStringLiteral("files")).toArray();
    if (files.isEmpty()) return refuse(QStringLiteral("'files' is empty; a bundle ships files"));

    QSet<QString> seenPaths;
    for (const QJsonValue& value : files) {
        if (!value.isObject()) return refuse(QStringLiteral("'files' holds a non-object entry"));
        const QJsonObject record = value.toObject();
        if (!noUnknownKeys(record, fileKeys(), &offender))
            return refuse(QStringLiteral("a file record has an unknown member '%1'").arg(offender));
        QString relative;
        if (!jsonString(record, QStringLiteral("path"), &relative))
            return refuse(QStringLiteral("a file record has no string 'path'"));
        QString fileReason;
        const QString normalized = safeRelativePath(relative, &fileReason);
        if (normalized.isEmpty()) return refuse(QStringLiteral("file %1").arg(fileReason));
        if (normalized == QLatin1String(kManifestFileName))
            return refuse(QStringLiteral("bundle.json may not appear in its own inventory"));
        qint64 bytes = -1;
        if (!jsonInteger(record.value(QStringLiteral("bytes")), &bytes) || bytes < 0)
            return refuse(QStringLiteral("file '%1' has no non-negative integer 'bytes'").arg(relative));
        QString digest;
        if (!jsonString(record, QStringLiteral("sha256"), &digest))
            return refuse(QStringLiteral("file '%1' has no string 'sha256'").arg(relative));
        digest = digest.toLower();
        if (!sha256Pattern().match(digest).hasMatch())
            return refuse(QStringLiteral("file '%1' has a malformed SHA-256").arg(relative));
        if (seenPaths.contains(normalized.toLower()))
            return refuse(QStringLiteral("file '%1' is listed twice").arg(relative));
        seenPaths.insert(normalized.toLower());
        bundle.files.append(BundleFile{normalized, bytes, digest});
    }

    const bool moduleDeclared =
        std::any_of(bundle.files.cbegin(), bundle.files.cend(), [&modulePath](const BundleFile& file) {
            return file.path.compare(modulePath, pathCaseSensitivity()) == 0;
        });
    if (!moduleDeclared)
        return refuse(QStringLiteral("module '%1' is not in the file inventory").arg(moduleRelative));

    // The preparation block is optional. Absent, the bundle is playback-only.
    // Present, it must be complete and coherent on its own terms: a placeholder
    // source pin or a zero ABI is refused here, before any file is read, so a
    // real bundle cannot carry an unmeasured provenance value.
    if (manifest.contains(QStringLiteral("preparation"))) {
        QString preparationReason;
        const std::optional<BundlePreparation> preparation =
            parsePreparation(manifest, &preparationReason);
        if (!preparation.has_value()) return refuse(preparationReason);
        if (kind != BundleKind::Native)
            return refuse(QStringLiteral("a standard bundle declares no preparation block"));
        if (preparation->coreSha == reservedCoreSha())
            return refuse(QStringLiteral("preparation source coreSha is the reserved placeholder"));
        if (preparation->sourceAbi <= 0)
            return refuse(QStringLiteral("preparation source ABI must be observed, not zero"));
        bundle.preparation = *preparation;
        // The block must name files this bundle actually declares, so a
        // hand-edited manifest cannot pair a valid source pin with a tool layout
        // nothing measured.
        if (!preparationInventoryCoherent(bundle, &preparationReason))
            return refuse(QStringLiteral("preparation %1").arg(preparationReason));
    }

    return bundle;
}

// --------------------------------------------------------------- validation

namespace {

/// Shared by readBundle-installed bundles and the stock bridge: given a root
/// and an inventory, prove the bytes say what the inventory says.
bool verifyInventory(const QString& root, const QVector<BundleFile>& files, const QString& modulePath,
                     QString* reason) {
    QSet<QString> basenames;
    for (const BundleFile& file : files) {
        const QString absolute = QDir::cleanPath(QDir(root).filePath(file.path));
        if (!pathWithinRoot(root, absolute))
            return (setReason(reason, QStringLiteral("'%1' resolves outside the bundle root")
                                       .arg(file.path)),
                    false);
        const QString canonical = canonicalPath(absolute);
        if (canonical.isEmpty())
            return (setReason(reason, QStringLiteral("'%1' is missing from the bundle")
                                       .arg(file.path)),
                    false);
        if (!pathWithinRoot(root, canonical))
            return (setReason(reason, QStringLiteral("'%1' resolves outside the bundle root")
                                       .arg(file.path)),
                    false);
        if (!pathHasNoReparsePoints(root, canonical))
            return (setReason(reason,
                              QStringLiteral("'%1' is reached through a reparse point").arg(file.path)),
                    false);
        const QFileInfo info(canonical);
        if (!info.isFile())
            return (setReason(reason, QStringLiteral("'%1' is not a regular file").arg(file.path)),
                    false);
        if (info.size() != file.bytes)
            return (setReason(reason, QStringLiteral("'%1' is %2 bytes, the manifest records %3")
                                       .arg(file.path)
                                       .arg(info.size())
                                       .arg(file.bytes)),
                    false);
        QString digestReason;
        const QString digest = sha256OfFile(canonical, nullptr, &digestReason);
        if (digest.isEmpty()) return (setReason(reason, digestReason), false);
        if (digest != file.sha256)
            return (setReason(reason, QStringLiteral("'%1' hashes to %2, the manifest records %3")
                                       .arg(file.path, digest, file.sha256)),
                    false);
        if (file.path.endsWith(QLatin1String(".dll"), Qt::CaseInsensitive)
#ifndef Q_OS_WIN
            || file.path.endsWith(QLatin1String(".so"))
            || file.path.contains(QLatin1String(".so."))
#endif
        )
            basenames.insert(QFileInfo(file.path).fileName().toLower());
    }

    // Every DLL in the inventory must be a real x64 PE, and every module it
    // imports must be either a Windows system component or another declared
    // file. That is the whole dependency-closure rule, and it is evaluated over
    // the inventory rather than over a declared list, so a DLL nobody declared
    // cannot satisfy an import.
    for (const BundleFile& file : files) {
        if (!file.path.endsWith(QLatin1String(".dll"), Qt::CaseInsensitive)) continue;
        const QString canonical = canonicalPath(QDir(root).filePath(file.path));
        QFile image(canonical);
        if (!image.open(QIODevice::ReadOnly))
            return (setReason(reason, QStringLiteral("'%1' cannot be read").arg(file.path)), false);
        const PeImageInfo info = inspectPeImage(image.readAll());
        if (!info.ok)
            return (setReason(reason, QStringLiteral("'%1' is not a usable x64 PE image: %2")
                                       .arg(file.path, info.reason)),
                    false);
        for (const QString& imported : info.imports + info.delayImports) {
            if (isSystemImportException(imported)) continue;
            if (basenames.contains(imported)) continue;
            return (setReason(reason,
                              QStringLiteral("'%1' imports '%2', which is neither a Windows system "
                                             "module nor a file inside the bundle")
                                  .arg(file.path, imported)),
                    false);
        }
    }

    const QString canonicalModule = canonicalPath(modulePath);
    if (canonicalModule.isEmpty() || !QFileInfo(canonicalModule).isFile())
        return (setReason(reason, QStringLiteral("module '%1' is not a file in the bundle")
                                   .arg(modulePath)),
                false);
    if (!basenames.contains(QFileInfo(canonicalModule).fileName().toLower()))
        return (setReason(reason, QStringLiteral("module '%1' is not a declared DLL").arg(modulePath)),
                false);
    return true;
}

/// Refuses anything present in the root that the manifest does not declare.
/// A file the manifest does not mention is a file nothing verified, and a
/// bundle is exactly the set of files it declared.
bool verifyNoUndeclaredFiles(const QString& root, const QVector<BundleFile>& files, QString* reason) {
    QStringList present;
    if (!listFilesRelative(root, &present, reason)) return false;
    QSet<QString> declared;
    for (const BundleFile& file : files) declared.insert(file.path.toLower());
    for (const QString& relative : present) {
        if (relative.compare(QLatin1String(kManifestFileName), Qt::CaseInsensitive) == 0) continue;
        if (declared.contains(relative.toLower())) continue;
        return (setReason(reason, QStringLiteral("'%1' is present in the bundle but not declared")
                                   .arg(relative)),
                false);
    }
    return true;
}

/// The preparation block's closure against the bundle's own inventory and
/// declared dependency search paths. Pure path and inventory reasoning: it
/// loads nothing, so a passing check is a coherent layout, not a working
/// toolchain and not an engine.
bool validatePreparationClosure(const Bundle& bundle, QString* reason) {
    if (bundle.kind != BundleKind::Native) {
        setReason(reason, QStringLiteral("a standard bundle declares no preparation block"));
        return false;
    }
    const BundlePreparation& preparation = *bundle.preparation;
    if (preparation.coreSha == reservedCoreSha()) {
        setReason(reason, QStringLiteral("preparation source coreSha is the reserved placeholder"));
        return false;
    }
    if (preparation.sourceAbi <= 0) {
        setReason(reason, QStringLiteral("preparation source ABI must be observed, not zero"));
        return false;
    }
    if (!preparationInventoryCoherent(bundle, reason)) return false;

    const QStringList runtimeDirs{preparation.runtime.ort, preparation.runtime.trt,
                                  preparation.runtime.cuda};
    QSet<QString> checked;
    for (const QString& directory : runtimeDirs) {
        const QString key = directory.toLower();
        if (checked.contains(key)) continue;
        checked.insert(key);
        if (QDir::isAbsolutePath(directory) || directory.contains(QLatin1Char('\\'))) {
            setReason(reason, QStringLiteral("preparation runtime directory '%1' must be relative to "
                                             "the bundle root")
                                  .arg(directory));
            return false;
        }
        QString directoryReason;
        const QString normalized = safeRelativePath(directory, &directoryReason);
        if (normalized.isEmpty()) {
            setReason(reason, QStringLiteral("preparation runtime directory %1").arg(directoryReason));
            return false;
        }
        const QString absolute = QDir::cleanPath(QDir(bundle.root).filePath(normalized));
        const QString canonicalDirectory = canonicalPath(absolute);
        if (canonicalDirectory.isEmpty() || !QFileInfo(canonicalDirectory).isDir()) {
            setReason(reason, QStringLiteral("preparation runtime directory '%1' does not exist")
                                  .arg(directory));
            return false;
        }
        if (!pathWithinRoot(bundle.root, canonicalDirectory) ||
            !pathHasNoReparsePoints(bundle.root, canonicalDirectory)) {
            setReason(reason, QStringLiteral("preparation runtime directory '%1' is outside the "
                                             "bundle root or reached through a reparse point")
                                  .arg(directory));
            return false;
        }
        bool holdsDeclaredFile = false;
        bool holdsDll = false;
        for (const BundleFile& file : bundle.files) {
            if (!file.path.startsWith(normalized + QLatin1Char('/'), pathCaseSensitivity())) continue;
            holdsDeclaredFile = true;
            if (file.path.endsWith(QLatin1String(".dll"), Qt::CaseInsensitive)) holdsDll = true;
        }
        if (!holdsDeclaredFile) {
            setReason(reason, QStringLiteral("preparation runtime directory '%1' contains no declared "
                                             "file")
                                  .arg(directory));
            return false;
        }
        if (holdsDll && !bundle.deps.contains(normalized, pathCaseSensitivity())) {
            setReason(reason, QStringLiteral("preparation runtime directory '%1' carries DLLs but is "
                                             "not a declared dependency")
                                  .arg(directory));
            return false;
        }
    }
    return true;
}

}  // namespace

bool validateBundle(const Bundle& bundle, QString* reason) {
    setReason(reason, QString{});
    if (bundle.root.isEmpty()) {
        setReason(reason, QStringLiteral("the bundle has no root"));
        return false;
    }
    const QString canonical = canonicalPath(bundle.root);
    if (canonical.isEmpty() || !QFileInfo(canonical).isDir()) {
        setReason(reason, QStringLiteral("bundle root %1 does not exist").arg(bundle.root));
        return false;
    }
    if (!pathHasNoReparsePoints(canonical, canonical)) {
        setReason(reason, QStringLiteral("bundle root %1 is a reparse point").arg(canonical));
        return false;
    }
    // A manifest version passed the release-token pattern in readBundle; a
    // described stock runtime carries a content-derived token instead. Either
    // way it has to be able to name a published directory.
    if (!tokenPattern().match(bundle.version).hasMatch()) {
        setReason(reason,
                  QStringLiteral("version '%1' cannot name a published directory").arg(bundle.version));
        return false;
    }
    if (bundle.files.isEmpty()) {
        setReason(reason, QStringLiteral("the bundle declares no files"));
        return false;
    }
    if (!verifyInventory(canonical, bundle.files, bundle.modulePath, reason)) return false;
    if (!verifyNoUndeclaredFiles(canonical, bundle.files, reason)) return false;

    // Dependency directories are canonical, inside the root, and actually hold
    // something this bundle declared. A directory that holds no declared file
    // is a search-path entry with no closure behind it.
    for (const QString& dep : bundle.deps) {
        if (QDir::isAbsolutePath(dep) || dep.contains(QLatin1Char('\\'))) {
            setReason(reason, QStringLiteral("dependency '%1' must be relative to the bundle root")
                                  .arg(dep));
            return false;
        }
        QString depReason;
        const QString normalizedDep = safeRelativePath(dep, &depReason);
        if (normalizedDep.isEmpty()) {
            setReason(reason, QStringLiteral("dependency %1").arg(depReason));
            return false;
        }
        const QString absolute = QDir::cleanPath(QDir(canonical).filePath(normalizedDep));
        const QString canonicalDep = canonicalPath(absolute);
        if (canonicalDep.isEmpty() || !QFileInfo(canonicalDep).isDir()) {
            setReason(reason, QStringLiteral("dependency directory '%1' does not exist").arg(dep));
            return false;
        }
        if (!pathWithinRoot(canonical, canonicalDep)) {
            setReason(reason,
                      QStringLiteral("dependency directory '%1' resolves outside the bundle root")
                          .arg(dep));
            return false;
        }
        if (!pathHasNoReparsePoints(canonical, canonicalDep)) {
            setReason(reason,
                      QStringLiteral("dependency directory '%1' is reached through a reparse point")
                          .arg(dep));
            return false;
        }
        bool holdsDeclaredFile = false;
        for (const BundleFile& file : bundle.files) {
            if (file.path.startsWith(normalizedDep + QLatin1Char('/'), pathCaseSensitivity())) {
                holdsDeclaredFile = true;
                break;
            }
        }
        if (!holdsDeclaredFile) {
            setReason(reason, QStringLiteral("dependency directory '%1' contains no declared file")
                                  .arg(dep));
            return false;
        }
    }
    if (bundle.preparation.has_value() && !validatePreparationClosure(bundle, reason)) return false;
    return true;
}

// -------------------------------------------------------------- stock bridge

std::optional<Bundle> describeStandardRuntime(const QString& modulePath, QString* reason) {
    setReason(reason, QString{});
    const auto refuse = [reason](const QString& text) -> std::optional<Bundle> {
        setReason(reason, text);
        return std::nullopt;
    };

    if (modulePath.trimmed().isEmpty()) return refuse(QStringLiteral("no module path was given"));
    const QFileInfo moduleInfo(modulePath);
    if (!moduleInfo.exists() || !moduleInfo.isFile())
        return refuse(QStringLiteral("%1 is not an existing file").arg(modulePath));
    const QString canonicalModule = moduleInfo.canonicalFilePath();
    const QString root = QFileInfo(canonicalModule).absolutePath();
    if (root.isEmpty())
        return refuse(QStringLiteral("%1 has no parent directory").arg(canonicalModule));
    if (!pathHasNoReparsePoints(root, canonicalModule))
        return refuse(QStringLiteral("%1 is reached through a reparse point").arg(canonicalModule));
    const bool isDll = moduleInfo.fileName().endsWith(QLatin1String(".dll"), Qt::CaseInsensitive);
#ifdef Q_OS_WIN
    const bool isLinuxSo = false;
#else
    const bool isLinuxSo =
        moduleInfo.fileName().endsWith(QLatin1String(".so")) ||
        moduleInfo.fileName().contains(QLatin1String(".so."));
#endif
    if (!isDll && !isLinuxSo)
        return refuse(QStringLiteral("%1 is not a DLL").arg(canonicalModule));
    if (QFileInfo::exists(QDir(root).filePath(QLatin1String(kManifestFileName))))
        return refuse(QStringLiteral("%1 holds a bundle.json; it is a published bundle, not a "
                                     "stock runtime")
                          .arg(root));

    {
        QFile image(canonicalModule);
        if (!image.open(QIODevice::ReadOnly))
            return refuse(QStringLiteral("%1 cannot be read").arg(canonicalModule));
        const QByteArray bytes = image.readAll();
        if (isDll) {
            const PeImageInfo info = inspectPeImage(bytes);
            if (!info.ok)
                return refuse(QStringLiteral("%1 is not a usable x64 PE image: %2")
                                  .arg(canonicalModule, info.reason));
        } else if (bytes.size() < 20 || !bytes.startsWith(QByteArrayLiteral("\x7f""ELF\x02"))) {
            return refuse(QStringLiteral("%1 is not a usable 64-bit ELF shared object")
                              .arg(canonicalModule));
        }
    }

    Bundle bundle;
    bundle.kind = BundleKind::Standard;
    bundle.root = root;
    bundle.modulePath = canonicalModule;
    bundle.protocolVersion = 0;
    bundle.engineSchemaVersion = 0;
    // A stock core's client API version is learned from the loaded module, not
    // from its bytes; 0 is the declared "unknown until load" value and the
    // loader resolves it through mpv_client_api_version().
    bundle.mpvApiVersion = 0;

    QStringList present;
    if (!listFilesRelative(root, &present, reason)) return std::nullopt;
    for (const QString& relative : present) {
        if (relative.compare(QLatin1String(kManifestFileName), Qt::CaseInsensitive) == 0) continue;
        const QString canonical = canonicalPath(QDir(root).filePath(relative));
        qint64 bytes = 0;
        const QString digest = sha256OfFile(canonical, &bytes, reason);
        if (digest.isEmpty()) return std::nullopt;
        bundle.files.append(BundleFile{relative, bytes, digest});
    }
    std::sort(bundle.files.begin(), bundle.files.end(), [](const BundleFile& a, const BundleFile& b) {
        return a.path.compare(b.path, pathCaseSensitivity()) < 0;
    });

    if (!verifyInventory(root, bundle.files, canonicalModule, reason)) return std::nullopt;

    // Dependency directories are the directories inside the root that actually
    // hold a DLL the closure needs. A flat stock runtime therefore has none:
    // its own directory is already the search path.
    QSet<QString> depSet;
    for (const BundleFile& file : bundle.files) {
        if (!file.path.endsWith(QLatin1String(".dll"), Qt::CaseInsensitive)) continue;
        const QString canonical = canonicalPath(QDir(root).filePath(file.path));
        QFile image(canonical);
        if (!image.open(QIODevice::ReadOnly)) continue;
        const PeImageInfo info = inspectPeImage(image.readAll());
        if (!info.ok) continue;
        for (const QString& imported : info.imports + info.delayImports) {
            if (isSystemImportException(imported)) continue;
            bool insideBundle = false;
            for (const BundleFile& candidate : bundle.files) {
                if (QFileInfo(candidate.path).fileName().compare(imported, Qt::CaseInsensitive) == 0) {
                    const QString directory =
                        QFileInfo(candidate.path).path().compare(QLatin1String("."), Qt::CaseInsensitive) == 0
                            ? QString{}
                            : QFileInfo(candidate.path).path();
                    if (!directory.isEmpty()) depSet.insert(directory);
                    insideBundle = true;
                    break;
                }
            }
            if (!insideBundle)
                return refuse(QStringLiteral("%1 imports '%2', which is neither a Windows system "
                                             "module nor a file beside it")
                                  .arg(QFileInfo(file.path).fileName(), imported));
        }
    }
    // Qt 6.12's QSet::values() on an rvalue moves the set's storage into the
    // returned list, so the dependency names are not copied one by one; the set
    // is not used again.
    bundle.deps = std::move(depSet).values();
    std::sort(bundle.deps.begin(), bundle.deps.end());

    // A stable identity for bytes that carry no manifest: derived from what is
    // actually installed, so a stock runtime replaced in place is a different
    // descriptor rather than silently the same one.
    QString material;
    for (const BundleFile& file : bundle.files)
        material += file.path + QLatin1Char(':') + QString::number(file.bytes) + QLatin1Char(':') +
                    file.sha256 + QLatin1Char('\n');
    material += QStringLiteral("module:") + QFileInfo(canonicalModule).fileName();
    bundle.descriptorId = sha256OfBytes(material.toUtf8());
    bundle.version = QStringLiteral("stock-") + bundle.descriptorId.left(12);

    return bundle;
}

// --------------------------------------------------------- validation cache

namespace {

constexpr const char* kCacheFileName = "verified.json";
// Bumped when the entry shape changes, so a cache written by an older build is
// ignored rather than half-read: version 2 records the root on a stock entry so
// an invalidation can drop it.
constexpr const char* kCacheSchema = "mikai.native.cache/2";

QString cachePath() {
    // MIKAI_NATIVE_CACHE lets a test point the cache at its own temporary
    // directory instead of the one the installed app uses.
    const QString override = qEnvironmentVariable("MIKAI_NATIVE_CACHE");
    if (!override.isEmpty()) return override;
    return QDir(nativeStorageRoot()).filePath(QString::fromLatin1(kCacheFileName));
}

QJsonObject readCacheObject() {
    QFile file(cachePath());
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("schema")).toString() != QLatin1String(kCacheSchema)) return {};
    return object;
}

bool writeCacheObject(const QJsonObject& object) {
    QDir().mkpath(nativeStorageRoot());
    QSaveFile file(cachePath());
    if (!file.open(QIODevice::WriteOnly)) return false;
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return file.write(payload) == payload.size() && file.commit();
}

/// A stat-only fingerprint of a directory tree: every file's relative path, size
/// and modification time. It answers "has anything changed", never "are the
/// bytes right", and that is exactly what makes trusting an earlier full check
/// bounded - a file rewritten with different content still moves its mtime, so
/// the next start re-hashes it.
QString treeFingerprint(const QString& root) {
    QStringList files;
    QString reason;
    if (!listFilesRelative(root, &files, &reason)) return {};
    files.sort(Qt::CaseInsensitive);
    // A one-byte separator between fields, so ("ab", 1) and ("a", 1, "b") cannot
    // hash the same. Held as a QByteArray because the const-char* overload is
    // deprecated.
    const QByteArray separator(1, '\0');
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString& relative : files) {
        const QFileInfo info(QDir(root).filePath(relative));
        hash.addData(relative.toUtf8());
        hash.addData(separator);
        hash.addData(QByteArray::number(info.size()));
        hash.addData(separator);
        hash.addData(QByteArray::number(info.lastModified().toMSecsSinceEpoch()));
        hash.addData(separator);
    }
    return QString::fromLatin1(hash.result().toHex());
}

QJsonObject descriptorToJson(const Bundle& bundle) {
    QJsonArray files;
    for (const BundleFile& file : bundle.files) {
        QJsonObject entry;
        entry.insert(QStringLiteral("path"), file.path);
        entry.insert(QStringLiteral("bytes"), file.bytes);
        entry.insert(QStringLiteral("sha256"), file.sha256);
        files.append(entry);
    }
    QJsonObject object;
    object.insert(QStringLiteral("root"), bundle.root);
    object.insert(QStringLiteral("modulePath"), bundle.modulePath);
    object.insert(QStringLiteral("descriptorId"), bundle.descriptorId);
    object.insert(QStringLiteral("version"), bundle.version);
    object.insert(QStringLiteral("deps"), QJsonArray::fromStringList(bundle.deps));
    object.insert(QStringLiteral("files"), files);
    return object;
}

std::optional<Bundle> descriptorFromJson(const QJsonObject& object) {
    Bundle bundle;
    bundle.kind = BundleKind::Standard;
    bundle.root = object.value(QStringLiteral("root")).toString();
    bundle.modulePath = object.value(QStringLiteral("modulePath")).toString();
    bundle.descriptorId = object.value(QStringLiteral("descriptorId")).toString();
    bundle.version = object.value(QStringLiteral("version")).toString();
    for (const QJsonValue& value : object.value(QStringLiteral("deps")).toArray())
        bundle.deps.append(value.toString());
    for (const QJsonValue& value : object.value(QStringLiteral("files")).toArray()) {
        const QJsonObject entry = value.toObject();
        BundleFile file;
        file.path = entry.value(QStringLiteral("path")).toString();
        file.bytes = static_cast<qint64>(entry.value(QStringLiteral("bytes")).toDouble());
        file.sha256 = entry.value(QStringLiteral("sha256")).toString();
        if (file.path.isEmpty() || file.sha256.isEmpty()) return std::nullopt;
        bundle.files.append(file);
    }
    if (bundle.root.isEmpty() || bundle.files.isEmpty()) return std::nullopt;
    return bundle;
}

}  // namespace

bool validateBundleCached(const Bundle& bundle, QString* reason) {
    const QString key = QDir::cleanPath(bundle.root);
    const QString fingerprint = treeFingerprint(bundle.root);
    QJsonObject cache = readCacheObject();
    const QJsonArray validations = cache.value(QStringLiteral("validations")).toArray();

    if (!fingerprint.isEmpty()) {
        for (const QJsonValue& value : validations) {
            const QJsonObject entry = value.toObject();
            if (entry.value(QStringLiteral("root")).toString().compare(key, pathCaseSensitivity()) ==
                    0 &&
                entry.value(QStringLiteral("fingerprint")).toString() == fingerprint) {
                setReason(reason, QString{});
                return true;
            }
        }
    }

    if (!validateBundle(bundle, reason)) {
        // A failure is never cached: a bundle that was just repaired, or a disk
        // that was just reconnected, must be re-probed rather than remembered as
        // broken.
        QJsonArray kept;
        for (const QJsonValue& value : validations) {
            if (value.toObject().value(QStringLiteral("root")).toString().compare(
                    key, pathCaseSensitivity()) != 0) {
                kept.append(value);
            }
        }
        cache.insert(QStringLiteral("schema"), QString::fromLatin1(kCacheSchema));
        cache.insert(QStringLiteral("validations"), kept);
        writeCacheObject(cache);
        return false;
    }

    if (!fingerprint.isEmpty()) {
        QJsonArray kept;
        for (const QJsonValue& value : validations) {
            if (value.toObject().value(QStringLiteral("root")).toString().compare(
                    key, pathCaseSensitivity()) != 0) {
                kept.append(value);
            }
        }
        QJsonObject entry;
        entry.insert(QStringLiteral("root"), key);
        entry.insert(QStringLiteral("fingerprint"), fingerprint);
        entry.insert(QStringLiteral("checkedAt"),
                     QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        kept.append(entry);
        cache.insert(QStringLiteral("schema"), QString::fromLatin1(kCacheSchema));
        cache.insert(QStringLiteral("validations"), kept);
        writeCacheObject(cache);
    }
    return true;
}

std::optional<Bundle> describeStandardRuntimeCached(const QString& modulePath, QString* reason) {
    const QString canonical = QFileInfo(modulePath).canonicalFilePath();
    const QString root =
        canonical.isEmpty() ? QString{} : QFileInfo(canonical).absolutePath();
    const QString fingerprint = root.isEmpty() ? QString{} : treeFingerprint(root);
    QJsonObject cache = readCacheObject();
    const QJsonArray stock = cache.value(QStringLiteral("stock")).toArray();

    if (!fingerprint.isEmpty()) {
        for (const QJsonValue& value : stock) {
            const QJsonObject entry = value.toObject();
            if (entry.value(QStringLiteral("modulePath")).toString().compare(
                    canonical, pathCaseSensitivity()) == 0 &&
                entry.value(QStringLiteral("fingerprint")).toString() == fingerprint) {
                const std::optional<Bundle> restored =
                    descriptorFromJson(entry.value(QStringLiteral("descriptor")).toObject());
                if (restored.has_value()) {
                    setReason(reason, QString{});
                    return restored;
                }
            }
        }
    }

    const std::optional<Bundle> bundle = describeStandardRuntime(modulePath, reason);

    // Whatever this module had recorded is dropped either way: a describe that
    // succeeded replaces it, and one that failed because the module is no longer
    // a usable image leaves nothing worth remembering. A stale entry could not
    // be trusted anyway - its fingerprint no longer matches - but leaving it
    // behind means a cache that only ever grows.
    QJsonArray kept;
    for (const QJsonValue& value : stock) {
        if (value.toObject().value(QStringLiteral("modulePath")).toString().compare(
                canonical, pathCaseSensitivity()) != 0) {
            kept.append(value);
        }
    }
    if (bundle.has_value() && !fingerprint.isEmpty()) {
        QJsonObject entry;
        entry.insert(QStringLiteral("modulePath"), canonical);
        // The root is recorded so an invalidation can drop this descriptor too:
        // a stock runtime that stops loading must be described again from its
        // bytes, not answered from the descriptor it just failed under.
        entry.insert(QStringLiteral("root"), QDir::cleanPath(bundle->root));
        entry.insert(QStringLiteral("fingerprint"), fingerprint);
        entry.insert(QStringLiteral("descriptor"), descriptorToJson(*bundle));
        kept.append(entry);
    }
    cache.insert(QStringLiteral("schema"), QString::fromLatin1(kCacheSchema));
    cache.insert(QStringLiteral("stock"), kept);
    writeCacheObject(cache);
    return bundle;
}

void invalidateBundleValidation(const QString& root) {
    const QString key = QDir::cleanPath(root);
    QJsonObject cache = readCacheObject();

    QJsonArray kept;
    for (const QJsonValue& value : cache.value(QStringLiteral("validations")).toArray()) {
        if (value.toObject().value(QStringLiteral("root")).toString().compare(
                key, pathCaseSensitivity()) != 0) {
            kept.append(value);
        }
    }
    cache.insert(QStringLiteral("schema"), QString::fromLatin1(kCacheSchema));
    cache.insert(QStringLiteral("validations"), kept);

    // A described stock runtime is cached under the same root as the bundle
    // built from it, so both have to go: keeping the descriptor would answer the
    // next start with the very description the loader just refused.
    QJsonArray keptStock;
    for (const QJsonValue& value : cache.value(QStringLiteral("stock")).toArray()) {
        if (value.toObject().value(QStringLiteral("root")).toString().compare(
                key, pathCaseSensitivity()) != 0) {
            keptStock.append(value);
        }
    }
    cache.insert(QStringLiteral("stock"), keptStock);
    writeCacheObject(cache);
}

// --------------------------------------------------------------- discovery

QList<Bundle> discoverBundles(const QStringList& roots, QStringList* rejections) {
    if (rejections) rejections->clear();
    QList<Bundle> found;

    QStringList searchRoots = roots;
    if (searchRoots.isEmpty()) {
        searchRoots.append(runtimeStorageRoot());
    }

    // A pointer target and a versioned directory can name the same bundle; a
    // caller must never see it twice.
    QSet<QString> seen;
    const auto consider = [&found, rejections, &seen](const QString& root,
                                                       BundleKind expectedKind) {
        QString readReason;
        const std::optional<Bundle> bundle = readBundle(root, &readReason);
        if (!bundle.has_value()) {
            if (rejections) rejections->append(QStringLiteral("%1: %2").arg(root, readReason));
            return;
        }
        if (bundle->kind != expectedKind) {
            if (rejections)
                rejections->append(QStringLiteral("%1: manifest says kind '%2' but the directory is "
                                                 "named for '%3'")
                                       .arg(root, bundleKindName(bundle->kind),
                                            bundleKindName(expectedKind)));
            return;
        }
        QString validateReason;
        if (!validateBundleCached(*bundle, &validateReason)) {
            if (rejections) rejections->append(QStringLiteral("%1: %2").arg(root, validateReason));
            return;
        }
        if (seen.contains(bundle->root.toLower())) return;
        seen.insert(bundle->root.toLower());
        found.append(*bundle);
    };

    for (const QString& root : searchRoots) {
        const QFileInfo rootInfo(root);
        if (!rootInfo.exists() || !rootInfo.isDir()) continue;
        if (isStockRuntimeTree(root)) continue;
        const QString canonicalRoot = rootInfo.canonicalFilePath();
        if (canonicalRoot.isEmpty()) continue;

        const QDir dir(canonicalRoot);
        const QFileInfoList entries = dir.entryInfoList(
            QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);

        // The pointer first: it names what a previous install decided, and a
        // valid pointer target outranks any other versioned directory.
        for (const BundleKind declared : {BundleKind::Native, BundleKind::Standard}) {
            const QString pointerPath = dir.filePath(bundlePointerFileName(declared));
            if (!QFileInfo::exists(pointerPath)) continue;
            QFile pointer(pointerPath);
            if (!pointer.open(QIODevice::ReadOnly)) {
                if (rejections)
                    rejections->append(QStringLiteral("%1: the current pointer cannot be read")
                                           .arg(pointerPath));
                continue;
            }
            const QString name = QString::fromUtf8(pointer.readAll()).trimmed();
            BundleKind kind = declared;
            if (!tokenPattern().match(name).hasMatch() ||
                versionOfDirectoryName(name, &kind).isEmpty()) {
                if (rejections)
                    rejections->append(QStringLiteral("%1: the current pointer names '%2', which is "
                                                     "not a published directory of this kind")
                                           .arg(pointerPath, name));
                continue;
            }
            consider(dir.filePath(name), kind);
        }

        // Then every versioned directory, newest first. An invalid one is named
        // and skipped: it must never hide a valid bundle of the same kind.
        struct Candidate {
            QString name;
            QString version;
            BundleKind kind;
        };
        QVector<Candidate> candidates;
        for (const QFileInfo& entry : entries) {
            if (!entry.isDir()) continue;
            const QString name = entry.fileName();
            if (name.startsWith(QLatin1String(kStagingPrefix))) continue;  // never a candidate
            for (BundleKind kind : {BundleKind::Standard, BundleKind::Native}) {
                const QString version = versionOfDirectoryName(name, &kind);
                if (version.isEmpty()) continue;
                candidates.append(Candidate{name, version, kind});
                break;
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) {
                      return compareVersionsNewestFirst(a.version, b.version);
                  });
        for (const Candidate& candidate : candidates)
            consider(dir.filePath(candidate.name), candidate.kind);
    }

    return found;
}

// ------------------------------------------------------------------ install

bool installAtomic(const Bundle& source, const QString& runtimeRoot, QString* reason,
                   QString* publishedRoot) {
    setReason(reason, QString{});
    if (publishedRoot) publishedRoot->clear();
    const auto fail = [reason](const QString& text) {
        setReason(reason, text);
        return false;
    };

    if (runtimeRoot.trimmed().isEmpty())
        return fail(QStringLiteral("no runtime root was given"));
    if (!QDir::isAbsolutePath(runtimeRoot))
        return fail(QStringLiteral("runtime root %1 must be absolute").arg(runtimeRoot));
    if (isStockRuntimeTree(runtimeRoot))
        return fail(QStringLiteral("refusing to install into the stock MPVBackend runtime tree %1")
                        .arg(runtimeRoot));

    // The source is proven before a single byte is written, so a bad bundle
    // cannot leave a staging directory behind.
    QString validateReason;
    if (!validateBundle(source, &validateReason))
        return fail(QStringLiteral("source bundle is not installable: %1").arg(validateReason));

    const QString cleanRoot = QDir::cleanPath(runtimeRoot);
    const QFileInfo rootInfo(cleanRoot);
    if (rootInfo.exists() && !rootInfo.isDir())
        return fail(QStringLiteral("runtime root %1 is not a directory").arg(cleanRoot));
    if (!rootInfo.exists() && !QDir().mkpath(cleanRoot))
        return fail(QStringLiteral("runtime root %1 could not be created").arg(cleanRoot));
    // Publishing through a junction would put "our" bytes somewhere else.
    if (!pathHasNoReparsePoints(cleanRoot, cleanRoot))
        return fail(QStringLiteral("runtime root %1 is a reparse point").arg(cleanRoot));
    const QString canonicalRoot = canonicalPath(cleanRoot);
    if (canonicalRoot.isEmpty())
        return fail(QStringLiteral("runtime root %1 cannot be resolved").arg(cleanRoot));

    if (pathWithinRoot(canonicalRoot, canonicalPath(source.root)) ||
        pathWithinRoot(canonicalPath(source.root), canonicalRoot))
        return fail(QStringLiteral("runtime root %1 and bundle root %2 overlap")
                        .arg(canonicalRoot, source.root));

    const QString manifestSource = QDir(source.root).filePath(QLatin1String(kManifestFileName));
    const bool manifestPresent = QFileInfo::exists(manifestSource);

    // A descriptor's `kind` and the manifest beside it are two independent
    // claims, and Standard and Native are never mixed inside one bundle.
    // Publishing a Native descriptor whose root holds a standard manifest would
    // create a `native-<version>` directory carrying a standard bundle.json: a
    // directory whose name and contents disagree, which discovery can only
    // refuse. The disagreement is caught here, before a byte is staged, and is
    // named rather than resolved.
    if (manifestPresent) {
        QString manifestReason;
        const std::optional<Bundle> manifestBundle = readBundle(source.root, &manifestReason);
        if (!manifestBundle.has_value()) {
            return fail(QStringLiteral("the source bundle.json cannot be read: %1")
                            .arg(manifestReason));
        }
        if (manifestBundle->kind != source.kind) {
            return fail(QStringLiteral("the source bundle is described as '%1' but its bundle.json "
                                       "declares kind '%2'")
                            .arg(bundleKindName(source.kind),
                                 bundleKindName(manifestBundle->kind)));
        }
        if (manifestBundle->version != source.version) {
            return fail(QStringLiteral("the source bundle is described as version '%1' but its "
                                       "bundle.json declares '%2'")
                            .arg(source.version, manifestBundle->version));
        }
    }

    // A stock descriptor has a content-derived version token and no manifest;
    // the strict manifest schema wants a release-shaped version, so the token
    // is folded into one that still names the exact inventory. Anything else
    // must already carry a publishable version (readBundle produced it).
    QString publishVersion = source.version;
    if (!versionPattern().match(publishVersion).hasMatch()) {
        if (source.kind != BundleKind::Standard || source.mpvApiVersion != 0 || manifestPresent)
            return fail(QStringLiteral("version '%1' cannot name a published directory")
                            .arg(source.version));
        publishVersion = QStringLiteral("0.0.0-stock.") + source.descriptorId;
    }

    const QString publishName = bundleDirectoryName(source.kind, publishVersion);
    const QString publishPath = QDir(canonicalRoot).filePath(publishName);
    if (QFileInfo::exists(publishPath))
        return fail(QStringLiteral("%1 is already published; published bundles are immutable")
                        .arg(publishPath));

    // A stale staging directory from an interrupted run is app-owned scratch
    // under our own prefix. Clearing it is what makes a retry possible; the
    // published bundles beside it are never touched.
    const QString stagingName = QString::fromLatin1(kStagingPrefix) + publishName;
    const QString stagingPath = QDir(canonicalRoot).filePath(stagingName);
    if (QFileInfo::exists(stagingPath)) {
        QString removeReason;
        if (!removeStaging(canonicalRoot, stagingPath, &removeReason))
            return fail(removeReason);
    }
    if (!QDir().mkdir(stagingPath))
        return fail(QStringLiteral("could not create staging directory %1").arg(stagingPath));
    if (!pathHasNoReparsePoints(canonicalRoot, stagingPath))
        return fail(QStringLiteral("staging directory %1 is reached through a reparse point")
                        .arg(stagingPath));

    QString writeFailure;
    const auto abort = [&](const QString& text) {
        QString removeReason;
        removeStaging(canonicalRoot, stagingPath, &removeReason);
        return fail(text);
    };

    // Only the verified inventory and the manifest are copied. Nothing else in
    // the source root is swept up, and nothing outside is written.
    for (const BundleFile& file : source.files) {
        const QString destination = QDir(stagingPath).filePath(file.path);
        const QString parent = QFileInfo(destination).absolutePath();
        if (!QDir().mkpath(parent)) {
            writeFailure = QStringLiteral("could not create %1").arg(parent);
            break;
        }
        if (QFileInfo::exists(destination)) {
            writeFailure = QStringLiteral("staged copy of '%1' already exists").arg(file.path);
            break;
        }
        if (!QFile::copy(QDir(source.root).filePath(file.path), destination)) {
            writeFailure = QStringLiteral("could not copy '%1'").arg(file.path);
            break;
        }
        qint64 copiedBytes = 0;
        const QString digest = sha256OfFile(destination, &copiedBytes, &writeFailure);
        if (digest.isEmpty()) break;
        if (copiedBytes != file.bytes || digest != file.sha256) {
            writeFailure = QStringLiteral("the staged copy of '%1' does not match the manifest")
                               .arg(file.path);
            break;
        }
    }
    if (writeFailure.isEmpty()) {
        const QString manifestDestination =
            QDir(stagingPath).filePath(QLatin1String(kManifestFileName));
        if (manifestPresent) {
            if (!QFile::copy(manifestSource, manifestDestination))
                writeFailure = QStringLiteral("could not copy bundle.json into the staging directory");
        } else if (source.kind == BundleKind::Standard && source.mpvApiVersion == 0) {
            // The one case where a manifest is synthesised: a validated stock
            // descriptor whose bytes carry no manifest of their own. It is
            // written into our staging directory from the inventory that was
            // already verified against disk, so the source is read, not edited.
            QJsonArray deps;
            for (const QString& dep : source.deps) deps.append(dep);
            QJsonArray files;
            for (const BundleFile& file : source.files) {
                files.append(QJsonObject{{QStringLiteral("path"), file.path},
                                         {QStringLiteral("bytes"), double(file.bytes)},
                                         {QStringLiteral("sha256"), file.sha256}});
            }
            const QJsonObject manifest{
                {QStringLiteral("schema"), bundleSchemaId()},
                {QStringLiteral("kind"), bundleKindName(source.kind)},
                {QStringLiteral("version"), publishVersion},
                {QStringLiteral("module"), QDir(source.root).relativeFilePath(source.modulePath)},
                {QStringLiteral("mpvApiVersion"), double(source.mpvApiVersion)},
                {QStringLiteral("protocolVersion"), double(source.protocolVersion)},
                {QStringLiteral("engineSchemaVersion"), double(source.engineSchemaVersion)},
                {QStringLiteral("deps"), deps},
                {QStringLiteral("files"), files}};
            QFile manifestOut(manifestDestination);
            if (!manifestOut.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                writeFailure =
                    QStringLiteral("could not write bundle.json into the staging directory");
            } else {
                const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
                if (manifestOut.write(bytes) != bytes.size())
                    writeFailure =
                        QStringLiteral("could not write bundle.json into the staging directory");
                manifestOut.close();
            }
        } else {
            // A Native descriptor arrives from its own manifest, so a missing
            // one is a refusal, not something to invent.
            writeFailure = QStringLiteral("the source bundle has no bundle.json to publish");
        }
    }
    if (!writeFailure.isEmpty()) return abort(writeFailure);

    // The staged payload is re-read and re-validated from disk, not trusted
    // because it was copied from a valid source.
    QString stagedReason;
    const std::optional<Bundle> staged = readBundle(stagingPath, &stagedReason);
    if (!staged.has_value()) return abort(stagedReason);
    if (!validateBundle(*staged, &stagedReason))
        return abort(QStringLiteral("the staged bundle did not validate: %1").arg(stagedReason));

    if (QFileInfo::exists(publishPath)) {
        return abort(QStringLiteral("%1 appeared while staging; nothing was published")
                         .arg(publishPath));
    }
    if (!QDir(canonicalRoot).rename(stagingName, publishPath))
        return abort(QStringLiteral("could not publish %1").arg(publishPath));

    // Only after the immutable directory exists is the pointer flipped. The
    // pointer is the single selected-bundle fact, and it is replaced
    // atomically, so a reader sees either the previous name or this one.
    QSaveFile pointer(QDir(canonicalRoot).filePath(bundlePointerFileName(source.kind)));
    pointer.setDirectWriteFallback(false);
    if (!pointer.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("could not write the current pointer in %1; the new bundle is "
                                   "published but not selected")
                        .arg(canonicalRoot));
    if (pointer.write(publishName.toUtf8() + '\n') != publishName.toUtf8().size() + 1) {
        pointer.cancelWriting();
        return fail(QStringLiteral("could not write the current pointer in %1; the new bundle is "
                                   "published but not selected")
                        .arg(canonicalRoot));
    }
    if (!pointer.commit())
        return fail(QStringLiteral("could not commit the current pointer in %1; the new bundle is "
                                   "published but not selected")
                        .arg(canonicalRoot));
    if (publishedRoot) *publishedRoot = publishPath;
    return true;
}

}  // namespace native
}  // namespace mplayer