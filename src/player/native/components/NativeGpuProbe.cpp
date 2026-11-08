#include "native/components/NativeGpuProbe.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#else
#include <dlfcn.h>
#include <QFile>
#include <QTextStream>
#endif

namespace mplayer::native {
namespace {

QString translated(const char* message) {
    return QCoreApplication::translate("NativeGpuProbe", message);
}

bool probeNvml(GpuComputeCapability& cap) {
#ifdef _WIN32
    HMODULE nvml = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (nvml == nullptr) {
        const QString system32 =
            QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("System32/nvml.dll"));
        nvml = LoadLibraryExW(reinterpret_cast<LPCWSTR>(system32.utf16()), nullptr,
                              LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (nvml == nullptr) return false;
    auto get = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(nvml, name)); };
    auto closeLib = [&]() { FreeLibrary(nvml); };
#else
    void* nvml = nullptr;
    for (const char* candidate : {
             "libnvidia-ml.so.1",
             "libnvidia-ml.so",
             "/usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1",
             "/usr/lib64/libnvidia-ml.so.1",
             "/usr/lib/wsl/lib/libnvidia-ml.so.1"}) {
        nvml = ::dlopen(candidate, RTLD_LAZY | RTLD_LOCAL);
        if (nvml != nullptr) break;
    }
    if (nvml == nullptr) return false;
    auto get = [&](const char* name) { return ::dlsym(nvml, name); };
    auto closeLib = [&]() { ::dlclose(nvml); };
#endif

    using InitFn = int (*)();
    using HandleFn = int (*)(unsigned, void**);
    using CapFn = int (*)(void*, int*, int*);
    using NameFn = int (*)(void*, char*, unsigned);
    using DriverFn = int (*)(char*, unsigned);
    using ShutdownFn = int (*)();

    auto init = reinterpret_cast<InitFn>(get("nvmlInit_v2"));
    if (init == nullptr) init = reinterpret_cast<InitFn>(get("nvmlInit"));
    auto getHandle = reinterpret_cast<HandleFn>(get("nvmlDeviceGetHandleByIndex_v2"));
    if (getHandle == nullptr) getHandle = reinterpret_cast<HandleFn>(get("nvmlDeviceGetHandleByIndex"));
    auto getCap = reinterpret_cast<CapFn>(get("nvmlDeviceGetCudaComputeCapability"));
    auto getName = reinterpret_cast<NameFn>(get("nvmlDeviceGetName"));
    auto getDriver = reinterpret_cast<DriverFn>(get("nvmlSystemGetDriverVersion"));
    auto shutdown = reinterpret_cast<ShutdownFn>(get("nvmlShutdown"));

    if (init == nullptr || getHandle == nullptr || getCap == nullptr || init() != 0) {
        closeLib();
        return false;
    }

    void* device = nullptr;
    if (getHandle(0, &device) != 0 || device == nullptr) {
        if (shutdown) shutdown();
        closeLib();
        return false;
    }

    int major = 0, minor = 0;
    if (getCap(device, &major, &minor) != 0) {
        if (shutdown) shutdown();
        closeLib();
        return false;
    }

    cap.nvidia = true;
    cap.smMajor = major;
    cap.smMinor = minor;

    if (getName) {
        char buffer[256] = {};
        if (getName(device, buffer, sizeof(buffer)) == 0) {
            cap.name = QString::fromUtf8(buffer).trimmed();
        }
    }
    if (getDriver) {
        char buffer[256] = {};
        if (getDriver(buffer, sizeof(buffer)) == 0) {
            cap.driverVersion = QString::fromUtf8(buffer);
        }
    }

    if (shutdown) shutdown();
    closeLib();
    return true;
}

bool probeCudaDriver(GpuComputeCapability& cap) {
#ifdef _WIN32
    HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (cuda == nullptr) {
        const QString system32 =
            QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("System32/nvcuda.dll"));
        cuda = LoadLibraryExW(reinterpret_cast<LPCWSTR>(system32.utf16()), nullptr,
                              LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (cuda == nullptr) return false;
    auto get = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(cuda, name)); };
    auto closeLib = [&]() { FreeLibrary(cuda); };
#else
    void* cuda = nullptr;
    for (const char* candidate : {"libcuda.so.1", "libcuda.so", "/usr/lib/wsl/lib/libcuda.so.1"}) {
        cuda = ::dlopen(candidate, RTLD_LAZY | RTLD_LOCAL);
        if (cuda != nullptr) break;
    }
    if (cuda == nullptr) return false;
    auto get = [&](const char* name) { return ::dlsym(cuda, name); };
    auto closeLib = [&]() { ::dlclose(cuda); };
#endif

    using CuInitFn = int (*)(unsigned int);
    using CuDeviceGetFn = int (*)(int*, int);
    using CuDeviceGetNameFn = int (*)(char*, int, int);
    using CuDeviceGetAttrFn = int (*)(int*, int, int);
    using CuDriverGetVerFn = int (*)(int*);

    auto cuInit = reinterpret_cast<CuInitFn>(get("cuInit"));
    auto cuDeviceGet = reinterpret_cast<CuDeviceGetFn>(get("cuDeviceGet"));
    auto cuDeviceGetName = reinterpret_cast<CuDeviceGetNameFn>(get("cuDeviceGetName"));
    auto cuDeviceGetAttribute = reinterpret_cast<CuDeviceGetAttrFn>(get("cuDeviceGetAttribute"));
    auto cuDriverGetVersion = reinterpret_cast<CuDriverGetVerFn>(get("cuDriverGetVersion"));

    if (cuInit == nullptr || cuDeviceGet == nullptr || cuDeviceGetName == nullptr ||
        cuDeviceGetAttribute == nullptr || cuInit(0) != 0) {
        closeLib();
        return false;
    }

    int dev = 0;
    if (cuDeviceGet(&dev, 0) != 0) {
        closeLib();
        return false;
    }

    int major = 0, minor = 0;
    // 75 = CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR
    // 76 = CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR
    if (cuDeviceGetAttribute(&major, 75, dev) != 0 ||
        cuDeviceGetAttribute(&minor, 76, dev) != 0) {
        closeLib();
        return false;
    }

    cap.nvidia = true;
    cap.smMajor = major;
    cap.smMinor = minor;

    char buffer[256] = {};
    if (cuDeviceGetName(buffer, sizeof(buffer), dev) == 0) {
        cap.name = QString::fromUtf8(buffer).trimmed();
    }
    if (cuDriverGetVersion) {
        int ver = 0;
        if (cuDriverGetVersion(&ver) == 0 && ver > 0) {
            cap.driverVersion = QStringLiteral("%1.%2").arg(ver / 1000).arg((ver % 100) / 10);
        }
    }
    closeLib();
    return true;
}

#ifdef _WIN32
bool probeDxgi(GpuComputeCapability& cap) {
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dxgi == nullptr) return false;
    using CreateFactoryFn = HRESULT (WINAPI*)(REFIID, void**);
    auto createFactory = reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (createFactory == nullptr) {
        FreeLibrary(dxgi);
        return false;
    }

    IDXGIFactory1* factory = nullptr;
    if (FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || factory == nullptr) {
        FreeLibrary(dxgi);
        return false;
    }

    bool found = false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            if (desc.VendorId == 0x10DE) { // NVIDIA Corporation
                cap.nvidia = true;
                cap.name = QString::fromWCharArray(desc.Description).trimmed();
                const QString upper = cap.name.toUpper();
                if (upper.contains(QLatin1String("RTX"))) {
                    cap.smMajor = 8;
                    cap.smMinor = 0;
                } else if (upper.contains(QLatin1String("GTX 16"))) {
                    cap.smMajor = 7;
                    cap.smMinor = 5;
                } else if (upper.contains(QLatin1String("GTX 10"))) {
                    cap.smMajor = 6;
                    cap.smMinor = 1;
                }
                found = true;
                adapter->Release();
                break;
            }
        }
        adapter->Release();
    }
    factory->Release();
    FreeLibrary(dxgi);
    return found;
}
#else
bool probeLinuxProcfs(GpuComputeCapability& cap) {
    const QDir gpusDir(QStringLiteral("/proc/driver/nvidia/gpus"));
    if (!gpusDir.exists()) return false;
    const QStringList entries = gpusDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : entries) {
        const QString infoPath = gpusDir.filePath(entry + QStringLiteral("/information"));
        QFile file(infoPath);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&file);
            while (!in.atEnd()) {
                const QString line = in.readLine();
                if (line.startsWith(QLatin1String("Model:"), Qt::CaseInsensitive)) {
                    cap.nvidia = true;
                    cap.name = line.section(QLatin1Char(':'), 1).trimmed();
                    const QString upper = cap.name.toUpper();
                    if (upper.contains(QLatin1String("RTX"))) {
                        cap.smMajor = 8;
                        cap.smMinor = 0;
                    }
                    return true;
                }
            }
        }
    }
    return false;
}
#endif

GpuComputeCapability probe() {
    GpuComputeCapability cap;
    if (probeNvml(cap)) return cap;
    if (probeCudaDriver(cap)) return cap;
#ifdef _WIN32
    if (probeDxgi(cap)) return cap;
#else
    if (probeLinuxProcfs(cap)) return cap;
#endif

    cap.reasonKey = QStringLiteral("native.gpu.deviceMissing");
    cap.reason = translated(QT_TRANSLATE_NOOP("NativeGpuProbe", "no NVIDIA device found"));
    return cap;
}

}  // namespace

const GpuComputeCapability& observeComputeCapability() {
    static const GpuComputeCapability cap = [] {
        const GpuComputeCapability value = probe();
        qInfo().noquote() << "native gpu:" << computeCapabilityText(value);
        return value;
    }();
    return cap;
}

QString computeCapabilityText(const GpuComputeCapability& capability) {
    if (!capability.nvidia) {
        return capability.reason;
    }
    return translated(QT_TRANSLATE_NOOP("NativeGpuProbe", "%1 - SM %2.%3"))
        .arg(capability.name.isEmpty() ? translated(QT_TRANSLATE_NOOP("NativeGpuProbe", "NVIDIA GPU")) : capability.name)
        .arg(capability.smMajor)
        .arg(capability.smMinor);
}

bool isNvidiaRtx(const GpuComputeCapability& capability) {
#ifdef _WIN32
    if (!capability.nvidia) {
        return false;
    }
    const QString upper = capability.name.toUpper();
    if (upper.contains(QLatin1String("GTX"))) {
        return false;
    }
    if (upper.contains(QLatin1String("RTX"))) {
        return true;
    }
    return capability.smMajor >= 8;
#else
    Q_UNUSED(capability);
    return false;
#endif
}

Backend defaultAnimeJanaiBackend(const GpuComputeCapability& capability) {
#ifdef _WIN32
    if (isNvidiaRtx(capability)) {
        return Backend::TensorRTRTX;
    }
    if (capability.nvidia) {
        return Backend::TensorRT;
    }
    return Backend::DirectML;
#else
    Q_UNUSED(capability);
    return Backend::TensorRT;
#endif
}

}  // namespace mplayer::native
