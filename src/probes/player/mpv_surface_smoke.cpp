#include <QApplication>
#include <QColor>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include "ProbeRuntime.hpp"
#include "VideoSurface.hpp"

namespace {

// The media clock, source geometry and health are retained with the picture:
// after EOF the surface resets its own fields, and a sample without them cannot
// be banded, paired or trusted.
struct FrameSample {
    QImage image;
    double position = -1.0;
    int videoWidth = 0;
    int videoHeight = 0;
    bool contextReady = false;
    bool clean = false;
    bool valid = false;
};

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }

    mplayer::VideoSurface surface;
    surface.resize(640, 360);
    std::optional<double> streamFps;
    auto streamFpsSource = mplayer::StreamFpsSource::Unknown;
    QString mediaError;
    QObject::connect(&surface, &mplayer::VideoSurface::errorOccurred,
                     [&](const QString& message) {
                         mediaError = message;
                         std::fprintf(stderr, "media error: %s\n", qPrintable(message));
                     });
    QObject::connect(&surface, &mplayer::VideoSurface::streamStatsChanged,
                     [&](double, double fps, mplayer::StreamFpsSource source) {
                         streamFps = fps;
                         streamFpsSource = source;
                     });
    surface.show();
    const QString target = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                    : QStringLiteral("av://lavfi:testsrc=size=640x360:rate=25");
    std::fprintf(stderr, "target=%s\n", qPrintable(target));
    if (!surface.open(target)) {
        std::fprintf(stdout, "RESULT: FAIL (open refused)\n");
        return 1;
    }

    const auto grabSample = [&](double position) {
        FrameSample sample;
        sample.image = surface.grabFramebuffer();
        sample.position = position;
        sample.videoWidth = surface.videoWidth();
        sample.videoHeight = surface.videoHeight();
        sample.contextReady = surface.isContextReady();
        sample.clean = mediaError.isEmpty();
        sample.valid = !sample.image.isNull();
        return sample;
    };

    FrameSample early;
    FrameSample lastLive;
    bool done = false;
    QTimer timer;
    timer.setInterval(100);

    const auto finish = [&]() {
        if (done) return;
        done = true;
        timer.stop();

        const bool havePair = early.valid && lastLive.valid && lastLive.position > early.position;
        const FrameSample& shown = havePair ? lastLive : early;
        const QSize pictureSize =
            shown.videoWidth > 0 && shown.videoHeight > 0
                ? QSize(shown.videoWidth, shown.videoHeight)
                      .scaled(shown.image.size(), Qt::KeepAspectRatio)
                : QSize();
        const QRect picture(QPoint((shown.image.width() - pictureSize.width()) / 2,
                                   (shown.image.height() - pictureSize.height()) / 2),
                            pictureSize);

        int nonblack = 0;
        int total = 0;
        int changed = 0;
        int compared = 0;
        if (!shown.image.isNull() && picture.isValid()) {
            for (int y = picture.top(); y <= picture.bottom(); ++y) {
                for (int x = picture.left(); x <= picture.right(); ++x) {
                    const QColor pixel = shown.image.pixelColor(x, y);
                    ++total;
                    // 0x080808 sums to 24: legitimate near-black content stays lit.
                    if (pixel.red() + pixel.green() + pixel.blue() > 12) ++nonblack;
                    if (havePair) {
                        const QColor before = early.image.pixelColor(x, y);
                        ++compared;
                        if (std::abs(pixel.red() - before.red()) +
                                std::abs(pixel.green() - before.green()) +
                                std::abs(pixel.blue() - before.blue()) >
                            8) {
                            ++changed;
                        }
                    }
                }
            }
        }

        const bool captured =
            !shown.image.isNull() && shown.videoWidth > 0 && shown.videoHeight > 0;
        const bool clean = mediaError.isEmpty() && early.clean && lastLive.clean &&
                           early.contextReady && lastLive.contextReady;
        const bool saved =
            !shown.image.isNull() && shown.image.save(QStringLiteral("mpv_surface_smoke.png"));

        std::fprintf(stderr,
                     "hasVideo=%d playing=%d contextReady=%d video=%dx%d "
                     "position=%.3f earlierPosition=%.3f duration=%.3f\n",
                     int(surface.hasVideo()), int(surface.isPlaying()),
                     int(surface.isContextReady()), shown.videoWidth, shown.videoHeight,
                     surface.position(), early.valid ? early.position : -1.0,
                     surface.duration());
        if (streamFps.has_value()) {
            const char* provenance =
                streamFpsSource == mplayer::StreamFpsSource::MpvOutput
                    ? "mpv output"
                    : streamFpsSource == mplayer::StreamFpsSource::Container
                          ? "container source"
                          : "unknown";
            std::fprintf(stderr, "reported stream FPS=%.3f (%s)\n", *streamFps, provenance);
        } else {
            std::fprintf(stderr, "reported stream FPS=unavailable\n");
        }
        const mplayer::PlaybackDiagnostics diagnostics = surface.playbackDiagnostics();
        if (diagnostics.estimatedVfFps.has_value()) {
            std::fprintf(stderr, "raw estimated-vf-fps=%.3f\n", *diagnostics.estimatedVfFps);
        } else {
            std::fprintf(stderr, "raw estimated-vf-fps=unavailable\n");
        }
        if (diagnostics.decoderDropCount.has_value()) {
            std::fprintf(stderr, "decoder-frame-drop-count=%lld\n",
                         static_cast<long long>(*diagnostics.decoderDropCount));
        } else {
            std::fprintf(stderr, "decoder-frame-drop-count=unavailable (named core limitation, "
                                 "not a measured zero)\n");
        }
        if (diagnostics.voDropCount.has_value()) {
            std::fprintf(stderr, "frame-drop-count (VO drops)=%lld\n",
                         static_cast<long long>(*diagnostics.voDropCount));
        } else {
            std::fprintf(stderr, "frame-drop-count (VO drops)=unavailable (named core "
                                 "limitation, not a measured zero)\n");
        }
        if (diagnostics.nativeFilterPresent.has_value()) {
            std::fprintf(stderr, "native filter in the live vf chain=%s (%d entries)\n",
                         *diagnostics.nativeFilterPresent ? "present" : "absent",
                         diagnostics.vfCount.value_or(-1));
        } else {
            std::fprintf(stderr, "native filter in the live vf chain=unknown (not read)\n");
        }
        std::fprintf(stderr,
                     "framebuffer=%dx%d picture band=%dx%d at %d,%d nonblack=%d/%d "
                     "motion=%d/%d PNG=%s\n",
                     shown.image.width(), shown.image.height(), pictureSize.width(),
                     pictureSize.height(), picture.x(), picture.y(), nonblack, total, changed,
                     compared, saved ? "written" : "FAILED");
        if (!mediaError.isEmpty()) {
            std::fprintf(stderr, "media error: %s\n", qPrintable(mediaError));
        }

        const bool motion = compared > 0 && changed * 100 >= compared;
        int exitCode = 1;
        if (!havePair) {
            std::fprintf(stdout, "RESULT: FAIL (no live picture pair)\n");
        } else if (!clean) {
            std::fprintf(stdout, "RESULT: FAIL (media or context failed while sampling)\n");
        } else if (!captured || total == 0) {
            std::fprintf(stdout, "RESULT: FAIL (no picture geometry)\n");
        } else if (!saved) {
            std::fprintf(stdout, "RESULT: FAIL (PNG not written)\n");
        } else if (nonblack * 2 <= total) {
            // Frames did arrive, so a black band is either valid black content
            // or a renderer that painted nothing; this probe cannot tell them apart.
            std::fprintf(stdout, "RESULT: SKIP (black picture: valid black content and a "
                                 "failed render are indistinguishable)\n");
            exitCode = 77;
        } else if (!motion) {
            std::fprintf(stdout, "RESULT: SKIP (no inter-frame motion: static content)\n");
            exitCode = 77;
        } else if (!diagnostics.nativeFilterPresent.has_value()) {
            std::fprintf(stdout, "RESULT: SKIP (vf chain not read: native absence not provable)\n");
            exitCode = 77;
        } else if (*diagnostics.nativeFilterPresent) {
            std::fprintf(stdout, "RESULT: FAIL (native filter present on a standard core)\n");
            exitCode = 1;
        } else {
            std::fprintf(stdout, "RESULT: PASS\n");
            exitCode = 0;
        }
        std::fflush(stdout);

        surface.hide();
        surface.close();
        app.exit(exitCode);
    };

    QObject::connect(&timer, &QTimer::timeout, &surface, [&]() {
        if (done || !surface.isContextReady()) return;
        if (!surface.hasVideo()) {
            // End of file after two live samples is not a failure; a short clip
            // arrives here instead of being read past its end on a fixed clock.
            if (early.valid && lastLive.valid && lastLive.position > early.position) finish();
            return;
        }
        const double position = surface.position();
        const double duration = surface.duration();
        if (!early.valid) {
            const double earlyAt =
                duration > 0.0 ? std::min(0.5, std::max(0.1, duration * 0.15)) : 0.5;
            if (position >= earlyAt) early = grabSample(position);
            return;
        }
        const double advance =
            duration > 0.0 ? std::min(2.0, std::max(0.3, duration * 0.5)) : 2.0;
        if (position >= early.position + advance) {
            lastLive = grabSample(position);
            finish();
            return;
        }
        // A recent live frame keeps a short clip's pre-EOF sample available.
        if (!lastLive.valid || position - lastLive.position >= 0.2) {
            lastLive = grabSample(position);
        }
    });

    QTimer::singleShot(9000, &surface, [&] { finish(); });
    timer.start();
    return app.exec();
}
