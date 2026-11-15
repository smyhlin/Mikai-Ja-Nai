#include "TestClipSizeEstimate.hpp"

#include "player/shell/ClipSizeEstimate.hpp"

#include <QImage>
#include <cmath>
#include <limits>

using namespace mplayer::shell;

void TestClipSizeEstimate::analyzesImageAndSummarizesAdjacentMotion() {
    QImage image(320, 180, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
            row[x] = qRgb((x * 13 + y * 7) & 255, (x * 5) & 255, (y * 11) & 255);
    }
    auto first = analyzeClipFrame(image);
    QVERIFY(first.has_value());
    QImage changed = image;
    changed.fill(Qt::white);
    auto second = analyzeClipFrame(changed);
    QVERIFY(second.has_value());
    const ClipAnalysisFrame frames[] = {*first, *second};
    const double times[] = {1.0, 1.02};
    const auto summary = summarizeClipSamples(frames, times, 30.0);
    QCOMPARE(summary.sampleCount, 2);
    QCOMPARE(summary.temporalPairCount, 1);
    QVERIFY(summary.temporal > 0.0);
    QVERIFY(summary.spatial > 0.0);
}

void TestClipSizeEstimate::ignoresSpacedSamplesForTemporalMotion() {
    ClipAnalysisFrame frames[2];
    frames[0].grayscale.fill(0);
    frames[1].grayscale.fill(255);
    frames[0].spatial = frames[1].spatial = 0.45;
    frames[0].grain = frames[1].grain = 0.20;
    const double times[] = {0.0, 0.5};
    const auto summary = summarizeClipSamples(frames, times, 30.0);
    QCOMPARE(summary.temporalPairCount, 0);
    QCOMPARE(summary.temporal, 0.25);
}

void TestClipSizeEstimate::rejectsUnknownAndNonFiniteInputs() {
    const ClipEncodingProfile profile{ClipVideoEncoder::H264, "libx264", "preset=veryfast,crf=23,profile=high"};
    ClipContentFeatures features;
    features.sampleCount = 3;
    QVERIFY(!estimateClipSize(profile, QSize(0, 720), 30, 10, true, features));
    QVERIFY(!estimateClipSize(profile, QSize(1280, 720), std::numeric_limits<double>::quiet_NaN(), 10, true, features));
    QVERIFY(!estimateClipSize(profile, QSize(1280, 720), 30, std::numeric_limits<double>::infinity(), true, features));
    features.spatial = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!estimateClipSize(profile, QSize(1280, 720), 30, 10, true, features));
    const double badTimes[] = {0.0, std::numeric_limits<double>::infinity()};
    ClipAnalysisFrame frames[2];
    QCOMPARE(summarizeClipSamples(frames, badTimes, 30).sampleCount, 0);
}

void TestClipSizeEstimate::estimatesActualEncoderProfilesAndAudioChanges() {
    ClipContentFeatures features;
    features.sampleCount = 4;
    const ClipEncodingProfile h264{ClipVideoEncoder::H264, "libx264", "preset=veryfast,crf=23,profile=high"};
    const ClipEncodingProfile mpeg4{ClipVideoEncoder::Mpeg4, "mpeg4", "flags=+qscale,global_quality=472"};
    const auto h264Audio = estimateClipSize(h264, QSize(1280, 720), 30, 12, true, features);
    const auto h264Muted = estimateClipSize(h264, QSize(1280, 720), 30, 12, false, features);
    const auto mpeg4Audio = estimateClipSize(mpeg4, QSize(1280, 720), 30, 12, true, features);
    QVERIFY(h264Audio && h264Muted && mpeg4Audio);
    QVERIFY(h264Muted->expectedBytes > 0);
    QVERIFY(h264Audio->lowerBytes < h264Audio->expectedBytes);
    QVERIFY(h264Audio->expectedBytes < h264Audio->upperBytes);
    QVERIFY(h264Audio->expectedBytes > h264Muted->expectedBytes);
    QVERIFY(mpeg4Audio->lowerBytes < mpeg4Audio->expectedBytes);
    QVERIFY(mpeg4Audio->expectedBytes < mpeg4Audio->upperBytes);
    QVERIFY(mpeg4Audio->expectedBytes > 0);
    QVERIFY(mpeg4Audio->upperBytes > h264Audio->upperBytes);
}

void TestClipSizeEstimate::tinyVideoGetsBroadUncalibratedAudioBounds() {
    const ClipEncodingProfile profile{ClipVideoEncoder::H264, "libx264", "preset=veryfast,crf=23,profile=high"};
    ClipContentFeatures features;
    features.sampleCount = 1;
    const auto silent = estimateClipSize(profile, QSize(16, 16), 1.0, 1.0, false, features);
    const auto audio = estimateClipSize(profile, QSize(16, 16), 1.0, 1.0, true, features);
    QVERIFY(silent && audio);
    QVERIFY(audio->lowerBytes > silent->lowerBytes);
    QVERIFY(audio->upperBytes > silent->upperBytes);
    QVERIFY(audio->upperBytes - audio->lowerBytes > silent->upperBytes - silent->lowerBytes);
    QVERIFY(audio->lowerBytes < audio->expectedBytes);
    QVERIFY(audio->expectedBytes < audio->upperBytes);
}

void TestClipSizeEstimate::sharesExactOutputGeometry() {
    QCOMPARE(clipOutputSize(QSize(1920, 1080), 720).value(), QSize(1280, 720));
    QCOMPARE(clipOutputSize(QSize(640, 480), 1080).value(), QSize(640, 480));
    QCOMPARE(clipOutputSize(QSize(1000, 700), 480).value(), QSize(686, 480));
    QVERIFY(!clipOutputSize(QSize(1920, 1080), 600));
}
