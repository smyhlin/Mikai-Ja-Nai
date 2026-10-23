// Standard-runtime player menu probe: supported public choices remain available,
// private Native controls are absent, and a stale private payload cannot act.
//
//   - Anime4K and official AnimeJaNai remain real menu branches
//   - private Native preferences survive the Standard player unchanged
//   - a stale Native row cannot dispatch after the runtime boundary
//   - the popover is grabbed so the row text and geometry are readable
//
// The process is bootstrapped explicitly onto the Standard runtime, in isolated
// settings, so the probe is deterministic and never touches the user's own
// player state. Run from the repo root, with shaders deployed beside the exe:
//   build-cmake/native_menu_probe.exe

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <cstdio>

#include "Anime4K.hpp"
#include "PresetShortcuts.hpp"
#include "ProbeUi.hpp"
#include "PlayerDialog.hpp"
#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
#include "ShellTheme.hpp"
#include "ShellWidgets.hpp"
#include "VideoSurface.hpp"
#include "native/status/NativeMenu.hpp"
#include "native/bundle/NativePrefs.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"

using namespace mplayer;
using namespace mplayer::shell;

namespace {

int failures = 0;

void check(bool ok, const QString& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) {
        ++failures;
    }
}

/// A press the row's own event handlers see, so the probe exercises the widget
/// path rather than calling the handler directly.
void realClick(QWidget* widget) {
    const QPointF at = widget->rect().center();
    const QPointF global = widget->mapToGlobal(at.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, at, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
}

/// The dock's enhancement button, found by the glyph a viewer actually sees
/// rather than by a name: the chrome auto-hides, and a hidden button is not one
/// a viewer could press, so the shell is woken first.
QPushButton* enhanceButton(PlayerShell* shell, QWidget* host) {
    shell->wake();
    QCoreApplication::processEvents();
    for (IconButton* button : host->findChildren<IconButton*>()) {
        if (button->isVisible() && button->icon() == &IconPainter::spark) {
            return button;
        }
    }
    return nullptr;
}

MenuPopup* levelPanel(QWidget* host, const QString& levelId) {
    for (MenuPopup* panel : host->findChildren<MenuPopup*>()) {
        if (panel->isVisible() && panel->property("enhancementLevel").toString() == levelId) {
            return panel;
        }
    }
    return nullptr;
}

QPushButton* rowButton(MenuPopup* panel, const QString& payload) {
    for (int spot = 0; spot < panel->rowCount(); ++spot) {
        if (panel->entryAt(spot).payload.toString() == payload) {
            return qobject_cast<QPushButton*>(panel->rowWidget(spot));
        }
    }
    return nullptr;
}

/// Opens the enhancement root through the button a viewer uses.
MenuPopup* openEnhancementRoot(PlayerShell* shell, QWidget* host) {
    QPushButton* trigger = enhanceButton(shell, host);
    if (trigger == nullptr) {
        return nullptr;
    }
    realClick(trigger);
    QCoreApplication::processEvents();
    return levelPanel(host, QString());
}

/// Grabs the popover and writes it out, then proves the grab is readable: a
/// popover that collapsed to nothing would otherwise pass silently.
void shoot(QWidget* widget, const QString& name) {
    if (widget == nullptr) {
        check(false, QStringLiteral("there is a popover to screenshot: ") + name);
        return;
    }
    const QPixmap shot = widget->grab();
    check(!shot.isNull() && shot.width() >= 80 && shot.height() >= 40,
          QStringLiteral("the popover geometry is readable: ") + name);
    const QString path = QDir::current().filePath(QStringLiteral("build-cmake/") + name);
    const bool written =
        !shot.isNull() && QDir().mkpath(QFileInfo(path).absolutePath()) && shot.save(path);
    check(written, QStringLiteral("screenshot written: ") + name);
}

struct FakeHost : PlayerHost {
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    // Composition double only: the player needs a host to exist. The probe never
    // reads a callback count as evidence - playback continuity below is what
    // proves the media path was left alone.
    void playbackFailed() override {}
};

/// Pushes the stored request at the shell through the public one-shot input
/// setter, leaving the availability and status the shell already holds.
void pushStoredRequest(PlayerShell* shell) {
    native::menu::Inputs inputs = shell->nativeInputs();
    inputs.preferences = native::loadNativePreferences();
    shell->setNativeMenuInputs(inputs);
}

// ── stored-binding keyboard scenarios ──────────────────────────────────
//
// The binding is installed when the player is built, so each scenario owns its
// store state *before* its player exists and its own dialog after: a second
// player in the same process would be answering for the first one's settings.

/// Empties the isolated store, so one scenario's bindings cannot leak into the
/// next one's loader.
void clearStore() {
    QSettings settings;
    settings.clear();
    settings.sync();
}

/// Seeds a preset binding in the group the preset loader reads.
void seedPresetBinding(const QString& presetId, const QString& portableText) {
    QSettings settings;
    settings.beginGroup(QLatin1String(kPresetShortcutSettingsGroup));
    settings.setValue(presetId, portableText);
    settings.endGroup();
    settings.sync();
}

/// A keystroke as a viewer's arrives: the player window is active and the key
/// goes through the window's own dispatch, where a WindowShortcut binding and
/// the player's built-in keys are resolved together.
void pressKey(QWidget* window, const QKeySequence& sequence) {
    if (window == nullptr || sequence.isEmpty()) {
        check(false, QStringLiteral("a keystroke has a window and a key to send"));
        return;
    }
    window->activateWindow();
    for (int waited = 0; QApplication::activeWindow() != window && waited < 1000; waited += 20) {
        QCoreApplication::processEvents();
    }
    // WindowShortcut dispatch keys off the active window, so a press that lands
    // while another window owns the keyboard would exercise the wrong dialog's
    // binding - and could even pass on that other dialog's shortcut. Refuse to
    // send a misrouted key rather than letting the assertions read one.
    if (QApplication::activeWindow() != window) {
        check(false, QStringLiteral("the window under test owns the keystroke that is sent"));
        return;
    }
    QTest::keyClick(window, sequence[0].key(), sequence[0].keyboardModifiers());
    QCoreApplication::processEvents();
}

/// Spins the event loop until the surface has decoded a real picture. A stream
/// that never produces one is a failure the probe must not read past.
bool waitForVideo(VideoSurface* surface, int timeoutMs = 15000) {
    for (int waited = 0; waited < timeoutMs; waited += 40) {
        QCoreApplication::processEvents();
        if (surface != nullptr && surface->hasVideo() && surface->videoWidth() > 0 &&
            surface->videoHeight() > 0) {
            return true;
        }
        QThread::msleep(40);
    }
    return surface != nullptr && surface->hasVideo();
}

/// The stream's own progress in seconds: the media path's clock, not a host
/// callback. A binding decision has to leave it running.
bool clockAdvanced(VideoSurface* surface, double from, int timeoutMs = 4000) {
    for (int waited = 0; waited < timeoutMs; waited += 40) {
        QCoreApplication::processEvents();
        if (surface != nullptr && surface->position() > from) {
            return true;
        }
        QThread::msleep(40);
    }
    return surface != nullptr && surface->position() > from;
}

/// The media path's own failure UI: the retry chip a failed stream raises. A
/// processing decision must never make it appear.
bool mediaRetryVisible(const QWidget* dialog) {
    const auto* retry = dialog != nullptr
                            ? dialog->findChild<QPushButton*>(QStringLiteral("playerRetryButton"))
                            : nullptr;
    return retry != nullptr && retry->isVisible();
}

/// One scenario's player and the host it answers to. The dialog owns a real
/// lavfi stream, so every binding decision is measured against a picture that is
/// actually playing rather than against a callback count.
struct Scenario {
    FakeHost host;
    QPointer<PlayerDialog> dialog;
    bool mediaError = false;

    Scenario()
        : dialog(new PlayerDialog(QStringLiteral("Native menu probe"),
                                  QStringLiteral("av://lavfi:testsrc2=size=320x180:rate=25"),
                                  QList<QualityEntry>{},
                                  0,
                                  &host)) {
        // The media path's own error report, read from the surface the probe
        // already has; nothing is added to the application API.
        if (VideoSurface* live = surface()) {
            QObject::connect(live, &VideoSurface::errorOccurred, dialog.data(),
                             [this](const QString&) { mediaError = true; });
        }
        dialog->startPlayer();
    }

    ~Scenario() {
        if (dialog == nullptr) {
            return;
        }
        // The dialog carries Qt::WA_DeleteOnClose, so it is never a stack member
        // Qt could destroy a second time: close() schedules the delete and the
        // QPointer is the only handle touched afterwards. It has to land before
        // the members die, because the dialog reads &host.
        dialog->close();
        for (int waited = 0; dialog != nullptr && waited < 6000; waited += 20) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents();
        }
        // If a future edit stops close() from deleting, own the teardown here
        // rather than leaking a window bound to a destroyed host.
        if (dialog != nullptr) {
            delete dialog.data();
        }
    }

    VideoSurface* surface() const {
        return dialog != nullptr ? dialog->findChild<VideoSurface*>() : nullptr;
    }

    PlayerShell* shell() const {
        return dialog != nullptr ? dialog->findChild<PlayerShell*>() : nullptr;
    }

    /// Shows and activates the window and waits for a real picture, which is the
    /// baseline every key press is judged against.
    bool begin() {
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
        QCoreApplication::processEvents();
        return waitForVideo(surface());
    }

    /// The picture is still on screen and the media path has raised no failure
    /// UI: the stream survived the decision untouched.
    bool streamUnbroken() const {
        VideoSurface* live = surface();
        return live != nullptr && live->hasVideo() && live->videoWidth() > 0 && !mediaError &&
               !mediaRetryVisible(dialog.data());
    }
};

/// What the shell shows about native processing, read as the viewer reads it:
/// the chip that is actually on screen, and the producer's own snapshot behind
/// it. `producerActive` is the surface's last read of the filter - the only
/// thing that may state Active - and `claimsActive` is the consumer derivation
/// the chip renders from. A stored request alone can make neither true.
struct ShownNativeStatus {
    bool chipVisible = false;
    QString chipText;
    QString chipTooltip;
    bool producerActive = false;
    bool claimsActive = false;
    QString reason;
};

ShownNativeStatus shownNativeStatus(PlayerShell* shell) {
    ShownNativeStatus seen;
    const auto* chip = shell->findChild<QLabel*>(QStringLiteral("playerNativeStatus"));
    if (chip != nullptr) {
        seen.chipVisible = chip->isVisible();
        seen.chipText = chip->text();
        // The chip carries the fact as its tooltip, so the displayed UI - not a
        // counter - is what proves the viewer can see why nothing is running.
        seen.chipTooltip = chip->toolTip();
    }
    // Anchored on the surface's own view, not the shell input derived from it: an
    // Active claim is only accounted for when the filter reported it.
    const native::NativeStatusView& status = shell->videoSurface()->nativeStatus();
    seen.producerActive =
        status.snapshot.has_value() && status.snapshot->state == native::NativeState::Active;
    const native::menu::StatusLines lines = native::menu::statusLines(shell->nativeInputs());
    seen.claimsActive = lines.active;
    seen.reason = lines.reason;
    return seen;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (!mikai::probes::initializeUi(app)) return 2;
    app.setQuitOnLastWindowClosed(false);

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiNativeMenuProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("NativeMenuProbe"));
    // The user's own player settings stay theirs: everything here is read and
    // written inside a temporary directory that dies with the process.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings settings;
    settings.clear();
    settings.sync();

    // Explicit bootstrap: the app picks the runtime before any core exists, so
    // the probe does the same and then sees the same selection the app would.
    const native::RuntimeSelection bootstrap = native::selectStandardRuntime();
    check(bootstrap.kind == native::BundleKind::Standard,
          "the process is bootstrapped on the Standard runtime");
    check(native::currentSelection().kind == native::BundleKind::Standard,
          "the Standard runtime is the recorded selection");

    FakeHost host;
    // Declared before the dialog so it outlives the connection that writes it.
    bool mediaError = false;
    PlayerDialog dialog(QStringLiteral("Native menu probe"),
                        QStringLiteral("av://lavfi:testsrc2=size=320x180:rate=25"), {}, 0, &host);
    auto* menuSurface = dialog.findChild<VideoSurface*>();
    if (menuSurface != nullptr) {
        QObject::connect(menuSurface, &VideoSurface::errorOccurred, &dialog,
                         [&mediaError](const QString&) { mediaError = true; });
    }
    dialog.startPlayer();
    dialog.show();
    QCoreApplication::processEvents();

    auto* shell = dialog.findChild<PlayerShell*>();
    check(shell != nullptr, "the standalone player has a shell");
    if (shell == nullptr) {
        return 2;
    }
    // The menu is judged against a picture that is actually playing: the native
    // rows must leave the media path untouched, and a callback count would prove
    // nothing. The position is the baseline the stream has to advance past.
    check(waitForVideo(menuSurface), "the menu probe's own stream produces a real picture");
    if (menuSurface == nullptr || !menuSurface->hasVideo()) {
        std::fprintf(stderr, "FAIL no render context: lavfi testsrc2 did not load\n");
        return 2;
    }
    const double menuStart = menuSurface->position();

    // ── Standard public enhancement choices ─────────────────────────────
    MenuPopup* root = openEnhancementRoot(shell, &dialog);
    check(root != nullptr, "the enhancement root opens by click");
    if (root == nullptr) {
        std::fprintf(stderr, "FAILURES %d\n", failures + 1);
        return 1;
    }
    auto hasPayload = [root](const QString& payload) {
        for (int spot = 0; spot < root->rowCount(); ++spot) {
            if (root->entryAt(spot).payload.toString() == payload) return true;
        }
        return false;
    };
    check(hasPayload(QStringLiteral("level|anime4k")),
          "the Standard enhancement root offers Anime4K");
    check(hasPayload(QStringLiteral("level|animejanai")),
          "the Standard enhancement root offers official AnimeJaNai");
    check(!hasPayload(QStringLiteral("level|native")) &&
              !hasPayload(QStringLiteral("level|rife")),
          "the Standard root omits private Native and RIFE branches");
    shoot(root, QStringLiteral("native_menu_probe.png"));

    // Store a request that the public Standard menu does not own. The runtime
    // must preserve it, and a stale popup payload must not dispatch it.
    native::NativePreferencePatch request;
    request.enabled = true;
    request.backend = native::Backend::DirectML;
    request.recipeId = QStringLiteral("user:preserved");
    QString saveReason;
    check(native::saveNativePreferences(request, &saveReason),
          QStringLiteral("a private Native request is stored: ") + saveReason);
    pushStoredRequest(shell);
    const native::NativePreferences saved = native::loadNativePreferences();
    QSignalSpy nativeActions(shell, &PlayerShell::nativeActionRequested);
    MenuEntry stalePrivateRow;
    stalePrivateRow.text = QStringLiteral("Stale Native action");
    stalePrivateRow.payload = QStringLiteral("native|on|");
    root->setEntries({stalePrivateRow});
    QPushButton* staleRow = rowButton(root, QStringLiteral("native|on|"));
    check(staleRow != nullptr, "a stale Native popup row can be represented");
    if (staleRow != nullptr) {
        realClick(staleRow);
        QCoreApplication::processEvents();
    }
    check(nativeActions.isEmpty(),
          "a stale private payload emits no native action from Standard");
    const native::NativePreferences afterStaleClick = native::loadNativePreferences();
    check(afterStaleClick.enabled == saved.enabled &&
              afterStaleClick.backend == saved.backend &&
              afterStaleClick.recipeId == saved.recipeId,
          "a stale private payload leaves every saved Native request field untouched");
    check(menuSurface->hasVideo() && !mediaError && !mediaRetryVisible(&dialog),
          "the Standard enhancement menu leaves the Original picture up with no retry chip");
    shoot(root, QStringLiteral("native_menu_probe.png"));

    check(clockAdvanced(menuSurface, menuStart),
          "the native menu never stopped the stream's clock");

    // ── preset keys still drive the player menus, not native ─────────
    //
    // Native processing has no toggle key: it is driven from the player menus.
    // A stored preset binding must still install and move the applied mode, and
    // it must never create a native request on its own.
    {
        clearStore();
        // The catalogue leads with Off, which is not a preset a key can apply.
        // Pick the first real enhancement, then prove the key moves the applied
        // mode rather than reading a coincidence.
        const QVector<anime4k::ModeInfo> modes = anime4k::availableModes();
        QString presetId;
        for (const anime4k::ModeInfo& info : modes) {
            if (info.mode != anime4k::Mode::Off) {
                presetId = info.id;
                break;
            }
        }
        check(!presetId.isEmpty(), "the catalogue offers a non-Off preset to bind");
        if (presetId.isEmpty()) {
            std::fprintf(stderr, "FAILURES %d\n", failures + 1);
            return 1;
        }
        const QKeySequence binding(QStringLiteral("Ctrl+Shift+P"));
        seedPresetBinding(presetId, binding.toString(QKeySequence::PortableText));

        Scenario scenario;
        const bool live = scenario.begin();
        check(live, "the preset-key scenario shows a live picture");
        PlayerShell* scenarioShell = scenario.shell();
        check(scenarioShell != nullptr, "the preset-key scenario has a shell");
        if (!live || scenarioShell == nullptr) {
            std::fprintf(stderr, "FAILURES %d\n", failures + 1);
            return 1;
        }
        const double started = scenario.surface()->position();

        const anime4k::Mode wanted = anime4k::modeForId(presetId);
        const anime4k::Mode before = scenarioShell->videoSurface()->anime4KMode();
        check(wanted != anime4k::Mode::Off, "the bound preset is a real enhancement, not Off");
        check(before != wanted, "the preset's mode is not already applied before the key");
        pressKey(scenario.dialog.data(), binding);
        check(scenarioShell->videoSurface()->anime4KMode() == wanted,
              "the preset key still applies its mode");
        check(!native::loadNativePreferences().enabled,
              "a preset key stores no native request");
        const ShownNativeStatus applied = shownNativeStatus(scenarioShell);
        check(!applied.producerActive && !applied.claimsActive,
              "a preset key claims no active native chain");
        check(scenario.streamUnbroken(),
              "the preset key left the stream playing with no retry chip");
        check(clockAdvanced(scenario.surface(), started),
              "the preset key never stopped the stream's clock");
    }

    // A key the player acts on itself still behaves exactly as it does for any
    // viewer, and it stores no native request either.
    {
        clearStore();

        Scenario scenario;
        const bool live = scenario.begin();
        check(live, "the reserved-key scenario shows a live picture");
        if (!live) {
            std::fprintf(stderr, "FAILURES %d\n", failures + 1);
            return 1;
        }
        // The player opens fullscreen; the probe proves the key still owns the
        // window mode rather than pinning which way it starts.
        const bool startedFullscreen = scenario.dialog->isFullScreen();

        pressKey(scenario.dialog.data(), QKeySequence(QStringLiteral("F")));
        check(scenario.dialog->isFullScreen() != startedFullscreen,
              "a reserved key still runs the player's own fullscreen action");
        check(!native::loadNativePreferences().enabled,
              "a reserved key stores no native request");

        pressKey(scenario.dialog.data(), QKeySequence(QStringLiteral("F")));
        check(scenario.dialog->isFullScreen() == startedFullscreen,
              "the player's own key is still reversible");
        check(scenario.streamUnbroken(),
              "the reserved key left the Original picture up with no retry chip");
    }

    if (failures == 0) {
        std::printf("OK native_menu_probe\n");
    } else {
        std::fprintf(stderr, "FAILURES %d\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
