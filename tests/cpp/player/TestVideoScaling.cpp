#include "TestVideoScaling.hpp"

#include "player/core/VideoSurface.hpp"

#include <QLocale>
#include <QSettings>
#include <QTemporaryDir>

#include <cmath>

using mplayer::VideoScaleMode;
using mplayer::VideoScaleOption;
using mplayer::VideoScalePolicy;

namespace {

QString valueOf(const std::vector<VideoScaleOption>& options, const std::string& name) {
    for (const VideoScaleOption& option : options) {
        if (option.name == name) return QString::fromStdString(option.value);
    }
    return QString();
}

bool hasOption(const std::vector<VideoScaleOption>& options, const std::string& name) {
    for (const VideoScaleOption& option : options) {
        if (option.name == name) return true;
    }
    return false;
}

}  // namespace

void TestVideoScaling::eachModeOwnsAllSevenOptions() {
    // A mode that wrote only its own option would keep the previous mode's
    // crop, aspect override or zoom alive.
    const VideoScaleMode modes[] = {
        VideoScaleMode::Fit, VideoScaleMode::Fill, VideoScaleMode::Stretch,
        VideoScaleMode::Aspect16x9, VideoScaleMode::Aspect4x3, VideoScaleMode::Original};
    const char* owned[] = {"keepaspect",          "panscan", "video-aspect-override",
                           "video-unscaled",      "video-zoom", "scale"};
    for (const VideoScaleMode mode : modes) {
        VideoScalePolicy policy;
        policy.mode = mode;
        const auto options = mplayer::videoScaleOptions(policy, std::string("hermite"));
        QCOMPARE(options.size(), std::size_t{7});
        for (const char* name : owned) {
            QVERIFY2(hasOption(options, name), name);
        }
        // Auto downscale still resolves to the core's own default, never to a
        // guessed id and never to the literal "auto" the pinned core refuses.
        QCOMPARE(valueOf(options, "dscale"), QStringLiteral("hermite"));
        QVERIFY(valueOf(options, "dscale") != QStringLiteral("auto"));
    }
}

void TestVideoScaling::fillAndStretchAdjustKeepaspectAndPanscan() {
    VideoScalePolicy fit;
    auto options = mplayer::videoScaleOptions(fit, std::string());
    QCOMPARE(valueOf(options, "keepaspect"), QStringLiteral("yes"));
    QCOMPARE(valueOf(options, "panscan"), QStringLiteral("0.000000"));

    VideoScalePolicy fill;
    fill.mode = VideoScaleMode::Fill;
    options = mplayer::videoScaleOptions(fill, std::string());
    QCOMPARE(valueOf(options, "keepaspect"), QStringLiteral("yes"));
    QCOMPARE(valueOf(options, "panscan"), QStringLiteral("1.000000"));

    VideoScalePolicy stretch;
    stretch.mode = VideoScaleMode::Stretch;
    options = mplayer::videoScaleOptions(stretch, std::string());
    QCOMPARE(valueOf(options, "keepaspect"), QStringLiteral("no"));
    QCOMPARE(valueOf(options, "panscan"), QStringLiteral("0.000000"));
}

void TestVideoScaling::aspectOverridesAndOriginalSize() {
    VideoScalePolicy wide;
    wide.mode = VideoScaleMode::Aspect16x9;
    auto options = mplayer::videoScaleOptions(wide, std::string());
    QCOMPARE(valueOf(options, "video-aspect-override"), QStringLiteral("16:9"));
    QCOMPARE(valueOf(options, "video-unscaled"), QStringLiteral("no"));

    VideoScalePolicy classic;
    classic.mode = VideoScaleMode::Aspect4x3;
    options = mplayer::videoScaleOptions(classic, std::string());
    QCOMPARE(valueOf(options, "video-aspect-override"), QStringLiteral("4:3"));

    // A reset is the literal "no", not a numeric sentinel the core would have
    // to interpret.
    VideoScalePolicy fit;
    options = mplayer::videoScaleOptions(fit, std::string());
    QCOMPARE(valueOf(options, "video-aspect-override"), QStringLiteral("no"));

    VideoScalePolicy original;
    original.mode = VideoScaleMode::Original;
    options = mplayer::videoScaleOptions(original, std::string());
    QCOMPARE(valueOf(options, "video-unscaled"), QStringLiteral("yes"));
    QCOMPARE(valueOf(options, "video-zoom"), QStringLiteral("0.000000"));
}

void TestVideoScaling::zoomUsesLog2OfTheFactor() {
    QCOMPARE(mplayer::videoZoomArgument(0), QStringLiteral("0.000000"));
    QCOMPARE(mplayer::videoZoomArgument(-50), QStringLiteral("-1.000000"));
    QCOMPARE(mplayer::videoZoomArgument(100), QStringLiteral("1.000000"));
    QCOMPARE(mplayer::videoZoomArgument(50), QStringLiteral("0.584963"));
    QCOMPARE(mplayer::videoZoomArgument(-25), QStringLiteral("-0.415037"));
    QVERIFY(std::abs(std::log2(1.5) - 0.5849625007) < 1e-9);
}

void TestVideoScaling::zoomIsClampedToTheCardRange() {
    QCOMPARE(mplayer::clampedVideoZoomPercent(-90), -50);
    QCOMPARE(mplayer::clampedVideoZoomPercent(0), 0);
    QCOMPARE(mplayer::clampedVideoZoomPercent(100), 100);
    QCOMPARE(mplayer::clampedVideoZoomPercent(150), 100);
    // Out-of-range input still reaches mpv as the clamped argument.
    QCOMPARE(mplayer::videoZoomArgument(-90), QStringLiteral("-1.000000"));
    QCOMPARE(mplayer::videoZoomArgument(300), QStringLiteral("1.000000"));
}

void TestVideoScaling::stretchAndOriginalKeepTheStoredZoomOutOfMpv() {
    QCOMPARE(mplayer::effectiveVideoZoomPercent(VideoScaleMode::Stretch, 100), 0);
    QCOMPARE(mplayer::effectiveVideoZoomPercent(VideoScaleMode::Original, 100), 0);
    QCOMPARE(mplayer::effectiveVideoZoomPercent(VideoScaleMode::Fit, 100), 100);
    QCOMPARE(mplayer::effectiveVideoZoomPercent(VideoScaleMode::Fill, -50), -50);

    VideoScalePolicy stretch;
    stretch.mode = VideoScaleMode::Stretch;
    stretch.zoomPercent = 100;
    const auto options = mplayer::videoScaleOptions(stretch, std::string());
    QCOMPARE(valueOf(options, "video-zoom"), QStringLiteral("0.000000"));
}

void TestVideoScaling::zoomFormattingIgnoresTheLocale() {
    // uk_UA formats a double with a comma, and mpv would refuse "0,584963".
    const QLocale previous = QLocale();
    QLocale::setDefault(QLocale(QLocale::Ukrainian, QLocale::Ukraine));
    const QString argument = mplayer::videoZoomArgument(50);
    QCOMPARE(argument, QStringLiteral("0.584963"));
    QVERIFY(!argument.contains(QLatin1Char(',')));
    const VideoScalePolicy policy;
    auto withZoom = policy;
    withZoom.zoomPercent = 50;
    const auto options = mplayer::videoScaleOptions(withZoom, std::string());
    QVERIFY(!valueOf(options, "video-zoom").contains(QLatin1Char(',')));
    // The scale factor the percentage names is intact: 1.5 -> log2 = 0.584963.
    QCOMPARE(QLocale::c().toDouble(argument), 0.584963);
    QLocale::setDefault(previous);
}

void TestVideoScaling::autoDownscaleLeavesTheCoreDefaultAlone() {
    // No queried default: Auto must not invent an id, so dscale stays unwritten
    // and mpv keeps whatever it initialized with.
    VideoScalePolicy policy;
    auto options = mplayer::videoScaleOptions(policy, std::string());
    QVERIFY(!hasOption(options, "dscale"));

    policy.downscale = QStringLiteral("mitchell");
    options = mplayer::videoScaleOptions(policy, std::string("hermite"));
    QCOMPARE(valueOf(options, "dscale"), QStringLiteral("mitchell"));

    policy.upscale = QStringLiteral("ewa_lanczossoft");
    options = mplayer::videoScaleOptions(policy, std::string());
    QCOMPARE(valueOf(options, "scale"), QStringLiteral("ewa_lanczossoft"));
}

void TestVideoScaling::settingsKeysRoundTrip() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("scaling.ini"));
    VideoScalePolicy saved;
    saved.mode = VideoScaleMode::Aspect4x3;
    saved.zoomPercent = 35;
    saved.upscale = QStringLiteral("catmull_rom");
    saved.downscale = QString();
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QString::fromLatin1(mplayer::VideoSurface::kVideoScaleModeSettingKey),
                          static_cast<int>(saved.mode));
        settings.setValue(QString::fromLatin1(mplayer::VideoSurface::kVideoZoomSettingKey),
                          saved.zoomPercent);
        settings.setValue(QString::fromLatin1(mplayer::VideoSurface::kVideoUpscaleSettingKey),
                          saved.upscale);
        settings.setValue(QString::fromLatin1(mplayer::VideoSurface::kVideoDownscaleSettingKey),
                          saved.downscale);
        settings.sync();
    }
    QSettings read(path, QSettings::IniFormat);
    QCOMPARE(read.value(QString::fromLatin1(mplayer::VideoSurface::kVideoScaleModeSettingKey), -1).toInt(),
             static_cast<int>(VideoScaleMode::Aspect4x3));
    QCOMPARE(read.value(QString::fromLatin1(mplayer::VideoSurface::kVideoZoomSettingKey), 0).toInt(),
             35);
    QCOMPARE(read.value(QString::fromLatin1(mplayer::VideoSurface::kVideoUpscaleSettingKey))
                 .toString(),
             QStringLiteral("catmull_rom"));
    QCOMPARE(read.value(QString::fromLatin1(mplayer::VideoSurface::kVideoDownscaleSettingKey))
                 .toString(),
             QString());
    // The keys are the contract with every reader and writer.
    QCOMPARE(QString::fromLatin1(mplayer::VideoSurface::kVideoScaleModeSettingKey),
             QStringLiteral("playback/videoScaleMode"));
    QCOMPARE(QString::fromLatin1(mplayer::VideoSurface::kVideoZoomSettingKey),
             QStringLiteral("playback/videoZoomPercent"));
    QCOMPARE(QString::fromLatin1(mplayer::VideoSurface::kVideoUpscaleSettingKey),
             QStringLiteral("playback/videoScale"));
    QCOMPARE(QString::fromLatin1(mplayer::VideoSurface::kVideoDownscaleSettingKey),
             QStringLiteral("playback/videoDownscale"));
}
