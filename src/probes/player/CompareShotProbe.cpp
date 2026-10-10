#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <cstdio>

#include "CompareCanvas.hpp"
#include "CompareWindow.hpp"
#include "PlayerDialog.hpp"
#include "PlayerShell.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSurface.hpp"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace mplayer;
using namespace mplayer::shell;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

class Host : public PlayerHost {
public:
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestOpenOriginal() override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    void playbackFailed() override {}
};

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // Before the first PlayerDialog reads player/anime4k and
    // player/presetShortcuts, or the probe inherits a real session.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiShotProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("CompareShotProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }
    static Host host;
    const QString url = QStringLiteral("av://lavfi:testsrc2=size=1280x720:rate=25");
    const QList<QualityEntry> qualities = {{480, url}, {720, url}, {1080, url}};

    auto* dialog = new PlayerDialog(QStringLiteral("shot"), url, qualities, 720, &host);
    dialog->setVariants({QVariantMap{{"label", QStringLiteral("v")},
                                    {"embed", QStringLiteral("e1")}}},
                        QStringLiteral("e1"));
    dialog->startPlayer();
    dialog->showFullScreen();

    QTimer::singleShot(5000, &app, [&] {
        auto* shell = dialog->findChild<PlayerShell*>();
        if (shell != nullptr) {
            emit shell->compareRequested();
        }
    });

    QTimer::singleShot(11000, &app, [&] {
        auto* compare = dialog->findChild<CompareWindow*>();
        check(compare != nullptr, "the compare row opened a window");
        if (compare == nullptr) {
            app.exit(1);
            return;
        }
        compare->resize(1280, 760);
        CompareCanvas* canvas = compare->findChild<CompareCanvas*>(QString::fromLatin1("compareCanvas"));
        check(canvas != nullptr, "the window has a canvas");
        if (canvas == nullptr) {
            app.exit(1);
            return;
        }

        const QImage image = canvas->grab().toImage().convertToFormat(QImage::Format_RGB888);
        image.save(QStringLiteral("compare_canvas.png"));
        std::fprintf(stderr, "     canvas %dx%d\n", image.width(), image.height());

        int varied = 0;
        int sampled = 0;
        for (int y = 0; y < image.height(); y += 4) {
            for (int x = 0; x < image.width(); x += 4) {
                ++sampled;
                const int lightness = image.pixelColor(x, y).lightness();
                if (lightness > 12 && lightness < 243) ++varied;
            }
        }
        const double share = sampled > 0 ? 100.0 * varied / sampled : 0.0;
        std::fprintf(stderr, "     canvas has picture: %.1f%% of samples in range\n", share);
        check(share > 20.0, "the canvas shows an actual picture, not a blank panel");

        compare->grab().save(QStringLiteral("compare_shot.png"));
        std::fprintf(stderr, "     compare_shot.png saved from the visible window\n");

        // WM_SYSCOMMAND/SC_CLOSE, what the native title bar sends.
        QPointer<CompareWindow> compareGuard(compare);
        VideoSurface* surface = dialog->findChild<VideoSurface*>();
        const QImage frozenBefore = compare->frozenFrame();
        std::fprintf(stderr,
                     "     before close: visible=%d frozen=%dx%d playing=%d planLocked=%d\n",
                     compare->isVisible() ? 1 : 0, frozenBefore.width(), frozenBefore.height(),
                     surface != nullptr && surface->isPlaying() ? 1 : 0,
                     surface != nullptr && surface->nativePlanLocked() ? 1 : 0);
        check(compare->isVisible(), "the compare window is on screen before the close");
#ifdef Q_OS_WIN
        const HWND hwnd = reinterpret_cast<HWND>(compare->winId());
        if (hwnd == nullptr || !IsWindow(hwnd)) {
            check(false, "the compare window owns a native HWND to close");
            app.exit(1);
            return;
        }
        SendMessageW(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
#else
        compare->close();
#endif
        QElapsedTimer afterClose;
        afterClose.start();
        while (afterClose.elapsed() < 2000) {
            const bool settled = (compareGuard.isNull() || !compareGuard->isVisible()) &&
                                 (compareGuard.isNull() || !compareGuard->playbackHeld()) &&
                                 (compareGuard.isNull() || !compareGuard->comparisonPending()) &&
                                 (surface == nullptr || !surface->nativePlanLocked());
            if (settled) break;
            QTest::qWait(50);
        }

        check(!compareGuard.isNull() && !compareGuard->isVisible(),
              "the native close command hid the Compare window");
        check(!compareGuard.isNull() && !compareGuard->playbackHeld(),
              "the close released the playback the frozen capture had paused");
        check(!compareGuard.isNull() && !compareGuard->comparisonPending(),
              "the close left no pending capture, build or release");
        check(surface != nullptr && !surface->nativePlanLocked(),
              "the close released the native plan lock the frozen capture held");
        check(!compareGuard.isNull() && compareGuard->frozenFrame().isNull() &&
                  compareGuard->renderedSideA().isNull() &&
                  compareGuard->renderedSideB().isNull(),
              "the native close clears the frozen source and both comparison outputs");
        if (!compareGuard.isNull()) {
            std::fprintf(stderr,
                         "     after close: visible=%d held=%d pending=%d planLocked=%d "
                         "frozenNull=%d outputsNull=%d\n",
                         compareGuard->isVisible() ? 1 : 0,
                         compareGuard->playbackHeld() ? 1 : 0,
                         compareGuard->comparisonPending() ? 1 : 0,
                         surface != nullptr && surface->nativePlanLocked() ? 1 : 0,
                         compareGuard->frozenFrame().isNull() ? 1 : 0,
                         compareGuard->renderedSideA().isNull() &&
                                 compareGuard->renderedSideB().isNull()
                             ? 1 : 0);
        }

        std::fprintf(stderr, "RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
        app.exit(failures == 0 ? 0 : 1);
    });
    return app.exec();
}
