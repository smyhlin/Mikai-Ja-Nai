// Live API title, episode drawer, cover lifetime and hosted refusal probes.

#include <QAccessible>
#include <QAccessibleInterface>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QClipboard>
#include <QImage>
#include <QMenu>
#include <QMap>
#include <QMimeData>
#include <QPointer>
#include <QTest>
#include <QMouseEvent>
#include <QDir>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPoint>
#include <QPushButton>
#include <QRect>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QScopeGuard>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QWindow>

#include <cstdio>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

#include "backend/MikaiClient.hpp"
#include "pages/TitlePage.hpp"
#include "player/quick/PlayerController.hpp"
#include "player/core/VideoSession.hpp"
#include "player/quick/PlayerMenuModel.hpp"
#include "player/quick/PlayerWindow.hpp"
#include "ProbeRuntime.hpp"
#include "gui/UiProfile.hpp"
#include "state/AppearanceStore.hpp"
#include "state/QualityCache.hpp"
#include "support/I18n.hpp"
#include "theme/Metrics.hpp"
#include "theme/Theme.hpp"
#include "workers/Runner.hpp"

using mikai::app::QualityCache;
using mikai::app::Runner;
using mikai::app::TitlePage;
using mikai::backend::MikaiClient;

namespace {

QObject* accessibleAlertTarget = nullptr;
bool accessibleAlertObserved = false;
QAccessible::UpdateHandler previousAccessibleUpdateHandler = nullptr;

void captureAccessibleAlert(QAccessibleEvent* event) {
    if (event != nullptr && event->type() == QAccessible::Alert &&
        (event->object() == accessibleAlertTarget ||
         (accessibleAlertTarget == nullptr &&
          qobject_cast<QMessageBox*>(event->object()) != nullptr))) {
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
bool accessibleContains(QAccessibleInterface* interface, const QString& text, int depth = 0) {
    if (interface == nullptr || depth > 8 || interface->state().invisible) return false;
    if (interface->text(QAccessible::Name).contains(text) ||
        interface->text(QAccessible::Description).contains(text)) {
        return true;
    }
    for (int child = 0; child < interface->childCount(); ++child) {
        if (accessibleContains(interface->child(child), text, depth + 1)) return true;
    }
    return false;
}

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
        if (!OpenClipboard(nullptr)) return snapshot;
        if (!CloseClipboard()) return snapshot;
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

    bool sameContents(const ClipboardSnapshot& after) const {
        return valid && after.valid && after.contents == contents && after.text == text &&
            after.image.convertToFormat(QImage::Format_RGBA8888) ==
                image.convertToFormat(QImage::Format_RGBA8888);
    }

    bool unchanged() const {
        // The sequence covers even native formats Qt cannot decode.
        if (!valid || GetClipboardSequenceNumber() != sequence) return false;
        const ClipboardSnapshot after = capture();
        return after.sequence == sequence && after.nativeFormats == nativeFormats &&
            sameContents(after);
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
            std::fprintf(stderr,
                         "TITLE-PROBE: FAIL clipboard holder did not refuse a competing open\n");
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
                         "TITLE-PROBE: FAIL clipboard holder owner window %p pid=%lu is not the "
                         "spawned holder pid=%lld (competing open denied with %lu)\n",
                         static_cast<void*>(holder), static_cast<unsigned long>(holderPid),
                         static_cast<long long>(process_.processId()), denial);
            logHolder("owner window belongs to another process");
            cleanup();
            return false;
        }
        return true;
    }

    bool release() {
        if (process_.state() == QProcess::NotRunning) {
            std::fprintf(stderr,
                         "TITLE-PROBE: FAIL clipboard holder exited before explicit release\n");
            logHolder("holder exited early");
            return false;
        }
        process_.write("x");
        process_.waitForBytesWritten(5000);
        if (!process_.waitForFinished(10000) ||
            process_.exitStatus() != QProcess::NormalExit || process_.exitCode() != 0) {
            std::fprintf(stderr,
                         "TITLE-PROBE: FAIL clipboard holder did not exit cleanly after release\n");
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
                     "TITLE-PROBE:     holder: %s error=%s exitStatus=%d exitCode=%d stderr=%s\n",
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

bool hostedSessionReady(mplayer::quick::PlayerController* controller,
                        mplayer::quick::PlayerWindow* window) {
    return controller != nullptr && window != nullptr && window->isVisible() &&
           controller->hasActiveSource() && !controller->currentEmbed().isEmpty() &&
           controller->session() != nullptr;
}

bool activateCopyLink(mplayer::quick::PlayerController* controller) {
    if (controller == nullptr || controller->pictureMenu() == nullptr) return false;
    const auto& rows = controller->pictureMenu()->rows();
    for (int row = 0; row < rows.size(); ++row) {
        if (rows.at(row).action != mplayer::quick::PlayerMenuModel::Action::CopyMediaLink) {
            continue;
        }
        if (!rows.at(row).enabled) return false;
        controller->activateMenuRow(QStringLiteral("picture"), row);
        return true;
    }
    return false;
}

bool hostCopyRefusalProbe(TitlePage* page) {
    const QPointer<TitlePage> guardedPage(page);
    QPointer<QPushButton> episode;
    for (int elapsed = 0; elapsed < 30000 && guardedPage; elapsed += 50) {
        auto* tabs = guardedPage->findChild<QTabWidget*>();
        if (tabs != nullptr && tabs->currentWidget() != nullptr) {
            for (auto* button : tabs->currentWidget()->findChildren<QPushButton*>(
                     QStringLiteral("episodeButton"))) {
                if (button->isVisible() && button->isEnabled()) {
                    episode = button;
                    break;
                }
            }
        }
        if (episode) break;
        QTest::qWait(50);
    }
    if (!episode) {
        std::fprintf(stderr, "TITLE-PROBE: FAIL real title7428 has no playable episode\n");
        return false;
    }
    episode->click();
    QPointer<mplayer::quick::PlayerController> controller;
    QPointer<mplayer::quick::PlayerWindow> playerWindow;
    for (int elapsed = 0; elapsed < 60000 && guardedPage; elapsed += 50) {
        for (QWindow* candidateWindow : QGuiApplication::topLevelWindows()) {
            auto* candidate = qobject_cast<mplayer::quick::PlayerWindow*>(candidateWindow);
            if (candidate == nullptr || !candidate->isVisible()) continue;
            auto* candidateController = candidate->controller();
            if (!hostedSessionReady(candidateController, candidate)) continue;
            controller = candidateController;
            playerWindow = candidate;
            break;
        }
        if (controller && playerWindow) break;
        QTest::qWait(50);
    }
    if (!controller || !playerWindow) {
        std::fprintf(stderr,
                     "TITLE-PROBE: FAIL real title7428 opened no hosted Quick player session\n");
        return false;
    }
    if (!controller->hasActiveSource()) {
        std::fprintf(stderr, "TITLE-PROBE: FAIL copy session has no decoded source frame\n");
        return false;
    }
    const bool wasPlaying = controller->playing();
    if (wasPlaying) controller->pauseIfPlaying();
    const auto resumePlayback = qScopeGuard([&controller, wasPlaying] {
        if (wasPlaying && controller) controller->resumePlayback();
    });
    QTest::qWait(100);
    if (!controller || !playerWindow) return false;
    const auto sourceEpoch = controller->session()->sourceEpoch();
    bool action = false;
    bool clipboardPreserved = true;
    bool lockReleased = true;
    QString alert;
    const auto alertConnection =
        QObject::connect(controller, &mplayer::quick::PlayerController::alertRequested,
                         page, [&alert](const QString& text) { alert = text; });
    const auto disconnectAlert = qScopeGuard(
        [alertConnection] { QObject::disconnect(alertConnection); });
    AccessibleAlertCapture alertCapture(playerWindow);
    const QString notificationBefore = controller->notificationText();
    bool notificationFailure = false;
    bool screenshot = false;
#ifdef Q_OS_WIN
    const ClipboardSnapshot clipboardBefore = ClipboardSnapshot::capture();
    if (!clipboardBefore.valid) {
        std::fprintf(stderr, "TITLE-PROBE: FAIL original clipboard snapshot unavailable\n");
        return false;
    }
    ClipboardLock lock;
    if (!lock.acquire()) {
        std::fprintf(stderr, "TITLE-PROBE: FAIL separate Win32 clipboard lock unavailable\n");
        return false;
    }
    action = activateCopyLink(controller);
    QTest::qWait(100);
    notificationFailure = controller && controller->notificationVisible() &&
                          controller->notificationFailure() &&
                          controller->notificationText() != notificationBefore;
    screenshot = playerWindow && playerWindow->grabWindow().save(
                                     QStringLiteral("title-host-copy-refusal.png"));
    const bool sequencePreserved = GetClipboardSequenceNumber() == clipboardBefore.sequence;
    lockReleased = lock.release();
    clipboardPreserved = sequencePreserved && clipboardBefore.unchanged();
#else
    return false;
#endif
    const bool sessionIntact = hostedSessionReady(controller, playerWindow) &&
                               controller->session()->sourceEpoch() == sourceEpoch;
    const bool accessible = !alert.isEmpty() &&
                            accessibleContains(QAccessible::queryAccessibleInterface(playerWindow),
                                               alert);
    const bool alertObserved = alertCapture.observed();
    const bool noFalseSuccess = notificationFailure &&
                                controller->notificationText() != notificationBefore;
    const bool passed = action && !alert.isEmpty() && notificationFailure && accessible &&
                        alertObserved && screenshot && lockReleased && clipboardPreserved &&
                        sessionIntact && noFalseSuccess;
    std::fprintf(stderr,
                 "TITLE-PROBE: hosted copy refusal action=%d visible=%d accessible=%d Alert=%d "
                 "screenshot=%d clipboard-preserved=%d released=%d no-false-success=%d "
                 "session-intact=%d RESULT %s\n",
                 action, notificationFailure, accessible, alertObserved, screenshot,
                 clipboardPreserved, lockReleased, noFalseSuccess, sessionIntact,
                 passed ? "PASS" : "FAIL");
    return passed;
}

bool titleBrowserRefusal(TitlePage* page) {
    QPushButton* episode = nullptr;
    for (int elapsed = 0; elapsed < 30000; elapsed += 50) {
        const auto buttons = page->findChildren<QPushButton*>(QStringLiteral("episodeButton"));
        for (QPushButton* button : buttons) {
            if (button->isEnabled() && button->isVisible()) {
                episode = button;
                break;
            }
        }
        if (episode != nullptr) break;
        QTest::qWait(50);
    }
    QLabel* status = page->findChild<QLabel*>(QStringLiteral("status"));
    if (episode == nullptr || status == nullptr) return false;

    const QString before = status->text();
    AccessibleAlertCapture alert(status);
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
                if (action->text() != QCoreApplication::translate("mikai::app::TitlePage", "Open in browser")) {
                    continue;
                }
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
    QContextMenuEvent event(QContextMenuEvent::Mouse, local, episode->mapToGlobal(local));
    QApplication::sendEvent(episode, &event);
    poller.stop();

    QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(status);
    const bool feedback = status->isVisible() && !status->visibleRegion().isEmpty() &&
                          status->text() != before &&
                          status->text() == QCoreApplication::translate(
                              "mikai::app::TitlePage", "Failed to open in browser") &&
                          accessible != nullptr && !accessible->state().invisible &&
                          accessible->text(QAccessible::Name) == status->text();
    const bool captured = page->grab().save(QStringLiteral("title-browser-refusal.png"));
    std::fprintf(stderr,
                 "TITLE-PROBE: browser refusal feedback=%d accessible=%d alert=%d screenshot=%d text=%s\n",
                 feedback ? 1 : 0, accessible != nullptr ? 1 : 0,
                 alert.observed() ? 1 : 0, captured ? 1 : 0,
                 status->text().toUtf8().constData());
    return triggered && feedback && alert.observed() && captured;
}

int runScreenshotReview(QApplication& app, TitlePage* page, const QString& ref,
                        const QString& outputDirectory) {
    struct ReviewCase {
        const char* name;
        QSize size;
        const char* language;
        mikai::app::theme::SiteTheme theme;
    };
    const ReviewCase cases[] = {
        {"minimum-uk-dark", QSize(860, 620), "uk", mikai::app::theme::SiteTheme::Dark},
        {"review-1000-uk-dark", QSize(1000, 700), "uk", mikai::app::theme::SiteTheme::Dark},
        {"review-1280-uk-dark", QSize(1280, 820), "uk", mikai::app::theme::SiteTheme::Dark},
        {"wide-en-dark", QSize(1080, 760), "en", mikai::app::theme::SiteTheme::Dark},
        {"large-uk-dark", QSize(1400, 900), "uk", mikai::app::theme::SiteTheme::Dark},
        {"large-en-light", QSize(1400, 900), "en", mikai::app::theme::SiteTheme::Light},
    };
    const QString platform = QGuiApplication::platformName();
    const QString scale = qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1"));
    const QString prefix = QStringLiteral("%1-scale-%2").arg(platform, scale);
    QJsonObject result;
    result.insert(QStringLiteral("platform"), platform);
    result.insert(QStringLiteral("scale"), scale);
    result.insert(QStringLiteral("devicePixelRatio"), page->devicePixelRatioF());
    result.insert(QStringLiteral("ref"), ref);
    QJsonArray screenshots;
    QJsonObject interactions;
    auto saveView = [&](const ReviewCase& reviewCase, const QString& view,
                        QWidget* focusTarget = nullptr) {
        if (focusTarget != nullptr) focusTarget->setFocus(Qt::OtherFocusReason);
        QCoreApplication::processEvents();
        QTest::qWait(120);
        const QString fileName = QStringLiteral("%1-%2-%3.png")
                                     .arg(prefix, QString::fromLatin1(reviewCase.name), view);
        const QString path = QDir(outputDirectory).filePath(fileName);
        if (!page->grab().save(path)) {
            std::fprintf(stderr, "TITLE-REVIEW: failed to save %s\n", qPrintable(path));
            return false;
        }
        screenshots.append(path);
        return true;
    };
    auto settle = [] {
        QTest::qWait(300);
        QCoreApplication::processEvents();
    };
    bool synopsisExpanded = false;

    // Measured geometry of the surfaces GOAL-C aligns, per review case and per
    // view: logical px at the run's own device ratio, from the live fixture.
    QJsonArray geometryCases;
    const auto measure = [&](const ReviewCase& reviewCase, const char* view) {
        const auto describe = [](QWidget* widget) {
            QJsonObject entry;
            if (widget == nullptr) return entry;
            entry.insert(QStringLiteral("visible"), widget->isVisible());
            entry.insert(QStringLiteral("width"), widget->width());
            entry.insert(QStringLiteral("height"), widget->height());
            return entry;
        };
        QJsonObject entry;
        entry.insert(QStringLiteral("case"), QString::fromLatin1(reviewCase.name));
        entry.insert(QStringLiteral("view"), QString::fromLatin1(view));
        entry.insert(QStringLiteral("devicePixelRatio"), page->devicePixelRatioF());
        entry.insert(QStringLiteral("viewport"), QStringLiteral("%1x%2")
                                                     .arg(reviewCase.size.width())
                                                     .arg(reviewCase.size.height()));
        const auto add = [&](const QString& id, const char* name) {
            entry.insert(id, describe(page->findChild<QWidget*>(QString::fromLatin1(name))));
        };
        add(QStringLiteral("header"), "titleHeader");
        add(QStringLiteral("back"), "titleBack");
        add(QStringLiteral("home"), "titleHome");
        add(QStringLiteral("settingsChip"), "settingsChip");
        add(QStringLiteral("heroWatch"), "titleHeroWatchButton");
        add(QStringLiteral("detailsToggle"), "titleDetailsToggle");
        add(QStringLiteral("watch"), "titleWatchButton");
        add(QStringLiteral("watchedToggle"), "titleWatchWatchedToggle");
        QJsonArray scoreLines;
        for (QLabel* label : page->findChildren<QLabel*>(QStringLiteral("titleScores"))) {
            QJsonObject line = describe(label);
            line.insert(QStringLiteral("text"), label->text());
            line.insert(QStringLiteral("wrappedHeight"), label->heightForWidth(label->width()));
            QWidget* surface = label->parentWidget();
            const QRect bounds(label->mapTo(surface, QPoint(0, 0)), label->size());
            line.insert(QStringLiteral("insideSurface"), surface->rect().contains(bounds));
            scoreLines.append(line);
        }
        entry.insert(QStringLiteral("scoreLines"), scoreLines);
        geometryCases.append(entry);
    };

    auto* heading = page->findChild<QLabel*>(QStringLiteral("titleHeading"));
    auto* tabs = page->findChild<QTabWidget*>(QStringLiteral("episodeTabs"));
    auto* overview = page->findChild<QScrollArea*>(QStringLiteral("titleOverview"));
    auto* discovery = page->findChild<QWidget*>(QStringLiteral("titleDiscoverySection"));
    auto* related = page->findChild<QWidget*>(QStringLiteral("titleRelated"));
    auto* discoveryToggle =
        page->findChild<QPushButton*>(QStringLiteral("titleDiscoveryToggle"));
    if (heading == nullptr || tabs == nullptr || overview == nullptr || discovery == nullptr) {
        std::fprintf(stderr, "TITLE-REVIEW: page is missing heading, tabs, scroll or discovery\n");
        return 1;
    }

    for (const ReviewCase& reviewCase : cases) {
        if (!mikai::app::applyInterfaceLanguage(
                QString::fromLatin1(reviewCase.language),
                qEnvironmentVariable("MIKAI_PROBE_TRANSLATION"))) {
            std::fprintf(stderr, "TITLE-REVIEW: unable to install %s interface language\n",
                         reviewCase.language);
            return 1;
        }
        auto appearance = mikai::app::currentAppearance();
        appearance.siteTheme = reviewCase.theme;
        appearance.accent = QStringLiteral("#6797e4");
        appearance.radiusStep = 1;
        appearance.backgroundGradient = true;
        appearance.gradientIntensity = 30;
        mikai::app::setCurrentAppearance(appearance);
        mikai::app::theme::applyPalette(appearance);
        app.setStyleSheet(mikai::app::theme::buildStylesheet(appearance));
        page->resize(reviewCase.size);
        page->show();
        settle();

        QElapsedTimer loadTimer;
        loadTimer.start();
        while (loadTimer.elapsed() < 30000) {
            QCoreApplication::processEvents();
            const bool hasPills =
                !page->findChildren<QPushButton*>(QStringLiteral("episodeButton")).isEmpty();
            const bool hasTabs = tabs->count() > 0 && tabs->currentWidget() != nullptr;
            if (!heading->text().isEmpty() && hasTabs && hasPills) break;
            QTest::qWait(50);
        }
        const bool loaded = !heading->text().isEmpty() && tabs->count() > 0 &&
                            tabs->currentWidget() != nullptr &&
                            !page->findChildren<QPushButton*>(
                                 QStringLiteral("episodeButton")).isEmpty();
        if (!loaded) {
            auto* status = page->findChild<QLabel*>(QStringLiteral("titleEpisodeStatus"));
            std::fprintf(stderr,
                         "TITLE-REVIEW: live load failed for %s; heading=%s status=%s tabs=%d\n",
                         qPrintable(ref), qPrintable(heading->text()),
                         status == nullptr ? "<missing>" : qPrintable(status->text()), tabs->count());
            result.insert(QStringLiteral("diagnostic"),
                          status == nullptr ? QStringLiteral("No episode status available")
                                            : status->text());
            return 1;
        }
        settle();
        if (!synopsisExpanded) {
            auto* synopsisToggle =
                page->findChild<QPushButton*>(QStringLiteral("titleSynopsisToggle"));
            if (synopsisToggle != nullptr && synopsisToggle->isVisible()) {
                synopsisToggle->click();
                settle();
                synopsisExpanded = true;
            }
        }
        result.insert(QStringLiteral("viewport_%1").arg(QString::fromLatin1(reviewCase.name)),
                      QStringLiteral("%1x%2").arg(reviewCase.size.width()).arg(reviewCase.size.height()));
        result.insert(QStringLiteral("language_%1").arg(QString::fromLatin1(reviewCase.name)),
                      QString::fromLatin1(reviewCase.language));
        result.insert(QStringLiteral("theme_%1").arg(QString::fromLatin1(reviewCase.name)),
                      reviewCase.theme == mikai::app::theme::SiteTheme::Dark
                          ? QStringLiteral("dark") : QStringLiteral("light"));

        QWidget* hero = page->findChild<QWidget*>(QStringLiteral("titleHero"));
        if (hero != nullptr) overview->ensureWidgetVisible(hero, 0, 0);
        if (!saveView(reviewCase, QStringLiteral("hero"))) return 1;
        measure(reviewCase, "hero");

        // Exercise live accent, gradient and corner updates without rebuilding
        // the page or refetching its title data; restore the case appearance.
        const auto baseAppearance = mikai::app::currentAppearance();
        QJsonArray appearanceChanges;
        for (const QString& accent : {QStringLiteral("#a78bfa"), QStringLiteral("#f5c542")}) {
            auto changed = baseAppearance;
            changed.accent = accent;
            mikai::app::setCurrentAppearance(changed);
            mikai::app::theme::applyPalette(changed);
            app.setStyleSheet(mikai::app::theme::buildStylesheet(changed));
            settle();
            appearanceChanges.append(accent);
        }
        auto changed = baseAppearance;
        changed.backgroundGradient = false;
        mikai::app::setCurrentAppearance(changed);
        mikai::app::theme::applyPalette(changed);
        app.setStyleSheet(mikai::app::theme::buildStylesheet(changed));
        settle();
        appearanceChanges.append(QStringLiteral("gradient-off"));
        changed.backgroundGradient = true;
        changed.gradientIntensity = 0;
        mikai::app::setCurrentAppearance(changed);
        mikai::app::theme::applyPalette(changed);
        app.setStyleSheet(mikai::app::theme::buildStylesheet(changed));
        settle();
        appearanceChanges.append(QStringLiteral("gradient-zero"));
        changed = baseAppearance;
        changed.radiusStep = 0;
        changed.coverGradient = false;
        mikai::app::setCurrentAppearance(changed);
        mikai::app::theme::applyPalette(changed);
        app.setStyleSheet(mikai::app::theme::buildStylesheet(changed));
        settle();
        changed.radiusStep = mikai::app::theme::radiusStepCount() - 1;
        changed.coverGradient = true;
        mikai::app::setCurrentAppearance(changed);
        mikai::app::theme::applyPalette(changed);
        app.setStyleSheet(mikai::app::theme::buildStylesheet(changed));
        settle();
        mikai::app::setCurrentAppearance(baseAppearance);
        mikai::app::theme::applyPalette(baseAppearance);
        app.setStyleSheet(mikai::app::theme::buildStylesheet(baseAppearance));
        interactions.insert(QString::fromLatin1(reviewCase.name) + QStringLiteral("-appearance"),
                            appearanceChanges);

        auto* episodeHeading = page->findChild<QLabel*>(
            QStringLiteral("episodeSectionHeading"));
        auto* detailsToggle =
            page->findChild<QPushButton*>(QStringLiteral("titleDetailsToggle"));
        if (episodeHeading != nullptr) overview->ensureWidgetVisible(episodeHeading, 0, 0);
        if (!saveView(reviewCase, QStringLiteral("episodes"))) return 1;
        overview->ensureWidgetVisible(discovery, 0, 0);
        if (!saveView(reviewCase, QStringLiteral("discovery"))) return 1;

        bool disclosurePassed = discoveryToggle == nullptr || related == nullptr;
        if (discoveryToggle != nullptr && related != nullptr) {
            discoveryToggle->click();
            settle();
            const bool collapsed = !related->isVisible() && discoveryToggle->isVisible();
            discoveryToggle->click();
            settle();
            disclosurePassed = collapsed && related->isVisible();
        }
        bool detailsPassed = detailsToggle != nullptr;
        if (detailsToggle != nullptr) {
            detailsToggle->click();
            settle();
            auto* overlay = page->findChild<QWidget*>(QStringLiteral("titleDetailsOverlay"));
            detailsPassed = overlay != nullptr && overlay->isVisible();
            if (detailsPassed && !saveView(reviewCase, QStringLiteral("details-open"))) return 1;
            if (detailsPassed) measure(reviewCase, "details-open");
            auto* close = page->findChild<QPushButton*>(QStringLiteral("titleDetailsClose"));
            if (close != nullptr) close->click();
            settle();
            detailsPassed = detailsPassed && overlay != nullptr && !overlay->isVisible();
            detailsToggle->click();
            settle();
            QTest::keyClick(page, Qt::Key_Escape);
            settle();
            detailsPassed = detailsPassed && !overlay->isVisible();
            detailsToggle->click();
            settle();
            auto* scrim = page->findChild<QPushButton*>(QStringLiteral("titleDetailsScrim"));
            if (scrim != nullptr) scrim->click();
            settle();
            detailsPassed = detailsPassed && !overlay->isVisible();
        }
        if (detailsToggle != nullptr) detailsToggle->click();
        settle();
        auto* jumpToEpisodes =
            page->findChild<QPushButton*>(QStringLiteral("titleDetailsAction"));
        if (jumpToEpisodes != nullptr) jumpToEpisodes->click();
        settle();
        auto* focused = QApplication::focusWidget();
        const bool focusPassed = focused != nullptr &&
                                 focused->objectName() == QLatin1String("episodeButton");
        if (!saveView(reviewCase, QStringLiteral("keyboard-focus"), focused)) return 1;
        interactions.insert(QString::fromLatin1(reviewCase.name) + QStringLiteral("-discovery-toggle"),
                            disclosurePassed);
        interactions.insert(QString::fromLatin1(reviewCase.name) + QStringLiteral("-details"),
                            detailsPassed);
        interactions.insert(QString::fromLatin1(reviewCase.name) + QStringLiteral("-keyboard-focus"),
                            focusPassed);
    }

    result.insert(QStringLiteral("screenshots"), screenshots);
    result.insert(QStringLiteral("interactions"), interactions);
    result.insert(QStringLiteral("geometry"), geometryCases);
    const QString resultPath = QDir(outputDirectory).filePath(
        QStringLiteral("%1-%2-result.json").arg(prefix, ref));
    QFile file(resultPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "TITLE-REVIEW: cannot write result JSON %s\n", qPrintable(resultPath));
        return 1;
    }
    file.write(QJsonDocument(result).toJson(QJsonDocument::Indented));
    file.close();
    std::fprintf(stderr, "TITLE-REVIEW: saved %d screenshots and %s\n",
                 static_cast<int>(screenshots.size()), qPrintable(resultPath));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setStyle(QStringLiteral("Fusion"));
    app.setStyleSheet(mikai::app::theme::buildStylesheet());
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--hold-clipboard")) {
        return holdClipboardOpen();
    }
    const bool layoutOnly = app.arguments().contains(QStringLiteral("--layout-only"));
    const bool disposeEarly = app.arguments().contains(QStringLiteral("--dispose-early"));
    const bool browserFailureOnly =
        app.arguments().contains(QStringLiteral("--browser-refusal-only"));
    const bool hostCopyFailureOnly =
        app.arguments().contains(QStringLiteral("--host-copy-refusal-only"));
    QString reviewOutput;
    QString reviewRef;
    bool reviewMode = false;
    for (int i = 1; i < app.arguments().size(); ++i) {
        if (app.arguments().at(i) == QLatin1String("--review-screenshots")) {
            if (reviewMode || i + 1 >= app.arguments().size()) {
                std::fprintf(stderr,
                             "TITLE-REVIEW: --review-screenshots requires an absolute output directory\n");
                return 2;
            }
            reviewMode = true;
            reviewOutput = app.arguments().at(++i);
        } else if (reviewMode && !app.arguments().at(i).startsWith(QLatin1Char('-')) &&
                   reviewRef.isEmpty()) {
            reviewRef = app.arguments().at(i);
        }
    }
    for (const QString& argument : app.arguments().mid(1)) {
        if (argument.startsWith(QLatin1Char('-')) &&
            argument != QLatin1String("--layout-only") &&
            argument != QLatin1String("--dispose-early") &&
            argument != QLatin1String("--browser-refusal-only") &&
            argument != QLatin1String("--host-copy-refusal-only") &&
            argument != QLatin1String("--review-screenshots")) {
            std::fprintf(stderr, "TITLE-PROBE: unknown option %s\n", qPrintable(argument));
            return 2;
        }
    }
    if (int(layoutOnly) + int(disposeEarly) + int(browserFailureOnly) +
        int(hostCopyFailureOnly) + int(reviewMode) > 1) {
        std::fprintf(stderr,
                     "TITLE-PROBE: choose one probe mode; --review-screenshots takes an "
                     "absolute output directory and optional title ref\n");
        return 2;
    }
    if (browserFailureOnly &&
        QGuiApplication::platformName() != QLatin1String("offscreen")) {
        std::fprintf(stderr, "TITLE-PROBE: browser refusal requires QT_QPA_PLATFORM=offscreen\n");
        return 2;
    }
    if (hostCopyFailureOnly) {
#ifndef Q_OS_WIN
        std::fprintf(stderr, "TITLE-PROBE: clipboard refusal requires Windows OpenClipboard\n");
        return 2;
#else
        if (QGuiApplication::platformName() != QLatin1String("windows")) {
            std::fprintf(stderr, "TITLE-PROBE: clipboard refusal requires the Windows QPA\n");
            return 2;
        }
#endif
    }
    if (reviewMode) {
        if (!QFileInfo(reviewOutput).isAbsolute()) {
            std::fprintf(stderr, "TITLE-REVIEW: output directory must be absolute: %s\n",
                         qPrintable(reviewOutput));
            return 2;
        }
        if (!QDir().mkpath(reviewOutput)) {
            std::fprintf(stderr, "TITLE-REVIEW: cannot create output directory: %s\n",
                         qPrintable(reviewOutput));
            return 2;
        }
    }
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) return 2;
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("TitlePage"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    QSettings().setValue(QStringLiteral("player/anime4k"), QStringLiteral("off"));
    if (!layoutOnly && !disposeEarly && !browserFailureOnly && !reviewMode &&
        !mplayer::probes::selectRuntime()) {
        return 2;
    }

    QString ref = reviewRef.isEmpty() ? QStringLiteral("7428") : reviewRef;
    if (!reviewMode) {
        for (const QString& argument : app.arguments().mid(1)) {
            if (!argument.startsWith(QLatin1Char('-'))) {
                ref = argument;
                break;
            }
        }
    }

    if (hostCopyFailureOnly && ref != QLatin1String("7428")) {
        std::fprintf(stderr, "TITLE-PROBE: hosted refusal scenarios require real title7428\n");
        return 2;
    }

    MikaiClient client(qEnvironmentVariable("MIKAI_API_KEY"));
    Runner runner;
    const auto drainBeforeClientDestruction = qScopeGuard([&runner] {
        runner.cancel();
        if (!runner.waitForDone(30000)) qFatal("Title probe workers did not stop");
    });
    QualityCache cache;
    auto* page = new TitlePage(runner, client, cache, ref);
    if (disposeEarly) {
        delete page;
        if (!runner.waitForDone(60000)) {
            std::fprintf(stderr, "TITLE-PROBE: early-disposal workers did not finish\n");
            return 1;
        }
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        std::fprintf(stderr, "TITLE-PROBE: early-disposal deliveries completed without crash\n");
        return 0;
    }
    if (browserFailureOnly) {
        page->resize(QSize(860, 620));
        page->show();
        const bool passed = titleBrowserRefusal(page);
        delete page;
        std::fprintf(stderr, "TITLE-PROBE: browser refusal %s\n", passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }
    page->resize(layoutOnly ? QSize(860, 620) : QSize(1400, 900));
    page->show();

    if (hostCopyFailureOnly) {
        const bool passed = hostCopyRefusalProbe(page);
        delete page;
        return passed ? 0 : 1;
    }
    if (reviewMode) {
        const int result = runScreenshotReview(app, page, ref, reviewOutput);
        delete page;
        return result;
    }

    // Stage 1: what did the tabs actually receive?
    QTimer::singleShot(9000, [page, ref, layoutOnly] {
        auto* tabs = page->findChild<QTabWidget*>();
        if (tabs == nullptr || tabs->count() == 0) {
            std::fprintf(stderr, "TITLE-PROBE: %s produced no release tabs\n", qPrintable(ref));
            if (layoutOnly) {
                QCoreApplication::exit(1);
            }
            return;
        }
        std::fprintf(stderr, "TITLE-PROBE: %s -> %d provider tabs\n", qPrintable(ref), tabs->count());
        for (int i = 0; i < tabs->count(); ++i) {
            QWidget* tabPage = tabs->widget(i);
            const int pills = tabPage->findChildren<QPushButton*>(QStringLiteral("episodeButton")).size();
            std::fprintf(stderr, "TITLE-PROBE:   tab %d %-12s pills=%d\n", i,
                         qPrintable(tabs->tabText(i)), pills);
        }

        QWidget* first = tabs->widget(0);
        // The episode grid now expands inside the page's one body scroll: there
        // is no inner episode viewport. Measure the pills where they land and
        // prove the body column still fits its scroll viewport.
        const auto pills = first->findChildren<QPushButton*>(QStringLiteral("episodeButton"));
        auto* overview = page->findChild<QScrollArea*>(QStringLiteral("titleOverview"));
        auto* body = page->findChild<QWidget*>(QStringLiteral("titleBody"));
        bool layoutPass = overview != nullptr && body != nullptr && !pills.isEmpty();
        if (layoutPass) {
            const int viewportWidth = overview->viewport()->width();
            layoutPass = body->minimumSizeHint().width() <= viewportWidth;
            int lastPillBottom = 0;
            for (QPushButton* button : pills) {
                const int right = button->mapTo(body, QPoint(button->width(), 0)).x();
                layoutPass = layoutPass && right <= body->width() + 1;
                lastPillBottom = qMax(lastPillBottom,
                                      button->mapTo(first, QPoint(0, button->height())).y());
            }
            layoutPass = layoutPass && lastPillBottom <= first->height() + 1;
            std::fprintf(stderr,
                         "TITLE-PROBE: body=%dx%d viewport=%dpx pills=%d last-row=%dpx/%dpx %s\n",
                         body->width(), body->height(), viewportWidth,
                         static_cast<int>(pills.size()), lastPillBottom, first->height(),
                         layoutPass ? "PASS" : "FAIL");
        } else {
            std::fprintf(stderr,
                         "TITLE-PROBE: episode layout FAIL (missing body scroll, body or pills)\n");
        }
        if (layoutOnly) {
            auto* episodesHeading = page->findChild<QLabel*>(
                QStringLiteral("episodeSectionHeading"));
            auto* releaseChipsHost = page->findChild<QWidget*>(
                QStringLiteral("releaseChipsHost"));
            const auto releaseChips = page->findChildren<QPushButton*>(
                QStringLiteral("releaseChip"));
            bool selectorPass = !releaseChips.isEmpty();
            int checkedCount = 0;
            for (QPushButton* chip : releaseChips) {
                checkedCount += chip->isChecked() ? 1 : 0;
            }
            selectorPass = selectorPass && checkedCount == 1;
            if (episodesHeading != nullptr && releaseChipsHost != nullptr) {
                // The selector lives inside the body scroll now, so bring it on
                // screen before grabbing its section instead of capturing
                // whatever happens to be scrolled into the viewport.
                auto* bodyScroll = page->findChild<QScrollArea*>(QStringLiteral("titleOverview"));
                if (bodyScroll != nullptr) {
                    bodyScroll->ensureWidgetVisible(releaseChipsHost, 0, 0);
                    QCoreApplication::processEvents();
                }
                const QRect headingRect(episodesHeading->mapTo(page, QPoint(0, 0)),
                                        episodesHeading->size());
                const QRect chipsRect(releaseChipsHost->mapTo(page, QPoint(0, 0)),
                                      releaseChipsHost->size());
                const QRect selectorRect =
                    headingRect.united(chipsRect)
                        .adjusted(-mikai::app::theme::kSpacingXs,
                                  -mikai::app::theme::kSpacingXs,
                                  mikai::app::theme::kSpacingXs,
                                  mikai::app::theme::kSpacingXs);
                const QString screenshotPath =
                    QDir(QDir::tempPath()).filePath(QStringLiteral("mikai-release-selector.png"));
                if (page->grab(selectorRect).save(screenshotPath)) {
                    std::fprintf(stderr, "TITLE-PROBE: selector section saved to %s\n",
                                 qPrintable(screenshotPath));
                }
            }
            QPushButton* nextRelease = nullptr;
            for (QPushButton* chip : releaseChips) {
                if (!chip->isChecked()) {
                    nextRelease = chip;
                    break;
                }
            }
            if (nextRelease != nullptr) {
                nextRelease->click();
                QCoreApplication::processEvents();
                checkedCount = 0;
                for (QPushButton* chip : releaseChips) {
                    checkedCount += chip->isChecked() ? 1 : 0;
                }
                selectorPass = selectorPass && nextRelease->isChecked() && checkedCount == 1;
            }
            std::fprintf(stderr, "TITLE-PROBE: release chips=%d exclusive-selection=%s\n",
                         static_cast<int>(releaseChips.size()),
                         selectorPass ? "PASS" : "FAIL");
            layoutPass = layoutPass && selectorPass;

            // GOAL-E repolish criterion: bounded work per breakpoint crossing,
            // proven by the counter and by the consumer-visible heading font.
            mikai::gui::uiprofile::forceCounters(true);
            mikai::gui::uiprofile::resetCounters();
            const QSize startedAt = page->size();
            page->resize(1400, 900);
            QCoreApplication::processEvents();
            auto* heading = page->findChild<QLabel*>(QStringLiteral("titleHeading"));
            const int fontBefore = heading != nullptr ? heading->font().pixelSize() : -1;
            const qint64 flatBefore = mikai::gui::uiprofile::snapshot().titleRepolishWidgets;
            page->resize(1300, 900);
            QCoreApplication::processEvents();
            const qint64 flat =
                mikai::gui::uiprofile::snapshot().titleRepolishWidgets - flatBefore;
            const qint64 compactBefore = mikai::gui::uiprofile::snapshot().titleRepolishWidgets;
            page->resize(860, 900);
            QCoreApplication::processEvents();
            const qint64 compact =
                mikai::gui::uiprofile::snapshot().titleRepolishWidgets - compactBefore;
            const qint64 narrowBefore = mikai::gui::uiprofile::snapshot().titleRepolishWidgets;
            page->resize(620, 900);
            QCoreApplication::processEvents();
            const qint64 narrow =
                mikai::gui::uiprofile::snapshot().titleRepolishWidgets - narrowBefore;
            const int fontAfter = heading != nullptr ? heading->font().pixelSize() : -1;
            page->resize(startedAt);
            QCoreApplication::processEvents();
            mikai::gui::uiprofile::forceCounters(false);
            std::fprintf(stderr,
                         "TITLE-PROBE: repolish widgets flat=%lld compact=%lld narrow=%lld "
                         "headingPx=%d->%d at %dx%d\n",
                         static_cast<long long>(flat), static_cast<long long>(compact),
                         static_cast<long long>(narrow), fontBefore, fontAfter,
                         startedAt.width(), startedAt.height());
            const bool repolishPass = flat == 0 && compact >= 1 && compact <= 20 &&
                                      narrow >= 1 && narrow <= 20 &&
                                      (heading == nullptr || fontAfter != fontBefore);
            if (!repolishPass) {
                std::fprintf(stderr, "TITLE-PROBE: repolish criterion FAIL\n");
            }
            layoutPass = layoutPass && repolishPass;
            QCoreApplication::exit(layoutPass ? 0 : 1);
            return;
        }

        auto* pill = pills.isEmpty() ? nullptr : pills.constFirst();
        if (pill == nullptr) {
            std::fprintf(stderr, "TITLE-PROBE: tab 0 has no pill to click\n");
            return;
        }
        std::fprintf(stderr, "TITLE-PROBE: clicking a pill on %s\n",
                     qPrintable(tabs->tabText(0)));
        pill->click();
    });

    // Stage 3: the native Quick player and its controller-owned episode drawer.
    QTimer::singleShot(20000, [&] {
        QPointer<mplayer::quick::PlayerWindow> player;
        for (QWindow* candidate : QGuiApplication::topLevelWindows()) {
            if (auto* quickPlayer = qobject_cast<mplayer::quick::PlayerWindow*>(candidate);
                quickPlayer != nullptr && quickPlayer->isVisible() &&
                quickPlayer->controller() != nullptr) {
                player = quickPlayer;
                break;
            }
        }
        if (!player) {
            std::fprintf(stderr, "TITLE-PROBE: NO Quick player opened after clicking a pill\n");
            std::fprintf(stderr, "TITLE-PROBE: RESULT INCONCLUSIVE\n");
            return;
        }
        QPointer<mplayer::quick::PlayerController> controller = player->controller();
        QPointer<mplayer::VideoSession> session =
            controller != nullptr ? controller->session() : nullptr;
        const quint64 mediaEpoch = session != nullptr ? session->sourceEpoch() : 0;
        const bool hasPlaylist = controller != nullptr && controller->seriesMenu() != nullptr &&
                                 controller->seriesMenu()->rowCount() > 0;
        std::fprintf(stderr, "TITLE-PROBE: Quick player exists; episode model rows=%d\n",
                     controller->seriesMenu() != nullptr
                         ? controller->seriesMenu()->rowCount()
                         : 0);
        if (hasPlaylist) {
            controller->toggleSeries();
            QCoreApplication::processEvents();
            const bool opened = controller->seriesExpanded();
            // The controller's interaction hold keeps an open drawer alive
            // beyond the fullscreen chrome's 2.5s idle dwell.
            QTest::qWait(3500);
            QCoreApplication::processEvents();
            const bool sourceStable =
                controller && session && mediaEpoch != 0 && controller->session() == session &&
                session->sourceEpoch() == mediaEpoch;
            const int rows =
                controller && controller->seriesMenu() != nullptr
                    ? controller->seriesMenu()->rowCount()
                    : 0;
            const bool remainedOpen =
                sourceStable && player && player->isVisible() && controller->seriesExpanded() &&
                rows > 0;
            std::fprintf(stderr,
                         "TITLE-PROBE: episode drawer opened=%d after-3.5s=%d rows=%d "
                         "source-stable=%d\n",
                         opened, remainedOpen, rows, sourceStable);
            if (!opened || !remainedOpen) {
                std::fprintf(stderr, "TITLE-PROBE: RESULT FAIL\n");
                return;
            }
        } else {
            std::fprintf(stderr, "TITLE-PROBE: hosted playlist is empty\n");
            std::fprintf(stderr, "TITLE-PROBE: RESULT FAIL\n");
            return;
        }
        std::fprintf(stderr, "TITLE-PROBE: RESULT OK\n");
    });

    // Repeated cover opens must not reuse a WA_DeleteOnClose pointer.
    QTimer::singleShot(26000, [page] {
        auto* cover = page->findChild<QWidget*>(QStringLiteral("titleCover"));
        if (cover == nullptr) {
            std::fprintf(stderr, "COVER-PROBE: no titleCover label found\n");
            return;
        }
        if (!cover->isEnabled()) {
            std::fprintf(stderr, "COVER-PROBE: cover is disabled, skipping\n");
            return;
        }
        int opened = 0;
        for (int round = 0; round < 4; ++round) {
            const QPointF at(4, 4);
            QMouseEvent press(QEvent::MouseButtonPress, at, cover->mapToGlobal(at),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(cover, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, at, cover->mapToGlobal(at),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(cover, &release);
            QApplication::processEvents();

            QWidget* viewer = nullptr;
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (QString::fromLatin1(w->metaObject()->className())
                        .contains(QStringLiteral("CoverViewer"))) {
                    viewer = w;
                    break;
                }
            }
            if (viewer == nullptr) {
                std::fprintf(stderr, "COVER-PROBE: round %d did not open a viewer\n", round);
                continue;
            }
            ++opened;
            std::fprintf(stderr, "COVER-PROBE: round %d opened the viewer\n", round);
            viewer->close();
            QApplication::processEvents();
        }
        std::fprintf(stderr, "COVER-PROBE: %d/4 rounds opened; reaching here is the fix\n",
                     opened);
        std::fprintf(stderr, "COVER-PROBE: RESULT OK\n");
    });

    return app.exec();
}
