// The one translation unit that knows the ONNX Runtime C API.
//
// It loads an explicitly named `onnxruntime.dll` with a restricted search (its
// own directory, System32 and explicitly registered directories - never the
// process PATH, never the current directory), reads the declared boundary of a
// graph from memory, and - for a dynamic graph whose dtype the FP16 bridge can
// actually bind - runs one CPU reference at the caller's stage geometry to learn
// the shape the model really produces.
//
// Three things this file never does.
//
//  * It never decides what a graph is from its name. The profile comes from
//    `SessionGetInputTypeInfo`/`SessionGetOutputTypeInfo`, and the scale comes
//    from declared or CPU-produced shapes.
//  * It never converts, re-exports or promotes a dtype. A non-FP16 graph is
//    reported as it is; the inventory records it unsupported.
//  * It never fabricates a geometry or a scale. A dimension the graph left
//    dynamic stays absent, and a run that did not complete fills a reason
//    instead of a number.

#include "NativeModelInspect.hpp"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <onnxruntime_c_api.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "native/bundle/NativePaths.hpp"

namespace mplayer {
namespace native {

namespace {

// The reference run allocates a whole fp16 NCHW input. 8K is the largest stage
// geometry this track is expected to resolve; the bound stops a caller-supplied
// size from turning a probe into an out-of-memory fault.
constexpr qint64 kMaxReferenceInputBytes = 512ll * 1024 * 1024;

// The largest H or W a reference may be fed or may report. It is a superset of
// the biggest stage this track resolves, and it also keeps the fp16 byte
// arithmetic below from overflowing before it can be compared with the bound.
constexpr qint64 kMaxReferenceSide = 32768;

#ifdef _WIN32
QString win32Text(DWORD code) {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                      reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  QString text;
  if (buffer != nullptr && length != 0) {
    text = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
  }
  if (buffer != nullptr) LocalFree(buffer);
  if (text.isEmpty()) text = QStringLiteral("unknown Windows error");
  return QStringLiteral("%1 (Win32 %2)").arg(text).arg(code);
}
#endif

/// Releases an ORT status and returns its message. Every ORT call whose result
/// this file does not otherwise consume goes through here, so no status leaks.
QString takeStatus(const OrtApi* api, OrtStatus* status) {
  if (status == nullptr) return QString();
  const QString text = QString::fromUtf8(api->GetErrorMessage(status));
  api->ReleaseStatus(status);
  return text;
}

/// A scope guard for the ORT handles, so an early return cannot leak one.
template <typename T, typename Fn>
struct Scoped {
  T* ptr;
  Fn release;
  Scoped(T* p, Fn fn) : ptr(p), release(fn) {}
  Scoped(const Scoped&) = delete;
  Scoped& operator=(const Scoped&) = delete;
  ~Scoped() {
    if (ptr != nullptr) release(ptr);
  }
};

template <typename T, typename Fn>
Scoped<T, Fn> scoped(T* ptr, Fn release) {
  return Scoped<T, Fn>(ptr, release);
}

QString dtypeName(ONNXTensorElementDataType type) {
  switch (type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
      return QStringLiteral("fp32");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
      return QStringLiteral("fp16");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16:
      return QStringLiteral("bf16");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:
      return QStringLiteral("fp64");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
      return QStringLiteral("int8");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
      return QStringLiteral("uint8");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:
      return QStringLiteral("int16");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16:
      return QStringLiteral("uint16");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
      return QStringLiteral("int32");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32:
      return QStringLiteral("uint32");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
      return QStringLiteral("int64");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64:
      return QStringLiteral("uint64");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:
      return QStringLiteral("bool");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING:
      return QStringLiteral("string");
    default:
      break;
  }
  // A dtype this build has no name for keeps its ONNX code rather than a guess.
  return QStringLiteral("onnx:%1").arg(static_cast<int>(type));
}

// ------------------------------------------------------------------ runtime

struct LoadedRuntime {
  const OrtApi* api = nullptr;
  OrtEnv* env = nullptr;
  QString canonicalModule;
  QString version;
};

QMutex g_runtimeMutex;
LoadedRuntime g_runtime;

struct RuntimeHandle {
  const OrtApi* api = nullptr;
  OrtEnv* env = nullptr;
  QString errorKey;
  QString error;
};

/// Resolves and loads the runtime, or returns the named reason it is not
/// available. A successful load is kept for the process lifetime: unloading
/// ORT underneath live sessions is not a thing to do.
RuntimeHandle acquireRuntime(const InspectionRuntime& runtime) {
  QMutexLocker locker(&g_runtimeMutex);

  RuntimeHandle handle;
  const auto refuse = [&handle](const QString& key, const QString& text) {
    handle.errorKey = key;
    handle.error = text;
    handle.api = nullptr;
    return handle;
  };

  if (g_runtime.api != nullptr) {
    if (runtime.isSet() && !runtime.modulePath.isEmpty()) {
      const QString canonical = QFileInfo(QDir::cleanPath(runtime.modulePath)).canonicalFilePath();
      if (!canonical.isEmpty() &&
          canonical.compare(g_runtime.canonicalModule, Qt::CaseInsensitive) != 0) {
        return refuse(QStringLiteral("inspect.runtime.locked"),
                      QStringLiteral("ONNX Runtime is already loaded from %1; a second module "
                                     "(%2) cannot be loaded in the same process")
                          .arg(g_runtime.canonicalModule, canonical));
      }
    }
    handle.api = g_runtime.api;
    handle.env = g_runtime.env;
    return handle;
  }

  if (!runtime.isSet()) {
    return refuse(QStringLiteral("inspect.runtime.unavailable"),
                  QStringLiteral("no ONNX Runtime directory was named; inspection needs an "
                                 "explicitly located onnxruntime.dll (--runtime or "
                                 "MIKAI_ORT_RUNTIME_DIR), never a PATH lookup"));
  }

  const QFileInfo directoryInfo(QDir::cleanPath(runtime.directory));
  if (!directoryInfo.isAbsolute()) {
    return refuse(QStringLiteral("inspect.runtime.absolute"),
                  QStringLiteral("the ONNX Runtime directory '%1' is not absolute")
                      .arg(runtime.directory));
  }
  const QString canonicalDirectory = directoryInfo.canonicalFilePath();
  if (canonicalDirectory.isEmpty() || !directoryInfo.isDir()) {
    return refuse(QStringLiteral("inspect.runtime.unavailable"),
                  QStringLiteral("the ONNX Runtime directory '%1' does not exist")
                      .arg(runtime.directory));
  }

  const QString modulePath = QDir(canonicalDirectory).filePath(inspectionRuntimeModuleName());
  const QFileInfo moduleInfo(modulePath);
  if (!moduleInfo.isFile()) {
    return refuse(QStringLiteral("inspect.runtime.unavailable"),
                  QStringLiteral("%1 does not hold %2")
                      .arg(canonicalDirectory, inspectionRuntimeModuleName()));
  }
  const QString canonicalModule = moduleInfo.canonicalFilePath();
  if (canonicalModule.isEmpty() || !pathWithinRoot(canonicalDirectory, canonicalModule)) {
    return refuse(QStringLiteral("inspect.runtime.outside"),
                  QStringLiteral("%1 resolves outside %2")
                      .arg(inspectionRuntimeModuleName(), canonicalDirectory));
  }
  if (!pathHasNoReparsePoints(canonicalDirectory, canonicalModule)) {
    return refuse(QStringLiteral("inspect.runtime.reparse"),
                  QStringLiteral("%1 is reached through a reparse point inside %2")
                      .arg(inspectionRuntimeModuleName(), canonicalDirectory));
  }

#ifdef _WIN32
  // The module's own imports resolve through its directory and System32 (for the
  // MSVC runtime). The process PATH and the current directory are never searched.
  const HANDLE cookie =
      AddDllDirectory(reinterpret_cast<LPCWSTR>(canonicalDirectory.utf16()));
  Q_UNUSED(cookie);
  constexpr DWORD kSearch = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 |
                            LOAD_LIBRARY_SEARCH_USER_DIRS;
  const HMODULE module =
      LoadLibraryExW(reinterpret_cast<LPCWSTR>(canonicalModule.utf16()), nullptr, kSearch);
  if (module == nullptr) {
    return refuse(QStringLiteral("inspect.runtime.load"),
                  QStringLiteral("%1 could not be loaded: %2")
                      .arg(canonicalModule, win32Text(GetLastError())));
  }

  using OrtGetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)(void);
  const auto getApiBase =
      reinterpret_cast<OrtGetApiBaseFn>(GetProcAddress(module, "OrtGetApiBase"));
#else
  const QByteArray utf8Module = canonicalModule.toUtf8();
  void* const module = ::dlopen(utf8Module.constData(), RTLD_NOW | RTLD_LOCAL);
  if (module == nullptr) {
    const char* dlErr = ::dlerror();
    return refuse(QStringLiteral("inspect.runtime.load"),
                  QStringLiteral("%1 could not be loaded: %2")
                      .arg(canonicalModule,
                           dlErr ? QString::fromUtf8(dlErr) : QStringLiteral("unknown dlopen error")));
  }

  using OrtGetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)(void);
  const auto getApiBase =
      reinterpret_cast<OrtGetApiBaseFn>(::dlsym(module, "OrtGetApiBase"));
#endif
  if (getApiBase == nullptr) {
    return refuse(QStringLiteral("inspect.runtime.abi"),
                  QStringLiteral("%1 does not export OrtGetApiBase").arg(canonicalModule));
  }
  const OrtApiBase* base = getApiBase();
  if (base == nullptr) {
    return refuse(QStringLiteral("inspect.runtime.abi"),
                  QStringLiteral("%1 returned no API base").arg(canonicalModule));
  }
  const OrtApi* api = base->GetApi(ORT_API_VERSION);
  if (api == nullptr) {
    return refuse(QStringLiteral("inspect.runtime.version"),
                  QStringLiteral("%1 does not carry ONNX Runtime C API version %2 (it reports %3)")
                      .arg(canonicalModule)
                      .arg(ORT_API_VERSION)
                      .arg(QString::fromLatin1(base->GetVersionString())));
  }

  OrtEnv* env = nullptr;
  if (OrtStatus* status = api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "mikai.native.inspect", &env)) {
    return refuse(QStringLiteral("inspect.runtime.env"), takeStatus(api, status));
  }

  g_runtime.api = api;
  g_runtime.env = env;
  g_runtime.canonicalModule = canonicalModule;
  g_runtime.version = QString::fromLatin1(base->GetVersionString());

  handle.api = api;
  handle.env = env;
  return handle;
}

// ------------------------------------------------------------------- session

OrtSession* createSession(const OrtApi* api, OrtEnv* env, const QByteArray& bytes, QString* error) {
  OrtSessionOptions* options = nullptr;
  if (OrtStatus* status = api->CreateSessionOptions(&options)) {
    *error = takeStatus(api, status);
    return nullptr;
  }
  auto optionsGuard = scoped(options, [api](OrtSessionOptions* value) {
    api->ReleaseSessionOptions(value);
  });
  if (OrtStatus* status = api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL)) {
    *error = takeStatus(api, status);
    return nullptr;
  }
  // The probe is a shape question, not a throughput one: one thread keeps it from
  // competing with the player's own pools and changes no result.
  if (OrtStatus* status = api->SetIntraOpNumThreads(options, 1)) {
    *error = takeStatus(api, status);
    return nullptr;
  }

  OrtSession* session = nullptr;
  if (OrtStatus* status = api->CreateSessionFromArray(
          env, bytes.constData(), static_cast<size_t>(bytes.size()), options, &session)) {
    *error = takeStatus(api, status);
    return nullptr;
  }
  return session;
}

/// Reads one tensor's declared type and shape. A negative dimension is dynamic
/// and stays absent; anything else is recorded exactly as declared.
bool readTensor(const OrtApi* api, OrtSession* session, bool input, size_t index,
                TensorProfile* profile, QString* name, QString* error,
                QVector<qint64>* declaredDims = nullptr) {
  OrtAllocator* allocator = nullptr;
  if (OrtStatus* status = api->GetAllocatorWithDefaultOptions(&allocator)) {
    *error = takeStatus(api, status);
    return false;
  }

  char* rawName = nullptr;
  OrtStatus* status = input ? api->SessionGetInputName(session, index, allocator, &rawName)
                            : api->SessionGetOutputName(session, index, allocator, &rawName);
  if (status != nullptr) {
    *error = takeStatus(api, status);
    return false;
  }
  if (name != nullptr) *name = QString::fromUtf8(rawName);
  api->AllocatorFree(allocator, rawName);

  OrtTypeInfo* typeInfo = nullptr;
  status = input ? api->SessionGetInputTypeInfo(session, index, &typeInfo)
                 : api->SessionGetOutputTypeInfo(session, index, &typeInfo);
  if (status != nullptr) {
    *error = takeStatus(api, status);
    return false;
  }
  auto typeGuard = scoped(typeInfo, [api](OrtTypeInfo* value) { api->ReleaseTypeInfo(value); });

  const OrtTensorTypeAndShapeInfo* tensorInfo = nullptr;
  status = api->CastTypeInfoToTensorInfo(typeInfo, &tensorInfo);
  if (status != nullptr) {
    *error = takeStatus(api, status);
    return false;
  }
  if (tensorInfo == nullptr) {
    *error = QStringLiteral("the boundary is not a tensor");
    return false;
  }

  ONNXTensorElementDataType element = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
  if (OrtStatus* elemStatus = api->GetTensorElementType(tensorInfo, &element)) {
    *error = takeStatus(api, elemStatus);
    return false;
  }
  size_t dimCount = 0;
  if (OrtStatus* dimStatus = api->GetDimensionsCount(tensorInfo, &dimCount)) {
    *error = takeStatus(api, dimStatus);
    return false;
  }
  QVector<int64_t> dims(static_cast<qsizetype>(dimCount), 0);
  if (dimCount > 0) {
    if (OrtStatus* dimError = api->GetDimensions(tensorInfo, dims.data(), dimCount)) {
      *error = takeStatus(api, dimError);
      return false;
    }
  }

  TensorProfile out;
  out.dtype = dtypeName(element);
  out.rank = static_cast<int>(dimCount);
  // N and C are non-optional in the contract, so the declared values are kept
  // verbatim: a dynamic batch reads as -1 and is refused by the boundary rules
  // rather than being quietly turned into a 1 the graph never declared.
  if (dimCount >= 1) out.batch = dims[0];
  if (dimCount >= 2) out.channels = dims[1];
  if (dimCount == 4) {
    if (dims[2] >= 0) out.height = dims[2];
    if (dims[3] >= 0) out.width = dims[3];
  }
  if (declaredDims != nullptr) {
    *declaredDims = dims;
  }
  *profile = out;
  return true;
}

/// Runs one fp16 NCHW input through the CPU and records what came out.
void runReferenceInto(const InspectionRuntime& runtime, const QByteArray& bytes,
                      const QSize& sourceGeometry, InspectionResult* result) {
  const qint64 h = sourceGeometry.height();
  const qint64 w = sourceGeometry.width();
  if (!sourceGeometry.isValid() || h <= 0 || w <= 0 || h > kMaxReferenceSide ||
      w > kMaxReferenceSide) {
    result->referenceKey = QStringLiteral("inspect.reference.geometry");
    result->referenceReason =
        QStringLiteral("the reference run needs a positive stage geometry within %1 per side")
            .arg(kMaxReferenceSide);
    return;
  }
  const qint64 inputBytes = 3 * h * w * 2;
  if (inputBytes <= 0 || inputBytes > kMaxReferenceInputBytes) {
    result->referenceKey = QStringLiteral("inspect.reference.geometry");
    result->referenceReason =
        QStringLiteral("a CPU reference at %1x%2 needs %3 bytes of fp16 input, beyond the "
                       "probe's bound")
            .arg(w)
            .arg(h)
            .arg(inputBytes);
    return;
  }

  const RuntimeHandle handle = acquireRuntime(runtime);
  if (handle.api == nullptr) {
    result->referenceKey = handle.errorKey;
    result->referenceReason = handle.error;
    return;
  }
  const OrtApi* api = handle.api;

  QString error;
  OrtSession* session = createSession(api, handle.env, bytes, &error);
  if (session == nullptr) {
    result->referenceKey = QStringLiteral("inspect.reference.session");
    result->referenceReason = error;
    return;
  }
  auto sessionGuard = scoped(session, [api](OrtSession* value) { api->ReleaseSession(value); });

  // Zeroed fp16 input. The probe answers a shape question; the values are never
  // interpreted and never fed back into anything.
  QByteArray data(static_cast<qsizetype>(inputBytes), '\0');
  const int64_t shape[4] = {1, 3, h, w};

  OrtMemoryInfo* memoryInfo = nullptr;
  if (OrtStatus* status =
          api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memoryInfo)) {
    result->referenceKey = QStringLiteral("inspect.reference.memory");
    result->referenceReason = takeStatus(api, status);
    return;
  }
  auto memoryGuard =
      scoped(memoryInfo, [api](OrtMemoryInfo* value) { api->ReleaseMemoryInfo(value); });

  OrtValue* inputValue = nullptr;
  if (OrtStatus* status = api->CreateTensorWithDataAsOrtValue(
          memoryInfo, data.data(), static_cast<size_t>(data.size()), shape, 4,
          ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16, &inputValue)) {
    result->referenceKey = QStringLiteral("inspect.reference.tensor");
    result->referenceReason = takeStatus(api, status);
    return;
  }
  auto inputGuard = scoped(inputValue, [api](OrtValue* value) { api->ReleaseValue(value); });

  const QByteArray inputName = result->inputName.toUtf8();
  const QByteArray outputName = result->outputName.toUtf8();
  const char* inputNames[] = {inputName.constData()};
  const OrtValue* inputs[] = {inputValue};
  OrtValue* outputs[] = {nullptr};
  const char* outputNames[] = {outputName.constData()};
  if (OrtStatus* status =
          api->Run(session, nullptr, inputNames, inputs, 1, outputNames, 1, outputs)) {
    result->referenceKey = QStringLiteral("inspect.reference.run");
    result->referenceReason = takeStatus(api, status);
    return;
  }
  auto outputGuard = scoped(outputs[0], [api](OrtValue* value) { api->ReleaseValue(value); });

  OrtTensorTypeAndShapeInfo* shapeInfo = nullptr;
  if (OrtStatus* status = api->GetTensorTypeAndShape(outputs[0], &shapeInfo)) {
    result->referenceKey = QStringLiteral("inspect.reference.shape");
    result->referenceReason = takeStatus(api, status);
    return;
  }
  auto shapeGuard = scoped(shapeInfo, [api](OrtTensorTypeAndShapeInfo* value) {
    api->ReleaseTensorTypeAndShapeInfo(value);
  });

  ONNXTensorElementDataType element = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
  if (OrtStatus* status = api->GetTensorElementType(shapeInfo, &element)) {
    result->referenceKey = QStringLiteral("inspect.reference.shape");
    result->referenceReason = takeStatus(api, status);
    return;
  }
  size_t dimCount = 0;
  if (OrtStatus* status = api->GetDimensionsCount(shapeInfo, &dimCount)) {
    result->referenceKey = QStringLiteral("inspect.reference.shape");
    result->referenceReason = takeStatus(api, status);
    return;
  }
  QVector<int64_t> dims(static_cast<qsizetype>(dimCount), 0);
  if (dimCount > 0) {
    if (OrtStatus* status = api->GetDimensions(shapeInfo, dims.data(), dimCount)) {
      result->referenceKey = QStringLiteral("inspect.reference.shape");
      result->referenceReason = takeStatus(api, status);
      return;
    }
  }

  result->referenceRan = true;
  result->runGeometry = sourceGeometry;

  // The proof is the actual NCHW boundary the run returned, so it is only ever
  // recorded once that boundary is itself the concrete 1x3xHxW fp16 one this
  // bridge binds: the dtype ORT reports, rank 4, N=1, C=3, positive H/W inside
  // the probe's allocation bound. This is read and checked *before* any geometry,
  // scale or proof is filled, so a malformed result names its reason instead of
  // yielding a number. `result->declared` is never touched: the batch measured
  // here is evidence, not a rewrite of what the graph stated.
  const QString actualDtype = dtypeName(element);
  if (!isEligibleIoType(actualDtype)) {
    result->referenceKey = QStringLiteral("inspect.reference.dtype");
    result->referenceReason =
        QStringLiteral("the CPU run returned %1, but this bridge binds %2 only")
            .arg(actualDtype, QLatin1String(kEligibleIoType));
    return;
  }
  if (dims.size() != 4) {
    result->referenceKey = QStringLiteral("inspect.reference.rank");
    result->referenceReason =
        QStringLiteral("the CPU run returned a rank-%1 output; a rank-4 NCHW output is required")
            .arg(dims.size());
    return;
  }
  if (dims[0] != 1) {
    result->referenceKey = QStringLiteral("inspect.reference.batch");
    result->referenceReason =
        QStringLiteral("the CPU run returned a batch of %1; N=1 is the only batch this bridge "
                       "binds")
            .arg(dims[0]);
    return;
  }
  if (dims[1] != 3) {
    result->referenceKey = QStringLiteral("inspect.reference.channels");
    result->referenceReason =
        QStringLiteral("the CPU run returned %1 channels; C=3 is the only channel count this "
                       "bridge binds")
            .arg(dims[1]);
    return;
  }
  if (dims[2] <= 0 || dims[3] <= 0 || dims[2] > kMaxReferenceSide ||
      dims[3] > kMaxReferenceSide) {
    result->referenceKey = QStringLiteral("inspect.reference.shape");
    result->referenceReason =
        QStringLiteral("the CPU run returned 1x3x%1x%2, whose H/W are not positive and bounded")
            .arg(dims[2])
            .arg(dims[3]);
    return;
  }
  const qint64 outH = dims[2];
  const qint64 outW = dims[3];
  const qint64 outputBytes = 3 * outH * outW * 2;
  if (outputBytes <= 0 || outputBytes > kMaxReferenceInputBytes) {
    result->referenceKey = QStringLiteral("inspect.reference.shape");
    result->referenceReason =
        QStringLiteral("the CPU run returned 1x3x%1x%2, whose fp16 size is beyond the probe's "
                       "bound")
            .arg(outH)
            .arg(outW);
    return;
  }

  // The concrete boundary the run produced: the input as it was bound, the output
  // as it came back, dtype included. It is evidence beside `declared`, never a
  // replacement for a single declared dimension.
  ModelProfile reference;
  reference.input.dtype = QString::fromLatin1(kEligibleIoType);
  reference.input.rank = 4;
  reference.input.batch = 1;
  reference.input.channels = 3;
  reference.input.height = h;
  reference.input.width = w;
  reference.output.dtype = actualDtype;
  reference.output.rank = 4;
  reference.output.batch = dims[0];
  reference.output.channels = dims[1];
  reference.output.height = outH;
  reference.output.width = outW;
  result->verifiedReferenceProfile = reference;
  result->resolvedGeometry = QSize(static_cast<int>(outW), static_cast<int>(outH));

  if (outH % h == 0 && outW % w == 0 && outH / h == outW / w && outH / h > 0) {
    result->resolvedScale = static_cast<int>(outH / h);
    result->referenceKey.clear();
    result->referenceReason.clear();
    return;
  }
  result->referenceKey = QStringLiteral("inspect.reference.uniform");
  result->referenceReason =
      QStringLiteral("the CPU run produced 1x3x%1x%2 from 1x3x%3x%4, which is not a uniform "
                     "positive integer multiple")
          .arg(outH)
          .arg(outW)
          .arg(h)
          .arg(w);
}

/// A declared batch the graph left unknown. `readTensor` keeps the declared value
/// verbatim, so an exporter's symbolic batch reads as -1 rather than becoming a 1.
bool hasUnknownBatch(const TensorProfile& tensor) { return tensor.batch < 0; }

/// Whether only a run can settle this declaration. A dynamic H/W needs a stage
/// geometry, and a symbolic batch needs a concrete one - even when H and W are
/// already fixed. A fully concrete declaration is its own answer.
bool needsReferenceRun(const ModelProfile& profile) {
  return profile.input.isDynamicGeometry() || profile.output.isDynamicGeometry() ||
         hasUnknownBatch(profile.input) || hasUnknownBatch(profile.output);
}

qint64 referenceElementSize(const QString& dtype) {
  if (dtype == QLatin1String("fp16")) return 2;
  if (dtype == QLatin1String("fp32")) return 4;
  return 0;
}

bool referenceElementType(const QString& dtype, ONNXTensorElementDataType* type) {
  if (dtype == QLatin1String("fp16")) {
    *type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
    return true;
  }
  if (dtype == QLatin1String("fp32")) {
    *type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    return true;
  }
  return false;
}

qint64 referenceElementCount(const QVector<int64_t>& dims, qint64 elementSize) {
  if (elementSize <= 0 || dims.isEmpty()) return 0;
  qint64 elements = 1;
  for (const int64_t dim : dims) {
    if (dim <= 0 || dim > kMaxReferenceInputBytes) return 0;
    if (elements > kMaxReferenceInputBytes / dim) return 0;
    elements *= dim;
  }
  return elements;
}

QString validateReferenceInput(const ReferenceInput& input, const QStringList& sessionInputs) {
  if (input.name.isEmpty()) {
    return QStringLiteral("an input with no name was supplied; a reference never guesses which "
                          "tensor a caller meant");
  }
  if (!sessionInputs.contains(input.name)) {
    return QStringLiteral("'%1' is not an input of this graph; its inputs are %2")
        .arg(input.name, sessionInputs.join(QStringLiteral(", ")));
  }
  const qint64 elementSize = referenceElementSize(input.dtype);
  if (elementSize == 0) {
    return QStringLiteral("input '%1' is named %2; this bridge binds fp16 and fp32 only")
        .arg(input.name, input.dtype);
  }
  if (input.shape.isEmpty()) {
    return QStringLiteral("input '%1' has no shape; a tensor cannot be bound without one")
        .arg(input.name);
  }
  qint64 elements = 1;
  for (const qint64 dim : input.shape) {
    if (dim <= 0) {
      return QStringLiteral("input '%1' has the non-positive dimension %2; a symbolic dimension "
                            "cannot be fed")
          .arg(input.name)
          .arg(dim);
    }
    if (elements > kMaxReferenceInputBytes / dim) {
      return QStringLiteral("input '%1' is beyond the probe's allocation bound").arg(input.name);
    }
    elements *= dim;
  }
  const qint64 needed = elements * elementSize;
  if (input.data.size() != needed) {
    return QStringLiteral("input '%1' holds %2 bytes, but its rank-%3 shape as %4 needs %5")
        .arg(input.name)
        .arg(input.data.size())
        .arg(input.shape.size())
        .arg(input.dtype)
        .arg(needed);
  }
  return QString();
}

bool readSessionNames(const OrtApi* api, OrtSession* session, bool input, QStringList* names,
                      QString* error) {
  size_t count = 0;
  if (OrtStatus* status = input ? api->SessionGetInputCount(session, &count)
                               : api->SessionGetOutputCount(session, &count)) {
    *error = takeStatus(api, status);
    return false;
  }
  OrtAllocator* allocator = nullptr;
  if (OrtStatus* status = api->GetAllocatorWithDefaultOptions(&allocator)) {
    *error = takeStatus(api, status);
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    char* raw = nullptr;
    if (OrtStatus* status = input ? api->SessionGetInputName(session, i, allocator, &raw)
                                  : api->SessionGetOutputName(session, i, allocator, &raw)) {
      *error = takeStatus(api, status);
      return false;
    }
    names->append(QString::fromUtf8(raw));
    api->AllocatorFree(allocator, raw);
  }
  return true;
}

}  // namespace

ReferenceRun runReferenceTensor(const InspectionRuntime& runtime, const QByteArray& graphBytes,
                                const QVector<ReferenceInput>& inputs) {
  ReferenceRun run;
  const auto refuse = [&run](const QString& key, const QString& text) {
    run.ok = false;
    run.reasonKey = key;
    run.reason = text;
    run.outputs.clear();
    return run;
  };

  if (graphBytes.isEmpty()) {
    return refuse(QStringLiteral("reference.graph.empty"), QStringLiteral("the graph has no bytes"));
  }
  if (inputs.isEmpty()) {
    return refuse(QStringLiteral("reference.inputs"),
                  QStringLiteral("a reference run binds the inputs it is given; none were supplied"));
  }

  const RuntimeHandle handle = acquireRuntime(runtime);
  if (handle.api == nullptr) return refuse(handle.errorKey, handle.error);
  const OrtApi* api = handle.api;

  QString error;
  OrtSession* session = createSession(api, handle.env, graphBytes, &error);
  if (session == nullptr) {
    return refuse(QStringLiteral("reference.session"),
                  QStringLiteral("the ONNX graph could not be read: %1").arg(error));
  }
  auto sessionGuard = scoped(session, [api](OrtSession* value) { api->ReleaseSession(value); });

  QStringList sessionInputs;
  QStringList sessionOutputs;
  if (!readSessionNames(api, session, true, &sessionInputs, &error) ||
      !readSessionNames(api, session, false, &sessionOutputs, &error)) {
    return refuse(QStringLiteral("reference.session"), error);
  }

  if (inputs.size() != sessionInputs.size()) {
    return refuse(QStringLiteral("reference.inputs.count"),
                  QStringLiteral("%1 input(s) were supplied but this graph takes %2")
                      .arg(inputs.size())
                      .arg(sessionInputs.size()));
  }
  QStringList supplied;
  for (const ReferenceInput& input : inputs) {
    const QString problem = validateReferenceInput(input, sessionInputs);
    if (!problem.isEmpty()) return refuse(QStringLiteral("reference.inputs"), problem);
    if (supplied.contains(input.name)) {
      return refuse(QStringLiteral("reference.inputs.duplicate"),
                    QStringLiteral("input '%1' was supplied twice").arg(input.name));
    }
    supplied.append(input.name);
  }

  OrtMemoryInfo* memoryInfo = nullptr;
  if (OrtStatus* status =
          api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memoryInfo)) {
    return refuse(QStringLiteral("reference.memory"), takeStatus(api, status));
  }
  auto memoryGuard =
      scoped(memoryInfo, [api](OrtMemoryInfo* value) { api->ReleaseMemoryInfo(value); });

  QVector<OrtValue*> values;
  QVector<QByteArray> nameBytes;
  values.reserve(inputs.size());
  nameBytes.reserve(inputs.size());
  const auto releaseValues = [&values, api]() {
    for (OrtValue* value : values) api->ReleaseValue(value);
    values.clear();
  };

  for (const ReferenceInput& input : inputs) {
    ONNXTensorElementDataType element = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    if (!referenceElementType(input.dtype, &element)) {
      releaseValues();
      return refuse(QStringLiteral("reference.inputs.dtype"),
                    QStringLiteral("input '%1' is named %2; this bridge binds fp16 and fp32 only")
                        .arg(input.name, input.dtype));
    }
    QVector<int64_t> dims(static_cast<qsizetype>(input.shape.size()));
    for (qsizetype i = 0; i < input.shape.size(); ++i) {
      dims[i] = static_cast<int64_t>(input.shape.at(i));
    }
    OrtValue* value = nullptr;
    // ORT declares this buffer `_Inout_` because the same call also wraps a
    // writable output, but an input tensor is only read during Run. The cast is
    // deliberate and no copy is made: these exact bytes are the ones this call
    // also digests and writes out, and a copy would break that provenance.
    if (OrtStatus* status = api->CreateTensorWithDataAsOrtValue(
            memoryInfo, const_cast<char*>(input.data.constData()),
            static_cast<size_t>(input.data.size()), dims.constData(), dims.size(), element,
            &value)) {
      releaseValues();
      return refuse(QStringLiteral("reference.tensor"), takeStatus(api, status));
    }
    values.append(value);
    nameBytes.append(input.name.toUtf8());
  }

  QVector<const OrtValue*> inputValues;
  QVector<const char*> inputNames;
  inputValues.reserve(values.size());
  inputNames.reserve(nameBytes.size());
  for (qsizetype i = 0; i < values.size(); ++i) {
    inputValues.append(values.at(i));
    inputNames.append(nameBytes.at(i).constData());
  }

  QVector<QByteArray> outputNameBytes;
  outputNameBytes.reserve(sessionOutputs.size());
  for (const QString& name : sessionOutputs) outputNameBytes.append(name.toUtf8());
  QVector<const char*> outputNames;
  outputNames.reserve(outputNameBytes.size());
  for (const QByteArray& name : outputNameBytes) outputNames.append(name.constData());

  QVector<OrtValue*> outputs(static_cast<qsizetype>(sessionOutputs.size()), nullptr);
  if (OrtStatus* status = api->Run(session, nullptr, inputNames.constData(),
                                  inputValues.constData(), inputValues.size(),
                                  outputNames.constData(), outputNames.size(), outputs.data())) {
    releaseValues();
    return refuse(QStringLiteral("reference.run"), takeStatus(api, status));
  }
  auto outputsGuard = scoped(&outputs, [api](QVector<OrtValue*>* value) {
    for (OrtValue* item : *value) {
      if (item != nullptr) api->ReleaseValue(item);
    }
  });

  QVector<ReferenceTensor> produced;
  produced.reserve(outputs.size());
  for (qsizetype i = 0; i < outputs.size(); ++i) {
    OrtTensorTypeAndShapeInfo* shapeInfo = nullptr;
    if (OrtStatus* status = api->GetTensorTypeAndShape(outputs.at(i), &shapeInfo)) {
      releaseValues();
      return refuse(QStringLiteral("reference.output.shape"), takeStatus(api, status));
    }
    auto shapeGuard = scoped(shapeInfo, [api](OrtTensorTypeAndShapeInfo* value) {
      api->ReleaseTensorTypeAndShapeInfo(value);
    });

    ONNXTensorElementDataType element = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    if (OrtStatus* status = api->GetTensorElementType(shapeInfo, &element)) {
      releaseValues();
      return refuse(QStringLiteral("reference.output.shape"), takeStatus(api, status));
    }
    size_t dimCount = 0;
    if (OrtStatus* status = api->GetDimensionsCount(shapeInfo, &dimCount)) {
      releaseValues();
      return refuse(QStringLiteral("reference.output.shape"), takeStatus(api, status));
    }
    QVector<int64_t> dims(static_cast<qsizetype>(dimCount), 0);
    if (dimCount > 0) {
      if (OrtStatus* status = api->GetDimensions(shapeInfo, dims.data(), dimCount)) {
        releaseValues();
        return refuse(QStringLiteral("reference.output.shape"), takeStatus(api, status));
      }
    }

    const QString actualDtype = dtypeName(element);
    const qint64 elementSize = referenceElementSize(actualDtype);
    if (elementSize == 0) {
      releaseValues();
      return refuse(QStringLiteral("reference.output.dtype"),
                    QStringLiteral("output '%1' is %2; this bridge exports fp16 and fp32 only")
                        .arg(sessionOutputs.at(i), actualDtype));
    }
    const qint64 elements = referenceElementCount(dims, elementSize);
    if (elements == 0) {
      QStringList printed;
      for (const int64_t dim : dims) printed.append(QString::number(dim));
      releaseValues();
      return refuse(QStringLiteral("reference.output.shape"),
                    QStringLiteral("output '%1' has the shape [%2], whose dimensions are not all "
                                   "positive and bounded")
                        .arg(sessionOutputs.at(i), printed.join(QStringLiteral(", "))));
    }
    const qint64 outputBytes = elements * elementSize;
    if (outputBytes > kMaxReferenceInputBytes) {
      releaseValues();
      return refuse(QStringLiteral("reference.output.shape"),
                    QStringLiteral("output '%1' holds %2 bytes, beyond the exporter's bound")
                        .arg(sessionOutputs.at(i))
                        .arg(outputBytes));
    }

    void* raw = nullptr;
    if (OrtStatus* status = api->GetTensorMutableData(outputs.at(i), &raw)) {
      releaseValues();
      return refuse(QStringLiteral("reference.output.data"), takeStatus(api, status));
    }
    if (raw == nullptr) {
      releaseValues();
      return refuse(QStringLiteral("reference.output.data"),
                    QStringLiteral("output '%1' came back with no readable data")
                        .arg(sessionOutputs.at(i)));
    }

    ReferenceTensor tensor;
    tensor.name = sessionOutputs.at(i);
    tensor.dtype = actualDtype;
    for (const int64_t dim : dims) tensor.shape.append(static_cast<qint64>(dim));
    tensor.data = QByteArray(static_cast<const char*>(raw), static_cast<qsizetype>(outputBytes));
    tensor.elementCount = elements;
    produced.append(tensor);
  }

  releaseValues();
  run.ok = true;
  run.reasonKey.clear();
  run.reason.clear();
  run.outputs = produced;
  return run;
}

namespace {

void refuseWith(QString* key, QString* reason, const QString& keyText, const QString& reasonText) {
  if (key != nullptr) *key = keyText;
  if (reason != nullptr) *reason = reasonText;
}

}  // namespace

// ------------------------------------------------------------------- public

QString inspectionRuntimeModuleName() { return QStringLiteral("onnxruntime.dll"); }

InspectionRuntime inspectionRuntimeAt(const QString& directory) {
  InspectionRuntime runtime;
  if (directory.isEmpty()) return runtime;
  runtime.directory = QDir::cleanPath(directory);
  runtime.modulePath = QDir(runtime.directory).filePath(inspectionRuntimeModuleName());
  return runtime;
}

InspectionRuntime defaultInspectionRuntime() {
  const QString directory = qEnvironmentVariable("MIKAI_ORT_RUNTIME_DIR");
  if (directory.isEmpty()) return InspectionRuntime{};
  return inspectionRuntimeAt(directory);
}

QString inspectionRuntimeLoadedVersion() {
  QMutexLocker locker(&g_runtimeMutex);
  return g_runtime.version;
}

bool inspectionRuntimeIsPresent(const InspectionRuntime& runtime, QString* reason) {
  if (!runtime.isSet()) {
    if (reason) *reason = QStringLiteral("no ONNX Runtime directory was named");
    return false;
  }
  const QFileInfo moduleInfo(runtime.modulePath);
  if (!moduleInfo.isAbsolute()) {
    if (reason) *reason = QStringLiteral("the runtime path '%1' is not absolute").arg(runtime.modulePath);
    return false;
  }
  if (!moduleInfo.isFile()) {
    if (reason) {
      *reason = QStringLiteral("%1 does not exist or is not a regular file").arg(runtime.modulePath);
    }
    return false;
  }
  if (reason) reason->clear();
  return true;
}

InspectionResult inspectDeclaredProfile(const InspectionRuntime& runtime, const QByteArray& bytes) {
  InspectionResult result;
  const auto refuse = [&result](const QString& key, const QString& text) {
    result.ok = false;
    result.reasonKey = key;
    result.reason = text;
    return result;
  };

  if (bytes.isEmpty()) {
    return refuse(QStringLiteral("inspect.graph.empty"), QStringLiteral("the graph has no bytes"));
  }

  const RuntimeHandle handle = acquireRuntime(runtime);
  if (handle.api == nullptr) return refuse(handle.errorKey, handle.error);
  const OrtApi* api = handle.api;

  QString error;
  OrtSession* session = createSession(api, handle.env, bytes, &error);
  if (session == nullptr) {
    return refuse(QStringLiteral("inspect.graph.unreadable"),
                  QStringLiteral("the ONNX graph could not be read: %1").arg(error));
  }
  auto sessionGuard = scoped(session, [api](OrtSession* value) { api->ReleaseSession(value); });

  size_t inputCount = 0;
  size_t outputCount = 0;
  if (OrtStatus* status = api->SessionGetInputCount(session, &inputCount)) {
    return refuse(QStringLiteral("inspect.graph.read"), takeStatus(api, status));
  }
  if (OrtStatus* status = api->SessionGetOutputCount(session, &outputCount)) {
    return refuse(QStringLiteral("inspect.graph.read"), takeStatus(api, status));
  }
  if (inputCount != 1 || outputCount != 1) {
    return refuse(QStringLiteral("inspect.graph.ioCount"),
                  QStringLiteral("the graph declares %1 input(s) and %2 output(s); exactly one "
                                 "input and one output are required")
                      .arg(static_cast<qulonglong>(inputCount))
                      .arg(static_cast<qulonglong>(outputCount)));
  }

  TensorProfile input;
  TensorProfile output;
  QString name;
  if (!readTensor(api, session, true, 0, &input, &name, &error)) {
    return refuse(QStringLiteral("inspect.graph.type"), error);
  }
  result.inputName = name;
  if (!readTensor(api, session, false, 0, &output, &name, &error)) {
    return refuse(QStringLiteral("inspect.graph.type"), error);
  }
  result.outputName = name;
  result.declared.input = input;
  result.declared.output = output;
  result.ok = true;
  result.reasonKey.clear();
  result.reason.clear();
  return result;
}

InspectionResult inspectGraph(const InspectionRuntime& runtime, const QByteArray& bytes,
                              const QSize& sourceGeometry) {
  InspectionResult result = inspectDeclaredProfile(runtime, bytes);
  if (!result.ok) return result;

  const ModelProfile& declared = result.declared;
  const bool fp16 = isEligibleIoType(declared.input.dtype) &&
                    isEligibleIoType(declared.output.dtype);
  // The FP16 bridge is the only binding: a graph in another dtype is reported,
  // never executed and never converted.
  if (!fp16) return result;
  // A fully concrete declaration is authoritative: there is nothing for a run to
  // learn, and executing would be work for no fact. A symbolic batch or a dynamic
  // H/W is exactly what a run settles, so those still run - even at fixed H/W.
  if (!needsReferenceRun(declared)) return result;
  // A run only makes sense for a boundary this bridge could bind. A graph the
  // declaration already refuses is named here rather than executed to learn a
  // shape it could never legally produce.
  const QString candidateRefusal = spatialCandidateRefusal(declared);
  if (!candidateRefusal.isEmpty()) {
    result.referenceKey = QStringLiteral("inspect.reference.ineligible");
    result.referenceReason = candidateRefusal;
    return result;
  }

  // A dynamic graph runs at the caller's stage geometry; a fixed one can only be
  // fed the geometry it declares, so that is the geometry it is run at. Either
  // way the geometry has to be a positive size.
  QSize runAt = sourceGeometry;
  if (const std::optional<QSize> fixedInput = declared.sourceGeometry()) runAt = *fixedInput;
  if (!runAt.isValid() || runAt.width() <= 0 || runAt.height() <= 0) {
    result.referenceKey = QStringLiteral("inspect.reference.geometry");
    result.referenceReason =
        QStringLiteral("resolving this graph needs a positive geometry to run at");
    return result;
  }

  runReferenceInto(runtime, bytes, runAt, &result);
  return result;
}

bool inspectDeclaredBoundaries(const InspectionRuntime& runtime, const QByteArray& bytes,
                               DeclaredBoundaries* boundaries, QString* reasonKey,
                               QString* reason) {
  const auto refuse = [reasonKey, reason](const QString& key, const QString& text) {
    if (reasonKey != nullptr) *reasonKey = key;
    if (reason != nullptr) *reason = text;
    return false;
  };

  if (bytes.isEmpty()) {
    return refuse(QStringLiteral("inspect.graph.empty"), QStringLiteral("the graph has no bytes"));
  }
  const RuntimeHandle handle = acquireRuntime(runtime);
  if (handle.api == nullptr) return refuse(handle.errorKey, handle.error);
  const OrtApi* api = handle.api;

  QString error;
  OrtSession* session = createSession(api, handle.env, bytes, &error);
  if (session == nullptr) {
    return refuse(QStringLiteral("inspect.graph.unreadable"),
                  QStringLiteral("the ONNX graph could not be read: %1").arg(error));
  }
  auto sessionGuard = scoped(session, [api](OrtSession* value) { api->ReleaseSession(value); });

  DeclaredBoundaries out;
  for (const bool input : {true, false}) {
    size_t count = 0;
    OrtStatus* status = nullptr;
    if (input) {
      status = api->SessionGetInputCount(session, &count);
    } else {
      status = api->SessionGetOutputCount(session, &count);
    }
    if (status != nullptr) {
      return refuse(QStringLiteral("inspect.graph.read"), takeStatus(api, status));
    }
    if (count == 0) {
      return refuse(QStringLiteral("inspect.graph.ioCount"),
                    QStringLiteral("the graph declares no %1 tensor")
                        .arg(input ? QStringLiteral("input") : QStringLiteral("output")));
    }
    QVector<NamedTensorProfile>& side = input ? out.inputs : out.outputs;
    for (size_t i = 0; i < count; ++i) {
      NamedTensorProfile entry;
      if (!readTensor(api, session, input, i, &entry.profile, &entry.name, &error, &entry.dims)) {
        return refuse(QStringLiteral("inspect.graph.type"), error);
      }
      side.append(entry);
    }
  }
  *boundaries = out;
  if (reasonKey != nullptr) reasonKey->clear();
  if (reason != nullptr) reason->clear();
  return true;
}

std::optional<StageResolution> resolveStageGeometry(const ModelProfile& declared,
                                                    const QSize& sourceGeometry,
                                                    const QByteArray& onnxBytes,
                                                    const InspectionRuntime& runtime,
                                                    QString* reasonKey, QString* reason) {
  // A strict, fully fixed declaration is its own answer: no runtime is needed and
  // nothing is executed.
  const QString strictRefusal = spatialEligibilityRefusal(declared);
  if (strictRefusal.isEmpty() && !needsReferenceRun(declared)) {
    const std::optional<QSize> input = declared.sourceGeometry();
    const std::optional<QSize> output = declared.outputGeometry();
    const std::optional<int> scale = declared.derivedUniformScale();
    if (!input.has_value() || !output.has_value() || !scale.has_value()) {
      refuseWith(reasonKey, reason, QStringLiteral("inspect.resolution.declared"),
                 QStringLiteral("the declared geometry does not resolve to a uniform positive "
                                "integer scale"));
      return std::nullopt;
    }
    StageResolution resolution;
    resolution.input = *input;
    resolution.output = *output;
    resolution.scale = *scale;
    resolution.fromCpuReference = false;
    if (reasonKey) reasonKey->clear();
    if (reason) reason->clear();
    return resolution;
  }

  // Everything else is either a dynamic graph or a declaration with a symbolic
  // batch, and both are answered only by a real CPU run. The declaration itself
  // still has to be a candidate this bridge could bind.
  const QString candidateRefusal = spatialCandidateRefusal(declared);
  if (!candidateRefusal.isEmpty()) {
    refuseWith(reasonKey, reason, QStringLiteral("inspect.profile.ineligible"), candidateRefusal);
    return std::nullopt;
  }

  if (onnxBytes.isEmpty()) {
    refuseWith(reasonKey, reason,
               QStringLiteral("inspect.reference.bytes"),
               QStringLiteral("resolving this graph needs the graph's bytes for the CPU "
                              "reference run"));
    return std::nullopt;
  }

  const InspectionResult inspected = inspectGraph(runtime, onnxBytes, sourceGeometry);
  if (!inspected.ok) {
    refuseWith(reasonKey, reason, inspected.reasonKey, inspected.reason);
    return std::nullopt;
  }
  if (!inspected.referenceRan || !inspected.verifiedReferenceProfile.has_value() ||
      !inspected.resolvedGeometry.has_value() || !inspected.resolvedScale.has_value()) {
    refuseWith(reasonKey, reason,
               inspected.referenceKey.isEmpty() ? QStringLiteral("inspect.reference.failed")
                                                : inspected.referenceKey,
               inspected.referenceReason.isEmpty()
                   ? QStringLiteral("the CPU reference did not produce a uniform geometry")
                   : inspected.referenceReason);
    return std::nullopt;
  }

  // The run has to prove *this* declaration, not merely have happened: the actual
  // boundary is checked against the declared one before it is answered with.
  const QString proofRefusal =
      spatialReferenceRefusal(declared, *inspected.verifiedReferenceProfile);
  if (!proofRefusal.isEmpty()) {
    refuseWith(reasonKey, reason, QStringLiteral("inspect.reference.unverified"), proofRefusal);
    return std::nullopt;
  }

  StageResolution resolution;
  resolution.input = inspected.runGeometry;
  resolution.output = *inspected.resolvedGeometry;
  resolution.scale = *inspected.resolvedScale;
  resolution.fromCpuReference = true;
  resolution.verifiedReferenceProfile = *inspected.verifiedReferenceProfile;
  if (reasonKey) reasonKey->clear();
  if (reason) reason->clear();
  return resolution;
}

}  // namespace native
}  // namespace mplayer
