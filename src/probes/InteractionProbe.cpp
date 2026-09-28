// Interaction probe: the dock's dub/subtles buttons, the gear's quality list,
// and the things that only break in use.
//
//   - a seek crashed the process: mpv_command wants a NULL-terminated array
//   - the quality and track popovers closed on every shell state change
//   - returning from Alt+Tab left nothing focused
//   - quality and dub/sub selection sat in a top strip, above the picture

#include <QApplication>
#include <windows.h>
#include <QGuiApplication>
#include <QDebug>
#include <QDir>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

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
    void playerClosed() override {}
    void playbackFailed() override {}
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

    QTimer::singleShot(11500, [&] {
        std::fprintf(stderr, failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL (%d)\n", failures);
        app.exit(failures == 0 ? 0 : 1);
    });

    return app.exec();
}
