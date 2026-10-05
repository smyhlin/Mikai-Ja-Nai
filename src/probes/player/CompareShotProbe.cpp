// Renders the comparison window and saves a PNG, so the thing can be looked at
// instead of described.
//
// Everything else in this repo verified the comparison by asserting that a
// button existed and a chain reached the core. None of it ever looked at the
// result, which is how a window that renders no image at all still passed 30
// checks. This probe exists to be looked at.
#include <QApplication>
#include "CompareCanvas.hpp"
#include <QComboBox>
#include <QGraphicsView>
#include <QPixmap>
#include <QPointingDevice>
#include <QTest>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>

#include "Anime4K.hpp"
#include "ComparePane.hpp"
#include "CompareWindow.hpp"
#include "PlayerDialog.hpp"
#include "PlayerShell.hpp"
#include "VideoSurface.hpp"
#include "ProbeRuntime.hpp"

using namespace mplayer;
using namespace mplayer::shell;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

/// True when the wheel rescaled the picture this view is showing. The scale on
/// screen is the view transform, so that is what is measured, and the wheel is
/// sent into the viewport, where a real wheel event lands.
bool wheelRescales(const QGraphicsView* view) {
    const double before = view->transform().m11();
    const QPointF centre(view->viewport()->width() / 2.0, view->viewport()->height() / 2.0);
    for (int notch : {3, -3, 5, -5}) {
        QWheelEvent wheel(centre, view->viewport()->mapToGlobal(centre.toPoint()), QPoint(),
                          QPoint(0, notch * 120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false, Qt::MouseEventNotSynthesized,
                          QPointingDevice::primaryPointingDevice());
        QApplication::sendEvent(view->viewport(), &wheel);
        QCoreApplication::processEvents();
    }
    return qAbs(view->transform().m11() - before) > 1e-9;
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
    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }
    static Host host;
    const QString url = QStringLiteral("av://lavfi:testsrc2=size=1280x720:rate=25");
    const QList<QualityEntry> qualities = {{480, url}, {720, url}, {1080, url}};

    // Driven through the real player, because that is how it is reached: a
    // window that only works when constructed one particular way is a window
    // nobody opens.
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

        // A pixmap with real pixels in it is the whole point: an empty or
        // all-one-colour canvas is exactly what the last version produced.
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

        // ── the wheel must not zoom, in either surface ────────────
        // The single canvas and the pane grid are separate classes with
        // separate event handlers, so one wheel is not evidence about the
        // other; both are wheeled here.
        auto* modes = compare->findChild<QComboBox*>(QString::fromLatin1("compareMode"));
        check(modes != nullptr && modes->count() > 1,
              "the window can switch to the surface being wheeled");
        if (modes != nullptr) {
            modes->setCurrentIndex(0);
            QTest::qWait(300);
            check(!wheelRescales(canvas), "the wheel does not rescale the single canvas");
            std::fprintf(stderr, "     canvas scale %.4f\n", canvas->transform().m11());

            const int sideBySide =
                modes->findData(QString::fromLatin1(modeKey(CompareMode::SideBySide)));
            modes->setCurrentIndex(sideBySide >= 0 ? sideBySide : 0);
            QTest::qWait(300);
            const QList<ComparePane*> panes = compare->findChildren<ComparePane*>();
            check(!panes.isEmpty(), "the side-by-side layout builds its panes");
            for (ComparePane* pane : panes) {
                if (!pane->isVisible()) {
                    continue;
                }
                check(!wheelRescales(pane), "the wheel does not rescale a side-by-side pane");
                std::fprintf(stderr, "     pane %s scale %.4f\n", qPrintable(pane->key()),
                             pane->transform().m11());
            }
        }

        compare->grab().save(QStringLiteral("compare_shot.png"));
        std::fprintf(stderr, "RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
        app.exit(failures == 0 ? 0 : 1);
    });
    return app.exec();
}
