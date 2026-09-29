// Interaction probe: the dock's dub/subtles buttons, the gear's quality list,
// and the things that only break in use.
//
//   - a seek crashed the process: mpv_command wants a NULL-terminated array
//   - the quality and track popovers closed on every shell state change
//   - returning from Alt+Tab left nothing focused
//   - quality and dub/sub selection sat in a top strip, above the picture

#include <QApplication>
#include <QGraphicsOpacityEffect>
#include <QTest>
#include <windows.h>
#include <QGuiApplication>
#include <QDebug>
#include <QDir>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include <cmath>
#include <functional>

#include "PlayerDialog.hpp"
#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
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

int failures = 0;

void check(bool ok, const char* what) {
    std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
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
        for (const QPushButton* button : dialog->findChildren<QPushButton*>()) {
            if (button->toolTip().startsWith(QStringLiteral("Озвучення"))) {
                check(button->isVisible(), "the dub button is on the dock");
            }
            if (button->toolTip().startsWith(QStringLiteral("Субтитри"))) {
                check(button->isVisible(), "the subtitle button is on the dock");
            }
        }
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
        int rows = 0;
        for (QPushButton* button : flyout->findChildren<QPushButton*>()) {
            if (button->text().endsWith("p")) ++rows;
        }
        check(rows == 3, "the popover lists all three qualities");
        // a Qt::Popup is its own window, so the dialog grab cannot show it
        flyout->grab().toImage().save(QStringLiteral("flyout_probe.png"));
        auto* series = dialog->findChild<QPushButton*>(QStringLiteral("playerSeriesToggle"));
        check(series != nullptr && series->isVisible(),
              "the series toggle is still on the strip");
        // a full-screen grab, because a widget grab cannot show z-order: a
        // popover that Qt believes is visible but that renders under the video
        // looks identical in every widget-level check
        if (auto* screen = QGuiApplication::primaryScreen()) {
            screen->grabWindow(0).save(QStringLiteral("screen_flyout.png"));
        }
    });

    // And the pickers still work, split by kind.
    QTimer::singleShot(9000, [&] {
        if (shell != nullptr) shell->openSettings();
        // A real press and release, not click(): a Qt::Popup shown during a
        // button's release can be closed by the tail of that same release, and
        // only a real event sequence reproduces that.
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
            std::fprintf(stderr,
                         "     audio popup: visible=%d size=%dx%d at %s winId=%p type=%d\n",
                         int(audio->isVisible()), audio->width(), audio->height(),
                         qPrintable(QString("%1,%2").arg(audio->mapToGlobal(QPoint(0, 0)).x())
                                        .arg(audio->mapToGlobal(QPoint(0, 0)).y())),
                         (void*)audio->winId(), int(audio->windowType()));
        }
        if (auto* screen = QGuiApplication::primaryScreen()) {
            const QImage shot = screen->grabWindow(0).toImage();
            const QRect geo = audio->geometry();
            const QPoint centre = geo.center();
            const QColor onScreen = shot.pixelColor(centre);
            const QColor ownGrab = audio->grab().toImage().pixelColor(audio->width() / 2,
                                                                    audio->height() / 2);
            {
            // A desktop BitBlt skips layered windows, so the screen pixel cannot
            // say anything about a translucent popover. Ask Win32 instead:
            // GW_HWNDPREV is the window stacked above this one.
            HWND popupHwnd = reinterpret_cast<HWND>(audio->winId());
            HWND above = GetWindow(popupHwnd, GW_HWNDPREV);
            HWND dialogHwnd = reinterpret_cast<HWND>(dialog->winId());
            std::fprintf(stderr, "     z-order: window above popover = %p (dialog=%p, shellTop=%p)\n",
                         (void*)above, (void*)dialogHwnd,
                         (void*)reinterpret_cast<HWND>(dialog->winId()));
            std::fprintf(stderr, "     -> %s\n",
                         above == dialogHwnd ? "DIALOG IS ABOVE THE POPOVER (occluded)"
                                             : "popover is on top");
        }
        std::fprintf(stderr, "     z-order: screen@%d,%d=%s  popover own=%s  %s\n", centre.x(),
                         centre.y(), qPrintable(onScreen.name()),
                         qPrintable(ownGrab.name()),
                         onScreen == ownGrab ? "POPOVER IS ON TOP" : "POPOVER IS OCCLUDED");
            shot.copy(0, 0, 1400, 1000).save(QStringLiteral("screen_corner.png"));
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
    });

    // The window controls are part of the chrome, and the chrome auto-hides
    // after kAutohideMs of pointer idle. The old check asserted them visible
    // with no pointer activity for two and a half seconds, which asserted the
    // opposite of the design and went red for that reason alone. It now states
    // the actual contract, in both directions: up while the viewer is present,
    // gone once they are not - and it is a "look twice" check, because the
    // second half only exists after the event loop has had time to run.
    QTimer::singleShot(11500, [&] {
        // A Qt::Popup holds the shell awake - PlayerShell::enterAutoHidden
        // re-arms the countdown and returns while any popup is open - so the
        // idle half below is unobservable until every one of them is gone.
        // Naming the two the earlier checks opened is not enough: the gear
        // flyout is a Qt::Popup too, and it is the one that would silently
        // disarm this assertion and make it red forever.
        for (QWidget* popup : QApplication::topLevelWidgets()) {
            if (popup->windowType() == Qt::Popup) {
                popup->close();
            }
        }
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
        QTest::qWait(3000);
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
            check(child("pipVolume") == nullptr, "PiP has no volume slider");
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

            // Close the float through its own path, then confirm the dialog is
            // back. Esc is the user's route, but a stage boundary should not
            // depend on a synthetic keypress landing.
            pip->close();
            QApplication::processEvents();
            std::fprintf(stderr, "     PiP closed=%d dialogVisible=%d\n",
                         pip->isVisible() ? 1 : 0, dialog->isVisible() ? 1 : 0);
        });
    });

    // THE REGRESSION CLASS. Every other check in this file asserts a view
    // right after it was built, which is why "it renders, then a beat later it
    // is empty" survived: nothing here ever looked twice. A populated view is
    // opened, the event loop is allowed to turn for 1.5s, and the same view is
    // asserted again. Any timer, resize pass, state handler or async callback
    // that empties a populated widget shows up here and nowhere else.
    QTimer::singleShot(21000, [&] {
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
    QTimer::singleShot(23500, &app, pressEsc);

    return app.exec();
}
