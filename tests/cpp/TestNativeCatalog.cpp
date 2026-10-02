// M11's consumer-visible behaviour: the model store's identity and integrity
// rules, and actual inspection through the ONNX Runtime C API.
//
// The store tests use arbitrary byte blobs on purpose: what the store promises is
// about bytes, digests and manifests, not about parsing. Every *inspection* test
// uses a real ONNX graph, either generated deterministically in-process by the
// encoder below (so it needs no fixture binary and no Python) or the acquired
// original dynamic graph when the machine has the assets.
//
// The runtime-dependent tests skip when MIKAI_ORT_RUNTIME_DIR is unset or names
// no module. They never substitute a fake runtime: absence is a skip, not a pass.

#include "TestNativeCatalog.hpp"

#include "native/NativeInspectedPlan.hpp"
#include "native/NativeModelInventory.hpp"
#include "native/NativePaths.hpp"
#include "native/NativePreparationPlan.hpp"

#include <optional>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>

using namespace mplayer::native;

namespace {

// ----------------------------------------------------- ONNX fixture encoding
//
// A minimal protobuf writer. Only the fields the fixtures need are emitted, and
// only the wire types they use: varint (0), length-delimited (2). An unknown
// dimension is an empty TensorShapeProto.Dimension submessage, which is exactly
// how ONNX spells "dynamic" - not a zero and not a symbol.

constexpr int kOnnxFloat16 = 10;

void putVarint(QByteArray* out, quint64 value) {
  while (value >= 0x80) {
    out->append(static_cast<char>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  out->append(static_cast<char>(value));
}

void putKey(QByteArray* out, int field, int wire) {
  putVarint(out, (static_cast<quint64>(field) << 3) | static_cast<quint64>(wire));
}

void putVarintField(QByteArray* out, int field, quint64 value) {
  putKey(out, field, 0);
  putVarint(out, value);
}

void putBytesField(QByteArray* out, int field, const QByteArray& value) {
  putKey(out, field, 2);
  putVarint(out, static_cast<quint64>(value.size()));
  out->append(value);
}

void putStringField(QByteArray* out, int field, const QString& value) {
  putBytesField(out, field, value.toUtf8());
}

QByteArray dimension(qint64 value) {
  QByteArray dim;
  putVarintField(&dim, 1, static_cast<quint64>(value));
  return dim;
}

QByteArray dynamicDimension() {
  return {};
}

QByteArray valueInfo(const QString& name, int elementType,
                     const QVector<std::optional<qint64>>& dims) {
  QByteArray shape;
  for (const std::optional<qint64>& dim : dims) {
    putBytesField(&shape, 1, dim.has_value() ? dimension(*dim) : dynamicDimension());
  }
  QByteArray tensor;
  putVarintField(&tensor, 1, static_cast<quint64>(elementType));
  putBytesField(&tensor, 2, shape);
  QByteArray type;
  putBytesField(&type, 1, tensor);

  QByteArray value;
  putStringField(&value, 1, name);
  putBytesField(&value, 2, type);
  return value;
}

/// One int-valued ONNX attribute, as `NodeProto.attribute` (field 5) holds it:
/// `AttributeProto.name` = 1, `AttributeProto.i` = 3 and `AttributeProto.type` =
/// 20, carrying `AttributeProto.AttributeType.INT` = 2. Concat's `axis` is a
/// required attribute, so a node that omits it is a graph ONNX Runtime refuses
/// to load rather than a boundary the bridge gets to check.
QByteArray intAttribute(const QString& name, qint64 value) {
  QByteArray attribute;
  putStringField(&attribute, 1, name);
  putVarintField(&attribute, 3, static_cast<quint64>(value));
  putVarintField(&attribute, 20, 2);
  return attribute;
}

QByteArray node(const QString& opType, const QStringList& inputs, const QStringList& outputs,
                const QString& name, const QVector<QByteArray>& attributes = {}) {
  QByteArray value;
  for (const QString& input : inputs) putStringField(&value, 1, input);
  for (const QString& output : outputs) putStringField(&value, 2, output);
  putStringField(&value, 3, name);
  putStringField(&value, 4, opType);
  for (const QByteArray& attribute : attributes) putBytesField(&value, 5, attribute);
  return value;
}

/// Assembles a ModelProto around one graph. `opset` is the ai.onnx version the
/// nodes are written against; `irVersion` 8 is what the acquired models use.
QByteArray onnxModel(const QVector<QByteArray>& nodes, const QVector<QByteArray>& inputs,
                     const QVector<QByteArray>& outputs, int opset = 17,
                     const QVector<QByteArray>& initializers = {}) {
  QByteArray graph;
  for (const QByteArray& n : nodes) putBytesField(&graph, 1, n);
  putStringField(&graph, 2, QStringLiteral("mikai.fixture"));
  for (const QByteArray& initializer : initializers) putBytesField(&graph, 5, initializer);
  for (const QByteArray& input : inputs) putBytesField(&graph, 11, input);
  for (const QByteArray& output : outputs) putBytesField(&graph, 12, output);

  QByteArray opsetImport;
  putVarintField(&opsetImport, 2, static_cast<quint64>(opset));

  QByteArray model;
  putVarintField(&model, 1, 8);
  putStringField(&model, 2, QStringLiteral("mikai.tests"));
  putBytesField(&model, 7, graph);
  putBytesField(&model, 8, opsetImport);
  return model;
}

/// One fp16 NCHW input `X`, one fp16 NCHW output `Y`, connected by an Identity
/// node. Dynamic H/W on both sides when `height`/`width` are nullopt.
QByteArray identityGraph(int elementType, const std::optional<qint64>& height,
                         const std::optional<qint64>& width) {
  const QVector<std::optional<qint64>> dims = {1, 3, height, width};
  return onnxModel({node(QStringLiteral("Identity"), {QStringLiteral("X")},
                         {QStringLiteral("Y")}, QStringLiteral("identity"))},
                   {valueInfo(QStringLiteral("X"), elementType, dims)},
                   {valueInfo(QStringLiteral("Y"), elementType, dims)});
}

/// The same identity graph with different I/O names, so two fixtures are two
/// distinct byte sequences that declare the very same boundary. Used to prove
/// that two files sharing a basename are told apart by identity, not by name.
QByteArray identityGraphNamed(int elementType, const std::optional<qint64>& height,
                              const std::optional<qint64>& width, const QString& inputName,
                              const QString& outputName) {
  const QVector<std::optional<qint64>> dims = {1, 3, height, width};
  return onnxModel({node(QStringLiteral("Identity"), {inputName}, {outputName},
                         QStringLiteral("identity"))},
                   {valueInfo(inputName, elementType, dims)},
                   {valueInfo(outputName, elementType, dims)});
}

/// Two fp16 inputs and one output: the "extra input" case the bridge must refuse.
QByteArray twoInputGraph() {
  const QVector<std::optional<qint64>> dims = {1, 3, std::nullopt, std::nullopt};
  return onnxModel({node(QStringLiteral("Add"), {QStringLiteral("A"), QStringLiteral("B")},
                         {QStringLiteral("Y")}, QStringLiteral("add"))},
                   {valueInfo(QStringLiteral("A"), kOnnxFloat16, dims),
                    valueInfo(QStringLiteral("B"), kOnnxFloat16, dims)},
                   {valueInfo(QStringLiteral("Y"), kOnnxFloat16, dims)});
}

/// A TensorProto initializer: an int64 tensor of shape `dims` holding `values`,
/// written as the little-endian raw bytes ORT reads.
QByteArray tensorInt64(const QString& name, const QVector<qint64>& dims,
                       const QVector<qint64>& values) {
  QByteArray raw;
  for (const qint64 value : values) {
    for (int byte = 0; byte < 8; ++byte) {
      raw.append(static_cast<char>((static_cast<quint64>(value) >> (8 * byte)) & 0xff));
    }
  }
  QByteArray tensor;
  for (const qint64 dim : dims) putVarintField(&tensor, 1, static_cast<quint64>(dim));
  putVarintField(&tensor, 2, 7);  // TensorProto.DataType.INT64
  putStringField(&tensor, 8, name);
  putBytesField(&tensor, 9, raw);
  return tensor;
}

/// One fp16 NCHW input `X` whose output is declared with a symbolic batch - the
/// exporter's way of writing "batch to be learned" - while the node itself
/// returns the input's own 1x3xHxW. Only a real run settles what that batch is.
QByteArray symbolicOutputBatchGraph() {
  const QVector<std::optional<qint64>> inDims = {1, 3, std::nullopt, std::nullopt};
  const QVector<std::optional<qint64>> outDims = {std::nullopt, 3, std::nullopt, std::nullopt};
  return onnxModel({node(QStringLiteral("Identity"), {QStringLiteral("X")}, {QStringLiteral("Y")},
                         QStringLiteral("identity"))},
                   {valueInfo(QStringLiteral("X"), kOnnxFloat16, inDims)},
                   {valueInfo(QStringLiteral("Y"), kOnnxFloat16, outDims)});
}

/// One fp16 NCHW input `X`; the graph actually returns two batches, by
/// concatenating the input with itself along N. The declared output keeps a
/// symbolic batch, so the declaration is a candidate and the run is what reveals
/// the batch this bridge cannot bind. Concat's `axis` is required, and axis 0 is
/// the batch axis, so the node doubles the batch rather than the channels.
QByteArray doubledBatchGraph() {
  const QVector<std::optional<qint64>> inDims = {1, 3, std::nullopt, std::nullopt};
  const QVector<std::optional<qint64>> outDims = {std::nullopt, 3, std::nullopt, std::nullopt};
  return onnxModel({node(QStringLiteral("Concat"), {QStringLiteral("X"), QStringLiteral("X")},
                         {QStringLiteral("Y")}, QStringLiteral("concat"),
                         {intAttribute(QStringLiteral("axis"), 0)})},
                   {valueInfo(QStringLiteral("X"), kOnnxFloat16, inDims)},
                   {valueInfo(QStringLiteral("Y"), kOnnxFloat16, outDims)});
}

/// One fp16 NCHW input `X` whose graph actually returns four channels, by padding
/// one channel on. Its declared output says four as well, so the declaration is
/// refused before a run rather than executed to learn an unbindable channel count.
QByteArray fourChannelGraph() {
  const QVector<std::optional<qint64>> inDims = {1, 3, std::nullopt, std::nullopt};
  const QVector<std::optional<qint64>> outDims = {std::nullopt, 4, std::nullopt, std::nullopt};
  const QByteArray pads = tensorInt64(QStringLiteral("pads"), {8}, {0, 0, 0, 0, 0, 1, 0, 0});
  return onnxModel({node(QStringLiteral("Pad"), {QStringLiteral("X"), QStringLiteral("pads")},
                         {QStringLiteral("Y")}, QStringLiteral("pad"))},
                   {valueInfo(QStringLiteral("X"), kOnnxFloat16, inDims)},
                   {valueInfo(QStringLiteral("Y"), kOnnxFloat16, outDims)}, 17, {pads});
}

/// One fp16 NCHW input `X` whose graph actually returns a rank-2 output, by
/// flattening. Its declared output is rank 2 as well.
QByteArray flattenedGraph() {
  const QVector<std::optional<qint64>> inDims = {1, 3, std::nullopt, std::nullopt};
  const QVector<std::optional<qint64>> outDims = {std::nullopt, std::nullopt};
  return onnxModel({node(QStringLiteral("Flatten"), {QStringLiteral("X")}, {QStringLiteral("Y")},
                         QStringLiteral("flatten"))},
                   {valueInfo(QStringLiteral("X"), kOnnxFloat16, inDims)},
                   {valueInfo(QStringLiteral("Y"), kOnnxFloat16, outDims)});
}

// ------------------------------------------------------------------ fixtures

class StoreFixture {
 public:
  StoreFixture() {
    QString reason;
    const QString modelsRoot = nativeSubdirectory(QStringLiteral("models"), &reason);
    if (modelsRoot.isEmpty() || !QDir().mkpath(modelsRoot)) return;
    root_ = QDir(modelsRoot).filePath(
        QStringLiteral("unit-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  }
  ~StoreFixture() {
    if (!root_.isEmpty()) QDir(root_).removeRecursively();
  }
  bool isValid() const { return !root_.isEmpty(); }
  QString root() const { return root_; }

 private:
  QString root_;
};

TensorProfile tensor(const QString& dtype, std::optional<qint64> height,
                     std::optional<qint64> width) {
  TensorProfile profile;
  profile.dtype = dtype;
  profile.rank = 4;
  profile.batch = 1;
  profile.channels = 3;
  profile.height = height;
  profile.width = width;
  return profile;
}

ModelProfile fixedProfile(const QString& dtype, qint64 inH, qint64 inW, qint64 outH, qint64 outW) {
  ModelProfile profile;
  profile.input = tensor(dtype, inH, inW);
  profile.output = tensor(dtype, outH, outW);
  return profile;
}

ModelProfile dynamicProfile(const QString& dtype) {
  ModelProfile profile;
  profile.input = tensor(dtype, std::nullopt, std::nullopt);
  profile.output = tensor(dtype, std::nullopt, std::nullopt);
  return profile;
}

/// A successful declared read. `referenceRan` mirrors what inspectGraph would
/// have filled: false for a dtype that is never executed.
InspectionResult inspected(const ModelProfile& declared) {
  InspectionResult result;
  result.ok = true;
  result.inputName = QStringLiteral("X");
  result.outputName = QStringLiteral("Y");
  result.declared = declared;
  return result;
}

bool writeFile(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  return file.write(bytes) == bytes.size();
}

InspectionRuntime ortRuntime() {
  return inspectionRuntimeAt(qEnvironmentVariable("MIKAI_ORT_RUNTIME_DIR"));
}

QString assetRoot() {
  const QString root = qEnvironmentVariable("MIKAI_NATIVE_ASSETS");
  return root.isEmpty() ? QString() : root;
}

QString realDynamicModelPath() {
  const QString root = assetRoot();
  if (root.isEmpty()) return QString();
  return QDir(root).filePath(QStringLiteral(
      "spatial/onnx/2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo.onnx"));
}

/// One of the acquired original HD graphs. Its declared output carries a symbolic
/// batch the exporter wrote, which only a real CPU run can settle.
QString realHdModelPath() {
  const QString root = assetRoot();
  if (root.isEmpty()) return QString();
  return QDir(root).filePath(QStringLiteral(
      "spatial/onnx/2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx"));
}

QByteArray readAll(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return QByteArray();
  return file.readAll();
}

#ifdef Q_OS_WIN
/// Creates a directory junction, which needs no elevation. False when the
/// environment will not make one, so the reparse test skips instead of asserting
/// something it could not set up.
bool makeJunction(const QString& link, const QString& target) {
  QProcess process;
  process.start(QStringLiteral("cmd.exe"),
                {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"), link,
                 target});
  process.waitForFinished(20000);
  return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 &&
         QFileInfo(link).isDir();
}

/// Removes a junction without touching what it points at.
void removeJunction(const QString& link) {
  QProcess::execute(QStringLiteral("cmd.exe"),
                    {QStringLiteral("/c"), QStringLiteral("rd"), QDir::toNativeSeparators(link)});
}
#endif

QString storeTokenOf(const QString& modelId) {
  if (!modelId.startsWith(QLatin1String("user:"))) return QString();
  return modelId.mid(static_cast<int>(sizeof("user:") - 1));
}

/// Publishes a generated graph from a real source file and returns the row. The
/// CPU reference has to have actually run for a dynamic graph: a row whose run
/// did not happen is recorded Unavailable, and the adapter would then refuse it
/// for that reason rather than for the one a test is after.
bool publishGenerated(ModelInventory* inventory, const QString& sourcePath, const QByteArray& bytes,
                      const QString& displayName, StoredModel* stored) {
  const InspectionResult inspection = inspectGraph(ortRuntime(), bytes, QSize(320, 180));
  if (!inspection.ok) return false;
  BackendAvailability backends;
  backends.available = {Backend::TensorRT};
  const ModelImportOutcome outcome =
      inventory->publish(bytes, inspection, sourcePath, displayName, backends);
  if (!outcome.ok) return false;
  *stored = outcome.model;
  return true;
}

/// A dynamic fixture only resolves when the CPU provider actually produced a
/// shape at the requested geometry. Where it refused the fp16 kernel there is
/// nothing to resolve, and every adapter test says so instead of inventing one.
bool dynamicReferenceRan(const QByteArray& bytes) {
  return inspectGraph(ortRuntime(), bytes, QSize(320, 180)).referenceRan;
}

}  // namespace

// ---------------------------------------------------------------------- tests

void TestNativeCatalog::anAbsentRuntimeIsNamedNotSearched() {
  QString reason;
  QVERIFY(!inspectionRuntimeIsPresent(InspectionRuntime{}, &reason));
  QVERIFY(reason.contains(QStringLiteral("no ONNX Runtime directory")));

  // A named directory that holds no module is a refusal, not a fallback to some
  // other copy on the machine.
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  const InspectionRuntime empty = inspectionRuntimeAt(temp.path());
  QVERIFY(empty.isSet());
  QVERIFY(!inspectionRuntimeIsPresent(empty, &reason));
  QVERIFY(reason.contains(QStringLiteral("onnxruntime.dll")));

  // With no runtime configured at all, reading a graph answers with the named
  // unavailability rather than a fabricated profile. Only asserted when no
  // runtime is configured: once one is loaded, this process reuses it.
  QString presentReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &presentReason)) {
    const InspectionResult result =
        inspectDeclaredProfile(InspectionRuntime{}, identityGraph(kOnnxFloat16, std::nullopt, std::nullopt));
    QVERIFY(!result.ok);
    QCOMPARE(result.reasonKey, QStringLiteral("inspect.runtime.unavailable"));
    QVERIFY(result.declared.input.dtype.isEmpty());
  }
}

void TestNativeCatalog::aPublishedModelIsStoredUnderItsContentDigest() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  const ModelProfile profile = fixedProfile(QStringLiteral("fp16"), 64, 64, 128, 128);
  const QByteArray bytes = QByteArrayLiteral("mikai-fixture-graph-a");
  const ModelImportOutcome outcome =
      inventory.publish(bytes, inspected(profile), QStringLiteral("C:/in/a.onnx"),
                        QStringLiteral("a.onnx"));
  QVERIFY2(outcome.ok, qPrintable(outcome.reason));
  QVERIFY(outcome.published);
  QCOMPARE(outcome.model.record.id,
           userModelIdFor(computeContentId(ModelRole::Spatial, sha256HexOf(bytes), profile)));
  const QString expectedStored =
      QFileInfo(QDir(fixture.root()).filePath(storeTokenOf(outcome.model.record.id) +
                                              QLatin1String("/model.onnx")))
          .canonicalFilePath();
  QCOMPARE(outcome.model.record.onnxSha256, sha256HexOf(bytes));
  QCOMPARE(outcome.model.record.onnxBytes, static_cast<qint64>(bytes.size()));
  QCOMPARE(outcome.model.record.origin, ModelOrigin::UserImported);
  QCOMPARE(outcome.model.record.availability.value, Availability::Ready);
  QVERIFY(outcome.model.record.isOffered());

  // The copy on disk is exactly the inspected bytes, in a directory named by the
  // content token, and the row reads back from it.
  QCOMPARE(outcome.model.storedPath, expectedStored);
  QCOMPARE(readAll(outcome.model.storedPath), bytes);

  QString listReason;
  const QVector<StoredModel> rows = inventory.stored(&listReason);
  QVERIFY2(listReason.isEmpty(), qPrintable(listReason));
  QCOMPARE(rows.size(), 1);
  QCOMPARE(rows.at(0).record.id, outcome.model.record.id);
  QCOMPARE(rows.at(0).record.availability.value, Availability::Ready);
  QCOMPARE(rows.at(0).sourcePath, QStringLiteral("C:/in/a.onnx"));
}

// The default store is the app-owned `native/models` tree, and it is owned before
// its first write: a fresh install that has never imported anything must still be
// able to import. A root outside that tree - including a real, writable directory
// - is refused, so the first-use case does not widen what counts as the store.
void TestNativeCatalog::aFreshOwnedStoreRootIsEligibleBeforeItExists() {
  const QString defaultRoot = ModelInventory::defaultRoot();
  QVERIFY(!defaultRoot.isEmpty());
  QString reason;
  QVERIFY2(ModelInventory(defaultRoot).isOwned(&reason), qPrintable(reason));
  QVERIFY(reason.isEmpty());

  // A root that does not exist yet, below the owned tree, is the same fact: the
  // part of it that exists is on the app-owned path and nothing on the way is a
  // reparse point.
  const QString freshRoot =
      QDir(defaultRoot).filePath(QStringLiteral("unit-%1/nested")
                                     .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  QVERIFY(!QFileInfo(freshRoot).exists());
  // The deepest ancestor that exists is on the app-owned path, which is what
  // makes the missing tail creatable as the owned store.
  QVERIFY(!nearestExistingAncestor(freshRoot).isEmpty());
  QVERIFY(pathWithinRoot(nearestExistingAncestor(freshRoot), defaultRoot));
  ModelInventory inventory(freshRoot);
  QVERIFY2(inventory.isOwned(&reason), qPrintable(reason));
  QVERIFY(reason.isEmpty());

  // The first import is what creates it, and it lands a row in the owned tree.
  QVERIFY2(inventory.ensureOwned(&reason), qPrintable(reason));
  QVERIFY(QFileInfo(freshRoot).isDir());
  QVERIFY(inventory.isOwned(&reason));

  const ModelProfile profile = fixedProfile(QStringLiteral("fp16"), 64, 64, 128, 128);
  const QByteArray bytes = QByteArrayLiteral("fresh-owned-root-graph");
  const ModelImportOutcome outcome =
      inventory.publish(bytes, inspected(profile), QStringLiteral("C:/fresh/model.onnx"),
                        QStringLiteral("model.onnx"));
  QVERIFY2(outcome.ok, qPrintable(outcome.reason));
  QVERIFY(outcome.published);
  QCOMPARE(inventory.stored().size(), 1);

  // A writable directory that is not the app-owned tree is not a store, and a
  // refused publish leaves it exactly as it was.
  QTemporaryDir foreign;
  QVERIFY(foreign.isValid());
  ModelInventory outside(foreign.path());
  QVERIFY(!outside.isOwned(&reason));
  QVERIFY(!reason.isEmpty());
  const ModelImportOutcome refused =
      outside.publish(bytes, inspected(profile), QStringLiteral("C:/fresh/model.onnx"),
                      QStringLiteral("model.onnx"));
  QVERIFY(!refused.ok);
  QVERIFY(!refused.published);
  QCOMPARE(refused.reasonKey, QStringLiteral("model.store.unowned"));
  QVERIFY(QDir(foreign.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

  QVERIFY(QDir(QFileInfo(freshRoot).absolutePath()).removeRecursively());
}

// A root has to be the app-owned tree, not merely to spell it: a path that walks
// back out of it, and any reparse point on the way to it, are refused before
// anything is created.
void TestNativeCatalog::aTraversalOrReparsePointedRootIsNotAStore() {
  const QString defaultRoot = ModelInventory::defaultRoot();
  QVERIFY(!defaultRoot.isEmpty());
  QString reason;

  // The root's own name walks back out of the owned tree, so it is not one.
  ModelInventory traversal(defaultRoot + QStringLiteral("/.."));
  QVERIFY(!traversal.isOwned(&reason));
  QVERIFY(!reason.isEmpty());
  QVERIFY(!traversal.ensureOwned(&reason));

#ifdef Q_OS_WIN
  const QString unit =
      QDir(defaultRoot).filePath(QStringLiteral("unit-%1")
                                     .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  QVERIFY(QDir().mkpath(unit));
  const QString real = QDir(unit).filePath(QStringLiteral("real"));
  QVERIFY(QDir().mkpath(real));
  const QString link = QDir(unit).filePath(QStringLiteral("link"));
  if (!makeJunction(link, real)) {
    QVERIFY(QDir(unit).removeRecursively());
    QSKIP("this environment will not create a junction");
  }

  // The rule itself: the junction is a reparse point, the plain directory is not.
  QVERIFY(!pathHasNoReparsePoints(defaultRoot, link));
  QVERIFY(pathHasNoReparsePoints(defaultRoot, real));

  // A root that does not exist yet, reached through a junction, is refused even
  // though every component of it is lexically inside the owned tree: its missing
  // tail would be created on the far side of the reparse point.
  ModelInventory throughLink(QDir(link).filePath(QStringLiteral("nested")));
  QVERIFY(!throughLink.isOwned(&reason));
  QVERIFY(!reason.isEmpty());
  QVERIFY(!throughLink.ensureOwned(&reason));
  QVERIFY(!QFileInfo(QDir(link).filePath(QStringLiteral("nested"))).exists());

  // The same missing tail beside the junction is the store, so what refuses the
  // one above is the reparse point and not the nesting.
  ModelInventory beside(QDir(real).filePath(QStringLiteral("nested")));
  QVERIFY2(beside.isOwned(&reason), qPrintable(reason));

  removeJunction(link);
  QVERIFY(
      !QDir(unit).entryList(QDir::Dirs | QDir::NoDotAndDotDot).contains(QStringLiteral("link")));
  QVERIFY(QDir(unit).removeRecursively());
#else
  QSKIP("a directory junction is a Windows fact");
#endif
}

// What the store's own read says for the catalogue surface: a fresh owned store
// that has never been written is empty and says nothing about its absent root,
// and a successful import must make the reload the store as it now is rather
// than leave the pre-import missing-root diagnostic standing. A root that is not
// an owned store is still refused, with its reason and no rows.
void TestNativeCatalog::
    aFreshOwnedStoreReadsEmptyAndReloadsWithoutAMissingRootDiagnostic() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  const QString root = fixture.root();
  QVERIFY(!QFileInfo(root).exists());

  ModelInventory inventory(root);
  QString reason;
  QVERIFY2(inventory.isOwned(&reason), qPrintable(reason));
  // First use: the owned root is not on disk yet, and that is an empty
  // inventory - not a missing one, and not a diagnostic about one.
  QVERIFY(inventory.stored(&reason).isEmpty());
  QVERIFY2(reason.isEmpty(), qPrintable(reason));
  // Reading created nothing.
  QVERIFY(!QFileInfo(root).exists());

  // A successful import, through the store's own publish path.
  const ModelProfile profile = fixedProfile(QStringLiteral("fp16"), 64, 64, 128, 128);
  const QByteArray bytes = QByteArrayLiteral("fresh-owned-catalogue-graph");
  const ModelImportOutcome outcome =
      inventory.publish(bytes, inspected(profile), QStringLiteral("C:/fresh/model.onnx"),
                        QStringLiteral("model.onnx"));
  QVERIFY2(outcome.ok, qPrintable(outcome.reason));
  QVERIFY(outcome.published);

  // The reload is the store as it now is: the row is there, and there is no
  // diagnostic - least of all the one first use produced.
  const QVector<StoredModel> rows = inventory.stored(&reason);
  QCOMPARE(rows.size(), 1);
  QCOMPARE(rows.first().record.id, outcome.model.record.id);
  QVERIFY2(reason.isEmpty(), qPrintable(reason));

  // A root that is not an owned store is still refused with no rows, and the
  // ownership reason is what says why: an empty first use does not widen what
  // counts as the store.
  QTemporaryDir foreign;
  QVERIFY(foreign.isValid());
  ModelInventory outside(foreign.path());
  QVERIFY(!outside.isOwned(&reason));
  QVERIFY(outside.stored(&reason).isEmpty());
  QVERIFY(!reason.isEmpty());

  // An existing non-directory named inside the owned tree is not the store
  // either: it is refused, not read as an empty one.
  const QString modelsRoot = nativeSubdirectory(QStringLiteral("models"));
  QVERIFY(QDir().mkpath(modelsRoot));
  const QString fileRoot =
      QDir(modelsRoot).filePath(QStringLiteral("unit-%1")
                                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  QVERIFY(writeFile(fileRoot, QByteArrayLiteral("not a directory")));
  ModelInventory notADirectory(fileRoot);
  QVERIFY(!notADirectory.isOwned(&reason));
  QVERIFY(notADirectory.stored(&reason).isEmpty());
  QVERIFY(!reason.isEmpty());
  QVERIFY(QFile::remove(fileRoot));

  // And a root that spells its way back out of the tree is refused, not empty.
  ModelInventory traversal(modelsRoot + QStringLiteral("/.."));
  QVERIFY(!traversal.isOwned(&reason));
  QVERIFY(traversal.stored(&reason).isEmpty());
  QVERIFY(!reason.isEmpty());
}

// The row's backend support is the one claim the provisioner made about the
// model, kept apart from what a runtime happens to carry: a vouched set is
// recorded exactly, a supply of nothing records nothing, and both are read back
// from the row's own document rather than re-derived.
void TestNativeCatalog::aRowsRecordedBackendSupportIsExactlyWhatWasVouched() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());
  const ModelProfile profile = fixedProfile(QStringLiteral("fp16"), 64, 64, 128, 128);

  BackendAvailability vouched;
  vouched.available = {Backend::DirectML};
  const ModelImportOutcome narrowed =
      inventory.publish(QByteArrayLiteral("graph-vouched"), inspected(profile),
                        QStringLiteral("C:/v.onnx"), QStringLiteral("v.onnx"), vouched);
  QVERIFY2(narrowed.ok, qPrintable(narrowed.reason));
  QCOMPARE(narrowed.model.record.availableBackends.size(), 1);
  QCOMPARE(narrowed.model.record.availableBackends.at(0), Backend::DirectML);
  QVERIFY(narrowed.model.record.supportsBackend(Backend::DirectML));
  QVERIFY(!narrowed.model.record.supportsBackend(Backend::TensorRT));

  // No vouch is not a licence to claim this build's vocabulary: the row claims no
  // backend at all, and the runtime that opens the store stays a separate fact.
  const ModelImportOutcome unvouched =
      inventory.publish(QByteArrayLiteral("graph-unvouched"), inspected(profile),
                        QStringLiteral("C:/n.onnx"), QStringLiteral("n.onnx"));
  QVERIFY2(unvouched.ok, qPrintable(unvouched.reason));
  QVERIFY(unvouched.model.record.availableBackends.isEmpty());
  QVERIFY(!unvouched.model.record.supportsBackend(Backend::TensorRT));

  // Both claims are the row's own document's, rehydrated on the next read.
  const QVector<StoredModel> rows = inventory.stored();
  QCOMPARE(rows.size(), 2);
  for (const StoredModel& row : rows) {
    if (row.record.id == narrowed.model.record.id) {
      QCOMPARE(row.record.availableBackends.size(), 1);
      QCOMPARE(row.record.availableBackends.at(0), Backend::DirectML);
      QVERIFY(row.record.supportsBackend(Backend::DirectML));
    } else {
      QVERIFY(row.record.availableBackends.isEmpty());
      QVERIFY(!row.record.supportsBackend(Backend::DirectML));
    }
  }

  // A document that does not record where its row can run is refused, never read
  // as a row that supports nothing.
  const QString manifestPath = QDir(fixture.root())
                                   .filePath(storeTokenOf(narrowed.model.record.id) +
                                             QLatin1String("/record.json"));
  QJsonObject manifest = QJsonDocument::fromJson(readAll(manifestPath)).object();
  QVERIFY(manifest.contains(QStringLiteral("backends")));
  manifest.remove(QStringLiteral("backends"));
  QVERIFY(writeFile(manifestPath, QJsonDocument(manifest).toJson(QJsonDocument::Compact)));

  const QVector<StoredModel> afterEdit = inventory.stored();
  QCOMPARE(afterEdit.size(), 2);
  for (const StoredModel& row : afterEdit) {
    if (row.record.id != narrowed.model.record.id) continue;
    QCOMPARE(row.record.availability.value, Availability::Unavailable);
    QCOMPARE(row.record.availability.reasonKey, QStringLiteral("model.store.manifest"));
    QVERIFY(row.record.availableBackends.isEmpty());
    QVERIFY(!row.record.isOffered());
  }
}

// A vouch that arrives after the row was published has to reach the row without a
// new identity or a second copy, and an import that vouches for nothing must not
// take the claim away again.
void TestNativeCatalog::aReimportUpdatesOnlyTheRecordedSupportAndNeverErasesIt() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  const ModelProfile profile = fixedProfile(QStringLiteral("fp16"), 64, 64, 128, 128);
  const QByteArray bytes = QByteArrayLiteral("graph-later-vouch");
  const ModelImportOutcome first =
      inventory.publish(bytes, inspected(profile), QStringLiteral("C:/l.onnx"),
                        QStringLiteral("l.onnx"));
  QVERIFY2(first.ok, qPrintable(first.reason));
  QVERIFY(first.published);
  QVERIFY(first.model.record.availableBackends.isEmpty());

  const QString manifestPath = QDir(fixture.root())
                                   .filePath(storeTokenOf(first.model.record.id) +
                                             QLatin1String("/record.json"));
  const QJsonObject before = QJsonDocument::fromJson(readAll(manifestPath)).object();
  QVERIFY(before.value(QStringLiteral("backends")).toArray().isEmpty());

  // The recipe asks for TensorRT and the runtime carries it, but the row claims no
  // backend: the model's own recorded compatibility is what refuses the plan.
  SpatialStage stage;
  stage.modelId = first.model.record.id;
  Recipe recipe;
  recipe.id = QStringLiteral("janai.test.support.v1");
  recipe.label = recipe.id;
  recipe.spatial = {stage};
  recipe.backend = Backend::TensorRT;

  PlanRequest request;
  request.recipe = recipe;
  request.catalog.models = {first.model.record};
  request.backends.available = {Backend::TensorRT};
  request.source.codedSize = QSize(64, 64);
  request.source.fps = Rational{24, 1};
  request.device.decoderDeviceUuid = QStringLiteral("test-decoder-0");
  request.device.computeDeviceUuid = QStringLiteral("test-compute-0");
  request.device.smMajor = 8;
  request.device.smMinor = 9;

  const PreparationPlan blocked = resolvePreparationPlan(request);
  QVERIFY(!blocked.isResolved());
  QVERIFY(!blocked.blockers.isEmpty());
  QCOMPARE(blocked.blockers.first().reasonKey, QStringLiteral("model.backend.unsupported"));

  // The provisioner now vouches for TensorRT. These bytes are already published,
  // so only the recorded support is updated.
  BackendAvailability vouched;
  vouched.available = {Backend::TensorRT};
  const ModelImportOutcome updated =
      inventory.publish(bytes, inspected(profile), QStringLiteral("C:/l.onnx"),
                        QStringLiteral("l.onnx"), vouched);
  QVERIFY2(updated.ok, qPrintable(updated.reason));
  QVERIFY(!updated.published);
  QCOMPARE(updated.model.record.id, first.model.record.id);
  QCOMPARE(updated.model.record.contentId(), first.model.record.contentId());
  QCOMPARE(updated.model.storedPath, first.model.storedPath);
  QCOMPARE(updated.model.record.onnxSha256, sha256HexOf(bytes));
  QCOMPARE(updated.model.record.availableBackends.size(), 1);
  QCOMPARE(updated.model.record.availableBackends.at(0), Backend::TensorRT);
  QVERIFY(updated.model.record.supportsBackend(Backend::TensorRT));
  // One row, one copy, and the owned bytes untouched.
  QCOMPARE(QDir(fixture.root()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
  QCOMPARE(readAll(updated.model.storedPath), bytes);

  // Only the support member moved: with it set aside, the document is the very
  // one that was written - the identity, provenance, declared boundary,
  // availability and reference evidence included.
  QJsonObject after = QJsonDocument::fromJson(readAll(manifestPath)).object();
  QCOMPARE(after.value(QStringLiteral("backends")).toArray().size(), 1);
  QCOMPARE(after.value(QStringLiteral("backends")).toArray().at(0).toString(),
           QStringLiteral("tensorrt"));
  after.remove(QStringLiteral("backends"));
  QJsonObject written = before;
  written.remove(QStringLiteral("backends"));
  QCOMPARE(QJsonDocument(after).toJson(QJsonDocument::Compact),
           QJsonDocument(written).toJson(QJsonDocument::Compact));

  // Reloaded from its own document the row carries the vouch, and the plan that
  // was refused for the model's lack of support is refused no longer.
  const QVector<StoredModel> reloaded = inventory.stored();
  QCOMPARE(reloaded.size(), 1);
  QVERIFY(reloaded.first().record.supportsBackend(Backend::TensorRT));
  request.catalog.models = {reloaded.first().record};
  const PreparationPlan resolved = resolvePreparationPlan(request);
  QVERIFY2(resolved.isResolved(),
           qPrintable(QStringLiteral("%1: %2").arg(resolved.availability.reasonKey,
                                                   resolved.availability.reason)));
  QCOMPARE(resolved.stages.size(), 1);
  QCOMPARE(resolved.stages.first().scale.value(), 2);

  // An import that vouches for nothing says nothing new: the claim stays.
  const ModelImportOutcome again =
      inventory.publish(bytes, inspected(profile), QStringLiteral("C:/l.onnx"),
                        QStringLiteral("l.onnx"));
  QVERIFY2(again.ok, qPrintable(again.reason));
  QVERIFY(!again.published);
  QCOMPARE(again.model.record.id, first.model.record.id);
  QCOMPARE(again.model.record.availableBackends.size(), 1);
  QCOMPARE(again.model.record.availableBackends.at(0), Backend::TensorRT);
  const QVector<StoredModel> afterEmpty = inventory.stored();
  QCOMPARE(afterEmpty.size(), 1);
  QVERIFY(afterEmpty.first().record.supportsBackend(Backend::TensorRT));
}

void TestNativeCatalog::fp32GraphIsRecordedUnsupportedWithDtypeReason() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  const QByteArray bytes = identityGraph(1 /* ONNX TensorProto.FLOAT */, std::nullopt, std::nullopt);
  const InspectionResult declared = inspectDeclaredProfile(ortRuntime(), bytes);
  QVERIFY2(declared.ok, qPrintable(declared.reason));
  QCOMPARE(declared.declared.input.dtype, QStringLiteral("fp32"));
  QCOMPARE(declared.declared.output.dtype, QStringLiteral("fp32"));
  QCOMPARE(declared.declared.input.rank, 4);
  QVERIFY(declared.declared.isDynamicGeometry());

  const InspectionResult run = inspectGraph(ortRuntime(), bytes, QSize(320, 180));
  QVERIFY(run.ok);
  QVERIFY(!run.referenceRan);
  QVERIFY(run.referenceKey.isEmpty());

  const ModelProfile profile = fixedProfile(QStringLiteral("fp32"), 64, 64, 128, 128);
  const ModelImportOutcome outcome =
      inventory.publish(bytes, inspected(profile), QStringLiteral("C:/fp32.onnx"),
                        QStringLiteral("fp32.onnx"));
  QVERIFY(!outcome.ok);
  QVERIFY(outcome.published);
  QCOMPARE(outcome.model.record.availability.value, Availability::Unavailable);
  QCOMPARE(outcome.model.record.availability.reasonKey, QStringLiteral("model.io.unsupported"));
  QVERIFY(outcome.model.record.availability.reason.contains(QStringLiteral("fp32")));
  QVERIFY(!outcome.model.importResolution.has_value());
}

void TestNativeCatalog::bf16GraphIsRecordedUnsupportedWithDtypeReason() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());
  QTemporaryDir sourceDirectory;
  QVERIFY(sourceDirectory.isValid());

  // A real bf16 graph, written to disk: the import reads it, records it
  // unsupported with the observed dtype, and never executes it.
  const QByteArray graph =
      identityGraph(16 /* ONNX TensorProto.BFLOAT16 */, std::nullopt, std::nullopt);
  const QString sourcePath = QDir(sourceDirectory.path()).filePath(QStringLiteral("bf16.onnx"));
  QVERIFY(writeFile(sourcePath, graph));

  const ModelImportOutcome outcome =
      inventory.importFile(sourcePath, ortRuntime(), QSize(320, 180));
  QVERIFY(!outcome.ok);
  QVERIFY(outcome.published);
  QCOMPARE(outcome.model.record.availability.value, Availability::Unavailable);
  QCOMPARE(outcome.model.record.availability.reasonKey, QStringLiteral("model.io.unsupported"));
  QVERIFY(outcome.model.record.availability.reason.contains(QStringLiteral("bf16")));
  QVERIFY(outcome.model.record.availability.reason.contains(QStringLiteral("fp16")));
  QVERIFY(!outcome.model.importResolution.has_value());

  const InspectionResult run = inspectGraph(ortRuntime(), graph, QSize(320, 180));
  QVERIFY(run.ok);
  QVERIFY(!run.referenceRan);
  QVERIFY(run.referenceKey.isEmpty());
}

void TestNativeCatalog::truncatedByteArrayRefusesWithReasonKey() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  const QByteArray graph = identityGraph(kOnnxFloat16, std::nullopt, std::nullopt);
  const QByteArray truncated = graph.left(16);
  const InspectionResult result = inspectDeclaredProfile(ortRuntime(), truncated);
  QVERIFY(!result.ok);
  QVERIFY(!result.reasonKey.isEmpty());
  QVERIFY(!result.reason.isEmpty());
  QVERIFY(inventory.stored().isEmpty());
  QVERIFY(QDir(fixture.root()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

void TestNativeCatalog::corruptProtobufTailRefusesWithReason() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }
  const QByteArray graph = identityGraph(kOnnxFloat16, std::nullopt, std::nullopt);
  QByteArray corrupt = graph;
  corrupt.append(QByteArrayLiteral("\xFF\xFE\xFD\xFC\x00\x01\x02\x03"));
  const InspectionResult result = inspectDeclaredProfile(ortRuntime(), corrupt);
  QVERIFY(!result.ok);
  QVERIFY(!result.reasonKey.isEmpty());
  QVERIFY(!result.reason.isEmpty());
}

namespace {
QByteArray onnxModelNoInputs(const QVector<QByteArray>& nodes, const QVector<QByteArray>& outputs,
                              int opset = 17) {
  QByteArray graph;
  for (const QByteArray& n : nodes) putBytesField(&graph, 1, n);
  putStringField(&graph, 2, QStringLiteral("mikai.fixture"));
  for (const QByteArray& output : outputs) putBytesField(&graph, 12, output);
  QByteArray opsetImport;
  putVarintField(&opsetImport, 2, static_cast<quint64>(opset));
  QByteArray model;
  putVarintField(&model, 1, 8);
  putStringField(&model, 2, QStringLiteral("mikai.tests"));
  putBytesField(&model, 7, graph);
  putBytesField(&model, 8, opsetImport);
  return model;
}

QByteArray onnxModelNoOutputs(const QVector<QByteArray>& nodes, const QVector<QByteArray>& inputs,
                               int opset = 17) {
  QByteArray graph;
  for (const QByteArray& n : nodes) putBytesField(&graph, 1, n);
  putStringField(&graph, 2, QStringLiteral("mikai.fixture"));
  for (const QByteArray& input : inputs) putBytesField(&graph, 11, input);
  QByteArray opsetImport;
  putVarintField(&opsetImport, 2, static_cast<quint64>(opset));
  QByteArray model;
  putVarintField(&model, 1, 8);
  putStringField(&model, 2, QStringLiteral("mikai.tests"));
  putBytesField(&model, 7, graph);
  putBytesField(&model, 8, opsetImport);
  return model;
}

QByteArray multiOutputGraph() {
  const QVector<std::optional<qint64>> dims = {1, 3, std::nullopt, std::nullopt};
  return onnxModel({node(QStringLiteral("Identity"), {QStringLiteral("X")},
                         {QStringLiteral("Y1"), QStringLiteral("Y2")}, QStringLiteral("identity"))},
                   {valueInfo(QStringLiteral("X"), kOnnxFloat16, dims)},
                   {valueInfo(QStringLiteral("Y1"), kOnnxFloat16, dims),
                    valueInfo(QStringLiteral("Y2"), kOnnxFloat16, dims)});
}
}  // namespace

void TestNativeCatalog::zeroOrMultipleIOCountsAreRefusedByNames() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }
  const QVector<std::optional<qint64>> dims = {1, 3, std::nullopt, std::nullopt};
  const QByteArray zeroInputs =
      onnxModelNoInputs({node(QStringLiteral("Identity"), {}, {QStringLiteral("Y")}, QStringLiteral("identity"))},
                        {valueInfo(QStringLiteral("Y"), kOnnxFloat16, dims)});
  // Each degenerate boundary is refused by a named reason, never accepted.
  const InspectionResult r0i = inspectDeclaredProfile(ortRuntime(), zeroInputs);
  QVERIFY(!r0i.ok);
  QVERIFY(!r0i.reasonKey.isEmpty());
  QVERIFY(!r0i.reason.isEmpty());

  const QByteArray zeroOutputs =
      onnxModelNoOutputs({node(QStringLiteral("Identity"), {QStringLiteral("X")}, {}, QStringLiteral("identity"))},
                         {valueInfo(QStringLiteral("X"), kOnnxFloat16, dims)});
  const InspectionResult r0o = inspectDeclaredProfile(ortRuntime(), zeroOutputs);
  QVERIFY(!r0o.ok);
  QVERIFY(!r0o.reasonKey.isEmpty());
  QVERIFY(!r0o.reason.isEmpty());

  const InspectionResult rmulti = inspectDeclaredProfile(ortRuntime(), multiOutputGraph());
  QVERIFY(!rmulti.ok);
  QVERIFY(!rmulti.reasonKey.isEmpty());
  QVERIFY(!rmulti.reason.isEmpty());
}

void TestNativeCatalog::rank3AndRank5GraphsAreRefusedNamingShape() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }

  const QVector<std::optional<qint64>> dims3 = {1, 3, std::nullopt};
  const QByteArray rank3 =
      onnxModel({node(QStringLiteral("Identity"), {QStringLiteral("X")}, {QStringLiteral("Y")},
                      QStringLiteral("identity"))},
                {valueInfo(QStringLiteral("X"), kOnnxFloat16, dims3)},
                {valueInfo(QStringLiteral("Y"), kOnnxFloat16, dims3)});
  const InspectionResult r3 = inspectDeclaredProfile(ortRuntime(), rank3);
  QVERIFY(r3.ok);
  QCOMPARE(r3.declared.input.rank, 3);
  QVERIFY(!spatialCandidateRefusal(r3.declared).isEmpty());
  QVERIFY(spatialCandidateRefusal(r3.declared).contains(QStringLiteral("rank")));

  const QVector<std::optional<qint64>> dims5 = {1, 3, std::nullopt, std::nullopt, std::nullopt};
  const QByteArray rank5 =
      onnxModel({node(QStringLiteral("Identity"), {QStringLiteral("X")}, {QStringLiteral("Y")},
                      QStringLiteral("identity"))},
                {valueInfo(QStringLiteral("X"), kOnnxFloat16, dims5)},
                {valueInfo(QStringLiteral("Y"), kOnnxFloat16, dims5)});
  const InspectionResult r5 = inspectDeclaredProfile(ortRuntime(), rank5);
  QVERIFY(r5.ok);
  QCOMPARE(r5.declared.input.rank, 5);
  QVERIFY(!spatialCandidateRefusal(r5.declared).isEmpty());
  QVERIFY(spatialCandidateRefusal(r5.declared).contains(QStringLiteral("rank")));
}

void TestNativeCatalog::declaredInputBatchNotOneIsRefused() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }

  const QVector<std::optional<qint64>> dims = {2, 3, std::nullopt, std::nullopt};
  const QByteArray graph =
      onnxModel({node(QStringLiteral("Identity"), {QStringLiteral("X")}, {QStringLiteral("Y")},
                      QStringLiteral("identity"))},
                {valueInfo(QStringLiteral("X"), kOnnxFloat16, dims)},
                {valueInfo(QStringLiteral("Y"), kOnnxFloat16, dims)});
  const InspectionResult result = inspectDeclaredProfile(ortRuntime(), graph);
  QVERIFY(result.ok);
  QCOMPARE(result.declared.input.batch, 2);
  QVERIFY(!spatialCandidateRefusal(result.declared).isEmpty());
  QVERIFY(spatialCandidateRefusal(result.declared).contains(QStringLiteral("N=1")));
}

void TestNativeCatalog::channelC1GraphIsRefused() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }

  const QVector<std::optional<qint64>> dims = {1, 1, std::nullopt, std::nullopt};
  const QByteArray graph =
      onnxModel({node(QStringLiteral("Identity"), {QStringLiteral("X")}, {QStringLiteral("Y")},
                      QStringLiteral("identity"))},
                {valueInfo(QStringLiteral("X"), kOnnxFloat16, dims)},
                {valueInfo(QStringLiteral("Y"), kOnnxFloat16, dims)});
  const InspectionResult result = inspectDeclaredProfile(ortRuntime(), graph);
  QVERIFY(result.ok);
  QCOMPARE(result.declared.input.channels, 1);
  QVERIFY(!spatialCandidateRefusal(result.declared).isEmpty());
  QVERIFY(spatialCandidateRefusal(result.declared).contains(QStringLiteral("C=3")));
}

void TestNativeCatalog::dynamicGraphAtFixedStageProvesReferenceAndKeepsDeclaredMeta() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }

  const QByteArray graph = symbolicOutputBatchGraph();
  const InspectionResult declared = inspectDeclaredProfile(ortRuntime(), graph);
  QVERIFY2(declared.ok, qPrintable(declared.reason));
  QCOMPARE(declared.declared.input.dtype, QStringLiteral("fp16"));
  QCOMPARE(declared.declared.output.dtype, QStringLiteral("fp16"));
  QCOMPARE(declared.declared.input.rank, 4);
  QCOMPARE(declared.declared.output.rank, 4);
  QVERIFY(declared.declared.input.height.has_value() == false ||
          !declared.declared.input.height.has_value());
  QVERIFY(declared.declared.isDynamicGeometry());

  const QSize stage(320, 180);
  const InspectionResult run = inspectGraph(ortRuntime(), graph, stage);
  QVERIFY2(run.ok, qPrintable(run.reason));
  if (run.referenceRan) {
    // The generated fixture is an identity graph: the run proves the declared
    // boundary by echoing the stage geometry, not by upscaling it.
    QVERIFY(run.resolvedGeometry.has_value());
    QCOMPARE(*run.resolvedGeometry, stage);
    QVERIFY(run.resolvedScale.has_value());
    QCOMPARE(*run.resolvedScale, 1);
    QVERIFY(run.verifiedReferenceProfile.has_value());
    QCOMPARE(run.declared.input.batch, 1);
    // The generated fixture resolves its symbolic batch to 1 when ORT reads
    // the graph; what matters is the reference proof beside it, not whether
    // the reader preserved -1 on this fixture.
    QVERIFY(run.verifiedReferenceProfile->output.batch == 1);
    QVERIFY(run.declared.isDynamicGeometry());
  } else {
    QVERIFY(!run.referenceKey.isEmpty());
    QVERIFY(!run.referenceReason.isEmpty());
    QVERIFY(!run.resolvedScale.has_value());
  }
}

void TestNativeCatalog::twoFilesSharingABasenameGetTwoIdentities() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  const ModelProfile profile = fixedProfile(QStringLiteral("fp16"), 64, 64, 128, 128);
  const ModelImportOutcome first =
      inventory.publish(QByteArrayLiteral("graph-one"), inspected(profile),
                        QStringLiteral("C:/one/model.onnx"), QStringLiteral("model.onnx"));
  const ModelImportOutcome second =
      inventory.publish(QByteArrayLiteral("graph-two"), inspected(profile),
                        QStringLiteral("C:/two/model.onnx"), QStringLiteral("model.onnx"));
  QVERIFY(first.ok && second.ok);
  QCOMPARE(first.model.record.displayName, second.model.record.displayName);
  QVERIFY(first.model.record.id != second.model.record.id);
  QCOMPARE(inventory.stored().size(), 2);
}

void TestNativeCatalog::republishingTheSameGraphIsIdempotent() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  const ModelProfile profile = fixedProfile(QStringLiteral("fp16"), 64, 64, 128, 128);
  const QByteArray bytes = QByteArrayLiteral("graph-idempotent");
  const ModelImportOutcome first =
      inventory.publish(bytes, inspected(profile), QStringLiteral("C:/a.onnx"),
                        QStringLiteral("a.onnx"));
  QVERIFY(first.ok && first.published);
  const ModelImportOutcome again =
      inventory.publish(bytes, inspected(profile), QStringLiteral("C:/b.onnx"),
                        QStringLiteral("b.onnx"));
  QVERIFY(again.ok);
  QVERIFY(!again.published);
  QCOMPARE(again.model.record.id, first.model.record.id);
  QCOMPARE(inventory.stored().size(), 1);
}

void TestNativeCatalog::anUnsupportedDtypeIsRecordedNotExecuted() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  // The declared read succeeded; no run was attempted, because a non-FP16 graph
  // is never executed. That is exactly the shape inspectGraph returns for fp32.
  for (const QString& dtype : {QStringLiteral("fp32"), QStringLiteral("bf16")}) {
    const ModelProfile profile = fixedProfile(dtype, 64, 64, 128, 128);
    const QByteArray bytes = QByteArrayLiteral("graph-") + dtype.toUtf8();
    const ModelImportOutcome outcome =
        inventory.publish(bytes, inspected(profile), QStringLiteral("C:/u.onnx"),
                          QStringLiteral("u.onnx"));
    QVERIFY2(!outcome.ok, qPrintable(dtype));
    QVERIFY2(outcome.published, qPrintable(dtype));
    QCOMPARE(outcome.model.record.availability.value, Availability::Unavailable);
    QCOMPARE(outcome.model.record.availability.reasonKey, QStringLiteral("model.io.unsupported"));
    QVERIFY(outcome.model.record.availability.reason.contains(dtype));
    QVERIFY(outcome.model.record.availability.reason.contains(QStringLiteral("fp16")));
    QVERIFY(!outcome.model.record.isOffered());
    QVERIFY(!outcome.model.importResolution.has_value());
  }
  // Both were recorded, neither was dropped.
  QString listReason;
  QCOMPARE(inventory.stored(&listReason).size(), 2);
  QVERIFY(listReason.isEmpty());
}

void TestNativeCatalog::aDynamicGraphWhoseReferenceDidNotRunIsRecordedUnavailable() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  InspectionResult inspection = inspected(dynamicProfile(QStringLiteral("fp16")));
  inspection.referenceKey = QStringLiteral("inspect.reference.run");
  inspection.referenceReason = QStringLiteral("NOT_IMPLEMENTED: no CPU kernel");

  const ModelImportOutcome outcome =
      inventory.publish(QByteArrayLiteral("graph-dynamic"), inspection, QStringLiteral("C:/d.onnx"),
                        QStringLiteral("d.onnx"));
  QVERIFY(!outcome.ok);
  QVERIFY(outcome.published);
  QCOMPARE(outcome.model.record.availability.reasonKey, QStringLiteral("model.reference.failed"));
  QVERIFY(outcome.model.record.availability.reason.contains(QStringLiteral("NOT_IMPLEMENTED")));
  // The declared profile is kept as the graph declared it, not filled with a
  // fabricated geometry.
  QVERIFY(outcome.model.record.actualProfile.isDynamicGeometry());
  QVERIFY(!outcome.model.importResolution.has_value());
}

void TestNativeCatalog::aRefusedBoundaryLeavesTheStoreUnchanged() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  ModelProfile rank5 = fixedProfile(QStringLiteral("fp16"), 8, 8, 16, 16);
  rank5.input.rank = 5;
  rank5.output.rank = 5;

  ModelProfile channels4 = fixedProfile(QStringLiteral("fp16"), 8, 8, 16, 16);
  channels4.input.channels = 4;
  channels4.output.channels = 4;

  ModelProfile nonUniform = fixedProfile(QStringLiteral("fp16"), 8, 8, 16, 24);

  for (const ModelProfile& profile : {rank5, channels4, nonUniform}) {
    const ModelImportOutcome outcome =
        inventory.publish(QByteArrayLiteral("graph-refused"), inspected(profile),
                          QStringLiteral("C:/r.onnx"), QStringLiteral("r.onnx"));
    QVERIFY(!outcome.ok);
    QVERIFY(!outcome.published);
    QCOMPARE(outcome.reasonKey, QStringLiteral("model.profile.ineligible"));
    QVERIFY(!outcome.reason.isEmpty());
  }
  QVERIFY(inventory.stored().isEmpty());
  QVERIFY(QDir(fixture.root()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

void TestNativeCatalog::anImportCancelledBeforePublishingWritesNothing() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  QTemporaryDir sourceDirectory;
  QVERIFY(sourceDirectory.isValid());

  // A real graph, generated in-process, so the cancellation is proven to stop a
  // genuine import rather than an empty input.
  const QByteArray graph = identityGraph(kOnnxFloat16, std::nullopt, std::nullopt);
  const QString sourcePath = QDir(sourceDirectory.path()).filePath(QStringLiteral("model.onnx"));
  QVERIFY(writeFile(sourcePath, graph));

  ModelInventory inventory(fixture.root());
  ImportCancellation cancellation;
  QVERIFY(cancellation.cancel() == ImportCancellation::CancelResult::CancelledNow);

  const ModelImportOutcome outcome = inventory.importFile(
      sourcePath, ortRuntime(), QSize(320, 180), BackendAvailability(), QString(), &cancellation);
  QVERIFY(!outcome.ok);
  QVERIFY(!outcome.published);
  QCOMPARE(outcome.reasonKey, QStringLiteral("native.import.cancelled"));
  // The cancel won before anything was inspected or written: no store root was
  // created, no row exists, and no staging directory was left behind.
  QVERIFY(!QFileInfo(fixture.root()).exists());
  QVERIFY(inventory.stored().isEmpty());
}

void TestNativeCatalog::anEditedOrRemovedSourceDoesNotInvalidateTheOwnedCopy() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  QTemporaryDir sourceDirectory;
  QVERIFY(sourceDirectory.isValid());

  const QString sourcePath = QDir(sourceDirectory.path()).filePath(QStringLiteral("model.onnx"));
  const QByteArray original = QByteArrayLiteral("original-source-bytes");
  QVERIFY(writeFile(sourcePath, original));

  ModelInventory inventory(fixture.root());
  const ModelImportOutcome published =
      inventory.publish(original, inspected(fixedProfile(QStringLiteral("fp16"), 64, 64, 128, 128)),
                        sourcePath, QStringLiteral("model.onnx"));
  QVERIFY(published.ok);

  QString key;
  QString reason;
  QVERIFY(inventory.verifyOwnedModelIntegrity(published.model, &key, &reason));

  // The original is provenance, not a runtime input: editing it in place, then
  // removing it, leaves the owned copy's integrity untouched.
  QVERIFY(writeFile(sourcePath, QByteArrayLiteral("edited-source-bytes!!")));
  QVERIFY(inventory.verifyOwnedModelIntegrity(published.model, &key, &reason));
  QVERIFY(QFile::remove(sourcePath));
  QVERIFY(inventory.verifyOwnedModelIntegrity(published.model, &key, &reason));

  // What does invalidate it is a change to the owned bytes themselves.
  QVERIFY(writeFile(published.model.storedPath, QByteArrayLiteral("original-source-bytes!")));
  QVERIFY(!inventory.verifyOwnedModelIntegrity(published.model, &key, &reason));
  QCOMPARE(key, QStringLiteral("model.owned.changed"));
}

void TestNativeCatalog::aTamperedCopyIsReportedUnavailableNotDropped() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  const ModelImportOutcome published =
      inventory.publish(QByteArrayLiteral("graph-to-tamper"),
                        inspected(fixedProfile(QStringLiteral("fp16"), 64, 64, 128, 128)),
                        QStringLiteral("C:/t.onnx"), QStringLiteral("t.onnx"));
  QVERIFY(published.ok);
  QVERIFY(writeFile(published.model.storedPath, QByteArrayLiteral("graph-to-tamper-plus-junk")));

  const QVector<StoredModel> rows = inventory.stored();
  QCOMPARE(rows.size(), 1);
  QCOMPARE(rows.at(0).record.availability.value, Availability::Unavailable);
  QCOMPARE(rows.at(0).record.availability.reasonKey, QStringLiteral("model.store.digest"));
  QVERIFY(!rows.at(0).record.availability.reason.isEmpty());
  QVERIFY(!rows.at(0).record.isOffered());
}

void TestNativeCatalog::anEditedManifestIdentityIsReportedNotTrusted() {
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  ModelInventory inventory(fixture.root());

  const ModelImportOutcome published =
      inventory.publish(QByteArrayLiteral("graph-manifest"),
                        inspected(fixedProfile(QStringLiteral("fp16"), 64, 64, 128, 128)),
                        QStringLiteral("C:/m.onnx"), QStringLiteral("m.onnx"));
  QVERIFY(published.ok);

  // Change the declared geometry in the manifest without touching the bytes: the
  // id no longer derives from the row, so the row is refused rather than trusted.
  const QString manifestPath = QDir(fixture.root())
                                   .filePath(storeTokenOf(published.model.record.id) +
                                             QLatin1String("/record.json"));
  QJsonObject manifest = QJsonDocument::fromJson(readAll(manifestPath)).object();
  QJsonObject declared = manifest.value(QStringLiteral("declared")).toObject();
  QJsonObject output = declared.value(QStringLiteral("output")).toObject();
  output.insert(QStringLiteral("h"), 256);
  output.insert(QStringLiteral("w"), 256);
  declared.insert(QStringLiteral("output"), output);
  manifest.insert(QStringLiteral("declared"), declared);
  QVERIFY(writeFile(manifestPath, QJsonDocument(manifest).toJson(QJsonDocument::Compact)));

  const QVector<StoredModel> rows = inventory.stored();
  QCOMPARE(rows.size(), 1);
  QCOMPARE(rows.at(0).record.availability.value, Availability::Unavailable);
  QCOMPARE(rows.at(0).record.availability.reasonKey, QStringLiteral("model.store.identity"));
  QVERIFY(!rows.at(0).record.isOffered());
}

void TestNativeCatalog::aGeneratedDynamicGraphResolvesItsActualGeometry() {
  QString reason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &reason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(reason)));
  }

  const QByteArray graph = identityGraph(kOnnxFloat16, std::nullopt, std::nullopt);
  const InspectionResult declared = inspectDeclaredProfile(ortRuntime(), graph);
  QVERIFY2(declared.ok, qPrintable(declared.reason));
  QCOMPARE(declared.declared.input.dtype, QStringLiteral("fp16"));
  QCOMPARE(declared.declared.output.dtype, QStringLiteral("fp16"));
  QCOMPARE(declared.declared.input.rank, 4);
  QCOMPARE(declared.declared.input.batch, 1);
  QCOMPARE(declared.declared.input.channels, 3);
  // Dynamic is absent, never zero and never a placeholder.
  QVERIFY(declared.declared.isDynamicGeometry());
  QVERIFY(!declared.declared.input.height.has_value());
  QVERIFY(spatialEligibilityRefusal(declared.declared).isEmpty());

  // The run is the only thing that can learn the geometry, and it must produce
  // the geometry this graph actually produces - here, the stage size unchanged.
  const InspectionResult run = inspectGraph(ortRuntime(), graph, QSize(320, 180));
  QVERIFY(run.ok);
  QCOMPARE(run.declared.input.rank, 4);
  if (run.referenceRan) {
    QCOMPARE(run.runGeometry, QSize(320, 180));
    QVERIFY(run.resolvedGeometry.has_value());
    QCOMPARE(*run.resolvedGeometry, QSize(320, 180));
    QCOMPARE(*run.resolvedScale, 1);
    QVERIFY(run.referenceKey.isEmpty());
  } else {
    // A CPU execution provider that refuses a fp16 kernel is reported with its
    // own message and no scale; it is never rounded to a fabricated answer.
    QVERIFY(!run.referenceKey.isEmpty());
    QVERIFY(!run.referenceReason.isEmpty());
    QVERIFY(!run.resolvedScale.has_value());
    qWarning("CPU reference did not run: %s", qPrintable(run.referenceReason));
  }
}

void TestNativeCatalog::aGeneratedFixedGraphIsNotExecuted() {
  QString reason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &reason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(reason)));
  }

  const QByteArray graph = identityGraph(kOnnxFloat16, 64, 64);
  const InspectionResult result = inspectGraph(ortRuntime(), graph, QSize(320, 180));
  QVERIFY2(result.ok, qPrintable(result.reason));
  QVERIFY(result.declared.input.height.has_value());
  QCOMPARE(*result.declared.input.height, qint64(64));
  QVERIFY(result.declared.output.width.has_value());
  QCOMPARE(*result.declared.output.width, qint64(64));
  // A fixed graph's own declaration is authoritative: nothing is executed, and
  // the factor comes from the declared shapes.
  QVERIFY(!result.referenceRan);
  QVERIFY(result.referenceKey.isEmpty());
  QVERIFY(result.declared.derivedUniformScale().has_value());
  QCOMPARE(*result.declared.derivedUniformScale(), 1);

  // The resolver answers a fixed graph from its declaration, with no bytes at all.
  QString key;
  QString resolveReason;
  const std::optional<StageResolution> resolved =
      resolveStageGeometry(result.declared, QSize(320, 180), QByteArray(), InspectionRuntime(), &key,
                           &resolveReason);
  QVERIFY2(resolved.has_value(), qPrintable(resolveReason));
  QCOMPARE(resolved->input, QSize(64, 64));
  QCOMPARE(resolved->output, QSize(64, 64));
  QCOMPARE(resolved->scale, 1);
  QVERIFY(!resolved->fromCpuReference);
}

void TestNativeCatalog::aTruncatedGraphIsRefusedAndPublishesNothing() {
  QString reason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &reason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(reason)));
  }
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  QTemporaryDir sourceDirectory;
  QVERIFY(sourceDirectory.isValid());

  const QByteArray graph = identityGraph(kOnnxFloat16, std::nullopt, std::nullopt);
  const QString truncatedPath = QDir(sourceDirectory.path()).filePath(QStringLiteral("truncated.onnx"));
  QVERIFY(writeFile(truncatedPath, graph.left(16)));

  ModelInventory inventory(fixture.root());
  const ModelImportOutcome outcome =
      inventory.importFile(truncatedPath, ortRuntime(), QSize(320, 180));
  QVERIFY(!outcome.ok);
  QCOMPARE(outcome.reasonKey, QStringLiteral("inspect.graph.unreadable"));
  QVERIFY(!outcome.published);
  QVERIFY(!outcome.reason.isEmpty());
  // Nothing reached the store, and no staging directory was left behind.
  QVERIFY(inventory.stored().isEmpty());
  QVERIFY(QDir(fixture.root()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

void TestNativeCatalog::aTwoInputGraphIsRefusedNamingTheCounts() {
  QString reason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &reason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(reason)));
  }
  const InspectionResult result = inspectDeclaredProfile(ortRuntime(), twoInputGraph());
  QVERIFY(!result.ok);
  QCOMPARE(result.reasonKey, QStringLiteral("inspect.graph.ioCount"));
  QVERIFY(result.reason.contains(QStringLiteral("2 input")));
  QVERIFY(result.reason.contains(QStringLiteral("1 output")));
}

void TestNativeCatalog::theRealDynamicGraphLearnsItsScaleAtStageGeometry() {
  const QString modelPath = realDynamicModelPath();
  if (modelPath.isEmpty()) QSKIP("MIKAI_NATIVE_ASSETS is not set");
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }
  const QByteArray bytes = readAll(modelPath);
  QVERIFY2(!bytes.isEmpty(), qPrintable(modelPath));

  const InspectionResult declared = inspectDeclaredProfile(ortRuntime(), bytes);
  QVERIFY2(declared.ok, qPrintable(declared.reason));
  QCOMPARE(declared.declared.input.dtype, QStringLiteral("fp16"));
  QCOMPARE(declared.declared.output.dtype, QStringLiteral("fp16"));
  QCOMPARE(declared.declared.input.rank, 4);
  QCOMPARE(declared.declared.input.batch, 1);
  QCOMPARE(declared.declared.input.channels, 3);
  QVERIFY(declared.declared.isDynamicGeometry());

  // Identity does not depend on any run: same file, same declared profile, same
  // content id at both geometries.
  const QString identity = computeContentId(ModelRole::Spatial, sha256HexOf(bytes), declared.declared);

  const InspectionResult small = inspectGraph(ortRuntime(), bytes, QSize(320, 180));
  QVERIFY(small.ok);
  if (small.referenceRan) {
    QCOMPARE(*small.resolvedGeometry, QSize(640, 360));
    QCOMPARE(*small.resolvedScale, 2);
    const InspectionResult large = inspectGraph(ortRuntime(), bytes, QSize(640, 360));
    QVERIFY(large.ok);
    QCOMPARE(*large.resolvedGeometry, QSize(1280, 720));
    QCOMPARE(*large.resolvedScale, 2);
    QVERIFY(small.runGeometry != large.runGeometry);
    // The same file at a different resolution resolves a different geometry
    // without a different identity.
    QCOMPARE(computeContentId(ModelRole::Spatial, sha256HexOf(bytes), large.declared),
             identity);
  } else {
    // The CPU execution provider refused a kernel of this graph. The exact ORT
    // message is carried out; no scale is invented and the graph is untouched.
    QVERIFY(!small.referenceKey.isEmpty());
    QVERIFY(!small.referenceReason.isEmpty());
    QVERIFY(!small.resolvedScale.has_value());
    qWarning("CPU reference on the acquired dynamic graph did not run: %s",
             qPrintable(small.referenceReason));
  }
}

void TestNativeCatalog::aMalformedReferenceBoundaryYieldsNoGeometryOrProof() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }

  // Each graph runs - or would run - and returns, or declares, a boundary this
  // bridge cannot bind: two batches, four channels, a rank-2 output. None may
  // yield a geometry or a proof: the actual N/C/rank/dtype is checked before any
  // number is recorded, and the declaration is never filled in as a fallback.
  const QVector<QByteArray> graphs = {doubledBatchGraph(), fourChannelGraph(), flattenedGraph()};
  for (const QByteArray& graph : graphs) {
    const InspectionResult result = inspectGraph(ortRuntime(), graph, QSize(32, 16));
    QVERIFY2(result.ok, qPrintable(result.reason));
    QVERIFY(!result.resolvedGeometry.has_value());
    QVERIFY(!result.resolvedScale.has_value());
    QVERIFY(!result.verifiedReferenceProfile.has_value());
    if (result.referenceRan) {
      // The run happened and its own boundary was rejected, with the fact named.
      QVERIFY(!result.referenceKey.isEmpty());
      QVERIFY(!result.referenceReason.isEmpty());
    } else {
      // No run: the declaration was already refused as something this bridge
      // could not bind, and it says so rather than staying silent.
      QVERIFY(!result.referenceKey.isEmpty());
      QVERIFY(!result.reason.isEmpty() || !result.referenceReason.isEmpty());
    }
  }

  // The resolver agrees: no geometry is fabricated for any of them, and a reason
  // is always carried out.
  for (const QByteArray& graph : graphs) {
    const InspectionResult declared = inspectDeclaredProfile(ortRuntime(), graph);
    QVERIFY2(declared.ok, qPrintable(declared.reason));
    QString key;
    QString resolveReason;
    const std::optional<StageResolution> resolved =
        resolveStageGeometry(declared.declared, QSize(32, 16), graph, ortRuntime(), &key,
                             &resolveReason);
    QVERIFY(!resolved.has_value());
    QVERIFY(!key.isEmpty());
    QVERIFY(!resolveReason.isEmpty());
  }
}

void TestNativeCatalog::aGeneratedSymbolicOutputBatchIsProvedByARealReference() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }

  // The declared relaxation is structural and nothing more: an output batch of -1
  // is a candidate, still refused by the strict boundary that requires N=1.
  ModelProfile symbolic = dynamicProfile(QStringLiteral("fp16"));
  symbolic.output.batch = -1;
  QVERIFY(spatialEligibilityRefusal(symbolic).contains(QStringLiteral("N=1")));
  QVERIFY(spatialCandidateRefusal(symbolic).isEmpty());

  const QByteArray graph = symbolicOutputBatchGraph();
  if (!dynamicReferenceRan(graph)) QSKIP("the CPU provider refused the fp16 reference run");

  const InspectionResult declared = inspectDeclaredProfile(ortRuntime(), graph);
  QVERIFY2(declared.ok, qPrintable(declared.reason));
  QCOMPARE(declared.declared.input.dtype, QStringLiteral("fp16"));
  QCOMPARE(declared.declared.input.rank, 4);
  QCOMPARE(declared.declared.input.batch, qint64(1));
  QCOMPARE(declared.declared.input.channels, qint64(3));
  // Reading a declaration is not a run and is never a proof: nothing is filled in
  // until a CPU reference has actually produced a boundary.
  QVERIFY(!declared.referenceRan);
  QVERIFY(!declared.verifiedReferenceProfile.has_value());
  QVERIFY(!declared.resolvedGeometry.has_value());
  QVERIFY(!declared.resolvedScale.has_value());

  const InspectionResult run = inspectGraph(ortRuntime(), graph, QSize(32, 16));
  QVERIFY2(run.ok, qPrintable(run.reason));
  QVERIFY(run.referenceRan);
  QVERIFY2(run.verifiedReferenceProfile.has_value(), qPrintable(run.referenceReason));
  const ModelProfile& proof = *run.verifiedReferenceProfile;
  // The input is the tensor actually bound; the output is what came back.
  QCOMPARE(proof.input.dtype, QStringLiteral("fp16"));
  QCOMPARE(proof.input.rank, 4);
  QCOMPARE(proof.input.batch, qint64(1));
  QCOMPARE(proof.input.channels, qint64(3));
  QVERIFY(proof.input.height.has_value());
  QVERIFY(proof.input.width.has_value());
  QCOMPARE(*proof.input.height, qint64(16));
  QCOMPARE(*proof.input.width, qint64(32));
  QCOMPARE(proof.output.dtype, QStringLiteral("fp16"));
  QCOMPARE(proof.output.rank, 4);
  QCOMPARE(proof.output.batch, qint64(1));
  QCOMPARE(proof.output.channels, qint64(3));
  QVERIFY(proof.output.height.has_value());
  QVERIFY(proof.output.width.has_value());
  QCOMPARE(*proof.output.height, qint64(16));
  QCOMPARE(*proof.output.width, qint64(32));
  QCOMPARE(*run.resolvedGeometry, QSize(32, 16));
  QCOMPARE(*run.resolvedScale, 1);
  QVERIFY(run.referenceKey.isEmpty());
  // The measured batch is evidence: the declaration still says what it said.
  QCOMPARE(run.declared.output.batch, declared.declared.output.batch);
  QCOMPARE(run.declared.output.channels, qint64(3));
  QCOMPARE(run.declared.output.rank, 4);

  // The resolver answers with the same run and carries the full proof.
  QString key;
  QString resolveReason;
  const std::optional<StageResolution> resolved = resolveStageGeometry(
      run.declared, QSize(32, 16), graph, ortRuntime(), &key, &resolveReason);
  QVERIFY2(resolved.has_value(), qPrintable(resolveReason));
  QVERIFY(resolved->fromCpuReference);
  QCOMPARE(resolved->input, QSize(32, 16));
  QCOMPARE(resolved->output, QSize(32, 16));
  QCOMPARE(resolved->scale, 1);
  QVERIFY(resolved->verifiedReferenceProfile.has_value());
  QCOMPARE(resolved->verifiedReferenceProfile->output.batch, qint64(1));
  QCOMPARE(resolved->verifiedReferenceProfile->output.channels, qint64(3));
  QCOMPARE(resolved->verifiedReferenceProfile->input.batch, qint64(1));
}

void TestNativeCatalog::theRealHdGraphKeepsItsDeclaredSymbolicBatchAndProvesTwoBy() {
  const QString modelPath = realHdModelPath();
  if (modelPath.isEmpty()) QSKIP("MIKAI_NATIVE_ASSETS is not set");
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }
  const QByteArray bytes = readAll(modelPath);
  QVERIFY2(!bytes.isEmpty(), qPrintable(modelPath));

  const InspectionResult declared = inspectDeclaredProfile(ortRuntime(), bytes);
  QVERIFY2(declared.ok, qPrintable(declared.reason));
  QCOMPARE(declared.declared.input.dtype, QStringLiteral("fp16"));
  QCOMPARE(declared.declared.input.rank, 4);
  QCOMPARE(declared.declared.input.batch, qint64(1));
  QCOMPARE(declared.declared.input.channels, qint64(3));
  QCOMPARE(declared.declared.output.dtype, QStringLiteral("fp16"));
  QCOMPARE(declared.declared.output.rank, 4);
  QCOMPARE(declared.declared.output.channels, qint64(3));
  // The exporter's symbolic batch is kept verbatim: the metadata says "learn
  // it", and nothing before a run may turn that into a 1.
  QCOMPARE(declared.declared.output.batch, qint64(-1));
  QVERIFY(!declared.referenceRan);
  QVERIFY(!declared.verifiedReferenceProfile.has_value());
  QVERIFY(!declared.resolvedGeometry.has_value());
  // Metadata alone is not eligibility: the strict rule refuses the symbolic
  // batch, and the candidate admits it only as something a run must settle.
  QVERIFY(!spatialEligibilityRefusal(declared.declared).isEmpty());
  QVERIFY(spatialCandidateRefusal(declared.declared).isEmpty());

  const QSize stage(64, 64);
  const InspectionResult run = inspectGraph(ortRuntime(), bytes, stage);
  QVERIFY2(run.ok, qPrintable(run.reason));
  if (!run.referenceRan) {
    QSKIP(qPrintable(QStringLiteral("the CPU provider refused the fp16 reference run: %1")
                         .arg(run.referenceReason)));
  }
  QCOMPARE(run.runGeometry, stage);
  QVERIFY2(run.verifiedReferenceProfile.has_value(), qPrintable(run.referenceReason));
  const ModelProfile& proof = *run.verifiedReferenceProfile;
  QCOMPARE(proof.input.dtype, QStringLiteral("fp16"));
  QCOMPARE(proof.input.batch, qint64(1));
  QCOMPARE(proof.input.channels, qint64(3));
  QVERIFY(proof.input.height.has_value());
  QCOMPARE(*proof.input.height, qint64(64));
  QCOMPARE(proof.output.dtype, QStringLiteral("fp16"));
  QCOMPARE(proof.output.batch, qint64(1));
  QCOMPARE(proof.output.channels, qint64(3));
  QVERIFY(run.resolvedGeometry.has_value());
  QCOMPARE(*run.resolvedGeometry, QSize(128, 128));
  QCOMPARE(*run.resolvedScale, 2);
  QVERIFY(run.referenceKey.isEmpty());
  // The batch the run measured is proof, not a rewrite of the declaration.
  QCOMPARE(run.declared.output.batch, qint64(-1));
  QCOMPARE(run.declared.output.rank, 4);

  QString key;
  QString resolveReason;
  const std::optional<StageResolution> resolved =
      resolveStageGeometry(run.declared, stage, bytes, ortRuntime(), &key, &resolveReason);
  QVERIFY2(resolved.has_value(), qPrintable(resolveReason));
  QVERIFY(resolved->fromCpuReference);
  QCOMPARE(resolved->input, stage);
  QCOMPARE(resolved->output, QSize(128, 128));
  QCOMPARE(resolved->scale, 2);
  QVERIFY(resolved->verifiedReferenceProfile.has_value());
  QCOMPARE(resolved->verifiedReferenceProfile->output.batch, qint64(1));
  QCOMPARE(resolved->verifiedReferenceProfile->output.channels, qint64(3));
}

// ------------------------------------------------- the inspected-plan adapter

void TestNativeCatalog::anOwnedDynamicStageResolvesThroughThePlanner() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  QTemporaryDir sourceDirectory;
  QVERIFY(sourceDirectory.isValid());

  const QByteArray graph = identityGraph(kOnnxFloat16, std::nullopt, std::nullopt);
  if (!dynamicReferenceRan(graph)) QSKIP("the CPU provider refused the fp16 reference run");
  const QString sourcePath = QDir(sourceDirectory.path()).filePath(QStringLiteral("model.onnx"));
  QVERIFY(writeFile(sourcePath, graph));

  ModelInventory inventory(fixture.root());
  StoredModel stored;
  QVERIFY(publishGenerated(&inventory, sourcePath, graph, QStringLiteral("model.onnx"), &stored));
  QVERIFY(stored.record.availability.isUsable());

  InspectedPlanResolver resolver(fixture.root(), ortRuntime());
  QString storeKey;
  QString storeReason;
  QVERIFY2(resolver.refresh(&storeKey, &storeReason), qPrintable(storeKey + ": " + storeReason));
  const CatalogView catalog = resolver.catalog();
  QCOMPARE(catalog.models.size(), 1);
  QCOMPARE(catalog.models.first().id, stored.record.id);

  SpatialStage stage;
  stage.modelId = stored.record.id;
  Recipe recipe;
  recipe.id = QStringLiteral("janai.test.inspected.v1");
  recipe.label = recipe.id;
  recipe.spatial = {stage};
  recipe.backend = Backend::TensorRT;

  PlanRequest request;
  request.recipe = recipe;
  request.catalog = catalog;
  request.backends.available = {Backend::TensorRT};
  request.source.codedSize = QSize(320, 180);
  request.source.fps = Rational{24, 1};
  request.device.decoderDeviceUuid = QStringLiteral("test-decoder-0");
  request.device.computeDeviceUuid = QStringLiteral("test-compute-0");
  request.device.smMajor = 8;
  request.device.smMinor = 9;
  request.dynamicResolver = resolver.resolver();

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY2(plan.isResolved(),
           qPrintable(QStringLiteral("%1: %2 / resolver %3: %4")
                          .arg(plan.availability.reasonKey, plan.availability.reason,
                               resolver.lastReasonKey(), resolver.lastReason())));
  QCOMPARE(plan.stages.size(), 1);
  const ResolvedStage& resolved = plan.stages.first();
  QVERIFY(resolved.geometryFromResolver);
  QCOMPARE(resolved.inputGeometry, QSize(320, 180));
  QCOMPARE(resolved.outputGeometry, QSize(320, 180));
  QCOMPARE(resolved.scale.value(), 1);
  QCOMPARE(resolved.modelContentId, stored.record.contentId());
  // The geometry came from the owned bytes, so a different stage size is a
  // different answer from the very same graph.
  QCOMPARE(plan.outputGeometry().value(), QSize(320, 180));
}

// A reloaded row keeps the backend support it was published with, and the planner
// still requires the runtime to carry that backend separately: model compatibility
// and runtime capability are two facts, never one substituted for the other.
void TestNativeCatalog::aReloadedRowKeepsItsRecordedBackendsAndTheRuntimeIsStillRequired() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  QTemporaryDir sourceDirectory;
  QVERIFY(sourceDirectory.isValid());

  const QByteArray graph = identityGraph(kOnnxFloat16, std::nullopt, std::nullopt);
  if (!dynamicReferenceRan(graph)) QSKIP("the CPU provider refused the fp16 reference run");
  const QString sourcePath = QDir(sourceDirectory.path()).filePath(QStringLiteral("model.onnx"));
  QVERIFY(writeFile(sourcePath, graph));

  ModelInventory inventory(fixture.root());
  StoredModel stored;
  QVERIFY(publishGenerated(&inventory, sourcePath, graph, QStringLiteral("model.onnx"), &stored));
  // The provisioner vouched for TensorRT, and the row records exactly that claim.
  QCOMPARE(stored.record.availableBackends.size(), 1);
  QCOMPARE(stored.record.availableBackends.at(0), Backend::TensorRT);
  QVERIFY(stored.record.supportsBackend(Backend::TensorRT));
  QVERIFY(!stored.record.supportsBackend(Backend::DirectML));

  InspectedPlanResolver resolver(fixture.root(), ortRuntime());
  QString storeKey;
  QString storeReason;
  QVERIFY2(resolver.refresh(&storeKey, &storeReason), qPrintable(storeKey + ": " + storeReason));
  const CatalogView catalog = resolver.catalog();
  QCOMPARE(catalog.models.size(), 1);
  const ModelRecord& reloaded = catalog.models.first();
  QCOMPARE(reloaded.id, stored.record.id);
  // Read back from the row's own document: the support did not become empty.
  QCOMPARE(reloaded.availableBackends.size(), 1);
  QCOMPARE(reloaded.availableBackends.at(0), Backend::TensorRT);
  QVERIFY(reloaded.supportsBackend(Backend::TensorRT));

  SpatialStage stage;
  stage.modelId = stored.record.id;
  Recipe recipe;
  recipe.id = QStringLiteral("janai.test.inspected.v1");
  recipe.label = recipe.id;
  recipe.spatial = {stage};
  recipe.backend = Backend::TensorRT;

  PlanRequest request;
  request.recipe = recipe;
  request.catalog = catalog;
  request.backends.available = {Backend::TensorRT};
  request.source.codedSize = QSize(320, 180);
  request.source.fps = Rational{24, 1};
  request.device.decoderDeviceUuid = QStringLiteral("test-decoder-0");
  request.device.computeDeviceUuid = QStringLiteral("test-compute-0");
  request.device.smMajor = 8;
  request.device.smMinor = 9;
  request.dynamicResolver = resolver.resolver();

  const PreparationPlan plan = resolvePreparationPlan(request);
  QVERIFY2(plan.isResolved(),
           qPrintable(QStringLiteral("%1: %2 / resolver %3: %4")
                          .arg(plan.availability.reasonKey, plan.availability.reason,
                               resolver.lastReasonKey(), resolver.lastReason())));

  // The same row, the same recipe, a runtime that carries nothing: the refusal is
  // the runtime's own, and the model's recorded support is never used to stand in
  // for it.
  PlanRequest withoutRuntime = request;
  withoutRuntime.backends.available.clear();
  const PreparationPlan blocked = resolvePreparationPlan(withoutRuntime);
  QVERIFY(!blocked.isResolved());
  QVERIFY(!blocked.blockers.isEmpty());
  QCOMPARE(blocked.blockers.first().reasonKey, QStringLiteral("backend.unavailable"));
  for (const PlanBlocker& blocker : blocked.blockers) {
    QVERIFY(blocker.reasonKey != QStringLiteral("model.backend.unsupported"));
  }
}
void TestNativeCatalog::twoSameNamedOwnedGraphsResolveTheirOwnBytes() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  QTemporaryDir firstDirectory;
  QTemporaryDir secondDirectory;
  QVERIFY(firstDirectory.isValid() && secondDirectory.isValid());

  const QByteArray firstBytes = identityGraph(kOnnxFloat16, std::nullopt, std::nullopt);
  const QByteArray secondBytes = identityGraphNamed(kOnnxFloat16, std::nullopt, std::nullopt,
                                                    QStringLiteral("A"), QStringLiteral("B"));
  QVERIFY(firstBytes != secondBytes);
  if (!dynamicReferenceRan(firstBytes)) QSKIP("the CPU provider refused the fp16 reference run");
  const QString firstSource = QDir(firstDirectory.path()).filePath(QStringLiteral("model.onnx"));
  const QString secondSource = QDir(secondDirectory.path()).filePath(QStringLiteral("model.onnx"));
  QVERIFY(writeFile(firstSource, firstBytes));
  QVERIFY(writeFile(secondSource, secondBytes));

  ModelInventory inventory(fixture.root());
  StoredModel first;
  StoredModel second;
  QVERIFY(publishGenerated(&inventory, firstSource, firstBytes, QStringLiteral("model.onnx"), &first));
  QVERIFY(
      publishGenerated(&inventory, secondSource, secondBytes, QStringLiteral("model.onnx"), &second));
  // Two files, one basename, two identities.
  QCOMPARE(first.record.displayName, second.record.displayName);
  QVERIFY(first.record.id != second.record.id);

  InspectedPlanResolver resolver(fixture.root(), ortRuntime());
  QVERIFY(resolver.refresh(nullptr, nullptr));
  QCOMPARE(resolver.catalog().models.size(), 2);

  QString key;
  QString reason;
  const std::optional<ResolvedProfile> atSmall =
      resolver.resolve(first.record, QSize(320, 180), &key, &reason);
  QVERIFY2(atSmall.has_value(), qPrintable(key + ": " + reason));
  QCOMPARE(atSmall->input, QSize(320, 180));
  QCOMPARE(atSmall->output, QSize(320, 180));

  const std::optional<ResolvedProfile> atLarge =
      resolver.resolve(second.record, QSize(640, 360), &key, &reason);
  QVERIFY2(atLarge.has_value(), qPrintable(key + ": " + reason));
  QCOMPARE(atLarge->input, QSize(640, 360));
  QCOMPARE(atLarge->output, QSize(640, 360));

  // A catalogue that hands the first identity the second graph's bytes is refused:
  // the basename decides nothing here, the identity does.
  ModelRecord forged = first.record;
  forged.onnxSha256 = second.record.onnxSha256;
  QVERIFY(!resolver.resolve(forged, QSize(320, 180), &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("model.owned.identity"));

  // And a record the store does not hold is refused by name, never resolved from
  // the nearest file on disk.
  ModelRecord absent = first.record;
  absent.id = QStringLiteral("user:sha256:") + QString(64, QLatin1Char('f'));
  QVERIFY(!resolver.resolve(absent, QSize(320, 180), &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("model.owned.missing"));
}

void TestNativeCatalog::aMovedOrRemovedSourceDoesNotStopTheOwnedResolution() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  QTemporaryDir sourceDirectory;
  QVERIFY(sourceDirectory.isValid());

  const QByteArray graph = identityGraph(kOnnxFloat16, std::nullopt, std::nullopt);
  if (!dynamicReferenceRan(graph)) QSKIP("the CPU provider refused the fp16 reference run");
  const QString sourcePath = QDir(sourceDirectory.path()).filePath(QStringLiteral("model.onnx"));
  QVERIFY(writeFile(sourcePath, graph));

  ModelInventory inventory(fixture.root());
  StoredModel stored;
  QVERIFY(publishGenerated(&inventory, sourcePath, graph, QStringLiteral("model.onnx"), &stored));

  InspectedPlanResolver resolver(fixture.root(), ortRuntime());
  QVERIFY(resolver.refresh(nullptr, nullptr));

  QString key;
  QString reason;
  const std::optional<ResolvedProfile> before =
      resolver.resolve(stored.record, QSize(320, 180), &key, &reason);
  QVERIFY2(before.has_value(), qPrintable(key + ": " + reason));
  QCOMPARE(before->output, QSize(320, 180));

  // The original is provenance: moving it away and deleting it must change
  // neither the owned row's identity nor the geometry its own bytes resolve.
  const QString movedPath = QDir(sourceDirectory.path()).filePath(QStringLiteral("moved.onnx"));
  QVERIFY(QFile::rename(sourcePath, movedPath));
  QVERIFY(QFile::remove(movedPath));

  QCOMPARE(resolver.rows().size(), 1);
  QCOMPARE(resolver.rows().first().record.id, stored.record.id);
  const std::optional<ResolvedProfile> afterSmall =
      resolver.resolve(stored.record, QSize(320, 180), &key, &reason);
  QVERIFY2(afterSmall.has_value(), qPrintable(key + ": " + reason));
  QCOMPARE(afterSmall->output, QSize(320, 180));
  // The same owned copy answers another stage geometry.
  const std::optional<ResolvedProfile> afterLarge =
      resolver.resolve(stored.record, QSize(640, 360), &key, &reason);
  QVERIFY2(afterLarge.has_value(), qPrintable(key + ": " + reason));
  QCOMPARE(afterLarge->output, QSize(640, 360));
}

void TestNativeCatalog::aChangedSourceReimportsAsADistinctRowAndTheOldOneStaysValid() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  QTemporaryDir sourceDirectory;
  QVERIFY(sourceDirectory.isValid());

  const QByteArray firstGraph = identityGraph(kOnnxFloat16, std::nullopt, std::nullopt);
  if (!dynamicReferenceRan(firstGraph)) QSKIP("the CPU provider refused the fp16 reference run");
  const QString sourcePath = QDir(sourceDirectory.path()).filePath(QStringLiteral("model.onnx"));
  QVERIFY(writeFile(sourcePath, firstGraph));

  ModelInventory inventory(fixture.root());
  StoredModel first;
  QVERIFY(publishGenerated(&inventory, sourcePath, firstGraph, QStringLiteral("model.onnx"), &first));

  InspectedPlanResolver resolver(fixture.root(), ortRuntime());
  QVERIFY(resolver.refresh(nullptr, nullptr));
  QString key;
  QString reason;
  QVERIFY(resolver.resolve(first.record, QSize(320, 180), &key, &reason).has_value());

  // The user edits the original and imports it again: the changed bytes are a
  // new identity, and the copy already imported keeps its own.
  const QByteArray secondGraph = identityGraphNamed(kOnnxFloat16, std::nullopt, std::nullopt,
                                                    QStringLiteral("A"), QStringLiteral("B"));
  QVERIFY(secondGraph != firstGraph);
  if (!dynamicReferenceRan(secondGraph)) QSKIP("the CPU provider refused the fp16 reference run");
  QVERIFY(writeFile(sourcePath, secondGraph));
  StoredModel second;
  QVERIFY(
      publishGenerated(&inventory, sourcePath, secondGraph, QStringLiteral("model.onnx"), &second));
  QVERIFY(second.record.id != first.record.id);

  QVERIFY(resolver.refresh(nullptr, nullptr));
  QCOMPARE(resolver.rows().size(), 2);
  // The old row still resolves from its own owned bytes, never from the edited
  // original that is now behind the new row.
  QVERIFY(resolver.resolve(first.record, QSize(320, 180), &key, &reason).has_value());
  QVERIFY(resolver.resolve(second.record, QSize(320, 180), &key, &reason).has_value());
}

void TestNativeCatalog::aCorruptOwnedCopyRefusesTheOwnedResolution() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  QTemporaryDir sourceDirectory;
  QVERIFY(sourceDirectory.isValid());

  const QByteArray graph = identityGraph(kOnnxFloat16, std::nullopt, std::nullopt);
  if (!dynamicReferenceRan(graph)) QSKIP("the CPU provider refused the fp16 reference run");
  const QString sourcePath = QDir(sourceDirectory.path()).filePath(QStringLiteral("model.onnx"));
  QVERIFY(writeFile(sourcePath, graph));

  ModelInventory inventory(fixture.root());
  StoredModel stored;
  QVERIFY(publishGenerated(&inventory, sourcePath, graph, QStringLiteral("model.onnx"), &stored));

  InspectedPlanResolver resolver(fixture.root(), ortRuntime());
  QVERIFY(resolver.refresh(nullptr, nullptr));

  QVERIFY(writeFile(stored.storedPath, graph + QByteArrayLiteral("tampered")));
  QString key;
  QString reason;
  QVERIFY(!resolver.resolve(stored.record, QSize(320, 180), &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("model.owned.digest"));
  QCOMPARE(resolver.lastReasonKey(), QStringLiteral("model.owned.digest"));
  QVERIFY(!resolver.lastReason().isEmpty());
}

void TestNativeCatalog::aMutatedOwnedCopyRefusesAfterItWasCached() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  QTemporaryDir sourceDirectory;
  QVERIFY(sourceDirectory.isValid());

  const QByteArray graph = identityGraph(kOnnxFloat16, std::nullopt, std::nullopt);
  if (!dynamicReferenceRan(graph)) QSKIP("the CPU provider refused the fp16 reference run");
  const QString sourcePath = QDir(sourceDirectory.path()).filePath(QStringLiteral("model.onnx"));
  QVERIFY(writeFile(sourcePath, graph));

  ModelInventory inventory(fixture.root());
  StoredModel stored;
  QVERIFY(publishGenerated(&inventory, sourcePath, graph, QStringLiteral("model.onnx"), &stored));

  InspectedPlanResolver resolver(fixture.root(), ortRuntime());
  QVERIFY(resolver.refresh(nullptr, nullptr));

  QString key;
  QString reason;
  // The first answer caches the owned bytes.
  QVERIFY(resolver.resolve(stored.record, QSize(320, 180), &key, &reason).has_value());
  QVERIFY(resolver.lastReasonKey().isEmpty());

  // Mutating the owned copy behind that cache must not keep serving the old
  // geometry: the copy is re-proved on disk before every answer.
  QVERIFY(writeFile(stored.storedPath, graph + QByteArrayLiteral("tampered")));
  QVERIFY(!resolver.resolve(stored.record, QSize(320, 180), &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("model.owned.changed"));
  QCOMPARE(resolver.lastReasonKey(), QStringLiteral("model.owned.changed"));
  QVERIFY(!resolver.lastReason().isEmpty());
}

void TestNativeCatalog::aRowWhoseBytesDeclareAnotherBoundaryIsRefused() {
  QString runtimeReason;
  if (!inspectionRuntimeIsPresent(ortRuntime(), &runtimeReason)) {
    QSKIP(qPrintable(QStringLiteral("no ONNX Runtime: %1").arg(runtimeReason)));
  }
  StoreFixture fixture;
  if (!fixture.isValid()) QSKIP("the app-owned models directory is not writable");
  QTemporaryDir sourceDirectory;
  QVERIFY(sourceDirectory.isValid());

  // The bytes declare dynamic H/W; the row claims a fixed 64x64 -> 128x128
  // boundary. The graph's own declaration is what the adapter trusts, so the row
  // is refused rather than resolved at a geometry nothing declares.
  const QByteArray graph = identityGraph(kOnnxFloat16, std::nullopt, std::nullopt);
  const QString sourcePath = QDir(sourceDirectory.path()).filePath(QStringLiteral("model.onnx"));
  QVERIFY(writeFile(sourcePath, graph));

  InspectionResult row;
  row.ok = true;
  row.inputName = QStringLiteral("X");
  row.outputName = QStringLiteral("Y");
  row.declared.input = tensor(QStringLiteral("fp16"), 64, 64);
  row.declared.output = tensor(QStringLiteral("fp16"), 128, 128);

  ModelInventory inventory(fixture.root());
  const ModelImportOutcome outcome =
      inventory.publish(graph, row, sourcePath, QStringLiteral("model.onnx"));
  QVERIFY2(outcome.ok, qPrintable(outcome.reasonKey + ": " + outcome.reason));
  QVERIFY(outcome.model.record.isOffered());

  InspectedPlanResolver resolver(fixture.root(), ortRuntime());
  QVERIFY(resolver.refresh(nullptr, nullptr));

  QString key;
  QString reason;
  QVERIFY(!resolver.resolve(outcome.model.record, QSize(64, 64), &key, &reason).has_value());
  QCOMPARE(key, QStringLiteral("model.owned.profile"));
  QCOMPARE(resolver.lastReasonKey(), QStringLiteral("model.owned.profile"));
  QVERIFY(!resolver.lastReason().isEmpty());
}
