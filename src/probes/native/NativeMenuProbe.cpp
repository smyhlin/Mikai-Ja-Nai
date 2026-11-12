// Native/Anime4K menu probe against the production Qt Quick player.
// Run from the repo root with shader assets deployed beside the executable:
//   build-cmake/native_menu_probe.exe

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QQuickItem>
#include <QQuickWindow>

#include <cstdio>

#include "ProbeUi.hpp"
#include "PlayerController.hpp"
#include "PlayerMenuModel.hpp"
#include "PlayerWindow.hpp"
#include "native/bundle/NativePrefs.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "native/status/NativeMenu.hpp"
#include "core/VideoSession.hpp"

using namespace mplayer;
using namespace mplayer::quick;

namespace {
int failures = 0;

void check(bool ok, const QString& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) ++failures;
}

struct FakeHost final : shell::PlayerHost {
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    void playbackFailed() override {}
};

bool waitForVideo(VideoSession* session, int timeoutMs = 15000) {
    for (int waited = 0; waited < timeoutMs; waited += 40) {
        QCoreApplication::processEvents();
        if (session != nullptr && session->hasVideo()) return true;
        QThread::msleep(40);
    }
    return session != nullptr && session->hasVideo();
}

bool clockAdvanced(VideoSession* session, double from, int timeoutMs = 4000) {
    for (int waited = 0; waited < timeoutMs; waited += 40) {
        QCoreApplication::processEvents();
        if (session != nullptr && session->position() > from) return true;
        QThread::msleep(40);
    }
    return session != nullptr && session->position() > from;
}

bool openMenu(PlayerController* controller, const QString& menuId) {
    PlayerWindow* window = controller != nullptr ? controller->window() : nullptr;
    QQuickItem* root = window != nullptr ? window->rootObject() : nullptr;
    if (root == nullptr || controller == nullptr) return false;
    const QRectF anchor(80, 80, 1, 1);
    const bool invoked = QMetaObject::invokeMethod(root, "openMenuAt", Qt::DirectConnection,
                                                    Q_ARG(QString, menuId), Q_ARG(QRectF, anchor));
    if (!invoked) return false;

    PlayerMenuModel* expectedModel = controller->menuModel(menuId);
    for (int waited = 0; waited < 2000; waited += 20) {
        QCoreApplication::processEvents();
        if (!controller->menuStackVisible() || expectedModel == nullptr) {
            QThread::msleep(20);
            continue;
        }
        for (QQuickItem* item : root->findChildren<QQuickItem*>()) {
            if (item->property("menuId").toString() == menuId && item->isVisible() &&
                item->property("menuModel").value<QObject*>() == expectedModel) {
                return true;
            }
        }
        QThread::msleep(20);
    }
    return false;
}

int rowForValue(const PlayerMenuModel* model, const QString& value) {
    if (model == nullptr) return -1;
    const auto& rows = model->rows();
    for (int i = 0; i < rows.size(); ++i) {
        if (rows.at(i).value == value) return i;
    }
    return -1;
}

bool writeMenuShot(PlayerWindow* window) {
    if (window == nullptr || window->rootObject() == nullptr) return false;
    const QImage image = window->grabWindow();
    const QString path = QDir::current().filePath(QStringLiteral("build-cmake/native_menu_probe.png"));
    return !image.isNull() && image.width() >= 80 && image.height() >= 40 &&
           QDir().mkpath(QFileInfo(path).absolutePath()) && image.save(path);
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
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings settings;
    settings.clear();
    settings.sync();

    const native::RuntimeSelection bootstrap = native::selectStandardRuntime();
    check(bootstrap.kind == native::BundleKind::Standard,
          "the process is bootstrapped on the Standard runtime");
    check(native::currentSelection().kind == native::BundleKind::Standard,
          "the Standard runtime is the recorded selection");

    FakeHost host;
    PlayerController controller(QStringLiteral("Native menu probe"),
        QStringLiteral("av://lavfi:testsrc2=size=320x180:rate=25"), QList<shell::QualityEntry>{},
        0, &host);
    PlayerWindow* window = controller.window();
    VideoSession* session = controller.session();
    bool mediaError = false;
    if (session != nullptr) {
        QObject::connect(session, &VideoSession::errorOccurred, &controller,
                         [&mediaError](const QString&) { mediaError = true; });
    }
    controller.startPlayer();
    controller.showWindow();
    check(window != nullptr && window->rootObject() != nullptr,
          "the native Quick player scene loads through its executable module");
    check(waitForVideo(session), "the menu probe's own stream produces a real picture");
    if (session == nullptr || !session->hasVideo()) {
        std::fprintf(stderr, "FAIL no render context: lavfi testsrc2 did not load\n");
        return 2;
    }
    const double menuStart = session->position();

    // Open the real scene's cascading menu and inspect its controller-owned model.
    check(openMenu(&controller, QStringLiteral("enhancements")),
          "the enhancement menu opens in the Quick scene");
    PlayerMenuModel* root = controller.menuModel(QStringLiteral("enhancements"));
    check(root != nullptr, "the enhancement menu has its typed model");
    const int anime4kRow = rowForValue(root, QStringLiteral("anime4k"));
    const int animeJanaiRow = rowForValue(root, QStringLiteral("animejanai"));
    check(anime4kRow >= 0, "the Standard enhancement root offers Anime4K");
    check(animeJanaiRow >= 0, "the Standard enhancement root offers official AnimeJaNai");
    check(rowForValue(root, QStringLiteral("native")) < 0 &&
              rowForValue(root, QStringLiteral("rife")) < 0,
          "the Standard root omits private Native and RIFE branches");
    check(writeMenuShot(window), "screenshot written: native_menu_probe.png");

    // Subtitles is a separate menu level in the same scene, not a QWidget
    // popup. Delay controls and the external-track action remain available
    // even when the source has no subtitle tracks.
    check(openMenu(&controller, QStringLiteral("subtitles")),
          "the subtitle track level opens in the Quick scene");
    PlayerMenuModel* subtitles = controller.subtitlesMenu();
    bool hasDelayEarlier = false;
    bool hasDelayLater = false;
    bool hasDelayReset = false;
    bool hasExternalSubtitles = false;
    if (subtitles != nullptr) {
        for (const PlayerMenuModel::Row& row : subtitles->rows()) {
            hasDelayEarlier = hasDelayEarlier ||
                (row.action == PlayerMenuModel::Action::SetSubtitleDelay &&
                 row.id == QStringLiteral("delay.-0.1"));
            hasDelayLater = hasDelayLater ||
                (row.action == PlayerMenuModel::Action::SetSubtitleDelay &&
                 row.id == QStringLiteral("delay.0.1"));
            hasDelayReset = hasDelayReset ||
                (row.action == PlayerMenuModel::Action::ResetSubtitleDelay &&
                 row.id == QStringLiteral("delay.reset"));
            hasExternalSubtitles = hasExternalSubtitles ||
                (row.action == PlayerMenuModel::Action::LoadExternalTrack &&
                 row.value == QStringLiteral("sub"));
        }
    }
    check(hasDelayEarlier && hasDelayLater && hasDelayReset,
          "the subtitle level exposes earlier, later, and reset delay rows");
    check(hasExternalSubtitles,
          "the subtitle level retains its external-track action without subtitle tracks");
    const int pictureSubtitlesRow = rowForValue(controller.menuModel(QStringLiteral("picture")),
                                                QStringLiteral("picture.subtitles"));
    PlayerMenuModel* pictureSubtitles = pictureSubtitlesRow >= 0
        ? controller.submenuModel(QStringLiteral("picture"), pictureSubtitlesRow) : nullptr;
    bool hasDiskSubtitles = false;
    bool hasOnlineSubtitles = false;
    if (pictureSubtitles != nullptr) {
        for (const PlayerMenuModel::Row& row : pictureSubtitles->rows()) {
            hasDiskSubtitles = hasDiskSubtitles ||
                (row.action == PlayerMenuModel::Action::LoadExternalTrack &&
                 row.value == QStringLiteral("sub"));
            hasOnlineSubtitles = hasOnlineSubtitles ||
                row.action == PlayerMenuModel::Action::OpenSubtitleSearch;
        }
    }
    check(hasDiskSubtitles && hasOnlineSubtitles,
          "the picture menu's subtitle submenu offers disk and OpenSubtitles routes");

    // The native catalogue itself remains honest about an uninstalled provider:
    // the backend row is disabled with its explanation, while the current mark
    // comes from the stored request rather than menu hover state.
    native::NativePreferencePatch request;
    request.enabled = true;
    request.backend = native::Backend::DirectML;
    request.recipeId = QStringLiteral("user:preserved");
    QString saveReason;
    check(native::saveNativePreferences(request, &saveReason),
          QStringLiteral("a private Native request is stored: ") + saveReason);
    native::menu::Inputs nativeInputs;
    nativeInputs.preferences = native::loadNativePreferences();
    const QVector<native::menu::Row> backendRows = native::menu::nativeRows(nativeInputs);
    bool disabledBackendExplained = false;
    for (const native::menu::Row& row : backendRows) {
        if (row.id.contains(QStringLiteral("backend"), Qt::CaseInsensitive) && row.disabled &&
            !row.tooltip.isEmpty()) disabledBackendExplained = true;
    }
    check(disabledBackendExplained,
          "an unavailable native backend is disabled with an explanatory tooltip");
    const QString currentNativeRow = native::menu::currentRowId(nativeInputs.preferences);
    bool nativeSelectionMatchesRequest = false;
    for (const native::menu::Row& row : backendRows) {
        if (row.current && row.id == currentNativeRow) nativeSelectionMatchesRequest = true;
    }
    check(nativeSelectionMatchesRequest,
          "the native menu selection mark reflects the stored engine request");

    const native::NativePreferences saved = native::loadNativePreferences();
    // A Standard controller cannot route private Native actions. The typed model
    // is the authority and contains no such selectable action in this runtime.
    bool hasNativeAction = false;
    if (root != nullptr) {
        for (const PlayerMenuModel::Row& row : root->rows()) {
            hasNativeAction = hasNativeAction ||
                (row.action == PlayerMenuModel::Action::SetEnhancement &&
                 row.value.startsWith(QStringLiteral("native|")));
        }
    }
    check(!hasNativeAction, "Standard model exposes no private Native action");
    check(native::loadNativePreferences().enabled == saved.enabled &&
              native::loadNativePreferences().backend == saved.backend &&
              native::loadNativePreferences().recipeId == saved.recipeId,
          "a Standard menu leaves every saved Native request field untouched");

    // Select a real Anime4K catalogue entry through the typed model action. Its
    // engine-selected mark is published by a model rebuild; merely opening or
    // hovering the menu cannot change it.
    PlayerMenuModel* anime4kModel = controller.submenuModel(QStringLiteral("enhancements"), anime4kRow);
    check(anime4kModel != nullptr, "Anime4K submenu resolves to a cached typed model");
    int selectedBefore = -1;
    int presetRow = -1;
    if (anime4kModel != nullptr) {
        for (int i = 0; i < anime4kModel->rows().size(); ++i) {
            const auto& row = anime4kModel->rows().at(i);
            if (row.selected) selectedBefore = i;
            if (presetRow < 0 && row.action == PlayerMenuModel::Action::SetEnhancement &&
                row.value.startsWith(QStringLiteral("anime4k|")) && row.enabled) presetRow = i;
        }
    }
    check(presetRow >= 0, "the Anime4K model contains a real selectable preset");
    if (anime4kModel != nullptr && presetRow >= 0) {
        controller.activateMenuRow(QStringLiteral("enhancements"), anime4kRow);
        QCoreApplication::processEvents();
        PlayerMenuModel* opened = controller.submenuModel(QStringLiteral("enhancements"), anime4kRow);
        int selectedAfterOpen = -1;
        if (opened != nullptr) {
            for (int i = 0; i < opened->rows().size(); ++i) {
                if (opened->rows().at(i).selected) selectedAfterOpen = i;
            }
        }
        check(selectedAfterOpen == selectedBefore,
              "opening the submenu does not move the engine-selected mark");
        controller.activateMenuRow(QStringLiteral("anime4k"), presetRow);
        QCoreApplication::processEvents();
        PlayerMenuModel* rebuilt = controller.submenuModel(QStringLiteral("enhancements"), anime4kRow);
        bool selectedAppliedMode = false;
        if (rebuilt != nullptr) {
            for (const PlayerMenuModel::Row& row : rebuilt->rows()) {
                if (row.selected && row.id == anime4kModel->row(presetRow).id) selectedAppliedMode = true;
            }
        }
        check(selectedAppliedMode, "the selected marker follows the applied engine preset");
        check(controller.session()->anime4KMode() != mplayer::anime4k::Mode::Off,
              "the menu action applies a real Anime4K engine mode");
    }
    check(session->hasVideo() && !mediaError && !controller.retryVisible(),
          "the Quick enhancement menu leaves the Original picture up with no retry state");
    check(clockAdvanced(session, menuStart), "the native menu never stopped the stream clock");

    controller.requestClose();
    if (failures == 0) std::printf("OK native_menu_probe\n");
    else std::fprintf(stderr, "FAILURES %d\n", failures);
    return failures == 0 ? 0 : 1;
}
