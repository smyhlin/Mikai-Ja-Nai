// Anime4K menu probe: production Quick menu activation, live shader chains,
// rendered picture changes, persistence, Compare isolation and same-window PiP.
//
// Rows are activated through QML's real pointer handlers. The menu model and
// mpv's shader-property echo are checked as evidence, but neither substitutes
// for the pixel comparison proving the shader actually rendered.
//
// Run with the shaders deployed beside the executable:
//   build-cmake/anime4k_menu_probe.exe

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QMenu>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

#include "Anime4K.hpp"
#include "CompareWindow.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"
#include "PlayerController.hpp"
#include "PlayerMenuModel.hpp"
#include "PlayerWindow.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSession.hpp"

using namespace mplayer;
using namespace mplayer::quick;

namespace {

constexpr int kSourceWidth = 480;
constexpr int kSourceHeight = 270;
constexpr auto kExperiment = "exp_3d_graphics";
int failures = 0;

void check(bool ok, const char* what) {
    std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

void pump(int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 20) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    QCoreApplication::processEvents();
}

template <typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMs) {
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 40) {
        QCoreApplication::processEvents();
        if (predicate()) return true;
        QThread::msleep(40);
    }
    return predicate();
}

QRect pictureBand(const QImage& frame) {
    if (frame.isNull() || frame.width() < 64 || frame.height() < 64) return {};
    const double scale = std::min(double(frame.width()) / kSourceWidth,
                                  double(frame.height()) / kSourceHeight);
    const int width = int(kSourceWidth * scale);
    const int height = int(kSourceHeight * scale);
    if (width < 64 || height < 64) return {};
    return {(frame.width() - width) / 2 + width / 8,
            (frame.height() - height) / 2 + height / 8, width * 3 / 4, height * 3 / 4};
}

double bandDifference(const QImage& before, const QImage& after) {
    const QRect band = pictureBand(before);
    if (band.isEmpty() || after.isNull() || after.size() != before.size()) return -1.0;
    const QImage left = before.convertToFormat(QImage::Format_RGB888);
    const QImage right = after.convertToFormat(QImage::Format_RGB888);
    double sum = 0.0;
    qint64 count = 0;
    for (int y = band.top(); y < band.bottom(); ++y) {
        const uchar* a = left.constScanLine(y);
        const uchar* b = right.constScanLine(y);
        for (int x = band.left(); x < band.right(); ++x) {
            const int at = x * 3;
            sum += std::abs(int(a[at]) - int(b[at])) +
                   std::abs(int(a[at + 1]) - int(b[at + 1])) +
                   std::abs(int(a[at + 2]) - int(b[at + 2]));
            count += 3;
        }
    }
    return count > 0 ? sum / double(count) : -1.0;
}

int bandRange(const QImage& frame) {
    const QRect band = pictureBand(frame);
    if (band.isEmpty()) return -1;
    const QImage image = frame.convertToFormat(QImage::Format_RGB888);
    int low = 255;
    int high = 0;
    for (int y = band.top(); y < band.bottom(); ++y) {
        const uchar* line = image.constScanLine(y);
        for (int x = band.left(); x < band.right(); ++x) {
            const int at = x * 3;
            const int luma = (line[at] * 299 + line[at + 1] * 587 + line[at + 2] * 114) / 1000;
            low = std::min(low, luma);
            high = std::max(high, luma);
        }
    }
    return high - low;
}

QImage settledFrame(PlayerWindow* window, int timeoutMs = 6000) {
    QImage previous;
    int stable = 0;
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 40) {
        QCoreApplication::processEvents();
        QImage current = window->grabVideoFrame();
        if (!current.isNull()) current = current.convertToFormat(QImage::Format_RGB32);
        if (!current.isNull() && current == previous) {
            if (++stable >= 3) return current;
        } else {
            stable = 0;
        }
        previous = current;
        QThread::msleep(40);
    }
    return {};
}

QQuickItem* menuPanel(PlayerWindow* window, const QString& menuId) {
    if (window == nullptr || window->rootObject() == nullptr) return nullptr;
    for (QQuickItem* item : window->rootObject()->findChildren<QQuickItem*>()) {
        if (item->property("menuId").toString() == menuId &&
            item->property("menuModel").isValid() && item->isVisible()) return item;
    }
    return nullptr;
}

QQuickItem* menuRow(PlayerWindow* window, const QString& menuId, const QString& value,
                    bool submenu = false) {
    QQuickItem* panel = menuPanel(window, menuId);
    if (panel == nullptr) return nullptr;
    for (QQuickItem* item : panel->findChildren<QQuickItem*>()) {
        if (!item->isVisible() || item->property("rowValue").toString() != value) continue;
        if (item->property("rowSubmenu").toBool() == submenu) return item;
    }
    return nullptr;
}

bool clickRow(PlayerWindow* window, const QString& menuId, const QString& value,
              bool submenu = false) {
    QQuickItem* row = menuRow(window, menuId, value, submenu);
    if (row == nullptr) return false;
    const QPoint point = row->mapToScene(QPointF(row->width() / 2.0, row->height() / 2.0)).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
    QCoreApplication::processEvents();
    return true;
}

bool openAnimeRoot(PlayerController* controller, PlayerWindow* window) {
    emit controller->menuOpenRequested(QStringLiteral("enhancements"));
    return waitUntil([&] { return menuPanel(window, QStringLiteral("enhancements")) != nullptr; }, 3000) &&
           clickRow(window, QStringLiteral("enhancements"), QStringLiteral("anime4k"), true) &&
           waitUntil([&] { return menuPanel(window, QStringLiteral("anime4k")) != nullptr; }, 3000);
}

bool clickLeaf(const QString& category, const QString& id, PlayerController* controller,
               PlayerWindow* window) {
    if (!openAnimeRoot(controller, window)) return false;
    QString parent = QStringLiteral("anime4k");
    if (!category.isEmpty()) {
        const QStringList parts = category.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (int i = 1; i < parts.size(); ++i) {
            const QString child = parent + QLatin1Char('/') + parts.at(i);
            if (!clickRow(window, parent, child, true) ||
                !waitUntil([&] { return menuPanel(window, child) != nullptr; }, 3000)) return false;
            parent = child;
        }
    }
    return clickRow(window, parent, id, false);
}

bool chainMatches(VideoSession* session, const QString& id) {
    const QStringList expected = anime4k::filesForId(id);
    const QString live = session->effectiveShaderChain();
    int found = -1;
    for (const QString& file : expected) {
        found = live.indexOf(file, found + 1);
        if (found < 0) return false;
    }
    const int staged = live.isEmpty() ? 0 : live.count(QLatin1String(".glsl"));
    return staged == expected.size() &&
           anime4k::chainIsComplete(anime4k::candidateForId(id));
}

bool chainSettlesTo(VideoSession* session, const QString& id, int timeoutMs = 8000) {
    return waitUntil([session, id] { return chainMatches(session, id); }, timeoutMs);
}

bool chainSettlesOff(VideoSession* session, int timeoutMs = 8000) {
    return waitUntil([session] { return session->effectiveShaderChain().isEmpty(); }, timeoutMs);
}

void saveFrame(const QImage& frame, const QString& name) {
    const QString path = QDir::current().filePath(QStringLiteral("build-cmake/") + name);
    check(!frame.isNull() && QDir().mkpath(QFileInfo(path).absolutePath()) && frame.save(path),
          (QStringLiteral("framebuffer written: ") + name).toUtf8().constData());
}
void shoot(PlayerWindow* window, const QString& name) {
    const QString path = QDir::current().filePath(QStringLiteral("build-cmake/") + name);
    const QImage image = window != nullptr ? window->grabWindow() : QImage();
    check(!image.isNull() && QDir().mkpath(QFileInfo(path).absolutePath()) && image.save(path),
          (QStringLiteral("screenshot written: ") + name).toUtf8().constData());
}

std::optional<QString> textProperty(VideoSession* session, const QString& name) {
    const auto value = session->probeMpvProperty(name, ProbePropertyKind::Text);
    if (!value || value->kind != ProbePropertyValue::Kind::Text) return std::nullopt;
    return value->text;
}

std::optional<double> numberProperty(VideoSession* session, const QString& name) {
    const auto value = session->probeMpvProperty(name, ProbePropertyKind::Number);
    if (!value || !value->isNumeric()) return std::nullopt;
    return value->asNumber();
}

PlayerController* createPlayer(const QString& url) {
    auto* controller = new PlayerController(QStringLiteral("Anime4K menu probe"), url);
    controller->showWindow();
    controller->startPlayer();
    return controller;
}

bool waitForVideo(PlayerController* controller, int timeoutMs = 15000) {
    return waitUntil([controller] {
        return controller != nullptr && controller->session()->hasVideo() &&
               controller->window()->hasVisibleOutputFrame();
    }, timeoutMs);
}

bool closePlayer(QPointer<PlayerController>& controller) {
    if (controller == nullptr || !controller->requestClose()) return false;
    if (!waitUntil([&controller] { return controller == nullptr ||
                                           !controller->isWindowVisible(); }, 2500)) {
        return false;
    }
    controller->deleteLater();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return waitUntil([&controller] { return controller.isNull(); }, 6000);
}

QStringList chooserLeafIds(const QList<QAction*>& actions) {
    QStringList ids;
    for (QAction* action : actions) {
        if (action->menu() != nullptr) ids += chooserLeafIds(action->menu()->actions());
        else ids << action->data().toString();
    }
    return ids;
}
QAction* chooserAction(QMenu* menu, const QString& id, QMenu** parent) {
    if (menu == nullptr) return nullptr;
    for (QAction* action : menu->actions()) {
        if (action->menu() != nullptr) {
            if (QAction* found = chooserAction(action->menu(), id, parent)) return found;
        } else if (action->data().toString() == id) {
            if (parent != nullptr) *parent = menu;
            return action;
        }
    }
    return nullptr;
}

int countActionText(const QList<QAction*>& actions, const QString& text) {
    int count = 0;
    for (QAction* action : actions) {
        if (action->text() == text) ++count;
        if (action->menu() != nullptr) count += countActionText(action->menu()->actions(), text);
    }
    return count;
}
QStringList chooserPathFor(const QString& categoryId) {
    QStringList path;
    QString parent;
    for (const QString& component : categoryId.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (!parent.isEmpty()) parent += QLatin1Char('/');
        parent += component;
        const QString label = anime4k::categoryLabel(parent);
        if (!label.isEmpty()) path << label;
    }
    return path;
}

bool chooserPlacementsMatch(QMenu* menu, const QStringList& parentPath,
                            const QHash<QString, QStringList>& expected) {
    if (menu == nullptr) return false;
    for (QAction* action : menu->actions()) {
        if (action->menu() != nullptr) {
            QStringList nestedPath = parentPath;
            nestedPath << action->text();
            if (!chooserPlacementsMatch(action->menu(), nestedPath, expected)) return false;
        } else if (expected.contains(action->data().toString()) &&
                   expected.value(action->data().toString()) != parentPath) {
            return false;
        }
    }
    return true;
}

bool runMissingStageRefusalSmoke(VideoSession* session, PlayerWindow* window,
                                 const QImage& fastAFrame) {
    const QString appDir = QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath();
    const QFileInfo marker(QDir(appDir).filePath(QStringLiteral("OWNED-ANIME4K-PIXEL-ROOT")));
    if (appDir.isEmpty() || !marker.isFile() || marker.isSymLink()) {
        std::fprintf(stderr, "FAIL refusal smoke requires its owned app-directory marker\n");
        return false;
    }
    const QString shaderDir = anime4k::findShadersDir();
    const QString expectedDir = QDir(appDir).filePath(QStringLiteral("shaders"));
    const QFileInfo directory(expectedDir);
    if (shaderDir.isEmpty() || directory.isSymLink() || !directory.isDir() ||
        QDir::cleanPath(shaderDir) != QDir::cleanPath(expectedDir) ||
        directory.canonicalFilePath() != QDir::cleanPath(expectedDir)) {
        std::fprintf(stderr, "FAIL refusal smoke requires nonsymlink applicationDir/shaders\n");
        return false;
    }
    const QString stageName = QStringLiteral("Anime4K_Clamp_Highlights.glsl");
    const QString stagePath = QDir(shaderDir).filePath(stageName);
    const QFileInfo stage(stagePath);
    if (!stage.isFile() || stage.isSymLink() ||
        stage.canonicalFilePath() != QDir::cleanPath(stagePath)) {
        std::fprintf(stderr, "FAIL refusal smoke requires a regular nonsymlink shader stage\n");
        return false;
    }
    const auto fastA = anime4k::candidateForId(QStringLiteral("fast_a"), shaderDir);
    const auto fastB = anime4k::candidateForId(QStringLiteral("fast_b"), shaderDir);
    if (!fastA.files.contains(stageName) || !fastB.files.contains(stageName) ||
        session->anime4KChainId() != QLatin1String("fast_a") ||
        session->anime4KMode() != anime4k::Mode::FastA || fastAFrame.isNull()) {
        std::fprintf(stderr, "FAIL refusal smoke requires fast_a and its captured active frame\n");
        return false;
    }
    const QString parked = stagePath + QStringLiteral(".refusal-smoke-parked");
    if (QFileInfo::exists(parked) || QFileInfo(parked).isSymLink() ||
        !QFile::rename(stagePath, parked)) {
        std::fprintf(stderr, "FAIL refusal smoke could not safely park the selected shader\n");
        return false;
    }
    QString reason;
    const auto connection = QObject::connect(
        session, &VideoSession::anime4kGated, session,
        [&reason](const QString& message) { reason = message; });
    session->setAnime4KChain(QStringLiteral("fast_b"));
    QObject::disconnect(connection);
    pump(250);
    const bool refused = !reason.trimmed().isEmpty() &&
                         session->anime4KChainId() == QLatin1String("fast_a") &&
                         session->anime4KMode() == anime4k::Mode::FastA;
    const double difference = bandDifference(fastAFrame, settledFrame(window));
    const bool restored = QFile::rename(parked, stagePath);
    std::fprintf(stderr,
                 "refusal smoke refused-fast_b=%s state-preserved=%s image-preserved=%s restored=%s\\n",
                 refused ? "yes" : "NO", refused ? "yes" : "NO",
                 difference >= 0.0 && difference <= 0.01 ? "yes" : "NO",
                 restored ? "yes" : "NO");
    return refused && difference >= 0.0 && difference <= 0.01 && restored;
}

int runPixelDiagnostic(const QString& directory) {
    const QString source = QString::fromLocal8Bit(qgetenv("MIKAI_ANIME4K_PIXEL_SOURCE")).trimmed();
    if (source.isEmpty() || !QDir().mkpath(directory)) {
        std::fprintf(stderr, "FAIL pixel diagnostic needs MIKAI_ANIME4K_PIXEL_SOURCE and writable output directory\n");
        return 1;
    }
    QPointer<PlayerController> controller = createPlayer(source);
    if (!waitForVideo(controller)) {
        std::fprintf(stderr, "FAIL pixel diagnostic source did not produce a ready Quick video surface\n");
        if (controller) controller->requestClose();
        return 1;
    }
    VideoSession* session = controller->session();
    PlayerWindow* window = controller->window();
    session->setPaused(true);
    const auto pause = session->probeMpvProperty(QStringLiteral("pause"), ProbePropertyKind::Flag);
    if (!pause || pause->kind != ProbePropertyValue::Kind::Flag || !pause->flag) {
        std::fprintf(stderr, "FAIL pixel diagnostic could not confirm paused playback\n");
        controller->requestClose();
        return 1;
    }
    const auto time = numberProperty(session, QStringLiteral("time-pos"));
    const auto path = textProperty(session, QStringLiteral("path"));
    const auto width = numberProperty(session, QStringLiteral("video-params/w"));
    const auto height = numberProperty(session, QStringLiteral("video-params/h"));
    std::fprintf(stderr, "pixel diagnostic path=%s time-pos=%s source-resolution=%sx%s\n",
                 path ? qPrintable(*path) : "unavailable",
                 time ? qPrintable(QString::number(*time, 'f', 3)) : "unavailable",
                 width ? qPrintable(QString::number(*width, 'f', 0)) : "unavailable",
                 height ? qPrintable(QString::number(*height, 'f', 0)) : "unavailable");
    if (!time || !width || !height || *width <= 0 || *height <= 0) {
        std::fprintf(stderr, "FAIL pixel diagnostic source properties unavailable\n");
        controller->requestClose();
        return 1;
    }
    const auto capture = [&](int multiplier, const QString& mode, QImage* output) {
        window->resize(qMax(1, qRound(*width * multiplier)),
                       qMax(1, qRound(*height * multiplier)));
        pump(500);
        if (mode == QLatin1String("off")) session->setAnime4KMode(anime4k::Mode::Off);
        else session->setAnime4KMode(anime4k::Mode::FastA);
        const bool applied = mode == QLatin1String("off") ? chainSettlesOff(session, 6000)
                                                          : chainSettlesTo(session, QStringLiteral("fast_a"), 6000);
        if (!applied) return false;
        pump(700);
        const auto now = numberProperty(session, QStringLiteral("time-pos"));
        *output = window->grabVideoFrame();
        return now && std::abs(*now - *time) <= 0.05 && !output->isNull() &&
               output->save(QDir(directory).filePath(mode + QStringLiteral("_%1x.png").arg(multiplier)));
    };
    QImage off1, fast1, off2, fast2;
    bool ok = capture(1, QStringLiteral("off"), &off1) && capture(1, QStringLiteral("fast_a"), &fast1) &&
              capture(2, QStringLiteral("off"), &off2) && capture(2, QStringLiteral("fast_a"), &fast2);
    if (!ok) std::fprintf(stderr, "FAIL pixel diagnostic capture/refusal\n");
    const double one = bandDifference(off1, fast1);
    const double two = bandDifference(off2, fast2);
    std::fprintf(stderr, "pixel diagnostic framebuffer=%dx%d/%dx%d off-fast MAE 1x=%.4f 2x=%.4f chain=%s\n",
                 off1.width(), off1.height(), off2.width(), off2.height(), one, two,
                 qPrintable(session->effectiveShaderChain()));
    if (one < 0 || two < 0 || (one <= 0.05 && two <= 0.05)) {
        std::fprintf(stderr, "FAIL pixel diagnostic found no real fast_a effect on the fixture\n");
        ok = false;
    }
    if (qgetenv("MIKAI_ANIME4K_REFUSAL_SMOKE") == "1" && ok &&
        !runMissingStageRefusalSmoke(session, window, fast2)) {
        std::fprintf(stderr, "FAIL pixel diagnostic missing-stage refusal smoke\n");
        ok = false;
    }
    if (controller) controller->requestClose();
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiAnime4KProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("Anime4KMenuProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    if (!mplayer::probes::selectRuntime()) return 2;
    QSettings settings;
    settings.clear();
    settings.sync();
    const QByteArray pixelDirectory = qgetenv("MIKAI_ANIME4K_PIXEL_DIR");
    if (!pixelDirectory.isEmpty()) return runPixelDiagnostic(QString::fromLocal8Bit(pixelDirectory));

    const QString url = QStringLiteral("av://lavfi:testsrc2=size=480x270:rate=25");
    check(!anime4k::findShadersDir().isEmpty(), "the Anime4K shader folder is deployed beside the exe");
    QPointer<PlayerController> controller = createPlayer(url);
    if (!waitForVideo(controller)) {
        std::fprintf(stderr, "FAIL no rendered frame from lavfi testsrc2 in production Quick scene\n");
        if (controller) controller->requestClose();
        return 2;
    }
    PlayerWindow* window = controller->window();
    VideoSession* session = controller->session();
    check(session->videoWidth() == kSourceWidth && session->videoHeight() == kSourceHeight,
          "a real 480x270 source is playing through the production session");

    session->setAnime4KMode(anime4k::Mode::Off);
    check(chainSettlesOff(session), "Disabled leaves mpv with no shader chain at all");
    session->setPaused(true);
    pump(400);
    const QImage offA = settledFrame(window);
    const QImage offB = settledFrame(window);
    const double noise = bandDifference(offA, offB);
    std::fprintf(stderr, "     control: two settled Off frames differ by %.4f / 255\n", noise);
    check(!offA.isNull() && noise >= 0.0, "a real Quick video framebuffer is readable while paused");
    check(noise <= 0.01, "the paused picture is stable between grabs, so the floor is ~zero");
    saveFrame(offA, QStringLiteral("anime4k-menu-off.png"));

    emit controller->menuOpenRequested(QStringLiteral("enhancements"));
    check(waitUntil([&] { return menuPanel(window, QStringLiteral("enhancements")) != nullptr; }, 3000),
          "the real Quick enhancement menu opens");
    check(clickRow(window, QStringLiteral("enhancements"), QStringLiteral("anime4k"), true),
          "the Anime4K submenu is activated by the real QML row");
    check(waitUntil([&] { return menuPanel(window, QStringLiteral("anime4k")) != nullptr; }, 3000),
          "Anime4K presets appear in their production cascade");
    shoot(window, QStringLiteral("anime4k-menu-root.png"));
    check(clickRow(window, QStringLiteral("anime4k"), QStringLiteral("anime4k/speed"), true),
          "the speed category opens from the Quick menu");
    check(session->effectiveShaderChain().isEmpty() &&
              controller->anime4kChainId() == QLatin1String("off"),
          "opening a Quick preset category does not apply a shader");
    check(waitUntil([&] { return menuPanel(window, QStringLiteral("anime4k/speed")) != nullptr; }, 3000),
          "the speed category presents its child panel");
    PlayerMenuModel* animeRootModel = controller->menuModel(QStringLiteral("anime4k"));
    const int speedRow = animeRootModel != nullptr
                             ? animeRootModel->indexOfId(QStringLiteral("anime4k/speed"))
                             : -1;
    PlayerMenuModel* speedModel = speedRow >= 0
                                      ? controller->submenuModel(QStringLiteral("anime4k"), speedRow)
                                      : nullptr;
    check(speedModel != nullptr && speedModel->indexOfId(QStringLiteral("fast_a")) >= 0 &&
              speedModel->indexOfId(QStringLiteral("fast_c")) >= 0,
          "the production speed model contains its catalogue leaves");
    check(clickRow(window, QStringLiteral("anime4k"), QStringLiteral("anime4k/experimental"), true),
          "the experimental category opens from its real Quick row");
    check(waitUntil([&] { return menuPanel(window, QStringLiteral("anime4k/experimental")) != nullptr; }, 3000),
          "the experimental category presents its child panel");
    shoot(window, QStringLiteral("anime4k-menu-experiments.png"));
    check(clickRow(window, QStringLiteral("anime4k/experimental"),
                   QStringLiteral("anime4k/experimental/noise"), true),
          "the experiment family opens from its real Quick row");
    check(waitUntil([&] { return menuPanel(window, QStringLiteral("anime4k/experimental/noise")) != nullptr; }, 3000),
          "the family panel is presented");
    shoot(window, QStringLiteral("anime4k-menu-family.png"));

    const auto choose = [&](const QString& id) {
        QString category;
        for (const anime4k::ModeInfo& info : anime4k::availableModes()) {
            if (info.id == id) { category = info.category; break; }
        }
        return clickLeaf(category, id, controller, window);
    };
    check(choose(QLatin1String(kExperiment)), "the experimental preset is picked through Quick menus");
    check(waitUntil([&] { return controller->anime4kChainId() == QLatin1String(kExperiment); }, 4000),
          "the chosen menu action changes the controller's active preset id");
    check(chainSettlesTo(session, QLatin1String(kExperiment)),
          "mpv runs the experiment's ordered shader chain, stage for stage");
    settings.sync();
    check(settings.value(QStringLiteral("player/anime4k")).toString() == QLatin1String(kExperiment),
          "the selected experiment persists under its stable id");
    QString experimentLabel;
    for (const anime4k::ModeInfo& info : anime4k::availableModes()) {
        if (info.id == QLatin1String(kExperiment)) experimentLabel = info.label;
    }
    check(!experimentLabel.isEmpty() &&
              controller->enhancementChipText().contains(experimentLabel),
          "the production enhancement chip names the selected catalogue preset");
    const QImage on = settledFrame(window);
    const double effect = bandDifference(offA, on);
    const double threshold = std::max(0.05, noise * 5.0);
    std::fprintf(stderr, "     experiment vs Off over the picture band: %.3f / 255 (floor %.4f, threshold %.4f)\n",
                 effect, noise, threshold);
    check(effect > threshold, "the shader changes actual rendered pixels beyond measured noise");
    saveFrame(on, QStringLiteral("anime4k-menu-experiment.png"));

    for (const anime4k::ModeInfo& info : anime4k::availableModes()) {
        if (!choose(info.id)) {
            check(false, (QStringLiteral("Quick menu leaf is clickable: ") + info.id).toUtf8().constData());
            continue;
        }
        check(waitUntil([&] { return controller->anime4kChainId() == info.id; }, 4000),
              (QStringLiteral("menu applies exact preset id: ") + info.id).toUtf8().constData());
        check(session->anime4KChainId() == info.id &&
                  session->anime4KMode() == anime4k::modeForId(info.id),
              (QStringLiteral("one selected preset id maps to its matching mode: ") + info.id)
                  .toUtf8().constData());
        check(chainSettlesTo(session, info.id),
              (QStringLiteral("mpv runs ordered preset chain: ") + info.id).toUtf8().constData());
Need tool write commentary. 
    }

    check(choose(QLatin1String(kExperiment)) && chainSettlesTo(session, QLatin1String(kExperiment)),
          "the experiment is selected again before Compare and PiP");
    const QString chainBeforeCompare = session->effectiveShaderChain();
    const QString idBeforeCompare = controller->anime4kChainId();
    controller->openCompare();
    pump(300);
    CompareWindow* compare = controller->findChild<CompareWindow*>();
    check(compare != nullptr && compare->isVisible(), "the controller opens the legitimate Compare window");
    if (compare != nullptr) {
        const QString compareShot = QDir::current().filePath(
            QStringLiteral("build-cmake/anime4k-menu-compare.png"));
        check(QDir().mkpath(QFileInfo(compareShot).absolutePath()) &&
                  compare->grab().save(compareShot),
              "screenshot written: anime4k-menu-compare.png");
        const QImage frozen = compare->frozenFrame();
        check(!frozen.isNull(), "Compare froze a real player frame");
        check(bandRange(frozen) > 32, "the frozen frame contains a decoded nonuniform picture");
        QToolButton* badge = nullptr;
        for (QToolButton* button : compare->findChildren<QToolButton*>()) {
            if (button->menu() != nullptr) { badge = button; break; }
        }
        check(badge != nullptr, "Compare's real candidate chooser is available");
        if (badge != nullptr) {
            const QStringList offered = chooserLeafIds(badge->menu()->actions());
            QStringList expected;
            QHash<QString, QStringList> expectedPaths;
            const QString restoreLabel =
                QCoreApplication::translate("QObject", "Restore (Anime4K)");
            for (const anime4k::ModeInfo& info : anime4k::availableModes()) {
                expected << info.id;
                QStringList path{restoreLabel};
                QStringList categoryPath = chooserPathFor(info.category);
                if (categoryPath.isEmpty()) categoryPath << QCoreApplication::translate(
                    "CompareWindow", "Presets");
                path.append(categoryPath);
                expectedPaths.insert(info.id, path);
            }
            for (const native::AnimeJanaiPreset& preset : native::animeJanaiPresets()) {
                if (!preset.rife && preset.id.startsWith(QLatin1String("spatial:"))) {
                    expected << preset.id;
                    expectedPaths.insert(preset.id,
                                         {QStringLiteral("AnimeJaNai"),
                                          QStringLiteral("Spatial models")});
                }
            }
            QStringList a = offered, b = expected;
            a.sort(); b.sort();
            const bool unique = std::adjacent_find(a.cbegin(), a.cend()) == a.cend() &&
                                std::adjacent_find(b.cbegin(), b.cend()) == b.cend();
            check(unique && a == b,
                  "Compare offers every shader preset and public spatial model only, once each");
            bool nonPublic = false;
            for (const native::AnimeJanaiPreset& preset : native::animeJanaiPresets()) {
                if (preset.rife || !preset.id.startsWith(QLatin1String("spatial:"))) {
                    nonPublic |= offered.contains(preset.id);
                }
            }
            check(!nonPublic, "Compare excludes RIFE and non-public AnimeJaNai entries");
            check(chooserPlacementsMatch(badge->menu(), {}, expectedPaths),
                  "Compare nests every preset under its catalogue category");
            check(countActionText(badge->menu()->actions(),
                                  QStringLiteral("AutoDownscalePre")) == 0,
                  "Compare never offers chain-internal AutoDownscalePre stages");
            QMenu* pickLevel = nullptr;
            QAction* hqA = chooserAction(badge->menu(), QStringLiteral("hq_a"), &pickLevel);
            check(hqA != nullptr && pickLevel != nullptr,
                  "Compare's chooser includes a real preset action");
            if (hqA != nullptr && pickLevel != nullptr) {
                const QList<QAction*> actionsBefore = pickLevel->actions();
                hqA->trigger();
                QCoreApplication::processEvents();
                check(pickLevel->actions() == actionsBefore,
                      "Compare reuses its cached chooser tree after a selection");
                check(hqA->isChecked(), "Compare marks the chosen preset in its real menu");
            }
            check(session->anime4KChainId() == idBeforeCompare &&
                      session->effectiveShaderChain() == chainBeforeCompare,
                  "Compare selection does not rewrite the player's active chain");
        }
        compare->close();
    }

    controller->togglePip();
    check(waitUntil([&] { return controller->isPipActive(); }, 2000),
          "PiP activates through the player's real control path");
    check(controller->window() == window && window->compactPip() && controller->session() == session,
          "PiP is the same Quick window and VideoSession, not another decoder");
    check(session->anime4KChainId() == QLatin1String(kExperiment) &&
              chainSettlesTo(session, QLatin1String(kExperiment)),
          "the active experiment remains applied in compact PiP");
    controller->togglePip();
    check(waitUntil([&] { return !controller->isPipActive(); }, 2000), "PiP exits through the same control");

    const QSize minimum = window->minimumSize();
    window->resize(minimum);
    pump(300);
    check(window->width() <= minimum.width() + 8 && window->height() <= minimum.height() + 8,
          "the production window fits its minimum size");
    emit controller->menuOpenRequested(QStringLiteral("enhancements"));
    check(clickRow(window, QStringLiteral("enhancements"), QStringLiteral("anime4k"), true),
          "Anime4K opens at minimum size");
    check(waitUntil([&] { return menuPanel(window, QStringLiteral("anime4k")) != nullptr; }, 2000),
          "the minimum-size Anime4K menu is visible");
    check(clickRow(window, QStringLiteral("anime4k"),
                   QStringLiteral("anime4k/experimental"), true),
          "the minimum-size experimental category opens");
    check(waitUntil([&] {
              return menuPanel(window, QStringLiteral("anime4k/experimental")) != nullptr;
          }, 2000), "the minimum-size experimental panel is visible");
    check(clickRow(window, QStringLiteral("anime4k/experimental"),
                   QStringLiteral("anime4k/experimental/noise"), true),
          "the minimum-size deepest family opens");
    QQuickItem* deepest = menuPanel(window, QStringLiteral("anime4k/experimental/noise"));
    const QRectF deepestRect = deepest != nullptr
                                   ? deepest->mapRectToScene(QRectF(0, 0, deepest->width(),
                                                                    deepest->height()))
                                   : QRectF();
    check(deepest != nullptr && QRectF(0, 0, window->width(), window->height())
                                        .contains(deepestRect),
          "the deepest Quick menu level fits inside the minimum-sized window");

    check(settings.value(QStringLiteral("player/anime4k")).toString() == QLatin1String(kExperiment),
          "the experiment remains stored before reopening the player");
    check(closePlayer(controller), "the acknowledged player close completes before reopen");
    controller = createPlayer(url);
    if (!waitForVideo(controller)) {
        std::fprintf(stderr, "FAIL reopened production player did not render\n");
        if (controller) controller->requestClose();
        return 4;
    }
    window = controller->window();
    session = controller->session();
    check(controller->anime4kChainId() == QLatin1String(kExperiment),
          "the reopened player restores the experiment's chain by id");
    check(chainSettlesTo(session, QLatin1String(kExperiment)),
          "the reopened player applies the persisted ordered chain");
    check(anime4k::modeId(session->anime4KMode()) == QLatin1String(kExperiment),
          "the reopened player restores the persisted mode as well as the chain id");
    session->setPaused(true);
    const QImage restoredOn = settledFrame(window);
    emit controller->menuOpenRequested(QStringLiteral("enhancements"));
    check(clickRow(window, QStringLiteral("enhancements"), QStringLiteral("anime4k"), true),
          "the reopened enhancement menu opens its Anime4K level");
    check(clickRow(window, QStringLiteral("anime4k"),
                   QStringLiteral("anime4k/experimental"), true),
          "the reopened experimental level opens");
    PlayerMenuModel* animeModel = controller->menuModel(QStringLiteral("anime4k"));
    const int experimentalRow = animeModel != nullptr
                                    ? animeModel->indexOfId(QStringLiteral("anime4k/experimental"))
                                    : -1;
    PlayerMenuModel* experimentModel = experimentalRow >= 0
                                           ? controller->submenuModel(QStringLiteral("anime4k"),
                                                                      experimentalRow)
                                           : nullptr;
    check(experimentModel != nullptr &&
              experimentModel->selectedId() == QLatin1String(kExperiment),
          "the reopened Quick menu marks the restored experiment as selected");
    const QImage restoredAgain = settledFrame(window);
    const double restoredNoise = bandDifference(restoredOn, restoredAgain);
    check(!restoredOn.isNull() && restoredNoise >= 0.0 && restoredNoise <= 0.01,
          "the reopened player holds a stable frame with the restored chain");
    check(choose(QStringLiteral("off")), "Disabled is selected through the reopened Quick menu");
    check(chainSettlesOff(session), "Disabled clears every active shader path");
    const QImage restoredOff = settledFrame(window);
    const double restoredEffect = bandDifference(restoredOn, restoredOff);
    const double restoredThreshold = std::max(0.05, restoredNoise * 5.0);
    std::fprintf(stderr, "     restored experiment vs Off: %.3f / 255 (floor %.4f, threshold %.4f)\n",
                 restoredEffect, restoredNoise, restoredThreshold);
    check(restoredEffect > restoredThreshold, "the restored experiment was really rendering");

    check(closePlayer(controller), "the reopened player closes cleanly");
    settings.clear();
    settings.sync();
    std::fprintf(stderr, "RESULT: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
