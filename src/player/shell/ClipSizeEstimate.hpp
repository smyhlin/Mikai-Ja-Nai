#pragma once

#include <QImage>
#include <QSize>
#include <QString>

#include <array>
#include <optional>
#include <span>

namespace mplayer::shell {

enum class ClipVideoEncoder { H264, Mpeg4 };

struct ClipEncodingProfile {
    ClipVideoEncoder encoder;
    const char* codec;
    const char* videoOptions;
};

bool probeClipEncodingProfile(ClipEncodingProfile& profile, QString& error);
std::optional<QSize> clipOutputSize(QSize sourceSize, int maximumHeight);

struct ClipContentFeatures {
    int sampleCount = 0;
    int temporalPairCount = 0;
    double spatial = 0.0;
    double temporal = 0.25;
    double grain = 0.0;
    double edgeDensity = 0.0;
    double entropy = 0.0;
};

struct ClipAnalysisFrame {
    std::array<unsigned char, 160 * 90> grayscale{};
    double spatial = 0.0;
    double grain = 0.0;
    double edgeDensity = 0.0;
    double entropy = 0.0;
};

std::optional<ClipAnalysisFrame> analyzeClipFrame(const QImage& image);
ClipContentFeatures summarizeClipSamples(std::span<const ClipAnalysisFrame> frames,
                                         std::span<const double> times,
                                         double sourceFps);

struct ClipSizeEstimate {
    qint64 lowerBytes;
    qint64 expectedBytes;
    qint64 upperBytes;
};

std::optional<ClipSizeEstimate> estimateClipSize(const ClipEncodingProfile& profile,
    QSize outputSize, double sourceFps, double durationSeconds, bool includeAudio,
    const ClipContentFeatures& features);

}  // namespace mplayer::shell
