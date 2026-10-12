#include "state/NativePlaybackAssembly.hpp"

#include <atomic>
#include <memory>
#include <optional>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtCore/QVector>

#include <PlayerDialog.hpp>

#include "dialogs/NativeAssetsPanel.hpp"
#include "native/prepare/NativeArtifactStore.hpp"
#include "native/bundle/NativeBundle.hpp"
#include "native/bundle/NativeManifest.hpp"
#include "native/models/NativeModelInspect.hpp"
#include "native/models/NativeModelInventory.hpp"
#include "native/prepare/NativePreparationPlan.hpp"
#include "native/models/NativeProfiles.hpp"
#include "native/models/NativeUserRecipes.hpp"
#include "workers/Runner.hpp"

namespace mikai::app {

namespace {

using mplayer::native::ArtifactStore;
using mplayer::native::Backend;
using mplayer::native::BuildOption;
using mplayer::native::Bundle;
using mplayer::native::BundleKind;
using mplayer::native::CatalogView;
using mplayer::native::ChainWindow;
using mplayer::native::DeviceIdentity;
using mplayer::native::DynamicProfileResolver;
using mplayer::native::EngineBuildIdentity;
using mplayer::native::EnginePrecision;
using mplayer::native::ModelInventory;
using mplayer::native::ModelRecord;
using mplayer::native::PlanRequest;
using mplayer::native::PreparationPlan;
using mplayer::native::PreparedAdmission;
using mplayer::native::PreparedAdmissionRequest;
using mplayer::native::Rational;
using mplayer::native::Recipe;
using mplayer::native::ResolvedProfile;
using mplayer::native::ResolvedStage;
using mplayer::native::StageResolution;
using mplayer::native::StoredModel;
using mplayer::native::UserRecipeStore;

NativeAssemblyOutcome refuse(const QString& reasonKey, const QString& reason) {
  NativeAssemblyOutcome outcome;
  outcome.reasonKey = reasonKey;
  outcome.reason = reason;
  return outcome;
}

QString assemblyText(const char* source) {
  return QCoreApplication::translate("NativePlaybackAssembly", source);
}

bool samePath(const QString& left, const QString& right) {
  const QFileInfo leftInfo(left);
  const QFileInfo rightInfo(right);
  const QString a =
      leftInfo.canonicalFilePath().isEmpty() ? QDir::cleanPath(leftInfo.absoluteFilePath())
                                             : leftInfo.canonicalFilePath();
  const QString b =
      rightInfo.canonicalFilePath().isEmpty() ? QDir::cleanPath(rightInfo.absoluteFilePath())
                                              : rightInfo.canonicalFilePath();
  if (a.isEmpty() || b.isEmpty()) return false;
#ifdef Q_OS_WIN
  return QString::compare(a, b, Qt::CaseInsensitive) == 0;
#else
  return a == b;
#endif
}

/// TensorRT reports its library version as a single integer,
/// `major*10000 + minor*100 + patch`. The builder records the dotted form, so
/// this is the one rendering every consumer must agree on.
QString renderTensorRtVersion(qint64 version) {
  if (version <= 0) return QString();
  return QStringLiteral("%1.%2.%3")
      .arg(version / 10000)
      .arg((version / 100) % 100)
      .arg(version % 100);
}

/// The CUDA runtime/driver API version, `major*1000 + minor*10`.
QString renderCudaVersion(qint64 version) {
  if (version <= 0) return QString();
  return QStringLiteral("%1.%2").arg(version / 1000).arg((version / 10) % 100);
}

/// "12.0" -> 12, 0. Two components rather than one joined number, because 12.0
/// and 1.20 are not the same revision.
bool parseComputeCapability(const QString& text, int* major, int* minor) {
  const int dot = text.indexOf(QLatin1Char('.'));
  if (dot <= 0 || dot == text.size() - 1) return false;
  bool majorOk = false;
  bool minorOk = false;
  const int majorValue = text.left(dot).toInt(&majorOk);
  const int minorValue = text.mid(dot + 1).toInt(&minorOk);
  if (!majorOk || !minorOk || majorValue <= 0 || minorValue < 0) return false;
  *major = majorValue;
  *minor = minorValue;
  return true;
}

// -------------------------------------------------------- core provenance

/// What the validated bundle states about the inference core and the actual
/// native backend it ships: the compiled inference source SHA and the source ABI
/// it exposes, plus the backend module's own content revision.
///
/// None of this is a device or runtime observation, and none of it is a display
/// version: the SHA and the ABI are the bundle's validated statement about what
/// it contains, and the revision is the recorded digest of the very backend
/// bytes the bundle ships for this provider. A bundle without the preparation
/// block is a playback-only bundle, and preparation is then named unavailable
/// rather than completed with a plausible value.
struct CoreProvenance {
  QString coreSha;
  int sourceAbi = 0;
  /// "sha256:<hex>" of the backend module this provider runs on. An observed
  /// content identity, never a marketing version string.
  QString pluginVersion;
};

/// The validated bundle the running module came from. The module is inside its
/// own bundle directory, so the nearest ancestor holding a `bundle.json` that
/// names exactly this module is the one the toolchain validated.
std::optional<Bundle> bundleOf(const QString& modulePath, QString* reasonKey, QString* reason) {
  QDir directory = QFileInfo(modulePath).absoluteDir();
  for (int depth = 0; depth < 4; ++depth) {
    if (QFileInfo::exists(directory.filePath(QStringLiteral("bundle.json")))) {
      QString readReason;
      const std::optional<Bundle> bundle = mplayer::native::readBundle(directory.absolutePath(),
                                                                      &readReason);
      if (bundle.has_value() && samePath(bundle->modulePath, modulePath)) return bundle;
    }
    if (!directory.cdUp()) break;
  }
  if (reasonKey != nullptr) *reasonKey = QStringLiteral("native.assembly.bundle");
  if (reason != nullptr) {
    *reason = assemblyText("The module the owning core runs on (%1) is not the module of a "
                           "validated native bundle, so its provenance cannot be read.")
                  .arg(modulePath);
  }
  return std::nullopt;
}

/// The bundle's recorded digest of the backend module this provider runs on. The
/// bundle shipped and verified those bytes, so the digest identifies the plugin
/// that will actually load - a patched shim included - where no version resource
/// exists to query.
QString backendContentRevision(const Bundle& bundle, Backend provider) {
  const QString module =
      mplayer::native::bundlePreparationBackendLibrary(mplayer::native::stableName(provider));
  if (module.isEmpty()) return QString();
  for (const auto& file : bundle.files) {
    if (QFileInfo(file.path).fileName().compare(module, Qt::CaseInsensitive) != 0) continue;
    const QString digest = file.sha256.trimmed().toLower();
    if (digest.size() != 64) continue;
    bool hex = true;
    for (const QChar character : digest) {
      const bool digit = character >= QLatin1Char('0') && character <= QLatin1Char('9');
      const bool lower = character >= QLatin1Char('a') && character <= QLatin1Char('f');
      if (!digit && !lower) {
        hex = false;
        break;
      }
    }
    if (hex) return QStringLiteral("sha256:%1").arg(digest);
  }
  return QString();
}

/// The core provenance, read from the bundle's own validated preparation block.
/// A bundle that states none, or whose backend module is not in its verified
/// inventory, refuses by name.
std::optional<CoreProvenance> bundleCoreProvenance(const Bundle& bundle, Backend provider,
                                                   QString* reasonKey, QString* reason) {
  const auto refuseKey = [reasonKey, reason](const QString& key, const QString& text) {
    if (reasonKey != nullptr) *reasonKey = key;
    if (reason != nullptr) *reason = text;
    return false;
  };
  if (!bundle.preparation.has_value()) {
    refuseKey(QStringLiteral("native.assembly.coreProvenance"),
              assemblyText("The validated native bundle states no preparation provenance, so the "
                           "inference core's source SHA and source ABI are not observed."));
    return std::nullopt;
  }
  CoreProvenance provenance;
  provenance.coreSha = bundle.preparation->coreSha.trimmed().toLower();
  provenance.sourceAbi = bundle.preparation->sourceAbi;
  if (provenance.coreSha.size() != 40 || provenance.sourceAbi <= 0) {
    refuseKey(QStringLiteral("native.assembly.coreProvenance"),
              assemblyText("The validated native bundle's preparation provenance records no "
                           "usable core SHA or source ABI."));
    return std::nullopt;
  }
  provenance.pluginVersion = backendContentRevision(bundle, provider);
  if (provenance.pluginVersion.isEmpty()) {
    refuseKey(QStringLiteral("native.assembly.pluginRevision"),
              assemblyText("The validated native bundle records no verified backend module for "
                           "this provider, so the plugin's content revision is not observed."));
    return std::nullopt;
  }
  if (reasonKey != nullptr) reasonKey->clear();
  if (reason != nullptr) reason->clear();
  return provenance;
}

// ------------------------------------------------------ measured identity

QString describeDeviceSchema() { return QStringLiteral("mikai.native.preparation.environment/1"); }

/// The child environment the helper needs: its own directory first, then the
/// provider runtime directories, then the system directories. The ambient PATH
/// is deliberately not inherited, so the helper loads the runtime it was
/// measured against rather than whatever happens to be reachable.
QProcessEnvironment describeChildEnvironment(const mplayer::native::BuilderToolPaths& tools) {
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

/// Runs `mikai_native_prep --describe-device --json --device-uuid <uuid>`, the
/// only non-building observation of this machine's device, runtime and builder
/// policy. The compute device is named explicitly: a helper that picked a
/// default adapter would measure a machine this process is not decoding on.
bool runDescribeDevice(const mplayer::native::BuilderToolPaths& tools, const QString& deviceUuid,
                       QString* document, QString* reasonKey, QString* reason) {
  const auto fail = [reasonKey, reason](const QString& key, const QString& text) {
    if (reasonKey != nullptr) *reasonKey = key;
    if (reason != nullptr) *reason = text;
    return false;
  };
  if (tools.prepExecutable.isEmpty()) {
    return fail(QStringLiteral("native.prepare.identityUnobserved"),
                assemblyText("No preparation helper is present, so this machine's build identity "
                             "cannot be measured."));
  }
  QProcess process;
  process.setProcessEnvironment(describeChildEnvironment(tools));
  process.setProgram(tools.prepExecutable);
  process.setArguments({QStringLiteral("--describe-device"), QStringLiteral("--json"),
                        QStringLiteral("--device-uuid"), deviceUuid});
  process.start();
  if (!process.waitForStarted(10000)) {
    return fail(QStringLiteral("native.prepare.identityLaunch"),
                assemblyText("The preparation helper could not be started: %1")
                    .arg(process.errorString()));
  }
  if (!process.waitForFinished(60000)) {
    process.kill();
    process.waitForFinished(5000);
    return fail(QStringLiteral("native.prepare.identityTimeout"),
                assemblyText("The preparation helper did not report this machine's build identity "
                             "within its deadline."));
  }
  const QByteArray output = process.readAllStandardOutput();
  const QByteArray errors = process.readAllStandardError();
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
      output.trimmed().isEmpty()) {
    const QString detail = QString::fromUtf8(errors).trimmed();
    return fail(QStringLiteral("native.prepare.identityUnobserved"),
                detail.isEmpty()
                    ? assemblyText("The preparation helper refused to describe this machine (exit "
                                   "%1).")
                          .arg(process.exitCode())
                    : detail);
  }
  if (document != nullptr) *document = QString::fromUtf8(output).trimmed();
  if (reasonKey != nullptr) reasonKey->clear();
  if (reason != nullptr) reason->clear();
  return true;
}

/// The canonical option set the builder derives from the flags the helper
/// measured. A different set would produce a build key for a configuration the
/// helper never used, so this matches the builder's own derivation exactly.
QVector<BuildOption> measuredBuildOptions(qint64 builderFlags, qint64 tacticSources,
                                          qint64 networkFlags, qint64 builderOptimizationLevel,
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
  options.append(option(QStringLiteral("builderOptimizationLevel"),
                        QString::number(builderOptimizationLevel)));
  options.append(option(QStringLiteral("engineCapability"), engineCapability));
  return options;
}

/// Parses the describe document into the measured build identity, verifying it
/// against the context this process observed and the provenance the validated
/// bundle ships. Strict: the schema, the device, the runtime, the policy, the
/// core provenance and the measured build flags must all be present and well
/// typed, and the measured device must be the observed one, or the whole
/// document is refused by name.
bool identityFromDescribe(const QJsonObject& document, Backend provider,
                          const DeviceIdentity& device, const CoreProvenance& core,
                          const QString& pluginVersion, EngineBuildIdentity* out,
                          QString* reasonKey, QString* reason) {
  const auto refuseKey = [reasonKey, reason](const QString& key, const QString& text) {
    if (reasonKey != nullptr) *reasonKey = key;
    if (reason != nullptr) *reason = text;
    return false;
  };
  const auto stringMember = [](const QJsonObject& object, const QString& key, QString* value,
                               QString* missing) {
    const QJsonValue found = object.value(key);
    if (!found.isString()) {
      *missing = key;
      return false;
    }
    *value = found.toString();
    return true;
  };
  const auto numberMember = [](const QJsonObject& object, const QString& key, qint64* value,
                               QString* missing) {
    const QJsonValue found = object.value(key);
    if (!found.isDouble()) {
      *missing = key;
      return false;
    }
    *value = static_cast<qint64>(found.toDouble());
    return true;
  };
  const auto boolMember = [](const QJsonObject& object, const QString& key, bool* value,
                             QString* missing) {
    const QJsonValue found = object.value(key);
    if (!found.isBool()) {
      *missing = key;
      return false;
    }
    *value = found.toBool();
    return true;
  };

  const QJsonValue schema = document.value(QStringLiteral("schema"));
  if (!schema.isString() || schema.toString() != describeDeviceSchema()) {
    return refuseKey(QStringLiteral("native.assembly.identitySchema"),
                     assemblyText("The preparation helper's describe document is not the %1 "
                                  "schema this build reads.")
                         .arg(describeDeviceSchema()));
  }

  // The document must be the helper's own answered observation, not an object
  // that merely carries the key names: the tool, its protocol and both verdict
  // flags are required before any measurement is believed.
  QString missing;
  QString tool;
  QString deviceUuid;
  QString computeCapability;
  QString driverVersion;
  QString engineCapability;
  qint64 protocol = 0;
  qint64 tensorRtLibVersion = 0;
  qint64 cudaRuntimeVersion = 0;
  qint64 builderFlags = 0;
  qint64 tacticSources = 0;
  qint64 networkFlags = 0;
  qint64 builderOptimizationLevel = 0;
  bool described = false;
  bool ok = false;
  bool stronglyTyped = false;
  bool tf32 = false;
  if (!stringMember(document, QStringLiteral("tool"), &tool, &missing) ||
      tool != QLatin1String("mikai_native_prep") ||
      !numberMember(document, QStringLiteral("protocol"), &protocol, &missing) || protocol != 1 ||
      !boolMember(document, QStringLiteral("described"), &described, &missing) || !described ||
      !boolMember(document, QStringLiteral("ok"), &ok, &missing) || !ok ||
      !stringMember(document, QStringLiteral("deviceUuid"), &deviceUuid, &missing) ||
      !stringMember(document, QStringLiteral("computeCapability"), &computeCapability, &missing) ||
      !stringMember(document, QStringLiteral("cudaDriverFileVersion"), &driverVersion, &missing) ||
      !numberMember(document, QStringLiteral("tensorRtLibVersion"), &tensorRtLibVersion, &missing) ||
      !numberMember(document, QStringLiteral("cudaRuntimeVersion"), &cudaRuntimeVersion, &missing) ||
      !boolMember(document, QStringLiteral("tf32Allowed"), &tf32, &missing) ||
      !boolMember(document, QStringLiteral("stronglyTyped"), &stronglyTyped, &missing) ||
      !numberMember(document, QStringLiteral("builderFlags"), &builderFlags, &missing) ||
      !numberMember(document, QStringLiteral("tacticSources"), &tacticSources, &missing) ||
      !numberMember(document, QStringLiteral("networkFlags"), &networkFlags, &missing) ||
      !numberMember(document, QStringLiteral("builderOptimizationLevel"), &builderOptimizationLevel,
                    &missing) ||
      !stringMember(document, QStringLiteral("engineCapability"), &engineCapability, &missing)) {
    return refuseKey(QStringLiteral("native.assembly.identityIncomplete"),
                     assemblyText("The describe document has no usable %1, so this machine's "
                                  "identity is not fully measured.")
                         .arg(missing));
  }

  const QJsonValue policyValue = document.value(QStringLiteral("policy"));
  if (!policyValue.isObject()) {
    return refuseKey(QStringLiteral("native.assembly.identityIncomplete"),
                     assemblyText("The describe document names no builder policy section."));
  }
  const QJsonObject policy = policyValue.toObject();
  QString precisionText;
  bool skipInference = false;
  if (!stringMember(policy, QStringLiteral("precision"), &precisionText, &missing) ||
      !boolMember(policy, QStringLiteral("skipInference"), &skipInference, &missing)) {
    return refuseKey(QStringLiteral("native.assembly.identityIncomplete"),
                     assemblyText("The describe document's builder policy records no %1.")
                         .arg(missing));
  }
  const std::optional<EnginePrecision> precision =
      mplayer::native::enginePrecisionFromName(precisionText);
  if (!precision.has_value()) {
    return refuseKey(QStringLiteral("native.assembly.identityIncomplete"),
                     assemblyText("The describe document names an unknown build precision."));
  }
  int smMajor = 0;
  int smMinor = 0;
  if (!parseComputeCapability(computeCapability, &smMajor, &smMinor)) {
    return refuseKey(QStringLiteral("native.assembly.identityIncomplete"),
                     assemblyText("The describe document's compute capability \"%1\" is not a "
                                  "major.minor revision.")
                         .arg(computeCapability));
  }
  const QString runtimeVersion = renderTensorRtVersion(tensorRtLibVersion);
  const QString cudaVersion = renderCudaVersion(cudaRuntimeVersion);
  if (runtimeVersion.isEmpty() || cudaVersion.isEmpty()) {
    return refuseKey(QStringLiteral("native.assembly.identityIncomplete"),
                     assemblyText("The describe document records no usable TensorRT or CUDA "
                                  "version."));
  }

  // The measured device must be the adapter this process is decoding on. A
  // describe document that selected another one would name a machine no engine
  // here could load on.
  if (!mplayer::native::isSameDeviceUuid(deviceUuid, device.computeDeviceUuid) ||
      smMajor != device.smMajor || smMinor != device.smMinor) {
    return refuseKey(QStringLiteral("native.assembly.deviceMismatch"),
                     assemblyText("The measured device (%1, SM %2.%3) is not the compute device "
                                  "this core observed (%4, SM %5.%6).")
                         .arg(deviceUuid)
                         .arg(smMajor)
                         .arg(smMinor)
                         .arg(device.computeDeviceUuid)
                         .arg(device.smMajor)
                         .arg(device.smMinor));
  }

  EngineBuildIdentity identity;
  identity.provider = provider;
  identity.runtimeVersion = runtimeVersion;
  // The plugin's own identity is the bundle's verified backend-content revision,
  // not a value this process can query from the loaded runtime.
  identity.pluginVersion = pluginVersion;
  identity.coreSha = core.coreSha;
  identity.sourceAbi = core.sourceAbi;
  identity.precision = *precision;
  identity.tf32 = tf32;
  identity.stronglyTyped = stronglyTyped;
  identity.skipInference = skipInference;
  identity.deviceUuid = deviceUuid;
  identity.smMajor = smMajor;
  identity.smMinor = smMinor;
  identity.cudaVersion = cudaVersion;
  identity.driverVersion = driverVersion;
  identity.options = measuredBuildOptions(builderFlags, tacticSources, networkFlags,
                                          builderOptimizationLevel, engineCapability);

  const QString incomplete = identity.firstMissingRequiredField();
  if (!incomplete.isEmpty()) {
    return refuseKey(QStringLiteral("native.assembly.identityIncomplete"),
                     assemblyText("The measured build identity records no %1.").arg(incomplete));
  }
  const QString sensitive = identity.firstSensitiveField();
  if (!sensitive.isEmpty()) {
    return refuseKey(QStringLiteral("native.assembly.identitySensitive"),
                     assemblyText("The measured build identity's %1 looks like a URL or a "
                                  "credential, which is never a real runtime or device.")
                         .arg(sensitive));
  }
  *out = identity;
  if (reasonKey != nullptr) reasonKey->clear();
  if (reason != nullptr) reason->clear();
  return true;
}

// ------------------------------------------------------------- the plan

/// The requested recipe, resolved against the observed source. A curated profile
/// is resolved from the catalogue by its window; anything else must be a stored
/// recipe document. Neither path substitutes another chain.
std::optional<Recipe> resolveRequestedRecipe(const mplayer::native::SelectionState& selection,
                                             const QSize& sourceSize, const Rational& sourceRate,
                                             const CatalogView& catalog,
                                             const UserRecipeStore& recipes, QString* reasonKey,
                                             QString* reason) {
  for (const mplayer::native::ProfileTemplate& profile : mplayer::native::profileTemplates()) {
    if (profile.id != selection.recipeId) continue;
    return mplayer::native::resolveProfile(selection.recipeId, sourceSize, sourceRate,
                                           selection.backend, catalog, reasonKey, reason);
  }
  return recipes.find(selection.recipeId, reasonKey, reason);
}

/// The only dynamic-stage resolver this path may use: the resolution an owned row
/// already recorded at import. Nothing is measured now - a stage geometry with no
/// matching recorded resolution is left unresolved for the planner to refuse by
/// name, because resolving it here would mean running a CPU reference at
/// playback time.
DynamicProfileResolver recordedResolutionResolver(const QVector<StoredModel>& rows) {
  return [rows](const ModelRecord& record,
                const QSize& stageInput) -> std::optional<ResolvedProfile> {
    for (const StoredModel& row : rows) {
      if (row.record.id != record.id) continue;
      if (!row.importResolution.has_value()) return std::nullopt;
      const StageResolution& resolution = *row.importResolution;
      if (resolution.input != stageInput || resolution.output.isEmpty()) return std::nullopt;
      ResolvedProfile profile;
      profile.input = resolution.input;
      profile.output = resolution.output;
      profile.inputChannels = 3;
      profile.outputChannels = 3;
      return profile;
    }
    return std::nullopt;
  };
}

}  // namespace

NativeAssemblyOutcome assembleNativeAdmission(const NativeAssemblyRequest& input) {
  if (input.selection.recipeId.isEmpty()) {
    return refuse(QStringLiteral("native.assembly.selection"),
                  assemblyText("No native recipe is requested, so there is nothing to prepare."));
  }
  if (!input.context.source.isKnown()) {
    return refuse(QStringLiteral("native.assembly.sourceUnobserved"),
                  assemblyText("The live source has not reported its display geometry and cadence "
                               "yet."));
  }
  if (!input.context.device.isKnown()) {
    return refuse(QStringLiteral("native.assembly.deviceUnobserved"),
                  assemblyText("The live core has not reported its decoder and compute device "
                               "yet."));
  }
  if (input.context.modulePath.isEmpty()) {
    return refuse(QStringLiteral("native.assembly.moduleUnobserved"),
                  assemblyText("The live core has not reported the module it was loaded from."));
  }
  const std::optional<QSize> displaySize = input.context.source.displaySize();
  const std::optional<Rational> sourceRate = input.context.source.fps;
  if (!displaySize.has_value() || !sourceRate.has_value()) {
    return refuse(QStringLiteral("native.assembly.sourceUnobserved"),
                  assemblyText("The live source has not reported its display geometry and cadence "
                               "yet."));
  }

  // The validated selected runtime and its tool closure, observed from the real
  // locations this build owns. An absent toolchain is named, never assumed.
  const NativePreparationToolchain toolchain = observeNativePreparationToolchain();
  if (!toolchain.available) {
    return refuse(toolchain.reasonKey.isEmpty() ? QStringLiteral("native.prepare.toolsUnavailable")
                                                : toolchain.reasonKey,
                  toolchain.reason);
  }
  // The plan is built for the runtime the toolchain was validated against, so
  // the core that observed the request must be running on that same module.
  if (!toolchain.modulePath.isEmpty() && !samePath(toolchain.modulePath, input.context.modulePath)) {
    return refuse(QStringLiteral("native.assembly.moduleMismatch"),
                  assemblyText("The owning core runs on %1, but the validated native toolchain is "
                               "the one at %2.")
                      .arg(input.context.modulePath, toolchain.modulePath));
  }

  // The validated bundle the owning core's module came from. Its own preparation
  // block is the only place the inference core's source SHA and ABI are stated,
  // and its verified inventory is the only place the backend content revision is;
  // neither can be measured here and neither is defaulted.
  QString provenanceKey;
  QString provenanceReason;
  const std::optional<Bundle> bundle =
      bundleOf(input.context.modulePath, &provenanceKey, &provenanceReason);
  if (!bundle.has_value()) {
    return refuse(provenanceKey, provenanceReason);
  }

  // The app-owned model store, exactly as it vouches for its rows. An unowned
  // root is a refusal, not an empty inventory.
  const ModelInventory models;
  QString storeReason;
  const QVector<StoredModel> rows = models.stored(&storeReason);
  if (!storeReason.isEmpty()) {
    return refuse(QStringLiteral("native.assembly.modelStore"), storeReason);
  }
  CatalogView catalog;
  catalog.models.reserve(rows.size());
  for (const StoredModel& row : rows) catalog.models.append(row.record);

  QString recipeKey;
  QString recipeReason;
  const std::optional<Recipe> recipe =
      resolveRequestedRecipe(input.selection, *displaySize, *sourceRate, catalog,
                             UserRecipeStore(), &recipeKey, &recipeReason);
  if (!recipe.has_value()) {
    return refuse(recipeKey.isEmpty() ? QStringLiteral("native.assembly.recipe") : recipeKey,
                  recipeReason.isEmpty()
                      ? assemblyText("The requested recipe could not be resolved.")
                      : recipeReason);
  }

  // The core provenance and the plugin's content revision come from the validated
  // bundle; the device, runtime and builder policy are measured here, off the GUI,
  // by the toolchain's own describe mode. Nothing is copied from a cache and
  // nothing is defaulted.
  const std::optional<CoreProvenance> core =
      bundleCoreProvenance(*bundle, recipe->backend, &provenanceKey, &provenanceReason);
  if (!core.has_value()) {
    return refuse(provenanceKey, provenanceReason);
  }
  QString describeDocument;
  QString identityKey;
  QString identityReason;
  if (!runDescribeDevice(toolchain.tools, input.context.device.computeDeviceUuid,
                         &describeDocument, &identityKey, &identityReason)) {
    return refuse(identityKey, identityReason);
  }
  QJsonParseError describeError{};
  const QJsonDocument describe = QJsonDocument::fromJson(describeDocument.toUtf8(), &describeError);
  if (describeError.error != QJsonParseError::NoError || !describe.isObject()) {
    return refuse(QStringLiteral("native.assembly.identityDocument"),
                  assemblyText("The preparation helper's describe document is not JSON: %1")
                      .arg(describeError.errorString()));
  }
  EngineBuildIdentity identity;
  if (!identityFromDescribe(describe.object(), recipe->backend, input.context.device, *core,
                            core->pluginVersion, &identity, &identityKey, &identityReason)) {
    return refuse(identityKey, identityReason);
  }

  PlanRequest planRequest;
  planRequest.recipe = *recipe;
  planRequest.catalog = catalog;
  planRequest.backends = toolchain.backends;
  planRequest.source = input.context.source;
  planRequest.device = input.context.device;
  planRequest.window = ChainWindow();
  planRequest.dynamicResolver = recordedResolutionResolver(rows);

  const PreparationPlan plan = mplayer::native::resolvePreparationPlan(planRequest);
  if (!plan.blockers.isEmpty()) {
    return refuse(plan.blockers.first().reasonKey, plan.blockers.first().reason);
  }
  if (!plan.isResolved()) {
    return refuse(QStringLiteral("native.assembly.planUnresolved"),
                  plan.unknowns.isEmpty() ? assemblyText("The plan is not resolved.")
                                          : plan.unknowns.first());
  }

  // One owned row per stage, in stage order, from the store the plan was
  // resolved against. A stage whose row is gone refuses the whole plan.
  QVector<ModelRecord> stageModels;
  stageModels.reserve(plan.stages.size());
  for (const ResolvedStage& stage : plan.stages) {
    const StoredModel* found = nullptr;
    for (const StoredModel& row : rows) {
      if (row.record.id == stage.modelId) {
        found = &row;
        break;
      }
    }
    if (found == nullptr) {
      return refuse(QStringLiteral("model.owned.missing"),
                    assemblyText("The owned store holds no %1.").arg(stage.modelId));
    }
    stageModels.append(found->record);
  }

  PreparedAdmissionRequest request;
  request.plan = plan;
  request.models = stageModels;
  request.build = identity;
  request.store = ArtifactStore();
  request.modelRoot = ModelInventory::defaultRoot();
  request.source = input.context.source;
  request.device = input.context.device;
  // The admission wire carries the compute device in the ABI's own spelling.
  // The producer publishes the device it runs on dashed for display, but its
  // admission validator compares the plan's compute value against the 32
  // lower-case hex form its ABI names, so the published spelling could never be
  // admitted. The factory observation IS that ABI form, and it is substituted
  // only when it already names this very observed compute device - never as a
  // second source of device identity. The decoder UUID is left exactly as
  // observed: the producer compares that one against its own published text.
  if (input.context.factory.has_value() &&
      mplayer::native::isSameDeviceUuid(input.context.factory->deviceIdentity,
                                        input.context.device.computeDeviceUuid)) {
    request.device.computeDeviceUuid = input.context.factory->deviceIdentity;
  }
  request.recipeId = input.selection.recipeId;
  // The plan is the authority for the backend it was resolved for; naming the
  // requested one here would refuse the whole admission on a mismatch.
  request.backend = plan.backend;
  request.requestId = input.requestId;
  request.revision = input.revision < 0 ? 0 : static_cast<quint64>(input.revision);
  // The owning core stamps the source epoch at submission; nothing here can know
  // it, and a value guessed here would be a request for another source.
  request.epoch = 0;

  // Prove the whole plan and lease every artifact it names, off the GUI thread.
  // The same resolver the controller runs is used, so the worker's verdict and
  // the controller's cannot diverge, and a refused plan is reported by name
  // before anything is submitted. The leases taken here are released with this
  // value; the controller re-proves and keeps the claims for the life of the
  // admission. Nothing is built, and no cache entry is written.
  QString admissionKey;
  QString admissionReason;
  const std::optional<PreparedAdmission> admission =
      mplayer::native::resolvePreparedAdmission(request, &admissionKey, &admissionReason);
  if (!admission.has_value()) {
    return refuse(admissionKey.isEmpty() ? QStringLiteral("native.assembly.admissionRefused")
                                         : admissionKey,
                  admissionReason);
  }

  NativeAssemblyOutcome outcome;
  outcome.ok = true;
  outcome.request = request;
  return outcome;
}

namespace {

/// One dialog's assembly identity. The generation is bumped when a request is
/// announced and when a request is cancelled, so a worker's result is only
/// delivered while it still describes the selection and owning core that asked.
struct AssemblyState {
  std::atomic<quint64> generation{0};
  std::atomic<qint64> revision{0};
};

void deliverAssembly(const std::shared_ptr<AssemblyState>& state,
                     const QPointer<mplayer::shell::PlayerDialog>& guard, quint64 generation,
                     const std::shared_ptr<NativeAssemblyOutcome>& outcome) {
  if (guard.isNull()) return;
  if (state->generation.load() != generation) {
    // The selection or the owning core moved while this was in flight. The
    // result describes a request nobody wants any more.
    return;
  }
  if (outcome->ok) {
    guard->submitNativeAdmission(outcome->request);
    return;
  }
  guard->reportNativeProcessingFailure(outcome->reasonKey, outcome->reason);
}

}  // namespace

void bindNativePlaybackAssembly(Runner& runner, mplayer::shell::PlayerDialog& dialog) {
  auto state = std::make_shared<AssemblyState>();
  const QPointer<mplayer::shell::PlayerDialog> guard(&dialog);
  QPointer<Runner> runnerGuard(&runner);

  // The connection's context is the dialog itself, so both die together and no
  // QObject chain is created.
  QObject::connect(
      &dialog, &mplayer::shell::PlayerDialog::nativeAdmissionRequested, &dialog,
      [state, guard, runnerGuard](const mplayer::native::SelectionState& selection,
                                  const mplayer::native::NativePreparationContext& context) {
        if (guard.isNull() || runnerGuard.isNull()) return;
        const quint64 generation = state->generation.fetch_add(1) + 1;
        NativeAssemblyRequest request;
        request.selection = selection;
        request.context = context;
        request.requestId = QStringLiteral("mikai.assembly.%1").arg(generation);
        request.revision = state->revision.fetch_add(1) + 1;
        auto outcome = std::make_shared<NativeAssemblyOutcome>();
        runnerGuard->run(
            [request, outcome]() -> QVariant {
              *outcome = assembleNativeAdmission(request);
              return QVariant();
            },
            [state, guard, generation, outcome](const QVariant&) {
              deliverAssembly(state, guard, generation, outcome);
            },
            [state, guard, generation, outcome](const backend::Failure&) {
              // The pool failed to run the work at all; there is no assembled
              // request, and the refusal is still delivered under the same
              // guards rather than silently dropped.
              deliverAssembly(state, guard, generation, outcome);
            });
      });

  QObject::connect(&dialog, &mplayer::shell::PlayerDialog::nativeAdmissionCancelled, &dialog,
                   [state]() { state->generation.fetch_add(1); });
}

}  // namespace mikai::app
