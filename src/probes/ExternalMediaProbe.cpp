#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStringList>
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
#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
#include "CompareWindow.hpp"
#include "ShellTheme.hpp"
#include "ShellWidgets.hpp"
#include "VideoSurface.hpp"
#include "ProbeRuntime.hpp"

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

/// Whether any on-screen button under `root` is painted with `glyph`. A glyph is
/// identity; a label or tooltip is display text that changes with the locale.
/// Absent and hidden both answer "no".
bool anyVisibleGlyphButton(QWidget* root, IconFn glyph) {
    for (auto* button : root->findChildren<QPushButton*>()) {
        auto* icon = qobject_cast<IconButton*>(button);
        if (icon != nullptr && icon->icon() == glyph && icon->isVisible()) return true;
    }
    return false;
}

QPushButton* buttonWithGlyph(QWidget* root, IconFn glyph) {
    for (auto* button : root->findChildren<QPushButton*>()) {
        auto* icon = qobject_cast<IconButton*>(button);
        if (icon != nullptr && icon->icon() == glyph) return button;
    }
    return nullptr;
}

/// Whether the flyout offers a quality row. Quality rows sit in their own nested
/// container while the speed presets are direct children, so a visible row that
/// is NOT a direct child is a quality row - which reads through a rename or a
/// translation, and is absent in the hostless case.
bool flyoutOffersQuality(const SettingsFlyout* flyout) {
    for (const auto* button : flyout->findChildren<QPushButton*>()) {
        if (button->parentWidget() != flyout && button->isVisible()) return true;
    }
    return false;
}

/// Presses the row carrying `action` as its payload. Returns false when no
/// visible menu offers it: the payload is the stable id ("compare", "copy"), so
/// a reworded label still passes and a missing action fails.
bool pressMenuRow(QWidget* root, const QString& action) {
    for (auto* panel : root->findChildren<MenuPopup*>()) {
        if (!panel->isVisible()) continue;
        for (int spot = 0; spot < panel->rowCount(); ++spot) {
            if (panel->entryAt(spot).payload.toString() != action) continue;
            if (auto* button = qobject_cast<QPushButton*>(panel->rowWidget(spot))) {
                button->click();
                return true;
            }
        }
    }
    return false;
}

/// Whether a visible enhancement popover offers at least one catalogue row.
/// The rows are addressed by the payload the shell hangs on them ("level|<id>"
/// for a category, "preset|<id>" for a preset) rather than by the words on
/// them, so the catalogue reaching the menu is what is being asserted.
bool enhancementCatalogueVisible(QWidget* root) {
    for (auto* panel : root->findChildren<MenuPopup*>()) {
        if (!panel->isVisible()) continue;
        for (int spot = 0; spot < panel->rowCount(); ++spot) {
            const QString payload = panel->entryAt(spot).payload.toString();
            if (payload.startsWith(QLatin1String("level|")) ||
                payload.startsWith(QLatin1String("preset|"))) {
                return true;
            }
        }
    }
    return false;
}

/// What the file-dialog driver observed, so a caller can report precisely which
/// step went wrong instead of guessing at a hang. `returned` is the chooser's
/// own answer - the path it emitted when it accepted - not the request typed in.
struct FileDialogOutcome {
    QString requested;
    QString returned;
    QString selected;
    bool sawDialog = false;
    bool attempted = false;
    bool accepted = false;
    bool forcedClose = false;
};

// The home page owns exactly one current player: the dialog its mediaPlayer_
// points at after each open. Creation order cannot identify it - a failed
// first request keeps its dialog alive while the retry runs on the same one,
// and a replaced stream must never be read through the old window.
PlayerDialog* activePlayer(mikai::app::MainWindow* window) {
    return window != nullptr ? window->currentMediaPlayer() : nullptr;
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

/// The probe is a hermetic fixture client: the URL it hands the player must name
/// the loopback interface, so a mistaken argument cannot turn a QA run into an
/// outbound request against a real host.
bool loopbackUrl(const QUrl& url) {
    if (!url.isValid() || (url.scheme() != QLatin1String("http") &&
                           url.scheme() != QLatin1String("https"))) {
        return false;
    }
    const QString host = url.host();
    return host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost") ||
           host == QLatin1String("::1");
}

/// The chooser's own filename field. Once selectFile() has cleared the view
/// selection, QFileDialog::selectedFiles() falls back to these typed paths when
/// the field is not empty; accept() then validates and returns them. The field
/// is named, and the fallback to the single visible QLineEdit keeps this
/// working if that name ever moves.
QLineEdit* chooserFileNameField(QFileDialog* dialog) {
    if (auto* named = dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"))) {
        return named;
    }
    for (auto* edit : dialog->findChildren<QLineEdit*>()) {
        if (edit->isVisible()) return edit;
    }
    return nullptr;
}

/// Drives the file-open modal that `open` raises, and reports what happened.
///
/// getOpenFileName runs its own nested event loop, so the driver has to be a
/// timer that keeps polling for as long as that loop is running. A one-shot
/// scheduled after a fixed delay races the dialog's construction: when it loses
/// the race it drives nothing and the modal blocks forever. Polling also lets
/// the driver canonicalise the path before handing it to selectFile, which a
/// relative input needs and an absolute one already satisfies.
///
/// Every tick re-states the selection and re-accepts, because a single press
/// races the dialog's own asynchronous directory listing: selecting into a
/// dialog whose model has not listed the file yet is dropped, and the modal
/// would then sit open until the budget expires even though nothing is wrong
/// with the choice.
///
/// Acceptance is the chooser's own: `accepted` is set only by the signals
/// QFileDialog emits on its accept path, never by the request typed into the
/// field or by the modal merely disappearing. A modal that closes any other
/// way - a reject, or the budget below - leaves `accepted` false.
///
/// Bounded at both ends. If the dialog it keeps pressing does not close within
/// the budget it is rejected, so a modal that ignores the selection ends the run
/// as a visible failure rather than an indefinite hang - there is no fallback
/// path that pretends a file was chosen.
FileDialogOutcome driveFileDialog(const QString& path, const std::function<void()>& open) {
    constexpr int kTick = 50;
    constexpr int kBudgetMs = 20000;

    // selectFile resolves against the dialog's own directory, so a relative
    // path has to be made absolute first; canonicalFilePath also collapses ".."
    // and a symlinked temp dir, and falls back to absoluteFilePath for a path
    // that does not exist so the mistake surfaces in the dialog rather than as
    // a silent no-selection.
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    const QString target = canonical.isEmpty() ? info.absoluteFilePath() : canonical;

    FileDialogOutcome outcome;
    outcome.requested = target;
    QTimer poller;
    int elapsed = 0;
    QObject::connect(&poller, &QTimer::timeout, &poller, [&] {
        QFileDialog* dialog = nullptr;
        if (auto* modal = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            modal != nullptr && modal->isVisible()) {
            dialog = modal;
        } else {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (auto* candidate = qobject_cast<QFileDialog*>(widget);
                    candidate != nullptr && candidate->isVisible()) {
                    dialog = candidate;
                    break;
                }
            }
        }
        elapsed += kTick;
        if (dialog == nullptr) {
            // Nothing addressable yet. The budget still advances, so a dialog
            // that turns up late is closed rather than hanging the run.
            if (elapsed >= kBudgetMs) outcome.forcedClose = true;
            return;
        }
        if (!outcome.sawDialog) {
            outcome.sawDialog = true;
            // Drive the widget implementation whose fields Qt can address. The
            // two signals below fire only on the chooser's accept path, so they
            // - not the field the driver itself filled in - are the answer.
            dialog->setOption(QFileDialog::DontUseNativeDialog, true);
            QObject::connect(dialog, &QFileDialog::fileSelected, &poller,
                             [&outcome](const QString& file) {
                                 outcome.returned = file;
                                 outcome.accepted = true;
                             });
            QObject::connect(dialog, &QFileDialog::filesSelected, &poller,
                             [&outcome](const QStringList& files) {
                                 if (outcome.returned.isEmpty() && !files.isEmpty()) {
                                     outcome.returned = files.first();
                                 }
                             });
        }
        // selectFile clears the view selection and, when the model can resolve
        // the path, leaves only the bare file name in the field. On Windows
        // accept() validates an ExistingFile by testing the field text verbatim,
        // so a bare name is checked against the process directory, the dialog
        // reports "not found", and it never closes. The field is left holding
        // the absolute path instead, exactly as a viewer who pasted one would
        // leave it, so the chooser's own validation passes on the real file.
        dialog->selectFile(target);
        if (auto* field = chooserFileNameField(dialog)) {
            if (field->text() != target) field->setText(target);
        }
        outcome.attempted = true;
        // accept() is protected on QFileDialog; going through the public QDialog
        // base still dispatches to the chooser's own override.
        static_cast<QDialog*>(dialog)->accept();
        outcome.selected = dialog->selectedFiles().value(0);
        // The chooser's own result code is a second, independent witness to a
        // real accept rather than a reject.
        if (dialog->result() == QDialog::Accepted) outcome.accepted = true;
        if (elapsed >= kBudgetMs) {
            outcome.forcedClose = true;
            dialog->reject();
        }
    });
    poller.start(kTick);

    open();  // blocks inside the modal's nested exec until the dialog closes
    poller.stop();

    std::fprintf(stderr,
                 "     file-dialog: requested=%s appeared=%d attempted=%d accepted=%d forced=%d "
                 "returned=%s selected=%s\n",
                 target.toUtf8().constData(), int(outcome.sawDialog), int(outcome.attempted),
                 int(outcome.accepted), int(outcome.forcedClose),
                 outcome.returned.toUtf8().constData(), outcome.selected.toUtf8().constData());
    return outcome;
}

/// Whether a standalone (hostless) player offers none of the controls that only
/// make sense for a catalogue title: the dub and subtitle pickers, the episode
/// chevrons, and the quality list. Each is identified by the glyph it is
/// painted with or by the container it lives in, never by the words printed on
/// it, so a reworded or re-translated label leaves this passing.
bool videoOnly(PlayerDialog* player) {
    if (!player) return false;
    auto* shell = player->findChild<PlayerShell*>();
    if (!shell) return false;
    shell->wake();
    player->toggleSeries();
    auto* series = player->findChild<QPushButton*>(QStringLiteral("playerSeriesToggle"));
    bool clean = series && !series->isVisible();
    clean = clean && !anyVisibleGlyphButton(shell, IconPainter::microphone);
    clean = clean && !anyVisibleGlyphButton(shell, IconPainter::captions);
    clean = clean && !anyVisibleGlyphButton(shell, IconPainter::chevronLeft);
    clean = clean && !anyVisibleGlyphButton(shell, IconPainter::chevronRight);
    shell->openSettings();
    QTest::qWait(150);
    auto* settings = player->findChild<SettingsFlyout*>();
    if (!settings || !settings->isVisible()) return false;
    clean = clean && !flyoutOffersQuality(settings);
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
        std::fprintf(stderr,
                     "usage: external_media_probe <320x180 video file> <HTTP video URL>\n"
                     "The URL must be a loopback fixture (127.0.0.1/localhost/::1); the probe\n"
                     "never reaches a real host. HTTP fixture: /retry.mp4 returns 404 once,\n"
                     "then the same video.\n");
        return 2;
    }
    // The probe is routinely handed a path relative to wherever it was started,
    // and the file dialog resolves against its own directory, so the path is
    // made absolute once here. An absolute invocation is already absolute and
    // is left as it is; canonicalFilePath also collapses ".." and a symlinked
    // temp dir when the file exists.
    const QFileInfo givenFile(QString::fromLocal8Bit(argv[1]));
    const QString file =
        givenFile.canonicalFilePath().isEmpty() ? givenFile.absoluteFilePath()
                                                : givenFile.canonicalFilePath();
    // The M02 clip is the source the picker has to open; if it is absent the run
    // cannot prove anything about the handoff, so it stops before the first
    // window instead of reporting a picker failure that is really a bad path.
    if (!QFileInfo::exists(file)) {
        std::fprintf(stderr, "FAIL media file does not exist: %s\n", file.toUtf8().constData());
        return 2;
    }
    const QString remote = QString::fromLocal8Bit(argv[2]);
    if (!loopbackUrl(QUrl(remote))) {
        std::fprintf(stderr, "FAIL media URL is not a loopback fixture URL: %s\n",
                     remote.toUtf8().constData());
        return 2;
    }

    // The player's constructor reads player/anime4k and player/presetShortcuts,
    // so both scopes are redirected into a directory that dies with this process
    // before the first one is built. A probe must not inherit - or overwrite -
    // whatever a real session left behind.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("ExternalMedia"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    if (!mplayer::probes::selectStandardRuntime()) {
        return 2;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("home/feeds"), QStringLiteral(""));
    settings.setValue(QStringLiteral("player/anime4k"), QStringLiteral("off"));
    app.setStyleSheet(mikai::app::theme::buildStylesheet());
    mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
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

    const FileDialogOutcome pick =
        driveFileDialog(file, [&openFile] { openFile->trigger(); });
    // The path under test is the chooser's own answer, read from the dialog on
    // its accept path: the fileSelected/filesSelected signals it emitted. A
    // picker that never closed, closed by any route other than accept, or
    // answered with a different file fails on the real handoff; the request
    // typed into the field cannot make any of those pass.
    const auto answersWithTarget = [&pick](const QString& candidate) {
        if (candidate.isEmpty()) return false;
        if (candidate == pick.requested) return true;
        const QString canonical = QFileInfo(candidate).canonicalFilePath();
        return !canonical.isEmpty() && canonical == pick.requested;
    };
    const bool retained = answersWithTarget(pick.returned) || answersWithTarget(pick.selected);
    check(pick.sawDialog && pick.attempted && pick.accepted && !pick.forcedClose && retained,
          "the file picker opened, accepted, and returned the requested video");

    // A failed picker is reported above but must not sink the URL, retry and
    // reopen phases below, which do not depend on a local player. The dependent
    // checks are skipped rather than run against a null player, so nothing here
    // is reported as a pass it did not earn.
    QPointer<PlayerDialog> local = activePlayer(&main);
    if (local) {
        check(playback(local), "file picker opens real 320x180 video and renders colored pixels");
        check(videoOnly(local), "local video has no anime quality, episode, or release controls");
        auto* shell = local->findChild<PlayerShell*>();
        emit shell->copyLinkRequested();
        check(app.clipboard()->text() == file,
              "standalone copy uses the currently playing file path");
        shell->wake();
        local->grab().save(QStringLiteral("external-media-player.png"));
        // The enhancement tree is opened by the glyph the shell paints it with,
        // and what has to reach the viewer is the catalogue itself, so that is
        // what is asserted: a visible popover holding rows addressed by their
        // catalogue payload. The button's caption is display text and proves
        // nothing.
        if (auto* enhance = buttonWithGlyph(local, IconPainter::spark); enhance != nullptr) {
            enhance->click();
        }
        QTest::qWait(200);
        check(enhancementCatalogueVisible(local),
              "standalone media retains the Anime4K enhancement catalogue");
        shell->dismissOverlay();

        // The picture menu is opened with the same right-click a viewer uses,
        // and the compare row is pressed by the stable payload the shell hangs
        // on it, so the window below is reached the way a person would reach it.
        QTest::mouseClick(shell->videoSurface(), Qt::RightButton, Qt::NoModifier, QPoint(120, 120));
        QTest::qWait(150);
        check(pressMenuRow(local, QStringLiteral("compare")),
              "the picture menu offers a compare action");
        QTest::qWait(250);
        auto* compare = local->findChild<CompareWindow*>();
        check(compare && compare->isVisible() && compare->captureError().isEmpty() &&
              !compare->frozenFrame().isNull(),
              "standalone Compare captures the actual video frame");
        if (compare) compare->close();
    } else {
        std::fprintf(stderr, "SKIP local-file player phases: the picker opened no player\n");
    }

    answerInput(remote, true);
    openUrl->trigger();
    QPointer<PlayerDialog> streamed = activePlayer(&main);
    if (local) {
        check(until([&local] { return local.isNull(); }),
              "opening URL disposes the previous local player");
    }
    check(playback(streamed), "URL input opens real HTTP video and renders colored pixels");
    check(videoOnly(streamed), "HTTP playback has no anime quality, episode, or release controls");
    if (streamed) {
        auto* remoteShell = streamed->findChild<PlayerShell*>();
        emit remoteShell->copyLinkRequested();
        check(app.clipboard()->text() == remote,
              "standalone copy uses the actual HTTP media URL");
        streamed->close();
        check(until([&streamed] { return streamed.isNull(); }),
              "closing standalone player destroys it safely");
    }

    QUrl retryUrl(remote);
    retryUrl.setPath(QStringLiteral("/retry.mp4"));
    answerInput(retryUrl.toString(), true);
    openUrl->trigger();
    // The 404 surfaces on mpv's clock in two steps: the dialog exists from the
    // open, and the failed stream - with its retry chip - only exists once the
    // core reports the failure. The wait below observes a dialog whose retry
    // chip is already up, so the chip read cannot conclude a phase that has
    // not happened yet. Only onEngineError shows the chip, so reaching the
    // chip is reaching the error path itself.
    QPointer<PlayerDialog> failed;
    QPushButton* retry = nullptr;
    const bool failedArrived = until([&main, &failed, &retry] {
        failed = activePlayer(&main);
        if (failed == nullptr) return false;
        retry = failed->findChild<QPushButton*>(QStringLiteral("playerRetryButton"));
        return retry != nullptr && retry->isVisible();
    });
    check(failed, "failed media URL keeps the player window open");
    check(failedArrived, "failed media URL reports a playback error without an anime host");
    if (failedArrived && retry != nullptr) {
        retry->click();
        check(playback(failed), "retry reopens the same URL and recovers actual video playback");
        check(videoOnly(failed), "retry does not introduce anime quality or episode controls");
        // A direct-media retry must replay the same source, not re-extract or
        // silently switch to a different URL.
        auto* failedShell = failed->findChild<PlayerShell*>();
        if (failedShell) {
            emit failedShell->copyLinkRequested();
            check(app.clipboard()->text() == retryUrl.toString(),
                  "direct-media retry keeps the same source URL");
        }
    }

    answerInput(QUrl::fromLocalFile(file).toString(), true);
    openUrl->trigger();
    QPointer<PlayerDialog> reopened = activePlayer(&main);
    check(playback(reopened), "file URL opens after close and is normalized for mpv");
    main.close();
    check(until([&reopened] { return reopened.isNull(); }),
          "closing home closes its standalone video player");
    std::fprintf(stderr, "RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
