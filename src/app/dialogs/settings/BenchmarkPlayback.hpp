#pragma once

#include <QOffscreenSurface>
#include <QSize>
#include <QString>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <native/components/AnimeJanaiRuntime.hpp>

namespace mikai::app {

enum class BenchmarkEngineBuildUpdate { Started, Ready, Failed };

mplayer::native::AnimeJanaiSourceContext readBenchmarkSourceContext(
    const QString& clipPath, QString* error);

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
    const std::function<void(BenchmarkEngineBuildUpdate, const QString&)>& onEngine,
    int ajiStageCount = 1);

struct BenchmarkFrameSample {
    qint64 startedAtNs = -1;
    int frames = 0;

    constexpr void reset() noexcept {
        startedAtNs = -1;
        frames = 0;
    }

    constexpr void recordFrame(qint64 nowNs) noexcept {
        if (startedAtNs < 0) {
            startedAtNs = nowNs;
            return;
        }
        ++frames;
    }

    constexpr bool started() const noexcept { return startedAtNs >= 0; }
    constexpr double seconds(qint64 nowNs) const noexcept {
        return started() ? static_cast<double>(nowNs - startedAtNs) / 1e9 : 0.0;
    }
};

}  // namespace mikai::app
