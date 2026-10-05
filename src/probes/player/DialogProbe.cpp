// The verdict reads the surface's own framebuffer, never the dialog grab.

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

#include <cstdio>
#include <functional>
#include <memory>

#include "PlayerDialog.hpp"
#include "PlayerShell.hpp"
#include "ProbePicture.hpp"
#include "ProbeRuntime.hpp"
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
    // A close must not end the loop before the reopened run is judged.
    app.setQuitOnLastWindowClosed(false);

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiDialogProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("DialogProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }

    static StubHost host;
    const QString url = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                 : QStringLiteral("av://lavfi:testsrc2=size=1280x720:rate=25");
    const QList<QualityEntry> qualities = {{720, url}};

    const QVector<QVariantMap> playlist = [] {
        QVector<QVariantMap> rows;
        for (int i = 1; i <= 12; ++i) {
            rows.append(QVariantMap{{"label", QStringLiteral("Серія %1").arg(i)}});
        }
        return rows;
    }();

    const auto buildDialog = [&] {
        auto* dialog = new PlayerDialog(QStringLiteral("probe 720p"), url, qualities, 720, &host);
        dialog->setVariants({QVariantMap{{"label", QStringLiteral("Ashdi 1080p")},
                                         {"embed", QStringLiteral("e1")},
                                         {"kind", QStringLiteral("voice")}},
                             QVariantMap{{"label", QStringLiteral("Цукіхана 720p")},
                                         {"embed", QStringLiteral("e2")},
                                         {"kind", QStringLiteral("sub")}}},
                            QStringLiteral("e1"));
        dialog->setPlaylist(playlist, 2);
        dialog->setSeriesExpanded(true);
        dialog->startPlayer();
        dialog->show();
        return dialog;
    };

    mplayer::probes::Verdict verdict;

    // Evidence on disk only: a painted dialog over a dead surface looks identical.
    const auto captureChrome = [&](PlayerDialog* dialog) {
        auto* surface = dialog->findChild<mplayer::VideoSurface*>();
        const QImage frame = dialog->grab().toImage();
        frame.save(QStringLiteral("dialog_probe.png"));
        verdict.metric(QStringLiteral("dialog %1x%2, visible children=%3")
                           .arg(frame.width())
                           .arg(frame.height())
                           .arg(visibleChildren(dialog)));
        auto* widgetShell = dialog->findChild<PlayerShell*>();
        if (widgetShell != nullptr && surface != nullptr) {
            verdict.metric(QStringLiteral("shell %1x%2, surface geometry %3x%4@%5,%6")
                               .arg(widgetShell->width())
                               .arg(widgetShell->height())
                               .arg(surface->width())
                               .arg(surface->height())
                               .arg(surface->x())
                               .arg(surface->y()));
        }
    };

    const std::function<void(PlayerDialog*, const char*, bool, std::function<void()>)>
        verifyPlayback = [&](PlayerDialog* dialog, const char* pngName, bool wantChrome,
                             std::function<void()> done) {
            auto* surface = dialog->findChild<mplayer::VideoSurface*>();
            if (surface == nullptr) {
                verdict.require(false, QStringLiteral("dialog exposes a VideoSurface"));
                QTimer::singleShot(0, &app, [&] { app.exit(verdict.report("dialog_probe")); });
                return;
            }
            auto sampler = std::make_shared<mplayer::probes::PlaybackSampler>(*surface);
            auto clock = std::make_shared<QElapsedTimer>();
            clock->start();
            auto mediaError = std::make_shared<QString>();
            QObject::connect(surface, &mplayer::VideoSurface::errorOccurred, dialog,
                             [mediaError](const QString& message) { *mediaError = message; });

            auto* timer = new QTimer(dialog);
            timer->setInterval(100);
            auto chromeShot = std::make_shared<bool>(false);
            QObject::connect(
                timer, &QTimer::timeout, dialog,
                [&, dialog, surface, sampler, clock, mediaError, timer, chromeShot, pngName,
                 wantChrome, done] {
                    if (wantChrome && !*chromeShot && clock->elapsed() >= 2000) {
                        *chromeShot = true;
                        captureChrome(dialog);
                    }
                    const bool paired = sampler->tick();
                    if (!(!mediaError->isEmpty() || (paired && clock->elapsed() >= 4000) ||
                          clock->elapsed() >= 9000)) {
                        return;
                    }
                    timer->stop();

                    const mplayer::probes::PlaybackSample& sample = sampler->later();
                    verdict.metric(
                        QStringLiteral("duration=%1 s, playing=%2, position=%3 s, video=%4x%5")
                            .arg(surface->duration(), 0, 'f', 2)
                            .arg(int(surface->isPlaying()))
                            .arg(sample.position, 0, 'f', 2)
                            .arg(sample.videoWidth)
                            .arg(sample.videoHeight));
                    verdict.metric(QStringLiteral("native status: polling=%1 (native work not "
                                                  "requested); filter presence/absence not "
                                                  "proven by this probe")
                                       .arg(int(surface->nativeStatusPolling())));

                    const bool saved =
                        sample.hasPicture() && sample.framebuffer.save(QString::fromLatin1(pngName));
                    verdict.metric(QStringLiteral("framebuffer png %1: %2")
                                       .arg(QString::fromLatin1(pngName),
                                            saved ? QStringLiteral("written")
                                                  : QStringLiteral("FAILED")));
                    verdict.require(
                        saved, QStringLiteral("framebuffer PNG written to %1")
                                   .arg(QString::fromLatin1(pngName)));

                    verdict.require(mediaError->isEmpty(),
                                    QStringLiteral("no playback error reported (%1)")
                                        .arg(mediaError->isEmpty() ? QStringLiteral("none")
                                                                   : *mediaError));

                    mplayer::probes::requirePlayback(verdict, sampler->earlier(), sampler->later());

                    // WA_DeleteOnClose: close() alone deletes it next loop turn.
                    QPointer<PlayerDialog> guard(dialog);
                    dialog->close();
                    QTimer::singleShot(200, &app, [&, guard, done] {
                        verdict.require(guard.isNull(),
                                        QStringLiteral("dialog closed and destroyed"));
                        done();
                    });
                });
            timer->start();
        };

    auto* dialog = buildDialog();
    verifyPlayback(dialog, "dialog_gl.png", /*wantChrome=*/true, [&] {
        auto* reopened = buildDialog();
        verifyPlayback(reopened, "dialog_gl_reopen.png", /*wantChrome=*/false,
                       [&] { app.exit(verdict.report("dialog_probe")); });
    });

    return app.exec();
}
