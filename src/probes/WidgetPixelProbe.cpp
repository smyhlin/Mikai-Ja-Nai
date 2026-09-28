// Pixel proof at the widget layer: the real VideoSurface under Qt's offscreen
// platform (software Mesa GL from opengl32sw.dll, which Qt shipped with),
// playing a test pattern, then grabFramebuffer() and count non-black pixels.
#include <QApplication>
#include <QImage>
#include <QTimer>

#include <cstdarg>
#include <cstdio>
#include <vector>

#include "VideoSurface.hpp"

static void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
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
        say("RESULT: %s\n", nonblack > total / 2 ? "PASS" : "FAIL");
        app.exit(nonblack > total / 2 ? 0 : 1);
    });
    return app.exec();
}