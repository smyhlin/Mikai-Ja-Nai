// Release-only driver for real Widgets surfaces. This is not a product CLI.
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScreen>
#include <QSlider>
#include <QSettings>
#include <QTest>
#include <QTimer>

#include <cstdio>
#include <memory>
#include <utility>

#include "ProbeIsolation.hpp"
#include "ProbePicture.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "WindowRaisePolicy.hpp"
#include "backend/MikaiClient.hpp"
#include "dialogs/SettingsDialog.hpp"
#include "pages/MainWindow.hpp"
#include "pages/SearchPage.hpp"
#include "state/AppearanceStore.hpp"
#include "theme/Theme.hpp"
#include "widgets/TitleCard.hpp"
#include "workers/Runner.hpp"
#include "CompareWindow.hpp"
#include "MediaInformationDialog.hpp"
#include "PlayerDialog.hpp"
#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
#include "ShellTheme.hpp"
#include "VideoSurface.hpp"

namespace {
using mikai::app::MainWindow;
using mplayer::shell::CompareWindow;
using mplayer::shell::MediaInformationDialog;
using mplayer::shell::PlayerDialog;
using mplayer::shell::PlayerShell;

constexpr int kWidth = 1280;
constexpr int kHeight = 800;

struct Arguments {
    QString output;
    QString video;
    QString credit;
    QString source;
    QString error;
    bool help = false;
};

Arguments parseArguments(const QStringList& arguments) {
    Arguments result;
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const QString option = arguments[index];
        if (option == QLatin1String("--help")) {
            result.help = true;
            continue;
        }
        QString* value = nullptr;
        if (option == QLatin1String("--output-dir")) value = &result.output;
        else if (option == QLatin1String("--video")) value = &result.video;
        else if (option == QLatin1String("--video-credit")) value = &result.credit;
        else if (option == QLatin1String("--video-source")) value = &result.source;
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

    bool pictureAction(const QString& payload) {
        if (shell_ == nullptr || player_ == nullptr) return false;
        shell_->openPictureMenu(shell_->rect().center());
        auto* menu = player_->findChild<mplayer::shell::MenuPopup*>(QStringLiteral("playerPictureMenu"));
        if (menu == nullptr || !menu->isVisible()) return false;
        for (int index = 0; index < menu->rowCount(); ++index) {
            if (menu->entryAt(index).payload.toString() != payload) continue;
            auto* button = qobject_cast<QPushButton*>(menu->rowWidget(index));
            if (button == nullptr || !button->isEnabled() || !button->isVisible()) return false;
            // Activate the real menu row, not the host's signal or a private slot.
            button->click();
            return true;
        }
        return false;
    }

    void startPlayback() {
        if (!window_->openMedia(demo_)) {
            stop(QStringLiteral("The application refused the original demo video."));
            return;
        }
        player_ = window_->currentMediaPlayer();
        if (player_ == nullptr) {
            stop(QStringLiteral("openMedia created no player."));
            return;
        }
        fit(player_);
        shell_ = player_->findChild<PlayerShell*>();
        surface_ = player_->findChild<mplayer::VideoSurface*>();
        if (shell_ == nullptr || surface_ == nullptr) {
            stop(QStringLiteral("The real player is missing its shell or video surface."));
            return;
        }
        QObject::connect(surface_, &mplayer::VideoSurface::errorOccurred, this,
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
                stop(QStringLiteral("Demo playback did not expose a frame, tracks and at least eight seconds of duration in 30 seconds. Runtime: ") +
                     mplayer::native::runtimeUnavailablePlaybackText(QString()));
            }
            break;
        case Stage::Pause:
            if (surface_->position() > firstPosition_ + 0.35) {
                const auto picture = mplayer::probes::measurePicture(surface_->grabVideoFramebuffer(),
                    surface_->videoWidth(), surface_->videoHeight());
                if (picture.isMostlyBlack()) {
                    stop(QStringLiteral("The original demo rendered a mostly black video framebuffer; refusing a misleading player screenshot."));
                    break;
                }
                evidence_.insert(QStringLiteral("playbackSmoke"), QJsonObject{
                    {QStringLiteral("clockAdvanced"), true},
                    {QStringLiteral("fromSeconds"), firstPosition_},
                    {QStringLiteral("toSeconds"), surface_->position()},
                    {QStringLiteral("pictureLitFraction"), picture.litFraction()}});
                auto* pause = player_->findChild<QPushButton*>(QStringLiteral("playerPlayToggle"));
                if (pause == nullptr || !pause->isVisible() || !pause->isEnabled()) {
                    stop(QStringLiteral("The real pause control is unavailable."));
                    break;
                }
                pause->click();
                shell_->wake();
                next(Stage::PlayerShot);
            } else if (stageClock_.elapsed() > 10000) {
                stop(QStringLiteral("The demo clock did not advance while playing."));
            }
            break;
        case Stage::PlayerShot:
            if (surface_->pauseObserved() && !surface_->isPlaying() && stageClock_.elapsed() >= 700) {
                shell_->wake();
                capture(player_, QStringLiteral("player"), playbackState());
                next(Stage::CompareOpen);
            } else if (stageClock_.elapsed() > 6000) {
                stop(QStringLiteral("The real pause action did not settle."));
            }
            break;
        case Stage::CompareOpen:
            next(Stage::CompareReady);
            if (!pictureAction(QStringLiteral("compare"))) stop(QStringLiteral("The real Compare quality menu row could not be activated."));
            break;
        case Stage::CompareReady: {
            compare_ = player_->findChild<CompareWindow*>();
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
            if (!pictureAction(QStringLiteral("information"))) stop(QStringLiteral("The real Media information menu row could not be activated."));
            break;
        case Stage::InformationRead: {
            information_ = player_->findChild<MediaInformationDialog*>();
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
                player_->hide();
                window_->show();
                window_->raise();
                window_->activateWindow();
                next(Stage::SettingsOpen);
            }
            break;
        case Stage::SettingsOpen: {
            QPushButton* settingsButton = nullptr;
            for (auto* button : window_->findChildren<QPushButton*>()) {
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
    QPointer<PlayerDialog> player_;
    QPointer<PlayerShell> shell_;
    QPointer<mplayer::VideoSurface> surface_;
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
} // namespace

int main(int argc, char** argv) {
    QStringList rawArguments;
    for (int index = 0; index < argc; ++index) rawArguments.append(QString::fromLocal8Bit(argv[index]));
    Arguments arguments = parseArguments(rawArguments);
    if (arguments.help || !arguments.error.isEmpty()) {
        std::fprintf(arguments.help ? stdout : stderr,
            "release_gallery_probe --output-dir <NEW-directory> --video <original-demo.mp4>\n"
            "  --video-credit <authorship-and-license> [--video-source <description-or-URL>]\n"
            "Requires an active desktop (at least 1280x800 plus window borders), an original\n"
            "bright demo of at least 8 seconds, and absolute MIKAI_MPV_MODULE pointing to an\n"
            "installed Standard mpv runtime. Never selects/downloads AI. Output must not exist.\n");
        if (!arguments.error.isEmpty()) diagnostic(arguments.error);
        return arguments.help ? 0 : 2;
    }
    // Anchor before isolation changes the working directory. The guard relaunch
    // preserves argv and CWD, so the child computes these same absolute paths.
    arguments.video = QFileInfo(arguments.video).absoluteFilePath();
    arguments.output = QFileInfo(arguments.output).absoluteFilePath();
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
    app.setApplicationDisplayName(QStringLiteral("Mikai"));
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
    Gallery gallery(app, std::move(arguments), demo, digest);
    return app.exec();
}
