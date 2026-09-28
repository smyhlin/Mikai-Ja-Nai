#include <QApplication>
#include <QTimer>
#include <cstdio>
#include "VideoSurface.hpp"
static void say(const char* s){ std::printf("%s\n", s); std::fflush(stdout); }
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    say("1: app created");
    mplayer::VideoSurface surface;
    surface.resize(640, 360);
    surface.show();
    say("2: surface shown");
    QObject::connect(&surface, &mplayer::VideoSurface::errorOccurred,
                     [](const QString& m){ std::printf("error: %s\n", m.toUtf8().constData()); std::fflush(stdout); });
    QObject::connect(&surface, &mplayer::VideoSurface::playingChanged, [](bool p){
        std::printf("playingChanged: %s\n", p?"PLAYING":"paused"); std::fflush(stdout); });
    QObject::connect(&surface, &mplayer::VideoSurface::durationChanged, [](double d){
        std::printf("durationChanged: %.1f\n", d); std::fflush(stdout); });
    say("3: signals wired");
    if (!surface.open("av://lavfi:testsrc=size=640x360:rate=25")) { say("4: open FAILED"); return 1; }
    say("4: open() ok");
    QTimer::singleShot(5000, &app, [&]{
        say("5: quit now");
        app.quit();
    });
    int rc = app.exec();
    say("6: app.exec done");
    return rc;
}
