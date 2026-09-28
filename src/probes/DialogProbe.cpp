// Live dialog probe: builds the ported PlayerDialog with a stub host, plays a
// test pattern, and writes PNGs so the strip, the status panel, the dock and the
// series drawer can all be inspected in one shot.

#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include "PlayerDialog.hpp"
#include "PlayerShell.hpp"
#include "VideoSurface.hpp"

using namespace mplayer::shell;

namespace {

class StubHost : public PlayerHost {
public:
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestOpenOriginal() override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    void playbackFailed() override {}
};

int visibleChildren(const QWidget* root) {
    int count = 0;
    for (const QWidget* child : root->findChildren<QWidget*>()) {
        if (child->isVisible()) ++count;
    }
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    static StubHost host;
    // A real URL can be handed in to check the mpv HTTP options against a live
    // CDN; the lavfi pattern is the default because it needs no network.
    const QString url = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                 : QStringLiteral("av://lavfi:testsrc2=size=1280x720:rate=25");
    const QList<QualityEntry> qualities = {{720, url}};
    auto* dialog = new PlayerDialog(QStringLiteral("probe 720p"), url, qualities, 720, &host);
    // Two tracks, so the track chip has to appear; the drawer needs a playlist.
    dialog->setVariants({QVariantMap{{"label", QStringLiteral("Ashdi 1080p")},
                                     {"embed", QStringLiteral("e1")},
                                     {"kind", QStringLiteral("voice")}},
                         QVariantMap{{"label", QStringLiteral("Цукіхана 720p")},
                                     {"embed", QStringLiteral("e2")},
                                     {"kind", QStringLiteral("sub")}}},
                        QStringLiteral("e1"));
    QStringList labels;
    for (int i = 1; i <= 12; ++i) {
        labels << QStringLiteral("Серія %1").arg(i);
    }
    dialog->setPlaylist([&] {
        QVector<QVariantMap> playlist;
        for (const QString& label : labels) {
            playlist.append(QVariantMap{{"label", label}});
        }
        return playlist;
    }(), 2);
    dialog->setSeriesExpanded(true);
    dialog->startPlayer();
    dialog->show();

    QTimer::singleShot(2000, [&] {
        const QImage frame = dialog->grab().toImage();
        frame.save(QStringLiteral("dialog_probe.png"));
        std::fprintf(stderr, "dialog %dx%d visible children=%d\n", frame.width(), frame.height(),
                     visibleChildren(dialog));
        long nonblack = 0;
        for (int y = 0; y < frame.height(); y += 2) {
            for (int x = 0; x < frame.width(); x += 2) {
                const QColor c = frame.pixelColor(x, y);
                if (c.red() + c.green() + c.blue() > 30) ++nonblack;
            }
        }
        std::fprintf(stderr, "nonblack=%ld\n", nonblack);
        auto* shell = dialog->findChild<mplayer::shell::PlayerShell*>();
        if (shell != nullptr) {
            auto* surface = shell->videoSurface();
            std::fprintf(stderr, "shell %dx%d surface geom %dx%d@%d,%d\n",
                         shell->width(), shell->height(), surface->width(),
                         surface->height(), surface->x(), surface->y());
            const QImage gl = surface->grabFramebuffer();
            gl.save(QStringLiteral("dialog_gl.png"));
            long lit = 0, total = 0;
            for (int y = 0; y < gl.height(); y += 2) {
                for (int x = 0; x < gl.width(); x += 2) {
                    const QColor c = gl.pixelColor(x, y);
                    ++total;
                    if (c.red() + c.green() + c.blue() > 30) ++lit;
                }
            }
            std::fprintf(stderr, "framebuffer %dx%d video=%dx%d playing=%d lit=%ld/%ld\n",
                         gl.width(), gl.height(), surface->videoWidth(),
                         surface->videoHeight(), int(surface->isPlaying()), lit, total);
        }
        app.exit(0);
    });

    return app.exec();
}
