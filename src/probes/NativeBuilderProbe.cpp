// mikai_native_builder_probe - the hand-run harness around the real preparation
// worker.
//
// This is not a second pipeline. It resolves a plan through the real planner over
// the app-owned model store, hands the real `runNativeBuild` the plan, the owned
// rows and the observed build identity, and reports exactly what that call did.
// It starts the real inspect/prep/verifier children, publishes through the real
// artifact store, and takes the real process lease.
//
// Every observed fact and every path comes from the request document; nothing is
// defaulted to a plausible constant. A document with no device UUID, no SM, no
// CUDA/driver version, no builder option or no tool path refuses by name before
// any work begins, because a build key assembled from placeholders names a
// machine the engine was never compiled for.
//
//   native_builder_probe --request <file.json> [--scenario build|reuse|cancel|lease|preserve]
//                         [--cancel-at <phase>] [--preserve-build-key <key>]
//                         [--app-org <org>] [--app-name <name>] [--json]
//
//   build     resolve, build, verify and publish the whole recipe once.
//   reuse     run twice; the second run must be a verified cache hit for every
//             stage and must publish nothing.
//   cancel    cancel at `--cancel-at` (default `build`); the run must report
//             cancelled, publish nothing and leave no staging behind.
//   lease     hold an inference lease, then run; the build must refuse busy.
//   preserve  run a request expected to fail while proving the artifact filed
//             under `--preserve-build-key` keeps its exact bytes.
//
// Exit: 0 the scenario's own outcome was observed; 1 the scenario failed; 3 the
// request document or usage was refused; 4 the plan is still Preparing.
//
// The probe runs under the application's own identity by default, because the
// owned model store it resolves against is the app-owned `native/models` tree
// that identity names. `--app-org`/`--app-name` select another app-owned
// boundary explicitly - for example the boundary the inspection tests populated
// - and are never guessed from a path.

#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QStringList>
#include <QtCore/QTextStream>

#include "native/NativeArtifactStore.hpp"
#include "native/NativeBuilder.hpp"
#include "native/NativeInspectedPlan.hpp"
#include "native/NativeModelInspect.hpp"
#include "native/NativePreparationPlan.hpp"
#include "native/NativeRecipe.hpp"
#include "native/NativeWorkLease.hpp"

#include <cstdio>
#include <optional>

using namespace mplayer;
using namespace mplayer::native;

namespace {

constexpr int kExitPass = 0;
constexpr int kExitScenarioFailed = 1;
constexpr int kExitUsage = 3;
constexpr int kExitPreparing = 4;

QJsonObject resultJson(const BuilderResult& result) {
  QJsonObject object;
  object.insert(QStringLiteral("ok"), result.ok);
  object.insert(QStringLiteral("cancelled"), result.cancelled);
  object.insert(QStringLiteral("reasonKey"), result.reasonKey);
  object.insert(QStringLiteral("reason"), result.reason);
  object.insert(QStringLiteral("phase"), result.phase);
  object.insert(QStringLiteral("stageIndex"), result.stageIndex);
  object.insert(QStringLiteral("toolError"), result.toolError);
  QJsonArray keys;
  for (const QString& key : result.publishedBuildKeys) keys.append(key);
  object.insert(QStringLiteral("publishedBuildKeys"), keys);
  QJsonArray stages;
  for (const BuilderStageOutcome& stage : result.stages) {
    QJsonObject entry;
    entry.insert(QStringLiteral("cacheHit"), stage.cacheHit);
    entry.insert(QStringLiteral("buildKey"), stage.record.buildKey());
    entry.insert(QStringLiteral("enginePath"), stage.record.enginePath);
    entry.insert(QStringLiteral("engineSha256"), stage.record.manifest.engineSha256);
    entry.insert(QStringLiteral("engineBytes"), stage.record.manifest.engine.bytes);
    stages.append(entry);
  }
  object.insert(QStringLiteral("stages"), stages);
  QJsonArray warnings;
  for (const QString& warning : result.warnings) warnings.append(warning);
  object.insert(QStringLiteral("warnings"), warnings);
  return object;
}

QJsonObject planJson(const PreparationPlan& plan) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), plan.id);
  object.insert(QStringLiteral("recipeId"), plan.recipeId);
  object.insert(QStringLiteral("backend"), stableName(plan.backend));
  object.insert(QStringLiteral("resolved"), plan.isResolved());
  QJsonArray blockers;
  for (const PlanBlocker& blocker : plan.blockers) {
    blockers.append(QJsonObject{{QStringLiteral("reasonKey"), blocker.reasonKey},
                                {QStringLiteral("reason"), blocker.reason},
                                {QStringLiteral("stageIndex"), blocker.stageIndex}});
  }
  object.insert(QStringLiteral("blockers"), blockers);
  QJsonArray unknowns;
  for (const QString& unknown : plan.unknowns) unknowns.append(unknown);
  object.insert(QStringLiteral("unknowns"), unknowns);
  QJsonArray stages;
  for (const ResolvedStage& stage : plan.stages) {
    stages.append(QJsonObject{{QStringLiteral("modelId"), stage.modelId},
                              {QStringLiteral("role"), stableName(stage.role)},
                              {QStringLiteral("inputShape"), stage.inputShape()},
                              {QStringLiteral("outputShape"), stage.outputShape()}});
  }
  object.insert(QStringLiteral("stages"), stages);
  return object;
}

/// Prints the refusal and returns `exitCode`: the usage code for a request this
/// build cannot read, the scenario code for an environment the request names but
/// the machine cannot satisfy. A plan that is still Preparing is neither.
int reportRefusal(const QString& reasonKey, const QString& reason, int exitCode) {
  QTextStream err(stderr);
  err << "native_builder_probe: refused " << reasonKey << ": " << reason << '\n';
  err.flush();
  return exitCode;
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("Mikai"));
  QCoreApplication::setApplicationName(QStringLiteral("Mikai Wrapper"));

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral(
      "Hand-run harness around runNativeBuild. Consumes one probe request document naming observed "
      "facts and real paths; resolves the plan through the real planner over the owned model store "
      "and reports the build's own outcome. Nothing is defaulted."));
  parser.addHelpOption();
  const QCommandLineOption requestOption(QStringList{QStringLiteral("request"), QStringLiteral("r")},
                                         QStringLiteral("Probe request JSON file."),
                                         QStringLiteral("file"));
  const QCommandLineOption scenarioOption(
      QStringList{QStringLiteral("scenario"), QStringLiteral("s")},
      QStringLiteral("build|reuse|cancel|lease|preserve."), QStringLiteral("name"),
      QStringLiteral("build"));
  const QCommandLineOption cancelPhaseOption(
      QStringLiteral("cancel-at"),
      QStringLiteral("Progress phase to cancel at (with --scenario cancel)."),
      QStringLiteral("phase"), QStringLiteral("build"));
  const QCommandLineOption preserveKeyOption(
      QStringLiteral("preserve-build-key"),
      QStringLiteral("Artifact build key to prove intact (with --scenario preserve)."),
      QStringLiteral("key"));
  const QCommandLineOption forgetKeyOption(
      QStringLiteral("forget-build-key"),
      QStringLiteral("Remove this artifact build key from the owned cache through the store's own "
                     "proved removal, then exit. Refuses a record that does not verify."),
      QStringLiteral("key"));
  const QCommandLineOption jsonOption(QStringLiteral("json"),
                                      QStringLiteral("Machine-readable result."));
  const QCommandLineOption orgOption(
      QStringLiteral("app-org"),
      QStringLiteral("Organization that owns the app-local data tree; pass an empty value to "
                     "select the empty-organization boundary (for example the one the inspection "
                     "tool published under). Defaults to the application's own Mikai."),
      QStringLiteral("org"));
  const QCommandLineOption nameOption(
      QStringLiteral("app-name"),
      QStringLiteral("Application name that owns the app-local data tree. Defaults to the "
                     "application's own name."),
      QStringLiteral("name"));
  parser.addOptions(QList<QCommandLineOption>{requestOption, scenarioOption, cancelPhaseOption,
                                              preserveKeyOption, forgetKeyOption, jsonOption,
                                              orgOption, nameOption});
  parser.process(app);
  QTextStream out(stdout);

  // The owned model store the resolver may read lives inside the app-local data
  // tree this process names; set it before any path is resolved. An explicitly
  // passed empty value is a real boundary (the empty-organization tree the
  // inspection tool published under), so presence is what is checked.
  QCoreApplication::setOrganizationName(parser.isSet(orgOption)
                                            ? parser.value(orgOption)
                                            : QStringLiteral("Mikai"));
  QCoreApplication::setApplicationName(parser.isSet(nameOption)
                                           ? parser.value(nameOption)
                                           : QStringLiteral("Mikai Wrapper"));

  const QString requestPath = parser.value(requestOption);
  if (requestPath.isEmpty()) {
    out << "native_builder_probe: --request is required\n";
    return kExitUsage;
  }
  const QString scenario = parser.value(scenarioOption);
  const QStringList known{QStringLiteral("build"), QStringLiteral("reuse"),
                          QStringLiteral("cancel"), QStringLiteral("lease"),
                          QStringLiteral("preserve")};
  if (!known.contains(scenario)) {
    out << "native_builder_probe: unknown scenario " << scenario << '\n';
    return kExitUsage;
  }

  QFile requestFile(requestPath);
  if (!requestFile.open(QIODevice::ReadOnly)) {
    out << "native_builder_probe: request " << requestPath
        << " cannot be read: " << requestFile.errorString() << '\n';
    return kExitUsage;
  }
  QJsonParseError parseError{};
  const QJsonDocument document = QJsonDocument::fromJson(requestFile.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    out << "native_builder_probe: request is not JSON: " << parseError.errorString() << '\n';
    return kExitUsage;
  }

  BuilderProbeRequest probe;
  QString reasonKey;
  QString reason;
  if (!parseBuilderProbeRequest(document.object(), &probe, &reasonKey, &reason)) {
    return reportRefusal(reasonKey, reason, kExitUsage);
  }

  // Clearing a wrong-provenance record goes through the store's own proved
  // removal, never a hand edit of an immutable manifest: it re-measures the
  // engine against the recorded digest and quarantines before deleting.
  if (parser.isSet(forgetKeyOption)) {
    const QString forgetKey = parser.value(forgetKeyOption);
    const ArtifactStore store(probe.request.store.root());
    QString removeKey;
    QString removeReason;
    const bool removed = removeArtifact(store, forgetKey, &removeKey, &removeReason);
    QJsonObject outcome;
    outcome.insert(QStringLiteral("scenario"), QStringLiteral("forget"));
    outcome.insert(QStringLiteral("pass"), removed);
    outcome.insert(QStringLiteral("buildKey"), forgetKey);
    outcome.insert(QStringLiteral("removed"), removed);
    outcome.insert(QStringLiteral("reasonKey"), removeKey);
    outcome.insert(QStringLiteral("reason"), removeReason);
    if (parser.isSet(jsonOption)) {
      out << QString::fromUtf8(QJsonDocument(outcome).toJson(QJsonDocument::Compact)) << '\n';
    } else if (removed) {
      out << "native_builder_probe forget removed " << forgetKey << '\n';
    } else {
      out << "native_builder_probe forget refused " << removeKey << ": " << removeReason << '\n';
    }
    out.flush();
    return removed ? kExitPass : kExitScenarioFailed;
  }

  // The resolver and the child tools share one explicitly named runtime.
  const InspectionRuntime runtime = inspectionRuntimeAt(probe.request.tools.ortRuntimeDir);
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(runtime, &runtimeReason)) {
    return reportRefusal(QStringLiteral("inspect.runtime.unavailable"), runtimeReason,
                         kExitScenarioFailed);
  }

  QFile recipeFile(probe.recipePath);
  if (!recipeFile.open(QIODevice::ReadOnly)) {
    out << "native_builder_probe: recipe " << probe.recipePath
        << " cannot be read: " << recipeFile.errorString() << '\n';
    return kExitUsage;
  }
  Recipe recipe;
  QString recipeKey;
  QString recipeReason;
  if (!parseRecipe(recipeFile.readAll(), &recipe, &recipeKey, &recipeReason)) {
    return reportRefusal(recipeKey, recipeReason, kExitScenarioFailed);
  }

  InspectedPlanResolver resolver(probe.request.modelRoot, runtime);
  QString storeKey;
  QString storeReason;
  if (!resolver.refresh(&storeKey, &storeReason)) {
    return reportRefusal(storeKey, storeReason, kExitScenarioFailed);
  }

  PlanRequest planRequest;
  planRequest.recipe = recipe;
  planRequest.catalog = resolver.catalog();
  planRequest.backends = probe.backends;
  planRequest.source = probe.source;
  planRequest.device = probe.device;
  planRequest.window = ChainWindow();
  planRequest.dynamicResolver = resolver.resolver();

  const PreparationPlan plan = resolvePreparationPlan(planRequest);
  if (!plan.blockers.isEmpty()) {
    QJsonObject failure;
    failure.insert(QStringLiteral("scenario"), scenario);
    failure.insert(QStringLiteral("pass"), false);
    failure.insert(QStringLiteral("plan"), planJson(plan));
    if (parser.isSet(jsonOption)) {
      out << QString::fromUtf8(QJsonDocument(failure).toJson(QJsonDocument::Compact)) << '\n';
    } else {
      out << "native_builder_probe: plan refused by " << plan.blockers.first().reasonKey << ": "
          << plan.blockers.first().reason << '\n';
    }
    return kExitScenarioFailed;
  }
  if (!plan.isResolved()) {
    if (parser.isSet(jsonOption)) {
      QJsonObject preparing;
      preparing.insert(QStringLiteral("scenario"), scenario);
      preparing.insert(QStringLiteral("pass"), false);
      preparing.insert(QStringLiteral("plan"), planJson(plan));
      out << QString::fromUtf8(QJsonDocument(preparing).toJson(QJsonDocument::Compact)) << '\n';
    } else {
      for (const QString& unknown : plan.unknowns) {
        out << "native_builder_probe: planning is waiting on " << unknown << '\n';
      }
    }
    return kExitPreparing;
  }

  // One owned row per stage, in stage order, from the store the plan was resolved
  // against; and one caller reference per stage, in the same order.
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
                           kExitScenarioFailed);
    }
    models.append(*found);
  }
  if (probe.request.references.size() != plan.stages.size()) {
    return reportRefusal(
        QStringLiteral("build.inputs.count"),
        QStringLiteral("the request needs one reference per stage (%1), it has %2")
            .arg(plan.stages.size())
            .arg(probe.request.references.size()),
        kExitScenarioFailed);
  }

  BuilderRequest request = probe.request;
  request.plan = plan;
  request.models = models;

  QJsonArray events;
  const BuilderProgressCallback progress = [&events](const BuilderProgress& event) {
    events.append(QJsonObject{{QStringLiteral("stage"), event.stageIndex},
                              {QStringLiteral("modelId"), event.modelId},
                              {QStringLiteral("phase"), event.phase},
                              {QStringLiteral("detail"), event.detail}});
  };

  bool pass = false;
  QString detail;
  QJsonObject extra;

  if (scenario == QLatin1String("build")) {
    NativeBuildCancellation cancellation;
    const BuilderResult result = runNativeBuild(request, &cancellation, progress);
    pass = result.ok && result.stages.size() == plan.stages.size();
    for (const BuilderStageOutcome& stage : result.stages) {
      pass = pass && !stage.record.enginePath.isEmpty();
    }
    detail = result.ok ? QStringLiteral("published %1 stage(s)").arg(result.stages.size())
                       : QStringLiteral("%1: %2").arg(result.reasonKey, result.reason);
    extra.insert(QStringLiteral("result"), resultJson(result));
  } else if (scenario == QLatin1String("reuse")) {
    const BuilderResult first = runNativeBuild(request, nullptr, progress);
    const BuilderResult second = runNativeBuild(request, nullptr, progress);
    int cacheHits = 0;
    for (const BuilderStageOutcome& stage : second.stages) {
      if (stage.cacheHit) ++cacheHits;
    }
    pass = first.ok && second.ok && second.publishedBuildKeys.isEmpty() &&
           second.stages.size() == first.stages.size() &&
           cacheHits == second.stages.size();
    for (int i = 0; pass && i < second.stages.size(); ++i) {
      pass = second.stages.at(i).record.buildKey() == first.stages.at(i).record.buildKey();
    }
    detail = pass ? QStringLiteral("second run reused every verified artifact")
                  : QStringLiteral("second run ok=%1 cacheHits=%2 published=%3")
                        .arg(second.ok ? 1 : 0)
                        .arg(cacheHits)
                        .arg(second.publishedBuildKeys.size());
    extra.insert(QStringLiteral("first"), resultJson(first));
    extra.insert(QStringLiteral("second"), resultJson(second));
  } else if (scenario == QLatin1String("cancel")) {
    NativeBuildCancellation cancellation;
    const QString cancelPhase = parser.value(cancelPhaseOption);
    bool armed = false;
    const BuilderProgressCallback cancelling =
        [&progress, &cancellation, &cancelPhase, &armed](const BuilderProgress& event) {
          progress(event);
          if (!armed && event.phase == cancelPhase) {
            cancellation.cancel();
            armed = true;
          }
        };
    const BuilderResult result = runNativeBuild(request, &cancellation, cancelling);
    pass = armed && result.cancelled && !result.ok && result.publishedBuildKeys.isEmpty();
    detail = armed ? QStringLiteral("cancelled=%1 ok=%2 published=%3")
                         .arg(result.cancelled ? 1 : 0)
                         .arg(result.ok ? 1 : 0)
                         .arg(result.publishedBuildKeys.size())
                   : QStringLiteral("phase '%1' was never reached").arg(cancelPhase);
    extra.insert(QStringLiteral("result"), resultJson(result));
  } else if (scenario == QLatin1String("lease")) {
    const std::optional<NativeWorkLease> held = NativeWorkLease::acquire(NativeWorkKind::Inference);
    if (held.has_value()) {
      const BuilderResult result = runNativeBuild(request, nullptr, progress);
      pass = !result.ok && result.reasonKey == QLatin1String("build.busy");
      detail = QStringLiteral("build busy=%1 key=%2").arg(result.ok ? 0 : 1).arg(result.reasonKey);
      extra.insert(QStringLiteral("result"), resultJson(result));
    } else {
      detail = QStringLiteral("the inference lease could not be taken");
    }
  } else {  // preserve
    const QString preserveKey = parser.value(preserveKeyOption);
    if (preserveKey.isEmpty()) {
      out << "native_builder_probe: --preserve-build-key is required for --scenario preserve\n";
      return kExitUsage;
    }
    QString beforeKey;
    QString beforeReason;
    const std::optional<ArtifactRecord> before =
        readArtifact(request.store, preserveKey, &beforeKey, &beforeReason);
    if (!before.has_value()) {
      return reportRefusal(beforeKey, beforeReason, kExitScenarioFailed);
    }
    const std::optional<FileDigest> beforeBytes = measureFile(before->enginePath);
    const BuilderResult result = runNativeBuild(request, nullptr, progress);
    QString afterKey;
    QString afterReason;
    const std::optional<ArtifactRecord> after =
        readArtifact(request.store, preserveKey, &afterKey, &afterReason);
    const std::optional<FileDigest> afterBytes =
        after.has_value() ? measureFile(after->enginePath) : std::nullopt;
    pass = !result.ok && beforeBytes.has_value() && afterBytes.has_value() &&
           beforeBytes->sha256 == afterBytes->sha256 && beforeBytes->bytes == afterBytes->bytes;
    detail = QStringLiteral("build ok=%1; artifact %2 %3")
                 .arg(result.ok ? 1 : 0)
                 .arg(preserveKey)
                 .arg(pass ? QStringLiteral("kept its exact bytes")
                           : QStringLiteral("changed or vanished"));
    extra.insert(QStringLiteral("result"), resultJson(result));
    extra.insert(QStringLiteral("artifactSha256"),
                 beforeBytes.has_value() ? beforeBytes->sha256 : QString());
  }

  QJsonObject outcome;
  outcome.insert(QStringLiteral("scenario"), scenario);
  outcome.insert(QStringLiteral("pass"), pass);
  outcome.insert(QStringLiteral("detail"), detail);
  outcome.insert(QStringLiteral("plan"), planJson(plan));
  outcome.insert(QStringLiteral("events"), events);
  outcome.insert(QStringLiteral("outcome"), extra);

  if (parser.isSet(jsonOption)) {
    out << QString::fromUtf8(QJsonDocument(outcome).toJson(QJsonDocument::Compact)) << '\n';
  } else {
    out << "native_builder_probe " << scenario << (pass ? " PASS " : " FAIL ") << detail << '\n';
    for (int i = 0; i < plan.stages.size(); ++i) {
      out << "  stage " << i << ' ' << plan.stages.at(i).modelId << ' '
          << plan.stages.at(i).inputShape() << " -> " << plan.stages.at(i).outputShape() << '\n';
    }
  }
  out.flush();
  return pass ? kExitPass : kExitScenarioFailed;
}
