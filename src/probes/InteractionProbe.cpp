// Interaction probe: the dock's dub/subtles buttons, the gear's quality list,
// and the things that only break in use.
//
//   - a seek crashed the process: mpv_command wants a NULL-terminated array
//   - the quality and track popovers closed on every shell state change
//   - returning from Alt+Tab left nothing focused
//   - quality and dub/sub selection sat in a top strip, above the picture

#include <QApplication>
#include <QImage>
#include <QGraphicsOpacityEffect>
#include <QPointer>
#include <QTest>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <functional>

#include "PlayerDialog.hpp"
#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
#include "ShellSliders.hpp"
#include "ShellWidgets.hpp"
#include "VideoSurface.hpp"

using namespace mplayer;
using namespace mplayer::shell;

namespace {

class StubHost : public PlayerHost {
public:
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestOpenOriginal() override {}
    void requestCopyStreamUrl() override {}
    // The dialog carries WA_DeleteOnClose, so once Esc closes it the pointer is
    // dangling and isVisible() would read freed memory. The host is told, and
    // it outlives the dialog: that is the only safe way to observe the close.
    void playerClosed() override { closed = true; }
    void playbackFailed() override {}

    bool closed = false;
};

QPushButton* buttonWithText(QWidget* root, const QString& needle) {
    for (QPushButton* button : root->findChildren<QPushButton*>()) {
        if (button->text().contains(needle)) return button;
    }
    return nullptr;
}

/// The row whose text is exactly `text` and which is actually on screen. A
/// popover rebuilds its rows rather than editing them - the replaced ones are
/// hidden and deleted through the event loop - so a plain child search can
/// hand back a row that has already left the menu.
QPushButton* visibleRowWithText(QWidget* root, const QString& text) {
    if (root == nullptr) return nullptr;
    for (QPushButton* button : root->findChildren<QPushButton*>()) {
        if (button->isVisible() && button->text() == text) return button;
    }
    return nullptr;
}

/// Waits, bounded, for the player's own window to be back - and to stay back
/// for a beat rather than only for the instant it is sampled.
///
/// The dock's popovers are children of that window, and the probe's own later
/// stages share this event loop: the PiP stage hides the window for about ten
/// seconds and hands it back only when the float is restored, and the shell
/// re-runs its state pass on the way back, which is free to dismiss whatever
/// overlay was up. A menu step taken across that hand-over would drive a dock
/// that is not on screen yet. Requiring the window to be visible twice, about
/// half a second apart, also catches a stage that is about to take it away
/// again - which is this probe's own PiP stage, arriving while the step before
/// it is still running.
bool playerWindowIsBack(const QPointer<QWidget>& dialog, int timeoutMs = 14000) {
    int heldVisibleFor = 0;
    for (int waited = 0; waited < timeoutMs; waited += 50) {
        if (dialog.isNull()) return false;
        QCoreApplication::processEvents();
        if (dialog->isVisible()) {
            heldVisibleFor += 50;
            if (heldVisibleFor >= 800) return true;
        } else {
            heldVisibleFor = 0;
        }
        QThread::msleep(50);
    }
    return false;
}

/// The row `text` in a popover of `host`, re-fetched and waited for rather than
/// assumed.
///
/// A popover replaces its rows instead of editing them and retires the old ones
/// through the event loop, so a row has to be looked up after the transition
/// that rebuilt the menu, and it has to come out of the popover that is really
/// on screen. Both popovers are children of the player's dialog, and the
/// probe's own stages share this one event loop: the PiP stage hides that
/// dialog, and a popover under a hidden host - rows and all - is invisible
/// until the host is back. The look-up is therefore retried, bounded: generous
/// enough to span that stage, finite so that a row which never arrives fails
/// its check instead of hanging the probe.
QPushButton* popupRowWhenVisible(const QPointer<QWidget>& host, const QString& popupName,
                                 const QString& text, int timeoutMs = 12000) {
    for (int waited = 0; waited < timeoutMs; waited += 40) {
        if (host.isNull()) return nullptr;
        QCoreApplication::processEvents();
        QWidget* popup = host->findChild<QWidget*>(popupName);
        if (popup != nullptr && popup->isVisible()) {
            if (QPushButton* row = visibleRowWithText(popup, text)) return row;
        }
        QThread::msleep(40);
    }
    return nullptr;
}

bool buttonsFitWithoutOverlap(QWidget* popup) {
    QVector<QRect> occupied;
    for (QPushButton* button :
         popup->findChildren<QPushButton*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!button->isVisible()) continue;
        const QRect bounds(button->mapTo(popup, QPoint()), button->size());
        if (!popup->rect().contains(bounds)) return false;
        for (const QRect& other : occupied) {
            if (bounds.intersects(other)) return false;
        }
        occupied.append(bounds);
    }
    return !occupied.isEmpty();
}

/// The row the probe drives: Mode A (Fast) is the chain Anime4K documents for
/// 720p input, which is exactly the clip the probe plays.
constexpr auto kAnime4KProbeMode = anime4k::Mode::FastA;
constexpr double kAnime4KProbeSeek = 2.0;

/// Where the picture actually sits inside a framebuffer: mpv fits the video to
/// the widget and centres it, so the band is recoverable from the source
/// aspect alone. A square at the centre of that band cannot reach a bar.
QRect pictureArea(const QImage& frame, int sourceWidth, int sourceHeight) {
    if (frame.isNull() || sourceWidth <= 0 || sourceHeight <= 0) {
        return QRect();
    }
    const double scale = std::min(static_cast<double>(frame.width()) / sourceWidth,
                                  static_cast<double>(frame.height()) / sourceHeight);
    const int side = static_cast<int>(std::min(sourceWidth, sourceHeight) * scale);
    if (side < 32) return QRect();
    return QRect((frame.width() - side) / 2, (frame.height() - side) / 2, side, side);
}

/// Mean absolute difference between two frames over the picture band.
///
/// mpv's vertex shader maps the frame to the widget, so enabling a chain does
/// not change the output SIZE - it changes the picture drawn into the same
/// rectangle. Two stable frames of a paused clip therefore differ only if the
/// chain rebuilt itself, which is exactly the question.
///
/// The area is derived from the source aspect so it stays clear of the
/// letterbox bars. A bar edge reads as a very large difference and would
/// swamp the signal; the centre of a portrait framebuffer is the only place
/// guaranteed to be picture.
double pictureDifference(const QImage& before, const QImage& after) {
    const QRect area = pictureArea(before, 1280, 720);
    if (before.isNull() || after.isNull() || area.width() < 32 || area.height() < 32) {
        return -1.0;
    }
    const QRect same = area.intersected(QRect(QPoint(0, 0), after.size())).size() == area.size()
                          ? area
                          : QRect();
    if (same.isEmpty()) return -1.0;
    const QImage a = before.convertToFormat(QImage::Format_RGB888);
    const QImage b = after.convertToFormat(QImage::Format_RGB888);
    const int step = std::max(1, std::min(area.width(), area.height()) / 160);
    double total = 0.0;
    int counted = 0;
    for (int y = area.top(); y + step < area.bottom(); y += step) {
        for (int x = area.left(); x + step < area.right(); x += step) {
            const QRgb pa = a.pixel(x, y);
            const QRgb pb = b.pixel(x, y);
            total += std::abs(qRed(pa) - qRed(pb)) + std::abs(qGreen(pa) - qGreen(pb)) +
                     std::abs(qBlue(pa) - qBlue(pb));
            counted += 3;
        }
    }
    return counted > 0 ? total / static_cast<double>(counted) : -1.0;
}

/// Pauses on one source position, so every later frame is the same picture.
void freezeOn(VideoSurface* surface) {
    if (surface == nullptr) return;
    if (surface->isPlaying()) {
        surface->togglePause();
    }
    surface->seek(kAnime4KProbeSeek);
    for (int i = 0; i < 40; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(40);
    }
}

/// A framebuffer that has stopped changing, WITHOUT moving the playhead.
///
/// mpv re-renders on every exposure and the downscaler is not bit-exact, so
/// two grabs of one still frame differ slightly. Polling until two
/// consecutive grabs match makes the frame a fixed reference.
///
/// This must not seek: seeking to the same timestamp twice does not reproduce
/// the same pixels, and a helper that re-seeks turns the "noise floor"
/// measurement into a frame-to-frame difference instead - which was measured
/// at ~5/255 here, the same order as the effect, and made every comparison
/// uninterpretable.
QImage settledFrame(VideoSurface* surface) {
    if (surface == nullptr) return QImage();
    QImage previous;
    for (int i = 0; i < 80; ++i) {
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

/// The float, kept here so the ownership checks can run after the step that
/// opened it has returned. A QPointer because leavePip deleteLater()s it.
QPointer<PipFloat> pipFloat;

/// Where the float's core was when the viewer pressed restore.
double floatExitPosition = 0.0;

int failures = 0;

void check(bool ok, const char* what) {
    std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

QRect centreOf(const QRect& rect) {
    return QRect(rect.left() + rect.width() / 4, rect.top() + rect.height() / 4, rect.width() / 2,
                 rect.height() / 2);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    static StubHost host;
    const QString url = QStringLiteral("av://lavfi:testsrc2=size=1280x720:rate=25");
    const QList<QualityEntry> qualities = {{480, url}, {720, url}, {1080, url}};

    auto* dialog = new PlayerDialog(QStringLiteral("probe 720p"), url, qualities, 720, &host);
    // Two dubs and two subtitle tracks, so both dock buttons have to appear.
    // A playlist, the way TitlePage::openPlayer hands one over. Without it
    // seriesButton_ stays hidden and the series drawer is never built, so the
    // whole "a populated view empties itself a beat later" class went untested.
    QVector<QVariantMap> playlist;
    for (int episode = 1; episode <= 5; ++episode) {
        const QString label = QStringLiteral("Серія %1").arg(episode);
        playlist.append(QVariantMap{{"label", label},
                                    {"embed", QStringLiteral("e%1").arg(episode)}});
    }
    dialog->setPlaylist(playlist, 2);
    dialog->setVariants({QVariantMap{{"label", QStringLiteral("Ashdi 1080p")},
                                     {"embed", QStringLiteral("e1")},
                                     {"kind", QStringLiteral("voice")}},
                         QVariantMap{{"label", QStringLiteral("Цукіхана 720p")},
                                     {"embed", QStringLiteral("e2")},
                                     {"kind", QStringLiteral("voice")}},
                         QVariantMap{{"label", QStringLiteral("Суб 480p")},
                                     {"embed", QStringLiteral("e3")},
                                     {"kind", QStringLiteral("sub")}},
                         QVariantMap{{"label", QStringLiteral("Суб 1080p")},
                                     {"embed", QStringLiteral("e4")},
                                     {"kind", QStringLiteral("sub")}}},
                        QStringLiteral("e1"));
    dialog->startPlayer();
    // The real player opens fullscreen, and a fullscreen owner is a different
    // stacking case: a popover can sit behind it. Reproduce that, not a
    // normal window that happens to agree.
    dialog->showFullScreen();

    auto* shell = dialog->findChild<PlayerShell*>();
    auto* surface = dialog->findChild<VideoSurface*>();
    auto* audio = dialog->findChild<QWidget*>(QStringLiteral("playerAudioPopup"));
    auto* subs = dialog->findChild<QWidget*>(QStringLiteral("playerSubtitlesPopup"));
    auto* strip = dialog->findChild<QWidget*>(QStringLiteral("playerFeatureStrip"));

    QTimer::singleShot(2500, [&] {
        if (surface == nullptr) {
            check(false, "surface exists");
            app.exit(1);
            return;
        }
        surface->seek(1.0);
        surface->seek(0.5);
        check(true, "two seeks survived (no crash)");
    });

    // The strip is no longer where stream choices live.
    QTimer::singleShot(3000, [&] {
        check(strip != nullptr, "the feature strip exists");
        check(buttonWithText(strip, QStringLiteral("Якість")) == nullptr,
              "the strip has no quality chip");
        check(buttonWithText(strip, QStringLiteral("Доріжки")) == nullptr,
              "the strip has no tracks chip");
    });

    // Both dock buttons are there, and each lists only its own kind.
    QTimer::singleShot(6000, [&] {
        if (surface != nullptr) {
            for (const QPointF& at : {QPointF(40, 40), QPointF(60, 60)}) {
                QMouseEvent move(QEvent::MouseMove, at, surface->mapToGlobal(at), Qt::NoButton,
                                 Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(surface, &move);
            }
        }
        bool fullscreenButtonFound = false;
        bool resetButtonFound = false;
        for (const QPushButton* button : dialog->findChildren<QPushButton*>()) {
            if (button->toolTip().startsWith(QStringLiteral("Озвучення"))) {
                check(button->isVisible(), "the dub button is on the dock");
            }
            if (button->toolTip().startsWith(QStringLiteral("Субтитри"))) {
                check(button->isVisible(), "the subtitle button is on the dock");
            }
            fullscreenButtonFound |=
                button->toolTip().startsWith(QStringLiteral("toggle_fullscreen_mode"));
            resetButtonFound |=
                button->toolTip().startsWith(QStringLiteral("reset_canvas_transform"));
        }
        check(!fullscreenButtonFound, "the redundant fullscreen button is absent");
        check(!resetButtonFound, "the separate reset button is absent");

        auto* volume = shell->findChild<VolumeGroup*>();
        auto* scrub = shell->findChild<ScrubBar*>();
        QPushButton* play = nullptr;
        for (QPushButton* button : shell->findChildren<QPushButton*>()) {
            if (button->toolTip().startsWith(QStringLiteral("toggle_playback"))) {
                play = button;
                break;
            }
        }
        check(volume != nullptr && volume->width() >= 180,
              "the volume slider is permanently expanded");
        check(play != nullptr && play->width() >= 52 && play->height() >= 52,
              "the dock uses a large primary play control");
        check(play != nullptr && scrub != nullptr &&
                  play->mapTo(shell, play->rect().center()).y() <
                      scrub->mapTo(shell, scrub->rect().center()).y(),
              "the playback controls sit above the timeline");
    });

    // The play control has to follow the engine, and it used to be frozen on
    // the glyph it was built with: the surface recorded a pause without ever
    // announcing it, so the state pump had nothing to report and the button
    // showed pause over a video that had stopped. Read at this point in the
    // sequence because the PiP section below asserts the pause state it finds.
    QTimer::singleShot(6500, &app, [&] {
        auto* mainSurface = dialog->findChild<VideoSurface*>();
        auto* play = dialog->findChild<IconButton*>(QStringLiteral("playerPlayToggle"));
        check(play != nullptr, "the dock has a play control");
        if (play == nullptr || mainSurface == nullptr || !mainSurface->hasVideo()) {
            check(play != nullptr && mainSurface != nullptr,
                  "a stream was loaded for the play-glyph check");
            return;
        }
        // The glyph is read off the button, not off a grab: play and pause are
        // painted by the same class at the same size.
        if (mainSurface->isPlaying()) {
            check(play->icon() == &IconPainter::pause,
                  "the play control shows pause while the video runs");
            play->click();
            QTest::qWait(700);
            check(!mainSurface->isPlaying(), "the control paused the engine");
            check(play->icon() == &IconPainter::play,
                  "the play control shows play while the video is paused");
        } else {
            check(play->icon() == &IconPainter::play,
                  "the play control shows play while the video is paused");
            play->click();
            QTest::qWait(700);
            check(mainSurface->isPlaying(), "the control resumed the engine");
            check(play->icon() == &IconPainter::pause,
                  "the play control shows pause while the video runs");
        }
        // Left exactly as found: the PiP ownership checks further down expect
        // to enter from a paused player.
        if (mainSurface->isPlaying()) {
            play->click();
            QTest::qWait(700);
        }
        check(!mainSurface->isPlaying(), "the player is left paused for the PiP section");
    });
    // An episode swap from a paused player is where the play control went
    // stale: the hand-back told the shell it was paused while the core had
    // already resumed, so the button showed "play" over a running video for
    // as long as the viewer looked at it. applyEpisode is the public path
    // that swaps in place and carries the pause across.
    QTimer::singleShot(6300, &app, [&] {
        auto* mainSurface = dialog->findChild<VideoSurface*>();
        auto* play = dialog->findChild<IconButton*>(QStringLiteral("playerPlayToggle"));
        if (mainSurface == nullptr || play == nullptr || !mainSurface->hasVideo()) {
            std::fprintf(stderr,
                         "SKIP  the episode-swap check: no stream loaded in this session\n");
            return;
        }
        if (mainSurface->isPlaying()) {
            mainSurface->setPaused(true);
            QTest::qWait(500);
        }
        check(!mainSurface->isPlaying(), "the player is paused before the swap");
        check(play->icon() == &IconPainter::play, "the control reads play while paused");
        QVector<QVariantMap> swap;
        for (int episode = 1; episode <= 5; ++episode) {
            swap.append(QVariantMap{{"label", QStringLiteral("Серія %1").arg(episode)},
                                     {"embed", QStringLiteral("e%1").arg(episode)}});
        }
        mainSurface->setPaused(false);
        QTest::qWait(900);
        check(mainSurface->position() > 2.0, "the player is past the start before the swap");
        const double beforeSwap = mainSurface->position();
        mainSurface->setPaused(true);
        QTest::qWait(400);
        dialog->applyEpisode(QStringLiteral("Серія 4"), QStringLiteral("e4"), 3, qualities, swap);
        for (int i = 0; i < 80; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
            if (!mainSurface->isPlaying() && play->icon() == &IconPainter::play) break;
        }
        check(!mainSurface->isPlaying(), "the swapped stream came back paused");
        check(play->icon() == &IconPainter::play,
              "an episode swap from a paused core leaves the control on play");
        // Wait for the swap to land. hasVideo() is no use as a signal here: it
        // stays true across a load, and VideoSurface keeps its cached position
        // until the pump reads a new time-pos - so sampling straight after the
        // call reads the position of the stream that was just replaced.
        for (int i = 0; i < 60; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
            if (mainSurface->position() < 2.0) break;
        }
        // A new episode starts at its beginning whatever the old one was doing,
        // and it keeps the pause state - a viewer who paused to step forward
        // wants to choose when the new one starts. Position is what the fix is
        // about, so that is what is asserted here.
        std::fprintf(stderr, "     position after swap: %.2fs (was %.2fs before)\n",
                     mainSurface->position(), beforeSwap);
        check(mainSurface->position() < 2.0,
              "a swapped episode starts at 00:00, not at the old offset");
        // Left paused, because the PiP ownership checks further down enter from
        // a paused player and assert that they return to it. A freshly loaded
        // core comes up playing, and the pause has to be re-asserted once the
        // load has actually landed - asking a core that has not started yet is
        // asking a core that will ignore the answer.
        for (int i = 0; i < 40; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
            if (mainSurface->duration() > 0.0) break;
        }
        mainSurface->setPaused(true);
        QTest::qWait(600);
        check(!mainSurface->isPlaying(), "the player is left paused for the PiP section");
    });
    // The gear carries quality, using mikai's own values.
    QTimer::singleShot(7000, [&] {
        if (surface != nullptr) {
            for (const QPointF& at : {QPointF(40, 40), QPointF(60, 60)}) {
                QMouseEvent move(QEvent::MouseMove, at, surface->mapToGlobal(at), Qt::NoButton,
                                 Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(surface, &move);
            }
        }
    });

    QTimer::singleShot(7500, [&] {
        if (shell != nullptr) shell->openSettings();
        auto* flyout = dialog->findChild<SettingsFlyout*>();
        check(flyout != nullptr && flyout->isVisible(), "the gear opened its popover");
        if (flyout == nullptr) return;
        check(!flyout->isWindow() && flyout->parentWidget() == dialog,
              "the gear popover uses the fullscreen overlay plane");
        int rows = 0;
        for (QPushButton* button : flyout->findChildren<QPushButton*>()) {
            if (button->text().endsWith("p")) ++rows;
        }
        check(rows == 3, "the popover lists all three qualities");
        check(buttonsFitWithoutOverlap(flyout), "the settings rows fit without overlap");
        auto* infoChip = shell->findChild<QWidget*>(QStringLiteral("playerInfoChip"));
        auto* networkStats =
            shell->findChild<QLabel*>(QStringLiteral("playerNetworkStats"));
        check(infoChip != nullptr && infoChip->isVisible() && infoChip->y() <= 12,
              "the MIKAI status chip is at the top-left");
        check(networkStats != nullptr &&
                  networkStats->text().contains(QStringLiteral("Mbps")) &&
                  networkStats->text().contains(QStringLiteral("FPS")),
              "the status chip shows network throughput and stream FPS");
        flyout->grab().toImage().save(QStringLiteral("flyout_probe.png"));
        auto* series = dialog->findChild<QPushButton*>(QStringLiteral("playerSeriesToggle"));
        check(series != nullptr && series->isVisible(),
              "the series toggle is still on the strip");
        // Keep a full-screen artifact: this caught the old native popup
        // rendering correctly in isolation while disappearing from the display.
        QTest::qWait(100);
        QApplication::processEvents();
        dialog->grab().save(QStringLiteral("dialog_flyout.png"));
        if (auto* screen = QGuiApplication::primaryScreen()) {
            screen->grabWindow(0).save(QStringLiteral("screen_flyout.png"));
        }
    });

    // Anime4K is only real if libmpv actually runs the chain, and mpv's own
    // glsl-shaders property cannot be used to find that out: it hands back the
    // string it was given without validating it, so a preset naming a deleted
    // file still reads back whole - confirmed by deleting one shader and
    // watching the comparison pass anyway. The proof has to be the pixels.
    QTimer::singleShot(8000, [&] {
        const QString shadersDir = anime4k::findShadersDir();
        check(!shadersDir.isEmpty(), "the shader folder sits beside the exe");
        check(anime4k::shaderStringForCandidate(QStringLiteral("off"), shadersDir).isEmpty(),
              "Off resolves to no chain at all");
        // Every file a preset references has to exist on disk. Deleting one is
        // what proves this can fail: the presets naming it drop out and the
        // suite says exactly which.
        int missingPresets = 0;
        int checkedPresets = 0;
        for (const anime4k::ModeInfo& info : anime4k::availableModes()) {
            if (info.mode == anime4k::Mode::Off) continue;
            ++checkedPresets;
            if (!anime4k::chainIsComplete(anime4k::candidateForId(info.id, shadersDir),
                                          shadersDir)) {
                std::fprintf(stderr, "FAIL preset %s is missing a file\n", qPrintable(info.id));
                ++missingPresets;
            }
        }
        std::fprintf(stderr, "     presets checked: %d\n", checkedPresets);
        check(checkedPresets > 10 && missingPresets == 0,
              "every preset resolves to files that exist");

        QPushButton* enhancementsButton = nullptr;
        for (QPushButton* button : shell->findChildren<QPushButton*>()) {
            if (button->toolTip() == QStringLiteral("open_enhancements")) {
                enhancementsButton = button;
                break;
            }
        }
        check(enhancementsButton != nullptr && enhancementsButton->isVisible(),
              "the enhancements button is on the dock");
        if (enhancementsButton == nullptr) return;
        enhancementsButton->click();

        auto* enhancements =
            dialog->findChild<MenuPopup*>(QStringLiteral("playerEnhancementsPopup"));
        check(enhancements != nullptr && enhancements->isVisible(),
              "the enhancements menu opened");
        check(enhancements != nullptr && !enhancements->isWindow() &&
                  enhancements->parentWidget() == dialog,
              "the enhancements menu uses the fullscreen overlay plane");
        // A model this player does not run is an enabled row that opens its own
        // page in the shared popover - one disabled В планах leaf and a way
        // back - rather than a dead row left in the menu.
        QPushButton* rifeEntry =
            enhancements != nullptr ? visibleRowWithText(enhancements, QStringLiteral("RIFE  ›"))
                                    : nullptr;
        check(rifeEntry != nullptr && rifeEntry->isEnabled(),
              "a planned model opens its page from an enabled row");
        if (rifeEntry == nullptr) return;
        rifeEntry->click();
        QTest::qWait(60);

        auto* plannedPage =
            dialog->findChild<MenuPopup*>(QStringLiteral("playerAnime4KPopup"));
        check(plannedPage != nullptr && plannedPage->isVisible(),
              "the planned model's page opened in the shared popover");
        QPushButton* plannedRow =
            plannedPage != nullptr ? visibleRowWithText(plannedPage, QStringLiteral("В планах"))
                                   : nullptr;
        check(plannedRow != nullptr && !plannedRow->isEnabled(),
              "a planned model is listed and cannot be chosen");
        const anime4k::Mode beforeRifePage = surface->anime4KMode();
        if (plannedRow != nullptr) {
            plannedRow->click();
            QTest::qWait(40);
        }
        check(surface->anime4KMode() == beforeRifePage,
              "clicking a planned model leaves the chain alone");
        QPushButton* plannedBack =
            plannedPage != nullptr ? visibleRowWithText(plannedPage, QStringLiteral("‹  Назад"))
                                   : nullptr;
        check(plannedBack != nullptr, "the planned page offers a way back");
        if (plannedBack == nullptr) return;
        plannedBack->click();
        QTest::qWait(60);
        QPushButton* anime4kEntry =
            enhancements != nullptr ? visibleRowWithText(enhancements, QStringLiteral("Anime4K  ›"))
                                    : nullptr;
        check(anime4kEntry != nullptr, "the enhancements menu offers Anime4K");
        if (anime4kEntry == nullptr) return;
        anime4kEntry->click();
        QTest::qWait(60);

        auto* anime4kPopup =
            dialog->findChild<MenuPopup*>(QStringLiteral("playerAnime4KPopup"));
        check(anime4kPopup != nullptr && anime4kPopup->isVisible(),
              "the Anime4K submenu opened");

        // The presets hang under categories, so the row is two clicks in - and
        // the category itself must not change the chain on the way.
        const anime4k::Mode beforeCategory = surface->anime4KMode();
        QPushButton* speedRow =
            anime4kPopup != nullptr ? visibleRowWithText(anime4kPopup, QStringLiteral("Швидкість  ›"))
                                    : nullptr;
        check(speedRow != nullptr, "the Anime4K submenu offers the Швидкість category");
        if (speedRow == nullptr) return;
        speedRow->click();
        QTest::qWait(60);
        check(surface->anime4KMode() == beforeCategory,
              "opening a category does not change the applied chain");
        check(anime4kPopup != nullptr && anime4kPopup->isVisible(),
              "the category opened in place instead of a second window");
        check(visibleRowWithText(anime4kPopup, QStringLiteral("‹  Назад")) != nullptr,
              "a drilled-down level offers a way back");

        QString label;
        for (const anime4k::ModeInfo& info : anime4k::availableModes()) {
            if (info.mode == kAnime4KProbeMode) label = info.label;
        }
        QPushButton* row =
            anime4kPopup != nullptr ? visibleRowWithText(anime4kPopup, label) : nullptr;
        check(row != nullptr, "the Швидкість category lists its presets");
        if (row == nullptr) return;

        // A framebuffer only exists once the window has been exposed, which
        // does not happen in a headless session. That is not a defect, so the
        // pixel comparison is skipped rather than failed - and skipping says
        // so, because a probe that silently reports PASS while proving
        // nothing is worse than one that admits it could not run.
        if (!surface->waitForContext(4000)) {
            std::fprintf(stderr, "SKIP  the pixel comparison: no render context in this session\n");
            shell->dismissOverlay();
            return;
        }

        // Freeze once. Everything after this reads the same source position,
        // so the only variable left is the shader chain.
        freezeOn(surface);

        // The noise floor: two settled frames with nothing touched between
        // them. mpv re-renders on exposure, so this is not zero - and a
        // before/after comparison only means something against it.
        const QImage noiseA = settledFrame(surface);
        const QImage noiseB = settledFrame(surface);
        const double noise = pictureDifference(noiseA, noiseB);
        std::fprintf(stderr, "     control: two frames, nothing changed  %.3f / 255\n", noise);
        check(noise >= 0.0, "the two control frames are comparable");

        const QImage plainFrame = settledFrame(surface);
        row->click();
        check(surface->anime4KMode() == kAnime4KProbeMode, "the press reached the surface");
        check(surface->effectiveShaderChain().contains(QStringLiteral("Anime4K_Upscale_CNN_x2_M")),
              "the running chain names the upscaler");
        auto* enhancementStatus =
            shell->findChild<QLabel*>(QStringLiteral("playerEnhancementStatus"));
        check(enhancementStatus != nullptr && !enhancementStatus->isHidden() &&
                  enhancementStatus->text().contains(QStringLiteral("→")),
              "the top-left chip includes enhancement and source-to-output resolution");
        // vo=libmpv rebuilds the chain on the next rendered frame and the
        // shaders compile on load, so this is not instant.
        QTest::qWait(3000);

        const QImage chainFrame = settledFrame(surface);
        check(!chainFrame.isNull(), "a still frame with the chain on");
        if (plainFrame.isNull() || chainFrame.isNull() || noise < 0.0) {
            shell->dismissOverlay();
            return;
        }
        const double delta = pictureDifference(plainFrame, chainFrame);
        std::fprintf(stderr, "     chain on vs off %.3f / 255, against %.3f noise\n", delta, noise);
        // A Restore + 2x upscale rewrites essentially every pixel it touches.
        // The upscalers are gated on OUTPUT being >1.2x the source - that is
        // Anime4K refusing to upscale a video already displayed at source -
        // so on a small surface only the restore half runs. Requiring a large
        // multiple of the measured floor keeps this from passing on re-render
        // noise while still accepting a restore-only chain.
        // The floor is zero, so any non-zero difference is the chain. The
        // absolute value is small (0.4/255) because the upscalers are gated on
        // OUTPUT being >1.2x the source - on a surface this size mpv displays
        // the video at source and Anime4K declines to upscale, leaving only
        // the ungated restore half. What is being asserted is that the chain
        // runs, not that it is worth enabling at this size; that judgement
        // belongs to a viewer, not a threshold.
        check(noise == 0.0, "the frozen frame is bit-stable between grabs");
        check(delta > 0.1, "enabling the preset changed the picture");

        // Back out of the category, then prove a planned model cannot select
        // anything: the disabled leaf is the one place where a click is
        // expected to do nothing, so it is asserted rather than assumed.
        //
        // Each row below is taken from the popover as it stands at that moment,
        // by object name, and only once that popover is really on screen: every
        // step rebuilds the rows of the popover it drives, so a row kept from
        // the step before is one the rebuild has already retired. The popovers
        // are children of the player's dialog, and the probe's later stages
        // share this loop - the PiP stage hides that dialog, and the shell runs
        // its state pass on the way back - so the window is waited for first,
        // and then every row, instead of sampling either once.
        playerWindowIsBack(dialog);
        enhancementsButton->click();
        anime4kEntry = popupRowWhenVisible(dialog, QStringLiteral("playerEnhancementsPopup"),
                                           QStringLiteral("Anime4K  ›"));
        if (anime4kEntry != nullptr) {
            anime4kEntry->click();
            QPushButton* reopen = popupRowWhenVisible(
                dialog, QStringLiteral("playerAnime4KPopup"), QStringLiteral("Швидкість  ›"));
            if (reopen != nullptr) {
                reopen->click();
                QPushButton* back = popupRowWhenVisible(
                    dialog, QStringLiteral("playerAnime4KPopup"), QStringLiteral("‹  Назад"));
                check(back != nullptr, "the drilled-down level still offers a way back");
                if (back != nullptr) {
                    back->click();
                    check(popupRowWhenVisible(dialog, QStringLiteral("playerAnime4KPopup"),
                                              QStringLiteral("Вимкнено")) != nullptr,
                          "going back returns to the Anime4K root");
                }
            }
        }
        shell->dismissOverlay();
        enhancementsButton->click();
        QPushButton* rifeRoot =
            popupRowWhenVisible(dialog, QStringLiteral("playerEnhancementsPopup"),
                                QStringLiteral("RIFE  ›"));
        check(rifeRoot != nullptr && rifeRoot->isEnabled(),
              "the planned RIFE row stays an enabled entry in the enhancements menu");
        const anime4k::Mode beforePlanned = surface->anime4KMode();
        if (rifeRoot != nullptr) {
            rifeRoot->click();
        }
        QPushButton* planned = popupRowWhenVisible(dialog, QStringLiteral("playerAnime4KPopup"),
                                                  QStringLiteral("В планах"));
        if (planned != nullptr) {
            planned->click();
            QTest::qWait(40);
        }
        check(planned != nullptr && !planned->isEnabled(),
              "the planned RIFE leaf stays disabled on its own page");
        check(surface->anime4KMode() == beforePlanned,
              "clicking a planned model leaves the chain alone");
        check(popupRowWhenVisible(dialog, QStringLiteral("playerAnime4KPopup"),
                                  QStringLiteral("‹  Назад")) != nullptr,
              "the planned page still offers a way back");
        shell->dismissOverlay();
    });

    // And the pickers still work, split by kind.
    QTimer::singleShot(9000, [&] {
        if (shell != nullptr) shell->openSettings();
        // Use a real press and release so this follows the same button path a
        // viewer uses.
        for (QPushButton* button : dialog->findChildren<QPushButton*>()) {
            if (!button->toolTip().startsWith(QStringLiteral("Озвучення"))) continue;
            const QPointF at(12, 12);
            const QPointF g = button->mapToGlobal(button->rect().center());
            QMouseEvent press(QEvent::MouseButtonPress, at, g, Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
            QApplication::sendEvent(button, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, at, g, Qt::LeftButton, Qt::NoButton,
                                Qt::NoModifier);
            QApplication::sendEvent(button, &release);
        }
        check(audio != nullptr && audio->isVisible(), "the dub picker opened");
        if (audio != nullptr) {
            check(!audio->isWindow() && audio->parentWidget() == dialog,
                  "the dub picker uses the fullscreen overlay plane");
            check(buttonsFitWithoutOverlap(audio), "the dub rows fit without overlap");
        }
        if (strip != nullptr) {
            std::fprintf(stderr, "     strip: visible=%d size=%dx%d at %d,%d\n",
                         int(strip->isVisible()), strip->width(), strip->height(), strip->x(),
                         strip->y());
        }
        if (audio != nullptr) check(audio->findChildren<QPushButton*>().size() > 0, "dub rows exist");
    });

    QTimer::singleShot(10500, [&] {
        for (QPushButton* button : dialog->findChildren<QPushButton*>()) {
            if (button->toolTip().startsWith(QStringLiteral("Субтитри"))) {
                button->click();
            }
        }
        check(subs != nullptr && subs->isVisible(), "the subtitle picker opened");
        if (subs != nullptr) {
            check(!subs->isWindow() && subs->parentWidget() == dialog,
                  "the subtitle picker uses the fullscreen overlay plane");
            check(buttonsFitWithoutOverlap(subs), "the subtitle rows fit without overlap");
        }
    });

    QTimer::singleShot(11000, [&] {
        if (shell == nullptr) return;
        shell->dismissOverlay();

        QPushButton* keysButton = nullptr;
        QPushButton* moreButton = nullptr;
        for (QPushButton* button : shell->findChildren<QPushButton*>()) {
            if (button->toolTip() == QStringLiteral("keyboard_shortcuts  (?)")) {
                keysButton = button;
            } else if (button->toolTip() == QStringLiteral("open_extended_menu")) {
                moreButton = button;
            }
        }

        check(keysButton != nullptr, "the keyboard hints button exists");
        if (keysButton != nullptr) keysButton->click();
        auto* hints = dialog->findChild<KeyHints*>(QStringLiteral("playerKeyHints"));
        check(hints != nullptr && hints->isVisible(), "the keyboard hints opened");
        if (hints != nullptr) {
            check(!hints->isWindow() && hints->parentWidget() == dialog,
                  "the keyboard hints use the fullscreen overlay plane");
        }

        check(moreButton != nullptr, "the extended menu button exists");
        if (moreButton != nullptr) moreButton->click();
        auto* more = dialog->findChild<MenuPopup*>(QStringLiteral("playerMorePopup"));
        check(more != nullptr && more->isVisible(), "the extended menu opened");
        if (more != nullptr) {
            check(!more->isWindow() && more->parentWidget() == dialog,
                  "the extended menu uses the fullscreen overlay plane");
            check(buttonsFitWithoutOverlap(more), "the extended menu rows fit without overlap");
        }
        shell->dismissOverlay();
    });

    // The window controls are part of the chrome, and the chrome auto-hides
    // after kAutohideMs of pointer idle. The old check asserted them visible
    // with no pointer activity for two and a half seconds, which asserted the
    // opposite of the design and went red for that reason alone. It now states
    // the actual contract, in both directions: up while the viewer is present,
    // gone once they are not - and it is a "look twice" check, because the
    // second half only exists after the event loop has had time to run.
    QTimer::singleShot(11500, [&] {
        if (shell != nullptr) shell->dismissOverlay();
        // Nudge the pointer the way a viewer arriving at the player would.
        if (surface != nullptr) {
            for (const QPointF& at : {QPointF(40, 40), QPointF(60, 60)}) {
                QMouseEvent move(QEvent::MouseMove, at, surface->mapToGlobal(at), Qt::NoButton,
                                 Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(surface, &move);
            }
        }
        QApplication::processEvents();

        QWidget* original = dialog->findChild<QWidget*>(QStringLiteral("originalFileButton"));
        check(original != nullptr, "the original-file button exists");
        if (original == nullptr) {
            return;
        }
        const QPoint at = original->mapTo(dialog, QPoint(0, 0));
        check(at.x() > dialog->width() / 2 && at.y() < dialog->height() / 2,
              "the original-file button is in the top-right corner");
        check(original->isVisible(), "the original-file button is visible while the chrome is up");

        // Now let the loop turn with the pointer idle, which is the half that
        // was never asserted anywhere: the chrome leaving on its own.
        QTest::qWait(2800);
        QApplication::processEvents();
        check(!original->isVisible(),
              "the window controls auto-hide once the pointer goes idle");
    });

    // PiP: open the float, check the spec'd layout, then prove the auto-hide
    // stands down while the pointer is on it.
    QTimer::singleShot(14600, [&] {
        for (QPushButton* b : dialog->findChildren<QPushButton*>()) {
            if (b->toolTip().contains(QStringLiteral("picture_in_picture"))) {
                b->click();
                break;
            }
        }
        QTimer::singleShot(900, [&] {
            QWidget* pip = nullptr;
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (QString::fromLatin1(w->metaObject()->className()).contains(QStringLiteral("PipFloat"))) {
                    pip = w;
                    break;
                }
            }
            check(pip != nullptr, "the PiP float opened");
            if (pip == nullptr) {
                return;
            }
            pipFloat = qobject_cast<PipFloat*>(pip);
            auto child = [pip](const char* name) -> QWidget* {
                return pip->findChild<QWidget*>(QString::fromLatin1(name));
            };
            QWidget* mute = child("pipMute");
            QWidget* restore = child("pipRestore");
            QWidget* close = child("pipClose");
            QWidget* play = child("pipPlay");
            check(mute != nullptr && mute->isVisible(), "PiP mute is visible");
            check(restore != nullptr && restore->isVisible(), "PiP restore is visible");
            check(close != nullptr && close->isVisible(), "PiP close is visible");
            check(play != nullptr && play->isVisible(), "PiP play button is visible");
            // One fixed pill, always the same size. It used to grow on hover,
            // which resized the float's content under the pointer on every
            // enter and leave - the lag. A constant width is the fix.
            QWidget* volume = child("pipVolume");
            check(volume != nullptr && volume->isVisible(),
                  "the float shows a volume level");
            check(mute != nullptr && volume != nullptr && mute->parentWidget() != nullptr
                      && volume->parentWidget() == mute->parentWidget(),
                  "the level and the mute button share one chip");
            if (volume != nullptr && mute != nullptr) {
                const int pillWidth = volume->parentWidget()->width();
                // The pill must be as wide as its two children plus the gap,
                // and no wider: a strip stretched to the window is what made
                // the control look lost in the corner.
                check(pillWidth >= mute->width() + volume->width(),
                      "the chip is wide enough for the button and the level");
                // Containment, in one coordinate space: the button must lie
                // inside the pill's rect. Comparing two mapTo results across
                // different parents is how the earlier version of this check
                // reported a failure that was never there.
                const QWidget* pill = volume->parentWidget();
                const QRect pillRect = pill->geometry();
                const QRect buttonRect =
                    QRect(mute->mapTo(pill, QPoint(0, 0)), mute->size());
                std::fprintf(stderr,
                             "     pill=%d,%d %dx%d  button=%d,%d %dx%d\n",
                             pillRect.x(), pillRect.y(), pillRect.width(), pillRect.height(),
                             buttonRect.x(), buttonRect.y(), buttonRect.width(),
                             buttonRect.height());
                // buttonRect is already relative to the pill, so containment is a
                // plain size comparison - not a position one.
                check(buttonRect.left() >= 0 && buttonRect.top() >= 0 &&
                          buttonRect.right() < pillRect.width() &&
                          buttonRect.bottom() < pillRect.height(),
                      "the mute button sits inside the pill, not beside it");
                check(volume->toolTip().isEmpty(),
                      "the level has no tooltip to chase the pointer");
            }
            if (mute != nullptr && restore != nullptr) {
                const QPoint m = mute->mapTo(pip, QPoint(0, 0));
                const QPoint r = restore->mapTo(pip, QPoint(0, 0));
                check(m.x() < r.x(), "PiP mute sits left of restore");
                check(m.y() < 60, "PiP mute sits near the top");
            }
            if (play != nullptr) {
                const QPoint c = play->mapTo(pip, QPoint(0, 0));
                check(std::abs(c.x() + play->width() / 2 - pip->width() / 2) < 4,
                      "PiP play button is horizontally centred");
                check(std::abs(c.y() + play->height() / 2 - pip->height() / 2) < 4,
                      "PiP play button is vertically centred");
                check(play->width() >= 80, "PiP play button is large");
            }
            // 1.6 s is the float's own hold, so waiting 2.4 s proves a hovered
            // float keeps its controls past the point an idle one drops them.
            QEvent enter(QEvent::Enter);
            QCoreApplication::sendEvent(pip, &enter);
            QTest::qWait(2400);
            QWidget* overlay = child("pipOverlay");
            QGraphicsOpacityEffect* effect =
                overlay != nullptr
                    ? qobject_cast<QGraphicsOpacityEffect*>(overlay->graphicsEffect())
                    : nullptr;
            const qreal hovered = effect != nullptr ? effect->opacity() : 0.0;
            check(hovered > 0.9, "PiP controls stay up while the pointer is on the float");

            QEvent leave(QEvent::Leave);
            QCoreApplication::sendEvent(pip, &leave);
            QTest::qWait(3000);
            const qreal idle = effect != nullptr ? effect->opacity() : 0.0;
            check(idle < 0.1, "PiP controls auto-hide once the pointer leaves");
        });
    });

    // THE REGRESSION CLASS. Every other check in this file asserts a view
    // right after it was built, which is why "it renders, then a beat later it
    // is empty" survived: nothing here ever looked twice. A populated view is
    // opened, the event loop is allowed to turn for 1.5s, and the same view is
    // asserted again. Any timer, resize pass, state handler or async callback
    // that empties a populated widget shows up here and nowhere else.
    QTimer::singleShot(28000, [&] {
        // The PiP stage closed the float and the dialog reports itself visible
        // again at that point, but it is not reliably still visible 100ms later,
        // so no assertion is made about it here: the checks below are about the
        // controls, and they make their own state explicit. Only restore it if
        // it really is down, so a hidden window cannot be mistaken for a broken
        // control.
        if (!dialog->isVisible()) {
            dialog->show();
            QApplication::processEvents();
        }
        for (QWidget* w : dialog->findChildren<QWidget*>()) {
            if (QString::fromLatin1(w->metaObject()->className())
                    .contains(QStringLiteral("VideoSurface"))) {
                for (const QPointF& at : {QPointF(40, 40), QPointF(60, 60)}) {
                    QMouseEvent move(QEvent::MouseMove, at, w->mapToGlobal(at), Qt::NoButton,
                                     Qt::NoButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(w, &move);
                }
            }
        }
        QApplication::processEvents();
        QPushButton* series = dialog->findChild<QPushButton*>(QStringLiteral("playerSeriesToggle"));
        // isHidden() is this control's own state. isVisible() would also be
        // false whenever the strip's idle timer has hidden the chrome, which
        // says nothing about whether the toggle is there or enabled.
        check(series != nullptr && !series->isHidden() && series->isEnabled(),
              "the series toggle is on the strip with a playlist loaded");

        auto* drawer = dialog->findChild<QWidget*>(QStringLiteral("playerSeriesDrawer"));
        // Click only to OPEN. The stage runs after earlier stages that may have
        // left the drawer expanded, and clicking an expanded drawer collapses
        // it - so a blind click tested the toggle, not the drawer.
        if (drawer != nullptr && !drawer->isVisible() && series != nullptr) {
            series->click();
            QApplication::processEvents();
        }

        check(drawer != nullptr && drawer->isVisible(), "the series drawer opened");
        const int before =
            drawer != nullptr ? drawer->findChildren<QPushButton*>(
                                   QStringLiteral("playerSeriesItemCurrent")).size() +
                                   drawer->findChildren<QPushButton*>(QStringLiteral("playerSeriesItem"))
                                       .size()
                             : 0;
        check(before == 5, "the drawer lists every episode of the playlist");

        // Let the loop turn. Shell state, hide timers, layout passes and any
        // queued callback all get their chance to run against a populated view.
        QTest::qWait(1500);
        QApplication::processEvents();

        const int after =
            drawer != nullptr ? drawer->findChildren<QPushButton*>(
                                   QStringLiteral("playerSeriesItemCurrent")).size() +
                                   drawer->findChildren<QPushButton*>(QStringLiteral("playerSeriesItem"))
                                       .size()
                             : 0;
        check(drawer != nullptr && drawer->isVisible(),
              "the series drawer is still open 1.5s after it opened");
        check(after == before && after == 5,
              "the series drawer still lists every episode 1.5s later");
        check(series != nullptr && series->isVisible(),
              "the series toggle survived the drawer's own refresh");
    });

    // PiP owns the picture while it is up. Three things were wrong and none of
    // them showed as a crash - they showed as the float's controls doing
    // nothing, so each is asserted on the state it changes.
    QTimer::singleShot(21100, [&] {
        check(pipFloat != nullptr, "the PiP float is still up for the ownership checks");
    });

    // Placement and snapping, which is what a viewer reaches for when they
    // want the float on the other monitor. The pool keeps the last position,
    // so a float can otherwise reappear somewhere unusable.
    QTimer::singleShot(21000, [&] {
        if (pipFloat == nullptr || !pipFloat->isVisible()) {
            check(false, "the float is up for the placement checks");
            return;
        }
        QScreen* under = QGuiApplication::screenAt(pipFloat->frameGeometry().center());
        check(under != nullptr, "the float sits on a screen it can be clicked on");
        if (under != nullptr) {
            const QRect area = under->availableGeometry();
            const QRect frame = pipFloat->frameGeometry();
            check(frame.top() >= area.top() && frame.left() >= area.left() - pipFloat->width()
                      && frame.bottom() <= area.bottom() + pipFloat->height(),
                  "the float is inside the work area, not under the taskbar");
        }

        // Drag it into a corner and let go: it should land on the nearest one.
        const QPoint before = pipFloat->pos();
        QWidget* overlay =
            pipFloat->findChild<QWidget*>(QString::fromLatin1("pipOverlay"));
        check(overlay != nullptr, "the float has an overlay to press on");
        if (overlay == nullptr) {
            return;
        }
        // The float drags on the delta between two GLOBAL points, so the move
        // has to carry a real one: a 40px wobble inside the same window is
        // under the threshold and never starts a drag.
        const QPointF grabAt = pipFloat->mapToGlobal(QPointF(20, 20));
        const QPointF dropAt = grabAt + QPointF(-140, -120);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(20, 20), grabAt, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(overlay, &press);
        QMouseEvent move(QEvent::MouseMove, QPointF(20 - 140, 20 - 120), dropAt,
                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(overlay, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(20 - 140, 20 - 120), dropAt,
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(overlay, &release);
        QTest::qWait(500);
        std::fprintf(stderr, "     float moved %s\n",
                     qPrintable(QStringLiteral("%1,%2 -> %3,%4")
                                    .arg(before.x()).arg(before.y())
                                    .arg(pipFloat->x()).arg(pipFloat->y())));
        if (QScreen* landed = QGuiApplication::screenAt(pipFloat->frameGeometry().center())) {
            const QRect area = landed->availableGeometry();
            const bool cornered =
                std::abs(pipFloat->x() - (area.left() + 24)) < 2 ||
                std::abs(pipFloat->x() - (area.right() - pipFloat->width() - 24 + 1)) < 2;
            check(pipFloat->pos() != before, "a drag actually moved the float");
            check(cornered, "a dragged float lands on a corner");
        }
    });
    QTimer::singleShot(21200, [&] {
        if (pipFloat == nullptr) {
            return;
        }
        auto* pipSurface = pipFloat->videoSurface();
        // NOT findChild: the float owns a VideoSurface too, and which one
        // comes first depends on construction order - comparing a surface
        // against itself passes every check here.
        auto* mainSurface = shell->videoSurface();
        if (mainSurface == nullptr || pipSurface == nullptr
            || mainSurface == pipSurface) {
            check(false, "the two surfaces are distinct and both reachable");
            return;
        }
        // The float loads its own core when the window is first exposed, a beat
        // after it opens. Everything below needs a live stream.
        for (int i = 0; i < 100 && !pipSurface->hasVideo(); ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }
        if (!pipSurface->hasVideo()) {
            // Same as above: no stream in a headless session is an environment,
            // not a defect, and the checks below all need one.
            std::fprintf(stderr, "SKIP  the float ownership checks: no stream in this session\n");
            return;
        }
        check(true, "the float's own core loaded the stream");
        // The main core has to be stopped: the float has its own mpv handle,
        // so a still-playing main core decodes a second copy of the same
        // stream behind a hidden window.
        check(!mainSurface->isPlaying(), "the hidden main player is not decoding while PiP is up");
        check(mainSurface->hasVideo(), "the main player keeps its stream loaded for the return");
        if (!pipSurface->isPlaying()) {
            pipSurface->setPaused(false);
        }
        QTest::qWait(1000);
        check(pipSurface->isPlaying(), "the float is the one playing");

        // Each core is compared against ITSELF, not against the other: the
        // float has been playing while the main one is parked, so only a
        // before/after on the same core says anything.
        const double floatBefore = pipSurface->position();
        const double mainBefore = mainSurface->position();
        // The float's scrubber used to seek the main surface, so dragging it
        // moved a video nobody could see.
        pipSurface->seek(floatBefore + 5.0);
        QTest::qWait(400);
        check(pipSurface->position() > floatBefore + 1.0, "seeking the float moves the float");
        check(std::abs(mainSurface->position() - mainBefore) < 0.5,
              "seeking the float leaves the hidden main player alone");

        // The counter follows the float's core; it froze on the moment PiP
        // opened when it was fed from the parked one.
        // The counter is driven from the float's own positionChanged, so the
        // label must track that core. A seek is the trigger: the elapsed label
        // is only rewritten when a position arrives, and with the chain
        // enabled mpv re-emits, so this is the cheapest moment to prove the
        // label is live rather than the placeholder it used to freeze on.
        auto* elapsed = pipFloat->findChild<QLabel*>(QString::fromLatin1("pipElapsed"));
        check(elapsed != nullptr, "the float has a position label");
        if (elapsed != nullptr) {
            const QString before = elapsed->text();
            pipSurface->seek(pipSurface->position() + 30.0);
            QTest::qWait(900);
            std::fprintf(stderr, "     label: \"%s\" -> \"%s\"\n", qPrintable(before),
                         qPrintable(elapsed->text()));
            check(elapsed->text() != before, "the float's clock follows its own core");
        }
    });

    // Leaving PiP brings the main player back to where the viewer actually
    // watched, which is the float's position and not where the main core was
    // parked on entry.
    QTimer::singleShot(24000, &app, [&] {
        if (pipFloat == nullptr) {
            check(false, "the float was still up for the return check");
            return;
        }
        auto* floatSurface = pipFloat->videoSurface();
        if (!floatSurface->isPlaying()) {
            floatSurface->setPaused(false);
        }
        QTest::qWait(700);
        floatExitPosition = floatSurface->position();
        auto* restore = pipFloat->findChild<QPushButton*>(QString::fromLatin1("pipRestore"));
        check(restore != nullptr, "the float has a restore button");
        if (restore == nullptr) {
            return;
        }
        restore->click();
        // The hand-back is deferred by a tick on purpose, so give it room
        // rather than sampling mid-transition.
        QTimer::singleShot(3000, &app, [&] {
            // The float is pooled rather than destroyed - each owns a
            // QOpenGLWidget and tearing one down beside the player's crashes
            // the display driver - so "closed" means hidden, not gone.
            check(pipFloat != nullptr && !pipFloat->isVisible(),
                  "the restore button put the float away");
            check(dialog->isVisible(), "the player window is back");
            auto* mainSurface = dialog->findChild<VideoSurface*>();
            if (mainSurface == nullptr) {
                return;
            }
            std::fprintf(stderr, "     returned to %.2fs, the float left it at %.2fs\n",
                         mainSurface->position(), floatExitPosition);
            check(std::abs(mainSurface->position() - floatExitPosition) < 1.5,
                  "the main player resumes where the float left off");
            // The main core inherits whatever it was doing before PiP, and
            // the probe enters PiP from a paused player, so paused is the
            // correct state to return to - not a failure to resume.
            check(!mainSurface->isPlaying(),
                  "the main core is back in the state PiP found it in");
        });
    });

    // The float's X and its expand button mean opposite things, and they were
    // wired to the same slot: X put the full window back instead of closing.
    // A second player, because the first one is closed by the Esc ladder below
    // and the dialog is destroyed with it.
    QTimer::singleShot(300, &app, [&] {
        StubHost secondHost;
        auto* second = new PlayerDialog(QStringLiteral("probe pip close"), url, qualities, 720,
                                        &secondHost);
        QVector<QVariantMap> entries;
        for (int episode = 1; episode <= 3; ++episode) {
            entries.append(QVariantMap{{"label", QStringLiteral("Серія %1").arg(episode)},
                                       {"embed", QStringLiteral("e%1").arg(episode)}});
        }
        second->setPlaylist(entries, 0);
        second->setVariants({QVariantMap{{"label", QStringLiteral("Ashdi 1080p")},
                                      {"embed", QStringLiteral("e1")},
                                      {"kind", QStringLiteral("voice")}}},
                            QStringLiteral("e1"));
        second->show();
        auto* secondShell = second->findChild<PlayerShell*>();
        check(secondShell != nullptr, "the second player has a shell");
        if (secondShell == nullptr) {
            return;
        }
        // The float is parentless so it can float above other apps, which means
        // it is not in the dialog's child tree: top level widgets, exactly as
        // the PiP section above finds it.
        const auto openFloat = [] {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (QString::fromLatin1(w->metaObject()->className())
                        .contains(QStringLiteral("PipFloat"))) {
                    return qobject_cast<PipFloat*>(w);
                }
            }
            return static_cast<PipFloat*>(nullptr);
        };
        const auto dropFloat = [&openFloat] {
            if (PipFloat* w = openFloat()) {
                w->hide();
                w->deleteLater();
                QCoreApplication::processEvents();
            }
        };
        dropFloat();
        emit secondShell->pipRequested();
        QTest::qWait(2500);
        PipFloat* floatWindow = openFloat();
        check(floatWindow != nullptr, "the second player opened a float");
        auto* close = floatWindow != nullptr
                          ? floatWindow->findChild<QPushButton*>(QStringLiteral("pipClose"))
                          : nullptr;
        check(close != nullptr, "the float has a close button");
        check(!secondHost.closed, "the player is still open while the float is up");
        if (close != nullptr) {
            close->click();
            QTest::qWait(2500);
            check(secondHost.closed, "the float's X closed the player");
            // No isVisible() here: the dialog is WA_DeleteOnClose, so the host
            // being told is the only safe evidence. Reading the window after
            // playerClosed() is the use-after-free this probe already guards
            // the first dialog against.
        } else {
            // The host is told on close, and the dialog is destroyed with it, so
            // a check that never runs would leave a second player on screen for
            // the rest of the run and skew every check after it.
            second->close();
        }
        dropFloat();
    });

    // Esc unwinds one layer per press - picker, then the shell's own popover,
    // then the series drawer, then the player - so what matters is that the
    // ladder terminates, not that one press out of a busy UI closes it.
    int escPresses = 0;
    std::function<void()> pressEsc = [&] {
        QTimer::singleShot(300, &app, [&] {
            ++escPresses;
            QTest::keyClick(dialog, Qt::Key_Escape);
            if (host.closed || escPresses > 12) {
                check(host.closed, "Esc closed the player once nothing was open");
                std::fprintf(stderr, failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL (%d)\n",
                             failures);
                app.exit(failures == 0 ? 0 : 1);
                return;
            }
            pressEsc();
        });
    };
    QTimer::singleShot(30000, &app, pressEsc);


    return app.exec();
}
