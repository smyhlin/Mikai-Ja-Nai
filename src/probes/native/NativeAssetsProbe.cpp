// M35's assets probe: a real window holding the actual NativeAssetsPanel, driven
// against real tiny ONNX graphs encoded in this file and the real app-owned
// store, cache and user-recipe store.
//
// What source reading cannot settle is whether an import really publishes
// exactly the bytes the user pointed at, whether a refused graph really publishes
// nothing, whether two files sharing a basename stay two rows, whether a cache
// deletion really refuses a held lease and leaves a foreign user file alone, and
// whether a verified model really becomes a selectable request without claiming
// the engine runs. So this drives the panel itself and reads the stores back.
//
// It needs an ONNX Runtime named by MIKAI_ORT_RUNTIME_DIR. When that is absent it
// skips with the runtime's own reason and exits 4 - it never fabricates metadata
// or a Ready row.

#include <cstdio>
#include <optional>

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QMetaObject>
#include <QObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>

#include "dialogs/NativeAssetsPanel.hpp"
#include "native/prepare/NativeArtifactStore.hpp"
#include "native/bundle/NativeManifest.hpp"
#include "native/models/NativeModelInventory.hpp"
#include "native/models/NativeModelInspect.hpp"
#include "native/bundle/NativePaths.hpp"
#include "native/prepare/NativePreparationPlan.hpp"
#include "native/bundle/NativePrefs.hpp"
#include "ProbeIsolation.hpp"
#include "ProbeUi.hpp"
#include "workers/Runner.hpp"

namespace {

using mikai::probes::isolation::enter;
using mikai::probes::isolation::exitCode;
using mikai::probes::isolation::Gate;
using mikai::probes::isolation::isOwnedScratch;
using mikai::probes::isolation::Options;

using namespace mplayer;
using namespace mplayer::native;
using mikai::app::NativeAssetsPanel;
using mikai::app::NativeCacheEntry;
using mikai::app::NativeModelView;

int failures = 0;

void check(bool ok, const QString& what) {
    std::fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) ++failures;
}

void say(const QString& text) { std::fprintf(stderr, "%s\n", qPrintable(text)); }

bool writeBytes(const QString& path, const QByteArray& bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

void shoot(QWidget* widget, const QString& name) {
    const QString path = QDir::current().filePath(name);
    const bool written =
        widget != nullptr && QDir().mkpath(QFileInfo(path).absolutePath()) &&
        widget->grab().save(path);
    check(written, QStringLiteral("screenshot written: %1").arg(name));
}

// ----------------------------------------------------- ONNX fixture encoding
// A minimal protobuf writer: only varint (0) and length-delimited (2) fields, the
// two wire types these fixtures need. An empty TensorShapeProto.Dimension is how
// ONNX spells a dynamic dimension - never a zero.

constexpr int kOnnxFloat = 1;
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
    QByteArray dim;
    putKey(&dim, 1, 2);
    putVarint(&dim, 0);
    return dim;
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

QByteArray node(const QString& opType, const QStringList& inputs, const QStringList& outputs,
                const QString& name) {
    QByteArray value;
    for (const QString& input : inputs) putStringField(&value, 1, input);
    for (const QString& output : outputs) putStringField(&value, 2, output);
    putStringField(&value, 3, name);
    putStringField(&value, 4, opType);
    return value;
}

QByteArray onnxModel(const QVector<QByteArray>& nodes, const QVector<QByteArray>& inputs,
                     const QVector<QByteArray>& outputs, int opset = 17) {
    QByteArray graph;
    for (const QByteArray& n : nodes) putBytesField(&graph, 1, n);
    putStringField(&graph, 2, QStringLiteral("mikai.fixture"));
    for (const QByteArray& input : inputs) putBytesField(&graph, 11, input);
    for (const QByteArray& output : outputs) putBytesField(&graph, 12, output);

    QByteArray opsetImport;
    putVarintField(&opsetImport, 2, static_cast<quint64>(opset));

    QByteArray model;
    putVarintField(&model, 1, 8);
    putStringField(&model, 2, QStringLiteral("mikai.probe"));
    putBytesField(&model, 7, graph);
    putBytesField(&model, 8, opsetImport);
    return model;
}

QByteArray identityGraph(int elementType, const std::optional<qint64>& height,
                         const std::optional<qint64>& width, const QString& inputName,
                         const QString& outputName) {
    const QVector<std::optional<qint64>> dims = {1, 3, height, width};
    return onnxModel({node(QStringLiteral("Identity"), {inputName}, {outputName},
                           QStringLiteral("identity"))},
                     {valueInfo(inputName, elementType, dims)},
                     {valueInfo(outputName, elementType, dims)});
}

QByteArray twoInputGraph() {
    const QVector<std::optional<qint64>> dims = {1, 3, std::nullopt, std::nullopt};
    return onnxModel({node(QStringLiteral("Add"), {QStringLiteral("A"), QStringLiteral("B")},
                           {QStringLiteral("Y")}, QStringLiteral("add"))},
                     {valueInfo(QStringLiteral("A"), kOnnxFloat16, dims),
                      valueInfo(QStringLiteral("B"), kOnnxFloat16, dims)},
                     {valueInfo(QStringLiteral("Y"), kOnnxFloat16, dims)});
}

// ------------------------------------------------------------- import driver

/// Runs one import through the panel's real asynchronous path and returns the
/// store's own verdict. `submitted` is false when the panel refused to even
/// submit, in which case no completion is expected.
bool runImport(NativeAssetsPanel* panel, const QString& path, const QSize& geometry,
               bool* submitted = nullptr) {
    bool ok = false;
    bool done = false;
    QEventLoop loop;
    QMetaObject::Connection connection = QObject::connect(
        panel, &NativeAssetsPanel::importFinished, &loop, [&](bool result) {
            ok = result;
            done = true;
            loop.quit();
        });
    const bool submittedNow = panel->importModel(path, geometry);
    if (submitted != nullptr) *submitted = submittedNow;
    if (submittedNow) {
        QTimer::singleShot(300000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    QObject::disconnect(connection);
    if (submittedNow && !done) {
        check(false, QStringLiteral("the import settled within the budget: %1").arg(path));
    }
    return ok && done;
}

int indexOfModel(const NativeAssetsPanel& panel, const QString& id) {
    const QVector<NativeModelView>& models = panel.models();
    for (int i = 0; i < models.size(); ++i) {
        if (models.at(i).id == id) return i;
    }
    return -1;
}

// ----------------------------------------------------- prepared-artifact fixture

const QString kCoreSha = QStringLiteral("63d127174d8f70336c82a22408929e2bc301edcb");
const QString kCreatedAt = QStringLiteral("2026-10-01T00:00:00Z");

QString sha256Hex(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

ModelProfile fixtureProfile() {
    ModelProfile profile;
    profile.input.dtype = QString::fromLatin1(kEligibleIoType);
    profile.input.rank = 4;
    profile.input.batch = 1;
    profile.input.channels = 3;
    profile.input.height = 64;
    profile.input.width = 64;
    profile.output = profile.input;
    return profile;
}

/// Publishes one real artifact into `store` so a lease can be taken on it. The
/// manifest is the fixture contract TestNativeManifest already exercises.
QString publishFixtureArtifact(const ArtifactStore& store, const QString& scratchDir) {
    const QByteArray graphBytes = QByteArrayLiteral("probe-graph-bytes");
    const QByteArray engineBytes = QByteArray(4096, char(0x42));

    GraphContentIdentity graph;
    graph.role = ModelRole::Spatial;
    graph.graphSha256 = sha256Hex(graphBytes);
    graph.declaredProfile = fixtureProfile();

    EngineBuildIdentity build;
    build.provider = Backend::TensorRT;
    build.runtimeVersion = QStringLiteral("11.3.0.99");
    build.pluginVersion = QStringLiteral("1.2.3");
    build.coreSha = kCoreSha;
    build.sourceAbi = 8;
    build.precision = EnginePrecision::Fp16;
    build.tf32 = false;
    build.stronglyTyped = true;
    build.skipInference = true;
    build.deviceUuid = QStringLiteral("GPU-9f2c1a44-0b7d-4e21-9c3a-1d5e");
    build.smMajor = 12;
    build.smMinor = 0;
    build.cudaVersion = QStringLiteral("13.4.92");
    build.driverVersion = QStringLiteral("616.92");
    build.options = QVector<BuildOption>{
        BuildOption{QStringLiteral("stronglyTyped"), QString(), false},
        BuildOption{QStringLiteral("skipInference"), QString(), false},
        BuildOption{QStringLiteral("tacticSources"), QStringLiteral("-CUDNN,-CUBLAS,-CUBLAS_LT"), true},
        BuildOption{QStringLiteral("optShapes"), QStringLiteral("1x3x64x64,1x3x64x64"), true}};

    StageBuildRequest stage;
    stage.role = ModelRole::Spatial;
    stage.inputShape = QStringLiteral("1x3x64x64");
    stage.outputShape = QStringLiteral("1x3x64x64");
    stage.ioFormat = QStringLiteral("fp16:chw");

    BuildRequest request;
    request.graph = graph;
    request.stage = stage;
    request.build = build;

    ArtifactManifest manifest;
    manifest.schema = artifactManifestSchema();
    manifest.mikaiFormat = 1;
    manifest.buildKey = request.buildKey();
    manifest.engineSha256 = sha256Hex(engineBytes);
    manifest.source.modelContentId = request.graph.contentId();
    manifest.source.onnxSha256 = request.graph.graphSha256;
    manifest.source.onnxBytes = graphBytes.size();
    manifest.source.declaredProfile = graph.declaredProfile;
    manifest.stage.role = stage.role;
    manifest.stage.inputShape = stage.inputShape;
    manifest.stage.outputShape = stage.outputShape;
    manifest.stage.ioFormat = stage.ioFormat;
    manifest.stage.padLeft = stage.padLeft;
    manifest.stage.padTop = stage.padTop;
    manifest.build.provider = build.provider;
    manifest.build.runtime = build.runtimeVersion;
    manifest.build.plugin = build.pluginVersion;
    manifest.build.coreSha = build.coreSha;
    manifest.build.sourceAbi = build.sourceAbi;
    manifest.build.precision = build.precision;
    manifest.build.tf32 = build.tf32;
    manifest.build.stronglyTyped = build.stronglyTyped;
    manifest.build.skipInference = build.skipInference;
    manifest.build.canonicalOptions = build.canonicalOptions();
    manifest.device.uuid = build.deviceUuid;
    manifest.device.sm = build.smText();
    manifest.device.driver = build.driverVersion;
    manifest.device.cuda = build.cudaVersion;
    manifest.device.luid = build.adapterLuid;
    manifest.engine.file = QStringLiteral("engine.plan");
    manifest.engine.bytes = engineBytes.size();
    manifest.engine.sha256 = manifest.engineSha256;
    manifest.createdAt = kCreatedAt;

    const QString scratchEngine = QDir(scratchDir).filePath(QStringLiteral("produced.engine"));
    if (!writeBytes(scratchEngine, engineBytes)) return {};
    QString reasonKey;
    QString reason;
    const std::optional<ArtifactRecord> published =
        publishArtifact(store, manifest, scratchEngine, &reasonKey, &reason);
    if (!published.has_value()) {
        say(QStringLiteral("artifact fixture refused: %1: %2").arg(reasonKey, reason));
        return {};
    }
    return manifest.buildKey;
}

}  // namespace

int main(int argc, char** argv) {
    // Real executable-local isolation, decided before anything reads
    // `nativeRoot()`: this probe imports into and wipes the app-owned model store,
    // so it runs on a staged copy of its own executable whose `native` tree is
    // owned scratch. Nothing beside that directory is written, moved or removed.
    Options isolation;
    isolation.probe = "native_assets_probe";
    const Gate gate = enter(argc, argv, isolation);
    if (gate != Gate::RunHere) return exitCode();

    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    if (!mikai::probes::initializeUi(app)) return 2;
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiNativeAssetsProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("NativeAssetsProbe"));

    const InspectionRuntime runtime = inspectionRuntimeAt(qEnvironmentVariable("MIKAI_ORT_RUNTIME_DIR"));
    QString runtimeReason;
    if (!inspectionRuntimeIsPresent(runtime, &runtimeReason)) {
        std::fprintf(stderr, "[native_assets_probe] SKIP: ONNX Runtime unavailable: %s\n",
                     qPrintable(runtimeReason));
        return 4;
    }

    // A clean app-owned tree for this probe only: the isolated executable's
    // `nativeRoot()` IS the scratch this process owns, so clearing it cannot
    // reach a real install beside the original build output.
    if (!nativeRoot().isEmpty() && isOwnedScratch(nativeRoot())) {
        QDir(nativeRoot()).removeRecursively();
    }

    QTemporaryDir sources;
    if (!sources.isValid()) {
        std::fprintf(stderr, "[native_assets_probe] FAIL: no temporary source directory\n");
        return 1;
    }

    const QByteArray fixedBytes = identityGraph(kOnnxFloat16, 64, 64, QStringLiteral("X"),
                                                QStringLiteral("Y"));
    const QByteArray sameA = identityGraph(kOnnxFloat16, 64, 64, QStringLiteral("A"),
                                           QStringLiteral("A_out"));
    const QByteArray sameB = identityGraph(kOnnxFloat16, 64, 64, QStringLiteral("B"),
                                           QStringLiteral("B_out"));
    const QByteArray fp32Bytes = identityGraph(kOnnxFloat, 64, 64, QStringLiteral("X"),
                                               QStringLiteral("Y"));
    const QByteArray dynamicBytes = identityGraph(kOnnxFloat16, std::nullopt, std::nullopt,
                                                  QStringLiteral("X"), QStringLiteral("Y"));
    const QByteArray refusedBytes = twoInputGraph();

    const QString fixedPath = QDir(sources.path()).filePath(QStringLiteral("fixed.onnx"));
    const QString dirAPath = QDir(sources.path()).filePath(QStringLiteral("a/anime.onnx"));
    const QString dirBPath = QDir(sources.path()).filePath(QStringLiteral("b/anime.onnx"));
    const QString fp32Path = QDir(sources.path()).filePath(QStringLiteral("fp32.onnx"));
    const QString dynamicPath = QDir(sources.path()).filePath(QStringLiteral("dynamic.onnx"));
    const QString refusedPath = QDir(sources.path()).filePath(QStringLiteral("refused.onnx"));
    const bool seeded = writeBytes(fixedPath, fixedBytes) && writeBytes(dirAPath, sameA) &&
                        writeBytes(dirBPath, sameB) && writeBytes(fp32Path, fp32Bytes) &&
                        writeBytes(dynamicPath, dynamicBytes) &&
                        writeBytes(refusedPath, refusedBytes);
    check(seeded, QStringLiteral("the tiny ONNX fixtures are written"));

    const QString artifactsRoot = ArtifactStore().root();
    check(QDir().mkpath(artifactsRoot), QStringLiteral("the artifacts root exists for the probe"));
    const QString userSentinel = QDir(artifactsRoot).filePath(QStringLiteral("user-note.bin"));
    const QByteArray sentinel = QByteArrayLiteral("USER-FILE-DO-NOT-DELETE");
    check(writeBytes(userSentinel, sentinel), QStringLiteral("a foreign user file sits in the cache"));
    // An artifact-shaped directory that does not verify: listed, never deletable.
    const QString unproven = QDir(artifactsRoot).filePath(QStringLiteral("artifact-") +
                                                          QString(64, QLatin1Char('c')));
    check(QDir().mkpath(unproven), QStringLiteral("an unverifiable cache directory exists"));

    mikai::app::Runner runner;
    QWidget window;
    window.setWindowTitle(QStringLiteral("Native assets"));
    auto* layout = new QVBoxLayout(&window);
    auto* panel = new NativeAssetsPanel(&window);
    panel->setObjectName(QStringLiteral("nativeAssetsPanel"));
    panel->setRunner(&runner);
    layout->addWidget(panel);
    window.resize(mikai::probes::windowSize(QSize(940, 760)));
    window.show();
    QApplication::processEvents();
    shoot(&window, QStringLiteral("native-assets-probe-empty.png"));

    // ── 1. a verified import publishes exactly the user's bytes ─────────────
    const bool fixedOk = runImport(panel, fixedPath, QSize(64, 64));
    check(fixedOk, QStringLiteral("the fixed fp16 graph imports and is offered"));
    QString fixedId;
    {
        const QVector<StoredModel> rows = ModelInventory().stored(nullptr);
        for (const StoredModel& row : rows) {
            if (row.record.onnxBytes == fixedBytes.size() &&
                row.record.actualProfile.input.dtype == QLatin1String(kEligibleIoType)) {
                fixedId = row.record.id;
                check(readBytes(row.storedPath) == fixedBytes,
                      QStringLiteral("the owned copy is byte-identical to the user's file"));
                check(QFileInfo(row.storedPath).canonicalFilePath().startsWith(
                          QFileInfo(ModelInventory::defaultRoot()).canonicalFilePath()),
                      QStringLiteral("the owned copy lives inside the app-owned store"));
            }
        }
    }
    check(!fixedId.isEmpty(), QStringLiteral("the imported row is in the inventory"));
    {
        const int index = indexOfModel(*panel, fixedId);
        check(index >= 0 && panel->models().at(index).dtype == QLatin1String(kEligibleIoType) &&
                  panel->models().at(index).usable,
              QStringLiteral("the inventory row carries dtype, size and a Ready status"));
        if (index >= 0) {
            const NativeModelView& view = panel->models().at(index);
            check(view.bytes == fixedBytes.size(),
                  QStringLiteral("the row's size is the graph's byte count"));
            check(!view.contentId.isEmpty() && view.contentId.startsWith(QStringLiteral("sha256:")),
                  QStringLiteral("the row carries its content identity"));
        }
    }

    // ── 2. a non-fp16 graph is recorded and unusable, never converted ───────
    bool fp32Submitted = false;
    const bool fp32Ok = runImport(panel, fp32Path, QSize(64, 64), &fp32Submitted);
    check(fp32Submitted, QStringLiteral("the fp32 graph is submitted"));
    check(!fp32Ok, QStringLiteral("the fp32 graph is not offered"));
    {
        bool foundFp32 = false;
        for (const NativeModelView& view : panel->models()) {
            if (view.dtype == QStringLiteral("fp32")) {
                foundFp32 = true;
                check(!view.usable, QStringLiteral("the fp32 row is not selectable"));
                check(view.statusKey == QStringLiteral("model.io.unsupported"),
                      QStringLiteral("the fp32 row names the observed dtype as the reason"));
            }
        }
        check(foundFp32, QStringLiteral("the fp32 diagnostic row is published"));
    }

    // ── 3. two files sharing a basename stay two rows ──────────────────────
    const bool sameAOk = runImport(panel, dirAPath, QSize(64, 64));
    const bool sameBOk = runImport(panel, dirBPath, QSize(64, 64));
    check(sameAOk && sameBOk, QStringLiteral("both same-basename graphs import"));
    {
        int animeRows = 0;
        QSet<QString> ids;
        for (const NativeModelView& view : panel->models()) {
            if (view.label == QStringLiteral("anime")) {
                ++animeRows;
                ids.insert(view.contentId);
            }
        }
        check(animeRows == 2, QStringLiteral("two rows carry the shared basename"));
        check(ids.size() == 2, QStringLiteral("the two rows have distinct content identities"));
    }

    // ── 4. a refused graph publishes nothing ───────────────────────────────
    const int beforeRefusal = panel->models().size();
    bool refusedSubmitted = false;
    const bool refusedOk = runImport(panel, refusedPath, QSize(64, 64), &refusedSubmitted);
    check(refusedSubmitted, QStringLiteral("the two-input graph is submitted"));
    check(!refusedOk, QStringLiteral("the two-input graph is refused"));
    check(panel->models().size() == beforeRefusal,
          QStringLiteral("a refused graph leaves the inventory exactly as it was"));
    check(panel->statusKey() == QStringLiteral("inspect.graph.ioCount"),
          QStringLiteral("the refusal names the actual input/output count"));

    // ── 5. a dynamic graph resolves at the geometry the user gave ──────────
    bool dynamicSubmitted = false;
    runImport(panel, dynamicPath, QSize(48, 48), &dynamicSubmitted);
    check(dynamicSubmitted, QStringLiteral("the dynamic graph is submitted at 48x48"));
    {
        for (const StoredModel& row : ModelInventory().stored(nullptr)) {
            if (row.record.availability.isUsable() && row.importResolution.has_value()) {
                check(row.importResolution->input == QSize(48, 48) &&
                          row.importResolution->scale == 1,
                      QStringLiteral("the dynamic row resolved at the user's geometry, not 2x"));
            } else if (row.record.actualProfile.isDynamicGeometry()) {
                check(row.record.availability.reasonKey ==
                              QStringLiteral("model.reference.failed") ||
                          row.record.availability.reasonKey ==
                              QStringLiteral("model.reference.uniform"),
                      QStringLiteral("an unresolved dynamic row names its reference reason"));
            }
        }
    }

    // ── 6. a verified model becomes a selectable request, with no engine claim
    QString requested;
    if (!fixedId.isEmpty()) {
        QMetaObject::Connection connection = QObject::connect(
            panel, &NativeAssetsPanel::recipeRequested, &app,
            [&requested](const QString& id) { requested = id; });
        const bool registered = panel->selectModelAsRecipe(fixedId);
        QObject::disconnect(connection);
        check(registered, QStringLiteral("the verified model registers as a single-spatial recipe"));
        check(!requested.isEmpty(), QStringLiteral("the panel hands the recipe id back"));
        // This probe holds no live source context, so no factory observation is
        // passed: the registered recipe is offered as a request, not a capability.
        const NativeAvailability available = currentNativeAvailability(nullptr);
        const NativeRecipeOption* option = available.findRecipe(requested);
        check(option != nullptr, QStringLiteral("the registered recipe is offered as a request"));
        check(option != nullptr && !option->availability.isUsable(),
              QStringLiteral("the request is offered unavailable, not as a fake capability"));
        check(!available.engineAvailable,
              QStringLiteral("no engine is claimed just because a request exists"));
    }

    // ── 7. the cache lists only proven artifacts and deletes only those ────
    panel->refresh();
    {
        bool listedUnproven = false;
        QString unprovenKey;
        for (const NativeCacheEntry& entry : panel->cache()) {
            if (entry.directory == unproven) {
                listedUnproven = true;
                check(!entry.proven, QStringLiteral("an unverifiable directory is not proven"));
                unprovenKey = entry.buildKey;
            }
        }
        check(listedUnproven, QStringLiteral("the unverifiable directory is still visible"));
        // Deleting refreshes the cache, so the key is taken first and the call is
        // made after the enumeration is done.
        if (!unprovenKey.isEmpty()) {
            check(!panel->deleteCacheEntry(unprovenKey),
                  QStringLiteral("an unverified cache entry is never deleted"));
        }
    }

    const QString scratchDir = QDir(sources.path()).filePath(QStringLiteral("scratch"));
    const QString buildKey = publishFixtureArtifact(ArtifactStore(), scratchDir);
    check(!buildKey.isEmpty(), QStringLiteral("a real artifact publishes into the cache"));
    panel->refresh();

    if (!buildKey.isEmpty()) {
        const std::optional<ArtifactLease> lease =
            acquireArtifactLease(ArtifactStore(), buildKey, nullptr, nullptr);
        check(lease.has_value(), QStringLiteral("a lease is taken on the published artifact"));
        QString key;
        QString reason;
        const bool removedWhileLeased = panel->deleteCacheEntry(buildKey);
        check(!removedWhileLeased, QStringLiteral("deletion refuses while a lease is alive"));
        check(panel->statusKey() == QStringLiteral("native.artifact.inUse"),
              QStringLiteral("the refusal names the live lease"));
        check(readArtifact(ArtifactStore(), buildKey, nullptr, nullptr).has_value(),
              QStringLiteral("the leased artifact is still there"));
    }

    // The lease is out of scope now; the same entry may be removed.
    if (!buildKey.isEmpty()) {
        check(panel->deleteCacheEntry(buildKey), QStringLiteral("the artifact deletes once free"));
        check(!readArtifact(ArtifactStore(), buildKey, nullptr, nullptr).has_value(),
              QStringLiteral("the deleted artifact is gone"));
    }

    check(readBytes(userSentinel) == sentinel,
          QStringLiteral("a foreign user file in the cache is never touched"));
    check(QFileInfo(unproven).isDir(), QStringLiteral("the unverifiable directory survives"));

    if (app.arguments().size() > 1) {
        const QString graphPath = QFileInfo(app.arguments().at(1)).absoluteFilePath();
        const QByteArray sourceBytes = readBytes(graphPath);
        const QString sourceDigest = sha256HexOf(sourceBytes);
        const bool imported = !sourceBytes.isEmpty() &&
                              runImport(panel, graphPath, QSize(48, 48));
        check(imported, QStringLiteral("the original graph imports through the visible panel"));
        bool proved = false;
        for (const StoredModel& model : ModelInventory().stored(nullptr)) {
            if (model.record.onnxSha256 != sourceDigest) continue;
            proved = model.importResolution.has_value() &&
                     model.importResolution->input == QSize(48, 48) &&
                     model.importResolution->output == QSize(96, 96) &&
                     model.record.availability.isUsable() &&
                     readBytes(model.storedPath) == sourceBytes;
            const int row = indexOfModel(*panel, model.record.id);
            check(row >= 0 && panel->models().at(row).usable,
                  QStringLiteral("the original graph appears as an eligible inventory row"));
            say(QStringLiteral("original graph: %1; learned geometry 48x48 -> %2x%3")
                    .arg(model.record.id)
                    .arg(model.importResolution.has_value()
                             ? model.importResolution->output.width() : 0)
                    .arg(model.importResolution.has_value()
                             ? model.importResolution->output.height() : 0));
        }
        check(proved, QStringLiteral("the original graph's CPU run proves scale 2 and preserves its bytes"));
        check(readBytes(graphPath) == sourceBytes,
              QStringLiteral("the original source graph remains unchanged after GUI import"));
    }

    // ── 8. preparation readiness is named and honest, never a licence banner ──
    {
        // The recipe registered above is the one this panel would prepare. With
        // no observed source context that is the first missing prerequisite, and
        // it is named rather than replaced by a blanket "unavailable".
        if (!requested.isEmpty()) {
            panel->setRequestedRecipe(requested);
            check(panel->preparationReadinessKey() == QStringLiteral("native.prepare.source"),
                  QStringLiteral("without an observed source context, preparation names the "
                                 "source"));
        } else {
            check(panel->preparationReadinessKey() == QStringLiteral("native.prepare.recipe"),
                  QStringLiteral("without a selected recipe, preparation names the recipe"));
        }
        const int cacheBefore = panel->cache().size();
        const bool submitted = panel->prepareArtifact();
        check(!submitted,
              QStringLiteral("preparation is not submitted while a prerequisite is missing"));
        check(!panel->prepareStatusKey().isEmpty(),
              QStringLiteral("the refusal carries a stable machine key"));
        check(!panel->prepareInFlight(), QStringLiteral("no preparation job is left in flight"));
        panel->refresh();
        check(panel->cache().size() == cacheBefore,
              QStringLiteral("a refused preparation publishes nothing into the cache"));

        // A source context is only the first prerequisite: the toolchain and the
        // whole-plan reference builder are separate observed facts, and whichever
        // is missing next is named.
        // An actual source a probe and a capture can run against, and the
        // observed context when the core already published one.
        panel->setPreparationSourcePresence([](QString*, QString*) { return true; });
        panel->setPreparationContextProvider([] {
            NativePreparationContext context;
            context.source.codedSize = QSize(64, 64);
            context.source.fps = Rational{24, 1};
            context.device.decoderDeviceUuid = QStringLiteral("GPU-probe-decoder");
            context.device.computeDeviceUuid = QStringLiteral("GPU-probe-compute");
            context.device.smMajor = 12;
            context.device.smMinor = 0;
            context.modulePath = QStringLiteral("probe-module");
            return std::optional<NativePreparationContext>(context);
        });
        const QString afterContext = panel->preparationReadinessKey();
        check(afterContext != QStringLiteral("native.prepare.source"),
              QStringLiteral("an observed source context clears the source prerequisite"));
        check(afterContext.isEmpty() ||
                  afterContext == QStringLiteral("native.prepare.toolchain") ||
                  afterContext == QStringLiteral("native.prepare.toolsUnavailable") ||
                  afterContext == QStringLiteral("native.prepare.backendUnavailable") ||
                  afterContext == QStringLiteral("native.prepare.capture") ||
                  afterContext == QStringLiteral("native.prepare.context"),
              QStringLiteral("the next missing prerequisite is named, never a fake Ready"));
        // No frame or reference builder is wired here, so the panel can never be
        // fully ready and must still refuse by name.
        check(!afterContext.isEmpty(),
              QStringLiteral("a panel without the reference builder is not ready"));
        check(!panel->prepareArtifact(),
              QStringLiteral("a partially-ready panel still refuses to submit"));
        check(!panel->prepareStatusKey().isEmpty(),
              QStringLiteral("the partial refusal also carries a stable key"));
    }

    shoot(&window, QStringLiteral("native-assets-probe.png"));

    // Cleanup is scoped to the directory this probe process owns; the parent
    // removes its own scratch. A tree this process did not create is never touched.
    if (!nativeRoot().isEmpty() && isOwnedScratch(nativeRoot())) {
        QDir(nativeRoot()).removeRecursively();
    }
    std::fprintf(stderr, "[native_assets_probe] %s (%d failures)\n",
                 failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
