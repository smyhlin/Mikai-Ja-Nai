// The app-owned assembly probe: it drives the real `assembleNativeAdmission`
// worker and the real `bindNativePlaybackAssembly` delivery against this
// machine's own app-owned model store, artifact cache and a real PlayerDialog.
//
// Two modes, because the two things it proves need different environments.
//
//  * Isolated (default): the probe names its own app-local tree, so it can
//    publish a genuine owned row, register a real stored recipe and assert the
//    assembly's refusals by their exact keys over real values, then prove that a
//    superseded run is discarded rather than delivered to a real player. It
//    makes no positive claim: a whole-plan admission needs a prepared engine and
//    this mode has none.
//
//  * Positive (`--recipe`): the probe names the application's own app-local tree
//    (or `--app-org`/`--app-name`), publishes nothing and wipes nothing, and runs
//    the real assembly over the caller's own observed source/device/module and a
//    recipe that already exists there. `ok` is the only pass: a refusal is a
//    failure with its exact key and reason printed, so a machine that is not
//    actually prepared cannot be reported as if it were.
//
// Every value is produced by the real store or is the caller's own observation.
// Nothing is echoed from a text file, no arbitrary bytes are published as a
// prepared engine, and no Ready state is fabricated.

#include <cstdio>
#include <optional>

#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtCore/QCryptographicHash>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QSize>
#include <QtCore/QStringList>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>

#include <PlayerDialog.hpp>
#include <VideoSurface.hpp>
#include <native/prepare/NativeArtifactStore.hpp>
#include <native/bundle/NativeManifest.hpp>
#include <native/models/NativeModelInspect.hpp>
#include <native/models/NativeModelInventory.hpp>
#include <native/bundle/NativePaths.hpp>
#include <native/prepare/NativePreparationContext.hpp>
#include <native/status/NativeTypes.hpp>
#include <native/models/NativeUserRecipes.hpp>

#include "ProbeRuntime.hpp"
#include "ProbeUi.hpp"
#include "dialogs/NativeAssetsPanel.hpp"
#include "state/NativePlaybackAssembly.hpp"
#include "workers/Runner.hpp"

using mplayer::native::ArtifactStore;
using mplayer::native::Backend;
using mplayer::native::BackendAvailability;
using mplayer::native::DeviceIdentity;
using mplayer::native::InspectionResult;
using mplayer::native::ModelInventory;
using mplayer::native::ModelRole;
using mplayer::native::NativePreparationContext;
using mplayer::native::Rational;
using mplayer::native::SelectionState;
using mplayer::native::SourceGeometry;
using mplayer::native::StoredModel;
using mplayer::native::UserRecipeStore;
using mikai::app::NativeAssemblyOutcome;
using mikai::app::NativeAssemblyRequest;
using mikai::app::NativePreparationToolchain;
using mikai::app::Runner;
using mikai::app::assembleNativeAdmission;
using mikai::app::bindNativePlaybackAssembly;
using mikai::app::observeNativePreparationToolchain;

namespace {

int failures = 0;

void check(bool ok, const QString& what) {
    std::fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) ++failures;
}

void say(const QString& text) { std::fprintf(stderr, "%s\n", qPrintable(text)); }

QString sha256Hex(const QByteArray& bytes) {
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QStringList cacheEntries() {
    QDir directory(ArtifactStore().root());
    if (!directory.exists()) return {};
    return directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
}

/// The source the caller observed. The probe cannot observe a decoder without a
/// running core, so in the isolated mode these are the caller's own observation
/// supplied to the worker exactly as a live player would; the positive mode takes
/// them from the command line.
SourceGeometry observedSource(const QSize& size, const Rational& fps) {
    SourceGeometry source;
    source.codedSize = size;
    source.rotationDegrees = 0;
    source.fps = fps;
    source.csp = QStringLiteral("nv12");
    source.range = QStringLiteral("tv");
    source.bitDepth = 8;
    return source;
}

DeviceIdentity observedDevice(const QString& uuid, int smMajor, int smMinor) {
    DeviceIdentity device;
    device.decoderDeviceUuid = uuid;
    device.computeDeviceUuid = uuid;
    device.smMajor = smMajor;
    device.smMinor = smMinor;
    return device;
}

/// Runs the worker and lets the GUI thread deliver whatever it produced.
void settle(Runner& runner) {
    runner.waitForDone(60000);
    for (int round = 0; round < 8; ++round) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    }
}

void invokeRequest(mplayer::shell::PlayerDialog& dialog, const SelectionState& selection,
                   const NativePreparationContext& context) {
    const bool invoked = QMetaObject::invokeMethod(
        &dialog, "nativeAdmissionRequested", Qt::DirectConnection,
        Q_ARG(mplayer::native::SelectionState, selection),
        Q_ARG(mplayer::native::NativePreparationContext, context));
    check(invoked, QStringLiteral("the dialog's request signal is invocable"));
}

void invokeCancelled(mplayer::shell::PlayerDialog& dialog) {
    const bool invoked = QMetaObject::invokeMethod(&dialog, "nativeAdmissionCancelled",
                                                   Qt::DirectConnection);
    check(invoked, QStringLiteral("the dialog's cancellation signal is invocable"));
}

/// The binding's own delivery behaviour, against a real dialog. A refusal for an
/// empty selection is deterministic, so the two cases differ only by whether the
/// run was superseded in flight.
void runBinderChecks(Runner& runner) {
    auto* dialog = new mplayer::shell::PlayerDialog(QStringLiteral("Assembly probe"),
                                                    QStringLiteral("file:///probe/absent"),
                                                    nullptr);
    QPointer<mplayer::shell::PlayerDialog> dialogGuard(dialog);
    QLabel* status = dialog->findChild<QLabel*>(QStringLiteral("playerStatusText"));
    check(status != nullptr, QStringLiteral("the player exposes its own status label"));
    QObject* const parentBeforeBind = dialog->parent();
    bindNativePlaybackAssembly(runner, *dialog);
    check(dialog->parent() == parentBeforeBind,
          QStringLiteral("binding the assembly creates no QObject parent chain"));

    SelectionState refusedSelection;  // no recipe: a deterministic named refusal
    NativePreparationContext context;
    context.source = observedSource(QSize(640, 360), Rational{24, 1});
    context.device = observedDevice(QStringLiteral("GPU-probe"), 12, 0);
    context.modulePath = QStringLiteral("probe-module");

    const QString beforeDelivery = status != nullptr ? status->text() : QString();
    invokeRequest(*dialog, refusedSelection, context);
    settle(runner);
    const QString afterDelivery = status != nullptr ? status->text() : QString();
    check(status != nullptr && afterDelivery != beforeDelivery,
          QStringLiteral("a current run's refusal is delivered to the real player"));

    const QString beforeStale = status != nullptr ? status->text() : QString();
    invokeRequest(*dialog, refusedSelection, context);
    invokeCancelled(*dialog);
    settle(runner);
    check(status != nullptr && status->text() == beforeStale,
          QStringLiteral("a superseded run is discarded instead of being delivered"));

    // A second, independent binding must not share state with the first.
    auto* second = new mplayer::shell::PlayerDialog(QStringLiteral("Assembly probe 2"),
                                                    QStringLiteral("file:///probe/absent"),
                                                    nullptr);
    QPointer<mplayer::shell::PlayerDialog> secondGuard(second);
    QLabel* secondStatus = second->findChild<QLabel*>(QStringLiteral("playerStatusText"));
    bindNativePlaybackAssembly(runner, *second);
    if (secondStatus != nullptr) {
        const QString beforeSecond = secondStatus->text();
        invokeRequest(*second, refusedSelection, context);
        settle(runner);
        check(secondStatus->text() != beforeSecond,
              QStringLiteral("a second binding delivers on its own dialog"));
    }
    if (!secondGuard.isNull()) secondGuard->deleteLater();

    // A dialog destroyed while a run is still in flight must never be delivered
    // to afterwards: the binding holds only a QPointer and the run's generation,
    // so the late completion is dropped rather than applied to a dead widget.
    auto* closing = new mplayer::shell::PlayerDialog(
        QStringLiteral("Assembly probe 3"), QStringLiteral("file:///probe/absent"), nullptr);
    bindNativePlaybackAssembly(runner, *closing);
    invokeRequest(*closing, refusedSelection, context);
    delete closing;
    settle(runner);

    // Reopening a fresh dialog after the destroyed one still binds, delivers on
    // its own surface and shares no state with the dead one.
    auto* reopened = new mplayer::shell::PlayerDialog(
        QStringLiteral("Assembly probe 4"), QStringLiteral("file:///probe/absent"), nullptr);
    QPointer<mplayer::shell::PlayerDialog> reopenGuard(reopened);
    QLabel* reopenStatus = reopened->findChild<QLabel*>(QStringLiteral("playerStatusText"));
    bindNativePlaybackAssembly(runner, *reopened);
    if (reopenStatus != nullptr) {
        const QString beforeReopen = reopenStatus->text();
        invokeRequest(*reopened, refusedSelection, context);
        settle(runner);
        check(reopenStatus->text() != beforeReopen,
              QStringLiteral("a reopened dialog binds and delivers on its own surface"));
    }
    if (!reopenGuard.isNull()) reopenGuard->deleteLater();
    if (!dialogGuard.isNull()) dialogGuard->deleteLater();
}

/// The isolated mode: a genuine owned row, a real stored recipe, the exact
/// refusal matrix and the binder checks. No prepared engine is published, so
/// nothing here claims a whole plan can be admitted.
int runIsolated() {
    QDir(mplayer::native::nativeRoot()).removeRecursively();

    const QByteArray graphBytes = QByteArrayLiteral("probe-owned-graph-bytes");
    InspectionResult inspection;
    inspection.ok = true;
    inspection.inputName = QStringLiteral("X");
    inspection.outputName = QStringLiteral("Y");
    inspection.declared.input.dtype = QString::fromLatin1(mplayer::native::kEligibleIoType);
    inspection.declared.input.rank = 4;
    inspection.declared.input.batch = 1;
    inspection.declared.input.channels = 3;
    inspection.declared.input.height = 64;
    inspection.declared.input.width = 64;
    inspection.declared.output = inspection.declared.input;
    BackendAvailability backends;
    backends.available.append(Backend::TensorRT);

    ModelInventory modelStore;
    const mplayer::native::ModelImportOutcome published = modelStore.publish(
        graphBytes, inspection, QStringLiteral("probe://owned"), QStringLiteral("Probe model"),
        backends, QString(), ModelRole::Spatial);
    check(published.ok, QStringLiteral("a real owned row publishes: %1").arg(published.reasonKey));

    QString storeReason;
    const QVector<StoredModel> rows = modelStore.stored(&storeReason);
    check(storeReason.isEmpty(), QStringLiteral("the owned store reads without a refusal"));
    const StoredModel* row = nullptr;
    for (const StoredModel& candidate : rows) {
        if (candidate.record.onnxSha256 == sha256Hex(graphBytes)) row = &candidate;
    }
    check(row != nullptr, QStringLiteral("the published row is in the inventory"));
    if (row != nullptr) {
        check(row->record.availability.isUsable(),
              QStringLiteral("the row is usable with a mandatory reason"));
        check(QFileInfo(row->storedPath).exists(),
              QStringLiteral("the owned copy is on disk inside the app-owned store"));
    }

    UserRecipeStore recipes;
    QString recipeKey;
    QString recipeReason;
    std::optional<mplayer::native::Recipe> recipe;
    if (row != nullptr) {
        recipe = recipes.addSpatialModel(row->record, Backend::TensorRT, &recipeKey, &recipeReason);
    }
    check(recipe.has_value(),
          QStringLiteral("the owned row registers as a spatial recipe: %1").arg(recipeKey));
    const QString recipeId = recipe.has_value() ? recipe->id : QString();

    const QStringList cacheBefore = cacheEntries();

    NativePreparationContext complete;
    complete.source = observedSource(QSize(640, 360), Rational{24, 1});
    complete.device = observedDevice(QStringLiteral("GPU-probe"), 12, 0);
    complete.modulePath =
        QDir(QDir::tempPath()).filePath(QStringLiteral("probe-bundle/libmpv-2.dll"));

    NativeAssemblyRequest request;
    request.requestId = QStringLiteral("probe-1");
    request.revision = 1;
    request.context = complete;

    NativeAssemblyOutcome outcome = assembleNativeAdmission(request);
    check(!outcome.ok && outcome.reasonKey == QStringLiteral("native.assembly.selection"),
          QStringLiteral("an empty selection refuses by name (got %1)").arg(outcome.reasonKey));

    request.selection.recipeId = recipeId.isEmpty() ? QStringLiteral("probe.recipe.missing")
                                                    : recipeId;
    request.selection.backend = Backend::TensorRT;

    NativeAssemblyRequest noSource = request;
    noSource.context.source = SourceGeometry();
    outcome = assembleNativeAdmission(noSource);
    check(!outcome.ok && outcome.reasonKey == QStringLiteral("native.assembly.sourceUnobserved"),
          QStringLiteral("an unobserved source refuses by name (got %1)").arg(outcome.reasonKey));

    NativeAssemblyRequest noDevice = request;
    noDevice.context.device = DeviceIdentity();
    outcome = assembleNativeAdmission(noDevice);
    check(!outcome.ok && outcome.reasonKey == QStringLiteral("native.assembly.deviceUnobserved"),
          QStringLiteral("an unobserved device refuses by name (got %1)").arg(outcome.reasonKey));

    NativeAssemblyRequest noModule = request;
    noModule.context.modulePath.clear();
    outcome = assembleNativeAdmission(noModule);
    check(!outcome.ok && outcome.reasonKey == QStringLiteral("native.assembly.moduleUnobserved"),
          QStringLiteral("an unobserved module refuses by name (got %1)").arg(outcome.reasonKey));

    // With every caller-observable fact present, the worker reaches the toolchain
    // gate. The probe observes that gate itself, so the assertion is exact: the
    // assembly must forward the toolchain's own named refusal, not a substitute.
    const NativePreparationToolchain toolchain = observeNativePreparationToolchain();
    outcome = assembleNativeAdmission(request);
    if (!toolchain.available) {
        check(!outcome.ok && !toolchain.reasonKey.isEmpty() &&
                  outcome.reasonKey == toolchain.reasonKey,
              QStringLiteral("the assembly forwards the observed toolchain refusal by name "
                             "(expected %1, got %2)")
                  .arg(toolchain.reasonKey, outcome.reasonKey));
    } else {
        say(QStringLiteral("SKIP whole-plan positive: a native toolchain is installed here, so a "
                           "positive proof needs the real inputs - rerun with --recipe, --module, "
                           "--device-uuid and --sm."));
    }

    check(cacheEntries() == cacheBefore,
          QStringLiteral("no assembly attempt added an artifact or a staging directory"));
    QString afterReason;
    const QVector<StoredModel> rowsAfter = modelStore.stored(&afterReason);
    check(rowsAfter.size() == rows.size() && afterReason.isEmpty(),
          QStringLiteral("no assembly attempt wrote into the owned model store"));

    Runner runner;
    runBinderChecks(runner);

    QDir(mplayer::native::nativeRoot()).removeRecursively();
    return failures == 0 ? 0 : 1;
}

/// The positive mode: the real app-owned tree, the caller's own observed facts
/// and an existing recipe. `ok` is the only pass.
int runPositive(const QString& recipeId, const QString& modulePath, const QString& deviceUuid,
                int smMajor, int smMinor, const QSize& size, const Rational& fps) {
    if (recipeId.isEmpty() || deviceUuid.isEmpty() || !size.isValid() || !fps.isValid() ||
        modulePath.isEmpty()) {
        std::fprintf(stderr,
                     "native_playback_assembly_probe: --recipe, --module, --device-uuid, "
                     "--width/--height and --fps-num/--fps-den are all required for the positive "
                     "mode\n");
        return 2;
    }

    const QStringList cacheBefore = cacheEntries();
    QString storeReason;
    const QVector<StoredModel> rowsBefore = ModelInventory().stored(&storeReason);

    NativeAssemblyRequest request;
    request.requestId = QStringLiteral("probe-positive");
    request.revision = 1;
    request.selection.recipeId = recipeId;
    request.selection.backend = Backend::TensorRT;
    request.context.source = observedSource(size, fps);
    request.context.device = observedDevice(deviceUuid, smMajor, smMinor);
    request.context.modulePath = modulePath;

    const NativeAssemblyOutcome outcome = assembleNativeAdmission(request);
    if (!outcome.ok) {
        std::fprintf(stderr,
                     "native_playback_assembly_probe: the whole-plan assembly refused %s: %s\n",
                     qPrintable(outcome.reasonKey), qPrintable(outcome.reason));
        ++failures;
        return 1;
    }
    check(outcome.request.plan.stages.size() > 0,
          QStringLiteral("the assembled request carries a whole resolved plan"));
    check(outcome.request.recipeId == recipeId,
          QStringLiteral("the assembled request names the requested recipe"));
    check(!outcome.request.requestId.isEmpty(),
          QStringLiteral("the assembled request carries its request id"));
    check(outcome.request.build.coreSha.size() == 40 && outcome.request.build.sourceAbi > 0,
          QStringLiteral("the build identity carries the bundle's core SHA and source ABI"));
    check(outcome.request.build.pluginVersion.startsWith(QStringLiteral("sha256:")),
          QStringLiteral("the plugin identity is the backend's verified content revision"));
    // The identity the worker assembled is the one the helper measured for the
    // device this caller observed: the same UUID and SM, and runtime revisions
    // the helper rendered rather than values this probe echoed.
    check(outcome.request.build.deviceUuid.compare(deviceUuid, Qt::CaseInsensitive) == 0,
          QStringLiteral("the measured identity names the observed device UUID"));
    check(outcome.request.build.smMajor == smMajor && outcome.request.build.smMinor == smMinor,
          QStringLiteral("the measured identity carries the observed compute capability"));
    check(!outcome.request.build.runtimeVersion.isEmpty() &&
              !outcome.request.build.cudaVersion.isEmpty(),
          QStringLiteral("the measured identity carries the helper's runtime versions"));
    check(!outcome.request.build.options.isEmpty(),
          QStringLiteral("the measured identity carries the helper's build options"));
    check(outcome.request.backend == Backend::TensorRT,
          QStringLiteral("the assembled request is for the requested backend"));
    for (const auto& stage : outcome.request.plan.stages) {
        bool owned = false;
        for (const StoredModel& row : rowsBefore) {
            if (row.record.id == stage.modelId) owned = true;
        }
        check(owned, QStringLiteral("every planned stage is an owned row (%1)").arg(stage.modelId));
        check(!stage.inputShape().isEmpty() && !stage.outputShape().isEmpty(),
              QStringLiteral("every planned stage resolves a concrete geometry (%1)")
                  .arg(stage.modelId));
    }

    check(cacheEntries() == cacheBefore,
          QStringLiteral("the whole-plan assembly wrote nothing into the artifact cache"));
    QString afterReason;
    const QVector<StoredModel> rowsAfter = ModelInventory().stored(&afterReason);
    check(rowsAfter.size() == rowsBefore.size() && afterReason.isEmpty(),
          QStringLiteral("the whole-plan assembly wrote nothing into the model store"));

    Runner runner;
    runBinderChecks(runner);
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    if (!mikai::probes::initializeUi(app)) return 2;

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Drives the app-owned native admission assembly. With no --recipe it runs the isolated "
        "refusal and delivery checks; with --recipe it proves a whole-plan assembly against the "
        "real app-owned tree and the caller's own observed facts."));
    parser.addHelpOption();
    const QCommandLineOption orgOption(
        QStringLiteral("app-org"),
        QStringLiteral("Organization that owns the app-local tree (positive mode)."),
        QStringLiteral("org"), QStringLiteral("Mikai"));
    const QCommandLineOption nameOption(
        QStringLiteral("app-name"),
        QStringLiteral("Application name that owns the app-local tree (positive mode)."),
        QStringLiteral("name"), QStringLiteral("Mikai Wrapper"));
    const QCommandLineOption bundleOption(
        QStringLiteral("bundle"),
        QStringLiteral("Absolute root of the real native bundle to assemble against (positive "
                       "mode). Defaults to MIKAI_PROBE_BUNDLE, then the app-local selection."),
        QStringLiteral("path"));
    const QCommandLineOption recipeOption(QStringLiteral("recipe"),
                                          QStringLiteral("Stored recipe id to assemble."),
                                          QStringLiteral("id"));
    const QCommandLineOption moduleOption(
        QStringLiteral("module"),
        QStringLiteral("Absolute module the owning core runs on; defaults to the selected "
                       "bundle's own module."),
        QStringLiteral("path"));
    const QCommandLineOption uuidOption(QStringLiteral("device-uuid"),
                                        QStringLiteral("Observed compute device UUID."),
                                        QStringLiteral("uuid"));
    const QCommandLineOption smOption(QStringLiteral("sm"),
                                      QStringLiteral("Observed compute capability, e.g. 12.0."),
                                      QStringLiteral("major.minor"));
    const QCommandLineOption widthOption(QStringLiteral("width"), QStringLiteral("Source width."),
                                         QStringLiteral("px"));
    const QCommandLineOption heightOption(QStringLiteral("height"), QStringLiteral("Source height."),
                                          QStringLiteral("px"));
    const QCommandLineOption fpsNumOption(QStringLiteral("fps-num"),
                                          QStringLiteral("Source fps numerator."),
                                          QStringLiteral("n"));
    const QCommandLineOption fpsDenOption(QStringLiteral("fps-den"),
                                          QStringLiteral("Source fps denominator."),
                                          QStringLiteral("n"));
    parser.addOptions(QList<QCommandLineOption>{orgOption, nameOption, bundleOption, recipeOption,
                                                moduleOption, uuidOption, smOption, widthOption,
                                                heightOption, fpsNumOption, fpsDenOption});
    parser.process(app);

    const bool positive = parser.isSet(recipeOption);
    if (positive) {
        QCoreApplication::setOrganizationName(parser.value(orgOption));
        QCoreApplication::setApplicationName(parser.value(nameOption));

        // A caller-named bundle is selected through the one probe convention
        // every GUI probe shares, so the assembly reads exactly the bundle named
        // here rather than whatever the app-local tree last recorded. Without a
        // name, the app-local selection is used, exactly as the app would.
        const QString namedBundle = parser.isSet(bundleOption)
                                        ? parser.value(bundleOption)
                                        : qEnvironmentVariable("MIKAI_PROBE_BUNDLE");
        if (!namedBundle.isEmpty() &&
            !mplayer::probes::detail::selectNamedBundle(namedBundle)) {
            std::fprintf(stderr,
                         "native_playback_assembly_probe: the named bundle %s could not be "
                         "selected\n",
                         qPrintable(namedBundle));
            return 2;
        }
        int smMajor = 0;
        int smMinor = 0;
        const QString sm = parser.value(smOption);
        const int dot = sm.indexOf(QLatin1Char('.'));
        bool smOk = false;
        if (dot > 0) {
            bool majorOk = false;
            bool minorOk = false;
            smMajor = sm.left(dot).toInt(&majorOk);
            smMinor = sm.mid(dot + 1).toInt(&minorOk);
            smOk = majorOk && minorOk && smMajor > 0 && smMinor >= 0;
        }
        if (!smOk) {
            std::fprintf(stderr,
                         "native_playback_assembly_probe: --sm must be major.minor (got %s)\n",
                         qPrintable(sm));
            return 2;
        }
        const int width = parser.value(widthOption).toInt();
        const int height = parser.value(heightOption).toInt();
        const int fpsNum = parser.value(fpsNumOption).toInt();
        const int fpsDen = parser.value(fpsDenOption).toInt();
        qRegisterMetaType<mplayer::native::SelectionState>("mplayer::native::SelectionState");
        qRegisterMetaType<mplayer::native::NativePreparationContext>(
            "mplayer::native::NativePreparationContext");
        // A named module is used verbatim; otherwise the module of the runtime
        // this process actually selected is the one the owning core runs on, so
        // naming it here would only be a second copy of an observed fact.
        QString modulePath = parser.value(moduleOption);
        if (modulePath.isEmpty()) {
            modulePath = mplayer::native::currentSelection().modulePath;
        }
        const int result =
            runPositive(parser.value(recipeOption), modulePath, parser.value(uuidOption), smMajor,
                        smMinor, QSize(width, height), Rational{fpsNum, fpsDen});
        std::fprintf(stderr, "[native_playback_assembly_probe] positive %s (%d failures)\n",
                     failures == 0 ? "PASS" : "FAIL", failures);
        return result;
    }

    QCoreApplication::setOrganizationName(QStringLiteral("MikaiNativeAssemblyProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("NativeAssemblyProbe"));
    qRegisterMetaType<mplayer::native::SelectionState>("mplayer::native::SelectionState");
    qRegisterMetaType<mplayer::native::NativePreparationContext>(
        "mplayer::native::NativePreparationContext");
    const int result = runIsolated();
    std::fprintf(stderr, "[native_playback_assembly_probe] isolated %s (%d failures)\n",
                 failures == 0 ? "PASS" : "FAIL", failures);
    return result;
}
