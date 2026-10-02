// Pixel proof at the widget layer: the real VideoSurface under Qt's offscreen
// platform (software Mesa GL from opengl32sw.dll, which Qt shipped with),
// playing a test pattern, then grabFramebuffer() and count non-black pixels.
//
// The framebuffer that proves the paint path is also written to disk, in the
// grade mpv's own renderer produced (no re-render, no format swap), so the
// nonblack count and the saved PNG are the same pixels.
#include <QApplication>
#include <QImage>
#include <QTimer>

#include <cstdarg>
#include <cstdio>
#include <vector>

#include "VideoSurface.hpp"
#include "ProbeRuntime.hpp"

static void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (!mplayer::probes::selectStandardRuntime()) {
        return 2;
    }
    // Optional output path for the proof frame: argv[1] when given, else a
    // fixed name in the current directory. The nonblack count below and the
    // written PNG describe the same framebuffer, so echoing the path back lets
    // the caller open exactly the image that was measured.
    const QString outPath = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                     : QStringLiteral("widget_pixel_probe.png");
    mplayer::VideoSurface surface;
    surface.resize(320, 180);
    surface.show();
    surface.open("av://lavfi:testsrc2=size=320x180:rate=25");
    surface.setSpeed(1.0);

    QTimer* pump = new QTimer(nullptr);
    QObject::connect(pump, &QTimer::timeout, &surface, [&] {
        surface.update();
    });
    pump->start(16);

    QTimer::singleShot(4000, &app, [&] {
        // A black frame and an absent renderer look identical in the pixel
        // count, so the two facts that separate them are printed beside it.
        const QSurfaceFormat format = surface.format();
        say("GL surface format=%d.%d profile=%d samples=%d\n", format.majorVersion(),
            format.minorVersion(), int(format.profile()), format.samples());
        say("hasVideo=%d playing=%d size=%dx%d\n", int(surface.hasVideo()),
            int(surface.isPlaying()), surface.videoWidth(), surface.videoHeight());
        QImage frame = surface.grabFramebuffer();
        say("grabFramebuffer: %dx%d fmt=%d\n", frame.width(), frame.height(), int(frame.format()));
        if (frame.isNull()) {
            say("RESULT: NULL IMAGE (paint path broken)\n");
            app.exit(2);
            return;
        }
        int nonblack = 0, total = 0;
        for (int y = 0; y < frame.height(); y += 3) {
            const uchar* line = frame.constScanLine(y);
            for (int x = 0; x < frame.width(); x += 3) {
                total++;
                // RGBA or RGB(A): treat byte0+byte1+byte2
                int sum = line[x * 4] + line[x * 4 + 1] + line[x * 4 + 2];
                if (sum > 24) nonblack++;
            }
        }
        say("pixels: nonblack=%d/%d (%.0f%%)\n", nonblack, total, 100.0 * nonblack / total);
        // The proof frame is the exact framebuffer above, saved as mpv's
        // renderer handed it over: no re-render and no format conversion, so
        // the picture on disk is the one that was just counted. A save that did
        // not write is a failure, not a silent absence of evidence.
        const bool saved = !outPath.isEmpty() && frame.save(outPath);
        say("screenshot: %s -> %s\n", outPath.toUtf8().constData(), saved ? "written" : "FAILED");
        const bool pass = nonblack > total / 2 && saved;
        say("RESULT: %s\n", pass ? "PASS" : "FAIL");
        app.exit(pass ? 0 : 1);
    });
    return app.exec();
}