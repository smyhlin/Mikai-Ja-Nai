#include "TestVideoScaling.hpp"

#include "player/core/MpvApi.hpp"
#include "player/core/VideoSurface.hpp"
#include "player/core/VideoSession.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QLocale>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <cmath>
#include <functional>

using mplayer::VideoSession;
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

void TestVideoScaling::videoScalePolicyIsProcessWide() {
    const VideoScalePolicy saved = VideoSession::videoScalePolicy();
    const auto restore = qScopeGuard([&] { VideoSession::setVideoScalePolicy(saved); });

    VideoScalePolicy selected;
    selected.mode = VideoScaleMode::Aspect4x3;
    selected.zoomPercent = 50;
    VideoSession::setVideoScalePolicy(selected);

    const VideoScalePolicy observedByAnotherConsumer = VideoSession::videoScalePolicy();
    QCOMPARE(observedByAnotherConsumer.mode, VideoScaleMode::Aspect4x3);
    QCOMPARE(observedByAnotherConsumer.zoomPercent, 50);
}

void TestVideoScaling::videoScalePolicyLoadsPersistedValues() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = QDir(directory.path()).filePath(QStringLiteral("settings.ini"));
    {
        QSettings persisted(path, QSettings::IniFormat);
        persisted.setValue(QString::fromLatin1(VideoSession::kVideoScaleModeSettingKey),
                           static_cast<int>(VideoScaleMode::Aspect16x9));
        persisted.setValue(QString::fromLatin1(VideoSession::kVideoZoomSettingKey), 50);
        persisted.setValue(QString::fromLatin1(VideoSession::kVideoUpscaleSettingKey),
                           QStringLiteral("ewa_lanczos"));
        persisted.setValue(QString::fromLatin1(VideoSession::kVideoDownscaleSettingKey),
                           QStringLiteral("mitchell"));
        persisted.sync();
        QCOMPARE(persisted.status(), QSettings::NoError);
    }

    QSettings reopened(path, QSettings::IniFormat);
    const VideoScalePolicy restored = mplayer::videoScalePolicyFromSettings(reopened);
    QCOMPARE(restored.mode, VideoScaleMode::Aspect16x9);
    QCOMPARE(restored.zoomPercent, 50);
    QCOMPARE(restored.upscale, QStringLiteral("ewa_lanczos"));
    QCOMPARE(restored.downscale, QStringLiteral("mitchell"));
}

namespace {

double pixelLuma(const QColor& color) {
    return 0.2126 * color.red() + 0.7152 * color.green() + 0.0722 * color.blue();
}

double meanLuma(const QImage& image, QRect rect) {
    rect = rect.intersected(image.rect());
    if (rect.isEmpty()) return -1.0;
    double total = 0.0;
    int count = 0;
    for (int y = rect.top(); y < rect.bottom(); y += 2) {
        for (int x = rect.left(); x < rect.right(); x += 2) {
            total += pixelLuma(image.pixelColor(x, y));
            ++count;
        }
    }
    return count > 0 ? total / count : -1.0;
}

int litWidth(const QImage& image) {
    int lit = 0;
    for (int x = 0; x < image.width(); ++x) {
        if (pixelLuma(image.pixelColor(x, image.height() / 2)) > 16.0) ++lit;
    }
    return lit;
}

bool pumpUntil(const std::function<bool()>& done, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(10);
    }
    return done();
}

bool loadPixelRuntime(QString* reason) {
    if (mplayer::MpvApi::instance().loaded()) return true;
    const QString module = qEnvironmentVariable("MIKAI_MPV_MODULE");
    mplayer::MpvBundle bundle;
    bundle.root = QFileInfo(module).absolutePath();
    bundle.modulePath = QFileInfo(module).absoluteFilePath();
    bundle.descriptorId = QStringLiteral("scale-pixel-runtime");
    bundle.protocolVersion = 1;
    return mplayer::MpvApi::instance().load(bundle, reason);
}

}  // namespace

void TestVideoScaling::hiddenPlaybackChangesThePicturePerMode_data() {
    QTest::addColumn<bool>("paused");
    QTest::newRow("playing") << false;
    QTest::newRow("paused") << true;
}

void TestVideoScaling::hiddenPlaybackChangesThePicturePerMode() {
    QFETCH(bool, paused);
    if (QGuiApplication::platformName() == QLatin1String("offscreen") ||
        QGuiApplication::platformName() == QLatin1String("minimal")) {
        QSKIP("scale pixels need a real GL platform");
    }
    const QString fixture = QStringLiteral(
        MIKAI_SOURCE_DIR "/tests/fixtures/media/motion_320x180_24fps_8bit_hevc.mp4");
    QVERIFY2(QFileInfo::exists(fixture), qPrintable(fixture));
    if (qEnvironmentVariable("MIKAI_MPV_MODULE").isEmpty()) {
        QSKIP("run through the isolated launcher with the pinned mpv runtime");
    }
    QString runtimeError;
    QVERIFY2(loadPixelRuntime(&runtimeError), qPrintable(runtimeError));

    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const QString outputRoot = qEnvironmentVariable("MIKAI_SCALE_PIXEL_OUTPUT");
    const QString outDir = outputRoot.isEmpty()
                               ? scratch.path()
                               : QDir(outputRoot).filePath(QString::fromLatin1(QTest::currentDataTag()));
    if (!outputRoot.isEmpty()) {
        QVERIFY(QFileInfo(outputRoot).isAbsolute());
        QVERIFY2(!QFileInfo::exists(outDir), qPrintable(outDir));
        QVERIFY(QDir().mkpath(outDir));
    }
    QFile log(QDir(outDir).filePath(QStringLiteral("log.txt")));
    QVERIFY2(log.open(QIODevice::WriteOnly | QIODevice::NewOnly), qPrintable(log.errorString()));
    const auto note = [&](const QString& line) {
        const QByteArray text = line.toUtf8() + '\n';
        QCOMPARE(log.write(text), qint64(text.size()));
        QVERIFY(log.flush());
    };
    note(QStringLiteral("platform=%1 paused=%2").arg(QGuiApplication::platformName()).arg(paused));

    const mplayer::VideoScalePolicy saved = mplayer::VideoSession::videoScalePolicy();
    const auto restore = qScopeGuard([&] { mplayer::VideoSession::setVideoScalePolicy(saved); });
    mplayer::VideoSession::setVideoScalePolicy(mplayer::VideoScalePolicy{});

    // Keep this pixel test on the Widgets renderer adapter without opening the
    // main player's window.
    mplayer::VideoSurface surface;
    surface.setAttribute(Qt::WA_DontShowOnScreen, true);
    surface.setAttribute(Qt::WA_ShowWithoutActivating, true);
    surface.setWindowFlag(Qt::Tool);
    surface.resize(960, 400);
    surface.show();
    QVERIFY2(surface.session()->open(QDir::toNativeSeparators(fixture)),
             "the hidden surface could not create an engine");

    QImage first;
    const bool framed = pumpUntil(
        [&] {
            if (surface.session()->videoWidth() != 320 ||
                surface.session()->videoHeight() != 180) {
                return false;
            }
            first = surface.grabVideoFramebuffer();
            return !first.isNull() && first.width() > 0 &&
                   meanLuma(first, QRect(first.width() / 2 - 24, first.height() / 2 - 16, 48, 32)) >
                       20.0;
        },
        8000);
    note(QStringLiteral("framed=%1 size=%2x%3 video=%4x%5 grab=%6x%7")
             .arg(framed ? 1 : 0)
             .arg(surface.width())
             .arg(surface.height())
             .arg(surface.session()->videoWidth())
             .arg(surface.session()->videoHeight())
             .arg(first.width())
             .arg(first.height()));
    QVERIFY2(framed, "the native hidden surface did not render the fixture");
    QVERIFY(surface.width() >= 200 && surface.height() >= 120);
    if (paused) {
        surface.session()->setPaused(true);
        QVERIFY(pumpUntil([&] { return !surface.session()->isPlaying(); }, 2000));
    }
    const auto grabSettled = [&] {
        QImage image;
        const quint64 before = surface.session()->playbackDiagnostics().renderedOutputFrames;
        const bool advanced = pumpUntil(
            [&] {
                image = surface.grabVideoFramebuffer();
                const double center = meanLuma(
                    image, QRect(image.width() / 2 - 24, image.height() / 2 - 16, 48, 32));
                return surface.session()->playbackDiagnostics().renderedOutputFrames > before &&
                       !image.isNull() && center > 20.0;
            },
            3000);
        return advanced ? image : QImage();
    };
    const auto grabMatching = [&](const std::function<bool(const QImage&)>& matches) {
        QImage image;
        const bool matched = pumpUntil(
            [&] {
                image = surface.grabVideoFramebuffer();
                return !image.isNull() && matches(image);
            },
            2500);
        return matched ? image : QImage();
    };
    const auto apply = [&](mplayer::VideoScaleMode mode, int zoom,
                           const std::function<bool(const QImage&)>& matches) {
        mplayer::VideoScalePolicy policy;
        policy.mode = mode;
        policy.zoomPercent = zoom;
        mplayer::VideoSession::setVideoScalePolicy(policy);
        return grabMatching(matches);
    };

    const QImage fitted = paused ? surface.grabVideoFramebuffer() : grabSettled();
    QVERIFY(!fitted.isNull());
    const int fitSpan = litWidth(fitted);
    const double fitCenter =
        meanLuma(fitted, QRect(fitted.width() / 2 - 24, fitted.height() / 2 - 16, 48, 32));
    QVERIFY2(fitCenter > 20.0, qPrintable(QStringLiteral("fit center luma %1").arg(fitCenter)));
    QVERIFY2(fitSpan > 0 && fitSpan < fitted.width() * 9 / 10,
             qPrintable(QStringLiteral("fit span %1 of %2").arg(fitSpan).arg(fitted.width())));

    const auto fillsWidth = [](const QImage& image) {
        return litWidth(image) > image.width() * 9 / 10;
    };
    const QImage filled = apply(mplayer::VideoScaleMode::Fill, 0, fillsWidth);
    QVERIFY(!filled.isNull());
    const int fillSpan = litWidth(filled);
    QVERIFY2(fillSpan > fitSpan && fillSpan > filled.width() * 9 / 10,
             qPrintable(QStringLiteral("fill span %1 fit %2 width %3")
                            .arg(fillSpan)
                            .arg(fitSpan)
                            .arg(filled.width())));

    const QImage stretched = apply(mplayer::VideoScaleMode::Stretch, 0, fillsWidth);
    QVERIFY(!stretched.isNull());
    QVERIFY2(litWidth(stretched) > stretched.width() * 9 / 10, "stretch leaves a side bar");

    const QImage original = apply(mplayer::VideoScaleMode::Original, 0, [](const QImage& image) {
        return std::abs(litWidth(image) - 320) <= 2;
    });
    QVERIFY(!original.isNull());
    const int originalSpan = litWidth(original);
    QVERIFY2(originalSpan > 0 && originalSpan < fitSpan,
             qPrintable(QStringLiteral("original span %1 fit %2").arg(originalSpan).arg(fitSpan)));

    const QImage classic = apply(mplayer::VideoScaleMode::Aspect4x3, 0, [&](const QImage& image) {
        return std::abs(litWidth(image) - fitSpan * 3 / 4) < 8;
    });
    QVERIFY(!classic.isNull());
    const int classicSpan = litWidth(classic);
    QVERIFY2(classicSpan > 0 && classicSpan < fitSpan,
             qPrintable(QStringLiteral("4:3 span %1 fit %2").arg(classicSpan).arg(fitSpan)));

    const auto fitsWidth = [&](const QImage& image) {
        return std::abs(litWidth(image) - fitSpan) < 8;
    };
    const QImage wide = apply(mplayer::VideoScaleMode::Aspect16x9, 0, fitsWidth);
    QVERIFY(!wide.isNull());
    const int wideSpan = litWidth(wide);
    QVERIFY2(std::abs(wideSpan - fitSpan) < fitted.width() / 10,
             qPrintable(QStringLiteral("16:9 span %1 fit %2").arg(wideSpan).arg(fitSpan)));

    const QImage zoomed = apply(mplayer::VideoScaleMode::Fit, 50, fillsWidth);
    QVERIFY(!zoomed.isNull());
    const int zoomSpan = litWidth(zoomed);
    QVERIFY2(zoomSpan > fitSpan,
             qPrintable(QStringLiteral("zoom span %1 fit %2").arg(zoomSpan).arg(fitSpan)));

    const QImage filledAgain = apply(mplayer::VideoScaleMode::Fill, 0, fillsWidth);
    QVERIFY(!filledAgain.isNull());
    auto lock = surface.session()->acquireVideoScaleFitLock();
    QVERIFY(lock.has_value());
    QVERIFY(surface.session()->compareFitHeld());
    const QImage locked = grabMatching(fitsWidth);
    QVERIFY(!locked.isNull());
    const int lockedSpan = litWidth(locked);
    QVERIFY2(lockedSpan < fillSpan && std::abs(lockedSpan - fitSpan) < fitted.width() / 8,
             qPrintable(QStringLiteral("fit-lock span %1 fit %2 fill %3")
                            .arg(lockedSpan)
                            .arg(fitSpan)
                            .arg(fillSpan)));

    note(QStringLiteral("spans fit=%1 fill=%2 original=%3 classic=%4 wide=%5 zoom=%6 lock=%7")
             .arg(fitSpan)
             .arg(fillSpan)
             .arg(originalSpan)
             .arg(classicSpan)
             .arg(wideSpan)
             .arg(zoomSpan)
             .arg(lockedSpan));
    QVERIFY(fitted.save(QDir(outDir).filePath(QStringLiteral("fit.png"))));
    QVERIFY(filled.save(QDir(outDir).filePath(QStringLiteral("fill.png"))));
    QVERIFY(stretched.save(QDir(outDir).filePath(QStringLiteral("stretch.png"))));
    QVERIFY(original.save(QDir(outDir).filePath(QStringLiteral("original.png"))));
    QVERIFY(classic.save(QDir(outDir).filePath(QStringLiteral("aspect-4x3.png"))));
    QVERIFY(wide.save(QDir(outDir).filePath(QStringLiteral("aspect-16x9.png"))));
    QVERIFY(zoomed.save(QDir(outDir).filePath(QStringLiteral("zoom-150.png"))));
    QVERIFY(locked.save(QDir(outDir).filePath(QStringLiteral("compare-fit-lock.png"))));
    lock.reset();
    const QImage restored = grabMatching(fillsWidth);
    QVERIFY2(!restored.isNull(), "closing Compare did not restore the selected Fill mode");
    QVERIFY(restored.save(QDir(outDir).filePath(QStringLiteral("compare-released-fill.png"))));
    QCOMPARE(surface.session()->isPlaying(), !paused);

    // A source transition must replace the prior 16:9 geometry: Fit follows
    // the portrait stream instead of retaining a previous aspect override.
    mplayer::VideoScalePolicy fitPolicy;
    VideoSession::setVideoScalePolicy(fitPolicy);
    const QString portraitFixture = QStringLiteral(
        MIKAI_SOURCE_DIR "/tests/fixtures/media/portrait_motion_1080x1920_24fps_8bit_hevc.mp4");
    QVERIFY2(QFileInfo::exists(portraitFixture), qPrintable(portraitFixture));
    QVERIFY2(surface.session()->open(QDir::toNativeSeparators(portraitFixture)),
             "the same session could not open the portrait fixture");
    QImage portrait;
    const bool portraitFramed = pumpUntil(
        [&] {
            if (surface.session()->videoWidth() != 1080 ||
                surface.session()->videoHeight() != 1920) {
                return false;
            }
            portrait = surface.grabVideoFramebuffer();
            return !portrait.isNull() &&
                   meanLuma(portrait, QRect(portrait.width() / 2 - 20,
                                            portrait.height() / 2 - 20, 40, 40)) > 20.0;
        },
        8000);
    QVERIFY2(portraitFramed, "the portrait stream did not render in the existing session");
    QVERIFY2(litWidth(portrait) < portrait.width() / 2,
             qPrintable(QStringLiteral("portrait stream filled width: %1 of %2")
                            .arg(litWidth(portrait))
                            .arg(portrait.width())));
    surface.hide();
}
