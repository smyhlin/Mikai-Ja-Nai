// M35's assets panel. See NativeAssetsPanel.hpp for the rules; this file is their
// one implementation.
//
// Everything the panel shows is read from the app-owned store and the
// artifact cache through their own APIs. Nothing here scans a foreign folder,
// invents availability, runs an inspection on the GUI thread, or deletes a file
// that did not first prove itself.

#include "NativeAssetsPanel.hpp"

#include <atomic>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtGui/QShowEvent>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QList>
#include <QtCore/QLocale>
#include <QtCore/QMetaObject>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTemporaryDir>
#include <QtCore/QVariant>
#include <QtGui/QIntValidator>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <backend/Errors.hpp>

#include <native/bundle/NativeBundle.hpp>
#include <native/models/NativeInspectedPlan.hpp>
#include <native/bundle/NativeManifest.hpp>
#include <native/bundle/NativePaths.hpp>
#include <native/prepare/NativePreparationPlan.hpp>
#include <native/prepare/NativePreparationReferences.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>
#include <native/models/NativeSpatialAssets.hpp>
#include <native/models/NativeUserRecipes.hpp>
#include <native/components/NativeComponentStore.hpp>
#include <native/components/NativeComponentToolchain.hpp>
#include <native/components/NativeGpuProbe.hpp>
#include "state/NativeComponentProvisioner.hpp"
#include "workers/Runner.hpp"
#include "theme/Metrics.hpp"

namespace mikai::app {

// The panel owns the app-side surface; the model, recipe and artifact
// vocabulary it reads stays in mplayer::native and is named through this
// file-local using-directive rather than re-homed.
using namespace mplayer::native;

namespace {

constexpr const char* kArtifactDirectoryPrefix = "artifact-";

QString humanBytes(qint64 bytes) {
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

// The describe/identity path below is reached only from the inspection-guarded
// preparation worker, so it is compiled only when that worker is: a default build
// that carried it would carry functions nothing can call. This first block is the
// UUID vocabulary that path alone speaks.
#ifdef MIKAI_WITH_MODEL_INSPECTION

/// Case and punctuation must never make the same adapter look like a different
/// one; this normalization is used only to compare observed spellings, never to
/// record one.
QString normalizedUuid(const QString& uuid) {
    QString normalized;
    normalized.reserve(uuid.size());
    for (const QChar character : uuid) {
        if (character.isLetterOrNumber()) normalized.append(character.toLower());
    }
    return normalized;
}

/// A CUDA device UUID is sixteen bytes. Only a value of that shape may be handed
/// to the CUDA-only describe helper or compared against its measurement; a D3D
/// adapter LUID is a different, shorter vocabulary and is left to the plan.
bool isCudaDeviceUuid(const QString& uuid) { return normalizedUuid(uuid).size() == 32; }

#endif  // MIKAI_WITH_MODEL_INSPECTION

QString statusCell(const NativeModelView& view) {
    if (view.usable) return QCoreApplication::translate("NativeAssetsPanel", "Ready");
    return QCoreApplication::translate("NativeAssetsPanel", "Unavailable");
}

// ------------------------------------------------------ measured identity
// The machine identity is measured by the toolchain's own describe mode, never
// copied from a cache and never assembled from plausible constants. The document
// is the helper's own observation: every member this consumer needs is required,
// and a missing one refuses the whole document rather than completing it.

#ifdef MIKAI_WITH_MODEL_INSPECTION

/// The child environment the toolchain needs: its own directory first, then the
/// provider runtime directories, then the system directories. The ambient PATH is
/// deliberately not inherited, so the helper loads the runtimes it was measured
/// against rather than whatever happens to be reachable.
QProcessEnvironment describeChildEnvironment(const BuilderToolPaths& tools) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QStringList path;
    if (!tools.prepExecutable.isEmpty()) {
        path.append(QFileInfo(tools.prepExecutable).absolutePath());
    }
    if (!tools.trtBinDir.isEmpty()) path.append(QDir::toNativeSeparators(tools.trtBinDir));
    if (!tools.cudaBinDir.isEmpty()) path.append(QDir::toNativeSeparators(tools.cudaBinDir));
    const QString systemRoot = qEnvironmentVariable("SystemRoot");
    if (!systemRoot.isEmpty()) {
        path.append(QDir::toNativeSeparators(systemRoot + QStringLiteral("/System32")));
        path.append(QDir::toNativeSeparators(systemRoot));
    }
    environment.insert(QStringLiteral("PATH"), path.join(QLatin1Char(';')));
    return environment;
}

/// Runs `mikai_native_prep --describe-device --json` - naming the observed device
/// when one is known, so the helper measures the adapter this job will build for
/// - and returns its stdout document. A helper that does not answer, or answers
/// with nothing, refuses by name; nothing is read from a file the operator wrote.
bool runDescribeDevice(const BuilderToolPaths& tools, const QString& deviceUuid, QString* document,
                       QString* reasonKey, QString* reason) {
    const auto refuse = [reasonKey, reason](const QString& key, const QString& text) {
        if (reasonKey != nullptr) *reasonKey = key;
        if (reason != nullptr) *reason = text;
        return false;
    };
    if (tools.prepExecutable.isEmpty()) {
        return refuse(QStringLiteral("native.prepare.identityUnobserved"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "No preparation helper is present, so this machine's build identity "
                          "cannot be measured."));
    }
    QProcess process;
    process.setProcessEnvironment(describeChildEnvironment(tools));
    process.setProgram(tools.prepExecutable);
    QStringList arguments{QStringLiteral("--describe-device"), QStringLiteral("--json")};
    if (!deviceUuid.isEmpty()) {
        arguments.append(QStringLiteral("--device-uuid"));
        arguments.append(deviceUuid);
    }
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(10000)) {
        return refuse(QStringLiteral("native.prepare.identityLaunch"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The preparation helper could not be started: %1")
                          .arg(process.errorString()));
    }
    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished(5000);
        return refuse(QStringLiteral("native.prepare.identityTimeout"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The preparation helper did not report this machine's build identity "
                          "within its deadline."));
    }
    const QByteArray output = process.readAllStandardOutput();
    const QByteArray errors = process.readAllStandardError();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
        output.trimmed().isEmpty()) {
        const QString detail = QString::fromUtf8(errors).trimmed();
        return refuse(QStringLiteral("native.prepare.identityUnobserved"),
                      detail.isEmpty()
                          ? QCoreApplication::translate(
                                "NativeAssetsPanel",
                                "The preparation helper refused to describe this machine (exit "
                                "%1).")
                                .arg(process.exitCode())
                          : detail);
    }
    if (document != nullptr) *document = QString::fromUtf8(output).trimmed();
    if (reasonKey != nullptr) reasonKey->clear();
    if (reason != nullptr) reason->clear();
    return true;
}

/// Exactly the canonical option set the builder derives from the flags the
/// helper measured. A different set would produce a build key for a
/// configuration the helper never used, so the builder compares this back.
QVector<BuildOption> measuredBuildOptions(quint64 builderFlags, quint64 tacticSources,
                                          quint64 networkFlags, int builderOptimizationLevel,
                                          const QString& engineCapability) {
    const auto option = [](const QString& name, const QString& value) {
        BuildOption build;
        build.name = name;
        build.value = value;
        build.hasValue = true;
        return build;
    };
    QVector<BuildOption> options;
    options.append(option(QStringLiteral("builderFlags"), QString::number(builderFlags)));
    options.append(option(QStringLiteral("tacticSources"), QString::number(tacticSources)));
    options.append(option(QStringLiteral("networkFlags"), QString::number(networkFlags)));
    options.append(
        option(QStringLiteral("builderOptimizationLevel"), QString::number(builderOptimizationLevel)));
    options.append(option(QStringLiteral("engineCapability"), engineCapability));
    return options;
}

/// The one schema this consumer reads from the helper's describe mode.
QString describeDeviceSchema() {
    return QStringLiteral("mikai.native.preparation.environment/1");
}

/// Parses the describe document into the build identity. Strict: the schema, the
/// device, the runtime, the core provenance and the measured build flags must all
/// be present and well typed, or the whole document is refused by name.
bool identityFromDescribe(const QJsonObject& document, Backend provider, const QString& coreSha,
                          int sourceAbi, const QString& pluginVersion, const DeviceIdentity& device,
                          EngineBuildIdentity* out, QString* reasonKey, QString* reason) {
    const auto refuse = [reasonKey, reason](const QString& key, const QString& text) {
        if (reasonKey != nullptr) *reasonKey = key;
        if (reason != nullptr) *reason = text;
        return false;
    };
    const auto member = [](const QJsonObject& object, const QString& key, QJsonValue* value) {
        if (!object.contains(key)) return false;
        *value = object.value(key);
        return true;
    };
    const auto stringMember = [&](const QJsonObject& object, const QString& key, QString* value,
                                  QString* missing) {
        QJsonValue found;
        if (!member(object, key, &found) || !found.isString()) {
            *missing = key;
            return false;
        }
        *value = found.toString();
        return true;
    };
    const auto intMember = [&](const QJsonObject& object, const QString& key, qint64* value,
                               QString* missing) {
        QJsonValue found;
        if (!member(object, key, &found) || !found.isDouble()) {
            *missing = key;
            return false;
        }
        *value = static_cast<qint64>(found.toDouble());
        return true;
    };
    const auto boolMember = [&](const QJsonObject& object, const QString& key, bool* value,
                                QString* missing) {
        QJsonValue found;
        if (!member(object, key, &found) || !found.isBool()) {
            *missing = key;
            return false;
        }
        *value = found.toBool();
        return true;
    };

    QJsonValue schema;
    if (!member(document, QStringLiteral("schema"), &schema) || !schema.isString() ||
        schema.toString() != describeDeviceSchema()) {
        return refuse(QStringLiteral("native.prepare.identitySchema"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The preparation helper's describe document is not the %1 schema this "
                          "build reads.")
                          .arg(describeDeviceSchema()));
    }

    // The settled document is flat and measured: the same key names and types the
    // helper's own build events record, so a describe record and a build log
    // compare key by key. Nothing is defaulted, and a fact the helper cannot
    // measure is refused by name rather than completed with a plausible value.
    EngineBuildIdentity identity;
    identity.provider = provider;
    QString missing;
    QString tool;
    qint64 protocol = 0;
    qint64 deviceOrdinal = 0;
    QString computeCapability;
    qint64 cudaRuntime = 0;
    qint64 tensorRtLib = 0;
    qint64 builderFlags = 0;
    qint64 tacticSources = 0;
    qint64 networkFlags = 0;
    qint64 builderOptimizationLevel = 0;
    QString engineCapability;
    bool describedFlag = false;
    bool okFlag = false;
    bool tf32Allowed = false;
    bool stronglyTyped = false;
    if (!stringMember(document, QStringLiteral("tool"), &tool, &missing) ||
        tool != QLatin1String("mikai_native_prep") ||
        !intMember(document, QStringLiteral("protocol"), &protocol, &missing) || protocol != 1 ||
        !boolMember(document, QStringLiteral("described"), &describedFlag, &missing) ||
        !describedFlag || !boolMember(document, QStringLiteral("ok"), &okFlag, &missing) ||
        !okFlag ||
        !intMember(document, QStringLiteral("deviceOrdinal"), &deviceOrdinal, &missing) ||
        !stringMember(document, QStringLiteral("deviceUuid"), &identity.deviceUuid, &missing) ||
        !stringMember(document, QStringLiteral("computeCapability"), &computeCapability,
                      &missing) ||
        !stringMember(document, QStringLiteral("cudaDriverFileVersion"), &identity.driverVersion,
                      &missing) ||
        !intMember(document, QStringLiteral("cudaRuntimeVersion"), &cudaRuntime, &missing) ||
        !intMember(document, QStringLiteral("tensorRtLibVersion"), &tensorRtLib, &missing) ||
        !intMember(document, QStringLiteral("builderFlags"), &builderFlags, &missing) ||
        !boolMember(document, QStringLiteral("tf32Allowed"), &tf32Allowed, &missing) ||
        !stringMember(document, QStringLiteral("engineCapability"), &engineCapability, &missing) ||
        !intMember(document, QStringLiteral("builderOptimizationLevel"), &builderOptimizationLevel,
                   &missing) ||
        !intMember(document, QStringLiteral("tacticSources"), &tacticSources, &missing) ||
        !intMember(document, QStringLiteral("networkFlags"), &networkFlags, &missing) ||
        !boolMember(document, QStringLiteral("stronglyTyped"), &stronglyTyped, &missing)) {
        return refuse(QStringLiteral("native.prepare.identityIncomplete"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The describe document is not the measured environment this build reads "
                          "(missing or unusable %1).")
                          .arg(missing));
    }
    Q_UNUSED(deviceOrdinal);

    // computeCapability "major.minor" is the measured SM the build must match.
    const QStringList sm = computeCapability.split(QLatin1Char('.'));
    bool smOk = false;
    if (sm.size() != 2) {
        return refuse(QStringLiteral("native.prepare.identityIncomplete"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The measured compute capability %1 is not major.minor.")
                          .arg(computeCapability));
    }
    const int smMajor = sm.at(0).toInt(&smOk);
    if (!smOk) {
        return refuse(QStringLiteral("native.prepare.identityIncomplete"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The measured compute capability %1 has no numeric major.")
                          .arg(computeCapability));
    }
    const int smMinor = sm.at(1).toInt(&smOk);
    if (!smOk) {
        return refuse(QStringLiteral("native.prepare.identityIncomplete"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The measured compute capability %1 has no numeric minor.")
                          .arg(computeCapability));
    }
    identity.smMajor = smMajor;
    identity.smMinor = smMinor;

    // The measured adapter must be the compute device this job observed: a plan
    // resolved for one adapter and an identity measured on another would name a
    // machine the engine cannot load on. An unobserved device is left to the
    // planner's own named refusal rather than compared against an empty name.
    if (isCudaDeviceUuid(device.computeDeviceUuid) &&
        !mplayer::native::isSameDeviceUuid(identity.deviceUuid, device.computeDeviceUuid)) {
        return refuse(QStringLiteral("native.prepare.deviceMismatch"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The measured device %1 is not the compute device this source observed "
                          "(%2).")
                          .arg(identity.deviceUuid, device.computeDeviceUuid));
    }
    if (device.smMajor > 0 && (smMajor != device.smMajor || smMinor != device.smMinor)) {
        return refuse(QStringLiteral("native.prepare.deviceMismatch"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The measured compute capability %1.%2 is not the SM the observed device "
                          "reports (%3.%4).")
                          .arg(smMajor)
                          .arg(smMinor)
                          .arg(device.smMajor)
                          .arg(device.smMinor));
    }

    // The dotted runtime spellings the build identity carries, derived from the
    // helper's measured integers by the exact inverse of the builder's own
    // conversion, so the two can never disagree.
    identity.runtimeVersion = QStringLiteral("%1.%2.%3")
                                  .arg(tensorRtLib / 10000)
                                  .arg((tensorRtLib / 100) % 100)
                                  .arg(tensorRtLib % 100);
    identity.cudaVersion =
        QStringLiteral("%1.%2").arg(cudaRuntime / 1000).arg((cudaRuntime % 1000) / 10);
    identity.pluginVersion = pluginVersion;
    identity.tf32 = tf32Allowed;
    identity.stronglyTyped = stronglyTyped;
    identity.options = measuredBuildOptions(static_cast<quint64>(builderFlags),
                                            static_cast<quint64>(tacticSources),
                                            static_cast<quint64>(networkFlags),
                                            static_cast<int>(builderOptimizationLevel),
                                            engineCapability);

    // `policy` is what this helper prepares, deliberately grouped rather than
    // dressed as a device readback: fp16 because the models are, and
    // skipInference because the helper never runs an inference pass.
    const QJsonValue policyValue = document.value(QStringLiteral("policy"));
    if (!policyValue.isObject()) {
        return refuse(QStringLiteral("native.prepare.identityIncomplete"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel", "The describe document names no policy section."));
    }
    const QJsonObject policy = policyValue.toObject();
    QString precisionText;
    if (!stringMember(policy, QStringLiteral("precision"), &precisionText, &missing) ||
        !boolMember(policy, QStringLiteral("skipInference"), &identity.skipInference, &missing)) {
        return refuse(QStringLiteral("native.prepare.identityIncomplete"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The describe document's policy has no usable %1.")
                          .arg(missing));
    }
    const std::optional<EnginePrecision> precision = enginePrecisionFromName(precisionText);
    if (!precision.has_value()) {
        return refuse(QStringLiteral("native.prepare.identityIncomplete"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The describe document names an unknown build precision."));
    }
    identity.precision = *precision;

    // The core's source SHA and ABI are not emitted by a TensorRT describe - it
    // cannot measure them - so they come from validated bundle provenance and the
    // actual C ABI. Without that provenance they are refused by name rather than
    // filled with a made-up digest.
    if (coreSha.isEmpty() || sourceAbi <= 0) {
        return refuse(QStringLiteral("native.prepare.provenance"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The inference core's source SHA and ABI are not part of the measured "
                          "environment and no validated bundle provenance supplied them."));
    }
    identity.coreSha = coreSha;
    identity.sourceAbi = sourceAbi;
    const QString incomplete = identity.firstMissingRequiredField();
    if (!incomplete.isEmpty()) {
        return refuse(QStringLiteral("native.prepare.identityIncomplete"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The measured build identity records no %1.").arg(incomplete));
    }
    const QString sensitive = identity.firstSensitiveField();
    if (!sensitive.isEmpty()) {
        return refuse(QStringLiteral("native.prepare.identitySensitive"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The measured build identity's %1 looks like a URL or a credential, "
                          "which is never a real runtime or device.")
                          .arg(sensitive));
    }
    *out = identity;
    if (reasonKey != nullptr) reasonKey->clear();
    if (reason != nullptr) reason->clear();
    return true;
}

#endif  // MIKAI_WITH_MODEL_INSPECTION

}  // namespace

NativeModelView describeStoredModel(const StoredModel& model) {
    NativeModelView view;
    view.id = model.record.id;
    view.label = model.record.displayName.isEmpty() ? model.record.id : model.record.displayName;
    view.contentId = model.record.contentId();
    view.role = model.record.role == ModelRole::Spatial
                    ? QCoreApplication::translate("NativeAssetsPanel", "Spatial")
                    : QCoreApplication::translate("NativeAssetsPanel", "Temporal");
    view.dtype = model.record.actualProfile.input.dtype;
    view.bytes = model.record.onnxBytes;
    view.backends = model.record.availableBackends;
    view.terms = model.record.termsNote;
    view.usable = model.record.availability.isUsable();
    if (!view.usable) {
        view.statusKey = model.record.availability.reasonKey;
        view.statusReason = model.record.availability.reason;
    }
    return view;
}

QVector<NativeCacheEntry> enumerateNativeCache(const ArtifactStore& store) {
    QVector<NativeCacheEntry> entries;
    // An unowned root is never walked: nothing beside a user directory is ours.
    if (!store.isOwned(nullptr)) return entries;

    const QString prefix = QString::fromLatin1(kArtifactDirectoryPrefix);
    const QFileInfoList directories = QDir(store.root()).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& directory : directories) {
        const QString name = directory.fileName();
        // A `.staging-*` directory and any foreign name are not cache entries;
        // they are never listed for deletion and never removed here.
        if (!name.startsWith(prefix)) continue;
        const QString token = name.mid(prefix.size());
        if (!isSha256Hex(token)) continue;

        NativeCacheEntry entry;
        entry.buildKey = QStringLiteral("sha256:") + token;
        entry.directory = directory.absoluteFilePath();

        QString reasonKey;
        QString reason;
        const std::optional<ArtifactRecord> record =
            readArtifact(store, entry.buildKey, &reasonKey, &reason);
        if (record.has_value()) {
            entry.proven = true;
            entry.enginePath = record->enginePath;
            entry.engineBytes = record->manifest.engine.bytes;
        } else {
            entry.reasonKey = reasonKey;
            entry.reason = reason;
        }
        entries.append(entry);
    }
    return entries;
}

bool deleteNativeCacheEntry(const ArtifactStore& store, const QVector<NativeCacheEntry>& entries,
                            const QString& buildKey, QString* reasonKey, QString* reason) {
    const auto refuse = [reasonKey, reason](const QString& key, const QString& text) {
        if (reasonKey != nullptr) *reasonKey = key;
        if (reason != nullptr) *reason = text;
        return false;
    };

    const NativeCacheEntry* found = nullptr;
    for (const NativeCacheEntry& entry : entries) {
        if (entry.buildKey == buildKey) {
            found = &entry;
            break;
        }
    }
    if (found == nullptr) {
        return refuse(QStringLiteral("native.cache.unknown"),
                      QCoreApplication::translate("NativeAssetsPanel",
                                                  "Build key %1 is not an entry of this cache.")
                          .arg(buildKey));
    }
    if (!found->proven) {
        return refuse(found->reasonKey.isEmpty() ? QStringLiteral("native.cache.unproven")
                                                 : found->reasonKey,
                      found->reason.isEmpty()
                          ? QCoreApplication::translate(
                                "NativeAssetsPanel",
                                "This cache entry does not verify, so it is not deleted.")
                          : found->reason);
    }
    return removeArtifact(store, buildKey, reasonKey, reason);
}

NativePreparationToolchain observeNativePreparationToolchain() {
    NativePreparationToolchain toolchain;
    const auto refuse = [&toolchain](const QString& key, const QString& reason) {
        toolchain.available = false;
        toolchain.reasonKey = key;
        toolchain.reason = reason;
        return toolchain;
    };

    // The selected native bundle is where a locally built toolchain and its
    // provider runtimes live. Nothing is created, copied or downloaded here.
    QString bundleRoot;
    std::optional<Bundle> validated;
    const RuntimeSelection& selection = currentSelection();
    if (selection.loaded && selection.kind == BundleKind::Native && !selection.root.isEmpty()) {
        bundleRoot = selection.root;
        toolchain.modulePath = selection.modulePath;
        validated = selection.resolvedBundle;
    }
    if (bundleRoot.isEmpty()) {
        const QList<Bundle> bundles = discoverBundles();
        for (const Bundle& candidate : bundles) {
            if (candidate.kind == BundleKind::Native) {
                bundleRoot = candidate.root;
                toolchain.modulePath = candidate.modulePath;
                validated = candidate;
                break;
            }
        }
    }

    // The selected bundle's own preparation provenance is the only source of the
    // toolchain: the declared executables and provider runtime directories, the
    // inference core's source SHA and ABI, and the provider types it carries. Each
    // path is resolved against the validated bundle root - absolute wherever the
    // block already states one - and nothing is searched for on PATH or beside the
    // binary: an SDK nobody validated is not this build's toolchain.
    if (!validated.has_value() || !validated->preparation.has_value()) {
        return refuse(QStringLiteral("native.prepare.provenance"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The selected native bundle declares no preparation provenance, so its "
                          "toolchain is not a validated one this build will build against."));
    }
    const BundlePreparation& preparation = *validated->preparation;
    toolchain.tools.inspectExecutable = QDir(bundleRoot).filePath(preparation.tools.inspect);
    toolchain.tools.prepExecutable = QDir(bundleRoot).filePath(preparation.tools.prep);
    toolchain.tools.verifierExecutable = QDir(bundleRoot).filePath(preparation.tools.verifier);
    toolchain.tools.ortRuntimeDir = QDir(bundleRoot).filePath(preparation.runtime.ort);

    // The TensorRT and CUDA bin directories are the bundle's own, unless this
    // machine's GPU needs a builder resource the bundle does not carry: then
    // both come from the app-owned component store, so the runtime and the
    // resource are always the same upstream pack set.
    QStringList declaredTrtFiles;
    const QString trtPrefix = preparation.runtime.trt + QLatin1Char('/');
    for (const BundleFile& file : validated->files) {
        if (file.path.startsWith(trtPrefix)) {
            declaredTrtFiles.append(file.path);
        }
    }
    const ComponentBinDirs componentDirs = componentBinDirs(
        bundleRoot, preparation.runtime.trt, preparation.runtime.cuda, declaredTrtFiles,
        observeComputeCapability(), componentStore());
    if (!componentDirs.reasonKey.isEmpty()) {
        return refuse(componentDirs.reasonKey, componentDirs.reason);
    }
    toolchain.tools.trtBinDir = componentDirs.trtBinDir;
    toolchain.tools.cudaBinDir = componentDirs.cudaBinDir;
    toolchain.coreSha = preparation.coreSha;
    toolchain.sourceAbi = preparation.sourceAbi;
    // The block's declared provider types are the bundle's own statement of which
    // backends it carries; a backend the block does not declare is not offered
    // even if a DLL happens to be present.
    for (const QString& type : preparation.types) {
        const std::optional<Backend> declared = backendFromName(type);
        if (declared.has_value() && !toolchain.backends.available.contains(*declared)) {
            toolchain.backends.available.append(*declared);
        }
    }

    // The prepared provider plugin's content revision: the digest of the actual
    // compiled plugin the validated bundle carries, so a cache entry is tied to
    // real bytes rather than to a version string nobody measured.
    const auto digestOf = [&validated](const QString& declaredPath) -> QString {
        for (const BundleFile& file : validated->files) {
            if (file.path == declaredPath) return QStringLiteral("sha256:") + file.sha256;
        }
        return {};
    };
    toolchain.pluginShaTensorRt =
        digestOf(bundlePreparationBackendLibrary(QStringLiteral("tensorrt")));
    toolchain.pluginShaDirectMl =
        digestOf(bundlePreparationBackendLibrary(QStringLiteral("directml")));

    if (toolchain.tools.inspectExecutable.isEmpty() ||
        toolchain.tools.prepExecutable.isEmpty() ||
        toolchain.tools.verifierExecutable.isEmpty() ||
        toolchain.tools.ortRuntimeDir.isEmpty()) {
        return refuse(QStringLiteral("native.prepare.toolsUnavailable"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The preparation toolchain is not installed: the inspect, prep and "
                          "verify executables and an ONNX Runtime directory must all be "
                          "present before an engine can be built."));
    }

    // The provider runtimes the validated bundle declared. A backend it does not
    // name is not offered even if a DLL happens to be present: the block is the
    // statement of what this toolchain carries.
    if (toolchain.backends.available.isEmpty()) {
        return refuse(QStringLiteral("native.prepare.backendUnavailable"),
                      QCoreApplication::translate(
                          "NativeAssetsPanel",
                          "The observed toolchain carries no provider runtime this build can "
                          "prepare for."));
    }

    toolchain.available = true;
    return toolchain;
}

struct NativeAssetsPanel::ImportResult {
    bool ok = false;
    bool published = false;
    bool cancelled = false;
    QString reasonKey;
    QString reason;
    QString modelId;
};

struct NativeAssetsPanel::PrepareResult {
    bool ok = false;
    bool cancelled = false;
    QString reasonKey;
    QString reason;
    int stageIndex = -1;
    QString phase;
    QString toolError;
    QVector<QString> publishedBuildKeys;
    /// The stage fixtures the real builder reads while it runs, kept alive for
    /// the whole job and removed when the result is dropped.
    std::unique_ptr<QTemporaryDir> scratch;
};

namespace {

/// One-line summaries for each actionable section, using the store's own facts.
QString modelsSummaryLine(const QVector<NativeModelView>& models) {
    int usable = 0;
    for (const NativeModelView& view : models) {
        if (view.usable) ++usable;
    }
    if (models.isEmpty()) {
        return QCoreApplication::translate("NativeAssetsPanel", "No owned models yet");
    }
    return QCoreApplication::translate("NativeAssetsPanel", "%1 model(s), %2 ready")
        .arg(models.size())
        .arg(usable);
}

QString cacheSummaryLine(const QVector<NativeCacheEntry>& entries) {
    int proven = 0;
    for (const NativeCacheEntry& entry : entries) {
        if (entry.proven) ++proven;
    }
    if (entries.isEmpty()) {
        return QCoreApplication::translate("NativeAssetsPanel", "No prepared artifacts");
    }
    return QCoreApplication::translate("NativeAssetsPanel", "%1 artifact(s), %2 proven")
        .arg(entries.size())
        .arg(proven);
}

}  // namespace

NativeAssetsPanel::NativeAssetsPanel(QWidget* parent)
    : QWidget(parent), artifacts_(ArtifactStore()) {
    // The single address a delivered worker body may hand a result to. One panel
    // owns the import/preparation surface at a time; the destructor clears it.
    livePanel_ = this;
    modelRoot_ = nativeSubdirectory(QStringLiteral("models"));
#ifdef MIKAI_WITH_MODEL_INSPECTION
    runtime_ = defaultInspectionRuntime();
#endif
    buildUi();
}

QSize NativeAssetsPanel::minimumSizeHint() const {
    // The layout's own minimum counts every child, including table headers and
    // unbroken strings that scroll or wrap instead of voting width. The panel
    // lives inside a page that scrolls as a whole, so its floor is the
    // narrowest usable content: the import row's buttons and fixed fields.
    // Reporting the layout sum would push the page past the smallest window in
    // any language with longer captions, which is exactly the panning the
    // minimum-size probe guards against.
    return {200, 200};
}

void NativeAssetsPanel::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(mikai::app::theme::kSpacingSmall);

    inventoryHeading_ = new QLabel(tr("Owned models"), this);
    inventoryHeading_->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(inventoryHeading_);

    modelSummary_ = new QLabel(this);
    modelSummary_->setObjectName(QStringLiteral("muted"));
    modelSummary_->setWordWrap(true);
    modelSummary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(modelSummary_);

    modelTable_ = new QTableWidget(this);
    modelTable_->setObjectName(QStringLiteral("nativeModelTable"));
    modelTable_->setAccessibleName(tr("Owned model inventory"));
    modelTable_->setColumnCount(3);
    modelTable_->setHorizontalHeaderLabels({tr("Model"), tr("Role"), tr("Status")});
    modelTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    modelTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    modelTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    modelTable_->setAlternatingRowColors(true);
    modelTable_->setShowGrid(false);
    modelTable_->verticalHeader()->setVisible(false);
    modelTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    modelTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    modelTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    modelTable_->horizontalHeader()->setMinimumSectionSize(0);
    modelTable_->setMaximumHeight(mikai::app::theme::kNativeAssetsTableMaxHeight);
    modelTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    modelTable_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(modelTable_);

    modelStatus_ = new QLabel(this);
    modelStatus_->setObjectName(QStringLiteral("muted"));
    modelStatus_->setWordWrap(true);
    root->addWidget(modelStatus_);

    auto* importRow = new QHBoxLayout();
    importRow->setSpacing(mikai::app::theme::kSpacingSmall);
    importDetailsToggle_ = new QPushButton(tr("Import a file…"), this);
    importDetailsToggle_->setObjectName(QStringLiteral("nativeImportDetailsToggle"));
    importDetailsToggle_->setAccessibleName(tr("Show the file-import controls"));
    importDetailsToggle_->setCheckable(true);
    importDetailsToggle_->setChecked(false);
    importRow->addWidget(importDetailsToggle_);
    importRow->addStretch();
    root->addLayout(importRow);

    // The file import is the rare path: it hides behind the toggle above, so
    // the common model and preparation actions own the first screen.
    importDetails_ = new QWidget(this);
    auto* importBox = new QVBoxLayout(importDetails_);
    importBox->setContentsMargins(0, 0, 0, 0);
    importBox->setSpacing(mikai::app::theme::kSpacingSmall);
    auto* importFields = new QHBoxLayout();
    importFields->setSpacing(mikai::app::theme::kSpacingSmall);
    importButton_ = new QPushButton(tr("Import"), importDetails_);
    importButton_->setObjectName(QStringLiteral("nativeImportButton"));
    importButton_->setAccessibleName(tr("Import a model"));
    importFields->addWidget(importButton_);

    stageWidthLabel_ = new QLabel(tr("Stage W"), importDetails_);
    // The field's own caption yields width too, but it stays a single line: it is
    // short by design and wrapping it would stack "Stage" over "W".
    stageWidthLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    importFields->addWidget(stageWidthLabel_);
    stageWidthEdit_ = new QLineEdit(importDetails_);
    stageWidthEdit_->setObjectName(QStringLiteral("nativeStageWidthEdit"));
    // The visible label is the field's accessible name, and the field is that
    // label's buddy: the two dimension boxes were the only actionable controls
    // here without an accessible name, and the label above them carries none of
    // its own. Both reuse the label's already-translated text, so no new string
    // enters the catalogue.
    stageWidthEdit_->setAccessibleName(stageWidthLabel_->text());
    stageWidthLabel_->setBuddy(stageWidthEdit_);
    stageWidthEdit_->setValidator(new QIntValidator(1, 100000, stageWidthEdit_));
    stageWidthEdit_->setFixedWidth(mikai::app::theme::kNativeDimensionFieldWidth);
    importFields->addWidget(stageWidthEdit_);

    stageHeightLabel_ = new QLabel(tr("H"), importDetails_);
    stageHeightLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    importFields->addWidget(stageHeightLabel_);
    stageHeightEdit_ = new QLineEdit(importDetails_);
    stageHeightEdit_->setObjectName(QStringLiteral("nativeStageHeightEdit"));
    stageHeightEdit_->setAccessibleName(stageHeightLabel_->text());
    stageHeightLabel_->setBuddy(stageHeightEdit_);
    stageHeightEdit_->setValidator(new QIntValidator(1, 100000, stageHeightEdit_));
    stageHeightEdit_->setFixedWidth(mikai::app::theme::kNativeDimensionFieldWidth);
    importFields->addWidget(stageHeightEdit_);

    cancelImportButton_ = new QPushButton(tr("Cancel"), importDetails_);
    cancelImportButton_->setObjectName(QStringLiteral("nativeImportCancelButton"));
    cancelImportButton_->setEnabled(false);
    importFields->addWidget(cancelImportButton_);
    importFields->addStretch();
    importBox->addLayout(importFields);

    importStatus_ = new QLabel(importDetails_);
    importStatus_->setObjectName(QStringLiteral("descriptionPanel"));
    importStatus_->setWordWrap(true);
    importStatus_->setMinimumHeight(mikai::app::theme::kSecondaryControlSize);
    importBox->addWidget(importStatus_);
    importDetails_->hide();
    root->addWidget(importDetails_);

    selectRecipeButton_ = new QPushButton(tr("Select as recipe"), this);
    selectRecipeButton_->setObjectName(QStringLiteral("nativeSelectRecipeButton"));
    selectRecipeButton_->setEnabled(false);
    root->addWidget(selectRecipeButton_);

    selectRecipeReason_ = new QLabel(this);
    selectRecipeReason_->setObjectName(QStringLiteral("muted"));
    selectRecipeReason_->setWordWrap(true);
    selectRecipeReason_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(selectRecipeReason_);

    preparationHeading_ = new QLabel(tr("Preparation"), this);
    preparationHeading_->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(preparationHeading_);

    auto* preparationRow = new QHBoxLayout();
    preparationRow->setSpacing(mikai::app::theme::kSpacingSmall);
    prepareButton_ = new QPushButton(tr("Prepare artifact"), this);
    prepareButton_->setObjectName(QStringLiteral("nativePrepareButton"));
    prepareButton_->setAccessibleName(tr("Prepare the selected recipe"));
    prepareButton_->setEnabled(false);
    preparationRow->addWidget(prepareButton_);
    cancelPrepareButton_ = new QPushButton(tr("Cancel"), this);
    cancelPrepareButton_->setObjectName(QStringLiteral("nativePrepareCancelButton"));
    cancelPrepareButton_->setEnabled(false);
    preparationRow->addWidget(cancelPrepareButton_);
    preparationRow->addStretch();
    root->addLayout(preparationRow);

    prepareProgress_ = new QLabel(this);
    prepareProgress_->setObjectName(QStringLiteral("muted"));
    prepareProgress_->setWordWrap(true);
    root->addWidget(prepareProgress_);

    preparationReason_ = new QLabel(this);
    preparationReason_->setObjectName(QStringLiteral("muted"));
    preparationReason_->setWordWrap(true);
    root->addWidget(preparationReason_);

    cacheHeading_ = new QLabel(tr("Prepared artifacts"), this);
    cacheHeading_->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(cacheHeading_);

    // The cache table is optional detail; the summary row remains visible.
    auto* cacheSummaryRow = new QHBoxLayout();
    cacheSummaryRow->setSpacing(mikai::app::theme::kSpacingSmall);
    cacheStatus_ = new QLabel(this);
    cacheStatus_->setObjectName(QStringLiteral("muted"));
    cacheStatus_->setWordWrap(true);
    cacheStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    cacheSummaryRow->addWidget(cacheStatus_, 1);
    cacheDetailsToggle_ = new QPushButton(tr("Details…"), this);
    cacheDetailsToggle_->setObjectName(QStringLiteral("nativeCacheDetailsToggle"));
    cacheDetailsToggle_->setAccessibleName(tr("Show the prepared-artifact details"));
    cacheDetailsToggle_->setCheckable(true);
    cacheDetailsToggle_->setChecked(false);
    cacheSummaryRow->addWidget(cacheDetailsToggle_);
    root->addLayout(cacheSummaryRow);

    cacheDetails_ = new QWidget(this);
    auto* cacheBox = new QVBoxLayout(cacheDetails_);
    cacheBox->setContentsMargins(0, 0, 0, 0);
    cacheBox->setSpacing(mikai::app::theme::kSpacingSmall);
    cacheTable_ = new QTableWidget(cacheDetails_);
    cacheTable_->setObjectName(QStringLiteral("nativeCacheTable"));
    cacheTable_->setAccessibleName(tr("Prepared artifact cache"));
    cacheTable_->setColumnCount(3);
    cacheTable_->setHorizontalHeaderLabels({tr("Engine"), tr("Size"), tr("Status")});
    cacheTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    cacheTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    cacheTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    cacheTable_->setAlternatingRowColors(true);
    cacheTable_->setShowGrid(false);
    cacheTable_->verticalHeader()->setVisible(false);
    cacheTable_->horizontalHeader()->setStretchLastSection(true);
    cacheTable_->horizontalHeader()->setMinimumSectionSize(0);
    cacheTable_->setMaximumHeight(mikai::app::theme::kNativeAssetsTableMaxHeight);
    cacheTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    cacheTable_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    cacheBox->addWidget(cacheTable_);

    auto* cacheRow = new QHBoxLayout();
    deleteCacheButton_ = new QPushButton(tr("Delete"), cacheDetails_);
    deleteCacheButton_->setObjectName(QStringLiteral("nativeCacheDeleteButton"));
    deleteCacheButton_->setEnabled(false);
    cacheRow->addWidget(deleteCacheButton_);
    cacheDeleteHint_ = new QLabel(tr("Remove the selected cache artifact."), cacheDetails_);
    cacheDeleteHint_->setObjectName(QStringLiteral("muted"));
    cacheDeleteHint_->setWordWrap(true);
    cacheDeleteHint_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // Bound to the delete action it explains, like the scratch legend below.
    cacheDeleteHint_->setBuddy(deleteCacheButton_);
    cacheRow->addWidget(cacheDeleteHint_, 1);
    cacheBox->addLayout(cacheRow);
    cacheDetails_->hide();
    root->addWidget(cacheDetails_);

    stagingHeading_ = new QLabel(tr("Incomplete scratch"), this);
    stagingHeading_->setObjectName(QStringLiteral("sectionHeading"));
    stagingHeading_->hide();
    root->addWidget(stagingHeading_);

    stagingList_ = new QListWidget(this);
    stagingList_->setObjectName(QStringLiteral("nativeStagingList"));
    stagingList_->setAccessibleName(tr("Incomplete scratch directories"));
    stagingList_->setMaximumHeight(mikai::app::theme::kNativeAssetsTableMaxHeight / 2);
    // Absolute paths are the items here, so the list scrolls rather than voting
    // for a width no window can hold.
    stagingList_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    stagingList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    stagingList_->hide();
    root->addWidget(stagingList_);

    auto* stagingRow = new QHBoxLayout();
    discardStagingButton_ = new QPushButton(tr("Discard"), this);
    discardStagingButton_->setObjectName(QStringLiteral("nativeStagingDiscardButton"));
    discardStagingButton_->setEnabled(false);
    stagingRow->addWidget(discardStagingButton_);
    stagingDiscardHint_ = new QLabel(tr("Remove the selected incomplete scratch directory."), this);
    stagingDiscardHint_->setObjectName(QStringLiteral("muted"));
    stagingDiscardHint_->setWordWrap(true);
    stagingDiscardHint_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // Bound to the discard action it explains, like the other row legends.
    stagingDiscardHint_->setBuddy(discardStagingButton_);
    stagingRow->addWidget(stagingDiscardHint_, 1);
    stagingRow->addStretch();
    // The scratch cleanup hides with the list: nothing incomplete, nothing to show.
    stagingRow->setEnabled(false);
    root->addLayout(stagingRow);
    stagingNote_ = new QLabel(this);
    stagingNote_->setObjectName(QStringLiteral("muted"));
    stagingNote_->hide();
    root->addWidget(stagingNote_);

    root->addStretch();

    connect(importButton_, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Import ONNX model"), QString(), tr("ONNX graphs (*.onnx)"));
        if (path.isEmpty()) return;
        bool okWidth = false;
        bool okHeight = false;
        const int width = stageWidthEdit_->text().toInt(&okWidth);
        const int height = stageHeightEdit_->text().toInt(&okHeight);
        const QSize geometry = (okWidth && okHeight && width > 0 && height > 0)
                                   ? QSize(width, height)
                                   : QSize();
        importModel(path, geometry);
    });
    connect(cancelImportButton_, &QPushButton::clicked, this, &NativeAssetsPanel::cancelImport);
    connect(deleteCacheButton_, &QPushButton::clicked, this, [this] {
        const int row = cacheTable_->currentRow();
        if (row < 0 || row >= cache_.size()) return;
        deleteCacheEntry(cache_.at(row).buildKey);
    });
    connect(stagingList_, &QListWidget::currentRowChanged, this, [this](int row) {
        discardStagingButton_->setEnabled(row >= 0);
    });
    connect(discardStagingButton_, &QPushButton::clicked, this, [this] {
        const int row = stagingList_->currentRow();
        if (row < 0 || row >= staging_.size()) return;
        const QString directory = staging_.at(row);
        // Removing scratch is destructive and cannot be undone, so it is
        // confirmed by the exact directory the user selected. Only a path
        // `stagingDirectories` proved to be an owned `.staging-*` directory is
        // ever offered here; a proven artifact is never a candidate.
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, tr("Discard incomplete scratch"),
            tr("Remove the incomplete scratch directory %1?\n\n"
               "This removes only work a cancelled or crashed build left behind; a proven "
               "artifact is never affected.")
                .arg(directory),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
        QString reasonKey;
        QString reason;
        if (discardStaging(artifacts_, directory, &reasonKey, &reason)) {
            applyStatus({}, tr("Discarded scratch directory %1.").arg(directory), stagingNote_);
        } else {
            applyStatus(reasonKey, reason, stagingNote_);
        }
        refreshCache();
    });
    connect(modelTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        updateImportControls();
    });
    connect(selectRecipeButton_, &QPushButton::clicked, this, [this] {
        const int row = modelTable_->currentRow();
        if (row < 0 || row >= models_.size()) return;
        selectModelAsRecipe(models_.at(row).id);
    });
    connect(importDetailsToggle_, &QPushButton::toggled, this, [this](bool on) {
        if (importDetails_ != nullptr) importDetails_->setVisible(on);
        importDetailsToggle_->setText(on ? tr("Hide import") : tr("Import a file…"));
        importDetailsToggle_->setAccessibleName(
            on ? tr("Hide the file-import controls")
               : tr("Show the file-import controls"));
    });
    connect(cacheDetailsToggle_, &QPushButton::toggled, this, [this](bool on) {
        if (cacheDetails_ != nullptr) cacheDetails_->setVisible(on);
        cacheDetailsToggle_->setText(on ? tr("Hide details") : tr("Details…"));
        cacheDetailsToggle_->setAccessibleName(
            on ? tr("Hide the prepared-artifact details")
               : tr("Show the prepared-artifact details"));
    });
    connect(cacheTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = cacheTable_->currentRow();
        const bool proven = row >= 0 && row < cache_.size() && cache_.at(row).proven;
        deleteCacheButton_->setEnabled(proven);
        if (row >= 0 && row < cache_.size() && !proven) {
            cacheStatus_->setText(cache_.at(row).reason);
        }
    });
    connect(prepareButton_, &QPushButton::clicked, this, [this] { prepareArtifact(); });
    connect(cancelPrepareButton_, &QPushButton::clicked, this,
            &NativeAssetsPanel::cancelPrepare);

    // Every caption yields its width instead of voting for the page's minimum: a
    // long translated heading, hint or provenance path wraps or scrolls, and the
    // panel never pushes the settings page wider than the smallest window.
    const QList<QLabel*> captions = {
        modelSummary_, inventoryHeading_, preparationHeading_,
        preparationReason_, prepareProgress_, cacheHeading_, cacheDeleteHint_,
        cacheStatus_, stagingDiscardHint_, stagingNote_};
    for (QLabel* caption : captions) {
        if (caption == nullptr) continue;
        caption->setWordWrap(true);
        caption->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    // The visible captions are the concise verb, because a QPushButton cannot
    // wrap; the accessible name keeps the full, specific action so a screen
    // reader never loses what the button does in any state.
    cancelImportButton_->setAccessibleName(tr("Cancel the running model import"));
    selectRecipeButton_->setAccessibleName(
        tr("Register the selected model as the requested recipe"));
    deleteCacheButton_->setAccessibleName(tr("Delete the selected prepared artifact"));
    discardStagingButton_->setAccessibleName(
        tr("Discard the selected incomplete scratch directory"));
    cancelPrepareButton_->setAccessibleName(tr("Cancel the running preparation"));

}

void NativeAssetsPanel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void NativeAssetsPanel::retranslateUi() {
    // In place. Nothing is re-read: the owned model rows, the artifact cache
    // and the prepared pipeline are the store's own facts, and re-reading them
    // to say the same thing in a new language would move the user's selection,
    // reset a revealed scratch directory and hide whatever an in-flight import
    // or preparation was reporting. A worker already running keeps running: its
    // gate, its generation and its callbacks are untouched, and its completion
    // paints itself in whatever language is active when it lands.
    inventoryHeading_->setText(tr("Owned models"));
    modelTable_->setAccessibleName(tr("Owned model inventory"));
    modelTable_->setHorizontalHeaderLabels({tr("Model"), tr("Role"), tr("Status")});

    stageWidthLabel_->setText(tr("Stage W"));
    stageHeightLabel_->setText(tr("H"));
    stageWidthEdit_->setAccessibleName(stageWidthLabel_->text());
    stageHeightEdit_->setAccessibleName(stageHeightLabel_->text());
    importButton_->setText(tr("Import"));
    importButton_->setAccessibleName(tr("Import a model"));
    cancelImportButton_->setText(tr("Cancel"));
    cancelImportButton_->setAccessibleName(tr("Cancel the running model import"));
    importDetailsToggle_->setText(importDetails_ != nullptr && importDetails_->isVisible()
                                      ? tr("Hide import")
                                      : tr("Import a file…"));
    importDetailsToggle_->setAccessibleName(importDetails_ != nullptr && importDetails_->isVisible()
                                                ? tr("Hide the file-import controls")
                                                : tr("Show the file-import controls"));
    selectRecipeButton_->setText(tr("Select as recipe"));
    selectRecipeButton_->setAccessibleName(
        tr("Register the selected model as the requested recipe"));

    preparationHeading_->setText(tr("Preparation"));
    cancelPrepareButton_->setText(tr("Cancel"));
    cancelPrepareButton_->setAccessibleName(tr("Cancel the running preparation"));

    cacheHeading_->setText(tr("Prepared artifacts"));
    cacheTable_->setAccessibleName(tr("Prepared artifact cache"));
    cacheDetailsToggle_->setText(cacheDetailsToggle_->isChecked() ? tr("Hide details") : tr("Details…"));
    cacheDetailsToggle_->setAccessibleName(
        cacheDetailsToggle_->isChecked() ? tr("Hide the prepared-artifact details")
                                        : tr("Show the prepared-artifact details"));
    cacheTable_->setHorizontalHeaderLabels({tr("Engine"), tr("Size"), tr("Status")});
    deleteCacheButton_->setText(tr("Delete"));
    deleteCacheButton_->setAccessibleName(tr("Delete the selected prepared artifact"));
    cacheDeleteHint_->setText(tr("Remove the selected cache artifact."));

    stagingHeading_->setText(tr("Incomplete scratch"));
    stagingList_->setAccessibleName(tr("Incomplete scratch directories"));
    discardStagingButton_->setText(tr("Discard"));
    discardStagingButton_->setAccessibleName(
        tr("Discard the selected incomplete scratch directory"));
    stagingDiscardHint_->setText(tr("Remove the selected incomplete scratch directory."));

    // The control refreshes restate only this panel's own captions and
    // tooltips; the per-row verdicts keep whatever the store and the producer
    // last said about them, because they are the record rather than chrome.
    updateImportControls();
    updateSummaries();
    updatePrepareControls();
}

void NativeAssetsPanel::setRunner(mikai::app::Runner* runner) {
    runner_ = runner;
    updateImportControls();
}

void NativeAssetsPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!initialRefreshDone_) {
        initialRefreshDone_ = true;
        refresh();
    }
}

void NativeAssetsPanel::refresh() {
    initialRefreshDone_ = true;
    refreshModels();
    refreshCache();
    refreshSourceData();
    // One observation per refresh, on the GUI thread: the toolchain and the
    // provider runtimes the validated bundle and the application directory
    // really carry. A selection change reads this value instead of re-walking it.
    observedToolchain_ = observeNativePreparationToolchain();
    updateImportControls();
    updateSummaries();
    updatePrepareControls();
    emit catalogRefreshed();
}

void NativeAssetsPanel::updateSummaries() {
    if (modelSummary_ != nullptr) modelSummary_->setText(modelsSummaryLine(models_));
    if (cacheStatus_ != nullptr) cacheStatus_->setText(cacheSummaryLine(cache_));
    cacheDetailsToggle_->setVisible(!cache_.isEmpty());
    cacheDetails_->setVisible(cacheDetailsToggle_->isChecked() && !cache_.isEmpty());
    const bool hasScratch = !staging_.isEmpty();
    if (stagingHeading_ != nullptr) stagingHeading_->setVisible(hasScratch);
    if (stagingList_ != nullptr) stagingList_->setVisible(hasScratch);
    discardStagingButton_->setVisible(hasScratch);
    stagingDiscardHint_->setVisible(hasScratch);
    if (stagingNote_ != nullptr) {
        stagingNote_->setVisible(hasScratch);
        if (hasScratch) {
            stagingNote_->setText(tr("%1 incomplete scratch director(ies) left by a cancelled or crashed build.").arg(staging_.size()));
        }
    }
}
void NativeAssetsPanel::refreshModels() {
    // The user's chosen model is a stable id, never a row index: rebuilding the
    // table must not move their choice.
    QString selectedModelId;
    const int previousRow = modelTable_->currentRow();
    if (previousRow >= 0 && previousRow < models_.size()) {
        selectedModelId = models_.at(previousRow).id;
    }

    models_.clear();
    storedModels_.clear();

    // The owned store's read path is unconditional: listing and describing the
    // app-owned rows needs no ONNX Runtime, so the catalogue is readable even in
    // a build without the inspector. Importing a new graph still is not.
    QString catalogKey;
    QString catalogReason;
    const ModelInventory inventory(modelRoot_);
    QString ownedReason;
    if (!inventory.isOwned(&ownedReason)) {
        catalogKey = QStringLiteral("model.store.unowned");
        catalogReason = ownedReason.isEmpty()
                            ? tr("The model store root is not app-owned, so it is not read.")
                            : ownedReason;
    } else {
        QString reason;
        const QVector<StoredModel> rows = inventory.stored(&reason);
        storedModels_ = rows;
        for (const StoredModel& row : rows) models_.append(describeStoredModel(row));
        if (!reason.isEmpty()) {
            // A genuine read failure names itself; it is never hidden behind an
            // empty table.
            catalogKey = QStringLiteral("model.store.unreadable");
            catalogReason = reason;
        } else if (models_.isEmpty()) {
            // An owned store nothing has been imported into yet is honestly
            // empty, not missing: first use carries no diagnostic about a root
            // the app owns and will create on its first write.
            catalogKey = QStringLiteral("native.models.empty");
            catalogReason = tr("No models are stored yet.");
        }
    }

    modelTable_->setRowCount(models_.size());
    modelTable_->setVisible(!models_.isEmpty());
    for (int row = 0; row < models_.size(); ++row) {
        const NativeModelView& view = models_.at(row);
        const QStringList cells = {view.label, view.role, statusCell(view)};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells.at(column));
            if (view.usable) {
                item->setToolTip(cells.at(column));
            } else {
                item->setToolTip(tr("Reason: %1\n%2").arg(view.statusKey, view.statusReason));
            }
            modelTable_->setItem(row, column, item);
        }
    }

    // The read's named failure remains visible; a valid empty store is represented
    // once by the existing model summary.
    const bool emptyStore = catalogKey == QLatin1String("native.models.empty");
    modelStatus_->setText(catalogReason.isEmpty() ? catalogKey : catalogReason);
    modelStatus_->setVisible(!emptyStore && !modelStatus_->text().isEmpty());
    // Re-selecting by id restores the user's choice without changing its status.
    if (!selectedModelId.isEmpty()) {
        for (int row = 0; row < models_.size(); ++row) {
            if (models_.at(row).id == selectedModelId) {
                modelTable_->setCurrentCell(row, 0);
                break;
            }
        }
    }
}

void NativeAssetsPanel::refreshCache() {
    cache_ = enumerateNativeCache(artifacts_);
    staging_ = stagingDirectories(artifacts_);

    cacheTable_->setRowCount(cache_.size());
    for (int row = 0; row < cache_.size(); ++row) {
        const NativeCacheEntry& entry = cache_.at(row);
        const QStringList cells = {
            entry.proven ? QFileInfo(entry.enginePath).fileName() : QStringLiteral("—"),
            entry.proven ? humanBytes(entry.engineBytes) : QStringLiteral("—"),
            entry.proven ? tr("Proven") : entry.reason};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells.at(column));
            item->setToolTip(entry.proven ? entry.enginePath : entry.reason);
            cacheTable_->setItem(row, column, item);
        }
    }
    deleteCacheButton_->setEnabled(false);

    stagingList_->clear();
    for (const QString& directory : staging_) stagingList_->addItem(directory);
    discardStagingButton_->setEnabled(false);
    updateSummaries();
}

void NativeAssetsPanel::refreshSourceData() {
    // Widget-free: the pinned catalogue and the published copies the data API
    // resolves by digest. The Components tab owns the visible inventory.
    sourceAssets_ = spatialAssetCatalog();
    publishedAssets_ = SpatialAssetStore().stored();
}
QString NativeAssetsPanel::publishedAssetPath(const QString& id) const {
    for (const StoredSpatialAsset& row : publishedAssets_) {
        if (row.verified && row.asset.id == id) return row.graphPath;
    }
    return {};
}

bool NativeAssetsPanel::importPublishedAsset(const QString& id, const QSize& stageGeometry) {
    const QString path = publishedAssetPath(id);
    if (path.isEmpty()) {
        applyStatus(QStringLiteral("native.import.downloadedMissing"),
                    tr("That source asset is not present and verified in the owned store yet; "
                       "get and verify it in Components first."),
                    importStatus_);
        return false;
    }
    // The one import path: the existing ONNX inspector with an explicit geometry,
    // off the GUI thread. It publishes a model row and nothing else - no build,
    // no preparation, no Ready pipeline.
    return importModel(path, stageGeometry);
}

void NativeAssetsPanel::applyStatus(const QString& key, const QString& reason, QLabel* target) {
    statusKey_ = key;
    statusReason_ = reason;
    if (target == nullptr) return;
    target->setText(reason.isEmpty() ? key : reason);
}

void NativeAssetsPanel::updateImportControls() {
    bool runtimePresent = false;
#ifdef MIKAI_WITH_MODEL_INSPECTION
    runtimePresent = inspectionRuntimeIsPresent(runtime_, nullptr);
#endif
    const bool canImport = runtimePresent && runner_ != nullptr && !importInFlight_;
    importButton_->setEnabled(canImport);
    cancelImportButton_->setEnabled(importInFlight_);

    if (!runtimePresent) {
#ifdef MIKAI_WITH_MODEL_INSPECTION
        const QString reason = inspectionRuntimeReason();
#else
        const QString reason = tr("This build was made without the ONNX Runtime inspector "
                                  "(MIKAI_WITH_MODEL_INSPECTION is off).");
#endif
        importButton_->setToolTip(reason);
    } else if (runner_ == nullptr) {
        importButton_->setToolTip(
            tr("No off-thread runner is available, so no import can be submitted."));
    } else {
        importButton_->setToolTip(tr("Inspect and publish a graph into the owned store."));
    }

    const int selectedRow = modelTable_ != nullptr ? modelTable_->currentRow() : -1;
    const bool usableSelected =
        selectedRow >= 0 && selectedRow < models_.size() && models_.at(selectedRow).usable;
    selectRecipeButton_->setVisible(!models_.isEmpty());
    selectRecipeReason_->setVisible(!models_.isEmpty());
    selectRecipeButton_->setEnabled(usableSelected);
    selectRecipeReason_->setText(
        usableSelected
            ? tr("Registers this model as a single-spatial recipe for the current backend. "
                 "The request is written only when Save is pressed.")
            : tr("Select a model this build offers (Ready) to register it as a recipe."));

    updatePrepareControls();
}

QString NativeAssetsPanel::inspectionRuntimeReason() const {
#ifdef MIKAI_WITH_MODEL_INSPECTION
    QString reason;
    if (!inspectionRuntimeIsPresent(runtime_, &reason)) {
        return reason.isEmpty() ? tr("The ONNX Runtime is not present.") : reason;
    }
#endif
    return {};
}

NativeAssetsPanel* NativeAssetsPanel::livePanel_ = nullptr;

NativeAssetsPanel* NativeAssetsPanel::livePanel() {
    // Read only from a body the dispatcher delivered, on the GUI thread, in the
    // one moment this widget is known to be readable.
    return livePanel_;
}

NativeAssetsPanel::~NativeAssetsPanel() {
    // Ask an in-flight worker to stop before this widget goes away. The worker
    // holds its own shared control, so the request is issued here and the GUI
    // thread never waits for the pool to observe it.
    if (importCancel_) importCancel_->cancel();
    if (prepareCancel_) prepareCancel_->cancel();
    // Anything still queued for this widget is dropped rather than delivered to
    // an owner that is going away.
    alive_->store(false);
    livePanel_ = nullptr;
}

bool NativeAssetsPanel::importModel(const QString& sourcePath, const QSize& stageGeometry) {
    applyStatus({}, {}, importStatus_);
    if (!inspectorBuilds()) {
        applyStatus(QStringLiteral("native.inspect.unavailable"),
                    tr("This build was made without the ONNX Runtime inspector "
                       "(MIKAI_WITH_MODEL_INSPECTION is off), so no graph can be inspected or "
                       "imported."),
                    importStatus_);
        return false;
    }
    if (importInFlight_) {
        applyStatus(QStringLiteral("native.import.busy"),
                    tr("An import is already running; wait for it or cancel it first."),
                    importStatus_);
        return false;
    }
    if (sourcePath.trimmed().isEmpty() || !QFileInfo(sourcePath).isAbsolute()) {
        applyStatus(QStringLiteral("native.import.source"),
                    tr("Choose an absolute path to a regular ONNX file."), importStatus_);
        return false;
    }
    if (!stageGeometry.isValid() || stageGeometry.width() <= 0 || stageGeometry.height() <= 0) {
        applyStatus(QStringLiteral("native.import.geometry"),
                    tr("A positive stage geometry (WxH) is required: a dynamic graph's CPU "
                       "reference resolves at exactly this size, and it is never defaulted."),
                    importStatus_);
        return false;
    }
    if (runner_ == nullptr) {
        applyStatus(QStringLiteral("native.import.runner"),
                    tr("No off-thread runner is available, so no import can be submitted."),
                    importStatus_);
        return false;
    }
#ifdef MIKAI_WITH_MODEL_INSPECTION
    if (!inspectionRuntimeIsPresent(runtime_, nullptr)) {
        applyStatus(QStringLiteral("native.inspect.runtimeUnavailable"),
                    inspectionRuntimeReason(), importStatus_);
        return false;
    }

    auto cancellation = std::make_shared<ImportCancellation>();
    importCancel_ = cancellation;
    auto result = std::make_shared<ImportResult>();
    importInFlight_ = true;
    const quint64 generation = ++importGeneration_;
    updateImportControls();

    const QString root = modelRoot_;
    const InspectionRuntime runtime = runtime_;
    const QString path = QFileInfo(sourcePath).absoluteFilePath();
    // The completions' only reference to this widget is its own alive flag: no
    // widget pointer is captured off-thread, and the GUI body resolves the
    // address it is allowed to touch.
    const std::shared_ptr<std::atomic_bool> alive = alive_;

    runner_->run(
        [root, path, stageGeometry, runtime, cancellation, result]() -> QVariant {
            if (cancellation->isCancelled()) {
                result->cancelled = true;
                result->reasonKey = QStringLiteral("native.import.cancelled");
                result->reason = QCoreApplication::translate(
                    "NativeAssetsPanel", "The import was cancelled before it started.");
                return QVariant();
            }
            ModelInventory inventory(root);
            const ModelImportOutcome outcome =
                inventory.importFile(path, runtime, stageGeometry, BackendAvailability(),
                                     QString(), cancellation.get());
            result->ok = outcome.ok;
            result->published = outcome.published;
            result->reasonKey = outcome.reasonKey;
            result->reason = outcome.reason;
            result->modelId = outcome.model.record.id;
            // Only a cancel that won the publication gate is reported as one.
            // Once publishing has begun this stays false, so the committed
            // outcome is delivered unchanged.
            if (cancellation->isCancelled()) result->cancelled = true;
            return QVariant();
        },
        [alive, result, generation](const QVariant&) {
            NativeAssetsPanel* target = livePanel();
            if (target == nullptr || !alive->load(std::memory_order_relaxed)) return;
            target->finishImport(result, generation);
        },
        [alive, result, generation](const backend::Failure& failure) {
            NativeAssetsPanel* target = livePanel();
            if (target == nullptr || !alive->load(std::memory_order_relaxed)) return;
            result->reasonKey = QStringLiteral("native.import.failed");
            result->reason = backend::failureLine(failure);
            target->finishImport(result, generation);
        });
    return true;
#else
    return false;
#endif
}

void NativeAssetsPanel::cancelImport() {
    if (!importInFlight_ || !importCancel_) return;
    const ImportCancellation::CancelResult result = importCancel_->cancel();
    if (result == ImportCancellation::CancelResult::TooLate) {
        // The worker already passed the publication gate. Nothing can stop the
        // write, so the import is not reported as cancelled and its committed
        // outcome is still delivered by the completion.
        applyStatus(QStringLiteral("native.import.commitInProgress"),
                    tr("The import has already begun publishing, so it can no longer be "
                       "cancelled; its committed outcome will be reported."),
                    importStatus_);
        return;
    }
    applyStatus(QStringLiteral("native.import.cancelling"),
                tr("Cancelling the import; the worker decides at its next check."),
                importStatus_);
    updateImportControls();
}

void NativeAssetsPanel::finishImport(const std::shared_ptr<ImportResult>& result,
                                     quint64 generation) {
    if (generation != importGeneration_) return;
    importInFlight_ = false;
    importCancel_.reset();

    if (result->published) {
        // The worker passed the publication gate, so the row is committed and
        // the outcome is reported as such, never re-labelled as a cancellation.
        applyStatus({}, {}, importStatus_);
    } else if (result->cancelled) {
        applyStatus(QStringLiteral("native.import.cancelled"),
                    tr("The import was cancelled; nothing was published."), importStatus_);
    } else if (result->ok) {
        applyStatus({}, {}, importStatus_);
    } else {
        applyStatus(result->reasonKey.isEmpty() ? QStringLiteral("native.import.failed")
                                                : result->reasonKey,
                    result->reason.isEmpty()
                        ? tr("The graph was refused and nothing was published.")
                        : result->reason,
                    importStatus_);
    }

    refresh();
    emit importFinished(result->ok);
}

bool NativeAssetsPanel::deleteCacheEntry(const QString& buildKey) {
    QString reasonKey;
    QString reason;
    const bool removed = deleteNativeCacheEntry(artifacts_, cache_, buildKey, &reasonKey, &reason);
    applyStatus(reasonKey, reason, cacheStatus_);
    refreshCache();
    return removed;
}

void NativeAssetsPanel::setRequestedBackend(Backend backend) {
    requestedBackend_ = backend;
    updateImportControls();
}

void NativeAssetsPanel::setPreparationDesiredBackend(
    std::function<Backend()> provider) {
    preparationDesiredBackendProvider_ = std::move(provider);
}

bool NativeAssetsPanel::selectModelAsRecipe(const QString& modelId) {
    applyStatus({}, {}, selectRecipeReason_);
    const StoredModel* found = nullptr;
    for (const StoredModel& model : storedModels_) {
        if (model.record.id == modelId) {
            found = &model;
            break;
        }
    }
    if (found == nullptr) {
        applyStatus(QStringLiteral("native.recipe.modelUnknown"),
                    tr("That model is not one of the rows this store offers."),
                    selectRecipeReason_);
        return false;
    }
    if (!found->record.availability.isUsable()) {
        applyStatus(found->record.availability.reasonKey.isEmpty()
                        ? QStringLiteral("native.recipe.modelUnusable")
                        : found->record.availability.reasonKey,
                    found->record.availability.reason, selectRecipeReason_);
        return false;
    }

    // The store owns the recipe document and its stable id; this panel only asks
    // for it and reports the id. It never writes the requested selection.
    UserRecipeStore store;
    QString reasonKey;
    QString reason;
    const std::optional<Recipe> recipe =
        store.addSpatialModel(found->record, requestedBackend_, &reasonKey, &reason);
    if (!recipe.has_value()) {
        applyStatus(reasonKey, reason, selectRecipeReason_);
        return false;
    }
    selectRecipeReason_->setText(tr("Registered recipe %1 for %2.")
                                     .arg(recipe->id, stableName(requestedBackend_)));
    emit recipeRequested(recipe->id);
    return true;
}

void NativeAssetsPanel::setPreparationContextProvider(NativePreparationContextProvider provider) {
    contextProvider_ = std::move(provider);
    updatePrepareControls();
}

void NativeAssetsPanel::setPreparationSourcePresence(NativePreparationSourcePresence presence) {
    sourcePresence_ = std::move(presence);
    updatePrepareControls();
}

void NativeAssetsPanel::setPreparationContextRequest(NativePreparationContextRequest request) {
    contextRequest_ = std::move(request);
    updatePrepareControls();
}

void NativeAssetsPanel::deliverPreparationContext(
    const mplayer::native::NativePreparationContext& context) {
    if (!prepareInFlight_ || !contextPending_) return;
    contextPending_ = false;
    if (prepareCancel_ && prepareCancel_->isCancelled()) {
        auto result = std::make_shared<PrepareResult>();
        result->cancelled = true;
        result->reasonKey = QStringLiteral("native.prepare.cancelled");
        result->reason = tr("The preparation was cancelled while probing; nothing was built.");
        finishPrepare(result, prepareGeneration_);
        return;
    }
    // The probed context is used exactly as observed: an absent geometry or
    // device stays absent and the plan refuses it by name rather than this panel
    // filling it with a guess.
    jobContext_ = context;
    requestNextPreparationFrame();
}

void NativeAssetsPanel::failPreparationContext(const QString& reasonKey, const QString& reason) {
    if (!prepareInFlight_ || !contextPending_) return;
    contextPending_ = false;
    auto result = std::make_shared<PrepareResult>();
    result->reasonKey = reasonKey.isEmpty() ? QStringLiteral("native.prepare.context") : reasonKey;
    result->reason =
        reason.isEmpty()
            ? tr("The live source's geometry and device could not be observed.")
            : reason;
    finishPrepare(result, prepareGeneration_);
}

void NativeAssetsPanel::setPreparationFrameRequest(NativePreparationFrameRequest request) {
    frameRequest_ = std::move(request);
    updatePrepareControls();
}

void NativeAssetsPanel::deliverPreparationFrame(int frameIndex,
                                                const mplayer::native::FrameFixture& frame,
                                                quint64 sourceEpoch,
                                                double observedPosition) {
    // A frame from a superseded job, or one nobody asked for, changes nothing.
    if (!prepareInFlight_ || !capturePending_ || frameIndex != frameIndexRequested_) return;
    // The source epoch and position are the owner's guard: it only delivers a
    // frame from the source the job was started against. This panel guards the
    // job generation and the frame index.
    Q_UNUSED(sourceEpoch);
    Q_UNUSED(observedPosition);
    capturePending_ = false;
    frameIndexRequested_ = -1;
    if (prepareCancel_ && prepareCancel_->isCancelled()) {
        auto result = std::make_shared<PrepareResult>();
        result->cancelled = true;
        result->reasonKey = QStringLiteral("native.prepare.cancelled");
        result->reason =
            tr("The preparation was cancelled while capturing; nothing was built.");
        finishPrepare(result, prepareGeneration_);
        return;
    }
    capturedFrames_.append(frame);
    requestNextPreparationFrame();
}

void NativeAssetsPanel::failPreparationFrame(int frameIndex, const QString& reasonKey,
                                             const QString& reason) {
    if (!prepareInFlight_ || !capturePending_ || frameIndex != frameIndexRequested_) return;
    capturePending_ = false;
    frameIndexRequested_ = -1;
    auto result = std::make_shared<PrepareResult>();
    result->reasonKey = reasonKey.isEmpty() ? QStringLiteral("native.prepare.capture") : reasonKey;
    result->reason =
        reason.isEmpty()
            ? tr("The actual decoded frame this reference needs could not be captured.")
            : reason;
    finishPrepare(result, prepareGeneration_);
}

void NativeAssetsPanel::requestNextPreparationFrame() {
    if (!prepareInFlight_) return;
    if (capturedFrames_.size() >= framesNeeded_) {
        submitPreparationJob(capturedFrames_, prepareGeneration_);
        return;
    }
    if (!frameRequest_) {
        auto result = std::make_shared<PrepareResult>();
        result->reasonKey = QStringLiteral("native.prepare.capture");
        result->reason = tr("No raw-frame capture path is wired, so the CPU reference this build "
                            "verifies against cannot be supplied.");
        finishPrepare(result, prepareGeneration_);
        return;
    }
    frameIndexRequested_ = capturedFrames_.size();
    capturePending_ = true;
    if (prepareProgress_ != nullptr) {
        prepareProgress_->setText(tr("Capturing the actual source frame %1 of %2…")
                                      .arg(frameIndexRequested_ + 1)
                                      .arg(framesNeeded_));
    }
    frameRequest_(frameIndexRequested_, prepareGeneration_);
}

void NativeAssetsPanel::setRequestedRecipe(const QString& recipeId) {
    if (requestedRecipeId_ == recipeId) return;
    requestedRecipeId_ = recipeId;
    // A different selection is a different request: a previous job's verdict no
    // longer describes it.
    prepareStatusKey_.clear();
    prepareStatusReason_.clear();
    prepareProgress_->clear();
    updatePrepareControls();
}

std::optional<NativePreparationContext> NativeAssetsPanel::currentContext() const {
    return contextProvider_ ? contextProvider_() : std::nullopt;
}

QString NativeAssetsPanel::preparationReadinessKey() const {
    QString key;
    QString reason;
    prepareRefusal(&key, &reason);
    return key;
}

QString NativeAssetsPanel::preparationReadinessReason() const {
    QString key;
    QString reason;
    prepareRefusal(&key, &reason);
    return reason;
}

void NativeAssetsPanel::prepareRefusal(QString* reasonKey, QString* reason) const {
    if (reasonKey != nullptr) reasonKey->clear();
    if (reason != nullptr) reason->clear();
    const auto refuse = [reasonKey, reason](const QString& key, const QString& text) {
        if (reasonKey != nullptr) *reasonKey = key;
        if (reason != nullptr) *reason = text;
    };

    if (!inspectorBuilds()) {
        refuse(QStringLiteral("native.prepare.inspector"),
               tr("This build was made without the ONNX Runtime inspector, so no plan can be "
                  "resolved and no engine built."));
        return;
    }
    if (runner_ == nullptr) {
        refuse(QStringLiteral("native.prepare.runner"),
               tr("No off-thread runner is available, so no preparation can be submitted."));
        return;
    }
    if (requestedRecipeId_.isEmpty()) {
        refuse(QStringLiteral("native.prepare.recipe"),
               tr("Select a registered recipe in this page first; the untouched Original "
                  "pipeline is never prepared."));
        return;
    }
    UserRecipeStore recipes;
    QString recipeKey;
    QString recipeReason;
    const std::optional<Recipe> recipe = recipes.find(requestedRecipeId_, &recipeKey, &recipeReason);
    if (!recipe.has_value()) {
        refuse(recipeKey.isEmpty() ? QStringLiteral("native.prepare.recipe") : recipeKey,
               recipeReason.isEmpty()
                   ? tr("The owned recipe store does not hold that recipe.") : recipeReason);
        return;
    }
    for (const SpatialStage& stage : recipe->spatial) {
        bool usable = false;
        for (const StoredModel& row : storedModels_) {
            if (row.record.id == stage.modelId && row.record.availability.isUsable()) {
                usable = true;
                break;
            }
        }
        if (!usable) {
            refuse(QStringLiteral("native.prepare.models"),
                   tr("The owned store holds no usable %1 for this recipe.").arg(stage.modelId));
            return;
        }
    }
    // An actual source a probe and a capture can run against, which is distinct
    // from a fully observed context: the device facts come from the native probe,
    // and this panel only arms that probe after the user clicks Prepare. Requiring
    // the context here would make the successful path unreachable.
    if (!sourcePresence_) {
        refuse(QStringLiteral("native.prepare.source"),
               tr("No source-presence path is wired, so there is no live video to prepare for."));
        return;
    }
    QString presenceKey;
    QString presenceReason;
    if (!sourcePresence_(&presenceKey, &presenceReason)) {
        refuse(presenceKey.isEmpty() ? QStringLiteral("native.prepare.source") : presenceKey,
               presenceReason.isEmpty()
                   ? tr("No playing or paused video is available to prepare for.")
                   : presenceReason);
        return;
    }
    if (!contextRequest_ && !currentContext().has_value()) {
        refuse(QStringLiteral("native.prepare.context"),
               tr("No native probe path is wired, so the live source's geometry and device "
                  "cannot be observed."));
        return;
    }
    if (!observedToolchain_.available) {
        refuse(observedToolchain_.reasonKey.isEmpty() ? QStringLiteral("native.prepare.toolchain")
                                                      : observedToolchain_.reasonKey,
               observedToolchain_.reason.isEmpty()
                   ? tr("The preparation toolchain has not been observed.")
                   : observedToolchain_.reason);
        return;
    }
    if (!observedToolchain_.backends.has(recipe->backend)) {
        refuse(QStringLiteral("backend.unavailable"),
               tr("The observed toolchain does not carry the %1 backend this recipe asks for.")
                   .arg(stableName(recipe->backend)));
        return;
    }
    if (!frameRequest_) {
        refuse(QStringLiteral("native.prepare.capture"),
               tr("No raw-frame capture path is wired, so the actual decoded frame this "
                  "reference needs cannot be requested."));
        return;
    }
}

void NativeAssetsPanel::updatePrepareControls() {
    if (prepareButton_ == nullptr) return;
    QString key;
    QString reason;
    prepareRefusal(&key, &reason);
    const bool ready = key.isEmpty();
    prepareButton_->setEnabled(ready && !prepareInFlight_);
    cancelPrepareButton_->setEnabled(prepareInFlight_);
    prepareButton_->setText(currentContext().has_value() ? tr("Prepare artifact")
                                                         : tr("Probe and prepare"));
    prepareButton_->setToolTip(
        ready ? tr("Build every stage of the selected recipe into the owned artifact cache.")
              : reason);
    if (prepareInFlight_) {
        preparationReason_->setText(tr("Preparing the selected recipe…"));
    } else if (!prepareStatusReason_.isEmpty()) {
        preparationReason_->setText(prepareStatusReason_);
    } else if (!ready) {
        preparationReason_->setText(reason);
    } else {
        preparationReason_->setText(
            tr("Ready to prepare: the source, the toolchain and every owned model this recipe "
               "needs are observed."));
    }
}

bool NativeAssetsPanel::prepareArtifact() {
    prepareStatusKey_.clear();
    prepareStatusReason_.clear();
    QString key;
    QString reason;
    prepareRefusal(&key, &reason);
    if (!key.isEmpty()) {
        prepareStatusKey_ = key;
        prepareStatusReason_ = reason;
        updatePrepareControls();
        return false;
    }
#ifdef MIKAI_WITH_MODEL_INSPECTION
    // How many actual frames the oracle needs is decided by the recipe before any
    // capture is requested: a spatial chain needs one, an interpolation stage two
    // distinct ones, and the surface never fabricates a pair.
    UserRecipeStore recipes;
    QString recipeKey;
    QString recipeReason;
    const std::optional<Recipe> recipe =
        recipes.find(requestedRecipeId_, &recipeKey, &recipeReason);
    if (!recipe.has_value()) {
        prepareStatusKey_ =
            recipeKey.isEmpty() ? QStringLiteral("native.prepare.recipe") : recipeKey;
        prepareStatusReason_ = recipeReason;
        updatePrepareControls();
        return false;
    }

    prepareCancel_ = std::make_shared<NativeBuildCancellation>();
    capturedFrames_.clear();
    framesNeeded_ = recipe->temporal.has_value() ? 2 : 1;
    frameIndexRequested_ = -1;
    capturePending_ = false;
    contextPending_ = false;
    jobContext_.reset();
    prepareInFlight_ = true;
    ++prepareGeneration_;
    if (prepareProgress_ != nullptr) prepareProgress_->clear();
    updatePrepareControls();
    // Announced the moment the request is accepted, not when the first stage
    // reports: resolving the plan and probing the core happen before any build
    // event, and a listener that only opened on the first event would show
    // nothing for that whole stretch.
    emit preparationStarted();

    // The context the core already observed, or - only now, because the user
    // actually clicked Prepare - an explicit request that arms the live core's own
    // native probe. Opening the settings page never arms anything.
    const std::optional<NativePreparationContext> observed = currentContext();
    if (observed.has_value()) {
        jobContext_ = observed;
        requestNextPreparationFrame();
    } else if (contextRequest_) {
        contextPending_ = true;
        if (prepareProgress_ != nullptr) {
            prepareProgress_->setText(
                tr("Probing the live source for its geometry and device…"));
        }
        contextRequest_(prepareGeneration_);
    } else {
        auto result = std::make_shared<PrepareResult>();
        result->reasonKey = QStringLiteral("native.prepare.context");
        result->reason = tr("No native probe path is wired, so the live source's geometry and "
                            "device cannot be observed.");
        finishPrepare(result, prepareGeneration_);
    }
    return true;
#else
    prepareStatusKey_ = QStringLiteral("native.prepare.inspector");
    prepareStatusReason_ = tr("This build was made without the ONNX Runtime inspector.");
    updatePrepareControls();
    return false;
#endif
}

void NativeAssetsPanel::submitPreparationJob(const QVector<mplayer::native::FrameFixture>& frames,
                                             quint64 generation) {
    if (generation != prepareGeneration_ || !prepareInFlight_ || !jobContext_.has_value()) return;
#ifdef MIKAI_WITH_MODEL_INSPECTION
    const auto cancellation = prepareCancel_;
    auto result = std::make_shared<PrepareResult>();
    result->scratch = std::make_unique<QTemporaryDir>();
    if (!result->scratch->isValid()) {
        prepareCancel_.reset();
        result->reasonKey = QStringLiteral("native.prepare.scratch");
        result->reason = tr("No scratch directory could be created for the reference lineage.");
        finishPrepare(result, generation);
        return;
    }
    const QString recipeId = requestedRecipeId_;
    const QString modelRoot = modelRoot_;
    const InspectionRuntime runtime = runtime_;
    const ArtifactStore store = artifacts_;
    const NativePreparationContext ctx = *jobContext_;
    const NativePreparationToolchain tools = observedToolchain_;
    // The worker's only route back to this widget: a binder built here, on the
    // GUI thread, plus this widget's own alive flag. Neither is a QPointer, so
    // neither is copied off-thread.
    const Runner::GuiDispatch post = runner_->post();
    const std::shared_ptr<std::atomic_bool> alive = alive_;

    runner_->run(
        [recipeId, modelRoot, runtime, store, ctx, tools, frames, cancellation, result, post,
         alive, generation]() -> QVariant {
            if (cancellation->isCancelled()) {
                result->cancelled = true;
                result->reasonKey = QStringLiteral("native.prepare.cancelled");
                result->reason = QCoreApplication::translate(
                    "NativeAssetsPanel", "The preparation was cancelled before it started.");
                return QVariant();
            }
            UserRecipeStore recipes;
            QString recipeKey;
            QString recipeReason;
            const std::optional<Recipe> recipe =
                recipes.find(recipeId, &recipeKey, &recipeReason);
            if (!recipe.has_value()) {
                result->reasonKey = recipeKey.isEmpty() ? QStringLiteral("native.prepare.recipe")
                                                        : recipeKey;
                result->reason = recipeReason;
                return QVariant();
            }
            // The machine identity is measured here, off the GUI, by the
            // toolchain's own describe mode. Nothing is copied from a cache and
            // nothing is defaulted: an unreadable or incomplete document refuses
            // the whole job rather than building for a machine nobody measured.
            QString describeDocument;
            QString identityKey;
            QString identityReason;
            // The helper is CUDA-only, so the compute UUID is named exactly when it
            // is a CUDA device UUID; a DirectML adapter identity reaches the plan
            // through the context and never through a CUDA describe argument.
            const QString describeDeviceUuid =
                isCudaDeviceUuid(ctx.device.computeDeviceUuid) ? ctx.device.computeDeviceUuid
                                                               : QString();
            if (!runDescribeDevice(tools.tools, describeDeviceUuid, &describeDocument, &identityKey,
                                   &identityReason)) {
                result->reasonKey = identityKey;
                result->reason = identityReason;
                return QVariant();
            }
            QJsonParseError describeError{};
            const QJsonDocument describe =
                QJsonDocument::fromJson(describeDocument.toUtf8(), &describeError);
            if (describeError.error != QJsonParseError::NoError || !describe.isObject()) {
                result->reasonKey = QStringLiteral("native.prepare.identityDocument");
                result->reason = QCoreApplication::translate(
                                     "NativeAssetsPanel",
                                     "The preparation helper's describe document is not JSON: %1")
                                     .arg(describeError.errorString());
                return QVariant();
            }
            EngineBuildIdentity identity;
            const QString pluginRevision = recipe->backend == Backend::DirectML
                                               ? tools.pluginShaDirectMl
                                               : tools.pluginShaTensorRt;
            if (!identityFromDescribe(describe.object(), recipe->backend, tools.coreSha,
                                      tools.sourceAbi, pluginRevision, ctx.device, &identity,
                                      &identityKey, &identityReason)) {
                result->reasonKey = identityKey;
                result->reason = identityReason;
                return QVariant();
            }

            InspectedPlanResolver resolver(modelRoot, runtime);
            QString storeKey;
            QString storeReason;
            if (!resolver.refresh(&storeKey, &storeReason)) {
                result->reasonKey = storeKey;
                result->reason = storeReason;
                return QVariant();
            }
            PlanRequest planRequest;
            planRequest.recipe = *recipe;
            planRequest.catalog = resolver.catalog();
            planRequest.backends = tools.backends;
            planRequest.source = ctx.source;
            planRequest.device = ctx.device;
            planRequest.window = ChainWindow();
            planRequest.dynamicResolver = resolver.resolver();

            const PreparationPlan plan = resolvePreparationPlan(planRequest);
            if (!plan.blockers.isEmpty()) {
                result->reasonKey = plan.blockers.first().reasonKey;
                result->reason = plan.blockers.first().reason;
                result->stageIndex = plan.blockers.first().stageIndex;
                return QVariant();
            }
            if (!plan.isResolved()) {
                result->reasonKey = QStringLiteral("plan.preparing");
                result->reason = plan.unknowns.isEmpty()
                                     ? QCoreApplication::translate("NativeAssetsPanel",
                                                                   "The plan is not resolved.")
                                     : plan.unknowns.first();
                return QVariant();
            }

            // One owned row per stage, in stage order, from the store the plan
            // was resolved against.
            const QVector<StoredModel> rows = resolver.rows();
            QVector<StoredModel> models;
            for (int index = 0; index < plan.stages.size(); ++index) {
                const ResolvedStage& stage = plan.stages.at(index);
                const StoredModel* found = nullptr;
                for (const StoredModel& row : rows) {
                    if (row.record.id == stage.modelId) {
                        found = &row;
                        break;
                    }
                }
                if (found == nullptr) {
                    result->reasonKey = QStringLiteral("model.owned.missing");
                    result->reason = QCoreApplication::translate(
                                         "NativeAssetsPanel", "The owned store holds no %1.")
                                         .arg(stage.modelId);
                    result->stageIndex = index;
                    return QVariant();
                }
                models.append(*found);
            }

            // The progress sink is shared by the lineage builder and the build.
            // The plan's own stage count travels with every event, so a listener
            // can say "stage 2 of 3" without a percentage nobody measured.
            const int stageCount = static_cast<int>(plan.stages.size());
            // The binder is the worker's only route back: it posts, so nothing
            // here touches a widget and nothing reads this panel off-thread.
            const BuilderProgressCallback progress =
                [post, alive, generation, stageCount](const BuilderProgress& event) {
                    const int stageIndex = event.stageIndex;
                    const QString phase = event.phase;
                    const QString detail = event.detail;
                    post([alive, generation, stageIndex, stageCount, phase, detail] {
                        if (!alive->load(std::memory_order_relaxed)) return;
                        NativeAssetsPanel* target = livePanel();
                        if (target == nullptr || target->prepareGeneration_ != generation) return;
                        target->applyPrepareProgress(stageIndex, stageCount, phase, detail);
                    });
                };

            // The whole-plan lineage: each stage's real CPU output becomes the
            // next stage's input, and an interpolation stage's eleven-channel
            // input is built from the actual frame pair. The frames are the ones
            // the live core actually captured, never a stretched copy.
            PlanReferenceRequest lineage;
            lineage.plan = plan;
            lineage.source = ctx.source;
            lineage.frames.reserve(frames.size());
            for (int index = 0; index < frames.size(); ++index) {
                ReferenceFrame frame;
                frame.fixture = frames.at(index);
                frame.unprocessed = true;
                frame.sourceIndex = index;
                lineage.frames.append(frame);
            }
            lineage.models = models;
            lineage.tools = tools.tools;
            lineage.scratchDirectory = result->scratch->path();
            lineage.cancellation = cancellation.get();
            lineage.progress = progress;
            QVector<BuilderStageReference> references;
            QString referenceKey;
            QString referenceReason;
            if (!buildPlanReferenceLineage(lineage, &references, &referenceKey, &referenceReason)) {
                result->reasonKey = referenceKey.isEmpty()
                                        ? QStringLiteral("native.prepare.reference")
                                        : referenceKey;
                result->reason = referenceReason;
                return QVariant();
            }
            if (references.size() != plan.stages.size()) {
                result->reasonKey = QStringLiteral("native.prepare.reference");
                result->reason = QCoreApplication::translate(
                    "NativeAssetsPanel",
                    "The reference builder supplied %1 reference(s) for a %2-stage plan.")
                    .arg(references.size())
                    .arg(plan.stages.size());
                return QVariant();
            }

            BuilderRequest request;
            request.plan = plan;
            request.models = models;
            request.build = identity;
            request.tools = tools.tools;
            request.references = references;
            request.store = store;
            request.modelRoot = modelRoot;
            request.workspaceBytes = 0;

            const BuilderResult built = runNativeBuild(request, cancellation.get(), progress);
            result->ok = built.ok;
            result->cancelled = built.cancelled;
            result->reasonKey = built.reasonKey;
            result->reason = built.reason;
            result->stageIndex = built.stageIndex;
            result->phase = built.phase;
            result->toolError = built.toolError;
            result->publishedBuildKeys = built.publishedBuildKeys;
            return QVariant();
        },
        [alive, result, generation](const QVariant&) {
            NativeAssetsPanel* target = livePanel();
            if (target == nullptr || !alive->load(std::memory_order_relaxed)) return;
            target->finishPrepare(result, generation);
        },
        [alive, result, generation](const backend::Failure& failure) {
            NativeAssetsPanel* target = livePanel();
            if (target == nullptr || !alive->load(std::memory_order_relaxed)) return;
            result->reasonKey = QStringLiteral("native.prepare.failed");
            result->reason = backend::failureLine(failure);
            target->finishPrepare(result, generation);
        });
#else
    auto result = std::make_shared<PrepareResult>();
    result->reasonKey = QStringLiteral("native.prepare.inspector");
    result->reason = tr("This build was made without the ONNX Runtime inspector.");
    finishPrepare(result, generation);
#endif
}

void NativeAssetsPanel::cancelPrepare() {
    if (!prepareInFlight_ || !prepareCancel_) return;
    const NativeBuildCancellation::CancelResult result = prepareCancel_->cancel();
    if (result == NativeBuildCancellation::CancelResult::TooLate) {
        // The worker already passed the publication gate. Nothing can stop the
        // write, so the job is not reported as cancelled and its committed
        // outcome is still delivered by the completion.
        prepareStatusKey_ = QStringLiteral("native.prepare.commitInProgress");
        prepareStatusReason_ =
            tr("The preparation has already begun publishing, so it can no longer be cancelled; "
               "its committed outcome will be reported.");
        preparationReason_->setText(prepareStatusReason_);
        return;
    }
    prepareStatusKey_ = QStringLiteral("native.prepare.cancelling");
    prepareStatusReason_ =
        tr("Cancelling the preparation; the builder decides at its next check.");
    preparationReason_->setText(prepareStatusReason_);
    updatePrepareControls();
}

void NativeAssetsPanel::applyPrepareProgress(int stageIndex, int stageCount, const QString& phase,
                                             const QString& detail) {
    if (!prepareInFlight_ || prepareProgress_ == nullptr) return;
    QString text = tr("Preparing stage %1: %2").arg(stageIndex).arg(phase);
    if (!detail.isEmpty()) text += QStringLiteral(" — ") + detail;
    prepareProgress_->setText(text);
    emit preparationProgress(stageIndex, stageCount, phase, detail);
}

void NativeAssetsPanel::finishPrepare(const std::shared_ptr<PrepareResult>& result,
                                      quint64 generation) {
    if (generation != prepareGeneration_) return;
    prepareInFlight_ = false;
    prepareCancel_.reset();
    contextPending_ = false;
    jobContext_.reset();
    capturePending_ = false;
    frameIndexRequested_ = -1;
    capturedFrames_.clear();

    if (result->ok) {
        prepareStatusKey_.clear();
        prepareStatusReason_.clear();
        prepareProgress_->setText(
            result->publishedBuildKeys.isEmpty()
                ? tr("Every stage was already prepared and verified in the owned artifact cache.")
                : tr("Published %1 stage(s) into the owned artifact cache.")
                      .arg(result->publishedBuildKeys.size()));
    } else if (result->cancelled) {
        prepareStatusKey_ = QStringLiteral("native.prepare.cancelled");
        prepareStatusReason_ = tr("The preparation was cancelled; nothing was published.");
        prepareProgress_->clear();
    } else {
        prepareStatusKey_ = result->reasonKey.isEmpty() ? QStringLiteral("native.prepare.failed")
                                                        : result->reasonKey;
        prepareStatusReason_ =
            result->reason.isEmpty() ? tr("The preparation failed; nothing was published.")
                                     : result->reason;
        if (!result->toolError.isEmpty()) {
            prepareStatusReason_ += QStringLiteral("\n") + result->toolError;
        }
        prepareProgress_->clear();
    }

    // A completed outcome is the only thing that refreshes the owned cache: a
    // failure or cancellation leaves what is really there in view, staging
    // included.
    refresh();
    emit preparationFinished(result->ok);
}

bool NativeAssetsPanel::inspectorBuilds() const {
#ifdef MIKAI_WITH_MODEL_INSPECTION
    return true;
#else
    return false;
#endif
}

}  // namespace mikai::app
