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

#include <utility>
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


struct NativeAssetsPanel::ImportResult {
    bool ok = false;
    bool published = false;
    bool cancelled = false;
    QString reasonKey;
    QString reason;
    QString modelId;
};

struct NativeAssetsPanel::PrepareResult {
    /// The panel's own wording for a GUI-side failure that carried no provider
    /// reason of its own. Kept typed, never as rendered words, so the caption can
    /// be restated in another language without rebuilding anything; the builder's
    /// own verdicts leave it `None` and their reasons ride verbatim in `reason`.
    enum class PanelReason { None, ContextUnobserved, CaptureUnavailable };
    PanelReason panelReason = PanelReason::None;
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

void NativeAssetsPanel::renderPrepareProgress() {
    // The builder's own `phase` and `detail` are the worker's words and are shown
    // verbatim in every language; only this panel's wrapper around them is
    // restated. A state with nothing to say clears the label.
    QString text;
    switch (prepareProgressState_.kind) {
        case PrepareProgress::Kind::None:
            break;
        case PrepareProgress::Kind::Probing:
            text = tr("Probing the live source for its geometry and device…");
            break;
        case PrepareProgress::Kind::Capturing:
            text = tr("Capturing the actual source frame %1 of %2…")
                       .arg(prepareProgressState_.captureIndex + 1)
                       .arg(prepareProgressState_.frameCount);
            break;
        case PrepareProgress::Kind::Stage:
            text = tr("Preparing stage %1: %2")
                       .arg(prepareProgressState_.stageIndex)
                       .arg(prepareProgressState_.phase);
            if (!prepareProgressState_.detail.isEmpty()) {
                text += QStringLiteral(" — ") + prepareProgressState_.detail;
            }
            break;
        case PrepareProgress::Kind::Published:
            text = tr("Published %1 stage(s) into the owned artifact cache.")
                       .arg(prepareProgressState_.publishedStages);
            break;
        case PrepareProgress::Kind::AlreadyPrepared:
            text = tr("Every stage was already prepared and verified in the owned artifact cache.");
            break;
    }
    if (prepareProgress_ != nullptr) prepareProgress_->setText(text);
}

QString NativeAssetsPanel::prepareStatusText() const {
    switch (prepareStatusState_.kind) {
        case PrepareStatus::Kind::None:
            return {};
        case PrepareStatus::Kind::InspectorMissing:
            return tr("This build was made without the ONNX Runtime inspector.");
        case PrepareStatus::Kind::Cancelling:
            return tr("Cancelling the preparation; the builder decides at its next check.");
        case PrepareStatus::Kind::CommitInProgress:
            return tr("The preparation has already begun publishing, so it can no longer be "
                      "cancelled; its committed outcome will be reported.");
        case PrepareStatus::Kind::Cancelled:
            return tr("The preparation was cancelled; nothing was published.");
        case PrepareStatus::Kind::Failed: {
            // The builder's reason and its tool output stay verbatim; the panel
            // only supplies the fallback for a run that named no reason at all, and
            // that fallback is typed so it is said in the active language.
            QString text = prepareStatusState_.text;
            if (text.isEmpty()) {
                switch (prepareStatusState_.fallback) {
                    case PrepareStatus::Fallback::ContextUnobserved:
                        text = tr("The live source's geometry and device could not be observed.");
                        break;
                    case PrepareStatus::Fallback::CaptureUnavailable:
                        text = tr("The actual decoded frame this reference needs could not be "
                                  "captured.");
                        break;
                    case PrepareStatus::Fallback::None:
                        text = tr("The preparation failed; nothing was published.");
                        break;
                }
            }
            if (!prepareStatusState_.toolError.isEmpty()) {
                text += QStringLiteral("\n") + prepareStatusState_.toolError;
            }
            return text;
        }
        case PrepareStatus::Kind::Refusal: {
            // Restated from the recorded template and its raw detail: no store is
            // re-read and no provider is re-asked to say the same refusal again.
            const QString text = prepareRefusalText(prepareStatusState_.refusal);
            return text.isEmpty() ? prepareStatusState_.text : text;
        }
    }
    return {};
}

void NativeAssetsPanel::renderPrepareStatus() {
    prepareStatusKey_ = prepareStatusState_.key;
    prepareStatusReason_ = prepareStatusText();
    if (preparationReason_ != nullptr) preparationReason_->setText(prepareStatusReason_);
}

void NativeAssetsPanel::setPrepareStatus(PrepareStatus::Kind kind, const QString& key,
                                         const QString& text, const QString& toolError,
                                         PrepareStatus::Fallback fallback) {
    prepareStatusState_.kind = kind;
    prepareStatusState_.key = key;
    // A status that is not a refusal owns no refusal identity.
    prepareStatusState_.refusal = PrepareRefusalState();
    prepareStatusState_.text = text;
    prepareStatusState_.toolError = toolError;
    prepareStatusState_.fallback = fallback;
    renderPrepareStatus();
}

void NativeAssetsPanel::setPrepareRefusal(const PrepareRefusalState& refusal) {
    prepareStatusState_.kind = PrepareStatus::Kind::Refusal;
    prepareStatusState_.key = refusal.key;
    prepareStatusState_.refusal = refusal;
    prepareStatusState_.text.clear();
    prepareStatusState_.toolError.clear();
    prepareStatusState_.fallback = PrepareStatus::Fallback::None;
    renderPrepareStatus();
}

void NativeAssetsPanel::clearPrepareStatus() {
    prepareStatusState_ = PrepareStatus();
    prepareStatusKey_.clear();
    prepareStatusReason_.clear();
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
    // The provider's own words ride verbatim; the panel's fallback stays typed and
    // is worded when the caption is painted.
    result->reason = reason;
    result->panelReason = PrepareResult::PanelReason::ContextUnobserved;
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
    // The provider's own words ride verbatim; the panel's fallback stays typed and
    // is worded when the caption is painted.
    result->reason = reason;
    result->panelReason = PrepareResult::PanelReason::CaptureUnavailable;
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
    // The caption is typed state, not rendered text: the same frame of the same
    // count is re-worded in place on a LanguageChange.
    prepareProgressState_.kind = PrepareProgress::Kind::Capturing;
    prepareProgressState_.captureIndex = frameIndexRequested_;
    prepareProgressState_.frameCount = framesNeeded_;
    renderPrepareProgress();
    frameRequest_(frameIndexRequested_, prepareGeneration_);
}

void NativeAssetsPanel::setRequestedRecipe(const QString& recipeId) {
    if (requestedRecipeId_ == recipeId) return;
    requestedRecipeId_ = recipeId;
    // A different selection is a different request: a previous job's verdict no
    // longer describes it.
    clearPrepareStatus();
    prepareProgressState_ = PrepareProgress();
    renderPrepareProgress();
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

QString NativeAssetsPanel::prepareRefusalText(const PrepareRefusalState& refusal) const {
    // The panel words its own refusals here, in whatever language is active; a
    // store's or provider's own reason rides in `detail` and is shown verbatim.
    switch (refusal.templateId) {
        case PrepareRefusalState::Template::None:
            return {};
        case PrepareRefusalState::Template::Inspector:
            return tr("This build was made without the ONNX Runtime inspector, so no plan can be "
                      "resolved and no engine built.");
        case PrepareRefusalState::Template::Runner:
            return tr("No off-thread runner is available, so no preparation can be submitted.");
        case PrepareRefusalState::Template::RecipeRequired:
            return tr("Select a registered recipe in this page first; the untouched Original "
                      "pipeline is never prepared.");
        case PrepareRefusalState::Template::RecipeMissing:
            return refusal.detail.isEmpty() ? tr("The owned recipe store does not hold that recipe.")
                                            : refusal.detail;
        case PrepareRefusalState::Template::ModelUnavailable:
            return tr("The owned store holds no usable %1 for this recipe.").arg(refusal.detail);
        case PrepareRefusalState::Template::SourceUnwired:
            return tr(
                "No source-presence path is wired, so there is no live video to prepare for.");
        case PrepareRefusalState::Template::SourceAbsent:
            return refusal.detail.isEmpty()
                       ? tr("No playing or paused video is available to prepare for.")
                       : refusal.detail;
        case PrepareRefusalState::Template::ContextUnwired:
            return tr("No native probe path is wired, so the live source's geometry and device "
                      "cannot be observed.");
        case PrepareRefusalState::Template::Toolchain: {
            // The three reasons this panel words are restated from the key; a
            // provider's own words are kept exactly as recorded.
            const QString text = nativePreparationToolchainReason(refusal.key, refusal.detail);
            return text.isEmpty() ? tr("The preparation toolchain has not been observed.") : text;
        }
        case PrepareRefusalState::Template::BackendUnavailable:
            return tr("The observed toolchain does not carry the %1 backend this recipe asks for.")
                .arg(refusal.detail);
        case PrepareRefusalState::Template::Capture:
            return tr("No raw-frame capture path is wired, so the actual decoded frame this "
                      "reference needs cannot be requested.");
    }
    return {};
}

void NativeAssetsPanel::prepareRefusal(QString* reasonKey, QString* reason,
                                       PrepareRefusalState* refusal) const {
    // One decision, recorded as a template plus its raw detail rather than as
    // rendered words: a caller that shows this reason can restate it later without
    // walking the store or asking a provider again.
    PrepareRefusalState state;
    const auto refuse = [&state](PrepareRefusalState::Template templateId, const QString& key,
                                 const QString& detail = QString()) {
        state.templateId = templateId;
        state.key = key;
        state.detail = detail;
        return true;
    };
    const auto decide = [&]() -> bool {
        if (!inspectorBuilds()) {
            return refuse(PrepareRefusalState::Template::Inspector,
                          QStringLiteral("native.prepare.inspector"));
        }
        if (runner_ == nullptr) {
            return refuse(PrepareRefusalState::Template::Runner,
                          QStringLiteral("native.prepare.runner"));
        }
        if (requestedRecipeId_.isEmpty()) {
            return refuse(PrepareRefusalState::Template::RecipeRequired,
                          QStringLiteral("native.prepare.recipe"));
        }
        UserRecipeStore recipes;
        QString recipeKey;
        QString recipeReason;
        const std::optional<Recipe> recipe =
            recipes.find(requestedRecipeId_, &recipeKey, &recipeReason);
        if (!recipe.has_value()) {
            return refuse(PrepareRefusalState::Template::RecipeMissing,
                          recipeKey.isEmpty() ? QStringLiteral("native.prepare.recipe")
                                              : recipeKey,
                          recipeReason);
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
                return refuse(PrepareRefusalState::Template::ModelUnavailable,
                              QStringLiteral("native.prepare.models"), stage.modelId);
            }
        }
        // An actual source a probe and a capture can run against, which is distinct
        // from a fully observed context: the device facts come from the native probe,
        // and this panel only arms that probe after the user clicks Prepare. Requiring
        // the context here would make the successful path unreachable.
        if (!sourcePresence_) {
            return refuse(PrepareRefusalState::Template::SourceUnwired,
                          QStringLiteral("native.prepare.source"));
        }
        QString presenceKey;
        QString presenceReason;
        if (!sourcePresence_(&presenceKey, &presenceReason)) {
            return refuse(PrepareRefusalState::Template::SourceAbsent,
                          presenceKey.isEmpty() ? QStringLiteral("native.prepare.source")
                                                : presenceKey,
                          presenceReason);
        }
        if (!contextRequest_ && !currentContext().has_value()) {
            return refuse(PrepareRefusalState::Template::ContextUnwired,
                          QStringLiteral("native.prepare.context"));
        }
        if (!observedToolchain_.available) {
            // The toolchain is not re-observed to say this: the key and the recorded
            // reason are all the restatement needs.
            return refuse(PrepareRefusalState::Template::Toolchain,
                          observedToolchain_.reasonKey.isEmpty()
                              ? QStringLiteral("native.prepare.toolchain")
                              : observedToolchain_.reasonKey,
                          observedToolchain_.reason);
        }
        if (!observedToolchain_.backends.has(recipe->backend)) {
            return refuse(PrepareRefusalState::Template::BackendUnavailable,
                          QStringLiteral("backend.unavailable"), stableName(recipe->backend));
        }
        if (!frameRequest_) {
            return refuse(PrepareRefusalState::Template::Capture,
                          QStringLiteral("native.prepare.capture"));
        }
        return false;
    };
    decide();
    if (reasonKey != nullptr) *reasonKey = state.key;
    if (reason != nullptr) {
        *reason = state.templateId == PrepareRefusalState::Template::None ? QString()
                                                                         : prepareRefusalText(state);
    }
    if (refusal != nullptr) *refusal = state;
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
    if (preparationReason_ == nullptr) return;
    if (prepareStatusState_.kind != PrepareStatus::Kind::None) {
        // A recorded verdict or in-progress notice is restated in the active
        // language rather than replaced by the generic in-flight line: a cancel
        // still landing and a publication that can no longer be stopped both say
        // more than "preparing".
        renderPrepareStatus();
    } else if (prepareInFlight_) {
        preparationReason_->setText(tr("Preparing the selected recipe…"));
    } else if (!ready) {
        preparationReason_->setText(reason);
    } else {
        preparationReason_->setText(
            tr("Ready to prepare: the source, the toolchain and every owned model this recipe "
               "needs are observed."));
    }
}

bool NativeAssetsPanel::prepareArtifact() {
    clearPrepareStatus();
    QString key;
    // The typed identity is what gets recorded; its words are worded when painted.
    PrepareRefusalState refusal;
    prepareRefusal(&key, nullptr, &refusal);
    if (!key.isEmpty()) {
        setPrepareRefusal(refusal);
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
        // Recorded as a typed refusal: the store's own words ride along verbatim in
        // `detail`, and the panel's named fallback answers when it gave none.
        PrepareRefusalState missingRecipe;
        missingRecipe.templateId = PrepareRefusalState::Template::RecipeMissing;
        missingRecipe.key =
            recipeKey.isEmpty() ? QStringLiteral("native.prepare.recipe") : recipeKey;
        missingRecipe.detail = recipeReason;
        setPrepareRefusal(missingRecipe);
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
    prepareProgressState_ = PrepareProgress();
    renderPrepareProgress();
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
        prepareProgressState_.kind = PrepareProgress::Kind::Probing;
        renderPrepareProgress();
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
    setPrepareStatus(PrepareStatus::Kind::InspectorMissing,
                     QStringLiteral("native.prepare.inspector"), QString());
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
        setPrepareStatus(PrepareStatus::Kind::CommitInProgress,
                         QStringLiteral("native.prepare.commitInProgress"), QString());
        return;
    }
    setPrepareStatus(PrepareStatus::Kind::Cancelling, QStringLiteral("native.prepare.cancelling"),
                     QString());
    updatePrepareControls();
}

void NativeAssetsPanel::applyPrepareProgress(int stageIndex, int stageCount, const QString& phase,
                                             const QString& detail) {
    if (!prepareInFlight_) return;
    // The stage stays typed so a LanguageChange re-announces this very stage - same
    // index, count, phase and detail - and re-words only this panel's wrapper.
    prepareProgressState_.kind = PrepareProgress::Kind::Stage;
    prepareProgressState_.stageIndex = stageIndex;
    prepareProgressState_.stageCount = stageCount;
    prepareProgressState_.phase = phase;
    prepareProgressState_.detail = detail;
    renderPrepareProgress();
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
        clearPrepareStatus();
        // The verdict is kept typed: a language switch restates the same outcome
        // with the same published count, and the finished stage is dropped.
        prepareProgressState_ = PrepareProgress();
        prepareProgressState_.kind = result->publishedBuildKeys.isEmpty()
                                         ? PrepareProgress::Kind::AlreadyPrepared
                                         : PrepareProgress::Kind::Published;
        prepareProgressState_.publishedStages = result->publishedBuildKeys.size();
        renderPrepareProgress();
    } else if (result->cancelled) {
        setPrepareStatus(PrepareStatus::Kind::Cancelled, QStringLiteral("native.prepare.cancelled"),
                         QString());
        prepareProgressState_ = PrepareProgress();
        renderPrepareProgress();
    } else {
        // The builder's own reason and tool output are kept verbatim; the panel
        // only names a typed fallback when the run gave no reason at all.
        const auto fallbackFor = [](PrepareResult::PanelReason reason) {
            switch (reason) {
                case PrepareResult::PanelReason::ContextUnobserved:
                    return PrepareStatus::Fallback::ContextUnobserved;
                case PrepareResult::PanelReason::CaptureUnavailable:
                    return PrepareStatus::Fallback::CaptureUnavailable;
                case PrepareResult::PanelReason::None:
                    break;
            }
            return PrepareStatus::Fallback::None;
        };
        setPrepareStatus(PrepareStatus::Kind::Failed,
                         result->reasonKey.isEmpty() ? QStringLiteral("native.prepare.failed")
                                                     : result->reasonKey,
                         result->reason, result->toolError, fallbackFor(result->panelReason));
        prepareProgressState_ = PrepareProgress();
        renderPrepareProgress();
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
