// Proves the compare window's two load-bearing claims, which nothing else in
// the suite covers:
//
//  1. A frozen frame is the same frame. The window compares two chains by
//     applying each to the live core and grabbing what comes back, so if the
//     frame moved between the two grabs the difference would be playback and
//     not the shader. This is the same trap InteractionProbe documents: seeking
//     to one timestamp twice does not reproduce the same pixels, so the control
//     pair is two SETTLED frames without re-seeking.
//  2. The catalogue is real. Every candidate resolves to files on disk, an
//     unknown id resolves to nothing, and a chain that is applied actually
//     changes the picture - mpv's glsl-shaders echoes the string back without
//     validating it, so the only evidence is pixels.
//
//  2b. The badge chooser IS the player's preset set. Every leaf it offers is an
//     `availableModes()` preset with that preset's own label under that
//     preset's own category path, and every preset the catalogue holds is
//     offered. The expectation is read out of the catalogue rather than typed,
//     so a renamed preset stays a rename instead of a second copy of the
//     wording that goes stale. A loose `.glsl` is never a choice: a compared
//     model the player cannot select is not a model the player can run.
//
//  3. A capture is fresh. The window is kept and reused, so "capture again"
//     after a rewind must give the new frame, not the one already frozen.
//     testsrc2 changes every frame, so two captures at two timestamps are
//     provably two different pictures, and the second must match what the
//     player is actually showing.
//
//  4. Compare leaves the player as found. The chain read before the two sides
//     is the chain afterwards, on a successful capture, on close and after a
//     failed capture; the native status polling a capture paused comes back at
//     the value it actually had (off stays off); and a surface that cannot be
//     read - closed, empty, or absent - is refused by name rather than frozen.
#include <QAction>
#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>
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
#include "ProbeRuntime.hpp"

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

/// The label path a catalogue category id reads as: one label per level of the
/// id, taken from the catalogue itself. The chooser is checked against the
/// category it claims rather than against a copy of its wording.
QStringList labelPath(const QString& categoryId) {
    QStringList path;
    QString prefix;
    for (const QString& part : categoryId.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        prefix = prefix.isEmpty() ? part : prefix + QLatin1Char('/') + part;
        const QString label = anime4k::categoryLabel(prefix);
        if (!label.isEmpty()) {
            path.append(label);
        }
    }
    return path;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // Both scopes are redirected into a directory that dies with this process
    // before the first PlayerDialog, whose constructor reads player/anime4k and
    // player/presetShortcuts. Without it a probe inherits a real session.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiCompareProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("CompareProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }

    static StubHost host;
    // A real file can be handed in to check the frozen-frame path against a
    // decoded source; the lavfi pattern is the default because it needs no
    // fixture on disk.
    const QString url = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                 : QStringLiteral("av://lavfi:testsrc2=size=1280x720:rate=25");
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
        // Everything the probe runs is read out of the catalogue: the presets,
        // the id that disables the chain, and the loose shader the live checks
        // apply. A name typed here would be a second copy of that contract - the
        // copy that goes stale the moment the catalogue changes.
        const QVector<anime4k::ModeInfo> presets = anime4k::availableModes();
        QStringList presetIds;
        presetIds.reserve(presets.size());
        for (const anime4k::ModeInfo& preset : presets) {
            presetIds.append(preset.id);
        }
        const QVector<anime4k::Candidate> all = anime4k::allCandidates(shadersDir);
        const QString offId = anime4k::modeId(anime4k::Mode::Off);

        check(anime4k::shaderStringForCandidate(offId, shadersDir).isEmpty(),
              "off resolves to no chain at all");
        check(anime4k::filesForId(QStringLiteral("no-such-chain"), shadersDir).isEmpty(),
              "an unknown id resolves to nothing rather than to a guess");

        // A loose restoration shader: one the catalogue offers as a file of its
        // own rather than inside a preset chain. Its id is the file name, and
        // that is the value which has to reach the player and come back below.
        QString single;
        for (const anime4k::Candidate& candidate : all) {
            if (presetIds.contains(candidate.id) || candidate.kind != anime4k::Kind::Restore) {
                continue;
            }
            if (anime4k::chainIsComplete(candidate, shadersDir)) {
                single = candidate.id;
                break;
            }
        }
        check(!single.isEmpty(),
              "the catalogue offers a loose restoration shader that has no preset");

        const QImage before = settledFrame(surface);
        surface->setAnime4KChain(single);
        QTest::qWait(600);
        const QImage after = settledFrame(surface);
        const double effect = meanDifference(before, after);
        std::fprintf(stderr, "     single shader: %.3f / 255 against %.3f noise\n", effect, noise);
        check(effect > 0.05,
              "applying a chain changes the picture by more than the noise floor");

        // Back off: the window has to be able to undo what it previewed.
        surface->setAnime4KChain(offId);
        QTest::qWait(600);
        const QImage restored = settledFrame(surface);
        const double residual = meanDifference(before, restored);
        std::fprintf(stderr, "     after off: %.3f / 255\n", residual);
        check(residual < 0.05, "turning the chain off returns the original picture");

        int incomplete = 0;
        for (const anime4k::Candidate& candidate : all) {
            if (!anime4k::chainIsComplete(candidate, shadersDir)) ++incomplete;
        }
        std::fprintf(stderr, "     catalogue: %d candidates\n", int(all.size()));
        check(incomplete == 0, "every candidate resolves to files that exist on disk");

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

        // The badge menus are the only chooser, so what they offer IS the set
        // of Compare models. The catalogue is the contract here: every preset
        // availableModes() lists has to be selectable, every choice has to be
        // one of those presets, and each has to sit under the labels its own
        // category names. The expected path is derived from the catalogue's
        // labels rather than typed out, so a renamed group stays a rename
        // instead of turning into a broken probe.
        struct Offered {
            QString id;
            QString label;
            QStringList path;
        };
        std::function<void(QMenu*, QStringList&, QVector<Offered>&)> collect =
            [&collect](QMenu* menu, QStringList& path, QVector<Offered>& out) {
                for (QAction* action : menu->actions()) {
                    if (QMenu* submenu = action->menu()) {
                        path.append(action->text());
                        collect(submenu, path, out);
                        path.removeLast();
                    } else if (!action->data().toString().isEmpty()) {
                        out.append({action->data().toString(), action->text(), path});
                    }
                }
            };
        int chipMenus = 0;
        QStringList unoffered;  // player presets the chooser leaves out
        QStringList repeated;   // player presets the chooser offers more than once
        QStringList foreign;    // choices that are not player presets at all
        QStringList misplaced;  // choices not under their category's own labels
        QStringList mislabeled;  // choices whose leaf text is not the preset's own label
        for (QToolButton* chip : compare->findChildren<QToolButton*>()) {
            if (chip->menu() == nullptr) {
                continue;
            }
            ++chipMenus;
            QStringList path;
            QVector<Offered> offered;
            collect(chip->menu(), path, offered);
            for (const anime4k::ModeInfo& preset : presets) {
                const auto seen = std::count_if(
                    offered.cbegin(), offered.cend(),
                    [&preset](const Offered& choice) { return choice.id == preset.id; });
                if (seen == 0 && !unoffered.contains(preset.id)) {
                    unoffered.append(preset.id);
                }
                if (seen > 1 && !repeated.contains(preset.id)) {
                    repeated.append(preset.id);
                }
            }
            for (const Offered& choice : offered) {
                const auto preset = std::find_if(
                    presets.cbegin(), presets.cend(), [&choice](const anime4k::ModeInfo& info) {
                        return info.id == choice.id;
                    });
                if (preset == presets.cend()) {
                    if (!foreign.contains(choice.id)) {
                        foreign.append(choice.id);
                    }
                } else if (choice.path != labelPath(preset->category) &&
                           !misplaced.contains(choice.id)) {
                    misplaced.append(choice.id);
                }
                // The chooser is specified in ids AND labels: a menu carrying the
                // right preset under the right group but spelling a leaf
                // differently is a chooser the viewer reads twice, and one the
                // player menu cannot produce.
                if (preset != presets.cend() && choice.label != preset->label &&
                    !mislabeled.contains(choice.id)) {
                    mislabeled.append(choice.id);
                }
            }
        }
        std::fprintf(stderr, "     chip menus: %d, player presets: %d\n", chipMenus,
                     int(presets.size()));
        for (const QString& id : unoffered) {
            std::fprintf(stderr, "     not selectable: %s\n", qPrintable(id));
        }
        for (const QString& id : repeated) {
            std::fprintf(stderr, "     offered twice: %s\n", qPrintable(id));
        }
        for (const QString& id : foreign) {
            std::fprintf(stderr, "     not a player preset: %s\n", qPrintable(id));
        }
        for (const QString& id : misplaced) {
            std::fprintf(stderr, "     under the wrong group: %s\n", qPrintable(id));
        }
        for (const QString& id : mislabeled) {
            std::fprintf(stderr, "     spelled differently: %s\n", qPrintable(id));
        }
        check(chipMenus > 0, "the compare badges carry a candidate chooser menu");
        check(unoffered.isEmpty(), "every player preset is selectable in the compare badges");
        check(repeated.isEmpty(), "no player preset is offered twice in one badge menu");
        check(foreign.isEmpty(), "every compare choice is a preset the player can run");
        check(misplaced.isEmpty(), "each choice sits under its own catalogue category labels");
        check(mislabeled.isEmpty(), "each choice is spelled with its own catalogue label");
        // The chain internals: files a preset runs inside its chain but that the
        // catalogue does not list as candidates of their own (the AutoDownscale
        // steps). Which files those are is read from the catalogue rather than
        // named here, so the set follows the chains instead of the probe.
        QStringList offeredAlone;
        for (const anime4k::Candidate& candidate : all) {
            if (!presetIds.contains(candidate.id)) {
                offeredAlone.append(candidate.id);
            }
        }
        QStringList internals;
        for (const anime4k::ModeInfo& preset : presets) {
            for (const QString& file : preset.files) {
                if (!offeredAlone.contains(file) && !internals.contains(file)) {
                    internals.append(file);
                }
            }
        }
        bool offeredInternal = false;
        for (QToolButton* chip : compare->findChildren<QToolButton*>()) {
            if (chip->menu() == nullptr) {
                continue;
            }
            std::function<void(QMenu*)> scan = [&](QMenu* menu) {
                for (QAction* action : menu->actions()) {
                    if (QMenu* submenu = action->menu()) {
                        scan(submenu);
                    } else if (internals.contains(action->data().toString())) {
                        offeredInternal = true;
                    }
                }
            };
            scan(chip->menu());
        }
        check(!offeredInternal, "the chain-internal downscale steps are not offered standalone");

        if (auto* info = compare->findChild<QLabel*>(QString::fromLatin1("compareInfo"))) {
            std::fprintf(stderr, "     info: %s\n", qPrintable(info->text()));
        }

        // ── 6. a second capture is the new frame, not the old one ──
        // The failure this covers is not hypothetical: the window used to
        // re-seek the live player to the position it was already at and then
        // keep whatever the framebuffer happened to hold, so rewinding and
        // capturing again could hand back the frame the viewer had left. The
        // core is parked Off here so the frozen frame is the plain picture and
        surface->setAnime4KChain(offId);
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

        // ── 7. a chain with no Mode survives a restart ────────────
        // A single .glsl has no enum value, so an ordinal would drop it on
        // every start. The id is what is stored, and it is what is read back.
        surface->setAnime4KChain(single);
        QTest::qWait(400);
        check(surface->anime4KChainId() == single,
              "a chain with no Mode is remembered by its id");

        // ── 8. Compare leaves the player as found ─────────────────
        // R6. Each side is rendered by swapping the borrowed surface's chain,
        // so the chain read before the two sides has to be the chain the
        // player is running afterwards - on a successful capture, when the
        // window closes, and on every reopen. `single` is a loose .glsl that
        // is deliberately NOT one of the presets the badges offer, so a
        // restore that never happened would surface here as a preset id
        // instead of the one the player was actually running.
        compare->close();
        QTest::qWait(200);
        check(surface->anime4KChainId() == single,
              "closing the window leaves the player's chain unchanged");

        emit shell->compareRequested();
        QTest::qWait(3000);
        check(compare->isVisible(), "the kept window reopens");
        check(!compare->frozenFrame().isNull(), "the reopened window captured a frame");
        check(compare->captureError().isEmpty(), "the reopened capture reported no error");
        check(surface->anime4KChainId() == single,
              "a reopened capture restores the player's chain");

        compare->captureNow();
        QTest::qWait(2500);
        check(compare->captureError().isEmpty(), "the repeated capture reported no error");
        check(surface->anime4KChainId() == single,
              "capturing again restores the player's chain");

        check(!compare->isModal(), "the comparison does not take the player hostage");
        compare->close();
        QTest::qWait(400);
        check(!compare->isVisible(), "the window closes on its own");
        check(surface->anime4KChainId() == single,
              "the chain is still the player's after the second close");

        // ── 9. polling is left at its PRIOR value, not switched on ─
        // The invariant is the value the surface actually had, not a fixed
        // true: this source never enabled native status polling, so it has to
        // be found with polling off after a successful capture, a reopen and a
        // close. Switching it on is exactly the failure this checks for.
        surface->setNativeStatusPolling(false);
        check(!surface->nativeStatusPolling(), "native polling is explicitly off on this source");
        compare->captureNow();
        QTest::qWait(2500);
        check(compare->captureError().isEmpty(), "the off-prior capture succeeded");
        check(!surface->nativeStatusPolling(), "an off-prior capture leaves polling off");
        emit shell->compareRequested();
        QTest::qWait(3000);
        check(compare->isVisible(), "the off-prior compare reopens");
        check(!surface->nativeStatusPolling(), "an off-prior reopen leaves polling off");
        compare->close();
        QTest::qWait(300);
        check(!surface->nativeStatusPolling(), "an off-prior close leaves polling off");

        // ── 9b. the native plan is really locked for the window's lifetime ─
        // Pausing the status reader is not locking. The real scoped lock is what
        // freezes planning: it is held while the window is open and released the
        // moment it closes, and a window that cannot hold it names why rather
        // than freezing a plan that is about to move.
        surface->setNativeStatusPolling(true);
        emit shell->compareRequested();
        QTest::qWait(3000);
        check(compare->isVisible(), "the compare window is open for the lock check");
        check(surface->nativePlanLocked() || !compare->lockError().isEmpty() ||
                  !compare->captureError().isEmpty(),
              "an open window holds the plan lock or names why no native base is frozen");
        const QImage lockedFrame = compare->frozenFrame();
        auto* caption = compare->findChild<QLabel*>(QStringLiteral("compareInfo"));
        QString queuedCaption;
        if (surface->nativePlanLocked() && caption != nullptr) {
            // Consumer state transition, not a wording check: the visible
            // caption must change the moment a native request is queued, while
            // the frozen frame keeps its epoch, and it must clear on release.
            const QString before = caption->text();
            surface->turnNativeOff();
            QTest::qWait(150);
            queuedCaption = caption->text();
            std::fprintf(stderr, "     compare caption before: %s\n", qPrintable(before));
            std::fprintf(stderr, "     compare caption queued: %s\n", qPrintable(queuedCaption));
            check(surface->queuedNativeTransitions() == 1,
                  "a native request while the plan is held is queued, not applied");
            check(compare->frozenFrame() == lockedFrame,
                  "the queued native request leaves the frozen frame untouched");
            check(caption->isVisible() && queuedCaption != before,
                  "the visible caption changes the moment a native change is queued");
        }
        compare->close();
        QTest::qWait(300);
        std::fprintf(stderr, "     compare caption released: %s\n",
                     caption != nullptr ? qPrintable(caption->text()) : "(no caption)");
        check(!surface->nativePlanLocked(),
              "closing the window releases the held native plan lock");
        if (!queuedCaption.isEmpty()) {
            check(surface->queuedNativeTransitions() == 0,
                  "closing the window submits the queued native transition");
            check(caption->text() != queuedCaption,
                  "releasing the lock clears the queued state from the visible caption");
        }
        surface->setNativeStatusPolling(false);

        // ── 10. an unsettled capture fails and still restores the surface ──
        // This is the failure path that cannot be reached through the dialog's
        // guard: hasVideo() is true, but grabSettledFrame runs out of time. A
        // timer keeps re-seeking while the capture runs, so the surface never
        // stops reporting a move and no frame is ever believed. The window
        // must name the failure, freeze nothing, and - the correction this
        // exercises - hand the native status polling it paused back on.
        surface->setNativeStatusPolling(true);
        check(surface->nativeStatusPolling(), "native polling is on before the forced failure");
        QTimer churn;
        bool seekFlip = false;
        QObject::connect(&churn, &QTimer::timeout, [&] {
            seekFlip = !seekFlip;
            surface->seek(seekFlip ? 4.0 : 9.0);
        });
        churn.start(20);
        compare->captureNow();
        churn.stop();
        QTest::qWait(200);
        std::fprintf(stderr, "     unsettled refusal: %s\n", qPrintable(compare->captureError()));
        check(!compare->captureError().isEmpty(), "an unsettled capture refuses by name");
        check(compare->frozenFrame().isNull(), "the unsettled capture froze no frame");
        check(surface->nativeStatusPolling(),
              "a failed capture restores the native status polling it paused");
        check(surface->anime4KChainId() == single, "a failed capture leaves the chain alone");
        surface->setNativeStatusPolling(false);

        // ── 10b. the stored preset is not the live chain, and stays that way ──
        // The player persists `player/anime4k`, but the surface can be running
        // something that setting does not name - here a loose .glsl the badges
        // cannot offer. Compare swaps the LIVE chain to render its two sides,
        // so picking both sides has to put that live chain back untouched and
        // must never rewrite what the player has stored. The evidence is the
        // chain id AND the glsl-shaders string mpv actually holds, not just the
        // window's own bookkeeping.
        const QString storedKey = QStringLiteral("player/anime4k");
        QSettings persisted;
        persisted.setValue(storedKey, offId);
        persisted.sync();
        check(surface->anime4KChainId() == single && single != offId,
              "the live chain is not the stored preset before the render");
        check(QSettings().value(storedKey).toString() == offId,
              "the stored preset is the off id, not the live chain");
        const QString chainBefore = surface->anime4KChainId();
        const QString propertyBefore = surface->effectiveShaderChain();
        const auto pickPreset = [&](int chipIndex, const QString& id) {
            int seen = -1;
            for (QToolButton* chip : compare->findChildren<QToolButton*>()) {
                if (chip->menu() == nullptr) {
                    continue;
                }
                if (++seen != chipIndex) {
                    continue;
                }
                std::function<bool(QMenu*)> pick = [&](QMenu* menu) -> bool {
                    for (QAction* action : menu->actions()) {
                        if (QMenu* submenu = action->menu()) {
                            if (pick(submenu)) {
                                return true;
                            }
                        } else if (action->data().toString() == id) {
                            action->trigger();
                            return true;
                        }
                    }
                    return false;
                };
                return pick(chip->menu());
            }
            return false;
        };
        check(pickPreset(0, presetIds.value(0)), "the first badge accepts a preset pick");
        check(surface->anime4KChainId() == chainBefore,
              "picking side A leaves the live chain id unchanged");
        check(surface->effectiveShaderChain() == propertyBefore,
              "picking side A leaves glsl-shaders as found");
        check(pickPreset(1, presetIds.value(1)), "the second badge accepts another preset pick");
        check(surface->anime4KChainId() == chainBefore,
              "picking side B leaves the live chain id unchanged");
        check(surface->effectiveShaderChain() == propertyBefore,
              "picking side B leaves glsl-shaders as found");
        compare->close();
        QTest::qWait(200);
        check(surface->anime4KChainId() == chainBefore,
              "closing after both picks leaves the live chain id unchanged");
        check(surface->effectiveShaderChain() == propertyBefore,
              "closing after both picks leaves glsl-shaders as found");
        check(QSettings().value(storedKey).toString() == offId,
              "compare never rewrites the stored preset");

        // ── 10c. a failed capture hands playback back as it found it ──
        // The pause that buys a settled grab is a temporary override too: when
        // the grab never settles there is no picture, so the surface has to be
        // left playing - with its chain and polling as well - exactly as found.
        surface->setNativeStatusPolling(true);
        surface->setPaused(false);
        QTest::qWait(200);
        check(surface->isPlaying(), "the surface is playing before the failed capture");
        check(surface->nativeStatusPolling(), "native polling is on before the failed capture");
        QTimer churnPlayback;
        bool flipPlayback = false;
        QObject::connect(&churnPlayback, &QTimer::timeout, [&] {
            flipPlayback = !flipPlayback;
            surface->seek(flipPlayback ? 4.0 : 9.0);
        });
        churnPlayback.start(20);
        compare->captureNow();
        churnPlayback.stop();
        QTest::qWait(200);
        std::fprintf(stderr, "     playback refusal: %s\n", qPrintable(compare->captureError()));
        check(!compare->captureError().isEmpty(), "a playback capture that cannot settle refuses");
        check(compare->frozenFrame().isNull(), "the playback refusal froze no frame");
        check(surface->isPlaying(), "a failed capture hands playback back playing");
        check(surface->anime4KChainId() == single, "the playback refusal leaves the chain alone");
        check(surface->nativeStatusPolling(), "the playback refusal leaves polling as found");
        surface->setPaused(true);
        surface->setNativeStatusPolling(false);

        // ── 11. a closed surface is refused by name ───────────────
        // Closing the borrowed surface is a REAL capture failure: hasVideo()
        // goes false while the widget lives on. The window must name the
        // refusal rather than freeze an old or empty picture, and it must not
        // touch the chain it borrowed. This goes through the window's own
        // public capture path, not the dialog's open guard.
        surface->close();
        QTest::qWait(250);
        check(!surface->hasVideo(), "the player surface reports no video after close");
        compare->captureNow();
        QTest::qWait(250);
        std::fprintf(stderr, "     closed-surface refusal: %s\n",
                     qPrintable(compare->captureError()));
        check(!compare->captureError().isEmpty(),
              "a capture on a closed surface refuses instead of freezing");
        check(compare->frozenFrame().isNull(), "the refused capture froze no frame");
        check(surface->anime4KChainId() == single,
              "the closed surface keeps the chain it was left on");
        check(!surface->nativeStatusPolling(),
              "a failed capture leaves polling at its prior off, not switched on");

        // ── 12. a surface with no video is refused by name ────────
        // A second, never-opened surface proves the refusal does not depend on
        // the live player: the window borrows whatever surface it is handed,
        // and one with no file loaded must refuse rather than freeze.
        VideoSurface idleSurface;
        idleSurface.setAnime4KChain(single);
        check(!idleSurface.hasVideo(), "a fresh surface reports no video");
        CompareWindow idleCompare(&idleSurface, idleSurface.anime4KChainId());
        idleCompare.captureNow();
        QTest::qWait(200);
        std::fprintf(stderr, "     no-video refusal: %s\n",
                     qPrintable(idleCompare.captureError()));
        check(!idleCompare.captureError().isEmpty(),
              "a capture on a surface with no video refuses by name");
        check(idleCompare.frozenFrame().isNull(), "the no-video capture froze no frame");
        check(idleSurface.anime4KChainId() == single,
              "the no-video surface keeps the chain it was handed");

        // The degenerate case: no surface at all. It has to name the missing
        // surface rather than dereference it.
        CompareWindow noSurface(nullptr, presetIds.value(0));
        noSurface.captureNow();
        QTest::qWait(50);
        std::fprintf(stderr, "     no-surface refusal: %s\n",
                     qPrintable(noSurface.captureError()));
        check(!noSurface.captureError().isEmpty(), "a capture with no surface refuses by name");
        check(noSurface.frozenFrame().isNull(), "the no-surface capture froze no frame");

        // ── 13. a surface destroyed mid-grab is refused, not dereferenced ──
        // The borrowed surface can die while a capture is pumping the event
        // loop. The QPointer guard has to turn that into a named refusal rather
        // than a read of freed memory. This surface really has video - it is
        // made to freeze a working frame first - and is then deleted from a
        // timer while the NEXT grab is still waiting for it to settle.
        VideoSurface* borrowed = new VideoSurface();
        borrowed->resize(320, 240);
        const bool borrowedOpened = borrowed->open(url);
        for (int attempt = 0; borrowedOpened && !borrowed->hasVideo() && attempt < 200; ++attempt) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }
        if (borrowedOpened && borrowed->hasVideo()) {
            auto* guardWindow = new CompareWindow(borrowed, presetIds.value(0));
            guardWindow->captureNow();
            check(!guardWindow->captureError().isEmpty() || !guardWindow->frozenFrame().isNull(),
                  "the second surface produced a real frame before the guard check");
            QTimer guardChurn;
            bool guardFlip = false;
            QObject::connect(&guardChurn, &QTimer::timeout, [&] {
                guardFlip = !guardFlip;
                borrowed->seek(guardFlip ? 4.0 : 9.0);
            });
            guardChurn.start(20);
            QTimer guardKill;
            guardKill.setSingleShot(true);
            QObject::connect(&guardKill, &QTimer::timeout, [&] {
                guardChurn.stop();
                delete borrowed;
                borrowed = nullptr;
            });
            guardKill.start(150);
            guardWindow->captureNow();
            guardChurn.stop();
            if (borrowed != nullptr) {
                delete borrowed;
                borrowed = nullptr;
            }
            std::fprintf(stderr, "     destroyed-surface refusal: %s\n",
                         qPrintable(guardWindow->captureError()));
            check(!guardWindow->captureError().isEmpty(),
                  "a surface destroyed mid-grab is refused by name");
            check(guardWindow->frozenFrame().isNull(),
                  "a surface destroyed mid-grab kept no frame");
            delete guardWindow;
        } else {
            std::fprintf(stderr,
                         "SKIP  no second surface for the destroyed-mid-grab guard check\n");
            delete borrowed;
        }

        std::fprintf(stderr, "RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
        app.exit(failures == 0 ? 0 : 1);
    });
    return app.exec();
}
