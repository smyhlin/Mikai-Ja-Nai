// mikai_native_inspect - read what an ONNX graph declares, and optionally run one
// CPU reference at a stage geometry.
//
// This is the M11 instrument: it is the only place a developer can see the real
// declared boundary of a graph and the geometry the model actually produces,
// without any of it reaching the app's engine path. It reports facts only.
//
//   * The ONNX Runtime is named explicitly (--runtime or MIKAI_ORT_RUNTIME_DIR)
//     and loaded with a restricted search. When it is absent the tool says so,
//     prints the graph's path and size, and exits 2 - it does not print invented
//     metadata, and it never reports a model as ready.
//   * --resolve WxH runs one fp16 CPU reference and reports the output geometry
//     the model really produced. A CPU execution-provider refusal (for instance a
//     fp16 kernel this CPU provider does not carry) is printed verbatim as the
//     run's outcome; the graph is not altered and no scale is invented.
//   * --publish writes the immutable copy into the app-owned model store and
//     prints the resulting row. A refused graph publishes nothing.
//   * --plan-recipe resolves an ordered recipe against the owned store and
//     prints each stage's actual geometry. The geometry comes from the inspected
//     bytes the app owns, never from a file name; the plan stays Preparing, with
//     a named reason, until the source and the device are observed (--source,
//     --fps, --device), and no device is invented to make it look ready. A
//     resolved plan exits 0, a blocked one 1, and one still waiting on an
//     observation exits 4.
//
// It never converts, re-exports or executes a non-FP16 graph, and it never
// classifies a graph by its file name.

#include <optional>

#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTextStream>

#include "native/prepare/NativeFrameReference.hpp"
#include "native/models/NativeInspectedPlan.hpp"
#include "native/models/NativeModelInspect.hpp"
#include "native/models/NativeModelInventory.hpp"
#include "native/prepare/NativePreparationPlan.hpp"

using namespace mplayer::native;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitRefused = 1;
constexpr int kExitRuntimeUnavailable = 2;
constexpr int kExitUsage = 3;
/// A plan that is waiting on an observation is neither a refusal nor a success.
constexpr int kExitPreparing = 4;

struct Options {
    QString runtimeDirectory;
    QSize resolveGeometry;
    bool resolveRequested = false;
    bool publish = false;
    QString storeRoot;
    QVector<Backend> backends;
    bool json = false;
    QStringList models;

    // --plan-recipe mode: resolve an ordered recipe against the owned store. The
    // observations below are supplied by the caller; nothing here invents one.
    QString planRecipe;
    QSize sourceGeometry;
    std::optional<Rational> fps;
    DeviceIdentity device;

    QString referenceDir;
    QString fixture;
    struct RawInput {
        QString name;
        QString dtype;
        QVector<qint64> shape;
        QString path;
    };
    QVector<RawInput> rawInputs;
};

bool readGeometry(const QString& text, QSize* size) {
    static const QRegularExpression pattern(QStringLiteral("^(\\d+)x(\\d+)$"));
    const QRegularExpressionMatch match = pattern.match(text.trimmed());
    if (!match.hasMatch()) return false;
    const qint64 width = match.captured(1).toLongLong();
    const qint64 height = match.captured(2).toLongLong();
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) return false;
    *size = QSize(static_cast<int>(width), static_cast<int>(height));
    return true;
}

QString tensorText(const TensorProfile& tensor) {
    return QStringLiteral("%1 rank %2 %3").arg(tensor.dtype).arg(tensor.rank).arg(tensor.describe());
}

QJsonObject tensorJson(const TensorProfile& tensor) {
    QJsonObject object;
    object.insert(QStringLiteral("dtype"), tensor.dtype);
    object.insert(QStringLiteral("rank"), tensor.rank);
    object.insert(QStringLiteral("n"), QJsonValue(static_cast<double>(tensor.batch)));
    object.insert(QStringLiteral("c"), QJsonValue(static_cast<double>(tensor.channels)));
    if (tensor.height.has_value()) {
        object.insert(QStringLiteral("h"), QJsonValue(static_cast<double>(*tensor.height)));
    }
    if (tensor.width.has_value()) {
        object.insert(QStringLiteral("w"), QJsonValue(static_cast<double>(*tensor.width)));
    }
    return object;
}

/// The declared boundary of a graph, or the concrete boundary a CPU run proved.
/// Both are `input`/`output` with dtype, rank, N, C and H/W where they exist.
QJsonObject profileJson(const ModelProfile& profile) {
    QJsonObject object;
    object.insert(QStringLiteral("input"), tensorJson(profile.input));
    object.insert(QStringLiteral("output"), tensorJson(profile.output));
    return object;
}

QJsonObject resultJson(const QString& path, qint64 bytes, const QString& digest,
                       const InspectionResult& inspection, const QString& eligibility, int exit) {
    QJsonObject object;
    object.insert(QStringLiteral("path"), path);
    object.insert(QStringLiteral("bytes"), QJsonValue(static_cast<double>(bytes)));
    object.insert(QStringLiteral("sha256"), digest);
    object.insert(QStringLiteral("ok"), inspection.ok);
    object.insert(QStringLiteral("exit"), exit);
    if (!inspection.ok) {
        object.insert(QStringLiteral("reasonKey"), inspection.reasonKey);
        object.insert(QStringLiteral("reason"), inspection.reason);
        return object;
    }
    object.insert(QStringLiteral("inputName"), inspection.inputName);
    object.insert(QStringLiteral("outputName"), inspection.outputName);
    object.insert(QStringLiteral("declaredInput"), tensorJson(inspection.declared.input));
    object.insert(QStringLiteral("declaredOutput"), tensorJson(inspection.declared.output));
    object.insert(QStringLiteral("eligibility"), eligibility);
    QJsonObject reference;
    reference.insert(QStringLiteral("ran"), inspection.referenceRan);
    if (inspection.referenceRan) {
        reference.insert(QStringLiteral("runW"), inspection.runGeometry.width());
        reference.insert(QStringLiteral("runH"), inspection.runGeometry.height());
        if (inspection.resolvedGeometry.has_value()) {
            reference.insert(QStringLiteral("outW"), inspection.resolvedGeometry->width());
            reference.insert(QStringLiteral("outH"), inspection.resolvedGeometry->height());
        }
        if (inspection.resolvedScale.has_value()) {
            reference.insert(QStringLiteral("scale"), *inspection.resolvedScale);
        }
    }
    // The concrete boundary the run proved, separate from the declaration: the
    // declared input/output above keep the graph's own N (including a symbolic
    // -1), while this is what the CPU run actually produced.
    if (inspection.verifiedReferenceProfile.has_value()) {
        reference.insert(QStringLiteral("profile"),
                         profileJson(*inspection.verifiedReferenceProfile));
    }
    if (!inspection.referenceKey.isEmpty()) {
        reference.insert(QStringLiteral("reasonKey"), inspection.referenceKey);
        reference.insert(QStringLiteral("reason"), inspection.referenceReason);
    }
    object.insert(QStringLiteral("reference"), reference);
    return object;
}

bool readDtype(const QString& text, QString* dtype) {
    if (text == QLatin1String("fp16")) {
        *dtype = text;
        return true;
    }
    if (text == QLatin1String("fp32")) {
        *dtype = text;
        return true;
    }
    return false;
}

bool readTensorShape(const QString& text, QVector<qint64>* shape) {
    const QStringList parts = text.trimmed().split(QLatin1Char('x'));
    if (parts.size() != 4) return false;
    QVector<qint64> dims;
    dims.reserve(4);
    for (const QString& part : parts) {
        if (part.isEmpty()) return false;
        bool ok = false;
        const qint64 dim = part.toLongLong(&ok);
        if (!ok || dim <= 0 || dim > 2147483647) return false;
        dims.append(dim);
    }
    *shape = dims;
    return true;
}

bool readReferenceInput(const QStringList& values, Options::RawInput* input) {
    if (values.size() != 4) return false;
    if (!readDtype(values.at(1), &input->dtype)) return false;
    if (!readTensorShape(values.at(2), &input->shape)) return false;
    input->name = values.at(0);
    input->path = values.at(3);
    return !input->name.isEmpty() && !input->path.isEmpty();
}

/// "N/D" or "N" - the cadence the probe observed. Nothing is assumed.
bool readCadence(const QString& text, Rational* fps) {
    static const QRegularExpression pattern(QStringLiteral("^(\\d+)(?:/(\\d+))?$"));
    const QRegularExpressionMatch match = pattern.match(text.trimmed());
    if (!match.hasMatch()) return false;
    const qint64 num = match.captured(1).toLongLong();
    const qint64 den = match.captured(2).isEmpty() ? 1 : match.captured(2).toLongLong();
    if (num <= 0 || den <= 0 || num > 2147483647 || den > 2147483647) return false;
    *fps = Rational{static_cast<int>(num), static_cast<int>(den)};
    return true;
}

/// "decoderUuid:computeUuid:smMajor[.smMinor]" - what the probe observed. Nothing
/// is defaulted: an absent flag leaves the device unknown and the plan Preparing.
bool readDevice(const QString& text, DeviceIdentity* device) {
    const QStringList parts = text.split(QLatin1Char(':'));
    if (parts.size() != 3) return false;
    if (parts.at(0).isEmpty() || parts.at(1).isEmpty() || parts.at(2).isEmpty()) return false;

    static const QRegularExpression smPattern(QStringLiteral("^(\\d+)(?:\\.(\\d+))?$"));
    const QRegularExpressionMatch match = smPattern.match(parts.at(2));
    if (!match.hasMatch()) return false;
    const qint64 major = match.captured(1).toLongLong();
    const qint64 minor = match.captured(2).isEmpty() ? 0 : match.captured(2).toLongLong();
    if (major <= 0 || major > 999 || minor < 0 || minor > 999) return false;

    device->decoderDeviceUuid = parts.at(0);
    device->computeDeviceUuid = parts.at(1);
    device->smMajor = static_cast<int>(major);
    device->smMinor = static_cast<int>(minor);
    return true;
}

QJsonObject stageJson(const ResolvedStage& stage) {
    QJsonObject object;
    object.insert(QStringLiteral("kind"), stage.kind == StageKind::Temporal
                                             ? QStringLiteral("temporal")
                                             : QStringLiteral("spatial"));
    object.insert(QStringLiteral("role"), stableName(stage.role));
    object.insert(QStringLiteral("modelId"), stage.modelId);
    object.insert(QStringLiteral("contentId"), stage.modelContentId);
    object.insert(QStringLiteral("recipeIndex"), stage.recipeIndex);
    object.insert(QStringLiteral("inputShape"), stage.inputShape());
    object.insert(QStringLiteral("outputShape"), stage.outputShape());
    object.insert(QStringLiteral("inputW"), stage.inputGeometry.width());
    object.insert(QStringLiteral("inputH"), stage.inputGeometry.height());
    object.insert(QStringLiteral("outputW"), stage.outputGeometry.width());
    object.insert(QStringLiteral("outputH"), stage.outputGeometry.height());
    if (stage.scale.has_value()) object.insert(QStringLiteral("scale"), *stage.scale);
    object.insert(QStringLiteral("fromResolver"), stage.geometryFromResolver);
    object.insert(QStringLiteral("padL"), stage.padL);
    object.insert(QStringLiteral("padT"), stage.padT);
    object.insert(QStringLiteral("inputFps"), stage.inputFps.toString());
    object.insert(QStringLiteral("outputFps"), stage.outputFps.toString());
    return object;
}

QJsonObject planJson(const Recipe& recipe, const SourceGeometry& source, const PreparationPlan& plan,
                     const InspectedPlanResolver& resolver, bool resolverInvolved) {
    QJsonObject object;
    object.insert(QStringLiteral("recipe"), recipe.id);
    object.insert(QStringLiteral("backend"), stableName(plan.backend));
    if (source.codedSize.has_value()) {
        object.insert(QStringLiteral("sourceW"), source.codedSize->width());
        object.insert(QStringLiteral("sourceH"), source.codedSize->height());
    }
    if (source.fps.has_value()) object.insert(QStringLiteral("fps"), source.fps->toString());
    object.insert(QStringLiteral("deviceKnown"), plan.device.isKnown());
    if (plan.device.isKnown()) {
        object.insert(QStringLiteral("computeDeviceUuid"), plan.device.computeDeviceUuid);
        object.insert(QStringLiteral("smMajor"), plan.device.smMajor);
        object.insert(QStringLiteral("smMinor"), plan.device.smMinor);
    }
    object.insert(QStringLiteral("availability"), stableName(plan.availability.value));
    object.insert(QStringLiteral("reasonKey"), plan.availability.reasonKey);
    object.insert(QStringLiteral("reason"), plan.availability.reason);
    object.insert(QStringLiteral("resolved"), plan.isResolved());
    object.insert(QStringLiteral("planId"), plan.id);

    QJsonArray unknowns;
    for (const QString& unknown : plan.unknowns) unknowns.append(unknown);
    object.insert(QStringLiteral("unknowns"), unknowns);

    QJsonArray blockers;
    for (const PlanBlocker& blocker : plan.blockers) {
        QJsonObject entry;
        entry.insert(QStringLiteral("reasonKey"), blocker.reasonKey);
        entry.insert(QStringLiteral("reason"), blocker.reason);
        entry.insert(QStringLiteral("stageIndex"), blocker.stageIndex);
        blockers.append(entry);
    }
    object.insert(QStringLiteral("blockers"), blockers);

    QJsonArray stages;
    for (const ResolvedStage& stage : plan.stages) stages.append(stageJson(stage));
    object.insert(QStringLiteral("stages"), stages);

    QJsonArray resizes;
    for (const PreResizeStep& resize : plan.resizes) {
        QJsonObject entry;
        entry.insert(QStringLiteral("stageIndex"), resize.stageIndex);
        entry.insert(QStringLiteral("modelId"), resize.modelId);
        entry.insert(QStringLiteral("fromW"), resize.from.width());
        entry.insert(QStringLiteral("fromH"), resize.from.height());
        entry.insert(QStringLiteral("toW"), resize.to.width());
        entry.insert(QStringLiteral("toH"), resize.to.height());
        entry.insert(QStringLiteral("beforeTemporal"), resize.beforeTemporal);
        resizes.append(entry);
    }
    object.insert(QStringLiteral("resizes"), resizes);

    if (const std::optional<QSize> output = plan.outputGeometry(); output.has_value()) {
        object.insert(QStringLiteral("outputW"), output->width());
        object.insert(QStringLiteral("outputH"), output->height());
    }
    if (const std::optional<Rational> cadence = plan.outputCadence(); cadence.has_value()) {
        object.insert(QStringLiteral("outputFps"), cadence->toString());
    }
    // The planner's resolver channel carries no reason, so the adapter's own named
    // failure is reported beside the planner's `stage.dynamic.unresolved` blocker.
    if (resolverInvolved) {
        object.insert(QStringLiteral("resolverReasonKey"), resolver.lastReasonKey());
        object.insert(QStringLiteral("resolverReason"), resolver.lastReason());
    }
    return object;
}

QString shapeText(const QJsonArray& shape) {
    QStringList parts;
    for (const QJsonValue& dim : shape) {
        parts.append(QString::number(static_cast<qint64>(dim.toDouble())));
    }
    return parts.join(QLatin1Char('x'));
}

int runReferenceMode(const Options& options, const InspectionRuntime& runtime,
                     const QByteArray& graphBytes, const QString& graphPath, QTextStream& out,
                     QTextStream& err) {
    ReferenceExportRequest request;
    request.directory = options.referenceDir;
    request.fixturePath = options.fixture;
    request.resolveGeometry = options.resolveGeometry;
    for (const Options::RawInput& raw : options.rawInputs) {
        RawReferenceInput input;
        input.name = raw.name;
        input.dtype = raw.dtype;
        input.shape = raw.shape;
        input.filePath = raw.path;
        request.rawInputs.append(input);
    }

    ReferenceExport exported;
    QString reasonKey;
    QString reason;
    if (!exportCpuFrameReference(runtime, graphBytes, graphPath, request, &exported, &reasonKey,
                                 &reason)) {
        if (options.json) {
            QJsonObject object;
            object.insert(QStringLiteral("ok"), false);
            object.insert(QStringLiteral("exported"), false);
            object.insert(QStringLiteral("graphPath"), graphPath);
            object.insert(QStringLiteral("reasonKey"), reasonKey);
            object.insert(QStringLiteral("reason"), reason);
            out << QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)) << '\n';
        }
        err << "native_inspect: reference export refused: " << reasonKey << ": " << reason << '\n';
        return reasonKey == QLatin1String("inspect.runtime.unavailable") ? kExitRuntimeUnavailable
                                                                        : kExitRefused;
    }

    if (options.json) {
        QJsonObject object = exported.manifest;
        object.insert(QStringLiteral("ok"), true);
        object.insert(QStringLiteral("exported"), true);
        object.insert(QStringLiteral("directory"), exported.directory);
        object.insert(QStringLiteral("manifestPath"), exported.manifestPath);
        out << QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)) << '\n';
        return kExitOk;
    }

    out << "reference: " << exported.directory << '\n'
        << "  graph " << graphPath << "  sha256 " << exported.manifest.value("graphSha256").toString()
        << '\n';
    if (!exported.manifest.value("frameSha256").toString().isEmpty()) {
        out << "  frame " << exported.manifest.value("framePath").toString() << "  sha256 "
            << exported.manifest.value("frameSha256").toString() << '\n';
    }
    const QJsonArray inputs = exported.manifest.value("inputs").toArray();
    for (const QJsonValue& value : inputs) {
        const QJsonObject tensor = value.toObject();
        out << "  input  " << tensor.value("name").toString() << "  " << tensor.value("dtype").toString()
            << "  " << shapeText(tensor.value("shape").toArray()) << "  "
            << tensor.value("file").toString() << "  sha256 "
            << tensor.value("sha256").toString() << '\n';
    }
    const QJsonArray outputs = exported.manifest.value("outputs").toArray();
    for (const QJsonValue& value : outputs) {
        const QJsonObject tensor = value.toObject();
        out << "  output " << tensor.value("name").toString() << "  " << tensor.value("dtype").toString()
            << "  " << shapeText(tensor.value("shape").toArray()) << "  "
            << tensor.value("file").toString() << "  sha256 "
            << tensor.value("sha256").toString() << '\n';
    }
    out << "  manifest " << exported.manifestPath << '\n';
    return kExitOk;
}

/// `--plan-recipe`: resolve an ordered recipe against the owned, inspected store.
/// The planner stays Preparing until the source and the device are observed; no
/// device is invented to make it look Ready.
int runPlanMode(const Options& options, const InspectionRuntime& runtime,
                const BackendAvailability& backends, QTextStream& out, QTextStream& err) {
    QFile recipeFile(options.planRecipe);
    if (!recipeFile.open(QIODevice::ReadOnly)) {
        err << "native_inspect: recipe " << options.planRecipe
            << " cannot be read: " << recipeFile.errorString() << '\n';
        return kExitUsage;
    }
    const QByteArray payload = recipeFile.readAll();
    recipeFile.close();

    Recipe recipe;
    QString recipeKey;
    QString recipeReason;
    if (!parseRecipe(payload, &recipe, &recipeKey, &recipeReason)) {
        err << "native_inspect: recipe refused: " << recipeKey << ": " << recipeReason << '\n';
        return kExitRefused;
    }

    SourceGeometry source;
    if (options.sourceGeometry.isValid()) source.codedSize = options.sourceGeometry;
    if (options.fps.has_value()) source.fps = options.fps;

    InspectedPlanResolver resolver(options.storeRoot, runtime);
    QString storeKey;
    QString storeReason;
    if (!resolver.refresh(&storeKey, &storeReason)) {
        err << "native_inspect: model store " << options.storeRoot << ": " << storeKey << ": "
            << storeReason << '\n';
        return storeKey == QLatin1String("inspect.runtime.unavailable") ? kExitRuntimeUnavailable
                                                                        : kExitRefused;
    }

    PlanRequest request;
    request.recipe = recipe;
    request.catalog = resolver.catalog();
    request.backends = backends;
    request.source = source;
    request.device = options.device;
    request.window = ChainWindow();
    request.dynamicResolver = resolver.resolver();

    const PreparationPlan plan = resolvePreparationPlan(request);
    const bool resolverInvolved =
        !plan.blockers.isEmpty() &&
        plan.blockers.first().reasonKey == QLatin1String("stage.dynamic.unresolved");

    int exit = kExitOk;
    if (!plan.blockers.isEmpty()) {
        exit = kExitRefused;
    } else if (!plan.isResolved()) {
        exit = kExitPreparing;
    }

    if (options.json) {
        out << QString::fromUtf8(
                   QJsonDocument(planJson(recipe, source, plan, resolver, resolverInvolved))
                       .toJson(QJsonDocument::Compact))
            << '\n';
        return exit;
    }

    out << "plan:   recipe " << recipe.id << "  backend " << stableName(plan.backend) << '\n';
    if (source.codedSize.has_value()) {
        out << "  source " << source.codedSize->width() << 'x' << source.codedSize->height();
        if (source.fps.has_value()) out << " @ " << source.fps->toString();
        out << '\n';
    } else {
        out << "  source: not observed (pass --source WxH and --fps N/D)\n";
    }
    if (plan.device.isKnown()) {
        out << "  device " << plan.device.decoderDeviceUuid << " / " << plan.device.computeDeviceUuid
            << " sm " << plan.device.smMajor << '.' << plan.device.smMinor << '\n';
    } else {
        out << "  device: not observed (pass --device decoderUuid:computeUuid:smMajor[.smMinor])\n";
    }
    out << "  availability " << stableName(plan.availability.value) << " ("
        << plan.availability.reasonKey << "): " << plan.availability.reason << '\n';
    for (const QString& unknown : plan.unknowns) out << "  waiting on: " << unknown << '\n';
    for (const PlanBlocker& blocker : plan.blockers) {
        out << "  blocker " << blocker.reasonKey;
        if (blocker.stageIndex >= 0) out << " (stage " << blocker.stageIndex << ')';
        out << ": " << blocker.reason << '\n';
    }
    if (resolverInvolved && !resolver.lastReasonKey().isEmpty()) {
        out << "  resolver: " << resolver.lastReasonKey() << ": " << resolver.lastReason() << '\n';
    }
    for (const ResolvedStage& stage : plan.stages) {
        out << "  stage " << stage.recipeIndex << ' ' << stableName(stage.role) << ' ' << stage.modelId
            << "  " << stage.inputShape() << " -> " << stage.outputShape();
        if (stage.scale.has_value()) out << "  scale " << *stage.scale;
        out << (stage.geometryFromResolver ? "  (resolver)\n" : "  (declared)\n");
    }
    for (const PreResizeStep& resize : plan.resizes) {
        out << "  resize " << resize.modelId << "  " << resize.from.width() << 'x'
            << resize.from.height() << " -> " << resize.to.width() << 'x' << resize.to.height()
            << (resize.beforeTemporal ? "  (before interpolation)" : "") << '\n';
    }
    if (const std::optional<QSize> output = plan.outputGeometry(); output.has_value()) {
        out << "  output " << output->width() << 'x' << output->height();
        if (const std::optional<Rational> cadence = plan.outputCadence(); cadence.has_value()) {
            out << " @ " << cadence->toString();
        }
        out << '\n';
    }
    return exit;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mikai_native_inspect"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Inspect ONNX graphs through the official ONNX Runtime C API: declared "
                       "I/O, spatial eligibility and an optional CPU reference run.\n"
                       "--reference-dir additionally reads --reference-input <name> <fp16|fp32> "
                       "<NxCxHxW> <raw.bin>, repeatable; the set must be exactly the graph's real "
                       "inputs and the bytes are bound and exported verbatim."));
    parser.addHelpOption();
    const QCommandLineOption runtimeOption(
        QStringLiteral("runtime"),
        QStringLiteral("Directory holding onnxruntime.dll. Defaults to MIKAI_ORT_RUNTIME_DIR. "
                       "The process PATH is never searched."),
        QStringLiteral("dir"));
    const QCommandLineOption resolveOption(
        QStringLiteral("resolve"),
        QStringLiteral("Run one fp16 CPU reference at WxH and report the geometry produced."),
        QStringLiteral("WxH"));
    const QCommandLineOption publishOption(
        QStringLiteral("publish"),
        QStringLiteral("Publish each accepted graph into the app-owned model store."));
    const QCommandLineOption storeOption(
        QStringLiteral("store"), QStringLiteral("Model store root (must be app-owned)."),
        QStringLiteral("dir"));
    const QCommandLineOption backendOption(
        QStringLiteral("backend"),
        QStringLiteral("A backend the runtime carries (tensorrt|directml). Repeatable."),
        QStringLiteral("name"));
    const QCommandLineOption jsonOption(QStringLiteral("json"), QStringLiteral("Machine-readable output."));
    const QCommandLineOption planRecipeOption(
        QStringLiteral("plan-recipe"),
        QStringLiteral("Resolve an ordered recipe (JSON file) against the owned model store and "
                       "report each stage's actual geometry. Takes no model arguments."),
        QStringLiteral("file"));
    const QCommandLineOption sourceOption(
        QStringLiteral("source"),
        QStringLiteral("The display geometry the decoder observed (WxH), for --plan-recipe. Without "
                       "it the plan stays Preparing."),
        QStringLiteral("WxH"));
    const QCommandLineOption fpsOption(
        QStringLiteral("fps"),
        QStringLiteral("The cadence the decoder observed (N/D or N), for --plan-recipe."),
        QStringLiteral("N/D"));
    const QCommandLineOption deviceOption(
        QStringLiteral("device"),
        QStringLiteral("The device the probe observed "
                       "(decoderUuid:computeUuid:smMajor[.smMinor]), for --plan-recipe. Without it "
                       "the device is unknown and is never invented."),
        QStringLiteral("spec"));
    parser.addOption(runtimeOption);
    parser.addOption(resolveOption);
    parser.addOption(publishOption);
    parser.addOption(storeOption);
    parser.addOption(backendOption);
    parser.addOption(jsonOption);
    parser.addOption(planRecipeOption);
    parser.addOption(sourceOption);
    parser.addOption(fpsOption);
    parser.addOption(deviceOption);
    const QCommandLineOption referenceDirOption(
        QStringLiteral("reference-dir"),
        QStringLiteral("Run one real CPU reference and write its exact input bytes, output bytes "
                       "and manifest into this NEW directory. Takes one model argument and no "
                       "--publish/--plan-recipe. An existing directory is refused."),
        QStringLiteral("dir"));
    const QCommandLineOption fixtureOption(
        QStringLiteral("fixture"),
        QStringLiteral("A binary P6 PPM frame to feed, for --reference-dir. Supplies the one "
                       "1x3xHxW fp16 spatial input a spatial graph declares; its geometry must "
                       "equal --resolve."),
        QStringLiteral("file"));
    parser.addOption(referenceDirOption);
    parser.addOption(fixtureOption);
    parser.addPositionalArgument(QStringLiteral("model"),
                                 QStringLiteral("One or more .onnx files to inspect."));
    // Four values per occurrence, which QCommandLineParser cannot bind: it would
    // take the first as the option value and leave the rest positional.
    QVector<Options::RawInput> rawInputs;
    QStringList parserArgs;
    {
        const QStringList rawArgs = QCoreApplication::arguments();
        // argv[0] must be present or QCommandLineParser will treat the first real
        // argument (the .onnx path) as the program name and drop it.
        parserArgs.append(rawArgs.value(0));
        for (qsizetype i = 1; i < rawArgs.size(); ++i) {
            if (rawArgs.at(i) != QStringLiteral("--reference-input")) {
                parserArgs.append(rawArgs.at(i));
                continue;
            }
            if (i + 4 >= rawArgs.size()) {
                QTextStream(stderr) << "native_inspect: --reference-input takes <name> "
                                       "<fp16|fp32> <NxCxHxW> <raw.bin>\n";
                return kExitUsage;
            }
            Options::RawInput input;
            if (!readReferenceInput(rawArgs.mid(i + 1, 4), &input)) {
                QTextStream(stderr) << "native_inspect: --reference-input takes <name> "
                                       "<fp16|fp32> <NxCxHxW> <raw.bin>, every dimension "
                                       "strictly positive\n";
                return kExitUsage;
            }
            rawInputs.append(input);
            i += 4;
        }
    }
    if (!parser.parse(parserArgs)) {
        if (parser.isSet(QStringLiteral("help"))) {
            QTextStream out(stdout);
            out << parser.helpText();
            out.flush();
            return kExitOk;
        }
        QTextStream(stderr) << parser.errorText() << '\n';
        return kExitUsage;
    }

    QTextStream out(stdout);
    QTextStream err(stderr);

    Options options;
    options.runtimeDirectory = parser.isSet(runtimeOption) ? parser.value(runtimeOption)
                                                           : qEnvironmentVariable("MIKAI_ORT_RUNTIME_DIR");
    if (parser.isSet(resolveOption)) {
        options.resolveRequested = true;
        if (!readGeometry(parser.value(resolveOption), &options.resolveGeometry)) {
            err << "native_inspect: --resolve takes WxH, e.g. 320x180\n";
            return kExitUsage;
        }
    }
    options.publish = parser.isSet(publishOption);
    options.storeRoot = parser.isSet(storeOption)
                            ? parser.value(storeOption)
                            : ModelInventory::defaultRoot();
    for (const QString& name : parser.values(backendOption)) {
        const std::optional<Backend> backend = backendFromName(name);
        if (!backend.has_value()) {
            err << "native_inspect: '" << name << "' is not a backend\n";
            return kExitUsage;
        }
        options.backends.append(*backend);
    }
    options.json = parser.isSet(jsonOption);
    options.planRecipe = parser.isSet(planRecipeOption) ? parser.value(planRecipeOption) : QString();
    if (parser.isSet(sourceOption) &&
        !readGeometry(parser.value(sourceOption), &options.sourceGeometry)) {
        err << "native_inspect: --source takes WxH, e.g. 1920x1080\n";
        return kExitUsage;
    }
    if (parser.isSet(fpsOption)) {
        Rational fps;
        if (!readCadence(parser.value(fpsOption), &fps)) {
            err << "native_inspect: --fps takes N/D or N, e.g. 24000/1001\n";
            return kExitUsage;
        }
        options.fps = fps;
    }
    if (parser.isSet(deviceOption) && !readDevice(parser.value(deviceOption), &options.device)) {
        err << "native_inspect: --device takes decoderUuid:computeUuid:smMajor[.smMinor]\n";
        return kExitUsage;
    }
    options.referenceDir = parser.isSet(referenceDirOption) ? parser.value(referenceDirOption)
                                                             : QString();
    options.fixture = parser.isSet(fixtureOption) ? parser.value(fixtureOption) : QString();
    options.rawInputs = rawInputs;
    options.models = parser.positionalArguments();
    if (options.planRecipe.isEmpty() && options.models.isEmpty()) {
        err << "native_inspect: name at least one .onnx file, or pass --plan-recipe\n";
        return kExitUsage;
    }
    if (!options.planRecipe.isEmpty() && !options.models.isEmpty()) {
        err << "native_inspect: --plan-recipe resolves the owned store and takes no model arguments\n";
        return kExitUsage;
    }

    if (!options.referenceDir.isEmpty()) {
        if (!options.planRecipe.isEmpty()) {
            err << "native_inspect: --reference-dir exports one reference and takes no --plan-recipe\n";
            return kExitUsage;
        }
        if (options.publish) {
            err << "native_inspect: --reference-dir exports one reference and takes no --publish\n";
            return kExitUsage;
        }
        if (options.models.size() != 1) {
            err << "native_inspect: --reference-dir names exactly one .onnx file\n";
            return kExitUsage;
        }
        if (options.fixture.isEmpty() == options.rawInputs.isEmpty()) {
            err << "native_inspect: --reference-dir takes either --fixture or one or more "
                   "--reference-input\n";
            return kExitUsage;
        }
        if (!options.fixture.isEmpty() && !options.resolveRequested) {
            err << "native_inspect: --fixture needs --resolve WxH, the geometry the frame is read "
                   "at\n";
            return kExitUsage;
        }
        if (!options.rawInputs.isEmpty() && options.resolveRequested) {
            err << "native_inspect: --reference-input supplies its own shapes; it takes no --resolve\n";
            return kExitUsage;
        }
    } else if (!options.fixture.isEmpty() || !options.rawInputs.isEmpty()) {
        err << "native_inspect: --fixture and --reference-input are read only for --reference-dir\n";
        return kExitUsage;
    }

    const InspectionRuntime runtime = inspectionRuntimeAt(options.runtimeDirectory);
    QString runtimeReason;
    const bool runtimePresent = inspectionRuntimeIsPresent(runtime, &runtimeReason);

    BackendAvailability backends;
    backends.available = options.backends;

    ModelInventory inventory(options.storeRoot);
    if (options.publish) {
        QString storeReason;
        if (!inventory.isOwned(&storeReason)) {
            err << "native_inspect: model store root " << inventory.root() << " is not app-owned: "
                << storeReason << '\n';
            return kExitUsage;
        }
    }

    // --plan-recipe is a whole run of its own: it resolves the owned store, not
    // the positional files.
    if (!options.planRecipe.isEmpty()) {
        return runPlanMode(options, runtime, backends, out, err);
    }

    int exit = kExitOk;
    QJsonArray jsonResults;
    for (const QString& path : options.models) {
        const QFileInfo info(QDir::cleanPath(path));
        QFile file(info.absoluteFilePath());
        QByteArray bytes;
        if (!info.isAbsolute() || !info.isFile() || !file.open(QIODevice::ReadOnly)) {
            err << "native_inspect: " << path << " is not a readable file\n";
            exit = (exit == kExitOk) ? kExitUsage : exit;
            continue;
        }
        bytes = file.readAll();
        const qint64 byteCount = bytes.size();
        const QString digest = sha256HexOf(bytes);
        file.close();

        if (!options.referenceDir.isEmpty()) {
            if (!runtimePresent) {
                if (options.json) {
                    QJsonObject object;
                    object.insert(QStringLiteral("ok"), false);
                    object.insert(QStringLiteral("exported"), false);
                    object.insert(QStringLiteral("graphPath"), info.absoluteFilePath());
                    object.insert(QStringLiteral("runtimeUnavailable"), true);
                    object.insert(QStringLiteral("reasonKey"),
                                  QStringLiteral("inspect.runtime.unavailable"));
                    object.insert(QStringLiteral("reason"), runtimeReason);
                    out << QString::fromUtf8(
                               QJsonDocument(object).toJson(QJsonDocument::Compact))
                        << '\n';
                }
                err << "native_inspect: ONNX Runtime unavailable: " << runtimeReason << '\n';
                return kExitRuntimeUnavailable;
            }
            return runReferenceMode(options, runtime, bytes, info.absoluteFilePath(), out, err);
        }

        if (!runtimePresent) {
            // A named absence is the whole answer: no metadata is invented.
            if (options.json) {
                QJsonObject object;
                object.insert(QStringLiteral("path"), info.absoluteFilePath());
                object.insert(QStringLiteral("bytes"), QJsonValue(static_cast<double>(byteCount)));
                object.insert(QStringLiteral("sha256"), digest);
                object.insert(QStringLiteral("ok"), false);
                object.insert(QStringLiteral("runtimeUnavailable"), true);
                object.insert(QStringLiteral("reasonKey"), QStringLiteral("inspect.runtime.unavailable"));
                object.insert(QStringLiteral("reason"), runtimeReason);
                jsonResults.append(object);
            } else {
                out << "model:  " << info.absoluteFilePath() << '\n'
                    << "  bytes  " << byteCount << '\n'
                    << "  sha256 " << digest << '\n'
                    << "  runtime unavailable: " << runtimeReason << '\n'
                    << "  declared metadata not read (no runtime)\n";
            }
            if (exit == kExitOk) exit = kExitRuntimeUnavailable;
            continue;
        }

        const InspectionResult inspection = inspectGraph(runtime, bytes,
                                                        options.resolveRequested
                                                            ? options.resolveGeometry
                                                            : QSize());
        QString eligibility;
        int modelExit = kExitOk;
        if (!inspection.ok) {
            eligibility = QStringLiteral("refused");
            modelExit = kExitRefused;
        } else {
            // Eligibility is decided on the actual boundary. The strict declared
            // rule is enough only when the declaration vouches for itself; a
            // candidate (a symbolic output batch, or a dynamic geometry) is
            // eligible only with the concrete boundary a real CPU run proved.
            const QString strictRefusal = spatialEligibilityRefusal(inspection.declared);
            const QString candidateRefusal = spatialCandidateRefusal(inspection.declared);
            if (strictRefusal.isEmpty()) {
                eligibility = QStringLiteral("eligible");
            } else if (!isEligibleIoType(inspection.declared.input.dtype) ||
                       !isEligibleIoType(inspection.declared.output.dtype)) {
                eligibility = QStringLiteral("unsupported dtype: %1").arg(candidateRefusal);
                modelExit = kExitRefused;
            } else if (candidateRefusal.isEmpty() &&
                       inspection.verifiedReferenceProfile.has_value() &&
                       spatialReferenceRefusal(inspection.declared,
                                               *inspection.verifiedReferenceProfile)
                           .isEmpty()) {
                eligibility = QStringLiteral("eligible (verified reference)");
            } else {
                eligibility = QStringLiteral("ineligible: %1")
                                  .arg(candidateRefusal.isEmpty()
                                           ? QStringLiteral("a real CPU reference run is required to "
                                                            "prove a concrete N=1/C=3 output")
                                           : candidateRefusal);
                modelExit = kExitRefused;
            }
        }

        StoredModel stored;
        bool published = false;
        if (options.publish && inspection.ok) {
            const ModelImportOutcome outcome =
                inventory.publish(bytes, inspection, info.absoluteFilePath(),
                                  info.completeBaseName(), backends, QString());
            published = outcome.published;
            stored = outcome.model;
            if (!outcome.ok) modelExit = kExitRefused;
        }

        if (options.json) {
            QJsonObject object = resultJson(info.absoluteFilePath(), byteCount, digest, inspection,
                                            eligibility, modelExit);
            if (options.publish && inspection.ok) {
                object.insert(QStringLiteral("published"), published);
                object.insert(QStringLiteral("modelId"), stored.record.id);
                object.insert(QStringLiteral("availability"),
                              stableName(stored.record.availability.value));
                if (!stored.record.availability.reasonKey.isEmpty()) {
                    object.insert(QStringLiteral("availabilityReasonKey"),
                                  stored.record.availability.reasonKey);
                    object.insert(QStringLiteral("availabilityReason"),
                                  stored.record.availability.reason);
                }
            }
            jsonResults.append(object);
        } else {
            out << "model:  " << info.absoluteFilePath() << '\n'
                << "  bytes  " << byteCount << '\n'
                << "  sha256 " << digest << '\n';
            if (!inspection.ok) {
                out << "  refused: " << inspection.reasonKey << ": " << inspection.reason << '\n';
            } else {
                out << "  input   " << tensorText(inspection.declared.input) << '\n'
                    << "  output  " << tensorText(inspection.declared.output) << '\n'
                    << "  eligibility: " << eligibility << '\n';
                if (inspection.referenceRan) {
                    out << "  reference: ran 1x3x" << inspection.runGeometry.height() << 'x'
                        << inspection.runGeometry.width() << " -> ";
                    if (inspection.resolvedGeometry.has_value()) {
                        out << "1x3x" << inspection.resolvedGeometry->height() << 'x'
                            << inspection.resolvedGeometry->width();
                    } else {
                        out << "(no geometry)";
                    }
                    if (inspection.resolvedScale.has_value()) {
                        out << ", scale " << *inspection.resolvedScale;
                    }
                    out << '\n';
                } else if (!inspection.referenceKey.isEmpty()) {
                    out << "  reference: not run: " << inspection.referenceKey << ": "
                        << inspection.referenceReason << '\n';
                } else if (inspection.declared.isDynamicGeometry()) {
                    out << "  reference: not requested (pass --resolve WxH for this dynamic graph)\n";
                } else {
                    out << "  reference: not needed (fixed declared geometry)\n";
                }
                if (inspection.verifiedReferenceProfile.has_value()) {
                    out << "  verified: "
                        << tensorText(inspection.verifiedReferenceProfile->input) << " -> "
                        << tensorText(inspection.verifiedReferenceProfile->output) << '\n';
                }
                if (options.publish) {
                    out << "  published: " << (published ? "new copy" : "existing copy")
                        << "  id " << stored.record.id << "  availability "
                        << stableName(stored.record.availability.value);
                    if (!stored.record.availability.reasonKey.isEmpty()) {
                        out << " (" << stored.record.availability.reasonKey << ": "
                            << stored.record.availability.reason << ')';
                    }
                    out << '\n';
                }
            }
        }
        if (modelExit != kExitOk && exit == kExitOk) exit = modelExit;
    }

    if (options.json) {
        out << QString::fromUtf8(QJsonDocument(jsonResults).toJson(QJsonDocument::Compact)) << '\n';
    }
    if (exit == kExitRuntimeUnavailable) {
        err << "native_inspect: ONNX Runtime unavailable: " << runtimeReason << '\n';
    }
    return exit;
}
