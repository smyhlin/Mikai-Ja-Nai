// Native menu probe: the native branch inside the real enhancement tree of a
// real player shell, exercised the way a viewer does it.
//
//   - the enhancements popover gains a native branch that opens its own level
//     in the same pooled, side-by-side tree; no seventh dock button, no new
//     window
//   - every row that would run something is disabled in this build, carries the
//     fact that blocks it as its tooltip, and cannot change the stored request
//   - Off is always choosable and clears the stored request
//   - a request stored through the preference contract marks the same row after
//     the tree is reopened: the mark follows the stable id, not a menu index
//   - the popover is grabbed so the row text and geometry are readable
//
// The process is bootstrapped explicitly onto the Standard runtime, in isolated
// settings, so the probe is deterministic and never touches the user's own
// player state. Run from the repo root, with the shaders deployed beside the
// exe:
//   build-cmake/native_menu_probe.exe

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>

#include "Anime4K.hpp"
#include "ProbeUi.hpp"
#include "PlayerDialog.hpp"
#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
#include "ShellTheme.hpp"
#include "ShellWidgets.hpp"
#include "native/NativeMenu.hpp"
#include "native/NativePrefs.hpp"
#include "native/NativeRuntimeSelection.hpp"

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

bool rowEntry(MenuPopup* panel, const QString& payload, MenuEntry* out) {
    for (int spot = 0; spot < panel->rowCount(); ++spot) {
        if (panel->entryAt(spot).payload.toString() == payload) {
            if (out != nullptr) {
                *out = panel->entryAt(spot);
            }
            return true;
        }
    }
    return false;
}

/// Opens the enhancement root and drills into the native branch, leaving its
/// panel on screen.
MenuPopup* openNativeBranch(PlayerShell* shell, QWidget* host) {
    QPushButton* trigger = enhanceButton(shell, host);
    if (trigger == nullptr) {
        return nullptr;
    }
    realClick(trigger);
    QCoreApplication::processEvents();
    MenuPopup* root = levelPanel(host, QString());
    if (root == nullptr) {
        return nullptr;
    }
    QPushButton* branch = rowButton(root, QStringLiteral("level|native"));
    if (branch == nullptr || !branch->isEnabled()) {
        return nullptr;
    }
    realClick(branch);
    QCoreApplication::processEvents();
    return levelPanel(host, QStringLiteral("native"));
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
    void requestOpenOriginal() override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    void playbackFailed() override { ++failures; }

    int failures = 0;
};

/// Pushes the stored request at the shell through the public one-shot input
/// setter, leaving the availability and status the shell already holds.
void pushStoredRequest(PlayerShell* shell) {
    native::menu::Inputs inputs = shell->nativeInputs();
    inputs.preferences = native::loadNativePreferences();
    shell->setNativeMenuInputs(inputs);
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
    PlayerDialog dialog(QStringLiteral("Native menu probe"),
                        QStringLiteral("av://lavfi:testsrc2=size=320x180:rate=25"), {}, 0, &host);
    dialog.show();
    QCoreApplication::processEvents();

    auto* shell = dialog.findChild<PlayerShell*>();
    check(shell != nullptr, "the standalone player has a shell");
    if (shell == nullptr) {
        return 2;
    }
    // An engine that cannot start - no GL context, no video output - asks the
    // host to recover once, before any row is touched. That is the media path,
    // not the menu, so the menu's own claim is measured as a delta.
    const int recoverBeforeMenu = host.failures;

    // ── the branch inside the real tree ─────────────────────────────────
    MenuPopup* native = openNativeBranch(shell, &dialog);
    check(native != nullptr, "the enhancement root opens the native branch by click");
    if (native == nullptr) {
        std::fprintf(stderr, "FAILURES %d\n", failures + 1);
        return 1;
    }

    int disabledRows = 0;
    bool sawOff = false;
    for (int spot = 0; spot < native->rowCount(); ++spot) {
        const MenuEntry entry = native->entryAt(spot);
        auto* row = qobject_cast<QPushButton*>(native->rowWidget(spot));
        check(row != nullptr, "every native row is a real row");
        if (row == nullptr) {
            continue;
        }
        check(!row->toolTip().isEmpty(), "every native row carries a tooltip");
        // The widget state and the entry must agree, because the widget is what
        // the viewer presses.
        check(row->isEnabled() == !entry.disabled, "the row widget matches its entry");
        if (entry.payload.toString().startsWith(QLatin1String("native|off|"))) {
            sawOff = true;
            check(row->isEnabled(), "Off is choosable even when nothing else is");
        } else {
            ++disabledRows;
            check(!row->isEnabled(), "an unavailable native row cannot be pressed");
        }
    }
    check(sawOff, "the native branch always offers Off");
    check(disabledRows > 0, "the branch shows the blocked rows rather than hiding them");
    shoot(native, QStringLiteral("native_menu_probe.png"));

    // ── the current mark survives a reopen ──────────────────────────────
    native::NativePreferencePatch request;
    request.enabled = true;
    QString saveReason;
    check(native::saveNativePreferences(request, &saveReason),
          QStringLiteral("a native request is stored: ") + saveReason);
    pushStoredRequest(shell);

    native = openNativeBranch(shell, &dialog);
    check(native != nullptr, "the native branch reopens");
    if (native != nullptr) {
        MenuEntry onEntry;
        const bool found = rowEntry(native, QStringLiteral("native|on|"), &onEntry);
        check(found && onEntry.current, "the stored request marks its own row as current");
        check(found && onEntry.disabled,
              "the current request is still shown as not applied");

        // A disabled row is informational: pressing it changes nothing.
        QPushButton* onRow = rowButton(native, QStringLiteral("native|on|"));
        if (onRow != nullptr) {
            realClick(onRow);
            QCoreApplication::processEvents();
        }
        check(native::loadNativePreferences().enabled,
              "a disabled row cannot change the stored request");
    }

    // Turning Off through the real row clears the request and keeps the mark on
    // the Off row after the tree is rebuilt.
    if (native != nullptr) {
        QPushButton* offRow = rowButton(native, QStringLiteral("native|off|"));
        check(offRow != nullptr && offRow->isEnabled(), "Off is reachable through the menu");
        if (offRow != nullptr) {
            realClick(offRow);
            QCoreApplication::processEvents();
        }
    }
    check(!native::loadNativePreferences().enabled, "choosing Off clears the stored request");

    native = openNativeBranch(shell, &dialog);
    if (native != nullptr) {
        MenuEntry offEntry;
        check(rowEntry(native, QStringLiteral("native|off|"), &offEntry) && offEntry.current,
              "the current mark follows the stable id after the reopen");
    }

    if (host.failures != recoverBeforeMenu) {
        const QLabel* status = dialog.findChild<QLabel*>(QStringLiteral("playerStatusText"));
        std::fprintf(stderr, "     the menu asked the host to recover: status=\"%s\"\n",
                     status != nullptr ? qPrintable(status->text()) : "<no status label>");
    }
    check(host.failures == recoverBeforeMenu,
          "no native menu row asked the host to recover");

    if (failures == 0) {
        std::printf("OK native_menu_probe\n");
    } else {
        std::fprintf(stderr, "FAILURES %d\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
