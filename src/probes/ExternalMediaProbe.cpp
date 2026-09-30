#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <cstdio>
#include <functional>

#include <backend/MikaiClient.hpp>
#include "CrashLog.hpp"
#include "pages/MainWindow.hpp"
#include "pages/SearchPage.hpp"
#include "theme/Theme.hpp"
#include "PlayerDialog.hpp"
#include "PlayerShell.hpp"
#include "CompareWindow.hpp"
#include "ShellWidgets.hpp"
#include "VideoSurface.hpp"

using namespace mplayer::shell;

namespace {
int failures = 0;
void check(bool condition, const char* description) {
    std::fprintf(stderr, "%s %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) ++failures;
}

bool until(const std::function<bool()>& predicate, int timeout = 10000) {
    for (int elapsed = 0; elapsed < timeout; elapsed += 50) {
        if (predicate()) return true;
        QTest::qWait(50);
    }
    return predicate();
}

QPushButton* buttonWithText(QWidget* parent, const QString& text) {
    for (auto* button : parent->findChildren<QPushButton*>()) {
        if (button->text() == text) return button;
    }
    return nullptr;
}

QPushButton* buttonWithTip(QWidget* parent, const QString& text) {
    for (auto* button : parent->findChildren<QPushButton*>()) {
        if (button->toolTip() == text) return button;
    }
    return nullptr;
}

PlayerDialog* activePlayer(QWidget* window) {
    for (auto* player : window->findChildren<PlayerDialog*>()) {
        if (player->isVisible()) return player;
    }
    return nullptr;
}

void answerInput(const QString& input, bool accept) {
    QTimer::singleShot(100, [input, accept] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QInputDialog*>(widget); dialog && dialog->isVisible()) {
                dialog->setTextValue(input);
                if (accept) dialog->accept();
                else dialog->reject();
                return;
            }
        }
    });
}

bool videoOnly(PlayerDialog* player) {
    if (!player) return false;
    auto* shell = player->findChild<PlayerShell*>();
    if (!shell) return false;
    shell->wake();
    player->toggleSeries();
    auto* series = player->findChild<QPushButton*>(QStringLiteral("playerSeriesToggle"));
    bool clean = series && !series->isVisible();
    for (auto* button : shell->findChildren<QPushButton*>()) {
        const QString hint = button->toolTip();
        if (hint.startsWith(QStringLiteral("Озвучення —")) ||
            hint.startsWith(QStringLiteral("Субтитри —")) ||
            hint.startsWith(QStringLiteral("load_previous_media")) ||
            hint.startsWith(QStringLiteral("load_next_media"))) {
            clean = clean && !button->isVisible();
        }
    }
    shell->openSettings();
    QTest::qWait(150);
    auto* settings = player->findChild<SettingsFlyout*>();
    if (!settings || !settings->isVisible()) return false;
    for (auto* label : settings->findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("ЯКІСТЬ")) clean = clean && !label->isVisible();
    }
    shell->dismissOverlay();
    return clean;
}

bool playback(PlayerDialog* player) {
    if (!player) return false;
    auto* surface = player->findChild<mplayer::VideoSurface*>();
    if (!surface || !until([surface] { return surface->hasVideo() && surface->duration() > 10.0; })) return false;
    surface->setPaused(true);
    QTest::qWait(300);
    const QImage image = surface->grabFramebuffer();
    if (image.isNull() || surface->videoWidth() != 320 || surface->videoHeight() != 180) return false;
    int colorful = 0;
    for (int y = image.height() / 4; y < image.height() * 3 / 4; y += 8) {
        for (int x = image.width() / 4; x < image.width() * 3 / 4; x += 8) {
            const QColor pixel = image.pixelColor(x, y);
            if (qMax(pixel.red(), qMax(pixel.green(), pixel.blue())) -
                qMin(pixel.red(), qMin(pixel.green(), pixel.blue())) > 30) ++colorful;
        }
    }
    return colorful > 100;
}
} // namespace

int main(int argc, char** argv) {
    mikai::app::installCrashLog();
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (argc != 3) {
        std::fprintf(stderr, "usage: external_media_probe <320x180 video file> <HTTP video URL>\n"
                             "HTTP fixture: /retry.mp4 returns 404 once, then the same video.\n");
        return 2;
    }
    const QString file = QString::fromLocal8Bit(argv[1]);
    const QString remote = QString::fromLocal8Bit(argv[2]);
    QTemporaryDir settingsDir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    app.setOrganizationName(QStringLiteral("MikaiProbe"));
    app.setApplicationName(QStringLiteral("ExternalMedia"));
    QSettings settings;
    settings.setValue(QStringLiteral("home/feeds"), QStringLiteral(""));
    settings.setValue(QStringLiteral("player/anime4k"), QStringLiteral("off"));
    app.setStyleSheet(mikai::app::theme::buildStylesheet());
    mikai::backend::MikaiClient client;
    mikai::app::MainWindow main(client);
    main.show();
    QTest::qWait(300);
    main.resize(860, 620);
    QTest::qWait(150);
    main.grab().save(QStringLiteral("external-media-home.png"));
    auto* menu = main.findChild<QMenu*>(QStringLiteral("openMediaMenu"));
    auto* openFile = main.findChild<QAction*>(QStringLiteral("openVideoFile"));
    auto* openUrl = main.findChild<QAction*>(QStringLiteral("openVideoUrl"));
    check(menu && openFile && openUrl, "home offers local file and media URL");
    if (!openFile || !openUrl) return 1;

    answerInput({}, false);
    openUrl->trigger();
    check(activePlayer(&main) == nullptr, "canceling URL input keeps home without a player");
    answerInput(QStringLiteral("   "), true);
    openUrl->trigger();
    check(activePlayer(&main) == nullptr, "blank URL input does not open a player");

    QTimer::singleShot(100, [file] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QFileDialog*>(widget); dialog && dialog->isVisible()) {
                dialog->selectFile(file);
                QMetaObject::invokeMethod(dialog, "accept");
                return;
            }
        }
    });
    openFile->trigger();
    QPointer<PlayerDialog> local = activePlayer(&main);
    check(playback(local), "file picker opens real 320x180 video and renders colored pixels");
    check(videoOnly(local), "local video has no anime quality, episode, or release controls");
    if (!local) return 1;
    auto* shell = local->findChild<PlayerShell*>();
    emit shell->copyLinkRequested();
    check(app.clipboard()->text() == file, "standalone copy uses the currently playing file path");
    shell->wake();
    local->grab().save(QStringLiteral("external-media-player.png"));
    auto* enhance = buttonWithTip(local, QStringLiteral("open_enhancements"));
    if (enhance) enhance->click();
    QTest::qWait(150);
    auto* anime = buttonWithText(local, QStringLiteral("Anime4K  ›"));
    check(anime && anime->isVisible(), "standalone media retains Anime4K enhancement menu");
    shell->dismissOverlay();
    QTest::mouseClick(shell->videoSurface(), Qt::RightButton, Qt::NoModifier, QPoint(120, 120));
    QTest::qWait(100);
    auto* compareAction = buttonWithText(local, QStringLiteral("Порівняти якість"));
    if (compareAction) compareAction->click();
    QTest::qWait(200);
    auto* compare = local->findChild<CompareWindow*>();
    check(compare && compare->isVisible() && compare->captureError().isEmpty() &&
          !compare->frozenFrame().isNull(), "standalone Compare captures the actual video frame");
    if (compare) compare->close();

    answerInput(remote, true);
    openUrl->trigger();
    QPointer<PlayerDialog> streamed = activePlayer(&main);
    check(until([&local] { return local.isNull(); }), "opening URL disposes the previous local player");
    check(playback(streamed), "URL input opens real HTTP video and renders colored pixels");
    check(videoOnly(streamed), "HTTP playback has no anime quality, episode, or release controls");
    if (!streamed) return 1;
    auto* remoteShell = streamed->findChild<PlayerShell*>();
    emit remoteShell->copyLinkRequested();
    check(app.clipboard()->text() == remote, "standalone copy uses the actual HTTP media URL");
    streamed->close();
    check(until([&streamed] { return streamed.isNull(); }), "closing standalone player destroys it safely");

    QUrl retryUrl(remote);
    retryUrl.setPath(QStringLiteral("/retry.mp4"));
    answerInput(retryUrl.toString(), true);
    openUrl->trigger();
    QPointer<PlayerDialog> failed = activePlayer(&main);
    check(failed, "failed media URL keeps the player window open");
    if (!failed) return 1;
    auto* retry = failed->findChild<QPushButton*>(QStringLiteral("playerRetryButton"));
    check(retry && until([retry] { return retry->isVisible(); }),
          "failed media URL reports a playback error without an anime host");
    if (retry && retry->isVisible()) retry->click();
    check(playback(failed), "retry reopens the same URL and recovers actual video playback");
    check(videoOnly(failed), "retry does not introduce anime quality or episode controls");

    answerInput(QUrl::fromLocalFile(file).toString(), true);
    openUrl->trigger();
    QPointer<PlayerDialog> reopened = activePlayer(&main);
    check(playback(reopened), "file URL opens after close and is normalized for mpv");
    main.close();
    check(until([&reopened] { return reopened.isNull(); }), "closing home closes its standalone video player");
    std::fprintf(stderr, "RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
