#include "TestNativeStatus.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <QThread>

#include <string>

#include "MpvApi.hpp"
#include "MpvHandle.hpp"
#include "native/NativeDiagnostics.hpp"
#include "native/NativeStatus.hpp"

using mplayer::AsyncReply;
using mplayer::MpvApi;
using mplayer::MpvBundle;
using mplayer::MpvHandle;
using mplayer::NodeValue;

using namespace mplayer::native;

namespace {

// The 1:1 spatial chain M19 asks for: source and output geometry are identical,
// so geometry alone says nothing, and the request (DirectML) is visibly not what
// the filter reports running (TensorRT).
StatusSnapshot oneToOneSpatial() {
  StatusSnapshot snapshot;
  snapshot.version = kNativeStatusProtocol;
  snapshot.revision = 412;
  snapshot.state = NativeState::Active;
  snapshot.epoch = 7;
  SelectionState requested;
  requested.recipeId = QStringLiteral("janai.spatial.hd2x.v1");
  requested.backend = Backend::DirectML;
  snapshot.requested = requested;
  NativeEffectivePlan plan;
  plan.recipe = QStringLiteral("janai.spatial.hd2x.v1");
  plan.provider = QStringLiteral("tensorrt");
  plan.model = QStringLiteral("2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16");
  plan.spatialStages = 1;
  plan.order = QStringLiteral("spatial-then-temporal");
  plan.sourceSize = NativeSize{1920, 1080};
  plan.outputSize = NativeSize{1920, 1080};
  plan.sourceFps = Rational{24, 1};
  plan.outputFps = Rational{24, 1};
  plan.decoder = QStringLiteral("nvdec/cuda");
  plan.csp = QStringLiteral("bt709/limited");
  snapshot.effective = plan;
  NativeCadenceCounters counters;
  counters.framesIn = 3100;
  counters.framesOut = 3100;
  counters.dropped = 0;
  counters.interpolated = 0;
  counters.sceneCuts = 3;
  snapshot.counters = counters;
  return snapshot;
}

// A combined RIFE 2x chain, whose cadence and counters must be genuinely 2x.
StatusSnapshot combinedTwoTimes() {
  StatusSnapshot snapshot = oneToOneSpatial();
  snapshot.revision = 500;
  NativeEffectivePlan plan = *snapshot.effective;
  plan.interpolation = NativeInterpolation{Rational{2, 1}, QStringLiteral("rife_v4.14")};
  plan.outputSize = NativeSize{3840, 2160};
  plan.outputFps = Rational{48, 1};
  snapshot.effective = plan;
  NativeCadenceCounters counters;
  counters.framesIn = 1550;
  counters.framesOut = 3100;
  counters.interpolated = 1550;
  counters.sceneCuts = 2;
  snapshot.counters = counters;
  return snapshot;
}

// The one shape a usable factory observation may take: an executed query that
// answered success on a real backend, against a runtime and a measured device it
// actually checked. Every field a consumer reads is stated, so a regression that
// lets any of them be dropped or defaulted is caught by name.
NativeFactoryObservation aUsableFactory() {
  NativeFactoryObservation factory;
  factory.observed = true;
  factory.backend = QStringLiteral("tensorrt");
  factory.spatial = true;
  factory.runtimeVersion = QStringLiteral("TensorRT-10.4.0");
  factory.deviceIdentity = QStringLiteral("9f2c1a440b7d4e219c3a1d5e6f708192");
  factory.smMajor = 8;
  factory.smMinor = 9;
  return factory;
}

// The same query, observed, but answered "not usable" with a named reason: no
// role, and - as the producer publishes an unavailable observation - no device it
// could claim was checked.
NativeFactoryObservation anObservedUnavailableFactory() {
  NativeFactoryObservation factory;
  factory.observed = true;
  factory.backend = QStringLiteral("tensorrt");
  factory.runtimeVersion = QStringLiteral("TensorRT-10.4.0");
  factory.reasonKey = QStringLiteral("native.factory.engineMissing");
  factory.reason = QStringLiteral("no compiled engine for this graph");
  return factory;
}

QJsonObject factoryObjectOf(const StatusSnapshot& snapshot) {
  return statusSnapshotToJson(snapshot).value(QStringLiteral("factory")).toObject();
}

QJsonObject documentWithFactory(const StatusSnapshot& snapshot, const QJsonObject& factory) {
  QJsonObject document = statusSnapshotToJson(snapshot);
  document.insert(QStringLiteral("factory"), factory);
  return document;
}

// The measurable verdict of the parser: the named reason key, or "<accepted>".
QString refusalOf(const QJsonObject& document) {
  StatusSnapshot parsed;
  QString key;
  QString text;
  if (parseStatusSnapshot(document, &parsed, &key, &text)) return QStringLiteral("<accepted>");
  return key;
}

QString refusalOfTags(const NodeValue& tags) {
  StatusSnapshot parsed;
  QString key;
  QString text;
  if (parseStatusTags(tags, &parsed, &key, &text)) return QStringLiteral("<accepted>");
  return key;
}

// The verdict over the exact bytes the writer produced: an out-of-bound integer
// must survive as itself and then be refused, not be rounded into the bound.
QString refusalOfEncoding(const StatusSnapshot& snapshot) {
  StatusSnapshot parsed;
  QString key;
  QString text;
  if (parseStatusSnapshot(serializeStatusSnapshot(snapshot), &parsed, &key, &text)) {
    return QStringLiteral("<accepted>");
  }
  return key;
}

NodeValue* tag(NodeValue& tags, const QString& key) {
  for (std::pair<std::string, NodeValue>& entry : tags.map) {
    if (entry.first == key.toStdString()) return &entry.second;
  }
  return nullptr;
}

void dropTag(NodeValue& tags, const QString& key) {
  for (auto it = tags.map.begin(); it != tags.map.end(); ++it) {
    if (it->first == key.toStdString()) {
      tags.map.erase(it);
      return;
    }
  }
}

// Drives a real core the way the player's 50 ms pump does.
template <typename Predicate>
bool pumpUntil(MpvHandle& handle, Predicate ready, int timeoutMs = 5000) {
  QElapsedTimer clock;
  clock.start();
  while (!ready() && clock.elapsed() < timeoutMs) {
    handle.drainEvents();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(5);
  }
  return ready();
}

MpvHandle makeHandle() {
  return MpvHandle({{"vo", "libmpv"}, {"idle", "yes"}, {"input-default-bindings", "no"}});
}

}  // namespace

// ── vocabulary ─────────────────────────────────────────────────────────────

void TestNativeStatus::theStateVocabularyIsExactAndRefusesAnythingElse() {
  const QVector<NativeState> states = allNativeStates();
  QCOMPARE(states.size(), 9);
  QCOMPARE(stableName(NativeState::Off), QStringLiteral("Off"));
  QCOMPARE(stableName(NativeState::Probe), QStringLiteral("Probe"));
  QCOMPARE(stableName(NativeState::Preparing), QStringLiteral("Preparing"));
  QCOMPARE(stableName(NativeState::NeedsPreparation), QStringLiteral("NeedsPreparation"));
  QCOMPARE(stableName(NativeState::Active), QStringLiteral("Active"));
  QCOMPARE(stableName(NativeState::Suspended), QStringLiteral("Suspended"));
  QCOMPARE(stableName(NativeState::Failed), QStringLiteral("Failed"));
  QCOMPARE(stableName(NativeState::Unsupported), QStringLiteral("Unsupported"));
  QCOMPARE(stableName(NativeState::Deactivated), QStringLiteral("Deactivated"));

  for (const NativeState state : states) {
    QCOMPARE(nativeStateFromName(stableName(state)), std::optional<NativeState>(state));
  }
  // An ordinal, a differently cased spelling and a name from a newer schema are
  // three ways to be unparseable, and none may resolve to the first enumerator.
  QCOMPARE(nativeStateFromName(QStringLiteral("active")), std::optional<NativeState>());
  QCOMPARE(nativeStateFromName(QStringLiteral("4")), std::optional<NativeState>());
  QCOMPARE(nativeStateFromName(QStringLiteral("Switching")), std::optional<NativeState>());
  QCOMPARE(nativeStateFromName(QString()), std::optional<NativeState>());

  QVERIFY(nativeStateNeedsPolling(NativeState::Active));
  QVERIFY(nativeStateNeedsPolling(NativeState::Preparing));
  QVERIFY(!nativeStateNeedsPolling(NativeState::Suspended));
  QVERIFY(!nativeStateNeedsPolling(NativeState::Probe));
  QVERIFY(!nativeStateNeedsPolling(NativeState::Off));
  QVERIFY(!nativeStateNeedsPolling(NativeState::Failed));
}

// ── two layers ─────────────────────────────────────────────────────────────

void TestNativeStatus::aTagMapAndItsSnapshotMustAgreeOnSchemaProtocolAndRevision() {
  const StatusSnapshot document = oneToOneSpatial();
  StatusSnapshot parsed;
  QString key;
  QString text;
  QVERIFY2(parseStatusTags(statusTags(document), &parsed, &key, &text), qPrintable(text));
  QCOMPARE(parsed, document);
  QCOMPARE(parsed.version, kNativeStatusProtocol);

  NodeValue tags = statusTags(document);
  NodeValue* revision = tag(tags, QStringLiteral("revision"));
  QVERIFY(revision != nullptr);
  revision->text = "413";
  QCOMPARE(refusalOfTags(tags), QStringLiteral("native.status.snapshot.mismatch"));

  // The inner document is the authority for its own identity: a protocol the
  // tags do not declare is refused even when the tags look current.
  QJsonObject inner = statusSnapshotToJson(document);
  inner.insert(QStringLiteral("protocol"), 2);
  QCOMPARE(refusalOf(inner), QStringLiteral("native.status.protocol.unknown"));
  inner = statusSnapshotToJson(document);
  inner.insert(QStringLiteral("revision"), 999);
  QCOMPARE(refusalOf(inner), QStringLiteral("<accepted>"));
}

void TestNativeStatus::anUnknownSchemaOrProtocolIsRefusedWithBothVersionsNamed() {
  const StatusSnapshot document = oneToOneSpatial();

  QJsonObject inner = statusSnapshotToJson(document);
  inner.insert(QStringLiteral("schema"), QStringLiteral("mikai.native.status/2"));
  QCOMPARE(refusalOf(inner), QStringLiteral("native.status.schema.unknown"));
  inner = statusSnapshotToJson(document);
  inner.insert(QStringLiteral("schema"), QStringLiteral("mikai.native.recipe/1"));
  QCOMPARE(refusalOf(inner), QStringLiteral("native.status.schema.unknown"));

  NodeValue tags = statusTags(document);
  NodeValue* schema = tag(tags, QStringLiteral("schema"));
  QVERIFY(schema != nullptr);
  schema->text = "mikai.native.status/2";
  QCOMPARE(refusalOfTags(tags), QStringLiteral("native.status.schema.unknown"));

  tags = statusTags(document);
  NodeValue* protocol = tag(tags, QStringLiteral("protocol"));
  QVERIFY(protocol != nullptr);
  protocol->text = "2";
  StatusSnapshot parsed;
  QString key;
  QString text;
  if (parseStatusTags(tags, &parsed, &key, &text)) {
    QFAIL("a protocol this build does not speak was accepted");
  }
  QCOMPARE(key, QStringLiteral("native.status.protocol.unknown"));

  tags = statusTags(document);
  protocol = tag(tags, QStringLiteral("protocol"));
  protocol->text = "one";
  QCOMPARE(refusalOfTags(tags), QStringLiteral("native.status.protocol.unknown"));
  tags = statusTags(document);
  protocol = tag(tags, QStringLiteral("protocol"));
  protocol->text = " 1";
  QCOMPARE(refusalOfTags(tags), QStringLiteral("native.status.protocol.unknown"));

  inner = statusSnapshotToJson(document);
  inner.insert(QStringLiteral("revision"), -1);
  QCOMPARE(refusalOf(inner), QStringLiteral("native.status.revision.invalid"));
  inner = statusSnapshotToJson(document);
  inner.insert(QStringLiteral("revision"), 1.5);
  QCOMPARE(refusalOf(inner), QStringLiteral("native.status.revision.invalid"));
  inner = statusSnapshotToJson(document);
  inner.insert(QStringLiteral("revision"), QStringLiteral("412"));
  QCOMPARE(refusalOf(inner), QStringLiteral("native.status.revision.invalid"));
}

void TestNativeStatus::aMalformedSnapshotIsRefusedRatherThanPartiallyAccepted() {
  NodeValue tags = statusTags(oneToOneSpatial());
  NodeValue* snapshot = tag(tags, QStringLiteral("snapshot"));
  QVERIFY(snapshot != nullptr);

  snapshot->text = "{ not json";
  QCOMPARE(refusalOfTags(tags), QStringLiteral("native.status.snapshot.malformed"));
  snapshot->text = "[]";
  QCOMPARE(refusalOfTags(tags), QStringLiteral("native.status.snapshot.malformed"));
  snapshot->text = "42";
  QCOMPARE(refusalOfTags(tags), QStringLiteral("native.status.snapshot.malformed"));
  snapshot->text.clear();
  QCOMPARE(refusalOfTags(tags), QStringLiteral("native.status.snapshot.missing"));

  QJsonObject inner = statusSnapshotToJson(oneToOneSpatial());
  inner.remove(QStringLiteral("state"));
  QCOMPARE(refusalOf(inner), QStringLiteral("native.status.state.unknown"));
  inner = statusSnapshotToJson(oneToOneSpatial());
  inner.insert(QStringLiteral("state"), 7);
  QCOMPARE(refusalOf(inner), QStringLiteral("native.status.state.unknown"));
  inner = statusSnapshotToJson(oneToOneSpatial());
  inner.remove(QStringLiteral("reason"));
  QCOMPARE(refusalOf(inner), QStringLiteral("native.status.reason.malformed"));
  inner = statusSnapshotToJson(oneToOneSpatial());
  inner.insert(QStringLiteral("capabilities"), 1);
  QCOMPARE(refusalOf(inner), QStringLiteral("native.status.unknown.member"));
  // The refused document is never half-written into the caller's snapshot.
  StatusSnapshot parsed = oneToOneSpatial();
  QString key;
  QString text;
  QVERIFY(!parseStatusSnapshot(inner, &parsed, &key, &text));
  QCOMPARE(parsed, StatusSnapshot{});
}

// The inner document carries real numbers, and every integer it states must
// survive the writer's bytes and the reader exactly. A reader that went through a
// double, or a writer that emitted the number as a string, crosses this boundary
// silently: 2^53+1 rounds onto 2^53 and is accepted as a revision, epoch or count
// that never was.
void TestNativeStatus::exactIntegersSurviveTheWireAndOversizedOnesAreRefused() {
  constexpr qint64 exact = kNativeStatusMaxExactInteger;       // 2^53
  constexpr qint64 oversized = kNativeStatusMaxExactInteger + 1;

  // Every integer member holds the bound at once and comes back identical. The
  // spatial-only chain keeps framesOut == framesIn and interpolated == 0.
  StatusSnapshot spatial = oneToOneSpatial();
  spatial.revision = exact;
  spatial.epoch = exact;
  spatial.counters->framesIn = exact;
  spatial.counters->framesOut = exact;
  spatial.counters->dropped = exact;
  spatial.counters->sceneCuts = exact;
  StatusSnapshot spatialBack;
  QString key;
  QString text;
  QVERIFY2(parseStatusSnapshot(serializeStatusSnapshot(spatial), &spatialBack, &key, &text),
           qPrintable(text));
  QCOMPARE(spatialBack, spatial);
  QCOMPARE(spatialBack.revision, exact);
  QCOMPARE(spatialBack.epoch, std::optional<qint64>(exact));

  // A combined chain is where interpolated carries a value of its own. At 2x the
  // RIFE grid emits one original per source frame, so the truthful counts hold the
  // exact-integer bound for `framesOut` while half the source frames are
  // synthesized.
  StatusSnapshot combined = combinedTwoTimes();
  combined.revision = exact;
  combined.epoch = exact;
  combined.counters->framesIn = exact / 2;
  combined.counters->framesOut = exact;
  combined.counters->interpolated = exact / 2;
  StatusSnapshot combinedBack;
  QVERIFY2(parseStatusSnapshot(serializeStatusSnapshot(combined), &combinedBack, &key, &text),
           qPrintable(text));
  QCOMPARE(combinedBack, combined);

  StatusSnapshot outOfBound = oneToOneSpatial();
  outOfBound.revision = oversized;
  QCOMPARE(refusalOfEncoding(outOfBound), QStringLiteral("native.status.revision.invalid"));

  outOfBound = oneToOneSpatial();
  outOfBound.epoch = oversized;
  QCOMPARE(refusalOfEncoding(outOfBound), QStringLiteral("native.status.epoch.invalid"));

  const auto refusedCounter = [](qint64 NativeCadenceCounters::*field) {
    StatusSnapshot snapshot = oneToOneSpatial();
    (*snapshot.counters).*field = kNativeStatusMaxExactInteger + 1;
    return refusalOfEncoding(snapshot);
  };
  QCOMPARE(refusedCounter(&NativeCadenceCounters::framesIn),
           QStringLiteral("native.status.counters.malformed"));
  QCOMPARE(refusedCounter(&NativeCadenceCounters::framesOut),
           QStringLiteral("native.status.counters.malformed"));
  QCOMPARE(refusedCounter(&NativeCadenceCounters::dropped),
           QStringLiteral("native.status.counters.malformed"));
  QCOMPARE(refusedCounter(&NativeCadenceCounters::interpolated),
           QStringLiteral("native.status.counters.malformed"));
  QCOMPARE(refusedCounter(&NativeCadenceCounters::sceneCuts),
           QStringLiteral("native.status.counters.malformed"));
}

void TestNativeStatus::theOuterFlatMapMustStateItsOwnTyping() {
  const StatusSnapshot document = oneToOneSpatial();

  NodeValue tags = statusTags(document);
  dropTag(tags, QStringLiteral("snapshot"));
  QCOMPARE(refusalOfTags(tags), QStringLiteral("native.status.tags.incomplete"));
  tags = statusTags(document);
  dropTag(tags, QStringLiteral("revision"));
  QCOMPARE(refusalOfTags(tags), QStringLiteral("native.status.tags.incomplete"));
  tags = statusTags(document);
  dropTag(tags, QStringLiteral("schema"));
  QCOMPARE(refusalOfTags(tags), QStringLiteral("native.status.tags.incomplete"));

  // A tag this schema does not define is a schema drift, not something to skip.
  tags = statusTags(document);
  NodeValue extra;
  extra.format = MPV_FORMAT_STRING;
  extra.text = "1";
  tags.map.emplace_back("capabilities", std::move(extra));
  QCOMPARE(refusalOfTags(tags), QStringLiteral("native.status.tags.unknown"));

  // Tags are strings; a number in the flat layer is exactly the typing a reader
  // would guess wrong.
  tags = statusTags(document);
  NodeValue* numericRevision = tag(tags, QStringLiteral("revision"));
  QVERIFY(numericRevision != nullptr);
  numericRevision->format = MPV_FORMAT_INT64;
  QCOMPARE(refusalOfTags(tags), QStringLiteral("native.status.tags.type"));

  tags = statusTags(document);
  tags.format = MPV_FORMAT_STRING;
  QCOMPARE(refusalOfTags(tags), QStringLiteral("native.status.tags.shape"));

  // An absent property reaches the parser as an empty node: no snapshot, named.
  NodeValue absent;
  absent.format = MPV_FORMAT_NONE;
  QCOMPARE(refusalOfTags(absent), QStringLiteral("native.status.missing"));
  NodeValue empty;
  empty.format = MPV_FORMAT_NODE_MAP;
  QCOMPARE(refusalOfTags(empty), QStringLiteral("native.status.tags.incomplete"));
}

void TestNativeStatus::aOneToOneSpatialChainStillCarriesRequestedAndEffectiveSeparately() {
  const StatusSnapshot document = oneToOneSpatial();
  StatusSnapshot parsed;
  QString key;
  QString text;
  QVERIFY2(parseStatusSnapshot(statusSnapshotToJson(document), &parsed, &key, &text),
           qPrintable(text));

  QCOMPARE(parsed.state, NativeState::Active);
  QVERIFY(parsed.requested.has_value());
  QVERIFY(parsed.effective.has_value());
  // Geometry says nothing here: both sides are the same pixels and the same
  // cadence, so the requested/effective split is the only evidence available.
  QCOMPARE(parsed.effective->sourceSize, parsed.effective->outputSize);
  QCOMPARE(parsed.effective->sourceFps, parsed.effective->outputFps);
  QVERIFY(parsed.effective->isSpatialOnly());
  // ... and it survives: the requested backend is not rewritten into the
  // provider the filter reports running.
  QCOMPARE(parsed.requested->backend, Backend::DirectML);
  QCOMPARE(parsed.requested->recipeId, QStringLiteral("janai.spatial.hd2x.v1"));
  QCOMPARE(parsed.effective->provider, QStringLiteral("tensorrt"));
  QCOMPARE(parsed.effective->model,
           QStringLiteral("2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16"));
  QCOMPARE(parsed.epoch, std::optional<qint64>(7));
  QVERIFY(parsed.reason.isEmpty());

  // Round trip through both layers is lossless.
  StatusSnapshot again;
  QVERIFY(parseStatusTags(statusTags(parsed), &again, &key, &text));
  QCOMPARE(again, parsed);

  // A requested recipe that is not a usable id is refused rather than shown.
  QJsonObject inner = statusSnapshotToJson(document);
  QJsonObject requested = inner.value(QStringLiteral("requested")).toObject();
  requested.insert(QStringLiteral("recipe"), QStringLiteral("janai spatial"));
  inner.insert(QStringLiteral("requested"), requested);
  QCOMPARE(refusalOf(inner), QStringLiteral("native.status.requested.recipe.invalid"));
  requested.insert(QStringLiteral("recipe"), QString());
  requested.insert(QStringLiteral("backend"), QStringLiteral("cuda"));
  inner.insert(QStringLiteral("requested"), requested);
  QCOMPARE(refusalOf(inner), QStringLiteral("native.status.requested.backend.unknown"));
  // An empty recipe is the untouched Original pipeline, which is a real value.
  requested.insert(QStringLiteral("backend"), QStringLiteral("tensorrt"));
  inner.insert(QStringLiteral("requested"), requested);
  QCOMPARE(refusalOf(inner), QStringLiteral("<accepted>"));
}

void TestNativeStatus::aCadenceOrGeometryThatContradictsTheStageShapeIsRefused() {
  StatusSnapshot document = oneToOneSpatial();
  QJsonObject inner = statusSnapshotToJson(document);
  QJsonObject effective = inner.value(QStringLiteral("effective")).toObject();

  // A spatial-only chain cannot change the cadence.
  QJsonObject fps;
  fps.insert(QStringLiteral("num"), 48);
  fps.insert(QStringLiteral("den"), 1);
  QJsonObject wrong = effective;
  wrong.insert(QStringLiteral("outputFps"), fps);
  QJsonObject documentWithWrongFps = inner;
  documentWithWrongFps.insert(QStringLiteral("effective"), wrong);
  QCOMPARE(refusalOf(documentWithWrongFps), QStringLiteral("native.status.effective.cadence"));

  // ... nor a combined one keep it.
  QJsonObject combined = statusSnapshotToJson(combinedTwoTimes());
  QJsonObject combinedEffective = combined.value(QStringLiteral("effective")).toObject();
  combinedEffective.insert(QStringLiteral("outputFps"), effective.value(QStringLiteral("outputFps")));
  combined.insert(QStringLiteral("effective"), combinedEffective);
  QCOMPARE(refusalOf(combined), QStringLiteral("native.status.effective.cadence"));

  // Interpolation cannot move geometry: a combined 2x chain whose spatial stage
  // has been removed keeps a valid cadence (48 = 2 * 24) but must report the
  // source size, so only the geometry rule can refuse it.
  QJsonObject rifeOnly = statusSnapshotToJson(combinedTwoTimes());
  QJsonObject rifeOnlyEffective = rifeOnly.value(QStringLiteral("effective")).toObject();
  rifeOnlyEffective.insert(QStringLiteral("spatialStages"), 0);
  rifeOnly.insert(QStringLiteral("effective"), rifeOnlyEffective);
  QCOMPARE(refusalOf(rifeOnly), QStringLiteral("native.status.effective.geometry"));

  // A plan with no spatial stage and no interpolation has nothing to run.
  QJsonObject nothing = effective;
  nothing.insert(QStringLiteral("spatialStages"), 0);
  QJsonObject nothingDocument = inner;
  nothingDocument.insert(QStringLiteral("effective"), nothing);
  QCOMPARE(refusalOf(nothingDocument), QStringLiteral("native.status.effective.stages"));

  // A zero side is not a geometry to clamp.
  QJsonObject zeroSize;
  zeroSize.insert(QStringLiteral("w"), 0);
  zeroSize.insert(QStringLiteral("h"), 1080);
  QJsonObject zeroDocument = inner;
  QJsonObject zeroEffective = effective;
  zeroEffective.insert(QStringLiteral("sourceSize"), zeroSize);
  zeroDocument.insert(QStringLiteral("effective"), zeroEffective);
  QCOMPARE(refusalOf(zeroDocument), QStringLiteral("native.status.effective.geometry"));

  // A rate whose components are not usable rates.
  QJsonObject zeroRate;
  zeroRate.insert(QStringLiteral("num"), 0);
  zeroRate.insert(QStringLiteral("den"), 1);
  QJsonObject zeroRateEffective = effective;
  zeroRateEffective.insert(QStringLiteral("sourceFps"), zeroRate);
  QJsonObject zeroRateDocument = inner;
  zeroRateDocument.insert(QStringLiteral("effective"), zeroRateEffective);
  QCOMPARE(refusalOf(zeroRateDocument), QStringLiteral("native.status.effective.cadence"));

  QJsonObject hugeRate;
  hugeRate.insert(QStringLiteral("num"), kNativeStatusMaxRateComponent + 1);
  hugeRate.insert(QStringLiteral("den"), 1);
  QJsonObject hugeRateEffective = effective;
  hugeRateEffective.insert(QStringLiteral("sourceFps"), hugeRate);
  QJsonObject hugeRateDocument = inner;
  hugeRateDocument.insert(QStringLiteral("effective"), hugeRateEffective);
  QCOMPARE(refusalOf(hugeRateDocument), QStringLiteral("native.status.effective.cadence"));

  // Missing required effective members are named, never defaulted.
  QJsonObject providerless = effective;
  providerless.remove(QStringLiteral("provider"));
  QJsonObject providerlessDocument = inner;
  providerlessDocument.insert(QStringLiteral("effective"), providerless);
  QCOMPARE(refusalOf(providerlessDocument),
           QStringLiteral("native.status.effective.provider.missing"));
  QJsonObject interpMemberless = statusSnapshotToJson(combinedTwoTimes());
  QJsonObject interpEffective = interpMemberless.value(QStringLiteral("effective")).toObject();
  QJsonObject interpolationMember = interpEffective.value(QStringLiteral("interpolation")).toObject();
  interpolationMember.insert(QStringLiteral("model"), QString());
  interpEffective.insert(QStringLiteral("interpolation"), interpolationMember);
  interpMemberless.insert(QStringLiteral("effective"), interpEffective);
  QCOMPARE(refusalOf(interpMemberless),
           QStringLiteral("native.status.effective.interpolation.model.missing"));
}

void TestNativeStatus::absentRatesAreRefusedForActiveAndNeverSerialisedAsZero() {
  // The protocol requires both rates on an effective plan. An absent one is a
  // named refusal, never a default the filter did not publish.
  const QJsonObject inner = statusSnapshotToJson(oneToOneSpatial());
  const QJsonObject effective = inner.value(QStringLiteral("effective")).toObject();

  QJsonObject sourceLess = effective;
  sourceLess.remove(QStringLiteral("sourceFps"));
  QJsonObject sourceLessDocument = inner;
  sourceLessDocument.insert(QStringLiteral("effective"), sourceLess);
  QCOMPARE(refusalOf(sourceLessDocument), QStringLiteral("native.status.effective.cadence"));

  QJsonObject outputLess = effective;
  outputLess.remove(QStringLiteral("outputFps"));
  QJsonObject outputLessDocument = inner;
  outputLessDocument.insert(QStringLiteral("effective"), outputLess);
  QCOMPARE(refusalOf(outputLessDocument), QStringLiteral("native.status.effective.cadence"));

  // ... and an absent rate is omitted on the way out too: no zero and no 1/1
  // sentinel is invented for a plan that never carried one.
  NativeEffectivePlan absent;
  absent.recipe = QStringLiteral("janai.spatial.hd2x.v1");
  absent.provider = QStringLiteral("tensorrt");
  absent.model = QStringLiteral("2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16");
  absent.spatialStages = 1;
  absent.order = QStringLiteral("spatial-then-temporal");
  absent.sourceSize = NativeSize{1920, 1080};
  absent.outputSize = NativeSize{1920, 1080};
  absent.decoder = QStringLiteral("nvdec/cuda");
  absent.csp = QStringLiteral("bt709/limited");
  StatusSnapshot snapshot;
  snapshot.version = kNativeStatusProtocol;
  snapshot.revision = 1;
  snapshot.state = NativeState::Active;
  snapshot.effective = absent;

  const QJsonObject serialised = statusSnapshotToJson(snapshot);
  const QJsonObject serialisedEffective = serialised.value(QStringLiteral("effective")).toObject();
  QVERIFY(serialisedEffective.value(QStringLiteral("sourceFps")).isUndefined());
  QVERIFY(serialisedEffective.value(QStringLiteral("outputFps")).isUndefined());
}

void TestNativeStatus::countersMustAgreeWithTheEffectivePlan() {
  StatusSnapshot document = oneToOneSpatial();
  QJsonObject inner = statusSnapshotToJson(document);
  QJsonObject counters = inner.value(QStringLiteral("counters")).toObject();

  QJsonObject interpolated = counters;
  interpolated.insert(QStringLiteral("interpolated"), 5);
  QJsonObject withInterpolated = inner;
  withInterpolated.insert(QStringLiteral("counters"), interpolated);
  QCOMPARE(refusalOf(withInterpolated), QStringLiteral("native.status.counters.inconsistent"));

  QJsonObject mismatched = counters;
  mismatched.insert(QStringLiteral("framesOut"), 3099);
  QJsonObject withMismatched = inner;
  withMismatched.insert(QStringLiteral("counters"), mismatched);
  QCOMPARE(refusalOf(withMismatched), QStringLiteral("native.status.counters.inconsistent"));

  QJsonObject negative = counters;
  negative.insert(QStringLiteral("dropped"), -1);
  QJsonObject withNegative = inner;
  withNegative.insert(QStringLiteral("counters"), negative);
  QCOMPARE(refusalOf(withNegative), QStringLiteral("native.status.counters.malformed"));

  // A combined chain cannot interpolate more frames than it read.
  QJsonObject combined = statusSnapshotToJson(combinedTwoTimes());
  QJsonObject combinedCounters = combined.value(QStringLiteral("counters")).toObject();
  combinedCounters.insert(QStringLiteral("interpolated"), 9000);
  combined.insert(QStringLiteral("counters"), combinedCounters);
  QCOMPARE(refusalOf(combined), QStringLiteral("native.status.counters.inconsistent"));

  // Counters with no effective plan describe frames nobody is processing. The
  // Probe state carries the reason every non-Active state requires, so the
  // counters rule is the only one left to fire.
  QJsonObject probe = statusSnapshotToJson(document);
  probe.remove(QStringLiteral("effective"));
  probe.insert(QStringLiteral("state"), QStringLiteral("Probe"));
  probe.insert(QStringLiteral("reason"),
               QStringLiteral("native.probe.awaiting-frame: no decoded frame has been observed"));
  probe.insert(QStringLiteral("counters"), counters);
  QCOMPARE(refusalOf(probe), QStringLiteral("native.status.counters.without.effective"));
}

// `interpolated` counts the frames actually synthesized at the output pin, not
// the net gain over the source. The filter consumes source frames ahead of the
// pin, so a snapshot can be a behind-prefix transient; the bound that accepts
// every reachable state is framesIn*(num-1)/den (proved by the filter owner),
// with the emitted total as a further limit. It must accept the true 5/2, 5/3 and
// 4/3 counts and the real behind-prefix tuples, refuse one above the general safe
// upper, and stay exact on large counts.
void TestNativeStatus::interpolationCountersFollowTheActualRationalFactor() {
  const auto combinedAtFactor = [](const Rational& factor, const Rational& sourceFps,
                                   const Rational& outputFps, qint64 framesIn, qint64 framesOut,
                                   qint64 interpolated) {
    StatusSnapshot snapshot = combinedTwoTimes();
    NativeEffectivePlan plan = *snapshot.effective;
    plan.interpolation = NativeInterpolation{factor, QStringLiteral("rife_v4.14")};
    plan.sourceFps = sourceFps;
    plan.outputFps = outputFps;
    snapshot.effective = plan;
    NativeCadenceCounters counters;
    counters.framesIn = framesIn;
    counters.framesOut = framesOut;
    counters.interpolated = interpolated;
    snapshot.counters = counters;
    return snapshot;
  };

  // Integer 4x/8x: one original per source frame, so 3 and 7 synthesized frames
  // per source frame are the exact capacity.
  StatusSnapshot four =
      combinedAtFactor(Rational{4, 1}, Rational{24, 1}, Rational{96, 1}, 800, 3200, 2400);
  QCOMPARE(refusalOf(statusSnapshotToJson(four)), QStringLiteral("<accepted>"));
  four.counters->interpolated = 2401;
  QCOMPARE(refusalOf(statusSnapshotToJson(four)),
           QStringLiteral("native.status.counters.inconsistent"));

  StatusSnapshot eight =
      combinedAtFactor(Rational{8, 1}, Rational{24, 1}, Rational{192, 1}, 400, 3200, 2800);
  QCOMPARE(refusalOf(statusSnapshotToJson(eight)), QStringLiteral("<accepted>"));
  eight.counters->interpolated = 2801;
  QCOMPARE(refusalOf(statusSnapshotToJson(eight)),
           QStringLiteral("native.status.counters.inconsistent"));

  // The producer's literal integer counts sit just below the ideal cap (the grid
  // origin is emitted alone), and must be accepted rather than refused as a
  // near-miss.
  StatusSnapshot fourReal =
      combinedAtFactor(Rational{4, 1}, Rational{24, 1}, Rational{96, 1}, 800, 3197, 2397);
  QCOMPARE(refusalOf(statusSnapshotToJson(fourReal)), QStringLiteral("<accepted>"));
  StatusSnapshot eightReal =
      combinedAtFactor(Rational{8, 1}, Rational{24, 1}, Rational{192, 1}, 400, 3193, 2793);
  QCOMPARE(refusalOf(statusSnapshotToJson(eightReal)), QStringLiteral("<accepted>"));
  StatusSnapshot twoReal =
      combinedAtFactor(Rational{2, 1}, Rational{24, 1}, Rational{48, 1}, 1550, 3099, 1549);
  QCOMPARE(refusalOf(statusSnapshotToJson(twoReal)), QStringLiteral("<accepted>"));

  // Behind-prefix transients: the current call's source frame is counted but its
  // outputs are not yet written, so the counts trail. These are real states, not
  // malformed ones.
  StatusSnapshot twoBehind =
      combinedAtFactor(Rational{2, 1}, Rational{24, 1}, Rational{48, 1}, 3, 3, 1);
  QCOMPARE(refusalOf(statusSnapshotToJson(twoBehind)), QStringLiteral("<accepted>"));
  StatusSnapshot fiveHalvesBehind =
      combinedAtFactor(Rational{5, 2}, Rational{2, 1}, Rational{5, 1}, 10, 20, 16);
  QCOMPARE(refusalOf(statusSnapshotToJson(fiveHalvesBehind)), QStringLiteral("<accepted>"));

  // 5/2 at 4 in: the completed grid emits 8 frames with 6 synthesized, and the
  // general safe upper for 4 consumed frames is 8.
  StatusSnapshot fiveHalves =
      combinedAtFactor(Rational{5, 2}, Rational{2, 1}, Rational{5, 1}, 4, 8, 6);
  QCOMPARE(refusalOf(statusSnapshotToJson(fiveHalves)), QStringLiteral("<accepted>"));
  fiveHalves.counters->interpolated = 9;
  QCOMPARE(refusalOf(statusSnapshotToJson(fiveHalves)),
           QStringLiteral("native.status.counters.inconsistent"));

  // 5/3 at 3 in: the completed grid emits 4 frames with 3 synthesized; the safe
  // upper is 4.
  StatusSnapshot fiveThirds =
      combinedAtFactor(Rational{5, 3}, Rational{3, 1}, Rational{5, 1}, 3, 4, 3);
  QCOMPARE(refusalOf(statusSnapshotToJson(fiveThirds)), QStringLiteral("<accepted>"));
  fiveThirds.counters->interpolated = 5;
  QCOMPARE(refusalOf(statusSnapshotToJson(fiveThirds)),
           QStringLiteral("native.status.counters.inconsistent"));

  // 4/3 at 4 in: the completed grid emits 5 frames with 3 synthesized; the safe
  // upper is 4.
  StatusSnapshot fourThirds =
      combinedAtFactor(Rational{4, 3}, Rational{3, 1}, Rational{4, 1}, 4, 5, 3);
  QCOMPARE(refusalOf(statusSnapshotToJson(fourThirds)), QStringLiteral("<accepted>"));
  fourThirds.counters->interpolated = 5;
  QCOMPARE(refusalOf(statusSnapshotToJson(fourThirds)),
           QStringLiteral("native.status.counters.inconsistent"));

  // A factor at or below one synthesizes nothing.
  StatusSnapshot stalled =
      combinedAtFactor(Rational{1, 1}, Rational{24, 1}, Rational{24, 1}, 1000, 1000, 1);
  QCOMPARE(refusalOf(statusSnapshotToJson(stalled)),
           QStringLiteral("native.status.counters.inconsistent"));

  // The existing 2x overcount stays refused.
  StatusSnapshot two = combinedTwoTimes();
  two.counters->interpolated = 1551;
  QCOMPARE(refusalOf(statusSnapshotToJson(two)),
           QStringLiteral("native.status.counters.inconsistent"));

  // Synthesized frames are a subset of the emitted frames.
  StatusSnapshot beyondOut =
      combinedAtFactor(Rational{4, 1}, Rational{24, 1}, Rational{96, 1}, 100, 50, 60);
  QCOMPARE(refusalOf(statusSnapshotToJson(beyondOut)),
           QStringLiteral("native.status.counters.inconsistent"));

  // More frames out than the grid lays down for the source frames is impossible.
  StatusSnapshot overOut =
      combinedAtFactor(Rational{4, 1}, Rational{24, 1}, Rational{96, 1}, 100, 401, 0);
  QCOMPARE(refusalOf(statusSnapshotToJson(overOut)),
           QStringLiteral("native.status.counters.inconsistent"));

  // Large legal counts stay exact: 2^40 source frames at 4x with the producer's
  // literal counts, and one past.
  const qint64 huge = qint64{1} << 40;
  StatusSnapshot large = combinedAtFactor(Rational{4, 1}, Rational{24, 1}, Rational{96, 1}, huge,
                                          4 * huge - 3, 3 * huge - 3);
  QCOMPARE(refusalOf(statusSnapshotToJson(large)), QStringLiteral("<accepted>"));
  large.counters->interpolated = 3 * huge + 1;
  QCOMPARE(refusalOf(statusSnapshotToJson(large)),
           QStringLiteral("native.status.counters.inconsistent"));

  // A 2^53 source count at a large rational factor overflows the naive
  // framesIn*num product; the split comparison accepts the legal count instead of
  // wrapping into a wrong refusal.
  StatusSnapshot wide =
      combinedAtFactor(Rational{1000000, 3}, Rational{3, 1}, Rational{1000000, 1},
                       kNativeStatusMaxExactInteger, kNativeStatusMaxExactInteger, 0);
  QCOMPARE(refusalOf(statusSnapshotToJson(wide)), QStringLiteral("<accepted>"));
}

void TestNativeStatus::aFailedRequestKeepsTheNewRequestAndTheOldEffectivePlan() {
  StatusSnapshot document = oneToOneSpatial();
  document.revision = 413;
  document.state = NativeState::Failed;
  document.reason = QStringLiteral("native.activate.model-missing: the requested model is not "
                                  "prepared");
  SelectionState requested;
  requested.recipeId = QStringLiteral("janai.spatial.hd3x.v1");
  requested.backend = Backend::TensorRT;
  document.requested = requested;

  StatusSnapshot parsed;
  QString key;
  QString text;
  QVERIFY2(parseStatusSnapshot(statusSnapshotToJson(document), &parsed, &key, &text),
           qPrintable(text));

  QCOMPARE(parsed.state, NativeState::Failed);
  QVERIFY(!parsed.reason.isEmpty());
  QVERIFY(parsed.requested.has_value());
  QVERIFY(parsed.effective.has_value());
  QCOMPARE(parsed.requested->recipeId, QStringLiteral("janai.spatial.hd3x.v1"));
  // The effective plan is still the one that was running, not the request that
  // failed: a reader that promoted it would show a plan nobody applied.
  QCOMPARE(parsed.effective->recipe, QStringLiteral("janai.spatial.hd2x.v1"));
  QCOMPARE(parsed.effective->model,
           QStringLiteral("2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16"));
  QVERIFY(parsed.requested->recipeId != parsed.effective->recipe);
  QCOMPARE(parsed.counters, document.counters);
}

void TestNativeStatus::theReasonIsEmptyExactlyForActive() {
  StatusSnapshot document = oneToOneSpatial();
  document.state = NativeState::Active;
  document.reason = QStringLiteral("native.recovered: something happened");
  QCOMPARE(refusalOf(statusSnapshotToJson(document)),
           QStringLiteral("native.status.reason.unexpected"));

  for (const NativeState state : {NativeState::Off, NativeState::Probe, NativeState::Preparing,
                                  NativeState::NeedsPreparation, NativeState::Suspended,
                                  NativeState::Failed, NativeState::Unsupported,
                                  NativeState::Deactivated}) {
    StatusSnapshot empty = oneToOneSpatial();
    empty.state = state;
    if (state == NativeState::Off || state == NativeState::Probe) {
      empty.effective.reset();
      empty.counters.reset();
    }
    empty.reason.clear();
    QCOMPARE(refusalOf(statusSnapshotToJson(empty)), QStringLiteral("native.status.reason.missing"));

    // Every state but Active carries a reason, and Active is the only one that
    // must not.
    empty.reason = QStringLiteral("native.state: a reason");
    QCOMPARE(refusalOf(statusSnapshotToJson(empty)), QStringLiteral("<accepted>"));
  }
}

void TestNativeStatus::anAbsentBlockIsNeverSynthesisedFromAnother() {
  StatusSnapshot probe;
  probe.version = kNativeStatusProtocol;
  probe.revision = 1;
  probe.state = NativeState::Probe;
  probe.epoch = 3;
  SelectionState requested;
  requested.recipeId = QStringLiteral("janai.spatial.hd2x.v1");
  requested.backend = Backend::TensorRT;
  probe.requested = requested;
  probe.reason = QStringLiteral("native.probe.awaiting-frame: no decoded frame has been observed");

  const QJsonObject document = statusSnapshotToJson(probe);
  QVERIFY(!document.contains(QStringLiteral("effective")));
  QVERIFY(!document.contains(QStringLiteral("counters")));

  StatusSnapshot parsed;
  QString key;
  QString text;
  QVERIFY2(parseStatusSnapshot(document, &parsed, &key, &text), qPrintable(text));
  QVERIFY(parsed.requested.has_value());
  QCOMPARE(parsed.effective.has_value(), false);
  QCOMPARE(parsed.counters.has_value(), false);
  QCOMPARE(parsed.epoch, std::optional<qint64>(3));
  // A second round trip keeps the absence absent.
  StatusSnapshot again;
  QVERIFY(parseStatusSnapshot(statusSnapshotToJson(parsed), &again, &key, &text));
  QCOMPARE(again, parsed);

  // A spatial-only effective document states interpolation as null, and that
  // null reads back as "no interpolation" rather than as a missing stage.
  const QJsonObject spatial =
      statusSnapshotToJson(oneToOneSpatial()).value(QStringLiteral("effective")).toObject();
  QVERIFY(spatial.contains(QStringLiteral("interpolation")));
  QVERIFY(spatial.value(QStringLiteral("interpolation")).isNull());
}

void TestNativeStatus::aRevisionRegressionIsStaleWhileANewSessionIsAccepted() {
  StatusSnapshot previous = oneToOneSpatial();
  StatusSnapshot next = previous;

  next.revision = previous.revision;
  QCOMPARE(classifyStatusRevision(previous, next), StatusRevision::Unchanged);

  next.revision = previous.revision + 1;
  QCOMPARE(classifyStatusRevision(previous, next), StatusRevision::Advanced);

  next.revision = previous.revision - 1;
  QCOMPARE(classifyStatusRevision(previous, next), StatusRevision::Stale);

  next.epoch = 8;
  QCOMPARE(classifyStatusRevision(previous, next), StatusRevision::Advanced);

  // No epoch on either side is not evidence of a new session: the counter is
  // then the only identity there is.
  previous.epoch.reset();
  next.epoch.reset();
  next.revision = 5;
  QCOMPARE(classifyStatusRevision(previous, next), StatusRevision::Stale);
  next.epoch = 2;
  QCOMPARE(classifyStatusRevision(previous, next), StatusRevision::Stale);
}

void TestNativeStatus::aSensitiveUrlOrCredentialIsRefusedAndNeverExported() {
  const QString url = QStringLiteral("https://cdn.example.invalid/graph.onnx?token=Zm9vYmFy");
  const QString key = QStringLiteral("mk_live_9f3c2a7d4b6e");

  // The document a filter publishes can carry a signed URL or an API key in any
  // of its text members. A planted value is refused by name, as one document,
  // with no member echoed back.
  QJsonObject document = statusSnapshotToJson(oneToOneSpatial());
  document.insert(QStringLiteral("state"), QStringLiteral("Failed"));
  document.insert(QStringLiteral("reason"), url);
  QJsonObject effective = document.value(QStringLiteral("effective")).toObject();
  effective.insert(QStringLiteral("provider"), key);
  effective.insert(QStringLiteral("decoder"), QStringLiteral("Bearer ") + key);
  effective.insert(QStringLiteral("csp"), QStringLiteral("X-API-Key: ") + key);
  document.insert(QStringLiteral("effective"), effective);

  StatusSnapshot parsed;
  QString errorKey;
  QString text;
  QVERIFY(!parseStatusSnapshot(document, &parsed, &errorKey, &text));
  QCOMPARE(errorKey, QStringLiteral("native.status.sensitiveData"));
  QCOMPARE(parsed, StatusSnapshot{});
  QVERIFY(!text.contains(url));
  QVERIFY(!text.contains(key));
  QVERIFY(!text.contains(QStringLiteral("Zm9vYmFy")));

  // A refusal that names an unknown tag planted with a secret does not repeat
  // the tag.
  NodeValue tags = statusTags(oneToOneSpatial());
  NodeValue plantedTag;
  plantedTag.format = MPV_FORMAT_STRING;
  plantedTag.text = "1";
  tags.map.emplace_back(url.toStdString(), std::move(plantedTag));
  StatusSnapshot tagParsed;
  QString tagKey;
  QString tagText;
  QVERIFY(!parseStatusTags(tags, &tagParsed, &tagKey, &tagText));
  QVERIFY(!tagText.contains(url));
  QVERIFY(!tagText.contains(QStringLiteral("Zm9vYmFy")));

  // The writer is the last boundary: a directly constructed, never-validated
  // snapshot that carries a credential still exports no plaintext ...
  StatusSnapshot planted = oneToOneSpatial();
  planted.state = NativeState::Failed;
  planted.reason = url;
  planted.effective->provider = key;
  planted.effective->decoder = QStringLiteral("Bearer ") + key;
  planted.effective->csp = QStringLiteral("X-API-Key: ") + key;
  const QByteArray exported = serializeStatusSnapshot(planted);
  QVERIFY(!exported.contains(url.toUtf8()));
  QVERIFY(!exported.contains(key.toUtf8()));
  // ... while the rest of the plan is still exported, so this is a redaction and
  // not a blank document.
  QVERIFY(exported.contains(planted.effective->model.toUtf8()));
  QVERIFY(exported.contains(planted.effective->recipe.toUtf8()));

  // A URL is redacted whole - its path and its query token together - so a
  // partial query can never survive.
  const QString withQuery =
      QStringLiteral("see https://host.invalid/path?token=SECRET123&x=1 now");
  const QString redacted = redactNativeDiagnosticText(withQuery);
  QVERIFY(!redacted.contains(QStringLiteral("host.invalid")));
  QVERIFY(!redacted.contains(QStringLiteral("SECRET123")));
  QVERIFY(!redacted.contains(QStringLiteral("token=")));
  QVERIFY(redacted.contains(QStringLiteral("see ")));
  QVERIFY(redacted.contains(QStringLiteral(" now")));
  QVERIFY(containsSensitiveNativeText(QStringLiteral("Bearer mk_live_9f3c2a7d4b6e")));
  QVERIFY(containsSensitiveNativeText(QStringLiteral("X-API-Key: abcdef123456")));

  // Ordinary Unicode, a Windows drive path, a comma and a percent label are not
  // secrets and pass through byte-for-byte.
  const QString ordinary = QStringLiteral("Résumé: C:\\models\\rtx 4090, 100% ready; ±5 ms");
  QVERIFY(!containsSensitiveNativeText(ordinary));
  QCOMPARE(redactNativeDiagnosticText(ordinary), ordinary);
  StatusSnapshot plain = oneToOneSpatial();
  plain.state = NativeState::Failed;
  plain.reason = ordinary;
  const QJsonObject plainJson = statusSnapshotToJson(plain);
  QCOMPARE(plainJson.value(QStringLiteral("reason")).toString(), ordinary);
  StatusSnapshot plainParsed;
  QString plainKey;
  QString plainText;
  QVERIFY2(parseStatusSnapshot(plainJson, &plainParsed, &plainKey, &plainText),
           qPrintable(plainText));
}

// ── the reader against a real core ─────────────────────────────────────────

void TestNativeStatus::aCoreWithoutTheFilterReportsUnavailableWithAReason() {
  if (!requireRuntime()) return;
  MpvHandle handle = makeHandle();
  NativeStatusConsumer consumer;
  consumer.attach(&handle);

  // Before any read the surface is unread, which is a different fact from the
  // filter being absent.
  QCOMPARE(consumer.availability().value, Availability::Unavailable);
  QVERIFY(!consumer.availability().reason.isEmpty());
  QCOMPARE(consumer.snapshot().has_value(), false);

  consumer.tick(0, 1, true);
  QCOMPARE(consumer.requestCount(), quint64{1});
  QVERIFY(pumpUntil(handle, [&consumer] { return consumer.requestCount() >= 1 &&
                                                  consumer.availability().reasonKey !=
                                                      QStringLiteral("native.status.unread"); }));

  // The property does not resolve without the filter: the reader says so by
  // name, with no snapshot presented as if it were current.
  QCOMPARE(consumer.availability().value, Availability::Unavailable);
  QCOMPARE(consumer.availability().reasonKey, QStringLiteral("native.status.missing"));
  QVERIFY(consumer.availability().reason.contains(QStringLiteral("mikai_native")));
  QCOMPARE(consumer.snapshot().has_value(), false);
  QCOMPARE(consumer.acceptedCount(), quint64{0});
}

void TestNativeStatus::oneOutstandingRequestIsReissuedAtTheBoundedCadence() {
  if (!requireRuntime()) return;
  MpvHandle handle = makeHandle();
  NativeStatusConsumer consumer;
  consumer.attach(&handle);
  int changes = 0;
  consumer.setChangeCallback([&changes](const NativeStatusView&) { ++changes; });

  consumer.tick(0, 1, true);
  QCOMPARE(consumer.requestCount(), quint64{1});
  // A second tick before the first reply must not stack a second request.
  consumer.tick(10, 1, true);
  QCOMPARE(consumer.requestCount(), quint64{1});

  QVERIFY(pumpUntil(handle, [&consumer] { return consumer.droppedCount() +
                                                     consumer.acceptedCount() > 0 ||
                                                 consumer.availability().reasonKey ==
                                                     QStringLiteral("native.status.missing"); }));
  const int observed = changes;
  QVERIFY(observed >= 1);

  // Inside the 500 ms window the poll stays quiet.
  consumer.tick(100, 1, true);
  QCOMPARE(consumer.requestCount(), quint64{1});
  consumer.tick(600, 1, true);
  QCOMPARE(consumer.requestCount(), quint64{2});

  // Native not Active/Preparing/Switching: no request at all.
  consumer.tick(5000, 1, false);
  QCOMPARE(consumer.requestCount(), quint64{2});
}

void TestNativeStatus::aLateReplyForASupersededEpochIsDropped() {
  if (!requireRuntime()) return;
  MpvHandle handle = makeHandle();
  NativeStatusConsumer consumer;
  consumer.attach(&handle);

  consumer.tick(0, 7, true);
  QCOMPARE(consumer.requestCount(), quint64{1});
  // The source generation moved before the reply landed: the answer describes a
  // plan that no longer applies.
  consumer.tick(1, 8, true);
  QCOMPARE(consumer.requestCount(), quint64{1});
  QVERIFY(pumpUntil(handle, [&consumer] { return consumer.droppedCount() + consumer.acceptedCount() > 0; }));
  QCOMPARE(consumer.droppedCount(), quint64{1});
  QCOMPARE(consumer.acceptedCount(), quint64{0});
  // The stale answer is not published as the current status.
  QCOMPARE(consumer.availability().reasonKey, QStringLiteral("native.status.unread"));
  QCOMPARE(consumer.snapshot().has_value(), false);
}

void TestNativeStatus::detachingBeforeTeardownCancelsTheOutstandingRequest() {
  if (!requireRuntime()) return;
  MpvHandle handle = makeHandle();
  NativeStatusConsumer consumer;
  consumer.attach(&handle);
  int changes = 0;
  consumer.setChangeCallback([&changes](const NativeStatusView&) { ++changes; });

  consumer.tick(0, 1, true);
  QCOMPARE(consumer.requestCount(), quint64{1});
  consumer.detach();
  QVERIFY(!consumer.attached());
  QCOMPARE(handle.pendingAsyncCount(), size_t{0});

  // The reply still arrives at the core; it must not reach the detached reader.
  pumpUntil(handle, [] { return false; }, 250);
  QCOMPARE(changes, 0);
  QCOMPARE(consumer.acceptedCount(), quint64{0});
  QCOMPARE(consumer.snapshot().has_value(), false);
  QCOMPARE(consumer.availability().reasonKey, QStringLiteral("native.status.unread"));

  // A detached reader issues nothing, and a rebound one starts fresh.
  consumer.tick(1000, 2, true);
  QCOMPARE(consumer.requestCount(), quint64{1});
  consumer.attach(&handle);
  consumer.tick(2000, 2, true);
  QCOMPARE(consumer.requestCount(), quint64{2});
}

bool TestNativeStatus::requireRuntime() {
  const QString module = qEnvironmentVariable("MIKAI_MPV_MODULE");
  if (MpvApi::instance().loaded()) return true;
  if (module.isEmpty()) {
    QTest::qSkip("set MIKAI_MPV_MODULE to the libmpv-2.dll of a real runtime to run this",
                 __FILE__, __LINE__);
    return false;
  }
  const QFileInfo info(module);
  MpvBundle bundle;
  bundle.root = info.absolutePath();
  bundle.modulePath = info.absoluteFilePath();
  bundle.descriptorId = QStringLiteral("native-status-test-runtime");
  bundle.protocolVersion = 1;
  QString reason;
  if (!MpvApi::instance().load(bundle, &reason)) {
    QTest::qFail(qPrintable(QStringLiteral("cannot select %1: %2").arg(bundle.modulePath, reason)),
                 __FILE__, __LINE__);
    return false;
  }
  return true;
}

void TestNativeStatus::detachingClearsTheViewSoNoPlanOutlivesItsCore() {
  if (!requireRuntime()) return;
  MpvHandle handle = makeHandle();
  NativeStatusConsumer consumer;
  consumer.attach(&handle);
  int changes = 0;
  consumer.setChangeCallback([&changes](const NativeStatusView&) { ++changes; });

  // Start unread.
  QCOMPARE(consumer.availability().value, Availability::Unavailable);
  QVERIFY(consumer.availability().reasonKey.contains(QStringLiteral("unread")));
  QCOMPARE(consumer.snapshot().has_value(), false);

  // Prime a change to something different from the initial unread state (if
  // anything happens) - but no filter attached, so it will become "missing".
  consumer.tick(0, 1, true);
  QVERIFY(pumpUntil(handle, [&consumer] { return consumer.availability().reasonKey !=
                                                  QStringLiteral("native.status.unread"); }));

  const QString beforeDetachKey = consumer.availability().reasonKey;
  QVERIFY(!beforeDetachKey.isEmpty());
  QCOMPARE(consumer.snapshot().has_value(), false);

  consumer.detach();
  QVERIFY(!consumer.attached());
  QCOMPARE(consumer.availability().value, Availability::Unavailable);
  QCOMPARE(consumer.availability().reasonKey, QStringLiteral("native.status.unread"));
  QCOMPARE(consumer.snapshot().has_value(), false);
}

void TestNativeStatus::aRefusedSnapshotHaltsThePollInsteadOfRepeatingIt() {
  NativeStatusConsumer consumer;
  NativeStatusView view;
  view.availability = unavailableAvailability(QStringLiteral("native.status.missing"),
                                             QStringLiteral("no filter"));
  // Simulate callback seeing it.
  int haltedFlag = 0;
  // We can't access nativeStatusHalted_ directly; instead verify the predicate.
  QVERIFY(statusRefusalHaltsPolling(QStringLiteral("native.status.missing")));
  QVERIFY(statusRefusalHaltsPolling(QStringLiteral("native.status.request")));
  QVERIFY(statusRefusalHaltsPolling(QStringLiteral("native.status.schema.unknown")));
  QVERIFY(statusRefusalHaltsPolling(QStringLiteral("native.status.protocol.unknown")));
  QVERIFY(statusRefusalHaltsPolling(QStringLiteral("native.status.tags.unknown")));
  QVERIFY(statusRefusalHaltsPolling(QStringLiteral("native.status.snapshot.mismatch")));
  QVERIFY(statusRefusalHaltsPolling(QStringLiteral("native.status.snapshot.malformed")));
  QVERIFY(statusRefusalHaltsPolling(QStringLiteral("native.status.sensitiveData")));
  QVERIFY(!statusRefusalHaltsPolling(QStringLiteral("native.status.reason.missing")));
  QVERIFY(!statusRefusalHaltsPolling(QStringLiteral("native.status.effective.geometry")));
}

void TestNativeStatus::aPreparingSnapshotIsPublishedAsPreparingAndNeverAsReady() {
  StatusSnapshot snapshot = oneToOneSpatial();
  snapshot.state = NativeState::Preparing;
  snapshot.reason = QStringLiteral("warming up");
  const AvailabilityState avail = statusSnapshotAvailability(snapshot);
  QCOMPARE(avail.value, Availability::Preparing);
  QCOMPARE(avail.reasonKey, QStringLiteral("native.status.preparing"));
  QCOMPARE(avail.reason, snapshot.reason);

  StatusSnapshot needs;
  needs = oneToOneSpatial();
  needs.state = NativeState::NeedsPreparation;
  needs.reason = QStringLiteral("needs prep");
  const AvailabilityState navail = statusSnapshotAvailability(needs);
  QCOMPARE(navail.value, Availability::Preparing);
  QCOMPARE(navail.reasonKey, QStringLiteral("native.status.needsPreparation"));
  QCOMPARE(navail.reason, needs.reason);

  StatusSnapshot active = oneToOneSpatial();
  const AvailabilityState aavail = statusSnapshotAvailability(active);
  QCOMPARE(aavail.value, Availability::Ready);
  QVERIFY(aavail.reason.isEmpty());
}

void TestNativeStatus::anEffectivePlanUnderAStateThatRunsNothingIsRefused() {
  StatusSnapshot probe = oneToOneSpatial();
  probe.state = NativeState::Probe;
  probe.reason = QStringLiteral("probing");
  QString key;
  QString text;
  QVERIFY(!statusSnapshotIsConsistent(probe, &key, &text));
  QCOMPARE(key, QStringLiteral("native.status.effective.unexpected"));

  StatusSnapshot off = oneToOneSpatial();
  off.state = NativeState::Off;
  off.reason = QStringLiteral("off");
  QVERIFY(!statusSnapshotIsConsistent(off, &key, &text));
  QCOMPARE(key, QStringLiteral("native.status.effective.unexpected"));

  StatusSnapshot failed = oneToOneSpatial();
  failed.state = NativeState::Failed;
  failed.reason = QStringLiteral("failed");
  QVERIFY(statusSnapshotIsConsistent(failed, &key, &text));
}

// Observed facts travel with the snapshot but never promote it: a Probe with
// real device and geometry stays Probe-flavoured Unavailable.
void TestNativeStatus::anObservedProbeRoundTripsAndIsNeverPromotedToReady() {
  StatusSnapshot snapshot;
  snapshot.version = kNativeStatusProtocol;
  snapshot.revision = 9;
  snapshot.state = NativeState::Probe;
  snapshot.epoch = 3;
  snapshot.reason = QStringLiteral("native.probe.awaiting-frame: passthrough");
  NativeProbePayload payload;
  SourceGeometry source;
  source.codedSize = QSize(1920, 1080);
  source.rotationDegrees = 0;
  source.fps = Rational{24000, 1001};
  source.csp = QStringLiteral("bt709");
  source.matrix = QStringLiteral("bt709");
  source.range = QStringLiteral("limited");
  source.siting = QStringLiteral("left");
  source.bitDepth = 8;
  payload.source = source;
  DeviceIdentity device;
  device.decoderDeviceUuid = QStringLiteral("GPU-9f2c1a44-0b7d-4e21-9c3a-1d5e6f70");
  payload.device = device;
  snapshot.probe = payload;

  StatusSnapshot parsed;
  QString key;
  QString text;
  QVERIFY2(parseStatusSnapshot(statusSnapshotToJson(snapshot), &parsed, &key, &text),
           qPrintable(text));
  QCOMPARE(parsed, snapshot);
  StatusSnapshot viaTags;
  QVERIFY2(parseStatusTags(statusTags(snapshot), &viaTags, &key, &text), qPrintable(text));
  QCOMPARE(viaTags, snapshot);
  const AvailabilityState avail = statusSnapshotAvailability(parsed);
  QCOMPARE(avail.value, Availability::Unavailable);
  QVERIFY(avail.reasonKey.contains(QStringLiteral("Probe")));
  QVERIFY(!parsed.effective.has_value());
  QVERIFY(!parsed.counters.has_value());
}

void TestNativeStatus::aMalformedProbeIdentityIsRefusedByName() {
  StatusSnapshot probe;
  probe.version = kNativeStatusProtocol;
  probe.revision = 9;
  probe.state = NativeState::Probe;
  probe.reason = QStringLiteral("native.probe.awaiting-frame: passthrough");
  NativeProbePayload payload;
  SourceGeometry source;
  source.codedSize = QSize(1920, 1080);
  payload.source = source;
  DeviceIdentity device;
  device.decoderDeviceUuid = QStringLiteral("unknown");
  payload.device = device;
  probe.probe = payload;
  QCOMPARE(refusalOf(statusSnapshotToJson(probe)),
           QStringLiteral("native.status.probe.identity"));

  probe.probe->device->decoderDeviceUuid = QStringLiteral("GPU-0");
  probe.probe->device->smMinor = 9;
  QCOMPARE(refusalOf(statusSnapshotToJson(probe)),
           QStringLiteral("native.status.probe.identity"));

  QJsonObject typed = statusSnapshotToJson(probe);
  QJsonObject inner = typed.value(QStringLiteral("probe")).toObject();
  QJsonObject dev = inner.value(QStringLiteral("device")).toObject();
  dev.insert(QStringLiteral("smMajor"), QStringLiteral("12"));
  inner.insert(QStringLiteral("device"), dev);
  typed.insert(QStringLiteral("probe"), inner);
  QCOMPARE(refusalOf(typed), QStringLiteral("native.status.probe.malformed"));

  QJsonObject unknown = statusSnapshotToJson(probe);
  QJsonObject unknownProbe = unknown.value(QStringLiteral("probe")).toObject();
  unknownProbe.insert(QStringLiteral("ordinal"), 0);
  unknown.insert(QStringLiteral("probe"), unknownProbe);
  QCOMPARE(refusalOf(unknown), QStringLiteral("native.status.probe.unknown.member"));
}

void TestNativeStatus::aContradictingProbeGeometryIsRefusedByName() {
  StatusSnapshot probe;
  probe.version = kNativeStatusProtocol;
  probe.revision = 9;
  probe.state = NativeState::Probe;
  probe.reason = QStringLiteral("native.probe.awaiting-frame: passthrough");
  NativeProbePayload payload;
  SourceGeometry source;
  source.codedSize = QSize(0, 1080);
  payload.source = source;
  probe.probe = payload;
  QCOMPARE(refusalOf(statusSnapshotToJson(probe)),
           QStringLiteral("native.status.probe.geometry"));

  probe.probe->source->codedSize = QSize(1920, 1080);
  probe.probe->source->crop = QRect(1700, 0, 400, 1080);
  QCOMPARE(refusalOf(statusSnapshotToJson(probe)),
           QStringLiteral("native.status.probe.geometry"));

  probe.probe->source->crop = QRect(0, 0, 1920, 1080);
  probe.probe->source->rotationDegrees = 45;
  QCOMPARE(refusalOf(statusSnapshotToJson(probe)),
           QStringLiteral("native.status.probe.geometry"));

  probe.probe->source->rotationDegrees = 0;
  probe.probe->source->fps = Rational{0, 1};
  QCOMPARE(refusalOf(statusSnapshotToJson(probe)),
           QStringLiteral("native.status.probe.cadence"));

  probe.probe->source->fps = Rational{24, 1};
  probe.probe->source->bitDepth = 7;
  QCOMPARE(refusalOf(statusSnapshotToJson(probe)),
           QStringLiteral("native.status.probe.geometry"));
}

void TestNativeStatus::aSensitiveProbeTagIsRefusedWithoutEcho() {
  const QString url = QStringLiteral("https://cdn.example.invalid/graph.onnx?token=Zm9vYmFy");
  StatusSnapshot probe;
  probe.version = kNativeStatusProtocol;
  probe.revision = 9;
  probe.state = NativeState::Probe;
  probe.reason = QStringLiteral("native.probe.awaiting-frame: passthrough");
  NativeProbePayload payload;
  SourceGeometry source;
  source.codedSize = QSize(1920, 1080);
  source.csp = url;
  payload.source = source;
  probe.probe = payload;
  StatusSnapshot parsed;
  QString key;
  QString text;
  QVERIFY(!parseStatusSnapshot(statusSnapshotToJson(probe), &parsed, &key, &text));
  QCOMPARE(key, QStringLiteral("native.status.sensitiveData"));
  QCOMPARE(parsed, StatusSnapshot{});
  QVERIFY(!text.contains(url));
  QVERIFY(!text.contains(QStringLiteral("Zm9vYmFy")));
  QVERIFY(!serializeStatusSnapshot(probe).contains(url.toUtf8()));
}

// ── the optional factory observation ───────────────────────────────────────

// Eligibility, not readiness: a factory observation is usable only when the
// filter actually ran the producer's query, answered success on a real backend,
// and named the runtime and measured device it checked. Each fact is required and
// never defaulted, and the same document round trips through both layers.
void TestNativeStatus::aUsableFactoryIsEarnedByAnObservedAnswerOnAMeasuredDevice() {
  StatusSnapshot snapshot = oneToOneSpatial();
  snapshot.factory = aUsableFactory();

  StatusSnapshot parsed;
  QString key;
  QString text;
  QVERIFY2(parseStatusSnapshot(statusSnapshotToJson(snapshot), &parsed, &key, &text),
           qPrintable(text));
  QCOMPARE(parsed, snapshot);
  QVERIFY(parsed.factory.has_value());
  QVERIFY(parsed.factory->isUsable());
  QVERIFY(parsed.factory->spatial);
  QVERIFY(!parsed.factory->rife);
  QCOMPARE(parsed.factory->backend, QStringLiteral("tensorrt"));
  QCOMPARE(parsed.factory->runtimeVersion, QStringLiteral("TensorRT-10.4.0"));
  QCOMPARE(parsed.factory->deviceIdentity, QStringLiteral("9f2c1a440b7d4e219c3a1d5e6f708192"));
  QCOMPARE(parsed.factory->smMajor, 8);
  QCOMPARE(parsed.factory->smMinor, 9);
  QVERIFY(parsed.factory->reasonKey.isEmpty());

  // The outer tag map carries the same observation back unchanged.
  StatusSnapshot viaTags;
  QVERIFY2(parseStatusTags(statusTags(snapshot), &viaTags, &key, &text), qPrintable(text));
  QCOMPARE(viaTags, snapshot);
  QVERIFY(viaTags.factory->isUsable());

  // Support can only be claimed through a runtime and a measured device it
  // actually checked: an absent one is refused by name, never read as an empty
  // string or a zero capability.
  QJsonObject noRuntime = factoryObjectOf(snapshot);
  noRuntime.remove(QStringLiteral("runtimeVersion"));
  QCOMPARE(refusalOf(documentWithFactory(snapshot, noRuntime)),
           QStringLiteral("native.status.factory.identity"));

  QJsonObject noDevice = factoryObjectOf(snapshot);
  noDevice.remove(QStringLiteral("deviceIdentity"));
  QCOMPARE(refusalOf(documentWithFactory(snapshot, noDevice)),
           QStringLiteral("native.status.factory.identity"));

  QJsonObject noCapability = factoryObjectOf(snapshot);
  noCapability.remove(QStringLiteral("smMajor"));
  noCapability.remove(QStringLiteral("smMinor"));
  QCOMPARE(refusalOf(documentWithFactory(snapshot, noCapability)),
           QStringLiteral("native.status.factory.identity"));

  // A null where a value is required is not an empty string either.
  QJsonObject nullRuntime = factoryObjectOf(snapshot);
  nullRuntime.insert(QStringLiteral("runtimeVersion"), QJsonValue(QJsonValue::Null));
  QCOMPARE(refusalOf(documentWithFactory(snapshot, nullRuntime)),
           QStringLiteral("native.status.factory.identity"));

  QJsonObject nullDevice = factoryObjectOf(snapshot);
  nullDevice.insert(QStringLiteral("deviceIdentity"), QJsonValue(QJsonValue::Null));
  QCOMPARE(refusalOf(documentWithFactory(snapshot, nullDevice)),
           QStringLiteral("native.status.factory.identity"));
}

// A supported role is the one place a capability flag can be turned on, and it
// can only come from an observed, successful, refusal-free answer on a backend
// this build checks.
void TestNativeStatus::aFactoryClaimsARoleOnlyWhenItsAnswerWasObservedAndRefusalFree() {
  StatusSnapshot snapshot = oneToOneSpatial();
  snapshot.factory = aUsableFactory();

  // A role without an executed query is a claim the document cannot support.
  QJsonObject unobserved = factoryObjectOf(snapshot);
  unobserved.insert(QStringLiteral("observed"), false);
  QCOMPARE(refusalOf(documentWithFactory(snapshot, unobserved)),
           QStringLiteral("native.status.factory.inconsistent"));

  // A failed query keeps its refusal; it cannot also keep a supported role.
  QJsonObject failed = factoryObjectOf(snapshot);
  failed.insert(QStringLiteral("reasonKey"), QStringLiteral("native.factory.queryFailed"));
  failed.insert(QStringLiteral("reason"), QStringLiteral("the producer query failed"));
  QCOMPARE(refusalOf(documentWithFactory(snapshot, failed)),
           QStringLiteral("native.status.factory.inconsistent"));

  // A backend this build does not check, and the honest "nothing checked" token,
  // can never back support.
  QJsonObject unknownBackend = factoryObjectOf(snapshot);
  unknownBackend.insert(QStringLiteral("backend"), QStringLiteral("vulkan"));
  QCOMPARE(refusalOf(documentWithFactory(snapshot, unknownBackend)),
           QStringLiteral("native.status.factory.backend"));

  QJsonObject noneBackend = factoryObjectOf(snapshot);
  noneBackend.insert(QStringLiteral("backend"), QStringLiteral("none"));
  QCOMPARE(refusalOf(documentWithFactory(snapshot, noneBackend)),
           QStringLiteral("native.status.factory.backend"));

  // An unavailable factory that names no reason is refused by name.
  QJsonObject reasonless = factoryObjectOf(snapshot);
  reasonless.insert(QStringLiteral("spatial"), false);
  QCOMPARE(refusalOf(documentWithFactory(snapshot, reasonless)),
           QStringLiteral("native.status.factory.reason"));

  // The rife role alone follows the same rule, and round trips when earned.
  StatusSnapshot rife = oneToOneSpatial();
  NativeFactoryObservation rifeFactory = aUsableFactory();
  rifeFactory.spatial = false;
  rifeFactory.rife = true;
  rife.factory = rifeFactory;
  QVERIFY(rife.factory->isUsable());
  StatusSnapshot rifeParsed;
  QString key;
  QString text;
  QVERIFY2(parseStatusSnapshot(statusSnapshotToJson(rife), &rifeParsed, &key, &text),
           qPrintable(text));
  QCOMPARE(rifeParsed, rife);
}

// A legitimate observed-but-unavailable answer states why in a stable key, keeps
// the runtime it really checked, and moves nothing else in the snapshot.
void TestNativeStatus::anUnavailableFactoryNamesItsReasonAndKeepsTheRestOfTheSnapshot() {
  StatusSnapshot snapshot = oneToOneSpatial();
  snapshot.factory = anObservedUnavailableFactory();

  StatusSnapshot parsed;
  QString key;
  QString text;
  QVERIFY2(parseStatusSnapshot(statusSnapshotToJson(snapshot), &parsed, &key, &text),
           qPrintable(text));
  QCOMPARE(parsed, snapshot);
  QVERIFY(parsed.factory.has_value());
  QVERIFY(!parsed.factory->isUsable());
  QVERIFY(!parsed.factory->spatial);
  QVERIFY(!parsed.factory->rife);
  QCOMPARE(parsed.factory->reasonKey, QStringLiteral("native.factory.engineMissing"));
  QCOMPARE(parsed.factory->reason, QStringLiteral("no compiled engine for this graph"));
  QCOMPARE(parsed.factory->runtimeVersion, QStringLiteral("TensorRT-10.4.0"));
  // An unavailable observation names no device it could have checked, and that
  // absence is preserved rather than filled with a placeholder.
  QCOMPARE(parsed.factory->deviceIdentity, QString());
  QCOMPARE(parsed.factory->smMajor, 0);
  QCOMPARE(parsed.factory->smMinor, 0);

  // The refusal concerns the factory alone: the filter's own state, plan,
  // counters and source identity are not rewritten by it.
  QCOMPARE(parsed.state, snapshot.state);
  QCOMPARE(parsed.epoch, snapshot.epoch);
  QCOMPARE(parsed.requested, snapshot.requested);
  QCOMPARE(parsed.effective, snapshot.effective);
  QCOMPARE(parsed.counters, snapshot.counters);

  // The honest "nothing was checked" backend is acceptable, but can only ever be
  // an unavailable observation.
  StatusSnapshot nothingChecked = oneToOneSpatial();
  NativeFactoryObservation noBackend = anObservedUnavailableFactory();
  noBackend.backend = QStringLiteral("none");
  nothingChecked.factory = noBackend;
  StatusSnapshot nothingParsed;
  QVERIFY2(parseStatusSnapshot(statusSnapshotToJson(nothingChecked), &nothingParsed, &key, &text),
           qPrintable(text));
  QCOMPARE(nothingParsed, nothingChecked);
  QVERIFY(!nothingParsed.factory->isUsable());

  // An unobserved observation with a named reason - there is no native source on
  // this core - is accepted and reads as unavailable. The reader caps the factory
  // at "missing" instead of rejecting the snapshot the filter did publish.
  StatusSnapshot noSource = oneToOneSpatial();
  NativeFactoryObservation unobservedFactory;
  unobservedFactory.observed = false;
  unobservedFactory.backend = QStringLiteral("none");
  unobservedFactory.reasonKey = QStringLiteral("native.factory.noSource");
  unobservedFactory.reason = QStringLiteral("no native source on this display");
  noSource.factory = unobservedFactory;
  StatusSnapshot noSourceParsed;
  QVERIFY2(parseStatusSnapshot(statusSnapshotToJson(noSource), &noSourceParsed, &key, &text),
           qPrintable(text));
  QCOMPARE(noSourceParsed, noSource);
  QVERIFY(!noSourceParsed.factory->isUsable());
  QCOMPARE(noSourceParsed.state, NativeState::Active);
}

// The status wire protocol, the producer's snapshot protocol/schema and the AJI
// source ABI are four separate identities. Each is stated and checked against its
// own value, so none can stand in for another, and every member has its real JSON
// type rather than a scalar a hostile document could reuse.
void TestNativeStatus::aFactoryStatesItsOwnIdentityAndTypesInsteadOfBeingConflated() {
  StatusSnapshot snapshot = oneToOneSpatial();
  snapshot.factory = aUsableFactory();

  const QJsonObject written = factoryObjectOf(snapshot);
  QCOMPARE(written.value(QStringLiteral("queryProtocol")).toInt(), kNativeFactoryQueryProtocol);
  QCOMPARE(written.value(QStringLiteral("nativeProtocol")).toInt(),
           kNativeFactorySnapshotProtocol);
  QCOMPARE(written.value(QStringLiteral("nativeSchema")).toInt(), kNativeFactorySnapshotSchema);
  QCOMPARE(written.value(QStringLiteral("sourceAbi")).toInt(), kNativeFactorySourceAbi);

  struct Conflation {
    const char* member;
    int value;
  };
  const Conflation conflated[] = {
      {"queryProtocol", kNativeFactorySnapshotProtocol},
      {"nativeProtocol", kNativeStatusProtocol},
      {"nativeSchema", kNativeStatusProtocol},
      {"sourceAbi", kNativeFactorySnapshotSchema},
  };
  for (const Conflation& field : conflated) {
    QJsonObject factory = factoryObjectOf(snapshot);
    factory.insert(QString::fromLatin1(field.member), field.value);
    QCOMPARE(refusalOf(documentWithFactory(snapshot, factory)),
             QStringLiteral("native.status.factory.protocol"));
  }
  QJsonObject textualVersion = factoryObjectOf(snapshot);
  textualVersion.insert(QStringLiteral("queryProtocol"), QStringLiteral("1"));
  QCOMPARE(refusalOf(documentWithFactory(snapshot, textualVersion)),
           QStringLiteral("native.status.factory.protocol"));

  // The role and observed flags are JSON booleans, never a 0/1 number or a string.
  QJsonObject numericObserved = factoryObjectOf(snapshot);
  numericObserved.insert(QStringLiteral("observed"), 1);
  QCOMPARE(refusalOf(documentWithFactory(snapshot, numericObserved)),
           QStringLiteral("native.status.factory.malformed"));
  QJsonObject stringSpatial = factoryObjectOf(snapshot);
  stringSpatial.insert(QStringLiteral("spatial"), QStringLiteral("true"));
  QCOMPARE(refusalOf(documentWithFactory(snapshot, stringSpatial)),
           QStringLiteral("native.status.factory.malformed"));

  // The capability is a whole integer, not a fractional or textual one, and a
  // required member can be neither absent nor null.
  QJsonObject fractionalSm = factoryObjectOf(snapshot);
  fractionalSm.insert(QStringLiteral("smMajor"), 8.5);
  QCOMPARE(refusalOf(documentWithFactory(snapshot, fractionalSm)),
           QStringLiteral("native.status.factory.malformed"));
  QJsonObject textualSm = factoryObjectOf(snapshot);
  textualSm.insert(QStringLiteral("smMajor"), QStringLiteral("8"));
  QCOMPARE(refusalOf(documentWithFactory(snapshot, textualSm)),
           QStringLiteral("native.status.factory.malformed"));
  QJsonObject noObserved = factoryObjectOf(snapshot);
  noObserved.remove(QStringLiteral("observed"));
  QCOMPARE(refusalOf(documentWithFactory(snapshot, noObserved)),
           QStringLiteral("native.status.factory.malformed"));
  QJsonObject nullBackend = factoryObjectOf(snapshot);
  nullBackend.insert(QStringLiteral("backend"), QJsonValue(QJsonValue::Null));
  QCOMPARE(refusalOf(documentWithFactory(snapshot, nullBackend)),
           QStringLiteral("native.status.factory.malformed"));
  QJsonObject nullReasonKey = factoryObjectOf(snapshot);
  nullReasonKey.insert(QStringLiteral("reasonKey"), QJsonValue(QJsonValue::Null));
  QCOMPARE(refusalOf(documentWithFactory(snapshot, nullReasonKey)),
           QStringLiteral("native.status.factory.malformed"));

  // An unknown member is a named refusal, never a zero-valued fact the filter
  // never stated.
  QJsonObject unknown = factoryObjectOf(snapshot);
  unknown.insert(QStringLiteral("ordinal"), 0);
  QCOMPARE(refusalOf(documentWithFactory(snapshot, unknown)),
           QStringLiteral("native.status.factory.unknown.member"));
}

// The factory is eligibility for a runtime, never an attestation that anything is
// prepared or active: it never moves the snapshot's own availability, the
// counters or the plan.
void TestNativeStatus::aFactoryObservationNeverMovesTheStateOrTheCounters() {
  // A usable factory under a Probe snapshot is still Probe-flavoured Unavailable.
  StatusSnapshot probe;
  probe.version = kNativeStatusProtocol;
  probe.revision = 9;
  probe.state = NativeState::Probe;
  probe.epoch = 3;
  probe.reason = QStringLiteral("native.probe.awaiting-frame: passthrough");
  probe.factory = aUsableFactory();

  StatusSnapshot parsed;
  QString key;
  QString text;
  QVERIFY2(parseStatusSnapshot(statusSnapshotToJson(probe), &parsed, &key, &text),
           qPrintable(text));
  QCOMPARE(parsed, probe);
  QVERIFY(parsed.factory->isUsable());
  const AvailabilityState probeAvailability = statusSnapshotAvailability(parsed);
  QCOMPARE(probeAvailability.value, Availability::Unavailable);
  QVERIFY(probeAvailability.reasonKey.contains(QStringLiteral("Probe")));
  QVERIFY(!probeAvailability.reasonKey.contains(QStringLiteral("factory")));

  // A Preparing snapshot with an unusable factory still reads Preparing, and the
  // plan and counters are exactly the filter's own report.
  StatusSnapshot preparing = oneToOneSpatial();
  preparing.state = NativeState::Preparing;
  preparing.reason = QStringLiteral("warming up the graph");
  preparing.factory = anObservedUnavailableFactory();
  StatusSnapshot preparingParsed;
  QVERIFY2(parseStatusSnapshot(statusSnapshotToJson(preparing), &preparingParsed, &key, &text),
           qPrintable(text));
  QCOMPARE(preparingParsed, preparing);
  QCOMPARE(statusSnapshotAvailability(preparingParsed).value, Availability::Preparing);
  QCOMPARE(preparingParsed.effective, preparing.effective);
  QCOMPARE(preparingParsed.counters, preparing.counters);

  // An Active snapshot stays Ready whether the factory is usable or not: the
  // factory never turns itself into the availability of the surface it rides on.
  StatusSnapshot active = oneToOneSpatial();
  active.factory = anObservedUnavailableFactory();
  StatusSnapshot activeParsed;
  QVERIFY2(parseStatusSnapshot(statusSnapshotToJson(active), &activeParsed, &key, &text),
           qPrintable(text));
  QCOMPARE(activeParsed, active);
  QCOMPARE(statusSnapshotAvailability(activeParsed).value, Availability::Ready);
  QVERIFY(statusSnapshotAvailability(activeParsed).reasonKey.isEmpty());
}

// A credential or signed URL in any factory field is refused by name without an
// echo, and the export boundary never puts it back in the clear.
void TestNativeStatus::aSensitiveFactoryValueIsRefusedRawAndWithheldFromTheExport() {
  const QString url = QStringLiteral("https://cdn.example.invalid/rtx.engine?token=Zm9vYmFy");
  StatusSnapshot snapshot = oneToOneSpatial();
  NativeFactoryObservation factory = aUsableFactory();
  factory.runtimeVersion = url;
  snapshot.factory = factory;

  StatusSnapshot parsed;
  QString key;
  QString text;
  QVERIFY(!parseStatusSnapshot(statusSnapshotToJson(snapshot), &parsed, &key, &text));
  QCOMPARE(key, QStringLiteral("native.status.sensitiveData"));
  QCOMPARE(parsed, StatusSnapshot{});
  QVERIFY(!text.contains(url));
  QVERIFY(!text.contains(QStringLiteral("Zm9vYmFy")));

  const QByteArray exported = serializeStatusSnapshot(snapshot);
  QVERIFY(!exported.contains(url.toUtf8()));
  QVERIFY(!exported.contains(QStringLiteral("Zm9vYmFy").toUtf8()));

  // A credential in the factory's free-text reason is refused the same way.
  StatusSnapshot reasonSnapshot = oneToOneSpatial();
  NativeFactoryObservation withReason = aUsableFactory();
  withReason.reason = QStringLiteral("Authorization: Bearer sk-live-abcdef");
  reasonSnapshot.factory = withReason;
  QVERIFY(!parseStatusSnapshot(statusSnapshotToJson(reasonSnapshot), &parsed, &key, &text));
  QCOMPARE(key, QStringLiteral("native.status.sensitiveData"));
  QVERIFY(!text.contains(QStringLiteral("sk-live-abcdef")));
  QVERIFY(!serializeStatusSnapshot(reasonSnapshot).contains(QByteArray("sk-live-abcdef")));
}

// A later document that publishes no factory clears the previous observation
// rather than letting a stale one describe a new source generation.
void TestNativeStatus::aFactoryAbsentFromTheNewDocumentIsNotCarriedOverFromTheOldOne() {
  StatusSnapshot withFactory = oneToOneSpatial();
  withFactory.factory = aUsableFactory();

  StatusSnapshot parsed;
  QString key;
  QString text;
  QVERIFY2(parseStatusSnapshot(statusSnapshotToJson(withFactory), &parsed, &key, &text),
           qPrintable(text));
  QVERIFY(parsed.factory.has_value());
  QVERIFY(parsed.factory->isUsable());

  // A new epoch that publishes no factory: the earlier observation belonged to
  // another source generation and must not read as current.
  StatusSnapshot withoutFactory = oneToOneSpatial();
  withoutFactory.revision = 413;
  withoutFactory.epoch = 8;
  QVERIFY2(parseStatusSnapshot(statusSnapshotToJson(withoutFactory), &parsed, &key, &text),
           qPrintable(text));
  QCOMPARE(parsed.factory.has_value(), false);
  QCOMPARE(parsed, withoutFactory);

  // The tag layer goes through the same parse and clears it too.
  QVERIFY2(parseStatusTags(statusTags(withFactory), &parsed, &key, &text), qPrintable(text));
  QVERIFY(parsed.factory.has_value());
  QVERIFY2(parseStatusTags(statusTags(withoutFactory), &parsed, &key, &text), qPrintable(text));
  QCOMPARE(parsed.factory.has_value(), false);

  // A refusal clears the output rather than leaving the previous factory behind.
  StatusSnapshot refused;
  QVERIFY(!parseStatusSnapshot(QJsonObject{}, &refused, &key, &text));
  QCOMPARE(refused.factory.has_value(), false);
  QCOMPARE(refused, StatusSnapshot{});
}

// The factory names the device it was checked against; the probe names the source
// device. The reader must never coerce one into the other, so a consumer can see a
// factory that does not belong to this source and refuse to derive availability.
void TestNativeStatus::aFactoryDeviceThatDoesNotMatchTheProbedSourceIsNeverRewrittenIntoOne() {
  const QString deviceHex = QStringLiteral("9f2c1a440b7d4e219c3a1d5e6f708192");
  const QString otherHex = QStringLiteral("0a1b2c3d4e5f60718293a4b5c6d7e8f9");

  // The probe saw one decoder device; a factory checked against the same device is
  // accepted even when the two spell its UUID differently (the dashed form is the
  // same device, never a different one).
  StatusSnapshot matching = oneToOneSpatial();
  NativeProbePayload probe;
  DeviceIdentity device;
  device.decoderDeviceUuid = QStringLiteral("9f2c1a44-0b7d-4e21-9c3a-1d5e6f708192");
  probe.device = device;
  matching.probe = probe;
  NativeFactoryObservation factory = aUsableFactory();
  factory.deviceIdentity = deviceHex;
  matching.factory = factory;

  StatusSnapshot parsed;
  QString key;
  QString text;
  QVERIFY2(parseStatusSnapshot(statusSnapshotToJson(matching), &parsed, &key, &text),
           qPrintable(text));
  QCOMPARE(parsed, matching);

  // A factory checked against another device is refused by name: it is never
  // rewritten into this source's device, so a consumer cannot read it as
  // availability for the wrong core.
  StatusSnapshot mismatched = matching;
  mismatched.factory->deviceIdentity = otherHex;
  QVERIFY(!parseStatusSnapshot(statusSnapshotToJson(mismatched), &parsed, &key, &text));
  QCOMPARE(key, QStringLiteral("native.status.factory.identity"));
  QCOMPARE(parsed, StatusSnapshot{});
}
