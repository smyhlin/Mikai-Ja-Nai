#pragma once

#include <QOffscreenSurface>
#include <QSize>
#include <QString>

#include <functional>
#include <string>
#include <vector>

#include <native/status/NativeTypes.hpp>

namespace mikai::app {

enum class BenchmarkEngineBuildUpdate { Started, Ready, Failed };

struct BenchmarkSampleResult {
    int measuredFrames = 0;
    double sampleSeconds = 0.0;
    double fps = -1.0;
    QString error;
    qint64 enginePrepMs = -1;
};

BenchmarkSampleResult runBenchmarkSample(
    const QString& clipPath,
    const QSize& targetSize,
    const std::vector<std::pair<std::string, std::string>>& mpvOptions,
    mplayer::native::Backend backend,
    int slot,
    QOffscreenSurface& surface,
    const std::function<void(const QString&)>& onPhase,
    const std::function<void(BenchmarkEngineBuildUpdate, const QString&)>& onEngine);

struct BenchmarkFrameSample {
    qint64 startedAtNs = -1;
    int frames = 0;

    void reset() {
        startedAtNs = -1;
        frames = 0;
    }

    void recordFrame(qint64 nowNs) {
        if (startedAtNs < 0) {
            startedAtNs = nowNs;
            return;
        }
        ++frames;
    }

    bool started() const { return startedAtNs >= 0; }
    double seconds(qint64 nowNs) const {
        return started() ? static_cast<double>(nowNs - startedAtNs) / 1e9 : 0.0;
    }
};

}  // namespace mikai::app
