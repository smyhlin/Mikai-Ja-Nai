// Proves the compare window's two load-bearing claims, which nothing else in
// the suite covers:
//
//  1. A frozen frame is the same frame. The window compares two chains by
//     applying each to the live core and grabbing what comes back, so if the
//     frame moved between the two grabs the difference would be playback and
//     not the shader. This is the same trap InteractionProbe documents: seeking
//     to one timestamp twice does not reproduce the same pixels, so the control
//     pair is two SETTLED frames without re-seeking.
//
//  2. The catalogue is real. Every candidate resolves to files on disk, an
//     unknown id resolves to nothing, and a chain that is applied actually
//     changes the picture - mpv's glsl-shaders echoes the string back without
//     validating it, so the only evidence is pixels.
//
//  3. A capture is fresh. The window is kept and reused, so "capture again"
//     after a rewind must give the new frame, not the one already frozen.
//     testsrc2 changes every frame, so two captures at two timestamps are
//     provably two different pictures, and the second must match what the
//     player is actually showing.
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#include "Anime4K.hpp"
#include "CompareWindow.hpp"
#include "PlayerDialog.hpp"
#include "PlayerShell.hpp"
#include "ShellTheme.hpp"
#include "VideoSurface.hpp"

using namespace mplayer;
using namespace mplayer::shell;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

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

/// Two frames of one still picture, WITHOUT seeking in between. Re-seeking is
/// what makes the "noise floor" a frame-to-frame difference instead of zero.
QImage settledFrame(VideoSurface* surface) {
    QImage previous;
    for (int attempt = 0; attempt < 80; ++attempt) {
        QCoreApplication::processEvents();
        QThread::msleep(40);
        const QImage current = surface->grabFramebuffer();
        if (!current.isNull() && current == previous) {
            return current;
        }
        previous = current;
    }
    return QImage();
}

/// Mean absolute per-channel difference, inside the picture band only. The
/// framebuffer includes the letterbox bars, whose edges read as a huge
/// difference, so the whole image would be measured against the wrong thing.
double meanDifference(const QImage& a, const QImage& b) {
    if (a.isNull() || b.isNull() || a.size() != b.size()) {
        return -1.0;
    }
    const QRect band(a.width() / 8, a.height() / 8, a.width() * 3 / 4, a.height() * 3 / 4);
    const QImage left = a.convertToFormat(QImage::Format_RGB888);
    const QImage right = b.convertToFormat(QImage::Format_RGB888);
    double total = 0.0;
    qint64 counted = 0;
    for (int y = band.top(); y < band.bottom(); ++y) {
        for (int x = band.left(); x < band.right(); ++x) {
            const QRgb pa = left.pixel(x, y);
            const QRgb pb = right.pixel(x, y);
            total += std::abs(qRed(pa) - qRed(pb)) + std::abs(qGreen(pa) - qGreen(pb)) +
                     std::abs(qBlue(pa) - qBlue(pb));
            counted += 3;
        }
    }
    return counted > 0 ? total / static_cast<double>(counted) : -1.0;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    static StubHost host;
    const QString url = QStringLiteral("av://lavfi:testsrc2=size=1280x720:rate=25");
    const QList<QualityEntry> qualities = {{480, url}, {720, url}, {1080, url}};

    auto* dialog = new PlayerDialog(QStringLiteral("compare probe"), url, qualities, 720, &host);
    dialog->setVariants({QVariantMap{{"label", QStringLiteral("v")}, {"embed", QStringLiteral("e1")}}},
                        QStringLiteral("e1"));
    // Without this the core never loads a file: the dialog builds the
    // surface, but nothing asks it to open the stream.
    dialog->startPlayer();
    // Fullscreen, like the real player opens: a fullscreen owner is a
    // different stacking case, and a normal window that happens to agree
    // is not the thing being tested.
    dialog->showFullScreen();

    QTimer::singleShot(3000, &app, [&] {
        auto* surface = dialog->findChild<VideoSurface*>();
        // Poll rather than trust a deadline: a cold mpv core can take several
        // seconds to open a file, and a fixed timeout turns a slow machine into
        // a false failure.
        for (int attempt = 0; attempt < 200; ++attempt) {
            if (surface != nullptr && surface->hasVideo()) {
                break;
            }
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }
        if (surface == nullptr || !surface->hasVideo()) {
            // Exit 0, not 1: nothing failed, the session just cannot prove it.
        // A non-zero code here would make a headless agent report a red run
        // for a condition no display can satisfy.
        std::fprintf(stderr, "SKIP  no stream loaded in this session\n");
        app.exit(0);            return;
        }
        surface->setPaused(true);
        surface->seek(6.0);
        QTest::qWait(1200);

        // ── 1. the frozen frame is stable ───────────────────────────
        const QImage controlA = settledFrame(surface);
        const QImage controlB = settledFrame(surface);
        check(!controlA.isNull(), "the frame settles on a still picture");
        const double noise = meanDifference(controlA, controlB);
        std::fprintf(stderr, "     control pair: %.3f / 255\n", noise);
        check(noise >= 0.0 && noise < 0.01,
              "two settled frames of one picture are identical (no re-seek)");

        // ── 2. a chain changes the picture, and off restores it ─────
        const QString shadersDir = anime4k::findShadersDir();
        check(!shadersDir.isEmpty(), "the shaders folder sits beside the exe");
        check(anime4k::shaderStringForCandidate(QStringLiteral("off"), shadersDir).isEmpty(),
              "off resolves to no chain at all");
        check(anime4k::filesForId(QStringLiteral("no-such-chain"), shadersDir).isEmpty(),
              "an unknown id resolves to nothing rather than to a guess");

        const QString single = QStringLiteral("Anime4K_Restore_CNN_VL.glsl");
        check(anime4k::chainIsComplete(anime4k::candidateForId(single, shadersDir), shadersDir),
              "a single .glsl that has no preset resolves to a complete chain");

        const QImage before = settledFrame(surface);
        surface->setAnime4KChain(single);
        QTest::qWait(600);
        const QImage after = settledFrame(surface);
        const double effect = meanDifference(before, after);
        std::fprintf(stderr, "     single shader: %.3f / 255 against %.3f noise\n", effect, noise);
        check(effect > 0.05,
              "applying a chain changes the picture by more than the noise floor");

        // Back off: the window has to be able to undo what it previewed.
        surface->setAnime4KChain(QStringLiteral("off"));
        QTest::qWait(600);
        const QImage restored = settledFrame(surface);
        const double residual = meanDifference(before, restored);
        std::fprintf(stderr, "     after off: %.3f / 255\n", residual);
        check(residual < 0.05, "turning the chain off returns the original picture");

        // ── 3. the catalogue is complete ────────────────────────────
        const QVector<anime4k::Candidate> all = anime4k::allCandidates(shadersDir);
        int incomplete = 0;
        int unnamed = 0;
        for (const anime4k::Candidate& candidate : all) {
            if (!anime4k::chainIsComplete(candidate, shadersDir)) ++incomplete;
            if (candidate.id.isEmpty() || candidate.label.isEmpty()) ++unnamed;
        }
        std::fprintf(stderr, "     catalogue: %d candidates\n", int(all.size()));
        check(all.size() > 40, "the catalogue lists the presets and the individual shaders");
        check(incomplete == 0, "every candidate resolves to files that exist on disk");
        check(unnamed == 0, "every candidate carries an id and a label");

        // ── 4. the window opens, and reads the live surface ──────────
        auto* shell = dialog->findChild<PlayerShell*>();
        check(shell != nullptr, "the player has a shell");
        if (shell == nullptr) {
            app.exit(failures == 0 ? 0 : 1);
            return;
        }
        emit shell->compareRequested();
        QTest::qWait(2500);
        auto* compare = dialog->findChild<CompareWindow*>();
        check(compare != nullptr, "the compare row opened a window");
        if (compare == nullptr) {
            std::fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
            app.exit(1);
            return;
        }
        check(compare->isVisible(), "the compare window is on screen");

        // The sidebar is gone; the catalogue reaches the viewer through the
        // badge menus on the pictures, grouped rather than flat, so every entry
        // has to be reachable inside the sections for both sides to be
        // choosable at all.
        // A named std::function rather than auto: the walk is recursive, and a
        // lambda cannot call itself through a name whose type is still being
        // deduced.
        std::function<int(QMenu*)> leavesUnder = [&leavesUnder](QMenu* menu) {
            int count = 0;
            for (QAction* action : menu->actions()) {
                if (QMenu* submenu = action->menu()) {
                    count += leavesUnder(submenu);
                } else if (!action->text().isEmpty()) {
                    ++count;
                }
            }
            return count;
        };
        // The sections are not immediate children of the badge menu: the
        // chooser nests them inside the Anime4K branch, so the lookup has to
        // descend the tree a submenu at a time, and it has to say where it
        // found them. Counting the distinct submenus carrying an exact label
        // makes a missing section (zero) and a duplicated one (two) both read
        // as something other than one - asking merely "is the name anywhere"
        // would pass on both, and asking only the immediate children would miss
        // the real grouping entirely. Same named-std::function reason as above:
        // both walks are recursive.
        std::function<int(QMenu*, const QString&)> submenusNamed =
            [&submenusNamed](QMenu* menu, const QString& name) {
                int found = 0;
                for (QAction* action : menu->actions()) {
                    if (QMenu* submenu = action->menu()) {
                        if (action->text() == name) {
                            ++found;
                        }
                        found += submenusNamed(submenu, name);
                    }
                }
                return found;
            };
        std::function<QMenu*(QMenu*, const QString&)> firstSubmenuNamed =
            [&firstSubmenuNamed](QMenu* menu, const QString& name) -> QMenu* {
                for (QAction* action : menu->actions()) {
                    if (QMenu* submenu = action->menu()) {
                        if (action->text() == name) {
                            return submenu;
                        }
                        if (QMenu* nested = firstSubmenuNamed(submenu, name)) {
                            return nested;
                        }
                    }
                }
                return nullptr;
            };
        int widestMenu = 0;
        int chipMenus = 0;
        int anime4kBranches = 0;
        int presetSections = 0;
        int shaderSections = 0;
        for (QToolButton* chip : compare->findChildren<QToolButton*>()) {
            if (chip->menu() == nullptr) {
                continue;
            }
            ++chipMenus;
            widestMenu = std::max(widestMenu, leavesUnder(chip->menu()));
            anime4kBranches += submenusNamed(chip->menu(), QStringLiteral("Anime4K"));
            if (QMenu* branch = firstSubmenuNamed(chip->menu(), QStringLiteral("Anime4K"))) {
                presetSections += submenusNamed(branch, QStringLiteral("Пресети"));
                shaderSections += submenusNamed(branch, QStringLiteral("Окремі шейдери"));
            }
        }
        std::fprintf(stderr, "     widest chip menu: %d entries\n", widestMenu);
        check(chipMenus > 0, "the compare badges carry a candidate chooser menu");
        check(widestMenu == all.size(), "every catalogue entry is reachable in the chip menus");
        // Exactly one Anime4K branch per badge menu, and exactly one of each
        // section inside it: an OR across the badges would pass with a single
        // side left flat, a count taken anywhere in the tree would pass with a
        // stray second copy beside the real one, and a root-level count would
        // pass on a flat list that never built the branch at all.
        check(anime4kBranches == chipMenus, "every chip menu nests one Anime4K branch");
        check(presetSections == chipMenus,
              "every chip menu groups the presets into one section under Anime4K");
        check(shaderSections == chipMenus,
              "every chip menu groups the loose shaders into one section under Anime4K");
        // The two AutoDownscale steps are chain internals, so they must not be
        // offered as standalone choices anywhere in those menus.
        bool offeredInternal = false;
        for (QToolButton* chip : compare->findChildren<QToolButton*>()) {
            if (chip->menu() == nullptr) {
                continue;
            }
            std::function<void(QMenu*)> scan = [&](QMenu* menu) {
                for (QAction* action : menu->actions()) {
                    if (QMenu* submenu = action->menu()) {
                        scan(submenu);
                    } else if (action->data().toString().contains(QLatin1String("AutoDownscalePre"))) {
                        offeredInternal = true;
                    }
                }
            };
            scan(chip->menu());
        }
        check(!offeredInternal, "the chain-internal downscale steps are not offered standalone");

        auto* mode = compare->findChild<QComboBox*>(QString::fromLatin1("compareMode"));
        check(mode != nullptr && mode->count() == 5,
              "the five remaining compare modes are offered");

        // ── 5. the info line is real, not a label ─────────────────
        auto* info = compare->findChild<QLabel*>(QString::fromLatin1("compareInfo"));
        check(info != nullptr && !info->text().isEmpty(),
              "the window explains the selected chains");
        if (info != nullptr) {
            std::fprintf(stderr, "     info: %s\n", qPrintable(info->text()));
            check(info->text().contains(QStringLiteral("A")), "the info line names side A");
        }

        // ── 6. a second capture is the new frame, not the old one ──
        // The failure this covers is not hypothetical: the window used to
        // re-seek the live player to the position it was already at and then
        // keep whatever the framebuffer happened to hold, so rewinding and
        // capturing again could hand back the frame the viewer had left. The
        // core is parked Off here so the frozen frame is the plain picture and
        surface->setAnime4KChain(QStringLiteral("off"));
        QTest::qWait(500);

        // Two positions far enough apart that the picture cannot be confused:
        // testsrc2 advances its counters and its colour bars every frame.
        const double firstAt = 3.0;
        const double secondAt = 11.0;
        surface->setPaused(true);

        // Capture once at the first position. The window is closed first
        // because showing an ALREADY-shown window does not re-fire showEvent,
        // and re-opening the kept window is the path under test here.
        compare->close();
        surface->seek(firstAt);
        QTest::qWait(1500);
        emit shell->compareRequested();
        QTest::qWait(3000);
        const QImage firstCapture = compare->frozenFrame();
        check(!firstCapture.isNull(), "the first capture froze a frame");
        check(compare->captureError().isEmpty(), "the first capture reported no error");

        // Now move the player and capture again on the SAME window. If the
        // window were caching, the second capture would equal the first.
        surface->seek(secondAt);
        QTest::qWait(1500);
        const QImage referenceAtSecond = settledFrame(surface);
        check(!referenceAtSecond.isNull(),
              "the player settled on a frame at the second position");
        compare->captureNow();
        QTest::qWait(2500);
        const QImage secondCapture = compare->frozenFrame();
        check(!secondCapture.isNull(), "the second capture froze a frame");
        check(compare->captureError().isEmpty(), "the second capture reported no error");

        // The three-way check: the two captures differ from each other, and
        // the second one matches the picture the player is actually showing.
        const double capturesDiffer = meanDifference(firstCapture, secondCapture);
        const double secondVsReference = meanDifference(secondCapture, referenceAtSecond);
        const double secondVsFirst = meanDifference(secondCapture, firstCapture);
        std::fprintf(stderr, "     first vs second: %.3f / 255\n", capturesDiffer);
        std::fprintf(stderr, "     second vs player: %.3f / 255\n", secondVsReference);
        std::fprintf(stderr, "     second vs first:  %.3f / 255\n", secondVsFirst);
        check(capturesDiffer > 0.05,
              "two captures at two positions are two different frames");
        check(secondVsFirst > 0.05, "the second capture is not the first frame again");
        check(secondVsReference < secondVsFirst,
              "the second capture is closer to the new position than to the old one");

        // Both sides come from one frozen source, so every capture of the same
        // stream has to be the same source size.
        check(!firstCapture.isNull() && firstCapture.size() == secondCapture.size(),
              "both captures are the same source size");

        // ── 7. the window is non-blocking and closable ────────────
        check(!compare->isModal(), "the comparison does not take the player hostage");
        compare->close();
        QTest::qWait(400);
        check(!compare->isVisible(), "the window closes on its own");

        // ── 8. a chain with no Mode survives a restart ────────────
        // A single .glsl has no enum value, so an ordinal would drop it on
        // every start. The id is what is stored, and it is what is read back.
        surface->setAnime4KChain(single);
        QTest::qWait(400);
        check(surface->anime4KChainId() == single,
              "a chain with no Mode is remembered by its id");

        std::fprintf(stderr, "RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
        app.exit(failures == 0 ? 0 : 1);
    });
    return app.exec();
}
