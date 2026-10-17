// The mikai player dialog: the composition root that hosts PlayerShell and
// carries the three panels the shell has no room for (quality list, dub/sub
// picker, episode drawer) plus the status panel, the feature strip and the PiP
// float.
//
// Two things the Python needed are gone on purpose. It gave mpv a native
// winId, so every floating panel carried WA_NativeWindow and was restacked by
// delayed re-raise passes; VideoSurface is a QOpenGLWidget, so the panels are
// ordinary siblings and z-order is Qt's problem. And it drove playback through
// PlayerController, a python-mpv wrapper; here the shell's signals go straight
// to the surface, and the state the shell needs is pushed back from the
// surface's own signals.

#include "CompareWindow.hpp"
#include "PlayerDialog.hpp"

#include <QSettings>

#include <QApplication>
#include <QAccessible>
#include <QMetaObject>
#include <QThreadPool>
#include <QRunnable>

#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
#include "PresetShortcuts.hpp"
#include "gui/Clipboard.hpp"
#include "ShellSliders.hpp"
#include "ShellTheme.hpp"
#include "ShellWidgets.hpp"
#include "VideoSurface.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QEasingCurve>
#include <QFileInfo>
#include <QFrame>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QRegion>
#include <QResizeEvent>
#include <QScreen>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <memory>

namespace mplayer {
namespace shell {

namespace {

// ── theme constants, from mikai_ui/theme ─────────────────────────────────
// The panel QSS is lifted verbatim from the Python, so the colours it names
// are mikai's, not the shell's painted palette.

constexpr auto kText = "#e7e7ea";
constexpr auto kMuted = "#a0a0ab";
constexpr auto kBorder = "#30303a";
constexpr auto kSurfaceActive = "#2b2b35";
constexpr auto kGhostHover = "rgba(231, 231, 234, 0.08)";
constexpr auto kIconDisabled = "rgba(160, 160, 171, 0.45)";

constexpr int kSpacingXS = 4;
constexpr int kSpacingSmall = 8;
constexpr int kSpacingMedium = 12;

constexpr int kIconSize = 18;
constexpr int kPlayerMinWidth = 860;
constexpr int kPlayerMinHeight = 520;
constexpr int kPlayerDefaultWidth = 1120;
constexpr int kPlayerDefaultHeight = 700;
constexpr int kPipDefaultWidth = 480;
constexpr int kPipDefaultHeight = 270;

// The shell's speed control tops out at 2.0x. The engine has no such limit, so
// only the value handed to the shell is clamped; mpv gets what was asked for.
constexpr double kShellRateMin = 0.25;
constexpr double kShellRateMax = 2.0;

constexpr auto kAnime4KSettingKey = "player/anime4k";
constexpr auto kAnimeJanaiPresetSettingKey = "player/animejanai/preset";
constexpr auto kAnimeJanaiBackendSettingKey = "player/animejanai/backend";

QString animeJanaiBackendName(native::Backend backend) {
    return backend == native::Backend::DirectML ? QStringLiteral("directml")
                                                 : QStringLiteral("tensorrt");
}

constexpr int kSeriesWidth = 190;
constexpr int kStripHeight = 30;
constexpr int kStatusMs = 1900;
constexpr int kStripMargin = 8;

QString chipQss() {
    return QStringLiteral(
               "QPushButton {"
               "  color: %1;"
               "  background: rgba(11, 11, 15, 0.72);"
               "  border: 1px solid %2;"
               "  border-radius: 8px;"
               "  padding: 0 %3px;"
               "  font-size: 12px;"
               "}"
               "QPushButton:hover {"
               "  background: rgba(231, 231, 234, 0.12);"
               "}")
        .arg(kText, kBorder)
        .arg(kSpacingSmall);
}

QString panelQss() {
    return QStringLiteral(
               "QFrame[chromePanel=\"true\"] {"
               "  background-color: rgba(11, 11, 15, 0.92);"
               "  border: 1px solid %1;"
               "  border-radius: 10px;"
               "}")
        .arg(kBorder);
}

QString statusTextQss() { return QStringLiteral("color: %1; background: transparent;").arg(kText); }

QPushButton* makeChip(QWidget* parent, const QString& name, const QString& text) {
    auto* button = new QPushButton(text, parent);
    button->setObjectName(name);
    button->setFixedHeight(kStripHeight);
    button->setCursor(Qt::PointingHandCursor);
    // A focused button would swallow Space, and Space is play/pause.
    button->setFocusPolicy(Qt::NoFocus);
    button->setStyleSheet(chipQss());
    return button;
}


/// Rasterises one painted glyph into an icon. IconPainter already draws into a
/// 24-unit grid, so this is a straight scale-and-tint with no SVG pipeline.
QIcon glyphIcon(void (*glyph)(QPainter&, qreal, qreal, qreal, const QColor&), qreal size) {
    const QColor normal(QStringLiteral("#ffffff"));
    const QColor disabled(kIconDisabled);
    QIcon icon;
    for (const QColor& color : {normal, disabled}) {
        const QIcon::Mode mode = (color == normal) ? QIcon::Normal : QIcon::Disabled;
        for (const int dpr : {1, 2, 3}) {
            const int px = std::max(1, static_cast<int>(std::lround(size * dpr)));
            QImage img(QSize(px, px), QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            {
                QPainter painter(&img);
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                painter.scale(dpr, dpr);
                glyph(painter, 0.0, 0.0, size, color);
            }
            QPixmap pixmap = QPixmap::fromImage(img);
            pixmap.setDevicePixelRatio(dpr);
            icon.addPixmap(pixmap, mode);
        }
    }
    return icon;
}

}  // namespace


// ═══════════════════════════════════════════════════════════════════════════
//  PlayerDialog
// ═══════════════════════════════════════════════════════════════════════════

PlayerDialog::PlayerDialog(const QString& caption, const QString& url, QWidget* parent)
    : PlayerDialog(caption, url, {}, 0, nullptr, parent) {}

PlayerDialog::PlayerDialog(const QString& caption,
                           const QString& url,
                           const QList<QualityEntry>& qualities,
                           int initialHeight,
                           PlayerHost* host,
                           QWidget* parent)
    : QDialog(parent),
      host_(host),
      directMedia_(host == nullptr),
      caption_(caption),
      url_(url),
      qualities_(usableQualities(qualities)),
      currentEmbed_(),
      currentHeight_(initialHeight),
      episodeIndex_(0),
      aspect_(kAspectFallback),
      volume_(0.7),
      speed_(1.0),
      muted_(false),
      fullscreen_(true),
      seriesExpanded_(false),
      errorActive_(false),
      refreshAttempted_(false),
      closed_(false),
      forwardingKey_(false),
      pipWasFullscreen_(false) {
    setObjectName(QStringLiteral("playerWindow"));
    setWindowTitle(tr("Player — %1").arg(caption_));
    setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                   Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setMinimumSize(kPlayerMinWidth, kPlayerMinHeight);

    placeWindowed();
    setContentsMargins(0, 0, 0, 0);

    shell_ = new PlayerShell(this, false);
    shell_->setFocusPolicy(Qt::StrongFocus);
    surface_ = shell_->videoSurface();

    const QString initialEpisode = !directMedia_ && caption_.contains(QStringLiteral(" · "))
                                       ? caption_.split(QStringLiteral(" · ")).first().trimmed()
                                       : caption_;
    if (!initialEpisode.isEmpty()) {
        shell_->setEpisodeLabel(initialEpisode);
    }

    // Applied before any file is opened: the surface builds its mpv core on the
    // first open() and glsl-shaders is only honoured as a construction option,
    // so a mode restored later would land on a core that already ignored it.
    QSettings animeJanaiSettings;
    animeJanaiPresetId_ = animeJanaiSettings.value(kAnimeJanaiPresetSettingKey).toString();
    const QString storedAnimeJanaiBackend =
        animeJanaiSettings.value(kAnimeJanaiBackendSettingKey).toString();
    if (const auto backend = native::backendFromName(storedAnimeJanaiBackend)) {
        animeJanaiBackend_ = *backend;
    } else {
        // Native's selected preference is an initial default only; subsequent
        // AnimeJaNai backend changes are explicit and persisted independently.
        animeJanaiBackend_ = native::loadNativePreferences().backend;
    }
    const QString storedChain = QSettings().value(kAnime4KSettingKey).toString();
    // Resolves the experiments too: every catalogue entry has an id.
    anime4kMode_ = anime4k::modeForId(storedChain);
    // A chain with no Mode is stored under the same key, so the id is loaded
    // even when the enum came back as Off: a single .glsl is a real setting
    // and reading only the mode would drop it on every start.
    anime4kChainId_ = storedChain.isEmpty() ? modeId(anime4kMode_) : storedChain;
    surface_->setAnime4KMode(anime4kMode_);
    // A stored chain that is not one of the presets has no Mode, so the enum
    // above came back Off and the shader would be dropped. The id is applied
    // after it, which is also the order the setter expects: the chain wins.
    if (!anime4kChainId_.isEmpty() && anime4kChainId_ != QLatin1String("off")) {
        surface_->setAnime4KChain(anime4kChainId_);
    }
    shell_->setAnime4KMode(anime4kMode_);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(shell_);
    // Every key belongs to the shell, the one widget that acts on them: the
    // panels are all NoFocus, so this is what routes Space to play/pause.
    setFocusProxy(shell_);
    // Win32 delivers activation to the top level and a changeEvent override
    // never sees it, so watch our own events. Without this, returning from
    // Alt+Tab leaves the window active but nothing focused, and the shell's
    // keys (Space, the arrows) do nothing until the user clicks the picture.
    installEventFilter(this);
    // Without tracking the dialog never hears a bare move - its children cover
    // it - and the top-level wake in eventFilter would be dead code.
    setMouseTracking(true);

    buildStrip();
    buildStatus();

    seriesDrawer_ = new SeriesDrawer(this);
    audioPopup_ = new MenuPopup(tr("AUDIO"), this);
    audioPopup_->setObjectName(QStringLiteral("playerAudioPopup"));
    subtitlesPopup_ = new MenuPopup(tr("SUBTITLES"), this);
    subtitlesPopup_->setObjectName(QStringLiteral("playerSubtitlesPopup"));
    // These two are the dialog's, but they are overlays all the same: a click
    // on the picture has to dismiss them, and one left open has to keep the
    // chrome awake. They are children of the dialog and are built after the
    // shell exists, so the shell cannot register them itself.
    shell_->registerOverlay(audioPopup_);
    shell_->registerOverlay(subtitlesPopup_);

    connect(audioPopup_, &MenuPopup::picked, this, &PlayerDialog::onTrackPicked);
    connect(subtitlesPopup_, &MenuPopup::picked, this, &PlayerDialog::onTrackPicked);
    connect(seriesDrawer_, &SeriesDrawer::picked, this,
            [this](int spot) { goToEpisode(spot, true); });

    watchPanels();
    connectShell();
    connectEngine();
    loadPresetShortcuts();
    refreshNativeInputs();
    connect(surface_, &VideoSurface::animeJanaiStatusChanged, this,
            &PlayerDialog::onAnimeJanaiStatusChanged);
    connect(surface_, &VideoSurface::animeJanaiFailure, this,
            &PlayerDialog::onAnimeJanaiFailure);
    connect(surface_, &VideoSurface::animeJanaiEngineBuildChanged, this,
            &PlayerDialog::onAnimeJanaiEngineBuildChanged);
    shell_->setAnimeJanaiBackend(animeJanaiBackendName(animeJanaiBackend_));
    shell_->setAnimeJanaiSelection(animeJanaiPresetId_, tr("Unavailable · not prepared"));
    if (!animeJanaiPresetId_.isEmpty()) {
        applyAnimeJanaiRequest(animeJanaiPresetId_);
    }

    // A child widget is shown when its parent is, so the panels that start
    // closed have to say so: an empty episode drawer and a retry chip would
    // otherwise appear over the picture on the first paint.
    seriesDrawer_->hide();
    retryButton_->hide();
    publishQualityOptions();
    setPlaylist({});
    setVariants({}, {});
    layoutPanels();
}

PlayerDialog::~PlayerDialog() {
    closed_ = true;
    // Child teardown (PlayerShell/surface_ destruction in ~QDialog) emits
    // nativeStatusChanged and friends through VideoSurface's reader detach.
    // Derived PlayerDialog members are already destroyed at that point, so a
    // live connection would run onNativeStatusChanged on a half-destroyed
    // receiver and corrupt the heap. Disconnect receiver-side first, before
    // member destruction can reach the children's destructors.
    if (surface_ != nullptr) {
        disconnect(surface_, nullptr, this, nullptr);
    }
    if (pip_ != nullptr && pip_->videoSurface() != nullptr &&
        (pipPool_ == nullptr || pip_->videoSurface() != pipPool_->videoSurface())) {
        disconnect(pip_->videoSurface(), nullptr, this, nullptr);
    }
    if (pipPool_ != nullptr && pipPool_->videoSurface() != nullptr) {
        disconnect(pipPool_->videoSurface(), nullptr, this, nullptr);
    }
    enginePreparationActive_ = false;
    // The overlay is a Qt::Tool child window: QObject parenting deletes it
    // here, but a top-level tool is not hidden by the parent hide/close, so
    // hide it explicitly rather than leaving a visible tool behind until the
    // deleteLater tick. No resume: the builder owns acknowledgement.
    if (processingOverlay_ != nullptr) {
        processingOverlay_->hide();
    }
    if (enginePreparationSurface_ != nullptr) {
        enginePreparationSurface_->setEnginePreparationPaused(false, false);
        enginePreparationSurface_.clear();
    }
    ++animeJanaiSequence_;
    if (animeJanaiCancellation_) {
        animeJanaiCancellation_->store(true);
    }
    if (statusTimer_) {
        statusTimer_->stop();
    }
    if (audioPopup_) {
        audioPopup_->close();
    }
    if (subtitlesPopup_) {
        subtitlesPopup_->close();
    }
    // A destructor cannot wait for an acknowledgement, so a native transition that
    // is still in flight here is named instead of being silently destroyed:
    // forced destruction is not a stop acknowledgement.
    if (surface_ != nullptr && !surface_->nativePlaybackStatus().pendingRequestId.isEmpty()) {
        qWarning().noquote() << "mikai: native transition in flight at dialog destruction ("
                             << surface_->nativePlaybackStatus().pendingRequestId
                             << "); forced teardown, not an acknowledgement";
    }
    releasePipPool();
}

// ── construction ─────────────────────────────────────────────────────────

void PlayerDialog::buildStrip() {
    strip_ = new QWidget(this);
    strip_->setObjectName(QStringLiteral("playerFeatureStrip"));
    strip_->setAttribute(Qt::WA_TranslucentBackground);

    auto* row = new QHBoxLayout(strip_);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(kSpacingXS);

    seriesButton_ = new QPushButton(strip_);
    seriesButton_->setObjectName(QStringLiteral("playerSeriesToggle"));
    seriesButton_->setFixedSize(kStripHeight, kStripHeight);
    seriesButton_->setCursor(Qt::PointingHandCursor);
    seriesButton_->setToolTip(tr("Episode list (S)"));
    seriesButton_->setAccessibleName(tr("Episode list"));
    seriesButton_->setFocusPolicy(Qt::NoFocus);
    seriesButton_->setStyleSheet(chipQss());
    connect(seriesButton_, &QPushButton::clicked, this, &PlayerDialog::toggleSeries);
    updateSeriesToggle();
    row->addWidget(seriesButton_);

}

void PlayerDialog::buildStatus() {
    statusPanel_ = new QFrame(this);
    statusPanel_->setObjectName(QStringLiteral("playerStatusPanel"));
    statusPanel_->setProperty("chromePanel", true);
    statusPanel_->setStyleSheet(panelQss());

    auto* row = new QHBoxLayout(statusPanel_);
    row->setContentsMargins(kSpacingSmall, kSpacingXS, kSpacingXS, kSpacingXS);
    row->setSpacing(kSpacingSmall);

    statusLabel_ = new QLabel(QString(), statusPanel_);
    statusLabel_->setObjectName(QStringLiteral("playerStatusText"));
    statusLabel_->setStyleSheet(statusTextQss());
    statusLabel_->setWordWrap(true);
    statusLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    row->addWidget(statusLabel_, 1);

    retryButton_ =
        makeChip(statusPanel_, QStringLiteral("playerRetryButton"), tr("Retry"));
    retryButton_->setToolTip(tr("Retry playback"));
    retryButton_->setAccessibleName(tr("Retry playback"));
    connect(retryButton_, &QPushButton::clicked, this, &PlayerDialog::retryPlayback);
    row->addWidget(retryButton_, 0);

    const auto addChip = [&](const QString& name, const QString& text,
                           const QString& tip, void (PlayerDialog::*handler)()) {
        auto* button = makeChip(statusPanel_, name, text);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        connect(button, &QPushButton::clicked, this, handler);
        row->addWidget(button, 0);
        button->hide();
        return button;
    };
    processingPrepareButton_ = addChip(
        QStringLiteral("playerPrepareButton"), tr("Prepare"),
        tr("Prepare the selected recipe"), &PlayerDialog::onProcessingPrepareRequested);
    processingProvidersButton_ = addChip(
        QStringLiteral("playerProvidersButton"), tr("Providers"),
        tr("Choose the processing provider"),
        &PlayerDialog::onProcessingProvidersRequested);
    processingOriginalButton_ = addChip(
        QStringLiteral("playerOriginalButton"), tr("Original"),
        tr("Stay on the Original picture"), &PlayerDialog::onProcessingOriginalRequested);

    // A reason can echo a provider's or a model's own error text, so it is
    // plain text by construction rather than per call site.
    statusLabel_->setTextFormat(Qt::PlainText);
    statusPanel_->hide();

    statusTimer_ = new QTimer(this);
    statusTimer_->setSingleShot(true);
    connect(statusTimer_, &QTimer::timeout, statusPanel_, &QWidget::hide);
}

void PlayerDialog::connectShell() {
    connect(shell_, &PlayerShell::playPauseRequested, surface_, &VideoSurface::togglePause);
    connect(shell_, &PlayerShell::seekRequested, surface_, &VideoSurface::seek);
    connect(shell_, &PlayerShell::rateChanged, this, [this](double rate) {
        speed_ = rate;
        surface_->setSpeed(speed_);
    });
    connect(shell_, &PlayerShell::animeJanaiPresetRequested, this,
            &PlayerDialog::applyAnimeJanaiRequest);
    connect(shell_, &PlayerShell::animeJanaiBackendRequested, this,
            &PlayerDialog::applyAnimeJanaiBackend);
    connect(shell_, &PlayerShell::anime4KModeChanged, this, [this](anime4k::Mode mode) {
        anime4kMode_ = mode;
        surface_->setAnime4KMode(mode);
        // The applied id must follow the mode, or PiP copies a stale chain.
        anime4kChainId_ = anime4k::modeId(mode);
        // The shader chain is a rendering choice, not per-episode state: it is
        // the one player setting that outlives a session, and a 4x upscale on
        // a GPU that cannot carry it is invisible until the next run.
        QSettings settings;
        settings.setValue(kAnime4KSettingKey, anime4kChainId_);
    });
    connect(shell_, &PlayerShell::volumeChanged, this, [this](double value) {
        // The shell speaks 0…1; the engine and mpv speak 0…100.
        volume_ = std::clamp(value, 0.0, 1.0);
        surface_->setVolume(volume_ * 100.0);
    });
    connect(shell_, &PlayerShell::muteChanged, this, [this](bool muted) {
        muted_ = muted;
        surface_->setMuted(muted_);
    });
    connect(shell_, &PlayerShell::prevRequested, this, [this] { stepEpisode(-1); });
    connect(shell_, &PlayerShell::nextRequested, this, [this] { stepEpisode(1); });
    connect(shell_, &PlayerShell::pipRequested, this, &PlayerDialog::togglePip);
    connect(shell_, &PlayerShell::openOriginalRequested, this, [this] {
        if (!directMedia_) {
            host_->requestOpenOriginal();
            return;
        }
        const QUrl source = QUrl::fromUserInput(url_);
        if (!QDesktopServices::openUrl(source)) {
            flashStatus(tr("Could not open the video source"));
        }
    });
    connect(shell_, &PlayerShell::downloadRequested, this, &PlayerDialog::copyStreamUrl);
    connect(shell_, &PlayerShell::mediaDropped, this, &PlayerDialog::onMediaDropped);
    connect(shell_, &PlayerShell::closeRequested, this, &PlayerDialog::close);
    connect(shell_, &PlayerShell::copyLinkRequested, this, &PlayerDialog::copyStreamUrl);
    connect(shell_, &PlayerShell::frameRequested, this, &PlayerDialog::copyCurrentFrame);
    connect(shell_, &PlayerShell::openFolderRequested, this, &PlayerDialog::openMediaFolder);
    connect(shell_, &PlayerShell::compareRequested, this, &PlayerDialog::openCompareWindow);
    connect(shell_, &PlayerShell::stateChanged, this, &PlayerDialog::onShellStateChanged);
    connect(shell_, &PlayerShell::audioRequested, this, &PlayerDialog::openAudioMenu);
    connect(shell_, &PlayerShell::subtitlesRequested, this, &PlayerDialog::openSubtitlesMenu);
    connect(shell_, &PlayerShell::qualityRequested, this, &PlayerDialog::onQualityPicked);
    connect(shell_, &PlayerShell::fullscreenChanged, this,
            &PlayerDialog::onShellFullscreenChanged);
    // The menu's native rows share one requested-change handler, so every
    // selection follows the same persistence and availability path.
    connect(shell_, &PlayerShell::nativeActionRequested, this,
            &PlayerDialog::onNativeActionRequested);
    shell_->installEventFilter(this);
}

void PlayerDialog::loadPresetShortcuts() {
    QSettings settings;
    settings.beginGroup(QLatin1String(kPresetShortcutSettingsGroup));
    // Claims are keyed on the sequence as it will fire, so a second preset bound
    // to the same keystroke is skipped rather than fighting the first one over
    // every press. The catalogue order decides which of the two wins.
    //
    // The one shared collision check answers here too, so a preset cannot be
    // refused for a reason the settings dialog would have allowed - and it is
    // what names the owner a skipped binding lost to.
    QMap<QString, ShortcutClaim> claims;
    const QVector<anime4k::ModeInfo> modes = anime4k::availableModes();
    for (const anime4k::ModeInfo& info : modes) {
        const QString stored = settings.value(info.id).toString();
        if (stored.isEmpty()) {
            continue;
        }
        const QKeySequence sequence =
            QKeySequence::fromString(stored, QKeySequence::PortableText);
        // Empty here means the stored text named no key at all, which is the
        // same thing as never having bound it.
        if (sequence.isEmpty()) {
            qWarning().noquote() << "mikai: stored preset shortcut" << info.id
                                 << "names no key and was not installed";
            continue;
        }
        const ShortcutConflict conflict = findShortcutConflict(sequence, claims);
        if (conflict.blocked) {
            // A stored binding that can never fire is a fact about this build's
            // key space, not a preference the viewer chose afresh: the key was
            // set and does nothing, so it is reported rather than dropped in
            // silence. The reason key and owner come from the shared check, so
            // this cannot name a different cause than the settings dialog would.
            qWarning().noquote() << "mikai: stored preset shortcut" << info.id << "for"
                                 << shortcutClaimKey(sequence) << "was not installed:"
                                 << conflict.reasonKey << "is held by" << conflict.ownerId
                                 << conflict.ownerLabel;
            continue;
        }
        ShortcutClaim claim;
        claim.ownerId = info.id;
        claim.ownerLabel = info.label;
        claim.kind = ShortcutOwnerKind::Preset;
        claims.insert(shortcutClaimKey(sequence), claim);

        // Parented to the dialog and WindowShortcut-scoped: active while the
        // player window is, and for the overlays that are its children, which
        // is exactly where the shell's own keys are live. ApplicationShortcut
        // would make a preset fire from the rest of mikai too.
        const QString presetId = info.id;
        auto* shortcut = new QShortcut(sequence, this);
        shortcut->setContext(Qt::WindowShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, presetId] {
            shell_->applyAnime4KPreset(presetId);
        });
    }
    settings.endGroup();
}


void PlayerDialog::copyStreamUrl() {
    if (!directMedia_) {
        host_->requestCopyStreamUrl();
        return;
    }
    if (!mikai::gui::copyText(url_)) {
        flashStatus(tr("Failed to copy link"), true);
        return;
    }
    flashStatus(tr("Video link copied"));
}

void PlayerDialog::openMediaFolder() {
    if (closed_) {
        return;
    }
    QString local = url_;
    // Native filenames can contain '#', which URL parsing treats as a fragment.
    if (!QDir::isAbsolutePath(local)) {
        const QUrl source = QUrl::fromUserInput(local);
        if (!source.isLocalFile()) {
            flashStatus(tr("This source is a stream — it has no folder"), true);
            return;
        }
        local = source.toLocalFile();
    }
    const QFileInfo media(local);
    if (!media.isFile()) {
        flashStatus(tr("The video file is no longer there"), true);
        return;
    }
    const QString folder = media.absolutePath();
    if (!QFileInfo(folder).isDir() || !QDesktopServices::openUrl(QUrl::fromLocalFile(folder))) {
        flashStatus(tr("Could not open the containing folder"), true);
        return;
    }
    flashStatus(tr("Containing folder opened"));
}

void PlayerDialog::connectEngine() {
    connect(surface_, &VideoSurface::errorOccurred, this, &PlayerDialog::onEngineError);
    connect(surface_, &VideoSurface::durationChanged, this, &PlayerDialog::onEngineDuration);
    connect(surface_, &VideoSurface::positionChanged, this, &PlayerDialog::onEnginePosition);
    connect(surface_, &VideoSurface::playingChanged, this, &PlayerDialog::onEnginePlaying);
    // The engine is the only authority on whether the video runs: after a PiP
    // hand-back the shell's cached flag and the core can disagree, and a play
    // control stuck on pause is unreadable.
    connect(surface_, &VideoSurface::playingChanged, shell_, [shell = shell_](bool) {
        shell->updatePlayIcon();
    });
    connect(surface_, &VideoSurface::videoSizeChanged, this, &PlayerDialog::onEngineVideoSize);
    connect(surface_, &VideoSurface::streamStatsChanged, shell_, &PlayerShell::setStreamStats);
    // The filter's own status arrives from the surface's reader, which is the
    // only thing that may report a chain as running. The held inputs and the HUD
    // follow it; nothing is synthesised from the request.
    connect(surface_, &VideoSurface::nativeStatusChanged, this,
            &PlayerDialog::onNativeStatusChanged);
    // The core's own transaction state and its named failures. A failure is
    // routed away from the media path on purpose: it never re-extracts a signed
    // URL and never offers the retry chip.
    connect(surface_, &VideoSurface::nativePlaybackStatusChanged, this,
            &PlayerDialog::onNativePlaybackStatusChanged);
    connect(surface_, &VideoSurface::nativeProcessingFailure, this,
            &PlayerDialog::onNativeProcessingFailure);
    // The reference capture is forwarded from whichever core owns playback; a
    // reply from a core that no longer does is dropped, not handed over.
    connect(surface_, &VideoSurface::nativeReferenceFrameReady, this,
            &PlayerDialog::onNativeReferenceFrameReady);
    connect(surface_, &VideoSurface::nativeReferenceFrameFailed, this,
            &PlayerDialog::onNativeReferenceFrameFailed);
}

QList<QualityEntry> PlayerDialog::usableQualities(const QList<QualityEntry>& qualities) {
    QList<QualityEntry> usable;
    for (const QualityEntry& entry : qualities) {
        if (!entry.url.trimmed().isEmpty()) {
            usable.append(entry);
        }
    }
    return usable;
}

// ═══════════════════════════════════════════════════════════════════════════
//  engine → shell
// ═══════════════════════════════════════════════════════════════════════════

void PlayerDialog::onEngineError(const QString& message) {
    if (closed_) {
        return;
    }
    syncPipBar();
    // No core exists yet because the app has not finished resolving the mpv
    // runtime. That reason is a wait, not a media failure: showing it as an error
    // named one turn before the module loads and re-extracting a URL that was
    // never tried is what made a launch report a runtime as permanently missing.
    // The pending line goes up instead and the request still goes out, so the
    // page joins the resolution and answers here when it settles. A dialog that
    // already HAS a core is reporting a real media failure, which is never
    // covered by this: it keeps its own reason and its own recovery path.
    if (!engineReady_ && mplayer::native::runtimeResolutionPending()) {
        showRuntimePendingStatus();
        emit runtimeRequested();
        return;
    }
    showErrorStatus(message.isEmpty() ? tr("Could not play the stream") : message);
    // One shot: the host re-extracts and reloads, and a second failure must not
    // start the loop over again. Direct media has no anime host, so only the
    // visible retry chip above applies.
    if (!directMedia_ && !refreshAttempted_) {
        refreshAttempted_ = true;
        host_->playbackFailed();
    }
}

void PlayerDialog::onEngineDuration(double seconds) {
    if (closed_) {
        return;
    }
    if (resumePending_ && seconds > 0.0) {
        restoreResumePoint();
    }
    // The shell's timeline is a scrub bar over a known length: a position fed
    // before mpv reports one only paints 0:00 / -0:00.
    if (seconds > 0.0) {
        shell_->setDuration(seconds);
        shell_->setPosition(surface_->position());
    }
    syncPipBar();
}

void PlayerDialog::onEnginePosition(double seconds) {
    if (closed_) {
        return;
    }
    // Position is held back until a stream swap has restored its resume point,
    // so the bar never counts down from zero mid-seek.
    if (resumePending_ || surface_->duration() <= 0.0) {
        return;
    }
    shell_->setPosition(seconds);
}

void PlayerDialog::onEnginePlaying(bool playing) {
    if (closed_) {
        return;
    }
    if (resumePending_ && surface_->duration() > 0.0) {
        restoreResumePoint();
    }
    shell_->setPlaying(playing);
    applyPendingPause();
    // The shell drops its own mute when a volume arrives, so the mute state is
    // pushed after it or the two would fight.
    shell_->setVolume(volume_);
    shell_->setMuted(muted_);
    shell_->setRate(std::clamp(speed_, kShellRateMin, kShellRateMax));
    // Dismissed only by a stream that really plays. This callback also fires
    // for the first pause observation, which a failed open still reports as
    // "not paused"; dismissing on that hid the retry chip the failure had just
    // raised, in the same pump tick that drained the end-file error. Real video
    // is the surface's own evidence that a stream is up, so the chip survives
    // until one exists.
    if (errorActive_ && surface_->hasVideo()) {
        errorActive_ = false;
        statusTimer_->stop();
        statusPanel_->hide();
    }
    syncPipBar();
}

void PlayerDialog::onEngineVideoSize(int width, int height) {
    if (closed_ || width <= 0 || height <= 0) {
        return;
    }
    aspect_ = static_cast<double>(width) / static_cast<double>(height);
    shell_->setSourceSize(width, height);
    shell_->setAspectRatio(aspect_);
}

bool PlayerDialog::openStream(const QString& url, int height, bool resume) {
    if (closed_ || url.isEmpty()) {
        return false;
    }
    ++animeJanaiSequence_;
    if (animeJanaiCancellation_) animeJanaiCancellation_->store(true);
    // A new source supersedes the failed one, so the chip it left is retired
    // here rather than lingering over the reload. Every recovery path runs
    // through openStream - a retry click, a refreshed link, an episode or
    // variant swap - so this is the one place that knows a stream is being
    // replaced. If THIS stream fails too, onEngineError raises a fresh chip.
    if (errorActive_) {
        errorActive_ = false;
        statusTimer_->stop();
        retryButton_->hide();
        statusPanel_->hide();
    }
    // A new source supersedes the remedies too: they described the previous
    // source's processing. The media chip and its loop are untouched above.
    retireProcessingRow();
    // A running engine operation keeps the replaced source paused as its owner:
    // open() drops the previous hold, so the hold is re-armed for the new one.
    const bool rearmPreparationAfterOpen = enginePreparationActive_;
    // The old source's pause as the VIEWER asked for it: under a hold the
    // physical paused state is the operation's own, not the viewer's.
    const bool carriedPausedIntent = surface_->playbackPauseIntent();
    // A swap carries the position, the pause and the rate across so playback
    // continues where it was instead of restarting.
    resumePending_ = resume && surface_->hasVideo();
    if (resumePending_) {
        resumePosition_ = surface_->position();
        resumePaused_ = carriedPausedIntent;
    } else {
        // A restart resets the position but not the viewer's intent: mpv
        // always brings a fresh file up playing, so a swap away from a paused
        // player would start the next episode loudly. Recorded here and applied
        // when the new stream reports itself - a core with no file loaded
        // cannot hold a pause for it.
        pauseAfterLoad_ = surface_->hasVideo() && carriedPausedIntent;
    }
    if (height > 0) {
        currentHeight_ = height;
        shell_->setQuality(QStringLiteral("%1p").arg(height));
    }
    // open() discards the hold, so the intent it kept is saved and handed back
    // to the re-established hold below.
    const auto rearmCarriedIntent = rearmPreparationAfterOpen
        ? std::optional<bool>(carriedPausedIntent)
        : std::nullopt;
    url_ = url;
    // open() reports false only when the engine itself could not be created, so
    // this is the one place that knows whether a retry is worth offering.
    // The real MpvError reason was already emitted synchronously through
    // onEngineError before this returns, so the failure chip names the cause.
    engineReady_ = surface_->open(url);
    if (rearmPreparationAfterOpen && engineReady_) {
        // Same operation, so its terminal still ends this hold; the hold is
        // re-established with the intent saved before the load.
        moveEnginePreparationPause(surface_, rearmCarriedIntent);
    }
    if (engineReady_) {
        // The runtime wait is over: what was an informational line becomes a
        // real open, so the pending status must not linger over playback.
        runtimePendingVisible_ = false;
        // The generation this load's pause is recorded under: a viewer request
        // that lands while the file loads supersedes it, and the deferred
        // restore then drops the recorded value rather than overriding them.
        resumePauseGeneration_ = surface_->pauseRequestGeneration();
    }
    if (!resumePending_) {
        // Volume, mute and rate live on the core across a load, but the shell
        // and the float still need to hear the new state.
        shell_->setVolume(volume_);
        shell_->setMuted(muted_);
        shell_->setRate(std::clamp(speed_, kShellRateMin, kShellRateMax));
    }
    // Observable selections stay as the caller set them: quality options and
    // the PiP bar follow the requested height even when no core opened.
    publishQualityOptions();
    syncPipBar();
    if (!engineReady_) {
        // No core: nothing observed, nothing to announce and no plan to paint
        // as Configured. The failure chip above is the only status.
        return engineReady_;
    }
    // A new source is a new filter session: the reader was rebound and its view
    // cleared, so the held inputs and the poll demand follow the new core rather
    // than a plan from the previous one.
    refreshNativeInputs();
    if (!animeJanaiPresetId_.isEmpty()) {
        applyAnimeJanaiRequest(animeJanaiPresetId_);
    }
    return engineReady_;
}

void PlayerDialog::restoreResumePoint() {
    resumePending_ = false;
    const double position = std::clamp(resumePosition_, 0.0, surface_->duration());
    surface_->seek(position);
    shell_->setPosition(position);
    if (resumePauseGeneration_ != surface_->pauseRequestGeneration()) {
        // A newer explicit request already answered this load, and the live
        // signal that carried it updated the shell: neither the recorded pause
        // nor the recorded playing glyph may overwrite it.
        return;
    }
    if (resumePaused_) {
        // Assert the desired pause, not a toggle: restoreResumePoint runs from
        // the duration/playing callbacks, and a toggle inverts whatever the
        // core currently holds instead of installing the recorded intent.
        surface_->setPaused(true);
        shell_->setPlaying(false);
    } else {
        shell_->setPlaying(true);
    }
}

void PlayerDialog::applyPendingPause() {
    if (!pauseAfterLoad_) {
        return;
    }
    pauseAfterLoad_ = false;
    if (resumePauseGeneration_ != surface_->pauseRequestGeneration()) {
        // The viewer already answered this load explicitly: a pause recorded
        // before the file was up must not overwrite the newer request.
        return;
    }
    surface_->setPaused(true);
    shell_->setPlaying(false);
}

void PlayerDialog::onShellStateChanged(const QString& state) {
    if (closed_) {
        return;
    }
    // The shell hides ALL its chrome after an idle moment, and these
    // dialog-owned panels ride along: a strip floating over a blank screen is a
    // stray panel.
    if (state == QStringLiteral("FULLSCREEN_AUTO_HIDDEN")) {
        audioPopup_->close();
        subtitlesPopup_->close();
        strip_->hide();
        seriesDrawer_->hide();
    } else {
        strip_->setVisible(!playlist_.isEmpty());
        seriesDrawer_->setVisible(seriesExpanded_ && !playlist_.isEmpty());
        syncStatusVisibility();
    }
    layoutPanels();
}

void PlayerDialog::onShellFullscreenChanged(bool fullscreen) {
    if (closed_ || pip_ != nullptr) {
        return;
    }
    fullscreen_ = fullscreen;
    transitioning_ = true;
    if (fullscreen) {
        if (!isFullScreen()) {
            windowedSize_ = geometry();
            showFullScreen();
        }
    } else {
        if (isFullScreen()) {
            showNormal();
        }
        if (windowedSize_.isValid()) {
            setGeometry(windowedSize_);
        } else {
            placeWindowed();
        }
    }
    transitioning_ = false;
    shell_->setGeometry(rect());
    layoutPanels();
}

void PlayerDialog::openCompareWindow() {
    if (closed_ || pip_ != nullptr) {
        return;
    }
    // Both guards say what is wrong rather than opening a window that cannot
    // show anything: without the shader folder the preset chains cannot render,
    // and without video there is no frame to compare.
    if (!surface_->hasVideo()) {
        flashStatus(tr("No video to compare"));
        return;
    }
    if (anime4k::findShadersDir().isEmpty()) {
        flashStatus(tr("Shaders folder not found — comparison is unavailable"));
        return;
    }
    if (anime4k::availableModes().size() < 2) {
        flashStatus(tr("Not enough variants to compare"));
        return;
    }
    // One window, kept and reused: it borrows the surface, and a second one
    // would fight the first for the same core while it swaps chains.
    const bool wasVisible = compare_ != nullptr && compare_->isVisible();
    if (compare_ == nullptr) {
        compare_ = new CompareWindow(surface_, anime4kChainId_, this, animeJanaiBackend_);
    } else if (!wasVisible) {
        compare_->setCurrentCandidateId(anime4kChainId_);
    }
    compare_->setAjiBackend(animeJanaiBackend_);
    compare_->show();
    if (wasVisible && !compare_->comparisonPending()) {
        compare_->captureNow();
    }
    compare_->raise();
    compare_->activateWindow();
}

void PlayerDialog::copyCurrentFrame() {
    if (closed_ || surface_ == nullptr || !surface_->hasVideo()) {
        flashStatus(tr("No frame to copy"), true);
        return;
    }
    const QImage frame = surface_->grabVideoFramebuffer();
    if (frame.isNull()) {
        flashStatus(tr("Could not read the current frame"), true);
        return;
    }
    if (!mikai::gui::copyImage(frame)) {
        flashStatus(tr("Failed to copy frame"), true);
        return;
    }
    flashStatus(tr("Frame copied (%1×%2)").arg(frame.width()).arg(frame.height()));
}

void PlayerDialog::onMediaDropped(const QString& path) {
    if (closed_ || path.isEmpty()) {
        return;
    }
    // Late episode/stream callbacks must not replace a directly opened video.
    directMedia_ = true;
    leavePip();
    qualities_.clear();
    currentHeight_ = 0;
    currentEmbed_.clear();
    setSeriesExpanded(false);
    setPlaylist({});
    setVariants({}, {});
    audioPopup_->close();
    subtitlesPopup_->close();
    shell_->dismissOverlay();
    publishQualityOptions();
    caption_ = QFileInfo(path).fileName();
    setWindowTitle(tr("Player — %1").arg(caption_));
    shell_->setEpisodeLabel(caption_);
    shell_->setMedia(path, caption_);
    if (!openStream(path, 0, /*resume=*/false)) {
        ensureOpenFailureVisible();
        return;
    }
            flashStatus(tr("Playing local file"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  episodes
// ═══════════════════════════════════════════════════════════════════════════

void PlayerDialog::setPlaylist(const QVector<QVariantMap>& playlist, int index) {
    if (directMedia_ && !playlist.isEmpty()) {
        return;
    }
    playlist_ = playlist;
    if (playlist_.isEmpty()) {
        seriesExpanded_ = false;
    }
    if (!playlist_.isEmpty() && shell_ != nullptr) {
        shell_->wake();
    }
    const bool autoHidden =
        shell_ != nullptr && shell_->stateName() == QStringLiteral("FULLSCREEN_AUTO_HIDDEN");
    strip_->setVisible(!playlist_.isEmpty() && !autoHidden);
    episodeIndex_ = (index >= 0 && index < playlist_.size()) ? index : 0;
    seriesButton_->setVisible(!playlist_.isEmpty());

    QStringList labels;
    labels.reserve(playlist_.size());
    for (const QVariantMap& item : playlist_) {
        labels.append(item.value(QStringLiteral("label")).toString());
    }
    seriesDrawer_->setEntries(labels, episodeIndex_);
    // The flank arrows are the only step control on the picture, so they
    // follow the playlist: on episode 1 of 11 the previous arrow leads nowhere
    // and should not be offered at all.
    shell_->setEpisodeStepping(episodeIndex_ > 0, episodeIndex_ + 1 < playlist_.size());
    if (episodeIndex_ >= 0 && episodeIndex_ < playlist_.size()) {
        const QString ep = playlist_.at(episodeIndex_).value(QStringLiteral("label")).toString();
        if (!ep.isEmpty()) {
            shell_->setEpisodeLabel(ep);
        }
    }
    // The drawer's rows were torn down and rebuilt: re-arm the wake watchers
    // over the fresh buttons or hovering the list stops counting as activity.
    watchPanels();
    seriesDrawer_->setVisible(seriesExpanded_);
    layoutPanels();
}

void PlayerDialog::stepEpisode(int delta) { goToEpisode(episodeIndex_ + delta); }

void PlayerDialog::goToEpisode(int position, bool closeList) {
    // Step or jump to an episode. Picking from the list closes it, because that
    // means "this one, now"; the flank chevrons leave it open, since stepping
    // while browsing a long list and having it snap shut under the pointer is a
    // fight.
    if (closed_ || directMedia_ || position < 0 || position >= playlist_.size()) {
        return;
    }
    if (closeList) {
        setSeriesExpanded(false);
    }
    host_->requestEpisode(position);
}

int PlayerDialog::pickTargetHeight(int requested) const {
    int best = 0;
    for (const QualityEntry& entry : qualities_) {
        best = std::max(best, entry.height);
    }
    if (requested <= 0) {
        return best;
    }
    for (const QualityEntry& entry : qualities_) {
        if (entry.height == requested) {
            return requested;
        }
    }
    return best;
}

void PlayerDialog::setQualities(const QList<QualityEntry>& qualities) {
    if (directMedia_) {
        return;
    }
    qualities_ = usableQualities(qualities);
    publishQualityOptions();
}

void PlayerDialog::applyEpisode(const QString& label,
                                const QString& embed,
                                int index,
                                const QList<QualityEntry>& qualities,
                                const QVector<QVariantMap>& variants) {
    if (closed_ || directMedia_ || qualities.isEmpty()) {
        return;
    }
    episodeIndex_ = index;
    QVector<VariantInfo> parsed;
    parsed.reserve(variants.size());
    for (const QVariantMap& map : variants) {
        parsed.append(variantFromMap(map));
    }
    variants_ = parsed;
    setVariantsVisibility();
    currentEmbed_ = embed;
    setQualities(qualities);
    const int target = pickTargetHeight(0);
    caption_ = QStringLiteral("%1 · %2p").arg(label).arg(target);
    setWindowTitle(tr("Player — %1").arg(caption_));
    shell_->setEpisodeLabel(label);
    setPlaylist(playlist_, index);
    for (const QualityEntry& entry : qualities_) {
        if (entry.height == target) {
            // A new episode starts at its beginning, always. openStream
            // carries the position and the pause across by design, which is
            // right for a quality or a dub swap inside one episode and wrong
            // here: stepping or picking the next one and landing at the same
            // offset into it is not what either gesture means.
            const bool ok = openStream(entry.url, target, /*resume=*/false);
            shell_->setMedia(entry.url, caption_, QStringLiteral("%1p").arg(target));
            if (!ok) {
                ensureOpenFailureVisible();
                return;
            }
            flashStatus(tr("Episode: %1").arg(label));
            return;
        }
    }
}

void PlayerDialog::applyVariant(const QList<QualityEntry>& qualities,
                                const QString& label,
                                const QString& embed) {
    if (closed_ || directMedia_ || qualities.isEmpty()) {
        return;
    }
    if (!embed.isEmpty()) {
        currentEmbed_ = embed;
    }
    setQualities(qualities);
    const int target = pickTargetHeight(0);
    for (const QualityEntry& entry : qualities_) {
        if (entry.height == target) {
            const bool ok = openStream(entry.url, target);
            shell_->setMedia(entry.url, caption_, QStringLiteral("%1p").arg(target));
            if (!ok) {
                ensureOpenFailureVisible();
                return;
            }
            flashStatus(tr("Track: %1").arg(label));
            return;
        }
    }
}

void PlayerDialog::applyRefreshedStreams(const QList<QualityEntry>& qualities, int height) {
    if (closed_ || directMedia_ || qualities.isEmpty()) {
        return;
    }
    setQualities(qualities);
    const int target = pickTargetHeight(height);
    for (const QualityEntry& entry : qualities_) {
        if (entry.height == target) {
            // A refreshed link is a fresh stream, not a continuation: the old
            // resume point belongs to the expired url.
            const bool ok = openStream(entry.url, target, /*resume=*/false);
            shell_->setMedia(entry.url, caption_, QStringLiteral("%1p").arg(target));
            if (!ok) {
                ensureOpenFailureVisible();
                return;
            }
            flashStatus(tr("Stream link has expired — refreshing"));
            return;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  tracks
// ═══════════════════════════════════════════════════════════════════════════

QString PlayerDialog::trackRowText(const VariantInfo& variant) {
    // `<release> · <provider>` plus the badge once a probe found it.
    QString base;
    if (!variant.display.isEmpty() && !variant.track.isEmpty()) {
        base = QStringLiteral("%1 · %2").arg(variant.display, variant.track);
    } else {
        base = variant.label;
    }
    return variant.quality > 0 ? QStringLiteral("%1 · %2p").arg(base).arg(variant.quality) : base;
}

void PlayerDialog::publishQualityOptions() {
    QVector<int> heights;
    heights.reserve(qualities_.size());
    for (const QualityEntry& entry : qualities_) {
        heights.append(entry.height);
    }
    std::sort(heights.begin(), heights.end(), std::greater<int>());

    QVector<SettingsFlyout::QualityOption> options;
    options.reserve(heights.size());
    for (int height : heights) {
        SettingsFlyout::QualityOption option;
        option.height = height;
        // mikai's own value, not a provider's wording for the same height
        option.label = QStringLiteral("%1p").arg(height);
        option.current = height == currentHeight_;
        options.append(option);
    }
    shell_->setQualityOptions(options);
}

void PlayerDialog::openAudioMenu() {
    if (closed_ || directMedia_) {
        return;
    }
    shell_->dismissOverlay();
    shell_->wake();
    audioPopup_->setEntries(trackEntries(QStringLiteral("voice")));
    if (audioPopup_->rowCount() == 0) {
        flashStatus(tr("No other audio tracks available"));
        return;
    }
    probeUnknownTracks();
    audioPopup_->popupNear(settingsAnchor());
}

void PlayerDialog::openSubtitlesMenu() {
    if (closed_ || directMedia_) {
        return;
    }
    shell_->dismissOverlay();
    shell_->wake();
    subtitlesPopup_->setEntries(trackEntries(QStringLiteral("sub")));
    if (subtitlesPopup_->rowCount() == 0) {
        flashStatus(tr("No subtitles available"));
        return;
    }
    probeUnknownTracks();
    subtitlesPopup_->popupNear(settingsAnchor());
}

QVector<MenuEntry> PlayerDialog::trackEntries(const QString& kind) const {
    QVector<MenuEntry> entries;
    QString heading;
    for (const VariantInfo& variant : variants_) {
        if (variant.embed.isEmpty()) {
            continue;
        }
        // Empty means every kind, which the PiP float wants; the dock's two
        // buttons pass "voice" or "sub" so a viewer picks by what they care about.
        if (!kind.isEmpty() && variant.kind != kind) {
            continue;
        }
        if (variant.kindLabel != heading) {
            heading = variant.kindLabel;
            if (!heading.isEmpty()) {
                MenuEntry section;
                section.heading = heading;
                entries.append(section);
            }
        }
        const QString text = trackRowText(variant);
        MenuEntry entry;
        entry.text = text;
        entry.tooltip = variant.label.isEmpty() ? text : variant.label;
        entry.current = variant.embed == currentEmbed_;
        QVariantMap payload;
        payload.insert(QStringLiteral("embed"), variant.embed);
        payload.insert(QStringLiteral("label"), variant.label.isEmpty() ? text : variant.label);
        entry.payload = payload;
        entries.append(entry);
    }
    return entries;
}

void PlayerDialog::probeUnknownTracks() {
    if (directMedia_) {
        return;
    }
    // Lazy on purpose: a track nobody looks at never costs a request.
    for (const VariantInfo& variant : variants_) {
        if (variant.embed.isEmpty() || variant.embed == currentEmbed_) {
            continue;
        }
        if (variant.quality <= 0) {
            host_->requestTrackProbe(variant.embed);
        }
    }
}

void PlayerDialog::setVariants(const QVector<QVariantMap>& variants, const QString& currentEmbed) {
    if (directMedia_ && !variants.isEmpty()) {
        return;
    }
    variants_.clear();
    variants_.reserve(variants.size());
    for (const QVariantMap& map : variants) {
        variants_.append(variantFromMap(map));
    }
    currentEmbed_ = currentEmbed;
    setVariantsVisibility();
    if (audioPopup_->isVisible()) {
        audioPopup_->setEntries(trackEntries(QStringLiteral("voice")));
    }
    if (subtitlesPopup_->isVisible()) {
        subtitlesPopup_->setEntries(trackEntries(QStringLiteral("sub")));
    }
}

void PlayerDialog::setVariantsVisibility() {
    // A button appears only when there is a choice: more than one dub, or at
    // least one subtitle track.
    int dub = 0;
    int subs = 0;
    for (const VariantInfo& variant : variants_) {
        if (variant.embed.isEmpty()) {
            continue;
        }
        if (variant.kind == QStringLiteral("sub")) {
            ++subs;
        } else {
            ++dub;
        }
    }
    shell_->setAudioAvailable(dub > 1);
    shell_->setSubtitlesAvailable(subs > 0);
}

PlayerDialog::VariantInfo PlayerDialog::variantFromMap(const QVariantMap& map) {
    VariantInfo info;
    info.embed = map.value(QStringLiteral("embed")).toString();
    info.label = map.value(QStringLiteral("label")).toString();
    info.display = map.value(QStringLiteral("display")).toString();
    info.track = map.value(QStringLiteral("track")).toString();
    info.kind = map.value(QStringLiteral("kind")).toString();
    info.kindLabel = map.value(QStringLiteral("kind_label")).toString();
    info.quality = map.value(QStringLiteral("quality")).toInt();
    return info;
}

void PlayerDialog::setTrackQuality(const QString& embed, int quality) {
    if (closed_ || embed.isEmpty() || quality <= 0) {
        return;
    }
    for (VariantInfo& variant : variants_) {
        if (variant.embed == embed) {
            variant.quality = quality;
        }
    }
    if (audioPopup_->isVisible()) {
        audioPopup_->setEntries(trackEntries(QStringLiteral("voice")));
    }
    if (subtitlesPopup_->isVisible()) {
        subtitlesPopup_->setEntries(trackEntries(QStringLiteral("sub")));
    }
}

void PlayerDialog::onTrackPicked(const QVariant& payload) {
    if (closed_ || directMedia_ || payload.typeId() != QMetaType::QVariantMap) {
        return;
    }
    const QVariantMap map = payload.toMap();
    const QString embed = map.value(QStringLiteral("embed")).toString();
    const QString label = map.value(QStringLiteral("label")).toString();
    if (!embed.isEmpty() && embed != currentEmbed_) {
        host_->requestVariant(embed, label.isEmpty() ? embed : label);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  the three panels
// ═══════════════════════════════════════════════════════════════════════════



void PlayerDialog::onQualityPicked(const QVariant& payload) {
    if (closed_ || payload.typeId() != QMetaType::Int) {
        return;
    }
    const int height = payload.toInt();
    for (const QualityEntry& entry : qualities_) {
        if (entry.height != height) {
            continue;
        }
        const bool ok = openStream(entry.url, height);
        shell_->setMedia(entry.url, caption_, QStringLiteral("%1p").arg(height));
        if (!ok) {
            ensureOpenFailureVisible();
            return;
        }
        flashStatus(tr("Switching to %1p…").arg(height));
        return;
    }
    // A press that matches nothing used to do nothing at all: the popover
    // closed and the stream kept running, which reads as a dead row.
    qWarning("PlayerDialog: no %dp stream is on offer (%lld qualities)",
             height, static_cast<long long>(qualities_.size()));
    flashStatus(tr("%1p is not available for this release").arg(height));
}


QPoint PlayerDialog::settingsAnchor() const {
    if (auto* button =
            shell_->findChild<QPushButton*>(QStringLiteral("playerSettingsButton"))) {
        return button->mapToGlobal(QPoint(button->width(), 0));
    }
    return shell_->mapToGlobal(QPoint(shell_->width() / 2, shell_->height() - 46));
}


void PlayerDialog::toggleSeries() { setSeriesExpanded(!seriesExpanded_); }

void PlayerDialog::setSeriesExpanded(bool expanded) {
    seriesExpanded_ = expanded && !directMedia_ && !playlist_.isEmpty();
    if (seriesExpanded_) {
        shell_->dismissOverlay();
        shell_->wake();
    }
    const bool autoHidden =
        shell_ != nullptr && shell_->stateName() == QStringLiteral("FULLSCREEN_AUTO_HIDDEN");
    strip_->setVisible(!playlist_.isEmpty() && !autoHidden);
    updateSeriesToggle();
    seriesDrawer_->setVisible(seriesExpanded_ && !autoHidden);
    layoutPanels();
}

void PlayerDialog::updateSeriesToggle() {
    const QString hint = seriesExpanded_ ? tr("Hide episode list (S)") : tr("Episode list (S)");
    seriesButton_->setToolTip(hint);
    seriesButton_->setAccessibleName(hint);
    seriesButton_->setIcon(glyphIcon(seriesExpanded_ ? &IconPainter::chevronLeft
                                                     : &IconPainter::chevronRight,
                                     kIconSize));
}

// ═══════════════════════════════════════════════════════════════════════════
//  status
// ═══════════════════════════════════════════════════════════════════════════

void PlayerDialog::syncStatusVisibility() {
    // Bring the status back after the auto-hide lifted. A flashing toast that
    // was still on screen when the chrome hid has to reappear with the chrome;
    // one that already retired stays retired. A visible retry chip and an
    // actionable processing row are the other things that must survive: they are
    // the only offers a failed stream and a failed plan make, and an auto-hidden
    // panel underneath them reads as a dead window with no way back.
    if (closed_) {
        return;
    }
    if (errorActive_ || retryButton_->isVisible() || processingActive_) {
        statusPanel_->show();
    } else if (statusTimer_->isActive()) {
        statusPanel_->show();
        statusTimer_->start(kStatusMs);
    }
}

void PlayerDialog::flashStatus(const QString& message, bool failure) {
    if (closed_) return;
    // Any other status supersedes the runtime wait's line: the wait is over for
    // this surface, whatever else it now has to say.
    runtimePendingVisible_ = false;
    statusLabel_->setText(message);
    if (!errorActive_ && !processingActive_) {
        retryButton_->hide();
        statusTimer_->start(kStatusMs);
    }
    statusPanel_->show();
    statusPanel_->raise();
    if (failure) {
        QAccessibleEvent event(statusLabel_, QAccessible::Alert);
        QAccessible::updateAccessibility(&event);
    }
}

void PlayerDialog::showErrorStatus(const QString& message) {
    if (closed_) {
        return;
    }
    errorActive_ = true;
    statusTimer_->stop();
    statusLabel_->setText(message);
    retryButton_->show();
    // A media failure raises its own chip. It never clears processingActive_, so a
    // remedy already on offer stays on offer beside it: the two are separate
    // failures and neither hides the other.
    if (!processingActive_) {
        processingPrepareButton_->hide();
        processingProvidersButton_->hide();
        processingOriginalButton_->hide();
    }
    statusPanel_->show();
    statusPanel_->raise();
    // A failed stream is an alert like any other failure toast: assistive
    // technology must hear the real reason, not just see it.
    QAccessibleEvent alert(statusLabel_, QAccessible::Alert);
    QAccessible::updateAccessibility(&alert);
}

void PlayerDialog::ensureOpenFailureVisible() {
    if (closed_) {
        return;
    }
    // No core exists yet because the app has not finished resolving the mpv
    // runtime: that is a wait, not a failure. The pending line is shown and
    // nothing else happens - no permanent error, no retry chip and no
    // re-extraction, because the source was never given a chance to play. The
    // request still goes out so the page can join the resolution that is
    // running, and the answer arrives here when that resolution settles.
    if (!engineReady_ && mplayer::native::runtimeResolutionPending()) {
        showRuntimePendingStatus();
        emit runtimeRequested();
        return;
    }
    // VideoSurface::open emits the real MpvError reason synchronously through
    // onEngineError before returning false, so errorActive_ already names the
    // cause. The generic text is only for an open that reported false without
    // emitting anything (empty url, no core, no signal). Never overwrites.
    if (!errorActive_) {
        showErrorStatus(tr("Could not play the stream"));
    }
    // One shot: the host re-extracts and reloads, and a second failure must not
    // start the loop over again. onEngineError already armed this for the
    // emitted path; this covers the silent-false path without duplicating it.
    if (!directMedia_ && !refreshAttempted_) {
        refreshAttempted_ = true;
        if (host_ != nullptr) {
            host_->playbackFailed();
        }
    }
    emit runtimeRequested();
}

void PlayerDialog::showError(const QString& message) { onEngineError(message); }


void PlayerDialog::retryPlayback() {
    if (closed_ || url_.isEmpty()) {
        return;
    }
    if (!engineReady_) {
        // There is no engine to retry with: the host has to provision the mpv
        // runtime first, and startPlayer() is what runs again afterwards. The
        // visible chip keeps the actual refusal reason; generic only when no
        // reason exists yet.
        ensureOpenFailureVisible();
        return;
    }
    // The retry starts the stream over, so no resume point survives it.
    if (!openStream(url_, currentHeight_, /*resume=*/false)) {
        ensureOpenFailureVisible();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  lifecycle
// ═══════════════════════════════════════════════════════════════════════════

void PlayerDialog::startPlayer() {
    if (closed_ || url_.isEmpty()) {
        return;
    }
    shell_->setMedia(url_, caption_,
                     currentHeight_ > 0 ? QStringLiteral("%1p").arg(currentHeight_) : QString());
    if (!openStream(url_, 0, /*resume=*/false)) {
        // No mpv core: the host has to provision the runtime before this can
        // work, and calling startPlayer() again is what retries it.
        ensureOpenFailureVisible();
    }
}

void PlayerDialog::layoutPanels() {
    if (closed_ || shell_ == nullptr) {
        return;
    }
    const QRect area = shell_->geometry();
    strip_->adjustSize();
    strip_->move(area.x() + kSpacingMedium,
                 area.y() + kStripMargin + kInfoChipHeight + kSpacingSmall);
    const int panelWidth = std::min(statusPanel_->sizeHint().width(),
                                    std::max(240, area.width() - 2 * kStripMargin));
    const int panelHeight = statusPanel_->layout() != nullptr &&
                                    statusPanel_->layout()->hasHeightForWidth()
                                 ? statusPanel_->layout()->heightForWidth(panelWidth)
                                 : statusPanel_->sizeHint().height();
    statusPanel_->setGeometry(area.x() + (area.width() - panelWidth) / 2, area.y() + kStripMargin,
                              panelWidth, panelHeight);
    if (seriesExpanded_) {
        const int top = strip_->geometry().bottom() + kSpacingXS + 1;
        const int height =
            std::min(seriesDrawer_->contentHeight(), area.height() - top - kSpacingMedium);
        seriesDrawer_->setGeometry(area.x() + kSpacingMedium, top, kSeriesWidth, std::max(height, 60));
    }
    const QVector<QWidget*> panels = {strip_, statusPanel_, seriesDrawer_};
    for (QWidget* panel : panels) {
        if (panel->isVisible()) {
            panel->raise();
        }
    }
}

void PlayerDialog::watchPanels() {
    // Watch the strip, the toast and the episode drawer for user input. Hovering
    // an episode row produces no move the shell would see - rows have no mouse
    // tracking, and hover is consumed above them - so the shell's idle timer
    // kept firing and the drawer vanished under the pointer. Every widget in the
    // panel subtrees now reports activity back to the shell's wake, and the
    // watcher list is rebuilt whenever the drawer's rows are rebuilt.
    panelWatchers_.clear();
    const QVector<QWidget*> roots = {strip_, statusPanel_, seriesDrawer_};
    for (QWidget* root : roots) {
        if (root == nullptr) {
            continue;
        }
        QVector<QWidget*> widgets;
        widgets.append(root);
        widgets.append(root->findChildren<QWidget*>());
        for (QWidget* widget : widgets) {
            widget->setMouseTracking(true);
            widget->installEventFilter(this);  // idempotent re-install
            panelWatchers_.append(widget);
        }
    }
}

void PlayerDialog::placeWindowed() {
    if (closed_) {
        return;
    }
    QScreen* current = screen();
    if (current == nullptr) {
        current = QGuiApplication::primaryScreen();
    }
    if (current == nullptr) {
        resize(kPlayerDefaultWidth, kPlayerDefaultHeight);
        return;
    }
    const QRect area = current->availableGeometry();
    QSize size(kPlayerDefaultWidth, kPlayerDefaultHeight);
    size = size.boundedTo(area.size());
    setGeometry(area.x() + (area.width() - size.width()) / 2,
                area.y() + (area.height() - size.height()) / 2,
                size.width(), size.height());
    windowedSize_ = geometry();
}

void PlayerDialog::showEvent(QShowEvent* event) {
    const bool wasTransitioning = transitioning_;
    transitioning_ = true;
    // The state is requested before the base handler shows the window, so the
    // platform window is created at monitor size. Calling showFullScreen()
    // afterwards meant it was created at the windowed rect and then animated
    // up, which read as a stutter on every episode open. It has to happen
    // here and not in the constructor: the shell and its OpenGL surface do
    // not exist yet at that point, and asking for full screen underneath them
    // faulted in Qt6Widgets three seconds into a session.
    if (fullscreen_ && !isFullScreen()) {
        setWindowState(windowState() | Qt::WindowFullScreen);
    }
    QDialog::showEvent(event);
    shell_->setGeometry(rect());
    shell_->show();
    shell_->setFocus(Qt::OtherFocusReason);
    if (fullscreen_) {
        if (!isFullScreen()) {
            showFullScreen();
        }
        shell_->enterFullscreen();
    } else {
        if (isFullScreen()) {
            showNormal();
        }
        if (windowedSize_.isValid()) {
            setGeometry(windowedSize_);
        } else {
            placeWindowed();
        }
        shell_->leaveFullscreen();
    }
    transitioning_ = wasTransitioning;
    shell_->setGeometry(rect());
    layoutPanels();
}

void PlayerDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event->type() != QEvent::LanguageChange) {
        return;
    }
    updateSeriesToggle();
    audioPopup_->setTitle(tr("AUDIO"));
    subtitlesPopup_->setTitle(tr("SUBTITLES"));
    retryButton_->setText(tr("Retry"));
    retryButton_->setToolTip(tr("Retry playback"));
    retryButton_->setAccessibleName(tr("Retry playback"));
    processingPrepareButton_->setText(tr("Prepare"));
    processingPrepareButton_->setToolTip(tr("Prepare the selected recipe"));
    const bool animeJanaiFailure =
        lastProcessingReasonKey_.startsWith(QLatin1String("animejanai."));
    processingProvidersButton_->setText(animeJanaiFailure ? tr("Settings") : tr("Providers"));
    processingProvidersButton_->setToolTip(
        animeJanaiFailure ? tr("Open processing and component settings")
                          : tr("Choose the processing provider"));
    processingOriginalButton_->setText(
        animeJanaiFailure ? tr("Turn off AnimeJaNai") : tr("Original"));
    processingOriginalButton_->setToolTip(tr("Stay on the Original picture"));
    layoutPanels();
}

void PlayerDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    rememberWindowedGeometry();
    shell_->setGeometry(rect());
    layoutPanels();
}

void PlayerDialog::moveEvent(QMoveEvent* event) {
    QDialog::moveEvent(event);
    rememberWindowedGeometry();
}

void PlayerDialog::rememberWindowedGeometry() {
    // Qt emits move/resize during mode changes; those rectangles are not user placement.
    if (closed_ || transitioning_ || fullscreen_ || isFullScreen() || !isVisible()) {
        return;
    }
    windowedSize_ = geometry();
}

bool PlayerDialog::onShellKey(QKeyEvent* event) {
    // Runs ahead of the shell's own keyPressEvent so the panels get first
    // refusal on Esc and own their keys; everything else falls through
    // untouched, so Space, F, M and the arrows still reach the shell.
    if (event->key() == Qt::Key_Escape) {
        if (audioPopup_->isVisible()) {
            audioPopup_->close();
        } else if (subtitlesPopup_->isVisible()) {
            subtitlesPopup_->close();
        } else if (!shell_->dismissOverlay()) {
            if (seriesExpanded_) {
                setSeriesExpanded(false);
            } else {
                close();
            }
        }
        event->accept();
        return true;
    }
    if (event->key() == Qt::Key_S && !playlist_.isEmpty()) {
        toggleSeries();
    } else if (event->key() == Qt::Key_T) {
        openAudioMenu();
    } else if (event->key() == Qt::Key_C) {
        openSubtitlesMenu();
    } else if (event->key() == Qt::Key_Q) {
        shell_->openSettings();
    } else {
        return false;
    }
    event->accept();
    return true;
}

void PlayerDialog::keyPressEvent(QKeyEvent* event) {
    // Every key goes to the shell, the only widget that acts on them; a dialog
    // that swallowed one would take play/pause away with it.
    if (closed_) {
        QDialog::keyPressEvent(event);
        return;
    }
    if (onShellKey(event)) {
        return;
    }
    if (forwardingKey_) {
        QDialog::keyPressEvent(event);
        return;
    }
    forwardingKey_ = true;
    QCoreApplication::sendEvent(shell_, event);
    forwardingKey_ = false;
}

bool PlayerDialog::eventFilter(QObject* watched, QEvent* event) {
    if (closed_) {
        return false;
    }
    if (watched == this && event->type() == QEvent::WindowActivate) {
        // The shell's registry is the same set of popovers this walk used to
        // rediscover - it holds this dialog's two pickers, registered at
        // construction, plus the shell's own - without the per-node dynamic
        // property lookup over the whole subtree.
        if (shell_ != nullptr && !shell_->hasVisibleOverlay()) {
            shell_->setFocus(Qt::OtherFocusReason);
        }
        return false;
    }
    if (watched == shell_ && event->type() == QEvent::KeyPress) {
        return onShellKey(static_cast<QKeyEvent*>(event));
    }
    // Panel activity counts as user presence: re-arm the shell's idle timer
    // without consuming the event.
    //
    // The type is tested first on purpose. panelWatchers_ holds every widget
    // in three panel subtrees - one entry per episode row on a long playlist -
    // and the scan is linear, so putting it ahead of the type switch made
    // every Paint, LayoutRequest, ChildAdded and Resize that any panel widget
    // received pay for a membership test nothing below reads.
    switch (event->type()) {
        case QEvent::Enter:
        case QEvent::HoverEnter:
        case QEvent::HoverMove:
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::Wheel:
            if (!panelWatchers_.isEmpty() && panelWatchers_.contains(watched)) {
                shell_->wake();
            }
            break;
        default:
            break;
    }
    return QDialog::eventFilter(watched, event);
}

void PlayerDialog::closeEvent(QCloseEvent* event) {
    if (shuttingDown_) {
        // A reentrant close arrives through the settle below, which spins the
        // event loop, so it must not tear the same cores down twice. A close
        // that arrives after the stop really finished (the owner disposing the
        // session from playerClosed()) is not reentrant and may be accepted.
        if (shutdownComplete_) {
            QDialog::closeEvent(event);
            return;
        }
        event->ignore();
        return;
    }
    shuttingDown_ = true;
    closed_ = true;
    // Remembered before the flag is cleared: a close that is refused below has to
    // restore the status this dialog was holding, not leave it blank.
    const bool runtimePendingWasVisible = runtimePendingVisible_;
    runtimePendingVisible_ = false;
    enginePreparationActive_ = false;
    if (enginePreparationSurface_ != nullptr) {
        enginePreparationSurface_->setEnginePreparationPaused(false, false);
        enginePreparationSurface_.clear();
    }
    ++animeJanaiSequence_;
    if (animeJanaiCancellation_) animeJanaiCancellation_->store(true);
    const bool statusTimerWasActive = statusTimer_->isActive();
    statusTimer_->stop();
    audioPopup_->close();
    subtitlesPopup_->close();
    // A frozen capture holds the plan lock, and a locked request would only queue
    // the stops below and time them out, so Compare is closed first; its own
    // close path releases the lock.
    if (compare_ != nullptr) {
        compare_->close();
    }
    // Both cores are paused before anything is asked of their filters: a stop is
    // not the place to keep decoding into.
    if (surface_ != nullptr) {
        surface_->setPaused(true);
    }
    if (PipFloat* parked = pip_ != nullptr ? pip_ : pipPool_) {
        if (parked->videoSurface() != nullptr) {
            parked->videoSurface()->setPaused(true);
        }
    }
    // Hides the float, retires the hand-off and cancels a capture in flight; the
    // pooled widget itself outlives this handler and is released by the destructor.
    leavePip(/*closing=*/true);
    // M30 order: both cores are stopped with a real acknowledgement while their
    // GL contexts are still alive, and only then are the streams closed. The
    // settle spins the event loop, so the dialog is guarded weakly across it.
    QPointer<PlayerDialog> self(this);
    const bool parkedSettled = settleParkedNative();
    if (self.isNull()) return;
    const bool coreSettled = surface_ == nullptr || surface_->settleNativeForTeardown();
    if (self.isNull()) return;
    if (!parkedSettled || !coreSettled) {
        // Nothing is torn down: both cores and their render contexts stay alive so
        // the stop can be retried, and no stream or core is closed on a stop that
        // was never acknowledged.
        qWarning().noquote()
            << "mikai: native stop unsettled (native.playback.teardownUnsettled); close refused";
        flashStatus(tr("Native processing did not acknowledge its stop; the player stays open so "
                       "it can be retried."));
        closed_ = false;
        shuttingDown_ = false;
        // The close was refused, so the dialog keeps the status it had: a
        // runtime wait that refused to close is still waiting.
        if (runtimePendingWasVisible && !engineReady_ &&
            mplayer::native::runtimeResolutionPending()) {
            showRuntimePendingStatus();
        } else if (statusTimerWasActive) {
            statusTimer_->start();
        }
        event->ignore();
        return;
    }
    // The overlay is a Qt::Tool child window: a parent hide does not hide a
    // Tool, so park it on the accepted path only. The refused path above keeps
    // the dialog open and leaves the overlay exactly as the builder left it;
    // no resume happens here, only the builder's own acknowledgement resumes.
    if (processingOverlay_ != nullptr) {
        processingOverlay_->hide();
    }
    shell_->close();
    // VideoSurface owns the libmpv handle: a dialog closed by the host's
    // shutdown must not leave the core running behind it.
    surface_->close();
    // The stop is complete, so the event may be accepted - and that has to be
    // recorded before the host is told, because the host reacts by disposing the
    // session, which closes this dialog again and must find it finished rather
    // than mid-settle.
    shutdownComplete_ = true;
    QDialog::closeEvent(event);
    if (host_ != nullptr) {
        host_->playerClosed();
    }
}


}  // namespace shell
}  // namespace mplayer
