#include <QAction>
#include <QApplication>
#include <QAccessible>
#include <QAccessibleInterface>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMap>
#include <QMimeData>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <functional>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <backend/MikaiClient.hpp>
#include <gui/Clipboard.hpp>
#include "CrashLog.hpp"
#include "pages/MainWindow.hpp"
#include "pages/SearchPage.hpp"
#include "theme/Theme.hpp"
#include "widgets/TitleCard.hpp"
#include "PlayerDialog.hpp"
#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
#include "CompareWindow.hpp"
#include "ShellTheme.hpp"
#include "ShellWidgets.hpp"
#include "VideoSurface.hpp"
#include "ProbeRuntime.hpp"
#include "ProbePicture.hpp"

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

// Quality rows are nested; speed presets are direct children.
bool flyoutOffersQuality(const SettingsFlyout* flyout) {
    for (const auto* button : flyout->findChildren<QPushButton*>()) {
        if (button->parentWidget() != flyout && button->isVisible()) return true;
    }
    return false;
}

// Menu payloads are stable across translations.
bool pressMenuRow(QWidget* root, const QString& action) {
    for (auto* panel : root->findChildren<MenuPopup*>()) {
        if (!panel->isVisible()) continue;
        for (int spot = 0; spot < panel->rowCount(); ++spot) {
            if (panel->entryAt(spot).payload.toString() != action) continue;
            if (auto* button = qobject_cast<QPushButton*>(panel->rowWidget(spot));
                button != nullptr && button->isVisible() && button->isEnabled()) {
                button->click();
                return true;
            }
        }
    }
    return false;
}

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

// Record the chooser's emitted answer, not just our requested path.
struct FileDialogOutcome {
    QString requested;
    QString returned;
    QString selected;
    bool sawDialog = false;
    bool attempted = false;
    bool accepted = false;
    bool forcedClose = false;
};

// A failed request can retain its player; creation order is not ownership.
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

// Only loopback HTTP fixtures are allowed.
bool loopbackUrl(const QUrl& url) {
    if (!url.isValid() || (url.scheme() != QLatin1String("http") &&
                           url.scheme() != QLatin1String("https"))) {
        return false;
    }
    const QString host = url.host();
    return host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost") ||
           host == QLatin1String("::1");
}

QLineEdit* chooserFileNameField(QFileDialog* dialog) {
    if (auto* named = dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"))) {
        return named;
    }
    for (auto* edit : dialog->findChildren<QLineEdit*>()) {
        if (edit->isVisible()) return edit;
    }
    return nullptr;
}

// The chooser runs a nested event loop and lists directories asynchronously.
// Poll and repeat acceptance until it emits an answer, or reject at the deadline.
FileDialogOutcome driveFileDialog(const QString& path, const std::function<void()>& open) {
    constexpr int kTick = 50;
    constexpr int kBudgetMs = 20000;

    // selectFile resolves relative paths against the chooser's directory.
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
            if (elapsed >= kBudgetMs) outcome.forcedClose = true;
            return;
        }
        if (!outcome.sawDialog) {
            outcome.sawDialog = true;
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
        // Windows validates the filename field verbatim, so retain its absolute path.
        dialog->selectFile(target);
        if (auto* field = chooserFileNameField(dialog)) {
            if (field->text() != target) field->setText(target);
        }
        outcome.attempted = true;
        // accept() is protected on QFileDialog; going through the public QDialog
        // base still dispatches to the chooser's own override.
        static_cast<QDialog*>(dialog)->accept();
        outcome.selected = dialog->selectedFiles().value(0);
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
    const QImage image = surface->grabVideoFramebuffer();
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

bool triggerCardAction(mikai::app::TitleCard* card, const QString& caption) {
    if (card == nullptr) return false;
    bool triggered = false;
    int elapsed = 0;
    QTimer poller;
    poller.setInterval(20);
    QObject::connect(&poller, &QTimer::timeout, &poller, [&] {
        elapsed += poller.interval();
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* menu = qobject_cast<QMenu*>(widget);
            if (menu == nullptr || !menu->isVisible()) continue;
            for (QAction* action : menu->actions()) {
                if (action->text() != caption) continue;
                triggered = true;
                action->trigger();
                menu->close();
                poller.stop();
                return;
            }
        }
        if (elapsed >= 5000) {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (auto* menu = qobject_cast<QMenu*>(widget); menu != nullptr && menu->isVisible()) {
                    menu->close();
                }
            }
            poller.stop();
        }
    });
    poller.start();
    const QPoint local(4, 4);
    QContextMenuEvent event(QContextMenuEvent::Mouse, local, card->mapToGlobal(local));
    QApplication::sendEvent(card, &event);
    poller.stop();
    return triggered;
}

QObject* accessibleAlertTarget = nullptr;
bool accessibleAlertObserved = false;
QAccessible::UpdateHandler previousAccessibleUpdateHandler = nullptr;

void captureAccessibleAlert(QAccessibleEvent* event) {
    if (event != nullptr && event->type() == QAccessible::Alert &&
        event->object() == accessibleAlertTarget) {
        accessibleAlertObserved = true;
    }
    if (previousAccessibleUpdateHandler != nullptr) {
        previousAccessibleUpdateHandler(event);
    }
}

class AccessibleAlertCapture {
public:
    explicit AccessibleAlertCapture(QObject* target)
        : wasActive_(QAccessible::isActive()) {
        accessibleAlertTarget = target;
        accessibleAlertObserved = false;
        previousAccessibleUpdateHandler = QAccessible::installUpdateHandler(
            &captureAccessibleAlert);
        QAccessible::setActive(true);
    }

    ~AccessibleAlertCapture() {
        QAccessible::installUpdateHandler(previousAccessibleUpdateHandler);
        QAccessible::setActive(wasActive_);
        accessibleAlertTarget = nullptr;
        previousAccessibleUpdateHandler = nullptr;
    }

    bool observed() const { return accessibleAlertObserved; }

private:
    bool wasActive_;
};

bool statusFeedbackChanged(QLabel* status, const QString& before, const QString& imagePath,
                           bool alertObserved) {
    if (status == nullptr || !status->isVisible() || status->visibleRegion().isEmpty() ||
        status->text().isEmpty() || status->text() == before) return false;
    QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(status);
    const bool exposed = accessible != nullptr &&
                         !accessible->state().invisible &&
                         accessible->text(QAccessible::Name) == status->text();
    const bool captured = status->window()->grab().save(imagePath);
    std::fprintf(stderr,
                 "     rendered feedback: %s (%s) screenshot=%s accessible=%d alert=%d\n",
                 status->text().toUtf8().constData(), imagePath.toUtf8().constData(),
                 captured ? "saved" : "failed", exposed, alertObserved);
    return exposed && captured && alertObserved;
}

// Positive copy flows restore Qt-exposed formats, text and image data only.
class ClipboardRestore {
public:
    ClipboardRestore() {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == QLatin1String("windows")) {
            if (!OpenClipboard(nullptr)) return;
            if (!CloseClipboard()) return;
        }
#endif
        captured_ = true;
        const QMimeData* mime = QApplication::clipboard()->mimeData();
        if (mime == nullptr) return;
        for (const QString& format : mime->formats()) contents_.insert(format, mime->data(format));
        text_ = mime->text();
        hasImage_ = mime->hasImage();
        if (hasImage_) image_ = QApplication::clipboard()->image();
    }

    bool isValid() const { return captured_; }

    bool restore() {
        if (!captured_) return false;
        if (restored_) return true;
        if (matchesOriginal()) {
            restored_ = true;
            return true;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
#ifdef Q_OS_WIN
            if (QGuiApplication::platformName() == QLatin1String("windows")) {
                if (!OpenClipboard(nullptr)) continue;
                if (!CloseClipboard()) continue;
            }
#endif
            auto* mime = new QMimeData;
            for (auto it = contents_.cbegin(); it != contents_.cend(); ++it) {
                mime->setData(it.key(), it.value());
            }
            if (hasImage_) mime->setImageData(image_);
            QApplication::clipboard()->setMimeData(mime);
#ifdef Q_OS_WIN
            if (QGuiApplication::platformName() == QLatin1String("windows") &&
                !QApplication::clipboard()->ownsClipboard()) continue;
#endif
            if (matchesOriginal()) {
                restored_ = true;
                return true;
            }
        }
        std::fprintf(stderr, "FAIL original clipboard formats/content could not be restored\n");
        return false;
    }

    int finish(int result) { return restore() ? result : 1; }

    ~ClipboardRestore() {
        if (captured_ && !restored_) restore();
    }

private:
    QMap<QString, QByteArray> contents_;
    bool matchesOriginal() const {
        const QMimeData* actual = QApplication::clipboard()->mimeData();
        QMap<QString, QByteArray> contents;
        if (actual != nullptr) {
            for (const QString& format : actual->formats()) {
                contents.insert(format, actual->data(format));
            }
        }
        return contents == contents_ && QApplication::clipboard()->text() == text_ &&
            (actual != nullptr ? actual->hasImage() : false) == hasImage_ &&
            (!hasImage_ || QApplication::clipboard()->image().convertToFormat(
                QImage::Format_RGBA8888) == image_.convertToFormat(QImage::Format_RGBA8888));
    }

    QString text_;
    QImage image_;
    bool hasImage_ = false;
    bool captured_ = false;
    bool restored_ = false;
};

#ifdef Q_OS_WIN
// A real owned HWND is mandatory: Windows only denies a competing OpenClipboard while a
// window owns the lock, so a NULL owner would let the probing process open it too.
HWND createClipboardHolderWindow() {
    const wchar_t className[] = L"MikaiClipboardHolder";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    RegisterClassExW(&wc);
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, className,
                           L"mikai clipboard holder", WS_POPUP, 0, 0, 0, 0,
                           nullptr, nullptr, wc.hInstance, nullptr);
}

// Bounded explicit release: 'x' from the parent, a closed stdin, or the timeout.
int waitForClipboardReleaseSignal(int timeoutMs) {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    for (int waited = 0; waited <= timeoutMs; waited += 50) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (input != nullptr && input != INVALID_HANDLE_VALUE) {
            DWORD available = 0;
            if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
                // The parent closed its write end (broken pipe): release immediately.
                if (GetLastError() == ERROR_BROKEN_PIPE) return 2;
            } else if (available > 0) {
                char byte = 0;
                DWORD read = 0;
                if (!ReadFile(input, &byte, 1, &read, nullptr) || read == 0) return 2;
                if (byte == 'x') return 0;
            }
        }
        Sleep(50);
    }
    return 3;
}
#endif

int holdClipboardOpen() {
#ifdef Q_OS_WIN
    HWND owner = createClipboardHolderWindow();
    if (owner == nullptr) return 2;
    if (!OpenClipboard(owner)) {
        DestroyWindow(owner);
        return 2;
    }
    DWORD ownerPid = 0;
    GetWindowThreadProcessId(owner, &ownerPid);
    std::fprintf(stdout, "CLIPBOARD-OPEN pid=%lu owner=%p\n",
                 static_cast<unsigned long>(ownerPid), static_cast<void*>(owner));
    std::fflush(stdout);
    const int released = waitForClipboardReleaseSignal(120000);
    CloseClipboard();
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    DestroyWindow(owner);
    return released;
#else
    return 2;
#endif
}

#ifdef Q_OS_WIN
struct ClipboardSnapshot {
    QMap<QString, QByteArray> contents;
    QList<UINT> nativeFormats;
    QString text;
    QImage image;
    DWORD sequence = 0;
    bool valid = false;

    static ClipboardSnapshot capture() {
        ClipboardSnapshot snapshot;
        if (const QMimeData* mime = QApplication::clipboard()->mimeData()) {
            for (const QString& format : mime->formats()) {
                snapshot.contents.insert(format, mime->data(format));
            }
            snapshot.text = mime->text();
            if (mime->hasImage()) snapshot.image = QApplication::clipboard()->image();
        }
        if (!OpenClipboard(nullptr)) return snapshot;
        for (UINT format = EnumClipboardFormats(0); format != 0;
             format = EnumClipboardFormats(format)) {
            snapshot.nativeFormats.append(format);
        }
        snapshot.sequence = GetClipboardSequenceNumber();
        snapshot.valid = CloseClipboard() != 0;
        return snapshot;
    }

    bool unchanged() const {
        // The sequence proves no mutation of even opaque native formats.
        if (!valid || GetClipboardSequenceNumber() != sequence) return false;
        const ClipboardSnapshot after = capture();
        return after.valid && after.sequence == sequence && after.contents == contents &&
            after.nativeFormats == nativeFormats && after.text == text &&
            after.image.convertToFormat(QImage::Format_RGBA8888) ==
                image.convertToFormat(QImage::Format_RGBA8888);
    }
};

class ClipboardLock {
public:
    bool acquire() {
        process_.setProcessChannelMode(QProcess::SeparateChannels);
        process_.start(QCoreApplication::applicationFilePath(),
                       {QStringLiteral("--hold-clipboard")});
        if (!process_.waitForStarted(10000)) {
            logHolder("process did not start");
            return false;
        }
        if (!process_.waitForReadyRead(10000)) {
            logHolder("process never reported ownership");
            cleanup();
            return false;
        }
        if (!process_.readAllStandardOutput().contains("CLIPBOARD-OPEN")) {
            logHolder("process reported no clipboard ownership");
            cleanup();
            return false;
        }
        // Real OS refusal: the probing process must be denied the held clipboard.
        if (OpenClipboard(nullptr)) {
            CloseClipboard();
            std::fprintf(stderr, "FAIL clipboard holder did not refuse a competing open\n");
            logHolder("competing open was not denied");
            cleanup();
            return false;
        }
        const unsigned long denial = static_cast<unsigned long>(GetLastError());
        HWND holder = GetOpenClipboardWindow();
        DWORD holderPid = 0;
        if (holder != nullptr) GetWindowThreadProcessId(holder, &holderPid);
        if (holder == nullptr || holderPid != static_cast<DWORD>(process_.processId())) {
            std::fprintf(stderr,
                         "FAIL clipboard holder owner window %p pid=%lu is not the spawned "
                         "holder pid=%lld (competing open denied with %lu)\n",
                         static_cast<void*>(holder), static_cast<unsigned long>(holderPid),
                         static_cast<long long>(process_.processId()),
                         static_cast<unsigned long>(denial));
            logHolder("owner window belongs to another process");
            cleanup();
            return false;
        }
        return true;
    }

    bool release() {
        if (process_.state() == QProcess::NotRunning) {
            std::fprintf(stderr, "FAIL clipboard holder exited before explicit release\n");
            logHolder("holder exited early");
            return false;
        }
        process_.write("x");
        process_.waitForBytesWritten(5000);
        if (!process_.waitForFinished(10000) ||
            process_.exitStatus() != QProcess::NormalExit || process_.exitCode() != 0) {
            std::fprintf(stderr, "FAIL clipboard holder did not exit cleanly after release\n");
            logHolder("holder exit was abnormal");
            return false;
        }
        return true;
    }

    ~ClipboardLock() { cleanup(); }

private:
    // Child lifecycle evidence only: no clipboard bytes, payloads or secrets are printed.
    void logHolder(const char* reason) {
        const QByteArray errors = process_.readAllStandardError();
        std::fprintf(stderr,
                     "     holder: %s error=%s exitStatus=%d exitCode=%d stderr=%s\n",
                     reason, qPrintable(process_.errorString()),
                     static_cast<int>(process_.exitStatus()),
                     process_.exitCode(),
                     errors.isEmpty() ? "<empty>" : errors.constData());
    }

    void cleanup() {
        if (process_.state() == QProcess::NotRunning) return;
        if (!release()) {
            logHolder("holder required a kill to release");
            process_.kill();
            process_.waitForFinished(5000);
        }
    }

    QProcess process_;
};
#endif

int playerClipboardFailureProbe(const QString& clip) {
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() != QLatin1String("windows")) {
        std::fprintf(stderr, "FAIL player clipboard refusal requires the Windows QPA\n");
        return 2;
    }
    const QString file = QFileInfo(clip).canonicalFilePath();
    if (file.isEmpty() || !QFileInfo(file).isFile()) {
        std::fprintf(stderr, "FAIL local clipboard fixture does not exist\n");
        return 2;
    }
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) return 2;
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("PlayerClipboardFailure"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    QSettings settings;
    settings.setValue(QStringLiteral("player/anime4k"), QStringLiteral("off"));
    if (!mplayer::probes::selectRuntime()) return 2;
    qApp->setStyleSheet(mikai::app::theme::buildStylesheet());

    QPointer<PlayerDialog> player = new PlayerDialog(
        QStringLiteral("Clipboard refusal fixture"), file);
    player->resize(960, 600);
    player->show();
    player->startPlayer();
    QPointer<PlayerShell> shell = player->findChild<PlayerShell*>();
    QPointer<mplayer::VideoSurface> surface =
        player->findChild<mplayer::VideoSurface*>();
    const bool decoded = until([&] {
        return player && surface && surface->hasVideo() && surface->videoWidth() > 0 &&
               surface->videoHeight() > 0 && surface->position() > 0.1;
    }, 20000);
    if (decoded) surface->setPaused(true);
    const bool lit = decoded && until([&] {
        return surface && surface->pauseObserved() && !mplayer::probes::measurePicture(
            surface->grabVideoFramebuffer(), surface->videoWidth(),
            surface->videoHeight()).isMostlyBlack();
    }, 5000);
    check(lit, "hostless player decodes a real lit frame before clipboard refusal");
    QPointer<QLabel> status = player ? player->findChild<QLabel*>(
        QStringLiteral("playerStatusText")) : nullptr;
    if (!lit || shell == nullptr || status == nullptr) {
        if (player) delete player;
        return 1;
    }
    const ClipboardSnapshot before = ClipboardSnapshot::capture();
    check(before.valid, "all MIME and native clipboard formats are snapshotted without seeding");
    if (!before.valid) {
        delete player;
        return 2;
    }
    ClipboardLock lock;
    if (!lock.acquire()) {
        check(false, "separate Win32 process owns the clipboard");
        delete player;
        return 2;
    }
    check(true, "separate Win32 process owns the clipboard");
    check(!mikai::gui::copyText(before.text),
          "locked clipboard rejects a public copy of its own existing text, including empty text");
    check(GetClipboardSequenceNumber() == before.sequence,
          "identical-text refusal leaves the exact native clipboard sequence unchanged");
    if (!before.image.isNull()) {
        check(!mikai::gui::copyImage(before.image),
              "locked clipboard rejects a public copy of its own existing image");
        check(GetClipboardSequenceNumber() == before.sequence,
              "identical-image refusal leaves the exact native clipboard sequence unchanged");
    } else {
        std::fputs("INFO identical-image case unavailable: original clipboard has no image\n", stderr);
    }
    for (const QString& payload : {QStringLiteral("copy"), QStringLiteral("frame")}) {
        if (!player || !shell || !surface || !status) {
            check(false, "player remains alive for both clipboard actions");
            break;
        }
        const QString previous = status->text();
        shell->wake();
        QTest::mouseClick(surface, Qt::RightButton, Qt::NoModifier, QPoint(60, 60));
        const bool menuVisible = until([&] {
            if (!player) return false;
            for (auto* menu : player->findChildren<MenuPopup*>()) {
                if (menu->isVisible()) return true;
            }
            return false;
        }, 3000);
        bool action = false;
        bool feedback = false;
        {
            AccessibleAlertCapture alert(status);
            action = menuVisible && pressMenuRow(player, payload);
            QCoreApplication::processEvents();
            const QString expectedFailure = QCoreApplication::translate(
                "mplayer::shell::PlayerDialog",
                payload == QLatin1String("copy") ? "Failed to copy link" : "Failed to copy frame");
            feedback = statusFeedbackChanged(
                status, previous, QStringLiteral("player-%1-refusal.png").arg(payload),
                alert.observed()) && status && status->text() == expectedFailure;
        }
        check(action, qPrintable(QStringLiteral("visible picture menu invokes %1").arg(payload)));
        check(feedback, qPrintable(QStringLiteral(
            "%1 refusal renders named accessible failure, never success").arg(payload)));
        check(player && surface && player->isVisible() && player->hasActiveSource() &&
              surface->hasVideo(),
              "player remains visible with its decoded source after refusal");
        check(GetClipboardSequenceNumber() == before.sequence,
              "refused action leaves the exact native clipboard sequence unchanged");
    }
    check(lock.release(), "clipboard holder exits cleanly");
    check(before.unchanged(),
          "untouched original retains exact sequence, all native formats, MIME bytes and image pixels");
    check(player && surface && player->isVisible() && surface->hasVideo() &&
          !mplayer::probes::measurePicture(surface->grabVideoFramebuffer(),
              surface->videoWidth(), surface->videoHeight()).isMostlyBlack(),
          "player still renders the real frame after both refused actions");
    if (player) delete player;
    std::fprintf(stderr, "RESULT player clipboard refusal: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
#else
    Q_UNUSED(clip);
    std::fprintf(stderr, "FAIL player clipboard refusal requires Windows OpenClipboard\n");
    return 2;
#endif
}

int searchActionFailureProbe(bool browserFailure) {
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) return 2;
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("SearchActionFailure"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    QJsonObject recent;
    recent.insert(QStringLiteral("ref"), QStringLiteral("QA-action-failure"));
    recent.insert(QStringLiteral("title"), QStringLiteral("QA action failure"));
    QSettings settings;
    settings.setValue(QStringLiteral("home/feeds"), QString());
    settings.setValue(QStringLiteral("home/recentTitles"),
                      QString::fromUtf8(QJsonDocument(QJsonArray{recent}).toJson(
                          QJsonDocument::Compact)));

    mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
    mikai::app::MainWindow main(client);
    main.show();
    QTest::qWait(100);
    auto* page = main.findChild<mikai::app::SearchPage*>();
    auto* card = page != nullptr ? page->findChild<mikai::app::TitleCard*>() : nullptr;
    auto* status = page != nullptr ? page->findChild<QLabel*>(QStringLiteral("status")) : nullptr;
    if (card == nullptr || status == nullptr) {
        std::fprintf(stderr, "FAIL search action probe could not find the recent title card/status\n");
        return 1;
    }

    if (browserFailure) {
        const QString before = status->text();
        bool action = false;
        bool feedback = false;
        {
            AccessibleAlertCapture alert(status);
            action = triggerCardAction(
                card, QCoreApplication::translate("mikai::app::SearchPage", "Open in browser"));
            feedback = statusFeedbackChanged(
                status, before, QStringLiteral("search-browser-refusal.png"), alert.observed());
        }
        const bool ok = action && feedback;
        std::fprintf(stderr, "%s SearchPage browser refusal reached accessible rendered feedback\n",
                     ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }

#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() != QLatin1String("windows")) {
        std::fprintf(stderr, "FAIL clipboard refusal requires the Windows QPA\n");
        return 2;
    }
    const ClipboardSnapshot clipboardBefore = ClipboardSnapshot::capture();
    if (!clipboardBefore.valid) {
        std::fprintf(stderr, "FAIL original clipboard cannot be snapshotted safely\n");
        return 2;
    }
    ClipboardLock lock;
    if (!lock.acquire()) {
        std::fprintf(stderr, "FAIL helper process did not acquire the clipboard\n");
        return 2;
    }
    const QString beforeLink = status->text();
    bool linkAction = false;
    bool linkFeedback = false;
    {
        AccessibleAlertCapture alert(status);
        linkAction = triggerCardAction(
            card, QCoreApplication::translate("mikai::app::SearchPage", "Copy link"));
        linkFeedback = statusFeedbackChanged(
            status, beforeLink, QStringLiteral("search-copy-link-refusal.png"), alert.observed());
    }
    const bool linkSequencePreserved =
        GetClipboardSequenceNumber() == clipboardBefore.sequence;
    const QString beforeId = status->text();
    bool idAction = false;
    bool idFeedback = false;
    {
        AccessibleAlertCapture alert(status);
        idAction = triggerCardAction(
            card, QCoreApplication::translate("mikai::app::SearchPage", "Copy ID"));
        idFeedback = statusFeedbackChanged(
            status, beforeId, QStringLiteral("search-copy-id-refusal.png"), alert.observed());
    }
    const bool idSequencePreserved =
        GetClipboardSequenceNumber() == clipboardBefore.sequence;
    const bool helperClosed = lock.release();
    const bool preserved = clipboardBefore.unchanged();
    const bool ok = helperClosed && preserved && linkSequencePreserved &&
        idSequencePreserved && linkAction && idAction && linkFeedback && idFeedback;
    std::fprintf(stderr,
                 "%s SearchPage Copy link/ID refusal surfaced accessible feedback and preserved clipboard\n",
                 ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
#else
    std::fprintf(stderr, "FAIL refused clipboard path requires Windows OpenClipboard ownership\n");
    return 2;
#endif
}
} // namespace

int main(int argc, char** argv) {
    mikai::app::installCrashLog();
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--hold-clipboard")) {
        return holdClipboardOpen();
    }
    if (argc == 3 &&
        QString::fromLocal8Bit(argv[1]) == QLatin1String("--player-clipboard-failure")) {
        return playerClipboardFailureProbe(QString::fromLocal8Bit(argv[2]));
    }
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--clipboard-failure")) {
        return searchActionFailureProbe(false);
    }
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--browser-failure")) {
        if (QGuiApplication::platformName() != QLatin1String("offscreen")) {
            std::fprintf(stderr, "FAIL browser-refusal probe requires QT_QPA_PLATFORM=offscreen\n");
            return 2;
        }
        return searchActionFailureProbe(true);
    }
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: external_media_probe --player-clipboard-failure <local video> (windows)\n"
                     "   or: external_media_probe --clipboard-failure (windows)\n"
                     "   or: external_media_probe --browser-failure (offscreen)\n"
                     "   or: external_media_probe <320x180 video file> <HTTP video URL>\n"
                     "The URL must be a loopback fixture (127.0.0.1/localhost/::1); the probe\n"
                     "never reaches a real host. HTTP fixture: /retry.mp4 returns 404 once,\n"
                     "then the same video.\n");
        return 2;
    }
    const QFileInfo givenFile(QString::fromLocal8Bit(argv[1]));
    const QString file =
        givenFile.canonicalFilePath().isEmpty() ? givenFile.absoluteFilePath()
                                                : givenFile.canonicalFilePath();
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

    // Both preference scopes must be isolated before constructing a player.
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
    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("home/feeds"), QStringLiteral(""));
    settings.setValue(QStringLiteral("player/anime4k"), QStringLiteral("off"));
    app.setStyleSheet(mikai::app::theme::buildStylesheet());
    ClipboardRestore clipboardRestore;
    if (!clipboardRestore.isValid()) {
        std::fprintf(stderr, "FAIL original clipboard cannot be snapshotted safely\n");
        return 2;
    }
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
    if (!openFile || !openUrl) return clipboardRestore.finish(1);

    answerInput({}, false);
    openUrl->trigger();
    check(activePlayer(&main) == nullptr, "canceling URL input keeps home without a player");
    answerInput(QStringLiteral("   "), true);
    openUrl->trigger();
    check(activePlayer(&main) == nullptr, "blank URL input does not open a player");

    const FileDialogOutcome pick =
        driveFileDialog(file, [&openFile] { openFile->trigger(); });
    // Check the chooser's returned answer, not the path we typed.
    const auto answersWithTarget = [&pick](const QString& candidate) {
        if (candidate.isEmpty()) return false;
        if (candidate == pick.requested) return true;
        const QString canonical = QFileInfo(candidate).canonicalFilePath();
        return !canonical.isEmpty() && canonical == pick.requested;
    };
    const bool retained = answersWithTarget(pick.returned) || answersWithTarget(pick.selected);
    check(pick.sawDialog && pick.attempted && pick.accepted && !pick.forcedClose && retained,
          "the file picker opened, accepted, and returned the requested video");

    // HTTP and retry phases remain independent of a failed local-file handoff.
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
        if (auto* enhance = buttonWithGlyph(local, IconPainter::spark); enhance != nullptr) {
            enhance->click();
        }
        QTest::qWait(200);
        check(enhancementCatalogueVisible(local),
              "standalone media retains the Anime4K enhancement catalogue");
        shell->dismissOverlay();

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
    // Wait for mpv's failure, not just the dialog's creation.
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
    check(clipboardRestore.restore(),
          "ordinary media flow restores original Qt-exposed formats, text and image data");
    std::fprintf(stderr, "RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
