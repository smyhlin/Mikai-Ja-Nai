#pragma once

#include <QString>
#include "native/status/NativeTypes.hpp"

namespace mplayer::native {

struct GpuComputeCapability {
    bool nvidia = false;
    int smMajor = 0;
    int smMinor = 0;
    QString name;
    QString driverVersion;
    QString reasonKey;
    QString reason;
};

const GpuComputeCapability& observeComputeCapability();

QString computeCapabilityText(const GpuComputeCapability& capability);

/// Returns true if the detected GPU is an NVIDIA GeForce RTX / RTX-class GPU.
bool isNvidiaRtx(const GpuComputeCapability& capability);

/// Returns the recommended default AnimeJaNai backend based on observed GPU capability:
/// - Windows + NVIDIA RTX -> Backend::TensorRTRTX
/// - NVIDIA non-RTX (GTX / older) or Linux -> Backend::TensorRT
/// - Non-NVIDIA (AMD / Intel) on Windows -> Backend::DirectML
Backend defaultAnimeJanaiBackend(const GpuComputeCapability& capability);

}  // namespace mplayer::native
