#pragma once

#include "NativeBuilder.hpp"

#include "native/bundle/NativeBundle.hpp"
#include "native/bundle/NativeManifest.hpp"
#include "native/bundle/NativePaths.hpp"
#include "native/models/NativeModelInventory.hpp"
#include "native/models/NativeProfiles.hpp"
#include "native/models/NativeRecipe.hpp"
#include "native/prepare/NativeArtifactStore.hpp"
#include "native/prepare/NativeFrameReference.hpp"
#include "native/prepare/NativePreparationPlan.hpp"
#include "native/prepare/NativePreparationReferences.hpp"
#include "native/prepare/NativeWorkLease.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>

namespace mplayer {
namespace native {
namespace builder_detail {

constexpr int kSliceMs = 100;
constexpr int kStartSlices = 300;
constexpr int kStopSlices = 100;
constexpr qint64 kMaxToolBytes = 2097152;
constexpr qint64 kMaxTensorBytes = 536870912;
constexpr qint64 kMaxExtent = 65536;
constexpr const char* kEngineFileName = "engine.plan";
constexpr const char* kReferenceFileName = "reference.json";
constexpr const char* kOrtModule = "onnxruntime.dll";

BuilderResult refuse(const QString& key, const QString& text, int stage,
                     const QString& phase, const QString& toolError = {});
BuilderResult cancelledResult(const QString& phase, int stage = -1);
void emitProgress(const BuilderProgressCallback& callback, int stage, const QString& modelId,
                  const QString& phase, const QString& detail = {},
                  const QString& log = {});
bool isCancelled(NativeBuildCancellation* cancellation);
QString tailText(const QByteArray& bytes, int maxBytes = 4000);
void appendBounded(QByteArray* kept, const QByteArray& chunk);
bool parseObject(const QByteArray& bytes, QJsonObject* object, QString* error = nullptr);
bool shapeDims(const QString& text, qint64 (&dims)[4]);
QString dimsText(const QJsonArray& array);
int elementBytes(const QString& dtype);
bool plainPathNoReparse(const QString& canonical);
bool canonicalDir(const QString& path, QString* canonicalOut);
bool canonicalExe(const QString& path, QString* canonicalOut);
bool canonicalFileInside(const QString& rootCanonical, const QString& path,
                         QString* canonicalOut);

struct ToolPaths {
  QString inspect;
  QString prep;
  QString verify;
  QString ortDir;
  QString trtBin;
  QString cudaBin;
};

struct ToolResult {
  int exitCode = -1;
  QProcess::ExitStatus exitStatus = QProcess::CrashExit;
  QByteArray output;
  QByteArray error;
  QString launchError;
  /// `QProcess::errorString()` after the child ended. A loader failure exits
  /// abnormally with empty stderr, so this is the only observed cause left; a
  /// refusal that omitted it would read as a bare "the helper failed".
  QString processError;
  bool sawOutputLimit = false;
  bool stopped = false;
};

/// Names how a child tool ended, for a refusal whose own stderr tail is empty.
/// A missing runtime dependency terminates the process with an abnormal exit
/// code and no output; naming the code and status is what separates that from a
/// graph the tool refused in words.

QString toolExitCause(const ToolResult& run);
QString toolPathList(const ToolPaths& tools);
QProcessEnvironment childEnvironment(const ToolPaths& tools, bool forInspect);
bool runBlocking(const QString& program, const QStringList& arguments,
                 const QProcessEnvironment& environment,
                 NativeBuildCancellation* cancellation,
                 const BuilderProgressCallback& progress, int stageIndex,
                 const QString& modelId, const QString& phase, ToolResult* result);

struct RefTensor {
  QString name;
  QString dtype;
  QString shape;
  QString file;
  QString sha256;
  qint64 bytes = -1;
};

struct Pending {
  BuildRequest request;
  QString enginePath;
  QString engineSha;
  qint64 engineBytes = 0;
  BuilderPrepIdentity prep;
  QString onnxSha;
  qint64 onnxBytes = 0;
  ModelProfile declared;
  QString storedCanonical;
  int index = -1;
};

struct ExportedRef {
  QJsonObject manifest;
  QString dir;
  QVector<RefTensor> inputs;
  QVector<RefTensor> outputs;
};


bool tensorBytes(const QString& dtype, const qint64 (&dims)[4], qint64* bytesOut);
bool refTensors(const QJsonObject& manifest, const QString& refDir, const QString& key,
                QVector<RefTensor>* out, QString* reasonKey, QString* reason);

struct ResolvedTools {
  ToolPaths paths;
  QProcessEnvironment prepVerifyEnv;
  QProcessEnvironment inspectEnv;
};


bool resolveTools(const BuilderToolPaths& raw, ResolvedTools* out, QString* key,
                  QString* reason);
bool proveOwnedModel(const QString& modelRootCanonical, const StoredModel& model,
                     QString* storedCanonicalOut, FileDigest* digestOut, QString* key,
                     QString* reason);
bool reqString(const QJsonObject& object, const char* key, QString* out);
bool reqInt(const QJsonObject& object, const char* key, qint64* out);
bool reqUint(const QJsonObject& object, const char* key, quint64* out);
bool declaredDimsFromArray(const QJsonArray& array, qint64 (&dims)[4], bool allowDynamic);
bool stridesFromArray(const QJsonArray& array, qint64 (&strides)[4], qint64 maxStride);

struct MeasuredPrep {
  BuilderPrepIdentity flags;
  int deviceOrdinal = -1;
  QString deviceUuid;
  QString deviceName;
  QString computeCapability;
  quint64 totalGlobalMemory = 0;
  int multiProcessorCount = 0;
  int cudaRuntimeVersion = 0;
  int cudaDriverApiVersion = 0;
  QString cudaDriverFileVersion;
  int tensorRtLibVersion = 0;
  quint64 workspaceRequested = 0;
  QString workspaceSource;
  QString onnxSha;
  qint64 onnxBytes = 0;
  QString inName;
  qint64 inDims[4] = {0, 0, 0, 0};
  QString inType;
  QString outName;
  qint64 outDims[4] = {0, 0, 0, 0};
  QString outType;
  quint64 workspaceBytes = 0;
  QString rawEngineSha;
  qint64 rawEngineBytes = 0;
  QString rawOnnxSha;
  qint64 rawOnnxBytes = 0;
  QString rawDeviceUuid;
  QString rawComputeCapability;
  QString rawDriverFileVersion;
  int rawCudaRuntimeVersion = 0;
  int rawCudaDriverApiVersion = 0;
  int rawTensorRtLibVersion = 0;
  quint64 rawWorkspaceRequested = 0;
  quint64 rawWorkspaceBytes = 0;
  QString rawWorkspaceSource;
  QString rawShapeBinding;
  QString rawIoFormat;
  BuilderPrepIdentity rawFlags;
};


bool parseDeviceEvent(const QJsonObject& event, MeasuredPrep* measured, QString* key,
                      QString* reason);
bool parseGraphEvent(const QJsonObject& event, MeasuredPrep* measured, QString* key,
                     QString* reason);
bool parseStartedEvent(const QJsonObject& event, MeasuredPrep* measured, QString* key,
                       QString* reason);
bool parseRawEvent(const QJsonObject& event, MeasuredPrep* measured, QString* key,
                   QString* reason);
QVector<BuildOption> expectedOptions(const BuilderPrepIdentity& flags);
bool declaredOptimizationLevel(const EngineBuildIdentity& build, int* levelOut,
                               QString* key, QString* reason);
bool sameFlags(const BuilderPrepIdentity& a, const BuilderPrepIdentity& b);
bool trtLibFromRuntime(const QString& runtime, int* libOut);
bool cudaRuntimeFromCuda(const QString& cuda, int* runtimeOut);
bool checkMeasuredComplete(const MeasuredPrep& measured, QString* key, QString* reason);
bool checkMeasuredAgainstBuild(const MeasuredPrep& measured, const EngineBuildIdentity& build,
                               quint64 workspaceRequest, QString* key, QString* reason);
bool canonicalAnyFile(const QString& path, QString* canonicalOut);

struct Preflight {
  ResolvedTools tools;
  QString modelRootCanonical;
};


bool preflight(const BuilderRequest& request, Preflight* out, QString* key,
               QString* reason);
bool validateCallerRef(const BuilderStageReference& ref, const ResolvedStage& stage,
                       QString* key, QString* reason);
bool exportReference(const ResolvedTools& tools, const QString& storedCanonical,
                     const ResolvedStage& stage, const BuilderStageReference& ref,
                     const QString& refDir, int index, const QString& modelId,
                     NativeBuildCancellation* cancellation,
                     const BuilderProgressCallback& progress, ExportedRef* out,
                     BuilderResult* bad);

}  // namespace builder_detail
}  // namespace native
}  // namespace mplayer
