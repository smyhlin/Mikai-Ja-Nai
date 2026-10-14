#include "native/components/NativeGpuProbe.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace mplayer::native {
namespace {

QString translated(const char* message) {
    return QCoreApplication::translate("NativeGpuProbe", message);
}

GpuComputeCapability probe() {
    GpuComputeCapability cap;

#ifdef _WIN32
    HMODULE nvml = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (nvml == nullptr) {
        const QString system32 =
            QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("System32/nvml.dll"));
        nvml = LoadLibraryExW(reinterpret_cast<LPCWSTR>(system32.utf16()), nullptr,
                              LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (nvml == nullptr) {
        cap.reasonKey = QStringLiteral("native.gpu.nvmlMissing");
        cap.reason = translated("NVIDIA GPU driver (nvml.dll) not found");
        return cap;
    }
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
    if (nvml == nullptr) {
        cap.reasonKey = QStringLiteral("native.gpu.nvmlMissing");
        cap.reason = translated("NVIDIA GPU driver (nvml.dll) not found");
        return cap;
    }
    auto get = [&](const char* name) { return ::dlsym(nvml, name); };
    auto closeLib = [&]() { ::dlclose(nvml); };
#endif

    // The index comes first and the out-parameter second; passing them the other
    // way round is accepted by the compiler and refused by NVML, which is how
    // this reported "no NVIDIA device found" on a machine with one.
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

    if (init == nullptr || getHandle == nullptr || getCap == nullptr) {
        closeLib();
        cap.reasonKey = QStringLiteral("native.gpu.nvmlMissing");
        cap.reason = translated("nvml.dll is missing required entry points");
        return cap;
    }

    if (init() != 0) {
        closeLib();
        cap.reasonKey = QStringLiteral("native.gpu.deviceMissing");
        cap.reason = translated("nvmlInit failed");
        return cap;
    }

    void* device = nullptr;
    if (getHandle(0, &device) != 0 || device == nullptr) {
        if (shutdown) shutdown();
        closeLib();
        cap.reasonKey = QStringLiteral("native.gpu.deviceMissing");
        cap.reason = translated("no NVIDIA device found");
        return cap;
    }

    int major = 0, minor = 0;
    if (getCap(device, &major, &minor) != 0) {
        if (shutdown) shutdown();
        closeLib();
        cap.reasonKey = QStringLiteral("native.gpu.capabilityUnreported");
        cap.reason = translated("could not read compute capability");
        return cap;
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
    return translated("%1 - SM %2.%3")
        .arg(capability.name.isEmpty() ? translated("NVIDIA GPU") : capability.name)
        .arg(capability.smMajor)
        .arg(capability.smMinor);
}

}  // namespace mplayer::native
