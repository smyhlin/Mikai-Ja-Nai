// mikai_native_preparation_references_probe - the hand-run harness around the
// whole-plan CPU reference lineage worker.
//
// This is not a second pipeline. It resolves a plan through the real planner over
// the app-owned model store, hands `buildPlanReferenceLineage` the real observed
// facts and real PPM frames, and reports exactly what that call produced: one
// genuine fp16 input file per stage, in stage order, with the digest a human can
// compare against the builder's own oracle run.
//
//   native_preparation_references_probe --inspect <native_inspect.exe>
//       --runtime <onnxruntime dir> --store <owned model root> --recipe <recipe.json>
//       --source WxH --fps N/D --device decoderUuid:computeUuid:smMajor[.smMinor]
//       --frame <frame.ppm> [--frame <second.ppm>] --scratch <new directory>
//       [--backend tensorrt|directml ...] [--trt-bin <dir>] [--cuda-bin <dir>]
//       [--scenario lineage|cancel] [--app-org <org>] [--app-name <name>] [--json]
//
// Nothing is defaulted: an unobserved device, a missing tool, an existing scratch
// directory or an unresolved plan refuses by name before any graph runs.
//
// Exit: 0 the lineage was produced and every stage input verified; 1 the scenario
// failed or was refused; 3 usage or a request this build cannot read; 4 the plan
// is still Preparing.

#include <optional>

#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QStringList>
#include <QtCore/QTextStream>

#include "native/prepare/NativeFrameReference.hpp"
#include "native/models/NativeInspectedPlan.hpp"
#include "native/models/NativeModelInspect.hpp"
#include "native/models/NativeModelInventory.hpp"
#include "native/prepare/NativePreparationPlan.hpp"
#include "native/prepare/NativePreparationReferences.hpp"
#include "native/models/NativeRecipe.hpp"

using namespace mplayer;
using namespace mplayer::native;

namespace {

constexpr int kExitPass = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 3;
constexpr int kExitPreparing = 4;

bool readGeometry(const QString& text, QSize* size) {
  const QStringList parts = text.split(QLatin1Char('x'));
  if (parts.size() != 2) return false;
  bool okWidth = false;
  bool okHeight = false;
  const int width = parts.at(0).toInt(&okWidth);
  const int height = parts.at(1).toInt(&okHeight);
  if (!okWidth || !okHeight || width <= 0 || height <= 0) return false;
  *size = QSize(width, height);
  return true;
}

bool readCadence(const QString& text, Rational* fps) {
  const QStringList parts = text.split(QLatin1Char('/'));
  bool okNum = false;
  const int num = parts.at(0).toInt(&okNum);
  int den = 1;
  bool okDen = true;
  if (parts.size() == 2) den = parts.at(1).toInt(&okDen);
  if (parts.size() > 2 || !okNum || !okDen) return false;
  fps->num = num;
  fps->den = den;
  return fps->isValid();
}

bool readDevice(const QString& text, DeviceIdentity* device) {
  const QStringList parts = text.split(QLatin1Char(':'));
  if (parts.size() != 3) return false;
  device->decoderDeviceUuid = parts.at(0);
  device->computeDeviceUuid = parts.at(1);
  const QStringList sm = parts.at(2).split(QLatin1Char('.'));
  bool okMajor = false;
  const int major = sm.at(0).toInt(&okMajor);
  int minor = 0;
  bool okMinor = true;
  if (sm.size() == 2) minor = sm.at(1).toInt(&okMinor);
  if (sm.size() > 2 || !okMajor || !okMinor || major <= 0 || minor < 0) return false;
  device->smMajor = major;
  device->smMinor = minor;
  return device->isKnown();
}

QJsonObject referenceJson(const BuilderStageReference& reference) {
  QJsonArray raw;
  for (const BuilderRawInput& input : reference.rawInputs) {
    QJsonObject entry;
    entry.insert(QStringLiteral("name"), input.name);
    entry.insert(QStringLiteral("dtype"), input.dtype);
    entry.insert(QStringLiteral("shape"), input.shape);
    entry.insert(QStringLiteral("file"), input.filePath);
    QFile file(input.filePath);
    if (file.open(QIODevice::ReadOnly)) {
      const QByteArray bytes = file.readAll();
      entry.insert(QStringLiteral("bytes"), static_cast<double>(bytes.size()));
      entry.insert(QStringLiteral("sha256"), sha256HexOf(bytes));
    }
    raw.append(entry);
  }
  QJsonObject object;
  object.insert(QStringLiteral("fixture"), reference.fixturePath);
  object.insert(QStringLiteral("rawInputs"), raw);
  return object;
}

int reportRefusal(const QString& reasonKey, const QString& reason, int exitCode) {
  QTextStream err(stderr);
  err << "native_preparation_references_probe: refused " << reasonKey << ": " << reason << '\n';
  err.flush();
  return exitCode;
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("Mikai"));
  QCoreApplication::setApplicationName(QStringLiteral("Mikai Wrapper"));

  QCommandLineParser parser;
  parser.setApplicationDescription(
      QStringLiteral("Hand-run harness around buildPlanReferenceLineage. Resolves a plan through the "
                     "real planner over the owned model store, runs the real inspector for every "
                     "stage whose output a later stage consumes, and reports the genuine fp16 input "
                     "lineage it produced. Nothing is defaulted."));
  parser.addHelpOption();
  const QCommandLineOption inspectOption(QStringLiteral("inspect"),
                                         QStringLiteral("native_inspect executable."),
                                         QStringLiteral("file"));
  const QCommandLineOption runtimeOption(QStringLiteral("runtime"),
                                         QStringLiteral("Directory holding onnxruntime.dll."),
                                         QStringLiteral("dir"));
  const QCommandLineOption storeOption(QStringLiteral("store"),
                                       QStringLiteral("Owned model store root."),
                                       QStringLiteral("dir"));
  const QCommandLineOption recipeOption(QStringLiteral("recipe"),
                                        QStringLiteral("Recipe JSON file."),
                                        QStringLiteral("file"));
  const QCommandLineOption sourceOption(QStringLiteral("source"),
                                        QStringLiteral("The decoder's display geometry (WxH)."),
                                        QStringLiteral("WxH"));
  const QCommandLineOption fpsOption(QStringLiteral("fps"),
                                     QStringLiteral("The decoder's cadence (N/D or N)."),
                                     QStringLiteral("N/D"));
  const QCommandLineOption deviceOption(
      QStringLiteral("device"),
      QStringLiteral("decoderUuid:computeUuid:smMajor[.smMinor]."), QStringLiteral("spec"));
  const QCommandLineOption frameOption(
      QStringLiteral("frame"),
      QStringLiteral("A real P6 PPM frame. Repeat for the second frame an interpolation stage "
                     "needs."),
      QStringLiteral("file"));
  const QCommandLineOption scratchOption(
      QStringLiteral("scratch"),
      QStringLiteral("A NEW directory the probe creates and owns; an existing one is refused."),
      QStringLiteral("dir"));
  const QCommandLineOption backendOption(
      QStringLiteral("backend"),
      QStringLiteral("A backend the runtime carries (tensorrt|directml). Repeatable."),
      QStringLiteral("name"));
  const QCommandLineOption trtOption(QStringLiteral("trt-bin"),
                                     QStringLiteral("Observed TensorRT bin directory."),
                                     QStringLiteral("dir"));
  const QCommandLineOption cudaOption(QStringLiteral("cuda-bin"),
                                      QStringLiteral("Observed CUDA bin directory."),
                                      QStringLiteral("dir"));
  const QCommandLineOption scenarioOption(
      QStringLiteral("scenario"),
      QStringLiteral("lineage|cancel; cancel proves a cancelled run sends nothing."),
      QStringLiteral("name"), QStringLiteral("lineage"));
  const QCommandLineOption orgOption(
      QStringLiteral("app-org"),
      QStringLiteral("Organization that owns the app-local data tree."), QStringLiteral("org"),
      QStringLiteral("Mikai"));
  const QCommandLineOption nameOption(QStringLiteral("app-name"),
                                      QStringLiteral("Application name that owns the app-local data "
                                                     "tree."),
                                      QStringLiteral("name"), QStringLiteral("Mikai Wrapper"));
  const QCommandLineOption jsonOption(QStringLiteral("json"),
                                      QStringLiteral("Machine-readable result."));
  parser.addOptions(QList<QCommandLineOption>{
      inspectOption, runtimeOption, storeOption, recipeOption, sourceOption, fpsOption,
      deviceOption, frameOption, scratchOption, backendOption, trtOption, cudaOption,
      scenarioOption, orgOption, nameOption, jsonOption});
  parser.process(app);
  QTextStream out(stdout);

  QCoreApplication::setOrganizationName(parser.value(orgOption));
  QCoreApplication::setApplicationName(parser.value(nameOption));

  const QString scenario = parser.value(scenarioOption);
  if (scenario != QLatin1String("lineage") && scenario != QLatin1String("cancel")) {
    return reportRefusal(QStringLiteral("probe.scenario"),
                         QStringLiteral("--scenario takes lineage or cancel"), kExitUsage);
  }
  for (const QCommandLineOption& required :
       QList<QCommandLineOption>{inspectOption, runtimeOption, storeOption, recipeOption,
                                 sourceOption, fpsOption, deviceOption, frameOption, scratchOption}) {
    if (!parser.isSet(required)) {
      return reportRefusal(QStringLiteral("probe.member"),
                           QStringLiteral("--%1 is required").arg(required.names().first()),
                           kExitUsage);
    }
  }

  QSize sourceGeometry;
  Rational fps;
  DeviceIdentity device;
  if (!readGeometry(parser.value(sourceOption), &sourceGeometry)) {
    return reportRefusal(QStringLiteral("probe.source"), QStringLiteral("--source takes WxH"),
                         kExitUsage);
  }
  if (!readCadence(parser.value(fpsOption), &fps)) {
    return reportRefusal(QStringLiteral("probe.fps"), QStringLiteral("--fps takes N/D or N"),
                         kExitUsage);
  }
  if (!readDevice(parser.value(deviceOption), &device)) {
    return reportRefusal(QStringLiteral("probe.device"),
                         QStringLiteral("--device takes decoderUuid:computeUuid:smMajor[.smMinor]"),
                         kExitUsage);
  }

  BackendAvailability backends;
  for (const QString& name : parser.values(backendOption)) {
    const std::optional<Backend> backend = backendFromName(name);
    if (!backend.has_value()) {
      return reportRefusal(QStringLiteral("probe.backend"),
                           QStringLiteral("'%1' is not a backend").arg(name), kExitUsage);
    }
    backends.available.append(*backend);
  }

  const QFileInfo scratchInfo(QDir::cleanPath(parser.value(scratchOption)));
  if (!scratchInfo.isAbsolute()) {
    return reportRefusal(QStringLiteral("probe.scratch"),
                         QStringLiteral("--scratch must be absolute"), kExitUsage);
  }
  if (scratchInfo.exists()) {
    return reportRefusal(QStringLiteral("probe.scratch.exists"),
                         QStringLiteral("'%1' already exists; the probe writes a fresh directory")
                             .arg(scratchInfo.absoluteFilePath()),
                         kExitUsage);
  }

  QFile recipeFile(parser.value(recipeOption));
  if (!recipeFile.open(QIODevice::ReadOnly)) {
    return reportRefusal(QStringLiteral("probe.recipe"),
                         QStringLiteral("the recipe cannot be read: %1").arg(recipeFile.errorString()),
                         kExitUsage);
  }
  const QByteArray recipeBytes = recipeFile.readAll();
  recipeFile.close();
  Recipe recipe;
  QString recipeKey;
  QString recipeReason;
  if (!parseRecipe(recipeBytes, &recipe, &recipeKey, &recipeReason)) {
    return reportRefusal(recipeKey, recipeReason, kExitUsage);
  }

  const InspectionRuntime runtime = inspectionRuntimeAt(parser.value(runtimeOption));
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(runtime, &runtimeReason)) {
    return reportRefusal(QStringLiteral("inspect.runtime.unavailable"), runtimeReason,
                         kExitFailed);
  }
  InspectedPlanResolver resolver(parser.value(storeOption), runtime);
  QString storeKey;
  QString storeReason;
  if (!resolver.refresh(&storeKey, &storeReason)) {
    return reportRefusal(storeKey, storeReason, kExitFailed);
  }

  PlanRequest planRequest;
  planRequest.recipe = recipe;
  planRequest.catalog = resolver.catalog();
  planRequest.backends = backends;
  planRequest.source.codedSize = sourceGeometry;
  planRequest.source.fps = fps;
  planRequest.device = device;
  planRequest.window = ChainWindow();
  planRequest.dynamicResolver = resolver.resolver();
  const PreparationPlan plan = resolvePreparationPlan(planRequest);
  if (!plan.blockers.isEmpty()) {
    return reportRefusal(plan.blockers.first().reasonKey, plan.blockers.first().reason, kExitFailed);
  }
  if (!plan.isResolved()) {
    return reportRefusal(QStringLiteral("plan.preparing"),
                         plan.unknowns.isEmpty() ? QStringLiteral("the plan is not resolved")
                                                 : plan.unknowns.first(),
                         kExitPreparing);
  }

  const QVector<StoredModel> rows = resolver.rows();
  QVector<StoredModel> models;
  for (const ResolvedStage& stage : plan.stages) {
    const StoredModel* found = nullptr;
    for (const StoredModel& row : rows) {
      if (row.record.id == stage.modelId) {
        found = &row;
        break;
      }
    }
    if (found == nullptr) {
      return reportRefusal(QStringLiteral("model.owned.missing"),
                           QStringLiteral("the owned store holds no %1").arg(stage.modelId),
                           kExitFailed);
    }
    models.append(*found);
  }

  QVector<ReferenceFrame> frames;
  const QStringList framePaths = parser.values(frameOption);
  for (int index = 0; index < framePaths.size(); ++index) {
    FrameFixture fixture;
    QString frameKey;
    QString frameReason;
    if (!readPpmFixture(framePaths.at(index), &fixture, &frameKey, &frameReason)) {
      return reportRefusal(frameKey, frameReason, kExitUsage);
    }
    ReferenceFrame frame;
    frame.fixture = fixture;
    frame.unprocessed = true;
    frame.sourceIndex = index;
    frames.append(frame);
  }

  if (!QDir().mkpath(scratchInfo.absoluteFilePath())) {
    return reportRefusal(QStringLiteral("probe.scratch.create"),
                         QStringLiteral("'%1' could not be created")
                             .arg(scratchInfo.absoluteFilePath()),
                         kExitFailed);
  }

  PlanReferenceRequest request;
  request.plan = plan;
  request.source = planRequest.source;
  request.frames = frames;
  request.models = models;
  request.tools.inspectExecutable = parser.value(inspectOption);
  request.tools.ortRuntimeDir = parser.value(runtimeOption);
  request.tools.trtBinDir = parser.value(trtOption);
  request.tools.cudaBinDir = parser.value(cudaOption);
  request.scratchDirectory = scratchInfo.absoluteFilePath();

  NativeBuildCancellation cancellation;
  if (scenario == QLatin1String("cancel")) cancellation.cancel();
  request.cancellation = &cancellation;

  QVector<BuilderStageReference> references;
  QString reasonKey;
  QString reason;
  const bool built = buildPlanReferenceLineage(request, &references, &reasonKey, &reason);

  QJsonObject result;
  result.insert(QStringLiteral("ok"), built);
  result.insert(QStringLiteral("scenario"), scenario);
  result.insert(QStringLiteral("planId"), plan.id);
  result.insert(QStringLiteral("recipeId"), plan.recipeId);
  result.insert(QStringLiteral("stages"), static_cast<int>(plan.stages.size()));
  result.insert(QStringLiteral("reasonKey"), reasonKey);
  result.insert(QStringLiteral("reason"), reason);
  QJsonArray lineage;
  for (const BuilderStageReference& reference : references) lineage.append(referenceJson(reference));
  result.insert(QStringLiteral("references"), lineage);
  result.insert(QStringLiteral("scratch"), scratchInfo.absoluteFilePath());
  if (parser.isSet(jsonOption)) {
    out << QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)) << '\n';
  }

  if (scenario == QLatin1String("cancel")) {
    const bool passed = !built && reasonKey == QLatin1String("native.prepare.cancelled") &&
                        references.isEmpty();
    if (!parser.isSet(jsonOption)) {
      out << (passed ? "cancel: nothing was sent and nothing was written\n"
                     : "cancel: FAILED\n");
    }
    return passed ? kExitPass : kExitFailed;
  }

  if (!built) {
    if (!parser.isSet(jsonOption)) {
      out << "lineage refused: " << reasonKey << ": " << reason << '\n';
    }
    return kExitFailed;
  }
  if (references.size() != plan.stages.size()) {
    if (!parser.isSet(jsonOption)) {
      out << "lineage: " << references.size() << " reference(s) for "
          << plan.stages.size() << " stage(s)\n";
    }
    return kExitFailed;
  }
  if (!parser.isSet(jsonOption)) {
    out << "lineage: " << plan.id << "  " << references.size() << " stage(s)\n";
    for (const BuilderStageReference& reference : references) {
      for (const BuilderRawInput& input : reference.rawInputs) {
        out << "  input " << input.name << "  " << input.dtype << "  " << input.shape << "  "
            << input.filePath << '\n';
      }
    }
  }
  return kExitPass;
}
