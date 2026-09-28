// Headless pixel proof at the Qt layer.
//
// Qt's offscreen platform gives QOpenGLWidget no real GL, so grabFramebuffer
// returns a null image and proves nothing. The combination that does work
// headlessly is QOffscreenSurface + QOpenGLContext + QOpenGLFramebufferObject,
// and that is what this probe uses. It drives the same MpvHandle and
// MpvRenderContext the shipping VideoSurface drives, renders into a real FBO
// and reads the pixels back, proving the mpv -> GL -> pixels path without a
// compositor.
//
// Results go to a file as well as stderr: console capture is unreliable in
// this build environment.
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QThread>

#include "MpvHandle.hpp"
#include "MpvRenderContext.hpp"

using namespace mplayer;

namespace {

QTextStream g_log;

void note(const QString& line) {
    g_log << line << Qt::endl;
    std::fprintf(stderr, "%s\n", qPrintable(line));
}

}  // namespace

int main(int argc, char** argv) {
    QFile out(QStringLiteral("headless_px.txt"));
    out.open(QIODevice::WriteOnly | QIODevice::Text);
    g_log.setDevice(&out);

    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);

    QGuiApplication app(argc, argv);

    note(QStringLiteral("platform: %1").arg(QGuiApplication::platformName()));

    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create()) {
        note(QStringLiteral("FAIL: no GL context"));
        return 2;
    }
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!context.makeCurrent(&surface)) {
        note(QStringLiteral("FAIL: context not current"));
        return 2;
    }

    QOpenGLFunctions* gl = context.functions();
    note(QStringLiteral("GL: %1")
             .arg(QString::fromLatin1(reinterpret_cast<const char*>(gl->glGetString(GL_VERSION)))));

    const int width = 320;
    const int height = 180;
    QOpenGLFramebufferObject fbo(width, height);
    if (!fbo.isValid()) {
        note(QStringLiteral("FAIL: FBO invalid"));
        return 2;
    }
    note(QStringLiteral("fbo valid, handle=%1").arg(fbo.handle()));

    gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fbo.handle()));
    gl->glViewport(0, 0, width, height);
    gl->glClearColor(0.f, 0.f, 0.f, 1.f);
    gl->glClear(GL_COLOR_BUFFER_BIT);

    auto api = std::make_shared<GlApi>(loadGlApi());
    std::uint64_t lastGeneration = 0;
    int delivered = 0;

    QImage image;
    try {
        MpvHandle mpv({{"vo", "libmpv"}});
        MpvRenderContext rc(mpv, *api);
        rc.setFlipY(false);
        rc.setUpdateCallback([] {});
        mpv.loadFile("av://lavfi:testsrc=size=320x180:rate=25");

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            const std::uint64_t generation = rc.update();
            if (generation != lastGeneration) {
                ++delivered;
                lastGeneration = generation;
            }
            gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fbo.handle()));
            rc.render(static_cast<int>(fbo.handle()), width, height, 0);
            rc.reportSwap();
            QThread::msleep(8);
        }

        gl->glFinish();
        image = fbo.toImage();
    } catch (const MpvError& e) {
        note(QStringLiteral("MpvError: %1").arg(QString::fromUtf8(e.what())));
        return 9;
    }

    long nonblack = 0;
    long total = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            ++total;
            if (pixel.red() + pixel.green() + pixel.blue() > 12) ++nonblack;
        }
    }
    note(QStringLiteral("frames delivered: %1").arg(delivered));
    note(QStringLiteral("image %1x%2 format=%3")
             .arg(image.width())
             .arg(image.height())
             .arg(static_cast<int>(image.format())));
    note(QStringLiteral("nonblack: %1 of %2").arg(nonblack).arg(total));

    if (image.isNull() || total == 0 || nonblack * 2 <= total) {
        note(QStringLiteral("RESULT: FAIL"));
        return 1;
    }
    note(QStringLiteral("RESULT: PASS"));
    return 0;
}