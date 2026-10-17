#include "TestNativePlaybackController.hpp"

#include <memory>
#include <optional>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>

#include "native/prepare/NativeArtifactStore.hpp"
#include "native/models/NativeCatalog.hpp"
#include "native/bundle/NativeManifest.hpp"
#include "native/status/NativePlaybackController.hpp"
#include "native/prepare/NativePreparedPlan.hpp"
#include "native/prepare/NativePreparationPlan.hpp"
#include "native/models/NativeRecipe.hpp"
#include "native/status/NativeStatus.hpp"
#include "native/status/NativeTypes.hpp"
#include "native/prepare/NativeWorkLease.hpp"

using namespace mplayer::native;

// Everything here goes through the public surface: real typed commands, real
// reader observations and the real process-local work claims. The fixture
// publishes a genuine artifact and resolves the real admission, so no step is a
// stand-in for the inventory or the lease.
namespace {

using Controller = NativePlaybackController;
using Outcome = Controller::Outcome;
using State = Controller::State;
using Command = NativePlaybackCommand;
using CommandKind = NativePlaybackCommandKind;

constexpr quint64 kSourceEpoch = 7;
constexpr quint64 kRevision = 5;

QString sha256Hex(const QByteArray& data) {
  return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool writeBytes(const QString& path, const QByteArray& data) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  return file.write(data) == data.size();
}

ModelProfile spatialProfile() {
  ModelProfile profile;
  profile.input.dtype = QString::fromLatin1(kEligibleIoType);
  profile.input.rank = 4;
  profile.input.batch = 1;
  profile.input.channels = 3;
  profile.input.height = 1080;
  profile.input.width = 1920;
  profile.output.dtype = QString::fromLatin1(kEligibleIoType);
  profile.output.rank = 4;
  profile.output.batch = 1;
  profile.output.channels = 3;
  profile.output.height = 2160;
  profile.output.width = 3840;
  return profile;
}

EngineBuildIdentity engineBuildFor(const DeviceIdentity& device) {
  EngineBuildIdentity build;
  build.provider = Backend::TensorRT;
  build.runtimeVersion = QStringLiteral("11.3.0.99");
  build.pluginVersion = QStringLiteral("1.2.3");
  build.coreSha = QStringLiteral("63d127174d8f70336c82a22408929e2bc301edcb");
  build.sourceAbi = 8;
  build.precision = EnginePrecision::Fp16;
  build.tf32 = false;
  build.stronglyTyped = true;
  build.skipInference = true;
  build.deviceUuid = device.computeDeviceUuid;
  build.smMajor = device.smMajor;
  build.smMinor = device.smMinor;
  build.cudaVersion = QStringLiteral("13.4.92");
  build.driverVersion = QStringLiteral("616.92");
  build.options = QVector<BuildOption>{
      BuildOption{QStringLiteral("stronglyTyped"), QString(), false},
      BuildOption{QStringLiteral("skipInference"), QString(), false},
      BuildOption{QStringLiteral("tacticSources"), QStringLiteral("-CUDNN,-CUBLAS"), true}};
  return build;
}

ArtifactManifest manifestFor(const BuildRequest& request, qint64 graphBytes,
                             const QByteArray& engineBytes) {
  ArtifactManifest manifest;
  manifest.schema = artifactManifestSchema();
  manifest.mikaiFormat = 1;
  manifest.buildKey = request.buildKey();
  manifest.engineSha256 = sha256Hex(engineBytes);
  manifest.source.modelContentId = request.graph.contentId();
  manifest.source.onnxSha256 = request.graph.graphSha256;
  manifest.source.onnxBytes = graphBytes;
  manifest.source.declaredProfile = request.graph.declaredProfile;
  manifest.stage.role = request.stage.role;
  manifest.stage.inputShape = request.stage.inputShape;
  manifest.stage.outputShape = request.stage.outputShape;
  manifest.stage.ioFormat = request.stage.ioFormat;
  manifest.stage.padLeft = request.stage.padLeft;
  manifest.stage.padTop = request.stage.padTop;
  manifest.build.provider = request.build.provider;
  manifest.build.runtime = request.build.runtimeVersion;
  manifest.build.plugin = request.build.pluginVersion;
  manifest.build.coreSha = request.build.coreSha;
  manifest.build.sourceAbi = request.build.sourceAbi;
  manifest.build.precision = request.build.precision;
  manifest.build.tf32 = request.build.tf32;
  manifest.build.stronglyTyped = request.build.stronglyTyped;
  manifest.build.skipInference = request.build.skipInference;
  manifest.build.canonicalOptions = request.build.canonicalOptions();
  manifest.device.uuid = request.build.deviceUuid;
  manifest.device.sm = request.build.smText();
  manifest.device.driver = request.build.driverVersion;
  manifest.device.cuda = request.build.cudaVersion;
  manifest.device.luid = request.build.adapterLuid;
  manifest.engine.file = QStringLiteral("engine.plan");
  manifest.engine.bytes = engineBytes.size();
  manifest.engine.sha256 = manifest.engineSha256;
  manifest.createdAt = QStringLiteral("2026-10-01T00:00:00Z");
  return manifest;
}

struct Fixture {
  QTemporaryDir dir;
  QString artifactsRoot;
  QString modelRoot;
  QByteArray graphBytes;
  QByteArray engineBytes;
  ModelRecord record;
  EngineBuildIdentity build;
  SourceGeometry source;
  DeviceIdentity device;
  PreparationPlan plan;
  PreparedAdmissionRequest admission;
  /// The published artifact's own build key, so a test can observe whether its
  /// claim is still held without re-deriving the manifest.
  QString artifactKey;
};

/// Publishes one genuine artifact and assembles the request the controller
/// admits. Returns null on any setup failure rather than asserting, so a caller
/// can report it with its own test macros.
std::unique_ptr<Fixture> buildFixture(const QString& requestId) {
  auto fixture = std::make_unique<Fixture>();
  if (!fixture->dir.isValid()) return nullptr;

  fixture->artifactsRoot = QDir(fixture->dir.path()).filePath(QStringLiteral("artifacts"));
  fixture->modelRoot = QDir(fixture->dir.path()).filePath(QStringLiteral("models"));
  fixture->graphBytes = QByteArrayLiteral("onnx-graph-bytes-for-playback-controller");
  fixture->engineBytes = QByteArray(4096, char(0x5a));

  const QString graphSha = sha256Hex(fixture->graphBytes);
  const QString modelPath =
      QDir(QDir(fixture->modelRoot).filePath(graphSha)).filePath(QStringLiteral("model.onnx"));
  if (!writeBytes(modelPath, fixture->graphBytes)) return nullptr;

  const ModelProfile profile = spatialProfile();
  fixture->record.id =
      userModelIdFor(computeContentId(ModelRole::Spatial, graphSha, profile));
  fixture->record.displayName = QStringLiteral("Playback Controller Test Model");
  fixture->record.origin = ModelOrigin::UserImported;
  fixture->record.onnxPath = modelPath;
  fixture->record.onnxSha256 = graphSha;
  fixture->record.onnxBytes = fixture->graphBytes.size();
  fixture->record.role = ModelRole::Spatial;
  fixture->record.actualProfile = profile;
  fixture->record.availableBackends = QVector<Backend>{Backend::TensorRT};
  fixture->record.availability = readyAvailability();

  fixture->source.codedSize = QSize(1920, 1080);
  fixture->source.rotationDegrees = 0;
  fixture->source.fps = Rational{24, 1};
  fixture->source.csp = QStringLiteral("bt709");
  fixture->source.matrix = QStringLiteral("bt709");
  fixture->source.range = QStringLiteral("limited");
  fixture->source.siting = QStringLiteral("left");
  fixture->source.bitDepth = 8;

  fixture->device.decoderDeviceUuid = QStringLiteral("GPU-9f2c1a44-0b7d-4e21-9c3a-1d5e");
  fixture->device.computeDeviceUuid = fixture->device.decoderDeviceUuid;
  fixture->device.smMajor = 12;
  fixture->device.smMinor = 0;

  fixture->build = engineBuildFor(fixture->device);

  Recipe recipe;
  recipe.id = QStringLiteral("recipe:playback-controller-test");
  recipe.label = QStringLiteral("Playback Controller Test");
  recipe.spatial = QVector<SpatialStage>{SpatialStage{fixture->record.id, QSize(), false}};
  recipe.backend = Backend::TensorRT;

  CatalogView catalog;
  catalog.models = QVector<ModelRecord>{fixture->record};
  BackendAvailability backends;
  backends.available = QVector<Backend>{Backend::TensorRT};

  PlanRequest planRequest;
  planRequest.recipe = recipe;
  planRequest.catalog = catalog;
  planRequest.backends = backends;
  planRequest.source = fixture->source;
  planRequest.device = fixture->device;
  planRequest.window = ChainWindow{};
  fixture->plan = resolvePreparationPlan(planRequest);
  if (!fixture->plan.isResolved() || fixture->plan.stages.isEmpty()) return nullptr;

  const ResolvedStage& stage = fixture->plan.stages.first();
  BuildRequest request;
  request.graph = graphIdentityOf(fixture->record);
  request.stage.role = stage.role;
  request.stage.inputShape = stage.inputShape();
  request.stage.outputShape = stage.outputShape();
  request.stage.ioFormat = stage.ioFormats();
  request.stage.padLeft = stage.padL;
  request.stage.padTop = stage.padT;
  request.build = fixture->build;
  if (!request.isComplete()) return nullptr;

  const ArtifactStore store(fixture->artifactsRoot);
  const QString scratch = QDir(fixture->dir.path()).filePath(QStringLiteral("produced.engine"));
  if (!writeBytes(scratch, fixture->engineBytes)) return nullptr;
  const ArtifactManifest manifest = manifestFor(request, fixture->graphBytes.size(),
                                                fixture->engineBytes);
  if (!publishArtifact(store, manifest, scratch).has_value()) return nullptr;
  fixture->artifactKey = manifest.buildKey;

  fixture->admission.plan = fixture->plan;
  fixture->admission.models = QVector<ModelRecord>{fixture->record};
  fixture->admission.build = fixture->build;
  fixture->admission.store = store;
  fixture->admission.modelRoot = fixture->modelRoot;
  fixture->admission.source = fixture->source;
  fixture->admission.device = fixture->device;
  fixture->admission.recipeId = recipe.id;
  fixture->admission.backend = Backend::TensorRT;
  fixture->admission.requestId = requestId;
  fixture->admission.revision = 1;
  fixture->admission.epoch = kSourceEpoch;
  return fixture;
}

NativeEffectivePlan effectiveFor(const PreparationPlan& plan) {
  NativeEffectivePlan effective;
  effective.recipe = plan.recipeId;
  effective.provider = stableName(plan.backend);
  effective.model = QStringLiteral("2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16");
  int spatial = 0;
  const ResolvedStage* temporal = nullptr;
  for (const ResolvedStage& stage : plan.stages) {
    if (stage.kind == StageKind::Spatial) {
      ++spatial;
    } else {
      temporal = &stage;
    }
  }
  effective.spatialStages = spatial;
  if (temporal != nullptr) {
    effective.interpolation = NativeInterpolation{temporal->factor, QStringLiteral("rife_v4.14")};
  }
  effective.order = QStringLiteral("spatial-then-temporal");
  const std::optional<QSize> sourceSize = plan.source.displaySize();
  const std::optional<QSize> outputSize = plan.outputGeometry();
  if (sourceSize.has_value()) {
    effective.sourceSize = NativeSize{sourceSize->width(), sourceSize->height()};
  }
  if (outputSize.has_value()) {
    effective.outputSize = NativeSize{outputSize->width(), outputSize->height()};
  }
  effective.sourceFps = plan.source.fps;
  effective.outputFps = plan.outputCadence();
  effective.decoder = QStringLiteral("nvdec/cuda");
  effective.csp = QStringLiteral("bt709/limited");
  return effective;
}

StatusSnapshot activeDocument(const PreparationPlan& plan, qint64 revision) {
  StatusSnapshot document;
  document.version = kNativeStatusProtocol;
  document.revision = revision;
  document.state = NativeState::Active;
  document.epoch = static_cast<qint64>(kSourceEpoch);
  document.requested = SelectionState{plan.recipeId, plan.backend};
  document.effective = effectiveFor(plan);
  NativeCadenceCounters counters;
  counters.framesIn = 240;
  counters.framesOut = 240;
  document.counters = counters;
  return document;
}

StatusSnapshot offDocument(qint64 revision) {
  StatusSnapshot document;
  document.version = kNativeStatusProtocol;
  document.revision = revision;
  document.state = NativeState::Off;
  document.epoch = static_cast<qint64>(kSourceEpoch);
  document.reason = QStringLiteral("native.probe.passthrough");
  return document;
}

StatusSnapshot failedDocument(qint64 revision) {
  StatusSnapshot document;
  document.version = kNativeStatusProtocol;
  document.revision = revision;
  document.state = NativeState::Failed;
  document.epoch = static_cast<qint64>(kSourceEpoch);
  document.reason = QStringLiteral("the native engine failed to load");
  return document;
}

/// A plan the producer has really released: the device resources are gone, so
/// this is the acknowledgement a teardown stop has to wait for.
StatusSnapshot suspendedDocument(qint64 revision) {
  StatusSnapshot document;
  document.version = kNativeStatusProtocol;
  document.revision = revision;
  document.state = NativeState::Suspended;
  document.epoch = static_cast<qint64>(kSourceEpoch);
  document.reason = QStringLiteral("the native chain is suspended");
  return document;
}

/// The producer's own first report after it loaded and verified the whole plan
/// but before any output completed: Prepared-without-a-completion is Preparing,
/// never Active - and it publishes no effective plan because none has run.
StatusSnapshot loadedDocument(const PreparationPlan& plan, qint64 revision) {
  StatusSnapshot document;
  document.version = kNativeStatusProtocol;
  document.revision = revision;
  document.state = NativeState::Preparing;
  document.epoch = static_cast<qint64>(kSourceEpoch);
  document.requested = SelectionState{plan.recipeId, plan.backend};
  document.reason = QStringLiteral("the prepared plan has not completed an output yet");
  return document;
}

/// The reader's own report once the core has no `mikai_native` property any more:
/// this, and only this, is how a removal is acknowledged.
NativeStatusView absentFilterView() {
  NativeStatusView view;
  view.availability = unavailableAvailability(QStringLiteral("native.status.missing"),
                                              QStringLiteral("no mikai_native filter is attached"));
  return view;
}

/// Round-trips a document through the real reader so a hand-built snapshot can
/// never be accepted here in a shape the parser would refuse.
bool makeView(const StatusSnapshot& document, NativeStatusView* view) {
  StatusSnapshot parsed;
  QString key;
  QString reason;
  if (!parseStatusSnapshot(statusSnapshotToJson(document), &parsed, &key, &reason)) return false;
  view->snapshot = parsed;
  view->availability = statusSnapshotAvailability(parsed);
  return true;
}

}  // namespace

// A submitted request is a request: the controller may hold a claim and have
// sent the command, but nothing is effective until the filter's own snapshot
// says so.
void TestNativePlaybackController::aRequestAloneIsNeverEffective() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);

  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(!sent.isEmpty());
  QVERIFY(sent.last().kind == CommandKind::Attach);
  QCOMPARE(sent.last().spec, QByteArray("@mikai_native:animejanai"));

  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);
  QVERIFY(sent.last().kind == CommandKind::Admit);
  QCOMPARE(sent.last().requestId, QStringLiteral("req:1"));
  QVERIFY(sent.last().spec.contains("mikai.native.admission/1"));

  QVERIFY(controller.status().requested.has_value());
  QCOMPARE(controller.status().requested->recipeId, fixture->admission.recipeId);
  QVERIFY(!controller.status().effective.has_value());
  QVERIFY(!controller.status().confirmed);
  QVERIFY(controller.state() == State::Admitting);
  QVERIFY(controller.status().availability.value != Availability::Ready);
  QVERIFY(controller.holdsInferenceLease());
}

// The whole transaction: attach, admit, observe progress, observe the whole
// plan. Only the last step makes the plan effective, and it keeps the claim.
void TestNativePlaybackController::anAcknowledgedWholePlanBecomesEffective() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);

  NativeStatusView view;
  QVERIFY(makeView(activeDocument(fixture->plan, kRevision), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Activated);

  QVERIFY(controller.state() == State::Active);
  QVERIFY(controller.status().confirmed);
  QVERIFY(controller.status().effective.has_value());
  QCOMPARE(controller.status().effective->recipe, fixture->plan.recipeId);
  QCOMPARE(controller.status().effective->provider, stableName(fixture->plan.backend));
  const NativeSize expectedOutput{fixture->plan.outputGeometry()->width(),
                                  fixture->plan.outputGeometry()->height()};
  QVERIFY(controller.status().effective->outputSize == expectedOutput);
  QVERIFY(controller.status().buildKeys.size() == 1);
  QCOMPARE(controller.status().requestId, QStringLiteral("req:1"));
  QVERIFY(controller.holdsInferenceLease());
}

// An observation read under another source epoch says nothing about this one.
void TestNativePlaybackController::anObservationFromASupersededEpochIsDropped() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);

  NativeStatusView view;
  QVERIFY(makeView(activeDocument(fixture->plan, kRevision), &view));
  QVERIFY(controller.observe(view, kSourceEpoch - 1, 0).outcome == Outcome::Ignored);
  QVERIFY(!controller.status().effective.has_value());
  QVERIFY(controller.state() == State::Admitting);

  // A new source epoch supersedes the old binding entirely.
  controller.beginSource(kSourceEpoch + 2);
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Ignored);
  QVERIFY(!controller.status().effective.has_value());
}

// A transport result is progress or a refusal, never acknowledgement; an id
// that no longer names the transition in flight is dropped.
void TestNativePlaybackController::aLateCompletionForASupersededTransitionIsDropped() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);

  QVERIFY(controller.onCommandCompleted(QStringLiteral("req:stale"), true).outcome ==
          Outcome::Ignored);
  QVERIFY(controller.state() == State::Admitting);
  QVERIFY(!controller.status().effective.has_value());

  // The real id's accepted transport is still not activation.
  QVERIFY(controller.onCommandCompleted(QStringLiteral("req:1"), true).outcome == Outcome::Pending);
  QVERIFY(!controller.status().confirmed);

  NativeStatusView view;
  QVERIFY(makeView(activeDocument(fixture->plan, kRevision), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Activated);
  QVERIFY(controller.status().confirmed);

  // Now that the transition is settled, a late result for it is discarded.
  QVERIFY(controller.onCommandCompleted(QStringLiteral("req:1"), false).outcome == Outcome::Ignored);
  QVERIFY(controller.status().confirmed);
}

// A replacement that cannot resolve leaves the running plan, its identity and
// its claims exactly where they were.
void TestNativePlaybackController::aRefusedReplacementKeepsTheRunningPlanAndItsClaims() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);
  NativeStatusView view;
  QVERIFY(makeView(activeDocument(fixture->plan, kRevision), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Activated);
  const QStringList keysBefore = controller.status().buildKeys;

  PreparedAdmissionRequest replacement = fixture->admission;
  replacement.requestId = QStringLiteral("req:2");
  replacement.revision = 2;
  replacement.models.clear();
  const Controller::Decision decision = controller.requestAdmission(replacement, 0);
  QVERIFY(decision.outcome == Outcome::Refused);
  QVERIFY(!decision.reasonKey.isEmpty());

  QVERIFY(controller.state() == State::Active);
  QVERIFY(controller.status().confirmed);
  QVERIFY(controller.status().effective.has_value());
  QCOMPARE(controller.status().buildKeys, keysBefore);
  QCOMPARE(controller.status().requestId, QStringLiteral("req:1"));
  QVERIFY(controller.holdsInferenceLease());

  // The acknowledged plan's artifact is still pinned by a live lease.
  QString reasonKey;
  QVERIFY(!removeArtifact(fixture->admission.store, keysBefore.first(), &reasonKey, nullptr));
  QCOMPARE(reasonKey, QStringLiteral("native.artifact.inUse"));
}

// The device claim is real: while a plan is in flight a preparation build
// cannot start, and a running preparation build refuses a new admission.
void TestNativePlaybackController::aPreparationBuildIsBlockedWhileAnAdmissionIsPending() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);

  {
    std::optional<NativeWorkLease> preparation = NativeWorkLease::acquire(NativeWorkKind::Preparation);
    QVERIFY(preparation.has_value());
    const Controller::Decision decision = controller.requestAdmission(fixture->admission, 0);
    QVERIFY(decision.outcome == Outcome::Refused);
    QCOMPARE(decision.reasonKey, QStringLiteral("native.playback.engineBusy"));
  }

  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);
  QVERIFY(controller.holdsInferenceLease());
  QVERIFY(!NativeWorkLease::acquire(NativeWorkKind::Preparation).has_value());

  controller.endSource();
  QVERIFY(NativeWorkLease::acquire(NativeWorkKind::Preparation).has_value());
}

// Requested off drains first; only the acknowledged drain releases the device
// claim, and it then asks for the filter's removal.
void TestNativePlaybackController::anAcknowledgedOffReleasesTheDeviceClaim() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);
  NativeStatusView view;
  QVERIFY(makeView(activeDocument(fixture->plan, kRevision), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Activated);

  QVERIFY(controller.requestOff(0).outcome == Outcome::Pending);
  QVERIFY(sent.last().kind == CommandKind::Drain);
  QVERIFY(controller.holdsInferenceLease());

  NativeStatusView off;
  QVERIFY(makeView(offDocument(kRevision + 1), &off));
  // The acknowledged drain is only the first half: the removal is its own tracked
  // transition and is not Removed until it has really happened.
  QVERIFY(controller.observe(off, kSourceEpoch, 0).outcome == Outcome::Pending);
  QVERIFY(sent.last().kind == CommandKind::Off);
  QCOMPARE(sent.last().spec, QByteArray("@mikai_native"));
  QVERIFY(controller.state() == State::Draining);
  QVERIFY(controller.holdsInferenceLease());

  const QString removalId = controller.status().pendingRequestId;
  QVERIFY(!removalId.isEmpty());
  QVERIFY(controller.onCommandCompleted(removalId, true).outcome == Outcome::Pending);
  QVERIFY(controller.holdsInferenceLease());
  QVERIFY(controller.observe(absentFilterView(), kSourceEpoch, 0).outcome == Outcome::Removed);
  QVERIFY(controller.state() == State::Off);
  QVERIFY(!controller.holdsInferenceLease());
  QVERIFY(!controller.status().effective.has_value());
  QVERIFY(NativeWorkLease::acquire(NativeWorkKind::Preparation).has_value());
}

// Abandoning a transition in flight releases the claims it took and discards a
// result that arrives for it afterwards, while the running plan survives.
void TestNativePlaybackController::aCancelledTransitionReleasesItsArtifactClaims() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);
  NativeStatusView view;
  QVERIFY(makeView(activeDocument(fixture->plan, kRevision), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Activated);

  PreparedAdmissionRequest replacement = fixture->admission;
  replacement.requestId = QStringLiteral("req:2");
  replacement.revision = 2;
  QVERIFY(controller.requestAdmission(replacement, 0).outcome == Outcome::Pending);
  QVERIFY(controller.state() == State::Active);

  QVERIFY(controller.cancelPending().outcome == Outcome::Cancelled);
  QVERIFY(controller.state() == State::Active);
  QVERIFY(controller.status().confirmed);
  QVERIFY(controller.onCommandCompleted(QStringLiteral("req:2"), true).outcome == Outcome::Ignored);

  // The acknowledged plan's artifact claim survives the abandoned replacement.
  QString reasonKey;
  QVERIFY(!removeArtifact(fixture->admission.store, controller.status().buildKeys.first(),
                          &reasonKey, nullptr));
  QCOMPARE(reasonKey, QStringLiteral("native.artifact.inUse"));
}

// A changed observed source fact set invalidates the pending admission; nothing
// is loaded or built to replace it.
void TestNativePlaybackController::aChangedSourceGeometryDropsThePendingAdmission() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);

  SourceGeometry moved = fixture->source;
  moved.fps = Rational{30, 1};
  QVERIFY(controller.onSourceGeometry(moved));
  QVERIFY(controller.state() == State::Attached);
  QVERIFY(controller.status().pendingRequestId.isEmpty());

  // The dropped admission cannot be adopted by a late observation.
  NativeStatusView view;
  QVERIFY(makeView(activeDocument(fixture->plan, kRevision), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Ignored);
  QVERIFY(!controller.status().effective.has_value());

  controller.endSource();
  QVERIFY(NativeWorkLease::acquire(NativeWorkKind::Preparation).has_value());
}

// A transition no observation acknowledged within its bound ends in a named,
// non-Active state rather than an unbounded wait.
void TestNativePlaybackController::aDeadlineWithoutAcknowledgementIsANamedNonActiveTransition() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.setControlTimeoutMs(1000);
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);

  QVERIFY(controller.tick(999).outcome == Outcome::Ignored);
  const Controller::Decision expired = controller.tick(1000);
  QVERIFY(expired.outcome == Outcome::TimedOut);
  QCOMPARE(expired.reasonKey, QStringLiteral("native.playback.admissionTimeout"));
  QVERIFY(controller.state() == State::Failed);
  QVERIFY(!controller.status().effective.has_value());
  QVERIFY(!controller.holdsInferenceLease());

  // A plan observed after the timeout is not adopted behind the caller's back.
  NativeStatusView view;
  QVERIFY(makeView(activeDocument(fixture->plan, kRevision + 1), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Ignored);
  QVERIFY(!controller.status().effective.has_value());
}

// A filter that reports a hard failure refuses the pending admission and never
// records it as effective.
void TestNativePlaybackController::aFailedFilterRefusesThePendingAdmission() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);

  NativeStatusView failed;
  QVERIFY(makeView(failedDocument(kRevision), &failed));
  const Controller::Decision decision = controller.observe(failed, kSourceEpoch, 0);
  QVERIFY(decision.outcome == Outcome::Refused);
  QVERIFY(!decision.reasonKey.isEmpty());
  QVERIFY(controller.state() == State::Failed);
  QVERIFY(!controller.status().effective.has_value());
  QVERIFY(!controller.status().confirmed);
  QVERIFY(!controller.holdsInferenceLease());
}

// The first report after the plan was loaded is not activation: Active is the
// only state that states an effective plan, and it takes a completed output.
void TestNativePlaybackController::aLoadedPlanIsNotActivationUntilAnOutputCompletes() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);

  NativeStatusView view;
  QVERIFY(makeView(loadedDocument(fixture->plan, kRevision), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Pending);
  QVERIFY(controller.state() == State::Admitting);
  QVERIFY(!controller.status().confirmed);
  QVERIFY(!controller.status().effective.has_value());

  QVERIFY(makeView(activeDocument(fixture->plan, kRevision + 1), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Activated);
  QVERIFY(controller.state() == State::Active);
  QVERIFY(controller.status().confirmed);
  QVERIFY(controller.status().effective.has_value());
}

// While a plan lock is held a request is recorded, never delivered: the
// effective plan, its claims and its identity do not move. Releasing submits
// exactly the latest request once, and that still has to be acknowledged.
void TestNativePlaybackController::aLockedPlanQueuesTheLatestRequestAndAppliesItOnUnlock() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);
  NativeStatusView view;
  QVERIFY(makeView(activeDocument(fixture->plan, kRevision), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Activated);

  const QStringList keysBefore = controller.status().buildKeys;
  QVERIFY(controller.lockPlan());
  QVERIFY(controller.planLocked());
  QVERIFY(!controller.hasQueuedTransition());
  const int commandsBefore = sent.size();

  PreparedAdmissionRequest second = fixture->admission;
  second.requestId = QStringLiteral("req:2");
  second.revision = 2;
  const Controller::Decision queued = controller.requestAdmission(second, 0);
  QVERIFY(queued.outcome == Outcome::Pending);
  QCOMPARE(queued.reasonKey, QStringLiteral("native.playback.planLocked"));
  QVERIFY(controller.hasQueuedTransition());
  QCOMPARE(sent.size(), commandsBefore);
  QVERIFY(controller.state() == State::Active);
  QVERIFY(controller.status().confirmed);
  QCOMPARE(controller.status().requestId, QStringLiteral("req:1"));
  QCOMPARE(controller.status().buildKeys, keysBefore);
  QVERIFY(controller.holdsInferenceLease());

  // Last wins: only the newest request survives the lock.
  PreparedAdmissionRequest third = fixture->admission;
  third.requestId = QStringLiteral("req:3");
  third.revision = 3;
  QVERIFY(controller.requestAdmission(third, 0).outcome == Outcome::Pending);
  QCOMPARE(controller.status().requestId, QStringLiteral("req:1"));
  QCOMPARE(sent.size(), commandsBefore);

  const Controller::Decision applied = controller.unlockPlan(0);
  QVERIFY(applied.outcome == Outcome::Pending);
  QVERIFY(!controller.planLocked());
  QVERIFY(!controller.hasQueuedTransition());
  QCOMPARE(sent.size(), commandsBefore + 1);
  QCOMPARE(sent.last().kind, CommandKind::Admit);
  QCOMPARE(sent.last().requestId, QStringLiteral("req:3"));
  // Submitted is not effective: the old plan is still the acknowledged one.
  QCOMPARE(controller.status().requestId, QStringLiteral("req:1"));
  QVERIFY(controller.status().confirmed);

  QVERIFY(makeView(activeDocument(fixture->plan, kRevision + 1), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Activated);
  QCOMPARE(controller.status().requestId, QStringLiteral("req:3"));
}

// A source that moved under the lock must not receive the request resolved for
// the old one: the queued request is dropped by name, not applied.
void TestNativePlaybackController::aLockedPlanDropsTheQueuedRequestWhenTheSourceMoved() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);
  NativeStatusView view;
  QVERIFY(makeView(activeDocument(fixture->plan, kRevision), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Activated);

  QVERIFY(controller.lockPlan());
  PreparedAdmissionRequest second = fixture->admission;
  second.requestId = QStringLiteral("req:2");
  second.revision = 2;
  QVERIFY(controller.requestAdmission(second, 0).outcome == Outcome::Pending);
  const int commandsBefore = sent.size();

  controller.beginSource(kSourceEpoch + 5);
  const Controller::Decision released = controller.unlockPlan(0);
  QVERIFY(released.outcome == Outcome::Refused);
  QCOMPARE(released.reasonKey, QStringLiteral("native.playback.lockSourceChanged"));
  QCOMPARE(sent.size(), commandsBefore);
  QVERIFY(!controller.planLocked());
  QVERIFY(!controller.hasQueuedTransition());
}

// A lock is only a freeze if the epoch is not already moving, and nesting is
// counted rather than flattened.
void TestNativePlaybackController::aPlanLockIsRefusedWhileATransitionIsInFlight() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);
  QVERIFY(!controller.lockPlan());
  QVERIFY(!controller.planLocked());

  NativeStatusView view;
  QVERIFY(makeView(activeDocument(fixture->plan, kRevision), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Activated);

  QVERIFY(controller.lockPlan());
  QVERIFY(controller.lockPlan());
  QVERIFY(controller.planLocked());
  QVERIFY(controller.unlockPlan(0).outcome == Outcome::Ignored);
  QVERIFY(controller.planLocked());
  QVERIFY(controller.unlockPlan(0).outcome == Outcome::Ignored);
  QVERIFY(!controller.planLocked());
}

// A deferred admission that refuses on release leaves the running plan, its
// identity and its claims exactly where they were.
void TestNativePlaybackController::aDeferredAdmissionRefusedOnUnlockKeepsTheRunningPlan() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);
  NativeStatusView view;
  QVERIFY(makeView(activeDocument(fixture->plan, kRevision), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Activated);
  const QStringList keysBefore = controller.status().buildKeys;

  QVERIFY(controller.lockPlan());
  PreparedAdmissionRequest replacement = fixture->admission;
  replacement.requestId = QStringLiteral("req:2");
  replacement.revision = 2;
  replacement.models.clear();
  QVERIFY(controller.requestAdmission(replacement, 0).outcome == Outcome::Pending);
  const Controller::Decision released = controller.unlockPlan(0);
  QVERIFY(released.outcome == Outcome::Refused);
  QVERIFY(!released.reasonKey.isEmpty());

  QVERIFY(controller.state() == State::Active);
  QVERIFY(controller.status().confirmed);
  QVERIFY(controller.status().effective.has_value());
  QCOMPARE(controller.status().requestId, QStringLiteral("req:1"));
  QCOMPARE(controller.status().buildKeys, keysBefore);
  QVERIFY(controller.holdsInferenceLease());
}

// A teardown is not an acknowledgement: an in-flight admission is never
// cancelled into a fake stop, and the stop itself has to be asked for and
// acknowledged.
void TestNativePlaybackController::aTeardownNeverFakesAStopAcknowledgement() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  controller.beginTeardown();
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);
  QVERIFY(controller.hasPendingTransition());
  QVERIFY(controller.holdsInferenceLease());

  // An observation that is not this admission's own acknowledgement settles
  // nothing, and the teardown does not clear the transition behind it.
  NativeStatusView view;
  QVERIFY(makeView(suspendedDocument(kRevision), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome != Outcome::Activated);
  QVERIFY(controller.hasPendingTransition());
  QVERIFY(controller.holdsInferenceLease());

  QVERIFY(makeView(activeDocument(fixture->plan, kRevision + 1), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Activated);
  QVERIFY(!controller.hasPendingTransition());
  QVERIFY(controller.holdsInferenceLease());

  // The stop names the admitted plan's own epoch and revision, which is what the
  // producer matches its stored plan against.
  const Controller::Decision stopped = controller.suspend(0);
  QVERIFY(stopped.outcome == Outcome::Pending);
  QCOMPARE(sent.last().revision, static_cast<quint64>(fixture->admission.revision));
  QCOMPARE(sent.last().epoch, static_cast<quint64>(fixture->admission.epoch));
  QVERIFY(controller.holdsInferenceLease());

  QVERIFY(makeView(suspendedDocument(kRevision + 2), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Suspended);
  QVERIFY(controller.state() == State::Suspended);
  QVERIFY(!controller.holdsInferenceLease());
  controller.endSource();
}

// A teardown that times out keeps the core's claims: an artifact a live filter
// may still be loading is not unpinned by a named timeout. Only core
// destruction releases it.
void TestNativePlaybackController::aTeardownTimeoutRetainsTheCoreAndItsClaims() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.setControlTimeoutMs(1000);
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  controller.beginTeardown();
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);
  QVERIFY(controller.holdsInferenceLease());

  const Controller::Decision expired = controller.tick(1000);
  QVERIFY(expired.outcome == Outcome::TimedOut);
  QVERIFY(!expired.reasonKey.isEmpty());
  QVERIFY(!controller.hasPendingTransition());
  QVERIFY(controller.state() == State::Failed);

  QVERIFY(controller.holdsInferenceLease());
  QString reasonKey;
  QVERIFY(!removeArtifact(fixture->admission.store, fixture->artifactKey, &reasonKey, nullptr));
  QCOMPARE(reasonKey, QStringLiteral("native.artifact.inUse"));

  controller.endSource();
  QVERIFY(!controller.holdsInferenceLease());
  QVERIFY(removeArtifact(fixture->admission.store, fixture->artifactKey, &reasonKey, nullptr));
}

// A request held behind the plan lock has to move the published status, or a
// caption reading it would never show that something is waiting.
void TestNativePlaybackController::aLockedRequestPublishesAQueuedReason() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);
  NativeStatusView view;
  QVERIFY(makeView(activeDocument(fixture->plan, kRevision), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Activated);
  QVERIFY(controller.status().reasonKey != QStringLiteral("native.playback.planLocked"));

  QVERIFY(controller.lockPlan());
  PreparedAdmissionRequest second = fixture->admission;
  second.requestId = QStringLiteral("req:2");
  second.revision = 2;
  QVERIFY(controller.requestAdmission(second, 0).outcome == Outcome::Pending);

  // Only the reason moved: the effective plan, its identity and its state are
  // untouched, so a caption can say a request is queued while Active stays Active.
  QCOMPARE(controller.status().reasonKey, QStringLiteral("native.playback.planLocked"));
  QVERIFY(controller.status().reason.contains(QStringLiteral("queued")));
  QVERIFY(controller.status().confirmed);
  QCOMPARE(controller.status().requestId, QStringLiteral("req:1"));
  QVERIFY(controller.state() == State::Active);

  // Releasing submits the queued request, which publishes its own transition
  // reason, so the queued caption cannot stick.
  const Controller::Decision applied = controller.unlockPlan(0);
  QVERIFY(applied.outcome == Outcome::Pending);
  QVERIFY(controller.status().reasonKey != QStringLiteral("native.playback.planLocked"));

  // A lock released with nothing waiting clears the queued reason too.
  Controller idle;
  QVector<Command> idleSent;
  idle.setCommandSink([&idleSent](const Command& command) { idleSent.append(command); });
  idle.beginSource(kSourceEpoch);
  QVERIFY(idle.lockPlan());
  const Controller::Decision released = idle.unlockPlan(0);
  QVERIFY(released.outcome == Outcome::Ignored);
  QCOMPARE(idle.status().reasonKey, QStringLiteral("native.playback.planLockRelease"));
}

// A removal from a probe is tracked: only the accepted command plus the filter's
// own observed absence makes it Removed.
void TestNativePlaybackController::aRemovalFromAnAttachedProbeIsTrackedUntilObserved() {
  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  QVERIFY(controller.state() == State::Attached);

  const Controller::Decision off = controller.requestOff(0);
  QVERIFY(off.outcome == Outcome::Pending);
  QVERIFY(sent.last().kind == CommandKind::Off);
  QVERIFY(controller.state() == State::Draining);
  QVERIFY(controller.hasPendingTransition());

  // An accepted command is progress, not absence.
  const QString removalId = controller.status().pendingRequestId;
  QVERIFY(!removalId.isEmpty());
  QVERIFY(controller.onCommandCompleted(removalId, true).outcome == Outcome::Pending);
  QVERIFY(controller.state() == State::Draining);

  QVERIFY(controller.observe(absentFilterView(), kSourceEpoch, 0).outcome == Outcome::Removed);
  QVERIFY(!controller.hasPendingTransition());
  QVERIFY(controller.state() == State::Off);
}

// A removal that times out is named and keeps every claim: it is never reported
// as Removed, and no artifact is unpinned.
void TestNativePlaybackController::aRemovalTimeoutKeepsTheClaimsAndIsNeverRemoved() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.setControlTimeoutMs(1000);
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  controller.beginTeardown();
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);
  QVERIFY(controller.tick(1000).outcome == Outcome::TimedOut);
  QVERIFY(controller.state() == State::Failed);
  QVERIFY(controller.holdsInferenceLease());

  // A Failed state still has to be removed for real.
  const Controller::Decision off = controller.requestOff(0);
  QVERIFY(off.outcome == Outcome::Pending);
  QVERIFY(controller.state() == State::Draining);
  const Controller::Decision expired = controller.tick(1000);
  QVERIFY(expired.outcome == Outcome::TimedOut);
  QVERIFY(expired.outcome != Outcome::Removed);
  QVERIFY(controller.state() == State::Failed);
  QVERIFY(controller.holdsInferenceLease());

  QString reasonKey;
  QVERIFY(!removeArtifact(fixture->admission.store, fixture->artifactKey, &reasonKey, nullptr));
  QCOMPARE(reasonKey, QStringLiteral("native.artifact.inUse"));

  controller.endSource();
  QVERIFY(!controller.holdsInferenceLease());
  QVERIFY(removeArtifact(fixture->admission.store, fixture->artifactKey, &reasonKey, nullptr));
}

// A plan that was loaded but never completed an output cannot be suspended, so it
// stops through an acknowledged removal - and it was never Active.
void TestNativePlaybackController::aPreparedStopIsRemovedWithoutBeingActive() {
  const std::unique_ptr<Fixture> fixture = buildFixture(QStringLiteral("req:1"));
  QVERIFY(fixture != nullptr);

  Controller controller;
  QVector<Command> sent;
  controller.setCommandSink([&sent](const Command& command) { sent.append(command); });
  controller.setControlTimeoutMs(1000);
  controller.beginSource(kSourceEpoch);
  QVERIFY(controller.attach().outcome == Outcome::Pending);
  controller.beginTeardown();
  QVERIFY(controller.requestAdmission(fixture->admission, 0).outcome == Outcome::Pending);
  NativeStatusView view;
  QVERIFY(makeView(loadedDocument(fixture->plan, kRevision), &view));
  QVERIFY(controller.observe(view, kSourceEpoch, 0).outcome == Outcome::Pending);
  QVERIFY(!controller.status().confirmed);

  QVERIFY(controller.tick(1000).outcome == Outcome::TimedOut);
  QVERIFY(controller.holdsInferenceLease());

  const Controller::Decision off = controller.requestOff(0);
  QVERIFY(off.outcome == Outcome::Pending);
  const QString removalId = controller.status().pendingRequestId;
  QVERIFY(!removalId.isEmpty());
  QVERIFY(controller.onCommandCompleted(removalId, true).outcome == Outcome::Pending);
  QVERIFY(controller.observe(absentFilterView(), kSourceEpoch, 0).outcome == Outcome::Removed);
  QVERIFY(!controller.holdsInferenceLease());

  QString reasonKey;
  QVERIFY(removeArtifact(fixture->admission.store, fixture->artifactKey, &reasonKey, nullptr));
}
