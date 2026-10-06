#pragma once

#include <QString>

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

}  // namespace mplayer::native
