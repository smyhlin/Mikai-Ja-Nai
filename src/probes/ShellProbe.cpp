// Live shell probe: builds the ported PlayerShell over the GL video surface,
// plays a test pattern, and writes PNGs so the chrome can be inspected.
//
// The shot has to land before the 2.5 s auto-hide, and a second one after it,
// because "chrome visible" and "chrome auto-hid on idle" are two different
// things to get right and one screenshot cannot show both.

#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include "PlayerShell.hpp"
#include "VideoSurface.hpp"
#include "ProbeRuntime.hpp"

using namespace mplayer;

namespace {

long nonBlackCount(const QImage& image) {
    long nonblack = 0;
    for (int y = 0; y < image.height(); y += 2) {
        for (int x = 0; x < image.width(); x += 2) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() + pixel.green() + pixel.blue() > 30) ++nonblack;
        }
    }
    return nonblack;
}

int visibleChrome(const mplayer::shell::PlayerShell* shell) {
    int count = 0;
    for (const QWidget* child : shell->findChildren<QWidget*>()) {
        if (child->isVisible()) ++count;
    }
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (!mplayer::probes::selectStandardRuntime()) {
        return 2;
    }

    auto* shell = new shell::PlayerShell(nullptr, /*frameless=*/false);
    shell->resize(1280, 760);
    shell->show();

    shell->setMedia(QStringLiteral("C:/temp/probe.mp4"), QStringLiteral("local"),
                    QStringLiteral("1080p"));
    shell->setDuration(600);
    shell->setRate(1.0);
    shell->setVolume(0.6);
    shell->setPlaying(true);
    shell->setAspectRatio(16.0 / 9.0);

    VideoSurface* surface = shell->videoSurface();
    surface->open(QStringLiteral("av://lavfi:testsrc2=size=1280x720:rate=25"));
    QObject::connect(surface, &VideoSurface::positionChanged, shell,
                     [shell](double seconds) { shell->setPosition(seconds); });
    QObject::connect(surface, &VideoSurface::durationChanged, shell,
                     [shell](double seconds) { shell->setDuration(seconds); });

    // Before the 2.5 s auto-hide: every chrome piece must be on screen.
    QTimer::singleShot(2000, [&] {
        const QImage frame = shell->grab().toImage();
        frame.save(QStringLiteral("shell_probe.png"));
        std::fprintf(stderr, "chrome visible widgets: %d\n", visibleChrome(shell));
        std::fprintf(stderr, "video: %dx%d duration=%.1f\n", surface->videoWidth(),
                     surface->videoHeight(), surface->duration());
        std::fprintf(stderr, "nonblack: %ld\n", nonBlackCount(frame));
    });

    // After the idle countdown: the chrome must be gone and the picture remain.
    QTimer::singleShot(4000, [&] {
        const QImage frame = shell->grab().toImage();
        frame.save(QStringLiteral("shell_probe_autohidden.png"));
        std::fprintf(stderr, "after auto-hide, visible widgets: %d\n", visibleChrome(shell));
        std::fprintf(stderr, "state after idle: %s\n", qPrintable(shell->stateName()));
        std::fprintf(stderr, "activePopup: %p\n", (void*)QApplication::activePopupWidget());
        std::fprintf(stderr, "after auto-hide, nonblack: %ld\n", nonBlackCount(frame));
        app.exit(0);
    });

    return app.exec();
}