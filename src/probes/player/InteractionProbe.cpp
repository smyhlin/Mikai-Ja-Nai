// Native Qt Quick interaction probe. All player input is delivered to the real
// PlayerWindow scene; playback and picture evidence come from its one session.

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QSettings>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTranslator>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#include "PlayerController.hpp"
#include "PlayerMenuModel.hpp"
#include "PlayerWindow.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSession.hpp"
#include "DelayedHttpSource.hpp"

using namespace mplayer;
using namespace mplayer::quick;

namespace {

int checks = 0;
int failures = 0;
QString outputDir;

void check(bool ok, const QString& description) {
    ++checks;
    std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", qPrintable(description));
    if (!ok) ++failures;
}

QString artifactPath(const QString& name) {
    return outputDir.isEmpty() ? name : QDir(outputDir).filePath(name);
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs,
             const std::function<void()>& sample = {}) {
    QElapsedTimer timer;
    timer.start();
    qint64 nextSample = 0;
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate()) return true;
        if (sample && timer.elapsed() >= nextSample) {
            sample();
            nextSample = timer.elapsed() + 100;
        }
        QThread::msleep(10);
    }
    return predicate();
}

QQuickItem* visibleMenu(QQuickItem* root, const QString& id) {
    if (!root) return nullptr;
    for (QQuickItem* item : root->findChildren<QQuickItem*>()) {
        if (item->property("menuId").toString() == id &&
            item->property("menuModel").isValid() && item->isVisible()) return item;
    }
    return nullptr;
}

QQuickItem* menuRow(QQuickItem* panel, const QString& value) {
    if (!panel) return nullptr;
    for (QQuickItem* item : panel->findChildren<QQuickItem*>()) {
        if (item->property("rowValue").toString() == value && item->isVisible()) return item;
    }
    return nullptr;
}
QQuickItem* itemWithProperty(QQuickItem* root, const char* property, const QString& value) {
    if (!root) return nullptr;
    for (QQuickItem* item : root->findChildren<QQuickItem*>()) {
        if (item->property(property).toString() == value && item->isVisible()) return item;
    }
    return nullptr;
}

void clickItem(QQuickWindow* window, QQuickItem* item) {
    if (!window || !item) return;
    const QPoint point = item->mapToScene(QPointF(item->width() / 2.0,
                                                  item->height() / 2.0)).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
}

QStringList visibleQuickText(QQuickItem* root) {
    QStringList text;
    if (!root) return text;
    for (QQuickItem* item : root->findChildren<QQuickItem*>()) {
        const QString value = item->property("text").toString();
        if (item->isVisible() && !value.isEmpty()) text.append(value);
    }
    return text;
}

QJsonObject windowFacts(PlayerWindow* window) {
    QJsonObject facts;
    if (!window) {
        facts.insert(QStringLiteral("present"), false);
        return facts;
    }
    const qreal dpr = window->devicePixelRatio();
    facts.insert(QStringLiteral("present"), true);
    facts.insert(QStringLiteral("devicePixelRatio"), dpr);
    facts.insert(QStringLiteral("logicalWidth"), window->width());
    facts.insert(QStringLiteral("logicalHeight"), window->height());
    facts.insert(QStringLiteral("pixelWidth"), qRound(window->width() * dpr));
    facts.insert(QStringLiteral("pixelHeight"), qRound(window->height() * dpr));
    facts.insert(QStringLiteral("globalX"), window->position().x());
    facts.insert(QStringLiteral("globalY"), window->position().y());
    if (window->screen()) {
        facts.insert(QStringLiteral("screenDevicePixelRatio"), window->screen()->devicePixelRatio());
        facts.insert(QStringLiteral("screenLogicalWidth"), window->screen()->geometry().width());
        facts.insert(QStringLiteral("screenLogicalHeight"), window->screen()->geometry().height());
    }
    return facts;
}

void saveEvidence(const QJsonObject& evidence, const QString& name) {
    QFile file(artifactPath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        check(false, QStringLiteral("evidence file %1 was written").arg(name));
        return;
    }
    file.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented));
}

struct Run {
    PlayerController* controller = nullptr;
    PlayerWindow* window = nullptr;
};

Run startPlayer(const QString& url, const QString& caption) {
    Run run;
    run.controller = new PlayerController(caption, url);
    run.window = run.controller->window();
    if (!run.window) return run;
    const QPointer<PlayerController> guardedController(run.controller);
    QObject::connect(run.window, &PlayerWindow::sceneLoadFailed, run.controller,
                     [guardedController](const QString& message) {
                         if (guardedController) guardedController->setProperty(
                             "interactionProbeError", message);
                     });
    QObject::connect(run.controller->session(), &VideoSession::errorOccurred, run.controller,
                     [guardedController](const QString& message) {
                         if (guardedController) guardedController->setProperty(
                             "interactionProbeError", message);
                     });
    run.controller->showWindow();
    run.controller->startPlayer();
    return run;
}

bool ready(Run& run) {
    return waitFor([&] {
        const QString error = run.controller
                                  ? run.controller->property("interactionProbeError").toString()
                                  : QString();
        return !error.isEmpty() ||
               (run.window && run.window->rootObject() && run.controller->session()->hasVideo() &&
                run.window->hasVisibleOutputFrame());
    }, 20000) &&
           run.controller->property("interactionProbeError").toString().isEmpty();
}

bool closeRun(Run& run) {
    if (!run.controller) return true;
    const bool accepted = run.controller->requestClose();
    if (accepted) waitFor([&] { return !run.controller->isWindowVisible(); }, 2500);
    return accepted;
}

void fillBaseEvidence(QJsonObject& evidence, const QString& mode, const QString& url,
                      PlayerWindow* window) {
    evidence.insert(QStringLiteral("probe"), QStringLiteral("interaction_probe"));
    evidence.insert(QStringLiteral("case"), mode);
    evidence.insert(QStringLiteral("media"), url);
    evidence.insert(QStringLiteral("window"), windowFacts(window));
}

int runHoverMotion(const QString& url) {
    QJsonObject evidence;
    QJsonArray screenshots;
    std::fprintf(stderr, "case: hover-motion media=%s\n", qPrintable(url));
    Run run = startPlayer(url, QStringLiteral("probe hover motion"));
    fillBaseEvidence(evidence, QStringLiteral("hover-motion"), url, run.window);
    check(run.window && ready(run), QStringLiteral("native Quick player loaded the motion fixture"));
    if (!run.window || !run.window->rootObject() || !run.controller->session()->hasVideo()) {
        evidence.insert(QStringLiteral("passed"), false);
        evidence.insert(QStringLiteral("checks"), checks);
        evidence.insert(QStringLiteral("failures"), failures);
        saveEvidence(evidence, QStringLiteral("interaction-hover-motion-evidence.json"));
        closeRun(run);
        return 1;
    }

    const auto root = run.window->rootObject();
    const QVariant timelineValue = root->property("timeline");
    auto* timeline = qobject_cast<QQuickItem*>(timelineValue.value<QObject*>());
    check(timeline != nullptr, QStringLiteral("the production Quick scene exposes its timeline item"));
    evidence.insert(QStringLiteral("surface"), windowFacts(run.window));
    evidence.insert(QStringLiteral("appDevicePixelRatio"), qApp->devicePixelRatio());
    evidence.insert(QStringLiteral("requestedScaleFactor"), qEnvironmentVariable("QT_SCALE_FACTOR"));

    qint64 hoverToFrameMs = -1;
    QElapsedTimer hoverClock;
    bool previewFrame = false;
    const auto frameConnection = QObject::connect(run.window, &QQuickWindow::frameSwapped,
                                                   run.window, [&] {
        if (hoverToFrameMs >= 0 || !run.controller->hoverPreviewVisible()) return;
        previewFrame = true;
        hoverToFrameMs = hoverClock.elapsed();
    });
    if (timeline) {
        const QPoint point = timeline->mapToScene(QPointF(timeline->width() * 0.35,
                                                           timeline->height() / 2)).toPoint();
        hoverClock.start();
        QTest::mouseMove(run.window, point);
        const bool appeared = waitFor([&] {
            return run.controller->hoverPreviewVisible() &&
                   run.controller->hoverPreviewSource().isValid();
        }, 6000);
        check(appeared, QStringLiteral("real timeline hover requested the decoded Quick preview"));
        evidence.insert(QStringLiteral("firstHoverMs"),
                        static_cast<double>(hoverClock.elapsed()));
        evidence.insert(QStringLiteral("firstHoverFrameSwappedMs"),
                        static_cast<double>(hoverToFrameMs));
        evidence.insert(QStringLiteral("previewSource"),
                        run.controller->hoverPreviewSource().toString());
        check(previewFrame, QStringLiteral("hover preview reached a real Quick window frame"));
        check(run.controller->hoverPreviewVisible(),
              QStringLiteral("hover preview remains visible while the pointer rests on the timeline"));
        const QImage scene = run.window->grabWindow();
        const QString shot = QStringLiteral("hover-motion-parked-hud.png");
        check(!scene.isNull() && scene.save(artifactPath(shot)),
              QStringLiteral("native hover scene pixels were captured"));
        if (!scene.isNull()) screenshots.append(shot);

        // Observe the live video while it continues; do not interpret black pixels
        // from an arbitrary input as a renderer failure.
        const double before = run.controller->session()->position();
        QThread::msleep(500);
        QCoreApplication::processEvents();
        const double after = run.controller->session()->position();
        check(after > before, QStringLiteral("hover sampling left playback live and advancing"));
        evidence.insert(QStringLiteral("playheadBefore"), before);
        evidence.insert(QStringLiteral("playheadAfter"), after);
        const QPoint leave(-24, run.window->height() / 2);
        QTest::mouseMove(run.window, leave);
        waitFor([&] { return !run.controller->hoverPreviewVisible(); }, 5000);
        check(!run.controller->hoverPreviewVisible(),
              QStringLiteral("leaving the timeline dismisses the real hover preview"));
    } else {
        check(false, QStringLiteral("timeline item is available for real pointer input"));
    }
    QObject::disconnect(frameConnection);
    closeRun(run);

    // Preserve the old delayed-opening proof using a local loopback source:
    // only the opening overlay is judged while bytes are deliberately withheld.
    mplayer::probes::DelayedHttpSource delayed;
    check(delayed.serve(url), QStringLiteral("the local delayed origin serves the source file"));
    const QElapsedTimer openingClock = [] {
        QElapsedTimer clock;
        clock.start();
        return clock;
    }();
    Run opening = startPlayer(delayed.url(), QStringLiteral("probe opening"));
    const bool withheld = waitFor([&] {
        return delayed.connections() > 0 && delayed.replyAtMs() < 0;
    }, 2000);
    const bool loadingVisible = opening.controller &&
                                opening.controller->openingLoading() &&
                                opening.window && opening.window->isVisible();
    check(withheld, QStringLiteral("the loopback origin received a request before its delayed reply"));
    check(loadingVisible, QStringLiteral("native opening state is visible while bytes are withheld"));
    evidence.insert(QStringLiteral("openingWindow"), windowFacts(opening.window));
    evidence.insert(QStringLiteral("openingVisibleMs"),
                    loadingVisible ? static_cast<double>(openingClock.elapsed()) : -1.0);
    evidence.insert(QStringLiteral("openingWhileWithheld"), delayed.replyAtMs() < 0);
    if (opening.window) {
        const QImage waiting = opening.window->grabWindow();
        const QString fullName = QStringLiteral("hover-motion-opening-waiting.png");
        check(!waiting.isNull() && waiting.save(artifactPath(fullName)),
              QStringLiteral("native Quick opening-state pixels were captured while waiting"));
        if (!waiting.isNull()) screenshots.append(fullName);
        const QImage centre = waiting.copy(QRect(waiting.width() / 3, waiting.height() / 3,
                                                 waiting.width() / 3, waiting.height() / 3));
        const QString centreName = QStringLiteral("hover-motion-opening-waiting-centre.png");
        check(!centre.isNull() && centre.save(artifactPath(centreName)),
              QStringLiteral("opening-state centre evidence was captured"));
        if (!centre.isNull()) screenshots.append(centreName);
    }
    check(ready(opening), QStringLiteral("the delayed source eventually produces a real picture"));
    evidence.insert(QStringLiteral("openingReplyAtMs"),
                    static_cast<double>(delayed.replyAtMs()));
    const bool retracted = waitFor([&] {
        return opening.controller && !opening.controller->openingLoading() &&
               opening.window && opening.window->hasVisibleOutputFrame();
    }, 5000);
    check(retracted, QStringLiteral("opening state retracts after the first output frame"));
    evidence.insert(QStringLiteral("openingRetracted"), retracted);
    closeRun(opening);

    evidence.insert(QStringLiteral("screenshots"), screenshots);
    evidence.insert(QStringLiteral("passed"), failures == 0);
    evidence.insert(QStringLiteral("checks"), checks);
    evidence.insert(QStringLiteral("failures"), failures);
    saveEvidence(evidence, QStringLiteral("interaction-hover-motion-evidence.json"));
    std::fprintf(stderr, "RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}

int runLanguageSmoke(const QString& url) {
    const QString catalog = qEnvironmentVariable("MIKAI_PROBE_TRANSLATION");
    QJsonObject evidence;
    QJsonArray languages{QStringLiteral("en"), QStringLiteral("uk"), QStringLiteral("en")};
    evidence.insert(QStringLiteral("probe"), QStringLiteral("interaction_probe"));
    evidence.insert(QStringLiteral("case"), QStringLiteral("language-smoke"));
    evidence.insert(QStringLiteral("media"), url);
    evidence.insert(QStringLiteral("catalog"), catalog);
    evidence.insert(QStringLiteral("languages"), languages);
    if (catalog.isEmpty() || !QFileInfo(catalog).isFile()) {
        check(false, QStringLiteral("MIKAI_PROBE_TRANSLATION names the built application catalog"));
        evidence.insert(QStringLiteral("passed"), false);
        evidence.insert(QStringLiteral("checks"), checks);
        evidence.insert(QStringLiteral("failures"), failures);
        saveEvidence(evidence, QStringLiteral("interaction-language-evidence.json"));
        return 1;
    }

    Run run = startPlayer(url, QStringLiteral("probe language smoke"));
    fillBaseEvidence(evidence, QStringLiteral("language-smoke"), url, run.window);
    check(run.window && ready(run), QStringLiteral("the native player presents the moving fixture"));
    QTranslator translator;
    check(translator.load(catalog), QStringLiteral("the application translation catalog loads"));
    const QLocale priorLocale = QLocale();
    const auto apply = [&](const QString& code) {
        if (QCoreApplication::removeTranslator(&translator)) {}
        QLocale::setDefault(QLocale(code == QLatin1String("uk") ? QLocale::Ukrainian
                                                                  : QLocale::English));
        if (!QCoreApplication::installTranslator(&translator)) return false;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LanguageChange);
        QCoreApplication::processEvents();
        return true;
    };
    check(apply(QStringLiteral("en")), QStringLiteral("English translator and locale are applied"));
    evidence.insert(QStringLiteral("localeBaseline"), QLocale().name());
    check(QLocale().language() == QLocale::English,
          QStringLiteral("English switch updates Qt's default locale"));
    const QStringList englishText = run.window
                                        ? visibleQuickText(run.window->rootObject())
                                        : QStringList();
    check(apply(QStringLiteral("uk")), QStringLiteral("Ukrainian translator and locale are applied"));
    evidence.insert(QStringLiteral("localeUkrainian"), QLocale().name());
    check(QLocale().language() == QLocale::Ukrainian,
          QStringLiteral("Ukrainian switch updates Qt's default locale"));
    const QStringList ukrainianText = run.window
                                          ? visibleQuickText(run.window->rootObject())
                                          : QStringList();
    check(ukrainianText != englishText,
          QStringLiteral("the open native Quick chrome re-translates in place"));
    const double before = run.controller ? run.controller->session()->position() : -1.0;
    QThread::msleep(400);
    QCoreApplication::processEvents();
    const double after = run.controller ? run.controller->session()->position() : -1.0;
    check(before >= 0.0 && after > before,
          QStringLiteral("language changes do not stop live playback"));
    evidence.insert(QStringLiteral("playheadBefore"), before);
    evidence.insert(QStringLiteral("playheadAfter"), after);
    check(apply(QStringLiteral("en")), QStringLiteral("English can be restored in the same session"));
    check(run.window && visibleQuickText(run.window->rootObject()) != ukrainianText,
          QStringLiteral("restoring English re-translates the live scene"));
    QLocale::setDefault(priorLocale);
    if (run.window) {
        const QImage shot = run.window->grabWindow();
        const QString path = QStringLiteral("interaction-language-player.png");
        check(!shot.isNull() && shot.save(artifactPath(path)),
              QStringLiteral("native translated player pixels were captured"));
        evidence.insert(QStringLiteral("screenshot"), path);
    }
    evidence.insert(QStringLiteral("passed"), failures == 0);
    evidence.insert(QStringLiteral("checks"), checks);
    evidence.insert(QStringLiteral("failures"), failures);
    saveEvidence(evidence, QStringLiteral("interaction-language-evidence.json"));
    closeRun(run);
    std::fprintf(stderr, "RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}

int runDefault(const QString& url) {
    QJsonObject evidence;
    QJsonArray screenshots;
    Run run = startPlayer(url, QStringLiteral("probe 720p"));
    fillBaseEvidence(evidence, QStringLiteral("native-interaction"), url, run.window);
    check(run.window && ready(run), QStringLiteral("production Quick player opened a visible picture"));
    if (!run.window || !run.window->rootObject() || !run.controller->session()->hasVideo()) {
        evidence.insert(QStringLiteral("passed"), false);
        evidence.insert(QStringLiteral("checks"), checks);
        evidence.insert(QStringLiteral("failures"), failures);
        saveEvidence(evidence, QStringLiteral("interaction-evidence.json"));
        closeRun(run);
        return 1;
    }

    PlayerController* controller = run.controller;
    PlayerWindow* window = run.window;
    VideoSession* session = controller->session();
    QQuickItem* root = window->rootObject();
    const QImage picture = window->grabVideoFrame();
    check(!picture.isNull(), QStringLiteral("the production video item supplied picture pixels"));
    if (!picture.isNull()) {
        const QString file = QStringLiteral("interaction-picture.png");
        check(picture.save(artifactPath(file)), QStringLiteral("overlay-free video evidence was saved"));
        screenshots.append(file);
    }

    // Measure input-to-presentation using a real key delivered to the active
    // Quick window and the next frame swap, not a synthetic controller call.
    window->requestActivate();
    window->requestUpdate();
    QCoreApplication::processEvents();
    check(waitFor([&] { return window->isActive(); }, 3000),
          QStringLiteral("native Quick window accepted activation"));
    check(window->activeFocusItem() != nullptr,
          QStringLiteral("the native scene has a focused input target after activation"));
    bool keyFrame = false;
    qint64 keyLatency = -1;
    QElapsedTimer keyClock;
    const auto keyConnection = QObject::connect(window, &QQuickWindow::frameSwapped, window, [&] {
        if (keyLatency < 0) {
            keyFrame = true;
            keyLatency = keyClock.elapsed();
        }
    });
    const bool wasPlaying = session->isPlaying();
    keyClock.start();
    QTest::keyClick(window, Qt::Key_Space);
    const bool keyWorked = waitFor([&] { return session->isPlaying() != wasPlaying && keyFrame; }, 2500);
    QObject::disconnect(keyConnection);
    check(keyWorked, QStringLiteral("native Space input changed playback and reached presentation"));
    evidence.insert(QStringLiteral("spaceToFrameMs"), static_cast<double>(keyLatency));
    if (session->isPlaying() != wasPlaying) controller->togglePlayPause();

    // A right-click through QQuickWindow opens the production picture menu.
    const QPoint center(window->width() / 2, window->height() / 2);
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, center);
    const bool pictureMenuOpened = waitFor([&] {
        return visibleMenu(root, QStringLiteral("picture")) != nullptr;
    }, 2000);
    check(pictureMenuOpened, QStringLiteral("native right-click opened the real picture menu"));
    evidence.insert(QStringLiteral("pictureMenuOpened"), pictureMenuOpened);
    if (pictureMenuOpened) {
        PlayerMenuModel* pictureModel = controller->menuModel(QStringLiteral("picture"));
        bool hasCapture = false;
        bool hasClip = false;
        for (int row = 0; pictureModel && row < pictureModel->rowCount(); ++row) {
            const auto action = pictureModel->row(row).action;
            hasCapture = hasCapture || action == PlayerMenuModel::Action::CaptureFrame;
            hasClip = hasClip || action == PlayerMenuModel::Action::OpenClipWindow;
        }
        check(hasCapture && hasClip,
              QStringLiteral("picture menu exposes its production capture and clip actions"));
        const QImage menuShot = window->grabWindow();
        const QString file = QStringLiteral("interaction-picture-menu.png");
        check(!menuShot.isNull() && menuShot.save(artifactPath(file)),
              QStringLiteral("picture-menu scene pixels were captured"));
        if (!menuShot.isNull()) screenshots.append(file);
        controller->closeMenus();
    }

    // Open the actual settings popover by clicking the real QML control, then
    // dismiss it through the native Escape key path.
    QQuickItem* settingsButton = itemWithProperty(root, "iconName", QStringLiteral("gear"));
    check(settingsButton != nullptr, QStringLiteral("the scene exposes its real settings button"));
    if (settingsButton) clickItem(window, settingsButton);
    const bool settingsOpen = waitFor([&] {
        return visibleMenu(root, QStringLiteral("settings")) != nullptr;
    }, 2000);
    check(settingsOpen, QStringLiteral("the real settings menu opened from its presentation request"));
    if (settingsOpen) {
        PlayerMenuModel* model = controller->menuModel(QStringLiteral("settings"));
        bool hasQualityAction = false;
        for (int i = 0; model && i < model->rowCount(); ++i) {
            if (model->row(i).action == PlayerMenuModel::Action::SelectQuality) {
                hasQualityAction = true;
                break;
            }
        }
        check(hasQualityAction, QStringLiteral("settings model contains the production quality choices"));
        QTest::keyClick(window, Qt::Key_Escape);
        check(waitFor([&] { return visibleMenu(root, QStringLiteral("settings")) == nullptr; }, 1500),
              QStringLiteral("native Escape dismissed the real settings menu"));
    }

    // Enhancement submenu expansion is driven by an actual pointer hover over
    // the production cascade row, not by a direct menu-model action.
    QQuickItem* enhanceButton = itemWithProperty(root, "iconName", QStringLiteral("spark"));
    check(enhanceButton != nullptr, QStringLiteral("the scene exposes its real enhancement button"));
    if (enhanceButton) {
        clickItem(window, enhanceButton);
        const bool opened = waitFor([&] {
            return visibleMenu(root, QStringLiteral("enhancements")) != nullptr;
        }, 2000);
        check(opened, QStringLiteral("real enhancement button opened the production menu"));
        PlayerMenuModel* model = controller->menuModel(QStringLiteral("enhancements"));
        int submenu = -1;
        for (int row = 0; opened && model && row < model->rowCount(); ++row) {
            if (model->row(row).submenu) { submenu = row; break; }
        }
        QQuickItem* cascadeRow = submenu >= 0
            ? menuRow(visibleMenu(root, QStringLiteral("enhancements")), model->row(submenu).value)
            : nullptr;
        if (cascadeRow) {
            const QPoint point = cascadeRow->mapToScene(
                QPointF(cascadeRow->width() / 2.0, cascadeRow->height() / 2.0)).toPoint();
            QTest::mouseMove(window, point);
            check(waitFor([&] {
                return visibleMenu(root, model->row(submenu).value) != nullptr;
            }, 1800), QStringLiteral("real submenu-row hover opened its production child"));
        } else {
            check(false, QStringLiteral("enhancement menu exposes a visible submenu row"));
        }
        controller->closeMenus();
    }

    // Timeline is a real QQuickItem owned by PlayerView. A drag emits seeks and
    // keeps a live source; hover causes the scene's own preview request path.
    auto* timeline = qobject_cast<QQuickItem*>(root->property("timeline").value<QObject*>());
    check(timeline != nullptr, QStringLiteral("native scene exposes its real timeline item"));
    if (timeline && controller->duration() > 0.0) {
        const QPoint dragPoint = timeline->mapToScene(QPointF(timeline->width() * 0.4,
                                                               timeline->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, dragPoint);
        check(waitFor([&] { return controller->hoverPreviewVisible(); }, 4000),
              QStringLiteral("timeline pointer input activated the real hover preview"));
        evidence.insert(QStringLiteral("hoverPreviewSource"), controller->hoverPreviewSource().toString());
        QTest::mouseMove(window, QPoint(-20, -20));
        waitFor([&] { return !controller->hoverPreviewVisible(); }, 4000);
    }

    // Deliver a local file through the production QQuickWindow/DropArea route.
    // A copied fixture gives the take-over a distinct URL while exercising the
    // same canonical controller and session.
    QTemporaryDir dropDir;
    const QString droppedPath = QDir(dropDir.path()).filePath(QStringLiteral("takeover.mp4"));
    const bool dropFixtureReady = dropDir.isValid() && QFile::copy(url, droppedPath);
    check(dropFixtureReady, QStringLiteral("a distinct local video fixture was prepared for drop"));
    evidence.insert(QStringLiteral("dropFixtureReady"), dropFixtureReady);
    if (dropFixtureReady) {
        const quint64 sourceEpochBeforeDrop = session->sourceEpoch();
        const QString sourceUrlBeforeDrop = session->sourceUrl();
        const QPoint dropPoint(window->width() / 2, window->height() / 2);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(droppedPath)});
        QDragEnterEvent dragEnter(dropPoint, Qt::CopyAction, &mime,
                                  Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &dragEnter);
        QDragMoveEvent dragMove(dropPoint, Qt::CopyAction, &mime,
                                Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &dragMove);
        QDropEvent drop(QPointF(dropPoint), Qt::CopyAction, &mime,
                        Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &drop);
        const bool tookOver = waitFor([&] {
        check(drop.isAccepted(),
              QStringLiteral("the Quick DropArea accepted the local video copy action"));
        const bool tookOver = waitFor([&] {
            return session->sourceEpoch() > sourceEpochBeforeDrop &&
                   QFileInfo(session->sourceUrl()).absoluteFilePath() ==
                       QFileInfo(droppedPath).absoluteFilePath() &&
                   session->hasVideo() && window->hasVisibleOutputFrame();
        }, 20000);
        check(tookOver, QStringLiteral("dropping a local video took over the existing player session"));
        check(controller->window() == window && controller->session() == session,
              QStringLiteral("dropped media retained the exact Quick window and session"));
        evidence.insert(QStringLiteral("dropAccepted"), drop.isAccepted());
        evidence.insert(QStringLiteral("dropEpochBefore"), static_cast<qint64>(sourceEpochBeforeDrop));
        evidence.insert(QStringLiteral("dropEpochAfter"), static_cast<qint64>(session->sourceEpoch()));
        evidence.insert(QStringLiteral("dropSourceBefore"), sourceUrlBeforeDrop);
        evidence.insert(QStringLiteral("dropSourceAfter"), session->sourceUrl());
    }

    // PiP is a compact presentation of the same QQuickWindow and VideoSession.
    PlayerWindow* const sameWindow = controller->window();
    VideoSession* const sameSession = controller->session();
    const uint64_t sourceEpochBeforePip = session->sourceEpoch();
    const double positionBeforePip = session->position();
    QQuickItem* pipButton = itemWithProperty(root, "iconName", QStringLiteral("pip"));
    check(pipButton != nullptr, QStringLiteral("the scene exposes its real PiP button"));
    if (pipButton) clickItem(window, pipButton);
    const bool pipEntered = waitFor([&] { return controller->isPipActive(); }, 3000);
    check(pipEntered, QStringLiteral("native PiP entered on the same player window"));
    check(controller->window() == sameWindow && controller->session() == sameSession,
          QStringLiteral("PiP retained the exact window and playback-session identities"));
    check(session->sourceEpoch() == sourceEpochBeforePip,
          QStringLiteral("PiP did not reopen or replace the active media source"));
    check(window->compactPip(), QStringLiteral("window reports its compact PiP presentation"));
    const QImage pipPicture = window->grabVideoFrame();
    const qreal pipDpr = window->devicePixelRatio();
    evidence.insert(QStringLiteral("pipDevicePixelRatio"), pipDpr);
    evidence.insert(QStringLiteral("pipLogicalWidth"), window->width());
    evidence.insert(QStringLiteral("pipLogicalHeight"), window->height());
    if (qEnvironmentVariableIsSet("MIKAI_PROBE_EXPECT_FRACTIONAL_DPR")) {
        check(std::abs(pipDpr - 1.25) < 0.01,
              QStringLiteral("PiP scene is presented at the requested fractional 1.25 DPR"));
    }
    const QImage pipScene = window->grabWindow();
    check(!pipScene.isNull() &&
              std::abs(pipScene.width() - qRound(window->width() * pipDpr)) <= 1 &&
              std::abs(pipScene.height() - qRound(window->height() * pipDpr)) <= 1,
          QStringLiteral("compact PiP scene pixels match its logical geometry at the active DPR"));
    if (!pipScene.isNull()) {
        const QString file = QStringLiteral("interaction-pip-scene.png");
        check(pipScene.save(artifactPath(file)),
              QStringLiteral("compact PiP controls and rounded scene pixels were saved"));
        screenshots.append(file);
    }
    const QStringList pipControlNames{
        QStringLiteral("player.pip.playPause"),
        QStringLiteral("player.pip.volume"),
        QStringLiteral("player.pip.timeline.scrub"),
        QStringLiteral("player.pip.settings.button")};
    bool pipControlsVisible = true;
    for (const QString& name : pipControlNames) {
        QQuickItem* item = root->findChild<QQuickItem*>(name);
        const QPointF topLeft = item != nullptr ? item->mapToItem(root, QPointF(0, 0)) : QPointF();
        const QPointF bottomRight =
            item != nullptr ? item->mapToItem(root, QPointF(item->width(), item->height())) : QPointF();
        const bool visible = item != nullptr && item->isVisible() &&
                             item->width() > 0.0 && item->height() > 0.0 &&
                             topLeft.x() >= 0.0 && topLeft.y() >= 0.0 &&
                             bottomRight.x() <= root->width() && bottomRight.y() <= root->height();
        pipControlsVisible = pipControlsVisible && visible;
        evidence.insert(name, visible);
    }
    check(pipControlsVisible,
          QStringLiteral("play, volume, timeline and settings controls render inside the compact scene"));
    bool translucentRoundedPanel = false;
    for (QQuickItem* item : root->findChildren<QQuickItem*>()) {
        const QVariant radius = item->property("radius");
        const QVariant color = item->property("color");
        if (!item->isVisible() || !radius.isValid() || !color.isValid() ||
            radius.toReal() <= 0.0 ||
            std::abs(item->width() - root->width()) > 0.5 ||
            std::abs(item->height() - root->height()) > 0.5) continue;
        const QColor panelColor = color.value<QColor>();
        if (panelColor.isValid() && panelColor.alphaF() > 0.0 && panelColor.alphaF() < 1.0) {
            translucentRoundedPanel = true;
            evidence.insert(QStringLiteral("pipPanelRadius"), radius.toReal());
            evidence.insert(QStringLiteral("pipPanelAlpha"), panelColor.alphaF());
            break;
        }
    }
    check(translucentRoundedPanel,
          QStringLiteral("the compact scene contains its visible rounded translucent control panel"));
    const QImage pipPicture = window->grabVideoFrame();
    check(!pipPicture.isNull(), QStringLiteral("the same video target remains available in PiP"));
    if (!pipPicture.isNull()) {
        const QString file = QStringLiteral("interaction-pip-picture.png");
        check(pipPicture.save(artifactPath(file)), QStringLiteral("PiP video pixels were saved"));
        screenshots.append(file);
    }
    const bool playingBeforePipKey = session->isPlaying();
    QTest::keyClick(window, Qt::Key_Space);
    check(waitFor([&] { return session->isPlaying() != playingBeforePipKey; }, 1500),
          QStringLiteral("real Space input controls the same playback session in PiP"));
    check(controller->window() == sameWindow && controller->session() == sameSession,
          QStringLiteral("keyboard input in PiP does not create or switch playback owners"));
    QQuickItem* restoreButton = itemWithProperty(root, "iconName", QStringLiteral("expand"));
    check(restoreButton != nullptr, QStringLiteral("the compact scene exposes its real restore button"));
    if (restoreButton) clickItem(window, restoreButton);
    check(waitFor([&] { return !controller->isPipActive(); }, 3000),
          QStringLiteral("real PiP restore input returned to the full presentation"));
    check(session->sourceEpoch() == sourceEpochBeforePip,
          QStringLiteral("restoring from PiP retained the original source epoch"));
    evidence.insert(QStringLiteral("positionBeforePip"), positionBeforePip);
    evidence.insert(QStringLiteral("positionAfterPip"), session->position());

    evidence.insert(QStringLiteral("screenshots"), screenshots);
    evidence.insert(QStringLiteral("passed"), failures == 0);
    evidence.insert(QStringLiteral("checks"), checks);
    evidence.insert(QStringLiteral("failures"), failures);
    saveEvidence(evidence, QStringLiteral("interaction-evidence.json"));
    closeRun(run);
    std::fprintf(stderr, "RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (QString::fromLocal8Bit(argv[index]) == QLatin1String("--fractional-dpr")) {
            qputenv("QT_SCALE_FACTOR", QByteArrayLiteral("1.25"));
            qputenv("MIKAI_PROBE_EXPECT_FRACTIONAL_DPR", QByteArrayLiteral("1"));
            break;
        }
    }
    QApplication app(argc, argv);
    QString media;
    QString output;
    bool language = false;
    bool hover = false;
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QLatin1String("--language-smoke")) language = true;
        else if (argument == QLatin1String("--hover-motion")) hover = true;
        else if (argument == QLatin1String("--fractional-dpr")) {}
        else if (argument == QLatin1String("--output-dir")) {
            if (++index >= argc) {
                std::fprintf(stderr, "FAIL --output-dir needs a directory\n");
                return 3;
            }
            output = QString::fromLocal8Bit(argv[index]);
        } else if (media.isEmpty()) media = argument;
    }
        if (media.isEmpty() || !QFileInfo::exists(media)) {
        std::fprintf(stderr, "Usage: InteractionProbe <known-duration-video-file> "
                             "[--language-smoke | --hover-motion | --fractional-dpr] "
                             "[--output-dir <absolute dir>]\\n");
        return 3;
    }
    if (!output.isEmpty()) {
        if (!QFileInfo(output).isAbsolute() || !QDir().mkpath(output)) {
            std::fprintf(stderr, "FAIL --output-dir must be an absolute directory this run may write: %s\n",
                         qPrintable(output));
            return 3;
        }
        outputDir = output;
    }
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiInteractionProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("InteractionProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    if (!mplayer::probes::selectRuntime()) return 2;
    media = QFileInfo(media).absoluteFilePath();
    if (language) return runLanguageSmoke(media);
    if (hover) return runHoverMotion(media);
    return runDefault(media);
}
