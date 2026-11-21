#include "ClipSizeEstimate.hpp"

#include "MpvApi.hpp"
#include "MpvHandle.hpp"

#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <vector>

namespace mplayer::shell {
namespace {

bool encoderAvailable(mplayer::MpvHandle& handle, const char* name) {
    mplayer::MpvApi& api = mplayer::MpvApi::instance();
    mpv_node node{};
    if (api.functions().getProperty(handle.raw(), "encoder-list", MPV_FORMAT_NODE, &node) < 0)
        return false;
    mplayer::NodeValue value;
    mplayer::copyNode(&node, value);
    api.functions().freeNodeContents(&node);
    if (value.format != MPV_FORMAT_NODE_ARRAY) return false;
    for (const mplayer::NodeValue& item : value.list) {
        if (item.format != MPV_FORMAT_NODE_MAP) continue;
        for (const auto& [key, field] : item.map)
            if (key == "driver" && field.format == MPV_FORMAT_STRING && field.text == name)
                return true;
    }
    return false;
}

double median(std::vector<double>& values) {
    if (values.empty()) return 0.0;
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    double result = *middle;
    if ((values.size() & 1U) == 0) {
        const auto lower = std::max_element(values.begin(), middle);
        result = (result + *lower) * 0.5;
    }
    return result;
}

bool finiteFeatures(const ClipContentFeatures& f) {
    return f.sampleCount > 0 && f.temporalPairCount >= 0 &&
        f.temporalPairCount < f.sampleCount &&
        std::isfinite(f.spatial) && f.spatial >= 0.0 && f.spatial <= 1.0 &&
        std::isfinite(f.temporal) && f.temporal >= 0.0 && f.temporal <= 1.0 &&
        std::isfinite(f.grain) && f.grain >= 0.0 && f.grain <= 1.0 &&
        std::isfinite(f.edgeDensity) && f.edgeDensity >= 0.0 && f.edgeDensity <= 1.0 &&
        std::isfinite(f.entropy) && f.entropy >= 0.0 && f.entropy <= 1.0;
}

} // namespace

bool probeClipEncodingProfile(ClipEncodingProfile& profile, QString& error) {
    struct CachedProbe {
        QMutex mutex;
        QString modulePath;
        bool valid = false;
        ClipEncodingProfile profile;
    };
    static CachedProbe cache;

    mplayer::MpvApi& api = mplayer::MpvApi::instance();
    const bool apiLoaded = api.loaded();
    const QString currentModule = apiLoaded ? api.bundle().modulePath : QString();
    if (apiLoaded && !currentModule.isEmpty()) {
        QMutexLocker lock(&cache.mutex);
        if (cache.valid && cache.modulePath == currentModule) {
            profile = cache.profile;
            error.clear();
            return true;
        }
    }

    try {
        mplayer::MpvHandle probe({{"config", "no"}, {"vo", "null"}, {"audio", "no"}});
        if (encoderAvailable(probe, "libx264")) {
            profile = {ClipVideoEncoder::H264, "libx264", "preset=veryfast,crf=23,profile=high"};
            error.clear();
            if (apiLoaded && !currentModule.isEmpty()) {
                QMutexLocker lock(&cache.mutex);
                cache.modulePath = currentModule;
                cache.profile = profile;
                cache.valid = true;
            }
            return true;
        }
        if (encoderAvailable(probe, "mpeg4")) {
            profile = {ClipVideoEncoder::Mpeg4, "mpeg4", "flags=+qscale,global_quality=472"};
            error.clear();
            if (apiLoaded && !currentModule.isEmpty()) {
                QMutexLocker lock(&cache.mutex);
                cache.modulePath = currentModule;
                cache.profile = profile;
                cache.valid = true;
            }
            return true;
        }
        error = QStringLiteral("The selected mpv runtime provides neither libx264 nor the MPEG-4 Part 2 encoder.");
    } catch (const std::exception& e) {
        error = QString::fromUtf8(e.what());
    }
    return false;
}

std::optional<QSize> clipOutputSize(QSize sourceSize, int maximumHeight) {
    if (maximumHeight != 480 && maximumHeight != 720 && maximumHeight != 1080) return std::nullopt;
    if (sourceSize.width() < 2 || sourceSize.height() < 2) return std::nullopt;
    const int sourceWidth = sourceSize.width(), sourceHeight = sourceSize.height();
    const int boundedHeight = std::min(sourceHeight, maximumHeight);
    const long double scale = static_cast<long double>(boundedHeight) / sourceHeight;
    const long double desiredWidth = std::min(static_cast<long double>(sourceWidth),
        static_cast<long double>(sourceWidth) * scale);
    const auto nearestEven = [](long double value, int maximum) {
        const int evenMaximum = maximum & ~1;
        const long double rounded = std::min(std::floor(value / 2.0L + 0.5L) * 2.0L,
                                             static_cast<long double>(evenMaximum));
        return std::clamp(static_cast<int>(rounded), 2, evenMaximum);
    };
    const int width = nearestEven(desiredWidth, sourceWidth);
    const int height = nearestEven(boundedHeight, boundedHeight);
    if (width < 2 || height < 2) return std::nullopt;
    return QSize(width, height);
}

std::optional<ClipAnalysisFrame> analyzeClipFrame(const QImage& image) {
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) return std::nullopt;
    const QImage scaled = image.scaled(QSize(160, 90), Qt::KeepAspectRatio, Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_Grayscale8);
    if (scaled.isNull() || scaled.width() <= 0 || scaled.height() <= 0) return std::nullopt;
    ClipAnalysisFrame out;
    const int x0 = (160 - scaled.width()) / 2, y0 = (90 - scaled.height()) / 2;
    for (int y = 0; y < scaled.height(); ++y) {
        const uchar* row = scaled.constScanLine(y);
        std::copy_n(row, scaled.width(), out.grayscale.begin() + (y + y0) * 160 + x0);
    }
    std::array<int, 16> histogram{};
    double gradientSum = 0.0, laplaceSum = 0.0;
    int edgeCount = 0, count = 0;
    for (int y = 1; y < 89; ++y) for (int x = 1; x < 159; ++x) {
        const int i = y * 160 + x;
        const int c = out.grayscale[size_t(i)];
        const int gx = int(out.grayscale[size_t(i + 1)]) - int(out.grayscale[size_t(i - 1)]);
        const int gy = int(out.grayscale[size_t(i + 160)]) - int(out.grayscale[size_t(i - 160)]);
        const int lap = int(out.grayscale[size_t(i - 1)]) + int(out.grayscale[size_t(i + 1)]) +
            int(out.grayscale[size_t(i - 160)]) + int(out.grayscale[size_t(i + 160)]) - 4 * c;
        gradientSum += std::hypot(double(gx), double(gy)) / (2.0 * 255.0);
        laplaceSum += std::abs(lap) / (4.0 * 255.0);
        edgeCount += (std::abs(gx) + std::abs(gy) >= 64);
        ++histogram[size_t(c >> 4)]; ++count;
    }
    if (count == 0) return std::nullopt;
    double entropy = 0.0;
    for (int bin : histogram) if (bin > 0) {
        const double p = double(bin) / count;
        entropy -= p * std::log2(p);
    }
    out.spatial = gradientSum / count;
    out.grain = laplaceSum / count;
    out.edgeDensity = double(edgeCount) / count;
    out.entropy = entropy / 4.0;
    return out;
}

ClipContentFeatures summarizeClipSamples(std::span<const ClipAnalysisFrame> frames,
                                         std::span<const double> times, double sourceFps) {
    ClipContentFeatures result;
    if (frames.empty() || frames.size() != times.size() ||
        frames.size() > size_t(std::numeric_limits<int>::max()) ||
        !std::isfinite(sourceFps) || sourceFps <= 0.0)
        return result;
    for (double t : times) if (!std::isfinite(t)) return result;
    std::vector<double> spatial, grain, edge, entropy, temporal;
    spatial.reserve(frames.size()); grain.reserve(frames.size()); edge.reserve(frames.size()); entropy.reserve(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& f = frames[i];
        if (!std::isfinite(f.spatial) || !std::isfinite(f.grain) || !std::isfinite(f.edgeDensity) || !std::isfinite(f.entropy))
            return {};
        spatial.push_back(f.spatial); grain.push_back(f.grain);
        edge.push_back(f.edgeDensity); entropy.push_back(f.entropy);
        if (i > 0) {
            const double gap = times[i] - times[i - 1];
            if (gap >= 0.0 && gap <= 1.5 / sourceFps) {
                double diff = 0.0;
                for (size_t p = 0; p < f.grayscale.size(); ++p)
                    diff += std::abs(int(f.grayscale[p]) - int(frames[i - 1].grayscale[p])) / 255.0;
                temporal.push_back(diff / f.grayscale.size());
            }
        }
    }
    result.sampleCount = int(frames.size());
    result.temporalPairCount = int(temporal.size());
    result.spatial = median(spatial); result.grain = median(grain);
    result.edgeDensity = median(edge); result.entropy = median(entropy);
    if (!temporal.empty()) result.temporal = median(temporal);
    return result;
}

std::optional<ClipSizeEstimate> estimateClipSize(const ClipEncodingProfile& profile,
    QSize outputSize, double sourceFps, double durationSeconds, bool includeAudio,
    const ClipContentFeatures& features) {
    if ((profile.encoder != ClipVideoEncoder::H264 && profile.encoder != ClipVideoEncoder::Mpeg4) ||
        outputSize.width() < 2 || outputSize.height() < 2 || (outputSize.width() & 1) ||
        (outputSize.height() & 1) || !std::isfinite(sourceFps) || sourceFps <= 0.0 ||
        sourceFps > 1000.0 || !std::isfinite(durationSeconds) || durationSeconds <= 0.0 ||
        durationSeconds > 60.0 || !finiteFeatures(features)) return std::nullopt;
    const double multiplier = std::exp(0.55 * (features.spatial - 0.45) +
        0.30 * (features.temporal - 0.25) + 0.35 * (features.grain - 0.20));
    const double coefficient = profile.encoder == ClipVideoEncoder::H264 ? 0.075 * 1.15 : 0.12;
    const double pixels = double(outputSize.width()) * outputSize.height();
    const double videoBits = pixels * sourceFps * durationSeconds * coefficient * multiplier;
    constexpr double kAudioExpectedBitsPerSecond = 192000.0;
    constexpr double kAudioLowerBitsPerSecond = 64000.0;
    constexpr double kAudioUpperBitsPerSecond = 320000.0;
    const double audioBits = includeAudio ? kAudioExpectedBitsPerSecond * durationSeconds : 0.0;
    constexpr double kMp4FixedOverheadBytes = 1024.0;
    const double expected = kMp4FixedOverheadBytes + (videoBits + audioBits) * 1.008 / 8.0;
    const double sigma = profile.encoder == ClipVideoEncoder::H264 ? 0.48 : 0.90;
    const double spread = 1.2815515655 * sigma;
    const double audioLowerBytes = includeAudio ? kAudioLowerBitsPerSecond * durationSeconds / 8.0 : 0.0;
    const double audioUpperBytes = includeAudio ? kAudioUpperBitsPerSecond * durationSeconds / 8.0 : 0.0;
    const double lower = kMp4FixedOverheadBytes * 0.5 +
        videoBits * std::exp(-spread) * 0.5 / 8.0 + audioLowerBytes;
    const double upper = kMp4FixedOverheadBytes * 2.0 +
        videoBits * std::exp(spread) * 1.8 / 8.0 + audioUpperBytes;
    constexpr double maxBytes = double(std::numeric_limits<qint64>::max());
    if (!std::isfinite(lower) || !std::isfinite(expected) || !std::isfinite(upper) ||
        lower < 1.0 || upper >= maxBytes) return std::nullopt;
    return ClipSizeEstimate{qint64(std::floor(lower)), qint64(std::llround(expected)),
                            qint64(std::ceil(upper))};
}

} // namespace mplayer::shell
