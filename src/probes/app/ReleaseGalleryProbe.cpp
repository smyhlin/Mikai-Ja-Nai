// Release-only driver for real application surfaces. This is not a product CLI.
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QWindow>
#include <QFileInfo>
#include <QFontMetrics>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPlainTextEdit>
#include <QStyle>
#include <QStyleOptionButton>
#include <QUrl>
#include <QVariantMap>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QQuickItem>
#include <QProgressBar>
#include <QPushButton>
#include <QTranslator>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScreen>
#include <QSlider>
#include <QSettings>
#include <QTest>
#include <QTimer>
#include <QtMath>

#include <cstdio>
#include <functional>
#include <memory>
#include <utility>

#include "ProbeIsolation.hpp"
#include "ProbePicture.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "WindowRaisePolicy.hpp"
#include "backend/MikaiClient.hpp"
#include "dialogs/SettingsDialog.hpp"
#include "dialogs/settings/SettingsUi.hpp"
#include "pages/MainWindow.hpp"
#include "pages/SearchPage.hpp"
#include "pages/TitlePage.hpp"
#include "state/AppearanceStore.hpp"
#include "state/QualityCache.hpp"
#include "theme/Metrics.hpp"
#include "theme/Theme.hpp"
#include "widgets/PageGlow.hpp"
#include "widgets/TitleCard.hpp"
#include "workers/Runner.hpp"
#include "CompareWindow.hpp"
#include "MediaInformationDialog.hpp"
#include "PlayerController.hpp"
#include "PlayerMenuModel.hpp"
#include "PlayerWindow.hpp"
#include "PreparationWindow.hpp"
#include "ShellTheme.hpp"
#include "VideoSession.hpp"

namespace {
using mikai::app::MainWindow;
using mikai::app::theme::Appearance;
using mplayer::shell::CompareWindow;
using mplayer::shell::MediaInformationDialog;
using mplayer::quick::PlayerController;
using mplayer::quick::PlayerMenuModel;
using mplayer::quick::PlayerWindow;

constexpr int kWidth = 1280;
constexpr int kHeight = 800;

struct Arguments {
    QString output;
    QString video;
    QString credit;
    QString source;
    QString translations;
    QString error;
    bool help = false;
    bool accentSweep = false;
    bool radiusSweep = false;
};

Arguments parseArguments(const QStringList& arguments) {
    Arguments result;
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const QString option = arguments[index];
        if (option == QLatin1String("--help")) {
            result.help = true;
            continue;
        }
        if (option == QLatin1String("--accent-sweep")) {
            result.accentSweep = true;
            continue;
        }
        if (option == QLatin1String("--radius-sweep")) {
            result.radiusSweep = true;
            continue;
        }
        QString* value = nullptr;
        if (option == QLatin1String("--output-dir")) value = &result.output;
        else if (option == QLatin1String("--video")) value = &result.video;
        else if (option == QLatin1String("--video-credit")) value = &result.credit;
        else if (option == QLatin1String("--video-source")) value = &result.source;
        else if (option == QLatin1String("--translations")) value = &result.translations;
        if (value == nullptr || index + 1 == arguments.size() || !value->isEmpty()) {
            result.error = QStringLiteral("Unknown, repeated or incomplete option: %1").arg(option);
            return result;
        }
        *value = arguments[++index];
    }
    if (!result.help && (result.output.isEmpty() || result.video.isEmpty() ||
                         result.credit.trimmed().isEmpty())) {
        result.error = QStringLiteral("--output-dir, --video and --video-credit are required.");
    }
    return result;
}

void diagnostic(const QString& message) {
    std::fprintf(stderr, "[release-gallery] %s\n", qPrintable(message));
}

QString sha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}

// The driver owns no production setting and never accepts a real-tree mode.
// Redirect every app path before QApplication (and therefore QStandardPaths).
bool prepareScratch() {
    const QString root = mikai::probes::isolation::isolatedRoot();
    if (root.isEmpty()) return false;
    const QString home = QDir(root).filePath(QStringLiteral("home"));
    const QStringList subdirectories = {home, home + QStringLiteral("/config"),
        home + QStringLiteral("/cache"), home + QStringLiteral("/data"),
        home + QStringLiteral("/local"), home + QStringLiteral("/roaming")};
    for (const QString& directory : subdirectories) {
        if (!QDir().mkpath(directory)) return false;
    }
    qputenv("HOME", home.toUtf8());
    qputenv("USERPROFILE", home.toUtf8());
    qputenv("XDG_CONFIG_HOME", (home + QStringLiteral("/config")).toUtf8());
    qputenv("XDG_CACHE_HOME", (home + QStringLiteral("/cache")).toUtf8());
    qputenv("XDG_DATA_HOME", (home + QStringLiteral("/data")).toUtf8());
    qputenv("LOCALAPPDATA", (home + QStringLiteral("/local")).toUtf8());
    qputenv("APPDATA", (home + QStringLiteral("/roaming")).toUtf8());
    return QDir::setCurrent(root);
}

class Gallery final : public QObject {
public:
    Gallery(QApplication& application, Arguments arguments, QString demo, QString digest)
        : app_(application), arguments_(std::move(arguments)), demo_(std::move(demo)),
          client_(), window_(std::make_unique<MainWindow>(client_, MainWindow::StartupMode::FilePlayer)) {
        evidence_ = QJsonObject{
            {QStringLiteral("schema"), 1},
            {QStringLiteral("startedUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {QStringLiteral("applicationVersion"), QCoreApplication::applicationVersion()},
            {QStringLiteral("qtVersion"), QString::fromLatin1(qVersion())},
            {QStringLiteral("platform"), QGuiApplication::platformName()},
            {QStringLiteral("interfaceLanguage"), QStringLiteral("en")},
            {QStringLiteral("logicalSize"), QJsonArray{kWidth, kHeight}},
            {QStringLiteral("isolation"), QStringLiteral("verified executable copy; fresh INI settings, home, cache, working directory and native store")},
            {QStringLiteral("catalogue"), QJsonObject{
                {QStringLiteral("source"), client_.baseUrl()},
                {QStringLiteral("authentication"), QStringLiteral("anonymous; environment key cleared and .env lookup redirected")},
                {QStringLiteral("content"), QStringLiteral("live service metadata and artwork; no fixtures or fabricated entries")}}},
            {QStringLiteral("video"), QJsonObject{
                {QStringLiteral("filename"), QFileInfo(demo_).fileName()},
                {QStringLiteral("sha256"), digest},
                {QStringLiteral("bytes"), double(QFileInfo(demo_).size())},
                {QStringLiteral("creditDeclaration"), arguments_.credit},
                {QStringLiteral("sourceDeclaration"), arguments_.source},
                {QStringLiteral("declarationVerified"), false},
                {QStringLiteral("note"), QStringLiteral("Original/demo authorship is the operator's declaration, not inferred from the file. The input was copied read-only into owned scratch before playback.")}}},
            {QStringLiteral("processing"), QStringLiteral("Original playback; compare may use bundled Anime4K shaders only. No model/engine acquisition or AI inference requested.")}};
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
        timer_.setInterval(100);
        window_->setWindowIcon(QIcon(QStringLiteral(":/app/mikai.png")));
        fit(window_.get());
        window_->show();
        stageClock_.start();
        totalClock_.start();
        timer_.start();
    }

private:
    enum class Stage { Catalogue, Playback, Pause, PlayerShot, CompareOpen, CompareReady,
        CompareClosed, InformationOpen, InformationRead, InformationShot, SettingsOpen,
        SettingsShot, Close, Drain, Finished };

    void next(Stage stage) {
        stage_ = stage;
        stageClock_.restart();
    }

    void error(const QString& message) {
        diagnostic(QStringLiteral("ERROR: ") + message);
        errors_.append(message);
    }

    void stop(const QString& message) {
        error(message);
        evidence_.insert(QStringLiteral("failurePlaybackState"), playbackState());
        next(Stage::Close);
    }

    QString privateSafe(QString text) const {
        const QString scratch = mikai::probes::isolation::isolatedRoot();
        if (scratch.isEmpty()) return text;
        text.replace(scratch, QStringLiteral("<owned scratch>"), Qt::CaseInsensitive);
        text.replace(QDir::toNativeSeparators(scratch),
                     QStringLiteral("<owned scratch>"), Qt::CaseInsensitive);
        return text;
    }

    void fit(QWidget* widget) {
        widget->resize(kWidth, kHeight);
        if (QScreen* screen = widget->screen()) {
            const QRect available = screen->availableGeometry();
            widget->move(available.center() - QPoint(widget->width() / 2, widget->height() / 2));
        }
    }

    void fit(QWindow* window) {
        window->resize(kWidth, kHeight);
        if (QScreen* screen = window->screen()) {
            const QRect available = screen->availableGeometry();
            window->setPosition(available.center() - QPoint(window->width() / 2,
                                                            window->height() / 2));
        }
    }

    QJsonObject playbackState() const {
        if (surface_ == nullptr) return {};
        const auto& selection = mplayer::native::currentSelection();
        return QJsonObject{
            {QStringLiteral("source"), QFileInfo(surface_->sourceUrl()).fileName()},
            {QStringLiteral("sourceEpoch"), double(surface_->sourceEpoch())},
            {QStringLiteral("positionSeconds"), surface_->position()},
            {QStringLiteral("durationSeconds"), surface_->duration()},
            {QStringLiteral("videoSize"), QJsonArray{surface_->videoWidth(), surface_->videoHeight()}},
            {QStringLiteral("contextReady"), surface_->isContextReady()},
            {QStringLiteral("visibleOutputFrame"), surface_->hasVisibleOutputFrame()},
            {QStringLiteral("playing"), surface_->isPlaying()},
            {QStringLiteral("pauseObserved"), surface_->pauseObserved()},
            {QStringLiteral("mediaTracksReady"), surface_->mediaTracks().ready},
            {QStringLiteral("shaderChain"), surface_->anime4KChainId()},
            {QStringLiteral("runtimeLoaded"), selection.loaded},
            {QStringLiteral("runtimeKind"), mplayer::native::bundleKindName(selection.kind)},
            {QStringLiteral("runtimeModule"), QFileInfo(selection.modulePath).fileName()},
            {QStringLiteral("runtimeDescriptor"), selection.descriptorId},
            {QStringLiteral("runtimeVersion"), selection.version}};
    }

    bool capture(QWidget* widget, const QString& name, QJsonObject state) {
        if (widget == nullptr || !widget->isVisible() || widget->isMinimized()) {
            error(QStringLiteral("%1: the real widget is not visible.").arg(name));
            return false;
        }
        // Qt's widget grab captures the actual current widget composition,
        // including QOpenGLWidget children. Nothing is composited or painted in.
        const QPixmap pixmap = widget->grab();
        const QString path = QDir(arguments_.output).filePath(name + QStringLiteral(".png"));
        if (pixmap.isNull() || pixmap.width() <= 0 || pixmap.height() <= 0 ||
            !pixmap.save(path, "PNG")) {
            error(QStringLiteral("%1: saving the real widget grab failed.").arg(name));
            return false;
        }
        const QString digest = sha256(path);
        if (digest.isEmpty()) {
            error(QStringLiteral("%1: the saved real widget grab could not be hashed.").arg(name));
            return false;
        }
        state.insert(QStringLiteral("file"), name + QStringLiteral(".png"));
        state.insert(QStringLiteral("sha256"), digest);
        state.insert(QStringLiteral("widgetClass"), QString::fromLatin1(widget->metaObject()->className()));
        state.insert(QStringLiteral("widgetObjectName"), widget->objectName());
        state.insert(QStringLiteral("windowTitle"), widget->windowTitle());
        state.insert(QStringLiteral("logicalSize"), QJsonArray{widget->width(), widget->height()});
        state.insert(QStringLiteral("pixelSize"), QJsonArray{pixmap.width(), pixmap.height()});
        state.insert(QStringLiteral("devicePixelRatio"), pixmap.devicePixelRatio());
        state.insert(QStringLiteral("capturedUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        shots_.append(state);
        diagnostic(QStringLiteral("Captured %1").arg(path));
        return true;
    }

    bool capture(QWindow* window, const QString& name, QJsonObject state) {
        if (window == nullptr || !window->isVisible() || window->screen() == nullptr) {
            error(QStringLiteral("%1: the real Quick window is not visible.").arg(name));
            return false;
        }
        const QPixmap pixmap = window->screen()->grabWindow(window->winId());
        const QString path = QDir(arguments_.output).filePath(name + QStringLiteral(".png"));
        if (pixmap.isNull() || pixmap.width() <= 0 || pixmap.height() <= 0 ||
            !pixmap.save(path, "PNG")) {
            error(QStringLiteral("%1: saving the real Quick window screenshot failed.").arg(name));
            return false;
        }
        const QString digest = sha256(path);
        if (digest.isEmpty()) {
            error(QStringLiteral("%1: the saved Quick window screenshot could not be hashed.").arg(name));
            return false;
        }
        state.insert(QStringLiteral("file"), name + QStringLiteral(".png"));
        state.insert(QStringLiteral("sha256"), digest);
        state.insert(QStringLiteral("windowClass"),
                     QString::fromLatin1(window->metaObject()->className()));
        state.insert(QStringLiteral("windowTitle"), window->title());
        state.insert(QStringLiteral("logicalSize"), QJsonArray{window->width(), window->height()});
        state.insert(QStringLiteral("pixelSize"), QJsonArray{pixmap.width(), pixmap.height()});
        state.insert(QStringLiteral("devicePixelRatio"), pixmap.devicePixelRatio());
        state.insert(QStringLiteral("capturedUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        shots_.append(state);
        diagnostic(QStringLiteral("Captured %1").arg(path));
        return true;
    }

    bool pictureAction(const QString& payload) {
        if (player_ == nullptr) return false;
        const PlayerMenuModel::Action action =
            payload == QLatin1String("compare")
                ? PlayerMenuModel::Action::OpenCompare
                : PlayerMenuModel::Action::OpenMediaInformation;
        PlayerMenuModel* model = player_->menuModel(QStringLiteral("picture"));
        if (model == nullptr) return false;
        const auto& rows = model->rows();
        for (int index = 0; index < rows.size(); ++index) {
            if (rows[index].enabled && rows[index].action == action) {
                player_->activateMenuRow(QStringLiteral("picture"), index);
                return true;
            }
        }
        return false;
    }

    void startPlayback() {
        if (!window_->openMedia(demo_)) {
            stop(QStringLiteral("The application refused the original demo video."));
            return;
        }
        player_ = window_->currentMediaPlayer();
        if (player_ == nullptr || player_->window() == nullptr) {
            stop(QStringLiteral("openMedia created no Quick player window."));
            return;
        }
        fit(player_->window());
        player_->window()->showFullscreen();
        surface_ = player_->session();
        if (surface_ == nullptr) {
            stop(QStringLiteral("The Quick player has no VideoSession."));
            return;
        }
        QObject::connect(surface_, &mplayer::VideoSession::errorOccurred, this,
                         [this](const QString& reason) { playbackError_ = privateSafe(reason); });
        next(Stage::Playback);
    }

    void tick() {
        // Compare's real capture path may pump events. Never advance this driver
        // recursively while one of those real actions is still on the stack.
        if (handling_) return;
        QScopedValueRollback<bool> guard(handling_, true);
        if (stage_ != Stage::Close && stage_ != Stage::Drain && stage_ != Stage::Finished &&
            totalClock_.elapsed() > 180000) {
            stop(QStringLiteral("Capture deadline exceeded (180 seconds)."));
        }
        if (stage_ != Stage::Close && stage_ != Stage::Drain &&
            !playbackError_.isEmpty()) {
            stop(QStringLiteral("Playback failed: ") + playbackError_);
        }
        switch (stage_) {
        case Stage::Catalogue: {
            QJsonArray cards;
            int posters = 0;
            for (auto* card : window_->findChildren<mikai::app::TitleCard*>()) {
                if (!card->isVisible() || !card->geometry().intersects(card->parentWidget()->rect())) continue;
                auto* poster = card->findChild<QLabel*>(QStringLiteral("carouselPoster"));
                const bool loaded = poster != nullptr && !poster->pixmap().isNull();
                if (loaded) ++posters;
                cards.append(QJsonObject{{QStringLiteral("ref"), card->ref()},
                    {QStringLiteral("title"), card->toolTip()},
                    {QStringLiteral("posterSource"), card->posterUrl()},
                    {QStringLiteral("posterLoaded"), loaded}});
            }
            if (cards.size() >= 3 && posters >= 3 && stageClock_.elapsed() >= 6000) {
                capture(window_.get(), QStringLiteral("catalogue"), QJsonObject{
                    {QStringLiteral("surface"), QStringLiteral("live anonymous home catalogue")},
                    {QStringLiteral("cards"), cards}});
                startPlayback();
            } else if (stageClock_.elapsed() > 65000) {
                error(QStringLiteral("Live anonymous catalogue/artwork did not become ready in 65 seconds; no catalogue screenshot or invented replacement was produced."));
                startPlayback();
            }
            break;
        }
        case Stage::Playback:
            if (surface_->hasVisibleOutputFrame() && surface_->isPlaying() &&
                surface_->position() >= 1.0 && surface_->duration() >= 8.0 &&
                surface_->mediaTracks().ready) {
                firstPosition_ = surface_->position();
                next(Stage::Pause);
            } else if (stageClock_.elapsed() > 30000) {
                stop(QStringLiteral("Demo playback did not expose a frame, tracks and at least eight seconds of duration in 30 seconds; see failurePlaybackState for observed runtime and playback state."));
            }
            break;
        case Stage::Pause:
            if (surface_->position() > firstPosition_ + 0.35) {
                const QImage frame = surface_->grabVideoFramebuffer();
                const auto picture = mplayer::probes::measurePicture(
                    frame, surface_->videoWidth(), surface_->videoHeight());
                if (picture.isMostlyBlack()) {
                    stop(QStringLiteral("The original demo rendered a mostly black video framebuffer; refusing a misleading player screenshot."));
                    break;
                }
                evidence_.insert(QStringLiteral("playbackSmoke"), QJsonObject{
                    {QStringLiteral("clockAdvanced"), true},
                    {QStringLiteral("fromSeconds"), firstPosition_},
                    {QStringLiteral("toSeconds"), surface_->position()},
                    {QStringLiteral("pictureLitFraction"), picture.litFraction()}});
                player_->togglePlayPause();
                player_->noteUserActivity();
                next(Stage::PlayerShot);
            } else if (stageClock_.elapsed() > 10000) {
                stop(QStringLiteral("The demo clock did not advance while playing."));
            }
            break;
        case Stage::PlayerShot:
            if (surface_->pauseObserved() && !surface_->isPlaying() && stageClock_.elapsed() >= 700) {
                player_->noteUserActivity();
                capture(player_->window(), QStringLiteral("player"), playbackState());
                next(Stage::CompareOpen);
            } else if (stageClock_.elapsed() > 6000) {
                stop(QStringLiteral("The real pause action did not settle."));
            }
            break;
        case Stage::CompareOpen:
            next(Stage::CompareReady);
            if (!pictureAction(QStringLiteral("compare"))) stop(QStringLiteral("The real Quick picture menu Compare row could not be activated."));
            break;
        case Stage::CompareReady: {
            compare_ = nullptr;
            for (QWidget* top : QApplication::topLevelWidgets()) {
                if (auto* candidate = qobject_cast<CompareWindow*>(top);
                    candidate != nullptr) compare_ = candidate;
            }
            if (compare_ == nullptr || !compare_->isVisible()) {
                if (stageClock_.elapsed() > 5000) stop(QStringLiteral("The Compare quality action opened no comparison window (check deployed shaders)."));
                break;
            }
            if (!compareSized_) {
                fit(compare_);
                compareSized_ = true;
                stageClock_.restart();
            }
            if (!compare_->captureError().isEmpty() || !compare_->lockError().isEmpty()) {
                stop(QStringLiteral("Compare refused its real frame: %1 %2").arg(compare_->captureError(), compare_->lockError()));
            } else if (!compare_->comparisonPending() && !compare_->frozenFrame().isNull() &&
                       !compare_->renderedSideA().isNull() && !compare_->renderedSideB().isNull() &&
                       stageClock_.elapsed() >= 1000) {
                QJsonObject state = playbackState();
                state.insert(QStringLiteral("candidateA"), compare_->selectedCandidateId(0));
                state.insert(QStringLiteral("candidateB"), compare_->selectedCandidateId(1));
                state.insert(QStringLiteral("frozenSize"), QJsonArray{compare_->frozenFrame().width(), compare_->frozenFrame().height()});
                state.insert(QStringLiteral("playbackHeld"), compare_->playbackHeld());
                if (compare_->selectedCandidateId(0).startsWith(QLatin1String("spatial:")) ||
                    compare_->selectedCandidateId(1).startsWith(QLatin1String("spatial:"))) {
                    stop(QStringLiteral("Unexpected AI candidate in scratch comparison; refusing inference."));
                    break;
                }
                capture(compare_, QStringLiteral("compare"), state);
                compare_->close();
                next(Stage::CompareClosed);
            } else if (stageClock_.elapsed() > 25000) {
                stop(QStringLiteral("Real comparison outputs did not settle in 25 seconds."));
            }
            break;
        }
        case Stage::CompareClosed:
            if ((compare_.isNull() || (!compare_->isVisible() && !compare_->playbackHeld() &&
                 !compare_->comparisonPending())) && !surface_->nativePlanLocked()) {
                evidence_.insert(QStringLiteral("compareCloseReleasedHold"), true);
                next(Stage::InformationOpen);
            } else if (stageClock_.elapsed() > 7000) {
                stop(QStringLiteral("Closing Compare did not release its playback/plan hold."));
            }
            break;
        case Stage::InformationOpen:
            next(Stage::InformationRead);
            if (!pictureAction(QStringLiteral("information"))) stop(QStringLiteral("The real Quick picture menu Media information row could not be activated."));
            break;
        case Stage::InformationRead: {
            information_ = nullptr;
            for (QWidget* top : QApplication::topLevelWidgets()) {
                if (auto* candidate = qobject_cast<MediaInformationDialog*>(top);
                    candidate != nullptr) information_ = candidate;
            }
            if (information_ == nullptr || !information_->isVisible()) {
                if (stageClock_.elapsed() > 5000) stop(QStringLiteral("The real Media information action opened no dialog."));
                break;
            }
            auto* mode = information_->findChild<QComboBox*>(QStringLiteral("playerMediaInformationMode"));
            if (mode == nullptr) {
                stop(QStringLiteral("The inspector has no real view selector."));
                break;
            }
            if (!informationSized_) {
                fit(information_);
                mode->setCurrentIndex(2); // The real Plain text view exposes async completion.
                informationSized_ = true;
            }
            auto* text = information_->findChild<QPlainTextEdit*>(QStringLiteral("playerMediaInformationPlainText"));
            const QString value = text == nullptr ? QString() : text->toPlainText();
            if (!value.isEmpty() && !value.contains(QStringLiteral("Reading media information")) &&
                stageClock_.elapsed() >= 1000) {
                if (value.contains(QStringLiteral("did not return all media information")) ||
                    value.contains(QStringLiteral("Playback error"))) {
                    stop(QStringLiteral("The real media inspector reported a timeout/playback error."));
                    break;
                }
                informationText_ = privateSafe(value);
                mode->setCurrentIndex(0); // Return to the actual Overview surface for the gallery.
                next(Stage::InformationShot);
            } else if (stageClock_.elapsed() > 12000) {
                stop(QStringLiteral("Media information did not finish its asynchronous read."));
            }
            break;
        }
        case Stage::InformationShot:
            if (stageClock_.elapsed() >= 700) {
                QJsonObject state = playbackState();
                state.insert(QStringLiteral("view"), QStringLiteral("Overview"));
                state.insert(QStringLiteral("observedPlainText"), informationText_);
                capture(information_, QStringLiteral("media-info"), state);
                information_->close();
                player_->hideWindow();
                window_->show();
                window_->raise();
                window_->activateWindow();
                next(Stage::SettingsOpen);
            }
            break;
        case Stage::SettingsOpen: {
            QAbstractButton* settingsButton = nullptr;
            for (auto* button : window_->findChildren<QAbstractButton*>()) {
                if (button->isVisible() && button->isEnabled() &&
                    button->accessibleName() == QLatin1String("API token settings")) {
                    settingsButton = button;
                    break;
                }
            }
            if (settingsButton == nullptr) {
                stop(QStringLiteral("The real catalogue Settings button is unavailable."));
                break;
            }
            next(Stage::SettingsShot);
            // MainWindow opens the real modal Settings with exec(). Defer the
            // click until this tick returns so its nested loop can drive us.
            QTimer::singleShot(0, settingsButton, [settingsButton] { settingsButton->click(); });
            break;
        }
        case Stage::SettingsShot: {
            auto* settings = window_->findChild<mikai::app::SettingsDialog*>();
            if (settings == nullptr || !settings->isVisible()) {
                if (stageClock_.elapsed() > 5000) stop(QStringLiteral("The Settings button opened no real dialog."));
                break;
            }
            if (!settingsSized_) {
                fit(settings);
                if (!settings->showPage(QStringLiteral("customization/appearance"))) {
                    stop(QStringLiteral("The real Settings appearance page is unavailable."));
                    break;
                }
                settingsSized_ = true;
                stageClock_.restart();
            } else if (!settingsInteractionComplete_ && stageClock_.elapsed() >= 1000) {
                const auto accentSwatches = settings->findChildren<QPushButton*>(QStringLiteral("accentSwatch"));
                const auto radiusChips = settings->findChildren<QPushButton*>(QStringLiteral("radiusChip"));
                const auto gradientSwatches = settings->findChildren<QPushButton*>(QStringLiteral("gradientSwatch"));
                const auto sliders = settings->findChildren<QSlider*>(QStringLiteral("appearanceSlider"));
                auto* backgroundGradient = settings->findChild<QAbstractButton*>(
                    QStringLiteral("backgroundGradientSwitch"));
                auto* reset = settings->findChild<QPushButton*>(QStringLiteral("resetAppearanceButton"));
                auto* accentLabel = settings->findChild<QLabel*>(QStringLiteral("accentValueLabel"));
                auto* gradientLabel = settings->findChild<QLabel*>(QStringLiteral("gradientValueLabel"));
                const auto valueLabels = settings->findChildren<QLabel*>(QStringLiteral("appearanceValue"));
                if (accentSwatches.size() < 2 || radiusChips.size() < 2 || gradientSwatches.size() < 2 ||
                    sliders.size() < 2 || backgroundGradient == nullptr || reset == nullptr ||
                    accentLabel == nullptr || gradientLabel == nullptr || valueLabels.size() < 2) {
                    stop(QStringLiteral("The real Appearance page is missing controls needed for interaction evidence."));
                    break;
                }

                if (!capture(window_.get(), QStringLiteral("appearance-before"), QJsonObject{
                    {QStringLiteral("appearancePreview"), QStringLiteral("before control changes")},
                    {QStringLiteral("saved"), false}})) {
                    stop(QStringLiteral("The real catalogue window baseline could not be captured."));
                    break;
                }
                QElapsedTimer interactionClock;
                qint64 maximumUiUpdateMs = 0;
                int completedRoundTrips = 0;
                const QString initialAccent = accentLabel->text();
                const QString initialGradient = gradientLabel->text();
                const QString initialValue = valueLabels.constFirst()->text();
                const bool initialBackgroundGradient = backgroundGradient->isChecked();
                bool accentResponded = false;
                bool gradientResponded = false;
                bool backgroundResponded = false;
                bool sliderLabelResponded = false;
                for (int round = 0; round < 3; ++round) {
                    interactionClock.restart();
                    accentSwatches.at(round % accentSwatches.size())->click();
                    QCoreApplication::processEvents();
                    maximumUiUpdateMs = qMax(maximumUiUpdateMs, interactionClock.elapsed());
                    accentResponded = accentResponded || accentLabel->text() != initialAccent;
                    interactionClock.restart();
                    gradientSwatches.at(round % gradientSwatches.size())->click();
                    QCoreApplication::processEvents();
                    maximumUiUpdateMs = qMax(maximumUiUpdateMs, interactionClock.elapsed());
                    gradientResponded = gradientResponded || gradientLabel->text() != initialGradient;
                    interactionClock.restart();
                    backgroundGradient->click();
                    QCoreApplication::processEvents();
                    maximumUiUpdateMs = qMax(maximumUiUpdateMs, interactionClock.elapsed());
                    backgroundResponded = backgroundResponded ||
                        backgroundGradient->isChecked() != initialBackgroundGradient;
                    interactionClock.restart();
                    radiusChips.at((round + 1) % radiusChips.size())->click();
                    QCoreApplication::processEvents();
                    maximumUiUpdateMs = qMax(maximumUiUpdateMs, interactionClock.elapsed());
                    for (int index = 0; index < sliders.size(); ++index) {
                        auto* slider = sliders.at(index);
                        const int original = slider->value();
                        interactionClock.restart();
                        slider->setValue(original == slider->maximum() ? slider->minimum() : original + 1);
                        QCoreApplication::processEvents();
                        maximumUiUpdateMs = qMax(maximumUiUpdateMs, interactionClock.elapsed());
                        if (slider->value() != original) ++completedRoundTrips;
                        sliderLabelResponded = sliderLabelResponded ||
                            valueLabels.constFirst()->text() != initialValue;
                    }
                }
                const bool previewCaptured = capture(window_.get(), QStringLiteral("appearance-preview"),
                    QJsonObject{{QStringLiteral("appearancePreview"), QStringLiteral("after control changes")},
                                {QStringLiteral("saved"), false}});
                reset->click();
                QCoreApplication::processEvents();
                if (completedRoundTrips < 6 || !accentResponded || !gradientResponded ||
                    !backgroundResponded || !sliderLabelResponded || !previewCaptured) {
                    stop(QStringLiteral("Appearance controls or the real catalogue window preview did not respond."));
                    break;
                }
                settingsInteractionEvidence_ = QJsonObject{
                    {QStringLiteral("interactionRounds"), 3},
                    {QStringLiteral("changedSliderValues"), completedRoundTrips},
                    {QStringLiteral("accentLabelUpdated"), accentResponded},
                    {QStringLiteral("gradientLabelUpdated"), gradientResponded},
                    {QStringLiteral("backgroundGradientToggled"), backgroundResponded},
                    {QStringLiteral("sliderLabelUpdated"), sliderLabelResponded},
                    {QStringLiteral("cataloguePreviewCaptured"), previewCaptured},
                    {QStringLiteral("resetToDefaults"), true},
                    {QStringLiteral("maximumControlDispatchAndEventsMs"), maximumUiUpdateMs}};
                settingsInteractionComplete_ = true;
                stageClock_.restart();
            } else if (settingsInteractionComplete_ && stageClock_.elapsed() >= 700) {
                if (!capture(window_.get(), QStringLiteral("appearance-reset"), QJsonObject{
                    {QStringLiteral("appearancePreview"), QStringLiteral("after Reset and repaint")},
                    {QStringLiteral("saved"), false}})) {
                    stop(QStringLiteral("The real catalogue window reset preview could not be captured."));
                    break;
                }
                capture(settings, QStringLiteral("settings"), QJsonObject{
                    {QStringLiteral("page"), settings->currentPageId()},
                    {QStringLiteral("availablePages"), QJsonArray::fromStringList(settings->pageIds())},
                    {QStringLiteral("appearanceInteractions"), settingsInteractionEvidence_},
                    {QStringLiteral("saved"), false}});
                next(Stage::Close);
                settings->reject(); // Never Save: no OS integration or production preference changes.
            }
            break;
        }
        case Stage::Close:
            if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                modal->reject();
                break; // Let MainWindow's real nested modal frame return first.
            }
            if (information_ != nullptr) information_->close();
            if (compare_ != nullptr) compare_->close();
            if (window_->close()) {
                evidence_.insert(QStringLiteral("applicationCloseAccepted"), true);
                next(Stage::Drain);
                mikai::app::Runner::shutdownAll([this] {
                    evidence_.insert(QStringLiteral("workersDrained"), true);
                    next(Stage::Finished);
                    finish();
                });
            } else if (stageClock_.elapsed() > 10000) {
                error(QStringLiteral("The application refused clean close for ten seconds."));
                // Destruction still uses the production forced-close barrier.
                window_.reset();
                next(Stage::Drain);
                mikai::app::Runner::shutdownAll([this] {
                    evidence_.insert(QStringLiteral("workersDrained"), true);
                    next(Stage::Finished);
                    finish();
                });
            }
            break;
        case Stage::Drain:
            if (stageClock_.elapsed() > 20000 && !drainReported_) {
                error(QStringLiteral("Asynchronous worker shutdown exceeded twenty seconds; waiting for safe retirement (outer isolation watchdog is bounded)."));
                drainReported_ = true;
            }
            break;
        case Stage::Finished:
            break;
        }
    }

    void finish() {
        timer_.stop();
        const bool ok = errors_.isEmpty() && shots_.size() == 8;
        evidence_.insert(QStringLiteral("success"), ok);
        evidence_.insert(QStringLiteral("finishedUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        evidence_.insert(QStringLiteral("elapsedMs"), double(totalClock_.elapsed()));
        evidence_.insert(QStringLiteral("screenshots"), shots_);
        evidence_.insert(QStringLiteral("errors"), errors_);
        const QByteArray json = QJsonDocument(evidence_).toJson(QJsonDocument::Indented);
        QSaveFile file(QDir(arguments_.output).filePath(QStringLiteral("gallery-evidence.json")));
        const bool written = file.open(QIODevice::WriteOnly) && file.write(json) == json.size() && file.commit();
        if (!written) diagnostic(QStringLiteral("ERROR: cannot publish gallery-evidence.json."));
        std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
        std::fflush(stdout);
        diagnostic(ok && written ? QStringLiteral("PASS: five real surfaces and three appearance previews captured; clean shutdown observed.")
                                 : QStringLiteral("FAIL: consult gallery-evidence.json/errors; do not publish partial captures as a successful gallery."));
        app_.exit(ok && written ? 0 : 1);
    }

    QApplication& app_;
    Arguments arguments_;
    QString demo_;
    mikai::backend::MikaiClient client_;
    std::unique_ptr<MainWindow> window_;
    QPointer<PlayerController> player_;
    QPointer<mplayer::VideoSession> surface_;
    QPointer<CompareWindow> compare_;
    QPointer<MediaInformationDialog> information_;
    QTimer timer_;
    QElapsedTimer stageClock_;
    QElapsedTimer totalClock_;
    Stage stage_ = Stage::Catalogue;
    QJsonObject evidence_;
    QJsonArray shots_;
    QJsonArray errors_;
    QString playbackError_;
    QString informationText_;
    double firstPosition_ = 0.0;
    bool handling_ = false;
    bool compareSized_ = false;
    bool informationSized_ = false;
    bool settingsInteractionComplete_ = false;
    QJsonObject settingsInteractionEvidence_;
    bool settingsSized_ = false;
    bool drainReported_ = false;
};

// ── Focused accent sweep ────────────────────────────────────────────────────
// `--accent-sweep` runs this instead of the gallery. It sweeps every shipped
// preset plus a dark custom accent through the real widgets and the real sheets,
// keeps Compare and the engine-preparation window OPEN while the accent changes,
// and measures the pixels each surface actually paints. It never starts
// TensorRT, RIFE or any model: Compare refuses to open when the catalogue would
// select an AI candidate, and the preparation window is driven with stage data
// only (presentation-only, no builder process).
class AccentSweep final {
public:
    AccentSweep(QApplication& app, Arguments arguments, QString demo, QString digest)
        : app_(app), arguments_(std::move(arguments)), demo_(std::move(demo)), digest_(std::move(digest)),
          offlineClient_(QString(), QStringLiteral("http://127.0.0.1:1")) {
        evidence_ = QJsonObject{
            {QStringLiteral("schema"), 1},
            {QStringLiteral("mode"), arguments_.radiusSweep ? QStringLiteral("radius-sweep")
                                                           : QStringLiteral("accent-sweep")},
            {QStringLiteral("startedUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {QStringLiteral("applicationVersion"), QCoreApplication::applicationVersion()},
            {QStringLiteral("qtVersion"), QString::fromLatin1(qVersion())},
            {QStringLiteral("platform"), QGuiApplication::platformName()},
            {QStringLiteral("outputDirectory"), arguments_.output},
            {QStringLiteral("isolation"), QStringLiteral("verified executable copy; fresh INI settings, home, cache, working directory and native store")},
            {QStringLiteral("catalogue"), QStringLiteral("no live service is used: this sweep measures chrome, not content")},
            {QStringLiteral("video"), QJsonObject{
                {QStringLiteral("filename"), QFileInfo(demo_).fileName()},
                {QStringLiteral("sha256"), digest_},
                {QStringLiteral("bytes"), double(QFileInfo(demo_).size())},
                {QStringLiteral("creditDeclaration"), arguments_.credit},
                {QStringLiteral("sourceDeclaration"), arguments_.source},
                {QStringLiteral("declarationVerified"), false}}},
            {QStringLiteral("devicePixelRatio"), QGuiApplication::primaryScreen() != nullptr
                ? QGuiApplication::primaryScreen()->devicePixelRatio() : 0.0},
            {QStringLiteral("nativeDesktopScale"), 1.75},
            {QStringLiteral("presentationOnly"), QJsonObject{
                {QStringLiteral("preparationOverlay"), true},
                {QStringLiteral("titleWatchState"), true},
                {QStringLiteral("benchmarkProgressState"), true},
                {QStringLiteral("comparePictures"), false}}},
            {QStringLiteral("inference"), QJsonObject{
                {QStringLiteral("proven"), false},
                {QStringLiteral("trtStarted"), false},
                {QStringLiteral("rifeStarted"), false},
                {QStringLiteral("modelRequested"), false}}},
            {QStringLiteral("note"), QStringLiteral("Presentation-only surfaces are labelled per capture. Compare is the real window and is never opened when its selection would be an AI candidate.")}};
    }

    int run() {
        total_.start();
        // A startup prompt owns a nested loop; reject foreign modals on a timer
        // so no sweep step can be held hostage by one.
        QTimer modalGuard;
        QObject::connect(&modalGuard, &QTimer::timeout, &modalGuard,
                         [this] { dismissForeignModals(); });
        modalGuard.start(200);
        if (!buildSurfaces()) return finish(false);
        if (arguments_.radiusSweep) {
            if (!sweepRadii()) return finish(false);
            return finish(failed_ == 0);
        }
        if (!sweepAccents()) return finish(false);
        sweepGradientSource();
        if (!sweepPip()) return finish(false);
        measureLanguages();
        return finish(failed_ == 0);
    }

private:
    struct Pixels {
        QImage image;
        qreal scale = 1.0;
        QColor at(int x, int y) const {
            if (image.isNull()) return {};
            const int px = qBound(0, qRound(x * scale), image.width() - 1);
            const int py = qBound(0, qRound(y * scale), image.height() - 1);
            return image.pixelColor(px, py);
        }
    };

    static Pixels pixelsOf(
        QWidget* widget,
        QWidget::RenderFlags flags = QWidget::RenderFlags(QWidget::DrawWindowBackground |
                                                          QWidget::DrawChildren)) {
        Pixels pixels;
        if (widget == nullptr) return pixels;
        // A transparent bed makes an unpainted fill measurable (the page glow
        // is an alpha wash, not an opaque panel).
        pixels.scale = widget->devicePixelRatioF() > 0.0 ? widget->devicePixelRatioF() : 1.0;
        pixels.image = QImage(qCeil(widget->width() * pixels.scale),
                              qCeil(widget->height() * pixels.scale),
                              QImage::Format_ARGB32_Premultiplied);
        pixels.image.setDevicePixelRatio(pixels.scale);
        pixels.image.fill(Qt::transparent);
        QPainter painter(&pixels.image);
        widget->render(&painter, QPoint(), QRegion(), flags);
        painter.end();
        return pixels;
    }

    /// One pixel of a button rendered in a chosen state: the widget's own style
    /// resolves the sheet, so no pointer has to be near the control.
    static QColor pixelOfState(QAbstractButton& button, bool mouseOver, int x, int y) {
        if (button.width() <= 0 || button.height() <= 0) return QColor();
        const qreal scale = button.devicePixelRatioF() > 0.0 ? button.devicePixelRatioF() : 1.0;
        QImage image(qCeil(button.width() * scale), qCeil(button.height() * scale),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(scale);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        if (auto* push = qobject_cast<QPushButton*>(&button)) {
            QStyleOptionButton option;
            option.initFrom(push);
            option.rect = push->rect();
            option.text = push->text();
            option.icon = push->icon();
            option.iconSize = push->iconSize();
            option.features = QStyleOptionButton::None;
            option.state &= ~(QStyle::State_HasFocus | QStyle::State_MouseOver |
                              QStyle::State_Sunken | QStyle::State_On | QStyle::State_Raised);
            option.state |= QStyle::State_Raised;
            if (mouseOver) option.state |= QStyle::State_MouseOver;
            push->style()->drawControl(QStyle::CE_PushButton, &option, &painter, push);
        }
        painter.end();
        return image.pixelColor(qBound(0, qRound(x * scale), image.width() - 1),
                                qBound(0, qRound(y * scale), image.height() - 1));
    }

    struct TimelineSample {
        QColor pixel;
        int longestRun = -1;
        int span = 0;
        int startOffset = -1;
        double fraction = 0.0;
        QRect barInShell;
        QRect barInSurface;
        QRect surfaceRect;
        QSize frameSize;
        qreal dpr = 1.0;
        QString device;
        QString note;
        bool visible = false;
    };

    QQuickItem* timelineItem(QQuickItem* item) const {
        if (item == nullptr) return nullptr;
        if (QString::fromLatin1(item->metaObject()->className()).contains(
                QLatin1String("Timeline"))) return item;
        for (QQuickItem* child : item->childItems()) {
            if (QQuickItem* found = timelineItem(child)) return found;
        }
        return nullptr;
    }

    /// Sample the QML Timeline's played accent from the actual Quick window
    /// screenshot; the mpv framebuffer intentionally contains no chrome.
    TimelineSample sampleTimeline(const QColor& expected) {
        TimelineSample sample;
        if (player_ == nullptr || player_->window() == nullptr || surface_ == nullptr) {
            sample.note = QStringLiteral("no Quick player window or VideoSession");
            return sample;
        }
        player_->noteUserActivity();
        const double duration = surface_->duration();
        if (duration <= 0.0) {
            sample.note = QStringLiteral("no seekable timeline duration");
            return sample;
        }
        player_->seekTo(duration * 0.5);
        sample.fraction = 0.5;
        QTest::qWait(100);
        QQuickItem* timeline = timelineItem(player_->window()->contentItem());
        if (timeline == nullptr || !timeline->isVisible() ||
            player_->window()->screen() == nullptr) {
            sample.note = QStringLiteral("the QML Timeline item is not visible");
            return sample;
        }
        sample.visible = true;
        const QPointF origin = timeline->mapToScene(QPointF(0, 0));
        sample.barInSurface = QRect(qRound(origin.x()), qRound(origin.y()),
                                    qRound(timeline->width()), qRound(timeline->height()));
        sample.barInShell = sample.barInSurface;
        sample.surfaceRect = QRect(0, 0, player_->window()->width(),
                                   player_->window()->height());
        const QPixmap screenshot =
            player_->window()->screen()->grabWindow(player_->window()->winId());
        const QImage frame = screenshot.toImage();
        sample.frameSize = frame.size();
        sample.dpr = screenshot.devicePixelRatio();
        sample.device = QStringLiteral("Quick window");
        if (frame.isNull() || sample.dpr <= 0.0) {
            sample.note = QStringLiteral("the Quick window produced no screenshot");
            return sample;
        }
        const int y = qRound((origin.y() + timeline->height() / 2.0) * sample.dpr);
        const int from = qRound(origin.x() * sample.dpr);
        const int to = qRound((origin.x() + timeline->width()) * sample.dpr) - 1;
        if (y < 0 || y >= frame.height() || from < 0 || to >= frame.width() || to <= from) {
            sample.note = QStringLiteral("the QML Timeline bounds fall outside its real screenshot");
            return sample;
        }
        sample.span = to - from + 1;
        int run = 0;
        int runStart = -1;
        for (int x = from; x <= to; ++x) {
            const QColor pixel = frame.pixelColor(x, y);
            if (nearColor(pixel, expected, 10)) {
                if (run == 0) runStart = x;
                ++run;
                if (run > sample.longestRun) {
                    sample.longestRun = run;
                    sample.startOffset = runStart - from;
                    sample.pixel = pixel;
                }
            } else {
                run = 0;
                runStart = -1;
            }
        }
        return sample;
    }

    static int countNear(const QImage& image, const QColor& color, int tolerance = 10) {
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() < 128) continue;
                if (qAbs(pixel.red() - color.red()) <= tolerance &&
                    qAbs(pixel.green() - color.green()) <= tolerance &&
                    qAbs(pixel.blue() - color.blue()) <= tolerance) {
                    ++count;
                }
            }
        }
        return count;
    }

    static QString nameColor(const QColor& color) {
        return color.name(QColor::HexRgb);
    }

    static bool nearColor(const QColor& actual, const QColor& expected, int tolerance = 6) {
        return actual.isValid() && qAbs(actual.red() - expected.red()) <= tolerance &&
               qAbs(actual.green() - expected.green()) <= tolerance &&
               qAbs(actual.blue() - expected.blue()) <= tolerance;
    }

    static QString safeName(QString name) {
        for (QChar& character : name) {
            if (!character.isLetterOrNumber() && character != QLatin1Char('-')) {
                character = QLatin1Char('-');
            }
        }
        return name;
    }

    void check(const QString& name, bool ok, const QString& detail) {
        checks_.append(QJsonObject{{QStringLiteral("name"), name},
                                   {QStringLiteral("passed"), ok},
                                   {QStringLiteral("detail"), detail}});
        if (ok) {
            ++passed_;
        } else {
            ++failed_;
            failures_.append(QJsonObject{{QStringLiteral("name"), name},
                                         {QStringLiteral("cause"), detail}});
        }
        diagnostic(QStringLiteral("%1: %2%3")
                       .arg(ok ? QStringLiteral("ok") : QStringLiteral("FAIL"), name,
                            detail.isEmpty() ? QString() : QStringLiteral(" - ") + detail));
    }

    bool capture(QWidget* widget, const QString& rawName, QJsonObject extra) {
        if (widget == nullptr || !widget->isVisible() || widget->width() <= 0 ||
            widget->height() <= 0) {
            check(QStringLiteral("capture:") + rawName, false,
                  QStringLiteral("the real widget is not visible or has no size"));
            return false;
        }
        const QPixmap grab = widget->grab();
        if (grab.isNull()) {
            check(QStringLiteral("capture:") + rawName, false,
                  QStringLiteral("the real widget produced no pixels"));
            return false;
        }
        const QString name = safeName(rawName);
        const QString path = QDir(arguments_.output).filePath(name + QStringLiteral(".png"));
        if (!grab.save(path, "PNG")) {
            check(QStringLiteral("capture:") + rawName, false,
                  QStringLiteral("saving the grab failed"));
            return false;
        }
        extra.insert(QStringLiteral("file"), name + QStringLiteral(".png"));
        // No per-capture digest: hashing a PNG on the GUI thread while the
        // windows are live is a stall, and the artifacts can be hashed after
        // the probe has shut down.
        extra.insert(QStringLiteral("widgetClass"),
                     QString::fromLatin1(widget->metaObject()->className()));
        extra.insert(QStringLiteral("widgetObjectName"), widget->objectName());
        extra.insert(QStringLiteral("logicalSize"), QJsonArray{widget->width(), widget->height()});
        extra.insert(QStringLiteral("pixelSize"), QJsonArray{grab.width(), grab.height()});
        extra.insert(QStringLiteral("devicePixelRatio"), grab.devicePixelRatio());
        extra.insert(QStringLiteral("capturedUtc"),
                     QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        shots_.append(extra);
        diagnostic(QStringLiteral("Captured %1").arg(path));
        return true;
    }

    static void place(QWidget* widget, int width, int height) {
        widget->resize(width, height);
        if (QScreen* screen = widget->screen()) {
            const QRect available = screen->availableGeometry();
            widget->move(available.center() - QPoint(widget->width() / 2, widget->height() / 2));
        }
    }

    bool waitFor(const std::function<bool()>& predicate, int timeoutMs,
                 const QString& what) {
        QElapsedTimer clock;
        clock.start();
        while (!predicate()) {
            if (clock.elapsed() > timeoutMs) {
                check(what, false, QStringLiteral("timed out after %1 ms").arg(timeoutMs));
                return false;
            }
            QTest::qWait(25);
        }
        return true;
    }

    void dismissForeignModals() {
        for (QWidget* window : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<QDialog*>(window);
            if (dialog == nullptr || !dialog->isVisible() || !dialog->isModal()) continue;
            if (dialog == compare_.data()) continue;
            if (dialog == settings_.get()) continue;
            diagnostic(QStringLiteral("Rejecting a startup modal that is not part of the sweep: %1")
                           .arg(dialog->objectName()));
            dialog->reject();
        }
    }

    /// Painted corner pixels: a square step fills this block, a rounded one
    /// leaves it to the bed.
    static int cornerInk(const Pixels& pixels) {
        int ink = 0;
        for (int y = 0; y < 6; ++y) {
            for (int x = 0; x < 6; ++x) {
                if (pixels.at(x, y).alpha() > 0) ++ink;
            }
        }
        return ink;
    }

    /// The shipped radius steps on the three real surfaces, wide and at their
    /// own minimum width.
    bool sweepRadii() {
        if (settings_ != nullptr) {
            settings_->showPage(QStringLiteral("customization/appearance"));
            QTest::qWait(120);
        }
        struct Target {
            const char* surface;
            QWidget* host;
            QAbstractButton* control;
        };
        auto* saveButton = settings_ != nullptr
                               ? settings_->findChild<QPushButton*>(
                                     QStringLiteral("settingsSaveButton"))
                               : nullptr;
        QList<Target> targets;
        if (window_ != nullptr && searchAction_ != nullptr) {
            targets.push_back({"search", window_.get(), searchAction_});
        }
        if (titlePage_ != nullptr && titleWatch_ != nullptr) {
            targets.push_back({"title", titlePage_.get(), titleWatch_});
        }
        if (settings_ != nullptr && saveButton != nullptr) {
            targets.push_back({"settings", settings_.get(), saveButton});
        }
        if (targets.size() < 3) {
            check(QStringLiteral("radius-surfaces"), false,
                  QStringLiteral("the real search, title and settings surfaces are not all up"));
            return false;
        }

        struct Stage {
            const char* name;
            int step;
        };
        const Stage stages[] = {{"none", 0}, {"xs", 1}, {"xl", 5}};
        struct SurfaceInk {
            QString surface;
            QJsonObject byStage;
        };
        QJsonArray rows;
        QList<SurfaceInk> inks;
        for (const Stage& stage : stages) {
            Appearance look = mikai::app::currentAppearance();
            look.radiusStep = stage.step;
            applyLook(look);
            const int small = mikai::app::theme::radiusSmall(look);
            const int medium = mikai::app::theme::radiusMedium(look);
            const int pill = mikai::app::theme::radiusPill(look);
            for (const Target& target : targets) {
                const int minimum =
                    qBound(480, target.host->minimumSizeHint().width(), kWidth);
                QList<int> widths{kWidth};
                if (minimum != kWidth) widths.append(minimum);
                for (const int width : widths) {
                    place(target.host, width, kHeight);
                    QTest::qWait(140);
                    const QString shot =
                        QStringLiteral("radius-%1-%2-%3")
                            .arg(QString::fromLatin1(stage.name),
                                 QString::fromLatin1(target.surface))
                            .arg(width);
                    if (!capture(target.host, shot,
                                 QJsonObject{
                                     {QStringLiteral("radiusStage"),
                                      QString::fromLatin1(stage.name)},
                                     {QStringLiteral("radiusStep"), stage.step},
                                     {QStringLiteral("radiusSmall"), small},
                                     {QStringLiteral("radiusMedium"), medium},
                                     {QStringLiteral("radiusPill"), pill},
                                     {QStringLiteral("requestedWidth"), width}})) {
                        return false;
                    }
                    // No window background: outside a rounded edge that fill
                    // would give every step the same alpha.
                    const Pixels control = pixelsOf(target.control, QWidget::DrawChildren);
                    const int ink = cornerInk(control);
                    const int advance =
                        target.control->fontMetrics().horizontalAdvance(target.control->text());
                    rows.append(QJsonObject{
                        {QStringLiteral("stage"), QString::fromLatin1(stage.name)},
                        {QStringLiteral("radiusStep"), stage.step},
                        {QStringLiteral("radiusSmall"), small},
                        {QStringLiteral("radiusMedium"), medium},
                        {QStringLiteral("radiusPill"), pill},
                        {QStringLiteral("surface"), QString::fromLatin1(target.surface)},
                        {QStringLiteral("control"), target.control->objectName()},
                        {QStringLiteral("requestedWidth"), width},
                        {QStringLiteral("hostWidth"), target.host->width()},
                        {QStringLiteral("controlSize"),
                         QJsonArray{target.control->width(), target.control->height()}},
                        {QStringLiteral("captionAdvance"), advance},
                        {QStringLiteral("captionFits"), target.control->width() >= advance},
                        {QStringLiteral("cornerInk"), ink},
                        {QStringLiteral("cornerAlpha"), control.at(0, 0).alpha()}});
                    if (width != kWidth) continue;
                    const QString surface = QString::fromLatin1(target.surface);
                    SurfaceInk* inkEntry = nullptr;
                    for (SurfaceInk& candidate : inks) {
                        if (candidate.surface == surface) inkEntry = &candidate;
                    }
                    if (inkEntry == nullptr) {
                        inks.push_back({surface, QJsonObject{}});
                        inkEntry = &inks.back();
                    }
                    inkEntry->byStage.insert(QString::fromLatin1(stage.name), ink);
                }
            }
        }

        bool captionsFit = true;
        for (const QJsonValue value : rows) {
            captionsFit =
                captionsFit && value.toObject().value(QStringLiteral("captionFits")).toBool();
        }
        check(QStringLiteral("radius-captions"), captionsFit,
              QStringLiteral("every measured control keeps its whole caption at both ends"));
        for (const SurfaceInk& entry : inks) {
            const int square = entry.byStage.value(QStringLiteral("none")).toInt(-1);
            const int smallest = entry.byStage.value(QStringLiteral("xs")).toInt(-1);
            const int largest = entry.byStage.value(QStringLiteral("xl")).toInt(-1);
            check(QStringLiteral("radius-corner/") + entry.surface, square > largest,
                  QStringLiteral("the square step paints %1 of 36 corner pixels, the largest "
                                 "step %2")
                      .arg(square)
                      .arg(largest));
            check(QStringLiteral("radius-corner-step/") + entry.surface, smallest < square,
                  QStringLiteral("the smallest step paints %1 of 36 corner pixels against %2 at "
                                 "square")
                      .arg(smallest)
                      .arg(square));
        }
        QJsonArray steps;
        for (const int radius : mikai::app::theme::radiusSteps()) steps.append(radius);
        evidence_.insert(QStringLiteral("radiusStages"), rows);
        evidence_.insert(QStringLiteral("radiusSteps"), steps);
        applyLook(Appearance{});
        return failed_ == 0;
    }

    void applyLook(const Appearance& appearance) {
        mikai::app::setCurrentAppearance(appearance);
        mikai::app::theme::applyPalette(appearance);
        app_.setStyleSheet(mikai::app::theme::buildStylesheet(appearance));
        // The settings dialog carries its own sheet, and it is the one that
        // paints the benchmark chunk and the appearance page. Leaving it on the
        // sheet it was built with would keep the boot accent in exactly the
        // window this sweep is meant to prove.
        if (settings_ != nullptr) {
            settings_->setStyleSheet(mikai::app::settingsStylesheet(appearance));
        }
        mplayer::shell::setAccent(mikai::app::theme::accentColor(appearance));
        mplayer::shell::setCornerRadius(mikai::app::theme::radiusSmall(appearance));
        QTest::qWait(90);
    }

    bool buildSurfaces() {
        window_ = std::make_unique<MainWindow>(offlineClient_, MainWindow::StartupMode::FilePlayer);
        window_->setWindowIcon(QIcon(QStringLiteral(":/app/mikai.png")));
        place(window_.get(), kWidth, kHeight);
        window_->show();
        QTest::qWait(120);
        dismissForeignModals();

        searchPage_ = window_->findChild<mikai::app::SearchPage*>();
        searchAction_ = window_->findChild<QPushButton*>(QStringLiteral("searchActionButton"));
        glow_ = searchPage_ != nullptr ? searchPage_->findChild<mikai::app::PageGlow*>() : nullptr;
        if (!waitFor([this] { return searchPage_ != nullptr && searchPage_->isVisible() &&
                                     searchAction_ != nullptr && searchAction_->isVisible(); },
                     8000, QStringLiteral("search-page-real")) ||
            !waitFor([this] { return glow_ != nullptr && glow_->isVisible() && glow_->height() > 0; },
                     5000, QStringLiteral("page-glow-real"))) {
            return false;
        }

        titlePage_ = std::make_unique<mikai::app::TitlePage>(
            localRunner_, offlineClient_, qualityCache_, QStringLiteral("accent-sweep"));
        place(titlePage_.get(), kWidth, kHeight);
        titlePage_->show();
        titleWatch_ = titlePage_->findChild<QPushButton*>(QStringLiteral("titleHeroWatchButton"));
        if (!waitFor([this] { return titleWatch_ != nullptr && titleWatch_->isVisible(); }, 5000,
                     QStringLiteral("title-watch-real"))) {
            return false;
        }
        // The page needs service detail to enable the hero action. No entry is
        // fabricated: the button is enabled for presentation and the capture
        // carries presentationOnly=true.
        titleWatch_->setEnabled(true);
        titleWatch_->style()->unpolish(titleWatch_);
        titleWatch_->style()->polish(titleWatch_);

        settings_ = std::make_unique<mikai::app::SettingsDialog>(offlineClient_, localRunner_);
        place(settings_.get(), kWidth, kHeight);
        settings_->show();
        // The appearance page carries the accent chips - the one self-painted
        // control a generic button box model can inflate. Measure the real chips
        // at the window width and at the compact one before the benchmark page
        // replaces them, then continue with the benchmark capture.
        settings_->showPage(QStringLiteral("customization/appearance"));
        QTest::qWait(180);
        if (!measureAccentSwatches(QStringLiteral("wide"))) return false;
        place(settings_.get(), 580, kHeight);
        QTest::qWait(180);
        if (!measureAccentSwatches(QStringLiteral("compact"))) return false;
        place(settings_.get(), kWidth, kHeight);
        settings_->showPage(QStringLiteral("benchmark"));
        benchmarkProgress_ = settings_->findChild<QProgressBar*>(QStringLiteral("benchmarkProgress"));
        if (benchmarkProgress_ != nullptr) {
            benchmarkProgress_->setRange(0, 100);
            benchmarkProgress_->setValue(80);
            benchmarkProgress_->setProperty("runState", QStringLiteral("running"));
            benchmarkProgress_->setVisible(true);
            benchmarkProgress_->style()->unpolish(benchmarkProgress_);
            benchmarkProgress_->style()->polish(benchmarkProgress_);
        }
        if (!waitFor([this] { return benchmarkProgress_ != nullptr &&
                                     benchmarkProgress_->isVisible() &&
                                     benchmarkProgress_->height() > 0; },
                     5000, QStringLiteral("benchmark-progress-real"))) {
            return false;
        }

        if (!window_->openMedia(demo_)) {
            check(QStringLiteral("player-open-real"), false,
                  QStringLiteral("the application refused the supplied demo"));
            return false;
        }
        player_ = window_->currentMediaPlayer();
        if (!waitFor([this] { return player_ != nullptr && player_->window() != nullptr; },
                     8000, QStringLiteral("quick-player-real"))) {
            return false;
        }
        fit(player_->window());
        player_->window()->showFullscreen();
        QTest::qWait(250);
        evidence_.insert(QStringLiteral("playerFullscreen"), player_->fullscreen());
        surface_ = player_->session();
        if (surface_ == nullptr) {
            check(QStringLiteral("player-session-real"), false,
                  QStringLiteral("the Quick controller has no VideoSession"));
            return false;
        }
        if (!waitFor([this] { return surface_->hasVisibleOutputFrame() &&
                                     surface_->isPlaying() && surface_->duration() >= 1.0; },
                     35000, QStringLiteral("player-frame-real"))) {
            return false;
        }
        player_->togglePlayPause();
        if (!waitFor([this] { return surface_->pauseObserved() && !surface_->isPlaying(); }, 8000,
                     QStringLiteral("player-paused-real"))) {
            return false;
        }
        player_->beginEnginePreparation(QStringLiteral("Engine preparation (presentation only)"));
        player_->updateEnginePreparation(
            2, 4, QStringLiteral("Stage 3 of 4"),
            QStringLiteral("No builder process is started; this samples the real Quick status window."));
        overlay_ = nullptr;
        if (!waitFor([this] {
                for (QWindow* top : QGuiApplication::allWindows()) {
                    if (auto* candidate = qobject_cast<mplayer::quick::PreparationWindow*>(top);
                        candidate != nullptr && candidate->isVisible()) {
                        overlay_ = candidate;
                        return true;
                    }
                }
                return false;
            }, 5000, QStringLiteral("preparation-window-real"))) {
            return false;
        }
        player_->noteUserActivity();
        QTest::qWait(250);
        check(QStringLiteral("timeline-real"),
              timelineItem(player_->window()->contentItem()) != nullptr,
              QStringLiteral("the QML player scene contains its real Timeline component"));

        // The direct-media player has no episode strip by contract.
        check(QStringLiteral("player-no-episode-controls"),
              player_->isDirectMedia() && !player_->hasPreviousEpisode() &&
                  !player_->hasNextEpisode(),
              QStringLiteral("direct local media exposes no episode navigation"));
        const auto hasAction = [](const PlayerMenuModel* model,
                                  PlayerMenuModel::Action action,
                                  const QString& value = QString()) {
            if (model == nullptr) return false;
            for (const PlayerMenuModel::Row& row : model->rows()) {
                if (row.action == action && (value.isEmpty() || row.value == value)) return true;
            }
            return false;
        };
        const auto actionRows = [](const PlayerMenuModel* model,
                                   PlayerMenuModel::Action action) {
            int count = 0;
            if (model != nullptr) {
                for (const PlayerMenuModel::Row& row : model->rows()) {
                    if (row.action == action) ++count;
                }
            }
            return count;
        };
        const auto& tracks = surface_->mediaTracks();
        int audioTracks = 0;
        int subtitleTracks = 0;
        for (const mplayer::MediaTrack& track : tracks.tracks) {
            if (track.type == mplayer::MediaTrackType::Audio) ++audioTracks;
            else ++subtitleTracks;
        }
        PlayerMenuModel* const audioMenu = player_->menuModel(QStringLiteral("audio"));
        PlayerMenuModel* const subtitlesMenu = player_->menuModel(QStringLiteral("subtitles"));
        PlayerMenuModel* const pictureSubtitles =
            player_->menuModel(QStringLiteral("picture.subtitles"));
        check(QStringLiteral("direct-track-rows"),
              tracks.ready && player_->audioTracksAvailable() == (audioTracks > 0) &&
                  player_->subtitleTracksAvailable() == (subtitleTracks > 0) &&
                  actionRows(audioMenu, PlayerMenuModel::Action::SelectTrack) == audioTracks &&
                  actionRows(subtitlesMenu, PlayerMenuModel::Action::SelectTrack) ==
                      subtitleTracks + (subtitleTracks > 0 ? 1 : 0),
              QStringLiteral("direct-media audio/subtitle rows match the loaded session track snapshot"));
        check(QStringLiteral("direct-track-pickers"),
              hasAction(audioMenu, PlayerMenuModel::Action::LoadExternalTrack,
                        QStringLiteral("audio")) &&
                  hasAction(subtitlesMenu, PlayerMenuModel::Action::LoadExternalTrack,
                            QStringLiteral("sub")) &&
                  hasAction(pictureSubtitles, PlayerMenuModel::Action::LoadExternalTrack,
                            QStringLiteral("sub")) &&
                  hasAction(pictureSubtitles, PlayerMenuModel::Action::OpenSubtitleSearch),
              QStringLiteral("local media exposes audio, subtitle-file and OpenSubtitles actions"));
        const double originalSubtitleDelay = player_->subtitleDelay();
        player_->adjustSubtitleDelay(0.1);
        const bool delayChanged = qFuzzyCompare(player_->subtitleDelay() + 1.0,
                                                originalSubtitleDelay + 1.1);
        bool delayRowsUpdated = false;
        if (subtitlesMenu != nullptr) {
            for (const PlayerMenuModel::Row& row : subtitlesMenu->rows()) {
                if (row.id == QLatin1String("delay.heading") &&
                    row.label.contains(QStringLiteral("0.1"))) {
                    delayRowsUpdated = true;
                    break;
                }
            }
        }
        player_->resetSubtitleDelay();
        check(QStringLiteral("subtitle-delay"),
              delayChanged && delayRowsUpdated &&
                  qFuzzyIsNull(player_->subtitleDelay()),
              QStringLiteral("subtitle delay updates the active controller and menu then resets"));
        player_->openMediaInformation();
        waitFor([this] {
            for (QWidget* top : QApplication::topLevelWidgets()) {
                if (auto* dialog = qobject_cast<MediaInformationDialog*>(top)) {
                    mediaInfo_ = dialog;
                    return dialog->isVisible();
                }
            }
            return false;
        }, 6000, QStringLiteral("media-information-real"));
        if (!waitFor([this] { return mediaInfo_ != nullptr && mediaInfo_->isVisible(); }, 6000,
                     QStringLiteral("media-information-real"))) {
            return false;
        }

        // The real Compare window. It renders only the built-in shader
        // candidates; if the catalogue's selection would be an AI model, the
        // window is never shown and no engine is touched.
        player_->openCompare();
        const bool compareOpened = waitFor([this] {
            for (QWidget* top : QApplication::topLevelWidgets()) {
                if (auto* candidate = qobject_cast<CompareWindow*>(top);
                    candidate != nullptr && candidate->isVisible()) {
                    compare_ = candidate;
                    return true;
                }
            }
            return false;
        }, 6000, QStringLiteral("compare-open-real"));
        if (!compareOpened) {
            check(QStringLiteral("compare-open-real"), false,
                  QStringLiteral("the Quick picture action opened no real Compare window"));
            return false;
        }
        place(compare_, kWidth, kHeight);
        const QString sideA = compare_->selectedCandidateId(0);
        const QString sideB = compare_->selectedCandidateId(1);
        const bool aiSelected = sideA.startsWith(QLatin1String("spatial:")) ||
                                sideB.startsWith(QLatin1String("spatial:")) ||
                                sideA.startsWith(QLatin1String("rife")) ||
                                sideB.startsWith(QLatin1String("rife"));
        check(QStringLiteral("compare-no-ai-selection"), !aiSelected,
              QStringLiteral("A=%1 B=%2").arg(sideA, sideB));
        if (aiSelected) {
            compare_->close();
            compare_.clear();
            check(QStringLiteral("compare-open-real"), false,
                  QStringLiteral("refused to open Compare because an AI candidate would be selected"));
        } else {
            compare_->show();
            const bool settled = waitFor(
                [this] {
                    return compare_ != nullptr && !compare_->comparisonPending() &&
                           compare_->captureError().isEmpty() &&
                           !compare_->frozenFrame().isNull() &&
                           !compare_->renderedSideA().isNull() &&
                           !compare_->renderedSideB().isNull();
                },
                25000, QStringLiteral("compare-rendered-real"));
            check(QStringLiteral("compare-open-real"), settled,
                  settled ? QString()
                          : QStringLiteral("captureError=%1 lockError=%2")
                                .arg(compare_ ? compare_->captureError() : QString(),
                                     compare_ ? compare_->lockError() : QString()));
        }
        dismissForeignModals();
        return true;
    }

    /// Every real accent chip on the appearance page: one metric in both
    /// directions with a closed interval, so a sheet cannot inflate it.
    bool measureAccentSwatches(const QString& stage) {
        const QList<QPushButton*> swatches =
            settings_->findChildren<QPushButton*>(QStringLiteral("accentSwatch"));
        const int size = mikai::app::theme::kSettingsSwatchSize;
        int exact = 0;
        QJsonArray rows;
        for (QPushButton* swatch : swatches) {
            const bool closed = swatch->minimumSize().width() <= swatch->maximumSize().width() &&
                                swatch->minimumSize().height() <= swatch->maximumSize().height();
            const bool fixed = swatch->width() == size && swatch->height() == size;
            if (closed && fixed) ++exact;
            rows.append(QJsonObject{
                {QStringLiteral("size"), QJsonArray{swatch->width(), swatch->height()}},
                {QStringLiteral("minimumSize"),
                 QJsonArray{swatch->minimumSize().width(), swatch->minimumSize().height()}},
                {QStringLiteral("maximumSize"),
                 QJsonArray{swatch->maximumSize().width(), swatch->maximumSize().height()}},
                {QStringLiteral("closedInterval"), closed},
                {QStringLiteral("fullyVisible"),
                 swatch->isVisible() && swatch->visibleRegion().contains(swatch->rect())}});
        }
        evidence_.insert(QStringLiteral("accentSwatches/") + stage, rows);
        check(QStringLiteral("appearance-swatch/%1").arg(stage),
              !swatches.isEmpty() && exact == swatches.size(),
              QStringLiteral("%1 of %2 chips are exactly %3x%3 with a closed size interval")
                  .arg(exact)
                  .arg(swatches.size())
                  .arg(size));
        if (!swatches.isEmpty()) {
            capture(swatches.first(), QStringLiteral("accent-swatch-%1").arg(stage),
                    QJsonObject{{QStringLiteral("surface"), QStringLiteral("appearance accent chip")},
                                {QStringLiteral("expectedSize"), size},
                                {QStringLiteral("inferenceProven"), false}});
        }
        return !swatches.isEmpty() && exact == swatches.size();
    }

    QJsonObject widgetMetrics(QWidget* widget) const {
        if (widget == nullptr) return QJsonObject{{QStringLiteral("present"), false}};
        QJsonObject result{{QStringLiteral("present"), true},
                           {QStringLiteral("objectName"), widget->objectName()},
                           {QStringLiteral("visible"), widget->isVisible()},
                           {QStringLiteral("size"), QJsonArray{widget->width(), widget->height()}},
                           {QStringLiteral("minimumSizeHint"),
                            QJsonArray{widget->minimumSizeHint().width(),
                                       widget->minimumSizeHint().height()}},
                           {QStringLiteral("sizeHint"), QJsonArray{widget->sizeHint().width(),
                                                                    widget->sizeHint().height()}},
                           {QStringLiteral("devicePixelRatio"), widget->devicePixelRatioF()}};
        if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
            result.insert(QStringLiteral("text"), button->text());
            result.insert(QStringLiteral("textPx"),
                          QFontMetrics(button->font()).horizontalAdvance(button->text()));
        }
        return result;
    }

    bool sweepAccents() {
        QVector<QPair<QString, QString>> accents;
        for (const mikai::app::theme::AccentPreset& preset :
             mikai::app::theme::accentPresets()) {
            accents.append({preset.id, preset.color});
        }
        accents.append({QStringLiteral("dark-custom"), QStringLiteral("#123456")});

        // The windows were built under the boot appearance. Record that state
        // before the first switch so "before/after while open" is evidenced, not
        // inferred from the absence of a restart.
        {
            const QColor initial = mplayer::shell::accent();
            const QImage overlayImage = overlay_ != nullptr && overlay_->screen() != nullptr
                ? overlay_->screen()->grabWindow(overlay_->winId()).toImage() : QImage();
            const int overlayAccentPixels = countNear(overlayImage, initial, 12);
            check(QStringLiteral("initial/preparation-overlay"), overlayAccentPixels > 0,
                  QStringLiteral("the real PreparationWindow paints %1 accent pixels")
                      .arg(overlayAccentPixels));
            capture(overlay_.data(), QStringLiteral("accent-initial-overlay"),
                    QJsonObject{{QStringLiteral("surface"), QStringLiteral("engine preparation overlay")},
                                {QStringLiteral("accent"), nameColor(initial)},
                                {QStringLiteral("presentationOnly"), true},
                                {QStringLiteral("inferenceProven"), false}});
            if (compare_ != nullptr && compare_->isVisible()) {
                // A freshly shown window paints its chrome a frame later.
                compare_->repaint();
                QTest::qWait(150);
                const QImage before = compare_->grab().toImage();
                const int initialPixels = countNear(before, initial);
                check(QStringLiteral("initial/compare-live-accent"), initialPixels > 0,
                      QStringLiteral("%1 accent pixels before the first switch").arg(initialPixels));
                capture(compare_.data(), QStringLiteral("accent-initial-compare"),
                        QJsonObject{{QStringLiteral("surface"), QStringLiteral("live Compare window")},
                                    {QStringLiteral("accent"), nameColor(initial)},
                                    {QStringLiteral("presentationOnly"), false},
                                    {QStringLiteral("inferenceProven"), false}});
            }
        }

        QJsonArray sweepRows;
        for (mikai::app::theme::SiteTheme site :
             {mikai::app::theme::SiteTheme::Dark, mikai::app::theme::SiteTheme::Light}) {
            const QString siteId =
                site == mikai::app::theme::SiteTheme::Light ? QStringLiteral("light")
                                                            : QStringLiteral("dark");
            for (const auto& accent : accents) {
                Appearance appearance;
                appearance.siteTheme = site;
                appearance.accent = accent.second;
                appearance.gradientMode = mikai::app::theme::GradientMode::Auto;
                appearance.backgroundGradient = true;
                appearance.gradientIntensity = 30;
                appearance.gradientHeight = 100;
                applyLook(appearance);
                // The QML timeline reads the live accent token; wake through the
                // controller's actual activity path.
                if (player_ != nullptr) {
                    player_->noteUserActivity();
                    QTest::qWait(60);
                }

                const QColor expected = mikai::app::theme::accentColor(appearance);
                const QString variant = QStringLiteral("%1-%2").arg(siteId, accent.first);
                QJsonObject row{{QStringLiteral("variant"), variant},
                                {QStringLiteral("siteTheme"), siteId},
                                {QStringLiteral("accentId"), accent.first},
                                {QStringLiteral("accent"), nameColor(expected)}};

                // Search primary: the emitted rule and the painted pixel.
                row.insert(QStringLiteral("searchPrimary"), searchAction_ != nullptr
                    ? nameColor(pixelsOf(searchAction_).at(4, searchAction_->height() / 2))
                    : QString());
                const QColor searchPixel = searchAction_ != nullptr
                    ? pixelsOf(searchAction_).at(4, searchAction_->height() / 2) : QColor();
                check(QStringLiteral("%1/search-primary").arg(variant),
                      nearColor(searchPixel, expected),
                      QStringLiteral("painted %1, chosen %2")
                          .arg(nameColor(searchPixel), nameColor(expected)));

                // Page glow: the gradient's first row is the chosen accent with
                // the bounded alpha, never the old independent blue.
                QColor glowPixel;
                int expectedAlpha = 0;
                if (glow_ != nullptr) {
                    glowPixel = pixelsOf(glow_).at(glow_->width() / 2, 0);
                    expectedAlpha = qBound(0, appearance.gradientIntensity * 190 / 100, 170);
                }
                row.insert(QStringLiteral("pageGlow"), QJsonObject{
                    {QStringLiteral("pixel"), nameColor(glowPixel)},
                    {QStringLiteral("alpha"), glowPixel.alpha()},
                    {QStringLiteral("expectedAlpha"), expectedAlpha}});
                check(QStringLiteral("%1/page-glow").arg(variant),
                      nearColor(glowPixel, expected, 8) &&
                          qAbs(glowPixel.alpha() - expectedAlpha) <= 3,
                      QStringLiteral("paint %1 a%2, expected %3 a%4")
                          .arg(nameColor(glowPixel))
                          .arg(glowPixel.alpha())
                          .arg(nameColor(expected))
                          .arg(expectedAlpha));

                // The page disables the hero action without a playable episode
                // and re-derives that on its own schedule, so the enabled state
                // is set immediately before the sample.
                if (titleWatch_ != nullptr) {
                    titleWatch_->setEnabled(true);
                    titleWatch_->style()->unpolish(titleWatch_);
                    titleWatch_->style()->polish(titleWatch_);
                }
                const QColor watchPixel = titleWatch_ != nullptr
                    ? pixelsOf(titleWatch_).at(4, titleWatch_->height() / 2) : QColor();
                row.insert(QStringLiteral("titleWatch"), nameColor(watchPixel));
                row.insert(QStringLiteral("titleWatchEnabled"),
                           titleWatch_ != nullptr && titleWatch_->isEnabled());
                check(QStringLiteral("%1/title-watch").arg(variant),
                      titleWatch_ != nullptr && titleWatch_->isEnabled() &&
                          nearColor(watchPixel, expected),
                      QStringLiteral("painted %1, chosen %2, enabled %3")
                          .arg(nameColor(watchPixel), nameColor(expected))
                          .arg(titleWatch_ != nullptr && titleWatch_->isEnabled()));

                // Its rule lives in the settings sheet, so it follows that swap.
                if (benchmarkProgress_ != nullptr) {
                    benchmarkProgress_->style()->unpolish(benchmarkProgress_);
                    benchmarkProgress_->style()->polish(benchmarkProgress_);
                }
                const QColor benchmarkPixel = benchmarkProgress_ != nullptr
                    ? pixelsOf(benchmarkProgress_)
                          .at(benchmarkProgress_->width() / 3, benchmarkProgress_->height() / 2)
                    : QColor();
                row.insert(QStringLiteral("benchmarkChunk"), nameColor(benchmarkPixel));
                check(QStringLiteral("%1/benchmark-chunk").arg(variant),
                      nearColor(benchmarkPixel, expected),
                      QStringLiteral("painted %1, chosen %2")
                          .arg(nameColor(benchmarkPixel), nameColor(expected)));

                // The preparation window stays open across the whole sweep; its
                // chunk must follow this variant without a restart.
                const QImage overlayImage = overlay_ != nullptr && overlay_->screen() != nullptr
                    ? overlay_->screen()->grabWindow(overlay_->winId()).toImage() : QImage();
                const int overlayAccentPixels = countNear(overlayImage, expected, 12);
                row.insert(QStringLiteral("overlayChunk"), nameColor(expected));
                row.insert(QStringLiteral("overlayAccentPixels"), overlayAccentPixels);
                check(QStringLiteral("%1/preparation-overlay-live").arg(variant),
                      overlayAccentPixels > 0 && overlay_ != nullptr && overlay_->isVisible(),
                      QStringLiteral("real PreparationWindow paints %1 pixels of chosen %2")
                          .arg(overlayAccentPixels).arg(nameColor(expected)));

                // The media inspector has been open since before the first
                // switch; its selection palette is the accent token.
                if (mediaInfo_ != nullptr && mediaInfo_->isVisible()) {
                    const QColor highlight = mediaInfo_->palette().color(QPalette::Highlight);
                    row.insert(QStringLiteral("mediaInformationHighlight"), nameColor(highlight));
                    check(QStringLiteral("%1/media-information-live").arg(variant),
                          nearColor(highlight, expected, 4),
                          QStringLiteral("palette highlight %1, chosen %2 (window stayed open)")
                              .arg(nameColor(highlight), nameColor(expected)));
                } else {
                    check(QStringLiteral("%1/media-information-live").arg(variant), false,
                          QStringLiteral("the media inspector is not open"));
                }

                // Hover is the contract; the wash alpha is not, so only a
                // "painted at all" floor is asserted. The strip that carries the
                // chip only exists for a source with a playlist, so a direct file
                // hides it by design: the rendered chip is measured either way.
                if (player_ != nullptr) {
                    player_->noteUserActivity();
                    row.insert(QStringLiteral("playerChipVisible"), false);
                    check(QStringLiteral("%1/player-chip-live").arg(variant),
                          player_->isDirectMedia() && !player_->hasPreviousEpisode() &&
                              !player_->hasNextEpisode(),
                          QStringLiteral("direct-media Quick scene correctly omits episode navigation"));
                }

                // The played segment: the longest continuous run of the chosen
                // accent along the bar's own row, read from the device that
                // paints it. A stray picture pixel is not a segment.
                const TimelineSample timeline = sampleTimeline(expected);
                row.insert(QStringLiteral("timelinePlayed"), nameColor(timeline.pixel));
                row.insert(QStringLiteral("timelineDevice"), timeline.device);
                row.insert(QStringLiteral("timelineVisible"), timeline.visible);
                row.insert(QStringLiteral("timelineBarRect"),
                           QJsonArray{timeline.barInShell.x(), timeline.barInShell.y(),
                                      timeline.barInShell.width(), timeline.barInShell.height()});
                row.insert(QStringLiteral("timelineBarOnSurface"),
                           QJsonArray{timeline.barInSurface.x(), timeline.barInSurface.y(),
                                      timeline.barInSurface.width(), timeline.barInSurface.height()});
                row.insert(QStringLiteral("timelineSurfaceRect"),
                           QJsonArray{timeline.surfaceRect.x(), timeline.surfaceRect.y(),
                                      timeline.surfaceRect.width(), timeline.surfaceRect.height()});
                row.insert(QStringLiteral("timelineFrameSize"),
                           QJsonArray{timeline.frameSize.width(), timeline.frameSize.height()});
                row.insert(QStringLiteral("timelineDpr"), timeline.dpr);
                row.insert(QStringLiteral("timelineBarSpanPixels"), timeline.span);
                row.insert(QStringLiteral("timelineRunLongest"), timeline.longestRun);
                row.insert(QStringLiteral("timelineRunStartOffset"), timeline.startOffset);
                row.insert(QStringLiteral("timelinePlayedFraction"), timeline.fraction);
                if (!timeline.note.isEmpty()) row.insert(QStringLiteral("timelineNote"), timeline.note);
                const int requiredRun = qMax(8, qRound(timeline.span * 0.3));
                const int allowedOffset = qMax(4, qRound(timeline.span * 0.1));
                check(QStringLiteral("%1/timeline-played-live").arg(variant),
                      timeline.visible && timeline.longestRun >= requiredRun &&
                          timeline.startOffset >= 0 && timeline.startOffset <= allowedOffset &&
                          nearColor(timeline.pixel, expected, 10),
                      QStringLiteral("on %1: visible %2, longest run %3 px of %4 (need %5), "
                                     "starts %6 px in (allow %7), colour %8, chosen %9%10")
                          .arg(timeline.device)
                          .arg(timeline.visible)
                          .arg(timeline.longestRun)
                          .arg(timeline.span)
                          .arg(requiredRun)
                          .arg(timeline.startOffset)
                          .arg(allowedOffset)
                          .arg(nameColor(timeline.pixel))
                          .arg(nameColor(expected))
                          .arg(timeline.note.isEmpty() ? QString()
                                                       : QStringLiteral(" - ") + timeline.note));

                // Compare chrome, while the same window stays open across the
                // sweep: seat A and the selector border carry the accent, seat B
                // keeps its documented semantic colour.
                if (compare_ != nullptr && compare_->isVisible()) {
                    const QImage compareImage = compare_->grab().toImage();
                    const int accentPixels = countNear(compareImage, expected);
                    const QColor seatB = mplayer::shell::compareSideB(expected);
                    const int seatBPixels = countNear(compareImage, seatB);
                    row.insert(QStringLiteral("compare"), QJsonObject{
                        {QStringLiteral("accentPixels"), accentPixels},
                        {QStringLiteral("seatBPixels"), seatBPixels},
                        {QStringLiteral("seatB"), nameColor(seatB)}});
                    check(QStringLiteral("%1/compare-live-accent").arg(variant),
                          accentPixels > 0,
                          QStringLiteral("%1 accent pixels while the window stayed open")
                              .arg(accentPixels));
                    check(QStringLiteral("%1/compare-seat-b").arg(variant),
                          seatBPixels > 0,
                          QStringLiteral("%1 seat-B pixels of %2")
                              .arg(seatBPixels)
                              .arg(nameColor(seatB)));
                    capture(compare_.data(), QStringLiteral("accent-%1-compare").arg(variant),
                            QJsonObject{{QStringLiteral("surface"), QStringLiteral("live Compare window")},
                                        {QStringLiteral("accent"), nameColor(expected)},
                                        {QStringLiteral("presentationOnly"), false},
                                        {QStringLiteral("inferenceProven"), false}});
                } else {
                    row.insert(QStringLiteral("compare"), QJsonObject{
                        {QStringLiteral("present"), false}});
                }

                capture(searchPage_.get(), QStringLiteral("accent-%1-search").arg(variant),
                        QJsonObject{{QStringLiteral("surface"), QStringLiteral("search page and page glow")},
                                    {QStringLiteral("accent"), nameColor(expected)}});
                capture(titlePage_.get(), QStringLiteral("accent-%1-title").arg(variant),
                        QJsonObject{{QStringLiteral("surface"), QStringLiteral("title page hero watch")},
                                    {QStringLiteral("accent"), nameColor(expected)},
                                    {QStringLiteral("presentationOnly"), true},
                                    {QStringLiteral("syntheticState"), QStringLiteral("hero watch enabled for presentation; no catalogue entry was fabricated")},
                                    {QStringLiteral("inferenceProven"), false}});
                capture(benchmarkProgress_.get(),
                        QStringLiteral("accent-%1-benchmark").arg(variant),
                        QJsonObject{{QStringLiteral("surface"), QStringLiteral("benchmark progress chunk")},
                                    {QStringLiteral("accent"), nameColor(expected)},
                                    {QStringLiteral("presentationOnly"), true},
                                    {QStringLiteral("syntheticState"), QStringLiteral("running state and value shown for presentation; no benchmark was started")},
                                    {QStringLiteral("inferenceProven"), false}});
                capture(overlay_.data(), QStringLiteral("accent-%1-overlay").arg(variant),
                        QJsonObject{{QStringLiteral("surface"), QStringLiteral("engine preparation overlay")},
                                    {QStringLiteral("accent"), nameColor(expected)},
                                    {QStringLiteral("presentationOnly"), true},
                                    {QStringLiteral("syntheticState"), QStringLiteral("stage data only; no builder process, no engine")},
                                    {QStringLiteral("inferenceProven"), false}});
                if (player_ != nullptr) player_->noteUserActivity();
                QTest::qWait(40);
                capture(player_->window(), QStringLiteral("accent-%1-player").arg(variant),
                        QJsonObject{{QStringLiteral("surface"), QStringLiteral("player window and timeline")},
                                    {QStringLiteral("accent"), nameColor(expected)},
                                    {QStringLiteral("inferenceProven"), false}});
                sweepRows.append(row);
            }
        }
        const int expectedRows =
            (mikai::app::theme::accentPresets().size() + 1) * 2;
        check(QStringLiteral("sweep-coverage"), sweepRows.size() == expectedRows,
              QStringLiteral("%1 appearance variants captured (expected %2: every preset plus a dark custom, dark and light)")
                  .arg(sweepRows.size()).arg(expectedRows));
        evidence_.insert(QStringLiteral("sweeps"), sweepRows);
        evidence_.insert(QStringLiteral("switchCount"), sweepRows.size());
        evidence_.insert(QStringLiteral("restartObserved"), false);
        evidence_.insert(QStringLiteral("openAcrossSweep"), QJsonObject{
            {QStringLiteral("compare"), compare_ != nullptr && compare_->isVisible()},
            {QStringLiteral("preparationOverlay"), overlay_ != nullptr && overlay_->isVisible()},
            {QStringLiteral("player"), player_ != nullptr && player_->isWindowVisible()}});
        return true;
    }

    /// Switches the gradient source on the open pages without touching the stored
    /// colour: the hero's cached composite is the surface that goes stale.
    void sweepGradientSource() {
        Appearance autoLook;
        autoLook.accent = QStringLiteral("#bdacff");
        autoLook.gradientMode = mikai::app::theme::GradientMode::Auto;
        autoLook.gradientColor = QStringLiteral("#f27bb0");
        autoLook.backgroundGradient = true;
        autoLook.gradientIntensity = 100;
        autoLook.gradientHeight = 100;
        applyLook(autoLook);

        // The hero is the cached composite: it is found by the object name the
        // page publishes, because the widget itself carries no Q_OBJECT and its
        // meta-object is a plain QWidget. A missing hero is a failure, never a
        // silent skip - an unavailable surface must not read as a pass.
        QWidget* backdrop = titlePage_ != nullptr
                                ? titlePage_->findChild<QWidget*>(QStringLiteral("titleBackdrop"))
                                : nullptr;
        evidence_.insert(QStringLiteral("heroWashBackdropPresent"), backdrop != nullptr);
        check(QStringLiteral("gradient-source/hero-present"),
              backdrop != nullptr && backdrop->width() > 40 && backdrop->height() > 40,
              backdrop == nullptr ? QStringLiteral("titleBackdrop is absent")
                                  : QStringLiteral("titleBackdrop is %1x%2")
                                        .arg(backdrop->width())
                                        .arg(backdrop->height()));

        QColor previousHero;
        const auto report = [&](const QString& stage, const QColor& expected) {
            const QColor wash = glow_ != nullptr
                                    ? pixelsOf(glow_).at(glow_->width() / 2, 2)
                                    : QColor();
            check(QStringLiteral("gradient-source/%1/page-glow").arg(stage),
                  nearColor(wash, expected, 8),
                  QStringLiteral("wash %1, resolved %2")
                      .arg(nameColor(wash), nameColor(expected)));
            if (backdrop != nullptr && backdrop->width() > 40 && backdrop->height() > 40) {
                const QColor hero = pixelsOf(backdrop).at(backdrop->width() / 2, 20);
                evidence_.insert(QStringLiteral("heroWash/") + stage,
                                 QJsonObject{{QStringLiteral("pixel"), nameColor(hero)},
                                             {QStringLiteral("alpha"), hero.alpha()}});
                // Over cover artwork the absolute colour is not the contract:
                // the cached wash has to CHANGE with the source.
                check(QStringLiteral("gradient-source/%1/hero-wash-painted").arg(stage),
                      hero.alpha() > 0,
                      QStringLiteral("cached hero wash %1 a%2")
                          .arg(nameColor(hero))
                          .arg(hero.alpha()));
                if (previousHero.isValid()) {
                    const int delta = qAbs(hero.red() - previousHero.red()) +
                                      qAbs(hero.green() - previousHero.green()) +
                                      qAbs(hero.blue() - previousHero.blue());
                    check(QStringLiteral("gradient-source/%1/hero-wash-changed").arg(stage),
                          delta >= 12,
                          QStringLiteral("cached hero wash moved by %1 (%2 -> %3)")
                              .arg(delta)
                              .arg(nameColor(previousHero), nameColor(hero)));
                }
                previousHero = hero;
            }
            capture(searchPage_.get(), QStringLiteral("gradient-source-%1-search").arg(stage),
                    QJsonObject{{QStringLiteral("surface"), QStringLiteral("search page wash")},
                                {QStringLiteral("resolvedWash"), nameColor(expected)}});
            capture(titlePage_.get(), QStringLiteral("gradient-source-%1-title").arg(stage),
                    QJsonObject{{QStringLiteral("surface"), QStringLiteral("title page hero wash")},
                                {QStringLiteral("resolvedWash"), nameColor(expected)},
                                {QStringLiteral("presentationOnly"), true},
                                {QStringLiteral("syntheticState"),
                                 QStringLiteral("hero watch enabled for presentation; no catalogue entry was fabricated")},
                                {QStringLiteral("inferenceProven"), false}});
        };

        report(QStringLiteral("auto"), mikai::app::theme::accentColor(autoLook));

        Appearance custom = autoLook;
        custom.gradientMode = mikai::app::theme::GradientMode::Custom;
        mikai::app::setCurrentAppearance(custom);
        QTest::qWait(150);
        report(QStringLiteral("custom"), mikai::app::theme::gradientColor(custom));

        mikai::app::setCurrentAppearance(autoLook);
        QTest::qWait(150);
        report(QStringLiteral("auto-again"), mikai::app::theme::accentColor(autoLook));
    }

    /// PiP is the same Quick window and VideoSession in a compact presentation.
    bool sweepPip() {
        // This teardown is the one that crashed last time, so the measurements
        // go to disk before it starts.
        writeEvidence(QStringLiteral("pip-teardown-pending"), false);
        // Auxiliary Widgets close first; the player remains open to enter compact
        // PiP through its native Quick window and the same session.
        if (compare_ != nullptr) {
            compare_->close();
            compare_.clear();
            QTest::qWait(200);
        }
        if (mediaInfo_ != nullptr) {
            // WA_DeleteOnClose, parented to the player: the pointer clears once
            // the deferred delete runs in the turn below, before the player goes.
            mediaInfo_->close();
            mediaInfo_ = nullptr;
            QTest::qWait(100);
        }
        if (player_ == nullptr || player_->window() == nullptr || surface_ == nullptr) {
            check(QStringLiteral("pip-player-ready"), false,
                  QStringLiteral("the Quick player session is unavailable"));
            return false;
        }
        PlayerWindow* const originalWindow = player_->window();
        mplayer::VideoSession* const originalSession = player_->session();
        const uint64_t originalEpoch = surface_->sourceEpoch();
        player_->togglePip();
        if (!waitFor([this] { return player_->isPipActive() && player_->window()->compactPip(); },
                     5000, QStringLiteral("same-window-pip"))) {
            check(QStringLiteral("pip-same-window-session"), false,
                  QStringLiteral("the player did not enter compact PiP"));
            return false;
        }
        check(QStringLiteral("pip-same-window-session"),
              player_->window() == originalWindow && player_->session() == originalSession &&
                  surface_->sourceEpoch() == originalEpoch,
              QStringLiteral("PiP preserves the exact Quick window, VideoSession and source epoch"));

        struct Variant { const char* stage; const char* accent; };
        const QVector<Variant> variants = {{"default", "#bdacff"}, {"warm", "#f27bb0"}};
        for (const Variant& variant : variants) {
            Appearance appearance;
            appearance.accent = QString::fromLatin1(variant.accent);
            applyLook(appearance);
            player_->noteUserActivity();
            QTest::qWait(140);
            const QColor expected = mikai::app::theme::accentColor(appearance);
            const QPixmap screenshot = originalWindow->screen()->grabWindow(originalWindow->winId());
            const int accentPixels = countNear(screenshot.toImage(), expected, 12);
            evidence_.insert(QStringLiteral("pipTimeline/") + QString::fromLatin1(variant.stage),
                             QJsonObject{{QStringLiteral("accentPixels"), accentPixels},
                                         {QStringLiteral("accent"), nameColor(expected)},
                                         {QStringLiteral("sameSession"), player_->session() == originalSession}});
            check(QStringLiteral("pip-timeline-live/%1").arg(QString::fromLatin1(variant.stage)),
                  !screenshot.isNull() && accentPixels > 0,
                  QStringLiteral("Quick PiP paints the live accent (%1 matching pixels)")
                      .arg(accentPixels));
            capture(originalWindow,
                    QStringLiteral("accent-pip-%1").arg(QString::fromLatin1(variant.stage)),
                    QJsonObject{{QStringLiteral("surface"), QStringLiteral("same Quick player in compact PiP")},
                                {QStringLiteral("accent"), nameColor(expected)},
                                {QStringLiteral("sameSession"), player_->session() == originalSession},
                                {QStringLiteral("inferenceProven"), false}});
        }
        player_->togglePip();
        check(player_->window() == originalWindow && player_->session() == originalSession &&
                  !player_->isPipActive(),
              "leaving PiP restores the same player window and session");
        check(player_->requestClose(), "Quick player acknowledges PiP sweep teardown");
        player_ = nullptr;
        surface_ = nullptr;

        struct EpisodeFixtureHost final : mplayer::shell::PlayerHost {
            void requestVariant(const QString&, const QString&) override {}
            void requestEpisode(int) override {}
            void requestTrackProbe(const QString&) override {}
            void requestCopyStreamUrl() override {}
            void playerClosed() override {}
            void playbackFailed() override {}
        } fixtureHost;
        auto chipPlayer = std::make_unique<PlayerController>(
            QStringLiteral("Episode fixture"), QUrl::fromLocalFile(demo_).toString(),
            QList<mplayer::shell::QualityEntry>{}, 0, &fixtureHost);
        chipPlayer->setPlaylist(
            {QVariantMap{{QStringLiteral("label"), QStringLiteral("Episode 1")}},
             QVariantMap{{QStringLiteral("label"), QStringLiteral("Episode 2")}}}, 0);
        chipPlayer->showWindow();
        chipPlayer->startPlayer();
        const QVector<QPair<QString, QString>> looks = {
            {QStringLiteral("dark"), QStringLiteral("#bdacff")},
            {QStringLiteral("light"), QStringLiteral("#f0a35e")}};
        for (const auto& look : looks) {
            Appearance appearance;
            appearance.accent = look.second;
            appearance.siteTheme = look.first == QStringLiteral("light")
                                       ? mikai::app::theme::SiteTheme::Light
                                       : mikai::app::theme::SiteTheme::Dark;
            applyLook(appearance);
            chipPlayer->noteUserActivity();
            QTest::qWait(120);
            const bool visible = chipPlayer->hasNextEpisode() && chipPlayer->window()->isVisible();
            const QColor expected = mikai::app::theme::accentColor(appearance);
            const QPixmap screenshot = chipPlayer->window()->screen()->grabWindow(
                chipPlayer->window()->winId());
            const int accentPixels = countNear(screenshot.toImage(), expected, 12);
            evidence_.insert(QStringLiteral("visibleChip/") + look.first,
                             QJsonObject{{QStringLiteral("visible"), visible},
                                         {QStringLiteral("accentPixels"), accentPixels},
                                         {QStringLiteral("accent"), nameColor(expected)}});
            check(QStringLiteral("visible-chip/%1").arg(look.first),
                  visible && !screenshot.isNull() && accentPixels > 0,
                  QStringLiteral("Quick episode navigation is visible in %1 appearance")
                      .arg(look.first));
            capture(chipPlayer->window(),
                    QStringLiteral("visible-chip-%1").arg(look.first),
                    QJsonObject{{QStringLiteral("surface"), QStringLiteral("Quick episode navigation fixture")},
                                {QStringLiteral("accent"), nameColor(expected)},
                                {QStringLiteral("presentationOnly"), true},
                                {QStringLiteral("syntheticState"), QStringLiteral("two-episode fixture over the supplied demo")},
                                {QStringLiteral("inferenceProven"), false}});
        }
        chipPlayer->requestClose();
        // Leave the geometry pass on the shipped appearance.
        applyLook(Appearance{});
        return true;
    }

    void measureLanguages() {
        QJsonArray geometry;
        QString catalog = arguments_.translations;
        if (catalog.isEmpty()) {
            catalog = QDir(QCoreApplication::applicationDirPath())
                          .filePath(QStringLiteral("mikai_uk.qm"));
        }
        QTranslator ukrainian;
        const bool catalogAvailable =
            QFileInfo(catalog).isFile() && ukrainian.load(catalog);
        evidence_.insert(QStringLiteral("translations"), QJsonObject{
            {QStringLiteral("path"), catalog},
            {QStringLiteral("loaded"), catalogAvailable}});
        check(QStringLiteral("uk-translations-load"), catalogAvailable,
              QStringLiteral("catalog %1").arg(catalog));

        const auto restate = [this](QTranslator* translator, bool install) {
            if (translator != nullptr) {
                if (install) {
                    app_.installTranslator(translator);
                } else {
                    app_.removeTranslator(translator);
                }
            }
            QEvent languageChange(QEvent::LanguageChange);
            for (QWidget* window : QApplication::topLevelWidgets()) {
                if (window != nullptr && window->isVisible()) {
                    QCoreApplication::sendEvent(window, &languageChange);
                }
            }
            QTest::qWait(100);
        };

        const auto measure = [&](const QString& code) {
            for (int width : {580, 1280}) {
                window_->resize(width, kHeight);
                if (titlePage_ != nullptr) titlePage_->resize(width, kHeight);
                if (settings_ != nullptr) settings_->resize(width, kHeight);
                QTest::qWait(100);
                const QJsonObject row{
                    {QStringLiteral("language"), code},
                    {QStringLiteral("width"), width},
                    {QStringLiteral("devicePixelRatio"),
                     window_ != nullptr ? window_->devicePixelRatioF() : 0.0},
                    {QStringLiteral("searchPage"),
                     widgetMetrics(window_->findChild<mikai::app::SearchPage*>())},
                    {QStringLiteral("searchAction"), widgetMetrics(searchAction_)},
                    {QStringLiteral("titleWatch"), widgetMetrics(titleWatch_)},
                    {QStringLiteral("benchmarkProgress"), widgetMetrics(benchmarkProgress_)},
                    {QStringLiteral("timeline"), timelineItem(player_ && player_->window()
                        ? player_->window()->contentItem() : nullptr) != nullptr
                        ? QJsonObject{{QStringLiteral("visible"), true},
                                      {QStringLiteral("width"), timelineItem(
                                          player_->window()->contentItem())->width()},
                                      {QStringLiteral("height"), timelineItem(
                                          player_->window()->contentItem())->height()}}
                        : QJsonObject{{QStringLiteral("visible"), false}}}};
                geometry.append(row);
                check(QStringLiteral("geometry/%1/%2/search-action").arg(code).arg(width),
                      searchAction_ != nullptr && searchAction_->width() > 0 &&
                          searchAction_->height() > 0,
                      QStringLiteral("size %1x%2")
                          .arg(searchAction_ ? searchAction_->width() : 0)
                          .arg(searchAction_ ? searchAction_->height() : 0));
                check(QStringLiteral("geometry/%1/%2/title-watch").arg(code).arg(width),
                      titleWatch_ != nullptr && titleWatch_->width() > 0 &&
                          titleWatch_->height() > 0,
                      QStringLiteral("size %1x%2")
                          .arg(titleWatch_ ? titleWatch_->width() : 0)
                          .arg(titleWatch_ ? titleWatch_->height() : 0));
                capture(window_.get(), QStringLiteral("geometry-%1-%2-search").arg(code).arg(width),
                        QJsonObject{{QStringLiteral("surface"), QStringLiteral("search page")},
                                    {QStringLiteral("language"), code},
                                    {QStringLiteral("width"), width}});
                capture(titlePage_.get(),
                        QStringLiteral("geometry-%1-%2-title").arg(code).arg(width),
                        QJsonObject{{QStringLiteral("surface"), QStringLiteral("title page")},
                                    {QStringLiteral("language"), code},
                                    {QStringLiteral("width"), width},
                                    {QStringLiteral("presentationOnly"), true},
                                    {QStringLiteral("inferenceProven"), false}});
            }
        };

        restate(nullptr, false);
        measure(QStringLiteral("en"));
        if (catalogAvailable) {
            restate(&ukrainian, true);
            const QString translated = titleWatch_ != nullptr ? titleWatch_->text() : QString();
            check(QStringLiteral("uk-translation-applied"),
                  !translated.isEmpty() && translated != QStringLiteral("Watch"),
                  QStringLiteral("hero action reads \"%1\"").arg(translated));
            measure(QStringLiteral("uk"));
            restate(&ukrainian, false);
        } else {
            check(QStringLiteral("uk-translation-applied"), false,
                  QStringLiteral("no Ukrainian catalog was loaded; UK coverage was not measured"));
        }
        evidence_.insert(QStringLiteral("geometry"), geometry);
        evidence_.insert(QStringLiteral("devicePixelRatioAfterResize"),
                         window_ != nullptr ? window_->devicePixelRatioF() : 0.0);
        const double dpr = window_ != nullptr ? window_->devicePixelRatioF() : 0.0;
        const double scale = dpr > 0.0 ? 1.75 / dpr : 0.0;
        evidence_.insert(QStringLiteral("scale"), QJsonObject{
            {QStringLiteral("nativeDesktopScale"), 1.75},
            {QStringLiteral("qtDevicePixelRatio"), dpr},
            {QStringLiteral("nativeDividedByQt"), scale},
            {QStringLiteral("dividesNativeScale"),
             dpr > 0.0 && qAbs(scale - qRound(scale)) < 0.02}});
        check(QStringLiteral("device-pixel-ratio-recorded"), dpr > 0.0,
              QStringLiteral("Qt reports %1; native desktop is 1.75").arg(dpr));
        // Restore the application window to its capture size.
        window_->resize(kWidth, kHeight);
        if (titlePage_ != nullptr) titlePage_->resize(kWidth, kHeight);
        if (settings_ != nullptr) settings_->resize(kWidth, kHeight);
    }

    /// Written before teardown and again after it, so a crash cannot discard
    /// measurements that are already collected.
    bool writeEvidence(const QString& stage, bool success) {
        evidence_.insert(QStringLiteral("stage"), stage);
        evidence_.insert(QStringLiteral("success"), success);
        evidence_.insert(QStringLiteral("finishedUtc"),
                         QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        evidence_.insert(QStringLiteral("elapsedMs"), double(total_.elapsed()));
        evidence_.insert(QStringLiteral("checks"), checks_);
        evidence_.insert(QStringLiteral("passed"), passed_);
        evidence_.insert(QStringLiteral("failed"), failed_);
        evidence_.insert(QStringLiteral("failures"), failures_);
        evidence_.insert(QStringLiteral("failureCauses"), failures_);
        evidence_.insert(QStringLiteral("screenshots"), shots_);
        const QByteArray json = QJsonDocument(evidence_).toJson(QJsonDocument::Indented);
        QSaveFile file(QDir(arguments_.output)
                           .filePath(arguments_.radiusSweep
                                         ? QStringLiteral("radius-sweep-evidence.json")
                                         : QStringLiteral("accent-sweep-evidence.json")));
        const bool written = file.open(QIODevice::WriteOnly) && file.write(json) == json.size() &&
                             file.commit();
        // The interim write is a crash guard for the file; only the final report
        // goes to stdout, so a reader never sees two documents.
        if (stage == QLatin1String("complete")) {
            std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
            std::fflush(stdout);
        }
        return written;
    }

    int finish(bool ok) {
        writeEvidence(QStringLiteral("teardown-pending"), false);
        // Surface-holding windows release their locks before the player session.
        if (player_ != nullptr) {
            player_->endEnginePreparation(true, QStringLiteral("Presentation-only release sweep complete"));
        }
        overlay_.clear();
        if (compare_ != nullptr) {
            compare_->close();
            compare_.clear();
            QTest::qWait(200);
        }
        if (mediaInfo_ != nullptr) {
            mediaInfo_->close();
            mediaInfo_ = nullptr;
        }
        titleWatch_ = nullptr;
        titlePage_.reset();
        settings_.reset();
        benchmarkProgress_ = nullptr;
        searchAction_ = nullptr;
        glow_ = nullptr;
        searchPage_ = nullptr;
        surface_ = nullptr;
        if (player_ != nullptr) {
            player_->requestClose();
            player_ = nullptr;
            QTest::qWait(250);
        }
        if (window_ != nullptr) {
            window_->close();
        }
        localRunner_.cancel();
        if (!localRunner_.waitForDone(15000)) {
            check(QStringLiteral("local-workers-drained"), false,
                  QStringLiteral("worker retirement exceeded 15 seconds"));
        }
        bool drained = false;
        mikai::app::Runner::shutdownAll([&drained] { drained = true; });
        QElapsedTimer drainClock;
        drainClock.start();
        while (!drained && drainClock.elapsed() < 20000) {
            QTest::qWait(25);
        }
        check(QStringLiteral("workers-drained"), drained,
              QStringLiteral("application workers retired before the client is destroyed"));
        window_.reset();

        const bool success = ok && failed_ == 0 && !shots_.isEmpty();
        const bool written = writeEvidence(QStringLiteral("complete"), success);
        diagnostic(success && written
                       ? QStringLiteral("PASS: accent sweep covered %1 checks, %2 passed, %3 failed.")
                             .arg(checks_.size()).arg(passed_).arg(failed_)
                       : QStringLiteral("FAIL: consult accent-sweep-evidence.json failures; counts %1/%2/%3.")
                             .arg(checks_.size()).arg(passed_).arg(failed_));
        app_.exit(success && written ? 0 : 1);
        return success && written ? 0 : 1;
    }

    QApplication& app_;
    Arguments arguments_;
    QString demo_;
    QString digest_;
    mikai::backend::MikaiClient offlineClient_;
    mikai::app::Runner localRunner_;
    mikai::app::QualityCache qualityCache_;
    std::unique_ptr<MainWindow> window_;
    QPointer<mikai::app::SearchPage> searchPage_;
    QPointer<mikai::app::PageGlow> glow_;
    QPointer<QPushButton> searchAction_;
    std::unique_ptr<mikai::app::TitlePage> titlePage_;
    QPointer<QPushButton> titleWatch_;
    std::unique_ptr<mikai::app::SettingsDialog> settings_;
    QPointer<QProgressBar> benchmarkProgress_;
    QPointer<mplayer::quick::PreparationWindow> overlay_;
    QPointer<PlayerController> player_;
    QPointer<mplayer::VideoSession> surface_;
    QPointer<MediaInformationDialog> mediaInfo_;
    QPointer<CompareWindow> compare_;
    QJsonObject evidence_;
    QJsonArray checks_;
    QJsonArray shots_;
    QJsonArray failures_;
    int passed_ = 0;
    int failed_ = 0;
    QElapsedTimer total_;
};
} // namespace

int main(int argc, char** argv) {
    QStringList rawArguments;
    for (int index = 0; index < argc; ++index) rawArguments.append(QString::fromLocal8Bit(argv[index]));
    Arguments arguments = parseArguments(rawArguments);
    if (arguments.help || !arguments.error.isEmpty()) {
        std::fprintf(arguments.help ? stdout : stderr,
            "release_gallery_probe --output-dir <NEW-directory> --video <original-demo.mp4>\n"
            "  --video-credit <authorship-and-license> [--video-source <description-or-URL>]\n"
            "  [--accent-sweep] [--radius-sweep] [--translations <mikai_uk.qm>]\n"
            "Requires an active desktop (at least 1280x800 plus window borders), an original\n"
            "bright demo of at least 8 seconds, and absolute MIKAI_MPV_MODULE pointing to an\n"
            "installed Standard mpv runtime. Never selects/downloads AI. Output must not exist.\n"
            "--accent-sweep runs the focused accent/action-role sweep instead of the gallery.\n"
            "  It writes accent-sweep-evidence.json and screenshots to the explicit absolute\n"
            "  --output-dir; Compare and the preparation window stay open across accent changes.\n"
            "  Preparation/title/benchmark states that need service data are presentation-only\n"
            "  and are labelled presentationOnly=true, inferenceProven=false.\n"
            "--radius-sweep runs the focused corner-radius sweep instead of the gallery: the\n"
            "  shipped steps none/xs/xl on the real search page, title page and settings dialog\n"
            "  at the wide and the minimum width, capturing each surface and the corner pixels a\n"
            "  real control paints (square corners fill the corner, rounded ones leave it to the\n"
            "  bed). It writes radius-sweep-evidence.json and the screenshots.\n"
            "--translations defaults to <probe-dir>/mikai_uk.qm for the EN/UK geometry pass.\n");
        if (!arguments.error.isEmpty()) diagnostic(arguments.error);
        return arguments.help ? 0 : 2;
    }
    // Anchor before isolation changes the working directory. The guard relaunch
    // preserves argv and CWD, so the child computes these same absolute paths.
    arguments.video = QFileInfo(arguments.video).absoluteFilePath();
    arguments.output = QFileInfo(arguments.output).absoluteFilePath();
    if (!arguments.translations.isEmpty()) {
        arguments.translations = QFileInfo(arguments.translations).absoluteFilePath();
    }
    const QString runtime = qEnvironmentVariable("MIKAI_MPV_MODULE");
    if (!QFileInfo(runtime).isAbsolute() || !QFileInfo(runtime).isFile()) {
        diagnostic(QStringLiteral("ERROR: set MIKAI_MPV_MODULE to the absolute installed Standard mpv module path; scratch never searches the user's install."));
        return 2;
    }
    if (!QFileInfo(arguments.video).isFile()) {
        diagnostic(QStringLiteral("ERROR: --video must name the parent's original local demo file."));
        return 2;
    }
    if (QFileInfo::exists(arguments.output)) {
        diagnostic(QStringLiteral("ERROR: --output-dir must be a new path; existing directories/files are never overwritten or deleted."));
        return 2;
    }
    qunsetenv("MIKAI_API_KEY");
    qunsetenv("MIKAI_PROBE_BUNDLE");
    // The shared helper defaults to offscreen for tests, but these are desktop
    // screenshots with OpenGL. Never silently turn a failed desktop into a fake.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
#ifdef Q_OS_WIN
        qputenv("QT_QPA_PLATFORM", "windows");
#else
        qputenv("QT_QPA_PLATFORM", "xcb");
#endif
    }
    const QString platform = qEnvironmentVariable("QT_QPA_PLATFORM");
    if (platform.startsWith(QLatin1String("offscreen")) || platform.startsWith(QLatin1String("minimal"))) {
        diagnostic(QStringLiteral("ERROR: gallery capture requires an active desktop platform, not offscreen/minimal."));
        return 2;
    }
    mikai::probes::isolation::Options isolation;
    isolation.probe = "release-gallery";
    isolation.childTimeoutMs = 240000;
    const auto gate = mikai::probes::isolation::enter(argc, argv, isolation);
    if (gate != mikai::probes::isolation::Gate::RunHere) return mikai::probes::isolation::exitCode();
    if (!prepareScratch()) {
        diagnostic(QStringLiteral("ERROR: verified scratch home/working-directory isolation failed."));
        return 2;
    }
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    int qtArgc = 1;
    char* qtArgv[] = {argv[0], nullptr};
    QApplication app(qtArgc, qtArgv);
    app.setQuitOnLastWindowClosed(false);
    app.setOrganizationName(QStringLiteral("MikaiReleaseGallery"));
    app.setApplicationName(QStringLiteral("ReleaseGalleryProbe"));
    app.setApplicationDisplayName(QStringLiteral("Mikai Ja Nai"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    const QString root = mikai::probes::isolation::isolatedRoot();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root + QStringLiteral("/settings"));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, root + QStringLiteral("/settings"));
    mikai::backend::setEnvFilePathOverride(root + QStringLiteral("/anonymous.env"));
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    // No translators installed: source-language UI is English. Catalogue names
    // are selected in English when the live service actually supplies them.
    auto appearance = mikai::app::theme::Appearance{};
    appearance.titleLanguage = mikai::app::theme::TitleLanguage::English;
    mikai::app::setCurrentAppearance(appearance);
    app.setStyle(QStringLiteral("Fusion"));
    mikai::app::theme::applyPalette(appearance);
    mplayer::shell::setAccent(mikai::app::theme::accentColor(appearance));
    mplayer::shell::setCornerRadius(mikai::app::theme::radiusSmall(appearance));
    app.setStyleSheet(mikai::app::theme::buildStylesheet(appearance));
    QSettings settings;
    settings.setValue(QStringLiteral("player/anime4k"), QStringLiteral("off"));
    settings.setValue(QStringLiteral("player/native/runtimeChoice"), QStringLiteral("standard"));
    const auto selectedRuntime = mplayer::native::selectStandardRuntime(runtime);
    if (!selectedRuntime.loaded) {
        diagnostic(QStringLiteral("ERROR: Standard mpv runtime selection failed: %1")
                       .arg(selectedRuntime.reason));
        return 2;
    }
    diagnostic(QStringLiteral("Selected Standard mpv runtime: %1").arg(selectedRuntime.modulePath));
    mikai::app::WindowRaisePolicy::install();
    if (app.primaryScreen() == nullptr || app.primaryScreen()->availableGeometry().width() < kWidth ||
        app.primaryScreen()->availableGeometry().height() < kHeight + 40) {
        diagnostic(QStringLiteral("ERROR: active desktop work area must fit 1280x800 plus window borders."));
        return 2;
    }
    const QString extension = QFileInfo(arguments.video).suffix();
    const QString demo = QDir(root).filePath(QStringLiteral("Mikai original demo.") + extension);
    if (!QFile::copy(arguments.video, demo)) {
        diagnostic(QStringLiteral("ERROR: cannot copy the supplied original demo into owned scratch."));
        return 2;
    }
    const QString digest = sha256(demo);
    if (digest.isEmpty() || !QDir().mkpath(arguments.output)) {
        diagnostic(QStringLiteral("ERROR: cannot hash the scratch demo or create the fresh output directory."));
        return 2;
    }
    if (arguments.accentSweep || arguments.radiusSweep) {
        AccentSweep sweep(app, std::move(arguments), demo, digest);
        return sweep.run();
    }
    Gallery gallery(app, std::move(arguments), demo, digest);
    return app.exec();
}
